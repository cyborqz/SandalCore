// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/actor_registry.h  (plan E1, stage 1)
//
//  The world's other actors: the NPCs, the resource nodes and a player's lingering effects, held once for the whole worldd, each as the
//  replicated state a connection opens (repl::Actor: its values, its subobjects, what SerializeNewActor says) plus what the server itself
//  knows of it (its kind, where it stands, who it belongs to).  The per-connection side is session/actor_view.h: which of these a client
//  has opened, on which channel, and what it has been sent.
//
//  An actor's NetGUID is the registry's key (its ObjectId, even, minted from worldd's allocator in the retail pattern: the actor, the AI
//  controller object its Owner and Controller name at +2, then the components; research/actors/evidence.md 1.1).  Nothing here sends
//  anything and nothing here reads a clock.
//
//  Change tracking.  An actor carries two counters.  `version` moves on every change of what a connection is sent (its replicated values and,
//  for a connection that has it open, nothing else) and `reliable_version` only on a change that must not be lost; a change of position alone
//  (`Change::Movement`, stage 2's walking NPCs) moves `version` only, so the view sends it unreliably, as the retail server sends
//  ReplicatedMovement every 67 ms.  The view compares both with what it last sent.  `modify` runs a function on the actor and bumps the
//  counters; changing `actor` any other way would go unnoticed.
//
//  Space.  A uniform grid of square cells (kCellCm, 100 m: retail's own cell for relevancy) keeps the actors by their horizontal position;
//  `query` returns the ones within a radius of a point.  Height does not take part: retail's relevancy is horizontal (evidence 3).
//
//  Removal.  `remove` calls the registered listeners with the actor before it goes: a connection that has the actor open closes its channel
//  (Destroyed), one that holds it dormant-visible opens and closes it so the client forgets it.
// ============================================================================
#pragma once

#include "protocol/repl/replicator.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aoc::worldd::world {

namespace repl = ::aoc::protocol::repl;

enum class ActorKind : uint8_t {
    Npc,         // ANPCCharacter: a creature, a villager, a vendor, a service building
    Gatherable,  // AGatherableActor: a resource node (visible only in stage 1)
    Effect,      // a player's lingering zone (AAoCAbilityLingeringEffect), D2's dance effect
    Pile,        // a dead player's loot pile (APlayerLootPile; plan E1, stage 3, part 2)
    Mount,       // a summoned mount (the mounts milestone, stage 2): an ANPCCharacter-layout actor with an owner, never dormant
    NodeLayout,  // a town's ANodeLayoutReplicator (Nodes 1, stage 3a): the scenery of a city node, awake within 1.45 km, never dormant
    Foliage,     // a foliage cell's AIntrepidStateIFA (Gathering stage 3, "Foliage gathering"): which instances of the cell are cut or growing; opened as a snapshot, never awake
};
// [C | Y | B ~ C-O-A]

/// Where an NPC or node came from (the report counts them).
enum class Source : uint8_t { Data, Captured, Runtime };

/// What changed on an actor, for the view's choice of reliable or not.
enum class Change : uint8_t {
    Movement,   // its position (ReplicatedMovement and its timestamp): unreliable is enough, the next one repeats it
    State,      // anything else that is replicated: reliable
};

