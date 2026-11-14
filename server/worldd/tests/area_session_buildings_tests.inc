// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/area_session_buildings_tests.inc: the node's service buildings through a session (session/player_node_buildings.cpp)
//  (included by area_session_registered.inc, after the citizenship tests: CitizenHarness)
// ============================================================================

namespace buildings_test {

namespace nb = world::node_buildings;

const nb::Building* building_named(const char* name) {
    for (size_t i = 0; i < nb::kBuildings_count; ++i) {
        if (std::string(nb::kBuildings[i].name) == name) return &nb::kBuildings[i];
    }
    return nullptr;
}

const nb::TreeNode* tree_node_named(const char* name) {
    for (size_t i = 0; i < nb::kTreeNodes_count; ++i) {
        if (std::string(nb::kTreeNodes[i].name) == name) return &nb::kTreeNodes[i];
    }
    return nullptr;
}

int64_t wall_now() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

}  // namespace buildings_test

void test_node_buildings_session() {
    std::printf("the node's service buildings through a session\n");
    namespace nb = world::node_buildings;
    using namespace buildings_test;
    CitizenHarness h;
    h.play();
    h.run(milliseconds(1500));
    spdlog::set_level(std::getenv("AOC_TEST_CITIZEN_LOG") ? spdlog::level::info : spdlog::level::off);
    h.fx.world->set_elections(world::ElectionRules{}, "");
    world::Elections* el = h.fx.world->elections();
    const world::ElectionPerson me{h.record.account_id, kCyb, "cyb", 1};
    el->set_citizen(kTestCitizenNode, me);
    const nb::Building* factory = building_named("Construction_Support_Factory_Building");
    const nb::Building* empty = building_named("Construction_EmptyPlot_Building");
    const nb::TreeNode* converters = tree_node_named("SupportBuilding_Factory_EXP_0A_CommodityConverters");
    CHECK(factory && empty && converters, "(the data's Factory, empty plot and the Factory's Commodity Converters expansion)");
    if (!factory || !empty || !converters) return;

    // the node's plots: an empty service plot and the seed's Factory (this harness has no node data)
    constexpr int64_t kPlotA = 9101, kPlotB = 9102;
    world::PlotInfo a;
    a.plot = kPlotA;
    a.building = empty->guid;
    a.empty_building = empty->guid;
    a.candidates = {factory->guid};
    world::PlotInfo b = a;
    b.plot = kPlotB;
    b.building = factory->guid;
    b.state = world::kPlotOperational;
    b.pre_construct = true;
    b.seeded = true;
    h.fx.world->set_node_plots_for_test(kTestCitizenNode, {a, b});
    const auto purchase = [&]() {
        h.t += milliseconds(500);
        h.pc_call("ServerPurchaseServiceBuilding", [&](const schema::RpcSchema& r, repl::RpcParams& p) {
            repl::set_rpc_param(r, p, "NodeId", repl::Value::i64(kTestCitizenNode));
            repl::set_rpc_param(r, p, "PlotId", repl::Value::i64(kPlotA));
            repl::set_rpc_param(r, p, "BuildingId", repl::Value::i64(factory->guid));
            repl::set_rpc_param(r, p, "bFree", repl::Value::boolean(false));
            repl::set_rpc_param(r, p, "bSkipPlayerPhase", repl::Value::boolean(false));
        });
        h.run(milliseconds(300));
    };
    const auto plot_state = [&](int64_t plot) -> const world::PlotState* {
        const world::NodeElection* e = el->node(kTestCitizenNode);
        const auto it = e ? e->buildings.plots.find(plot) : decltype(e->buildings.plots.end()){};
        return e && it != e->buildings.plots.end() ? &it->second : nullptr;
    };

    // not the mayor: nothing is bought
    purchase();
    CHECK(h.s->buildings_bought() == 0 && h.s->building_refusals() == 1 && !plot_state(kPlotA), "a character who is not the node's mayor buys nothing");

    // the mayor buys a Factory with the node's money and stores
    el->set_mayor(kTestCitizenNode, me, 1);
    el->change_node(kTestCitizenNode, [&](world::NodeElection& e) {
        e.treasury = 600000;
        for (const auto& [item, q] : nb::cost_items(factory->node_cost)) e.stored[item] = q;
        return true;
    });
    size_t from = h.got.size();
    purchase();
    const world::PlotState* pa = plot_state(kPlotA);
    CHECK(h.s->buildings_bought() == 1 && pa && pa->state == world::kPlotUnderConstruction && pa->building == factory->guid && el->treasury(kTestCitizenNode) == 100000,
          "the mayor's ServerPurchaseServiceBuilding: the Factory is under construction, 500,000 copper from the treasury");
    const world::NodeElection* node = el->node(kTestCitizenNode);
    CHECK(node && !node->news.empty() && node->news.back().type == 1 && node->news.back().plot == kPlotA, "the node's feed: ServiceBuildingConstructionStarted");
    const std::vector<world::CityNodes::PlotLive> live = h.nodes->live_plots(kTestCitizenNode);
    CHECK(live.size() == 1 && live[0].plot == kPlotA && live[0].state == world::kPlotUnderConstruction && h.nodes->plots_revision(kTestCitizenNode) >= 1,
          "the registry's live plots show the site (the scenery and the town's people follow them)");

    // the progress
    from = h.got.size();
    h.t += milliseconds(500);
    h.pc_call("ServerGetBuildingConstructionProgress", [&](const schema::RpcSchema& r, repl::RpcParams& p) {
        repl::set_rpc_param(r, p, "Node", repl::Value::i64(kTestCitizenNode));
        repl::set_rpc_param(r, p, "Plot", repl::Value::i64(kPlotA));
    });
    h.run(milliseconds(300));
    auto calls = pc_calls_since(h, from);
    const repl::RpcCall* pc = last_call(calls, "ClientReceiveBuildingConstructionProgress");
    const repl::Value* pv = pc ? repl::rpcf::value_of(*pc->rpc, pc->params, "Progress") : nullptr;
    const repl::ArrayValue* progress = pv ? pv->get_if<repl::ArrayValue>() : nullptr;
    CHECK(pc && repl::rpcf::uint_or(repl::rpcf::value_of(*pc->rpc, pc->params, "BuildingState"), 99).value_or(99) == world::kPlotUnderConstruction &&
              call_int(*pc, "Plot") == kPlotA && progress && progress->elements.size() == 3 &&
              call_int(*pc, "ProjectTimeEnd") == pa->phase_ends_ms / 1000,
          "ServerGetBuildingConstructionProgress: the plot under construction, its three crates (none given yet), the week's end");

    // the Construction Foreman's window: nothing on the character's back, the window opens on the site
    from = h.got.size();
    h.s->open_contribution_window(h.conn, h.npc(), h.t);
    h.run(milliseconds(300));
    calls = pc_calls_since(h, from);
    const repl::RpcCall* wc = last_call(calls, "ClientShowBuildingContributionWindow");
    const repl::Value* iv = wc ? repl::rpcf::value_of(*wc->rpc, wc->params, "PlayerConstructionResourceCostIds") : nullptr;
    const repl::ArrayValue* ids = iv ? iv->get_if<repl::ArrayValue>() : nullptr;
    CHECK(wc && call_int(*wc, "PlotId") == kPlotA && call_int(*wc, "BuildingId") == factory->guid && ids && ids->elements.size() == 1 && h.s->contribution_windows() == 1 &&
              h.s->contribution_units() == 0,
          "the Foreman's line: ClientShowBuildingContributionWindow for the Factory's site, with its players' cost record");
    // the window's Fulfill with nothing the site needs
    h.t += milliseconds(500);
    h.pc_call("ServerFulfillServiceBuildingConstruction", [&](const schema::RpcSchema& r, repl::RpcParams& p) {
        repl::set_rpc_param(r, p, "NodeGuid", repl::Value::i64(kTestCitizenNode));
        repl::set_rpc_param(r, p, "PlotGuid", repl::Value::i64(kPlotA));
        repl::set_rpc_param(r, p, "BuildingGuid", repl::Value::i64(factory->guid));
    });
    h.run(milliseconds(300));
    CHECK(h.s->contribution_units() == 0 && plot_state(kPlotA)->given.empty(), "ServerFulfillServiceBuildingConstruction with nothing it needs gives nothing");

    // a game master's crates, then the world's clock builds it
    h.fx.world->set_gm_commands(true);
    h.t += milliseconds(500);
    h.pc_call("ServerContributeToServiceBuildingConstructionCheat", [&](const schema::RpcSchema& r, repl::RpcParams& p) {
        repl::set_rpc_param(r, p, "NodeRecordGuid", repl::Value::i64(kTestCitizenNode));
        repl::set_rpc_param(r, p, "PlotRecordGuid", repl::Value::i64(kPlotA));
        repl::set_rpc_param(r, p, "ItemRecordGuid", repl::Value::i64(0));
        repl::set_rpc_param(r, p, "Quantity", repl::Value::i64(0));
    });
    h.run(milliseconds(300));
    pa = plot_state(kPlotA);
    CHECK(pa && pa->complete_ms != 0 && nb::needs(el->node(kTestCitizenNode)->buildings, a).empty(), "ServerContributeToServiceBuildingConstructionCheat: all its crates, built in a minute");
    h.fx.world->tick_buildings(wall_now() + 61000);
    pa = plot_state(kPlotA);
    CHECK(pa && pa->state == world::kPlotOperational && h.fx.world->building_mods(kTestCitizenNode).size() == 2 * factory->mods_count && h.fx.world->building_events() >= 1,
          "the world's tick a minute later: the Factory is Operational; both Factories' modifications run (their boons)");

    // a game master's free expansion, its players' phase skipped
    from = h.got.size();
    h.t += milliseconds(500);
    h.pc_call("ServerPurchaseExpansion", [&](const schema::RpcSchema& r, repl::RpcParams& p) {
        repl::set_rpc_param(r, p, "NodeGuid", repl::Value::i64(kTestCitizenNode));
        repl::set_rpc_param(r, p, "PlotGuid", repl::Value::i64(kPlotA));
        repl::set_rpc_param(r, p, "ExpansionSkillRankGuid", repl::Value::i64(converters->rank));
        repl::set_rpc_param(r, p, "bSkipPlayerPhase", repl::Value::boolean(true));
        repl::set_rpc_param(r, p, "bFree", repl::Value::boolean(true));
    });
    h.run(milliseconds(300));
    pa = plot_state(kPlotA);
    CHECK(pa && ((pa->exp_state == world::kExpConstruction && pa->exp_rank == converters->rank) || pa->ranks.size() == 1) && el->treasury(kTestCitizenNode) == 100000,
          "a game master's ServerPurchaseExpansion with bFree and bSkipPlayerPhase: no vote, no crates, straight to its construction (or built by the session's own tick), nothing paid");
    h.fx.world->tick_buildings(wall_now() + 3600000);
    pa = plot_state(kPlotA);
    CHECK(pa && pa->ranks.size() == 1 && pa->exp_state == world::kExpNone, "... and built by the world's tick");

    // the seed's Factory is demolished; a demolition has no vote
    h.t += milliseconds(500);
    h.pc_call("ServerCommenceDemolition", [&](const schema::RpcSchema& r, repl::RpcParams& p) { repl::set_rpc_param(r, p, "Plot", repl::Value::i64(kPlotB)); });
    h.run(milliseconds(300));
    const world::PlotState* pb = plot_state(kPlotB);
    CHECK(pb && pb->state == world::kPlotDemolition && pb->building == factory->guid, "ServerCommenceDemolition names the plot alone: the node is found, the seed's Factory is being demolished");
    const uint32_t refused = h.s->building_refusals();
    h.t += milliseconds(500);
    h.pc_call("ServerVoteOnDemolition", [&](const schema::RpcSchema& r, repl::RpcParams& p) {
        repl::set_rpc_param(r, p, "Plot", repl::Value::i64(kPlotB));
        repl::set_rpc_param(r, p, "bIsVoteYes", repl::Value::boolean(true));
    });
    h.run(milliseconds(300));
    CHECK(h.s->building_refusals() == refused + 1, "ServerVoteOnDemolition is refused (the data's demolitions have no vote)");
    std::string why;
    world::Elections back;
    CHECK(back.from_json(el->to_json(), why) && back.node(kTestCitizenNode) && back.node(kTestCitizenNode)->buildings == el->node(kTestCitizenNode)->buildings,
          "the node's buildings are in its elections file");
}

AOC_REGISTER_TEST(test_node_buildings_session);
