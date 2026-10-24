// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  spawns/node_population.cpp (see node_population.h)
// ============================================================================
#include "spawns/node_population.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace aoc::worldd::spawns {

namespace {

struct Stmt {
    sqlite3_stmt* s = nullptr;
    ~Stmt() {
        if (s) sqlite3_finalize(s);
    }
};

constexpr double kPi = 3.14159265358979323846;
constexpr uint64_t kGolden = 0x9E3779B97F4A7C15ull;

}  // namespace

const char* node_level_name(int level) {
    static const char* const kNames[] = {"Wilderness", "Crossroads", "Encampment", "Village", "Town", "City", "Metropolis"};
    return level >= 0 && level < 7 ? kNames[level] : "";
}

const char* node_type_name(int type) {
    static const char* const kNames[] = {"Default", "Scientific", "Divine", "Martial", "Economic"};
    return type >= 0 && type < 5 ? kNames[type] : "";
}

const char* biome_name(int biome) {
    static const char* const kNames[] = {"Default", "AlpineForest", "AncientForest", "ArcticTundra", "Asian", "Badlands", "Desert", "Fjords", "Glacier", "Grasslands", "Iceland",
                                         "Jungle", "MagicalForest", "RedwoodForest", "Riverlands", "Swamp", "Taiga", "TabletopMountains", "Tropics", "Volcano", "UnderRealm", "Highlands"};
    return biome >= 0 && biome < 22 ? kNames[biome] : "";
}

double quaternion_yaw(double qx, double qy, double qz, double qw) {
    return std::atan2(2.0 * (qw * qz + qx * qy), 1.0 - 2.0 * (qy * qy + qz * qz)) * 180.0 / kPi;
}

std::unique_ptr<NodePopulationData> NodePopulationData::load(const std::string& path, std::string& error) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        error = "cannot open " + path + ": " + (db ? sqlite3_errmsg(db) : "out of memory");
        if (db) sqlite3_close(db);
        return nullptr;
    }
    struct Closer {
        sqlite3* db;
        ~Closer() { sqlite3_close(db); }
    } closer{db};
    auto out = std::unique_ptr<NodePopulationData>(new NodePopulationData());
    const auto prepare = [&](Stmt& st, const char* sql, const char* what) {
        if (sqlite3_prepare_v2(db, sql, -1, &st.s, nullptr) == SQLITE_OK) return true;
        error = path + ": " + what + " cannot be read (" + sqlite3_errmsg(db) + "): it was built by a contentc before version 12";
        return false;
    };
    {
        Stmt st;
        if (!prepare(st, "SELECT layout_guid, population_set_guid FROM node_layout_population ORDER BY layout_guid, ord", "the layouts' population sets")) return nullptr;
        while (sqlite3_step(st.s) == SQLITE_ROW) out->layout_sets_[sqlite3_column_int64(st.s, 0)].push_back(sqlite3_column_int64(st.s, 1));
    }
    {
        Stmt st;
        if (!prepare(st, "SELECT building_guid, level, spawn_guid FROM building_pop_spawn ORDER BY building_guid, level", "the buildings' populations")) return nullptr;
        while (sqlite3_step(st.s) == SQLITE_ROW) out->building_spawns_[sqlite3_column_int64(st.s, 0)][sqlite3_column_int(st.s, 1)] = sqlite3_column_int64(st.s, 2);
    }
    {
        Stmt st;
        if (!prepare(st, "SELECT spawn_guid, state, population_set_guid FROM building_level_spawn_pop ORDER BY spawn_guid, state, ord", "the buildings' population sets")) return nullptr;
        while (sqlite3_step(st.s) == SQLITE_ROW) out->spawn_sets_[{sqlite3_column_int64(st.s, 0), sqlite3_column_int(st.s, 1)}].push_back(sqlite3_column_int64(st.s, 2));
    }
    return out;
}
// [Cyb ~ C O A]

std::unique_ptr<NodePopulationData> NodePopulationData::make(std::map<int64_t, std::vector<int64_t>> layout_sets) {
    auto out = std::unique_ptr<NodePopulationData>(new NodePopulationData());
    out->layout_sets_ = std::move(layout_sets);
    return out;
}

const std::vector<int64_t>& NodePopulationData::layout_sets(int64_t layout) const {
    const auto it = layout_sets_.find(layout);
    return it == layout_sets_.end() ? none_ : it->second;
}

int64_t NodePopulationData::spawn_for(int64_t building, int level) const {
    const auto it = building_spawns_.find(building);
    if (it == building_spawns_.end()) return 0;
    int64_t best = 0;
    for (const auto& [key, spawn] : it->second) {
        if (key <= level) best = spawn;   // (ascending keys: the last one at or below the level)
        else break;
    }
    return best;
}

std::vector<int> NodePopulationData::levels_of(int64_t building) const {
    std::vector<int> out;
    const auto it = building_spawns_.find(building);
    if (it != building_spawns_.end()) {
        for (const auto& [key, spawn] : it->second) out.push_back(key);
    }
    return out;
}

