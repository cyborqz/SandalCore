// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_nodes.cpp
//
//  The nodes the world knows: the answers to the node window's and the login's requests, the live map updates and the client's node game master console.
// ============================================================================
#include "session/player_session.h"

#include "bootstrap/character_setup.h"
#include "combat/combat_world.h"
#include "crafting/job.h"
#include "crafting/rewards.h"
#include "crafting/rules.h"
#include "crafting/station.h"
#include "content/content_db.h"
#include "dialogue/talk.h"
#include "protocol/repl/ability_rpc.h"
#include "protocol/repl/dialogue_rpc.h"
#include "world/city_nodes.h"
#include "protocol/repl/hit_rpc.h"
#include "protocol/repl/item_rpc.h"
#include "protocol/repl/move_packed.h"
#include "protocol/repl/storage_rpc.h"
#include "session/gm_travel.h"
#include "protocol/repl/rpc_fields.h"
#include "items/acquired.h"
#include "vendor/node_tax.h"
#include "vendor/sell_rules.h"
#include "quest/quest_wire.h"
#include "quest/registry_points.h"
#include "world/zoi_surface.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <map>
#include <random>
#include <set>

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;
namespace schema = ::aoc::protocol::schema;
namespace wire = ::aoc::protocol::wire;
using bootstrap::Role;
using protocol::emit::FIntrepidNetworkGUID;

// ============================================================================================================================
//  Nodes 1, stage 2: the world knows its nodes
// ============================================================================================================================

void PlayerSession::note_node_request(std::deque<int64_t>& wait, std::set<int64_t>& queued, int64_t node, const char* what) {
    if (node == 0) {
        // the client's own "Requesting Node Data with invalid Node ID" is a request it does not send; a zero from elsewhere has nothing to answer
        spdlog::debug("[nodes] {}: {} for node 0: ignored", name(), what);
        return;
    }
    if (queued.count(node)) return;   // its answer is already waiting
    if (wait.size() >= kMaxNodeRequests) {
        ++node_requests_dropped_;
        if (!node_overflow_logged_) {
            node_overflow_logged_ = true;
            spdlog::warn("[nodes] {}: {} requests are waiting for their answers: further ones are dropped (said once)", name(), wait.size());
        }
        return;
    }
    queued.insert(node);
    wait.push_back(node);
}

void PlayerSession::on_node_summary_request(const repl::ClientRpc& rpc) {
    ++node_requests_;
    const auto node = repl::parse_node_summary_request(rpc.call);
    if (!node) {
        if (!node_unreadable_logged_) {
            node_unreadable_logged_ = true;
            spdlog::warn("[nodes] {}: a ServerRequestNodeSummary does not read ({} parameter bits): ignored (further ones are not logged)", name(), rpc.call.param_bits);
        }
        return;
    }
    note_node_request(node_summary_wait_, node_summary_queued_, *node, "ServerRequestNodeSummary");
}

void PlayerSession::on_siege_summary_request(const repl::ClientRpc& rpc) {
    ++node_requests_;
    const auto node = repl::parse_siege_summary_request(rpc.call);
    if (!node) {
        if (!node_unreadable_logged_) {
            node_unreadable_logged_ = true;
            spdlog::warn("[nodes] {}: a ServerRequestCurrentSiegeSummary does not read ({} parameter bits): ignored (further ones are not logged)", name(), rpc.call.param_bits);
        }
        return;
    }
    note_node_request(siege_wait_, siege_queued_, *node, "ServerRequestCurrentSiegeSummary");
}

void PlayerSession::on_siege_cooldowns_request() {
    ++node_requests_;
    cooldowns_pending_ = true;
}

// (The citizenship window's request, ServerSetEssentialCitizenshipWindowData, is answered in session/player_citizenship.cpp.)
// C.Y.B with CoA

