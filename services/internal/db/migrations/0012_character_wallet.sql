-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
-- The vendors milestone, step 0, "your purse and the general merchant": the money a character carries.
--
-- One NEW table, character_wallet: per character and currency (the BASE currency def's Guid: Copper 6064629132706054144, Node currency 6064629879350493185), the amount in
-- that unit (copper for money: 100 copper = 1 silver, 100 silver = 1 gold). A currency with no row reads as 0. worldd keeps the wallet while a character plays and saves it
-- with the items in the same call (CharacterService.SaveInventory with replace_wallet), so a purchase and the items it bought are stored together or not at all.
--
-- Nothing is backfilled here: a character with no wallet row is given the start purse ONCE, by the character service, the first time its wallet is read (GetCharacter with
-- include_items). The rows then exist for good, so the purse never comes twice, even when the character spends everything.
--
-- The amount is a non-negative BIGINT of at most 10^13 (the largest cap of any currency; worldd holds the smaller cap of each, 10,000,000 copper for money).
--
-- This ADDS ONE TABLE AND ONLY WRITES TO IT: no existing table, row, column or constraint is touched, nothing is dropped. Rolling it back is a documented block in
-- services/README.md ("Rolling back migration 0012").

SET LOCAL lock_timeout = '10s';

CREATE TABLE character_wallet (
    character_id  TEXT        NOT NULL REFERENCES characters (id) ON DELETE CASCADE,
    currency_guid BIGINT      NOT NULL
                  CONSTRAINT character_wallet_guid_positive CHECK (currency_guid > 0),
    amount        BIGINT      NOT NULL DEFAULT 0
                  CONSTRAINT character_wallet_amount CHECK (amount >= 0 AND amount <= 10000000000000),
    updated_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    CONSTRAINT character_wallet_pkey PRIMARY KEY (character_id, currency_guid)
);

COMMENT ON TABLE character_wallet IS 'The vendors milestone, step 0: the money of a character, one row per currency (the base currency def''s Guid), in the base unit (copper for money). worldd saves it with the items (SaveInventory).';
COMMENT ON COLUMN character_wallet.currency_guid IS 'The design database''s CurrencyDef Guid of the BASE unit (Copper 6064629132706054144, Node currency 6064629879350493185).';
COMMENT ON COLUMN character_wallet.amount IS 'How much the character holds, in the currency''s base unit; 0 to 10^13.';