const std::vector<int64_t>& NodePopulationData::spawn_sets(int64_t spawn, int state) const {
    const auto it = spawn_sets_.find({spawn, state});
    return it == spawn_sets_.end() ? none_ : it->second;
}

NodeSite node_site_of(const world::CityNodes& nodes, const world::NodeWire& wire, const world::CityNodeInfo& info, int level) {
    NodeSite s;
    s.guid = info.guid;
    s.name = info.name;
    s.x = info.x;
    s.y = info.y;
    s.z = info.z;
    s.yaw = info.yaw;
    s.type = info.type;
    s.level = level;
    s.zoi = info.zoi_guid;
    if (const world::CityZoneInfo* z = nodes.zone_by_guid(info.zoi_guid)) s.biome = biome_name(z->biome);
    s.layout = wire.layout_of(info.guid);
    return s;
}

SpawnContext node_spawn_context(const SpawnContext& base, const NodeSite& site, const std::set<std::string>& shown) {
    SpawnContext c = base;
    c.node_level = node_level_name(site.level);
    c.node_id = site.guid;
    c.node_type = node_type_name(site.type);
    c.biome = site.biome;
    c.zone = site.zoi;
    c.active_gates.insert(kLayoutGate);
    c.active_gates.insert(kBuildingGate);
    c.shown_humanoids.insert(shown.begin(), shown.end());
    return c;
}
// (CoA : C_Y_B)

std::vector<PlacedSet> placed_sets(const NodePopulationData& data, const world::NodeWire& wire, const NodeSite& site) {
    std::vector<PlacedSet> out;
    for (const int64_t set : data.layout_sets(site.layout)) {
        PlacedSet p;
        p.placement = site.guid;
        p.set = set;
        p.x = site.x;
        p.y = site.y;
        p.z = site.z;
        p.yaw = site.yaw;
        p.source = PopSource::Layout;
        out.push_back(p);
    }
    const double yaw = site.yaw * kPi / 180.0;
    const double c = std::cos(yaw), s = std::sin(yaw);
    for (const world::PlotRow& plot : wire.plot_rows_of(site.guid, site.level)) {
        if (plot.building == 0 || (plot.state != 1 && plot.state != 2)) continue;
        const int64_t spawn = data.spawn_for(plot.building, site.level);
        if (spawn == 0) continue;
        const double x = site.x + plot.tx * c - plot.ty * s;
        const double y = site.y + plot.tx * s + plot.ty * c;
        const double plot_yaw = site.yaw + quaternion_yaw(plot.qx, plot.qy, plot.qz, plot.qw);
        const int64_t placement = static_cast<int64_t>(static_cast<uint64_t>(site.guid) ^ (static_cast<uint64_t>(plot.plot) * kGolden));
        for (const int64_t set : data.spawn_sets(spawn, plot.state)) {
            PlacedSet p;
            p.placement = placement;
            p.set = set;
            p.x = x;
            p.y = y;
            p.z = site.z + plot.tz;
            p.yaw = plot_yaw;
            p.source = PopSource::Building;
            p.plot = plot.plot;
            out.push_back(p);
        }
    }
    return out;
}

std::vector<NodeSpawn> select_node_spawns(const content::PopulationData& population, const TemplateSource& templates, const NodePopulationData& data, const world::NodeWire& wire,
                                          const NodeSite& site, const AreaConfig& area, const SpawnFilter& filter, NodePopStats& stats) {
    std::vector<NodeSpawn> out;
    for (const PlacedSet& ps : placed_sets(data, wire, site)) {
        const size_t src = ps.source == PopSource::Layout ? 0 : 1;
        const auto set_it = population.sets.find(ps.set);
        if (set_it == population.sets.end()) {
            ++stats.missing_sets;
            continue;
        }
        const content::NpcSetRow& set = set_it->second;
        content::NpcPlacement placement;
        placement.spawn = ps.placement;
        placement.set = ps.set;
        placement.x = ps.x;
        placement.y = ps.y;
        placement.z = ps.z;
        placement.yaw = ps.yaw;
        std::vector<Point> points;
        bool from_area = false;
        if (set.area != 0) {
            const auto area_it = population.areas.find(set.area);
            if (area_it == population.areas.end()) continue;
            points = area_spawn_points(placement, set, area_it->second);
            from_area = true;
        } else {
            points = placement_points(placement, set);
        }
        ++stats.sets[src];
        for (const Point& pt : points) {
            ++stats.points[src];
            if (!area.contains(pt.x, pt.y)) {
                ++stats.outside;
                continue;
            }
            const auto inst_it = population.instances.find(pt.instance);
            if (inst_it == population.instances.end()) continue;
            DataSpawn spawn;
            if (!spawn_point(population, templates, &filter, nullptr, set, inst_it->second, pt, ps.placement, 0, 0, 0, from_area, stats.pop, spawn)) continue;
            NodeSpawn n;
            n.spawn = spawn;
            n.source = ps.source;
            n.plot = ps.plot;
            n.ident = point_hash(0, ps.set, ps.placement, pt.key);
            out.push_back(n);
            ++stats.spawned[src];
        }
    }
    return out;
}

