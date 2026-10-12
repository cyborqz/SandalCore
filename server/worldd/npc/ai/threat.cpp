// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  npc/ai/threat.cpp (see threat.h)
// ============================================================================
#include "npc/ai/threat.h"

#include <algorithm>
#include <cmath>

namespace aoc::worldd::npc::ai {

bool ThreatTable::add(uint64_t id, double amount, double now) {
    if (!std::isfinite(amount)) return false;
    for (Entry& e : entries_) {
        if (e.id != id) continue;
        e.threat = std::max(0.0, e.threat + amount);
        if (amount > 0) e.last_at = now;
        return true;
    }
    if (entries_.size() >= kMaxEntries) return false;
    entries_.push_back({id, std::max(0.0, amount), now});
    return true;
}

void ThreatTable::remove(uint64_t id) {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.id == id; }), entries_.end());
}

double ThreatTable::of(uint64_t id) const {
    for (const Entry& e : entries_) {
        if (e.id == id) return e.threat;
    }
    return 0;
}

bool ThreatTable::has(uint64_t id) const {
    for (const Entry& e : entries_) {
        if (e.id == id) return true;
    }
    return false;
}

uint64_t ThreatTable::pick(uint64_t current) const {
    const Entry* top = nullptr;
    for (const Entry& e : entries_) {
        if (!top || e.threat > top->threat) top = &e;
    }
    if (!top) return 0;
    if (current != 0 && current != top->id) {
        const double cur = of(current);
        if (has(current) && top->threat < cur * kSwitchRatio) return current;
    }
    return top->id;
}

}  // namespace aoc::worldd::npc::ai
