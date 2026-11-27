// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  market/fee.h
//
//  The fee a Marketplace listing costs, from the client's own rule: the larger of the item minimum ((rarity + 1) x Level x 5) and 5%
//  of the listing's value (quantity x unit price in copper), rounded up to the next copper and never past 1,000,000.  The rarity is the
//  one the item's record shows at the stack's quality, by the server's own rule (items/rarity.h, inside RarityMin..RarityMax).
//  Pure: plain numbers, no database and no session.
// ============================================================================
#pragma once

#include "items/rarity.h"

#include <algorithm>
#include <cstdint>

namespace aoc::worldd::market {

/// The most the price part of the fee may be (copper).
inline constexpr int64_t kMaxPriceFee = 1'000'000;
/// The price part's divisor: 5% is one twentieth.
inline constexpr int64_t kPriceFeeDivisor = 20;

/// The fee of listing `quantity` units at `unit_price_copper` each: the larger of the item minimum and the price part.
inline int64_t listing_fee(const items::ItemRarityFacts& facts, int quality, int level, int32_t quantity, int64_t unit_price_copper) {
    const int rarity = std::clamp(facts.rarity_of(static_cast<double>(quality)), 0, items::kRarityCount - 1);
    const int64_t item_minimum = static_cast<int64_t>(rarity + 1) * std::max(0, level) * 5;
    int64_t price_part = 0;
    if (quantity > 0 && unit_price_copper > 0) {
        const int64_t units = quantity;
        if (units > kMaxPriceFee * kPriceFeeDivisor / unit_price_copper) {
            price_part = kMaxPriceFee;
        } else {
            const int64_t value = units * unit_price_copper;
            price_part = std::min(kMaxPriceFee, (value + kPriceFeeDivisor - 1) / kPriceFeeDivisor);
        }
    }
    return std::max(item_minimum, price_part);
}

}  // namespace aoc::worldd::market
