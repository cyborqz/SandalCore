// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  combat/zone_area.h
//
//  The ground a lingering zone reaches, for the query that finds who stands in it (a player's zone: the pipeline asks the world for the
//  creatures and the other players whose bodies touch it).  The shape and the numbers are the zone's data (its targeting record): a sphere,
//  or one or more boxes in the caster's frame (a Wall's, a Saga's).  The zone's own record says where it stands; this only says how far it reaches.
// ============================================================================
#pragma once

#include "combat/combat_data.h"
#include "combat/fighter.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace aoc::worldd::combat {

/// One box of a zone: its half extents, its centre in the zone's frame (the caster's way as the zone's yaw) and its own turn about Z.
struct ZoneBoxArea {
    double ex = 0, ey = 0, ez = 0;
    double cx = 0, cy = 0, cz = 0;
    double yaw = 0;
};

/// What a zone reaches: a sphere of `radius`, or the boxes it lists (a zone may have both in its data: the sphere is used only when it has no box).
struct ZoneArea {
    double                   radius = 0;
    std::vector<ZoneBoxArea> boxes;
    double yaw = 0;   // the zone's own facing (the caster's), degrees
    bool empty() const { return radius <= 0 && boxes.empty(); }
};

/// One actor that stands in a zone, and its side of the zone's caster.
struct ZoneTarget {
    uint64_t actor = 0;
    Relation relation = Relation::Neutral;
    double   distance_cm = 0;   // from the zone's centre, on the ground
};

/// Whether a body (its centre and its radius) touches the zone: inside the sphere, or inside one of the boxes.  A body is measured as a
/// cylinder: on the ground it counts when its edge is inside the shape, and it may stand up to 2 m above or below the zone's plane (a ledge).
inline bool zone_holds(const ZoneArea& area, const Vec3& centre, const Vec3& body, double body_radius_cm) {
    constexpr double kPi = 3.14159265358979323846;
    if (!area.boxes.empty()) {
        for (const ZoneBoxArea& b : area.boxes) {
            // the box's centre in the world: the zone's own frame turns the box's offset, then the box's own turn
            const double zone_yaw = area.yaw * kPi / 180.0;
            const double bx = centre.x + std::cos(zone_yaw) * b.cx - std::sin(zone_yaw) * b.cy;
            const double by = centre.y + std::sin(zone_yaw) * b.cx + std::cos(zone_yaw) * b.cy;
            const double bz = centre.z + b.cz;
            const double yaw = (area.yaw + b.yaw) * kPi / 180.0;
            const double dx = body.x - bx, dy = body.y - by;
            const double along = dx * std::cos(yaw) + dy * std::sin(yaw);
            const double aside = -dx * std::sin(yaw) + dy * std::cos(yaw);
            if (std::fabs(along) - body_radius_cm <= b.ex && std::fabs(aside) - body_radius_cm <= b.ey && std::fabs(body.z - bz) - body_radius_cm <= b.ez) return true;
        }
        return false;
    }
    if (area.radius <= 0) return false;
    const double d = std::hypot(body.x - centre.x, body.y - centre.y);
    if (d - body_radius_cm > area.radius) return false;
    return std::fabs(body.z - centre.z) - body_radius_cm <= area.radius + 200.0;
}

}  // namespace aoc::worldd::combat
