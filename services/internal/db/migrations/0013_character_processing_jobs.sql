-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
-- The crafting milestone, step 1, "start a processing job": the processing jobs a character has started at the crafting stations.
--
-- One NEW table, character_processing_jobs: one row per job (a batch of a recipe started at a station), in the order the player queued them. A job runs on the wall clock: it began at
-- started_at (queued_at, or the end of the job before it when it had to wait) and is done duration_ms later, whether its player is logged in or not. worldd keeps the jobs while a
-- character plays and saves them with the items and the wallet in the same call (CharacterService.SaveInventory with replace_processing_jobs), so the materials and the coin a job took and
-- the job itself are stored together or not at all.
--
-- station_key names the station the job is at: 16 hex digits made from the station's place in the world's data, the same on every start of the server. resources is what the player paid,
-- a JSON array of {record, rarities: [{rarity, count, warehouse}]}, which the station window shows the job back with.
--
-- Nothing is backfilled here: a character with no job has no row.
--
-- This ADDS ONE TABLE AND ONLY WRITES TO IT: no existing table, row, column or constraint is touched, nothing is dropped. Rolling it back is a documented block in
-- services/README.md ("Rolling back migration 0013").

SET LOCAL lock_timeout = '10s';

CREATE TABLE character_processing_jobs (
    character_id    TEXT        NOT NULL REFERENCES characters (id) ON DELETE CASCADE,
    job_guid        TEXT        NOT NULL
                    CONSTRAINT character_processing_jobs_guid CHECK (job_guid ~ '^[0-9a-f]{32}$' AND job_guid <> '00000000000000000000000000000000'),
    ordinal         INTEGER     NOT NULL
                    CONSTRAINT character_processing_jobs_ordinal CHECK (ordinal >= 0),
    station_key     TEXT        NOT NULL
                    CONSTRAINT character_processing_jobs_station CHECK (station_key ~ '^[0-9a-f]{16}$'),
    recipe_guid     BIGINT      NOT NULL
                    CONSTRAINT character_processing_jobs_recipe CHECK (recipe_guid > 0),
    sub_recipe_guid BIGINT      NOT NULL DEFAULT 0
                    CONSTRAINT character_processing_jobs_sub_recipe CHECK (sub_recipe_guid >= 0),
    quantity        INTEGER     NOT NULL
                    CONSTRAINT character_processing_jobs_quantity CHECK (quantity BETWEEN 1 AND 250),
    queued_at       TIMESTAMPTZ NOT NULL,
    started_at      TIMESTAMPTZ NOT NULL,
    duration_ms     BIGINT      NOT NULL
                    CONSTRAINT character_processing_jobs_duration CHECK (duration_ms BETWEEN 1 AND 10000000000),
    client_txn      TEXT        NOT NULL
                    CONSTRAINT character_processing_jobs_txn CHECK (client_txn ~ '^[0-9a-f]{32}$'),
    resources       JSONB       NOT NULL DEFAULT '[]'::jsonb
                    CONSTRAINT character_processing_jobs_resources CHECK (jsonb_typeof(resources) = 'array'),
    updated_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    CONSTRAINT character_processing_jobs_pkey PRIMARY KEY (character_id, job_guid)
);

CREATE INDEX character_processing_jobs_order ON character_processing_jobs (character_id, ordinal);

COMMENT ON TABLE character_processing_jobs IS 'The crafting milestone, step 1: the processing jobs of a character, in the order they were queued. worldd saves them with the items and the wallet (SaveInventory).';
COMMENT ON COLUMN character_processing_jobs.station_key IS '16 lowercase hex digits: the station the job is at, the same on every start of the server.';
COMMENT ON COLUMN character_processing_jobs.started_at IS 'When the job began to run on the wall clock: queued_at, or the end of the job before it when it waited. It is done duration_ms later.';
COMMENT ON COLUMN character_processing_jobs.resources IS 'What the player paid: a JSON array of {record, rarities: [{rarity, count, warehouse}]}.';
