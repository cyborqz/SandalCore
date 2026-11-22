// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/shared_world_data.h: one opened content.db and one WorldData per
//  database file for the life of a test program.  A Fixture builds its own
//  fresh PlayerWorld from it.  The cache outlives the Fixtures (they are
//  destroyed before the program ends), so a world never outlives its data.
// ============================================================================
#pragma once

#include "content/content_db.h"
#include "session/world_data.h"

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace shared_world {

struct Entry {
    std::string                          path;
    std::optional<aoc::worldd::content::ContentDb> db;
    std::shared_ptr<const aoc::worldd::WorldData>  data;
    std::string                          problem;
};

/// The opened database and its data for `path`, opened and loaded once.
inline std::map<std::string, std::unique_ptr<Entry>>& cache() {
    static std::map<std::string, std::unique_ptr<Entry>> opened;
    return opened;
}

inline Entry& get(const std::string& path) {
    std::unique_ptr<Entry>& slot = cache()[path];
    if (slot) return *slot;
    slot = std::make_unique<Entry>();
    slot->path = path;
    std::string error;
    slot->db = aoc::worldd::content::ContentDb::open(path, error);
    if (!slot->db) {
        slot->problem = "content.db: " + error;
        return *slot;
    }
    slot->data = aoc::worldd::WorldData::load(&*slot->db, error);
    slot->problem = error;
    return *slot;
}

/// How many different database files this program has opened.
inline size_t files() { return cache().size(); }

}  // namespace shared_world
