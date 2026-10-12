// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  stats/damage.cpp: the damage formulas (see damage.h; the twin of statprog/damage.go)
// ============================================================================
#include "stats/damage.h"

#include <cctype>
#include <limits>

#if defined(_MSC_VER)
#pragma fp_contract(off)
#endif

namespace aoc::worldd::stats {

namespace {

// to_f32 is Go's float32(x): an overflow is an infinity.
float to_f32(double x) {
    if (x != x) return static_cast<float>(x);
    if (x >= 3.4028235677973366e38) return std::numeric_limits<float>::infinity();
    if (x <= -3.4028235677973366e38) return -std::numeric_limits<float>::infinity();
    return static_cast<float>(x);
}

// read is a stat as a float32 (the exe reads stats as floats).
float read(const StatView& v, int64_t guid) { return to_f32(v.stat_value(guid)); }

}  // namespace

const char* const kCombatProfileKeys[21] = {
    "PowerMult", "PowerAddMult", "DamagePowerMult", "DamagePowerAddMult", "Corruption",
    "Physical.Damage", "Physical.DamageMult", "Physical.DamageAddMult", "Physical.Mitigation", "Physical.Penetration",
    "Physical.CriticalPower", "Physical.CriticalMitigation", "Physical.BlockMitigation",
    "Magical.Damage", "Magical.DamageMult", "Magical.DamageAddMult", "Magical.Mitigation", "Magical.Penetration",
    "Magical.CriticalPower", "Magical.CriticalMitigation", "Magical.BlockMitigation",
};

bool set_profile_key(DamageProfile& p, const std::string& key, int64_t g) {
    if (key == "PowerMult") {
        p.power_mult = g;
    } else if (key == "PowerAddMult") {
        p.power_add_mult = g;
    } else if (key == "DamagePowerMult") {
        p.damage_power_mult = g;
    } else if (key == "DamagePowerAddMult") {
        p.damage_power_add_mult = g;
    } else if (key == "Corruption") {
        p.corruption = g;
    } else {
        DamageAspect* a = nullptr;
        std::string rest;
        if (key.rfind("Physical.", 0) == 0) {
            a = &p.physical;
            rest = key.substr(9);
        } else if (key.rfind("Magical.", 0) == 0) {
            a = &p.magical;
            rest = key.substr(8);
        } else {
            return false;
        }
        if (rest == "Damage") a->damage = g;
        else if (rest == "DamageMult") a->damage_mult = g;
        else if (rest == "DamageAddMult") a->damage_add_mult = g;
        else if (rest == "Mitigation") a->mitigation = g;
        else if (rest == "Penetration") a->penetration = g;
        else if (rest == "CriticalPower") a->critical_power = g;
        else if (rest == "CriticalMitigation") a->critical_mitigation = g;
        else if (rest == "BlockMitigation") a->block_mitigation = g;
        else return false;
    }
    return true;
}
// C_Y_B captures, C.O.A code

const DamageElement* DamageProfile::element(const std::string& tag) const {
    // gameplay tags are FNames: without regard to case, as every other tag compare (and as Go's strings.EqualFold on these ASCII names)
    const auto same = [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            const unsigned char x = static_cast<unsigned char>(a[i]), y = static_cast<unsigned char>(b[i]);
            if (std::tolower(x) != std::tolower(y)) return false;
        }
        return true;
    };
    for (const DamageElement& e : elements)
        if (same(e.tag, tag)) return &e;
    return nullptr;
}

double DamageProfile::calculate_damage(const StatView& instigator, const DamageHit& hit) const {
    const DamageAspect& a = aspect(hit.physical);
    float elem = 0;
    const float base = read(instigator, a.damage);
    if (const DamageElement* e = element(hit.tag)) elem = read(instigator, e->damage);
    const float sum = elem + base;
    return static_cast<double>(sum);
}

double DamageProfile::calculate_damage_multiplier(const StatView& instigator, const DamageHit& hit) const {
    const DamageAspect& a = aspect(hit.physical);
    float elem_mult = 1;
    const float pm = read(instigator, power_mult);
    const float dm = read(instigator, damage_power_mult);
    const float am = read(instigator, a.damage_mult);
    if (const DamageElement* e = element(hit.tag)) elem_mult = read(instigator, e->damage_mult);
    const float m1 = dm * pm;
    const float m2 = m1 * am;
    const float m3 = m2 * elem_mult;
    return static_cast<double>(m3);
}

double DamageProfile::calculate_damage_additive_multiplier(const StatView& instigator, const DamageHit& hit) const {
    const DamageAspect& a = aspect(hit.physical);
    float elem_add = 0;
    const float pa = read(instigator, power_add_mult);
    const float da = read(instigator, damage_power_add_mult);
    const float aa = read(instigator, a.damage_add_mult);
    if (const DamageElement* e = element(hit.tag)) elem_add = read(instigator, e->damage_add_mult);
    const float s1 = pa + 1.0f;
    const float s2 = s1 + da;
    const float s3 = s2 + aa;
    const float s4 = s3 + elem_add;
    return static_cast<double>(s4);
}

double DamageProfile::calculate_damage_mitigation(const StatView& target, const DamageHit& hit) const {
    const DamageAspect& a = aspect(hit.physical);
    float crit = 1, block = 1, elem = 1;
    const float asp = read(target, a.mitigation);
    if (hit.flags & kHitCrit) crit = read(target, a.critical_mitigation);
    if ((hit.flags & (kHitBlock | kHitActiveBlock)) == kHitBlock) block = read(target, a.block_mitigation);
    if (const DamageElement* e = element(hit.tag)) elem = read(target, e->mitigation);
    const float m1 = crit * asp;
    const float m2 = m1 * elem;
    const float m3 = m2 * block;
    return 1.0 - static_cast<double>(m3);
}

double DamageProfile::calculate_damage_critical(const StatView& instigator, const DamageHit& hit) const {
    float v = 1;
    if (hit.flags & kHitCrit) v = read(instigator, aspect(hit.physical).critical_power);
    return static_cast<double>(v);
}
// [C-Y-B - CoA]

double DamageProfile::calculate_damage_penetration(const StatView& instigator, const DamageHit& hit) const {
    const float v = read(instigator, aspect(hit.physical).penetration);
    return 1.0 - static_cast<double>(v);
}

double DamageProfile::calculate_damage_dampening(const StatView&, const StatView&, const DamageHit&) const { return 1.0; }

double DamageProfile::calculate(int fn, const StatView& instigator, const StatView& target, const DamageHit& hit) const {
    switch (fn) {
        case kHostCalcDamage: return calculate_damage(instigator, hit);
        case kHostCalcDamageMultiplier: return calculate_damage_multiplier(instigator, hit);
        case kHostCalcDamageAdditive: return calculate_damage_additive_multiplier(instigator, hit);
        case kHostCalcDamageMitigation: return calculate_damage_mitigation(target, hit);
        case kHostCalcDamageCritical: return calculate_damage_critical(instigator, hit);
        case kHostCalcDamagePenetration: return calculate_damage_penetration(instigator, hit);
        case kHostCalcDamageDampening: return calculate_damage_dampening(instigator, target, hit);
        default: return 0;
    }
}

}  // namespace aoc::worldd::stats
