// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/node_wire.cpp (see node_wire.h)
// ============================================================================
#include "world/node_wire.h"

#include "world/node_constants.h"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace aoc::worldd::world {

namespace repl = ::aoc::protocol::repl;

namespace {

struct Stmt {
    sqlite3_stmt* s = nullptr;
    ~Stmt() {
        if (s) sqlite3_finalize(s);
    }
};

}  // namespace

bool NodeWire::is_test_estate(const std::string& n) { return n.rfind("Test_", 0) == 0 || n.find("Playground") != std::string::npos; }

int NodeWire::percent_of(int level, int64_t experience) {
    using namespace node_constants;
    if (level < 0 || level >= kMaxNodeLevel || static_cast<size_t>(level) + 1 >= kExpToLevel.size()) return 0;
    const int64_t lo = kExpToLevel[static_cast<size_t>(level)], hi = kExpToLevel[static_cast<size_t>(level) + 1];
    if (experience <= lo) return 0;
    if (experience >= hi) return 100;
    return static_cast<int>((experience - lo) * 100 / (hi - lo));
}

std::unique_ptr<NodeWire> NodeWire::load(const std::string& path, const CityNodes& nodes, std::string& error) {
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
    auto out = std::unique_ptr<NodeWire>(new NodeWire());
    out->nodes_ = &nodes;

    const auto prepare = [&](Stmt& st, const char* sql, const char* what) {
        if (sqlite3_prepare_v2(db, sql, -1, &st.s, nullptr) == SQLITE_OK) return true;
        error = path + ": " + what + " cannot be read (" + sqlite3_errmsg(db) + "): it was built by a contentc before version 12";
        return false;
    };
// C_Y_B x CoA

    // the layout of each node, and whether the record is a real node of a map
    {
        Stmt st;
        if (!prepare(st, "SELECT guid, layout_guid FROM city_node", "city_node")) return nullptr;
        while (sqlite3_step(st.s) == SQLITE_ROW) {
            const int64_t guid = sqlite3_column_int64(st.s, 0);
            const CityNodeInfo* info = nodes.by_guid(guid);
            if (!info) continue;
            NodeData d;
            d.real = !info->map_name.empty();
            d.layout = d.real ? sqlite3_column_int64(st.s, 1) : 0;
            out->by_node_[guid] = std::move(d);
        }
    }
    for (const CityNodeInfo& n : nodes.nodes()) {
        if (n.name.rfind("Verra_", 0) == 0 && !n.map_name.empty()) out->roster_.push_back(n.rank);
    }
    // what each level grants
    {
        Stmt st;
        const char* sql =
            "SELECT l.node_guid, l.level, COALESCE(SUM(m.relic_slots), 0), COALESCE(SUM(m.node_essence), 0), COALESCE(MAX(m.vehicle_deploy_radius), 0) "
            "FROM city_node_level l JOIN node_level_details_mod d ON d.details_guid = l.details_guid JOIN node_modification m ON m.guid = d.mod_guid "
            "GROUP BY l.node_guid, l.level ORDER BY l.node_guid, l.level";
        if (!prepare(st, sql, "the node level details")) return nullptr;
        while (sqlite3_step(st.s) == SQLITE_ROW) {
            const auto it = out->by_node_.find(sqlite3_column_int64(st.s, 0));
            if (it == out->by_node_.end()) continue;
            LevelGrants g;
            g.level = sqlite3_column_int(st.s, 1);
            g.relic_slots = sqlite3_column_int(st.s, 2);
            g.node_essence = sqlite3_column_int(st.s, 3);
            g.vehicle_deploy_radius = static_cast<float>(sqlite3_column_double(st.s, 4));
            it->second.grants.push_back(g);
        }
    }
    // the plots of each layout at each level
    {
        Stmt st;
        const char* sql =
            "SELECT p.layout_guid, p.level, p.plot_guid, COALESCE(sp.pre_construct, 0), COALESCE(sp.empty_building_guid, 0), "
            "(SELECT COUNT(*) FROM service_plot_building b WHERE b.plot_guid = p.plot_guid), "
            "COALESCE((SELECT b.building_guid FROM service_plot_building b WHERE b.plot_guid = p.plot_guid ORDER BY b.ord LIMIT 1), 0), "
            "p.qx, p.qy, p.qz, p.qw, p.tx, p.ty, p.tz, p.sx, p.sy, p.sz, COALESCE(sp.starting_state, 0), COALESCE(sp.name, '') "
            "FROM node_layout_plot p LEFT JOIN service_plot sp ON sp.guid = p.plot_guid ORDER BY p.layout_guid, p.level, p.idx";
        if (!prepare(st, sql, "the layout plots")) return nullptr;
        while (sqlite3_step(st.s) == SQLITE_ROW) {
            PlotRow r;
            const int64_t layout = sqlite3_column_int64(st.s, 0);
            const int level = sqlite3_column_int(st.s, 1);
            r.plot = sqlite3_column_int64(st.s, 2);
            r.pre_construct = sqlite3_column_int(st.s, 3) != 0;
            const int64_t empty_building = sqlite3_column_int64(st.s, 4);
            r.empty_building = empty_building;
            const int building_count = sqlite3_column_int(st.s, 5);
            const int64_t first_building = sqlite3_column_int64(st.s, 6);
            // the replicator's rule (spike derive_payload.py): the single building of the plot, else the plot's empty building, else the first
            r.building = building_count == 1 ? first_building : empty_building;
            if (r.building == 0 && building_count > 0) r.building = first_building;
            // A pre-built plot is Operational: the civic core, and the five small and medium NodeXP construction projects whose data start is UnderConstruction (the spike:
            // all six live nodes at level 3-4 show them finished).  The LARGE project stays as the data starts it, UnderConstruction: retail's summaries of Joeva, Winstead
            // and Halcyon say so (the three live Riverlands nodes), and its under-construction scaffold is filtered out of a Town's scenery.  The mayor's service plots start Empty.
            const char* plot_name = reinterpret_cast<const char*>(sqlite3_column_text(st.s, 18));
            const bool large_project = plot_name && std::string(plot_name).find("ConstructionProject_Large") != std::string::npos;
            r.name = plot_name ? plot_name : "";
            r.starting_state = static_cast<uint8_t>(sqlite3_column_int(st.s, 17));
            // Below Village (Crossroads and Encampment) nobody has built the small projects yet: they are the building sites the data starts them as.  From Village up this world
            // starts them finished.
            const bool building_site_level = level < node_constants::kMinimumOperationalNodeLevel;
            r.state = !r.pre_construct ? 0 : (large_project || building_site_level) ? r.starting_state : 2;
            r.qx = sqlite3_column_double(st.s, 7);
            r.qy = sqlite3_column_double(st.s, 8);
            r.qz = sqlite3_column_double(st.s, 9);
            r.qw = sqlite3_column_double(st.s, 10);
            r.tx = sqlite3_column_double(st.s, 11);
            r.ty = sqlite3_column_double(st.s, 12);
            r.tz = sqlite3_column_double(st.s, 13);
            r.sx = sqlite3_column_double(st.s, 14);
            r.sy = sqlite3_column_double(st.s, 15);
            r.sz = sqlite3_column_double(st.s, 16);
            out->plots_[{layout, level}].push_back(r);
        }
    }
    // the buildings each plot may carry (the mayor's service plots have fourteen)
    {
        Stmt st;
        if (!prepare(st, "SELECT b.plot_guid, b.building_guid, COALESCE(r.name, '') FROM service_plot_building b LEFT JOIN record r ON r.guid = b.building_guid ORDER BY b.plot_guid, b.ord",
                     "the plots' buildings"))
            return nullptr;
        while (sqlite3_step(st.s) == SQLITE_ROW) {
            Candidate c;
            c.building = sqlite3_column_int64(st.s, 1);
            const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st.s, 2));
            c.name = n ? n : "";
            out->candidates_[sqlite3_column_int64(st.s, 0)].push_back(std::move(c));
        }
    }
    // the estates
    {
        Stmt st;
        if (!prepare(st, "SELECT guid, name FROM record WHERE schema = 'FreeholdEstateRecordBase' ORDER BY guid", "the estate records")) return nullptr;
        while (sqlite3_step(st.s) == SQLITE_ROW) {
            const char* name = reinterpret_cast<const char*>(sqlite3_column_text(st.s, 1));
            if (is_test_estate(name ? name : "")) continue;
            out->estates_.push_back(sqlite3_column_int64(st.s, 0));
        }
    }
    if (out->roster_.empty()) {
        error = path + ": no Verra_* node with a map in city_node";
        return nullptr;
    }
    return out;
}

