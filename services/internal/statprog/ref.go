// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package statprog

import (
	"fmt"
	"math"
	"strings"

	"aocrop/services/internal/aocexpr"
)

// The reference engine: the same stat model run straight over the formulas'
// text with aocexpr (a tree walk, lazy, no compiled program), which is what the
// compiled engine must agree with. It exists for the tests and for tools that
// want an answer without a compile; it is slower and stops with an error where
// the compiled engine counts a fault.

// RefEngine computes the stats of one character from a Graph.
type RefEngine struct {
	g      *Graph
	in     Inputs
	mode   Mode
	env    *aocexpr.Env
	items  map[int64][]string // a stat's initializer expressions, in list order
	member map[int64]bool
	mods   map[int64]float64
	fx     map[int64][]effectApp // the active effects' mods by stat
	st     aocexpr.State
	// topTerms: terms[0] is the StatMod terms of the expression EvalExpr runs (see compile.go's fnc.topTerms)
	topTerms bool

	initMemo map[int64]float64
	baseMemo map[int64]float64
	valMemo  map[int64]float64
	active   map[string]bool
	parsed   map[string]*aocexpr.Program

	total   float64
	totalOK bool
	terms   []map[int64]string
	depth   int
}

type refState struct {
	aocexpr.State
	in *Inputs
}

// EffectCount, EffectCountByTag and SkillPurchased add the inputs' active effects and ranks to the state's, as the
// compiled engine's machine does.
func (s refState) EffectCount(r aocexpr.Ref) float64 {
	n := s.State.EffectCount(r)
	for i := range s.in.Effects {
		if s.in.Effects[i].Effect == r {
			n += float64(s.in.Effects[i].Stacks)
		}
	}
	return n
}

func (s refState) EffectCountByTag(tag string) float64 {
	n := s.State.EffectCountByTag(tag)
	for i := range s.in.Effects {
		for _, t := range s.in.Effects[i].Tags {
			if aocexpr.TagMatches(t, tag) {
				n += float64(s.in.Effects[i].Stacks)
				break
			}
		}
	}
	return n
}

// [C-Y-B ~ C.O.A]

func (s refState) SkillPurchased(class, skill aocexpr.Ref) bool {
	for _, r := range s.in.Ranks {
		if r.Class == class && r.Skill == skill {
			return true
		}
	}
	return s.State.SkillPurchased(class, skill)
}

func (s refState) EquippedItemInSlotHasTag(slot aocexpr.Enum, tag string) bool {
	for _, e := range s.in.Equipped {
		if slot.Type == "EEquipSlot" && e.Slot == slot.Value && aocexpr.TagMatches(e.Tag, tag) {
			return true
		}
	}
	return s.State.EquippedItemInSlotHasTag(slot, tag)
}

// NewRefEngine prepares a reference run of the character in over g.
func NewRefEngine(g *Graph, in Inputs, st aocexpr.State, mode Mode) (*RefEngine, error) {
	if st == nil {
		st = aocexpr.NoState{}
	}
	if in.Level < 1 || in.Level > MaxLevel {
		return nil, fmt.Errorf("statprog: level %d is not 1 to %d", in.Level, MaxLevel)
	}
	var cls *ClassDef
	for i := range g.Classes {
		if int(g.Classes[i].Enum) == in.Class {
			cls = &g.Classes[i]
		}
	}
	if cls == nil {
		return nil, fmt.Errorf("statprog: no class %d", in.Class)
	}
	if err := checkEffects(in.Effects); err != nil {
		return nil, err
	}
	e := &RefEngine{
		g: g, in: in, mode: mode, st: st, fx: map[int64][]effectApp{},
		items: map[int64][]string{}, member: map[int64]bool{}, mods: map[int64]float64{},
		initMemo: map[int64]float64{}, baseMemo: map[int64]float64{}, valMemo: map[int64]float64{},
		active: map[string]bool{}, parsed: map[string]*aocexpr.Program{},
	}
	for _, l := range append(append([]string(nil), CommonLists...), cls.List) {
		items, ok := g.Lists[l]
		if !ok {
			return nil, fmt.Errorf("statprog: no initializer list %q", l)
		}
		for _, it := range items {
			e.member[it.Stat] = true
			if strings.TrimSpace(it.Expr) != "" {
				e.items[it.Stat] = append(e.items[it.Stat], it.Expr)
			}
		}
	}
	for _, m := range in.Mods {
		if _, ok := g.Stats[m.Stat]; ok {
			e.mods[m.Stat] += m.Value
		}
	}
	for _, ef := range in.Effects {
		if ef.Stacks == 0 {
			continue
		}
		for _, md := range ef.Mods {
			if _, ok := g.Stats[md.Stat]; ok {
				e.fx[md.Stat] = append(e.fx[md.Stat], effectApp{md.Value, ef.Stacks})
			}
		}
	}
	env := aocexpr.StatEnv(refCtx{e, refState{st, &e.in}}, &aocexpr.CurveMap{TypeID: TypeStatCurve, Curves: g.Curves})
	env.Single = mode == ModeSingle
	e.env = env
	return e, nil
}

