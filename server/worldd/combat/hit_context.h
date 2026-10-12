// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  combat/hit_context.h  (plan E1, stage 3, part 1)
//
//  What an expression program reads of the hit it runs for, besides the stats of its actors: the hit's own tags (its element, Melee,
//  ComboStep.End ...), the flags the server rolled (Crit, Block, Miss ...), where the source of the hit comes from (an ability, a projectile ...),
//  how many targets it has, and the relation of the caster and the target.  A hit on someone other than the caster has one; the ability
//  runtime's host (session/ability_pipeline.cpp) and the NPCs' (combat/hit_resolver.cpp) both answer the host functions through it, so a damage
//  formula gives the same number whoever casts.
//
//  The host functions it answers are stats/stat_program.h's kHost... numbers: the seven damage functions (the profile over the instigator's and the
//  target's stat views and the hit), the distance and identity of two actors, whether the hit contains an actor, the type of the hit's source and
//  the relations IsEnemy / IsNeutral.  The others (a random number, an ability's charges and cooldown ...) belong to whoever runs the cast.
// ============================================================================
#pragma once

#include "stats/damage.h"

#include <cstdint>
#include <string>
#include <vector>

namespace aoc::worldd::combat {

/// EHitFlag names ("Crit", "Block", "Miss" ...) to the bits stats/damage.h gives.
uint8_t hit_flag_bit(const std::string& name);

/// A tag matches itself and its descendants ("Element.Physical.Slash" matches "Element.Physical").
bool tag_matches(const std::string& tag, const std::string& want);

struct HitContext {
    const stats::DamageProfile* profile = nullptr;
    stats::DamageHit            hit;                  // the element tag, the aspect and the flags the damage functions read
    std::vector<std::string>    tags;                 // the hit record's event tags and its ability's tags
    int64_t                     source_type = 0;      // the record type of the hit's source (content::kSourceType...)
    const stats::StatView*      instigator = nullptr; // the caster's stats
    const stats::StatView*      target = nullptr;     // the target's (null: the hit is on the caster)
    double                      targets = 1;          // how many targets the hit has
    double                      distance_cm = 0;      // caster to target
    bool                        enemy = true;         // the target is the caster's enemy (IsEnemy(caster, target))
    bool                        neutral = false;      // ... neutral to it
// C.O.A.Cyb

    /// CheckForDamageTag / HitHasTag: the hit carries the tag.
    bool has_tag(const std::string& tag) const;
    /// HitFlagsContains(GetHitFlags(hit, target), EHitFlag::name).
    bool has_flag(const std::string& name) const { return (hit.flags & hit_flag_bit(name)) != 0; }
    /// The named host functions this context answers (see the header): true, with `out`, when `fn` is one of them.
    bool host_fn(int fn, const std::string& name, const double* args, int n, double& out) const;
};

/// The element and aspect of a hit from its tags: the first "Element.<Physical|Magical>.*" tag (else a bare "Element.Physical" or "Element.Magical"),
/// and whether it scales with the physical aspect (ability.MagicalScalingOverride and ability.PhysicalScalingOverride decide first; with no element tag
/// the hit is physical).
struct HitAspect {
    std::string element;
    bool        physical = true;
};
HitAspect aspect_of(const std::vector<std::string>& tags);

}  // namespace aoc::worldd::combat
