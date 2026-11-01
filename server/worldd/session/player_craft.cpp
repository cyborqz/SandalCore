// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_craft.cpp
//
//  Crafting at the stations: opening a station's window, starting a processing job, the quick make, interrupting a craft, and the recipes a game master teaches.
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

// -- crafting, step 0: the stations --------------------------------------------------------------------------------------------------------------------------

void PlayerSession::on_station_interact(Connection& conn, const world::WorldActor& a, Clock::time_point now) {
    constexpr size_t kStationLogged = 30;   // a player tries a handful of stations: each click is a line in the log, up to this many of each kind
    const auto ignore = [&](const std::string& why) {
        ++interact_ignored_;
        spdlog::log(level_for(station_ignored_logged_, kStationLogged), "[session] {}: a click on the station {} is ignored: {}", name(), a.station.name, why);
    };
    if (view_->state_of(a.id) != world::ClientState::Awake) return ignore("the client does not have that station open");
    const GoodPosition at = checker_.good();
    const double dx = a.x - at.x, dy = a.y - at.y, dz = a.z - at.z;
    const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (distance >= crafting::kStationRangeCm) {
        return ignore("the station is " + std::to_string(static_cast<int>(distance / 100.0)) + " m away (a window opens within " + std::to_string(static_cast<int>(crafting::kStationRangeCm / 100.0)) + " m)");
    }
    if (!room_for(1)) return;
    const protocol::repl::StationTax tax = crafting::station_tax(a.station.tax_tag);
    const FIntrepidNetworkGUID guid = a.actor.guid;
    ++station_opens_;
    // what this step cannot put in the window is said once per kind: the recipes, prices and certification are the client's own; the player's known recipes (OwnerInfo) and jobs are not sent yet
    if (station_told_.insert("known recipes").second) {
        spdlog::info("[session] {}: a station window opens with the client's own recipe lists: the server sends no known recipes (told once)", name());
    }
    if (a.station.def == 0 && station_told_.insert("no def " + a.station.name).second) {
        spdlog::info("[session] {}: the station {} has no definition in the data: its window opens with the client's own recipes and the default tax", name(), a.station.name);
    }
    // crafting, steps 1 and 2: the player's own jobs at this station, in the order they were queued; a finished one is shown Complete with what it made
    const int64_t now_ms = wall_ms();
    std::vector<repl::CraftItemState> states;
    if (const std::string key = crafting::station_key(a.station.seed); !key.empty()) {
        states = crafting::states_at(jobs_, key, now_ms, sync_now(now), [this](const svc::ProcessingJob& j) { return job_rewards(j); });
        for (const repl::CraftItemState& s : states) {   // (the window is told what the jobs are now: later changes are sent as updates)
            if (const std::string guid = items::guid_hex(s.job); job_seen_.count(guid) != 0) job_seen_[guid] = s.status;
        }
    }
    spdlog::log(level_for(station_logged_, kStationLogged), "[session] {}: opens the window of {} in node {} ({} m away, tax {:.2f} of the price, the owning node keeps {:.2f}; {} job(s) of the player's here)", name(),
                a.station.name, a.station.node, static_cast<int>(distance / 100.0), tax.percent, tax.owning_share, states.size());
    queue("ClientOpenCraftingStationUI", [guid, tax, states](bootstrap::PlayerBootstrap& pb) { return pb.open_station(guid, tax, states); }, conn, /*must_build=*/true);
}

// -- crafting, step 1: starting a processing job -------------------------------------------------------------------------------------------------------------

bool PlayerSession::sync_active_stations(int64_t now_ms) {
    const std::vector<std::array<int32_t, 4>> stations = crafting::active_station_words(jobs_, now_ms);
    if (stations.empty() && !active_told_) return false;   // nothing was ever told and nothing is: the property stays out of the state
    if (stations == active_words_ && active_told_) return false;
    if (!pb_.set_active_stations(stations)) return false;
    active_words_ = stations;
    active_told_ = true;
    return true;
}

