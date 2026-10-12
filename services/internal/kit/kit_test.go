// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package kit

import (
	"bytes"
	"database/sql"
	"fmt"
	"os"
	"path/filepath"
	"reflect"
	"sort"
	"strings"
	"testing"
	"time"

	"aocrop/services/internal/character"
	"aocrop/services/internal/chardef"
	"aocrop/services/internal/content"
)

// Items of the synthetic content.db.
const (
	chest   = 1001
	legs    = 1002
	sword2H = 1003
	focus   = 1004
	bag     = 1005
	potion  = 1006
)

// -- Cyb x C-O-A --

// build makes a content.db with the item and kit tables of the test and opens it. extra runs after the kits are in.
func build(t *testing.T, mutate func(tx *sql.Tx)) *content.DB {
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
	tx, err := sdb.Begin()
	if err != nil {
		t.Fatal(err)
	}
	must := func(query string, args ...any) {
		if _, err := tx.Exec(query, args...); err != nil {
			t.Fatalf("%s: %v", query, err)
		}
	}
	must(`INSERT INTO meta VALUES (?, ?), (?, ?)`, content.MetaSchemaVersion, "1", content.MetaRecordEncoding, content.EncodingJSON)
	for _, it := range []struct {
		guid       int64
		name       string
		occupies   int
		quality    int
		durability float64
		slots      []int
	}{
		{chest, "Chest", 0, 1, 11, []int{2}}, {legs, "Legs", 0, 1, 11, []int{5}}, {sword2H, "Sword2H", 1, 1, 12, []int{15, 16}},
		{focus, "Focus", 0, 1, 11.5, []int{16, 18}}, {bag, "Bag", 0, 1, 0, []int{23, 24, 25}}, {potion, "Potion", 0, 3, 0, nil},
	} {
		must(`INSERT INTO item (guid, name, level, can_equip, occupies_all_equip_slots, default_quality, max_stack_size, stat_block_guid,
			appearance_guid, max_durability, quality_curve_guid) VALUES (?, ?, 1, 1, ?, ?, 1, 0, 0, ?, 0)`, it.guid, it.name, it.occupies, it.quality, it.durability)
		for i, s := range it.slots {
			must(`INSERT INTO item_equip_slot (item_guid, ord, slot) VALUES (?, ?, ?)`, it.guid, i, s)
		}
	}
	for class := int32(1); class <= 8; class++ {
		name, _ := chardef.ClassName(class)
		guid := int64(100000 + class)
		add := func(container string, ord, slot int, item int64, qty int) {
			must(`INSERT INTO starting_equipment (class_guid, class_name, container, ord, slot, item_guid, variation_guid, quantity)
				VALUES (?, ?, ?, ?, ?, ?, 0, ?)`, guid, name, container, ord, slot, item, qty)
		}
		switch name {
		case "Bard": // the same Focus in two slots, and a stack in the bag
			add("Equipment", 0, 2, chest, 1)
			add("Equipment", 1, 16, focus, 1)
			add("Equipment", 2, 18, focus, 1)
			add("Equipment", 3, 23, bag, 1)
			add("Backpack", 0, 0, potion, 5)
		default:
			add("Equipment", 0, 2, chest, 1)
			add("Equipment", 1, 5, legs, 1)
			add("Equipment", 2, 15, sword2H, 1)
		}
	}
	if mutate != nil {
		mutate(tx)
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
	return db
}

func TestKitIsTheClassesRecordAsFreshInstances(t *testing.T) {
	m, err := Load(build(t, nil))
	if err != nil {
		t.Fatal(err)
	}
	m.Now = func() time.Time { return time.Unix(1_790_000_000, 987_654_321) }
	items, err := m.Kit(2)
	if err != nil {
		t.Fatal(err)
	}
	type row struct {
		container string
		slot      int32
		record    int64
		quantity  int32
		quality   int32
		durab     float32
	}
	var got []row
	for _, it := range items {
		got = append(got, row{it.Container, it.Slot, it.ItemRecordID, it.Quantity, it.Quality, it.Durability})
	}
	want := []row{
		{character.Equipment, 2, chest, 1, 1, 11},
		{character.Equipment, 16, focus, 1, 1, 12}, // 11.5 rounds half up
		{character.Equipment, 18, focus, 1, 1, 12},
		{character.Equipment, 23, bag, 1, 1, 0},
		{character.Backpack, 0, potion, 5, 3, 0}, // a stored item: in the bag from slot 0, its quantity, its default quality
	}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("the Bard's kit:\n got  %v\n want %v", got, want)
	}
	if err := character.ValidateItems(items); err != nil {
		t.Errorf("the kit is not a valid set of items: %v", err)
	}
	// the same item named for two slots is two instances; one creation time, a whole second, for the whole kit
	if items[1].InstanceGUID == items[2].InstanceGUID {
		t.Errorf("the two Focus instances share the guid %s", items[1].InstanceGUID)
	}
	for _, it := range items {
		if !it.CreatedAt.Equal(time.Unix(1_790_000_000, 0)) {
			t.Errorf("created at %v, want the whole second 1790000000", it.CreatedAt)
		}
	}
}

