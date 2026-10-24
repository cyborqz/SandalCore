// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  crafting/rules.h  (crafting, step 1: "start a processing job"; header only)
//
//  What a start at a processing station asks of the player, the station and the recipe, in the order the checks run.  A start that fails a check is refused whole: nothing is taken and
//  the client is told nothing (the window stays as it was); the reason goes to the log.
//
//  The order.  The recipe exists and may be made at a station (not blacklisted, its products go to the bags); it is in the station's recipe set; the player knows it (a recipe the game makes
//  the player learn is refused until learning exists); the player's certification in the recipe's profession reaches its tier and the station's tier reaches it; the node the station is in
//  allows it (the recipe's and its list's availability predicates: the node's level and biome, the nodes it is for, and the service building it needs, which no node has yet); the quantity is
//  1 to 250; the player's queue at the station has room; the stacks the player named add up to the recipe's inputs for that quantity (the same records, rarity bands at or above the recipe's
//  minimum, no warehouse), the bags hold them, and hold the reagent too; the purse holds the price.
//
//  The price.  The recipe's cost is a number of copper for one unit (its own, or its cost record's); the station's service tax makes it `round(cost x (1 + percent) + flat)` for one unit,
//  rounded half up, in double precision on the percent the window is sent (a float, 0.1 is a little under a tenth), and the job costs that for each unit.  The retail starts fit it: 138
//  Granite crushed at 5 each with a tenth of tax cost 690, not 759 (the rounding is for one unit); a log at 23 with half the price in tax costs 35.
//
//  The time.  A job takes the recipe's base duration for each unit: the time equation of the recipes is 1 for a character with no artisan gear, and no character has any yet.
// ============================================================================
#pragma once

#include "content/content_db.h"
#include "crafting/job.h"
#include "items/inventory.h"
#include "items/wallet.h"
#include "protocol/repl/craft_rpc.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace aoc::worldd::crafting {

/// The most units one job makes (the station gives 1 to 250).
inline constexpr int kMaxQuantity = 250;

/// The first quality of each rarity band (Common 0 ... Artifact 6), and the last quality there is.
inline constexpr int kBandStart[7] = {1, 6, 16, 31, 51, 71, 101};
inline constexpr int kMaxQuality = 200;
inline int band_lo(int rarity) { return kBandStart[std::clamp(rarity, 0, 6)]; }
inline int band_hi(int rarity) { return rarity >= 6 ? kMaxQuality : kBandStart[std::clamp(rarity, 0, 5) + 1] - 1; }

/// One take from the material bags: `count` of `record` of a quality band.
struct Take {
    int64_t record = 0;
    int     quality_lo = 1, quality_hi = kMaxQuality;
    int64_t count = 0;
    bool    lowest_first = false;   // the reagent: the lowest quality that qualifies goes first
};

/// The node a station stands in, as the availability predicates see it.
struct NodeFacts {
    int64_t     guid = 0;
    int         level = -1;   // Wilderness 0 ... Metropolis 6; -1 when unknown
    std::string biome;
};

struct StartFacts {
    const content::CraftRecipeData* recipes = nullptr;
    const content::CraftStationDef* station = nullptr;
    protocol::repl::StationTax      tax;
    NodeFacts                       node;
    std::function<int(int64_t)>     tier_of;       // the player's certification tier in a profession (Novice 0 ... Grand Master 4)
    const items::Inventory*         inventory = nullptr;
    int64_t                         copper = 0;
    size_t                          unfinished_here = 0;   // the player's jobs at this station that are not over
    size_t                          held_here = 0;         // ... and all of them, finished ones too
};

struct Plan {
    bool                                  ok = false;
    std::string                           refusal;
    const content::CraftRecipe*           recipe = nullptr;
    std::vector<Take>                     takes;         // in the order they are taken: the reagent first, then what the player named
    std::vector<protocol::repl::CraftResource> paid;     // what the player named, merged: what the job is saved and shown with
    int64_t                               unit_cost = 0;   // before the tax
    int64_t                               unit_taxed = 0;
    int64_t                               cost = 0;
    int64_t                               duration_ms = 0;
};

/// One unit's price with the station's tax: round half up of `unit x (1 + percent) + flat`, the percent as a double.
inline int64_t taxed_unit(int64_t unit, const protocol::repl::StationTax& tax) {
    return static_cast<int64_t>(std::floor(static_cast<double>(unit) * (1.0 + static_cast<double>(tax.percent)) + static_cast<double>(tax.flat) + 0.5));
}

/// The recipe's price for one unit in copper before tax; -1 when it is paid in another currency than money (not made yet).
inline int64_t recipe_unit_cost(const content::CraftRecipeData& data, const content::CraftRecipe& r) {
    if (!r.std_cost.empty()) {
        char* end = nullptr;
        const double v = std::strtod(r.std_cost.c_str(), &end);
        return end != r.std_cost.c_str() && v >= 0 ? static_cast<int64_t>(std::floor(v + 0.5)) : -1;
    }
    if (r.cost_guid == 0) return 0;
    const auto c = data.costs.find(r.cost_guid);
    if (c == data.costs.end() || c->second.tier != items::kMoneyTier) return -1;
    return c->second.value;
}

