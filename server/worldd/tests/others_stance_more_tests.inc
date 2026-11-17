// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  tests/others_stance_more_tests.inc: more of other players' combat stances (Social, seeing other players: their combat stances; the first part is others_stance_tests.inc)
//
//  What the first file does not reach: a fight that goes on (the six seconds count from the LAST exchange), a player who dies in a fight and is brought back (no fight and no draw
//  come back with him, the flag stays, the draw and a new fight are shown again), a draw and a death in the same tick, the PvP flag turned on, off and on again in one tick, and what the
//  player's OWN client is told (his draw, his flag with the name it gives, his death frame).
// ============================================================================

/// The first chain of the player's OWN client on his pawn's channel from `from` on that carries the CombatInfo bit `path` as `want`, with the rest of the stance (and the Alignment) in
/// that same chain.
static std::optional<StanceFrame> own_stance_chain(const Harness& h, size_t from, const char* path, int64_t want) {
    const bootstrap::SessionGuids& g = h.s->bootstrap().setup().guids;
    for (size_t i = from; i < h.got.size(); ++i) {
        const fake::Delivered& d = h.got[i];
        if (d.channel != h.pawn_ch || d.close || d.open) continue;
        const auto f = stance_in(d, h.s->bootstrap().pawn(), g.combat_info.ObjectId, g.alignment.ObjectId, h.got_at[i]);
        if (f && frame_bit(*f, path) == want) return f;
    }
    return std::nullopt;
}

// ---- a fight that goes on ----------------------------------------------------------------------------------------------------------------------

static void test_a_fight_that_goes_on_ends_six_seconds_after_its_last_exchange() {
    std::printf("a fight that goes on ends six seconds after its LAST exchange: the second exchange sends the watcher nothing and moves the end\n");
    StanceScene sc;
    CHECK(sc.ok(), "(the world's fights are on)");
    if (!sc.ok()) return;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    const world::WorldActor* pawn = shown_pawn(a);
    CHECK(pawn != nullptr && sc.channel(pawn ? pawn->id : 0) != 0, "(the watcher holds the player's pawn)");
    if (!pawn) return;
    const uint32_t pch = sc.channel(pawn->id);
    const repl::Actor pawn_copy = pawn->actor;
    const uint64_t combat = a.s->bootstrap().setup().guids.combat_info.ObjectId;
    const uint64_t alignment = a.s->bootstrap().setup().guids.alignment.ObjectId;
    const size_t from = b.got.size();
    exchange(a);
    a.run(milliseconds(300));
    CHECK(stance_chain(b, pch, pawn_copy, combat, alignment, from, "bInCombat", 1).has_value(), "the first exchange puts bInCombat 1 on the watcher's copy");
    a.run(milliseconds(3700));   // four seconds after the first exchange
    const size_t from_second = b.got.size();
    const Clock::time_point second_at = a.t;
    exchange(a);
    a.run(milliseconds(300));
    CHECK(!stance_chain(b, pch, pawn_copy, combat, alignment, from_second, "bInCombat", 1).has_value() &&
              !stance_chain(b, pch, pawn_copy, combat, alignment, from_second, "bInCombat", 0).has_value(),
          "the second exchange of the same fight sends the watcher no bInCombat at all (the bit is up already)");
    a.run(milliseconds(2500));   // 6.8 s after the first exchange, 2.8 s after the second
    CHECK(!stance_chain(b, pch, pawn_copy, combat, alignment, from, "bInCombat", 0).has_value(), "six seconds after the FIRST exchange he is still in combat: the second moved the end");
    CHECK(shown_bit(*shown_pawn(a), "bInCombat") == 1, "...and the pawn the watchers hold says in combat");
    a.run(milliseconds(4500));   // 7.3 s after the second exchange
    const auto off = stance_chain(b, pch, pawn_copy, combat, alignment, from, "bInCombat", 0);
    CHECK(off.has_value(), "he is out of combat after the second exchange's six seconds");
    CHECK(off && off->at - second_at >= milliseconds(5700) && off->at - second_at <= milliseconds(7200), "...six seconds after the LAST exchange (within a tick), not after the first");
    CHECK(shown_bit(*shown_pawn(a), "bInCombat") == 0, "the pawn the watchers hold says out of combat");
}
AOC_REGISTER_TEST(test_a_fight_that_goes_on_ends_six_seconds_after_its_last_exchange);

// ---- a death in a fight, and the way back --------------------------------------------------------------------------------------------------------

