// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  tests/session_party_stats_more_tests.inc: the party's live stats, the cases that follow the first ones (Social, seeing other players: live Health and Mana)
//
//  Registered tests of test_worldd_session, after tests/session_party_stats_tests.inc (its helpers).  A member who dies and comes back, a level, a place, a Health that changes
//  faster than the cadence allows, and the frame that follows a join: what a party's clients are sent, read from the wire.
//
//  Modelled: no recording decodes either payload, so the shapes are the game's own schema's, and the alive status and the percentages are modelled (0 alive, 1 dead; the current
//  value as the share of the maximum).
// ============================================================================

// ---- a member who dies and comes back ----------------------------------------------------------------------------------------------------

static void test_a_dead_member_is_shown_dead_with_empty_bars_and_alive_again_after_the_revive() {
    std::printf("a member who dies is shown dead with empty bars, in the frame and in the live item, and alive again after the revive\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 2), "two players enter the world");
    if (!w[0].s || !w[1].s) return;
    CHECK(party_pair(w, 0, 1, pc_cls), "p1 and p2 form a party");
    party_run(w, 1200);
    const float max_h = party_stat(w, 1, content::kGuidMaxHealth), max_m = party_stat(w, 1, content::kGuidMaxMana);
    const size_t before = w[0].got.size();
    CHECK(party_hurt(w, 1, 0.0f), "(p2's Health falls to 0)");
    party_run(w, 1300);
    CHECK(w[1].s->dead(), "(p2 is dead)");
    const auto frames = party_frames_in(w[0].got, pc_cls, before);
    CHECK(!frames.empty() && frames.back().members.size() == 2, "p1 is sent a frame about the death within a second and a bit (a change of the alive state does not wait for the refresh)");
    if (frames.empty() || frames.back().members.size() != 2) return;
    const PartyMemberFrame& dead_row = frames.back().members[1];
    CHECK(dead_row.alive_status == 1 && dead_row.health_percent == 0.0f && dead_row.mana_percent == 0.0f && dead_row.max_health == max_h && dead_row.max_mana == max_m,
          "p2's row says dead, with empty bars and its maxima kept");
    CHECK(frames.back().members[0].alive_status == 0 && frames.back().members[0].health_percent > 0.0f, "...and p1's row still says alive");
    const PartyStatsUpdate dead_update = last_party_stats(w, 0, before);
    const PartyStatsItem* dead_item = item_of(dead_update, w.guid_of(1));
    CHECK(dead_item && dead_item->alive == 1 && dead_item->health_percent == 0.0f && dead_item->mana_percent == 0.0f && dead_item->max_health == max_h,
          "the live item of p2 says dead with empty bars too");
    // nothing regenerates while dead: no further update of the item
    const size_t quiet_from = w[0].got.size();
    party_run(w, 3000);
    const auto quiet = party_stats_updates(w, 0, quiet_from);
    bool dead_item_moved = false;
    for (const PartyStatsUpdate& u : quiet) dead_item_moved = dead_item_moved || changed_item_of(u, w.guid_of(1)) != nullptr;
    CHECK(!dead_item_moved, "a dead member's item does not change while he stays dead");
    // the revive: alive again at full Health and Mana
    const size_t before_revive = w[0].got.size();
    const PlayerSession::GmReviveResult revived = w[1].s->gm_revive(w[1].conn, w.t, "the test");
    CHECK(revived.ok && !w[1].s->dead(), "(p2 is revived)");
    party_run(w, 1300);
    const auto frames2 = party_frames_in(w[0].got, pc_cls, before_revive);
    CHECK(!frames2.empty() && frames2.back().members.size() == 2, "p1 is sent a frame about the revive");
    if (!frames2.empty() && frames2.back().members.size() == 2) {
        const PartyMemberFrame& row = frames2.back().members[1];
        CHECK(row.alive_status == 0 && party_near(row.health_percent, 1.0f) && party_near(row.mana_percent, 1.0f), "p2's row says alive with full bars");
    }
    const PartyStatsItem* back = item_of(last_party_stats(w, 0, before_revive), w.guid_of(1));
    CHECK(back && back->alive == 0 && party_near(back->health_percent, 1.0f) && party_near(back->mana_percent, 1.0f), "...and so does his live item");
}
AOC_REGISTER_TEST(test_a_dead_member_is_shown_dead_with_empty_bars_and_alive_again_after_the_revive);

// ---- a level --------------------------------------------------------------------------------------------------------------------------------

static void test_a_members_level_and_new_maxima_reach_the_frame_at_once_and_the_live_item() {
    std::printf("a member's level-up and his new maxima reach the party frame without waiting for the refresh, and the live item\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 2), "two players enter the world");
    if (!w[0].s || !w[1].s) return;
    CHECK(party_pair(w, 0, 1, pc_cls), "p1 and p2 form a party");
    party_run(w, 1200);
    const float max_h0 = party_stat(w, 1, content::kGuidMaxHealth);
    const size_t before = w[0].got.size();
    const PlayerSession::GmXpResult r = w[1].s->gm_give_experience(w[1].conn, 350, w.t, "the test");
    CHECK(r.ok && r.leveled && r.level_after == 2, "(350 XP crosses level 2)");
    party_run(w, 1300);
    const float max_h1 = party_stat(w, 1, content::kGuidMaxHealth);
    CHECK(max_h1 > max_h0, "(the level raised the maximum Health)");
    const auto frames = party_frames_in(w[0].got, pc_cls, before);
    CHECK(!frames.empty() && frames.back().members.size() == 2, "p1 is sent a frame within a second and a bit of the level-up");
    if (!frames.empty() && frames.back().members.size() == 2) {
        const PartyMemberFrame& row = frames.back().members[1];
        CHECK(row.level == 2 && row.max_health == max_h1, "...its row has the new level and the new maximum");
    }
    const PartyStatsItem* item = item_of(last_party_stats(w, 0, before), w.guid_of(1));
    CHECK(item && item->level == 2 && item->max_health == max_h1, "the live item has them too");
}
AOC_REGISTER_TEST(test_a_members_level_and_new_maxima_reach_the_frame_at_once_and_the_live_item);

// ---- a place --------------------------------------------------------------------------------------------------------------------------------

static void test_a_members_place_follows_him_in_the_live_item_and_the_frame() {
    std::printf("a member who moves has his new place in the live item within a second and in the frame with the refresh\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 2), "two players enter the world");
    if (!w[0].s || !w[1].s) return;
    CHECK(party_pair(w, 0, 1, pc_cls), "p1 and p2 form a party");
    party_run(w, 5000);   // (the first refresh is past)
    const PartyStatsExpected before_place = party_stats_expected(*w[1].s);
    const GoodPosition at = w[1].s->movement().good();
    const size_t before = w[0].got.size();
    const PlayerSession::GmMoveResult moved = w[1].s->gm_teleport(w[1].conn, at.x + 5000.0, at.y, at.z, w.t, "the test");
    CHECK(moved.status == PlayerSession::GmMoveResult::Status::Moved, "(p2 is moved 50 m)");
    party_run(w, 1300);
    const PartyStatsExpected now_place = party_stats_expected(*w[1].s);
    CHECK(!(now_place.location == before_place.location), "(p2's place changed)");
    const PartyStatsItem* item = item_of(last_party_stats(w, 0, before), w.guid_of(1));
    CHECK(item && item->location == now_place.location, "the live item carries the new place within a second and a bit");
    party_run(w, 4600);
    const auto frames = party_frames_in(w[0].got, pc_cls, before);
    CHECK(!frames.empty() && frames.back().members.size() == 2 && frames.back().members[1].location == now_place.location, "the frame carries it with the refresh, within 4.5 s");
}
AOC_REGISTER_TEST(test_a_members_place_follows_him_in_the_live_item_and_the_frame);

// ---- a Health that changes faster than the cadence ----------------------------------------------------------------------------------------

static void test_a_health_that_changes_every_tenth_of_a_second_is_sent_at_the_cadence() {
    std::printf("a member's Health that changes ten times a second is sent about once a second in the live item, and the frame is not repeated\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 2), "two players enter the world");
    if (!w[0].s || !w[1].s) return;
    CHECK(party_pair(w, 0, 1, pc_cls), "p1 and p2 form a party");
    party_run(w, 1200);
    const float max_h = party_stat(w, 1, content::kGuidMaxHealth);
    const size_t from = w[0].got.size();
    for (int i = 0; i < 30; ++i) {
        CHECK(party_hurt(w, 1, max_h - (i % 2 ? 10.0f : 60.0f)), "(p2's Health changes)");
        party_run(w, 100);
    }
    const auto updates = party_stats_updates(w, 0, from);
    CHECK(updates.size() >= 2 && updates.size() <= 4, ("thirty changes in three seconds are sent as two to four live-stat updates, not thirty: " + std::to_string(updates.size())).c_str());
    const auto frames = party_frames_in(w[0].got, pc_cls, from);
    CHECK(frames.size() <= 1, ("...and at most one party frame (the roster's refresh), not one for each change: " + std::to_string(frames.size())).c_str());
}
AOC_REGISTER_TEST(test_a_health_that_changes_every_tenth_of_a_second_is_sent_at_the_cadence);

// ---- the frame after a join ----------------------------------------------------------------------------------------------------------------

static void test_the_frame_of_a_join_is_not_followed_at_once_by_a_refresh() {
    std::printf("the frame a join sends is not followed within a second by a refresh frame for a Health that regenerates\n");
    PartyWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(party_enter(w, 3), "three players enter the world");
    if (!w[0].s || !w[2].s) return;
    CHECK(party_pair(w, 0, 1, pc_cls), "p1 and p2 form a party");
    party_run(w, 1200);
    const size_t before_request = w[2].got.size();
    CHECK(party_invite(w, 0, "p3"), "p1 invites p3");
    w.settle();
    const auto requests = party_requests_in(w[2].got, pc_cls, before_request);
    CHECK(requests.size() == 1, "p3's client gets the invitation");
    if (requests.size() != 1) return;
    // p2 is hurt, so its Health regenerates a second after the join
    const float max_h = party_stat(w, 1, content::kGuidMaxHealth);
    CHECK(party_hurt(w, 1, max_h - 40.0f), "(p2 is hurt)");
    const size_t before = w[0].got.size();
    CHECK(party_accept(w, 2, requests[0].group_guid, requests[0].inviter), "p3 accepts");
    party_run(w, 1500);
    const auto frames = party_frames_in(w[0].got, pc_cls, before);
    CHECK(frames.size() == 1 && frames.back().members.size() == 3,
          ("p1 is sent the one frame of the join: the regeneration of p2's Health waits for the roster's refresh (" + std::to_string(frames.size()) + " frames)").c_str());
}
AOC_REGISTER_TEST(test_the_frame_of_a_join_is_not_followed_at_once_by_a_refresh);
