-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
-- Storage clerks, step 0, "put it in, take it out": the items a character keeps in the warehouse.
--
-- One NEW table, character_warehouse_items, with the item columns and the constraints of character_items (migration 0004 and 0007). A warehouse has three kinds of containers:
-- the item chunks (area 'Items': the same in every town, so node_guid is 0 and container_guid is the chunk's record guid), the material tabs (area 'Materials': one set per
-- town, node_guid is the town's record guid and container_guid the tab's record guid) and the cargo tab (area 'Cargo', the same keys). The slot is the item's place in its
-- container: the index in a chunk (a plain list), the row-major index of the item's top-left cell in a tab (a grid, as in a material bag).
--
-- worldd keeps the warehouse while a character plays and saves it with the items in the same call (CharacterService.SaveInventory with replace_warehouse), so a move between a bag
-- and the warehouse is stored together or not at all. An instance guid is unique per character across all the item tables: that rule is checked in the service's Go code
-- (ValidateWarehouseItems); within this table the UNIQUE below holds it.
--
-- Nothing is backfilled: a character that stored nothing has no row.
--
-- This ADDS ONE TABLE AND ONLY WRITES TO IT: no existing table, row, column or constraint is touched, nothing is dropped. Rolling it back is a documented block in
-- services/README.md ("Rolling back migration 0017").

SET LOCAL lock_timeout = '10s';

CREATE TABLE character_warehouse_items (
    character_id   TEXT        NOT NULL REFERENCES characters (id) ON DELETE CASCADE,
    area           TEXT        NOT NULL
                   CONSTRAINT character_warehouse_items_area CHECK (area IN ('Items', 'Materials', 'Cargo')),
    node_guid      BIGINT      NOT NULL DEFAULT 0
                   CONSTRAINT character_warehouse_items_node CHECK (node_guid >= 0),
    container_guid BIGINT      NOT NULL
                   CONSTRAINT character_warehouse_items_container_positive CHECK (container_guid > 0),
    slot           INT         NOT NULL
                   CONSTRAINT character_warehouse_items_slot CHECK (slot >= 0 AND slot < 1024),
    item_record_id BIGINT      NOT NULL
                   CONSTRAINT character_warehouse_items_record_positive CHECK (item_record_id > 0),
    instance_guid  TEXT        NOT NULL
                   CONSTRAINT character_warehouse_items_guid_hex CHECK (instance_guid ~ '^[0-9a-f]{32}$' AND instance_guid <> repeat('0', 32)),
    quantity       INT         NOT NULL DEFAULT 1
                   CONSTRAINT character_warehouse_items_quantity CHECK (quantity BETWEEN 1 AND 1000000),
    quality        INT         NOT NULL DEFAULT 1
                   CONSTRAINT character_warehouse_items_quality CHECK (quality BETWEEN 1 AND 200),
    durability     REAL        NOT NULL DEFAULT 0
                   CONSTRAINT character_warehouse_items_durability CHECK (durability >= 0 AND durability <= 1e9),   -- NaN is above 1e9 in PostgreSQL
    created_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
    persisted_health REAL,
    persisted_mana   REAL,
    lock_expiration  BIGINT    NOT NULL DEFAULT 0,
    CONSTRAINT character_warehouse_items_where
        CHECK ((area = 'Items' AND node_guid = 0) OR (area <> 'Items' AND node_guid > 0)),
    CONSTRAINT character_warehouse_items_persisted_health CHECK (persisted_health IS NULL OR (persisted_health >= 0 AND persisted_health <= 1e9)),
    CONSTRAINT character_warehouse_items_persisted_mana   CHECK (persisted_mana   IS NULL OR (persisted_mana   >= 0 AND persisted_mana   <= 1e9)),
    CONSTRAINT character_warehouse_items_lock_expiration  CHECK (lock_expiration >= 0 AND lock_expiration <= 253402300799),
    CONSTRAINT character_warehouse_items_pkey PRIMARY KEY (character_id, area, node_guid, container_guid, slot),
    CONSTRAINT character_warehouse_items_guid_unique UNIQUE (character_id, instance_guid)
);

COMMENT ON TABLE character_warehouse_items IS 'Storage clerks, step 0: the items of a character in the warehouse, one row per instance. worldd saves them with the other items (SaveInventory with replace_warehouse).';
COMMENT ON COLUMN character_warehouse_items.area IS 'Items (the item chunks, the same in every town), Materials (the material tabs of a town) or Cargo (the cargo tab of a town).';
COMMENT ON COLUMN character_warehouse_items.node_guid IS 'The town''s record guid for Materials and Cargo; 0 for Items.';
COMMENT ON COLUMN character_warehouse_items.container_guid IS 'The design database''s WarehouseStorageItemChunk (Items) or WarehouseStorageMaterialTab (Materials, Cargo) Guid.';
COMMENT ON COLUMN character_warehouse_items.slot IS 'The index in an item chunk, or the row-major index of the item''s top-left cell in a tab.';
