-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
-- PvP: what a character carries from one session to the next.
--
-- One NEW table, character_pvp: one row per character that has any, as the game keeps it on the character's stats (GameStatProfile.CorruptionId: the Corruption stat, a float
-- every client is told; CorruptionSoulburnId: Corruption_Blight, the owner's, which grows with every player killed and adds to the corruption of the next). worldd keeps them while
-- the character plays and saves them with the progress (CharacterService.SaveProgress with replace_pvp); GetCharacter returns them with the items. A character with none of either
-- keeps no row.
--
-- This ADDS ONE TABLE AND ONLY WRITES TO IT: no existing table, row, column or constraint is touched, nothing is dropped. An older character service ignores it (the rows stay in the
-- table). Rolling it back: services/README.md ("Rolling back migration 0025").

SET LOCAL lock_timeout = '10s';

CREATE TABLE character_pvp (
    character_id TEXT             PRIMARY KEY REFERENCES characters (id) ON DELETE CASCADE,
    corruption   DOUBLE PRECISION NOT NULL DEFAULT 0
                                  CONSTRAINT character_pvp_corruption CHECK (corruption >= 0 AND corruption < 1000000),
    blight       DOUBLE PRECISION NOT NULL DEFAULT 0
                                  CONSTRAINT character_pvp_blight CHECK (blight >= 0 AND blight < 1000000),
    updated_at   TIMESTAMPTZ      NOT NULL DEFAULT now()
);

COMMENT ON TABLE character_pvp IS 'A character''s PvP state between sessions: Corruption and Corruption_Blight (GameStatProfile). worldd saves it with the progress (SaveProgress).';
