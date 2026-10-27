// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package content

import (
	"database/sql"
	"fmt"
)

// Storage clerks (the storage milestone, step 0, "put it in, take it out"; contentc version 25): what a town's warehouse holds and what a window lists.
//
//	warehouse_node             the three defs a node names: its material tabs, its cargo tab and its item chunks
//	warehouse_def              a def of material or cargo tabs: the tab grid's columns and rows
//	warehouse_def_tab          the tabs of a def, in the def's order (the order the window and its summary list them)
//	warehouse_tab              one tab: the kind of goods (General or Crates), whether it is a member's tab, whether it can be given a job, the expression that says when it is
//	                           available and what it costs
//	warehouse_chunks_def       a def of item chunks
//	warehouse_chunks_def_chunk the chunks of a def, in the def's order
//	warehouse_chunk            one item chunk: its slots, the node level it needs, the availability expression and the costs
//
// A cost is {"c": currency guid, "ty": the currency's record type id, "v": value expression, "m": multiplier expression}; the expressions are literals in the data (the
// summary multiplies them). An availability expression is the data's text (an empty text and "true" always hold).
const WarehouseDDL = `
CREATE TABLE warehouse_node (
    node_guid    INTEGER PRIMARY KEY,   -- city_node.guid
    resource_def INTEGER NOT NULL,      -- WarehouseStorageDef of the material tabs (0: none)
    crate_def    INTEGER NOT NULL,      -- WarehouseStorageDef of the cargo tab (0: none)
    chunks_def   INTEGER NOT NULL       -- WarehouseStorageItemChunksDef (0: none)
);
CREATE TABLE warehouse_def (
    guid    INTEGER PRIMARY KEY,
    name    TEXT NOT NULL,
    columns INTEGER NOT NULL,           -- TabSlotColumns
    rows    INTEGER NOT NULL            -- TabSlotRows
);
CREATE TABLE warehouse_def_tab (
    def_guid INTEGER NOT NULL,
    ord      INTEGER NOT NULL,
    tab_guid INTEGER NOT NULL,
    PRIMARY KEY (def_guid, ord)
) WITHOUT ROWID;
CREATE TABLE warehouse_tab (
    guid          INTEGER PRIMARY KEY,
    name          TEXT NOT NULL,
    storage_type  INTEGER NOT NULL,     -- EResourceStorageType: 0 General, 1 Crates
    member        INTEGER NOT NULL,     -- bIsMemberStorage
    specializable INTEGER NOT NULL,     -- IsSpecializable
    requirement   TEXT NOT NULL,        -- AvailabilityRequirements
    costs         TEXT NOT NULL         -- JSON list of {"c", "ty", "v", "m"}
);
CREATE TABLE warehouse_chunks_def (
    guid INTEGER PRIMARY KEY,
    name TEXT NOT NULL
);
CREATE TABLE warehouse_chunks_def_chunk (
    def_guid   INTEGER NOT NULL,
    ord        INTEGER NOT NULL,
    chunk_guid INTEGER NOT NULL,
    PRIMARY KEY (def_guid, ord)
) WITHOUT ROWID;
CREATE TABLE warehouse_chunk (
    guid        INTEGER PRIMARY KEY,
    name        TEXT NOT NULL,
    size        INTEGER NOT NULL,       -- ChunkSize: the slots of the chunk
    level       INTEGER NOT NULL,       -- LevelRequirement
    requirement TEXT NOT NULL,          -- AvailabilityRequirements
    costs       TEXT NOT NULL           -- JSON list of {"c", "ty", "v", "m"}
);
`

// WarehouseTables are the tables WarehouseDDL creates, in the DDL's order.
var WarehouseTables = []string{"warehouse_node", "warehouse_def", "warehouse_def_tab", "warehouse_tab", "warehouse_chunks_def", "warehouse_chunks_def_chunk", "warehouse_chunk"}

func createWarehouse(db *sql.DB) error {
	if _, err := db.Exec(WarehouseDDL); err != nil {
		return fmt.Errorf("content: create the warehouse tables: %w", err)
	}
	return nil
}
