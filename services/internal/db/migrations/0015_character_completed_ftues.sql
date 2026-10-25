-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
-- Missing systems, step 0, "tutorials stay done": the tutorials a character finished.
--
-- One NEW table, character_completed_ftues: one row per tutorial a character finished (the design database's FirstTimeEvent Guid), with its place in the character's list. The client says
-- a tutorial is finished and shows the hint again at every matching event until the server's list holds it; worldd keeps the list while a character plays, sends it to the client at login
-- and whenever it grows, and saves it with the progress (CharacterService.SaveProgress with replace_completed_ftues). The place keeps the order the tutorials were finished in.
--
-- Nothing is backfilled here: a character that finished no tutorial has no row.
--
-- This ADDS ONE TABLE AND ONLY WRITES TO IT: no existing table, row, column or constraint is touched, nothing is dropped. Rolling it back is a documented block in
-- services/README.md ("Rolling back migration 0015").

SET LOCAL lock_timeout = '10s';

CREATE TABLE character_completed_ftues (
    character_id TEXT    NOT NULL REFERENCES characters (id) ON DELETE CASCADE,
    ftue_guid    BIGINT  NOT NULL
                 CONSTRAINT character_completed_ftues_guid CHECK (ftue_guid > 0),
    position     INTEGER NOT NULL
                 CONSTRAINT character_completed_ftues_position CHECK (position >= 0),
    CONSTRAINT character_completed_ftues_pkey PRIMARY KEY (character_id, ftue_guid),
    CONSTRAINT character_completed_ftues_order UNIQUE (character_id, position)
);

COMMENT ON TABLE character_completed_ftues IS 'Missing systems, step 0: the tutorials a character finished, in the order they were finished. worldd saves them with the progress (SaveProgress).';
COMMENT ON COLUMN character_completed_ftues.ftue_guid IS 'The design database''s FirstTimeEvent Guid.';
COMMENT ON COLUMN character_completed_ftues.position IS 'The place in the character''s list, from 0.';
