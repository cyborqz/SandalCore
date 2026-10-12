// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package statprog

import (
	"fmt"
	"math"
	"math/rand"
	"strconv"
	"strings"
	"testing"

	"aocrop/services/internal/aocexpr"
)

// Plan D2: effect state, the expression programs and their host, against the reference engine (aocexpr over the text) and by
// hand. None of it needs a content.db: the TinyGraph covers every instruction.

func f32of(x float64) float64 { return float64(float32(x)) }

// statValue is the value of the stat n of the tiny graph in a result.
func statValue(t *testing.T, res *Result, n int) float64 {
	t.Helper()
	for _, s := range res.Stats {
		if s.Guid == tinyStat(n) {
			return s.Value
		}
	}
	t.Fatalf("stat %d is not in the result", n)
	return 0
}

// C.Y.B x C.O.A

func TestEffectFormsAreTheStatsOwn(t *testing.T) {
	b := tinyBundle(t, false, ModeFloat32)
	in := tinyInputs(2, 20, false) // a Bard: Str 80
	bare, err := b.Run(in, nil)
	if err != nil {
		t.Fatal(err)
	}
	// Speed is additive (1.0 base), Heal multiplies (1.02 at Str 80), Compl multiplies by one minus the mod (0.92).
	in.Effects = []ActiveEffect{
		{Effect: tinyEffect(7), Stacks: 2, Mods: []EffectMod{{tinyStat(19), 0.25}, {tinyStat(20), 1.05}, {tinyStat(17), 0.1}}},
		{Effect: tinyEffect(8), Stacks: 1, Mods: []EffectMod{{tinyStat(19), 0.125}, {tinyStat(20), 1.1}, {tinyStat(17), 0.5}, {424242, 7}}},
		{Effect: tinyEffect(9), Stacks: 0, Mods: []EffectMod{{tinyStat(19), 100}}},
	}
	res, err := b.Run(in, nil)
	if err != nil {
		t.Fatal(err)
	}
	if res.IgnoredEffectMods != 1 || res.Faults != 0 {
		t.Errorf("ignored effect mods %d, faults %d", res.IgnoredEffectMods, res.Faults)
	}
	speed0, heal0, compl0 := statValue(t, bare, 19), statValue(t, bare, 20), statValue(t, bare, 17)
	if speed0 != 1 || heal0 != f32of(1+80.0/4000.0) || compl0 != f32of(1-80.0/1000.0) {
		t.Fatalf("bare: Speed %v Heal %v Compl %v", speed0, heal0, compl0)
	}
	// additive: base + 0.25 * 2 stacks + 0.125 * 1 stack
	if got, want := statValue(t, res, 19), f32of(1+0.25*2+0.125); got != want {
		t.Errorf("Speed = %v, want %v", got, want)
	}
	// multiplying: the stat times the mod once per stack: * 1.05 * 1.05 * 1.1
	want := f32of(heal0 * 1.05 * 1.05 * 1.1)
	if got := statValue(t, res, 20); got != want {
		t.Errorf("Heal = %v, want %v", got, want)
	}
	// the complement: * (1 - 0.1) * (1 - 0.1) * (1 - 0.5)
	want = f32of(compl0 * (1 - 0.1) * (1 - 0.1) * (1 - 0.5))
	if got := statValue(t, res, 17); got != want {
		t.Errorf("Compl = %v, want %v", got, want)
	}
	// a stat with no effect is untouched
	for _, n := range []int{1, 2, 3, 4, 5, 8, 10, 11, 13} {
		for _, s := range bare.Stats {
			if s.Guid == tinyStat(n) && statValue(t, res, n) != s.Value {
				t.Errorf("stat %d moved with effects that do not name it", n)
			}
		}
	}
	// the stat's limits bind the result (Speed's maximum is 10, Compl's minimum 0)
	in.Effects = []ActiveEffect{{Effect: tinyEffect(7), Stacks: 4000, Mods: []EffectMod{{tinyStat(19), 1}, {tinyStat(17), 0.999}}}}
	res, _ = b.Run(in, nil)
	if statValue(t, res, 19) != 10 {
		t.Errorf("Speed past its maximum = %v", statValue(t, res, 19))
	}
	if got := statValue(t, res, 17); got < 0 || got > 1e-30 {
		t.Errorf("Compl after 4000 stacks of 0.999 = %v", got)
	}
}

