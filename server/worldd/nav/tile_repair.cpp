// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "nav/tile_repair.h"

#include <DetourNavMesh.h>

#include <cstring>
#include <vector>

namespace aoc::worldd::nav {

namespace {

constexpr size_t al4(size_t x) { return (x + 3) & ~static_cast<size_t>(3); }

/// The distance (squared, in the horizontal plane) from p to the segment a-b.
double dist_sqr_to_segment(const double* p, const double* a, const double* b) {
    const double abx = b[0] - a[0], abz = b[1] - a[1];
    const double len = abx * abx + abz * abz;
    double t = len > 0 ? ((p[0] - a[0]) * abx + (p[1] - a[1]) * abz) / len : 0.0;
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    const double dx = a[0] + abx * t - p[0], dz = a[1] + abz * t - p[1];
    return dx * dx + dz * dz;
}

}  // namespace

TileRepair repair_detail_boundaries(unsigned char* tile, size_t size) {
    TileRepair out;
    if (!tile || size < sizeof(dtMeshHeader)) return out;
    dtMeshHeader h;
    std::memcpy(&h, tile, sizeof h);
    if (h.magic != DT_NAVMESH_MAGIC || h.version != DT_NAVMESH_VERSION) return out;
    if (h.polyCount <= 0 || h.vertCount < 0 || h.maxLinkCount < 0 || h.detailMeshCount < 0 || h.detailVertCount < 0 || h.detailTriCount < 0 || h.bvNodeCount < 0 || h.offMeshConCount < 0)
        return out;
    // the layout dtCreateNavMeshData writes (and area_nav.cpp's tile_is_sound checks): a tile of any other size is not read
    const size_t o_verts = al4(sizeof(dtMeshHeader));
    const size_t o_polys = o_verts + al4(sizeof(float) * 3 * static_cast<size_t>(h.vertCount));
    const size_t o_links = o_polys + al4(sizeof(dtPoly) * static_cast<size_t>(h.polyCount));
    const size_t o_dmeshes = o_links + al4(sizeof(dtLink) * static_cast<size_t>(h.maxLinkCount));
    const size_t o_dverts = o_dmeshes + al4(sizeof(dtPolyDetail) * static_cast<size_t>(h.detailMeshCount));
    const size_t o_dtris = o_dverts + al4(sizeof(float) * 3 * static_cast<size_t>(h.detailVertCount));
    const size_t total = o_dtris + al4(4 * static_cast<size_t>(h.detailTriCount)) + al4(sizeof(dtBVNode) * static_cast<size_t>(h.bvNodeCount)) +
                         al4(sizeof(dtOffMeshConnection) * static_cast<size_t>(h.offMeshConCount));
    if (total != size) return out;

    const float* verts = reinterpret_cast<const float*>(tile + o_verts);
    dtPoly* polys = reinterpret_cast<dtPoly*>(tile + o_polys);
    const dtPolyDetail* dmeshes = reinterpret_cast<const dtPolyDetail*>(tile + o_dmeshes);
    const float* dverts = reinterpret_cast<const float*>(tile + o_dverts);
    unsigned char* dtris = tile + o_dtris;

    const double tol2 = kBoundaryToleranceCm * kBoundaryToleranceCm;
    std::vector<double> pts;          // x, z of the polygon's own vertices, then of its detail mesh's extra vertices
    std::vector<unsigned char> on;    // per point: the bits of the polygon edges it lies on (a polygon has at most DT_VERTS_PER_POLYGON of them)
    for (int pi = 0; pi < h.polyCount; ++pi) {
        dtPoly& poly = polys[pi];
        if (poly.getType() != DT_POLYTYPE_GROUND) continue;   // an off-mesh connection has no detail mesh
        const int nv = poly.vertCount;
        bool usable = pi < h.detailMeshCount && nv >= 3 && nv <= DT_VERTS_PER_POLYGON;
        if (usable) {
            const dtPolyDetail& pd = dmeshes[pi];
            usable = pd.triCount > 0 && static_cast<long long>(pd.triBase) + pd.triCount <= h.detailTriCount &&
                     static_cast<long long>(pd.vertBase) + pd.vertCount <= h.detailVertCount;
            for (int k = 0; usable && k < nv; ++k) usable = static_cast<int>(poly.verts[k]) < h.vertCount;
        }
        if (!usable) {   // Detour would read outside the tile's arrays, or have no triangle to take a point from
            poly.flags = 0;
            ++out.polys_hidden;
            continue;
        }
        const dtPolyDetail& pd = dmeshes[pi];
        const int nd = pd.vertCount;
        pts.resize(static_cast<size_t>(nv + nd) * 2);
        for (int k = 0; k < nv; ++k) { pts[k * 2] = verts[poly.verts[k] * 3]; pts[k * 2 + 1] = verts[poly.verts[k] * 3 + 2]; }
        for (int k = 0; k < nd; ++k) { pts[(nv + k) * 2] = dverts[(pd.vertBase + k) * 3]; pts[(nv + k) * 2 + 1] = dverts[(pd.vertBase + k) * 3 + 2]; }
        on.assign(static_cast<size_t>(nv + nd), 0);
        for (int e = 0; e < nv; ++e) {
            const double* a = &pts[static_cast<size_t>(e) * 2];
            const double* b = &pts[static_cast<size_t>((e + 1) % nv) * 2];
            for (int k = 0; k < nv + nd; ++k)
                if (dist_sqr_to_segment(&pts[static_cast<size_t>(k) * 2], a, b) <= tol2) on[static_cast<size_t>(k)] |= static_cast<unsigned char>(1u << e);
        }
        bool had = false, has = false;
        uint64_t added = 0;
        bool sound = true;
        for (int t = 0; t < pd.triCount && sound; ++t) {
            unsigned char* tri = &dtris[(static_cast<size_t>(pd.triBase) + t) * 4];
            if (tri[0] >= nv + nd || tri[1] >= nv + nd || tri[2] >= nv + nd) { sound = false; break; }
            for (int e = 0; e < 3; ++e) {
                const unsigned char bit = static_cast<unsigned char>(DT_DETAIL_EDGE_BOUNDARY << (e * 2));
                if (tri[3] & bit) { had = true; has = true; continue; }
                if (on[tri[e]] & on[tri[(e + 1) % 3]]) { tri[3] = static_cast<unsigned char>(tri[3] | bit); ++added; has = true; }
            }
        }
        if (!sound || !has) {
            poly.flags = 0;
            ++out.polys_hidden;
            continue;
        }
        out.edges_flagged += added;
        if (!had) ++out.polys_repaired;
    }
    return out;
}

}  // namespace aoc::worldd::nav
