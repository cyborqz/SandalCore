// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"

	"aocrop/services/internal/content"
)

// fakeRoadTables is the shape of the real tables: four kinds of preset, each with an appearance per node level.
func fakeRoadTables() (map[int64]roadPreset, map[int64]roadAppearance, map[int64]int) {
	tiers := map[int64]int{9000: 0, 9001: 1, 9002: 2, 9003: 3, 9004: 4}
	apps := map[int64]roadAppearance{}
	presets := map[int64]roadPreset{}
	// Artery: T1, T1, T2, T3, T4, T4, T4 (1000 wide); Vein: none, T0, T1, T3, T4, T4, T4; Capillary: none, none, none, T2, T4, T4, T4; Lion's Hold: T2 at every level (750 wide)
	kinds := []struct {
		name   string
		width  float64
		effect []int64
	}{
		{"Artery", 1000, []int64{9001, 9001, 9002, 9003, 9004, 9004, 9004}},
		{"Vein", 600, []int64{0, 9000, 9001, 9003, 9004, 9004, 9004}},
		{"Capillary", 400, []int64{0, 0, 0, 9002, 9004, 9004, 9004}},
		{"LionsHold_Roads", 750, []int64{9002, 9002, 9002, 9002, 9002, 9002, 9002}},
	}
	next := int64(1000)
	for i, k := range kinds {
		p := roadPreset{Guid: int64(100 + i), Name: k.name}
		for level, e := range k.effect {
			next++
			apps[next] = roadAppearance{Guid: next, Name: fmt.Sprintf("%s_Level_%d", k.name, level), Width: k.width, MountEffect: e}
			p.Appearances = append(p.Appearances, next)
		}
		presets[p.Guid] = p
	}
	return presets, apps, tiers
}

func presetNamed(t *testing.T, ps map[int64]roadPreset, name string) roadPreset {
	t.Helper()
	for _, p := range ps {
		if p.Name == name {
			return p
		}
	}
	t.Fatalf("no preset %s", name)
	return roadPreset{}
}

func TestNodeLevelNumber(t *testing.T) {
	for in, want := range map[string]int{"Wilderness": 0, "wilderness": 0, "Village": 3, "Metropolis": 6, "0": 0, "4": 4} {
		if got, err := nodeLevelNumber(in); err != nil || got != want {
			t.Errorf("%q = %d, %v; want %d", in, got, err, want)
		}
	}
	for _, in := range []string{"", "Capital", "7", "-1"} {
		if _, err := nodeLevelNumber(in); err == nil {
			t.Errorf("%q is no node level but was taken", in)
		}
	}
}

// CoA + C_Y_B

// TestResolveRoadTiers holds the tier chain (preset, appearance of the node level, the Mount buff, Road_Tn_Speed_Mount) to the research's table of the real data.
func TestResolveRoadTiers(t *testing.T) {
	presets, apps, tiers := fakeRoadTables()
	want := map[string][]int{ // -1: nothing for a mount
		"Artery":          {1, 1, 2, 3, 4, 4, 4},
		"Vein":            {-1, 0, 1, 3, 4, 4, 4},
		"Capillary":       {-1, -1, -1, 2, 4, 4, 4},
		"LionsHold_Roads": {2, 2, 2, 2, 2, 2, 2},
	}
	for name, levels := range want {
		p := presetNamed(t, presets, name)
		for level, tier := range levels {
			w, why, ok := resolveRoad(p, level, apps, tiers)
			if tier < 0 {
				if ok || why == "" {
					t.Errorf("%s at level %d: gives a mount %+v, want nothing", name, level, w)
				}
				continue
			}
			if !ok || w.Tier != tier {
				t.Errorf("%s at level %d: tier %d (%v, %s), want %d", name, level, w.Tier, ok, why, tier)
			}
		}
	}
	// the width comes from the appearance
	if w, _, _ := resolveRoad(presetNamed(t, presets, "LionsHold_Roads"), 0, apps, tiers); w.Width != 750 {
		t.Errorf("Lion's Hold is %v wide, want 750", w.Width)
	}
	// a level outside the preset, an appearance that is no record, a buff that is no road effect
	p := presetNamed(t, presets, "Artery")
	if _, _, ok := resolveRoad(p, 9, apps, tiers); ok {
		t.Error("a level the preset has no appearance for resolved")
	}
	if _, _, ok := resolveRoad(roadPreset{Guid: 1, Name: "x", Appearances: []int64{424242}}, 0, apps, tiers); ok {
		t.Error("an appearance that is no record resolved")
	}
	apps[777] = roadAppearance{Guid: 777, Width: 300, MountEffect: 31337}
	if _, _, ok := resolveRoad(roadPreset{Guid: 2, Name: "y", Appearances: []int64{777}}, 0, apps, tiers); ok {
		t.Error("a mount buff that is no Road_Tn_Speed_Mount effect resolved")
	}
}

