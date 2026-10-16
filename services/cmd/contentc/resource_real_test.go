// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

// Gathering stage 3, "Resource storage" (contentc version 14), on the real design database (the shared build of real_test.go): the item table's inv_w, inv_h and
// resource columns against the research's table of all 10,568 items, and the 70 resource bags' grids against the three payloads of capture 1198 (the login of a
// character who wore a Sanctus bag, a Mining T2 bag and a Lumberjacking T2 bag) to the cell.

import (
	"testing"

	"aocrop/services/internal/content"
)

func TestRealResourceItems(t *testing.T) {
	db := builtDB(t)
	meta, err := db.Meta()
	if err != nil {
		t.Fatal(err)
	}
	count := func(q string, args ...any) int64 {
		t.Helper()
		return scalarInt(t, db, q, args...)
	}
	if n := count(`SELECT COUNT(*) FROM item`); n != 10568 {
		t.Fatalf("item has %d rows, want 10,568", n)
	}
	// the tag rule: 1,824 items carry a child of item.resource (case-insensitive), 182 of the 184 items the 236 gathering interactions reward
	if n := count(`SELECT COUNT(*) FROM item WHERE resource = 1`); n != 1824 {
		t.Errorf("%d resource items, want 1,824", n)
	}
	if meta["typed.item_resource"] != "1824" {
		t.Errorf("meta typed.item_resource = %q, want 1824", meta["typed.item_resource"])
	}
	// the column is the tag table's rule, item by item
	if n := count(`SELECT COUNT(*) FROM item i WHERE i.resource <> EXISTS (SELECT 1 FROM item_tag t WHERE t.item_guid = i.guid AND lower(t.tag) LIKE 'item.resource%')`); n != 0 {
		t.Errorf("%d items whose resource column is not the item_tag rule", n)
	}
	// the dimensions: (1,1) for all but 354 items, none larger than the Sanctus bag's 6 x 4
	for dims, want := range map[[2]int64]int64{{1, 1}: 10192, {2, 2}: 202, {1, 2}: 68, {1, 3}: 52, {2, 1}: 48, {2, 3}: 4, {3, 1}: 1, {4, 2}: 1} {
		if n := count(`SELECT COUNT(*) FROM item WHERE inv_w = ? AND inv_h = ?`, dims[0], dims[1]); n != want {
			t.Errorf("%d items of %d x %d, want %d", n, dims[0], dims[1], want)
		}
	}
	if n := count(`SELECT COUNT(*) FROM item WHERE inv_w > 6 OR inv_h > 4`); n != 0 {
		t.Errorf("%d items are larger than 6 x 4", n)
	}
	if n := count(`SELECT COUNT(*) FROM item WHERE inv_w < 1 OR inv_h < 1`); n != 0 {
		t.Errorf("%d items have no cell", n)
	}
	// the resource items by dimension and by stack size (research 1.1 and the table of check (b))
	for dims, want := range map[[2]int64]int64{{1, 1}: 1502, {2, 2}: 172, {1, 3}: 52, {2, 1}: 48, {1, 2}: 48, {3, 1}: 1, {4, 2}: 1} {
		if n := count(`SELECT COUNT(*) FROM item WHERE resource = 1 AND inv_w = ? AND inv_h = ?`, dims[0], dims[1]); n != want {
			t.Errorf("%d resource items of %d x %d, want %d", n, dims[0], dims[1], want)
		}
	}
	for stack, want := range map[int64]int64{1: 202, 3: 1, 10: 6, 20: 1155, 99: 429, 100: 31} {
		if n := count(`SELECT COUNT(*) FROM item WHERE resource = 1 AND max_stack_size = ?`, stack); n != want {
			t.Errorf("%d resource items stack to %d, want %d", n, stack, want)
		}
	}
	// examples the research and the captures name
	oak, err := db.Item(mustItem(t, db, "Resource_Raw_Wood_Oak"))
	if err != nil || oak.InvW != 1 || oak.InvH != 3 || !oak.Resource || oak.MaxStackSize != 20 {
		t.Errorf("Resource_Raw_Wood_Oak = %+v, %v; want 1 x 3, resource, stack 20", oak, err)
	}
	sanctus, err := db.Item(6064629382556287782)
	if err != nil || sanctus.Resource || sanctus.InvW != 1 || sanctus.InvH != 1 {
		t.Errorf("Bag_Resource_Sanctus = %+v, %v; want a 1 x 1 item that is not a resource", sanctus, err)
	}
	bear, err := db.Item(mustItem(t, db, "Resource_Raw_Carcass_Bear"))
	if err != nil || bear.InvW != 2 || bear.InvH != 2 || !bear.Resource {
		t.Errorf("Resource_Raw_Carcass_Bear = %+v, %v; want 2 x 2, resource (1198's bear sits at slot 3 of the 8 x 5 grid)", bear, err)
	}
	if n := count(`SELECT COUNT(*) FROM item WHERE name = 'Certificate_T1_Glint' AND resource = 1`); n != 1 {
		t.Errorf("Certificate_T1_Glint is not a resource item (%d)", n)
	}
}

