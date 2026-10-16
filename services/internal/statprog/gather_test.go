// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package statprog

import (
	"fmt"
	"math"
	"strings"
	"testing"

	"aocrop/services/internal/aocexpr"
)

// The gathering reads (Gathering stage 2, "Gathering data"): what they compile to, what they give with a GatherContext host, and that the compiled
// programs and the reference evaluator agree.

// gatherExprs are the gathering expressions of the tiny set by name.
func gatherExprs() []Expr {
	var out []Expr
	for _, e := range TinyExprs() {
		switch e.What {
		case "the gathering speed", "the xp of a harvest", "the quantity of a harvest", "a rating, a tier and the quality in one sum":
			out = append(out, e)
		}
	}
	return out
}

func tinyStatRef(n int) string { return fmt.Sprintf("$#%d:%d$", TypeStatDef, tinyStat(n)) }

func TestGatherReadsCompileOrRefuse(t *testing.T) {
	for name, tc := range map[string]struct {
		text, err string // "" compiles
	}{
		"a rating":                       {"GetArtisanshipModifiedStat(GetInstigator(), " + tinyStatRef(1) + ") > 0.0", ""},
		"a rating of the owner":          {"GetArtisanshipModifiedStat(GetOwner(), " + tinyStatRef(2) + ") > 0.0", ""},
		"a rating of the target":         {"GetArtisanshipModifiedStat(GetTarget(), " + tinyStatRef(2) + ") > 0.0", ""},
		"a rating divided":               {"GetArtisanshipModifiedStat(GetInstigator(), " + tinyStatRef(1) + ") / 4.0 > 1.0", ""},
		"a duration divided":             {"GetGatheringInteractionBaseDuration() / 2.0 > 1.0", ""},
		"the quality, rarity and tier":   {"GetRolledItemQuality() == 6 && GetItemRarity() >= 2 && GetSourceCertificationTier() != 1", ""},
		"a rating of a number":           {"GetArtisanshipModifiedStat(3, " + tinyStatRef(1) + ") > 0.0", "not an actor"},
		"a rating of a stat that is not": {"GetArtisanshipModifiedStat(GetInstigator(), $#4547065559311335368:1$) > 0.0", "is not a stat"},
		"a rating with one argument":     {"GetArtisanshipModifiedStat(GetInstigator()) > 0.0", "takes 2 argument"},
		"a rating of the stat's name":    {"GetArtisanshipModifiedStat(GetInstigator(), \"Str\") > 0.0", "not a stat"},
		"the quality with an argument":   {"GetRolledItemQuality(1) > 0", "takes 0 argument"},
		"the rarity with an argument":    {"GetItemRarity(GetOwner()) > 0", "takes 0 argument"},
		"a whole number divided":         {"GetItemRarity() / 2 > 0", divisionConstruct},
	} {
		_, r := compileOne(t, ModeFloat32, "SelectFloat("+tc.text+", 1.0, 2.0)")
		if tc.err == "" {
			if r.Program < 0 {
				t.Errorf("%s: %q did not compile: %s", name, tc.text, r.Err)
			}
			continue
		}
		if r.Program != -1 || !strings.Contains(r.Err, tc.err) {
			t.Errorf("%s: %q gave program %d, error %q; want an error with %q", name, tc.text, r.Program, r.Err, tc.err)
		}
	}
	// a stat formula has no host: it cannot ask the harvest anything
	for _, fn := range []string{"GetItemRarity()", "GetRolledItemQuality()", "GetSourceCertificationTier()", "GetGatheringInteractionBaseDuration()",
		"GetArtisanshipModifiedStat(GetOwner(), " + tinyStatRef(1) + ")"} {
		g := TinyGraph(false)
		sd := g.Stats[tinyStat(19)]
		sd.Base = "1.0 + SelectFloat(" + fn + " > 0.5, 1.0, 1.0)"
		g.Stats[tinyStat(19)] = sd
		if _, err := Compile(g, Options{}); err == nil || !strings.Contains(err.Error(), "a stat formula has no host") {
			t.Errorf("a stat formula calling %s: %v", fn, err)
		}
	}
}

