// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  quest/registry_points.h  (quests milestone, "map markers")
//
//  The places of the world's people and creatures, for the map markers of an objective (quest/markers.h):
//    * the people (an NPC a Dialogue objective names, an instance an objective tracks) stand where the actor registry has them: the spot each spawned at, which is its place in the
//      data.  An instance the registry does not have (a prop this server does not make, a person outside its area) is marked where the population data places it;
//    * the creatures of a Kill objective are the spawn points of the population the objective names, read from the population data (a set's placements, each with the offset of
//      each member, and the placements of the sets that hold a named instance): the points do not depend on which of them are alive or filtered out of this server's area.
//  The registry and the population data are read once per object, the first time a place is asked for, and the places are kept in a fixed order, so the same world gives the same
//  markers.  A set whose creatures spawn at random in an area has no fixed point and gives none.
// ============================================================================
#pragma once

#include "content/content_db.h"
#include "quest/markers.h"
#include "quest/quest_book.h"
#include "world/actor_registry.h"

#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace aoc::worldd::quest {

class RegistryPoints final : public PointSource {
public:
    /// `population` may be null (then no creature, and no person the registry lacks, gets a place).
    RegistryPoints(const world::ActorRegistry& registry, const content::PopulationData* population) : registry_(registry), population_(population) {}

    void npc_places(int64_t instance, std::vector<MarkerPoint>& out) override {
        build_live();
        const auto it = live_.find(instance);
        if (it != live_.end() && !it->second.empty()) {
            out.insert(out.end(), it->second.begin(), it->second.end());
            return;
        }
        // not in the registry: where the data places it
        std::set<int64_t> one{instance};
        const size_t first = out.size();
        data_places(one, {}, /*named_sets=*/false, out);
        sort_from(out, first);
    }

    void creature_places(const std::vector<int64_t>& instances, const std::vector<int64_t>& sets, std::vector<MarkerPoint>& out) override {
        const size_t first = out.size();
        data_places(std::set<int64_t>(instances.begin(), instances.end()), std::set<int64_t>(sets.begin(), sets.end()), /*named_sets=*/true, out);
        sort_from(out, first);
    }

private:
    static void sort_from(std::vector<MarkerPoint>& pts, size_t first) {
        std::sort(pts.begin() + static_cast<std::ptrdiff_t>(first), pts.end(), [](const MarkerPoint& a, const MarkerPoint& b) {
            if (a.x != b.x) return a.x < b.x;
            if (a.y != b.y) return a.y < b.y;
            return a.z < b.z;
        });
    }

    void build_live() {
        if (live_built_) return;
        live_built_ = true;
        registry_.for_each([&](const world::WorldActor& a) {
            if (a.kind != world::ActorKind::Npc || a.npc.instance_guid == 0) return;
            live_[a.npc.instance_guid].push_back({a.npc.home_x, a.npc.home_y, a.z});   // (the place it spawned at: a walker is marked where it lives)
        });
        for (auto& [instance, places] : live_) sort_from(places, 0);
    }

    /// The placements of each set that count (the open world's, none at the origin), indexed once.
    void build_data() {
        if (data_built_ || !population_) return;
        data_built_ = true;
        const content::PopulationData& pop = *population_;
        for (const content::NpcPlacement& p : pop.placements) {
            if (p.x == 0 && p.y == 0 && p.z == 0) continue;   // a template (the people of a node's building): its place comes from a node's layout
            if (const auto w = pop.world_spawns.find(p.spawn); w != pop.world_spawns.end() && w->second.map_name != "Verra_World_Master") continue;   // (a quest's or an event's own map)
            placements_[p.set].push_back(&p);
        }
        for (const auto& [guid, row] : pop.sets) {
            for (const content::NpcSetMember& m : row.members) holders_[m.instance].push_back({guid, &m});
        }
    }

    /// The places the data gives: every member of each named set at each of its placements, and each named instance (with its own offset) at the placements of the sets that hold it.
    void data_places(const std::set<int64_t>& instances, const std::set<int64_t>& sets, bool named_sets, std::vector<MarkerPoint>& out) {
        if (!population_) return;
        build_data();
        const content::PopulationData& pop = *population_;
        if (named_sets) {
            for (const int64_t s : sets) {
                const auto row = pop.sets.find(s);
                const auto pl = placements_.find(s);
                if (row == pop.sets.end() || pl == placements_.end()) continue;
                for (const content::NpcPlacement* p : pl->second) {
                    for (const content::NpcSetMember& m : row->second.members) out.push_back({p->x + m.x, p->y + m.y, p->z + m.z});
                }
            }
        }
        for (const int64_t instance : instances) {
            const auto held = holders_.find(instance);
            if (held == holders_.end()) continue;
            for (const auto& [set, member] : held->second) {
                if (named_sets && sets.count(set)) continue;   // (the whole set was added above)
                const auto pl = placements_.find(set);
                if (pl == placements_.end()) continue;
                for (const content::NpcPlacement* p : pl->second) out.push_back({p->x + member->x, p->y + member->y, p->z + member->z});
            }
        }
    }

    const world::ActorRegistry&                  registry_;
    const content::PopulationData*               population_;
    bool                                         live_built_ = false, data_built_ = false;
    std::map<int64_t, std::vector<MarkerPoint>>  live_;                                   // instance -> where its spawned NPCs stand
    std::map<int64_t, std::vector<const content::NpcPlacement*>> placements_;             // set -> the placements that count
    std::map<int64_t, std::vector<std::pair<int64_t, const content::NpcSetMember*>>> holders_;   // instance -> (set, member) for every set that holds it
};

}  // namespace aoc::worldd::quest
