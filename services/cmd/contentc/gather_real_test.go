// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

// The gathering tables on the real design database (the shared build of real_test.go), against what the Gathering research found in the data and in the
// retail captures, and the profession level-up curves of mapc's artisan_curves.tsv. They need dist/content/artisan_curves.tsv (`mapc artisan-curves`) beside
// the checkout's content.db; without it the build keeps the placeholder curves and TestRealGatherCurves says so.

import (
	"encoding/json"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"testing"

	"aocrop/services/internal/aocexpr"
	"aocrop/services/internal/content"
)

// repoFile is a path under the checkout (the tests run in services/cmd/contentc).
func repoFile(parts ...string) string {
	return filepath.Join(append([]string{"..", "..", ".."}, parts...)...)
}

// The slots of the tools (EEquipSlot, equip_slot.slot).
const (
	slotPickaxe, slotAxe, slotSickle, slotFishingRod, slotBow = 26, 29, 33, 35, 38
)

// [C-Y-B x CoA]

func TestRealGatherTables(t *testing.T) {
	db := builtDB(t)
	meta, err := db.Meta()
	if err != nil {
		t.Fatal(err)
	}
	count := func(q string, args ...any) int64 {
		t.Helper()
		return scalarInt(t, db, q, args...)
	}
	// the research's counts of the records
	for table, want := range map[string]int64{"gather_def": 339, "gather_dynamic": 136, "gather_tool": 87, "item_variation": 47, "profession": 22, "certification_level": 5,
		"action_certification": 98} {
		if got := count(`SELECT COUNT(*) FROM ` + table); got != want {
			t.Errorf("%s has %d rows, want %d", table, got, want)
		}
		if meta["typed."+table] != strconv.FormatInt(want, 10) {
			t.Errorf("meta typed.%s = %q, want %d", table, meta["typed."+table], want)
		}
	}
	if got := count(`SELECT COUNT(*) FROM gather_dynamic WHERE season = 0 AND day_mult = 0.75 AND night_mult = 0.75`); got != 4+1 { // the Winter 0.75 of hemlock, larch, oak and the coconut
		t.Logf("%d seasonal defs have a Winter of 0.75", got)
	}

	// All 339 defs: all three joins are there, in the three gathering professions, and the numbers are the research's.
	defs, err := db.GatherDefs()
	if err != nil || len(defs) != 339 {
		t.Fatalf("GatherDefs = %d, %v", len(defs), err)
	}
	type key struct {
		prof string
		tier int
	}
	byKey := map[key]int{}
	profName := map[int64]string{}
	profs, err := db.Professions()
	if err != nil || len(profs) != 22 {
		t.Fatalf("Professions = %d, %v", len(profs), err)
	}
	for _, p := range profs {
		profName[p.Guid] = p.Name
	}
	bySlot := map[int]int{}
	byName := map[string]content.GatherDef{}
	byGuid := map[int64]content.GatherDef{}
	for _, d := range defs {
		if d.FoliageGuid == 0 || d.InteractionGuid == 0 || d.GatherGuid == 0 || d.RewardGuid == 0 || d.ProfessionGuid == 0 || d.CertGuid == 0 || d.DurationExpr == 0 || d.DurabilityExpr == 0 ||
			d.InteractRadius <= 0 || d.BaseDuration <= 0 {
			t.Errorf("%s is missing a join: %+v", d.Name, d)
		}
		byKey[key{profName[d.ProfessionGuid], d.CertTier}]++
		bySlot[d.ToolSlot]++
		byName[d.Name] = d
		byGuid[d.Guid] = d
		var loot int
		if err := db.SQL().QueryRow(`SELECT COUNT(*) FROM loot_table WHERE guid = ?`, d.RewardGuid).Scan(&loot); err != nil || loot != 1 {
			t.Errorf("%s: reward table %d is not in loot_table (%d, %v)", d.Name, d.RewardGuid, loot, err)
		}
	}
	// Mining 156, Lumberjacking 101, Herbalism 82 (no Master node anywhere, no GrandMaster for Mining and Lumberjacking: the garden's gaps)
	for k, want := range map[key]int{{"Mining", 0}: 59, {"Mining", 1}: 47, {"Mining", 2}: 50, {"Lumberjacking", 0}: 52, {"Lumberjacking", 1}: 16, {"Lumberjacking", 2}: 33,
		{"Herbalism", 0}: 35, {"Herbalism", 1}: 19, {"Herbalism", 2}: 27, {"Herbalism", 4}: 1} {
		if byKey[k] != want {
			t.Errorf("%v: %d defs, want %d", k, byKey[k], want)
		}
	}
	if len(byKey) != 10 || bySlot[slotPickaxe] != 156 || bySlot[slotAxe] != 101 || bySlot[slotSickle] != 82 || len(bySlot) != 3 {
		t.Errorf("tiers %v, tool slots %v", byKey, bySlot)
	}
	// E1's captured nodes (research evidence.md section 1.3): trees base 15 s, radius 250, tolerance 500, EmptyDuration 300, Override 14400; stone base 15 (Large) / 7.5 (Medium),
	// radius 300, Empty 840, Override 0; plants base 11.25, radius 250, tolerance 0, Empty 1800, Override 14400
	for name, want := range map[string][8]float64{ // base, radius, tolerance, empty, override, slot, tier, combat
		"Tree_Oak_Large_2":          {15, 250, 500, 300, 14400, slotAxe, 0, 1},
		"Tree_WesternLarch_Large_1": {15, 250, 500, 300, 14400, slotAxe, 0, 1},
		"Stone_Granite_Large_1":     {15, 300, 500, 840, 0, slotPickaxe, 0, 1},
		"Stone_Basalt_Medium_3":     {7.5, 300, 500, 840, 0, slotPickaxe, 0, 1},
		"Plant_Daffodil_Medium_1":   {11.25, 250, 0, 1800, 14400, slotSickle, 0, 1},
		"Plant_Flax_Medium_2":       {11.25, 250, 0, 1800, 14400, slotSickle, 0, 1},
	} {
		d, ok := byName[name]
		if !ok {
			t.Errorf("no def %s", name)
			continue
		}
		got := [8]float64{d.BaseDuration, d.InteractRadius, d.InteractTolerance, d.EmptyDuration, d.OverrideRespawn, float64(d.ToolSlot), float64(d.CertTier), float64(b2i(d.AllowInCombat))}
		if got != want {
			t.Errorf("%s = %v, want %v", name, got, want)
		}
	}
	if d := byName["Tree_Oak_Large_2"]; d.Guid != 6064632138726178834 || d.InteractionGuid != 6064632138648518658 || d.FoliageGuid != 109183214311032 || d.GatherGuid != 6064629528354095104 ||
		d.RewardGuid != 6064629013188968448 || d.GatherableState != "Juvenile" || profName[d.ProfessionGuid] != "Lumberjacking" || d.DynamicGuid != 6064631436954501123 {
		t.Errorf("Tree_Oak_Large_2 = %+v", d)
	}

	// The 439 placed nodes (fixtures/region/node_picks.json): their 58 defs are all there, with the tiers the research counted.
	raw, err := os.ReadFile(repoFile("fixtures", "region", "node_picks.json"))
	if err != nil {
		t.Skipf("no node picks: %v", err)
	}
	var picks struct {
		Nodes []struct {
			Definition int64 `json:"definition"`
		} `json:"nodes"`
	}
	if err := json.Unmarshal(raw, &picks); err != nil || len(picks.Nodes) != 439 {
		t.Fatalf("node_picks.json: %d nodes, %v", len(picks.Nodes), err)
	}
	placed := map[int64]bool{}
	placedBy := map[key]int{}
	for _, n := range picks.Nodes {
		d, ok := byGuid[n.Definition]
		if !ok {
			t.Errorf("a placed node's def %d is not in gather_def", n.Definition)
			continue
		}
		placed[n.Definition] = true
		placedBy[key{profName[d.ProfessionGuid], d.CertTier}]++
	}
	if len(placed) != 58 {
		t.Errorf("the 439 placed nodes use %d defs, want 58", len(placed))
	}
	for k, want := range map[key]int{{"Herbalism", 0}: 138, {"Herbalism", 1}: 21, {"Herbalism", 2}: 6, {"Lumberjacking", 0}: 155, {"Lumberjacking", 1}: 13, {"Lumberjacking", 2}: 2,
		{"Mining", 0}: 73, {"Mining", 1}: 12, {"Mining", 2}: 19} {
		if placedBy[k] != want {
			t.Errorf("placed nodes %v: %d, want %d", k, placedBy[k], want)
		}
	}
	// the GM garden's 10 (server/worldd/spawns/garden_defs.h)
	header, err := os.ReadFile(repoFile("server", "worldd", "spawns", "garden_defs.h"))
	if err != nil {
		t.Skipf("no garden defs: %v", err)
	}
	rowRe := regexp.MustCompile(`\{"(\w+)", (\d), "(\w+)", (\d+)ull, \d+ull, (\d+)ull, "(\w+)", "(\w+)", "(\w+)", ([\d.]+), \d+\}`)
	garden := rowRe.FindAllStringSubmatch(string(header), -1)
	if len(garden) != 10 {
		t.Fatalf("garden_defs.h has %d defs, want 10", len(garden))
	}
	slotNumber := map[string]int{"Pickaxe": slotPickaxe, "Axe": slotAxe, "Sickle": slotSickle}
	for _, g := range garden {
		guid, _ := strconv.ParseInt(g[4], 10, 64)
		interaction, _ := strconv.ParseInt(g[5], 10, 64)
		tier, _ := strconv.Atoi(g[2])
		base, _ := strconv.ParseFloat(g[9], 64)
		d, ok := byGuid[guid]
		if !ok || d.Name != g[6] || d.CertTier != tier || profName[d.ProfessionGuid] != g[1] || d.InteractionGuid != interaction || d.ToolSlot != slotNumber[g[8]] || d.BaseDuration != base {
			t.Errorf("the garden's %s: gather_def has %+v", g[6], d)
		}
	}

	// The tools: 87 items, each with the tool data it names; the item's own requirement is the tool data's for 86 of them (MA_0005B_Thanacite_Pickaxe names none).
	slotCount := map[int]int{}
	rows, err := db.SQL().Query(`SELECT tool_slot, COUNT(*) FROM gather_tool GROUP BY tool_slot`)
	if err != nil {
		t.Fatal(err)
	}
	for rows.Next() {
		var slot, n int
		rows.Scan(&slot, &n)
		slotCount[slot] = n
	}
	rows.Close()
	// Pickaxe 19, Bow 16, Sickle 15, Axe 15, FishingRod 15, FishingLure (74) 6, Trap (40) 1
	if want := map[int]int{slotPickaxe: 19, slotBow: 16, slotSickle: 15, slotAxe: 15, slotFishingRod: 15, 74: 6, 40: 1}; len(slotCount) != len(want) {
		t.Errorf("tool slots %v, want %v", slotCount, want)
	} else {
		for s, n := range want {
			if slotCount[s] != n {
				t.Errorf("tool slot %d: %d tools, want %d", s, slotCount[s], n)
			}
		}
	}
	if got := count(`SELECT COUNT(*) FROM gather_tool WHERE required_profession = profession_guid AND required_cert = cert_guid`); got != 86 {
		t.Errorf("%d tools carry the tool data's own requirement, want 86", got)
	}
	if got := count(`SELECT COUNT(*) FROM gather_tool WHERE required_profession = 0 AND required_cert = 0 AND item_name = 'MA_0005B_Thanacite_Pickaxe'`); got != 1 {
		t.Errorf("the Thanacite pickaxe: %d rows, want 1", got)
	}
	// every profession has a tool at every tier (Novice to GrandMaster), as many as the data gives
	for prof, want := range map[string][5]int64{"Mining": {5, 5, 7, 1, 1}, "Lumberjacking": {5, 3, 5, 1, 1}, "Herbalism": {5, 3, 5, 1, 1}, "Fishing": {7, 5, 7, 1, 1}, "Hunting": {6, 4, 5, 1, 1}} {
		for tier, n := range want {
			if got := count(`SELECT COUNT(*) FROM gather_tool t JOIN profession p ON p.guid = t.profession_guid WHERE p.name = ? AND t.cert_tier = ?`, prof, tier); got != n {
				t.Errorf("%s tier %d: %d tools, want %d", prof, tier, got, n)
			}
		}
	}
	if got := count(`SELECT tool_slot FROM gather_tool WHERE item_name = 'Gear_Artisan_Mining_Tool_Pickaxe_T1_0'`); got != slotPickaxe {
		t.Errorf("the T1 pickaxe is worn in slot %d, want %d", got, slotPickaxe)
	}

	// The professions: 5 gathering, 8 crafting, 9 processing; each names a leveling type that is there.
	cats := map[string]int{}
	for _, p := range profs {
		cats[p.Category]++
		if got := count(`SELECT COUNT(*) FROM leveling_type WHERE guid = ?`, p.LevelingTypeGuid); got != 1 {
			t.Errorf("%s: leveling type %d is not in leveling_type", p.Name, p.LevelingTypeGuid)
		}
		if p.SkillTreeGuid == 0 {
			t.Errorf("%s has no skill tree", p.Name)
		}
	}
	if cats["Gathering"] != 5 || cats["Crafting"] != 8 || cats["Processing"] != 9 || len(cats) != 3 {
		t.Errorf("categories %v", cats)
	}
	for name, guid := range map[string]int64{"Mining": 108752769996425, "Lumberjacking": 108752769996426, "Herbalism": 108752770061963, "Fishing": 108752770061964, "Hunting": 108752770061965} {
		var got int64
		for _, p := range profs {
			if p.Name == name {
				got = p.Guid
			}
		}
		if got != guid {
			t.Errorf("%s has guid %d, want %d", name, got, guid)
		}
	}
	// the certification levels: the caps of the tiers (the user's settled rule: Novice has none)
	certs, err := db.CertificationLevels()
	if err != nil || len(certs) != 5 {
		t.Fatalf("CertificationLevels = %d, %v", len(certs), err)
	}
	for i, want := range []content.CertificationLevel{
		{Guid: 108906339653240, Name: "Novice", Tier: 0, MinLevel: 1, MaxLevel: 10},
		{Guid: 108906340767353, Name: "Apprentice", Tier: 1, MinLevel: 10, MaxLevel: 20, HasMaxAllowable: true, MaxAllowable: 5},
		{Guid: 108906342012538, Name: "Journeyman", Tier: 2, MinLevel: 20, MaxLevel: 30, HasMaxAllowable: true, MaxAllowable: 4},
		{Guid: 108906343061115, Name: "Master", Tier: 3, MinLevel: 30, MaxLevel: 40, HasMaxAllowable: true, MaxAllowable: 3},
		{Guid: 108906343782012, Name: "GrandMaster", Tier: 4, MinLevel: 40, MaxLevel: 50, HasMaxAllowable: true, MaxAllowable: 2}} {
		if certs[i] != want {
			t.Errorf("certification %d = %+v, want %+v", i, certs[i], want)
		}
	}
	// the promote and demote actions
	if got := count(`SELECT COUNT(*) FROM action_certification WHERE action = 'Promote'`); got != 70 {
		t.Errorf("%d promote actions, want 70", got)
	}
	if got := count(`SELECT COUNT(*) FROM action_certification WHERE action = 'Demote'`); got != 28 {
		t.Errorf("%d demote actions, want 28", got)
	}
	if got := count(`SELECT COUNT(*) FROM action_certification a JOIN profession p ON p.guid = a.profession_guid`); got != 98 {
		t.Errorf("%d of the 98 actions name a profession of the table", got)
	}
	if got := count(`SELECT COUNT(*) FROM action_certification a JOIN action_response r ON r.guid = a.action_guid WHERE a.name LIKE 'GM_Scroll_ArtinsanshipCert_%'`); got < 22 {
		t.Errorf("only %d of the GM scrolls' promote actions are in action_response", got)
	}
}

