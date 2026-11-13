// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/node_reputation.h  (node citizenship: a character's reputation with the nodes)
//
//  What a character stands with each city node, as the game keeps it in UOwnerInfoComponent.NodeToPlayerReputation (the node, the reputation, when it last decayed).  Pure rules
//  from content.db; the session (session/player_reputation.cpp) keeps each character's and saves it with the progress (migration 0022).
//
//    the ranks     ENodeReputationRank Peasant 0, Commoner 1, Liege 2, Noble 3, Aristocrat 4 (node_reputation_tier), reached at CityNodeReputationToLevel's
//                  0 / 10,000 / 30,000 / 60,000 / 100,000 (world/node_constants.h).  A reputation below 0 is still Peasant.  Each rank's TaxModifier scales the taxes
//                  the node's shops charge the character (1, 0.95, 0.9, 0.85, 0.8).
//    the range     city_node's MinReputation and MaxReputation: -700 to 100,000 for most nodes, -700 to 1,100 for some, 0 to 0 (none) for the rest.
//    the decay     node_constant NodeToPlayerReputationDecayPerHour (1): each whole hour since the last decay moves the reputation toward 0 by that much [modelled:
//                  toward 0 from either side; the client gets LastDecayTime and may show the decay itself].
//    the rank asked by the data   GetNodeToPlayerReputationRank() (no argument) in shop listings, warehouse tabs and abilities: the character's rank with the node the
//                  question is about (the shopkeeper's settlement), or none (ENodeReputationRank::cCount) when the character is no citizen of it: "Must be a citizen of this
//                  settlement" is all five ranks or'ed.
// ============================================================================
#pragma once

#include "world/node_constants.h"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>

namespace aoc::worldd::world {

/// ENodeReputationRank: Peasant 0 .. Aristocrat 4; kNoReputationRank (cCount) when there is no rank to give.
inline constexpr int kReputationRanks = 5;
inline constexpr int kNoReputationRank = 5;
/// "Peasant" .. "Aristocrat", "cCount" for kNoReputationRank.
const char* reputation_rank_name(int rank);

/// The NodeToPlayerReputation currency (its tier "Reputation"): what a reward table's reputation line names.
inline constexpr int64_t kNodeReputationCurrency = 6064629801116499968ll;
inline constexpr int64_t kNodeReputationTier = 6064629801127837697ll;

/// A character's reputation with one node.  last_decay_ms is when it last decayed (Unix milliseconds; 0 for never).
struct NodeRep {
    double  reputation = 0;
    int64_t last_decay_ms = 0;
    bool operator==(const NodeRep&) const = default;
};

struct ReputationRules {
    std::array<double, kReputationRanks> threshold{0, 10000, 30000, 60000, 100000};   // CityNodeReputationToLevel
    std::array<double, kReputationRanks> tax_modifier{1.0, 0.95, 0.9, 0.85, 0.8};    // node_reputation_tier
    double decay_per_hour = 1;                                                        // NodeToPlayerReputationDecayPerHour
    std::map<int64_t, std::pair<double, double>> range;                               // city_node MinReputation, MaxReputation by node
    static constexpr double kDefaultMin = -700, kDefaultMax = 100000;                  // (a node the data does not list)

    /// content.db's node_reputation_tier, node_constant and city_node; nullopt with `error` for a file without them.
    static std::optional<ReputationRules> load(const std::string& content_db_path, std::string& error);

    /// The rank a reputation reaches (Peasant below Commoner, negatives included).
    int rank_of(double reputation) const;
    /// The node's range (min, max).
    std::pair<double, double> range_of(int64_t node) const;
    /// `r` brought to `now_ms`: each whole hour since its last decay moves it toward 0 by decay_per_hour; one that never decayed starts its clock now.  True when the
    /// reputation changed.
    bool decay(NodeRep& r, int64_t now_ms) const;
    /// Adds `delta` to `r` (after its decay), within the node's range; returns what was added.
    double add(int64_t node, NodeRep& r, double delta, int64_t now_ms) const;
    /// The rank's tax modifier (1 for no rank).
    double tax_of(int rank) const { return rank >= 0 && rank < kReputationRanks ? tax_modifier[static_cast<size_t>(rank)] : 1.0; }
};

}  // namespace aoc::worldd::world
