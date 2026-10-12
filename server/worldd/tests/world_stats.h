// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  server/worldd/tests/world_stats.h
//
//  The stat context (plan B3) a test gives build_player_setup: the stat program of a ContentDb parsed and verified once, and the catalog of
//  its items' stat data. Kept per file path, for the life of the test.
// ============================================================================
#pragma once

#include "bootstrap/character_setup.h"
#include "content/content_db.h"
#include "stats/stat_program.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <utility>

namespace world_stats {

struct Holder {
    const aoc::worldd::content::ContentDb*                 owner = nullptr;
    aoc::worldd::stats::StatBundle                         bundle;
    std::unique_ptr<aoc::worldd::content::ContentGearCatalog> gear;
    aoc::worldd::bootstrap::StatContext                    ctx;
};

/// The stat context of `db`: its stat_program row, parsed (the test exits with status 2 when that does not work: the fixture or the real
/// content.db must have a valid one) and the gear catalog on it. `db` must outlive every use of the context.
inline const aoc::worldd::bootstrap::StatContext& of(const aoc::worldd::content::ContentDb& db) {
    // Kept per file and per object, and never replaced: a Holder that was replaced when the same file was opened again left the contexts of the rigs that
    // were still alive pointing at a bundle that was gone (a flaky crash of a test that holds two rigs at once). A db object at an address that was used
    // before shares the Holder of the same file: its catalog reads through the live object at that address.
    static std::map<std::pair<std::string, const aoc::worldd::content::ContentDb*>, std::unique_ptr<Holder>> held;
    std::unique_ptr<Holder>& h = held[{db.path(), &db}];
    if (!h) {
        h = std::make_unique<Holder>();
        h->owner = &db;
        std::string error;
        const auto row = db.stat_program(error);
        if (!row || !aoc::worldd::stats::parse_bundle(row->data, h->bundle, error)) {
            std::printf("  [FAIL] test setup: %s has no usable stat program: %s\n", db.path().c_str(), error.c_str());
            std::exit(2);
        }
        h->gear = std::make_unique<aoc::worldd::content::ContentGearCatalog>(db);
        h->ctx = {&h->bundle, h->gear.get(), aoc::worldd::stats::Rounding::HalfUp};
    }
    return h->ctx;
}

}  // namespace world_stats
