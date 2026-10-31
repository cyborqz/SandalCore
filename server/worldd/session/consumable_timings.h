// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/consumable_timings.h
//
//  When the montage of a consumable's ability is cleared (the ability item is replaced by an empty one), counted from the use.  The four montage assets are shared by
//  all the consumables of a kind, so the table is by asset, not by ability.  A scroll's montage is cleared after about a second, an elixir's (the potion montage) after
//  about 1.9 s; a meal's montage runs on while the character eats and is ended by the ability, so it has no time here.
//  Numbers measured on the retail recordings of three scrolls (0.94, 0.98 and 1.22 s) and an elixir (1.86 s).
// ============================================================================
#pragma once

#include <string>

namespace aoc::worldd {

/// Seconds after the use at which the montage of the asset at `soft_path` is cleared; -1 for a montage that has no fixed end.
inline double consumable_montage_end(const std::string& soft_path) {
    if (soft_path.find("Consumable_Scroll") != std::string::npos) return 0.98;
    if (soft_path.find("Consumable_Potion") != std::string::npos) return 1.86;
    return -1;
}

}  // namespace aoc::worldd