bool NodeWire::in_roster(int rank) const { return std::binary_search(roster_.begin(), roster_.end(), rank); }

const std::vector<PlotRow>& NodeWire::plot_rows(int64_t layout, int level) const {
    const auto it = plots_.find({layout, level});
    return it == plots_.end() ? none_ : it->second;
}

std::vector<PlotRow> NodeWire::plot_rows_of(int64_t node_guid, int level) const {
    std::vector<PlotRow> rows = data_plot_rows_of(node_guid, level);
    if (!nodes_) return rows;
    const std::vector<CityNodes::PlotLive> live = nodes_->live_plots(node_guid);
    if (live.empty()) return rows;
    for (PlotRow& row : rows) {
        const auto it = std::lower_bound(live.begin(), live.end(), row.plot, [](const CityNodes::PlotLive& l, int64_t p) { return l.plot < p; });
        if (it == live.end() || it->plot != row.plot) continue;
        row.state = it->state;
        row.building = it->building != 0 ? it->building : (row.empty_building != 0 ? row.empty_building : row.building);
        row.pre_construct = it->state != 0;   // something stands on it (the scenery sends only such plots)
        row.asset_sets = it->asset_sets;
        row.replace_assets = it->replace_assets;
        row.spawns = it->spawns;
    }
    return rows;
}

