// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package designdb

import (
	"encoding/json"
	"math"
	"os"
	"strings"
	"sync"
	"testing"
	"time"

	"aocrop/services/internal/usmap"
)

// These tests read the game's design database and mappings file, which stay
// outside the repo: AOC_DESIGN_DB is the client's CacheDB.dbc (the only .dbc in
// the game's Generated\Client folder since 2026-10-01; until then a third-party
// 0_AoCEvolve.dbc sat beside it and was the file the client loaded, with the same
// rows for everything checked here, see the package documentation) and AOC_USMAP
// a usmap: the old 5.6.0-434126 AOC_FIXED one or the one tools/ghidra/usmapgen.py
// writes from the client (dist/content/aoc.usmap). Both decode every table; the
// patches that get them there differ, and the tests know which file they have
// from AbilityHitRecordBase.bMayCorrupt, which only the generated one lists.
// They skip when either variable is unset or missing.

const (
	guidStamina    = 6064629575339934476
	guidMaxStamina = 6064631747701440948
	guidSprint     = 6064631641648332884 // DefaultAoCRecordConstants.ini
	guidDodge      = 107258680465016

	typeStatCurve = 7037248826340794574
)

// Cyb and C | O | A

var fixture struct {
	once      sync.Once
	db        *DB
	m         *usmap.Map
	d         *Decoder
	cl        []Classification
	generated bool // the usmap is the one generated from the client (it has bMayCorrupt)
	err       error
	took      struct{ open, classify time.Duration }
}

// hasProp reports whether struct sc of m lists a property called name.
func hasProp(m *usmap.Map, sc, name string) bool {
	s := m.Schema(sc)
	if s == nil {
		return false
	}
	for _, p := range s.Props {
		if p.Name == name {
			return true
		}
	}
	return false
}

// realFixture opens the real files once for all the tests below.
func realFixture(t *testing.T) (*DB, *usmap.Map, *Decoder, []Classification) {
	t.Helper()
	dbPath, mapPath := os.Getenv("AOC_DESIGN_DB"), os.Getenv("AOC_USMAP")
	if dbPath == "" || mapPath == "" {
		t.Skip("AOC_DESIGN_DB and AOC_USMAP not set")
	}
	for _, p := range []string{dbPath, mapPath} {
		if _, err := os.Stat(p); err != nil {
			t.Skipf("%v", err)
		}
	}
	fixture.once.Do(func() {
		t0 := time.Now()
		if fixture.db, fixture.err = Open(dbPath); fixture.err != nil {
			return
		}
		if fixture.m, fixture.err = usmap.Load(mapPath); fixture.err != nil {
			return
		}
		fixture.took.open = time.Since(t0)
		fixture.generated = hasProp(fixture.m, "AbilityHitRecordBase", "bMayCorrupt")
		fixture.d = fixture.db.Decoder(fixture.m)
		t0 = time.Now()
		fixture.cl = fixture.d.Classify()
		fixture.took.classify = time.Since(t0)
	})
	if fixture.err != nil {
		t.Fatal(fixture.err)
	}
	return fixture.db, fixture.m, fixture.d, fixture.cl
}

func TestRealOpenAndVerify(t *testing.T) {
	db, _, _, _ := realFixture(t)
	h := db.Header()
	if h.Tables != 369 {
		t.Errorf("tables = %d, want 369", h.Tables)
	}
	// CacheDB.dbc's generation time (the 0_AoCEvolve.dbc that sat beside it until
	// 2026-10-01 carried the same).
	if got := h.GeneratedAt().Format(time.RFC3339); got != "2026-01-20T01:53:09Z" {
		t.Errorf("generated %s", got)
	}
	t0 := time.Now()
	if err := db.Verify(); err != nil {
		t.Errorf("Verify: %v", err)
	}
	t.Logf("open+usmap %v, verify %v, %d MB", fixture.took.open, time.Since(t0), db.Size()>>20)
	// The tables that matter to the first slice, by TypeId.
	for id, want := range map[uint64]int{
		1841048597116038327: 0, // Ability
		3672506661632452365: 2, // AbilityHit
		5365104778484440215: 3, // StatTypeDef
		3377240744094973029: 4, // Effect
		2937500229480787749: 303,
		typeStatCurve:       296,
	} {
		if ti, ok := db.TableByTypeID(id); !ok || ti.Index != want {
			t.Errorf("TableByTypeID(%d) = %+v, %v; want table %d", id, ti, ok, want)
		}
	}
}

