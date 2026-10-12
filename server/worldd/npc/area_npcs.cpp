// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "npc/area_npcs.h"

#include "npc/retail_motion.h"
#include "protocol/repl/codecs.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <map>
#include <sstream>
#include <vector>

namespace aoc::worldd::npc {

namespace repl = ::aoc::protocol::repl;

std::string GroundReport::describe() const {
    std::ostringstream o;
    o << npcs << " NPCs on the ground (" << on_ground << " found a ground, " << no_ground << " did not and stand where they were" << (pending_ground ? ", " + std::to_string(pending_ground) + " wait for their chunk of the ground" : std::string()) << "): " << stationary << " stand, " << wander
      << " wander, " << patrol << " patrol, " << fly << " fly (" << patrol_unwalked << " patrols have no route to walk and wander); the biggest change of height " << static_cast<long long>(largest_snap_cm)
      << " cm, " << moved_by_more_than_1m << " moved more than 1 m; " << default_motion << " NPCs have no line in npc_motion.tsv (the NPCPawn defaults), " << scaled << " are scaled";
    return o.str();
}

std::string archetype_leaf(const std::string& path) {
    const size_t dot = path.rfind('.');
    std::string leaf = path.substr(dot == std::string::npos ? (path.rfind('/') == std::string::npos ? 0 : path.rfind('/') + 1) : dot + 1);
    if (leaf.rfind("Default__", 0) == 0) leaf.erase(0, 9);
    return leaf.empty() ? std::string("?") : leaf;
}

std::vector<std::string> GroundReport::describe_types(size_t per_line) const {
    std::vector<std::pair<std::string, TypeCount>> v(by_type.begin(), by_type.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.total != b.second.total ? a.second.total > b.second.total : a.first < b.first; });
    std::vector<std::string> lines;
    std::ostringstream o;
    size_t n = 0;
    for (const auto& [name, c] : v) {
        o << (n ? ", " : "") << name << " " << c.total << " (" << c.stand << "/" << c.wander << "/" << c.patrol << ")";
        if (++n == per_line) { lines.push_back(o.str()); o.str(""); n = 0; }
    }
    if (n) lines.push_back(o.str());
    return lines;
}

double actor_scale_z(const world::WorldActor& a) {
    const auto& s = a.actor.open.spawn.scale;
    if (!s) return 1.0;
    double z = 1.0;
    switch (s->mode) {
        case repl::QuantVec::Mode::RawDouble: z = std::bit_cast<double>(static_cast<uint64_t>(s->z)); break;
        case repl::QuantVec::Mode::RawFloat: z = static_cast<double>(std::bit_cast<float>(static_cast<uint32_t>(s->z))); break;
        case repl::QuantVec::Mode::Scaled: z = static_cast<double>(s->z) / 100.0; break;
        case repl::QuantVec::Mode::Unscaled: z = static_cast<double>(s->z); break;
    }
    return std::isfinite(z) && z > 0.05 && z < 20.0 ? z : 1.0;
}

const RetailMotion* retail_motion(const std::string& archetype_path) {
    static const std::map<std::string, const RetailMotion*> index = [] {
        std::map<std::string, const RetailMotion*> m;
        for (std::size_t i = 0; i < kRetailMotionCount; ++i) m.emplace(kRetailMotion[i].archetype, &kRetailMotion[i]);
        return m;
    }();
    const auto it = index.find(archetype_path);
    return it == index.end() ? nullptr : it->second;
}

namespace {

/// The number a SpeedMultiplierExpression is ("0.15"), or 0 when it is empty or an expression.
double numeric_expression(std::string e) {
    while (e.size() >= 2 && e.front() == '(' && e.back() == ')') e = e.substr(1, e.size() - 2);   // "(0.2)" is 0.2
    if (e.empty()) return 0;
    char* end = nullptr;
    const double v = std::strtod(e.c_str(), &end);
    return end && *end == 0 && std::isfinite(v) && v > 0 && v < 20 ? v : 0;
}
// [C O A :: C | Y | B]

}  // namespace

