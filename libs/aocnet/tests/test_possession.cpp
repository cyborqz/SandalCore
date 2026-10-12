// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  libs/aocnet/tests/test_possession.cpp
//
//  Possession handshake RPCs on the PlayerController channel:
//    S>C  ClientRestart / ClientRetryClientRestart(APawn*)   fields 45 / 46
//    C>S  ServerAcknowledgePossession(APawn*)                field 73
//
//  The encoded content block is read back with the same RPC parser the
//  server uses on client traffic (parse_actor_rpc_bunch, ch=3), so the
//  layout is the one proven on ServerUpdateLevelVisibility.
// ============================================================================
#include "protocol/wire/actor_rpc_parser.h"
#include "protocol/wire/possession.h"
#include <cstdio>

using namespace aoc::protocol::wire;
using aoc::protocol::emit::FIntrepidNetworkGUID;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

static FIntrepidNetworkGUID replay_pawn() {
    // The replayed session's pawn: ObjectId 88, ServerId 60.
    FIntrepidNetworkGUID g;
    g.ObjectId   = 88;
    g.ServerId   = 60;
    g.Randomizer = 0x6ee87ee4;
    return g;
}

static void test_field_indices() {
    std::printf("field indices (client exe FFunctionParams, base 19)\n");
    CHECK(kPcFieldIdx_ClientRestart == 45, "ClientRestart = 45");
    CHECK(kPcFieldIdx_ClientRetryClientRestart == 46, "ClientRetryClientRestart = 46");
    CHECK(kPcFieldIdx_ServerAcknowledgePossession == 73, "ServerAcknowledgePossession = 73");
    CHECK(kPcFieldIdx_ServerCheckClientPossession == 77, "ServerCheckClientPossession = 77");
    CHECK(kPcFieldIdx_ServerCheckClientPossessionReliable == 78,
          "ServerCheckClientPossessionReliable = 78");
}
// -- C.O.A x Cyb --

static void test_retry_client_restart_round_trip() {
    std::printf("ClientRetryClientRestart encode -> parse\n");
    uint8_t buf[64] = {};
    size_t off = 0;
    const FIntrepidNetworkGUID pawn = replay_pawn();
    const size_t bits = write_pc_object_rpc(buf, sizeof(buf), off,
                                             kPcFieldIdx_ClientRetryClientRestart, pawn);

    // params = send(1) + 128-bit GUID = 129 -> SIP 16 bits.
    // field  = SerializeInt(46, 1025) = 10 bits (46 + 1024 >= 1025 ends the loop).
    // block  = 10 + 16 + 129 = 155 -> SIP 16 bits; plus bHasRepLayout, bIsActor.
    CHECK(bits == 2 + 16 + 155, "content block is 173 bits");
    CHECK(off == bits, "write offset advanced by the returned bit count");

    FActorRpcBunch rpc;
    const bool parsed = parse_actor_rpc_bunch(buf, sizeof(buf), 0, bits, rpc, /*ch_idx=*/3);
    CHECK(parsed, "parse_actor_rpc_bunch accepts the block");
    CHECK(rpc.b_is_actor, "bIsActor (the PlayerController itself)");
    CHECK(rpc.fn_idx == 46, "field index reads back as 46");
    CHECK(rpc.cb_payload_bits == 155, "content-block size SIP = 155");

    FIntrepidNetworkGUID got;
    const bool ok = parse_pawn_param(buf, sizeof(buf), rpc.param_bit_offset, bits, got);
    CHECK(ok, "pawn parameter decodes");
    CHECK(got.ObjectId == 88 && got.ServerId == 60 && got.Randomizer == 0x6ee87ee4,
          "pawn GUID round-trips (88 / 60 / 0x6ee87ee4)");
}

static void test_client_restart_field() {
    std::printf("ClientRestart uses field 45\n");
    uint8_t buf[64] = {};
    size_t off = 0;
    const size_t bits = write_pc_object_rpc(buf, sizeof(buf), off,
                                             kPcFieldIdx_ClientRestart, replay_pawn());
    FActorRpcBunch rpc;
    CHECK(parse_actor_rpc_bunch(buf, sizeof(buf), 0, bits, rpc, 3) && rpc.fn_idx == 45,
          "field index reads back as 45");
}

static void test_matches_original_server() {
    std::printf("ClientRestart matches the original server bit for bit\n");
    // replay_data.bin pkt#97, ch=3 reliable chSeq=992: the first content
    // block of the bunch (173 bits), ClientRestart naming the captured pawn.
    static const uint8_t kCaptured[] = {
        0xde, 0x08, 0xb4, 0x30, 0x20, 0x50, 0x94, 0xb9, 0x13, 0x00, 0x00,
        0x00, 0x00, 0x80, 0x07, 0x00, 0x00, 0x80, 0xdc, 0x0f, 0xdd, 0x0d };
    constexpr size_t kCapturedBits = 173;

    FIntrepidNetworkGUID pawn;
    pawn.ObjectId   = 10341538;
    pawn.ServerId   = 60;
    pawn.Randomizer = 0x6ee87ee4;
    uint8_t buf[sizeof(kCaptured)] = {};
    size_t off = 0;
    const size_t bits = write_pc_object_rpc(buf, sizeof(buf), off, kPcFieldIdx_ClientRestart, pawn);
    CHECK(bits == kCapturedBits, "173 bits, like the captured block");
    bool same = bits == kCapturedBits;
    for (size_t i = 0; same && i < kCapturedBits; ++i)
        same = ((buf[i >> 3] >> (i & 7)) & 1) == ((kCaptured[i >> 3] >> (i & 7)) & 1);
    CHECK(same, "every bit equals the captured ClientRestart");
}

static void test_null_pawn_param() {
    std::printf("pawn parameter with send bit 0\n");
    // SIP(1) then a single 0 send bit: the parameter was left at its default (null).
    uint8_t buf[8] = {};
    size_t off = 0;
    ::ue5::write_sip(buf, sizeof(buf), off, 1);
    ::ue5::write_bits(buf, sizeof(buf), off, 0, 1);
    FIntrepidNetworkGUID got;
    got.ObjectId = 123;
    const bool ok = parse_pawn_param(buf, sizeof(buf), 0, off, got);
    CHECK(ok && got.ObjectId == 0 && got.ServerId == 0, "decodes as a null pawn");
}

int main() {
    test_field_indices();
    test_retry_client_restart_round_trip();
    test_client_restart_field();
    test_matches_original_server();
    test_null_pawn_param();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
