// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  npc/mover.h  (plan E1, stage 2)
//
//  The movement service of one NPC: it takes a destination (or a path), finds the way over the navmesh (nav/area_nav.h) and walks it a step at
//  a time, putting the feet on the ground the navmesh and the landscape say.  It knows nothing of the registry, the clock or replication: it is
//  given a time step and says where the NPC stands, which way it faces and how fast it goes.  Stage 3's AI calls move_to / stop on it (to chase, to
//  keep a range, to go home); stage 2's behaviours (wander, patrol, return home) do the same.
//
//  Coordinates are world cm.  `ground()` is where the feet are; the actor's origin, a capsule's centre, is `origin_z()` = ground + the foot offset
//  (the capsule's half height times the actor's scale).
//
//  Turning is limited (the Blueprint's RotationRate); the speed is the one asked for, with no ramp: the retail captures show NPCs at one constant
//  speed along a walk.
// ============================================================================
#pragma once

#include "nav/area_nav.h"

#include <cstdint>
#include <vector>

namespace aoc::worldd::npc {

using nav::Vec3;

class Mover {
public:
    enum class Status : uint8_t {
        Idle,      // nothing asked
        Moving,    // on its way
        Arrived,   // got there (until the next order)
        Blocked,   // no way: the destination is off the navmesh or not connected to where it stands
    };

    Mover() = default;

    /// `nav` may be null (every order is then Blocked).
    void init(const nav::AreaNav* nav, double foot_offset_cm, double yaw_rate_degrees_per_second);
    /// Puts the NPC on the ground at (x, y) with its feet at `ground_z`.
    void place(double x, double y, double ground_z, double yaw_degrees);

    /// Orders a walk to `dest` at `speed_cm_s`, ending `stop_distance` cm short of it.  The path is found now (Detour's A*): false and Blocked when
    /// there is none, when it only gets part of the way (`allow_partial` false) or when it is longer than `max_length` (cm, 0: any).
    bool move_to(const Vec3& dest, double speed_cm_s, double stop_distance = 0, double max_length = 0, bool allow_partial = false);
    /// Orders a walk along points already found (ground positions; the first may be where it stands).
    bool follow(const std::vector<Vec3>& path, double speed_cm_s);
    void stop();

    /// Moves `dt` seconds along the path.  Returns the status after the step.
    Status advance(double dt);

    Status status() const { return status_; }
    bool   moving() const { return status_ == Status::Moving; }
    Vec3   ground() const { return ground_; }
    double origin_z() const { return ground_.z + foot_offset_; }
    double yaw_degrees() const { return yaw_; }
    /// The velocity of the last step, cm/s (zero when it is not moving).
    Vec3   velocity() const { return velocity_; }
    double speed() const { return speed_; }
    double foot_offset() const { return foot_offset_; }
    /// The path left to walk, cm (horizontal), from where it stands.
    double remaining() const;
    const std::vector<Vec3>& path() const { return path_; }
    /// The steps whose ground neither the navmesh nor the landscape gave (a count for the tests and the log).
    size_t ground_misses() const { return ground_misses_; }
    /// The A* node expansions the last move_to used (a failed search counts too): the NPC world budgets a step's searches by it.
    int last_search_iterations() const { return last_iterations_; }
    /// Why the last move_to found no way (nav::PathResult::Fail; None when it found one).
    nav::PathResult::Fail last_fail() const { return last_fail_; }
    /// The expansions a walk of `straight_cm` is allowed (review A I5): the base for a leg of a few hundred metres plus a share per metre of the straight line, so that a long patrol leg
    /// (a river, a lake in the way) is not given up at the cap that a short wander leg needs, and a leg that cannot be found still costs a bounded search.
    static int search_iterations(double straight_cm);
// built with C_Y_B and CoA

private:
    void finish(Status s);

    const nav::AreaNav* nav_ = nullptr;
    double              foot_offset_ = 88.0;
    double              yaw_rate_ = 360.0;
    Vec3                ground_;
    double              yaw_ = 0;
    Vec3                velocity_;
    double              speed_ = 0;
    double              stop_distance_ = 0;
    std::vector<Vec3>   path_;
    size_t              next_ = 0;      // the index of the point it walks to
    Status              status_ = Status::Idle;
    size_t              ground_misses_ = 0;
    int                 last_iterations_ = 0;
    nav::PathResult::Fail last_fail_ = nav::PathResult::Fail::None;
};

}  // namespace aoc::worldd::npc
