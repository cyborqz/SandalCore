// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package statprog

import (
	"database/sql"
	"fmt"
	"math"
	"sort"
	"strings"
	"testing"

	"aocrop/services/internal/aocexpr"
	"aocrop/services/internal/content"
)

// The tests of plan D2 on the real design data: the ability expressions that contentc stored (ab_expr, compiled into the stat
// program), the closures of the Bard's abilities, the retail values of the research (mana costs, the Sprint's speed, the mitigation
// and healing multipliers). They need a content.db of contentc version 6 (AOC_CONTENT_DB) and skip without one.

func openContent(t *testing.T) *sql.DB {
	t.Helper()
	db, err := content.Open(contentDBPath(t))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { db.Close() })
	return db.SQL()
}

func queryInts(t *testing.T, sq *sql.DB, q string, args ...any) []int64 {
	t.Helper()
	rows, err := sq.Query(q, args...)
	if err != nil {
		t.Fatalf("%s: %v", q, err)
	}
	defer rows.Close()
	var out []int64
	for rows.Next() {
		var v int64
		if err := rows.Scan(&v); err != nil {
			t.Fatal(err)
		}
		out = append(out, v)
	}
	return out
}

// C.Y.B - CoA

// TestStoredExpressionsAreWhatTheCompilerGives: every ab_expr row's program, target flag and error is what compiling the file's expressions
// gives, and a program a row names is an expression program of the stored bundle.
func TestStoredExpressionsAreWhatTheCompilerGives(t *testing.T) {
	b, res, exprs, ids := compileRealExprs(t)
	sq := openContent(t)
	rows, err := sq.Query(`SELECT id, program, uses_target, error FROM ab_expr ORDER BY id`)
	if err != nil {
		t.Fatal(err)
	}
	defer rows.Close()
	n := 0
	for rows.Next() {
		var id, prog, target int64
		var errText string
		if err := rows.Scan(&id, &prog, &target, &errText); err != nil {
			t.Fatal(err)
		}
		if n >= len(ids) || ids[n] != id {
			t.Fatalf("row %d is expression id %d, the compile has %v", n, id, ids[min(n, len(ids)-1)])
		}
		r := res[n]
		if int(prog) != r.Program || (target != 0) != r.UsesTarget || errText != r.Err {
			t.Errorf("expression %d: row (%d, %d, %q), compile (%d, %v, %q)", id, prog, target, errText, r.Program, r.UsesTarget, r.Err)
		}
		if prog >= 0 && !b.IsExpr(int(prog)) {
			t.Errorf("expression %d: program %d is not in Exprs", id, prog)
		}
		n++
	}
	if n != len(exprs) {
		t.Errorf("%d rows, %d expressions", n, len(exprs))
	}
	if len(b.Exprs) == 0 || len(b.Exprs) > len(exprs) {
		t.Errorf("%d expression programs for %d expressions", len(b.Exprs), len(exprs))
	}
}

// closure of an ability: the expressions of what it reaches without an NPC, as the research of 2026-10-03 listed them (cost, cooldown,
// charges, activation requirement, cast time; per hit its requirement and what it applies; per effect its duration, requirement and stat
// mods), and whether each has a program.
type abClosure struct {
	name        string
	npcFree     bool
	exprs       []int64
	failed      []int64
	effects     map[int64]bool
	hits        map[int64]bool
	usesTarget  bool
	hasMontage  bool
	costExprs   []int64
	timingExprs []int64
}

