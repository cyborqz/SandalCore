// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

// Plan B2's tables: item, item_equip_slot, equip_slot and starting_equipment. The synthetic tests build them from rows made here
// (so the arithmetic of a fresh item's durability and every refusal are checked on numbers chosen for them); TestRealItems
// reads the real file, as the other real tests do, and checks the facts the captured retail Bard fixes.

import (
	"context"
	"database/sql"
	"io"
	"path/filepath"
	"strings"
	"testing"

	"aocrop/services/internal/content"
	"aocrop/services/internal/designdb"
	"aocrop/services/internal/usmap"
)

const (
	tChest    = 6000000000000000001
	tScepter  = 6000000000000000002
	tFocus    = 6000000000000000003
	tSword2H  = 6000000000000000004
	tBag      = 6000000000000000005
	tPotion   = 6000000000000000006
	tNoStat   = 6000000000000000007 // a stat block with no MaxDurability
	tHalf     = 6000000000000000008 // 37.5: rounds to 38
	tNoCurve  = 6000000000000000009 // a stat block but no quality curve: CommonMin
	tBlock    = 7000000000000000001
	tBlockMix = 7000000000000000002
	tBlockNoD = 7000000000000000003
	tBlockHlf = 7000000000000000004
	tCurve    = 8000000000000000001
)

func itemEnums() *usmap.Map {
	names := []string{"Head", "Helmet", "Chest", "Wrists", "Hands", "Legs", "Feet", "Shoulders", "Back", "Belt", "RingRight", "RingLeft", "Necklace",
		"EarRight", "EarLeft", "Primary", "Secondary", "Ranged", "RangedOffHand"}
	e := &usmap.Enum{Name: "EEquipSlot"}
	for i, n := range names {
		e.Values = append(e.Values, usmap.EnumValue{Name: n, Value: int64(i)})
		if n == "Head" {
			e.Values = append(e.Values, usmap.EnumValue{Name: "_MAIN_EQUIPMENT_CHECK_START", Value: 0}) // a pseudo name for the same value
		}
		if n == "Primary" {
			e.Values = append(e.Values, usmap.EnumValue{Name: "_WEAPON_CHECK_START", Value: 15})
		}
	}
	e.Values = append(e.Values, usmap.EnumValue{Name: "Shirt", Value: 19}, usmap.EnumValue{Name: "ResourceBag_0", Value: 23},
		usmap.EnumValue{Name: "cCount", Value: 83}, usmap.EnumValue{Name: "EEquipSlot_MAX", Value: 84})
	return usmap.New([]*usmap.Enum{e}, nil)
}

func idStruct(guid int64) designdb.Struct {
	return designdb.Struct{"Guid": guid, "Name": "None", "TypeId": int64(0)}
}

func band(min, max float32) []designdb.Value { return []designdb.Value{min, max} }

// statBlock is an ItemStatBlock whose MaxDurability is the given Min and Max of each rarity (Common first).
func statBlock(guid int64, name string, durability map[string][2]float32) designdb.Row {
	var stats designdb.Pairs
	stats = append(stats, designdb.Pair{Key: int64(111), Value: valuesOf(map[string][2]float32{"Common": {20, 20}})}) // another stat
	if durability != nil {
		stats = append(stats, designdb.Pair{Key: int64(content.GuidMaxDurability), Value: valuesOf(durability)})
	}
	return designdb.Row{Guid: guid, Name: name, Fields: designdb.Struct{"Stats": stats}}
}

func valuesOf(by map[string][2]float32) designdb.Struct {
	s := designdb.Struct{}
	for _, r := range rarities {
		v := by[r]
		s[r+"Min"], s[r+"Max"] = v[0], v[1]
	}
	return s
}

// ~ Cyb - C-O-A ~

