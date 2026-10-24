// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  vendor/town_shops.h  (the vendors milestone, step 0: the general merchant)
//
//  The shops of the towns' people, over content.db's vendor tables (contentc version 18): which NPC opens which shop, what each listing costs, and what a cart comes to.  Pure
//  logic: no sockets, no session.
//
//  The price of a cart line, which the client computes the same way for the window it shows (so the number on the button and the one taken from the purse agree):
//
//    unit  = P + ceil(P * percent + flat)        P is the listing's price before tax (vendor_price: per unit, in copper), percent and flat the node's tax row
//    line  = quantity * unit
//
//  The row is the shop's tax tag looked up in the node's tax table; a row counts only when both of its flags are set, so the lookup climbs the tag tree ("NodeTax.GoodsTax.
//  MarketplaceTax", then "NodeTax.GoodsTax", then "NodeTax") until it finds one.  The default table (a node nobody has changed) has only its root row set, at 50 percent, so every
//  shop taxes at the root's rate; a shop with no tax tag is not taxed.  The arithmetic is in single precision and the tax rounds up, as the client's does.
//
//  What a cart must be to be sold (V2): the cart names the open shop; every line names an item, a variation and an offer that one listing of the shop carries (a listing whose
//  tab or own predicates do not hold for the player is not sold); every price is in money; the whole cart costs no more than a purse can hold.  Anything else is refused whole.
//
//  Only the shops in kOpenShops are switched on: the Barracks, the citizen outfitter and the mayor's shop keep answering "nothing to offer yet" until their systems exist.
//
//  The shops of shopkeepers who talk (the Black Market, the caravan salesman, the sheriff's tax vouchers) have no use link: a line of their conversation opens them.  A window can
//  hide a whole tab, or hide or grey out a listing, for a player the tab's or the listing's predicates do not hold for (unavailable()); a price in Glint is paid with Glint T1 items.
// ============================================================================
#pragma once

#include "content/content_db.h"
#include "dialogue/expression.h"
#include "items/wallet.h"
#include "protocol/repl/value.h"
#include "protocol/repl/vendor_rpc.h"
#include "vendor/node_tax_table.h"
#include "world/city_nodes.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aoc::worldd::vendor {

/// The shops this step switches on, by their def's name.
inline constexpr const char* kOpenShops[] = {"Marketplace_Vendor", "Node_Vendor_BlackMarket", "Caravansary_VendorDef", "ServiceBuilding_TownHallAdmin_Vendor"};

/// How far the shopkeeper may be from the player, in cm, in three dimensions, for the shop to open and for a purchase to go through (12 m).
inline constexpr double kShopRangeCm = 1200.0;

/// The most lines a cart may carry and the most of one item a line asks for.
inline constexpr size_t  kMaxShopCartLines = 70;
inline constexpr int32_t kMaxShopQuantity = 100000;

/// The node tax a shop applies: percent of the price plus a flat amount, from the tax row the shop's tag finds.
struct TaxRate {
    float   percent = 0.0f;
    int32_t flat = 0;
    bool    found = false;
};

/// The rate a tag has in the default table: the first row, from the tag up the tag tree, with both flags set.
inline TaxRate default_tax_rate(std::string_view tag) {
    std::string t(tag);
    while (!t.empty()) {
        for (const tax_table::Row& r : tax_table::kRows) {
            if (t == r.tag && r.flag_a && r.flag_b) return TaxRate{r.tax_percent, r.flat_tax, true};
        }
        const size_t dot = t.rfind('.');
        if (dot == std::string::npos) break;
        t.resize(dot);
    }
    return TaxRate{};
}

/// A price plus the tax on it (single precision, rounded up: the client's rule).
inline int64_t taxed_unit(int64_t price, const TaxRate& rate) {
    if (!rate.found) return price;
    const float tax = static_cast<float>(price) * rate.percent + static_cast<float>(rate.flat);
    if (!(tax > 0.0f)) return price;
    return price + static_cast<int64_t>(std::ceil(tax));
}

