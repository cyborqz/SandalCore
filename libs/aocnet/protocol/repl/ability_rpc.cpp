// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/ability_rpc.cpp
//
//  See ability_rpc.h.
// ============================================================================
#include "protocol/repl/ability_rpc.h"

namespace aoc { namespace protocol { namespace repl {

namespace {

/// The value of command `path` of the call, nullptr when the parameter was left at its default (a 0 send bit).
const Value* param(const RpcCall& call, std::string_view path) {
    const schema::RepCommand* c = rpc_command(*call.rpc, path);
    if (!c) return nullptr;
    const auto it = call.params.values.find(c->handle);
    return it == call.params.values.end() ? nullptr : &it->second;
}

int64_t int_param(const RpcCall& call, std::string_view path, int64_t default_value = 0) {
    const Value* v = param(call, path);
    const int64_t* i = v ? v->get_if<int64_t>() : nullptr;
    return i ? *i : default_value;
}

uint32_t count_param(const RpcCall& call, std::string_view path) {
    const Value* v = param(call, path);
    const ArrayValue* a = v ? v->get_if<ArrayValue>() : nullptr;
    return a ? static_cast<uint32_t>(a->elements.size()) : 0;
}

bool is_call(const RpcCall& call, std::string_view name) {
    return call.rpc && call.decoded && name == call.rpc->name;
}

}  // namespace

std::optional<TriggerAbility> parse_trigger_ability(const RpcCall& call) {
    if (!is_call(call, "ServerTriggerAbility")) return std::nullopt;
    TriggerAbility t;
    t.ability_guid = int_param(call, "AbilityGuid");
    t.client_instance_id = static_cast<int32_t>(int_param(call, "ClientInstanceId"));
    t.animation_index = static_cast<int32_t>(int_param(call, "AnimationIndex"));
    if (const Value* v = param(call, "InputVector")) {
        const Vec2d* in = v->get_if<Vec2d>();
        if (!in) return std::nullopt;
        t.input_x = in->x;
        t.input_y = in->y;
    }
    if (const Value* v = param(call, "bPvpForceFlag")) {
        const bool* b = v->get_if<bool>();
        if (!b) return std::nullopt;
        t.pvp_force = *b;
    }
    t.target_count = count_param(call, "Targets");
    if (const Value* v = param(call, "Targets")) {
        if (const ArrayValue* a = v->get_if<ArrayValue>()) {
            for (const ElementState& e : a->elements) {
                for (const auto& [handle, value] : e.values) {
                    (void)handle;
                    if (const auto* g = value.get_if<emit::FIntrepidNetworkGUID>()) t.targets.push_back(*g);
                    else if (const auto* o = value.get_if<InlineObject>()) t.inline_targets.push_back(*o);
                }
            }
        }
    }
    t.target_transform_count = count_param(call, "TargetTransforms");
    t.item_slot = static_cast<int32_t>(int_param(call, "ItemData.Slot", -1));
    if (const Value* v = param(call, "ItemData.Location")) {
        const uint64_t* u = v->get_if<uint64_t>();
        if (!u) return std::nullopt;
        t.item_location = static_cast<uint32_t>(*u);
    }
    return t;
}
// ~ CoA - C-Y-B ~

std::optional<TriggerPrefix> read_trigger_prefix(const uint8_t* data, size_t data_bytes, size_t pos, size_t end) {
    if (!data || end > data_bytes * 8 || pos > end) return std::nullopt;
    wire::PacketReader r(data, data_bytes, end);
    r.set_pos(pos);
    TriggerPrefix p;
    if (r.read_bit()) p.ability_guid = static_cast<int64_t>(r.read_uint64());
    if (r.read_bit()) p.client_instance_id = static_cast<int32_t>(r.read_uint32());
    if (overran(r)) return std::nullopt;
    return p;
}

std::optional<HeldAbilityCancelled> parse_held_ability_cancelled(const RpcCall& call) {
    if (!is_call(call, "ServerNotifyHeldAbilityCancelled")) return std::nullopt;
    HeldAbilityCancelled c;
    c.client_instance_id = static_cast<int32_t>(int_param(call, "ClientInstanceId"));
    if (const Value* v = param(call, "CancelledTime")) {
        const float* f = v->get_if<float>();
        if (!f) return std::nullopt;
        c.cancelled_time = *f;
    }
    c.cancel_section_index = static_cast<int32_t>(int_param(call, "CancelSectionIndex"));
    return c;
}

std::optional<int32_t> parse_end_ability(const RpcCall& call) {
    if (!is_call(call, "ServerEndAbility")) return std::nullopt;
    return static_cast<int32_t>(int_param(call, "ServerInstanceId"));
}

std::optional<Emission> client_end_ability(const Actor& pawn, const Subobject& ability, int32_t client_instance_id,
                                           const ExportAcks& acks) {
    const schema::RpcSchema* rpc = ability.obj.schema ? ability.obj.schema->rpc("ClientEndAbility") : nullptr;
    if (!rpc) return std::nullopt;
    RpcParams params;
    if (!set_rpc_param(*rpc, params, "ClientInstanceId", Value::i64(client_instance_id))) return std::nullopt;
    return call_rpc(pawn, &ability, "ClientEndAbility", params, {}, acks);
}

}}}  // namespace aoc::protocol::repl
