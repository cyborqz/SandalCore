// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  tests/friend_name_check_tests.inc
//
//  Finding 6 of the friends-list review: the name check waits for the character service on the network thread.  A slow service must not hold the
//  session: the call returns at once and the answer arrives from the lookup worker on a later tick.
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================
#include <optional>
#include <thread>

static void test_the_name_check_does_not_wait_for_the_service() {
    std::printf("a name check to a slow character service returns to the session at once and is answered from a worker\n");
    FriendWorld w;
    w.service.lookup_delay = std::chrono::milliseconds(250);
    if (!w.enter(0, friend_character("Astra", kFriendA, 2, "Bard"))) {
        CHECK(false, "the character joins");
        return;
    }
    const schema::ClassSchema& pc = FriendWorld::pc_schema();
    const size_t before = w.p[0].got.size();
    const auto start = std::chrono::steady_clock::now();
    w.client_sends(w.p[0], {w.p[0].client.actor(w.p[0].s->channel_of(bs::Role::PlayerController), friend_check_call("Astra"), true)});
    const double dispatch_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("    the call returned to the session after %.1f ms (the service takes 250 ms)\n", dispatch_ms);
    CHECK(dispatch_ms < 100.0, "the call does not wait for the 250 ms lookup");
    std::optional<FriendCheck> answer;
    for (int i = 0; i < 1000 && !answer; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        w.step();
        for (size_t k = before; k < w.p[0].got.size() && !answer; ++k) {
            if (auto a = friend_check_of(w.p[0].got[k], pc)) answer = *a;
        }
    }
    CHECK(answer && answer->name == "Astra" && answer->exists, "the answer arrives on a later tick, from the lookup worker");
}

AOC_REGISTER_TEST(test_the_name_check_does_not_wait_for_the_service);
