// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"log/slog"
	"math"
	"reflect"
	"strings"
	"testing"
	"time"
)

var created = time.UnixMilli(1_790_000_000_000)

// item is a valid item; the tests change what they want.
func item(container string, slot int32, record int64, guid string) Item {
	return Item{Container: container, Slot: slot, ItemRecordID: record, InstanceGUID: guid, Quantity: 1, Quality: 1,
		Durability: 11, CreatedAt: created}
}

// guid is a distinct instance guid for n.
func guid(n int) string { return fmt.Sprintf("%032x", n) }

// a small kit: a chest and a weapon worn, a potion stack in the bag and a quest item.
func smallKit() []Item {
	return []Item{
		item(Backpack, 3, 7001, guid(1)),
		item(Equipment, 2, 6064630214426886486, guid(2)),
		item(Equipment, 15, 6064631131688402948, guid(3)),
		item(Quest, 99, 7002, guid(4)),
	}
}

// C.O.A : Cyb

func TestItemValidate(t *testing.T) {
	good := item(Backpack, 0, 1, guid(1))
	if err := good.Validate(); err != nil {
		t.Fatalf("a good item: %v", err)
	}
	for _, tc := range []struct {
		name string
		mod  func(*Item)
		want string
	}{
		{"no container", func(i *Item) { i.Container = "" }, "container"},
		{"another container", func(i *Item) { i.Container = "Bank" }, "container"},
		{"slot -1", func(i *Item) { i.Slot = -1 }, "slot -1"},
		{"Backpack slot 70", func(i *Item) { i.Slot = 70 }, "slot 70"},
		{"Equipment slot 83", func(i *Item) { i.Container, i.Slot = Equipment, 83 }, "slot 83"},
		{"Quest slot 100", func(i *Item) { i.Container, i.Slot = Quest, 100 }, "slot 100"},
		{"Resource0 slot 256", func(i *Item) { i.Container, i.Slot = Resource0, 256 }, "slot 256"},
		{"Resource2 slot -1", func(i *Item) { i.Container, i.Slot = Resource2, -1 }, "slot -1"},
		{"a fourth resource container", func(i *Item) { i.Container = "Resource3" }, "container"},
		{"record 0", func(i *Item) { i.ItemRecordID = 0 }, "record"},
		{"negative record", func(i *Item) { i.ItemRecordID = -5 }, "record"},
		{"guid in capitals", func(i *Item) { i.InstanceGUID = strings.ToUpper(guid(255)) }, "guid"},
		{"short guid", func(i *Item) { i.InstanceGUID = "abcd" }, "guid"},
		{"zero guid", func(i *Item) { i.InstanceGUID = strings.Repeat("0", 32) }, "guid"},
		{"quantity 0", func(i *Item) { i.Quantity = 0 }, "quantity"},
		{"quantity past the limit", func(i *Item) { i.Quantity = MaxQuantity + 1 }, "quantity"},
		{"quality 0", func(i *Item) { i.Quality = 0 }, "quality"},
		{"quality 201", func(i *Item) { i.Quality = MaxQuality + 1 }, "quality"},
		{"negative durability", func(i *Item) { i.Durability = -0.5 }, "durability"},
		{"NaN durability", func(i *Item) { i.Durability = float32(math.NaN()) }, "durability"},
		{"infinite durability", func(i *Item) { i.Durability = float32(math.Inf(1)) }, "durability"},
		{"no creation time", func(i *Item) { i.CreatedAt = time.Time{} }, "creation time"},
		{"negative persisted health", func(i *Item) { i.PersistedHealth = f32(-1) }, "persisted health"},
		{"NaN persisted health", func(i *Item) { i.PersistedHealth = f32(float32(math.NaN())) }, "persisted health"},
		{"persisted health past the limit", func(i *Item) { i.PersistedHealth = f32(1.5e9) }, "persisted health"},
		{"negative persisted mana", func(i *Item) { i.PersistedMana = f32(-0.5) }, "persisted mana"},
		{"infinite persisted mana", func(i *Item) { i.PersistedMana = f32(float32(math.Inf(1))) }, "persisted mana"},
		{"negative lock expiration", func(i *Item) { i.LockExpiration = -1 }, "lock expiration"},
		{"a lock past the year 9999", func(i *Item) { i.LockExpiration = 253402300800 }, "lock expiration"},
	} {
		i := good
		tc.mod(&i)
		if err := i.Validate(); !errors.Is(err, ErrInvalid) || !strings.Contains(err.Error(), tc.want) {
			t.Errorf("%s: %v, want ErrInvalid about %q", tc.name, err, tc.want)
		}
	}
	limits := Item{Container: Quest, Slot: 99, ItemRecordID: math.MaxInt64, InstanceGUID: strings.Repeat("f", 32), Quantity: MaxQuantity,
		Quality: MaxQuality, Durability: 1e9, CreatedAt: created}
	if err := limits.Validate(); err != nil {
		t.Errorf("an item at every limit: %v", err)
	}
	limits.PersistedHealth, limits.PersistedMana, limits.LockExpiration = f32(1e9), f32(0), 253402300799
	if err := limits.Validate(); err != nil {
		t.Errorf("a mount item at every limit: %v", err)
	}
}

