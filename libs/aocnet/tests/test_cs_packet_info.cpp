// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tools/test_cs_packet_info.cpp
//
//  C>S PacketInfo layout regression test.  When the client sends several
//  packets in one frame, all but the first carry bHasPacketInfoPayload=0.
//  In that case the client writes NOTHING else for PacketInfo (stock UE5
//  ReadPacketInfo early-return) — there is no hasServerFrameTime bit.
//  Reading one anyway shifts every bunch header by 1 bit, which turned
//  reliable ch=3 RPCs into garbage channels (513, 769, ... 16257) in the
//  2026-09-28 04:09 run.
//
//  Fixtures are verbatim C>S packets from logs/run_20260928_040905/
//  capture.jsonl.  Expected values were derived independently with a
//  Python brute-force header decode (contiguous ch=3 chSeq, BunchDataBits
//  == remaining bits minus the AoC sentinel bit).
// ============================================================================
#include "protocol/wire/packet_parser.h"
#include <cstdio>
#include <string>
#include <vector>

using namespace aoc::protocol::wire;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

// 04:10:30.544, 130B — PktInfo=0, single reliable ch=3 bunch, chSeq=707, 834 bits.
static const char* kPktInfo0Single =
    "96760c500060dfdec2ffffffbf635346f5420f1a60d8cc42c330062e5a184e16"
    "0000c0cb51585bd90b53995d19dbdc8b55999c5cd8d7d59b1c1bd95753d81c5d"
    "99dc8b55999c5cd8d7d59b1c1bd95753d81c5d99dccbd751995b995c185d19d9"
    "d74b0c9211cdd1cd4c0c144e550e55108cd4944c1415518d9154150000000000"
    "8001";

// 04:10:27.263, 209B — PktInfo=0, multi-bunch; first is partial ch=3 chSeq=703.
static const char* kPktInfo0Multi =
    "96760c50005cdfdac2ffffff9f635346f5420f1af05766262351995b995c185d"
    "19d9d7cb0d9552d6d413cdd2954dd68c524e11124d8e9051cc134d160c000000"
    "000080c902000078390a6b2b7b612ab32b639b7bb12a93930bfbba7a936323fb"
    "6a0a9ba32b937bb12a93930bfbba7a936323fb6a0a9ba32b937bf93a2a732b93"
    "0ba32b23fb7a89590a9269923aaa8a8189b1117a82a251aa31caa20982529a02"
    "00000000004003009b990390e40a0018201ef82b50c5a24000000080c0dd9360"
    "000000b08268912000000030039467a018";

// 04:12:00.542, 29B — PktInfo=1, control-channel Close bunch (client quit).
static const char* kPktInfo1Close =
    "96760c50004c1432cfffffffbf635346f5424f80140480b0ff05008001";

// run_20260928_054900 05:50:24.431, 209B — PktInfo=0.  Final fragment of a
// split ch=3 RPC (chSeq 266, carries a ChName like every reliable bunch),
// then a 28-bit ch=3 bunch chSeq 267.
static const char* kContinuationThenBunch =
    "96760c500460df25ddffffff3f8b3212ddc2341a502166262351995b995c185d"
    "19d9d7cb0d9552d6d413cdd2954dd68c524e11124d8e9051cc134d160c000000"
    "000080c902000078390a6b2b7b612ab32b639b7bb12a93930bfbba7a936323fb"
    "6a0a9ba32b937bb12a93930bfbba7a936323fb6a0a9ba32b937bf93a2a732b93"
    "0ba32b23fb7a89590a9269923aaa8a8189b1117a82a251aa31caa20982529a02"
    "000000000040032c94990390e40a0018201ef82b50c5a24000000080c0dd9360"
    "000000b08268912000000030039467a018";

// run_20260928_054900 05:51:23.312, 309B — PktInfo=1.  Two SULV RPCs
// (ch=3 chSeq 312, 313; 834 bits each) then an unreliable ch=19 bunch.
static const char* kTwoRpcsThenMove =
    "96760c50040009ace5ffffff3f8b3212ddc2f40cd0003865161a863170d1c270"
    "b20000005e8ec2daca5e98caeccad8e65eaccae4e4c2beaedee4d8c8be9ac2e6"
    "e8cae45eaccae4e4c2beaedee4d8c8be9ac2e6e8cae45ebe8ecadccae4c2e8ca"
    "c8be5e868eaa88a8668a90aaac66689ea2609a6a6698966ea89a6aa600000000"
    "0000d0003965161a863170d1c230b20000005e8ec2daca5e98caeccad8e65eac"
    "cae4e4c2beaedee4d8c8be9ac2e6e8cae45eaccae4e4c2beaedee4d8c8be9ac2"
    "e6e8cae45ebe8ecadccae4c2e8cac8be5e84846aaca666b08e98a0ae84828caa"
    "646a9e86707260a890b2000000000000c00405c27303965b8395060648544252"
    "088b42c30600803be258b5694db9b3778401a7e8a3ab026a05874a482a4a45d9"
    "b2000070f11aab76ef2977f68e30e0147d7455500c";
