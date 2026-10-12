// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  spawns/region_index.cpp  (see region_index.h)
// ============================================================================
#include "spawns/region_index.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace aoc::worldd::spawns::region {

namespace {

/// A point as the integer cm the rectangles are in: the floor, or -1 (not found) for a non-finite or absurd number.
bool to_cm(double v, int64_t* out) {
    if (!std::isfinite(v) || std::fabs(v) > 1.0e12) return false;
    *out = static_cast<int64_t>(std::floor(v));
    return true;
}

}  // namespace

const Index& Index::get() {
    static const Index index(kRiverlandsRects, kRiverlandsRectCount, kZois, kZoiCount, kZoiRects, kZoiRectCount);
    return index;
}

Index::Index(const Rect* rects, size_t rect_count, const ZoiRows* zois, size_t zoi_count, const Rect* zoi_rects, size_t zoi_rect_count)
    : rects_(rects), rect_count_(rect_count), zois_(zois), zoi_count_(zoi_count), zoi_rects_(zoi_rects), zoi_rect_count_(zoi_rect_count) {
    if (rect_count_ == 0) return;
    min_x_ = rects_[0].x0; min_y_ = rects_[0].y0; max_x_ = rects_[0].x1; max_y_ = rects_[0].y1;
    for (size_t i = 1; i < rect_count_; ++i) {
        min_x_ = std::min(min_x_, rects_[i].x0); min_y_ = std::min(min_y_, rects_[i].y0);
        max_x_ = std::max(max_x_, rects_[i].x1); max_y_ = std::max(max_y_, rects_[i].y1);
    }
    columns_ = (max_x_ - min_x_ + kIndexSquareCm - 1) / kIndexSquareCm;
    rows_ = (max_y_ - min_y_ + kIndexSquareCm - 1) / kIndexSquareCm;
    build(footprint_, rects_, rect_count_);
    if (zoi_rect_count_ != 0) {
        build(zoi_, zoi_rects_, zoi_rect_count_);
        zoi_row_of_rect_.assign(zoi_rect_count_, -1);
        for (size_t z = 0; z < zoi_count_; ++z) {
            for (uint32_t k = 0; k < zois_[z].count; ++k) {
                const size_t at = zois_[z].first + k;
                if (at < zoi_rect_count_) zoi_row_of_rect_[at] = static_cast<int32_t>(z);
            }
        }
    }
}
// -- Cyb . C | O | A --

void Index::build(Grid& grid, const Rect* rects, size_t count) const {
    const size_t squares = static_cast<size_t>(columns_) * static_cast<size_t>(rows_);
    std::vector<uint32_t> counts(squares + 1, 0);
    const auto span = [&](const Rect& r, int* c0, int* c1, int* r0, int* r1) {
        *c0 = std::max(0, (r.x0 - min_x_) / kIndexSquareCm);
        *c1 = std::min(columns_ - 1, (r.x1 - 1 - min_x_) / kIndexSquareCm);
        *r0 = std::max(0, (r.y0 - min_y_) / kIndexSquareCm);
        *r1 = std::min(rows_ - 1, (r.y1 - 1 - min_y_) / kIndexSquareCm);
    };
    for (size_t i = 0; i < count; ++i) {
        int c0, c1, r0, r1;
        span(rects[i], &c0, &c1, &r0, &r1);
        for (int r = r0; r <= r1; ++r)
            for (int c = c0; c <= c1; ++c) ++counts[static_cast<size_t>(r) * columns_ + c + 1];
    }
    for (size_t s = 0; s < squares; ++s) counts[s + 1] += counts[s];
    grid.offsets = counts;
    grid.items.assign(counts[squares], 0);
    std::vector<uint32_t> fill(counts.begin(), counts.end() - 1);
    for (size_t i = 0; i < count; ++i) {
        int c0, c1, r0, r1;
        span(rects[i], &c0, &c1, &r0, &r1);
        for (int r = r0; r <= r1; ++r)
            for (int c = c0; c <= c1; ++c) grid.items[fill[static_cast<size_t>(r) * columns_ + c]++] = static_cast<uint32_t>(i);
    }
}

int Index::find(const Grid& grid, const Rect* rects, double x, double y) const {
    if (rect_count_ == 0) return -1;
    int64_t ix, iy;
    if (!to_cm(x, &ix) || !to_cm(y, &iy)) return -1;
    if (ix < min_x_ || iy < min_y_ || ix >= max_x_ || iy >= max_y_) return -1;
    const size_t square = static_cast<size_t>((iy - min_y_) / kIndexSquareCm) * static_cast<size_t>(columns_) + static_cast<size_t>((ix - min_x_) / kIndexSquareCm);
    for (uint32_t k = grid.offsets[square]; k < grid.offsets[square + 1]; ++k) {
        const Rect& r = rects[grid.items[k]];
        if (ix >= r.x0 && ix < r.x1 && iy >= r.y0 && iy < r.y1) return static_cast<int>(grid.items[k]);
    }
    return -1;
}

bool Index::contains(double x, double y) const { return find(footprint_, rects_, x, y) >= 0; }

int Index::zoi_row(double x, double y) const {
    if (zoi_rect_count_ == 0) return -1;
    const int at = find(zoi_, zoi_rects_, x, y);
    return at < 0 ? -1 : zoi_row_of_rect_[static_cast<size_t>(at)];
}

uint64_t Index::zoi_guid(double x, double y) const {
    const int row = zoi_row(x, y);
    return row < 0 ? 0 : zois_[row].guid;
}

const char* Index::zoi_name(double x, double y) const {
    const int row = zoi_row(x, y);
    return row < 0 ? nullptr : zois_[row].name;
}

}  // namespace aoc::worldd::spawns::region
