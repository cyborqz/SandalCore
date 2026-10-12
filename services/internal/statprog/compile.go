// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package statprog

import (
	"encoding/binary"
	"fmt"
	"math"
	"sort"
	"strconv"
	"strings"

	"aocrop/services/internal/aocexpr"
)

// The compiler: AoCExpression text -> programs (format.go).
//
// A formula is parsed by aocexpr, folded into a small tree (constants computed,
// variables resolved to local slots or constants, EvalE / EvalFormula / GetTerm
// replaced by the equation's own code, stat and curve references resolved to
// indexes) and emitted as instructions. What it does not accept is an error
// that names the formula: nothing is skipped.
//
// Typing is static. A value is a number, a boolean, or a compile-time thing
// (an object such as GetOwner(), a reference, a string, an enum value, an actor
// id) that never reaches the stack: the functions that take them are
// operations that name them (a stat index, a reference, a string). Division
// follows aocexpr: it needs an operand proven to be a float (a literal with a
// decimal point, Float(), SelectFloat(), EvalCurve(), GetStat() and the other
// stat functions of aocexpr.StatFloatFuncs, a variable that has only been given
// such a value, + - * / with such an operand), else it is an error.
//
// Two differences from running the text in aocexpr, both invisible to values:
// the statements of an inlined equation run where its call is (not hoisted), and
// a division by zero is a fault at run time (the interpreter pushes 0 and
// counts it) where aocexpr stops with an error.

// Options configure a compile.
type Options struct {
	// Mode is the arithmetic (Bundle.Mode); the default is ModeFloat32.
	Mode Mode
}

type vtype uint8

const (
	tNum vtype = iota
	tBool
	tObj
	tRef
	tStr
	tEnum
	tActor
	tFlags // the flags of a hit on a target (GetHitFlags): only HitFlagsContains reads them
	// tPlayer is GetPlayer() or GetPlayerGuid() (Nodes 1): the character the expression is for; only the city node reads take it
	tPlayer
)

func (t vtype) String() string {
	return [...]string{"a number", "a boolean", "an object", "a record reference", "a string", "an enum value", "an actor id", "the flags of a hit", "the player"}[t]
}

// == C | Y | B | C O A ==

// The tree: what the emitter reads.
type (
	node   interface{}
	nConst struct{ f float64 }
	nLoad  struct{ slot int }
	nStat  struct{ idx int }
	nTotal struct{}
	nLevel struct{}
	nUn    struct {
		op byte
		x  node
	}
	nBin struct {
		op   byte
		l, r node
	}
	nAnd struct{ l, r node }
	nOr  struct{ l, r node }
	nSel struct{ c, a, b node }
	nFn1 struct {
		op byte
		x  node
	}
	nFn2 struct {
		op   byte
		a, b node
	}
	nFn3 struct {
		op      byte
		a, b, c node
	}
	nCurve struct {
		idx int
		x   node
	}
	nCtx struct {
		op  byte
		imm [2]uint16
	}
	nHost struct { // plan D2, stage 2: a named host function with its 0, 1 or 2 numeric arguments
		op      byte
		fn, str uint16
		args    []node
	}
	nBlock struct {
		stmts  []stmt
		result node
	}
	nActor struct { // a read of an actor's own state (aocexpr.Role...): the leaf run in that actor's context
		role int
		x    node
	}
)

type (
	stmt   interface{}
	sStore struct {
		slot int
		x    node
	}
	sIf struct {
		cond      node
		then, els []stmt
	}
	sDrop struct{ x node }
)

// val is a folded expression: its type, its tree (a number or a boolean), and
// for the compile-time kinds their value.
type val struct {
	t     vtype
	n     node
	float bool // provably a float (the division rule)
	ref   aocexpr.Ref
	str   string
	enum  aocexpr.Enum
	role  int // an object's role (aocexpr.Role...): which actor of a hit it stands for
	// etype says a number is an enum's ordinal (a host function's answer: GetNodeBiome gives an "EBiomeType"), so that the enum literal it is compared
	// with becomes the same ordinal; etypeNode says it is a city node's rank, and the reference it is compared with becomes the rank.
	etype string
}

const etypeNode = "CityNode"

func (v val) constant() (float64, bool) {
	c, ok := v.n.(nConst)
	return c.f, ok
}

type binding struct {
	t      vtype
	konst  bool // bound to a compile-time value (v), no slot
	v      val
	slot   int
	float  bool
	isSlot bool
	etype  string
}

type scope struct {
	vars    map[string]*binding
	barrier bool // a called equation does not see its caller's variables
}

type unitKind uint8

const (
	unitInit  unitKind = iota // a StatInitializerList entry: GetStatInitRunningTotal is available
	unitBase                  // a stat's Base
	unitLimit                 // its Min or Max
	unitRegen                 // its RegenRate
	unitExpr                  // an ability expression (plan D2): a cost, a cooldown, a requirement, the value of an effect's stat mod
)

type compiler struct {
	g      *Graph
	single bool
	b      *Bundle

	statIdx  map[int64]int
	queue    []int // stats whose own formulas are still to compile (from qpos on)
	qpos     int
	constIdx map[uint64]int
	strIdx   map[string]int
	refIdx   map[Ref]int
	curveIdx map[int64]int
	progIdx  map[string]int
	parsed   map[string]*aocexpr.Program
}

// Compile compiles the player stat graph: every stat of the common lists and of
// each class's list, with the stats their formulas read, and the order to evaluate
// them in.
func Compile(g *Graph, opt Options) (*Bundle, error) {
	b, _, err := CompileExprs(g, opt, nil)
	return b, err
}

// Expr is an ability expression to compile (plan D2): the text of a cost, a cooldown, a charge count, a cast time, a requirement,
// a duration or the value of an effect's stat mod.
type Expr struct {
	What  string           // names the expression in an error
	Text  string           // AoCExpression text
	Terms map[int64]string // the input terms of a StatMod (ValueInputTerms): what GetTerm reads at the top of the text
}

// ExprResult is what compiling one Expr gave.
type ExprResult struct {
	Program int    // the index in Bundle.Progs (and in Bundle.Exprs), -1 when it did not compile
	Err     string // why it did not
	// UsesTarget says the expression calls GetTarget(): compiled as the caster (until there are other actors, the target of
	// a self or party hit is the caster), so a host that has other actors must not run it for a hit on one of them.
	UsesTarget bool
}

// CoA + C-Y-B

// CompileExprs compiles the stat graph as Compile does, then the ability expressions (functions only an expression may call:
// GetTarget, GetAbilityProgressRatio, GetEffectCountByRecordAndInstigator, RandDouble), into the same bundle: its stats are the stats
// the expressions read, and its Exprs the programs they compile to. An expression that does not compile (a function the engine
// does not have) is an ExprResult with the error and no program, and changes nothing in the bundle: the others are not
// affected. The error returned is the graph's own. The result is deterministic.
func CompileExprs(g *Graph, opt Options, exprs []Expr) (*Bundle, []ExprResult, error) {
	c, err := compileGraph(g, opt)
	if err != nil {
		return nil, nil, err
	}
	results := make([]ExprResult, len(exprs))
	var progs []int
	for i, ex := range exprs {
		results[i] = c.expr(ex)
		if results[i].Program >= 0 {
			progs = append(progs, results[i].Program)
		}
	}
	sort.Ints(progs)
	for i, p := range progs {
		if i == 0 || p != progs[i-1] {
			c.b.Exprs = append(c.b.Exprs, uint32(p))
		}
	}
	if err := c.order(); err != nil {
		return nil, nil, err
	}
	return c.b, results, nil
}

// compileGraph compiles the stats of the lists and what their formulas read, up to the order.
func compileGraph(g *Graph, opt Options) (*compiler, error) {
	c := &compiler{
		g: g, single: opt.Mode == ModeSingle, b: &Bundle{Mode: opt.Mode},
		statIdx: map[int64]int{}, constIdx: map[uint64]int{}, strIdx: map[string]int{},
		refIdx: map[Ref]int{}, curveIdx: map[int64]int{}, progIdx: map[string]int{},
		parsed: map[string]*aocexpr.Program{},
	}
	b := c.b
	classes := append([]ClassDef(nil), g.Classes...)
	sort.Slice(classes, func(i, j int) bool { return classes[i].Enum < classes[j].Enum })
	if len(classes) > MaxClasses {
		return nil, fmt.Errorf("statprog: %d classes (the most is %d)", len(classes), MaxClasses)
	}

	// The stats of the lists first, in list order, so their indexes are stable.
	member := func(list string) ([]uint32, error) {
		items, ok := g.Lists[list]
		if !ok {
			return nil, fmt.Errorf("statprog: no initializer list %q", list)
		}
		var out []uint32
		seen := map[uint32]bool{}
		for _, it := range items {
			idx, err := c.stat(it.Stat)
			if err != nil {
				return nil, fmt.Errorf("list %s: %w", list, err)
			}
			if !seen[uint32(idx)] {
				seen[uint32(idx)] = true
				out = append(out, uint32(idx))
			}
		}
		return out, nil
	}
	for _, l := range CommonLists {
		m, err := member(l)
		if err != nil {
			return nil, err
		}
		b.CommonMembers = appendUnique(b.CommonMembers, m)
	}
	for _, cl := range classes {
		var m []uint32
		for _, l := range cl.listNames() {
			ml, err := member(l)
			if err != nil {
				return nil, err
			}
			m = appendUnique(m, ml)
		}
		b.Classes = append(b.Classes, Class{Enum: uint8(cl.Enum), Name: c.str(cl.Name), Members: m})
	}

	// The initializer entries.
	steps := func(list string) ([]Step, error) {
		var out []Step
		for i, it := range g.Lists[list] {
			if strings.TrimSpace(it.Expr) == "" {
				continue
			}
			idx, _ := c.stat(it.Stat)
			p, _, err := c.program(it.Expr, unitInit, fmt.Sprintf("list %s entry %d (%s)", list, i, g.Stats[it.Stat].Name), nil)
			if err != nil {
				return nil, err
			}
			out = append(out, Step{Stat: uint32(idx), Prog: uint32(p)})
		}
		return out, nil
	}
	for _, l := range CommonLists {
		s, err := steps(l)
		if err != nil {
			return nil, err
		}
		b.CommonSteps = append(b.CommonSteps, s...)
	}
	for i, cl := range classes {
		for _, l := range cl.listNames() {
			s, err := steps(l)
			if err != nil {
				return nil, err
			}
			b.Classes[i].Steps = append(b.Classes[i].Steps, s...)
		}
	}

	// The stats' own formulas; compiling them can name more stats, queued behind.
	if err := c.drain(); err != nil {
		return nil, err
	}
	return c, nil
}

