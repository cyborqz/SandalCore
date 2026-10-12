// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  spawns/area_loader.cpp
//
//  See area_loader.h.
// ============================================================================
#include "spawns/area_loader.h"

#include "world/interaction_state.h"

#include <cmath>
#include <sstream>

namespace aoc::worldd::spawns {

namespace {

std::string leaf_of(const std::string& archetype_path) {
    const size_t dot = archetype_path.rfind('.');
    std::string leaf = archetype_path.substr(dot == std::string::npos ? 0 : dot + 1);
    if (leaf.rfind("Default__", 0) == 0) leaf = leaf.substr(9);
    if (leaf.size() > 2 && leaf.compare(leaf.size() - 2, 2, "_C") == 0) leaf.resize(leaf.size() - 2);
    return leaf;
}

uint64_t mix(uint64_t z) {
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

}  // namespace

std::string AreaReport::describe() const {
    std::ostringstream o;
    if (area.region) {
        o << "area: the Riverlands (" << region::kRiverlandsRectCount << " rectangles, " << region::kRiverlandsAreaCells * (region::kCellCm / 100.0) * (region::kCellCm / 100.0) / 1.0e6
          << " km2, " << region::kZoiCount << " zones of influence)";
    } else {
        o << "area: centre (" << static_cast<long long>(area.center_x) << ", " << static_cast<long long>(area.center_y) << ") cm, radius " << area.radius_cm / 100.0 << " m";
    }
    o << ": " << data_npcs << " NPCs from the data (" << population.points_in_area << " placement points and " << population.area_points << " random points of "
      << population.area_sets << " area sets in the area: " << population.generic_spawned << " NPCs made from a generic Blueprint template, " << population.area_spawned
      << " from area points, " << population.no_template << " points without any template, " << population.blacklisted << " blacklisted, " << population.no_asset
      << " without an asset, " << population.filtered_total() << " dropped by the spawn filter), " << captured_npcs << " captured NPCs (" << duplicates << " dropped as the data's), " << gatherables << " resource nodes, " << gm_vendors
      << " GM vendors";
    return o.str();
}
// (Cyb) (C.O.A)

std::optional<world::WorldActor> make_data_actor(const DataSpawn& s, const Minter& mint, uint32_t server_id, double world_time, InstanceParams* params, std::string& error) {
    InstanceParams p;
    p.x = s.x;
    p.y = s.y;
    p.z = s.z;
    p.yaw_degrees = s.yaw;
    p.level = s.level;
    p.server_id = server_id;
    p.world_time = world_time;
    p.pop_instance = s.instance ? s.instance->guid : 0;
    p.pop_asset = s.asset ? s.asset->guid : 0;
    p.seed = s.seed;
    if (params) *params = p;
    auto w = instantiate(*s.open, mint, p, error);
    if (!w) return std::nullopt;
    w->npc.source = world::Source::Data;
    w->npc.ground_by_terrain = s.from_area;
    w->npc.asset_guid = p.pop_asset;
    w->npc.instance_guid = p.pop_instance;
    w->npc.vendor_guid = s.instance ? s.instance->vendor : 0;
    w->npc.home_x = s.x;
    w->npc.home_y = s.y;
    w->npc.home_z = s.z;
    // how far it notices and follows: the instance's own radius when it overrides the asset's [R: research/combat 6]
    if (s.instance && s.instance->override_aggro && s.instance->aggro_radius > 0) w->npc.aggro_radius_cm = s.instance->aggro_radius;
    else if (s.asset && s.asset->aggro_radius > 0) w->npc.aggro_radius_cm = s.asset->aggro_radius;
    if (s.asset && s.asset->leash_radius > 0) w->npc.leash_radius_cm = s.asset->leash_radius;
    if (s.instance) {
        w->npc.respawn_seconds = s.instance->respawn_time;
        w->npc.respawn_variance = s.instance->respawn_variance;
    }
    // what its death does: the tables its asset and instance name, how it comes back
    if (s.asset) w->npc.loot_tables.insert(w->npc.loot_tables.end(), s.asset->loot_tables.begin(), s.asset->loot_tables.end());
    if (s.instance) {
        w->npc.loot_tables.insert(w->npc.loot_tables.end(), s.instance->loot_tables.begin(), s.instance->loot_tables.end());
        w->npc.respawn_rule = s.instance->respawn_rule;
    }
    return w;
}

std::optional<AreaReport> load_area(world::ActorRegistry& registry, const AreaSources& sources, const AreaConfig& area, const Minter& mint, uint32_t server_id,
                                    double world_time, std::string& error) {
    if (!sources.db || !sources.captured) {
        error = "the area needs content.db and the captured spawns";
        return std::nullopt;
    }
    AreaReport report;
    report.area = area;
    auto loaded = sources.db->population_data(error);
    if (!loaded) return std::nullopt;
    // (kept by the report: the node populations select from the same chain, and an NPC's life needs its instance's and route's rows)
    report.data = std::make_shared<const content::PopulationData>(std::move(*loaded));
    const content::PopulationData* population = report.data.get();

    report.respawn_points = respawn_points(*population, area);
    const TemplateSource templates{sources.captured, sources.generic};
    const std::vector<DataSpawn> data = select_data_spawns(*population, templates, area, report.population, sources.filter);
    const auto note = [&](std::string n) {
        if (report.notes.size() < 20) report.notes.push_back(std::move(n));
    };

    // the data's NPCs
    struct Placed { double x, y; std::string archetype; };
    std::vector<Placed> placed;
    for (const DataSpawn& s : data) {
        InstanceParams p;
        std::string why;
        auto w = make_data_actor(s, mint, server_id, world_time, &p, why);
        if (!w) {
            ++report.refused;
            note("a data NPC (" + leaf_of(s.open->archetype_path) + ") was refused: " + why);
            continue;
        }
        if (s.instance) {
            report.instances.emplace(s.instance->guid, *s.instance);
            if (s.instance->patrol_route != 0) {
                if (const auto r = population->routes.find(s.instance->patrol_route); r != population->routes.end()) report.routes.emplace(r->first, r->second);
            }
            report.recipes.push_back({s.open, p, w->npc});
            w->npc.spawn_ref = static_cast<uint32_t>(report.recipes.size());
            report.recipes.back().info.spawn_ref = w->npc.spawn_ref;
        }
        const std::string path = s.open->archetype_path;
        const double x = w->x, y = w->y;
        if (!registry.add(std::move(*w))) {
            ++report.refused;
            note("a data NPC could not be added to the registry");
            continue;
        }
        placed.push_back({x, y, path});
        ++report.data_npcs;
        ++report.npcs_by_archetype[leaf_of(path)];
    }
// C_Y_B and CoA

    // the captured ones: an NPC is dropped when the data has one of its archetype at the same point
    std::vector<Placed> nodes;
    for (size_t i = 0; i < sources.captured->spawns().size(); ++i) {
        const CapturedSpawn& c = sources.captured->spawns()[i];
        if (!area.contains(c.x, c.y)) continue;
        const bool npc = c.open->category == "npc";
        const std::vector<Placed>& against = npc ? placed : nodes;
        const double near = npc ? kSamePointCm : 50.0;
        bool dup = false;
        for (const Placed& p : against) {
            if (p.archetype == c.open->archetype_path && std::hypot(p.x - c.x, p.y - c.y) <= near) {
                dup = true;
                break;
            }
        }
        if (dup) {
            ++(npc ? report.duplicates : report.gatherable_duplicates);
            continue;
        }
        InstanceParams p;
        p.keep_captured_position = true;
        p.server_id = server_id;
        p.world_time = world_time;
        p.seed = mix(0xC0FFEEull ^ static_cast<uint64_t>(i));
        std::string why;
        auto w = instantiate(*c.open, mint, p, why);
        if (!w) {
            ++report.refused;
            note("a captured spawn (" + leaf_of(c.open->archetype_path) + ") was refused: " + why);
            continue;
        }
        // the spawn stands where its capture saw it: the registry's position is the capture's own place for this spawn
        w->x = c.x;
        w->y = c.y;
        w->z = c.z;
        w->npc.source = world::Source::Captured;
        w->npc.home_x = c.x;
        w->npc.home_y = c.y;
        w->npc.home_z = c.z;
        const std::string path = c.open->archetype_path;
        if (!registry.add(std::move(*w))) {
            ++report.refused;
            note("a captured spawn could not be added to the registry");
            continue;
        }
        if (npc) {
            placed.push_back({c.x, c.y, path});
            ++report.captured_npcs;
            ++report.npcs_by_archetype[leaf_of(path)];
        } else {
            nodes.push_back({c.x, c.y, path});
            ++report.gatherables;
        }
    }

    // the GM vendors
    if (sources.gm) {
        const OpenTemplate* tpl = sources.captured->template_for(kGmVendorArchetype);
        if (!tpl) {
            note(std::string("the vendor archetype ") + kGmVendorArchetype + " has no capture: no GM vendors");
        } else {
            const auto& defs = sources.gm->vendors();
            const std::vector<vendor::GmPlacement> layout = vendor::gm_layout(defs.size(), area, kLoginSpotYaw, kLoginSpotZ);
            for (size_t i = 0; i < defs.size(); ++i) {
                InstanceParams p;
                p.x = layout[i].x;
                p.y = layout[i].y;
                p.z = layout[i].z;
                p.yaw_degrees = layout[i].yaw_degrees;
                p.server_id = server_id;
                p.world_time = world_time;
                p.name = defs[i].label;
                p.clear_population = true;   // the records of the retail "Siege Vendor" would put its name over this one on the client

                p.seed = mix(0x6D5E11ull ^ static_cast<uint64_t>(defs[i].guid));
                std::string why;
                auto w = instantiate(*tpl, mint, p, why);
                if (!w) {
                    ++report.refused;
                    note("the GM vendor " + defs[i].label + " was refused: " + why);
                    continue;
                }
                // the client offers an interaction only for an input the server allows (UInteractableComponent::CanInputActivate, replicated, in no
                // captured NPC open): the four inputs are allowed.  The per-player part of the gate, the player's guid in the two state maps, is the
                // actor view's (world/interaction_state.h)
                if (!world::allow_inputs(w->actor)) {
                    ++report.refused;
                    note("the GM vendor " + defs[i].label + " has no interactable component to allow inputs on");
                    continue;
                }
                w->npc.source = world::Source::Runtime;
                w->npc.name = defs[i].label;
                w->npc.vendor_guid = defs[i].guid;
                w->npc.is_gm_vendor = true;
                w->npc.home_x = layout[i].x;
                w->npc.home_y = layout[i].y;
                w->npc.home_z = layout[i].z;
                if (!registry.add(std::move(*w))) {
                    ++report.refused;
                    note("the GM vendor " + defs[i].label + " could not be added to the registry");
                    continue;
                }
                ++report.gm_vendors;
            }
        }
    }
    return report;
}

}  // namespace aoc::worldd::spawns
