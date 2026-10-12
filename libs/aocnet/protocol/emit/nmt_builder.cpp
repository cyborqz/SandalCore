// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/emit/nmt_builder.cpp
// ============================================================================
#include "protocol/emit/nmt_builder.h"
#include "protocol/wire/ue5_primitives.h"
#include <cstring>

namespace aoc { namespace protocol { namespace emit {

// ─── FString writer ────────────────────────────────────────────────────────

void NmtBuilder::write_fstring_ansi(BunchWriter& out, const std::string& s) {
    if (s.empty()) {
        // UE5 SerializeFString with empty → 4-byte int32(0), no char bytes.
        out.write_int32(0);
        return;
    }
    int32_t save_num = static_cast<int32_t>(s.size() + 1);  // +1 for NUL
    out.write_int32(save_num);
    for (char c : s) out.write_uint8(static_cast<uint8_t>(c));
    out.write_uint8(0);  // NUL terminator
}

// ─── Bunch-header writer (NMT format) ──────────────────────────────────────
//
// Mirrors the byte layout from game_server.h send_nmt (lines 2487-2519)
// for the channel-already-open case.  NMT uses the 10-bit ChSequence and
// 13-bit BunchDataBits "C>S format" per the existing server code.
// [CoA ~ C:Y:B]

size_t NmtBuilder::write_nmt_bunch_header(BunchWriter& out,
                                            const NmtBunchContext& ctx) {
    if (ctx.opens_channel) {
        out.write_bit(1);       // bControl = 1
        out.write_bit(1);       // bOpen    = 1
        out.write_bit(0);       // bClose   = 0
    } else {
        out.write_bit(0);       // bControl = 0 (open/close not re-emitted)
    }
    out.write_bit(0);           // bIsReplicationPaused
    out.write_bit(1);           // bReliable = 1

    // ChIndex = 0 via SerializeIntPacked — writes a single byte 0x00.
    // SerializeIntPacked encoding of 0 is a single byte with value 0x00
    // (the "more-follows" bit unset).  We write 8 bits of zeros.
    out.write_bits(0, 8);

    out.write_bit(0);           // bHasPackageMapExports
    out.write_bit(0);           // bHasMustBeMappedGUIDs
    out.write_bit(0);           // bPartial

    // ChSequence (10-bit for NMT C>S-format reliable bunches)
    out.write_bits(ctx.ch_sequence & 0x3FFu, 10);

    // ChName — bHardcoded=1 + packed bytes 0xFF 0x02 → hardcoded index 255
    // (EName[255] = "Name_Control" / default control-channel hardcoded name)
    out.write_bit(1);           // bHardcoded
    out.write_uint8(0xFF);
    out.write_uint8(0x02);

    // Reserve 13 bits for BunchDataBits; caller patches after payload.
    size_t bdb_bit_pos = out.bit_pos();
    out.write_bits(0, 13);
    return bdb_bit_pos;
}

// ─── Back-patch BDB ────────────────────────────────────────────────────────

void NmtBuilder::patch_bunch_data_bits(BunchWriter& out,
                                          size_t bit_pos,
                                          uint16_t bdb) {
    // Overwrite 13 bits in the already-written buffer.  Manual bit-level
    // write since BunchWriter doesn't expose a patch-at-position API —
    // we mirror the layout expected by ue5_primitives write_bits.
    // Safe because the buffer is sized and the target position is within it.
    // Implementation: read current 13 bits, replace with `bdb`.
    uint8_t* buf = const_cast<uint8_t*>(out.data());
    // Clear + write bit-by-bit LSB-first.
    for (int i = 0; i < 13; ++i) {
        const size_t bp = bit_pos + static_cast<size_t>(i);
        const uint8_t mask = static_cast<uint8_t>(1u << (bp & 7));
        const bool bit = (bdb >> i) & 1u;
        if (bit) buf[bp >> 3] |=  mask;
        else     buf[bp >> 3] &= static_cast<uint8_t>(~mask);
    }
}

// ─── build_welcome ─────────────────────────────────────────────────────────

size_t NmtBuilder::build_welcome(BunchWriter& out,
                                    const NmtBunchContext& ctx,
                                    const std::string& level,
                                    const std::string& game_mode,
                                    const std::string& redirect_url) {
    const size_t start_bit = out.bit_pos();
    const size_t bdb_bit_pos = write_nmt_bunch_header(out, ctx);
    const size_t payload_start = out.bit_pos();

    // NMT_Welcome opcode byte (1 from DataChannel.h line 174)
    out.write_uint8(1);

    // Three FString fields.
    write_fstring_ansi(out, level);
    write_fstring_ansi(out, game_mode);
    write_fstring_ansi(out, redirect_url);

    const size_t payload_end = out.bit_pos();
    const size_t payload_bits = payload_end - payload_start;
    if (payload_bits > 0x1FFF) return 0;  // overflow 13-bit BDB

    patch_bunch_data_bits(out, bdb_bit_pos,
                           static_cast<uint16_t>(payload_bits));
    return out.bit_pos() - start_bit;
}

// ─── build_challenge ──────────────────────────────────────────────────────

size_t NmtBuilder::build_challenge(BunchWriter& out,
                                      const NmtBunchContext& ctx,
                                      const std::string& challenge) {
    const size_t start_bit    = out.bit_pos();
    const size_t bdb_bit_pos  = write_nmt_bunch_header(out, ctx);
    const size_t payload_start = out.bit_pos();

    // NMT_Challenge opcode byte (3 from DataChannel.h line 176)
    out.write_uint8(3);

    // Single FString challenge.
    write_fstring_ansi(out, challenge);

    const size_t payload_bits = out.bit_pos() - payload_start;
    if (payload_bits > 0x1FFF) return 0;
    patch_bunch_data_bits(out, bdb_bit_pos, static_cast<uint16_t>(payload_bits));
    return out.bit_pos() - start_bit;
}

// ─── build_netguid_assign ────────────────────────────────────────────────

size_t NmtBuilder::build_netguid_assign(BunchWriter& out,
                                           const NmtBunchContext& ctx,
                                           uint32_t netguid,
                                           const std::string& asset_path) {
    const size_t start_bit    = out.bit_pos();
    const size_t bdb_bit_pos  = write_nmt_bunch_header(out, ctx);
    const size_t payload_start = out.bit_pos();
// (CoA | C.Y.B)

    // NMT_NetGUIDAssign opcode byte (18 from DataChannel.h line 187)
    out.write_uint8(18);

    // FNetworkGUID via SerializeIntPacked (BunchWriter supports this natively).
    out.write_sip(netguid);

    // FString asset path.
    write_fstring_ansi(out, asset_path);

    const size_t payload_bits = out.bit_pos() - payload_start;
    if (payload_bits > 0x1FFF) return 0;
    patch_bunch_data_bits(out, bdb_bit_pos, static_cast<uint16_t>(payload_bits));
    return out.bit_pos() - start_bit;
}

}}} // namespace aoc::protocol::emit
