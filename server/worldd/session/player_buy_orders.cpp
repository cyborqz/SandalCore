// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_buy_orders.cpp  (node citizenship: buy orders, step 2)
//
//  A node's buy orders (world/buy_orders.h) between a session and the client.  Read from the client (HANDOFF 0000014):
//
//    the window      the Buy Order Manager's conversation (zLegacy_DD_ServiceNPC_BuyOrderManager) starts at an automatic node whose line to the BuyOrderFulfillment
//                    node holds while Node_IsBuyOrderActive(the NPC's node); the talk queues that node's panel (ESubDialogueType BuyOrderFulfillment: the dialogue widget's
//                    HandleBuyOrderFulfillmentSubDialogue opens the HUD's BuyOrderFulfillmentMenu) and the server names the window's node with
//                    ClientOpenBuyOrderFulfillmentWindow(NodeGuid) (AAoCHUDBase::SetBuyOrderFulfillmentNodeId, which needs the talking window open)
//                    [modelled: the order, the panel first and then the call, then the node's data unasked];
//    the data        ServerGetPossibleAndActiveBuyOrders(NodeGuid) -> ClientUpdateBuyOrderData(NodeGuid, FClientBuyOrderData, FPlayerBuyOrderContributions): the
//                    orders that run (their numbers on the world clock, as the mayor's commissions'), what this character gave each this period, the node's slots,
//                    and for the node's mayor every commodity recipe the node's biome allows (the treasury window's choices; others get none) [modelled];
//    placing         the mayor's treasury window sends ServerAddBuyOrder(FBuyOrderFulfillmentData): world/buy_orders.h checks and places it (NodeCost from the
//                    treasury); the feed gets NewBuyOrder (13); the answer is the node's data and stores again;
//    delivering      ServerFulfillActiveBuyOrder(NodeGuid, RequestedBuyOrderGuid, FulfillmentCombo {ItemCombination [{FItemInstanceGuid, Quantity, InventoryLocated}],
//                    OriginalNumToFulfill, AdjustedNumToFulfill, TransactionId}) while the panel is open at that node's Buy Order Manager: every named stack must be in
//                    the bag it says and hold the item and the quantity, the stacks must hold at least each material's quantity per unit times the units, and the
//                    character's limit must leave room; then the materials leave the bags, the units count (an order that has them all leaves the list, the feed
//                    gets BuyOrderFulfilled, 14), each unit adds one of the commodity's item to the node's stores [modelled], the reward table is rolled
//                    RewardQuantity times per unit (at most kMaxBuyOrderRewardRolls) [modelled], and ClientBuyOrderFulfilled(the order, the combo) answers.  A refusal
//                    is ClientBuyOrderFulfillmentRejection(the guid, the combo) with nothing taken.
//  The game's server bodies of these calls are not in the client's exe (empty stubs), so what the server does is modelled on what the client sends and expects.
// ============================================================================
#include "session/player_session_util.h"

#include "combat/rewards.h"
#include "world/buy_orders.h"
#include "world/citizenship.h"
#include "world/elections.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;
namespace schema = ::aoc::protocol::schema;
namespace bo = world::buy_orders;

namespace {

/// The most reward rolls one delivery makes.
constexpr int64_t kMaxBuyOrderRewardRolls = 100;

const repl::Value* bparam(const repl::ClientRpc& rpc, std::string_view path) {
    return rpc.call.decoded && rpc.call.rpc ? repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, path) : nullptr;
}

int64_t bint(const repl::Value* v) {
    if (!v) return 0;
    if (const int64_t* i = v->get_if<int64_t>()) return *i;
    if (const uint64_t* u = v->get_if<uint64_t>()) return static_cast<int64_t>(*u);
    if (const bool* b = v->get_if<bool>()) return *b ? 1 : 0;
    return 0;
}

int64_t bint(const repl::ClientRpc& rpc, std::string_view path) { return bint(bparam(rpc, path)); }

int32_t bi32(const repl::ClientRpc& rpc, std::string_view path) { return static_cast<int32_t>(std::clamp<int64_t>(bint(rpc, path), INT32_MIN, INT32_MAX)); }

