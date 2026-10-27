// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package content

import (
	"database/sql"
	"fmt"
)

// Quests (quests milestone, slice 1, "take a quest"; contentc version 21): what a character needs to take, show and abandon a quest.
//
//	quest             one quest: its kind and level, whether it can be abandoned or taken again, the rule that makes it available (activation: a requirement as in
//	                  dialogue.go, empty when it has none), what it does when it starts (start_actions: JSON list, below) and the reward tables it pays (rewards: JSON list of guids)
//	quest_objective   the objectives of a quest in the quest's own order (idx), with the two flags that say how each starts (default_awake, default_hidden), how often it
//	                  has to happen (completion_count) and whether finishing it finishes the quest
//	npc_offered_quest the quests an NPC offers (source_kind 'instance' or 'asset', as in npc_dialogue): the node its talk starts at while the quest is offered
//	dialogue_accept   the nodes that offer a quest: the quest, the node the talk goes on at after the player takes it and the objectives the node names (JSON list)
//	zoi_quest         the quests a zone of influence starts for a character who stands in it (the zone record's LocationQuests: the starting areas' first quests)
//	item_quest        the quests an item is bound to: it is deleted when the quest is taken out of the log ('delete_all', 'delete_one'), or the quest is taken out of the log when the
//	                  item is deleted ('abandon')
//
// A start action is {"a": kind, "o": objective, "q": quest, "n": variable name, "v": value, "c": 1 when the action has a condition of its own}: only what is set.
const QuestDDL = `
CREATE TABLE quest (
    guid             INTEGER PRIMARY KEY,
    name             TEXT NOT NULL,
    flavor           TEXT NOT NULL,        -- Quest, Commission, Event, Destiny, TreasureMap, HiddenEvent, Module
    locality         TEXT NOT NULL,        -- Local (one character's log) or Global
    level            INTEGER NOT NULL,
    can_abandon      INTEGER NOT NULL,     -- bCanAbandon
    repeatable       INTEGER NOT NULL,     -- bRepeatable
    disabled         INTEGER NOT NULL,     -- bDisabled
    default_activate INTEGER NOT NULL,     -- bDefaultActivate
    activation       TEXT NOT NULL,        -- JSON requirement, '' when the quest has none
    start_actions    TEXT NOT NULL,        -- JSON list of what the quest does when it starts
    rewards          TEXT NOT NULL         -- JSON list of reward table guids
);
CREATE TABLE quest_objective (
    quest_guid       INTEGER NOT NULL,
    idx              INTEGER NOT NULL,     -- the place in the quest's ObjectiveIds
    guid             INTEGER NOT NULL,
    name             TEXT NOT NULL,
    obj_type         TEXT NOT NULL,        -- Custom, SubQuest, Kill, Dialogue, GoTo, Collect, TurnIn, ContainerReward, Interact, Gather, ...
    completion_count INTEGER NOT NULL,
    default_awake    INTEGER NOT NULL,     -- bDefaultAwake
    default_hidden   INTEGER NOT NULL,     -- bDefaultHidden
    completes_quest  INTEGER NOT NULL,     -- bCompletesQuest
    optional         INTEGER NOT NULL,     -- bOptionalObjective
    PRIMARY KEY (quest_guid, idx)
) WITHOUT ROWID;
CREATE TABLE npc_offered_quest (
    source_kind  TEXT NOT NULL,            -- 'instance' (PopulationInstance.OfferedQuests) or 'asset' (PopulationAsset.OfferedQuests)
    source_guid  INTEGER NOT NULL,
    idx          INTEGER NOT NULL,
    quest_guid   INTEGER NOT NULL,
    dynamic_node INTEGER NOT NULL,         -- DynamicNodeId: the node the talk starts at while the quest is offered (0: none)
    PRIMARY KEY (source_kind, source_guid, idx)
) WITHOUT ROWID;
CREATE TABLE dialogue_accept (
    node_guid  INTEGER PRIMARY KEY,
    quest_guid INTEGER NOT NULL,
    on_success INTEGER NOT NULL,           -- OnSuccessNode: where the talk goes on after the quest is taken (0: it ends)
    objectives TEXT NOT NULL               -- JSON list of the node's ObjectiveIds
);
CREATE TABLE zoi_quest (
    zoi_guid   INTEGER NOT NULL,
    idx        INTEGER NOT NULL,
    quest_guid INTEGER NOT NULL,
    PRIMARY KEY (zoi_guid, idx)
) WITHOUT ROWID;
CREATE TABLE item_quest (
    item_guid  INTEGER NOT NULL,
    quest_guid INTEGER NOT NULL,
    kind       TEXT NOT NULL,              -- delete_all, delete_one (DeleteOnQuestsRemovedData's type) or abandon (AbandonQuestsOnDelete)
    PRIMARY KEY (item_guid, quest_guid, kind)
) WITHOUT ROWID;
`

// QuestTables are the tables QuestDDL creates, in the DDL's order.
var QuestTables = []string{"quest", "quest_objective", "npc_offered_quest", "dialogue_accept", "zoi_quest", "item_quest"}

func createQuests(db *sql.DB) error {
	if _, err := db.Exec(QuestDDL); err != nil {
		return fmt.Errorf("content: create the quest tables: %w", err)
	}
	return nil
}
