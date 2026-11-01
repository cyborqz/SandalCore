// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_based_move.cpp
//
//  Moves of a character that stands on a movable thing.  The game sends such a move with the thing as its movement base (the mount the character just got off, a corpse), and
//  then the move's Location and Acceleration are not the world's: they are the offset from the base, turned back by the base's rotation (the base's own frame).  A move is put
//  in the world here before the move checker judges it, so the checker, the ability runtime and the aggro and leash checks only ever see world places:
//
//    a base worldd can place   the root cylinder of an actor the world holds: the actor's place plus the offset turned by the actor's yaw (a capsule has no other rotation);
//                              the acceleration is turned by the same yaw
//    a base it cannot place    another component, a bone, an actor the world does not hold: the first move on a base stays where the player was, and each move after it
//                              on the same base moves the player by what its offset changed by (the base is taken to stand still), judged like any move: its speed, its
//                              height, its TimeStamp
//
//  A client that names a new base with every move would stay unjudged: its first move on each base stays in place.  After kBasedHeldCap of them in a row, counted from the last
//  move whose place the server knows, the player is put back at its last good position.  A move the checker refused, a teleport or a respawn ends the following: the next move on
//  the base is a first move again.
// ============================================================================
#include "session/player_session.h"

#include "session/gm_travel.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <string>

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;

