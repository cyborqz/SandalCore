// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  replay_rpcs — list the RPCs the original server sent on one actor channel
//
//  Walks every S>C packet of replay_data.bin with the verified bunch walker
//  (libaocnet walk_bunches), reassembles partial bunches per channel, and
//  decodes the content blocks of the chosen channel:
//    bHasRepLayout(1) bIsActor(1) SIP(payload bits) payload
//  Property blocks (bHasRepLayout=1) are skipped by size; RPC blocks start
//  with SerializeInt(field, max) and SIP(param bits).  Open bunches
//  (SerializeNewActor first), bunches with package-map exports and subobject
//  blocks are counted but not decoded.
//
//  Usage:
//    replay_rpcs <replay_data.bin> [--channel 3] [--max-field 1025]
//                [--names file]...
//  Name files are the output of tools/ghidra/ue_net_functions.py
//  ("<index>  <name> ..." lines).
// ============================================================================
#include "protocol/wire/packet_parser.h"
#include "protocol/wire/ue5_primitives.h"
#include "replay_file.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using aoc::protocol::wire::ParsedBunch;
using aoc::tools::ReplayPacket;
using aoc::tools::load_replay;

namespace {
// CoA - C_Y_B

std::map<uint32_t, std::string> load_names(const std::vector<std::string>& files) {
    std::map<uint32_t, std::string> names;
    for (const auto& path : files) {
        std::ifstream f(path);
        std::string line;
        while (std::getline(f, line)) {
            std::istringstream ss(line);
            uint32_t idx = 0;
            std::string name;
            if (ss >> idx >> name) names[idx] = name;
        }
    }
    return names;
}

struct Stats {
    struct Rpc {
        uint32_t count = 0; uint32_t first_pkt = 0; uint32_t first_ms = 0; size_t param_bits = 0;
        std::map<size_t, uint32_t> sizes;   // param bits -> occurrences
    };
    std::map<uint32_t, Rpc> rpcs;
    uint32_t bunches = 0, open_skipped = 0, export_skipped = 0, subobject_stops = 0,
             property_blocks = 0, malformed = 0;
};

// Decodes the content blocks of one complete bunch payload.
void decode_payload(const std::vector<uint8_t>& buf, size_t bits, uint32_t pkt, uint32_t ms,
                    uint32_t max_field, Stats& st) {
    const uint8_t* d = buf.data();
    const size_t n = buf.size();
    size_t off = 0;
    while (off + 2 <= bits) {
        const bool has_rep = ::ue5::read_bits(d, n, off, 1) != 0;
        const bool is_actor = ::ue5::read_bits(d, n, off, 1) != 0;
        if (!is_actor) { ++st.subobject_stops; return; }
        const size_t payload = static_cast<size_t>(::ue5::read_sip(d, n, off));
        const size_t end = off + payload;
        if (payload == 0 || end > bits) { ++st.malformed; return; }
        if (has_rep) {
            ++st.property_blocks;
        } else {
            const uint32_t field = static_cast<uint32_t>(::ue5::read_serialize_int(d, n, off, max_field));
            const size_t param_bits = static_cast<size_t>(::ue5::read_sip(d, n, off));
            auto& r = st.rpcs[field];
            if (r.count++ == 0) { r.first_pkt = pkt; r.first_ms = ms; r.param_bits = param_bits; }
            ++r.sizes[param_bits];
        }
        off = end;
    }
}

void append_bits(std::vector<uint8_t>& dst, size_t& dst_bits,
                 const uint8_t* src, size_t src_len, size_t start, size_t count) {
    dst.resize((dst_bits + count + 7) / 8 + 1, 0);
    size_t r = start;
    for (size_t i = 0; i < count; ++i)
        ::ue5::write_bits(dst.data(), dst.size(), dst_bits,
                          static_cast<uint32_t>(::ue5::read_bits(src, src_len, r, 1)), 1);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: replay_rpcs <replay_data.bin | capture.jsonl> [--channel N] [--max-field N] [--names file]...\n");
        return 2;
    }
    std::string path = argv[1];
    uint32_t channel = 3, max_field = 1025;
    std::vector<std::string> name_files;
    for (int i = 2; i + 1 < argc; i += 2) {
        std::string a = argv[i];
        if (a == "--channel") channel = static_cast<uint32_t>(std::stoul(argv[i + 1]));
        else if (a == "--max-field") max_field = static_cast<uint32_t>(std::stoul(argv[i + 1]));
        else if (a == "--names") name_files.push_back(argv[i + 1]);
    }

