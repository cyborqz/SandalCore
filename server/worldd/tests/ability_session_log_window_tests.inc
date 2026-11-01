// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/ability_session_log_window_tests.inc: the ability log does not go blind
//  (registered with AOC_REGISTER_TEST: test_worldd_ability_session.cpp runs it through aoc_tests::run; it is included from ability_session_registered.inc)
// ============================================================================

// The first 100 transitions of the ability log are at info and the rest at debug (a client can cause them as often as its budget allows). That cap used to be for the whole session, so a long
// session was blind after its first 100 lines; it is for each minute now.
static void test_ability_log_window() {
    std::printf("the ability log: the first 100 transitions of each minute are at info, so a long session is not blind after its first 100 lines\n");
    Run r;
    r.play(0.3, true);
    g_log.str("");
    const auto sprints = [&](int rounds) {
        for (int i = 0; i < rounds; ++i) {   // (a Sprint that starts, a release: two transitions)
            const int32_t id = r.trigger(kSprint);
            r.play(0.1, true);
            r.cancel(id);
            r.play(0.1, true);
        }
    };
    sprints(60);   // 120 transitions in 12 s
    spdlog::default_logger()->flush();
    const size_t first = count_lines(g_log.str(), "[info]", "(Stamina ");
    r.play(61.0, true);   // the minute is over
    g_log.str("");
    sprints(10);   // 20 more
    spdlog::default_logger()->flush();
    const size_t later = count_lines(g_log.str(), "[info]", "(Stamina ");
    CHECKF(first == PlayerSession::kLoggedAbilityTransitions, "120 transitions in the first minute: the first 100 are at info (%zu)", first);
    CHECKF(later >= 20, "a minute later 20 more transitions are at info again, not at debug (%zu)", later);
    CHECK(r.h.s->ability_triggers() == 70 && r.h.s->ability_triggers_dropped() == 0 && !r.h.s->abilities_failed(), "(all 70 triggers were handled)");
}
AOC_REGISTER_TEST(test_ability_log_window);
