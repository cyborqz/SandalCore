// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_movement.cpp
//
//  Moves and the game masters' travel tools: the client's moves judged by the move checker, the ground speed diagnostic, teleports, the floor net and the run speed.
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

#include "session/player_session_util.h"

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;
namespace schema = ::aoc::protocol::schema;
namespace wire = ::aoc::protocol::wire;
using bootstrap::Role;
using protocol::emit::FIntrepidNetworkGUID;

/// The ground speed diagnostic: accepted moving steps (one frame to a few, 1 cm or more along the ground) are added to the
/// sprinting or the running tally; a Dodge's steps are left out.  The first moving step after a Sprint logs both averages
/// and starts both tallies again.  It shows what speed the client really moves at while sprinting: the server replicates
/// MoveSpeedMult (1 + 0.2 for the Sprint, the retail server's own packet), and the client moves at 600 cm/s times it, so
/// 720 cm/s against 600 running (retail: 780 with two purchased sprint skills).  The line is logged for the first few Sprints
/// at info, the rest at debug.
void PlayerSession::tally_speed(const GoodPosition& before, const GoodPosition& after, bool sprinting, bool evading) {
    if (!before.has_time_stamp || evading) return;
    const double seconds = static_cast<double>(after.time_stamp) - before.time_stamp;
    const double cm = std::hypot(after.x - before.x, after.y - before.y);
    if (!(seconds > 0 && seconds < 0.5) || cm < 1.0) return;
    SpeedTally& t = sprinting ? sprint_speed_ : run_speed_;
    t.cm += cm;
    t.seconds += seconds;
    if (sprinting) {
        sprint_seen_ = true;
        return;
    }
    if (!sprint_seen_ || sprint_speed_.seconds <= 0) return;
    const auto average = [](const SpeedTally& s) { return s.seconds > 0 ? s.cm / s.seconds : 0.0; };
    spdlog::log(level_for(speed_logged_),
                "[session] {}: ground speed while sprinting {:.0f} cm/s over {:.1f} s; while running (not sprinting) {:.0f} cm/s "
                "over {:.1f} s (dodges left out; 600 x the sprint's MoveSpeedMult while sprinting: 720 with no talents, 780 for a level-25 Bard)",
                name(), average(sprint_speed_), sprint_speed_.seconds, average(run_speed_), run_speed_.seconds);
    sprint_speed_ = {};
    run_speed_ = {};
    sprint_seen_ = false;
}

