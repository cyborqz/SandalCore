// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  stats/damage.h: the damage formulas (plan E1, stage 3)
//
//  The seven functions the design data's DamageMod equation calls and the game implements natively (GameSystemsPlugin, Combat/Formulas/DamageFormula.cpp,
//  registered by name as the expression functions CalculateDamage, ...Multiplier, ...AdditiveMultiplier, ...Mitigation, ...Critical, ...Penetration and
//  ...Dampening). Each reads a few stats of the instigator or the target by the references of the combat config (the DamageProfile of DefaultCombat.ini,
//  content.db's combat_profile and combat_element) and the hit in progress: its element tag, whether it scales with the physical or the magical aspect, and
//  its flags (EHitFlag). The arithmetic is the exe's own: float32 reads, float32 products and sums in the order the machine code runs them, the final value
//  widened to double. services/internal/statprog/damage.go is the twin; the shared corpus (stats/testdata/corpus.txt, "damage" lines) holds both to the same bits.
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "stats/stat_program.h"

namespace aoc::worldd::stats {

// EHitFlag bits.
constexpr uint8_t kHitCrit = 1;
constexpr uint8_t kHitLethal = 2;
constexpr uint8_t kHitBlock = 4;
constexpr uint8_t kHitMiss = 8;
constexpr uint8_t kHitReflect = 16;
constexpr uint8_t kHitRedirect = 32;
constexpr uint8_t kHitActiveBlock = 64;

// The stat references of one aspect of damage (physical or magical).
struct DamageAspect {
    int64_t damage = 0, damage_mult = 0, damage_add_mult = 0, mitigation = 0;
    int64_t penetration = 0, critical_power = 0, critical_mitigation = 0, block_mitigation = 0;
};

// The stat references of one element (Element.Physical.Slash, Element.Magical.Fire ...).
struct DamageElement {
    std::string tag;
    int64_t damage = 0, damage_mult = 0, damage_add_mult = 0, mitigation = 0;
};

// The hit in progress as the damage functions see it.
struct DamageHit {
    std::string tag;       // the element tag of the hit ("Element.Physical.Slash"); empty for none
    bool physical = true;  // the hit scales with the physical aspect (else the magical one)
    uint8_t flags = 0;     // EHitFlag bits
};

// The current value of the stats of an actor by guid (0 for a stat it has none of).
class StatView {
public:
    virtual ~StatView() = default;
    virtual double stat_value(int64_t guid) const = 0;
};
// C Y B.C-O-A

struct DamageProfile {
    int64_t power_mult = 0, power_add_mult = 0, damage_power_mult = 0, damage_power_add_mult = 0;
    int64_t corruption = 0;
    DamageAspect physical, magical;
    std::vector<DamageElement> elements;

    const DamageElement* element(const std::string& tag) const;

    double calculate_damage(const StatView& instigator, const DamageHit& hit) const;
    double calculate_damage_multiplier(const StatView& instigator, const DamageHit& hit) const;
    double calculate_damage_additive_multiplier(const StatView& instigator, const DamageHit& hit) const;
    double calculate_damage_mitigation(const StatView& target, const DamageHit& hit) const;
    double calculate_damage_critical(const StatView& instigator, const DamageHit& hit) const;
    double calculate_damage_penetration(const StatView& instigator, const DamageHit& hit) const;
    double calculate_damage_dampening(const StatView& instigator, const StatView& target, const DamageHit& hit) const;

    // calculate runs the function with the host number kHostCalcDamage..kHostCalcLast: the instigator's stats for all but the mitigation, which is the
    // target's. 0 for another number.
    double calculate(int fn, const StatView& instigator, const StatView& target, const DamageHit& hit) const;

    const DamageAspect& aspect(bool is_physical) const { return is_physical ? physical : magical; }
};

// The keys of content.db's combat_profile (content.CombatProfileKeys on the Go side).
extern const char* const kCombatProfileKeys[21];
// set_profile_key sets the stat of a combat_profile key; false for a key that is none.
bool set_profile_key(DamageProfile& p, const std::string& key, int64_t guid);

}  // namespace aoc::worldd::stats
