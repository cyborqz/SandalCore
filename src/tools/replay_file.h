// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  replay_file.h — loader for replay_data.bin ('RPLY' v1), shared by the
//  offline replay tools.  Layout matches ReplayData in net/game_server.h.
// ============================================================================
#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace aoc { namespace tools {

struct ReplayPacket {
    uint32_t index = 0;
    uint32_t timestamp_ms = 0;
    uint16_t bunch_start_bit = 0;   // first bunch-header bit in raw
    uint16_t bunch_bits = 0;        // bits of bunch data (header + payloads)
    std::vector<uint8_t> raw;
};
// -- CoA x C Y B --

inline bool load_replay(const std::string& path, std::vector<ReplayPacket>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    uint32_t magic = 0, version = 0, count = 0;
    f.read(reinterpret_cast<char*>(&magic), 4);
    f.read(reinterpret_cast<char*>(&version), 4);
    f.read(reinterpret_cast<char*>(&count), 4);
    if (magic != 0x52504C59 || version != 1) return false;   // 'RPLY'
    char header_rest[6 + 6 + 1 + 1 + 2 + 2 + 4];              // custom fields, ids, seqs, reserved
    f.read(header_rest, sizeof(header_rest));
    for (uint32_t i = 0; i < count && f; ++i) {
        ReplayPacket p;
        p.index = i;
        uint16_t raw_size = 0, seq = 0, ack = 0, jitter = 0;
        uint8_t has_info = 0, has_frame = 0, frame = 0, hist = 0;
        f.read(reinterpret_cast<char*>(&p.timestamp_ms), 4);
        f.read(reinterpret_cast<char*>(&raw_size), 2);
        f.read(reinterpret_cast<char*>(&seq), 2);
        f.read(reinterpret_cast<char*>(&ack), 2);
        f.read(reinterpret_cast<char*>(&p.bunch_start_bit), 2);
        f.read(reinterpret_cast<char*>(&p.bunch_bits), 2);
        f.read(reinterpret_cast<char*>(&has_info), 1);
        f.read(reinterpret_cast<char*>(&has_frame), 1);
        f.read(reinterpret_cast<char*>(&frame), 1);
        f.read(reinterpret_cast<char*>(&jitter), 2);
        f.read(reinterpret_cast<char*>(&hist), 1);
        p.raw.resize(raw_size);
        f.read(reinterpret_cast<char*>(p.raw.data()), raw_size);
        if (f) out.push_back(std::move(p));
    }
    return !out.empty();
}

/// Packets sent in one direction ("S>C" or "C>S") from a capture.jsonl the
/// game server's ProxyLogger writes; `index` is the line number.
inline bool load_capture(const std::string& path, const std::string& dir,
                         std::vector<ReplayPacket>& out) {
    std::ifstream f(path);
    if (!f) return false;
    const std::string dir_field = "\"dir\":\"" + dir + "\"";
    std::string line;
    for (uint32_t i = 0; std::getline(f, line); ++i) {
        if (line.find(dir_field) == std::string::npos) continue;
        const auto h = line.find("\"hex\":\"");
        if (h == std::string::npos) continue;
        const auto e = line.find('"', h + 7);
        ReplayPacket p;
        p.index = i;
        for (size_t k = h + 7; k + 1 < e; k += 2)
            p.raw.push_back(static_cast<uint8_t>(std::stoul(line.substr(k, 2), nullptr, 16)));
        out.push_back(std::move(p));
    }
    return !out.empty();
}

inline bool is_capture_path(const std::string& path) {
    return path.size() > 6 && path.substr(path.size() - 6) == ".jsonl";
}

}} // namespace aoc::tools
