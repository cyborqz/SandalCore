// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"testing"

	"aocrop/services/internal/aocexpr"
	"aocrop/services/internal/chardef"
	"aocrop/services/internal/content"
	"aocrop/services/internal/designdb"
	"aocrop/services/internal/usmap"
)

// These tests build content.db from the game's real design database. The
// inputs stay outside the repo: AOC_DESIGN_DB is CacheDB.dbc, AOC_USMAP the
// mappings file, and AOC_CLIENT_CONFIG (optional) the Cache.db export's
// AOC\Config directory. The tests skip when the first two are unset or missing.
//
// One build (about 20 s) is shared by every test below; the determinism test
// makes a second.

func realInputs(t *testing.T) (dbc, usmap, config string) {
	t.Helper()
	dbc, usmap, config = os.Getenv("AOC_DESIGN_DB"), os.Getenv("AOC_USMAP"), os.Getenv("AOC_CLIENT_CONFIG")
	if dbc == "" || usmap == "" {
		t.Skip("AOC_DESIGN_DB and AOC_USMAP not set")
	}
	for _, p := range []string{dbc, usmap} {
		if _, err := os.Stat(p); err != nil {
			t.Skipf("%v", err)
		}
	}
	return dbc, usmap, config
}

// [CoA ~ C:Y:B]

var built struct {
	once   sync.Once
	dir    string
	path   string
	stdout string
	stderr string
	code   int
}

// TestMain removes the shared build's directory when the tests are done.
func TestMain(m *testing.M) {
	code := m.Run()
	if built.dir != "" {
		os.RemoveAll(built.dir)
	}
	os.Exit(code)
}

// builtDB runs the command line once (as a user would) and opens its output.
func builtDB(t *testing.T) *content.DB {
	t.Helper()
	dbc, usmap, config := realInputs(t)
	built.once.Do(func() {
		var err error
		if built.dir, err = os.MkdirTemp("", "contentc-test-"); err != nil {
			t.Fatal(err)
		}
		built.path = filepath.Join(built.dir, "out", "content.db")
		var stdout, stderr bytes.Buffer
		args := []string{"--dbc", dbc, "--usmap", usmap, "--out", built.path}
		if config != "" {
			args = append(args, "--config", config)
		}
		built.code = run(args, &stdout, &stderr)
		built.stdout, built.stderr = stdout.String(), stderr.String()
	})
	if built.code != 0 {
		t.Fatalf("contentc exited %d: %s", built.code, built.stderr)
	}
	db, err := content.Open(built.path)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { db.Close() })
	return db
}

func scalarInt(t *testing.T, db *content.DB, query string, args ...any) int64 {
	t.Helper()
	var n int64
	if err := db.SQL().QueryRow(query, args...).Scan(&n); err != nil {
		t.Fatalf("%s: %v", query, err)
	}
	return n
}

// generatedUsmap reports whether path is the usmap generated from the client
// executable (it lists AbilityHitRecordBase.bMayCorrupt; the old one does not).
func generatedUsmap(t *testing.T, path string) bool {
	t.Helper()
	m, err := usmap.Load(path)
	if err != nil {
		t.Fatal(err)
	}
	for _, p := range m.Schema("AbilityHitRecordBase").Props {
		if p.Name == "bMayCorrupt" {
			return true
		}
	}
	return false
}

// expectedPatches is what meta records about the patches with this usmap:
// the status of each (all applied to the old usmap, all already present in
// the generated one), and the ones applied, sorted as meta lists them.
func expectedPatches(generated bool) (status map[string]string, applied []string) {
	status = map[string]string{}
	applied = []string{}
	for _, p := range designdb.Patches() {
		s := designdb.PatchApplied
		if generated {
			s = designdb.PatchPresent
		}
		status[p.String()] = s.String()
		if s == designdb.PatchApplied {
			applied = append(applied, p.String())
		}
	}
	sort.Strings(applied)
	return status, applied
}

// CoA with C:Y:B

// TestRealBuild: the command line runs end to end, and meta says where the
// file came from.
func TestRealBuild(t *testing.T) {
	dbcPath, usmapPath, _ := realInputs(t)
	db := builtDB(t)
	if !strings.Contains(built.stdout, "369 tables decoded of 369, 205882 rows") {
		t.Errorf("summary line: %s", built.stdout)
	}
	t.Logf("summary: %s", strings.TrimSpace(built.stdout[strings.LastIndex(strings.TrimSpace(built.stdout), "\n")+1:]))
	st, err := os.Stat(built.path)
	if err != nil {
		t.Fatal(err)
	}
	t.Logf("content.db: %d MB", st.Size()>>20)
	// 270 MB with every table decoded (the first slice's 301 tables took 165 MB;
	// the spec allowed for 250).
	if st.Size() > 300<<20 {
		t.Errorf("content.db is %d MB, over 300 MB", st.Size()>>20)
	}
	patchStatus, patchesApplied := expectedPatches(generatedUsmap(t, usmapPath))
	statusJSON, _ := json.Marshal(patchStatus)
	appliedJSON, _ := json.Marshal(patchesApplied)
	if _, err := os.Stat(built.path + ".tmp"); err == nil {
		t.Error("the temporary file was left behind")
	}

	meta, err := db.Meta()
	if err != nil {
		t.Fatal(err)
	}
	hash := func(path string) (string, int64) {
		data, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		sum := sha256.Sum256(data)
		return hex.EncodeToString(sum[:]), int64(len(data))
	}
	dbcSum, dbcSize := hash(dbcPath)
	mapSum, mapSize := hash(usmapPath)
	for key, want := range map[string]string{
		"schema_version":          "1",
		"record.encoding":         content.EncodingGzip,
		"contentc.version":        Version,
		"source.dbc.sha256":       dbcSum,
		"source.dbc.size":         strconv.FormatInt(dbcSize, 10),
		"source.dbc.tables":       "369",
		"source.usmap.sha256":     mapSum,
		"source.usmap.size":       strconv.FormatInt(mapSize, 10),
		"decode.tables":           "369",
		"decode.rows":             "205882",
		"decode.skipped_tables":   "0",
		"decode.nonfinite_floats": "0",
		"decode.patches":          string(appliedJSON),
		"decode.patch_status":     string(statusJSON),
	} {
		if meta[key] != want {
			t.Errorf("meta %s = %q, want %q", key, meta[key], want)
		}
	}
	if name := filepath.Base(dbcPath); meta["source.dbc.name"] != name {
		t.Errorf("meta source.dbc.name = %q, want %q", meta["source.dbc.name"], name)
	}
	if !strings.HasPrefix(meta["source.dbc.crc"], "ok:") {
		t.Errorf("meta source.dbc.crc = %q", meta["source.dbc.crc"])
	}
	// CacheDB.dbc's generation time (package designdb).
	if meta["source.dbc.generated_at"] != "2026-01-20T01:53:09Z" {
		t.Errorf("meta source.dbc.generated_at = %q", meta["source.dbc.generated_at"])
	}
	if !strings.Contains(meta["note.0_AoCEvolve"], "0_AoCEvolve.dbc") || !strings.Contains(meta["note.0_AoCEvolve"], "6 tables") {
		t.Errorf("meta note.0_AoCEvolve = %q", meta["note.0_AoCEvolve"])
	}
	for _, k := range []string{"built_at", "build_seconds", "source.dbc.path", "source.usmap.path", "typed.stat_def", "typed.class_stat_base"} {
		if meta[k] == "" {
			t.Errorf("meta %s is empty", k)
		}
	}
}

