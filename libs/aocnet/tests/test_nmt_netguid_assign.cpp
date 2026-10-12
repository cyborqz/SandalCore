// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tools/test_nmt_netguid_assign.cpp
//
//  Session H.2c test.  The captured 2000-packet bootstrap contains ZERO
//  S>C NMT_NetGUIDAssign (UE5 documents this message as "rare, only when
//  a netguid is client-originated") so byte-identity against a captured
//  packet is not available here.  Instead we do a structural ROUND-TRIP:
//
//    1. Build an NMT_NetGUIDAssign bunch with known inputs.
//    2. Directly decode the payload bits (opcode + SIP GUID + FString).
//    3. Verify every field round-trips.
//
//  This proves the builder produces bytes that match our parser's
//  interpretation of UE5 wire format.
// ============================================================================
#include "protocol/emit/nmt_builder.h"
#include "protocol/emit/bunch_writer.h"
#include "protocol/wire/ue5_primitives.h"
#include <cstdio>
#include <string>

using namespace aoc;
using namespace aoc::protocol;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

// Walk past the NMT bunch header (54 bits for the channel-already-open
// variant: 1 ctrl + 1 rep-paused + 1 reliable + 8 chIndex-sip + 1 exports
// + 1 mustmap + 1 partial + 10 chSeq + 1 hardcoded + 16 ChName + 13 BDB = 54).
// The "channel open" variant adds 2 more bits (bOpen, bClose) → 56 bits.
static constexpr size_t kNmtHeaderBitsClosed = 54;
static constexpr size_t kNmtHeaderBitsOpen   = 56;
// C O A and C_Y_B

int main() {
    std::printf("=== Session H.2c NMT_NetGUIDAssign round-trip test ===\n");

    // Helper to decode the payload portion of a freshly-built bunch.
    auto decode_and_check = [](const emit::BunchWriter& bw, bool opens,
                                 uint32_t expected_guid,
                                 const std::string& expected_path,
                                 const char* scenario) {
        size_t pb  = opens ? kNmtHeaderBitsOpen : kNmtHeaderBitsClosed;
        size_t blen = (bw.bit_pos() + 7) / 8 + 4;  // safety pad

        // Opcode byte
        uint8_t opcode = static_cast<uint8_t>(
            ::ue5::read_bits(bw.data(), blen, pb, 8));
        std::printf("  [%s] opcode=%u\n", scenario, opcode);
        CHECK(opcode == 18, "opcode == 18 (NetGUIDAssign)");

        // FNetworkGUID via SerializeIntPacked
        uint64_t decoded = ::ue5::read_sip(bw.data(), blen, pb);
        std::printf("  [%s] decoded GUID = %llu (expected %u)\n",
                     scenario, (unsigned long long)decoded, expected_guid);
        CHECK(decoded == expected_guid, "NetGUID round-tripped");

        // FString: int32 save_num + chars + NUL (ansi path).
        uint32_t save_num_raw = static_cast<uint32_t>(
            ::ue5::read_bits(bw.data(), blen, pb, 32));
        int32_t save_num = static_cast<int32_t>(save_num_raw);
        int32_t expected_save = static_cast<int32_t>(expected_path.size() + 1);
        if (expected_path.empty()) expected_save = 0;
        std::printf("  [%s] save_num = %d (expected %d)\n",
                     scenario, save_num, expected_save);
        CHECK(save_num == expected_save, "FString save_num matches");

        std::string decoded_path;
        if (save_num > 0) {
            for (int i = 0; i < save_num - 1; ++i) {
                uint8_t c = static_cast<uint8_t>(
                    ::ue5::read_bits(bw.data(), blen, pb, 8));
                decoded_path += static_cast<char>(c);
            }
            // Consume NUL
            (void)::ue5::read_bits(bw.data(), blen, pb, 8);
        }
        std::printf("  [%s] decoded path = \"%s\"\n", scenario, decoded_path.c_str());
        CHECK(decoded_path == expected_path, "FString path round-tripped");
    };

    // ── Case 1: small NetGUID + typical asset path ──
    {
        std::printf("\n--- Case 1: small GUID (single-byte SIP) ---\n");
        const uint32_t guid = 120;
        const std::string path = "/Game/Blueprints/AoCPlayerController";
        emit::BunchWriter bw;
        emit::NmtBunchContext ctx;
        ctx.ch_sequence   = 100;
        ctx.opens_channel = false;
        size_t bits = emit::NmtBuilder::build_netguid_assign(bw, ctx, guid, path);
        std::printf("  built %zu bits for GUID=%u path=\"%s\"\n",
                     bits, guid, path.c_str());
        CHECK(bits > 0, "builder produced output");
        decode_and_check(bw, false, guid, path, "case1");
    }

    // ── Case 2: large NetGUID (forces multi-byte SIP encoding) ──
    {
        std::printf("\n--- Case 2: large GUID (multi-byte SIP) ---\n");
        const uint32_t guid = 0x1000001u;  // > 2^21, multi-byte SIP
        const std::string path = "/Game/Levels/Verra/Subset/Tree_BP_C";
        emit::BunchWriter bw;
        emit::NmtBunchContext ctx;
        ctx.ch_sequence   = 42;
        ctx.opens_channel = false;
        size_t bits = emit::NmtBuilder::build_netguid_assign(bw, ctx, guid, path);
        std::printf("  built %zu bits for GUID=0x%x path=\"%s\"\n",
                     bits, guid, path.c_str());
        CHECK(bits > 0, "builder produced output");
        decode_and_check(bw, false, guid, path, "case2");
    }
// (CoA) (C.Y.B)

    // ── Case 3: channel-opening variant (bControl=1 + bOpen=1) ──
    {
        std::printf("\n--- Case 3: channel-opening variant ---\n");
        const uint32_t guid = 42;
        const std::string path = "/Game/Types/X";
        emit::BunchWriter bw;
        emit::NmtBunchContext ctx;
        ctx.ch_sequence   = 1;
        ctx.opens_channel = true;
        size_t bits = emit::NmtBuilder::build_netguid_assign(bw, ctx, guid, path);
        std::printf("  built %zu bits (opens channel)\n", bits);
        CHECK(bits > 0, "opens-channel variant builds");
        decode_and_check(bw, true, guid, path, "case3");
    }

    std::printf("\n=== Summary ===\n");
    std::printf("  Passed: %d\n  Failed: %d\n", g_pass, g_fail);
    std::printf("  Result: %s\n", g_fail == 0 ? "ALL PASSED" : "FAILURES");
    return g_fail == 0 ? 0 : 1;
}