// The retail values of the research: a mitigation under an effect multiplies by one minus the mod, a healing multiplier by the mod.
func TestRetailEffectValues(t *testing.T) {
	b := tinyBundle(t, false, ModeFloat32)
	in := tinyInputs(2, 20, false)
	// Mitigation 0.9652 -> 0.9362 under Bard_Glee 0.03 (capture 44): put the stat at 0.9652 with the equipment, then apply the effect.
	// Compl's base is 1 - 80/1000 = 0.92 for this Bard: the equipment adds 0.0452.
	in.Mods = []Mod{{tinyStat(17), 0.9652 - f32of(1-80.0/1000.0)}}
	before, err := b.Run(in, nil)
	if err != nil {
		t.Fatal(err)
	}
	if got := statValue(t, before, 17); math.Abs(got-0.9652) > 1e-6 {
		t.Fatalf("Compl before = %v", got)
	}
	in.Effects = []ActiveEffect{{Effect: tinyEffect(1), Stacks: 1, Mods: []EffectMod{{tinyStat(17), 0.03}}}}
	after, _ := b.Run(in, nil)
	if got := statValue(t, after, 17); math.Round(got*1e4)/1e4 != 0.9362 || math.Abs(got-0.9652*0.97) > 1e-6 {
		t.Errorf("Compl under 0.03 = %v, retail 0.9362", got)
	}
	// HealingMult 1.1025 -> 1.1576 under Bard_Note_Comedy 1.05 (x v)
	in.Mods = []Mod{{tinyStat(20), 1.1025 - f32of(1+80.0/4000.0)}}
	in.Effects = nil
	before, _ = b.Run(in, nil)
	if got := statValue(t, before, 20); math.Abs(got-1.1025) > 1e-6 {
		t.Fatalf("Heal before = %v", got)
	}
	in.Effects = []ActiveEffect{{Effect: tinyEffect(2), Stacks: 1, Mods: []EffectMod{{tinyStat(20), 1.05}}}}
	after, _ = b.Run(in, nil)
	if got := statValue(t, after, 20); math.Round(got*1e4)/1e4 != 1.1576 || math.Abs(got-1.1025*1.05) > 1e-6 {
		t.Errorf("Heal under 1.05 = %v, retail 1.1576", got)
	}
}

func TestEffectsFeedWhatReadsTheStat(t *testing.T) {
	// Str is read by other stats' formulas: an effect that adds to it moves them too, as equipment does.
	b := tinyBundle(t, false, ModeFloat32)
	in := tinyInputs(1, 20, false)
	bare, _ := b.Run(in, nil)
	in.Effects = []ActiveEffect{{Effect: tinyEffect(1), Stacks: 1, Mods: []EffectMod{{tinyStat(1), 10}}}}
	with, _ := b.Run(in, nil)
	if statValue(t, with, 1) != statValue(t, bare, 1)+10 || statValue(t, with, 2) == statValue(t, bare, 2) || statValue(t, with, 4) == statValue(t, bare, 4) {
		t.Errorf("Str %v -> %v, Dex %v -> %v", statValue(t, bare, 1), statValue(t, with, 1), statValue(t, bare, 2), statValue(t, with, 2))
	}
	// Equip is the equipment's alone; Base is before it.
	for _, s := range with.Stats {
		if s.Guid == tinyStat(1) && (s.Equip != 0 || s.Base != 150) {
			t.Errorf("Str base %v equip %v", s.Base, s.Equip)
		}
	}
}

func TestEffectInputsAreChecked(t *testing.T) {
	b := tinyBundle(t, false, ModeFloat32)
	for name, fx := range map[string][]ActiveEffect{
		"negative stacks":   {{Effect: tinyEffect(1), Stacks: -1}},
		"too many stacks":   {{Effect: tinyEffect(1), Stacks: MaxEffectStacks + 1}},
		"too many effects":  make([]ActiveEffect, MaxActiveEffects+1),
		"too many products": append(make([]ActiveEffect, 0), manyApplications()...),
	} {
		if _, err := b.Run(Inputs{Class: 1, Level: 1, Effects: fx}, nil); err == nil {
			t.Errorf("%s: the run was accepted", name)
		}
		if _, err := NewRefEngine(TinyGraph(false), Inputs{Class: 1, Level: 1, Effects: fx}, nil, ModeFloat32); err == nil {
			t.Errorf("%s: the reference accepted it", name)
		}
	}
	// the most it takes
	max := ActiveEffect{Effect: tinyEffect(1), Stacks: MaxEffectStacks, Mods: []EffectMod{{tinyStat(20), 1.0000001}}}
	if _, err := b.Run(Inputs{Class: 1, Level: 1, Effects: []ActiveEffect{max}}, nil); err != nil {
		t.Errorf("4096 stacks: %v", err)
	}
}

