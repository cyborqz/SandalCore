// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_processing.cpp
//
//  Processing jobs at the stations: the states the window is told, collecting a finished job, what a job pays, and the game master tool that finishes jobs.
// ============================================================================
#include "session/player_session.h"

#include "bootstrap/character_setup.h"
#include "combat/combat_world.h"
#include "crafting/job.h"
#include "crafting/rewards.h"
#include "crafting/rules.h"
#include "crafting/station.h"
#include "content/content_db.h"
#include "dialogue/talk.h"
#include "protocol/repl/ability_rpc.h"
#include "protocol/repl/dialogue_rpc.h"
#include "world/city_nodes.h"
#include "protocol/repl/hit_rpc.h"
#include "protocol/repl/item_rpc.h"
#include "protocol/repl/move_packed.h"
#include "protocol/repl/storage_rpc.h"
#include "session/gm_travel.h"
#include "protocol/repl/rpc_fields.h"
#include "items/acquired.h"
#include "vendor/node_tax.h"
#include "vendor/sell_rules.h"
#include "quest/quest_wire.h"
#include "quest/registry_points.h"
#include "world/zoi_surface.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <map>
#include <random>
#include <set>

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;
namespace schema = ::aoc::protocol::schema;
namespace wire = ::aoc::protocol::wire;
using bootstrap::Role;
using protocol::emit::FIntrepidNetworkGUID;

// -- crafting, step 2: collecting a job ---------------------------------------------------------------------------------------------------------------------

std::optional<crafting::JobRewards> PlayerSession::job_rewards(const svc::ProcessingJob& job) const {
    const content::CraftRecipeData* recipes = world_ ? world_->craft_recipes() : nullptr;
    if (!recipes) return std::nullopt;
    // crafting quality: the job's extra units, steps up and experience come from the artisan ratings it was started with, saved with it (a job saved before they were kept has none)
    return crafting::roll_job(*recipes, job, progress_.level, crafting::job_ratings(job), [this](int64_t item) { return world_->item_rarity_facts(item); });
}

double PlayerSession::artisan_stat(int64_t stat) const {
    const stats::StatValue* v = stats_ ? stats_->find(stat) : nullptr;
    return v ? static_cast<double>(v->value) : 0.0;
}

crafting::Ratings PlayerSession::artisan_ratings(int64_t profession) const {
    const content::CraftRecipeData* recipes = world_ ? world_->craft_recipes() : nullptr;
    if (!recipes || !stats_ || !stats_->started()) return {};
    return crafting::ratings_of(recipes->artisan, profession, [this](int64_t stat) { return artisan_stat(stat); });
}

const world::WorldActor* PlayerSession::station_by_key(const std::string& key) const {
    const auto seed = crafting::station_seed(key);
    if (!seed || !registry_) return nullptr;
    const world::WorldActor* found = nullptr;
    registry_->for_each([&](const world::WorldActor& a) {
        if (!found && a.kind == world::ActorKind::Station && a.station.seed == *seed) found = &a;
    });
    return found;
}

int64_t PlayerSession::next_job_event(int64_t now_ms) const {
    int64_t next = std::numeric_limits<int64_t>::max();
    for (const svc::ProcessingJob& j : jobs_) {
        const auto seen = job_seen_.find(j.job_guid);
        if (seen == job_seen_.end() || seen->second != crafting::status_at(j, now_ms)) return 0;   // a change is waiting to be sent
        if (j.started_at_ms > now_ms) next = std::min(next, j.started_at_ms);
        if (j.end_ms() > now_ms) next = std::min(next, j.end_ms());
    }
    return next;
}

