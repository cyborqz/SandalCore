// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  crafting/rules.h  (crafting, steps 1 and 3: "start a processing job", "craft"; header only)
//
//  What a start at a processing station (a job) or at a crafting station (the quick make, `quick`) asks of the player, the station and the recipe, in the order the checks run.  A start
//  that fails a check is refused whole: nothing is taken and the client is told nothing (the window stays as it was); the reason goes to the log.
//
//  The order.  The recipe exists and may be made at a station (not blacklisted, its products go to the bags); it is in the station's recipe set; the player knows it (a recipe the game makes
//  the player learn is known only once a game master taught it); the player's certification in the recipe's profession reaches its tier and the station's tier reaches it; the node the
//  station is in allows it (the recipe's and its list's availability predicates: the node's level and biome, the nodes it is for, and the service building it needs, which no node has yet);
//  the quantity is 1 to 250 (a quick make is one unit); the player's queue at the station has room (a quick make has none); the stacks the player named add up to the recipe's inputs for that
//  quantity (the same records, rarities at or above the recipe's minimum, no warehouse), the bags hold them, and hold the reagent too; the purse holds the price.
//
//  The rarity of a stack is the one the client shows for it (items/rarity.h): the band its quality falls in, held inside the item's own RarityMin..RarityMax.  The client groups the stacks it
//  offers by that rarity and names it in the request, so a general input is taken from the stacks whose rarity is the one named, and a reagent (which the request does not name) from the
//  stacks whose rarity is the recipe's minimum or better: a Bee's Wax or a Flux bought at quality 1 is Rare or Uncommon and meets the recipes that ask for it.
//
//  The price.  The recipe's cost is a number of copper for one unit (its own, or its cost record's); the station's service tax makes it `round(cost x (1 + percent) + flat)` for one unit,
//  rounded half up, in double precision on the percent the window is sent (a float, 0.1 is a little under a tenth), and the job costs that for each unit.  The retail starts fit it: 138
//  Granite crushed at 5 each with a tenth of tax cost 690, not 759 (the rounding is for one unit); a log at 23 with half the price in tax costs 35.
//
//  The time.  A job takes the recipe's base duration for each unit: the time equation of the recipes is 1 for a character with no artisan gear, and no character has any yet.  A quick make
//  takes the base duration, and takes the materials and the price when it ends, not when it starts.
// ============================================================================
#pragma once

#include "content/content_db.h"
#include "crafting/job.h"
#include "items/inventory.h"
#include "items/rarity.h"
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

/// The first quality of a rarity band (Common 0 ... Artifact 6), and the last quality there is: the rarity rule's (items/rarity.h).  A job counts a rarity it was paid with as that quality.
inline constexpr int kMaxQuality = items::kMaxQuality;
inline int band_lo(int rarity) { return items::first_quality_of(rarity); }

/// One take from the material bags: `count` of `record` whose rarity (by the record's rarity facts) is `rarity_lo` to `rarity_hi`.
struct Take {
    int64_t                record = 0;
    items::ItemRarityFacts facts = items::default_rarity_facts();   // the record's quality curve and RarityMin..RarityMax
    int                    rarity_lo = 0, rarity_hi = items::kRarityCount - 1;
    int64_t                count = 0;
    bool                   lowest_first = false;   // the reagent: the lowest quality that qualifies goes first
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
    bool                            quick = false;         // the station crafts at once (a crafting station): one unit, no queue, no job
    std::function<bool(int64_t)>    taught;                // whether the character was taught a recipe (one that has to be learned)
    std::function<items::ItemRarityFacts(int64_t)> rarity_facts;   // an item record's rarity rule (its quality curve and RarityMin..RarityMax); none: the common curve with no bound
};

