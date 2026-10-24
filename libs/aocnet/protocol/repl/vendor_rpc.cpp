// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/vendor_rpc.cpp
//
//  See vendor_rpc.h.
// ============================================================================
#include "protocol/repl/vendor_rpc.h"

#include "protocol/repl/rpc_fields.h"

#include <string_view>

namespace aoc { namespace protocol { namespace repl {

namespace {

bool is_rpc(const schema::RpcSchema& rpc, std::string_view name) { return rpc.name && name == rpc.name; }

bool decoded_as(const RpcCall& call, std::string_view name) { return call.rpc && call.decoded && is_rpc(*call.rpc, name); }

}  // namespace

std::optional<InteractRequest> parse_interact_request(const RpcCall& call) {
    if (!decoded_as(call, "ServerInteractWithInteractableComponent")) return std::nullopt;
    const schema::RpcSchema& rpc = *call.rpc;
    InteractRequest r;
    const Value* o = rpcf::value_of(rpc, call.params, "Interactable");
    if (o) {
        const auto* g = rpcf::as<emit::FIntrepidNetworkGUID>(o);
        if (!g) return std::nullopt;
        r.interactable = *g;
    }
    const auto counter = rpcf::int_or(rpcf::value_of(rpc, call.params, "Counter"), 0);
    const auto zone = rpcf::uint_or(rpcf::value_of(rpc, call.params, "ZoneID"), 0);
    if (!counter || !zone || *counter < INT16_MIN || *counter > INT16_MAX || *zone > UINT16_MAX) return std::nullopt;
    r.counter = static_cast<int16_t>(*counter);
    r.zone = static_cast<uint16_t>(*zone);
    return r;
}

std::optional<RpcParams> interact_params(const schema::RpcSchema& rpc, const InteractRequest& request) {
    if (!is_rpc(rpc, "ServerInteractWithInteractableComponent")) return std::nullopt;
    RpcParams params;
    if (!set_rpc_param(rpc, params, "Interactable", Value(request.interactable)) ||
        !set_rpc_param(rpc, params, "Counter", Value::i64(request.counter)) ||
        !set_rpc_param(rpc, params, "ZoneID", Value::u64(request.zone))) {
        return std::nullopt;
    }
    return params;
}

std::optional<VendorPurchaseCart> parse_vendor_purchase(const RpcCall& call) {
    if (!decoded_as(call, "ServerHandleVendorPurchase")) return std::nullopt;
    const auto* c = rpcf::as<VendorPurchaseCart>(rpcf::value_of(*call.rpc, call.params, "PurchaseData"));
    if (!c) return std::nullopt;
    return *c;
}

std::optional<RpcParams> vendor_purchase_params(const schema::RpcSchema& rpc, const VendorPurchaseCart& cart) {
    if (!is_rpc(rpc, "ServerHandleVendorPurchase")) return std::nullopt;
    RpcParams params;
    if (!set_rpc_param(rpc, params, "PurchaseData", Value(cart))) return std::nullopt;
    return params;
}
// ~ C:Y:B & C O A ~

std::optional<VendorTransactionCart> parse_vendor_transaction(const RpcCall& call) {
    if (!decoded_as(call, "ServerHandleVendorTransaction")) return std::nullopt;
    const auto* c = rpcf::as<VendorTransactionCart>(rpcf::value_of(*call.rpc, call.params, "VendorCart"));
    if (!c) return std::nullopt;
    return *c;
}

std::optional<RpcParams> vendor_transaction_params(const schema::RpcSchema& rpc, const VendorTransactionCart& cart) {
    if (!is_rpc(rpc, "ServerHandleVendorTransaction")) return std::nullopt;
    RpcParams params;
    if (!set_rpc_param(rpc, params, "VendorCart", Value(cart))) return std::nullopt;
    return params;
}

namespace {

/// The element commands of an array command of `rpc` (empty when its range is bad), and the local handle (1-based position) of the one named `path`, 0 when there is none.
std::span<const schema::RepCommand> elements_of(const schema::RpcSchema& rpc, const schema::RepCommand& array) {
    if (array.type != schema::PropType::DynamicArray || array.elem_end < array.elem_begin || array.elem_end > rpc.element_commands.size()) return {};
    return rpc.element_commands.subspan(array.elem_begin, array.elem_end - array.elem_begin);
}
uint16_t local_handle(std::span<const schema::RepCommand> elems, std::string_view path) {
    for (size_t i = 0; i < elems.size(); ++i) {
        if (path == elems[i].path) return static_cast<uint16_t>(i + 1);
    }
    return 0;
}

/// An array of uint8 (the positions of the failed predicates).
Value bytes_value(const std::vector<uint8_t>& bytes) {
    ArrayValue a;
    for (const uint8_t b : bytes) {
        ElementState e;
        e.values[1] = Value::u64(b);
        a.elements.push_back(std::move(e));
    }
    return Value(std::move(a));
}

}  // namespace

std::optional<RpcParams> open_vendor_params(const schema::RpcSchema& rpc, int64_t merch_set, const emit::FIntrepidNetworkGUID& vendor, const UnavailableList& unavailable) {
    if (!is_rpc(rpc, "ClientOpenVendor")) return std::nullopt;
    RpcParams params;
    if (!set_rpc_param(rpc, params, "MerchSetId", Value::i64(merch_set)) || !set_rpc_param(rpc, params, "Vendor", Value(vendor))) return std::nullopt;
    if (unavailable.empty()) return params;
    // VendorInventoryUnavailableList: per tab { UnavailableListings: [{ Index, PredicateFailures, bHidden }], UnavailableInventory: { Index, PredicateFailures, bHidden } }
    const schema::RepCommand* list = rpc_command(rpc, "VendorInventoryUnavailableList");
    if (!list || unavailable.size() > 0xFFFF) return std::nullopt;
    const auto tab_elems = elements_of(rpc, *list);
    const uint16_t h_listings = local_handle(tab_elems, "VendorInventoryUnavailableList.UnavailableListings"), h_index = local_handle(tab_elems, "VendorInventoryUnavailableList.UnavailableInventory.Index"),
                   h_failures = local_handle(tab_elems, "VendorInventoryUnavailableList.UnavailableInventory.PredicateFailures"),
                   h_hidden = local_handle(tab_elems, "VendorInventoryUnavailableList.UnavailableInventory.bHidden");
    if (!h_listings || !h_index || !h_failures || !h_hidden) return std::nullopt;
    const auto listing_elems = elements_of(rpc, tab_elems[h_listings - 1]);
    const uint16_t l_index = local_handle(listing_elems, "UnavailableListings.Index"), l_failures = local_handle(listing_elems, "UnavailableListings.PredicateFailures"),
                   l_hidden = local_handle(listing_elems, "UnavailableListings.bHidden");
    if (!l_index || !l_failures || !l_hidden) return std::nullopt;
    ArrayValue tabs;
    for (const UnavailableTab& t : unavailable) {
        if (t.listings.size() > 0xFFFF || t.inventory.failures.size() > 0xFFFF) return std::nullopt;
        ArrayValue listings;
        for (const UnavailableEntry& l : t.listings) {
            if (l.failures.size() > 0xFFFF) return std::nullopt;
            ElementState e;
            e.values[l_index] = Value::i64(l.index);
            e.values[l_failures] = bytes_value(l.failures);
            e.values[l_hidden] = Value::boolean(l.hidden);
            listings.elements.push_back(std::move(e));
        }
        ElementState tab;
        tab.values[h_listings] = Value(std::move(listings));
        tab.values[h_index] = Value::i64(t.inventory.index);
        tab.values[h_failures] = bytes_value(t.inventory.failures);
        tab.values[h_hidden] = Value::boolean(t.inventory.hidden);
        tabs.elements.push_back(std::move(tab));
    }
    if (!set_rpc_param(rpc, params, "VendorInventoryUnavailableList", Value(std::move(tabs)))) return std::nullopt;
    return params;
}

std::optional<OpenVendorRequest> parse_server_open_vendor(const RpcCall& call) {
    if (!decoded_as(call, "ServerOpenVendor")) return std::nullopt;
    const schema::RpcSchema& rpc = *call.rpc;
    // a parameter left at its default is not on the wire: the zero FGuid reads as zero words
    const auto word = [&](std::string_view path) -> std::optional<int64_t> { return rpcf::int_or(rpcf::value_of(rpc, call.params, path), 0); };
    const auto a = word("VendorNPCGuid.A"), b = word("VendorNPCGuid.B"), c = word("VendorNPCGuid.C"), d = word("VendorNPCGuid.D");
    const auto def = rpcf::int_or(rpcf::value_of(rpc, call.params, "VendorDefRecordGuid"), 0);
    if (!a || !b || !c || !d || !def) return std::nullopt;
    OpenVendorRequest r;
    r.npc = GuidValue{static_cast<uint32_t>(*a), static_cast<uint32_t>(*b), static_cast<uint32_t>(*c), static_cast<uint32_t>(*d)};
    r.vendor = *def;
    return r;
}

std::optional<RpcParams> server_open_vendor_params(const schema::RpcSchema& rpc, const OpenVendorRequest& request) {
    if (!is_rpc(rpc, "ServerOpenVendor")) return std::nullopt;
    RpcParams params;
    if (!set_rpc_param(rpc, params, "VendorNPCGuid.A", Value::i64(static_cast<int32_t>(request.npc.a))) || !set_rpc_param(rpc, params, "VendorNPCGuid.B", Value::i64(static_cast<int32_t>(request.npc.b))) ||
        !set_rpc_param(rpc, params, "VendorNPCGuid.C", Value::i64(static_cast<int32_t>(request.npc.c))) || !set_rpc_param(rpc, params, "VendorNPCGuid.D", Value::i64(static_cast<int32_t>(request.npc.d))) ||
        !set_rpc_param(rpc, params, "VendorDefRecordGuid", Value::i64(request.vendor))) {
        return std::nullopt;
    }
    return params;
}

std::optional<int64_t> parse_node_tax_request(const RpcCall& call) {
    if (!decoded_as(call, "ServerGetNodeTaxTable")) return std::nullopt;
    return rpcf::int_or(rpcf::value_of(*call.rpc, call.params, "NodeGuid"), 0);
}

std::optional<RpcParams> node_tax_request_params(const schema::RpcSchema& rpc, int64_t node) {
    if (!is_rpc(rpc, "ServerGetNodeTaxTable")) return std::nullopt;
    RpcParams params;
    if (!set_rpc_param(rpc, params, "NodeGuid", Value::i64(node))) return std::nullopt;
    return params;
}

std::optional<NodeTaxDetails> parse_node_tax_table(const RpcCall& call) {
    if (!decoded_as(call, "ClientSetNodeTaxTable")) return std::nullopt;
    const auto* d = rpcf::as<NodeTaxDetails>(rpcf::value_of(*call.rpc, call.params, "NodeTaxDetails"));
    if (!d) return std::nullopt;
    return *d;
}

std::optional<RpcParams> node_tax_table_params(const schema::RpcSchema& rpc, const NodeTaxDetails& details) {
    if (!is_rpc(rpc, "ClientSetNodeTaxTable")) return std::nullopt;
    RpcParams params;
    if (!set_rpc_param(rpc, params, "NodeTaxDetails", Value(details))) return std::nullopt;
    return params;
}

std::optional<OpenVendor> parse_open_vendor(const RpcCall& call) {
    if (!decoded_as(call, "ClientOpenVendor")) return std::nullopt;
    OpenVendor o;
    const auto set = rpcf::int_or(rpcf::value_of(*call.rpc, call.params, "MerchSetId"), 0);
    if (!set) return std::nullopt;
    o.merch_set = *set;
    if (const Value* v = rpcf::value_of(*call.rpc, call.params, "Vendor")) {
        const auto* g = rpcf::as<emit::FIntrepidNetworkGUID>(v);
        if (!g) return std::nullopt;
        o.vendor = *g;
    }
    return o;
}

}}}  // namespace aoc::protocol::repl
