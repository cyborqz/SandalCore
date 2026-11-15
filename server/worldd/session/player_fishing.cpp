// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_fishing.cpp  (Fishing, milestone 1)
//
//  The player's side of fishing: the cast of Economy_FishingCast, its branches and the wire of each step.  The cast's timeline is fishing/cast.h's, where the lure lands and the water's
//  data fishing/fishing.h's; the result of a loop is a harvest like any other (gather/gathering.h's rolls, deliver_harvest's frames), so the bag, the quality, the wear of the rod and
//  the profession's experience are the same code and the same data as mining's.
//
//    the trigger   the client sends ONE ServerTriggerAbility.  It is refused (ClientEndAbility) for an ability the character does not know, a rod it does not wear or that is broken, a rod
//                  its profession may not use, and a cooldown that is not over; a trigger that comes twice with one ClientInstanceId is one cast; a cast while one runs replaces it.  The
//                  lure's landing is planned at once (fishing/fishing.h): on water of a body that names fishing data the loop starts, anywhere else the cast fails
//    the loop      a bite and 4.4 s later the result; the rod wears at every result, a catch or not.  Movement, combat, a mount, the rod gone or broken end it
//    the wire      the ability item (the montage, then its jumps), the Cooldowns item and the Fishing_Buff are written to the pawn as the retail server's are (fishing/cast.h)
//
//  Every refusal and every end is logged with its reason; the first few of each kind at info.  The log lines a test reads start with "[fishing]".
// ============================================================================
#include "session/player_session.h"

#include "bootstrap/constants.h"
#include "fishing/predicate.h"
#include "gather/rules.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;

namespace {

/// How many ClientInstanceIds a session remembers: a replayed trigger is dropped, an old id may come again.
constexpr size_t kRecentIds = 16;
/// The cast is of the spot the player stood on [modelled: the branch tests IsMoving; a teleport or a push moves the player without a move of his own, and the lure would then lie far from
/// him]: a player more than this far (cm, horizontally) from where he cast has walked or been moved away.
constexpr double kDriftCm = 150.0;

/// The equipment slot a reward predicate names ("FishingRod"), as the worn tool's slot number; -1 for a slot that holds no tool.
int tool_slot_of(std::string_view name) {
    if (name == "FishingRod") return items::equip::FishingRod;
    if (name == "Pickaxe") return items::equip::Pickaxe;
    if (name == "Axe") return items::equip::Axe;
    if (name == "Sickle") return items::equip::Sickle;
    if (name == "Bow") return items::equip::Bow;
    return -1;
}

}  // namespace