func loadClosure(t *testing.T, sq *sql.DB, ability int64, errs map[int64]string, targets map[int64]bool) abClosure {
	t.Helper()
	var c abClosure
	var tr string
	var mask, cooldown, charges, cast, req int64
	if err := sq.QueryRow(`SELECT name, target_requirement, applicable_targets, cooldown_expr, charges_expr, cast_expr, requirement_expr FROM ab_ability WHERE guid = ?`, ability).
		Scan(&c.name, &tr, &mask, &cooldown, &charges, &cast, &req); err != nil {
		t.Fatal(err)
	}
	c.npcFree = !(strings.HasSuffix(tr, "::Always") && mask&1 == 0)
	c.effects, c.hits = map[int64]bool{}, map[int64]bool{}
	add := func(ids ...int64) {
		for _, id := range ids {
			if id != 0 {
				c.exprs = append(c.exprs, id)
			}
		}
	}
	c.timingExprs = []int64{cooldown, charges, cast}
	add(cooldown, charges, cast, req)
	for _, id := range queryInts(t, sq, `SELECT enabled_expr FROM ab_cost WHERE ability_guid = ?`, ability) {
		add(id)
		c.costExprs = append(c.costExprs, id)
	}
	for _, id := range queryInts(t, sq, `SELECT amount_expr FROM ab_cost WHERE ability_guid = ?`, ability) {
		add(id)
		c.costExprs = append(c.costExprs, id)
	}
	for _, id := range queryInts(t, sq, `SELECT num_expr FROM ab_effect_cost WHERE ability_guid = ?`, ability) {
		add(id)
		c.costExprs = append(c.costExprs, id)
	}
	reachable := func(h int64) bool {
		var m int64
		if err := sq.QueryRow(`SELECT applicable_targets FROM ab_hit WHERE guid = ?`, h).Scan(&m); err != nil {
			t.Fatal(err)
		}
		return m&(1|4|8|16) != 0 || m == 0
	}
	var addEffect func(g int64)
	var addHit func(g int64)
	addHit = func(g int64) {
		if g == 0 || c.hits[g] || !reachable(g) {
			return
		}
		c.hits[g] = true
		var rq int64
		if err := sq.QueryRow(`SELECT requirement_expr FROM ab_hit WHERE guid = ?`, g).Scan(&rq); err != nil {
			t.Fatal(err)
		}
		add(rq)
		rows, err := sq.Query(`SELECT effect_guid, requirement_expr, stack_count_expr, duration_expr FROM ab_hit_effect WHERE hit_guid = ? ORDER BY kind, ord`, g)
		if err != nil {
			t.Fatal(err)
		}
		type he struct{ e, r, s, d int64 }
		var hes []he
		for rows.Next() {
			var x he
			if err := rows.Scan(&x.e, &x.r, &x.s, &x.d); err != nil {
				t.Fatal(err)
			}
			hes = append(hes, x)
		}
		rows.Close()
		for _, x := range hes {
			add(x.r, x.s, x.d)
			addEffect(x.e)
		}
		add(queryInts(t, sq, `SELECT value_expr FROM ab_mod WHERE owner_kind = 'hit' AND owner_guid = ?`, g)...)
	}
	addEffect = func(g int64) {
		if g == 0 || c.effects[g] {
			return
		}
		c.effects[g] = true
		var dur, rq int64
		if err := sq.QueryRow(`SELECT duration_expr, requirement_expr FROM ab_effect WHERE guid = ?`, g).Scan(&dur, &rq); err != nil {
			t.Fatal(err)
		}
		add(dur, rq)
		add(queryInts(t, sq, `SELECT value_expr FROM ab_mod WHERE owner_kind = 'effect' AND owner_guid = ?`, g)...)
		for _, h := range queryInts(t, sq, `SELECT hit_guid FROM ab_effect_hit WHERE effect_guid = ? ORDER BY phase, ord`, g) {
			addHit(h)
		}
	}
	for _, h := range queryInts(t, sq, `SELECT hit_guid FROM ab_ability_hit WHERE ability_guid = ? ORDER BY source, key, owner_guid, role, ord`, ability) {
		addHit(h)
	}
	for _, e := range queryInts(t, sq, `SELECT effect_guid FROM ab_ability_effect WHERE ability_guid = ? ORDER BY key`, ability) {
		addEffect(e)
	}
	seen := map[int64]bool{}
	var uniq []int64
	for _, id := range c.exprs {
		if !seen[id] {
			seen[id] = true
			uniq = append(uniq, id)
		}
	}
	c.exprs = uniq
	for _, id := range c.exprs {
		if errs[id] != "" {
			c.failed = append(c.failed, id)
		}
		if targets[id] {
			c.usesTarget = true
		}
	}
	return c
}

