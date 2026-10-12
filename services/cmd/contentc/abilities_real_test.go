// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

// Plan D2 on the real design database (the shared build of real_test.go): the ability tables hold what the records say.

import (
	"strings"
	"testing"

	"aocrop/services/internal/content"
)

func TestRealAbilityTables(t *testing.T) {
	db := builtDB(t)
	meta, err := db.Meta()
	if err != nil {
		t.Fatal(err)
	}
	for table, want := range map[string]int64{
		// plan E1 stage 3: ability_names.tsv names the 540 NPC abilities too (ab_ability 249 -> 789 with their hits, effects, projectiles and zones),
		// and ab_expr holds the reward tables' 537 expressions besides the abilities' 1,308 (meta ab.exprs counts the abilities' alone; loot.exprs the rest)
		"ab_ability": 789, "ab_hit": 1296, "ab_projectile": 195, "ab_zone": 58, "ab_effect": 1145, "ab_expr": 1845,
	} {
		if n := scalarInt(t, db, `SELECT count(*) FROM `+table); n != want {
			t.Errorf("%s has %d rows, want %d", table, n, want)
		}
	}
	for key, want := range map[string]string{"ab.abilities": "789", "ab.hits": "1296", "ab.effects": "1145", "ab.projectiles": "195", "ab.zones": "58", "ab.exprs": "1308", "loot.exprs": "537"} {
		if meta[key] != want {
			t.Errorf("meta %s = %q, want %q", key, meta[key], want)
		}
	}

	// the 189 abilities of the eight class trees, by class (the research's Bard: 27)
	for _, c := range []struct {
		enum int
		name string
		n    int64
	}{{1, "Ranger", 26}, {2, "Bard", 27}, {3, "Cleric", 19}, {4, "Fighter", 19}, {5, "Rogue", 21}, {6, "Summoner", 37}, {7, "Tank", 21}, {8, "Mage", 19}} {
		if got := scalarInt(t, db, `SELECT count(DISTINCT r.ability_guid) FROM class c JOIN skill_node n ON n.tree_guid = c.skill_tree_guid AND n.slot_guid = 0
			JOIN skill_rank r ON r.skill_guid = n.skill_guid WHERE c.name = ? AND r.ability_guid <> 0`, c.name); got != c.n {
			t.Errorf("%s has %d abilities, want %d", c.name, got, c.n)
		}
	}
	// every ability reached is one of them, plus Sprint, Dodge and Rest: 189 + 3, and the rest of the 202 are granted by skills of other trees
	for _, g := range []int64{content.GuidSprint, content.GuidDodge, content.GuidRest} {
		if n := scalarInt(t, db, `SELECT count(*) FROM ab_ability WHERE guid = ?`, g); n != 1 {
			t.Errorf("ability %d is in %d times", g, n)
		}
	}
	// the abilities of the retail captures that no rank grants are there (ability_names.tsv: Guard, Unstuck, the consumables)
	for _, name := range []string{"General_Guard", "General_Unstuck", "Consumable_Food_FruitSalad"} {
		if n := scalarInt(t, db, `SELECT count(*) FROM ab_ability WHERE name = ?`, name); n < 1 {
			t.Errorf("ability %s (ability_names.tsv) is not in ab_ability", name)
		}
	}
	// no ability, hit, effect, projectile or zone a row names is missing from its table
	for _, q := range []string{
		`SELECT count(*) FROM skill_rank r WHERE ability_guid <> 0 AND NOT EXISTS (SELECT 1 FROM ab_ability a WHERE a.guid = r.ability_guid)`,
		`SELECT count(*) FROM skill_rank r WHERE effect_guid <> 0 AND NOT EXISTS (SELECT 1 FROM ab_effect e WHERE e.guid = r.effect_guid)`,
		`SELECT count(*) FROM ab_ability_hit x WHERE NOT EXISTS (SELECT 1 FROM ab_hit h WHERE h.guid = x.hit_guid)`,
		`SELECT count(*) FROM ab_ability_effect x WHERE NOT EXISTS (SELECT 1 FROM ab_effect e WHERE e.guid = x.effect_guid)`,
		`SELECT count(*) FROM ab_effect_hit x WHERE NOT EXISTS (SELECT 1 FROM ab_hit h WHERE h.guid = x.hit_guid)`,
		`SELECT count(*) FROM ab_hit_effect x WHERE NOT EXISTS (SELECT 1 FROM ab_effect e WHERE e.guid = x.effect_guid)`,
		`SELECT count(*) FROM ab_hit_projectile x WHERE NOT EXISTS (SELECT 1 FROM ab_projectile p WHERE p.guid = x.projectile_guid)`,
		`SELECT count(*) FROM ab_hit_zone x WHERE NOT EXISTS (SELECT 1 FROM ab_zone z WHERE z.guid = x.zone_guid)`,
		`SELECT count(*) FROM ab_projectile_hit x WHERE NOT EXISTS (SELECT 1 FROM ab_hit h WHERE h.guid = x.hit_guid)`,
		`SELECT count(*) FROM ab_zone_hit x WHERE NOT EXISTS (SELECT 1 FROM ab_hit h WHERE h.guid = x.hit_guid)`,
		`SELECT count(*) FROM ab_hit x WHERE NOT EXISTS (SELECT 1 FROM ab_ability_hit a WHERE a.hit_guid = x.guid)
			AND NOT EXISTS (SELECT 1 FROM ab_effect_hit e WHERE e.hit_guid = x.guid) AND NOT EXISTS (SELECT 1 FROM ab_projectile_hit p WHERE p.hit_guid = x.guid)
			AND NOT EXISTS (SELECT 1 FROM ab_zone_hit z WHERE z.hit_guid = x.guid)`,
		`SELECT count(*) FROM ab_cost c WHERE amount_expr <> 0 AND NOT EXISTS (SELECT 1 FROM ab_expr e WHERE e.id = c.amount_expr)`,
		`SELECT count(*) FROM ab_mod m WHERE value_expr <> 0 AND NOT EXISTS (SELECT 1 FROM ab_expr e WHERE e.id = m.value_expr)`,
	} {
		if n := scalarInt(t, db, q); n != 0 {
			t.Errorf("%d dangling or orphaned rows: %s", n, strings.Join(strings.Fields(q), " "))
		}
	}

	// no expression names a skill tree, skill, effect or ability by name outside a comment: contentc wrote the numbers (Jest's Pep hit asked for
	// $Skill:Bard_Passive_QuickWit_Jest$, which would have compiled as the null reference and never been true)
	rows, err := db.SQL().Query(`SELECT id, text FROM ab_expr WHERE text LIKE '%$Skill:%' OR text LIKE '%$SkillTree:%' OR text LIKE '%$Effect:%' OR text LIKE '%$Ability:%'`)
	if err != nil {
		t.Fatal(err)
	}
	for rows.Next() {
		var id int64
		var text string
		if err := rows.Scan(&id, &text); err != nil {
			t.Fatal(err)
		}
		live := namedRef.FindAllStringSubmatch(text, -1)
		for _, m := range live {
			if m[1] != "" {
				t.Errorf("expression %d still names %s by name: %s", id, m[0], text)
			}
		}
	}
	rows.Close()
	jestHit, err := db.SQL().Query(`SELECT e.program FROM ab_hit h JOIN ab_expr e ON e.id = h.requirement_expr WHERE h.name = 'Bard_QuickWit_Jest'`)
	if err != nil {
		t.Fatal(err)
	}
	if !jestHit.Next() {
		t.Error("Jest's Pep hit has no requirement")
	} else {
		var p int
		_ = jestHit.Scan(&p)
		if p < 0 {
			t.Error("Jest's Pep hit's requirement does not compile")
		}
	}
	jestHit.Close()

	// the Bard's Fortissimo consumes 5 stacks of Bard_Resonance; its cooldown and charges are expressions
	ab, err := db.AbilityRowByName("Bard_Misc_Fortissimo")
	if err != nil {
		t.Fatal(err)
	}
	ecs, err := db.AbilityEffectCosts(ab.Guid)
	if err != nil || len(ecs) != 1 || ecs[0].EffectGuid != 6064632468947286527 {
		t.Fatalf("Fortissimo's effect costs = %+v, %v", ecs, err)
	}
	if e, err := db.Expr(ecs[0].NumExpr); err != nil || e.Text != "5" {
		t.Errorf("Fortissimo consumes %+v, %v", e, err)
	}
	if res, err := db.EffectRow(ecs[0].EffectGuid); err != nil || res.Name != "Bard_Resonance" {
		t.Errorf("the consumed effect = %+v, %v", res, err)
	}

	// the cost of Jest is 10 x ManaCostMult, in Mana; its montage is the one the capture names
	jest, err := db.AbilityRowByName("Bard_Misc_QuickWit_Jest")
	if err != nil {
		t.Fatal(err)
	}
	costs, err := db.AbilityCosts(jest.Guid)
	if err != nil || len(costs) != 1 || costs[0].StatName != "Mana" {
		t.Fatalf("Jest's costs = %+v, %v", costs, err)
	}
	if e, err := db.Expr(costs[0].AmountExpr); err != nil || !strings.HasPrefix(strings.TrimSpace(e.Text), "10") || e.Program < 0 {
		t.Errorf("Jest's Mana = %+v, %v", e, err)
	}
	ms, err := db.AbilityMontages(jest.Guid)
	if err != nil || len(ms) == 0 || ms[0].NetGuid == nil || *ms[0].NetGuid != 16448494777319543161 || !strings.Contains(ms[0].SoftPath, "Jest") && !strings.Contains(ms[0].SoftPath, "QuickWit") {
		t.Errorf("Jest's montages = %+v, %v", ms, err)
	}

	// Nimble Dance: a lingering zone whose ticks fire hits (the zone's own tick), a self hit, and effects with the record's durations
	nimble, err := db.AbilityRowByName("Bard_Dance_Nimble")
	if err != nil {
		t.Fatal(err)
	}
	hits, err := db.AbilityHits(nimble.Guid)
	if err != nil {
		t.Fatal(err)
	}
	zone := 0
	for _, h := range hits {
		if h.Source == "zone" {
			zone++
		}
	}
	if len(hits) < 3 || zone == 0 {
		t.Errorf("Nimble fires %d hits, %d through its zone", len(hits), zone)
	}
	nz, err := db.Zone(hits[len(hits)-1].OwnerGuid)
	if hits[len(hits)-1].Source == "zone" && (err != nil || nz.LifeTime != 30 || nz.TickRate != 1.5) {
		t.Errorf("Nimble's zone = %+v, %v (30 s, a tick every 1.5 s)", nz, err)
	}
	if pep, err := db.EffectRowByName("Bard_Pep"); err != nil || pep.InfiniteDuration {
		t.Errorf("Bard_Pep = %+v, %v", pep, err)
	}

	// the auto-granted ranks: the Universal tree's Sprint, Guard and StamBoost at the levels of the research, and QuickWit
	for name, level := range map[string]int{"General_Stamina_Passive_Sprint_1": 5, "General_Stamina_Passive_Sprint_2": 20, "General_Stamina_Passive_Sprint_3": 35,
		"General_Stamina_Passive_Guard_1": 15, "General_Stamina_Passive_Guard_2": 45, "General_Stamina_Passive_StamBoost_1": 10, "General_Stamina_Passive_StamBoost_3": 50} {
		if got := scalarInt(t, db, `SELECT count(*) FROM skill_node n JOIN skill_tree t ON t.guid = n.tree_guid WHERE n.skill_name = ? AND t.name = 'Universal' AND n.auto_granted = 1 AND n.level_requirement = ?`, name, level); got != 1 {
			t.Errorf("%s: %d auto-granted nodes at level %d, want 1", name, got, level)
		}
		if eff := scalarInt(t, db, `SELECT r.effect_guid FROM skill_rank r JOIN skill_node n ON n.skill_guid = r.skill_guid WHERE n.skill_name = ? LIMIT 1`, name); eff == 0 {
			t.Errorf("%s grants no effect", name)
		}
	}
	if n := scalarInt(t, db, `SELECT count(*) FROM skill_node n JOIN skill_rank r ON r.skill_guid = n.skill_guid WHERE n.auto_granted = 1`); n != 231 {
		t.Errorf("%d auto-granted ranks, the research counted 231", n)
	}

	// the NetGUIDs of the captures that matched an ability of the tables
	if n := scalarInt(t, db, `SELECT count(*) FROM ab_montage WHERE net_guid IS NOT NULL`); n < 100 {
		t.Errorf("only %d montages carry a NetGUID", n)
	}
	// montage_ids.tsv names Sprint and Dodge abilities too only through their own tables: none of its abilities is missing from ab_ability by a typo,
	// except those that are no ability of a class tree (the caravan, fishing and economy ones)
	m, err := montageNetGuids()
	if err != nil {
		t.Fatal(err)
	}
	missing := 0
	for name := range m {
		if scalarInt(t, db, `SELECT count(*) FROM ab_ability WHERE name = ?`, name) == 0 {
			missing++
		}
	}
	if missing > 40 {
		t.Errorf("%d abilities of montage_ids.tsv are in no ability table", missing)
	}
}
