// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/node_layout_data.cpp (see node_layout_data.h)
// ============================================================================
#include "world/node_layout_data.h"

#include <sqlite3.h>

namespace aoc::worldd::world {

namespace {

struct Stmt {
    sqlite3_stmt* s = nullptr;
    ~Stmt() {
        if (s) sqlite3_finalize(s);
    }
};

bool bit(int64_t mask, int n) { return n >= 0 && n < 63 && ((mask >> n) & 1) != 0; }

}  // namespace

std::unique_ptr<NodeLayoutData> NodeLayoutData::load(const std::string& path, std::string& error) {
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
    auto out = std::unique_ptr<NodeLayoutData>(new NodeLayoutData());

    const auto prepare = [&](Stmt& st, const char* sql, const char* what) {
        if (sqlite3_prepare_v2(db, sql, -1, &st.s, nullptr) == SQLITE_OK) return true;
        error = path + ": " + what + " cannot be read (" + sqlite3_errmsg(db) + "): it was built by a contentc before version 12";
        return false;
    };
// Cyb captures, C.O.A code

    {   // the props of each layout
        Stmt st;
        if (!prepare(st, "SELECT layout_guid, idx, asset_set_guid, qx, qy, qz, qw, tx, ty, tz, sx, sy, sz FROM node_layout_prop ORDER BY layout_guid, idx", "the layout props")) return nullptr;
        while (sqlite3_step(st.s) == SQLITE_ROW) {
            Prop p;
            const int64_t layout = sqlite3_column_int64(st.s, 0);
            p.idx = sqlite3_column_int(st.s, 1);
            p.asset_set = sqlite3_column_int64(st.s, 2);
            double qx = sqlite3_column_double(st.s, 3), qy = sqlite3_column_double(st.s, 4), qz = sqlite3_column_double(st.s, 5), qw = sqlite3_column_double(st.s, 6);
            if (qw < 0) {   // FQuat::NetSerialize saves the quaternion with W >= 0 (the sign of a rotation is free): the client derives W
                qx = -qx;
                qy = -qy;
                qz = -qz;
            }
            p.item.rotation = repl::Quat{qx, qy, qz};
            p.item.translation = repl::Vec3d{sqlite3_column_double(st.s, 7), sqlite3_column_double(st.s, 8), sqlite3_column_double(st.s, 9)};
            p.item.scale = repl::Vec3d{sqlite3_column_double(st.s, 10), sqlite3_column_double(st.s, 11), sqlite3_column_double(st.s, 12)};
            p.item.asset_set = p.asset_set;
            out->props_[layout].push_back(std::move(p));
        }
    }
    {   // the definitions of each asset set
        Stmt st;
        if (!prepare(st, "SELECT set_guid, def_guid FROM node_asset_set_member ORDER BY set_guid, ord", "the asset set members")) return nullptr;
        while (sqlite3_step(st.s) == SQLITE_ROW) out->members_[sqlite3_column_int64(st.s, 0)].push_back(sqlite3_column_int64(st.s, 1));
    }
    {   // the sets with no member are in the data too (they are never valid): nothing to keep
        Stmt st;
        if (!prepare(st, "SELECT guid, level_min, level_max, all_biomes, cultures, node_types, seasons, biomes FROM node_asset_def", "the asset definitions")) return nullptr;
        while (sqlite3_step(st.s) == SQLITE_ROW) {
            Def d;
            const int64_t guid = sqlite3_column_int64(st.s, 0);
            d.level_min = sqlite3_column_int(st.s, 1);
            d.level_max = sqlite3_column_int(st.s, 2);
            d.all_biomes = sqlite3_column_int(st.s, 3) != 0;
            d.cultures = sqlite3_column_int64(st.s, 4);
            d.node_types = sqlite3_column_int64(st.s, 5);
            d.seasons = sqlite3_column_int64(st.s, 6);
            d.biomes = sqlite3_column_int64(st.s, 7);
            out->defs_[guid] = d;
        }
    }
    {   // the buildings' asset sets by state
        Stmt st;
        if (!prepare(st, "SELECT guid, asset_sets_guid FROM building", "the buildings")) return nullptr;
        while (sqlite3_step(st.s) == SQLITE_ROW) out->building_sets_[sqlite3_column_int64(st.s, 0)] = sqlite3_column_int64(st.s, 1);
    }
    {
        Stmt st;
        if (!prepare(st, "SELECT sets_guid, state, asset_set_guid FROM building_asset_state ORDER BY sets_guid, state, ord", "the buildings' asset sets by state")) return nullptr;
        while (sqlite3_step(st.s) == SQLITE_ROW) {
            out->state_sets_[{sqlite3_column_int64(st.s, 0), sqlite3_column_int(st.s, 1)}].push_back(sqlite3_column_int64(st.s, 2));
        }
    }
    if (out->props_.empty() || out->defs_.empty()) {
        error = path + ": no layout props or asset definitions (it was built by a contentc before version 12)";
        return nullptr;
    }
    return out;
}

bool NodeLayoutData::def_valid(const Def& d, const LayoutContext& c) const {
    if (c.level < d.level_min || c.level > d.level_max) return false;
    if (!bit(d.cultures, c.culture) || !bit(d.node_types, c.node_type) || !bit(d.seasons, c.season)) return false;
    if (c.biome >= 0 && !d.all_biomes && !bit(d.biomes, c.biome)) return false;
    return true;
}

bool NodeLayoutData::set_valid(int64_t asset_set, const LayoutContext& c) const {
    const auto m = members_.find(asset_set);
    if (m == members_.end()) return false;
    for (const int64_t def : m->second) {
        const auto d = defs_.find(def);
        if (d != defs_.end() && def_valid(d->second, c)) return true;
    }
    return false;
}

size_t NodeLayoutData::props_of(int64_t layout) const {
    const auto it = props_.find(layout);
    return it == props_.end() ? 0 : it->second.size();
}

std::vector<repl::NodeLayoutItem> NodeLayoutData::items(const NodeWire& wire, const LayoutRequest& req) const {
    std::vector<repl::NodeLayoutItem> out;
    if (!wire.is_real_node(req.node)) return out;
    const int64_t layout = wire.layout_of(req.node);
    if (layout == 0) return out;
    if (const auto p = props_.find(layout); p != props_.end()) {
        for (const Prop& prop : p->second) {
            if (!set_valid(prop.asset_set, req.context)) continue;
            repl::NodeLayoutItem it = prop.item;
            it.owner_guid = req.node;
            it.owner_secondary = 0;
            it.owner_index = prop.idx;
            out.push_back(std::move(it));
        }
    }
    for (const PlotRow& row : wire.plot_rows(layout, req.context.level)) {
        uint8_t state;
        if (req.plot_states) {
            const auto s = req.plot_states->find(row.plot);
            if (s != req.plot_states->end()) {
                state = s->second;
            } else if (!row.pre_construct) {
                continue;
            } else {
                state = req.plots == PlotPolicy::DataStartingState ? row.starting_state : row.state;
            }
        } else {
            if (!row.pre_construct) continue;   // nobody built on it: it sends nothing
            state = req.plots == PlotPolicy::DataStartingState ? row.starting_state : row.state;
        }
        if (state == 0 || row.building == 0) continue;
        const auto b = building_sets_.find(row.building);
        if (b == building_sets_.end()) continue;
        const auto sets = state_sets_.find({b->second, state});
        if (sets == state_sets_.end()) continue;
        for (size_t k = 0; k < sets->second.size(); ++k) {
            if (!set_valid(sets->second[k], req.context)) continue;
            repl::NodeLayoutItem it;
            it.owner_guid = row.plot;
            it.owner_secondary = state;
            it.owner_index = static_cast<int32_t>(k);
            it.asset_set = sets->second[k];
            double qx = row.qx, qy = row.qy, qz = row.qz;
            if (row.qw < 0) {
                qx = -qx;
                qy = -qy;
                qz = -qz;
            }
            it.rotation = repl::Quat{qx, qy, qz};
            it.translation = repl::Vec3d{row.tx, row.ty, row.tz};
            it.scale = repl::Vec3d{row.sx, row.sy, row.sz};
            out.push_back(std::move(it));
        }
    }
    return out;
}

}  // namespace aoc::worldd::world
