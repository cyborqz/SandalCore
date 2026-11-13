// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/node_rewards.h  (node citizenship: what a done mayoral commission gives its node)
//
//  A mayoral commission's RewardId is a NodeRewardRecordBase: NodeMods, each a NodeModification for a Duration (24 h for every commission's).  A modification's
//  EffectsIds are NodeEffect records (content.db's node_reward_mod, node_modification_ref and node_effect_* tables, contentc version 45):
//
//    the boons       ZOIEffects: an effect (Effect_ZOI_*, "Lesser Boon of Dexterity" ...) on the characters, caravans or mounts in the node's zone of influence.
//                    One that asks the NodeCitizen alignment or an org affiliation is a citizen's: the account must be a citizen of the node, at its
//                    MinimumReputationRank or above (Commoner for every citizen boon of the data), and with CanBeAppliedGlobally it reaches the citizen
//                    anywhere (at MinimumReputationRankForGlobalApplication).  Any other reaches every character in the zone (Peasant: no rank asked).
//    the commodities CommoditiesToGiveNode: the node's stores gain them when the modification starts (each commodity as the item it makes).
//  Modelled: a boon is one stack for each running modification that gives it (the effects stack in place, at most 10); caravan and mount boons have nothing to
//  land on yet; an alignment other than NodeCitizen is read as citizenship too (the data names no other).
// ============================================================================
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace aoc::worldd::world {

/// The CharacterAlignmentRecordBase NodeCitizen: what a citizen's boon asks.
inline constexpr int64_t kNodeCitizenAlignment = 6064631051897143296ll;

/// One of a node effect's ZOIEffects.
struct NodeBoon {
    int64_t node_effect = 0;     // the NodeEffect record it is part of
    int64_t effect = 0;          // the EffectRecordBase it puts on
    bool    on_character = false;
    int     min_rank = 0;        // ENodeReputationRank
    bool    global = false;      // CanBeAppliedGlobally
    int     global_min_rank = 0;
    bool    citizens = false;    // the NodeCitizen alignment or an org affiliation is asked
    bool operator==(const NodeBoon&) const = default;
};

struct NodeCommodityGift {
    int64_t commodity = 0;       // a CommodityDefRecordBase
    int32_t quantity = 0;
};

/// One of a reward's NodeMods.
struct RewardMod {
    int64_t mod = 0;             // the NodeModification
    int64_t duration_ms = 0;
};

class NodeRewards {
public:
    /// content.db's node reward tables; nullopt with `error` for a file older than contentc version 45.
    static std::optional<NodeRewards> load(const std::string& content_db_path, std::string& error);

    /// The modifications reward `reward` gives (null: the tables do not list it).
    const std::vector<RewardMod>* mods_of(int64_t reward) const;
    /// What modification `mod` puts on the characters about the node, and what it gives the node's stores (empty: nothing).
    const std::vector<NodeBoon>& boons_of(int64_t mod) const;
    const std::vector<NodeCommodityGift>& commodities_of(int64_t mod) const;

    size_t rewards() const { return rewards_.size(); }
    size_t mods() const { return boons_.size(); }

    /// (tests) the rows by hand.
    void add_reward_for_test(int64_t reward, std::vector<RewardMod> mods) { rewards_[reward] = std::move(mods); }
    void add_boon_for_test(int64_t mod, NodeBoon b) { boons_[mod].push_back(b); }
    void add_commodity_for_test(int64_t mod, NodeCommodityGift g) { commodities_[mod].push_back(g); }

private:
    std::map<int64_t, std::vector<RewardMod>>         rewards_;
    std::map<int64_t, std::vector<NodeBoon>>          boons_;
    std::map<int64_t, std::vector<NodeCommodityGift>> commodities_;
};

/// Whether boon `b` of a node reaches a character: `citizen` (the account is a citizen of that node), `rank` its rank there (kNoReputationRank for none) and
/// `in_zone` (it stands in the node's zone of influence).
bool boon_reaches(const NodeBoon& b, bool citizen, int rank, bool in_zone);

/// The item a commodity makes (CommodityProducedId), 0 for one the tables do not know.
int64_t commodity_item(int64_t commodity);

}  // namespace aoc::worldd::world
