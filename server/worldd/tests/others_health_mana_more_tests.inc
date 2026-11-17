// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  tests/others_health_mana_more_tests.inc: other players see a player's live Health and Mana, the cases that follow the first ones (Social, seeing other players: live Health and Mana)
//
//  Included after tests/others_health_mana_tests.inc (its helpers: stat_in, stat_deltas_on, shown_stat, session_stat).  A player who dies and is revived (the bars of a dead player: the
//  next milestone adds the death itself on top of them), and changes that cancel out inside one tick.
// ============================================================================

static void test_a_dead_players_bars_fall_to_zero_stay_there_and_fill_again_at_the_revive() {
    std::printf("a player who dies is shown with empty Health and Mana bars that stay empty, and with full bars after the revive; the pawn stays in the watcher's world\n");
    Scene sc;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    const world::WorldActor* pawn = shown_pawn(a);
    CHECK(pawn != nullptr && sc.channel(pawn ? pawn->id : 0) != 0, "(the watcher holds the player's pawn)");
    if (!pawn) return;
    const uint64_t pawn_id = pawn->id;
    const uint32_t pch = sc.channel(pawn_id);
    const repl::Actor pawn_copy = pawn->actor;
    const float max_h = session_stat(*a.s, content::kGuidMaxHealth), max_m = session_stat(*a.s, content::kGuidMaxMana);
    const size_t from = b.got.size();
    CHECK(a.s->set_current_value(content::kGuidHealth, 0.0f), "(the player's Health falls to 0)");
    a.run(milliseconds(300));
    CHECK(a.s->dead(), "(the player is dead)");
    const auto health = stat_deltas_on(b, pch, pawn_copy, from, content::kGuidHealth);
    const auto mana = stat_deltas_on(b, pch, pawn_copy, from, content::kGuidMana);
    CHECK(!health.empty() && health.back().value == 0.0f, "the watcher is sent Health 0");
    CHECK(!mana.empty() && mana.back().value == 0.0f, "...and Mana 0");
    CHECK(shown_stat(*shown_pawn(a), content::kGuidHealth) == 0.0f && shown_stat(*shown_pawn(a), content::kGuidMana) == 0.0f, "the pawn the watchers hold has both at 0");
    CHECK(shown_stat(*shown_pawn(a), content::kGuidMaxHealth) == max_h && shown_stat(*shown_pawn(a), content::kGuidMaxMana) == max_m, "...and keeps its maxima");
    CHECK(closes_on(b, pch, from).empty() && sc.channel(pawn_id) == pch, "the pawn stays in the watcher's world (the death itself is the next milestone's)");
    // a dead player does not regenerate
    const size_t from_dead = b.got.size();
    a.run(milliseconds(3000));
    CHECK(stat_deltas_on(b, pch, pawn_copy, from_dead, content::kGuidHealth).empty() && stat_deltas_on(b, pch, pawn_copy, from_dead, content::kGuidMana).empty(),
          "a dead player's bars do not move: nothing regenerates");
    // the revive: full bars on the same pawn
    const size_t from_revive = b.got.size();
    const PlayerSession::GmReviveResult revived = a.s->gm_revive(a.conn, a.t, "the test");
    CHECK(revived.ok && !a.s->dead(), "(the player is revived)");
    a.run(milliseconds(500));
    const auto health2 = stat_deltas_on(b, pch, pawn_copy, from_revive, content::kGuidHealth);
    const auto mana2 = stat_deltas_on(b, pch, pawn_copy, from_revive, content::kGuidMana);
    CHECK(!health2.empty() && health2.back().value == max_h, "the watcher is sent the full Health after the revive");
    CHECK(!mana2.empty() && mana2.back().value == max_m, "...and the full Mana");
    CHECK(sc.channel(pawn_id) == pch, "(the same pawn, on the same channel)");
}
AOC_REGISTER_TEST(test_a_dead_players_bars_fall_to_zero_stay_there_and_fill_again_at_the_revive);

static void test_a_hurt_and_a_heal_inside_one_tick_send_nothing() {
    std::printf("a hurt and a heal inside one tick cancel out: the watcher is sent no Health delta\n");
    Scene sc;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    const world::WorldActor* pawn = shown_pawn(a);
    CHECK(pawn != nullptr && sc.channel(pawn ? pawn->id : 0) != 0, "(the watcher holds the player's pawn)");
    if (!pawn) return;
    const uint32_t pch = sc.channel(pawn->id);
    const repl::Actor pawn_copy = pawn->actor;
    const float max_h = session_stat(*a.s, content::kGuidMaxHealth);
    const size_t from = b.got.size();
    CHECK(a.s->set_current_value(content::kGuidHealth, max_h - 30.0f), "(the player is hurt)");
    CHECK(a.s->set_current_value(content::kGuidHealth, max_h), "(and healed to full before the tick)");
    a.run(milliseconds(300));
    CHECK(session_stat(*a.s, content::kGuidHealth) == max_h, "(Health is full)");
    CHECK(stat_deltas_on(b, pch, pawn_copy, from, content::kGuidHealth).empty(), "the watcher is sent nothing: the pawn it holds already had the full Health");
}
AOC_REGISTER_TEST(test_a_hurt_and_a_heal_inside_one_tick_send_nothing);

// ---- a crowd in one place ---------------------------------------------------------------------------------------------------------------------

static void test_a_crowd_of_players_each_send_a_watcher_about_one_mana_delta_a_second() {
    std::printf("a crowd of players in one place: each one's Mana is sent to a watcher about once a second while it regenerates, not once a tick\n");
    Harness a(cyb_record());
    a.play();
    Players all;
    std::vector<std::unique_ptr<Harness>> crowd;
    const char* names[] = {"ayla", "kira", "mira", "tess", "nori"};
    for (int i = 0; i < 5; ++i) crowd.push_back(join_world(a, all, other_record(other_id(2 + i), names[i], 4 + i)));
    a.run(milliseconds(500));
    // every one of them is short of Mana by four regeneration steps
    std::vector<uint32_t>   channels;
    std::vector<repl::Actor> copies;
    for (const auto& h : crowd) {
        const world::WorldActor* p = shown_pawn(*h);
        const uint32_t ch = p && a.s->actor_view() ? a.s->actor_view()->channel_of(p->id) : 0;
        CHECK(p != nullptr && ch != 0, "(the watcher holds the crowd member's pawn)");
        if (!p || ch == 0) return;
        channels.push_back(ch);
        copies.push_back(p->actor);
        const float max_m = session_stat(*h->s, content::kGuidMaxMana), regen = session_stat(*h->s, content::kGuidManaRegeneration);
        CHECK(h->s->set_current_value(content::kGuidMana, std::max(0.0f, max_m - regen * 4.0f)), "(a crowd member's Mana is set below its maximum)");
    }
    a.run(milliseconds(300));   // (the sets' own deltas go first: the window starts after them)
    const size_t from = a.got.size();
    a.run(milliseconds(5000));
    size_t total = 0;
    for (size_t i = 0; i < crowd.size(); ++i) {
        const auto deltas = stat_deltas_on(a, channels[i], copies[i], from, content::kGuidMana);
        total += deltas.size();
        CHECK(deltas.size() >= 2 && deltas.size() <= 6, ("five seconds of one crowd member's regeneration are two to six Mana deltas to the watcher, not one a tick: " + std::to_string(deltas.size())).c_str());
    }
    CHECK(total <= 5 * 6, "...and the five of them together send the watcher no more than six a second each");
}
AOC_REGISTER_TEST(test_a_crowd_of_players_each_send_a_watcher_about_one_mana_delta_a_second);
