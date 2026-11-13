// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "world/citizenship.h"

#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <regex>

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

/// A dues formula's "var Threshold = N;" (the data's CitizenshipRankImpact and LevelImpact declare one each); nullopt when it has none.
std::optional<double> formula_threshold(const std::string& text) {
    static const std::regex kThreshold(R"(var\s+Threshold\s*=\s*([0-9]+(?:\.[0-9]+)?))");
    std::smatch m;
    if (!std::regex_search(text, m, kThreshold)) return std::nullopt;
    const double v = std::strtod(m[1].str().c_str(), nullptr);
    return v > 0 ? std::optional<double>(v) : std::nullopt;
}

std::string column_text(sqlite3_stmt* st, int col) {
    const unsigned char* t = sqlite3_column_text(st, col);
    return t ? reinterpret_cast<const char*>(t) : "";
}

/// The quantity of `tier` in a BaseItemValue's buy price ("[[tier,"1"]]"); 0 when it lists none.
int64_t price_in(const std::string& json, int64_t tier) {
    const std::string key = "[" + std::to_string(tier) + ",\"";
    const size_t at = json.find(key);
    if (at == std::string::npos) return 0;
    return std::strtoll(json.c_str() + at + key.size(), nullptr, 10);
}

}  // namespace

CitizenshipRules::DuesFactors CitizenshipRules::dues_factors(int rank, int level, int node_level) const {
    DuesFactors f;
    // the data's formulas: x / Threshold when that is above 1, else 1
    if (rank_threshold > 0 && rank / rank_threshold > 1.0) f.rank = rank / rank_threshold;
    if (level_threshold > 0 && level / level_threshold > 1.0) f.level = level / level_threshold;
    f.non_owner = non_owner_impact > 0 ? non_owner_impact : 1.0;
    if (const auto it = dues_level_scalar.find(node_level); it != dues_level_scalar.end() && it->second > 0) f.node_level = it->second;
    return f;
}

int32_t CitizenshipRules::dues_amount(const DuesFactors& f) const {
    const double v = static_cast<double>(std::max(1, base_dues)) * f.rank * f.level * f.non_owner * f.node_level;
    return static_cast<int32_t>(std::clamp<double>(std::floor(v + 1e-9), 1.0, 100000.0));
}

int32_t CitizenshipRules::dues_owed(int64_t due_ms, int64_t now_ms, int32_t amount, int node_level) const {
    if (due_ms <= 0 || now_ms < overdue_ms(due_ms)) return amount;
    const auto it = missed_dues_increase.find(node_level);
    const double increase = it == missed_dues_increase.end() ? 0.0 : std::max(0.0, it->second);
    return std::max<int32_t>(1, static_cast<int32_t>(std::floor(amount * (1.0 + increase) + 1e-9))) + late_fee;
}

const TaxLimit* CitizenshipRules::tax_limit(int64_t node, const std::string& tag) const {
    const auto t = node_tax_table.find(node);
    if (t == node_tax_table.end()) return nullptr;
    const auto table = tax_limits.find(t->second);
    if (table == tax_limits.end()) return nullptr;
    std::string at = tag;
    while (!at.empty()) {
        if (const auto it = table->second.find(at); it != table->second.end()) return &it->second;
        const size_t dot = at.rfind('.');
        if (dot == std::string::npos) break;
        at.resize(dot);
    }
    return nullptr;
}

int64_t CitizenshipRules::cooldown_ms(int level) const {
    const auto it = cooldown_secs.find(level);
    return it == cooldown_secs.end() ? 0 : std::max<int64_t>(0, it->second) * 1000;
}

int64_t CitizenshipRules::next_dues_ms(int64_t anchor_ms, int64_t now_ms) const {
    const int64_t period = period_ms();
    if (now_ms < anchor_ms) return anchor_ms + period;
    return anchor_ms + ((now_ms - anchor_ms) / period + 1) * period;
}