// == C O A | Cyb ==

// TestGatherReadsGive runs the gathering expressions of the tiny set against GatherContext hosts and checks the numbers by hand.
func TestGatherReadsGive(t *testing.T) {
	exprs := gatherExprs()
	b, res, err := CompileExprs(TinyGraph(false), Options{}, exprs)
	if err != nil {
		t.Fatal(err)
	}
	in := tinyInputs(2, 20, false)
	run, _ := b.Run(in, nil)
	ev, _ := b.NewEvaluator(run, in, nil)
	stat1, stat2, stat3 := tinyStat(1), tinyStat(2), tinyStat(3)
	// quantity: RandIntRange(1, MaxQuant) the way the reward roller's host answers it: lo + floor(rand * (hi - lo + 1))
	quantity := func(seed uint64, max float64) float64 {
		r := SplitMix{State: seed}
		return 1 + math.Floor(r.RandDouble()*(max-1+1))
	}
	// the tiny curve 11 holds (1, 10), (10, 55 constant), (20, 100), (50, 400): rating 15 sits on the constant step, 30 on the way to 400 (200)
	for _, tc := range []struct {
		what string
		ctx  GatherContext
		seed uint64
		want float64
	}{
		{"the gathering speed", GatherContext{BaseDuration: 15, Ratings: map[int64]float64{stat1: 7, stat2: 8}}, 1, 15 * 55},
		{"the gathering speed", GatherContext{BaseDuration: 11.25, Ratings: map[int64]float64{stat1: 30}}, 1, 11.25 * 200},
		{"the gathering speed", GatherContext{BaseDuration: 7.5}, 1, 7.5 * 10},
		// BaseXP: 80 for a Novice recipe, 90 Apprentice, 100 Journeyman and above; x1 Common, x1.5 Uncommon, x3.5 Legendary and above (the data's equation: 2.0 Rare is
		// not in this set's cut-down copy, which has the same ifs)
		{"the xp of a harvest", GatherContext{SourceTier: 0, ItemRarity: 0}, 1, 80},
		{"the xp of a harvest", GatherContext{SourceTier: 1, ItemRarity: 1}, 1, 135},
		{"the xp of a harvest", GatherContext{SourceTier: 2, ItemRarity: 6}, 1, 350},
		{"the xp of a harvest", GatherContext{SourceTier: 3, ItemRarity: 2}, 1, 80},
		// the quantity: a quality (read as a rarity band here) of 0 rolls 1..7, 3 rolls 1..4, 6 rolls 1..1, anything else rolls RandIntRange(1, 0) = 1
		{"the quantity of a harvest", GatherContext{Quality: 0}, 5, quantity(5, 7)},
		{"the quantity of a harvest", GatherContext{Quality: 3}, 6, quantity(6, 4)},
		{"the quantity of a harvest", GatherContext{Quality: 6}, 7, 1},
		{"the quantity of a harvest", GatherContext{Quality: 31}, 8, 1},
		// 0.5 x the rating of stat 3 + 100 x the tier + the quality + 1000 x the rarity
		{"a rating, a tier and the quality in one sum", GatherContext{Quality: 31, ItemRarity: 3, SourceTier: 2, Ratings: map[int64]float64{stat3: 40}}, 1, 20 + 200 + 31 + 3000},
		{"a rating, a tier and the quality in one sum", GatherContext{Ratings: map[int64]float64{stat1: 99}}, 1, 0},
	} {
		idx := -1
		for i, ex := range exprs {
			if ex.What == tc.what {
				idx = i
			}
		}
		ctx := tc.ctx
		host := &TableHost{Gather: &ctx, Rand: SplitMix{State: tc.seed}}
		got, err := ev.Eval(res[idx].Program, host)
		if err != nil {
			t.Fatalf("%s: %v", tc.what, err)
		}
		if got != float64(float32(tc.want)) {
			t.Errorf("%s with %+v = %v, want %v", tc.what, tc.ctx, got, tc.want)
		}
	}
	if ev.Faults() != 0 {
		t.Errorf("%d faults", ev.Faults())
	}
}

