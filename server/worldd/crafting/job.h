// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  crafting/job.h  (crafting, steps 1 and 2: "start a processing job", "collect it"; header only)
//
//  A processing job as the session holds it (svc::ProcessingJob, the character service's row) and how the window is shown it.
//
//  The clock.  A job runs on the server's wall clock, not on the session's: it began at `started_at_ms` (the moment it was started, or the end of the player's last job at the station
//  when it had to wait) and is done `duration_ms` later, whether its player is logged in or not.  The window wants the times on the client's synchronized clock (the one ClientSetTime sets,
//  whole seconds), so the same instant is shown as that clock's reading now less the time that passed since.
//
//  The status.  A job that has not begun is Pending (1), one that has is InProgress (2), one whose time is over is Complete (3) and carries what it made (its products, the
//  experience, the crafter's level).  The window shows a Complete job's Claim button the moment it is sent such a state, whatever the clock says; its own countdown only changes the label.
//  Collecting a job (step 2) deletes it: the window is sent the same state once more, at Claimed (4), without the products.
//
//  Finishing a job at once (a game master's tool) moves its start back so that it ends now: the job then is what a job that ran its time out is, and nothing else needs to know.
//
//  The queue.  A player has his own queue at a station (the station's `individual`): at most `queue_size` jobs that are not over, one after the other (`simultaneous` is 1 at every
//  processing station).  A job started at a station with a job still running begins when that job ends.
// ============================================================================
#pragma once

#include "content/content_db.h"
#include "crafting/station.h"
#include "items/inventory.h"
#include "protocol/repl/craft_rpc.h"
#include "svc/services.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace aoc::worldd::crafting {

namespace repl = ::aoc::protocol::repl;

/// EProcessingJobState values a job is shown with.
inline constexpr uint8_t kPending = 1, kInProgress = 2, kComplete = 3, kClaimed = 4, kCancelled = 5;
/// Jobs one player may hold at one station, finished ones included (they stay until they can be collected), and in all.
inline constexpr size_t kMaxJobsAtStation = 20;
inline constexpr size_t kMaxJobsHeld = 200;

/// The job's status at the wall-clock instant `now_ms`.
inline uint8_t status_at(const svc::ProcessingJob& j, int64_t now_ms) { return now_ms < j.started_at_ms ? kPending : now_ms < j.end_ms() ? kInProgress : kComplete; }
/// Whether the job's time is not over yet (it counts against the station's queue).
inline bool unfinished(const svc::ProcessingJob& j, int64_t now_ms) { return now_ms < j.end_ms(); }

/// What a finished job made and pays, as rewards.h works it out: the products of the whole job (one entry for each item, all of one quality), the experience, the level it was worked out at.
struct Product {
    int64_t item = 0;
    int64_t quantity = 0;
};
struct JobRewards {
    std::vector<Product> products;
    int                  quality = 1;           // the quality of every product
    int                  rarity = 0;            // ... and its band (Common 0 ... Artifact 6)
    int64_t              profession = 0;        // the profession the experience goes to
    int64_t              profession_xp = 0;     // 0: none (a recipe with no such reward, or a formula the server does not know)
    int64_t              adventuring_xp = 0;
    int                  level = 1;             // the crafter's level the Adventuring experience was worked out at
};

/// The rewards of a job, or nullopt when they cannot be worked out (its recipe is none of the data's).
using RewardFn = std::function<std::optional<JobRewards>(const svc::ProcessingJob&)>;

/// The synchronized clock's reading at the wall-clock instant `at_ms`, given its reading `sync_now` at `now_ms`.
inline double sync_of(int64_t at_ms, int64_t now_ms, double sync_now) { return sync_now - static_cast<double>(now_ms - at_ms) / 1000.0; }

inline std::vector<repl::CraftResource> resources_of(const std::vector<svc::JobResource>& in) {
    std::vector<repl::CraftResource> out;
    out.reserve(in.size());
    for (const svc::JobResource& r : in) {
        repl::CraftResource c;
        c.record = r.record_id;
        for (const svc::JobRarity& ra : r.rarities) c.rarities.push_back({static_cast<uint8_t>(ra.rarity), ra.count, ra.from_warehouse});
        out.push_back(std::move(c));
    }
    return out;
}

