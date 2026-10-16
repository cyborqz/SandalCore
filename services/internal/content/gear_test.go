// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package content

import (
	"database/sql"
	"strings"
	"testing"
)

// openWith makes a content.db whose meta has contentc.version `version` (none when empty), lets setup fill it and opens it.
func openWith(t *testing.T, version string, setup func(db *sql.DB)) *DB {
	t.Helper()
	path := newDB(t, "1")
	raw, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatal(err)
	}
	if version != "" {
		exec(t, raw, `INSERT INTO meta (key, value) VALUES (?, ?)`, MetaContentcVersion, version)
	}
	if setup != nil {
		setup(raw)
	}
	raw.Close()
	db, err := Open(path)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { db.Close() })
	return db
}

func TestRequireVersionRefusesAnOlderContentc(t *testing.T) {
	withProgram := func(db *sql.DB) {
		exec(t, db, `INSERT INTO stat_program (id, version, mode, stats, classes, crc32, data) VALUES (1, 1, 'float32', 1, 1, 7, x'0102')`)
	}
	for name, tc := range map[string]struct {
		version string
		setup   func(*sql.DB)
		want    string // "" accepted
	}{
		"version 3 (plan B2)":            {"3", withProgram, "built by contentc version 3, this checkout needs version 15"},
		"version 4 (plan B3)":            {"4", withProgram, "built by contentc version 4, this checkout needs version 15"},
		"version 5 (plan D1)":            {"5", withProgram, "built by contentc version 5, this checkout needs version 15"},
		"version 6 (plan D2)":            {"6", withProgram, "built by contentc version 6, this checkout needs version 15"},
		"no version at all":              {"", withProgram, "not written by contentc"},
		"a version that is no number":    {"four", withProgram, "not written by contentc"},
		"version 7 (plan E1, stage 1)":   {"7", withProgram, "built by contentc version 7, this checkout needs version 15"},
		"version 8 (plan E1, stage 2)":   {"8", withProgram, "built by contentc version 8, this checkout needs version 15"},
		"version 9 (plan E1, stage 3)":   {"9", withProgram, "built by contentc version 9, this checkout needs version 15"},
		"version 10 (plan E1, stage 3)":  {"10", withProgram, "built by contentc version 10, this checkout needs version 15"},
		"version 11 (the mounts)":        {"11", withProgram, "built by contentc version 11, this checkout needs version 15"},
		"version 12 (Nodes 1)":           {"12", withProgram, "built by contentc version 12, this checkout needs version 15"},
		"version 13 (Gathering stage 2)": {"13", withProgram, "built by contentc version 13, this checkout needs version 15"},
		"version 14 (Gathering stage 3)": {"14", withProgram, "built by contentc version 14, this checkout needs version 15"},
		"version 15 without the row":     {"15", nil, "stat_program table has 0 rows"},
		"version 15 with the stat row":   {"15", withProgram, ""},
		"a later version":                {"16", withProgram, ""},
	} {
		db := openWith(t, tc.version, tc.setup)
		err := db.RequireVersion()
		if tc.want == "" {
			if err != nil {
				t.Errorf("%s: RequireVersion = %v", name, err)
			}
			continue
		}
		if err == nil || !strings.Contains(err.Error(), tc.want) || !strings.Contains(err.Error(), "go run ./cmd/contentc") {
			t.Errorf("%s: RequireVersion = %v, want an error with %q and the rebuild command", name, err, tc.want)
		}
	}
	// a file with no stat_program table at all (a content.db of an older schema) says so
	old := openWith(t, "15", func(db *sql.DB) { exec(t, db, `DROP TABLE stat_program`) })
	if err := old.RequireVersion(); err == nil || !strings.Contains(err.Error(), "no stat program table") {
		t.Errorf("RequireVersion without the table = %v", err)
	}
}

// C O A captures, C.Y.B code

func TestStatProgramRow(t *testing.T) {
	db := openWith(t, "4", func(db *sql.DB) {
		exec(t, db, `INSERT INTO stat_program (id, version, mode, stats, classes, crc32, data) VALUES (1, 1, 'float32', 473, 8, 4294967295, x'41414141')`)
	})
	p, err := db.StatProgram()
	if err != nil {
		t.Fatal(err)
	}
	if p.Version != 1 || p.Mode != "float32" || p.Stats != 473 || p.Classes != 8 || p.CRC32 != 0xFFFFFFFF || string(p.Data) != "AAAA" {
		t.Errorf("StatProgram = %+v", p)
	}
	empty := openWith(t, "4", nil)
	if _, err := empty.StatProgram(); err == nil || !strings.Contains(err.Error(), "no stat program") {
		t.Errorf("StatProgram of a file without one = %v", err)
	}
}