/// What an NPC is to the server (the AI of stage 3 drives these; stage 1 only reads them).
struct NpcInfo {
    std::string name;                // the display name the client shows (UNPCInfo.CreatureOwnerName)
    int         level = 0;
    std::string archetype;           // the archetype's path
    Source      source = Source::Data;
    int64_t     asset_guid = 0;      // PopulationAsset, PopulationInstance (0 for a captured one)
    int64_t     instance_guid = 0;
    int64_t     vendor_guid = 0;     // VendorDef the NPC sells from (0: none)
    double      home_x = 0, home_y = 0, home_z = 0;   // where it spawned: it returns here (stage 2, 3)
    double      respawn_seconds = 0;
    double      respawn_variance = 0;
    double      aggro_radius_cm = 0;     // the instance's override or the asset's AggroRadius (0: the Blueprint's, which is not in the data: the AI's default)
    /// Retest 3 (Timid Grems): the instance overrides the aggro radius WITH 0 (bOverrideAggroRadius on, AggroRadius 0: all 31 instances of the Pop_TimidGrem set, and the Timid Raven, Spider
    /// and Deer sets), which is the data saying "never notices a player on its own"; unlike an instance with no override, whose 0 means "the Blueprint's radius" (the AI's default).  It still
    /// fights back what hurts it.
    bool        aggro_off = false;
    double      leash_radius_cm = 0;     // the asset's LeashRadius (0 for every NPC type of the area)
    uint64_t    controller = 0;      // the AI controller object's ObjectId (the actor's Owner and Controller name it; it has no channel)
    uint64_t    interact_component = 0;   // the ObjectId of its InteractableComponent (0: none), what a client's interaction names
    /// Gathering stage 3, "Hunting": the PlayerInteractionDef its InteractableComponent's first input names (the block's InteractionInputData[0].PlayerInteractDefinition; 0: none).  For a
    /// huntable creature it is the Hunting_<kind> def, which names the gathering interaction its corpse is harvested with (gather/gathering.h hunt_kind); for a vendor it is the vendor's.
    int64_t     interact_def = 0;
    /// E1 stage 3 fix 3: the ObjectId of its CollisionCylinder (0: none), minted INSIDE the actor's own block (the actor's + 4, retail's slot: the
    /// AlignmentComponent's - 2 in 211 of 211 captured exports). The hit confirmations and the ability items name it and export it as the actor's
    /// "CollisionCylinder". A GUID outside the block (the draft's + 24) was the next-minted object's (the next NPC's AlignmentComponent, a dance's
    /// LingeringEffectMovementComp): the client, holding that GUID under another path and outer, dropped the connection (NetGuidMismatch).
    uint64_t    collision_cylinder = 0;
    // plan E1, stage 3, part 2: what its death does (combat/aftermath.h)
    std::vector<int64_t> loot_tables;   // the RewardTables its asset and instance name (its Blueprint's come from the motion table)
    std::string respawn_rule;           // the instance's: Refill, AllDead, NoRespawn
    uint32_t    spawn_ref = 0;          // 1-based index of the spawn point's recipe (spawns::AreaReport::recipes): what a respawn is made from (0: none)
    bool        is_gm_vendor = false;
    bool        ground_by_terrain = false;   // its height is not known (a PopulationArea's random point): the ground under it decides
    bool        ground_by_data = false;      // its height is known and right (a city node's people: retail's z to 1-168 cm) while the baked terrain under a bowl is not: the data's height
                                             // stands unless a surface of the ground is within reach of it (NpcWorld never moves it to the terrain)
};

/// What a town's replicator (ActorKind::NodeLayout) is to the server: which node it shows, at which level, and what its LayoutAssetSetGuids holds (the log and the tests).
struct SceneryInfo {
    int64_t     node = 0;            // CityNodeRecordBase
    std::string name;                // the node's display name
    int         level = 0;           // the level the payload shows
    size_t      items = 0;           // items in the payload
    uint8_t     plot_version = 0;    // PlotChangeVersion
    size_t      payload_bits = 0;    // the field payload as an open sends it
};

/// Gathering stage 2, "Harvesting": what the harvesting player's own connection sees of a resource node that the shared state cannot say (an object reference to a pawn that only that
/// player's client has).  The node's InteractableComponent holds InteractingPlayer as none; the actor view of the player `player_id` shows `pawn` in it while `show` is set
/// (gather/node_wire.h).  Every other connection sees none, as the retail captures' second observer does.
struct GatherSeen {
    uint64_t                         player_id = 0;   // the PlayerId of the player who harvests (or harvested) the node
    protocol::emit::FIntrepidNetworkGUID pawn;        // ... and its pawn
    bool                             show = false;
};

struct WorldActor {
    uint64_t   id = 0;                    // = actor.guid.ObjectId
    ActorKind  kind = ActorKind::Npc;
    repl::Actor actor;                    // the replicated state
    double     x = 0, y = 0, z = 0;       // cm
    double     yaw_degrees = 0;
    double     awake_radius_cm = 0;       // 0: the relevancy config's (retail: some large creatures 119 m, the rest 150 m)
    bool       never_dormant = false;     // players, mounts and a player's effects are never dormancy-closed (evidence 4)
    /// Gathering stage 3 (a foliage cell's state actor): retail opens it when something in the cell changes and closes it with Dormancy about 265 ms later [R: capture 1463's open
    /// to its close], whatever the distance: it is never awake.  The view puts it in the snapshot ring wherever it is in range, closes its snapshots `dormancy_delay` after the open
    /// (0: the relevancy config's 134 ms), keeps the shadows of its fast arrays between opens (`keep_fast_shadow`: UE keeps a dormant replicator's state, and the client's list of
    /// cut instances has to be deleted from, not rebuilt) and shows a connection that never had it `first_open_form` of it instead.
    bool       snapshot_only = false;
    std::string label;                    // a short name for the log (a foliage cell's owner)
    std::chrono::milliseconds dormancy_delay{0};
    bool       keep_fast_shadow = false;
    std::function<void(repl::Actor&)> first_open_form;
    uint64_t   owner = 0;                 // the PlayerId an effect belongs to (0: nobody's): only that player's connection is sent it
    /// Exports that go with the actor block of its open (an object its values name by path that the client has no other way to find: a lingering
    /// zone's AttachComponent, the pawn's CharacterMesh0).
    std::vector<repl::ExportChain> open_exports;
    uint32_t   version = 1;               // every change of the replicated state
    uint32_t   reliable_version = 1;      // the changes that must not be lost
    NpcInfo    npc;
    SceneryInfo scenery;                  // ActorKind::NodeLayout only
    GatherSeen gather;                    // ActorKind::Gatherable only (gather/node_wire.h)
};

