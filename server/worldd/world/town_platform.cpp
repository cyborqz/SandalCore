// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "world/town_platform.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>

namespace aoc::worldd::world {

namespace {

constexpr char kMagic[8] = {'A', 'O', 'C', 'T', 'P', 'F', '0', '1'};
constexpr uint64_t kMaxLatticeVertices = 16ull * 1024 * 1024;   // 64 MB of heights: far above the game's 1,009 x 1,009

/// Little-endian reads with bounds (the host is little-endian: Windows x64).
struct Reader {
    const uint8_t* p;
    size_t         n;
    size_t         pos = 0;
    bool           ok = true;

    bool need(size_t k) {
        if (!ok || pos > n || n - pos < k) ok = false;
        return ok;
    }
    template <class T>
    T get() {
        T v{};
        if (!need(sizeof(T))) return v;
        std::memcpy(&v, p + pos, sizeof(T));
        pos += sizeof(T);
        return v;
    }
    std::string str(size_t k) {
        if (!need(k)) return {};
        std::string s(reinterpret_cast<const char*>(p + pos), k);
        pos += k;
        return s;
    }
    size_t skip(size_t k) {
        if (!need(k)) return 0;
        const size_t at = pos;
        pos += k;
        return at;
    }
};

struct Comp {
    double   x0 = 0, y0 = 0, sx = 0, sy = 0, zb = 0, zs = 0;
    uint32_t nx = 0, ny = 0, flags = 0;
    size_t   raw = 0, bits = 0;   // offsets in the file
};

}  // namespace

// ---- the grid ---------------------------------------------------------------------------------------------------------------------------------

void PlatformGrid::finish() {
    reach = 0;
    for (uint32_t iy = 0; iy < ny; ++iy) {
        const double v = y0 + iy * step_y;
        for (uint32_t ix = 0; ix < nx; ++ix) {
            if (hole[static_cast<size_t>(iy) * nx + ix]) continue;
            const double u = x0 + ix * step_x;
            reach = std::max(reach, std::hypot(u, v));
        }
    }
}

size_t PlatformGrid::holes() const { return static_cast<size_t>(std::count(hole.begin(), hole.end(), static_cast<uint8_t>(1))); }

bool PlatformGrid::height(double u, double v, double* out) const {
    if (nx < 2 || ny < 2 || z.size() != static_cast<size_t>(nx) * ny || hole.size() != z.size()) return false;
    const double fx = (u - x0) / step_x, fy = (v - y0) / step_y;
    if (!(fx >= 0 && fy >= 0 && fx <= nx - 1 && fy <= ny - 1)) return false;   // (also false for NaN)
    const uint32_t ix = std::min<uint32_t>(static_cast<uint32_t>(fx), nx - 2);
    const uint32_t iy = std::min<uint32_t>(static_cast<uint32_t>(fy), ny - 2);
    const size_t i00 = static_cast<size_t>(iy) * nx + ix;
    if (hole[i00] || hole[i00 + 1] || hole[i00 + nx] || hole[i00 + nx + 1]) return false;
    const double tx = fx - ix, ty = fy - iy;
    const double z00 = z[i00], z10 = z[i00 + 1], z01 = z[i00 + nx], z11 = z[i00 + nx + 1];
    *out = tx >= ty ? z00 + (z10 - z00) * tx + (z11 - z10) * ty : z00 + (z11 - z01) * tx + (z01 - z00) * ty;
    return true;
}
// [CoA + C:Y:B]

// ---- the file ---------------------------------------------------------------------------------------------------------------------------------

bool parse_town_platforms(const uint8_t* data, size_t size, std::vector<PlatformLayout>* out, std::string* err) {
    const auto fail = [&](const std::string& m) {
        if (err) *err = m;
        return false;
    };
    if (!data || size < 16 || std::memcmp(data, kMagic, 8) != 0) return fail("not a town platforms file (the magic is not AOCTPF01)");
    Reader r{data, size};
    r.pos = 8;
    const uint32_t version = r.get<uint32_t>(), layouts = r.get<uint32_t>();
    if (version != 1) return fail("the town platforms file is version " + std::to_string(version) + ", this worldd reads 1");
    if (layouts == 0 || layouts > 64) return fail("the town platforms file holds " + std::to_string(layouts) + " layouts");
    std::vector<PlatformLayout> result;
    for (uint32_t li = 0; li < layouts; ++li) {
        PlatformLayout layout;
        layout.name = r.str(r.get<uint16_t>());
        layout.package = r.str(r.get<uint16_t>());
        const uint32_t count = r.get<uint32_t>();
        if (!r.ok) return fail("the town platforms file is cut short in layout " + std::to_string(li));
        if (count == 0 || count > 65536) return fail("layout " + layout.name + " holds " + std::to_string(count) + " components");
        std::vector<Comp> comps;
        comps.reserve(count);
        for (uint32_t ci = 0; ci < count; ++ci) {
            Comp c;
            c.x0 = r.get<double>();
            c.y0 = r.get<double>();
            c.sx = r.get<double>();
            c.sy = r.get<double>();
            c.zb = r.get<double>();
            c.zs = r.get<double>();
            c.nx = r.get<uint32_t>();
            c.ny = r.get<uint32_t>();
            c.flags = r.get<uint32_t>();
            if (!r.ok) return fail("the town platforms file is cut short in " + layout.name + " component " + std::to_string(ci));
            const bool finite = std::isfinite(c.x0) && std::isfinite(c.y0) && std::isfinite(c.sx) && std::isfinite(c.sy) && std::isfinite(c.zb) && std::isfinite(c.zs);
            if (!finite || c.sx <= 0 || c.sy <= 0 || c.nx < 2 || c.ny < 2 || c.nx > 4096 || c.ny > 4096) return fail(layout.name + " component " + std::to_string(ci) + " has a bad grid");
            const size_t verts = static_cast<size_t>(c.nx) * c.ny;
            c.raw = r.skip(verts * 2);
            if (c.flags & 1u) c.bits = r.skip((verts + 7) / 8);
            if (!r.ok) return fail("the town platforms file is cut short in " + layout.name + " component " + std::to_string(ci));
            comps.push_back(c);
        }
        // one lattice: the same step everywhere, every component on a multiple of it from the lowest corner (neighbours share a border row of vertices)
        const double sx = comps[0].sx, sy = comps[0].sy;
        double ox = std::numeric_limits<double>::infinity(), oy = ox;
        for (const Comp& c : comps) {
            if (std::fabs(c.sx - sx) > 1e-9 || std::fabs(c.sy - sy) > 1e-9) return fail(layout.name + " has components with different steps");
            ox = std::min(ox, c.x0);
            oy = std::min(oy, c.y0);
        }
        uint64_t width = 0, height = 0;
        std::vector<std::pair<uint32_t, uint32_t>> at(comps.size());
        for (size_t i = 0; i < comps.size(); ++i) {
            const Comp& c = comps[i];
            const double fx = (c.x0 - ox) / sx, fy = (c.y0 - oy) / sy;
            if (!(fx >= 0 && fx < 1.0e6 && fy >= 0 && fy < 1.0e6) || std::fabs(fx - std::round(fx)) * sx > 0.01 || std::fabs(fy - std::round(fy)) * sy > 0.01) return fail(layout.name + " has a component off the lattice");
            at[i] = {static_cast<uint32_t>(std::llround(fx)), static_cast<uint32_t>(std::llround(fy))};
            width = std::max<uint64_t>(width, static_cast<uint64_t>(at[i].first) + c.nx);
            height = std::max<uint64_t>(height, static_cast<uint64_t>(at[i].second) + c.ny);
        }
        if (width * height > kMaxLatticeVertices) return fail(layout.name + " is " + std::to_string(width) + " x " + std::to_string(height) + " vertices: too big");
        PlatformGrid& g = layout.grid;
        g.step_x = sx;
        g.step_y = sy;
        g.x0 = ox;
        g.y0 = oy;
        g.nx = static_cast<uint32_t>(width);
        g.ny = static_cast<uint32_t>(height);
        g.z.assign(static_cast<size_t>(width * height), 0.0f);
        g.hole.assign(static_cast<size_t>(width * height), 2);   // 2: no component covers it yet
        for (size_t i = 0; i < comps.size(); ++i) {
            const Comp& c = comps[i];
            for (uint32_t vy = 0; vy < c.ny; ++vy) {
                for (uint32_t vx = 0; vx < c.nx; ++vx) {
                    const size_t src = static_cast<size_t>(vy) * c.nx + vx;
                    const size_t dst = static_cast<size_t>(at[i].second + vy) * g.nx + at[i].first + vx;
                    uint16_t raw;
                    std::memcpy(&raw, data + c.raw + src * 2, 2);
                    g.z[dst] = static_cast<float>(c.zb + (static_cast<int>(raw) - 32768) * c.zs);
                    const bool is_hole = (c.flags & 1u) && ((data[c.bits + (src >> 3)] >> (src & 7)) & 1u);
                    // a vertex two components share is a hole when either says so (fewer false floors)
                    g.hole[dst] = (g.hole[dst] == 1 || is_hole) ? 1 : 0;
                }
            }
        }
        for (uint8_t& h : g.hole) h = h == 0 ? 0 : 1;   // (2, uncovered, is a hole)
        g.finish();
        result.push_back(std::move(layout));
    }
    if (r.pos != size) return fail("the town platforms file has " + std::to_string(size - r.pos) + " bytes after its last layout");
    *out = std::move(result);
    return true;
}
// [C-O-A ~ Cyb]

bool read_town_platforms(const std::string& path, std::vector<PlatformLayout>* out, std::string* err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (err) *err = "cannot open " + path;
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!parse_town_platforms(bytes.data(), bytes.size(), out, err)) {
        if (err) *err = path + ": " + *err;
        return false;
    }
    return true;
}