// TestRealClassify: the survey matched 243 of the 369 tables to a record
// struct, the prototype with the AbilityHit patch and the map rule 309 (211
// with one struct), the walker with FText string-table entries and object
// references 335 (238 with one, 301 settled), and the walker with the facts of
// the client build (the leaf record struct, the properties not stored, the row
// alignment, the inherited row's tail, InstancedStruct, Optional) and the
// TypeIds the client registers all 369, with either usmap: 268 by a single fit
// and 101 by their TypeId (the rows' names decide nothing any more).
func TestRealClassify(t *testing.T) {
	db, _, _, cl := realFixture(t)
	if len(cl) != len(db.Tables()) {
		t.Fatalf("classified %d of %d tables", len(cl), len(db.Tables()))
	}
	matched, unique, settled := 0, 0, 0
	byBasis := map[string]int{}
	for _, c := range cl {
		if len(c.Candidates) > 0 {
			matched++
		}
		if len(c.Candidates) == 1 {
			unique++
		}
		if c.Decodes {
			settled++
			byBasis[c.Basis]++
			if !contains(c.Candidates, c.Schema) {
				t.Errorf("table %d: Decodes but %s is not among %v", c.Table, c.Schema, c.Candidates)
			}
		}
		// The TypeId names every table, and the walk agrees with the name.
		if want := exeTypes[c.TypeID]; want == "" || c.Schema != want {
			t.Errorf("table %d (TypeId %d): schema %q, the executable says %q", c.Table, c.TypeID, c.Schema, want)
		}
	}
	t.Logf("classify %v: %d of %d tables self-check with some struct, %d with exactly one, %d settled to one struct (%v)",
		fixture.took.classify, matched, len(cl), unique, settled, byBasis)
	if matched != 369 || unique != 268 || settled != 369 {
		t.Errorf("%d tables self-check, %d fit one struct, %d are settled; want 369, 268, 369", matched, unique, settled)
	}
	if byBasis[BasisNameStem] != 0 || byBasis[BasisSingleFit] != 268 || byBasis[BasisTypeID] != 101 {
		t.Errorf("settled by %v; want 268 by a single fit and 101 by a TypeId", byBasis)
	}

	for _, w := range []struct {
		table  int
		schema string
		rows   uint32
		end    uint64 // 0: not checked
	}{
		{0, "AoCAbilityRecordBase", 2130, 0x1e0f35},
		{2, "AbilityHitRecordBase", 3608, 0x3c8c0f},
		{3, "StatTypeDefRecordBase", 732, 0},
		{4, "EffectRecordBase", 2794, 0},
		{44, "CharacterClassesRecordBase", 16, 0},
		{296, "StatCurveRecordBase", 122, 0},
		{303, "StatInitializerListRecordBase", 145, 0},
		// Tables the earlier walker could not read: the leaf struct's own
		// properties (Toast, CertificationLevel), a 16-byte row alignment
		// (CraftingStationVisualDef, PathingRoute, StoryArcPhase), the inherited
		// row's tail (Item, ContentSocket), InstancedStruct (AIUseAbilityCondition),
		// Optional (GlobalKillVolumeList, NarrativeObjectiveV2), and the 30 record
		// structs not named XxxRecordBase (DialogueNode, NarrativeQuestV2).
		{330, "ToastRecordBase", 77, 0},
		{40, "CertificationLevelRecordBase", 5, 0},
		{77, "CraftingStationVisualDefRecordBase", 888, 0},
		{220, "PathingRouteRecordBase", 1346, 0},
		{311, "StoryArcPhaseRecordBase", 128, 0},
		{161, "ItemRecordBase", 10568, 0},
		{367, "ContentSocketRecord", 6464, 0},
		{12, "AIUseAbilityConditionRecord", 19, 0},
		{266, "GlobalKillVolumeList", 1, 0},
		{366, "NarrativeObjectiveV2", 10762, 0},
		{201, "DialogueNode", 5317, 0},
		{365, "NarrativeQuestV2", 3026, 0},
		{228, "PopulationAreaRecordBase", 608, 0},
		{233, "PopulationInteractVolumeRecordBase", 62, 0},
		{359, "WindowsRecordBase", 91, 0},
	} {
		c := cl[w.table]
		if c.Schema != w.schema || !c.Decodes || c.Rows != w.rows {
			t.Errorf("table %d = %+v; want %s, %d rows, decoded", w.table, c, w.schema, w.rows)
		}
		if ti, _ := db.Table(w.table); w.end != 0 && ti.End() != w.end {
			t.Errorf("table %d ends at %#x, want %#x", w.table, ti.End(), w.end)
		}
	}
}