std::vector<PlotRow> NodeWire::data_plot_rows_of(int64_t node_guid, int level) const {
    std::vector<PlotRow> rows = plot_rows(layout_of(node_guid), level);
    if (!nodes_) return rows;
    for (const CityNodes::PlotChoice& choice : nodes_->plot_choices(node_guid)) {
        const std::string tail = "_" + choice.building + "_Building";
        for (PlotRow& row : rows) {
            if (row.name != choice.plot) continue;
            const auto cands = candidates_.find(row.plot);
            if (cands == candidates_.end()) break;
            for (const Candidate& c : cands->second) {
                if (c.name.size() < tail.size() || c.name.compare(c.name.size() - tail.size(), tail.size(), tail) != 0) continue;
                row.building = c.building;
                row.pre_construct = true;
                row.seeded = true;
                row.state = 2;
                row.starting_state = 2;
                break;
            }
            break;
        }
    }
    return rows;
}

std::vector<int64_t> NodeWire::plot_buildings(int64_t plot) const {
    std::vector<int64_t> out;
    const auto it = candidates_.find(plot);
    if (it == candidates_.end()) return out;
    for (const Candidate& c : it->second) out.push_back(c.building);
    return out;
}

std::string NodeWire::building_name(int64_t plot, int64_t building) const {
    const auto it = candidates_.find(plot);
    if (it == candidates_.end()) return std::string();
    for (const Candidate& c : it->second) {
        if (c.building == building) return c.name;
    }
    return std::string();
}

LevelGrants NodeWire::grants(int64_t node_guid, int level) const {
    LevelGrants best;
    best.level = -1;
    const auto it = by_node_.find(node_guid);
    if (it == by_node_.end()) return best;
    for (const LevelGrants& g : it->second.grants) {
        if (g.level <= level) best = g;
    }
    return best;
}

