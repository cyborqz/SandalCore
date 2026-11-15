// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_duels.cpp  (PvP, slice 1, step 3: duels)
//
//  The player's side of the world's duels (world/duels.h):
//    the calls     ServerRequestDuel(InCharacterName) (the /duel command or the right-click), ServerAcceptDuel / ServerDeclineDuel(DuelGuid) (the request
//                  window, opened by the banner's replicated Duel), ServerForfeitDuel(DuelGuid) (/duel forfeit); all on the pawn
//    the fight     while it is fought his PvP side names the duel (combat/pvp.h Side::duel: the two harm each other, nobody is flagged) and his pawn holds the
//                  Duel alignment {the duel's guid}; a blow of his partner's that would kill him leaves him 1 Health and loses the duel; another player's
//                  harm, or his death, ends it
//    the end       the result as a callout to both, and the Health back (EndOfDuelHealthRecoveryPercentage)
// ============================================================================
#include "session/player_session.h"

#include "bootstrap/constants.h"
#include "combat/combat_world.h"
#include "content/content_db.h"
#include "protocol/repl/rpc_fields.h"
#include "session/player_session_util.h"
#include "session/player_world.h"
#include "session/rpc_table.h"

#include <spdlog/spdlog.h>

#include <limits>

namespace aoc { namespace worldd {

void PlayerSession::register_duels_rpcs(RpcTable& t) {
    t.add(RpcScope::Pawn, "ServerRequestDuel", [](PlayerSession& s, const RpcContext& c) { s.on_duel_request(c.conn, c.rpc, c.now); });
    t.add(RpcScope::Pawn, "ServerAcceptDuel", [](PlayerSession& s, const RpcContext& c) { s.on_duel_answer(c.conn, c.rpc, true, c.now); });
    t.add(RpcScope::Pawn, "ServerDeclineDuel", [](PlayerSession& s, const RpcContext& c) { s.on_duel_answer(c.conn, c.rpc, false, c.now); });
    t.add(RpcScope::Pawn, "ServerForfeitDuel", [](PlayerSession& s, const RpcContext& c) { s.on_duel_forfeit(c.conn, c.rpc, c.now); });
}

void PlayerSession::connect_duels(PlayerWorld& world) {
    duel_book_ = world.duel_book();
    if (duel_book_) duel_book_->connect(pb_.setup().character_guid, name(), pb_.setup().guids.pawn.ObjectId);
}

void PlayerSession::leave_duels(Clock::time_point now) {
    if (duel_book_) duel_book_->disconnect(pb_.setup().character_guid, pvp_world_now(now));
    duel_book_.reset();
}

void PlayerSession::duel_callout(Connection& conn, const std::string& text, Clock::time_point now) {
    if (text.empty() || !room_for(2)) return;
    quest_toast(conn, text, now);
}

void PlayerSession::on_duel_request(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!duel_book_) return;
    const auto* s = rpc.call.decoded ? repl::rpcf::as<std::string>(repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "InCharacterName")) : nullptr;
    const std::string target = s ? *s : std::string();
    const std::string refusal = dead_ ? std::string("You cannot duel while dead.") : duel_book_->request(pb_.setup().character_guid, target, pvp_world_now(now));
    if (!refusal.empty()) {
        spdlog::log(level_for(duels_logged_, 20), "[duel] {}: a duel with {} is refused: {}", name(), target.empty() ? "nobody" : target, refusal);
        duel_callout(conn, refusal, now);
    }
}

void PlayerSession::on_duel_answer(Connection& conn, const repl::ClientRpc& rpc, bool accept, Clock::time_point now) {
    (void)conn;
    if (!duel_book_) return;
    world::DuelGuid duel{};
    constexpr const char* kWords[4] = {"DuelGuid.A", "DuelGuid.B", "DuelGuid.C", "DuelGuid.D"};
    for (size_t i = 0; rpc.call.decoded && i < 4; ++i) {
        const auto* v = repl::rpcf::as<int64_t>(repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, kWords[i]));   // (a word at 0 is not on the wire)
        duel[i] = v ? static_cast<uint32_t>(*v) : 0;
    }
    const bool ok = accept ? duel_book_->accept(pb_.setup().character_guid, duel, pvp_world_now(now)) : duel_book_->decline(pb_.setup().character_guid, duel, pvp_world_now(now));
    if (!ok) spdlog::log(level_for(duels_logged_, 20), "[duel] {}: {} names no duel request of his", name(), accept ? "ServerAcceptDuel" : "ServerDeclineDuel");
}