bool PlayerSession::fishing_takes_trigger(Connection& conn, int64_t ability, int32_t client_instance, Clock::time_point now) {
    if (ability != fishing::kGuidFishingCast) return false;
    const fishing::Fishing* world_fishing = world_ ? world_->fishing() : nullptr;
    AbilityRuntime* runtime = ability_runtime(now);
    if (!world_fishing || !runtime) return false;   // the world does not fish: the generic runtime's cast, as before
    // one trigger, once: the client's own instance id is never reused soon, so a repeat is a replay
    if (std::find(fishing_recent_.begin(), fishing_recent_.end(), client_instance) != fishing_recent_.end()) {
        spdlog::debug("[fishing] {}: the trigger of instance {} came twice: dropped", name(), client_instance);
        return true;
    }
    fishing_recent_.push_back(client_instance);
    while (fishing_recent_.size() > kRecentIds) fishing_recent_.pop_front();

    const double sec = ability_seconds(now);
    const auto refuse = [&](const char* kind, const std::string& why) {
        ++fishing_refused_;
        spdlog::log(level_for(fishing_logged_, 24), "[fishing] {}: the cast is refused ({}): {}", name(), kind, why);
        end_instance(conn, client_instance, false);
        return true;
    };
    if (std::find(known_.begin(), known_.end(), ability) == known_.end()) return refuse("not known", "the character does not know the fishing ability");
    if (dead_) return refuse("dead", "the character is dead");
    if (mounted()) return refuse("mounted", "the character is on a mount");
    gather::Gathering* g = npc_world_ ? npc_world_->gathering() : nullptr;
    if (!g || &world_fishing->gathering() != g) return refuse("no gathering", "this world does not harvest");
    if (sec < fishing_cooldown_until_) return refuse("cooldown", "the fishing cooldown has not ended");
    // the rod: HasValidEquipment(FishingRod), and the requirements any gathering has (its profession, tier and durability)
    const svc::InventoryItem* rod = inventory_.at(svc::ItemContainer::Equipment, items::equip::FishingRod);
    if (!rod) return refuse("no rod", "nothing is worn in the fishing rod slot");
    const GoodPosition at = checker_.good();
    fishing::Landing landing = world_fishing->plan_cast(at.x, at.y, at.z - PlayerWorld::kCapsuleHalfHeightCm, at.yaw_degrees);
    const content::GatherDefRow* def = landing.def ? landing.def : world_fishing->default_def();
    if (!def) return refuse("no fishing data", "the gathering tables have no fishing interaction");
    gather::ToolInfo tool;
    tool.worn = true;
    tool.tool = g->tool(rod->item_record_id);
    tool.durability = rod->durability;
    const gather::Verdict verdict = gather::check_requirements(*def, professions_, tool, stats_ && stats_->in_combat());
    if (!verdict.ok()) return refuse(gather::refusal_name(verdict.why), verdict.text);
    std::string why;
    auto run = gather_run(why);
    if (!run) return refuse("no stats", why);
    double loop_seconds = 0;
    if (landing.lands) {
        const auto seconds = g->channel_seconds(*def, *run, why);
        if (!seconds) return refuse("no duration", why);
        loop_seconds = *seconds;
    }

    // a cast while one runs replaces it: its item and its buff go with the new reply
    FishingRun fresh{fishing::Cast(sec, landing.lands, loop_seconds, world_fishing->config().timing), landing};
    if (fishing_) {
        if (fishing_->cast.active()) {
            ++fishing_ended_;
            spdlog::log(level_for(fishing_logged_, 24), "[fishing] {}: a new cast replaces the one that runs", name());
        }
        if (fishing_->effect_id >= 0) fresh.replaced_effects.push_back(fishing_->effect_id);
    }
    if (fishing_item_ >= 0) fresh.replaced_items.push_back(fishing_item_);
    fishing_item_ = -1;
    fresh.client_id = client_instance;
    fresh.def = landing.lands ? def : nullptr;
    fresh.tool_guid = rod->instance_guid;
    fresh.cast_x = landing.x;
    fresh.cast_y = landing.y;
    fresh.start_x = at.x;
    fresh.start_y = at.y;
    if (landing.water) fresh.water = world_fishing->water().info(landing.water->body).actor;
    fishing_.emplace(std::move(fresh));
    fishing_cooldown_until_ = sec + world_fishing->config().timing.cooldown;
    ++fishing_casts_;
    const std::string rod_name = tool.tool ? tool.tool->item_name : std::string("?");
    if (landing.lands) {
        spdlog::log(level_for(fishing_logged_, 24), "[fishing] {}: casts the line with {}: it comes down {:.0f} cm away on {} ({}, {}), a channel of {:.2f} s (base {:.2f} s)", name(), rod_name,
                    landing.distance_cm, fishing_->water, def->name, landing.water && landing.water->river ? "a river" : "a lake", loop_seconds, def->base_duration);
    } else {
        // where the water is, so that a tester can step to it: the closest swim point of a lake's outline or a river's volumes within 50 m of the player
        std::string hint;
        if (const auto near = world_fishing->water().nearest(at.x, at.y, 5000.0)) {
            hint = fmt::format("; the nearest water is {:.1f} m away, at the map point ({:.0f}, {:.0f}) m", near->distance / 100.0, near->x / 100.0, near->y / 100.0);
        } else {
            hint = "; no fishable water within 50 m";
        }
        spdlog::log(level_for(fishing_logged_, 24), "[fishing] {}: casts the line with {}: {} ({:.0f} cm away): the cast will fail{}", name(), rod_name, landing.why, landing.distance_cm, hint);
    }
    return true;
}

bool PlayerSession::fishing_apply(Connection& conn, bootstrap::AbilityChange& change, const std::string& note) {
    if (change.empty()) return true;
    if (abilities_failed_) return false;
    if (!pb_.apply_ability_change(change)) {
        abilities_off("the pawn's state does not take the fishing change (" + note + ": " + (pb_.error().empty() ? std::string("its items and the runtime's keys disagree") : pb_.error()) + ")");
        return false;
    }
    abilities_dirty_ = true;
    spdlog::debug("[fishing] {}: {}", name(), note);
    flush_abilities(conn);
    return true;
}

