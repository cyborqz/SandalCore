-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
-- Quests, slice 1, "take a quest": the quest service's tables.
--
-- FOUR NEW TABLES, all of them keyed to a character and removed with it:
--   character_quests            the quests a character took and has not finished: the quest's design record, the instance guid the client knows it by (stable for as long as the
--                               character has the quest), its status, when it started (the world's seconds), the NPC that gave it and its objectives (JSONB: the objective guids and the
--                               states the client reads), in the order the quests were taken
--   character_quests_completed  how often a character finished each quest
--   character_quest_variables   the character's quest variables (named values the quests' rules read)
--   character_quest_commands    the inbox of GM commands for a character (questctl puts them in, worldd takes them out and does them)
--
-- Nothing is backfilled: a character that took no quest has no row. This ADDS FOUR TABLES AND ONLY WRITES TO THEM: no existing table, row, column or constraint is touched, nothing is
-- dropped. Rolling it back is a documented block in services/README.md ("Rolling back migration 0016").

SET LOCAL lock_timeout = '10s';

CREATE TABLE character_quests (
    character_id  TEXT             NOT NULL REFERENCES characters (id) ON DELETE CASCADE,
    instance_guid TEXT             NOT NULL
                  CONSTRAINT character_quests_guid_hex CHECK (instance_guid ~ '^[0-9a-f]{32}$' AND instance_guid <> repeat('0', 32)),
    record_id     BIGINT           NOT NULL
                  CONSTRAINT character_quests_record_positive CHECK (record_id > 0),
    status        SMALLINT         NOT NULL DEFAULT 0
                  CONSTRAINT character_quests_status CHECK (status BETWEEN 0 AND 3),
    start_time    DOUBLE PRECISION NOT NULL DEFAULT 0
                  CONSTRAINT character_quests_start_time CHECK (start_time >= 0),
    source_guid   BIGINT           NOT NULL DEFAULT 0
                  CONSTRAINT character_quests_source CHECK (source_guid >= 0),
    position      INTEGER          NOT NULL
                  CONSTRAINT character_quests_position CHECK (position >= 0),
    objectives    JSONB            NOT NULL DEFAULT '[]'::jsonb
                  CONSTRAINT character_quests_objectives CHECK (jsonb_typeof(objectives) = 'array'),
    CONSTRAINT character_quests_pkey PRIMARY KEY (character_id, instance_guid),
    CONSTRAINT character_quests_record_unique UNIQUE (character_id, record_id),
    CONSTRAINT character_quests_order UNIQUE (character_id, position)
);

CREATE TABLE character_quests_completed (
    character_id TEXT    NOT NULL REFERENCES characters (id) ON DELETE CASCADE,
    record_id    BIGINT  NOT NULL
                 CONSTRAINT character_quests_completed_record_positive CHECK (record_id > 0),
    completions  INTEGER NOT NULL
                 CONSTRAINT character_quests_completed_count CHECK (completions > 0),
    CONSTRAINT character_quests_completed_pkey PRIMARY KEY (character_id, record_id)
);

CREATE TABLE character_quest_variables (
    character_id TEXT   NOT NULL REFERENCES characters (id) ON DELETE CASCADE,
    name         TEXT   NOT NULL
                 CONSTRAINT character_quest_variables_name CHECK (name <> '' AND length(name) <= 128),
    value        BIGINT NOT NULL,
    CONSTRAINT character_quest_variables_pkey PRIMARY KEY (character_id, name)
);

CREATE TABLE character_quest_commands (
    id           BIGSERIAL   PRIMARY KEY,
    character_id TEXT        NOT NULL REFERENCES characters (id) ON DELETE CASCADE,
    kind         TEXT        NOT NULL
                 CONSTRAINT character_quest_commands_kind CHECK (kind IN ('give', 'finish', 'fail', 'abandon', 'clear', 'set-var')),
    quest        TEXT        NOT NULL DEFAULT '',
    name         TEXT        NOT NULL DEFAULT ''
                 CONSTRAINT character_quest_commands_name CHECK (length(name) <= 128),
    value        BIGINT      NOT NULL DEFAULT 0,
    created_at   TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX character_quest_commands_character ON character_quest_commands (character_id, id);

COMMENT ON TABLE character_quests IS 'Quests slice 1: the quests a character took and has not finished, in the order they were taken. worldd saves the whole log whenever it changes.';
COMMENT ON COLUMN character_quests.instance_guid IS 'The four words of the FGuid the client knows the quest instance by, 32 lower-case hex digits.';
COMMENT ON COLUMN character_quests.objectives IS 'A JSON array: per objective its guid, its record and the states the client reads (active, awake, hidden, completed, succeeded, failed, count).';
COMMENT ON TABLE character_quests_completed IS 'Quests slice 1: how often a character finished each quest.';
COMMENT ON TABLE character_quest_variables IS 'Quests slice 1: a character''s quest variables.';
COMMENT ON TABLE character_quest_commands IS 'Quests slice 1: GM commands waiting for a character (questctl puts them in, worldd takes them out).';
