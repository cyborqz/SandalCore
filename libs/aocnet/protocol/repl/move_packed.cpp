// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/move_packed.cpp
//
//  See move_packed.h.
// ============================================================================
#include "protocol/repl/move_packed.h"

#include "protocol/wire/packet_reader.h"

#include <bit>

namespace aoc { namespace protocol { namespace repl {

using emit::BunchWriter;
using wire::PacketReader;

namespace {

bool fail(std::string* why, const char* what, size_t at) {
    if (why) *why = std::string(what) + " at bit " + std::to_string(at);
    return false;
}

/// SerializeOptionalValue<uint8>: a bit that says whether the value differs from `fallback`, then the value.
uint8_t read_optional_u8(PacketReader& r, uint8_t fallback) { return r.read_bit() ? r.read_uint8() : fallback; }

void write_optional_u8(BunchWriter& out, uint8_t value, uint8_t fallback) {
    out.write_bit(value != fallback);
    if (value != fallback) out.write_uint8(value);
}
// ~ C | O | A & C Y B ~

bool read_move(PacketReader& r, NetworkMove& m, std::string* why) {
    m.time_stamp = std::bit_cast<float>(r.read_uint32());
    const auto acc = read_quantized_vector(r);
    if (!acc) return fail(why, "Acceleration does not read", r.pos());
    m.acceleration = *acc;
    const auto loc = read_quantized_vector(r);
    if (!loc) return fail(why, "Location does not read", r.pos());
    m.location = *loc;
    uint16_t* axes[3] = {&m.control_rotation.pitch, &m.control_rotation.yaw, &m.control_rotation.roll};
    for (uint16_t* a : axes) *a = r.read_bit() ? r.read_uint16() : 0;
    m.compressed_flags = read_optional_u8(r, 0);
    if (r.read_bit()) {
        auto base = read_object(r);
        if (!base) return fail(why, "MovementBase is not an object reference the reader knows", r.pos());
        m.base = *base;
    }
    if (r.read_bit()) {
        auto bone = read_name(r);
        if (!bone) return fail(why, "MovementBaseBoneName does not read", r.pos());
        m.base_bone = std::move(*bone);
    }
    m.movement_mode = read_optional_u8(r, kMoveWalking);
    m.input_bits = static_cast<uint8_t>(r.read_bits(4));
    if (overran(r)) return fail(why, "the move runs past the end", r.pos());
    return true;
}

bool write_move(BunchWriter& out, const NetworkMove& m) {
    out.write_uint32(std::bit_cast<uint32_t>(m.time_stamp));
    if (!write_quantized_vector(out, m.acceleration) || !write_quantized_vector(out, m.location)) return false;
    const uint16_t axes[3] = {m.control_rotation.pitch, m.control_rotation.yaw, m.control_rotation.roll};
    for (uint16_t a : axes) {
        out.write_bit(a != 0);
        if (a != 0) out.write_uint16(a);
    }
    write_optional_u8(out, m.compressed_flags, 0);
    out.write_bit(m.base.has_value());
    if (m.base && !write_object(out, *m.base)) return false;
    out.write_bit(m.base_bone.has_value());
    if (m.base_bone && !write_name(out, *m.base_bone)) return false;
    write_optional_u8(out, m.movement_mode, kMoveWalking);
    out.write_bits(m.input_bits & 15u, 4);
    return true;
}

}  // namespace

std::optional<MovePacked> read_move_packed(const uint8_t* data, size_t data_bytes, size_t pos, size_t end, std::string* why) {
    // `end` is a claim of the caller (a bunch's own numbers); the buffer is what can be read. A parameter that claims
    // bits the buffer does not hold is not a parameter.
    if (end > data_bytes * 8) {
        fail(why, "the parameter runs past the end of the buffer", end);
        return std::nullopt;
    }
    PacketReader r(data, data_bytes, end);
    r.set_pos(pos);
    if (!r.read_bit()) {
        fail(why, "the parameter is zeroed (send bit 0)", pos);
        return std::nullopt;
    }
    const auto num_bits = r.read_sip();
    if (!num_bits) {
        fail(why, "NumBits does not read", r.pos());
        return std::nullopt;
    }
    if (*num_bits > kMaxPackedMoveBits) {
        fail(why, "NumBits is over the exe's limit", r.pos());
        return std::nullopt;
    }
    if (*num_bits != r.remaining_bits()) {
        fail(why, "NumBits is not where the parameter ends", r.pos());
        return std::nullopt;
    }
    MovePacked m;
    if (!read_move(r, m.new_move, why)) return std::nullopt;
    if (r.read_bit()) {
        m.dual_hybrid_root_motion = r.read_bit() != 0;
        NetworkMove pending;
        if (!read_move(r, pending, why)) return std::nullopt;
        m.pending = std::move(pending);
    }
    if (r.read_bit()) {
        NetworkMove old;
        if (!read_move(r, old, why)) return std::nullopt;
        m.old = std::move(old);
    }
    m.disable_combined_scoped_move = r.read_bit() != 0;
    if (overran(r)) {
        fail(why, "the container runs past the end", r.pos());
        return std::nullopt;
    }
    if (r.remaining_bits() != 0) {
        fail(why, "bits are left after the container", r.pos());
        return std::nullopt;
    }
    return m;
}

bool write_move_packed(BunchWriter& out, const MovePacked& moves) {
    BunchWriter bits;
    if (!write_move(bits, moves.new_move)) return false;
    bits.write_bit(moves.pending.has_value());
    if (moves.pending) {
        bits.write_bit(moves.dual_hybrid_root_motion);
        if (!write_move(bits, *moves.pending)) return false;
    }
    bits.write_bit(moves.old.has_value());
    if (moves.old && !write_move(bits, *moves.old)) return false;
    bits.write_bit(moves.disable_combined_scoped_move);
    if (bits.bit_pos() > kMaxPackedMoveBits) return false;
    out.write_bit(1);
    out.write_sip(bits.bit_pos());
    out.write_bit_range(bits.data(), 0, bits.bit_pos());
    return true;
}

}}}  // namespace aoc::protocol::repl