void PlayerSession::on_start_craft(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    constexpr size_t kCraftLogged = 30;   // a player starts a few jobs: each is a line in the log, up to this many
    if (dead_) return;
    const auto req = repl::parse_start_craft(rpc.call);
    if (!req) {
        if (!vendor_unreadable_logged_) {
            vendor_unreadable_logged_ = true;
            spdlog::warn("[session] {}: a ServerStartCraft does not read ({} parameter bits): ignored, and the client is told to stop crafting (further ones are not logged)", name(), rpc.call.param_bits);
        }
        // (a quick make's client marked itself as crafting when it sent the start, and a start this server cannot read, such as one that names materials by tags, never answers: it is told to stop)
        if (!quick_.active && within_budget(vendor_budget_, "ServerStartCraft", now) && room_for(2)) {
            queue("ClientInterruptCraft", [](bootstrap::PlayerBootstrap& pb) { return pb.craft_interrupted(); }, conn, /*must_build=*/true);
        }
        return;
    }
    if (!within_budget(vendor_budget_, "ServerStartCraft", now) || !room_for(8)) return;
    bool quick = false;   // the station crafts at once (set below): its refusals also stop the client's own craft
    const auto refuse = [&](const std::string& why) {
        if (quick) {   // (the quick make has its own count and its own log lines)
            ++quick_refused_;
            spdlog::log(level_for(quick_refused_logged_, kCraftLogged), "[session] {}: a craft is refused (nothing is taken): {}{}", name(), why, quick_.active ? " (the craft that runs goes on)" : "");
            if (!quick_.active) queue("ClientInterruptCraft", [](bootstrap::PlayerBootstrap& pb) { return pb.craft_interrupted(); }, conn, /*must_build=*/true);
            return;
        }
        ++crafts_refused_;
        spdlog::log(level_for(craft_refused_logged_, kCraftLogged), "[session] {}: a start at a station is refused (nothing is taken, nothing is sent): {}", name(), why);
    };
    if (!registry_ || !view_ || !world_) return refuse("the world has no actors");
    const world::WorldActor* a = registry_->find(req->station.ObjectId);
    if (!a || a->kind != world::ActorKind::Station) return refuse("the object named is no station of this world");
    quick = !a->station.processing;
    if (view_->state_of(a->id) != world::ClientState::Awake) return refuse("the client does not have that station open");
    const GoodPosition at = checker_.good();
    const double dx = a->x - at.x, dy = a->y - at.y, dz = a->z - at.z;
    const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (distance >= crafting::kStationRangeCm) return refuse("the station " + a->station.name + " is " + std::to_string(static_cast<int>(distance / 100.0)) + " m away");
    if (quick) {
        if (quick_.active) return refuse("a craft of this player runs already (at " + quick_.station_name + "): a second start does nothing");
        return on_quick_start(conn, *req, *a, now);
    }
    const std::string key = crafting::station_key(a->station.seed);
    if (key.empty()) return refuse(a->station.name + " has no key that survives a restart: no job can be saved there");
    const content::CraftStationData* defs = world_->station_data();
    const content::CraftRecipeData* recipes = world_->craft_recipes();
    if (!defs || !recipes) return refuse("worldd has no station definitions or recipes (an older content.db)");
    const auto def = defs->defs.find(a->station.def);
    if (a->station.def == 0 || def == defs->defs.end()) return refuse("the data holds no definition for " + a->station.name);
    const int64_t now_ms = wall_ms();
    crafting::StartFacts facts;
    facts.recipes = recipes;
    facts.station = &def->second;
    facts.taught = [this](int64_t recipe) { return std::find(taught_.begin(), taught_.end(), recipe) != taught_.end(); };
    facts.tax = crafting::station_tax(a->station.tax_tag);
    facts.node.guid = a->station.node;
    if (world_->city_nodes() && world_->node_wire()) {
        if (const world::CityNodeInfo* node = world_->city_nodes()->by_guid(a->station.node)) {
            facts.node.level = world_->city_nodes()->level_of(node->guid);
            facts.node.biome = spawns::node_site_of(*world_->city_nodes(), *world_->node_wire(), *node, facts.node.level).biome;
        }
    }
    facts.tier_of = [this](int64_t profession) { return professions_.valid() ? professions_.tier(profession) : 0; };
    facts.inventory = &inventory_;
    facts.rarity_facts = [this](int64_t item) { return world_->item_rarity_facts(item); };   // (the rarity the client shows for a stack: the inputs are found by it)
    facts.copper = wallet_.copper();
    facts.rating_stat = [this](int64_t stat) { return artisan_stat(stat); };   // crafting quality: the job's time reads the character's processing time ratings
    facts.unfinished_here = crafting::unfinished_at(jobs_, key, now_ms);
    facts.held_here = crafting::held_at(jobs_, key);
    if (jobs_.size() >= crafting::kMaxJobsHeld) return refuse("the character holds " + std::to_string(jobs_.size()) + " jobs already");
    const crafting::Plan plan = crafting::plan_start(*req, facts);
    if (!plan.ok) return refuse(plan.refusal);
    // the stacks leave the material bags: the reagent first, then what the player named (the retail order); any failure puts the bags back as they were
    const items::Inventory kept = inventory_;
    items::Outcome taken;
    taken.ok = true;
    for (const crafting::Take& t : plan.takes) {
        items::Outcome o = inventory_.take_materials(t.record, t.facts, t.rarity_lo, t.rarity_hi, t.count, t.lowest_first);
        if (!o.ok) {
            inventory_ = kept;
            return refuse(o.refusal);
        }
        taken.events.insert(taken.events.end(), std::make_move_iterator(o.events.begin()), std::make_move_iterator(o.events.end()));
    }
    if (taken.events.size() > 8 * bootstrap::kMaxEventsPerChain) {
        inventory_ = kept;
        return refuse("the job takes from " + std::to_string(taken.events.size()) + " stacks: more events than one answer holds");
    }
    svc::ProcessingJob job = crafting::make_job(jobs_, key, plan.recipe->guid, req->quantity, plan.duration_ms, req->transaction, crafting::job_resources_of(plan.paid), now_ms, plan.ratings);   // (the ratings are saved with the job)
    const auto state = crafting::to_state(job, now_ms, sync_now(now));
    if (!state) {   // (the guids were made here: cannot happen)
        inventory_ = kept;
        return refuse("the job's guids do not read");
    }
    const int64_t before = wallet_.copper();
    if (plan.cost > 0) wallet_.pay(plan.cost);   // checked by the plan: it cannot be short
    job_seen_[job.job_guid] = state->status;   // (the answer below tells the window this status: a later change is sent by run_crafting)
    jobs_.push_back(std::move(job));
    next_job_event_ms_ = 0;
    inventory_dirty_ = true;
    inventory_changed_ = true;
    ++items_applied_;
    ++crafts_started_;
    // the answer in the retail order: the Remove events (each with its "Removed" callout), then one tick with the active stations and the new balance, the money callout and the new job's state
    std::vector<bootstrap::ItemEvent> events = item_events_for(taken);
    add_item_toasts(events, /*removals=*/true);
    const FIntrepidNetworkGUID station_guid = a->actor.guid;
    const int64_t delta = -plan.cost;
    const repl::CraftItemState shown = *state;
    constexpr auto kTickAfterRemoves = std::chrono::milliseconds(40);   // retail: the Removes at +79 ms, the tick at +121 ms
    for (size_t at_event = 0; at_event < events.size(); at_event += bootstrap::kMaxEventsPerChain) {
        const std::vector<bootstrap::ItemEvent> part(events.begin() + static_cast<std::ptrdiff_t>(at_event),
                                                     events.begin() + static_cast<std::ptrdiff_t>(std::min(events.size(), at_event + bootstrap::kMaxEventsPerChain)));
        queue_later("ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, conn, now, /*must_build=*/true);
    }
    pb_.set_currency(items::kCopperCurrency, wallet_.copper());
    sync_active_stations(now_ms);
    queue_later("OwnerInfo", [](bootstrap::PlayerBootstrap& pb) { return pb.currencies_update(); }, conn, now + kTickAfterRemoves, /*must_build=*/true);
    queue_later("craft started", [station_guid, delta, shown](bootstrap::PlayerBootstrap& pb) { return pb.craft_started_answer(station_guid, items::kMoneyTier, delta, shown); }, conn,
                now + kTickAfterRemoves, /*must_build=*/true);
    spdlog::log(level_for(craft_logged_, kCraftLogged),
                "[session] {}: started {} x{} at {} ({}): {} copper ({} each with the tax of {:.2f}, {} before it) leaves the purse ({} -> {}), {} stack(s) leave the material bags, the job runs {:.0f} s{} {}", name(),
                plan.recipe->name, req->quantity, a->station.name, key, plan.cost, plan.unit_taxed, facts.tax.percent, plan.unit_cost, before, wallet_.copper(), taken.events.size(),
                plan.duration_ms / 1000.0,
                plan.ratings.any() ? " (x" + std::to_string(plan.time_multiplier).substr(0, 5) + " of its base time with the ratings: time " + std::to_string(static_cast<int64_t>(std::lround(plan.ratings.time))) + ", quantity " +
                                         std::to_string(static_cast<int64_t>(std::lround(plan.ratings.quantity))) + ", rarity " + std::to_string(static_cast<int64_t>(std::lround(plan.ratings.rarity))) + ", experience " +
                                         std::to_string(static_cast<int64_t>(std::lround(plan.ratings.xp))) + ")"
                                   : std::string(),
                state->status == crafting::kPending ? "after the one before it" : "from now");
}

// -- crafting, step 3: the quick make ------------------------------------------------------------------------------------------------------------------------

namespace {

constexpr std::chrono::milliseconds kQuickTickAfterRemoves{40};   // the product's tick follows the Remove events by this (the Remove events go first)

/// Whether the rarity of a crate follows the materials it was made of: a trade crate's does (the original's recorded Market crate, made of Common stacks, is Common; its recorded Citizen crate, made of three Common stacks and a
/// certificate of quality 51, is Uncommon at quality 6, the first quality of its band), a construction crate's does not (4 of 4 recorded are quality 0, from Common and from Uncommon materials alike, and the crate
/// is worth the same at every rarity).
bool crate_follows_materials(const content::ContentDb* db, int64_t record) {
    if (!db) return false;
    std::string error;
    const auto gear = db->item_gear(record, error);
    if (!gear) return false;
    for (const std::string& tag : gear->tags) {
        std::string lower;
        for (const char c : tag) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower == "item.crate.commodity" || lower.rfind("item.crate.commodity.", 0) == 0) return true;
    }
    return false;
}

/// What a craft's price is counted in, for the log: copper, Glint T1 items (a trade crate of the market, node, harbor and smugglers tables) or the node currency (the citizen table).
const char* price_unit(int64_t tier) { return tier == items::kGlintTier ? "Glint T1" : tier == items::kNodeCurrencyTier ? "node currency" : "copper"; }

}  // namespace