// TestRealRecordCounts: record counts per decoded table, in meta, in
// record_table and in record itself, agree; the tables of the first slice have
// the counts the survey found.
func TestRealRecordCounts(t *testing.T) {
	db := builtDB(t)
	meta, err := db.Meta()
	if err != nil {
		t.Fatal(err)
	}
	var perTable map[string]int
	if err := json.Unmarshal([]byte(meta["decode.table_rows"]), &perTable); err != nil {
		t.Fatal(err)
	}
	tables, err := db.RecordTables()
	if err != nil {
		t.Fatal(err)
	}
	if len(tables) != 369 {
		t.Errorf("record_table has %d tables, want 369", len(tables))
	}
	decoded, total := 0, 0
	for _, tb := range tables {
		inRecord := int(scalarInt(t, db, `SELECT count(*) FROM record WHERE table_index = ?`, tb.Index))
		if !tb.Decoded {
			if inRecord != 0 {
				t.Errorf("table %d did not decode but has %d records", tb.Index, inRecord)
			}
			if _, ok := perTable[strconv.Itoa(tb.Index)]; ok {
				t.Errorf("meta counts the undecoded table %d", tb.Index)
			}
			continue
		}
		decoded++
		total += inRecord
		if inRecord != tb.Rows || perTable[strconv.Itoa(tb.Index)] != tb.Rows {
			t.Errorf("table %d (%s): directory %d rows, record %d, meta %d", tb.Index, tb.Schema, tb.Rows, inRecord, perTable[strconv.Itoa(tb.Index)])
		}
	}
	if decoded != 369 || total != 205882 || len(perTable) != 369 {
		t.Errorf("decoded %d tables, %d rows (meta lists %d tables); want 369, 205882", decoded, total, len(perTable))
	}
	for _, w := range []struct {
		table  int
		schema string
		rows   int
	}{
		{0, "AoCAbilityRecordBase", 2130},
		{2, "AbilityHitRecordBase", 3608},
		{3, "StatTypeDefRecordBase", 732},
		{4, "EffectRecordBase", 2794},
		{44, "CharacterClassesRecordBase", 16},
		{296, "StatCurveRecordBase", 122},
		{303, "StatInitializerListRecordBase", 145},
		{307, "StatModRecordBase", 3133},
	} {
		tb := tables[w.table]
		if tb.Schema != w.schema || tb.Rows != w.rows || !tb.Decoded {
			t.Errorf("table %d = %+v, want %s with %d rows, decoded", w.table, tb, w.schema, w.rows)
		}
	}
	// The typed tables hold what the record rows say: one stat_def per StatTypeDef
	// row, one stat_curve key per key of the 122 curves, one stat_init entry per
	// entry of the 145 lists (1087 of them), the 3 abilities, and the effects and
	// hits they reach (see buildSkills).
	for table, count := range map[string]int64{
		"stat_def":        732,
		"stat_curve_def":  122,
		"stat_curve":      3067, // 2917 and the 150 keys of the three profession level-up curves (mapc artisan-curves; Gathering stage 2)
		"stat_init":       1087,
		"class":           8,
		"class_stat_base": 8 * 6 * 50,
		"ability":         3,
		"ability_cost":    2,
		"effect":          14,
		"hit":             14,
		"ability_runtime": 3,
		"effect_runtime":  4,
	} {
		if got := scalarInt(t, db, `SELECT count(*) FROM `+table); got != count {
			t.Errorf("%s has %d rows, want %d", table, got, count)
		}
	}
}

// [CoA + C.Y.B]

// The values the original server sent for the Bard at level 1: replay_data.bin,
// packet 107, UAoCStatsComponent, the StatRepInt32Owner fast array (each item
// is a stat Guid and the bits of a float32), decoded by the survey's
// dump_stats.py into scratchpad c1\stats200.json. The Intelligence entry is the
// open item of spec §6: contentc computes 39.
var (
	capturedBardLevel1 = map[string]float64{
		"Strength": 24, "Dexterity": 24, "Constitution": 30, "Mentality": 30, "Wisdom": 36,
	}
	capturedBardLevel1Intelligence = 60.0
	// The same packet's stamina stats.
	capturedStamina = map[string]float64{"Stamina": 100, "MaxStamina": 100, "StaminaRegeneration": 5}
)

// TestRealGoldenBard: contentc reproduces the captured level-1 Strength,
// Dexterity, Constitution, Mentality and Wisdom of the Bard.
func TestRealGoldenBard(t *testing.T) {
	db := builtDB(t)
	got, err := db.ClassStatBase("Bard", 1)
	if err != nil {
		t.Fatal(err)
	}
	if len(got) != 6 {
		t.Errorf("Bard level 1 has %d attributes, want 6: %v", len(got), got)
	}
	for stat, want := range capturedBardLevel1 {
		if got[stat].Value != want {
			t.Errorf("Bard level 1 %s = %v, the capture has %v", stat, got[stat].Value, want)
		}
		if got[stat].RoundingAmbiguous {
			t.Errorf("Bard level 1 %s is flagged as depending on the arithmetic's width", stat)
		}
	}
}

// TestKnownDifferenceBardIntelligence documents the open item: the formula
// (Player_Base_Bard: Round(curve * 1.3), the curve's level 1 value being 30)
// gives 39, the original server sent 60. Nothing in the design database
// explains the 21 points. The test pins the computed value so that a change in
// either number is noticed; it does not hide the difference.
func TestKnownDifferenceBardIntelligence(t *testing.T) {
	db := builtDB(t)
	got, err := db.ClassStatBase("Bard", 1)
	if err != nil {
		t.Fatal(err)
	}
	if got["Intelligence"].Value != 39 {
		t.Errorf("Bard level 1 Intelligence = %v, the formula gives 39", got["Intelligence"].Value)
	}
	if got["Intelligence"].Value == capturedBardLevel1Intelligence {
		t.Errorf("Intelligence now matches the capture (%v): the open item is closed, update spec §6 and this test", capturedBardLevel1Intelligence)
	}
	t.Logf("KNOWN DIFFERENCE: Bard level 1 Intelligence is %v by Player_Base_Bard's formula and %v in the capture (packet 107)",
		got["Intelligence"].Value, capturedBardLevel1Intelligence)
}

var guidDefaultClasses = regexp.MustCompile(`\((\w+), \(Guid=(\d+)\)\)`)