func TestResolveRoadsText(t *testing.T) {
	presets, apps, tiers := fakeRoadTables()
	lions, artery, vein := presetNamed(t, presets, "LionsHold_Roads"), presetNamed(t, presets, "Artery"), presetNamed(t, presets, "Vein")
	raw := []rawRoad{
		{Preset: lions.Guid, Active: true, Cell: "B", Actor: "a", XY: []string{"0.0,0.0", "300.0,400.0"}},
		{Preset: artery.Guid, Active: true, Cell: "A", Actor: "b", XY: []string{"10.0,10.0", "10.0,5010.0"}},
		{Preset: vein.Guid, Active: true, Cell: "A", Actor: "c", XY: []string{"0.0,0.0", "1.0,1.0"}},    // nothing for a mount at level 0
		{Preset: artery.Guid, Active: false, Cell: "A", Actor: "d", XY: []string{"0.0,0.0", "1.0,1.0"}}, // inactive
		{Preset: 4242, Active: true, Cell: "A", Actor: "e", XY: []string{"0.0,0.0", "1.0,1.0"}},         // no such preset
	}
	text, sum := resolveRoads(raw, presets, apps, tiers, 0)
	if sum.Read != 5 || sum.Kept != 2 || sum.NoWorth != 1 || sum.Inactive != 1 || sum.NoPreset != 1 {
		t.Fatalf("summary %+v", sum)
	}
	lines := strings.Split(strings.TrimSpace(text), "\n")
	if len(lines) != 3 || !strings.HasPrefix(lines[0], "# roads v1: 2 roads for a mount at node level 0 (Wilderness)") {
		t.Fatalf("file:\n%s", text)
	}
	// sorted by preset name, cell and actor: Artery before LionsHold; the half width is max(RoadWidth, 1000) / 2 (500 for a 750 wide road, 500 for a 1000 wide one)
	if lines[1] != "road\t1\t500\t10.0,10.0\t10.0,5010.0" || lines[2] != "road\t2\t500\t0.0,0.0\t300.0,400.0" {
		t.Errorf("lines:\n%s\n%s", lines[1], lines[2])
	}
	if sum.LengthKmByTier[2] < 0.0049 || sum.LengthKmByTier[2] > 0.0051 || sum.LengthKmByTier[1] < 0.0499 || sum.LengthKmByTier[1] > 0.0501 {
		t.Errorf("lengths %v", sum.LengthKmByTier)
	}
	// a wider road is half as wide again
	apps[107] = roadAppearance{Guid: 107, Width: 3000, MountEffect: 9002}
	wide := roadPreset{Guid: 5, Name: "Wide", Appearances: []int64{107}}
	presets[5] = wide
	out, _ := resolveRoads([]rawRoad{{Preset: 5, Active: true, XY: []string{"0,0", "1,1"}}}, presets, apps, tiers, 0)
	if !strings.Contains(out, "road\t2\t1500\t") {
		t.Errorf("a 3000 wide road:\n%s", out)
	}
	// the same raw roads at Town (4): the Vein is T4 now
	_, town := resolveRoads(raw, presets, apps, tiers, 4)
	if town.Kept != 3 || town.ByTier[4] != 2 {
		t.Errorf("at Town: %+v", town)
	}
}

func TestReadRawRoads(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "roads_raw.tsv")
	body := "# roads_raw v1\n# road\n" +
		"road\t6064631874603515921\t6064629126905528345\t1\t0\tCELL\tBP_RoadActor_C_X\t1.0,2.0\t3.0,4.0\n" +
		"\n" +
		"road\t5\t6\t0\t1\tC2\tA2\t0,0\t1,1\t2,2\n"
	if err := os.WriteFile(path, []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}
	got, err := readRawRoads(path)
	if err != nil || len(got) != 2 {
		t.Fatalf("%v %v", got, err)
	}
	if got[0].Preset != 6064631874603515921 || got[0].Zoi != 6064629126905528345 || !got[0].Active || got[0].Cell != "CELL" || len(got[0].XY) != 2 || got[1].Active || len(got[1].XY) != 3 {
		t.Errorf("%+v", got)
	}
	for _, bad := range []string{"road\t1\t2\t1\t0\tC\tA\t1,1\n", "wall\t1\t2\t1\t0\tC\tA\t1,1\t2,2\n", "road\tx\t2\t1\t0\tC\tA\t1,1\t2,2\n"} {
		if err := os.WriteFile(path, []byte(bad), 0o644); err != nil {
			t.Fatal(err)
		}
		if _, err := readRawRoads(path); err == nil {
			t.Errorf("%q was taken", bad)
		}
	}
}

