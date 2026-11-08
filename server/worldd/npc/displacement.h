// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  npc/displacement.h
//
//  How a creature moves when something other than its own legs moves it: a hit that throws it (a velocity change), a crowd-control push (a force, an acceleration or a velocity that works for as long as
//  its effect lasts), a launch that lifts it into the air.  The small integrator the NPC world steps for such a creature (NpcWorld::advance_displacement) lives here, apart from the world: it knows a
//  body's velocity, its constants and the floor under it, nothing of the registry, the clock or replication.
//
//  The rules, as the engine's character movement has them [modelled where the movement component is not read: the numbers are the engine's defaults]:
//    on the ground   the velocity along the ground loses its friction (friction times the speed, per second) and its braking (a deceleration) and stops, never reverses; the body follows the floor
//    in the air      gravity (980 cm/s^2) pulls it down and nothing slows it along the ground; it lands where its feet meet the floor, keeps its velocity along the ground and slides on from it
//    a push          adds its acceleration (a Force over the body's mass, or an Acceleration) to the velocity at every step, or holds the velocity it names (a Velocity); it does nothing within its
//                    arrival distance of its origin, and for good once there when it ends at arrival
//    take-off        a body on the ground whose velocity points up faster than gravity pulls goes into the air
//  A step is worked out in four parts so that a high friction does not overshoot (a Wolf's friction is 24 a second; the step is 67 ms).
// ============================================================================
#pragma once

#include "nav/area_nav.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>

