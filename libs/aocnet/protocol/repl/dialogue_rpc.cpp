// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/dialogue_rpc.cpp
//
//  See dialogue_rpc.h.
// ============================================================================
#include "protocol/repl/dialogue_rpc.h"

#include "protocol/repl/rpc_fields.h"

#include <string_view>

namespace aoc { namespace protocol { namespace repl {

namespace {

bool is_rpc(const schema::RpcSchema& rpc, std::string_view name) { return rpc.name && name == rpc.name; }

bool decoded_as(const RpcCall& call, std::string_view name) { return call.rpc && call.decoded && is_rpc(*call.rpc, name); }

}  // namespace

std::optional<DialogueResponse> parse_dialogue_response(const RpcCall& call) {
    if (!decoded_as(call, "ServerSelectDialogueChoice")) return std::nullopt;
    const schema::RpcSchema& rpc = *call.rpc;
    DialogueResponse r;
    const auto type = rpcf::uint_or(rpcf::value_of(rpc, call.params, "Response.ResponseType"), 0);
    const auto quest = rpcf::uint_or(rpcf::value_of(rpc, call.params, "Response.QuestResponse"), 0);
    const auto node = rpcf::int_or(rpcf::value_of(rpc, call.params, "Response.Choice.SourceNodeId.Guid"), 0);
    const auto node_type = rpcf::int_or(rpcf::value_of(rpc, call.params, "Response.Choice.SourceNodeId.TypeId"), 0);
    const auto index = rpcf::int_or(rpcf::value_of(rpc, call.params, "Response.Choice.ChoiceIndex"), 0);
    const auto choice_type = rpcf::uint_or(rpcf::value_of(rpc, call.params, "Response.Choice.ChoiceType"), 0);
    const auto window = rpcf::uint_or(rpcf::value_of(rpc, call.params, "Response.WindowType"), 0);
    if (!type || !quest || !node || !node_type || !index || !choice_type || !window) return std::nullopt;
    if (*type > UINT8_MAX || *quest > UINT8_MAX || *choice_type > UINT8_MAX || *window > UINT8_MAX || *index < INT32_MIN || *index > INT32_MAX) return std::nullopt;
    r.response_type = static_cast<uint8_t>(*type);
    r.quest_response = static_cast<uint8_t>(*quest);
    r.node = *node;
    r.node_type_id = *node_type;
    r.choice_index = static_cast<int32_t>(*index);
    r.choice_type = static_cast<uint8_t>(*choice_type);
    r.window_type = static_cast<uint8_t>(*window);
    return r;
}

std::optional<RpcParams> dialogue_response_params(const schema::RpcSchema& rpc, const DialogueResponse& r) {
    if (!is_rpc(rpc, "ServerSelectDialogueChoice")) return std::nullopt;
    RpcParams params;
    if (!set_rpc_param(rpc, params, "Response.ResponseType", Value::u64(r.response_type))) return std::nullopt;
    if (!set_rpc_param(rpc, params, "Response.QuestResponse", Value::u64(r.quest_response))) return std::nullopt;
    if (!set_rpc_param(rpc, params, "Response.Choice.SourceNodeId.Guid", Value::i64(r.node))) return std::nullopt;
    if (!set_rpc_param(rpc, params, "Response.Choice.SourceNodeId.TypeId", Value::i64(r.node_type_id))) return std::nullopt;
    if (!set_rpc_param(rpc, params, "Response.Choice.SourceNodeId.Name", Value(NameValue::hardcoded_name(0)))) return std::nullopt;
    if (!set_rpc_param(rpc, params, "Response.Choice.ChoiceIndex", Value::i64(r.choice_index))) return std::nullopt;
    if (!set_rpc_param(rpc, params, "Response.Choice.ChoiceType", Value::u64(r.choice_type))) return std::nullopt;
    if (!set_rpc_param(rpc, params, "Response.WindowType", Value::u64(r.window_type))) return std::nullopt;
    return params;
}

}}}  // namespace aoc::protocol::repl
