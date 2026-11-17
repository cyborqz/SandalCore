-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
-- Node citizenship: an account is a citizen of one city node.
--
-- One NEW table, account_citizenship: one row per account that ever joined a node, keyed by the account as the game keys citizenship (the citizenship window's
-- FCitizenshipStatus names a primary character and the account's other characters). worldd keeps it while a character of the account plays and saves it with the
-- progress (CharacterService.SaveProgress with replace_citizenship); GetCharacter returns it with the items. node_guid 0 is no citizenship: the row then keeps only when
-- the account may join again (renounce_cooldown_until_ms).
--
-- Nothing is backfilled: an account that never joined a node has no row.
--
-- This ADDS ONE TABLE AND ONLY WRITES TO IT: no existing table, row, column or constraint is touched, nothing is dropped. An older character service ignores it (the
-- citizenship stays in the table). Rolling it back: services/README.md ("Rolling back migration 0020").

SET LOCAL lock_timeout = '10s';

CREATE TABLE account_citizenship (
    account_id                 BIGINT      PRIMARY KEY REFERENCES accounts (id) ON DELETE CASCADE,
    node_guid                  BIGINT      NOT NULL DEFAULT 0,
    primary_character_id       TEXT        REFERENCES characters (id) ON DELETE SET NULL,
    joined_ms                  BIGINT      NOT NULL DEFAULT 0
                               CONSTRAINT account_citizenship_joined CHECK (joined_ms >= 0),
    dues_due_ms                BIGINT      NOT NULL DEFAULT 0
                               CONSTRAINT account_citizenship_dues_due CHECK (dues_due_ms >= 0),
    renounce_cooldown_until_ms BIGINT      NOT NULL DEFAULT 0
                               CONSTRAINT account_citizenship_cooldown CHECK (renounce_cooldown_until_ms >= 0),
    updated_at                 TIMESTAMPTZ NOT NULL DEFAULT now()
);

COMMENT ON TABLE account_citizenship IS 'The city node an account is a citizen of (node_guid 0: none, the row then keeps the renounce cooldown). worldd saves it with the progress (SaveProgress).';
COMMENT ON COLUMN account_citizenship.node_guid IS 'The city node record (CityNodeRecordBase guid) the account is a citizen of; 0 for none.';
COMMENT ON COLUMN account_citizenship.primary_character_id IS 'The character that joined; the account''s other characters are its alternates.';
COMMENT ON COLUMN account_citizenship.dues_due_ms IS 'When the next citizenship dues are due (Unix milliseconds); 0 for none.';
COMMENT ON COLUMN account_citizenship.renounce_cooldown_until_ms IS 'The account cannot join a node again before this (Unix milliseconds); 0 for none.';