// ~ C | Y | B :: C | O | A ~

// TestRealOneTablePerSchema: no record struct is two tables' schema. Two tables
// that a struct merely fits used to be settled as that struct: table 126 (24
// FishingSizeData rows) as StatLevelGains and table 157 (two interaction
// states) as GroupConstants, beside the real StatLevelGains (304) and
// GroupConstants (145). The TypeIds the executable registers name them
// (FishingSizeData and InteractionStateDef are record structs not named
// XxxRecordBase), and settle the tables several structs fit (StartingStats,
// StartingStatsClass, ...) that their rows' names used to.
func TestRealOneTablePerSchema(t *testing.T) {
	_, _, _, cl := realFixture(t)
	owner := map[string]int{}
	for _, c := range cl {
		if c.Schema == "" {
			continue
		}
		if prev, dup := owner[c.Schema]; dup {
			t.Errorf("tables %d and %d are both %s", prev, c.Table, c.Schema)
		}
		owner[c.Schema] = c.Table
	}
	for _, w := range []struct {
		table  int
		schema string
		basis  string
	}{
		{126, "FishingSizeData", BasisTypeID},
		{157, "InteractionStateDef", BasisTypeID},
		{304, "StatLevelGainsRecordBase", BasisTypeID},
		{145, "GroupConstantsRecordBase", BasisSingleFit},
		{290, "StartingStatsRecordBase", BasisTypeID},
		{291, "StartingStatsClassRecordBase", BasisTypeID},
		{292, "StartingStatsRaceRecordBase", BasisTypeID},
		{305, "StatLevelGainsClassRecordBase", BasisTypeID},
		{306, "StatLevelGainsRaceRecordBase", BasisTypeID},
	} {
		if c := cl[w.table]; c.Schema != w.schema || c.Basis != w.basis || !c.Decodes {
			t.Errorf("table %d = %+v; want %q (%s)", w.table, c, w.schema, w.basis)
		}
	}
	// Both structs still walk the 24 fishing rows; the TypeId tells them apart.
	if got := cl[126].Candidates; len(got) != 2 || got[0] != "FishingSizeData" || got[1] != "StatLevelGainsRecordBase" {
		t.Errorf("table 126 candidates = %v", got)
	}
}

