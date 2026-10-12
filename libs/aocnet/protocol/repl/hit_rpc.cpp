// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/hit_rpc.cpp
//
//  See hit_rpc.h.
// ============================================================================
#include "protocol/repl/hit_rpc.h"

#include "protocol/repl/rpc_fields.h"

namespace aoc { namespace protocol { namespace repl {

namespace {

using schema::RepCommand;
using schema::RpcSchema;

constexpr std::string_view kRequestRpc = "RequestHitVerification_Server";

bool is_request_rpc(const RpcSchema& rpc) {
    return rpc.name && kRequestRpc == rpc.name && rpc_command(rpc, "HitRequest.HitComponents") &&
           rpc_command(rpc, "HitRequest.ActorLocation");
}

bool is_confirmations_rpc(const RpcSchema& rpc) {
    return rpc.name && (std::string_view(rpc.name) == "ClientHitConfirmationsReliable" ||
                        std::string_view(rpc.name) == "ClientHitConfirmations") &&
           rpc_command(rpc, "Hits");
}

}  // namespace

std::optional<HitRequest> parse_hit_request(const RpcCall& call) {
    if (!call.rpc || !call.decoded || !is_request_rpc(*call.rpc)) return std::nullopt;
    const RpcSchema& rpc = *call.rpc;
    HitRequest req;
    const auto ability = rpcf::int_or(rpcf::value_of(rpc, call.params, "HitRequest.AbilityGuid"), 0);
    const auto targeting = rpcf::int_or(rpcf::value_of(rpc, call.params, "HitRequest.TargetingGuid"), 0);
    const auto hit = rpcf::int_or(rpcf::value_of(rpc, call.params, "HitRequest.HitGuid"), 0);
    const auto yaw = rpcf::uint_or(rpcf::value_of(rpc, call.params, "HitRequest.ActorYawAndPitch"), 0);
    if (!ability || !targeting || !hit || !yaw || *yaw > UINT32_MAX) return std::nullopt;
    req.ability_guid = *ability;
    req.targeting_guid = *targeting;
    req.hit_guid = *hit;
    req.actor_yaw_and_pitch = static_cast<uint32_t>(*yaw);
    if (const Value* v = rpcf::value_of(rpc, call.params, "HitRequest.ActorLocation")) {
        const QuantVec* q = v->get_if<QuantVec>();
        if (!q) return std::nullopt;
        req.actor_location = *q;
    }
    if (const Value* v = rpcf::value_of(rpc, call.params, "HitRequest.HitComponents")) {
        const ArrayValue* a = v->get_if<ArrayValue>();
        if (!a || a->elements.size() > kMaxHitRequestComponents) return std::nullopt;
        const RepCommand* cmd = rpc_command(rpc, "HitRequest.HitComponents");
        const auto elems = rpcf::elements_of(rpc, *cmd);
        const uint16_t h_object = rpcf::local(elems, "HitComponents.HitComponent");
        const uint16_t h_time = rpcf::local(elems, "HitComponents.ServerTimestamp");
        if (!h_object || !h_time) return std::nullopt;
        for (const ElementState& e : a->elements) {
            HitComponentRef c;
            const Value* o = rpcf::element_value(e, h_object);
            if (const auto* g = rpcf::as<emit::FIntrepidNetworkGUID>(o)) c.object = *g;
            else if (const auto* inl = rpcf::as<InlineObject>(o)) c.inline_object = *inl;
            else if (o) return std::nullopt;
            const Value* t = rpcf::element_value(e, h_time);
            if (t) {
                const float* f = t->get_if<float>();
                if (!f) return std::nullopt;
                c.server_timestamp = *f;
            }
            req.components.push_back(std::move(c));
        }
    }
    return req;
}
// C.Y.B captures, C | O | A code

std::optional<RpcParams> hit_request_params(const RpcSchema& rpc, const HitRequest& req) {
    if (!is_request_rpc(rpc) || req.components.size() > kMaxHitRequestComponents) return std::nullopt;
    RpcParams params;
    bool ok = set_rpc_param(rpc, params, "HitRequest.AbilityGuid", Value::i64(req.ability_guid));
    ok = ok && set_rpc_param(rpc, params, "HitRequest.TargetingGuid", Value::i64(req.targeting_guid));
    ok = ok && set_rpc_param(rpc, params, "HitRequest.HitGuid", Value::i64(req.hit_guid));
    ok = ok && set_rpc_param(rpc, params, "HitRequest.ActorLocation", Value(req.actor_location));
    ok = ok && set_rpc_param(rpc, params, "HitRequest.ActorYawAndPitch", Value::u64(req.actor_yaw_and_pitch));
    if (!ok) return std::nullopt;
    const RepCommand* cmd = rpc_command(rpc, "HitRequest.HitComponents");
    const auto elems = rpcf::elements_of(rpc, *cmd);
    const uint16_t h_object = rpcf::local(elems, "HitComponents.HitComponent");
    const uint16_t h_time = rpcf::local(elems, "HitComponents.ServerTimestamp");
    if (!h_object || !h_time) return std::nullopt;
    ArrayValue components;
    for (const HitComponentRef& c : req.components) {
        ElementState e;
        e.values[h_object] = c.inline_object ? Value(*c.inline_object) : Value(c.object);
        e.values[h_time] = Value::f32(c.server_timestamp);
        components.elements.push_back(std::move(e));
    }
    if (!set_rpc_param(rpc, params, "HitRequest.HitComponents", Value(std::move(components)))) return std::nullopt;
    return params;
}

std::optional<HitConfirmations> parse_hit_confirmations(const RpcCall& call) {
    if (!call.rpc || !call.decoded || !is_confirmations_rpc(*call.rpc)) return std::nullopt;
    const Value* v = rpcf::value_of(*call.rpc, call.params, "Hits");
    const HitConfirmations* c = rpcf::as<HitConfirmations>(v);
    if (!c) return std::nullopt;
    return *c;
}

std::optional<RpcParams> hit_confirmations_params(const RpcSchema& rpc, const HitConfirmations& hits) {
    if (!is_confirmations_rpc(rpc) || !hit_confirmations_fit(hits)) return std::nullopt;
    RpcParams params;
    if (!set_rpc_param(rpc, params, "Hits", Value(hits))) return std::nullopt;
    return params;
}

std::optional<Emission> client_hit_confirmations(const Actor& pc, const HitConfirmations& hits, bool reliable,
                                                 const ExportAcks& acks, std::span<const ExportChain> extra_exports) {
    const char* name = reliable ? "ClientHitConfirmationsReliable" : "ClientHitConfirmations";
    const RpcSchema* rpc = pc.obj.schema ? pc.obj.schema->rpc(name) : nullptr;
    if (!rpc) return std::nullopt;
    const auto params = hit_confirmations_params(*rpc, hits);
    if (!params) return std::nullopt;
    return call_rpc(pc, nullptr, name, *params, extra_exports, acks);
}

}}}  // namespace aoc::protocol::repl
