// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  spawns/hand_placed.h  (Ashes Reborn: the named mobs the data places nowhere)
//
//  The mob audit of 16 November 2026 (every design record scanned for what places each creature; the report is kept outside the repository, in
//  tools\re\out\mob_locations) found named rares that the design data defines but no WorldSpawn places: Magnus's instance is in no PopulationSet,
//  the Misbegotten's and Financier Carrig's AssetSets are in no instance, and the Lore Warden is in no AssetSet.  The client holds no other position
//  for them: the cooked maps have no NPC spawner actor, the design data's WorldSpawns are the only placements.  So each is put here at its own place,
//  3 m from a rare of the same place, with that rare's instance settings (respawn, wander, aggro, loot) and its own Blueprint and filter.
//  MODELLED: no retail capture shows any of them.  NPC_Minotaur_Berserker_Mummy2_RareSpawn names no place and is left out.
//
//  The live world adds them (AreaSources::hand_placed, set by PlayerWorld::load_area); the tests and the golden counts read the data alone.
// ============================================================================
#pragma once

#include "content/content_db.h"

#include <cstdint>
#include <vector>

namespace aoc::worldd::spawns {

/// One hand-placed point.
struct HandPlaced {
    const char* name;            // the made set's, instance's and WorldSpawn's name (AR_HandPlaced_...)
    int64_t     asset_set;       // the AssetSet the made instance draws from; 0: one is made of `asset`
    int64_t     asset;           // the PopulationAsset a made AssetSet holds (0 when `asset_set` is given)
    int64_t     like_instance;   // the rare of the same place whose instance settings are copied
    double      x, y, z, yaw;    // world cm, degrees
    int         level;           // the placement's level (min and max); 0: the copied instance's
    int64_t     loot = 0;        // the made instance's reward table, in place of the copied instance's (its named drop: the sibling's would be the sibling's); 0: the copied instance's
};

/// The points.
const std::vector<HandPlaced>& hand_placed();

/// The guids the made records take: kHandPlacedGuid + 8 n + (1 the AssetSet, 2 the instance, 3 the set, 4 the WorldSpawn), above every design guid (6.06e18).
inline constexpr int64_t kHandPlacedGuid = 0x7A52000000000000;

/// Adds the points to `data`: a WorldSpawn of the world map, a set of one member, an instance and, when needed, an AssetSet each.  A point whose AssetSet,
/// asset or sibling instance is not in `data` is left out.  Returns the number added.
int add_hand_placed(content::PopulationData& data);

}  // namespace aoc::worldd::spawns
