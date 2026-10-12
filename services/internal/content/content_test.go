// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package content

import (
	"database/sql"
	"os"
	"path/filepath"
	"reflect"
	"sort"
	"strings"
	"testing"

	"aocrop/services/internal/aocexpr"
	"aocrop/services/internal/chardef"
)

// newDB makes an empty content.db with the schema and the meta rows Open needs.
func newDB(t *testing.T, version string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "content.db")
	db, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	if err := Create(db); err != nil {
		t.Fatal(err)
	}
	exec(t, db, `INSERT INTO meta (key, value) VALUES (?, ?), (?, ?)`,
		MetaSchemaVersion, version, MetaRecordEncoding, EncodingJSON)
	return path
}

func exec(t *testing.T, db *sql.DB, query string, args ...any) {
	t.Helper()
	if _, err := db.Exec(query, args...); err != nil {
		t.Fatalf("%s: %v", query, err)
	}
}

func TestCreateMakesTheDocumentedTables(t *testing.T) {
	path := newDB(t, "1")
	db, err := Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	rows, err := db.SQL().Query(`SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name`)
	if err != nil {
		t.Fatal(err)
	}
	defer rows.Close()
	var got []string
	for rows.Next() {
		var n string
		if err := rows.Scan(&n); err != nil {
			t.Fatal(err)
		}
		got = append(got, n)
	}
	want := []string{"ab_ability", "ab_ability_effect", "ab_ability_hit", "ab_cost", "ab_effect", "ab_effect_cost", "ab_effect_hit", "ab_expr", "ab_hit",
		"ab_hit_effect", "ab_hit_projectile", "ab_hit_zone", "ab_mod", "ab_montage", "ab_projectile", "ab_projectile_hit", "ab_targeting", "ab_zone",
		"ab_zone_hit",
		"ability", "ability_cost", "ability_effect", "ability_hit", "ability_runtime", "class", "class_stat_base", "combat_config", "combat_element", "combat_profile", "relationship", "alignment_relation",
		"effect", "effect_hit", "effect_runtime", "effect_stat_runtime", "equip_slot", "hit", "hit_effect", "item", "item_equip_slot",
		"item_set", "item_tag", "meta", "owner_stat_mod", "quality_curve", "record", "record_table", "set_bonus", "stamina_regen_disable",
		"stamina_rule", "starting_equipment", "stat_block_stat", "stat_curve", "stat_curve_def", "stat_def", "stat_init", "stat_program",
		// plan D1 (contentc version 5)
		"item_use", "item_action", "action_response", "action_reward", "reward_table", "reward_item", "reward_xp", "leveling_type", "leveling_curve",
		"skill_auto_grant", "skill_rank_stat_mod",
		// plan D1, stage 2: the talents
		"skill_point_type", "leveling_point", "skill_tree", "skill_node", "skill_node_require", "skill_node_exclusive", "skill_rank", "skill_rank_cost",
		"item_weapon",
		// plan E1, stage 3, part 2 (contentc version 10): the reward tables
		"loot_table", "xp_rule",
		// the mounts (contentc version 11)
		"mount", "mount_stat", "mount_ability", "mount_effect"}
	want = append(want, NodeTables...)       // Nodes 1 (contentc version 12): the city node tables
	want = append(want, PopulationTables...) // plan E1 (contentc version 7): the NPC population chain and the vendors
	sort.Strings(want)
	if !reflect.DeepEqual(got, want) {
		t.Errorf("tables = %v\nwant     %v", got, want)
	}
}

// [C | O | A x C.Y.B]

func TestOpenChecksTheSchemaVersion(t *testing.T) {
	if _, err := Open(newDB(t, "999")); err == nil || !strings.Contains(err.Error(), `schema version "999"`) {
		t.Errorf("Open(version 999) = %v", err)
	}
	missing := filepath.Join(t.TempDir(), "missing.db")
	if _, err := Open(missing); err == nil || !strings.Contains(err.Error(), "missing.db") {
		t.Errorf("Open of a missing file = %v", err)
	}
	// ... and does not leave an empty file behind, which a second run would then
	// open as a database with no meta table.
	if _, err := os.Stat(missing); err == nil {
		t.Error("Open created the missing file")
	}
	// An empty file is not a content.db.
	empty := filepath.Join(t.TempDir(), "empty.db")
	db, _ := sql.Open("sqlite", empty)
	db.Exec(`CREATE TABLE x (y)`)
	db.Close()
	if _, err := Open(empty); err == nil {
		t.Error("Open of a database with no meta table succeeded")
	}
}

