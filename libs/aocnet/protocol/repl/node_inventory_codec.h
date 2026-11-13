// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/node_inventory_codec.h
//
//  FNodeInventoryData (ClientSendNodeInventoryData), a node's stored items and money: its own NetSerialize, read from the client (the struct's NetSerialize; the struct
//  is 0x60 bytes and has no reflected member).  An archive: raw words, LSB first.
//
//    64                        +0x00 int64: the node (the client's cache counts the data as held while it is not 0)
//    32 + n x (64 + 32)        +0x08 a TMap<int64, int32>: an int32 count, then each key (an item record) and its quantity
//    64                        +0x58 int64: the node's Money in copper [modelled: no code of the node window reads it; the data's NodeInventoryContents is CurrencyValues
//                              (Money) and StoredItems, and this is the one other word]
// ============================================================================
#pragma once

#include "protocol/emit/bunch_writer.h"
#include "protocol/repl/value.h"
#include "protocol/wire/packet_reader.h"

#include <cstdint>
#include <optional>

namespace aoc::protocol::repl {

/// The most items a node's inventory sends (the data's commodities are 39; a TMap has no limit of its own).
constexpr size_t kMaxNodeInventoryItems = 4096;

inline bool node_inventory_fits(const NodeInventoryData& d) { return d.items.size() <= kMaxNodeInventoryItems; }

inline bool write_node_inventory(emit::BunchWriter& out, const NodeInventoryData& d) {
    if (!node_inventory_fits(d)) return false;
    out.write_uint64(static_cast<uint64_t>(d.node));
    out.write_uint32(static_cast<uint32_t>(d.items.size()));
    for (const auto& [item, quantity] : d.items) {
        out.write_uint64(static_cast<uint64_t>(item));
        out.write_int32(quantity);
    }
    out.write_uint64(static_cast<uint64_t>(d.money));
    return true;
}

inline std::optional<NodeInventoryData> read_node_inventory(wire::PacketReader& r) {
    NodeInventoryData d;
    d.node = static_cast<int64_t>(r.read_uint64());
    const uint32_t n = r.read_uint32();
    if (n > kMaxNodeInventoryItems) return std::nullopt;
    d.items.reserve(n);
    for (uint32_t i = 0; i < n && !r.overflowed(); ++i) {
        const int64_t item = static_cast<int64_t>(r.read_uint64());
        const int32_t quantity = static_cast<int32_t>(r.read_uint32());
        d.items.emplace_back(item, quantity);
    }
    d.money = static_cast<int64_t>(r.read_uint64());
    if (r.overflowed()) return std::nullopt;
    return d;
}

}  // namespace aoc::protocol::repl
