// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/wire/server_move_parser.h
//
//  Authoritative UE5 ServerMoveNoBase wire decoder and ClientAckGoodMove framer.
//  Parses incoming client movement RPC bunches (ch=19, unreliable, 230 bits)
//  and produces validated world transform data for LiveWorld and ActorRegistry.
// ============================================================================
#pragma once

#include "protocol/wire/ue5_primitives.h"
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <string>

namespace aoc { namespace protocol { namespace wire {

struct FVector3D {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct FServerMoveNoBase {
    uint8_t   fn_idx = 44;               // RPC Function Index (44 = ServerMoveNoBase in ACharacter)
    uint8_t   compressed_move_flags = 0; // Flags (e.g. 0xaf)
    uint8_t   client_roll = 0;           // Roll angle
    float     timestamp = 0.0f;          // Strictly monotonic frame timestamp (seconds)
    FVector3D in_accel{};                // Acceleration (FVector_NetQuantize10, scale 10.0)
    FVector3D client_loc{};              // Authoritative client world location (scale 100.0)
    uint32_t  view = 0;                  // View rotator packed
    uint8_t   client_movement_mode = 1;  // MOVE_Walking = 1
    size_t    total_bits_consumed = 0;
};

/// Reads a UE5 quantized vector (FVector_NetQuantize10 / FVector_NetQuantize100)
inline FVector3D read_quantized_vector(const uint8_t* data, size_t data_len,
                                      size_t& bit_off, double scale) {
    uint32_t comp_and_extra = ue5::read_serialize_int(data, data_len, bit_off, 128);
    uint32_t comp_bits = comp_and_extra & 63;
    uint32_t extra = comp_and_extra >> 6;
    if (comp_bits == 0) {
        return {0.0, 0.0, 0.0};
    }
    uint64_t raw_x = ue5::read_bits(data, data_len, bit_off, static_cast<int>(comp_bits));
    uint64_t raw_y = ue5::read_bits(data, data_len, bit_off, static_cast<int>(comp_bits));
    uint64_t raw_z = ue5::read_bits(data, data_len, bit_off, static_cast<int>(comp_bits));

    int64_t sign_bit = 1ULL << (comp_bits - 1);
    int64_t x = static_cast<int64_t>((raw_x ^ sign_bit) - sign_bit);
    int64_t y = static_cast<int64_t>((raw_y ^ sign_bit) - sign_bit);
    int64_t z = static_cast<int64_t>((raw_z ^ sign_bit) - sign_bit);
// built by C Y B with C-O-A

    if (extra != 0) {
        return {static_cast<double>(x) / scale,
                static_cast<double>(y) / scale,
                static_cast<double>(z) / scale};
    } else {
        return {static_cast<double>(x),
                static_cast<double>(y),
                static_cast<double>(z)};
    }
}

/// Decodes an incoming ServerMoveNoBase RPC bunch starting at bunch_data_start.
/// Returns true if successfully parsed and extracted valid movement state.
inline bool parse_server_move_no_base(const uint8_t* data, size_t data_len,
                                      size_t bunch_data_start, size_t bunch_data_bits,
                                      FServerMoveNoBase& out_move) {
    if (bunch_data_bits < 100 || (bunch_data_start + bunch_data_bits) > (data_len * 8)) {
        return false;
    }

    size_t bit_off = bunch_data_start;

    // ── 1. ContentBlock Header ──
    uint64_t b_has_rep = ue5::read_bits(data, data_len, bit_off, 1);
    uint64_t b_is_actor = ue5::read_bits(data, data_len, bit_off, 1);
    if (b_has_rep != 0 || b_is_actor != 1) {
        return false; // Must target Actor RPC
    }

    uint64_t cb_size = ue5::read_sip(data, data_len, bit_off);
    if (cb_size == 0 || (bit_off + cb_size) > (bunch_data_start + bunch_data_bits)) {
        return false;
    }

    // ── 2. Field Header ──
    uint8_t fn_idx = static_cast<uint8_t>(ue5::read_bits(data, data_len, bit_off, 8));
    if (fn_idx != 44) { // ServerMoveNoBase
        return false;
    }
    out_move.fn_idx = fn_idx;

    uint64_t inner_size = ue5::read_sip(data, data_len, bit_off);
    if (inner_size == 0 || (bit_off + inner_size) > (bunch_data_start + bunch_data_bits)) {
        return false;
    }

    // ── 3. RPC Parameters ──
    out_move.compressed_move_flags = static_cast<uint8_t>(ue5::read_bits(data, data_len, bit_off, 8));
    out_move.client_roll           = static_cast<uint8_t>(ue5::read_bits(data, data_len, bit_off, 8));

    // TimeStamp parameter: 1-bit prefix flag + 32-bit IEEE 754 float
    (void)ue5::read_bits(data, data_len, bit_off, 1);
    uint32_t ts_u32 = static_cast<uint32_t>(ue5::read_bits(data, data_len, bit_off, 32));
    std::memcpy(&out_move.timestamp, &ts_u32, sizeof(float));

    if (!std::isfinite(out_move.timestamp) || out_move.timestamp <= 0.0f) {
        return false;
    }

    // InAccel: FVector_NetQuantize10 (scale 10.0)
    out_move.in_accel = read_quantized_vector(data, data_len, bit_off, 10.0);

    // ClientLoc: FVector_NetQuantize100 (scale 100.0)
    out_move.client_loc = read_quantized_vector(data, data_len, bit_off, 100.0);

    // Remaining bits (View rotator + movement mode)
    if (bit_off < (bunch_data_start + bunch_data_bits)) {
        uint64_t view_send = ue5::read_bits(data, data_len, bit_off, 1);
        if (view_send != 0 && (bit_off + 32) <= (bunch_data_start + bunch_data_bits)) {
            out_move.view = static_cast<uint32_t>(ue5::read_bits(data, data_len, bit_off, 32));
        }
    }

    out_move.total_bits_consumed = bit_off - bunch_data_start;
    return true;
}

}}} // namespace aoc::protocol::wire
