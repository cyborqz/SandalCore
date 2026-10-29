// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  quest/quest_log.h  (quests milestone, slice 1, "take a quest")
//
//  One character's quest log: the quests it took and has not finished, how often it finished each, and its quest variables.  Pure logic over the data: no network, no clock
//  (the caller says what time it is).
//
//  What the client sees of a quest is a few numbers.  The quest's record, a guid for the instance and for each of its objectives, and the quest variables: for the quest
//  "<quest>.Active", "<quest>.Completed", "<quest>.Failed", "<quest>.Paused", and for each objective "<quest>.<objective>.Active", ".Awake", ".Completed", ".Count",
//  ".Failed", ".Hidden", ".Succeeded" (the numbers are the records' guids in decimal).  The client keys the variables by the 64-bit CityHash (version 1.1) of the name's
//  UTF-8 bytes.  A new quest has the quest Active and each objective Hidden or Awake (and Active) as its record says; the quest's start actions then change a few of them.
//
//  The rules of taking a quest (Create below) are the game's: a quest has to be in the data and not disabled, switched on for default activation, not in the log already, not
//  finished before unless it is repeatable, the log not full (500 quests), and its activation rule has to hold.  A GM start may bypass the activation rule only.
// ============================================================================
#pragma once

#include "content/content_db.h"
#include "dialogue/requirement.h"
#include "svc/services.h"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace aoc::worldd::quest {

/// CityHash64 (version 1.1) of `bytes`: the hash the client keys quest variables by.
uint64_t city_hash64(std::string_view bytes);

/// "<quest>.<suffix>" and "<quest>.<objective>.<suffix>" with the records' guids in decimal.
std::string quest_variable_name(int64_t quest, std::string_view suffix);
std::string objective_variable_name(int64_t quest, int64_t objective, std::string_view suffix);

/// An FGuid: four 32-bit words.
struct Guid128 {
    uint32_t a = 0, b = 0, c = 0, d = 0;
    bool zero() const { return (a | b | c | d) == 0; }
    std::string hex() const;                                   // 32 lower-case digits, the words in order
    static std::optional<Guid128> from_hex(std::string_view s);
    bool operator==(const Guid128&) const = default;
};

/// EQuestCreateResponse, the values this module answers with.
enum class Create : uint8_t {
    Ok = 0,
    InvalidStaticData = 4,       // the quest is not in the data
    MaxQuestLimitReached = 6,
    QuestDisabled = 8,
    DefaultActivateNotSet = 9,
    QuestAlreadyActive = 10,
    QuestNotRepeatable = 11,
    FailedActivationRules = 12,
};
const char* create_name(Create c);

/// The most quests one log holds (the game's own limit).
inline constexpr size_t kMaxQuests = 500;

enum class Status : int32_t { Active = 0, Completed = 1, Failed = 2, Paused = 3 };

struct ObjectiveState {
    Guid128 guid;
    int64_t record = 0;
    bool    active = false, awake = false, hidden = false, completed = false, succeeded = false, failed = false;
    int32_t count = 0;
    double  start_time = 0;
    /// Map markers: the replication id of the objective's item in the quest component's marker array (-1: the client has none).  The client never redraws a drawn objective, so an item
    /// is made once, when the objective is active with places to mark, and never changed or taken out (not saved: the next login makes the items of the active objectives again).
    int32_t marker_id = -1;
};

struct QuestInstance {
    Guid128 guid;
    int64_t record = 0;
    Status  status = Status::Active;
    double  start_time = 0;         // when it started, in the world's seconds
    double  end_time = 0;           // quests, slice 2: when it was completed or failed (not saved: a quest found done at the login is taken out of the log at once)
    int64_t source = 0;             // the NPC (population instance) that gave it, 0: none
    std::vector<ObjectiveState> objectives;
    // what the client was told: the replication ids of its two items (kIndexNone while it was not sent), and whether it has to be sent again
    int32_t cache_id = -1, vars_id = -1;
    bool    wire_dirty = true;
};

/// What taking a quest did.
struct Started {
    QuestInstance*           instance = nullptr;
    std::vector<std::string> skipped;       // start actions this milestone does not do ("GrantItem")
};

class QuestLog {
public:
    /// `seed` 0: the guids are random; a test gives one.
    explicit QuestLog(uint64_t seed = 0);

