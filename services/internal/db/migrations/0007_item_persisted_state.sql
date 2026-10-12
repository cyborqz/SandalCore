-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
-- The mounts milestone, stage 1: the state a mount item persists.
--
-- Retail's login shows it on every mount item (24 of them in capture 1198): an FItemInstance with two persisted stats, the mount's Health and Mana
-- as they were when it was last desummoned or died, and a LockExpiration (Unix seconds; a time in the past means the item is not locked). A mount that
-- dies locks its item for the lock time the data gives and comes back with the Health it was saved with.
--
-- This ADDS COLUMNS ONLY, all of them with a default or NULL: no row is rewritten with a value of its own (every existing item reads NULL, NULL and
-- 0, which the services read as "this item keeps nothing"), no constraint on the existing columns changes, and nothing is dropped. The play database
-- of the user's stage runs migrates in place. The whole file runs in the migration's own transaction. Rolling it back is a documented block in
-- services/README.md ("Rolling back migration 0007").

SET LOCAL lock_timeout = '10s';

ALTER TABLE character_items
    ADD COLUMN persisted_health REAL,
    ADD COLUMN persisted_mana   REAL,
    ADD COLUMN lock_expiration  BIGINT NOT NULL DEFAULT 0;

ALTER TABLE character_items
    ADD CONSTRAINT character_items_persisted_health CHECK (persisted_health IS NULL OR (persisted_health >= 0 AND persisted_health <= 1e9)),   -- NaN is above 1e9 in PostgreSQL
    ADD CONSTRAINT character_items_persisted_mana   CHECK (persisted_mana   IS NULL OR (persisted_mana   >= 0 AND persisted_mana   <= 1e9)),
    ADD CONSTRAINT character_items_lock_expiration  CHECK (lock_expiration >= 0 AND lock_expiration <= 253402300799);                             -- the end of the year 9999

COMMENT ON COLUMN character_items.persisted_health IS 'The mounts milestone: the Health a mount item persists (FItemInstance persisted stat 108986352686712). NULL: the item keeps none.';
COMMENT ON COLUMN character_items.persisted_mana IS 'The mounts milestone: the Mana a mount item persists (persisted stat 109183576135288). NULL: the item keeps none.';
COMMENT ON COLUMN character_items.lock_expiration IS 'The mounts milestone: FItemInstance.LockExpiration, Unix seconds until which the item cannot be used (a mount that died); 0: not locked.';
