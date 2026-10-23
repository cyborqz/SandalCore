// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package content

import (
	"database/sql"
	"fmt"
)

// NPC dialogue (quests milestone, slice 0, "the quest givers talk"; contentc version 17): the conversation graphs an NPC opens when a player uses it.
//
//	dialogue_root    a conversation's entry: the node it starts at (top_node 0: the root has no conversation)
//	dialogue_node    one node: its kind (Response, Automatic, AcceptQuest, TurnIn, Confirmation, BuyOrderFulfillment), what it does when the
//	                 conversation runs out of ways on (exhaust), the quest it belongs to and the actions it runs on entering and leaving (JSON lists)
//	dialogue_choice  one line the player can pick, by its index in the node's list (the client names a line by that index): where it leads (next_node 0: it
//	                 ends the conversation), the condition that shows it (requirement: JSON, empty when it always shows) and its actions (JSON list)
//	dialogue_quest   the quests a condition names, with the rule that makes each available (activation, JSON, empty when it has none): only what the conditions
//	                 need, so that "Quest(X.Available)" can be answered from a character with no quests
//	npc_dialogue     which root an NPC opens: the instance's own root wins over the asset's (source_kind 'instance' or 'asset')
//
// A requirement is {"tok": [..], "q": [..]}: tok is the condition tree as numbers (node := type count child..., type 0 AND, 2 OR over nodes; 1 ALL, 3 ANY, 5 NONE
// over criterion indexes), q the criteria it indexes. A criterion keeps only what is set: t (kind), op, v (the number it compares with), q (quest), qq (quest query),
// o (objective), oq (objective query), var (variable name), cur (currency), x (expression text), items (item ids), tags (item tags), loc (item location).
const DialogueDDL = `
CREATE TABLE dialogue_root (
    guid     INTEGER PRIMARY KEY,
    name     TEXT NOT NULL,
    top_node INTEGER NOT NULL          -- 0: the root has no conversation
);
CREATE TABLE dialogue_node (
    guid       INTEGER PRIMARY KEY,
    name       TEXT NOT NULL,
    node_type  TEXT NOT NULL,          -- Response, Automatic, AcceptQuest, TurnIn, Confirmation, BuyOrderFulfillment
    exhaust    TEXT NOT NULL,          -- CloseAndClear, ReEnterGreeting: what happens when no line is left
    quest_guid INTEGER NOT NULL,       -- the node's QuestId (0: none)
    on_enter   TEXT NOT NULL,          -- JSON list of {"a": action, "w": window}: what entering runs
    on_exit    TEXT NOT NULL           -- the same for leaving
);
CREATE TABLE dialogue_choice (
    node_guid   INTEGER NOT NULL,
    idx         INTEGER NOT NULL,      -- the index in the node's Choices: how the client names the line
    next_node   INTEGER NOT NULL,      -- 0: the line ends the conversation
    requirement TEXT NOT NULL,         -- JSON requirement, '' when the line always shows
    actions     TEXT NOT NULL,         -- JSON list of {"a": action, "w": window}
    PRIMARY KEY (node_guid, idx)
) WITHOUT ROWID;
CREATE TABLE dialogue_quest (
    guid       INTEGER PRIMARY KEY,
    name       TEXT NOT NULL,
    disabled   INTEGER NOT NULL,       -- bDisabled
    repeatable INTEGER NOT NULL,       -- bRepeatable
    activation TEXT NOT NULL           -- JSON requirement, '' when the quest has none
);
CREATE TABLE npc_dialogue (
    source_kind TEXT NOT NULL,         -- 'instance' (PopulationInstance.DialogueRootId) or 'asset' (PopulationAsset.DialogueRoot)
    source_guid INTEGER NOT NULL,
    root_guid   INTEGER NOT NULL,
    PRIMARY KEY (source_kind, source_guid)
) WITHOUT ROWID;
`

// DialogueTables are the tables DialogueDDL creates, in the DDL's order.
var DialogueTables = []string{"dialogue_root", "dialogue_node", "dialogue_choice", "dialogue_quest", "npc_dialogue"}

func createDialogue(db *sql.DB) error {
	if _, err := db.Exec(DialogueDDL); err != nil {
		return fmt.Errorf("content: create the dialogue tables: %w", err)
	}
	return nil
}
