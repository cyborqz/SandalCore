// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  npc/area_npcs.h  (plan E1, stage 2)
//
//  Gives the NPCs of the loaded area their lives: for each NPC of the registry, its place on the ground, how it moves and where it goes, from
//  what the data says (content.db's PopulationInstance: the ambient behaviour, the radii and delays, the patrol route) and what the Blueprint says
//  (npc_motion.tsv: the capsule and the speeds).
//
//    standing     the NPC's origin is its feet's ground plus the capsule's half height times the actor's scale (a wolf 96 cm, a player 98).  A data NPC,
//                 a captured one and a GM vendor all go through it: the nearest surface to the height they had within 15 m is the ground; where there
//                 is none (outside the extracted terrain, over a hole) the NPC keeps its height and stands for good.
//    behaviour    Stationary, Wander and Default (= Wander) and Patrol instances; a captured NPC and a GM vendor have no instance and stand.
//    speeds       (modelled, from the retail captures' ReplicatedMovement velocities of 16 captures' NPCs matched to their data points: a human
//                 Wander walks at 126 to 162 cm/s of a Blueprint MaxWalkSpeed of 600 (0.21 to 0.27), a bear 216 of 800 (0.27), Patrol with a
//                 SpeedMultiplierExpression of 0.15 walks 94 to 95 (0.15 x 600 x 1.05), a patrol without one 243 (0.4)):
//                     wander   MaxWalkSpeed x 0.27
//                     patrol   MaxWalkSpeed x the instance's expression when it is a number (x 1.05), else x 0.4
//                     return   MaxRunSpeed x 0.5
//
//  A route's waypoints are already placed (contentc's `waypoints` are the route component's transform applied to its `points`: 911 of the 915
//  routes that carry both say so, and the retail NPCs of the captures walk them as they are, to 30 to 63 cm).  A route placed by a city node is
//  relative to the node's record: a town person's is placed there (place_node_route); one placed by an encounter is relative to something worldd does not place, and a Patrol instance with no route or with
//  fewer than two waypoints (the 17 of the area: bandit-attack wolves, ravens, a guard) wanders like the others.  A Stationary instance stands: the
//  retail NPCs of those stand too (215 observed epochs of Stationary instances, 15 moved, all scripted).
// ============================================================================
#pragma once

#include "npc/motion_table.h"
#include "npc/npc_world.h"
#include "spawns/area_loader.h"
#include "world/city_nodes.h"

