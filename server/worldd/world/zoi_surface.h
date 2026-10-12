// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/zoi_surface.h  (Nodes 1, stage 1)
//
//  Which zone of influence (ZOI) owns a point of the world: a 2D lookup over the SURFACE table tools/capture/gen_zoi_surface.py writes
//  (world/zoi_surface_table.h) from the ZOI chunk grid of the game's export, for the whole world.  The owner of a point is the same for every
//  height from the first metres above the sea to 1,638 m (a ZOI is a prism), and the 3D grid has no chunk above z 163,840, so the lookup takes no
//  height.  Only the ZOI guid is here; what a ZOI is (its node, its biome) is world/city_nodes.h's, from content.db.
//
//  G1 stage 1 (merged) has a Riverlands-only region table from the same grid, spawns/region_data.h (what spawning uses); the two sit side by side and
//  test_worldd_city_nodes checks they agree cell for cell.  A later step unifies them: this one is the whole-world side.
// ============================================================================
#pragma once

#include <cstdint>
#include <vector>

namespace aoc::worldd::world {

/// The ZOI guid at (x, y) in cm; 0 where the grid has no chunk or the cell has no ZOI.  Pure and thread-safe.
uint64_t zoi_at(double x_cm, double y_cm);

/// The guids of every ZOI that owns a cell of the surface (ascending, 0 first: no ZOI).
std::vector<uint64_t> zoi_surface_palette();

/// How many chunk columns the table has, and how many of them are one owner.
struct ZoiSurfaceInfo {
    int chunks = 0;
    int uniform = 0;
};
ZoiSurfaceInfo zoi_surface_info();

}  // namespace aoc::worldd::world