// manyApplications is a few effects whose stacks times mods go past the bound a run accepts.
func manyApplications() []ActiveEffect {
	var mods []EffectMod
	for i := 0; i < 300; i++ {
		mods = append(mods, EffectMod{Stat: tinyStat(20), Value: 1})
	}
	var out []ActiveEffect
	for i := 0; i < 1000; i++ {
		out = append(out, ActiveEffect{Effect: tinyEffect(1), Stacks: MaxEffectStacks, Mods: mods})
	}
	return out
}

// (Cyb + C O A)

func TestEffectsAnswerTheStateFunctions(t *testing.T) {
	b := tinyBundle(t, false, ModeFloat32)
	in := tinyInputs(1, 20, false)
	// Ctx: effects of record 1 * 2 + effects tagged pet.summon + 100 for the skill + 1000 in combat or moving + 10000 per Fishing level
	// + 100000 per consumed + the NPC rank. With no state Fishing is level 1.
	base, _ := b.Run(in, nil)
	if got := statValue(t, base, 7); got != 10000 {
		t.Fatalf("Ctx = %v", got)
	}
	in.Effects = []ActiveEffect{
		{Effect: tinyEffect(1), Stacks: 3, Tags: []string{"pet.summon.wolf"}},
		{Effect: tinyEffect(5), Stacks: 4, Tags: []string{"pet.summon", "other"}},
		{Effect: tinyEffect(6), Stacks: 5, Tags: []string{"pet.summoner", "pet"}}, // does not match: a sibling, and the parent
	}
	in.Ranks = []SkillRank{tinyRank(3)}
	res, err := b.Run(in, nil)
	if err != nil {
		t.Fatal(err)
	}
	// 3 stacks of effect 1 * 2 = 6; tagged: 3 (a descendant) + 4 = 7; the rank: 100
	if got, want := statValue(t, res, 7), 10000.0+6+7+100; got != want {
		t.Errorf("Ctx = %v, want %v", got, want)
	}
	// the state's own counts are added to the effects'
	res, err = b.Run(in, richState())
	if err != nil {
		t.Fatal(err)
	}
	// richState: 3 more of effect 1 (6 more), 2 more tagged, the same skill; combat 1000, fishing 7, consumed 11, npc 2
	if got, want := statValue(t, res, 7), float64(2*(3+3)+(7+2)+100+1000+70000+1100000+2); got != want {
		t.Errorf("Ctx with both = %v, want %v", got, want)
	}
	// a rank of another skill is nothing
	in.Ranks = []SkillRank{tinyRank(4)}
	res, _ = b.Run(in, nil)
	if got := statValue(t, res, 7); got != 10000+6+7 {
		t.Errorf("Ctx with another rank = %v", got)
	}
}

