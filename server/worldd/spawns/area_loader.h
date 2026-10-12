// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  spawns/area_loader.h  (plan E1, stage 1)
//
//  Fills the world's ActorRegistry with the living area around the login spot, from the three sources the spec names:
//
//    data       content.db's population chain (spawns/population.h): the NPCs the game's own placements put within the area (and the random
//               points of its PopulationAreas), each from the retail template of its archetype (a capture opened that archetype) or, when no
//               capture did, from the generic template of its Blueprint (spawns/generic_npc.h);
//    captured   the town and node NPCs and the resource nodes the retail captures saw within the area (spawns/captured_spawns.h), the
//               open the retail server sent with its position.  A captured NPC that stands where a data NPC of the same archetype stands
//               (within kSamePointCm) is the same spawn seen twice: kept once, the data's first.  Gatherables are visible only (stage 1).
//    runtime    the GM vendors (vendor/gm_vendor.h): one NPC per PTR vendor def in two arcs in front of the login spot, from the template
//               of the archetype retail uses for its vendors (General_Gear_Base_Vendor).
//
//  Pure setup: nothing here sends anything or reads a clock; the registry is the only thing changed.
// ============================================================================
#pragma once

#include "content/content_db.h"
#include "spawns/area.h"
#include "spawns/captured_spawns.h"
#include "spawns/population.h"
#include "vendor/gm_vendor.h"
#include "world/actor_registry.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace aoc::worldd::spawns {

/// A captured NPC within this distance (cm, horizontal) of a data NPC of the same archetype is that spawn seen twice.
inline constexpr double kSamePointCm = 300.0;
/// The archetype retail's vendors use: the GM NPCs are made from its open.
inline constexpr const char* kGmVendorArchetype =
    "/Game/DesignAssets/Population/NPC/PCRaces/Node/Vendors/General_Gear_Base_Vendor.Default__General_Gear_Base_Vendor_C";

struct AreaSources {
    const content::ContentDb*    db = nullptr;
    const CapturedData*          captured = nullptr;
    const vendor::GmVendors*     gm = nullptr;     // null: no GM vendors
    const GenericNpcs*           generic = nullptr;   // null: only the Blueprints a capture opened
    const SpawnFilter*           filter = nullptr;    // null: stage 2's behaviour, every point whose Blueprint opens (plan E1, stage 3: spawns/spawn_filter.h)
};

/// What a respawn makes a spawn point's NPC again from (plan E1, stage 3, part 2): the open its Blueprint came from, the instance parameters and the
/// NpcInfo the loader gave it (its controller and interaction component are the new actor's own).  Held by the report; `NpcInfo::spawn_ref` is the index + 1.
struct SpawnRecipe {
    const OpenTemplate* open = nullptr;
    InstanceParams      params;
    world::NpcInfo      info;
};
// ~ C.Y.B & CoA ~

/// What the load did, for the log and the report.
struct AreaReport {
    AreaConfig                    area;
    PopulationStats               population;
    size_t                        data_npcs = 0;
    size_t                        captured_npcs = 0;
    size_t                        gatherables = 0;
    size_t                        gm_vendors = 0;
    size_t                        duplicates = 0;        // captured NPCs dropped for a data NPC at the same point
    size_t                        gatherable_duplicates = 0;
    size_t                        refused = 0;           // spawns that did not instantiate (their `error` is in `notes`)
    std::map<std::string, size_t> npcs_by_archetype;     // leaf name, all sources
    /// Stage 2: the rows the data NPCs' lives come from (how a PopulationInstance's NPC wanders or patrols): the instances that spawned something
    /// and the routes they walk.
    std::map<int64_t, content::NpcInstanceRow> instances;
    std::map<int64_t, content::NpcRouteRow>    routes;
    /// Plan E1, stage 3, part 2: the recipe of every data NPC that respawns (its spawn point: where, what, at which level).
    std::vector<SpawnRecipe>                   recipes;
    /// Plan E1, stage 3, part 2: the respawn shrines within the area (nearest the centre first): where a dead player may come back.
    std::vector<RespawnPoint>                  respawn_points;
    /// Nodes 1, stage 3b: the population chain the area was selected from (the node populations select from it too, and an NPC's life reads its instance's and route's rows).
    std::shared_ptr<const content::PopulationData> data;
    std::vector<std::string>      notes;
    size_t total() const { return data_npcs + captured_npcs + gatherables + gm_vendors; }
    /// A few lines for the log.
    std::string describe() const;
};

/// A data NPC made an actor: the open instantiated with the spawn's place, level and population, and the NpcInfo the area's NPCs carry (home, aggro and leash radii, the loot tables its
/// asset and instance name, how it respawns).  The loader's and the node populations' (spawns/node_populations.h) one way to make one.  `params` (optional) gets the instance parameters used.
/// nullopt, with `error`, when the open refuses a value.
std::optional<world::WorldActor> make_data_actor(const DataSpawn& spawn, const Minter& mint, uint32_t server_id, double world_time, InstanceParams* params, std::string& error);

/// Fills `registry`. nullopt, with `error` set, when a source cannot be read at all (the population chain). `server_id` is the ServerId the
/// actors carry (AuthServerIDReplicated); `world_time` seconds is the timestamp their movement starts at.
std::optional<AreaReport> load_area(world::ActorRegistry& registry, const AreaSources& sources, const AreaConfig& area, const Minter& mint, uint32_t server_id,
                                    double world_time, std::string& error);

}  // namespace aoc::worldd::spawns
