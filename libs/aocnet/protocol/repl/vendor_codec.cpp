// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/vendor_codec.cpp
//
//  See vendor_codec.h.
// ============================================================================
#include "protocol/repl/vendor_codec.h"

#include "protocol/repl/codecs.h"

#include <bit>

namespace aoc { namespace protocol { namespace repl {

namespace {

using emit::BunchWriter;
using wire::PacketReader;

constexpr size_t kItemMinBits = 64 + 32 + 32 + 32;    // an item with both optionals unset
constexpr size_t kInstanceBits = 128;

/// A TOptional<int64>: a UBOOL (32 bits, at most 1), then 64 bits when set.
bool read_optional(PacketReader& r, std::optional<int64_t>& out) {
    const uint32_t set = r.read_uint32();
    if (overran(r) || set > 1) return false;
    if (set) out = static_cast<int64_t>(r.read_uint64());
    return !overran(r);
}

void write_optional(BunchWriter& out, const std::optional<int64_t>& v) {
    out.write_uint32(v ? 1u : 0u);
    if (v) out.write_uint64(static_cast<uint64_t>(*v));
}
// ~ C.O.A :: C:Y:B ~

constexpr size_t kTaxEntryMinBits = 32 + 64 + 32 + 32 + 32 + 32 + 32;   // an entry with an empty tag

bool tag_fits(const std::string& tag) {
    if (tag.size() > kMaxNodeTaxTagChars) return false;
    for (const char c : tag) {
        if (c == 0 || static_cast<unsigned char>(c) > 0x7e) return false;
    }
    return true;
}

bool read_bool(PacketReader& r, bool& out) {
    const uint32_t v = r.read_uint32();
    if (overran(r) || v > 1) return false;
    out = v != 0;
    return true;
}

}  // namespace

bool node_tax_details_fit(const NodeTaxDetails& details) {
    if (details.entries.size() > kMaxNodeTaxEntries) return false;
    for (const NodeTaxEntry& e : details.entries) {
        if (!tag_fits(e.tag)) return false;
    }
    return true;
}

std::optional<NodeTaxDetails> read_node_tax_details(PacketReader& r) {
    NodeTaxDetails d;
    d.node = static_cast<int64_t>(r.read_uint64());
    const int32_t n = static_cast<int32_t>(r.read_uint32());
    if (overran(r) || n < 0 || static_cast<size_t>(n) > kMaxNodeTaxEntries || static_cast<size_t>(n) * kTaxEntryMinBits > r.remaining_bits()) return std::nullopt;
    d.entries.reserve(static_cast<size_t>(n));
    for (int32_t i = 0; i < n; ++i) {
        NodeTaxEntry e;
        const int32_t save_num = static_cast<int32_t>(r.read_uint32());
        // an ANSI FString: SaveNum characters with the NUL (0: empty); a negative one is UTF-16, which no tag uses
        if (overran(r) || save_num < 0 || static_cast<size_t>(save_num) > kMaxNodeTaxTagChars + 1 || static_cast<size_t>(save_num) * 8 > r.remaining_bits()) return std::nullopt;
        if (save_num > 0) {
            e.tag.resize(static_cast<size_t>(save_num) - 1);
            for (char& c : e.tag) c = static_cast<char>(r.read_uint8());
            if (r.read_uint8() != 0 || overran(r) || !tag_fits(e.tag)) return std::nullopt;
        }
        e.details = static_cast<int64_t>(r.read_uint64());
        e.flat_tax = static_cast<int32_t>(r.read_uint32());
        e.tax_percent = std::bit_cast<float>(r.read_uint32());
        e.owning_node_percent = std::bit_cast<float>(r.read_uint32());
        if (!read_bool(r, e.flag_a) || !read_bool(r, e.flag_b)) return std::nullopt;
        d.entries.push_back(std::move(e));
    }
    if (overran(r)) return std::nullopt;
    return d;
}

bool write_node_tax_details(BunchWriter& out, const NodeTaxDetails& details) {
    if (!node_tax_details_fit(details)) return false;
    out.write_uint64(static_cast<uint64_t>(details.node));
    out.write_uint32(static_cast<uint32_t>(details.entries.size()));
    for (const NodeTaxEntry& e : details.entries) {
        out.write_uint32(e.tag.empty() ? 0u : static_cast<uint32_t>(e.tag.size() + 1));
        if (!e.tag.empty()) {
            for (const char c : e.tag) out.write_uint8(static_cast<uint8_t>(c));
            out.write_uint8(0);
        }
        out.write_uint64(static_cast<uint64_t>(e.details));
        out.write_uint32(static_cast<uint32_t>(e.flat_tax));
        out.write_uint32(std::bit_cast<uint32_t>(e.tax_percent));
        out.write_uint32(std::bit_cast<uint32_t>(e.owning_node_percent));
        out.write_uint32(e.flag_a ? 1u : 0u);
        out.write_uint32(e.flag_b ? 1u : 0u);
    }
    return true;
}

bool vendor_purchase_cart_fits(const VendorPurchaseCart& cart) { return cart.items.size() <= kMaxVendorCartItems; }

std::optional<VendorPurchaseCart> read_vendor_purchase_cart(PacketReader& r) {
    VendorPurchaseCart c;
    c.vendor = static_cast<int64_t>(r.read_uint64());
    const int32_t n = static_cast<int32_t>(r.read_uint32());
    if (overran(r) || n < 0 || static_cast<size_t>(n) > kMaxVendorCartItems || static_cast<size_t>(n) * kItemMinBits > r.remaining_bits()) return std::nullopt;
    c.items.reserve(static_cast<size_t>(n));
    for (int32_t i = 0; i < n; ++i) {
        VendorCartItem item;
        item.item_record = static_cast<int64_t>(r.read_uint64());
        item.quantity = static_cast<int32_t>(r.read_uint32());
        if (!read_optional(r, item.optional_a) || !read_optional(r, item.optional_b)) return std::nullopt;
        c.items.push_back(item);
    }
    if (overran(r)) return std::nullopt;
    return c;
}

bool write_vendor_purchase_cart(BunchWriter& out, const VendorPurchaseCart& cart) {
    if (!vendor_purchase_cart_fits(cart)) return false;
    out.write_uint64(static_cast<uint64_t>(cart.vendor));
    out.write_uint32(static_cast<uint32_t>(cart.items.size()));
    for (const VendorCartItem& item : cart.items) {
        out.write_uint64(static_cast<uint64_t>(item.item_record));
        out.write_uint32(static_cast<uint32_t>(item.quantity));
        write_optional(out, item.optional_a);
        write_optional(out, item.optional_b);
    }
    return true;
}

bool vendor_transaction_cart_fits(const VendorTransactionCart& cart) { return cart.instances.size() <= kMaxVendorTransactionItems; }

std::optional<VendorTransactionCart> read_vendor_transaction_cart(PacketReader& r) {
    VendorTransactionCart c;
    c.vendor = static_cast<int64_t>(r.read_uint64());
    const int32_t n = static_cast<int32_t>(r.read_uint32());
    if (overran(r) || n < 0 || static_cast<size_t>(n) > kMaxVendorTransactionItems || static_cast<size_t>(n) * kInstanceBits > r.remaining_bits()) return std::nullopt;
    c.instances.reserve(static_cast<size_t>(n));
    for (int32_t i = 0; i < n; ++i) {
        GuidValue g;
        g.a = r.read_uint32();
        g.b = r.read_uint32();
        g.c = r.read_uint32();
        g.d = r.read_uint32();
        c.instances.push_back(g);
    }
    c.operation = r.read_uint8();
    if (overran(r)) return std::nullopt;
    return c;
}
// Cyb.C | O | A

bool write_vendor_transaction_cart(BunchWriter& out, const VendorTransactionCart& cart) {
    if (!vendor_transaction_cart_fits(cart)) return false;
    out.write_uint64(static_cast<uint64_t>(cart.vendor));
    out.write_uint32(static_cast<uint32_t>(cart.instances.size()));
    for (const GuidValue& g : cart.instances) {
        out.write_uint32(g.a);
        out.write_uint32(g.b);
        out.write_uint32(g.c);
        out.write_uint32(g.d);
    }
    out.write_uint8(cart.operation);
    return true;
}

}}}  // namespace aoc::protocol::repl