// [C O A + Cyb]

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

// run_20260928_061401 06:16:22.282, 65B — PktInfo=1.  A 31-bit reliable ch=19
// bunch (BunchDataBits < 32 -> 14-bit length field), then ServerMoveNoBase
// (pawn field 44) unreliable on ch=19, 246 bits.
static const char* kSmallReliableThenMove =
    "96760c5008980a44c7ffffff3f4ec871c350d7f1d1042166fe00549155303081"
    "3d3059802553e09d40e9769950105c89ecaacde8cd9da02c0cc8b9fe60150e00"
    "06";

// Bunches as the live server sees them: header offset computed the way
// GameServer::handle_game_data does, then wire::walk_bunches.  `left` is
// the number of bits after the last bunch (1 = just the AoC sentinel).
static std::vector<ParsedBunch> live_walk(const char* hex, std::vector<uint8_t>& raw,
                                          size_t* left = nullptr) {
    raw = hex_decode(hex);
    const size_t end = ::ue5::strip_termination(raw.data(), raw.size());
    size_t off = OUTER_HDR_BITS;
    uint32_t packed = static_cast<uint32_t>(::ue5::read_bits(raw.data(), raw.size(), off, 32));
    off += 32 * ((packed & 0xF) + 1) + CUSTOM_FIELD_BITS;
    read_packet_info(raw.data(), raw.size(), off, Direction::ClientToServer);
    std::vector<ParsedBunch> seen;
    size_t last = off;
    walk_bunches(raw.data(), raw.size(), off, end, [&](const ParsedBunch& b) {
        seen.push_back(b);
        last = b.data_start_bit + b.bunch_data_bits;
    });
    if (left) *left = end - last;
    return seen;
}

