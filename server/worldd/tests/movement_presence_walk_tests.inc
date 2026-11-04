// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/movement_presence_walk_tests.inc: players seeing each other, walking, stopping and being moved by the server (through the Endpoint, with 0, 1 and 3 view threads)
//
//  The pawn the other players are shown follows the player's last good position, written once a tick from the first part of the session's tick (never from a move, never from a view
//  pass): a walk shows a velocity, the stop of a client that sends no more moves shows none within 300 ms, a server move (a game master's teleport) is shown with no client move to carry
//  it, and players who join and leave all the time never write the registry while the views read it.
// ============================================================================
#include "movement_presence_rig.h"

/// Two players in the world and phased in, standing at the join spot; `a` is cyb, `b` is ayla.
struct PresencePair {
    PresenceEndpoints        e;
    std::vector<PresenceClient*> all;
    PresenceClient           a, b;
    Clock::time_point        now = t0;
    explicit PresencePair(unsigned threads) : e(threads), a(50510, "cyb"), b(50511, "ayla") {
        g_counter = 1;
        Rig& rig = *e.rig;
        presence_join(rig, all, a, cyb_record(), now);
        presence_join(rig, all, b, presence_record(presence_id(2), "ayla", 4), now);
        presence_phase_in(rig, all, a, now);
        presence_phase_in(rig, all, b, now);
        presence_idle(rig, all, now, 12);
    }
    Rig&                       rig() { return *e.rig; }
    const world::ActorRegistry& reg() const { return e.fx.world->registry(); }
};

static void test_presence_walk_then_stop_with_no_more_moves() {
    std::printf("players seeing each other: a walk shows a velocity, and a stop of a client that sends nothing more shows none within 300 ms, with 0, 1 and 3 view threads\n");
    char text[420];
    for (const unsigned threads : {0u, 1u, 3u}) {
        PresencePair p(threads);
        const uint64_t ida = presence_player_id(p.rig(), p.a.addr);
        // ten moves 100 ms apart on the client's clock and the server's, 40 cm each: 400 cm/s
        p.a.ts = 1.0f;
        presence_move(p.a, 0.0f, 40.0);
        p.now += milliseconds(100);
        presence_step(p.rig(), p.all, p.now);
        double fastest = 0;
        for (int i = 0; i < 10; ++i) {
            presence_move(p.a, 0.1f, 40.0);
            p.now += milliseconds(100);
            presence_step(p.rig(), p.all, p.now);
            if (const world::WorldActor* pawn = presence_pawn(p.reg(), ida)) {
                const PresenceMotion m = presence_motion(*pawn);
                fastest = std::max(fastest, std::hypot(m.vx, m.vy));
            }
        }
        const world::WorldActor* pawn = presence_pawn(p.reg(), ida);
        PresenceMotion walking = pawn ? presence_motion(*pawn) : PresenceMotion{};
        std::snprintf(text, sizeof(text), "%u view thread(s): while it walks the pawn shows 400 cm/s and the walking input, and stands where the last move put it (x %.0f, %.0f from the first; the fastest was %.0f)", threads,
                      walking.x, walking.x - kX, fastest);
        CHECK(pawn && walking.ok && walking.moving && std::fabs(std::hypot(walking.vx, walking.vy) - 400.0) < 25.0 && near(pawn->x, p.a.x) && near(walking.x, p.a.x), text);
        // no more moves: the client says nothing but its acknowledgements
        const Clock::time_point last_move = p.now;
        double velocity_at_300 = -1;
        for (int ms = 50; ms <= 300; ms += 50) {
            presence_step(p.rig(), p.all, last_move + milliseconds(ms));
            p.now = last_move + milliseconds(ms);
        }
        if (const world::WorldActor* stopped = presence_pawn(p.reg(), ida)) {
            const PresenceMotion m = presence_motion(*stopped);
            velocity_at_300 = std::hypot(m.vx, m.vy);
            std::snprintf(text, sizeof(text), "%u view thread(s): 300 ms after the last move the pawn shows no velocity and no walking input (%.0f cm/s), still at x %.0f", threads, velocity_at_300, m.x);
            CHECK(m.ok && velocity_at_300 == 0.0 && !m.moving && near(m.x, p.a.x), text);
        } else {
            CHECK(false, "the pawn is in the registry");
        }
        // the other player's client was sent the stop: its view holds the pawn and got updates on its channel after the last move
        const PlayerSession* sb = p.rig().ep.player_at(p.b.addr);
        const uint32_t channel = (sb && sb->actor_view() && pawn) ? sb->actor_view()->channel_of(pawn->id) : 0;
        size_t updates = 0;
        for (const fake::Delivered& d : p.b.j.got) {
            if (d.channel == channel && !d.open && !d.close) ++updates;
        }
        std::snprintf(text, sizeof(text), "%u view thread(s): the other player's client got the pawn's updates on its channel (%zu so far)", threads, updates);
        CHECK(channel != 0 && updates >= 3, text);
    }
}
AOC_REGISTER_TEST(test_presence_walk_then_stop_with_no_more_moves);

