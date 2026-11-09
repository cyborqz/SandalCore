// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_node_taxes.cpp  (node citizenship: the mayor's taxes and the node's treasury)
//
//  A node's mayor sets its taxes, its shops charge them and its treasury keeps what they took:
//    ServerSetTaxOverride(TaxUpdates: FNodeTaxDetails)    the mayor of TaxUpdates' node sends the rates of its tags; each must lie within the limits its tag has in
//                                                         the node's tax table (CityNodeRecordBase's TaxTable -> NodeGlobalTaxTable -> TaxDetailsRecordBase: the
//                                                         Minimum/MaximumTaxPercentage and FlatTaxAmount); the ones that do are taken, the others refused, and the
//                                                         node's table goes back (ClientSetNodeTaxTable).  A player who is no mayor of it: ClientShowMustBeMayorCallout.
//    the shops                                            a town shop charges its tag's rate in the shopkeeper's node (vendor::TownShops::rate), and what a purchase or a
//                                                         repair in money costs over its untaxed price goes to that node's treasury (world/elections.h)
//    ServerShowMayorInventory(NodeGuid)                   the mayor's view of the treasury: ClientShowMayorInventory(NodeDescription, InventoryItems, InventoryGold)
//    ServerGMSetTaxOverride(NodeName, TaxTag, NewTaxPercent, NewFlatTaxAmount, NewOwningNodeTaxPercent), ServerGMAddNodeGold(NodeGuid, Amount, bIgnoreTaxes)
//                                                         a game master's (gm_rpc_admitted): a rate set past the limits (0 to 100%), gold for the treasury
//  Not built: the node's items (InventoryItems is empty), the owning node's share (NewOwningNodeTaxPercent: all of a tax stays with the node), taxes in Glint or
//  the node's currency (only money pays into the treasury).
// ============================================================================
#include "session/player_session_util.h"

#include "vendor/node_tax.h"
#include "world/citizenship.h"
#include "world/elections.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;
namespace schema = ::aoc::protocol::schema;

namespace {

const repl::Value* param_of(const repl::ClientRpc& rpc, std::string_view path) {
    return rpc.call.decoded ? repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, path) : nullptr;
}

int64_t int_param_of(const repl::ClientRpc& rpc, std::string_view path) {
    const repl::Value* v = param_of(rpc, path);
    if (!v) return 0;
    if (const int64_t* i = v->get_if<int64_t>()) return *i;
    if (const uint64_t* u = v->get_if<uint64_t>()) return static_cast<int64_t>(*u);
    return 0;
}

double number_param_of(const repl::ClientRpc& rpc, std::string_view path) {
    const repl::Value* v = param_of(rpc, path);
    if (!v) return 0;
    if (const float* f = v->get_if<float>()) return *f;
    if (const double* d = v->get_if<double>()) return *d;
    return static_cast<double>(int_param_of(rpc, path));
}

std::string str_param_of(const repl::ClientRpc& rpc, std::string_view path) {
    const repl::Value* v = param_of(rpc, path);
    const std::string* s = v ? v->get_if<std::string>() : nullptr;
    return s ? *s : std::string();
}

constexpr float kTaxSlack = 1e-4f;   // (the client's floats: a rate at its limit may come a rounding past it)

}  // namespace

void PlayerSession::send_node_tax_table(Connection& conn, int64_t node) {
    const world::Elections* el = world_ ? world_->elections() : nullptr;
    auto table = std::make_shared<repl::NodeTaxDetails>(vendor::node_table(node, el ? el->taxes(node) : nullptr));
    queue("ClientSetNodeTaxTable", [table](bootstrap::PlayerBootstrap& pb) { return pb.node_tax_table(*table); }, conn, /*must_build=*/true);
}

void PlayerSession::on_set_tax_override(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!within_budget(citizenship_budget_, "ServerSetTaxOverride", now)) return;
    world::Elections* el = world_ ? world_->elections() : nullptr;
    const world::CitizenshipRules* rules = world_ ? world_->citizenship_rules() : nullptr;
    const repl::Value* v = param_of(rpc, "TaxUpdates");
    const repl::NodeTaxDetails* updates = v ? v->get_if<repl::NodeTaxDetails>() : nullptr;
    if (!el || !rules || !updates) {
        ++tax_overrides_refused_;
        spdlog::log(level_for(citizenship_log_), "[nodes] {}: ServerSetTaxOverride is refused: {}", name(), !updates ? "its taxes do not read" : "this world has no node taxes");
        return;
    }
    const int64_t node = updates->node;
    if (node == 0 || el->mayor_node(account_id_) != node) {
        ++tax_overrides_refused_;
        spdlog::log(level_for(citizenship_log_), "[nodes] {}: ServerSetTaxOverride for node {} is refused: the account is not its mayor", name(), node);
        send_must_be_mayor(conn);
        return;
    }
    const auto* current = el->taxes(node);
    size_t taken = 0, refused = 0;
    std::string first_refusal;
    for (const repl::NodeTaxEntry& e : updates->entries) {
        const world::TaxLimit* limit = rules->tax_limit(node, e.tag);
        std::string why;
        if (!limit) why = "the node's tax table has no " + e.tag;
        else if (!(e.tax_percent >= limit->min_percent - kTaxSlack && e.tax_percent <= limit->max_percent + kTaxSlack))
            why = e.tag + " at " + std::to_string(e.tax_percent) + " is outside " + std::to_string(limit->min_percent) + " to " + std::to_string(limit->max_percent);
        else if (e.flat_tax < limit->min_flat || e.flat_tax > limit->max_flat)
            why = e.tag + "'s flat amount " + std::to_string(e.flat_tax) + " is outside " + std::to_string(limit->min_flat) + " to " + std::to_string(limit->max_flat);
        if (!why.empty()) {
            ++refused;
            if (first_refusal.empty()) first_refusal = why;
            continue;
        }
        const world::TaxOverride rate{std::clamp(e.tax_percent, limit->min_percent, limit->max_percent), e.flat_tax};
        if (current) {
            const auto it = current->find(e.tag);
            if (it != current->end() && it->second == rate) continue;   // (unchanged)
        }
        el->set_tax(node, e.tag, rate);
        current = el->taxes(node);
        ++taken;
    }
    tax_overrides_taken_ += static_cast<uint32_t>(taken);
    tax_overrides_refused_ += static_cast<uint32_t>(refused);
    if (taken > 0) {
        el->add_news(node, world::NodeNews{10, wall_ms(), name()});   // ENodeSystemNotificationType::Taxes: the mayor changed the node's taxes
        world_->save_elections();
    }
    spdlog::info("[nodes] {}: the mayor sets node {}'s taxes: {} rate(s) taken, {} refused{}", name(), node, taken, refused, first_refusal.empty() ? "" : " (" + first_refusal + ")");
    send_node_tax_table(conn, node);
}