// TestRealClasses: the eight player classes, their Guids (checked against
// DefaultClasses in DefaultAoCRecordConstants.ini when the Config directory is
// given) and archetype ids.
func TestRealClasses(t *testing.T) {
	_, _, config := realInputs(t)
	db := builtDB(t)
	classes, err := db.Classes()
	if err != nil {
		t.Fatal(err)
	}
	if len(classes) != 8 {
		t.Fatalf("%d classes: %+v", len(classes), classes)
	}
	// Archetype order is the order of the ids: Bard 17747 ... Tank 17754.
	wantOrder := []string{"Bard", "Cleric", "Fighter", "Mage", "Ranger", "Rogue", "Summoner", "Tank"}
	for i, c := range classes {
		enum, _ := chardef.ClassByName(c.Name)
		if c.Name != wantOrder[i] || c.ArchetypeID != int64(17747+i) || int64(chardef.ArchetypeID(enum)) != c.ArchetypeID {
			t.Errorf("class %d = %s with archetype %d (chardef says %d), want %s %d", i, c.Name, c.ArchetypeID, chardef.ArchetypeID(enum), wantOrder[i], 17747+i)
		}
		if c.ClassEnum != "ECharacterClass::"+c.Name || c.StatInitListName != "Player_Base_"+c.Name || c.StatInitListGuid == 0 {
			t.Errorf("class %+v", c)
		}
		if c.LevelTypeName != "Adventuring" || c.LevelTypeGuid != 108651085518458 {
			t.Errorf("%s level type = %s (%d)", c.Name, c.LevelTypeName, c.LevelTypeGuid)
		}
	}
	// Class records known from the survey: the Guid, whether it prefers melee.
	byName := map[string]content.Class{}
	for _, c := range classes {
		byName[c.Name] = c
	}
	for name, want := range map[string]struct {
		guid  int64
		melee bool
	}{
		"Bard": {108640158570105, false}, "Tank": {108640201692798, true}, "Mage": {108640210802303, false},
	} {
		if c := byName[name]; c.Guid != want.guid || c.PrefersMelee != want.melee {
			t.Errorf("%s = %+v, want Guid %d, melee %v", name, c, want.guid, want.melee)
		}
	}

	if config == "" {
		t.Log("AOC_CLIENT_CONFIG is not set: the class Guids are not checked against DefaultAoCRecordConstants.ini")
		return
	}
	data, err := os.ReadFile(filepath.Join(config, "DefaultAoCRecordConstants.ini"))
	if err != nil {
		t.Logf("%v: the class Guids are not checked against the ini", err)
		return
	}
	line := ""
	for _, l := range strings.Split(string(data), "\n") {
		if strings.HasPrefix(l, "DefaultClasses=") {
			line = l
		}
	}
	matches := guidDefaultClasses.FindAllStringSubmatch(line, -1)
	if len(matches) != 8 {
		t.Fatalf("DefaultClasses in the ini lists %d classes: %q", len(matches), line)
	}
	for _, m := range matches {
		g, _ := strconv.ParseInt(m[2], 10, 64)
		if byName[m[1]].Guid != g {
			t.Errorf("%s: content.db has Guid %d, DefaultClasses has %d", m[1], byName[m[1]].Guid, g)
		}
	}
}

var attributeFormula = regexp.MustCompile(`^const v = EvalCurve\(\$#7037248826340794574:(\d+)\$, Float\(GetCharacterLevel\(GetOwner\(\)\)\)\);\r\nRound\(v \* ([0-9.]+)\)$`)

// TestRealClassStatBase recomputes every class_stat_base value without
// aocexpr: the six formulas of each Player_Base_<Class> list all have the shape
// "curve value times a multiplier, rounded", so the test reads the curve's keys
// and the multiplier and does the arithmetic itself, in float64 and in float32.
func TestRealClassStatBase(t *testing.T) {
	db := builtDB(t)
	classes, err := db.Classes()
	if err != nil {
		t.Fatal(err)
	}
	curveValue := func(guid int64, level int) float64 {
		var v float64
		if err := db.SQL().QueryRow(`SELECT value FROM stat_curve WHERE guid = ? AND level = ?`, guid, level).Scan(&v); err != nil {
			t.Fatalf("curve %d level %d: %v", guid, level, err)
		}
		return v
	}
	if n := scalarInt(t, db, `SELECT count(*) FROM class_stat_base`); n != 2400 {
		t.Fatalf("class_stat_base has %d rows, want 2400", n)
	}
	// The six attribute curves have 50 linear keys (levels 1-50) and no external
	// curve asset (3 other StatCurve rows have one next to their keys, 3 have
	// nothing else), so no extrapolation or cubic segment is ever evaluated.
	if n := scalarInt(t, db, `SELECT count(*) FROM stat_curve_def WHERE name LIKE 'Player_Base_Attrib_%' AND key_count = 50 AND external_curve = ''`); n != 6 {
		t.Errorf("%d of the 6 Player_Base_Attrib curves have 50 keys and no external curve", n)
	}
	if n := scalarInt(t, db, `SELECT count(*) FROM stat_curve WHERE name LIKE 'Player_Base_Attrib_%' AND interp != 'Linear'`); n != 0 {
		t.Errorf("%d keys of the attribute curves are not linear", n)
	}
	ambiguous := map[[3]string]bool{}
	var halves, differing int
	for _, c := range classes {
		inits, err := db.StatInits(c.StatInitListName)
		if err != nil {
			t.Fatal(err)
		}
		attrs := 0
		for _, in := range inits {
			isAttr := false
			for _, a := range content.PrimaryAttributes {
				isAttr = isAttr || a.Guid == in.StatGuid
			}
			if !isAttr {
				continue
			}
			attrs++
			m := attributeFormula.FindStringSubmatch(in.Expression)
			if m == nil {
				t.Errorf("%s %s: formula %q has an unexpected shape", c.Name, in.StatName, in.Expression)
				continue
			}
			curve, _ := strconv.ParseInt(m[1], 10, 64)
			mult, _ := strconv.ParseFloat(m[2], 64)
			for level := 1; level <= 50; level++ {
				v := curveValue(curve, level)
				x := v * mult
				want64 := math.Floor(x + 0.5)
				x32 := float32(v) * float32(mult)
				want32 := math.Floor(float64(x32 + 0.5))
				var got float64
				var amb int
				if err := db.SQL().QueryRow(`SELECT value, rounding_ambiguous FROM class_stat_base WHERE class_guid = ? AND level = ? AND stat_guid = ?`,
					c.Guid, level, in.StatGuid).Scan(&got, &amb); err != nil {
					t.Fatalf("%s %s level %d: %v", c.Name, in.StatName, level, err)
				}
				if got != want64 {
					t.Errorf("%s %s level %d = %v, want round(%v * %v) = %v", c.Name, in.StatName, level, got, v, mult, want64)
				}
				if got != math.Trunc(got) {
					t.Errorf("%s %s level %d = %v is not an integer", c.Name, in.StatName, level, got)
				}
				if frac := x - math.Floor(x); math.Abs(frac-0.5) < 1e-6 {
					halves++
				}
				if want64 != want32 {
					differing++
					ambiguous[[3]string{c.Name, in.StatName, strconv.Itoa(level)}] = true
				}
				if (amb == 1) != (want64 != want32) {
					t.Errorf("%s %s level %d: rounding_ambiguous = %d, but float64 gives %v and float32 %v", c.Name, in.StatName, level, amb, want64, want32)
				}
			}
		}
		if attrs != 6 {
			t.Errorf("%s: %d attribute formulas, want 6", c.Name, attrs)
		}
	}
	// 80 of the 2400 values sit on a half; for 7 of them float32 arithmetic
	// rounds the other way (335 * 1.3 at level 20): not proven either way.
	if halves != 80 || differing != 7 {
		t.Errorf("%d values on a half, %d where float32 differs; the survey found 80 and 7", halves, differing)
	}
	amb, err := db.RoundingAmbiguous()
	if err != nil {
		t.Fatal(err)
	}
	if len(amb) != 7 {
		t.Errorf("RoundingAmbiguous lists %d values, want 7: %+v", len(amb), amb)
	}
	for _, a := range amb {
		if a.Level != 20 || a.Value != 436 || !ambiguous[[3]string{a.Class, a.Stat, strconv.Itoa(a.Level)}] {
			t.Errorf("unexpected ambiguous value %+v", a)
		}
	}

	// Values the survey read from the curves with its own walker (prototype
	// scratchpad c1\statcurves.json): Player_Base_Attrib_Constitution runs 30,
	// 68, 118, 152 at levels 1-4 and 516 at level 50; the Tank's multiplier is 1.3.
	for _, w := range []struct {
		class string
		level int
		stat  string
		want  float64
	}{
		{"Tank", 1, "Constitution", 39}, {"Tank", 2, "Constitution", 88}, {"Tank", 3, "Constitution", 153},
		{"Tank", 4, "Constitution", 198}, {"Tank", 50, "Constitution", 671},
		{"Bard", 2, "Constitution", 68}, {"Bard", 50, "Constitution", 516},
		{"Mage", 1, "Intelligence", 39}, {"Mage", 1, "Strength", 24},
	} {
		got, err := db.ClassStatBase(w.class, w.level)
		if err != nil || got[w.stat].Value != w.want {
			t.Errorf("%s level %d %s = %v, %v; want %v", w.class, w.level, w.stat, got[w.stat].Value, err, w.want)
		}
	}
}

