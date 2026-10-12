// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package citynode_test

import (
	"database/sql"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"

	"aocrop/services/internal/citynode"
	"aocrop/services/internal/content"
)

const (
	joeva   = int64(108646121231993)
	halcyon = int64(108646610851452)
)

func TestRosterIsTheRetail46(t *testing.T) {
	r := citynode.DefaultRoster()
	if r.Len() != 46 {
		t.Fatalf("%d nodes, want the 46 of the retail game state", r.Len())
	}
	nodes := r.Nodes()
	if !sort.SliceIsSorted(nodes, func(i, j int) bool { return nodes[i].Guid < nodes[j].Guid }) {
		t.Error("the roster is not in guid order")
	}
	for _, g := range []int64{108646108518008, joeva, 108646608033403, halcyon, 108646615701117, 6064630651307950121} {
		if _, ok := r.ByGuid(g); !ok {
			t.Errorf("the Riverlands node %d is missing", g)
		}
	}
	// the three dungeon / lawless extras the client asks about are NOT in the game state's 46, and the test rows are not nodes
	for _, g := range []int64{6064634132054344390, 6064634132002701330, 6064633448239402132, 108323252295288} {
		if _, ok := r.ByGuid(g); ok {
			t.Errorf("%d is not one of the 46", g)
		}
	}
}

func TestResolve(t *testing.T) {
	r := citynode.DefaultRoster()
	for _, name := range []string{"Verra_RVR_Joeva", "verra_rvr_joeva", "RVR_Joeva", "Joeva", " joeva ", "108646121231993"} {
		n, err := r.Resolve(0, name)
		if err != nil || n.Guid != joeva {
			t.Errorf("Resolve(%q) = %v, %v; want Joeva", name, n, err)
		}
	}
	if n, err := r.Resolve(halcyon, "Joeva"); err != nil || n.Guid != halcyon {
		t.Errorf("a guid wins over a name: %v, %v", n, err)
	}
	if n, err := r.Resolve(0, "New Aela"); err != nil || n.Name != "Verra_RVR_NewAela" {
		t.Errorf("display name with a space: %v, %v", n, err)
	}
	if _, err := r.Resolve(0, "Atlantis"); !errors.Is(err, citynode.ErrNotFound) {
		t.Errorf("unknown name: %v", err)
	}
	if _, err := r.Resolve(5, ""); !errors.Is(err, citynode.ErrNotFound) {
		t.Errorf("unknown guid: %v", err)
	}
	if _, err := r.Resolve(0, "  "); !errors.Is(err, citynode.ErrInvalid) {
		t.Errorf("no node named: %v", err)
	}
}

// [C_Y_B :: CoA]

// Every guid and name of the roster is a CityNodeRecordBase of the real content.db (skipped without one).
func TestRosterMatchesContentDB(t *testing.T) {
	path := os.Getenv("AOC_CONTENT_DB")
	if path == "" {
		path = filepath.Join("..", "..", "..", "dist", "content", "content.db")
	}
	if _, err := os.Stat(path); err != nil {
		t.Skipf("no content.db at %s", path)
	}
	d, err := sql.Open("sqlite", path+"?_pragma=query_only(1)")
	if err != nil {
		t.Fatal(err)
	}
	defer d.Close()
	var encoding string
	if err := d.QueryRow(`SELECT value FROM meta WHERE key = ?`, content.MetaRecordEncoding).Scan(&encoding); err != nil {
		t.Skipf("%s is not a content.db this test reads: %v", path, err)
	}
	for _, n := range citynode.DefaultRoster().Nodes() {
		var name string
		var data []byte
		err := d.QueryRow(`SELECT name, data FROM record WHERE schema = 'CityNodeRecordBase' AND guid = ?`, n.Guid).Scan(&name, &data)
		if err != nil {
			t.Errorf("%s (%d): not a CityNodeRecordBase of content.db: %v", n.Name, n.Guid, err)
			continue
		}
		if name != n.Name {
			t.Errorf("guid %d is %q in content.db, %q in the roster", n.Guid, name, n.Name)
		}
		js, err := content.DecodeData(encoding, data)
		if err != nil {
			t.Fatal(err)
		}
		var rec struct {
			NodeName string `json:"nodeName"`
		}
		if err := json.Unmarshal(js, &rec); err != nil {
			t.Fatal(err)
		}
		if rec.NodeName != n.DisplayName {
			t.Errorf("%s: nodeName %q in content.db, %q in the roster", n.Name, rec.NodeName, n.DisplayName)
		}
	}
}

