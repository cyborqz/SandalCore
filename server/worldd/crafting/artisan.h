// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  crafting/artisan.h  (crafting quality: "the artisan ratings"; header only)
//
//  A crafter carries rating stats for the crafting and processing professions (Artisan_Crafting_Carpentry_Quality_Rating, Artisan_Processing_Lumbermilling_Time_Rating ...).  The stat
//  engine holds them: worn artisan gear adds its stat block, and effects (potions, food, a guild's perks) will add theirs.  What a recipe makes of them is data, one formula for each outcome, and every
//  formula has the same shape: it adds up TWO ratings, the general one and the profession's own, and reads a curve at the sum (linear between the curve's points, flat past its ends).
//
//    crafting quality     the product's quality is its base quality plus the curve value (CraftingQuality: +3 at a rating of 316, +10 at 1,984, +25 at 8,409)
//    crafting experience  the profession experience of a craft is multiplied by 1 plus the curve value (CraftingXP: up to x6)
//    processing time      a job lasts its base time times the curve value (ProcessingTime: 1.0 at 0 down to 0.2)
//    processing quantity  each unit made has a chance of one more: the curve (ProcessingQuantityChance) of the unit's rarity, 10 % at 316 for a Common unit
//    processing rarity    each unit has a chance of coming out one rarity higher: the curve (ProcessingRarityIncrease) of the unit's rarity, 5 % at 316 for a Common unit
//    processing experience the profession experience of a job is multiplied by 1 plus the curve value (ProcessingXP: up to x6)
//
//  The ratings of a recipe's profession are read when a craft or a job starts (a job keeps what it was started with), so a pure function of the ratings and the data decides everything
//  here: nothing reads a file or the stat engine itself (the caller gives the stat reader).
// ============================================================================
#pragma once

#include "content/content_db.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace aoc::worldd::crafting {

/// A curve at `x`: linear between its points, flat past the ends, 0 for no points.
inline double eval_points(const std::vector<std::pair<double, double>>& points, double x) {
    if (points.empty()) return 0.0;
    if (x <= points.front().first) return points.front().second;
    for (size_t i = 1; i < points.size(); ++i) {
        if (x > points[i].first) continue;
        const auto& a = points[i - 1];
        const auto& b = points[i];
        return b.first > a.first ? a.second + (b.second - a.second) * (x - a.first) / (b.first - a.first) : b.second;
    }
    return points.back().second;
}

/// The ratings one recipe's formulas add up: for each outcome the general rating plus the rating of the recipe's own profession.  All 0 for a crafter with no artisan gear.
struct Ratings {
    double quality = 0, xp = 0, time = 0, quantity = 0, rarity = 0;
    bool any() const { return quality != 0 || xp != 0 || time != 0 || quantity != 0 || rarity != 0; }
};

/// Whether `profession` is a processing profession of the data (its recipes are jobs).
inline bool is_processing(const content::ArtisanData& a, int64_t profession) {
    const auto it = a.by_profession.find(profession);
    return it != a.by_profession.end() && it->second.processing;
}

/// The ratings of a crafter for the recipes of `profession`: `stat` reads the settled value of a rating stat by its guid (0 for a stat the crafter does not have).  A profession the data gives
/// no rating stats has none.
inline Ratings ratings_of(const content::ArtisanData& a, int64_t profession, const std::function<double(int64_t)>& stat) {
    Ratings r;
    const auto it = a.by_profession.find(profession);
    if (it == a.by_profession.end() || !stat) return r;
    const content::ArtisanProfession& own = it->second;
    const content::ArtisanProfession& general = own.processing ? a.general_processing : a.general_crafting;
    const auto sum = [&](int64_t general_stat, int64_t own_stat) { return (general_stat != 0 ? stat(general_stat) : 0.0) + (own_stat != 0 ? stat(own_stat) : 0.0); };
    r.quality = sum(general.quality, own.quality);
    r.xp = sum(general.xp, own.xp);
    r.time = sum(general.time, own.time);
    r.quantity = sum(general.quantity, own.quantity);
    r.rarity = sum(general.rarity, own.rarity);
    return r;
}

/// The extra quality of a crafted piece (a decimal: the rating's place on the CraftingQuality curve).
inline double quality_bonus(const content::ArtisanData& a, const Ratings& r) { return eval_points(a.crafting_quality, r.quality); }

/// What a job's base time is multiplied by (1.0 for a crafter with no time rating).
inline double time_multiplier(const content::ArtisanData& a, const Ratings& r) { return a.processing_time.empty() ? 1.0 : eval_points(a.processing_time, r.time); }

/// What the profession experience of a craft (`processing` false) or of a job is multiplied by: 1 for a crafter with no experience rating.
inline double xp_multiplier(const content::ArtisanData& a, const Ratings& r, bool processing) { return 1.0 + eval_points(processing ? a.processing_xp : a.crafting_xp, r.xp); }

/// The chance that a unit of rarity `rarity` (Common 0 ... Artifact 6) comes with one more (0 to 1); an Artifact unit never does.
inline double quantity_chance(const content::ArtisanData& a, const Ratings& r, int rarity) {
    if (rarity < 0 || rarity >= 6) return 0.0;
    return std::clamp(eval_points(a.quantity_chance[rarity], r.quantity), 0.0, 1.0);
}

/// The chance that a unit of rarity `rarity` comes out one rarity higher (0 to 1); a Legendary or Artifact unit never does.
inline double rarity_step_chance(const content::ArtisanData& a, const Ratings& r, int rarity) {
    if (rarity < 0 || rarity >= 5) return 0.0;
    return std::clamp(eval_points(a.rarity_step[rarity], r.rarity), 0.0, 1.0);
}

/// The whole quality an exact number becomes, at the very last step: the nearest whole number, a half up (9.98 is 10, 9.4 is 9, 9.5 is 10).  An item's quality is a whole number on the client (the
/// FItemInstance NetSerialize carries it as a packed int32), so this is where the exact number ends; nothing before it is rounded: not a piece's share of its rarity band and not the rating's
/// bonus (the community crafting calculator cuts both off; the server carries them exactly, 6 and 3.98 is 9.98 until here).  The one place that converts.
inline int quality_from(double v) { return static_cast<int>(std::floor(v + 0.5 + 1e-9)); }

}  // namespace aoc::worldd::crafting