func f32(v float32) *float32 { return &v }

// A mount item keeps its Health, Mana and lock time (migration 0007); every other item keeps none, and a save of a set that holds both gives both back.
func TestSaveInventoryKeepsAMountItemsPersistedState(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c, err := s.Create(ctx, New{AccountID: acc, Name: "rider", ClassID: 7, Items: smallKit()})
	if err != nil {
		t.Fatal(err)
	}
	// what retail's login shows on a level 20 owner's terror bird: Health 3572, Mana 1, a lock time in the past
	bird := item(Backpack, 5, 6064631121016127508, guid(20))
	bird.PersistedHealth, bird.PersistedMana, bird.LockExpiration = f32(3572), f32(1), 1770131735
	dead := item(Backpack, 6, 6064634325298708480, guid(21))
	dead.PersistedHealth, dead.PersistedMana, dead.LockExpiration = f32(0), f32(1), 1790003600 // a mount that died: saved at no Health, locked
	onlyLock := item(Backpack, 7, 6064634099174146054, guid(22))
	onlyLock.LockExpiration = 1790000123
	plain := item(Backpack, 8, 7001, guid(23))
	set := []Item{item(Backpack, 3, 7001, guid(1)), bird, dead, onlyLock, plain}
	if err := s.SaveInventory(ctx, c.ID, set); err != nil {
		t.Fatalf("SaveInventory: %v", err)
	}
	got, err := s.Inventory(ctx, c.ID)
	if err != nil || !reflect.DeepEqual(got, set) {
		t.Fatalf("after the save: %+v, %v; want %+v", got, err, set)
	}
	if got[1].PersistedHealth == nil || *got[1].PersistedHealth != 3572 || *got[1].PersistedMana != 1 || got[1].LockExpiration != 1770131735 ||
		got[2].PersistedHealth == nil || *got[2].PersistedHealth != 0 || got[3].PersistedHealth != nil || got[3].PersistedMana != nil || got[3].LockExpiration != 1790000123 ||
		got[4].PersistedHealth != nil || got[4].LockExpiration != 0 {
		t.Errorf("the persisted state read back: %+v", got)
	}
	// a later save replaces it: the mount came back with the Health it was saved with, then was hurt, and the lock ended
	bird.PersistedHealth, bird.LockExpiration = f32(1200.5), 0
	if err := s.SaveInventory(ctx, c.ID, []Item{bird}); err != nil {
		t.Fatal(err)
	}
	if got, _ := s.Inventory(ctx, c.ID); len(got) != 1 || *got[0].PersistedHealth != 1200.5 || *got[0].PersistedMana != 1 || got[0].LockExpiration != 0 {
		t.Errorf("after the second save: %+v", got)
	}
	// a state the table cannot hold is refused whole, with nothing changed
	bad := bird
	bad.PersistedHealth = f32(-3)
	if err := s.SaveInventory(ctx, c.ID, []Item{bad}); !errors.Is(err, ErrInvalid) {
		t.Errorf("a negative persisted health: %v, want ErrInvalid", err)
	}
	if got, _ := s.Inventory(ctx, c.ID); len(got) != 1 || *got[0].PersistedHealth != 1200.5 {
		t.Errorf("a refused save changed the items: %+v", got)
	}
	// a new character is made with a mount in its bag
	other, err := s.Create(ctx, New{AccountID: acc, Name: "rider2", ClassID: 7, Items: []Item{bird}})
	if err != nil {
		t.Fatalf("Create with a mount item: %v", err)
	}
	if got, _ := s.Inventory(ctx, other.ID); len(got) != 1 || *got[0].PersistedHealth != 1200.5 {
		t.Errorf("the created character's mount item: %+v", got)
	}
}

// C:Y:B + CoA

func TestValidateItemsRefusesDuplicatesAndTooMany(t *testing.T) {
	if err := ValidateItems(smallKit()); err != nil {
		t.Fatal(err)
	}
	slot := append(smallKit(), item(Equipment, 2, 9, guid(50)))
	if err := ValidateItems(slot); !errors.Is(err, ErrInvalid) || !strings.Contains(err.Error(), "two items in Equipment slot 2") {
		t.Errorf("two items in a slot: %v", err)
	}
	same := append(smallKit(), item(Backpack, 4, 9, guid(2)))
	if err := ValidateItems(same); !errors.Is(err, ErrInvalid) || !strings.Contains(err.Error(), "two items with instance guid") {
		t.Errorf("two items with a guid: %v", err)
	}
	// a Backpack slot and an Equipment slot of the same number are different slots
	if err := ValidateItems([]Item{item(Backpack, 5, 1, guid(1)), item(Equipment, 5, 1, guid(2)), item(Quest, 5, 1, guid(3))}); err != nil {
		t.Errorf("slot 5 of three storages: %v", err)
	}
	var all []Item
	n := 0
	for _, c := range []string{Backpack, Equipment, Quest, Resource0, Resource1, Resource2} { // Gathering stage 3: the resource containers count too
		for s := int32(0); s < SlotCount[c]; s++ {
			n++
			all = append(all, item(c, s, 1, guid(n)))
		}
	}
	if len(all) != MaxItems {
		t.Fatalf("%d slots, MaxItems %d", len(all), MaxItems)
	}
	if err := ValidateItems(all); err != nil {
		t.Errorf("every slot filled: %v", err)
	}
	if err := ValidateItems(append(all, item(Backpack, 0, 1, guid(999)))); !errors.Is(err, ErrInvalid) {
		t.Errorf("one item more than every slot: %v", err)
	}
}