void PlayerSession::on_move(Connection& conn, const repl::ClientRpc& rpc, const InBunch& bunch, Clock::time_point now, bool on_mount) {
    // the mounts milestone, stage 3: the controlled actor's channel is the only one that moves.  A move on the other (a pawn's while the player rides, a mount's that is not
    // ridden: a race of a few frames at a possession, or a client that lies) is dropped without a word to the client
    if (on_mount != mounted()) {
        ++moves_wrong_channel_;
        if (++ride_wrong_logged_ <= kLoggedPerKind) {
            spdlog::debug("[session] {}: a move on the {} channel while the player {}: dropped", name(), on_mount ? "mount's" : "pawn's", mounted() ? "rides" : "is on foot");
        }
        return;
    }
    std::string why;
    auto moves = repl::read_move_packed(bunch.data.data(), bunch.data.size(), rpc.call.param_pos,
                                        rpc.call.param_pos + rpc.call.param_bits, &why);
    if (!moves) {
        ++moves_unreadable_;
        if (!unreadable_move_logged_ || moves_unreadable_ % 100 == 0) {
            unreadable_move_logged_ = true;
            spdlog::warn("[session] {}: a ServerMovePacked does not read ({}; {} parameter bits): not acknowledged ({} such moves so far; the first and every 100th are said)",
                         name(), log_text(why), rpc.call.param_bits, moves_unreadable_);
        }
        return;
    }
    // A move that stands on a movement base gives its place relative to the base: it is put in the world before it is judged (session/player_based_move.cpp).  False: a run of moves on
    // bases worldd cannot follow ended in a correction.
    if (!stand_on_bases(*moves)) return;

    // The older moves of the packet first, oldest first, unless they were judged already (every packet repeats the last
    // frame's move as its pending move).  Their verdicts only move the last good position; the reply is the new move's.
    // (a Dodge's dash, and plan D2's movement abilities: Flourish's slide, a Blink: the speed their window allows, for that window only; a mount has its own limit)
    if (!on_mount) scale_pawn_limits(now);   // the GM travel tools: the GM's run speed multiplier scales the pawn's horizontal limits
    const double allow = on_mount ? 0.0
                                  : std::max(dash_allowed(now) ? checker_.limits().dash_speed : 0.0,
                                             pipeline_ ? pipeline_->movement_allowance(ability_seconds(now)) : 0.0);
    const auto judge = [&](const repl::NetworkMove& m) {
        if (on_mount) {
            // the Blueprint's speed times MoveSpeedMult, now (stage 4: the 3D modes, swimming and flying, scale with it as well)
            checker_.set_max_speed(ride_->speed_limit(m.movement_mode, ability_seconds(now)));
            checker_.set_speed_mult(ride_->effective_mult(ability_seconds(now)));
        }
        return checker_.check_allowing(m, now, allow);
    };
    for (const repl::NetworkMove* older : {moves->old ? &*moves->old : nullptr, moves->pending ? &*moves->pending : nullptr}) {
        if (older && !checker_.is_stale(older->time_stamp)) judge(*older);
    }
    const repl::NetworkMove& move = moves->new_move;
    const GoodPosition before = checker_.good();
    const MoveVerdict verdict = judge(move);
    if (on_mount && ride_) {
        // what the moves of the packet accepted moved the mount too (its place in the registry, the road under it)
        const GoodPosition& g = checker_.good();
        if (g.has_time_stamp) ride_->moved(g.x, g.y, g.z, g.yaw_degrees, ability_seconds(now), g.pitch16, g.movement_mode);
    }
    if (verdict.fault == MoveFault::TimeStampNotRising) {
        // A duplicate or a datagram that arrived out of order (UE ignores such a move too): nothing to answer, nothing to
        // correct. The client is not at fault, and it is not one of the refused moves.
        // A long run of them is not out of order, though: the client's clock restarted and the checker's did not (the death of 2026-10-10 dropped 66 s of moves and the info log showed
        // nothing).  Said once per run, at info.
        if (++stale_streak_ >= kStaleStreakLog && !stale_streak_logged_) {
            stale_streak_logged_ = true;
            spdlog::info("[session] {}: {} moves in a row at a TimeStamp that does not rise (this one {}, the last good {}): dropped without a reply; the client's movement clock may have restarted without the server forgetting its own",
                         name(), stale_streak_, move.time_stamp, checker_.good().time_stamp);
        }
        if (++moves_stale_ <= kLoggedPerKind) {
            spdlog::debug("[session] {}: the move at TimeStamp {} does not rise above {}: dropped without a reply", name(), move.time_stamp,
                          checker_.good().time_stamp);
        }
        return;
    }
    stale_streak_ = 0;
    stale_streak_logged_ = false;
    if (verdict.accepted && verdict.borderline) {
        // stage 4 fix 1: flight beyond the plain law but inside the client's physics is not corrected; the numbers go to the log so a later run can tune the law
        ++moves_borderline_;
        if (moves_borderline_ <= kLoggedPerKind || moves_borderline_ % 100 == 0) {
            spdlog::info("[session] {}: the {}move at TimeStamp {} (movement mode {}) would be refused by the flight law ({:.0f} cm in {:.3f} s, the law allows {:.0f}; speed multiplier {:.2f}, view pitch {:.1f} degrees): accepted, not corrected ({} so far)",
                         name(), on_mount ? "mount's " : "", move.time_stamp, static_cast<unsigned>(move.movement_mode), verdict.step, verdict.dt, verdict.soft_allowed,
                         checker_.speed_mult(),
                         (move.control_rotation.pitch >= 32768 ? move.control_rotation.pitch - 65536.0 : move.control_rotation.pitch) * 360.0 / 65536.0, moves_borderline_);
        }
    }
    if (verdict.accepted) {
        pending_fix_.reset();   // the client is where the server has it now: an earlier refusal of this datagram needs no correction
        if (!on_mount && is_moving(before, move)) moved_at_ = now;   // Gathering stage 3: a tree is not cut while walking
        // #14 review: the presence sync (the other players' views follow the pawn) is not called from here any more: tick_logic does it once a tick, after the moves and the teleports of the tick.
        if (AbilityRuntime* abilities = ability_runtime(now)) {
            const bool moving = is_moving(before, move);
            abilities->set_moving(moving, ability_seconds(now));
            if (pipeline_) pipeline_->set_moving(moving);
            if (!on_mount) tally_speed(before, checker_.good(), abilities->sprinting(), abilities->evading());
        }
        // the answer: at once, or (PlayerWorld::move_ack_gap, the original server's pace) the latest good move's at the tick (session/player_move_ack.cpp)
        if (!hold_move_ack(move.time_stamp, on_mount)) answer_move(conn, move.time_stamp, on_mount);
        return;
    }
// C O A and Cyb

    // Not plausible: the client is put back at the last good position, measured from there again with its next move.
    ++moves_refused_;
    if (moves_refused_ == 10 || moves_refused_ == 100 || moves_refused_ == 1000 || moves_refused_ == 10000) {
        spdlog::info("[session] {}: {} moves refused so far", name(), moves_refused_);
    }
    const GoodPosition& good = checker_.good();
    const repl::Vec3d at = move.location_cm();
    const spdlog::level::level_enum level = level_for(refused_logged_);
    if (verdict.step > 0) {
        spdlog::log(level, "[session] {}: the {}move at TimeStamp {} (movement mode {}) is refused ({}: {:.0f} cm in {:.3f} s, at most {:.0f}): put back at ({:.1f}, {:.1f}, {:.1f}) cm",
                    name(), on_mount ? "mount's " : "", move.time_stamp, static_cast<unsigned>(move.movement_mode), fault_name(verdict.fault), verdict.step, verdict.dt,
                    verdict.allowed, good.x, good.y, good.z);
    } else {
        spdlog::log(level, "[session] {}: the {}move at TimeStamp {} (movement mode {}) to ({:.1f}, {:.1f}, {:.1f}) cm is refused ({}): put back at ({:.1f}, {:.1f}, {:.1f}) cm",
                    name(), on_mount ? "mount's " : "", move.time_stamp, static_cast<unsigned>(move.movement_mode), at.x, at.y, at.z, fault_name(verdict.fault), good.x, good.y,
                    good.z);
    }
    // The correction goes out once the datagram's bunches are all judged (end_of_datagram), the latest refusal's.
    // (a TimeStamp that is NaN or infinite names no saved move: the client ignores it, so 0 goes out instead)
    pending_fix_ = std::isfinite(move.time_stamp) ? move.time_stamp : 0.0f;
    held_move_ack_.reset();   // the correction wins over the answer held for the move before it (the original server's one pending adjustment)
}

