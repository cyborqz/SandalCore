// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/city_nodes.h  (Nodes 1, stage 1)
//
//  worldd's city node registry: what the design data says about the 81 city nodes (content.db's city_node and city_zoi, contentc version 12:
//  the rank, the type, the place, the zone of influence and its biome) and the nodes' LIVE state, the levels the citynode service holds
//  (svc/city_node_feed.h watches it).  "Node" is the settlement (CityNodeRecord), not a resource node or a talent node.
//
//    node_at(x, y)    the zone of influence at a point (world/zoi_surface.h, the whole world), and its node and biome
//    level_of(node)   the node's level now
//
//  Where the levels come from, and what happens when the service is not there: a registry starts with every node at Wilderness (source Default).  The
//  service's snapshot (Service) replaces every level at once, and each later change moves one.  When the service cannot be reached at start, worldd
//  loads the seed file's levels (Seed; svc/city_node_feed.h logs the one warning and keeps retrying); login never waits for any of it.
//
//  The node reads of an expression (stats/stat_program.h kHostNodeBiome ...) are answered by node_host_fn below, the C++ twin of statprog's NodeWorld:
//  services/internal/statprog/nodes.go says what each gives and the shared corpus (stats/testdata/corpus.txt, the `nodehost` blocks) checks both.
// ============================================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace aoc::worldd::world {

struct CityNodeInfo {
    int         rank = -1;          // the index in CityNodeRecordBase sorted by guid (0 to 80)
    int64_t     guid = 0;
    std::string name;               // the record name: Verra_RVR_Joeva
    std::string display_name;       // Joeva
    int         type = 0;           // ENodeType: Default 0, Scientific 1, Divine 2, Martial 3, Economic 4
    double      x = 0, y = 0, z = 0;   // cm
    double      yaw = 0;            // degrees (the record's rotation)
    std::string map_name;           // the map the location is on ("Verra_World_Master", "NODE_Playground" ...; "" for none)
    int64_t     zoi_guid = 0;       // the zone of influence of the node (0: none)
    bool        levelable = false;  // one of the 26 nodes of the five regions that grow
};

struct CityZoneInfo {
    int64_t     guid = 0;
    std::string name;
    int         biome = 0;          // EBiomeType (Riverlands 14)
    int64_t     node_guid = 0;      // the node the zone belongs to (0: none)
    int         node_rank = -1;     // ... as a rank
};

/// Where a point is: its zone of influence, the zone's node and biome.
struct NodeSite {
    int64_t zoi_guid = 0;   // 0: no zone of influence here
    int     node_rank = -1; // the zone's node, -1 for none
    int     biome = 0;      // the zone's biome; Default (0) for none
};
// C | O | A and C_Y_B

/// One node as the citynode service reports it.
struct LiveNode {
    int64_t guid = 0;
    int     level = 0;
    int64_t experience = 0;
    int64_t parent_guid = 0;
};

enum class LevelSource { Default, Seed, Service };

const char* level_source_name(LevelSource s);

class CityNodes {
public:
    /// The fixed data, in rank order (the ranks must be 0..n-1).
    CityNodes(std::vector<CityNodeInfo> nodes, std::vector<CityZoneInfo> zones);

    /// Reads content.db's city_node and city_zoi (read-only); null with `error` when the file has no such tables (it is older than contentc version 12)
    /// or they are not what the registry needs.
    static std::unique_ptr<CityNodes> load(const std::string& content_db_path, std::string& error);

    // ---- the fixed data
    size_t size() const { return nodes_.size(); }
    const std::vector<CityNodeInfo>& nodes() const { return nodes_; }
    const CityNodeInfo* by_rank(int rank) const { return rank >= 0 && static_cast<size_t>(rank) < nodes_.size() ? &nodes_[static_cast<size_t>(rank)] : nullptr; }
    const CityNodeInfo* by_guid(int64_t guid) const;
    /// A node by the record name ("Verra_RVR_Joeva"), the same without "Verra_" or the display name ("Joeva"): case, spaces and punctuation do not
    /// count (the service resolves names the same way).  Null when none or when the name is ambiguous.
    const CityNodeInfo* by_name(const std::string& name) const;
    const CityZoneInfo* zone_by_guid(int64_t guid) const;
    size_t zones() const { return zones_.size(); }

    /// The zone of influence at (x, y) cm, with its node and biome.
    NodeSite node_at(double x_cm, double y_cm) const;
    /// The same for a zone guid.
    NodeSite site_of_zoi(int64_t zoi_guid) const;

    // ---- the live state
    /// The node's level now: 0 (Wilderness) for a rank that is no node.
    int level_of_rank(int rank) const;
    int level_of(int64_t guid) const;
    /// The node's type (ENodeType), Default for a rank that is no node.
    int type_of_rank(int rank) const;
    LevelSource source() const { return source_.load(); }
    /// Bumped by every change of the live state: readers that cache something made from the levels compare it.
    uint64_t version() const { return version_.load(); }
    /// The live state of every node (the levels, XP and parents the source gave; Wilderness for the rest).
    std::vector<LiveNode> live() const;

    /// The service's whole state: every node's level (a node it does not list goes back to Wilderness).  One change; `source` is Service.
    void apply_snapshot(const std::vector<LiveNode>& nodes, LevelSource source = LevelSource::Service);
    /// One node changed.  False when the guid is no node of the registry.
    bool apply_change(const LiveNode& node);

    /// Loads the seed file's levels (deploy/citynode_seed.json: {"nodes": {"Verra_RVR_Joeva": {"level": 4}, "Verra_RVR_Halcyon": {"level": 3, "parent": ...}}}):
    /// source Seed.  A node the file names that the registry does not have, or a level outside 0..6, is skipped and counted in `skipped`.  False with `error`
    /// when the file cannot be read or is no seed.
    bool apply_seed_file(const std::string& path, std::string& error, size_t* applied = nullptr, size_t* skipped = nullptr);

    /// The registry's levels as text, for the log: "Joeva 4, Halcyon 3" (the nodes above Wilderness).
    std::string describe_levels() const;

private:
    static constexpr int kMaxLevel = 6;   // Metropolis: the most a level number can be (the service allows 4)
    void set_all(const std::vector<LiveNode>& nodes, LevelSource source);

    std::vector<CityNodeInfo>                 nodes_;
    std::vector<CityZoneInfo>                 zones_;
    std::unordered_map<int64_t, size_t>       by_guid_;
    std::unordered_map<int64_t, size_t>       zone_by_guid_;
    std::unique_ptr<std::atomic<int>[]>       levels_;   // by rank
    mutable std::mutex                        live_mu_;
    std::vector<LiveNode>                     live_;     // by rank, for the XP and the parent
    std::atomic<LevelSource>                  source_{LevelSource::Default};
    std::atomic<uint64_t>                     version_{0};
};

/// The evaluation context of an expression's node reads: where it runs.
struct NodeContext {
    int node = -1;          // the rank of the node of the zone the expression runs in (-1: none)
    int biome = 0;          // that zone's biome (EBiomeType; Default for none)
    int actor_node = -1;    // the node of the expression's own actor
    int player_node = -1;   // the node of the player (GetPlayer())
};

/// Answers a node read (stats/stat_program.h kHostNodeBiome .. kHostNodeLast) with `args` (n of them); false when `fn` is no node read.  `nodes` may be
/// null (a registry with no nodes).  The citizen and mayor reads answer 0 until the citizenship milestone.
bool node_host_fn(const CityNodes* nodes, const NodeContext& ctx, int fn, const double* args, int n, double& out);

}  // namespace aoc::worldd::world
