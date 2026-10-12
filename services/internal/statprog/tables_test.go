// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package statprog

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"math"
	"math/rand"
	"sort"
	"testing"

	"aocrop/services/internal/content"
)

// Plan B3: contentc writes what an item adds to the stats into tables (content/gear.go reads them); worldd's C++ reads the
// same tables. Gear reads the records' JSON. These tests prove that the tables say what the records say, by reading the tables
// with the engine's own rules (the reading of gear.go, restated over the tables) and comparing with Gear.Mods and Gear.Tags
// bit for bit, over every item and over random sets of worn items; and that the stored stat program is the compiled graph.

// tableMods is Gear.Mods read from the tables.
func tableMods(db *content.DB, items []Item, rd Rounding) ([]Mod, error) {
	sum := map[int64]float64{}
	setRarities := map[int64][]int{}
	var setOrder []int64
	for _, it := range items {
		rec, err := db.Item(it.Record)
		if err != nil {
			return nil, err
		}
		var bands [7]content.QualityBand
		hasCurve := rec.QualityCurveGuid != 0
		if hasCurve {
			if bands, err = db.QualityCurve(rec.QualityCurveGuid); err != nil {
				return nil, err
			}
		}
		q := float64(it.Quality)
		band := func() (idx int, x, y float64) {
			found := false
			for i, b := range bands {
				if b.X <= q && q <= b.Y {
					return i, b.X, b.Y
				}
				if b.X <= q {
					idx, x, y, found = i, b.X, b.Y, true
				}
			}
			if !found {
				return 0, bands[0].X, bands[0].Y
			}
			return
		}
		if rec.StatBlockGuid != 0 {
			block, err := db.StatBlock(rec.StatBlockGuid)
			if err != nil {
				return nil, err
			}
			for _, s := range block {
				var val float64
				if hasCurve {
					idx, x, y := band()
					lo, hi := s.Values[idx].Min, s.Values[idx].Max
					t := 0.0
					if y != x {
						t = (q - x) / (y - x)
					}
					val = lo + (hi-lo)*t
				} else {
					val = s.Values[0].Min
				}
				if rd == RoundHalfUp {
					val = math.Floor(val + 0.5)
				}
				sum[s.Stat] += val
			}
		}
		rarity := 0
		if hasCurve {
			rarity, _, _ = band()
		}
		sets, err := db.ItemSets(it.Record)
		if err != nil {
			return nil, err
		}
		for _, s := range sets {
			if _, ok := setRarities[s]; !ok {
				setOrder = append(setOrder, s)
			}
			setRarities[s] = append(setRarities[s], rarity)
		}
	}
	sort.Slice(setOrder, func(i, j int) bool { return setOrder[i] < setOrder[j] })
	for _, guid := range setOrder {
		rars := setRarities[guid]
		low := rars[0]
		for _, r := range rars {
			if r < low {
				low = r
			}
		}
		tiers, err := db.SetBonus(guid)
		if err != nil {
			return nil, err
		}
		for _, tier := range tiers {
			if tier.Pieces > len(rars) {
				continue
			}
			for _, e := range tier.Entries {
				var val float64
				if e.Has[low] {
					val = e.Values[low]
				}
				sum[e.Stat] += val
			}
		}
	}
	out := make([]Mod, 0, len(sum))
	for guid, v := range sum {
		out = append(out, Mod{Stat: guid, Value: v})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Stat < out[j].Stat })
	return out, nil
}

func tableTags(db *content.DB, items []Item) ([]EquippedTag, error) {
	var out []EquippedTag
	for _, it := range items {
		tags, err := db.ItemTags(it.Record)
		if err != nil {
			return nil, err
		}
		for _, t := range tags {
			out = append(out, EquippedTag{Slot: it.Slot, Tag: t})
		}
	}
	return out, nil
}

func sameMods(a, b []Mod) error {
	if len(a) != len(b) {
		return fmt.Errorf("%d mods and %d", len(a), len(b))
	}
	for i := range a {
		if a[i].Stat != b[i].Stat || math.Float64bits(a[i].Value) != math.Float64bits(b[i].Value) {
			return fmt.Errorf("mod %d: stat %d value %v and stat %d value %v", i, a[i].Stat, a[i].Value, b[i].Stat, b[i].Value)
		}
	}
	return nil
}

func openBoth(t *testing.T) (*Gear, *content.DB) {
	t.Helper()
	path := contentDBPath(t)
	gear, err := OpenGear(path)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { gear.Close() })
	db, err := content.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { db.Close() })
	if err := db.RequireVersion(); err != nil {
		t.Fatal(err)
	}
	return gear, db
}