inline std::vector<svc::JobResource> job_resources_of(const std::vector<repl::CraftResource>& in) {
    std::vector<svc::JobResource> out;
    out.reserve(in.size());
    for (const repl::CraftResource& r : in) {
        svc::JobResource c;
        c.record_id = r.record;
        for (const repl::CraftRarityCount& ra : r.rarities) c.rarities.push_back({ra.rarity, ra.count, ra.from_warehouse});
        out.push_back(std::move(c));
    }
    return out;
}

/// The guid of the `index`th product of a job: made from the job's own guid, so the same job shows the same products every time it is shown.
inline repl::GuidValue product_guid(const repl::GuidValue& job, size_t index) {
    repl::GuidValue g{job.a ^ 0x5bd1e995u, job.b + static_cast<uint32_t>(index) * 0x9e3779b9u, job.c ^ 0x1b873593u, job.d + static_cast<uint32_t>(index) + 1u};
    if (!(g.a | g.b | g.c | g.d)) g.a = 1;
    return g;
}

/// The items a finished job shows in its window: the products at the job's quality, made when the job ended.
inline std::vector<repl::ItemInstance> product_instances(const svc::ProcessingJob& j, const repl::GuidValue& job, const JobRewards& rewards) {
    std::vector<repl::ItemInstance> out;
    out.reserve(rewards.products.size());
    for (size_t i = 0; i < rewards.products.size(); ++i) {
        const Product& p = rewards.products[i];
        repl::ItemInstance it;
        it.has_item = true;
        it.record_id = static_cast<uint64_t>(p.item);
        it.quantity = static_cast<uint32_t>(std::clamp<int64_t>(p.quantity, 1, 1 << 20));
        it.quality = static_cast<uint32_t>(std::clamp(rewards.quality, 1, 200));
        it.guid = product_guid(job, i);
        it.creation_time = items::ticks_of_ms(j.end_ms());
        out.push_back(std::move(it));
    }
    return out;
}

/// The job as the window shows it at `now_ms` (the synchronized clock reading `sync_now` then): a Pending job's StartTime is the moment it was queued, a running or finished one's the moment
/// it began.  A job whose time is over is shown Complete with what `rewards` says it made; without them (the recipe is unknown) it stays InProgress.  nullopt for a job whose guids do not read.
inline std::optional<repl::CraftItemState> to_state(const svc::ProcessingJob& j, int64_t now_ms, double sync_now, const JobRewards* rewards = nullptr) {
    const auto txn = items::guid_of(j.client_txn);
    const auto guid = items::guid_of(j.job_guid);
    if (!txn || !guid) return std::nullopt;
    repl::CraftItemState s;
    s.recipe = j.recipe_guid;
    s.sub_recipe = j.sub_recipe_guid;
    s.status = status_at(j, now_ms);
    if (s.status == kComplete && !rewards) s.status = kInProgress;
    s.start_time = std::floor(sync_of(s.status == kPending ? j.queued_at_ms : j.started_at_ms, now_ms, sync_now));
    s.duration = static_cast<float>(static_cast<double>(j.duration_ms) / 1000.0);
    s.quantity = j.quantity;
    s.general = resources_of(j.resources);
    s.transaction = *txn;
    s.job = *guid;
    if (s.status == kComplete) {
        if (rewards->profession_xp > 0) s.experience.push_back({rewards->profession, content::kTypeCharacterProfession, rewards->profession_xp});
        if (rewards->adventuring_xp > 0) s.experience.push_back({content::kGuidAdventuring, content::kTypeLevelingTypeDef, rewards->adventuring_xp});
        s.products = product_instances(j, *guid, *rewards);
        s.level = rewards->level;
    }
    return s;
}

/// The state a collected job leaves the window: its own, at Claimed, without the products and the experience.
inline std::optional<repl::CraftItemState> claimed_state(const svc::ProcessingJob& j, int64_t now_ms, double sync_now) {
    auto s = to_state(j, now_ms, sync_now);
    if (!s) return std::nullopt;
    s->status = kClaimed;
    s->start_time = std::floor(sync_of(j.started_at_ms, now_ms, sync_now));
    return s;
}

/// The player's jobs at the station `key`, in the order they were queued, as the window shows them.  `rewards` works out what a finished job made (none: a finished job stays InProgress).
inline std::vector<repl::CraftItemState> states_at(const std::vector<svc::ProcessingJob>& jobs, const std::string& key, int64_t now_ms, double sync_now, const RewardFn& rewards = {}) {
    std::vector<repl::CraftItemState> out;
    for (const svc::ProcessingJob& j : jobs) {
        if (j.station_key != key) continue;
        std::optional<JobRewards> made;
        if (rewards && !unfinished(j, now_ms)) made = rewards(j);
        if (auto s = to_state(j, now_ms, sync_now, made ? &*made : nullptr)) out.push_back(std::move(*s));
    }
    return out;
}

