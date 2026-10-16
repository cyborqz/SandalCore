// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  items/resource_grid.h  (Gathering stage 3, part "Resource storage")
//
//  The grid of a resource container: what the client's UResourceStorageComponent lays out for the resource bag worn in Equipment slot 23, 24 or 25 (EItemLocation
//  Resource0 to Resource2).  Pure logic: no sockets, no database.
//
//  The bag item's ResourceBagDef gives the grid (content.db resource_bag): `columns` x `rows` cells (maximum_items = columns * rows, blocked cells included), and a matrix,
//  a list of COLUMNS each a list of rows with -1 for a usable cell and -2 for a blocked one, that applies only when the def says bCustomDimension (the Sanctus def carries a
//  four-column matrix its 6 x 4 grid ignores).  Retail's StoragePayload carries the matrix as OverrideMatrix when the bag has one [R: capture 1198: Resource0 (Sanctus) none,
//  Resource1 (Mining T2, 8 x 5) and Resource2 (Lumberjacking T2, 6 x 5) theirs].
//
//  An item takes inv_w x inv_h cells and is never turned.  The slot the client is told (the event's slot, the payload's) is the row-major index of the item's top-left cell,
//  slot = y * columns + x, and a new stack goes to the FIRST free rectangle scanning rows, then columns, skipping blocked and covered cells [R: 12 captured slots in five
//  captures fit this: 1198's Resource1 and Resource2 lists, 1469, 1470 and 1471; tests/test_worldd_resource.cpp checks each].
// ============================================================================
#pragma once

#include <cstdint>
#include <vector>

namespace aoc::worldd::items {

/// The most cells a resource bag has (the Livestream promotion bag is 8 x 30 = 240): a slot of a resource container is below this, the character service's limit too.
inline constexpr int kMaxResourceCells = 256;
/// The values of a bag matrix's cells.
inline constexpr int kCellUsable = -1;
inline constexpr int kCellBlocked = -2;

class ResourceGrid {
public:
    /// No bag: a container with no cells (nothing fits, nothing is sent).
    ResourceGrid() = default;
    /// A grid of `columns` x `rows` cells.  `matrix` is the def's BagMatrixOverride by column (Matrix[x][y]); it applies only when `custom` (a cell the matrix does not reach is usable).
    ResourceGrid(int columns, int rows, bool custom, std::vector<std::vector<int>> matrix = {});

    bool empty() const { return cells() == 0; }
    int columns() const { return columns_; }
    int rows() const { return rows_; }
    /// maximum_items: every cell, blocked ones included.
    int cells() const { return columns_ * rows_; }
    int usable_cells() const;
    bool custom() const { return custom_; }
    /// The matrix as the client is sent it (StoragePayload.override_matrix): the def's columns when it applies, none otherwise.
    const std::vector<std::vector<int>>& override_matrix() const;

    bool contains(int slot) const { return slot >= 0 && slot < cells(); }
    int x_of(int slot) const { return columns_ > 0 ? slot % columns_ : 0; }
    int y_of(int slot) const { return columns_ > 0 ? slot / columns_ : 0; }
    int slot_of(int x, int y) const { return y * columns_ + x; }
    bool blocked(int x, int y) const;
// C-O-A + Cyb

    /// Whether a `w` x `h` rectangle with its top-left cell in `slot` lies inside the grid, covers no blocked cell and no cell of `covered` (one flag per cell, row-major; it may be empty:
    /// nothing is covered).
    bool fits(int slot, int w, int h, const std::vector<bool>& covered) const;
    /// The slot of the first free `w` x `h` rectangle (rows, then columns), -1 when there is none.
    int place(int w, int h, const std::vector<bool>& covered) const;
    /// Marks the cells of a rectangle (the caller checked it fits).
    void cover(std::vector<bool>& covered, int slot, int w, int h) const;

    bool operator==(const ResourceGrid& o) const = default;

private:
    int                           columns_ = 0, rows_ = 0;
    bool                          custom_ = false;
    std::vector<std::vector<int>> matrix_;
};

}  // namespace aoc::worldd::items
