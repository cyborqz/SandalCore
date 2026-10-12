// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  npc/ai/threat.h  (plan E1, stage 3, part 1)
//
//  What an NPC remembers of who hurt it: a threat table.  Damage dealt to it adds threat (the hit's own ThreatToTarget when the data gives one, else the
//  damage times the attacker's ThreatGeneration), healing an enemy of its adds some, and merely coming into its aggro radius adds a little.  It fights the
//  one with the most; to take another target the challenger must have kSwitchRatio (110 %, the MMO convention) of what the current one has, so two attackers
//  with about the same threat do not make it run back and forth.  [modelled: no capture shows a threat value on the wire, research/combat 7 and 9.5]
// ============================================================================
#pragma once

#include <cstdint>
#include <vector>

namespace aoc::worldd::npc::ai {

class ThreatTable {
public:
    /// A challenger needs this multiple of the current target's threat to take it over.
    static constexpr double kSwitchRatio = 1.10;
    static constexpr size_t kMaxEntries = 64;

    struct Entry {
        uint64_t id = 0;       // the attacker's actor id
        double   threat = 0;
        double   last_at = 0;  // the clock when it last added threat
    };

    /// Adds threat (negative amounts take it away, not below 0); a new attacker is entered. Returns false when the table is full of others.
    bool add(uint64_t id, double amount, double now);
    void remove(uint64_t id);
    void clear() { entries_.clear(); }
    bool empty() const { return entries_.empty(); }
    size_t size() const { return entries_.size(); }
    double of(uint64_t id) const;
    bool has(uint64_t id) const;
    const std::vector<Entry>& entries() const { return entries_; }

    /// The target to fight: `current` while no one has kSwitchRatio of its threat (and it is still in the table), else the one with the most (the earliest
    /// entered wins a tie). 0 for an empty table.
    uint64_t pick(uint64_t current) const;

private:
    std::vector<Entry> entries_;
};

}  // namespace aoc::worldd::npc::ai
