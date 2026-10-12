// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tools/test_player_controller_builder.cpp
//
//  Offline smoke test for the Phase 3.6 PlayerController builder.
//
//  Calls aoc::protocol::actors::player_controller::build() with a default
//  CharacterProfile and compares the output bits against the captured
//  bunch payload (bootstrap packet #22, payload at bit 206, 3302 bits).
//
//  Pass criteria (Phase 3.6 — very relaxed):
//    * Builder returns true without crashing
//    * Output bit count is within a few hundred bits of the captured one
//
//  Future (Phase 3.7):
//    * Output bits MATCH the captured payload for fields we emit from
//      scratch (header + prefix + 3 GUIDs + 4 vector flags).  We'll
//      assert byte-identity once the vectors are decoded precisely.
// ============================================================================
#include "protocol/actors/player_controller.h"
#include "protocol/bootstrap/bootstrap_data.h"

#include <cstdio>

int main() {
    using namespace aoc::protocol;

    CharacterProfile profile;
    profile.name = "RandomChar";  // match the capture for a baseline test

    BunchBuffer out;
    const bool ok = actors::player_controller::build(out, profile);
    if (!ok) {
        std::fprintf(stderr, "[FAIL] build() returned false\n");
        return 1;
    }

    const std::size_t built_bits = out.bit_count();
    const std::size_t built_bytes = out.byte_size();

    // Expected bit count based on our decoded model:
    //   1   (bHasRepLayoutExport)
    // + 32  (NumExports)
    // + 411 (export mask)
    // + GUIDs + vectors (variable, ~275 bits for captured values)
    // + 2582 (property-values splice)
    constexpr std::size_t kCapturedPayloadBits = 3302u;

    std::printf("[OK] PlayerController build() returned true\n");
    std::printf("     Output bits : %zu\n", built_bits);
    std::printf("     Output bytes: %zu\n", built_bytes);
    std::printf("     Expected    : ~%zu (captured bunch payload)\n",
                kCapturedPayloadBits);
// C_Y_B x C O A

    const std::ptrdiff_t diff =
        static_cast<std::ptrdiff_t>(built_bits) -
        static_cast<std::ptrdiff_t>(kCapturedPayloadBits);
    std::printf("     Diff        : %td bits\n", diff);

    if (std::abs(diff) > 200) {
        std::fprintf(stderr,
            "[WARN] Builder output differs from captured by >200 bits.\n"
            "       Expected for Phase 3.6 (GUID/vector formats still rough).\n"
            "       Phase 3.7 will close the gap.\n");
    }

    // ──────────────────────────────────────────────────────────────────
    // Proper bit-level comparison: for each bit position, compare the
    // builder's output bit to the bit at the corresponding payload
    // position in the captured packet (bunch payload starts at bit 206
    // of raw packet #22).
    // ──────────────────────────────────────────────────────────────────
    const auto& captured = bootstrap_data::kPackets[22];
    constexpr std::size_t kCapturedPayloadStartBit = 206u;

    auto bit_of = [](const uint8_t* buf, std::size_t size, std::size_t bp) -> int {
        if ((bp >> 3) >= size) return -1;
        return (buf[bp >> 3] >> (bp & 7)) & 1;
    };

    std::size_t compare_bits = std::min<std::size_t>(built_bits, kCapturedPayloadBits);
    std::size_t first_mismatch = compare_bits;
    std::size_t total_mismatches = 0;
    for (std::size_t i = 0; i < compare_bits; ++i) {
        const int b_bit = bit_of(out.bytes().data(), built_bytes, i);
        const int c_bit = bit_of(captured.raw, captured.raw_size,
                                 kCapturedPayloadStartBit + i);
        if (b_bit != c_bit) {
            if (first_mismatch == compare_bits) first_mismatch = i;
            ++total_mismatches;
        }
    }

    std::printf("\n=== Bit-level compare vs captured ===\n");
    std::printf("  Compared : %zu bits\n", compare_bits);
    std::printf("  Matches  : %zu bits (%.1f%%)\n",
                compare_bits - total_mismatches,
                100.0 * (compare_bits - total_mismatches) / double(compare_bits));
    std::printf("  Mismatch : %zu bits\n", total_mismatches);
    if (total_mismatches == 0) {
        std::printf("  [PASS] BYTE-IDENTICAL OUTPUT — builder matches captured bunch!\n");
    } else {
        std::printf("  First mismatch at bit %zu\n", first_mismatch);
        // Show a 32-bit window around the first mismatch for context
        std::printf("  Context around first mismatch (bits %zu..%zu):\n",
                    first_mismatch,
                    std::min(compare_bits - 1, first_mismatch + 31));
        std::printf("    builder : ");
        for (std::size_t i = first_mismatch;
             i < compare_bits && i < first_mismatch + 32; ++i)
            std::printf("%d", bit_of(out.bytes().data(), built_bytes, i));
        std::printf("\n    captured: ");
        for (std::size_t i = first_mismatch;
             i < compare_bits && i < first_mismatch + 32; ++i)
            std::printf("%d",
                bit_of(captured.raw, captured.raw_size,
                       kCapturedPayloadStartBit + i));
        std::printf("\n");
    }

    return total_mismatches == 0 ? 0 : 2;
}
