// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/movement_presence_mode_tests.inc: the movement mode other players are shown (players seeing each other)
//
//  The movement mode of a move is the client's own byte, and the move checker keeps it as sent.  The pawn other players are shown carries only a mode a character moves in:
//  walking (1), falling (3), swimming (4), flying (5) and the custom modes 16, 17 and 18; the client's 0 is shown as walking, and so is any other number a client may send.
// ============================================================================
#include "movement_presence_rig.h"

static void test_presence_the_movement_mode_other_clients_see() {
    std::printf("players seeing each other: the pawn is shown in walking, falling, swimming, flying or a custom mode 16 to 18, and in walking for any other number\n");
    Harness h;
    {
        BunchWriter ready;
        const auto bytes = capture_blocks::bytes_of(capture_blocks::kClientReadyToPhaseIn.hex);
        ready.write_bit_range(bytes.data(), 0, capture_blocks::kClientReadyToPhaseIn.bits);
        h.client_sends({h.client.actor(h.pawn_ch, ready, true)});
    }
    h.settle();
    const uint64_t id = static_cast<uint64_t>(h.s->bootstrap().setup().player_id);
    float ts = 1.0f;
    double x = kX;
    // the mode the registry's pawn shows after the client moved 10 cm in `mode`
    const auto shown = [&](uint8_t mode) -> uint64_t {
        ts += 0.05f;
        x += 10.0;
        h.send_move(move_at(ts, x, kY, kZ, kSpawnYaw, mode));
        h.step(milliseconds(50));
        const world::WorldActor* a = presence_pawn(h.fx.world->registry(), id);
        return a ? presence_motion(*a).mode : 9999;
    };
    struct Case {
        uint8_t sent;
        uint64_t expected;
    };
    // pass cases: the modes a character moves in come through as they are
    for (const Case c : {Case{1, 1}, Case{3, 3}, Case{4, 4}, Case{5, 5}, Case{16, 16}, Case{17, 17}, Case{18, 18}, Case{1, 1}}) {
        char text[200];
        std::snprintf(text, sizeof(text), "a move in mode %u: the other players' copy of the pawn is in mode %llu", static_cast<unsigned>(c.sent), static_cast<unsigned long long>(c.expected));
        const uint64_t got = shown(c.sent);
        CHECK(got == c.expected, text);
    }
    // fail cases: none (0), and numbers a modified client sends, are shown as walking
    for (const Case c : {Case{0, 1}, Case{2, 1}, Case{6, 1}, Case{10, 1}, Case{15, 1}, Case{19, 1}, Case{200, 1}, Case{255, 1}}) {
        char text[200];
        std::snprintf(text, sizeof(text), "a move in mode %u: the other players' copy of the pawn is in mode %llu (walking)", static_cast<unsigned>(c.sent), static_cast<unsigned long long>(c.expected));
        const uint64_t got = shown(c.sent);
        CHECK(got == c.expected, text);
    }
    CHECK(h.s->moves_refused() == 0, "(none of the moves was refused: every mode above reached the registry through the move checker)");
}
AOC_REGISTER_TEST(test_presence_the_movement_mode_other_clients_see);
