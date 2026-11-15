// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  fishing/water_map.cpp (see water_map.h)
// ============================================================================
#include "fishing/water_map.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>

namespace aoc::worldd::fishing {

namespace {

std::vector<std::string_view> split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    size_t at = 0;
    while (true) {
        const size_t next = s.find(sep, at);
        if (next == std::string_view::npos) {
            out.push_back(s.substr(at));
            return out;
        }
        out.push_back(s.substr(at, next - at));
        at = next + 1;
    }
}

bool number(std::string_view s, double& out) {
    if (s.empty()) return false;
    const char* first = s.data();
    const char* last = s.data() + s.size();
    const auto r = std::from_chars(first, last, out);
    return r.ec == std::errc() && r.ptr == last && std::isfinite(out);
}

bool integer(std::string_view s, int64_t& out) {
    if (s.empty()) return false;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

int64_t cell_key(int64_t cx, int64_t cy) { return static_cast<int64_t>((static_cast<uint64_t>(cx) << 32) ^ (static_cast<uint64_t>(cy) & 0xffffffffull)); }

}  // namespace

bool WaterMap::load(const std::string& path, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "cannot open " + path;
        return false;
    }
    std::ostringstream text;
    text << f.rdbuf();
    return parse(text.str(), error);
}

bool WaterMap::parse(const std::string& text, std::string& error) {
    error.clear();
    std::vector<Body> bodies;
    std::map<int64_t, size_t> by_id;   // the file's id -> the position in `bodies`
    size_t outlines = 0, boxes = 0, line_no = 0;
    std::istringstream in(text);
    std::string line;
    const auto bad = [&](const std::string& why) {
        error = "line " + std::to_string(line_no) + ": " + why;
        return false;
    };
    while (std::getline(in, line)) {
        ++line_no;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const std::vector<std::string_view> p = split(line, '\t');
        if (p[0] == "body") {
            if (p.size() != 8) return bad("a body line has 8 fields");
            int64_t id = 0, fishing = 0;
            Body b;
            if (!integer(p[1], id) || !integer(p[4], fishing) || fishing == 0 || !number(p[5], b.x) || !number(p[6], b.y) || !number(p[7], b.z)) return bad("a body's id, fishing data or place is no number");
            if (p[2] != "lake" && p[2] != "river") return bad("a body is a lake or a river");
            if (by_id.count(id)) return bad("body " + std::to_string(id) + " is listed twice");
            b.river = p[2] == "river";
            b.actor = std::string(p[3]);
            b.fishing = fishing;
            by_id[id] = bodies.size();
            bodies.push_back(std::move(b));
        } else if (p[0] == "outline") {
            int64_t id = 0;
            if (p.size() < 5 || !integer(p[1], id) || !by_id.count(id)) return bad("an outline names no body, or has fewer than three points");
            Body& b = bodies[by_id[id]];
            if (!b.outline.empty()) return bad("body " + std::to_string(id) + " has two outlines");
            for (size_t i = 2; i < p.size(); ++i) {
                const size_t comma = p[i].find(',');
                double x = 0, y = 0;
                if (comma == std::string_view::npos || !number(p[i].substr(0, comma), x) || !number(p[i].substr(comma + 1), y)) return bad("an outline point is not x,y");
                b.outline.emplace_back(x, y);
            }
            ++outlines;
        } else if (p[0] == "box") {
            int64_t id = 0;
            if (p.size() != 9 || !integer(p[1], id) || !by_id.count(id)) return bad("a box line has 9 fields and names a body");
            Box x;
            double yaw = 0;
            if (!number(p[2], x.cx) || !number(p[3], x.cy) || !number(p[4], x.cz) || !number(p[5], x.hx) || !number(p[6], x.hy) || !number(p[7], x.hz) || !number(p[8], yaw)) return bad("a box has a field that is no number");
            if (!(x.hx > 0) || !(x.hy > 0)) return bad("a box has no size");
            const double rad = yaw * 3.14159265358979323846 / 180.0;
            x.cos_yaw = std::cos(rad);
            x.sin_yaw = std::sin(rad);
            bodies[by_id[id]].boxes.push_back(x);
            ++boxes;
        } else {
            return bad("an unknown line kind '" + std::string(p[0]) + "'");
        }
    }
    if (bodies.empty()) {
        error = "the file holds no water body";
        return false;
    }
    for (const Body& b : bodies) {
        if (b.outline.empty() && b.boxes.empty()) {   // nothing to answer from: the extractor leaves such a body out, so the file is not its own
            error = "body " + b.actor + " has neither an outline nor a swim volume";
            return false;
        }
    }
    bodies_ = std::move(bodies);
    outlines_ = outlines;
    boxes_ = boxes;
    index();
    return true;
}