void PlayerSession::fishing_step(Connection& conn, Clock::time_point now, const fishing::Event& e) {
    FishingRun& run = *fishing_;
    const fishing::Fishing* world_fishing = world_ ? world_->fishing() : nullptr;
    AbilityRuntime* runtime = ability_runtime(now);
    if (!world_fishing || !runtime) return;
    const double synced = bootstrap::effect_clock(pb_.setup().world_time + ability_seconds(now));
    const auto item_at = [&](float position, bool jump, bool predicted, bool cleared) {
        bootstrap::ActiveAbilityItem item;
        item.ability_guid = fishing::kGuidFishingCast;
        item.server_instance_id = run.instance;
        item.montage_path = cleared ? std::string() : world_fishing->montage_path();
        item.montage_guid = cleared ? 0 : world_fishing->montage_guid();
        item.montage_group = "DefaultGroup";
        item.play_rate = 1.0f;
        item.montage_position = position;
        item.may_jump_to_position = jump;
        item.branch_predicted = predicted;
        item.animation_counter = ++run.animation_counter;
        return item;
    };
    const fishing::Timing& t = run.cast.timing();
    bootstrap::AbilityChange c;
    switch (e.step) {
        case fishing::Step::Reply: {
            for (const int64_t id : run.replaced_items) c.abilities_removed.push_back(id);
            for (const int64_t id : run.replaced_effects) c.effects_removed.push_back(id);
            run.replaced_items.clear();
            run.replaced_effects.clear();
            run.instance = runtime->allocate_instance_id();
            run.effect_id = runtime->allocate_effect_id();
            c.abilities_added.push_back(item_at(0.0f, false, true, false));
            bootstrap::CooldownItem cd;
            cd.ability_guid = fishing::kGuidFishingCast;
            cd.triggered = true;
            cd.initial_cooldown = static_cast<float>(t.cooldown);
            cd.start_time = synced;
            cd.end_time = synced + t.cooldown;
            cd.charges = 0;
            c.cooldowns_set.push_back(cd);
            bootstrap::EffectItem buff;
            buff.effect_id = run.effect_id;
            buff.effect_guid = fishing::kGuidFishingBuff;
            buff.start_time = synced;
            buff.duration = AbilityRuntime::kInfiniteDuration;
            buff.stacks = 1;
            c.effects_added.push_back(buff);
            if (fishing_apply(conn, c, "the fishing cast's item, cooldown and buff")) fishing_item_ = run.instance;
            return;
        }
        case fishing::Step::CooldownEnd:
            c.cooldowns_removed.push_back(fishing::kGuidFishingCast);
            fishing_apply(conn, c, "the fishing cooldown ends");
            return;
        case fishing::Step::Bite:
            ++fishing_bites_;
            c.abilities_changed.push_back(item_at(t.loop_end_position, true, false, false));
            fishing_apply(conn, c, "a fish bites: the montage jumps to its LoopEnd section");
            spdlog::log(level_for(fishing_done_logged_, 40), "[fishing] {}: a fish bites ({}), the result comes in {:.1f} s", name(), run.cast.bites(), t.bite_to_result);
            return;
        case fishing::Step::Result:
            fishing_result(conn, now);
            return;
        case fishing::Step::CastFail:
            ++fishing_failed_;
            c.abilities_changed.push_back(item_at(t.fail_position, true, false, false));
            fishing_apply(conn, c, "the cast fails: the montage jumps to its CastFail section");
            spdlog::log(level_for(fishing_logged_, 24), "[fishing] {}: the line fell where there is nothing to fish: the cast failed", name());
            return;
        case fishing::Step::Cancel:
            ++fishing_ended_;
            c.abilities_changed.push_back(item_at(t.end_position, true, false, false));
            fishing_apply(conn, c, "the cast ends: the montage jumps to its AbilityEnd section");
            spdlog::log(level_for(fishing_logged_, 24), "[fishing] {}: the cast ends ({}) after {} bite(s) and {} catch(es)", name(), fishing::stop_name(run.cast.stopped_by()), run.cast.bites(), run.catches);
            return;
        case fishing::Step::BuffOff:
            if (run.effect_id >= 0) {
                c.effects_removed.push_back(run.effect_id);
                run.effect_id = -1;
                fishing_apply(conn, c, "the fishing buff goes");
            }
            return;
        case fishing::Step::Clear:
            c.abilities_changed.push_back(item_at(t.end_position, true, false, true));
            fishing_apply(conn, c, "the montage is cleared: the cast is over");
            return;
    }
}

