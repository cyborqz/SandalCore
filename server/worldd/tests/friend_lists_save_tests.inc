// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  tests/friend_lists_save_tests.inc
//
//  Finding 3 of the friends-list review: a friend-lists save that lands while a full progress is queued.  The saver must keep the progress and
//  merge the lists into it, so the level, the experience and the ignored names are not dropped.
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================
#include "friend_fixes_rig.inc"

static void test_a_lists_only_save_keeps_a_queued_progress() {
    std::printf("a friend-lists save that lands while a full progress is queued is merged into it: the level and the rest are not dropped\n");
    FriendStoreApi api;
    svc::Character cyb = cyb_record();
    cyb.level = 1;
    api.characters[kCyb] = cyb;
    FriendStoreApi::Gate gate;
    api.gate = &gate;
    api.gate_id = "ffffffffffffffffffffffffffffffff";
    api.gate_progress = false;   // (the worker is held on another character's position)
    CharacterSaver::Config config;
    config.max_attempts = 1;
    config.destructor_wait = std::chrono::milliseconds(500);
    CharacterSaver saver(api, config);
    saver.save(api.gate_id, svc::Position{});
    if (!gate.wait_entered()) {
        CHECK(false, "(the worker is held)");
        gate.release();
        return;
    }
    svc::Progress full;
    full.level = 17;
    full.class_id = 2;
    full.experience = {{108651085518458, 12345, 7}};
    full.replace_ignored_names = true;
    full.ignored_character_names = {"Nobody_xyz"};
    saver.save_progress(kCyb, full);
    saver.save_friend_lists(kCyb, {friend_entry(1)}, {friend_request(2, kCyb)});
    gate.release();
    CHECK(saver.flush(std::chrono::seconds(5)), "(the saver wrote the character)");
    std::lock_guard<std::mutex> lock(api.mu);
    const svc::Character& kept = api.characters[kCyb];
    CHECK(kept.level == 17, "the queued progress's level is written");
    CHECK(kept.experience.size() == 1 && kept.experience[0].xp == 12345 && kept.experience[0].debt == 7, "and its experience");
    CHECK(kept.ignored_character_names == std::vector<std::string>{"Nobody_xyz"}, "and the names it ignored");
    CHECK(kept.friends.size() == 1 && kept.friends[0].guid == friend_entry(1).guid, "and the friend the lists save carried");
    CHECK(kept.friend_requests.size() == 1 && kept.friend_requests[0].sender_guid == friend_request(2, kCyb).sender_guid, "and the pending request");
}

AOC_REGISTER_TEST(test_a_lists_only_save_keeps_a_queued_progress);
