// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/zoi_surface.cpp (see zoi_surface.h)
// ============================================================================
#include "world/zoi_surface.h"

#include "world/zoi_surface_table.h"

#include <cmath>
#include <iterator>

namespace aoc::worldd::world {

namespace zs = zoi_surface;

uint64_t zoi_at(double x_cm, double y_cm) {
    if (!std::isfinite(x_cm) || !std::isfinite(y_cm)) return 0;
    const double fx = std::floor(x_cm), fy = std::floor(y_cm);
    // far outside the table's box, before the integer conversion
    if (std::fabs(fx) > 1.0e9 || std::fabs(fy) > 1.0e9) return 0;
    const int64_t ix = static_cast<int64_t>(fx), iy = static_cast<int64_t>(fy);
    // floor division: the chunk of a negative position is the one below
    auto floor_div = [](int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); };
    const int64_t cx = floor_div(ix, zs::kChunkCm), cy = floor_div(iy, zs::kChunkCm);
    const int64_t col = cx - zs::kMinCx, row = cy - zs::kMinCy;
    if (col < 0 || col >= zs::kColumns || row < 0 || row >= zs::kRowsOfChunks) return 0;
    const uint32_t slot = zs::kIndex[row * zs::kColumns + col];
    if (slot == zs::kNone) return 0;
    if (slot & zs::kUniform) return zs::kPalette[slot & 0xFFu];
    // the cell inside the chunk, row-major
    const int64_t cell_x = (ix - cx * zs::kChunkCm) / zs::kCellCm, cell_y = (iy - cy * zs::kChunkCm) / zs::kCellCm;
    const int64_t want = cell_y * zs::kCells + cell_x;
    int64_t at = 0;
    for (size_t p = slot; p + 2 < std::size(zs::kRuns); p += 3) {
        at += static_cast<int64_t>(zs::kRuns[p]) | (static_cast<int64_t>(zs::kRuns[p + 1]) << 8);
        if (want < at) return zs::kPalette[zs::kRuns[p + 2]];
    }
    return 0;
}

std::vector<uint64_t> zoi_surface_palette() { return std::vector<uint64_t>(std::begin(zs::kPalette), std::end(zs::kPalette)); }

ZoiSurfaceInfo zoi_surface_info() {
    ZoiSurfaceInfo info;
    for (const uint32_t slot : zs::kIndex) {
        if (slot == zs::kNone) continue;
        ++info.chunks;
        if (slot & zs::kUniform) ++info.uniform;
    }
    return info;
}

}  // namespace aoc::worldd::world