func TestOpenIsReadOnly(t *testing.T) {
	db, err := Open(newDB(t, "1"))
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	if _, err := db.SQL().Exec(`INSERT INTO meta (key, value) VALUES ('x', 'y')`); err == nil {
		t.Error("the reader wrote to the database")
	}
}

// TestRecordKeyIsUnique: a (type_id, guid) names one row of record, which is
// what Record relies on.
func TestRecordKeyIsUnique(t *testing.T) {
	raw, err := sql.Open("sqlite", newDB(t, "1"))
	if err != nil {
		t.Fatal(err)
	}
	defer raw.Close()
	exec(t, raw, `INSERT INTO record VALUES (3, 5, 'A', 10, 0, 'x', 0, '{}')`)
	exec(t, raw, `INSERT INTO record VALUES (4, 5, 'B', 11, 0, 'y', 0, '{}'), (4, 6, 'B', 10, 0, 'z', 0, '{}')`) // another Guid, another type
	if _, err := raw.Exec(`INSERT INTO record VALUES (9, 5, 'C', 10, 0, 'dup', 0, '{}')`); err == nil || !strings.Contains(err.Error(), "UNIQUE") {
		t.Errorf("a second row with type 5 and Guid 10: %v", err)
	}
}

func TestAudience(t *testing.T) {
	for _, tt := range []struct {
		rep             int
		audience, field string
	}{
		{0, "none", ""},
		{1, "Rep_Autonomous", "StatRepInt32Owner"},
		{5, "Rep_Autonomous|Rep_Proxy", "StatRepInt32OwnerProxy"},
		{7, "Rep_Autonomous|Rep_Simulated|Rep_Proxy", "StatRepInt32EveryoneProxy"},
		{6, "Rep_Simulated|Rep_Proxy", ""},
		{2, "Rep_Simulated", ""},
		{9, "Rep_Autonomous|0x8", ""},
	} {
		a, f := Audience(tt.rep)
		if a != tt.audience || f != tt.field {
			t.Errorf("Audience(%d) = %q, %q; want %q, %q", tt.rep, a, f, tt.audience, tt.field)
		}
	}
}

// C Y B / C O A

// TestArchetypeIDsMatchTheServices: the archetype ids content holds are those
// of the services' character enums (services/internal/chardef).
func TestArchetypeIDsMatchTheServices(t *testing.T) {
	if len(PlayerClasses) != 8 {
		t.Fatalf("%d player classes", len(PlayerClasses))
	}
	for _, c := range PlayerClasses {
		class, ok := chardef.ClassByName(c)
		if !ok {
			t.Fatalf("chardef has no class %s", c)
		}
		if got, want := ArchetypeID(c), int64(chardef.ArchetypeID(class)); got != want {
			t.Errorf("ArchetypeID(%s) = %d, chardef says %d", c, got, want)
		}
	}
	if ArchetypeID("Bard") != 17747 || ArchetypeID("Tank") != 17754 || ArchetypeID("Paladin") != 0 {
		t.Errorf("ArchetypeID: Bard %d, Tank %d, Paladin %d", ArchetypeID("Bard"), ArchetypeID("Tank"), ArchetypeID("Paladin"))
	}
}

func TestEncodeDecodeData(t *testing.T) {
	js := []byte(`{"Cooldown":"0","Costs":[{"Enabled":"IsMoving(GetOwner())"}]}`)
	for _, enc := range []string{EncodingJSON, EncodingGzip} {
		data, err := EncodeData(enc, js)
		if err != nil {
			t.Fatal(err)
		}
		if enc == EncodingGzip && string(data) == string(js) {
			t.Error("gzip left the data as it was")
		}
		back, err := DecodeData(enc, data)
		if err != nil || string(back) != string(js) {
			t.Errorf("%s round trip = %q, %v", enc, back, err)
		}
	}
	if _, err := EncodeData("zstd", js); err == nil {
		t.Error("unknown encoding accepted")
	}
	if _, err := DecodeData(EncodingGzip, []byte("not gzip")); err == nil {
		t.Error("garbage decoded as gzip")
	}
}