void WaterMap::index() {
    grid_.clear();
    for (size_t i = 0; i < bodies_.size(); ++i) {
        Body& b = bodies_[i];
        double lo_x = std::numeric_limits<double>::max(), lo_y = lo_x, hi_x = std::numeric_limits<double>::lowest(), hi_y = hi_x;
        const auto take = [&](double x, double y) {
            lo_x = std::min(lo_x, x);
            hi_x = std::max(hi_x, x);
            lo_y = std::min(lo_y, y);
            hi_y = std::max(hi_y, y);
        };
        if (!b.outline.empty()) {
            for (const auto& [x, y] : b.outline) take(x, y);
        } else {
            for (const Box& x : b.boxes) {
                // the rotated box's corners
                for (const double sx : {-1.0, 1.0}) {
                    for (const double sy : {-1.0, 1.0}) {
                        const double ox = sx * x.hx, oy = sy * x.hy;
                        take(x.cx + ox * x.cos_yaw - oy * x.sin_yaw, x.cy + ox * x.sin_yaw + oy * x.cos_yaw);
                    }
                }
            }
        }
        b.min_x = lo_x;
        b.max_x = hi_x;
        b.min_y = lo_y;
        b.max_y = hi_y;
        const int64_t c0x = static_cast<int64_t>(std::floor(lo_x / kCellCm)), c1x = static_cast<int64_t>(std::floor(hi_x / kCellCm));
        const int64_t c0y = static_cast<int64_t>(std::floor(lo_y / kCellCm)), c1y = static_cast<int64_t>(std::floor(hi_y / kCellCm));
        for (int64_t cx = c0x; cx <= c1x; ++cx) {
            for (int64_t cy = c0y; cy <= c1y; ++cy) grid_[cell_key(cx, cy)].push_back(static_cast<uint32_t>(i));
        }
    }
}

bool WaterMap::inside_outline(const Body& b, double x, double y) {
    bool in = false;
    const size_t n = b.outline.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const auto& [xi, yi] = b.outline[i];
        const auto& [xj, yj] = b.outline[j];
        if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) in = !in;
    }
    return in;
}

bool WaterMap::inside_box(const Box& b, double x, double y) {
    const double dx = x - b.cx, dy = y - b.cy;
    const double u = dx * b.cos_yaw + dy * b.sin_yaw;    // into the box's own axes
    const double v = -dx * b.sin_yaw + dy * b.cos_yaw;
    return std::fabs(u) <= b.hx && std::fabs(v) <= b.hy;
}

std::optional<WaterHit> WaterMap::at(double x, double y) const {
    if (!std::isfinite(x) || !std::isfinite(y)) return std::nullopt;
    const auto cell = grid_.find(cell_key(static_cast<int64_t>(std::floor(x / kCellCm)), static_cast<int64_t>(std::floor(y / kCellCm))));
    if (cell == grid_.end()) return std::nullopt;
    for (const uint32_t i : cell->second) {
        const Body& b = bodies_[i];
        if (x < b.min_x || x > b.max_x || y < b.min_y || y > b.max_y) continue;
        if (!b.outline.empty()) {
            if (!inside_outline(b, x, y)) continue;
            return WaterHit{i, b.fishing, b.z, b.river};
        }
        for (const Box& box : b.boxes) {
            if (inside_box(box, x, y)) return WaterHit{i, b.fishing, box.cz, b.river};
        }
    }
    return std::nullopt;
}

size_t WaterMap::lakes() const {
    return static_cast<size_t>(std::count_if(bodies_.begin(), bodies_.end(), [](const Body& b) { return !b.river; }));
}

size_t WaterMap::rivers() const { return bodies_.size() - lakes(); }

WaterMap::BodyInfo WaterMap::info(size_t body) const {
    BodyInfo out;
    if (body >= bodies_.size()) return out;
    const Body& b = bodies_[body];
    out.actor = b.actor;
    out.fishing = b.fishing;
    out.river = b.river;
    out.x = b.x;
    out.y = b.y;
    out.z = b.z;
    out.outline_points = b.outline.size();
    out.boxes = b.boxes.size();
    return out;
}

std::vector<std::pair<int64_t, size_t>> WaterMap::fishing_data() const {
    std::map<int64_t, size_t> count;
    for (const Body& b : bodies_) ++count[b.fishing];
    return {count.begin(), count.end()};
}

std::optional<std::pair<double, double>> WaterMap::box_centre(size_t body, size_t index) const {
    if (body >= bodies_.size() || index >= bodies_[body].boxes.size()) return std::nullopt;
    const Box& b = bodies_[body].boxes[index];
    return std::make_pair(b.cx, b.cy);
}

std::optional<WaterMap::Near> WaterMap::nearest(double x, double y, double radius_cm) const {
    std::optional<Near> best;
    const auto offer = [&](double px, double py, size_t body) {
        const double d = std::hypot(px - x, py - y);
        if (d <= radius_cm && (!best || d < best->distance)) best = Near{px, py, d, body};
    };
    for (size_t i = 0; i < bodies_.size(); ++i) {
        const Body& b = bodies_[i];
        if (x < b.min_x - radius_cm || x > b.max_x + radius_cm || y < b.min_y - radius_cm || y > b.max_y + radius_cm) continue;
        if (!b.outline.empty()) {
            for (const auto& [px, py] : b.outline) offer(px, py, i);
        } else {
            for (const Box& box : b.boxes) {
                for (const double sx : {-1.0, 0.0, 1.0}) {
                    for (const double sy : {-1.0, 0.0, 1.0}) {
                        const double ox = sx * box.hx, oy = sy * box.hy;
                        offer(box.cx + ox * box.cos_yaw - oy * box.sin_yaw, box.cy + ox * box.sin_yaw + oy * box.cos_yaw, i);
                    }
                }
            }
        }
    }
    return best;
}

}  // namespace aoc::worldd::fishing
