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
//          a craft with, CraftItemStates the player's own jobs at this station (an empty list is not sent: a send bit of 0)
//      ClientUpdateCraftingStationUI(CraftingStation, CraftStates)
//          the window's jobs changed: the states of the jobs that did (a start answers with the new job's)
//    client, on the PlayerController
//      ServerStartCraft(CraftingStation, ClientTransactionId, RecipeId, SubRecipeId, Quantity, GeneralResources, FlexibleResources, TagResources, FuelItems,
//                       AutoSelectInventoryBehavior, bUseExactItems)
//          the player pressed Start: the station ACTOR, a guid that comes back in the job's state, the recipe and how many units, and the stacks the player chose to pay with (by record and
//          rarity band; the reagent is not named: the server takes it)
//      ServerSendProcessingStationRequest(ProcessingStation, CraftItemState, RequestType, bWarehouse)
//          the player pressed Claim (RequestType 4) or Cancel (5) on a job: the station ACTOR, the job's state as the window holds it, and whether the products were asked to go to the warehouse
//      ServerInterruptStationCraft(CraftingStation)
//          the player closed the window of a quick make, or pressed its Cancel button, while the craft ran
//    server, on the PlayerController
//      ClientInterruptCraft()
//          no parameters: the client's craft stops (its progress, and the flag that says it is crafting, which only this or the product's arrival clears)
//
//  TaxDetails (FRuntimeTaxDetails) is the tax record the numbers come from, a flat amount, the node's percent, the owning node's share of it, and two flags the window
//  reads as "the tax is set" and "use these numbers".
// ============================================================================
#pragma once

#include "protocol/emit/intrepid_netguid.h"
#include "protocol/repl/craft_codec.h"
#include "protocol/repl/rpc.h"
#include "protocol/repl/rpc_fields.h"
#include "protocol/repl/value.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

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

/// The states of a list of jobs as the array value a CraftItemStates / CraftStates parameter takes: one element each, its only command the state.
inline ArrayValue craft_states_array(const std::vector<CraftItemState>& states) {
    ArrayValue a;
    a.elements.reserve(states.size());
    for (const CraftItemState& s : states) {
        ElementState e;
        e.values[1] = Value(s);
        a.elements.push_back(std::move(e));
    }
    return a;
}

/// ClientOpenCraftingStationUI's parameters, with the player's jobs at the station (none: the list is not sent). nullopt when `rpc` is not that RPC or a value is refused.
inline std::optional<RpcParams> open_station_params(const schema::RpcSchema& rpc, const emit::FIntrepidNetworkGUID& station, const StationTax& tax, const std::vector<CraftItemState>& jobs = {}) {
    if (!rpc.name || std::string_view(rpc.name) != "ClientOpenCraftingStationUI") return std::nullopt;
    RpcParams params;
    if (!set_rpc_param(rpc, params, "CraftingStation", Value(station)) || !set_rpc_param(rpc, params, "TaxDetails.TaxDetailRecordId", Value::i64(tax.record)) ||
        !set_rpc_param(rpc, params, "TaxDetails.FlatRateAmount", Value::i64(tax.flat)) || !set_rpc_param(rpc, params, "TaxDetails.TaxPercent", Value::f32(tax.percent)) ||
        !set_rpc_param(rpc, params, "TaxDetails.OwningNodeTaxPercent", Value::f32(tax.owning_share)) || !set_rpc_param(rpc, params, "TaxDetails.bIsUnlocked", Value::boolean(tax.unlocked)) ||
        !set_rpc_param(rpc, params, "TaxDetails.bUseOverride", Value::boolean(tax.use_override))) {
        return std::nullopt;
    }
    if (!jobs.empty() && !set_rpc_param(rpc, params, "CraftItemStates", Value(craft_states_array(jobs)))) return std::nullopt;
    return params;
}

/// ClientUpdateCraftingStationUI's parameters: the station actor and the states that changed. nullopt when `rpc` is not that RPC or a value is refused.
inline std::optional<RpcParams> update_station_params(const schema::RpcSchema& rpc, const emit::FIntrepidNetworkGUID& station, const std::vector<CraftItemState>& states) {
    if (!rpc.name || std::string_view(rpc.name) != "ClientUpdateCraftingStationUI") return std::nullopt;
    RpcParams params;
    if (!set_rpc_param(rpc, params, "CraftingStation", Value(station))) return std::nullopt;
    if (!states.empty() && !set_rpc_param(rpc, params, "CraftStates", Value(craft_states_array(states)))) return std::nullopt;
    return params;
}

