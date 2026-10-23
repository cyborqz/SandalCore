// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  vendor/sell_rules.h  (the vendors milestone, step 1: selling and repairing)
//
//  What a shopkeeper pays for an item and asks for mending it, over content.db's vendor tables (contentc version 19).  Pure logic: no sockets, no session.
//
//  Whether a shop buys an item (the client asks the same before it lets a player offer one):
//    * the shop is not blacklisted and does sell operations (bit 2 of its operations);
//    * the item is Vendorable and not Blacklisted;
//    * the shop's sell filters: with none, the item may be sold; otherwise a filter whose whitelist holds the item lets it through at once, and failing that EVERY filter's
//      tag query must match the item's tags (a filter with no query never matches).
//
//  What the shop pays, per unit, in one currency (the same chain as a purchase, with the sell bit):
//
//    P0 = the item's base value for a sale at its rarity (item_sell_value: the value expression, rounded up, made by contentc)
//    P  = the shop's price modifiers in order, each one that applies to selling, to the item's tags and to the currency
//    P1 = ceil(max(P, 0))              the stack pays quantity * P1; the node tax is not added to a sale
//
//  The rarity of an instance is the band its quality falls in on the item's quality curve, held inside the item's own range.  A modifier may be a plain number, or a curve read at
//  the instance's quality, rarity, durability share, stolen flag or the character's level; a modifier that needs more than that (a grade, a slot, an expression with a function)
//  makes the sale refuse and says so.
//
//  What a repair asks: the same chain with the repair bit, read at the share of durability the item has left, plus the node tax, rounded up, as a purchase's.
// ============================================================================
#pragma once

#include "content/content_db.h"
#include "vendor/town_shops.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

namespace aoc::worldd::vendor {

/// The operations of a shop and of a price modifier's contexts (bit masks).
inline constexpr int64_t kBuyBit = 1;
inline constexpr int64_t kSellBit = 2;
inline constexpr int64_t kRepairBit = 4;

namespace detail {

/// Evaluates a tag query's token stream: version, a non-empty flag, then the tree -- an expression kind and a count; kinds 1 to 3 hold any, all or none of the tags that follow
/// (indexes into the dictionary), kinds 4 to 6 any, all or none of the sub-expressions that follow.
struct TagEval {
    const content::TagQuery&     q;
    const std::set<std::string>& have;
    size_t                       pos = 2;

    bool has(int64_t i) const { return i >= 0 && static_cast<size_t>(i) < q.tags.size() && have.count(q.tags[static_cast<size_t>(i)]) != 0; }

