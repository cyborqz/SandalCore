// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_citizenship_gm.cpp  (node citizenship: the game master's console)
//
//  The client's game master calls of citizenship and the mayoral elections, answered only for a game master (gm_rpc_admitted); every one is logged with
//  what it did or why it did nothing.  A node is named by its record or display name (CityNodes::by_name; empty: the node the game master stands in), a player
//  by its character name among the players in the world (GmTravel::find_player), a citizen or candidate by name among the node's (world/elections.h).
//
//    ServerGMSetMayor(NodeName, CharacterName)                              the node's mayor is that citizen now (an empty name: no mayor)
//    ServerGMSetMayorCandidate(NodeName, CandidateName, bAdding, ...)       the citizen stands, or no longer stands, in any phase
//    ServerGMSetMayorMessage(NodeGuid, Message)                             the mayor's message, whoever the mayor is
//    ServerGMAddNodeCitizen(NodeName, PlayerName, bSkipHousingCheck)        that player's account becomes a citizen (no cost, no level asked)
//    ServerGMRemoveNodeCitizen(NodeName, PlayerName)                        that player's account is a citizen no more (no renounce cooldown)
//    ServerGMSetCitizenshipDuesDueFromNow(NodeGuid, SecondsDueFromNow, bSetLateFeesTime)
//                                                                           the game master's own dues are due then (bSetLateFeesTime: overdue then)
//    ServerGMEndCitizenshipRenounceCooldown()                               the game master's account may join a node again at once
//    ServerGMCheckCitizenshipStatus(PlayerName)                             logs that player's citizenship
//    ServerGMDisableCitizenshipDues(NodeName), ServerGMTryEnableCitizenshipDues(NodeName)
//                                                                           the node's dues off (nothing owed, nothing lapses) or on again
//    ServerGMGetMayorVoteData(NodeName)                                     logs the node's election: its phase, candidates and ballots
//    ServerGMRankedChoiceVoteForMayor(NodeName, RankedCandidateNames)       the game master's ballot by candidates' names, citizen or not
// ============================================================================
#include "session/player_session_util.h"

#include "items/inventory.h"
#include "session/gm_travel.h"
#include "world/citizenship.h"
#include "world/elections.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;
namespace schema = ::aoc::protocol::schema;

namespace {

const repl::Value* param(const repl::ClientRpc& rpc, std::string_view path) {
    return rpc.call.decoded ? repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, path) : nullptr;
}

std::string str_of(const repl::ClientRpc& rpc, std::string_view path) {
    const repl::Value* v = param(rpc, path);
    const std::string* s = v ? v->get_if<std::string>() : nullptr;
    return s ? *s : std::string();
}

int64_t int_of(const repl::ClientRpc& rpc, std::string_view path) {
    const repl::Value* v = param(rpc, path);
    if (!v) return 0;
    if (const int64_t* i = v->get_if<int64_t>()) return *i;
    if (const uint64_t* u = v->get_if<uint64_t>()) return static_cast<int64_t>(*u);
    return 0;
}

bool bool_of(const repl::ClientRpc& rpc, std::string_view path) {
    const repl::Value* v = param(rpc, path);
    if (!v) return false;
    if (const bool* b = v->get_if<bool>()) return *b;
    return int_of(rpc, path) != 0;
}

std::vector<std::string> strings_of(const repl::ClientRpc& rpc, std::string_view path) {
    std::vector<std::string> out;
    const repl::Value* v = param(rpc, path);
    const auto* arr = v ? v->get_if<repl::ArrayValue>() : nullptr;
    if (!arr) return out;
    for (const repl::ElementState& e : arr->elements) {
        const auto it = e.values.find(1);
        const std::string* s = it == e.values.end() ? nullptr : it->second.get_if<std::string>();
        if (s && out.size() < 32) out.push_back(*s);
    }
    return out;
}

std::string clipped(const std::string& s) { return s.size() > 100 ? s.substr(0, 100) : s; }

}  // namespace