int64_t NodeWire::layout_of(int64_t node_guid) const {
    const auto it = by_node_.find(node_guid);
    return it == by_node_.end() ? 0 : it->second.layout;
}

bool NodeWire::is_real_node(int64_t node_guid) const {
    const auto it = by_node_.find(node_guid);
    return it != by_node_.end() && it->second.real;
}

namespace {

/// A character id (32 hex digits) as the FGuid's four words; all zero for one that does not read.
std::array<uint32_t, 4> guid_words(const std::string& hex) {
    std::array<uint32_t, 4> w{};
    if (hex.size() != 32) return w;
    for (size_t i = 0; i < 32; ++i) {
        const char c = hex[i];
        const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return std::array<uint32_t, 4>{};
        w[i / 8] = (w[i / 8] << 4) | static_cast<uint32_t>(d);
    }
    return w;
}

}  // namespace

repl::NodeStateMaps NodeWire::maps(const CityNodes& nodes, const Elections* elections) const {
    repl::NodeStateMaps m;
    const std::vector<LiveNode> live = nodes.live();
    for (const int rank : roster_) {
        const CityNodeInfo* info = nodes.by_rank(rank);
        if (!info) continue;
        const int level = nodes.level_of_rank(rank);
        const int64_t xp = static_cast<size_t>(rank) < live.size() ? live[static_cast<size_t>(rank)].experience : 0;
        repl::NodeDataEntry e;
        e.guid = info->guid;
        e.level = static_cast<uint8_t>(std::clamp(level, 0, 255));
        e.culture = 0;   // Kaelar: the export has only Kaelar building art, and every node of the captures says 0
        e.parent = static_cast<size_t>(rank) < live.size() ? live[static_cast<size_t>(rank)].parent_guid : 0;
        e.bounty = info->name == "Verra_Lawless_Ocean" ? 0 : kBountyCommodity;
        e.tags = {repl::kSiegeInactiveTag};
        e.deploy_radius = std::max(0.0f, grants(info->guid, level).vehicle_deploy_radius);
        const NodeElection* el = elections ? elections->node(info->guid) : nullptr;
        if (el && el->mayor.account != 0) {   // the node's mayor: the character's guid and name (the node window and the map name them)
            e.mayor_guid = guid_words(el->mayor.character_id);
            e.mayor_name = el->mayor.name;
        }
        m.nodes.push_back(std::move(e));
        if (level >= 1) m.percent.emplace_back(static_cast<uint32_t>(rank), static_cast<uint8_t>(percent_of(level, xp)));
        if (level >= 3) m.citizens.emplace_back(static_cast<uint32_t>(rank), static_cast<uint8_t>(std::min<size_t>(el ? el->citizens.size() : 0, 255)));
    }
    for (const int64_t g : estates_) m.estates.emplace_back(g, std::array<uint32_t, 4>{0, 0, 0, 0});
    return m;
}

std::vector<PlotInfo> NodeWire::plot_infos(int64_t node_guid, int level) const {
    std::vector<PlotInfo> out;
    for (const PlotRow& r : data_plot_rows_of(node_guid, level)) {
        PlotInfo p;
        p.plot = r.plot;
        p.building = r.building;
        p.state = r.state;
        p.empty_building = r.empty_building;
        p.pre_construct = r.pre_construct;
        p.seeded = r.seeded;
        p.candidates = plot_buildings(r.plot);
        out.push_back(std::move(p));
    }
    return out;
}