/// The local handle of the element command named `path` of the RPC (0: none).
uint16_t belem(const schema::RpcSchema& rpc, std::string_view path) {
    for (const schema::RepCommand& c : rpc.element_commands) {
        if (path == c.path) return c.handle;
    }
    return 0;
}

const repl::Value* bvalue(const repl::ElementState& e, uint16_t handle) {
    if (handle == 0) return nullptr;
    const auto it = e.values.find(handle);
    return it == e.values.end() ? nullptr : &it->second;
}

repl::Value i32(uint32_t w) { return repl::Value::i64(static_cast<int32_t>(w)); }

world::MayorGuid bguid(const repl::ClientRpc& rpc, const std::string& base) {
    return world::MayorGuid{static_cast<uint32_t>(bint(rpc, base + ".A")), static_cast<uint32_t>(bint(rpc, base + ".B")), static_cast<uint32_t>(bint(rpc, base + ".C")),
                            static_cast<uint32_t>(bint(rpc, base + ".D"))};
}

repl::GuidValue wire_guid(const world::MayorGuid& g) {
    repl::GuidValue w;
    w.a = g.a;
    w.b = g.b;
    w.c = g.c;
    w.d = g.d;
    return w;
}

std::string hex_of(const world::MayorGuid& g) { return items::guid_hex(wire_guid(g)); }

/// Where a delivery's stack lies (EItemLocation): the bag, the quest bag or a resource container.
std::optional<svc::ItemContainer> buy_order_container(int64_t location) {
    switch (location) {
        case static_cast<int64_t>(repl::ItemLocation::Backpack): return svc::ItemContainer::Backpack;
        case static_cast<int64_t>(repl::ItemLocation::Quest): return svc::ItemContainer::Quest;
        case static_cast<int64_t>(repl::ItemLocation::Resource0): return svc::ItemContainer::Resource0;
        case static_cast<int64_t>(repl::ItemLocation::Resource1): return svc::ItemContainer::Resource1;
        case static_cast<int64_t>(repl::ItemLocation::Resource2): return svc::ItemContainer::Resource2;
        default: return std::nullopt;
    }
}

template <typename T>
T fit(int64_t v) { return static_cast<T>(std::clamp<int64_t>(v, std::numeric_limits<T>::min(), std::numeric_limits<T>::max())); }

/// Every commodity recipe the node's biome allows, as the treasury window lists them.
std::vector<repl::BuyOrderPossible> possible_orders(int biome) {
    std::vector<repl::BuyOrderPossible> out;
    for (const bo::Commodity& c : bo::kCommodities) {
        repl::BuyOrderPossible p;
        p.commodity = c.def;
        p.category = c.category;
        for (size_t i = c.first; i < c.first + c.count; ++i) {
            const bo::Recipe& r = bo::kRecipes[i];
            if (!bo::in_biome(r, biome)) continue;
            repl::BuyOrderOption o;
            o.recipe = r.recipe;
            o.limit_min = fit<int16_t>(r.min_limit);
            o.limit_max = fit<int16_t>(r.max_limit);
            o.days_min = fit<uint8_t>(r.min_days);
            o.days_max = fit<uint8_t>(r.max_days);
            o.reward_min = fit<int16_t>(r.min_reward);
            o.reward_max = fit<int16_t>(r.max_reward);
            o.cost_min = r.min_cost;
            o.cost_max = r.max_cost;
            o.quantity_min = fit<int16_t>(r.min_quantity);
            o.quantity_max = fit<int16_t>(r.max_quantity);
            for (size_t k = r.first; k < r.first + r.count; ++k) {
                const bo::Requirement& q = bo::kRequirements[k];
                o.materials.emplace_back(bo::kOptions + q.first, bo::kOptions + q.first + q.count);
                o.quantities.push_back(q.quantity);
            }
            p.options.push_back(std::move(o));
        }
        if (!p.options.empty()) out.push_back(std::move(p));
    }
    return out;
}

/// A delivery's combo as the answers carry it back.
struct ComboEcho {
    repl::ArrayValue items;
    int64_t          original = 0, adjusted = 0;
    world::MayorGuid transaction;
};