// ~ C | O | A - Cyb ~

// refCtx is the aocexpr.StatContext over the engine.
type refCtx struct {
	e *RefEngine
	refState
}

func (c refCtx) Stat(r aocexpr.Ref) (float64, error) {
	guid, err := c.e.statGuid(r)
	if err != nil {
		return 0, err
	}
	return c.e.Value(guid)
}
func (c refCtx) InitRunningTotal() (float64, error) {
	if !c.e.totalOK {
		return 0, fmt.Errorf("outside a stat initializer")
	}
	return c.e.total, nil
}
func (c refCtx) CharacterLevel() float64 { return float64(c.e.in.Level) }
func (c refCtx) Equation(r aocexpr.Ref) (float64, error) {
	if r.Kind != "" || r.TypeID != TypeStatEquation {
		return 0, fmt.Errorf("%v is not a StatEquation reference", r)
	}
	text, ok := c.e.g.Equations[r.Guid]
	if !ok {
		return 0, fmt.Errorf("no equation %d", r.Guid)
	}
	v, err := c.e.evalIn("E"+fmt.Sprint(r.Guid), text, nil)
	return c.e.clamp(v, r.Guid), err
}
func (c refCtx) Formula(r aocexpr.Ref) (float64, error) {
	if r.Kind != "" || r.TypeID != TypeStatFormula {
		return 0, fmt.Errorf("%v is not a StatFormula reference", r)
	}
	fm, ok := c.e.g.Formulas[r.Guid]
	if !ok {
		return 0, fmt.Errorf("no formula %d", r.Guid)
	}
	text, ok := c.e.g.Equations[fm.Equation]
	if !ok {
		return 0, fmt.Errorf("no equation %d", fm.Equation)
	}
	v, err := c.e.evalIn("F"+fmt.Sprint(r.Guid), text, fm.Terms)
	return c.e.clamp(v, fm.Equation), err
}
func (c refCtx) Term(r aocexpr.Ref) (float64, error) {
	e := c.e
	idx := -1
	if r.Kind == "" {
		for i := len(e.terms) - 1; i >= 0; i-- {
			if _, ok := e.terms[i][r.Guid]; ok {
				idx = i
				break
			}
		}
	}
	td, known := e.g.Terms[r.Guid]
	var v float64
	switch {
	case idx >= 0:
		text := e.terms[idx][r.Guid]
		saved := e.terms
		depth := idx
		if idx == 0 && e.topTerms {
			depth = 1 // the StatMod's terms are written against each other (compile.go: fnc.term)
		}
		e.terms = append([]map[int64]string(nil), saved[:depth]...) // a copy: the callee pushes onto it
		defer func() { e.terms = saved }()
		var err error
		if v, err = e.evalIn("T"+fmt.Sprint(r.Guid)+"@"+fmt.Sprint(depth+1), text, nil); err != nil {
			return 0, err
		}
	case known && r.Kind == "":
		if strings.TrimSpace(td.Default) != "" {
			saved := e.terms
			e.terms = nil
			defer func() { e.terms = saved }()
			var err error
			if v, err = e.evalIn("T"+fmt.Sprint(r.Guid)+"@1", td.Default, nil); err != nil {
				return 0, err
			}
		}
	default:
		if len(e.terms) == 0 {
			return 0, fmt.Errorf("GetTerm outside a formula")
		}
		return 0, fmt.Errorf("the formula has no term %v", r)
	}
	if known {
		cl := td.Clamp
		if cl.HasMin {
			v = math.Max(v, e.num(cl.Min))
		}
		if cl.HasMax {
			v = math.Min(v, e.num(cl.Max))
		}
	}
	return v, nil
}

