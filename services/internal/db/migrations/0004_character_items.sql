-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
-- Plan B2: the items a character owns: its bag, what it wears and its quest items.
--
-- One row per item instance. container says which storage, slot where in it: the Backpack has 70 slots, the
-- Equipment 83 (the slot is the client's EEquipSlot: 2 Chest, 15 Primary ...) and the Quest storage 100. item_record_id
-- is the design database's ItemRecord Guid (content.db's item.guid), instance_guid the item's own 128-bit id as the client
-- carries it (an FGuid, 32 lowercase hex digits), quality an index into the item's quality curve (Common 1-5 ... Artifact
-- 101-200), durability a float32 as the client sends it. created_at is when the item was made (the client shows it as
-- FDateTime ticks).
--
-- worldd is the single writer of a character's items once it plays (CharacterService.SaveInventory replaces the whole set
-- in one transaction, the latest save wins); the character service writes them at creation and, for a character that has
-- none, when it starts (the starter kit backfill: cmd/character). A character's items go with the character.
--
-- Nothing is added to or changed in the existing tables; the whole file runs in the migration's own transaction.

SET LOCAL lock_timeout = '10s';

CREATE TABLE character_items (
    character_id   TEXT        NOT NULL REFERENCES characters (id) ON DELETE CASCADE,
    container      TEXT        NOT NULL
                   CONSTRAINT character_items_container CHECK (container IN ('Backpack', 'Equipment', 'Quest')),
    slot           INT         NOT NULL,
    item_record_id BIGINT      NOT NULL
                   CONSTRAINT character_items_record_positive CHECK (item_record_id > 0),
    instance_guid  TEXT        NOT NULL
                   CONSTRAINT character_items_guid_hex CHECK (instance_guid ~ '^[0-9a-f]{32}$' AND instance_guid <> repeat('0', 32)),
    quantity       INT         NOT NULL DEFAULT 1
                   CONSTRAINT character_items_quantity CHECK (quantity BETWEEN 1 AND 1000000),
    quality        INT         NOT NULL DEFAULT 1
                   CONSTRAINT character_items_quality CHECK (quality BETWEEN 1 AND 200),
    durability     REAL        NOT NULL DEFAULT 0
                   CONSTRAINT character_items_durability CHECK (durability >= 0 AND durability <= 1e9),   -- NaN is above 1e9 in PostgreSQL
    created_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
    CONSTRAINT character_items_slot
        CHECK (slot >= 0 AND slot < CASE container WHEN 'Backpack' THEN 70 WHEN 'Equipment' THEN 83 ELSE 100 END),
    CONSTRAINT character_items_pkey PRIMARY KEY (character_id, container, slot),
    CONSTRAINT character_items_guid_unique UNIQUE (character_id, instance_guid)
);

COMMENT ON TABLE character_items IS 'Plan B2: the items of a character, one row per instance. worldd saves them (SaveInventory: the whole set, latest wins).';
COMMENT ON COLUMN character_items.slot IS 'Backpack 0-69, Equipment 0-82 (the client''s EEquipSlot), Quest 0-99.';
COMMENT ON COLUMN character_items.item_record_id IS 'The design database''s ItemRecord Guid (content.db item.guid).';
COMMENT ON COLUMN character_items.instance_guid IS 'The item''s own FGuid, 32 lowercase hex digits (the four int32 words in order).';
