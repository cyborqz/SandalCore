// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

// Plan B3's tables: quality_curve, stat_block_stat, item_set, set_bonus and item_tag, and item.quality_curve_guid. The synthetic tests
// build them from rows made here; the real file's tables are checked against the records in services/internal/statprog.

import (
	"context"
	"database/sql"
	"io"
	"math"
	"path/filepath"
	"strings"
	"testing"

	"aocrop/services/internal/content"
	"aocrop/services/internal/designdb"
)

const (
	tSetA = 9000000000000000001
	tSetB = 9000000000000000002
	sDex  = 112
)

func setBonusRow(guid int64, name string, tiers designdb.Pairs) designdb.Row {
	return designdb.Row{Guid: guid, Name: name, Fields: designdb.Struct{"SetStatBonuses": tiers}}
}

func setTier(pieces int64, bonuses ...designdb.Value) designdb.Pair {
	if bonuses == nil {
		bonuses = []designdb.Value{}
	}
	return designdb.Pair{Key: pieces, Value: designdb.Struct{"StatBonuses": bonuses}}
}

func setBonus(stat int64, byRarity designdb.Pairs) designdb.Struct {
	return designdb.Struct{"AffectedStat": idStruct(stat), "StatBonuses": byRarity}
}

// (C:Y:B + CoA)

func rarityValue(name string, v float32) designdb.Pair {
	return designdb.Pair{Key: "EItemRarity::" + name, Value: v}
}

func tagList(tags ...string) []designdb.Value {
	out := []designdb.Value{}
	for _, t := range tags {
		out = append(out, t)
	}
	return out
}

func idList(guids ...int64) []designdb.Value {
	out := []designdb.Value{}
	for _, g := range guids {
		out = append(out, idStruct(g))
	}
	return out
}

// gearSource is itemSource with the fields buildGear reads: tags and sets on the items and two SetBonus records.
func gearSource() *source {
	src := itemSource()
	src.typeIDs["SetBonusRecordBase"] = content.TypeSetBonus
	src.rows["StatTypeDefRecordBase"] = append(src.rows["StatTypeDefRecordBase"], designdb.Row{Guid: sDex, Name: "Dexterity"})
	for i := range src.rows["ItemRecordBase"] {
		r := &src.rows["ItemRecordBase"][i]
		r.Fields["GameplayTags"] = tagList()
		r.Fields["SetBonusIds"] = idList()
		switch r.Guid {
		case tChest:
			r.Fields["GameplayTags"] = tagList("Item.Gear.Armor.Light", "Slot.Chest")
			r.Fields["SetBonusIds"] = idList(tSetA)
		case tScepter:
			r.Fields["GameplayTags"] = tagList("Item.Gear.Weapon.Scepter")
			r.Fields["SetBonusIds"] = idList(tSetB, tSetA) // the record's order is kept
		}
	}
	src.rows["SetBonusRecordBase"] = []designdb.Row{
		setBonusRow(tSetA, "SetA", designdb.Pairs{
			setTier(2, // two bonuses, the first for Common and Rare only
				setBonus(111, designdb.Pairs{rarityValue("Common", 10), rarityValue("Rare", 30)}),
				setBonus(sDex, designdb.Pairs{})),
			setTier(4, setBonus(111, designdb.Pairs{rarityValue("Common", 5), rarityValue("Common", 6), rarityValue("Epic", 9), {Key: "EItemRarity::Mythic", Value: float32(99)}})),
			setTier(6), // a tier with nothing to give
		}),
		setBonusRow(tSetB, "SetB", designdb.Pairs{setTier(3, setBonus(424242, designdb.Pairs{rarityValue("Common", 1)}))}), // a stat that is no StatTypeDef
	}
	return src
}

func buildGearSynthetic(t *testing.T, src *source) (*content.DB, map[string]string, error) {
	t.Helper()
	path := filepath.Join(t.TempDir(), "content.db")
	sdb, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatal(err)
	}
	defer sdb.Close()
	sdb.SetMaxOpenConns(1)
	if err := content.Create(sdb); err != nil {
		t.Fatal(err)
	}
	tx, err := sdb.BeginTx(context.Background(), nil)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback()
	w := &writer{tx: tx, stmts: map[string]*sql.Stmt{}}
	meta := map[string]string{}
	opt := &Options{Log: io.Discard}
	if err := buildItems(w, src, itemEnums(), opt, meta); err != nil {
		return nil, nil, err
	}
	if err := buildGear(w, src, opt, meta); err != nil {
		return nil, nil, err
	}
	if err := w.close(); err != nil {
		t.Fatal(err)
	}
	if _, err := tx.Exec(`INSERT INTO meta VALUES (?, ?), (?, ?)`, content.MetaSchemaVersion, "1", content.MetaRecordEncoding, content.EncodingJSON); err != nil {
		t.Fatal(err)
	}
	if err := tx.Commit(); err != nil {
		t.Fatal(err)
	}
	sdb.Close()
	db, err := content.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { db.Close() })
	return db, meta, nil
}

