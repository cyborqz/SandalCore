// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/vendor_codec.h
//
//  The NetSerialize codecs of the two vendor carts (plan E1): FVendorPurchaseCart, the parameter of the client's
//  AAoCPlayerController::ServerHandleVendorPurchase, and FVendorTransactionCart, the parameter of ServerHandleVendorTransaction.
//  codecs.h's read_struct / write_struct reach them by the struct names; the functions here are the same codec for a caller that holds a
//  reader.  Every layout is the client exe's own (AOCClient-Win64-Shipping.exe, read-only), in the order the code runs, bit stream LSB first.
//
//  FVendorPurchaseCart, NetSerialize slot 10 of its TCppStructOps vtable -> thunk -> 0x146461550
//    64      VendorRecordGuid (the VendorDef the window shows: ClientOpenVendor's MerchSetId)
//    32      ItemCart.Num (an int32; the exe refuses a negative one and, for a map-sized array, one above 0x55555)
//    per item (FVendorPurchaseCartItem, 0x30 bytes in memory, serialized by 0x14643ede0)
//      64      ItemRecordGuid
//      32      Quantity
//      32 + 64 a TOptional<int64> (0x14593e9e0: a UBOOL "is set", 32 bits and at most 1, then the value when set)
//      32 + 64 a second one
//    A parameter of this struct type goes out as a send bit (always 1: rpc.h) and these bits.
//
//    [observed, captures 863, 1531 and 1536 of the retail set: 1 item (257 bits), 1 item whose first optional is set (321), 6 items (1,057)].
//    A purchase from a full window of the retail client is one item of quantity 1; the six-item cart is a multi-select.  The meaning of the
//    two optionals is not known (the one set value in the captures is an item record id); worldd treats a cart that sets either as one it
//    reads and prices as any other.
//
//  FVendorTransactionCart (a sale to the vendor or a buy-back), the captured form: capture 716's 233 bits are
//    64      VendorRecordGuid
//    32      ItemInstanceGuids.Num
//    per instance  128 (an FGuid: four int32)
//    8       Operation (EVendorTransactionOperation, a byte)
//  and `Num * 128 + 105` bits plus the send bit: the layout is read from that one capture, not from the exe.
//
//  FNodeTaxDetails (fix 4), the parameter of the server's ClientSetNodeTaxTable: the table of a node's taxes the vendor window waits for.
//  NetSerialize slot 10 of its TCppStructOps vtable -> 0x145b6b4c0: 64 bits NodeGuid, then the table (0x145b6e540) as
//    32      Num (an int32)
//    per entry (observed in captures 716 and 1536: 78 entries each, 6,817 bytes, the same 78 tags; 54,537 bits with the send bit)
//      32 + 8*n   the tag as an FString: SaveNum = n (the characters with their NUL; 0 for an empty string, negative for UTF-16, which is refused here)
//      64         the TaxDetails record guid
//      32         flat tax (an int32)
//      32 + 32    the tax percent and the owning node's percent (floats)
//      32 + 32    two bools (a UBOOL each, 0 or 1)
//  The layout is read from the captures and the exe's functions (the entry's field order from its bytes: the record guid, the record's own
//  default values 0 / 0.5 / 0.1 / 1 show in the floats).
// ============================================================================
#pragma once
// (Cyb : C O A)

#include "protocol/emit/bunch_writer.h"
#include "protocol/repl/value.h"
#include "protocol/wire/packet_reader.h"

#include <optional>

namespace aoc { namespace protocol { namespace repl {

/// The most items a purchase cart carries here (the retail client's window shows a few dozen; the server refuses a longer one).
constexpr size_t kMaxVendorCartItems = 255;
/// The most instances a transaction cart carries.
constexpr size_t kMaxVendorTransactionItems = 255;

std::optional<VendorPurchaseCart>    read_vendor_purchase_cart(wire::PacketReader& r);
bool                                 write_vendor_purchase_cart(emit::BunchWriter& out, const VendorPurchaseCart& cart);
bool                                 vendor_purchase_cart_fits(const VendorPurchaseCart& cart);

/// The most entries a tax table carries here (retail's has 78) and the longest tag (characters, without the NUL).
constexpr size_t kMaxNodeTaxEntries = 512;
constexpr size_t kMaxNodeTaxTagChars = 255;

std::optional<NodeTaxDetails>        read_node_tax_details(wire::PacketReader& r);
bool                                 write_node_tax_details(emit::BunchWriter& out, const NodeTaxDetails& details);
bool                                 node_tax_details_fit(const NodeTaxDetails& details);

std::optional<VendorTransactionCart> read_vendor_transaction_cart(wire::PacketReader& r);
bool                                 write_vendor_transaction_cart(emit::BunchWriter& out, const VendorTransactionCart& cart);
bool                                 vendor_transaction_cart_fits(const VendorTransactionCart& cart);

}}}  // namespace aoc::protocol::repl
