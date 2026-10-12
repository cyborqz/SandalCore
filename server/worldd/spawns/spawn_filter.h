// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  spawns/spawn_filter.h  (plan E1, stage 3, part 1)
//
//  Which of the design data's spawn points retail spawns for a character in this area now.  Stage 2 spawned every point whose Blueprint could be
//  opened: the user's run then showed spawns retail does not have (invisible villagers, invisible "NPC Quest Master"s, a T-posed "Wail", ...).  The
//  filter is the data's own conditions, evaluated against what this world is (SpawnContext), and the answer key is the retail captures.
//
//    the point     a WorldSpawn places a PopulationSet; the point spawns when
//                    * no record on its way is bDevOnly (WorldSpawn, set, instance, asset);
//                    * its WorldSpawn is the world map's (MapName "Verra_World_Master"): the rest are quest, event and phase spawns (RVR_QST_..., RVR_OST_...,
//                      WS_EVT_...), a node's layout (NodeHelper_...: placed at the node, not at these coordinates) and the designers' test maps.  Calibration
//                      against the 1,105 retail captures: of the points a capture's player walked within 120 m of, 12.3% of the world map's have an NPC of
//                      their archetype within 20 m, the empty-map ones 0.4% (10 of 2,345, all of one event) and every test map 0%;
//                    * it is no blacklisted set or instance;
//                    * no quest objective, event, story arc phase, building level, node layout or action response spawns its set when its turn comes
//                      (npc_gate): a fresh character has no quest, no event runs and no phase or building is announced (SpawnContext::active_gates
//                      names the sources a test or a later stage says are on).  The sets a kill objective, a dialogue source or a tracker merely names
//                      are the world's own and are not gated: of the instances such a record names that were seen in a covered place 1.6% have an NPC
//                      against 12.7%, which is the gate for the instance-level spawn records, not the tracking ones.
//    the asset     each candidate asset of the instance must pass its PopulationFilter, or the FilteringOverride of the instance, else the set, else the
//                  WorldSpawn (the most specific wins and replaces the asset's own): the node level (Wilderness: this world has no node), the culture
//                  (Kaelar: the area is Kaelar's), node ids (the area names none) and zones of influence (G1: the point's own, from the Riverlands' ZOI
//                  grid; a circle area has none), the node type, the biome, the season, the weather,
//                  the time of day (the game hour) and the required gameplay tags (the instance's, the set's, the asset set's, the asset's, the world's)
//    the visuals   a Blueprint must draw (npc_motion.tsv, from the game's own Blueprints by tools/mapc):
//                    * a hidden actor or mesh (the siege gates, the insect cloud), a mesh with the engine's pink placeholder material, a mesh with no
//                      AnimClass (it stands in a T-pose: "Lingering Wail") and a Blueprint with no mesh at all do not;
//                    * a humanoid's mesh is the player's master mesh (SK_PC_MASTER) and its body is merged at run time from an NPCAppearanceComponent, which
//                      a generic open does not carry: of the 172 Blueprints the retail captures open, those with that mesh have the component in all 92 and
//                      the others in none (0 of 80).  The villagers and the quest masters of the stage-2 run were invisible for that: only the Blueprints in
//                      SpawnContext::shown_humanoids (Guard_Soldier, which the user saw) open, the others wait for a stage that makes the appearance;
//                    * an asset tagged npc.type.prop is a prop.
//
//  Everything is counted by reason and by Blueprint (PopulationStats), and logged at start.  `--no-spawn-filter` turns it all off.
//
//  Nodes 1, stage 3b: the required gameplay tags.  A filter that requires siege states (`War.Siege.States.*`: 45 filters require Declared, Inactive and Prep, 5 require Active) passes
//  when the spawner carries ANY of them: the four states are mutually exclusive and every retail node carries only SiegeInactive (E1's all-of could never pass).  Every other
//  required tag is all-of, as before.
// ============================================================================
#pragma once

#include "content/content_db.h"
#include "npc/motion_table.h"

#include <cstdint>
#include <set>
#include <string>
// ~ C-O-A & C-Y-B ~

