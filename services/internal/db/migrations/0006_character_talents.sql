-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
-- Plan D1, stage 2: a character's talents and action bar.
--
-- character_skill_ranks holds the ranks a character bought, per skill tree (the design database's SkillTree Guid), in the order it
-- bought them (ord: the tree's list in the XPTracker is the ranks a level grants by itself and then these, so the order is part of what a
-- relog must give back). The ranks a level grants by itself are not stored. The ranks are kept per tree, so a character that switches
-- class (a GM class scroll) and back finds the class's ranks again. A rank is in a tree once.
--
-- character_action_bar holds the character's action bar: one row per slot (bank 0-19, slot 0-15), FActionbarSlotUseData's members. A
-- character with no rows has never arranged its bar and gets the default.
--
-- Nothing in the existing tables is changed; the whole file runs in the migration's own transaction. Rolling it back is a documented
-- block in services/README.md ("Rolling back migration 0006").

SET LOCAL lock_timeout = '10s';

CREATE TABLE character_skill_ranks (
    character_id TEXT   NOT NULL REFERENCES characters (id) ON DELETE CASCADE,
    tree_guid    BIGINT NOT NULL
                 CONSTRAINT character_skill_ranks_tree_positive CHECK (tree_guid > 0),
    ord          INT    NOT NULL
                 CONSTRAINT character_skill_ranks_ord CHECK (ord >= 0),
    rank_guid    BIGINT NOT NULL
                 CONSTRAINT character_skill_ranks_rank_positive CHECK (rank_guid > 0),
    CONSTRAINT character_skill_ranks_pkey PRIMARY KEY (character_id, tree_guid, ord),
    CONSTRAINT character_skill_ranks_once UNIQUE (character_id, tree_guid, rank_guid)
);

COMMENT ON TABLE character_skill_ranks IS 'Plan D1 stage 2: the skill ranks a character bought, per tree in purchase order. worldd saves them (SaveProgress).';

CREATE TABLE character_action_bar (
    character_id   TEXT     NOT NULL REFERENCES characters (id) ON DELETE CASCADE,
    bank           SMALLINT NOT NULL
                   CONSTRAINT character_action_bar_bank CHECK (bank BETWEEN 0 AND 19),
    slot           SMALLINT NOT NULL
                   CONSTRAINT character_action_bar_slot CHECK (slot BETWEEN 0 AND 15),
    use_type       SMALLINT NOT NULL
                   CONSTRAINT character_action_bar_use_type CHECK (use_type BETWEEN 1 AND 9),
    instigator     SMALLINT NOT NULL DEFAULT 0
                   CONSTRAINT character_action_bar_instigator CHECK (instigator BETWEEN 0 AND 3),
    ability_id     BIGINT   NOT NULL DEFAULT 0,
    skill_rank_id  BIGINT   NOT NULL DEFAULT 0,
    pet_command_id BIGINT   NOT NULL DEFAULT 0,
    item_record_id BIGINT   NOT NULL DEFAULT 0,
    item_guid      TEXT     NOT NULL DEFAULT ''
                   CONSTRAINT character_action_bar_item_guid CHECK (item_guid = '' OR item_guid ~ '^[0-9a-f]{32}$'),
    item_rarity    SMALLINT NOT NULL DEFAULT 0
                   CONSTRAINT character_action_bar_rarity CHECK (item_rarity BETWEEN 0 AND 15),
    item_quality   INT      NOT NULL DEFAULT 0
                   CONSTRAINT character_action_bar_quality CHECK (item_quality >= 0),
    combo_button   SMALLINT NOT NULL DEFAULT 0
                   CONSTRAINT character_action_bar_combo CHECK (combo_button BETWEEN 0 AND 15),
    CONSTRAINT character_action_bar_pkey PRIMARY KEY (character_id, bank, slot)
);

COMMENT ON TABLE character_action_bar IS 'Plan D1 stage 2: a character''s action bar, one row per slot (FActionbarSlotUseData). worldd saves it (SaveProgress).';
