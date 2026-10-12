// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "npc/motion_table.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

namespace aoc::worldd::npc {

namespace {

std::vector<std::string> split_tabs(const std::string& line) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        const size_t tab = line.find('\t', start);
        if (tab == std::string::npos) {
            out.push_back(line.substr(start));
            return out;
        }
        out.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        const size_t at = s.find(sep, start);
        if (at == std::string::npos) {
            out.push_back(s.substr(start));
            return out;
        }
        out.push_back(s.substr(start, at - start));
        start = at + 1;
    }
}

bool number(const std::string& s, double* v) {
    if (s.empty()) return false;
    char* end = nullptr;
    *v = std::strtod(s.c_str(), &end);
    return end && *end == 0 && std::isfinite(*v);
}

}  // namespace
// built with Cyb and C | O | A

bool MotionTable::parse(const std::string& text, std::string& error) {
    std::map<std::string, MotionStats> rows;
    std::istringstream in(text);
    std::string line;
    size_t n = 0;
    bool header = false;
    while (std::getline(in, line)) {
        ++n;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (!header) {
            if (line.rfind("archetype\t", 0) != 0) {
                error = "npc_motion.tsv: line " + std::to_string(n) + " is not the header";
                return false;
            }
            header = true;
            continue;
        }
        const auto f = split_tabs(line);
        MotionStats m;
        if ((f.size() != 13 && f.size() != 17 && f.size() != 21) || f[0].empty() || !number(f[2], &m.default_scale) || !number(f[4], &m.half_height) || !number(f[5], &m.radius) || !number(f[6], &m.max_walk_speed) ||
            !number(f[7], &m.max_run_speed) || !number(f[8], &m.max_acceleration) || !number(f[9], &m.rotation_rate_yaw) || !number(f[11], &m.fly_speed)) {
            error = "npc_motion.tsv: line " + std::to_string(n) + " is not an archetype, its Blueprint's facts and its numbers";
            return false;
        }
        m.parents = f[1];
        m.move_mode = f[10];
        m.tags = f[12];
        if (f.size() >= 17) {
            m.has_visuals_columns = true;
            m.mesh = f[13];
            m.anim = f[14];
            m.placeholder = f[15] == "1";
            m.hidden = f[16] == "1";
        }
        if (f.size() == 21) {
            m.has_combat_columns = true;
            const auto semi = split(f[17], ';');
            if (semi.size() != 3) {
                error = "npc_motion.tsv: line " + std::to_string(n) + " has a stat list column that is not three guids";
                return false;
            }
            m.base_list = std::strtoll(semi[0].c_str(), nullptr, 10);
            m.list1 = std::strtoll(semi[1].c_str(), nullptr, 10);
            m.list2 = std::strtoll(semi[2].c_str(), nullptr, 10);
            const auto dist = split(f[18], ',');
            if (dist.size() != 8) {
                error = "npc_motion.tsv: line " + std::to_string(n) + " has a stat distribution that is not eight weights";
                return false;
            }
            for (int i = 0; i < 8; ++i) m.dist[i] = static_cast<int>(std::strtol(dist[static_cast<size_t>(i)].c_str(), nullptr, 10));
            if (!f[19].empty()) {
                for (const std::string& pair : split(f[19], ',')) {
                    const size_t colon = pair.find(':');
                    if (colon == std::string::npos) {
                        error = "npc_motion.tsv: line " + std::to_string(n) + " has an elite rank that is not level:rank";
                        return false;
                    }
                    m.elite.emplace_back(static_cast<int>(std::strtol(pair.c_str(), nullptr, 10)), static_cast<int>(std::strtol(pair.c_str() + colon + 1, nullptr, 10)));
                }
            }
            if (!f[20].empty()) {
                for (const std::string& g : split(f[20], ',')) m.rewards.push_back(std::strtoll(g.c_str(), nullptr, 10));
            }
        }
        if (!f[3].empty()) {
            size_t start = 0;
            for (;;) {
                const size_t comma = f[3].find(',', start);
                const std::string one = f[3].substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                char* end = nullptr;
                const long long v = std::strtoll(one.c_str(), &end, 10);
                if (one.empty() || !end || *end != 0) {
                    error = "npc_motion.tsv: line " + std::to_string(n) + " has a starting alignment that is not a record guid";
                    return false;
                }
                if (v != 0) m.alignments.push_back(static_cast<int64_t>(v));   // an unset slot (the None record) names nothing
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        }
        if (m.default_scale <= 0 || m.default_scale > 50 || m.fly_speed < 0) {
            error = "npc_motion.tsv: line " + std::to_string(n) + " has a number out of range";
            return false;
        }
        if (m.half_height <= 0 || m.half_height > 2000 || m.radius <= 0 || m.max_walk_speed < 0 || m.max_run_speed < 0 || m.max_acceleration <= 0 || m.rotation_rate_yaw <= 0) {
            error = "npc_motion.tsv: line " + std::to_string(n) + " has a number out of range";
            return false;
        }
        rows[f[0]] = m;
    }
    if (!header) {
        error = "npc_motion.tsv is empty";
        return false;
    }
    rows_ = std::move(rows);
    return true;
}

bool MotionTable::load(const std::string& path, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path;
        return false;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return parse(buf.str(), error);
}

std::optional<MotionStats> MotionTable::find(const std::string& archetype_path) const {
    const auto it = rows_.find(archetype_path);
    if (it == rows_.end()) return std::nullopt;
    return it->second;
}

MotionStats MotionTable::get(const std::string& archetype_path) const {
    if (const auto m = find(archetype_path)) return *m;
    return MotionStats{};
}

}  // namespace aoc::worldd::npc
