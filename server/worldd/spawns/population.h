// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  spawns/population.h  (plan E1, stage 1)
//
//  The data's half of the area's spawns: content.db's population chain (contentc version 7) turned into the NPCs that stand within the area.
//
//    placement    a WorldSpawn places a PopulationSet at an origin and a yaw; the set's instances stand at offsets turned by that yaw
//                 (x' = x cos - y sin, y' = x sin + y cos): the 74,778 absolute points of the research, 3,929 of them within 1 km of the login spot.
//    asset        the instance names AssetSets (weighted lists of PopulationAssets); an asset is an NPC blueprint, and the blueprint's archetype
//                 path is what the retail server names it by.  A real spawner draws one at random: here one is drawn by a hash of the point
//                 (the instance, the set, the placement and its key), so the same world is the same at every start, among the assets
//                 of the instance whose archetype a retail capture opened (`CapturedData::template_for`): an asset no capture shows cannot
//                 be shown (its NetGUID and checksum are in no export).
//    level        the instance's NPCLevelMin to NPCLevelMax, drawn by the same hash; the placement's override when it has one.
//
//  area sets    a set with a PopulationArea places no instance at an offset: the area says how many NPCs the world keeps (MinNumSpawns to MaxNumSpawns,
//               drawn by the same kind of hash) and where: inside its circles (Center and Extent, relative to the placement's origin; a circle is
//               picked by its weight times its area, the point uniformly inside it), the set's instances in turn.  The point's height is the
//               ground's (stage 2's NpcWorld stands it there).  The 103 placements of the area's neighbourhood keep about 950 NPCs this way
//               (the stage-1 draft counted the 148 offsets of their members: no NPC).
//    templates    an asset's NPC opens from a captured template (`CapturedData::template_for`) or, when no capture opened its Blueprint, from a
//                 generic one (spawns/generic_npc.h); an asset with neither cannot be shown.
//
//  Points of a blacklisted set or instance are dropped, and (plan E1, stage 3) so is every point the spawn filter says retail does not spawn: another
//  map's, a quest's or event's, one whose conditions this world does not meet, a Blueprint with no valid visuals (spawns/spawn_filter.h).
// ============================================================================
#pragma once

#include "content/content_db.h"
#include "spawns/area.h"
#include "spawns/captured_spawns.h"
#include "spawns/generic_npc.h"
#include "spawns/spawn_filter.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace aoc::worldd::spawns {

/// Where an asset's NPC opens come from: a captured template first, else a generic one (null: captured only).
struct TemplateSource {
    const CapturedData* captured = nullptr;
    const GenericNpcs*  generic = nullptr;

    /// The template for `asset`; nullptr, with `why` (Verdict::NoPath when the asset names no Blueprint) when there is none.  `is_generic` says which.
    const OpenTemplate* find(const content::NpcAssetRow& asset, bool* is_generic = nullptr, Verdict* why = nullptr) const;
};

/// One NPC the data places inside the area, with what is needed to make it.
struct DataSpawn {
    double                       x = 0, y = 0, z = 0, yaw = 0;   // cm, degrees
    int                          level = 0;
    const content::NpcAssetRow*    asset = nullptr;
    const content::NpcInstanceRow* instance = nullptr;
    const OpenTemplate*          open = nullptr;
    bool                         generic = false; // made from the generic donor (no capture opened the Blueprint)
    bool                         from_area = false; // a PopulationArea's random point: `z` is the circle's, the ground decides
    int64_t                      placement = 0;   // the WorldSpawn
    int64_t                      set = 0;
    int64_t                      key = 0;
    uint64_t                     seed = 0;        // the point's hash
    uint64_t                     zoi = 0;         // the guid of the zone of influence the point stands in (G1: the Riverlands' grid; 0: the area has none)
};