func itemRow(guid int64, name string, canEquip, all bool, quality int64, block, curve int64, slots ...string) designdb.Row {
	var list []designdb.Value
	for _, s := range slots {
		list = append(list, "EEquipSlot::"+s)
	}
	if list == nil {
		list = []designdb.Value{}
	}
	return designdb.Row{Guid: guid, Name: name, Fields: designdb.Struct{
		"CanEquip": canEquip, "OccupiesAllEquipSlots": all, "Level": int64(1), "DefaultQuality": quality, "MaxStackSize": int64(1),
		"StatBlockId": idStruct(block), "QualityCurveId": idStruct(curve), "AppearanceId": idStruct(0), "EquipSlots": list}}
}

func kitRow(guid int64, class string, equipped designdb.Pairs, stored ...designdb.Value) designdb.Row {
	if stored == nil {
		stored = []designdb.Value{}
	}
	return designdb.Row{Guid: guid, Name: class + "_StartingEquipment", Fields: designdb.Struct{
		"Class": "ECharacterClass::" + class, "EquippedItems": equipped, "StoredItems": stored}}
}

func variant(item int64) designdb.Struct {
	return designdb.Struct{"ItemId": idStruct(item), "VariationId": idStruct(0)}
}

func equippedItem(slot string, item int64) designdb.Pair {
	return designdb.Pair{Key: "EEquipSlot::" + slot, Value: variant(item)}
}

// itemSource is the rows buildItems reads: six worn or carried items with the durabilities the tests check, and a kit for each class.
func itemSource() *source {
	src := &source{rows: map[string][]designdb.Row{}, typeIDs: map[string]uint64{}}
	for schema, id := range itemRecordTypes {
		src.typeIDs[schema] = id
	}
	// quality 1-5 Common, 6-15 Uncommon ... as the real curve
	curve := designdb.Struct{}
	for i, r := range rarities {
		x := []float64{1, 6, 16, 31, 51, 71, 101}[i]
		y := []float64{5, 15, 30, 50, 70, 100, 200}[i]
		curve[r] = designdb.Struct{"X": x, "Y": y}
	}
	src.rows["QualityCurveRecordBase"] = []designdb.Row{{Guid: tCurve, Name: "Default Quality Curve", Fields: curve}}
	src.rows["StatTypeDefRecordBase"] = []designdb.Row{{Guid: content.GuidMaxDurability, Name: "MaxDurability"}, {Guid: 111, Name: "Strength"}}
	src.rows["ItemStatBlockRecordBase"] = []designdb.Row{
		statBlock(tBlock, "ChestBlock", map[string][2]float32{"Common": {11, 11}, "Uncommon": {12, 12}}),
		statBlock(tBlockMix, "MixBlock", map[string][2]float32{"Common": {10, 20}, "Uncommon": {30, 40}}),
		statBlock(tBlockNoD, "NoDurabilityBlock", nil),
		statBlock(tBlockHlf, "HalfBlock", map[string][2]float32{"Common": {37.5, 37.5}}),
	}
	src.rows["ItemRecordBase"] = []designdb.Row{
		itemRow(tChest, "Chest", false, false, 1, tBlock, tCurve, "Chest"),                        // CanEquip false and worn (the starter armor)
		itemRow(tScepter, "Scepter", true, false, 3, tBlockMix, tCurve, "Primary"),                // quality 3: 10 + 10 * (3-1)/(5-1) = 15
		itemRow(tFocus, "Focus", true, false, 7, tBlockMix, tCurve, "Secondary", "RangedOffHand"), // quality 7: Uncommon, 30 + 10 * (7-6)/(15-6)
		itemRow(tSword2H, "Sword2H", true, true, 1, tBlockHlf, tCurve, "Primary", "Secondary"),
		itemRow(tBag, "Bag", true, false, 1, 0, 0, "ResourceBag_0"),
		itemRow(tPotion, "Potion", false, false, 1, 0, 0),
		itemRow(tNoStat, "NoStat", true, false, 1, tBlockNoD, tCurve, "Shirt"),
		itemRow(tHalf, "Half", true, false, 1, tBlockHlf, tCurve, "Head"),
		itemRow(tNoCurve, "NoCurve", true, false, 4, tBlockMix, 0, "Belt"),
	}
	classRows := []designdb.Row{}
	kits := []designdb.Row{}
	for i, name := range content.PlayerClasses {
		classRows = append(classRows, designdb.Row{Guid: int64(100000 + i), Name: name})
		switch name {
		case "Bard":
			kits = append(kits, kitRow(int64(200000+i), name,
				designdb.Pairs{equippedItem("Chest", tChest), equippedItem("Secondary", tFocus), equippedItem("RangedOffHand", tFocus), equippedItem("ResourceBag_0", tBag)},
				designdb.Struct{"Item": variant(tPotion), "Quantity": int64(5)}))
		default:
			kits = append(kits, kitRow(int64(200000+i), name, designdb.Pairs{equippedItem("Chest", tChest), equippedItem("Primary", tSword2H)}))
		}
	}
	src.rows["CharacterClassesRecordBase"] = classRows
	src.rows["StartingEquipmentRecordBase"] = kits
	return src
}

