// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/bootstrap/packet_recipe.cpp
//
//  Implementation of StaticPacketRecipe / PatchedPacketRecipe + the
//  bit-patcher utility functions.  These are the primitives that
//  every dynamic-field-aware recipe builds on.
// ============================================================================
#include "net/bootstrap/packet_recipe.h"
#include "net/game_server.h"   // ReplayData

#include <cstring>
#include <spdlog/spdlog.h>

namespace aoc { namespace net { namespace bootstrap {

// ── Bit primitives (LSB-first, same convention as BunchWriter) ──────

static inline bool read_bit_lsb(const uint8_t* buf, std::size_t bit_off) {
    return (buf[bit_off >> 3] >> (bit_off & 7)) & 1;
}

static inline void write_bit_lsb(uint8_t* buf, std::size_t bit_off, bool v) {
    if (v)
        buf[bit_off >> 3] |= static_cast<uint8_t>(1u << (bit_off & 7));
    else
        buf[bit_off >> 3] &= static_cast<uint8_t>(~(1u << (bit_off & 7)));
}

void patch_bits_in_place(std::vector<uint8_t>& dst,
                          std::size_t            dst_bit_off,
                          const uint8_t*         src,
                          std::size_t            bit_count) {
    for (std::size_t i = 0; i < bit_count; ++i) {
        write_bit_lsb(dst.data(), dst_bit_off + i,
                      read_bit_lsb(src, i));
    }
}
// -- C O A x C Y B --

BunchBits encode_intrepid_netguid(const IntrepidNetGUID& g) {
    // 128 bits = 16 bytes, LSB-first for each 32-bit component.
    // Layout: [lo 32 of Obj][hi 32 of Obj][Srv 32][Rnd 32]
    BunchBits out;
    out.bytes.resize(16, 0);
    auto w32 = [&](std::size_t byte_off, uint32_t v) {
        out.bytes[byte_off + 0] = static_cast<uint8_t>( v        & 0xff);
        out.bytes[byte_off + 1] = static_cast<uint8_t>((v >>  8) & 0xff);
        out.bytes[byte_off + 2] = static_cast<uint8_t>((v >> 16) & 0xff);
        out.bytes[byte_off + 3] = static_cast<uint8_t>((v >> 24) & 0xff);
    };
    w32(0,  static_cast<uint32_t>(g.obj & 0xFFFFFFFFULL));
    w32(4,  static_cast<uint32_t>(g.obj >> 32));
    w32(8,  g.srv);
    w32(12, g.rnd);
    out.bit_count = 128;
    return out;
}

BunchBits encode_location_body(int32_t x, int32_t y, int32_t z, uint8_t location_bits) {
    // UE5 SerializePackedVector body: 3 × N-bit offset-binary ints.
    // NOTE: the 5-bit "BitsNeeded" prefix that precedes this must be
    // patched separately (it's captured-static at 24 for the PC spawn).
    //
    // Offset-binary = signed int shifted up so 0 maps to 2^(N-1).
    // Our CharacterProfile::FVectorScaled already holds the raw
    // offset-binary integer (it's whatever the quantizer produces).
    // If the caller is passing signed cm*scale, convert here:
    const uint32_t bias = 1u << (location_bits - 1);
    const uint32_t mask = (location_bits >= 32) ? 0xFFFFFFFFu : ((1u << location_bits) - 1u);
    uint32_t ux = (static_cast<uint32_t>(x) + bias) & mask;
    uint32_t uy = (static_cast<uint32_t>(y) + bias) & mask;
    uint32_t uz = (static_cast<uint32_t>(z) + bias) & mask;

    BunchBits out;
    out.bit_count = 3ULL * location_bits;
    out.bytes.resize((out.bit_count + 7) / 8, 0);

    auto write_bits = [&](uint32_t val, std::size_t bit_off, std::size_t bits) {
        for (std::size_t i = 0; i < bits; ++i) {
            if ((val >> i) & 1u) {
                out.bytes[(bit_off + i) >> 3] |=
                    static_cast<uint8_t>(1u << ((bit_off + i) & 7));
            }
        }
    };
    write_bits(ux, 0,                    location_bits);
    write_bits(uy, location_bits,        location_bits);
    write_bits(uz, location_bits * 2,    location_bits);
    return out;
}

BunchBits encode_fstring(const std::string& s) {
    // UE5 FString format: [int32 length LE][ASCII bytes][NUL terminator].
    // Length includes the NUL in some contexts and excludes it in others;
    // captured AoC pkt#0 uses: len=9 for 8-char string + NUL, so LEN
    // INCLUDES the NUL.  We follow that convention.
    const int32_t len = static_cast<int32_t>(s.size() + 1);
    BunchBits out;
    out.bytes.resize(4 + s.size() + 1, 0);
    out.bytes[0] = static_cast<uint8_t>( len        & 0xff);
    out.bytes[1] = static_cast<uint8_t>((len >>  8) & 0xff);
    out.bytes[2] = static_cast<uint8_t>((len >> 16) & 0xff);
    out.bytes[3] = static_cast<uint8_t>((len >> 24) & 0xff);
    std::memcpy(out.bytes.data() + 4, s.data(), s.size());
    out.bytes[4 + s.size()] = 0;  // NUL
    out.bit_count = (4 + s.size() + 1) * 8;
    return out;
}

// ── Replay bunch extraction ─────────────────────────────────────────

/// Extract the bunch-bit-stream portion of a captured packet.  Starts
/// at `bunch_start_bit` (metadata) and runs for `bunch_bits` bits.
/// Returns the raw bits as a byte vector (LSB-first), ready for
/// IGameServerHost::send_bunch_packet to wrap with our session's
/// seq/ack/PacketInfo header.
static BunchBits extract_bunch_bits(const ::ReplayData& replay, int32_t pkt_idx) {
    BunchBits out;
    if (pkt_idx < 0 || static_cast<std::size_t>(pkt_idx) >= replay.packets.size()) {
        spdlog::warn("[PacketRecipe] replay_idx {} out of range "
                     "(replay has {} packets)",
                     pkt_idx, replay.packets.size());
        return out;
    }
    const auto& rpkt = replay.packets[pkt_idx];
    if (rpkt.bunch_bits == 0 || rpkt.raw.empty()) {
        spdlog::warn("[PacketRecipe] replay_idx {} has no bunch data "
                     "(bunch_bits={}, raw empty={})",
                     pkt_idx,
                     static_cast<int>(rpkt.bunch_bits),
                     rpkt.raw.empty() ? 1 : 0);
        return out;
    }
    out.bit_count = rpkt.bunch_bits;
    out.bytes.resize((rpkt.bunch_bits + 7) / 8, 0);

    // Copy bit-by-bit from raw[bunch_start_bit..bunch_start_bit+bunch_bits)
    // into out[0..bunch_bits).  Both src and dst are LSB-first.
    const std::size_t base_bit = static_cast<std::size_t>(rpkt.bunch_start_bit);
    const std::size_t total_bits = static_cast<std::size_t>(rpkt.bunch_bits);
    for (std::size_t i = 0; i < total_bits; ++i) {
        std::size_t src_bit = base_bit + i;
        if (read_bit_lsb(rpkt.raw.data(), src_bit)) {
            out.bytes[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
        }
    }
    return out;
}

// ── StaticPacketRecipe ──────────────────────────────────────────────

BunchBits StaticPacketRecipe::build(const BuildContext& ctx) const {
    return extract_bunch_bits(ctx.replay, replay_idx_);
}

// ── PatchedPacketRecipe ─────────────────────────────────────────────

BunchBits PatchedPacketRecipe::build(const BuildContext& ctx) const {
    // 1. Extract captured bunch bits.
    BunchBits result = extract_bunch_bits(ctx.replay, replay_idx_);
    if (result.bit_count == 0) {
        return result;  // already-warned above
    }
// (C Y B | C-O-A)

    // 2. Apply each patch in order.
    for (const auto& p : patches_) {
        BunchBits new_bits = p.encode(ctx);
        std::size_t width = p.bit_width > 0 ? p.bit_width : new_bits.bit_count;

        if (p.bit_offset + width > result.bit_count) {
            spdlog::error("[PatchedRecipe] Patch '{}' would write past end "
                          "of bunch (offset={}, width={}, bunch={})",
                          p.field_name, p.bit_offset, width, result.bit_count);
            continue;
        }
        if (new_bits.bit_count < width) {
            spdlog::error("[PatchedRecipe] Patch '{}' encode produced {} "
                          "bits but field width is {}",
                          p.field_name, new_bits.bit_count, width);
            continue;
        }
        patch_bits_in_place(result.bytes, p.bit_offset,
                             new_bits.bytes.data(), width);
        spdlog::debug("[PatchedRecipe] '{}': patched {} bits at offset {}",
                      p.field_name, width, p.bit_offset);
    }
    return result;
}

}}} // namespace aoc::net::bootstrap
