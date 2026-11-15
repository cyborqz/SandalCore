// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/ability_session_interrupt_budget_tests.inc: the client's channel interrupts have a request budget of their own
//  (registered with AOC_REGISTER_TEST: test_worldd_ability_session.cpp runs it through aoc_tests::run; it is included from ability_session_registered.inc)
// ============================================================================

// A client may send ServerInterruptChannel every frame while its player moves.  A burst of them is cut by a budget of its own, so it does not use up the budget of the abilities' own
// cancels and ends: a held ability's cancel that comes right after the burst is still taken.
static void test_channel_interrupts_leave_the_cancel_budget() {
    std::printf("a burst of channel interrupts leaves the ability cancels their budget\n");
    const svc::Character rec = bard25();
    Run c(0, true, &rec);
    c.play(0.5, true);
    const size_t dropped = c.h.s->requests_dropped();
    for (int i = 0; i < 40; ++i) c.send_reliable(actor_bunch(class_named("PlayerPawn_C"), "ServerInterruptChannel", {}));
    char msg[160];
    std::snprintf(msg, sizeof msg, "40 interrupts in one moment: the budget drops the surplus (%zu dropped)", c.h.s->requests_dropped() - dropped);
    CHECK(c.h.s->requests_dropped() >= dropped + 20, msg);
    const size_t taken = c.h.s->held_cancels(), cut = c.h.s->held_cancels_dropped();
    c.cancel(1);
    CHECK(c.h.s->held_cancels() == taken + 1, "a held ability's cancel right after the burst is taken");
    CHECK(c.h.s->held_cancels_dropped() == cut, "and not dropped for the interrupts before it");
    CHECK(!c.h.s->abilities_failed(), "(the session is fine)");
}
AOC_REGISTER_TEST(test_channel_interrupts_leave_the_cancel_budget);
