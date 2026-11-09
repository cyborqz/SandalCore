// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_abilities.cpp
//
//  The player's abilities: the ability runtime and the generic ability pipeline's events, the client's ability calls (trigger, cancel, branch and hit verification, end),
//  the lingering zones, and the consumables that cast an ability.
// ============================================================================
#include "session/player_session.h"
#include "session/summoner_pets.h"

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

#include "session/player_combat_side.h"
#include "session/player_session_util.h"

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;
namespace schema = ::aoc::protocol::schema;
namespace wire = ::aoc::protocol::wire;
using bootstrap::Role;
using protocol::emit::FIntrepidNetworkGUID;

// ============================================================================================================================
//  abilities (plan A7)
// ============================================================================================================================

AbilityRuntime* PlayerSession::ability_runtime(Clock::time_point now) {
    if (abilities_failed_ || finished_ || failed_ || !ability_rules_) return nullptr;   // a finished session answers nothing more
    if (!abilities_) {
        // The stats the runtime reads or sets, from the pawn's own arrays as they are now (the join's values and the equips' changes since).
        std::map<int64_t, double> stats;
        for (const auto& [record, value] : pb_.current_stats()) stats[record] = value;
        session_seconds(now);
        abilities_origin_ = origin_;
        abilities_.emplace(*ability_rules_, std::move(stats), pb_.setup().world_time);
        if (!abilities_->ok()) {
            spdlog::error("[session] {}: the ability runtime cannot start: {}", name(), abilities_->error());
            abilities_failed_ = true;
            abilities_.reset();
            return nullptr;
        }
        // plan D2: the generic runtime beside it (Health and Mana's regeneration runs there even without the ability tables)
        if (stats_ && stats_->started()) {
            pipeline_.emplace(world_ ? world_->ability_catalog() : nullptr, *stats_, *abilities_,
                              [this] { return bootstrap::worn_items_of(inventory_); }, pb_.setup().world_time, combat::session_dice_seed(world_ ? world_->dice_seed() : 0, static_cast<uint64_t>(pb_.setup().player_id)));
            pipeline_->set_known(known_);
            if (book_) pipeline_->set_reaction_carriers(book_->reaction_carriers(wielded_line_));
            if (world_) pipeline_->set_reactions_enabled(world_->reactions_enabled());
            pipeline_->set_weapon_tags([this] { return weapon_tags(); });
            pipeline_->set_active_target([this]() -> uint64_t { return active_targets_[0].is_valid() ? active_targets_[0].ObjectId : 0; });
            pipeline_->set_item_taker([this](int32_t client_instance_id) { return consumable_paid(client_instance_id); });   // consumables: the cost step takes the item
            sync_ability_values();
            if (npc_world_ && npc_world_->combat()) {
                combat_side_ = std::make_shared<CombatSide>(*this);
                // the runtime's clock is the session's (0 at the join); the combat world's and the aftermath's is the server's world time: the port
                // translates (the join's world time is the runtime's zero) [E1 stage 3 fix 2: the corpses closed and respawned at the next tick]
                hit_port_ = std::make_unique<combat::WorldHitPort>(*npc_world_->combat(), npc_world_->registry(), *combat_side_, pb_.setup().world_time);
                pipeline_->set_combat(hit_port_.get());
                npc_world_->combat()->add_player(combat_side_.get());
            }
            if (world_ && world_->ability_catalog()) {
                spdlog::info("[session] {}: the generic ability runtime is on ({} abilities known, {} in the tables)", name(), known_.size(),
                             world_->ability_catalog()->abilities());
            }
        }
    }
    return &*abilities_;
}

double PlayerSession::ability_seconds(Clock::time_point now) const {
    return std::chrono::duration<double>(now - abilities_origin_).count();
}

bool PlayerSession::dash_allowed(Clock::time_point now) const {
    return abilities_ && abilities_->dash_allowed(ability_seconds(now));
}

