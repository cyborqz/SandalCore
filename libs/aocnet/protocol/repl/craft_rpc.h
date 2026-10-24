// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/craft_rpc.h
//
//  The crafting station RPCs in a typed form, over rpc.h's parameter sets (header only):
//
//    client, on the pawn
//      ServerInteractWithInteractableComponent(Interactable, Counter, ZoneID)    the player used a station's InteractableComponent (vendor_rpc.h)
//    server, on the PlayerController
//      ClientOpenCraftingStationUI(CraftingStation, TaxDetails, CraftItemStates)
//          opens the station's window: CraftingStation is the station ACTOR (the click names its InteractableComponent), TaxDetails the service tax the window prices
//          a craft with, CraftItemStates the player's own jobs at this station (an empty list is not sent: a send bit of 0; this step opens with none)
//
//  TaxDetails (FRuntimeTaxDetails) is the tax record the numbers come from, a flat amount, the node's percent, the owning node's share of it, and two flags the window
//  reads as "the tax is set" and "use these numbers".
// ============================================================================
#pragma once

#include "protocol/emit/intrepid_netguid.h"
#include "protocol/repl/rpc.h"
#include "protocol/repl/rpc_fields.h"
#include "protocol/repl/value.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace aoc { namespace protocol { namespace repl {

struct StationTax {
    int64_t record = 0;          // the TaxDetails record (a design-data guid)
    int32_t flat = 0;            // FlatRateAmount
    float   percent = 0.0f;      // TaxPercent
    float   owning_share = 0.0f; // OwningNodeTaxPercent
    bool    unlocked = true;     // bIsUnlocked
    bool    use_override = true; // bUseOverride
    bool operator==(const StationTax&) const = default;
};

/// ClientOpenCraftingStationUI's parameters with no jobs. nullopt when `rpc` is not that RPC or a value is refused.
inline std::optional<RpcParams> open_station_params(const schema::RpcSchema& rpc, const emit::FIntrepidNetworkGUID& station, const StationTax& tax) {
    if (!rpc.name || std::string_view(rpc.name) != "ClientOpenCraftingStationUI") return std::nullopt;
    RpcParams params;
    if (!set_rpc_param(rpc, params, "CraftingStation", Value(station)) || !set_rpc_param(rpc, params, "TaxDetails.TaxDetailRecordId", Value::i64(tax.record)) ||
        !set_rpc_param(rpc, params, "TaxDetails.FlatRateAmount", Value::i64(tax.flat)) || !set_rpc_param(rpc, params, "TaxDetails.TaxPercent", Value::f32(tax.percent)) ||
        !set_rpc_param(rpc, params, "TaxDetails.OwningNodeTaxPercent", Value::f32(tax.owning_share)) || !set_rpc_param(rpc, params, "TaxDetails.bIsUnlocked", Value::boolean(tax.unlocked)) ||
        !set_rpc_param(rpc, params, "TaxDetails.bUseOverride", Value::boolean(tax.use_override))) {
        return std::nullopt;
    }
    return params;
}

/// What ClientOpenCraftingStationUI carries, read back (no jobs).
struct OpenStation {
    emit::FIntrepidNetworkGUID station;
    StationTax                 tax;
    bool operator==(const OpenStation&) const = default;
};

/// ClientOpenCraftingStationUI from a decoded call; nullopt for another RPC, an undecoded call or a parameter of another kind.
inline std::optional<OpenStation> parse_open_station(const RpcCall& call) {
    if (!call.rpc || !call.decoded || !call.rpc->name || std::string_view(call.rpc->name) != "ClientOpenCraftingStationUI") return std::nullopt;
    const schema::RpcSchema& rpc = *call.rpc;
    OpenStation o;
    if (const Value* v = rpcf::value_of(rpc, call.params, "CraftingStation")) {
        const auto* g = rpcf::as<emit::FIntrepidNetworkGUID>(v);
        if (!g) return std::nullopt;
        o.station = *g;
    }
    const auto record = rpcf::int_or(rpcf::value_of(rpc, call.params, "TaxDetails.TaxDetailRecordId"), 0);
    const auto flat = rpcf::int_or(rpcf::value_of(rpc, call.params, "TaxDetails.FlatRateAmount"), 0);
    if (!record || !flat) return std::nullopt;
    o.tax.record = *record;
    o.tax.flat = static_cast<int32_t>(*flat);
    const auto float_of = [&](std::string_view path, float& out) {
        const Value* v = rpcf::value_of(rpc, call.params, path);
        if (!v) return true;
        const float* f = rpcf::as<float>(v);
        if (!f) return false;
        out = *f;
        return true;
    };
    const auto bool_of = [&](std::string_view path, bool& out) {
        const Value* v = rpcf::value_of(rpc, call.params, path);
        if (!v) {
            out = false;
            return true;
        }
        const bool* b = rpcf::as<bool>(v);
        if (!b) return false;
        out = *b;
        return true;
    };
    if (!float_of("TaxDetails.TaxPercent", o.tax.percent) || !float_of("TaxDetails.OwningNodeTaxPercent", o.tax.owning_share) || !bool_of("TaxDetails.bIsUnlocked", o.tax.unlocked) ||
        !bool_of("TaxDetails.bUseOverride", o.tax.use_override)) {
        return std::nullopt;
    }
    return o;
}

}}}  // namespace aoc::protocol::repl