bool PlayerSession::is_moving(const GoodPosition& before, const repl::NetworkMove& move) const {
    if (move.input_x() != 0 || move.input_y() != 0) return true;   // the player holds a direction
    if (!before.has_time_stamp) return false;
    const double dt = static_cast<double>(move.time_stamp) - static_cast<double>(before.time_stamp);
    if (!(dt > 0)) return false;
    const repl::Vec3d at = move.location_cm();
    return std::hypot(at.x - before.x, at.y - before.y) / dt > kMovingSpeed;   // or it is going somewhere
}

// ============================================================================================================================
//  the GM travel tools (session/gm_travel.h)
// ============================================================================================================================

namespace {

// [C | Y | B - CoA]

/// The value of the RPC parameter named `name`, null when the call was not decoded or the parameter is absent (a parameter at its default is not sent).
const repl::Value* rpc_value(const repl::ClientRpc& rpc, std::string_view name) {
    if (!rpc.call.decoded || !rpc.call.rpc) return nullptr;
    const schema::RepCommand* c = repl::rpc_command(*rpc.call.rpc, name);
    if (!c) return nullptr;
    const auto it = rpc.call.params.values.find(c->handle);
    return it == rpc.call.params.values.end() ? nullptr : &it->second;
}

/// A numeric parameter, whatever its width: 0 when absent.
double number_value(const repl::Value* v) {
    if (!v) return 0.0;
    if (const auto* i = v->get_if<int64_t>()) return static_cast<double>(*i);
    if (const auto* u = v->get_if<uint64_t>()) return static_cast<double>(*u);
    if (const auto* d = v->get_if<double>()) return *d;
    if (const auto* f = v->get_if<float>()) return static_cast<double>(*f);
    return 0.0;
}

std::string text_value(const repl::Value* v) {
    if (!v) return {};
    if (const auto* s = v->get_if<std::string>()) return *s;
    return {};
}

const char* gm_refusal(PlayerSession::GmMoveResult::Status s) {
    switch (s) {
        case PlayerSession::GmMoveResult::Status::Dead: return "the player is dead";
        case PlayerSession::GmMoveResult::Status::OutsideWorld: return "outside the world";
        case PlayerSession::GmMoveResult::Status::Busy: return "the reply queue is full";
        case PlayerSession::GmMoveResult::Status::Moved: break;
    }
    return "moved";
}

}  // namespace

