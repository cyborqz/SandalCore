// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/node_layout_data.h  (Nodes 1, stage 3a)
//
//  The items of a town's ANodeLayoutReplicator.LayoutAssetSetGuids, made from content.db alone (contentc version 12), exactly as the spike's derive_payload.py makes them
//  (research/spike_replicator: 0 differences in 18 captured node, level and season cases):
//
//    layout items   for each prop i of the node's layout (node_layout_prop, in index order) whose asset set is valid for the node:
//                   OwnerGuid the node, OwnerSecondaryId 0, OwnerArrayIndex i, AssetSetRecordGuid the prop's set, the prop's transform
//    plot items     for each plot of the layout at the node's level (NodeWire::plot_rows, the same source as the node summary) that has a building standing on it (the
//                   pre-built plots: the civic core and the NodeXP projects; the mayor's service plots send nothing while empty): for each asset set of the building in the
//                   plot's state (building_asset_state, in order) that is valid for the node: OwnerGuid the plot, OwnerSecondaryId the EBuildingState, OwnerArrayIndex the
//                   position in that list, the plot's transform
//
//  An asset set is valid for a node when SOME definition it holds (node_asset_set_member -> node_asset_def) is: the level between its NodeLevelMinimum and Maximum, the
//  node's culture in ValidCultures, its type in ValidNodeTypes, the season in ValidSeasons and the biome in ValidBiomes (or bAllBiomes).  ValidGameplayTags is empty
//  on all 634 definitions, so the tags play no part.
//
//  What a fresh server cannot know: the buildings a mayor built, the expansions bought (items of nodes with a history).  Those are left out.
//
//  The NodeXP projects.  The design data starts all six UnderConstruction (1).  The captures' live nodes at level 3 and 4 show the five small and medium ones Operational (2),
//  which is proven to render, and retail's summaries of Joeva, Winstead and Halcyon show the LARGE one still under construction (a Town's scenery filters its scaffold out;
//  KalTorhum alone has it built).  So this world starts every pre-built plot Operational except the Large project, which keeps the data's state (PlotPolicy::PreBuiltOperational,
//  the same states the node summary lists: NodeWire::plot_rows).  A test can also ask for the data's own starting state for all, or give the state of each plot.
// ============================================================================
#pragma once

#include "protocol/repl/node_layout.h"
#include "world/node_wire.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace aoc::worldd::world {

namespace repl = ::aoc::protocol::repl;

/// ESeason.
enum Season : uint8_t { kWinter = 0, kSpring = 1, kSummer = 2, kFall = 3 };

enum class PlotPolicy : uint8_t {
    PreBuiltOperational,   // every pre-built plot Operational (this world's start)
    DataStartingState,     // the plot's StartingState in the design data
};

/// What the definition filter asks of a node.
struct LayoutContext {
    int level = 0;        // ENodeLevel
    int culture = 0;      // ECharacterRace (Kaelar 0)
    int node_type = 0;    // ENodeType
    int season = kSpring;
    int biome = -1;       // EBiomeType; -1: no biome filter
};

struct LayoutRequest {
    int64_t       node = 0;           // CityNodeRecordBase
    LayoutContext context;
    PlotPolicy    plots = PlotPolicy::PreBuiltOperational;
    /// A test's: the state of each pre-built plot by plot guid (a plot not listed follows the policy).  A state of 0 leaves the plot out.
    const std::map<int64_t, uint8_t>* plot_states = nullptr;
};

class NodeLayoutData {
public:
    /// Reads content.db's layout, asset set, asset definition and building tables (read-only).  nullptr, with `error`, when a table is missing (the file was built by a
    /// contentc before version 12).
    static std::unique_ptr<NodeLayoutData> load(const std::string& content_db_path, std::string& error);

    /// The items of the request, layout items first (by prop index), then the plot items in the layout's plot order.  `wire` gives the node's layout and its plots.
    /// Empty for a node that is no real node.
    std::vector<repl::NodeLayoutItem> items(const NodeWire& wire, const LayoutRequest& request) const;
// [C O A ~ C | Y | B]

    /// Whether some definition of the asset set is valid for the context (false for a set the data does not have).
    bool set_valid(int64_t asset_set, const LayoutContext& context) const;

    size_t layouts() const { return props_.size(); }
    size_t asset_sets() const { return members_.size(); }
    size_t definitions() const { return defs_.size(); }
    size_t buildings() const { return building_sets_.size(); }

    /// The props of a layout (a test counts them): their number.
    size_t props_of(int64_t layout) const;

private:
    struct Prop {
        int       idx = 0;
        int64_t   asset_set = 0;
        repl::NodeLayoutItem item;   // the transform only
    };
    struct Def {
        int      level_min = 0, level_max = 6;
        bool     all_biomes = false;
        int64_t  cultures = 0, node_types = 0, seasons = 0, biomes = 0;
    };

    bool def_valid(const Def& d, const LayoutContext& c) const;

    std::map<int64_t, std::vector<Prop>>                     props_;          // by layout
    std::unordered_map<int64_t, std::vector<int64_t>>        members_;        // asset set -> definitions, in order
    std::unordered_map<int64_t, Def>                         defs_;
    std::unordered_map<int64_t, int64_t>                     building_sets_;  // building -> its BuildingNodeAssetSets
    std::map<std::pair<int64_t, int>, std::vector<int64_t>>  state_sets_;     // (BuildingNodeAssetSets, EBuildingState) -> asset sets, in order
};

}  // namespace aoc::worldd::world