// Compiled and reference engines agree on random effects, in every mode, bit for bit.
func TestEffectsAgreeWithTheReference(t *testing.T) {
	g := TinyGraph(false)
	rng := rand.New(rand.NewSource(77))
	stats := []int64{tinyStat(1), tinyStat(2), tinyStat(3), tinyStat(4), tinyStat(5), tinyStat(7), tinyStat(17), tinyStat(19), tinyStat(20), 424242}
	vals := []float64{0, 0.03, 0.1, 0.25, 0.5, 1, 1.05, 1.5, -0.5, 7, 1e-3, 123.456}
	for _, mode := range []Mode{ModeFloat32, ModeDouble, ModeSingle} {
		b := tinyBundle(t, false, mode)
		for iter := 0; iter < 60; iter++ {
			in := tinyInputs(1+rng.Intn(2), []int{1, 5, 11, 20, 39, 41, 50}[rng.Intn(7)], rng.Intn(2) == 0)
			for i, n := 0, rng.Intn(5); i < n; i++ {
				e := ActiveEffect{Effect: tinyEffect(int64(1 + rng.Intn(9))), Stacks: rng.Intn(4)}
				if rng.Intn(2) == 0 {
					e.Tags = []string{[]string{"pet.summon", "pet", "pet.summon.x", "Debuff"}[rng.Intn(4)]}
				}
				for k, m := 0, rng.Intn(4); k < m; k++ {
					e.Mods = append(e.Mods, EffectMod{stats[rng.Intn(len(stats))], vals[rng.Intn(len(vals))]})
				}
				in.Effects = append(in.Effects, e)
			}
			if rng.Intn(2) == 0 {
				in.Ranks = []SkillRank{tinyRank(int64(3 + rng.Intn(3)))}
			}
			var st aocexpr.State
			if in.Level%2 == 1 {
				st = richState()
			}
			got, err := b.Run(in, st)
			if err != nil {
				t.Fatal(err)
			}
			want, err := RefRun(g, in, st, mode)
			if err != nil {
				t.Fatalf("%v: RefRun: %v", mode, err)
			}
			sameResults(t, fmt.Sprintf("%v iter %d (%+v)", mode, iter, in.Effects), got, want)
			if t.Failed() {
				return
			}
		}
	}
}

func TestExprsCompileAsTheyShould(t *testing.T) {
	b, res, err := TinyBundle(true, ModeFloat32)
	if err != nil {
		t.Fatal(err)
	}
	exprs := TinyExprs()
	if len(res) != len(exprs) {
		t.Fatalf("%d results for %d expressions", len(res), len(exprs))
	}
	wantErr := map[string]string{
		"an unknown function":                        `unsupported function "GetWeatherName"`,
		"a stat formula's function in an expression": "GetStatInitRunningTotal outside a stat initializer",
		"a stat the graph does not have":             "no stat 424242",
	}
	failed := 0
	for i, r := range res {
		w, bad := wantErr[exprs[i].What]
		switch {
		case bad:
			failed++
			if r.Program != -1 || !strings.Contains(r.Err, w) {
				t.Errorf("%s: program %d, error %q; want a failure with %q", exprs[i].What, r.Program, r.Err, w)
			}
		case r.Program < 0 || r.Err != "":
			t.Errorf("%s did not compile: %s", exprs[i].What, r.Err)
		case !b.IsExpr(r.Program):
			t.Errorf("%s: program %d is not in Exprs", exprs[i].What, r.Program)
		}
	}
	if failed != 3 {
		t.Errorf("%d expressions failed, want 3", failed)
	}
	// UsesTarget is what the text says
	for i, r := range res {
		if r.Program >= 0 && r.UsesTarget != strings.Contains(exprs[i].Text, "GetTarget") && !strings.Contains(exprs[i].Text, "GetTerm") {
			t.Errorf("%s: UsesTarget = %v", exprs[i].What, r.UsesTarget)
		}
	}
	// A failed expression leaves nothing behind: the bundle is what the others alone make.
	var good []Expr
	for i, e := range exprs {
		if res[i].Program >= 0 {
			good = append(good, e)
		}
	}
	b2, res2, err := CompileExprs(TinyGraph(true), Options{}, good)
	if err != nil {
		t.Fatal(err)
	}
	for _, r := range res2 {
		if r.Program < 0 {
			t.Fatalf("an expression that compiled does not compile alone: %s", r.Err)
		}
	}
	if string(b.Encode()) != string(b2.Encode()) {
		t.Error("the failed expressions changed the bundle")
	}
	// Compiling twice gives the same bytes; the bundle verifies.
	b3, _, _ := TinyBundle(true, ModeFloat32)
	if string(b.Encode()) != string(b3.Encode()) {
		t.Error("two compiles differ")
	}
	if _, err := Decode(b.Encode()); err != nil {
		t.Errorf("the bundle does not verify: %v", err)
	}
	// A stat formula cannot call what only an expression may.
	for _, fn := range []string{"GetXPDebtRatio(GetActorGuid(GetOwner()))", "GetNumTargets(GetHitInProgress())", "GetTarget()", "GetAbilityProgressRatio(\"x\")", "RandDouble()"} {
		g := TinyGraph(false)
		sd := g.Stats[tinyStat(19)]
		sd.Base = "1.0 + Float(0) * 0.0 + 0.0 * SelectFloat(true, 0.0, 0.0) + SelectFloat(" + fn + " > 0.5, 1.0, 1.0)"
		if fn == "GetTarget()" {
			sd.Base = "GetStat(GetTarget(), " + fmt.Sprintf("$#%d:%d$", TypeStatDef, tinyStat(1)) + ")"
		}
		g.Stats[tinyStat(19)] = sd
		if _, err := Compile(g, Options{}); err == nil || !strings.Contains(err.Error(), "unsupported function") {
			t.Errorf("a stat formula calling %s: %v", fn, err)
		}
	}
}