PlayerSession::GmMoveResult PlayerSession::gm_teleport(Connection& conn, double x, double y, double z, Clock::time_point now, const std::string& by) {
    return move_to(conn, x, y, z, now, by, /*gm=*/true);
}

PlayerSession::GmMoveResult PlayerSession::move_to(Connection& conn, double x, double y, double z, Clock::time_point now, const std::string& by, bool gm) {
    GmMoveResult r;
    r.from = last_position();
    r.to = r.from;
    if (dead_) {
        r.status = GmMoveResult::Status::Dead;
        return r;
    }
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !base_limits_.world.contains(x, y, z)) {
        r.status = GmMoveResult::Status::OutsideWorld;
        return r;
    }
    // a dismount queues four steps (the restart, the controller, the detached pawn, the mount's update), the correction one more
    if (!room_for(mounted() ? 6 : 2)) {
        r.status = GmMoveResult::Status::Busy;
        return r;
    }
    if (mounted()) {
        dismount(conn, now, DismountWhy::GmTeleport);   // the ride is over; the mount stays where it was and desummons by distance (run_mounts)
        r.dismounted = true;
    }
    // the path a respawn uses: the checker moves with the player (the credits start over, the TimeStamp state stays) and the client is snapped there
    const double yaw = checker_.good().yaw_degrees;
    checker_.teleport(x, y, z, yaw);
    pending_fix_.reset();   // a correction a refused move left would put the client back where it was
    based_fix_.reset();
    const float ts = checker_.good().has_time_stamp ? checker_.good().time_stamp : 0.0f;
    constexpr uint8_t mode = repl::kMoveFalling;   // the client lands and walks again by itself
    queue(gm ? "GM teleport" : "floor net teleport", [ts, x, y, z, mode](bootstrap::PlayerBootstrap& pb) { return pb.correction(ts, repl::Vec3d{x, y, z}, repl::Vec3d{}, mode); }, conn, /*must_build=*/true);
    if (gm) ++gm_teleports_;
    r.to = checker_.good();
    spdlog::info("[{}] {}: teleported by {} from ({:.0f}, {:.0f}, {:.0f}) to ({:.0f}, {:.0f}, {:.0f}){}", gm ? "gm" : "floor-net", name(), by, r.from.x, r.from.y, r.from.z, x, y, z,
                 r.dismounted ? "; dismounted first" : "");
    if (position_changed(/*with_yaw=*/false)) hand_over(gm ? "a GM teleport" : "the floor net", /*periodic=*/false);   // (a crash must not take the player back)
    return r;
}
// C_Y_B with C.O.A

void PlayerSession::run_floor_net(Connection& conn, Clock::time_point now) {
    if (!phase_in_done_ || dead_ || finished_ || failed_ || !npc_world_) return;   // (before the phase-in the pawn is held at the join height and cannot fall)
    const GoodPosition g = checker_.good();
    const auto rescue = npc_world_->floor_rescue(g.x, g.y, g.z);
    if (!rescue) {
        net_since_.reset();
        return;
    }
    if (!net_since_) net_since_ = now;
    if (std::chrono::duration<double>(now - *net_since_).count() < kNetHoldSeconds) return;
    if (net_last_valid_ && std::chrono::duration<double>(now - net_last_).count() < kNetRepeatSeconds) return;
    const GmMoveResult r = move_to(conn, rescue->x, rescue->y, rescue->z, now, "the floor net", /*gm=*/false);
    if (r.status == GmMoveResult::Status::Busy) return;   // (the reply queue is full: the next tick tries again)
    net_last_ = now;
    net_last_valid_ = true;
    net_since_.reset();
    if (r.status != GmMoveResult::Status::Moved) {
        spdlog::warn("[floor-net] {}: at ({:.0f}, {:.0f}, {:.0f}) cm, under the floor, and the move back is refused ({})", name(), g.x, g.y, g.z, gm_refusal(r.status));
        return;
    }
    ++net_rescues_;
    spdlog::warn("[floor-net] {}: its last good position ({:.0f}, {:.0f}, {:.0f}) cm is {:.0f} m under {}: put back at ({:.0f}, {:.0f}, {:.0f}) cm (rescue {} of this session)", name(), g.x, g.y, g.z,
                 (rescue->z - g.z) / 100.0, rescue->where, rescue->x, rescue->y, rescue->z, net_rescues_);
}

