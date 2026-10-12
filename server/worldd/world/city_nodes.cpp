// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/city_nodes.cpp (see city_nodes.h)
// ============================================================================
#include "world/city_nodes.h"

#include "stats/stat_program.h"
#include "world/zoi_surface.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>

namespace aoc::worldd::world {

const char* level_source_name(LevelSource s) {
    switch (s) {
        case LevelSource::Default: return "default";
        case LevelSource::Seed: return "seed";
        case LevelSource::Service: return "citynode";
    }
    return "?";
}

namespace {

/// A name for comparing: lower case letters and digits only ("New Aela" = "NewAela" = "new_aela").
std::string normalize(const std::string& s) {
    std::string out;
    for (const unsigned char c : s) {
        if (std::isalnum(c)) out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

struct Stmt {
    sqlite3_stmt* s = nullptr;
    ~Stmt() {
        if (s) sqlite3_finalize(s);
    }
};

}  // namespace

CityNodes::CityNodes(std::vector<CityNodeInfo> nodes, std::vector<CityZoneInfo> zones) : nodes_(std::move(nodes)), zones_(std::move(zones)) {
    for (size_t i = 0; i < nodes_.size(); ++i) {
        nodes_[i].rank = static_cast<int>(i);
        by_guid_[nodes_[i].guid] = i;
    }
    for (size_t i = 0; i < zones_.size(); ++i) {
        zone_by_guid_[zones_[i].guid] = i;
        if (const CityNodeInfo* n = by_guid(zones_[i].node_guid)) zones_[i].node_rank = n->rank;
    }
    levels_ = std::make_unique<std::atomic<int>[]>(nodes_.size() + 1);
    for (size_t i = 0; i <= nodes_.size(); ++i) levels_[i].store(0);
    live_.resize(nodes_.size());
    for (size_t i = 0; i < nodes_.size(); ++i) live_[i].guid = nodes_[i].guid;
}
// C_Y_B captures, C O A code

std::unique_ptr<CityNodes> CityNodes::load(const std::string& path, std::string& error) {
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
    std::vector<CityNodeInfo> nodes;
    std::vector<CityZoneInfo> zones;
    {
        Stmt st;
        const char* sql = "SELECT rank, guid, name, node_name, node_type_ord, x, y, z, zoi_guid, levelable, map_name, yaw FROM city_node ORDER BY rank";
        if (sqlite3_prepare_v2(db, sql, -1, &st.s, nullptr) != SQLITE_OK) {
            error = path + " has no city_node table (" + sqlite3_errmsg(db) + "): it was built by a contentc before version 12";
            return nullptr;
        }
        while (sqlite3_step(st.s) == SQLITE_ROW) {
            CityNodeInfo n;
            n.rank = sqlite3_column_int(st.s, 0);
            n.guid = sqlite3_column_int64(st.s, 1);
            n.name = reinterpret_cast<const char*>(sqlite3_column_text(st.s, 2));
            n.display_name = reinterpret_cast<const char*>(sqlite3_column_text(st.s, 3));
            n.type = sqlite3_column_int(st.s, 4);
            n.x = sqlite3_column_double(st.s, 5);
            n.y = sqlite3_column_double(st.s, 6);
            n.z = sqlite3_column_double(st.s, 7);
            n.zoi_guid = sqlite3_column_int64(st.s, 8);
            n.levelable = sqlite3_column_int(st.s, 9) != 0;
            if (const unsigned char* map = sqlite3_column_text(st.s, 10)) n.map_name = reinterpret_cast<const char*>(map);
            n.yaw = sqlite3_column_double(st.s, 11);
            if (n.rank != static_cast<int>(nodes.size())) {
                error = path + ": the ranks of city_node are not 0.." + std::to_string(nodes.size()) + " in order (rank " + std::to_string(n.rank) + ")";
                return nullptr;
            }
            nodes.push_back(std::move(n));
        }
    }
    if (nodes.empty()) {
        error = path + ": city_node is empty";
        return nullptr;
    }
    {
        Stmt st;
        const char* sql = "SELECT guid, name, biome_ord, node_guid FROM city_zoi ORDER BY guid";
        if (sqlite3_prepare_v2(db, sql, -1, &st.s, nullptr) != SQLITE_OK) {
            error = path + " has no city_zoi table (" + sqlite3_errmsg(db) + ")";
            return nullptr;
        }
        while (sqlite3_step(st.s) == SQLITE_ROW) {
            CityZoneInfo z;
            z.guid = sqlite3_column_int64(st.s, 0);
            z.name = reinterpret_cast<const char*>(sqlite3_column_text(st.s, 1));
            z.biome = sqlite3_column_int(st.s, 2);
            z.node_guid = sqlite3_column_int64(st.s, 3);
            zones.push_back(std::move(z));
        }
    }
    return std::make_unique<CityNodes>(std::move(nodes), std::move(zones));
}

const CityNodeInfo* CityNodes::by_guid(int64_t guid) const {
    const auto it = by_guid_.find(guid);
    return it == by_guid_.end() ? nullptr : &nodes_[it->second];
}

const CityNodeInfo* CityNodes::by_name(const std::string& name) const {
    const std::string want = normalize(name);
    if (want.empty()) return nullptr;
    const CityNodeInfo* found = nullptr;
    for (const CityNodeInfo& n : nodes_) {
        std::string bare = n.name;
        if (bare.rfind("Verra_", 0) == 0) bare = bare.substr(6);
        if (normalize(n.name) == want || normalize(bare) == want || normalize(n.display_name) == want) {
            if (found && found != &n) return nullptr;   // two nodes answer to it
            found = &n;
        }
    }
    return found;
}

const CityZoneInfo* CityNodes::zone_by_guid(int64_t guid) const {
    const auto it = zone_by_guid_.find(guid);
    return it == zone_by_guid_.end() ? nullptr : &zones_[it->second];
}

NodeSite CityNodes::site_of_zoi(int64_t zoi_guid) const {
    NodeSite site;
    site.zoi_guid = zoi_guid;
    if (zoi_guid == 0) return site;
    if (const CityZoneInfo* z = zone_by_guid(zoi_guid)) {
        site.node_rank = z->node_rank;
        site.biome = z->biome;
    }
    return site;
}

NodeSite CityNodes::node_at(double x_cm, double y_cm) const { return site_of_zoi(static_cast<int64_t>(zoi_at(x_cm, y_cm))); }

int CityNodes::level_of_rank(int rank) const {
    if (rank < 0 || static_cast<size_t>(rank) >= nodes_.size()) return 0;
    return levels_[static_cast<size_t>(rank)].load();
}

int CityNodes::level_of(int64_t guid) const {
    const CityNodeInfo* n = by_guid(guid);
    return n ? level_of_rank(n->rank) : 0;
}

int CityNodes::type_of_rank(int rank) const {
    const CityNodeInfo* n = by_rank(rank);
    return n ? n->type : 0;
}

std::vector<LiveNode> CityNodes::live() const {
    std::lock_guard<std::mutex> lock(live_mu_);
    std::vector<LiveNode> out = live_;
    for (size_t i = 0; i < out.size(); ++i) out[i].level = levels_[i].load();
    return out;
}
// built by C_Y_B with CoA

void CityNodes::set_all(const std::vector<LiveNode>& nodes, LevelSource source) {
    std::lock_guard<std::mutex> lock(live_mu_);
    // The new state is made first and stored node by node: the people, the scenery and the roads read the levels with no lock, acting on the version they saw, and a reader of an
    // EARLIER version must never find a node at Wilderness that this snapshot leaves where it was (a stream that reconnects sends what is already held; Nodes 1 review A, M4).
    std::vector<LiveNode> fresh;
    fresh.reserve(nodes_.size());
    for (size_t i = 0; i < nodes_.size(); ++i) fresh.push_back(LiveNode{nodes_[i].guid, 0, 0, 0});
    for (const LiveNode& n : nodes) {
        const CityNodeInfo* info = by_guid(n.guid);
        if (!info) continue;   // a node this content.db does not know
        const size_t i = static_cast<size_t>(info->rank);
        fresh[i] = n;
        fresh[i].level = std::clamp(n.level, 0, kMaxLevel);
    }
    for (size_t i = 0; i < nodes_.size(); ++i) {
        live_[i] = fresh[i];
        levels_[i].store(fresh[i].level);
    }
    source_.store(source);
    version_.fetch_add(1);
}

void CityNodes::apply_snapshot(const std::vector<LiveNode>& nodes, LevelSource source) { set_all(nodes, source); }

bool CityNodes::apply_change(const LiveNode& node) {
    const CityNodeInfo* info = by_guid(node.guid);
    if (!info) return false;
    std::lock_guard<std::mutex> lock(live_mu_);
    const size_t i = static_cast<size_t>(info->rank);
    live_[i] = node;
    live_[i].level = std::clamp(node.level, 0, kMaxLevel);
    levels_[i].store(live_[i].level);
    source_.store(LevelSource::Service);
    version_.fetch_add(1);
    return true;
}

bool CityNodes::apply_seed_file(const std::string& path, std::string& error, size_t* applied, size_t* skipped) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "cannot read " + path;
        return false;
    }
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(f);
    } catch (const std::exception& e) {
        error = path + " is no JSON: " + e.what();
        return false;
    }
    if (!j.is_object() || !j.contains("nodes") || !j["nodes"].is_object()) {
        error = path + " is no node seed: it has no \"nodes\" object";
        return false;
    }
    std::vector<LiveNode> live;
    std::map<int64_t, std::string> parents;
    size_t ok = 0, bad = 0;
    for (auto it = j["nodes"].begin(); it != j["nodes"].end(); ++it) {
        const CityNodeInfo* n = by_name(it.key());
        const auto& e = it.value();
        if (!n || !e.is_object() || !e.contains("level") || !e["level"].is_number_integer()) {
            ++bad;
            continue;
        }
        const int level = e["level"].get<int>();
        if (level < 0 || level > kMaxLevel) {
            ++bad;
            continue;
        }
        LiveNode ln;
        ln.guid = n->guid;
        ln.level = level;
        live.push_back(ln);
        if (e.contains("parent") && e["parent"].is_string()) parents[n->guid] = e["parent"].get<std::string>();
        ++ok;
    }
    for (LiveNode& ln : live) {
        const auto p = parents.find(ln.guid);
        if (p == parents.end()) continue;
        if (const CityNodeInfo* lord = by_name(p->second)) ln.parent_guid = lord->guid;
    }
    set_all(live, LevelSource::Seed);
    if (applied) *applied = ok;
    if (skipped) *skipped = bad;
    return true;
}
// CoA + C.Y.B

