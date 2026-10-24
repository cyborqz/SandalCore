// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  items/wallet.h  (the vendors milestone, step 0: the purse)
//
//  The money a character carries: a balance per currency, each in the currency's BASE unit (copper for money: 100 copper = 1 silver, 100 silver = 1 gold).  A currency is named
//  by its base def's Guid (Copper, Node currency); a price and a callout name the TIER (Money, the tier of Copper, Silver and Gold).  Pure logic: no sockets, no storage; the
//  session saves it with the inventory (CharacterApi::save_inventory) and shows it through OwnerInfo.Currencies.
//
//    cap      money cannot pass 1,000 gold (10,000,000 copper, the Money tier's largest quantity): what a grant would add past it is not added
//    spend    refused, changing nothing, when the purse holds less than the price
//    order    entries() lists Copper, then Node currency (0 when the character holds none: the client's own list has both), then any other currency in Guid order
// ============================================================================
#pragma once

#include "svc/services.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <vector>

namespace aoc::worldd::items {

/// The design database's money: the base def of copper, the tier a price and a callout name, and the second currency the client's purse always lists.
inline constexpr int64_t kCopperCurrency = 6064629132706054144ll;
inline constexpr int64_t kMoneyTier = 6064629127873626112ll;
inline constexpr int64_t kNodeCurrency = 6064629879350493185ll;
/// The most copper a character can hold (1,000 gold).
inline constexpr int64_t kMoneyCap = 10'000'000;
/// Glint is held as items, not in the purse: the tier a price and a callout name, the item that is one unit of it (Glint T1), and the most a price may come to.
inline constexpr int64_t kGlintTier = 6064629147812364289ll;
inline constexpr int64_t kGlintT1Item = 6064628753953390592ll;
inline constexpr int64_t kGlintCap = 3'368'420;

class Wallet {
public:
    Wallet() = default;
    /// The balances the character service sent.  An entry of a currency below 1 or a negative amount is ignored; a copper balance above the cap is kept as it is (the cap limits
    /// what is added, never takes money away).
    explicit Wallet(const std::vector<svc::WalletEntry>& saved) {
        for (const svc::WalletEntry& e : saved) {
            if (e.currency_guid > 0 && e.amount >= 0) amounts_[e.currency_guid] = e.amount;
        }
    }

    int64_t balance(int64_t currency) const {
        const auto it = amounts_.find(currency);
        return it == amounts_.end() ? 0 : it->second;
    }
    int64_t copper() const { return balance(kCopperCurrency); }
    /// Whether the purse holds `copper` (a negative amount is never payable).
    bool can_pay(int64_t copper_amount) const { return copper_amount >= 0 && copper_amount <= copper(); }
    /// Takes `copper_amount` out; false, changing nothing, when the purse holds less or the amount is negative.
    bool pay(int64_t copper_amount) {
        if (!can_pay(copper_amount)) return false;
        amounts_[kCopperCurrency] = copper() - copper_amount;
        return true;
    }
    /// How much of `copper_amount` fits below the cap.
    int64_t room() const { return std::max<int64_t>(0, kMoneyCap - copper()); }
    /// Adds up to `copper_amount` (never past the cap); returns what was added.
    int64_t give(int64_t copper_amount) {
        if (copper_amount <= 0) return 0;
        const int64_t added = std::min(copper_amount, room());
        if (added > 0) amounts_[kCopperCurrency] = copper() + added;
        return added;
    }

    /// Every balance in the order the client's list and the save use (see the top of the file).
    std::vector<svc::WalletEntry> entries() const {
        std::vector<svc::WalletEntry> out;
        out.push_back({kCopperCurrency, copper()});
        out.push_back({kNodeCurrency, balance(kNodeCurrency)});
        for (const auto& [currency, amount] : amounts_) {
            if (currency != kCopperCurrency && currency != kNodeCurrency) out.push_back({currency, amount});
        }
        return out;
    }

private:
    std::map<int64_t, int64_t> amounts_;
};

}  // namespace aoc::worldd::items
