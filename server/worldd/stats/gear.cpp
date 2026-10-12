// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  stats/gear.cpp (see gear.h)
// ============================================================================
#include "stats/gear.h"

#include <cmath>
#include <map>

namespace aoc::worldd::stats {

namespace {

/// The rarity whose range holds the quality, else the last rarity that starts at or below it, else Common (gear.go's band).
int band_of(const ItemGear& item, double q, double& x, double& y) {
    bool found = false;
    int rarity = 0;
    for (int i = 0; i < kRarities; ++i) {
        const QualityBand& b = item.bands[static_cast<size_t>(i)];
        if (b.x <= q && q <= b.y) {
            x = b.x;
            y = b.y;
            return i;
        }
        if (b.x <= q) {
            rarity = i;
            x = b.x;
            y = b.y;
            found = true;
        }
    }
    if (!found) {
        x = item.bands[0].x;
        y = item.bands[0].y;
        return 0;
    }
    return rarity;
}
// CoA + C | Y | B

}  // namespace

GearInputs gear_inputs(const GearCatalog& catalog, std::span<const WornItem> worn, Rounding rounding) {
    GearInputs out;
    std::map<int64_t, double> sum;
    std::map<int64_t, std::vector<int>> set_rarities;   // by set guid, ascending: the order the sets are applied in
    for (const WornItem& w : worn) {
        const ItemGear* item = catalog.item(w.record);
        if (!item) {
            ++out.unknown_items;
            continue;
        }
        const double q = static_cast<double>(w.quality);
        double x = 0, y = 0;
        int rarity = 0;
        if (item->has_curve) rarity = band_of(*item, q, x, y);
        for (const BlockStat& s : item->stats) {
            double val;
            if (item->has_curve) {
                const double lo = s.min[static_cast<size_t>(rarity)], hi = s.max[static_cast<size_t>(rarity)];
                double t = 0.0;
                if (y != x) t = (q - x) / (y - x);
                val = lo + (hi - lo) * t;
            } else {
                val = s.min[0];
            }
            if (rounding == Rounding::HalfUp) val = std::floor(val + 0.5);
            sum[s.stat] += val;
        }
        for (const int64_t set : item->sets) set_rarities[set].push_back(rarity);
        for (const std::string& tag : item->tags) out.equipped.push_back({catalog.slot_name(w.slot), tag});
    }
    for (const auto& [guid, rarities] : set_rarities) {
        const SetBonus* set = catalog.set(guid);
        if (!set) {
            ++out.unknown_sets;
            continue;
        }
        int low = rarities[0];
        for (const int r : rarities) {
            if (r < low) low = r;
        }
        for (const SetTier& tier : set->tiers) {
            if (tier.pieces > static_cast<int>(rarities.size())) continue;
            for (const SetEntry& e : tier.entries) {
                const size_t r = static_cast<size_t>(low);
                sum[e.stat] += e.has[r] ? e.value[r] : 0.0;
            }
        }
    }
    out.mods.reserve(sum.size());
    for (const auto& [stat, value] : sum) out.mods.push_back({stat, value});
    return out;
}

}  // namespace aoc::worldd::stats