void PlayerSession::on_duel_forfeit(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)conn;
    (void)rpc;   // (the client sends the instigator's guid, which is the duel's; his own duel is ended whatever it says)
    if (duel_book_ && !duel_book_->forfeit(pb_.setup().character_guid, pvp_world_now(now))) spdlog::log(level_for(duels_logged_, 20), "[duel] {}: a forfeit, but he is in no duel", name());
}

void PlayerSession::drain_duels(Connection& conn, Clock::time_point now) {
    if (!duel_book_) return;
    if (duel_heal_at_ && now >= *duel_heal_at_) {   // EndOfDuelHealthRecoveryPercentage
        duel_heal_at_.reset();
        if (!dead_ && stats_ && stats_->started()) {
            const stats::StatValue* max_hp = stats_->find(content::kGuidMaxHealth);
            const stats::StatValue* hp = stats_->find(content::kGuidHealth);
            const double want = max_hp ? max_hp->value * duel_book_->settings().health_recovery : 0.0;
            if (hp && hp->value < want) set_current_value(content::kGuidHealth, static_cast<float>(want));
        }
    }
    for (const world::DuelNotice& n : duel_book_->take_notices(pb_.setup().character_guid)) {
        switch (n.kind) {
            case world::DuelNotice::Kind::Sent:
            case world::DuelNotice::Kind::Declined:
                duel_callout(conn, n.text, now);
                break;
            case world::DuelNotice::Kind::Asked:     // (his client opens the window from the banner's Duel)
            case world::DuelNotice::Kind::Counting:  // (his client counts down from the banner's SyncBeginTime)
                break;
            case world::DuelNotice::Kind::Started:
                ++duels_fought_;
                break;
            case world::DuelNotice::Kind::Ended:
                duel_callout(conn, n.text, now);
                if (n.won) ++duels_won_;
                if (n.fought) duel_heal_at_ = now + std::chrono::milliseconds(250);
                break;
        }
    }
}

bool PlayerSession::duel_filter(combat::IncomingHit& h, Clock::time_point now) {
    if (!duel_book_ || !h.from_player || h.instigator == 0 || h.instigator == pb_.setup().guids.pawn.ObjectId) return false;
    const world::DuelBook::Duel* d = duel_book_->duel_of(pb_.setup().character_guid);
    if (!d) return false;
    bool harms = false;
    for (const combat::ResolvedMod& m : h.mods) harms = harms || (m.stat_guid == content::kGuidHealth && m.value < 0);
    harms = harms || h.hostile;
    if (!harms) return false;
    const uint64_t partner = duel_book_->partner_pawn(pb_.setup().character_guid);
    if (partner != 0 && h.instigator == partner) {
        // his duel partner's blow may not kill him: it leaves him 1 Health, and he loses the duel
        const stats::StatValue* hp = stats_ ? stats_->find(content::kGuidHealth) : nullptr;
        if (!hp || (h.flags & stats::kHitMiss) != 0) return false;
        double taken = 0;
        for (const combat::ResolvedMod& m : h.mods) {
            if (m.stat_guid == content::kGuidHealth && m.value < 0) taken -= m.value;
        }
        const double health = hp->value;
        if (health - taken >= 1.0) return false;
        const double may = std::max(0.0, health - 1.0);
        const double share = taken > 0 ? may / taken : 0.0;
        for (combat::ResolvedMod& m : h.mods) {
            if (m.stat_guid == content::kGuidHealth && m.value < 0) m.value *= share;
        }
        spdlog::info("[duel] {}: a blow of his duel partner's would kill him ({:.0f} of {:.0f} Health): he keeps 1 and loses", name(), taken, health);
        return true;
    }
    // another player harms one of the two: the duel is over
    const world::WorldActor* by = registry_ ? registry_->find(h.instigator) : nullptr;
    if (by && by->kind == world::ActorKind::Player) {
        duel_book_->interrupted(pb_.setup().character_guid, "another player joined the fight", pvp_world_now(now));
    }
    return false;
}

void PlayerSession::duel_died(Clock::time_point now) {
    if (duel_book_ && duel_book_->duel_of(pb_.setup().character_guid)) duel_book_->interrupted(pb_.setup().character_guid, name() + " died", pvp_world_now(now));
}

}}  // namespace aoc::worldd
