// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  spawns/gm_garden.cpp  (see gm_garden.h)
// ============================================================================
#include "spawns/gm_garden.h"

#include "spawns/spawn_util.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

namespace aoc::worldd::spawns {

namespace {

using util::angle16;
using util::angle8;
using util::set_path;

constexpr double kPi = 3.14159265358979323846;

int profession_index(const char* profession) {
    if (std::strcmp(profession, "Mining") == 0) return 0;
    if (std::strcmp(profession, "Lumberjacking") == 0) return 1;
    return 2;
}

uint64_t mix(uint64_t z) {
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
// C-Y-B + C.O.A

/// Moves a node made from a captured open to (x, y, z) facing `yaw`, and gives it the def.  False, with `error`, when a value is refused.
bool make_node(world::WorldActor& w, const garden::Def& def, double x, double y, double z, double yaw, std::string& error) {
    repl::Actor& a = w.actor;
    repl::ObjectState& actor = a.obj;
    if (!set_path(actor, "GatherableActorDef.Guid", repl::Value::i64(static_cast<int64_t>(def.guid)), error)) return false;
    // the interaction of the def: the first input of the component names the PlayerInteractionDef
    for (repl::Subobject& s : a.subobjects) {
        if (!s.obj.schema || std::strcmp(s.obj.schema->name, "UInteractableComponent") != 0) continue;
        const repl::Value* data = s.obj.state.get(*s.obj.schema, "InteractionInputData");
        const auto* arr = data ? data->get_if<repl::ArrayValue>() : nullptr;
        if (arr && !arr->elements.empty()) {
            repl::ArrayValue copy = *arr;
            // (review A M6) an element's values are a map by field index: `values[6]` on a donor without field 6 would make one up.  The garden writes field 6 (the def's Guid) only where the donor has it.
            if (!copy.elements[0].values.count(6)) {
                error = "the donor's interaction input has no field 6 (PlayerInteractDefinition.Guid)";
                return false;
            }
            copy.elements[0].values[6] = repl::Value::i64(static_cast<int64_t>(def.interaction));   // InteractionInputData.PlayerInteractDefinition.Guid
            if (!s.obj.state.set(*s.obj.schema, "InteractionInputData", repl::Value(std::move(copy)))) {
                error = "the interaction of the garden node is refused";
                return false;
            }
        }
    }
    // the place
    a.open.spawn.location = repl::quantize_vector(10, x, y, z);
    a.open.spawn.rotation = repl::Rot{0, angle16(yaw), 0};
    a.open.spawn.velocity.reset();
    if (const repl::Value* v = actor.state.get(7)) {
        if (const auto* old = v->get_if<repl::RepMovement>()) {
            repl::RepMovement m = *old;
            m.location = repl::quantize_location(actor.opts, x, y, z);
            m.rotation = repl::Rot{0, actor.opts.rep_movement_short_rotation ? angle16(yaw) : angle8(yaw), 0};
            m.linear_velocity = repl::QuantVec{repl::QuantVec::Mode::Scaled, 0, 0, 0};
            m.angular_velocity = repl::QuantVec{repl::QuantVec::Mode::Scaled, 0, 0, 0};
            m.acceleration.reset();
            if (!set_path(actor, "ReplicatedMovement", repl::Value(m), error)) return false;
        }
    }
    w.x = x;
    w.y = y;
    w.z = z;
    w.yaw_degrees = yaw;
    return true;
}

}  // namespace

const OpenTemplate* garden_donor(const CapturedData& captured) {
    for (const CapturedSpawn& s : captured.spawns()) {
        if (s.open && s.open->category == "gatherable") return s.open;
    }
    return nullptr;
}

std::string GardenReport::describe() const {
    std::ostringstream o;
    o << "GM garden: " << nodes.size() << " nodes";
    size_t ungrounded = 0;
    for (const GardenNode& n : nodes) ungrounded += n.grounded ? 0 : 1;
    if (ungrounded) o << " (" << ungrounded << " at the login spot's height: no ground there)";
    o << "; " << gaps << " profession/tier slots have no actor node in the design data (Master for the three professions, GrandMaster for Mining and Lumberjacking)";
    if (unplaced) o << "; " << unplaced << " could not be placed";
    o << ":";
    for (const GardenNode& n : nodes) {
        o << " " << n.def->profession << "/" << n.def->tier_name << "=" << n.def->name << "@(" << static_cast<long long>(n.x) << "," << static_cast<long long>(n.y) << ","
          << static_cast<long long>(n.z) << ")";
    }
    return o.str();
}
// built by C O A with C_Y_B

GardenReport place_gm_garden(world::ActorRegistry& registry, const OpenTemplate* donor, const AreaConfig& area, const Minter& mint, uint32_t server_id, double world_time,
                             const GardenGround& ground) {
    GardenReport report;
    report.gaps = garden::kGardenGapCount;
    if (!donor) {
        report.notes.push_back("no captured gatherable open to make the garden from: no garden");
        report.unplaced = garden::kGardenDefCount;
        return report;
    }
    const double facing = kLoginSpotYaw;
    std::vector<uint64_t> near;
    for (size_t i = 0; i < garden::kGardenDefCount; ++i) {
        const garden::Def& def = garden::kGardenDefs[i];
        const int prof = profession_index(def.profession);
        const double base_angle = facing + (prof - 1) * 55.0;     // a sector for each profession, the middle one straight ahead
        const double base_radius = 3200.0 + 600.0 * def.tier;     // the tiers outwards: 32 m for Novice ... 56 m for GrandMaster
        bool placed = false;
        // the candidates, nearest the ideal first: along the ring (3 degrees a step, both ways), then a ring nearer or further (4 m a step)
        for (int ring = 0; ring <= 12 && !placed; ++ring) {
            const double radius = base_radius + ((ring % 2) ? 1 : -1) * ((ring + 1) / 2) * 400.0;
            if (radius < kGardenMinCm || radius > kGardenMaxCm) continue;
            for (int step = 0; step <= 16 && !placed; ++step) {
                const double angle = base_angle + ((step % 2) ? 1 : -1) * ((step + 1) / 2) * 3.0;
                const double rad = angle * kPi / 180.0;
                const double x = area.center_x + radius * std::cos(rad), y = area.center_y + radius * std::sin(rad);
                if (!area.contains(x, y)) continue;
                double z = kLoginSpotZ;
                bool grounded = false;
                if (ground) {
                    if (!ground(x, y, kLoginSpotZ, &z)) continue;
                    grounded = true;
                }
                bool clear = true;
                near.clear();
                registry.query(x, y, kGardenActorClearCm, near);
                if (!near.empty()) clear = false;
                for (const GardenNode& n : report.nodes) {
                    if (std::hypot(n.x - x, n.y - y) < kGardenNodeClearCm) clear = false;
                }
                if (!clear) continue;
                InstanceParams p;
                p.keep_captured_position = true;   // (the node is moved below)
                p.server_id = server_id;
                p.world_time = world_time;
                p.seed = mix(0x6A12DE0ull ^ def.guid);
                std::string why;
                auto w = instantiate(*donor, mint, p, why);
                if (!w || !make_node(*w, def, x, y, z, std::fmod(angle + 180.0 + static_cast<double>(mix(def.guid) % 360), 360.0), why)) {
                    report.notes.push_back(std::string("the garden node ") + def.name + " was refused: " + why);
                    ++report.unplaced;
                    placed = true;   // (not retried: the same donor would be refused again)
                    break;
                }
                w->npc.source = world::Source::Runtime;
                const uint64_t id = w->id;
                const double yaw = w->yaw_degrees;
                if (!registry.add(std::move(*w))) {
                    report.notes.push_back(std::string("the garden node ") + def.name + " could not be added to the registry");
                    ++report.unplaced;
                    placed = true;
                    break;
                }
                GardenNode node;
                node.def = &def;
                node.actor_id = id;
                node.x = x;
                node.y = y;
                node.z = z;
                node.yaw = yaw;
                node.grounded = grounded;
                report.nodes.push_back(node);
                placed = true;
            }
        }
        if (!placed) {
            ++report.unplaced;
            report.notes.push_back(std::string("no free place for the garden node ") + def.name + " between 30 and 60 m from the login spot");
        }
    }
    return report;
}

}  // namespace aoc::worldd::spawns
