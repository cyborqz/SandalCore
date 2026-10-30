// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package content

import (
	"database/sql"
	"fmt"
)

// Quests (quests milestone, slice 1, "take a quest"; contentc version 21; slice 2, "do it and hand it in", version 27): what a character needs to take, show, play, hand in and abandon a quest.
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
//	item_crate        (version 27) the crate type of each crate item (ItemRecordBase.CrateId): what a construction deposit counts for a crate objective
//	shrine_icd        (version 27) the Emberspring interactable definitions of the places, found by name (an Interact objective names the definition of the Emberspring to attune to)
//	quest_kill_set    (version 27) the creatures of each population set a Kill objective names (npc_set_member, only for the sets objectives name): a kill counts for the sets its creature belongs to
//
// An action (the lists in start_actions and in the detail columns) is {"a": kind, "o": objective, "q": quest, "n": variable name, "v": value, "c": 1 when the action has a condition of its own,
// "r": that condition (a requirement as in dialogue.go), "rw": the reward table a GrantItem gives, "it": [[item, quantity]] a RemoveItem takes, "t": a Toast's text, "sp": the population sets a
// spawn action names}: only what is set.
//
// The detail columns (version 27) are JSON objects, only what is set:
//
//	quest.detail            {"ok": [actions run when the quest is completed], "fail": [...], "tick": [...], "to": timeout in seconds, "so": 1 when the timeout completes the quest}
//	quest_objective.detail  {"sd": 1 when finishing it shows the completion dialogue, "to": timeout, "so": 1, "ar": its activation rule (a requirement), "rw": [reward tables],
//	                         "on": {"start": [actions], "tick": [...], "ok": [...], "fail": [...]}, "qi": [{"t": reward table, "inst": [...], "sets": [...], "tq": 1}] (quest items the
//	                         listed creatures drop while the objective is active), "d": what the objective's kind needs:
//	                         Kill {"sets": [...], "inst": [...], "elite": n, "adv": 1, "owner": 1, "ctx": 1, "tq": 1 when a tag query is set},
//	                         Dialogue {"src": [{"k": kind, "i": population instance, "n": node, "x": interactable, "it": item}]},
//	                         Interact {"comp": [...], "actor": [...]}, Collect {"cty": type, "items": [...], "tags": [...], "rar": [...]},
//	                         GoTo {"loc": [{"x", "y", "z", "r", "done": 1, "hid": 1, "id": location id}], "area": 1},
//	                         TurnIn {"items": [...], "tags": [...], "src": [...], "any": 1, "crate": 1, "ct": [crate types], "ci": [crate items], "qv": variable, "inc": 1},
//	                         EventNotification {"nt": type, "recipe": id}, UseAbility {"ab": [...]}, Custom {"rule": a requirement of one criterion}}
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
    rewards          TEXT NOT NULL,        -- JSON list of reward table guids
    detail           TEXT NOT NULL         -- (version 27) JSON object: the actions run when it completes, fails and every tick, its timeout
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
    detail           TEXT NOT NULL,        -- (version 27) JSON object: see the list above
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
    node_guid  INTEGER PRIMARY KEY,        -- an AcceptQuest node or (version 27) a Confirmation node
    quest_guid INTEGER NOT NULL,           -- 0 for a Confirmation node
    on_success INTEGER NOT NULL,           -- OnSuccessNode: where the talk goes on after the quest is taken or the player confirms (0: it ends)
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
CREATE TABLE item_crate (
    item_guid  INTEGER PRIMARY KEY,
    crate_guid INTEGER NOT NULL            -- the CrateRecordBase the item is a crate of (ItemRecordBase.CrateId)
);
CREATE TABLE shrine_icd (
    icd_guid    INTEGER PRIMARY KEY,       -- an InteractableComponentDef named ICD_<Place>_Attunable_Respawn_Shrine
    attune_guid INTEGER NOT NULL           -- the attune point of that place (attune_point), found by the name
);
CREATE TABLE quest_kill_set (
    set_guid      INTEGER NOT NULL,        -- a population set some Kill objective names
    instance_guid INTEGER NOT NULL,        -- a creature (population instance) that stands in it
    PRIMARY KEY (set_guid, instance_guid)
) WITHOUT ROWID;
CREATE TABLE interactable (
    icd_guid       INTEGER PRIMARY KEY,    -- (version 36) an InteractableComponentDef: what the object of a quest (a body, a crate, a root, a site) is used by
    name           TEXT NOT NULL,
    radius         REAL NOT NULL,          -- InteractRadius, cm
    tolerance      REAL NOT NULL,          -- InteractTolerance, cm
    discovery      REAL NOT NULL,          -- DiscoveryDistance, cm
    require_los    INTEGER NOT NULL,       -- bRequireLOS
    requirement    TEXT NOT NULL,          -- the first input point's GameplayRequirements as a requirement tree ('' none): what must hold for the prompt
    pid_guid       INTEGER NOT NULL,       -- its PlayerInteractionDef (0: none)
    channel        REAL NOT NULL,          -- ChannelDuration, seconds: the time the use takes (0: at once)
    busy           REAL NOT NULL,          -- BusyTime
    allow_combat   INTEGER NOT NULL,       -- AllowInCombat
    single_use     INTEGER NOT NULL,       -- SingleUse
    delete_done    INTEGER NOT NULL,       -- DeleteOnCompletion
    delete_delay   REAL NOT NULL,          -- DeletionDelay
    anim           TEXT NOT NULL,          -- ChannelAnimation (the client plays it)
    grants         TEXT NOT NULL,          -- a JSON list [[item, quantity], ...]: ItemsToGrantUponCompletion
    reward_table   INTEGER NOT NULL,       -- RewardTableId (0: none)
    complete_objs  TEXT NOT NULL,          -- a JSON list of objective records the use completes
    reveal_objs    TEXT NOT NULL           -- a JSON list of objective records the use reveals
);
CREATE TABLE interactable_actor_def (
    iad_guid      INTEGER PRIMARY KEY,     -- (version 36) an InteractableActorDef
    despawn_delay REAL NOT NULL,           -- DespawnDelayTime, seconds
    despawn_final INTEGER NOT NULL         -- bDespawnAfterFinalState: the object leaves after its use
);
CREATE TABLE instance_iad (
    instance_guid INTEGER PRIMARY KEY,     -- (version 36) a population instance that names an InteractableActorDef
    iad_guid      INTEGER NOT NULL
);
CREATE TABLE quest_item (
    item_guid INTEGER PRIMARY KEY,         -- (version 36) an item whose record says InventoryFilterType Quest: it belongs in the Quest bag
    major     INTEGER NOT NULL             -- QuestItemSignificance Major
);
CREATE TABLE item_box (
    item_guid  INTEGER PRIMARY KEY,        -- (version 36) an item that opens the choose-one reward window (its activation loot box)
    table_guid INTEGER NOT NULL            -- ... the reward table it lists
);
`

// QuestTables are the tables QuestDDL creates, in the DDL's order.
var QuestTables = []string{"quest", "quest_objective", "npc_offered_quest", "dialogue_accept", "zoi_quest", "item_quest", "item_crate", "shrine_icd", "quest_kill_set", "interactable", "interactable_actor_def", "instance_iad", "quest_item", "item_box"}

func createQuests(db *sql.DB) error {
	if _, err := db.Exec(QuestDDL); err != nil {
		return fmt.Errorf("content: create the quest tables: %w", err)
	}
	return nil
}