// [C_Y_B :: CoA]

// TestRealStatDefs: the stats of the first slice, and the replication audience
// against what the capture shows.
func TestRealStatDefs(t *testing.T) {
	db := builtDB(t)
	for _, w := range []struct {
		name         string
		guid         int64
		replication  int
		field        string
		min, max     string
		onSpawn      string
		regenContain string
	}{
		{"Stamina", content.GuidStamina, 1, "StatRepInt32Owner", "0", "GetStat(GetInstigator(), $#5365104778484440215:6064631747701440948$)", "EStatResetBehavior::Reset_SetMax", "$#5365104778484440215:6064631663405498419$"},
		{"MaxStamina", content.GuidMaxStamina, 1, "StatRepInt32Owner", "1", "", "EStatResetBehavior::Reset_None", ""},
		{"StaminaRegeneration", content.GuidStaminaRegeneration, 1, "StatRepInt32Owner", "0", "", "EStatResetBehavior::Reset_None", ""},
		{"Strength", content.GuidStrength, 1, "StatRepInt32Owner", "0", "", "EStatResetBehavior::Reset_None", ""},
	} {
		s, err := db.StatDefByName(w.name)
		if err != nil {
			t.Fatal(err)
		}
		if s.Guid != w.guid || s.Replication != w.replication || s.RepField != w.field || s.MinExpr != w.min || s.MaxExpr != w.max ||
			s.OnSpawn != w.onSpawn || !strings.Contains(s.RegenExpr, w.regenContain) || s.StatType != "EStatRecordType::Type_Float" {
			t.Errorf("%s = %+v", w.name, s)
		}
	}

	// The audience against the capture (replay_data.bin, the StatsComponent
	// fast arrays): the stats sent in each array and the field they travel in.
	// Everyone: all 11 stats of StatRepInt32EveryoneProxy; the others are samples
	// of the 96 of StatRepInt32OwnerProxy and the 331 of StatRepInt32Owner.
	capture := map[string][]string{
		"StatRepInt32EveryoneProxy": {"CharacterType", "Corruption", "GravityScale", "Health", "MagicalPenetration", "Mana",
			"MaxHealth", "MaxMana", "MoveSpeedMult", "PhysicalPenetration", "StealthLevel"},
		"StatRepInt32OwnerProxy": {"ArcaneDamage", "ArcaneDamageAddMult", "ArcaneDamageMult", "ArcaneMitigation", "BleedDamage",
			"BlockChance", "BlockMitigation", "BludgeonDamage"},
		"StatRepInt32Owner": {"Accuracy", "AccuracyRating", "AdventuringXPAddMult", "ArcaneDamageRating", "ArcaneMitigationRating",
			"Strength", "Dexterity", "Constitution", "Mentality", "Intelligence", "Wisdom", "Stamina", "MaxStamina", "StaminaRegeneration"},
	}
	for field, names := range capture {
		for _, name := range names {
			s, err := db.StatDefByName(name)
			if err != nil {
				t.Errorf("%s: %v", name, err)
				continue
			}
			if s.RepField != field {
				t.Errorf("%s: the capture sends it in %s, content.db says %q (replication %d)", name, field, s.RepField, s.Replication)
			}
		}
	}
	if n := scalarInt(t, db, `SELECT count(*) FROM stat_def WHERE rep_field IS NOT NULL AND replication NOT IN (1, 5, 7)`); n != 0 {
		t.Errorf("%d stats have a rep_field with another replication value", n)
	}
	// 7 means everyone: health and mana.
	if s, _ := db.StatDefByName("Health"); s.Audience != "Rep_Autonomous|Rep_Simulated|Rep_Proxy" || s.Replication != 7 {
		t.Errorf("Health = %+v", s)
	}
}

// (Cyb : C.O.A)

// TestRealStamina: Stamina and MaxStamina start at 100, StaminaRegeneration's
// base formula gives 5, and the stamina rules hold the numbers A7 needs.
func TestRealStamina(t *testing.T) {
	_, _, config := realInputs(t)
	db := builtDB(t)

	// The capture's own values for the same stats.
	for name, want := range capturedStamina {
		var rule string
		switch name {
		case "Stamina":
			rule = content.RuleStaminaInitial
		case "MaxStamina":
			rule = content.RuleMaxStaminaInitial
		case "StaminaRegeneration":
			rule = content.RuleRegenRateEnabled
		}
		rules, err := db.StaminaRules()
		if err != nil {
			t.Fatal(err)
		}
		if v, err := rules[rule].Float(); err != nil || v != want {
			t.Errorf("%s: rule %s = %q, the capture has %v", name, rule, rules[rule].Value, want)
		}
	}

	// Player_Base's entries are the plain text "100".
	inits, err := db.StatInits("Player_Base")
	if err != nil {
		t.Fatal(err)
	}
	got := map[string]string{}
	for _, in := range inits {
		got[in.StatName] = in.Expression
	}
	if got["MaxStamina"] != "100" || got["Stamina"] != "100" || got["StaminaRegeneration"] != "" {
		t.Errorf("Player_Base: MaxStamina %q, Stamina %q, StaminaRegeneration %q", got["MaxStamina"], got["Stamina"], got["StaminaRegeneration"])
	}

	rules, err := db.StaminaRules()
	if err != nil {
		t.Fatal(err)
	}
	num := func(key string) float64 {
		t.Helper()
		r, ok := rules[key]
		if !ok {
			t.Fatalf("no stamina rule %s", key)
		}
		v, err := r.Float()
		if err != nil {
			t.Fatalf("rule %s = %q: %v", key, r.Value, err)
		}
		if r.Source == "" {
			t.Errorf("rule %s has no source", key)
		}
		return v
	}
	for key, want := range map[string]float64{
		content.RuleStaminaInitial:        100,
		content.RuleMaxStaminaInitial:     100,
		content.RuleRegenRateEnabled:      5,
		content.RuleRegenRateDisabled:     0,
		content.RuleSprintCostOutOfCombat: 1.25,
		content.RuleSprintCostInCombat:    2.5,
		"sprint.cooldown":                 0,
		"sprint.effect_tick_timer":        1,
		content.RuleDodgeCostBase:         50,
		content.RuleDodgeCostSkilled:      30,
		content.RuleDodgeCooldownBase:     0.65,
		content.RuleDodgeCooldownSkilled:  0.4,
		content.RuleStatRegenTime:         1,
	} {
		if got := num(key); got != want {
			t.Errorf("rule %s = %v, want %v", key, got, want)
		}
	}
	// The regeneration formula itself, run here with the test's own host
	// functions: 5 per second, none while an exhausting effect is on.
	if v, err := aocexpr.EvalFloat(rules[content.RuleRegenBaseExpr].Value, ruleEnv{}.env()); err != nil || v != 5 {
		t.Errorf("StaminaRegeneration base expression = %v, %v; want 5", v, err)
	}
	if v, err := aocexpr.EvalFloat(rules[content.RuleRegenBaseExpr].Value, ruleEnv{moving: true}.with(6064631641826394197).env()); err != nil || v != 0 {
		t.Errorf("StaminaRegeneration while sprinting = %v, %v; want 0", v, err)
	}
	// StatRegenTime comes from DefaultCombat.ini when the Config directory is
	// given, else it is the documented default.
	src := rules[content.RuleStatRegenTime].Source
	if _, err := os.Stat(filepath.Join(config, "DefaultCombat.ini")); config != "" && err == nil {
		if !strings.HasPrefix(src, "DefaultCombat.ini") {
			t.Errorf("stat_regen_time source = %q, want DefaultCombat.ini", src)
		}
	} else if !strings.Contains(src, "the default 1 s") {
		t.Errorf("stat_regen_time source = %q, want the documented default", src)
	}
	// How often a held Sprint charges is not in any row it reaches: the interval is the TickTimer of the effect
	// General_Sprint_StaminaDrain, which no record references, and the rule says so.
	if r := rules[content.RuleSprintChargeInterval]; r.Value != "0.25" || !strings.Contains(r.Source, "General_Sprint_StaminaDrain") ||
		!strings.Contains(r.Source, "no record references that effect") {
		t.Errorf("sprint charge interval = %+v", r)
	}

	// The effects that stop regeneration, as the formula names them.
	dis, err := db.StaminaRegenDisables()
	if err != nil {
		t.Fatal(err)
	}
	wantDis := []content.RegenDisable{
		{Ord: 0, EffectGuid: 6064631641826394197, EffectName: "General_Sprint", DisablesStanding: false, DisablesMoving: true}, // only while moving
		{Ord: 1, EffectGuid: 6064631667969556480, EffectName: "General_Guard", DisablesStanding: true, DisablesMoving: true},
		{Ord: 2, EffectGuid: 106223481149050, EffectName: "General_Dodge_Evasion", DisablesStanding: true, DisablesMoving: true},
		{Ord: 3, EffectGuid: 6064631799761208354, EffectName: "General_Guard_GuardBreak_Stun", DisablesStanding: true, DisablesMoving: true},
		{Ord: 4, EffectGuid: 6064631668308443138, EffectName: "General_Stamina_DisableRegen", DisablesStanding: true, DisablesMoving: true},
	}
	if fmt.Sprint(dis) != fmt.Sprint(wantDis) {
		t.Errorf("regen disables = %+v\nwant %+v", dis, wantDis)
	}
}