namespace aoc::worldd::spawns {

/// What the world is, for the conditions a spawn names.  The defaults are this server's: the open world, no node, the Kaelar area, summer, clear, noon.
struct SpawnContext {
    bool        enabled = true;
    std::string map_name = "Verra_World_Master";
    std::string node_level = "Wilderness";        // an ENodeLevel name
    std::string culture = "Kaelar";
    std::string biome;                            // "": unknown (a filter that names biomes never matches)
    std::string season = "Summer";
    std::string weather;                          // "": clear
    std::string node_type;                        // "": none
    int64_t     node_id = 0;                      // 0: none
    double      hour = 12.0;                      // the game hour, 0 to 24
    std::set<std::string> world_tags{"War.Siege.States.SiegeInactive"};
    /// Nodes 1, stage 3b: the zone of influence the context itself stands in (a city node's), used when a point has none of its own.  0: none.  A node's populations (a
    /// layout's, a building's) are judged with their node's zone, so a filter that names zones passes there (the area's circle has none: E1's zone refusal stays for it).
    int64_t     zone = 0;
    /// The spawn sources (npc_gate.source: "NarrativeObjective", "EventTriggerDef", "StoryArcPhase", ...) that are running; a set only they spawn spawns.
    std::set<std::string> active_gates;
    /// Let the Blueprints whose body needs an NPCAppearanceComponent through (their opens carry none: they show invisible).
    bool        humanoids = false;
    /// The appearance-dependent Blueprints (their class names) that open with a valid look all the same.
    std::set<std::string> shown_humanoids{"Guard_Soldier"};
};

/// Why a point or an asset does not spawn, in the order the filter asks.
enum class Why : uint8_t {
    Spawns,
    DevOnly,        // a record on its way is bDevOnly
    NotWorldMap,    // its WorldSpawn is another map's: a quest, event or phase spawn, a node layout, a test map
    Blacklisted,
    Gated,          // a quest, an event, a story arc phase, a building level, a layout or an action response spawns its set when its turn comes
    NodeLevel,
    Culture,
    NodeOrZone,
    NodeType,
    Biome,
    Season,
    Weather,
    TimeOfDay,
    RequiredTag,
    Prop,           // the asset is tagged npc.type.prop
    Hidden,         // the Blueprint's actor or mesh is hidden: a controller, a gate, a cloud
    Placeholder,    // its mesh wears the engine's placeholder material
    NoMesh,
    NoAnimation,    // a mesh with no AnimClass: a T-pose
    Appearance,     // a humanoid whose body is merged from an NPCAppearanceComponent the open does not carry
};

const char* why_name(Why w);
/// One sentence for the log.
const char* why_text(Why w);

class SpawnFilter {
public:
    /// `motion` (may be null: nothing is known of the Blueprints' visuals, and they are taken to draw) must outlive this.
    SpawnFilter(SpawnContext ctx, const npc::MotionTable* motion) : ctx_(std::move(ctx)), motion_(motion) {}

    const SpawnContext& context() const { return ctx_; }
    bool enabled() const { return ctx_.enabled; }

    /// The point's conditions: dev only, the map, blacklists, gates.  `world_spawn` may be null (a placement whose WorldSpawn is no row).
    Why point(const content::PopulationData& data, const content::NpcWorldSpawn* world_spawn, const content::NpcSetRow& set, const content::NpcInstanceRow& instance) const;

    /// One candidate asset: dev only, the PopulationFilter (or the override that replaces it), the tags, the Blueprint's visuals.  `asset_set` is the
    /// AssetSet the instance drew it from (its tags count).
    /// `zoi` is the guid of the zone of influence the point stands in (spawns/area.h `AreaConfig::zoi_at`; 0: the area has no zones, so a filter that names one
    /// does not spawn: E1's rule).
    Why asset(const content::PopulationData& data, const content::NpcWorldSpawn* world_spawn, const content::NpcSetRow& set, const content::NpcInstanceRow& instance,
              int64_t asset_set, const content::NpcAssetRow& asset, int64_t zoi = 0) const;

    /// A PopulationFilter against the context; `tags` are the gameplay tags the spawner carries; `zoi` as above (a filter that names zones needs the point's to be one
    /// of them).
    Why filter(const content::PopFilter& f, const std::set<std::string>& tags, int64_t zoi = 0) const;

    /// A Blueprint's visuals (the part of `asset` that needs the motion table).
    Why visuals(const content::NpcAssetRow& asset) const;

    /// An ENodeLevel name as its number (Wilderness 0, Crossroads 1, Encampment 2, Village 3, Town 4, City 5, Metropolis 6; cCount 7 and an unknown
    /// name -1).
    static int node_level_number(const std::string& name);

private:
    SpawnContext            ctx_;
    const npc::MotionTable* motion_;
};

/// The class name of an archetype path or class ("Wolf" of ".../Wolf.Default__Wolf_C").
std::string archetype_leaf(const std::string& archetype_path);

}  // namespace aoc::worldd::spawns
