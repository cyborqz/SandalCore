// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/bunch_builder.h
//
//  Low-level bit-stream writer used by actor builders to emit UE5 bunch
//  payload bytes.  Mirrors the semantics of UE5's FBitWriter (LSB-first
//  within each byte, bit-granular append, grows automatically).
//
//  All actor builders (player_controller, characters, npcs, ...) write
//  into a BunchBuffer via this interface.  Higher-level helpers
//  (write_sip, write_fstring, write_compressed_rotator, ...) are thin
//  wrappers around the primitive write_bits() call.
//
//  Design choice: we keep this module standalone — no dependency on
//  game_server.h — so it can be unit-tested in isolation, and so the
//  protocol/ module stays portable (could compile outside the server
//  if needed for tools).
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace aoc { namespace protocol {

/// A bit-granular, append-only write buffer.  Grows automatically as
/// bits are pushed.  When the buffer is done, `bytes()` returns the
/// packed byte representation and `bit_count()` returns the exact
/// number of valid bits (may be non-multiple of 8).
class BunchBuffer {
public:
    BunchBuffer() = default;

    /// Append `n` low-order bits of `value` to the buffer.  Bit 0 of
    /// `value` is emitted first (LSB-first within byte, matching UE5's
    /// FBitWriter).
    void write_bits(uint64_t value, int n) {
        // Ensure capacity: round up to the byte containing the last bit.
        const std::size_t needed = (bit_count_ + static_cast<std::size_t>(n) + 7) / 8;
        if (data_.size() < needed) data_.resize(needed, 0);
        for (int i = 0; i < n; ++i) {
            const uint64_t bit = (value >> i) & 1ULL;
            if (bit) {
                const std::size_t bp = bit_count_ + static_cast<std::size_t>(i);
                data_[bp >> 3] |= static_cast<uint8_t>(1u << (bp & 7));
            }
        }
        bit_count_ += static_cast<std::size_t>(n);
    }

    /// Append a raw byte array (bit-accurate).  `n_bits` can be less
    /// than `src.size() * 8` if the source tail is partial.
    void write_raw_bits(const uint8_t* src, std::size_t src_byte_count,
                       std::size_t n_bits) {
        for (std::size_t i = 0; i < n_bits; ++i) {
            const std::size_t sb = i >> 3;
            if (sb >= src_byte_count) break;
            const uint64_t bit = (src[sb] >> (i & 7)) & 1ULL;
            write_bits(bit, 1);
        }
    }
// (Cyb : C.O.A)

    // ── UE5-specific helpers ───────────────────────────────────────────

    /// SerializeIntPacked (variable length: 7 data bits per byte + 1 continuation).
    void write_sip(uint64_t value) {
        do {
            uint8_t byte = static_cast<uint8_t>((value & 0x7F) << 1);
            value >>= 7;
            if (value > 0) byte |= 1; // continuation
            write_bits(byte, 8);
        } while (value > 0);
    }

    /// SerializeInt(Max) — adaptive: writes bits one at a time while
    /// (Value + Mask < Max).  Matches FBitWriter::SerializeInt.
    void write_serialize_int(uint32_t value, uint32_t max_val) {
        uint32_t mask = 1;
        uint32_t new_value = 0;
        while ((new_value + mask) < max_val && mask != 0) {
            const uint32_t bit = (value & mask) != 0 ? 1u : 0u;
            if (bit) new_value |= mask;
            write_bits(bit, 1);
            mask <<= 1;
        }
    }

    /// UE5 FString: int32 SaveNum (char count including NUL) + char data.
    /// SaveNum > 0 → ANSI (8 bits per char).  SaveNum < 0 → UTF-16LE.
    /// Empty string emits just the zero count.
    void write_fstring_ansi(const std::string& s) {
        if (s.empty()) {
            write_bits(0u, 32);
            return;
        }
        const int32_t save_num = static_cast<int32_t>(s.size() + 1); // +1 for NUL
        write_bits(static_cast<uint32_t>(save_num), 32);
        for (char c : s) write_bits(static_cast<uint8_t>(c), 8);
        write_bits(0u, 8); // NUL terminator
    }

    /// FRotator::NetSerialize → SerializeCompressedShort layout.
    /// Emits: 1 bit (bPitchNonZero) [+ 16 bits if set] repeated for yaw+roll.
    void write_compressed_rotator(uint16_t pitch, uint16_t yaw, uint16_t roll) {
        auto emit_axis = [&](uint16_t v) {
            const uint32_t flag = v != 0 ? 1u : 0u;
            write_bits(flag, 1);
            if (flag) write_bits(v, 16);
        };
        emit_axis(pitch);
        emit_axis(yaw);
        emit_axis(roll);
    }

    /// ConditionallySerializeQuantizedVector — 1 bit wasSerialized,
    /// optional 1 bit shouldQuantize, optional vector.  If not serialized
    /// the field defaults client-side (Location→zero, Scale→one, etc.).
    ///
    /// Passing `was_serialized = false` emits just the 1-bit clear flag.
    void write_conditional_vector_stub(bool was_serialized) {
        write_bits(was_serialized ? 1u : 0u, 1);
        // Full vector emission with quantization comes in Phase 3.7 —
        // this stub only supports the "not serialized" case which is
        // what the captured PlayerController uses for Location.
    }

    // ── Accessors ──────────────────────────────────────────────────────

    std::size_t bit_count() const { return bit_count_; }
    std::size_t byte_size() const { return (bit_count_ + 7) / 8; }
    const std::vector<uint8_t>& bytes() const { return data_; }
    std::vector<uint8_t>&       bytes()       { return data_; }

    /// Clear the buffer (reuse without reallocation).
    void reset() {
        std::fill(data_.begin(), data_.end(), 0);
        bit_count_ = 0;
    }

private:
    std::vector<uint8_t> data_;
    std::size_t          bit_count_ = 0;
};

}} // namespace aoc::protocol