// TestGatherReadsAgreeWithTheReference: the compiled programs and the reference evaluator (the text over aocexpr) give the same numbers for the
// harvests, in every arithmetic.
func TestGatherReadsAgreeWithTheReference(t *testing.T) {
	g := TinyGraph(false)
	exprs := append(gatherExprs(),
		Expr{What: "a duration over a rating", Text: "GetGatheringInteractionBaseDuration() / (1.0 + GetArtisanshipModifiedStat(GetInstigator(), " + tinyStatRef(1) + ") * 0.01)"},
		Expr{What: "the rating of the target", Text: "GetArtisanshipModifiedStat(GetTarget(), " + tinyStatRef(1) + ") + 3.0"})
	ctxs := []GatherContext{
		{},
		{Quality: 1, ItemRarity: 0, SourceTier: 0, BaseDuration: 15, Ratings: map[int64]float64{tinyStat(1): 0}},
		{Quality: 6, ItemRarity: 1, SourceTier: 1, BaseDuration: 11.25, Ratings: map[int64]float64{tinyStat(1): 215, tinyStat(2): 8409, tinyStat(3): 3}},
		{Quality: 3, ItemRarity: 3, SourceTier: 2, BaseDuration: 7.5, Ratings: map[int64]float64{tinyStat(1): 316.5, tinyStat(3): 0.25}},
		{Quality: 6, ItemRarity: 6, SourceTier: 4, BaseDuration: 14.8, Ratings: map[int64]float64{tinyStat(1): 1e6}},
	}
	for _, mode := range []Mode{ModeFloat32, ModeDouble, ModeSingle} {
		b, res, err := CompileExprs(g, Options{Mode: mode}, exprs)
		if err != nil {
			t.Fatal(err)
		}
		in := tinyInputs(1, 11, false)
		run, err := b.Run(in, nil)
		if err != nil {
			t.Fatal(err)
		}
		ev, err := b.NewEvaluator(run, in, nil)
		if err != nil {
			t.Fatal(err)
		}
		ref, err := NewRefEngine(g, in, nil, mode)
		if err != nil {
			t.Fatal(err)
		}
		for ci, c := range ctxs {
			for i, ex := range exprs {
				if res[i].Program < 0 {
					t.Fatalf("%s did not compile: %s", ex.What, res[i].Err)
				}
				c1, c2 := c, c
				got, err := ev.Eval(res[i].Program, &TableHost{Gather: &c1, Rand: SplitMix{State: uint64(ci + 1)}})
				if err != nil {
					t.Fatalf("%s: %v", ex.What, err)
				}
				want, err := ref.EvalExpr(ex.Text, ex.Terms, &TableHost{Gather: &c2, Rand: SplitMix{State: uint64(ci + 1)}})
				if err != nil {
					t.Fatalf("%v context %d %s: reference: %v", mode, ci, ex.What, err)
				}
				if math.Float64bits(got) != math.Float64bits(want) {
					t.Errorf("%v context %d: %s = %v, reference %v", mode, ci, ex.What, got, want)
				}
			}
		}
	}
}

// ~ C_Y_B - CoA ~