/// What a shop's predicates are asked against: the node the player stands in and, for a shop a shopkeeper opens, the node the shopkeeper stands in.  GetPlayer, GetInteractionTarget,
/// GetCurrentNode, Node_GetActorCityNode and Node_InLevelRangeInclusive are the functions the shops' own predicates use; any other function ends the evaluation with no value, and the
/// predicate does not hold.
class ShopFacts final : public dialogue::ExprHost {
public:
    const world::CityNodes* nodes = nullptr;
    double                  player_x = 0, player_y = 0;
    bool                    has_npc = false;
    double                  npc_x = 0, npc_y = 0;

    std::optional<dialogue::ExprValue> call(const std::string& name, const std::vector<dialogue::ExprValue>& args) const override {
        using dialogue::ExprValue;
        constexpr int64_t kPlayer = 1, kNpc = 2;   // the objects an expression passes around
        if (name == "GetPlayer") return ExprValue::of(kPlayer);
        if (name == "GetInteractionTarget") return ExprValue::of(has_npc ? kNpc : 0);
        if (name == "Node_GetActorCityNode") {
            if (args.size() != 1 || !nodes) return std::nullopt;
            const bool of_npc = args[0].i == kNpc && has_npc;
            const world::NodeSite site = nodes->node_at(of_npc ? npc_x : player_x, of_npc ? npc_y : player_y);
            const world::CityNodeInfo* n = nodes->by_rank(site.node_rank);
            return ExprValue::of(n ? n->guid : 0);
        }
        if (name == "GetCurrentNode") {
            if (args.size() != 1 || !nodes) return std::nullopt;
            const world::NodeSite site = nodes->node_at(player_x, player_y);
            const world::CityNodeInfo* n = nodes->by_rank(site.node_rank);
            return ExprValue::of(n ? n->guid : 0);
        }
        if (name == "Node_InLevelRangeInclusive") {
            if (args.size() != 3 || !nodes) return std::nullopt;
            const int lo = level_named(args[1].text), hi = level_named(args[2].text);
            if (lo < 0 || hi < 0) return std::nullopt;
            const int level = args[0].i == 0 ? 0 : nodes->level_of(args[0].i);
            return ExprValue::of(level >= lo && level <= hi ? 1 : 0);
        }
        return std::nullopt;
    }
    int64_t random_range(int64_t lo, int64_t) const override { return lo; }

    /// The ENodeLevel ordinal of "ENodeLevel::Encampment" (or "Encampment"); -1 for a name that is none.
    static int level_named(const std::string& text) {
        static constexpr const char* kLevels[] = {"Wilderness", "Crossroads", "Encampment", "Village", "Town", "City", "Metropolis"};
        const size_t colons = text.rfind("::");
        const std::string name = colons == std::string::npos ? text : text.substr(colons + 2);
        for (int i = 0; i < 7; ++i) {
            if (name == kLevels[i]) return i;
        }
        return -1;
    }
};

class TownShops {
public:
    TownShops() = default;
    explicit TownShops(content::VendorData data) : d_(std::move(data)) {
        for (const auto& [guid, def] : d_.defs) {
            for (const char* open : kOpenShops) {
                if (def.name == open) open_.push_back(guid);
            }
        }
    }

    bool   loaded() const { return !d_.defs.empty(); }
    size_t defs() const { return d_.defs.size(); }
    size_t links() const { return d_.instance_links.size() + d_.asset_links.size(); }
    size_t priced_rows() const {
        size_t n = 0;
        for (const auto& [shop, items] : d_.prices) {
            for (const auto& [item, rows] : items) n += rows.size();
        }
        return n;
    }
    const std::vector<int64_t>& open_shops() const { return open_; }
    /// The data the shops are made of (the modifiers and filters of a sale or a repair are read from it: vendor/sell_rules.h).
    const content::VendorData& data() const { return d_; }

