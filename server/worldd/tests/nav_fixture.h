// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/nav_fixture.h  (plan E1, stage 2)
//
//  A small world to build a navmesh from, in memory, for the nav, movement and session tests: no game files.
//
//    centre (0, 0), radius 20,000 cm, terrain z = 1,000 everywhere but a hill (a cone of 400 cm over 3,000 cm) at (6,000, 0)
//    a wall      x -3,100..-2,900, y -2,000..2,000, z 1,000..1,600          (a path from one side to the other goes round it)
//    a platform  x 8,000..10,000, y -6,000..-4,000, z 1,500..1,540           (a floor 500 cm above the ground: not reachable, its top is a surface)
//    a pen       four walls round x, y in [-9,000, -7,800] x [6,000, 7,200]  (no way in: a point inside is unreachable)
// ============================================================================
#pragma once

#include "nav/area_nav.h"
#include "nav/nav_build.h"

#include <cmath>
#include <memory>

namespace aoc::worldd::navfix {

inline constexpr double kRadius = 20000.0;

inline nav::GeomMesh box_mesh(double hx, double hy, double hz, const char* name) {
    nav::GeomMesh m;
    m.name = name;
    for (int i = 0; i < 8; ++i) {
        m.v.push_back(static_cast<float>((i & 1) ? hx : -hx));
        m.v.push_back(static_cast<float>((i & 2) ? hy : -hy));
        m.v.push_back(static_cast<float>((i & 4) ? hz : -hz));
    }
    auto quad = [&](int a, int b, int c, int d) {
        for (int k : {a, b, c, a, c, d}) m.i.push_back(static_cast<uint32_t>(k));
    };
    quad(0, 2, 3, 1); quad(4, 5, 7, 6); quad(0, 1, 5, 4); quad(2, 6, 7, 3); quad(0, 4, 6, 2); quad(1, 3, 7, 5);   // outward: (b-a)x(c-a) out of the solid
    return m;
}

inline void place_box(nav::Geometry& g, uint32_t mesh, double x0, double x1, double y0, double y1, double z0, double z1) {
    // meshes are unit boxes (half extents 1): the placement scales and moves them
    nav::GeomPlacement p;
    p.mesh = mesh;
    p.r[0] = static_cast<float>((x1 - x0) / 2); p.r[4] = static_cast<float>((y1 - y0) / 2); p.r[8] = static_cast<float>((z1 - z0) / 2);
    p.t[0] = (x0 + x1) / 2; p.t[1] = (y0 + y1) / 2; p.t[2] = (z0 + z1) / 2;
    g.placements.push_back(p);
}

inline nav::Geometry test_geometry() {
    nav::Geometry g;
    g.center_x = 0; g.center_y = 0; g.radius = kRadius; g.margin = 3000;
    // terrain: 4 x 4 grids of 129 x 129 samples at 100 cm (12,800 cm each), from -25,600
    for (int gy = 0; gy < 4; ++gy)
        for (int gx = 0; gx < 4; ++gx) {
            nav::TerrainGrid t;
            t.x0 = -25600.0 + gx * 12800.0; t.y0 = -25600.0 + gy * 12800.0;
            t.step_x = t.step_y = 100.0;
            t.z_base = 1000.0; t.z_scale = 1.0;
            t.nx = t.ny = 129;
            t.raw.resize(129 * 129);
            for (uint32_t iy = 0; iy < 129; ++iy)
                for (uint32_t ix = 0; ix < 129; ++ix) {
                    const double wx = t.x0 + ix * 100.0, wy = t.y0 + iy * 100.0;
                    const double d = std::hypot(wx - 6000.0, wy);
                    const double h = d < 3000.0 ? 400.0 * (1.0 - d / 3000.0) : 0.0;
                    t.raw[iy * 129 + ix] = static_cast<uint16_t>(32768 + std::lround(h));
                }
            g.terrain.push_back(std::move(t));
        }
    g.meshes.push_back(box_mesh(1, 1, 1, "unit_box"));
    place_box(g, 0, -3100, -2900, -2000, 2000, 1000, 1600);                 // the wall
    place_box(g, 0, 8000, 10000, -6000, -4000, 1500, 1540);                 // the platform
    place_box(g, 0, -9000, -7800, 7100, 7200, 1000, 1500);                  // the pen's four walls (north)
    place_box(g, 0, -9000, -7800, 6000, 6100, 1000, 1500);                  // (south)
    place_box(g, 0, -9000, -8900, 6000, 7200, 1000, 1500);                  // (west)
    place_box(g, 0, -7900, -7800, 6000, 7200, 1000, 1500);                  // (east)
    return g;
}
// [Cyb - C | O | A]

inline const nav::Geometry& geometry() {
    static const nav::Geometry g = test_geometry();
    return g;
}

inline nav::NavFile build_file(int threads = 2) {
    nav::BuildOptions opt;
    opt.threads = threads;
    opt.nav_margin_cm = 3000;
    nav::NavFile f;
    std::string err;
    nav::BuildStats st;
    if (!nav::build_nav(geometry(), 7, opt, &f, &st, &err)) { std::fprintf(stderr, "nav fixture: %s\n", err.c_str()); std::abort(); }
    return f;
}

/// One shared navmesh of the test world (a test that changes nothing uses it).
inline nav::AreaNav& shared_nav() {
    static std::unique_ptr<nav::AreaNav> n = [] {
        std::string err;
        auto p = nav::AreaNav::from_file(build_file(), &err);
        if (!p) { std::fprintf(stderr, "nav fixture: %s\n", err.c_str()); std::abort(); }
        return p;
    }();
    return *n;
}

}  // namespace aoc::worldd::navfix