    const std::vector<QuestInstance>& instances() const { return instances_; }
    /// For the wire layer, which records what the client was told in each instance (cache_id, vars_id, wire_dirty).
    std::vector<QuestInstance>& instances_for_wire() { return instances_; }
    const QuestInstance* find(int64_t record) const;
    QuestInstance*       find(int64_t record);
    const QuestInstance* find_guid(const Guid128& guid) const;
    int32_t              completions(int64_t record) const;
    const std::map<int64_t, int32_t>& completed() const { return completed_; }
    const std::map<std::string, int64_t, std::less<>>& variables() const { return variables_; }
    int64_t variable(const std::string& name) const;
    void    set_variable(const std::string& name, int64_t value);
    /// Quests, slice 2: a named variable of one quest (IncrementQuestVariable / SetQuestVariable).  It is kept with the character's variables under the name "@<quest>.<name>" (so it is saved
    /// with the log and read back by the next login) and is forgotten when the quest leaves the log.
    int64_t quest_variable(int64_t quest, const std::string& name) const;
    void    set_quest_variable(int64_t quest, const std::string& name, int64_t value);
    /// The log changed in a way that is saved (the engine edited an instance); `touch` also marks the instance for the wire.
    void    mark_changed() { changed_ = true; }
    void    touch(QuestInstance& q) {
        q.wire_dirty = true;
        changed_ = true;
    }

    /// Whether the character may take `def` now.  `ev` answers the activation rule (and the quest conditions inside it).  `bypass_rules`: a GM start (the rule is not asked).
    Create can_start(const content::QuestRow* def, dialogue::Evaluator& ev, bool bypass_rules) const;

    /// Takes the quest: the instance joins the log (its objectives as the record says, then its start actions).  The caller has asked can_start.  `run_actions` false: the instance only (the
    /// quest engine runs the start actions itself, all of them).
    Started start(const content::QuestRow& def, double now, int64_t source, bool run_actions = true);

    /// Takes the quest out of the log without counting it as finished (an abandon, a fail).  False when it is not in the log.
    bool remove(int64_t record);
    /// The quest counts as finished: out of the log, one more finish.  False when it is not in the log.
    bool finish(int64_t record);
    /// Forget the finished quests and the variables (`record` 0: all; else that quest's finishes only).
    void clear(int64_t record);

    /// The dialogue conditions' view of the log (dialogue::Facts).
    std::optional<int64_t> quest_count(int64_t quest, const std::string& query) const;
    std::optional<int64_t> objective_count(int64_t quest, int64_t objective, const std::string& query) const;

    /// Whether the client has to be told of a change: an instance not sent yet or changed since, or one that left the log.
    bool wire_pending() const {
        if (!removed_.empty()) return true;
        for (const QuestInstance& q : instances_) {
            if (q.wire_dirty || q.cache_id < 0 || q.vars_id < 0) return true;
        }
        return false;
    }

    /// The two items of a quest removed from the log that the client has not been told of yet (a removal is a deletion of those ids).
    struct Removed { int32_t cache_id = -1, vars_id = -1; };
    std::vector<Removed> take_removed();

    /// Whether the log changed since the last time it was saved (clear_changed); the save is the whole log.
    bool changed() const { return changed_; }
    void clear_changed() { changed_ = false; }

    svc::SavedQuestLog to_saved() const;
    /// Loads a saved log.  A quest the data does not hold is dropped (and counted in `dropped`); `now` caps the start times (the world's clock restarts with the server).
    /// The log then counts as changed when anything was dropped or repaired.
    size_t load(const svc::SavedQuestLog& saved, const content::QuestData& book, double now);

    Guid128 new_guid();

private:
    QuestInstance* start_instance(const content::QuestRow& def, double now, int64_t source);

    std::vector<QuestInstance> instances_;
    std::map<int64_t, int32_t> completed_;
    std::map<std::string, int64_t, std::less<>> variables_;
    std::vector<Removed> removed_;
    bool changed_ = false;
    std::mt19937_64 rng_;
};

/// The variables the client keeps for one instance, in the order the game lists them: the quest's four, then each objective's seven.  (key, value).
std::vector<std::pair<uint64_t, int32_t>> instance_variables(const QuestInstance& q);

}  // namespace aoc::worldd::quest