// The level constants are the ini's (DefaultAoCRecordConstants.ini): pinned here, and compared with the file when it is there.
func TestLevelConstants(t *testing.T) {
	want := []int64{0, 2636934, 10547736, 31643207, 73834150, 538730690, 1162745780}
	if len(citynode.ExpToLevel) != len(want) {
		t.Fatalf("%d thresholds, want %d", len(citynode.ExpToLevel), len(want))
	}
	for i, w := range want {
		if citynode.ExpToLevel[i] != w {
			t.Errorf("ExpToLevel[%d] = %d, want %d", i, citynode.ExpToLevel[i], w)
		}
	}
	if citynode.MaxNodeLevel != 4 || citynode.MinimumOperationalNodeLevel != 3 {
		t.Errorf("MaxNodeLevel %d, MinimumOperationalNodeLevel %d; want Town 4 and Village 3", citynode.MaxNodeLevel, citynode.MinimumOperationalNodeLevel)
	}
	ini := filepath.Join(os.Getenv("AOC_CLIENT_CONFIG"), "DefaultAoCRecordConstants.ini") // the exported client's AOC/Config folder; without it the test is skipped
	b, err := os.ReadFile(ini)
	if err != nil {
		t.Skipf("no ini at %s", ini)
	}
	var got []string
	for _, line := range strings.Split(strings.ReplaceAll(string(b), "\r", ""), "\n") {
		if v, ok := strings.CutPrefix(line, "+CityNodeExpToLevel="); ok {
			got = append(got, v)
		}
		if v, ok := strings.CutPrefix(line, "MaxNodeLevel="); ok && v != citynode.LevelName(citynode.MaxNodeLevel) {
			t.Errorf("the ini's MaxNodeLevel is %s, the code's %s", v, citynode.LevelName(citynode.MaxNodeLevel))
		}
		if v, ok := strings.CutPrefix(line, "MinimumOperationalNodeLevel="); ok && v != citynode.LevelName(citynode.MinimumOperationalNodeLevel) {
			t.Errorf("the ini's MinimumOperationalNodeLevel is %s, the code's %s", v, citynode.LevelName(citynode.MinimumOperationalNodeLevel))
		}
	}
	if len(got) != len(want) {
		t.Fatalf("the ini has %d thresholds: %v", len(got), got)
	}
	for i, v := range got {
		if v != itoa(want[i]) {
			t.Errorf("ini threshold %d is %s, the code's %d", i, v, want[i])
		}
	}
}

func itoa(n int64) string { b, _ := json.Marshal(n); return string(b) }

func TestProgress(t *testing.T) {
	type tc struct {
		level  int
		xp     int64
		pct    int
		toNext int64
	}
	for _, c := range []tc{
		{0, 0, 0, 2636934},
		{0, 1318467, 50, 2636934 - 1318467},
		{1, 0, 0, 10547736},                  // below the level's own threshold: clamped
		{1, 2636934, 0, 10547736 - 2636934},  // exactly at the threshold
		{1, 6592335, 50, 10547736 - 6592335}, // halfway between 2,636,934 and 10,547,736
		{1, 10547736, 100, 0},                // at the next threshold: the level does not move
		{3, 999999999, 100, 0},               // far past it
		{4, 100, 0, 0},                       // Town is the highest level: retail's max-level nodes show 0
		{4, 999999999, 0, 0},
		{7, 5, 0, 0}, // a level the table lacks
	} {
		pct, next := citynode.Progress(c.level, c.xp)
		if pct != c.pct || next != c.toNext {
			t.Errorf("Progress(%d, %d) = %d, %d; want %d, %d", c.level, c.xp, pct, next, c.pct, c.toNext)
		}
	}
}

func TestParseSeed(t *testing.T) {
	r := citynode.DefaultRoster()
	good, err := citynode.ParseSeed([]byte(`{"comment":"x","nodes":{"Joeva":{"level":4},"RVR_Halcyon":{"level":3,"parent":"Verra_RVR_Joeva"},"Verra_RVR_Winstead":{"level":0}}}`), r)
	if err != nil {
		t.Fatal(err)
	}
	if good.Levels[joeva] != 4 || good.Levels[halcyon] != 3 || good.Parents[halcyon] != joeva || len(good.Parents) != 1 || len(good.Levels) != 3 {
		t.Errorf("seed = %+v", good)
	}
	if s, err := citynode.ParseSeed([]byte(`{}`), r); err != nil || len(s.Levels) != 0 {
		t.Errorf("an empty seed is every node at Wilderness: %+v, %v", s, err)
	}
	for name, body := range map[string]string{
		"not json":          `{"nodes":`,
		"unknown field":     `{"nodes":{},"levels":1}`,
		"unknown node":      `{"nodes":{"Atlantis":{"level":1}}}`,
		"level too high":    `{"nodes":{"Joeva":{"level":5}}}`,
		"level negative":    `{"nodes":{"Joeva":{"level":-1}}}`,
		"no level":          `{"nodes":{"Joeva":{}}}`,
		"node twice":        `{"nodes":{"Joeva":{"level":1},"Verra_RVR_Joeva":{"level":2}}}`,
		"own parent":        `{"nodes":{"Joeva":{"level":1,"parent":"Joeva"}}}`,
		"unknown parent":    `{"nodes":{"Joeva":{"level":1,"parent":"Atlantis"}}}`,
		"parent loop":       `{"nodes":{"Joeva":{"level":1,"parent":"Halcyon"},"Halcyon":{"level":1,"parent":"Joeva"}}}`,
		"text after object": `{"nodes":{}} {}`,
	} {
		if _, err := citynode.ParseSeed([]byte(body), r); err == nil {
			t.Errorf("%s: accepted %s", name, body)
		}
	}
}

// deploy/citynode_seed.json, the file the launcher's services apply, is the fresh start of decision D2.
func TestDeploySeed(t *testing.T) {
	s, err := citynode.LoadSeed(filepath.Join("..", "..", "..", "deploy", "citynode_seed.json"), citynode.DefaultRoster())
	if err != nil {
		t.Fatal(err)
	}
	if len(s.Levels) != 2 || s.Levels[joeva] != 4 || s.Levels[halcyon] != 3 || len(s.Parents) != 1 || s.Parents[halcyon] != joeva {
		t.Errorf("deploy/citynode_seed.json = %+v; want Joeva Town (4), Halcyon Village (3) as Joeva's vassal, nothing else", s)
	}
}