func TestGatherContextAnswers(t *testing.T) {
	c := GatherContext{Quality: 31, ItemRarity: 3, SourceTier: 2, BaseDuration: 11.25, Ratings: map[int64]float64{1001: 215, 1002: 8409}}
	for _, tc := range []struct {
		fn   int
		name string
		args []float64
		want float64
	}{
		{aocexpr.HostArtisanStat, "1001", []float64{float64(aocexpr.RoleCaster)}, 215},
		{aocexpr.HostArtisanStat, "1002", []float64{float64(aocexpr.RoleCaster)}, 8409},
		{aocexpr.HostArtisanStat, "1003", []float64{float64(aocexpr.RoleCaster)}, 0}, // a stat the host has no rating for
		{aocexpr.HostArtisanStat, "1001", []float64{float64(aocexpr.RoleTarget)}, 0}, // only the instigator has ratings
		{aocexpr.HostArtisanStat, "not a guid", []float64{0}, 0},
		{aocexpr.HostArtisanStat, "1001", nil, 215}, // no role given: the caster
		{aocexpr.HostRolledItemQuality, "", nil, 31},
		{aocexpr.HostItemRarity, "", nil, 3},
		{aocexpr.HostSourceCertTier, "", nil, 2},
		{aocexpr.HostGatherBaseSecs, "", nil, 11.25},
	} {
		if got, ok := c.HostFn(tc.fn, tc.name, tc.args); !ok || got != tc.want {
			t.Errorf("HostFn(%d, %q, %v) = %v, %v; want %v", tc.fn, tc.name, tc.args, got, ok, tc.want)
		}
	}
	var empty GatherContext
	if v, ok := empty.HostFn(aocexpr.HostArtisanStat, "1001", []float64{0}); !ok || v != 0 {
		t.Errorf("a context with no ratings: %v, %v", v, ok)
	}
	for _, fn := range []int{aocexpr.HostNow, aocexpr.HostNodeBiome, aocexpr.HostGatherLast + 1, aocexpr.HostGatherFirst - 1} {
		if _, ok := c.HostFn(fn, "", nil); ok {
			t.Errorf("function %d is no gathering read", fn)
		}
	}
	// the TableHost sends the gathering range to its context, and without one adds nothing to the Fns it is given
	h := &TableHost{Gather: &c}
	if v := h.HostFn(aocexpr.HostArtisanStat, "1002", []float64{0}); v != 8409 {
		t.Errorf("TableHost with a context = %v", v)
	}
	h = &TableHost{Fns: map[string]float64{"180|1001": 7, "182|": 2}}
	if v := h.HostFn(aocexpr.HostArtisanStat, "1001", []float64{0}); v != 7 {
		t.Errorf("TableHost with Fns = %v, want 7 (the role is not added)", v)
	}
	if v := h.HostFn(aocexpr.HostItemRarity, "", nil); v != 2 {
		t.Errorf("TableHost with Fns = %v, want 2", v)
	}
}

func TestGatherHostCasesRoundTrip(t *testing.T) {
	for _, c := range gatherHostCases() {
		if err := c.check(); err != nil {
			t.Error(err)
		}
	}
	// the corpus text of a block reads back as the block
	text := "gatherhost x\nquality " + f64hex(31) + "\nrarity 3\ntier 2\nduration " + f64hex(11.25) + "\nrating 1001 " + f64hex(215.5) + "\nrating 1002 " + f64hex(0) +
		"\nq 180 1001 0 = " + f64hex(215.5) + "\nq 180 424242 0 = " + f64hex(0) + "\nq 180 1001 1 = " + f64hex(0) + "\nq 181 - 0 = " + f64hex(31) + "\nq 182 - 0 = " + f64hex(3) +
		"\nq 183 - 0 = " + f64hex(2) + "\nq 184 - 0 = " + f64hex(11.25) + "\nend\n"
	c, err := ParseCorpus(strings.NewReader(text))
	if err != nil || len(c.GatherHosts) != 1 {
		t.Fatalf("ParseCorpus = %v, %v", c, err)
	}
	h := c.GatherHosts[0]
	if h.Ctx.Quality != 31 || h.Ctx.ItemRarity != 3 || h.Ctx.SourceTier != 2 || h.Ctx.BaseDuration != 11.25 || h.Ctx.Ratings[1001] != 215.5 || len(h.Ctx.Ratings) != 2 || len(h.Queries) != 7 ||
		h.Queries[2].Role != 1 || h.Queries[3].Name != "" || h.Queries[0].Name != "1001" {
		t.Errorf("block = %+v", h)
	}
	if err := h.check(); err != nil {
		t.Error(err)
	}
	if c.Replay() != nil {
		t.Error("the block did not replay")
	}
	bad := strings.Replace(text, "q 182 - 0 = "+f64hex(3), "q 182 - 0 = "+f64hex(4), 1)
	c, _ = ParseCorpus(strings.NewReader(bad))
	if c == nil || c.Replay() == nil {
		t.Error("a wrong answer replayed")
	}
	for _, line := range []string{"rating 1 0000000000000000\n", "gatherhost\n", "gatherhost x\nq 180 1 0\nend\n", "gatherhost x\nq 999 - 0 = 0000000000000000\nend\n",
		"gatherhost x\nrarity x\nend\n", "gatherhost x\nrating 1 zz\nend\n", "gatherhost x\nquality\nend\n", "gatherhost x\n", "gatherhost x\nq 180 1 0 = 12\nend\n"} {
		if _, err := ParseCorpus(strings.NewReader(line)); err == nil {
			t.Errorf("%q was accepted", line)
		}
	}
}