bool set_combo(const schema::RpcSchema& r, repl::RpcParams& p, const ComboEcho& c) {
    return repl::set_rpc_param(r, p, "FulfillmentCombo.ItemCombination", repl::Value(c.items)) &&
           repl::set_rpc_param(r, p, "FulfillmentCombo.OriginalNumToFulfill", repl::Value::i64(c.original)) &&
           repl::set_rpc_param(r, p, "FulfillmentCombo.AdjustedNumToFulfill", repl::Value::i64(c.adjusted)) &&
           repl::set_rpc_param(r, p, "FulfillmentCombo.TransactionId.A", i32(c.transaction.a)) &&
           repl::set_rpc_param(r, p, "FulfillmentCombo.TransactionId.B", i32(c.transaction.b)) &&
           repl::set_rpc_param(r, p, "FulfillmentCombo.TransactionId.C", i32(c.transaction.c)) &&
           repl::set_rpc_param(r, p, "FulfillmentCombo.TransactionId.D", i32(c.transaction.d));
}

}  // namespace

int PlayerSession::buy_order_slots(int64_t node) const {
    if (buy_order_slots_for_test_) return *buy_order_slots_for_test_;
    const world::CitizenshipRules* rules = world_ ? world_->citizenship_rules() : nullptr;
    const world::CityNodes* nodes = world_ ? world_->city_nodes() : nullptr;
    return rules && nodes ? rules->buy_order_slots_at(nodes->level_of(node)) : 0;
}

int PlayerSession::node_biome(int64_t node) const {
    const world::CityNodes* nodes = world_ ? world_->city_nodes() : nullptr;
    const world::CityNodeInfo* n = nodes ? nodes->by_guid(node) : nullptr;
    const world::CityZoneInfo* z = n && n->zoi_guid != 0 ? nodes->zone_by_guid(n->zoi_guid) : nullptr;   // (the biome of the node's zone of influence)
    return z ? z->biome : 0;
}

int64_t PlayerSession::to_world_seconds(int64_t wall_at_ms, Clock::time_point now) {
    return static_cast<int64_t>(std::floor(quest_clock(now) + static_cast<double>(wall_at_ms - wall_ms()) / 1000.0));
}

void PlayerSession::open_buy_order_window(Connection& conn, const world::WorldActor& npc, Clock::time_point now) {
    const world::CityNodes* nodes = world_ ? world_->city_nodes() : nullptr;
    const world::CityNodeInfo* n = nodes ? nodes->by_rank(nodes->node_at(npc.x, npc.y).node_rank) : nullptr;
    if (!n) {
        spdlog::log(level_for(citizenship_log_), "[nodes] {}: {} opens the buy orders but stands in no node: nothing to show", name(), npc.npc.name);
        return;
    }
    const int64_t node = n->guid;
    buy_order_node_ = node;
    ++buy_order_windows_;
    spdlog::info("[nodes] {}: {} opens the buy orders of node {} ({})", name(), npc.npc.name, n->display_name, node);
    send_citizenship_call(conn, "ClientOpenBuyOrderFulfillmentWindow", [node](const schema::RpcSchema& r, repl::RpcParams& p) {
        return repl::set_rpc_param(r, p, "NodeGuid", repl::Value::i64(node));
    });
    send_buy_order_data(conn, node, now);
}