// TestRealRules checks, on the tables themselves, the rules read off the client
// build: the leaf record struct's own properties start a row, the four
// 16-byte-aligned record structs' rows start on 16-byte boundaries, rows with
// a parent carry the 4-byte tail, and the InstancedStruct and Optional values
// decode to their structs.
func TestRealRules(t *testing.T) {
	_, _, d, _ := realFixture(t)
	toasts, err := d.Decode(330, "ToastRecordBase")
	if err != nil {
		t.Fatal(err)
	}
	if _, ok := toasts[0].Fields["bPersistAfterCountdown"].(bool); !ok || toasts[0].Fields["ToastWidgetClass"] == "" {
		t.Errorf("Toast row 0 = %v", toasts[0].Fields)
	}
	certs, err := d.Decode(40, "CertificationLevelRecordBase")
	if err != nil {
		t.Fatal(err)
	}
	if _, ok := certs[1].Fields["Label"].(Text); !ok || certs[1].Fields["MinLevel"] != int64(10) || certs[1].Fields["MaxLevel"] != int64(20) {
		t.Errorf("CertificationLevel row 1 = %v", certs[1].Fields)
	}
	for _, w := range []struct {
		table  int
		schema string
	}{{77, "CraftingStationVisualDefRecordBase"}, {220, "PathingRouteRecordBase"}, {311, "StoryArcPhaseRecordBase"}, {315, "SurveyPylonVisualDefRecordBase"}} {
		rows, err := d.Decode(w.table, w.schema)
		if err != nil {
			t.Fatal(err)
		}
		for _, r := range rows {
			if r.Offset%16 != 0 {
				t.Errorf("table %d: a row starts at %#x, not on a 16-byte boundary", w.table, r.Offset)
				break
			}
		}
	}
	items, err := d.Decode(161, "ItemRecordBase")
	if err != nil {
		t.Fatal(err)
	}
	inherited, byName := 0, map[string]Row{}
	for _, r := range items {
		if r.ParentGuid != 0 {
			inherited++
		}
		byName[r.Name] = r
	}
	// 359 items inherit (the crates); Crate_M_General follows Crate_S_General,
	// whose tail is the first one that pushed the next row by 8 bytes: the row
	// ends (tail included) at 0x1954523 and the next starts at 0x1954528.
	s, m := byName["Crate_S_General"], byName["Crate_M_General"]
	if inherited != 359 || s.ParentGuid == 0 || s.End != 0x1954523 || m.Offset != 0x1954528 {
		t.Errorf("%d items inherit; Crate_S_General ends at %#x (parent %d), Crate_M_General starts at %#x", inherited, s.End, s.ParentGuid, m.Offset)
	}
	conds, err := d.Decode(12, "AIUseAbilityConditionRecord")
	if err != nil {
		t.Fatal(err)
	}
	c0 := conds[0].Fields["Conditions"].([]Value)
	if len(c0) != 2 || c0[0].(Struct)["Type"] != "/Script/GameSystemsPlugin.AbilityConditionIsMostHated" || c0[0].(Struct)["Value"] == nil {
		t.Errorf("AIUseAbilityCondition row 0 conditions = %v", c0)
	}
	kills, err := d.Decode(266, "GlobalKillVolumeList")
	if err != nil {
		t.Fatal(err)
	}
	entries := kills[0].Fields["KillVolumesMap"].(Pairs)
	set, unset := 0, 0
	for _, e := range entries {
		if e.Value.(Struct)["ComplexSplineCurve"] == nil {
			unset++
		} else {
			set++
		}
	}
	if len(entries) == 0 || set+unset != len(entries) {
		t.Errorf("GlobalKillVolumeList: %d entries, %d with a ComplexSplineCurve", len(entries), set)
	}
	t.Logf("GlobalKillVolumeList: %d kill volumes, %d with a ComplexSplineCurve", len(entries), set)
}

// (C:Y:B : C | O | A)

// TestRealDecodeAll decodes every table that classifies to one struct and
// reports how long it takes; the walk's self-check makes each of them exact.
func TestRealDecodeAll(t *testing.T) {
	_, _, d, cl := realFixture(t)
	t0 := time.Now()
	tables, rows := 0, 0
	for _, c := range cl {
		if !c.Decodes {
			continue
		}
		rs, err := d.Decode(c.Table, c.Schema)
		if err != nil {
			t.Errorf("table %d as %s: %v", c.Table, c.Schema, err)
			continue
		}
		if len(rs) != int(c.Rows) {
			t.Errorf("table %d: %d rows, directory says %d", c.Table, len(rs), c.Rows)
		}
		for _, r := range rs {
			if r.Guid == 0 && r.Name == "" {
				t.Errorf("table %d: row at %#x has neither a Guid nor a name", c.Table, r.Offset)
				break
			}
		}
		tables++
		rows += len(rs)
	}
	took := time.Since(t0)
	t.Logf("decoded %d tables, %d rows in %v", tables, rows, took)
	if tables != 369 || rows != 205882 {
		t.Errorf("decoded %d tables, %d rows; want 369, 205882", tables, rows)
	}
	if took > 60*time.Second {
		t.Errorf("decoding took %v, want seconds", took)
	}
}