std::optional<repl::NodeSummary> NodeWire::summary(int64_t guid, const CityNodes& nodes, int64_t unix_now, const Elections* elections, int64_t requester_account) const {
    const CityNodeInfo* info = nodes.by_guid(guid);
    if (!info) return std::nullopt;
    repl::NodeSummary s;
    s.node = guid;
    if (!is_real_node(guid)) {
        // retail's default of a record that is no node (the dungeons, the jungle): culture 9, no layout, no plots, a mayor message that can never be edited
        s.culture = 9;
        s.next_mayor_message_edit_time = INT64_MAX;
        return s;
    }
    const int level = nodes.level_of_rank(info->rank);
    s.layout = layout_of(guid);
    s.level = level;
    s.culture = 0;
    const NodeElection* el = elections ? elections->node(guid) : nullptr;
    static const NodeBuildings kNoBuildings;
    const NodeBuildings& buildings = el ? el->buildings : kNoBuildings;
    namespace nb = node_buildings;
    const auto amounts = [](const std::map<int64_t, int32_t>& m) {
        std::vector<std::pair<int64_t, uint32_t>> out;
        for (const auto& [item, q] : m) {
            if (q > 0) out.emplace_back(item, static_cast<uint32_t>(q));
        }
        return out;
    };
    for (const PlotRow& r : plot_rows_of(guid, level)) {
        repl::PlotEntry e;
        e.plot = r.plot;
        e.building = r.building;
        e.state = r.state;
        if (const auto it = buildings.plots.find(r.plot); it != buildings.plots.end()) {
            const PlotState& p = it->second;
            int64_t ends_ms = 0;
            if (p.state == kPlotUnderConstruction) ends_ms = p.complete_ms != 0 ? p.complete_ms : p.phase_ends_ms;
            else if (p.state == kPlotDemolition || p.state == kPlotRepairing) ends_ms = p.phase_ends_ms;
            e.building_time_end = ends_ms / 1000;
            e.building_contribution = amounts(p.given);
            e.expansion_state = p.exp_state;
            if (const nb::TreeNode* t = nb::rank_node(p.exp_rank)) e.in_progress_expansion = t->expansion;
            e.expansion_project_time_end = p.exp_ends_ms / 1000;
            e.expansion_contribution = amounts(p.exp_given);
            for (const int64_t rank : p.ranks) {
                if (const nb::TreeNode* t = nb::rank_node(rank); t && t->expansion != 0) e.purchased_expansions.push_back(repl::RecordId{t->expansion, nb::kExpansionTypeId});
            }
            for (const auto& [account, yes] : p.votes) e.project_yes_votes += yes ? 1u : 0u;
            e.project_total_votes = static_cast<uint32_t>(p.votes.size());
            e.has_requester_voted = requester_account != 0 && p.votes.count(requester_account) != 0;
        }
        s.plots.push_back(e);
    }
    for (const auto& [type, q] : nb::skill_points(buildings, plot_infos(guid, level))) s.node_skill_points.emplace_back(repl::RecordId{type, nb::kSkillPointTypeTypeId}, q);
    s.time_until_next_maintenance = buildings.next_upkeep_ms != 0 ? buildings.next_upkeep_ms / 1000 : unix_now + 7 * 24 * 3600;
    s.next_mayor_message_edit_time = 60;   // retail's value for a node with no mayor
    if (el && el->mayor.account != 0) {
        s.mayor_message = el->mayor_message;
        s.next_mayor_message_edit_time = 0;   // the mayor may edit it now (ServerSetMayorMessage has no cooldown)
    }
    const LevelGrants g = grants(guid, level);
    if (g.level >= 0) {
        s.max_node_essence = static_cast<uint8_t>(std::clamp(g.node_essence, 0, 255));
        s.active_relic_slots = static_cast<uint8_t>(std::clamp(g.relic_slots, 0, 255));
    }
    return s;
}
// [C:Y:B ~ C.O.A]

std::optional<repl::SiegeSummary> NodeWire::siege(int64_t guid, const CityNodes& nodes) const {
    const CityNodeInfo* info = nodes.by_guid(guid);
    // retail answers the siege request of the nodes of a map only: the two dungeons and the jungle the client asks about get no answer (the retail data: 46 of the 49)
    if (!info || !is_real_node(guid)) return std::nullopt;
    return repl::idle_siege_summary(guid, info->display_name);
}

}  // namespace aoc::worldd::world
