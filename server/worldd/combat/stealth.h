// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  combat/stealth.h
//
//  The game's stealth rule, as its data and its client have it.  Two stats decide a stealth effect's chance to be noticed: the character's StealthLevel (its level,
//  less the effect's own modifier) and the observer's StealthPerceptionLevel (the observer's level).  A stealthed character is seen in a cone in front of the observer
//  up to a distance that grows 1 m for each point of the difference, and heard all round at a shorter one; a character in Camouflage is always noticed at least
//  as "detected" (its effect's own rule), and one that is close and in front is "revealed".  The four states are the client's EStealthPerceptionState:
//  Observed (not stealthed), Revealed (a target), Detected (noticed but not a target) and Hidden (not noticed).
// ============================================================================
#pragma once

#include "combat/fighter.h"

#include <algorithm>
#include <cmath>

namespace aoc::worldd::combat {

/// Which of the three effects that stealth a character is on it, and the stats the rule reads.  A character that is not stealthed has `active` false.
struct StealthState {
    bool   active = false;
    bool   camouflage = false;   // Ranger_Camouflage: its effect's rule makes the character noticed even out of the normal detection range
    double level = 0;            // the character's StealthLevel
    double perception = 0;       // its StealthPerceptionLevel (the observer's, when the character observes)
};

/// The client's EStealthPerceptionState values.
enum StealthPerception : int { kStealthObserved = 0, kStealthRevealed = 1, kStealthDetected = 2, kStealthHidden = 3 };

/// The state of `stealther` as `observer` (at `observer_pos`, facing `observer_yaw_degrees`, with `observer_perception` as its StealthPerceptionLevel) perceives it:
/// Observed when it is not stealthed; else Hidden when neither rule holds, Detected when only the detection rule does, Revealed when both do.
inline StealthPerception stealth_perception(const StealthState& stealther, const Vec3& stealther_pos, const Vec3& observer_pos, double observer_yaw_degrees, double observer_perception) {
    if (!stealther.active) return kStealthObserved;
    const double dx = stealther_pos.x - observer_pos.x, dy = stealther_pos.y - observer_pos.y, dz = stealther_pos.z - observer_pos.z;
    const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    const double diff = observer_perception - stealther.level;   // a point of perception is 1 m of detection
    // the 60 degree cone in front of the observer (dot > 0.5), on the direction to the stealther
    bool facing = false;
    if (distance > 0.0) {
        const double yaw = observer_yaw_degrees * 3.14159265358979323846 / 180.0;
        facing = (std::cos(yaw) * dx + std::sin(yaw) * dy + 0.0 * dz) / distance > 0.5;
    }
    const bool detected = stealther.camouflage || (facing && distance < std::max(0.0, 1500.0 + diff * 100.0)) || distance < diff * 100.0;
    if (!detected) return kStealthHidden;
    const bool revealed = (facing && distance < std::max(0.0, 750.0 + diff * 100.0)) || distance < -500.0 + diff * 100.0;
    return revealed ? kStealthRevealed : kStealthDetected;
}

/// Whether a creature may fight the character: everyone it perceives, and a stealthed character only when it has been revealed.
inline bool stealth_targetable(const StealthState& stealther, const Vec3& stealther_pos, const Vec3& observer_pos, double observer_yaw_degrees, double observer_perception) {
    return stealth_perception(stealther, stealther_pos, observer_pos, observer_yaw_degrees, observer_perception) <= kStealthRevealed;
}

}  // namespace aoc::worldd::combat
