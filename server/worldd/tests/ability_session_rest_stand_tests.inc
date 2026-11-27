// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/ability_session_rest_stand_tests.inc: a Rest the client started stands up on the client
//
//  The client keeps its own Rest montage in its sit loop and takes the server's jump to the end section only when the item says the server decided it (bBranchPredictedOnAutonomous 0).
//  So every end of such a Rest (the player walks away, the client's cancel, a second press, ServerEndAbility) jumps the item with that bit clear, and when the montage is over, 1.069 s later,
//  the item's montage is cleared (the item stays listed).  A Rest the /sit command starts is the server's already and is left as it was.
//  (registered with AOC_REGISTER_TEST: test_worldd_ability_session.cpp runs it through aoc_tests::run; it is included from ability_session_registered.inc)
// ============================================================================

/// The one Rest item the client holds, or null.
static const Mirror::ActiveAbility* held_rest_item(const Run& r) {
    for (const auto& [id, a] : r.m.abilities)
        if (a.ability == kRest) return &a;
    return nullptr;
}

/// The Rest item the client holds has just jumped to its end section.  Checks the jump as the client reads it, plays on to the clear of its montage (`moving`: whether the player moves while
/// it plays) and checks that: no montage, the next counter, the item still listed, and nothing told or changed after it.
static void check_rest_stands_up_on_the_wire(Run& r, bool moving, int32_t jump_counter, const std::string& what) {
    const Mirror::ActiveAbility* item = held_rest_item(r);
    CHECK(item && item->may_jump && item->montage_position == 5.82606792f && item->animation_counter == jump_counter && !item->branch_predicted && item->has_montage && item->montage != 0,
          (what + ": the jump to the end section reaches the client as the server's branch (bBranchPredictedOnAutonomous 0), the montage still on the item").c_str());
    const size_t ends = r.m.end_ids().size();
    r.play(0.8, moving);
    item = held_rest_item(r);
    CHECK(item && item->has_montage && item->animation_counter == jump_counter && !item->branch_predicted, (what + ": 0.8 s later the montage is not over yet: no clear").c_str());
    int ticks = 0;
    while (ticks < 8 && held_rest_item(r) && held_rest_item(r)->has_montage) {
        r.tick(moving);
        ++ticks;
    }
    item = held_rest_item(r);
    CHECK(item && ticks >= 1 && ticks <= 4, (what + ": the clear comes 1.0 to 1.2 s after the jump, when the montage is over (1.069 s)").c_str());
    CHECK(item && !item->has_montage && item->montage == 0 && !item->branch_predicted && item->animation_counter == jump_counter + 1 && item->may_jump && item->montage_position == 5.82606792f,
          (what + ": the item stays listed with no montage, bBranchPredictedOnAutonomous 0 and the next counter").c_str());
    r.play(4.0, moving);
    item = held_rest_item(r);
    CHECK(item && !item->has_montage && item->animation_counter == jump_counter + 1 && items_of(r, kRest) == 1, (what + ": the clear comes once: nothing changes the item again").c_str());
    CHECK(r.m.end_ids().size() == ends && !r.h.s->abilities_failed() && r.m.unreadable == 0, (what + ": (nothing more is told to the client, nothing failed)").c_str());
}

static void test_a_rest_stands_up_when_the_player_walks_away() {
    std::printf("Rest: the player walks away: the item jumps as the server's branch, then its montage is cleared\n");
    Run r;
    r.play(0.5, false);
    const int32_t id = r.trigger(kRest);
    const Mirror::ActiveAbility* item = held_rest_item(r);
    CHECK(item && item->branch_predicted && item->has_montage && item->animation_counter == 0, "the client's own Rest starts with bBranchPredictedOnAutonomous 1, as before");
    r.play(2.2, false);
    CHECK(r.rt().resting() && r.m.has_effect(kRestEffect), "(the Rest runs with its effect)");
    r.tick(true);   // the first step
    CHECK(!r.rt().resting() && r.m.end_ids().empty(), "the Rest ends on the server, and the client is told nothing");
    check_rest_stands_up_on_the_wire(r, true, 1, "walking away");
    CHECK(!r.m.has_effect(kRestEffect) && r.m.stats[kHealthRegen] == 0.6f && id == 0, "(the Rest effect went a moment after the jump)");
}

AOC_REGISTER_TEST(test_a_rest_stands_up_when_the_player_walks_away);

static void test_a_rest_stands_up_on_the_clients_cancel() {
    std::printf("Rest: the client's cancel (a jump): the item jumps as the server's branch, then its montage is cleared\n");
    Run r;
    r.play(0.5, false);
    const int32_t id = r.trigger(kRest);
    r.play(2.2, false);
    r.cancel(id);
    check_rest_stands_up_on_the_wire(r, false, 1, "the client's cancel");
    CHECK(r.m.end_ids().empty(), "(no ClientEndAbility: the client ended its own)");
}

AOC_REGISTER_TEST(test_a_rest_stands_up_on_the_clients_cancel);

