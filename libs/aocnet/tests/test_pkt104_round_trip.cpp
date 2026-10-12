// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tools/test_pkt104_round_trip.cpp
//
//  pkt#104 (HUD character name property update) round-trip harness.
//
//  Goal: prove we can DECODE + RE-ENCODE the Name FString property inside
//  the captured pkt#104 bunch with bit-identical output.  This is the
//  easier sibling of test_pkt22_round_trip.cpp — we're not parsing the
//  whole bunch, just the FString region.
//
//  pkt#104 structure (derived from old patcher constants + verification):
//
//     raw_size          : 978 B (7824 bits)
//     bunch_start_bit   : 152
//     bunch_bits        : 7665  (bunch ends at bit 7817)
//     Name cmd_index    : bit 1592 (32 bits, LSB-first)  ← assumed
//     Name save_num     : bit 1624 (int32 = 11, ASCII path)
//     Name bytes        : bit 1656 ("RandomChar" + NUL, 11 bytes = 88 bits)
//     Name end          : bit 1744
//
//  So the "FString region" we round-trip is bits 1624..1744 (120 bits):
//     32 bits save_num + 88 bits FString bytes.
//
//  The cmd_index at bit 1592 is a separate preamble — we read it for
//  identification but don't yet validate its interpretation.
//
//  Fixture: src/protocol/tools/captured_pkt_104.bin
//           (extract via extract_pkt_fixture.py 104 captured_pkt_104)
//
//  PHASE 1 (this file):
//    * Load fixture
//    * Verify header/bunch metadata
//    * Read cmd_index at bit 1592
//    * Decode FString at bit 1624 via our FString codec
//    * Re-encode via our FString codec
//    * Verify bit-identical with captured bytes 1624..1744
//    * If clean: attempt MUTATION (FString = "Bob") — observe byte shift
//
//  Success criterion: "RandomChar" FString encodes to exactly the same
//  120 bits we decoded.  Mutation to "Bob" produces a 56-bit string
//  (32 bits save_num=4 + 4 bytes "Bob\0" = 56 bits total).
//
//  LAYER:  tools
// ============================================================================
#include "protocol/emit/replayout/encoders/fstring_codec.h"
#include "protocol/emit/bunch_writer.h"
#include "protocol/wire/packet_reader.h"
// C-O-A : C Y B

#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h"

#include <cstdio>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace aoc::protocol::emit::replayout;
using aoc::protocol::emit::BunchWriter;
using aoc::protocol::wire::PacketReader;

constexpr const char* FIXTURE_PATH =
    "src\\protocol\\tools\\captured_pkt_104.bin";

constexpr size_t RAW_BITS            = 7824;
constexpr size_t BUNCH_START         = 152;
constexpr size_t BUNCH_BITS          = 7665;
constexpr size_t BUNCH_END           = 7817;
constexpr size_t NAME_CMD_BIT        = 1592;   // uint32 cmd_index
constexpr size_t NAME_LEN_BIT        = 1624;   // int32 save_num
constexpr size_t NAME_BYTES_BIT      = 1656;   // first byte of FString payload
constexpr size_t NAME_END_BIT        = 1744;   // after 11 bytes (10 chars + NUL)

int g_passed = 0;
int g_failed = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                      \
        if (cond) {                                                           \
            ++g_passed;                                                       \
        } else {                                                              \
            spdlog::error("  FAIL  {}: {}", __FUNCTION__, msg);               \
            ++g_failed;                                                       \
        }                                                                     \
    } while (0)

static std::vector<uint8_t> read_binary(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    auto n = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(n);
    f.read(reinterpret_cast<char*>(buf.data()), n);
    return buf;
}

static uint32_t read_bits_at(const uint8_t* data, size_t len, size_t bit_off, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; ++i) {
        size_t bp = bit_off + i;
        if ((bp >> 3) >= len) break;
        int bit = (data[bp >> 3] >> (bp & 7)) & 1;
        v |= static_cast<uint32_t>(bit) << i;
    }
    return v;
}

/// Compare two bit ranges for equality.  `a`/`b` are LSB-first per byte.
static bool bit_range_equal(const uint8_t* a, size_t a_bit_off,
                             const uint8_t* b, size_t b_bit_off,
                             size_t n_bits) {
    for (size_t i = 0; i < n_bits; ++i) {
        size_t ap = a_bit_off + i;
        size_t bp = b_bit_off + i;
        int ab = (a[ap >> 3] >> (ap & 7)) & 1;
        int bb = (b[bp >> 3] >> (bp & 7)) & 1;
        if (ab != bb) {
            spdlog::error("  bit diff at offset {}: a={} b={}", i, ab, bb);
            return false;
        }
    }
    return true;
}

