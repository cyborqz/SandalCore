-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
-- Embersprings, "respawn points a player discovers and attunes to": the places a character discovered and the place it made its home.
--
-- Two NEW tables. character_attunements: one row per place a character discovered (its number in the design database's master list of places, 1 to 255), with its place in the
-- character's list so that the order of discovery is kept. character_home_attunement: the one place the character made its home and when the home respawn was last used (Unix
-- milliseconds on the server's wall clock; the home respawn waits two hours after its last use). worldd keeps both while a character plays, sends them to the client at login and
-- whenever they change, and saves them with the progress (CharacterService.SaveProgress with replace_attunements).
--
-- Nothing is backfilled here: a character that discovered nothing has no row.
--
-- This ADDS TWO TABLES AND ONLY WRITES TO THEM: no existing table, row, column or constraint is touched, nothing is dropped. Rolling it back is a documented block in
-- services/README.md ("Rolling back migration 0018").

SET LOCAL lock_timeout = '10s';

CREATE TABLE character_attunements (
    character_id TEXT    NOT NULL REFERENCES characters (id) ON DELETE CASCADE,
    attune_index INTEGER NOT NULL
                 CONSTRAINT character_attunements_index CHECK (attune_index BETWEEN 1 AND 255),
    position     INTEGER NOT NULL
                 CONSTRAINT character_attunements_position CHECK (position >= 0),
    CONSTRAINT character_attunements_pkey PRIMARY KEY (character_id, attune_index),
    CONSTRAINT character_attunements_order UNIQUE (character_id, position)
);

CREATE TABLE character_home_attunement (
    character_id TEXT    NOT NULL PRIMARY KEY REFERENCES characters (id) ON DELETE CASCADE,
    attune_index INTEGER NOT NULL
                 CONSTRAINT character_home_attunement_index CHECK (attune_index BETWEEN 0 AND 255),
    last_used_ms BIGINT  NOT NULL
                 CONSTRAINT character_home_attunement_used CHECK (last_used_ms >= 0)
);

COMMENT ON TABLE character_attunements IS 'Embersprings: the places a character discovered, in the order it discovered them. worldd saves them with the progress (SaveProgress).';
COMMENT ON COLUMN character_attunements.attune_index IS 'The place''s number in the design database''s master list of places (1 to 255).';
COMMENT ON COLUMN character_attunements.position IS 'The place in the character''s list, from 0.';
COMMENT ON TABLE character_home_attunement IS 'Embersprings: the place a character made its home (0: none) and when its home respawn was last used.';
COMMENT ON COLUMN character_home_attunement.last_used_ms IS 'Unix milliseconds on the server''s wall clock; 0 for never.';