#include <array>
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace aoc::worldd::npc {

struct GroundReport {
    size_t npcs = 0;                 // NPCs under the NpcWorld's care
    size_t on_ground = 0;            // stood on a ground
    size_t no_ground = 0;            // no ground within reach: left at their height
    size_t pending_ground = 0;       // their chunk of the ground is not loaded (a chunked file's window): they stand on the terrain and are grounded when a player comes near
    size_t stationary = 0;
    size_t wander = 0;
    size_t patrol = 0;
    size_t patrol_unwalked = 0;      // a patrol with no usable route: it wanders
    size_t default_motion = 0;       // NPCs whose archetype has no line in the table (the NPCPawn defaults)
    size_t scaled = 0;               // NPCs whose open carries a scale other than 1
    double largest_snap_cm = 0;      // the biggest change of height the standing made
    size_t moved_by_more_than_1m = 0;
    /// By archetype (the Blueprint's leaf, "Goblin_Gravehide_C" and so on): how many NPCs stand, wander and patrol.  No NPC flies: the retail captures show
    /// every creature (the hawks and ravens too) in NavWalking mode 10 with no vertical velocity, its feet on the ground.
    struct TypeCount {
        size_t total = 0, stand = 0, wander = 0, patrol = 0;
        size_t stand_by_data = 0;   // of `stand`: the instance says Stationary
        size_t stand_no_life = 0;   // of `stand`: no instance (a captured NPC, a GM vendor) or no ground
    };
    std::map<std::string, TypeCount> by_type;
    size_t fly = 0;
    size_t stations = 0;             // crafting, step 0: stations the area's data places (a workshop outside the nodes), put on the ground; not counted among the NPCs
    std::string describe() const;
    /// The per-type counts as log lines (`per_line` types a line), most numerous first.
    std::vector<std::string> describe_types(size_t per_line = 12) const;
};
// [C:Y:B - CoA]

/// The Blueprint-speed factors above, in one place.
struct SpeedModel {
    double wander_factor = 0.27;
    double patrol_default_factor = 0.4;
    double patrol_expression_gain = 1.05;
    /// A guard's patrol speed is MaxWalkSpeed x the expression with no gain (the retail guards walk it as it is: the 1.05 is the villagers' and animals').
    double guard_expression_gain = 1.00;
    double return_factor = 0.5;
    /// Gathering stage 3, "Hunting": the PlayerInteractionDefs of the creatures that always stand (the huntable ones, gather/gathering.h hunting_defs).  A huntable creature is interacted with while
    /// it stands still (the client refuses the hunt of one that walks: "can't tame right now"), so its instance's Wander or Patrol is not lived.  Null: none.
    const std::set<int64_t>* standing_defs = nullptr;
};

struct RetailMotion;
/// What the retail captures show of an archetype's walking (npc/retail_motion.h), null when no capture showed it.
const RetailMotion* retail_motion(const std::string& archetype_path);

/// The leaf of an NPC's archetype path ("Goblin_Gravehide_C").
std::string archetype_leaf(const std::string& archetype_path);

/// True for the Blueprints that stand at their post whatever their instance says (by kind: only 13 of the 686 Default service instances carry a vendor of their own): the vendors (`*Vendor*`), the
/// clerks and managers (`*Clerk*`, `*Manager*`, the `*_KAE` service NPCs, `Node_Warehouse_*`, `Node_Mayor*`, `Node_TownHall_*`) and the stations and boards (`CraftingStation_*`, `ProcessingStation_*`,
/// `CommissionBoard_*`, turn-in boxes and mailboxes).  Judged by the Blueprint's leaf (archetype_leaf).
bool stands_by_kind(const std::string& leaf);

/// A workshop worker wanders only within `radius_cm` of the place it was put at (its workshop): a few metres, never across the town.  Only a wandering setup changes.
void keep_to_workshop(NpcMotionSetup* s, double radius_cm);
/// How far a workshop worker strolls from its workshop (cm).
constexpr double kWorkshopRadiusCm = 600.0;

/// True for a guard's Blueprint (`Guard_Soldier`, `Guard_Wizard`).
bool is_guard_leaf(const std::string& leaf);

/// True for a creature of the data's Beast family (an archetype path under /NPC/Beast/: wolves, rabids, ravens, pigs ...), never a person (/NPC/PCRaces/, /NPC/Humanoid/) or a monster.
bool is_beast_archetype(const std::string& archetype_path);

/// True for a beast that flies (a bird, a bat, a gryphon, an elder dragon, an insect cloud): `NpcInfo::flies`.  A rule that stands creatures on the ground only lifts such a creature out of the ground.
bool is_flying_beast_archetype(const std::string& archetype_path);

/// The z scale an actor's open carries (1 when it has none).
double actor_scale_z(const world::WorldActor& a);

/// Where a node-relative route stands in the world: the node's record and its yaw (world = record + R(yaw) (wx, wy), z = record z + wz).
void place_node_route(const world::CityNodeInfo& node, double wx, double wy, double wz, double* x, double* y, double* z);

/// Makes the motion setup of one NPC (without placing it).  `node` (optional) is the city node a town person belongs to: a patrol route placed by a city node is walked at that node's record.
NpcMotionSetup make_setup(const world::WorldActor& a, const MotionTable& table, const content::NpcInstanceRow* instance, const content::NpcRouteRow* route, const SpeedModel& speeds,
                          bool* default_motion = nullptr, const world::CityNodeInfo* node = nullptr);

/// Puts every NPC of the registry on the ground and starts its behaviour.
GroundReport populate(world::ActorRegistry& registry, NpcWorld& world, const MotionTable& table, const spawns::AreaReport& area, const SpeedModel& speeds = {});

}  // namespace aoc::worldd::npc