int64_t PlayerSession::gm_node_named(const std::string& name, std::string& why) const {
    const world::CityNodes* nodes = world_ ? world_->city_nodes() : nullptr;
    if (!nodes) {
        why = "this world has no city nodes";
        return 0;
    }
    if (!name.empty()) {
        const world::CityNodeInfo* named = nodes->by_name(name);
        if (!named) why = "no node is named \"" + clipped(name) + "\"";
        return named ? named->guid : 0;
    }
    const GoodPosition at = checker_.good();
    const int64_t here = citizenship_node_at(at.x, at.y).first;
    if (here == 0) why = "no node name, and the player stands in no node";
    return here;
}

void PlayerSession::send_must_be_mayor(Connection& conn) {
    send_citizenship_call(conn, "ClientShowMustBeMayorCallout", [](const schema::RpcSchema&, repl::RpcParams&) { return true; });
}

std::string PlayerSession::gm_join_citizenship(Connection& conn, int64_t node, const std::string& by) {
    const world::CitizenshipRules* rules = world_ ? world_->citizenship_rules() : nullptr;
    if (!rules) return "this world has no citizenship rules";
    if (citizenship_.node_guid == node) return name() + "'s account is a citizen of that node already";
    const int64_t now_ms = wall_ms();
    svc::Citizenship c;
    c.node_guid = node;
    c.primary_character_id = character_id_;
    c.joined_ms = now_ms;
    start_dues_schedule(node, now_ms);
    {
        const world::Elections* el = world_->elections();
        c.dues_due_ms = rules->next_dues_ms(el && el->dues_anchor(node) != 0 ? el->dues_anchor(node) : now_ms, now_ms);
    }
    citizenship_offer_.reset();
    ++citizenship_joins_;
    spdlog::info("[gm] {}: {} makes the account a citizen of node {}", name(), by, node);
    set_citizenship(conn, c, "a game master's citizenship");
    send_citizenship_call(conn, "ClientShowCitizenshipToast", [node](const schema::RpcSchema& s, repl::RpcParams& p) {
        return repl::set_rpc_param(s, p, "NodeGuid", repl::Value::i64(node)) && repl::set_rpc_param(s, p, "bRenouncing", repl::Value::boolean(false));
    });
    return name() + "'s account is a citizen of node " + std::to_string(node) + " now";
}

std::string PlayerSession::gm_leave_citizenship(Connection& conn, const std::string& by) {
    if (citizenship_.node_guid == 0) return name() + "'s account is a citizen of no node";
    const int64_t node = citizenship_.node_guid;
    ++citizenship_renounces_;
    spdlog::info("[gm] {}: {} ends the account's citizenship of node {}", name(), by, node);
    set_citizenship(conn, svc::Citizenship{}, "a game master's removal");
    send_citizenship_call(conn, "ClientShowCitizenshipToast", [node](const schema::RpcSchema& s, repl::RpcParams& p) {
        return repl::set_rpc_param(s, p, "NodeGuid", repl::Value::i64(node)) && repl::set_rpc_param(s, p, "bRenouncing", repl::Value::boolean(true));
    });
    return name() + "'s account is a citizen of node " + std::to_string(node) + " no more";
}

std::string PlayerSession::citizenship_status_text() const {
    const int64_t now_ms = wall_ms();
    if (citizenship_.node_guid == 0) {
        return name() + ": a citizen of no node" +
               (citizenship_.renounce_cooldown_until_ms > now_ms ? ", may join again in " + std::to_string((citizenship_.renounce_cooldown_until_ms - now_ms) / 1000) + " s" : "");
    }
    world::CitizenshipRules::DuesFactors f;
    int rank = 0;
    const int32_t owed = citizenship_dues(now_ms, &f, &rank);
    char buf[256];
    std::snprintf(buf, sizeof buf, ": a citizen of node %lld (rank %d), dues %d %s, due in %lld s (level x%.2f, rank x%.2f)", static_cast<long long>(citizenship_.node_guid),
                  rank, owed, dues_paid_ahead(now_ms) ? "paid" : "unpaid", static_cast<long long>((citizenship_.dues_due_ms - now_ms) / 1000), f.level, f.rank);
    return name() + buf;
}