func TestSyntheticGear(t *testing.T) {
	db, meta, err := buildGearSynthetic(t, gearSource())
	if err != nil {
		t.Fatal(err)
	}
	if meta["typed.quality_curve"] != "1" || meta["typed.stat_block_stat"] != "7" || meta["typed.set_bonus"] != "4" || meta["typed.item_tag"] != "3" ||
		meta["typed.stat_block_unknown_stats"] != "1" {
		t.Errorf("meta = %v", meta)
	}

	// the curve and the item's reference to it
	bands, err := db.QualityCurve(tCurve)
	if err != nil || bands[0] != (content.QualityBand{X: 1, Y: 5}) || bands[3] != (content.QualityBand{X: 31, Y: 50}) || bands[6] != (content.QualityBand{X: 101, Y: 200}) {
		t.Errorf("QualityCurve = %v, %v", bands, err)
	}
	if it, err := db.Item(tScepter); err != nil || it.QualityCurveGuid != tCurve {
		t.Errorf("the Scepter's curve = %d, %v", it.QualityCurveGuid, err)
	}
	if it, err := db.Item(tBag); err != nil || it.QualityCurveGuid != 0 {
		t.Errorf("the Bag has curve %d, %v, want none", it.QualityCurveGuid, err)
	}

	// a stat block: every stat, every rarity
	block, err := db.StatBlock(tBlockMix)
	if err != nil || len(block) != 2 || block[0].Stat != 111 || block[1].Stat != content.GuidMaxDurability {
		t.Fatalf("StatBlock(tBlockMix) = %+v, %v", block, err)
	}
	if block[0].Values[0] != (content.RarityValue{Min: 20, Max: 20}) || block[0].Values[1] != (content.RarityValue{}) ||
		block[1].Values[0] != (content.RarityValue{Min: 10, Max: 20}) || block[1].Values[1] != (content.RarityValue{Min: 30, Max: 40}) {
		t.Errorf("the block's values = %+v", block)
	}

	// tags and sets in the record's order
	if tags, err := db.ItemTags(tChest); err != nil || len(tags) != 2 || tags[0] != "Item.Gear.Armor.Light" || tags[1] != "Slot.Chest" {
		t.Errorf("the Chest's tags = %v, %v", tags, err)
	}
	if tags, err := db.ItemTags(tBag); err != nil || len(tags) != 0 {
		t.Errorf("the Bag's tags = %v, %v", tags, err)
	}
	if sets, err := db.ItemSets(tScepter); err != nil || len(sets) != 2 || sets[0] != tSetB || sets[1] != tSetA {
		t.Errorf("the Scepter's sets = %v, %v, want the record's order", sets, err)
	}

	// SetA: tier 2 with a bonus for Common and Rare (the others none) and one with no rarity at all; tier 4 where the last
	// Common wins and a key that is no rarity is left out; tier 6 has no bonus and so no row
	tiers, err := db.SetBonus(tSetA)
	if err != nil || len(tiers) != 2 || tiers[0].Pieces != 2 || tiers[1].Pieces != 4 || len(tiers[0].Entries) != 2 || len(tiers[1].Entries) != 1 {
		t.Fatalf("SetBonus(tSetA) = %+v, %v", tiers, err)
	}
	e := tiers[0].Entries[0]
	if e.Stat != 111 || !e.Has[0] || e.Values[0] != 10 || e.Has[1] || !e.Has[2] || e.Values[2] != 30 || e.Has[6] {
		t.Errorf("tier 2's first bonus = %+v", e)
	}
	if e := tiers[0].Entries[1]; e.Stat != sDex || e.Has != [7]bool{} {
		t.Errorf("tier 2's second bonus = %+v, want a row with no rarity", e)
	}
	e = tiers[1].Entries[0]
	if !e.Has[0] || e.Values[0] != 6 || !e.Has[4] || e.Values[4] != 9 || e.Has[1] || e.Has[5] || e.Has[6] {
		t.Errorf("tier 4's bonus = %+v, want Common 6 (the last) and Epic 9", e)
	}
	if b, err := db.SetBonus(tSetB); err != nil || len(b) != 1 || b[0].Entries[0].Stat != 424242 {
		t.Errorf("SetB = %+v, %v: a stat that is no StatTypeDef is kept", b, err)
	}
}

// [C O A x C:Y:B]

