// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// Rest and Sprint end: contributed by war-drayv in #20.
// ============================================================================
//  tests/ability_session_rest_end_tests.inc: ServerEndAbility ends the Rest and the Sprint by the server's instance id,
//  and a fight ends a running Rest
//  Registered with AOC_REGISTER_TEST; included from ability_session_registered.inc.
// ============================================================================

/// ServerEndAbility with the server's instance id (the number the ability item carries).
static void end_instance(Run& r, int64_t instance) { r.send_reliable(end_bunch(r.h.ability_guid(), instance)); }

static void test_rest_and_sprint_end_by_server_instance() {
    std::printf("ServerEndAbility names the server's instance: it ends the Rest and the Sprint\n");
    // the Rest: the client's own id is not the server's instance
    {
        Run e;
        e.next_id = 42;
        e.play(0.5, false);
        const int32_t id = e.trigger(kRest);
        e.play(2.2, false);
        CHECK(e.rt().resting() && e.m.has_effect(kRestEffect), "(the Rest is on)");
        const auto server = e.m.server_of.find(id);
        CHECK(server != e.m.server_of.end(), "(the Rest's item names its ServerInstanceId)");
        if (server != e.m.server_of.end()) end_instance(e, server->second);
        e.play(0.1, false);
        bool jumped = false;
        for (const auto& [eid, a] : e.m.abilities) jumped = jumped || (a.ability == kRest && a.may_jump && a.montage_position == AbilityRuntime::kRestSitEndPosition);
        CHECK(jumped && !e.rt().resting(), "ServerEndAbility stands the character up: the Rest ends and the montage jumps to its end section");
        e.play(0.3, false);
        CHECK(!e.m.has_effect(kRestEffect) && e.m.stats[kHealthRegen] == 0.6f && e.m.stats[kManaRegen] == 0.6f,
              "the Rest's effect is removed a moment later and the regeneration is back");
        CHECK(!e.m.end_before_removal && !e.h.s->abilities_failed() && e.m.unreadable == 0, "(nothing failed)");
    }
    // the Sprint: the same server id, and the client is not told again
    {
        Run s;
        s.next_id = 7;
        s.play(0.5, true);
        const int32_t id = s.trigger(kSprint);
        s.play(0.5, true);
        CHECK(s.rt().sprinting() && s.m.has_ability(kSprint), "(the Sprint runs)");
        const auto server = s.m.server_of.find(id);
        CHECK(server != s.m.server_of.end(), "(the Sprint's item names its ServerInstanceId)");
        if (server != s.m.server_of.end()) end_instance(s, server->second);
        s.play(0.3, true);
        CHECK(!s.rt().sprinting() && !s.m.has_ability(kSprint) && !s.m.has_effect(kSprintEffect) && s.m.stats[kMoveSpeedMult] == 1.0f,
              "ServerEndAbility with the server's instance id ends the Sprint and its effect");
        CHECK(s.m.end_ids().empty(), "... and the client is not told again: it ended it");
    }
}
AOC_REGISTER_TEST(test_rest_and_sprint_end_by_server_instance);

/// A creature's hit ends a running Rest: the fight starts and the Rest effect breaks.
static void test_a_mobs_hit_ends_the_rest() {
    std::printf("a mob's hit ends the Rest: the fight starts and the Rest effect breaks\n");
    const svc::Character rec = bard25();
    std::vector<uint64_t> wolves;
    uint64_t far_wolf = 0, next_actor_id = 5000000;
    bool ground = false;
    auto prepare = [&](PlayerWorld& w) { spawn_wolves(w, wolves, far_wolf, 1, 24, 800, ground, next_actor_id); };
    Run r(0, true, &rec, prepare);
    CHECK(ground && wolves.size() == 1 && r.h.fx.world->combat(), "a world with ground and a Wolf at the player's side");
    if (!ground || wolves.empty() || !r.h.fx.world->combat()) return;
    combat::CombatWorld& cw = *r.h.fx.world->combat();
    r.play(0.5, false);
    phase_in(r);
    const float regen_out_of_combat = r.m.stats[kHealthRegen];
    r.trigger(kRest);
    r.play(2.2, false);
    CHECK(r.rt().resting() && r.m.has_effect(kRestEffect), "(the Rest runs with its effect, out of combat)");
    bool in_combat = false;
    for (int i = 0; i < 80 && !in_combat; ++i) {
        r.play(0.1, false);
        r.h.fx.world->tick_world(r.now());
        in_combat = r.h.s->character_stats() && r.h.s->character_stats()->in_combat();
    }
    r.play(0.3, false);
    const auto fighting = cw.ai_state(wolves[0]);
    CHECK(in_combat && fighting.has_value() && *fighting != npc::ai::State::Idle, "(the Wolf fights and the player is in a fight)");
    CHECK(!r.rt().resting() && !r.m.has_effect(kRestEffect), "the mob's hit starts the fight and the Rest ends: its effect goes");
    // in a fight the regeneration is five times slower than the character's own out of it, before the Rest
    CHECKF(regen_out_of_combat > 0 && std::fabs(r.m.stats[kHealthRegen] * 5 - regen_out_of_combat) < 0.01f * regen_out_of_combat,
           "... and the regeneration is the fight's, without the Rest's (%.3f a second; %.3f out of combat)", r.m.stats[kHealthRegen],
           regen_out_of_combat);
    CHECK(!r.h.s->abilities_failed() && r.m.unreadable == 0, "(nothing failed)");
}