// ---- the platforms at their nodes --------------------------------------------------------------------------------------------------------------

void TownPlatforms::add_layout(const std::string& name, PlatformGrid grid) { layouts_[name] = std::move(grid); }

bool TownPlatforms::load(const std::string& path, std::string* err) {
    std::vector<PlatformLayout> layouts;
    if (!read_town_platforms(path, &layouts, err)) return false;
    for (PlatformLayout& l : layouts) add_layout(l.name, std::move(l.grid));
    return true;
}

bool TownPlatforms::place(int64_t node, const std::string& layout, const Frame& frame) {
    const auto it = layouts_.find(layout);
    if (it == layouts_.end() || !std::isfinite(frame.x) || !std::isfinite(frame.y) || !std::isfinite(frame.z) || !std::isfinite(frame.yaw_degrees)) return false;
    Placed p;
    p.node = node;
    p.layout = layout;
    p.frame = frame;
    const double yaw = frame.yaw_degrees * 3.14159265358979323846 / 180.0;
    p.cos_yaw = std::cos(yaw);
    p.sin_yaw = std::sin(yaw);
    p.reach = it->second.reach;
    for (Placed& q : placed_) {
        if (q.node == node) {
            q = p;
            return true;
        }
    }
    placed_.push_back(p);
    return true;
}