namespace aoc::worldd::npc {

/// The engine's defaults of a character's ground movement, which a Blueprint may override (the Wolf's friction is 24).
inline constexpr double kDefaultGroundFriction = 8.0;      // 1/s
inline constexpr double kDefaultBrakingDeceleration = 2048.0;   // cm/s^2
inline constexpr double kDefaultMass = 100.0;
inline constexpr double kGravityCm = 980.0;                // cm/s^2

/// The rotation a yaw and a pitch (degrees) make, applied to a vector in that frame: the x axis is the frame's forward, the y axis its right, z up (roll is none).
inline nav::Vec3 frame_rotate(double yaw_degrees, double pitch_degrees, const nav::Vec3& local) {
    constexpr double kPi = 3.14159265358979323846;
    const double cy = std::cos(yaw_degrees * kPi / 180.0), sy = std::sin(yaw_degrees * kPi / 180.0);
    const double cp = std::cos(pitch_degrees * kPi / 180.0), sp = std::sin(pitch_degrees * kPi / 180.0);
    const nav::Vec3 x{cp * cy, cp * sy, sp}, y{-sy, cy, 0.0}, z{-sp * cy, -sp * sy, cp};
    return {local.x * x.x + local.y * y.x + local.z * z.x, local.x * x.y + local.y * y.y + local.z * z.y, local.x * x.z + local.y * y.z + local.z * z.z};
}

inline double distance3(const nav::Vec3& a, const nav::Vec3& b) {
    const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

/// What a push is at one step of the creature it works on: the place it works from and the way that frame faces, the number its record gives now, and the share of the step it is on for (its effect
/// began or ended within the step).  `alive` false: the push is over for good (its effect is gone, the one who pushed is).
struct PushFrame {
    bool      alive = true;
    nav::Vec3 origin;
    double    yaw_degrees = 0, pitch_degrees = 0;
    double    magnitude = 0;
    double    fraction = 1.0;
};

/// A crowd-control push (a record with the displaced bit): how it pushes, along which direction in its frame, and where it stops.  `frame` is asked once at every step of the creature, with the centre of
/// its body, the clock the step ends at and the length of the step.
struct PushPlay {
    enum class Kind : uint8_t { Force, Acceleration, Velocity };
    Kind      kind = Kind::Force;
    nav::Vec3 dir;                       // PushDirection, in the frame of the origin
    double    arrival_cm = 0;            // within this of the origin the push does nothing
    bool      end_at_arrival = false;    // ... and ends there for good
    std::function<bool(const nav::Vec3& body, double now, double dt, PushFrame& frame)> frame;
};

/// A creature that is thrown, pushed or sliding.
struct Displacement {
    nav::Vec3                velocity;               // cm/s
    bool                     airborne = false;
    std::optional<PushPlay>  push;
    bool                     falling_shown = false;  // its movement mode says falling (the NPC world put it there)
    std::optional<uint64_t>  saved_mode;             // the mode it had before
    nav::Vec3                last_floor;             // the last place its feet stood on a floor
    // the body's constants
    double                   friction = kDefaultGroundFriction;
    double                   braking = kDefaultBrakingDeceleration;
    double                   mass = kDefaultMass;
    /// A creature at rest with no push is no longer displaced.
    static constexpr double  kRestSpeed = 5.0;
};

/// Where the integrator stands the body: the position of its feet and the height of its centre above them.
struct DisplacePose {
    double x = 0, y = 0, feet_z = 0;
    double foot_offset = 0;
};

/// What the integrator asks of the world.  `floor`: the walkable floor under (x, y) nearest `z_hint` (false: none).  `walk`: whether a body on the ground can go in a straight line from one place to another
/// without leaving the walkable ground (false: it stops at `*edge`, the last place it can stand).
struct DisplaceWorld {
    std::function<bool(double x, double y, double z_hint, double* z)>              floor;
    std::function<bool(const nav::Vec3& from, const nav::Vec3& to, nav::Vec3* edge)> walk;
};

struct DisplaceOutcome {
    bool took_off = false, landed = false, push_ended = false, at_rest = false, blocked = false;
};

/// A body on the ground for `h` seconds: the friction and the braking take their share of its speed (it stops without reversing) and the push `(ax, ay)` (cm/s^2) adds its own, in fine sub-steps,
/// so that the speed a push builds settles where the push equals friction times speed plus braking, (a - braking) / friction, whatever the length of `h`.  A body that stands still stays so under a push
/// the braking beats.  Moves (x, y) along the way.
inline void slide_on_ground(Displacement& d, double ax, double ay, double h, double& x, double& y) {
    constexpr int kSub = 8;
    const double s = h / kSub;
    const double push = std::hypot(ax, ay);
    for (int i = 0; i < kSub; ++i) {
        const double before = std::hypot(d.velocity.x, d.velocity.y);
        double vx = d.velocity.x, vy = d.velocity.y;
        bool stopped = false;
        if (before > 0.0) {
            const double next = before - (d.friction * before + d.braking) * s;
            if (next > 0.0) {
                vx *= next / before;
                vy *= next / before;
            } else {
                vx = vy = 0.0;
                stopped = true;
            }
        } else {
            stopped = true;
        }
        if (!(stopped && push <= d.braking)) {
            vx += ax * s;
            vy += ay * s;
        }
        x += 0.5 * (d.velocity.x + vx) * s;
        y += 0.5 * (d.velocity.y + vy) * s;
        d.velocity.x = vx;
        d.velocity.y = vy;
    }
}

/// Gives the body a velocity change.  Returns whether it leaves the ground for it (its up part beats what gravity takes in one step of `dt`).
inline bool add_velocity_change(Displacement& d, const nav::Vec3& change, double dt) {
    d.velocity.x += change.x;
    d.velocity.y += change.y;
    d.velocity.z += change.z;
    if (!d.airborne && change.z - kGravityCm * dt > 0.0) d.airborne = true;
    return d.airborne;
}

/// One step of `dt` seconds of a displaced body, ending at the clock `now`.
inline void displace_step(Displacement& d, DisplacePose& p, double now, double dt, const DisplaceWorld& world, DisplaceOutcome& out) {
    constexpr int kParts = 4;
    const double h = dt / kParts;
    // the push of this step: an acceleration to add, or a velocity to hold
    nav::Vec3 accel{};
    nav::Vec3 held{};
    bool holds = false;
    if (d.push) {
        PushFrame frame;
        const nav::Vec3 body{p.x, p.y, p.feet_z + p.foot_offset};
        if (!d.push->frame || !d.push->frame(body, now, dt, frame) || !frame.alive) {
            d.push.reset();
            out.push_ended = true;
        } else if (distance3(body, frame.origin) <= d.push->arrival_cm) {
            if (d.push->end_at_arrival) {
                d.push.reset();
                out.push_ended = true;
            }
        } else if (frame.fraction > 0.0 && std::isfinite(frame.magnitude)) {
            const nav::Vec3 dir = frame_rotate(frame.yaw_degrees, frame.pitch_degrees, d.push->dir);
            switch (d.push->kind) {
                case PushPlay::Kind::Force: {
                    const double a = frame.magnitude / std::max(1e-6, d.mass) * frame.fraction;
                    accel = {dir.x * a, dir.y * a, dir.z * a};
                    break;
                }
                case PushPlay::Kind::Acceleration: {
                    const double a = frame.magnitude * frame.fraction;
                    accel = {dir.x * a, dir.y * a, dir.z * a};
                    break;
                }
                case PushPlay::Kind::Velocity:
                    held = {dir.x * frame.magnitude, dir.y * frame.magnitude, dir.z * frame.magnitude};
                    holds = true;
                    break;
            }
        }
    }
    const bool was_airborne = d.airborne;
    const nav::Vec3 from{p.x, p.y, p.feet_z};
    for (int part = 0; part < kParts; ++part) {
        if (holds) d.velocity = held;
        else d.velocity.z += accel.z * h;   // (the lift of a push; the part of it along the ground is added below, with the ground's friction)
        const double x0 = p.x, y0 = p.y, z0 = p.feet_z, vz0 = d.velocity.z;   // where this part begins, and how fast it goes up
        if (!d.airborne && vz0 > kGravityCm * h) d.airborne = true;            // a push that lifts it faster than gravity pulls: it leaves the ground
        d.velocity.z = d.airborne ? vz0 - kGravityCm * h : 0.0;
        if (d.airborne || holds) {   // (no friction in the air, and none on a body whose velocity a push holds)
            if (!holds) {
                d.velocity.x += accel.x * h;
                d.velocity.y += accel.y * h;
            }
            p.x += d.velocity.x * h;
            p.y += d.velocity.y * h;
        } else {
            slide_on_ground(d, accel.x, accel.y, h, p.x, p.y);
        }
        double floor_z = 0.0;
        if (d.airborne) {
            p.feet_z = z0 + vz0 * h - 0.5 * kGravityCm * h * h;   // (a parabola: this step of it is exact)
            if (d.velocity.z <= 0.0 && world.floor && world.floor(p.x, p.y, p.feet_z + 120.0, &floor_z) && p.feet_z <= floor_z) {
                // its feet meet the floor within this part: it lands then, keeping its velocity along the ground, and slides on for the rest of the part
                const double disc = vz0 * vz0 + 2.0 * kGravityCm * (z0 - floor_z);
                const double t = std::clamp(disc > 0.0 ? (vz0 + std::sqrt(disc)) / kGravityCm : 0.0, 0.0, h);   // (when: the later root of z0 + vz0 t - g t^2 / 2 = the floor)
                p.x = x0 + d.velocity.x * t;
                p.y = y0 + d.velocity.y * t;
                p.feet_z = floor_z;
                d.velocity.z = 0.0;
                d.airborne = false;
                out.landed = true;
                if (h - t > 1e-9) {
                    if (holds) {
                        p.x += d.velocity.x * (h - t);
                        p.y += d.velocity.y * (h - t);
                    } else {
                        slide_on_ground(d, accel.x, accel.y, h - t, p.x, p.y);
                    }
                }
            }
        } else if (world.floor && world.floor(p.x, p.y, p.feet_z, &floor_z)) {
            p.feet_z = floor_z;
        }
    }
    if (d.airborne && p.feet_z < d.last_floor.z - 3000.0) {   // it fell off the ground the world knows: it is put back where it last stood
        p.x = d.last_floor.x;
        p.y = d.last_floor.y;
        p.feet_z = d.last_floor.z;
        d.velocity = {};
        d.airborne = false;
        out.landed = true;
    }
    if (!was_airborne && d.airborne) out.took_off = true;
    // a body that stays on the ground cannot leave the walkable ground: it stops at the edge, its velocity along the ground with it
    if (!was_airborne && !d.airborne && world.walk) {
        nav::Vec3 edge;
        const nav::Vec3 to{p.x, p.y, p.feet_z};
        if ((std::fabs(to.x - from.x) > 1e-6 || std::fabs(to.y - from.y) > 1e-6) && !world.walk(from, to, &edge)) {
            p.x = edge.x;
            p.y = edge.y;
            double floor_z = 0.0;
            if (world.floor && world.floor(p.x, p.y, from.z, &floor_z)) p.feet_z = floor_z;
            else p.feet_z = from.z;
            d.velocity.x = d.velocity.y = 0.0;
            out.blocked = true;
        }
    }
    if (!d.airborne) {
        d.last_floor = {p.x, p.y, p.feet_z};
        if (!d.push && std::hypot(d.velocity.x, d.velocity.y) < Displacement::kRestSpeed) {
            d.velocity = {};
            out.at_rest = true;
        }
    }
}

}  // namespace aoc::worldd::npc