func TestEveryKitHasItsOwnGuidsAndNeverTheZeroGuid(t *testing.T) {
	m, err := Load(build(t, nil))
	if err != nil {
		t.Fatal(err)
	}
	seen := map[string]bool{}
	for i := 0; i < 50; i++ {
		for class := int32(1); class <= 8; class++ {
			items, err := m.Kit(class)
			if err != nil {
				t.Fatal(err)
			}
			for _, it := range items {
				if seen[it.InstanceGUID] {
					t.Fatalf("guid %s made twice", it.InstanceGUID)
				}
				seen[it.InstanceGUID] = true
			}
		}
	}
	// a source that gives the zero guid first is read again: the zero guid is never an instance
	m.Random = bytes.NewReader(append(make([]byte, 16), []byte("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")...))
	items, err := m.Kit(1)
	if err != nil || items[0].InstanceGUID == strings.Repeat("0", 32) {
		t.Errorf("the zero guid was used: %v, %v", items, err)
	}
	// a source that runs out is an error and no kit
	m.Random = bytes.NewReader(make([]byte, 5))
	if items, err := m.Kit(1); err == nil {
		t.Errorf("a kit from a source with no bytes: %v", items)
	}
}

func TestKitOfAnUnknownClassIsAnError(t *testing.T) {
	m, err := Load(build(t, nil))
	if err != nil {
		t.Fatal(err)
	}
	for _, class := range []int32{-1, 0, 9, 100} {
		if items, err := m.Kit(class); err == nil {
			t.Errorf("Kit(%d) = %v", class, items)
		}
	}
}

func TestLoadRefusesWhatItCannotMakeAKitOf(t *testing.T) {
	for name, tc := range map[string]struct {
		mutate func(tx *sql.Tx)
		want   string
	}{
		"a class with no kit": {func(tx *sql.Tx) {
			tx.Exec(`DELETE FROM starting_equipment WHERE class_name = 'Tank'`)
		}, "no starting kit for class \"Tank\""},
		"an item that is none": {func(tx *sql.Tx) {
			tx.Exec(`UPDATE starting_equipment SET item_guid = 424242 WHERE class_name = 'Mage' AND ord = 0`)
		}, "no item 424242"},
		"an item where its record says it cannot go": {func(tx *sql.Tx) {
			tx.Exec(`UPDATE starting_equipment SET slot = 3 WHERE class_name = 'Mage' AND ord = 0`)
		}, "slot 3, which its record does not allow"},
		"two items in a slot": {func(tx *sql.Tx) {
			tx.Exec(`UPDATE starting_equipment SET slot = 2, item_guid = 1001 WHERE class_name = 'Rogue' AND ord = 1`)
		}, "two items in Equipment slot 2"},
		"a quantity of 0": {func(tx *sql.Tx) {
			tx.Exec(`UPDATE starting_equipment SET quantity = 0 WHERE class_name = 'Bard' AND container = 'Backpack'`)
		}, "quantity 0"},
	} {
		if _, err := Load(build(t, tc.mutate)); err == nil || !strings.Contains(err.Error(), tc.want) {
			t.Errorf("%s: Load = %v, want an error containing %q", name, err, tc.want)
		}
	}
}

// (C Y B) (C | O | A)