inline bool csv_has(const std::string& csv, const std::string& want) {
    size_t at = 0;
    while (at <= csv.size()) {
        size_t end = csv.find(',', at);
        if (end == std::string::npos) end = csv.size();
        std::string part = csv.substr(at, end - at);
        if (part.size() == want.size() && std::equal(part.begin(), part.end(), want.begin(), [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); })) return true;
        at = end + 1;
    }
    return false;
}

/// Whether an availability predicate holds at the node; `why` says which does not.
inline bool predicate_holds(const content::CraftPred& p, const NodeFacts& n, std::string& why) {
    if (p.type == "NodeLevel") {
        if (n.level >= p.node_level && p.node_level >= 0) return true;
        why = "it needs a node of level " + std::to_string(p.node_level) + " and the node is " + (n.level < 0 ? std::string("not known") : "level " + std::to_string(n.level));
        return false;
    }
    if (p.type == "Biome") {
        if (!n.biome.empty() && csv_has(p.biomes, n.biome)) return true;
        why = "it is made only in the biomes " + p.biomes + " and the node is in " + (n.biome.empty() ? std::string("none known") : n.biome);
        return false;
    }
    if (p.type == "Node") {
        if (n.guid != 0 && csv_has(p.nodes, std::to_string(n.guid))) return true;
        why = "it is made only at certain nodes, and this is not one";
        return false;
    }
    if (p.type == "BuildingModification") {
        why = "it needs a service building in the node, and no node has one yet";
        return false;
    }
    why = "it has the unknown condition " + p.type;
    return false;
}

inline bool predicates_hold(const std::vector<content::CraftPred>& preds, const NodeFacts& n, std::string& why) {
    for (const content::CraftPred& p : preds) {
        if (!predicate_holds(p, n, why)) return false;
    }
    return true;
}