void PlayerSession::send_buy_order_data(Connection& conn, int64_t node, Clock::time_point now) {
    world::Elections* el = world_ ? world_->elections() : nullptr;
    if (!el || node == 0) return;
    const int64_t wall = wall_ms();
    if (el->change_node(node, [&](world::NodeElection& e) { return bo::expire(e.buy_orders, wall) > 0; })) world_->save_elections();
    repl::ClientBuyOrderData d;
    repl::PlayerBuyOrderContributions c;
    if (el->mayor_node(account_id_) == node) d.possible = possible_orders(node_biome(node));
    int64_t refresh_ms = 0;   // the end of this character's earliest running period
    if (const world::NodeBuyOrders* b = el->buy_orders(node)) {
        for (const world::NodeBuyOrder& o : b->active) {
            repl::BuyOrderData w;
            w.commodity = o.commodity;
            w.recipe = o.recipe;
            w.reward = o.reward;
            w.time_created = to_world_seconds(o.created_ms, now);
            for (const auto& [item, quantity] : o.materials) w.materials.push_back(repl::BuyOrderMaterial{item, quantity});
            w.contribution_limit = o.contribution_limit;
            w.requested = o.requested;
            w.visibly_fulfilled = o.fulfilled;
            w.node_cost = o.node_cost;
            w.reward_quantity = o.reward_quantity;
            w.time_remaining = fit<int32_t>((o.expires_ms - wall) / 1000);
            d.active.emplace_back(wire_guid(o.guid), std::move(w));
            if (const auto it = o.given.find(character_id_); it != o.given.end() && wall - it->second.since_ms < bo::kContributionPeriodMs) {
                c.contributions.emplace_back(wire_guid(o.guid), it->second.units);
                const int64_t ends = it->second.since_ms + bo::kContributionPeriodMs;
                if (refresh_ms == 0 || ends < refresh_ms) refresh_ms = ends;
            }
        }
    }
    d.max_slots = buy_order_slots(node);
    c.refresh_time = refresh_ms == 0 ? 0 : fit<int32_t>(to_world_seconds(refresh_ms, now));   // [modelled: on the world clock, 0 for none]
    ++buy_order_data_sent_;
    spdlog::log(level_for(citizenship_log_), "[nodes] {}: the buy orders of node {}: {} running of {} slot(s), {} commodit(ies) to order, {} given to", name(), node, d.active.size(),
                d.max_slots, d.possible.size(), c.contributions.size());
    send_citizenship_call(conn, "ClientUpdateBuyOrderData", [node, d, c](const schema::RpcSchema& r, repl::RpcParams& p) {
        return repl::set_rpc_param(r, p, "NodeGuid", repl::Value::i64(node)) && repl::set_rpc_param(r, p, "BuyOrderData", repl::Value(d)) &&
               repl::set_rpc_param(r, p, "BuyOrderContributions", repl::Value(c));
    });
}

void PlayerSession::on_buy_orders_request(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!within_budget(buy_order_budget_, "ServerGetPossibleAndActiveBuyOrders", now)) return;
    int64_t node = bint(rpc, "NodeGuid");
    const world::CityNodes* nodes = world_ ? world_->city_nodes() : nullptr;
    if (node == 0 || !nodes || !nodes->by_guid(node)) node = buy_order_node_ != 0 ? buy_order_node_ : stores_node();
    if (node == 0) {
        spdlog::log(level_for(citizenship_log_), "[nodes] {}: ServerGetPossibleAndActiveBuyOrders names no node and the player stands in none", name());
        return;
    }
    send_buy_order_data(conn, node, now);
}

