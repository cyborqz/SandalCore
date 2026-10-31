// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/item_cast.h
//
//  The item a consumable's ability is cast with (food, drink, elixir, scroll, potion, salve).  Using such an item is the ability system's work: the client sends the
//  item's ability with the item's place in the bag, and what the ability gives (its effects' stat mods and durations) is read from the ITEM: the formulas ask
//  GetConsumedItemStat(stat), which is the stat's value in the item's stat block at the item's quality, the way worn gear's stats are.  The value is fixed when
//  the effect is applied, so the cast keeps the item's numbers: one of the stack leaves the bag at the cast's cost step, before the hit.
// ============================================================================
#pragma once

#include "stats/stat_engine.h"

#include <cstdint>
#include <string>
#include <vector>

namespace aoc::worldd {

struct ItemCast {
    int64_t                      record = 0;       // the item record
    int64_t                      ability = 0;      // the ability the item casts
    int                          quality = 1;      // the item's quality: the rarity band its stat block is read at
    std::vector<stats::StatMod>  stats;            // the stat block's values at that quality, by stat guid (what GetConsumedItemStat answers)
    int                          container = 0;    // the bag the item lay in (svc::ItemContainer as a number)
    int32_t                      slot = -1;        // ... and its slot there
    std::string                  instance_guid;    // ... and its instance
    bool                         consumes = true;  // a use takes one of the stack
};

}  // namespace aoc::worldd
