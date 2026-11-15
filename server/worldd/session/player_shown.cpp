// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_shown.cpp: this player's mount and ride, gear, weapon set and level, as the other players see them (Social, seeing other players: riders on their mounts; gear and
//  level changes; the members are in session/members/shown.inc)
//
//  The pawn the other players are shown (the registry's Player actor, session/player_presence.cpp) and the player's mount (the registry's Mount actor, session/mounts.h) are the two
//  actors another player's view holds of a rider.  This file keeps the pawn in line with the player, once a tick (PlayerSession::run_shown, after sync_presence):
//
//    summon           the pawn's InteractInfo.MountablePet is the mount (and none again when the mount is gone), as the mount controller says (apply_mount_effects);
//    getting on       the pawn is attached at the mount's MountSocket (bootstrap/observer_rider.h), and the mount is shown to the other players as a simulated proxy that moves
//                     (session/shown_to_others.h): its ReplicatedMovement from the moves of the ride, written here into WorldActor::mount_shown;
//    riding           the mount's motion once a tick: the velocity of the step between two good moves, from the moves' TimeStamps; the pawn follows the mount's place in the registry (the
//                     other players' rings).  The rider's view pitch is the mount's (Ride::moved writes it to the mount's RemoteViewPitch16); the seated pawn is sent nothing while it rides;
//    getting off      the attachment goes; the pawn is at the place the player landed (session/player_presence.cpp) and walks again;
//    gear             an item worn or taken off: the held items (all 23 marked, as the player's own reply does), the appearance list and the two EveryoneProxy stat arrays;
//    a weapon swap    the flag the client picks the two weapons in hand from (CombatInfo.bIsUsingRangedWeapon) and the stats the other line gives;
//    a level          the new Level, and the level-up call retail multicasts (BroadcastPlayLevelUpEffects: the announcement on the pawn, answered by every other session with one call
//                     of its own on that pawn's channel).
//
//  Modelled: no recording shows another player's pawn receive these (nothing records an observer), so the shape is the owner's own reply's, kept current on the pawn the watchers hold;
//  the inactive weapon pair's back placement is animation, not replicated data.
//  Modelled too, not retail: the ride's velocity's 200 ms of stillness (retail's server integrates the mount's own physics), and a flying or swimming mount's vertical velocity from the step.
// ============================================================================
#include "session/player_session.h"

#include "bootstrap/character_setup.h"
#include "bootstrap/observer_rider.h"
#include "content/content_db.h"
#include "items/inventory.h"
#include "protocol/repl/move_packed.h"
#include "protocol/repl/rpc.h"
#include "session/player_session_util.h"
#include "session/shown_to_others.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;
namespace schema = ::aoc::protocol::schema;

namespace {
/// A step whose speed is above this is a jump (a step with next to no time between), shown without a velocity: twice the fastest ground speed the move checker lets a pawn have
/// (player_presence.cpp).  A ride's own bound is never less than this, and never less than what the move checker lets the ride do (Ride::speed_limit; a dive: kShownDiveCmPerS).
constexpr double kShownJumpCmPerS = 3500.0;
/// The fastest step the move checker accepts of a flying or swimming ride: a flyer's dive, up to Mount_Flying_Base's MaxFlyFallSpeed at 90 degrees down, with the checker's margin over it
/// (session/move_checker.h).  The game's own dives reach 3,671 cm/s at 34.5 degrees, above the walker's bound.
constexpr double kShownDiveCmPerS = Ride::kMaxFlyFallSpeed * 1.10;
/// ... and the moves that stopped: with no new place for this long the mount is shown standing, so another client does not run it on in place.
constexpr auto kShownStill = std::chrono::milliseconds(200);
/// Abilities between players: how often the shown pawn's Health and Mana are brought in line (sync_shown_vitals).  Ours: retail's server replicates its stats at the net
/// update rate, which no recording of an observer shows; 150 ms keeps a health bar smooth for the eye at a sixth of a 30 Hz tick's updates.
constexpr auto kShownVitalsEvery = std::chrono::milliseconds(150);
}  // namespace

/// The world's clock as the mount's own stamps run on it (the mount's open and the ride's effects are stamped from the ability runtime's clock).
double PlayerSession::world_clock(Clock::time_point now) const { return pb_.setup().world_time + ability_seconds(now); }

void PlayerSession::run_shown(Connection& conn, Clock::time_point now) {
    if (!registry_ || world_pawn_ == 0 || finished_ || failed_) return;   // (before the pawn the others are shown is in the world, the mount waits here)
    sync_shown_mount(now);
    sync_shown_ride(now);
    sync_shown_gear();
    sync_shown_weapon();
    sync_shown_level();
    sync_shown_level_ups(conn);
    sync_shown_vitals(now);
}