// (Cyb + C O A)

// TestRealSprintAndDodge: the cost and cooldown fields of the two abilities,
// the effects they list and the effects' timers, tags and stat mods.
func TestRealSprintAndDodge(t *testing.T) {
	db := builtDB(t)

	sprint, err := db.AbilityByName("General_Sprint")
	if err != nil {
		t.Fatal(err)
	}
	if sprint.Guid != content.GuidSprint || sprint.DisplayName != "Sprint" || sprint.CooldownExpr != "0" ||
		sprint.HeldBehavior != "EHeldAbilityPressBehavior::Smart" || !sprint.OverrideHeldBehavior || !sprint.HoldAnimation ||
		sprint.DisableDuringCooldown || !sprint.UsableInAir || sprint.UsableWhileSwimming || !sprint.UsableWhileWalking ||
		sprint.UsableWhileMounted || fmt.Sprint(sprint.ComboInputs) != "[EComboButtons::BUTTON_Sprint]" ||
		sprint.ActivationRequirements != "GetStat(GetSource(), $#5365104778484440215:6064629575339934476$) >= 1.0" {
		t.Errorf("Sprint = %+v", sprint)
	}
	if len(sprint.Costs) != 1 {
		t.Fatalf("Sprint has %d costs: %+v", len(sprint.Costs), sprint.Costs)
	}
	sc := sprint.Costs[0]
	if sc.EnabledExpr != "IsMoving(GetOwner())" || sc.TriggersCooldown || sc.StatGuid != content.GuidStamina || sc.StatName != "Stamina" ||
		sc.ChargeCostExpr != "0" || !strings.Contains(sc.Expression, "var Cost = 1.25;") || !strings.Contains(sc.Expression, "set Cost = 2.5;") ||
		!strings.Contains(sc.Expression, "IsInCombat(GetSource())") {
		t.Errorf("Sprint cost = %+v", sc)
	}
	if len(sprint.Effects) != 1 || sprint.Effects[0].Name != "General_Sprint" || sprint.Effects[0].Guid != 6064631641826394197 || len(sprint.Hits) != 0 {
		t.Errorf("Sprint effects = %+v, hits = %+v", sprint.Effects, sprint.Hits)
	}

	dodge, err := db.AbilityByName("General_Dodge")
	if err != nil {
		t.Fatal(err)
	}
	if dodge.Guid != content.GuidDodge || dodge.DisplayName != "Dodge" || !dodge.DisableDuringCooldown || dodge.HeldBehavior != "EHeldAbilityPressBehavior::Naive" ||
		dodge.OverrideHeldBehavior || fmt.Sprint(dodge.Tags) != "[ability.dodge]" || dodge.NetworkingMaxSpeed != 2500 ||
		!dodge.UsableInAir || !dodge.UsableWhileSwimming || dodge.UsableWhileMounted {
		t.Errorf("Dodge = %+v", dodge)
	}
	if !strings.HasPrefix(dodge.CooldownExpr, "SelectFloat(") || !strings.HasSuffix(dodge.CooldownExpr, "0.4,\r\n0.65)") {
		t.Errorf("Dodge cooldown = %q", dodge.CooldownExpr)
	}
	if len(dodge.Costs) != 1 {
		t.Fatalf("Dodge has %d costs", len(dodge.Costs))
	}
	dc := dodge.Costs[0]
	if dc.EnabledExpr != "" || !dc.TriggersCooldown || dc.StatName != "Stamina" || !strings.Contains(dc.Expression, "30,\r\n50) * mult") ||
		!strings.Contains(dc.Expression, "Min((snareStacks + chilled),3)") {
		t.Errorf("Dodge cost = %+v", dc)
	}
	var names []string
	for _, e := range dodge.Effects {
		names = append(names, e.Name)
	}
	if fmt.Sprint(names) != "[General_Dodge_Evasion General_Stamina_Dodge_EvasiveManeuver_Tracker]" {
		t.Errorf("Dodge effects = %v", names)
	}

	// The effects.
	hits := func(e content.Effect) string {
		var parts []string
		for _, h := range e.Hits {
			parts = append(parts, fmt.Sprintf("%s:%s", h.Phase, h.Name))
		}
		return strings.Join(parts, " ")
	}
	e, err := db.EffectByName("General_Sprint")
	if err != nil {
		t.Fatal(err)
	}
	if !e.InfiniteDuration || e.TickTimer != 1 || !e.IgnoreInitialTick || fmt.Sprint(e.Tags) != "[Effect.DisableStamRegen]" ||
		fmt.Sprint(e.EffectTags) != "[Effect.DisableStamRegen]" || e.DisplayName != "Sprint" || e.Category != "EEffectCategory::EFFECT_Buff" ||
		hits(e) != "Tick:General_Stamina_Sprint_StartMoving_Hit Tick:General_Stamina_Sprint_SecondWind_Charge_Hit Tick:General_Sprint_StopMoving_Exertion End:General_Stamina_Sprint_EndHit" {
		t.Errorf("effect General_Sprint = %+v (hits %s)", e, hits(e))
	}
	if len(e.StatMods) != 1 || e.StatMods[0].StatName != "MoveSpeedMult" || !strings.HasPrefix(e.StatMods[0].ValueExpr, "const skilled1 = GetSkillPurchased(") ||
		!strings.HasSuffix(e.StatMods[0].ValueExpr, "SelectFloat(skilled3, 0.05, 0.0)") {
		t.Errorf("General_Sprint stat mods = %+v", e.StatMods)
	}

	e, err = db.EffectByName("General_Sprint_NotMoving")
	if err != nil {
		t.Fatal(err)
	}
	if !e.InfiniteDuration || e.TickTimer != 0.25 || !e.IgnoreInitialTick || e.GlobalStackLimit != 1 || e.Category != "EEffectCategory::EFFECT_Hidden" {
		t.Errorf("effect General_Sprint_NotMoving = %+v", e)
	}

	e, err = db.EffectByName("General_Stamina_DisableRegen")
	if err != nil {
		t.Fatal(err)
	}
	if e.DisplayName != "Exertion" || e.InfiniteDuration || e.DurationExpr != "1" || e.TickTimer != 0 ||
		hits(e) != "Apply:General_Stamina_StamRegenDisable_ApplyHit End:General_Stamina_StamRegenDisable_EndHit" {
		t.Errorf("effect General_Stamina_DisableRegen = %+v (hits %s)", e, hits(e))
	}
	applyHit, err := db.StatMods("hit", scalarGuid(t, db, "hit", "General_Stamina_StamRegenDisable_ApplyHit"))
	if err != nil || len(applyHit) != 1 || applyHit[0].StatName != "Stamina" || applyHit[0].ValueExpr != "0.001" {
		t.Errorf("DisableRegen apply hit stat mods = %+v, %v", applyHit, err)
	}

	e, err = db.EffectByName("General_Dodge_Evasion")
	if err != nil {
		t.Fatal(err)
	}
	if e.DisplayName != "Evading" || e.InfiniteDuration || e.DurationExpr != "0.5" ||
		hits(e) != "PreApply:General_Stamina_Dodge_EvasiveManeuver_Apply_Hit Apply:General_Stamina_Dodge_Recuperation_Counter_Apply Apply:General_Stamina_Dodge_GracefulReflexes_Cleanse_Hit End:General_Stamina_Dodge_EndHit" {
		t.Errorf("effect General_Dodge_Evasion = %+v (hits %s)", e, hits(e))
	}
	if len(e.StatMods) != 2 || e.StatMods[0].StatName != "MagicalEvasion" || e.StatMods[1].StatName != "PhysicalEvasion" ||
		!strings.HasPrefix(e.StatMods[1].ValueExpr, "1.0") {
		t.Errorf("General_Dodge_Evasion stat mods = %+v", e.StatMods)
	}

	e, err = db.EffectByName("General_Stamina_Dodge_EvasiveManeuver_Tracker")
	if err != nil {
		t.Fatal(err)
	}
	if e.DurationExpr != "0.5" || !strings.HasPrefix(e.ApplicationRequirements, "GetSkillPurchased(GetOwner(),") {
		t.Errorf("effect Evasive Maneuver Tracker = %+v", e)
	}
	// Everything in the effect set is findable by Guid.
	if n := scalarInt(t, db, `SELECT count(*) FROM effect`); n < 6 {
		t.Errorf("only %d effects", n)
	}
	if n := scalarInt(t, db, `SELECT count(*) FROM effect_hit WHERE hit_guid NOT IN (SELECT guid FROM hit)`); n != 0 {
		t.Errorf("%d effect hits have no hit row", n)
	}
	if n := scalarInt(t, db, `SELECT count(*) FROM hit_effect WHERE effect_guid NOT IN (SELECT guid FROM record WHERE type_id = ?)`, int64(content.TypeEffect)); n != 0 {
		t.Errorf("%d hit effects name no Effect record", n)
	}
}

