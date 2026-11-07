// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/movement_surface_tests.inc: the ground check of a walking move (session/move_checker.h: kBelowGroundCm)
//
//  A modified client sends a walking move 4 m under the ground (nav_fixture.h).  The real session judges it: the move is refused and
//  corrected to the last good position, as a too-fast move is.  On the unchanged checker it passes the speed, rise and fall rules
//  (400 cm in a second is far under the fall limit), so that check fails there.  A 1 m sink is acknowledged.
// ============================================================================

namespace surface_checks {

/// Teleports the player to (x, y, z) on the fixture's ground and lets the 2 s after a server-side teleport pass.
void place(Harness& h, double x, double y, double z) {
    const PlayerSession::GmMoveResult moved = h.s->gm_teleport(h.conn, x, y, z, h.t, "the test: on the ground");
    CHECK(moved.status == PlayerSession::GmMoveResult::Status::Moved, "(the player is put on the fixture's ground)");
    h.flush();
    h.step(milliseconds(3000));
}

}  // namespace surface_checks

void test_moves_under_the_ground() {
    using namespace surface_checks;
    std::printf("a walking move under the ground\n");
    Harness h({}, true, /*with_ground=*/true);
    float ts = 1.0f;

    place(h, 2000, 0, 1000);
    auto replies = h.send_move(move_at(ts, 2000, 0, 1000));
    CHECK(replies.size() == 1 && is_ack(*replies[0], ts), "(a walking move on the fixture's ground is acknowledged)");
    h.step(milliseconds(1000));
    const GoodPosition on_ground = h.s->movement().good();
    ts += 1.0f;
    replies = h.send_move(move_at(ts, 2100, 0, 600));
    CHECK(replies.size() == 1 && correction_is(*replies[0], ts, on_ground.x, on_ground.y, on_ground.z) && h.s->moves_refused() == 1,
          "a walking move 4 m under the ground is refused: one ClientAdjustPosition to the last good position");
    CHECK(h.s->movement().good() == on_ground, "(the last good position is the ground's: the refused move changed nothing)");

    h.step(milliseconds(1000));
    ts += 1.0f;
    replies = h.send_move(move_at(ts, 2200, 0, 900));
    CHECK(replies.size() == 1 && is_ack(*replies[0], ts) && h.s->moves_refused() == 1, "a walking move 1 m under the ground is acknowledged: the allowance is 300 cm");
}
AOC_REGISTER_TEST(test_moves_under_the_ground);
