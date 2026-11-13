// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  tests/friend_store_race_tests.inc
//
//  Finding 1 of the friends-list review: two changes to one offline character.  The other side's record is read, changed and saved whole; the
//  second read must see the first save, so that both changes are kept.
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================
#include "friend_fixes_rig.inc"

static void test_two_requests_for_one_offline_character_are_both_kept() {
    std::printf("two requests for one offline character both reach its list, though the first save is still being written\n");
    FriendStoreApi api;
    const svc::Character a = friend_character("Astra", kFriendA, 2, "Bard");
    const svc::Character b = friend_character("Brannoc", kFriendB, 7, "Tank");
    const svc::Character target = friend_character("Target", kFriendC, 3, "Cleric");
    api.characters[kFriendA] = a;
    api.characters[kFriendB] = b;
    api.characters[kFriendC] = target;
    FriendStoreApi::Gate gate;
    api.gate = &gate;
    api.gate_id = kFriendC;
    api.gate_progress = true;   // (the first request's save stays in flight)
    CharacterSaver::Config config;
    config.max_attempts = 1;
    config.destructor_wait = std::chrono::milliseconds(500);
    CharacterSaver saver(api, config);
    {
        FriendWorld w;
        w.fx.world->set_position_sink(&saver);
        if (!w.enter(0, a) || !w.enter(1, b)) {
            CHECK(false, "both senders join");
            gate.release();
            return;
        }
        w.request(0, friend_send_call("Target"));
        if (!gate.wait_entered()) {
            CHECK(false, "(the first request's save reaches the character service)");
            gate.release();
            return;
        }
        // the second request is sent while the first save is still being written: its read must wait for that save
        std::thread second([&] { w.request(1, friend_send_call("Target")); });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        gate.release();
        second.join();
        w.settle();
        CHECK(saver.flush(std::chrono::seconds(5)), "(the saver wrote both requests)");
        std::lock_guard<std::mutex> lock(api.mu);
        const svc::Character& kept = api.characters[kFriendC];
        CHECK(kept.friend_requests.size() == 2, "the target keeps both requests");
        if (kept.friend_requests.size() == 2) {
            const bool first = kept.friend_requests[0].sender_guid == kFriendA && kept.friend_requests[0].sender_name == "Astra";
            const bool second = kept.friend_requests[1].sender_guid == kFriendB && kept.friend_requests[1].sender_name == "Brannoc";
            CHECK(first && second, "both are on its list, in the order they were sent");
        }
    }
}

AOC_REGISTER_TEST(test_two_requests_for_one_offline_character_are_both_kept);
