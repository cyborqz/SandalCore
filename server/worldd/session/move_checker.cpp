// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/move_checker.cpp
//
//  See move_checker.h.
// ============================================================================
#include "session/move_checker.h"

#include <algorithm>
#include <cmath>

namespace aoc { namespace worldd {

const char* fault_name(MoveFault f) {
    switch (f) {
        case MoveFault::None: return "none";
        case MoveFault::NotFinite: return "a number that is not finite";
        case MoveFault::OutOfWorld: return "a location outside the world";
        case MoveFault::TimeStampNotRising: return "a TimeStamp that does not rise";
        case MoveFault::TooFast: return "too fast along the ground";
        case MoveFault::RisingTooFast: return "rising too fast";
        case MoveFault::FallingTooFast: return "falling too fast";
        case MoveFault::ModeNotAllowed: return "a movement mode this mount cannot use";
        case MoveFault::TooFastInAir: return "too fast in the air or water";
        case MoveFault::TooFastInAirForLong: return "beyond the flight law for too long";
    }
    return "?";
}

MoveVerdict MoveChecker::check(const protocol::repl::NetworkMove& move, std::chrono::steady_clock::time_point now, bool dash) {
    return judge(move, now, dash ? std::max(limits_.max_speed, limits_.dash_speed) : limits_.max_speed);
}

MoveVerdict MoveChecker::check_allowing(const protocol::repl::NetworkMove& move, std::chrono::steady_clock::time_point now, double allow_speed) {
    return judge(move, now, std::max(limits_.max_speed, std::isfinite(allow_speed) ? allow_speed : 0.0));
}

MoveVerdict MoveChecker::judge(const protocol::repl::NetworkMove& move, std::chrono::steady_clock::time_point now, double speed) {
    MoveVerdict v;
    // the server's clock fills the bank, whatever the move turns out to be (a refused move earns time too)
    if (!paced_) {
        paced_ = true;
        bank_ = limits_.pace_bank;
        last_now_ = now;
    } else if (now > last_now_) {
        bank_ = std::min(limits_.pace_bank, bank_ + limits_.pace_rate * std::chrono::duration<double>(now - last_now_).count());
        last_now_ = now;
    }
    const auto reject = [&v](MoveFault f) {
        v.accepted = false;
        v.fault = f;
        return v;
    };

    const protocol::repl::Vec3d loc = move.location_cm();
    const protocol::repl::Vec3d acc = move.acceleration_cm();
    const double ts = move.time_stamp;
    if (!std::isfinite(ts) || !std::isfinite(loc.x) || !std::isfinite(loc.y) || !std::isfinite(loc.z) || !std::isfinite(acc.x) ||
        !std::isfinite(acc.y) || !std::isfinite(acc.z)) {
        return reject(MoveFault::NotFinite);
    }
    if (!limits_.world.contains(loc.x, loc.y, loc.z)) return reject(MoveFault::OutOfWorld);

    // the TimeStamp: positive and rising (UE's IsClientTimeStampValid), or the client's clock reset (every 240 s: see time_stamp_reset_gap)
    double dt = limits_.first_move_seconds;
    if (!(ts > 0.0)) return reject(MoveFault::TimeStampNotRising);
    if (good_.has_time_stamp) {
        const double last = good_.time_stamp;
        if (ts > last) {
            dt = ts - last;
        } else if (last - ts > limits_.time_stamp_reset_gap) {
            dt = limits_.first_move_seconds;   // the client's clock restarted: measure it like a first move
        } else {
            return reject(MoveFault::TimeStampNotRising);
        }
    }
    dt = std::min(dt, limits_.max_time_step);
    dt = std::min(dt, std::max(bank_, 0.0));   // the server's clock has earned this much, whatever the move claims
    v.dt = dt;
// ~ C | Y | B & C O A ~

    const double dx = loc.x - good_.x, dy = loc.y - good_.y, dz = loc.z - good_.z;
    const AirLimits& air = limits_.air;
    const uint8_t mode = move.movement_mode;

    // the mounts milestone, stage 4: swimming and flying are judged in three dimensions (see the header), never along the ground
    if (air.active && mode == protocol::repl::kMoveFlying && !air.can_fly) return reject(MoveFault::ModeNotAllowed);
    const bool three_d = mode == protocol::repl::kMoveSwimming || mode == protocol::repl::kMoveFlying;   // (a pawn's mode 5 too: Wings of Salvation; its limit is the plain speed)
    if (three_d && (air.active || mode == protocol::repl::kMoveFlying)) {
        const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        // an accepted 3D move: the time is spent, the horizontal credit refills from what the allowance did not use, the move is the last good one.  The vertical credits are NOT
        // touched (review A C1): a move in a 3D mode has no vertical allowance of its own to earn them back from, and refilling them to the full burst on every 3D move let a client
        // alternate a 3D move and a climb of max_rise_burst (500 cm) on the ground rule, for an unbounded climb.
        const auto accept = [&](double allowed) {
            bank_ -= dt;
            credit_along_ = std::min(limits_.slack, allowed - dist);
            good_.x = loc.x;
            good_.y = loc.y;
            good_.z = loc.z;
            good_.yaw_degrees = move.yaw_degrees();
            good_.movement_mode = move.movement_mode;
            good_.pitch16 = move.control_rotation.pitch;
            good_.has_time_stamp = true;
            good_.time_stamp = move.time_stamp;
            v.step = dist;
            v.allowed = allowed;
            return v;
        };
        const auto too_fast = [&](MoveFault f, double allowed) {
            v.step = dist;
            v.allowed = allowed;
            return reject(f);
        };
        if (!air.active) {
            // no air rules (the pawn's checker): mode 5 is judged in 3D against the plain speed, not along the ground
            const double allowed3 = speed * dt + credit_along_;
            if (dist > allowed3) return too_fast(MoveFault::TooFastInAir, allowed3);
            return accept(allowed3);
        }
        if (mode == protocol::repl::kMoveSwimming) {
            const double allowed3 = air.swim_speed * mult_ * air.margin * dt + credit_along_;
            if (dist > allowed3) return too_fast(MoveFault::TooFastInAir, allowed3);
            air_speed_ = 0;
            hold_speed_ = std::max(0.0, hold_speed_ - air.hold_brake * dt);
            over_law_ = 0;
            return accept(allowed3);
        }

        // ---- flying (stage 4 fix 1).  Two judges:
        //  the plain law   the dive ceiling the view's pitch (or the path's steepness) gives, reached at `air.accel`, left at `air.brake`: what the retail captures fit.  A move beyond
        //                  it is only LOGGED (`borderline`), and counts as time over the law when it is beyond `sustain_margin` x it; `sustain_seconds` of that are corrected.
        //  the physics     what the client itself can do: the speed it carries (momentum, measured from the accepted moves, falling at `hold_brake`) plus the climb it can make
        //                  (`hard_accel`, plus gravity in proportion to how steeply the move descends), at least `gross_cruise` x the cruise, at most MaxFlyFallSpeed x MoveSpeedMult
        //                  x `phys_margin`.  A move beyond it is a teleport or a hack, and is corrected at once.
        const double cruise = air.fly_speed * mult_;
        const double cap = air.dive_speed * mult_ * air.phys_margin;
        const double pitch16 = static_cast<double>(move.control_rotation.pitch);
        const double degrees = (pitch16 >= 32768.0 ? pitch16 - 65536.0 : pitch16) * 360.0 / 65536.0;
        const double descent = dist > 1e-6 ? std::clamp(-dz / dist, 0.0, 1.0) : 0.0;
        const double path_down = std::asin(descent) * 180.0 / 3.14159265358979323846;
        // the view's pitch is the client's claim: it counts as far as the path backs it (review A I2), the path's own steepness in full
        const double view_down = std::clamp(-degrees, 0.0, 90.0);
        const double down = std::max(std::min(view_down, path_down + air.pitch_trust), path_down);
        const double ceiling = mult_ * (air.fly_speed + (air.dive_speed - air.fly_speed) * down / 90.0);
        const double held = std::max(air_speed_, cruise);
        double now_air = held;
        if (ceiling > held) now_air = std::min(ceiling, held + air.accel * dt);
        else now_air = std::max(ceiling, held - air.brake * dt);
        now_air = std::max(now_air, cruise);
        const double law = std::max(held, now_air);   // the plain law's speed for this move

        const double carried = std::max(cruise, hold_speed_ - air.hold_brake * dt);
        const double climb = air.hard_accel + air.gravity * descent;
        const double gross_speed = std::min(cap, std::max({cruise * air.gross_cruise, carried + climb * dt, law * air.margin}));
        const double gross_allowed = gross_speed * dt + credit_along_;
        if (dist > gross_allowed) return too_fast(MoveFault::TooFastInAir, gross_allowed);

        const double sustain_allowed = law * air.sustain_margin * dt;   // (no credit: the slack is for the physics' judge)
        if (dist > sustain_allowed) {
            // time over the law: corrected once there is more than `sustain_seconds` of it, and then every move that stays over (the debt is paid back by moves that do not)
            if (over_law_ + dt > air.sustain_seconds) {
                over_law_ = air.sustain_seconds;
                return too_fast(MoveFault::TooFastInAirForLong, sustain_allowed);
            }
            over_law_ += dt;
        } else {
            over_law_ = std::max(0.0, over_law_ - dt);
        }
        v.soft_allowed = law * air.margin * dt;
        v.borderline = dist > v.soft_allowed;

        air_speed_ = now_air;
        // momentum is the speed the plain law (with its sustain margin) let the client earn: a dive's speed is carried through the level-out, but a speed the law never allowed does not
        // buy a higher ramp for the next move (a hack cannot lift itself by its own bootstraps)
        const double measured = dt > 1e-6 ? std::min(dist / dt, std::min(gross_speed, law * air.sustain_margin)) : 0.0;
        hold_speed_ = std::min(cap, std::max(carried, measured));
        return accept(gross_allowed);
    }
// built with C.Y.B and CoA

    // G1 stage 1, the user's gryphon (2026-10-10): a flyer in movement mode 3 (falling: the take-off, a stall, the moment before a wing beat) still travels at its flight speed; judged
    // along the ground at the mount's ground limit it was refused by 1 to 19 cm and put back mid-flight.  Mode 3 on a flyer gets the cruise (times the margin) as its horizontal limit,
    // the same the flying mode allows level; the vertical rules below stay (a faller does not climb), and a mount that cannot fly keeps the ground's.
    if (air.active && air.can_fly && mode == protocol::repl::kMoveFalling) speed = std::max(speed, air.fly_speed * mult_ * air.margin);
    // a fall or a landing after a flight still carries the flight's speed for a moment (it brakes at air.brake)
    double carry = 0;
    if (air.active && (air_speed_ > 0 || hold_speed_ > 0)) {
        carry = std::max({0.0, air_speed_ - air.brake * dt, hold_speed_ - air.hold_brake * dt}) * air.margin;
        speed = std::max(speed, carry);
    }
    const double along = std::hypot(dx, dy);
    const double allowed_along = speed * dt + credit_along_;
    if (along > allowed_along) {
        v.step = along;
        v.allowed = allowed_along;
        return reject(MoveFault::TooFast);
    }
    const double rise = std::max(dz, 0.0), fall = std::max(-dz, 0.0);
    const double allowed_up = std::max(limits_.max_rise_speed, carry) * dt + credit_up_;
    if (rise > allowed_up) {
        v.step = rise;
        v.allowed = allowed_up;
        return reject(MoveFault::RisingTooFast);
    }
    const double allowed_down = std::max(limits_.max_fall_speed, carry) * dt + credit_down_;
    if (fall > allowed_down) {
        v.step = fall;
        v.allowed = allowed_down;
        return reject(MoveFault::FallingTooFast);
    }

    // accepted: the time is spent; what was not used of the allowance refills the credit, up to the slack (up: the burst)
    bank_ -= dt;
    if (air.active) {
        air_speed_ = std::max(0.0, air_speed_ - air.brake * dt);
        hold_speed_ = std::max(0.0, hold_speed_ - air.hold_brake * dt);
        over_law_ = 0;
    }
    credit_along_ = std::min(limits_.slack, allowed_along - along);
    credit_up_ = std::min(limits_.max_rise_burst, allowed_up - rise);
    credit_down_ = std::min(limits_.slack, allowed_down - fall);

    good_.x = loc.x;
    good_.y = loc.y;
    good_.z = loc.z;
    good_.yaw_degrees = move.yaw_degrees();
    good_.movement_mode = move.movement_mode;
    good_.pitch16 = move.control_rotation.pitch;
    good_.has_time_stamp = true;
    good_.time_stamp = move.time_stamp;
    v.step = along;
    v.allowed = allowed_along;
    return v;
}

}}  // namespace aoc::worldd