void PlayerSession::on_gm_set_mayor(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const std::string node_name = str_of(rpc, "NodeName"), who = str_of(rpc, "CharacterName");
    if (!gm_rpc_admitted("ServerGMSetMayor", "NodeName=\"" + clipped(node_name) + "\" CharacterName=\"" + clipped(who) + "\"", now) || !rpc.call.decoded) return;
    world::Elections* el = world_ ? world_->elections() : nullptr;
    std::string why;
    const int64_t node = el ? gm_node_named(node_name, why) : 0;
    if (!el) why = "this world holds no elections";
    if (node != 0) {
        if (who.empty()) {
            el->set_mayor(node, world::ElectionPerson{}, wall_ms());
            world_->save_elections();
            spdlog::info("[gm] {}: ServerGMSetMayor: node {} has no mayor now", name(), node);
            run_elections(conn, wall_ms());
            return;
        }
        if (const world::ElectionPerson* c = el->citizen_named(node, who)) {
            el->set_mayor(node, *c, wall_ms());
            world_->save_elections();
            spdlog::info("[gm] {}: ServerGMSetMayor: {} is the mayor of node {} now", name(), c->name, node);
            run_elections(conn, wall_ms());
            return;
        }
        why = "node " + std::to_string(node) + " has no citizen named \"" + clipped(who) + "\"";
    }
    spdlog::warn("[gm] {}: ServerGMSetMayor does nothing: {}", name(), why);
}

void PlayerSession::on_gm_set_mayor_candidate(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const std::string node_name = str_of(rpc, "NodeName"), who = str_of(rpc, "CandidateName");
    const bool adding = bool_of(rpc, "bAdding");
    if (!gm_rpc_admitted("ServerGMSetMayorCandidate", "NodeName=\"" + clipped(node_name) + "\" CandidateName=\"" + clipped(who) + "\" bAdding=" + (adding ? "1" : "0"), now) ||
        !rpc.call.decoded) {
        return;
    }
    world::Elections* el = world_ ? world_->elections() : nullptr;
    std::string why;
    const int64_t node = el ? gm_node_named(node_name, why) : 0;
    if (!el) why = "this world holds no elections";
    if (node != 0) {
        const world::ElectionPerson* c = adding ? el->citizen_named(node, who) : el->candidate_named(node, who);
        if (!c) {
            why = "node " + std::to_string(node) + " has no " + (adding ? "citizen" : "candidate") + " named \"" + clipped(who) + "\"";
        } else {
            const world::ElectionPerson person = *c;
            if (el->gm_set_candidate(node, person, adding, wall_ms(), why)) {
                world_->save_elections();
                spdlog::info("[gm] {}: ServerGMSetMayorCandidate: {} {} for mayor of node {}", name(), person.name, adding ? "stands" : "no longer stands", node);
                if (citizenship_.node_guid == node) send_citizenship_window(conn);
                return;
            }
        }
    }
    spdlog::warn("[gm] {}: ServerGMSetMayorCandidate does nothing: {}", name(), why);
}

void PlayerSession::on_gm_set_mayor_message(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const int64_t node = int_of(rpc, "NodeGuid");
    const std::string message = str_of(rpc, "Message");
    if (!gm_rpc_admitted("ServerGMSetMayorMessage", "NodeGuid=" + std::to_string(node) + " Message=\"" + clipped(message) + "\"", now) || !rpc.call.decoded) return;
    world::Elections* el = world_ ? world_->elections() : nullptr;
    std::string why = "this world holds no elections";
    if (el && el->gm_set_message(node, message, why)) {
        world_->save_elections();
        spdlog::info("[gm] {}: ServerGMSetMayorMessage: node {}'s message is set", name(), node);
        (void)conn;
        return;
    }
    spdlog::warn("[gm] {}: ServerGMSetMayorMessage does nothing: {}", name(), why);
}