// drain compiles the formulas of the stats queued so far (compiling them can queue more).
func (c *compiler) drain() error {
	b, g := c.b, c.g
	for ; c.qpos < len(c.queue); c.qpos++ {
		idx := c.queue[c.qpos]
		sd := g.Stats[b.Stats[idx].Guid]
		for _, f := range []struct {
			text string
			kind unitKind
			what string
		}{
			{sd.Base, unitBase, "Base"},
			{sd.Min, unitLimit, "ValueMin"},
			{sd.Max, unitLimit, "ValueMax"},
			{sd.Regen, unitRegen, "RegenRate"},
		} {
			if strings.TrimSpace(f.text) == "" {
				continue
			}
			p, _, err := c.program(f.text, f.kind, fmt.Sprintf("%s of %s", f.what, sd.Name), nil)
			if err != nil {
				return err
			}
			// c.program may have grown b.Stats, so the stat is looked up again.
			b.Stats[idx].setProg(f.what, int32(p))
		}
	}
	return nil
}

// (Cyb + C.O.A)

// snapshot is the compiler's tables as they were: what an expression that did not compile must leave behind.
type snapshot struct{ consts, strs, refs, curves, progs, stats, queue int }

func (c *compiler) snap() snapshot {
	return snapshot{len(c.b.Consts), len(c.b.Strings), len(c.b.Refs), len(c.b.Curves), len(c.b.Progs), len(c.b.Stats), len(c.queue)}
}

// restore puts the tables back (the compiler only appends, so it cuts what was added and forgets its indexes).
func (c *compiler) restore(s snapshot) {
	b := c.b
	b.Consts, b.Strings, b.Refs, b.Curves, b.Progs, b.Stats = b.Consts[:s.consts], b.Strings[:s.strs], b.Refs[:s.refs], b.Curves[:s.curves], b.Progs[:s.progs], b.Stats[:s.stats]
	c.queue = c.queue[:s.queue]
	if c.qpos > s.queue {
		c.qpos = s.queue
	}
	for k, i := range c.constIdx {
		if i >= s.consts {
			delete(c.constIdx, k)
		}
	}
	for k, i := range c.strIdx {
		if i >= s.strs {
			delete(c.strIdx, k)
		}
	}
	for k, i := range c.refIdx {
		if i >= s.refs {
			delete(c.refIdx, k)
		}
	}
	for k, i := range c.curveIdx {
		if i >= s.curves {
			delete(c.curveIdx, k)
		}
	}
	for k, i := range c.progIdx {
		if i >= s.progs {
			delete(c.progIdx, k)
		}
	}
	for k, i := range c.statIdx {
		if i >= s.stats {
			delete(c.statIdx, k)
		}
	}
}

// expr compiles one ability expression, and the formulas of the stats it names; when either fails nothing of it stays.
func (c *compiler) expr(ex Expr) ExprResult {
	if strings.TrimSpace(ex.Text) == "" {
		return ExprResult{Program: -1, Err: ex.What + ": the expression is empty"}
	}
	s := c.snap()
	p, usesTarget, err := c.program(ex.Text, unitExpr, ex.What, ex.Terms)
	if err == nil {
		err = c.drain()
	}
	if err != nil {
		c.restore(s)
		return ExprResult{Program: -1, Err: err.Error()}
	}
	return ExprResult{Program: p, UsesTarget: usesTarget}
}

func (s *Stat) setProg(what string, p int32) {
	switch what {
	case "Base":
		s.Base = p
	case "ValueMin":
		s.Min = p
	case "ValueMax":
		s.Max = p
	case "RegenRate":
		s.Regen = p
	}
}

func appendUnique(dst, src []uint32) []uint32 {
	have := map[uint32]bool{}
	for _, x := range dst {
		have[x] = true
	}
	for _, x := range src {
		if !have[x] {
			have[x] = true
			dst = append(dst, x)
		}
	}
	return dst
}

// stat returns the index of a stat, adding it (and queueing its formulas).
func (c *compiler) stat(guid int64) (int, error) {
	if i, ok := c.statIdx[guid]; ok {
		return i, nil
	}
	sd, ok := c.g.Stats[guid]
	if !ok {
		return 0, fmt.Errorf("no stat %d", guid)
	}
	if len(c.b.Stats) >= MaxStats {
		return 0, fmt.Errorf("more than %d stats", MaxStats)
	}
	if sd.Replication < 0 || sd.Replication > 7 {
		return 0, fmt.Errorf("stat %s: replication %d", sd.Name, sd.Replication)
	}
	var fl uint8
	if sd.MultiplyMods {
		fl |= StatMulMods
	}
	if sd.ClampFinal {
		fl |= StatClampFinal
	}
	switch sd.OnSpawn {
	case "Reset_SetMax":
		fl |= StatSpawnMax
	case "Reset_SetMin":
		fl |= StatSpawnMin
	}
	if sd.ReplicatesBuckets {
		fl |= StatBuckets
	}
	if sd.MultiplyCompliment {
		fl |= StatMulCompl
	}
	if sd.StatType == "Type_FloatArray" {
		fl |= StatFloatArray
	}
	idx := len(c.b.Stats)
	c.b.Stats = append(c.b.Stats, Stat{
		Guid: guid, Name: c.str(sd.Name), Replication: uint8(sd.Replication), Flags: fl,
		Base: -1, Min: -1, Max: -1, Regen: -1,
	})
	c.statIdx[guid] = idx
	c.queue = append(c.queue, idx)
	return idx, nil
}

// [Cyb + C | O | A]

func (c *compiler) str(s string) uint32 {
	if i, ok := c.strIdx[s]; ok {
		return uint32(i)
	}
	i := len(c.b.Strings)
	c.b.Strings = append(c.b.Strings, s)
	c.strIdx[s] = i
	return uint32(i)
}

func (c *compiler) strOperand(s string) (uint16, error) {
	if len(s) > MaxStringLen {
		return 0, fmt.Errorf("a string of %d bytes (the most is %d)", len(s), MaxStringLen)
	}
	i := c.str(s)
	if i >= MaxStrings {
		return 0, fmt.Errorf("more than %d strings", MaxStrings)
	}
	return uint16(i), nil
}

func (c *compiler) ref(r aocexpr.Ref) (uint16, error) {
	k := Ref{r.TypeID, r.Guid}
	if i, ok := c.refIdx[k]; ok {
		return uint16(i), nil
	}
	if len(c.b.Refs) >= MaxRefs {
		return 0, fmt.Errorf("more than %d references", MaxRefs)
	}
	i := len(c.b.Refs)
	c.b.Refs = append(c.b.Refs, k)
	c.refIdx[k] = i
	return uint16(i), nil
}

func (c *compiler) constant(f float64) (uint16, error) {
	bits := math.Float64bits(f)
	if i, ok := c.constIdx[bits]; ok {
		return uint16(i), nil
	}
	if len(c.b.Consts) >= MaxConsts {
		return 0, fmt.Errorf("more than %d constants", MaxConsts)
	}
	i := len(c.b.Consts)
	c.b.Consts = append(c.b.Consts, f)
	c.constIdx[bits] = i
	return uint16(i), nil
}

// -- C-Y-B & C.O.A --

func (c *compiler) curve(guid int64) (int, error) {
	if i, ok := c.curveIdx[guid]; ok {
		return i, nil
	}
	cv, ok := c.g.Curves[guid]
	if !ok {
		return 0, fmt.Errorf("no curve %d", guid)
	}
	if len(cv.Keys) == 0 {
		return 0, fmt.Errorf("curve %d has no keys", guid)
	}
	if len(c.b.Curves) >= MaxCurves || len(cv.Keys) > MaxCurveKeys {
		return 0, fmt.Errorf("curve %d: too many curves or keys", guid)
	}
	out := Curve{Keys: make([]CurveKey, len(cv.Keys))}
	for i, k := range cv.Keys {
		ck := CurveKey{Time: k.Time, Value: k.Value}
		switch k.Interp {
		case aocexpr.InterpLinear:
			ck.Interp = InterpLinear
		case aocexpr.InterpConstant:
			ck.Interp = InterpConstant
		default:
			ck.Interp = InterpCubic
		}
		ck.Arrive, ck.Leave = k.Arrive, k.Leave
		out.Keys[i] = ck
	}
	i := len(c.b.Curves)
	c.b.Curves = append(c.b.Curves, out)
	c.curveIdx[guid] = i
	return i, nil
}

func (c *compiler) parse(text string) (*aocexpr.Program, error) {
	if p, ok := c.parsed[text]; ok {
		return p, nil
	}
	p, err := aocexpr.Parse(text)
	if err != nil {
		return nil, err
	}
	c.parsed[text] = p
	return p, nil
}