void PlayerSession::on_gm_set_tax_override(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const std::string node_name = str_param_of(rpc, "NodeName"), tag = str_param_of(rpc, "TaxTag");
    const double percent = number_param_of(rpc, "NewTaxPercent");
    const int64_t flat = int_param_of(rpc, "NewFlatTaxAmount");
    if (!gm_rpc_admitted("ServerGMSetTaxOverride", "NodeName=\"" + node_name.substr(0, 100) + "\" TaxTag=\"" + tag.substr(0, 100) + "\" NewTaxPercent=" + std::to_string(percent) +
                             " NewFlatTaxAmount=" + std::to_string(flat),
                         now) ||
        !rpc.call.decoded) {
        return;
    }
    world::Elections* el = world_ ? world_->elections() : nullptr;
    std::string why = "this world has no node taxes";
    const int64_t node = el ? gm_node_named(node_name, why) : 0;
    if (node == 0 || tag.rfind("NodeTax", 0) != 0) {
        spdlog::warn("[gm] {}: ServerGMSetTaxOverride does nothing: {}", name(), node == 0 ? why : "\"" + tag.substr(0, 100) + "\" is no NodeTax tag");
        return;
    }
    el->set_tax(node, tag, world::TaxOverride{static_cast<float>(std::clamp(percent, 0.0, 1.0)), static_cast<int32_t>(std::clamp<int64_t>(flat, 0, 1000000))});
    world_->save_elections();
    spdlog::info("[gm] {}: ServerGMSetTaxOverride: node {}'s {} is {:.4f} and {} flat", name(), node, tag, std::clamp(percent, 0.0, 1.0), flat);
    send_node_tax_table(conn, node);
}

void PlayerSession::on_show_mayor_inventory(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!within_budget(citizenship_budget_, "ServerShowMayorInventory", now)) return;
    world::Elections* el = world_ ? world_->elections() : nullptr;
    const int64_t node = int_param_of(rpc, "NodeGuid");
    if (!el || node == 0 || el->mayor_node(account_id_) != node) {
        spdlog::log(level_for(citizenship_log_), "[nodes] {}: ServerShowMayorInventory for node {} is not answered: the account is not its mayor", name(), node);
        if (el) send_must_be_mayor(conn);
        return;
    }
    const world::CityNodeInfo* info = world_->city_nodes() ? world_->city_nodes()->by_guid(node) : nullptr;
    const std::string description = info ? info->display_name : std::string();
    const int64_t gold = el->treasury(node);
    send_citizenship_call(conn, "ClientShowMayorInventory", [description, gold](const schema::RpcSchema& s, repl::RpcParams& p) {
        return repl::set_rpc_param(s, p, "NodeDescription", repl::Value::str(description)) && repl::set_rpc_param(s, p, "InventoryItems", repl::Value(repl::ArrayValue{})) &&
               repl::set_rpc_param(s, p, "InventoryGold", repl::Value::i64(gold));
    });
}

void PlayerSession::on_gm_add_node_gold(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const int64_t node = int_param_of(rpc, "NodeGuid"), amount = int_param_of(rpc, "Amount");
    if (!gm_rpc_admitted("ServerGMAddNodeGold", "NodeGuid=" + std::to_string(node) + " Amount=" + std::to_string(amount), now) || !rpc.call.decoded) return;
    (void)conn;
    world::Elections* el = world_ ? world_->elections() : nullptr;
    const world::CityNodes* nodes = world_ ? world_->city_nodes() : nullptr;
    if (!el || !nodes || !nodes->by_guid(node)) {
        spdlog::warn("[gm] {}: ServerGMAddNodeGold does nothing: node {} is no node of this world", name(), node);
        return;
    }
    el->add_treasury(node, amount);
    world_->save_elections();
    spdlog::info("[gm] {}: ServerGMAddNodeGold: node {}'s treasury holds {} now", name(), node, el->treasury(node));
}

/// The client calls of the mayor's taxes and the node's treasury (session/rpc_table.h).
void PlayerSession::register_node_taxes_rpcs(RpcTable& t) {
    t.add(RpcScope::PlayerController, "ServerSetTaxOverride", [](PlayerSession& s, const RpcContext& c) { s.on_set_tax_override(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerGMSetTaxOverride", [](PlayerSession& s, const RpcContext& c) { s.on_gm_set_tax_override(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerShowMayorInventory", [](PlayerSession& s, const RpcContext& c) { s.on_show_mayor_inventory(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerGMAddNodeGold", [](PlayerSession& s, const RpcContext& c) { s.on_gm_add_node_gold(c.conn, c.rpc, c.now); });
}

}}  // namespace aoc::worldd