static std::string bits_to_string(const uint8_t* data, size_t bit_off, size_t n) {
    std::string out;
    out.reserve(n + n / 8);
    for (size_t i = 0; i < n; ++i) {
        if (i > 0 && (i & 7) == 0) out += ' ';
        size_t bp = bit_off + i;
        int bit = (data[bp >> 3] >> (bp & 7)) & 1;
        out += (bit ? '1' : '0');
    }
    return out;
}

// ─── Phase 1: metadata verification ──────────────────────────────────────

void phase1_verify(const std::vector<uint8_t>& data) {
    spdlog::info("== fixture ==");
    spdlog::info("  path  : {}", FIXTURE_PATH);
    spdlog::info("  size  : {} bytes ({} bits, expected {})",
                 data.size(), data.size() * 8, RAW_BITS);
    CHECK(data.size() * 8 == RAW_BITS, "raw size matches expected");

    // Read save_num at NAME_LEN_BIT and verify = 11
    uint32_t save_num_raw = read_bits_at(data.data(), data.size(), NAME_LEN_BIT, 32);
    int32_t save_num = static_cast<int32_t>(save_num_raw);
    spdlog::info("  save_num @ bit {} = {} (expected 11 for ASCII 'RandomChar\\0')",
                 NAME_LEN_BIT, save_num);
    CHECK(save_num == 11, "save_num = 11");
// -- C.O.A . Cyb --

    // Read cmd_index at NAME_CMD_BIT — we don't yet know what the expected
    // value is, but log it for future RE.
    uint32_t cmd_idx = read_bits_at(data.data(), data.size(), NAME_CMD_BIT, 32);
    spdlog::info("  cmd_index @ bit {} = {} (0x{:08X})",
                 NAME_CMD_BIT, cmd_idx, cmd_idx);
    spdlog::info("  (this cmd_index is whatever pkt#104's stream identifies");
    spdlog::info("   Name with; TBD once we decode more of pkt#104's format)");

    // Read first 16 bytes of the ASCII string region and ensure it spells
    // "RandomChar\0" bytes.
    std::string name;
    for (int i = 0; i < 10; ++i) {
        uint32_t b = read_bits_at(data.data(), data.size(),
                                    NAME_BYTES_BIT + i * 8, 8);
        name.push_back(static_cast<char>(b));
    }
    spdlog::info("  name @ bit {} = \"{}\" (expected 'RandomChar')",
                 NAME_BYTES_BIT, name);
    CHECK(name == "RandomChar", "ASCII string matches");

    uint32_t nul_byte = read_bits_at(data.data(), data.size(),
                                       NAME_BYTES_BIT + 10 * 8, 8);
    CHECK(nul_byte == 0, "NUL terminator at end");
}

// ─── Phase 2: surgical round-trip of the FString region ────────────────

void phase2_round_trip(const std::vector<uint8_t>& data) {
    spdlog::info("");
    spdlog::info("== phase 2: FString region round-trip (bit {}..{}) ==",
                 NAME_LEN_BIT, NAME_END_BIT);

    // ── Decode
    PacketReader reader(data.data(), data.size(), data.size() * 8);
    for (size_t i = 0; i < NAME_LEN_BIT; ++i) (void)reader.read_bit();

    PropertyValue decoded = decode_fstring(reader);
    const size_t consumed_bits = reader.pos() - NAME_LEN_BIT;
    spdlog::info("  decode consumed {} bits", consumed_bits);
    CHECK(consumed_bits == (NAME_END_BIT - NAME_LEN_BIT),
          "decode consumed 120 bits (4 byte len + 11 bytes)");

    const std::string* s = std::get_if<std::string>(&decoded.payload);
    CHECK(s && *s == "RandomChar", "decoded FString = 'RandomChar'");

    // ── Re-encode
    BunchWriter writer;
    bool ok = encode_fstring(decoded, writer);
    CHECK(ok, "encode succeeded");
    spdlog::info("  encode produced {} bits", writer.bit_pos());
    CHECK(writer.bit_pos() == (NAME_END_BIT - NAME_LEN_BIT),
          "encode produced 120 bits");

    // ── Bit-identical check
    bool identical = bit_range_equal(
        writer.data(),    0,
        data.data(),      NAME_LEN_BIT,
        writer.bit_pos());
    CHECK(identical, "encode == captured bits (byte-for-byte)");

    if (!identical) {
        spdlog::error("  captured bits:  {}",
                      bits_to_string(data.data(), NAME_LEN_BIT, writer.bit_pos()));
        spdlog::error("  encoded bits:   {}",
                      bits_to_string(writer.data(), 0, writer.bit_pos()));
    } else {
        spdlog::info("  FString region round-trips BIT-IDENTICAL ({} bits).",
                     writer.bit_pos());
    }
}

