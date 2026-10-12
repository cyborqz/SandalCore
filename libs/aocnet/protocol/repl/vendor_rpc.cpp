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

std::optional<RpcParams> open_vendor_params(const schema::RpcSchema& rpc, int64_t merch_set, const emit::FIntrepidNetworkGUID& vendor) {
    if (!is_rpc(rpc, "ClientOpenVendor")) return std::nullopt;
    RpcParams params;
    if (!set_rpc_param(rpc, params, "MerchSetId", Value::i64(merch_set)) || !set_rpc_param(rpc, params, "Vendor", Value(vendor))) return std::nullopt;
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
