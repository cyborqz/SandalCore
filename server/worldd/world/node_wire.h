// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/node_wire.h  (Nodes 1, stage 2)
//
//  What the client is told about the city nodes, made from content.db's fixed data and the registry's live levels (world/city_nodes.h):
//
//    maps()          the GameState's node maps (protocol/repl/node_rpc.h): NodeIDToClientNodeData for retail's 46 nodes, the progress map (the nodes at level >= 1,
//                    keyed by RANK), the citizens map (level >= 3, counts 0) and EstateIDToOwnerGuid (the 244 estates, no owner)
//    summary()       the answer to ServerRequestNodeSummary: the layout, the level, the plots of the layout at that level, the essence and relic slots
//    siege()         the answer to ServerRequestCurrentSiegeSummary: the idle default of a node no war is declared on
//
//  The 46.  Retail's GameState lists the nodes of the Verra world: the records named Verra_* that have a map (the four Verra_* records with no map name, Lawless_Desert,
//  Lawless_Jungle and the two dungeons, are not in it; the client asks for the summaries of the dungeons and the jungle all the same and gets the small default of a
//  record that is no node).  Capture 1198 line 47, evidence.md section 4.1.
//
//  The plots.  A plot of the layout at the node's level is listed (a Town has 31); the pre-built plots (the civic core and, by the spike's decision, the NodeXP
//  construction projects) are Operational (2) with their building, the mayor's service plots are Empty (0) with the plot's empty building.  The plot logic is the
//  one the replicator's payload uses (spike_replicator/derive_payload.py), so the summary and the scenery agree: plot_rows() is the shared source.
//
//  What this does NOT know: mayors, citizens, a war, a relic, a skill point (nobody has one until the milestones that make them); the fields are the defaults.
// ============================================================================
#pragma once

#include "protocol/repl/node_rpc.h"
#include "world/city_nodes.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace aoc::worldd::world {

/// One plot of a layout at a level, with what stands on it at the start of the world.
struct PlotRow {
    int64_t plot = 0;        // ServiceBuildingPlot
    int64_t building = 0;    // the building on it (a pre-built plot's) or the plot's empty building
    uint8_t state = 0;       // EBuildingState: Empty 0, UnderConstruction 1, Operational 2
    uint8_t starting_state = 0;   // the plot's StartingState in the design data (the five NodeXP projects: UnderConstruction 1); `state` is what this world starts them at
    bool    pre_construct = false;
    double  qx = 0, qy = 0, qz = 0, qw = 1, tx = 0, ty = 0, tz = 0, sx = 1, sy = 1, sz = 1;   // the plot's transform in the layout
};

/// What a node's level grants (the summed NodeModifications of its LevelDetails).
struct LevelGrants {
    int     level = 0;               // the defined level these belong to
    int     relic_slots = 0;
    int     node_essence = 0;
    float   vehicle_deploy_radius = 0;
};

class NodeWire {
public:
    /// Reads content.db (read-only) once.  nullptr with `error` when a table is missing (it was built by a contentc before version 12) or the registry has no node of
    /// the data.
    static std::unique_ptr<NodeWire> load(const std::string& content_db_path, const CityNodes& nodes, std::string& error);

    /// The GameState's maps for the registry's levels now.
    protocol::repl::NodeStateMaps maps(const CityNodes& nodes) const;

    /// The nodes of the GameState, as ranks, in rank order (retail's 46).
    const std::vector<int>& roster() const { return roster_; }
    bool in_roster(int rank) const;
// C O A and C Y B

    /// The summary of the node `guid` at its level now; nullopt for a guid that is no city node record.  `unix_now` places the maintenance a week ahead.
    std::optional<protocol::repl::NodeSummary> summary(int64_t guid, const CityNodes& nodes, int64_t unix_now) const;
    /// The idle siege summary of the node `guid`; nullopt for a guid that is no city node record.
    std::optional<protocol::repl::SiegeSummary> siege(int64_t guid, const CityNodes& nodes) const;

    /// The plots of `layout` at `level`, in the layout's order (the shared source of the summary and the scenery).
    const std::vector<PlotRow>& plot_rows(int64_t layout, int level) const;
    /// What the node's level grants: the highest defined level at or below `level` (nothing before the first).
    LevelGrants grants(int64_t node_guid, int level) const;
    /// The layout record of a node (0: none) and whether the record is a real node of a map (the dungeons and the jungle are not).
    int64_t layout_of(int64_t node_guid) const;
    bool is_real_node(int64_t node_guid) const;

    size_t estates() const { return estates_.size(); }
    size_t layouts() const { return plots_.size(); }

    /// The percent of the current level a node's experience is at (0 at the highest level; node_constants::kExpToLevel).
    static int percent_of(int level, int64_t experience);

    /// The six test estates left out of the 244 (names), for the log and the tests.
    static bool is_test_estate(const std::string& record_name);

private:
    struct NodeData {
        int64_t layout = 0;
        bool    real = false;
        std::vector<LevelGrants> grants;   // ascending by level
    };

    std::vector<int>                                       roster_;
    std::map<int64_t, NodeData>                            by_node_;
    std::map<std::pair<int64_t, int>, std::vector<PlotRow>> plots_;   // (layout, level)
    std::vector<int64_t>                                   estates_;
    std::vector<PlotRow>                                   none_;
};

/// The bounty commodity every node of capture 1198 carries (ItemRecordBase PlayerCommodity_Test_KyleJusto); the ocean node's is 0.
inline constexpr int64_t kBountyCommodity = 6064629965616447488ll;

}  // namespace aoc::worldd::world
