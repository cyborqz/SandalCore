// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/vendor_rpc.h
//
//  The vendor RPCs of plan E1's GM vendor in a typed form, over rpc.h's parameter sets and vendor_codec.h's carts:
//
//    client, on the pawn (APlayerCharacter)
//      ServerInteractWithInteractableComponent(Interactable, Counter, ZoneID)
//          the player used an InteractableComponent of the world (an NPC's, a node's): the component's NetGUID, a counter the client
//          counts its interactions with (an int16) and the zone id (a uint16, 0 on the wire when 0)
//    client, on the PlayerController (AAoCPlayerController)
//      ServerHandleVendorPurchase(PurchaseData: FVendorPurchaseCart)        buy
//      ServerHandleVendorTransaction(VendorCart: FVendorTransactionCart)    sell to the vendor / buy back
//    server, on the PlayerController
//      ClientSetNodeTaxTable(NodeTaxDetails: FNodeTaxDetails)
//          the answer to the client's ServerGetNodeTaxTable: the tax table of the node the vendor stands in.  The vendor window shows nothing
//          until it arrives (below)
//    client, on the PlayerController, too
//      ServerGetNodeTaxTable(NodeGuid)
//          sent by the vendor window as soon as it opens: the node of the vendor (UAoCNodePlotComponent::NodeRecordGuid of its NPC)
//    server, on the PlayerController
//      ClientOpenVendor(MerchSetId, Vendor, VendorInventoryUnavailableList)
//          opens the vendor window: MerchSetId is the VendorDef guid (the client's own table says what it sells and for how much), Vendor the
//          NPC (the actor), the list the listings the player cannot buy (empty in the captured open: a 0 send bit)
//
//  What the retail server does [observed, captures 716, 863, 1531, 1536]: ServerInteract..., then within one packet ClientOpenVendor (the
//  client answers with ServerGetNodeTaxTable, the next packet; the server's ClientSetNodeTaxTable, 6.8 KB in about eight packets, follows; the
//  window's listings, the taxes and the funds all appear when it is received: UVendorMenu::InitializeVendor of the exe asks for the table and
//  only HandleReceiveNodeTaxDetails, which the table calls, fills the list, the tax labels and the funds); a purchase is answered with ClientShowCurrencyCallout (the price), one
//  ClientReceiveStorageEvent (Add) per item and the Currencies of the UOwnerInfoComponent.
// ============================================================================
#pragma once

#include "protocol/emit/intrepid_netguid.h"
#include "protocol/repl/rpc.h"
#include "protocol/repl/value.h"

#include <cstdint>
#include <optional>

namespace aoc { namespace protocol { namespace repl {

struct InteractRequest {
    emit::FIntrepidNetworkGUID interactable;   // the InteractableComponent
    int16_t                    counter = 0;
    uint16_t                   zone = 0;
    bool operator==(const InteractRequest&) const = default;
};
// [C.Y.B :: C.O.A]

/// ServerInteractWithInteractableComponent from a decoded call; nullopt for another RPC, an undecoded call or a parameter of another kind.
std::optional<InteractRequest> parse_interact_request(const RpcCall& call);
std::optional<RpcParams>       interact_params(const schema::RpcSchema& rpc, const InteractRequest& request);

/// ServerHandleVendorPurchase from a decoded call; nullopt as above (a call without a cart is the empty cart: nullopt too).
std::optional<VendorPurchaseCart>    parse_vendor_purchase(const RpcCall& call);
std::optional<RpcParams>             vendor_purchase_params(const schema::RpcSchema& rpc, const VendorPurchaseCart& cart);

/// ServerHandleVendorTransaction from a decoded call.
std::optional<VendorTransactionCart> parse_vendor_transaction(const RpcCall& call);
std::optional<RpcParams>             vendor_transaction_params(const schema::RpcSchema& rpc, const VendorTransactionCart& cart);

/// ClientOpenVendor's parameters (no unavailable listings). nullopt when `rpc` is not that RPC.
std::optional<RpcParams> open_vendor_params(const schema::RpcSchema& rpc, int64_t merch_set, const emit::FIntrepidNetworkGUID& vendor);

/// ServerGetNodeTaxTable's NodeGuid from a decoded call (0 when the client sent the default); nullopt for another RPC or an undecoded call.
std::optional<int64_t>   parse_node_tax_request(const RpcCall& call);
std::optional<RpcParams> node_tax_request_params(const schema::RpcSchema& rpc, int64_t node);

/// ClientSetNodeTaxTable's table, read back and as parameters.
std::optional<NodeTaxDetails> parse_node_tax_table(const RpcCall& call);
std::optional<RpcParams>      node_tax_table_params(const schema::RpcSchema& rpc, const NodeTaxDetails& details);

/// What ClientOpenVendor carries, read back.
struct OpenVendor {
    int64_t                    merch_set = 0;
    emit::FIntrepidNetworkGUID vendor;
    bool operator==(const OpenVendor&) const = default;
};
std::optional<OpenVendor> parse_open_vendor(const RpcCall& call);

}}}  // namespace aoc::protocol::repl
