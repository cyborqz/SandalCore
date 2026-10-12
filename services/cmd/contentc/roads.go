// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"bufio"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"math"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"

	"aocrop/services/internal/content"
)

// The roads (milestone "mounts", stage 4).  `contentc roads` resolves what the map's road splines are worth to a mount and writes the file worldd loads:
//
//	contentc roads [--content content.db] [--raw roads_raw.tsv] [--out roads.tsv] [--node-level Wilderness|Crossroads|...|Metropolis|0..6]
//
// tools/mapc (`mapc roads`) writes roads_raw.tsv: one line per BP_RoadActor_C with the guids of its RoadPresetRecord and of its owner ZOI and its spline sampled every
// 200 cm.  This resolves, for every road, the preset's RoadAppearancesIds[node level] (ENodeLevel: Wilderness 0 .. Metropolis 6) to a RoadAppearanceRecord, takes its
// RoadBuffs entry for ECharacterClassification::Mount (an effect Road_T{n}_Speed_Mount, the mount_effect table's road rows) and its RoadWidth.  The node level is the
// one worldd's --spawn-node-level names (the area's spawns are judged at it, the roads belong to the same node): a ZOI that is a starting area has no node of its own,
// the level the world runs at stands in for it.  A road whose appearance has no Mount buff at that level (a Vein or a Capillary below its levels, a Node_N3 road at
// level 0) gives a mount nothing and is left out; so is an inactive one.
//
//	road <TAB> tier <TAB> half_width_cm <TAB> x,y <TAB> x,y ...
//
// Nodes 1, stage 3b: `contentc roads --levels` writes dist/content/roads_levels.tsv instead: every road at ALL seven node levels, with the zone of influence that owns it, so that worldd can
// set each road's tier to the level of ITS node as the node's level changes (a ZOI that is a starting area has no node: worldd's default level stands in for it).
//
//	roadn <TAB> zoi_guid <TAB> t0,..,t6 <TAB> h0,..,h6 <TAB> x,y <TAB> x,y ...      (tier -1 and half width 0 at a level where the road gives a mount nothing)
//
// A road that gives a mount nothing at any level is left out, and so is an inactive one.
//
// The half width of the overlap is max(RoadWidth, 1000) / 2 [modelled]: the buff volumes are built at run time from the spline (ARoadBase::GenerateRoadBuffVolumes) and
// no clean retail sample calibrates their width (capture 918, 11.7 m from a 750 cm road, is not clean).

// nodeLevels are ENodeLevel in order.
var nodeLevels = []string{"Wilderness", "Crossroads", "Encampment", "Village", "Town", "City", "Metropolis"}

// ~ C Y B :: C | O | A ~

// nodeLevelNumber reads "Town" or "4".
func nodeLevelNumber(s string) (int, error) {
	for i, n := range nodeLevels {
		if strings.EqualFold(n, s) {
			return i, nil
		}
	}
	if n, err := strconv.Atoi(s); err == nil && n >= 0 && n < len(nodeLevels) {
		return n, nil
	}
	return 0, fmt.Errorf("%q is no node level (Wilderness, Crossroads, Encampment, Village, Town, City, Metropolis, or 0 to 6)", s)
}

// roadMinHalfWidth is the least width the overlap uses: max(RoadWidth, 1000) / 2.
const roadMinWidthCm = 1000.0

// roadPreset is a RoadPresetRecord: its appearance per node level.
type roadPreset struct {
	Guid        int64
	Name        string
	Appearances []int64
}

// roadAppearance is a RoadAppearanceRecord as a mount sees it.
type roadAppearance struct {
	Guid        int64
	Name        string
	Width       float64
	MountEffect int64 // the EffectRecord of the RoadBuffs entry for ECharacterClassification::Mount (0: none)
}

// roadWorth is what a road gives a mount at a node level.
type roadWorth struct {
	Tier  int
	Width float64
}