/// The job of the player with this guid (32 hex digits) at the station `key`, nullptr when none.
inline const svc::ProcessingJob* find_job(const std::vector<svc::ProcessingJob>& jobs, const std::string& key, const std::string& job_guid) {
    for (const svc::ProcessingJob& j : jobs) {
        if (j.station_key == key && j.job_guid == job_guid) return &j;
    }
    return nullptr;
}

/// A game master finishes the player's jobs that are not over (all of them, or only those at the station `key` when it is not empty): each one's start moves back so that it ends at
/// `now_ms`, its length kept (and its queue time with it: a job never starts before it is queued).  The number of jobs finished.
inline size_t finish_now(std::vector<svc::ProcessingJob>& jobs, const std::string& key, int64_t now_ms) {
    size_t n = 0;
    for (svc::ProcessingJob& j : jobs) {
        if ((!key.empty() && j.station_key != key) || !unfinished(j, now_ms)) continue;
        j.started_at_ms = now_ms - j.duration_ms;
        j.queued_at_ms = std::min(j.queued_at_ms, j.started_at_ms);
        ++n;
    }
    return n;
}

/// How many of the player's jobs at the station `key` are not over / are held at all.
inline size_t unfinished_at(const std::vector<svc::ProcessingJob>& jobs, const std::string& key, int64_t now_ms) {
    size_t n = 0;
    for (const svc::ProcessingJob& j : jobs) n += j.station_key == key && unfinished(j, now_ms) ? 1 : 0;
    return n;
}
inline size_t held_at(const std::vector<svc::ProcessingJob>& jobs, const std::string& key) {
    size_t n = 0;
    for (const svc::ProcessingJob& j : jobs) n += j.station_key == key ? 1 : 0;
    return n;
}

/// The stations where the player has a job that is not over, each once, as the four words of its StationGuid (OwnerInfo.ActiveProcessingStations), in the order of the first job there.
/// A job at a key that names no station is left out.
inline std::vector<std::array<int32_t, 4>> active_station_words(const std::vector<svc::ProcessingJob>& jobs, int64_t now_ms) {
    std::vector<std::array<int32_t, 4>> out;
    std::vector<std::string> seen;
    for (const svc::ProcessingJob& j : jobs) {
        if (!unfinished(j, now_ms) || std::find(seen.begin(), seen.end(), j.station_key) != seen.end()) continue;
        seen.push_back(j.station_key);
        if (const auto seed = station_seed(j.station_key)) out.push_back(station_guid_words(*seed));
    }
    return out;
}

/// A new job guid: 32 lower-case hex digits, not all zero.
inline std::string new_job_guid() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    for (;;) {
        const uint64_t a = rng(), b = rng();
        const repl::GuidValue g{static_cast<uint32_t>(a >> 32), static_cast<uint32_t>(a), static_cast<uint32_t>(b >> 32), static_cast<uint32_t>(b)};
        if (g.a | g.b | g.c | g.d) return items::guid_hex(g);
    }
}

/// A job started at the station `key` at the wall-clock instant `now_ms`: it begins now, or when the player's last job there that is not over ends.
inline svc::ProcessingJob make_job(const std::vector<svc::ProcessingJob>& jobs, const std::string& key, int64_t recipe, int32_t quantity, int64_t duration_ms, const repl::GuidValue& transaction,
                                   std::vector<svc::JobResource> resources, int64_t now_ms) {
    svc::ProcessingJob j;
    j.job_guid = new_job_guid();
    j.station_key = key;
    j.recipe_guid = recipe;
    j.sub_recipe_guid = 0;
    j.quantity = quantity;
    j.queued_at_ms = now_ms;
    j.started_at_ms = now_ms;
    for (const svc::ProcessingJob& other : jobs) {
        if (other.station_key == key && other.end_ms() > j.started_at_ms) j.started_at_ms = other.end_ms();
    }
    j.duration_ms = duration_ms;
    j.client_txn = items::guid_hex(transaction);
    j.resources = std::move(resources);
    return j;
}

}  // namespace aoc::worldd::crafting