func exprState(t *testing.T, sq *sql.DB) (errs map[int64]string, targets map[int64]bool) {
	t.Helper()
	errs, targets = map[int64]string{}, map[int64]bool{}
	rows, err := sq.Query(`SELECT id, error, uses_target FROM ab_expr`)
	if err != nil {
		t.Fatal(err)
	}
	defer rows.Close()
	for rows.Next() {
		var id, ut int64
		var e string
		if err := rows.Scan(&id, &e, &ut); err != nil {
			t.Fatal(err)
		}
		errs[id] = e
		targets[id] = ut != 0
	}
	return
}

// TestTheBardsAbilitiesCompile reports how many of the Bard's abilities compile (every expression of the closure) and checks that all
// that can be cast without an NPC compile their costs, cooldowns, charges and cast times.
func TestTheBardsAbilitiesCompile(t *testing.T) {
	compileRealExprs(t)
	sq := openContent(t)
	errs, targets := exprState(t, sq)
	// the research's 27: the abilities of the nodes that name a skill (the talent slots' options, the songs, are stage 2's)
	abilities := queryInts(t, sq, `SELECT DISTINCT r.ability_guid FROM class c JOIN skill_node n ON n.tree_guid = c.skill_tree_guid AND n.slot_guid = 0
		JOIN skill_rank r ON r.skill_guid = n.skill_guid WHERE c.name = 'Bard' AND r.ability_guid <> 0 ORDER BY r.ability_guid`)
	if len(abilities) != 27 {
		t.Fatalf("the Bard has %d abilities, the research counted 27", len(abilities))
	}
	free, full, needTarget := 0, 0, 0
	var blocked []string
	for _, a := range abilities {
		c := loadClosure(t, sq, a, errs, targets)
		if !c.npcFree {
			continue
		}
		free++
		// the costs and timing of every ability castable without an NPC compile
		for _, id := range append(append([]int64(nil), c.costExprs...), c.timingExprs...) {
			if id != 0 && errs[id] != "" {
				t.Errorf("%s: cost or timing expression %d does not compile: %s", c.name, id, errs[id])
			}
		}
		if len(c.failed) == 0 {
			full++
			if c.usesTarget {
				needTarget++
			}
		} else {
			why := map[string]bool{}
			for _, id := range c.failed {
				if i := strings.Index(errs[id], "unsupported "); i >= 0 {
					why[strings.TrimSpace(errs[id][i+len("unsupported "):])] = true
				} else {
					why[errs[id]] = true
				}
			}
			var ws []string
			for w := range why {
				ws = append(ws, w)
			}
			sort.Strings(ws)
			blocked = append(blocked, fmt.Sprintf("%s: %s", c.name, strings.Join(ws, "; ")))
		}
	}
	if free != 22 {
		t.Errorf("%d of the Bard's abilities can be cast without an NPC, the research counted 22", free)
	}
	t.Logf("the Bard: %d abilities castable without an NPC, %d compile in full (%d of them through GetTarget as the caster)", free, full, needTarget)
	for _, b := range blocked {
		t.Logf("  not in full: %s", b)
	}
	// the research counted 7 that compile as the B3 compiler had them, 13 with GetTarget as the caster, 18 with Lerp and the progress ratio, and
	// the last four (the heals and the shield) needed the hit-event functions: with them all 22 compile
	if full != 22 {
		t.Errorf("%d of 22 compile in full, want all", full)
	}
}

// byName finds the id of a row by name in a table with a name column.
func guidByName(t *testing.T, sq *sql.DB, table, name string) int64 {
	t.Helper()
	var g int64
	if err := sq.QueryRow(`SELECT guid FROM `+table+` WHERE name = ?`, name).Scan(&g); err != nil {
		t.Fatalf("%s %s: %v", table, name, err)
	}
	return g
}

func programOf(t *testing.T, sq *sql.DB, exprID int64) int {
	t.Helper()
	var p int64
	var e string
	if err := sq.QueryRow(`SELECT program, error FROM ab_expr WHERE id = ?`, exprID).Scan(&p, &e); err != nil {
		t.Fatal(err)
	}
	if p < 0 {
		t.Fatalf("expression %d has no program: %s", exprID, e)
	}
	return int(p)
}