// program compiles the text of one formula and returns its index, and whether it called GetTarget. terms are the input terms
// of the text's own (a StatMod's ValueInputTerms), or nil.
func (c *compiler) program(text string, kind unitKind, what string, terms map[int64]string) (int, bool, error) {
	p, err := c.parse(text)
	if err != nil {
		return 0, false, fmt.Errorf("%s: %w", what, err)
	}
	f := &fnc{c: c, kind: kind}
	if terms != nil {
		f.terms = []map[int64]string{terms}
		f.topTerms = true
	}
	f.pushScope(false)
	v, err := f.tree(p.Tree())
	if err != nil {
		return 0, false, fmt.Errorf("%s: %w", what, err)
	}
	if v.t != tNum && v.t != tBool {
		return 0, false, fmt.Errorf("%s: the formula's value is %v, not a number", what, v.t)
	}
	em := &emitter{c: c}
	em.node(v.n)
	em.op(OpRet)
	if em.err != nil {
		return 0, false, fmt.Errorf("%s: %w", what, em.err)
	}
	if len(em.code) > MaxCodeBytes {
		return 0, false, fmt.Errorf("%s: %d bytes of code (the most is %d)", what, len(em.code), MaxCodeBytes)
	}
	if f.nlocals > MaxLocals {
		return 0, false, fmt.Errorf("%s: %d local variables (the most is %d)", what, f.nlocals, MaxLocals)
	}
	if em.maxH > MaxStack {
		return 0, false, fmt.Errorf("%s: the stack gets %d deep (the most is %d)", what, em.maxH, MaxStack)
	}
	key := strconv.Itoa(f.nlocals) + ":" + string(em.code)
	if i, ok := c.progIdx[key]; ok {
		return i, f.usesTarget, nil
	}
	if len(c.b.Progs) >= MaxPrograms {
		return 0, false, fmt.Errorf("more than %d programs", MaxPrograms)
	}
	i := len(c.b.Progs)
	c.b.Progs = append(c.b.Progs, Program{Locals: uint16(f.nlocals), Code: em.code})
	c.progIdx[key] = i
	return i, f.usesTarget, nil
}

// fnc is the state of compiling one program.
type fnc struct {
	c        *compiler
	kind     unitKind
	scopes   []*scope
	nlocals  int
	inlining []string
	terms    []map[int64]string
	// topTerms: terms[0] is the expression's own StatMod terms (plan D2, stage 2): a term of it may read the others of the same map.
	topTerms bool
	// usesTarget: the text called GetTarget().
	usesTarget bool
}

const maxInline = 16

func (f *fnc) pushScope(barrier bool) {
	f.scopes = append(f.scopes, &scope{vars: map[string]*binding{}, barrier: barrier})
}
func (f *fnc) popScope() { f.scopes = f.scopes[:len(f.scopes)-1] }

func (f *fnc) lookup(name string) *binding {
	for i := len(f.scopes) - 1; i >= 0; i-- {
		if b, ok := f.scopes[i].vars[name]; ok {
			return b
		}
		if f.scopes[i].barrier {
			break
		}
	}
	return nil
}

func (f *fnc) newSlot() int {
	s := f.nlocals
	f.nlocals++
	return s
}

func errAt(pos aocexpr.Pos, format string, args ...any) error {
	return fmt.Errorf("%v: %s", pos, fmt.Sprintf(format, args...))
}

func unsupportedAt(pos aocexpr.Pos, format string, args ...any) error {
	return fmt.Errorf("%v: unsupported %s", pos, fmt.Sprintf(format, args...))
}

const divisionConstruct = "division where no operand is proven to be a float (integer division is not modelled; write 2.0 or Float(x))"

// tree folds a parsed formula (in its own scope) into one value.
func (f *fnc) tree(t aocexpr.Tree) (val, error) {
	var out []stmt
	for _, s := range t.Stmts {
		ss, err := f.stmt(s)
		if err != nil {
			return val{}, err
		}
		out = append(out, ss...)
	}
	r, err := f.expr(t.Result)
	if err != nil {
		return val{}, err
	}
	if len(out) > 0 && (r.t == tNum || r.t == tBool) {
		r.n = nBlock{stmts: out, result: r.n}
	}
	return r, nil
}

func (f *fnc) stmts(list []aocexpr.Stmt) ([]stmt, error) {
	f.pushScope(false)
	defer f.popScope()
	var out []stmt
	for _, s := range list {
		ss, err := f.stmt(s)
		if err != nil {
			return nil, err
		}
		out = append(out, ss...)
	}
	return out, nil
}

// -- C | Y | B . CoA --

func (f *fnc) stmt(s aocexpr.Stmt) ([]stmt, error) {
	switch s := s.(type) {
	case aocexpr.Decl:
		v, err := f.expr(s.X)
		if err != nil {
			return nil, err
		}
		sc := f.scopes[len(f.scopes)-1]
		if _, dup := sc.vars[s.Name]; dup {
			return nil, errAt(s.At, "%q is already declared in this block", s.Name)
		}
		switch v.t {
		case tNum, tBool:
			if _, isConst := v.constant(); isConst && s.Const {
				sc.vars[s.Name] = &binding{t: v.t, konst: true, v: v, float: v.float}
				return nil, nil
			}
			slot := f.newSlot()
			sc.vars[s.Name] = &binding{t: v.t, slot: slot, isSlot: true, float: v.float, etype: v.etype}
			return []stmt{sStore{slot, v.n}}, nil
		default:
			if !s.Const {
				return nil, unsupportedAt(s.At, "var %q holds %v (only numbers and booleans can change)", s.Name, v.t)
			}
			sc.vars[s.Name] = &binding{t: v.t, konst: true, v: v}
			return nil, nil
		}
	case aocexpr.Set:
		b := f.lookup(s.Name)
		if b == nil {
			return nil, errAt(s.At, "set of undeclared variable %q", s.Name)
		}
		if b.konst {
			return nil, errAt(s.At, "set of const %q", s.Name)
		}
		v, err := f.expr(s.X)
		if err != nil {
			return nil, err
		}
		if v.t != b.t {
			return nil, unsupportedAt(s.At, "set of %q (%v) to %v", s.Name, b.t, v.t)
		}
		b.float = b.float && v.float
		return []stmt{sStore{b.slot, v.n}}, nil
	case aocexpr.If:
		cv, err := f.expr(s.Cond)
		if err != nil {
			return nil, err
		}
		if cv.t != tBool {
			return nil, errAt(s.At, "the condition of an if is %v, not a boolean", cv.t)
		}
		if c, ok := cv.constant(); ok {
			if c != 0 {
				return f.stmts(s.Then)
			}
			return f.stmts(s.Else)
		}
		then, err := f.stmts(s.Then)
		if err != nil {
			return nil, err
		}
		els, err := f.stmts(s.Else)
		if err != nil {
			return nil, err
		}
		return []stmt{sIf{cond: cv.n, then: then, els: els}}, nil
	case aocexpr.ExprStmt:
		v, err := f.expr(s.X)
		if err != nil {
			return nil, err
		}
		if _, isConst := v.constant(); (v.t == tNum || v.t == tBool) && !isConst {
			return []stmt{sDrop{v.n}}, nil
		}
		return nil, nil
	}
	return nil, fmt.Errorf("internal error: statement %T", s)
}

func rnd(x float64) float64 { return float64(float32(x)) }

func (f *fnc) num(x float64) float64 {
	if f.c.single {
		return rnd(x)
	}
	return x
}

func b2f(b bool) float64 {
	if b {
		return 1
	}
	return 0
}

func (f *fnc) expr(e aocexpr.Expr) (val, error) {
	switch e := e.(type) {
	case aocexpr.Num:
		n := f.num(e.V)
		if math.IsInf(n, 0) {
			return val{}, errAt(e.At, "the number %v does not fit a float32 (single arithmetic)", e.V)
		}
		return val{t: tNum, n: nConst{n}, float: e.Float}, nil
	case aocexpr.Bool:
		return val{t: tBool, n: nConst{b2f(e.V)}}, nil
	case aocexpr.RefLit:
		return val{t: tRef, ref: e.Ref}, nil
	case aocexpr.Str:
		return val{t: tStr, str: e.V}, nil
	case aocexpr.EnumLit:
		return val{t: tEnum, enum: e.V}, nil
	case aocexpr.Var:
		b := f.lookup(e.Name)
		if b == nil {
			return val{}, errAt(e.At, "unknown variable %q", e.Name)
		}
		if b.konst {
			return b.v, nil
		}
		return val{t: b.t, n: nLoad{b.slot}, float: b.float, etype: b.etype}, nil
	case aocexpr.Unary:
		x, err := f.expr(e.X)
		if err != nil {
			return val{}, err
		}
		if e.Op == "-" {
			if x.t != tNum {
				return val{}, errAt(e.At, "unary - needs a number, got %v", x.t)
			}
			if c, ok := x.constant(); ok {
				return val{t: tNum, n: nConst{f.num(-c)}, float: x.float}, nil
			}
			return val{t: tNum, n: nUn{OpNeg, x.n}, float: x.float}, nil
		}
		if x.t != tBool {
			return val{}, errAt(e.At, "! needs a boolean, got %v", x.t)
		}
		if c, ok := x.constant(); ok {
			return val{t: tBool, n: nConst{b2f(c == 0)}}, nil
		}
		return val{t: tBool, n: nUn{OpNot, x.n}}, nil
	case aocexpr.Binary:
		return f.binary(e)
	case aocexpr.Call:
		return f.call(e)
	}
	return val{}, fmt.Errorf("internal error: expression %T", e)
}

// (C | O | A) (Cyb)

var binOps = map[string]byte{
	"+": OpAdd, "-": OpSub, "*": OpMul, "/": OpDiv, "%": OpMod,
	"<": OpLt, "<=": OpLe, ">": OpGt, ">=": OpGe, "==": OpEq, "!=": OpNe,
}