func TestGearReaders(t *testing.T) {
	db := openWith(t, "4", func(db *sql.DB) {
		for i, b := range [][2]float64{{1, 5}, {6, 15}, {16, 30}, {31, 50}, {51, 70}, {71, 100}, {101, 200}} {
			exec(t, db, `INSERT INTO quality_curve (guid, ord, x, y) VALUES (9, ?, ?, ?)`, i, b[0], b[1])
		}
		exec(t, db, `INSERT INTO quality_curve (guid, ord, x, y) VALUES (8, 0, 1, 5)`) // a curve with one rarity
		exec(t, db, `INSERT INTO stat_block_stat VALUES (5, 100, 1,2, 3,4, 5,6, 7,8, 9,10, 11,12, 13,14), (5, 101, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0)`)
		exec(t, db, `INSERT INTO item_set VALUES (1, 1, 77), (1, 0, 76)`)
		exec(t, db, `INSERT INTO item_tag VALUES (1, 1, 'Slot.Chest'), (1, 0, 'Item.Gear.Armor.Light')`)
		// set 76: tier 0 (2 pieces) has two bonuses, one with a value at Common and Rare only; tier 1 (4 pieces) has one
		exec(t, db, `INSERT INTO set_bonus (set_guid, tier_ord, pieces, bonus_ord, stat_guid, common, rare) VALUES (76, 0, 2, 0, 100, 10, 30), (76, 0, 2, 1, 101, NULL, NULL), (76, 1, 4, 0, 102, 5, NULL)`)
	})
	bands, err := db.QualityCurve(9)
	if err != nil || bands[2] != (QualityBand{16, 30}) || bands[6] != (QualityBand{101, 200}) {
		t.Errorf("QualityCurve(9) = %v, %v", bands, err)
	}
	if _, err := db.QualityCurve(8); err == nil || !strings.Contains(err.Error(), "has 1 rarities") {
		t.Errorf("QualityCurve(8) = %v, want the missing rarities", err)
	}
	if _, err := db.QualityCurve(7); err == nil {
		t.Errorf("QualityCurve of a curve that is none = nil")
	}
	block, err := db.StatBlock(5)
	if err != nil || len(block) != 2 || block[0].Stat != 100 || block[0].Values[3] != (RarityValue{7, 8}) || block[0].Values[6] != (RarityValue{13, 14}) {
		t.Errorf("StatBlock(5) = %+v, %v", block, err)
	}
	if none, err := db.StatBlock(6); err != nil || len(none) != 0 {
		t.Errorf("StatBlock(6) = %+v, %v, want none", none, err)
	}
	sets, err := db.ItemSets(1)
	if err != nil || len(sets) != 2 || sets[0] != 76 || sets[1] != 77 {
		t.Errorf("ItemSets(1) = %v, %v, want the record's order 76, 77", sets, err)
	}
	tags, err := db.ItemTags(1)
	if err != nil || len(tags) != 2 || tags[0] != "Item.Gear.Armor.Light" || tags[1] != "Slot.Chest" {
		t.Errorf("ItemTags(1) = %v, %v", tags, err)
	}
	tiers, err := db.SetBonus(76)
	if err != nil || len(tiers) != 2 || tiers[0].Pieces != 2 || len(tiers[0].Entries) != 2 || tiers[1].Pieces != 4 {
		t.Fatalf("SetBonus(76) = %+v, %v", tiers, err)
	}
	e := tiers[0].Entries[0]
	if e.Stat != 100 || !e.Has[0] || e.Values[0] != 10 || e.Has[1] || !e.Has[2] || e.Values[2] != 30 {
		t.Errorf("the first bonus = %+v", e)
	}
	if e := tiers[0].Entries[1]; e.Stat != 101 || e.Has != [7]bool{} {
		t.Errorf("a bonus with no rarity value = %+v, want none present", e)
	}
	if none, err := db.SetBonus(1); err != nil || len(none) != 0 {
		t.Errorf("SetBonus of no set = %+v, %v", none, err)
	}
}

// A set_bonus row with a negative tier_ord (a file this checkout's contentc never writes) used to index tiers[-1]: it is refused.
func TestSetBonusRefusesANegativeTierOrder(t *testing.T) {
	for name, rows := range map[string]string{
		"the first row":     `(5, -1, 2, 0, 100, 10), (5, 0, 4, 0, 101, 20)`,
		"the only row":      `(5, -3, 2, 0, 100, 10)`,
		"after a good tier": `(5, 0, 2, 0, 100, 10), (5, 1, 4, 0, 101, 20)`, // control: accepted
	} {
		db := openWith(t, "4", func(db *sql.DB) {
			exec(t, db, `INSERT INTO set_bonus (set_guid, tier_ord, pieces, bonus_ord, stat_guid, common) VALUES `+rows)
		})
		tiers, err := db.SetBonus(5)
		if name == "after a good tier" {
			if err != nil || len(tiers) != 2 {
				t.Errorf("%s: SetBonus = %+v, %v", name, tiers, err)
			}
			continue
		}
		if err == nil || !strings.Contains(err.Error(), "negative tier order") || len(tiers) != 0 {
			t.Errorf("%s: SetBonus = %+v, %v, want the negative tier refused", name, tiers, err)
		}
	}
}