func scalarGuid(t *testing.T, db *content.DB, table, name string) int64 {
	t.Helper()
	return scalarInt(t, db, `SELECT guid FROM `+table+` WHERE name = ?`, name)
}

// built by C O A with Cyb

// TestRealRecord: a row of the record table decodes to the fields the typed
// tables were made from.
func TestRealRecord(t *testing.T) {
	db := builtDB(t)
	rec, err := db.Record(content.TypeAbility, content.GuidSprint)
	if err != nil {
		t.Fatal(err)
	}
	if rec.Name != "General_Sprint" || rec.TableIndex != 0 || rec.Schema != "AoCAbilityRecordBase" || rec.DevOnly {
		t.Errorf("Sprint record = %+v", rec)
	}
	var f struct {
		Cooldown string
		Costs    []struct {
			Enabled    string
			StatCosts  []struct{ Value string }
			ChargeCost string
		}
		AbilityName struct{ Source string }
		HitsIds     []struct{ Key int }
		EffectsIds  []struct {
			Key   int
			Value struct{ Guid int64 }
		}
	}
	if err := json.Unmarshal(rec.Fields, &f); err != nil {
		t.Fatal(err)
	}
	if f.Cooldown != "0" || len(f.Costs) != 1 || f.Costs[0].Enabled != "IsMoving(GetOwner())" || f.AbilityName.Source != "Sprint" ||
		len(f.Costs[0].StatCosts) != 1 || !strings.Contains(f.Costs[0].StatCosts[0].Value, "var Cost = 1.25;") ||
		len(f.EffectsIds) != 1 || f.EffectsIds[0].Value.Guid != 6064631641826394197 {
		t.Errorf("Sprint fields = %s", rec.Fields)
	}
	// A table the slice does not otherwise read is there too: an Item-free
	// check, any row of the largest decoded table.
	var guid int64
	var tid int64
	if err := db.SQL().QueryRow(`SELECT type_id, guid FROM record WHERE table_index = 230 LIMIT 1`).Scan(&tid, &guid); err != nil {
		t.Fatal(err)
	}
	if r, err := db.Record(uint64(tid), guid); err != nil || r.Schema != "PopulationInstanceRecordBase" || !json.Valid(r.Fields) {
		t.Errorf("a PopulationInstance record = %+v, %v", r, err)
	}
	// The stat row of Stamina, from record and from stat_def.
	srec, err := db.Record(content.TypeStatDef, content.GuidStamina)
	if err != nil {
		t.Fatal(err)
	}
	var sf struct {
		ValueMax    string
		RegenRate   string
		Replication int
	}
	if err := json.Unmarshal(srec.Fields, &sf); err != nil {
		t.Fatal(err)
	}
	s, _ := db.StatDef(content.GuidStamina)
	if sf.ValueMax != s.MaxExpr || sf.RegenRate != s.RegenExpr || sf.Replication != s.Replication {
		t.Errorf("Stamina record %+v differs from stat_def %+v", sf, s)
	}
}