namespace {

/// An asset's names a game master may call a station by, lower case: the asset's name without its spawn suffix, and the Blueprint's leaf.
std::vector<std::string> station_names_of(const content::NpcAssetRow& asset) {
    const auto lowered = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    std::string name = asset.name;
    const std::string tail = "_SpawnAsset";
    if (name.size() > tail.size() && name.compare(name.size() - tail.size(), tail.size(), tail) == 0) name.resize(name.size() - tail.size());
    std::vector<std::string> out{lowered(name)};
    const size_t dot = asset.archetype_path.rfind('.');
    std::string leaf = asset.archetype_path.substr(dot == std::string::npos ? 0 : dot + 1);
    if (leaf.rfind("Default__", 0) == 0) leaf = leaf.substr(9);
    if (leaf.size() > 2 && leaf.compare(leaf.size() - 2, 2, "_C") == 0) leaf.resize(leaf.size() - 2);
    if (!leaf.empty()) out.push_back(lowered(leaf));
    return out;
}

bool is_station_asset(const content::NpcAssetRow& asset) {
    return asset.archetype_path.find("/Artisanship/") != std::string::npos &&
           (asset.archetype_path.find("CraftingStation_") != std::string::npos || asset.archetype_path.find("ProcessingStation_") != std::string::npos);
}

}  // namespace

std::optional<NodeSpawn> select_station_on_plot(const content::PopulationData& population, const TemplateSource& templates, const NodePopulationData& data, const NodeSite& site,
                                                const world::PlotRow& plot, const std::vector<int64_t>& buildings, const std::string& station, int64_t& building, std::string& why) {
    std::string want;
    for (const char c : station) want.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    const double yaw = site.yaw * kPi / 180.0;
    const double c = std::cos(yaw), s = std::sin(yaw);
    const double x = site.x + plot.tx * c - plot.ty * s;
    const double y = site.y + plot.tx * s + plot.ty * c;
    const double plot_yaw = site.yaw + quaternion_yaw(plot.qx, plot.qy, plot.qz, plot.qw);
    const int64_t placement = static_cast<int64_t>(static_cast<uint64_t>(site.guid) ^ (static_cast<uint64_t>(plot.plot) * kGolden));
    bool any_station = false;
    for (const int64_t b : buildings) {
        const int64_t spawn = data.spawn_for(b, site.level);
        if (spawn == 0) continue;
        for (const int64_t set_guid : data.spawn_sets(spawn, 2)) {
            const auto set_it = population.sets.find(set_guid);
            if (set_it == population.sets.end() || set_it->second.area != 0) continue;
            content::NpcPlacement at;
            at.spawn = placement;
            at.set = set_guid;
            at.x = x;
            at.y = y;
            at.z = site.z + plot.tz;
            at.yaw = plot_yaw;
            for (const Point& pt : placement_points(at, set_it->second)) {
                const auto inst_it = population.instances.find(pt.instance);
                if (inst_it == population.instances.end()) continue;
                PopulationStats stats;
                DataSpawn spawn_out;
                if (!spawn_point(population, templates, nullptr, nullptr, set_it->second, inst_it->second, pt, placement, 0, 0, 0, false, stats, spawn_out) || !spawn_out.asset) continue;
                if (!is_station_asset(*spawn_out.asset)) continue;
                any_station = true;
                for (const std::string& name : station_names_of(*spawn_out.asset)) {
                    if (name != want) continue;
                    NodeSpawn out;
                    out.spawn = spawn_out;
                    out.source = PopSource::Building;
                    out.plot = plot.plot;
                    out.ident = point_hash(0, set_guid, placement, pt.key);
                    building = b;
                    return out;
                }
            }
        }
    }
    why = any_station ? "no building that can stand on this plot has a station named " + station : "no building that can stand on this plot has a station at the node's level";
    return std::nullopt;
}

std::vector<std::string> stations_on_plot(const content::PopulationData& population, const NodePopulationData& data, const NodeSite& site, const std::vector<int64_t>& buildings) {
    std::vector<std::string> out;
    for (const int64_t b : buildings) {
        const int64_t spawn = data.spawn_for(b, site.level);
        if (spawn == 0) continue;
        for (const int64_t set_guid : data.spawn_sets(spawn, 2)) {
            const auto set_it = population.sets.find(set_guid);
            if (set_it == population.sets.end() || set_it->second.area != 0) continue;
            for (const content::NpcSetMember& m : set_it->second.members) {
                const auto inst_it = population.instances.find(m.instance);
                if (inst_it == population.instances.end()) continue;
                for (const int64_t as : inst_it->second.asset_sets) {
                    const auto set_assets = population.asset_sets.find(as);
                    if (set_assets == population.asset_sets.end()) continue;
                    for (const auto& [asset_guid, weight] : set_assets->second) {
                        const auto a = population.assets.find(asset_guid);
                        if (a == population.assets.end() || !is_station_asset(a->second)) continue;
                        std::string name = a->second.name;
                        const std::string tail = "_SpawnAsset";
                        if (name.size() > tail.size() && name.compare(name.size() - tail.size(), tail.size(), tail) == 0) name.resize(name.size() - tail.size());
                        if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
                    }
                }
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace aoc::worldd::spawns