/// What ClientOpenCraftingStationUI carries, read back.
struct OpenStation {
    emit::FIntrepidNetworkGUID  station;
    StationTax                  tax;
    std::vector<CraftItemState> jobs;
    bool operator==(const OpenStation&) const = default;
};

/// The states an array parameter holds, or nullopt when an element is not a state.
inline std::optional<std::vector<CraftItemState>> craft_states_of(const Value* v) {
    std::vector<CraftItemState> out;
    if (!v) return out;
    const ArrayValue* a = v->get_if<ArrayValue>();
    if (!a) return std::nullopt;
    for (const ElementState& e : a->elements) {
        const auto it = e.values.find(1);
        const CraftItemState* s = it == e.values.end() ? nullptr : it->second.get_if<CraftItemState>();
        if (!s) return std::nullopt;
        out.push_back(*s);
    }
    return out;
}

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
    auto jobs = craft_states_of(rpcf::value_of(rpc, call.params, "CraftItemStates"));
    if (!jobs) return std::nullopt;
    o.jobs = std::move(*jobs);
    return o;
}

/// What ClientUpdateCraftingStationUI carries, read back.
struct UpdateStation {
    emit::FIntrepidNetworkGUID  station;
    std::vector<CraftItemState> states;
    bool operator==(const UpdateStation&) const = default;
};

inline std::optional<UpdateStation> parse_update_station(const RpcCall& call) {
    if (!call.rpc || !call.decoded || !call.rpc->name || std::string_view(call.rpc->name) != "ClientUpdateCraftingStationUI") return std::nullopt;
    UpdateStation u;
    if (const Value* v = rpcf::value_of(*call.rpc, call.params, "CraftingStation")) {
        const auto* g = rpcf::as<emit::FIntrepidNetworkGUID>(v);
        if (!g) return std::nullopt;
        u.station = *g;
    }
    auto states = craft_states_of(rpcf::value_of(*call.rpc, call.params, "CraftStates"));
    if (!states) return std::nullopt;
    u.states = std::move(*states);
    return u;
}

/// ServerSendProcessingStationRequest: the player pressed Claim (type 4) or Cancel (type 5) on a job of a processing station.  `state` is the job's state as the window holds it; a
/// server finds the job by its guid and does not trust the rest.  `type` is an EProcessingJobState value; `warehouse` says the products were asked to go to the warehouse.
struct ProcessingRequest {
    emit::FIntrepidNetworkGUID station;
    CraftItemState             state;
    uint8_t                    type = 0;
    bool                       warehouse = false;
    bool operator==(const ProcessingRequest&) const = default;
};

/// ServerSendProcessingStationRequest from a decoded call; nullopt for another RPC, an undecoded call or a parameter of another kind.
inline std::optional<ProcessingRequest> parse_processing_request(const RpcCall& call) {
    if (!call.rpc || !call.decoded || !call.rpc->name || std::string_view(call.rpc->name) != "ServerSendProcessingStationRequest") return std::nullopt;
    const schema::RpcSchema& rpc = *call.rpc;
    ProcessingRequest p;
    if (const Value* v = rpcf::value_of(rpc, call.params, "ProcessingStation")) {
        const auto* g = rpcf::as<emit::FIntrepidNetworkGUID>(v);
        if (!g) return std::nullopt;
        p.station = *g;
    }
    if (const Value* v = rpcf::value_of(rpc, call.params, "CraftItemState")) {
        const auto* s = rpcf::as<CraftItemState>(v);
        if (!s) return std::nullopt;
        p.state = *s;
    }
    const auto type = rpcf::uint_or(rpcf::value_of(rpc, call.params, "RequestType"), 0);
    const auto warehouse = rpcf::bool_or(rpcf::value_of(rpc, call.params, "bWarehouse"), false);
    if (!type || !warehouse || *type > 7) return std::nullopt;
    p.type = static_cast<uint8_t>(*type);
    p.warehouse = *warehouse;
    return p;
}

/// ServerSendProcessingStationRequest's parameters, the form the client sends.  nullopt when `rpc` is not that RPC or a value is refused.
inline std::optional<RpcParams> processing_request_params(const schema::RpcSchema& rpc, const ProcessingRequest& p) {
    if (!rpc.name || std::string_view(rpc.name) != "ServerSendProcessingStationRequest") return std::nullopt;
    RpcParams params;
    if (!set_rpc_param(rpc, params, "ProcessingStation", Value(p.station)) || !set_rpc_param(rpc, params, "CraftItemState", Value(p.state)) ||
        !set_rpc_param(rpc, params, "RequestType", Value::u64(p.type)) || !set_rpc_param(rpc, params, "bWarehouse", Value::boolean(p.warehouse))) {
        return std::nullopt;
    }
    return params;
}

