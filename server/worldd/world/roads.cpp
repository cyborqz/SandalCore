// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/roads.cpp
//
//  See roads.h.
// ============================================================================
#include "world/roads.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace aoc::worldd::world {

namespace {

double dist2_to_segment(double px, double py, double x0, double y0, double x1, double y1) {
    const double dx = x1 - x0, dy = y1 - y0;
    const double len2 = dx * dx + dy * dy;
    double t = len2 > 0 ? ((px - x0) * dx + (py - y0) * dy) / len2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    const double cx = x0 + t * dx - px, cy = y0 + t * dy - py;
    return cx * cx + cy * cy;
}

}  // namespace

bool RoadMap::add(Polyline road, int initial_level) {
    if (road.xy.size() < 4 || (road.xy.size() % 2) != 0) return false;
    for (const double v : road.xy) {
        if (!std::isfinite(v)) return false;
    }
    if (road.by_level) {
        if (initial_level < 0 || initial_level >= kLevels) return false;
        bool any = false;
        for (int l = 0; l < kLevels; ++l) {
            if (road.tiers[l] < -1 || road.tiers[l] > 4) return false;
            if (road.tiers[l] >= 0) {
                if (!(road.halves[l] > 0) || !std::isfinite(road.halves[l])) return false;
                any = true;
            }
        }
        if (!any) return false;
        road.tier = road.tiers[initial_level];
        road.half_width_cm = road.tier >= 0 ? road.halves[initial_level] : 0.0;
    } else if (road.tier < 0 || road.tier > 4 || !(road.half_width_cm > 0) || !std::isfinite(road.half_width_cm)) {
        return false;
    }
    const size_t at = roads_.size();
    index(road, at);
    level_.push_back(road.by_level ? initial_level : -1);
    if (road.by_level) ++by_level_;
    roads_.push_back(std::move(road));
    return true;
}
// Cyb and C O A

int RoadMap::tier_of_road(size_t i) const {
    if (i >= roads_.size()) return -1;
    return roads_[i].tier;
}

void RoadMap::set_level(size_t road, int level) {
    Polyline& r = roads_[road];
    r.tier = r.tiers[static_cast<size_t>(level)];
    r.half_width_cm = r.tier >= 0 ? r.halves[static_cast<size_t>(level)] : 0.0;
    level_[road] = level;
    for (uint32_t s = first_seg_[road]; s < first_seg_[road] + seg_count_[road]; ++s) {
        segs_[s].tier = static_cast<int8_t>(r.tier);   // (-1: the road gives a mount nothing at this level: tier_at skips it)
        segs_[s].half_width = static_cast<float>(r.half_width_cm);
    }
}

size_t RoadMap::apply_levels(const std::function<int(int64_t zoi)>& level_of_zoi, int default_level) {
    size_t changed = 0;
    for (size_t i = 0; i < roads_.size(); ++i) {
        if (!roads_[i].by_level) continue;
        int level = level_of_zoi ? level_of_zoi(roads_[i].zoi) : -1;
        if (level < 0) level = default_level;
        if (level < 0) level = 0;
        if (level >= kLevels) level = kLevels - 1;
        if (level == level_[i]) continue;
        const int before_tier = roads_[i].tier;
        const double before_half = roads_[i].half_width_cm;
        set_level(i, level);
        if (roads_[i].tier != before_tier || roads_[i].half_width_cm != before_half) ++changed;
    }
    return changed;
}

void RoadMap::index(const Polyline& road, size_t road_index) {
    first_seg_.resize(road_index + 1, 0);
    seg_count_.resize(road_index + 1, 0);
    first_seg_[road_index] = static_cast<uint32_t>(segs_.size());
    // the cells a by-level road occupies are those of its widest level: its width changes with the level, the cells it is found in must not
    double widest = road.half_width_cm;
    if (road.by_level) {
        for (const double h : road.halves) widest = std::max(widest, h);
    }
    for (size_t i = 0; i + 3 < road.xy.size(); i += 2) {
        Seg s;
        s.x0 = static_cast<float>(road.xy[i]);
        s.y0 = static_cast<float>(road.xy[i + 1]);
        s.x1 = static_cast<float>(road.xy[i + 2]);
        s.y1 = static_cast<float>(road.xy[i + 3]);
        s.half_width = static_cast<float>(road.half_width_cm);
        s.tier = static_cast<int8_t>(road.tier);
        const uint32_t id = static_cast<uint32_t>(segs_.size());
        segs_.push_back(s);
        ++seg_count_[road_index];
        ++segments_;
        // every cell the segment's box (grown by the half width) touches
        const double lo_x = std::min(s.x0, s.x1) - widest, hi_x = std::max(s.x0, s.x1) + widest;
        const double lo_y = std::min(s.y0, s.y1) - widest, hi_y = std::max(s.y0, s.y1) + widest;
        const int32_t c0x = static_cast<int32_t>(std::floor(lo_x / kCellCm)), c1x = static_cast<int32_t>(std::floor(hi_x / kCellCm));
        const int32_t c0y = static_cast<int32_t>(std::floor(lo_y / kCellCm)), c1y = static_cast<int32_t>(std::floor(hi_y / kCellCm));
        for (int32_t cx = c0x; cx <= c1x; ++cx) {
            for (int32_t cy = c0y; cy <= c1y; ++cy) grid_[key(cx, cy)].push_back(id);
        }
        if (segments_ == 1) {
            bounds_ = {lo_x, lo_y, hi_x, hi_y};
        } else {
            bounds_.min_x = std::min(bounds_.min_x, lo_x);
            bounds_.min_y = std::min(bounds_.min_y, lo_y);
            bounds_.max_x = std::max(bounds_.max_x, hi_x);
            bounds_.max_y = std::max(bounds_.max_y, hi_y);
        }
    }
}