void PlayerSession::on_quick_start(Connection& conn, const repl::StartCraft& req, const world::WorldActor& a, Clock::time_point now) {
    constexpr size_t kQuickLogged = 30;   // a player crafts a few things: each start is a line in the log, up to this many
    const auto refuse = [&](const std::string& why) {
        ++quick_refused_;
        spdlog::log(level_for(quick_refused_logged_, kQuickLogged), "[session] {}: a craft is refused (nothing is taken): {}", name(), why);
        queue("ClientInterruptCraft", [](bootstrap::PlayerBootstrap& pb) { return pb.craft_interrupted(); }, conn, /*must_build=*/true);
    };
    const content::CraftStationData* defs = world_->station_data();
    const content::CraftRecipeData* recipes = world_->craft_recipes();
    if (!defs || !recipes) return refuse("worldd has no station definitions or recipes (an older content.db)");
    const auto def = defs->defs.find(a.station.def);
    if (a.station.def == 0 || def == defs->defs.end()) return refuse("the data holds no definition for " + a.station.name);
    crafting::StartFacts facts;
    facts.recipes = recipes;
    facts.station = &def->second;
    facts.quick = true;
    facts.taught = [this](int64_t recipe) { return std::find(taught_.begin(), taught_.end(), recipe) != taught_.end(); };
    facts.tax = crafting::station_tax(a.station.tax_tag);
    facts.node.guid = a.station.node;
    if (world_->city_nodes() && world_->node_wire()) {
        if (const world::CityNodeInfo* node = world_->city_nodes()->by_guid(a.station.node)) {
            facts.node.level = world_->city_nodes()->level_of(node->guid);
            facts.node.biome = spawns::node_site_of(*world_->city_nodes(), *world_->node_wire(), *node, facts.node.level).biome;
        }
    }
    facts.tier_of = [this](int64_t profession) { return professions_.valid() ? professions_.tier(profession) : 0; };
    facts.inventory = &inventory_;
    facts.rarity_facts = [this](int64_t item) { return world_->item_rarity_facts(item); };   // (the rarity the client shows for a stack: the inputs are found by it)
    facts.copper = wallet_.copper();
    facts.node_currency = wallet_.balance(items::kNodeCurrency);
    const crafting::Plan plan = crafting::plan_start(req, facts);
    if (!plan.ok) return refuse(plan.refusal);
    // (the room for the product is looked at when the craft ends, with the materials already out of the bags)
    const content::CraftReward* reward = recipes->reward_of(*plan.recipe);
    if (!reward || reward->containers.empty()) return refuse(plan.recipe->name + " has no reward the server can give: it makes nothing");
    // a crate is carried in the cargo slot, one at a time: a craft that makes one is refused while the slot is taken
    if (cargo_crate_made(crafting::roll_products(*reward, 1, 0)) && inventory_.at(svc::ItemContainer::Equipment, items::equip::CargoHold)) {
        say(conn, "You are already carrying a crate.", now);
        return refuse("the cargo slot holds a crate already, and " + plan.recipe->name + " makes another: a second crate cannot be carried");
    }
    const GoodPosition at = checker_.good();
    quick_ = crafting::QuickMake{};
    quick_.active = true;
    quick_.station = a.id;
    quick_.station_name = a.station.name;
    quick_.recipe_name = plan.recipe->name;
    quick_.request = req;
    quick_.began = now;
    quick_.ready = now + std::chrono::milliseconds(plan.duration_ms);
    quick_.x = at.x;
    quick_.y = at.y;
    quick_.z = at.z;
    ++quick_started_;
    spdlog::log(level_for(quick_logged_, kQuickLogged), "[session] {}: begins to craft {} at {} ({:.1f} s; nothing is taken until it is made): {} {} ({} each with the tax of {:.2f}), {} stack(s) of materials", name(),
                plan.recipe->name, a.station.name, plan.duration_ms / 1000.0, plan.cost, price_unit(plan.currency), plan.unit_taxed, facts.tax.percent, plan.takes.size());
}