func TestCreateStoresItemsWithTheCharacterAndReadsThemBackInOrder(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	kit := smallKit()
	c, err := s.Create(ctx, New{AccountID: acc, Name: "cyb", ClassID: 2, ClassName: "Bard", ArchetypeID: 17747, Items: kit})
	if err != nil {
		t.Fatalf("Create: %v", err)
	}
	got, err := s.Inventory(ctx, c.ID)
	if err != nil {
		t.Fatal(err)
	}
	want := append([]Item{}, kit...)
	SortItems(want)
	if !reflect.DeepEqual(got, want) {
		t.Errorf("Inventory =\n%+v\nwant\n%+v", got, want)
	}
	if got[0].Container != Backpack || got[1].Container != Equipment || got[1].Slot != 2 || got[2].Slot != 15 || got[3].Container != Quest {
		t.Errorf("not in order of storage and slot: %+v", got)
	}
	// every column comes back as it went in
	if it := got[1]; it.ItemRecordID != 6064630214426886486 || it.InstanceGUID != guid(2) || it.Quantity != 1 || it.Quality != 1 ||
		it.Durability != 11 || !it.CreatedAt.Equal(created) {
		t.Errorf("the chest: %+v", it)
	}
}

func TestCreateWithoutItemsHasNone(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c, err := s.Create(ctx, New{AccountID: acc, Name: "Bare", ClassID: 2})
	if err != nil {
		t.Fatal(err)
	}
	got, err := s.Inventory(ctx, c.ID)
	if err != nil || len(got) != 0 {
		t.Errorf("Inventory = %+v, %v; want none and no error", got, err)
	}
}

func TestACharacterThatCannotBeCreatedHasNoItemsAndTheOtherKeepsItsOwn(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	first, err := s.Create(ctx, New{AccountID: acc, Name: "First", ClassID: 2, Items: smallKit()})
	if err != nil {
		t.Fatal(err)
	}
	// the name is taken: the character is refused and its items are never written (the guids are the first's)
	if _, err := s.Create(ctx, New{AccountID: acc, Name: "first", ClassID: 2, Items: smallKit()}); !errors.Is(err, ErrNameTaken) {
		t.Errorf("name taken: %v", err)
	}
	// an item that breaks a rule: nothing at all is stored, not even the character
	bad := append(smallKit(), item(Backpack, 3, 1, guid(77)))
	if _, err := s.Create(ctx, New{AccountID: acc, Name: "Second", ClassID: 2, Items: bad}); !errors.Is(err, ErrInvalid) {
		t.Errorf("two items in a slot: %v", err)
	}
	if list, err := s.List(ctx, acc); err != nil || len(list) != 1 || list[0].ID != first.ID {
		t.Errorf("characters = %+v, %v; want only the first", list, err)
	}
	var n int
	if err := s.pool.QueryRow(ctx, `SELECT count(*) FROM character_items`).Scan(&n); err != nil || n != len(smallKit()) {
		t.Errorf("%d items in the table, want the first character's %d (%v)", n, len(smallKit()), err)
	}
	// a database refusal of an item the checks let through (the same guid under another character)
	other := New{AccountID: acc, Name: "Third", ClassID: 2, Items: []Item{item(Backpack, 0, 1, guid(1))}}
	if _, err := s.Create(ctx, other); err != nil {
		t.Errorf("the same guid under another character is allowed: %v", err)
	}
}

func TestInventoryOfUnknownAndMalformedIDsIsNotFound(t *testing.T) {
	s, _ := setup(t)
	ctx := context.Background()
	for _, id := range []string{"ffffffffffffffffffffffffffffffff", "", "not-an-id", "BBEC5C86EB101FE5F38C87F0FA22E7D0", "ab\x00cd"} {
		if _, err := s.Inventory(ctx, id); !errors.Is(err, ErrNotFound) {
			t.Errorf("Inventory(%q) = %v, want ErrNotFound", id, err)
		}
		if err := s.SaveInventory(ctx, id, smallKit()); !errors.Is(err, ErrNotFound) {
			t.Errorf("SaveInventory(%q) = %v, want ErrNotFound", id, err)
		}
	}
}

// -- C | Y | B . C.O.A --

