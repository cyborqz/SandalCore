// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/node_reputation.cpp
// ============================================================================
#include "world/node_reputation.h"

#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

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

std::string text_of(sqlite3_stmt* st, int col) {
    const unsigned char* t = sqlite3_column_text(st, col);
    return t ? reinterpret_cast<const char*>(t) : "";
}

constexpr int64_t kHourMs = 3600000;

}  // namespace

const char* reputation_rank_name(int rank) {
    static constexpr const char* kNames[] = {"Peasant", "Commoner", "Liege", "Noble", "Aristocrat"};
    return rank >= 0 && rank < kReputationRanks ? kNames[rank] : "cCount";
}

std::optional<ReputationRules> ReputationRules::load(const std::string& path, std::string& error) {
    Db db;
    if (sqlite3_open_v2(path.c_str(), &db.db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        error = "cannot open " + path + ": " + (db.db ? sqlite3_errmsg(db.db) : "out of memory");
        return std::nullopt;
    }
    ReputationRules r;
    for (size_t i = 0; i < node_constants::kReputationToLevel.size() && i < r.threshold.size(); ++i) r.threshold[i] = static_cast<double>(node_constants::kReputationToLevel[i]);
    {
        Stmt st;
        if (sqlite3_prepare_v2(db.db, "SELECT rank_ord, tax_modifier FROM node_reputation_tier", -1, &st.s, nullptr) != SQLITE_OK) {
            error = path + " has no node_reputation_tier table (" + sqlite3_errmsg(db.db) + ")";
            return std::nullopt;
        }
        while (sqlite3_step(st.s) == SQLITE_ROW) {
            const int rank = sqlite3_column_int(st.s, 0);
            const double tax = sqlite3_column_double(st.s, 1);
            if (rank >= 0 && rank < kReputationRanks && tax > 0 && tax <= 10) r.tax_modifier[static_cast<size_t>(rank)] = tax;
        }
    }
    {
        Stmt st;
        if (sqlite3_prepare_v2(db.db, "SELECT value FROM node_constant WHERE name = 'NodeToPlayerReputationDecayPerHour'", -1, &st.s, nullptr) == SQLITE_OK &&
            sqlite3_step(st.s) == SQLITE_ROW) {
            const double v = std::strtod(text_of(st.s, 0).c_str(), nullptr);
            if (v >= 0 && v <= 1e6) r.decay_per_hour = v;
        }
    }
    {
        Stmt st;
        if (sqlite3_prepare_v2(db.db, "SELECT guid, min_reputation, max_reputation FROM city_node", -1, &st.s, nullptr) != SQLITE_OK) {
            error = path + "'s city_node has no reputation range (" + sqlite3_errmsg(db.db) + ")";
            return std::nullopt;
        }
        while (sqlite3_step(st.s) == SQLITE_ROW) {
            const double lo = sqlite3_column_double(st.s, 1), hi = sqlite3_column_double(st.s, 2);
            r.range[sqlite3_column_int64(st.s, 0)] = {std::min(lo, hi), std::max(lo, hi)};
        }
    }
    return r;
}

int ReputationRules::rank_of(double reputation) const {
    int rank = 0;
    for (int i = 1; i < kReputationRanks; ++i) {
        if (reputation >= threshold[static_cast<size_t>(i)]) rank = i;
    }
    return rank;
}

std::pair<double, double> ReputationRules::range_of(int64_t node) const {
    const auto it = range.find(node);
    return it == range.end() ? std::pair<double, double>{kDefaultMin, kDefaultMax} : it->second;
}

bool ReputationRules::decay(NodeRep& r, int64_t now_ms) const {
    if (r.last_decay_ms <= 0 || r.last_decay_ms > now_ms) {
        r.last_decay_ms = now_ms;
        return false;
    }
    const int64_t hours = (now_ms - r.last_decay_ms) / kHourMs;
    if (hours <= 0) return false;
    r.last_decay_ms += hours * kHourMs;
    if (r.reputation == 0 || decay_per_hour <= 0) return false;
    const double step = decay_per_hour * static_cast<double>(hours);
    const double before = r.reputation;
    r.reputation = r.reputation > 0 ? std::max(0.0, r.reputation - step) : std::min(0.0, r.reputation + step);
    return r.reputation != before;
}

double ReputationRules::add(int64_t node, NodeRep& r, double delta, int64_t now_ms) const {
    decay(r, now_ms);
    if (!std::isfinite(delta) || delta == 0) return 0;
    const auto [lo, hi] = range_of(node);
    const double before = r.reputation;
    r.reputation = std::clamp(r.reputation + delta, lo, hi);
    return r.reputation - before;
}

}  // namespace aoc::worldd::world
