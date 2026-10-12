// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  libs/aocnet/tests/test_sc_packet_info.cpp
//
//  S>C PacketInfo: when bHasPacketInfoPayload is 0 nothing else follows —
//  no bHasServerFrameTime bit, no FrameTimeByte (stock UE5 ReadPacketInfo
//  early-return, the same rule as C>S).  Checked against the original
//  server's packets in fixtures/replay_data.bin: packets without PacketInfo
//  make up the bootstrap stream right after the PC ActorOpen.
// ============================================================================
#include "protocol/wire/packet_parser.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace aoc::protocol::wire;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

// Raw S>C packet payloads from replay_data.bin ('RPLY' v1).
static std::vector<std::vector<uint8_t>> load_packets(const std::string& path) {
    std::vector<std::vector<uint8_t>> out;
    std::ifstream f(path, std::ios::binary);
    if (!f) return out;
    uint32_t magic = 0, version = 0, count = 0;
    f.read(reinterpret_cast<char*>(&magic), 4);
    f.read(reinterpret_cast<char*>(&version), 4);
    f.read(reinterpret_cast<char*>(&count), 4);
    if (magic != 0x52504C59 || version != 1) return out;
    f.ignore(22);
    for (uint32_t i = 0; i < count && f; ++i) {
        uint32_t ts = 0;
        uint16_t raw_size = 0;
        f.read(reinterpret_cast<char*>(&ts), 4);
        f.read(reinterpret_cast<char*>(&raw_size), 2);
        f.ignore(2 + 2 + 2 + 2 + 1 + 1 + 1 + 2 + 1);   // seq, ack, start, bits, info, srv, frame, jitter, hist
        std::vector<uint8_t> raw(raw_size);
        f.read(reinterpret_cast<char*>(raw.data()), raw_size);
        if (f) out.push_back(std::move(raw));
    }
    return out;
}
// ~ C Y B :: CoA ~

int main() {
    const auto packets = load_packets(std::string(AOC_REPO_ROOT) + "/fixtures/replay_data.bin");
    CHECK(packets.size() == 29010, "replay_data.bin holds 29010 packets");
    if (packets.size() != 29010) {
        std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
        return 1;
    }

    std::printf("pkt#23 (no PacketInfo, no frame time)\n");
    {
        auto pp = parse_packet(packets[23].data(), packets[23].size(), Direction::ServerToClient);
        CHECK(pp && !pp->has_pkt_info, "parses; bHasPacketInfoPayload = 0");
        CHECK(pp && !pp->bunches.empty() && pp->bunches[0].channel == 3 &&
              pp->bunches[0].is_reliable && pp->bunches[0].ch_sequence == 958,
              "first bunch continues the PC channel: ch=3 reliable chSeq=958");
    }

    std::printf("pkt#24 (no PacketInfo, frame-time bit would have been 1)\n");
    {
        auto pp = parse_packet(packets[24].data(), packets[24].size(), Direction::ServerToClient);
        CHECK(pp && !pp->has_pkt_info && !pp->has_srv_frame,
              "parses; no PacketInfo and so no frame time");
        CHECK(pp && !pp->bunches.empty() && pp->bunches[0].channel == 4 &&
              pp->bunches[0].is_open && pp->bunches[0].ch_sequence == 954 &&
              pp->bunches[0].partial_initial,
              "first bunch opens ch=4: reliable, chSeq=954, partial initial");
    }

    std::printf("whole capture\n");
    {
        size_t with_bunches = 0, clean = 0;
        for (const auto& raw : packets) {
            auto pp = parse_packet(raw.data(), raw.size(), Direction::ServerToClient);
            if (!pp || pp->bunches.empty()) continue;
            ++with_bunches;
            const auto& last = pp->bunches.back();
            // Clean: the last bunch ends at the stream end or on AoC's sentinel bit.
            if (last.data_start_bit + last.bunch_data_bits + 1 >= pp->inner_end_bit) ++clean;
        }
        std::printf("  %zu of %zu packets with bunches walk to the end\n", clean, with_bunches);
        CHECK(with_bunches >= 28000, "at least 28000 packets carry bunches");
        CHECK(clean * 1000 >= with_bunches * 999, "at least 99.9% walk cleanly to the end");
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