func (f *fnc) binary(e aocexpr.Binary) (val, error) {
	l, err := f.expr(e.L)
	if err != nil {
		return val{}, err
	}
	if e.Op == "&&" || e.Op == "||" {
		r, err := f.expr(e.R)
		if err != nil {
			return val{}, err
		}
		if l.t != tBool || r.t != tBool {
			return val{}, errAt(e.At, "%s needs booleans, got %v and %v", e.Op, l.t, r.t)
		}
		if c, ok := l.constant(); ok {
			if (e.Op == "&&") == (c != 0) {
				return r, nil // true && x, false || x
			}
			return val{t: tBool, n: nConst{b2f(e.Op == "||")}}, nil
		}
		if e.Op == "&&" {
			return val{t: tBool, n: nAnd{l.n, r.n}}, nil
		}
		return val{t: tBool, n: nOr{l.n, r.n}}, nil
	}
	r, err := f.expr(e.R)
	if err != nil {
		return val{}, err
	}
	op := binOps[e.Op]
	l, r = f.loose(l), f.loose(r)
	if l, r, err = f.enumPair(e, l, r); err != nil {
		return val{}, err
	}
	if op == OpEq || op == OpNe {
		return f.equality(e, op, l, r)
	}
	if l.t != tNum || r.t != tNum {
		return val{}, errAt(e.At, "%s needs numbers, got %v and %v", e.Op, l.t, r.t)
	}
	lc, lok := l.constant()
	rc, rok := r.constant()
	switch op {
	case OpMod:
		// whole numbers: a combo count and its modulus
		if lok && rok && int64(rc) != 0 {
			return val{t: tNum, n: nConst{float64(int64(lc) % int64(rc))}}, nil
		}
		return val{t: tNum, n: nBin{op, l.n, r.n}}, nil
	case OpAdd, OpSub, OpMul, OpDiv:
		fl := l.float || r.float
		if op == OpDiv {
			if !fl {
				return val{}, unsupportedAt(e.At, "%s", divisionConstruct)
			}
		}
		if lok && rok && !(op == OpDiv && rc == 0) {
			// Not folded when it overflows: the interpreter settles that at run time.
			if r := arith(op, lc, rc, f.c.single); !math.IsInf(r, 0) && !math.IsNaN(r) {
				return val{t: tNum, n: nConst{r}, float: fl}, nil
			}
		}
		return val{t: tNum, n: nBin{op, l.n, r.n}, float: fl}, nil
	default: // < <= > >=
		if lok && rok {
			var res bool
			switch op {
			case OpLt:
				res = lc < rc
			case OpLe:
				res = lc <= rc
			case OpGt:
				res = lc > rc
			case OpGe:
				res = lc >= rc
			}
			return val{t: tBool, n: nConst{b2f(res)}}, nil
		}
		return val{t: tBool, n: nBin{op, l.n, r.n}}, nil
	}
}

// enumPair turns the literal a host function's enum answer is compared with into the same number (Nodes 1): GetNodeBiome() == EBiomeType::Riverlands
// compares the host's ordinal with Riverlands' own, and GetCurrentNode(GetPlayer()) != $#...$ the node's rank with the rank of the record named.
func (f *fnc) enumPair(e aocexpr.Binary, l, r val) (val, val, error) {
	conv := func(num, lit val, litPos aocexpr.Pos) (val, error) {
		switch {
		case lit.t == tEnum && num.etype != "" && num.etype != etypeNode && lit.enum.Type == num.etype:
			ord, ok := aocexpr.EnumOrdinal(lit.enum.Type, lit.enum.Value)
			if !ok {
				return lit, errAt(litPos, "%v is not a value of %s", lit.enum, lit.enum.Type)
			}
			return val{t: tNum, n: nConst{float64(ord)}, etype: num.etype}, nil
		case lit.t == tRef && num.etype == etypeNode:
			rank, err := f.c.g.nodeRank(lit.ref)
			if err != nil {
				return lit, errAt(litPos, "%v", err)
			}
			return val{t: tNum, n: nConst{float64(rank)}, etype: etypeNode}, nil
		}
		return lit, nil
	}
	var err error
	if l.t == tNum && l.etype != "" {
		if r, err = conv(l, r, e.R.Pos()); err != nil {
			return l, r, err
		}
	}
	if r.t == tNum && r.etype != "" {
		if l, err = conv(r, l, e.L.Pos()); err != nil {
			return l, r, err
		}
	}
	return l, r, nil
}

// minMaxVal is Min or Max of two numbers, folded when both are constants.
func (f *fnc) minMaxVal(op byte, a, b val) val {
	fl := a.float || b.float
	ac, aok := a.constant()
	bc, bok := b.constant()
	if aok && bok {
		return val{t: tNum, n: nConst{minmax(op, ac, bc)}, float: fl}
	}
	return val{t: tNum, n: nFn2{op, a.n, b.n}, float: fl}
}

// loose is a boolean as a number in an ability expression (the stack holds 1.0 or 0.0 either way): the design data compares and
// adds the booleans formulas give. A stat formula is strict.
func (f *fnc) loose(v val) val {
	if v.t == tBool && f.kind == unitExpr {
		v.t = tNum
		v.float = false
	}
	return v
}

// C O A x Cyb

func (f *fnc) equality(e aocexpr.Binary, op byte, l, r val) (val, error) {
	want := op == OpEq
	switch {
	case l.t == r.t && (l.t == tNum || l.t == tBool):
		lc, lok := l.constant()
		rc, rok := r.constant()
		if lok && rok {
			return val{t: tBool, n: nConst{b2f((lc == rc) == want)}}, nil
		}
		return val{t: tBool, n: nBin{op, l.n, r.n}}, nil
	case l.t == tStr && r.t == tStr:
		return val{t: tBool, n: nConst{b2f((l.str == r.str) == want)}}, nil
	case l.t == tEnum && r.t == tEnum:
		return val{t: tBool, n: nConst{b2f((l.enum == r.enum) == want)}}, nil
	case l.t == tRef && r.t == tRef:
		return val{t: tBool, n: nConst{b2f((l.ref == r.ref) == want)}}, nil
	case l.t == tObj && r.t == tObj:
		if l.role == r.role {
			return val{t: tBool, n: nConst{b2f(want)}}, nil // the same role is the same actor
		}
		// two roles are one actor when the host says so (GetTarget() != GetInstigator() asks whether the hit is on the caster itself)
		if f.kind != unitExpr {
			return val{}, errAt(e.At, "two actors in a stat formula")
		}
		same, err := f.hostCall(aocexpr.Call{Name: "==", At: e.At}, aocexpr.HostSameActor, aocexpr.RolePair(l.role, r.role), tBool)
		if err != nil {
			return val{}, err
		}
		if !want {
			same = val{t: tBool, n: nUn{OpNot, same.n}}
		}
		return same, nil
	}
	return val{}, errAt(e.At, "cannot compare %v with %v", l.t, r.t)
}

// arith is the one place + - * / are computed (the interpreter, the folder).
func arith(op byte, a, b float64, single bool) float64 {
	var r float64
	switch op {
	case OpAdd:
		r = a + b
	case OpSub:
		r = a - b
	case OpMul:
		r = a * b
	case OpDiv:
		r = a / b
	}
	if single {
		r = float64(float32(r))
	}
	return r
}

// argc checks a call's argument count.
func argc(e aocexpr.Call, n int) error {
	if len(e.Args) != n {
		return errAt(e.At, "%s takes %d argument(s), got %d", e.Name, n, len(e.Args))
	}
	return nil
}

