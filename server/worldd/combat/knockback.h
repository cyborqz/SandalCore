// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  combat/knockback.h
//
//  The numbers of the throws and pulls that are not in the tables: the modelled Get Off The Stage, and the pull's gap.
//
//  Get Off The Stage.  The data gives one velocity change: 1000 cm/s, 35 degrees up, along the Bard's facing, whatever the distance.  This server models a throw that depends on how near the creature
//  stands and how far off the Bard's facing it is [modelled; not what the data says]: up by 60 degrees at 0.5 m or less, 35 at 2 m, 20 at 5 m or more (linear between), at the record's speed up to
//  2 m and half of it at 5 m (linear between), less by half of phi/45 for a creature phi degrees off the facing (0 to 45), and along the line from the Bard to the creature.  The change is added to
//  the creature's own velocity.  `kGetOffTheStageModelled` false gives the data's version back.
// ============================================================================
#pragma once

#include <algorithm>
#include <cmath>

namespace aoc::worldd::combat {

inline constexpr bool kGetOffTheStageModelled = false;

/// The gap between a creature that is pulled in front of the one who pulls and him, besides the radii of the two (cm): the pull script's padding.
inline constexpr double kPullPaddingCm = 300.0;

/// How a throw is lifted and how fast it goes.
struct ThrowShape {
    double speed = 0;            // cm/s
    double lift_degrees = 0;     // up from the ground
};

/// The modelled Get Off The Stage for a creature `distance_cm` away from the Bard (centre to centre along the ground) and `phi_degrees` off his facing, from the record's own speed and lift (what the data
/// gives at 2 m).
inline ThrowShape modelled_throw(double distance_cm, double phi_degrees, double record_speed, double record_lift_degrees) {
    const double metres = std::max(0.0, distance_cm) / 100.0;
    const double phi = std::clamp(phi_degrees, 0.0, 45.0);
    double lift;   // the model's lift curve, anchored on the record's own lift at 2 m
    if (metres <= 0.5) lift = 60.0;
    else if (metres <= 2.0) lift = 60.0 + (35.0 - 60.0) * (metres - 0.5) / 1.5;
    else if (metres <= 5.0) lift = 35.0 + (20.0 - 35.0) * (metres - 2.0) / 3.0;
    else lift = 20.0;
    double speed_scale;
    if (metres <= 2.0) speed_scale = 1.0;
    else if (metres <= 5.0) speed_scale = 1.0 - 0.5 * (metres - 2.0) / 3.0;
    else speed_scale = 0.5;
    speed_scale *= 1.0 - 0.5 * phi / 45.0;
    return {record_speed * speed_scale, lift + (record_lift_degrees - 35.0)};
}

}  // namespace aoc::worldd::combat
