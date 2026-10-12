// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package aocexpr

import (
	"errors"
	"math"
	"strings"
	"testing"
)

// fakeCtx is a StatContext over a few numbers, with NoState's values for the
// state functions.
type fakeCtx struct {
	NoState
	stats map[string]float64
	total float64
	level float64
	eqs   map[int64]float64
}

func (f *fakeCtx) Stat(r Ref) (float64, error) {
	if r.Kind == "Stat" {
		if v, ok := f.stats[r.Name]; ok {
			return v, nil
		}
		return 0, errors.New("no stat " + r.Name)
	}
	return 0, errors.New("unresolved " + r.String())
}
func (f *fakeCtx) InitRunningTotal() (float64, error) { return f.total, nil }
func (f *fakeCtx) Equation(r Ref) (float64, error)    { return f.eqs[r.Guid], nil }
func (f *fakeCtx) Formula(Ref) (float64, error)       { return 0, errors.New("no formula") }
func (f *fakeCtx) Term(Ref) (float64, error)          { return 0, errors.New("no term") }
func (f *fakeCtx) CharacterLevel() float64            { return f.level }

func statEval(t *testing.T, src string, ctx StatContext) (any, error) {
	t.Helper()
	p, err := Parse(src)
	if err != nil {
		t.Fatalf("Parse(%q): %v", src, err)
	}
	return p.Eval(StatEnv(ctx, nil))
}

// built by C | Y | B with C-O-A

func TestStringEnumAndNamedRefSyntax(t *testing.T) {
	for _, tt := range []struct {
		src  string
		want any
	}{
		{`"abc" == "abc"`, true},
		{`'abc' == "abd"`, false},
		{`"a\"b" == 'a"b'`, true},
		{`"a\\b" == 'a\\b'`, true},
	} {
		got, err := mustParse(t, tt.src).Eval(&Env{})
		if err != nil || got != tt.want {
			t.Errorf("%s = %v, %v; want %v", tt.src, got, err, tt.want)
		}
	}
	got, err := mustParse(t, `EEquipSlot::Secondary == EEquipSlot::Secondary && EEquipSlot::Secondary != EEquipSlot::Primary`).Eval(&Env{})
	if err != nil || got != true {
		t.Errorf("enum equality = %v, %v", got, err)
	}
	// A multi-part enum keeps the leading parts in its type.
	tr := mustParse(t, `Foo(A::B::C)`).Tree()
	if c, ok := tr.Result.(Call); !ok || len(c.Args) != 1 || c.Args[0].(EnumLit).V != (Enum{Type: "A::B", Value: "C"}) {
		t.Errorf("A::B::C parsed as %#v", tr.Result)
	}
	// A named reference is a Ref with a Kind.
	tr = mustParse(t, `Foo($Stat:MaxStamina$)`).Tree()
	if r := tr.Result.(Call).Args[0].(RefLit).Ref; r != (Ref{Kind: "Stat", Name: "MaxStamina"}) || r.String() != "$Stat:MaxStamina$" {
		t.Errorf("named ref = %#v (%v)", r, r)
	}
	// Errors name what is wrong.
	for _, tt := range []struct{ src, want string }{
		{`"abc`, "unterminated string literal"},
		{"\"abc\ndef\"", "unterminated string literal"},
		{`"a\nb"`, "string escape"},
		{`$Stat$`, "is not $Kind:Name$"},
		{`$Stat:$`, "is not $Kind:Name$"},
		{`$$`, "empty $ reference"},
		{`1 + "a"`, "needs a number"},
	} {
		_, err := EvalFloat(tt.src, &Env{})
		if err == nil || !strings.Contains(err.Error(), tt.want) {
			t.Errorf("%s: error %v, want one with %q", tt.src, err, tt.want)
		}
	}
	// Refs() still lists the references (numeric and named).
	refs := mustParse(t, `GetStat(GetOwner(), $#5:6$) + GetStat(GetOwner(), $Stat:X$)`).Refs()
	if len(refs) != 2 || refs[0] != (Ref{TypeID: 5, Guid: 6}) || refs[1].Name != "X" {
		t.Errorf("Refs = %v", refs)
	}
}

func TestFloorCeil(t *testing.T) {
	for _, tt := range []struct {
		src  string
		want float64
	}{
		{"Floor(2.7)", 2}, {"Floor(-2.2)", -3}, {"Ceil(2.1)", 3}, {"Ceil(-2.7)", -2}, {"Floor(3)", 3},
	} {
		v, err := EvalFloat(tt.src, &Env{})
		if err != nil || v != tt.want {
			t.Errorf("%s = %v, %v; want %v", tt.src, v, err, tt.want)
		}
	}
	// Floor's result is an integer in the engine: dividing it is not proven.
	if _, err := EvalFloat("Floor(2.5) / 2", &Env{}); err == nil {
		t.Error("Floor(2.5) / 2 was evaluated")
	}
}

