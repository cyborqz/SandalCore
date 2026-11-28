-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
-- Player trading: idempotency and audit ledger for atomic two-character swaps.
--
-- ONE NEW TABLE: character_trades. Each committed player-to-player trade inserts one row keyed by its
-- 32-hex trade_id in the same database transaction that replaces both characters' bags, wallets, jobs,
-- recipes and warehouses. Replaying the same trade_id for the same pair returns replayed without
-- rewriting inventories; reusing a trade_id for a different pair is rejected.
--
-- Nothing is backfilled and no existing table, row, column or constraint is touched.

SET LOCAL lock_timeout = '10s';

CREATE TABLE character_trades (
    id              TEXT PRIMARY KEY
                    CONSTRAINT character_trades_id_hex CHECK (id ~ '^[0-9a-f]{32}$' AND id <> repeat('0', 32)),
    char_a_id       TEXT    NOT NULL REFERENCES characters (id) ON DELETE CASCADE,
    char_b_id       TEXT    NOT NULL REFERENCES characters (id) ON DELETE CASCADE,
    copper_a_to_b   BIGINT  NOT NULL DEFAULT 0
                    CONSTRAINT character_trades_copper_a CHECK (copper_a_to_b >= 0 AND copper_a_to_b <= 10000000),
    copper_b_to_a   BIGINT  NOT NULL DEFAULT 0
                    CONSTRAINT character_trades_copper_b CHECK (copper_b_to_a >= 0 AND copper_b_to_a <= 10000000),
    items_a_to_b    INTEGER NOT NULL DEFAULT 0
                    CONSTRAINT character_trades_items_a CHECK (items_a_to_b >= 0),
    items_b_to_a    INTEGER NOT NULL DEFAULT 0
                    CONSTRAINT character_trades_items_b CHECK (items_b_to_a >= 0),
    committed_at_ms BIGINT  NOT NULL
                    CONSTRAINT character_trades_committed_at CHECK (committed_at_ms > 0),
    CONSTRAINT character_trades_distinct_chars CHECK (char_a_id <> char_b_id),
    CONSTRAINT character_trades_non_empty CHECK (copper_a_to_b > 0 OR copper_b_to_a > 0 OR items_a_to_b > 0 OR items_b_to_a > 0)
);

CREATE INDEX character_trades_char_a_idx ON character_trades (char_a_id, committed_at_ms DESC);
CREATE INDEX character_trades_char_b_idx ON character_trades (char_b_id, committed_at_ms DESC);

COMMENT ON TABLE character_trades IS 'Player trading: one row per committed atomic swap, providing idempotency on retry and a transfer ledger.';
COMMENT ON COLUMN character_trades.id IS 'The trade id: 32 lower-case hex digits, not all zero.';
COMMENT ON COLUMN character_trades.char_a_id IS 'First participant character id.';
COMMENT ON COLUMN character_trades.char_b_id IS 'Second participant character id.';
COMMENT ON COLUMN character_trades.copper_a_to_b IS 'Copper transferred from char_a_id to char_b_id (0 to 10,000,000).';
COMMENT ON COLUMN character_trades.copper_b_to_a IS 'Copper transferred from char_b_id to char_a_id (0 to 10,000,000).';
COMMENT ON COLUMN character_trades.items_a_to_b IS 'Number of item stacks transferred from char_a_id to char_b_id.';
COMMENT ON COLUMN character_trades.items_b_to_a IS 'Number of item stacks transferred from char_b_id to char_a_id.';
COMMENT ON COLUMN character_trades.committed_at_ms IS 'Unix milliseconds when the trade was committed.';
