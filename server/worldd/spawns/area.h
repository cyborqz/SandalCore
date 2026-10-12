// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  spawns/area.h  (plan E1, stage 1; G1, stage 1, Part B)
//
//  The area worldd fills with actors.  It is one of two shapes:
//
//    the Riverlands   (G1 stage 1: `AreaConfig::riverlands()`, worldd's default) the 12 zones of influence whose biome is Riverlands, 49.12 km^2 as 401 exact
//                     rectangles (spawns/region_data.h, the lookup in spawns/region_index.h).  Every point also has a zone of influence (ZOI), the guid of the
//                     record that owns its cell: the spawn filter's zone names (11 PopulationFilters name Aela, Winstead or NewAela) are answered by it.
//    a circle         (E1's: `AreaConfig{}`, and worldd's `--area-radius METRES` > 0) a disc around the login spot, 1 km by default.  It has no zones: a point's
//                     ZOI is 0 ("none") and a filter that names a zone does not spawn there, exactly as in E1, so E1's golden numbers stay E1's.
//
//  `--area-radius` / `AOC_AREA_RADIUS_M`: -1 (the default) the Riverlands, 0 no other actors, > 0 the circle of that radius.  This is the one place the area's
//  shape is set: nothing else reads a number of its own.
//
//  The centre is where a character with no saved position appears: the captured character's spawn point (bootstrap/character_setup.h kDefaultSpawn); the GM
//  vendors and the GM garden stand around it in either shape.
// ============================================================================
#pragma once

#include "bootstrap/character_setup.h"
#include "spawns/region_index.h"

#include <cmath>
#include <cstdint>

namespace aoc::worldd::spawns {

struct AreaConfig {
    double center_x = bootstrap::kDefaultSpawn.x;   // cm: the login spot
    double center_y = bootstrap::kDefaultSpawn.y;
    double radius_cm = 100000.0;                    // the circle's: 1 km
    bool   region = false;                          // true: the Riverlands (radius_cm is then only a hint for tools)
    bool   zones = true;                            // the Riverlands only: a point has a zone of influence (false: none, so a filter that names zones never passes: E1's rule)
// (C.O.A + Cyb)

    /// The Riverlands.
    static AreaConfig riverlands() {
        AreaConfig a;
        a.region = true;
        return a;
    }

    bool contains(double x, double y) const {
        if (region) return region::Index::get().contains(x, y);
        return std::hypot(x - center_x, y - center_y) <= radius_cm;
    }

    /// The guid of the zone of influence that owns (x, y); 0 when the area has no zones (a circle) or the point is outside every zone.
    uint64_t zoi_at(double x, double y) const { return region && zones ? region::Index::get().zoi_guid(x, y) : 0; }
    /// The zone's record name ("Verra_RVR_StartingArea_RuinsOfAela"); nullptr as for zoi_at.
    const char* zoi_name_at(double x, double y) const { return region && zones ? region::Index::get().zoi_name(x, y) : nullptr; }
};

/// The login spot's height (cm): the captured character's spawn point.
inline constexpr double kLoginSpotZ = bootstrap::kDefaultSpawn.z;
/// The yaw the captured character faces at the login spot, degrees.
inline constexpr double kLoginSpotYaw = bootstrap::kDefaultSpawn.yaw_degrees;

}  // namespace aoc::worldd::spawns
