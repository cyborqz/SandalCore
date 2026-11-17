// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_node_inventory.cpp  (node citizenship: the node's stores)
//
//  A node holds goods and money (world/elections.h NodeElection::stored and ::treasury), as the data's NodeInventoryContents does (StoredItems, CurrencyValues):
//    the start                          each node's record names what it starts with (CityNodeRecordBase.InitialNodeResourcesId; Default_InitialNodeResources:
//                                       1,525,050 copper and 225 each of the T1 Materials, Tools and Food commodities); a node has it once
//                                       (PlayerWorld::stock_node_stores, at set_elections; Elections::stock)
//    ServerRequestNodeInventoryData()   the node window's inventory tab (UNodeMaster::HandleRequestNodeInventory, UNodeInventory's refresh: the client keeps the
//                                       answer a while and asks again only after, or on the refresh button): ClientSendNodeInventoryData(FNodeInventoryData) of
//                                       the node the account is a citizen of, else of the node the player stands in
//    ServerGMAddNodeItem(NodeName, CommodityDefName, StackCount), ServerGMAddAllNodeItems(NodeName, StackCount)
//                                       a game master's (gm_rpc_admitted): StackCount of the item a commodity makes (CommodityDefRecordBase.CommodityProducedId),
//                                       or of every commodity's, into the node's stores
//    what spends them                   a mayoral commission's ResourceCost (session/player_mayor_commissions.cpp)
//  Not built: putting goods into the stores (ServerAddNodeResourceStorage, the guild storage menu's "open node storage"; the commodity turn-in box).
// ============================================================================
#include "session/player_session_util.h"

#include "world/elections.h"
#include "world/node_inventory_table.h"

#include <algorithm>
#include <string>
#include <vector>

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;
namespace schema = ::aoc::protocol::schema;

namespace {

const repl::Value* inv_param(const repl::ClientRpc& rpc, std::string_view path) {
    return rpc.call.decoded ? repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, path) : nullptr;
}

std::string inv_str(const repl::ClientRpc& rpc, std::string_view path) {
    const repl::Value* v = inv_param(rpc, path);
    const std::string* s = v ? v->get_if<std::string>() : nullptr;
    return s ? *s : std::string();
}

int64_t inv_int(const repl::ClientRpc& rpc, std::string_view path) {
    const repl::Value* v = inv_param(rpc, path);
    if (!v) return 0;
    if (const int64_t* i = v->get_if<int64_t>()) return *i;
    if (const uint64_t* u = v->get_if<uint64_t>()) return static_cast<int64_t>(*u);
    return 0;
}

/// The item the commodity named `name` makes (0: no such commodity); the names compare without regard to the case of letters.
int64_t commodity_item(const std::string& name) {
    const auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        return s;
    };
    const std::string want = lower(name);
    for (const world::node_inventory::Commodity& c : world::node_inventory::kCommodities) {
        if (lower(c.name) == want) return c.item;
    }
    return 0;
}

constexpr int32_t kMaxGmStack = 1'000'000;

}  // namespace

void PlayerWorld::stock_node_stores() {
    world::Elections* el = elections();
    if (!el) return;
    size_t stocked = 0;
    for (const world::node_inventory::NodeStart& n : world::node_inventory::kNodeStarts) {
        const world::node_inventory::Contents& c = world::node_inventory::kContents[n.contents];
        std::vector<std::pair<int64_t, int32_t>> items;
        for (size_t i = c.first; i < c.first + c.count; ++i) items.emplace_back(world::node_inventory::kStoredItems[i].item, world::node_inventory::kStoredItems[i].quantity);
        stocked += el->stock(n.node, c.money, items) ? 1 : 0;
    }
    if (stocked == 0) return;
    spdlog::info("[nodes] {} node(s) have what their records start them with (InitialNodeResourcesId: money for the treasury, goods for the stores)", stocked);
    save_elections();
}

int64_t PlayerSession::stores_node() const {
    if (const int64_t citizen = pb_.setup().citizen_node; citizen != 0) return citizen;
    const world::CityNodes* nodes = world_ ? world_->city_nodes() : nullptr;
    if (!nodes) return 0;
    const GoodPosition at = checker_.good();
    const world::CityNodeInfo* n = nodes->by_rank(nodes->node_at(at.x, at.y).node_rank);
    return n ? n->guid : 0;
}

