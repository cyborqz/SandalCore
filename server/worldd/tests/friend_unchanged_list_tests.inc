// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  tests/friend_unchanged_list_tests.inc
//
//  Finding 7 of the friends-list review: the untouched-list test never reached friends_update again.  The change itself writes nothing when the
//  lists are as the connection has them, and the test now asks the component directly instead of relying on a tick that never builds it.
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================

static void test_an_unchanged_list_has_no_change_to_write() {
    std::printf("the SocialComponent's change is empty when the lists are as the connection has them\n");
    FriendWorld w;
    svc::Friend friend_b;
    friend_b.guid = kFriendB;
    friend_b.name = "Brannoc";
    friend_b.level = 1;
    friend_b.class_id = 7;
    svc::Character a = friend_character("Astra", kFriendA, 2, "Bard");
    a.friends = {friend_b};
    if (!w.enter(0, a)) {
        CHECK(false, "the character joins");
        return;
    }
    bootstrap::PlayerBootstrap& pb = const_cast<bootstrap::PlayerBootstrap&>(w.p[0].s->bootstrap());
    CHECK(!pb.friends_update().has_value(), "friends_update() after the join writes nothing: the shadow still matches");
    const size_t before = w.p[0].got.size();
    for (int i = 0; i < 25; ++i) w.step();
    CHECK(friend_blocks(w, 0, before).empty(), "25 ticks of an untouched list send no SocialComponent change");
}

AOC_REGISTER_TEST(test_an_unchanged_list_has_no_change_to_write);
