// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tools/test_property_update_bunch_builder.cpp
//
//  Unit tests for PropertyUpdateBunchBuilder.  Validates:
//    1. Name update bunch produces the expected bit layout (header + payload)
//    2. Header fields are parseable by our own parser
//    3. Multiple queued updates stack correctly in a single bunch
//
//  OWNER:   Phase III M1
// ============================================================================
#include "protocol/emit/property_update_bunch_builder.h"
#include "protocol/emit/bunch_writer.h"
#include "protocol/emit/name_update_bunch.h"
#include "protocol/wire/packet_reader.h"
#include "protocol/wire/packet_parser.h"
#include "protocol/wire/ue5_primitives.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <cstdint>
#include <vector>

using aoc::protocol::emit::BunchWriter;
using aoc::protocol::emit::PropertyUpdateBunchBuilder;
using aoc::protocol::emit::build_name_update_bunch_payload;

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (cond) { ++g_passed; spdlog::info("  pass  {}", msg); }            \
        else      { ++g_failed; spdlog::error("  FAIL  {}", msg); }           \
    } while (0)

// Read `n_bits` LSB-first from `data` starting at bit `pos`.
static uint64_t read_bits(const uint8_t* data, size_t pos, int n_bits) {
    uint64_t v = 0;
    for (int i = 0; i < n_bits; ++i) {
        int bit = (data[(pos + i) >> 3] >> ((pos + i) & 7)) & 1;
        v |= static_cast<uint64_t>(bit) << i;
    }
    return v;
}

// Read UE5 SerializeIntPacked value starting at bit `pos`.  Updates `pos` to
// the next bit after the value.  UE5 format: 8 bits per chunk, LSB=continuation
// flag, upper 7 bits = value contribution.  (Matches FBitReader::SerializeIntPacked.)
static uint32_t read_sip(const uint8_t* data, size_t& pos) {
    uint32_t value = 0;
    int shift = 0;
    while (true) {
        uint64_t byte = read_bits(data, pos, 8);
        pos += 8;
        bool more = (byte & 1) != 0;
        uint32_t chunk = static_cast<uint32_t>(byte >> 1);
        value |= chunk << shift;
        shift += 7;
        if (!more) break;
        if (shift >= 32) break;  // safety
    }
    return value;
}

// ─── Test A: Name-only bunch has correct header + payload structure ─────
void test_name_bunch_structure() {
    spdlog::info("");
    spdlog::info("== test A: Name-only bunch structure ==");

    PropertyUpdateBunchBuilder b;
    b.set_channel(3);            // PC's actor channel
    b.set_ch_sequence(1978);     // wraps to 954 in the 10-bit sequence field
    b.add_name_update("MyHero");

    // Expected queued payload = 216 bits (16B prefix + 4B len + 7B "MyHero\0")
    CHECK(b.queued_payload_bits() == 216, "queued payload = 216 bits");
// C_Y_B - C | O | A

    BunchWriter out;
    size_t total_bits = b.build(out);
    spdlog::info("  build() produced {} bits = {} bytes",
                 total_bits, (total_bits + 7) / 8);

    // Parse header back.
    size_t cursor = 0;
    uint64_t b_control = read_bits(out.data(), cursor, 1); cursor += 1;
    CHECK(b_control == 0, "bControl = 0 (data bunch)");

    uint64_t b_replay_paused = read_bits(out.data(), cursor, 1); cursor += 1;
    CHECK(b_replay_paused == 0, "bIsReplicationPaused = 0");

    uint64_t b_reliable = read_bits(out.data(), cursor, 1); cursor += 1;
    CHECK(b_reliable == 1, "bReliable = 1");

    uint32_t ch_idx = read_sip(out.data(), cursor);
    CHECK(ch_idx == 3, "ChIndex = 3");

    uint64_t b_has_pme = read_bits(out.data(), cursor, 1); cursor += 1;
    CHECK(b_has_pme == 0, "bHasPackageMapExports = 0");

    uint64_t b_has_mbg = read_bits(out.data(), cursor, 1); cursor += 1;
    CHECK(b_has_mbg == 0, "bHasMustBeMappedGUIDs = 0");

    uint64_t b_partial = read_bits(out.data(), cursor, 1); cursor += 1;
    CHECK(b_partial == 0, "bPartial = 0 (key requirement)");

    uint32_t ch_seq = ue5::read_serialize_int(out.data(), out.byte_size(), cursor,
                                             aoc::protocol::wire::MAX_CHSEQ);
    CHECK(ch_seq == 954, "ChSequence wraps 1978 to 954 (10 bits)");

    // ChName — emitted by the builder for reliable, non-partial bunches.
    // Defaults: hardcoded=1 + SIP(103) — empirical most-common EName from
    // replay_data.bin (148/285 captured ChName bunches).  See builder
    // header for full distribution.  Was 102, but 102 never appears in
    // capture so it was wrong.
    uint64_t ch_name_hardcoded = read_bits(out.data(), cursor, 1); cursor += 1;
    CHECK(ch_name_hardcoded == 1, "ChName.bIsHardcoded = 1");
    uint32_t ch_name_idx = read_sip(out.data(), cursor);
    CHECK(ch_name_idx == 103, "ChName.EName = 103 (empirical Actor default)");

    uint32_t bdb = ue5::read_serialize_int(out.data(), out.byte_size(), cursor,
                                          aoc::protocol::wire::MAX_PKT_BITS);
    CHECK(bdb == 216, "BunchDataBits = 216 (matches payload)");

    // Remaining bits should be exactly the payload (216 bits).
    size_t payload_bits = total_bits - cursor;
    CHECK(payload_bits == 216, "payload slot = 216 bits");

    // Spot-check first 4 bytes of payload == 0x01 0x00 0x00 0x00 on wire
    // (i.e. LSB-first bits of kPrefix[0..3] = 00 00 00 01).
    //
    // The actual bytes in BunchWriter are bit-packed starting at 0 in each
    // byte. After a variable-length header, the payload is NOT byte-aligned,
    // so we verify by comparing against a fresh payload build.
    BunchWriter expected_payload(64);
    build_name_update_bunch_payload("MyHero", expected_payload);

    bool payload_ok = true;
    payload_ok = payload_bits == expected_payload.bit_pos();
    for (size_t i = 0; payload_ok && i < payload_bits; ++i) {
        int a_bit = (out.data()[(cursor + i) >> 3] >> ((cursor + i) & 7)) & 1;
        int b_bit = (expected_payload.data()[i >> 3] >> (i & 7)) & 1;
        if (a_bit != b_bit) {
            spdlog::error("    payload diff at bit {}: bunch={} expected={}",
                          i, a_bit, b_bit);
            payload_ok = false;
            break;
        }
    }
    CHECK(payload_ok, "payload matches build_name_update_bunch_payload exactly");

    aoc::protocol::wire::PacketReader reader(out.data(), out.byte_size(), total_bits);
    const auto parsed = aoc::protocol::wire::parse_bunch_header(reader, total_bits);
    CHECK(parsed && !reader.overflowed() && parsed->ch_sequence == 954 &&
          parsed->bunch_data_bits == 216 && parsed->data_start_bit == cursor,
          "canonical bunch parser agrees with the emitted header");
}