    const content::VendorData::Def* def(int64_t shop) const {
        const auto it = d_.defs.find(shop);
        return it == d_.defs.end() ? nullptr : &it->second;
    }
    bool is_open(int64_t shop) const {
        for (const int64_t s : open_) {
            if (s == shop) return true;
        }
        return false;
    }

    /// The shop an NPC's use opens: its instance's link wins over its asset's.  0 when it opens none, or the shop is one this step has not switched on.
    int64_t shop_of(int64_t instance, int64_t asset) const {
        int64_t shop = 0;
        if (const auto it = d_.instance_links.find(instance); it != d_.instance_links.end()) {
            shop = it->second;
        } else if (const auto at = d_.asset_links.find(asset); at != d_.asset_links.end()) {
            shop = at->second;
        }
        return is_open(shop) ? shop : 0;
    }

    /// The tax rate of a shop at a node whose table is the default one.
    TaxRate default_rate(int64_t shop) const {
        const content::VendorData::Def* d = def(shop);
        return d && !d->tax_tag.empty() ? default_tax_rate(d->tax_tag) : TaxRate{};
    }

    struct Line {
        int64_t item = 0;
        int32_t quantity = 0;
        int64_t unit = 0;   // the price of one, tax included
    };
    struct Quote {
        bool              ok = false;
        std::string       refusal;
        std::vector<Line> lines;
        int64_t           total = 0;                    // in `currency`'s units: copper, or Glint T1 for the Glint tier
        int64_t           currency = items::kMoneyTier; // the tier the whole cart is paid in
    };
    /// Whether a predicate holds for the player now.
    using Holds = std::function<bool(const content::VendorPredicateRow&)>;

    /// What `cart` comes to at `shop` with the tax `rate`.  Pure: nothing is changed.
    Quote quote(int64_t shop, const protocol::repl::VendorPurchaseCart& cart, const TaxRate& rate, const Holds& holds) const {
        Quote q;
        const auto refuse = [&](std::string why) {
            q.ok = false;
            q.refusal = std::move(why);
            q.lines.clear();
            q.total = 0;
            return q;
        };
        const content::VendorData::Def* d = def(shop);
        if (!d) return refuse("the shop is not one worldd knows");
        if (d->blacklisted) return refuse("the shop is blacklisted");
        if ((d->operations & 1) == 0) return refuse("the shop does not sell");
        if (cart.vendor != shop) return refuse("the cart names vendor " + std::to_string(cart.vendor) + ", not the one the player has open (" + d->name + ")");
        if (cart.items.empty()) return refuse("the cart is empty");
        if (cart.items.size() > kMaxShopCartLines) return refuse("the cart holds " + std::to_string(cart.items.size()) + " lines (at most " + std::to_string(kMaxShopCartLines) + ")");
        const auto prices = d_.prices.find(shop);
        for (const protocol::repl::VendorCartItem& line : cart.items) {
            if (line.quantity < 1 || line.quantity > kMaxShopQuantity) return refuse("a line of the cart asks for a quantity of " + std::to_string(line.quantity));
            if (prices == d_.prices.end()) return refuse(d->name + " has no priced listing");
            const auto item = prices->second.find(line.item_record);
            if (item == prices->second.end()) return refuse("item " + std::to_string(line.item_record) + " is not one of " + d->name + "'s listings");
            const int64_t variation = line.optional_a.value_or(0), offer = line.optional_b.value_or(0);
            // the rows of the listings that carry this variation and offer, by listing (tab, index)
            std::map<std::pair<int64_t, int>, std::vector<const content::VendorPriceRow*>> listings;
            for (const content::VendorPriceRow& r : item->second) {
                if (r.variation == variation && r.offer == offer) listings[{r.inventory, r.ord}].push_back(&r);
            }
            if (listings.empty()) {
                return refuse("item " + std::to_string(line.item_record) + " is not listed by " + d->name + " with variation " + std::to_string(variation) + " and offer " + std::to_string(offer));
            }
            const std::vector<const content::VendorPriceRow*>* chosen = nullptr;
            std::string why;
            for (const auto& [key, rows] : listings) {
                bool all = true;
                for (const std::pair<int64_t, int> where : {std::pair<int64_t, int>{key.first, -1}, key}) {
                    const auto p = d_.predicates.find(where);
                    if (p == d_.predicates.end()) continue;
                    for (const content::VendorPredicateRow& pred : p->second) {
                        if (!holds(pred)) {
                            all = false;
                            why = pred.failure.empty() ? pred.expression : pred.failure;
                        }
                    }
                }
                if (all) {
                    chosen = &rows;
                    break;
                }
            }
            if (!chosen) return refuse("item " + std::to_string(line.item_record) + " is not for sale to this player here: " + why);
            int64_t unit = 0, currency = 0;
            for (const content::VendorPriceRow* r : *chosen) {
                if (currency != 0 && r->currency != currency) return refuse("item " + std::to_string(line.item_record) + " is paid in more than one currency");
                currency = r->currency;
                unit += taxed_unit(r->price, rate);
            }
            // money comes from the purse, Glint from the Glint T1 items of the bags; every other currency waits for its system
            if (currency != items::kMoneyTier && currency != items::kGlintTier) return refuse("item " + std::to_string(line.item_record) + " is paid in a currency this shop cannot take yet");
            if (q.lines.empty()) q.currency = currency;
            else if (q.currency != currency) return refuse("a cart is paid in one currency, and this one mixes two");
            const int64_t cap = currency == items::kGlintTier ? items::kGlintCap : items::kMoneyCap;
            if (unit > cap) return refuse("item " + std::to_string(line.item_record) + " costs more than can be held");
            q.total += unit * line.quantity;
            if (q.total > cap) return refuse("the cart costs more than can be held");
            q.lines.push_back({line.item_record, line.quantity, unit});
        }
        q.ok = true;
        return q;
    }

