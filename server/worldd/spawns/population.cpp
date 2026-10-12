// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  spawns/population.cpp
//
//  See population.h.
// ============================================================================
#include "spawns/population.h"

#include <algorithm>
#include <cmath>

namespace aoc::worldd::spawns {

namespace {

constexpr double kPi = 3.14159265358979323846;

uint64_t mix(uint64_t z) {
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

std::string leaf_of(const std::string& archetype_path) {
    std::string leaf = archetype_path.substr(archetype_path.rfind('.') == std::string::npos ? 0 : archetype_path.rfind('.') + 1);
    if (leaf.rfind("Default__", 0) == 0) leaf = leaf.substr(9);
    return leaf;
}

double unit(uint64_t h) { return static_cast<double>(h >> 11) / static_cast<double>(1ull << 53); }

}  // namespace

uint64_t point_hash(int64_t instance, int64_t set, int64_t placement, int64_t key) {
    uint64_t h = mix(static_cast<uint64_t>(instance));
    h = mix(h ^ static_cast<uint64_t>(set));
    h = mix(h ^ static_cast<uint64_t>(placement));
    h = mix(h ^ static_cast<uint64_t>(key));
    return h;
}
// ~ C.Y.B & C.O.A ~

std::vector<Point> placement_points(const content::NpcPlacement& placement, const content::NpcSetRow& set) {
    const double yaw = placement.yaw * kPi / 180.0;
    const double c = std::cos(yaw), s = std::sin(yaw);
    std::vector<Point> out;
    out.reserve(set.members.size());
    for (const content::NpcSetMember& m : set.members) {
        Point p;
        p.x = placement.x + m.x * c - m.y * s;
        p.y = placement.y + m.x * s + m.y * c;
        p.z = placement.z + m.z;
        p.yaw = placement.yaw + m.yaw;
        p.key = m.key;
        p.instance = m.instance;
        out.push_back(p);
    }
    return out;
}

const OpenTemplate* TemplateSource::find(const content::NpcAssetRow& asset, bool* is_generic, Verdict* why) const {
    if (is_generic) *is_generic = false;
    if (why) *why = Verdict::NoPath;
    if (asset.archetype_path.empty()) return nullptr;
    if (captured) {
        if (const OpenTemplate* t = captured->template_for(asset.archetype_path)) {
            if (why) *why = Verdict::Generic;
            return t;
        }
    }
    if (generic) {
        Verdict v = Verdict::NoTable;
        const OpenTemplate* t = generic->template_for(asset, &v);
        if (why) *why = v;
        if (t && is_generic) *is_generic = true;
        return t;
    }
    if (why) *why = Verdict::Disabled;
    return nullptr;
}

std::vector<Point> area_spawn_points(const content::NpcPlacement& placement, const content::NpcSetRow& set, const content::NpcAreaRow& area) {
    std::vector<Point> out;
    if (set.members.empty() || area.circles.empty()) return out;
    // how many the area keeps, from Min to Max
    const int lo = std::max(0, area.min_spawns), hi = std::max(lo, area.max_spawns);
    const uint64_t base = point_hash(set.guid, area.guid, placement.spawn, placement.key);
    const int count = lo + static_cast<int>(mix(base ^ 0xC0DE5ull) % static_cast<uint64_t>(hi - lo + 1));
    // a circle's chance: its weight times its area
    std::vector<double> weight(area.circles.size());
    for (size_t i = 0; i < area.circles.size(); ++i) {
        const content::NpcAreaCircle& c = area.circles[i];
        weight[i] = std::max(0.0, c.weight) * c.extent * c.extent;
    }
    std::vector<int64_t> used(area.circles.size(), 0);
    const auto has_room = [&](size_t i) { return area.circles[i].max_spawns < 0 || used[i] < area.circles[i].max_spawns; };
    for (int n = 0; n < count; ++n) {
        const uint64_t h = mix(base ^ (static_cast<uint64_t>(n + 1) * 0x9E3779B97F4A7C15ull));
        double total = 0;
        for (size_t i = 0; i < area.circles.size(); ++i) {
            if (has_room(i)) total += weight[i];
        }
        if (!(total > 0)) break;
        double r = unit(mix(h ^ 0x51Cull)) * total;
        size_t pick = area.circles.size();
        for (size_t i = 0; i < area.circles.size(); ++i) {
            if (!has_room(i) || weight[i] <= 0) continue;
            pick = i;
            if (r < weight[i]) break;
            r -= weight[i];
        }
        if (pick >= area.circles.size()) break;
        ++used[pick];
        const content::NpcAreaCircle& c = area.circles[pick];
        const double radius = c.extent * std::sqrt(unit(mix(h ^ 0xA1ull))), angle = unit(mix(h ^ 0xA2ull)) * 2.0 * kPi;
        const content::NpcSetMember& m = set.members[static_cast<size_t>(n) % set.members.size()];
        Point pt;
        pt.x = placement.x + c.x + radius * std::cos(angle);
        pt.y = placement.y + c.y + radius * std::sin(angle);
        pt.z = placement.z + c.z;
        pt.yaw = unit(mix(h ^ 0xA3ull)) * 360.0;
        pt.key = 0x10000 + n;       // distinct from every member's key
        pt.instance = m.instance;
        out.push_back(pt);
    }
    return out;
}

std::vector<RespawnPoint> respawn_points(const content::PopulationData& data, const AreaConfig& area) {
    std::vector<RespawnPoint> out;
    for (const content::NpcPlacement& placement : data.placements) {
        const auto ws = data.world_spawns.find(placement.spawn);
        if (ws == data.world_spawns.end() || ws->second.map_name != "Verra_World_Master") continue;
        const auto set_it = data.sets.find(placement.set);
        if (set_it == data.sets.end() || set_it->second.area != 0) continue;
        for (const Point& pt : placement_points(placement, set_it->second)) {
            if (!area.contains(pt.x, pt.y)) continue;
            const auto inst = data.instances.find(pt.instance);
            if (inst == data.instances.end()) continue;
            for (const int64_t set_guid : inst->second.asset_sets) {
                const auto as = data.asset_sets.find(set_guid);
                if (as == data.asset_sets.end()) continue;
                for (const auto& [asset_guid, weight] : as->second) {
                    const auto a = data.assets.find(asset_guid);
                    if (a == data.assets.end()) continue;
                    const std::string& path = a->second.archetype_path;
                    const bool attuneable = path.find("AttuneableCharacterRespawn") != std::string::npos;
                    const bool plain = path.find("/CharacterRespawnBP") != std::string::npos;
                    if (!attuneable && !plain) continue;
                    out.push_back({pt.x, pt.y, pt.z, pt.yaw, attuneable, a->second.name});
                }
            }
        }
    }
    std::sort(out.begin(), out.end(), [&](const RespawnPoint& a, const RespawnPoint& b) {
        return std::hypot(a.x - area.center_x, a.y - area.center_y) < std::hypot(b.x - area.center_x, b.y - area.center_y);
    });
    return out;
}

bool spawn_point(const content::PopulationData& data, const TemplateSource& templates, const SpawnFilter* filter, const content::NpcWorldSpawn* world_spawn, const content::NpcSetRow& set,
                 const content::NpcInstanceRow& inst, const Point& pt, int64_t placement_id, int min_level, int max_level, int64_t zoi, bool from_area, PopulationStats& stats,
                 DataSpawn& out) {
    if (filter) {
        if (const Why w = filter->point(data, world_spawn, set, inst); w != Why::Spawns) {
            ++stats.filtered[w];
            if (w == Why::Blacklisted) ++stats.blacklisted;
            return false;
        }
    } else if (set.blacklisted || inst.blacklisted) {
        ++stats.blacklisted;
        return false;
    }
    // the assets this point could be, with their weights, in the order of the instance's sets
    struct Candidate {
        const content::NpcAssetRow* asset;
        double                      weight;
        const OpenTemplate*         open;
        bool                        generic;
        Verdict                     why;
        Why                         filtered = Why::Spawns;   // the spawn filter's verdict on the asset
    };
    std::vector<Candidate> all, shown;
    for (const int64_t set_guid : inst.asset_sets) {
        const auto as = data.asset_sets.find(set_guid);
        if (as == data.asset_sets.end()) continue;
        for (const auto& [asset_guid, weight] : as->second) {
            const auto a = data.assets.find(asset_guid);
            if (a == data.assets.end() || a->second.archetype_path.empty()) continue;
            if (filter) {
                if (const Why w = filter->asset(data, world_spawn, set, inst, set_guid, a->second, zoi); w != Why::Spawns) {
                    Candidate cand{&a->second, weight > 0 ? weight : 1.0, nullptr, false, Verdict::NoPath, w};
                    all.push_back(cand);
                    continue;
                }
            }
            bool is_generic = false;
            Verdict why = Verdict::Generic;
            const OpenTemplate* open = templates.find(a->second, &is_generic, &why);
            all.push_back({&a->second, weight > 0 ? weight : 1.0, open, is_generic, why});
            if (open) shown.push_back(all.back());
        }
    }
    const uint64_t h = point_hash(inst.guid, set.guid, placement_id, pt.key);
    if (all.empty()) {
        ++stats.no_asset;
        return false;
    }
    if (shown.empty()) {
        // the filter's reason when every asset the point could be was filtered, else (some asset had no template) stage 2's accounting
        bool all_filtered = true;
        for (const Candidate& cd : all) all_filtered = all_filtered && cd.filtered != Why::Spawns;
        if (all_filtered) {
            ++stats.filtered[all.front().filtered];
            ++stats.filtered_by_blueprint[all.front().filtered][leaf_of(all.front().asset->archetype_path)];
            return false;
        }
        ++stats.no_template;
        std::vector<const Candidate*> unfiltered;
        for (const Candidate& cd : all) {
            if (cd.filtered == Why::Spawns) unfiltered.push_back(&cd);
        }
        const Candidate& pick = *unfiltered[static_cast<size_t>(h % unfiltered.size())];
        ++stats.missing[leaf_of(pick.asset->archetype_path)];
        ++stats.lost_by_verdict[pick.why];
        if (pick.why == Verdict::NotPawn) ++stats.lost_props[leaf_of(pick.asset->archetype_path)];
        if (pick.why == Verdict::Excluded) ++stats.lost_excluded[leaf_of(pick.asset->archetype_path)];
        return false;
    }
    double total = 0;
    for (const Candidate& c : shown) total += c.weight;
    double r = unit(mix(h ^ 0xA55E7ull)) * total;
    const Candidate* chosen = &shown.back();
    for (const Candidate& c : shown) {
        if (r < c.weight) {
            chosen = &c;
            break;
        }
        r -= c.weight;
    }
    DataSpawn s;
    s.x = pt.x;
    s.y = pt.y;
    s.z = pt.z;
    s.yaw = pt.yaw;
    int lo = inst.level_min, hi = inst.level_max;
    if (max_level > 0 || min_level > 0) {
        lo = min_level > 0 ? min_level : lo;
        hi = max_level > 0 ? max_level : hi;
    }
    if (hi < lo) hi = lo;
    s.level = lo > 0 ? lo + static_cast<int>(mix(h ^ 0x1E7E1ull) % static_cast<uint64_t>(hi - lo + 1)) : 0;
    s.asset = chosen->asset;
    s.instance = &inst;
    s.open = chosen->open;
    s.generic = chosen->generic;
    s.from_area = from_area;
    s.placement = placement_id;
    s.set = set.guid;
    s.key = pt.key;
    s.seed = h;
    s.zoi = static_cast<uint64_t>(zoi);
    out = s;
    if (zoi != 0) ++stats.by_zoi[static_cast<uint64_t>(zoi)];
    ++stats.spawned;
    if (from_area) ++stats.area_spawned;
    const std::string leaf = leaf_of(chosen->asset->archetype_path);
    ++stats.by_archetype[leaf];
    if (chosen->generic) {
        ++stats.generic_spawned;
        ++stats.generic_by_archetype[leaf];
    }
    return true;
}

std::vector<DataSpawn> select_data_spawns(const content::PopulationData& data, const TemplateSource& templates, const AreaConfig& area, PopulationStats& stats,
                                          const SpawnFilter* filter) {
    std::vector<DataSpawn> out;
    for (const content::NpcPlacement& placement : data.placements) {
        ++stats.placements;
        const auto set_it = data.sets.find(placement.set);
        if (set_it == data.sets.end()) continue;
        const content::NpcSetRow& set = set_it->second;
        std::vector<Point> points;
        bool from_area = false;
        if (set.area != 0) {
            // an area set: its instances are not at their offsets, the area's circles hold them
            const auto area_it = data.areas.find(set.area);
            if (area_it == data.areas.end()) continue;
            points = area_spawn_points(placement, set, area_it->second);
            from_area = true;
            bool any = false;
            for (const Point& pt : points) any = any || area.contains(pt.x, pt.y);
            if (any) ++stats.area_sets;
        } else {
            points = placement_points(placement, set);
        }
        for (const Point& pt : points) {
            if (!area.contains(pt.x, pt.y)) continue;
            ++(from_area ? stats.area_points : stats.points_in_area);
            const auto inst_it = data.instances.find(pt.instance);
            if (inst_it == data.instances.end()) continue;
            const int64_t zoi = static_cast<int64_t>(area.zoi_at(pt.x, pt.y));   // the zone of influence the point stands in (0: the area has none)
            const auto ws_it = data.world_spawns.find(placement.spawn);
            const content::NpcWorldSpawn* world_spawn = ws_it == data.world_spawns.end() ? nullptr : &ws_it->second;
            DataSpawn spawn;
            if (spawn_point(data, templates, filter, world_spawn, set, inst_it->second, pt, placement.spawn, placement.min_level, placement.max_level, zoi, from_area, stats, spawn)) out.push_back(spawn);
        }
    }
    return out;
}

}  // namespace aoc::worldd::spawns