// TestRealJSON: every row of every decoded table encodes as JSON as it is (no NaN
// or infinite float, which contentc would store as a string), which is how
// contentc stores them.
func TestRealJSON(t *testing.T) {
	_, _, d, cl := realFixture(t)
	for _, c := range cl {
		if !c.Decodes {
			continue
		}
		rs, err := d.Decode(c.Table, c.Schema)
		if err != nil {
			t.Fatal(err)
		}
		for _, r := range rs {
			if _, err := json.Marshal(r); err != nil {
				t.Errorf("table %d (%s) row %q: %v", c.Table, c.Schema, r.Name, err)
				break
			}
		}
	}
}

func TestRealStats(t *testing.T) {
	_, _, d, _ := realFixture(t)

	defs, err := d.Decode(3, "StatTypeDefRecordBase")
	if err != nil {
		t.Fatal(err)
	}
	if len(defs) != 732 {
		t.Errorf("StatTypeDef has %d rows, want 732", len(defs))
	}
	byGuid := map[int64]Row{}
	for _, r := range defs {
		byGuid[r.Guid] = r
	}
	for guid, name := range map[int64]string{guidStamina: "Stamina", guidMaxStamina: "MaxStamina"} {
		if r, ok := byGuid[guid]; !ok || r.Name != name {
			t.Errorf("stat %d = %+v, want %s", guid, r, name)
		}
	}
	if st := byGuid[guidStamina].Fields; st["StatType"] != "EStatRecordType::Type_Float" {
		t.Errorf("Stamina StatType = %v", st["StatType"])
	}

	// Player_Base sets MaxStamina (and Stamina) to 100.
	inits, err := d.Decode(303, "StatInitializerListRecordBase")
	if err != nil {
		t.Fatal(err)
	}
	var playerBase *Row
	for i := range inits {
		if inits[i].Name == "Player_Base" {
			playerBase = &inits[i]
		}
	}
	if playerBase == nil {
		t.Fatal("no Player_Base initializer list")
	}
	got := map[int64]string{}
	for _, v := range playerBase.Fields["StatIDs"].([]Value) {
		s := v.(Struct)
		got[s["StatId"].(Struct)["Guid"].(int64)] = s["ValueExpression"].(string)
	}
	if got[guidMaxStamina] != "100" || got[guidStamina] != "100" {
		t.Errorf("Player_Base: MaxStamina = %q, Stamina = %q; want 100", got[guidMaxStamina], got[guidStamina])
	}

	// The StatCurve table: 122 rows, and the EvalCurve calls of the initializers
	// (which name the table by its TypeId) refer to its rows.
	curves, err := d.Decode(296, "StatCurveRecordBase")
	if err != nil {
		t.Fatal(err)
	}
	if len(curves) != 122 {
		t.Errorf("StatCurve has %d rows, want 122", len(curves))
	}
	curveByGuid := map[int64]Row{}
	for _, r := range curves {
		curveByGuid[r.Guid] = r
	}
	evalCalls := 0
	const marker = "EvalCurve($#7037248826340794574:"
	for _, r := range inits {
		for _, v := range r.Fields["StatIDs"].([]Value) {
			expr := v.(Struct)["ValueExpression"].(string)
			for rest := expr; ; {
				i := strings.Index(rest, marker)
				if i < 0 {
					break
				}
				rest = rest[i+len(marker):]
				end := strings.Index(rest, "$")
				var guid int64
				for _, c := range rest[:end] {
					guid = guid*10 + int64(c-'0')
				}
				if _, ok := curveByGuid[guid]; !ok {
					t.Errorf("%s: EvalCurve refers to %d, which is not a StatCurve row", r.Name, guid)
				}
				evalCalls++
			}
		}
	}
	if evalCalls == 0 {
		t.Error("no EvalCurve call in the stat initializers")
	}

	// A curve's keys: Player_Base_Attrib_Constitution runs 30 at level 1 to 516
	// at level 50 (the survey read the same keys with the Python walker).
	var con *Row
	for i := range curves {
		if curves[i].Name == "Player_Base_Attrib_Constitution" {
			con = &curves[i]
		}
	}
	if con == nil {
		t.Fatal("no Player_Base_Attrib_Constitution curve")
	}
	keys := con.Fields["Curve"].(Struct)["EditorCurveData"].(Struct)["Keys"].([]Value)
	if len(keys) != 50 {
		t.Fatalf("Constitution curve has %d keys, want 50", len(keys))
	}
	first, last := keys[0].(Struct), keys[49].(Struct)
	if first["Time"] != float32(1) || first["Value"] != float32(30) || last["Time"] != float32(50) || last["Value"] != float32(516) {
		t.Errorf("Constitution keys: first %v, last %v", first, last)
	}
}

