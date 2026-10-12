// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tools/test_nmt_welcome.cpp
//
//  Session H.2a exit-criterion test.
//
//  Byte-identity check: feed the captured NMT_Welcome (packet #2 in the
//  2000-packet bootstrap, seq=14267) inputs into our fresh NmtBuilder,
//  then compare the produced bunch bits to the bits extracted from the
//  captured packet at the same offset.
//
//  If the bunch-bit stream matches, Session H.2a is done for the Welcome
//  opcode — our emitter is a drop-in replacement for the replay's bytes.
//  The outer packet framing (FNetPacketNotify + custom field +
//  termination) is not in scope for H.2a; UdpPacketEmitter::wrap_and_send
//  already handles that and will be hooked up in H.2's final step.
//
//  Captured packet #2 payload (from replay_full.jsonl via catalog):
//      level        = "/Game/Levels/Verra_World_Master/Verra_World_Master"
//      gameMode     = "/Game/GameBlueprints/AoCGameModeBaseBP.AoCGameModeBaseBP_C"
//      redirect_url = ""
//      ch_sequence  = 955   (bunch[0].ch_seq)
//      bunch_bits   = 984   (bunch[0].bdb payload + header)
// ============================================================================
#include "protocol/emit/nmt_builder.h"
#include "protocol/emit/bunch_writer.h"
#include "protocol/wire/packet_parser.h"
#include "protocol/wire/packet_reader.h"
#include "protocol/wire/ue5_primitives.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace aoc;
using namespace aoc::protocol;
namespace wire = aoc::protocol::wire;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

// Captured packet #2 from the 2000-packet bootstrap (seq=14267).
// This is the real AoC server's NMT_Welcome reply to a client NMT_Login.
static const char* kCapturedHex =
    "96760c500c5012bb770000000065af0953ecd16c1000bbff17c01e01330000002f"
    "47616d652f4c6576656c732f56657272615f576f726c645f4d61737465722f56657272"
    "615f576f726c645f4d6173746572003b0000002f47616d652f47616d65426c75657072"
    "696e74732f416f4347616d654d6f64654261736542502e416f4347616d654d6f646542"
    "6173654250"
    "5f4300000000" "0003";

static std::vector<uint8_t> hex_decode(const std::string& s) {
    std::vector<uint8_t> out;
    out.reserve(s.size() / 2);
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        char hi = s[i], lo = s[i + 1];
        auto nyb = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return 0;
        };
        out.push_back(static_cast<uint8_t>((nyb(hi) << 4) | nyb(lo)));
    }
    return out;
}

// Read `n_bits` bits at offset `bit_off` of `src` as a byte stream, with
// each bit placed MSB-within-byte (standard way to "linearize" a bit
// sequence for comparison).  Actually simpler: store as a vector of 0/1s.
static std::vector<uint8_t> extract_bit_range(const uint8_t* src,
                                                size_t bit_off, size_t n_bits) {
    std::vector<uint8_t> out;
    out.reserve(n_bits);
    for (size_t i = 0; i < n_bits; ++i) {
        size_t bp = bit_off + i;
        int bit = (src[bp >> 3] >> (bp & 7)) & 1;
        out.push_back(static_cast<uint8_t>(bit));
    }
    return out;
}
// ~ Cyb :: C | O | A ~