// C Y B x C | O | A

func TestRealGatherCurves(t *testing.T) {
	db := builtDB(t)
	meta, err := db.Meta()
	if err != nil {
		t.Fatal(err)
	}
	if meta["artisan_curves.status"] != "applied" {
		t.Fatalf("artisan_curves.status = %q: run `mapc artisan-curves` (dist/content/artisan_curves.tsv) before building", meta["artisan_curves.status"])
	}
	if meta["artisan_curves.curves"] != "3" || meta["artisan_curves.keys"] != "150" || len(meta["artisan_curves.sha256"]) != 64 {
		t.Errorf("meta artisan_curves.* = %q %q %q", meta["artisan_curves.curves"], meta["artisan_curves.keys"], meta["artisan_curves.sha256"])
	}
	// the three StatCurves have their keys now: levels 1 to 50, 400 x level^2 XP, 5.1922868E-08 at level 1
	const processing = 6064632988845080586
	for _, guid := range []int64{6064632988842590217, processing, 6064632988846325771} {
		var n int
		if err := db.SQL().QueryRow(`SELECT key_count FROM stat_curve_def WHERE guid = ?`, guid).Scan(&n); err != nil || n != 50 {
			t.Errorf("curve %d has %d keys (%v), want 50", guid, n, err)
		}
		rows, err := db.SQL().Query(`SELECT level, value, interp FROM stat_curve WHERE guid = ? ORDER BY ord`, guid)
		if err != nil {
			t.Fatal(err)
		}
		prevLevel, prevValue := 0.0, -1.0
		keys := 0
		for rows.Next() {
			var level, value float64
			var interp string
			rows.Scan(&level, &value, &interp)
			keys++
			if level != prevLevel+1 || value <= prevValue || interp != "Linear" {
				t.Errorf("curve %d key %d: level %v value %v %s after %v / %v: not monotone", guid, keys, level, value, interp, prevLevel, prevValue)
			}
			if level >= 2 && value != 400*level*level {
				t.Errorf("curve %d: level %v needs %v XP, want %v", guid, level, value, 400*level*level)
			}
			prevLevel, prevValue = level, value
		}
		rows.Close()
		if keys != 50 {
			t.Errorf("curve %d has %d keys", guid, keys)
		}
	}
	// a threshold at every certification edge (10, 20, 30, 40, 50)
	var curve []float64
	rows, err := db.SQL().Query(`SELECT value FROM stat_curve WHERE guid = ? ORDER BY ord`, processing)
	if err != nil {
		t.Fatal(err)
	}
	for rows.Next() {
		var v float64
		rows.Scan(&v)
		curve = append(curve, v)
	}
	rows.Close()
	for level, want := range map[int]float64{10: 40000, 20: 160000, 30: 360000, 40: 640000, 50: 1000000} {
		if curve[level-1] != want {
			t.Errorf("the XP of level %d is %v, want %v", level, curve[level-1], want)
		}
	}
	// the 22 profession leveling types hold the pak curve (their own placeholder is gone), one row per level 1..50
	prof, err := db.Professions()
	if err != nil {
		t.Fatal(err)
	}
	profTypes := map[int64]bool{}
	for _, p := range prof {
		profTypes[p.LevelingTypeGuid] = true
		var n, maxLevel, first, last int64
		if err := db.SQL().QueryRow(`SELECT COUNT(*), MAX(level), (SELECT xp FROM leveling_curve WHERE type_guid = ? AND level = 1), (SELECT xp FROM leveling_curve WHERE type_guid = ? AND level = 50)
			FROM leveling_curve WHERE type_guid = ?`, p.LevelingTypeGuid, p.LevelingTypeGuid, p.LevelingTypeGuid).Scan(&n, &maxLevel, &first, &last); err != nil {
			t.Fatal(err)
		}
		if n != 50 || maxLevel != 50 || first != 0 || last != 1000000 {
			t.Errorf("%s: leveling type %d has %d levels to %d, %d XP at level 1 and %d at 50", p.Name, p.LevelingTypeGuid, n, maxLevel, first, last)
		}
		if got := scalarInt(t, db, `SELECT COUNT(*) FROM leveling_curve c JOIN stat_curve s ON s.guid = ? AND s.level = c.level WHERE c.type_guid = ? AND c.xp = CAST(s.value + 0.5 AS INTEGER)`,
			processing, p.LevelingTypeGuid); got != 50 {
			t.Errorf("%s: %d levels equal the Processing curve", p.Name, got)
		}
	}
	if len(profTypes) != 22 {
		t.Errorf("%d profession leveling types", len(profTypes))
	}
	// Adventuring and the other types are as they were (616 rows of 26 types before; a curve of a profession is not one of them)
	var others, check, types int64
	var in []string
	for g := range profTypes {
		in = append(in, strconv.FormatInt(g, 10))
	}
	if err := db.SQL().QueryRow(`SELECT COUNT(*), SUM(level * 7 + xp), COUNT(DISTINCT type_guid) FROM leveling_curve WHERE type_guid NOT IN (`+strings.Join(in, ",")+`)`).Scan(&others, &check, &types); err != nil {
		t.Fatal(err)
	}
	if others != 616 || check != 2233116504 || types != 26 {
		t.Errorf("the other leveling curves: %d rows (%d) of %d types; want 616 (2233116504) of 26", others, check, types)
	}

	// The scroll formula: `xp - cur + 1` with xp = EvalCurve(curve, N) and cur = EvalCurve(curve, level) lands a level 1 character on level N.
	keys, err := db.SQL().Query(`SELECT level, value, interp FROM stat_curve WHERE guid = ? ORDER BY ord`, processing)
	if err != nil {
		t.Fatal(err)
	}
	var ks []aocexpr.Key
	for keys.Next() {
		var k aocexpr.Key
		var interp string
		keys.Scan(&k.Time, &k.Value, &interp)
		k.Interp, _ = aocexpr.ParseInterp(interp)
		ks = append(ks, k)
	}
	keys.Close()
	c, err := aocexpr.NewCurve(ks)
	if err != nil {
		t.Fatal(err)
	}
	levelOf := func(xp float64) int {
		level := 1
		for l := 2; l <= 50; l++ {
			if int64(xp) >= int64(curve[l-1]) {
				level = l
			}
		}
		return level
	}
	for _, target := range []float64{9, 15, 25} {
		want, _ := c.Eval(target)
		cur, _ := c.Eval(1)
		xp := float64(float32(want - cur + 1)) // the program's value is a float32
		if got := levelOf(xp); float64(got) != target {
			t.Errorf("the Level %v scroll's %v XP is level %d", target, xp, got)
		}
	}
	// The gathering curves the data holds itself (stat_curve): their keys are there
	for name, want := range map[string]int64{"GatheringSpeed": 12, "GatheringRarityChance": 11, "GatheringQuantityChance": 11, "GatheringQuantityIncrease_Common": 12,
		"GatheringQuantityIncrease_Uncommon": 12, "GatheringQuantityIncrease_Rare": 12, "GatheringQuantityIncrease_Heroic": 12, "GatheringQuantityIncrease_Epic": 12,
		"GatheringQuantityIncrease_Legendary": 12, "GatheringXP": 11, "GatheringExpertiseXP": 11} {
		if got := scalarInt(t, db, `SELECT key_count FROM stat_curve_def WHERE name = ?`, name); got != want {
			t.Errorf("curve %s has %d keys, want %d", name, got, want)
		}
	}
	// GatheringSpeed: 1.0 at rating 0 and 0.5 at 8409, and 0.927 at about 215 (the speed factor the captures measured: 11.13 s of 11.25)
	var speed []aocexpr.Key
	srows, err := db.SQL().Query(`SELECT level, value, interp FROM stat_curve WHERE name = 'GatheringSpeed' ORDER BY ord`)
	if err != nil {
		t.Fatal(err)
	}
	for srows.Next() {
		var k aocexpr.Key
		var interp string
		srows.Scan(&k.Time, &k.Value, &interp)
		k.Interp, _ = aocexpr.ParseInterp(interp)
		speed = append(speed, k)
	}
	srows.Close()
	sc, _ := aocexpr.NewCurve(speed)
	if v, _ := sc.Eval(0); v != 1 {
		t.Errorf("GatheringSpeed(0) = %v", v)
	}
	if v, _ := sc.Eval(8409); v != 0.5 {
		t.Errorf("GatheringSpeed(8409) = %v", v)
	}
	if v, _ := sc.Eval(215); v < 0.9265 || v > 0.9275 {
		t.Errorf("GatheringSpeed(215) = %v, want 0.927", v)
	}
}

