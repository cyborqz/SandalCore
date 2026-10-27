// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  quest/quest_wire.h  (quests milestone, slice 1, "take a quest")
//
//  The quest log as the client's quest component takes it: the PlayerController's "QuestConsumerComponent" holds the quests as two fast arrays.
//
//    ReplicatedQuestCache   one item per quest: the quest's record, the guid of the instance and of each of its objectives, when it started, the Local flag
//                           (Locality 1) and, empty or at their defaults, the members slice 1 does not use (rewards, marker, participants, spawn results)
//    RepQuestVariables      one item per quest: its variables (quest_log.h names them), keyed by the quest instance's guid
//
//  The client's journal lists the quests of the first array and reads each one's state from the second; its tracker follows the objectives that are Active and not Hidden.
//  An item is laid out the way the game's own server writes it: the members of the quest instance first and its InstanceGuid and RecordId LAST (the generated schema has
//  that order for this one struct), an objective inside it the other way round (its InstanceGuid and RecordId first).  The marker array (ObjectiveMarkerCache) is not used.
//
//  sync() makes the arrays equal the log: a quest the client was not told of is added, one that changed is replaced, one that left the log is deleted.  The caller sends the change.
// ============================================================================
#pragma once

#include "protocol/repl/fast_array.h"
#include "quest/quest_log.h"

#include <string>

namespace aoc::worldd::quest {

/// The type id every item record id carries (the item table's); the quest instance's InstigatorItemInstance has it even when no item started the quest.
inline constexpr int64_t kItemRecordTypeId = 2616523457551378613LL;

/// The component's schema (the class UQuestConsumerComponent); null when the build has none.
const protocol::schema::ClassSchema* component_schema();

/// The two items of a quest, for the layouts of the component's two arrays.  Nullopt when a command of the layout is not one this module knows.
std::optional<protocol::repl::ElementState> cache_item(const protocol::repl::FastArrayLayout& layout, const QuestInstance& q);
std::optional<protocol::repl::ElementState> variables_item(const protocol::repl::FastArrayLayout& layout, const QuestInstance& q);

/// Brings `arrays` (the component's fast arrays) in line with `log`.  False, with `error`, when an item does not fit; the arrays may then hold part of the change.
bool sync(protocol::repl::FastArrays& arrays, QuestLog& log, std::string* error = nullptr);

}  // namespace aoc::worldd::quest