// (Cyb | C | O | A)

func TestRealSprintAndDodge(t *testing.T) {
	db, _, d, _ := realFixture(t)
	if uint64(guidSprint) != 0x5429e658a60b0054 {
		t.Errorf("Sprint %d is %#x, the survey quotes 0x5429e658a60b0054", int64(guidSprint), uint64(guidSprint))
	}
	rows, err := d.Decode(0, "AoCAbilityRecordBase")
	if err != nil {
		t.Fatal(err)
	}
	if len(rows) != 2130 {
		t.Errorf("Ability has %d rows, want 2130", len(rows))
	}
	var sprint, dodge *Row
	for i := range rows {
		switch rows[i].Guid {
		case guidSprint:
			sprint = &rows[i]
		case guidDodge:
			dodge = &rows[i]
		}
	}
	if sprint == nil || dodge == nil {
		t.Fatalf("Sprint %v, Dodge %v", sprint != nil, dodge != nil)
	}
	if sprint.Name != "General_Sprint" || dodge.Name != "General_Dodge" {
		t.Errorf("names %q, %q", sprint.Name, dodge.Name)
	}

	// The raw table holds each Guid inside the row the walker decoded it from
	// (Guids end a row, after the record's own properties).
	for i := range rows {
		r := &rows[i]
		if r != sprint && r != dodge {
			continue
		}
		end := rows[i+1].Offset
		inside := 0
		for _, h := range db.FindInt64(0, r.Guid) {
			if h >= r.Offset && h+8 <= end {
				inside++
			}
		}
		if inside == 0 {
			t.Errorf("%s: Guid %d is not inside its row [%#x, %#x) in the raw table", r.Name, r.Guid, r.Offset, end)
		}
	}

	cost := func(r *Row) Struct { return r.Fields["Costs"].([]Value)[0].(Struct) }
	statCost := func(r *Row) Struct { return cost(r)["StatCosts"].([]Value)[0].(Struct) }

	// Sprint: held, drains stamina while moving, 1.25 per second out of combat
	// and 2.5 in combat, no cooldown.
	if sprint.Fields["Cooldown"] != "0" {
		t.Errorf("Sprint Cooldown = %q", sprint.Fields["Cooldown"])
	}
	if cost(sprint)["Enabled"] != "IsMoving(GetOwner())" || cost(sprint)["bTriggersCooldown"] != false {
		t.Errorf("Sprint cost = %v", cost(sprint))
	}
	if g := statCost(sprint)["Stat"].(Struct)["Guid"]; g != int64(guidStamina) {
		t.Errorf("Sprint cost stat = %v, want Stamina", g)
	}
	expr := statCost(sprint)["Value"].(string)
	if !strings.Contains(expr, "var Cost = 1.25;") || !strings.Contains(expr, "set Cost = 2.5;") || !strings.Contains(expr, "IsInCombat(GetSource())") {
		t.Errorf("Sprint cost expression = %q", expr)
	}
	if req := sprint.Fields["ActivationRequirements"].(string); !strings.Contains(req, "$#5365104778484440215:6064629575339934476$") {
		t.Errorf("Sprint ActivationRequirements = %q", req)
	}
	if sprint.Fields["HeldAbilityBehavior"] != "EHeldAbilityPressBehavior::Smart" {
		t.Errorf("Sprint HeldAbilityBehavior = %v", sprint.Fields["HeldAbilityBehavior"])
	}

	// Dodge: one cost of stamina that does trigger the cooldown, whose
	// expression selects 0.4 or 0.65 seconds.
	if cost(dodge)["bTriggersCooldown"] != true {
		t.Errorf("Dodge cost = %v", cost(dodge))
	}
	if g := statCost(dodge)["Stat"].(Struct)["Guid"]; g != int64(guidStamina) {
		t.Errorf("Dodge cost stat = %v, want Stamina", g)
	}
	if cd := dodge.Fields["Cooldown"].(string); !strings.Contains(cd, "0.65") || !strings.Contains(cd, "0.4") {
		t.Errorf("Dodge Cooldown = %q", cd)
	}
}