// evalIn evaluates a called equation (or term) with its own terms in scope.
func (e *RefEngine) evalIn(key, text string, terms map[int64]string) (float64, error) {
	if e.active[key] {
		return 0, fmt.Errorf("it calls itself")
	}
	if e.depth >= maxInline {
		return 0, fmt.Errorf("calls nest more than %d deep", maxInline)
	}
	e.active[key] = true
	e.depth++
	if strings.HasPrefix(key, "T") {
		// a term is evaluated in the formula's caller's scope: e.terms was already popped
	} else {
		e.terms = append(e.terms, terms)
	}
	defer func() {
		if !strings.HasPrefix(key, "T") {
			e.terms = e.terms[:len(e.terms)-1]
		}
		e.depth--
		delete(e.active, key)
	}()
	return e.evalText(text)
}

// clamp applies an equation's constraint (raised to Min, lowered to Max).
func (e *RefEngine) clamp(v float64, equation int64) float64 {
	cl, ok := e.g.Clamps[equation]
	if !ok {
		return v
	}
	if cl.HasMin {
		v = math.Max(v, e.num(cl.Min))
	}
	if cl.HasMax {
		v = math.Min(v, e.num(cl.Max))
	}
	return v
}

// C | O | A and Cyb

func (e *RefEngine) statGuid(r aocexpr.Ref) (int64, error) {
	if r.Kind == "" {
		if r.TypeID != TypeStatDef {
			return 0, fmt.Errorf("%v is not a StatTypeDef reference", r)
		}
		return r.Guid, nil
	}
	if r.Kind != "Stat" {
		return 0, fmt.Errorf("%v is not a stat reference", r)
	}
	guid, ok := e.g.StatByName(r.Name)
	if !ok {
		return 0, fmt.Errorf("no stat (or more than one) is called %q", r.Name)
	}
	return guid, nil
}

// readsTotal says the entry's text calls GetStatInitRunningTotal (the compiled program then holds OpTotal: a static property, a call behind a
// branch counts). No equation, formula or term of the design data calls it (contentc's stat_init is the only place), so the text decides.
func (e *RefEngine) readsTotal(text string) bool {
	p, ok := e.parsed[text]
	if !ok {
		var err error
		if p, err = aocexpr.Parse(text); err != nil {
			return false
		}
		e.parsed[text] = p
	}
	t := p.Tree()
	found := false
	var walkExpr func(x aocexpr.Expr)
	var walkStmts func(list []aocexpr.Stmt)
	walkExpr = func(x aocexpr.Expr) {
		if found || x == nil {
			return
		}
		switch x := x.(type) {
		case aocexpr.Unary:
			walkExpr(x.X)
		case aocexpr.Binary:
			walkExpr(x.L)
			walkExpr(x.R)
		case aocexpr.Call:
			if strings.EqualFold(x.Name, "GetStatInitRunningTotal") {
				found = true
				return
			}
			for _, a := range x.Args {
				walkExpr(a)
			}
		}
	}
	walkStmts = func(list []aocexpr.Stmt) {
		for _, s := range list {
			if found {
				return
			}
			switch s := s.(type) {
			case aocexpr.Decl:
				walkExpr(s.X)
			case aocexpr.Set:
				walkExpr(s.X)
			case aocexpr.If:
				walkExpr(s.Cond)
				walkStmts(s.Then)
				walkStmts(s.Else)
			case aocexpr.ExprStmt:
				walkExpr(s.X)
			}
		}
	}
	walkStmts(t.Stmts)
	walkExpr(t.Result)
	return found
}

func (e *RefEngine) evalText(text string) (float64, error) {
	p, ok := e.parsed[text]
	if !ok {
		var err error
		if p, err = aocexpr.Parse(text); err != nil {
			return 0, err
		}
		e.parsed[text] = p
	}
	v, err := p.Eval(e.env)
	if err != nil {
		return 0, err
	}
	switch v := v.(type) {
	case float64:
		return v, nil
	case bool:
		if v {
			return 1, nil
		}
		return 0, nil
	}
	return 0, fmt.Errorf("the formula's value is not a number")
}

// [CoA x C Y B]

// num is an arithmetic result of the engine itself (not of a formula): float32 only in ModeSingle.
func (e *RefEngine) num(x float64) float64 {
	if e.mode == ModeSingle {
		return float64(float32(x))
	}
	return x
}

// top evaluates a stat's own formula: its result is a float32 except in ModeDouble.
func (e *RefEngine) top(text string) (float64, error) {
	v, err := e.evalText(text)
	if err == nil && e.mode != ModeDouble {
		v = float64(float32(v))
	}
	return v, err
}