func (f *fnc) call(e aocexpr.Call) (val, error) {
	args := make([]val, len(e.Args))
	for i, a := range e.Args {
		v, err := f.expr(a)
		if err != nil {
			return val{}, err
		}
		args[i] = v
	}
	need := func(i int, t vtype) error {
		if t == tNum {
			args[i] = f.loose(args[i])
		}
		if args[i].t != t {
			return errAt(e.Args[i].Pos(), "%s: argument %d is %v, not %v", e.Name, i+1, args[i].t, t)
		}
		return nil
	}
	check := func(types ...vtype) error {
		if err := argc(e, len(types)); err != nil {
			return err
		}
		for i, t := range types {
			if err := need(i, t); err != nil {
				return err
			}
		}
		return nil
	}
	ctx := func(op byte, t vtype, imm ...uint16) (val, error) {
		n := nCtx{op: op}
		copy(n.imm[:], imm)
		return val{t: t, n: n}, nil
	}
	if v, ok, err := f.nodeCall(e, args); ok {
		return v, err
	}
	switch e.Name {
	case "Round", "Floor", "Ceil":
		if err := check(tNum); err != nil {
			return val{}, err
		}
		op := map[string]byte{"Round": OpRound, "Floor": OpFloor, "Ceil": OpCeil}[e.Name]
		if c, ok := args[0].constant(); ok {
			return val{t: tNum, n: nConst{roundOp(op, c)}}, nil
		}
		return val{t: tNum, n: nFn1{op, args[0].n}}, nil
	case "Float":
		if err := check(tNum); err != nil {
			return val{}, err
		}
		args[0].float = true
		return args[0], nil
	case "Min", "Max":
		if err := check(tNum, tNum); err != nil {
			return val{}, err
		}
		op := OpMin
		if e.Name == "Max" {
			op = OpMax
		}
		fl := args[0].float || args[1].float
		a, aok := args[0].constant()
		b, bok := args[1].constant()
		if aok && bok {
			return val{t: tNum, n: nConst{minmax(op, a, b)}, float: fl}, nil
		}
		return val{t: tNum, n: nFn2{op, args[0].n, args[1].n}, float: fl}, nil
	case "SelectFloat", "SelectInt":
		if err := check(tBool, tNum, tNum); err != nil {
			return val{}, err
		}
		fl := e.Name == "SelectFloat"
		cc, cok := args[0].constant()
		a, aok := args[1].constant()
		b, bok := args[2].constant()
		if cok && aok && bok {
			x := b
			if cc != 0 {
				x = a
			}
			return val{t: tNum, n: nConst{x}, float: fl}, nil
		}
		return val{t: tNum, n: nSel{args[0].n, args[1].n, args[2].n}, float: fl}, nil
	case "EvalCurve":
		if err := check(tRef, tNum); err != nil {
			return val{}, err
		}
		r := args[0].ref
		if r.Kind != "" || r.TypeID != TypeStatCurve {
			return val{}, errAt(e.Args[0].Pos(), "EvalCurve: %v is not a StatCurve reference", r)
		}
		idx, err := f.c.curve(r.Guid)
		if err != nil {
			return val{}, errAt(e.At, "EvalCurve %v: %v", r, err)
		}
		return val{t: tNum, n: nCurve{idx, args[1].n}, float: true}, nil
	case "GetOwner", "GetInstigator", "GetSource", "GetSelf":
		if err := argc(e, 0); err != nil {
			return val{}, err
		}
		return val{t: tObj}, nil
	case "GetTarget":
		// An ability expression's target is the caster until there are other actors (self and party hits).
		if f.kind != unitExpr {
			return val{}, unsupportedAt(e.At, "function %q (a stat formula has no target)", e.Name)
		}
		if err := argc(e, 0); err != nil {
			return val{}, err
		}
		f.usesTarget = true
		return val{t: tObj, role: aocexpr.RoleTarget}, nil
	case "GetActiveTarget":
		if f.kind != unitExpr {
			return val{}, unsupportedAt(e.At, "function %q (a stat formula has no target)", e.Name)
		}
		if err := check(tObj, tEnum); err != nil {
			return val{}, err
		}
		if args[1].enum.Type != "ETargetingIntention" || args[0].role != aocexpr.RoleCaster {
			return val{}, unsupportedAt(e.At, "%v of that actor", e.Name)
		}
		switch args[1].enum.Value {
		case "Harm":
			f.usesTarget = true
			return val{t: tObj, role: aocexpr.RoleActiveHarm}, nil
		case "Support":
			f.usesTarget = true
			return val{t: tObj, role: aocexpr.RoleActiveSupport}, nil
		}
		return val{}, unsupportedAt(e.At, "targeting intention %v", args[1].enum)
	case "GetCreatureOwner":
		if f.kind != unitExpr {
			return val{}, unsupportedAt(e.At, "function %q (a stat formula has no pet)", e.Name)
		}
		if err := check(tObj); err != nil {
			return val{}, err
		}
		switch args[0].role {
		case aocexpr.RoleCaster:
			return val{t: tObj, role: aocexpr.RoleCasterOwner}, nil
		case aocexpr.RoleTarget:
			f.usesTarget = true
			return val{t: tObj, role: aocexpr.RoleTargetOwner}, nil
		}
		return val{}, unsupportedAt(e.At, "the owner of that actor")
	case "IsClient":
		if err := check(); err != nil {
			return val{}, err
		}
		return f.hostCall(e, aocexpr.HostIsClient, "", tBool)
	case "GetAbilityComboCount":
		if err := check(); err != nil {
			return val{}, err
		}
		v, err := f.hostCall(e, aocexpr.HostComboCount, "", tNum)
		v.float = false // a whole number: % and == take it
		return v, err
	case "IsComboButtonPressed":
		if err := check(tEnum); err != nil {
			return val{}, err
		}
		if args[0].enum.Type != "EComboButtons" {
			return val{}, errAt(e.Args[0].Pos(), "IsComboButtonPressed: %v is not an EComboButtons", args[0].enum)
		}
		return f.hostCall(e, aocexpr.HostComboButton, args[0].enum.Value, tBool)
	case "GetCurrentWeaponSkilltree":
		if err := check(tObj); err != nil {
			return val{}, err
		}
		return val{t: tRef, ref: aocexpr.WeaponSkilltreeRef}, nil
	case "GetHitRecord":
		if f.kind != unitExpr {
			return val{}, unsupportedAt(e.At, "function %q (a stat formula has no hit)", e.Name)
		}
		if err := check(tObj); err != nil {
			return val{}, err
		}
		return val{t: tObj}, nil
	case "GetHitSourceRecord":
		if f.kind != unitExpr {
			return val{}, unsupportedAt(e.At, "function %q (a stat formula has no hit)", e.Name)
		}
		if err := check(tObj); err != nil {
			return val{}, err
		}
		return val{t: tRef, ref: aocexpr.HitSourceRef}, nil
	case "GetRecordTypeId":
		if err := check(tRef); err != nil {
			return val{}, err
		}
		if args[0].ref == aocexpr.HitSourceRef {
			return f.hostCall(e, aocexpr.HostHitSourceType, "", tNum)
		}
		return val{t: tNum, n: nConst{float64(args[0].ref.TypeID)}, float: true}, nil
	case "GetProjectileTypeId":
		if err := check(); err != nil {
			return val{}, err
		}
		return val{t: tNum, n: nConst{float64(aocexpr.TypeProjectile)}, float: true}, nil
	case "DoesHitContainActor":
		if err := check(tObj, tObj); err != nil {
			return val{}, err
		}
		return f.hostCall(e, aocexpr.HostHitContains, strconv.Itoa(args[1].role), tBool)
	case "GetClosestTargetToActorInHit":
		if f.kind != unitExpr {
			return val{}, unsupportedAt(e.At, "function %q (a stat formula has no hit)", e.Name)
		}
		if err := check(tObj, tObj); err != nil {
			return val{}, err
		}
		if args[1].role != aocexpr.RoleCaster {
			return val{}, unsupportedAt(e.At, "%v of another actor", e.Name)
		}
		return val{t: tObj, role: aocexpr.RoleClosestInHit}, nil
	case "RandIntRange":
		if err := check(tNum, tNum); err != nil {
			return val{}, err
		}
		return f.hostCall(e, aocexpr.HostRandIntRange, "", tNum, args[0], args[1])
	case "GetDistanceBetween":
		if err := check(tObj, tObj); err != nil {
			return val{}, err
		}
		return f.hostCall(e, aocexpr.HostDistanceBetween, aocexpr.RolePair(args[0].role, args[1].role), tNum)
	case "GetHealthPercentage":
		if err := check(tObj); err != nil {
			return val{}, err
		}
		hg, ok := f.c.g.StatByName("Health")
		mg, ok2 := f.c.g.StatByName("MaxHealth")
		if !ok || !ok2 {
			return val{}, unsupportedAt(e.At, "%v (the graph has no Health and MaxHealth)", e.Name)
		}
		hi, err := f.c.stat(hg)
		if err != nil {
			return val{}, errAt(e.At, "%v", err)
		}
		mi, err := f.c.stat(mg)
		if err != nil {
			return val{}, errAt(e.At, "%v", err)
		}
		health := f.actor(args[0].role, val{t: tNum, n: nStat{hi}, float: true})
		max := f.actor(args[0].role, val{t: tNum, n: nStat{mi}, float: true})
		return val{t: tNum, n: nBin{OpDiv, health.n, max.n}, float: true}, nil
	case "HasAlignment", "GetEffectRemainingDuration":
		if err := check(tObj, tRef); err != nil {
			return val{}, err
		}
		if e.Name == "HasAlignment" {
			return f.hostCall(e, aocexpr.RoleFn(args[0].role, aocexpr.RoleOpHasAlignment), strconv.FormatInt(args[1].ref.Guid, 10), tBool)
		}
		return f.hostCall(e, aocexpr.RoleFn(args[0].role, aocexpr.RoleOpEffectRemaining), strconv.FormatInt(args[1].ref.Guid, 10), tNum)
	case "GetRemainingAbilityCooldown":
		if err := check(tRef); err != nil {
			return val{}, err
		}
		return f.hostCall(e, aocexpr.HostCooldownLeft, strconv.FormatInt(args[0].ref.Guid, 10), tNum)
	case "GetHitTriggerCount", "GetAbilityHitCount":
		if err := check(tObj, tRef); err != nil {
			return val{}, err
		}
		fn := aocexpr.HostHitTriggerCount
		if e.Name == "GetAbilityHitCount" {
			fn = aocexpr.HostAbilityHitCount
		}
		return f.hostCall(e, fn, strconv.FormatInt(args[1].ref.Guid, 10), tNum, val{t: tNum, n: nConst{float64(args[0].role)}})
	case "CalculateDamage", "CalculateDamageMultiplier", "CalculateDamageAdditiveMultiplier", "CalculateDamageMitigation", "CalculateDamageCritical",
		"CalculateDamagePenetration", "CalculateDamageDampening":
		if err := check(); err != nil {
			return val{}, err
		}
		fn := 0
		for i, name := range aocexpr.DamageFnNames {
			if name == e.Name {
				fn = aocexpr.HostCalcDamage + i
			}
		}
		f.usesTarget = true
		return f.hostCall(e, fn, "", tNum)
	case "Lerp":
		if err := check(tNum, tNum, tNum); err != nil {
			return val{}, err
		}
		a, aok := args[0].constant()
		b, bok := args[1].constant()
		t, tok := args[2].constant()
		if aok && bok && tok {
			d := arith(OpSub, b, a, f.c.single)
			p := arith(OpMul, t, d, f.c.single)
			if r := arith(OpAdd, a, p, f.c.single); !math.IsInf(r, 0) && !math.IsNaN(r) {
				return val{t: tNum, n: nConst{r}, float: true}, nil
			}
		}
		return val{t: tNum, n: nFn3{OpLerp, args[0].n, args[1].n, args[2].n}, float: true}, nil
	case "Abs":
		if err := check(tNum); err != nil {
			return val{}, err
		}
		if c, ok := args[0].constant(); ok {
			return val{t: tNum, n: nConst{math.Abs(c)}, float: args[0].float}, nil
		}
		return val{t: tNum, n: nFn1{OpAbs, args[0].n}, float: args[0].float}, nil
	case "GetAbilityProgressRatio":
		if f.kind != unitExpr {
			return val{}, unsupportedAt(e.At, "function %q (a stat formula has no running ability)", e.Name)
		}
		group := "" // the form with no argument asks about the ability that is running
		if len(e.Args) != 0 {
			if err := check(tStr); err != nil {
				return val{}, err
			}
			group = args[0].str
		}
		i, err := f.c.strOperand(group)
		if err != nil {
			return val{}, errAt(e.At, "%v", err)
		}
		v, _ := ctx(OpProgress, tNum, i)
		v.float = true
		return v, nil
	case "GetEffectCountByRecordAndInstigator":
		// The target and the instigator are both the caster for now (see GetTarget).
		if f.kind != unitExpr {
			return val{}, unsupportedAt(e.At, "function %q (a stat formula has no instigator of an effect)", e.Name)
		}
		if err := check(tObj, tRef, tObj); err != nil {
			return val{}, err
		}
		if args[0].role != aocexpr.RoleCaster || args[2].role != aocexpr.RoleCaster {
			return f.hostCall(e, aocexpr.RoleFn(args[0].role, aocexpr.RoleOpEffectInst), strconv.FormatInt(args[1].ref.Guid, 10), tNum, val{t: tNum, n: nConst{float64(args[2].role)}})
		}
		i, err := f.c.ref(args[1].ref)
		if err != nil {
			return val{}, errAt(e.At, "%v", err)
		}
		return ctx(OpEffectInst, tNum, i)
	case "GetRemainingAbilityCharges", "IsApplicableTarget", "IsAnyMovementKeyPressed", "isanymovementkeypressed", "GetStatDeltaOverTime", "GetHitEvent",
		"GetStatModValue", "GetStatModEffectiveValue", "IsNeutral", "IsEnemy", "GetAngleBetween", "Now", "GetProgressDuration":
		if f.kind != unitExpr {
			return val{}, unsupportedAt(e.At, "function %q (a stat formula has no host)", e.Name)
		}
		host := func(fn int, name string, t vtype, args ...val) (val, error) {
			i, err := f.c.strOperand(name)
			if err != nil {
				return val{}, errAt(e.At, "%v", err)
			}
			nodes := make([]node, len(args))
			for k, a := range args {
				nodes[k] = a.n
			}
			op := []byte{OpHost0, OpHost1, OpHost2}[len(args)]
			return val{t: t, n: nHost{op: op, fn: uint16(fn), str: i, args: nodes}, float: true}, nil
		}
		switch e.Name {
		case "GetRemainingAbilityCharges":
			if err := check(tRef); err != nil {
				return val{}, err
			}
			return host(aocexpr.HostRemainingCharges, strconv.FormatInt(args[0].ref.Guid, 10), tNum)
		case "IsApplicableTarget":
			if err := check(tObj, tEnum); err != nil {
				return val{}, err
			}
			if args[1].enum.Type != "EAffectedTarget" {
				return val{}, errAt(e.Args[1].Pos(), "IsApplicableTarget: %v is not an EAffectedTarget", args[1].enum)
			}
			return host(aocexpr.HostApplicableTarget, args[1].enum.Value, tBool)
		case "IsAnyMovementKeyPressed", "isanymovementkeypressed":
			if err := check(); err != nil {
				return val{}, err
			}
			return host(aocexpr.HostMovementKey, "", tBool)
		case "GetStatDeltaOverTime":
			if err := check(tObj, tNum, tRef); err != nil {
				return val{}, err
			}
			return host(aocexpr.HostStatDelta, strconv.FormatInt(args[2].ref.Guid, 10), tNum, args[1])
		case "GetHitEvent":
			if err := check(); err != nil {
				return val{}, err
			}
			return val{t: tObj}, nil
		case "GetStatModValue", "GetStatModEffectiveValue":
			fn := aocexpr.HostHitStatMod
			if e.Name == "GetStatModEffectiveValue" {
				fn = aocexpr.HostHitStatEffective
			}
			if len(e.Args) == 3 {
				if err := check(tObj, tObj, tRef); err != nil {
					return val{}, err
				}
				return host(fn, strconv.FormatInt(args[2].ref.Guid, 10), tNum)
			}
			if err := check(tObj, tRef); err != nil {
				return val{}, err
			}
			return host(fn, strconv.FormatInt(args[1].ref.Guid, 10), tNum)
		case "IsNeutral", "IsEnemy":
			if err := check(tObj, tObj); err != nil {
				return val{}, err
			}
			fn := aocexpr.HostIsNeutral
			if e.Name == "IsEnemy" {
				fn = aocexpr.HostIsEnemy
			}
			return host(fn, aocexpr.RolePair(args[0].role, args[1].role), tBool)
		case "GetAngleBetween":
			if err := check(tObj, tObj); err != nil {
				return val{}, err
			}
			return host(aocexpr.HostAngleBetween, aocexpr.RolePair(args[0].role, args[1].role), tNum)
		case "Now":
			if err := check(); err != nil {
				return val{}, err
			}
			return host(aocexpr.HostNow, "", tNum)
		default: // GetProgressDuration
			if err := check(tStr); err != nil {
				return val{}, err
			}
			return host(aocexpr.HostProgressDuration, args[0].str, tNum)
		}
	case "RandDoubleRange":
		if f.kind != unitExpr {
			return val{}, unsupportedAt(e.At, "function %q (a stat formula has no random source)", e.Name)
		}
		if err := check(tNum, tNum); err != nil {
			return val{}, err
		}
		r, _ := ctx(OpRand, tNum)
		return val{t: tNum, n: nFn3{OpLerp, args[0].n, args[1].n, r.n}, float: true}, nil
	case "GetHitInProgress":
		if f.kind != unitExpr {
			return val{}, unsupportedAt(e.At, "function %q (a stat formula has no hit)", e.Name)
		}
		if err := argc(e, 0); err != nil {
			return val{}, err
		}
		return val{t: tObj}, nil
	case "CheckForDamageTag", "HitHasTag":
		if f.kind != unitExpr {
			return val{}, unsupportedAt(e.At, "function %q (a stat formula has no hit)", e.Name)
		}
		if err := check(tObj, tStr); err != nil {
			return val{}, err
		}
		i, err := f.c.strOperand(args[1].str)
		if err != nil {
			return val{}, errAt(e.At, "%v", err)
		}
		return ctx(OpHitTag, tBool, i)
	case "GetHitFlags":
		if f.kind != unitExpr {
			return val{}, unsupportedAt(e.At, "function %q (a stat formula has no hit)", e.Name)
		}
		if err := check(tObj, tObj); err != nil {
			return val{}, err
		}
		return val{t: tFlags}, nil
	case "HitFlagsContains":
		if f.kind != unitExpr {
			return val{}, unsupportedAt(e.At, "function %q (a stat formula has no hit)", e.Name)
		}
		if err := check(tFlags, tEnum); err != nil {
			return val{}, err
		}
		if args[1].enum.Type != "EHitFlag" {
			return val{}, errAt(e.Args[1].Pos(), "HitFlagsContains: %v is not an EHitFlag", args[1].enum)
		}
		i, err := f.c.strOperand(args[1].enum.Value)
		if err != nil {
			return val{}, errAt(e.At, "%v", err)
		}
		return ctx(OpHitFlag, tBool, i)
	case "GetNumTargets":
		if f.kind != unitExpr {
			return val{}, unsupportedAt(e.At, "function %q (a stat formula has no hit)", e.Name)
		}
		if err := check(tObj); err != nil {
			return val{}, err
		}
		return ctx(OpHitTargets, tNum)
	case "GetXPDebtRatio":
		if f.kind != unitExpr {
			return val{}, unsupportedAt(e.At, "function %q (a stat formula has no experience debt)", e.Name)
		}
		if err := check(tActor); err != nil {
			return val{}, err
		}
		v, _ := ctx(OpXpDebt, tNum)
		v.float = true
		return v, nil
	case "Clamp":
		if err := check(tNum, tNum, tNum); err != nil {
			return val{}, err
		}
		lo := f.minMaxVal(OpMax, args[0], args[1])
		return f.minMaxVal(OpMin, lo, args[2]), nil
	case "RandDouble":
		if f.kind != unitExpr {
			return val{}, unsupportedAt(e.At, "function %q (a stat formula has no random source)", e.Name)
		}
		if err := check(); err != nil {
			return val{}, err
		}
		v, _ := ctx(OpRand, tNum)
		v.float = true
		return v, nil
	case "Int":
		// int() truncates toward zero; the reward tables truncate non-negative rolls, where Floor is the same number.
		if err := check(tNum); err != nil {
			return val{}, err
		}
		if c, ok := args[0].constant(); ok {
			return val{t: tNum, n: nConst{roundOp(OpFloor, c)}}, nil
		}
		return val{t: tNum, n: nFn1{OpFloor, args[0].n}}, nil
	case "GetPlayerLevel":
		if err := check(); err != nil {
			return val{}, err
		}
		return f.hostCall(e, aocexpr.HostPlayerLevel, "", tNum)
	case "DoesNPCContainTag":
		if err := check(tStr); err != nil {
			return val{}, err
		}
		return f.hostCall(e, aocexpr.HostNpcTag, args[0].str, tBool)
	case "GetPlayerLootMaxLevel", "GetPlayerLootAvgLuck":
		if err := check(); err != nil {
			return val{}, err
		}
		fn := aocexpr.HostLootMaxLevel
		if e.Name == "GetPlayerLootAvgLuck" {
			fn = aocexpr.HostLootAvgLuck
		}
		v, err := f.hostCall(e, fn, "", tNum)
		v.float = true
		return v, err
	case "GetCharacterLevel", "GetNPCLevel":
		if len(e.Args) == 0 && e.Name == "GetNPCLevel" { // GetNPCLevel() of a reward table: the NPC that died (the owner)
			return val{t: tNum, n: nLevel{}}, nil
		}
		if err := check(tObj); err != nil {
			return val{}, err
		}
		return f.actor(args[0].role, val{t: tNum, n: nLevel{}}), nil
	case "GetNpcStatDistribution":
		if err := check(tObj, tEnum); err != nil {
			return val{}, err
		}
		i := aocexpr.StatDistributionIndex(args[1].enum.Value)
		if args[1].enum.Type != "EStatDistributionStat" || i < 0 {
			return val{}, errAt(e.Args[1].Pos(), "GetNpcStatDistribution: %v is not an EStatDistributionStat", args[1].enum)
		}
		if args[0].role != aocexpr.RoleCaster {
			return val{}, unsupportedAt(e.At, "%v of another actor", e.Name)
		}
		v, _ := ctx(OpNpcDist, tNum, uint16(i))
		v.float = true
		return v, nil
	case "GetStat":
		if err := check(tObj, tRef); err != nil {
			return val{}, err
		}
		guid, err := f.statRef(args[1].ref)
		if err != nil {
			return val{}, errAt(e.Args[1].Pos(), "GetStat: %v", err)
		}
		idx, err := f.c.stat(guid)
		if err != nil {
			return val{}, errAt(e.At, "GetStat: %v", err)
		}
		return f.actor(args[0].role, val{t: tNum, n: nStat{idx}, float: true}), nil
	case "GetStatInitRunningTotal":
		if err := check(); err != nil {
			return val{}, err
		}
		if f.kind != unitInit {
			return val{}, errAt(e.At, "GetStatInitRunningTotal outside a stat initializer")
		}
		return val{t: tNum, n: nTotal{}, float: true}, nil
	case "EvalE", "EvalFormula", "EvalEquation", "EvalF":
		if err := check(tRef); err != nil {
			return val{}, err
		}
		r := args[0].ref
		isFormula := e.Name == "EvalFormula" || e.Name == "EvalF"
		want := uint64(TypeStatEquation)
		if isFormula {
			want = TypeStatFormula
		}
		if r.Kind != "" || r.TypeID != want {
			return val{}, unsupportedAt(e.Args[0].Pos(), "%s of %v (only $#<TypeId>:<Guid>$ of the %s table)", e.Name, r, map[bool]string{true: "formula", false: "equation"}[isFormula])
		}
		return f.inline(e, r.Guid, isFormula)
	case "GetTerm":
		if err := check(tRef); err != nil {
			return val{}, err
		}
		return f.term(e, args[0].ref)
	case "GetEffectCount":
		if err := check(tObj, tRef); err != nil {
			return val{}, err
		}
		i, err := f.c.ref(args[1].ref)
		if err != nil {
			return val{}, errAt(e.At, "%v", err)
		}
		v, _ := ctx(OpEffect, tNum, i)
		return f.actor(args[0].role, v), nil
	case "GetEffectCountByTag":
		if err := check(tObj, tStr); err != nil {
			return val{}, err
		}
		i, err := f.c.strOperand(args[1].str)
		if err != nil {
			return val{}, errAt(e.At, "%v", err)
		}
		v, _ := ctx(OpEffectTag, tNum, i)
		return f.actor(args[0].role, v), nil
	case "GetSkillPurchased":
		if err := check(tObj, tRef, tRef); err != nil {
			return val{}, err
		}
		a, err := f.c.ref(args[1].ref)
		if err != nil {
			return val{}, errAt(e.At, "%v", err)
		}
		b, err := f.c.ref(args[2].ref)
		if err != nil {
			return val{}, errAt(e.At, "%v", err)
		}
		v, _ := ctx(OpSkill, tBool, a, b)
		return f.actor(args[0].role, v), nil
	case "IsInCombat", "IsMoving", "IsUnkillable":
		if err := check(tObj); err != nil {
			return val{}, err
		}
		v, _ := ctx(map[string]byte{"IsInCombat": OpCombat, "IsMoving": OpMoving, "IsUnkillable": OpUnkillable}[e.Name], tBool)
		return f.actor(args[0].role, v), nil
	case "EquippedItemInSlotHasTag":
		if err := check(tEnum, tStr); err != nil {
			return val{}, err
		}
		if args[0].enum.Type != "EEquipSlot" {
			return val{}, errAt(e.Args[0].Pos(), "EquippedItemInSlotHasTag: %v is not an EEquipSlot", args[0].enum)
		}
		a, err := f.c.strOperand(args[0].enum.Value)
		if err != nil {
			return val{}, errAt(e.At, "%v", err)
		}
		b, err := f.c.strOperand(args[1].str)
		if err != nil {
			return val{}, errAt(e.At, "%v", err)
		}
		return ctx(OpEquipTag, tBool, a, b)
	case "GetActorGuid":
		if err := check(tObj); err != nil {
			return val{}, err
		}
		return val{t: tActor}, nil
	case "GetPlayerProfessionLevel":
		if err := check(tActor, tStr); err != nil {
			return val{}, err
		}
		i, err := f.c.strOperand(args[1].str)
		if err != nil {
			return val{}, errAt(e.At, "%v", err)
		}
		return ctx(OpProfession, tNum, i)
	case "GetConsumedItemStat":
		if err := check(tRef); err != nil {
			return val{}, err
		}
		i, err := f.c.ref(args[0].ref)
		if err != nil {
			return val{}, errAt(e.At, "%v", err)
		}
		v, _ := ctx(OpConsumed, tNum, i)
		v.float = true
		return v, nil
	case "GetNpcEliteRank":
		if err := check(tObj); err != nil {
			return val{}, err
		}
		v, _ := ctx(OpNpcRank, tNum)
		return f.actor(args[0].role, v), nil
	}
	return val{}, unsupportedAt(e.At, "function %q", e.Name)
}