void PlayerSession::on_gm_add_node_citizen(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const std::string node_name = str_of(rpc, "NodeName"), who = str_of(rpc, "PlayerName");
    if (!gm_rpc_admitted("ServerGMAddNodeCitizen", "NodeName=\"" + clipped(node_name) + "\" PlayerName=\"" + clipped(who) + "\"", now) || !rpc.call.decoded) return;
    std::string why;
    const int64_t node = gm_node_named(node_name, why);
    if (node != 0) {
        if (who.empty() || iequals(who, name())) {
            spdlog::info("[gm] {}: ServerGMAddNodeCitizen: {}", name(), gm_join_citizenship(conn, node, "ServerGMAddNodeCitizen by " + name()));
            return;
        }
        auto* gm = world_ ? dynamic_cast<GmTravel*>(world_->gm_port()) : nullptr;
        const PlayerHandle h = gm ? gm->find_player(who) : PlayerHandle{};
        if (h) {
            spdlog::info("[gm] {}: ServerGMAddNodeCitizen: {}", name(), h.player->gm_join_citizenship(*h.connection, node, "ServerGMAddNodeCitizen by " + name()));
            return;
        }
        why = "no player named \"" + clipped(who) + "\" is in the world";
    }
    spdlog::warn("[gm] {}: ServerGMAddNodeCitizen does nothing: {}", name(), why);
}

void PlayerSession::on_gm_remove_node_citizen(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const std::string node_name = str_of(rpc, "NodeName"), who = str_of(rpc, "PlayerName");
    if (!gm_rpc_admitted("ServerGMRemoveNodeCitizen", "NodeName=\"" + clipped(node_name) + "\" PlayerName=\"" + clipped(who) + "\"", now) || !rpc.call.decoded) return;
    if (who.empty() || iequals(who, name())) {
        spdlog::info("[gm] {}: ServerGMRemoveNodeCitizen: {}", name(), gm_leave_citizenship(conn, "ServerGMRemoveNodeCitizen by " + name()));
        return;
    }
    auto* gm = world_ ? dynamic_cast<GmTravel*>(world_->gm_port()) : nullptr;
    const PlayerHandle h = gm ? gm->find_player(who) : PlayerHandle{};
    if (h) {
        spdlog::info("[gm] {}: ServerGMRemoveNodeCitizen: {}", name(), h.player->gm_leave_citizenship(*h.connection, "ServerGMRemoveNodeCitizen by " + name()));
        return;
    }
    // (a player who is not in the world: the node's election store forgets the account; its saved citizenship goes at its next login's adoption)
    world::Elections* el = world_ ? world_->elections() : nullptr;
    std::string why;
    const int64_t node = el ? gm_node_named(node_name, why) : 0;
    if (const world::ElectionPerson* c = node ? el->citizen_named(node, who) : nullptr) {
        const int64_t account = c->account;
        el->remove_citizen(account);
        world_->save_elections();
        spdlog::info("[gm] {}: ServerGMRemoveNodeCitizen: {} (not in the world) leaves node {}'s citizens and candidates", name(), who, node);
        return;
    }
    spdlog::warn("[gm] {}: ServerGMRemoveNodeCitizen does nothing: no player named \"{}\" is in the world{}", name(), clipped(who), why.empty() ? "" : " (" + why + ")");
}

void PlayerSession::on_gm_set_dues_due(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const int64_t node = int_of(rpc, "NodeGuid"), secs = int_of(rpc, "SecondsDueFromNow");
    const bool late = bool_of(rpc, "bSetLateFeesTime");
    if (!gm_rpc_admitted("ServerGMSetCitizenshipDuesDueFromNow", "NodeGuid=" + std::to_string(node) + " SecondsDueFromNow=" + std::to_string(secs) + (late ? " bSetLateFeesTime" : ""),
                         now) ||
        !rpc.call.decoded) {
        return;
    }
    const world::CitizenshipRules* rules = world_ ? world_->citizenship_rules() : nullptr;
    if (!rules || citizenship_.node_guid == 0 || (node != 0 && node != citizenship_.node_guid)) {
        spdlog::warn("[gm] {}: ServerGMSetCitizenshipDuesDueFromNow does nothing: the account is no citizen of node {}", name(), node);
        return;
    }
    svc::Citizenship c = citizenship_;
    const int64_t at = wall_ms() + secs * 1000;
    // bSetLateFeesTime: the late fees (the overdue date) come at that time, the dues were due overdue_days before
    c.dues_due_ms = late ? at - static_cast<int64_t>(rules->overdue_days) * 86400000 : at;
    spdlog::info("[gm] {}: ServerGMSetCitizenshipDuesDueFromNow: the dues are due at {} ms{}", name(), c.dues_due_ms, late ? " (overdue in " + std::to_string(secs) + " s)" : "");
    set_citizenship(conn, c, "a game master's dues date");
    send_citizenship_window(conn);
}

