// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// PvP: contributed by MrJambix in #23.
// ============================================================================
//  combat/pvp_corruption.cpp  (PvP)  -- see pvp_corruption.h
// ============================================================================
#include "combat/pvp_corruption.h"

#include <algorithm>
#include <cmath>

namespace aoc::worldd::combat::pvp {

namespace {
int clamp_tier(int tier) { return std::clamp(tier, 0, 5); }
}  // namespace

double corruption_for_kill(int killer_level, int victim_level, double blight) {
    // "if delta >= 5, scale corruption: base 1 corruption per pk plus 0.2 per level" (the formula multiplies the whole delta: Max(0, Delta) * 0.2)
    const int delta = killer_level - victim_level;
    const double base = delta >= 5 ? 1.0 + std::max(0, delta) * 0.2 : 1.0;
    return base + std::max(0.0, blight) * 0.5;   // "player corruption gain scales with blight"
}

double blight_for_kill(int killer_level, int victim_level) {
    const int delta = killer_level - victim_level;
    return delta >= 5 ? 1.0 + std::max(0, delta) * 0.1 : 1.0;
}

double corruption_lost_dying(double corruption) {
    if (!(corruption > 0)) return 0;
    // "lose a little more than half ur corruption on death and always at least 1; effectively, each time you die you go down 1 corruption level"
    double reduce = std::max(2.0, corruption * 0.5);
    if (corruption - reduce < 1.0) reduce = corruption;
    return reduce;
}

double base_kill_xp(int level) { return level <= 1 ? 50.0 : 50.0 * (level + 1); }

double corruption_lost_killing(double base_xp, int player_level) {
    // baseRewardMult * baseXpMult * levelDeltaMult / avgKillsPerRedux: the kill's base experience (its reward mult, its level's Base_Kill_XP and the level difference's
    // decay, which the reward tables have already worked in) over the Base_Kill_XP of the player's level, over 40 ("on avg how many regular mobs it should take to
    // reduce a point of corruption"), clamped to 0..1
    if (!(base_xp > 0)) return 0;
    return std::clamp(base_xp / base_kill_xp(player_level) / 40.0, 0.0, 1.0);
}

double xp_debt_mult(int tier) {
    static constexpr double k[] = {1, 2, 4, 7, 10, 15};
    return k[clamp_tier(tier)];
}

double resource_drop_add(int tier) {
    static constexpr double k[] = {0, 0.25, 0.35, 0.45, 0.65, 1.0};
    return k[clamp_tier(tier)];
}

int item_drop_min(int tier) {
    static constexpr int k[] = {0, 1, 2, 3, 3, 3};
    return k[clamp_tier(tier)];
}

int item_drop_max(int tier) {
    static constexpr int k[] = {0, 3, 3, 5, 5, 6};
    return k[clamp_tier(tier)];
}

int64_t tier_effect(int tier) {
    static constexpr int64_t k[] = {0, 6064630485691858944ll, 6064630697904963785ll, 6064630697907060938ll, 6064630697907323083ll, 6064630697907585228ll};
    return k[clamp_tier(tier)];
}

}  // namespace aoc::worldd::combat::pvp