// actor runs a read of an actor's own state in that actor's context (the caster's needs none).
func (f *fnc) actor(role int, v val) val {
	if role != aocexpr.RoleCaster {
		v.n = nActor{role, v.n}
	}
	return v
}

// hostCall is a call of a named host function (aocexpr/hostfn.go): its number, its name operand and its numeric arguments (0, 1 or 2). Only an ability
// expression has a host.
func (f *fnc) hostCall(e aocexpr.Call, fn int, name string, t vtype, args ...val) (val, error) {
	if f.kind != unitExpr {
		return val{}, unsupportedAt(e.At, "function %q (a stat formula has no host)", e.Name)
	}
	i, err := f.c.strOperand(name)
	if err != nil {
		return val{}, errAt(e.At, "%v", err)
	}
	nodes := make([]node, len(args))
	for k, a := range args {
		nodes[k] = a.n
	}
	op := []byte{OpHost0, OpHost1, OpHost2}[len(args)]
	return val{t: t, n: nHost{op: op, fn: uint16(fn), str: i, args: nodes}, float: true}, nil
}

func roundOp(op byte, x float64) float64 {
	switch op {
	case OpRound:
		return math.Floor(x + 0.5)
	case OpFloor:
		return math.Floor(x)
	}
	return math.Ceil(x)
}

