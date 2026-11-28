// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  bootstrap/bootstrap_trade.cpp
//
//  PlayerBootstrap builders for player-to-player trade RPCs and bTradeInProgress
//  replication on the owner's pawn and observer pawns.
// ============================================================================
#include "bootstrap/bootstrap.h"

#include "protocol/repl/rpc.h"

#include <span>

namespace aoc::worldd::bootstrap {

using repl::Value;
namespace schema = ::aoc::protocol::schema;

std::optional<Send> PlayerBootstrap::trade_request(const repl::GuidValue& from) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::RpcSchema* rpc = pc_.obj.schema ? pc_.obj.schema->rpc("ClientRequestTrade") : nullptr;
    if (!rpc) return std::nullopt;
    const auto params = repl::client_request_trade_params(*rpc, from);
    if (!params) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = "ClientRequestTrade";
    call.params = *params;
    return rpcs(Role::PlayerController, "ClientRequestTrade", std::span<const repl::RpcRequest>(&call, 1));
}

std::optional<Send> PlayerBootstrap::trade_request_response(const repl::GuidValue& target, uint8_t response) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::RpcSchema* rpc = pc_.obj.schema ? pc_.obj.schema->rpc("ClientProcessTradeRequestResponse") : nullptr;
    if (!rpc) return std::nullopt;
    const auto params = repl::client_trade_response_params(*rpc, target, response);
    if (!params) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = "ClientProcessTradeRequestResponse";
    call.params = *params;
    return rpcs(Role::PlayerController, "ClientProcessTradeRequestResponse", std::span<const repl::RpcRequest>(&call, 1));
}

std::optional<Send> PlayerBootstrap::trade_begin(const repl::GuidValue& trade_guid, const repl::GuidValue& partner) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::RpcSchema* rpc = pc_.obj.schema ? pc_.obj.schema->rpc("ClientBeginTrade") : nullptr;
    if (!rpc) return std::nullopt;
    const auto params = repl::client_begin_trade_params(*rpc, trade_guid, partner);
    if (!params) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = "ClientBeginTrade";
    call.params = *params;
    return rpcs(Role::PlayerController, "ClientBeginTrade", std::span<const repl::RpcRequest>(&call, 1));
}

std::optional<Send> PlayerBootstrap::trade_update(const repl::GuidValue& owner, const repl::TradeOffer& offer) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::RpcSchema* rpc = pc_.obj.schema ? pc_.obj.schema->rpc("ClientUpdateTrade") : nullptr;
    if (!rpc) return std::nullopt;
    const auto params = repl::client_update_trade_params(*rpc, owner, offer);
    if (!params) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = "ClientUpdateTrade";
    call.params = *params;
    return rpcs(Role::PlayerController, "ClientUpdateTrade", std::span<const repl::RpcRequest>(&call, 1));
}

std::optional<Send> PlayerBootstrap::trade_lock(const repl::GuidValue& character) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::RpcSchema* rpc = pc_.obj.schema ? pc_.obj.schema->rpc("ClientLockTrade") : nullptr;
    if (!rpc) return std::nullopt;
    const auto params = repl::client_lock_trade_params(*rpc, character);
    if (!params) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = "ClientLockTrade";
    call.params = *params;
    return rpcs(Role::PlayerController, "ClientLockTrade", std::span<const repl::RpcRequest>(&call, 1));
}

std::optional<Send> PlayerBootstrap::trade_unlock(const repl::GuidValue& character) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::RpcSchema* rpc = pc_.obj.schema ? pc_.obj.schema->rpc("ClientUnlockTrade") : nullptr;
    if (!rpc) return std::nullopt;
    const auto params = repl::client_unlock_trade_params(*rpc, character);
    if (!params) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = "ClientUnlockTrade";
    call.params = *params;
    return rpcs(Role::PlayerController, "ClientUnlockTrade", std::span<const repl::RpcRequest>(&call, 1));
}

std::optional<Send> PlayerBootstrap::trade_confirmed(const repl::GuidValue& character) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::RpcSchema* rpc = pc_.obj.schema ? pc_.obj.schema->rpc("ClientTradeConfirmed") : nullptr;
    if (!rpc) return std::nullopt;
    const auto params = repl::client_trade_confirmed_params(*rpc, character);
    if (!params) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = "ClientTradeConfirmed";
    call.params = *params;
    return rpcs(Role::PlayerController, "ClientTradeConfirmed", std::span<const repl::RpcRequest>(&call, 1));
}

std::optional<Send> PlayerBootstrap::trade_close(uint8_t status) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::RpcSchema* rpc = pc_.obj.schema ? pc_.obj.schema->rpc("ClientCloseTrade") : nullptr;
    if (!rpc) return std::nullopt;
    const auto params = repl::client_close_trade_params(*rpc, status);
    if (!params) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = "ClientCloseTrade";
    call.params = *params;
    return rpcs(Role::PlayerController, "ClientCloseTrade", std::span<const repl::RpcRequest>(&call, 1));
}

bool PlayerBootstrap::set_trade_in_progress(bool in_progress) {
    if (!ok_ || !second_state_added_) return false;
    repl::Subobject* base = component(setup_.guids.base_character_info);
    if (!base || !base->obj.schema || !base->obj.schema->command("bTradeInProgress")) return false;
    return base->obj.state.set(*base->obj.schema, "bTradeInProgress", Value::boolean(in_progress));
}

bool PlayerBootstrap::set_observer_trade_in_progress(repl::Actor& shown, bool in_progress) {
    if (!ok_ || !second_state_added_) return false;
    repl::Subobject* base = shown.find(setup_.guids.base_character_info);
    if (!base || !base->obj.schema || !base->obj.schema->command("bTradeInProgress")) return false;
    return base->obj.state.set(*base->obj.schema, "bTradeInProgress", Value::boolean(in_progress));
}

}  // namespace aoc::worldd::bootstrap
