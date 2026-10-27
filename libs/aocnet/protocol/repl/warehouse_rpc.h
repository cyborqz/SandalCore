// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/warehouse_rpc.h  (storage clerks, step 0; header only)
//
//  The warehouse window's RPCs in a typed form, over rpc.h's parameter sets:
//
//    server, on the PlayerController
//      ClientOpenWarehouseWindow(WarehouseId, StorageAccessConfig)
//          opens the storage window of a warehouse: WarehouseId is {Area, AreaRecordGuid} (Area 1 is a node's warehouse, the guid the node's record), StorageAccessConfig a
//          bit mask of what the window may show (1 the character's item chunks, 2 the node's material tabs, 4 its cargo tab; a storage clerk always sends 7)
//      ClientReceiveWarehouseSummary(Summary)
//          the answer to a summary request: the two defs the node names (material tabs, cargo tab) and, in the defs' order, every item chunk, material tab and cargo tab with
//          whether the player may buy it and what it costs (warehouse_codec.h)
//    client, on the PlayerController
//      ServerRequestWarehouseSummary(WarehouseId)        the window wants the summary (sent twice when it opens)
//      ServerVisitWarehouse(WarehouseId, StorageAccessConfig)   a window opened from another place (the caravan master's); a storage clerk's window never sends it
//    client, on the pawn
//      ServerFetchWarehouse(WarehouseId, StorageType, StorageAccessConfig)    the window asks the server to have a kind of container ready (StorageType 0 General, 1 Crates)
//
//  A parameter left at its default is not on the wire; the readers fall back to the default.
// ============================================================================
#pragma once

#include "protocol/repl/rpc.h"
#include "protocol/repl/rpc_fields.h"
#include "protocol/repl/value.h"
#include "protocol/repl/warehouse_codec.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace aoc { namespace protocol { namespace repl {

/// EWarehouseArea.
constexpr uint8_t kWarehouseAreaNode = 1;

/// EStorageAccessConfig's bits.
constexpr uint8_t kAccessGlobalItems = 1;
constexpr uint8_t kAccessNodeMaterials = 2;
constexpr uint8_t kAccessNodeCrates = 4;
constexpr uint8_t kAccessClerk = kAccessGlobalItems | kAccessNodeMaterials | kAccessNodeCrates;

/// EResourceStorageType.
constexpr uint8_t kStorageGeneral = 0;
constexpr uint8_t kStorageCrates = 1;

struct WarehouseId {
    uint8_t area = 0;
    int64_t record = 0;
    bool operator==(const WarehouseId&) const = default;
};

struct WarehouseSummary {
    WarehouseId                        id;
    int64_t                            resource_def = 0;
    int64_t                            crate_def = 0;
    std::vector<WarehouseSummaryEntry> chunks;
    std::vector<WarehouseSummaryEntry> tabs;
    std::vector<WarehouseSummaryEntry> crates;
    bool operator==(const WarehouseSummary&) const = default;
};

namespace warehouse_detail {

inline bool set_id(const schema::RpcSchema& rpc, RpcParams& params, std::string_view prefix, const WarehouseId& id) {
    std::string base(prefix);
    return set_rpc_param(rpc, params, base + "Area", Value::u64(id.area)) && set_rpc_param(rpc, params, base + "AreaRecordGuid", Value::i64(id.record));
}

inline bool read_id(const RpcCall& call, std::string_view prefix, WarehouseId& out) {
    std::string base(prefix);
    const auto area = rpcf::uint_or(rpcf::value_of(*call.rpc, call.params, base + "Area"), 0);
    const auto record = rpcf::int_or(rpcf::value_of(*call.rpc, call.params, base + "AreaRecordGuid"), 0);
    if (!area || !record || *area > 7) return false;
    out.area = static_cast<uint8_t>(*area);
    out.record = *record;
    return true;
}

inline ArrayValue entries_array(const std::vector<WarehouseSummaryEntry>& entries) {
    ArrayValue a;
    a.elements.reserve(entries.size());
    for (const WarehouseSummaryEntry& e : entries) {
        ElementState s;
        s.values[1] = Value(e);
        a.elements.push_back(std::move(s));
    }
    return a;
}

inline bool entries_of(const Value* v, std::vector<WarehouseSummaryEntry>& out) {
    out.clear();
    if (!v) return true;
    const ArrayValue* a = v->get_if<ArrayValue>();
    if (!a) return false;
    for (const ElementState& e : a->elements) {
        const auto it = e.values.find(1);
        const WarehouseSummaryEntry* s = it == e.values.end() ? nullptr : it->second.get_if<WarehouseSummaryEntry>();
        if (!s) return false;
        out.push_back(*s);
    }
    return true;
}

inline bool call_is(const RpcCall& call, std::string_view name) { return call.rpc && call.decoded && call.rpc->name && std::string_view(call.rpc->name) == name; }

}  // namespace warehouse_detail

/// ClientOpenWarehouseWindow's parameters. nullopt when `rpc` is not that RPC or a value is refused.
inline std::optional<RpcParams> open_warehouse_window_params(const schema::RpcSchema& rpc, const WarehouseId& id, uint8_t access_config) {
    if (!rpc.name || std::string_view(rpc.name) != "ClientOpenWarehouseWindow") return std::nullopt;
    RpcParams params;
    if (!warehouse_detail::set_id(rpc, params, "WarehouseId.", id) || !set_rpc_param(rpc, params, "StorageAccessConfig", Value::u64(access_config))) return std::nullopt;
    return params;
}

