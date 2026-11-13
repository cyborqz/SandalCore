// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/elections_buy_orders_tests.inc: the node's buy orders (world/buy_orders.h)
//  (included by elections_registered.inc)
// ============================================================================

/// The node's buy orders (world/buy_orders.h): placing one against the data's recipe, giving to it, its limit per period, its end, and the file.
void test_buy_orders() {
    std::printf("the node's buy orders\n");
    namespace bo = aoc::worldd::world::buy_orders;
    const bo::Recipe* r = nullptr;
    for (const bo::Recipe& x : bo::kRecipes) {
        if (std::string(x.name) == "Commodity_RVR_Spirits_T1_1") r = &x;
    }
    CHECK(r && r->count == 2 && r->biomes == (1u << 14) && r->min_cost == 10000 && r->max_cost == 50000 && r->min_limit == 10 && r->max_limit == 100,
          "the data: Commodity_RVR_Spirits_T1_1, a Riverlands recipe of two requirements (NodeCost 10,000-50,000, ContributionLimit 10-100)");
    if (!r) return;
    const bo::Requirement& q0 = bo::kRequirements[r->first];
    const bo::Requirement& q1 = bo::kRequirements[r->first + 1];
    const int64_t m0 = bo::kOptions[q0.first], m1 = bo::kOptions[q1.first + q1.count - 1];
    const int64_t node = 4242, now = 1'000'000'000'000;
    bo::Request req;
    req.commodity = r->commodity;
    req.recipe = r->recipe;
    req.materials = {m1, m0};   // (in any order)
    req.contribution_limit = 20;
    req.requested = 60;
    req.reward_quantity = 2;
    req.seconds = 4 * 86400;
    req.node_cost = 12000;
    NodeBuyOrders orders;
    int64_t treasury = 20000;
    aoc::worldd::world::MayorGuid g;
    std::string clamped;
    CHECK(bo::add(orders, node, 14, req, 2, treasury, now, &g, &clamped) == bo::Add::Ok && orders.active.size() == 1 && treasury == 8000 && !g.zero() && clamped.empty(),
          "the mayor places an order in the Riverlands: NodeCost from the treasury, a guid");
    const NodeBuyOrder& o = orders.active.front();
    CHECK(o.materials.size() == 2 && o.materials[0] == std::make_pair(m0, q0.quantity) && o.materials[1] == std::make_pair(m1, q1.quantity) && o.requested == 60 &&
              o.contribution_limit == 20 && o.expires_ms == now + 4 * 86400000LL && o.reward == r->reward_table && o.commodity == r->commodity,
          "... each requirement gets the item named for it, at the requirement's quantity per unit; the numbers and the days are kept");
    // the client's own defaults may lie outside the data's ranges: brought in, not refused
    bo::Request wide = req;
    wide.contribution_limit = 300;
    wide.node_cost = 9000;
    treasury = 20000;
    CHECK(bo::add(orders, node, 14, wide, 2, treasury, now, nullptr, &clamped) == bo::Add::Ok && orders.active.back().contribution_limit == 100 &&
              orders.active.back().node_cost == 10000 && treasury == 10000 && clamped.find("ContributionLimit 300 -> 100") != std::string::npos,
          "a ContributionLimit of 300 and a NodeCost under the range are brought into the recipe's ranges (and said)");
    treasury = 1'000'000;
    CHECK(bo::add(orders, node, 14, req, 2, treasury, now) == bo::Add::NoSlot && orders.active.size() == 2 && treasury == 1'000'000, "no third order on two slots, nothing paid");
    CHECK(bo::add(orders, node, 3, req, 9, treasury, now) == bo::Add::WrongBiome && bo::add(orders, node, 0, req, 3, treasury, now) == bo::Add::Ok,
          "a Riverlands recipe is refused in another biome, and allowed in a node of no known biome");
    bo::Request bad = req;
    bad.materials = {m0, 12345};
    CHECK(bo::add(orders, node, 14, bad, 9, treasury, now) == bo::Add::BadMaterials, "an item no requirement takes is refused");
    bad = req;
    bad.recipe = 77;
    CHECK(bo::add(orders, node, 14, bad, 9, treasury, now) == bo::Add::NoRecipe, "a recipe that is not in the data is refused");
    bad = req;
    bad.commodity = 77;
    CHECK(bo::add(orders, node, 14, bad, 9, treasury, now) == bo::Add::WrongCommodity, "a recipe that is not the commodity's is refused");
    int64_t poor = 100;
    CHECK(bo::add(orders, node, 14, req, 9, poor, now) == bo::Add::NoMoney && poor == 100, "a treasury that cannot pay places nothing");

    // giving: whole units, within what the order needs and what the character's limit leaves this period
    CHECK(bo::left_for(*bo::find(orders, g), "a", now) == 20, "a character may give the order's limit");
    bool done = true;
    CHECK(bo::give(orders, g, "a", 15, now, &done) == bo::Give::Ok && !done && bo::find(orders, g)->fulfilled == 15 && bo::left_for(*bo::find(orders, g), "a", now) == 5,
          "15 units count; the character has 5 left this period");
    CHECK(bo::give(orders, g, "a", 6, now + 1000, &done) == bo::Give::OverLimit && bo::find(orders, g)->fulfilled == 15, "... and 6 more are refused");
    CHECK(bo::give(orders, g, "b", 20, now, &done) == bo::Give::Ok && bo::find(orders, g)->fulfilled == 35, "another character has its own limit");
    CHECK(bo::give(orders, g, "a", 20, now + bo::kContributionPeriodMs, &done) == bo::Give::Ok && bo::find(orders, g)->fulfilled == 55,
          "a day later the character's limit is whole again");
    CHECK(bo::give(orders, g, "c", 6, now, &done) == bo::Give::OverNeed && bo::give(orders, g, "c", 0, now, &done) == bo::Give::Nothing, "no more than the order needs, and no nothing");
    const NodeBuyOrders saved = orders;   // (three orders, the first with what "a" and "b" gave)
    CHECK(bo::give(orders, g, "c", 5, now, &done) == bo::Give::Ok && done && !bo::find(orders, g) && bo::give(orders, g, "c", 1, now, &done) == bo::Give::NoOrder,
          "the last units fill the order: it leaves the list");
    NodeBuyOrders late = saved;
    const size_t running = late.active.size();
    CHECK(bo::give(late, g, "c", 1, now + 9 * 86400000LL, &done) == bo::Give::Expired, "an order whose days ran out takes nothing");
    CHECK(running == 3 && bo::expire(late, now + 9 * 86400000LL) == running && late.active.empty(), "... and the orders whose days ran out leave the list");

    // the file
    Elections el{ElectionRules{}};
    el.change_node(node, [&](NodeElection& e) {
        e.buy_orders = saved;
        return true;
    });
    Elections back{ElectionRules{}};
    std::string error;
    CHECK(back.from_json(el.to_json(), error) && back.buy_orders(node) && *back.buy_orders(node) == saved && saved.active.front().given.size() == 2,
          "the file keeps the orders, their counts and what each character gave");
    CitizenshipRules rules;
    rules.buy_order_slots = {{3, 15}, {4, 20}};
    CHECK(rules.buy_order_slots_at(3) == 15 && rules.buy_order_slots_at(4) == 20 && rules.buy_order_slots_at(2) == 0, "a node's slots by its level (MinBuyOrderSlots)");
}
AOC_REGISTER_TEST(test_buy_orders);
