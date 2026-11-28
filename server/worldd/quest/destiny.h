// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  quest/destiny.h  (quests milestone, the Destiny window: the tracks)
//
//  The Destiny window shows six tracks.  In the data a track is a chain of quests: the track quest the window's request names (First Steps: Destiny_Novice_FirstStep_02_Root) has one
//  SubQuest objective that starts the track's own quest (Destiny_Novice_FirstStep_02), and that quest has twelve SubQuest objectives: eight tasks, each started at once and each
//  raising the quest variable Destiny_Reward by one when its quest is done, and the milestones, which wait for Destiny_Reward to reach their number.  The master quest
//  Destiny_Root_Quest has one SubQuest objective for each track's own quest, with the character-level rule that opens the track (First Steps and Combat at level 1, the other four at 5).
//
//  What the game's server does with them (the answers the game's own server sent are the evidence):
//    * the master quest and the track quests never reach the client's quest cache; the six own quests do, with their task quests under them.  So the character's log holds the own
//      quests and the tasks only: seeds() names the own quests whose gate holds now and that are neither in the log nor finished.
//    * ClientReceiveDestinyClientData answers each track the client names: its track quest, the guid of the track's OWN quest instance (the guid the window blacklists and looks up in
//      the cache; zero when the character has none), how many of the eight tasks are done (the quest variable Destiny_Reward), the eight and whether the quest is in the log and active.
//
//  Pure logic over the log and the data: no network, no clock.
// ============================================================================
#pragma once

#include "dialogue/requirement.h"
#include "quest/quest_book.h"
#include "quest/quest_log.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

namespace aoc::worldd::quest {

/// The master quest's name in the data.
inline constexpr const char* kDestinyMasterQuest = "Destiny_Root_Quest";
/// The quest variable that counts a track's finished tasks (the quest variable name the tasks' success actions raise).
inline constexpr const char* kDestinyRewardVariable = "Destiny_Reward";

/// The own quests the character takes now: each SubQuest objective of the master quest whose activation rule holds starts its quest, when the character has it neither in the log nor
/// finished (QuestLog::can_start).  In the master quest's order.  Nothing here changes the log: the caller starts the quests (which may change what the next one's rule sees).
inline std::vector<const content::QuestRow*> destiny_seeds(const QuestBook& book, const QuestLog& log, dialogue::Evaluator& ev) {
    std::vector<const content::QuestRow*> out;
    const content::QuestRow* master = book.by_name(kDestinyMasterQuest);
    if (!master) return out;
    for (const content::QuestObjectiveRow& o : master->objectives) {
        if (o.type != "SubQuest" || o.sub_quest == 0) continue;
        if (!ev.holds(o.activation)) continue;
        const content::QuestRow* own = book.quest(o.sub_quest);
        if (own && log.can_start(own, ev, /*bypass_rules=*/false) == Create::Ok) out.push_back(own);
    }
    return out;
}

/// One track of the window's answer.
struct DestinyAnswer {
    int64_t track_quest = 0;      // the quest the client named (echoed)
    Guid128 guid;                 // the track's own quest instance in the log (zero: none)
    int32_t completed = 0;        // the tasks done: the quest variable Destiny_Reward, never above `total`
    int32_t total = 0;            // the tasks: the own quest's objectives that have no activation rule
    bool    active = false;       // the quest is in the log and active
};

/// The answer for the track the client names by `track_quest` (a _02_Root record); nullopt for a quest that is no track of the data.
inline std::optional<DestinyAnswer> destiny_answer(const QuestBook& book, const QuestLog& log, int64_t track_quest) {
    const content::DestinyTrackRow* track = book.destiny_track(track_quest);
    if (!track) return std::nullopt;
    DestinyAnswer a;
    a.track_quest = track_quest;
    const content::QuestRow* own = track->quest != 0 ? book.quest(track->quest) : nullptr;
    if (!own) return a;
    for (const content::QuestObjectiveRow& o : own->objectives) {
        if (o.activation.empty()) ++a.total;
    }
    if (const QuestInstance* q = log.find(own->guid)) {
        a.guid = q->guid;
        a.active = q->status == Status::Active;
        a.completed = static_cast<int32_t>(std::clamp<int64_t>(log.quest_variable(own->guid, kDestinyRewardVariable), 0, a.total));
    }
    return a;
}

/// The answers to a request that names `tracks` (the DestinyTrackQuest of each element, in the client's order): one for each track of the data, in the request's order, a track named
/// twice answered once, a quest that is no track skipped.  At most `kMaxDestinyRequest` elements are read.
inline constexpr size_t kMaxDestinyRequest = 16;
inline std::vector<DestinyAnswer> destiny_answers(const QuestBook& book, const QuestLog& log, const std::vector<int64_t>& tracks) {
    std::vector<DestinyAnswer> out;
    for (size_t i = 0; i < tracks.size() && i < kMaxDestinyRequest; ++i) {
        const bool again = std::any_of(out.begin(), out.end(), [&](const DestinyAnswer& a) { return a.track_quest == tracks[i]; });
        if (again) continue;
        if (auto a = destiny_answer(book, log, tracks[i])) out.push_back(*a);
    }
    return out;
}

}  // namespace aoc::worldd::quest
