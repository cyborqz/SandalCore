// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/wire/ue5_primitives.h
//
//  Canonical UE5 wire-format primitives.  Consolidates the `write_sip` /
//  `read_sip` / `read_bits` helpers that used to live in three separate
//  places:
//
//      net/game_server.h        (standalone ue5::write_sip)
//      net/ue5_replication.h    (Writer::write_sip, Writer::write_sip64)
//      protocol/bunch_builder.h (BunchBuffer::write_sip)
//
//  All three had identical encoding logic.  This file is now the ONE source
//  of truth.  Downstream code includes this header and calls `ue5::*`.
//
//  This header is header-only and dependency-free beyond <cstdint>, <string>,
//  <cstring>, <sstream>, <iomanip> — so it can be included from anywhere
//  (wire/, protocol/, net/, services/, tools/) without transitive bloat.
//
//  LAYER:  Protocol / wire
//  OWNED BY: The new 4-layer architecture.  See docs/live-server-
//            implementation-plan.md Session A.
// ============================================================================
#pragma once

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace aoc { namespace protocol { namespace wire {} } }

// ─── Compatibility namespace ──────────────────────────────────────────────
// Historical code uses the global `ue5::` namespace.  All symbols here
// are exposed there; new code should prefer `aoc::protocol::wire::` but
// both are identical.
// -----------------------------------------------------------------------------

