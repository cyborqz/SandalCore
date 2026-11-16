// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  combat/combat_zone_area.inc: who stands in a lingering zone (included at the end of combat_world.cpp, inside its namespace)
//
//  A player's zone asks this at its open and at every tick: the creatures of the world and the other players whose bodies touch the zone's shape.
//  The zone's own filter (its hits' ApplicableTargets) picks who they land on; this only says who is in the ground it reaches, and on whose side.
// ============================================================================

std::vector<ZoneTarget> CombatWorld::actors_in_area(const Fighter& from, const Vec3& centre, const ZoneArea& area, size_t max) {
    std::vector<ZoneTarget> out;
    if (area.empty() || max == 0) return out;
    double reach = area.radius;
    for (const ZoneBoxArea& b : area.boxes) reach = std::max(reach, std::hypot(b.cx, b.cy) + std::hypot(b.ex, b.ey) + std::fabs(b.cz));
    if (!(reach > 0)) return out;
    // the creatures: the registry is asked a little wider than the zone (a body is measured from its centre, and a box's centre may stand off it)
    std::vector<uint64_t> ids;
    registry_.query(centre.x, centre.y, reach + 600.0, ids);
    for (const uint64_t id : ids) {
        const TargetInfo ti = inspect(id, from);
        if (!ti.exists || !ti.alive) continue;
        if (!zone_holds(area, centre, ti.position, ti.radius_cm)) continue;
        out.push_back({id, ti.relation, distance_xy_cm(centre, ti.position)});
    }
    // the other players: a player is no creature of the registry (the caster adds himself; his own side of himself is none)
    for (PlayerPort* p : players_) {
        if (!p || p->actor_id() == from.id() || !p->fighter().alive()) continue;
        const Vec3 at = p->fighter().position();
        if (!zone_holds(area, centre, at, p->fighter().radius_cm())) continue;
        out.push_back({p->actor_id(), data_.relation(from.alignments(), p->fighter().alignments()), distance_xy_cm(centre, at)});
    }
    std::sort(out.begin(), out.end(), [](const ZoneTarget& a, const ZoneTarget& b) { return a.distance_cm != b.distance_cm ? a.distance_cm < b.distance_cm : a.actor < b.actor; });
    if (out.size() > max) out.resize(max);
    return out;
}