func TestSaveInventoryReplacesAllItemsAndTheLatestWins(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c, err := s.Create(ctx, New{AccountID: acc, Name: "cyb", ClassID: 2, Items: smallKit()})
	if err != nil {
		t.Fatal(err)
	}
	// the chest moved from the Equipment into the bag, the weapon stays, the potions are gone, new durability
	moved := []Item{item(Backpack, 0, 6064630214426886486, guid(2)), item(Equipment, 15, 6064631131688402948, guid(3))}
	moved[0].Durability = 37.5
	moved[1].Quantity = 3
	moved[1].Quality = 51
	if err := s.SaveInventory(ctx, c.ID, moved); err != nil {
		t.Fatalf("SaveInventory: %v", err)
	}
	got, err := s.Inventory(ctx, c.ID)
	if err != nil || !reflect.DeepEqual(got, moved) {
		t.Errorf("after the save: %+v, %v; want %+v", got, err, moved)
	}
	// the latest save wins, whatever it was based on
	latest := []Item{item(Equipment, 2, 6064630214426886486, guid(2))}
	if err := s.SaveInventory(ctx, c.ID, latest); err != nil {
		t.Fatal(err)
	}
	if got, _ := s.Inventory(ctx, c.ID); !reflect.DeepEqual(got, latest) {
		t.Errorf("after the second save: %+v", got)
	}
	// a save keeps the character's other columns
	if after, err := s.Get(ctx, c.ID); err != nil || after.Name != "cyb" || after.ClassID != 2 {
		t.Errorf("the character after saving its items: %+v, %v", after, err)
	}
	// sub-millisecond time and a fractional float survive as the wire carries them
	odd := item(Backpack, 1, 5, guid(9))
	odd.Durability = 0.1
	odd.CreatedAt = time.UnixMilli(1_790_000_000_123)
	if err := s.SaveInventory(ctx, c.ID, []Item{odd}); err != nil {
		t.Fatal(err)
	}
	if got, _ := s.Inventory(ctx, c.ID); len(got) != 1 || got[0].Durability != 0.1 || got[0].CreatedAt.UnixMilli() != 1_790_000_000_123 {
		t.Errorf("durability and time: %+v", got)
	}
}

func TestSaveInventoryRefusalsChangeNothing(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c, err := s.Create(ctx, New{AccountID: acc, Name: "cyb", ClassID: 2, Items: smallKit()})
	if err != nil {
		t.Fatal(err)
	}
	want := func() []Item { got, _ := s.Inventory(ctx, c.ID); return got }()

	dupSlot := []Item{item(Backpack, 1, 1, guid(1)), item(Backpack, 1, 1, guid(2))}
	dupGuid := []Item{item(Backpack, 1, 1, guid(1)), item(Backpack, 2, 1, guid(1))}
	badContainer := []Item{item("Bank", 1, 1, guid(1))}
	badSlot := []Item{item(Backpack, 70, 1, guid(1))}
	badQuantity := item(Backpack, 1, 1, guid(1))
	badQuantity.Quantity = 0
	nan := item(Backpack, 1, 1, guid(1))
	nan.Durability = float32(math.NaN())
	noTime := item(Backpack, 1, 1, guid(1))
	noTime.CreatedAt = time.Time{}
	tooMany := make([]Item, MaxItems+1)
	for i := range tooMany {
		tooMany[i] = item(Backpack, int32(i%70), 1, guid(i+1))
	}
	for name, items := range map[string][]Item{
		"no items at all": nil, "an empty list": {}, "two items in a slot": dupSlot, "two items with a guid": dupGuid,
		"a container that is none": badContainer, "a slot out of range": badSlot, "quantity 0": {badQuantity},
		"a NaN durability": {nan}, "no creation time": {noTime}, "more items than slots": tooMany,
	} {
		if err := s.SaveInventory(ctx, c.ID, items); !errors.Is(err, ErrInvalid) {
			t.Errorf("%s: %v, want ErrInvalid", name, err)
		}
		if got, _ := s.Inventory(ctx, c.ID); !reflect.DeepEqual(got, want) {
			t.Fatalf("%s changed the items: %+v", name, got)
		}
	}
}

func TestOneCharactersItemsAreNeverAnothers(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	a, _ := s.Create(ctx, New{AccountID: acc, Name: "Alpha", ClassID: 2, Items: smallKit()})
	b, err := s.Create(ctx, New{AccountID: acc, Name: "Bravo", ClassID: 7, Items: []Item{item(Equipment, 2, 99, guid(1))}}) // the same guid
	if err != nil {
		t.Fatal(err)
	}
	if err := s.SaveInventory(ctx, b.ID, []Item{item(Equipment, 2, 100, guid(1))}); err != nil {
		t.Fatal(err)
	}
	got, _ := s.Inventory(ctx, a.ID)
	if len(got) != len(smallKit()) || got[1].ItemRecordID != 6064630214426886486 {
		t.Errorf("Alpha's items after Bravo's save: %+v", got)
	}
	if got, _ := s.Inventory(ctx, b.ID); len(got) != 1 || got[0].ItemRecordID != 100 {
		t.Errorf("Bravo's items: %+v", got)
	}
}

// built by CoA with C_Y_B

