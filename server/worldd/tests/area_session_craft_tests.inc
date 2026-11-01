// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/area_session_craft_tests.inc: the client's craft starts that are refused before the session knows the station, and the request budget of the crafts
//  (registered with AOC_REGISTER_TEST: test_worldd_area_session.cpp runs it through aoc_tests::run; it is included from area_session_registered.inc)
// ============================================================================

/// ServerStartCraft naming no object (the zero guid): a start that reads, for a station that is not in the world.
static BunchWriter start_craft_for_no_station() {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerStartCraft");
    repl::StartCraft start;
    start.quantity = 1;
    const auto params = rpc ? repl::start_craft_params(*rpc, start) : std::nullopt;
    return params ? actor_rpc(pc, "ServerStartCraft", *params) : BunchWriter();
}

// A craft start that names an object that is no station is refused, and the client, which marked its own craft as running when it sent the start, is told to stop it.
static void test_craft_start_for_no_station_stops_the_client() {
    std::printf("a craft start that names no station: the client's craft is stopped\n");
    VendorHarness h;
    h.play();
    h.run(milliseconds(1500));
    const size_t from = h.got.size();
    h.client_request(start_craft_for_no_station(), h.pc_ch);
    CHECK(h.s->quick_refused() + h.s->crafts_refused() == 1, "the start is refused and counted");
    CHECK(count_calls(pc_calls_since(h, from), "ClientInterruptCraft") == 1, "the client is sent ClientInterruptCraft");
}
AOC_REGISTER_TEST(test_craft_start_for_no_station_stops_the_client);

// The shops' budget (eight at once, then one every 100 ms) is not the crafts': a burst of purchases does not make a craft start wait.
static void test_craft_start_is_not_slowed_by_the_shops() {
    std::printf("a craft start has a request budget of its own: a burst of purchases does not use it\n");
    VendorHarness h;
    h.play();
    h.run(milliseconds(1500));
    std::vector<fake::Bunch> burst;
    for (int i = 0; i < 12; ++i) burst.push_back(h.client.actor(h.pc_ch, purchase(cart_of(h.gear_def, {{fi::kPotion, 1}})), true));   // (no window is open: each is refused, and each takes a token of the shops' budget)
    burst.push_back(h.client.actor(h.pc_ch, start_craft_for_no_station(), true));
    h.client_sends(std::move(burst));
    h.settle();
    CHECK(h.s->requests_dropped() >= 4, "the shops' budget cut the purchases of the burst");
    CHECK(h.s->quick_refused() + h.s->crafts_refused() == 1, "the craft start of the same burst was not cut: it was answered");
}
AOC_REGISTER_TEST(test_craft_start_is_not_slowed_by_the_shops);