void PlayerSession::send_node_inventory(Connection& conn, int64_t node) {
    const world::Elections* el = world_ ? world_->elections() : nullptr;
    if (!el || node == 0) return;
    repl::NodeInventoryData d;
    d.node = node;
    if (const auto* stored = el->stored(node)) {
        for (const auto& [item, quantity] : *stored) d.items.emplace_back(item, quantity);
    }
    d.money = el->treasury(node);
    ++node_inventories_sent_;
    spdlog::log(level_for(citizenship_log_), "[nodes] {}: the stores of node {}: {} kind(s) of goods, {} copper", name(), node, d.items.size(), d.money);
    send_citizenship_call(conn, "ClientSendNodeInventoryData", [d](const schema::RpcSchema& r, repl::RpcParams& p) {
        return repl::set_rpc_param(r, p, "Data", repl::Value(d));
    });
}

void PlayerSession::on_node_inventory_request(Connection& conn, const repl::ClientRpc&, Clock::time_point now) {
    if (!within_budget(node_inventory_budget_, "ServerRequestNodeInventoryData", now)) return;
    const int64_t node = stores_node();
    if (node == 0) {
        spdlog::log(level_for(citizenship_log_), "[nodes] {}: ServerRequestNodeInventoryData: the player is no citizen and stands in no node", name());
        return;
    }
    send_node_inventory(conn, node);
}

void PlayerSession::on_gm_node_items(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const std::string call = rpc.call.rpc ? rpc.call.rpc->name : "";
    const bool all = call == "ServerGMAddAllNodeItems";
    const std::string node_name = inv_str(rpc, "NodeName"), commodity = all ? std::string() : inv_str(rpc, "CommodityDefName");
    const int64_t count = inv_int(rpc, "StackCount");
    if (!gm_rpc_admitted(all ? "ServerGMAddAllNodeItems" : "ServerGMAddNodeItem", "NodeName=\"" + node_name.substr(0, 100) + "\"" + (all ? "" : " CommodityDefName=\"" + commodity.substr(0, 100) + "\"") +
                                   " StackCount=" + std::to_string(count),
                         now) ||
        !rpc.call.decoded) {
        return;
    }
    world::Elections* el = world_ ? world_->elections() : nullptr;
    std::string why = "this world keeps no node stores";
    const int64_t node = el ? gm_node_named(node_name, why) : 0;
    std::vector<int64_t> items;
    if (all) {
        for (const world::node_inventory::Commodity& c : world::node_inventory::kCommodities) items.push_back(c.item);
    } else if (const int64_t item = commodity_item(commodity); item != 0) {
        items.push_back(item);
    }
    if (node == 0 || items.empty() || count <= 0) {
        spdlog::warn("[gm] {}: {} does nothing: {}", name(), call,
                     node == 0 ? why : items.empty() ? "\"" + commodity.substr(0, 100) + "\" is no commodity" : std::string("the count is not above nothing"));
        return;
    }
    const int32_t each = static_cast<int32_t>(std::min<int64_t>(count, kMaxGmStack));
    el->change_node(node, [&](world::NodeElection& e) {
        for (const int64_t item : items) e.stored[item] = static_cast<int32_t>(std::min<int64_t>(int64_t{e.stored[item]} + each, INT32_MAX));
        return true;
    });
    world_->save_elections();
    spdlog::info("[gm] {}: {}: {} of {} kind(s) of goods into node {}'s stores", name(), call, each, items.size(), node);
    send_node_inventory(conn, node);
}

/// The client calls of this feature (session/rpc_table.h).
void PlayerSession::register_node_inventory_rpcs(RpcTable& t) {
    t.add(RpcScope::PlayerController, "ServerRequestNodeInventoryData", [](PlayerSession& s, const RpcContext& c) { s.on_node_inventory_request(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerGMAddNodeItem", [](PlayerSession& s, const RpcContext& c) { s.on_gm_node_items(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerGMAddAllNodeItems", [](PlayerSession& s, const RpcContext& c) { s.on_gm_node_items(c.conn, c.rpc, c.now); });
}

}}  // namespace aoc::worldd
