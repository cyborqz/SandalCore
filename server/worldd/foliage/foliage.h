// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  foliage/foliage.h  (Gathering stage 3, part "Foliage gathering")
//
//  The world's painted foliage as the session sees it: the map (foliage_map.h), the state of every instance (foliage_store.h), the cell actors that show it to the clients
//  (foliage_cells.h) and what the data says each def and state is worth (content.db version 15: foliage_def, foliage_state, gather_interaction).  Like gather::Gathering for the
//  placed resource nodes it is the world's, not one player's; the session (session/player_foliage.cpp) asks it and applies the answer to the player's items and experience.
//
//  The defs.  The map names each instance's def; the name is resolved to its foliage_def (the schema repeats one name, Stone_Limestone_Large_2, in two records alike in every number:
//  the first by guid is used).  A state of a def that has an interaction can be harvested; the interaction is a content::GatherDefRow made by Gathering::interaction, so the
//  stage-2 rules and rolls run unchanged.  73 defs are in the map; 69 can be harvested (the Riverlands_* pillars have no interaction).
//
//  The reach.  A start is accepted from max(the state's OverrideInteractionDistance, the interaction's InteractRadius) + its InteractTolerance + the state's CollisionRadius + 100 cm
//  (the stage-2 leeway) horizontally from the instance [modelled; the three captured starts stood 148 (Oak), 166 (Oak) and 75 (Daffodil) cm away: tools/capture, capture_foliage.h
//  with the TSV's rows].
// ============================================================================
#pragma once

#include "foliage/foliage_cells.h"
#include "foliage/foliage_map.h"
#include "foliage/foliage_store.h"
#include "gather/gathering.h"
#include "net/connection.h"

#include <memory>
#include <string>
#include <vector>

namespace aoc::worldd::foliage {

struct Config {
    std::string map_path;              // dist/content/region_foliage.tsv
    double      regrow_mult = 1.0;     // scales every phase of the regrowth (AOC_FOLIAGE_REGROW_MULT)
    /// Every cell has its state actor from the start (the client finds no instance without it: foliage_cells.h).  False is the first draft's behaviour, an actor made at a cell's first cut,
    /// kept for the tests that measure what the actors cost.
    bool        all_cells = true;
};

/// A def of the map, resolved against content.db.
struct DefInfo {
    std::string                      name;
    const content::FoliageDefRow*    row = nullptr;                     // nullptr: the name is no foliage_def
    const content::FoliageStateRow*  states[3] = {nullptr, nullptr, nullptr};      // by state: Infancy, Juvenile, Adult
    const content::GatherDefRow*     interactions[3] = {nullptr, nullptr, nullptr};   // the harvest of each state (nullptr: none)
    bool                             harvestable = false;               // some state has one
};

class Foliage {
public:
    /// nullptr, with `error`, when the map does not load or content.db has no foliage tables.  `gathering` and `registry` must outlive it.
    static std::unique_ptr<Foliage> make(const content::ContentDb& db, gather::Gathering& gathering, world::ActorRegistry& registry, FoliageCells::Minter mint, Config config,
                                         std::string& error);
// ~ C_Y_B - C-O-A ~

    const FoliageMap& map() const { return map_; }
    FoliageStore&     store() { return *store_; }
    const FoliageStore& store() const { return *store_; }
    FoliageCells&     cells() { return *cells_; }
    const FoliageCells& cells() const { return *cells_; }
    const Config&     config() const { return config_; }

    const DefInfo&    def_of(uint32_t row) const { return defs_[map_.rows()[row].def]; }
    const std::string& name_of(uint32_t row) const { return map_.def_names()[map_.rows()[row].def]; }
    size_t            defs() const { return defs_.size(); }
    size_t            harvestable_defs() const;
    size_t            unresolved_defs() const;
    size_t            duplicated_names() const { return duplicated_; }

    /// The horizontal distance in cm a start may come from for the instance in `state` (the model above); 0 when the state has no interaction.
    double reach_cm(uint32_t row, int state) const;

    /// A steady time on the store's clock (seconds).
    static double seconds(Clock::time_point t) { return std::chrono::duration<double>(t.time_since_epoch()).count(); }

    /// Runs the regrowth due by `now` and shows it to the clients; cheap when nothing is due.  Returns the number of changes.
    size_t tick(Clock::time_point now);
    /// The instance is harvested: Empty, shown to the clients.  nullopt when the row is unknown or Empty already.
    std::optional<Change> harvested(uint32_t row, Clock::time_point now);

    std::string describe() const;

private:
    Foliage(FoliageMap map, Config config) : map_(std::move(map)), config_(std::move(config)) {}

    FoliageMap                     map_;
    Config                         config_;
    std::vector<DefInfo>           defs_;          // by the map's def index
    std::unique_ptr<FoliageStore>  store_;
    std::unique_ptr<FoliageCells>  cells_;
    size_t                         duplicated_ = 0;
    size_t                         logged_ = 0;        // the regrowth changes said at info so far (the first 60; the rest at debug)
};

}  // namespace aoc::worldd::foliage