func TestDeletingACharacterDeletesItsItems(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	a, _ := s.Create(ctx, New{AccountID: acc, Name: "Alpha", ClassID: 2, Items: smallKit()})
	b, _ := s.Create(ctx, New{AccountID: acc, Name: "Bravo", ClassID: 2, Items: []Item{item(Backpack, 0, 1, guid(9))}})
	if err := s.Delete(ctx, acc, a.ID); err != nil {
		t.Fatal(err)
	}
	var n int
	if err := s.pool.QueryRow(ctx, `SELECT count(*) FROM character_items WHERE character_id = $1`, a.ID).Scan(&n); err != nil || n != 0 {
		t.Errorf("%d items of the deleted character remain (%v)", n, err)
	}
	if got, _ := s.Inventory(ctx, b.ID); len(got) != 1 {
		t.Errorf("the other character's items: %+v", got)
	}
}

// ---- Gathering stage 3, "Resource storage": the resource containers --------------------------------------------------------------------

// A kit of the Backpack, the Equipment and the Quest storage and two resource containers: stacks of raw items and a carcass.
func withResources() []Item {
	oak := item(Resource0, 0, 6064630000000000001, guid(20))
	oak.Quantity = 17
	oak.Quality = 6
	carcass := item(Resource0, 7, 6064630000000000002, guid(21))
	carcass.Durability = 0
	ore := item(Resource1, 39, 6064630000000000003, guid(22))
	ore.Quantity = 99
	return append(smallKit(), oak, carcass, ore)
}

func TestResourceItemsAreStoredAndReadBackInTheOrderOfStorage(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	kit := withResources()
	c, err := s.Create(ctx, New{AccountID: acc, Name: "cyb", ClassID: 2, Items: kit})
	if err != nil {
		t.Fatalf("Create: %v", err)
	}
	got, err := s.Inventory(ctx, c.ID)
	if err != nil {
		t.Fatal(err)
	}
	want := append([]Item{}, kit...)
	SortItems(want)
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("Inventory =\n%+v\nwant\n%+v", got, want)
	}
	order := []string{Backpack, Equipment, Equipment, Quest, Resource0, Resource0, Resource1}
	for i, o := range order {
		if got[i].Container != o {
			t.Errorf("item %d is in %s, want %s: %+v", i, got[i].Container, o, got)
		}
	}
	if got[4].Slot != 0 || got[5].Slot != 7 || got[6].Slot != 39 || got[4].Quantity != 17 || got[4].Quality != 6 {
		t.Errorf("the resource items: %+v", got[4:])
	}
	// the Backpack and the Equipment tables hold none of them: a resource item lives only in its own table
	var inItems, inResource int
	if err := s.pool.QueryRow(ctx, `SELECT count(*) FROM character_items WHERE character_id = $1`, c.ID).Scan(&inItems); err != nil {
		t.Fatal(err)
	}
	if err := s.pool.QueryRow(ctx, `SELECT count(*) FROM character_resource_items WHERE character_id = $1`, c.ID).Scan(&inResource); err != nil {
		t.Fatal(err)
	}
	if inItems != 4 || inResource != 3 {
		t.Errorf("%d rows in character_items and %d in character_resource_items, want 4 and 3", inItems, inResource)
	}
}

func TestSaveInventoryReplacesBothTables(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c, err := s.Create(ctx, New{AccountID: acc, Name: "cyb", ClassID: 2, Items: withResources()})
	if err != nil {
		t.Fatal(err)
	}
	// the oak is topped up and moved, the carcass is gone, the ore moves to the third container, a new reagent comes; the other items stay
	oak := item(Resource0, 12, 6064630000000000001, guid(20))
	oak.Quantity = 20
	oak.Quality = 6
	ore := item(Resource2, 3, 6064630000000000003, guid(22))
	ore.Quantity = 99
	reagent := item(Resource0, 0, 6064630000000000009, guid(23))
	next := append(smallKit(), oak, ore, reagent)
	if err := s.SaveInventory(ctx, c.ID, next); err != nil {
		t.Fatalf("SaveInventory: %v", err)
	}
	got, err := s.Inventory(ctx, c.ID)
	want := append([]Item{}, next...)
	SortItems(want)
	if err != nil || !reflect.DeepEqual(got, want) {
		t.Errorf("after the save:\n%+v, %v\nwant\n%+v", got, err, want)
	}
	// a save with no resource item empties the resource table (the latest save wins in both)
	if err := s.SaveInventory(ctx, c.ID, smallKit()); err != nil {
		t.Fatal(err)
	}
	var n int
	if err := s.pool.QueryRow(ctx, `SELECT count(*) FROM character_resource_items WHERE character_id = $1`, c.ID).Scan(&n); err != nil || n != 0 {
		t.Errorf("%d resource items remain after a save with none (%v)", n, err)
	}
	// one character's resource items are never another's
	other, _ := s.Create(ctx, New{AccountID: acc, Name: "Bravo", ClassID: 2, Items: []Item{item(Backpack, 0, 1, guid(30)), item(Resource0, 0, 5, guid(31))}})
	if err := s.SaveInventory(ctx, c.ID, withResources()); err != nil {
		t.Fatal(err)
	}
	if got, _ := s.Inventory(ctx, other.ID); len(got) != 2 || got[1].Container != Resource0 || got[1].ItemRecordID != 5 {
		t.Errorf("Bravo's items after Cyb's save: %+v", got)
	}
}