int main() {
    std::printf("=== C>S PacketInfo layout test ===\n");

    std::printf("\n[1] PktInfo=0, single bunch\n");
    {
        auto raw = hex_decode(kPktInfo0Single);
        auto p = parse_packet(raw.data(), raw.size(), Direction::ClientToServer);
        CHECK(p.has_value(), "packet parses");
        if (p) {
            CHECK(!p->has_pkt_info, "has_pkt_info == 0");
            CHECK(p->bunches.size() == 1, "exactly one bunch");
            if (!p->bunches.empty()) {
                const auto& b = p->bunches[0];
                CHECK(!b.is_control, "not a control bunch");
                CHECK(b.is_reliable, "reliable");
                CHECK(b.channel == 3, "channel == 3 (PlayerController)");
                CHECK(b.ch_sequence == 707, "chSequence == 707");
                CHECK(b.bunch_data_bits == 834, "BunchDataBits == 834");
            }
        }
    }

    std::printf("\n[2] PktInfo=0, multi-bunch\n");
    {
        auto raw = hex_decode(kPktInfo0Multi);
        auto p = parse_packet(raw.data(), raw.size(), Direction::ClientToServer);
        CHECK(p.has_value(), "packet parses");
        if (p) {
            CHECK(!p->has_pkt_info, "has_pkt_info == 0");
            CHECK(p->bunches.size() >= 2, "more than one bunch");
            if (!p->bunches.empty()) {
                const auto& b = p->bunches[0];
                CHECK(b.channel == 3, "first bunch channel == 3");
                CHECK(b.ch_sequence == 703, "first bunch chSequence == 703");
                CHECK(b.is_partial, "first bunch is partial");
            }
        }
    }

    std::printf("\n[3] PktInfo=1, control Close (regression guard)\n");
    {
        auto raw = hex_decode(kPktInfo1Close);
        auto p = parse_packet(raw.data(), raw.size(), Direction::ClientToServer);
        CHECK(p.has_value(), "packet parses");
        if (p) {
            CHECK(p->has_pkt_info, "has_pkt_info == 1");
            CHECK(p->bunches.size() == 1, "exactly one bunch");
            if (!p->bunches.empty()) {
                const auto& b = p->bunches[0];
                CHECK(b.is_control && b.is_close, "control Close bunch");
                CHECK(b.channel == 0, "channel == 0");
            }
        }
    }

    std::printf("\n[4] partial continuation keeps its ChName (walk_bunches)\n");
    {
        std::vector<uint8_t> raw;
        auto seen = live_walk(kContinuationThenBunch, raw);
        CHECK(seen.size() >= 2, "at least two bunches");
        if (seen.size() >= 2) {
            CHECK(seen[0].channel == 3 && seen[0].ch_sequence == 266, "bunch 0: ch=3 chSeq=266");
            CHECK(seen[0].is_partial && !seen[0].partial_initial && seen[0].partial_final,
                  "bunch 0: final partial fragment");
            CHECK(seen[0].bunch_data_bits == 1124, "bunch 0: BunchDataBits == 1124");
            CHECK(seen[1].channel == 3 && seen[1].ch_sequence == 267, "bunch 1: ch=3 chSeq=267");
            CHECK(seen[1].bunch_data_bits == 28, "bunch 1: BunchDataBits == 28");
        }
    }

    std::printf("\n[5] bunches after an RPC are still visited (walk_bunches)\n");
    {
        std::vector<uint8_t> raw;
        auto seen = live_walk(kTwoRpcsThenMove, raw);
        CHECK(seen.size() == 3, "three bunches");
        if (seen.size() == 3) {
            CHECK(seen[0].ch_sequence == 312 && seen[0].bunch_data_bits == 834, "bunch 0: SULV chSeq=312");
            CHECK(seen[1].ch_sequence == 313 && seen[1].bunch_data_bits == 834, "bunch 1: SULV chSeq=313");
            CHECK(seen[2].channel == 19 && !seen[2].is_reliable, "bunch 2: unreliable ch=19");
        }
    }

    std::printf("\n[6] zero-bit Close bunch is a valid bunch (walk_bunches)\n");
    {
        std::vector<uint8_t> raw;
        auto seen = live_walk(kPktInfo1Close, raw);
        CHECK(seen.size() == 1, "one bunch");
        if (!seen.empty())
            CHECK(seen[0].is_close && seen[0].bunch_data_bits == 0, "Close with 0 data bits");
    }

    // MaxPacket is 1028 bytes: BunchDataBits = SerializeInt(v, 8224), so a
    // bunch under 32 bits carries a 14th length bit.  Reading 13 bits put
    // the next header one bit early (ch=3 read as 6, ch=19 as 38).
    std::printf("\n[7] 28-bit RPC then ServerSetSpectatorLocation (14-bit length)\n");
    {
        std::vector<uint8_t> raw;
        size_t left = 0;
        auto seen = live_walk(kContinuationThenBunch, raw, &left);
        CHECK(seen.size() == 3, "three bunches");
        if (seen.size() == 3) {
            CHECK(seen[1].bunch_data_bits == 28, "bunch 1: 28-bit zero-param RPC");
            CHECK(seen[2].channel == 3 && !seen[2].is_reliable, "bunch 2: unreliable ch=3");
            CHECK(seen[2].bunch_data_bits == 241, "bunch 2: BunchDataBits == 241");
        }
        CHECK(left == 1, "walk ends at the sentinel bit");
    }

    std::printf("\n[8] 31-bit reliable ch=19 then ServerMoveNoBase\n");
    {
        std::vector<uint8_t> raw;
        size_t left = 0;
        auto seen = live_walk(kSmallReliableThenMove, raw, &left);
        CHECK(seen.size() == 2, "two bunches");
        if (seen.size() == 2) {
            CHECK(seen[0].channel == 19 && seen[0].is_reliable && seen[0].bunch_data_bits == 31,
                  "bunch 0: reliable ch=19, 31 bits");
            CHECK(seen[1].channel == 19 && !seen[1].is_reliable && seen[1].bunch_data_bits == 246,
                  "bunch 1: unreliable ch=19, 246 bits");
        }
        CHECK(left == 1, "walk ends at the sentinel bit");
    }
// -- Cyb x C.O.A --

    std::printf("\n[9] write_bunch_data_bits matches the reader\n");
    {
        for (uint32_t v : {0u, 28u, 31u, 32u, 241u, 8000u}) {
            uint8_t buf[8] = {};
            size_t off = 0;
            write_bunch_data_bits(buf, sizeof(buf), off, v);
            size_t r = 0;
            const uint32_t back = ::ue5::read_serialize_int(buf, sizeof(buf), r, MAX_PKT_BITS);
            char msg[96];
            std::snprintf(msg, sizeof(msg), "BunchDataBits=%u: %zu bits, reads back", v, off);
            CHECK(back == v && r == off && off == (v < 32 ? 14u : 13u), msg);
        }
    }

    std::printf("\n=== %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
