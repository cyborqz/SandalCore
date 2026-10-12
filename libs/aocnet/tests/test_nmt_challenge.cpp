// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tools/test_nmt_challenge.cpp
//
//  Session H.2b byte-identity test.  Captured packet #0 from the bootstrap
//  (seq=14265) is a 42-byte S>C packet containing a single NMT_Challenge
//  bunch with the challenge string "50995344".  We reproduce the bunch via
//  NmtBuilder::build_challenge and verify bit-identical output.
// ============================================================================
#include "protocol/emit/nmt_builder.h"
#include "protocol/emit/bunch_writer.h"
#include "protocol/wire/packet_parser.h"
#include <cstdio>
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

// Captured packet #0 (seq=14265), 42 bytes — single NMT_Challenge bunch.
static const char* kCapturedHex =
    "96760c500c4c12b9370000000065af0953ecd1"
    "ff1100baff17"
    "8003030900000035303939353334340003";

static std::vector<uint8_t> hex_decode(const std::string& s) {
    std::vector<uint8_t> out;
    out.reserve(s.size() / 2);
    auto nyb = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    };
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        out.push_back(static_cast<uint8_t>((nyb(s[i]) << 4) | nyb(s[i+1])));
    }
    return out;
}

static std::vector<uint8_t> extract_bits(const uint8_t* src,
                                           size_t off, size_t n) {
    std::vector<uint8_t> out; out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        size_t bp = off + i;
        out.push_back(static_cast<uint8_t>((src[bp >> 3] >> (bp & 7)) & 1));
    }
    return out;
}

int main() {
    std::printf("=== Session H.2b NMT_Challenge byte-identity test ===\n");

    auto captured = hex_decode(kCapturedHex);
    std::printf("  captured bytes: %zu\n", captured.size());
// ~ C | O | A - C_Y_B ~

    auto parsed = wire::parse_packet(captured.data(), captured.size(),
                                       wire::Direction::ServerToClient);
    CHECK(parsed.has_value(), "captured packet parses");
    if (!parsed || parsed->bunches.empty()) return 1;
    const auto& cb = parsed->bunches[0];

    std::printf("  bunch: ch=%u chSeq=%u bdb=%u\n",
                 cb.channel, cb.ch_sequence, cb.bunch_data_bits);

    CHECK(cb.channel == 0,            "channel = 0");
    CHECK(cb.ch_sequence == 954,      "ch_sequence = 954");
    CHECK(cb.bunch_data_bits == 112,  "bunch_data_bits = 112");

    // Build our own.
    emit::BunchWriter bw;
    emit::NmtBunchContext ctx;
    ctx.ch_sequence   = 954;
    ctx.opens_channel = false;  // captured bunch had ctrl=0
    size_t bits = emit::NmtBuilder::build_challenge(bw, ctx, "50995344");
    std::printf("  built bits: %zu\n", bits);

    size_t total = cb.header_bits + cb.bunch_data_bits;
    CHECK(bits == total, "built bit count matches captured");

    auto expected = extract_bits(captured.data(),
                                   cb.data_start_bit - cb.header_bits, total);
    auto actual   = extract_bits(bw.data(), 0, bits);
    size_t cmp = std::min(expected.size(), actual.size());
    size_t first = cmp, n_diff = 0;
    for (size_t i = 0; i < cmp; ++i) {
        if (expected[i] != actual[i]) {
            if (first == cmp) first = i;
            ++n_diff;
        }
    }
    if (n_diff == 0) {
        std::printf("  [ok ] BYTE-IDENTICAL: all %zu bits match!\n", cmp);
        g_pass++;
    } else {
        std::printf("  [FAIL] %zu bits differ, first at bit %zu\n", n_diff, first);
        g_fail++;
    }

    std::printf("\n=== Summary ===\n");
    std::printf("  Passed: %d\n  Failed: %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
