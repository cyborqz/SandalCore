// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  items/resource_grid.cpp
//
//  See resource_grid.h.
// ============================================================================
#include "items/resource_grid.h"

namespace aoc::worldd::items {

ResourceGrid::ResourceGrid(int columns, int rows, bool custom, std::vector<std::vector<int>> matrix)
    : columns_(columns > 0 && rows > 0 && columns * rows <= kMaxResourceCells ? columns : 0),
      rows_(columns_ > 0 ? rows : 0),
      custom_(custom && columns_ > 0),
      matrix_(std::move(matrix)) {}

const std::vector<std::vector<int>>& ResourceGrid::override_matrix() const {
    static const std::vector<std::vector<int>> kNone;
    return custom_ ? matrix_ : kNone;
}

bool ResourceGrid::blocked(int x, int y) const {
    if (!custom_ || x < 0 || y < 0 || static_cast<size_t>(x) >= matrix_.size()) return false;
    const std::vector<int>& column = matrix_[static_cast<size_t>(x)];
    return static_cast<size_t>(y) < column.size() && column[static_cast<size_t>(y)] == kCellBlocked;
}

int ResourceGrid::usable_cells() const {
    int n = 0;
    for (int y = 0; y < rows_; ++y) {
        for (int x = 0; x < columns_; ++x) n += blocked(x, y) ? 0 : 1;
    }
    return n;
}

bool ResourceGrid::fits(int slot, int w, int h, const std::vector<bool>& covered) const {
    if (w < 1 || h < 1 || !contains(slot)) return false;
    const int x0 = x_of(slot), y0 = y_of(slot);
    if (x0 + w > columns_ || y0 + h > rows_) return false;
    for (int y = y0; y < y0 + h; ++y) {
        for (int x = x0; x < x0 + w; ++x) {
            if (blocked(x, y)) return false;
            const size_t i = static_cast<size_t>(slot_of(x, y));
            if (i < covered.size() && covered[i]) return false;
        }
    }
    return true;
}
// [Cyb + C | O | A]

int ResourceGrid::place(int w, int h, const std::vector<bool>& covered) const {
    for (int y = 0; y < rows_; ++y) {
        for (int x = 0; x < columns_; ++x) {
            if (fits(slot_of(x, y), w, h, covered)) return slot_of(x, y);
        }
    }
    return -1;
}

void ResourceGrid::cover(std::vector<bool>& covered, int slot, int w, int h) const {
    if (covered.size() < static_cast<size_t>(cells())) covered.resize(static_cast<size_t>(cells()), false);
    const int x0 = x_of(slot), y0 = y_of(slot);
    for (int y = y0; y < y0 + h && y < rows_; ++y) {
        for (int x = x0; x < x0 + w && x < columns_; ++x) covered[static_cast<size_t>(slot_of(x, y))] = true;
    }
}

}  // namespace aoc::worldd::items
