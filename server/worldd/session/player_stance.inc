// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_stance.inc: a player's combat stance as the other players see it (Social, seeing other players: their combat stances)
//  Included at the end of session/player_shown.cpp; its declarations are in session/members/stance.inc.
//
//  A stance is not a message of its own: it is bits of the player's pawn's UPlayerCombatInfo, replicated to every client that holds the pawn.  bIsTargeting is the
//  draw (the client's own ServerSetTargeting asks for it), bInCombat the fight (the world's flag goes up with the first exchange and ends six seconds after the last,
//  combat_config.in_combat_timer), bPvPForceFlag the flag the client sets (ServerSetPvpFlag), bInPVPCombat the fight with another player (session/player_pvp.cpp),
//  and InPvpSuppressionArea has no source yet: this world has no suppression area, so it stays 0.  A dead player's bInCombat, bIsTargeting and bInPVPCombat are 0 in
//  the death's update.
//
//  The name colour comes from the AlignmentComponent, not from the flag bit: the game's own records carry the colour (Combatant purple, NonCombatant white, Corrupted
//  red), and a player carries one of the three.  The data's sequence puts the Combatant on a player at his first harmful action, and that is what session/player_pvp.cpp
//  does now (a flagging action, its 60 s removal timer refreshed by each one); the flag bit alone does not add it.  The colour itself is the game's own record's, and
//  the other instances of the container stay as they are (combat/pvp.cpp's merge_container).
//
//  The Combatant stays for 60 s after the last harmful action that flagged (the record's RemovalTimer, run by session/player_pvp.cpp), and a mount keeps the
//  alignments its owner had when it was summoned (the mount copies them once, session/player_mounts.cpp).
//
//  Modelled too: no recording shows another player's pawn receive these (nothing records an observer); the shape is the player's own reply's, kept current on the
//  pawn the watchers hold; and nothing records the draw (bIsTargeting), so its clear at death is ours.  A request over the draw's budget is dropped, and a request that
//  repeats the bit changes nothing (as the flag's and the target's do).
// ============================================================================

void PlayerSession::sync_shown_stance() {
    // what the player's own pawn says now: a dead player has no fight and no draw (the death's update carries bIsDead with them)
    const bool targeting = targeting_ && !dead_;
    const bool in_combat = combat_flag_ && !dead_;
    const bool pvp_flag = pvp_flag_;
    const bool suppression = false;   // (this world has no PvP suppression area)
    const bool pvp_combat = pvp_combat_ && !dead_;   // (session/player_pvp.cpp: a harmful hit with another player, for the in-combat time)
    const bool bits_change = targeting != shown_targeting_ || in_combat != shown_in_combat_ || pvp_flag != shown_pvp_flag_ || suppression != shown_suppression_ ||
                             pvp_combat != shown_pvp_combat_;
    if (!bits_change && !shown_alignment_dirty_) return;   // (the stance of the shown pawn moves only on a change)
    bool ok = true;
    const bool done = show_in_pawn(world::Change::State, [&](world::WorldActor& w) {
        if (bits_change) {
            bootstrap::PlayerBootstrap::ObserverStance stance;
            stance.targeting = targeting;
            stance.in_combat = in_combat;
            stance.pvp_flag = pvp_flag;
            stance.suppression = suppression;
            stance.pvp_combat = pvp_combat;
            ok = pb_.set_observer_stance(w.actor, stance) && ok;
        }
        if (shown_alignment_dirty_) ok = pb_.set_observer_alignments(w.actor) && ok;
    });
    if (!done || !ok) {
        if (++shown_failures_ <= kLoggedPerKind) {
            spdlog::warn("[players] {}: the pawn the other players are shown does not take its combat stance ({}): they do not see it", name(),
                         done ? "a value does not fit" : "the pawn is not in the world");
        }
        return;   // (tried again at the next tick)
    }
    if (bits_change) {
        shown_targeting_ = targeting;
        shown_in_combat_ = in_combat;
        shown_pvp_flag_ = pvp_flag;
        shown_suppression_ = suppression;
        shown_pvp_combat_ = pvp_combat;
        std::string what;
        if (targeting) what = "the draw";
        if (in_combat) what += std::string(what.empty() ? "" : ", ") + "a fight";
        if (pvp_flag) what += std::string(what.empty() ? "" : ", ") + "the PvP flag";
        spdlog::log(level_for(stance_logged_), "[players] {}: the other players are shown its combat stance: {}", name(), what.empty() ? std::string("neither the draw, a fight nor the flag") : what);
    }
    shown_alignment_dirty_ = false;
}

void PlayerSession::on_set_targeting(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    ++targeting_calls_;
    const auto want = rpc.call.decoded ? repl::rpcf::bool_or(repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "bNewTargeting"), false) : std::nullopt;
    if (!want) {
        if (++quiet_unreadable_logged_ <= kLoggedPerKind) spdlog::warn("[session] {}: a ServerSetTargeting does not read ({} parameter bits): the draw is not changed (further ones are not logged)", name(), rpc.call.param_bits);
        return;
    }
    if (!within_budget(targeting_budget_, "ServerSetTargeting", now) || !room_for(1)) {
        ++targeting_refused_;
        return;
    }
    if (*want == targeting_) return;   // (the bit is the bit: the game's server marks it and sends nothing, as nothing changed)
    if (dead_) return;                // (a dead player draws nothing; the death cleared the bit)
    targeting_ = *want;
    ++targeting_changes_;
    if (!pb_.set_targeting(targeting_)) {
        spdlog::warn("[session] {}: the pawn does not take CombatInfo.bIsTargeting: the client is not told", name());
        return;
    }
    // the player's own client is answered as the target's and the flag's calls are, with the property update 84 ms after the call (modelled for the draw: no recording holds this call),
    // and the pawn the other players are shown follows at this session's next tick
    queue_later("CombatInfo (targeting)", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("targeting"); }, conn, now, /*must_build=*/false);
    spdlog::log(level_for(stance_logged_, 20), "[session] {}: the draw is {}", name(), targeting_ ? "out" : "away");
}

/// The client call of the draw (session/rpc_table.h); the flag's own call stays with the quiet requests (session/player_requests.cpp).
void PlayerSession::register_stance_rpcs(RpcTable& t) {
    t.add(RpcScope::Pawn, "ServerSetTargeting", [](PlayerSession& s, const RpcContext& c) { s.on_set_targeting(c.conn, c.rpc, c.now); });
}
