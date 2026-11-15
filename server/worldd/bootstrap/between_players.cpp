// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  bootstrap/between_players.cpp: the PlayerBootstrap builders of what one player does to another (declared in bootstrap/bootstrap.h)
//
//  Abilities between players: ClientPromptResurrect, the alert a dead player is asked with when another player's Resurrect lands (session/player_resurrect.cpp).
// ============================================================================
#include "bootstrap/bootstrap.h"

#include "protocol/repl/rpc.h"

#include <span>

namespace aoc::worldd::bootstrap {

using repl::Value;
namespace schema = ::aoc::protocol::schema;

std::optional<Send> PlayerBootstrap::prompt_resurrect(const std::array<uint32_t, 4>& by) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::RpcSchema* rpc = pc_.obj.schema->rpc("ClientPromptResurrect");
    if (!rpc) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = "ClientPromptResurrect";
    if (!repl::set_rpc_param(*rpc, call.params, "InstigatorGuid.A", Value::i64(static_cast<int32_t>(by[0]))) ||
        !repl::set_rpc_param(*rpc, call.params, "InstigatorGuid.B", Value::i64(static_cast<int32_t>(by[1]))) ||
        !repl::set_rpc_param(*rpc, call.params, "InstigatorGuid.C", Value::i64(static_cast<int32_t>(by[2]))) ||
        !repl::set_rpc_param(*rpc, call.params, "InstigatorGuid.D", Value::i64(static_cast<int32_t>(by[3]))))
        return std::nullopt;
    return rpcs(Role::PlayerController, "ClientPromptResurrect", std::span<const repl::RpcRequest>(&call, 1));
}

}  // namespace aoc::worldd::bootstrap