struct OpenWarehouse {
    WarehouseId id;
    uint8_t     access_config = 0;
    bool operator==(const OpenWarehouse&) const = default;
};

inline std::optional<OpenWarehouse> parse_open_warehouse_window(const RpcCall& call) {
    if (!warehouse_detail::call_is(call, "ClientOpenWarehouseWindow")) return std::nullopt;
    OpenWarehouse o;
    if (!warehouse_detail::read_id(call, "WarehouseId.", o.id)) return std::nullopt;
    const auto config = rpcf::uint_or(rpcf::value_of(*call.rpc, call.params, "StorageAccessConfig"), 0);
    if (!config || *config > 63) return std::nullopt;
    o.access_config = static_cast<uint8_t>(*config);
    return o;
}

/// ClientReceiveWarehouseSummary's parameters. nullopt when `rpc` is not that RPC or a value is refused.
inline std::optional<RpcParams> warehouse_summary_params(const schema::RpcSchema& rpc, const WarehouseSummary& s) {
    if (!rpc.name || std::string_view(rpc.name) != "ClientReceiveWarehouseSummary") return std::nullopt;
    RpcParams params;
    if (!warehouse_detail::set_id(rpc, params, "Summary.Id.", s.id) || !set_rpc_param(rpc, params, "Summary.ResourceWarehouseStorageDefRecordGuid", Value::i64(s.resource_def)) ||
        !set_rpc_param(rpc, params, "Summary.CrateWarehouseStorageDefRecordGuid", Value::i64(s.crate_def))) {
        return std::nullopt;
    }
    if (!s.chunks.empty() && !set_rpc_param(rpc, params, "Summary.ItemChunkSummaries", Value(warehouse_detail::entries_array(s.chunks)))) return std::nullopt;
    if (!s.tabs.empty() && !set_rpc_param(rpc, params, "Summary.MaterialTabSummaries", Value(warehouse_detail::entries_array(s.tabs)))) return std::nullopt;
    if (!s.crates.empty() && !set_rpc_param(rpc, params, "Summary.CrateTabSummaries", Value(warehouse_detail::entries_array(s.crates)))) return std::nullopt;
    return params;
}

inline std::optional<WarehouseSummary> parse_warehouse_summary(const RpcCall& call) {
    if (!warehouse_detail::call_is(call, "ClientReceiveWarehouseSummary")) return std::nullopt;
    WarehouseSummary s;
    if (!warehouse_detail::read_id(call, "Summary.Id.", s.id)) return std::nullopt;
    const auto res = rpcf::int_or(rpcf::value_of(*call.rpc, call.params, "Summary.ResourceWarehouseStorageDefRecordGuid"), 0);
    const auto crate = rpcf::int_or(rpcf::value_of(*call.rpc, call.params, "Summary.CrateWarehouseStorageDefRecordGuid"), 0);
    if (!res || !crate) return std::nullopt;
    s.resource_def = *res;
    s.crate_def = *crate;
    if (!warehouse_detail::entries_of(rpcf::value_of(*call.rpc, call.params, "Summary.ItemChunkSummaries"), s.chunks) ||
        !warehouse_detail::entries_of(rpcf::value_of(*call.rpc, call.params, "Summary.MaterialTabSummaries"), s.tabs) ||
        !warehouse_detail::entries_of(rpcf::value_of(*call.rpc, call.params, "Summary.CrateTabSummaries"), s.crates)) {
        return std::nullopt;
    }
    return s;
}

/// What the client's four warehouse calls carry: the id, and (ServerVisitWarehouse, ServerFetchWarehouse) the access config and (ServerFetchWarehouse) the kind of storage.
struct WarehouseRequest {
    WarehouseId id;
    uint8_t     access_config = 0;
    uint8_t     storage_type = 0;
    bool operator==(const WarehouseRequest&) const = default;
};

/// ServerRequestWarehouseSummary, ServerVisitWarehouse or ServerFetchWarehouse from a decoded call; nullopt for another RPC, an undecoded call or a value out of range.
inline std::optional<WarehouseRequest> parse_warehouse_request(const RpcCall& call) {
    const bool summary = warehouse_detail::call_is(call, "ServerRequestWarehouseSummary");
    const bool visit = warehouse_detail::call_is(call, "ServerVisitWarehouse");
    const bool fetch = warehouse_detail::call_is(call, "ServerFetchWarehouse");
    if (!summary && !visit && !fetch) return std::nullopt;
    WarehouseRequest q;
    if (!warehouse_detail::read_id(call, "WarehouseId.", q.id)) return std::nullopt;
    if (visit || fetch) {
        const auto config = rpcf::uint_or(rpcf::value_of(*call.rpc, call.params, "StorageAccessConfig"), 0);
        if (!config || *config > 63) return std::nullopt;
        q.access_config = static_cast<uint8_t>(*config);
    }
    if (fetch) {
        const auto type = rpcf::uint_or(rpcf::value_of(*call.rpc, call.params, "StorageType"), 0);
        if (!type || *type > 3) return std::nullopt;
        q.storage_type = static_cast<uint8_t>(*type);
    }
    return q;
}

}}}  // namespace aoc::protocol::repl
