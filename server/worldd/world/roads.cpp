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
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>

namespace aoc::worldd::world {

namespace {

bool fits_float(double value) {
    return std::isfinite(value) && std::fabs(value) <= std::numeric_limits<float>::max();
}

bool active_width(double value) {
    return value > 0 && fits_float(value) && static_cast<float>(value) > 0;
}

bool number(const std::string& text, double& value) {
    char* end = nullptr;
    value = std::strtod(text.c_str(), &end);
    return end != text.c_str() && end == text.c_str() + text.size() && std::isfinite(value);
}

bool points(const std::vector<std::string>& fields, size_t first, RoadMap::Polyline& road) {
    for (size_t i = first; i < fields.size(); ++i) {
        const size_t comma = fields[i].find(',');
        double x = 0, y = 0;
        if (comma == std::string::npos || !number(fields[i].substr(0, comma), x) || !number(fields[i].substr(comma + 1), y)) return false;
        road.xy.push_back(x);
        road.xy.push_back(y);
    }
    return true;
}

double dist2_to_segment(double px, double py, double x0, double y0, double x1, double y1) {
    const double dx = x1 - x0, dy = y1 - y0;
    const double len2 = dx * dx + dy * dy;
    const double ax = px - x0, ay = py - y0;
    const double projection = ax * dx + ay * dy;
    if (len2 == 0 || projection <= 0) return ax * ax + ay * ay;
    if (projection >= len2) {
        const double bx = px - x1, by = py - y1;
        return bx * bx + by * by;
    }
    // The perpendicular distance avoids subtracting a rounded projection from a point on a very long segment.
    const double cross = ax * dy - ay * dx;
    return cross * cross / len2;
}

}  // namespace

bool RoadMap::add(Polyline road, int initial_level) {
    if (road.xy.size() < 4 || (road.xy.size() % 2) != 0) return false;
    for (const double v : road.xy) {
        if (!fits_float(v)) return false;
    }
    if (road.by_level) {
        if (initial_level < 0 || initial_level >= kLevels) return false;
        bool any = false;
        for (int l = 0; l < kLevels; ++l) {
            if (road.tiers[l] < -1 || road.tiers[l] > 4) return false;
            if (road.tiers[l] >= 0) {
                if (!active_width(road.halves[l])) return false;
                any = true;
            }
        }
        if (!any) return false;
        road.tier = road.tiers[initial_level];
        road.half_width_cm = road.tier >= 0 ? road.halves[initial_level] : 0.0;
    } else if (road.tier < 0 || road.tier > 4 || !active_width(road.half_width_cm)) {
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
    double widest = static_cast<float>(road.half_width_cm);
    if (road.by_level) {
        for (size_t level = 0; level < kLevels; ++level) {
            if (road.tiers[level] >= 0) widest = std::max(widest, static_cast<double>(static_cast<float>(road.halves[level])));
        }
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
        const double c0x = std::floor(lo_x / kCellCm), c1x = std::floor(hi_x / kCellCm);
        const double c0y = std::floor(lo_y / kCellCm), c1y = std::floor(hi_y / kCellCm);
        const bool in_grid = c0x >= std::numeric_limits<int32_t>::min() && c1x <= std::numeric_limits<int32_t>::max() &&
                             c0y >= std::numeric_limits<int32_t>::min() && c1y <= std::numeric_limits<int32_t>::max();
        bool indexed = false;
        if (in_grid) {
            const int64_t x0 = static_cast<int64_t>(c0x), x1 = static_cast<int64_t>(c1x);
            const int64_t y0 = static_cast<int64_t>(c0y), y1 = static_cast<int64_t>(c1y);
            const int64_t columns = x1 - x0 + 1, rows = y1 - y0 + 1;
            if (columns <= 4096 && rows <= 4096 / columns) {
                for (int64_t cx = x0; cx <= x1; ++cx) {
                    for (int64_t cy = y0; cy <= y1; ++cy) grid_[key(static_cast<int32_t>(cx), static_cast<int32_t>(cy))].push_back(id);
                }
                indexed = true;
            }
        }
        if (!indexed) exceptional_.push_back(id);
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
    if (x < bounds_.min_x || x > bounds_.max_x || y < bounds_.min_y || y > bounds_.max_y) return -1;
    int best = -1;
    const auto check = [&](const std::vector<uint32_t>& ids) {
        for (const uint32_t id : ids) {
            const Seg& s = segs_[id];
            if (s.tier <= best) continue;
            if (dist2_to_segment(x, y, s.x0, s.y0, s.x1, s.y1) <= static_cast<double>(s.half_width) * s.half_width) best = s.tier;
        }
    };
    check(exceptional_);
    const double cx = std::floor(x / kCellCm), cy = std::floor(y / kCellCm);
    if (cx < std::numeric_limits<int32_t>::min() || cx > std::numeric_limits<int32_t>::max() ||
        cy < std::numeric_limits<int32_t>::min() || cy > std::numeric_limits<int32_t>::max()) return best;
    const auto it = grid_.find(key(static_cast<int32_t>(cx), static_cast<int32_t>(cy)));
    if (it != grid_.end()) check(it->second);
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
            const auto zone = std::from_chars(f[1].data(), f[1].data() + f[1].size(), p.zoi);
            if (zone.ec != std::errc{} || zone.ptr != f[1].data() + f[1].size() || p.zoi < 0) {
                error = path + " line " + std::to_string(n) + ": the zone is not a number";
                return false;
            }
            const auto list = [&](const std::string& text, auto&& put) {
                std::stringstream parts(text);
                std::string one;
                int k = 0;
                while (std::getline(parts, one, ',')) {
                    double v = 0;
                    if (!number(one, v) || k >= kLevels || !put(k++, v)) return false;
                }
                return k == kLevels;
            };
            if (!list(f[2], [&](int k, double v) {
                    if (v < -1 || v > 4 || std::trunc(v) != v) return false;
                    p.tiers[static_cast<size_t>(k)] = static_cast<int>(v);
                    return true;
                }) || !list(f[3], [&](int k, double v) { p.halves[static_cast<size_t>(k)] = v; return true; })) {
                error = path + " line " + std::to_string(n) + ": the tiers and half widths are not " + std::to_string(kLevels) + " numbers each";
                return false;
            }
            if (!points(f, 4, p)) {
                error = path + " line " + std::to_string(n) + ": a point is not finite x,y";
                return false;
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
        const auto tier = std::from_chars(f[1].data(), f[1].data() + f[1].size(), p.tier);
        if (tier.ec != std::errc{} || tier.ptr != f[1].data() + f[1].size()) {
            error = path + " line " + std::to_string(n) + ": the tier is not a number";
            return false;
        }
        if (!number(f[2], p.half_width_cm)) {
            error = path + " line " + std::to_string(n) + ": the half width is not a number";
            return false;
        }
        if (!points(f, 3, p)) {
            error = path + " line " + std::to_string(n) + ": a point is not finite x,y";
            return false;
        }
        if (!add(std::move(p))) {
            error = path + " line " + std::to_string(n) + ": not a road (tier 0 to 4, a positive width, two points or more)";
            return false;
        }
    }
    return true;
}

}  // namespace aoc::worldd::world