// (C O A | C_Y_B)

func TestRealResourceBags(t *testing.T) {
	db := builtDB(t)
	meta, err := db.Meta()
	if err != nil {
		t.Fatal(err)
	}
	bags, err := db.ResourceBags()
	if err != nil || len(bags) != 70 {
		t.Fatalf("ResourceBags = %d, %v; want the 70 items with a ResourceBagDefId", len(bags), err)
	}
	if meta["typed.resource_bag"] != "70" {
		t.Errorf("meta typed.resource_bag = %q, want 70", meta["typed.resource_bag"])
	}
	// 64 are worn in the three bag slots (ResourceBag_0 23, _1 24, _2 25), 6 are warehouse tab definitions
	worn := 0
	largest := ResourceLargest{}
	for _, b := range bags {
		if n := scalarInt(t, db, `SELECT COUNT(*) FROM item_equip_slot WHERE item_guid = ? AND slot IN (23, 24, 25)`, b.ItemGuid); n == 3 {
			worn++
		}
		if b.Columns < 1 || b.Rows < 1 {
			t.Errorf("bag %d has a %d x %d grid", b.ItemGuid, b.Columns, b.Rows)
		}
		if b.Custom {
			// a custom matrix is the grid, column by column
			if len(b.Matrix) != b.Columns {
				t.Errorf("bag %d (%s): %d matrix columns for a grid of %d", b.ItemGuid, b.DefName, len(b.Matrix), b.Columns)
			}
			for c, col := range b.Matrix {
				if len(col) != b.Rows {
					t.Errorf("bag %d (%s): matrix column %d has %d cells for %d rows", b.ItemGuid, b.DefName, c, len(col), b.Rows)
				}
			}
		}
		if cells := b.Columns * b.Rows; cells > largest.Cells {
			largest = ResourceLargest{Cells: cells, Name: b.DefName}
		}
	}
	if worn != 64 {
		t.Errorf("%d bags are worn in all three bag slots, want 64", worn)
	}
	if largest.Cells != 240 { // Bag_Resource_SandalsOfHolding, 8 x 30 (the Livestream promotion item)
		t.Errorf("the largest bag has %d cells (%s), want 240", largest.Cells, largest.Name)
	}
	byName := func(name string) content.ResourceBag {
		t.Helper()
		g := mustItem(t, db, name)
		b, err := db.ResourceBag(g)
		if err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		return b
	}
	type want struct {
		cols, rows int
		custom     bool
		blocked    int
	}
	for name, w := range map[string]want{
		"Bag_Resource_Sanctus":                   {6, 4, false, 0}, // 6 x 4 = 24, no blocked cell (its def carries a 4-column matrix a non-custom grid ignores)
		"Bag_Resource_T1_Mining_0":               {5, 5, true, 9},
		"Bag_Resource_T1_Lumberjacking_0":        {7, 3, true, 9},
		"Bag_Resource_T2_Mining_0":               {8, 5, true, 16}, // 1198's Resource1
		"Bag_Resource_T2_Lumberjacking_0":        {6, 5, true, 12}, // 1198's Resource2
		"Bag_Resource_T3_Mining_0":               {8, 8, true, 28}, // the largest worn bag of the gathering professions
		"Bag_Resource_FTUE_Bag":                  {4, 3, false, 0},
		"Bag_Resource_SandalsOfHolding":          {8, 30, false, 0},
		"Bag_Resource_T1_Lumberjacking_Quantity": {8, 3, true, 12},
	} {
		b := byName(name)
		if b.Columns != w.cols || b.Rows != w.rows || b.Custom != w.custom || b.Blocked() != w.blocked {
			t.Errorf("%s = %dx%d custom %v blocked %d, want %dx%d custom %v blocked %d", name, b.Columns, b.Rows, b.Custom, b.Blocked(), w.cols, w.rows, w.custom, w.blocked)
		}
	}
	// the Sanctus def's matrix is stored although it does not apply
	if s := byName("Bag_Resource_Sanctus"); len(s.Matrix) != 4 || s.Blocked() != 0 {
		t.Errorf("the Sanctus def's stored matrix = %v (blocked %d), want the 4 columns of the data", s.Matrix, s.Blocked())
	}

	// capture 1198: the override matrices the client was sent for the Mining T2 bag (8 x 5, columns 2 and 5 and row 2 blocked) and the Lumberjacking T2 bag (6 x 5)
	const F, B = content.CellUsable, content.CellBlocked
	mining := [][]int{{F, F, B, F, F}, {F, F, B, F, F}, {B, B, B, B, B}, {F, F, B, F, F}, {F, F, B, F, F}, {B, B, B, B, B}, {F, F, B, F, F}, {F, F, B, F, F}}
	lumber := [][]int{{B, B, F, F, F}, {F, F, F, B, B}, {B, B, F, F, F}, {F, F, F, B, B}, {B, B, F, F, F}, {F, F, F, B, B}}
	for name, wantMatrix := range map[string][][]int{"Bag_Resource_T2_Mining_0": mining, "Bag_Resource_T2_Lumberjacking_0": lumber} {
		got := byName(name).Matrix
		if len(got) != len(wantMatrix) {
			t.Errorf("%s: %d columns, capture 1198 has %d", name, len(got), len(wantMatrix))
			continue
		}
		for c := range wantMatrix {
			for r := range wantMatrix[c] {
				if got[c][r] != wantMatrix[c][r] {
					t.Errorf("%s: cell (%d, %d) = %d, capture 1198 has %d", name, c, r, got[c][r], wantMatrix[c][r])
				}
			}
		}
	}
	// every class's kit wears the Sanctus bag in slot 23 (research 1.2)
	for _, class := range content.PlayerClasses {
		kit, err := db.StartingKit(class)
		if err != nil {
			t.Fatal(err)
		}
		found := false
		for _, k := range kit {
			if k.Slot == 23 && k.ItemGuid == 6064629382556287782 {
				found = true
			}
		}
		if !found {
			t.Errorf("%s's kit does not wear Bag_Resource_Sanctus in slot 23", class)
		}
	}
}