// (C O A) (C Y B)

func TestRealGatherExpressions(t *testing.T) {
	db := builtDB(t)
	// None of the 24 expressions that failed on the five gathering reads fails now (ab_expr ids are the order the build met the texts in; the gathering
	// ones are appended after the reward tables', so the older ids did not move)
	for _, id := range []int{1312, 1313, 1314, 1315, 1321, 1355, 1356, 1357, 1358, 1359, 1360, 1561, 1562, 1564, 1565, 1630, 1632, 1633, 1634, 1635, 1636, 1649, 1650, 1656} {
		var program int
		var errText string
		if err := db.SQL().QueryRow(`SELECT program, error FROM ab_expr WHERE id = ?`, id).Scan(&program, &errText); err != nil || program < 0 || errText != "" {
			t.Errorf("expression %d: program %d, error %q (%v)", id, program, errText, err)
		}
	}
	for _, fn := range []string{"GetArtisanshipModifiedStat", "GetRolledItemQuality", "GetItemRarity", "GetSourceCertificationTier", "GetGatheringInteractionBaseDuration"} {
		if got := scalarInt(t, db, `SELECT COUNT(*) FROM ab_expr WHERE error LIKE ?`, "%"+fn+"%"); got != 0 {
			t.Errorf("%d expressions still fail on %s", got, fn)
		}
	}
	if got := scalarInt(t, db, `SELECT COUNT(*) FROM ab_expr WHERE program < 0`); got != 198 {
		t.Errorf("%d expressions do not compile, want 198 (300 in version 12: 24 compile now on the gathering reads, 79 more on GetPlayerProfessionLevel(GetPlayer(), ...), and the Quality formula of the Hunting prototype variation, new in version 13, does not)", got)
	}
	// every gathering interaction's duration and durability formula compiles, as does every ItemVariation's Quality
	if got := scalarInt(t, db, `SELECT COUNT(*) FROM gather_def g JOIN ab_expr e ON e.id = g.duration_expr WHERE e.program >= 0`); got != 339 {
		t.Errorf("%d of 339 duration formulas compile", got)
	}
	if got := scalarInt(t, db, `SELECT COUNT(*) FROM gather_def g JOIN ab_expr e ON e.id = g.durability_expr WHERE e.program >= 0`); got != 339 {
		t.Errorf("%d of 339 durability formulas compile", got)
	}
	if got := scalarInt(t, db, `SELECT COUNT(*) FROM item_variation v JOIN ab_expr e ON e.id = v.quality_expr WHERE e.program >= 0`); got != 44 {
		t.Errorf("%d of the 45 Quality formulas compile, want 44 (two variations have none; the Hunting prototype's names a Term by its name)", got)
	}
	if got := scalarInt(t, db, `SELECT COUNT(*) FROM item_variation WHERE quality_expr = 0`); got != 2 {
		t.Errorf("%d variations have no Quality formula, want 2", got)
	}
	if got := scalarInt(t, db, `SELECT COUNT(*) FROM item_variation v JOIN ab_expr e ON e.id = v.quality_expr WHERE v.name LIKE 'Artisan_Gathering%' AND e.program >= 0`); got != 20 {
		t.Errorf("%d of the 21 Artisan_Gathering variations compile, want 20", got)
	}
	// the one that does not: the Hunting bow's prototype (Gathering stage 3), whose formula reads $Term:Artisan_Gathering_Rarity$ by name
	var protoErr string
	if err := db.SQL().QueryRow(`SELECT e.error FROM item_variation v JOIN ab_expr e ON e.id = v.quality_expr WHERE v.name = 'Artisan_Gathering_Hunting_Bow_Rarity_Prototype' AND e.program < 0`).Scan(&protoErr); err != nil ||
		!strings.Contains(protoErr, "$Term:Artisan_Gathering_Rarity$") {
		t.Errorf("the prototype variation: %q, %v", protoErr, err)
	}
	if got := scalarInt(t, db, `SELECT COUNT(*) FROM item_variation v JOIN ab_expr e ON e.id = v.durability_expr WHERE e.program >= 0`); got != 2 {
		t.Errorf("%d durability formulas compile, want 2 (Durability_Broken 0, Durability_Damaged 50)", got)
	}
	// the Profession experience rewards point at their expression: 88 rows of 67 tables, every one compiled
	if got := scalarInt(t, db, `SELECT COUNT(*) FROM reward_xp WHERE experience_type = 'Profession' AND expr > 0`); got != 88 {
		t.Errorf("%d of 88 Profession rewards point at an expression", got)
	}
	if got := scalarInt(t, db, `SELECT COUNT(*) FROM reward_xp x JOIN ab_expr e ON e.id = x.expr WHERE x.experience_type = 'Profession' AND e.program >= 0 AND e.text = x.expression`); got != 88 {
		t.Errorf("%d of 88 Profession reward expressions compile and equal the row's text", got)
	}
	if got := scalarInt(t, db, `SELECT COUNT(*) FROM reward_xp WHERE experience_type <> 'Profession' AND expr <> 0`); got != 0 {
		t.Errorf("%d other rewards point at an expression", got)
	}
}