// ─── Phase 3: mutation — replace "RandomChar" with "Bob" ───────────────

void phase3_mutation(const std::vector<uint8_t>& data) {
    spdlog::info("");
    spdlog::info("== phase 3: mutation — Name = \"Bob\" ==");

    // Decode, mutate, re-encode
    PacketReader reader(data.data(), data.size(), data.size() * 8);
    for (size_t i = 0; i < NAME_LEN_BIT; ++i) (void)reader.read_bit();
    PropertyValue decoded = decode_fstring(reader);

    // Mutate
    decoded = PropertyValue::make_string("Bob");

    // Encode
    BunchWriter writer;
    bool ok = encode_fstring(decoded, writer);
    CHECK(ok, "encode 'Bob' succeeded");

    // Expected: 32 bits save_num (=4) + 4 bytes "Bob\0" (=32 bits) = 64 bits total
    const size_t expected = 32 + 4 * 8;
    spdlog::info("  encoded {} bits (expected {} for 'Bob' + NUL)",
                 writer.bit_pos(), expected);
    CHECK(writer.bit_pos() == expected, "Bob encodes to 64 bits");

    // Dump the encoded bits for eyeballing
    spdlog::info("  encoded: {}",
                 bits_to_string(writer.data(), 0, writer.bit_pos()));

    // Verify first 32 bits = save_num = 4
    uint32_t sn = read_bits_at(writer.data(), writer.byte_size(), 0, 32);
    spdlog::info("  save_num in encoded = {} (expected 4)", sn);
    CHECK(sn == 4, "save_num = 4 for 'Bob'");

    // Verify the characters
    std::string recovered;
    for (int i = 0; i < 3; ++i) {
        uint32_t b = read_bits_at(writer.data(), writer.byte_size(),
                                    32 + i * 8, 8);
        recovered.push_back(static_cast<char>(b));
    }
    uint32_t nul = read_bits_at(writer.data(), writer.byte_size(), 32 + 24, 8);
    spdlog::info("  recovered chars = \"{}\" (expected 'Bob'), NUL={}",
                 recovered, nul);
    CHECK(recovered == "Bob", "recovered chars = 'Bob'");
    CHECK(nul == 0, "terminator NUL");

    // Difference from captured: -56 bits (120 original → 64 new)
    spdlog::info("  bit-length delta: {} -> {}  ({:+d} bits)",
                 static_cast<int>(NAME_END_BIT - NAME_LEN_BIT),
                 static_cast<int>(writer.bit_pos()),
                 static_cast<int>(writer.bit_pos()) -
                 static_cast<int>(NAME_END_BIT - NAME_LEN_BIT));
    spdlog::info("  (to send live: subsequent bits in pkt#104 must be");
    spdlog::info("   shifted by this delta AND BDB field updated in the");
    spdlog::info("   bunch header — next milestone)");
}

} // namespace

int main() {
    auto logger = spdlog::stdout_color_mt("test_pkt104_round_trip");
    spdlog::set_default_logger(logger);
    spdlog::set_pattern("[%^%l%$] %v");

    spdlog::info("================================================================");
    spdlog::info("  pkt#104 (HUD character name) round-trip harness");
    spdlog::info("================================================================");

    auto data = read_binary(FIXTURE_PATH);
    if (data.empty()) {
        spdlog::error("Could not read fixture: {}", FIXTURE_PATH);
        spdlog::error("Extract with: python src/protocol/tools/extract_pkt_fixture.py 104 captured_pkt_104");
        return 1;
    }

    phase1_verify(data);
    phase2_round_trip(data);
    phase3_mutation(data);

    spdlog::info("");
    spdlog::info("================================================================");
    spdlog::info("  Result: {} passed, {} failed", g_passed, g_failed);
    spdlog::info("================================================================");
    return g_failed == 0 ? 0 : 1;
}