void PlayerSession::sync_shown_vitals(Clock::time_point now) {
    // death first, at once: a fallen player is shown dead in the update that takes his Health to 0
    if (dead_ != shown_dead_) {
        const bool dead = dead_;
        bool ok = true;
        const bool done = show_in_pawn(world::Change::State, [&](world::WorldActor& w) {
            ok = pb_.set_observer_dead(w.actor, dead);
            ok = pb_.set_observer_stats(w.actor) && ok;
        });
        if (!done || !ok) {
            if (++shown_failures_ <= kLoggedPerKind) {
                spdlog::warn("[players] {}: the pawn the other players are shown does not take its death ({}): they do not see it", name(), done ? "a value does not fit" : "the pawn is not in the world");
            }
            return;   // (tried again at the next tick)
        }
        shown_dead_ = dead;
        shown_vitals_at_ = now;
        return;
    }
    // Health and Mana: a few updates a second at most (a regeneration moves them every second, a fight more often)
    if (now - shown_vitals_at_ < kShownVitalsEvery) return;
    shown_vitals_at_ = now;
    const world::WorldActor* shown = registry_->find(world_pawn_);
    if (!shown || !pb_.observer_stats_differ(shown->actor)) return;
    bool ok = true;
    const bool done = show_in_pawn(world::Change::State, [&](world::WorldActor& w) { ok = pb_.set_observer_stats(w.actor); });
    if ((!done || !ok) && ++shown_failures_ <= kLoggedPerKind) {
        spdlog::warn("[players] {}: the pawn the other players are shown does not take its Health ({}): they do not see it", name(), done ? "a value does not fit" : "the pawn is not in the world");
    }
}

void PlayerSession::sync_shown_mount(Clock::time_point now) {
    const protocol::emit::FIntrepidNetworkGUID pet = wanted_pet_;
    const protocol::emit::FIntrepidNetworkGUID mount = mounted() ? mount_guid_ : protocol::emit::FIntrepidNetworkGUID{};
    const bool pet_changes = pet.ObjectId != shown_pet_.ObjectId;
    const bool ride_changes = mount.ObjectId != shown_mount_.ObjectId;
    if (!pet_changes && !ride_changes) return;
    if (!interact_info_) {   // the pawn's InteractInfo is minted at the first summon or ride: the pawn the others are shown holds the same one
        if (!world_) return;
        interact_info_ = world_->mint_guid();
    }
    const protocol::emit::FIntrepidNetworkGUID info = *interact_info_;
    bool ok = true;
    const bool done = show_in_pawn(world::Change::State, [&](world::WorldActor& w) {
        if (pet_changes) ok = bootstrap::observer_mountable_pet(w.actor, info, pet) && ok;
        if (!ride_changes) return;
        if (mount.ObjectId != 0) {
            // the update that attaches the pawn carries the export of the mount's mesh, until every connection has acknowledged it
            ok = bootstrap::observer_mount_up(w.actor, info, mount, mount_mesh_, w.actor_exports) && ok;
        } else {
            ok = bootstrap::observer_get_off(w.actor, info) && ok;
            w.actor_exports.clear();
        }
    });
    if (!done || !ok) {
        if (++shown_failures_ <= kLoggedPerKind) {
            spdlog::warn("[players] {}: the pawn the other players are shown does not take its mount ({}): they do not see it", name(), done ? "a value does not fit" : "the pawn is not in the world");
        }
    }
    shown_pet_ = pet;
    shown_mount_ = mount;
    if (ride_changes) {
        ride_moving_ = false;
        ride_at_ = checker_.good();
        ride_at_.has_time_stamp = false;
        if (mount.ObjectId != 0 && ride_) {   // the mount's motion starts standing, on the clock its other stamps run on (an update older than the last one the client holds is dropped)
            const double stamp = world_clock(now);
            const uint8_t mode = shown_movement_mode(ride_at_.movement_mode);
            registry_->modify(ride_->actor_id(), world::Change::Movement, [&](world::WorldActor& a) {
                a.mount_shown.vx = a.mount_shown.vy = a.mount_shown.vz = 0;
                a.mount_shown.moving = false;
                a.mount_shown.mode = mode;
                a.mount_shown.stamp = stamp;
            });
        }
        spdlog::log(level_for(ride_logged_, kLoggedMounts), "[players] {}: {}", name(), mount.ObjectId != 0 ? "the other players are shown it riding its mount (actor " + std::to_string(mount.ObjectId) + ")" : std::string("the other players are shown it off its mount"));
    }
}