/// ServerStartCraft's parameters. The stacks the player chose to pay with are `general` (and `flexible`, which a processing station's window leaves empty); `has_tags` and `has_fuel` say a
/// list this codec does not read was not empty (a crafting window sends tag selections).
struct StartCraft {
    emit::FIntrepidNetworkGUID station;
    GuidValue                  transaction;
    int64_t                    recipe = 0;
    int64_t                    sub_recipe = 0;
    int32_t                    quantity = 0;
    std::vector<CraftResource> general, flexible;
    bool                       has_tags = false;
    bool                       has_fuel = false;
    uint8_t                    auto_select = 0;
    bool                       exact = false;
    bool operator==(const StartCraft&) const = default;
};

namespace craft_detail {

/// The resources of GeneralResources / FlexibleResources: each element is {RecordId (local 1), RarityCount (local 2: an array of {Rarity (1), Count (2), bFromWarehouse (3)})}, the order of
/// the generated element commands.
inline std::optional<std::vector<CraftResource>> resources_of(const Value* v) {
    std::vector<CraftResource> out;
    if (!v) return out;
    const ArrayValue* a = v->get_if<ArrayValue>();
    if (!a || a->elements.size() > kMaxCraftResources) return std::nullopt;
    for (const ElementState& e : a->elements) {
        CraftResource r;
        const auto rec = e.values.find(1);
        const auto cnt = e.values.find(2);
        const auto id = rec == e.values.end() ? std::optional<int64_t>() : rpcf::int_or(&rec->second, 0);
        const ArrayValue* rarities = cnt == e.values.end() ? nullptr : cnt->second.get_if<ArrayValue>();
        if (!id || !rarities || rarities->elements.size() > kMaxCraftRarities) return std::nullopt;
        r.record = *id;
        for (const ElementState& re : rarities->elements) {
            const auto rar = re.values.find(1), num = re.values.find(2), wh = re.values.find(3);
            const auto rarity = rar == re.values.end() ? std::optional<uint64_t>() : rpcf::uint_or(&rar->second, 0);
            const auto count = num == re.values.end() ? std::optional<int64_t>() : rpcf::int_or(&num->second, 0);
            const auto warehouse = wh == re.values.end() ? std::optional<bool>(false) : rpcf::bool_or(&wh->second, false);
            if (!rarity || !count || !warehouse || *rarity > 6 || *count < 0 || *count > kMaxCraftCount) return std::nullopt;
            r.rarities.push_back({static_cast<uint8_t>(*rarity), static_cast<int32_t>(*count), *warehouse});
        }
        out.push_back(std::move(r));
    }
    return out;
}

inline Value resources_value(const std::vector<CraftResource>& list) {
    ArrayValue a;
    for (const CraftResource& r : list) {
        ElementState e;
        e.values[1] = Value::i64(r.record);
        ArrayValue rarities;
        for (const CraftRarityCount& c : r.rarities) {
            ElementState re;
            re.values[1] = Value::u64(c.rarity);
            re.values[2] = Value::i64(c.count);
            re.values[3] = Value::boolean(c.from_warehouse);
            rarities.elements.push_back(std::move(re));
        }
        e.values[2] = Value(std::move(rarities));
        a.elements.push_back(std::move(e));
    }
    return Value(std::move(a));
}

}  // namespace craft_detail

