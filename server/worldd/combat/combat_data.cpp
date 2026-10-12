// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  combat/combat_data.cpp (see combat_data.h)
// ============================================================================
#include "combat/combat_data.h"

namespace aoc::worldd::combat {

std::optional<CombatData> CombatData::build(const content::CombatRows& rows, const stats::StatBundle& bundle, std::string& err) {
    CombatData d;
    for (const char* const key : stats::kCombatProfileKeys) {
        const auto it = rows.profile.find(key);
        if (it == rows.profile.end()) {
            err = std::string("the combat profile has no ") + key + " (a content.db built by another contentc?)";
            return std::nullopt;
        }
        stats::set_profile_key(d.profile_, key, it->second);
        if (bundle.by_guid.find(it->second) == bundle.by_guid.end()) {
            err = std::string("the combat profile's ") + key + " is stat " + std::to_string(it->second) + ", which the stat program does not have";
            return std::nullopt;
        }
    }
    for (const content::CombatRows::Element& e : rows.elements) {
        stats::DamageElement el;
        el.tag = e.tag;
        el.damage = e.damage;
        el.damage_mult = e.damage_mult;
        el.damage_add_mult = e.damage_add_mult;
        el.mitigation = e.mitigation;
        d.profile_.elements.push_back(std::move(el));
    }
    d.config_ = rows.config;
    d.permissions_ = rows.permissions;
    d.relation_ = rows.relation;
// C.Y.B / CoA

    std::map<std::string, int64_t> by_name;
    for (size_t i = 0; i < bundle.stats.size(); ++i) by_name[bundle.stat_name(i)] = bundle.stats[i].guid;
    const auto find = [&](const char* name) -> int64_t {
        const auto it = by_name.find(name);
        return it == by_name.end() ? 0 : it->second;
    };
    CombatStatIds& s = d.ids_;
    s.physical_crit_chance = find("PhysicalCriticalChance");
    s.magical_crit_chance = find("MagicalCriticalChance");
    s.physical_crit_avoidance = find("PhysicalCriticalAvoidance");
    s.magical_crit_avoidance = find("MagicalCriticalAvoidance");
    s.physical_accuracy = find("PhysicalAccuracy");
    s.magical_accuracy = find("MagicalAccuracy");
    s.physical_evasion = find("PhysicalEvasion");
    s.magical_evasion = find("MagicalEvasion");
    s.physical_block_chance = find("PhysicalBlockChance");
    s.magical_block_chance = find("MagicalBlockChance");
    s.threat_generation = find("ThreatGeneration");
    s.health = find("Health");
    s.max_health = find("MaxHealth");
    s.mana = find("Mana");
    s.max_mana = find("MaxMana");
    s.move_speed_mult = find("MoveSpeedMult");
    s.health_regeneration = find("HealthRegeneration");
    return d;
}

double CombatData::number(const std::string& key, double fallback) const {
    const auto it = config_.find(key);
    return it == config_.end() ? fallback : it->second;
}

Relation CombatData::relation(const std::vector<int64_t>& a, const std::vector<int64_t>& b) const {
    bool friendly = false;
    const auto look = [&](int64_t x, int64_t y) -> int {
        const auto ax = relation_.find(x);
        if (ax == relation_.end()) return 0;
        const auto r = ax->second.find(y);
        if (r == ax->second.end()) return 0;
        const auto p = permissions_.find(r->second);
        return p == permissions_.end() ? 0 : p->second;
    };
    for (const int64_t x : a) {
        for (const int64_t y : b) {
            for (const int p : {look(x, y), look(y, x)}) {
                if (p == content::kPermissionHostile) return Relation::Hostile;
                if (p == content::kPermissionFriendly) friendly = true;
            }
        }
    }
    return friendly ? Relation::Friendly : Relation::Neutral;
}

std::vector<int64_t> CombatData::player_alignments() { return {content::kAlignmentPlayer}; }

}  // namespace aoc::worldd::combat