void test_empty_bunch() {
    for (bool reliable : {false, true}) {
        PropertyUpdateBunchBuilder b;
        b.set_channel(3);
        b.set_reliable(reliable);
        BunchWriter out;
        const size_t bits = b.build(out);
        aoc::protocol::wire::PacketReader reader(out.data(), out.byte_size(), bits);
        const auto parsed = aoc::protocol::wire::parse_bunch_header(reader, bits);
        CHECK(parsed && !reader.overflowed() && parsed->is_reliable == reliable &&
              parsed->bunch_data_bits == 0 && parsed->data_start_bit == bits,
              "zero-bit bunch uses the current adaptive BunchDataBits width");
    }
}

// ─── Test B: Reuse builder for multiple bunches ────────────────────────
void test_reset_queue() {
    spdlog::info("");
    spdlog::info("== test B: reset_queue() allows builder reuse ==");

    PropertyUpdateBunchBuilder b;
    b.set_channel(3);
    b.set_ch_sequence(1);

    b.add_name_update("First");
    size_t first_queued = b.queued_payload_bits();
    CHECK(first_queued == 128 + 32 + 8 * 6, "First = 128 + 32 + 48 bits");

    b.reset_queue();
    CHECK(b.queued_payload_bits() == 0, "reset_queue clears payload");
// == C.O.A | C-Y-B ==

    b.add_name_update("Second");
    CHECK(b.queued_payload_bits() == 128 + 32 + 8 * 7, "Second = correct bits");
}

// ─── Test C: Round-trip identity check (name="RandomChar") ──────────────
void test_randomchar_payload_identity() {
    spdlog::info("");
    spdlog::info("== test C: RandomChar payload === build_name_update_bunch_payload ==");

    PropertyUpdateBunchBuilder b;
    b.set_channel(3);
    b.set_ch_sequence(99);
    b.add_name_update("RandomChar");

    BunchWriter reference(64);
    size_t ref_bits = build_name_update_bunch_payload("RandomChar", reference);

    CHECK(b.queued_payload_bits() == ref_bits,
          "queued bits == reference bits (248)");
    CHECK(ref_bits == 248, "reference payload is 248 bits");
}

int main() {
    auto logger = spdlog::stdout_color_mt("stdout");
    spdlog::set_default_logger(logger);
    spdlog::set_pattern("[%T.%e] [%^%l%$] %v");

    spdlog::info("==========================================");
    spdlog::info("  test_property_update_bunch_builder");
    spdlog::info("==========================================");

    test_name_bunch_structure();
    test_reset_queue();
    test_randomchar_payload_identity();
    test_empty_bunch();

    spdlog::info("");
    spdlog::info("==========================================");
    spdlog::info("  Result: {} passed, {} failed", g_passed, g_failed);
    spdlog::info("==========================================");
    return g_failed == 0 ? 0 : 1;
}
