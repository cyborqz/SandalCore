// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package statprog

import (
	"fmt"
	"math"
	"testing"
)

// An equation's record may list a limit (a StatEquationType's Constraints). A read of the equation (EvalE) or of a formula that uses it (EvalFormula) gives the
// equation's own value: the limit is applied neither to the equation's result nor to the formulas that share the equation. The burn's formula is the case in
// the design data: it shares the damage equation, whose limit is "not above 0", with a term of +1 (the damage's is -1), and its pool is a positive number.

func TestEquationLimitsAreNotApplied(t *testing.T) {
	for _, mode := range []Mode{ModeFloat32, ModeDouble} {
		g := TinyGraph(false)
		stat := func(n int) string { return fmt.Sprintf("$#%d:%d$", TypeStatDef, 1000+n) }
		term := func(n int64) string { return fmt.Sprintf("$#%d:%d$", TypeTerm, n) }
		eq := func(n int) string { return fmt.Sprintf("$#%d:%d$", TypeStatEquation, n) }
		fm := func(n int) string { return fmt.Sprintf("$#%d:%d$", TypeStatFormula, n) }
		// a damage-like equation limited to "not above 0", shared by a formula whose term is -1 (the damage) and one whose term is +1 (the burn)
		g.Equations[105] = "GetTerm(" + term(310) + ") * (GetStat(GetOwner(), " + stat(1) + ") + 5.0)"
		g.Clamps[105] = Clamp{HasMax: true, Max: 0}
		g.Formulas[210] = Formula{Equation: 105, Terms: map[int64]string{310: "-1.0"}}
		g.Formulas[211] = Formula{Equation: 105, Terms: map[int64]string{310: "1.0"}}
		// an equation limited both ways whose value is under the lower limit
		g.Equations[106] = "GetStat(GetOwner(), " + stat(1) + ") - 100.0"
		g.Clamps[106] = Clamp{HasMin: true, Min: 0, HasMax: true, Max: 15}
		exprs := []Expr{
			{What: "the damage formula", Text: "EvalFormula(" + fm(210) + ")"},
			{What: "the burn formula", Text: "EvalFormula(" + fm(211) + ")"},
			{What: "an equation over its upper limit", Text: "EvalE(" + eq(103) + ")"},
			{What: "an equation under its lower limit", Text: "EvalE(" + eq(106) + ")"},
			{What: "a formula over its equation's upper limit", Text: "EvalFormula(" + fm(201) + ")"},
		}
		b, res, err := CompileExprs(g, Options{Mode: mode}, exprs)
		if err != nil {
			t.Fatalf("%v: %v", mode, err)
		}
		in := tinyInputs(2, 20, false) // Bard, Str 80
		run, err := b.Run(in, nil)
		if err != nil {
			t.Fatal(err)
		}
		str := statValue(t, run, 1)
		ev, err := b.NewEvaluator(run, in, nil)
		if err != nil {
			t.Fatal(err)
		}
		for i, tc := range []struct {
			what string
			want float64
		}{
			{"the damage formula", -(str + 5)},
			{"the burn formula", str + 5},
			{"an equation over its upper limit", str - 20},
			{"an equation under its lower limit", str - 100},
			{"a formula over its equation's upper limit", str * 0.01},
		} {
			if exprs[i].What != tc.what || res[i].Program < 0 {
				t.Fatalf("%v: expression %d is %q (%q)", mode, i, exprs[i].What, res[i].Err)
			}
			got, err := ev.Eval(res[i].Program, nil)
			if err != nil {
				t.Fatal(err)
			}
			if math.Abs(got-tc.want) > 1e-5*math.Max(1, math.Abs(tc.want)) {
				t.Errorf("%v: %s = %v, want its own value %v", mode, tc.what, got, tc.want)
			}
		}
		if ev.Faults() != 0 {
			t.Errorf("%v: %d faults", mode, ev.Faults())
		}
		// the stat that reads two limited equations gives their own values as well: (Str - 20) + (Str * 0.01) * 100
		if got, want := statValue(t, run, 18), (str-20)+(str*0.01)*100; math.Abs(got-want) > 1e-3 {
			t.Errorf("%v: the Constrained stat = %v, want %v", mode, got, want)
		}
	}
}

// The reference engine reads the same way as the compiler (the parity tests hold the two together for every stat of the graph; this holds the case by name).
func TestReferenceEngineAppliesNoEquationLimit(t *testing.T) {
	g := TinyGraph(false)
	if len(g.Clamps) == 0 {
		t.Fatal("the tiny graph lists no limits")
	}
	e, err := NewRefEngine(g, tinyInputs(2, 20, false), nil, ModeFloat32)
	if err != nil {
		t.Fatal(err)
	}
	guid, ok := g.StatByName("Constrained")
	if !ok {
		t.Fatal("no stat Constrained")
	}
	got, err := e.Value(guid)
	if err != nil {
		t.Fatal(err)
	}
	if want := (80.0 - 20) + (80.0*0.01)*100; math.Abs(got-want) > 1e-3 {
		t.Errorf("Constrained = %v, want %v", got, want)
	}
}

// The case in the design data: the burn formula (Mage_EE_Fire_Burn and the other fire burns) gives a level-25 Mage a positive pool. It shares the damage equation,
// whose record lists a limit of "not above 0"; with that limit applied the pool was 0 and no creature burned.
func TestBurnFormulaGivesAPositivePool(t *testing.T) {
	g := loadReal(t)
	const burnFormula = 6064631359490035967
	const baseTerm = 6064631035951644675
	exprs := []Expr{
		{What: "the burn pool", Text: fmt.Sprintf("EvalFormula($#%d:%d$)", TypeStatFormula, burnFormula), Terms: map[int64]string{baseTerm: "1.0"}},
	}
	b, res, err := CompileExprs(g, Options{}, exprs)
	if err != nil {
		t.Fatal(err)
	}
	if res[0].Program < 0 {
		t.Fatalf("the burn pool does not compile: %s", res[0].Err)
	}
	in := Inputs{Class: classEnum("Mage"), Level: 25}
	run, err := b.Run(in, nil)
	if err != nil {
		t.Fatal(err)
	}
	ev, err := b.NewEvaluator(run, in, nil)
	if err != nil {
		t.Fatal(err)
	}
	// a magical fire hit: Element.Magical and Element.Magical.Fire (the burn's damage reads the magical power, and the fire power when the hit is fire)
	host := &TableHost{Tags: map[string]bool{"Element.Magical": true, "Element.Magical.Fire": true}}
	pool, err := ev.Eval(res[0].Program, host)
	if err != nil {
		t.Fatal(err)
	}
	const magicalPower = 109343399106254
	mi, ok := b.StatIndex(magicalPower)
	if !ok {
		t.Fatal("the bundle has no magical power stat")
	}
	power := run.Values[mi]
	if !(pool > 0) || pool < 0.5*power || pool > 2*power {
		t.Errorf("the burn pool for a level 25 Mage with %.2f magical power = %v: want a positive number about the power", power, pool)
	}
	// a hit that is neither magical nor physical burns nothing
	none, err := ev.Eval(res[0].Program, &TableHost{})
	if err != nil {
		t.Fatal(err)
	}
	if none != 0 {
		t.Errorf("a hit with no element gives a pool of %v, want 0", none)
	}
	t.Logf("level 25 Mage: magical power %.4f, burn pool %.4f", power, pool)
}
