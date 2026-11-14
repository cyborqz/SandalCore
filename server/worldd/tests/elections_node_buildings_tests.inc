// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/elections_node_buildings_tests.inc: the service buildings' rules (world/node_buildings.h) over the data's table (world/node_buildings_table.h)
//  (included by elections_registered.inc)
// ============================================================================

namespace world = ::aoc::worldd::world;

namespace nbt {

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

int64_t point_type_named(const char* name) {
    for (size_t i = 0; i < nb::kPointTypes_count; ++i) {
        if (std::string(nb::kPointTypes[i].name) == name) return nb::kPointTypes[i].guid;
    }
    return 0;
}

void stock(world::NodeElection& n, const nb::CostList& items, int times = 1) {
    for (const auto& [item, q] : items) n.stored[item] += q * times;
}

nb::CostList rank_items(const nb::TreeNode& t) {
    nb::CostList out;
    for (size_t i = t.items_first; i < t.items_first + t.items_count; ++i) out.emplace_back(nb::kRankItems[i].item, nb::kRankItems[i].quantity);
    return out;
}

int64_t units_of(const nb::CostList& l) {
    int64_t u = 0;
    for (const auto& [item, q] : l) u += q;
    return u;
}

bool has_event(const std::vector<nb::Event>& ev, nb::Event::Kind k, int64_t plot) {
    for (const nb::Event& e : ev) {
        if (e.kind == k && e.plot == plot) return true;
    }
    return false;
}

}  // namespace nbt

