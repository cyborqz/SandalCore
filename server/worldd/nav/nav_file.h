// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  nav/nav_file.h  (plan E1, stage 2; version 2: G1 stage 1)
//
//  The ground as one file, dist/content/area_nav.bin, written by aoc_navbuild (from mapc's area_geom.bin) and read by worldd:
//    * the terrain: the landscape's heightfield as the client's own 16-bit samples, one grid per landscape component;
//    * the navmesh: Detour tiles, built by Recast for a walking pawn (nav_build.h holds the settings).
//
//  Two layouts, told apart by the magic and the version (a file of the other layout is refused by the reader that does not know it):
//
//  VERSION 1, flat (E1: one circle of 2,488 tiles, every tile resident at once; still read and written, the tests' small worlds use it)
//    "AOCNAV01", u32 version (1)
//    f64 centre x, centre y, radius, margin (cm: the area the geometry was cut for)
//    u64 geometry hash (FNV-1a of area_geom.bin, to see which geometry a file came from)
//    f32 cell size, cell height, agent radius, agent height, agent max climb, agent max slope (degrees)
//    f32 tile size (cm), i32 max tiles, i32 max polys per tile, f32 origin x, y, z (the navmesh grid's corner, in nav-local coordinates)
//    u32 terrain grids, u32 tiles
//    grid:  f64 x0 y0 stepX stepY zBase zScale, u32 nx ny, u16 raw[nx*ny]       (world z = zBase + (raw - 32768) * zScale; x fastest)
//    tile:  i32 tx ty, u32 size, size bytes (a Detour tile as dtCreateNavMeshData made it)
//
//  VERSION 2, chunked (G1 stage 1: the whole Riverlands, ~35,000 tiles and 950 MB, which one Detour mesh cannot hold: dtNavMesh::init needs 10 bits of salt, so at most 4,096
//  tiles with 1,024 polygons each).  A chunk is 8 x 8 tiles (32,000 cm); worldd keeps the chunks around its players resident and reads the rest off disk when needed.
//    header (160 bytes, little-endian):
//      0   "AOCNAV02", 8  u32 version (2)
//      12  f64 centre x, centre y, radius, margin      (as in version 1: E1's circle; the grid is anchored on the centre so that E1's tiles are the same tiles)
//      44  u64 geometry hash
//      52  f32 cell size, cell height, agent radius, agent height, agent max climb, agent max slope
//      76  f32 tile size, 80 i32 max tiles, 84 i32 max polys, 88 f32 origin x, y, z
//      100 u32 chunk tiles (8), 104 u64 region hash (FNV-1a of the region file's rectangles), 112 u32 region rectangles
//      116 u32 terrain grids, 120 u32 chunks, 124 u32 tiles in all
//      128 u64 terrain directory offset, 136 u64 chunk directory offset, 144 u64 blobs offset, 152 u64 file size
//    region:    f64 x0 y0 x1 y1 per rectangle (the footprint the tiles were built for; worldd's edge test)
//    terrain directory: per grid  f64 x0 y0 stepX stepY zBase zScale, u32 nx ny, u64 offset of its u16 samples       (64 bytes)
//    terrain samples:   the grids' u16 raw arrays, one after the other (resident in worldd: a height query never waits for the disk)
//    chunk directory:   per chunk i32 cx cy, u32 tiles, u32 size, u64 offset, u64 hash (FNV-1a of the blob)              (32 bytes), sorted by (cy, cx)
//    blobs: per chunk its tiles in (ty, tx) order, each  i32 tx ty, u32 size, size bytes (a Detour tile)
//  A tile (tx, ty) belongs to the chunk (floor(tx / 8), floor(ty / 8)); tile indices are Detour's own (they can be negative: the grid's origin is E1's corner).
//  `mapc --check` compares two version 2 files by the chunks' hashes (aoc_navbuild --compare says which chunk differs).
//
//  Nav-local coordinates (what Detour sees): x = world x - centre x, y = world z (up), z = world y - centre y, in cm.  The origin keeps float precision small near the
//  login spot (the world's x is -777,664 cm; a float is good to a hundredth of a centimetre within 10 m of the centre, to 0.06 cm at 6 km, the Riverlands' farthest
//  corner, finer than Recast's cells of 25 x 10 cm).  The mapping swaps the world's y and z, so triangles are wound the other way round (nav_build.cpp).  Nothing else
//  in worldd sees nav-local coordinates: nav/area_nav.h speaks world cm.
// ============================================================================
#pragma once
// ~ CoA - C:Y:B ~

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace aoc::worldd::nav {

inline constexpr char kNavMagic[8] = {'A', 'O', 'C', 'N', 'A', 'V', '0', '1'};
inline constexpr uint32_t kNavVersion = 1;
inline constexpr char kNavMagic2[8] = {'A', 'O', 'C', 'N', 'A', 'V', '0', '2'};
inline constexpr uint32_t kNavVersion2 = 2;
inline constexpr int32_t kChunkTiles = 8;            // a chunk is 8 x 8 tiles
inline constexpr size_t kNavHeaderSize2 = 160;

/// One landscape component's heights.
struct TerrainGrid {
    double x0 = 0, y0 = 0;          // world cm of sample (0, 0)
    double step_x = 100, step_y = 100;
    double z_base = 0, z_scale = 1;
    uint32_t nx = 0, ny = 0;
    std::vector<uint16_t> raw;

    double x1() const { return x0 + (nx - 1) * step_x; }
    double y1() const { return y0 + (ny - 1) * step_y; }
    double sample(uint32_t ix, uint32_t iy) const { return z_base + (static_cast<int>(raw[iy * nx + ix]) - 32768) * z_scale; }
    /// The height at a world point (the landscape's own triangles: each quad is split along its (0,0)-(1,1) diagonal), false outside the grid.
    bool height(double x, double y, double* z) const;
};

struct NavTileBlob {
    int32_t tx = 0, ty = 0;
    std::vector<uint8_t> data;
};

struct NavSettings {
    float cell_size = 25.f;
    float cell_height = 10.f;
    float agent_radius = 35.f;
    float agent_height = 144.f;
    float agent_max_climb = 45.f;
    float agent_max_slope = 52.f;
    float tile_size = 4000.f;
    int32_t max_tiles = 4096;
    int32_t max_polys = 1024;
    float origin[3] = {0, 0, 0};
};

/// A rectangle [x0, x1) x [y0, y1) of the region's footprint, world cm.
struct RegionRect {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool contains(double x, double y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
};

/// A chunk of 8 x 8 tiles.
struct ChunkKey {
    int32_t x = 0, y = 0;
    bool operator==(const ChunkKey& o) const { return x == o.x && y == o.y; }
    bool operator!=(const ChunkKey& o) const { return !(*this == o); }
    bool operator<(const ChunkKey& o) const { return y != o.y ? y < o.y : x < o.x; }
};

/// Floor division (tile indices are negative left of E1's corner).
inline int32_t floor_div(int32_t a, int32_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0)) ? 1 : 0); }
inline ChunkKey chunk_of_tile(int32_t tx, int32_t ty, int32_t chunk_tiles = kChunkTiles) { return {floor_div(tx, chunk_tiles), floor_div(ty, chunk_tiles)}; }