// C Y B + C O A

func TestSaveInventoryRefusalsForResourceItemsChangeNothing(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c, err := s.Create(ctx, New{AccountID: acc, Name: "cyb", ClassID: 2, Items: withResources()})
	if err != nil {
		t.Fatal(err)
	}
	want, _ := s.Inventory(ctx, c.ID)
	for name, items := range map[string][]Item{
		// the instance guid is unique per character across BOTH tables (checked here: a table cannot hold it)
		"a guid in the Backpack and in a resource container": {item(Backpack, 0, 1, guid(1)), item(Resource0, 0, 5, guid(1))},
		"a guid in two resource containers":                  {item(Resource0, 0, 5, guid(1)), item(Resource1, 0, 5, guid(1))},
		"two items in a resource slot":                       {item(Resource0, 4, 5, guid(1)), item(Resource0, 4, 5, guid(2))},
		"a resource slot out of range":                       {item(Resource1, 256, 5, guid(1))},
		"a fourth resource container":                        {item("Resource3", 0, 5, guid(1))},
	} {
		if err := s.SaveInventory(ctx, c.ID, items); !errors.Is(err, ErrInvalid) {
			t.Errorf("%s: %v, want ErrInvalid", name, err)
		}
		if got, _ := s.Inventory(ctx, c.ID); !reflect.DeepEqual(got, want) {
			t.Fatalf("%s changed the items: %+v", name, got)
		}
	}
}

func TestDeletingACharacterDeletesItsResourceItems(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	a, _ := s.Create(ctx, New{AccountID: acc, Name: "Alpha", ClassID: 2, Items: withResources()})
	if err := s.Delete(ctx, acc, a.ID); err != nil {
		t.Fatal(err)
	}
	var n int
	if err := s.pool.QueryRow(ctx, `SELECT count(*) FROM character_resource_items WHERE character_id = $1`, a.ID).Scan(&n); err != nil || n != 0 {
		t.Errorf("%d resource items of the deleted character remain (%v)", n, err)
	}
}

// A character whose only items are in a resource container is not a character without items: the starter kit backfill leaves it alone.
func TestGrantMissingKitsLeavesACharacterWithResourceItemsAlone(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c, err := s.Create(ctx, New{AccountID: acc, Name: "Hoarder", ClassID: 2, Items: []Item{item(Resource0, 0, 6064630000000000001, guid(40))}})
	if err != nil {
		t.Fatal(err)
	}
	captureLog(t)
	res, err := s.GrantMissingKits(ctx, &fakeKits{})
	if err != nil || res.Granted != 0 {
		t.Fatalf("GrantMissingKits = %+v, %v; a character with a resource item has items", res, err)
	}
	if got, _ := s.Inventory(ctx, c.ID); len(got) != 1 || got[0].Container != Resource0 {
		t.Errorf("the character's items after the backfill: %+v", got)
	}
}

// ---- the backfill ----------------------------------------------------------------------------------------------------

// fakeKits makes a kit of two items for classes 2 and 7 and refuses the rest; onKit runs before a kit is returned.
type fakeKits struct {
	calls []int32
	onKit func(class int32)
	n     int
}

func (f *fakeKits) Kit(class int32) ([]Item, error) {
	f.calls = append(f.calls, class)
	if f.onKit != nil {
		f.onKit(class)
	}
	if class != 2 && class != 7 {
		return nil, fmt.Errorf("class %d has no starting kit", class)
	}
	f.n += 10
	return []Item{item(Equipment, 2, int64(1000+class), guid(f.n+1)), item(Equipment, 15, int64(2000+class), guid(f.n+2))}, nil
}

func captureLog(t *testing.T) *bytes.Buffer {
	t.Helper()
	var logs bytes.Buffer
	saved := slog.Default()
	slog.SetDefault(slog.New(slog.NewTextHandler(&logs, nil)))
	t.Cleanup(func() { slog.SetDefault(saved) })
	return &logs
}

