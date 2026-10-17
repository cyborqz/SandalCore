// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/town_platform_fixture.h  (the floor of the five city nodes' bowls)
//
//  A small platform to stand a bowl's floor on without the game's files: (2 half + 1)^2 vertices at 100 cm in the platform's own (u, v), a floor of `base + slope * u` cm, and a ring of
//  holes beyond `hole_r` from the origin (the footprint is a disc, as the real platforms' is a rounded square).  Plus the file of such grids in the exact format
//  tools/mapc/TownFloors.cs writes (components of 16 x 16 vertices that share a border row), so the reader is tested on bytes.
// ============================================================================
#pragma once

#include "world/town_platform.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace aoc::worldd::tpfix {

inline world::PlatformGrid grid(int half = 30, double base = 500.0, double slope = 0.0, double hole_r = 2700.0) {
    world::PlatformGrid g;
    g.step_x = g.step_y = 100.0;
    g.nx = g.ny = static_cast<uint32_t>(2 * half + 1);
    g.x0 = g.y0 = -half * 100.0;
    g.z.assign(static_cast<size_t>(g.nx) * g.ny, 0.0f);
    g.hole.assign(g.z.size(), 0);
    for (uint32_t iy = 0; iy < g.ny; ++iy) {
        for (uint32_t ix = 0; ix < g.nx; ++ix) {
            const double u = g.x0 + ix * g.step_x, v = g.y0 + iy * g.step_y;
            g.z[iy * g.nx + ix] = static_cast<float>(base + slope * u);
            g.hole[iy * g.nx + ix] = std::hypot(u, v) > hole_r ? 1 : 0;
        }
    }
    g.finish();
    return g;
}
// -- CoA & C.Y.B --

/// A platform set with `g` stood at `node` in the frame (x, y, z, yaw).
inline std::unique_ptr<world::TownPlatforms> platforms_at(int64_t node, double x, double y, double z, double yaw, world::PlatformGrid g = grid(), const std::string& layout = "Layout_Test") {
    auto set = std::make_unique<world::TownPlatforms>();
    set->add_layout(layout, std::move(g));
    set->place(node, layout, {x, y, z, yaw});
    return set;
}

namespace detail {
template <class T>
void put(std::vector<uint8_t>& b, T v) {
    uint8_t raw[sizeof(T)];
    std::memcpy(raw, &v, sizeof(T));
    b.insert(b.end(), raw, raw + sizeof(T));
}
inline void put_string(std::vector<uint8_t>& b, const std::string& s) {
    put<uint16_t>(b, static_cast<uint16_t>(s.size()));
    b.insert(b.end(), s.begin(), s.end());
}
}  // namespace detail

/// The bytes of a file holding `layouts`; each lattice is cut into components of `comp` x `comp` vertices (neighbours share a border row), (nx - 1) a multiple of (comp - 1).  Heights are
/// stored as zBase + (raw - 32768) * 0.25, so a grid of multiples of 0.25 cm round-trips exactly.  A component with a hole gets the holes flag and its bits.
inline std::vector<uint8_t> file_of(const std::vector<std::pair<std::string, world::PlatformGrid>>& layouts, uint32_t comp = 16, double z_base = 0.0, bool x_flipped_components = false,
                                   const std::function<bool(uint32_t component, uint32_t vx, uint32_t vy, bool hole)>& tweak = nullptr, const std::set<uint32_t>& skip = {}) {
    std::vector<uint8_t> b;
    const char magic[8] = {'A', 'O', 'C', 'T', 'P', 'F', '0', '1'};
    b.insert(b.end(), magic, magic + 8);
    detail::put<uint32_t>(b, 1);
    detail::put<uint32_t>(b, static_cast<uint32_t>(layouts.size()));
    for (const auto& [name, g] : layouts) {
        detail::put_string(b, name);
        detail::put_string(b, "AOC/Content/ENV/Test/" + name + ".umap");
        const uint32_t step = comp - 1;
        const uint32_t cx = (g.nx - 1) / step, cy = (g.ny - 1) / step;
        uint32_t skipped = 0;
        for (const uint32_t c : skip) skipped += c < cx * cy ? 1 : 0;
        detail::put<uint32_t>(b, cx * cy - skipped);
        for (uint32_t j = 0; j < cy; ++j) {
            for (uint32_t k = 0; k < cx; ++k) {
                if (skip.count(j * cx + k)) continue;   // (a component the file does not have)
                const uint32_t i = x_flipped_components ? cx - 1 - k : k;   // (the order in the file is not the lattice's order)
                detail::put<double>(b, g.x0 + i * step * g.step_x);
                detail::put<double>(b, g.y0 + j * step * g.step_y);
                detail::put<double>(b, g.step_x);
                detail::put<double>(b, g.step_y);
                detail::put<double>(b, z_base);
                detail::put<double>(b, 0.25);
                detail::put<uint32_t>(b, comp);
                detail::put<uint32_t>(b, comp);
                std::vector<uint8_t> bits((static_cast<size_t>(comp) * comp + 7) / 8, 0);
                bool any = false;
                std::vector<uint16_t> raw;
                for (uint32_t vy = 0; vy < comp; ++vy) {
                    for (uint32_t vx = 0; vx < comp; ++vx) {
                        const size_t src = static_cast<size_t>(j * step + vy) * g.nx + i * step + vx;
                        raw.push_back(static_cast<uint16_t>(32768 + std::lround((g.z[src] - z_base) / 0.25)));
                        const bool is_hole = tweak ? tweak(j * cx + k, vx, vy, g.hole[src] != 0) : g.hole[src] != 0;
                        if (is_hole) {
                            const size_t at = static_cast<size_t>(vy) * comp + vx;
                            bits[at >> 3] = static_cast<uint8_t>(bits[at >> 3] | (1u << (at & 7)));
                            any = true;
                        }
                    }
                }
                detail::put<uint32_t>(b, any ? 1u : 0u);
                for (const uint16_t r : raw) detail::put<uint16_t>(b, r);
                if (any) b.insert(b.end(), bits.begin(), bits.end());
            }
        }
    }
    return b;
}

}  // namespace aoc::worldd::tpfix
