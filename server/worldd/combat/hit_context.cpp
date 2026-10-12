// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  combat/hit_context.cpp (see hit_context.h)
// ============================================================================
#include "combat/hit_context.h"

#include "stats/stat_engine.h"
#include "stats/stat_program.h"

#include <cstdlib>

namespace aoc::worldd::combat {

uint8_t hit_flag_bit(const std::string& name) {
    if (name == "Crit") return stats::kHitCrit;
    if (name == "Lethal") return stats::kHitLethal;
    if (name == "Block") return stats::kHitBlock;
    if (name == "Miss") return stats::kHitMiss;
    if (name == "Reflect") return stats::kHitReflect;
    if (name == "Redirect") return stats::kHitRedirect;
    if (name == "ActiveBlock") return stats::kHitActiveBlock;
    return 0;
}

bool tag_matches(const std::string& tag, const std::string& want) {
    return stats::tag_matches(tag, want);   // (one rule: FNames, compared without regard to case)
}

bool HitContext::has_tag(const std::string& tag) const {
    for (const std::string& t : tags) {
        if (tag_matches(t, tag)) return true;
    }
    return false;
}

namespace {

// "A,B": the roles of two actors.
bool parse_pair(const std::string& s, int& a, int& b) {
    const size_t comma = s.find(',');
    if (comma == std::string::npos) return false;
    a = std::atoi(s.substr(0, comma).c_str());
    b = std::atoi(s.substr(comma + 1).c_str());
    return true;
}

}  // namespace

bool HitContext::host_fn(int fn, const std::string& name, const double* /*args*/, int /*n*/, double& out) const {
    if (fn >= stats::kHostCalcDamage && fn <= stats::kHostCalcLast) {
        if (!profile || !instigator) {
            out = 0;
            return true;
        }
        const stats::StatView& tgt = target ? *target : *instigator;   // a hit on the caster itself
        out = profile->calculate(fn, *instigator, tgt, hit);
        return true;
    }
    switch (fn) {
        case stats::kHostDistanceBetween: {
            int a = 0, b = 0;
            out = (parse_pair(name, a, b) && a != b) ? distance_cm : 0.0;
            return true;
        }
        case stats::kHostIsClient:
            out = 0;
            return true;
        case stats::kHostSameActor: {
            int a = 0, b = 0;
            out = (parse_pair(name, a, b) && (a == b || target == nullptr)) ? 1.0 : 0.0;
            return true;
        }
        case stats::kHostHitContains: {
            const int role = std::atoi(name.c_str());
            out = (role == stats::kRoleTarget || role == stats::kRoleClosestInHit) && target ? 1.0 : 0.0;
            return true;
        }
        case stats::kHostHitSourceType:
            out = static_cast<double>(source_type);
            return true;
        case stats::kHostIsEnemy:
        case stats::kHostIsNeutral: {
            int a = 0, b = 0;
            if (!parse_pair(name, a, b) || a == b || !target) {
                out = 0;
                return true;
            }
            // the caster and the target (either way round): the relation of the hit; any other pair is the caster's own side
            const bool pair = (a == stats::kRoleCaster && b == stats::kRoleTarget) || (a == stats::kRoleTarget && b == stats::kRoleCaster);
            if (!pair) {
                out = 0;
                return true;
            }
            out = (fn == stats::kHostIsEnemy ? enemy : neutral) ? 1.0 : 0.0;
            return true;
        }
        default:
            return false;
    }
}
// -- C_Y_B x C.O.A --

HitAspect aspect_of(const std::vector<std::string>& tags) {
    HitAspect a;
    std::string physical, magical, bare_physical, bare_magical;
    bool force_magical = false, force_physical = false;
    for (const std::string& t : tags) {
        if (t == "ability.MagicalScalingOverride") force_magical = true;
        else if (t == "ability.PhysicalScalingOverride") force_physical = true;
        else if (t.rfind("Element.Physical.", 0) == 0) {
            if (physical.empty()) physical = t;
        } else if (t.rfind("Element.Magical.", 0) == 0) {
            if (magical.empty()) magical = t;
        } else if (t == "Element.Physical") {
            bare_physical = t;
        } else if (t == "Element.Magical") {
            bare_magical = t;
        }
    }
    if (!physical.empty()) {
        a.element = physical;
        a.physical = true;
    } else if (!magical.empty()) {
        a.element = magical;
        a.physical = false;
    } else if (!bare_physical.empty()) {
        a.element = bare_physical;
        a.physical = true;
    } else if (!bare_magical.empty()) {
        a.element = bare_magical;
        a.physical = false;
    }
    if (force_magical) a.physical = false;
    else if (force_physical) a.physical = true;
    return a;
}

}  // namespace aoc::worldd::combat
