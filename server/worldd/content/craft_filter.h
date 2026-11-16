// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  content/craft_filter.h
//
//  A gather-item filter's items (craft_filter_item, contentc version 45): a recipe input may name no item but a filter (CostGatherItemFilter), a curated list of items
//  (GatherItemFilterRecordBase.ItemsIds) the client shows as the input's allowed materials.  A pick for such an input is one of these items, not anything that carries
//  the input's tag.  The reader keeps each filter's items ascending and once, the shape ContentDb::craft_recipe_data holds them in.
// ============================================================================
#pragma once

#include <sqlite3.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace aoc::worldd::content {

/// Reads craft_filter_item (filter_guid, item_guid) into `out`, each filter's items ascending.  A file that has no such table is an older contentc's: `present` is false,
/// `out` is left empty and the call returns true.  False with `error` set when the table is there but cannot be read or a column is not an integer.
inline bool read_craft_filter_items(sqlite3* db, std::map<int64_t, std::vector<int64_t>>& out, bool& present, std::string& error) {
    present = false;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT filter_guid, item_guid FROM craft_filter_item ORDER BY filter_guid, item_guid", -1, &stmt, nullptr) != SQLITE_OK) {
        const std::string why = sqlite3_errmsg(db);
        if (why.find("no such table") != std::string::npos) return true;   // (contentc before 45: the filters then admit no item)
        error = "content.db craft_filter_item query failed: " + why;
        return false;
    }
    present = true;
    int step = SQLITE_ROW;
    while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (sqlite3_column_type(stmt, 0) != SQLITE_INTEGER || sqlite3_column_type(stmt, 1) != SQLITE_INTEGER) {
            error = "content.db craft_filter_item has a column of the wrong storage class";
            sqlite3_finalize(stmt);
            return false;
        }
        const int64_t filter = sqlite3_column_int64(stmt, 0);
        const int64_t item = sqlite3_column_int64(stmt, 1);
        std::vector<int64_t>& items = out[filter];
        if (items.empty() || items.back() != item) items.push_back(item);
    }
    if (step != SQLITE_DONE) {
        error = std::string("content.db craft_filter_item query failed: ") + sqlite3_errmsg(db);
        sqlite3_finalize(stmt);
        return false;
    }
    sqlite3_finalize(stmt);
    return true;
}

}  // namespace aoc::worldd::content