double PlayerSession::gm_limit_scale(Clock::time_point now) const {
    double m = gm_speed_;
    if (now < gm_grace_until_) m = std::max(m, gm_grace_speed_);   // the client learns of a lower speed a trip later
    return std::max(1.0, m);
}

void PlayerSession::scale_pawn_limits(Clock::time_point now) {
    const double s = gm_limit_scale(now);
    checker_.set_speed_limits(base_limits_.max_speed * s, base_limits_.dash_speed * s);
}

void PlayerSession::apply_gm_speed(Connection* conn, Clock::time_point now) {
    AbilityRuntime* runtime = ability_runtime(now);   // (first: it starts the session's ability clock)
    const double t = ability_seconds(now);
    // the pawn's MoveSpeedMult stat: the one the Sprint's effect raises (the client moves at 600 cm/s times it).  The base the Sprint ends on is the GM's, so a Sprint in progress
    // keeps its +0.2 on top and the multiplier survives the end of it.
    const stats::StatValue* engine = stats_ ? stats_->find(spawns::kStatMoveSpeedMult) : nullptr;
    const double base = engine ? static_cast<double>(engine->value) : 1.0;
    if (!pb_.has_stat_item(spawns::kStatMoveSpeedMult)) {
        spdlog::warn("[gm] {}: the pawn has no MoveSpeedMult stat to carry the run speed: the move checker's limits scale, the client does not move faster", name());
    } else {
        bootstrap::AbilityChange change;
        if (runtime && runtime->tracks(spawns::kStatMoveSpeedMult)) {
            runtime->gear_stat(change, spawns::kStatMoveSpeedMult, base * gm_speed_);
        } else {
            bootstrap::StatChange sc;
            sc.record = spawns::kStatMoveSpeedMult;
            sc.value = static_cast<float>(base * gm_speed_);
            change.stats.push_back(sc);
        }
        if (!change.empty()) {
            if (pb_.apply_ability_change(change)) {
                abilities_dirty_ = true;
            } else {
                spdlog::warn("[gm] {}: the pawn does not take the run speed ({})", name(), pb_.error().empty() ? std::string("its stat has no item") : pb_.error());
            }
        }
    }
    // the mount (the same stat, the one the road's and the dash's effects raise), and the checker's limits
    if (ride_) ride_->set_gm_multiplier(gm_speed_, t);
    if (!mounted()) scale_pawn_limits(now);
    if (conn && abilities_dirty_) flush_abilities(*conn);
}

PlayerSession::GmSpeedChange PlayerSession::gm_set_speed(Connection& conn, double multiplier, Clock::time_point now, const std::string& by) {
    GmSpeedChange r;
    r.previous = gm_speed_;
    const double m = std::isfinite(multiplier) ? std::clamp(multiplier, 0.5, 10.0) : 1.0;
    if (m < gm_speed_) {
        // a lower speed: the checker keeps the old limit for a moment, while the client still moves at the old speed
        gm_grace_speed_ = std::max(gm_speed_, now < gm_grace_until_ ? gm_grace_speed_ : 0.0);
        gm_grace_until_ = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(Ride::kSpeedGraceSeconds));
    }
    gm_speed_ = m;
    apply_gm_speed(&conn, now);
    r.now = gm_speed_;
    spdlog::info("[gm] {}: run speed x{:g} (was x{:g}) by {}{}", name(), gm_speed_, r.previous, by, mounted() ? "; riding: the mount's MoveSpeedMult follows" : "");
    return r;
}

