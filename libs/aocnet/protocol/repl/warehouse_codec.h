// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/warehouse_codec.h  (storage clerks, step 0; header only)
//
//  FWarehouseItemChunkSummary and FWarehouseMaterialTabSummary as the wire carries them: the elements of ClientReceiveWarehouseSummary's three arrays.  The two structs have one layout (a
//  NetSerialize struct, a bit stream read in the order below; "ubool" is a bool written as an int32, 1 at most; every number is little endian):
//
//      int64   the chunk's or tab's record guid
//      ubool   bIsAvailable            whether the player may buy it now
//      int32   n, then n x             the costs, a map from a currency to an amount
//                int64   the currency's record guid
//                int64   the currency's record type id
//                double  the amount in the currency's own units (gold: 5.0 is five gold)
//
//  A free chunk or tab (the base ones, the cargo tab) lists no cost: 128 bits.  One with a cost is 128 + 192 bits.  The layout was read from the recorded summaries of a character in a
//  town of the fourth level (9 chunks, 10 material tabs, 1 cargo tab: 6,068 parameter bits, read to the last bit) and checked against the second summary of the same capture.
// ============================================================================
#pragma once

#include "protocol/emit/bunch_writer.h"
#include "protocol/repl/codecs.h"
#include "protocol/repl/value.h"
#include "protocol/wire/packet_reader.h"

#include <bit>
#include <optional>

namespace aoc { namespace protocol { namespace repl {

/// The most costs one chunk or tab lists (the data has one or two).
constexpr size_t kMaxWarehouseCosts = 8;

inline bool warehouse_entry_fits(const WarehouseSummaryEntry& e) { return e.costs.size() <= kMaxWarehouseCosts; }

inline std::optional<WarehouseSummaryEntry> read_warehouse_entry(wire::PacketReader& r) {
    WarehouseSummaryEntry e;
    e.record = static_cast<int64_t>(r.read_uint64());
    const uint32_t available = r.read_uint32();
    const uint32_t count = r.read_uint32();
    if (overran(r) || available > 1 || count > kMaxWarehouseCosts) return std::nullopt;
    e.available = available == 1;
    for (uint32_t i = 0; i < count; ++i) {
        WarehouseCost c;
        c.currency = static_cast<int64_t>(r.read_uint64());
        c.type_id = static_cast<int64_t>(r.read_uint64());
        c.value = std::bit_cast<double>(r.read_uint64());
        if (overran(r)) return std::nullopt;
        e.costs.push_back(c);
    }
    return e;
}

inline bool write_warehouse_entry(emit::BunchWriter& out, const WarehouseSummaryEntry& e) {
    if (!warehouse_entry_fits(e)) return false;
    out.write_uint64(static_cast<uint64_t>(e.record));
    out.write_uint32(e.available ? 1u : 0u);
    out.write_uint32(static_cast<uint32_t>(e.costs.size()));
    for (const WarehouseCost& c : e.costs) {
        out.write_uint64(static_cast<uint64_t>(c.currency));
        out.write_uint64(static_cast<uint64_t>(c.type_id));
        out.write_uint64(std::bit_cast<uint64_t>(c.value));
    }
    return true;
}

}}}  // namespace aoc::protocol::repl
