// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  fishing/fishing.h  (Fishing, milestone 1)
//
//  What fishing needs that is the world's and not one player's: the water (fishing/water_map.h), the fishing ability and its montage, buff and projectile from the ability tables, the
//  gathering data (the interaction a water names: its rod, certification, channel and reward table) and where a cast lands.  A session asks it to plan a cast and runs the cast's timeline
//  (fishing/cast.h) itself.
//
//  Where a cast lands [modelled from the projectile's record: Fishing_Projectile is an arcing projectile of speed 3500 cm/s fired at 20 degrees above the character's facing from a point
//  150 cm to its right and 40 cm behind it, under the arcing gravity scale of 15.  The fire rotation is the owner's orientation plus that 20 degrees, as the other projectiles' fire rotations
//  here are, and an orientation has a yaw and no pitch.  The cast's ServerTriggerAbility carries no aim (no targets, no transform, a zero input vector).  Every move does carry the camera's
//  pitch in its control rotation (GoodPosition::pitch16); it plays no part here, and whether the game's own throw uses it is not known]: the lure flies along its arc until it comes down to
//  the height of the water's surface.  The yaw is the control rotation's of the character's last accepted move.  A cast that comes down on a lake's outline or on a river's swim volumes,
//  within the projectile's range, is on that water; anywhere else the cast fails.
// ============================================================================
#pragma once

#include "fishing/cast.h"
#include "fishing/water_map.h"
#include "gather/gathering.h"
#include "session/ability_catalog.h"

#include <cstdint>
#include <memory>
#include <string>

namespace aoc::worldd::fishing {

struct Config {
    std::string water_path = "dist/content/fishing_water.tsv";
    Timing      timing;
};

/// The throw of the lure (see above); the constants the projectile table does not hold.
struct Throw {
    double speed_cm_s = 3500.0;        // ab_projectile.speed of Fishing_Projectile
    double range_cm = 15000.0;         // ab_projectile.range
    double gravity_cm_s2 = 980.0 * 15.0;   // the arcing gravity scale of 15 on the game's 980 cm/s^2
    double pitch_deg = 20.0;           // FireTransform's pitch
    double forward_cm = -40.0;         // FireTransform's offset: behind (negative) the character
    double right_cm = 150.0;           // ... and to its right
    double hand_cm = 100.0;            // the height of the rod's tip above the feet [modelled]
    double max_drop_cm = 1500.0;       // a surface lower than this under the feet is out of the cast's reach [modelled]
};

/// Where a cast lands and what is there.
struct Landing {
    bool                         lands = false;      // on water a line can be cast into, within range
    double                       x = 0, y = 0;       // where it comes down (cm); the flat throw's point when it found no water
    double                       distance_cm = 0;    // from the character
    double                       flight_s = 0;
    std::optional<WaterHit>      water;
    const content::GatherDefRow* def = nullptr;      // the interaction the water names
    std::string                  why;                // when it does not land: what is there instead
};

class Fishing {
public:
    /// nullptr, with `error`, when the ability tables have no Economy_FishingCast, or the water file does not read (a file that is missing is an error too: no water, no fishing).
    static std::unique_ptr<Fishing> make(const Config& config, const abilities::Catalog& catalog, gather::Gathering& gathering, std::string& error);

    const WaterMap&              water() const { return water_; }
    const Config&                config() const { return config_; }
    const abilities::AbilityDef& ability() const { return *ability_; }
    const Throw&                 throw_model() const { return throw_; }
    gather::Gathering&           gathering() { return *gathering_; }
    const gather::Gathering&     gathering() const { return *gathering_; }
    /// The montage the ability plays (anim index 0): its soft path and static NetGUID; 0 for none.
    const std::string&           montage_path() const { return montage_path_; }
    uint64_t                     montage_guid() const { return montage_guid_; }

    /// Where a cast from (x, y), feet at `feet_z`, facing `yaw_degrees` comes down.
    Landing plan_cast(double x, double y, double feet_z, double yaw_degrees) const;

    /// The interaction of the first water in the file: what a cast that lands nowhere is checked against (the rod and the profession), since no water names one for it.
    const content::GatherDefRow* default_def() const;

    /// A line of the log for the start: what the world fishes.
    std::string describe() const;

private:
    Fishing() = default;
    Config                       config_;
    WaterMap                     water_;
    const abilities::AbilityDef* ability_ = nullptr;
    gather::Gathering*           gathering_ = nullptr;
    Throw                        throw_;
    std::string                  montage_path_;
    uint64_t                     montage_guid_ = 0;
};

/// The horizontal distance a lure that leaves `drop_cm` above the surface it comes down on has flown (the arc's own range, with no offset); 0 when the throw cannot reach that low.
double flight_distance(const Throw& t, double drop_cm, double* flight_seconds = nullptr);

}  // namespace aoc::worldd::fishing