namespace ue5 {

// ─── Bit-level I/O (LSB-first per byte, matches UE5 FBitReader/FBitWriter) ──
// ~ C:Y:B & CoA ~

/// Read `count` bits from buffer starting at bit offset, LSB-first per byte.
/// Advances `bit_off` by `count` bits.
inline uint64_t read_bits(const uint8_t* data, size_t data_len,
                          size_t& bit_off, int count) {
    uint64_t val = 0;
    for (int i = 0; i < count; ++i) {
        size_t byte_idx = (bit_off + i) / 8;
        int    bit_idx  = (bit_off + i) % 8;
        if (byte_idx < data_len) {
            val |= static_cast<uint64_t>((data[byte_idx] >> bit_idx) & 1) << i;
        }
    }
    bit_off += count;
    return val;
}

/// Write `count` bits into buffer at bit offset, LSB-first per byte.
/// Advances `bit_off` by `count` bits.  OR'd into the target byte — caller
/// is responsible for zeroing buffer if a fresh write is needed.
inline void write_bits(uint8_t* data, size_t data_cap,
                       size_t& bit_off, uint64_t value, int count) {
    for (int i = 0; i < count; ++i) {
        size_t byte_idx = (bit_off + i) / 8;
        int    bit_idx  = (bit_off + i) % 8;
        if (byte_idx < data_cap) {
            if ((value >> i) & 1)
                data[byte_idx] |= (1 << bit_idx);
            else
                data[byte_idx] &= ~(1 << bit_idx);
        }
    }
    bit_off += count;
}

/// Read `count` bits from buffer at a specific offset WITHOUT advancing.
inline uint64_t read_bits_at(const uint8_t* data, size_t data_len,
                             size_t bit_off, int count) {
    size_t tmp = bit_off;
    return read_bits(data, data_len, tmp, count);
}

/// Patch `count` bits in buffer at bit offset WITHOUT advancing.
/// (Same as write_bits but takes a non-reference offset.)
inline void patch_bits(uint8_t* data, size_t data_cap,
                      size_t bit_off, uint64_t value, int count) {
    size_t tmp = bit_off;
    write_bits(data, data_cap, tmp, value, count);
}

// ─── Termination (UE5 PacketHandler eff_bits marker) ─────────────────────────

/// Strip the UE5 PacketHandler termination bit.  Returns effective bit count
/// (everything BEFORE the trailing '1' + zero-padding).
inline size_t strip_termination(const uint8_t* data, size_t len) {
    if (len == 0) return 0;
    uint8_t last = data[len - 1];
    size_t bits = len * 8;
    if (last != 0) {
        bits--;
        while (!(last & 0x80)) {
            last <<= 1;
            bits--;
        }
    }
    return bits;
}

/// Append the termination '1' bit and pad remaining bits in the last byte
/// to zero.  Returns new byte length.
inline size_t add_termination(uint8_t* data, size_t data_cap, size_t bit_off) {
    write_bits(data, data_cap, bit_off, 1, 1);
    size_t pad = (8 - (bit_off % 8)) % 8;
    if (pad > 0) write_bits(data, data_cap, bit_off, 0, static_cast<int>(pad));
    return bit_off / 8;
}

// ─── Adaptive SerializeInt (UE5 FArchive::SerializeInt) ──────────────────────

/// UE5 SerializeInt — reads `value` in range [0, max_val).  Uses CeilLog2
/// adaptive bit count (stops early if value + current mask exceeds max).
/// Consumes 1..32 bits depending on max_val.
inline uint32_t read_serialize_int(const uint8_t* data, size_t data_len,
                                   size_t& off, uint32_t max_val) {
    if (max_val <= 1) return 0;
    uint32_t value = 0;
    uint32_t mask  = 1;
    while (value + mask < max_val && mask != 0) {
        if (read_bits(data, data_len, off, 1))
            value |= mask;
        mask <<= 1;
    }
    return value;
}

/// Write SerializeInt.  The inverse of read_serialize_int.  Writes the number
/// of bits required to encode `value` within [0, max_val).
inline void write_serialize_int(uint8_t* data, size_t data_cap, size_t& off,
                                uint32_t value, uint32_t max_val) {
    if (max_val <= 1) return;
    uint32_t new_val = 0;
    uint32_t mask = 1;
    while (new_val + mask < max_val && mask != 0) {
        write_bits(data, data_cap, off, (value & mask) ? 1 : 0, 1);
        if (value & mask) new_val |= mask;
        mask <<= 1;
    }
}

// ─── SerializeIntPacked (variable-length byte-based varint) ──────────────────

/// UE5 SerializeIntPacked — write uint64 as 1..10 bytes.
/// Each byte: bit0=continuation, bits 1..7 = 7 data bits (LSB-first chunks).
/// Matches FArchive::SerializeIntPacked for uint32 and uint64 values.
/// Advances `off` by 8 bits per byte written.
inline void write_sip(uint8_t* buf, size_t cap, size_t& off, uint64_t value) {
    do {
        uint8_t byte = static_cast<uint8_t>((value & 0x7F) << 1);
        value >>= 7;
        if (value > 0) byte |= 1; // continuation bit
        write_bits(buf, cap, off, byte, 8);
    } while (value > 0);
}
// (C-O-A | Cyb)

/// UE5 SerializeIntPacked — read uint64 starting at bit offset `off`.
/// Caps at 10 bytes (64-bit precision limit).
/// Advances `off` by 8 bits per byte read.
inline uint64_t read_sip(const uint8_t* data, size_t data_len, size_t& off) {
    uint64_t value = 0;
    int shift = 0;
    for (int i = 0; i < 10; ++i) {
        uint8_t bv = static_cast<uint8_t>(read_bits(data, data_len, off, 8));
        value |= static_cast<uint64_t>(bv >> 1) << shift;
        if ((bv & 1) == 0) break;
        shift += 7;
        if (shift >= 64) break;
    }
    return value;
}

// ─── Utility ─────────────────────────────────────────────────────────────────

/// Parse FGuid from 16 LE bytes → 32-char lowercase hex string.
inline std::string guid_to_hex(const uint8_t* b) {
    char buf[33];
    uint32_t a, bv, c, d;
    std::memcpy(&a,  b + 0,  4);
    std::memcpy(&bv, b + 4,  4);
    std::memcpy(&c,  b + 8,  4);
    std::memcpy(&d,  b + 12, 4);
    std::snprintf(buf, sizeof(buf), "%08x%08x%08x%08x", a, bv, c, d);
    return std::string(buf);
}

/// Hex dump for logging.  Caps at `max` bytes to avoid log bloat.
inline std::string hex_dump(const uint8_t* data, size_t len, size_t max = 256) {
    std::ostringstream oss;
    size_t show = std::min(len, max);
    for (size_t i = 0; i < show; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<unsigned>(data[i]);
        if (i + 1 < show) oss << ' ';
    }
    if (len > max) oss << " ... (" << len << "B total)";
    return oss.str();
}

} // namespace ue5

// ─── Forwarding into aoc::protocol::wire ──
// New-style preferred namespace.  Same functions.
namespace aoc { namespace protocol { namespace wire {
    using ::ue5::read_bits;
    using ::ue5::write_bits;
    using ::ue5::read_bits_at;
    using ::ue5::patch_bits;
    using ::ue5::strip_termination;
    using ::ue5::add_termination;
    using ::ue5::read_serialize_int;
    using ::ue5::write_serialize_int;
    using ::ue5::write_sip;
    using ::ue5::read_sip;
    using ::ue5::guid_to_hex;
    using ::ue5::hex_dump;
}}} // namespace aoc::protocol::wire
