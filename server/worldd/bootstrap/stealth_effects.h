// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  bootstrap/stealth_effects.h
//
//  The effects that stealth the character they are on.  The game's effect record has a flag for it (bAppliesStealth) and three records set it: Ranger_Camouflage,
//  Rogue_Stealth and Summoner_PhaseOut (the design database's only records with the flag on).  The retail server puts the pawn's StealthInfo.bInStealth up in the
//  packet that adds one of them and down in the packet that takes the last one off (the recordings of Camouflage on and off, of Rogue's Stealth and of Phase Out);
//  the client's stealth look of a character reads it (ABaseCharacter::IsStealthed), beside the effect's own visual and the character's StealthLevel.
// ============================================================================
#pragma once

#include <cstdint>

namespace aoc::worldd::bootstrap {

inline constexpr int64_t kEffectRangerCamouflage = 6064630841868877827ll;
inline constexpr int64_t kEffectRogueStealth = 6064632711256473912ll;
inline constexpr int64_t kEffectSummonerPhaseOut = 6064633866989608593ll;

/// The two stats the stealth rule reads: the character's stealth and its perception (both the character's level, from the game's combat stat list).
inline constexpr int64_t kStatStealthLevel = 6064633186496741774ll;
inline constexpr int64_t kStatStealthPerceptionLevel = 6064633186506047887ll;

/// Whether the effect record stealths the character that holds it.
constexpr bool applies_stealth(int64_t effect_guid) {
    return effect_guid == kEffectRangerCamouflage || effect_guid == kEffectRogueStealth || effect_guid == kEffectSummonerPhaseOut;
}

}  // namespace aoc::worldd::bootstrap
