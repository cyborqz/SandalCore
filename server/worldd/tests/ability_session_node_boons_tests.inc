// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/ability_session_node_boons_tests.inc: the boons of a node's running modifications (session/player_node_boons.cpp, contentc version 45)
//  (registered with AOC_REGISTER_TEST: test_worldd_ability_session.cpp runs it through aoc_tests::run; it is included from ability_session_registered.inc)
//
//  A citizen's boon that can be applied globally reaches the node's citizen wherever it stands: a running modification of the node's buildings puts it on as an
//  effect of the ability runtime, a second running modification that gives it makes two stacks, and the boon goes when the modifications stop.  The node's
//  running modifications are set by hand (the effect is one the real content.db has had since contentc version 43).
// ============================================================================
#include "world/elections.h"
#include "world/node_rewards.h"

static constexpr int64_t kBoonNode = 108646610851452ll;   // Verra_RVR_Halcyon
static constexpr int64_t kBoonMod = 7790001, kBoonMod2 = 7790002;
static constexpr int64_t kArmorTypeHeavy1 = 6064633946081984518ll;   // ArmorType_Heavy_1 (PhysicalMitigation +0.1): an effect of the real tables

static void test_node_boons() {
    std::printf("the nodes' boons (contentc version 45): a citizen's global boon from the node's running modifications, stacked per modification, gone when they stop\n");
    const auto prepare = [](PlayerWorld& w) {
        world::NodeRewards book;
        world::NodeBoon b;
        b.effect = kArmorTypeHeavy1;   // (an effect of the real tables)
        b.on_character = true;
        b.citizens = true;
        b.global = true;
        book.add_boon_for_test(kBoonMod, b);
        book.add_boon_for_test(kBoonMod2, b);
        w.set_node_rewards(std::move(book));
        w.set_elections(world::ElectionRules{}, "");
    };
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    svc::Character citizen = bard25();
    svc::Citizenship cz;
    cz.node_guid = kBoonNode;
    cz.primary_character_id = citizen.id;
    cz.joined_ms = now_ms - 86400000;
    cz.dues_due_ms = now_ms + 10LL * 86400000;
    citizen.citizenship = cz;
    Run a(0, true, &citizen, prepare);
    phase_in(a);
    a.play(1.0, false);
    const AbilityPipeline* p = a.h.s->pipeline();
    world::Elections* el = a.h.fx.world->elections();
    CHECK(p && el && a.h.s->citizenship().node_guid == kBoonNode, "(a citizen of Halcyon, on the real content.db's ability runtime)");
    if (!p || !el) return;
    CHECK(a.h.s->node_boons().empty() && p->stacks_of(kArmorTypeHeavy1) == 0, "no running modification: no boon");

    std::vector<int64_t> running;
    const auto run_mod = [&](int64_t mod) {
        running.push_back(mod);
        a.h.fx.world->set_building_mods_for_test(kBoonNode, running);
        a.h.s->refresh_node_boons_for_test();
        a.play(0.5, false);
    };
    run_mod(kBoonMod);
    CHECK(a.h.s->node_boons().size() == 1 && a.h.s->node_boons().count(kArmorTypeHeavy1) && a.h.s->node_boons().at(kArmorTypeHeavy1) == 1 && p->stacks_of(kArmorTypeHeavy1) == 1,
          "a running modification of the citizen's node puts its global boon on, wherever the citizen stands: one stack");
    run_mod(kBoonMod2);
    CHECK(a.h.s->node_boons().at(kArmorTypeHeavy1) == 2 && p->stacks_of(kArmorTypeHeavy1) == 2, "a second modification that gives the same boon: two stacks");
    a.h.fx.world->set_building_mods_for_test(kBoonNode, {});
    a.h.s->refresh_node_boons_for_test();
    a.play(0.5, false);
    CHECK(a.h.s->node_boons().empty() && p->stacks_of(kArmorTypeHeavy1) == 0, "the modifications stop: the boon is taken off");

    // the rules: a citizen's boon asks the node's citizenship and its rank, a zone's boon the zone
    world::NodeBoon citizen_boon;
    citizen_boon.on_character = true;
    citizen_boon.citizens = true;
    citizen_boon.min_rank = 1;
    citizen_boon.global = true;
    citizen_boon.global_min_rank = 1;
    world::NodeBoon zone_boon;
    zone_boon.on_character = true;
    world::NodeBoon caravan = zone_boon;
    caravan.on_character = false;
    CHECK(world::boon_reaches(citizen_boon, true, 1, false) && !world::boon_reaches(citizen_boon, true, 0, true) && !world::boon_reaches(citizen_boon, false, world::kNoReputationRank, true),
          "a citizen's boon (Commoner, global): a Commoner anywhere; not a Peasant, not a stranger in the zone");
    CHECK(world::boon_reaches(zone_boon, false, world::kNoReputationRank, true) && !world::boon_reaches(zone_boon, true, 4, false) && !world::boon_reaches(caravan, true, 4, true),
          "a zone's boon: anyone in the zone, nobody outside it; a caravan's boon reaches no character");
}

AOC_REGISTER_TEST(test_node_boons);