func TestGrantMissingKitsGivesAKitToCharactersWithNoItemsOnlyAndIsIdempotent(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	logs := captureLog(t)
	// cyb as the real database has it, made before items existed: a Bard with no items
	cyb, _ := s.Create(ctx, New{AccountID: acc, Name: "cyb", ClassID: 2, ClassName: "Bard", ArchetypeID: 17747})
	tank, _ := s.Create(ctx, New{AccountID: acc, Name: "Tank", ClassID: 7, ClassName: "Tank"})
	// one that has items already: untouched
	has, _ := s.Create(ctx, New{AccountID: acc, Name: "Has", ClassID: 2, Items: []Item{item(Backpack, 0, 555, guid(500))}})
	// one whose class is unknown (the migration of plan B1 could not derive it), one whose class has no kit
	old, _ := s.Create(ctx, New{AccountID: acc, Name: "Old"})
	odd, _ := s.Create(ctx, New{AccountID: acc, Name: "Odd", ClassID: 5, ClassName: "Rogue"})

	kits := &fakeKits{}
	res, err := s.GrantMissingKits(ctx, kits)
	if err != nil {
		t.Fatal(err)
	}
	if res.Without != 4 || res.Granted != 2 || res.Items != 4 || len(res.Skipped) != 2 {
		t.Errorf("result = %+v, want 4 without items, 2 granted (4 items), 2 skipped", res)
	}
	for _, id := range []string{cyb.ID, tank.ID} {
		got, _ := s.Inventory(ctx, id)
		if len(got) != 2 || got[0].Slot != 2 || got[1].Slot != 15 {
			t.Errorf("the kit of %s: %+v", id, got)
		}
	}
	if got, _ := s.Inventory(ctx, cyb.ID); got[0].ItemRecordID != 1002 {
		t.Errorf("cyb got %+v, want the Bard's kit (class 2)", got)
	}
	if got, _ := s.Inventory(ctx, has.ID); len(got) != 1 || got[0].ItemRecordID != 555 {
		t.Errorf("a character with items was given a kit: %+v", got)
	}
	for _, id := range []string{old.ID, odd.ID} {
		if got, _ := s.Inventory(ctx, id); len(got) != 0 {
			t.Errorf("a character that cannot have a kit has items: %+v", got)
		}
	}
	// the maker was asked only for the characters that had none and a class (never for the one with items)
	if !reflect.DeepEqual(kits.calls, []int32{2, 7, 5}) && !reflect.DeepEqual(kits.calls, []int32{5, 2, 7}) && !reflect.DeepEqual(kits.calls, []int32{2, 5, 7}) {
		t.Errorf("kits made for classes %v", kits.calls)
	}
	// the log says each grant and each skip, by name and id
	text := logs.String()
	for _, want := range []string{"starter kit granted (backfill)", "character_id=" + cyb.ID, "name=cyb", "items=2", "no starter kit granted",
		"character_id=" + old.ID, "class_id is NULL", "character_id=" + odd.ID, "class 5 has no starting kit", "starter kit backfill",
		"characters_without_items=4", "granted=2", "skipped=2"} {
		if !strings.Contains(text, want) {
			t.Errorf("the log lacks %q:\n%s", want, text)
		}
	}
	if strings.Contains(text, "character_id="+has.ID) {
		t.Errorf("the log mentions the character that had items:\n%s", text)
	}

	// once more: only the two that cannot have one are still without, nothing is granted, nothing changes
	before, _ := s.Inventory(ctx, cyb.ID)
	again, err := s.GrantMissingKits(ctx, kits)
	if err != nil || again.Without != 2 || again.Granted != 0 || again.Items != 0 || len(again.Skipped) != 2 {
		t.Errorf("second run = %+v, %v", again, err)
	}
	if after, _ := s.Inventory(ctx, cyb.ID); !reflect.DeepEqual(after, before) {
		t.Errorf("cyb's items changed by the second run: %+v", after)
	}
}

// ~ Cyb - C.O.A ~

func TestGrantMissingKitsNeverOverwritesItemsSavedInTheMeantime(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	captureLog(t)
	c, _ := s.Create(ctx, New{AccountID: acc, Name: "cyb", ClassID: 2})
	// worldd saves the character's inventory between the backfill's list of characters and its grant
	saved := []Item{item(Backpack, 0, 4242, guid(4242))}
	kits := &fakeKits{onKit: func(int32) {
		if err := s.SaveInventory(ctx, c.ID, saved); err != nil {
			t.Error(err)
		}
	}}
	res, err := s.GrantMissingKits(ctx, kits)
	if err != nil || res.Granted != 0 || res.Without != 1 {
		t.Errorf("result = %+v, %v; want nothing granted", res, err)
	}
	if got, _ := s.Inventory(ctx, c.ID); !reflect.DeepEqual(got, saved) {
		t.Errorf("the saved items were replaced: %+v", got)
	}
}

func TestGrantMissingKitsRefusesAKitThatBreaksTheRules(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	captureLog(t)
	c, _ := s.Create(ctx, New{AccountID: acc, Name: "cyb", ClassID: 2})
	broken := &brokenKits{}
	if _, err := s.GrantMissingKits(ctx, broken); !errors.Is(err, ErrInvalid) {
		t.Errorf("a kit with two items in a slot: %v, want ErrInvalid", err)
	}
	if got, _ := s.Inventory(ctx, c.ID); len(got) != 0 {
		t.Errorf("a broken kit was stored: %+v", got)
	}
}

type brokenKits struct{}

func (brokenKits) Kit(int32) ([]Item, error) {
	return []Item{item(Equipment, 2, 1, guid(1)), item(Equipment, 2, 2, guid(2))}, nil
}

// ---- the character's row lock --------------------------------------------------------------------------------