// finite settles a value (a float32 except in ModeDouble) and refuses one that is not finite.
func (e *RefEngine) finite(what string, v float64) (float64, error) {
	if e.mode != ModeDouble {
		v = float64(float32(v))
	}
	if math.IsNaN(v) || math.IsInf(v, 0) {
		return 0, fmt.Errorf("%s is not finite", what)
	}
	return v, nil
}

func (e *RefEngine) stat(guid int64) (StatDef, error) {
	sd, ok := e.g.Stats[guid]
	if !ok {
		return sd, fmt.Errorf("no stat %d", guid)
	}
	return sd, nil
}

// Init is a stat's initial value.
func (e *RefEngine) Init(guid int64) (float64, error) {
	if v, ok := e.initMemo[guid]; ok {
		return v, nil
	}
	sd, err := e.stat(guid)
	if err != nil {
		return 0, err
	}
	key := fmt.Sprintf("init%d", guid)
	if e.active[key] {
		return 0, fmt.Errorf("the initial value of %s depends on itself", sd.Name)
	}
	e.active[key] = true
	defer delete(e.active, key)
	v := 0.0
	if sd.MultiplyMods {
		v = 1
	}
	hasBase := strings.TrimSpace(sd.Base) != ""
	for _, text := range e.items[guid] {
		// An entry that reads the running total sees the stat's value so far, Base included, and its result is the stat's new value
		// (engine.go, Run): the initial value stored is what the Base is added to again.
		folds := hasBase && e.readsTotal(text)
		var base float64
		if folds {
			b, err := e.top(sd.Base)
			if err != nil {
				return 0, fmt.Errorf("Base of %s: %w", sd.Name, err)
			}
			base = b
			if sd.MultiplyMods {
				v = e.num(base * v)
			} else {
				v = e.num(v + base)
			}
		}
		savedTotal, savedOK := e.total, e.totalOK
		e.total, e.totalOK = v, true
		r, err := e.top(text)
		e.total, e.totalOK = savedTotal, savedOK
		if err != nil {
			return 0, fmt.Errorf("initializer of %s: %w", sd.Name, err)
		}
		v = r
		if folds {
			if sd.MultiplyMods {
				if base == 0 {
					return 0, fmt.Errorf("initializer of %s: the Base is 0, the running total cannot be taken out", sd.Name)
				}
				v = e.num(v / base)
			} else {
				v = e.num(v - base)
			}
		}
	}
	if v, err = e.finite("the initial value of "+sd.Name, v); err != nil {
		return 0, err
	}
	e.initMemo[guid] = v
	return v, nil
}

// -- C.Y.B & CoA --

// Value is a stat's settled value.
func (e *RefEngine) Value(guid int64) (float64, error) {
	if v, ok := e.valMemo[guid]; ok {
		return v, nil
	}
	sd, err := e.stat(guid)
	if err != nil {
		return 0, err
	}
	key := fmt.Sprintf("val%d", guid)
	if e.active[key] {
		return 0, fmt.Errorf("the value of %s depends on itself", sd.Name)
	}
	e.active[key] = true
	defer delete(e.active, key)
	savedTotal, savedOK := e.total, e.totalOK
	e.totalOK = false
	defer func() { e.total, e.totalOK = savedTotal, savedOK }()

	init, err := e.Init(guid)
	if err != nil {
		return 0, err
	}
	v := init
	if strings.TrimSpace(sd.Base) != "" {
		b, err := e.top(sd.Base)
		if err != nil {
			return 0, fmt.Errorf("Base of %s: %w", sd.Name, err)
		}
		if sd.MultiplyMods {
			v = e.num(b * init)
		} else {
			v = e.num(init + b)
		}
	}
	if v, err = e.finite("the base of "+sd.Name, v); err != nil {
		return 0, err
	}
	e.baseMemo[guid] = v
	v = e.num(v + e.mods[guid])
	v = applyEffects(v, refFlags(sd), e.fx[guid], e.mode == ModeSingle)
	lo, hi := math.NaN(), math.NaN()
	if strings.TrimSpace(sd.Min) != "" {
		if lo, err = e.top(sd.Min); err != nil {
			return 0, fmt.Errorf("ValueMin of %s: %w", sd.Name, err)
		}
		if lo > v {
			v = lo
		}
	}
	if strings.TrimSpace(sd.Max) != "" {
		if hi, err = e.top(sd.Max); err != nil {
			return 0, fmt.Errorf("ValueMax of %s: %w", sd.Name, err)
		}
		if hi < v {
			v = hi
		}
	}
	if sd.OnSpawn == "Reset_SetMax" && strings.TrimSpace(sd.Max) != "" {
		v = hi
	}
	if sd.OnSpawn == "Reset_SetMin" && strings.TrimSpace(sd.Min) != "" {
		v = lo
	}
	if v, err = e.finite("the value of "+sd.Name, v); err != nil {
		return 0, err
	}
	e.valMemo[guid] = v
	return v, nil
}