AOC_REGISTER_TEST(test_a_mobs_hit_ends_the_rest);

/// Another player's hit ends a running Rest: the fight starts and the Rest effect breaks.
static void test_another_players_hit_ends_the_rest() {
    std::printf("another player's hit ends the Rest: the fight starts and the Rest effect breaks\n");
    using namespace pvp_tests;
    Duo d;
    const bool started = d.start();
    CHECK(started, "two players in one world with ground, both with the 1H axe combo");
    if (!started) return;
    Run& a = *d.a;
    const float regen_out_of_combat = a.m.stats[kHealthRegen];
    a.trigger(kRest);
    d.both(2.2);
    CHECK(a.rt().resting() && a.m.has_effect(kRestEffect), "(the Rest runs with its effect, out of combat)");
    d.swing(*d.b, d.sb, d.sa, /*force=*/true);
    d.both(0.3);
    CHECK(!a.rt().resting() && !a.m.has_effect(kRestEffect), "the other player's hit starts the fight and the Rest ends: its effect goes");
    // in a fight the regeneration is five times slower than the character's own out of it, before the Rest
    CHECKF(regen_out_of_combat > 0 && std::fabs(a.m.stats[kHealthRegen] * 5 - regen_out_of_combat) < 0.01f * regen_out_of_combat,
           "... and the regeneration is the fight's, without the Rest's (%.3f a second; %.3f out of combat)", a.m.stats[kHealthRegen],
           regen_out_of_combat);
    CHECK(a.h.s->character_stats() && a.h.s->character_stats()->in_combat(), "(the player is in a fight)");
    CHECK(!a.h.s->abilities_failed() && a.m.unreadable == 0, "(nothing failed)");
}

AOC_REGISTER_TEST(test_another_players_hit_ends_the_rest);

/// An id that is no Rest's and no Sprint's still reaches the pipeline: a cast bar the client ends is cancelled.
static void test_an_id_the_runtime_does_not_know_still_ends_the_cast_bar() {
    std::printf("ServerEndAbility of an id no Rest or Sprint has still reaches the pipeline: the cast bar ends\n");
    const svc::Character rec = bard25();
    Run r(0, true, &rec);
    r.play(0.5, true);
    const int32_t id = trigger_aimed_now(r, kQuickWit, true);
    r.play(0.4, false);
    int64_t server_id = -1;
    for (const auto& [k, a] : r.m.abilities)
        if (a.ability == kQuickWit) server_id = a.instance;
    CHECK(server_id >= 0 && !r.rt().sprinting() && !r.rt().resting(), "(the cast bar's server instance is no Rest's and no Sprint's)");
    const size_t ends = r.m.end_ids().size();
    r.send_reliable(end_bunch(r.h.ability_guid(), server_id));
    r.play(0.6, false);
    CHECK(r.m.end_ids().size() == ends + 1 && r.m.end_ids().back() == id,
          "the end reaches the pipeline: the cast bar is cancelled and ClientEndAbility(the client's instance) follows");
    CHECK(!r.h.s->abilities_failed() && r.m.unreadable == 0, "(nothing failed)");
}

AOC_REGISTER_TEST(test_an_id_the_runtime_does_not_know_still_ends_the_cast_bar);

/// An id that matches no Rest, no Sprint and no cast of the pipeline is logged once, with the id.
static void test_an_end_of_no_instance_is_logged_once() {
    std::printf("ServerEndAbility of an id nothing owns is logged once, with the id\n");
    Run r;
    r.play(0.5, false);
    g_log.str("");
    r.send_reliable(end_bunch(r.h.ability_guid(), 424242));
    r.send_reliable(end_bunch(r.h.ability_guid(), 424243));
    r.play(0.2, false);
    spdlog::default_logger()->flush();
    const std::string log = g_log.str();
    CHECK(log.find("424242") != std::string::npos && count_lines(log, "ServerEndAbility", "no cast of the pipeline") == 1,
          "the log names the id once (a second id does not write another line)");
}

AOC_REGISTER_TEST(test_an_end_of_no_instance_is_logged_once);