NpcMotionSetup make_setup(const world::WorldActor& a, const MotionTable& table, const content::NpcInstanceRow* instance, const content::NpcRouteRow* route, const SpeedModel& speeds,
                          bool* default_motion) {
    NpcMotionSetup s;
    const auto found = table.find(a.npc.archetype);
    if (default_motion) *default_motion = !found;
    const MotionStats m = found ? *found : MotionStats{};
    s.foot_offset = m.half_height * actor_scale_z(a);
    s.yaw_rate = m.rotation_rate_yaw;
    s.home_yaw = a.yaw_degrees;
    s.walk_speed = m.max_walk_speed * speeds.wander_factor;
    s.run_speed = std::max(m.max_run_speed, m.max_walk_speed) * speeds.return_factor;
    s.patrol_speed = m.max_walk_speed * speeds.patrol_default_factor;
    // what the retail captures show of this archetype's walking, where they show it (npc/retail_motion.h): its own speeds and the length of its legs
    const RetailMotion* retail = retail_motion(a.npc.archetype);
    // (a median faster than 55 % of MaxWalkSpeed is a chase or a run the captures caught an idle instance's NPC in, not a wander)
    if (retail && retail->wander_speed >= 20.0f && retail->wander_speed <= 0.55 * m.max_walk_speed) s.walk_speed = retail->wander_speed;
    if (retail && retail->leg_p50 >= 100.0f && retail->leg_p90 > retail->leg_p50) {
        s.leg_median = retail->leg_p50;
        s.leg_p90 = retail->leg_p90;
    }
    if (!instance) return s;   // a captured NPC or a GM vendor: no life of its own
    s.wander_min = instance->min_radius > 0 ? instance->min_radius : 100.0;
    s.wander_max = std::max(instance->max_radius, s.wander_min);
    s.wander_delay = instance->wander_delay;
    s.wander_delay_deviation = instance->wander_delay_deviation;
    s.arrival_distance = instance->arrival_distance > 0 ? instance->arrival_distance : 100.0;
    if (const double e = numeric_expression(instance->speed_expr); e > 0) s.patrol_speed = m.max_walk_speed * e * speeds.patrol_expression_gain;
    else if (retail && retail->patrol_speed >= 20.0f && retail->patrol_speed <= 0.9 * m.max_walk_speed) s.patrol_speed = retail->patrol_speed;
    const std::string& amb = instance->ambient_behavior;
    if (amb.find("Wander") != std::string::npos || amb.find("Default") != std::string::npos || amb.empty()) s.ambient = Ambient::Wander;
    else if (amb.find("Patrol") != std::string::npos) s.ambient = Ambient::Patrol;
    else s.ambient = Ambient::Stationary;
    if (s.ambient == Ambient::Patrol) {
        if (route && !route->relative_city_node && !route->relative_encounter && route->waypoints.size() >= 2) {
            for (const auto& w : route->waypoints) s.route.push_back({w[0], w[1], w[2]});   // already placed (see the header)
            s.pauses = route->pauses;
            s.loop = route->looping;
            s.start_closest = route->start_closest;
        }
        // (without a usable route the patrol wanders: populate counts it)
    }
    return s;
}

GroundReport populate(world::ActorRegistry& registry, NpcWorld& world, const MotionTable& table, const spawns::AreaReport& area, const SpeedModel& speeds) {
    GroundReport rep;
    std::vector<uint64_t> ids;
    registry.for_each([&](const world::WorldActor& a) {
        if (a.kind == world::ActorKind::Npc) ids.push_back(a.id);
    });
    std::sort(ids.begin(), ids.end());   // the registry's order is hash order: the same world every start
    for (const uint64_t id : ids) {
        const world::WorldActor* a = registry.find(id);
        if (!a) continue;
        const content::NpcInstanceRow* inst = nullptr;
        const content::NpcRouteRow* route = nullptr;
        if (a->npc.instance_guid != 0) {
            if (const auto it = area.instances.find(a->npc.instance_guid); it != area.instances.end()) inst = &it->second;
            if (inst && inst->patrol_route != 0) {
                if (const auto r = area.routes.find(inst->patrol_route); r != area.routes.end()) route = &r->second;
            }
        }
        bool default_motion = false;
        NpcMotionSetup setup = make_setup(*a, table, inst, route, speeds, &default_motion);
        if (setup.ambient == Ambient::Patrol && setup.route.size() < 2) {
            setup.ambient = Ambient::Wander;
            ++rep.patrol_unwalked;
        }
        const double z_before = a->z;
        bool found = false, pending = false;
        if (!world.add(id, setup, &found, &pending)) continue;
        ++rep.npcs;
        if (pending) ++rep.pending_ground;
        else (found ? rep.on_ground : rep.no_ground)++;
        if (default_motion) ++rep.default_motion;
        if (actor_scale_z(*a) != 1.0) ++rep.scaled;
        auto& type = rep.by_type[archetype_leaf(a->npc.archetype)];
        ++type.total;
        if (!found && !pending) {
            ++rep.stationary;   // no ground: it stands where it was, whatever its instance says
            ++type.stand;
            ++type.stand_no_life;
        } else {
            switch (setup.ambient) {
                case Ambient::Stationary: ++rep.stationary; ++type.stand; ++(inst ? type.stand_by_data : type.stand_no_life); break;
                case Ambient::Wander: ++rep.wander; ++type.wander; break;
                case Ambient::Patrol: ++rep.patrol; ++type.patrol; break;
            }
        }
        if (found) {
            const double dz = std::fabs(registry.find(id)->z - z_before);
            rep.largest_snap_cm = std::max(rep.largest_snap_cm, dz);
            if (dz > 100.0) ++rep.moved_by_more_than_1m;
        }
    }
    return rep;
}

}  // namespace aoc::worldd::npc
