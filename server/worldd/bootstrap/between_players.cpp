// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  bootstrap/between_players.cpp: the PlayerBootstrap builders of what one player does to another (declared in bootstrap/bootstrap.h)
//
//  Abilities between players: ClientPromptResurrect, the alert a dead player is asked with when another player's Resurrect lands (session/player_resurrect.cpp).
//  PvP (session/player_pvp.cpp): the pawn's Alignments and bInPVPCombat, the account's PvP settings, and the flag's and the corruption's chat notices.
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

// ---- PvP (session/player_pvp.cpp) ---------------------------------------------------------------------------------------------------

bool PlayerBootstrap::set_in_pvp_combat(bool on) {
    if (!ok_) return false;
    repl::Subobject* combat = component(setup_.guids.combat_info);
    if (!combat || !combat->obj.schema->command("bInPVPCombat")) return false;
    return combat->obj.state.set(*combat->obj.schema, "bInPVPCombat", Value::boolean(on));
}

bool PlayerBootstrap::set_alignments(const repl::AlignmentContainer& alignments) {
    if (!ok_ || !second_state_added_) return false;
    repl::Subobject* alignment = component(setup_.guids.alignment);
    if (!alignment || !alignment->obj.schema->command("Alignments")) return false;
    return alignment->obj.state.set(*alignment->obj.schema, "Alignments", Value(alignments));
}

bool PlayerBootstrap::controller_bool(std::string_view path, bool fallback) const {
    if (!pc_.obj.schema) return fallback;
    const Value* v = pc_.obj.state.get(*pc_.obj.schema, path);
    const bool* b = v ? v->get_if<bool>() : nullptr;
    return b ? *b : fallback;
}

std::optional<Send> PlayerBootstrap::notify_flagging_reason(uint8_t intention, const emit::FIntrepidNetworkGUID& target) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::RpcSchema* rpc = pc_.obj.schema->rpc("NotifyFlaggingReason");
    if (!rpc) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = "NotifyFlaggingReason";
    if (!repl::set_rpc_param(*rpc, call.params, "Intention", Value::u64(intention)) || !repl::set_rpc_param(*rpc, call.params, "Target", Value(target))) return std::nullopt;
    return rpcs(Role::PlayerController, "NotifyFlaggingReason", std::span<const repl::RpcRequest>(&call, 1));
}

std::optional<Send> PlayerBootstrap::notify_corruption_for_kill(float delta, const emit::FIntrepidNetworkGUID& target) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::RpcSchema* rpc = pc_.obj.schema->rpc("NotifyCorruptionDeltaForKill");
    if (!rpc) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = "NotifyCorruptionDeltaForKill";
    if (!repl::set_rpc_param(*rpc, call.params, "CorruptionDelta", Value::f32(delta)) || !repl::set_rpc_param(*rpc, call.params, "Target", Value(target))) return std::nullopt;
    return rpcs(Role::PlayerController, "NotifyCorruptionDeltaForKill", std::span<const repl::RpcRequest>(&call, 1));
}

std::optional<Send> PlayerBootstrap::notify_corruption_lost_for_dying(float delta) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::RpcSchema* rpc = pc_.obj.schema->rpc("NotifyCorruptionLostForDying");
    if (!rpc) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = "NotifyCorruptionLostForDying";
    if (!repl::set_rpc_param(*rpc, call.params, "CorruptionDelta", Value::f32(delta))) return std::nullopt;
    return rpcs(Role::PlayerController, "NotifyCorruptionLostForDying", std::span<const repl::RpcRequest>(&call, 1));
}

}  // namespace aoc::worldd::bootstrap