struct RegistryConfig {
    /// The cell of the grid in cm: retail's relevancy used 100 m.
    double cell_cm = 10000.0;
};

class ActorRegistry {
public:
    explicit ActorRegistry(RegistryConfig config = {}) : config_(config) {}

    /// Adds an actor (its `id` must be its actor.guid.ObjectId, even, and not held). False for a duplicate id, an odd one or a position that is
    /// not a number. The grid takes it by its position.
    bool add(WorldActor actor);

    /// Removes an actor, telling the listeners first. False when there is none.
    bool remove(uint64_t id);

    WorldActor*       find(uint64_t id);
    const WorldActor* find(uint64_t id) const;
    /// The actor that owns the component whose ObjectId is `component` (NpcInfo::interact_component or collision_cylinder); nullptr when none does.
    const WorldActor* find_by_component(uint64_t component) const;
    size_t            size() const { return actors_.size(); }
    size_t            count(ActorKind kind) const;

    /// Runs `fn(WorldActor&)` on the actor and counts the change: the version always, the reliable version unless `kind` is Movement. False for
    /// an actor that is not held (`fn` is not run).
    template <class F>
    bool modify(uint64_t id, Change kind, F&& fn) {
        WorldActor* a = find(id);
        if (!a) return false;
        fn(*a);
        touch(*a, kind);
        return true;
    }

    /// Names the InteractableComponent a client's interaction will name for the actor (a corpse's, made at its death): the index `find_by_component`
    /// answers from follows; the actor's version is not touched (the caller changed the replicated state itself).  False for an unknown actor, an actor
    /// that has the component already, or a component another actor owns.
    bool set_interact_component(uint64_t id, uint64_t component);

    /// Moves an actor (the grid follows); counts a Movement change. False for an unknown actor or a position that is not a number.
    bool move(uint64_t id, double x, double y, double z, double yaw_degrees);

    /// Counts a change of `a` made outside modify (the registry's own bookkeeping aside, nothing else bumps the counters).
    void touch(WorldActor& a, Change kind);

    /// The ids of the actors within `radius_cm` (horizontal) of (x, y), in no particular order, appended to `out`.
    void query(double x, double y, double radius_cm, std::vector<uint64_t>& out) const;
// C Y B captures, CoA code

    /// An actor whose awake radius is more than this is a WIDE one (a town's replicator: 1.45 km): `query` would have to visit a quarter of the world's cells to find it, so the
    /// registry keeps the wide actors apart and `query_wide` finds them.
    static constexpr double kWideCm = 40000.0;
    /// The wide actors within their own awake radius plus `margin_cm` of (x, y), appended to `out`.  A view calls it beside `query`; the two may name an actor twice.
    void query_wide(double x, double y, double margin_cm, std::vector<uint64_t>& out) const;
    size_t wide_count() const { return wide_.size(); }

    /// Calls `fn` for every actor (no particular order).
    template <class F>
    void for_each(F&& fn) const {
        for (const auto& [id, a] : actors_) fn(a);
    }

    using Listener = std::function<void(const WorldActor&)>;
    /// Registers a function called with an actor just before it is removed. The returned handle takes it off again.
    uint32_t on_remove(Listener l);
    void     off_remove(uint32_t handle);

    double cell_cm() const { return config_.cell_cm; }

private:
    static int64_t cell_key(int32_t cx, int32_t cy) { return (static_cast<int64_t>(cx) << 32) ^ static_cast<uint32_t>(cy); }
    int32_t        cell_of(double v) const;
    void           grid_add(const WorldActor& a);
    void           grid_remove(const WorldActor& a);

    RegistryConfig                                config_;
    std::unordered_map<uint64_t, WorldActor>      actors_;
    std::unordered_map<uint64_t, uint64_t>        by_component_;   // InteractableComponent ObjectId -> its actor
    std::unordered_map<int64_t, std::vector<uint64_t>> grid_;
    std::vector<uint64_t>                         wide_;
    std::vector<std::pair<uint32_t, Listener>>    listeners_;
    uint32_t                                      next_listener_ = 1;
};

}  // namespace aoc::worldd::world
