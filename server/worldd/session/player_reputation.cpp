// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_reputation.cpp  (node citizenship: a character's reputation with the nodes)
//
//  The rules are world/node_reputation.h; this keeps a character's reputation by node, shows it in OwnerInfo.NodeToPlayerReputation and saves it with the
//  progress (migration 0022: the whole list once the session changed it).
//
//    earned        a game master's grant (the console below) for now; a gain shows the currency callout of the Reputation tier.
//    the decay     at the join and with every gain (world/node_reputation.h); what the session answers is decayed to the moment asked.
//    the rank      GetNodeToPlayerReputationRank() in the shop, warehouse and dialogue rules: the rank at the node asked of for its citizen, none for anyone else;
//                  a shop's tax is scaled by the rank's TaxModifier.  A new rank is told in the chat [modelled: no capture shows the game's notice].
//    the console   ServerGrantNodeToPlayerReputation(PlayerName, NodeName, ReputationModifier) and ServerGrantCitizensNodeToPlayerReputation(NodeName,
//                  ReputationModifier), a game master's (gm_rpc_admitted): an empty player is the game master, an empty node the one the game master stands in;
//                  the citizens' grant reaches every citizen of the node in the world.
// ============================================================================
#include "session/player_session_util.h"

#include "session/gm_travel.h"
#include "world/node_reputation.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;

namespace {

/// The most a game master's grant adds or takes at once.
constexpr double kMaxGmReputationGrant = 1'000'000;

const repl::Value* rparam(const repl::ClientRpc& rpc, std::string_view path) {
    return rpc.call.decoded && rpc.call.rpc ? repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, path) : nullptr;
}

std::string rstr(const repl::ClientRpc& rpc, std::string_view path) {
    const repl::Value* v = rparam(rpc, path);
    const std::string* s = v ? v->get_if<std::string>() : nullptr;
    return s ? *s : std::string();
}

double rfloat(const repl::ClientRpc& rpc, std::string_view path) {
    const repl::Value* v = rparam(rpc, path);
    if (!v) return 0;
    if (const float* f = v->get_if<float>()) return *f;
    if (const double* d = v->get_if<double>()) return *d;
    if (const int64_t* i = v->get_if<int64_t>()) return static_cast<double>(*i);
    return 0;
}

std::string rclipped(const std::string& s) { return s.size() > 100 ? s.substr(0, 100) : s; }

std::string amount_text(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.1f", v);
    return buf;
}

}  // namespace

const world::ReputationRules& PlayerSession::reputation_rules_of() const {
    static const world::ReputationRules kDefault{};
    return world_ ? world_->reputation_rules() : kDefault;
}

std::string PlayerSession::node_display_name(int64_t node) const {
    const world::CityNodes* nodes = world_ ? world_->city_nodes() : nullptr;
    const world::CityNodeInfo* n = nodes ? nodes->by_guid(node) : nullptr;
    return n && !n->display_name.empty() ? n->display_name : "node " + std::to_string(node);
}

void PlayerSession::init_node_reputation(const svc::Character& character) {
    const world::ReputationRules& rules = reputation_rules_of();
    const int64_t now = wall_ms();
    node_rep_.clear();
    for (const svc::NodeReputation& saved : character.node_reputation) {
        if (saved.node_guid == 0 || !std::isfinite(saved.reputation)) continue;
        world::NodeRep r{saved.reputation, saved.last_decay_ms};
        rules.decay(r, now);   // (a decay alone is not saved: the next login works it out again from the same clock)
        if (r.reputation != 0) node_rep_[saved.node_guid] = r;
    }
    set_node_reputation_state();
    if (!node_rep_.empty()) spdlog::info("[nodes] {}: reputation with {} node(s)", name(), node_rep_.size());
}

void PlayerSession::set_node_reputation_state() {
    std::vector<bootstrap::NodeReputationEntry> entries;
    entries.reserve(node_rep_.size());
    for (const auto& [node, r] : node_rep_) {
        if (r.reputation != 0) entries.push_back({node, static_cast<float>(r.reputation), r.last_decay_ms / 1000});
    }
    pb_.set_node_reputation(entries);
}

double PlayerSession::node_reputation(int64_t node) const {
    const auto it = node_rep_.find(node);
    if (it == node_rep_.end()) return 0;
    world::NodeRep r = it->second;
    reputation_rules_of().decay(r, wall_ms());
    return r.reputation;
}

int PlayerSession::reputation_rank_at(int64_t node) const {
    if (node == 0 || citizenship_.node_guid != node) return world::kNoReputationRank;
    return reputation_rules_of().rank_of(node_reputation(node));
}

vendor::TaxRate PlayerSession::reputation_taxed(vendor::TaxRate rate, int64_t node) const {
    const double m = reputation_rules_of().tax_of(reputation_rank_at(node));
    if (m == 1.0) return rate;
    rate.percent = static_cast<float>(rate.percent * m);
    rate.flat = static_cast<int32_t>(std::lround(rate.flat * m));
    return rate;
}

