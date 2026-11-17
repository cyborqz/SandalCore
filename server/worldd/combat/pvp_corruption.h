// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// PvP: contributed by MrJambix in #23.
// ============================================================================
//  combat/pvp_corruption.h  (PvP: corruption)
//
//  What a player kill and a death do to a character's Corruption, and what Corruption costs him, as the game's data writes it (GameStatProfile's
//  formulas and the CorruptionTier curves of content.db; the comments quote them).  Pure logic: the session keeps the numbers (session/player_pvp.cpp).
//
//    a kill of a non-combatant  CorruptionAddedOnKill: 1, or 1 + 0.2 x the levels the killer is above the victim when that is 5 or more; plus half his blight
//                               BlightAddedOnKill: 1, or 1 + 0.1 x the levels above when 5 or more (Corruption_Blight: it only grows)
//    a death while corrupted    CorruptionReductionOnDeath: at least 2, else half; all of it when less than 1 would remain
//    a creature's kill          CorruptionReductionOnKillNPC: the kill's base experience over a regular same-level kill's (Base_Kill_XP at the player's
//                               level), a fortieth of it, at most 1
//    the tier                   Corruption_Tier (combat/pvp.h corruption_tier), and by tier the experience debt's multiplier (2 % x 1 2 4 7 10 15), the
//                               resources dropped on death (+0 .25 .35 .45 .65 1), the worn items dropped on death (0..0 1..3 2..3 3..5 3..5 3..6),
//                               and the debuff Corruption_1 .. Corruption_5 (its stat mods read the tier: -25 % to -90 % mitigation, health and mana,
//                               -5 % to -25 % speed)
// ============================================================================
#pragma once

#include <cstdint>

namespace aoc::worldd::combat::pvp {

/// The Corruption_Blight stat (GameStatProfile.CorruptionSoulburnId: the owner's only).
inline constexpr int64_t kStatBlight = 6064630492098134016ll;

/// CorruptionAddedOnKill for a kill of a player (not an owned creature's, which is always 1).
double corruption_for_kill(int killer_level, int victim_level, double blight);
/// BlightAddedOnKill.
double blight_for_kill(int killer_level, int victim_level);
/// CorruptionReductionOnDeath: what a death takes off.
double corruption_lost_dying(double corruption);
/// CorruptionReductionOnKillNPC: what a creature's kill takes off, from its base experience (before the server's rate) and the player's level.
double corruption_lost_killing(double base_kill_xp, int player_level);
/// Base_Kill_XP: a regular creature's experience at a level (50 at level 1, then 50 x (level + 1)).
double base_kill_xp(int level);

/// CorruptionTier->XpDebtMult: the experience debt's multiplier of a tier.
double xp_debt_mult(int tier);
/// CorruptionTier->DropRateAdditive: what a tier adds to the share of resources dropped on death.
double resource_drop_add(int tier);
/// CorruptionTier->MinItemDrop / MaxItemDrop: how many worn items a death drops (CorruptedDeathNumItemsToDrop: RandIntRange(min, max)).
int item_drop_min(int tier);
int item_drop_max(int tier);
/// The debuff of a tier (Corruption_1 .. Corruption_5); 0 for tier 0.
int64_t tier_effect(int tier);

}  // namespace aoc::worldd::combat::pvp