void PlayerSession::on_buy_order_add(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!within_budget(buy_order_budget_, "ServerAddBuyOrder", now)) return;
    world::Elections* el = world_ ? world_->elections() : nullptr;
    const int64_t node = el ? el->mayor_node(account_id_) : 0;
    if (node == 0) {
        ++buy_orders_refused_;
        spdlog::log(level_for(citizenship_log_), "[nodes] {}: ServerAddBuyOrder is refused: the account is no node's mayor", name());
        if (el) send_must_be_mayor(conn);
        return;
    }
    const auto refuse = [&](const std::string& why) {
        ++buy_orders_refused_;
        spdlog::info("[nodes] {}: the mayor of node {} does not place the buy order: {}", name(), node, why);
        send_buy_order_data(conn, node, now);
    };
    if (!rpc.call.decoded || !rpc.call.rpc) return refuse("its parameters do not decode");
    bo::Request req;
    req.commodity = bint(rpc, "RequestedBuyOrder.CommodityId");
    req.recipe = bint(rpc, "RequestedBuyOrder.CommodityRecipeId");
    req.contribution_limit = bi32(rpc, "RequestedBuyOrder.ContributionLimit");
    req.requested = bi32(rpc, "RequestedBuyOrder.Requested");
    req.node_cost = bint(rpc, "RequestedBuyOrder.NodeCost");
    req.reward_quantity = bi32(rpc, "RequestedBuyOrder.RewardQuantity");
    req.seconds = bi32(rpc, "RequestedBuyOrder.TimeRemainingSeconds");
    if (const repl::Value* m = bparam(rpc, "RequestedBuyOrder.Materials")) {
        const repl::ArrayValue* list = m->get_if<repl::ArrayValue>();
        const uint16_t h = belem(*rpc.call.rpc, "Materials.Resource");
        if (list && list->elements.size() > 8) return refuse("it names " + std::to_string(list->elements.size()) + " materials");
        if (list && h != 0) {
            for (const repl::ElementState& e : list->elements) req.materials.push_back(bint(bvalue(e, h)));
        }
    }
    const int64_t wall = wall_ms();
    const int biome = node_biome(node), slots = buy_order_slots(node);
    bo::Add result = bo::Add::NoRecipe;
    world::MayorGuid made;
    std::string clamped;
    int64_t left = 0;
    el->change_node(node, [&](world::NodeElection& e) {
        bo::expire(e.buy_orders, wall);
        result = bo::add(e.buy_orders, node, biome, req, slots, e.treasury, wall, &made, &clamped);
        left = e.treasury;
        return result == bo::Add::Ok;
    });
    if (result != bo::Add::Ok) return refuse(bo::add_name(result));
    ++buy_orders_added_;
    el->add_news(node, world::NodeNews{13, wall, name()});   // ENodeSystemNotificationType::NewBuyOrder
    world_->save_elections();
    const world::NodeBuyOrders* b = el->buy_orders(node);
    const world::NodeBuyOrder* o = b ? bo::find(*b, made) : nullptr;
    const bo::Recipe* r = bo::recipe(req.recipe);
    spdlog::info("[nodes] {}: the mayor of node {} orders {} unit(s) of {} ({} a character a day, {} day(s)) for {} copper of the treasury (left {}){}{}", name(), node,
                 o ? o->requested : 0, r ? r->name : std::to_string(req.recipe), o ? o->contribution_limit : 0, o ? (o->expires_ms - o->created_ms) / 86400000 : 0,
                 o ? o->node_cost : 0, left, clamped.empty() ? "" : "; brought into the data's ranges: ", clamped);
    send_buy_order_data(conn, node, now);
    send_node_inventory(conn, node);
}