// Cyb + C-O-A

func TestExprProgramsAgreeWithTheReference(t *testing.T) {
	exprs := TinyExprs()
	g := TinyGraph(false)
	for _, mode := range []Mode{ModeFloat32, ModeDouble, ModeSingle} {
		b, res, err := TinyBundle(false, mode)
		if err != nil {
			t.Fatal(err)
		}
		for _, class := range []int{1, 2} {
			for _, level := range []int{1, 10, 20, 41, 50} {
				for hi, spec := range tinyHosts() {
					in := tinyInputs(class, level, level%2 == 0)
					in.Effects = []ActiveEffect{{Effect: tinyEffect(7), Stacks: 3, Tags: []string{"pet.summon"}, Mods: []EffectMod{{tinyStat(1), 4}, {tinyStat(19), 0.5}}}}
					if hi == 2 {
						in.Ranks = []SkillRank{tinyRank(3), tinyRank(5)}
					}
					var st aocexpr.State
					if level == 20 {
						st = richState()
					}
					run, err := b.Run(in, st)
					if err != nil {
						t.Fatal(err)
					}
					ev, err := b.NewEvaluator(run, in, st)
					if err != nil {
						t.Fatal(err)
					}
					ref, err := NewRefEngine(g, in, st, mode)
					if err != nil {
						t.Fatal(err)
					}
					gotHost, refHost := spec.Host(), spec.Host()
					for i, ex := range exprs {
						if res[i].Program < 0 {
							continue
						}
						got, err := ev.Eval(res[i].Program, gotHost)
						if err != nil {
							t.Fatalf("%s: %v", ex.What, err)
						}
						want, err := ref.EvalExpr(ex.Text, ex.Terms, refHost)
						if err != nil {
							t.Fatalf("%v %s: reference: %v", mode, ex.What, err)
						}
						if math.Float64bits(got) != math.Float64bits(want) {
							t.Errorf("%v class %d level %d host %d: %s = %v (%016x); reference %v (%016x)", mode, class, level, hi, ex.What, got, math.Float64bits(got), want, math.Float64bits(want))
						}
					}
					if ev.Faults() != 0 {
						t.Errorf("%d faults", ev.Faults())
					}
				}
			}
		}
	}
}

