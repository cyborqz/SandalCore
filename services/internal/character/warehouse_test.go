// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

import (
	"context"
	"errors"
	"testing"
)

// Storage clerks, step 0: the warehouse rows against a real database (the statements that write and read character_warehouse_items run here, with every column).

const (
	joevaNode   = 108646121231993
	halcyonNode = 108646610851452
	baseChunk   = 6064631806178426880
	baseTab     = 6064631957531525120
	cargoTab    = 6064631963946647553
)

// wh is a valid warehouse item; the tests change what they want.
func wh(area string, node, container int64, slot int32, record int64, guidN int, quantity int32) WarehouseItem {
	return WarehouseItem{Area: area, NodeGUID: node, ContainerGUID: container, Slot: slot,
		Item: Item{ItemRecordID: record, InstanceGUID: guid(guidN), Quantity: quantity, Quality: 1, Durability: 11, CreatedAt: created}}
}

// sameWarehouse compares two lists field by field (a stored time comes back in the database's zone, so it is compared as milliseconds).
func sameWarehouse(t *testing.T, what string, got, want []WarehouseItem) {
	t.Helper()
	if len(got) != len(want) {
		t.Fatalf("%s: %d items %+v, want %d %+v", what, len(got), got, len(want), want)
	}
	for i := range want {
		g, w := got[i], want[i]
		if g.Area != w.Area || g.NodeGUID != w.NodeGUID || g.ContainerGUID != w.ContainerGUID || g.Slot != w.Slot || g.ItemRecordID != w.ItemRecordID || g.InstanceGUID != w.InstanceGUID ||
			g.Quantity != w.Quantity || g.Quality != w.Quality || g.Durability != w.Durability || g.CreatedAt.UnixMilli() != w.CreatedAt.UnixMilli() ||
			g.LockExpiration != w.LockExpiration || (g.PersistedHealth == nil) != (w.PersistedHealth == nil) || (g.PersistedMana == nil) != (w.PersistedMana == nil) {
			t.Errorf("%s: item %d is %+v, want %+v", what, i, g, w)
		}
	}
}

func saveWarehouse(s *Store, id string, bag []Item, stored []WarehouseItem) error {
	return s.SaveInventoryEverything(context.Background(), id, bag, nil, false, nil, false, nil, false, stored, true)
}

func TestSaveInventoryEverythingStoresTheWarehouseAndReadsItBackInOrder(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c, err := s.Create(ctx, New{AccountID: acc, Name: "cyb", ClassID: 2, Items: smallKit()})
	if err != nil {
		t.Fatal(err)
	}
	if got, err := s.WarehouseItems(ctx, c.ID); err != nil || len(got) != 0 {
		t.Fatalf("a character that stored nothing: %+v, %v", got, err)
	}
	hp, mp := float32(12.5), float32(3)
	mace := wh(WarehouseItems, 0, baseChunk, 3, 6064633867197874424, 22, 1)
	mace.PersistedHealth, mace.PersistedMana, mace.LockExpiration = &hp, &mp, 1_790_000_999
	stored := []WarehouseItem{
		wh(WarehouseMaterials, joevaNode, baseTab, 31, 6064634086348357635, 20, 20),
		wh(WarehouseItems, 0, baseChunk, 25, 6064632025123913728, 21, 16),
		mace,
		wh(WarehouseMaterials, halcyonNode, baseTab, 5, 6064634086348357635, 23, 5),
		wh(WarehouseMaterials, joevaNode, baseTab, 9, 6064628753953390592, 24, 10),
		wh(WarehouseCargo, joevaNode, cargoTab, 0, 6064634086348357635, 25, 1),
	}
	bag := smallKit()
	if err := saveWarehouse(s, c.ID, bag, stored); err != nil {
		t.Fatalf("SaveInventoryEverything with stored items: %v", err)
	}
	// read back by area (Items, Materials, Cargo), town, container and slot
	want := append([]WarehouseItem(nil), stored...)
	SortWarehouseItems(want)
	got, err := s.WarehouseItems(ctx, c.ID)
	if err != nil {
		t.Fatal(err)
	}
	sameWarehouse(t, "after the save", got, want)
	if got[0].Area != WarehouseItems || got[0].Slot != 3 || got[0].PersistedHealth == nil || *got[0].PersistedHealth != 12.5 || got[0].LockExpiration != 1_790_000_999 {
		t.Errorf("the first item (the mace of the chunk): %+v", got[0])
	}
	// the bags were written in the same call
	if inv, err := s.Inventory(ctx, c.ID); err != nil || len(inv) != len(bag) {
		t.Errorf("the bags after the save: %d items, %v", len(inv), err)
	}
}

