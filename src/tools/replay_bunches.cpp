// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  replay_bunches — dump the original server's raw bunches as JSON lines
//
//  Walks each S>C packet of replay_data.bin with libaocnet's walk_bunches
//  and prints one line per raw bunch (not reassembled: the client reads
//  package-map exports per raw bunch, before it merges partial bunches):
//    {"pkt":22,"ms":3968,"ch":3,"ctrl":0,"open":1,"close":0,"rel":1,
//     "chseq":954,"exports":1,"mustmap":0,"partial":1,"pinit":1,"pfinal":0,
//     "bits":3545,"hex":"<payload bits, LSB-first per byte>"}
//  tools/decode_actor_open.py reads this format; the native-bunch harness
//  writes it too.
//
//  Usage:
//    replay_bunches <replay_data.bin | capture.jsonl> [--from N] [--to N] [--channel N]
//  A capture.jsonl (a live run) prints the S>C packets our server sent, or
//  with --client the C>S packets the client sent.
// ============================================================================
#include "protocol/wire/packet_parser.h"
#include "protocol/wire/ue5_primitives.h"
#include "replay_file.h"

#include <cstdio>
#include <string>
#include <vector>

using aoc::protocol::wire::ParsedBunch;

static std::string payload_hex(const uint8_t* raw, size_t raw_len, size_t start, size_t bits) {
    std::vector<uint8_t> out((bits + 7) / 8, 0);
    size_t r = start, w = 0;
    for (size_t i = 0; i < bits; ++i)
        ::ue5::write_bits(out.data(), out.size(), w,
                          static_cast<uint32_t>(::ue5::read_bits(raw, raw_len, r, 1)), 1);
    static const char* digits = "0123456789abcdef";
    std::string hex;
    hex.reserve(out.size() * 2);
    for (uint8_t b : out) { hex += digits[b >> 4]; hex += digits[b & 15]; }
    return hex;
}
// -- C O A x C:Y:B --

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: replay_bunches <replay_data.bin> [--from N] [--to N] [--channel N]\n");
        return 2;
    }
    uint32_t from = 0, to = UINT32_MAX;
    long channel = -1;
    for (int i = 2; i + 1 < argc; ++i) {
        std::string a = argv[i];
        if (a == "--from") from = static_cast<uint32_t>(std::stoul(argv[i + 1]));
        else if (a == "--to") to = static_cast<uint32_t>(std::stoul(argv[i + 1]));
        else if (a == "--channel") channel = std::stol(argv[i + 1]);
    }
    // A capture.jsonl from a live run holds what our server sent (its S>C
    // side) or, with --client, what the client sent.
    bool client_side = false;
    for (int i = 2; i < argc; ++i)
        if (std::string(argv[i]) == "--client") client_side = true;
    const auto direction = client_side ? aoc::protocol::wire::Direction::ClientToServer
                                       : aoc::protocol::wire::Direction::ServerToClient;
    std::vector<aoc::tools::ReplayPacket> packets;
    const bool loaded = aoc::tools::is_capture_path(argv[1])
                            ? aoc::tools::load_capture(argv[1], client_side ? "C>S" : "S>C", packets)
                            : aoc::tools::load_replay(argv[1], packets);
    if (!loaded) {
        std::fprintf(stderr, "cannot load %s\n", argv[1]);
        return 1;
    }
    if (std::string(argc > 2 ? argv[argc - 1] : "") == "--summary") {
        // How many packets walk exactly to the end of their bunch data (the
        // recorded bunch_bits include AoC's trailing sentinel bit), and
        // whether the recorded bunch start agrees with parse_packet's.
        uint32_t with_bunches = 0, clean = 0, shown = 0, start_differs = 0, pp_clean = 0;
        for (const auto& p : packets) {
            if (p.bunch_bits == 0 || p.index < from || p.index > to) continue;
            ++with_bunches;
            const size_t start = p.bunch_start_bit, end = start + p.bunch_bits - 1;
            size_t reached = start, n = 0;
            aoc::protocol::wire::walk_bunches(p.raw.data(), p.raw.size(), start, end,
                [&](const ParsedBunch& b) { reached = b.data_start_bit + b.bunch_data_bits; ++n; });
            auto pp = aoc::protocol::wire::parse_packet(p.raw.data(), p.raw.size(),
                                                        aoc::protocol::wire::Direction::ServerToClient);
            size_t pp_reached = 0, pp_end = 0, pp_start = 0;
            if (pp) {
                pp_start = pp->inner_start_bit;
                pp_end = pp->inner_end_bit;
                pp_reached = pp->bunches.empty() ? pp_start
                           : pp->bunches.back().data_start_bit + pp->bunches.back().bunch_data_bits;
                if (pp_reached + 1 >= pp_end) ++pp_clean;   // sentinel may remain
            }
            if (pp && pp_start != start) ++start_differs;
            if (reached == end) { ++clean; continue; }
            if (shown++ < 25)
                std::printf("pkt %u: recorded start %zu walks %zu bunches, stops %zu bits short; "
                            "parse_packet start %zu, %zu bunches, stops %zu bits short\n",
                            p.index, start, n, end - reached, pp_start,
                            pp ? pp->bunches.size() : 0, pp ? pp_end - pp_reached : 0);
        }
        std::printf("recorded start: %u of %u packets walk cleanly; parse_packet: %u clean; "
                    "start differs on %u\n", clean, with_bunches, pp_clean, start_differs);
        return 0;
    }
    for (const auto& p : packets) {
        if (p.index < from || p.index > to) continue;
        // parse_packet finds the bunches itself: the recorded bunch_start_bit is
        // 1 or 9 bits late on packets without PacketInfo (see packet_info.h).
        auto pp = aoc::protocol::wire::parse_packet(p.raw.data(), p.raw.size(), direction);
        if (!pp) continue;
        for (const ParsedBunch& b : pp->bunches) {
                if (channel >= 0 && b.channel != static_cast<uint32_t>(channel)) continue;
                std::printf("{\"pkt\":%u,\"ms\":%u,\"ch\":%u,\"ctrl\":%d,\"open\":%d,\"close\":%d,"
                            "\"rel\":%d,\"chseq\":%u,\"exports\":%d,\"mustmap\":%d,\"partial\":%d,"
                            "\"pinit\":%d,\"pfinal\":%d,\"bits\":%u,\"hex\":\"%s\"}\n",
                            p.index, p.timestamp_ms, b.channel, b.is_control, b.is_open, b.is_close,
                            b.is_reliable, b.ch_sequence, b.has_package_map_exports,
                            b.has_must_map_guids, b.is_partial, b.partial_initial, b.partial_final,
                            b.bunch_data_bits,
                            payload_hex(p.raw.data(), p.raw.size(), b.data_start_bit,
                                        b.bunch_data_bits).c_str());
        }
    }
    return 0;
}
