// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_dice_tests.inc: the number a session's dice start from
//  (registered with AOC_REGISTER_TEST: test_worldd_combat.cpp runs it through aoc_tests::run; it is included from combat_registered.inc)
// ============================================================================

void test_session_dice_seed(Fixture&) {
    std::printf("the dice of a session\n");
    using combat::session_dice_seed;
    CHECK(session_dice_seed(0, 3) == 3 * 7919u + 1u, "a test's world (seed 0) keeps the plain function of the player id");
    CHECK(session_dice_seed(11, 3) == session_dice_seed(11, 3), "the same run seed and player repeat their rolls (--combat-seed)");
    CHECK(session_dice_seed(11, 3) != session_dice_seed(12, 3), "another start rolls another sequence for the same player");
    CHECK(session_dice_seed(11, 3) != session_dice_seed(11, 4), "two players of one run do not share a sequence");
    bool none_zero = true;
    for (uint64_t run = 1; run < 2000; ++run) none_zero = none_zero && session_dice_seed(run, run * 3) != 0;
    CHECK(none_zero, "a seed is never 0");
}
AOC_REGISTER_TEST(test_session_dice_seed);
