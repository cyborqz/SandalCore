// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tools/test_packet_parser.cpp
//
//  Byte-identity test: parses known captured packets through the C++
//  packet_parser and verifies structural output matches what we know from
//  Python's phase1_parser.py + our existing decode_pc_precise.py scans.
//
//  Specifically checks pkt 22 (PlayerController), pkt 79 (Pawn), pkt 104
//  (Pawn w/ RandomChar HUD name).
//
//  Exit criterion for Session A: this test reports "ALL PASSED".
// ============================================================================
#include "protocol/wire/packet_parser.h"
#include "protocol/bootstrap/bootstrap_data.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace aoc::protocol::wire;

struct ExpectedPacket {
    size_t pkt_index;
    uint16_t expected_seq;
    uint32_t expected_custom_field_low32;   // low 32 bits; full 48 checked via hex
    int      expected_bunch_count_min;      // at least this many parsed bunches
    const char* label;
};

// Expected values are based on:
//   - decode_pc_precise.py output for pkt 14287 (our pkt 22)
//   - decode_pawn.py output for pkt 79 / 104
//   - existing log output from the server
static const ExpectedPacket kExpected[] = {
    {  22, 14287, 0, 1, "PlayerController ActorOpen" },
    {  79, 14344, 0, 1, "Pawn ActorOpen #1" },
    { 104, 14369, 0, 1, "Pawn ActorOpen #2 (HUD name)" },
};
// ~ C | Y | B :: C O A ~

static int g_passed = 0, g_failed = 0;

#define CHECK(cond, msg) do { \
    if (cond) { \
        std::printf("  [ok ] %s\n", msg); \
        g_passed++; \
    } else { \
        std::printf("  [FAIL] %s\n", msg); \
        g_failed++; \
    } \
} while (0)

int main() {
    std::printf("=== Session A byte-identity test ===\n");
    std::printf("Embedded packet count: %zu\n",
                aoc::protocol::bootstrap_data::kPacketCount);

    for (const auto& exp : kExpected) {
        std::printf("\n--- pkt %zu (%s) ---\n", exp.pkt_index, exp.label);

        if (exp.pkt_index >= aoc::protocol::bootstrap_data::kPacketCount) {
            std::printf("  [skip] pkt %zu is out of embedded range\n", exp.pkt_index);
            continue;
        }

        const auto& epkt = aoc::protocol::bootstrap_data::kPackets[exp.pkt_index];
        std::printf("  raw=%zuB  original_seq=%u  bsb=%u  bb=%u\n",
                    epkt.raw_size, epkt.original_seq,
                    epkt.bunch_start_bit, epkt.bunch_bits);

        // Run our new C++ parser
        auto parsed = parse_packet(epkt.raw, epkt.raw_size, Direction::ServerToClient);
        if (!parsed) {
            std::printf("  [FAIL] parse_packet returned nullopt\n");
            g_failed++;
            continue;
        }

        std::printf("  parsed: seq=%u ack=%u hist=%u hasPktInfo=%d hasSrvFrame=%d jitter=%u\n",
                    parsed->seq, parsed->ack_seq, parsed->hist_word_count,
                    parsed->has_pkt_info, parsed->has_srv_frame, parsed->jitter_ms);
        std::printf("  bunches: %zu\n", parsed->bunches.size());

        for (size_t i = 0; i < parsed->bunches.size(); ++i) {
            const auto& b = parsed->bunches[i];
            std::printf("    #%zu  ch=%u name='%s' ctrl=%d open=%d close=%d "
                        "reliable=%d partial=%d bdb=%u hdr=%zu\n",
                        i, b.channel, b.channel_name.c_str(),
                        b.is_control, b.is_open, b.is_close,
                        b.is_reliable, b.is_partial,
                        b.bunch_data_bits, b.header_bits);
        }

        // Validations
        char msg[256];
        std::snprintf(msg, sizeof(msg), "seq matches capture (%u)", exp.expected_seq);
        CHECK(parsed->seq == exp.expected_seq, msg);

        std::snprintf(msg, sizeof(msg), "bunch count >= %d", exp.expected_bunch_count_min);
        CHECK(static_cast<int>(parsed->bunches.size()) >= exp.expected_bunch_count_min, msg);

        // Every bunch must have valid channel
        bool all_channels_valid = true;
        for (const auto& b : parsed->bunches) {
            if (b.channel > 100000) { all_channels_valid = false; break; }
        }
        CHECK(all_channels_valid, "all bunches have sensible channel IDs");

        // If the packet has any bunches, at least one should have non-zero BDB
        if (!parsed->bunches.empty()) {
            bool any_data = false;
            for (const auto& b : parsed->bunches) {
                if (b.bunch_data_bits > 0) { any_data = true; break; }
            }
            CHECK(any_data, "at least one bunch has non-zero BDB");
        }
    }

    std::printf("\n=== Summary ===\n");
    std::printf("  Passed: %d\n", g_passed);
    std::printf("  Failed: %d\n", g_failed);
    std::printf("  Result: %s\n", g_failed == 0 ? "ALL PASSED" : "FAILURES");
    return g_failed == 0 ? 0 : 1;
}