void PlayerSession::sync_shown_ride(Clock::time_point now) {
    if (!mounted() || !ride_ || shown_mount_.ObjectId == 0) return;
    const GoodPosition g = checker_.good();   // the mount's own place: the move checker follows the controlled actor
    const double stamp = world_clock(now);
    const bool moved = g.x != ride_at_.x || g.y != ride_at_.y || g.z != ride_at_.z || g.yaw_degrees != ride_at_.yaw_degrees || g.movement_mode != ride_at_.movement_mode;
    const uint64_t mount_id = ride_->actor_id();
    if (!moved) {
        if (ride_moving_ && now - ride_moved_at_ >= kShownStill) {   // the moves stopped: the mount is shown standing where it is
            ride_moving_ = false;
            registry_->modify(mount_id, world::Change::Movement, [&](world::WorldActor& a) {
                a.mount_shown.vx = a.mount_shown.vy = a.mount_shown.vz = 0;
                a.mount_shown.moving = false;
                a.mount_shown.stamp = stamp;
            });
        }
    } else {
        // the velocity of the step, from the client's own clock (the moves' TimeStamps); none for a jump or a step with no time between
        double vx = 0, vy = 0, vz = 0;
        const double dt = (g.has_time_stamp && ride_at_.has_time_stamp) ? static_cast<double>(g.time_stamp) - static_cast<double>(ride_at_.time_stamp) : 0.0;
        const bool vertical = g.movement_mode == repl::kMoveFlying || g.movement_mode == repl::kMoveSwimming;
        if (dt > 0 && dt < 1.0) {
            vx = (g.x - ride_at_.x) / dt;
            vy = (g.y - ride_at_.y) / dt;
            if (vertical) vz = (g.z - ride_at_.z) / dt;
            // what the move checker accepted of this ride is no jump: the mount's own limit (the GM's speed multiplier is in it), a flyer's dive above all
            const double fastest = std::max({kShownJumpCmPerS, ride_->speed_limit(g.movement_mode, ability_seconds(now)), vertical ? kShownDiveCmPerS : 0.0});
            if (std::hypot(vx, vy, vz) > fastest) vx = vy = vz = 0;
        }
        const bool moving = vx != 0 || vy != 0 || vz != 0;
        ride_at_ = g;
        ride_moving_ = moving;
        ride_moved_at_ = now;
        const uint8_t mode = shown_movement_mode(g.movement_mode);
        registry_->modify(mount_id, world::Change::Movement, [&](world::WorldActor& a) {
            a.mount_shown.vx = vx;
            a.mount_shown.vy = vy;
            a.mount_shown.vz = vz;
            a.mount_shown.moving = moving;
            a.mount_shown.mode = mode;
            a.mount_shown.stamp = stamp;
        });
    }
}

