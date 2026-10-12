// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/emit/bunch_writer.h
//
//  Bit-level bunch writer — the producer counterpart to `wire::PacketReader`.
//  Owns an internal std::vector<uint8_t> and accumulates bit-level output.
//
//  This is the single canonical writer; classes like Writer (ue5_replication.h)
//  and BunchBuffer (bunch_builder.h) still exist for compatibility with
//  existing code, but new emit/ code uses BunchWriter exclusively.
//
//  LAYER:  Protocol / emit
//  SESSION: C
// ============================================================================
#pragma once

#include "protocol/wire/ue5_primitives.h"
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace aoc { namespace protocol { namespace emit {

class BunchWriter {
public:
    explicit BunchWriter(size_t initial_capacity_bytes = 512) {
        buffer_.reserve(initial_capacity_bytes);
    }

    /// Write a single bit (0 or 1).  LSB-first within byte.
    void write_bit(int value) { write_bits(value ? 1u : 0u, 1); }

    /// Write `n_bits` of `value` (LSB-first).
    void write_bits(uint64_t value, int n_bits) {
        ensure_capacity(bit_pos_ + n_bits);
        ::ue5::write_bits(buffer_.data(), buffer_.capacity(), bit_pos_, value, n_bits);
        // Track logical size (bytes).  bit_pos_ was advanced by write_bits.
        update_size();
    }

    void write_uint8(uint8_t v)   { write_bits(v, 8); }
    void write_uint16(uint16_t v) { write_bits(v, 16); }
    void write_uint32(uint32_t v) { write_bits(v, 32); }
    void write_uint64(uint64_t v) { write_bits(v, 64); }
    void write_int32(int32_t v)   { write_bits(static_cast<uint32_t>(v), 32); }

    /// UE5 SerializeIntPacked — 7-bit-per-byte varint for up to 64-bit values.
    void write_sip(uint64_t value) {
        ensure_capacity(bit_pos_ + 80);  // up to 10 bytes
        ::ue5::write_sip(buffer_.data(), buffer_.capacity(), bit_pos_, value);
        update_size();
    }

    /// UE5 SerializeInt — adaptive-length bounded int.
    void write_serialize_int(uint32_t value, uint32_t max_val) {
        ensure_capacity(bit_pos_ + 32);
        ::ue5::write_serialize_int(buffer_.data(), buffer_.capacity(), bit_pos_, value, max_val);
        update_size();
    }

    /// UE5 FString: int32 length (including NUL) + ASCII bytes + NUL terminator.
    /// For ASCII strings only; UCS-2 path requires negative length + 16-bit chars.
    void write_fstring_ansi(const std::string& s) {
        int32_t save_num = static_cast<int32_t>(s.size()) + 1;  // +1 for NUL
        write_int32(save_num);
        for (char c : s) write_uint8(static_cast<uint8_t>(c));
        write_uint8(0);  // NUL terminator
    }
// [Cyb x C-O-A]

    /// Raw byte sequence write.  Each byte emitted LSB-first.
    void write_bytes(const uint8_t* data, size_t n) {
        for (size_t i = 0; i < n; ++i) write_uint8(data[i]);
    }

    /// Write arbitrary bit sequence from a source buffer.
    void write_bit_range(const uint8_t* src, size_t src_bit_off, size_t n_bits) {
        for (size_t i = 0; i < n_bits; ++i) {
            size_t bp = src_bit_off + i;
            int bit = (src[bp >> 3] >> (bp & 7)) & 1;
            write_bit(bit);
        }
    }

    /// Current output in bits.
    size_t bit_pos() const { return bit_pos_; }

    /// Current output in bytes (ceiling).
    size_t byte_size() const { return (bit_pos_ + 7) / 8; }

    /// Access the underlying byte buffer.  Note: valid bits are [0..bit_pos()).
    const std::vector<uint8_t>& bytes() const { return buffer_; }
    const uint8_t* data() const { return buffer_.data(); }

    /// Reset to an empty state, reusing the buffer capacity.
    void reset() { bit_pos_ = 0; buffer_.clear(); }

    /// Drops everything written after `bits` (which must not exceed
    /// bit_pos()), e.g. to take back a partly written block.
    void truncate(size_t bits) {
        if (bits >= bit_pos_) return;
        bit_pos_ = bits;
        const size_t keep = (bits + 7) / 8;
        if (bits % 8) buffer_[keep - 1] &= static_cast<uint8_t>((1u << (bits % 8)) - 1);
        buffer_.resize(keep);
    }

    /// Splice in raw bits from a pre-rendered buffer (byte-aligned).
    /// Convenience for "splice captured bytes into builder output" — used by
    /// player_controller.cpp's 3.7-era builder.  Can be removed when all
    /// bytes are generated from schema.
    void splice_bytes_at_bit(const uint8_t* src, size_t src_bit_off, size_t n_bits) {
        write_bit_range(src, src_bit_off, n_bits);
    }

private:
    std::vector<uint8_t> buffer_;
    size_t bit_pos_ = 0;

    /// Ensure the internal buffer has enough bytes to hold `required_bits`.
    void ensure_capacity(size_t required_bits) {
        size_t needed_bytes = (required_bits + 7) / 8 + 4;  // +4 slack
        if (buffer_.size() < needed_bytes) {
            buffer_.resize(needed_bytes, 0);
        }
    }

    void update_size() {
        size_t needed_bytes = (bit_pos_ + 7) / 8;
        if (buffer_.size() < needed_bytes) buffer_.resize(needed_bytes, 0);
    }
};

}}} // namespace aoc::protocol::emit
