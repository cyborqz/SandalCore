// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  nav/nav_build.h  (plan E1, stage 2)
//
//  The offline half of the ground: mapc's area geometry (tools/mapc/GeomFile.cs: the landscape's heightfield, the placed collision
//  meshes) into a tiled Recast navmesh (nav/nav_file.h).  Linked by aoc_navbuild (the tool mapc runs) and by the tests, which build
//  small worlds of their own.
//
//  The settings are the game's own where the game says (DefaultEngine.ini, [/Script/NavigationSystem.RecastNavMesh] and the Default
//  supported agent): agent radius 35 and height 144, slope 52 degrees, tiles of 4,000 cm, watershed regions.  The cells (25 x 10 cm) are
//  between the game's three resolutions (40, 20, 10 wide x 20 high; a finer height keeps a floor within about 10 cm of its surface); the climb is 45 cm, UE's MaxStepHeight and the move checker's ledge, where the
//  game's navmesh says 100 (modelled: an NPC that steps up a metre looks wrong, and the player cannot).
// ============================================================================
#pragma once

#include "nav/nav_file.h"

#include <cstdint>
#include <string>
#include <vector>

namespace aoc::worldd::nav {

struct GeomMesh {
    std::vector<float> v;         // xyz
    std::vector<uint32_t> i;      // triangles; (b-a)x(c-a) points out of the solid
    std::string name;
};

struct GeomPlacement {
    uint32_t mesh = 0;
    float r[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};   // row-major 3x3 (the scale inside)
    double t[3] = {0, 0, 0};
};

struct Geometry {
    double center_x = 0, center_y = 0, radius = 0, margin = 0;
    /// G1: the footprint the map was read for (geometry version 2).  Empty: E1's circle (the navmesh covers the tiles within radius + nav_margin_cm of the centre, version 1 file);
    /// else the navmesh covers the tiles that touch a rectangle and is written chunked (nav/nav_file.h version 2).  The tile grid is anchored on the centre either way.
    std::vector<RegionRect> region;
    uint64_t region_hash = 0;
    std::vector<TerrainGrid> terrain;
    std::vector<GeomMesh> meshes;
    std::vector<GeomPlacement> placements;
};

/// Reads geometry file version 1 (E1: a circle) or 2 (G1: with the region's rectangles).
bool parse_geometry(const uint8_t* data, size_t size, Geometry* out, std::string* err);
/// The file layout of tools/mapc/GeomFile.cs (mapc writes it; this writer is for the tests): version 1, or version 2 when the geometry has a region.
std::vector<uint8_t> serialize_geometry(const Geometry& g);
bool read_geometry(const std::string& path, Geometry* out, std::vector<uint8_t>* raw, std::string* err);
// [C-Y-B ~ C-O-A]

struct BuildOptions {
    NavSettings settings;
    double nav_margin_cm = 10000.0;   // the navmesh covers the area's radius plus this
    int threads = 0;                  // 0: the machine's
    bool verbose = false;
};

struct BuildStats {
    int tiles = 0;
    int empty_tiles = 0;
    long long polys = 0;
    long long bytes = 0;
    long long input_triangles = 0;
    int failed_tiles = 0;
    int retried_tiles = 0;      // tiles whose first build had more polygons than a Detour tile may hold and that were rebuilt with a coarser simplification
    int max_tile_polys = 0;
    double seconds = 0;
};

/// Builds the navmesh for the geometry; `geometry_hash` is recorded in the file.  False with `err` when a tile cannot be built.
bool build_nav(const Geometry& g, uint64_t geometry_hash, const BuildOptions& opt, NavFile* out, BuildStats* stats, std::string* err);

}  // namespace aoc::worldd::nav