// minmax is math.Min / math.Max, which the interpreters match.
func minmax(op byte, a, b float64) float64 {
	if op == OpMin {
		return math.Min(a, b)
	}
	return math.Max(a, b)
}

// statRef resolves the reference GetStat names to a stat guid.
func (f *fnc) statRef(r aocexpr.Ref) (int64, error) {
	if r.Kind == "" {
		if r.TypeID != TypeStatDef {
			return 0, fmt.Errorf("%v is not a StatTypeDef reference", r)
		}
		return r.Guid, nil
	}
	if r.Kind != "Stat" {
		return 0, fmt.Errorf("%v is not a stat reference", r)
	}
	guid, ok := f.c.g.StatByName(r.Name)
	if !ok {
		return 0, fmt.Errorf("no stat (or more than one) is called %q", r.Name)
	}
	return guid, nil
}

// CoA.C:Y:B

// inline replaces EvalE / EvalFormula by the equation's code.
func (f *fnc) inline(e aocexpr.Call, guid int64, formula bool) (val, error) {
	key := "E" + strconv.FormatInt(guid, 10)
	var terms map[int64]string
	eq := guid
	if formula {
		key = "F" + strconv.FormatInt(guid, 10)
		fm, ok := f.c.g.Formulas[guid]
		if !ok {
			return val{}, errAt(e.At, "%s: no formula %d", e.Name, guid)
		}
		terms, eq = fm.Terms, fm.Equation
	}
	text, ok := f.c.g.Equations[eq]
	if !ok {
		return val{}, errAt(e.At, "%s: no equation %d (or it has constraints, which are not modelled)", e.Name, eq)
	}
	if err := f.enter(key); err != nil {
		return val{}, errAt(e.At, "%s: %v", e.Name, err)
	}
	defer f.leave()
	p, err := f.c.parse(text)
	if err != nil {
		return val{}, errAt(e.At, "%s %d: %v", e.Name, guid, err)
	}
	f.terms = append(f.terms, terms)
	f.pushScope(true)
	v, err := f.tree(p.Tree())
	f.popScope()
	f.terms = f.terms[:len(f.terms)-1]
	if err != nil {
		return val{}, errAt(e.At, "%s %d: %v", e.Name, guid, err)
	}
	if v.t == tBool && f.kind == unitExpr {
		return v, nil // an ability's requirement may be a formula that gives a boolean
	}
	if v.t != tNum {
		return val{}, errAt(e.At, "%s %d: its value is %v, not a number", e.Name, guid, v.t)
	}
	v.float = true // a stat equation is a float (aocexpr.StatFloatFuncs)
	if cl, ok := f.c.g.Clamps[eq]; ok {
		v = f.clamp(v, cl)
	}
	return v, nil
}

// clamp applies an equation's constraint: raised to its Min, then lowered to its Max (Unreal's Clamp order).
func (f *fnc) clamp(v val, cl Clamp) val {
	limit := func(op byte, v val, bound float64) val {
		b := f.num(bound)
		if c, ok := v.constant(); ok {
			return val{t: tNum, n: nConst{minmax(op, c, b)}, float: true}
		}
		return val{t: tNum, n: nFn2{op, v.n, nConst{b}}, float: true}
	}
	if cl.HasMin {
		v = limit(OpMax, v, cl.Min)
	}
	if cl.HasMax {
		v = limit(OpMin, v, cl.Max)
	}
	return v
}