func TestFloatFuncs(t *testing.T) {
	ctx := &fakeCtx{stats: map[string]float64{"A": 3}, level: 5}
	env := StatEnv(ctx, nil)
	// GetStat is a float: a division by it is proven, and a stat can be divided.
	for _, tt := range []struct {
		src  string
		want float64
	}{
		{`GetStat(GetOwner(), $Stat:A$) / 2`, 1.5},
		{`1 / GetStat(GetOwner(), $Stat:A$)`, 1.0 / 3},
		{`const r = GetStat(GetOwner(), $Stat:A$); r / (r + 1)`, 0.75},
		{`GetStatInitRunningTotal() / 4`, 0.5},
	} {
		ctx.total = 2
		v, err := EvalFloat(tt.src, env)
		if err != nil || v != tt.want {
			t.Errorf("%s = %v, %v; want %v", tt.src, v, err, tt.want)
		}
	}
	// A level is not: GetCharacterLevel(...) / 2 stays unsupported.
	_, err := EvalFloat(`GetCharacterLevel(GetOwner()) / 2`, env)
	if c, ok := Unsupported(err); !ok || c != divisionConstruct {
		t.Errorf("level / 2: %v", err)
	}
	// FloatFuncs is the environment's: CharacterEnv has none.
	if _, err := EvalFloat(`7 / 2`, CharacterEnv(1, nil)); err == nil {
		t.Error("7 / 2 was evaluated")
	}
	// A host cannot make a built-in a float function by naming it.
	e := &Env{FloatFuncs: map[string]bool{"Floor": true}}
	if _, err := EvalFloat("Floor(2.5) / 2", e); err == nil {
		t.Error("FloatFuncs reached a built-in")
	}
}

func TestStatEnvFunctions(t *testing.T) {
	ctx := &fakeCtx{stats: map[string]float64{"Con": 30, "Power": 12}, total: 7, level: 20, eqs: map[int64]float64{9: 1.5}}
	for _, tt := range []struct {
		src  string
		want any
	}{
		{`GetStat(GetInstigator(), $Stat:Con$) * EvalE($#1:9$)`, 45.0},
		{`GetStatInitRunningTotal() + 1`, 8.0},
		{`GetCharacterLevel(GetSelf())`, 20.0},
		{`GetEffectCount(GetOwner(), $#1:2$)`, 0.0},
		{`GetEffectCountByTag(GetOwner(), "pet.summon")`, 0.0},
		{`GetSkillPurchased(GetOwner(), $#1:2$, $#1:3$)`, false},
		{`IsInCombat(GetSource())`, false},
		{`IsMoving(GetSource())`, false},
		{`IsUnkillable(GetInstigator())`, false},
		{`EquippedItemInSlotHasTag(EEquipSlot::Secondary, "Item.Gear.Weapon.Shield")`, false},
		{`GetPlayerProfessionLevel(GetActorGuid(GetInstigator()), "Fishing")`, 1.0},
		{`GetConsumedItemStat($#1:2$)`, 0.0},
		{`GetNpcEliteRank(GetSelf())`, 0.0},
		{`10 * GetPlayerProfessionLevel(GetActorGuid(GetInstigator()), "Fishing")`, 10.0},
	} {
		got, err := statEval(t, tt.src, ctx)
		if err != nil || got != tt.want {
			t.Errorf("%s = %v, %v; want %v", tt.src, got, err, tt.want)
		}
	}
	// Wrong argument counts and types are errors that name the function.
	for _, src := range []string{
		`GetStat($#1:2$)`, `GetStat(GetOwner(), 5)`, `GetStatInitRunningTotal(1)`, `EvalE(GetOwner())`,
		`GetEffectCountByTag(GetOwner(), 5)`, `EquippedItemInSlotHasTag("Secondary", "x")`, `IsMoving()`,
		`GetPlayerProfessionLevel(GetOwner(), "Fishing")`, `GetOwner(1)`, `GetCharacterLevel(5)`,
	} {
		if _, err := statEval(t, src, ctx); err == nil {
			t.Errorf("%s did not fail", src)
		}
	}
	// A host error carries the function's name.
	_, err := statEval(t, `GetStat(GetOwner(), $Stat:Nope$)`, ctx)
	if err == nil || !strings.Contains(err.Error(), "GetStat: no stat Nope") {
		t.Errorf("GetStat error = %v", err)
	}
	// Single mode applies to the stat environment too.
	env := StatEnv(ctx, nil)
	env.Single = true
	v, err := EvalFloat(`GetStat(GetOwner(), $Stat:Con$) * 0.1`, env)
	if err != nil || v != float64(float32(30*float64(float32(0.1)))) {
		t.Errorf("single = %v, %v", v, err)
	}
	if got := len(StatFuncNames()); got != len(StatEnv(ctx, nil).Funcs) {
		t.Errorf("StatFuncNames has %d names, StatEnv %d functions", got, len(StatEnv(ctx, nil).Funcs))
	}
	for _, n := range StatFuncNames() {
		if StatEnv(ctx, nil).Funcs[n] == nil {
			t.Errorf("StatFuncNames lists %s, StatEnv has no such function", n)
		}
	}
}