void PlayerSession::on_gm_end_renounce_cooldown(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!gm_rpc_admitted("ServerGMEndCitizenshipRenounceCooldown", "", now)) return;
    (void)rpc;
    if (citizenship_.renounce_cooldown_until_ms == 0) {
        spdlog::info("[gm] {}: ServerGMEndCitizenshipRenounceCooldown: the account has no cooldown", name());
        return;
    }
    svc::Citizenship c = citizenship_;
    c.renounce_cooldown_until_ms = 0;
    set_citizenship(conn, c, "a game master's end of the cooldown");
    spdlog::info("[gm] {}: ServerGMEndCitizenshipRenounceCooldown: the account may join a node again", name());
}

void PlayerSession::on_gm_check_citizenship(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const std::string who = str_of(rpc, "PlayerName");
    if (!gm_rpc_admitted("ServerGMCheckCitizenshipStatus", "PlayerName=\"" + clipped(who) + "\"", now) || !rpc.call.decoded) return;
    (void)conn;
    if (who.empty() || iequals(who, name())) {
        spdlog::info("[gm] {}: ServerGMCheckCitizenshipStatus: {}", name(), citizenship_status_text());
        return;
    }
    auto* gm = world_ ? dynamic_cast<GmTravel*>(world_->gm_port()) : nullptr;
    const PlayerHandle h = gm ? gm->find_player(who) : PlayerHandle{};
    if (h) spdlog::info("[gm] {}: ServerGMCheckCitizenshipStatus: {}", name(), h.player->citizenship_status_text());
    else spdlog::warn("[gm] {}: ServerGMCheckCitizenshipStatus: no player named \"{}\" is in the world", name(), clipped(who));
}

void PlayerSession::on_gm_citizenship_dues_switch(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now, bool off) {
    const char* rpc_name = off ? "ServerGMDisableCitizenshipDues" : "ServerGMTryEnableCitizenshipDues";
    const std::string node_name = str_of(rpc, "NodeName");
    if (!gm_rpc_admitted(rpc_name, "NodeName=\"" + clipped(node_name) + "\"", now) || !rpc.call.decoded) return;
    world::Elections* el = world_ ? world_->elections() : nullptr;
    std::string why = "this world holds no elections";
    const int64_t node = el ? gm_node_named(node_name, why) : 0;
    if (node == 0) {
        spdlog::warn("[gm] {}: {} does nothing: {}", name(), rpc_name, why);
        return;
    }
    el->set_dues_disabled(node, off);
    world_->save_elections();
    spdlog::info("[gm] {}: {}: node {}'s citizenship dues are {}", name(), rpc_name, node, off ? "off (nothing is owed, nothing lapses)" : "on");
    if (citizenship_.node_guid == node) send_citizenship_window(conn);
}

void PlayerSession::on_gm_mayor_vote_data(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const std::string node_name = str_of(rpc, "NodeName");
    if (!gm_rpc_admitted("ServerGMGetMayorVoteData", "NodeName=\"" + clipped(node_name) + "\"", now) || !rpc.call.decoded) return;
    (void)conn;
    world::Elections* el = world_ ? world_->elections() : nullptr;
    std::string why = "this world holds no elections";
    const int64_t node = el ? gm_node_named(node_name, why) : 0;
    const world::NodeElection* e = node ? el->node(node) : nullptr;
    if (!e) {
        spdlog::warn("[gm] {}: ServerGMGetMayorVoteData: {}", name(), node ? "node " + std::to_string(node) + " holds no election" : why);
        return;
    }
    std::string text = std::string(world::election_state_name(e->state)) + ", ends at " + std::to_string(e->phase_ends_ms) + " ms; mayor " +
                       (e->mayor.account ? e->mayor.name : std::string("none")) + "; " + std::to_string(e->citizens.size()) + " citizen(s); candidates:";
    for (const world::ElectionPerson& c : e->candidates) {
        size_t firsts = 0;
        for (const auto& [a, ranked] : e->votes) firsts += !ranked.empty() && ranked.front() == c.character_id;
        text += " " + c.name + " (" + std::to_string(firsts) + " first choice(s))";
    }
    text += "; " + std::to_string(e->votes.size()) + " ballot(s)";
    spdlog::info("[gm] {}: ServerGMGetMayorVoteData: node {}: {}", name(), node, text);
}