bool PlayerSession::cancel_quick_make(Connection* conn, const std::string& why) {
    if (!quick_.active) return false;
    const std::string recipe = quick_.recipe_name, station = quick_.station_name;
    quick_ = crafting::QuickMake{};
    ++quick_interrupted_;
    spdlog::log(level_for(quick_logged_, 30), "[session] {}: the craft of {} at {} ends with nothing taken: {}", name(), recipe, station, why);
    if (conn) queue("ClientInterruptCraft", [](bootstrap::PlayerBootstrap& pb) { return pb.craft_interrupted(); }, *conn, /*must_build=*/true);
    return true;
}

void PlayerSession::on_interrupt_station_craft(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now) {
    if (!within_budget(vendor_budget_, "ServerInterruptStationCraft", now)) return;
    const auto station = repl::parse_interrupt_craft(rpc.call);
    if (!station) {
        if (!interrupt_unreadable_logged_) {
            interrupt_unreadable_logged_ = true;
            spdlog::warn("[session] {}: a ServerInterruptStationCraft does not read ({} parameter bits): ignored (further ones are not logged)", name(), rpc.call.param_bits);
        }
        return;
    }
    if (!quick_.active) {
        spdlog::debug("[session] {}: ServerInterruptStationCraft with no craft under way: ignored", name());
        return;
    }
    const world::WorldActor* a = registry_ ? registry_->find(station->ObjectId) : nullptr;
    if (a && a->id != quick_.station) {   // (it names another station: this player's craft is at the one it began at; the client only sends this for the station it has open)
        spdlog::debug("[session] {}: ServerInterruptStationCraft names {}, the craft runs at {}: ignored", name(), a->station.name, quick_.station_name);
        return;
    }
    cancel_quick_make(&conn, "the client closed the window or pressed Cancel (ServerInterruptStationCraft)");
}

void PlayerSession::run_quick_make(Connection& conn, Clock::time_point now) {
    if (!quick_.active || failed_ || finished_ || !join_.empty()) return;
    if (dead_) {
        cancel_quick_make(&conn, "the player is dead");
        return;
    }
    if (mounted()) {
        cancel_quick_make(&conn, "the player is on a mount");
        return;
    }
    const world::WorldActor* a = registry_ ? registry_->find(quick_.station) : nullptr;
    if (!a) {
        cancel_quick_make(&conn, "the station left the world");
        return;
    }
    if (!view_ || view_->state_of(a->id) != world::ClientState::Awake) {
        cancel_quick_make(&conn, "the client does not have the station open any more");
        return;
    }
    const GoodPosition at = checker_.good();
    const double sx = a->x - at.x, sy = a->y - at.y, sz = a->z - at.z;
    const double distance = std::sqrt(sx * sx + sy * sy + sz * sz);
    if (distance >= crafting::kStationRangeCm) {
        cancel_quick_make(&conn, "the station is " + std::to_string(static_cast<int>(distance / 100.0)) + " m away");
        return;
    }
    // standing still: measured from where the craft began, so a slow walk adds up
    const double drift = std::sqrt((at.x - quick_.x) * (at.x - quick_.x) + (at.y - quick_.y) * (at.y - quick_.y) + (at.z - quick_.z) * (at.z - quick_.z));
    if (drift > crafting::kQuickDriftCm) {
        cancel_quick_make(&conn, "the player moved " + std::to_string(static_cast<int>(std::lround(drift))) + " cm since the craft began (a craft needs standing still; at most " +
                                     std::to_string(static_cast<int>(crafting::kQuickDriftCm)) + " cm)");
        return;
    }
    if (now < quick_.ready) return;
    if (!room_for(24)) return;   // the replies are full: the next tick tries again
    complete_quick_make(conn, now);   // (a craft that cannot be made now says why in the log and is over)
}

