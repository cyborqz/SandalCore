// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  nav/town_geometry.h  (the towns' floors in the walking map)
//
//  The five city nodes stand on a platform, not on the baked landscape (world/town_platform.h): a town's floor is 50 m above the baked pit under it.  The walking map is built from the
//  map's own geometry (mapc's area_geom.bin), which has the pit and not the platform, so a person placed in a town had no ground of the town's own to walk on.
//
//  This puts the platforms into the geometry before the navigation mesh is built: for each node the layout's platform is cut into blocks of 63 x 63 quads (a mesh each, so that
//  a tile gathers only the blocks it touches) and each block is placed at the node's record, in the node's frame (world = record + R(yaw) (u, v), z = record z + the platform's z).
//  A quad is walkable when none of its four corners is a hole; UE splits it along its (0,0)-(1,1) diagonal, as the platform's sampler does.  Nothing of this is shipped as data: it is
//  built from town_platforms.bin and content.db's city_node by aoc_navbuild, in the step that builds area_nav.bin.
// ============================================================================
#pragma once

#include "nav/nav_build.h"
#include "world/town_platform.h"

#include <cstdint>
#include <string>
#include <vector>

namespace aoc::worldd::nav {

/// A node whose floor is a platform: the layout's name and the node record's place.
struct TownFrame {
    int64_t     node = 0;
    std::string layout;
    double      x = 0, y = 0, z = 0;
    double      yaw_degrees = 0;
};

struct TownGeometryStats {
    size_t layouts = 0;        // layouts a mesh was made of
    size_t nodes = 0;          // frames placed
    size_t blocks = 0;         // meshes added
    size_t placements = 0;
    size_t triangles = 0;      // the triangles of the meshes (one placement of each)
    size_t holes_skipped = 0;  // quads left out for a corner that is a hole
    std::vector<std::string> unplaced;   // frames whose layout the file does not have
};

/// The quads a block holds on a side.
inline constexpr uint32_t kTownBlockQuads = 63;

/// Appends the platforms of `layouts` to `g` (meshes and placements) at the `frames`.  False with `err` for a grid that is empty or inconsistent.  A frame of a layout the list does not
/// have is left out and named in `stats->unplaced`.
bool add_town_platforms(Geometry* g, const std::vector<world::PlatformLayout>& layouts, const std::vector<TownFrame>& frames, TownGeometryStats* stats, std::string* err);

/// A 64-bit signature of what add_town_platforms added to `g` (the meshes from index `first_mesh` and the placements from `first_placement`): what the navigation file's geometry hash
/// has to cover besides the map's own file.
uint64_t town_geometry_hash(const Geometry& g, size_t first_mesh, size_t first_placement);

}  // namespace aoc::worldd::nav