// The kit grant takes the character's row lock, so a save in flight cannot be mixed with the kit: it waits, then finds items and writes nothing.
func TestGrantKitWaitsForTheRowLockAndWritesNothingOverASave(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	captureLog(t)
	c, err := s.Create(ctx, New{AccountID: acc, Name: "cyb", ClassID: 2})
	if err != nil {
		t.Fatal(err)
	}
	// "worldd" holds the character's row lock in the middle of its SaveInventory transaction
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(ctx)
	if err := lockCharacter(ctx, tx, c.ID); err != nil {
		t.Fatal(err)
	}
	saved := []Item{item(Backpack, 0, 4242, guid(4242))}
	type result struct {
		granted bool
		err     error
	}
	done := make(chan result, 1)
	go func() {
		g, err := s.grantKit(ctx, c.ID, []Item{item(Equipment, 2, 1, guid(1)), item(Equipment, 15, 2, guid(2))})
		done <- result{g, err}
	}()
	select {
	case r := <-done:
		t.Fatalf("grantKit did not wait for the row lock (granted=%v err=%v)", r.granted, r.err)
	case <-time.After(400 * time.Millisecond):
	}
	if err := insertItems(ctx, tx, c.ID, saved); err != nil {
		t.Fatal(err)
	}
	if err := tx.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	r := <-done
	if r.err != nil || r.granted {
		t.Errorf("grantKit after the save: granted=%v err=%v, want false and no error", r.granted, r.err)
	}
	if got, _ := s.Inventory(ctx, c.ID); !reflect.DeepEqual(got, saved) {
		t.Errorf("the character has %+v, want only what was saved", got)
	}
}

// built with C O A and C Y B

// SaveInventory takes the same lock: a save that arrives while a kit grant is open waits for it and then replaces the kit, never mixing with it.
func TestSaveInventoryWaitsForAnOpenKitGrant(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c, err := s.Create(ctx, New{AccountID: acc, Name: "cyb", ClassID: 2})
	if err != nil {
		t.Fatal(err)
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(ctx)
	if err := lockCharacter(ctx, tx, c.ID); err != nil {
		t.Fatal(err)
	}
	kit := []Item{item(Equipment, 2, 1, guid(1)), item(Equipment, 15, 2, guid(2))}
	if err := insertItems(ctx, tx, c.ID, kit); err != nil {
		t.Fatal(err)
	}
	saved := []Item{item(Backpack, 0, 4242, guid(4242)), item(Equipment, 2, 1, guid(1))}
	done := make(chan error, 1)
	go func() { done <- s.SaveInventory(ctx, c.ID, saved) }()
	select {
	case err := <-done:
		t.Fatalf("SaveInventory did not wait for the open kit grant (err=%v)", err)
	case <-time.After(400 * time.Millisecond):
	}
	if err := tx.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	if err := <-done; err != nil {
		t.Fatal(err)
	}
	got, _ := s.Inventory(ctx, c.ID)
	if len(got) != 2 || got[0].ItemRecordID != 4242 {
		t.Errorf("the character has %+v, want exactly the saved two", got)
	}
}

// A character whose row stays locked past the grant's lock timeout is skipped (logged), the backfill goes on with the others,
// nothing is written for it, and the next run grants it.
func TestGrantMissingKitsSkipsACharacterWhoseRowStaysLocked(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	logs := captureLog(t)
	saved := grantLockTimeout
	grantLockTimeout = 300 * time.Millisecond
	t.Cleanup(func() { grantLockTimeout = saved })
	locked, err := s.Create(ctx, New{AccountID: acc, Name: "Locked", ClassID: 2})
	if err != nil {
		t.Fatal(err)
	}
	free, err := s.Create(ctx, New{AccountID: acc, Name: "Free", ClassID: 2})
	if err != nil {
		t.Fatal(err)
	}
	// a save of Locked's items is in flight (it holds the row lock) for the whole backfill
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(ctx)
	if err := lockCharacter(ctx, tx, locked.ID); err != nil {
		t.Fatal(err)
	}
	kits := &fakeKits{}
	start := time.Now()
	res, err := s.GrantMissingKits(ctx, kits)
	if err != nil {
		t.Fatalf("a locked row stopped the backfill: %v", err)
	}
	if took := time.Since(start); took < 250*time.Millisecond || took > 4*time.Second {
		t.Errorf("the backfill took %v, want the lock timeout (300ms) and no more", took)
	}
	if res.Without != 2 || res.Granted != 1 || res.Items != 2 || len(res.Skipped) != 1 || !strings.Contains(res.Skipped[0], locked.ID) {
		t.Errorf("result = %+v, want 2 without items, only Free granted, Locked skipped", res)
	}
	if got, _ := s.Inventory(ctx, free.ID); len(got) != 2 {
		t.Errorf("the free character's kit: %+v", got)
	}
	text := logs.String()
	for _, want := range []string{"no starter kit granted", "character_id=" + locked.ID, "name=Locked", "locked by another transaction", "granted=1", "skipped=1"} {
		if !strings.Contains(text, want) {
			t.Errorf("the log lacks %q:\n%s", want, text)
		}
	}
	// the save ends without items for Locked: the next backfill grants it
	if err := tx.Rollback(ctx); err != nil {
		t.Fatal(err)
	}
	again, err := s.GrantMissingKits(ctx, kits)
	if err != nil || again.Granted != 1 || len(again.Skipped) != 0 {
		t.Errorf("next run = %+v, %v; want Locked granted", again, err)
	}
	if got, _ := s.Inventory(ctx, locked.ID); len(got) != 2 {
		t.Errorf("Locked's kit: %+v", got)
	}
}