void PlayerSession::run_nodes(Connection& conn) {
    if (finished_ || failed_ || !join_.empty()) return;   // (the join includes the pawn's second state: the client's world exists, as retail's answers assume)
    run_citizenship(conn);
    const world::CityNodes* nodes = world_ ? world_->city_nodes() : nullptr;
    const world::NodeWire* wire = world_ ? world_->node_wire() : nullptr;
    // room for a reply, leaving the others theirs (an answer burst of 98 must not starve a purchase's reply): no log when it is full, the next tick tries again
    const auto has_room = [this] { return !failed_ && replies_.size() + delayed_.size() + 8 <= kMaxPendingReplies; };

    // the registry's levels changed: a delta of the GameState's node maps
    const world::Elections* elections = world_ ? world_->elections() : nullptr;
    const uint64_t election_version = elections ? elections->version() : 0;
    if (pb_.mode() == bootstrap::GameStateMode::Tier3 && nodes && wire && (nodes->version() != node_maps_version_ || election_version != node_maps_election_version_) &&
        has_room()) {
        node_maps_version_ = nodes->version();
        node_maps_election_version_ = election_version;   // (a mayor or a citizen count changed: the maps follow; an unchanged map sends nothing)
        auto maps = std::make_shared<protocol::repl::NodeStateMaps>(wire->maps(*nodes, elections));
        ++node_map_updates_;
        std::string levels = nodes->describe_levels();
        spdlog::log(levels == node_maps_levels_ ? spdlog::level::debug : spdlog::level::info, "[nodes] {}: the node {} changed (registry version {}): sending the GameState's node maps ({})",
                    name(), levels == node_maps_levels_ ? "XP" : "levels", node_maps_version_, levels);
        node_maps_levels_ = std::move(levels);
        queue("GameState node maps", [maps](bootstrap::PlayerBootstrap& pb) { return pb.node_maps_update(*maps); }, conn, /*must_build=*/false);
    }
    if (cooldowns_pending_ && has_room()) {
        cooldowns_pending_ = false;
        ++siege_cooldowns_sent_;
        queue("ClientReceiveAllCurrentNodeSiegeCooldowns", [](bootstrap::PlayerBootstrap& pb) { return pb.siege_cooldowns(); }, conn, /*must_build=*/true);
    }
    const auto unknown = [this](int64_t node, const char* what) {
        if (node_unknown_logged_) return;
        node_unknown_logged_ = true;
        spdlog::warn("[nodes] {}: the client asked {} for node {}, which worldd does not know: not answered (further ones are not logged)", name(), what, node);
    };
    // retail's order: every siege summary first, then every node summary, each in the order the client asked
    while (!siege_wait_.empty() && has_room()) {
        const int64_t node = siege_wait_.front();
        siege_wait_.pop_front();
        siege_queued_.erase(node);
        const auto s = nodes && wire ? wire->siege(node, *nodes) : std::nullopt;
        if (!s) {
            // a record that is no node of a map (the dungeons, the jungle) gets no siege answer, as in retail; a guid worldd does not know is said once
            if (nodes && nodes->by_guid(node)) spdlog::debug("[nodes] {}: no siege summary for {}: it is no node of a map (retail does not answer it either)", name(), node);
            else unknown(node, "for its siege summary");
            continue;
        }
        ++siege_summaries_sent_;
        queue("ClientReceiveSiegeSummary", [s](bootstrap::PlayerBootstrap& pb) { return pb.siege_summary(*s); }, conn, /*must_build=*/true);
    }
    while (siege_wait_.empty() && !node_summary_wait_.empty() && has_room()) {
        const int64_t node = node_summary_wait_.front();
        node_summary_wait_.pop_front();
        node_summary_queued_.erase(node);
        const int64_t unix_now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const auto s = nodes && wire ? wire->summary(node, *nodes, unix_now, elections) : std::nullopt;
        if (!s) {
            unknown(node, "for its summary");
            continue;
        }
        ++node_summaries_sent_;
        if (node_summaries_sent_ == 1 || node_summaries_sent_ == 49) {
            spdlog::info("[nodes] {}: node summary #{}: {} at level {} with {} plots", name(), node_summaries_sent_,
                         nodes->by_guid(node) ? nodes->by_guid(node)->display_name : std::to_string(node), s->level, s->plots.size());
        }
        queue("ClientReceiveNodeSummary", [s](bootstrap::PlayerBootstrap& pb) { return pb.node_summary(*s); }, conn, /*must_build=*/true);
    }
}