void PlayerSession::on_gm_ranked_vote(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const std::string node_name = str_of(rpc, "NodeName");
    const std::vector<std::string> names = strings_of(rpc, "RankedCandidateNames");
    if (!gm_rpc_admitted("ServerGMRankedChoiceVoteForMayor", "NodeName=\"" + clipped(node_name) + "\" " + std::to_string(names.size()) + " name(s)", now) || !rpc.call.decoded) {
        return;
    }
    world::Elections* el = world_ ? world_->elections() : nullptr;
    std::string why = "this world holds no elections";
    const int64_t node = el ? gm_node_named(node_name, why) : 0;
    if (node != 0) {
        std::vector<std::string> ranked;
        for (const std::string& n : names) {
            if (const world::ElectionPerson* c = el->candidate_named(node, n)) ranked.push_back(c->character_id);
        }
        if (el->gm_vote(node, election_person(), ranked, wall_ms(), why)) {
            world_->save_elections();
            spdlog::info("[gm] {}: ServerGMRankedChoiceVoteForMayor: a ballot of {} candidate(s) in node {}", name(), ranked.size(), node);
            if (citizenship_.node_guid == node) send_citizenship_window(conn);
            return;
        }
    }
    spdlog::warn("[gm] {}: ServerGMRankedChoiceVoteForMayor does nothing: {}", name(), why);
}

/// The client's game master calls of citizenship and the elections (session/rpc_table.h).
void PlayerSession::register_citizenship_gm_rpcs(RpcTable& t) {
    t.add(RpcScope::PlayerController, "ServerGMSetMayor", [](PlayerSession& s, const RpcContext& c) { s.on_gm_set_mayor(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerGMSetMayorCandidate", [](PlayerSession& s, const RpcContext& c) { s.on_gm_set_mayor_candidate(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerGMSetMayorMessage", [](PlayerSession& s, const RpcContext& c) { s.on_gm_set_mayor_message(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerGMAddNodeCitizen", [](PlayerSession& s, const RpcContext& c) { s.on_gm_add_node_citizen(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerGMRemoveNodeCitizen", [](PlayerSession& s, const RpcContext& c) { s.on_gm_remove_node_citizen(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerGMSetCitizenshipDuesDueFromNow", [](PlayerSession& s, const RpcContext& c) { s.on_gm_set_dues_due(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerGMEndCitizenshipRenounceCooldown", [](PlayerSession& s, const RpcContext& c) { s.on_gm_end_renounce_cooldown(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerGMCheckCitizenshipStatus", [](PlayerSession& s, const RpcContext& c) { s.on_gm_check_citizenship(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerGMDisableCitizenshipDues", [](PlayerSession& s, const RpcContext& c) { s.on_gm_citizenship_dues_switch(c.conn, c.rpc, c.now, true); });
    t.add(RpcScope::PlayerController, "ServerGMTryEnableCitizenshipDues", [](PlayerSession& s, const RpcContext& c) { s.on_gm_citizenship_dues_switch(c.conn, c.rpc, c.now, false); });
    t.add(RpcScope::PlayerController, "ServerGMGetMayorVoteData", [](PlayerSession& s, const RpcContext& c) { s.on_gm_mayor_vote_data(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerGMRankedChoiceVoteForMayor", [](PlayerSession& s, const RpcContext& c) { s.on_gm_ranked_vote(c.conn, c.rpc, c.now); });
}

}}  // namespace aoc::worldd