// ResourceLargest is the biggest grid seen.
type ResourceLargest struct {
	Cells int
	Name  string
}

// Cyb - C-O-A

func mustItem(t *testing.T, db *content.DB, name string) int64 {
	t.Helper()
	var g int64
	if err := db.SQL().QueryRow(`SELECT guid FROM item WHERE name = ?`, name).Scan(&g); err != nil {
		t.Fatalf("item %q: %v", name, err)
	}
	return g
}

// Gathering stage 3, "the pick-up popup" (contentc version 16): item.display_name is the record's ItemName source text.  The 17 items the 24 retail harvest events name ("Acquired
// <a ...> [Coconut Wood] </>x5": captures 1405 to 1519, 1178, 1179) must read the name the retail server wrote into the text.
func TestRealItemDisplayNames(t *testing.T) {
	db := builtDB(t)
	for guid, want := range map[int64]string{
		6064633254017630211: "Coconut Wood", 6064629184431849472: "Daffodil", 109162169652924: "Oak Wood", 6064630520851136512: "Moonbell", 6064629207102521358: "Snowdrop",
		6064630406148390932: "Bear Carcass Portion", 6064630406124011534: "Raven Carcass Portion", 6064629326302543894: "Wolf", 6064631481841025063: "Weeping Willow Wood",
		6064628912716447747: "Ash Wood", 6064629252186439693: "Rough Halcyonite", 6064629252228972574: "Limestone", 109162169652923: "Coal", 6064629252182507530: "Rough Sapphire",
		6064629252198170643: "Rividium", 6064634024974680066: "Ram Carcass Portion", 6064634024982937603: "Hawk Carcass Portion",
	} {
		it, err := db.Item(guid)
		if err != nil {
			t.Errorf("item %d: %v", guid, err)
			continue
		}
		if it.DisplayName != want {
			t.Errorf("item %d (%s) reads the display name %q, want %q", guid, it.Name, it.DisplayName, want)
		}
	}
	// every resource item has a name to show (the gathered ones at least)
	if n := scalarInt(t, db, `SELECT COUNT(*) FROM item WHERE resource = 1 AND display_name = ''`); n != 0 {
		t.Logf("%d of the 1,824 resource items have no ItemName (the popup falls back to the record name)", n)
	}
}
