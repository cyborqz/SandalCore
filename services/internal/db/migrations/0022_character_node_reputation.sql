-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
-- Node reputation: what a character stands with each city node.
--
-- One NEW table, character_node_reputation: one row per character and node the character has reputation with, as the game keeps it (UOwnerInfoComponent.NodeToPlayerReputation:
-- the node, the reputation and when it last decayed). worldd keeps the character's rows while it plays and saves them with the progress (CharacterService.SaveProgress with
-- replace_node_reputation); GetCharacter returns them with the items. A row of 0 reputation is not kept.
--
-- Nothing is backfilled here: the reputation worldd kept in the purse before this (the NodeToPlayerReputation currency in character_wallet) is moved to the character's citizen node by
-- worldd itself, the first time the character plays with this build.
--
-- This ADDS ONE TABLE AND ONLY WRITES TO IT: no existing table, row, column or constraint is touched, nothing is dropped. An older character service ignores it (the
-- reputations stay in the table). Rolling it back: services/README.md ("Rolling back migration 0022").

SET LOCAL lock_timeout = '10s';

CREATE TABLE character_node_reputation (
    character_id  TEXT             NOT NULL REFERENCES characters (id) ON DELETE CASCADE,
    node_guid     BIGINT           NOT NULL
                                   CONSTRAINT character_node_reputation_node CHECK (node_guid <> 0),
    reputation    DOUBLE PRECISION NOT NULL
                                   CONSTRAINT character_node_reputation_range CHECK (reputation > -1000000000 AND reputation < 1000000000),
    last_decay_ms BIGINT           NOT NULL DEFAULT 0
                                   CONSTRAINT character_node_reputation_decay CHECK (last_decay_ms >= 0),
    updated_at    TIMESTAMPTZ      NOT NULL DEFAULT now(),
    PRIMARY KEY (character_id, node_guid)
);

COMMENT ON TABLE character_node_reputation IS 'A character''s reputation with a city node (UOwnerInfoComponent.NodeToPlayerReputation). worldd saves it with the progress (SaveProgress).';
COMMENT ON COLUMN character_node_reputation.node_guid IS 'The city node record (CityNodeRecordBase guid).';
COMMENT ON COLUMN character_node_reputation.reputation IS 'The reputation, within the node''s MinReputation and MaxReputation (city_node in content.db).';
COMMENT ON COLUMN character_node_reputation.last_decay_ms IS 'When the reputation last decayed (Unix milliseconds); 0 for never.';