// fixture inserts a few rows by hand and checks the typed queries read them
// back.
func TestQueries(t *testing.T) {
	path := newDB(t, "1")
	raw, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatal(err)
	}
	exec(t, raw, `INSERT INTO stat_def VALUES (10, 'Stamina', 'Type_Combat', 'Type_Float', 1, 'Rep_Autonomous', 'StatRepInt32Owner',
		'', '0', 'GetStat(1)', 'GetStat(2)', 'SetMax', 'SetMax', 'SetMax', 0, 0, 0, 0, 0, 0)`)
	exec(t, raw, `INSERT INTO stat_def VALUES (11, 'Dup', 'Type_Combat', 'Type_Float', 0, 'none', NULL,
		'', '', '', '', 'None', 'None', 'None', 1, 1, 1, 1, 1, 1), (12, 'Dup', 'Type_Combat', 'Type_Float', 0, 'none', NULL,
		'', '', '', '', 'None', 'None', 'None', 0, 0, 0, 0, 0, 0)`)
	exec(t, raw, `INSERT INTO stat_init VALUES (1, 'List', 1, 10, 'Stamina', '100'), (1, 'List', 0, 11, 'Dup', '')`)
	exec(t, raw, `INSERT INTO stat_curve VALUES (5, 'C', 0, 1, 10, 'Linear', 0, 0), (5, 'C', 1, 3, 30, 'Linear', 0, 0), (5, 'C', 2, 5, 30, 'Constant', 0, 0)`)
	exec(t, raw, `INSERT INTO class VALUES (7, 'Bard', 'ECharacterClass::Bard', 17747, 1, 'Adventuring', 2, 'Tree', 0, 1, 'List'),
		(8, 'Tank', 'ECharacterClass::Tank', 17754, 1, 'Adventuring', 3, 'Tree', 1, 1, 'List')`)
	exec(t, raw, `INSERT INTO class_stat_base VALUES (7, 'Bard', 1, 10, 'Stamina', 24, 0), (7, 'Bard', 2, 10, 'Stamina', 48, 1)`)
	exec(t, raw, `INSERT INTO ability VALUES (20, 'Sprint', 'Sprint', '["a.b"]', 'req', '0', 0, 'None', 'Smart', 1, 1, 0, '["BUTTON"]', 'In', 1, 0, 1, 0, 0)`)
	exec(t, raw, `INSERT INTO ability_cost VALUES (20, 0, 0, 'IsMoving(GetOwner())', 0, '0', 10, 'Stamina', '1.25'), (20, 1, -1, '', 1, '0', 0, '', '')`)
	exec(t, raw, `INSERT INTO ability_effect VALUES (20, 1, 30, 'Eff')`)
	exec(t, raw, `INSERT INTO effect VALUES (30, 'Eff', 'Effect', 'Buff', '["t1"]', '[]', '', 1, '', 1, 1, 'Override', 50, 'Refresh', 0, 0, 0, 1)`)
	exec(t, raw, `INSERT INTO effect_hit VALUES (30, 'End', 0, 40, 'EndHit'), (30, 'PreApply', 0, 41, 'Pre'), (30, 'Tick', 1, 42, 'Tick1'), (30, 'Tick', 0, 43, 'Tick0')`)
	exec(t, raw, `INSERT INTO owner_stat_mod VALUES ('effect', 30, 0, 50, 'Mod', 10, 'Stamina', '0.5')`)
	exec(t, raw, `INSERT INTO stamina_rule VALUES ('regen.rate_enabled', '5', 'src'), ('stat_regen_time', '1', 'ini')`)
	exec(t, raw, `INSERT INTO stamina_regen_disable VALUES (1, 31, 'B', 1, 1), (0, 30, 'Eff', 0, 1)`)
	exec(t, raw, `INSERT INTO record_table VALUES (3, 5, 'StatTypeDefRecordBase', 2, 1), (4, 6, '', 9, 0)`)
	exec(t, raw, `INSERT INTO record VALUES (3, 5, 'StatTypeDefRecordBase', 10, 0, 'Stamina', 0, '{"a":1}')`)
	raw.Close()

	db, err := Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()

	s, err := db.StatDefByName("Stamina")
	if err != nil || s.Guid != 10 || s.RepField != "StatRepInt32Owner" || s.MaxExpr != "GetStat(1)" || s.PersistValue {
		t.Errorf("StatDefByName(Stamina) = %+v, %v", s, err)
	}
	if _, err := db.StatDefByName("Dup"); err == nil || !strings.Contains(err.Error(), "2 stats are called") {
		t.Errorf("StatDefByName(Dup) = %v", err)
	}
	if _, err := db.StatDefByName("None"); err == nil {
		t.Error("StatDefByName of an unknown stat succeeded")
	}
	if d, err := db.StatDef(11); err != nil || d.RepField != "" || !d.PersistValue || !d.RecalcOnCombatState {
		t.Errorf("StatDef(11) = %+v, %v", d, err)
	}
	if _, err := db.StatDef(99); err == nil {
		t.Error("StatDef(99) succeeded")
	}

	inits, err := db.StatInits("List")
	if err != nil || len(inits) != 2 || inits[0].StatName != "Dup" || inits[1].Expression != "100" {
		t.Errorf("StatInits = %+v, %v", inits, err)
	}

	curves, err := db.Curves()
	if err != nil {
		t.Fatal(err)
	}
	if v, err := aocexpr.EvalFloat("EvalCurve($#7037248826340794574:5$, 2)", &aocexpr.Env{Curves: curves}); err != nil || v != 20 {
		t.Errorf("EvalCurve over the loaded curve = %v, %v; want 20", v, err)
	}

	classes, err := db.Classes()
	if err != nil || len(classes) != 2 || classes[0].Name != "Bard" || classes[1].ArchetypeID != 17754 || !classes[1].PrefersMelee {
		t.Errorf("Classes = %+v, %v", classes, err)
	}
	base, err := db.ClassStatBase("Bard", 2)
	if err != nil || len(base) != 1 || base["Stamina"] != (StatValue{Value: 48, RoundingAmbiguous: true}) {
		t.Errorf("ClassStatBase(Bard, 2) = %v, %v", base, err)
	}
	if base, err := db.ClassStatBase("Bard", 1); err != nil || base["Stamina"] != (StatValue{Value: 24}) {
		t.Errorf("ClassStatBase(Bard, 1) = %v, %v", base, err)
	}
	if amb, err := db.RoundingAmbiguous(); err != nil || len(amb) != 1 || amb[0] != (BaseValue{"Bard", 2, "Stamina", 48}) {
		t.Errorf("RoundingAmbiguous = %+v, %v", amb, err)
	}
	if _, err := db.ClassStatBase("Bard", 3); err == nil {
		t.Error("ClassStatBase at a level with no rows succeeded")
	}

	a, err := db.AbilityByName("Sprint")
	if err != nil {
		t.Fatal(err)
	}
	if a.Guid != 20 || !reflect.DeepEqual(a.Tags, []string{"a.b"}) || !reflect.DeepEqual(a.ComboInputs, []string{"BUTTON"}) ||
		!a.OverrideHeldBehavior || a.UsableWhileSwimming || len(a.Costs) != 2 || a.Costs[0].Expression != "1.25" ||
		a.Costs[0].EnabledExpr != "IsMoving(GetOwner())" || a.Costs[1].StatOrd != -1 || !a.Costs[1].TriggersCooldown ||
		len(a.Effects) != 1 || a.Effects[0].Name != "Eff" || len(a.Hits) != 0 {
		t.Errorf("AbilityByName = %+v", a)
	}
	if b, err := db.Ability(20); err != nil || b.Name != "Sprint" {
		t.Errorf("Ability(20) = %v", err)
	}
	if _, err := db.Ability(21); err == nil {
		t.Error("Ability(21) succeeded")
	}

	e, err := db.EffectByName("Eff")
	if err != nil {
		t.Fatal(err)
	}
	if e.Guid != 30 || !e.InfiniteDuration || e.TickTimer != 1 || !e.Significant || !reflect.DeepEqual(e.Tags, []string{"t1"}) ||
		len(e.EffectTags) != 0 || len(e.StatMods) != 1 || e.StatMods[0].ValueExpr != "0.5" {
		t.Errorf("EffectByName = %+v", e)
	}
	// Hits come in phase order (PreApply before Tick before End), then position.
	var order []string
	for _, h := range e.Hits {
		order = append(order, h.Name)
	}
	if want := []string{"Pre", "Tick0", "Tick1", "EndHit"}; !reflect.DeepEqual(order, want) {
		t.Errorf("effect hits = %v, want %v", order, want)
	}

	rules, err := db.StaminaRules()
	if err != nil || len(rules) != 2 {
		t.Fatalf("StaminaRules = %v, %v", rules, err)
	}
	if v, err := rules[RuleRegenRateEnabled].Float(); err != nil || v != 5 || rules[RuleStatRegenTime].Source != "ini" {
		t.Errorf("rules = %+v", rules)
	}
	dis, err := db.StaminaRegenDisables()
	if err != nil || len(dis) != 2 || dis[0].EffectName != "Eff" || dis[0].DisablesStanding || !dis[0].DisablesMoving {
		t.Errorf("StaminaRegenDisables = %+v, %v", dis, err)
	}

	tabs, err := db.RecordTables()
	if err != nil || len(tabs) != 2 || tabs[0].Index != 3 || !tabs[0].Decoded || tabs[1].Decoded || tabs[1].Schema != "" {
		t.Errorf("RecordTables = %+v, %v", tabs, err)
	}
	rec, err := db.Record(5, 10)
	if err != nil || rec.Name != "Stamina" || rec.Schema != "StatTypeDefRecordBase" || string(rec.Fields) != `{"a":1}` {
		t.Errorf("Record = %+v, %v", rec, err)
	}
	if _, err := db.Record(5, 11); err == nil {
		t.Error("Record(5, 11) succeeded")
	}
}
