// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/emit/intrepid_netguid.h
//
//  AoC-specific 128-bit NetGUID structure and serialisation helpers.
//
//  Stock UE5's `FNetworkGUID` is a 32-bit integer serialised via SerializeIntPacked.
//  AoC REPLACED it with `FIntrepidNetworkGUID` — a 16-byte struct representing
//  a distributed NetGUID across their cross-server architecture.
//
//  Source: RE of `UIntrepidNetServerPackageMap::InternalLoadObject` + helper
//          `sub_14141E960` in AOCClient-Win64-Shipping.exe.  The log format
//          string in InternalLoadObject leaks the field names directly:
//              "ObjectId: %llu | ServerId: %u | Randomizer: %u"
//          And `sub_14141E960` (the archive reader) is literally 4 consecutive
//          4-byte reads with no bit-packing — confirming fixed 128-bit layout.
//
//  Wire format (little-endian, byte-aligned within archive):
//     offset 0 : uint32  ObjectId_Low
//     offset 4 : uint32  ObjectId_High     → together form uint64 ObjectId
//     offset 8 : uint32  ServerId
//     offset 12: uint32  Randomizer
//     (total: 16 bytes = 128 bits)
//
//  In a BIT-packed bunch payload, these 128 bits are written contiguously.
//  If the write position is not byte-aligned, the 128 bits are still emitted
//  as-is (each uint32 as 32 bits LSB-first within the bitstream).
//
//  LAYER:   Protocol / emit
//  SESSION: H.3d (RE-driven byte-identity work)
// ============================================================================
#pragma once

#include "protocol/emit/bunch_writer.h"
#include "protocol/wire/ue5_primitives.h"
#include <cstdint>

namespace aoc { namespace protocol { namespace emit {

/// AoC's distributed NetGUID.  Replaces stock UE5's `FNetworkGUID` uint32.
///
/// Field meanings (from RE):
///  - `ObjectId`    — the primary handle (analog to stock FNetworkGUID)
///  - `ServerId`    — which backend server owns/authors this object (0 for
///                    bootstrap/default objects, non-zero for inter-server refs)
///  - `Randomizer`  — collision-avoidance salt (always 0 for captured bootstrap)
struct FIntrepidNetworkGUID {
    uint64_t ObjectId   = 0;
    uint32_t ServerId   = 0;
    uint32_t Randomizer = 0;

    bool operator==(const FIntrepidNetworkGUID&) const = default;

    bool is_valid() const { return ObjectId != 0; }
    bool is_default() const { return ObjectId == 0 && ServerId == 0 && Randomizer == 0; }

    /// Convenience constructor from a plain uint64 (for compat with the
    /// existing stock-UE5 32-bit NetGUID code paths).  ServerId + Randomizer
    /// default to 0 which matches how the captured bootstrap serialises
    /// "local-server bootstrap" GUIDs.
    static FIntrepidNetworkGUID from_u64(uint64_t obj) {
        FIntrepidNetworkGUID g;
        g.ObjectId = obj;
        return g;
    }
};
// (CoA + C | Y | B)

/// Write an FIntrepidNetworkGUID into the bunch bit-stream.
/// Matches the layout `sub_14141E960` reads: 4 consecutive uint32s LSB-first.
inline void write_intrepid_guid(BunchWriter& bw, const FIntrepidNetworkGUID& g) {
    // Split uint64 ObjectId into low/high 32-bit halves (matches the 4-DWORD
    // read order in sub_14141E960 — decomp shows it reads 4 consecutive DWORDs).
    const uint32_t obj_lo = static_cast<uint32_t>(g.ObjectId);
    const uint32_t obj_hi = static_cast<uint32_t>(g.ObjectId >> 32);
    bw.write_uint32(obj_lo);
    bw.write_uint32(obj_hi);
    bw.write_uint32(g.ServerId);
    bw.write_uint32(g.Randomizer);
}

/// Read an FIntrepidNetworkGUID from a bit-stream at `pos`.  Advances `pos`
/// by exactly 128 bits.  Mirror of `sub_14141E960`.
inline FIntrepidNetworkGUID read_intrepid_guid(const uint8_t* data, size_t buf_len,
                                                  size_t& pos) {
    FIntrepidNetworkGUID g;
    const uint32_t obj_lo = static_cast<uint32_t>(
        ::ue5::read_bits(data, buf_len, pos, 32));
    const uint32_t obj_hi = static_cast<uint32_t>(
        ::ue5::read_bits(data, buf_len, pos, 32));
    g.ObjectId  = static_cast<uint64_t>(obj_lo) |
                  (static_cast<uint64_t>(obj_hi) << 32);
    g.ServerId  = static_cast<uint32_t>(
        ::ue5::read_bits(data, buf_len, pos, 32));
    g.Randomizer = static_cast<uint32_t>(
        ::ue5::read_bits(data, buf_len, pos, 32));
    return g;
}

}}} // namespace aoc::protocol::emit