func TestSyntheticGearRefusals(t *testing.T) {
	rowOf := func(src *source, schema string, guid int64) *designdb.Row {
		for i := range src.rows[schema] {
			if src.rows[schema][i].Guid == guid {
				return &src.rows[schema][i]
			}
		}
		t.Fatalf("no %s row %d", schema, guid)
		return nil
	}
	for name, tc := range map[string]struct {
		change func(*source)
		want   string
	}{
		"a SetBonus table of the wrong TypeId": {func(s *source) { s.typeIDs["SetBonusRecordBase"] = 1 }, "SetBonusRecordBase has TypeId 1"},
		"no SetBonus table":                    {func(s *source) { delete(s.rows, "SetBonusRecordBase") }, "no decoded SetBonusRecordBase"},
		"an item in a set that is none": {func(s *source) {
			rowOf(s, "ItemRecordBase", tChest).Fields["SetBonusIds"] = idList(77)
		}, "its set 77 is no SetBonus"},
		"an item with a curve that is none": {func(s *source) {
			rowOf(s, "ItemRecordBase", tChest).Fields["QualityCurveId"] = idStruct(77)
		}, "its quality curve 77 is no QualityCurve"},
		"an item with a stat block that is none": {func(s *source) {
			rowOf(s, "ItemRecordBase", tChest).Fields["StatBlockId"] = idStruct(77)
		}, "its stat block 77 is no ItemStatBlock"},
		"an item without tags": {func(s *source) { delete(rowOf(s, "ItemRecordBase", tChest).Fields, "GameplayTags") }, "no field GameplayTags"},
		"a stat twice in a block": {func(s *source) {
			f := rowOf(s, "ItemStatBlockRecordBase", tBlock).Fields
			f["Stats"] = append(f["Stats"].(designdb.Pairs), f["Stats"].(designdb.Pairs)[0])
		}, "appears twice"},
		"a value that is not finite": {func(s *source) {
			f := rowOf(s, "ItemStatBlockRecordBase", tBlock).Fields
			f["Stats"].(designdb.Pairs)[0].Value.(designdb.Struct)["RareMax"] = float32(math.Inf(1))
		}, "is not finite"},
		"a block value that is no float": {func(s *source) {
			f := rowOf(s, "ItemStatBlockRecordBase", tBlock).Fields
			f["Stats"].(designdb.Pairs)[0].Value.(designdb.Struct)["RareMax"] = "lots"
		}, "RareMax is string"},
		"a curve whose band runs backwards": {func(s *source) {
			rowOf(s, "QualityCurveRecordBase", tCurve).Fields["Rare"] = designdb.Struct{"X": 30.0, "Y": 16.0}
		}, "covers 30 to 16"},
		"a curve without a rarity": {func(s *source) { delete(rowOf(s, "QualityCurveRecordBase", tCurve).Fields, "Epic") }, "no field Epic"},
		"a set bonus value that is no number": {func(s *source) {
			f := rowOf(s, "SetBonusRecordBase", tSetA).Fields
			f["SetStatBonuses"].(designdb.Pairs)[0].Value.(designdb.Struct)["StatBonuses"].([]designdb.Value)[0].(designdb.Struct)["StatBonuses"] =
				designdb.Pairs{{Key: "EItemRarity::Common", Value: "ten"}}
		}, "the value of EItemRarity::Common is a string"},
		"a set bonus value that is not finite": {func(s *source) {
			f := rowOf(s, "SetBonusRecordBase", tSetA).Fields
			f["SetStatBonuses"].(designdb.Pairs)[0].Value.(designdb.Struct)["StatBonuses"].([]designdb.Value)[0].(designdb.Struct)["StatBonuses"] =
				designdb.Pairs{{Key: "EItemRarity::Common", Value: float32(math.NaN())}}
		}, "is not finite"},
		"a tier whose key is no number": {func(s *source) {
			f := rowOf(s, "SetBonusRecordBase", tSetA).Fields
			f["SetStatBonuses"].(designdb.Pairs)[0].Key = "two"
		}, "SetStatBonuses[0] is a string"},
	} {
		src := gearSource()
		tc.change(src)
		if _, _, err := buildGearSynthetic(t, src); err == nil || !strings.Contains(err.Error(), tc.want) {
			t.Errorf("%s: buildGear = %v, want an error containing %q", name, err, tc.want)
		}
	}
}

// buildGear and buildItems agree on what a record is: the synthetic item test's source (rows without the gear fields) still builds items.
func TestItemsWithoutGearFieldsStillBuild(t *testing.T) {
	if _, _, err := buildItemsSynthetic(t, itemSource(), itemEnums()); err != nil {
		t.Fatal(err)
	}
}