std::string CityNodes::describe_levels() const {
    std::ostringstream out;
    bool first = true;
    for (size_t i = 0; i < nodes_.size(); ++i) {
        const int level = levels_[i].load();
        if (level <= 0) continue;
        out << (first ? "" : ", ") << nodes_[i].display_name << " " << level;
        first = false;
    }
    return first ? "every node at Wilderness" : out.str();
}

bool node_host_fn(const CityNodes* nodes, const NodeContext& ctx, int fn, const double* args, int n, double& out) {
    // the rank an argument names: a whole number inside the registry, else -1 (no node)
    auto rank_arg = [&](int i) -> int {
        if (i >= n) return -1;
        const double a = args[i];
        const int64_t size = nodes ? static_cast<int64_t>(nodes->size()) : 0;
        if (!(a >= 0 && a < static_cast<double>(size))) return -1;
        const int r = static_cast<int>(a);
        return static_cast<double>(r) == a ? r : -1;
    };
    switch (fn) {
        case stats::kHostNodeBiome: out = ctx.biome; return true;
        case stats::kHostCurrentNode:
            out = (n >= 1 && args[0] == stats::kActorPlayer) ? ctx.player_node : ctx.actor_node;
            return true;
        case stats::kHostNodeRecord: out = ctx.node; return true;
        case stats::kHostNodeType: {
            const int r = rank_arg(0);
            out = r < 0 ? 0 : nodes->type_of_rank(r);
            return true;
        }
        case stats::kHostNodeLevel: {
            const int r = rank_arg(0);
            out = r < 0 ? 0 : nodes->level_of_rank(r);
            return true;
        }
        case stats::kHostPlayerCitizen:
        case stats::kHostPlayerCitizenOfNode:
        case stats::kHostPlayerMayor: out = 0; return true;   // until the citizenship milestone
        default: return false;
    }
}

}  // namespace aoc::worldd::world
