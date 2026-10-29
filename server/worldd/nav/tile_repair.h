// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  nav/tile_repair.h
//
//  The boundary edges of a Detour tile's detail mesh.
//
//  Detour answers "the point of this polygon nearest to that point" (findNearestPoly asks it of every polygon whose box the query box touches; ground_height, nearest_point and find_path
//  go through it) for a point that is not over the polygon from the detail triangles' edges that are marked as lying on the polygon's own border (DT_DETAIL_EDGE_BOUNDARY): it keeps
//  the nearest of them and interpolates along it.  A polygon with no such edge leaves it holding two null pointers, and it dereferences them (worldd.exe died that way when an NPC
//  started to wander next to such a polygon).
//
//  Recast marks an edge when both of its end points are within 0.001 of one of the polygon's edges, measured in float in the mesh's own units.  Here the unit is the centimetre and
//  the coordinates reach 450,000: a float's step is 0.03 there, so a point sampled on the border often lies a rounding step off it and the edge stays unmarked.  Of the Riverlands'
//  4.3 million polygons 27 had no marked edge at all and about 4 % of those far from the origin lacked at least one.
//
//  repair_detail_boundaries marks every detail triangle edge whose two end points lie within kBoundaryToleranceCm of one edge of the polygon (it never removes a mark), and hides a polygon
//  that has no marked edge even then (no detail triangle, a detail index outside the tile, no edge on the border) by clearing its flags: the query filter skips it, so Detour never asks
//  it for a closest point.  The builder runs it on every tile it makes; the loader runs it on every tile it reads, so a file made before the fix is safe as well.
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>

namespace aoc::worldd::nav {

/// How far (cm) from a polygon's edge a detail vertex may lie and still count as on it.  The rounding of a float at the file's largest coordinates is 0.03 cm.
inline constexpr double kBoundaryToleranceCm = 0.1;

struct TileRepair {
    uint64_t edges_flagged = 0;    // detail triangle edges that lay on a polygon's border and were not marked; now marked
    uint64_t polys_repaired = 0;   // polygons that had no marked edge at all (the ones that made Detour dereference null) and have one now
    uint64_t polys_hidden = 0;     // polygons that still cannot answer a closest-point query: their flags are cleared
    TileRepair& operator+=(const TileRepair& o) {
        edges_flagged += o.edges_flagged;
        polys_repaired += o.polys_repaired;
        polys_hidden += o.polys_hidden;
        return *this;
    }
};

/// Repairs a Detour tile (dtCreateNavMeshData's layout) in place.  A buffer that is not a tile of this layout (wrong magic, version or size) is left alone.  No allocation outside the
/// function's own scratch; safe to call from any thread on its own buffer.
TileRepair repair_detail_boundaries(unsigned char* tile, size_t size);

}  // namespace aoc::worldd::nav
