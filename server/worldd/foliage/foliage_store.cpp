// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  foliage/foliage_store.cpp (see foliage_store.h)
// ============================================================================
#include "foliage/foliage_store.h"

#include <algorithm>
#include <cmath>

namespace aoc::worldd::foliage {

FoliageStore::FoliageStore(std::vector<uint32_t> def_of_row, std::vector<DefTimes> defs, Config config)
    : def_of_row_(std::move(def_of_row)), defs_(std::move(defs)), config_(config) {
    if (!(config_.regrow_mult > 0.0) || !std::isfinite(config_.regrow_mult)) config_.regrow_mult = 1.0;
}

std::vector<int> FoliageStore::chain(const DefTimes& def) {
    std::vector<int> out;
    for (int s = kInfancy; s <= kAdult && s <= def.paint; ++s) {
        if (def.has[s]) out.push_back(s);
    }
    if (out.empty() || out.back() != def.paint) out.push_back(def.paint);
    return out;
}

std::optional<int> FoliageStore::next_state(const DefTimes& def, int state) const {
    const std::vector<int> c = chain(def);
    if (state == kEmpty) return c.front();
    for (size_t i = 0; i + 1 < c.size(); ++i) {
        if (c[i] == state) return c[i + 1];
    }
    return std::nullopt;   // the painted state, or a state the chain does not pass
}

double FoliageStore::duration_of(const DefTimes& def, int state) const {
    const double seconds = state == kEmpty ? def.empty_seconds : (state >= kInfancy && state <= kAdult ? def.life[state] : 0.0);
    return std::max(0.0, seconds) * config_.regrow_mult;
}

int FoliageStore::state_of(uint32_t row) const {
    const auto it = dev_.find(row);
    return it != dev_.end() ? it->second.state : def_of(row).paint;
}

std::optional<Change> FoliageStore::harvest(uint32_t row, double now) {
    if (row >= rows() || state_of(row) == kEmpty) return std::nullopt;
    Dev d;
    d.state = kEmpty;
    d.due = now + duration_of(def_of(row), kEmpty);
    dev_[row] = d;
    due_.push({d.due, row});
    Change c;
    c.row = row;
    c.state = kEmpty;
    return c;
}

std::vector<Change> FoliageStore::advance(double now) {
    std::vector<Change> out;
    while (!due_.empty() && due_.top().at <= now) {
        const Due top = due_.top();
        due_.pop();
        const auto it = dev_.find(top.row);
        if (it == dev_.end() || it->second.due != top.at) continue;   // a stale entry: the instance was harvested again or is whole
        const DefTimes& def = def_of(top.row);
        const std::optional<int> next = next_state(def, it->second.state);
        if (!next) {   // the end of the chain (a state the chain does not pass): whole
            dev_.erase(it);
            out.push_back({top.row, def.paint, true});
            continue;
        }
        Change c;
        c.row = top.row;
        c.state = *next;
        if (*next == def.paint) {
            dev_.erase(it);   // whole again
            c.painted = true;
        } else {
            it->second.state = *next;
            it->second.due = top.at + duration_of(def, *next);
            due_.push({it->second.due, top.row});
        }
        out.push_back(c);
    }
    return out;
}
// (C | O | A ~ C-Y-B)

std::optional<double> FoliageStore::next_due() const {
    // (stale entries are dropped lazily by advance, so the top may be one: a harmless early wake)
    if (due_.empty()) return std::nullopt;
    return due_.top().at;
}

double FoliageStore::seconds_until_whole(uint32_t row, double now) const {
    const auto it = dev_.find(row);
    if (it == dev_.end()) return 0.0;
    const DefTimes& def = def_of(row);
    double left = std::max(0.0, it->second.due - now);
    int state = it->second.state;
    while (const std::optional<int> next = next_state(def, state)) {
        if (*next == def.paint) break;
        left += duration_of(def, *next);
        state = *next;
    }
    return left;
}

double FoliageStore::regrow_seconds(uint32_t row) const {
    const DefTimes& def = def_of(row);
    double total = duration_of(def, kEmpty);
    int state = kEmpty;
    while (const std::optional<int> next = next_state(def, state)) {
        if (*next == def.paint) break;
        total += duration_of(def, *next);
        state = *next;
    }
    return total;
}

bool FoliageStore::lock(uint32_t row, uint64_t player) {
    const auto it = locks_.find(row);
    if (it != locks_.end()) return it->second == player;
    locks_.emplace(row, player);
    return true;
}

void FoliageStore::unlock(uint32_t row, uint64_t player) {
    const auto it = locks_.find(row);
    if (it != locks_.end() && it->second == player) locks_.erase(it);
}

uint64_t FoliageStore::locked_by(uint32_t row) const {
    const auto it = locks_.find(row);
    return it == locks_.end() ? 0 : it->second;
}

}  // namespace aoc::worldd::foliage