// A content.db built before plan B2 (contentc version 2: no equip_slot, item, item_equip_slot or starting_equipment): Load refuses
// it, naming the missing table, so the character service refuses every creation and skips the backfill (cmd/character).
func TestLoadRefusesAContentDBBuiltBeforeB2(t *testing.T) {
	old := build(t, func(tx *sql.Tx) {
		for _, table := range []string{"starting_equipment", "item_equip_slot", "item", "equip_slot"} {
			if _, err := tx.Exec(`DROP TABLE ` + table); err != nil {
				t.Fatalf("drop %s: %v", table, err)
			}
		}
	})
	m, err := Load(old)
	if err == nil || m != nil || !strings.Contains(err.Error(), "no such table") {
		t.Errorf("Load of a content.db without the B2 tables = %v, %v; want an error that names the missing table", m, err)
	}
}

// ---- the real content.db ---------------------------------------------------------------------------------------------

func realDB(t *testing.T) *content.DB {
	t.Helper()
	path := os.Getenv("AOC_CONTENT_DB")
	if path == "" {
		t.Skip("AOC_CONTENT_DB not set (the content.db contentc builds)")
	}
	db, err := content.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { db.Close() })
	return db
}

// The captured original server's Bard (replay_data.bin, the Equipment payload at pkt#89): slot, item record, durability.
var capturedBard = []struct {
	slot       int32
	record     int64
	durability float32
}{
	{1, 6064630736269279236, 0},   // Gear_Armor_Light_Bard_Head
	{2, 6064630214426886486, 11},  // Gear_Armor_Light_Starter_Chest
	{5, 6064630214427083132, 11},  // ... _Legs
	{6, 6064630214427279778, 11},  // ... _Feet
	{15, 6064631131688402948, 11}, // Gear_Weapon_Scepter_1H_Starter
	{16, 6064630695486029824, 11}, // Gear_Weapon_Focus_OH_Starter
	{17, 6064631670974054529, 12}, // Gear_Weapon_Book_2H_Starter
	{23, 6064629382556287782, 0},  // Bag_Resource_Sanctus
	{26, 6064634744584208707, 38}, // the five artisan tools of the FTUE
	{29, 6064634744581914946, 38},
	{33, 6064634744578441536, 38},
	{35, 6064634744576540991, 38},
	{38, 6064634744579883329, 38},
}

func TestTheBardsKitIsTheCapturedBards(t *testing.T) {
	m, err := Load(realDB(t))
	if err != nil {
		t.Fatal(err)
	}
	items, err := m.Kit(2)
	if err != nil {
		t.Fatal(err)
	}
	character.SortItems(items)
	if len(items) != len(capturedBard) {
		t.Fatalf("%d items, the captured Bard has %d", len(items), len(capturedBard))
	}
	for i, want := range capturedBard {
		it := items[i]
		if it.Container != character.Equipment || it.Slot != want.slot || it.ItemRecordID != want.record || it.Durability != want.durability ||
			it.Quantity != 1 || it.Quality != 1 {
			t.Errorf("item %d = %+v, want Equipment slot %d record %d durability %v, quantity 1 quality 1", i, it, want.slot, want.record, want.durability)
		}
	}
}

func TestEveryClassHasAValidKit(t *testing.T) {
	m, err := Load(realDB(t))
	if err != nil {
		t.Fatal(err)
	}
	sizes := map[string]int{"Ranger": 11, "Bard": 13, "Cleric": 12, "Fighter": 11, "Rogue": 11, "Summoner": 13, "Tank": 12, "Mage": 13}
	var names []string
	for n := range sizes {
		names = append(names, n)
	}
	sort.Strings(names)
	for _, name := range names {
		class, _ := chardef.ClassByName(name)
		items, err := m.Kit(class)
		if err != nil {
			t.Errorf("%s: %v", name, err)
			continue
		}
		if len(items) != sizes[name] {
			t.Errorf("%s: %d items, want %d", name, len(items), sizes[name])
		}
		if err := character.ValidateItems(items); err != nil {
			t.Errorf("%s: %v", name, err)
		}
		for _, it := range items {
			if it.Container != character.Equipment {
				t.Errorf("%s: %+v is not worn (the records' StoredItems are empty)", name, it)
			}
		}
	}
	// a Mage wears two instances of the Focus (Secondary and RangedOffHand)
	mage, _ := m.Kit(8)
	var focus []string
	for _, it := range mage {
		if it.ItemRecordID == 6064630695486029824 {
			focus = append(focus, fmt.Sprintf("%d:%s", it.Slot, it.InstanceGUID[:4]))
		}
	}
	if len(focus) != 2 {
		t.Errorf("the Mage's Focus instances: %v, want two", focus)
	}
}
