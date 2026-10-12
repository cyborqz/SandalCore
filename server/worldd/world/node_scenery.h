// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/node_scenery.h  (Nodes 1, stage 3a)
//
//  The towns' scenery: one ANodeLayoutReplicator per city node, held in the world's actor registry (world/actor_registry.h, ActorKind::NodeLayout), so every connection's
//  ActorView opens it, updates it and closes it with the rest of the world.  What the client draws of a town (its buildings, walls, props) is what this actor's
//  LayoutAssetSetGuids lists (protocol/repl/node_layout.h; the items come from world/node_layout_data.h); the client loads the meshes itself.
//
//  The actor.  One per node that has a layout and items at its level: the node's place is the open's location (the layout's translations are offsets from the ACTOR), its
//  yaw the node record's.  Retail opens it for a player within 1.19 to 1.45 km and keeps it while the player is near: the awake radius is 1.45 km (Config::awake_cm), the actor is
//  never dormancy-closed, and the registry keeps it apart from the grid (ActorRegistry::kWideCm, query_wide) so that finding it costs nothing.  The properties after the open
//  are retail's: AuthServerIDReplicated, ReplicatedMovement (the place), RemoteRole 1, Role 4, NodeGuid (the client waits for it to be non-zero) and the fast array.
//
//  Where it stands.  Retail's open location is 6 to 40 m from the node record's location with no constant offset (the layout origin is an actor of the map, not the record's
//  place); node_origins.h has the 13 nodes the captures show, and those are used (Config::captured_origins) so that the scenery lines up with the ground and the roads; every
//  other node stands at its record's location.  `--node-origin record` puts all of them at the record's (to judge the difference in game).
//
//  Changes.  tick() compares the registry's version() with the last it saw; a node whose level changed gets a NEW FULL PAYLOAD (node_layout.h sync_node_layout: the old items
//  removed, the new list added, the header's level new) and PlotChangeVersion + 1, as one change of the actor, which every connection that holds it open is sent as an
//  update.  A node that has no item at the new level loses its actor (the clients close it); one that gets items again gets a new one.
//
//  Culture Kaelar and one fixed season (Spring) for all: every retail payload is Kaelar; retail flips Winter and Spring across re-opens, unexplained.
// ============================================================================
#pragma once

#include "protocol/emit/intrepid_netguid.h"
#include "world/actor_registry.h"
#include "world/city_nodes.h"
#include "world/node_layout_data.h"
#include "world/node_wire.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
// [C-O-A + C-Y-B]

namespace aoc::worldd::world {

class NodeScenery {
public:
    struct Config {
        bool    enabled = true;
        double  awake_cm = 145000.0;        // retail: opens seen at 1.19 to 1.45 km
        bool    captured_origins = true;    // the open locations of the 13 captured nodes (node_origins.h); false: every node at its record's location
        uint8_t culture = 0;                // Kaelar
        uint8_t season = kSpring;
    };
    using Minter = std::function<protocol::emit::FIntrepidNetworkGUID()>;

    /// All four must outlive the scenery.  `mint` gives the actors' NetGUIDs (worldd's allocator).
    NodeScenery(ActorRegistry& registry, const CityNodes& nodes, const NodeWire& wire, const NodeLayoutData& data, Minter mint, Config config);

    /// Makes the replicator of every node that has items at its level now.  Returns how many; each is logged.  Once.
    size_t start();

    /// Call every world tick: rebuilds the nodes whose level changed since the last call (cheap when nothing did).  Returns how many it rebuilt.
    size_t tick();

    struct Entry {
        int64_t  node = 0;
        int      rank = -1;
        uint64_t actor = 0;       // the registry's id (0: none now)
        int      level = -1;      // the level the payload shows (-1: never built)
        uint8_t  plot_version = 0;
        size_t   items = 0;
        double   x = 0, y = 0, z = 0, yaw = 0;
        bool     captured_origin = false;
    };
    const std::map<int64_t, Entry>& entries() const { return entries_; }
    const Entry* entry_of(int64_t node) const;
    size_t   actors() const;                  // replicators in the registry now
    uint64_t rebuilds() const { return rebuilds_; }
    const Config& config() const { return config_; }

    /// The bits of a node's whole payload (a full snapshot) as it is now; 0 for a node with no actor.
    size_t payload_bits(int64_t node) const;

private:
    /// The items and header of `node` at `level` with `plot_version`.
    struct Built {
        repl::NodeLayoutHeader        header;
        std::vector<repl::NodeLayoutItem> items;
    };
    Built build(const CityNodeInfo& info, int level, uint8_t plot_version) const;
    bool  place(Entry& e, const CityNodeInfo& info) const;
    bool  make_actor(Entry& e, const CityNodeInfo& info, const Built& b);
    bool  refresh(Entry& e, const CityNodeInfo& info, int level);
    static uint8_t next_version(uint8_t v) { return v == 255 ? 1 : static_cast<uint8_t>(v + 1); }

    ActorRegistry&        registry_;
    const CityNodes&      nodes_;
    const NodeWire&       wire_;
    const NodeLayoutData& data_;
    Minter                mint_;
    Config                config_;
    std::map<int64_t, Entry> entries_;       // by node guid
    uint64_t              seen_version_ = 0;
    uint64_t              rebuilds_ = 0;
    bool                  started_ = false;
};

}  // namespace aoc::worldd::world
