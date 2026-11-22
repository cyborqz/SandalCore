// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/ability_session_shared_data_tests.inc: the split timer and the shared
//  WorldData of the ability session program
// ============================================================================

/// Two runs and one second of play: the timer counts two fresh worlds and puts
/// seconds in the build, the join and the play.  The line has no text without the
/// variable.
static void test_split_timer_counts_what_it_measures() {
    std::printf("the split timer: two worlds and one second of play are counted apart\n");
    const SplitTimes before = SplitTimes::now();
    const auto wall_started = std::chrono::steady_clock::now();
    {
        Run first;
        Run second;
        first.play(1.0, false);
    }
    const double wall = SplitTimes::seconds_since(wall_started);
    const SplitTimes after = SplitTimes::now();
    const size_t worlds = after.worlds - before.worlds;
    const double build = after.build_s - before.build_s;
    const double join = after.join_s - before.join_s;
    const double play = after.play_s - before.play_s;
    CHECKF(worlds == 2, "two Harness objects built two worlds (%zu)", worlds);
    CHECKF(build > 0 && join > 0 && play > 0, "world build, join and play each took time (%.3f s, %.3f s, %.3f s)", build, join, play);
    CHECKF(build + join + play <= wall + 0.05, "their sum is not above the test's wall time (%.3f s of %.3f s)", build + join + play, wall);
    CHECK(SplitTimes::line(false).empty(), "without AOC_TEST_TIMES no split line is printed");
    CHECKF(SplitTimes::line(true).rfind("[time] split:", 0) == 0, "with AOC_TEST_TIMES the split line is printed (%s)", SplitTimes::line(true).c_str());
}

/// The five fixtures share the one WorldData of their database file; the count of
/// loads grows by at most one and equals the number of database files opened.
static void test_world_data_is_loaded_once_per_program() {
    std::printf("the game data: five fixtures load their database's WorldData once\n");
    const size_t before = WorldData::loads();
    {
        Fixture a;
        Fixture b;
        Fixture c;
        Fixture d;
        Fixture e;
        (void)a; (void)b; (void)c; (void)d; (void)e;
    }
    const size_t after = WorldData::loads();
    CHECKF(after - before <= 1, "five fixtures loaded their game data once (%zu load(s))", after - before);
    CHECKF(after == WorldData::files() && WorldData::files() <= 2, "the loads are one per database file opened so far (%zu load(s), %zu file(s))",
           after, WorldData::files());
}

/// The counts a world reports of the data it reads; two worlds from the same
/// content.db must report the same ones.
static std::string world_data_counts(const PlayerWorld& w) {
    const auto* rules = w.ability_rules();
    const auto* bundle = w.stat_bundle();
    const auto* curve = w.level_curve();
    const auto* talents = w.talents();
    const auto* professions = w.professions();
    const auto* weapons = w.weapon_lines();
    const auto* currencies = w.currencies();
    const auto* catalog = w.ability_catalog();
    const auto* mounts = w.mounts();
    char buf[512];
    std::snprintf(buf, sizeof buf, "%zu|%zu|%zu|%d|%zu|%zu|%zu|%zu|%zu|%zu|%zu|%zu|%zu|%zu|%zu|%zu",
                  rules ? rules->effects.size() : 0,
                  bundle ? bundle->stats.size() : 0,
                  bundle ? bundle->classes.size() : 0,
                  curve ? curve->max_level() : -1,
                  w.auto_grants().size(),
                  talents ? talents->tree_count() : 0,
                  professions ? professions->size() : 0,
                  weapons ? weapons->size() : 0,
                  currencies ? currencies->tiers().size() : 0,
                  catalog ? catalog->abilities() : 0,
                  catalog ? catalog->effects() : 0,
                  catalog ? catalog->hits() : 0,
                  catalog ? catalog->exprs() : 0,
                  mounts ? mounts->data().mounts.size() : 0,
                  mounts ? mounts->data().abilities.size() : 0,
                  mounts ? mounts->data().effects.size() : 0);
    return buf;
}

/// A world built from the shared WorldData reports the same data as one built with
/// its own: the production constructor is the reference.
static void test_a_world_from_shared_data_equals_one_built_alone() {
    std::printf("a world from the shared data holds the same data as one built alone\n");
    Fixture shared;
    aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
    PlayerWorld alone(shared.content, "", bs::GameStateMode::Tier2, kServerId, guids);
    CHECKF(shared.world->ready() && alone.ready(), "the world from the shared data and the world built alone are ready");
    const std::string from_shared = world_data_counts(*shared.world);
    const std::string from_alone = world_data_counts(alone);
    CHECKF(from_shared == from_alone, "the two worlds report the same data counts (%s and %s)", from_shared.c_str(), from_alone.c_str());
}

/// The auto-grant list a world hands out is the shared data's own list: a copy would
/// die at the semicolon while a span a caller holds still points at it.
static_assert(std::is_lvalue_reference_v<decltype(std::declval<const PlayerWorld&>().auto_grants())>,
              "auto_grants() hands out a reference to the list, not a copy");

static void test_auto_grants_are_the_shared_data_list() {
    std::printf("the auto-grant list a world hands out is the shared data's own\n");
    Fixture shared;
    CHECKF(!shared.world->auto_grants().empty(), "the shared data grants ranks by level (%zu)", shared.world->auto_grants().size());
    CHECK(shared.world->auto_grants().data() == shared.data->auto_grants().data(),
          "a world from the shared data hands out its own list, not a copy");
    Fixture second;
    CHECK(second.world->auto_grants().data() == shared.world->auto_grants().data(),
          "a second world from the same shared data hands out the same list");
    aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
    PlayerWorld alone(shared.content, "", bs::GameStateMode::Tier2, kServerId, guids);
    CHECK(alone.auto_grants().data() == alone.auto_grants().data(),
          "a world built alone hands out one list, the same on every call");
    CHECKF(alone.auto_grants().size() == shared.world->auto_grants().size(), "both worlds hand out the same number of ranks (%zu)",
           alone.auto_grants().size());
}

/// A case's player, actors and player id do not survive into the next case's world:
/// every Fixture makes a fresh PlayerWorld from the shared data.
static void test_a_fresh_world_holds_nothing_of_the_case_before() {
    std::printf("a fresh world holds nothing of the case before it\n");
    {
        WolfWorld ww;
        const svc::Character bard = bard25();
        Run r(0, true, &bard, [&](PlayerWorld& w) { ww.prepare(w); });
        r.play(0.5, false);
    }
    Fixture fresh;
    CHECKF(fresh.world->registry().size() == 0, "the fresh world's registry is empty (%zu actor(s))", fresh.world->registry().size());
    CHECKF(fresh.world->area_report() == nullptr && fresh.world->quests() == nullptr && fresh.world->npcs() == nullptr &&
               fresh.world->combat() == nullptr && fresh.world->aftermath() == nullptr,
           "no area, quests, ground, fights or aftermath of the earlier case is in it");
    const int32_t first_id = fresh.world->new_session(0).player_id;
    CHECKF(first_id == 1, "its first player id is 1 (%d)", first_id);
}

AOC_REGISTER_TEST(test_a_world_from_shared_data_equals_one_built_alone);
AOC_REGISTER_TEST(test_auto_grants_are_the_shared_data_list);
AOC_REGISTER_TEST(test_a_fresh_world_holds_nothing_of_the_case_before);