std::optional<CitizenshipRules> CitizenshipRules::load(const std::string& path, std::string& error) {
    Db db;
    if (sqlite3_open_v2(path.c_str(), &db.db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        error = "cannot open " + path + ": " + (db.db ? sqlite3_errmsg(db.db) : "out of memory");
        return std::nullopt;
    }
    CitizenshipRules r;
    {
        Stmt st;
        // (the generic record's InitialDueCost is not the join's cost, which is CitizenshipCost 1: the table only shows the file holds citizenship)
        if (sqlite3_prepare_v2(db.db, "SELECT initial_due_cost FROM citizenship_generic ORDER BY guid LIMIT 1", -1, &st.s, nullptr) != SQLITE_OK) {
            error = path + " has no citizenship_generic table (" + sqlite3_errmsg(db.db) + ")";
            return std::nullopt;
        }
    }
    {
        Stmt st;
        if (sqlite3_prepare_v2(db.db, "SELECT level, cooldown_secs, dues_level_scalar, missed_dues_increase FROM citizenship_node ORDER BY level", -1, &st.s, nullptr) !=
            SQLITE_OK) {
            error = path + " has no citizenship_node table (" + sqlite3_errmsg(db.db) + ")";
            return std::nullopt;
        }
        while (sqlite3_step(st.s) == SQLITE_ROW) {
            const int level = sqlite3_column_int(st.s, 0);
            if (level < 0 || level > 6) continue;
            r.cooldown_secs[level] = std::max<int64_t>(0, sqlite3_column_int64(st.s, 1));
            if (const double v = sqlite3_column_double(st.s, 2); v > 0 && v <= 100) r.dues_level_scalar[level] = v;
            if (const double v = sqlite3_column_double(st.s, 3); v >= 0 && v <= 100) r.missed_dues_increase[level] = v;
        }
    }
    {
        Stmt st;   // the buy order slots (a content.db without those columns: no node runs buy orders)
        if (sqlite3_prepare_v2(db.db, "SELECT level, min_buy_order_slots FROM citizenship_node", -1, &st.s, nullptr) == SQLITE_OK) {
            while (sqlite3_step(st.s) == SQLITE_ROW) {
                const int level = sqlite3_column_int(st.s, 0), slots = sqlite3_column_int(st.s, 1);
                if (level >= 0 && level <= 6 && slots > 0 && slots <= 1000) r.buy_order_slots[level] = slots;
            }
        }
    }
    {
        Stmt st;
        if (sqlite3_prepare_v2(db.db,
                               "SELECT weeks, late_fee, overdue_days, base_dues_guid, rank_impact, level_impact, non_property_owner FROM citizenship_dues_def "
                               "ORDER BY guid LIMIT 1",
                               -1, &st.s, nullptr) != SQLITE_OK) {
            error = path + " has no citizenship_dues_def table (" + sqlite3_errmsg(db.db) + ")";
            return std::nullopt;
        }
        if (sqlite3_step(st.s) == SQLITE_ROW) {
            const int weeks = sqlite3_column_int(st.s, 0);
            if (weeks > 0 && weeks <= 52) r.dues_weeks = weeks;
            const double fee = sqlite3_column_double(st.s, 1);
            if (fee >= 0 && fee <= 100000) r.late_fee = static_cast<int32_t>(std::lround(fee));
            const int days = sqlite3_column_int(st.s, 2);
            if (days >= 0 && days <= 365) r.overdue_days = days;
            const int64_t base = sqlite3_column_int64(st.s, 3);
            if (const auto t = formula_threshold(column_text(st.s, 4))) r.rank_threshold = *t;
            if (const auto t = formula_threshold(column_text(st.s, 5))) r.level_threshold = *t;
            if (const double v = std::strtod(column_text(st.s, 6).c_str(), nullptr); v > 0 && v <= 100) r.non_owner_impact = v;
            sqlite3_finalize(st.s);
            st.s = nullptr;
            // BaseDuesRequired: a BaseItemValue whose buy price is in the dues tier
            if (base != 0 && sqlite3_prepare_v2(db.db, "SELECT buy FROM base_item_value WHERE guid = ?", -1, &st.s, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(st.s, 1, base);
                if (sqlite3_step(st.s) == SQLITE_ROW) {
                    if (const int64_t n = price_in(column_text(st.s, 0), r.dues_tier); n > 0 && n <= 100000) r.base_dues = static_cast<int32_t>(n);
                }
            }
        }
    }
    {
        Stmt st;   // the nodes' tax tables and their tags' limits (a content.db without them: no node takes a mayor's rates)
        if (sqlite3_prepare_v2(db.db, "SELECT guid, tax_table_guid FROM city_node WHERE tax_table_guid != 0", -1, &st.s, nullptr) == SQLITE_OK) {
            while (sqlite3_step(st.s) == SQLITE_ROW) r.node_tax_table[sqlite3_column_int64(st.s, 0)] = sqlite3_column_int64(st.s, 1);
        }
    }
    {
        Stmt st;
        if (sqlite3_prepare_v2(db.db,
                               "SELECT t.table_guid, t.tag, d.default_percent, d.min_percent, d.max_percent, d.default_flat, d.min_flat, d.max_flat FROM node_tax_table_tag t "
                               "JOIN node_tax_details d ON d.guid = t.details_guid",
                               -1, &st.s, nullptr) == SQLITE_OK) {
            while (sqlite3_step(st.s) == SQLITE_ROW) {
                TaxLimit l;
                l.default_percent = static_cast<float>(sqlite3_column_double(st.s, 2));
                l.min_percent = static_cast<float>(sqlite3_column_double(st.s, 3));
                l.max_percent = static_cast<float>(sqlite3_column_double(st.s, 4));
                l.default_flat = static_cast<int32_t>(std::lround(sqlite3_column_double(st.s, 5)));
                l.min_flat = static_cast<int32_t>(std::lround(sqlite3_column_double(st.s, 6)));
                l.max_flat = static_cast<int32_t>(std::lround(sqlite3_column_double(st.s, 7)));
                r.tax_limits[sqlite3_column_int64(st.s, 0)][column_text(st.s, 1)] = l;
            }
        }
    }
    {
        Stmt st;   // (a content.db without the column keeps the default)
        if (sqlite3_prepare_v2(db.db, "SELECT guid, citizenship_at_risk_secs FROM city_node", -1, &st.s, nullptr) == SQLITE_OK) {
            while (sqlite3_step(st.s) == SQLITE_ROW) r.at_risk_secs[sqlite3_column_int64(st.s, 0)] = sqlite3_column_int64(st.s, 1);
        }
    }
    if (r.cooldown_secs.empty()) {
        error = path + "'s citizenship_node lists no level: no node takes citizens";
        return std::nullopt;
    }
    return r;
}

}  // namespace aoc::worldd::world