// resolveRoad is a preset's worth at a node level; ok false (and why) when it gives a mount nothing.
func resolveRoad(p roadPreset, level int, apps map[int64]roadAppearance, tiers map[int64]int) (w roadWorth, why string, ok bool) {
	if level < 0 || level >= len(p.Appearances) {
		return w, "the preset has no appearance for the node level", false
	}
	a, found := apps[p.Appearances[level]]
	if !found {
		return w, "the preset's appearance at the node level is no RoadAppearanceRecord", false
	}
	if a.MountEffect == 0 {
		return w, "the appearance has no buff for mounts", false
	}
	tier, found := tiers[a.MountEffect]
	if !found {
		return w, "the mount buff is no Road_T{n}_Speed_Mount effect", false
	}
	return roadWorth{Tier: tier, Width: a.Width}, "", true
}

type rawRoad struct {
	Preset, Zoi int64
	Active      bool
	Cell, Actor string
	XY          []string // "x,y" as mapc wrote them
}

// readRawRoads reads mapc's roads_raw.tsv.
func readRawRoads(path string) ([]rawRoad, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	var out []rawRoad
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 1<<20), 64<<20)
	n := 0
	for sc.Scan() {
		n++
		line := strings.TrimRight(sc.Text(), "\r")
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		fs := strings.Split(line, "\t")
		if len(fs) < 9 || fs[0] != "road" {
			return nil, fmt.Errorf("%s line %d is not a road (road, preset, zoi, active, in_node, cell, actor and two points at least)", path, n)
		}
		preset, err1 := strconv.ParseUint(fs[1], 10, 64)
		zoi, err2 := strconv.ParseUint(fs[2], 10, 64)
		if err1 != nil || err2 != nil {
			return nil, fmt.Errorf("%s line %d: a guid is not a number", path, n)
		}
		out = append(out, rawRoad{Preset: int64(preset), Zoi: int64(zoi), Active: fs[3] != "0", Cell: fs[5], Actor: fs[6], XY: fs[7:]})
	}
	return out, sc.Err()
}

// loadRoadTables reads the three tables a tier comes from out of content.db.
func loadRoadTables(db *content.DB) (map[int64]roadPreset, map[int64]roadAppearance, map[int64]int, error) {
	meta, err := db.Meta()
	if err != nil {
		return nil, nil, nil, err
	}
	enc := meta["record.encoding"]
	records := func(schema string, each func(guid int64, name string, js json.RawMessage) error) error {
		rows, err := db.SQL().Query(`SELECT guid, name, data FROM record WHERE schema = ? ORDER BY guid`, schema)
		if err != nil {
			return err
		}
		defer rows.Close()
		for rows.Next() {
			var guid int64
			var name string
			var data []byte
			if err := rows.Scan(&guid, &name, &data); err != nil {
				return err
			}
			js, err := content.DecodeData(enc, data)
			if err != nil {
				return fmt.Errorf("%s %s: %w", schema, name, err)
			}
			if err := each(guid, name, js); err != nil {
				return fmt.Errorf("%s %s: %w", schema, name, err)
			}
		}
		return rows.Err()
	}
	presets := map[int64]roadPreset{}
	if err := records("RoadPresetRecordBase", func(guid int64, name string, js json.RawMessage) error {
		var r struct {
			RoadAppearancesIds []struct{ Guid int64 }
		}
		if err := json.Unmarshal(js, &r); err != nil {
			return err
		}
		p := roadPreset{Guid: guid, Name: name}
		for _, a := range r.RoadAppearancesIds {
			p.Appearances = append(p.Appearances, a.Guid)
		}
		presets[guid] = p
		return nil
	}); err != nil {
		return nil, nil, nil, err
	}
	apps := map[int64]roadAppearance{}
	if err := records("RoadAppearanceRecordBase", func(guid int64, name string, js json.RawMessage) error {
		var r struct {
			RoadWidth float64
			RoadBuffs []struct {
				EffectClassifications []string
				EffectRecordId        struct{ Guid int64 }
			}
		}
		if err := json.Unmarshal(js, &r); err != nil {
			return err
		}
		a := roadAppearance{Guid: guid, Name: name, Width: r.RoadWidth}
		for _, b := range r.RoadBuffs {
			for _, c := range b.EffectClassifications {
				if c == "ECharacterClassification::Mount" {
					a.MountEffect = b.EffectRecordId.Guid
				}
			}
		}
		apps[guid] = a
		return nil
	}); err != nil {
		return nil, nil, nil, err
	}
	tiers := map[int64]int{}
	rows, err := db.SQL().Query(`SELECT effect_guid, tier FROM mount_effect WHERE kind = 'road' ORDER BY tier`)
	if err != nil {
		return nil, nil, nil, err
	}
	defer rows.Close()
	for rows.Next() {
		var guid int64
		var tier int
		if err := rows.Scan(&guid, &tier); err != nil {
			return nil, nil, nil, err
		}
		tiers[guid] = tier
	}
	return presets, apps, tiers, rows.Err()
}

