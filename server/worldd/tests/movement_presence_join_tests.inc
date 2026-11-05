// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/movement_presence_join_tests.inc: players seeing each other, the join and the leave (through the Endpoint, with the view passes on 0, 1 and 3 threads)
//
//  A player who joins and then stands still, with no move sent, is in the world's registry for the other players (an actor of kind Player, its pawn alone) within two ticks of its
//  phase-in, at the join spot, and the other player's view opens it; its own view never does.  The registry holds one Player actor for each player in the world, and a player who
//  quits, is kicked for a login elsewhere or times out takes it out again.
// ============================================================================
#include "movement_presence_rig.h"

/// Whether the Player actor stands where the characters join (kX, kY), to the centimetre the moves have.
static bool presence_at_join_spot(const world::WorldActor* a) { return a && near(a->x, kX) && near(a->y, kY); }

static void test_presence_idle_join_is_in_the_world_within_two_ticks() {
    std::printf("players seeing each other: a player who joins and stands still is in the world for the other players within two ticks, with 0, 1 and 3 view threads\n");
    char text[420];
    for (const unsigned threads : {0u, 1u, 3u}) {
        g_counter = 1;
        PresenceEndpoints e(threads);
        Rig& rig = *e.rig;
        const world::ActorRegistry& reg = e.fx.world->registry();
        Clock::time_point now = t0;
        std::vector<PresenceClient*> all;
        PresenceClient a(50510, "cyb"), b(50511, "ayla");
        presence_join(rig, all, a, cyb_record(), now);
        presence_join(rig, all, b, presence_record(presence_id(2), "ayla", 4), now);
        const int took_a = presence_phase_in(rig, all, a, now);
        const int took_b = presence_phase_in(rig, all, b, now);
        presence_idle(rig, all, now, 2);   // (nobody has sent a move: the clients only acknowledge)
        const uint64_t ida = presence_player_id(rig, a.addr), idb = presence_player_id(rig, b.addr);
        const world::WorldActor* pa = presence_pawn(reg, ida);
        const world::WorldActor* pb = presence_pawn(reg, idb);
        std::snprintf(text, sizeof(text), "%u view thread(s): two ticks after the phase-ins (%d and %d steps) both players are Player actors of the registry, at the join spot, with no move sent", threads,
                      took_a, took_b);
        CHECK(took_a >= 0 && took_b >= 0 && ida != 0 && idb != 0 && ida != idb && pa && pb && presence_count(reg) == 2 && presence_at_join_spot(pa) && presence_at_join_spot(pb), text);
        // ... and the other player's view opens it, its own never does
        presence_idle(rig, all, now, 12);
        const PlayerSession* sa = rig.ep.player_at(a.addr);
        const PlayerSession* sb = rig.ep.player_at(b.addr);
        const ActorView* va = sa ? sa->actor_view() : nullptr;
        const ActorView* vb = sb ? sb->actor_view() : nullptr;
        std::snprintf(text, sizeof(text), "%u view thread(s): each player's view opens the other's pawn and holds nothing else (one Player actor for each player)", threads);
        CHECK(va && vb && pa && pb && va->state_of(pb->id) == world::ClientState::Awake && vb->state_of(pa->id) == world::ClientState::Awake && va->tracked() == 1 && vb->tracked() == 1, text);
        std::snprintf(text, sizeof(text), "%u view thread(s): a player's own pawn is not one of its view's actors", threads);
        CHECK(va && vb && pa && pb && va->state_of(pa->id) == world::ClientState::None && vb->state_of(pb->id) == world::ClientState::None, text);
        CHECK(!reg.frozen(), "(the registry is thawed after the ticks)");
    }
}
AOC_REGISTER_TEST(test_presence_idle_join_is_in_the_world_within_two_ticks);

static void test_presence_leaves_with_the_player() {
    std::printf("players seeing each other: a player who quits, is kicked for a login elsewhere or times out leaves the registry, and its view's partner closes it\n");
    for (const unsigned threads : {0u, 3u}) {
        char text[300];
        g_counter = 1;
        PresenceEndpoints e(threads);
        Rig& rig = *e.rig;
        const world::ActorRegistry& reg = e.fx.world->registry();
        Clock::time_point now = t0;
        std::vector<PresenceClient*> all;
        PresenceClient a(50510, "cyb"), b(50511, "ayla"), c(50512, "mira");
        presence_join(rig, all, a, cyb_record(), now);
        presence_join(rig, all, b, presence_record(presence_id(2), "ayla", 4), now);
        presence_join(rig, all, c, presence_record(presence_id(3), "mira", 5), now);
        for (PresenceClient* p : all) presence_phase_in(rig, all, *p, now);
        presence_idle(rig, all, now, 12);
        std::snprintf(text, sizeof(text), "%u view thread(s): three players, three Player actors", threads);
        CHECK(rig.ep.session_count() == 3 && presence_count(reg) == 3, text);
        // c quits
        const uint64_t id_c = presence_player_id(rig, c.addr);
        c.out.push_back(c.j.client->close_channel());
        presence_step(rig, all, now += milliseconds(40));
        all.erase(std::remove(all.begin(), all.end(), &c), all.end());
        presence_idle(rig, all, now, 12);
        std::snprintf(text, sizeof(text), "%u view thread(s): a player who quits is out of the registry, the other two stay, and the others' views have closed it", threads);
        const PlayerSession* sa = rig.ep.player_at(a.addr);
        CHECK(rig.ep.session_count() == 2 && presence_count(reg) == 2 && !presence_pawn(reg, id_c) && sa && sa->actor_view() && sa->actor_view()->tracked() == 1, text);
        // a second login of the character b plays: b's first session is finished at once, the new one has a pawn of its own
        const uint64_t id_b = presence_player_id(rig, b.addr);
        PresenceClient b2(50513, "ayla");
        presence_join(rig, all, b2, presence_record(presence_id(2), "ayla", 4), now);
        presence_phase_in(rig, all, b2, now);
        presence_idle(rig, all, now, 10);
        const uint64_t id_b2 = presence_player_id(rig, b2.addr);
        std::snprintf(text, sizeof(text), "%u view thread(s): kicked for a login elsewhere, the first session's pawn is gone and the new session has its own (%zu Player actors)", threads, presence_count(reg));
        CHECK(id_b2 != 0 && id_b2 != id_b && !presence_pawn(reg, id_b) && presence_pawn(reg, id_b2) && presence_count(reg) == 2, text);
        // the kicked session lingers for 2 s, then closes: its client sends nothing more
        all.erase(std::remove(all.begin(), all.end(), &b), all.end());
        presence_idle(rig, all, now, 80);
        std::snprintf(text, sizeof(text), "%u view thread(s): after the linger the kicked session is gone and the registry still holds the two players", threads);
        CHECK(rig.ep.session_count() == 2 && presence_count(reg) == 2, text);
        // a (cyb) stops answering: the session times out
        const uint64_t id_a = presence_player_id(rig, a.addr);
        all.erase(std::remove(all.begin(), all.end(), &a), all.end());
        now += seconds(70);
        presence_step(rig, all, now);
        presence_idle(rig, all, now, 4);
        std::snprintf(text, sizeof(text), "%u view thread(s): a player whose connection times out is out of the registry, the last player's pawn stays", threads);
        CHECK(rig.ep.session_count() == 1 && presence_count(reg) == 1 && !presence_pawn(reg, id_a) && presence_pawn(reg, id_b2), text);
        CHECK(!reg.frozen(), "(the registry is thawed after the ticks)");
    }
}
AOC_REGISTER_TEST(test_presence_leaves_with_the_player);
