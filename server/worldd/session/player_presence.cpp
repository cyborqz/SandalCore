// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// Players seeing each other (stage A): written by unspike in #14.
// Moved here from session/player_session.cpp so that file stays within its size limit (scripts/hotspot_limits.txt).
// ============================================================================
//  session/player_presence.cpp: this player in the world's registry, as the other players see it (the members are in session/members/presence.inc)
//
//  Once the phase-in is built, the player's pawn is put into the registry as an actor of kind Player; the other players' actor views open it, keep it where the player is and close it
//  when the player leaves.  The place is the move checker's last good position; the velocity is the one of the step between two good positions, from the moves' TimeStamps.
//  The player's own view never sends it: the session has its own pawn on channels of its own.
//
//  #14 review: what changed in the code that moved here
//    - sync_presence runs once a tick, from tick_logic (session/player_view_phases.cpp), and nowhere else.  It used to run from tick(), which the server does not call when the players'
//      view passes run on threads (the default), and after every accepted move.
//    - the player's PlayerState is no longer put into the registry: the other players are shown the pawn alone.
//    - a rider's pawn is not moved by its own motion while its player rides: it is attached to the mount (session/player_shown.cpp), and follows the mount's place for the other players' rings.
//    - the movement mode the other players' clients are shown is one a character moves in.
// ============================================================================
#include "session/player_session.h"

#include "protocol/repl/move_packed.h"
#include "session/shown_to_others.h"
#include "spawns/mount_state.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cmath>

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;

namespace {
/// Multiplayer visibility: a step whose speed is above this is a jump (a teleport, a respawn, the floor net), shown without a velocity.  Twice the fastest
/// ground speed the move checker lets a pawn have (a mount's dash: 1,741.5 cm/s): a bound of ours, not a retail number.
constexpr double kPresenceJumpCmPerS = 3500.0;
/// ... and the moves that stopped: with no new place for this long, the pawn is shown standing (its velocity 0), so another client does not run it on in place.
/// INFERRED: a client sends a move every frame while it walks; 200 ms is three of the view's passes.
constexpr auto kPresenceStill = std::chrono::milliseconds(200);

void show_movement_mode(world::WorldActor& a, uint8_t mode) {
    repl::ObjectState& obj = a.actor.obj;
    obj.state.set(*obj.schema, "ReplicatedMovementMode", repl::Value::u64(shown_movement_mode(mode)));
}
}  // namespace

