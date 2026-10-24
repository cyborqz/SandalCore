// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  crafting/rewards.h  (crafting, step 2: "collect it"; header only)
//
//  What a finished processing job makes and pays: its products, their quality, the profession's experience and Adventuring's.  Everything here is a pure function of the job (its recipe, its
//  length, what was paid, its guid) and the crafter's level, so a job shows and pays the same whenever it is looked at: when it ends, when its window is opened, when it is collected.
//
//  The products.  The recipe's reward table gives them for one unit; a job makes that many times its quantity.  Most tables give fixed items; the few that pick (a mount's training picks
//  one of two or more animals) pick once for the job, by the job's guid.  Items of one record are added up.
//
//  The quality.  Every product has the quality of what was paid: for each record the job took, the mean of its units' quality (a unit of a rarity band counts as the band's first
//  quality, since a job is saved with its bands and not with each stack's exact number), then the mean over the records, rounded up.
//
//  The experience.  The profession's: 50, 60 or 70 (Novice, Apprentice, Journeyman recipes; Master and Grand Master fall back to 50; Animal Husbandry ten times that) times 1, 1.5, 2, 2.5,
//  3, 3.5 or 4 by the products' rarity, times the quantity.  Adventuring's: what a kill at the crafter's level gives, a fifth of it for processing (half for crafting), times the recipe tier's
//  modifier at that level, times 1, 1.4, 2, 4, 8 or 20 by the rarity, times the quantity.  A character's artisan gear would raise the first; no character has any yet.  Both are rounded to
//  the nearest whole number once, for the whole job.
// ============================================================================
#pragma once

#include "content/content_db.h"
#include "crafting/job.h"
#include "crafting/rules.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace aoc::worldd::crafting {

/// The rarity band (Common 0 ... Artifact 6) a quality is in.
inline int rarity_of_quality(int quality) {
    int band = 0;
    for (int i = 0; i < 7; ++i) {
        if (quality >= kBandStart[i]) band = i;
    }
    return band;
}

/// The quality the products of a job that was paid with `paid` have (see the top of this file); 1 for a job that paid no item.
inline int product_quality(const std::vector<svc::JobResource>& paid) {
    double sum = 0;
    int records = 0;
    for (const svc::JobResource& r : paid) {
        double quality = 0;
        int64_t units = 0;
        for (const svc::JobRarity& x : r.rarities) {
            if (x.count < 1) continue;
            quality += static_cast<double>(band_lo(x.rarity)) * static_cast<double>(x.count);
            units += x.count;
        }
        if (units < 1) continue;
        sum += quality / static_cast<double>(units);
        ++records;
    }
    if (records == 0) return 1;
    return std::clamp(static_cast<int>(std::ceil(sum / records - 1e-9)), 1, kMaxQuality);
}

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

inline int64_t round_half_up(double v) { return static_cast<int64_t>(std::floor(v + 0.5)); }

inline constexpr double kProfessionXpByTier[3] = {50.0, 60.0, 70.0};
inline constexpr double kProfessionXpByRarity[7] = {1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0};
inline constexpr double kAdventuringXpByRarity[7] = {1.0, 1.4, 2.0, 4.0, 8.0, 20.0, 20.0};

/// The profession's experience for `units` units of a recipe of certification tier `tier` made at quality band `rarity`; 0 for a rule this server does not work out.
inline int64_t profession_xp(content::XpRule rule, int tier, int rarity, int64_t units) {
    if (rule != content::XpRule::Processing && rule != content::XpRule::AnimalHusbandry) return 0;
    const double base = (tier >= 0 && tier < 3 ? kProfessionXpByTier[tier] : kProfessionXpByTier[0]) * (rule == content::XpRule::AnimalHusbandry ? 10.0 : 1.0);
    return round_half_up(base * kProfessionXpByRarity[std::clamp(rarity, 0, 6)] * static_cast<double>(units));
}

