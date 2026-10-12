// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/nav_chunk_fixture.h  (G1 stage 1)
//
//  A small CHUNKED world, built in memory (no game files), for the ground's window: 8 x 8 chunks of 8 x 8 tiles.  The tiles are 1,000 cm (the real ones are 4,000: the code only
//  knows "tile size" and "8 tiles to a chunk", so the same logic runs on a world 16 times smaller in area), the grid is anchored where E1's is (centre (0, 0), radius 20,000
//  and a nav margin of 3,000 give the corner at -23,000), the footprint is the rectangle [-31,000, 33,000)^2.
//
//    chunk (j, k) covers x, y in [-23,000 + 8,000 j, -15,000 + 8,000 j);  chunk (2, 2) holds the centre; borders at x = -23,000, -15,000, -7,000, 1,000, 9,000, ...
//    terrain z = 1,000 everywhere (a flat plain: a tile is one polygon, so the navmesh is cheap to build and a path across a border is a straight line)
//    a wall      x -3,100..-2,900, y -2,000..2,000, z 1,000..1,600           inside chunk (2, 2)
//    a platform  x -1,000..3,000, y 5,000..7,000, z 1,500..1,540              a floor 500 cm above the ground that lies across the border x = 1,000 (chunks (2, 3) and (3, 3))
//    a long wall x 1,000..1,200, y -6,000..-1,000                             standing ON the border x = 1,000 from y -6,000 to -1,000: a path across the border must go round it
// ============================================================================
#pragma once

#include "nav/area_nav.h"
#include "nav/nav_build.h"
#include "nav_fixture.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace aoc::worldd::navfix {

inline constexpr double kChunkTile = 1000.0;
inline constexpr double kChunkOrigin = -23000.0;            // the grid's corner (x and y)
inline constexpr double kChunkSpan = 8.0 * kChunkTile;      // one chunk, 8,000 cm
inline double chunk_x0(int j) { return kChunkOrigin + kChunkSpan * j; }   // west edge of chunk column j

inline nav::Geometry chunk_geometry(bool region) {
    nav::Geometry g;
    g.center_x = 0; g.center_y = 0; g.radius = 20000; g.margin = 3000;
    // terrain: 6 x 6 grids of 129 x 129 samples at 100 cm (12,800 cm each), from -38,400
    for (int gy = 0; gy < 6; ++gy)
        for (int gx = 0; gx < 6; ++gx) {
            nav::TerrainGrid t;
            t.x0 = -38400.0 + gx * 12800.0; t.y0 = -38400.0 + gy * 12800.0;
            t.step_x = t.step_y = 100.0;
            t.z_base = 1000.0; t.z_scale = 1.0;
            t.nx = t.ny = 129;
            t.raw.assign(129 * 129, 32768);
            g.terrain.push_back(std::move(t));
        }
    g.meshes.push_back(box_mesh(1, 1, 1, "unit_box"));
    place_box(g, 0, -3100, -2900, -2000, 2000, 1000, 1600);
    place_box(g, 0, -1000, 3000, 5000, 7000, 1500, 1540);
    place_box(g, 0, 1000, 1200, -6000, -1000, 1000, 1600);
    if (region) {
        g.region.push_back({-31000, -31000, 33000, 33000});
        g.region_hash = 0x1234;
    }
    return g;
}
// Cyb with C O A

inline nav::BuildOptions chunk_options(int threads = 4) {
    nav::BuildOptions opt;
    opt.threads = threads;
    opt.nav_margin_cm = 3000;
    opt.settings.tile_size = static_cast<float>(kChunkTile);
    opt.settings.cell_size = 25.f;
    return opt;
}

inline nav::NavFile build_chunk_file(bool region, int threads = 4, int max_tiles = 4096) {
    nav::BuildOptions opt = chunk_options(threads);
    opt.settings.max_tiles = max_tiles;
    nav::NavFile f;
    std::string err;
    nav::BuildStats st;
    if (!nav::build_nav(chunk_geometry(region), 9, opt, &f, &st, &err)) { std::fprintf(stderr, "chunk fixture: %s\n", err.c_str()); std::abort(); }
    return f;
}

/// The chunked fixture world written to a file (the window reads files); the path is per process.
inline std::string write_chunk_file(const std::string& tag, int max_tiles = 4096) {
    const std::string path = "nav_chunk_fixture_" + tag + ".bin";
    std::string err;
    if (!nav::write_nav_file(build_chunk_file(true, 4, max_tiles), path, &err)) { std::fprintf(stderr, "chunk fixture: %s\n", err.c_str()); std::abort(); }
    return path;
}

}  // namespace aoc::worldd::navfix