func buildItemsSynthetic(t *testing.T, src *source, m *usmap.Map) (*content.DB, map[string]string, error) {
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
	if err := buildItems(w, src, m, &Options{Log: io.Discard}, meta); err != nil {
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

// C_Y_B / C-O-A

func TestSyntheticItems(t *testing.T) {
	db, meta, err := buildItemsSynthetic(t, itemSource(), itemEnums())
	if err != nil {
		t.Fatal(err)
	}
	if meta["typed.item"] != "9" || meta["typed.starting_equipment"] != "19" { // the Bard's 4 worn and 1 carried, two worn for each of the other seven
		t.Errorf("meta = %v", meta)
	}

	// equip_slot: the names with a value of their own, not the pseudo names, cCount or _MAX
	names, err := db.EquipSlotNames()
	if err != nil || len(names) != 21 || names[0] != "Head" || names[2] != "Chest" || names[15] != "Primary" || names[23] != "ResourceBag_0" {
		t.Errorf("EquipSlotNames = %v, %v", names, err)
	}
	for _, v := range []int{83, 84} {
		if _, ok := names[v]; ok {
			t.Errorf("slot %d is in equip_slot", v)
		}
	}

	// the items, their slots in the record's order, and a fresh item's durability
	type want struct {
		slots      []int
		all        bool
		canEquip   bool
		durability float64
		quality    int
	}
	for guid, w := range map[int64]want{
		tChest:   {[]int{2}, false, false, 11, 1},     // CanEquip false, yet worn (every kit's starter armor)
		tScepter: {[]int{15}, false, true, 15, 3},     // Common band at quality 3
		tFocus:   {[]int{16, 18}, false, true, 31, 7}, // Uncommon band: 30 + 10 / 9 = 31.1 rounds to 31
		tSword2H: {[]int{15, 16}, true, true, 38, 1},  // 37.5 rounds half up
		tBag:     {[]int{23}, false, true, 0, 1},      // no stat block
		tPotion:  {nil, false, false, 0, 1},           // not worn
		tNoStat:  {[]int{19}, false, true, 0, 1},      // a stat block without MaxDurability
		tHalf:    {[]int{0}, false, true, 38, 1},      // 37.5
		tNoCurve: {[]int{9}, false, true, 10, 4},      // no quality curve: CommonMin (Belt is 9)
	} {
		it, err := db.Item(guid)
		if err != nil {
			t.Errorf("item %d: %v", guid, err)
			continue
		}
		if !equalInts(it.EquipSlots, w.slots) || it.OccupiesAllEquipSlots != w.all || it.CanEquip != w.canEquip ||
			it.MaxDurability != w.durability || it.DefaultQuality != w.quality {
			t.Errorf("%s = %+v, want slots %v all %v canEquip %v durability %v quality %d", it.Name, it, w.slots, w.all, w.canEquip, w.durability, w.quality)
		}
	}
	if _, err := db.Item(42); err == nil {
		t.Error("an item that is none was found")
	}

	// the kits
	bard, err := db.StartingKit("Bard")
	if err != nil || len(bard) != 5 {
		t.Fatalf("the Bard's kit = %+v, %v", bard, err)
	}
	for i, wantItem := range []content.KitItem{
		{Container: content.KitEquipment, Ord: 0, Slot: 2, ItemGuid: tChest, Quantity: 1},
		{Container: content.KitEquipment, Ord: 1, Slot: 16, ItemGuid: tFocus, Quantity: 1},
		{Container: content.KitEquipment, Ord: 2, Slot: 18, ItemGuid: tFocus, Quantity: 1}, // the same item for two slots
		{Container: content.KitEquipment, Ord: 3, Slot: 23, ItemGuid: tBag, Quantity: 1},
		{Container: content.KitBackpack, Ord: 0, Slot: 0, ItemGuid: tPotion, Quantity: 5},
	} {
		if bard[i] != wantItem {
			t.Errorf("Bard kit item %d = %+v, want %+v", i, bard[i], wantItem)
		}
	}
	if tank, err := db.StartingKit("Tank"); err != nil || len(tank) != 2 || tank[1].Slot != 15 {
		t.Errorf("the Tank's kit = %+v, %v", tank, err)
	}
	if _, err := db.StartingKit("Paladin"); err == nil {
		t.Error("a class that has no kit has one")
	}
	if n := scalarInt(t, db, `SELECT count(DISTINCT class_guid) FROM starting_equipment`); n != 8 {
		t.Errorf("%d classes have a kit", n)
	}
}

func equalInts(a, b []int) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

// ~ C-Y-B - C | O | A ~

func TestSyntheticItemsRefusals(t *testing.T) {
	type change func(*source)
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
		change change
		enums  *usmap.Map
		want   string
	}{
		"a table of the wrong TypeId": {func(s *source) { s.typeIDs["ItemRecordBase"] = 1 }, nil, "ItemRecordBase has TypeId 1"},
		"a missing table":             {func(s *source) { delete(s.rows, "StartingEquipmentRecordBase") }, nil, "no decoded StartingEquipmentRecordBase"},
		"no EEquipSlot":               {nil, usmap.New(nil, nil), "no enum EEquipSlot"},
		"an EEquipSlot that is not the client's": {nil, usmap.New([]*usmap.Enum{{Name: "EEquipSlot", Values: []usmap.EnumValue{{Name: "Chest", Value: 5}}}}, nil),
			"is not the enum the client has"},
		"MaxDurability under another name": {func(s *source) { s.rows["StatTypeDefRecordBase"][0].Name = "Durability" }, nil, "content names it MaxDurability"},
		"an unknown slot": {func(s *source) {
			rowOf(s, "ItemRecordBase", tChest).Fields["EquipSlots"] = []designdb.Value{"EEquipSlot::Wings"}
		}, nil, `"EEquipSlot::Wings", which is not an EEquipSlot`},
		"a slot that is not a string": {func(s *source) {
			rowOf(s, "ItemRecordBase", tChest).Fields["EquipSlots"] = []designdb.Value{int64(2)}
		}, nil, "EquipSlots[0] is int64"},
		"an item row without a field": {func(s *source) { delete(rowOf(s, "ItemRecordBase", tChest).Fields, "CanEquip") }, nil, "no field CanEquip"},
		"two items with a guid": {func(s *source) {
			s.rows["ItemRecordBase"] = append(s.rows["ItemRecordBase"], itemRow(tChest, "Again", true, false, 1, 0, 0))
		}, nil, "two ItemRecord rows have Guid"},
		"a kit item that is none": {func(s *source) {
			rowOf(s, "StartingEquipmentRecordBase", 200001).Fields["EquippedItems"] = designdb.Pairs{equippedItem("Chest", 424242)}
		}, nil, "item 424242 is not an ItemRecord"},
		"a kit item where it cannot go": {func(s *source) {
			rowOf(s, "StartingEquipmentRecordBase", 200001).Fields["EquippedItems"] = designdb.Pairs{equippedItem("Legs", tChest)}
		}, nil, "is not worn in EEquipSlot::Legs"},
		"a kit slot that is none": {func(s *source) {
			rowOf(s, "StartingEquipmentRecordBase", 200001).Fields["EquippedItems"] = designdb.Pairs{equippedItem("Tail", tChest)}
		}, nil, `"EEquipSlot::Tail" is not an EEquipSlot`},
		"a class with no kit": {func(s *source) { s.rows["StartingEquipmentRecordBase"] = s.rows["StartingEquipmentRecordBase"][1:] }, nil, "no StartingEquipment row for class Bard"},
		"two kits for a class": {func(s *source) {
			s.rows["StartingEquipmentRecordBase"] = append(s.rows["StartingEquipmentRecordBase"], kitRow(999, "Tank", nil))
		}, nil, "two StartingEquipment rows are for class Tank"},
		"a class with no CharacterClasses row": {func(s *source) { s.rows["CharacterClassesRecordBase"] = s.rows["CharacterClassesRecordBase"][1:] }, nil, "no CharacterClasses row named Bard"},
		"a stored item with no quantity": {func(s *source) {
			rowOf(s, "StartingEquipmentRecordBase", 200001).Fields["StoredItems"] = []designdb.Value{designdb.Struct{"Item": variant(tPotion), "Quantity": int64(0)}}
		}, nil, "quantity 0"},
		"a stored item that is none": {func(s *source) {
			rowOf(s, "StartingEquipmentRecordBase", 200001).Fields["StoredItems"] = []designdb.Value{designdb.Struct{"Item": variant(5), "Quantity": int64(1)}}
		}, nil, "item 5 is not an ItemRecord"},
	} {
		src := itemSource()
		if tc.change != nil {
			tc.change(src)
		}
		enums := tc.enums
		if enums == nil {
			enums = itemEnums()
		}
		if _, _, err := buildItemsSynthetic(t, src, enums); err == nil || !strings.Contains(err.Error(), tc.want) {
			t.Errorf("%s: buildItems = %v, want an error containing %q", name, err, tc.want)
		}
	}
}

func TestDurabilityAtQuality(t *testing.T) {
	bands := []qualityBand{{1, 5}, {6, 15}, {16, 30}, {31, 50}, {51, 70}, {71, 100}, {101, 200}}
	d := durability{min: [7]float64{10, 30, 50, 70, 90, 110, 130}, max: [7]float64{20, 40, 60, 80, 100, 120, 140}}
	for q, want := range map[int]float64{
		1: 10, 3: 15, 5: 20, // Common
		6: 30, 15: 40, 7: 30 + 10.0/9, // Uncommon
		16: 50, 30: 60, 31: 70, 50: 80, 51: 90, 70: 100, 71: 110, 100: 120, 101: 130, 200: 140,
		0:   10,  // below every band: the first, at its start
		250: 140, // past them all: the last, at its end
	} {
		if got := d.at(bands, q); got != want {
			t.Errorf("at quality %d = %v, want %v", q, got, want)
		}
	}
	if got := d.at(nil, 50); got != 10 {
		t.Errorf("with no quality curve = %v, want CommonMin", got)
	}
}

// ---- the real file ---------------------------------------------------------------------------------------------------------
// (C | O | A : Cyb)

func TestRealItems(t *testing.T) {
	db := builtDB(t)
	meta, err := db.Meta()
	if err != nil {
		t.Fatal(err)
	}
	if meta["typed.item"] != "10568" || meta["typed.starting_equipment"] != "96" {
		t.Errorf("meta typed.item = %q, typed.starting_equipment = %q", meta["typed.item"], meta["typed.starting_equipment"])
	}
	if n := scalarInt(t, db, `SELECT count(*) FROM item`); n != 10568 {
		t.Errorf("item has %d rows, want the 10,568 ItemRecords", n)
	}
	if n := scalarInt(t, db, `SELECT count(*) FROM equip_slot`); n != 83 {
		t.Errorf("equip_slot has %d rows, want the 83 slots below cCount", n)
	}
	names, err := db.EquipSlotNames()
	if err != nil || names[1] != "Helmet" || names[2] != "Chest" || names[10] != "RingRight" || names[15] != "Primary" || names[23] != "ResourceBag_0" ||
		names[26] != "Pickaxe" || names[71] != "Costume" || names[79] != "CloakCosmetic" || names[82] != "None" {
		t.Errorf("EquipSlotNames = %v, %v", names, err)
	}

	// The captured original server's Bard wore its class's record: 13 items, each with the durability the capture shows.
	bard, err := db.StartingKit("Bard")
	if err != nil || len(bard) != 13 {
		t.Fatalf("the Bard's kit = %d items, %v", len(bard), err)
	}
	want := map[int]struct {
		record     int64
		durability float64
	}{
		1: {6064630736269279236, 0}, 2: {6064630214426886486, 11}, 5: {6064630214427083132, 11}, 6: {6064630214427279778, 11},
		15: {6064631131688402948, 11}, 16: {6064630695486029824, 11}, 17: {6064631670974054529, 12}, 23: {6064629382556287782, 0},
		26: {6064634744584208707, 38}, 29: {6064634744581914946, 38}, 33: {6064634744578441536, 38}, 35: {6064634744576540991, 38},
		38: {6064634744579883329, 38},
	}
	for _, k := range bard {
		w, ok := want[k.Slot]
		it, err := db.Item(k.ItemGuid)
		if err != nil {
			t.Fatal(err)
		}
		if !ok || k.Container != content.KitEquipment || k.ItemGuid != w.record || it.MaxDurability != w.durability || it.DefaultQuality != 1 {
			t.Errorf("Bard kit slot %d: %+v (%s, durability %v), want record %d durability %v", k.Slot, k, it.Name, it.MaxDurability, w.record, w.durability)
		}
	}
	// the equip facts the server depends on
	chest, _ := db.Item(6064630214426886486)
	if chest.CanEquip || !equalInts(chest.EquipSlots, []int{2}) {
		t.Errorf("the starter chest: %+v (CanEquip is false in the design data and the kit wears it: the server must not gate on it)", chest)
	}
	book, _ := db.Item(6064631670974054529)
	if !book.OccupiesAllEquipSlots || !equalInts(book.EquipSlots, []int{17, 18}) {
		t.Errorf("the Book: %+v, want a two-hander for Ranged and RangedOffHand", book)
	}
	focus, _ := db.Item(6064630695486029824)
	if focus.OccupiesAllEquipSlots || !equalInts(focus.EquipSlots, []int{16, 18}) {
		t.Errorf("the Focus: %+v", focus)
	}
	// every class has a kit of the size the survey found, in slots its items allow
	sizes := map[string]int{"Ranger": 11, "Bard": 13, "Cleric": 12, "Fighter": 11, "Rogue": 11, "Summoner": 13, "Tank": 12, "Mage": 13}
	for class, n := range sizes {
		kit, err := db.StartingKit(class)
		if err != nil || len(kit) != n {
			t.Errorf("%s: %d items, %v; want %d", class, len(kit), err, n)
		}
		for _, k := range kit {
			it, err := db.Item(k.ItemGuid)
			if err != nil {
				t.Fatal(err)
			}
			fits := false
			for _, s := range it.EquipSlots {
				fits = fits || s == k.Slot
			}
			if !fits {
				t.Errorf("%s: %s in slot %d, its slots %v", class, it.Name, k.Slot, it.EquipSlots)
			}
		}
	}
	// the 8 StoredItems are empty in every record: no backpack kit
	if n := scalarInt(t, db, `SELECT count(*) FROM starting_equipment WHERE container = 'Backpack'`); n != 0 {
		t.Errorf("%d backpack kit items", n)
	}
	// gear that is worn but has CanEquip false: the design data has 374 (the starter armor among them)
	if n := scalarInt(t, db, `SELECT count(*) FROM item WHERE can_equip = 0 AND EXISTS (SELECT 1 FROM item_equip_slot s WHERE s.item_guid = item.guid)`); n != 374 {
		t.Errorf("%d items have a slot and CanEquip false, the survey found 374", n)
	}
	// a ring is worn in either hand: RingRight 10 and RingLeft 11, in the order of its record
	if n := scalarInt(t, db, `SELECT count(*) FROM (SELECT item_guid FROM item_equip_slot GROUP BY item_guid HAVING group_concat(slot) IN ('10,11', '11,10'))`); n < 50 {
		t.Errorf("%d ring items", n)
	}
}