void PlayerSession::sync_presence(Clock::time_point now) {
    if (!registry_ || finished_ || failed_ || !phase_in_done_ || presence_failed_) return;
    const GoodPosition here = checker_.good();
    const double stamp = pb_.setup().world_time + session_seconds(now);   // ReplicatedServerLastTransformUpdateTimeStamp: the world's clock, as an NPC's
    if (world_pawn_ == 0) {
        const GoodPosition& g = here;
        auto pawn = pb_.observer_pawn();
        world::WorldActor p;
        if (pawn) {
            p.id = pawn->guid.ObjectId;
            p.kind = world::ActorKind::Player;
            p.actor = std::move(*pawn);
            p.never_dormant = true;   // a player is never dormancy-closed (evidence 4): awake within the NPCs' 150 m, closed beyond
            p.owner = static_cast<uint64_t>(pb_.setup().player_id);
            // abilities between players: the pawn's collision cylinder, which another player's hit may name as its target, is indexed as the pawn's (CombatWorld::actor_of_object)
            p.npc.collision_cylinder = pb_.setup().guids.collision_cylinder.ObjectId;   // #23 (MrJambix)
            p.player.name = name();
            p.label = name();
            p.x = g.x, p.y = g.y, p.z = g.z, p.yaw_degrees = g.yaw_degrees;
            spawns::write_motion(p, g.x, g.y, g.z, g.yaw_degrees, 0, 0, false, stamp);
            show_movement_mode(p, g.movement_mode);
        }
        const uint64_t pid = p.id;
        // #14 review: the pawn alone is put into the registry (the PlayerState that was built beside it and moved with it is gone)
        if (!pawn || !registry_->add(std::move(p))) {
            presence_failed_ = true;
            spdlog::warn("[players] {}: the pawn the other players are shown does not build or is refused by the registry: nobody sees this player", name());
            return;
        }
        world_pawn_ = pid;
        presence_at_ = g;
        presence_moving_ = false;
        presence_moved_at_ = now;
        spdlog::info("[players] {}: in the world for the other players (pawn {}) at ({:.0f}, {:.0f}, {:.0f}) cm", name(), pid, g.x, g.y, g.z);
        return;
    }
    // A rider's pawn is attached to its mount (session/player_shown.cpp): it keeps the ReplicatedMovement it had when it got on, standing, and follows the mount's place in the registry (the
    // other players' rings), where the move checker's place is the mount's (the checker is rebased to the mount at every possession).  It follows the player again after the dismount.
    if (mounted()) {
        if (presence_moving_) {   // it got on walking: it is shown standing where it got on
            presence_moving_ = false;
            registry_->modify(world_pawn_, world::Change::Movement, [&](world::WorldActor& a) { spawns::write_motion(a, presence_at_.x, presence_at_.y, presence_at_.z, presence_at_.yaw_degrees, 0, 0, false, stamp); });
        }
        if (here.x != presence_at_.x || here.y != presence_at_.y || here.z != presence_at_.z) registry_->move(world_pawn_, here.x, here.y, here.z, here.yaw_degrees);
        presence_at_ = here;
        presence_at_.has_time_stamp = false;   // (the mount's moves run on a clock of their own: the first step after the dismount has no time between)
        return;
    }
    const GoodPosition g = here;
    const bool moved = g.x != presence_at_.x || g.y != presence_at_.y || g.z != presence_at_.z || g.yaw_degrees != presence_at_.yaw_degrees ||
                       g.movement_mode != presence_at_.movement_mode;
    if (!moved) {
        if (presence_moving_ && now - presence_moved_at_ >= kPresenceStill) {   // the moves stopped: shown standing where it is
            presence_moving_ = false;
            registry_->modify(world_pawn_, world::Change::Movement, [&](world::WorldActor& a) { spawns::write_motion(a, g.x, g.y, g.z, g.yaw_degrees, 0, 0, false, stamp); });
        }
        return;
    }
    // the velocity of the step, from the client's own clock (the moves' TimeStamps); none for a jump or a step with no time between
    double vx = 0, vy = 0;
    const double dt = (g.has_time_stamp && presence_at_.has_time_stamp) ? static_cast<double>(g.time_stamp) - static_cast<double>(presence_at_.time_stamp) : 0.0;
    if (dt > 0 && dt < 1.0) {
        vx = (g.x - presence_at_.x) / dt;
        vy = (g.y - presence_at_.y) / dt;
        if (std::hypot(vx, vy) > kPresenceJumpCmPerS) vx = vy = 0;
    }
    const bool moving = vx != 0 || vy != 0;
    presence_at_ = g;
    presence_moving_ = moving;
    presence_moved_at_ = now;
    registry_->move(world_pawn_, g.x, g.y, g.z, g.yaw_degrees);
    registry_->modify(world_pawn_, world::Change::Movement, [&](world::WorldActor& a) {
        spawns::write_motion(a, g.x, g.y, g.z, g.yaw_degrees, vx, vy, moving, stamp);
        show_movement_mode(a, g.movement_mode);   // #14 review: a mode a character moves in (it was the client's byte as sent)
    });
}

void PlayerSession::leave_presence(const char* why) {
    if (!registry_ || world_pawn_ == 0) return;
    registry_->remove(world_pawn_);   // every view that holds the pawn closes it (Destroyed)
    spdlog::info("[players] {}: out of the world for the other players ({})", name(), why);
    world_pawn_ = 0;
}

}}  // namespace aoc::worldd