// term replaces GetTerm by the term's expression, in the scope of the formula's caller. The term is the innermost formula's own, or
// else one the callers give (a StatMod's ValueInputTerms feed the formula its Value calls: the formula's own InputTerms need not name
// every term its equation reads).
func (f *fnc) term(e aocexpr.Call, r aocexpr.Ref) (val, error) {
	if r.Kind != "" {
		return val{}, errAt(e.At, "GetTerm: the formula has no term %v", r)
	}
	if len(f.terms) == 0 || f.terms[len(f.terms)-1] == nil && !f.anyTerms() {
		return val{}, errAt(e.At, "GetTerm outside a formula")
	}
	idx := -1
	for i := len(f.terms) - 1; i >= 0; i-- {
		if _, ok := f.terms[i][r.Guid]; ok {
			idx = i
			break
		}
	}
	td, known := f.c.g.Terms[r.Guid]
	var text string
	switch {
	case idx >= 0:
		text = f.terms[idx][r.Guid]
	case known:
		text = td.Default // nothing supplies the term: its own default ("" is 0)
	default:
		return val{}, errAt(e.At, "GetTerm: the formula has no term %v", r)
	}
	var v val
	if idx < 0 && strings.TrimSpace(text) == "" {
		v = val{t: tNum, n: nConst{0}, float: true}
	} else {
		scopeDepth := idx
		if idx < 0 {
			scopeDepth = 0
		}
		if idx == 0 && f.topTerms {
			scopeDepth = 1 // the StatMod's terms are written against each other (a term's text asks for its siblings)
		}
		key := "T" + strconv.FormatInt(r.Guid, 10) + "@" + strconv.Itoa(scopeDepth+1)
		if err := f.enter(key); err != nil {
			return val{}, errAt(e.At, "GetTerm: %v", err)
		}
		defer f.leave()
		p, err := f.c.parse(text)
		if err != nil {
			return val{}, errAt(e.At, "GetTerm %v: %v", r, err)
		}
		saved := f.terms
		f.terms = append([]map[int64]string(nil), saved[:scopeDepth]...) // a copy: the callee pushes onto it
		f.pushScope(true)
		v, err = f.tree(p.Tree())
		f.popScope()
		f.terms = saved
		if err != nil {
			return val{}, errAt(e.At, "GetTerm %v: %v", r, err)
		}
		if v.t != tNum {
			return val{}, errAt(e.At, "GetTerm %v: its value is %v, not a number", r, v.t)
		}
		v.float = true
	}
	if known {
		v = f.clamp(v, td.Clamp)
	}
	return v, nil
}

// C:Y:B and C.O.A

// anyTerms says some formula on the stack has terms.
func (f *fnc) anyTerms() bool {
	for _, t := range f.terms {
		if t != nil {
			return true
		}
	}
	return false
}

func (f *fnc) enter(key string) error {
	for _, k := range f.inlining {
		if k == key {
			return fmt.Errorf("it calls itself")
		}
	}
	if len(f.inlining) >= maxInline {
		return fmt.Errorf("calls nest more than %d deep", maxInline)
	}
	f.inlining = append(f.inlining, key)
	return nil
}

func (f *fnc) leave() { f.inlining = f.inlining[:len(f.inlining)-1] }

// ---- emitter ----

type emitter struct {
	c    *compiler
	code []byte
	h    int
	maxH int
	err  error
}

func (e *emitter) fail(err error) {
	if e.err == nil {
		e.err = err
	}
}

func (e *emitter) push(n int) {
	e.h += n
	if e.h > e.maxH {
		e.maxH = e.h
	}
}

// op emits an instruction with its operands.
func (e *emitter) op(op byte, operands ...uint16) {
	if e.err != nil {
		return
	}
	e.code = append(e.code, op)
	switch operandBytes(op) {
	case 1:
		e.code = append(e.code, byte(operands[0]))
	case 2, 4:
		for _, o := range operands {
			e.code = binary.LittleEndian.AppendUint16(e.code, o)
		}
	}
	pop, push := stackEffect(op)
	if e.h < pop {
		e.fail(fmt.Errorf("internal error: stack underflow emitting %#x", op))
		return
	}
	e.h -= pop
	e.push(push)
}

// jump emits a forward jump and returns where its target goes.
func (e *emitter) jump(op byte) int {
	e.op(op, 0)
	return len(e.code) - 2
}

func (e *emitter) patch(at int) {
	if len(e.code) > math.MaxUint16 {
		e.fail(fmt.Errorf("code too long to jump over"))
		return
	}
	binary.LittleEndian.PutUint16(e.code[at:], uint16(len(e.code)))
}

// ~ C-O-A & C:Y:B ~

func (e *emitter) node(n node) {
	if e.err != nil {
		return
	}
	switch n := n.(type) {
	case nConst:
		i, err := e.c.constant(n.f)
		if err != nil {
			e.fail(err)
			return
		}
		e.op(OpConst, i)
	case nLoad:
		e.op(OpLoad, uint16(n.slot))
	case nStat:
		e.op(OpStat, uint16(n.idx))
	case nTotal:
		e.op(OpTotal)
	case nLevel:
		e.op(OpLevel)
	case nUn:
		e.node(n.x)
		e.op(n.op)
	case nBin:
		e.node(n.l)
		e.node(n.r)
		e.op(n.op)
	case nAnd:
		e.node(n.l)
		jf := e.jump(OpJz)
		e.node(n.r)
		je := e.jump(OpJmp)
		e.patch(jf)
		e.h-- // the branch that jumped here holds one value less than the one that fell through
		i, err := e.c.constant(0)
		if err != nil {
			e.fail(err)
			return
		}
		e.op(OpConst, i)
		e.patch(je)
	case nOr:
		e.node(n.l)
		jt := e.jump(OpJnz)
		e.node(n.r)
		je := e.jump(OpJmp)
		e.patch(jt)
		e.h--
		i, err := e.c.constant(1)
		if err != nil {
			e.fail(err)
			return
		}
		e.op(OpConst, i)
		e.patch(je)
	case nSel:
		e.node(n.c)
		e.node(n.a)
		e.node(n.b)
		e.op(OpSel)
	case nFn1:
		e.node(n.x)
		e.op(n.op)
	case nFn2:
		e.node(n.a)
		e.node(n.b)
		e.op(n.op)
	case nFn3:
		e.node(n.a)
		e.node(n.b)
		e.node(n.c)
		e.op(n.op)
	case nCurve:
		e.node(n.x)
		e.op(OpCurve, uint16(n.idx))
	case nHost:
		for _, a := range n.args {
			e.node(a)
		}
		e.op(n.op, n.fn, n.str)
	case nActor:
		e.op(OpActor, uint16(n.role))
		e.node(n.x)
		e.op(OpActor, 0)
	case nCtx:
		switch operandBytes(n.op) {
		case 0:
			e.op(n.op)
		case 1:
			e.op(n.op, n.imm[0])
		case 2:
			e.op(n.op, n.imm[0])
		default:
			e.op(n.op, n.imm[0], n.imm[1])
		}
	case nBlock:
		for _, s := range n.stmts {
			e.stmt(s)
		}
		e.node(n.result)
	default:
		e.fail(fmt.Errorf("internal error: node %T", n))
	}
}

func (e *emitter) stmt(s stmt) {
	switch s := s.(type) {
	case sStore:
		e.node(s.x)
		e.op(OpStore, uint16(s.slot))
	case sDrop:
		e.node(s.x)
		e.op(OpPop)
	case sIf:
		e.node(s.cond)
		jz := e.jump(OpJz)
		for _, t := range s.then {
			e.stmt(t)
		}
		if len(s.els) == 0 {
			e.patch(jz)
			return
		}
		je := e.jump(OpJmp)
		e.patch(jz)
		for _, t := range s.els {
			e.stmt(t)
		}
		e.patch(je)
	}
}

// (C O A) (Cyb)

// ---- the order ----

// order computes the evaluation order: a depth-first walk of what each node
// reads, in stat order, so that the result is the same on every run.
func (c *compiler) order() error {
	b := c.b
	ns := len(b.Stats)
	infos := make([]progInfo, len(b.Progs))
	for i := range b.Progs {
		inf, err := b.analyze(i)
		if err != nil {
			return fmt.Errorf("internal error: %w", err)
		}
		infos[i] = inf
	}
	initDeps := make([][]uint16, ns)
	addSteps := func(steps []Step) {
		for _, s := range steps {
			initDeps[s.Stat] = append(initDeps[s.Stat], infos[s.Prog].stats...)
			// an entry that reads the running total sees the stat's value so far, Base included (engine.go): the Base runs at the
			// initial-value node, after the stats it reads
			if infos[s.Prog].total && b.Stats[s.Stat].Base >= 0 {
				initDeps[s.Stat] = append(initDeps[s.Stat], infos[b.Stats[s.Stat].Base].stats...)
			}
		}
	}
	addSteps(b.CommonSteps)
	for _, cl := range b.Classes {
		addSteps(cl.Steps)
	}
	valueDeps := make([][]uint16, ns)
	for i, s := range b.Stats {
		for _, p := range []int32{s.Base, s.Min, s.Max} {
			if p >= 0 {
				valueDeps[i] = append(valueDeps[i], infos[p].stats...)
			}
		}
	}
	state := make([]uint8, 2*ns) // 0 new, 1 on the path, 2 done
	var stack []string
	var visit func(node int) error
	visit = func(node int) error {
		switch state[node] {
		case 2:
			return nil
		case 1:
			return fmt.Errorf("statprog: the stat formulas depend on themselves: %s -> %s", strings.Join(stack, " -> "), b.Strings[b.Stats[node>>1].Name])
		}
		state[node] = 1
		stack = append(stack, b.Strings[b.Stats[node>>1].Name])
		s := node >> 1
		var deps []uint16
		if node&1 == NodeInit {
			deps = initDeps[s]
		} else {
			if err := visit(2*s + NodeInit); err != nil {
				return err
			}
			deps = valueDeps[s]
		}
		sorted := append([]uint16(nil), deps...)
		sort.Slice(sorted, func(i, j int) bool { return sorted[i] < sorted[j] })
		for _, t := range sorted {
			if err := visit(2*int(t) + NodeValue); err != nil {
				return err
			}
		}
		stack = stack[:len(stack)-1]
		state[node] = 2
		b.Order = append(b.Order, uint32(node))
		return nil
	}
	for s := 0; s < ns; s++ {
		if err := visit(2*s + NodeValue); err != nil {
			return err
		}
	}
	return nil
}