// -- C-Y-B x C | O | A --

func TestReadArtisanCurves(t *testing.T) {
	const head = "curve\tord\ttime\tvalue\tinterp\tarrive_tangent\tleave_tangent\tpre_extrap\tpost_extrap\n"
	write := func(text string) string {
		p := filepath.Join(t.TempDir(), "artisan_curves.tsv")
		if err := os.WriteFile(p, []byte(text), 0o644); err != nil {
			t.Fatal(err)
		}
		return p
	}
	a, err := readArtisanCurves(write(head + "/Game/A/B\t0\t1\t5.1922868E-08\tLinear\t0\t0\tConstant\tConstant\n/Game/A/B\t1\t2\t1600\tLinear\t0\t0\tConstant\tConstant\n/Game/A/C\t0\t0\t0.1\tCubic\t1\t2\tConstant\tConstant\n"))
	if err != nil {
		t.Fatal(err)
	}
	if len(a.Curves) != 2 || a.Keys != 3 || len(a.SHA256) != 64 {
		t.Fatalf("curves %v keys %d", a.Curves, a.Keys)
	}
	if k := a.Curves["/Game/A/B"][0]; k.Time != 1 || k.Value != 5.1922868e-08 || k.Interp != aocexpr.InterpLinear {
		t.Errorf("first key = %+v", k)
	}
	if k := a.Curves["/Game/A/C"][0]; k.Value != 0.1 || k.Interp != aocexpr.InterpCubic || k.Arrive != 1 || k.Leave != 2 {
		t.Errorf("cubic key = %+v (a float32 reads back as the decimal it was typed as)", k)
	}
	// keysFor takes an object path (what ExternalCurve holds)
	if ks := a.keysFor("/Game/A/B.B"); len(ks) != 2 {
		t.Errorf("keysFor(/Game/A/B.B) = %v", ks)
	}
	if a.keysFor("/Game/A/Z.Z") != nil || a.keysFor("") != nil || (*artisanCurves)(nil).keysFor("/Game/A/B.B") != nil {
		t.Error("keysFor made keys up")
	}
	for name, text := range map[string]string{
		"no header":          "/Game/A/B\t0\t1\t0\tLinear\t0\t0\tConstant\tConstant\n",
		"a short line":       head + "/Game/A/B\t0\t1\t0\n",
		"a key out of order": head + "/Game/A/B\t1\t1\t0\tLinear\t0\t0\tConstant\tConstant\n",
		"not a number":       head + "/Game/A/B\t0\tone\t0\tLinear\t0\t0\tConstant\tConstant\n",
		"an unknown mode":    head + "/Game/A/B\t0\t1\t0\tSpline\t0\t0\tConstant\tConstant\n",
		"cycling":            head + "/Game/A/B\t0\t1\t0\tLinear\t0\t0\tCycle\tConstant\n",
		"two keys at once":   head + "/Game/A/B\t0\t1\t0\tLinear\t0\t0\tConstant\tConstant\n/Game/A/B\t1\t1\t5\tLinear\t0\t0\tConstant\tConstant\n",
		"no curves":          head,
	} {
		if _, err := readArtisanCurves(write(text)); err == nil {
			t.Errorf("%s was accepted", name)
		}
	}
	if _, err := readArtisanCurves(filepath.Join(t.TempDir(), "nope.tsv")); !os.IsNotExist(err) {
		t.Errorf("a missing file gave %v", err)
	}
}
