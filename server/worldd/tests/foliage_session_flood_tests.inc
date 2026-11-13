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

// A client that sends garbage without end does not grow the log without end: after the first three unreadable bunches of a session (said whole), a line is written at the 10th, the
// 100th, the 1,000th and so on, not at every tenth.
static void test_unreadable_bunches_do_not_flood_the_log() {
    std::printf("a flood of bunches that do not read: the log gains a handful of lines, not one for every ten\n");
    FRig fr;
    if (!fr.open()) return;
    Player& p = join_at_the_oak(fr, "bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb");
    BunchWriter garbage;
    for (int i = 0; i < 12; ++i) garbage.write_bits(0xFFFFFFFFu, 32);
    const size_t before = count_in_log("does not read: ignored");
    for (int i = 0; i < 2000; ++i) {   // (the reliable window of a connection is 256 bunches: the world runs a little every 50 of them, as a real client's acks would)
        p.client_sends({p.client.actor(p.pc_ch, garbage, true)});
        if (i % 50 == 49) fr.rig.run(milliseconds(40));
    }
    fr.rig.run(milliseconds(200));
    const size_t lines = count_in_log("does not read: ignored") - before;
    char msg[200];
    std::snprintf(msg, sizeof msg, "2,000 unreadable bunches of one kind: 6 lines (the first three, then the 10th, the 100th and the 1,000th), not %zu", lines);
    CHECK(lines == 6, msg);
}
AOC_REGISTER_TEST(test_unreadable_bunches_do_not_flood_the_log);

// The names a client sends with a tree start go into the log cut short and with no control character in them: a long name cannot grow the log, and a newline in a name cannot start
// a log line of the player's making.
static void test_foliage_names_are_logged_short_and_on_one_line() {
    std::printf("a tree start whose names are long and hold a newline: one short log line, no line of the client's making\n");
    FRig fr;
    if (!fr.open()) return;
    Player& p = join_at_the_oak(fr, "bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb");
    const std::string owner = std::string(300, 'A') + "\nFORGED the server says the client's words";
    const std::string component = "Comp\nFORGED again" + std::string(300, 'B');
    const size_t before = g_log.str().size();
    const size_t refused = p.s->foliage_refused();
    p.send_now(foliage_start(owner.c_str(), component.c_str(), 7, 2), p.pc_ch);
    fr.rig.run(milliseconds(200));
    const std::string added = g_log.str().substr(before);
    CHECK(p.s->foliage_refused() == refused + 1 && added.find("is refused") != std::string::npos, "(the start is refused, and the log says so)");
    char msg[160];
    std::snprintf(msg, sizeof msg, "the log grew by %zu characters for it; must be under 700", added.size());
    CHECK(added.size() < 700, msg);
    CHECK(added.find("\nFORGED") == std::string::npos, "no line of the log starts with the client's own text");
}
AOC_REGISTER_TEST(test_foliage_names_are_logged_short_and_on_one_line);