func TestSaveInventoryEverythingReplacesOrKeepsTheWarehouse(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c, err := s.Create(ctx, New{AccountID: acc, Name: "cyb", ClassID: 2, Items: smallKit()})
	if err != nil {
		t.Fatal(err)
	}
	first := []WarehouseItem{wh(WarehouseItems, 0, baseChunk, 1, 7001, 40, 2), wh(WarehouseMaterials, joevaNode, baseTab, 2, 7002, 41, 3)}
	if err := saveWarehouse(s, c.ID, smallKit(), first); err != nil {
		t.Fatal(err)
	}
	// a save that does not replace the warehouse (the older saves) leaves it as it is
	if err := s.SaveInventory(ctx, c.ID, smallKit()); err != nil {
		t.Fatal(err)
	}
	if got, _ := s.WarehouseItems(ctx, c.ID); len(got) != 2 {
		t.Errorf("a save that does not name the warehouse changed it: %+v", got)
	}
	// a save that replaces it makes it exactly that: one stack moved, one taken out
	second := []WarehouseItem{wh(WarehouseItems, 0, baseChunk, 6, 7001, 40, 2)}
	if err := saveWarehouse(s, c.ID, smallKit(), second); err != nil {
		t.Fatal(err)
	}
	got, _ := s.WarehouseItems(ctx, c.ID)
	sameWarehouse(t, "after the second save", got, second)
	// and an empty list empties it
	if err := saveWarehouse(s, c.ID, smallKit(), nil); err != nil {
		t.Fatal(err)
	}
	if got, err := s.WarehouseItems(ctx, c.ID); err != nil || len(got) != 0 {
		t.Errorf("after an empty replacement: %+v, %v", got, err)
	}
}