static void test_a_player_brought_back_after_a_death_in_a_fight_has_no_fight_and_no_draw() {
    std::printf("a player who dies in a fight and is brought back has no fight and no draw (the PvP flag stays); the draw and a new fight are shown again\n");
    StanceScene sc;
    CHECK(sc.ok(), "(the world's fights are on)");
    if (!sc.ok()) return;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    const world::WorldActor* pawn = shown_pawn(a);
    CHECK(pawn != nullptr && sc.channel(pawn ? pawn->id : 0) != 0, "(the watcher holds the player's pawn)");
    if (!pawn) return;
    const uint32_t pch = sc.channel(pawn->id);
    const repl::Actor pawn_copy = pawn->actor;
    const uint64_t combat = a.s->bootstrap().setup().guids.combat_info.ObjectId;
    const uint64_t alignment = a.s->bootstrap().setup().guids.alignment.ObjectId;
    a.client_sends({a.client.actor(a.pawn_ch, targeting_call(true), true)});
    a.client_sends({a.client.actor(a.pc_ch, pvp_flag_call(true), true)});
    exchange(a);
    a.run(milliseconds(300));
    CHECK(a.s->targeting() && a.s->pvp_flag() && shown_bit(*shown_pawn(a), "bInCombat") == 1, "(the player draws, flags and fights)");
    CHECK(a.s->gm_kill(a.conn, a.t, "the test").ok, "(the player dies in the fight)");
    a.run(milliseconds(500));
    CHECK(pawn_dead(*shown_pawn(a)) == 1, "(the watchers hold him dead)");
    const size_t from_revive = b.got.size();
    CHECK(a.s->gm_revive(a.conn, a.t, "the test").ok && !a.s->dead(), "(the player is brought back)");
    a.run(milliseconds(8000));   // longer than the fight's six seconds: an old fight cannot come back
    const world::WorldActor* back = shown_pawn(a);
    CHECK(back != nullptr && pawn_dead(*back) == 0, "the pawn the watchers hold says alive again");
    if (!back) return;
    const uint32_t pch2 = sc.channel(back->id);
    CHECK(pch2 == pch, "(the watcher still holds the same pawn on the same channel)");
    CHECK(shown_bit(*back, "bInCombat") == 0 && shown_bit(*back, "bIsTargeting") == 0, "...with no fight and no draw");
    CHECK(shown_bit(*back, "bPvPForceFlag") == 1, "...and the PvP flag, which a death does not take");
    const auto name = shown_alignments(*back);
    CHECK(name && names_alignment(*name, kStanceCombatant) && !names_alignment(*name, kStanceNonCombatant), "...with the Combatant name still");
    CHECK(!stance_chain(b, pch, pawn_copy, combat, alignment, from_revive, "bInCombat", 1).has_value(), "the watcher is never sent a fight after the revive");
    CHECK(!stance_chain(b, pch, pawn_copy, combat, alignment, from_revive, "bIsTargeting", 1).has_value(), "...and never a draw");
    CHECK(own_pawn_bit(a, "bInCombat") == 0 && own_pawn_bit(a, "bIsTargeting") == 0 && own_pawn_bit(a, "bIsDead") == 0, "the player's own pawn says alive with no fight and no draw");
    // the draw is heard again, and a new fight is shown
    const size_t from_draw = b.got.size();
    a.client_sends({a.client.actor(a.pawn_ch, targeting_call(true), true)});
    a.run(milliseconds(250));
    CHECK(a.s->targeting() && stance_chain(b, pch, pawn_copy, combat, alignment, from_draw, "bIsTargeting", 1).has_value(), "a draw after the revive is heard and shown to the watcher");
    exchange(a);
    a.run(milliseconds(300));
    CHECK(stance_chain(b, pch, pawn_copy, combat, alignment, from_draw, "bInCombat", 1).has_value(), "a new fight after the revive is shown to the watcher");
}
AOC_REGISTER_TEST(test_a_player_brought_back_after_a_death_in_a_fight_has_no_fight_and_no_draw);

static void test_a_draw_and_a_death_in_the_same_tick_show_no_draw() {
    std::printf("a player who draws and dies in the same tick is never shown drawn, and the draw is not heard again until he is back\n");
    StanceScene sc;
    CHECK(sc.ok(), "(the world's fights are on)");
    if (!sc.ok()) return;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    const world::WorldActor* pawn = shown_pawn(a);
    CHECK(pawn != nullptr && sc.channel(pawn ? pawn->id : 0) != 0, "(the watcher holds the player's pawn)");
    if (!pawn) return;
    const uint32_t pch = sc.channel(pawn->id);
    const repl::Actor pawn_copy = pawn->actor;
    const uint64_t combat = a.s->bootstrap().setup().guids.combat_info.ObjectId;
    const uint64_t alignment = a.s->bootstrap().setup().guids.alignment.ObjectId;
    const size_t from = b.got.size();
    a.client_sends({a.client.actor(a.pawn_ch, targeting_call(true), true)});
    CHECK(a.s->targeting(), "(the draw is heard)");
    CHECK(a.s->gm_kill(a.conn, a.t, "the test").ok, "(the player dies before the tick that would show the draw)");
    a.run(milliseconds(500));
    CHECK(!a.s->targeting() && own_pawn_bit(a, "bIsTargeting") == 0, "the draw went with the death");
    CHECK(!stance_chain(b, pch, pawn_copy, combat, alignment, from, "bIsTargeting", 1).has_value(), "the watcher is never sent bIsTargeting 1");
    CHECK(stance_chain(b, pch, pawn_copy, combat, alignment, from, "bIsDead", 1).has_value(), "...and is sent the death");
}
AOC_REGISTER_TEST(test_a_draw_and_a_death_in_the_same_tick_show_no_draw);