/// One row of the chunk directory.
struct NavChunkEntry {
    int32_t cx = 0, cy = 0;
    uint32_t tiles = 0;
    uint32_t size = 0;       // bytes of the chunk's blob
    uint64_t offset = 0;     // from the start of the file
    uint64_t hash = 0;       // FNV-1a 64 of the blob
};

struct NavFile {
    double center_x = 0, center_y = 0, radius = 0, margin = 0;
    uint64_t geometry_hash = 0;
    NavSettings settings;
    std::vector<TerrainGrid> terrain;
    std::vector<NavTileBlob> tiles;     // version 1: every tile.  version 2: every tile when built or read whole; empty when only the index was read (read_nav_index)

    // version 2 (chunked); chunk_tiles == 0 is a version 1 file
    uint32_t chunk_tiles = 0;
    uint64_t region_hash = 0;
    std::vector<RegionRect> region;
    std::vector<NavChunkEntry> chunks;  // read from a file; the writer computes it from `tiles`
    uint64_t tile_count_v2 = 0;         // tiles in all, as the header says (when only the index was read)

    bool chunked() const { return chunk_tiles != 0; }
};

/// Serialises to the layout above (version 2 when `f.chunked()`).  A big file is better written with write_nav_file.
std::vector<uint8_t> serialize_nav_file(const NavFile& f);
/// Writes the file without building it in memory; false with `err` on a failure.
bool write_nav_file(const NavFile& f, const std::string& path, std::string* err);
/// Parses; false with `err` set on a short, wrong or inconsistent file.  A version 2 file is read whole (every chunk).
bool parse_nav_file(const uint8_t* data, size_t size, NavFile* out, std::string* err);
bool read_nav_file(const std::string& path, NavFile* out, std::string* err);

/// Which layout a file has: 0 not a nav file / cannot open, 1 or 2.
int nav_file_version(const std::string& path);
/// Version 2 only: the header, the region, the terrain (resident) and the chunk directory; no tile is read.  `*file_size` gets the file's size.
bool read_nav_index(const std::string& path, NavFile* out, std::string* err);
/// Version 2 only: reads one chunk's tiles from an open file and checks them against the directory row (tile count, size, hash).  The stream's fail bits are cleared first (a
/// failed read of an earlier chunk does not fail this one).  `chunk_tiles` (the file's, tiles to a chunk side; 0: not checked) also checks that every tile lies in the chunk's own
/// row and that the tiles are unique and sorted.
bool read_nav_chunk(std::ifstream& in, const NavChunkEntry& e, std::vector<NavTileBlob>* out, std::string* err, uint32_t chunk_tiles = 0);

/// FNV-1a 64 of a byte range (the geometry hash).
uint64_t fnv1a64(const uint8_t* data, size_t size);

}  // namespace aoc::worldd::nav
