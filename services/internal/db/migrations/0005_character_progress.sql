-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
-- Plan D1: a character's progress: its experience per leveling type and the one-time grant of the GM starter set.
--
-- character_experience holds, per character and leveling type (the design database's LevelingTypeDef Guid: Adventuring
-- 108651085518458, the weapon lines ...), the XP total as the client's XPTracker replicates it (ExperienceFastMap) and the
-- experience debt (ExperienceDebtFastMap). The level itself stays in characters.level (it is what the character list shows
-- and what worldd joins with); worldd keeps it consistent with the Adventuring row when it saves (SaveProgress). A character
-- with no row has the XP of its level's threshold and no debt.
--
-- characters.gm_starter_set_granted_at says when the character service put the GM starter set (the level scrolls, the class
-- scrolls and a gear scroll) into the character's bag: NULL until it did. The grant runs only with AOC_GM_STARTER_SET=1 and
-- happens once per character (at creation, or at a start of the service for the characters made before).
--
-- Nothing in the existing tables is changed but for that one nullable column; the whole file runs in the migration's own
-- transaction. Rolling it back is a documented block in services/README.md ("Rolling back migration 0005").

SET LOCAL lock_timeout = '10s';

CREATE TABLE character_experience (
    character_id       TEXT        NOT NULL REFERENCES characters (id) ON DELETE CASCADE,
    leveling_type_guid BIGINT      NOT NULL
                       CONSTRAINT character_experience_type_positive CHECK (leveling_type_guid > 0),
    xp                 BIGINT      NOT NULL DEFAULT 0
                       CONSTRAINT character_experience_xp CHECK (xp >= 0),
    debt               BIGINT      NOT NULL DEFAULT 0
                       CONSTRAINT character_experience_debt CHECK (debt >= 0),
    updated_at         TIMESTAMPTZ NOT NULL DEFAULT now(),
    CONSTRAINT character_experience_pkey PRIMARY KEY (character_id, leveling_type_guid)
);

COMMENT ON TABLE character_experience IS 'Plan D1: a character''s XP and XP debt per leveling type (the LevelingTypeDef Guid). worldd saves them (SaveProgress).';

ALTER TABLE characters ADD COLUMN gm_starter_set_granted_at TIMESTAMPTZ;

COMMENT ON COLUMN characters.gm_starter_set_granted_at IS 'Plan D1: when the GM starter set was put into the bag (AOC_GM_STARTER_SET=1); NULL until then.';