// (C:Y:B) (CoA)

// roadsSummary is what a run reports.
type roadsSummary struct {
	Level              int
	Read, Kept         int
	NoPreset, NoWorth  int
	Inactive           int
	ByTier             map[int]int
	LengthKmByTier     map[int]float64
	ByPreset           map[string]int
	NoWorthByPreset    map[string]int
	UnknownPresetGuids map[int64]int
}

func xyLength(xy []string) float64 {
	var px, py float64
	total := 0.0
	for i, p := range xy {
		c := strings.IndexByte(p, ',')
		if c < 0 {
			continue
		}
		x, _ := strconv.ParseFloat(p[:c], 64)
		y, _ := strconv.ParseFloat(p[c+1:], 64)
		if i > 0 {
			total += math.Hypot(x-px, y-py)
		}
		px, py = x, y
	}
	return total
}

// writeRoads resolves `raw` at `level` and writes the file; the text and the summary.
func resolveRoads(raw []rawRoad, presets map[int64]roadPreset, apps map[int64]roadAppearance, tiers map[int64]int, level int) (string, roadsSummary) {
	s := roadsSummary{Level: level, ByTier: map[int]int{}, LengthKmByTier: map[int]float64{}, ByPreset: map[string]int{}, NoWorthByPreset: map[string]int{}, UnknownPresetGuids: map[int64]int{}}
	var b strings.Builder
	type line struct {
		tier  int
		width float64
		text  string
		key   string
	}
	var lines []line
	for _, r := range raw {
		s.Read++
		if !r.Active {
			s.Inactive++
			continue
		}
		p, ok := presets[r.Preset]
		if !ok {
			s.NoPreset++
			s.UnknownPresetGuids[r.Preset]++
			continue
		}
		w, _, ok := resolveRoad(p, level, apps, tiers)
		if !ok {
			s.NoWorth++
			s.NoWorthByPreset[p.Name]++
			continue
		}
		half := math.Max(w.Width, roadMinWidthCm) / 2
		var l strings.Builder
		fmt.Fprintf(&l, "road\t%d\t%g", w.Tier, half)
		for _, xy := range r.XY {
			l.WriteString("\t")
			l.WriteString(xy)
		}
		lines = append(lines, line{tier: w.Tier, width: half, text: l.String(), key: p.Name + "/" + r.Cell + "/" + r.Actor})
		s.Kept++
		s.ByTier[w.Tier]++
		s.LengthKmByTier[w.Tier] += xyLength(r.XY) / 100000.0
		s.ByPreset[p.Name]++
	}
	sort.SliceStable(lines, func(i, j int) bool { return lines[i].key < lines[j].key })
	fmt.Fprintf(&b, "# roads v1: %d roads for a mount at node level %d (%s) from %d road actors (mapc roads, contentc roads). road, tier (Road_Tn_Speed_Mount), half width cm (max(RoadWidth, %g) / 2), points x,y in world cm\n",
		s.Kept, level, nodeLevels[level], s.Read, roadMinWidthCm)
	for _, l := range lines {
		b.WriteString(l.text)
		b.WriteString("\n")
	}
	return b.String(), s
}