    /// What the window of `shop` hides or greys out for the player `holds` is asked for: one entry for each tab (counted in the def's order) with a predicate that fails, naming the tab
    /// (hidden when a failed predicate of its own is a hiding one) and, unless the whole tab is hidden, its listings that fail.  A failed predicate that is not a hiding one and has a
    /// reason to show is named by its position.
    protocol::repl::UnavailableList unavailable(int64_t shop, const Holds& holds) const {
        using protocol::repl::UnavailableEntry;
        using protocol::repl::UnavailableTab;
        protocol::repl::UnavailableList out;
        const content::VendorData::Def* d = def(shop);
        if (!d) return out;
        // true (and the entry's hidden/failures set) when a predicate of the list fails
        const auto evaluate = [&](const std::vector<content::VendorPredicateRow>& preds, UnavailableEntry& e) {
            bool failed = false;
            for (size_t k = 0; k < preds.size() && k <= 0xFF; ++k) {
                if (holds(preds[k])) continue;
                failed = true;
                if (preds[k].hidden) e.hidden = true;
                else if (!preds[k].failure.empty()) e.failures.push_back(static_cast<uint8_t>(k));
            }
            return failed;
        };
        for (size_t t = 0; t < d->inventories.size(); ++t) {
            const int64_t tab = d->inventories[t];
            UnavailableTab entry;
            entry.inventory.index = static_cast<int32_t>(t);
            bool any = false;
            if (const auto own = d_.predicates.find({tab, -1}); own != d_.predicates.end()) any = evaluate(own->second, entry.inventory);
            if (!entry.inventory.hidden) {
                for (auto it = d_.predicates.lower_bound({tab, 0}); it != d_.predicates.end() && it->first.first == tab; ++it) {
                    UnavailableEntry listing;
                    listing.index = it->first.second;
                    if (evaluate(it->second, listing)) entry.listings.push_back(std::move(listing));
                }
            }
            if (any || !entry.listings.empty()) out.push_back(std::move(entry));
        }
        return out;
    }

private:
    content::VendorData  d_;
    std::vector<int64_t> open_;
};

}  // namespace aoc::worldd::vendor