func TestExprValuesByHand(t *testing.T) {
	b, res, err := TinyBundle(false, ModeFloat32)
	if err != nil {
		t.Fatal(err)
	}
	exprs := TinyExprs()
	prog := func(what string) int {
		for i, e := range exprs {
			if e.What == what {
				return res[i].Program
			}
		}
		t.Fatalf("no expression %q", what)
		return -1
	}
	in := tinyInputs(2, 20, false) // Bard, Str 80
	run, _ := b.Run(in, nil)
	ev, _ := b.NewEvaluator(run, in, nil)
	eval := func(what string, h *TableHost) float64 {
		v, err := ev.Eval(prog(what), h)
		if err != nil {
			t.Fatal(err)
		}
		return v
	}
	host := &TableHost{Progress: map[string]float64{"SongGroup": 0.5, "Other": 1}, ByInstigator: map[aocexpr.Ref]float64{tinyEffect(7): 4}, Rand: SplitMix{State: 5},
		Fns: map[string]float64{strconv.Itoa(aocexpr.RoleFn(aocexpr.RoleTarget, aocexpr.RoleOpEffectInst)) + "|" + strconv.FormatInt(tinyEffect(7).Guid, 10): 4}}
	if got, want := eval("lerp of a stat of the target", host), f32of(80+(100.0-80)*0.25); got != want {
		t.Errorf("lerp = %v, want %v", got, want)
	}
	if got := eval("abs", host); got != f32of(math.Abs(80-200)+0.5) {
		t.Errorf("abs = %v", got)
	}
	if got := eval("progress", host); got != 1.25 {
		t.Errorf("Lerp(1, 1.5, 0.5) = %v", got)
	}
	if got := eval("effects by instigator", host); got != 8 {
		t.Errorf("by instigator = %v (4 * 2 + 0 effects)", got)
	}
	if got := eval("terms", host); got != f32of((0.5+1)*float64(statValue(t, run, 2))) {
		t.Errorf("terms = %v", got)
	}
	if got := eval("a constant lerp", host); got != 6 {
		t.Errorf("a constant lerp = %v", got)
	}
	// the sprint sum: 0.2 + 0.05 per rank
	for n, want := range []float64{0.2, 0.25, 0.3, 0.35} {
		var ranks []SkillRank
		for k := 0; k < n; k++ {
			ranks = append(ranks, tinyRank(int64(3+k)))
		}
		in2 := in
		in2.Ranks = ranks
		run2, _ := b.Run(in2, nil)
		ev2, _ := b.NewEvaluator(run2, in2, nil)
		got, err := ev2.Eval(prog("the sprint sum"), nil)
		if err != nil || got != f32of(want) {
			t.Errorf("sprint sum with %d ranks = %v (%v), want %v", n, got, err, want)
		}
	}
	// the random numbers: SplitMix64 from the seed, one per call, the second term times 10; and none drawn when the && is false
	h1 := &TableHost{Rand: SplitMix{State: 5}}
	var sm SplitMix
	sm.State = 5
	r1, r2 := sm.RandDouble(), sm.RandDouble()
	if got, want := eval("two random numbers", h1), f32of(r1+r2*10); got != want {
		t.Errorf("two random numbers = %v, want %v", got, want)
	}
	h2 := &TableHost{Rand: SplitMix{State: 5}}
	if got := eval("a random number behind a skill test", h2); got != 2 || h2.Rand.State != 5 {
		t.Errorf("without the skill: %v, the source advanced to %d (it must not be asked)", got, h2.Rand.State)
	}
	in3 := in
	in3.Ranks = []SkillRank{tinyRank(3)}
	run3, _ := b.Run(in3, nil)
	ev3, _ := b.NewEvaluator(run3, in3, nil)
	h3 := &TableHost{Rand: SplitMix{State: 5}}
	got, _ := ev3.Eval(prog("a random number behind a skill test"), h3)
	sm.State = 5
	if want := map[bool]float64{true: 1, false: 2}[sm.RandDouble() < 0.5]; got != want || h3.Rand.State == 5 {
		t.Errorf("with the skill: %v, want %v", got, want)
	}
	// the constrained equation: Str - 20 = 60, lowered to 15: 15 * 2 + Clamped (15 + 0.75 * 100 = 90)
	if got := eval("a constrained equation", host); got != 15*2+90 {
		t.Errorf("a constrained equation = %v", got)
	}
}

// C:Y:B with C.O.A

func TestEvaluatorRefusals(t *testing.T) {
	b, res, err := TinyBundle(false, ModeFloat32)
	if err != nil {
		t.Fatal(err)
	}
	in := tinyInputs(1, 20, false)
	run, _ := b.Run(in, nil)
	ev, err := b.NewEvaluator(run, in, nil)
	if err != nil {
		t.Fatal(err)
	}
	// a formula of the stat graph is not an expression
	if _, err := ev.Eval(int(b.Stats[1].Base), nil); err == nil {
		t.Error("a stat formula was run as an expression")
	}
	for _, p := range []int{-1, len(b.Progs), len(b.Progs) + 5} {
		if _, err := ev.Eval(p, nil); err == nil {
			t.Errorf("program %d was run", p)
		}
	}
	if _, err := b.NewEvaluator(nil, in, nil); err == nil {
		t.Error("an evaluator with no run")
	}
	if _, err := b.NewEvaluator(&Result{}, in, nil); err == nil {
		t.Error("an evaluator with no values")
	}
	bad := in
	bad.Level = 0
	if _, err := b.NewEvaluator(run, bad, nil); err == nil {
		t.Error("an evaluator at level 0")
	}
	// nil host is NoHost
	for i, r := range res {
		if r.Program < 0 {
			continue
		}
		a, err1 := ev.Eval(r.Program, nil)
		c, err2 := ev.Eval(r.Program, NoHost{})
		if err1 != nil || err2 != nil || math.Float64bits(a) != math.Float64bits(c) {
			t.Errorf("expression %d: nil host %v, NoHost %v", i, a, c)
		}
	}
}

