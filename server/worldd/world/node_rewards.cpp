// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/node_rewards.cpp
// ============================================================================
#include "world/node_rewards.h"

#include "world/node_inventory_table.h"
#include "world/node_reputation.h"

#include <sqlite3.h>

#include <algorithm>
#include <set>
#include <utility>

namespace aoc::worldd::world {

namespace {

struct Db {
    sqlite3* db = nullptr;
    ~Db() {
        if (db) sqlite3_close(db);
    }
};

struct Stmt {
    sqlite3_stmt* s = nullptr;
    ~Stmt() {
        if (s) sqlite3_finalize(s);
    }
};

const std::vector<NodeBoon> kNoBoons;
const std::vector<NodeCommodityGift> kNoCommodities;

}  // namespace

std::optional<NodeRewards> NodeRewards::load(const std::string& path, std::string& error) {
    Db db;
    if (sqlite3_open_v2(path.c_str(), &db.db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        error = "cannot open " + path + ": " + (db.db ? sqlite3_errmsg(db.db) : "out of memory");
        return std::nullopt;
    }
    const auto prepare = [&](Stmt& st, const char* sql, const char* what) {
        if (sqlite3_prepare_v2(db.db, sql, -1, &st.s, nullptr) == SQLITE_OK) return true;
        error = path + " has no " + what + " (" + sqlite3_errmsg(db.db) + "; contentc version 45 writes it)";
        return false;
    };
    NodeRewards r;
    {
        Stmt st;
        if (!prepare(st, "SELECT reward_guid, mod_guid, duration_ms FROM node_reward_mod ORDER BY reward_guid, ord", "node_reward_mod table")) return std::nullopt;
        while (sqlite3_step(st.s) == SQLITE_ROW) {
            r.rewards_[sqlite3_column_int64(st.s, 0)].push_back(RewardMod{sqlite3_column_int64(st.s, 1), sqlite3_column_int64(st.s, 2)});
        }
    }
    // a citizen's boon: one that asks an alignment (the data names only NodeCitizen) or an org affiliation
    std::set<std::pair<int64_t, int64_t>> aligned;   // (node effect, its ZOIEffects ord)
    {
        Stmt st;
        if (!prepare(st, "SELECT node_effect_guid, zoi_ord FROM node_effect_zoi_alignment", "node_effect_zoi_alignment table")) return std::nullopt;
        while (sqlite3_step(st.s) == SQLITE_ROW) aligned.insert({sqlite3_column_int64(st.s, 0), sqlite3_column_int64(st.s, 1)});
    }
    {
        Stmt st;
        if (!prepare(st,
                     "SELECT m.mod_guid, z.node_effect_guid, z.ord, z.effect_guid, z.on_character, z.min_rank, z.global, z.global_min_rank, z.require_org "
                     "FROM node_modification_ref m JOIN node_effect_zoi z ON z.node_effect_guid = m.ref_guid WHERE m.kind = 'effect' ORDER BY m.mod_guid, m.ord, z.ord",
                     "node_effect_zoi table")) {
            return std::nullopt;
        }
        while (sqlite3_step(st.s) == SQLITE_ROW) {
            NodeBoon b;
            b.node_effect = sqlite3_column_int64(st.s, 1);
            b.effect = sqlite3_column_int64(st.s, 3);
            b.on_character = sqlite3_column_int(st.s, 4) != 0;
            b.min_rank = sqlite3_column_int(st.s, 5);
            b.global = sqlite3_column_int(st.s, 6) != 0;
            b.global_min_rank = sqlite3_column_int(st.s, 7);
            b.citizens = sqlite3_column_int(st.s, 8) != 0 || aligned.count({b.node_effect, sqlite3_column_int64(st.s, 2)}) > 0;
            if (b.effect != 0) r.boons_[sqlite3_column_int64(st.s, 0)].push_back(b);
        }
    }
    {
        Stmt st;
        if (!prepare(st,
                     "SELECT m.mod_guid, c.commodity_guid, c.quantity FROM node_modification_ref m JOIN node_effect_commodity c ON c.node_effect_guid = m.ref_guid "
                     "WHERE m.kind = 'effect' ORDER BY m.mod_guid, m.ord, c.ord",
                     "node_effect_commodity table")) {
            return std::nullopt;
        }
        while (sqlite3_step(st.s) == SQLITE_ROW) {
            const int64_t q = sqlite3_column_int64(st.s, 2);
            if (q > 0) r.commodities_[sqlite3_column_int64(st.s, 0)].push_back(NodeCommodityGift{sqlite3_column_int64(st.s, 1), static_cast<int32_t>(std::min<int64_t>(q, 1000000))});
        }
    }
    return r;
}

const std::vector<RewardMod>* NodeRewards::mods_of(int64_t reward) const {
    const auto it = rewards_.find(reward);
    return it == rewards_.end() ? nullptr : &it->second;
}

const std::vector<NodeBoon>& NodeRewards::boons_of(int64_t mod) const {
    const auto it = boons_.find(mod);
    return it == boons_.end() ? kNoBoons : it->second;
}

const std::vector<NodeCommodityGift>& NodeRewards::commodities_of(int64_t mod) const {
    const auto it = commodities_.find(mod);
    return it == commodities_.end() ? kNoCommodities : it->second;
}

bool boon_reaches(const NodeBoon& b, bool citizen, int rank, bool in_zone) {
    if (!b.on_character) return false;
    const bool ranked = citizen && rank >= 0 && rank < kReputationRanks;
    if (b.citizens) {
        if (!ranked || rank < b.min_rank) return false;
        return in_zone || (b.global && rank >= b.global_min_rank);
    }
    if (!in_zone) return false;
    return b.min_rank <= 0 || (ranked && rank >= b.min_rank);
}

int64_t commodity_item(int64_t commodity) {
    for (const node_inventory::Commodity& c : node_inventory::kCommodities) {
        if (c.def == commodity) return c.item;
    }
    return 0;
}

}  // namespace aoc::worldd::world
