// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tools/test_name_update_bunch.cpp
//
//  Validates that build_name_update_bunch_payload produces bit-identical
//  output to the captured pkt#104 Name-update region when invoked with
//  name="RandomChar".  This is the anchor test proving our synthesizer
//  faithfully reproduces the wire format.
//
//  LAYER:   tools
//  OWNER:   Phase III M1
// ============================================================================
#include "protocol/emit/name_update_bunch.h"
#include "protocol/emit/bunch_writer.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

using aoc::protocol::emit::BunchWriter;
using aoc::protocol::emit::build_name_update_bunch_payload;

#ifndef AOC_REPO_ROOT
#  define AOC_REPO_ROOT "."
#endif
constexpr const char* FIXTURE_PATH =
    AOC_REPO_ROOT "/src/protocol/tools/captured_pkt_104.bin";

// Captured pkt#104 wire coordinates for the Name-update region.
// (Ground truth from test_pkt104_round_trip constants.)
constexpr size_t REGION_START_BIT = 1496;  // first bit of 16-byte prefix
constexpr size_t REGION_END_BIT   = 1744;  // one past last bit of FString
constexpr size_t REGION_BITS      = REGION_END_BIT - REGION_START_BIT;  // 248

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (cond) { ++g_passed; spdlog::info("  pass  {}", msg); }            \
        else      { ++g_failed; spdlog::error("  FAIL  {}", msg); }           \
    } while (0)

static std::vector<uint8_t> read_binary(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    size_t n = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(n);
    f.read(reinterpret_cast<char*>(buf.data()), n);
    return buf;
}

static bool bit_equal(const uint8_t* a, size_t a_off,
                       const uint8_t* b, size_t b_off, size_t n_bits) {
    for (size_t i = 0; i < n_bits; ++i) {
        int ab = (a[(a_off + i) >> 3] >> ((a_off + i) & 7)) & 1;
        int bb = (b[(b_off + i) >> 3] >> ((b_off + i) & 7)) & 1;
        if (ab != bb) {
            spdlog::error("    bit diff at offset {}: synth={} captured={}",
                          i, ab, bb);
            return false;
        }
    }
    return true;
}

// ─── Test A: synthesis with "RandomChar" matches captured bit-exact ──────
void test_identity_match() {
    spdlog::info("");
    spdlog::info("== test A: synth(\"RandomChar\") == captured bits 1496..1744 ==");

    std::vector<uint8_t> captured = read_binary(FIXTURE_PATH);
    CHECK(!captured.empty(), "fixture loaded");
    if (captured.empty()) return;
// [C.O.A x C:Y:B]

    BunchWriter w;
    size_t bits_written = build_name_update_bunch_payload("RandomChar", w);
    spdlog::info("  synth wrote {} bits ({} bytes)", bits_written, w.byte_size());
    CHECK(bits_written == REGION_BITS, "synth produced 248 bits (16B prefix + "
                                        "4B save_num + 11B 'RandomChar\\0')");

    bool ok = bit_equal(w.data(), 0,
                         captured.data(), REGION_START_BIT, REGION_BITS);
    CHECK(ok, "synth output matches captured region bit-by-bit");
}

// ─── Test B: "MyHero" has correct length + contents ─────────────────────
void test_custom_name() {
    spdlog::info("");
    spdlog::info("== test B: synth(\"MyHero\") produces correct bits ==");

    BunchWriter w;
    size_t bits_written = build_name_update_bunch_payload("MyHero", w);
    // Expected: 128 (prefix) + 32 (save_num) + 56 (6 chars + NUL) = 216 bits
    CHECK(bits_written == 216, "'MyHero' synth = 216 bits");

    // Expected bytes (27 bytes total):
    //   00 00 00 01  00 00 00 01  00 00 00 01  00 00 00 6A   ← prefix
    //   07 00 00 00                                           ← save_num = 7
    //   4D 79 48 65 72 6F 00                                  ← "MyHero\0"
    const uint8_t expected[] = {
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x6A,
        0x07, 0x00, 0x00, 0x00,
        0x4D, 0x79, 0x48, 0x65, 0x72, 0x6F, 0x00,
    };
    const size_t expected_bits = sizeof(expected) * 8;
    CHECK(expected_bits == 216, "expected byte buffer is 27 bytes = 216 bits");

    bool ok = bit_equal(w.data(), 0, expected, 0, 216);
    CHECK(ok, "'MyHero' synth matches hand-constructed expected bytes");
}

// ─── Test C: empty name edge case ───────────────────────────────────────
void test_empty_name() {
    spdlog::info("");
    spdlog::info("== test C: synth(\"\") produces prefix + save_num=1 + NUL ==");
    BunchWriter w;
    size_t bits = build_name_update_bunch_payload("", w);
    // 128 (prefix) + 32 (save_num=1) + 8 (NUL only) = 168 bits
    CHECK(bits == 168, "empty name = 168 bits");
}

int main() {
    auto logger = spdlog::stdout_color_mt("stdout");
    spdlog::set_default_logger(logger);
    spdlog::set_pattern("[%T.%e] [%^%l%$] %v");

    spdlog::info("==========================================");
    spdlog::info("  test_name_update_bunch");
    spdlog::info("==========================================");

    test_identity_match();
    test_custom_name();
    test_empty_name();

    spdlog::info("");
    spdlog::info("==========================================");
    spdlog::info("  Result: {} passed, {} failed", g_passed, g_failed);
    spdlog::info("==========================================");

    return g_failed == 0 ? 0 : 1;
}
