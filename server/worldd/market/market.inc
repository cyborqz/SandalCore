// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  market/market.inc (see market.h)
// ============================================================================
#include "market/market.h"

#include "bootstrap/bootstrap.h"
#include "session/player_session.h"
#include "vendor/node_tax.h"
#include "world/city_nodes.h"

#include <spdlog/spdlog.h>

#include <span>
#include <utility>

namespace aoc::worldd {

namespace repl = ::aoc::protocol::repl;

void PlayerSession::open_marketplace(Connection& conn, const world::WorldActor& manager) {
    if (dead_) return;
    int64_t node = market::node_of(manager.actor);   // the node the actor carries (the area loader sets it)
    if (node == 0) {
        const world::CityNodes* nodes = world_ ? world_->city_nodes() : nullptr;
        if (nodes) {
            const world::NodeSite site = nodes->node_at(manager.x, manager.y);
            if (const world::CityNodeInfo* info = nodes->by_rank(site.node_rank)) node = info->guid;
        }
    }
    if (node == 0) {
        spdlog::warn("[session] {}: the Marketplace Manager {} stands in no node: no window", name(), manager.npc.name);
        return;
    }
    if (!room_for(1)) return;
    pb_.market_window = market::MarketWindow{manager.id, node};
    const repl::GuidValue guid = market::marketplace_guid(node);
    const repl::NodeTaxDetails tax = vendor::default_table(node);
    spdlog::info("[session] {}: opens the marketplace of node {} (market {}/{}/{}/{}, {} tax tags)", name(), node, guid.a, guid.b, guid.c, guid.d, tax.entries.size());
    queue("ClientOpenMarketplaceUI", [guid, tax](bootstrap::PlayerBootstrap& pb) { return pb.open_marketplace(guid, tax); }, conn, /*must_build=*/true);
    push_storefront_pages(conn, node);   // the window's tabs learn their market from the last page
}

}  // namespace aoc::worldd

namespace aoc::worldd::bootstrap {

namespace repl = ::aoc::protocol::repl;
namespace schema = ::aoc::protocol::schema;

std::optional<Send> PlayerBootstrap::open_marketplace(const repl::GuidValue& marketplace, const repl::NodeTaxDetails& tax) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::RpcSchema* rpc = pc_.obj.schema->rpc("ClientOpenMarketplaceUI");
    if (!rpc) return std::nullopt;
    repl::RpcParams params;
    if (!repl::set_rpc_param(*rpc, params, "MarketplaceGuid.A", repl::Value::i64(static_cast<int32_t>(marketplace.a))) ||
        !repl::set_rpc_param(*rpc, params, "MarketplaceGuid.B", repl::Value::i64(static_cast<int32_t>(marketplace.b))) ||
        !repl::set_rpc_param(*rpc, params, "MarketplaceGuid.C", repl::Value::i64(static_cast<int32_t>(marketplace.c))) ||
        !repl::set_rpc_param(*rpc, params, "MarketplaceGuid.D", repl::Value::i64(static_cast<int32_t>(marketplace.d))) ||
        !repl::set_rpc_param(*rpc, params, "NodeTaxDetails", repl::Value(tax))) {
        return std::nullopt;
    }
    repl::RpcRequest call;
    call.rpc = "ClientOpenMarketplaceUI";
    call.params = std::move(params);
    return rpcs(Role::PlayerController, "ClientOpenMarketplaceUI", std::span<const repl::RpcRequest>(&call, 1));
}

}  // namespace aoc::worldd::bootstrap