void PlayerSession::on_gm_teleport_xyz(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const double x = number_value(rpc_value(rpc, "X")), y = number_value(rpc_value(rpc, "Y")), z = number_value(rpc_value(rpc, "Z"));
    const double radius = number_value(rpc_value(rpc, "OptionalRadius"));
    char params[160];
    std::snprintf(params, sizeof params, "X=%.0f Y=%.0f Z=%.0f OptionalRadius=%g%s", x, y, z, radius, rpc.call.decoded ? "" : "; the parameters did not decode");
    if (!gm_rpc_admitted("ServerTeleportXYZ", params, now) || !rpc.call.decoded) return;
    const GmMoveResult r = gm_teleport(conn, x, y, z, now, "ServerTeleportXYZ");
    if (r.status == GmMoveResult::Status::Moved) ++gm_rpcs_answered_;
    else spdlog::warn("[gm] {}: ServerTeleportXYZ to ({:.0f}, {:.0f}, {:.0f}) is refused ({})", name(), x, y, z, gm_refusal(r.status));
}

void PlayerSession::on_gm_teleport_key(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const std::string key = text_value(rpc_value(rpc, "DestinationLookupKey"));
    const double radius = number_value(rpc_value(rpc, "OptionalRadius"));
    char params[256];
    std::snprintf(params, sizeof params, "DestinationLookupKey=\"%.100s\" OptionalRadius=%g%s", key.c_str(), radius, rpc.call.decoded ? "" : "; the parameters did not decode");
    if (!gm_rpc_admitted("ServerTeleport", params, now) || !rpc.call.decoded) return;
    const world::Places places = world_->places();
    const world::Place* place = places.find(key);
    if (!place) {
        spdlog::warn("[gm] {}: ServerTeleport \"{}\": worldd knows no such place (the city nodes and \"login\"; worldctl places lists them)", name(), log_text(key));
        return;
    }
    const GmMoveResult r = gm_teleport(conn, place->x, place->y, world_->place_height(*place), now, "ServerTeleport");
    if (r.status == GmMoveResult::Status::Moved) ++gm_rpcs_answered_;
    else spdlog::warn("[gm] {}: ServerTeleport \"{}\" is refused ({})", name(), log_text(key), gm_refusal(r.status));
}
// C O A x Cyb

void PlayerSession::on_gm_teleport_player(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const std::string names = text_value(rpc_value(rpc, "CharacterNames"));
    const double x = number_value(rpc_value(rpc, "X")), y = number_value(rpc_value(rpc, "Y")), z = number_value(rpc_value(rpc, "Z"));
    char params[256];
    std::snprintf(params, sizeof params, "CharacterNames=\"%.100s\" X=%.0f Y=%.0f Z=%.0f%s", names.c_str(), x, y, z, rpc.call.decoded ? "" : "; the parameters did not decode");
    if (!gm_rpc_admitted("ServerTeleportPlayer", params, now) || !rpc.call.decoded) return;
    size_t moved = 0;
    if (GmPort* port = world_ ? world_->gm_port() : nullptr) {
        moved = port->teleport_characters(names, x, y, z, "ServerTeleportPlayer by " + name(), now);
    } else if (iequals(names, name())) {   // (no port: a world with only this session)
        moved = gm_teleport(conn, x, y, z, now, "ServerTeleportPlayer").status == GmMoveResult::Status::Moved ? 1 : 0;
    }
    if (moved > 0) ++gm_rpcs_answered_;
    else spdlog::warn("[gm] {}: ServerTeleportPlayer \"{}\" moved no one", name(), log_text(names));
}

/// The client calls of moves and the client's own game master teleports (logged always, answered only with --gm-commands) (session/rpc_table.h).
void PlayerSession::register_movement_rpcs(RpcTable& t) {
    t.add(RpcScope::Pawn, "ServerMovePacked", [](PlayerSession& s, const RpcContext& c) { s.on_move(c.conn, c.rpc, c.bunch, c.now, /*on_mount=*/false); });
    t.add(RpcScope::PlayerController, "ServerTeleportXYZ", [](PlayerSession& s, const RpcContext& c) { s.on_gm_teleport_xyz(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerTeleport", [](PlayerSession& s, const RpcContext& c) { s.on_gm_teleport_key(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerTeleportPlayer", [](PlayerSession& s, const RpcContext& c) { s.on_gm_teleport_player(c.conn, c.rpc, c.now); });
}

}}  // namespace aoc::worldd