/// What the selection found, for the report.
struct PopulationStats {
    size_t placements = 0;               // placements of the whole world looked at
    size_t points_in_area = 0;           // instance points within the area (before any filter)
    size_t blacklisted = 0;              // dropped: a blacklisted set or instance
    size_t area_sets = 0;                // members of sets with a PopulationArea within the area (the area's random points are counted below)
    size_t area_points = 0;              // the random points of the areas' circles within the area (before any filter)
    size_t area_spawned = 0;             // ... of which became NPCs
    size_t generic_spawned = 0;          // NPCs made from a generic template (data and area points)
    size_t no_asset = 0;                 // the instance names no asset set or the sets hold no asset
    size_t no_template = 0;              // no asset of the point has an archetype a capture opened
    size_t spawned = 0;
    std::map<std::string, size_t> by_archetype;     // spawned NPCs by archetype leaf
    std::map<uint64_t, size_t>    by_zoi;           // spawned NPCs by the zone of influence they stand in (the Riverlands; empty for a circle)
    std::map<std::string, size_t> missing;          // points lost to a missing template, by the leaf of the blueprint that would have been drawn
    std::map<std::string, size_t> generic_by_archetype;   // NPCs made from a generic template, by the leaf of the Blueprint
    std::map<Verdict, size_t>     lost_by_verdict;  // the points lost, by why (a prop is NotPawn)
    std::map<std::string, size_t> lost_props;       // ... the NotPawn ones by Blueprint leaf
    std::map<std::string, size_t> lost_excluded;    // ... the ones the kill switch left out, by Blueprint leaf
    /// Plan E1, stage 3: the points the spawn filter removed, by reason (a point is counted once: the first reason, or for an instance whose assets
    /// all fail the reason of its first asset), and the asset-level reasons by the Blueprint leaf of that first asset.
    std::map<Why, size_t>                         filtered;
    std::map<Why, std::map<std::string, size_t>>  filtered_by_blueprint;
    size_t filtered_total() const { size_t n = 0; for (const auto& [w, c] : filtered) n += c; return n; }
};
// ~ CoA :: C Y B ~

/// The NPCs of the data within `area`, with their templates. Pure: the same inputs give the same list, in the order of the placements (a
/// placement's own points first, then its area's).
std::vector<DataSpawn> select_data_spawns(const content::PopulationData& data, const TemplateSource& templates, const AreaConfig& area, PopulationStats& stats,
                                          const SpawnFilter* filter = nullptr);



/// A place a dead player may come back at (plan E1, stage 3, part 2): a respawn shrine the data places (an AttuneableCharacterRespawn or a static
/// CharacterRespawn Blueprint; the corrupted players' shrines are another kind).
struct RespawnPoint {
    double      x = 0, y = 0, z = 0, yaw = 0;   // cm, degrees
    bool        attuneable = false;
    std::string name;                           // the asset's name ("Emberspring_Respawn_Shrine_Attuneable")
};

/// The respawn shrines of the data's open-world placements within `area`, nearest the area's centre first.
std::vector<RespawnPoint> respawn_points(const content::PopulationData& data, const AreaConfig& area);

/// The absolute points of a placement: where each member of its set stands (cm), and its yaw (degrees).
struct Point {
    double  x = 0, y = 0, z = 0, yaw = 0;
    int64_t key = 0;
    int64_t instance = 0;
};
std::vector<Point> placement_points(const content::NpcPlacement& placement, const content::NpcSetRow& set);

/// The random points a placement of an area set keeps in the world (before the area's radius is applied): the count the area's Min/MaxNumSpawns
/// draw, each point a circle picked by weight times its area and a place inside it, standing for the set's instances in turn.
std::vector<Point> area_spawn_points(const content::NpcPlacement& placement, const content::NpcSetRow& set, const content::NpcAreaRow& area);

/// A hash of a point's identity: the same for the same inputs on every platform.
uint64_t point_hash(int64_t instance, int64_t set, int64_t placement, int64_t key);

/// Judges ONE point of a placed set (the body of select_data_spawns' loop, shared with the node populations, spawns/node_population.h): the spawn filter's verdict on the point and on each
/// candidate asset of its instance, the template, the weighted draw by the point's hash and the level.  `placement_id` is the id the hash and DataSpawn::placement carry (the WorldSpawn's
/// guid; a node's populations use the node's, or the node's and the plot's), `min_level` and `max_level` the placement's level overrides (0: none), `zoi` the zone the point stands
/// in (0: none; the filter's own context zone stands in), `world_spawn` may be null.  True, with `out` filled, when an NPC spawns; false when the point is dropped, counted in `stats`.
bool spawn_point(const content::PopulationData& data, const TemplateSource& templates, const SpawnFilter* filter, const content::NpcWorldSpawn* world_spawn, const content::NpcSetRow& set,
                 const content::NpcInstanceRow& inst, const Point& pt, int64_t placement_id, int min_level, int max_level, int64_t zoi, bool from_area, PopulationStats& stats, DataSpawn& out);

}  // namespace aoc::worldd::spawns
