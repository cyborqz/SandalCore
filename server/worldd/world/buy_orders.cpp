// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/buy_orders.cpp
// ============================================================================
#include "world/buy_orders.h"

#include <algorithm>
#include <string>

namespace aoc::worldd::world::buy_orders {

namespace {

/// `v` brought into [a, b] (either way round); a note in `clamped` when it moved.
template <typename T>
T into(T v, int64_t a, int64_t b, const char* what, std::string* clamped) {
    const int64_t lo = std::min(a, b), hi = std::max(a, b);
    const int64_t c = std::clamp<int64_t>(static_cast<int64_t>(v), lo, hi);
    if (c != static_cast<int64_t>(v) && clamped) {
        if (!clamped->empty()) *clamped += "; ";
        *clamped += std::string(what) + " " + std::to_string(static_cast<int64_t>(v)) + " -> " + std::to_string(c);
    }
    return static_cast<T>(c);
}

std::vector<NodeBuyOrder>::iterator find_it(NodeBuyOrders& orders, const MayorGuid& g) {
    return std::find_if(orders.active.begin(), orders.active.end(), [&](const NodeBuyOrder& o) { return o.guid == g; });
}

}  // namespace

const Recipe* recipe(int64_t guid) {
    if (guid == 0) return nullptr;
    for (const Recipe& r : kRecipes) {
        if (r.recipe == guid) return &r;
    }
    return nullptr;
}

const Commodity* commodity(int64_t def) {
    if (def == 0) return nullptr;
    for (const Commodity& c : kCommodities) {
        if (c.def == def) return &c;
    }
    return nullptr;
}

bool in_biome(const Recipe& r, int biome) {
    if (r.biomes == 0 || biome <= 0) return true;
    return biome < 32 && ((r.biomes >> biome) & 1u) != 0;
}

const char* add_name(Add a) {
    switch (a) {
        case Add::Ok: return "placed";
        case Add::NoRecipe: return "the recipe is not in the data";
        case Add::WrongCommodity: return "the recipe is not the commodity's";
        case Add::WrongBiome: return "the recipe is not ordered in the node's biome";
        case Add::BadMaterials: return "a material is not one the recipe's requirements take";
        case Add::OutOfRange: return "the order asks for nothing";
        case Add::NoSlot: return "the node has no free buy order slot";
        case Add::NoMoney: return "the treasury cannot pay the order's NodeCost";
    }
    return "?";
}

Add add(NodeBuyOrders& orders, int64_t node, int biome, const Request& req, int max_slots, int64_t& treasury, int64_t now_ms, MayorGuid* made, std::string* clamped) {
    const Recipe* r = recipe(req.recipe);
    if (!r) return Add::NoRecipe;
    if (req.commodity != 0 && req.commodity != r->commodity) return Add::WrongCommodity;
    if (!in_biome(*r, biome)) return Add::WrongBiome;
    // the materials: for each requirement, the first item the mayor named that it takes (none named: its first option [modelled])
    std::vector<std::pair<int64_t, int32_t>> materials;
    std::vector<bool> used(req.materials.size(), false);
    for (size_t k = 0; k < r->count; ++k) {
        const Requirement& q = kRequirements[r->first + k];
        if (q.count == 0) return Add::BadMaterials;
        int64_t pick = 0;
        for (size_t m = 0; m < req.materials.size() && pick == 0; ++m) {
            if (used[m]) continue;
            for (size_t o = q.first; o < q.first + q.count; ++o) {
                if (kOptions[o] == req.materials[m]) {
                    pick = req.materials[m];
                    used[m] = true;
                    break;
                }
            }
        }
        if (pick == 0) pick = kOptions[q.first];
        materials.emplace_back(pick, q.quantity);
    }
    for (size_t m = 0; m < req.materials.size(); ++m) {
        if (!used[m] && req.materials[m] != 0) return Add::BadMaterials;   // an item no requirement takes
    }
    if (static_cast<int64_t>(orders.active.size()) >= std::max(0, max_slots)) return Add::NoSlot;
    NodeBuyOrder o;
    std::string notes;
    o.node_cost = into(req.node_cost, r->min_cost, r->max_cost, "NodeCost", &notes);
    o.contribution_limit = into(req.contribution_limit, r->min_limit, r->max_limit, "ContributionLimit", &notes);
    o.requested = into(req.requested, r->min_quantity, r->max_quantity, "Requested", &notes);
    o.reward_quantity = into(req.reward_quantity, r->min_reward, r->max_reward, "RewardQuantity", &notes);
    const int64_t days = into<int64_t>((std::max<int64_t>(0, req.seconds) + 86399) / 86400, r->min_days, r->max_days, "days", &notes);
    if (o.requested <= 0 || o.contribution_limit <= 0 || days <= 0) return Add::OutOfRange;
    if (o.node_cost > treasury) return Add::NoMoney;
    treasury -= o.node_cost;
    const uint64_t u = static_cast<uint64_t>(node);
    o.guid = MayorGuid{static_cast<uint32_t>(u), static_cast<uint32_t>(u >> 32), 0x42554F52u /* "BUOR" */, static_cast<uint32_t>(++orders.serial)};
    o.commodity = r->commodity;
    o.recipe = r->recipe;
    o.reward = r->reward_table;
    o.created_ms = now_ms;
    o.expires_ms = now_ms + days * 86400000;
    o.materials = std::move(materials);
    if (made) *made = o.guid;
    if (clamped) *clamped = notes;
    orders.active.push_back(std::move(o));
    return Add::Ok;
}

const NodeBuyOrder* find(const NodeBuyOrders& orders, const MayorGuid& g) {
    for (const NodeBuyOrder& o : orders.active) {
        if (o.guid == g) return &o;
    }
    return nullptr;
}

size_t expire(NodeBuyOrders& orders, int64_t now_ms) {
    const size_t before = orders.active.size();
    orders.active.erase(std::remove_if(orders.active.begin(), orders.active.end(), [&](const NodeBuyOrder& o) { return o.expires_ms <= now_ms; }), orders.active.end());
    return before - orders.active.size();
}

int32_t left_for(const NodeBuyOrder& order, const std::string& character, int64_t now_ms) {
    const int32_t need = std::max(0, order.requested - order.fulfilled);
    int32_t given = 0;
    if (const auto it = order.given.find(character); it != order.given.end() && now_ms - it->second.since_ms < kContributionPeriodMs) given = it->second.units;
    return std::max(0, std::min(need, order.contribution_limit - given));
}

const char* give_name(Give g) {
    switch (g) {
        case Give::Ok: return "counted";
        case Give::NoOrder: return "the node has no such order";
        case Give::Expired: return "the order ran out";
        case Give::Nothing: return "it gives no unit";
        case Give::OverNeed: return "the order needs fewer units";
        case Give::OverLimit: return "the character's limit for this period is reached";
    }
    return "?";
}

Give give(NodeBuyOrders& orders, const MayorGuid& g, const std::string& character, int32_t units, int64_t now_ms, bool* completed) {
    if (completed) *completed = false;
    const auto it = find_it(orders, g);
    if (it == orders.active.end()) return Give::NoOrder;
    if (it->expires_ms <= now_ms) return Give::Expired;
    if (units <= 0) return Give::Nothing;
    if (units > it->requested - it->fulfilled) return Give::OverNeed;
    if (units > left_for(*it, character, now_ms)) return Give::OverLimit;
    BuyOrderGiven& mine = it->given[character];
    if (now_ms - mine.since_ms >= kContributionPeriodMs) mine = BuyOrderGiven{0, now_ms};
    mine.units += units;
    it->fulfilled += units;
    if (it->fulfilled >= it->requested) {
        if (completed) *completed = true;
        orders.active.erase(it);
    }
    return Give::Ok;
}

}  // namespace aoc::worldd::world::buy_orders