// dump hashes every table of a content.db in a fixed order, except the
// meta rows that hold the time or the duration of the run.
func dump(t *testing.T, db *content.DB) map[string]string {
	t.Helper()
	rows, err := db.SQL().Query(`SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name`)
	if err != nil {
		t.Fatal(err)
	}
	var tables []string
	for rows.Next() {
		var n string
		if err := rows.Scan(&n); err != nil {
			t.Fatal(err)
		}
		tables = append(tables, n)
	}
	rows.Close()
	sort.Strings(tables)
	out := map[string]string{}
	for _, table := range tables {
		query := `SELECT * FROM ` + table + ` ORDER BY rowid`
		switch table {
		case "meta":
			query = `SELECT * FROM meta WHERE key NOT IN ('built_at', 'build_seconds', 'source.dbc.path', 'source.usmap.path') ORDER BY key`
		default:
			// The tables WITHOUT ROWID are read in primary key order.
			if _, err := db.SQL().Exec(`SELECT rowid FROM ` + table + ` LIMIT 0`); err != nil {
				query = `SELECT * FROM ` + table
			}
		}
		r, err := db.SQL().Query(query)
		if err != nil {
			t.Fatalf("%s: %v", query, err)
		}
		cols, _ := r.Columns()
		h := sha256.New()
		n := 0
		for r.Next() {
			vals := make([]any, len(cols))
			ptrs := make([]any, len(cols))
			for i := range vals {
				ptrs[i] = &vals[i]
			}
			if err := r.Scan(ptrs...); err != nil {
				t.Fatal(err)
			}
			for _, v := range vals {
				switch x := v.(type) {
				case []byte:
					h.Write(x)
				case string:
					h.Write([]byte(x))
				default:
					fmt.Fprintf(h, "%v", x)
				}
				h.Write([]byte{0})
			}
			n++
		}
		r.Close()
		out[table] = fmt.Sprintf("%d rows, %x", n, h.Sum(nil)[:8])
	}
	return out
}

// ~ Cyb :: C O A ~

// TestRealDeterministic: building twice gives the same contents in every table
// (meta's time and path rows aside). The gzip bytes of record.data are compared
// too, and they are the same because both builds ran on one Go toolchain: the
// compress/flate output is deterministic for a given Go version, not promised
// across versions, so a content.db built with another toolchain has the same rows
// but may have other bytes.
func TestRealDeterministic(t *testing.T) {
	dbc, usmap, config := realInputs(t)
	first := builtDB(t)

	dir := t.TempDir()
	path := filepath.Join(dir, "content.db")
	var stdout, stderr bytes.Buffer
	args := []string{"--dbc", dbc, "--usmap", usmap, "--out", path}
	if config != "" {
		args = append(args, "--config", config)
	}
	if code := run(args, &stdout, &stderr); code != 0 {
		t.Fatalf("second build exited %d: %s", code, stderr.String())
	}
	second, err := content.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer second.Close()

	a, b := dump(t, first), dump(t, second)
	for table := range a {
		if a[table] != b[table] {
			t.Errorf("table %s differs between two builds: %s, %s", table, a[table], b[table])
		}
	}
	if len(a) != len(b) || len(a) < 20 {
		t.Errorf("%d and %d tables compared", len(a), len(b))
	}
	t.Logf("compared %d tables, e.g. record: %s", len(a), a["record"])

	// Re-running over an existing output replaces it (the rename), and a
	// failed run leaves it alone.
	before, _ := os.ReadFile(path)
	badArgs := []string{"--dbc", dbc, "--usmap", filepath.Join(dir, "no-such.usmap"), "--out", path}
	if code := run(badArgs, &stdout, &stderr); code == 0 {
		t.Error("a build with a missing usmap succeeded")
	}
	after, _ := os.ReadFile(path)
	if !bytes.Equal(before, after) {
		t.Error("a failed build changed the existing output")
	}
	if _, err := os.Stat(path + ".tmp"); err == nil {
		t.Error("a failed build left its temporary file")
	}
}

// TestRealRejectsACorruptDesignDatabase: one flipped byte in the middle of the
// file (a table's rows, so the structure still parses) fails the database's
// own checksums, and the build stops with exit 1 and writes nothing.
func TestRealRejectsACorruptDesignDatabase(t *testing.T) {
	dbc, usmap, _ := realInputs(t)
	data, err := os.ReadFile(dbc)
	if err != nil {
		t.Fatal(err)
	}
	data[len(data)/2] ^= 0x40
	dir := t.TempDir()
	corrupt := filepath.Join(dir, "CacheDB.dbc")
	if err := os.WriteFile(corrupt, data, 0o644); err != nil {
		t.Fatal(err)
	}
	out := filepath.Join(dir, "out", "content.db")
	var stdout, stderr bytes.Buffer
	code := run([]string{"--dbc", corrupt, "--usmap", usmap, "--out", out}, &stdout, &stderr)
	if code != 1 || !strings.Contains(stderr.String(), "fails its own checksums") {
		t.Errorf("exit %d, stderr %q; want exit 1 and the checksum failure", code, stderr.String())
	}
	for _, p := range []string{out, out + ".tmp"} {
		if _, err := os.Stat(p); err == nil {
			t.Errorf("%s was written", p)
		}
	}
}

// TestRealRejectsAPatchThatFitsNothing: a mappings patch whose struct or anchor
// the mappings lack stops the build, and the error names the patch.
func TestRealRejectsAPatchThatFitsNothing(t *testing.T) {
	dbc, usmap, _ := realInputs(t)
	boolProp := designdb.Patches()[0].Prop
	noSuchProp := boolProp
	noSuchProp.Name = "NoSuchProp"
	for _, tt := range []struct {
		patch designdb.Patch
		want  string
	}{
		// (bMayCorrupt itself is already present in the generated usmap, so the
		// property that fits nothing is one neither usmap has.)
		{designdb.Patch{Struct: "AbilityHitRecordBase", After: "NoSuchProperty", Prop: noSuchProp, Why: "test"},
			"AbilityHitRecordBase.NoSuchProp after NoSuchProperty: anchor missing"},
		{designdb.Patch{Struct: "NoSuchRecordBase", After: "X", Prop: boolProp, Why: "test"},
			"NoSuchRecordBase.bMayCorrupt after X: struct missing"},
	} {
		out := filepath.Join(t.TempDir(), "content.db")
		_, err := Build(context.Background(), Options{DBC: dbc, USMap: usmap, Out: out, Encoding: content.EncodingJSON,
			Patches: []designdb.Patch{tt.patch}})
		if err == nil || !strings.Contains(err.Error(), tt.want) {
			t.Errorf("patch %v: Build = %v, want an error containing %q", tt.patch, err, tt.want)
		}
		if _, err := os.Stat(out); err == nil {
			t.Errorf("patch %v: content.db was written", tt.patch)
		}
	}
}

