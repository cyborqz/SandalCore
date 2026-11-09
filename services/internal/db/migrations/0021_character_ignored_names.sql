-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
-- Social, step 6, "the ignore list": the players and the guilds a character ignores.
--
-- One NEW table, character_ignored_names: one row per name a character ignores, of one kind (a player or a guild), with its place in the character's list of that kind so that the order the
-- names were added in is kept. The game client shows both lists and hides the chat lines of every name on them by itself; worldd keeps the lists while a character plays, sends them to the
-- client and saves them with the progress (CharacterService.SaveProgress with replace_ignored_names).
--
-- Nothing is backfilled here: a character that ignores nobody has no row.
--
-- The version is 0021 and 0020 is left free: the runner (internal/db/db.go) applies every version it has not recorded, in version order, and allows gaps, so a migration numbered 0020 that comes
-- later still applies, to a database that holds 0021 as well as to a new one. Two files with the same version are refused when the services start.
--
-- This ADDS ONE TABLE AND ONLY WRITES TO IT: no existing table, row, column or constraint is touched, nothing is dropped. Rolling it back is a documented block in
-- services/README.md ("Rolling back migration 0021").

SET LOCAL lock_timeout = '10s';

CREATE TABLE character_ignored_names (
    character_id TEXT    NOT NULL REFERENCES characters (id) ON DELETE CASCADE,
    kind         TEXT    NOT NULL
                 CONSTRAINT character_ignored_names_kind CHECK (kind IN ('character', 'guild')),
    name         TEXT    NOT NULL
                 CONSTRAINT character_ignored_names_name CHECK (name <> '' AND name = btrim(name) AND length(name) <= 64),
    position     INTEGER NOT NULL
                 CONSTRAINT character_ignored_names_position CHECK (position >= 0),
    CONSTRAINT character_ignored_names_pkey PRIMARY KEY (character_id, kind, name),
    CONSTRAINT character_ignored_names_order UNIQUE (character_id, kind, position)
);

COMMENT ON TABLE character_ignored_names IS 'Social, step 6: the players and the guilds a character ignores, each list in the order the names were added. worldd saves them with the progress (SaveProgress).';
COMMENT ON COLUMN character_ignored_names.kind IS 'character: a player''s name; guild: a guild''s name. Each kind is a list of its own.';
COMMENT ON COLUMN character_ignored_names.name IS 'The name as the player typed it (the game client compares names without regard to the case of letters); never empty, no spaces at either end, at most 64 characters.';
COMMENT ON COLUMN character_ignored_names.position IS 'The place in the character''s list of that kind, from 0.';