// (C | O | A) (Cyb)

// ranksAt are the ranks the game grants by itself at a level (every auto-granted rank of any tree whose level requirement is reached).
func ranksAt(t *testing.T, sq *sql.DB, level int) []SkillRank {
	t.Helper()
	auto, err := LoadAutoRanks(sq)
	if err != nil {
		t.Fatal(err)
	}
	return RanksAt(auto, level)
}

func statIndexByName(t *testing.T, b *Bundle, name string) int {
	t.Helper()
	for i, s := range b.Stats {
		if b.Strings[s.Name] == name {
			return i
		}
	}
	t.Fatalf("no stat %s in the bundle", name)
	return -1
}

func valueOf(t *testing.T, b *Bundle, res *Result, name string) float64 {
	t.Helper()
	return res.Values[statIndexByName(t, b, name)]
}

// TestSprintAndTheRetailBard: the retail level-25 Bard, from the evidence of the research (docs: statprog README, "D2 engine layer").
func TestSprintAndTheRetailBard(t *testing.T) {
	b, _, _, _ := compileRealExprs(t)
	sq := openContent(t)
	in := Inputs{Class: 2, Level: 25}

	// ---- the Sprint: MoveSpeedMult = 1 + 0.2 + 0.05 per Universal sprint skill held; a retail level-25 Bard sprints at 1.3 ----
	sprint := guidByName(t, sq, "ab_effect", "General_Sprint")
	mods := queryInts(t, sq, `SELECT value_expr FROM ab_mod WHERE owner_kind = 'effect' AND owner_guid = ? ORDER BY ord`, sprint)
	if len(mods) != 1 {
		t.Fatalf("General_Sprint has %d stat mods", len(mods))
	}
	prog := programOf(t, sq, mods[0])
	var statGuid int64
	if err := sq.QueryRow(`SELECT stat_guid FROM ab_mod WHERE owner_kind = 'effect' AND owner_guid = ?`, sprint).Scan(&statGuid); err != nil {
		t.Fatal(err)
	}
	moveSpeed := statIndexByName(t, b, "MoveSpeedMult")
	if b.Stats[moveSpeed].Guid != statGuid {
		t.Fatalf("the Sprint's stat %d is not MoveSpeedMult (%d)", statGuid, b.Stats[moveSpeed].Guid)
	}
	sprintSpeed := func(level int, ranks []SkillRank) (mod, speed float64) {
		in := Inputs{Class: 2, Level: level, Ranks: ranks}
		bare, err := b.Run(in, nil)
		if err != nil {
			t.Fatal(err)
		}
		ev, err := b.NewEvaluator(bare, in, nil)
		if err != nil {
			t.Fatal(err)
		}
		mod, err = ev.Eval(prog, nil)
		if err != nil {
			t.Fatal(err)
		}
		in.Effects = []ActiveEffect{{Effect: aocexpr.Ref{TypeID: content.TypeEffect, Guid: sprint}, Stacks: 1, Mods: []EffectMod{{Stat: statGuid, Value: mod}}}}
		res, err := b.Run(in, nil)
		if err != nil {
			t.Fatal(err)
		}
		return mod, valueOf(t, b, res, "MoveSpeedMult")
	}
	// what the game grants by itself: Sprint_1 at level 5, Sprint_2 at 20, Sprint_3 at 35
	for _, tc := range []struct {
		level int
		want  float64
	}{{1, 1.2}, {5, 1.25}, {20, 1.3}, {25, 1.3}, {34, 1.3}, {35, 1.35}, {50, 1.35}} {
		ranks := ranksAt(t, sq, tc.level)
		mod, speed := sprintSpeed(tc.level, ranks)
		if speed != float64(float32(tc.want)) || math.Abs(mod-(tc.want-1)) > 1e-6 {
			t.Errorf("level %d: Sprint mod %v, MoveSpeedMult %v, want %v", tc.level, mod, speed, tc.want)
		}
	}
	t.Logf("the level-25 Bard sprints at MoveSpeedMult %v (retail capture 44: 1.3); with no skill 1.2, with all three Universal sprint skills 1.35", func() float64 { _, s := sprintSpeed(25, ranksAt(t, sq, 25)); return s }())

	// ---- the mana costs: k x ManaCostMult(25) = k x 5.6875; every retail delta of the research ----
	bare, err := b.Run(in, nil)
	if err != nil {
		t.Fatal(err)
	}
	ev, err := b.NewEvaluator(bare, in, nil)
	if err != nil {
		t.Fatal(err)
	}
	for name, want := range map[string]float64{
		"Bard_Misc_QuickWit_Jest": 56.875, "Bard_Misc_Flourish": 56.875, "Bard_Misc_QuickWit_Mock": 56.875,
		"Bard_Dance_Mesmerizing": 85.3125, "Bard_Story_Joy": 85.3125,
		"Bard_Dance_Nimble": 113.75, "Bard_Dance_Maddening": 113.75, "Bard_Dance_Shielding": 113.75, "Bard_Misc_Misdirection": 113.75,
		"Bard_Saga": 199.0625,
	} {
		g := guidByName(t, sq, "ab_ability", name)
		costs := queryInts(t, sq, `SELECT amount_expr FROM ab_cost WHERE ability_guid = ? AND stat_name = 'Mana'`, g)
		if len(costs) != 1 {
			t.Errorf("%s: %d Mana costs", name, len(costs))
			continue
		}
		got, err := ev.Eval(programOf(t, sq, costs[0]), nil)
		if err != nil || got != want {
			t.Errorf("%s: Mana cost %v (%v), retail %v", name, got, err, want)
		}
	}
	if ev.Faults() != 0 {
		t.Errorf("%d faults", ev.Faults())
	}

	// ---- cooldowns and charges of the Bard's abilities as the table of the research has them ----
	for name, want := range map[string]float64{"Bard_Misc_Flourish": 20, "Bard_Misc_Fortissimo": 60, "Bard_Dance_Nimble": 25, "Bard_Saga": 90, "Bard_Story_Joy": 30, "Bard_Misc_QuickWit": 8} {
		g := guidByName(t, sq, "ab_ability", name)
		var cd int64
		if err := sq.QueryRow(`SELECT cooldown_expr FROM ab_ability WHERE guid = ?`, g).Scan(&cd); err != nil {
			t.Fatal(err)
		}
		got, err := ev.Eval(programOf(t, sq, cd), nil)
		if err != nil || got != want {
			t.Errorf("%s: cooldown %v (%v), want %v", name, got, err, want)
		}
	}
	// Flourish has 2 charges, 1 with the Dance_Energy talent; QuickWit 1, 2 with QuickWit_Jest: the expression reads the purchased list.
	flourish := guidByName(t, sq, "ab_ability", "Bard_Misc_Flourish")
	var ch int64
	if err := sq.QueryRow(`SELECT charges_expr FROM ab_ability WHERE guid = ?`, flourish).Scan(&ch); err != nil {
		t.Fatal(err)
	}
	if ch == 0 {
		t.Fatal("Flourish has no charges expression")
	}
	got, err := ev.Eval(programOf(t, sq, ch), nil)
	if err != nil {
		t.Fatal(err)
	}
	t.Logf("Flourish charges with no talent: %v", got)
	var energy int64
	if err := sq.QueryRow(`SELECT guid FROM record WHERE schema = 'SkillRecordBase' AND name = 'Bard_Passive_Dance_Energy'`).Scan(&energy); err != nil {
		t.Fatal(err)
	}
	var bardTree int64
	if err := sq.QueryRow(`SELECT skill_tree_guid FROM class WHERE name = 'Bard'`).Scan(&bardTree); err != nil {
		t.Fatal(err)
	}
	in2 := in
	in2.Ranks = []SkillRank{{Class: aocexpr.Ref{TypeID: TypeSkillTree, Guid: bardTree}, Skill: aocexpr.Ref{TypeID: TypeSkill, Guid: energy}}}
	run2, _ := b.Run(in2, nil)
	ev2, _ := b.NewEvaluator(run2, in2, nil)
	got2, err := ev2.Eval(programOf(t, sq, ch), nil)
	if err != nil {
		t.Fatal(err)
	}
	if got != 2 || got2 != 1 {
		t.Errorf("Flourish charges %v without Dance_Energy and %v with it, retail 2 and 1", got, got2)
	}

	// ---- the effect values of the retail capture 44: Glee 0.03 on Mitigation (x (1 - v)), Note_Comedy 1.05 on HealingMult (x v) ----
	for _, tc := range []struct {
		effect, stat string
		value        float64
		start, end   float64
	}{{"Bard_Glee", "Mitigation", 0.03, 0.9652, 0.9362}, {"Bard_Note_Comedy", "HealingMult", 1.05, 1.1025, 1.1576}} {
		e := guidByName(t, sq, "ab_effect", tc.effect)
		var statName string
		var stat, vexpr int64
		if err := sq.QueryRow(`SELECT stat_guid, stat_name, value_expr FROM ab_mod WHERE owner_kind = 'effect' AND owner_guid = ? AND stat_name = ?`, e, tc.stat).Scan(&stat, &statName, &vexpr); err != nil {
			t.Fatalf("%s: %v", tc.effect, err)
		}
		v, err := ev.Eval(programOf(t, sq, vexpr), nil)
		if tc.effect == "Bard_Glee" {
			// 0.02, or 0.03 with the talent Merriment, which the retail Bard of capture 44 held
			if err != nil || math.Abs(v-0.02) > 1e-6 {
				t.Errorf("Glee without Merriment: %v (%v), want 0.02", v, err)
			}
			var merry int64
			if err := sq.QueryRow(`SELECT guid FROM record WHERE schema = 'SkillRecordBase' AND name = 'Bard_Passive_Merriment'`).Scan(&merry); err != nil {
				t.Fatal(err)
			}
			inM := in
			inM.Ranks = []SkillRank{{Class: aocexpr.Ref{TypeID: TypeSkillTree, Guid: bardTree}, Skill: aocexpr.Ref{TypeID: TypeSkill, Guid: merry}}}
			runM, _ := b.Run(inM, nil)
			evM, _ := b.NewEvaluator(runM, inM, nil)
			if v, err = evM.Eval(programOf(t, sq, vexpr), nil); err != nil {
				t.Fatal(err)
			}
		}
		if err != nil || math.Abs(v-tc.value) > 1e-6 {
			t.Errorf("%s: the value of the %s mod is %v (%v), retail %v", tc.effect, tc.stat, v, err, tc.value)
		}
		// put the stat at the retail value before the effect with the equipment, then apply the effect
		idx := statIndexByName(t, b, tc.stat)
		base := bare.Values[idx]
		in3 := in
		in3.Mods = []Mod{{Stat: stat, Value: tc.start - base}}
		pre, err := b.Run(in3, nil)
		if err != nil {
			t.Fatal(err)
		}
		if got := pre.Values[idx]; math.Abs(got-tc.start) > 1e-6 {
			t.Fatalf("%s: %s before = %v, want %v", tc.effect, tc.stat, got, tc.start)
		}
		in3.Effects = []ActiveEffect{{Effect: aocexpr.Ref{TypeID: content.TypeEffect, Guid: e}, Stacks: 1, Mods: []EffectMod{{Stat: stat, Value: v}}}}
		post, err := b.Run(in3, nil)
		if err != nil {
			t.Fatal(err)
		}
		got := post.Values[idx]
		if math.Round(got*1e4)/1e4 != tc.end {
			t.Errorf("%s: %s %v -> %v, retail %v -> %v", tc.effect, tc.stat, tc.start, got, tc.start, tc.end)
		}
		if f := b.Stats[idx].Flags; f&StatMulMods == 0 || (tc.stat == "Mitigation") != (f&StatMulCompl != 0) {
			t.Errorf("%s has flags %#x", tc.stat, f)
		}
	}
}