int main() {
    std::printf("=== Session H.2a NMT_Welcome byte-identity test ===\n");

    // ── Step 1: parse the captured packet to locate the bunch ──
    std::vector<uint8_t> captured = hex_decode(kCapturedHex);
    std::printf("  captured bytes     : %zu\n", captured.size());

    auto parsed = wire::parse_packet(captured.data(), captured.size(),
                                       wire::Direction::ServerToClient);
    CHECK(parsed.has_value(), "captured packet parses cleanly");
    if (!parsed) {
        std::printf("\n=== Summary ===\n  Passed: %d  Failed: %d\n", g_pass, g_fail);
        return 1;
    }

    CHECK(parsed->bunches.size() == 1, "packet has exactly 1 bunch");
    if (parsed->bunches.empty()) return 1;
    const auto& cap_bunch = parsed->bunches[0];

    std::printf("  bunch: ch=%u chSeq=%u bdb=%u ctrl=%d open=%d\n",
                 cap_bunch.channel, cap_bunch.ch_sequence,
                 cap_bunch.bunch_data_bits,
                 int(cap_bunch.is_control), int(cap_bunch.is_open));

    CHECK(cap_bunch.channel == 0,            "bunch is on control channel (ch=0)");
    CHECK(cap_bunch.ch_sequence == 955,      "ch_sequence is 955");
    CHECK(cap_bunch.bunch_data_bits == 984,  "bunch_data_bits is 984");
    CHECK(!cap_bunch.is_partial,             "bunch is not partial");

    // ── Step 2: build our own NMT_Welcome with matching inputs ──
    emit::BunchWriter bw;
    emit::NmtBunchContext ctx;
    ctx.ch_sequence  = 955;
    ctx.opens_channel = false;  // channel already open (via NMT_Challenge)

    const std::string level        = "/Game/Levels/Verra_World_Master/Verra_World_Master";
    const std::string gamemode     = "/Game/GameBlueprints/AoCGameModeBaseBP.AoCGameModeBaseBP_C";
    const std::string redirect_url = "";

    size_t bits_written = emit::NmtBuilder::build_welcome(
        bw, ctx, level, gamemode, redirect_url);

    std::printf("  built bits         : %zu\n", bits_written);
    CHECK(bits_written > 0, "NmtBuilder::build_welcome produced output");

    // ── Step 3: extract the reference bunch bits from the captured packet ──
    // The phase1 parser's data_start_bit is relative to the OUTER packet raw
    // buffer (cap_bunch.data_start_bit).  For NMT bunches, the "bunch" that
    // we build consists of:
    //   - bunch header (flags + chIndex + chSeq + ChName + BDB)
    //   - bunch payload (NMT opcode + fstrings)
    // data_start_bit points to AFTER the bunch header at the payload start.
    // To compare bunches byte-for-byte we need the range from header_start
    // to payload_end.  The parser stores header_bits for this purpose.
    size_t hdr_start = cap_bunch.data_start_bit - cap_bunch.header_bits;
    size_t total_bunch_bits = cap_bunch.header_bits + cap_bunch.bunch_data_bits;

    std::printf("  captured bunch bits: %zu (header=%zu + payload=%u)\n",
                 total_bunch_bits, cap_bunch.header_bits, cap_bunch.bunch_data_bits);

    CHECK(bits_written == total_bunch_bits,
          "built bunch bit-length matches captured");

    if (bits_written != total_bunch_bits) {
        std::printf("    (expected %zu, got %zu, delta=%ld)\n",
                     total_bunch_bits, bits_written,
                     static_cast<long>(bits_written) - static_cast<long>(total_bunch_bits));
    }
// built with C Y B and C O A

    // ── Step 4: bit-by-bit comparison ──
    auto expected = extract_bit_range(captured.data(), hdr_start, total_bunch_bits);
    auto actual   = extract_bit_range(bw.data(), 0, bits_written);

    size_t compare_bits = std::min(expected.size(), actual.size());
    size_t first_diff = compare_bits;  // compare_bits == "no diff found"
    size_t n_diffs    = 0;
    for (size_t i = 0; i < compare_bits; ++i) {
        if (expected[i] != actual[i]) {
            if (first_diff == compare_bits) first_diff = i;
            ++n_diffs;
        }
    }
    if (expected.size() != actual.size()) ++n_diffs;

    if (n_diffs == 0) {
        std::printf("  [ok ] BYTE-IDENTICAL: all %zu bits match!\n", compare_bits);
        g_pass++;
    } else {
        std::printf("  [FAIL] %zu bits differ, first at bit %zu\n",
                     n_diffs, first_diff);
        // Print context around the first diff for debugging.
        const size_t ctx_bits = 32;
        size_t lo = first_diff > ctx_bits/2 ? first_diff - ctx_bits/2 : 0;
        size_t hi = std::min(lo + ctx_bits, compare_bits);
        std::printf("     expected [bits %zu..%zu]: ", lo, hi);
        for (size_t i = lo; i < hi; ++i) std::printf("%d", expected[i]);
        std::printf("\n     actual   [bits %zu..%zu]: ", lo, hi);
        for (size_t i = lo; i < hi; ++i) std::printf("%d", actual[i]);
        std::printf("\n");
        g_fail++;
    }

    std::printf("\n=== Summary ===\n");
    std::printf("  Passed: %d\n", g_pass);
    std::printf("  Failed: %d\n", g_fail);
    std::printf("  Result: %s\n", g_fail == 0 ? "ALL PASSED" : "FAILURES");
    return g_fail == 0 ? 0 : 1;
}
