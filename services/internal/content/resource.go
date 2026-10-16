// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package content

import (
	"database/sql"
	"errors"
	"fmt"
)

// Resource storage (milestone "Gathering stage 3", part "Resource storage"; contentc version 14): the grids of the resource bags, and what each item
// says of the storage it goes to. The item's own columns are in schema.go's item table: inv_w, inv_h (the record's InventoryDimension, in cells) and resource
// (1 when its GameplayTags hold a child of item.resource, compared case-insensitively: the data mixes Item.Resource.Raw.Reagent and item.resource.cert.nodecert).
//
//	resource_bag        the 70 items that carry a ResourceBagDefId: the bag item, its ResourceBagDef, the grid's columns and rows (BagDimension X and Y) and
//	                    bCustomDimension. The def's matrix applies only when custom is 1: the Sanctus def carries a 4-column matrix that its 6 x 4 grid ignores
//	resource_bag_cell   the def's BagMatrixOverride, one row per cell: the column (the matrix is a list of columns), the row, and the value, -1 usable or
//	                    -2 blocked (a blocked cell takes no item, but counts in maximum_items = columns * rows)
const ResourceDDL = `
CREATE TABLE resource_bag (
    item_guid INTEGER PRIMARY KEY,   -- the bag item (ItemRecord)
    def_guid  INTEGER NOT NULL,      -- its ResourceBagDef
    def_name  TEXT NOT NULL,         -- Item_Bag_Resource_Sanctus_def
    columns   INTEGER NOT NULL,      -- BagDimension.X
    rows      INTEGER NOT NULL,      -- BagDimension.Y
    custom    INTEGER NOT NULL       -- bCustomDimension: 1 when the def's matrix applies
);
CREATE INDEX resource_bag_by_def ON resource_bag (def_guid);
CREATE TABLE resource_bag_cell (
    def_guid INTEGER NOT NULL,
    col      INTEGER NOT NULL,       -- the index in BagMatrixOverride
    row      INTEGER NOT NULL,       -- the index in the column
    value    INTEGER NOT NULL,       -- -1 usable, -2 blocked
    PRIMARY KEY (def_guid, col, row)
) WITHOUT ROWID;
`

// ResourceTables are the tables ResourceDDL creates, in the DDL's order.
var ResourceTables = []string{"resource_bag", "resource_bag_cell"}

// built by Cyb with C | O | A

func createResource(db *sql.DB) error {
	if _, err := db.Exec(ResourceDDL); err != nil {
		return fmt.Errorf("content: create the resource bag tables: %w", err)
	}
	return nil
}

// The values of a bag matrix's cells.
const (
	CellUsable  = -1
	CellBlocked = -2
)

// ResourceBag is the grid a worn resource bag gives its container.
type ResourceBag struct {
	ItemGuid int64
	DefGuid  int64
	DefName  string
	Columns  int
	Rows     int
	Custom   bool    // the matrix applies
	Matrix   [][]int // Matrix[col][row], CellUsable or CellBlocked; empty when the def has none
}

// Blocked is the number of blocked cells the grid has (0 when the matrix does not apply).
func (b ResourceBag) Blocked() int {
	if !b.Custom {
		return 0
	}
	n := 0
	for _, col := range b.Matrix {
		for _, v := range col {
			if v == CellBlocked {
				n++
			}
		}
	}
	return n
}

// ResourceBag returns the grid of a bag item; an error for an item that is no resource bag.
func (d *DB) ResourceBag(itemGuid int64) (ResourceBag, error) {
	var b ResourceBag
	err := d.sql.QueryRow(`SELECT item_guid, def_guid, def_name, columns, rows, custom FROM resource_bag WHERE item_guid = ?`, itemGuid).
		Scan(&b.ItemGuid, &b.DefGuid, &b.DefName, &b.Columns, &b.Rows, &b.Custom)
	if errors.Is(err, sql.ErrNoRows) {
		return ResourceBag{}, fmt.Errorf("content: item %d is no resource bag", itemGuid)
	}
	if err != nil {
		return ResourceBag{}, err
	}
	rows, err := d.sql.Query(`SELECT col, row, value FROM resource_bag_cell WHERE def_guid = ? ORDER BY col, row`, b.DefGuid)
	if err != nil {
		return ResourceBag{}, err
	}
	defer rows.Close()
	for rows.Next() {
		var col, row, v int
		if err := rows.Scan(&col, &row, &v); err != nil {
			return ResourceBag{}, err
		}
		for len(b.Matrix) <= col {
			b.Matrix = append(b.Matrix, nil)
		}
		for len(b.Matrix[col]) <= row {
			b.Matrix[col] = append(b.Matrix[col], CellUsable)
		}
		b.Matrix[col][row] = v
	}
	return b, rows.Err()
}

// ResourceBags returns every resource bag, by item guid order.
func (d *DB) ResourceBags() ([]ResourceBag, error) {
	rows, err := d.sql.Query(`SELECT item_guid FROM resource_bag ORDER BY item_guid`)
	if err != nil {
		return nil, err
	}
	var guids []int64
	for rows.Next() {
		var g int64
		if err := rows.Scan(&g); err != nil {
			rows.Close()
			return nil, err
		}
		guids = append(guids, g)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return nil, err
	}
	out := make([]ResourceBag, 0, len(guids))
	for _, g := range guids {
		b, err := d.ResourceBag(g)
		if err != nil {
			return nil, err
		}
		out = append(out, b)
	}
	return out, nil
}
