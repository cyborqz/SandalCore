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
//  that order for this one struct), an objective inside it the other way round (its InstanceGuid and RecordId first).
//
//  Map markers: a third array, ObjectiveMarkerCache, holds one item per objective that was active with places to mark (quest/markers.h makes the places): the objective's record and
//  instance guid, which are how the client matches the item to the objective in the quest cache, and the places.  The client draws a pin for each active objective with places and
//  never redraws a drawn one, so an item is added once, in the same update that makes the objective active, and is never changed or deleted (a pin goes when its objective is done).
//
//  Quest givers: a fourth array, QuestGiverInfos, tells the client which NPCs have a quest to take: one item per NPC (its population instance) with the map registration the NPC shows
//  while the quest is on offer.  An NPC with nothing on offer any more has its item changed to "none" (the client keeps what it was told when an item is deleted).
//
//  sync() makes the arrays equal the log: a quest the client was not told of is added, one that changed is replaced, one that left the log is deleted.  The caller sends the change.
// ============================================================================
#pragma once

#include "protocol/repl/fast_array.h"
#include "quest/markers.h"
#include "quest/quest_book.h"
#include "quest/quest_log.h"

#include <string>
#include <vector>

namespace aoc::worldd::quest {

/// The type id every item record id carries (the item table's); the quest instance's InstigatorItemInstance has it even when no item started the quest.
inline constexpr int64_t kItemRecordTypeId = 2616523457551378613LL;

/// The component's schema (the class UQuestConsumerComponent); null when the build has none.
const protocol::schema::ClassSchema* component_schema();

/// The two items of a quest, for the layouts of the component's two arrays.  Nullopt when a command of the layout is not one this module knows.
std::optional<protocol::repl::ElementState> cache_item(const protocol::repl::FastArrayLayout& layout, const QuestInstance& q);
std::optional<protocol::repl::ElementState> variables_item(const protocol::repl::FastArrayLayout& layout, const QuestInstance& q);

/// What sync needs to put the active objectives on the map: the quests' data and the places of the world's people and creatures.
struct MarkerContext {
    const QuestBook* book = nullptr;
    PointSource*     points = nullptr;
    /// sync fills this: the objectives it made an item for (the objective's record, the quest's record and the number of places), for the log
    struct Made {
        int64_t objective = 0, quest = 0;
        size_t  places = 0;
        double  x = 0, y = 0, radius = 0;   // the first place (cm), for the log
        bool    area = false;               // ... drawn as a circle
    };
    std::vector<Made> made;
    /// ... and this: the objectives it could not make an item for (the quests go on without that pin)
    size_t refused = 0;
};

/// The item of the marker array for one objective: its record, its instance guid in the quest cache and its places.  Nullopt when a command of the layout is not one this module knows.
std::optional<protocol::repl::ElementState> marker_item(const protocol::repl::FastArrayLayout& layout, int64_t objective_record, const Guid128& objective_instance,
                                                        const std::vector<MarkerLocation>& locations);

/// The EQuestFlavor value the client's quest giver item carries for a quest of the data's flavor name ("Quest", "Commission" ...), and the value for "no quest".
uint8_t giver_flavor(const std::string& data_flavor);
inline constexpr uint8_t kGiverFlavorNone = 7;

/// The type ids of the ids a quest giver item holds (the population instance's and the map registration's).
inline constexpr int64_t kPopulationInstanceTypeId = 6814160152926513068LL;
inline constexpr int64_t kMapRegistrationTypeId = 5551905168185487477LL;

/// The item of the quest giver array for the NPC `instance`: the flavor of the quest on offer and the map registration the NPC shows (registration 0, flavor kGiverFlavorNone: nothing on offer).
std::optional<protocol::repl::ElementState> giver_item(const protocol::repl::FastArrayLayout& layout, int64_t instance, uint8_t flavor, int64_t registration);

/// Brings `arrays` (the component's fast arrays) in line with `log`.  False, with `error`, when an item does not fit; the arrays may then hold part of the change.  With `markers`, an
/// active objective that has places to mark and no item yet gets its item in the marker array (an item is never changed or deleted).
bool sync(protocol::repl::FastArrays& arrays, QuestLog& log, std::string* error = nullptr, MarkerContext* markers = nullptr);

}  // namespace aoc::worldd::quest
