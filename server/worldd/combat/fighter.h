// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  combat/fighter.h  (plan E1, stage 3, part 1)
//
//  One side of a hit.  The combat rules do not care whether a combatant is a player (its stats, worn items and ability runtime belong to its session)
//  or an NPC (its stats are the stat engine's run for its kind and level, kept by the CombatWorld): they ask a Fighter for who it is, where it
//  stands, whether it lives, and the stats the formulas run on.  Applying what a hit does to a fighter is not here: the world applies it to an NPC,
//  the session to its player (shields first, the confirmation, the replicated arrays).
// ============================================================================
#pragma once

#include "stats/character_stats.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace aoc::worldd::combat {

struct Vec3 {
    double x = 0, y = 0, z = 0;
};

inline double distance_cm(const Vec3& a, const Vec3& b) {
    const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
inline double distance_xy_cm(const Vec3& a, const Vec3& b) {
    const double dx = a.x - b.x, dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

class Fighter {
public:
    virtual ~Fighter() = default;

    /// The actor's id: an NPC's ObjectId in the registry, a player's pawn ObjectId.
    virtual uint64_t id() const = 0;
    virtual bool     is_player() const = 0;
    virtual int      level() const = 0;
    virtual Vec3     position() const = 0;
    /// How far the actor's body reaches from its centre (cm): a range test counts from the surface.
    virtual double   radius_cm() const { return 50.0; }
    virtual bool     alive() const = 0;
    /// CharacterAlignment records the actor carries: who it fights and who fights it.
    virtual const std::vector<int64_t>& alignments() const = 0;
    /// The stats the formulas run on: the actor's class (or NPC profile), level, effects and held values.
    virtual stats::CharacterStats& stats() = 0;
    /// The items the player wears (an NPC wears none).
    virtual std::vector<stats::WornItem> worn() const { return {}; }
};

}  // namespace aoc::worldd::combat