namespace {

constexpr double kPi = 3.14159265358979323846;
/// The place the move checker has the player at, to within this many cm, is the place the last move on a followed base was given.
constexpr double kSamePlaceCm = 1.0;

bool finite3(const repl::Vec3d& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

bool at_place(const repl::Vec3d& p, const GoodPosition& g) {
    return std::fabs(p.x - g.x) <= kSamePlaceCm && std::fabs(p.y - g.y) <= kSamePlaceCm && std::fabs(p.z - g.z) <= kSamePlaceCm;
}

}  // namespace

repl::Vec3d PlayerSession::turn_by_yaw(double yaw_degrees, const repl::Vec3d& v) {
    const double yaw = yaw_degrees * kPi / 180.0;
    const double c = std::cos(yaw), s = std::sin(yaw);
    return repl::Vec3d{c * v.x - s * v.y, s * v.x + c * v.y, v.z};
}

std::optional<PlayerSession::BasedPlace> PlayerSession::place_on_base(const repl::NetworkMove& m) const {
    if (!registry_ || m.base_bone) return std::nullopt;   // a bone's place is the animation's
    const world::WorldActor* a = nullptr;
    if (m.base) {
        a = registry_->find_by_component(m.base->ObjectId);
        if (a && a->npc.collision_cylinder != m.base->ObjectId) a = nullptr;   // only the root cylinder is the actor's own place
    } else if (m.base_inline && m.base_inline->path == "CollisionCylinder") {
        a = registry_->find(m.base_inline->outer.ObjectId);
    }
    if (!a) return std::nullopt;
    const repl::Vec3d offset = turn_by_yaw(a->yaw_degrees, m.location_cm());
    BasedPlace out;
    out.location = repl::Vec3d{a->x + offset.x, a->y + offset.y, a->z + offset.z};
    out.acceleration = turn_by_yaw(a->yaw_degrees, m.acceleration_cm());
    if (!finite3(out.location) || !finite3(out.acceleration)) return std::nullopt;   // (a place that is not a number would be quantized to the world's origin)
    return out;
}

bool PlayerSession::stand_on_bases(repl::MovePacked& packed) {
    repl::NetworkMove* const moves[3] = {packed.old ? &*packed.old : nullptr, packed.pending ? &*packed.pending : nullptr, &packed.new_move};
    const auto key_of = [](const repl::NetworkMove& m) {
        BaseKey k;
        if (m.base_inline) {
            k.object = m.base_inline->outer.ObjectId;
            k.path = m.base_inline->path;
        } else if (m.base) {
            k.object = m.base->ObjectId;
        }
        if (m.base_bone) k.bone = (m.base_bone->hardcoded ? "#" + std::to_string(m.base_bone->index) : m.base_bone->text) + "_" + std::to_string(m.base_bone->number);
        return k;
    };
    const auto quantized = [](const repl::Vec3d& v) { return repl::quantize_vector(100, v.x, v.y, v.z); };
    // a base followed from a move that the checker refused (or from before a teleport, a respawn, a mount-up) is not followed any more
    if (base_track_ && !at_place(base_track_->place, checker_.good())) base_track_.reset();

    for (repl::NetworkMove* m : moves) {
        if (!m) continue;
        if (checker_.is_stale(m->time_stamp)) {
            // judged already: the checker drops it by its TimeStamp, whatever its Location says
            if (m->has_base()) {
                const GoodPosition& g = checker_.good();
                m->location = quantized(repl::Vec3d{g.x, g.y, g.z});
                m->base.reset();
                m->base_inline.reset();
                m->base_bone.reset();
            }
            continue;
        }
        if (!m->has_base()) {
            base_track_.reset();
            based_held_run_ = 0;
            continue;
        }

        ++moves_based_;
        const bool say = moves_based_ <= kLoggedPerKind || moves_based_ % 100 == 0;
        const std::string described = say ? log_text(repl::describe_base(*m)) : std::string();
        const char* how = "";
        if (const auto placed = place_on_base(*m)) {
            m->location = quantized(placed->location);
            m->acceleration = repl::quantize_vector(10, placed->acceleration.x, placed->acceleration.y, placed->acceleration.z);
            base_track_.reset();
            based_held_run_ = 0;
            how = "placed in the world";
        } else {
            ++moves_based_unplaced_;
            const repl::Vec3d offset = m->location_cm();
            const BaseKey key = key_of(*m);
            const GoodPosition& g = checker_.good();
            repl::Vec3d place{g.x, g.y, g.z};
            if (!finite3(offset)) {
                // not a number: the move keeps it, so that the checker refuses it
                base_track_.reset();
                how = "not placed: its offset is not a number";
            } else {
                if (base_track_ && base_track_->key == key) {
                    place = repl::Vec3d{base_track_->place.x + (offset.x - base_track_->offset.x), base_track_->place.y + (offset.y - base_track_->offset.y),
                                        base_track_->place.z + (offset.z - base_track_->offset.z)};
                    how = "not placed: followed from the move before on this base";
                } else {
                    ++moves_based_held_;
                    ++based_held_run_;
                    how = "not placed: held where the player was (the first move on this base)";
                }
                m->location = quantized(place);
                // (the acceleration stays as read: its frame cannot be turned without the base's rotation, and the checker only wants it to be a number)
                base_track_ = BaseTrack{key, offset, m->location_cm()};
            }
        }
        m->base.reset();
        m->base_inline.reset();
        m->base_bone.reset();
        if (say) {
            spdlog::info("[session] {}: a move on a movement base ({}): {} ({} so far, {} not placed)", name(), described, how, moves_based_, moves_based_unplaced_);
        }

        if (based_held_run_ > kBasedHeldCap) {
            // a client that names a new base with every move: its place is unknown, so it is put back where the server has it
            const GoodPosition& g = checker_.good();
            spdlog::log(level_for(moves_based_cut_), "[session] {}: {} moves in a row on movement bases worldd can neither place nor follow: put back at ({:.1f}, {:.1f}, {:.1f}) cm",
                        name(), based_held_run_, g.x, g.y, g.z);
            based_fix_ = pending_fix_ = std::isfinite(packed.new_move.time_stamp) ? packed.new_move.time_stamp : 0.0f;
            held_move_ack_.reset();   // the correction wins over the answer held for the move before it
            base_track_.reset();
            based_held_run_ = 0;
            return false;
        }
    }
    return true;
}

bool PlayerSession::release_based_fix() {
    if (!based_fix_) return false;
    if (!pending_fix_) pending_fix_ = based_fix_;
    based_fix_.reset();
    return true;
}

std::string PlayerSession::based_move_summary() const {
    std::string s;
    if (moves_unreadable_ > 0) s += ", " + std::to_string(moves_unreadable_) + " unreadable";
    if (moves_based_ > 0) {
        s += ", " + std::to_string(moves_based_) + " on a movement base (" + std::to_string(moves_based_unplaced_) + " not placed";
        if (moves_based_cut_ > 0) s += ", " + std::to_string(moves_based_cut_) + " runs cut by a correction";
        s += ")";
    }
    return s;
}

}}  // namespace aoc::worldd