func TestSplitMixVectors(t *testing.T) {
	// SplitMix64 from seed 0 (the reference implementation's first outputs).
	s := SplitMix{}
	for i, want := range []uint64{0xE220A8397B1DCDAF, 0x6E789E6AA1B965F4, 0x06C45D188009454F} {
		if got := s.Next(); got != want {
			t.Errorf("output %d = %016x, want %016x", i, got, want)
		}
	}
	// RandDouble is in [0, 1) and the top 53 bits over 2^53.
	s = SplitMix{State: 1}
	for i := 0; i < 100000; i++ {
		if v := s.RandDouble(); v < 0 || v >= 1 {
			t.Fatalf("RandDouble = %v", v)
		}
	}
	s = SplitMix{}
	if got, want := s.RandDouble(), float64(0xE220A8397B1DCDAF>>11)/9007199254740992.0; got != want {
		t.Errorf("first RandDouble = %v, want %v", got, want)
	}
}

// [Cyb + C.O.A]

func TestVerifierRulesOfD2(t *testing.T) {
	enc := func() *Bundle {
		b, _, err := TinyBundle(false, ModeFloat32)
		if err != nil {
			t.Fatal(err)
		}
		d, err := Decode(b.Encode())
		if err != nil {
			t.Fatal(err)
		}
		return d
	}
	progOf := func(b *Bundle, what string) int {
		_, res, _ := TinyBundle(false, ModeFloat32)
		for i, e := range TinyExprs() {
			if e.What == what {
				return res[i].Program
			}
		}
		return -1
	}
	cases := map[string]func(b *Bundle){
		"expression outside the programs":   func(b *Bundle) { b.Exprs = append(b.Exprs, uint32(len(b.Progs))) },
		"expressions not increasing":        func(b *Bundle) { b.Exprs[0], b.Exprs[1] = b.Exprs[1], b.Exprs[0] },
		"an expression twice":               func(b *Bundle) { b.Exprs[1] = b.Exprs[0] },
		"a host opcode in a base formula":   func(b *Bundle) { b.Stats[0].Base = int32(progOf(b, "two random numbers")) },
		"a host opcode in a limit":          func(b *Bundle) { b.Stats[0].Max = int32(progOf(b, "progress")) },
		"a host opcode in a regeneration":   func(b *Bundle) { b.Stats[0].Regen = int32(progOf(b, "effects by instigator")) },
		"a host opcode in an init step":     func(b *Bundle) { b.CommonSteps[0].Prog = uint32(progOf(b, "two random numbers")) },
		"a host opcode in a class step":     func(b *Bundle) { b.Classes[0].Steps[0].Prog = uint32(progOf(b, "two random numbers")) },
		"a progress string outside":         func(b *Bundle) { b.Progs[0].Code = []byte{OpProgress, 0xff, 0xff, OpRet} },
		"an instigator reference outside":   func(b *Bundle) { b.Progs[0].Code = []byte{OpEffectInst, 0xff, 0xff, OpRet} },
		"lerp with two values":              func(b *Bundle) { b.Progs[0].Code = []byte{OpConst, 0, 0, OpConst, 0, 0, OpLerp, OpRet} },
		"abs of nothing":                    func(b *Bundle) { b.Progs[0].Code = []byte{OpAbs, OpRet} },
		"too many expression programs":      func(b *Bundle) { b.Exprs = make([]uint32, MaxExprs+1) },
		"an unknown stat flag past the new": func(b *Bundle) { b.Stats[0].Flags |= 0x80 },
	}
	for name, mut := range cases {
		b := enc()
		mut(b)
		if _, err := Decode(b.Encode()); err == nil {
			t.Errorf("%s: accepted", name)
		}
	}
	// what is allowed: the complement flag, a host opcode in an expression
	b := enc()
	b.Stats[0].Flags |= StatMulCompl
	if _, err := Decode(b.Encode()); err != nil {
		t.Errorf("the complement flag: %v", err)
	}
}