void PlayerSession::queue_worn_gear(Connection& conn, size_t marks, std::optional<Clock::time_point> when) {
    const size_t m = marks != 0 ? marks : 1;
    const std::vector<bootstrap::HeldItem> held = bootstrap::held_items_for(inventory_);
    const std::vector<repl::AppearanceId> appearance = items::appearance_ids(inventory_);
    const auto build = [held, appearance, m](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> {
        if (!pb.apply_equipment(held, appearance, m)) return std::nullopt;
        return pb.equipment_update();
    };
    if (when) queue_later("HeldItems and AppearanceIDs", build, conn, *when, /*must_build=*/true);
    else queue("HeldItems and AppearanceIDs", build, conn, /*must_build=*/true);
    // the pawn the other players are shown takes the same look, held items and stats at this session's next tick (sync_shown_gear)
    note_shown_gear(m);
}

void PlayerSession::sync_shown_gear() {
    if (!shown_gear_dirty_) return;
    const std::vector<bootstrap::HeldItem> held = bootstrap::held_items_for(inventory_);
    const std::vector<repl::AppearanceId> appearance = items::appearance_ids(inventory_);
    const size_t marks = std::min<size_t>(shown_gear_marks_ == 0 ? 1 : shown_gear_marks_, 64);
    bool ok = true;
    const bool done = show_in_pawn(world::Change::State, [&](world::WorldActor& w) {
        ok = pb_.set_observer_gear(w.actor, held, appearance, marks);
        ok = pb_.set_observer_ranged(w.actor, use_ranged_) && ok;
        ok = pb_.set_observer_stats(w.actor) && ok;
    });
    if (!done || !ok) {
        if (++shown_failures_ <= kLoggedPerKind) {
            spdlog::warn("[players] {}: the pawn the other players are shown does not take its gear ({}): they do not see it", name(),
                         done ? "a value does not fit" : "the pawn is not in the world");
        }
        return;   // (tried again at the next tick)
    }
    shown_gear_dirty_ = false;
    shown_gear_marks_ = 0;
}

void PlayerSession::sync_shown_weapon() {
    if (!shown_weapon_dirty_) return;
    bool ok = true;
    const bool done = show_in_pawn(world::Change::State, [&](world::WorldActor& w) {
        ok = pb_.set_observer_ranged(w.actor, use_ranged_);
        ok = pb_.set_observer_stats(w.actor) && ok;
    });
    if (!done || !ok) {
        if (++shown_failures_ <= kLoggedPerKind) {
            spdlog::warn("[players] {}: the pawn the other players are shown does not take its weapon set ({}): they do not see it", name(),
                         done ? "a value does not fit" : "the pawn is not in the world");
        }
        return;
    }
    shown_weapon_dirty_ = false;
}

void PlayerSession::sync_shown_level() {
    if (!shown_level_dirty_) return;
    const int level = shown_level_;
    bool ok = true;
    const bool done = show_in_pawn(world::Change::State, [&](world::WorldActor& w) {
        ok = pb_.set_observer_level(w.actor, level);
        ok = pb_.set_observer_stats(w.actor) && ok;
        if (!ok) return;
        // the announcement every other session answers with the level-up call of its own (retail's is a multicast on the pawn)
        ++w.player.level_up_serial;
        w.player.level_up_category = content::kGuidAdventuring;
        w.player.level_up_level = level;
    });
    if (!done || !ok) {
        if (++shown_failures_ <= kLoggedPerKind) {
            spdlog::warn("[players] {}: the pawn the other players are shown does not take its level ({}): they do not see it", name(),
                         done ? "a value does not fit" : "the pawn is not in the world");
        }
        return;
    }
    shown_level_dirty_ = false;
    registry_->note_level_up();   // (every other session reads it at its next tick: sync_shown_level_ups)
}

void PlayerSession::sync_shown_level_ups(Connection& conn) {
    const uint64_t serial = registry_->level_up_serial();
    const uint64_t epoch = registry_->player_epoch();
    if (serial == shown_level_ups_seen_ && epoch == shown_players_seen_) return;   // (nobody levelled and no player joined or left: no walk of the players)
    const uint64_t me = static_cast<uint64_t>(pb_.setup().player_id);
    bool all = true;
    registry_->for_each_player([&](const world::WorldActor& a) {
        if (a.owner == me) return;
        const uint64_t announced = a.player.level_up_serial;
        auto it = shown_level_ups_.find(a.id);
        if (it == shown_level_ups_.end()) {   // the first look at it: the level-ups before it are its open's values, not an effect to play now
            shown_level_ups_.emplace(a.id, announced);
            return;
        }
        if (it->second == announced) return;
        if (announced != 0 && a.player.level_up_category != 0 && a.player.level_up_level > 0 && view_ && view_->state_of(a.id) == world::ClientState::Awake) {
            const schema::RpcSchema* rpc = a.actor.obj.schema ? a.actor.obj.schema->rpc("BroadcastPlayLevelUpEffects") : nullptr;
            repl::RpcParams params;
            std::optional<repl::Emission> em;
            if (rpc && repl::set_rpc_param(*rpc, params, "Category", repl::Value::i64(a.player.level_up_category)) &&
                repl::set_rpc_param(*rpc, params, "InLevel", repl::Value::i64(a.player.level_up_level))) {
                em = repl::call_rpc(a.actor, nullptr, "BroadcastPlayLevelUpEffects", params, {}, repl::ExportAcks{});
            }
            if (!em || !view_->send_multicast(conn, a.id, *em)) {   // (built again at the next tick)
                all = false;
                if (++shown_failures_ <= kLoggedPerKind) {
                    spdlog::warn("[players] {}: the level-up of {} is not shown to this player ({})", name(), a.player.name,
                                 em ? "the connection did not take it" : "the call does not build");
                }
                return;
            }
        }
        it->second = announced;
    });
    // a pawn that left the world is forgotten: a player that comes back has a pawn of its own, whose first look is a baseline again
    for (auto it = shown_level_ups_.begin(); it != shown_level_ups_.end();) it = registry_->find(it->first) ? std::next(it) : shown_level_ups_.erase(it);
    if (all) {
        shown_level_ups_seen_ = serial;
        shown_players_seen_ = epoch;
    }
}

}}  // namespace aoc::worldd