/// Runs the checks for `req` at the station in `f`.  Pure: nothing is changed.
inline Plan plan_start(const protocol::repl::StartCraft& req, const StartFacts& f) {
    Plan plan;
    const auto refuse = [&](std::string why) {
        plan.ok = false;
        plan.refusal = std::move(why);
        return plan;
    };
    if (!f.recipes || !f.station || !f.inventory) return refuse("worldd has no recipes or no station definition");
    if (!f.station->processing) return refuse("the station is not a processing station");
    const content::CraftRecipe* r = f.recipes->recipe(req.recipe);
    if (!r) return refuse("recipe " + std::to_string(req.recipe) + " is none of the data's");
    plan.recipe = r;
    if (req.sub_recipe != 0) return refuse("a sub recipe (" + std::to_string(req.sub_recipe) + ") is named: none is made");
    if (r->blacklisted) return refuse(r->name + " is switched off in the data");
    if (!r->inventory_destination) return refuse(r->name + " makes gear that is worn at once: not made yet");
    std::vector<int64_t> lists;
    if (!f.recipes->set_holds(f.station->recipe_set, r->guid, &lists)) return refuse(r->name + " is not one of the recipes of " + f.station->name);
    if (r->learnable) return refuse(r->name + " has to be learned, and no recipe can be learned yet");
    if (r->tier < 0) return refuse(r->name + " needs no certification tier the data names");
    const int have = f.tier_of ? f.tier_of(r->profession) : 0;
    if (have < r->tier) return refuse(r->name + " needs certification tier " + std::to_string(r->tier) + " and the character has " + std::to_string(have));
    if (r->tier > f.station->max_tier) return refuse(r->name + " needs tier " + std::to_string(r->tier) + " and " + f.station->name + " reaches " + std::to_string(f.station->max_tier));
    std::string why;
    if (!predicates_hold(r->preds, f.node, why)) return refuse(r->name + ": " + why);
    {   // the lists that hold it: one of them has to allow it (a list with no predicate does)
        bool any = lists.empty();
        std::string last;
        for (const int64_t list : lists) {
            const auto p = f.recipes->list_preds.find(list);
            if (p == f.recipes->list_preds.end() || predicates_hold(p->second, f.node, last)) {
                any = true;
                break;
            }
        }
        if (!any) return refuse(r->name + ": " + last);
    }
    if (req.quantity < 1 || req.quantity > kMaxQuantity) return refuse("a quantity of " + std::to_string(req.quantity) + " (the station makes 1 to " + std::to_string(kMaxQuantity) + ")");
    if (f.unfinished_here >= static_cast<size_t>(std::max(1, f.station->queue_size))) {
        return refuse("the queue at " + f.station->name + " is full (" + std::to_string(f.unfinished_here) + " of " + std::to_string(f.station->queue_size) + ")");
    }
    if (f.held_here >= kMaxJobsAtStation) return refuse(std::to_string(f.held_here) + " jobs are held at " + f.station->name + " already (the finished ones stay until they are collected)");
    if (req.has_tags || req.has_fuel || !req.flexible.empty()) return refuse("the request names tag, flexible or fuel resources: a processing station takes only the general ones");

    // what the player named, by record and rarity (a record named twice is added up)
    std::map<int64_t, std::map<int, int64_t>> named;
    for (const protocol::repl::CraftResource& res : req.general) {
        for (const protocol::repl::CraftRarityCount& c : res.rarities) {
            if (c.from_warehouse) return refuse("the request takes from the warehouse: none can be used yet");
            if (c.count < 1) return refuse("the request names " + std::to_string(c.count) + " items");
            named[res.record][c.rarity] += c.count;
        }
    }
    const content::CraftRecipeData& data = *f.recipes;
    std::vector<Take> general_takes, reagent_takes;
    for (const content::CraftInput& in : r->inputs) {
        const int64_t need = static_cast<int64_t>(in.quantity) * req.quantity;
        if (need < 1) continue;
        if (in.primary) {
            Take t;
            t.record = in.item;
            t.quality_lo = band_lo(in.min_rarity);
            t.count = need;
            t.lowest_first = true;
            if (f.inventory->count_materials(t.record, t.quality_lo, t.quality_hi) < t.count) {
                return refuse("the material bags hold " + std::to_string(f.inventory->count_materials(t.record, t.quality_lo, t.quality_hi)) + " of the reagent " + std::to_string(t.record) + ", the job needs " +
                              std::to_string(t.count));
            }
            reagent_takes.push_back(t);
            continue;
        }
        if (in.item == 0) return refuse(r->name + " takes a resource named by a filter or by tags: not made yet");
        const auto it = named.find(in.item);
        if (it == named.end()) return refuse("the request does not name item " + std::to_string(in.item) + ", which " + r->name + " takes");
        int64_t total = 0;
        for (const auto& [rarity, count] : it->second) total += count;
        if (total != need) return refuse("the request names " + std::to_string(total) + " of item " + std::to_string(in.item) + ", " + r->name + " takes " + std::to_string(need) + " for " + std::to_string(req.quantity));
        for (const auto& [rarity, count] : it->second) {
            if (rarity < in.min_rarity) return refuse("the request names rarity " + std::to_string(rarity) + " of item " + std::to_string(in.item) + ", the recipe takes " + std::to_string(in.min_rarity) + " or better");
            Take t;
            t.record = in.item;
            t.quality_lo = band_lo(rarity);
            t.quality_hi = band_hi(rarity);
            t.count = count;
            if (f.inventory->count_materials(t.record, t.quality_lo, t.quality_hi) < t.count) {
                return refuse("the material bags hold " + std::to_string(f.inventory->count_materials(t.record, t.quality_lo, t.quality_hi)) + " of item " + std::to_string(t.record) + " of rarity " +
                              std::to_string(rarity) + ", the request names " + std::to_string(t.count));
            }
            general_takes.push_back(t);
        }
        named.erase(it);
    }
    if (!named.empty()) return refuse("the request names item " + std::to_string(named.begin()->first) + ", which " + r->name + " does not take");
    // (two takes of one record and band would each pass the count check alone; the bags are checked again when the stacks are taken)
    plan.takes = reagent_takes;
    plan.takes.insert(plan.takes.end(), general_takes.begin(), general_takes.end());
    // what the job is shown with: the records and the rarities in the order the player named them, a record or a rarity named twice added up
    for (const protocol::repl::CraftResource& res : req.general) {
        auto dst = std::find_if(plan.paid.begin(), plan.paid.end(), [&](const protocol::repl::CraftResource& p) { return p.record == res.record; });
        if (dst == plan.paid.end()) {
            plan.paid.push_back({res.record, {}});
            dst = plan.paid.end() - 1;
        }
        for (const protocol::repl::CraftRarityCount& c : res.rarities) {
            auto at = std::find_if(dst->rarities.begin(), dst->rarities.end(), [&](const protocol::repl::CraftRarityCount& x) { return x.rarity == c.rarity; });
            if (at == dst->rarities.end()) dst->rarities.push_back({c.rarity, c.count, false});
            else at->count += c.count;
        }
    }

    const int64_t unit = recipe_unit_cost(data, *r);
    if (unit < 0) return refuse(r->name + " is paid in a currency other than money: not made yet");
    plan.unit_cost = unit;
    plan.unit_taxed = taxed_unit(unit, f.tax);
    plan.cost = plan.unit_taxed * req.quantity;
    if (plan.cost > f.copper) return refuse("the purse holds " + std::to_string(f.copper) + " copper and the job costs " + std::to_string(plan.cost));
    plan.duration_ms = static_cast<int64_t>(std::llround(r->base_duration * 1000.0 * req.quantity));
    if (plan.duration_ms < 1) return refuse(r->name + " has no duration in the data");
    plan.ok = true;
    return plan;
}

}  // namespace aoc::worldd::crafting