void PlayerSession::on_buy_order_fulfil(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!within_budget(buy_order_budget_, "ServerFulfillActiveBuyOrder", now)) return;
    const world::MayorGuid g = bguid(rpc, "RequestedBuyOrderGuid");
    ComboEcho echo;
    const repl::Value* combo = bparam(rpc, "FulfillmentCombo.ItemCombination");
    const repl::ArrayValue* list = combo ? combo->get_if<repl::ArrayValue>() : nullptr;
    if (list) echo.items = *list;
    echo.original = bint(rpc, "FulfillmentCombo.OriginalNumToFulfill");
    echo.adjusted = bint(rpc, "FulfillmentCombo.AdjustedNumToFulfill");
    echo.transaction = bguid(rpc, "FulfillmentCombo.TransactionId");
    const auto refuse = [&](const std::string& why) {
        ++buy_order_deliveries_refused_;
        spdlog::info("[nodes] {}: a buy order delivery is refused: {}", name(), why);
        send_citizenship_call(conn, "ClientBuyOrderFulfillmentRejection", [g, echo](const schema::RpcSchema& r, repl::RpcParams& p) {
            return repl::set_rpc_param(r, p, "RequestedBuyOrderGuid.A", i32(g.a)) && repl::set_rpc_param(r, p, "RequestedBuyOrderGuid.B", i32(g.b)) &&
                   repl::set_rpc_param(r, p, "RequestedBuyOrderGuid.C", i32(g.c)) && repl::set_rpc_param(r, p, "RequestedBuyOrderGuid.D", i32(g.d)) && set_combo(r, p, echo);
        });
    };
    world::Elections* el = world_ ? world_->elections() : nullptr;
    if (!el) return refuse("this world keeps no buy orders");
    if (!rpc.call.decoded || !rpc.call.rpc) return refuse("its parameters do not decode");
    if (dead_) return refuse("the player is dead");
    const int64_t node = bint(rpc, "NodeGuid");
    if (node == 0 || node != buy_order_node_) return refuse("node " + std::to_string(node) + " is not the one whose Buy Order Manager the character talks to");
    if (!buy_order_window_for_test_) {
        if (!talk_.open() || !talk_.sub() || talk_.sub()->type != 5) return refuse("no buy order window is open");
        const world::WorldActor* npc = registry_ ? registry_->find(talk_.state().source) : nullptr;
        const GoodPosition at = checker_.good();
        if (!npc || std::hypot(npc->x - at.x, npc->y - at.y) > kDialogueRangeCm) return refuse("the player is not by the Buy Order Manager");
    }
    if (!room_for(8)) return refuse("the replies are queued too deep");
    const int64_t wall = wall_ms();
    if (el->change_node(node, [&](world::NodeElection& e) { return bo::expire(e.buy_orders, wall) > 0; })) world_->save_elections();
    const world::NodeBuyOrders* orders = el->buy_orders(node);
    const world::NodeBuyOrder* found = orders ? bo::find(*orders, g) : nullptr;
    if (!found) return refuse("node " + std::to_string(node) + " runs no order " + hex_of(g) + " (it ran out or has all it asked for)");
    const world::NodeBuyOrder order = *found;   // (a copy: a filled order leaves the list)
    const int64_t units = echo.adjusted > 0 ? echo.adjusted : echo.original;
    if (units <= 0 || units > 100000) return refuse("it gives " + std::to_string(units) + " unit(s)");
    const int32_t may = bo::left_for(order, character_id_, wall);
    if (units > may) return refuse("the character may give " + std::to_string(may) + " unit(s) now, it offers " + std::to_string(units));

    // ---- the stacks it names ----
    if (!list || list->elements.empty() || list->elements.size() > 64) return refuse(!list || list->elements.empty() ? "it names no stack" : "it names too many stacks");
    const schema::RpcSchema& srpc = *rpc.call.rpc;
    const uint16_t h_guid[4] = {belem(srpc, "ItemCombination.FItemInstanceGuid.A"), belem(srpc, "ItemCombination.FItemInstanceGuid.B"),
                                belem(srpc, "ItemCombination.FItemInstanceGuid.C"), belem(srpc, "ItemCombination.FItemInstanceGuid.D")};
    const uint16_t h_quantity = belem(srpc, "ItemCombination.Quantity"), h_location = belem(srpc, "ItemCombination.InventoryLocated");
    if (!h_guid[0] || !h_guid[1] || !h_guid[2] || !h_guid[3] || !h_quantity || !h_location) return refuse("the call's layout is not the one this build reads");
    std::map<int64_t, int64_t> offered;      // item record -> what the named stacks give
    std::map<std::string, int64_t> claimed;  // stack -> what the call takes from it so far
    for (const repl::ElementState& e : list->elements) {
        repl::GuidValue gv;
        gv.a = static_cast<uint32_t>(bint(bvalue(e, h_guid[0])));
        gv.b = static_cast<uint32_t>(bint(bvalue(e, h_guid[1])));
        gv.c = static_cast<uint32_t>(bint(bvalue(e, h_guid[2])));
        gv.d = static_cast<uint32_t>(bint(bvalue(e, h_guid[3])));
        const std::string guid = items::guid_hex(gv);
        const int64_t quantity = bint(bvalue(e, h_quantity));
        if (quantity <= 0) return refuse("a stack is named with no quantity");
        const auto container = buy_order_container(bint(bvalue(e, h_location)));
        if (!container) return refuse("a stack lies where a delivery does not look (location " + std::to_string(bint(bvalue(e, h_location))) + ")");
        const svc::InventoryItem* held = inventory_.find_guid(guid);
        if (held && held->container != *container) held = nullptr;
        if (!held) return refuse("a stack the character does not carry (guid " + guid + ")");
        int64_t& used = claimed[guid];
        used += quantity;
        if (used > held->quantity) return refuse("a stack is asked for " + std::to_string(used) + ", it holds " + std::to_string(held->quantity));
        offered[held->item_record_id] += quantity;
    }
    std::vector<std::pair<int64_t, int64_t>> leaving;   // what the units take: each material, its quantity per unit times the units
    for (const auto& [item, per_unit] : order.materials) {
        const int64_t need = static_cast<int64_t>(per_unit) * units;
        const int64_t have = offered.count(item) ? offered.at(item) : 0;
        if (have < need) return refuse(std::to_string(units) + " unit(s) take " + std::to_string(need) + " of item " + std::to_string(item) + ", the stacks named give " + std::to_string(have));
        leaving.emplace_back(item, need);
        offered.erase(item);
    }
    if (!offered.empty()) return refuse("it names item " + std::to_string(offered.begin()->first) + ", which the order does not take");

    // ---- carry it out: the units count, the stores fill, the materials leave the bags ----
    const bo::Commodity* commodity = bo::commodity(order.commodity);
    bo::Give result = bo::Give::NoOrder;
    bool completed = false;
    el->change_node(node, [&](world::NodeElection& e) {
        result = bo::give(e.buy_orders, g, character_id_, static_cast<int32_t>(units), wall, &completed);
        if (result != bo::Give::Ok) return false;
        if (commodity && commodity->item != 0) e.stored[commodity->item] = static_cast<int32_t>(std::min<int64_t>(int64_t{e.stored[commodity->item]} + units, INT32_MAX));
        return true;
    });
    if (result != bo::Give::Ok) return refuse(bo::give_name(result));
    take_quest_items(conn, leaving);
    if (completed) el->add_news(node, world::NodeNews{14, wall, name()});   // ENodeSystemNotificationType::BuyOrderFulfilled
    world_->save_elections();
    ++buy_order_deliveries_;
    buy_order_units_ += units;
    const bo::Recipe* r = bo::recipe(order.recipe);
    spdlog::info("[nodes] {}: gives {} unit(s) to the buy order {} of node {} ({} of {}{}); the stores gain {} of item {}", name(), units, r ? r->name : hex_of(g), node,
                 order.fulfilled + units, order.requested, completed ? ": it has all it asked for" : "", units, commodity ? commodity->item : 0);
    // the answer: the order as it stands now
    world::NodeBuyOrder after = order;
    after.fulfilled += static_cast<int32_t>(units);
    const int64_t created = to_world_seconds(after.created_ms, now), expires = to_world_seconds(after.expires_ms, now);
    const int32_t remaining = fit<int32_t>((after.expires_ms - wall) / 1000);
    repl::ArrayValue materials;
    for (const auto& [item, quantity] : after.materials) {
        repl::ElementState m;
        m.values[1] = repl::Value::i64(item);
        m.values[2] = repl::Value::u64(static_cast<uint64_t>(std::max(0, quantity)));
        materials.elements.push_back(std::move(m));
    }
    send_citizenship_call(conn, "ClientBuyOrderFulfilled", [after, created, expires, remaining, materials, echo](const schema::RpcSchema& rs, repl::RpcParams& p) {
        return repl::set_rpc_param(rs, p, "FulfilledBuyOrder.TimeCreated", repl::Value::i64(created)) &&
               repl::set_rpc_param(rs, p, "FulfilledBuyOrder.CommodityId", repl::Value::i64(after.commodity)) &&
               repl::set_rpc_param(rs, p, "FulfilledBuyOrder.CommodityRecipeId", repl::Value::i64(after.recipe)) &&
               repl::set_rpc_param(rs, p, "FulfilledBuyOrder.RewardId", repl::Value::i64(after.reward)) &&
               repl::set_rpc_param(rs, p, "FulfilledBuyOrder.Materials", repl::Value(materials)) &&
               repl::set_rpc_param(rs, p, "FulfilledBuyOrder.ContributionLimit", repl::Value::i64(after.contribution_limit)) &&
               repl::set_rpc_param(rs, p, "FulfilledBuyOrder.NumTimesLimitIncreased", repl::Value::u64(0)) &&
               repl::set_rpc_param(rs, p, "FulfilledBuyOrder.VisiblyFulfilled", repl::Value::i64(after.fulfilled)) &&
               repl::set_rpc_param(rs, p, "FulfilledBuyOrder.Requested", repl::Value::i64(after.requested)) &&
               repl::set_rpc_param(rs, p, "FulfilledBuyOrder.NodeCost", repl::Value::i64(after.node_cost)) &&
               repl::set_rpc_param(rs, p, "FulfilledBuyOrder.RewardQuantity", repl::Value::i64(after.reward_quantity)) &&
               repl::set_rpc_param(rs, p, "FulfilledBuyOrder.TimeRemainingSeconds", repl::Value::i64(remaining)) &&
               repl::set_rpc_param(rs, p, "FulfilledBuyOrder.ExpirationTimeInSeconds", repl::Value::i64(fit<int32_t>(expires))) && set_combo(rs, p, echo);
    });
    roll_buy_order_rewards(conn, order.reward, static_cast<int64_t>(order.reward_quantity) * units, now);
    send_buy_order_data(conn, node, now);
}