static void test_presence_a_server_move_is_followed_with_no_client_move() {
    std::printf("players seeing each other: a teleport is in the registry on the next tick with no move from the client, with 0, 1 and 3 view threads\n");
    char text[420];
    for (const unsigned threads : {0u, 1u, 3u}) {
        PresencePair p(threads);
        const uint64_t ida = presence_player_id(p.rig(), p.a.addr);
        const world::WorldActor* pawn = presence_pawn(p.reg(), ida);
        const uint32_t channel = [&] {
            const PlayerSession* sb = p.rig().ep.player_at(p.b.addr);
            return (sb && sb->actor_view() && pawn) ? sb->actor_view()->channel_of(pawn->id) : 0u;
        }();
        size_t updates_before = 0;
        for (const fake::Delivered& d : p.b.j.got) {
            if (d.channel == channel && !d.open && !d.close) ++updates_before;
        }
        // a game master moves cyb 30 m east and 5 m north
        const double to_x = kX + 3000.0, to_y = kY + 500.0;
        PlayerSession::GmMoveResult moved;
        for (PlayerHandle& h : p.rig().ep.players()) {
            if (h.player->name() == "cyb") moved = h.player->gm_teleport(*h.connection, to_x, to_y, kZ, p.now, "a test");
        }
        CHECK(moved.status == PlayerSession::GmMoveResult::Status::Moved, "(the game master's teleport is taken)");
        presence_idle(p.rig(), p.all, p.now, 8);   // (cyb's client sends no move: only its acknowledgements)
        pawn = presence_pawn(p.reg(), ida);
        const PresenceMotion m = pawn ? presence_motion(*pawn) : PresenceMotion{};
        std::snprintf(text, sizeof(text), "%u view thread(s): the pawn is where the teleport put the player (x %+.0f, y %+.0f from the join spot) and stands still", threads, pawn ? pawn->x - kX : 0.0,
                      pawn ? pawn->y - kY : 0.0);
        CHECK(pawn && near(pawn->x, to_x) && near(pawn->y, to_y) && m.ok && near(m.x, to_x) && near(m.y, to_y) && !m.moving, text);
        size_t updates_after = 0;
        for (const fake::Delivered& d : p.b.j.got) {
            if (d.channel == channel && !d.open && !d.close) ++updates_after;
        }
        std::snprintf(text, sizeof(text), "%u view thread(s): the other player's client was sent the new place (%zu updates before, %zu after)", threads, updates_before, updates_after);
        CHECK(channel != 0 && updates_after > updates_before, text);
    }
}
AOC_REGISTER_TEST(test_presence_a_server_move_is_followed_with_no_client_move);

static void test_presence_churn_never_writes_the_registry_in_a_view_pass() {
    std::printf("players seeing each other: 200 ticks of players joining, walking and leaving next to one who stays, with 0, 1 and 3 view threads\n");
    char text[420];
    for (const unsigned threads : {0u, 1u, 3u}) {
        g_counter = 1;
        PresenceEndpoints e(threads);
        Rig& rig = *e.rig;
        const world::ActorRegistry& reg = e.fx.world->registry();
        Clock::time_point now = t0;
        std::vector<PresenceClient*> all;
        PresenceClient stays(50510, "cyb");
        presence_join(rig, all, stays, cyb_record(), now);
        presence_phase_in(rig, all, stays, now);
        stays.ts = 1.0f;
        const size_t log_from = g_log.str().size();
        int ticks = 0, joined = 0;
        for (int round = 0; round < 6; ++round) {
            PresenceClient visitor(static_cast<uint16_t>(50600 + round), "mx" + std::to_string(round));
            presence_join(rig, all, visitor, presence_record(presence_id(16 + round), "mx" + std::to_string(round), 10 + round), now);
            ++joined;
            presence_phase_in(rig, all, visitor, now);
            visitor.ts = 1.0f;
            for (int k = 0; k < 6; ++k) {
                presence_move(visitor, k == 0 ? 0.0f : 0.1f, 25.0);
                presence_move(stays, k == 0 ? 0.0f : 0.1f, -25.0);
                now += milliseconds(100);
                presence_step(rig, all, now);
                ++ticks;
            }
            visitor.out.push_back(visitor.j.client->close_channel());
            now += milliseconds(40);
            presence_step(rig, all, now);
            all.erase(std::remove(all.begin(), all.end(), &visitor), all.end());
            presence_idle(rig, all, now, 3);
            ticks += 4;
        }
        presence_idle(rig, all, now, 20);
        ticks += 20;
        const std::string log = g_log.str().substr(log_from);
        const bool threw = log.find("the tick threw") != std::string::npos || log.find("was changed (") != std::string::npos;
        std::snprintf(text, sizeof(text), "%u view thread(s): %d joins and leaves in %d+ ticks: the player who stays is still there, alone in the registry, the registry is thawed and no tick threw", threads, joined, ticks);
        CHECK(rig.ep.session_count() == 1 && presence_count(reg) == 1 && !reg.frozen() && !threw, text);
    }
}
AOC_REGISTER_TEST(test_presence_churn_never_writes_the_registry_in_a_view_pass);