func TestSaveInventoryEverythingRefusalsChangeNothing(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c, err := s.Create(ctx, New{AccountID: acc, Name: "cyb", ClassID: 2, Items: smallKit()})
	if err != nil {
		t.Fatal(err)
	}
	kept := []WarehouseItem{wh(WarehouseItems, 0, baseChunk, 1, 7001, 50, 2)}
	if err := saveWarehouse(s, c.ID, smallKit(), kept); err != nil {
		t.Fatal(err)
	}
	bagItems := func() []Item { got, _ := s.Inventory(ctx, c.ID); return got }
	bagBefore := bagItems()

	badArea := wh("Bank", 0, baseChunk, 1, 7001, 51, 1)
	chunkWithTown := wh(WarehouseItems, joevaNode, baseChunk, 1, 7001, 52, 1)
	tabWithoutTown := wh(WarehouseMaterials, 0, baseTab, 1, 7001, 53, 1)
	badSlot := wh(WarehouseItems, 0, baseChunk, MaxWarehouseSlot, 7001, 54, 1)
	noContainer := wh(WarehouseItems, 0, 0, 1, 7001, 55, 1)
	zeroQuantity := wh(WarehouseItems, 0, baseChunk, 1, 7001, 56, 0)
	for name, tc := range map[string][]WarehouseItem{
		"an area that is none":                 {badArea},
		"a chunk that belongs to a town":       {chunkWithTown},
		"a tab that belongs to no town":        {tabWithoutTown},
		"a slot out of range":                  {badSlot},
		"a container that is not a record":     {noContainer},
		"quantity 0":                           {zeroQuantity},
		"two items in one place":               {wh(WarehouseItems, 0, baseChunk, 1, 7001, 57, 1), wh(WarehouseItems, 0, baseChunk, 1, 7002, 58, 1)},
		"two items with one instance":          {wh(WarehouseItems, 0, baseChunk, 1, 7001, 59, 1), wh(WarehouseItems, 0, baseChunk, 2, 7001, 59, 1)},
		"an instance that is also in the bags": {wh(WarehouseItems, 0, baseChunk, 1, 7001, 1, 1)},
	} {
		if err := saveWarehouse(s, c.ID, smallKit(), tc); !errors.Is(err, ErrInvalid) {
			t.Errorf("%s: %v, want ErrInvalid", name, err)
		}
		got, _ := s.WarehouseItems(ctx, c.ID)
		sameWarehouse(t, name+" changed the warehouse", got, kept)
		if after := bagItems(); len(after) != len(bagBefore) {
			t.Fatalf("%s changed the bags: %d items, were %d", name, len(after), len(bagBefore))
		}
	}
	// too many items for one save
	tooMany := make([]WarehouseItem, MaxWarehouseItems+1)
	for i := range tooMany {
		tooMany[i] = wh(WarehouseItems, 0, baseChunk, int32(i%MaxWarehouseSlot), 7001, 1000+i, 1)
	}
	if err := saveWarehouse(s, c.ID, smallKit(), tooMany); !errors.Is(err, ErrInvalid) {
		t.Errorf("more items than a save can hold: %v, want ErrInvalid", err)
	}
}

func TestWarehouseItemsOfUnknownAndMalformedIDsIsNotFound(t *testing.T) {
	s, _ := setup(t)
	ctx := context.Background()
	for _, id := range []string{"", "nothex", "00000000000000000000000000000000", "0123456789abcdef0123456789abcdef"} {
		if _, err := s.WarehouseItems(ctx, id); !errors.Is(err, ErrNotFound) {
			t.Errorf("WarehouseItems(%q): %v, want ErrNotFound", id, err)
		}
	}
	if err := saveWarehouse(s, "not a character", smallKit(), nil); !errors.Is(err, ErrNotFound) {
		t.Errorf("a save for a malformed id: %v, want ErrNotFound", err)
	}
}

func TestDeletingACharacterDeletesItsWarehouseAndNeverTouchesAnothers(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	a, _ := s.Create(ctx, New{AccountID: acc, Name: "Alpha", ClassID: 2, Items: smallKit()})
	b, _ := s.Create(ctx, New{AccountID: acc, Name: "Bravo", ClassID: 2, Items: []Item{item(Backpack, 0, 1, guid(90))}})
	if err := saveWarehouse(s, a.ID, smallKit(), []WarehouseItem{wh(WarehouseItems, 0, baseChunk, 1, 7001, 60, 2)}); err != nil {
		t.Fatal(err)
	}
	// the same slot and a different instance for another character: the places are per character
	if err := saveWarehouse(s, b.ID, []Item{item(Backpack, 0, 1, guid(90))}, []WarehouseItem{wh(WarehouseItems, 0, baseChunk, 1, 7001, 61, 2)}); err != nil {
		t.Fatal(err)
	}
	if err := s.Delete(ctx, acc, a.ID); err != nil {
		t.Fatal(err)
	}
	var n int
	if err := s.pool.QueryRow(ctx, `SELECT count(*) FROM character_warehouse_items WHERE character_id = $1`, a.ID).Scan(&n); err != nil || n != 0 {
		t.Errorf("%d warehouse items of the deleted character remain (%v)", n, err)
	}
	if got, err := s.WarehouseItems(ctx, b.ID); err != nil || len(got) != 1 || got[0].InstanceGUID != guid(61) {
		t.Errorf("the other character's warehouse: %+v, %v", got, err)
	}
}
