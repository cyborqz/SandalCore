// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/gather_session_pile_tests.inc: the pile of a full bag when the session ends
//  (registered with AOC_REGISTER_TEST: test_worldd_gather_session.cpp runs it through aoc_tests::run; it is included from gather_session_registered.inc)
// ============================================================================

// The stacks a full bag could not take wait 100 ms after the harvest's result and then become a pile where the player stood. A session that ends in that time makes the pile at once:
// the stacks are not lost with it.
static void test_pile_is_made_when_the_session_ends() {
    std::printf("a full bag's pile that still waits is made when the session ends\n");
    Rig rig;
    if (!rig.open(g_path)) return;
    struct Pile {
        std::vector<combat::LootStack> stacks;
    };
    std::vector<Pile> piles;
    rig.world->set_pile_hook([&](double, double, double, const repl::GuidValue&, const std::string&, const std::vector<combat::LootStack>& stacks) -> uint64_t {
        piles.push_back({stacks});
        return 9100 + piles.size();
    });
    const uint64_t node = rig.add_node(id::kDaffodil, 200, 0, 46);
    svc::Character full = record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb");
    for (int slot = 0; slot < 24; ++slot) full.items.push_back(make_item(R0, slot, id3::kCoal, 300 + static_cast<uint32_t>(slot), 20));
    Player& p = rig.add_player(std::move(full));
    play(p);
    p.send_now(interact(rig.component_of(node)), p.pawn_ch);
    run_until(rig, [&] { return p.s->harvests_completed() == 1; }, seconds(20));
    CHECK(p.s->harvests_completed() == 1 && piles.empty() && p.s->harvest_piles() == 0, "the harvest is done and its pile still waits for its time");
    p.s->finish("the test ends the session");
    CHECK(piles.size() == 1 && p.s->harvest_piles() == 1, "the end of the session made the pile");
    if (piles.size() == 1) {
        CHECK(piles[0].stacks.size() == 1 && piles[0].stacks[0].item == id::kDaffodilItem && piles[0].stacks[0].quantity == 3, "it holds the harvest: Daffodil x3");
    }
    rig.run(milliseconds(300));
    CHECK(piles.size() == 1 && p.s->harvest_piles() == 1, "and the pile is not made a second time");
}
AOC_REGISTER_TEST(test_pile_is_made_when_the_session_ends);