// realContentDB opens the working tree's content.db (dist/content/content.db) read-only; the test is skipped without it.
func realContentDB(t *testing.T) *content.DB {
	t.Helper()
	path := filepath.Join(repoRoot(), "dist", "content", "content.db")
	if _, err := os.Stat(path); err != nil {
		t.Skipf("no %s", path)
	}
	db, err := content.Open(path)
	if err != nil {
		t.Skipf("%s: %v", path, err)
	}
	t.Cleanup(func() { db.Close() })
	return db
}

// [CoA ~ C.Y.B]

// TestRealRoadTiers reads the real tables: 27 presets, the research's tier table by node level, the widths.
func TestRealRoadTiers(t *testing.T) {
	db := realContentDB(t)
	presets, apps, tiers, err := loadRoadTables(db)
	if err != nil {
		t.Fatal(err)
	}
	if len(presets) != 27 || len(apps) != 26 || len(tiers) != 5 {
		t.Fatalf("%d presets, %d appearances, %d road effects (want 27, 26 and 5)", len(presets), len(apps), len(tiers))
	}
	check := func(preset string, want []int, width float64) {
		var p roadPreset
		for _, c := range presets {
			if c.Name == preset {
				p = c
			}
		}
		if p.Name == "" {
			t.Errorf("no preset %s", preset)
			return
		}
		for level, tier := range want {
			w, _, ok := resolveRoad(p, level, apps, tiers)
			if tier < 0 {
				if ok {
					t.Errorf("%s at %s: tier %d, want none", preset, nodeLevels[level], w.Tier)
				}
				continue
			}
			if !ok || w.Tier != tier || (width > 0 && w.Width != width) {
				t.Errorf("%s at %s: tier %d width %v (%v), want %d, %v", preset, nodeLevels[level], w.Tier, w.Width, ok, tier, width)
			}
		}
	}
	check("LionsHold_Roads", []int{2, 2, 2, 2, 2, 2, -1}, 750) // (six appearances: Wilderness to City; Metropolis has none)
	check("RoadPreset_Riverlands_Artery", []int{1, 1, 2, 3, 4, 4, 4}, 1000)
	check("RoadPreset_Riverlands_Vein", []int{-1, 0, 1, 3, 4, 4, 4}, 0)
	check("RoadPreset_Riverlands_Capillary", []int{-1, -1, -1, 2, 4, 4, 4}, 0)
}

