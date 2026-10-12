// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  combat/combat_data.h  (plan E1, stage 3, part 1)
//
//  What the combat rules read of the design data, resolved once at startup:
//
//    the damage profile   the stats the seven damage functions read (DefaultCombat.ini's DamageProfile: content.db's combat_profile and
//                         combat_element; stats/damage.h)
//    the stats of rolls   the guids of the stats the hit rules read by name from the stat program (critical, block, evasion and accuracy chances
//                         of both aspects, ThreatGeneration, the vitals), found in the bundle by their names: a stat the bundle has none of is 0
//                         and every rule that reads it sees a stat that is always 0
//    the numbers          combat_config (the in-combat timer, the global damage and healing multipliers, the distances)
//    who fights whom      the alignments' relationships: an NPC whose alignments make it hostile to the Player alignment is an enemy of every player
//                         (CharacterAlignment.RelationshipsIds -> CharacterRelationship.CombatPermissions: 1 hostile, 2 friendly, 3 neutral)
// ============================================================================
#pragma once

#include "content/content_db.h"
#include "stats/damage.h"
#include "stats/stat_program.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace aoc::worldd::combat {

enum class Relation : uint8_t { Hostile, Friendly, Neutral };
// C Y B - C O A

/// The stats the hit rules read besides the damage profile's (0: the bundle has no such stat).
struct CombatStatIds {
    int64_t physical_crit_chance = 0, magical_crit_chance = 0;
    int64_t physical_crit_avoidance = 0, magical_crit_avoidance = 0;
    int64_t physical_accuracy = 0, magical_accuracy = 0;
    int64_t physical_evasion = 0, magical_evasion = 0;
    int64_t physical_block_chance = 0, magical_block_chance = 0;
    int64_t threat_generation = 0;
    int64_t health = 0, max_health = 0, mana = 0, max_mana = 0;
    int64_t move_speed_mult = 0;
    int64_t health_regeneration = 0;
};

class CombatData {
public:
    /// Builds from content.db's rows and the stat program (the stats by name). nullopt, with `err`, when the profile names a stat the bundle does not
    /// have or lacks a key.
    static std::optional<CombatData> build(const content::CombatRows& rows, const stats::StatBundle& bundle, std::string& err);

    const stats::DamageProfile& profile() const { return profile_; }
    const CombatStatIds&        ids() const { return ids_; }

    /// A number of combat_config (`fallback` when the file has none).
    double number(const std::string& key, double fallback) const;
    /// The seconds after the last hit dealt or taken that a combatant stays in combat (DefaultCombat.ini InCombatTimer).
    double in_combat_timer() const { return number(content::kCombatInCombatTimer, 6.0); }
    double global_damage_multiplier() const { return number(content::kCombatGlobalDamageMult, 1.0); }
    double global_healing_multiplier() const { return number(content::kCombatGlobalHealingMult, 1.0); }
    /// How far past its range the server lets a cast start (cm), and how far a client's hit may be from the actor it names (cm).
    double activation_leeway_cm() const { return number(content::kCombatActivationLeeway, 1000.0); }

    /// How the actors with alignments `a` stand to the ones with `b`: hostile when any relation either way is a hostile one, else friendly when one
    /// is friendly, else neutral.
    Relation relation(const std::vector<int64_t>& a, const std::vector<int64_t>& b) const;
    /// The alignments of a player (the Player alignment).
    static std::vector<int64_t> player_alignments();

private:
    stats::DamageProfile            profile_;
    CombatStatIds                   ids_;
    std::map<std::string, double>   config_;
    std::map<int64_t, int>          permissions_;
    std::map<int64_t, std::map<int64_t, int64_t>> relation_;
};

}  // namespace aoc::worldd::combat
