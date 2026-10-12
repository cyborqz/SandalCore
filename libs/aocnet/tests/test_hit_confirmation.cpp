// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tools/test_hit_confirmation.cpp
//
//  AAoCPlayerController::ClientHitConfirmationsReliable (PC field 171) and the
//  per-hit SerializedData blob.  The blob is read back the way the client's
//  handler does it (sub_146A99840 -> sub_1463E2F90 -> sub_1463E1750 ->
//  sub_1463E1870): a plain FBitReader over the bytes, FHitInst fields via
//  FArchive (int64 = 64 bits, bool = uint32), NetGUIDs as 4x uint32
//  (sub_14141E960), Targets count via SerializeBits(7) clamped to 40, then
//  bPredictedOnAutonomous / bPredictedOnSimulated as uint32 bools.
// ============================================================================
#include "protocol/wire/hit_confirmation.h"
#include <cstdio>
#include <vector>

using namespace aoc::protocol::wire;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

static uint64_t rd(const std::vector<uint8_t>& b, size_t& off, int n) {
    return ::ue5::read_bits(b.data(), b.size(), off, n);
}

int main() {
    std::printf("=== ClientHitConfirmations test ===\n");

    HitConfirmation hc;
    hc.inst_guid          = 0x1122334455667788ULL;
    hc.record_guid        = 0x0102030405060708ULL;
    hc.source_record_type = 0x01c5;
    hc.source_record_guid = 0xa1a2a3a4a5a6a7a8ULL;
    hc.pvp_force          = false;
    hc.instigator         = {88, 0, 60, 0x6ee87ee4};
    hc.source             = {88, 0, 60, 0x6ee87ee4};
    hc.predicted_on_autonomous = true;
    hc.predicted_on_simulated  = false;

    std::printf("\n[1] SerializedData blob, read like the client\n");
    const std::vector<uint8_t> blob = serialize_hit_confirmation(hc);
    {
        size_t o = 0;
        CHECK(rd(blob, o, 64) == hc.inst_guid,          "InstGuid");
        CHECK(rd(blob, o, 64) == hc.record_guid,        "RecordGuid");
        CHECK(rd(blob, o, 64) == hc.source_record_type, "SourceRecordType");
        CHECK(rd(blob, o, 64) == hc.source_record_guid, "SourceRecordGuid");
        CHECK(rd(blob, o, 32) == 0,                     "bPvPForceFlag as uint32 0");
        bool inst = rd(blob, o, 32) == 88 && rd(blob, o, 32) == 0 &&
                    rd(blob, o, 32) == 60 && rd(blob, o, 32) == 0x6ee87ee4;
        CHECK(inst, "Instigator NetGUID as 4x uint32");
        bool src = rd(blob, o, 32) == 88 && rd(blob, o, 32) == 0 &&
                   rd(blob, o, 32) == 60 && rd(blob, o, 32) == 0x6ee87ee4;
        CHECK(src, "Source NetGUID as 4x uint32");
        CHECK(rd(blob, o, 7) == 0,  "Targets count (7 bits) == 0");
        CHECK(rd(blob, o, 32) == 1, "bPredictedOnAutonomous as uint32 1");
        CHECK(rd(blob, o, 32) == 0, "bPredictedOnSimulated as uint32 0");
        CHECK(o == 615, "615 bits consumed");
        CHECK(blob.size() == 77, "blob padded to 77 bytes");
    }
// == CoA | C.Y.B ==

    std::printf("\n[2] ClientHitConfirmationsReliable content block\n");
    {
        uint8_t buf[512] = {};
        size_t off = 0;
        size_t bits = write_client_hit_confirmations(buf, sizeof(buf), off, {blob}, /*reliable=*/true);
        CHECK(bits == off && bits > 0, "returns bits written");

        size_t r = 0;
        CHECK(::ue5::read_bits(buf, sizeof(buf), r, 1) == 0, "bHasRepLayout == 0");
        CHECK(::ue5::read_bits(buf, sizeof(buf), r, 1) == 1, "bIsActor == 1 (the PC)");
        uint64_t cb = ::ue5::read_sip(buf, sizeof(buf), r);
        CHECK(r + cb == off, "content block size covers the rest");
        uint32_t fn = ::ue5::read_serialize_int(buf, sizeof(buf), r, 1025);
        CHECK(fn == kPcFieldIdx_ClientHitConfirmationsReliable, "field == 171");
        uint64_t params = ::ue5::read_sip(buf, sizeof(buf), r);
        CHECK(r + params == off, "param size covers the rest");

        CHECK(::ue5::read_bits(buf, sizeof(buf), r, 1) == 1, "Hits send bit");
        CHECK(::ue5::read_bits(buf, sizeof(buf), r, 16) == 1, "HitConfirmations count (uint16) == 1");
        CHECK(::ue5::read_bits(buf, sizeof(buf), r, 16) == blob.size(), "SerializedData count (uint16) == 77");
        bool same = true;
        for (uint8_t byte : blob)
            same &= ::ue5::read_bits(buf, sizeof(buf), r, 8) == byte;
        CHECK(same, "SerializedData bytes echoed");
        CHECK(r == off, "no trailing bits");
    }

    std::printf("\n[3] unreliable variant uses field 170\n");
    {
        uint8_t buf[512] = {};
        size_t off = 0;
        write_client_hit_confirmations(buf, sizeof(buf), off, {blob}, /*reliable=*/false);
        size_t r = 2;
        (void)::ue5::read_sip(buf, sizeof(buf), r);
        CHECK(::ue5::read_serialize_int(buf, sizeof(buf), r, 1025) ==
              kPcFieldIdx_ClientHitConfirmations, "field == 170");
    }

    std::printf("\n=== %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
