// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/elections_node_stores_tests.inc: the node's stores (world/elections.h NodeElection::stored)
//  (included by elections_registered.inc)
// ============================================================================

/// The node's stores (world/elections.h NodeElection::stored, session/player_node_inventory.cpp): the start from the data once, the file.
void test_node_stores() {
    std::printf("the node's stores\n");
    Elections el{ElectionRules{}};
    const int64_t node = 4242;
    CHECK(el.stock(node, 1525050, {{11, 225}, {12, 225}}) && el.treasury(node) == 1525050 && el.stored(node) && el.stored(node)->at(11) == 225 && el.stored(node)->at(12) == 225,
          "a node is stocked: the money to its treasury, the goods to its stores");
    CHECK(!el.stock(node, 1525050, {{11, 225}}) && el.treasury(node) == 1525050 && el.stored(node)->at(11) == 225, "... once");
    el.change_node(node, [](NodeElection& e) {
        e.stored[11] -= 225;
        e.treasury -= 99999999;
        return true;
    });
    CHECK(el.stored(node)->count(11) == 0 && el.stored(node)->at(12) == 225 && el.treasury(node) == 0, "a kind used up goes, and the treasury goes no lower than nothing");
    Elections back{ElectionRules{}};
    std::string error;
    CHECK(back.from_json(el.to_json(), error) && back.stored(node) && *back.stored(node) == *el.stored(node) && back.node(node) && back.node(node)->stocked,
          "the file keeps the stores and that the node was stocked");
    // the data: where nodes start
    namespace ni = aoc::worldd::world::node_inventory;
    const ni::NodeStart* halcyon = nullptr;
    for (const ni::NodeStart& n : ni::kNodeStarts) {
        if (std::string(n.name) == "Verra_RVR_Halcyon") halcyon = &n;
    }
    CHECK(halcyon && std::string(ni::kContents[halcyon->contents].name) == "Default_InitialNodeResources" && ni::kContents[halcyon->contents].money == 1525050 &&
              ni::kContents[halcyon->contents].count == 3,
          "Halcyon starts with Default_InitialNodeResources: 1,525,050 copper and three kinds of goods");
    bool spirits = false;
    for (const ni::Commodity& c : ni::kCommodities) spirits = spirits || (std::string(c.name) == "Commodity_Spirits_T1" && c.item != 0);
    CHECK(spirits, "the commodities name the item each makes (Commodity_Spirits_T1)");
}
AOC_REGISTER_TEST(test_node_stores);
