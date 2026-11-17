// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/buy_orders.h  (node citizenship: buy orders, step 2)
//
//  A node's buy orders: what the mayor asks the players to bring, kept with the node's elections (world/elections.h NodeElection::buy_orders, so elections.json
//  holds them).  Pure logic over the data's table (world/buy_order_table.h, tools/worldd/gen_buy_orders.py); the session (session/player_buy_orders.cpp) talks to
//  the client.
//
//    placing     the mayor's ServerAddBuyOrder names a commodity, one of its recipes, an item for each of the recipe's material requirements (from that requirement's
//                options) and the order's numbers.  The recipe must be the commodity's and allowed in the node's biome (its mask; a node of no known biome allows
//                every recipe), and the node must have a free slot (citizenship_node's MinBuyOrderSlots of its level).  Each number is brought into the recipe's
//                range: NodeCost (copper, paid from the treasury at once), ContributionLimit (what one character may give in a contribution period), Requested
//                (the units the order asks for), RewardQuantity (rolls of the reward table per unit) and the days it runs [modelled: the client's own defaults, its
//                ContributionLimit of 300 among them, may lie outside the data's ranges, so a number is clamped rather than refused].  A material's quantity per
//                unit is its requirement's.
//    giving      a character delivers whole units: at most what the order still needs and what its limit leaves it this period (BuyOrderConstants'
//                BuyOrderContributionLimitFrequencySeconds, a day; the period starts at the character's first unit after the last one ended).  An order that has
//                all it asked for leaves the node's list.
//    expiring    an order whose days ran out leaves the list; its NodeCost is not given back [modelled].
// ============================================================================
#pragma once

#include "world/buy_order_table.h"
#include "world/mayor_guid.h"

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace aoc::worldd::world {

/// What one character gave an order in its current contribution period.
struct BuyOrderGiven {
    int32_t units = 0;
    int64_t since_ms = 0;   // when the period began (its first unit)
    bool operator==(const BuyOrderGiven&) const = default;
};

/// A buy order the node's mayor placed.
struct NodeBuyOrder {
    MayorGuid guid;
    int64_t   commodity = 0, recipe = 0, reward = 0;   // CommodityDefRecordBase, CommodityRecipeDefRecordBase, its RewardTableRecordBase
    int64_t   created_ms = 0, expires_ms = 0;
    std::vector<std::pair<int64_t, int32_t>> materials;   // the item for each requirement, and how many one unit takes
    int32_t   contribution_limit = 0, requested = 0, fulfilled = 0, reward_quantity = 0;
    int64_t   node_cost = 0;
    std::map<std::string, BuyOrderGiven> given;   // character id -> what it gave this period
    bool operator==(const NodeBuyOrder&) const = default;
};

struct NodeBuyOrders {
    std::vector<NodeBuyOrder> active;
    uint64_t                  serial = 0;   // the guids so far (they follow from it)
    bool operator==(const NodeBuyOrders&) const = default;
};

namespace buy_orders {

const Recipe*    recipe(int64_t guid);
const Commodity* commodity(int64_t def);
/// The recipe may be ordered in a node of `biome` (EBiomeType); a node of no known biome (0) allows every recipe.
bool in_biome(const Recipe& r, int biome);

/// What the mayor asks for (ServerAddBuyOrder's FBuyOrderFulfillmentData).
struct Request {
    int64_t              commodity = 0, recipe = 0;
    std::vector<int64_t> materials;   // the items the mayor picked, one for each requirement of the recipe (in any order)
    int32_t              contribution_limit = 0, requested = 0, reward_quantity = 0, seconds = 0;
    int64_t              node_cost = 0;
};

enum class Add : uint8_t { Ok, NoRecipe, WrongCommodity, WrongBiome, BadMaterials, OutOfRange, NoSlot, NoMoney };
const char* add_name(Add a);

/// Places `req` in `node`'s orders, its NodeCost from `treasury`, when it checks (nothing changes otherwise).  The new order's guid in `*made`; the numbers that were
/// brought into the recipe's range are listed in `*clamped` ("NodeCost 9000 -> 10000; ...").
Add add(NodeBuyOrders& orders, int64_t node, int biome, const Request& req, int max_slots, int64_t& treasury, int64_t now_ms, MayorGuid* made = nullptr,
        std::string* clamped = nullptr);

const NodeBuyOrder* find(const NodeBuyOrders& orders, const MayorGuid& g);

/// The orders whose days ran out by `now_ms` leave: how many did.
size_t expire(NodeBuyOrders& orders, int64_t now_ms);

/// The units `character` may still give the order now: what it still needs, within what the character's limit leaves this period.
int32_t left_for(const NodeBuyOrder& order, const std::string& character, int64_t now_ms);

enum class Give : uint8_t { Ok, NoOrder, Expired, Nothing, OverNeed, OverLimit };
const char* give_name(Give g);

/// `character` gives `units` to the order: counted, or refused with nothing changed.  `*completed`: the order has all it asked for now and has left the list.
Give give(NodeBuyOrders& orders, const MayorGuid& g, const std::string& character, int32_t units, int64_t now_ms, bool* completed = nullptr);

/// The contribution period (BuyOrderConstants).
inline constexpr int64_t kContributionPeriodMs = kContributionPeriodSeconds * 1000;

}  // namespace buy_orders
}  // namespace aoc::worldd::world
