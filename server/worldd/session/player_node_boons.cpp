// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_node_boons.cpp  (node citizenship: the boons of a node's running modifications)
//
//  A node's Operational service buildings and their expansions run NodeModifications (world/node_buildings.h, world/node_rewards.h).  This puts their boons
//  on the character: every kNodeBoonInterval it works out what the running modifications of the node it stands in, and of the node its account is a citizen
//  of, give it (world::boon_reaches: a citizen's boon by rank, and anywhere when it can be applied globally), one stack for each modification that gives it,
//  and puts on or takes off the difference.  A boon persists through death and not through a relog: the next login works it out again.  The client shows each
//  as an ordinary effect (the TUI_Icon_Node_Boon_* icons) [modelled: no capture shows a boon given].
// ============================================================================
#include "session/player_session_util.h"

#include "world/node_rewards.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace aoc { namespace worldd {

namespace {

/// How often the boons are looked at.
constexpr Clock::duration kNodeBoonInterval = std::chrono::seconds(2);
/// The most stacks of one boon (the effects' InPlaceStackLimit).
constexpr int kMaxNodeBoonStacks = 10;

}  // namespace

std::map<int64_t, int> PlayerSession::wanted_node_boons() const {
    std::map<int64_t, int> out;
    const world::NodeRewards* book = world_ ? world_->node_rewards() : nullptr;
    if (!book) return out;
    const GoodPosition at = checker_.good();
    const int64_t here = citizenship_node_at(at.x, at.y).first, citizen = citizenship_.node_guid;
    std::set<int64_t> nodes{here, citizen};
    nodes.erase(0);
    for (const int64_t node : nodes) {
        const std::vector<int64_t>& built = world_->building_mods(node);   // the node's Operational buildings and their expansions (session/player_node_buildings.cpp)
        if (built.empty()) continue;
        const bool is_citizen = citizen == node, in_zone = here == node;
        const int rank = reputation_rank_at(node);
        for (const int64_t mod : built) {
            for (const world::NodeBoon& b : book->boons_of(mod)) {
                if (!world::boon_reaches(b, is_citizen, rank, in_zone)) continue;
                int& n = out[b.effect];
                n = std::min(n + 1, kMaxNodeBoonStacks);
            }
        }
    }
    return out;
}

void PlayerSession::run_node_boons(Connection& conn, Clock::time_point now) {
    (void)conn;   // (the runtime's events carry the change: take_ability_events stages it in the pawn, flush_abilities sends it)
    if (!pipeline_ || !world_ || !world_->node_rewards() || finished_ || failed_ || abilities_failed_ || !phase_in_done_ || !join_.empty()) return;
    if (!node_boons_dirty_ && now < next_node_boons_) return;
    node_boons_dirty_ = false;
    next_node_boons_ = now + kNodeBoonInterval;
    const std::map<int64_t, int> wanted = wanted_node_boons();
    if (wanted.empty() && node_boons_.empty()) return;
    const double sec = ability_seconds(now);
    size_t added = 0, removed = 0;
    for (auto it = node_boons_.begin(); it != node_boons_.end();) {
        const auto w = wanted.find(it->first);
        if (w != wanted.end() && w->second == it->second && pipeline_->stacks_of(it->first) > 0) {
            ++it;
            continue;
        }
        if (pipeline_->remove_effect(it->first, sec)) ++removed;
        it = node_boons_.erase(it);
    }
    for (const auto& [effect, stacks] : wanted) {
        if (node_boons_.count(effect) || node_boons_refused_.count(effect)) continue;
        if (!pipeline_->grant_effect(effect, stacks, sec)) {
            node_boons_refused_.insert(effect);
            spdlog::warn("[nodes] {}: the node boon {} is not in the ability tables (content.db older than contentc version 45?): it gives nothing", name(), effect);
            continue;
        }
        node_boons_[effect] = stacks;
        ++added;
    }
    if (added == 0 && removed == 0) return;
    take_ability_events();
    spdlog::log(level_for(node_boons_log_), "[nodes] {}: the nodes' boons: {} put on, {} taken off ({} on now)", name(), added, removed, node_boons_.size());
}

}}  // namespace aoc::worldd
