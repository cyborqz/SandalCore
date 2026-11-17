// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// Abilities between players: contributed by MrJambix in #23.
// ============================================================================
//  bootstrap/between_players.cpp: the PlayerBootstrap builders of what one player does to another (declared in bootstrap/bootstrap.h)
//
//  Abilities between players: the PvP flag's Alignments and bInPVPCombat of the pawn, the account's PvP settings echoed on the PlayerController, and
//  the flag's and the corruption's chat notices (session/player_pvp.cpp).
// ============================================================================
#include "bootstrap/bootstrap.h"

#include "combat/pvp.h"
#include "protocol/repl/rpc.h"

#include <span>

namespace aoc::worldd::bootstrap {

using repl::Value;
namespace schema = ::aoc::protocol::schema;

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
    // the flag's instance and its timer (and a duel's) change; what the pawn holds for any other alignment (a party's, a node's) stays
    const repl::Value* held = alignment->obj.state.get(*alignment->obj.schema, "Alignments");
    const auto* now = held ? held->get_if<repl::AlignmentContainer>() : nullptr;
    if (!now) return false;
    const repl::AlignmentContainer next = combat::pvp::merge_container(*now, alignments);
    return alignment->obj.state.set(*alignment->obj.schema, "Alignments", Value(next));
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