bool PlayerSession::complete_quick_make(Connection& conn, Clock::time_point now) {
    constexpr size_t kMadeLogged = 30;
    constexpr const char* kNoRoomText = "Not enough inventory space.";
    const auto fail = [&](const std::string& why) {
        const std::string recipe = quick_.recipe_name, station = quick_.station_name;
        quick_ = crafting::QuickMake{};
        ++quick_interrupted_;
        spdlog::log(level_for(quick_logged_, 30), "[session] {}: the craft of {} at {} cannot be made now, nothing was taken: {}", name(), recipe, station, why);
        queue("ClientInterruptCraft", [](bootstrap::PlayerBootstrap& pb) { return pb.craft_interrupted(); }, conn, /*must_build=*/true);
        return false;
    };
    const world::WorldActor* a = registry_ ? registry_->find(quick_.station) : nullptr;
    const content::CraftStationData* defs = world_ ? world_->station_data() : nullptr;
    const content::CraftRecipeData* recipes = world_ ? world_->craft_recipes() : nullptr;
    if (!a || !defs || !recipes) return fail("the station or the recipes are gone");
    const auto def = defs->defs.find(a->station.def);
    if (a->station.def == 0 || def == defs->defs.end()) return fail("the data holds no definition for " + a->station.name);
    // the checks run again on what the player holds now (materials, purse, certification, ...)
    crafting::StartFacts facts;
    facts.recipes = recipes;
    facts.station = &def->second;
    facts.quick = true;
    facts.taught = [this](int64_t recipe) { return std::find(taught_.begin(), taught_.end(), recipe) != taught_.end(); };
    facts.tax = crafting::station_tax(a->station.tax_tag);
    facts.node.guid = a->station.node;
    if (world_->city_nodes() && world_->node_wire()) {
        if (const world::CityNodeInfo* node = world_->city_nodes()->by_guid(a->station.node)) {
            facts.node.level = world_->city_nodes()->level_of(node->guid);
            facts.node.biome = spawns::node_site_of(*world_->city_nodes(), *world_->node_wire(), *node, facts.node.level).biome;
        }
    }
    facts.tier_of = [this](int64_t profession) { return professions_.valid() ? professions_.tier(profession) : 0; };
    facts.inventory = &inventory_;
    facts.rarity_facts = [this](int64_t item) { return world_->item_rarity_facts(item); };   // (the rarity the client shows for a stack: the inputs are found by it)
    facts.copper = wallet_.copper();
    facts.node_currency = wallet_.balance(items::kNodeCurrency);
    const repl::StartCraft req = quick_.request;
    const crafting::Plan plan = crafting::plan_start(req, facts);
    if (!plan.ok) return fail(plan.refusal);
    const content::CraftRecipe& recipe = *plan.recipe;

    // 1. the materials leave the bags (the reagent first, then what the player named: the order of the Remove events), 2. the price leaves the purse
    const items::Inventory kept_bags = inventory_;
    const items::Wallet kept_purse = wallet_;
    items::Outcome taken;
    taken.ok = true;
    std::vector<crafting::PaidUnits> paid;   // what the recipe's own materials really were (the reagent is not part of the quality)
    for (const crafting::Take& t : plan.takes) {
        items::Outcome o = inventory_.take_materials(t.record, t.facts, t.rarity_lo, t.rarity_hi, t.count, t.lowest_first);
        if (!o.ok) {
            inventory_ = kept_bags;
            return fail(o.refusal);
        }
        if (!t.lowest_first) {
            for (const items::Event& e : o.events) {
                paid.push_back({t.slot, e.item.quality, e.count > 0 ? e.count : e.item.quantity, world_->item_rarity_facts(e.item.item_record_id).rarity_of(e.item.quality)});
            }
        }
        taken.events.insert(taken.events.end(), std::make_move_iterator(o.events.begin()), std::make_move_iterator(o.events.end()));
    }
    if (taken.events.size() > 8 * bootstrap::kMaxEventsPerChain) {
        inventory_ = kept_bags;
        return fail("the craft takes from " + std::to_string(taken.events.size()) + " stacks: more events than one answer holds");
    }
    const int64_t before = wallet_.copper();
    items::Outcome glint_paid;   // the Glint T1 stacks that pay a price in Glint (the Remove events of the answer)
    if (plan.cost > 0) {   // (checked by the plan: none of them can be short)
        if (plan.currency == items::kGlintTier) {
            glint_paid = inventory_.take(items::kGlintT1Item, plan.cost);
            if (!glint_paid.ok) {
                inventory_ = kept_bags;
                wallet_ = kept_purse;
                return fail(glint_paid.refusal);
            }
            if (glint_paid.events.size() > 8 * bootstrap::kMaxEventsPerChain) {
                inventory_ = kept_bags;
                wallet_ = kept_purse;
                return fail("the Glint comes from " + std::to_string(glint_paid.events.size()) + " stacks: more events than the answers hold");
            }
        } else if (plan.currency == items::kNodeCurrencyTier) {
            wallet_.pay_currency(items::kNodeCurrency, plan.cost);
        } else {
            wallet_.pay(plan.cost);
        }
    }

    // 3. the product: its quality starts from the paid materials' rarity and the crafter's artisan ratings add to it, its experience is the recipe's reward table's times the experience rating's multiplier
    const content::ContentDb* db = world_->content();
    const double crate_mean = crafting::craft_mean_rarity(paid);   // (a trade crate starts where the mean rarity of what it was made of puts it in its band, like a piece of gear, and no rating adds to it)
    const auto facts_of = [db](int64_t item) -> std::optional<content::ItemSaleFacts> {
        if (!db) return std::nullopt;
        std::string error;
        auto f = db->item_sale(item, error);
        if (!f || !f->known) return std::nullopt;
        return f;
    };
    const crafting::Ratings ratings = artisan_ratings(recipe.profession);
    const std::optional<crafting::JobRewards> made = crafting::roll_craft(*recipes, recipe, paid, ratings, progress_.level, crafting::seed_of(items::guid_hex(req.transaction)), facts_of);
    if (!made || made->products.empty()) {
        inventory_ = kept_bags;
        wallet_ = kept_purse;
        return fail(recipe.name + " makes nothing the server knows");
    }
    const crafting::JobRewards& rewards = *made;
    // a crate goes to the cargo slot, never to the bag, and only one is carried: the slot may have been filled since the craft began
    const bool cargo = cargo_crate_made(rewards.products);
    if (cargo && inventory_.at(svc::ItemContainer::Equipment, items::equip::CargoHold)) {
        inventory_ = kept_bags;
        wallet_ = kept_purse;
        say(conn, "You are already carrying a crate.", now);
        return fail("the cargo slot holds a crate already, so " + recipe.name + " cannot be made (the player was told)");
    }
    const int64_t created = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() * 1000;
    std::vector<svc::InventoryItem> fresh;                    // every stack, product after product
    std::vector<std::vector<svc::InventoryItem>> by_product;  // ... and each product's own
    std::map<int64_t, std::string> shown_name;
    for (const crafting::Product& p : rewards.products) {
        const auto info = item_catalog().info(p.item);
        if (!info) {
            inventory_ = kept_bags;
            wallet_ = kept_purse;
            return fail("the item table does not know product " + std::to_string(p.item) + " of " + recipe.name);
        }
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
            if (cargo) {
                // a crate has no quality numbers of its own to add up: a trade crate starts where the mean rarity of the paid stacks puts it in its band (the first quality of that rarity and its share of the
                // band, to the nearest whole number, as a crafted piece has it; its own rarity bounds hold it inside them), a construction crate has the record's own
                it.quality = std::max(1, rec ? rec->default_quality : 1);
                if (crate_follows_materials(db, p.item)) {
                    const items::ItemRarityFacts crate_facts = world_->item_rarity_facts(p.item);
                    it.quality = std::clamp(crafting::quality_from(crafting::craft_base_quality(crate_mean, crate_facts)), 1, crafting::kMaxQuality);
                    const int shown = crate_facts.rarity_of(it.quality);
                    if (shown != crate_facts.band_of(it.quality)) it.quality = items::first_quality_of(shown);
                }
            }
            it.durability = rec ? rec->max_durability_at(it.quality) : 0.0f;
            it.created_at_ms = created;
            by_product.back().push_back(it);
            fresh.push_back(std::move(it));
            left -= take;
        }
    }
    // the room, all or nothing, with the materials already gone from the bags (their cells are free): with none nothing is taken and the player is told so
    if (!cargo && !inventory_.can_grant(fresh, item_catalog())) {
        inventory_ = kept_bags;
        wallet_ = kept_purse;
        say(conn, kNoRoomText, now);
        return fail("the bags have no room for the " + std::to_string(fresh.size()) + " stack(s) of " + recipe.name + " (the player was told)");
    }
    std::vector<items::Outcome> granted;
    if (cargo) {
        items::Outcome o = inventory_.grant_cargo(fresh.front(), item_catalog());
        if (!o.ok) {
            const std::string why = o.refusal;
            inventory_ = kept_bags;
            wallet_ = kept_purse;
            return fail("the crate is not put in the cargo slot: " + why);
        }
        granted.push_back(std::move(o));
    } else {
        for (std::vector<svc::InventoryItem>& stacks : by_product) {
            items::Outcome o = inventory_.grant(stacks, item_catalog());
            if (!o.ok) {
                const std::string why = o.refusal;
                inventory_ = kept_bags;
                wallet_ = kept_purse;
                return fail("the product is not granted: " + why);
            }
            granted.push_back(std::move(o));
        }
    }

    // 4. the craft is made: the experience is paid once, the bags and the purse are saved
    const std::string station_name = quick_.station_name;
    const repl::GuidValue txn = req.transaction;
    quick_ = crafting::QuickMake{};
    inventory_dirty_ = true;
    inventory_changed_ = true;
    ++items_applied_;
    ++quick_made_;
    if (cargo) apply_gear_stats("a crate was made");   // the worn stats (the crate's stat block is among them): before the replies are queued, which are built later and carry the stats' deltas
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

    // 5. what the client is told, in the order the game does it: the Remove events of the materials; then in one tick the money callout and the product's Add events (the last one carries the
    // id of the start, which ends the client's craft); then the purse and the experience
    std::vector<bootstrap::ItemEvent> removes = item_events_for(taken);
    for (bootstrap::ItemEvent& e : removes) e.instigated = true;   // (the character's own guid, as the game names it for a craft)
    add_item_toasts(removes, /*removals=*/true);
    std::vector<bootstrap::ItemEvent> adds;
    for (const items::Outcome& o : granted) {
        std::vector<bootstrap::ItemEvent> part = item_events_for(o);
        adds.insert(adds.end(), std::make_move_iterator(part.begin()), std::make_move_iterator(part.end()));
    }
    add_item_toasts(adds, /*removals=*/false);
    for (auto it = adds.rbegin(); it != adds.rend(); ++it) {
        if (it->type == repl::StorageEventType::Add) {
            it->craft_txn = txn;
            break;
        }
    }
    const int64_t delta = -plan.cost;
    const int64_t tier = plan.currency;
    // a price in Glint: its Remove events come before the callout, with no toast; the answer holds the last of them (a send holds kMaxEventsPerChain events, the crate's Add among them), the ones
    // before are sent ahead in chains of their own
    std::vector<bootstrap::ItemEvent> glint_removes = item_events_for(glint_paid), glint_ahead;
    if (!glint_removes.empty()) {
        const size_t first_adds = std::min(adds.size(), bootstrap::kMaxEventsPerChain);
        const size_t room = bootstrap::kMaxEventsPerChain > first_adds ? bootstrap::kMaxEventsPerChain - first_adds : 0;
        if (glint_removes.size() > room) {
            glint_ahead.assign(glint_removes.begin(), glint_removes.end() - static_cast<std::ptrdiff_t>(room));
            glint_removes.erase(glint_removes.begin(), glint_removes.end() - static_cast<std::ptrdiff_t>(room));
        }
    }
    for (size_t at_event = 0; at_event < removes.size(); at_event += bootstrap::kMaxEventsPerChain) {
        const std::vector<bootstrap::ItemEvent> part(removes.begin() + static_cast<std::ptrdiff_t>(at_event),
                                                     removes.begin() + static_cast<std::ptrdiff_t>(std::min(removes.size(), at_event + bootstrap::kMaxEventsPerChain)));
        queue_later("ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, conn, now, /*must_build=*/true);
    }
    const Clock::time_point tick = now + kQuickTickAfterRemoves;
    for (size_t at_event = 0; at_event < glint_ahead.size(); at_event += bootstrap::kMaxEventsPerChain) {
        const std::vector<bootstrap::ItemEvent> part(glint_ahead.begin() + static_cast<std::ptrdiff_t>(at_event),
                                                     glint_ahead.begin() + static_cast<std::ptrdiff_t>(std::min(glint_ahead.size(), at_event + bootstrap::kMaxEventsPerChain)));
        queue_later("ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, conn, tick, /*must_build=*/true);
    }
    for (size_t at_event = 0; at_event < adds.size(); at_event += bootstrap::kMaxEventsPerChain) {
        const std::vector<bootstrap::ItemEvent> part(adds.begin() + static_cast<std::ptrdiff_t>(at_event),
                                                     adds.begin() + static_cast<std::ptrdiff_t>(std::min(adds.size(), at_event + bootstrap::kMaxEventsPerChain)));
        if (at_event == 0) {
            queue_later("craft made", [tier, delta, part, glint_removes](bootstrap::PlayerBootstrap& pb) {
                return tier == items::kGlintTier ? pb.item_payment_answer(tier, delta, glint_removes, part) : pb.purchase_answer(tier, delta, part);
            }, conn, tick, /*must_build=*/true);
        } else {
            queue_later("craft items", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, conn, tick, /*must_build=*/true);
        }
    }
    pb_.set_currency(items::kCopperCurrency, wallet_.copper());
    pb_.set_currency(items::kNodeCurrency, wallet_.balance(items::kNodeCurrency));
    queue_later("OwnerInfo", [](bootstrap::PlayerBootstrap& pb) { return pb.currencies_update(); }, conn, tick, /*must_build=*/true);
    if (cargo) {   // the crate shows on the carrier's back: the pawn's held items and appearance follow the Add, as for any item put on
        const std::vector<bootstrap::HeldItem> held = bootstrap::held_items_for(inventory_);
        const std::vector<repl::AppearanceId> appearance = items::appearance_ids(inventory_);
        queue_later("HeldItems and AppearanceIDs",
                    [held, appearance](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> {
                        if (!pb.apply_equipment(held, appearance, 1)) return std::nullopt;
                        return pb.equipment_update();
                    },
                    conn, tick, /*must_build=*/true);
        for (const items::Outcome& o : granted) note_cargo_crate(o);   // the quest module is told a construction crate is carried now
        sync_cargo(conn, tick);   // the crate's effects and the Drop Crate slot of the bar follow the Add
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
        }, conn, tick, /*must_build=*/true);
    }
    if (leveled) {
        const int level = progress_.level;
        queue_later("BroadcastPlayLevelUpEffects", [level](bootstrap::PlayerBootstrap& pb) { return pb.level_up_broadcast(content::kGuidAdventuring, level); }, conn, tick);
    }
    for (const professions::Change& c : changes) {
        const professions::Profession* pf = professions_.book()->find(c.profession);
        if (!pf || !c.leveled()) continue;
        const int64_t category = pf->leveling_type;
        const int level = c.after.level;
        queue_later("BroadcastPlayLevelUpEffects", [category, level](bootstrap::PlayerBootstrap& pb) { return pb.level_up_broadcast(category, level); }, conn, tick);
    }
    if (leveled) queue_later("pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("level and stats"); }, conn, tick, /*must_build=*/true);
    for (const repl::SkillOnActionbar& u : skill_updates) {
        queue_later("ClientUpdateSkillOnActionbar", [u](bootstrap::PlayerBootstrap& pb) { return pb.skill_on_actionbar(u); }, conn, tick, /*must_build=*/true);
    }

    // the materials, the price and the product are saved together
    hand_over_inventory("a craft was made", /*periodic=*/false);
    if (!changes.empty() || xp.changed()) hand_over_progress(leveled ? "a level-up" : "a craft was made");

    std::string line = "made " + recipe.name + " at " + station_name + ": ";
    for (size_t i = 0; i < rewards.products.size(); ++i) {
        line += (i ? ", " : "") + (shown_name.count(rewards.products[i].item) ? shown_name[rewards.products[i].item] : std::to_string(rewards.products[i].item)) + " x" +
                std::to_string(rewards.products[i].quantity);
    }
    // the quality: the whole number the item has (1 to 200: the nearest to the exact number the calculation came to, nothing is cut off before) and what it reads as on the community crafting calculator's tier
    // scale (the rarity, the way through its band: 10 in Uncommon is "Uncommon +44.4 %, 2.4444")
    const items::ItemRarityFacts product_facts = world_->item_rarity_facts(rewards.products.front().item);
    const double on_scale = crafting::tier_scale(product_facts, rewards.quality);
    const std::string as_tier = fmt::format("{:.2f}", rewards.quality_exact) + " exactly, nearest whole; " + std::string(items::rarity_name(product_facts.rarity_of(rewards.quality))) + " +" +
                                fmt::format("{:.1f}", (on_scale - std::floor(on_scale)) * 100.0) + " %, " + fmt::format("{:.4f}", on_scale) + " on the tier scale";
    line += (cargo ? " into the cargo slot at quality " + std::to_string(fresh.front().quality) + " (rarity " + std::string(items::rarity_name(world_->item_rarity_facts(fresh.front().item_record_id).rarity_of(fresh.front().quality))) + ")"
                   : " at quality " + std::to_string(rewards.quality) + " (" + as_tier + ") in " + std::to_string(fresh.size()) + " stack(s)") +
            "; " + std::to_string(plan.cost) + " " + price_unit(plan.currency) + " paid" +
            (plan.currency == items::kMoneyTier ? " (" + std::to_string(before) + " -> " + std::to_string(wallet_.copper()) + ")" : std::string()) + ", " + std::to_string(taken.events.size()) +
            " stack(s) of materials taken";
    if (rewards.ratings.any()) {
        line += "; artisan ratings: quality " + std::to_string(std::lround(rewards.ratings.quality)) + " adds " + fmt::format("{:.2f}", rewards.quality_added) + " to the base, experience " +
                std::to_string(std::lround(rewards.ratings.xp)) + " (x" + fmt::format("{:.3f}", rewards.xp_multiplier) + ")";
    }
    for (const professions::Change& c : changes) {
        const professions::Profession* pf = professions_.book()->find(c.profession);
        if (pf) line += "; " + pf->name + " +" + std::to_string(c.after.xp - c.before.xp) + " XP (now " + std::to_string(c.after.xp) + ", level " + std::to_string(c.after.level) + ")";
    }
    if (rewards.adventuring_xp > 0) {
        line += "; Adventuring +" + std::to_string(rewards.adventuring_xp) + " XP offered (debt " + std::to_string(xp.before.debt) + " -> " + std::to_string(xp.after.debt) + ", total " +
                std::to_string(xp.after.xp) + (leveled ? ", level " + std::to_string(xp.before.level) + " -> " + std::to_string(xp.after.level) : std::string()) + ")";
    }
    spdlog::log(level_for(quick_made_logged_, kMadeLogged), "[session] {}: {}", name(), line);
    return true;
}

bool PlayerSession::cargo_crate_made(const std::vector<crafting::Product>& made) const {
    if (made.size() != 1 || made.front().quantity != 1) return false;
    const auto info = item_catalog().info(made.front().item);
    return info && info->cargo_crate();
}

void PlayerSession::say(Connection& conn, const char* text, Clock::time_point now) {
    constexpr int32_t kCalloutType = 1, kCalloutPointSize = 32;
    constexpr std::chrono::seconds kCalloutLife{3};
    if (!room_for(2)) return;
    const uint64_t key_of_callout = (static_cast<uint64_t>(pb_.setup().player_id) << 32) | (0x40000000u + gather_callout_seq_++);
    const int64_t ticks = items::ticks_of_ms(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    queue("a callout", [ticks, key_of_callout, text](bootstrap::PlayerBootstrap& pb) { return pb.callout_add(text, kCalloutType, kCalloutPointSize, ticks, key_of_callout); }, conn);
    gather_callouts_.push_back({now + kCalloutLife, key_of_callout});
}

bool PlayerSession::gm_teach_recipe(Connection& conn, int64_t recipe, const std::string& by) {
    if (std::find(taught_.begin(), taught_.end(), recipe) != taught_.end()) return false;
    if (taught_.size() >= bootstrap::kMaxKnownRecipes) return false;
    taught_.push_back(recipe);
    hand_over_inventory("a recipe was taught by a game master", /*periodic=*/false);
    if (pb_.set_known_recipes(taught_)) queue("OwnerInfo", [](bootstrap::PlayerBootstrap& pb) { return pb.currencies_update(); }, conn, /*must_build=*/true);
    spdlog::info("[session] {}: was taught recipe {} by {} ({} taught in all)", name(), recipe, by, taught_.size());
    return true;
}

bool PlayerSession::gm_forget_recipe(Connection& conn, int64_t recipe, const std::string& by) {
    const auto it = std::find(taught_.begin(), taught_.end(), recipe);
    if (it == taught_.end()) return false;
    taught_.erase(it);
    hand_over_inventory("a taught recipe was taken away by a game master", /*periodic=*/false);
    if (pb_.set_known_recipes(taught_)) queue("OwnerInfo", [](bootstrap::PlayerBootstrap& pb) { return pb.currencies_update(); }, conn, /*must_build=*/true);
    spdlog::info("[session] {}: was made to forget recipe {} by {} ({} taught in all)", name(), recipe, by, taught_.size());
    return true;
}

/// The client calls of crafting stations (session/rpc_table.h).
void PlayerSession::register_craft_rpcs(RpcTable& t) {
    t.add(RpcScope::PlayerController, "ServerStartCraft", [](PlayerSession& s, const RpcContext& c) { s.on_start_craft(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerInterruptStationCraft", [](PlayerSession& s, const RpcContext& c) { s.on_interrupt_station_craft(c.conn, c.rpc, c.now); });
}

}}  // namespace aoc::worldd