void PlayerSession::abilities_off(const std::string& why) {
    abilities_failed_ = true;
    pending_ends_.clear();
    // Nothing more is sent, so the client keeps whatever it was last told, never ended or removed: what the pawn lists is that (a
    // Sprint's effect and MoveSpeedMult stay until the client logs in again).
    std::string running;
    if (abilities_) {
        const auto add = [&](bool on, const char* what) {
            if (on) running += (running.empty() ? "" : ", ") + std::string(what);
        };
        add(abilities_->sprinting(), "a Sprint");
        add(abilities_->resting() || abilities_->rest_effect_on(), "a Rest");
        add(abilities_->evading(), "the Evasion");
    }
    spdlog::warn("[session] {}: abilities are off for this session ({}): the pawn lists {} ability item(s), {} effect(s) and {} cooldown(s) that are never "
                 "ended or removed now{}",
                 name(), why, pb_.listed_abilities(), pb_.listed_effects(), pb_.listed_cooldowns(),
                 running.empty() ? "" : "; the runtime has running: " + running);
}

void PlayerSession::take_ability_events() {
    AbilityRuntime& runtime = *abilities_;
    std::vector<AbilityEvent> events = runtime.take_events();
    if (pipeline_) {
        std::vector<AbilityEvent> more = pipeline_->take_events();
        events.insert(events.end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
    }
    for (AbilityEvent& e : events) {
        if (!e.change.empty()) {
            if (!pb_.apply_ability_change(e.change)) {
                abilities_off("the pawn's state does not take the ability change (" + e.note + ": " +
                              (pb_.error().empty() ? std::string("its items and the runtime's keys disagree") : pb_.error()) + ")");
                return;
            }
            abilities_dirty_ = true;
            note_emote_items(e.change);   // the emotes among the pawn's ability items: the other players are shown them (session/player_emotes.cpp)
        }
        for (const int64_t id : e.change.effects_removed) pet_effects_.erase(id);
        for (const auto& effect : e.change.effects_added) {
            if (pets::for_effect(effect.effect_guid)) pet_effects_[effect.effect_id] = effect.effect_guid;
        }
        for (bootstrap::HitConfirm& h : e.confirmations) {
            if (pending_confirms_.size() >= kMaxPendingConfirms) pending_confirms_.pop_front();
            pending_confirms_.push_back(std::move(h));
        }
        if (e.end_ability) {
            pending_ends_.push_back(*e.end_ability);
            item_reserved_.erase(*e.end_ability);   // a cast that ended unpaid takes nothing
        }
        for (const bootstrap::ZoneOp& z : e.zones) {
            if (zone_ops_.size() < kMaxZoneOps) zone_ops_.push_back(z);
        }
        // State changes (an ability starts or ends, a refusal) are what an in-game run reads; Stamina's steps are debug.  A client
        // can cause them as often as its budget allows: the first few of each kind at info, the rest at debug.
        const bool transition = e.end_ability || !e.confirmations.empty() || !e.zones.empty() || !e.change.abilities_added.empty() || !e.change.abilities_removed.empty() ||
                                !e.change.abilities_changed.empty() || !e.change.effects_added.empty() || !e.change.effects_removed.empty() ||
                                !e.change.effects_changed.empty() || !e.change.cooldowns_set.empty() || !e.change.cooldowns_removed.empty();
        const bool refusal = e.refusal;
        if (transition && last_now_ - ability_log_window_start_ >= kAbilityLogWindow) {   // (a session that runs for hours keeps its recent ability steps in the log: 100 at info in each minute)
            ability_log_window_start_ = last_now_;
            ability_transitions_logged_ = 0;
        }
        const spdlog::level::level_enum level = refusal      ? level_for(ability_logged_)
                                                : transition ? level_for(ability_transitions_logged_, kLoggedAbilityTransitions)
                                                             : spdlog::level::debug;
        spdlog::log(level, "[session] {}: {} (Stamina {:.2f}){}", name(), e.note, runtime.stamina(),
                    e.end_ability ? ", ClientEndAbility " + std::to_string(*e.end_ability) : std::string());
    }
}
// == C:Y:B | CoA ==

/// The lingering zones' actors (plan D2's dance effect): each zone the ability runtime opens is an actor of the world's registry (plan E1), owned by this
/// player: the actor system opens it on a channel of its own, sends the bFadingOut update and closes it with Destroyed, as the retail server does for a
/// dance's visual effect.  The session only builds the actor, adds it, changes it and removes it.
void PlayerSession::flush_zones(Connection& conn) {
    (void)conn;
    const auto forget = [&](int64_t id) {
        const auto it = zone_actors_.find(id);
        if (it == zone_actors_.end()) return;
        if (registry_) registry_->remove(it->second.actor);
        pb_.lingering_remove(it->second.actor);
        zone_actors_.erase(it);
    };
    while (!zone_ops_.empty()) {
        const bootstrap::ZoneOp op = zone_ops_.front();
        zone_ops_.pop_front();
        const auto existing = zone_actors_.find(op.id);
        if (op.kind == bootstrap::ZoneOp::Kind::Open) {
            if (!world_ || !registry_ || existing != zone_actors_.end()) continue;
            bootstrap::LingeringSpec spec;
            if (!pawn_mesh_) pawn_mesh_ = world_->mint_guid();
            spec.actor = world_->mint_guid();
            spec.ability = world_->mint_guid();
            spec.stats = world_->mint_guid();
            spec.movement = world_->mint_guid();
            spec.pawn_mesh = *pawn_mesh_;
            spec.zone = op.zone;
            spec.source = op.ability;
            const GoodPosition at = checker_.good();
            spec.x = at.x;
            spec.y = at.y;
            spec.z = at.z;
            spec.yaw_degrees = at.yaw_degrees;
            if (zone_random_ == 0) zone_random_ = 0x9E3779B97F4A7C15ull ^ (static_cast<uint64_t>(pb_.setup().player_id) * 0xBF58476D1CE4E5B9ull) ^ 1;
            const auto next_word = [&] {   // SplitMix64: the FGuids of an actor are random words
                zone_random_ += 0x9E3779B97F4A7C15ull;
                uint64_t z = zone_random_;
                z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
                z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
                return static_cast<uint32_t>((z ^ (z >> 31)) >> 16);
            };
            for (int i = 0; i < 4; ++i) {
                spec.effect_guid[i] = next_word();
                spec.instance_guid[i] = next_word();
            }
            auto actor = pb_.lingering_actor_of(spec);
            if (!actor) {
                spdlog::warn("[session] {}: the zone actor of ability {} cannot be built: dropped", name(), op.ability);
                continue;
            }
            world::WorldActor w;
            w.id = spec.actor.ObjectId;
            w.kind = world::ActorKind::Effect;
            w.actor = std::move(*actor);
            w.x = at.x;
            w.y = at.y;
            w.z = at.z;
            w.yaw_degrees = at.yaw_degrees;
            w.never_dormant = true;
            w.owner = static_cast<uint64_t>(pb_.setup().player_id);
            w.npc.source = world::Source::Runtime;
            w.open_exports.push_back(pb_.lingering_mesh_export(*pawn_mesh_));
            if (!registry_->add(std::move(w))) {
                spdlog::warn("[session] {}: the registry does not take the zone actor of ability {}: dropped", name(), op.ability);
                continue;
            }
            ZoneActor& za = zone_actors_[op.id];
            za.actor = spec.actor.ObjectId;
            ++zone_opened_;
            continue;
        }
        if (existing == zone_actors_.end()) continue;   // an actor that was never made has nothing to fade or close
        if (op.kind == bootstrap::ZoneOp::Kind::Close) {
            ++zone_closed_;
            spdlog::log(level_for(zone_logged_), "[session] {}: the zone actor is removed from the world (the client's channel is closed, destroyed)", name());
            forget(op.id);
            continue;
        }
        registry_->modify(existing->second.actor, world::Change::State, [](world::WorldActor& a) {
            a.actor.obj.state.set(*a.actor.obj.schema, "bFadingOut", repl::Value::boolean(true));
        });
    }
}
// ~ C | Y | B & C-O-A ~

void PlayerSession::flush_abilities(Connection& conn) {
    if (abilities_failed_ || failed_ || !join_.empty()) return;   // the join's chains go first: the pawn's second state is theirs
    flush_zones(conn);
    if (blocked_ && conn.unacked_reliable(blocked_channel_) >= blocked_unacked_) return;
    if (abilities_dirty_) {
        const bootstrap::PlayerBootstrap::Checkpoint checkpoint = pb_.checkpoint();
        bool build_failed = false;
        std::optional<bootstrap::Send> update = pb_.ability_update(&build_failed);
        if (!update) {
            // Either the changes since the last update came to nothing (an effect added and removed), or the update does not
            // build: the shadow is as it was, so the next change sends both, and the cause is logged (a few times).
            if (build_failed) {
                spdlog::log(level_for(ability_build_logged_) == spdlog::level::info ? spdlog::level::warn : spdlog::level::debug,
                            "[session] {}: an ability update does not build ({}): it is sent with the next change, if it builds then", name(),
                            pb_.error().empty() ? "no reason" : pb_.error());
            }
            abilities_dirty_ = false;
        } else {
            switch (send(conn, *update)) {
                case Outcome::Sent:
                    abilities_dirty_ = false;
                    ++ability_updates_;
                    break;
                case Outcome::Wait:
                    pb_.restore(checkpoint);
                    return;
                case Outcome::Refused:
                    pb_.restore(checkpoint);
                    spdlog::error("[session] {}: the ability update was refused by the connection", name());
                    abilities_off("the connection refused an ability update");
                    return;
            }
        }
    }
    // consumables: the cast paid its cost, so one of the stack leaves the client's bag now (the retail server sent it in the packet of the cooldown and the ability item)
    while (!item_taken_.empty()) {
        const auto sent = item_sends_.find(item_taken_.front());
        if (sent == item_sends_.end()) {
            item_taken_.pop_front();
            continue;
        }
        if (!room_for(sent->second.size() / bootstrap::kMaxEventsPerChain + 2)) break;   // (no room now: the next flush tries again)
        const std::vector<bootstrap::ItemEvent> events = std::move(sent->second);
        item_sends_.erase(sent);
        item_taken_.pop_front();
        for (size_t at = 0; at < events.size(); at += bootstrap::kMaxEventsPerChain) {
            const std::vector<bootstrap::ItemEvent> part(events.begin() + static_cast<std::ptrdiff_t>(at),
                                                         events.begin() + static_cast<std::ptrdiff_t>(std::min(events.size(), at + bootstrap::kMaxEventsPerChain)));
            queue("ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, conn, /*must_build=*/true);
        }
    }
    while (!pending_ends_.empty()) {
        const bootstrap::PlayerBootstrap::Checkpoint checkpoint = pb_.checkpoint();
        std::optional<bootstrap::Send> end = pb_.end_ability(pending_ends_.front());
        if (!end) {
            spdlog::warn("[session] {}: ClientEndAbility({}) does not build: dropped", name(), pending_ends_.front());
            pending_ends_.pop_front();
            continue;
        }
        switch (send(conn, *end)) {
            case Outcome::Sent:
                ++ability_ends_;
                pending_ends_.pop_front();
                break;
            case Outcome::Wait:
                pb_.restore(checkpoint);
                return;
            case Outcome::Refused:
                pb_.restore(checkpoint);
                spdlog::error("[session] {}: ClientEndAbility({}) was refused by the connection: dropped", name(), pending_ends_.front());
                pending_ends_.pop_front();
                break;
        }
    }
    // the confirmations of the hits (the client's floating numbers), after the update that holds their effect: up to 20 hits a call, as retail's
    while (!pending_confirms_.empty()) {
        std::vector<bootstrap::HitConfirm> batch;
        const size_t n = std::min(pending_confirms_.size(), kConfirmsPerCall);
        for (size_t i = 0; i < n; ++i) batch.push_back(pending_confirms_[i]);
        const GoodPosition at = checker_.good();
        const bootstrap::PlayerBootstrap::Checkpoint checkpoint = pb_.checkpoint();
        std::optional<bootstrap::Send> send_it = pb_.hit_confirmations(batch, repl::Vec3d{at.x, at.y, at.z});
        if (!send_it) {
            spdlog::warn("[session] {}: {} hit confirmation(s) do not build: dropped", name(), n);
            pending_confirms_.erase(pending_confirms_.begin(), pending_confirms_.begin() + static_cast<std::ptrdiff_t>(n));
            continue;
        }
        switch (send(conn, *send_it)) {
            case Outcome::Sent:
                confirms_sent_ += n;
                pending_confirms_.erase(pending_confirms_.begin(), pending_confirms_.begin() + static_cast<std::ptrdiff_t>(n));
                break;
            case Outcome::Wait:
                pb_.restore(checkpoint);
                return;
            case Outcome::Refused:
                pb_.restore(checkpoint);
                spdlog::error("[session] {}: the hit confirmations were refused by the connection: dropped", name());
                pending_confirms_.erase(pending_confirms_.begin(), pending_confirms_.begin() + static_cast<std::ptrdiff_t>(n));
                break;
        }
    }
}

void PlayerSession::run_abilities(Connection& conn, Clock::time_point now) {
    AbilityRuntime* runtime = ability_runtime(now);
    if (!runtime) return;
    runtime->tick(ability_seconds(now));
    if (pipeline_) {
        pipeline_->tick(ability_seconds(now));
        drain_combat(conn, now);
    }
    take_ability_events();
    check_death(conn, now);
    flush_abilities(conn);
}
// (CoA : C.Y.B)

void PlayerSession::on_ability_trigger(Connection& conn, const repl::ClientRpc& rpc, const InBunch& bunch, Clock::time_point now, bool on_mount) {
    AbilityRuntime* runtime = ability_runtime(now);
    if (!runtime) return;
    if (!within_budget(ability_budget_, "ServerTriggerAbility", now)) {   // (a dead character's triggers take the budget too: each costs a reliable answer)
        ++ability_dropped_;
        return;
    }
    if (dead_) {
        // a dead character casts nothing: the instance is ended (ClientEndAbility), whichever runtime would have run it
        if (const auto t = repl::read_trigger_prefix(bunch.data.data(), bunch.data.size(), rpc.call.param_pos, rpc.call.param_pos + rpc.call.param_bits)) {
            end_instance(conn, t->client_instance_id, on_mount);
        }
        return;
    }
    if (on_mount && !mounted()) {
        // a trigger on a mount nobody rides (a race at a dismount, or a client that lies): the cast is ended where it was started
        if (const auto t = repl::read_trigger_prefix(bunch.data.data(), bunch.data.size(), rpc.call.param_pos, rpc.call.param_pos + rpc.call.param_bits)) {
            ++mount_triggers_refused_;
            end_instance(conn, t->client_instance_id, true);
        }
        return;
    }
    ++ability_triggers_;
    cancel_gather(&conn, "an ability was triggered");   // Gathering stage 2: a new ability ends the channel [modelled: no capture]
    int64_t ability = 0;
    int32_t instance = 0;
    int32_t item_slot = -1;
    uint32_t item_location = 0;
    TriggerInfo info;
    std::optional<nav::Vec3> trigger_place;
    if (const auto t = repl::parse_trigger_ability(rpc.call)) {
        ability = t->ability_guid;
        if (t->first_target_place) trigger_place = nav::Vec3{t->first_target_place->x, t->first_target_place->y, t->first_target_place->z};
        instance = t->client_instance_id;
        pb_.note_trigger_pvp_flag(instance, t->pvp_force);   // (the item this trigger makes carries the flag the trigger carried)
        item_slot = t->item_slot;
        item_location = t->item_location;
        info.animation_index = t->animation_index;
        info.input_x = t->input_x;
        info.input_y = t->input_y;
        info.target_count = t->target_count;
        info.target_actors = actors_of(t->targets, t->inline_targets);
        info.item = t->item_slot != -1;
    } else if (const auto prefix = repl::read_trigger_prefix(bunch.data.data(), bunch.data.size(), rpc.call.param_pos,
                                                             rpc.call.param_pos + rpc.call.param_bits)) {
        // The call does not read whole, but its first two parameters do: that is enough to refuse what worldd does not run.
        // An ability it does run is not started from half a call.
        if (runtime->enabled(prefix->ability_guid) || (pipeline_ && pipeline_->handles(prefix->ability_guid))) {
            if (++ability_unreadable_logged_ <= kLoggedPerKind) {
                spdlog::warn("[session] {}: a ServerTriggerAbility for an ability worldd runs does not read ({} parameter bits): ignored", name(),
                             rpc.call.param_bits);
            }
            return;
        }
        ability = prefix->ability_guid;
        instance = prefix->client_instance_id;
    } else {
        if (++ability_unreadable_logged_ <= kLoggedPerKind) {
            spdlog::warn("[session] {}: a ServerTriggerAbility does not read ({} parameter bits): ignored", name(), rpc.call.param_bits);
        }
        return;
    }
    // the mounts milestone, stage 3: what a rider may do, and the mount's dash
    if (mounted()) {
        if (!mounted_trigger(conn, ability, instance, on_mount, now)) return;
    } else if (mounts_ && ability != 0) {
        // the dash is the mount's: nobody rides, so it is not run on the pawn (the generic runtime has the ability: it would put its effect on the character)
        const content::MountAbilityRow* row = mounts_->book().data().ability(ability);
        if (row && row->role == "dash") {
            ++mount_triggers_refused_;
            end_instance(conn, instance, false);
            return;
        }
    }
    if (pet_takes_trigger(conn, ability, instance, info, now)) return;
    // the mounts milestone: Summon_Mount is the mount controller's (the pipeline refuses an ability that names an item)
    if (mounts_ && ability != 0 && ability == mounts_->book().summon_ability()) return on_summon_trigger(conn, instance, item_slot, item_location, now);
    bool accepted_cast = true;   // (the generic runtime has no refusal to give back: a pet's command goes on as before)
    if (pipeline_ && pipeline_->handles(ability)) {
        if (item_slot != -1) info.item_cast = consumable_cast(ability, item_slot, item_location);   // consumables: the item whose numbers the ability reads
        const Refusal accepted = pipeline_->trigger(ability, instance, ability_seconds(now), info);
        accepted_cast = accepted == Refusal::None;
        if (accepted == Refusal::None && info.item_cast) take_consumable(info.item_cast, instance);
        if (accepted == Refusal::None && !info.target_actors.empty()) pet_joins_owners_cast(info.target_actors.front());
        // Drop Crate: the cast plays its montage and the crate leaves the back at the montage's notify, whatever the place (the original drops nothing at a place a crate cannot lie)
        if (ability == content::kGuidDropCrate && accepted == Refusal::None) {
            crate_drop_due_ = now + kCrateDropDelay;
            crate_drop_instance_ = instance;
        }
    } else {
        const Refusal ran = runtime->trigger(ability, instance, ability_seconds(now), info);
        // the game's rule: a Dodge the server took cancels every ability that has not gone out yet, gives back what it paid (the cast bars, the Salvo and the Barrage, the weapon swing under way, the held stance (Guard)
        // and every other ability in its wind up) and keeps a crate that has not left the back; a Dodge it refused ends nothing
        if (ran == Refusal::None && ability == content::kGuidDodge && pipeline_) {
            pipeline_->end_by_dodge(ability_seconds(now));
            crate_drop_due_.reset();   // (Drop Crate has not gone out until the crate leaves the back, 0.7 s after the press)
        }
    }
    take_ability_events();
    flush_abilities(conn);
    // a pet's command is the ability's: the pet obeys only when the pipeline took the cast (a refused Go To, still on its cooldown, orders nothing)
    if (const int64_t command = pets::command_of_ability(ability); command != 0 && pet_actor_ && accepted_cast) {
        pet_command(conn, command, info.target_actors.empty() ? 0 : info.target_actors.front(), trigger_place, now);
    }
}
// C_Y_B x CoA

void PlayerSession::on_ability_cancel(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    AbilityRuntime* runtime = ability_runtime(now);
    if (!runtime) return;
    if (!within_budget(cancel_budget_, "ServerNotifyHeldAbilityCancelled", now)) {
        ++cancels_dropped_;
        return;
    }
    ++cancels_;
    const auto cancel = repl::parse_held_ability_cancelled(rpc.call);
    if (!cancel) return;
    if (crate_drop_due_ && cancel->client_instance_id == crate_drop_instance_) crate_drop_due_.reset();   // the cast of Drop Crate was cancelled before its notify: the crate stays
    if (!runtime->cancel(cancel->client_instance_id, ability_seconds(now)) && pipeline_) pipeline_->cancel(cancel->client_instance_id, ability_seconds(now), static_cast<double>(cancel->cancelled_time));
    take_ability_events();
    flush_abilities(conn);
}

void PlayerSession::on_branch_request(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    AbilityRuntime* runtime = ability_runtime(now);
    if (!runtime || !pipeline_) return;
    if (!within_budget(cancel_budget_, "ServerRequestBranchVerification", now)) {
        ++cancels_dropped_;
        return;
    }
    const auto request = repl::parse_branch_verification(rpc.call);
    if (!request) return;
    // the client's cast bar ended: a cast that waits for its branch goes on from here (the montage's jump, the cost and the hits follow)
    if (!pipeline_->branch_request(request->client_instance_id, request->branch_guid, ability_seconds(now))) {
        if (unhandled_logged_.insert("ServerRequestBranchVerification").second) {
            spdlog::info("[session] {}: the client's ServerRequestBranchVerification belongs to no cast worldd waits for (it answers it for the cast bars it knows: kBranchedCasts in ability_pipeline.cpp; told once)", name());
        }
        return;
    }
    take_ability_events();
    flush_abilities(conn);
}

void PlayerSession::teach_ability(int64_t ability) {
    if (std::find(known_.begin(), known_.end(), ability) == known_.end()) known_.push_back(ability);
    if (pipeline_) pipeline_->set_known(known_);
}

void PlayerSession::on_ability_end(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    AbilityRuntime* runtime = ability_runtime(now);
    if (!runtime || !pipeline_) return;
    if (!within_budget(cancel_budget_, "ServerEndAbility", now)) {
        ++cancels_dropped_;
        return;
    }
    const auto id = repl::parse_end_ability(rpc.call);
    if (!id) return;
    if (crate_drop_due_ && *id == crate_drop_instance_) crate_drop_due_.reset();   // (the same for an end request of that cast)
    pipeline_->end_request(*id, ability_seconds(now));
    take_ability_events();
    flush_abilities(conn);
}

void PlayerSession::on_hit_request(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    AbilityRuntime* runtime = ability_runtime(now);
    if (!runtime || !pipeline_) return;
    if (!within_budget(hit_budget_, "RequestHitVerification_Server", now)) {
        ++hit_dropped_;
        return;
    }
    ++hit_requests_;
    if (dead_) return;
    const auto req = repl::parse_hit_request(rpc.call);
    if (!req) {
        if (++hit_unreadable_logged_ <= kLoggedPerKind) spdlog::warn("[session] {}: a RequestHitVerification_Server does not read ({} parameter bits): ignored", name(), rpc.call.param_bits);
        return;
    }
    // what the client says it hit: the caster's own components (its pawn, its collision cylinder, by GUID or named in place) and the NPCs' (named
    // in place: the client has no GUID for a component worldd never exported; E1 stage 3 fix)
    const bootstrap::SessionGuids& g = pb_.setup().guids;
    size_t own = 0;
    std::vector<protocol::emit::FIntrepidNetworkGUID> objects;
    std::vector<repl::InlineObject> inline_objects;
    for (const repl::HitComponentRef& c : req->components) {
        if (c.inline_object) {
            if (own_inline_object(*c.inline_object)) ++own;
            else inline_objects.push_back(*c.inline_object);
        } else if (c.object == g.collision_cylinder || c.object == g.pawn) {
            ++own;
        } else {
            objects.push_back(c.object);
        }
    }
    const AbilityPipeline::HitVerdict verdict = pipeline_->hit_request(req->ability_guid, req->targeting_guid, req->hit_guid, req->components.size(), own,
                                                                       ability_seconds(now), actors_of(objects, inline_objects));
    if (verdict != AbilityPipeline::HitVerdict::Scheduled && verdict != AbilityPipeline::HitVerdict::Quiet) {
        if (++hit_unserved_logged_ <= kLoggedPerKind) {
            spdlog::info("[session] {}: a hit request for {} / {} is {}", name(), req->ability_guid, req->hit_guid, AbilityPipeline::verdict_name(verdict));
        }
    }
    take_ability_events();
    flush_abilities(conn);
}

bool PlayerSession::set_current_value(int64_t record, float value) {
    if (!stats_ || !stats_->started() || abilities_failed_ || !std::isfinite(value)) return false;
    const stats::StatValue* v = stats_->find(record);
    if (!v || !v->held() || !pb_.has_stat_item(record)) return false;
    bootstrap::AbilityChange c;
    bootstrap::StatChange sc;
    sc.record = record;
    sc.value = value;
    sc.buckets = v->buckets();
    sc.base = v->base;
    sc.equipment = v->equip;
    c.stats.push_back(sc);
    if (!pb_.apply_ability_change(c)) return false;
    stats_->set_current(record, value);
    abilities_dirty_ = true;
    return true;
}

void PlayerSession::sync_ability_values() {
    if (pipeline_) pipeline_->sync_a7_values(pipeline_->now());
}

bool PlayerSession::item_casts_ability(int64_t record) const {
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    if (!db) return false;
    std::string error;
    return db->consumable_ability(record, error) != 0;
}

std::shared_ptr<const ItemCast> PlayerSession::consumable_cast(int64_t ability, int32_t slot, uint32_t location) {
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    const auto container = container_of_location(static_cast<uint8_t>(location));
    if (!db || !container || *container != svc::ItemContainer::Backpack || !stats_ || !stats_->started()) return nullptr;
    const svc::InventoryItem* item = inventory_.at(*container, slot);
    if (!item || item->quantity < 1) return nullptr;
    int32_t reserved = 0;   // the casts already counting this stack
    for (const auto& [id, held] : item_reserved_) reserved += held->container == static_cast<int>(*container) && held->slot == slot && held->instance_guid == item->instance_guid ? 1 : 0;
    if (item->quantity - reserved < 1) return nullptr;
    std::string error;
    if (db->consumable_ability(item->item_record_id, error) != ability) return nullptr;
    auto cast = std::make_shared<ItemCast>();
    cast->record = item->item_record_id;
    cast->ability = ability;
    cast->quality = item->quality;
    cast->stats = stats_->item_stats(item->item_record_id, item->quality);
    cast->container = static_cast<int>(*container);
    cast->slot = slot;
    cast->instance_guid = item->instance_guid;
    return cast;
}

void PlayerSession::take_consumable(const std::shared_ptr<const ItemCast>& cast, int32_t client_instance_id) {
    if (!cast->consumes) return;
    while (item_reserved_.size() >= 16) item_reserved_.erase(item_reserved_.begin());   // (a hostile client's casts that never pay)
    item_reserved_[client_instance_id] = cast;
    spdlog::log(level_for(use_logged_), "[session] {}: a consumable is used: item record {} (quality {}) from the {} slot {} casts ability {}; {} stat(s) from its stat block", name(), cast->record,
                cast->quality, items::container_name(static_cast<svc::ItemContainer>(cast->container)), cast->slot, cast->ability, cast->stats.size());
}

bool PlayerSession::consumable_paid(int32_t client_instance_id) {
    const auto reserved = item_reserved_.find(client_instance_id);
    if (reserved == item_reserved_.end()) return false;   // (nothing is reserved for this cast: a hostile client's casts pushed it out)
    const std::shared_ptr<const ItemCast> cast = reserved->second;
    item_reserved_.erase(reserved);
    items::Outcome taken = inventory_.use_up(static_cast<svc::ItemContainer>(cast->container), cast->slot, cast->instance_guid, 1);
    if (!taken.ok) {
        // the stack moved since the trigger (a move or a split in those 65 ms): the instance is looked for where it lies now
        for (const svc::InventoryItem& it : inventory_.items()) {
            if (it.item_record_id == cast->record && it.instance_guid == cast->instance_guid && it.container == svc::ItemContainer::Backpack) {
                taken = inventory_.use_up(it.container, it.slot, it.instance_guid, 1);
                break;
            }
        }
    }
    if (!taken.ok) {
        // the stack is gone since the trigger (a sale, a destroy or a hand-in within those 65 ms): the cast cannot be paid, so it ends and gives nothing
        spdlog::log(level_for(use_logged_), "[session] {}: the consumable cast {} cannot be paid, its item is not in the bag any more ({}): the cast is over", name(), client_instance_id,
                    taken.refusal);
        return false;
    }
    inventory_dirty_ = true;
    inventory_changed_ = true;
    ++uses_applied_;
    ++items_applied_;
    while (item_sends_.size() >= 16) item_sends_.erase(item_sends_.begin());   // (a hostile client's casts that never reach their cost step)
    item_sends_[client_instance_id] = item_events_for(taken);
    item_taken_.push_back(client_instance_id);
    return true;
}

/// The client calls of the ability component: triggers, held-ability cancels, branch and hit verification, ends (session/rpc_table.h).
void PlayerSession::register_abilities_rpcs(RpcTable& t) {
    t.add(RpcScope::AbilityComponent, "ServerTriggerAbility", [](PlayerSession& s, const RpcContext& c) { s.on_ability_trigger(c.conn, c.rpc, c.bunch, c.now); });
    t.add(RpcScope::AbilityComponent, "ServerNotifyHeldAbilityCancelled", [](PlayerSession& s, const RpcContext& c) { s.on_ability_cancel(c.conn, c.rpc, c.now); });
    t.add(RpcScope::AbilityComponent, "ServerRequestBranchVerification", [](PlayerSession& s, const RpcContext& c) { s.on_branch_request(c.conn, c.rpc, c.now); });
    t.add(RpcScope::AbilityComponent, "RequestHitVerification_Server", [](PlayerSession& s, const RpcContext& c) { s.on_hit_request(c.conn, c.rpc, c.now); });
    t.add(RpcScope::AbilityComponent, "ServerEndAbility", [](PlayerSession& s, const RpcContext& c) { s.on_ability_end(c.conn, c.rpc, c.now); });
}

}}  // namespace aoc::worldd