    // A capture.jsonl from our packet log holds the client's side: decode its C>S packets.
    const bool client_capture = aoc::tools::is_capture_path(path);
    const auto direction = client_capture ? aoc::protocol::wire::Direction::ClientToServer
                                          : aoc::protocol::wire::Direction::ServerToClient;
    std::vector<ReplayPacket> packets;
    if (client_capture) {
        aoc::tools::load_capture(path, "C>S", packets);
    } else if (!load_replay(path, packets)) {
        std::fprintf(stderr, "cannot load %s\n", path.c_str());
        return 1;
    }
    const auto names = load_names(name_files);
// == C O A | Cyb ==

    Stats st;
    std::vector<uint8_t> partial;
    size_t partial_bits = 0;
    bool partial_active = false, partial_skip = false;

    for (const auto& p : packets) {
        // parse_packet finds the bunches itself: the recorded bunch_start_bit is
        // 1 or 9 bits late on packets without PacketInfo (see packet_info.h).
        auto pp = aoc::protocol::wire::parse_packet(p.raw.data(), p.raw.size(), direction);
        if (!pp) continue;
        for (const ParsedBunch& b : pp->bunches) {
            [&] {
                if (b.is_control || b.channel != channel) return;
                ++st.bunches;
                const bool skip = b.is_open || b.has_package_map_exports;
                if (b.is_partial) {
                    if (b.partial_initial) {
                        partial.clear(); partial_bits = 0; partial_active = true; partial_skip = skip;
                    } else if (!partial_active) {
                        return;  // continuation without its start
                    }
                    partial_skip = partial_skip || skip;
                    append_bits(partial, partial_bits, p.raw.data(), p.raw.size(),
                                b.data_start_bit, b.bunch_data_bits);
                    if (!b.partial_final) return;
                    partial_active = false;
                    if (partial_skip) { b.is_open ? ++st.open_skipped : ++st.export_skipped; return; }
                    decode_payload(partial, partial_bits, p.index, p.timestamp_ms, max_field, st);
                    return;
                }
                if (skip) { b.is_open ? ++st.open_skipped : ++st.export_skipped; return; }
                std::vector<uint8_t> one;
                size_t one_bits = 0;
                append_bits(one, one_bits, p.raw.data(), p.raw.size(), b.data_start_bit, b.bunch_data_bits);
                decode_payload(one, one_bits, p.index, p.timestamp_ms, max_field, st);
            }();
        }
    }

    std::printf("channel %u: %u bunches; %u open and %u export bunches not decoded; "
                "%u property blocks; %u subobject blocks stopped decoding; %u malformed\n",
                channel, st.bunches, st.open_skipped, st.export_skipped, st.property_blocks,
                st.subobject_stops, st.malformed);
    std::printf("%6s %6s %8s %9s %10s  %s\n", "field", "count", "1st pkt", "1st ms", "param bits", "name");
    for (const auto& [field, r] : st.rpcs) {
        auto it = names.find(field);
        std::printf("%6u %6u %8u %9u %10zu  %s\n", field, r.count, r.first_pkt, r.first_ms,
                    r.param_bits, it == names.end() ? "?" : it->second.c_str());
        if (r.sizes.size() > 1) {
            std::printf("%44s", "sizes:");
            int shown = 0;
            for (const auto& [bits, n] : r.sizes) {
                if (shown++ == 12) { std::printf(" ..."); break; }
                std::printf(" %zu(x%u)", bits, n);
            }
            std::printf("  [%zu distinct]\n", r.sizes.size());
        }
    }
    return 0;
}