static void test_a_rest_stands_up_after_a_second_press() {
    std::printf("Rest: a second press (a new Rest, its cancel 0.2 s later): the new item jumps as the server's branch, then its montage is cleared\n");
    Run r;
    r.play(0.5, false);
    r.trigger(kRest);
    r.play(2.5, false);
    CHECK(r.m.has_effect(kRestEffect), "(the first Rest runs with its effect)");
    const int32_t second = r.trigger(kRest);
    const Mirror::ActiveAbility* item = held_rest_item(r);
    CHECK(items_of(r, kRest) == 1 && item && item->branch_predicted && item->has_montage && item->animation_counter == 2, "the second press replaces the item with the new Rest's: bBranchPredictedOnAutonomous 1, counter 2");
    r.play(0.2, false);
    r.cancel(second);
    check_rest_stands_up_on_the_wire(r, false, 3, "a second press");
    CHECK(r.m.end_ids().empty() && !r.m.has_effect(kRestEffect), "(the client is told nothing, the effect is gone)");
}

AOC_REGISTER_TEST(test_a_rest_stands_up_after_a_second_press);

static void test_a_rest_ended_by_serverendability_stands_up() {
    std::printf("Rest: ServerEndAbility: the item jumps as the server's branch with ClientEndAbility once, then its montage is cleared\n");
    Run r;
    r.next_id = 42;
    r.play(0.5, false);
    const int32_t id = r.trigger(kRest);
    r.play(2.2, false);
    const auto server = r.m.server_of.find(id);
    CHECK(server != r.m.server_of.end() && r.m.end_ids().empty(), "(the Rest's item names its ServerInstanceId; the client was told nothing)");
    if (server == r.m.server_of.end()) return;
    end_instance(r, server->second);
    r.play(0.1, false);
    CHECK(r.m.end_ids() == std::vector<int32_t>{id} && id == 42, "ClientEndAbility with the client's id 42, once");
    const size_t ends_before = r.m.end_ids().size();
    const Mirror::ActiveAbility* item = held_rest_item(r);
    CHECK(item && item->may_jump && item->animation_counter == 1 && !item->branch_predicted, "(the item jumped in that step)");
    // the check plays on from the jump: its first read is the jump as the client holds it
    check_rest_stands_up_on_the_wire(r, false, 1, "ServerEndAbility");
    CHECK(r.m.end_ids().size() == ends_before, "(the clear tells the client nothing)");
    end_instance(r, server->second);
    r.play(0.3, false);
    CHECK(r.m.end_ids() == std::vector<int32_t>{id}, "a second ServerEndAbility for the instance tells the client nothing more");
}

AOC_REGISTER_TEST(test_a_rest_ended_by_serverendability_stands_up);

static void test_a_new_rest_in_the_time_keeps_its_item() {
    std::printf("Rest: a new Rest started before the montage is over keeps its own item: the late clear never touches it\n");
    Run r;
    r.play(0.5, false);
    const int32_t first = r.trigger(kRest);
    r.play(2.2, false);
    r.cancel(first);
    r.play(0.5, false);
    CHECK(held_rest_item(r) && held_rest_item(r)->has_montage && held_rest_item(r)->animation_counter == 1, "(0.5 s after the jump the montage is not over)");
    const int32_t second = r.trigger(kRest);
    const Mirror::ActiveAbility* item = held_rest_item(r);
    CHECK(items_of(r, kRest) == 1 && item && item->branch_predicted && item->has_montage && item->animation_counter == 3, "the new Rest replaces the item: bBranchPredictedOnAutonomous 1, counter 3");
    r.play(4.0, false);
    item = held_rest_item(r);
    CHECK(items_of(r, kRest) == 1 && item && item->branch_predicted && item->has_montage && item->animation_counter == 3 && !item->may_jump,
          "4 s later (the first Rest's montage was over long ago) its item is as it was: the late clear never came");
    CHECK(r.rt().resting() && r.m.has_effect(kRestEffect), "(the new Rest runs with its effect)");
    r.cancel(second);
    check_rest_stands_up_on_the_wire(r, false, 4, "the new Rest");
}

AOC_REGISTER_TEST(test_a_new_rest_in_the_time_keeps_its_item);

static void test_the_rest_of_a_sit_is_left_as_it_was() {
    std::printf("Rest: the Rest /sit starts is the server's already: its end is the jump alone, as before\n");
    Run r;
    r.play(0.5, false);
    say_emote(r, "sit");
    r.play(2.5, false);
    const Mirror::ActiveAbility* item = held_rest_item(r);
    CHECK(item && !item->branch_predicted && item->has_montage && item->animation_counter == 0, "(/sit: the item says the client did not predict it)");
    say_emote(r, "sit");   // a second /sit: the character stands up
    item = held_rest_item(r);
    CHECK(item && item->may_jump && item->montage_position == 5.82606792f && item->animation_counter == 1 && !item->branch_predicted && item->has_montage, "the second /sit jumps the item: bBranchPredictedOnAutonomous 0, counter 1");
    r.play(4.0, false);
    item = held_rest_item(r);
    CHECK(item && item->has_montage && item->animation_counter == 1 && !item->branch_predicted && items_of(r, kRest) == 1, "4 s later the item is the same: no clear is made");
    CHECK(r.m.end_ids().empty() && !r.h.s->abilities_failed() && r.m.unreadable == 0, "(nothing is told to the client, nothing failed)");
}

AOC_REGISTER_TEST(test_the_rest_of_a_sit_is_left_as_it_was);