// [Cyb - C-O-A]

// TestRealPatches: the four patches apply to the old usmap and are already
// present in the generated one, which needs none; without them the old usmap
// does not decode AbilityHit (the walk fails in the first row, at the byte it
// does not know) or PopulationArea, and reads PopulationInteractVolume wrongly
// (a location as a Guid: the walk ends where it should by a 24-byte
// coincidence). With them both files read the same rows.
func TestRealPatches(t *testing.T) {
	db, m, d, _ := realFixture(t)
	want := PatchApplied
	if fixture.generated {
		want = PatchPresent
	}
	results := d.PatchResults()
	if len(results) != 4 {
		t.Errorf("%d patch results, want 4", len(results))
	}
	for _, r := range results {
		if r.Status != want {
			t.Errorf("patch %s: %s, want %s", r.Patch, r.Status, want)
		}
	}
	if err := d.CheckPatches(); err != nil {
		t.Error(err)
	}
	unpatched := db.DecoderWith(m, Options{})
	if !fixture.generated {
		rows, err := unpatched.DecodePartial(2, "AbilityHitRecordBase")
		de, ok := err.(*DecodeError)
		if !ok {
			t.Fatalf("unpatched AbilityHit decoded: %d rows, %v", len(rows), err)
		}
		if de.Row != 0 || de.Path != "DefaultFallbackTargetType" || len(rows) != 0 {
			t.Errorf("unpatched error = %v", de)
		}
		if err := unpatched.Check(228, "PopulationAreaRecordBase"); err == nil {
			t.Error("the old usmap decoded PopulationArea without the MaxNumSpawns patch")
		}
	}
	// PV_0026_MinotaurBlocker: a zone at (-561823, 598365, 15492) with an extent
	// of 1000 and no rotation; the old usmap without its patch reads the location
	// as ZoneFGuid and the extent as the location.
	zones, err := d.Decode(233, "PopulationInteractVolumeRecordBase")
	if err != nil {
		t.Fatal(err)
	}
	z := zones[1].Fields["PopulationInteractZone"].(Struct)
	loc, ext, rot := z["Location"].(Struct), z["Extents"].(Struct), z["Rotation"].(Struct)
	if zones[1].Name != "PV_0026_MinotaurBlocker" || math.Round(loc["X"].(float64)) != -561823 || ext["X"] != float64(1000) ||
		rot["Yaw"] != float64(0) || z["ZoneFGuid"] != nil || z["OwningVolumeGuid"] != nil {
		t.Errorf("PV_0026_MinotaurBlocker zone = %v", z)
	}
	// Windows: Banner is the stored path, IconImage (Transient) is not read.
	wins, err := d.Decode(359, "WindowsRecordBase")
	if err != nil {
		t.Fatal(err)
	}
	if w := wins[0].Fields["Window"].(Struct); w["Icon"] == "" || w["IconImage"] != nil || w["Banner"] == nil {
		t.Errorf("Window of %s = %v", wins[0].Name, w)
	}
	// The patched walk reads every AbilityHit row and the first has the property.
	hits, err := d.Decode(2, "AbilityHitRecordBase")
	if err != nil {
		t.Fatal(err)
	}
	if len(hits) != 3608 {
		t.Errorf("AbilityHit has %d rows, want 3608", len(hits))
	}
	if _, ok := hits[0].Fields["bMayCorrupt"].(bool); !ok {
		t.Errorf("bMayCorrupt = %v", hits[0].Fields["bMayCorrupt"])
	}
	// The Ability table itself needs no patch (it needs the map rule).
	if err := unpatched.Check(0, "AoCAbilityRecordBase"); err != nil {
		t.Errorf("Ability without patches: %v", err)
	}
}
