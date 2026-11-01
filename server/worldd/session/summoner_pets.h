// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/summoner_pets.h: the Summoner's three pets, by their records in the design data
// ============================================================================
#pragma once

#include <cstdint>

namespace aoc::worldd::pets {

// Ability, effect and SpawnNPCRecordBase identities from the design database.
// The summon montage has no branch request. Its captured end is about 8.38 s;
// completion at 8 s is modelled, leaving the montage's short recovery afterward.
inline constexpr double kSummonSeconds = 8.0;
struct Definition {
    int64_t ability, effect;
    const char* name;
    const char* archetype;
    int64_t attack;     // its basic attack (the Hunter's melee one: it starts in that stance)
    double  reach_cm;   // how near its abilities need a target
};
inline constexpr Definition kDefinitions[] = {
    {6064633584217554944ll, 6064633584224960514ll, "Spirit Guardian",
     "/Game/DesignAssets/Population/NPC/TestNPCs/Pets/Guardian/Pet_Summoner_Guardian.Default__Pet_Summoner_Guardian_C", 6064633577425274287ll, 300.0},
    {6064633854969511983ll, 6064633826102476801ll, "Spirit Hunter",
     "/Game/DesignAssets/Population/NPC/TestNPCs/Pets/Hunter/Pet_Summoner_Hunter.Default__Pet_Summoner_Hunter_C", 6064633577428813236ll, 300.0},
    {6064633809144250565ll, 6064633809137500355ll, "Spirit Mystic",
     "/Game/DesignAssets/Population/NPC/TestNPCs/Pets/Mystic/Pet_Summoner_Mystic.Default__Pet_Summoner_Mystic_C", 6064633577431958969ll, 1500.0},
};
inline const Definition* for_ability(int64_t guid) {
    for (const auto& d : kDefinitions) if (d.ability == guid) return &d;
    return nullptr;
}
inline const Definition* for_effect(int64_t guid) {
    for (const auto& d : kDefinitions) if (d.effect == guid) return &d;
    return nullptr;
}
inline constexpr int64_t kDismiss = 6064633780525466222ll;
inline constexpr int64_t kFollow = 6064633690279903233ll;
inline constexpr int64_t kStay = 6064633774860402690ll;
inline constexpr int64_t kAttack = 6064633690276102144ll;
inline constexpr int64_t kGoTo = 6064633723838726145ll;
inline constexpr int64_t kAggressive = 6064633724790243328ll;
inline constexpr int64_t kDefensive = 6064633724791750657ll;
inline constexpr int64_t kPassive = 6064633724791816194ll;

// The abilities a pet plays when its owner asks (their PetCommandId is General_UseAbility*; the Stance Swap names none and is cast as an ability [R: 476, 477]).
inline constexpr int64_t kUseAbility = 6064633804669583415ll;
inline constexpr int64_t kUseAbilityNonCombat = 6064634080500056064ll;
inline constexpr int64_t kUseAbilityNonTarget = 6064634478170341376ll;
inline constexpr int64_t kGuardianTaunt = 6064633780078903470ll;
inline constexpr int64_t kHunterStanceSwap = 6064633577431434680ll;
inline constexpr int64_t kHunterLunge = 6064633577430189494ll;
inline constexpr int64_t kHunterIncapacitate = 6064633577429468597ll;
inline constexpr int64_t kHunterIncapMelee = 6064634064199483408ll;
inline constexpr int64_t kHunterIncapRanged = 6064634206203946297ll;
inline constexpr int64_t kHunterRangedAttack = 6064634052822827010ll;
inline constexpr int64_t kHunterRefMelee = 6064634094316027906ll;
inline constexpr int64_t kHunterRefRanged = 6064634094322712579ll;
inline constexpr double  kHunterRangedReach = 1500.0;

// Dimensional Bolt: the first press throws the bolt and marks the target. Its montages' select expressions make the next press on a marked target, with a pet out, send the pet there
// (a Hunter behind it) to attack; with no mark, nothing may be cast until the lockouts are over. Lockout_2 ending, or that second press, starts the cooldown [R: Bolt 442 and 451, CD 12.0].
inline constexpr int64_t kBolt = 6064633731255289624ll;
inline constexpr int64_t kBoltMark = 6064633742197423059ll;
inline constexpr int64_t kBoltLockout = 6064633742768605729ll;
inline constexpr int64_t kBoltLockout2 = 6064633742917640384ll;
inline constexpr int64_t kBoltRemoveHit = 6064633742803078007ll;
inline constexpr int64_t kBoltCooldownHit = 6064633742955587572ll;
inline constexpr double  kBoltArrivalCm = 150.0;        // [modelled: the teleport's own place is in its Blueprint script]
inline constexpr int64_t kHunterMeleeIncapHit = 6064634064216195089ll;   // its ScriptOnHit is Test_Pet_FollowEvent: the Hunter lets go of what it just incapacitated
inline constexpr double  kLeashCm = 4000.0;             // Summoner_Pet_LeashTeleport brings a following pet this far off back [modelled: the distance is in BP_PetLeashTeleportScript]
/// The stats an effect on a pet multiplies what the pet deals by (Amplify).
inline constexpr int64_t kStatDamageMult = 6064630854084526095ll, kStatHealingMult = 6064630634099769360ll;
/// When a pet uses an ability by itself: each ability's AIUseAbilityConditionId (all of them also ask for the Summoner's auto mode, TEMP_Pet_Automation).
enum class AutoRule : uint8_t {
    Never,          // NeverConsider
    NotMostHated,   // NotMostHated: the creature is not on the pet
    AtMaxEssence,   // SummonerAtMaxEssence: the Summoner has Essence to spare
    HealBelow80,    // TargetHealth_Below%_80: the owner or the pet
    Cleanse,        // TargetHasDispellableEffect: the owner or the pet
};
struct PetAbility {
    int64_t  ability;
    int64_t  pet_effect;   // the Definition's effect
    AutoRule rule;
};
inline constexpr int64_t kAutomationAbility = 6064634126074249222ll;   // TEMP_Pet_Automation: cast from the bar, it turns the mode on and off
inline constexpr int64_t kAutomationEffect = 6064634126094237703ll;
inline constexpr int64_t kEssence = 6064633668368219158ll;
inline constexpr float   kEssenceMax = 100.0f;                          // the stat's own maximum
inline constexpr double  kGuardianTauntReach = 2000.0;                  // the record's AreaTargetRange: it is shouted, not walked up to
inline constexpr int64_t kGuardianEffect = 6064633584224960514ll, kHunterEffect = 6064633826102476801ll, kMysticEffect = 6064633809137500355ll;
inline constexpr PetAbility kPetAbilities[] = {
    {6064634403600334848ll, kGuardianEffect, AutoRule::Never},          // Brace
    {kGuardianTaunt, kGuardianEffect, AutoRule::NotMostHated},
    {6064633577427633586ll, kGuardianEffect, AutoRule::AtMaxEssence},   // Stomp
    {6064633577430189494ll, kHunterEffect, AutoRule::Never},            // Lunge
    {6064633577429468597ll, kHunterEffect, AutoRule::Never},            // Incapacitate
    {6064633577430779319ll, kHunterEffect, AutoRule::AtMaxEssence},     // Eviscerate
    {kHunterStanceSwap, kHunterEffect, AutoRule::Never},
    {6064633577432614330ll, kMysticEffect, AutoRule::HealBelow80},      // Recreate
    {6064634103529603072ll, kMysticEffect, AutoRule::Cleanse},          // Cleanse
    {6064633577433793980ll, kMysticEffect, AutoRule::AtMaxEssence},     // Essence Eruption
};
/// The commands of the pet bar, each cast through its own ability, as the client's own slots name them.
struct PetBarCommand {
    int64_t ability;
    int64_t command;
};
inline constexpr PetBarCommand kPetBarCommands[] = {
    {6064633701503401988ll, kAttack},
    {6064633576846066210ll, kFollow},
    {6064633576956297816ll, kStay},
    {6064633723849342980ll, kGoTo},
    {6064633724848504841ll, kAggressive},
    {6064633724849553418ll, kDefensive},
    {6064633724850470923ll, kPassive},
    {6064633780469564013ll, kDismiss},
};
/// The PetCommandId of a pet's ability on the bar (its record's).
inline int64_t bar_command_of(int64_t ability) {
    switch (ability) {
        case 6064634403600334848ll: case 6064633577427633586ll: return kUseAbilityNonTarget;
        case 6064633577430189494ll: return kUseAbility;
        default: return kUseAbilityNonCombat;
    }
}
/// The pet an ability belongs to (its Definition's effect), 0: it is no pet's special ability.
inline int64_t pet_of_ability(int64_t ability) {
    for (const PetAbility& a : kPetAbilities) if (a.ability == ability) return a.pet_effect;
    return 0;
}
/// Where a hit of a pet's ability lands, when that is more than its one target: the sizes are its targeting records'.
struct HitArea {
    double radius_cm = 0;
    bool   around_pet = false;    // else around its target
    bool   others_only = false;   // not on the target itself
};
inline HitArea area_of_hit(int64_t hit) {
    switch (hit) {
        case 6064633821443985837ll: return {750.0, true, false};    // Guardian_Stomp_Hit
        case 6064634404685029632ll: return {350.0, false, true};    // Hunter_Eviscerate_AOE: a box 500 cm across
        case 6064634103603658841ll: return {600.0, false, false};   // Mystic_EssenceEruption_Hit
        default: return {};
    }
}

/// EPetCommandStance (layouts/aoc.usmap).
enum class Stance : int { Aggressive = 1, Defensive = 2, Passive = 3 };

/// The command an ability of the Summoner's bar gives (its record's PetCommandId): the client casts these like any ability [R: Dismiss and GotoLocation in the Summoner captures].
/// 0 for any other.  Dismiss is not listed: its own hit takes the summon's effect off, and the pet goes with the effect.
inline int64_t command_of_ability(int64_t ability) {
    switch (ability) {
        case 6064633701503401988ll: return kAttack;       // Summoner_PetCommand_Sic
        case 6064633576846066210ll: return kFollow;       // Summoner_PetCommand_Follow
        case 6064633576956297816ll: return kStay;         // Summoner_PetCommand_Stay
        case 6064633723849342980ll: return kGoTo;         // Summoner_PetCommand_GotoLocation
        case 6064633724848504841ll: return kAggressive;   // Summoner_PetCommand_AssistStance
        case 6064633724849553418ll: return kDefensive;    // Summoner_PetCommand_DefensiveStance
        case 6064633724850470923ll: return kPassive;      // Summoner_PetCommand_PassiveStance
        default: return 0;
    }
}

} // namespace aoc::worldd::pets