void PlayerSession::on_gm_node_rpc(const repl::ClientRpc& rpc, Clock::time_point now) {
    const std::string rpc_name = rpc.call.rpc->name;
    ++node_gm_rpcs_;
    char params[256];
    std::snprintf(params, sizeof params, "%zu parameter bits%s", rpc.call.param_bits, rpc.call.decoded ? "" : "; the parameters did not decode");
    world::NodeControl* control = world_ ? world_->node_control() : nullptr;
    const std::string by = rpc_name + " by " + name();
    const auto admitted = [&](const std::string& shown) { return gm_rpc_admitted(rpc_name.c_str(), shown, now) && rpc.call.decoded; };
    const auto unavailable = [&] { spdlog::warn("[gm] {}: {}: the node service is unavailable (worldd runs without a citynode connection)", name(), rpc_name); };
    if (rpc_name == "ServerSetNodeLevel") {
        const auto g = repl::parse_gm_set_node_level(rpc.call);
        const std::string shown = g ? "InNode=\"" + g->node.substr(0, 80) + "\" InLevel=" + std::to_string(g->level) + " InCulture=" + std::to_string(g->culture) +
                                          " bDisableVassalshipSystem=" + (g->disable_vassalship ? "1" : "0")
                                    : std::string(params);
        if (!admitted(shown) || !g) return;
        if (!control) return unavailable();
        ++gm_rpcs_answered_;
        control->set_level(g->node, g->level, g->disable_vassalship, by);
    } else if (rpc_name == "ServerAddNodeExperience") {
        const auto g = repl::parse_gm_add_node_experience(rpc.call);
        const std::string shown = g ? "InNode=\"" + g->node.substr(0, 80) + "\" Experience=" + std::to_string(g->experience) + " InCulture=" + std::to_string(g->culture) +
                                          " bUseRawXP=" + (g->raw_xp ? "1" : "0")
                                    : std::string(params);
        if (!admitted(shown) || !g) return;
        if (!control) return unavailable();
        ++gm_rpcs_answered_;
        control->add_experience(g->node, g->experience, by);
    } else if (rpc_name == "ServerGMGetNodeData") {
        const auto g = repl::parse_gm_get_node_data(rpc.call);
        const std::string shown = g ? "NodeName=\"" + g->substr(0, 80) + "\"" : std::string(params);
        if (!admitted(shown) || !g) return;
        if (!control) return unavailable();
        ++gm_rpcs_answered_;
        control->node_data(*g, by);
    } else if (rpc_name == "ServerGMWhatTimeIsIt") {
        if (!admitted(params)) return;
        if (!control) return unavailable();
        ++gm_rpcs_answered_;
        control->clock(by);
    } else if (rpc_name == "ServerGMFastForwardRealTime") {
        int64_t seconds = 0;
        if (rpc.call.decoded) {
            if (const repl::Value* v = repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "FastForwardDuration")) {
                if (const repl::Ticks* t = v->get_if<repl::Ticks>()) seconds = t->ticks / 10000000;   // an FTimespan is 100 ns ticks
            }
        }
        if (!admitted("FastForwardDuration=" + std::to_string(seconds) + " s")) return;
        if (seconds <= 0) {
            spdlog::warn("[gm] {}: ServerGMFastForwardRealTime of {} s: nothing to skip", name(), seconds);
            return;
        }
        if (!control) return unavailable();
        ++gm_rpcs_answered_;
        control->advance_clock(seconds, by);
    } else {
        // every other ServerGM* call (mayors, citizens, sieges, relics ...) has its milestone: logged, so a test of the shipping client's console shows what it sends
        gm_rpc_admitted(rpc_name.c_str(), params, now);
        spdlog::log(level_for(node_logged_, 20),
                    "[gm] {}: {} is not implemented in Nodes 1 (only ServerSetNodeLevel, ServerAddNodeExperience, ServerGMGetNodeData, ServerGMWhatTimeIsIt and "
                    "ServerGMFastForwardRealTime reach the node service): ignored",
                    name(), rpc_name);
    }
}

/// The client calls of the node window, the login's 49 + 49 requests and the client's node game master console (session/rpc_table.h).
void PlayerSession::register_nodes_rpcs(RpcTable& t) {
    t.add(RpcScope::PlayerController, "ServerRequestNodeSummary", [](PlayerSession& s, const RpcContext& c) { s.on_node_summary_request(c.rpc); });
    t.add(RpcScope::PlayerController, "ServerRequestCurrentSiegeSummary", [](PlayerSession& s, const RpcContext& c) { s.on_siege_summary_request(c.rpc); });
    t.add(RpcScope::PlayerController, "ServerRequestAllNodeSiegeCooldowns", [](PlayerSession& s, const RpcContext&) { s.on_siege_cooldowns_request(); });
    t.add(RpcScope::PlayerController, "ServerSetNodeLevel", [](PlayerSession& s, const RpcContext& c) { s.on_gm_node_rpc(c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerAddNodeExperience", [](PlayerSession& s, const RpcContext& c) { s.on_gm_node_rpc(c.rpc, c.now); });
    t.add_prefix(RpcScope::PlayerController, "ServerGM", [](PlayerSession& s, const RpcContext& c) { s.on_gm_node_rpc(c.rpc, c.now); });
}

}}  // namespace aoc::worldd
