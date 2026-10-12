// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  spawns/node_population.h  (Nodes 1, stage 3b)
//
//  The town's people: the NPCs a city node stands with at its level, selected from content.db's population chain and the node tables (contentc version 12).  Pure: the same inputs give the
//  same list; nothing here reads a clock, a registry or the network (the live manager is spawns/node_populations.h).
//
//  What a node's people are (research data_world.md s3.4 and s3.5):
//    layout sets    the node's layout's PopulationSetsIds (`node_layout_population`, 201 sets over the layouts): the guards, villagers, quest givers and mobs of a level.  A set is placed at
//                   the node RECORD's location with the record's yaw (placement id = the node's guid).
//    building sets  for each plot of the layout at the node's level (the node wire's plot rows, world/node_wire.h: the same plots the summary and the scenery list) that has a building and
//                   a state of UnderConstruction (1) or Operational (2): the building's BuildingLevelSpawn of the node's level (`building_pop_spawn`: the highest level key at or below
//                   the node's) and the sets it lists for that state (`building_level_spawn_pop`, 332 sets): the vendors, crafting stations, clerks and managers.  A set is placed at the
//                   plot's transform in the node's frame (placement id = the node's guid xor the plot's guid times the golden ratio).
//
//  The frames are the data's own, fitted to the retail captures: of the 506 town NPCs capture 721 shows at Joeva, 407 stand within 3 m of a layout point placed at the node record's location
//  and yaw (the open location of the town's replicator, 6 to 40 m away, fits 18), and of the 268 whose Blueprint a building names 81 stand within 3 m of a building point placed at the
//  plot's transform in that frame (the other readings of the plot's yaw: 7 and 25).
//
//  The context.  Each node is judged as itself (SpawnContext): its id, type, level, biome (its zone's) and zone of influence (the zone refusal is dropped: a filter that names the node's zone
//  passes), Kaelar, the SiegeInactive tag it carries, and the spawn sources that are running for a node: Layout and BuildingLevelSpawn.  The siege tags a filter requires are any-of (the filter's
//  rule).  The humanoids whose body needs an NPCAppearanceComponent are shown when a retail capture opened that Blueprint (its open carries a component): the town's villagers, wizards and
//  clerks; the Blueprints no capture opened stay out, as in the open world.
//
//  The tests' golden is an independent Python port of all this: tools/capture/node_pop_oracle.py, gen_node_pop_golden.py, tests/node_pop_golden.h.
// ============================================================================
#pragma once

