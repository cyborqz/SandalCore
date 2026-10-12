// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  native_bunch_dump — run the native bootstrap offline and print every bunch
//
//  Drives WorldBootstrapEmitter (the same plan aoc_server --native runs) with
//  a recording IGameServerHost and prints each bunch it would have sent as a
//  JSON line in replay_bunches' format, so tools/decode_actor_open.py can
//  check our spawns against the client's read path without launching the
//  game.  Spliced plan rows print the captured packet's bunches.
//
//  Usage (run it in a scratch directory: the emitters read and write
//  probe_*.txt files in the working directory):
//    native_bunch_dump <replay_data.bin> [--name cyb] > native.jsonl
// ============================================================================
#ifdef _WIN32
#  include <winsock2.h>
#else
#  include <netinet/in.h>
#endif

#include "net/native_connect_sequencer.h"
#include "net/world_bootstrap_emitter.h"
#include "protocol/net_guid_allocator.h"
#include "protocol/wire/packet_parser.h"
#include "protocol/wire/ue5_primitives.h"
#include "replay_file.h"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

using aoc::protocol::wire::ParsedBunch;

std::string payload_hex(const uint8_t* raw, size_t raw_len, size_t start, size_t bits) {
    std::vector<uint8_t> out((bits + 7) / 8, 0);
    size_t r = start, w = 0;
    for (size_t i = 0; i < bits; ++i)
        ::ue5::write_bits(out.data(), out.size(), w,
                          static_cast<uint32_t>(::ue5::read_bits(raw, raw_len, r, 1)), 1);
    static const char* digits = "0123456789abcdef";
    std::string hex;
    for (uint8_t b : out) { hex += digits[b >> 4]; hex += digits[b & 15]; }
    return hex;
}

void print_bunch(uint32_t seq, const char* source, const uint8_t* raw, size_t raw_len,
                 const ParsedBunch& b) {
    std::printf("{\"pkt\":%u,\"src\":\"%s\",\"ch\":%u,\"ctrl\":%d,\"open\":%d,\"close\":%d,"
                "\"rel\":%d,\"chseq\":%u,\"exports\":%d,\"mustmap\":%d,\"partial\":%d,"
                "\"pinit\":%d,\"pfinal\":%d,\"bits\":%u,\"hex\":\"%s\"}\n",
                seq, source, b.channel, b.is_control, b.is_open, b.is_close, b.is_reliable,
                b.ch_sequence, b.has_package_map_exports, b.has_must_map_guids, b.is_partial,
                b.partial_initial, b.partial_final, b.bunch_data_bits,
                payload_hex(raw, raw_len, b.data_start_bit, b.bunch_data_bits).c_str());
}
// CoA.C | Y | B

class RecordingHost : public aoc::net::IGameServerHost {
public:
    RecordingHost(std::vector<aoc::tools::ReplayPacket> replay, std::string name)
        : replay_(std::move(replay)), name_(std::move(name)) {}

    int send_to_client(const uint8_t*, size_t n, const sockaddr_in&) override { return static_cast<int>(n); }
    const std::string& custom_name() const override { return name_; }
    bool send_keepalive_for(const std::string&, const sockaddr_in&) override { return true; }
    bool has_client_finished_map_load() const override { return true; }

    bool send_bunch_packet(const std::string&, const sockaddr_in&, const uint8_t* data,
                           size_t bits) override {
        const uint32_t seq = seq_++;
        const size_t len = (bits + 7) / 8;
        size_t walked = aoc::protocol::wire::walk_bunches(data, len, 0, bits,
            [&](const ParsedBunch& b) { print_bunch(seq, "native", data, len, b); });
        if (walked == 0)
            std::fprintf(stderr, "native send #%u: %zu bits, no bunch header parsed\n", seq, bits);
        return true;
    }

    bool send_ch0_reliable_payload(const std::string&, const sockaddr_in&, const uint8_t*,
                                   size_t bytes) override {
        std::fprintf(stderr, "ch0 reliable payload: %zu bytes (not dumped)\n", bytes);
        ++seq_;
        return true;
    }

    aoc::protocol::PlayerNetGuidBlock allocate_player_block(const std::string& key) override {
        auto existing = allocator_.get_block(key);
        return existing.is_valid() ? existing : allocator_.allocate_player_block(key);
    }

    std::string current_character_customization_json() const override { return {}; }
    std::string current_character_name() const override { return name_; }

    bool emit_client_initialize_character(const std::string&, const sockaddr_in&) override {
        std::fprintf(stderr, "ClientInitializeCharacter requested (host-built; not dumped)\n");
        return true;
    }

    bool send_captured_packet(const std::string&, const sockaddr_in&, uint32_t idx) override {
        if (idx >= replay_.size()) return false;
        const auto& p = replay_[idx];
        auto pp = aoc::protocol::wire::parse_packet(p.raw.data(), p.raw.size(),
                                                    aoc::protocol::wire::Direction::ServerToClient);
        const uint32_t seq = seq_++;
        if (pp)
            for (const auto& b : pp->bunches)
                print_bunch(seq, "splice", p.raw.data(), p.raw.size(), b);
        return true;
    }

    uint32_t loaded_replay_packet_count() const override {
        return static_cast<uint32_t>(replay_.size());
    }

    const std::vector<uint8_t>* captured_packet_raw(uint32_t idx) const override {
        return idx < replay_.size() ? &replay_[idx].raw : nullptr;
    }

    bool send_client_restart_native(const std::string&, const sockaddr_in&, uint64_t, uint32_t,
                                    uint32_t) override {
        return false;
    }

private:
    std::vector<aoc::tools::ReplayPacket> replay_;
    std::string name_;
    aoc::protocol::NetGuidAllocator allocator_;
    uint32_t seq_ = 0;
};

} // namespace
// C | O | A x Cyb

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: native_bunch_dump <replay_data.bin> [--name cyb]\n");
        return 2;
    }
    std::string name = "cyb";
    for (int i = 2; i + 1 < argc; i += 2)
        if (std::string(argv[i]) == "--name") name = argv[i + 1];

    std::vector<aoc::tools::ReplayPacket> replay;
    if (!aoc::tools::load_replay(argv[1], replay)) {
        std::fprintf(stderr, "cannot load %s\n", argv[1]);
        return 1;
    }
    // stdout carries the JSON lines; the emitters' logs go to stderr.
    spdlog::set_default_logger(spdlog::stderr_color_mt("native_bunch_dump"));
    spdlog::set_level(spdlog::level::warn);

    RecordingHost host(std::move(replay), name);
    sockaddr_in addr{};
    aoc::net::WorldBootstrapEmitter em(host, "127.0.0.1:57429");
    em.emit_all(addr, aoc::net::kDefaultBootstrapPlan);
    return 0;
}