// TestRealRoadsFile resolves the area's real roads_raw.tsv (mapc roads) at Wilderness, as the run that wrote dist/content/roads.tsv did; skipped without the files.
func TestRealRoadsFile(t *testing.T) {
	db := realContentDB(t)
	raw := filepath.Join(repoRoot(), "dist", "content", "roads_raw.tsv")
	if _, err := os.Stat(raw); err != nil {
		t.Skipf("no %s", raw)
	}
	out := filepath.Join(t.TempDir(), "roads.tsv")
	var stdout, stderr bytes.Buffer
	if code := runRoads([]string{"--content", filepath.Join(repoRoot(), "dist", "content", "content.db"), "--raw", raw, "--out", out}, &stdout, &stderr); code != 0 {
		t.Fatalf("exit %d: %s", code, stderr.String())
	}
	_ = db
	text, err := os.ReadFile(out)
	if err != nil {
		t.Fatal(err)
	}
	// review B I4: dist/content/roads.tsv (what worldd loads) is what a fresh run writes from this content.db and this roads_raw.tsv, byte for byte (the loose bounds below
	// would pass a stale file)
	if have, err := os.ReadFile(filepath.Join(repoRoot(), "dist", "content", "roads.tsv")); err != nil {
		t.Logf("no dist/content/roads.tsv to compare: %v", err)
	} else if !bytes.Equal(have, text) {
		t.Errorf("dist/content/roads.tsv (%d bytes) is not what contentc roads writes from dist/content/content.db and roads_raw.tsv (%d bytes): run `go run ./cmd/contentc roads`", len(have), len(text))
	}
	lines := strings.Split(strings.TrimSpace(string(text)), "\n")
	roads := len(lines) - 1
	t1, t2 := 0, 0
	for _, l := range lines[1:] {
		f := strings.Split(l, "\t")
		switch f[1] {
		case "1":
			t1++
		case "2":
			t2++
		default:
			t.Errorf("a tier that is neither 1 nor 2 at Wilderness: %q", f[1])
		}
		if f[2] != "500" && f[2] != "500.0" {
			t.Errorf("half width %q", f[2])
		}
	}
	if roads < 30 || t1 < 5 || t2 < 20 {
		t.Errorf("%d roads (%d tier 1, %d tier 2); the E1 area has 49 road actors: 31 Lion's Hold (T2), 11 Artery (T1), 7 nothing for a mount", roads, t1, t2)
	}
	if !strings.Contains(stdout.String(), "RoadPreset_Node_N3") {
		t.Errorf("the Node_N3 roads (nothing for a mount at level 0) are not reported:\n%s", stdout.String())
	}
	// --check says the same file is up to date, and a different one is not
	stdout.Reset()
	stderr.Reset()
	if code := runRoads([]string{"--content", filepath.Join(repoRoot(), "dist", "content", "content.db"), "--raw", raw, "--out", out, "--check"}, &stdout, &stderr); code != 0 {
		t.Errorf("--check on a fresh file: exit %d: %s", code, stderr.String())
	}
	if err := os.WriteFile(out, append(text, '\n'), 0o644); err != nil {
		t.Fatal(err)
	}
	stdout.Reset()
	stderr.Reset()
	if code := runRoads([]string{"--content", filepath.Join(repoRoot(), "dist", "content", "content.db"), "--raw", raw, "--out", out, "--check"}, &stdout, &stderr); code != 1 {
		t.Errorf("--check on a changed file: exit %d, want 1", code)
	}
}

// -- C | O | A x Cyb --

// TestResolveRoadsByLevel writes every road at all seven node levels with its owner zone (Nodes 1, stage 3b): the tier and half width of each level are what resolveRoads gives for
// that level alone, a level with nothing for a mount is -1 and 0, and a road that gives nothing at any level is left out.
func TestResolveRoadsByLevel(t *testing.T) {
	presets, apps, tiers := fakeRoadTables()
	lions, artery, vein, capillary := presetNamed(t, presets, "LionsHold_Roads"), presetNamed(t, presets, "Artery"), presetNamed(t, presets, "Vein"), presetNamed(t, presets, "Capillary")
	apps[900] = roadAppearance{Guid: 900, Width: 500, MountEffect: 0}
	dead := roadPreset{Guid: 77, Name: "Dead", Appearances: []int64{900, 900, 900, 900, 900, 900, 900}}
	presets[77] = dead
	raw := []rawRoad{
		{Preset: lions.Guid, Zoi: 11, Active: true, Cell: "B", Actor: "a", XY: []string{"0.0,0.0", "300.0,400.0"}},
		{Preset: artery.Guid, Zoi: 22, Active: true, Cell: "A", Actor: "b", XY: []string{"10.0,10.0", "10.0,5010.0"}},
		{Preset: capillary.Guid, Zoi: 33, Active: true, Cell: "A", Actor: "c", XY: []string{"0.0,0.0", "1.0,1.0"}},
		{Preset: vein.Guid, Zoi: 44, Active: false, Cell: "A", Actor: "d", XY: []string{"0.0,0.0", "1.0,1.0"}}, // inactive
		{Preset: dead.Guid, Zoi: 55, Active: true, Cell: "A", Actor: "e", XY: []string{"0.0,0.0", "1.0,1.0"}},  // nothing at any level
		{Preset: 4242, Zoi: 66, Active: true, Cell: "A", Actor: "f", XY: []string{"0.0,0.0", "1.0,1.0"}},       // no such preset
	}
	text, sum := resolveRoadsByLevel(raw, presets, apps, tiers)
	if sum.Read != 6 || sum.Kept != 3 || sum.NoWorth != 1 || sum.Inactive != 1 || sum.NoPreset != 1 {
		t.Fatalf("summary %+v", sum)
	}
	lines := strings.Split(strings.TrimSpace(text), "\n")
	if len(lines) != 4 || !strings.HasPrefix(lines[0], "# roads v2: 3 roads with a tier") {
		t.Fatalf("file:\n%s", text)
	}
	// sorted by preset name, cell and actor: Artery, Capillary, LionsHold
	want := []string{
		"roadn\t22\t1,1,2,3,4,4,4\t500,500,500,500,500,500,500\t10.0,10.0\t10.0,5010.0",
		"roadn\t33\t-1,-1,-1,2,4,4,4\t0,0,0,500,500,500,500\t0.0,0.0\t1.0,1.0",
		"roadn\t11\t2,2,2,2,2,2,2\t500,500,500,500,500,500,500\t0.0,0.0\t300.0,400.0",
	}
	for i, w := range want {
		if lines[i+1] != w {
			t.Errorf("line %d:\n got %s\nwant %s", i+1, lines[i+1], w)
		}
	}
	// the by-level file agrees with the one-level file at every level (the road lines are the same roads, in the same order)
	for level := 0; level < len(nodeLevels); level++ {
		one, _ := resolveRoads(raw, presets, apps, tiers, level)
		var tiersAtLevel []string
		for _, l := range lines[1:] {
			f := strings.Split(l, "\t")
			if tier := strings.Split(f[2], ",")[level]; tier != "-1" {
				tiersAtLevel = append(tiersAtLevel, tier)
			}
		}
		var oneTiers []string
		for _, l := range strings.Split(strings.TrimSpace(one), "\n")[1:] {
			oneTiers = append(oneTiers, strings.Split(l, "\t")[1])
		}
		if strings.Join(tiersAtLevel, ",") != strings.Join(oneTiers, ",") {
			t.Errorf("level %d: by-level tiers %v, one-level tiers %v", level, tiersAtLevel, oneTiers)
		}
	}
}

