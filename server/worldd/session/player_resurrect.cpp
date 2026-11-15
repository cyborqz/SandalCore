// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_resurrect.cpp: another player's Resurrect (abilities between players; the members are in session/members/resurrect.inc)
//
//  A hit whose record names a ResurrectId (Cleric_Resurrect) reaches only a dead player (session/ability_pipeline.cpp); the world tells his session
//  (combat/combat_players.cpp resurrect_player), which asks his client: ClientPromptResurrect(InstigatorGuid), the alert that names the caster.  The client answers
//  with ServerPromptedResurrectResponse(Response).  Accepted, he comes back where he fell with the record's 25% of his Health and Mana and all of his Stamina
//  (finish_respawn, respawn/session_embersprings.cpp); declined, he stays dead with the respawn window, as before.  A respawn of his own ends the offer.
// ============================================================================
#include "session/player_session.h"

#include "session/abilities.h"

#include <spdlog/spdlog.h>

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;

void PlayerSession::offer_resurrect(Connection& conn, const std::array<uint32_t, 4>& by) {
    if (!dead_) return;
    resurrect_offered_ = true;
    spdlog::log(level_for(death_logged_), "[session] {}: another player's Resurrect landed: the client is asked", name());
    queue("ClientPromptResurrect", [by](bootstrap::PlayerBootstrap& pb) { return pb.prompt_resurrect(by); }, conn, /*must_build=*/true);
}

void PlayerSession::on_resurrect_response(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!dead_ || !resurrect_offered_) return;
    if (!within_budget(loot_budget_, "ServerPromptedResurrectResponse", now) || !room_for(4)) return;
    resurrect_offered_ = false;
    // (a bool parameter that is false is not on the wire)
    const auto it = rpc.call.params.values.find(1);
    const bool* accepted = it == rpc.call.params.values.end() ? nullptr : it->second.get_if<bool>();
    if (!accepted || !*accepted) {
        spdlog::log(level_for(death_logged_), "[session] {}: declined the Resurrect", name());
        return;
    }
    // the record Cleric_Resurrect: HealthRatio 0.25, ManaRatio 0.25, StaminaRatio 1, where it fell
    finish_respawn(conn, PlayerWorld::RespawnSpot{death_x_, death_y_, death_z_, checker_.good().yaw_degrees, false, "where it fell, raised by another player"}, now, 0.25f, 0.25f);
    if (abilities_) {
        bootstrap::AbilityChange refill;
        abilities_->refill_stamina(refill);
        if (stats_) stats_->set_current(content::kGuidStamina, static_cast<float>(abilities_->stamina()));
        if (pb_.apply_ability_change(refill)) abilities_dirty_ = true;
    } else if (const stats::StatValue* sp = stats_ ? stats_->find(content::kGuidMaxStamina) : nullptr) {
        set_current_value(content::kGuidStamina, sp->value);
    }
}

/// The client call of the Resurrect alert (session/rpc_table.h).
void PlayerSession::register_resurrect_rpcs(RpcTable& t) {
    t.add(RpcScope::PlayerController, "ServerPromptedResurrectResponse", [](PlayerSession& s, const RpcContext& c) { s.on_resurrect_response(c.conn, c.rpc, c.now); });
}

}}  // namespace aoc::worldd