void test_node_buildings() {
    std::printf("the service buildings (world/node_buildings.h)\n");
    namespace nb = world::node_buildings;
    using namespace nbt;
    const nb::Building* factory = building_named("Construction_Support_Factory_Building");
    const nb::Building* smithy = building_named("Construction_Active_Smithy_Building");
    const nb::Building* empty = building_named("Construction_EmptyPlot_Building");
    const nb::Building* project = building_named("Default_NodeXP_ConstructionProject_Small_01");
    const int64_t fabrication = point_type_named("Node_Fabrication");
    CHECK(factory && smithy && empty && project && fabrication != 0, "the data: the Factory, the Smithy, the empty plot, a NodeXP project and the Node_Fabrication points");
    if (!factory || !smithy || !empty || !project || fabrication == 0) return;
    CHECK(nb::buyable(*factory) && nb::buyable(*smithy) && !nb::buyable(*empty) && !nb::buyable(*project),
          "the Factory and the Smithy are sold in the Building Manager; the empty plot and the NodeXP project are not");
    CHECK(nb::cost_copper(factory->node_cost) == 500000 && nb::cost_items(factory->node_cost).size() == 4 && factory->construction_ms == 604800000 && factory->can_time_out,
          "the Factory: 500,000 copper and four commodities from the node, then a week for the players");
    const nb::CostList crates = nb::cost_items(factory->player_cost);
    CHECK(crates.size() == 3 && crates[0].second == 17 && nb::interpolation_ms(factory->phases) == 60000, "... 17 each of three construction crates, then a minute of building");
    const std::map<int64_t, int32_t> grant = nb::granted_by(nullptr, factory->guid);
    CHECK(grant.count(fabrication) && grant.at(fabrication) == 1, "... and when built, 1 Node_Fabrication skill point");
    CHECK(nb::node_point_types().size() == 5, "five node kinds of skill points (Vocational, Fabrication, Civic, Cultural, Scholarly)");

    // a node with three plots: an empty service plot, the seed's Factory and a small NodeXP project
    constexpr int64_t kPlotA = 9001, kPlotB = 9002, kPlotC = 9003;
    world::PlotInfo a;
    a.plot = kPlotA;
    a.building = empty->guid;
    a.empty_building = empty->guid;
    a.candidates = {factory->guid, smithy->guid};
    world::PlotInfo b = a;
    b.plot = kPlotB;
    b.building = factory->guid;
    b.state = world::kPlotOperational;
    b.pre_construct = true;
    b.seeded = true;
    world::PlotInfo c;
    c.plot = kPlotC;
    c.building = project->guid;
    c.state = world::kPlotUnderConstruction;
    c.pre_construct = true;
    c.candidates = {project->guid};
    const std::vector<world::PlotInfo> plots{a, b, c};
    const int64_t t = 1'800'000'000'000ll;

    // buying
    world::NodeElection n;
    CHECK(nb::purchase(n, a, factory->guid, t) == nb::Result::NoMoney && n.buildings.plots.empty(), "a node with no money buys nothing");
    n.treasury = 600000;
    stock(n, nb::cost_items(factory->node_cost));
    CHECK(nb::purchase(n, a, empty->guid, t) == nb::Result::NotCandidate && nb::purchase(n, b, factory->guid, t) == nb::Result::NotEmpty &&
              nb::purchase(n, a, project->guid, t) == nb::Result::NotCandidate,
          "the empty building is not among the plot's buildings to buy, a built plot is not empty, another plot's building is no candidate");
    CHECK(nb::purchase(n, a, factory->guid, t) == nb::Result::Ok && n.treasury == 100000 && n.stored.empty() && n.buildings.plots.count(kPlotA) &&
              n.buildings.plots.at(kPlotA).state == world::kPlotUnderConstruction && n.buildings.plots.at(kPlotA).phase_ends_ms == t + 604800000,
          "the mayor buys a Factory: the treasury and the stores pay, the plot is under construction for a week");
    CHECK(nb::needs(n.buildings, a) == crates && nb::now_of(n.buildings, a).building == factory->guid, "... and it needs the players' crates");

    // the players give
    const int64_t mining = crates[0].first;
    nb::CostList taken = nb::contribute(n, a, nb::CostList{{mining, 1}, {12345, 4}}, t + 1000);
    CHECK(taken.size() == 1 && taken[0].first == mining && taken[0].second == 1 && nb::needs(n.buildings, a)[0].second == 16, "a crate on the back: taken; an item it does not need is not");
    const std::vector<nb::NeedRow> rows = nb::progress(n.buildings, a);
    CHECK(rows.size() == 3 && rows[0].given == 1 && rows[0].asked == 17 && rows[1].given == 0, "the window's progress: 1 of 17, 0 of 17, 0 of 17");
    taken = nb::contribute(n, a, nb::CostList{{crates[0].first, 50}, {crates[1].first, 17}, {crates[2].first, 17}}, t + 2000);
    CHECK(units_of(taken) == 50 && nb::needs(n.buildings, a).empty() && n.buildings.plots.at(kPlotA).complete_ms == t + 62000, "the rest given (no more than it needs): built in a minute");
    CHECK(nb::contribute(n, a, nb::CostList{{mining, 1}}, t + 3000).empty(), "a site with all it needs takes nothing more");
    std::vector<nb::Event> ev = nb::advance(n, plots, t + 30000);
    CHECK(!has_event(ev, nb::Event::Built, kPlotA) && n.buildings.next_upkeep_ms == t + 30000 + nb::kUpkeepPeriodMs, "not before the minute (the first look schedules the upkeep a week ahead)");
    ev = nb::advance(n, plots, t + 62000);
    CHECK(has_event(ev, nb::Event::Built, kPlotA) && n.buildings.plots.at(kPlotA).state == world::kPlotOperational && n.buildings.plots.at(kPlotA).built_ms == t + 62000 &&
              n.buildings.plots.at(kPlotA).given.empty(),
          "a minute later: Operational");
    std::map<int64_t, int32_t> points = nb::skill_points(n.buildings, plots);
    CHECK(points[fabrication] == 2, "the two Factories (the mayor's and the seed's): 2 Node_Fabrication points");
    CHECK(nb::running_mods(n.buildings, plots).size() == 2 * factory->mods_count, "the running modifications: the two Factories'");

    // a NodeXP project: the data starts it under construction; players give; it never times out; the node gets its XP
    const nb::CostList project_crates = nb::cost_items(project->player_cost);
    CHECK(!project_crates.empty() && !project->can_time_out && project->completion_xp == 100000.0, "the small NodeXP project: crates, no time limit, 100,000 node XP");
    CHECK(nb::needs(n.buildings, c) == project_crates && !n.buildings.plots.count(kPlotC), "it needs its crates before anyone gives (no entry of its own yet)");
    nb::contribute(n, c, project_crates, t + 70000);
    ev = nb::advance(n, plots, t + 70000 + nb::interpolation_ms(project->phases) + 1);
    bool xp = false;
    for (const nb::Event& e : ev) xp = xp || (e.kind == nb::Event::Built && e.plot == kPlotC && e.node_xp == 100000.0);
    CHECK(xp && nb::now_of(n.buildings, c).state == world::kPlotOperational, "given all its crates, it is built and the event carries its 100,000 node XP");

    // a support building's expansion: the citizens' two-day vote first
    const nb::TreeNode* conv = tree_node_named("SupportBuilding_Factory_EXP_0A_CommodityConverters");
    const nb::Expansion* xc = conv ? nb::expansion(conv->expansion) : nullptr;
    CHECK(conv && xc && xc->vote_ms == 172800000, "the data: the Factory's Commodity Converters, voted on for two days");
    if (conv && xc) {
        CHECK(nb::purchase_expansion(n, plots, a, conv->rank, 4, t + 80000, /*free=*/true) == nb::Result::Ok && n.buildings.plots.at(kPlotA).exp_state == world::kExpVote &&
                  n.buildings.plots.at(kPlotA).exp_ends_ms == t + 80000 + xc->vote_ms,
              "bought (free): the citizens vote for two days");
        CHECK(nb::vote(n, a, 5, true) == nb::Result::Ok && nb::cancel_expansion_purchase(n, a) == nb::Result::Ok && n.buildings.plots.at(kPlotA).exp_state == world::kExpNone,
              "a citizen votes; ServerCancelExpansionPurchase ends the project in its vote");
        nb::purchase_expansion(n, plots, a, conv->rank, 4, t + 90000, /*free=*/true);
        nb::vote(n, a, 5, true);
        ev = nb::advance(n, plots, t + 90000 + xc->vote_ms);
        const uint8_t after = n.buildings.plots.at(kPlotA).exp_state;
        CHECK(has_event(ev, nb::Event::ExpansionStarted, kPlotA) && (after == world::kExpPlayerContribution || after == world::kExpConstruction),
              "the vote passes: on to its players' phase");
        nb::cancel_expansion_purchase(n, a);
        CHECK(nb::purchase_expansion(n, plots, a, conv->rank, 4, t + 100000, true, /*skip_players=*/true) == nb::Result::Ok &&
                  n.buildings.plots.at(kPlotA).exp_state == world::kExpConstruction,
              "a game master's bSkipPlayerPhase skips the vote and the crates");
        nb::cancel_expansion_purchase(n, a);   // (no project left: the construction cannot be cancelled, so the next look builds it)
        n.buildings.plots.at(kPlotA).exp_state = world::kExpNone;
        n.buildings.plots.at(kPlotA).exp_rank = 0;
    }

    // a construction that runs out of time
    world::NodeElection m;
    m.treasury = 600000;
    stock(m, nb::cost_items(factory->node_cost));
    nb::purchase(m, a, factory->guid, t);
    ev = nb::advance(m, plots, t + 604800000 + 1);
    CHECK(has_event(ev, nb::Event::TimedOut, kPlotA) && !m.buildings.plots.count(kPlotA) && m.treasury == 100000,
          "a week with no crates: the plot is empty again, and the node's cost is not given back");
    world::NodeElection g;
    CHECK(nb::purchase(g, a, factory->guid, t, /*free=*/true, /*skip_players=*/true) == nb::Result::Ok && nb::needs(g.buildings, a).empty() &&
              g.buildings.plots.at(kPlotA).complete_ms == t + 60000,
          "a game master's bFree and bSkipPlayerPhase: nothing paid, the crates count as given");

    // an expansion: a Smithy on plot A (a game master's), the Weaponsmith line
    world::NodeElection s;
    s.treasury = 10'000'000;
    CHECK(nb::set_state(s, a, smithy->guid, world::kPlotOperational, t) == nb::Result::Ok && nb::now_of(s.buildings, a).state == world::kPlotOperational, "ServerSetPlotState: a Smithy stands on plot A");
    const nb::TreeNode* w0 = tree_node_named("ServiceBuilding_Smithy_EXP_0A_Weaponsmith");
    const nb::TreeNode* w1 = tree_node_named("ServiceBuilding_Smithy_EXP_1A1_Weaponsmith");
    CHECK(w0 && w1 && w0->tree == smithy->tree && w1->req_count == 1, "the data: the Smithy's Weaponsmith expansions, the second needing the first");
    if (!w0 || !w1) return;
    stock(s, rank_items(*w0), 2);
    stock(s, rank_items(*w1), 2);
    CHECK(nb::purchase_expansion(s, plots, a, w1->rank, 3, t) != nb::Result::Ok, "the second Weaponsmith expansion before the first: refused");
    CHECK(nb::purchase_expansion(s, plots, a, w0->rank, 2, t) == nb::Result::LevelTooLow, "an Encampment cannot expand (a Village's level is asked)");
    CHECK(nb::purchase_expansion(s, plots, b, w0->rank, 3, t) == nb::Result::NoRank, "the Smithy's expansion on the Factory: no such expansion there");
    const int64_t treasury_before = s.treasury;
    CHECK(nb::purchase_expansion(s, plots, a, w0->rank, 3, t) == nb::Result::Ok && s.treasury == treasury_before - w0->copper,
          "the first Weaponsmith expansion: bought with the node's copper and commodities");
    points = nb::skill_points(s.buildings, plots);
    CHECK(points[fabrication] == 0, "... and the seed's Factory's Fabrication point");
    const nb::Expansion* x0 = nb::expansion(w0->expansion);
    CHECK(x0 && s.buildings.plots.at(kPlotA).exp_state == world::kExpPlayerContribution && s.buildings.plots.at(kPlotA).exp_ends_ms == t + x0->player_phase_ms &&
              nb::needs(s.buildings, a) == nb::cost_items(x0->player_cost),
          "its players' phase: two days for its crates");
    CHECK(nb::purchase_expansion(s, plots, a, w1->rank, 3, t) == nb::Result::Busy, "one project at a time");
    nb::contribute(s, a, nb::cost_items(x0->player_cost), t + 1000);
    CHECK(s.buildings.plots.at(kPlotA).exp_state == world::kExpConstruction, "given its crates: its construction");
    ev = nb::advance(s, plots, t + 1000 + x0->construction_ms);
    CHECK(has_event(ev, nb::Event::ExpansionBuilt, kPlotA) && s.buildings.plots.at(kPlotA).ranks.size() == 1 && s.buildings.plots.at(kPlotA).exp_state == world::kExpNone,
          "then it is built");
    CHECK(nb::purchase_expansion(s, plots, a, w1->rank, 3, t + 2000) == nb::Result::NoSkillPoints, "the next needs a Fabrication point the node has spent");
    CHECK(nb::demolish(s, plots, b, t + 2000) == nb::Result::PointsInUse, "the seed's Factory whose point is spent cannot be demolished");
    // the removal vote
    CHECK(nb::remove_expansion(s, a, w0->rank, t + 3000) == nb::Result::Ok && s.buildings.plots.at(kPlotA).exp_state == world::kExpRemovalVote &&
              s.buildings.plots.at(kPlotA).exp_ends_ms == t + 3000 + x0->removal_vote_ms,
          "ServerRemoveExpansion: a two-day vote");
    CHECK(nb::vote(s, a, 7, false) == nb::Result::Ok && nb::vote(s, a, 7, true) == nb::Result::AlreadyVoted, "a citizen votes once");
    ev = nb::advance(s, plots, t + 3000 + x0->removal_vote_ms);
    CHECK(has_event(ev, nb::Event::ExpansionKept, kPlotA) && s.buildings.plots.at(kPlotA).ranks.size() == 1, "more against than for: the expansion stays");
    nb::remove_expansion(s, a, w0->rank, t + 4000);
    nb::vote(s, a, 7, true);
    CHECK(nb::cancel_expansion_removal(s, a) == nb::Result::Ok && s.buildings.plots.at(kPlotA).exp_state == world::kExpNone, "ServerCancelExpansionRemoval ends the vote");
    nb::remove_expansion(s, a, w0->rank, t + 5000);
    ev = nb::advance(s, plots, t + 5000 + x0->removal_vote_ms);
    CHECK(has_event(ev, nb::Event::ExpansionRemoved, kPlotA) && s.buildings.plots.at(kPlotA).ranks.empty() && nb::skill_points(s.buildings, plots)[fabrication] == 1,
          "a vote nobody opposes passes: the expansion goes, its point is free again");
    // the demolition
    CHECK(nb::demolish(s, plots, c, t + 6000) == nb::Result::NotOperational, "a building still under construction cannot be demolished");
    CHECK(nb::demolish(n, plots, c, t + 6000) == nb::Result::CannotDemolish, "nor a NodeXP project once built (it is no building the mayor buys)");
    CHECK(nb::demolish(s, plots, b, t + 6000) == nb::Result::Ok && nb::now_of(s.buildings, b).state == world::kPlotDemolition, "the seed's Factory is demolished");
    CHECK(nb::cancel_demolition(s, b) == nb::Result::Ok && nb::now_of(s.buildings, b).state == world::kPlotOperational, "... or not (ServerCancelDemolition)");
    nb::demolish(s, plots, b, t + 7000);
    ev = nb::advance(s, plots, t + 7000 + factory->demolition_ms);
    CHECK(has_event(ev, nb::Event::Demolished, kPlotB) && nb::now_of(s.buildings, b).state == world::kPlotEmpty && nb::now_of(s.buildings, b).building == empty->guid &&
              s.buildings.plots.count(kPlotB),
          "a day later the plot is empty (an entry of its own: the data's row still names the Factory)");

    // the upkeep: a Factory the mayor built on plot A pays it, the seed's on plot B does not
    world::NodeElection u;
    nb::set_state(u, a, factory->guid, world::kPlotOperational, t);
    ev = nb::advance(u, plots, t);
    ev = nb::advance(u, plots, t + 1, /*force_upkeep=*/true);
    CHECK(has_event(ev, nb::Event::Disarray, kPlotA) && nb::now_of(u.buildings, a).state == world::kPlotDisarray && nb::skill_points(u.buildings, plots)[fabrication] == 1 &&
              nb::running_mods(u.buildings, plots).size() == factory->mods_count,
          "an upkeep the node cannot pay: the mayor's Factory falls into disarray (no point, no effect)");
    CHECK(!has_event(ev, nb::Event::Disarray, kPlotB) && nb::now_of(u.buildings, b).state == world::kPlotOperational, "... the seed's Factory pays no upkeep of its own");
    u.treasury = 10'000'000;
    stock(u, nb::cost_items(factory->maintenance_cost), 2);
    stock(u, nb::cost_items(factory->disarray_repair_cost), 2);
    ev = nb::advance(u, plots, t + 2, /*force_upkeep=*/true);
    CHECK(has_event(ev, nb::Event::Repairing, kPlotA) && nb::now_of(u.buildings, a).state == world::kPlotRepairing, "the next upkeep it can pay, with the repair: it is repaired");
    ev = nb::advance(u, plots, t + 3 + factory->repair_ms);
    CHECK(has_event(ev, nb::Event::Repaired, kPlotA) && nb::now_of(u.buildings, a).state == world::kPlotOperational, "... and Operational again");
    const int64_t before_upkeep = u.treasury;
    ev = nb::advance(u, plots, t + 4, /*force_upkeep=*/true, /*upkeep_scale=*/0.5);
    CHECK(before_upkeep - u.treasury == nb::cost_copper(factory->maintenance_cost, 0.5), "ServerForceBuildingMaintainance at 50 %: half the copper");
    CHECK(nb::income_due(u.buildings, t) == 0 && nb::income_due(u.buildings, t + 3 * nb::kIncomePeriodMs) == 3, "the income: an hour after the first look, one hour's worth each hour");

    // elections.json
    const world::NodeBuildings back = nb::from_json(nb::to_json(s.buildings));
    CHECK(back == s.buildings, "the buildings read back from their JSON as they were");
    world::Elections el;
    el.change_node(77, [&](world::NodeElection& e) {
        e.buildings = s.buildings;
        return true;
    });
    world::Elections again;
    std::string why;
    CHECK(again.from_json(el.to_json(), why) && again.node(77) && again.node(77)->buildings == s.buildings, "elections.json keeps a node's buildings");
}
AOC_REGISTER_TEST(test_node_buildings);
