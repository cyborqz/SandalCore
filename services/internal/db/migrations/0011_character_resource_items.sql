-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
-- Gathering stage 3, "Resource storage": the items a character keeps in its resource containers.
--
-- The client keeps what is gathered (raw wood, plants, ore, carcasses, reagents, certificates ...: every item whose gameplay tags hold a child of item.resource)
-- in up to three containers of its own, Resource0, Resource1 and Resource2. They are the resource bags worn in Equipment slots 23, 24 and 25 (each bag item gives its
-- container a grid of columns x rows cells, an item takes inv_w x inv_h of them), and the item's slot is the index of the top-left cell of the item's rectangle,
-- row by row. The Backpack and the Equipment refuse such an item, and the resource containers refuse everything else.
--
-- One NEW table, with the columns and the constraints of character_items (migration 0004) and the three container names. character_items itself is NOT touched: its
-- container CHECK stays Backpack / Equipment / Quest (every migration is add-only), and a character's resource items live only here. The slot is 0 to 255, the most
-- cells a bag can have (the Livestream promotion bag is 8 x 30 = 240); a character service that validates an item against its bag's real grid is not possible (it has
-- no item data), worldd does that. An instance guid is unique per character across BOTH tables: that rule is checked in the service's Go code (ValidateItems), a
-- table cannot hold a constraint over two tables, and within this table the UNIQUE below holds it.
--
-- Nothing is backfilled: a character's earlier harvests sit in its Backpack (Gathering stage 2 granted them there), and worldd moves them to Resource0 at the login.
-- worldd is the single writer of a character's items once it plays (CharacterService.SaveInventory replaces both tables in one transaction). A character's resource
-- items go with the character.
--
-- This ADDS ONE TABLE AND ONLY WRITES TO IT: no existing table, row, column or constraint is touched, nothing is dropped. Rolling it back is a documented block in
-- services/README.md ("Rolling back migration 0011").

SET LOCAL lock_timeout = '10s';

CREATE TABLE character_resource_items (
    character_id   TEXT        NOT NULL REFERENCES characters (id) ON DELETE CASCADE,
    container      TEXT        NOT NULL
                   CONSTRAINT character_resource_items_container CHECK (container IN ('Resource0', 'Resource1', 'Resource2')),
    slot           INT         NOT NULL
                   CONSTRAINT character_resource_items_slot CHECK (slot >= 0 AND slot < 256),
    item_record_id BIGINT      NOT NULL
                   CONSTRAINT character_resource_items_record_positive CHECK (item_record_id > 0),
    instance_guid  TEXT        NOT NULL
                   CONSTRAINT character_resource_items_guid_hex CHECK (instance_guid ~ '^[0-9a-f]{32}$' AND instance_guid <> repeat('0', 32)),
    quantity       INT         NOT NULL DEFAULT 1
                   CONSTRAINT character_resource_items_quantity CHECK (quantity BETWEEN 1 AND 1000000),
    quality        INT         NOT NULL DEFAULT 1
                   CONSTRAINT character_resource_items_quality CHECK (quality BETWEEN 1 AND 200),
    durability     REAL        NOT NULL DEFAULT 0
                   CONSTRAINT character_resource_items_durability CHECK (durability >= 0 AND durability <= 1e9),   -- NaN is above 1e9 in PostgreSQL
    created_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
    -- the columns of migration 0007: a resource item keeps none of them, but the table has the columns of character_items so that one row shape serves both
    persisted_health REAL
                   CONSTRAINT character_resource_items_persisted_health CHECK (persisted_health IS NULL OR (persisted_health >= 0 AND persisted_health <= 1e9)),
    persisted_mana   REAL
                   CONSTRAINT character_resource_items_persisted_mana   CHECK (persisted_mana   IS NULL OR (persisted_mana   >= 0 AND persisted_mana   <= 1e9)),
    lock_expiration  BIGINT    NOT NULL DEFAULT 0
                   CONSTRAINT character_resource_items_lock_expiration  CHECK (lock_expiration >= 0 AND lock_expiration <= 253402300799),
    CONSTRAINT character_resource_items_pkey PRIMARY KEY (character_id, container, slot),
    CONSTRAINT character_resource_items_guid_unique UNIQUE (character_id, instance_guid)
);

COMMENT ON TABLE character_resource_items IS 'Gathering stage 3: the items of a character''s resource containers (Resource0-2: the bags worn in Equipment slots 23-25), one row per instance. worldd saves them with the rest (SaveInventory).';
COMMENT ON COLUMN character_resource_items.slot IS 'The index (row by row) of the top-left cell of the item''s rectangle in the container''s grid: 0 to columns x rows - 1 of the bag, at most 255.';
COMMENT ON COLUMN character_resource_items.item_record_id IS 'The design database''s ItemRecord Guid (content.db item.guid).';
COMMENT ON COLUMN character_resource_items.instance_guid IS 'The item''s own FGuid, 32 lowercase hex digits; unique per character across character_items and this table (the service checks the other table).';