void PlayerSession::run_crafting(Connection& conn, Clock::time_point now) {
    if (failed_ || finished_ || !join_.empty() || jobs_.empty()) return;
    const int64_t now_ms = wall_ms();
    if (now_ms < next_job_event_ms_) return;
    // the jobs whose status changed since the window was last told of them, by station, in the order the jobs were queued
    struct Group {
        std::string                       key;
        std::vector<repl::CraftItemState> states;
    };
    std::vector<Group> groups;
    std::vector<std::pair<std::string, uint8_t>> told;
    size_t completed = 0, began = 0;
    const double sync = sync_now(now);
    for (const svc::ProcessingJob& j : jobs_) {
        const uint8_t status = crafting::status_at(j, now_ms);
        const auto seen = job_seen_.find(j.job_guid);
        if (seen == job_seen_.end()) {
            job_seen_[j.job_guid] = status;
            continue;
        }
        if (seen->second == status) continue;
        std::optional<crafting::JobRewards> made;
        if (status == crafting::kComplete) made = job_rewards(j);
        const auto state = crafting::to_state(j, now_ms, sync, made ? &*made : nullptr);
        if (!state || state->status != status) continue;   // (it cannot be shown so yet: the next tick looks again)
        auto g = std::find_if(groups.begin(), groups.end(), [&](const Group& x) { return x.key == j.station_key; });
        if (g == groups.end()) {
            groups.push_back({j.station_key, {}});
            g = groups.end() - 1;
        }
        g->states.push_back(*state);
        told.emplace_back(j.job_guid, status);
        (status == crafting::kComplete ? completed : began) += 1;
    }
    if (groups.empty()) {
        next_job_event_ms_ = next_job_event(now_ms);
        return;
    }
    size_t steps = 1;
    for (const Group& g : groups) steps += (g.states.size() + bootstrap::kMaxEventsPerChain - 1) / bootstrap::kMaxEventsPerChain;
    if (!room_for(steps)) return;   // (the replies are full: the next tick tries again)
    // the player's list of active stations first (a station with no job left running leaves it), then the window's states
    if (sync_active_stations(now_ms)) queue("OwnerInfo", [](bootstrap::PlayerBootstrap& pb) { return pb.currencies_update(); }, conn, /*must_build=*/true);
    size_t sent = 0;
    for (const Group& g : groups) {
        // the window can only be open at a station the client has: for any other the next open shows the jobs as they are
        const world::WorldActor* a = station_by_key(g.key);
        if (!a || !view_ || view_->state_of(a->id) != world::ClientState::Awake) continue;
        const FIntrepidNetworkGUID guid = a->actor.guid;
        for (size_t at = 0; at < g.states.size(); at += bootstrap::kMaxEventsPerChain) {
            const std::vector<repl::CraftItemState> part(g.states.begin() + static_cast<std::ptrdiff_t>(at),
                                                         g.states.begin() + static_cast<std::ptrdiff_t>(std::min(g.states.size(), at + bootstrap::kMaxEventsPerChain)));
            queue("craft update", [guid, part](bootstrap::PlayerBootstrap& pb) { return pb.craft_states_update(guid, part); }, conn, /*must_build=*/true);
        }
        sent += g.states.size();
    }
    for (const auto& [guid, status] : told) job_seen_[guid] = status;
    next_job_event_ms_ = next_job_event(now_ms);
    constexpr size_t kUpdatesLogged = 30;
    spdlog::log(level_for(craft_update_logged_, kUpdatesLogged), "[session] {}: {} job(s) ended and {} began: {} state(s) sent to the window{}", name(), completed, began, sent,
                sent == told.size() ? "" : " (the rest are at stations the client does not have open: the next open shows them)");
}

