// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  market/window.h
//
//  The marketplace window a session has open: the Marketplace Manager it last opened and the node his market is at.
//  Every marketplace request acts only on that Manager and only while the player stays within the reach the game
//  allows (1000 units, 10 m).  The state is one struct so a session keeps no growing list of market fields.
// ============================================================================
#pragma once

#include <cstdint>

namespace aoc::worldd::market {

/// The Manager a session opened last: his registry actor id (0: no window) and the node of his market.
struct MarketWindow {
    uint64_t manager = 0;
    int64_t  node = 0;
    bool operator==(const MarketWindow&) const = default;
};

/// How close to that Manager a marketplace request may come (cm): 1000 units, 10 m.
inline constexpr double kWindowRangeCm = 1000.0;

}  // namespace aoc::worldd::market
