// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  market/market_tax.h
//
//  The sales tax of a Marketplace sale: the node's own rate (the MarketplaceTax row of the node's tax table, the one the window shows at the open), and the tax a cart adds
//  to a sum.
//
//    the table   what the open sends: the node's default table with the MarketplaceTax row set to the node's stored rate and both of its flags on.  The shops' tables stay as they are.
//    the rate    ONE function (sale_rate) gives the open's rate and the sale's rate: the client's climb (the first row, from the tag up the tag tree, with both flags set) over the
//                very table the open sends, so the window shows the rate the sale charges.
//    the tax     what the client's cart adds up: RoundToInt(float(sum) * TaxPercent + Flat), single precision, written as `cvtss2si(F + F + 0.5) >> 1` (the doubled
//                value rounds to the nearest whole number, so a half goes up: 0.5 is 1, 1.5 is 2): 100 is paid as 110 at 10 percent, 10,000 as 11,000, and 1 as 1.  The cart shows whole numbers of 32 bits, so a sum or a taxed sum of 2^31 or more is not
//                a sale (our rule, not the game's).
//    the money   the buyer pays the price plus the tax, the seller's mail carries the full price, and the tax goes to the node's total in the database (a node treasury will spend it).
// ============================================================================
#pragma once

#include "market/listings.h"
#include "protocol/repl/value.h"
#include "vendor/node_tax.h"

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace aoc::worldd::market {

/// The tag of the Marketplace's tax row.
inline constexpr const char* kMarketTaxTag = "NodeTax.GoodsTax.MarketplaceTax";

/// The tax table of a node the Marketplace's open sends: the node's default table with the MarketplaceTax row at `rate` and both its flags on.
inline ::aoc::protocol::repl::NodeTaxDetails market_tax_table(int64_t node, float rate) {
    ::aoc::protocol::repl::NodeTaxDetails table = vendor::default_table(node);
    for (::aoc::protocol::repl::NodeTaxEntry& e : table.entries) {
        if (e.tag == kMarketTaxTag) {
            e.tax_percent = rate;
            e.flag_a = true;
            e.flag_b = true;
        }
    }
    return table;
}

/// A tax row the climb found.
struct TaxRow {
    float   percent = 0.0f;
    int32_t flat = 0;
    bool    found = false;
};

/// The client's climb (FNodeCache::GetTaxDetailsByGameplayTag_Client): the first row of `table`, from `tag` up the tag tree, with both flags set.
inline TaxRow climb_tax(const ::aoc::protocol::repl::NodeTaxDetails& table, std::string_view tag) {
    std::string t(tag);
    while (!t.empty()) {
        for (const ::aoc::protocol::repl::NodeTaxEntry& e : table.entries) {
            if (e.tag == t && e.flag_a && e.flag_b) return TaxRow{e.tax_percent, e.flat_tax, true};
        }
        const size_t dot = t.rfind('.');
        if (dot == std::string::npos) break;
        t.resize(dot);
    }
    return TaxRow{};
}

/// The row the window shows and a sale charges for the table the open sends.
inline TaxRow sale_row(const ::aoc::protocol::repl::NodeTaxDetails& table) { return climb_tax(table, kMarketTaxTag); }

/// The rate of the window's table: what "Sales Tax:" shows and what a sale charges.
inline float sale_rate(const ::aoc::protocol::repl::NodeTaxDetails& table) {
    const TaxRow row = sale_row(table);
    return row.found ? row.percent : kDefaultTaxRate;
}

/// The tax a cart adds to `sum` copper: RoundToInt(float(sum) * percent + flat) in single precision (the client's `cvtss2si(F + F + 0.5) >> 1`).  nullopt for a negative sum and for
/// a value the client's own 32-bit arithmetic cannot hold.
inline std::optional<int64_t> cart_tax(int64_t sum, float percent, int32_t flat) {
    if (sum < 0) return std::nullopt;
    volatile float product = static_cast<float>(sum) * percent;   // (single precision at every step, and no fused multiply-add)
    volatile float f = product + static_cast<float>(flat);
    volatile float doubled = f + f;
    volatile float biased = doubled + 0.5f;
    const float b = biased;
    if (!(b > -2147483648.0f && b < 2147483648.0f)) return std::nullopt;
    const int32_t rounded = static_cast<int32_t>(std::nearbyint(b));   // (the default rounding mode: to the nearest whole number, ties to even, of the doubled value)
    return static_cast<int64_t>(rounded >> 1);
}

/// What a buyer pays for `quantity` units at `price` copper each: the sum, the tax on it and both together.
struct SaleCharge {
    int64_t sum = 0;
    int64_t tax = 0;
    int64_t total = 0;
};

/// The charge of a sale at the row's rate.  nullopt when the units or the price are not above 0, when the sum overflows, or when the sum or the taxed sum is 2^31 or more (what
/// the client's arithmetic cannot show).
inline std::optional<SaleCharge> charge_for(int64_t price, int32_t quantity, const TaxRow& row) {
    constexpr int64_t kLimit = int64_t{1} << 31;
    if (price <= 0 || quantity <= 0) return std::nullopt;
    if (price > kLimit / quantity) return std::nullopt;
    const int64_t sum = price * quantity;
    const std::optional<int64_t> tax = cart_tax(sum, row.percent, row.flat);
    if (!tax || *tax < 0 || sum + *tax >= kLimit) return std::nullopt;
    return SaleCharge{sum, *tax, sum + *tax};
}

}  // namespace aoc::worldd::market