#include "content/content_db.h"
#include "spawns/area.h"
#include "spawns/population.h"
#include "world/city_nodes.h"
#include "world/node_wire.h"

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace aoc::worldd::spawns {

/// ENodeLevel / ENodeType / EBiomeType names (the filters' strings); "" for a number outside the enum.
const char* node_level_name(int level);
const char* node_type_name(int type);
const char* biome_name(int biome);

/// The spawn sources that run for a node (npc_gate.source).
inline constexpr const char* kLayoutGate = "Layout";
inline constexpr const char* kBuildingGate = "BuildingLevelSpawn";

/// What a node is to its people.
struct NodeSite {
    int64_t     guid = 0;
    std::string name;            // the record name
    double      x = 0, y = 0, z = 0, yaw = 0;   // the record's location (cm) and yaw (degrees)
    int         type = 0;        // ENodeType
    int         level = 0;       // ENodeLevel, now
    std::string biome;           // its zone's biome ("Riverlands"; "" when the node has no zone)
    int64_t     zoi = 0;         // its zone of influence
    int64_t     layout = 0;      // LayoutRecord
};

/// The site of a registry node at a level: its record's place and yaw, its type, its zone's biome and its layout.
NodeSite node_site_of(const world::CityNodes& nodes, const world::NodeWire& wire, const world::CityNodeInfo& info, int level);

enum class PopSource : uint8_t { Layout, Building };

/// One NPC of a node's people.
struct NodeSpawn {
    DataSpawn spawn;
    PopSource source = PopSource::Layout;
    int64_t   plot = 0;          // the plot a building's NPC stands at (0 for the layout's)
    /// The point's identity: the same point of the same set at another level has the same one (the live manager keeps an NPC across a level change when its identity and its asset are the same).
    uint64_t  ident = 0;
};
// [C-O-A - Cyb]

/// What a selection found, for the log and the tests.
struct NodePopStats {
    size_t sets[2] = {0, 0};        // sets placed, by source
    size_t points[2] = {0, 0};      // points of those sets (before any filter)
    size_t spawned[2] = {0, 0};
    size_t missing_sets = 0;        // a set the data names that is no row
    size_t outside = 0;             // points outside the area
    PopulationStats pop;            // the spawn filter's reasons, templates and the NPCs by Blueprint
    size_t spawned_total() const { return spawned[0] + spawned[1]; }
};

/// The node tables of content.db the selection reads, once (read-only).
class NodePopulationData {
public:
    /// nullptr, with `error`, when a table is missing (the file was built by a contentc before version 12).
    static std::unique_ptr<NodePopulationData> load(const std::string& content_db_path, std::string& error);

    /// Data made by hand (the tests'): `layout_sets` by layout, no building populations.
    static std::unique_ptr<NodePopulationData> make(std::map<int64_t, std::vector<int64_t>> layout_sets);

    const std::vector<int64_t>& layout_sets(int64_t layout) const;
    /// The BuildingLevelSpawn a building runs at a node level: the entry of the highest level key at or below `level` (0: none).
    int64_t spawn_for(int64_t building, int level) const;
    /// The level keys a building has a BuildingLevelSpawn for, ascending (the tests').
    std::vector<int> levels_of(int64_t building) const;
    /// The sets of a BuildingLevelSpawn in a state (EBuildingState).
    const std::vector<int64_t>& spawn_sets(int64_t spawn, int state) const;

    size_t layouts() const { return layout_sets_.size(); }
    size_t buildings() const { return building_spawns_.size(); }

private:
    std::map<int64_t, std::vector<int64_t>>                  layout_sets_;
    std::unordered_map<int64_t, std::map<int, int64_t>>      building_spawns_;   // building -> level -> BuildingLevelSpawn
    std::map<std::pair<int64_t, int>, std::vector<int64_t>>  spawn_sets_;        // (spawn, state) -> sets
    std::vector<int64_t>                                     none_;
};

/// A node's spawn context: `base` (the world's: culture, season, hour ...) with the node's id, type, level, biome and zone, the sources that run, and `shown` added to the humanoids shown.
SpawnContext node_spawn_context(const SpawnContext& base, const NodeSite& site, const std::set<std::string>& shown = {});

/// The yaw (degrees) of a rotation quaternion.
double quaternion_yaw(double qx, double qy, double qz, double qw);

/// A set placed: its origin, yaw and what placed it.
struct PlacedSet {
    int64_t   placement = 0;
    int64_t   set = 0;
    double    x = 0, y = 0, z = 0, yaw = 0;
    PopSource source = PopSource::Layout;
    int64_t   plot = 0;
};

/// The sets of a node at its level, placed, in order: the layout's, then each plot's.
std::vector<PlacedSet> placed_sets(const NodePopulationData& data, const world::NodeWire& wire, const NodeSite& site);

/// The node's people: every set of `placed_sets` through the spawn filter `filter` (built from node_spawn_context).  Points outside `area` are dropped (the ground ends there).
std::vector<NodeSpawn> select_node_spawns(const content::PopulationData& population, const TemplateSource& templates, const NodePopulationData& data, const world::NodeWire& wire,
                                          const NodeSite& site, const AreaConfig& area, const SpawnFilter& filter, NodePopStats& stats);

}  // namespace aoc::worldd::spawns