    bool expr(bool& good) {
        if (pos + 1 >= q.tok.size()) {
            good = false;
            return false;
        }
        const int64_t kind = q.tok[pos], n = q.tok[pos + 1];
        pos += 2;
        if (kind >= 1 && kind <= 3) {
            if (n < 0 || pos + static_cast<size_t>(n) > q.tok.size()) {
                good = false;
                return false;
            }
            bool any = false, all = true;
            for (int64_t i = 0; i < n; ++i) {
                if (has(q.tok[pos + static_cast<size_t>(i)])) any = true;
                else all = false;
            }
            pos += static_cast<size_t>(n);
            return kind == 1 ? any : kind == 2 ? all : !any;
        }
        if (kind >= 4 && kind <= 6) {
            bool any = false, all = true;
            for (int64_t i = 0; i < n; ++i) {
                const bool r = expr(good);
                if (!good) return false;
                if (r) any = true;
                else all = false;
            }
            return kind == 4 ? any : kind == 5 ? all : !any;
        }
        good = false;
        return false;
    }
};

/// A text that is one plain decimal number.
inline bool literal_number(const std::string& text, double& out) {
    const char* begin = text.c_str();
    char*       end = nullptr;
    out = std::strtod(begin, &end);
    if (end == begin) return false;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') ++end;
    return *end == '\0' && std::isfinite(out);
}

/// The value of a curve at `x`: the first key's value before the curve, the last key's after it, and between two keys by the earlier key's interpolation (Constant or Linear).
/// A curve is read in single precision, as the client reads it (a worn share of 100 / 150 gives 0.06666666, not 0.06666667, and the price rounds up from there).
inline bool curve_at(const std::vector<content::VendorModifier::Key>& keys, double x, double& out, std::string& why) {
    if (keys.empty()) {
        why = "the curve has no keys";
        return false;
    }
    const float fx = static_cast<float>(x);
    if (fx <= static_cast<float>(keys.front().time)) {
        out = static_cast<double>(static_cast<float>(keys.front().value));
        return true;
    }
    if (fx >= static_cast<float>(keys.back().time)) {
        out = static_cast<double>(static_cast<float>(keys.back().value));
        return true;
    }
    size_t i = 0;
    while (i + 1 < keys.size() && static_cast<float>(keys[i + 1].time) <= fx) ++i;
    const content::VendorModifier::Key& a = keys[i];
    const content::VendorModifier::Key& b = keys[i + 1];
    if (a.interp == "Constant") {
        out = static_cast<double>(static_cast<float>(a.value));
        return true;
    }
    if (a.interp == "Linear") {
        const float ta = static_cast<float>(a.time), tb = static_cast<float>(b.time), va = static_cast<float>(a.value), vb = static_cast<float>(b.value);
        const float span = tb - ta;
        const float alpha = span > 0.0f ? (fx - ta) / span : 1.0f;
        out = static_cast<double>(va + alpha * (vb - va));
        return true;
    }
    why = "a curve key with the interpolation " + a.interp + " is not read";
    return false;
}

}  // namespace detail

/// Whether a tag query holds for a set of lower-case tags (each tag with its parents).  A query that is not there, or whose stream does not read, matches nothing.
inline bool tag_query_matches(const content::TagQuery& q, const std::set<std::string>& have) {
    if (!q.present) return false;
    detail::TagEval e{q, have};
    bool            good = true;
    const bool      r = e.expr(good);
    return r && good;
}

/// What a price modifier may read of the item and the character.
struct PriceFacts {
    int                          rarity = 0;
    int64_t                      quality = 1;
    double                       durability_share = 1.0;   // the part of the item's durability left, 0 to 1
    bool                         stolen = false;
    int                          character_level = 1;
    const std::set<std::string>* tags = nullptr;
};

/// Runs a chain of price modifiers over `v` for one operation (`op`: a bit) and one currency tier, in order.  False, with `why` set, for a modifier this reader cannot run.
inline bool apply_chain(const content::VendorData& d, const std::vector<int64_t>& chain, int64_t op, int64_t currency, const PriceFacts& f, double& v, std::string& why, int depth = 0) {
    if (depth > 8) {
        why = "the price modifiers nest too deep";
        return false;
    }
    for (const int64_t id : chain) {
        const auto it = d.modifiers.find(id);
        if (it == d.modifiers.end()) {
            why = "price modifier " + std::to_string(id) + " is not in the data";
            return false;
        }
        const content::VendorModifier& m = it->second;
        if ((m.contexts & op) != op) continue;
        if (m.filter.present && !(f.tags && tag_query_matches(m.filter, *f.tags))) continue;
        if (m.source == "PriceModifierSet") {
            if (!apply_chain(d, m.children, op, currency, f, v, why, depth + 1)) return false;
            continue;
        }
        if (std::find(m.currencies.begin(), m.currencies.end(), currency) == m.currencies.end()) continue;
        double x = 0;
        if (m.source == "Expression") {
            if (!detail::literal_number(m.expression, x)) {
                why = "price modifier " + m.name + " has an expression that is not a plain number";
                return false;
            }
        } else if (m.source == "Curve") {
            double at = 0;
            if (m.factor == "Quality") at = static_cast<double>(f.quality);
            else if (m.factor == "Rarity") at = static_cast<double>(f.rarity);
            else if (m.factor == "DurabilityPercentage") at = f.durability_share;
            else if (m.factor == "Stolen") at = f.stolen ? 1.0 : 0.0;
            else if (m.factor == "CharacterLevel") at = static_cast<double>(f.character_level);
            else {
                why = "price modifier " + m.name + " reads a " + m.factor + " this reader does not know";
                return false;
            }
            if (!detail::curve_at(m.curve, at, x, why)) {
                why = "price modifier " + m.name + ": " + why;
                return false;
            }
        } else {
            why = "price modifier " + m.name + " has the source " + m.source;
            return false;
        }
        if (m.operation == "Add") v += x;
        else if (m.operation == "Multiply") v *= x;
        else if (m.operation == "Set") v = x;
        else {
            why = "price modifier " + m.name + " has the operation " + m.operation;
            return false;
        }
    }
    return true;
}

/// Whether the shop `def` may buy the item `item` (the facts of content.db), by the rules at the top.  `why` says what stopped it.
inline bool is_sellable(const content::VendorData& d, const content::VendorData::Def& def, int64_t item, const content::ItemSaleFacts& facts, std::string& why) {
    if (def.blacklisted) {
        why = "the shop is blacklisted";
        return false;
    }
    if ((def.operations & kSellBit) == 0) {
        why = "the shop does not buy";
        return false;
    }
    if (!facts.known) {
        why = "the item is not in the data";
        return false;
    }
    if (!facts.vendorable) {
        why = "the item is not for sale";
        return false;
    }
    if (facts.blacklisted) {
        why = "the item is blacklisted";
        return false;
    }
    bool all = true;
    for (const int64_t id : def.sell_filters) {
        const auto it = d.sell_filters.find(id);
        if (it == d.sell_filters.end()) continue;   // a filter the data does not have changes nothing
        if (it->second.whitelist.count(item) != 0) return true;
        if (!tag_query_matches(it->second.filter, facts.tags)) all = false;
    }
    if (!all) why = "the shop does not buy that kind of item";
    return all;
}

/// What a sale of one stack comes to.
struct SaleQuote {
    bool        ok = false;
    std::string refusal;
    int64_t     unit = 0;    // per item, in the currency tier
    int64_t     total = 0;   // quantity * unit
};

/// The sale of `quantity` of an item of `quality` to the shop `vendor`, in the currency tier `tier` (the money tier for copper).
inline SaleQuote quote_sale(const content::VendorData& d, int64_t vendor, int64_t item, int64_t quantity, int64_t quality, const content::ItemSaleFacts& facts, int64_t tier,
                            int character_level) {
    SaleQuote q;
    const auto refuse = [&](std::string why) {
        q.ok = false;
        q.refusal = std::move(why);
        q.unit = q.total = 0;
        return q;
    };
    const auto def = d.defs.find(vendor);
    if (def == d.defs.end()) return refuse("the shop " + std::to_string(vendor) + " is not in the data");
    std::string why;
    if (!is_sellable(d, def->second, item, facts, why)) return refuse(why);
    if (quantity < 1) return refuse("a stack of " + std::to_string(quantity));
    const int       rarity = facts.rarity_of(quality);
    const auto      base = facts.values.find({rarity, tier});
    if (base == facts.values.end()) return refuse("the item has no value in that currency at its rarity");
    PriceFacts f;
    f.rarity = rarity;
    f.quality = quality;
    f.character_level = character_level;
    f.tags = &facts.tags;
    double v = static_cast<double>(base->second);
    if (!apply_chain(d, def->second.modifiers, kSellBit, tier, f, v, why)) return refuse(why);
    const double rounded = std::ceil(std::max(v, 0.0));
    if (!(rounded < 1e15)) return refuse("the price is out of range");
    q.unit = static_cast<int64_t>(rounded);
    if (q.unit > 0 && quantity > INT64_MAX / q.unit) return refuse("the stack is worth more than a purse can hold");
    q.total = q.unit * quantity;
    q.ok = true;
    return q;
}

/// What the repair of one item comes to.
struct RepairQuote {
    bool        ok = false;
    std::string refusal;
    int64_t     before_tax = 0;
    int64_t     total = 0;   // before_tax and the node tax
};

/// The repair of an item of `quality` whose durability is `durability` of at most `max_durability`, at the shop `vendor`, in the currency tier `tier`, with the node tax `tax`.
inline RepairQuote quote_repair(const content::VendorData& d, int64_t vendor, int64_t item, int64_t quality, double durability, double max_durability, const content::ItemSaleFacts& facts,
                                int64_t tier, const TaxRate& tax, int character_level) {
    RepairQuote q;
    const auto refuse = [&](std::string why) {
        q.ok = false;
        q.refusal = std::move(why);
        q.before_tax = q.total = 0;
        return q;
    };
    const auto def = d.defs.find(vendor);
    if (def == d.defs.end()) return refuse("the shop " + std::to_string(vendor) + " is not in the data");
    if (def->second.blacklisted) return refuse("the shop is blacklisted");
    if ((def->second.operations & kRepairBit) == 0) return refuse("the shop does not repair");
    if (!facts.known) return refuse("the item is not in the data");
    if (facts.blacklisted) return refuse("the item is blacklisted");
    if (!(max_durability > 0) || !(durability < max_durability)) return refuse("the item is not worn");
    const int  rarity = facts.rarity_of(quality);
    const auto base = facts.values.find({rarity, tier});
    if (base == facts.values.end()) return refuse("the item has no value in that currency at its rarity");
    PriceFacts f;
    f.rarity = rarity;
    f.quality = quality;
    f.durability_share = std::clamp(durability / max_durability, 0.0, 1.0);
    f.character_level = character_level;
    f.tags = &facts.tags;
    double      v = static_cast<double>(base->second);
    std::string why;
    if (!apply_chain(d, def->second.modifiers, kRepairBit, tier, f, v, why)) return refuse(why);
    const double rounded = std::ceil(std::max(v, 0.0));
    if (!(rounded < 1e15)) return refuse("the price is out of range");
    q.before_tax = static_cast<int64_t>(rounded);
    q.total = taxed_unit(q.before_tax, tax);
    q.ok = true;
    return q;
}

}  // namespace aoc::worldd::vendor
