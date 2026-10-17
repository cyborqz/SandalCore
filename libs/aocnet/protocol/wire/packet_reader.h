// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/wire/packet_reader.h
//
//  Bit-level packet reader — the C++ port of Python's BR class from
//  phase1_parser.py.  Wraps a raw UDP payload buffer and exposes position-
//  tracking reads: read_bits, read_serialize_int, read_sip (SIP), etc.
//
//  Design notes:
//    * LSB-first per-byte (matches UE5 FBitReader convention).
//    * Read operations advance an internal position cursor.
//    * `peek_*` operations read without advancing.
//    * Out-of-range reads return 0 rather than throw — callers check
//      `overflowed()` after a read batch.
//    * Snapshot/restore pattern via `save()`/`restore()` for lookahead.
//
//  This class is the bit-reader foundation; packet_parser / bunch_parser
//  sit on top of it.
//
//  LAYER:  Protocol / wire
//  SESSION: A (the parser)
// ============================================================================
#pragma once

#include "protocol/wire/ue5_primitives.h"
#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>

namespace aoc { namespace protocol { namespace wire {

class PacketReader {
public:
    /// Construct from a raw UDP payload buffer.  The reader does NOT copy —
    /// `data` must outlive the reader.  `effective_bits` is the number of
    /// valid bits (usually from strip_termination); defaults to `byte_len * 8`.
    PacketReader(const uint8_t* data, size_t byte_len)
        : PacketReader(data, byte_len, capacity_bits(data, byte_len)) {}

    PacketReader(const uint8_t* data, size_t byte_len, size_t effective_bits)
        : data_(data), byte_len_(byte_len),
          effective_bits_(std::min(effective_bits, capacity_bits(data, byte_len))), pos_(0),
          overflowed_(effective_bits > capacity_bits(data, byte_len)) {}

    /// Read `n_bits` bits (max 64).  Advances position.  Returns 0 if overflow.
    uint64_t read_bits(int n_bits) {
        if (n_bits == 0) return 0;
        if (n_bits < 0 || n_bits > 64 || pos_ > effective_bits_ ||
            static_cast<size_t>(n_bits) > remaining_bits()) {
            overflowed_ = true;
            return 0;
        }
        uint64_t v = ::ue5::read_bits(data_, byte_len_, pos_, n_bits);
        return v;
    }

    /// Read a single bit (0 or 1).
    int read_bit() {
        return static_cast<int>(read_bits(1));
    }

    /// Read a little-endian unsigned integer of width `n_bytes` bytes.
    uint64_t read_uint(int n_bytes) {
        if (n_bytes < 0 || n_bytes > 8) { overflowed_ = true; return 0; }
        return read_bits(n_bytes * 8);
    }

    uint8_t  read_uint8()  { return static_cast<uint8_t >(read_bits(8)); }
    uint16_t read_uint16() { return static_cast<uint16_t>(read_bits(16)); }
    uint32_t read_uint32() { return static_cast<uint32_t>(read_bits(32)); }
    uint64_t read_uint64() { return read_bits(64); }

    /// UE5 SerializeInt — adaptive-length read in range [0, max_val).
    uint32_t read_serialize_int(uint32_t max_val) {
        uint32_t value = 0;
        for (uint32_t mask = 1; mask != 0 && value + mask < max_val; mask <<= 1) {
            const auto bit = read_bit();
            if (overflowed_) return 0;
            if (bit) value |= mask;
        }
        return value;
    }
// [Cyb + C O A]

    /// UE5 SerializeIntPacked — variable-length 7-bit-per-byte uint64.
    /// Returns std::nullopt if the read would overflow.
    std::optional<uint64_t> read_sip() {
        const size_t before = pos_;
        uint64_t value = 0;
        for (int i = 0; i < 10; ++i) {
            if (remaining_bits() < 8) break;
            const uint8_t byte = read_uint8();
            // The tenth byte has room for only the final bit of a uint64.
            if (i == 9 && (byte & 0xfe) > 2) break;
            value |= static_cast<uint64_t>(byte >> 1) << (i * 7);
            if ((byte & 1) == 0) return value;
        }
        pos_ = before;
        overflowed_ = true;
        return std::nullopt;
    }

    /// Read a UE5 FString.  Format:
    ///   int32 save_num
    ///     if save_num > 0  → save_num bytes of ASCII (includes NUL terminator)
    ///     if save_num < 0  → -save_num UCS-2 chars (includes NUL)
    ///     if save_num == 0 → empty string
    /// Returns std::nullopt on malformed/overflow.
    std::optional<std::string> read_fstring(int max_chars = 1024) {
        if (remaining_bits() < 32) { overflowed_ = true; return std::nullopt; }
        int32_t save_num = static_cast<int32_t>(read_uint32());
        if (save_num == 0) return std::string{};
        if (save_num == INT32_MIN) return std::nullopt;  // -save_num would overflow
        bool is_utf16 = save_num < 0;
        int32_t count = is_utf16 ? -save_num : save_num;
        if (count < 1 || count > max_chars) return std::nullopt;
        const size_t char_bits = is_utf16 ? 16 : 8;
        if (static_cast<size_t>(count) > remaining_bits() / char_bits) {
            overflowed_ = true;
            return std::nullopt;
        }
        std::string out;
        for (int i = 0; i < count; ++i) {
            const uint16_t c = static_cast<uint16_t>(read_bits(static_cast<int>(char_bits)));
            if (i == count - 1) {
                if (c != 0) return std::nullopt;
            } else {
                // Preserve the legacy UCS-2-to-ASCII conversion, consuming
                // exactly the declared field even if it contains an early NUL.
                out += is_utf16 && c >= 128 ? '?' : static_cast<char>(c);
            }
        }
        return out;
    }

    /// Current position in bits.
    size_t pos() const { return pos_; }
    void set_pos(size_t p) { pos_ = p; }

    /// Effective end of data in bits.
    size_t effective_bits() const { return effective_bits_; }

    /// Bits remaining to read.
    size_t remaining_bits() const {
        return pos_ < effective_bits_ ? effective_bits_ - pos_ : 0;
    }

    /// True if any previous read attempted to go past the end.
    bool overflowed() const { return overflowed_; }
    void clear_overflow() { overflowed_ = false; }

    /// Direct access to underlying buffer (for interop with older code).
    const uint8_t* data() const { return data_; }
    size_t byte_len() const { return byte_len_; }

    /// Snapshot the current position for later restore.
    struct Snapshot { size_t pos; bool overflowed; };
    Snapshot save() const { return {pos_, overflowed_}; }
    void restore(Snapshot s) { pos_ = s.pos; overflowed_ = s.overflowed; }

private:
    static size_t capacity_bits(const uint8_t* data, size_t byte_len) {
        return data && byte_len <= std::numeric_limits<size_t>::max() / 8 ? byte_len * 8 : 0;
    }
    const uint8_t* data_;
    size_t byte_len_;
    size_t effective_bits_;
    size_t pos_;
    bool overflowed_ = false;
};

}}} // namespace aoc::protocol::wire
