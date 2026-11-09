// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/projectile_flight.h: the flight of a homing projectile, flown the way the game flies it.
//  Pure: no engine types, no clock, no world. A step turns the direction toward the aim point by the game's law and moves the projectile; the caller reads the
//  target's place and velocity again for every step (a moving target is followed, no path is precomputed).
// ============================================================================
#pragma once

#include <algorithm>
#include <cmath>

namespace aoc::worldd::flight {

struct Vec3 {
    double x = 0, y = 0, z = 0;
};

inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(const Vec3& a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline double length(const Vec3& v) { return std::sqrt(dot(v, v)); }
inline double distance(const Vec3& a, const Vec3& b) { return length(a - b); }
inline Vec3 normalized(const Vec3& v) {
    const double len = length(v);
    return len > 1e-12 ? v * (1.0 / len) : Vec3{};
}

constexpr double kPi = 3.14159265358979323846;

/// How late the game shows a creature that moves: the place it draws follows the server's place with this time constant (seconds), a first-order lag.
/// A creature that runs is therefore drawn about this long behind where the server has it, and the game's projectiles chase the drawn place. Measured
/// on the game's own flights at a running creature: the replayed paths follow the recorded ones to a few tens of centimetres with this value.
constexpr double kViewLagS = 0.3;

/// Where a projectile is, which way it goes, how fast, and where its target's aim point sits above the target's place (the spine socket; zero for the target's centre).
/// `speed` is the record's Speed and stays constant; `turn_rad_s` is the record's DesiredAngularSpeed. `view` is the target's drawn place (see kViewLagS): the aim
/// point and the arrival are taken from it; the first step sets it.
struct Flight {
    Vec3   pos, vel;
    double speed = 0;
    double turn_rad_s = 0;
    double aim_height = 0;
    Vec3   view;
    bool   viewing = false;
    bool   trace = false;   // the flight trace is on (AOC_PROJECTILE_TRACE=1 at its launch)
};

/// The target's drawn place follows its real place: the first call sets it there, every later call moves it toward the real place by the lag's share of `dt`.
inline void follow(Flight& f, const Vec3& target, double dt) {
    if (!f.viewing) {
        f.view = target;
        f.viewing = true;
        return;
    }
    f.view = f.view + (target - f.view) * (1.0 - std::exp(-dt / kViewLagS));
}

/// One step of `dt` seconds by the game's law.
///  - The target's drawn place follows its real place (`follow`), and the flight aims at the point `aim_height` above the drawn place.
///  - `angle` is the angle between the flight's direction and the line to the aim point, 0 to 180 degrees: a target behind counts as more than 90.
///  - `closing` is the distance to the aim point less what the target and the projectile cover in this step, never below 0.
///  - The turn of the step is the whole angle once `closing` is 0. Before that it is the larger of `speed * angle / closing` and the record's turn rate, times `dt`,
///    and never more than the angle.
///  - The game rotates the direction about the cross product of the direction and the line to the aim point without scaling that axis to length 1, so the
///    direction really changes by atan2(sin(turn) * sin(angle), cos(turn)): about `turn * sin(angle)`. Far from the target a projectile therefore turns at the
///    record's rate times the sine of the angle (a target straight ahead or straight behind barely turns it), and only within a few metres of the target
///    does `speed * angle / closing` take over and bring it in sharply.
///  - The projectile then moves by the average of its old and new velocity.
inline void step(Flight& f, const Vec3& target, const Vec3& target_velocity, double dt) {
    if (dt <= 0) return;
    follow(f, target, dt);
    const Vec3 aim = f.view + Vec3{0, 0, f.aim_height};
    const Vec3 to = aim - f.pos;
    const double dist = length(to);
    const Vec3 old_vel = f.vel;
    if (dist > 1e-6) {
        const Vec3 have = length(f.vel) > 1e-9 ? normalized(f.vel) : normalized(to);
        const Vec3 want = normalized(to);
        const double c = std::clamp(dot(have, want), -1.0, 1.0);
        const double s = std::sqrt(std::max(0.0, 1.0 - c * c));          // the sine of the angle: the length of the rotation axis
        const double angle = std::acos(c) * 180.0 / kPi;                 // degrees, 0 to 180
        const double closing = std::max(dist - (length(target_velocity) + f.speed) * dt, 0.0);
        double turn = angle;                                             // degrees
        if (closing > 0) turn = std::min(std::max(f.speed * angle / closing, f.turn_rad_s * 180.0 / kPi) * dt, angle);
        const double rad = turn * kPi / 180.0;
        Vec3 side = want - have * c;                                     // the part of the line to the aim point that is square to the direction (length s)
        if (s < 1e-6 && c < 0) {
            // The target is straight behind: the game's axis has length 0, so only the launch's small random cone gets such a projectile round. Any perpendicular
            // turns it; a level flight turns in the horizontal plane when it can, so that a bolt at a target behind it stays at the height of the two bodies.
            side = std::fabs(have.z) < 0.9 ? normalized(cross(Vec3{0, 0, 1}, have)) : Vec3{0, 1, 0};
        }
        f.vel = normalized(have * std::cos(rad) + side * std::sin(rad)) * f.speed;
    }
    f.pos = f.pos + (old_vel + f.vel) * (0.5 * dt);
}

/// Whether the projectile's sphere (radius `sphere`) touches the target's capsule (radius `radius`, half height `half_height`,
/// centred on the target's position).
inline bool arrived(const Vec3& pos, const Vec3& target, double radius, double half_height, double sphere) {
    const double segment = std::max(0.0, half_height - radius);
    const double z = std::clamp(pos.z, target.z - segment, target.z + segment);
    return distance(pos, Vec3{target.x, target.y, z}) <= radius + sphere;
}

}  // namespace aoc::worldd::flight