/// Adventuring's experience for the same job, by the crafter's level.
inline int64_t adventuring_xp(const content::CraftXpCurves& curves, content::XpRule rule, int tier, int level, int rarity, int64_t units) {
    if ((rule != content::XpRule::Processing && rule != content::XpRule::Crafting) || !curves.ok()) return 0;
    const double share = rule == content::XpRule::Crafting ? 0.5 : 0.2;
    const double lv = static_cast<double>(std::max(level, 1));
    const double kill = eval_points(curves.kill, lv);
    const double modifier = eval_points(curves.tier[std::clamp(tier, 0, 2)], lv);
    return round_half_up(kill * share * modifier * kAdventuringXpByRarity[std::clamp(rarity, 0, 6)] * static_cast<double>(units));
}

/// A number from a job's guid for the picks of a reward table: the same job picks the same every time.
inline uint64_t seed_of(const std::string& job_guid) {
    uint64_t h = 1469598103934665603ull;
    for (const char c : job_guid) h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    return h;
}
inline uint64_t next_random(uint64_t& state) {
    uint64_t z = (state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

/// The products of `units` units of a recipe whose reward is `reward`.
inline std::vector<Product> roll_products(const content::CraftReward& reward, int64_t units, uint64_t seed) {
    std::vector<Product> out;
    const auto add = [&](const content::CraftRewardItem& it) {
        const int64_t quantity = it.quantity * units;
        if (quantity < 1) return;
        const auto have = std::find_if(out.begin(), out.end(), [&](const Product& p) { return p.item == it.item; });
        if (have != out.end()) have->quantity += quantity;
        else out.push_back({it.item, quantity});
    };
    uint64_t state = seed;
    for (const content::CraftRewardContainer& c : reward.containers) {
        std::vector<size_t> chosen;
        if (c.pick == content::CraftRewardContainer::Pick::All || c.rewards.size() <= 1) {
            for (size_t i = 0; i < c.rewards.size(); ++i) chosen.push_back(i);
        } else {
            std::vector<size_t> left;
            for (size_t i = 0; i < c.rewards.size(); ++i) left.push_back(i);
            const size_t want = std::min(left.size(), static_cast<size_t>(std::max(1, c.number)));
            while (chosen.size() < want && !left.empty()) {
                double total = 0;
                for (const size_t i : left) total += c.pick == content::CraftRewardContainer::Pick::WeightedRandom && i < c.weights.size() ? std::max(0.0, c.weights[i]) : 1.0;
                const double at = total > 0 ? static_cast<double>(next_random(state) >> 11) / 9007199254740992.0 * total : 0.0;
                double run = 0;
                size_t pick = 0;
                for (size_t k = 0; k < left.size(); ++k) {
                    run += c.pick == content::CraftRewardContainer::Pick::WeightedRandom && left[k] < c.weights.size() ? std::max(0.0, c.weights[left[k]]) : 1.0;
                    pick = k;
                    if (at < run) break;
                }
                chosen.push_back(left[pick]);
                left.erase(left.begin() + static_cast<std::ptrdiff_t>(pick));
            }
        }
        for (const size_t i : chosen) {
            for (const content::CraftRewardItem& it : c.rewards[i]) add(it);
        }
    }
    return out;
}

/// What the job makes and pays for a crafter of `level`; nullopt when its recipe is none of the data's.
inline std::optional<JobRewards> roll_job(const content::CraftRecipeData& data, const svc::ProcessingJob& job, int level) {
    const content::CraftRecipe* recipe = data.recipe(job.recipe_guid);
    if (!recipe) return std::nullopt;
    JobRewards out;
    out.profession = recipe->profession;
    out.level = std::max(level, 1);
    out.quality = product_quality(job.resources);
    out.rarity = rarity_of_quality(out.quality);
    if (const content::CraftReward* reward = data.reward_of(*recipe)) {
        out.products = roll_products(*reward, job.quantity, seed_of(job.job_guid));
        out.profession_xp = profession_xp(reward->profession_rule, recipe->tier, out.rarity, job.quantity);
        out.adventuring_xp = adventuring_xp(data.xp_curves, reward->adventuring_rule, recipe->tier, out.level, out.rarity, job.quantity);
    }
    return out;
}

}  // namespace aoc::worldd::crafting