// TestRealRoadsLevelsFile runs `contentc roads --levels` on the real tables and roads_raw.tsv: seven tiers and widths for every road, and at Wilderness the roads of the one-level file.
func TestRealRoadsLevelsFile(t *testing.T) {
	db := realContentDB(t)
	raw := filepath.Join(repoRoot(), "dist", "content", "roads_raw.tsv")
	if _, err := os.Stat(raw); err != nil {
		t.Skipf("no %s", raw)
	}
	_ = db
	out := filepath.Join(t.TempDir(), "roads_levels.tsv")
	var stdout, stderr bytes.Buffer
	args := []string{"--content", filepath.Join(repoRoot(), "dist", "content", "content.db"), "--raw", raw, "--out", out, "--levels"}
	if code := runRoads(args, &stdout, &stderr); code != 0 {
		t.Fatalf("exit %d: %s", code, stderr.String())
	}
	text, err := os.ReadFile(out)
	if err != nil {
		t.Fatal(err)
	}
	if have, err := os.ReadFile(filepath.Join(repoRoot(), "dist", "content", "roads_levels.tsv")); err != nil {
		t.Logf("no dist/content/roads_levels.tsv to compare: %v", err)
	} else if !bytes.Equal(have, text) {
		t.Errorf("dist/content/roads_levels.tsv (%d bytes) is not what contentc roads --levels writes (%d bytes): run `go run ./cmd/contentc roads --levels`", len(have), len(text))
	}
	// at level 0 the by-level file holds the roads dist's one-level file holds (roads.tsv is Wilderness'), with the same tier and width
	lines := strings.Split(strings.TrimSpace(string(text)), "\n")
	var level0 []string
	for _, l := range lines[1:] {
		f := strings.Split(l, "\t")
		if len(f) < 7 || f[0] != "roadn" {
			t.Fatalf("not a by-level road: %q", l)
		}
		ts, hs := strings.Split(f[2], ","), strings.Split(f[3], ",")
		if len(ts) != 7 || len(hs) != 7 {
			t.Fatalf("a road without seven levels: %q", l)
		}
		if ts[0] != "-1" {
			level0 = append(level0, strings.Join(append([]string{"road", ts[0], hs[0]}, f[4:]...), "\t"))
		}
	}
	oneOut := filepath.Join(t.TempDir(), "roads.tsv")
	stdout.Reset()
	stderr.Reset()
	if code := runRoads([]string{"--content", filepath.Join(repoRoot(), "dist", "content", "content.db"), "--raw", raw, "--out", oneOut}, &stdout, &stderr); code != 0 {
		t.Fatalf("exit %d: %s", code, stderr.String())
	}
	one, _ := os.ReadFile(oneOut)
	oneLines := strings.Split(strings.TrimSpace(string(one)), "\n")[1:]
	sort.Strings(level0)
	sort.Strings(oneLines)
	if strings.Join(level0, "\n") != strings.Join(oneLines, "\n") {
		t.Errorf("the by-level file at Wilderness (%d roads) is not the Wilderness file (%d roads)", len(level0), len(oneLines))
	}
}