// refFlags are the bundle flags of a stat the effects depend on.
func refFlags(sd StatDef) uint8 {
	var fl uint8
	if sd.MultiplyMods {
		fl |= StatMulMods
	}
	if sd.MultiplyCompliment {
		fl |= StatMulCompl
	}
	return fl
}

// refAbilityCtx is the aocexpr.AbilityContext over the engine and a host.
type refAbilityCtx struct {
	refCtx
	host ExprHost
}

func (c refAbilityCtx) AbilityProgress(g string) float64 { return c.host.AbilityProgress(g) }
func (c refAbilityCtx) EffectCountByInstigator(r aocexpr.Ref) float64 {
	return c.host.EffectCountByInstigator(r)
}
func (c refAbilityCtx) RandDouble() float64      { return c.host.RandDouble() }
func (c refAbilityCtx) HitHasTag(t string) bool  { return c.host.HitHasTag(t) }
func (c refAbilityCtx) HitHasFlag(f string) bool { return c.host.HitHasFlag(f) }
func (c refAbilityCtx) HitTargets() float64      { return c.host.HitTargets() }
func (c refAbilityCtx) XPDebtRatio() float64     { return c.host.XPDebtRatio() }
func (c refAbilityCtx) HostFn(fn int, name string, args []float64) float64 {
	return c.host.HostFn(fn, name, args)
}

// NodeRank and NodeGuid are the city nodes of the graph (aocexpr.NodeRanker).
func (c refAbilityCtx) NodeRank(guid int64, name string) (int, bool) {
	return c.e.g.NodeRank(guid, name)
}
func (c refAbilityCtx) NodeGuid(rank int) (int64, bool) { return c.e.g.NodeGuid(rank) }

// [Cyb :: C | O | A]

// EvalExpr runs the text of an ability expression (terms: a StatMod's input terms) straight over aocexpr, reading the
// character's stats as this engine computes them: what the compiled expression program must give (a float32 except in
// ModeDouble, as a program's return is).
func (e *RefEngine) EvalExpr(text string, terms map[int64]string, host ExprHost) (float64, error) {
	if host == nil {
		host = NoHost{}
	}
	ctx := refAbilityCtx{refCtx{e, refState{e.st, &e.in}}, host}
	env := aocexpr.AbilityEnv(ctx, &aocexpr.CurveMap{TypeID: TypeStatCurve, Curves: e.g.Curves})
	env.Single = e.mode == ModeSingle
	savedEnv, savedTerms := e.env, e.terms
	e.env, e.terms = env, nil
	savedTop := e.topTerms
	e.topTerms = false
	if terms != nil {
		e.terms = []map[int64]string{terms}
		e.topTerms = true
	}
	e.totalOK = false
	defer func() { e.env, e.terms, e.topTerms = savedEnv, savedTerms, savedTop }()
	return e.top(text)
}

// RefRun computes every stat of the character's lists with the reference engine.
// It is keyed by stat guid.
func RefRun(g *Graph, in Inputs, st aocexpr.State, mode Mode) (map[int64]StatOut, error) {
	e, err := NewRefEngine(g, in, st, mode)
	if err != nil {
		return nil, err
	}
	out := map[int64]StatOut{}
	for guid := range e.member {
		v, err := e.Value(guid)
		if err != nil {
			return nil, err
		}
		sd := g.Stats[guid]
		eq, err := e.finite("the equipment of "+sd.Name, e.mods[guid])
		if err != nil {
			return nil, err
		}
		o := StatOut{Guid: guid, Replication: uint8(sd.Replication), Value: v, Base: e.baseMemo[guid], Equip: eq}
		if strings.TrimSpace(sd.Regen) != "" {
			saved := e.totalOK
			e.totalOK = false
			r, err := e.top(sd.Regen)
			e.totalOK = saved
			if err != nil {
				return nil, fmt.Errorf("RegenRate of %s: %w", sd.Name, err)
			}
			if o.Regen, err = e.finite("the regeneration of "+sd.Name, r); err != nil {
				return nil, err
			}
			o.HasRegen = true
		}
		out[guid] = o
	}
	return out, nil
}