// C | O | A x C:Y:B

// TestProfessionLevelOfThePlayer: the GM artisan scrolls' experience expressions ask GetPlayerProfessionLevel(GetPlayer(), "Mining") where the stat formulas ask
// GetPlayerProfessionLevel(GetActorGuid(GetInstigator()), "Fishing"): both read the profession level of the state, compiled and by the reference evaluator.
func TestProfessionLevelOfThePlayer(t *testing.T) {
	g := TinyGraph(false)
	exprs := []Expr{
		{What: "the actor form", Text: "GetPlayerProfessionLevel(GetActorGuid(GetInstigator()), \"Fishing\") * 10.0"},
		{What: "the player form", Text: "GetPlayerProfessionLevel(GetPlayer(), \"Fishing\") * 10.0"},
		{What: "the scroll's shape", Text: "const xp = EvalCurve($#" + fmt.Sprint(TypeStatCurve) + ":11$, 15.0);\nconst cur = EvalCurve($#" + fmt.Sprint(TypeStatCurve) + ":11$, (1.0 * GetPlayerProfessionLevel(GetPlayer(), \"Fishing\")));\nxp - cur + 1"},
	}
	for _, mode := range []Mode{ModeFloat32, ModeDouble, ModeSingle} {
		b, res, err := CompileExprs(g, Options{Mode: mode}, exprs)
		if err != nil {
			t.Fatal(err)
		}
		for i, r := range res {
			if r.Program < 0 {
				t.Fatalf("%s did not compile: %s", exprs[i].What, r.Err)
			}
		}
		in := tinyInputs(2, 20, false)
		for _, st := range []aocexpr.State{nil, richState()} { // no profession: level 1; rich: Fishing 7
			run, err := b.Run(in, st)
			if err != nil {
				t.Fatal(err)
			}
			ev, _ := b.NewEvaluator(run, in, st)
			ref, err := NewRefEngine(g, in, st, mode)
			if err != nil {
				t.Fatal(err)
			}
			level := 1.0
			if st != nil {
				level = 7
			}
			for i, ex := range exprs {
				got, err := ev.Eval(res[i].Program, &TableHost{})
				if err != nil {
					t.Fatal(err)
				}
				want, err := ref.EvalExpr(ex.Text, nil, &TableHost{})
				if err != nil {
					t.Fatalf("%v %s: reference: %v", mode, ex.What, err)
				}
				if math.Float64bits(got) != math.Float64bits(want) {
					t.Errorf("%v %s = %v, reference %v", mode, ex.What, got, want)
				}
				if i < 2 && got != level*10 {
					t.Errorf("%v %s = %v, want %v", mode, ex.What, got, level*10)
				}
			}
		}
	}
	// a number or an actor object where the profession level wants the player or an actor id is still refused
	_, r := compileOne(t, ModeFloat32, "GetPlayerProfessionLevel(3.0, \"Fishing\")")
	if r.Program != -1 {
		t.Errorf("GetPlayerProfessionLevel(3.0 ...) compiled")
	}
}