// TestGearTablesEqualTheRecords: every item of the design database alone, at six qualities across the bands and both roundings,
// then 400 random sets of up to 20 worn items (with whole sets worn, so that the tiers apply), each at a random quality.
func TestGearTablesEqualTheRecords(t *testing.T) {
	gear, db := openBoth(t)
	rows, err := db.SQL().Query(`SELECT guid FROM item ORDER BY guid`)
	if err != nil {
		t.Fatal(err)
	}
	var guids []int64
	for rows.Next() {
		var g int64
		if err := rows.Scan(&g); err != nil {
			t.Fatal(err)
		}
		guids = append(guids, g)
	}
	rows.Close()
	if len(guids) != 10568 {
		t.Fatalf("%d items", len(guids))
	}
	slots := []string{"Head", "Chest", "Primary", "Secondary", "Ranged", "Slot26"}
	bad := 0
	check := func(what string, items []Item, rd Rounding) {
		want, err := gear.Mods(items, rd)
		if err != nil {
			t.Fatalf("%s: Gear.Mods: %v", what, err)
		}
		got, err := tableMods(db, items, rd)
		if err != nil {
			t.Fatalf("%s: tableMods: %v", what, err)
		}
		if err := sameMods(want, got); err != nil && bad < 10 {
			bad++
			t.Errorf("%s (rounding %d): %v", what, rd, err)
		}
		wt, err := gear.Tags(items)
		if err != nil {
			t.Fatal(err)
		}
		gt, err := tableTags(db, items)
		if err != nil {
			t.Fatal(err)
		}
		if len(wt) != len(gt) {
			t.Errorf("%s: %d tags and %d", what, len(wt), len(gt))
			return
		}
		for i := range wt {
			if wt[i] != gt[i] && bad < 10 {
				bad++
				t.Errorf("%s: tag %d is %v and %v", what, i, wt[i], gt[i])
			}
		}
	}
	for i, g := range guids {
		for j, q := range []int{1, 7, 20, 40, 75, 150} {
			check(fmt.Sprintf("item %d at quality %d", g, q), []Item{{Record: g, Quality: q, Slot: slots[(i+j)%len(slots)]}}, Rounding((i+j)%2))
		}
	}
	// random worn sets: items of the same set together so that tiers apply, and loose items
	bySet := map[int64][]int64{}
	srows, err := db.SQL().Query(`SELECT set_guid, item_guid FROM item_set ORDER BY set_guid, item_guid`)
	if err != nil {
		t.Fatal(err)
	}
	var setGuids []int64
	for srows.Next() {
		var s, it int64
		if err := srows.Scan(&s, &it); err != nil {
			t.Fatal(err)
		}
		if _, ok := bySet[s]; !ok {
			setGuids = append(setGuids, s)
		}
		bySet[s] = append(bySet[s], it)
	}
	srows.Close()
	rng := rand.New(rand.NewSource(20261002))
	tiersApplied := 0
	for n := 0; n < 400; n++ {
		var items []Item
		if n%4 != 3 { // three in four wear pieces of one or two sets
			for k := 0; k < 1+n%2; k++ {
				members := bySet[setGuids[rng.Intn(len(setGuids))]]
				for _, m := range rng.Perm(len(members))[:min(len(members), 2+rng.Intn(7))] {
					items = append(items, Item{Record: members[m], Quality: 1 + rng.Intn(200), Slot: slots[rng.Intn(len(slots))]})
				}
			}
		}
		for len(items) < 3+rng.Intn(18) {
			items = append(items, Item{Record: guids[rng.Intn(len(guids))], Quality: 1 + rng.Intn(200), Slot: slots[rng.Intn(len(slots))]})
		}
		check(fmt.Sprintf("worn set %d", n), items, Rounding(n%2))
		if m, _ := tableMods(db, items, RoundNone); len(m) > 0 {
			tiersApplied++
		}
	}
	if tiersApplied == 0 {
		t.Error("no random set had any mod")
	}
	if bad > 0 {
		t.Errorf("%d mismatches (the first ten are listed)", bad)
	}
}

// TestStatProgramInTheContentDB: the bundle stored in content.db decodes, is the compile of the file's own graph and agrees
// with its header.
func TestStatProgramInTheContentDB(t *testing.T) {
	_, db := openBoth(t)
	p, err := db.StatProgram()
	if err != nil {
		t.Fatal(err)
	}
	b, err := Decode(p.Data)
	if err != nil {
		t.Fatalf("the stored bundle does not verify: %v", err)
	}
	if int(binary.LittleEndian.Uint16(p.Data[4:6])) != p.Version || len(b.Stats) != p.Stats || len(b.Classes) != p.Classes || b.Mode.String() != p.Mode ||
		binary.LittleEndian.Uint32(p.Data[12:16]) != p.CRC32 {
		t.Errorf("the row (%+v without data) disagrees with the bundle", struct {
			Version, Stats, Classes int
			Mode                    string
		}{p.Version, p.Stats, p.Classes, p.Mode})
	}
	fresh, _, _, _ := compileRealExprs(t)
	if !bytes.Equal(fresh.Encode(), p.Data) {
		t.Errorf("the stored bundle is not what compiling the file's graph and expressions gives")
	}
}

// SlotName names every equip slot the way worldd's catalog does: from the equip_slot table of the file.
func TestSlotNamesAreTheEquipSlotTable(t *testing.T) {
	_, db := openBoth(t)
	names, err := db.EquipSlotNames()
	if err != nil {
		t.Fatal(err)
	}
	if len(names) != len(equipSlotNames) {
		t.Errorf("equip_slot has %d names, SlotName knows %d", len(names), len(equipSlotNames))
	}
	for slot, want := range names {
		if got := SlotName(slot); got != want {
			t.Errorf("SlotName(%d) = %q, the equip_slot table says %q", slot, got, want)
		}
	}
}

func TestSlotNameOfANumberThatIsNoSlot(t *testing.T) {
	for n, want := range map[int]string{0: "Head", 2: "Chest", 15: "Primary", 23: "ResourceBag_0", 35: "FishingRod", 83: "Slot83", -1: "Slot-1", 1000: "Slot1000"} {
		if got := SlotName(n); got != want {
			t.Errorf("SlotName(%d) = %q, want %q", n, got, want)
		}
	}
}