double PlayerSession::give_node_reputation(Connection& conn, int64_t node, double delta, const std::string& why) {
    if (node == 0 || delta == 0 || !std::isfinite(delta)) return 0;
    const world::ReputationRules& rules = reputation_rules_of();
    const int64_t now = wall_ms();
    world::NodeRep& r = node_rep_[node];
    world::NodeRep was = r;
    rules.decay(was, now);
    const int rank_before = rules.rank_of(was.reputation);
    const double added = rules.add(node, r, delta, now);
    const double total = r.reputation;
    if (total == 0) node_rep_.erase(node);
    if (added == 0) {
        spdlog::log(level_for(reputation_log_), "[nodes] {}: {} node reputation with {} from {} is not added: the node's range ends there ({})", name(), amount_text(delta),
                    node_display_name(node), why, amount_text(total));
        return 0;
    }
    node_rep_changed_ = true;
    progress_changed_ = true;
    ++reputation_gains_;
    set_node_reputation_state();
    if (room_for(1)) queue("OwnerInfo", [](bootstrap::PlayerBootstrap& pb) { return pb.currencies_update(); }, conn);
    const int64_t shown = std::llround(added);
    if (shown > 0 && room_for(1)) {
        queue("ClientShowCurrencyCallout", [shown](bootstrap::PlayerBootstrap& pb) { return pb.currency_callout(world::kNodeReputationTier, shown); }, conn, /*must_build=*/true);
    }
    const int rank_after = rules.rank_of(total);
    spdlog::info("[nodes] {}: {} node reputation with {} from {} (now {}, {})", name(), amount_text(added), node_display_name(node), why, amount_text(total),
                 world::reputation_rank_name(rank_after));
    if (rank_after != rank_before && citizenship_.node_guid == node) {
        receive_chat_line(conn, "Your standing with " + node_display_name(node) + " is now " + world::reputation_rank_name(rank_after) + ".", 4, "");
    }
    hand_over_progress("node reputation");
    return added;
}

std::string PlayerSession::reputation_text() const {
    std::string out = name() + "'s node reputation:";
    bool any = false;
    const int64_t citizen = citizenship_.node_guid;
    for (const auto& [node, r] : node_rep_) {
        (void)r;
        const double now = node_reputation(node);
        out += "\n  " + node_display_name(node) + ": " + amount_text(now) +
               (node == citizen ? std::string(" (") + world::reputation_rank_name(reputation_rank_at(node)) + ")" : std::string(" (not a citizen: no rank)"));
        any = true;
    }
    if (citizen != 0 && !node_rep_.count(citizen)) {
        out += "\n  " + node_display_name(citizen) + ": 0.0 (" + world::reputation_rank_name(reputation_rank_at(citizen)) + ")";
        any = true;
    }
    if (!any) out += " none (a citizen earns it with the node from rewards)";
    return out;
}

void PlayerSession::on_gm_grant_reputation(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now, bool citizens) {
    const char* call = citizens ? "ServerGrantCitizensNodeToPlayerReputation" : "ServerGrantNodeToPlayerReputation";
    const std::string who = citizens ? std::string() : rstr(rpc, "PlayerName"), node_name = rstr(rpc, "NodeName");
    const double amount = rfloat(rpc, "ReputationModifier");
    if (!gm_rpc_admitted(call, (citizens ? std::string() : "PlayerName=\"" + rclipped(who) + "\" ") + "NodeName=\"" + rclipped(node_name) + "\" ReputationModifier=" + amount_text(amount), now) ||
        !rpc.call.decoded) {
        return;
    }
    if (!std::isfinite(amount) || amount == 0 || std::fabs(amount) > kMaxGmReputationGrant) {
        spdlog::warn("[gm] {}: {} does nothing: the amount {} is not one to grant (at most {} either way)", name(), call, amount_text(amount), amount_text(kMaxGmReputationGrant));
        return;
    }
    std::string why;
    const int64_t node = gm_node_named(node_name, why);
    if (node == 0) {
        spdlog::warn("[gm] {}: {} does nothing: {}", name(), call, why);
        return;
    }
    const std::string by = std::string(call) + " by " + name();
    if (!citizens) {
        if (who.empty() || iequals(who, name())) {
            const double added = give_node_reputation(conn, node, amount, by);
            spdlog::info("[gm] {}: {}: {} node reputation with {} for {}", name(), call, amount_text(added), node_display_name(node), name());
            return;
        }
        auto* gm = world_ ? dynamic_cast<GmTravel*>(world_->gm_port()) : nullptr;
        const PlayerHandle h = gm ? gm->find_player(who) : PlayerHandle{};
        if (!h) {
            spdlog::warn("[gm] {}: {} does nothing: no player named \"{}\" is in the world", name(), call, rclipped(who));
            return;
        }
        const double added = h.player->give_node_reputation(*h.connection, node, amount, by);
        spdlog::info("[gm] {}: {}: {} node reputation with {} for {}", name(), call, amount_text(added), node_display_name(node), h.player->name());
        return;
    }
    auto* gm = world_ ? dynamic_cast<GmTravel*>(world_->gm_port()) : nullptr;
    std::vector<PlayerHandle> online = gm ? gm->online() : std::vector<PlayerHandle>{};
    bool self_listed = false;
    for (const PlayerHandle& h : online) self_listed = self_listed || h.player == this;
    if (!self_listed) online.push_back(PlayerHandle{this, &conn});   // (a test world lists no one)
    size_t reached = 0;
    for (const PlayerHandle& h : online) {
        if (!h || h.player->citizenship_.node_guid != node) continue;
        h.player->give_node_reputation(*h.connection, node, amount, by);
        ++reached;
    }
    spdlog::info("[gm] {}: {}: {} node reputation with {} for {} citizen(s) in the world", name(), call, amount_text(amount), node_display_name(node), reached);
}

/// The client calls of node reputation (session/rpc_table.h).
void PlayerSession::register_reputation_rpcs(RpcTable& t) {
    t.add(RpcScope::PlayerController, "ServerGrantNodeToPlayerReputation", [](PlayerSession& s, const RpcContext& c) { s.on_gm_grant_reputation(c.conn, c.rpc, c.now, false); });
    t.add(RpcScope::PlayerController, "ServerGrantCitizensNodeToPlayerReputation", [](PlayerSession& s, const RpcContext& c) { s.on_gm_grant_reputation(c.conn, c.rpc, c.now, true); });
}

}}  // namespace aoc::worldd