void PlayerSession::on_processing_request(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    constexpr size_t kClaimLogged = 30;   // a player collects a few jobs: each is a line in the log, up to this many
    constexpr const char* kNoRoomText = "Not enough inventory space.";
    constexpr int32_t kCalloutType = 1, kCalloutPointSize = 32;
    constexpr std::chrono::seconds kCalloutLife{3};
    constexpr auto kBetweenProducts = std::chrono::milliseconds(75);   // retail: the second product's items 75 ms after the first's
    if (dead_) return;
    const auto req = repl::parse_processing_request(rpc.call);
    if (!req) {
        if (!claim_unreadable_logged_) {
            claim_unreadable_logged_ = true;
            spdlog::warn("[session] {}: a ServerSendProcessingStationRequest does not read ({} parameter bits): ignored (further ones are not logged)", name(), rpc.call.param_bits);
        }
        return;
    }
    if (!within_budget(vendor_budget_, "ServerSendProcessingStationRequest", now)) return;
    const auto refuse = [&](const std::string& why) {
        ++claims_refused_;
        spdlog::log(level_for(claim_refused_logged_, kClaimLogged), "[session] {}: a request at a station is refused (nothing is changed): {}", name(), why);
    };
    if (req->type == crafting::kCancelled) return refuse("cancelling a job is not made yet: the job runs on");
    if (req->type != crafting::kClaimed) return refuse("a request of type " + std::to_string(req->type) + ": only a claim (type " + std::to_string(crafting::kClaimed) + ") is served");
    if (!registry_ || !view_ || !world_) return refuse("the world has no actors");
    const world::WorldActor* a = registry_->find(req->station.ObjectId);
    if (!a || a->kind != world::ActorKind::Station) return refuse("the object named is no station of this world");
    if (view_->state_of(a->id) != world::ClientState::Awake) return refuse("the client does not have that station open");
    const GoodPosition at = checker_.good();
    const double dx = a->x - at.x, dy = a->y - at.y, dz = a->z - at.z;
    const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (distance >= crafting::kStationRangeCm) return refuse("the station " + a->station.name + " is " + std::to_string(static_cast<int>(distance / 100.0)) + " m away");
    if (!a->station.processing) return refuse(a->station.name + " is a station that crafts at once: it holds no jobs to collect");
    const std::string key = crafting::station_key(a->station.seed);
    if (key.empty()) return refuse(a->station.name + " has no key that survives a restart: it holds no jobs");
    const content::CraftRecipeData* recipes = world_->craft_recipes();
    if (!recipes) return refuse("worldd has no recipes (an older content.db)");
    // the job: found by its guid among the player's own at this station; what the client sends back of it (its products, its experience) is never read
    const std::string job_guid = items::guid_hex(req->state.job);
    const svc::ProcessingJob* found = crafting::find_job(jobs_, key, job_guid);
    if (!found) return refuse("the player has no job " + job_guid + " at " + a->station.name);
    const int64_t now_ms = wall_ms();
    if (crafting::unfinished(*found, now_ms)) {
        return refuse("the job is not over: " + std::to_string((found->end_ms() - now_ms + 999) / 1000) + " s to go");
    }
    const svc::ProcessingJob job = *found;
    const auto recipe_it = recipes->recipes.find(job.recipe_guid);
    const std::optional<crafting::JobRewards> made = job_rewards(job);
    if (recipe_it == recipes->recipes.end() || !made) return refuse("recipe " + std::to_string(job.recipe_guid) + " of the job is none of the data's");
    const crafting::JobRewards& rewards = *made;
    if (const content::CraftReward* table = recipes->reward_of(recipe_it->second); table && !table->unknown_formula.empty() && station_told_.insert("xp " + table->unknown_formula).second) {
        spdlog::info("[session] {}: {} pays its experience by the formula {}, which this server does not work out: none is paid for it (told once)", name(), recipe_it->second.name, table->unknown_formula);
    }
    if (req->warehouse && station_told_.insert("warehouse claim").second) {
        spdlog::info("[session] {}: a claim asks for the warehouse: none exists yet, the items go to the bags (told once)", name());
    }

    // what the job made, as stacks of the item's size: one call of the answer for each product, the last product first (retail's order)
    const content::ContentDb* db = world_->content();
    const int64_t created = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() * 1000;
    std::vector<svc::InventoryItem> fresh;                    // every stack, product after product
    std::vector<std::vector<svc::InventoryItem>> by_product;  // ... and each product's own
    std::map<int64_t, std::string> shown_name;
    for (size_t at_product = rewards.products.size(); at_product-- > 0;) {
        const crafting::Product& p = rewards.products[at_product];
        if (p.quantity < 1) continue;   // (a line with no unit has no stack: an empty entry would make the grant refuse and the job could never be collected)
        const auto info = item_catalog().info(p.item);
        if (!info) return refuse("the item table does not know product " + std::to_string(p.item) + " of " + recipe_it->second.name);
        std::string error;
        const auto rec = db ? db->item(p.item, error) : std::nullopt;
        if (rec) shown_name[p.item] = rec->display_name.empty() ? rec->name : rec->display_name;
        const int64_t max_stack = std::max(1, info->max_stack);
        by_product.emplace_back();
        for (int64_t left = p.quantity; left > 0;) {
            const int64_t take = std::min(left, max_stack);
            svc::InventoryItem it;
            it.container = svc::ItemContainer::Backpack;
            it.item_record_id = p.item;
            it.instance_guid = fresh_guid();
            for (const svc::InventoryItem& other : fresh) {
                if (other.instance_guid == it.instance_guid) it.instance_guid = fresh_guid();   // (fresh_guid looks at the inventory only)
            }
            it.quantity = static_cast<int32_t>(take);
            it.quality = p.quality > 0 ? p.quality : rewards.quality;
            it.durability = rec ? rec->max_durability_at(it.quality) : 0.0f;
            it.created_at_ms = created;
            by_product.back().push_back(it);
            fresh.push_back(std::move(it));
            left -= take;
        }
    }
    // the room, all or nothing: with none the player is told so and the job stays as it is
    if (!fresh.empty() && !inventory_.can_grant(fresh, item_catalog())) {
        const uint64_t key_of_callout = (static_cast<uint64_t>(pb_.setup().player_id) << 32) | (0x40000000u + gather_callout_seq_++);
        const int64_t ticks = items::ticks_of_ms(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        if (room_for(2)) {
            queue("a callout", [ticks, key_of_callout, kNoRoomText](bootstrap::PlayerBootstrap& pb) { return pb.callout_add(kNoRoomText, kCalloutType, kCalloutPointSize, ticks, key_of_callout); }, conn);
            gather_callouts_.push_back({now + kCalloutLife, key_of_callout});
        }
        return refuse("the bags have no room for the " + std::to_string(fresh.size()) + " stack(s) of " + recipe_it->second.name + " (the player was told; the job stays)");
    }
    size_t steps = 4 + rewards.products.size() + 8;
    if (!room_for(steps)) return;

    // the items into the bags, a product after the other (the room was checked: a refusal here is a record the table refuses, not something the client can cause)
    const items::Inventory before_grant = inventory_;
    std::vector<items::Outcome> granted;
    for (std::vector<svc::InventoryItem>& stacks : by_product) {
        items::Outcome o = inventory_.grant(stacks, item_catalog());
        if (!o.ok) {
            spdlog::error("[session] {}: the items of {} are not granted ({}): the job stays", name(), recipe_it->second.name, o.refusal);
            inventory_ = before_grant;
            return;
        }
        granted.push_back(std::move(o));
    }
    // the job is gone: the experience is paid once
    const std::string station_name = a->station.name;
    const FIntrepidNetworkGUID station_guid = a->actor.guid;
    const auto claimed = crafting::claimed_state(job, now_ms, sync_now(now));
    jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [&](const svc::ProcessingJob& j) { return j.job_guid == job_guid; }), jobs_.end());
    job_seen_.erase(job_guid);
    next_job_event_ms_ = 0;
    inventory_dirty_ = true;
    inventory_changed_ = true;
    ++items_applied_;
    ++crafts_claimed_;

    // the experience: the profession's, then Adventuring's (the debt first), as a harvest pays it
    earn_node_experience(rewards.adventuring_xp + rewards.profession_xp, "a processing job");   // Nodes 2
    professions::Professions after = professions_;
    if (professions_.valid() && rewards.profession_xp > 0 && rewards.profession != 0) after.add_xp(rewards.profession, rewards.profession_xp);
    const std::vector<professions::Change> changes = professions_.valid() ? set_professions(after) : std::vector<professions::Change>();
    const leveling::LevelCurve* curve = world_->level_curve();
    const std::span<const content::AutoGrant> grant_list = world_->auto_grants();
    leveling::XpResult xp;
    xp.before = xp.after = progress_;
    if (curve && rewards.adventuring_xp > 0) {
        xp = leveling::add_xp(*curve, grant_list, progress_, rewards.adventuring_xp);
        progress_ = xp.after;
        progress_changed_ = true;
    }
    const bool leveled = xp.leveled();
    if (leveled) levels_gained_ += static_cast<size_t>(xp.after.level - xp.before.level);
    bootstrap::AbilityChange rank_effects;
    std::vector<repl::SkillOnActionbar> skill_updates;
    if (leveled) finish_level_up(xp, false, now, rank_effects, skill_updates);

    // what the client is told, in the retail order: the job at Claimed with the first product's items, the other products' items 75 ms apart, then the experience and the level-ups
    // one call of events for each product: its stacks' Adds (a stack that was topped up is the Remove of the old stack and the Add of the new total, kept together)
    Clock::time_point due = now;
    for (size_t i = 0; i < granted.size(); ++i) {
        std::vector<bootstrap::ItemEvent> batch = item_events_for(granted[i]);
        add_item_toasts(batch, /*removals=*/false);
        if (i == 0) {
            const repl::CraftItemState state = claimed ? *claimed : repl::CraftItemState{};
            queue_later("craft claimed", [station_guid, state, batch](bootstrap::PlayerBootstrap& pb) { return pb.craft_claimed_answer(station_guid, state, batch); }, conn, due, /*must_build=*/true);
        } else {
            due += kBetweenProducts;
            queue_later("craft items", [batch](bootstrap::PlayerBootstrap& pb) { return pb.craft_batched_events(batch); }, conn, due, /*must_build=*/true);
        }
    }
    if (granted.empty() && claimed) {   // (a job that made no item)
        const repl::CraftItemState state = *claimed;
        queue_later("craft claimed", [station_guid, state](bootstrap::PlayerBootstrap& pb) { return pb.craft_claimed_answer(station_guid, state, {}); }, conn, due, /*must_build=*/true);
    }
    if (xp.changed() || leveled || !changes.empty()) {
        const int64_t total = progress_.xp, debt = progress_.debt;
        const std::vector<ProfessionWire> wire = profession_wire(changes);
        queue_later("XPTracker", [total, debt, wire](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> {
            if (!pb.set_progress(total, debt)) return std::nullopt;
            for (const ProfessionWire& w : wire) {
                if (!pb.set_profession(w.leveling_type, w.xp, w.certification)) return std::nullopt;
            }
            return pb.leveling_update();
        }, conn, due, /*must_build=*/true);
    }
    if (leveled) {
        const int level = progress_.level;
        queue_later("BroadcastPlayLevelUpEffects", [level](bootstrap::PlayerBootstrap& pb) { return pb.level_up_broadcast(content::kGuidAdventuring, level); }, conn, due);
    }
    if (!changes.empty()) {
        for (const professions::Change& c : changes) {
            const professions::Profession* pf = professions_.book()->find(c.profession);
            if (!pf || !c.leveled()) continue;
            const int64_t category = pf->leveling_type;
            const int level = c.after.level;
            queue_later("BroadcastPlayLevelUpEffects", [category, level](bootstrap::PlayerBootstrap& pb) { return pb.level_up_broadcast(category, level); }, conn, due);
        }
    }
    if (leveled) queue_later("pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("level and stats"); }, conn, due, /*must_build=*/true);
    for (const repl::SkillOnActionbar& u : skill_updates) {
        queue_later("ClientUpdateSkillOnActionbar", [u](bootstrap::PlayerBootstrap& pb) { return pb.skill_on_actionbar(u); }, conn, due, /*must_build=*/true);
    }

    // the job is deleted and the items are in the bags: saved together
    hand_over_inventory("a job was collected", /*periodic=*/false);
    if (!changes.empty() || xp.changed()) hand_over_progress(leveled ? "a level-up" : "a job was collected");

    std::string line = "collected " + recipe_it->second.name + " x" + std::to_string(job.quantity) + " at " + station_name + " (" + key + "): ";
    for (size_t i = 0; i < rewards.products.size(); ++i) {
        line += (i ? ", " : "") + (shown_name.count(rewards.products[i].item) ? shown_name[rewards.products[i].item] : std::to_string(rewards.products[i].item)) + " x" +
                std::to_string(rewards.products[i].quantity) + " (q" + std::to_string(rewards.products[i].quality > 0 ? rewards.products[i].quality : rewards.quality) + ")";
    }
    line += " at base quality " + std::to_string(rewards.quality) + " in " + std::to_string(fresh.size()) + " stack(s)";
    if (rewards.ratings.any()) {
        line += "; artisan ratings of the start: quantity " + std::to_string(std::lround(rewards.ratings.quantity)) + " (" + std::to_string(rewards.extra_units) + " extra unit(s)), rarity " +
                std::to_string(std::lround(rewards.ratings.rarity)) + " (" + std::to_string(rewards.stepped_units) + " step(s) up), experience " + std::to_string(std::lround(rewards.ratings.xp)) + " (x" +
                fmt::format("{:.3f}", rewards.xp_multiplier) + ")";
    }
    for (const professions::Change& c : changes) {
        const professions::Profession* pf = professions_.book()->find(c.profession);
        if (pf) line += "; " + pf->name + " +" + std::to_string(c.after.xp - c.before.xp) + " XP (now " + std::to_string(c.after.xp) + ", level " + std::to_string(c.after.level) + ")";
    }
    if (rewards.adventuring_xp > 0) {
        line += "; Adventuring +" + std::to_string(rewards.adventuring_xp) + " XP offered (debt " + std::to_string(xp.before.debt) + " -> " + std::to_string(xp.after.debt) + ", total " + std::to_string(xp.after.xp) +
                (leveled ? ", level " + std::to_string(xp.before.level) + " -> " + std::to_string(xp.after.level) : std::string()) + ")";
    }
    spdlog::log(level_for(claim_logged_, kClaimLogged), "[session] {}: {}", name(), line);
}

size_t PlayerSession::gm_finish_jobs(Connection& conn, Clock::time_point now, const std::string& station_name, const std::string& by) {
    const int64_t now_ms = wall_ms();
    size_t finished = 0;
    if (station_name.empty()) {
        finished = crafting::finish_now(jobs_, std::string(), now_ms);
    } else {
        const auto same = [](const std::string& a, const std::string& b) {
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
        };
        std::vector<std::string> keys;
        for (const svc::ProcessingJob& j : jobs_) {
            if (std::find(keys.begin(), keys.end(), j.station_key) != keys.end()) continue;
            const world::WorldActor* a = station_by_key(j.station_key);
            if (!a) continue;
            const content::CraftStationData* defs = world_ ? world_->station_data() : nullptr;
            const auto def = defs ? defs->defs.find(a->station.def) : decltype(defs->defs.end()){};
            if (same(a->station.name, station_name) || (defs && def != defs->defs.end() && same(def->second.name, station_name))) keys.push_back(j.station_key);
        }
        for (const std::string& key : keys) finished += crafting::finish_now(jobs_, key, now_ms);
    }
    if (finished == 0) return 0;
    crafts_finished_ += finished;
    next_job_event_ms_ = 0;
    inventory_dirty_ = true;
    inventory_changed_ = true;
    hand_over_inventory("jobs were finished by a game master", /*periodic=*/false);
    spdlog::info("[session] {}: {} job(s) finished at once by {}{}", name(), finished, by, station_name.empty() ? "" : " at " + station_name);
    run_crafting(conn, now);   // the window is told now
    return finished;
}

/// The client calls of processing jobs (session/rpc_table.h).
void PlayerSession::register_processing_rpcs(RpcTable& t) {
    t.add(RpcScope::PlayerController, "ServerSendProcessingStationRequest", [](PlayerSession& s, const RpcContext& c) { s.on_processing_request(c.conn, c.rpc, c.now); });
}

}}  // namespace aoc::worldd