std::optional<TownPlatforms::Hit> TownPlatforms::floor_at(double x, double y) const {
    if (!std::isfinite(x) || !std::isfinite(y)) return std::nullopt;
    std::optional<Hit> best;
    double best_d2 = std::numeric_limits<double>::infinity();
    for (const Placed& p : placed_) {
        const double dx = x - p.frame.x, dy = y - p.frame.y;
        const double d2 = dx * dx + dy * dy;
        if (d2 > (p.reach + 200.0) * (p.reach + 200.0) || d2 >= best_d2) continue;
        // the node's frame: R(-yaw) (point - record)
        const double u = dx * p.cos_yaw + dy * p.sin_yaw;
        const double v = -dx * p.sin_yaw + dy * p.cos_yaw;
        double h = 0;
        if (!layouts_.at(p.layout).height(u, v, &h)) continue;
        best_d2 = d2;
        best = Hit{p.node, p.frame.z + h, u, v};
    }
    return best;
}

const PlatformGrid* TownPlatforms::layout(const std::string& name) const {
    const auto it = layouts_.find(name);
    return it == layouts_.end() ? nullptr : &it->second;
}

bool TownPlatforms::has_node(int64_t node) const { return frame_of(node) != nullptr; }

const TownPlatforms::Frame* TownPlatforms::frame_of(int64_t node) const {
    for (const Placed& p : placed_) {
        if (p.node == node) return &p.frame;
    }
    return nullptr;
}

std::string TownPlatforms::layout_of(int64_t node) const {
    for (const Placed& p : placed_) {
        if (p.node == node) return p.layout;
    }
    return {};
}

}  // namespace aoc::worldd::world