void PlayerSession::roll_buy_order_rewards(Connection& conn, int64_t table, int64_t rolls, Clock::time_point now) {
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    if (table == 0 || rolls <= 0 || !db) return;
    rolls = std::min(rolls, kMaxBuyOrderRewardRolls);
    // the area's book of tables, or this table on its own (an order's table is no mob's or quest's), as a random coffer does (session/player_chests.cpp)
    const combat::RewardBook* book = world_->rewards();
    std::optional<combat::RewardBook> local_book;
    if ((!book || !book->table(table)) && world_->ability_catalog()) {
        std::string why;
        auto tables = db->loot_tables({table}, why);
        if (!tables) {
            spdlog::warn("[nodes] {}: the buy order's reward table {} does not read: {}", name(), table, why);
            return;
        }
        local_book.emplace(*world_->ability_catalog(), std::move(*tables), 0);
        book = &*local_book;
    }
    if (!book || !book->table(table)) {
        spdlog::warn("[nodes] {}: the buy order's reward table {} cannot be rolled", name(), table);
        return;
    }
    std::string error;
    auto run = gather_run(error);
    if (!run) {
        spdlog::warn("[nodes] {}: the buy order's reward is not rolled: no stat run ({})", name(), error);
        return;
    }
    combat::RollContext ctx;
    ctx.npc = run.get();
    ctx.player_level = progress_.level;
    ctx.loot_max_level = progress_.level;
    ctx.rng = &chest_rng_;
    const GoodPosition at = checker_.good();
    if (const world::CityNodes* nodes = world_->city_nodes()) {
        const world::NodeSite site = nodes->node_at(at.x, at.y);
        ctx.nodes = nodes;
        ctx.node = site.node_rank;
        ctx.biome = site.biome;
    }
    gather::Harvest harvest;
    harvest.durability_loss = 0;
    for (int64_t i = 0; i < rolls; ++i) {
        const combat::Rolled rolled = book->roll({table}, ctx);
        harvest.items.insert(harvest.items.end(), rolled.loot.begin(), rolled.loot.end());
        harvest.items.insert(harvest.items.end(), rolled.direct.begin(), rolled.direct.end());
        for (const combat::RolledXp& x : rolled.xp) {
            if (x.type == "Adventuring") harvest.adventuring_xp += x.amount;
        }
        harvest.tables += rolled.tables;
        harvest.gated += rolled.gated;
    }
    if (harvest.items.empty() && harvest.adventuring_xp == 0) {
        spdlog::info("[nodes] {}: the buy order's reward table {} rolled nothing in {} roll(s)", name(), table, rolls);
        return;
    }
    HarvestFrame frame;
    frame.what = "a buy order's reward (table " + std::to_string(table) + ", " + std::to_string(rolls) + " roll(s))";
    frame.harvest = &harvest;
    frame.tag = "nodes";
    deliver_harvest(conn, now, frame);
}

/// The client calls of the buy orders (session/rpc_table.h).
void PlayerSession::register_buy_orders_rpcs(RpcTable& t) {
    t.add(RpcScope::PlayerController, "ServerGetPossibleAndActiveBuyOrders", [](PlayerSession& s, const RpcContext& c) { s.on_buy_orders_request(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerAddBuyOrder", [](PlayerSession& s, const RpcContext& c) { s.on_buy_order_add(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerFulfillActiveBuyOrder", [](PlayerSession& s, const RpcContext& c) { s.on_buy_order_fulfil(c.conn, c.rpc, c.now); });
}

}}  // namespace aoc::worldd
