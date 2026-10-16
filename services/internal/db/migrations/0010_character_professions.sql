-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
-- Gathering stage 2: the professions of a character.
--
-- One NEW table, character_professions: per character and profession (the design database's CharacterProfessions Guid: Mining 108752769996425, Lumberjacking
-- 108752769996426 ...), the certification tier (0 Novice, 1 Apprentice, 2 Journeyman, 3 Master, 4 GrandMaster), the level (1 to 50) and the XP total. The XP lives ONLY here
-- (nothing of a profession is in character_experience). A character with no row for a profession reads as Novice, level 1, XP 0.
--
-- Every character holds Novice certification in all 22 CharacterProfessions (the user's rule of 2026-10-09: the data's Novice tier has no MaximumAllowable), so this migration
-- BACKFILLS 22 rows (Novice, level 1, XP 0) for every character that exists, and the character service inserts the 22 in the transaction that creates a character.
-- The 22 guids below are content.db's `profession` table (a test holds the two together).
--
-- This ADDS ONE TABLE AND ONLY WRITES TO IT: no existing table, row, column or constraint is touched, nothing is dropped. Rolling it back is a documented block in
-- services/README.md ("Rolling back migration 0010").

SET LOCAL lock_timeout = '10s';

CREATE TABLE character_professions (
    character_id       TEXT        NOT NULL REFERENCES characters (id) ON DELETE CASCADE,
    profession_guid    BIGINT      NOT NULL
                       CONSTRAINT character_professions_guid_positive CHECK (profession_guid > 0),
    certification_tier SMALLINT    NOT NULL DEFAULT 0
                       CONSTRAINT character_professions_tier CHECK (certification_tier BETWEEN 0 AND 4),
    level              SMALLINT    NOT NULL DEFAULT 1
                       CONSTRAINT character_professions_level CHECK (level BETWEEN 1 AND 50),
    xp                 BIGINT      NOT NULL DEFAULT 0
                       CONSTRAINT character_professions_xp CHECK (xp >= 0),
    updated_at         TIMESTAMPTZ NOT NULL DEFAULT now(),
    CONSTRAINT character_professions_pkey PRIMARY KEY (character_id, profession_guid)
);

COMMENT ON TABLE character_professions IS 'Gathering stage 2: a character''s certification tier (0 Novice ... 4 GrandMaster), level (1-50) and XP per profession (the CharacterProfessions Guid). worldd saves them (SaveProgress).';

INSERT INTO character_professions (character_id, profession_guid)
SELECT c.id, p.guid
FROM characters c
CROSS JOIN (VALUES
    (108752746862200::bigint), -- Weaponsmithing
    (108752746927737),         -- Carpentry
    (108752746927738),         -- ArcaneEngineering
    (108752746927739),         -- Armorsmithing
    (108752746927740),         -- Leatherworking
    (108752746993277),         -- Tailoring
    (108752746993278),         -- Jeweler
    (108752746993279),         -- Scribe
    (108752756889216),         -- Metalworking
    (108752756889217),         -- Stonemasonry
    (108752756889218),         -- Weaving
    (108752756954755),         -- Tanning
    (108752756954756),         -- LumberMilling
    (108752756954757),         -- Farming
    (108752756954758),         -- AnimalHusbandry
    (108752757020295),         -- Alchemy
    (108752757020296),         -- Cooking
    (108752769996425),         -- Mining
    (108752769996426),         -- Lumberjacking
    (108752770061963),         -- Herbalism
    (108752770061964),         -- Fishing
    (108752770061965)          -- Hunting
) AS p (guid);
