// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/elections_citizen_shops_tests.inc: the shops' citizenship predicates (vendor/town_shops.h)
//  (included by elections_registered.inc)
// ============================================================================

/// The shops' citizenship predicates (vendor/town_shops.h ShopFacts): Node_GetActorCityNode of the player is the node the player is a citizen of, of the shopkeeper the
/// settlement it stands in.  The two shapes the data's 210 citizen listings take, on two real nodes (content.db; skipped without it).
void test_citizen_shops() {
    std::printf("the shops' citizen-only listings\n");
    const char* env = std::getenv("AOC_CONTENT_DB");
    const std::string path = env && *env ? env : "dist/content/content.db";
    if (!std::filesystem::exists(path)) {
        std::printf("  [skip] SKIP: no %s\n", path.c_str());
        return;
    }
    std::string error;
    const std::unique_ptr<CityNodes> nodes = CityNodes::load(path, error);
    CHECK(nodes != nullptr, "(the real city nodes load)");
    if (!nodes) return;
    // two nodes that stand inside their own zones: a shopkeeper at one's location is in it
    const CityNodeInfo* here = nullptr;
    const CityNodeInfo* other = nullptr;
    for (const CityNodeInfo& n : nodes->nodes()) {
        const CityNodeInfo* at = nodes->by_rank(nodes->node_at(n.x, n.y).node_rank);
        if (!at || at->guid != n.guid) continue;
        if (!here) here = &n;
        else if (!other) other = &n;
    }
    CHECK(here && other, "(two nodes that stand in their own zones)");
    if (!here || !other) return;
    const auto holds = [&](const std::string& expression, int64_t citizen_of) {
        aoc::worldd::vendor::ShopFacts f;
        f.nodes = nodes.get();
        f.player_x = here->x;   // the player stands at the shop, in `here`
        f.player_y = here->y;
        f.has_npc = true;
        f.npc_x = here->x;
        f.npc_y = here->y;
        f.citizen_node = citizen_of;
        const auto v = aoc::worldd::dialogue::evaluate_expression(expression, f);
        return v && v->i != 0;
    };
    // "Must be a citizen of this settlement"
    const std::string citizen_here = "Node_GetActorCityNode(GetInteractionTarget()) == Node_GetActorCityNode(GetPlayer())";
    CHECK(holds(citizen_here, here->guid), "a citizen of the shopkeeper's settlement may buy its citizens' listing");
    CHECK(!holds(citizen_here, 0), "a player who is no citizen may not, though standing in the settlement (before, anyone standing there could)");
    CHECK(!holds(citizen_here, other->guid), "nor may a citizen of another node");
    // "Must not be a citizen of <node>"
    const std::string not_citizen_of = "Node_GetActorCityNode(GetPlayer()) != $#8574414081702566619:" + std::to_string(here->guid) + "$";
    CHECK(holds(not_citizen_of, 0) && holds(not_citizen_of, other->guid), "a listing for those who are not its citizens holds for a non-citizen standing there, and another node's citizen");
    CHECK(!holds(not_citizen_of, here->guid), "... and not for its own citizen");
    CHECK(holds("Node_IsPlayerCitizenOfNode(GetPlayer(), $#8574414081702566619:" + std::to_string(here->guid) + "$)", here->guid) && !holds("Node_IsPlayerCitizen(GetPlayer())", 0),
          "Node_IsPlayerCitizenOfNode and Node_IsPlayerCitizen read the citizenship too");
}
AOC_REGISTER_TEST(test_citizen_shops);
