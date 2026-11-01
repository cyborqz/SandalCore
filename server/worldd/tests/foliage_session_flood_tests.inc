// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/foliage_session_flood_tests.inc: the limit on a client's tree-cutting starts
//  (registered with AOC_REGISTER_TEST: test_worldd_foliage_session.cpp runs it through aoc_tests::run; it is included from foliage_session_registered.inc)
// ============================================================================

// A client that starts the cutting again and again is not worked on again and again: the starts over the request budget (8 at once, then one every 100 ms) are dropped, and counted as refusals.
static void test_foliage_start_budget() {
    std::printf("a flood of tree starts: the budget drops the surplus\n");
    FRig fr;
    if (!fr.open()) return;
    Player& p = join_at_the_oak(fr, "bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb");
    for (int i = 0; i < 400; ++i) {   // (the reliable window of a connection is 256 bunches: the world runs a little every 50 starts, as a real client's acks would)
        p.client_sends({p.client.actor(p.pc_ch, foliage_start(fid::kOwner, fid::kFismc0, static_cast<uint32_t>(i % 5), 2), true)});
        if (i % 50 == 49) fr.rig.run(milliseconds(40));
    }
    fr.rig.run(milliseconds(200));
    CHECK(p.s->foliage_interacts() == 400, "all 400 starts arrived");
    char msg[160];
    std::snprintf(msg, sizeof msg, "...the budget dropped all but the first few of them (%zu dropped)", p.s->requests_dropped());
    CHECK(p.s->requests_dropped() >= 350, msg);
    CHECK(p.s->foliage_started() == 1 && p.s->foliage_refused() == 399, "...one cutting, and every other start counted as a refusal");
    const auto over = p.s->foliage_refusals().find("over the request budget");
    CHECK(over != p.s->foliage_refusals().end() && over->second >= 350, "...the refusals the budget made are listed under their own name");
}
AOC_REGISTER_TEST(test_foliage_start_budget);