// ~ Cyb & C-O-A ~

func TestNoStateValues(t *testing.T) {
	var s State = NoState{}
	if s.EffectCount(Ref{}) != 0 || s.EffectCountByTag("x") != 0 || s.SkillPurchased(Ref{}, Ref{}) || s.InCombat() || s.Moving() ||
		s.Unkillable() || s.EquippedItemInSlotHasTag(Enum{}, "x") || s.ActorGuid() != 0 || s.ProfessionLevel(0, "Fishing") != 1 ||
		s.ConsumedItemStat(Ref{}) != 0 || s.NpcEliteRank() != 0 {
		t.Error("NoState has a value other than the documented ones")
	}
}

func TestTagMatches(t *testing.T) {
	for _, tt := range []struct {
		tag, want string
		ok        bool
	}{
		{"Item.Gear.Weapon.Shield", "Item.Gear.Weapon.Shield", true},
		{"Item.Gear.Weapon.Shield.Tower", "Item.Gear.Weapon.Shield", true},
		{"Item.Gear.Weapon.ShieldBash", "Item.Gear.Weapon.Shield", false},
		{"Item.Gear.Weapon", "Item.Gear.Weapon.Shield", false},
	} {
		if TagMatches(tt.tag, tt.want) != tt.ok {
			t.Errorf("TagMatches(%q, %q) != %v", tt.tag, tt.want, tt.ok)
		}
	}
}

func TestTree(t *testing.T) {
	tr := mustParse(t, `const a = 1.5; var b = a; if (b > 1) { set b = 2; } else { set b = 3; } b * 2 // done`).Tree()
	if len(tr.Stmts) != 3 {
		t.Fatalf("%d statements", len(tr.Stmts))
	}
	d := tr.Stmts[0].(Decl)
	if !d.Const || d.Name != "a" || d.X.(Num).V != 1.5 || !d.X.(Num).Float {
		t.Errorf("decl a = %#v", d)
	}
	if v := tr.Stmts[1].(Decl); v.Const || v.Name != "b" {
		t.Errorf("decl b = %#v", v)
	}
	i := tr.Stmts[2].(If)
	if len(i.Then) != 1 || len(i.Else) != 1 || i.Cond.(Binary).Op != ">" {
		t.Errorf("if = %#v", i)
	}
	if r := tr.Result.(Binary); r.Op != "*" || r.L.(Var).Name != "b" || r.R.(Num).V != 2 || r.R.(Num).Float {
		t.Errorf("result = %#v", r)
	}
	if tr.Result.Pos().Line != 1 || tr.Stmts[0].Pos().Col != 1 {
		t.Errorf("positions: %v %v", tr.Result.Pos(), tr.Stmts[0].Pos())
	}
	// An expression statement keeps its expression.
	tr = mustParse(t, `Min(1, 2); 3`).Tree()
	if _, ok := tr.Stmts[0].(ExprStmt); !ok || tr.Stmts[0].Pos() != tr.Stmts[0].(ExprStmt).X.Pos() {
		t.Errorf("stmts = %#v", tr.Stmts)
	}
	// Builtins lists exactly the names the evaluator has.
	for _, n := range Builtins() {
		if builtins[n] == nil {
			t.Errorf("Builtins lists %s", n)
		}
	}
	if len(Builtins()) != len(builtins) {
		t.Errorf("Builtins has %d names, the evaluator %d", len(Builtins()), len(builtins))
	}
	_ = math.Pi
}

func TestNestingIsBounded(t *testing.T) {
	deep := func(open, body, close string, n int) string {
		return strings.Repeat(open, n) + body + strings.Repeat(close, n)
	}
	for name, src := range map[string]string{
		"parentheses":  deep("(", "1", ")", 100000),
		"unary":        strings.Repeat("-", 100000) + "1",
		"not":          strings.Repeat("!", 100000) + "true",
		"calls":        deep("Min(1, ", "1", ")", 100000),
		"else if":      "var x = 1; " + strings.Repeat("if (true) {} else ", 100000) + "{} x",
		"nested ifs":   deep("if (true) { ", "", "}", 100000) + " 1",
		"right of ops": "1 " + strings.Repeat("+ (1 ", 100000) + strings.Repeat(")", 100000),
	} {
		_, err := Parse(src)
		if err == nil || !strings.Contains(err.Error(), "levels deep") {
			t.Errorf("%s: %v", name, err)
		}
	}
	// A text that nests a reasonable amount is fine, and a long flat one too.
	if v, err := EvalFloat(deep("(", "1", ")", maxNesting/2-2), &Env{}); err != nil || v != 1 {
		t.Errorf("a modest nesting: %v, %v", v, err)
	}
	if v, err := EvalFloat(strings.Repeat("1 + ", 20000)+"1", &Env{}); err != nil || v != 20001 {
		t.Errorf("a long sum: %v, %v", v, err)
	}
}
