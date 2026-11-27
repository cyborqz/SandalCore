-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
-- Marketplace: one table for the listings a player registers at a Marketplace Manager.
--
-- ONE NEW TABLE. character_market_listings holds one row per active listing: its GUID (the client's key), its market's node, the seller's character (deleting the
-- character removes its listings), the listed stack's own item data (record, instance guid, quantity, quality, durability, creation time, the mount state of
-- migration 0007 and a lock), the unit price in copper, the client's status and when the listing was made and expires (Unix milliseconds). A listing's item is no
-- longer in character_items while the listing is stored: the two are written in one transaction.
--
-- Nothing is backfilled: a character with no listing has no row. worldd reads every LISTED listing once when it starts and inserts one listing together with the
-- seller's bags, wallet, jobs, recipes and warehouse. A listing that has ended keeps its row and is not read again; a later piece that removes ended rows is where
-- they go.
--
-- The version is 0030. 0028 and 0029 stay free for the mail work's later migrations. The runner (internal/db/db.go) skips a version it has recorded, applies
-- every other one in version order and allows gaps, so a later 0028 or 0029 still applies.
--
-- This ADDS ONE TABLE AND ONLY WRITES TO IT: no existing table, row, column or constraint is touched, nothing is dropped. Rolling it back is a documented block
-- in services/README.md ("Rolling back migration 0030").
--
-- The checks keep a row from holding a value the client's struct or the store cannot carry; the store (market.go) repeats them as a backstop.

SET LOCAL lock_timeout = '10s';

CREATE TABLE character_market_listings (
    id               TEXT PRIMARY KEY
                     CONSTRAINT character_market_listings_id_hex CHECK (id ~ '^[0-9a-f]{32}$' AND id <> repeat('0', 32)),
    character_id     TEXT    NOT NULL REFERENCES characters (id) ON DELETE CASCADE,
    market_node      BIGINT  NOT NULL
                     CONSTRAINT character_market_listings_node CHECK (market_node <> 0),
    item_record_id   BIGINT  NOT NULL
                     CONSTRAINT character_market_listings_record_positive CHECK (item_record_id > 0),
    instance_guid    TEXT    NOT NULL
                     CONSTRAINT character_market_listings_guid_hex CHECK (instance_guid ~ '^[0-9a-f]{32}$' AND instance_guid <> repeat('0', 32)),
    quantity         INTEGER NOT NULL
                     CONSTRAINT character_market_listings_quantity CHECK (quantity BETWEEN 1 AND 1000000),
    quality          INTEGER NOT NULL
                     CONSTRAINT character_market_listings_quality CHECK (quality BETWEEN 1 AND 200),
    durability       REAL    NOT NULL DEFAULT 0
                     CONSTRAINT character_market_listings_durability CHECK (durability >= 0 AND durability <= 1e9),   -- NaN is above 1e9 in PostgreSQL
    created_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
    persisted_health REAL,
    persisted_mana   REAL,
    lock_expiration  BIGINT  NOT NULL DEFAULT 0,
    price_per_unit   BIGINT  NOT NULL
                     CONSTRAINT character_market_listings_price CHECK (price_per_unit > 0),
    status           INTEGER NOT NULL DEFAULT 0
                     CONSTRAINT character_market_listings_status CHECK (status >= 0),
    issue_date_ms    BIGINT  NOT NULL
                     CONSTRAINT character_market_listings_issued CHECK (issue_date_ms > 0),
    expiry_date_ms   BIGINT  NOT NULL
                     CONSTRAINT character_market_listings_expires CHECK (expiry_date_ms >= 0),
    CONSTRAINT character_market_listings_persisted_health CHECK (persisted_health IS NULL OR (persisted_health >= 0 AND persisted_health <= 1e9)),
    CONSTRAINT character_market_listings_persisted_mana   CHECK (persisted_mana   IS NULL OR (persisted_mana   >= 0 AND persisted_mana   <= 1e9)),
    CONSTRAINT character_market_listings_lock_expiration  CHECK (lock_expiration >= 0 AND lock_expiration <= 253402300799)   -- the end of the year 9999
);

-- Serves the read of the listed rows and of one market's listed rows, oldest first (status, then market node, then issue date, then id); one seller's listings are not read with it.
CREATE INDEX character_market_listings_listed ON character_market_listings (status, market_node, issue_date_ms, id);

COMMENT ON TABLE character_market_listings IS 'Marketplace: one row per active listing (its GUID, the market node, the seller, the listed stack and its price and dates). worldd reads every LISTED row at the start and inserts one listing with the seller''s bags, wallet and the rest in one transaction.';
COMMENT ON COLUMN character_market_listings.id IS 'The listing''s GUID (the client''s key): 32 lower-case hex digits, not all zero.';
COMMENT ON COLUMN character_market_listings.character_id IS 'The seller. Deleting the character removes its listings.';
COMMENT ON COLUMN character_market_listings.market_node IS 'The city node whose Marketplace holds the listing.';
COMMENT ON COLUMN character_market_listings.item_record_id IS 'The design database''s ItemRecord Guid.';
COMMENT ON COLUMN character_market_listings.instance_guid IS 'The listed item instance''s own GUID: 32 lower-case hex digits, not all zero.';
COMMENT ON COLUMN character_market_listings.quantity IS 'The listed count, 1 to 1000000.';
COMMENT ON COLUMN character_market_listings.quality IS 'The item''s quality, 1 to 200.';
COMMENT ON COLUMN character_market_listings.durability IS 'The item''s durability, 0 to 1e9.';
COMMENT ON COLUMN character_market_listings.created_at IS 'When the listed item was made.';
COMMENT ON COLUMN character_market_listings.persisted_health IS 'The mounts milestone: the Health a mount item persists, NULL for every other item.';
COMMENT ON COLUMN character_market_listings.persisted_mana IS 'The mounts milestone: the Mana a mount item persists, NULL for every other item.';
COMMENT ON COLUMN character_market_listings.lock_expiration IS 'The item''s lock time, Unix seconds (0: not locked).';
COMMENT ON COLUMN character_market_listings.price_per_unit IS 'The unit price in copper.';
COMMENT ON COLUMN character_market_listings.status IS 'The client''s listing status: 0 LISTED. Only a listed row is read.';
COMMENT ON COLUMN character_market_listings.issue_date_ms IS 'When the listing was made, Unix milliseconds.';
COMMENT ON COLUMN character_market_listings.expiry_date_ms IS 'When the listing expires, Unix milliseconds.';
