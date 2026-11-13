// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_node_xp.cpp  (Nodes 2: a node grows from what the players do in it)
//
//  The XP a player earns (a kill's Adventuring XP, a harvest's, a craft's or a processing job's Adventuring and profession XP, a quest reward's node XP)
//  gives the node whose zone of influence the player stands in (a quest's: the node where its giver stands) the same base XP, before the players' rate,
//  times the node rate (--node-exp-rate, config/rates.json node_exp_rate: 100).  Only a node that grows (CityNodeInfo::levelable) takes it.  PlayerWorld
//  gathers it and sends citynode one AddNodeExperience per node every kNodeXpFlushMs (world::NodeControl::gain_experience); citynode levels the node
//  up (services/internal/citynode/leveling.go) and the change comes back through the feed.  [modelled: the client and the data say nothing of
//  where a node's XP comes from (LevelingTypeDef's bProvidesNodeExperience is false for every type)]
//
//  The level-up countdown: a node that reached its next threshold has Node.level_up_at_ms; the players in its zone get
//  ClientShowNodeLevelUpCountdownToast(NodeGuid, TimeStarted) once (the client's Toast_NodeEvent_Countdown_LevelUp, "{NodeName} Leveling Up", counts 5 s)
//  [modelled: TimeStarted on the GameState's world clock, in seconds, as the other node times].
// ============================================================================
#include "session/player_session_util.h"

#include "world/node_control.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <string>

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;
namespace schema = ::aoc::protocol::schema;

namespace {

/// How often the gathered node XP goes to citynode.
constexpr int64_t kNodeXpFlushMs = 10000;
/// The countdown the client's toast counts (Toast_NodeEvent_Countdown_LevelUp.CountdownDuration).
constexpr int64_t kLevelUpCountdownMs = 5000;

}  // namespace

bool PlayerWorld::add_node_experience(int64_t guid, int64_t base, const char* why) const {
    if (base <= 0 || !city_nodes_) return false;
    const world::CityNodeInfo* n = city_nodes_->by_guid(guid);
    if (!n || !n->levelable) return false;
    const double scaled = std::round(static_cast<double>(base) * node_experience_rate_);
    if (!(scaled >= 1)) return false;
    int64_t& pending = node_xp_pending_[guid];
    pending = static_cast<int64_t>(std::min<double>(static_cast<double>(pending) + scaled, static_cast<double>(INT64_MAX / 2)));
    node_xp_total_ += static_cast<int64_t>(std::min<double>(scaled, static_cast<double>(INT64_MAX / 4)));
    spdlog::debug("[nodes] {} node XP to {} ({} base, {})", static_cast<int64_t>(scaled), n->display_name, base, why);
    return true;
}

void PlayerWorld::flush_node_experience(int64_t now_ms, bool force) const {
    if (node_xp_pending_.empty()) return;
    if (!force && now_ms - node_xp_flushed_ms_ < kNodeXpFlushMs) return;
    node_xp_flushed_ms_ = now_ms;
    if (!node_control_) {
        node_xp_pending_.clear();   // (no node service: nowhere to send it)
        return;
    }
    for (const auto& [guid, amount] : node_xp_pending_) {
        if (amount > 0) node_control_->gain_experience(guid, amount);
    }
    node_xp_pending_.clear();
}

void PlayerSession::earn_node_experience(int64_t base, const char* why) {
    if (base <= 0 || !world_) return;
    const GoodPosition at = checker_.good();
    const int64_t node = citizenship_node_at(at.x, at.y).first;
    if (node != 0 && world_->add_node_experience(node, base, why)) ++node_xp_gains_;
}

void PlayerSession::run_node_levels(Connection& conn, Clock::time_point now) {
    if (world_) world_->flush_node_experience(wall_ms());
    const world::CityNodes* nodes = world_ ? world_->city_nodes() : nullptr;
    if (!nodes || nodes->version() == node_levels_version_ || !phase_in_done_) return;
    node_levels_version_ = nodes->version();
    const GoodPosition at = checker_.good();
    const int64_t here = citizenship_node_at(at.x, at.y).first;
    const int64_t wall = wall_ms();
    for (const world::LiveNode& l : nodes->live()) {
        if (l.level_up_at_ms <= 0) continue;
        int64_t& shown = level_up_toasts_[l.guid];
        if (shown == l.level_up_at_ms) continue;
        shown = l.level_up_at_ms;   // (once per countdown, wherever the player is: one entering the zone later sees the level change)
        if (l.guid != here || l.level_up_at_ms < wall - kLevelUpCountdownMs || !room_for(1)) continue;
        const int64_t node = l.guid;
        const int64_t started = to_world_seconds(l.level_up_at_ms - kLevelUpCountdownMs, now);
        ++level_up_toasts_sent_;
        const world::CityNodeInfo* n = nodes->by_guid(node);
        spdlog::info("[nodes] {}: {} is levelling up: the countdown toast", name(), n ? n->display_name : std::to_string(node));
        send_citizenship_call(conn, "ClientShowNodeLevelUpCountdownToast", [node, started](const schema::RpcSchema& r, repl::RpcParams& p) {
            return repl::set_rpc_param(r, p, "NodeGuid", repl::Value::i64(node)) && repl::set_rpc_param(r, p, "TimeStarted", repl::Value::i64(started));
        });
    }
}

}}  // namespace aoc::worldd
