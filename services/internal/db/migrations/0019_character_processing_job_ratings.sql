-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
-- Crafting quality: a processing job keeps the artisan ratings its crafter had when it was started.
--
-- A job's extra units, the units that come out a rarity higher and its experience are worked out from the crafter's quantity, rarity and experience ratings. The ratings the job was
-- started with are what counts, whatever the crafter wears when the job is collected, so they are stored with the job: three NEW columns of character_processing_jobs, each the sum
-- the formula adds up (the general rating and the profession's own) as it stood when the job was queued. worldd saves them with the job (CharacterService.SaveInventory with
-- replace_processing_jobs) and reads them back at login.
--
-- A job saved before this migration has none: the columns read 0, which is what its crafter had, since no job was worked out from ratings before (the job is paid at the base tables).
--
-- This ADDS COLUMNS ONLY, all of them with a default of 0: no row is rewritten with a value of its own, no constraint on the existing columns changes and nothing is dropped. The whole file
-- runs in the migration's own transaction. Rolling it back is a documented block in services/README.md ("Rolling back migration 0019").

SET LOCAL lock_timeout = '10s';

ALTER TABLE character_processing_jobs
    ADD COLUMN rating_xp       DOUBLE PRECISION NOT NULL DEFAULT 0,
    ADD COLUMN rating_quantity DOUBLE PRECISION NOT NULL DEFAULT 0,
    ADD COLUMN rating_rarity   DOUBLE PRECISION NOT NULL DEFAULT 0;

ALTER TABLE character_processing_jobs
    ADD CONSTRAINT character_processing_jobs_rating_xp       CHECK (rating_xp       >= 0 AND rating_xp       <= 1e9),   -- NaN is above 1e9 in PostgreSQL
    ADD CONSTRAINT character_processing_jobs_rating_quantity CHECK (rating_quantity >= 0 AND rating_quantity <= 1e9),
    ADD CONSTRAINT character_processing_jobs_rating_rarity   CHECK (rating_rarity   >= 0 AND rating_rarity   <= 1e9);

COMMENT ON COLUMN character_processing_jobs.rating_xp IS 'Crafting quality: the crafter''s processing experience rating (the general one plus the profession''s own) when the job was queued. 0: none.';
COMMENT ON COLUMN character_processing_jobs.rating_quantity IS 'Crafting quality: the crafter''s processing quantity rating when the job was queued (the chance of one more unit). 0: none.';
COMMENT ON COLUMN character_processing_jobs.rating_rarity IS 'Crafting quality: the crafter''s processing rarity rating when the job was queued (the chance of a unit coming out one rarity higher). 0: none.';