/// ServerStartCraft from a decoded call; nullopt for another RPC, an undecoded call or a parameter of another kind.
inline std::optional<StartCraft> parse_start_craft(const RpcCall& call) {
    if (!call.rpc || !call.decoded || !call.rpc->name || std::string_view(call.rpc->name) != "ServerStartCraft") return std::nullopt;
    const schema::RpcSchema& rpc = *call.rpc;
    StartCraft s;
    if (const Value* v = rpcf::value_of(rpc, call.params, "CraftingStation")) {
        const auto* g = rpcf::as<emit::FIntrepidNetworkGUID>(v);
        if (!g) return std::nullopt;
        s.station = *g;
    }
    const auto word = [&](const char* path, uint32_t& out) {
        const auto v = rpcf::int_or(rpcf::value_of(rpc, call.params, path), 0);
        if (!v) return false;
        out = static_cast<uint32_t>(*v);
        return true;
    };
    const auto recipe = rpcf::int_or(rpcf::value_of(rpc, call.params, "RecipeId"), 0);
    const auto sub = rpcf::int_or(rpcf::value_of(rpc, call.params, "SubRecipeId"), 0);
    const auto quantity = rpcf::int_or(rpcf::value_of(rpc, call.params, "Quantity"), 0);
    const auto select = rpcf::uint_or(rpcf::value_of(rpc, call.params, "AutoSelectInventoryBehavior"), 0);
    const auto exact = rpcf::bool_or(rpcf::value_of(rpc, call.params, "bUseExactItems"), false);
    auto general = craft_detail::resources_of(rpcf::value_of(rpc, call.params, "GeneralResources"));
    auto flexible = craft_detail::resources_of(rpcf::value_of(rpc, call.params, "FlexibleResources"));
    if (!word("ClientTransactionId.A", s.transaction.a) || !word("ClientTransactionId.B", s.transaction.b) || !word("ClientTransactionId.C", s.transaction.c) ||
        !word("ClientTransactionId.D", s.transaction.d) || !recipe || !sub || !quantity || !select || !exact || !general || !flexible || *quantity < INT32_MIN || *quantity > INT32_MAX) {
        return std::nullopt;
    }
    s.recipe = *recipe;
    s.sub_recipe = *sub;
    s.quantity = static_cast<int32_t>(*quantity);
    s.general = std::move(*general);
    s.flexible = std::move(*flexible);
    s.has_tags = rpcf::value_of(rpc, call.params, "TagResources") != nullptr;
    s.has_fuel = rpcf::value_of(rpc, call.params, "FuelItems") != nullptr;
    s.auto_select = static_cast<uint8_t>(*select);
    s.exact = *exact;
    return s;
}

/// ServerStartCraft's parameters, the form the client sends (the stacks it chose; the reagent is not named). nullopt when `rpc` is not that RPC or a value is refused.
inline std::optional<RpcParams> start_craft_params(const schema::RpcSchema& rpc, const StartCraft& s) {
    if (!rpc.name || std::string_view(rpc.name) != "ServerStartCraft") return std::nullopt;
    RpcParams params;
    if (!set_rpc_param(rpc, params, "CraftingStation", Value(s.station)) || !set_rpc_param(rpc, params, "ClientTransactionId.A", Value::i64(static_cast<int32_t>(s.transaction.a))) ||
        !set_rpc_param(rpc, params, "ClientTransactionId.B", Value::i64(static_cast<int32_t>(s.transaction.b))) || !set_rpc_param(rpc, params, "ClientTransactionId.C", Value::i64(static_cast<int32_t>(s.transaction.c))) ||
        !set_rpc_param(rpc, params, "ClientTransactionId.D", Value::i64(static_cast<int32_t>(s.transaction.d))) || !set_rpc_param(rpc, params, "RecipeId", Value::i64(s.recipe)) ||
        !set_rpc_param(rpc, params, "SubRecipeId", Value::i64(s.sub_recipe)) || !set_rpc_param(rpc, params, "Quantity", Value::i64(s.quantity)) ||
        !set_rpc_param(rpc, params, "AutoSelectInventoryBehavior", Value::u64(s.auto_select)) || !set_rpc_param(rpc, params, "bUseExactItems", Value::boolean(s.exact))) {
        return std::nullopt;
    }
    if (!s.general.empty() && !set_rpc_param(rpc, params, "GeneralResources", craft_detail::resources_value(s.general))) return std::nullopt;
    if (!s.flexible.empty() && !set_rpc_param(rpc, params, "FlexibleResources", craft_detail::resources_value(s.flexible))) return std::nullopt;
    return params;
}

/// ServerInterruptStationCraft(CraftingStation): the player closed the station's window, or pressed Cancel, while a craft ran.  The station ACTOR, or nullopt for another RPC, an undecoded
/// call or a parameter of another kind.
inline std::optional<emit::FIntrepidNetworkGUID> parse_interrupt_craft(const RpcCall& call) {
    if (!call.rpc || !call.decoded || !call.rpc->name || std::string_view(call.rpc->name) != "ServerInterruptStationCraft") return std::nullopt;
    const Value* v = rpcf::value_of(*call.rpc, call.params, "CraftingStation");
    if (!v) return emit::FIntrepidNetworkGUID{};
    const auto* g = rpcf::as<emit::FIntrepidNetworkGUID>(v);
    if (!g) return std::nullopt;
    return *g;
}

/// ServerInterruptStationCraft's parameters, the form the client sends.  nullopt when `rpc` is not that RPC or a value is refused.
inline std::optional<RpcParams> interrupt_craft_params(const schema::RpcSchema& rpc, const emit::FIntrepidNetworkGUID& station) {
    if (!rpc.name || std::string_view(rpc.name) != "ServerInterruptStationCraft") return std::nullopt;
    RpcParams params;
    if (!set_rpc_param(rpc, params, "CraftingStation", Value(station))) return std::nullopt;
    return params;
}

}}}  // namespace aoc::protocol::repl