void PlayerSession::fishing_result(Connection& conn, Clock::time_point now) {
    FishingRun& run = *fishing_;
    gather::Gathering* g = npc_world_ ? npc_world_->gathering() : nullptr;
    const double sec = ability_seconds(now);
    const svc::InventoryItem* rod = inventory_.at(svc::ItemContainer::Equipment, items::equip::FishingRod);
    if (!g || !run.def || !rod || rod->instance_guid != run.tool_guid) {
        run.cast.cancel(sec, fishing::Stop::Tool);
        return;
    }
    std::string why;
    auto stat_run = gather_run(why);
    if (!stat_run) {
        spdlog::warn("[fishing] {}: no stat run for the rolls: {}", name(), why);
        run.cast.cancel(sec, fishing::Stop::Tool);
        return;
    }
    // the reward tables' predicates on the player's certification and rod (fishing/predicate.h)
    fishing::Facts facts;
    facts.certification = [this](std::string_view profession) -> int {
        if (!professions_.valid()) return -1;
        const professions::Profession* p = professions_.book()->named(profession);
        return p ? professions_.state(p->guid).tier : -1;
    };
    facts.equipment_tier = [this, g](std::string_view slot) -> int {
        const int number = tool_slot_of(slot);
        if (number < 0) return -1;
        const svc::InventoryItem* worn = inventory_.at(svc::ItemContainer::Equipment, number);
        const content::GatherToolRow* tool = worn ? g->tool(worn->item_record_id) : nullptr;
        return tool ? tool->cert_tier : -1;
    };
    const world::CityNodes* nodes = world_ ? world_->city_nodes() : nullptr;
    const GoodPosition at = checker_.good();
    const world::NodeSite site = nodes ? nodes->node_at(at.x, at.y) : world::NodeSite{};
    auto harvest = g->harvest(*run.def, *stat_run, progress_.level, site, nodes, g->rng(), why, &facts);
    if (!harvest) {
        spdlog::warn("[fishing] {}: the rolls do not run: {}", name(), why);
        run.cast.cancel(sec, fishing::Stop::Tool);
        return;
    }
    if (harvest->items.empty()) {
        ++run.empty;
        ++fishing_empty_;
    } else {
        ++run.catches;
        ++fishing_catches_;
    }
    HarvestFrame frame;
    frame.what = "fishing in " + run.water + " (" + run.def->name + ")";
    frame.tag = "fishing";
    frame.profession = run.def->profession_guid;
    frame.harvest = &*harvest;
    frame.tool_guid = run.tool_guid;
    frame.tool_slot = items::equip::FishingRod;
    frame.log_tail = harvest->items.empty() ? "; the fish got away" : "";
    deliver_harvest(conn, now, frame);
    // the branch of a rod with no durability left (Economy_Fishing_Durability_Cancel)
    const svc::InventoryItem* after = inventory_.at(svc::ItemContainer::Equipment, items::equip::FishingRod);
    if (!after || !(after->durability > 0)) run.cast.cancel(sec, fishing::Stop::Tool);
}

void PlayerSession::run_fishing(Connection& conn, Clock::time_point now) {
    if (!fishing_) return;
    AbilityRuntime* runtime = ability_runtime(now);
    if (!runtime || abilities_failed_) {
        end_fishing("the abilities are off");
        return;
    }
    FishingRun& run = *fishing_;
    const double sec = ability_seconds(now);
    // the branches, tested every tick once the cast has been running for the montage's first moment: movement, combat, a mount, death, the rod
    if (run.cast.active() && !run.cast.ending() && sec >= run.cast.first_check_at() && run.cast.first_check_at() > 0) {
        fishing::Stop stop = fishing::Stop::None;
        if (dead_) stop = fishing::Stop::Dead;
        else if (mounted()) stop = fishing::Stop::Mounted;
        else if (runtime->moving(sec)) stop = fishing::Stop::Moving;
        else if (std::hypot(checker_.good().x - run.start_x, checker_.good().y - run.start_y) > kDriftCm) stop = fishing::Stop::Moving;
        else if (stats_ && stats_->in_combat()) stop = fishing::Stop::Combat;
        else {
            const svc::InventoryItem* rod = inventory_.at(svc::ItemContainer::Equipment, items::equip::FishingRod);
            if (!rod || rod->instance_guid != run.tool_guid || !(rod->durability > 0)) stop = fishing::Stop::Tool;
        }
        if (stop != fishing::Stop::None) run.cast.cancel(sec, stop);
    }
    // a result waits for room in the reply queue, like a harvest's
    if (run.cast.result_due(sec)) {
        gather::Gathering* g = npc_world_ ? npc_world_->gathering() : nullptr;
        const size_t grants = world_ ? world_->auto_grants().size() : 0;
        if (g && !room_for(14 + grants)) return;
    }
    gather::Gathering* g = npc_world_ ? npc_world_->gathering() : nullptr;
    stats::SplitMix fallback;   // (a world with no gathering has no stream of draws; the cast cannot have started then)
    for (const fishing::Event& e : run.cast.advance(sec, g ? g->rng() : fallback)) {
        fishing_step(conn, now, e);
        if (!fishing_) return;
    }
    if (!run.cast.active()) {
        // the cast is over: its item stays on the pawn (the next cast takes it off), nothing else is held
        spdlog::debug("[fishing] {}: the cast is over ({} bites, {} catches, {} empty)", name(), run.cast.bites(), run.catches, run.empty);
    }
}

void PlayerSession::end_fishing(const char* why) {
    if (!fishing_) return;
    if (fishing_->cast.active()) {
        ++fishing_ended_;
        spdlog::log(level_for(fishing_logged_, 24), "[fishing] {}: the cast ends with no result: {} ({} bites, {} catches)", name(), why, fishing_->cast.bites(), fishing_->catches);
    }
    fishing_->cast.drop();
    fishing_.reset();
}

}}  // namespace aoc::worldd
