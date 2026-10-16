// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  bootstrap/gather_wire.cpp  (Gathering stage 3, part "Foliage gathering")
//
//  What a harvest of a painted tree or plant says to the client besides the items: the pawn's UPlayerInfo.FoliageData (the bar and the animation), the PlayerController's
//  ClientConfirmMultiInteractFoliage (done) and ClientInterruptInteractionWithFoliage (ended by the server).  All three are written from the retail captures
//  (libs/aocnet/tests/capture_foliage.h): see bootstrap.h.
// ============================================================================
#include "bootstrap/bootstrap.h"

#include "protocol/repl/fname.h"
#include "protocol/schema/generated/index.h"

namespace aoc::worldd::bootstrap {

using repl::Value;

namespace {

repl::NameValue name_of(const std::string& s) { return s.empty() ? repl::NameValue::hardcoded_name(0) : repl::fname_parse(s); }   // EName 0 is None

}  // namespace

std::optional<Send> PlayerBootstrap::pawn_foliage_data(const FoliageChannel& d) {
    if (!ok_ || !second_state_sent_) return std::nullopt;
    repl::Subobject* info = nullptr;
    for (repl::Subobject& s : pawn_.subobjects) {
        if (s.name == "BaseCharacterInfo") info = &s;
    }
    if (!info || !info->obj.schema || !info->obj.schema->command("FoliageData.Duration")) return std::nullopt;
    repl::ObjectState& o = info->obj;
    const auto set = [&](const char* path, Value v) { return o.state.set(*o.schema, path, std::move(v)); };
    const bool ok = set("FoliageData.Duration", Value::f32(d.duration)) && set("FoliageData.ServerStartTime", Value::f64(d.start_time)) &&
                    set("FoliageData.OriginalInstanceIndex", Value::i64(static_cast<int32_t>(d.instance))) && set("FoliageData.FoliageName", Value(name_of(d.owner))) &&
                    set("FoliageData.FoliageComponentUUID", Value(name_of(d.component))) && set("FoliageData.InteractionIndex", Value::u64(d.interaction_index)) &&
                    set("FoliageData.FoliageState", Value::u64(d.state)) && set("FoliageData.ToolRecordId", Value::i64(d.tool_record)) &&
                    set("FoliageData.ToolItemInstanceGuid.A", Value::i64(static_cast<int32_t>(d.tool_guid[0]))) &&
                    set("FoliageData.ToolItemInstanceGuid.B", Value::i64(static_cast<int32_t>(d.tool_guid[1]))) &&
                    set("FoliageData.ToolItemInstanceGuid.C", Value::i64(static_cast<int32_t>(d.tool_guid[2]))) &&
                    set("FoliageData.ToolItemInstanceGuid.D", Value::i64(static_cast<int32_t>(d.tool_guid[3])));
    if (!ok) return std::nullopt;
    return update(Role::Pawn, "the pawn's foliage data", pawn_, pawn_shadow_);
}
// ~ C.O.A & C.Y.B ~

std::optional<Send> PlayerBootstrap::foliage_confirm() {
    if (!ok_ || !second_state_added_) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = "ClientConfirmMultiInteractFoliage";
    if (!pc_.obj.schema->rpc(call.rpc)) return std::nullopt;
    return rpcs(Role::PlayerController, "ClientConfirmMultiInteractFoliage", std::span<const repl::RpcRequest>(&call, 1));
}

std::optional<Send> PlayerBootstrap::foliage_interrupt(uint32_t instance, const std::string& owner, const std::string& component) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const protocol::schema::RpcSchema* rpc = pc_.obj.schema->rpc("ClientInterruptInteractionWithFoliage");
    if (!rpc) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = "ClientInterruptInteractionWithFoliage";
    if (!repl::set_rpc_param(*rpc, call.params, "OriginalInstanceIndex", Value::i64(static_cast<int32_t>(instance))) ||
        !repl::set_rpc_param(*rpc, call.params, "FoliageName", Value(name_of(owner))) || !repl::set_rpc_param(*rpc, call.params, "FoliageComponentUUID", Value(name_of(component)))) {
        return std::nullopt;
    }
    return rpcs(Role::PlayerController, "ClientInterruptInteractionWithFoliage", std::span<const repl::RpcRequest>(&call, 1));
}

}  // namespace aoc::worldd::bootstrap