// resolveRoadsByLevel resolves `raw` at every node level and writes the by-level file's text.
func resolveRoadsByLevel(raw []rawRoad, presets map[int64]roadPreset, apps map[int64]roadAppearance, tiers map[int64]int) (string, roadsSummary) {
	s := roadsSummary{Level: -1, ByTier: map[int]int{}, LengthKmByTier: map[int]float64{}, ByPreset: map[string]int{}, NoWorthByPreset: map[string]int{}, UnknownPresetGuids: map[int64]int{}}
	type line struct {
		text string
		key  string
	}
	var lines []line
	for _, r := range raw {
		s.Read++
		if !r.Active {
			s.Inactive++
			continue
		}
		p, ok := presets[r.Preset]
		if !ok {
			s.NoPreset++
			s.UnknownPresetGuids[r.Preset]++
			continue
		}
		var ts, hs []string
		any := false
		for level := 0; level < len(nodeLevels); level++ {
			w, _, ok := resolveRoad(p, level, apps, tiers)
			if !ok {
				ts = append(ts, "-1")
				hs = append(hs, "0")
				continue
			}
			any = true
			ts = append(ts, strconv.Itoa(w.Tier))
			hs = append(hs, fmt.Sprintf("%g", math.Max(w.Width, roadMinWidthCm)/2))
			s.ByTier[w.Tier]++
		}
		if !any {
			s.NoWorth++
			s.NoWorthByPreset[p.Name]++
			continue
		}
		var l strings.Builder
		fmt.Fprintf(&l, "roadn\t%d\t%s\t%s", r.Zoi, strings.Join(ts, ","), strings.Join(hs, ","))
		for _, xy := range r.XY {
			l.WriteString("\t")
			l.WriteString(xy)
		}
		lines = append(lines, line{text: l.String(), key: p.Name + "/" + r.Cell + "/" + r.Actor})
		s.Kept++
		s.ByPreset[p.Name]++
	}
	sort.SliceStable(lines, func(i, j int) bool { return lines[i].key < lines[j].key })
	var b strings.Builder
	fmt.Fprintf(&b, "# roads v2: %d roads with a tier (Road_Tn_Speed_Mount, -1 none) and a half width cm (max(RoadWidth, %g) / 2) at each node level 0 to 6, from %d road actors (mapc roads, contentc roads --levels). roadn, owner zone guid, tiers, half widths, points x,y in world cm\n",
		s.Kept, roadMinWidthCm, s.Read)
	for _, l := range lines {
		b.WriteString(l.text)
		b.WriteString("\n")
	}
	return b.String(), s
}