// ---- the flag three times in one tick ------------------------------------------------------------------------------------------------------------

static void test_the_flag_on_off_and_on_in_one_tick_is_shown_once_as_it_ended() {
    std::printf("the PvP flag turned on, off and on again before one tick is shown as it ended: one change to the Combatant name, no white name in between\n");
    StanceScene sc;
    CHECK(sc.ok(), "(the world's fights are on)");
    if (!sc.ok()) return;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    const world::WorldActor* pawn = shown_pawn(a);
    CHECK(pawn != nullptr && sc.channel(pawn ? pawn->id : 0) != 0, "(the watcher holds the player's pawn)");
    if (!pawn) return;
    const uint32_t pch = sc.channel(pawn->id);
    const repl::Actor pawn_copy = pawn->actor;
    const uint64_t combat = a.s->bootstrap().setup().guids.combat_info.ObjectId;
    const uint64_t alignment = a.s->bootstrap().setup().guids.alignment.ObjectId;
    const size_t from = b.got.size();
    a.client_sends({a.client.actor(a.pc_ch, pvp_flag_call(true), true), a.client.actor(a.pc_ch, pvp_flag_call(false), true), a.client.actor(a.pc_ch, pvp_flag_call(true), true)});
    a.run(milliseconds(400));
    CHECK(a.s->pvp_flag() && own_pawn_bit(a, "bPvPForceFlag") == 1, "(the flag ended on)");
    const auto on = stance_chain(b, pch, pawn_copy, combat, alignment, from, "bPvPForceFlag", 1);
    CHECK(on.has_value(), "the watcher is sent the flag on");
    CHECK(on && on->alignments && names_alignment(*on->alignments, kStanceCombatant) && !names_alignment(*on->alignments, kStanceNonCombatant), "...with the Combatant name and no NonCombatant");
    CHECK(!stance_chain(b, pch, pawn_copy, combat, alignment, from, "bPvPForceFlag", 0).has_value(), "the watcher is never sent the flag off in between");
    const auto shown = shown_alignments(*shown_pawn(a));
    const auto own = own_alignments(a);
    CHECK(shown && own && *shown == *own, "the pawn the watchers hold carries the very Alignments the player's own pawn does");
}
AOC_REGISTER_TEST(test_the_flag_on_off_and_on_in_one_tick_is_shown_once_as_it_ended);

// ---- what the player's own client is told ---------------------------------------------------------------------------------------------------------

static void test_the_players_own_client_is_told_his_draw_his_flag_with_its_name_and_his_death_frame() {
    std::printf("the player's own client is sent his draw, his flag with the name it gives, and a death frame with the fight and the draw gone\n");
    StanceScene sc;
    CHECK(sc.ok(), "(the world's fights are on)");
    if (!sc.ok()) return;
    Harness& a = sc.a;
    const size_t from_draw = a.got.size();
    a.client_sends({a.client.actor(a.pawn_ch, targeting_call(true), true)});
    a.run(milliseconds(300));
    CHECK(own_stance_chain(a, from_draw, "bIsTargeting", 1).has_value(), "his own client is sent bIsTargeting 1");
    const size_t from_flag = a.got.size();
    a.client_sends({a.client.actor(a.pc_ch, pvp_flag_call(true), true)});
    a.run(milliseconds(300));
    const auto flag = own_stance_chain(a, from_flag, "bPvPForceFlag", 1);
    CHECK(flag.has_value(), "...and bPvPForceFlag 1");
    CHECK(flag && flag->alignments && names_alignment(*flag->alignments, kStanceCombatant) && !names_alignment(*flag->alignments, kStanceNonCombatant),
          "...with the Combatant alignment in that same update (his own name)");
    exchange(a);
    a.run(milliseconds(300));
    CHECK(own_stance_chain(a, from_flag, "bInCombat", 1).has_value(), "...and bInCombat 1 with the fight");
    const size_t from_kill = a.got.size();
    CHECK(a.s->gm_kill(a.conn, a.t, "the test").ok, "(the player dies in the fight)");
    a.run(milliseconds(500));
    const auto death = own_stance_chain(a, from_kill, "bIsDead", 1);
    CHECK(death.has_value(), "his own client is sent bIsDead 1");
    CHECK(death && death->in_combat == 0 && death->targeting == 0, "...with bInCombat 0 and bIsTargeting 0 in that same update");
    const size_t from_off = a.got.size();
    CHECK(a.s->gm_revive(a.conn, a.t, "the test").ok, "(the player is brought back)");
    a.client_sends({a.client.actor(a.pc_ch, pvp_flag_call(false), true)});
    a.run(milliseconds(500));
    const auto off = own_stance_chain(a, from_off, "bPvPForceFlag", 0);
    CHECK(off.has_value() && off->alignments && names_alignment(*off->alignments, kStanceNonCombatant) && !names_alignment(*off->alignments, kStanceCombatant),
          "the flag off gives his own client the NonCombatant alignment back, with the bit");
}
AOC_REGISTER_TEST(test_the_players_own_client_is_told_his_draw_his_flag_with_its_name_and_his_death_frame);
