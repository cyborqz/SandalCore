// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tools/test_level_visibility.cpp
//
//  ServerUpdateLevelVisibility (C>S, PC fn_idx 92) decode and
//  ClientAckUpdateLevelVisibility (S>C, PC fn_idx 21) encode.
//
//  Fixtures are verbatim C>S packets from logs/run_20260928_040905/
//  capture.jsonl.  The encode test reads the produced content block back
//  with the stock-UE5 RPC layout that all 7 captured S>C ch=3 actor RPCs
//  satisfy: bHasRepLayout, bIsActor, SIP(cb), SerializeInt(fn, 1025),
//  SIP(params), params.
// ============================================================================
#include "protocol/wire/packet_parser.h"
#include "protocol/wire/actor_rpc_parser.h"
#include "protocol/wire/level_visibility.h"
#include <cstdio>
#include <string>
#include <vector>

using namespace aoc::protocol::wire;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

// 04:10:30.544, 130B — PktInfo=0, SULV chSeq=707, IsVisible=1.
static const char* kSulvVisible =
    "96760c500060dfdec2ffffffbf635346f5420f1a60d8cc42c330062e5a184e16"
    "0000c0cb51585bd90b53995d19dbdc8b55999c5cd8d7d59b1c1bd95753d81c5d"
    "99dc8b55999c5cd8d7d59b1c1bd95753d81c5d99dccbd751995b995c185d19d9"
    "d74b0c9211cdd1cd4c0c144e550e55108cd4944c1415518d9154150000000000"
    "8001";

// 04:10:30.523, 131B — PktInfo=1, SULV chSeq=705, IsVisible=0.
static const char* kSulvHidden =
    "96760c500060dfdcc2ffffffbf635346f542cfffd100c166161a863170d1c230"
    "b20000005e8ec2daca5e98caeccad8e65eaccae4e4c2beaedee4d8c8be9ac2e6"
    "e8cae45eaccae4e4c2beaedee4d8c8be9ac2e6e8cae45ebe8ecadccae4c2e8ca"
    "c8be5e869ea8b0b2ac688896948aa486a46cb28caa869a986a68a0a800000000"
    "00000c";

static const char* kPrefix = "/Game/Levels/Verra_World_Master/Verra_World_Master/_Generated_/";

static std::vector<uint8_t> hex_decode(const std::string& s) {
    std::vector<uint8_t> out;
    out.reserve(s.size() / 2);
    auto nyb = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    };
    for (size_t i = 0; i + 1 < s.size(); i += 2)
        out.push_back(static_cast<uint8_t>((nyb(s[i]) << 4) | nyb(s[i + 1])));
    return out;
}
// (C-Y-B : CoA)

// Runs one captured packet through packet → RPC → SULV decode.
static bool decode_sulv(const char* hex, LevelVisibilityRequest& req, uint32_t& fn_idx) {
    auto raw = hex_decode(hex);
    auto p = parse_packet(raw.data(), raw.size(), Direction::ClientToServer);
    if (!p || p->bunches.size() != 1) return false;
    const auto& b = p->bunches[0];
    FActorRpcBunch rpc;
    if (!parse_actor_rpc_bunch(raw.data(), raw.size(), b.data_start_bit,
                               b.bunch_data_bits, rpc, b.channel))
        return false;
    fn_idx = rpc.fn_idx;
    return parse_server_update_level_visibility(
        raw.data(), raw.size(), rpc.param_bit_offset,
        b.data_start_bit + b.bunch_data_bits, req);
}

int main() {
    std::printf("=== Level visibility RPC test ===\n");

    std::printf("\n[1] decode SULV, IsVisible=1\n");
    LevelVisibilityRequest vis;
    {
        uint32_t fn = 0;
        bool ok = decode_sulv(kSulvVisible, vis, fn);
        CHECK(fn == kPcFieldIdx_ServerUpdateLevelVisibility, "fn_idx == 92");
        CHECK(ok, "params decode");
        CHECK(vis.package_name == std::string(kPrefix) + "1HF4G731P8U9TA0RS2QTD5FRU",
              "PackageName matches captured WP cell");
        CHECK(vis.package_number == 0, "PackageName number == 0");
        CHECK(!vis.has_file_name, "FileName same as PackageName");
        CHECK(vis.is_visible, "IsVisible == 1");
        CHECK(vis.transaction_bits.size() == 9, "transaction id tail is 9 bits");
    }

    std::printf("\n[2] decode SULV, IsVisible=0\n");
    {
        LevelVisibilityRequest req;
        uint32_t fn = 0;
        bool ok = decode_sulv(kSulvHidden, req, fn);
        CHECK(ok, "params decode");
        CHECK(req.package_name == std::string(kPrefix) + "COTXYV4DKJERCR6YFUCML54PT",
              "PackageName matches captured WP cell");
        CHECK(!req.is_visible, "IsVisible == 0");
    }

    std::printf("\n[3] encode CALV and read it back\n");
    {
        uint8_t buf[512] = {};
        size_t off = 0;
        size_t bits = write_client_ack_update_level_visibility(buf, sizeof(buf), off, vis, true);
        CHECK(bits == off && bits > 0, "returns bits written");

        size_t r = 0;
        CHECK(::ue5::read_bits(buf, sizeof(buf), r, 1) == 0, "bHasRepLayout == 0");
        CHECK(::ue5::read_bits(buf, sizeof(buf), r, 1) == 1, "bIsActor == 1");
        uint64_t cb = ::ue5::read_sip(buf, sizeof(buf), r);
        size_t cb_start = r;
        CHECK(cb_start + cb == off, "content block size covers the rest");
        uint32_t fn = ::ue5::read_serialize_int(buf, sizeof(buf), r, kPcMaxFieldIdx);
        CHECK(fn == kPcFieldIdx_ClientAckUpdateLevelVisibility, "fn_idx == 21");
        uint64_t params = ::ue5::read_sip(buf, sizeof(buf), r);
        CHECK(r + params == off, "param size covers the rest");

        CHECK(::ue5::read_bits(buf, sizeof(buf), r, 1) == 1, "PackageName send bit");
        CHECK(::ue5::read_bits(buf, sizeof(buf), r, 1) == 0, "FName bHardcoded == 0");
        uint32_t len = static_cast<uint32_t>(::ue5::read_bits(buf, sizeof(buf), r, 32));
        CHECK(len == vis.package_name.size() + 1, "FString length includes NUL");
        std::string name;
        for (uint32_t i = 0; i < len; ++i) {
            char c = static_cast<char>(::ue5::read_bits(buf, sizeof(buf), r, 8));
            if (c) name += c;
        }
        CHECK(name == vis.package_name, "PackageName echoed");
        CHECK(::ue5::read_bits(buf, sizeof(buf), r, 32) == 0, "FName number == 0");
        CHECK(::ue5::read_bits(buf, sizeof(buf), r, 1) == 1, "TransactionId send bit");
        bool txn_echo = true;
        for (bool bit : vis.transaction_bits)
            txn_echo &= (::ue5::read_bits(buf, sizeof(buf), r, 1) != 0) == bit;
        CHECK(txn_echo, "TransactionId bits echoed verbatim");
        CHECK(::ue5::read_bits(buf, sizeof(buf), r, 1) == 1, "bClientAckCanMakeVisible == 1");
        CHECK(r == off, "no trailing bits");
    }

    std::printf("\n=== %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
