// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  nav/town_geometry.cpp (see town_geometry.h)
// ============================================================================
#include "nav/town_geometry.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace aoc::worldd::nav {

namespace {

constexpr double kPi = 3.14159265358979323846;

/// The blocks of one layout: a mesh index per block that has at least one walkable quad.
std::vector<uint32_t> add_layout_meshes(Geometry* g, const world::PlatformLayout& layout, TownGeometryStats* stats) {
    std::vector<uint32_t> out;
    const world::PlatformGrid& grid = layout.grid;
    const uint32_t nx = grid.nx, ny = grid.ny;
    const bool flip = (grid.step_x < 0) != (grid.step_y < 0);   // keeps the triangles facing up
    for (uint32_t by = 0; by + 1 < ny; by += kTownBlockQuads) {
        for (uint32_t bx = 0; bx + 1 < nx; bx += kTownBlockQuads) {
            const uint32_t qx = std::min(kTownBlockQuads, nx - 1 - bx), qy = std::min(kTownBlockQuads, ny - 1 - by);
            GeomMesh m;
            std::vector<int64_t> remap(static_cast<size_t>(qx + 1) * (qy + 1), -1);
            const auto vertex = [&](uint32_t ix, uint32_t iy) -> uint32_t {
                int64_t& slot = remap[static_cast<size_t>(iy - by) * (qx + 1) + (ix - bx)];
                if (slot < 0) {
                    slot = static_cast<int64_t>(m.v.size() / 3);
                    m.v.push_back(static_cast<float>(grid.x0 + ix * grid.step_x));
                    m.v.push_back(static_cast<float>(grid.y0 + iy * grid.step_y));
                    m.v.push_back(grid.z[static_cast<size_t>(iy) * nx + ix]);
                }
                return static_cast<uint32_t>(slot);
            };
            for (uint32_t iy = by; iy < by + qy; ++iy) {
                for (uint32_t ix = bx; ix < bx + qx; ++ix) {
                    const size_t i00 = static_cast<size_t>(iy) * nx + ix, i10 = i00 + 1, i01 = i00 + nx, i11 = i01 + 1;
                    if (grid.hole[i00] || grid.hole[i10] || grid.hole[i01] || grid.hole[i11]) {
                        ++stats->holes_skipped;
                        continue;
                    }
                    const uint32_t a = vertex(ix, iy), b = vertex(ix + 1, iy), c = vertex(ix + 1, iy + 1), d = vertex(ix, iy + 1);
                    // (0,0)-(1,1) is the diagonal: (a, b, c) and (a, c, d), counter-clockwise seen from above
                    const uint32_t tris[6] = {a, b, c, a, c, d};
                    for (int k = 0; k < 6; k += 3) {
                        m.i.push_back(tris[k]);
                        m.i.push_back(flip ? tris[k + 2] : tris[k + 1]);
                        m.i.push_back(flip ? tris[k + 1] : tris[k + 2]);
                    }
                }
            }
            if (m.i.empty()) continue;
            m.name = layout.name + "/block " + std::to_string(bx / kTownBlockQuads) + "," + std::to_string(by / kTownBlockQuads);
            stats->triangles += m.i.size() / 3;
            out.push_back(static_cast<uint32_t>(g->meshes.size()));
            g->meshes.push_back(std::move(m));
            ++stats->blocks;
        }
    }
    return out;
}

}  // namespace

bool add_town_platforms(Geometry* g, const std::vector<world::PlatformLayout>& layouts, const std::vector<TownFrame>& frames, TownGeometryStats* stats, std::string* err) {
    TownGeometryStats local;
    if (!stats) stats = &local;
    std::map<std::string, std::vector<uint32_t>> blocks;   // by layout: only the layouts a frame asks for are made
    for (const TownFrame& f : frames) {
        const world::PlatformLayout* layout = nullptr;
        for (const auto& l : layouts) {
            if (l.name == f.layout) layout = &l;
        }
        if (!layout) {
            stats->unplaced.push_back(f.layout);
            continue;
        }
        const world::PlatformGrid& grid = layout->grid;
        if (grid.nx < 2 || grid.ny < 2 || grid.z.size() != static_cast<size_t>(grid.nx) * grid.ny || grid.hole.size() != grid.z.size()) {
            if (err) *err = "the platform of " + layout->name + " is empty or inconsistent";
            return false;
        }
        auto it = blocks.find(layout->name);
        if (it == blocks.end()) {
            it = blocks.emplace(layout->name, add_layout_meshes(g, *layout, stats)).first;
            ++stats->layouts;
        }
        const double yaw = f.yaw_degrees * kPi / 180.0;
        const float c = static_cast<float>(std::cos(yaw)), s = static_cast<float>(std::sin(yaw));
        for (const uint32_t mesh : it->second) {
            GeomPlacement p;
            p.mesh = mesh;
            p.r[0] = c; p.r[1] = -s; p.r[2] = 0;
            p.r[3] = s; p.r[4] = c;  p.r[5] = 0;
            p.r[6] = 0; p.r[7] = 0;  p.r[8] = 1;
            p.t[0] = f.x; p.t[1] = f.y; p.t[2] = f.z;
            g->placements.push_back(p);
            ++stats->placements;
        }
        ++stats->nodes;
    }
    return true;
}

uint64_t town_geometry_hash(const Geometry& g, size_t first_mesh, size_t first_placement) {
    uint64_t h = 1469598103934665603ull;
    const auto mix = [&](const void* data, size_t n) {
        const auto* b = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    };
    for (size_t i = first_mesh; i < g.meshes.size(); ++i) {
        mix(g.meshes[i].v.data(), g.meshes[i].v.size() * sizeof(float));
        mix(g.meshes[i].i.data(), g.meshes[i].i.size() * sizeof(uint32_t));
    }
    for (size_t i = first_placement; i < g.placements.size(); ++i) {
        mix(&g.placements[i].mesh, sizeof(uint32_t));
        mix(g.placements[i].r, sizeof(g.placements[i].r));
        mix(g.placements[i].t, sizeof(g.placements[i].t));
    }
    return h;
}

}  // namespace aoc::worldd::nav
