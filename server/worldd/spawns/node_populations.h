// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  spawns/node_populations.h  (Nodes 1, stage 3b)
//
//  The live population manager: the town's people follow the node's level.  Each city node inside the area gets the NPCs spawns/node_population.h selects for its level; when the
//  level changes (a GM's SetNodeLevel through citynode, WatchNodes, CityNodes::version()) the manager selects again and makes the difference real: the NPCs the new level no
//  longer has leave (the registry removes them: every connection that holds one closes its channel), the ones it adds arrive, and the ones both levels have stay where they are.
//
//  Identity.  A point of a placed set has an identity (NodeSpawn::ident: its set, its placement and its key); an NPC stays across a level change when the new selection has the same
//  identity with the same Blueprint standing in the same place.  A different Blueprint for the same point (the filter judged another asset) or another place replaces the NPC.
//
//  No races.  The world runs on one thread: tick_world drives the ground's window (the chunks that arrived ground the NPCs waiting for them), THEN this manager, then the NPC world's step,
//  the fights and the corpses.  The manager only touches the registry and calls two hooks:
//    spawned(id, instance, route)   the world puts the new actor under the NPC world's care (npc::NpcWorld::add: an NPC whose chunk of the ground is not resident stands on the terrain
//                                   and waits for it, pending, exactly as the area's NPCs do - so a town far from every player is grounded when the first player comes near).  A node's NPC keeps
//                                   the height the data gave it (NpcInfo::ground_by_data, set by make): the baked terrain under a city node's bowl is 50 m below the town's floor, so it
//                                   waits at that height, and is stood on a surface only when one is within 15 m of it
//    despawned(id)                  the fights forget the actor (combat::CombatWorld::forget); the NPC world forgets it by itself (the registry's removal listener), also an NPC that
//                                   is still waiting for its chunk
//  A corpse that is still lying there when its node's people change is removed with the rest; the aftermath's closing of a corpse that is gone is harmless.
//
//  Respawns.  An NPC the manager made carries a spawn reference in the node range (kNodeRefBase and above); when the aftermath's timer fires, respawn() makes the NPC again from the
//  node's CURRENT selection: the point is still there (the same Blueprint, which may differ from the NPC that died if the level changed in between: then the dead one was replaced at
//  the change and its reference closed), or it is gone and nothing comes back.
//
//  Out of scope here (later node milestones): a building's state changing (Disarray, Damaged, Demolition ...), a mayor's buildings, a plot that is built after the start.
// ============================================================================
#pragma once

#include "spawns/area.h"
#include "spawns/area_loader.h"
#include "spawns/node_population.h"
#include "world/actor_registry.h"
#include "world/city_nodes.h"
#include "world/node_wire.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace aoc::worldd::spawns {

/// NpcInfo::spawn_ref of an NPC the manager made: this plus its slot.  (The area's references are 1-based indices of AreaReport::recipes, far below.)
inline constexpr uint32_t kNodeRefBase = 0x80000000u;

class NodePopulations {
public:
    struct Config {
        bool enabled = true;
        /// Blueprints (leaf names) whose NPC a retail capture opened and whose body needs an NPCAppearanceComponent: the open carries one, so they are shown (see node_population.h).
        std::set<std::string> shown_humanoids;
    };
    struct Hooks {
        std::function<void(uint64_t actor, const content::NpcInstanceRow* instance, const content::NpcRouteRow* route)> spawned;
        std::function<void(uint64_t actor)> despawned;
    };

    /// All the pointers must outlive the manager.  `population` is the chain the area was selected from (AreaReport::data), `base` the world's spawn context (the filter's switches, the hour,
    /// the humanoids), `motion` the Blueprints' visuals (may be null).
    NodePopulations(world::ActorRegistry& registry, const world::CityNodes& nodes, const world::NodeWire& wire, const NodePopulationData& data,
                    std::shared_ptr<const content::PopulationData> population, const TemplateSource& templates, SpawnContext base, const npc::MotionTable* motion,
                    const AreaConfig& area, Minter mint, uint32_t server_id, Hooks hooks, Config config);

    /// Makes the people of every node inside the area at its level now, once.  Returns the NPCs made; each node is logged.
    size_t start(double now);

    /// Call every world tick (after the ground window): follows the registry's version().  Returns the nodes it rebuilt (cheap when nothing changed).
    size_t tick(double now);

    /// The aftermath's respawn of an NPC the manager made (`ref` >= kNodeRefBase): the new actor's id, 0 when the point is gone or the reference is closed.
    uint64_t respawn(uint32_t ref, double now);
    static bool is_node_ref(uint32_t ref) { return ref >= kNodeRefBase; }
// (C:Y:B : CoA)

    struct Entry {
        int64_t  node = 0;
        int      level = -1;           // the level the people show (-1: never built)
        size_t   npcs = 0;             // NPCs that exist now (alive or a corpse that is lying there)
        size_t   desired = 0;          // the points the selection has
        size_t   by_source[2] = {0, 0};   // desired, layout and building
    };
    const std::map<int64_t, Entry>& entries() const { return entries_; }
    const Entry* entry_of(int64_t node) const;
    /// NPCs of the node that exist now (their actor ids).
    std::vector<uint64_t> actors_of(int64_t node) const;
    /// The nodes' NPCs made in all, removed in all, respawned, and the rebuilds done after the start.
    uint64_t made() const { return made_; }
    uint64_t removed() const { return removed_; }
    uint64_t respawned() const { return respawned_; }
    uint64_t rebuilds() const { return rebuilds_; }
    size_t   nodes() const { return entries_.size(); }
    const Config& config() const { return config_; }
    /// The spawn context a node is judged in at its level now (for the tests and the log).
    SpawnContext context_of(const world::CityNodeInfo& info, int level) const;

private:
    struct Slot {
        int64_t  node = 0;
        uint64_t ident = 0;
        bool     open = true;     // false: the point was replaced or is gone; a respawn that names it does nothing
    };
    struct Live {
        NodeSpawn spawn;           // the point as the current selection has it
        uint64_t  actor = 0;       // its NPC (0: none now)
        uint32_t  slot = 0;        // its reference's slot
    };
    struct State {
        std::map<uint64_t, Live> live;   // by identity
    };

    NodeSite site_of(const world::CityNodeInfo& info, int level) const;
    bool     in_area(const world::CityNodeInfo& info) const;
    /// Selects the people of `info` at `level` and makes the registry match; returns the NPCs made.
    size_t   apply(const world::CityNodeInfo& info, int level, double now, bool first);
    uint64_t make(Live& l, double now, const world::CityNodeInfo& info);
    void     remove(uint64_t actor);
    static bool same_point(const NodeSpawn& a, const NodeSpawn& b);

    world::ActorRegistry&                       registry_;
    const world::CityNodes&                     nodes_;
    const world::NodeWire&                      wire_;
    const NodePopulationData&                   data_;
    std::shared_ptr<const content::PopulationData> population_;
    TemplateSource                              templates_;
    SpawnContext                                base_;
    const npc::MotionTable*                     motion_;
    AreaConfig                                  area_;
    Minter                                      mint_;
    uint32_t                                    server_id_;
    Hooks                                       hooks_;
    Config                                      config_;
    std::map<int64_t, Entry>                    entries_;     // by node guid
    std::map<int64_t, State>                    states_;
    std::vector<Slot>                           slots_;
    uint64_t                                    seen_version_ = 0;
    uint64_t                                    made_ = 0, removed_ = 0, respawned_ = 0, rebuilds_ = 0;
    bool                                        started_ = false;
};

}  // namespace aoc::worldd::spawns