func runRoads(args []string, stdout, stderr io.Writer) int {
	fs := flag.NewFlagSet("contentc roads", flag.ContinueOnError)
	fs.SetOutput(stderr)
	dir := filepath.Join(repoRoot(), "dist", "content")
	contentPath := fs.String("content", filepath.Join(dir, "content.db"), "the content.db the road tables are read from")
	rawPath := fs.String("raw", filepath.Join(dir, "roads_raw.tsv"), "mapc's roads_raw.tsv")
	outPath := fs.String("out", filepath.Join(dir, "roads.tsv"), "the roads file worldd loads")
	levelName := fs.String("node-level", "Wilderness", "the node level the roads are worth what they are at (worldd's --spawn-node-level): Wilderness .. Metropolis, or 0 to 6")
	levels := fs.Bool("levels", false, "write the by-level file (roads_levels.tsv: every road at all seven node levels with its owner zone) instead of one level's")
	check := fs.Bool("check", false, "write nothing; exit 1 when --out differs from what would be written")
	if err := fs.Parse(args); err != nil {
		if err == flag.ErrHelp {
			return 0
		}
		return 2
	}
	outSet := false
	fs.Visit(func(f *flag.Flag) {
		if f.Name == "out" {
			outSet = true
		}
	})
	if *levels && !outSet {
		*outPath = filepath.Join(dir, "roads_levels.tsv")
	}
	level, err := nodeLevelNumber(*levelName)
	if err != nil {
		fmt.Fprintf(stderr, "contentc roads: %v\n", err)
		return 2
	}
	raw, err := readRawRoads(*rawPath)
	if err != nil {
		fmt.Fprintf(stderr, "contentc roads: %v\n", err)
		return 1
	}
	db, err := content.Open(*contentPath)
	if err != nil {
		fmt.Fprintf(stderr, "contentc roads: %v\n", err)
		return 1
	}
	defer db.Close()
	presets, apps, tiers, err := loadRoadTables(db)
	if err != nil {
		fmt.Fprintf(stderr, "contentc roads: %v\n", err)
		return 1
	}
	var text string
	var sum roadsSummary
	if *levels {
		text, sum = resolveRoadsByLevel(raw, presets, apps, tiers)
		fmt.Fprintf(stdout, "contentc roads: %d road actors, %d kept at every node level: %d inactive, %d with no preset, %d giving a mount nothing at any level\n",
			sum.Read, sum.Kept, sum.Inactive, sum.NoPreset, sum.NoWorth)
	} else {
		text, sum = resolveRoads(raw, presets, apps, tiers, level)
		fmt.Fprintf(stdout, "contentc roads: %d road actors, %d kept at node level %d (%s): %d inactive, %d with no preset, %d giving a mount nothing there\n",
			sum.Read, sum.Kept, level, nodeLevels[level], sum.Inactive, sum.NoPreset, sum.NoWorth)
	}
	var tierKeys []int
	for t := range sum.ByTier {
		tierKeys = append(tierKeys, t)
	}
	sort.Ints(tierKeys)
	for _, t := range tierKeys {
		if *levels {
			fmt.Fprintf(stdout, "contentc roads:   Road_T%d_Speed_Mount: %d (road, level) pairs\n", t, sum.ByTier[t])
		} else {
			fmt.Fprintf(stdout, "contentc roads:   Road_T%d_Speed_Mount: %d roads, %.1f km\n", t, sum.ByTier[t], sum.LengthKmByTier[t])
		}
	}
	var names []string
	for n := range sum.ByPreset {
		names = append(names, n)
	}
	sort.Strings(names)
	for _, n := range names {
		fmt.Fprintf(stdout, "contentc roads:   %s: %d\n", n, sum.ByPreset[n])
	}
	names = names[:0]
	for n := range sum.NoWorthByPreset {
		names = append(names, n)
	}
	sort.Strings(names)
	for _, n := range names {
		fmt.Fprintf(stdout, "contentc roads:   (nothing for mounts) %s: %d\n", n, sum.NoWorthByPreset[n])
	}
	if sum.NoPreset > 0 {
		fmt.Fprintf(stderr, "contentc roads: %d road actors name a preset that is no RoadPresetRecord: %v\n", sum.NoPreset, sum.UnknownPresetGuids)
		return 1
	}
	if *check {
		have, err := os.ReadFile(*outPath)
		if err != nil || string(have) != text {
			fmt.Fprintf(stderr, "contentc roads: %s is not what a fresh run writes\n", *outPath)
			return 1
		}
		fmt.Fprintf(stdout, "contentc roads: %s is up to date (%d bytes)\n", *outPath, len(have))
		return 0
	}
	if err := os.MkdirAll(filepath.Dir(*outPath), 0o755); err != nil {
		fmt.Fprintf(stderr, "contentc roads: %v\n", err)
		return 1
	}
	tmp := *outPath + ".tmp"
	if err := os.WriteFile(tmp, []byte(text), 0o644); err != nil {
		fmt.Fprintf(stderr, "contentc roads: %v\n", err)
		return 1
	}
	if err := os.Rename(tmp, *outPath); err != nil {
		fmt.Fprintf(stderr, "contentc roads: %v\n", err)
		return 1
	}
	fmt.Fprintf(stdout, "contentc roads: wrote %s (%d bytes)\n", *outPath, len(text))
	return 0
}