int RoadMap::tier_at(double x, double y) const {
    if (!std::isfinite(x) || !std::isfinite(y) || segs_.empty()) return -1;
    const auto it = grid_.find(key(static_cast<int32_t>(std::floor(x / kCellCm)), static_cast<int32_t>(std::floor(y / kCellCm))));
    if (it == grid_.end()) return -1;
    int best = -1;
    for (const uint32_t id : it->second) {
        const Seg& s = segs_[id];
        if (s.tier <= best) continue;
        if (dist2_to_segment(x, y, s.x0, s.y0, s.x1, s.y1) <= static_cast<double>(s.half_width) * s.half_width) best = s.tier;
    }
    return best;
}
// [C-O-A x C | Y | B]

bool RoadMap::load(const std::string& path, std::string& error, int initial_level) {
    std::ifstream in(path);
    if (!in) {
        error = "cannot open " + path;
        return false;
    }
    std::string line;
    size_t n = 0;
    while (std::getline(in, line)) {
        ++n;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line[0] == '#') {
            const size_t at = line.find("node level ");
            if (at != std::string::npos && node_level_ < 0) {
                char* end = nullptr;
                const long v = std::strtol(line.c_str() + at + 11, &end, 10);
                if (end != line.c_str() + at + 11 && v >= 0 && v <= 6) node_level_ = static_cast<int>(v);
            }
            continue;
        }
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string cell;
        while (std::getline(ss, cell, '\t')) f.push_back(cell);
        if (f.size() >= 6 && f[0] == "roadn") {
            // roadn <TAB> zoi <TAB> tiers <TAB> half widths <TAB> points ...
            Polyline p;
            p.by_level = true;
            char* end = nullptr;
            p.zoi = static_cast<int64_t>(std::strtoull(f[1].c_str(), &end, 10));
            if (end == f[1].c_str() || *end) {
                error = path + " line " + std::to_string(n) + ": the zone is not a number";
                return false;
            }
            const auto list = [&](const std::string& text, auto&& put) {
                std::stringstream parts(text);
                std::string one;
                int k = 0;
                while (std::getline(parts, one, ',')) {
                    char* e = nullptr;
                    const double v = std::strtod(one.c_str(), &e);
                    if (e == one.c_str() || *e || k >= kLevels) return false;
                    put(k++, v);
                }
                return k == kLevels;
            };
            if (!list(f[2], [&](int k, double v) { p.tiers[static_cast<size_t>(k)] = static_cast<int>(v); }) || !list(f[3], [&](int k, double v) { p.halves[static_cast<size_t>(k)] = v; })) {
                error = path + " line " + std::to_string(n) + ": the tiers and half widths are not " + std::to_string(kLevels) + " numbers each";
                return false;
            }
            for (size_t i = 4; i < f.size(); ++i) {
                const size_t comma = f[i].find(',');
                if (comma == std::string::npos) {
                    error = path + " line " + std::to_string(n) + ": a point is not x,y";
                    return false;
                }
                p.xy.push_back(std::strtod(f[i].substr(0, comma).c_str(), nullptr));
                p.xy.push_back(std::strtod(f[i].substr(comma + 1).c_str(), nullptr));
            }
            if (!add(std::move(p), initial_level)) {
                error = path + " line " + std::to_string(n) + ": not a road (tiers -1 to 4 with a positive width where there is one, two points or more)";
                return false;
            }
            continue;
        }
        if (f.size() < 5 || f[0] != "road") {
            error = path + " line " + std::to_string(n) + " is not a road";
            return false;
        }
        Polyline p;
        char* end = nullptr;
        p.tier = static_cast<int>(std::strtol(f[1].c_str(), &end, 10));
        if (end == f[1].c_str() || *end) {
            error = path + " line " + std::to_string(n) + ": the tier is not a number";
            return false;
        }
        p.half_width_cm = std::strtod(f[2].c_str(), &end);
        if (end == f[2].c_str() || *end) {
            error = path + " line " + std::to_string(n) + ": the half width is not a number";
            return false;
        }
        for (size_t i = 3; i < f.size(); ++i) {
            const size_t comma = f[i].find(',');
            if (comma == std::string::npos) {
                error = path + " line " + std::to_string(n) + ": a point is not x,y";
                return false;
            }
            p.xy.push_back(std::strtod(f[i].substr(0, comma).c_str(), nullptr));
            p.xy.push_back(std::strtod(f[i].substr(comma + 1).c_str(), nullptr));
        }
        if (!add(std::move(p))) {
            error = path + " line " + std::to_string(n) + ": not a road (tier 0 to 4, a positive width, two points or more)";
            return false;
        }
    }
    return true;
}

}  // namespace aoc::worldd::world