// TestRealClassStatBaseMatchesTheGoldenData compares content.db with the
// golden data (golden_data_test.go): the six attribute curves, the 48 formulas
// and every one of the 2400 values of class_stat_base, with an arithmetic done
// from the golden numbers alone.
func TestRealClassStatBaseMatchesTheGoldenData(t *testing.T) {
	db := builtDB(t)
	// The curves: Guid, name and the 50 keys.
	for _, c := range goldenCurves {
		var name string
		var keys int
		if err := db.SQL().QueryRow(`SELECT name, key_count FROM stat_curve_def WHERE guid = ?`, c.guid).Scan(&name, &keys); err != nil || name != c.name || keys != 50 {
			t.Errorf("curve %d = %q with %d keys, %v; want %s with 50", c.guid, name, keys, err, c.name)
			continue
		}
		for i, want := range goldenCurveValues {
			var level, value float64
			var interp string
			if err := db.SQL().QueryRow(`SELECT level, value, interp FROM stat_curve WHERE guid = ? AND ord = ?`, c.guid, i).Scan(&level, &value, &interp); err != nil ||
				level != float64(i+1) || value != want || interp != "Linear" {
				t.Errorf("curve %s key %d = (%v, %v, %s), %v; want (%d, %v, Linear)", c.name, i, level, value, interp, err, i+1, want)
				break
			}
		}
	}
	// The formulas and the values.
	checked := 0
	for _, f := range goldenFormulas {
		listGuid := scalarInt(t, db, `SELECT stat_init_list_guid FROM class WHERE name = ?`, f.class)
		statGuid := scalarInt(t, db, `SELECT guid FROM stat_def WHERE name = ?`, f.stat)
		var expr string
		if err := db.SQL().QueryRow(`SELECT expression FROM stat_init WHERE list_guid = ? AND stat_guid = ?`, listGuid, statGuid).Scan(&expr); err != nil {
			t.Errorf("%s %s: %v", f.class, f.stat, err)
			continue
		}
		if want := formula(f.curve, f.multiplier); expr != want {
			t.Errorf("%s %s: formula %q, golden %q", f.class, f.stat, expr, want)
		}
		for level := 1; level <= 50; level++ {
			want64, want32, _ := goldenValue(f.multiplier, level)
			var got float64
			var amb bool
			if err := db.SQL().QueryRow(`SELECT value, rounding_ambiguous FROM class_stat_base WHERE class_name = ? AND stat_name = ? AND level = ?`,
				f.class, f.stat, level).Scan(&got, &amb); err != nil {
				t.Fatalf("%s %s level %d: %v", f.class, f.stat, level, err)
			}
			if got != want64 || amb != (want64 != want32) {
				t.Errorf("%s %s level %d = %v (ambiguous %v); golden %v, float32 %v", f.class, f.stat, level, got, amb, want64, want32)
			}
			checked++
		}
	}
	if checked != 2400 || len(goldenFormulas) != 48 {
		t.Errorf("%d values of %d formulas compared, want 2400 of 48", checked, len(goldenFormulas))
	}
}

// (CoA ~ C:Y:B)

// TestRealAbilityRuntime: the runtime tables worldd reads for Sprint, Dodge and Rest, resolved from the real rows.
func TestRealAbilityRuntime(t *testing.T) {
	db := builtDB(t)
	abilities, err := db.AbilityRuntimes()
	if err != nil {
		t.Fatal(err)
	}
	if len(abilities) != 3 {
		t.Fatalf("%d ability_runtime rows, want 3: %+v", len(abilities), abilities)
	}
	sprint, dodge, rest := abilities[content.GuidSprint], abilities[content.GuidDodge], abilities[content.GuidRest]
	// Sprint: held while pressed, needs 1 Stamina, takes 1.25 per 0.25 s only while moving (5 a second), no cooldown.
	if sprint.Name != "General_Sprint" || sprint.HeldBehavior != "Smart" || sprint.RequiresOutOfCombat || sprint.MinStamina != 1 ||
		sprint.CostStatGuid != content.GuidStamina || sprint.CostAmount != 1.25 || !sprint.CostRequiresMoving || sprint.ChargeInterval != 0.25 ||
		sprint.Cooldown != 0 || sprint.TriggersCooldown || sprint.DisableDuringCooldown || sprint.TargetsSelf ||
		sprint.EffectGuid != 6064631641826394197 || sprint.CancelSection != "End" ||
		sprint.MontagePath != "/Game/Characters/Animations/00_ABLT/CM_M_ABLT_Sprint_Montage.CM_M_ABLT_Sprint_Montage" {
		t.Errorf("Sprint = %+v", sprint)
	}
	// Dodge: 50 Stamina once, a 0.65 s cooldown that the paid cost starts and that refuses a second dodge.
	if dodge.Name != "General_Dodge" || dodge.HeldBehavior != "Naive" || dodge.RequiresOutOfCombat || dodge.MinStamina != 0 ||
		dodge.CostStatGuid != content.GuidStamina || dodge.CostAmount != 50 || dodge.CostRequiresMoving || dodge.ChargeInterval != 0 ||
		dodge.Cooldown != 0.65 || !dodge.TriggersCooldown || !dodge.DisableDuringCooldown || dodge.TargetsSelf ||
		dodge.EffectGuid != 106223481149050 {
		t.Errorf("Dodge = %+v", dodge)
	}
	// Rest: a toggle, out of combat only, free, no cooldown; it names the caster as its target and ends by jumping to SitEnd.
	if rest.Name != "General_Rest" || rest.HeldBehavior != "Toggle" || !rest.RequiresOutOfCombat || rest.CostStatGuid != 0 ||
		rest.CostAmount != 0 || rest.Cooldown != 0 || !rest.TargetsSelf || rest.EffectGuid != 109394092054139 ||
		rest.CancelSection != "SitEnd" ||
		rest.MontagePath != "/Game/Characters/Animations/00_ABLT/CM_M_ABLT_General_Rest_Montage.CM_M_ABLT_General_Rest_Montage" {
		t.Errorf("Rest = %+v", rest)
	}
	for _, a := range abilities {
		if a.Source == "" {
			t.Errorf("%s has no source", a.Name)
		}
	}

	effects, err := db.EffectRuntimes()
	if err != nil {
		t.Fatal(err)
	}
	// The Sprint effect lasts until it is ended and stops regeneration while moving, then its End hit applies the
	// exertion effect (1 s without regeneration); the Evasion lasts 0.5 s and stops regeneration, then the same exertion;
	// Rest is infinite and touches no regeneration of stamina.
	const exertion = 6064631668308443138
	want := map[int64]content.EffectRuntime{
		6064631641826394197: {Name: "General_Sprint", Infinite: true, TickTimer: 1, StopsRegenMoving: true, EndEffectGuid: exertion, EndEffectDuration: 1},
		106223481149050:     {Name: "General_Dodge_Evasion", Duration: 0.5, StopsRegenStanding: true, StopsRegenMoving: true, EndEffectGuid: exertion, EndEffectDuration: 1},
		109394092054139:     {Name: "General_Rest", Infinite: true},
		exertion:            {Name: "General_Stamina_DisableRegen", Duration: 1, StopsRegenStanding: true, StopsRegenMoving: true},
	}
	if len(effects) != len(want) {
		t.Errorf("%d effect_runtime rows, want %d: %+v", len(effects), len(want), effects)
	}
	for g, w := range want {
		got := effects[g]
		got.Source = ""
		w.EffectGuid = g
		if got != w {
			t.Errorf("effect %d = %+v, want %+v", g, got, w)
		}
	}

	mods, err := db.EffectStatRuntimes()
	if err != nil {
		t.Fatal(err)
	}
	// Rest adds 0.01 x MaxHealth + 1 to HealthRegeneration and 0.01 x MaxMana + 1 to ManaRegeneration: with the captured
	// Bard's 90 each that is 1.9 on 0.6, the 2.5 the original server sent (pkt#8228).
	rm := mods[109394092054139]
	if len(rm) != 2 || rm[0].StatName != "HealthRegeneration" || rm[0].SourceStatName != "MaxHealth" || rm[0].Factor != 0.01 || rm[0].Addend != 1 ||
		rm[1].StatName != "ManaRegeneration" || rm[1].SourceStatName != "MaxMana" || rm[1].Factor != 0.01 || rm[1].Addend != 1 {
		t.Errorf("Rest stat mods = %+v", rm)
	}
	if sm := mods[6064631641826394197]; len(sm) != 1 || sm[0].StatName != "MoveSpeedMult" || sm[0].SourceStatGuid != 0 || sm[0].Addend != 0.2 {
		t.Errorf("Sprint stat mods = %+v", sm)
	}
	if em := mods[106223481149050]; len(em) != 2 || em[0].Addend != 1 || em[1].Addend != 1 {
		t.Errorf("Evasion stat mods = %+v", em)
	}
}
