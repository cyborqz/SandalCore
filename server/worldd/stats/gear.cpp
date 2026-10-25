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

// CoA + C | Y | B

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
        const int rarity = item->rarity_of(q);   // the rarity the item shows: its band held inside its RarityMin..RarityMax
        for (const BlockStat& s : item->stats) sum[s.stat] += item->stat_at(s, q, rounding == Rounding::HalfUp);
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