/// Whether a station definition is one the quick make serves: a profession's crafting station.  The crate tables, the stations that make gathering tools and the event and dungeon
/// stations answer nothing.
inline bool serves_quick_make(const content::CraftStationDef& d) {
    const std::string& n = d.name;
    if (d.processing || n.rfind("CraftingStation_", 0) != 0) return false;
    for (const char* other : {"GatheringTools", "Commodity", "HarbingerEvent", "BefallenForge"}) {
        if (n.find(other) != std::string::npos) return false;
    }
    return true;
}

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
    if (f.quick) {
        if (f.station->processing) return refuse(f.station->name + " is a processing station: a start there is a job");
        if (!serves_quick_make(*f.station)) return refuse(f.station->name + " makes nothing yet (crate tables and the stations of gathering tools are not served)");
    } else if (!f.station->processing) {
        return refuse("the station is not a processing station");
    }
    const content::CraftRecipe* r = f.recipes->recipe(req.recipe);
    if (!r) return refuse("recipe " + std::to_string(req.recipe) + " is none of the data's");
    plan.recipe = r;
    if (req.sub_recipe != 0) return refuse("a sub recipe (" + std::to_string(req.sub_recipe) + ") is named: none is made");
    if (r->blacklisted) return refuse(r->name + " is switched off in the data");
    if (!r->inventory_destination) return refuse(r->name + " makes gear that is worn at once: not made yet");
    std::vector<int64_t> lists;
    if (!f.recipes->set_holds(f.station->recipe_set, r->guid, &lists)) return refuse(r->name + " is not one of the recipes of " + f.station->name);
    if (r->learnable && !(f.taught && f.taught(r->guid))) return refuse(r->name + " has to be learned, and the character was not taught it");
    if (r->fuel > 0) return refuse(r->name + " burns fuel: not made yet");
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
    if (f.quick) {
        if (req.quantity != 1) return refuse("a quantity of " + std::to_string(req.quantity) + " (a quick make is one unit)");
    } else {
        if (req.quantity < 1 || req.quantity > kMaxQuantity) return refuse("a quantity of " + std::to_string(req.quantity) + " (the station makes 1 to " + std::to_string(kMaxQuantity) + ")");
        if (f.unfinished_here >= static_cast<size_t>(std::max(1, f.station->queue_size))) {
            return refuse("the queue at " + f.station->name + " is full (" + std::to_string(f.unfinished_here) + " of " + std::to_string(f.station->queue_size) + ")");
        }
        if (f.held_here >= kMaxJobsAtStation) return refuse(std::to_string(f.held_here) + " jobs are held at " + f.station->name + " already (the finished ones stay until they are collected)");
    }
    if (req.has_tags || req.has_fuel || !req.flexible.empty()) return refuse("the request names tag, flexible or fuel resources: only the general ones are served");

    // what the player named, by record and rarity, in the order it was named (a record or a rarity named twice is added up: the client names each stack it offers)
    std::map<int64_t, std::vector<std::pair<int, int64_t>>> named;
    for (const protocol::repl::CraftResource& res : req.general) {
        for (const protocol::repl::CraftRarityCount& c : res.rarities) {
            if (c.from_warehouse) return refuse("the request takes from the warehouse: none can be used yet");
            if (c.count < 1) return refuse("the request names " + std::to_string(c.count) + " items");
            std::vector<std::pair<int, int64_t>>& by_rarity = named[res.record];
            const auto same = std::find_if(by_rarity.begin(), by_rarity.end(), [&](const std::pair<int, int64_t>& x) { return x.first == c.rarity; });
            if (same == by_rarity.end()) by_rarity.emplace_back(c.rarity, c.count);
            else same->second += c.count;
        }
    }
    const content::CraftRecipeData& data = *f.recipes;
    // the rarity rule of an item, and what the bags hold of it by rarity (a refusal says it, so the log shows what the client named against what is held)
    const auto facts_of = [&](int64_t record) { return f.rarity_facts ? f.rarity_facts(record) : items::default_rarity_facts(); };
    const auto held_text = [&](int64_t record, const items::ItemRarityFacts& facts) {
        const auto held = f.inventory->materials_by_rarity(record, facts);
        std::string text;
        for (int rarity = 0; rarity < items::kRarityCount; ++rarity) {
            if (held[static_cast<size_t>(rarity)] > 0) text += (text.empty() ? "" : ", ") + std::string(items::rarity_name(rarity)) + " " + std::to_string(held[static_cast<size_t>(rarity)]);
        }
        return text.empty() ? std::string("none") : text;
    };
    std::vector<Take> general_takes, reagent_takes;
    for (const content::CraftInput& in : r->inputs) {
        const int64_t need = static_cast<int64_t>(in.quantity) * req.quantity;
        if (need < 1) continue;
        if (in.primary) {
            // the reagent: stacks whose rarity is the recipe's minimum or better, the lowest quality first (a shop sells it at quality 1, and an item that is never Common shows its own rarity)
            Take t;
            t.record = in.item;
            t.facts = facts_of(in.item);
            t.rarity_lo = std::clamp(in.min_rarity, 0, items::kRarityCount - 1);
            t.count = need;
            t.lowest_first = true;
            const int64_t held = f.inventory->count_materials(t.record, t.facts, t.rarity_lo, t.rarity_hi);
            if (held < t.count) {
                return refuse("the material bags hold " + std::to_string(held) + " of the reagent " + std::to_string(t.record) + " at " + items::rarity_range_text(t.rarity_lo, t.rarity_hi) +
                              " (they hold: " + held_text(t.record, t.facts) + "), the job needs " + std::to_string(t.count));
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
            if (rarity < 0 || rarity >= items::kRarityCount) {
                return refuse("the request names rarity " + std::to_string(rarity) + " of item " + std::to_string(in.item) + ": the rarities are 0 (Common) to " + std::to_string(items::kRarityCount - 1) + " (Artifact)");
            }
            if (rarity < in.min_rarity) return refuse("the request names rarity " + std::to_string(rarity) + " of item " + std::to_string(in.item) + ", the recipe takes " + std::to_string(in.min_rarity) + " or better");
            // a general input: the stacks that show the rarity the request names (the client groups what it offers by that rarity)
            Take t;
            t.record = in.item;
            t.facts = facts_of(in.item);
            t.rarity_lo = t.rarity_hi = rarity;
            t.count = count;
            const int64_t held = f.inventory->count_materials(t.record, t.facts, t.rarity_lo, t.rarity_hi);
            if (held < t.count) {
                return refuse("the material bags hold " + std::to_string(held) + " of item " + std::to_string(t.record) + " of rarity " + std::string(items::rarity_name(rarity)) + " (they hold: " +
                              held_text(t.record, t.facts) + "), the request names " + std::to_string(t.count));
            }
            general_takes.push_back(t);
        }
        named.erase(it);
    }
    if (!named.empty()) return refuse("the request names item " + std::to_string(named.begin()->first) + ", which " + r->name + " does not take");
    // (two takes of one record and rarity would each pass the count check alone; the bags are checked again when the stacks are taken)
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
