// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  tests/friend_refriend_tests.inc
//
//  Finding 5 of the friends-list review: asking a removed friend again.  After a one-way removal the other side keeps an entry with still-friend
//  false; it must not count as a friend already, and the accept must put it back rather than add a second one.
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================

static void test_a_removed_friend_can_be_asked_and_accepted_again() {
    std::printf("after a one-way removal the two can be friends again: the still-friend-false entry does not block the request or the accept\n");
    FriendWorld w;
    svc::Friend entry_for_b = friend_entry(0);
    entry_for_b.guid = kFriendB;
    entry_for_b.name = "Brannoc";
    entry_for_b.level = 1;
    entry_for_b.class_id = 7;
    entry_for_b.guild_name.clear();
    svc::Friend entry_for_a = entry_for_b;
    entry_for_a.guid = kFriendA;
    entry_for_a.name = "Astra";
    entry_for_a.class_id = 2;
    svc::Character a = friend_character("Astra", kFriendA, 2, "Bard");
    a.friends = {entry_for_b};
    svc::Character b = friend_character("Brannoc", kFriendB, 7, "Tank");
    b.friends = {entry_for_a};
    if (!w.enter(0, a) || !w.enter(1, b)) {
        CHECK(false, "both characters join");
        return;
    }
    w.request(1, friend_remove_call(kFriendA));
    CHECK(w.p[0].s->friends().size() == 1 && !w.p[0].s->friends()[0].still_friend, "(the removal left the other side's entry with still friend false)");
    const size_t b_before = w.p[1].got.size();
    w.request(0, friend_send_call("Brannoc"));
    CHECK(w.p[1].s->friend_requests().size() == 1 && w.p[1].s->friend_requests()[0].sender_guid == kFriendA,
          "the removed side's new request is sent, not refused");
    const auto pending_blocks = friend_blocks(w, 1, b_before);
    CHECK(pending_blocks.size() == 1 && friend_one_change(pending_blocks[0], "PendingFriendRequests", 1, 0), "(the target is shown one new pending request)");
    const size_t a_before = w.p[0].got.size();
    w.request(1, friend_name_call("ServerAcceptFriendRequest", "Astra"));
    CHECK(w.p[0].s->friends().size() == 1 && w.p[0].s->friends()[0].still_friend, "the accept restores the other side's entry as a friend");
    CHECK(w.p[1].s->friends().size() == 1 && w.p[1].s->friends()[0].still_friend, "and the accepting side's entry too");
    CHECK(w.p[1].s->friend_requests().empty(), "the pending request is cleared");
    const auto a_blocks = friend_blocks(w, 0, a_before);
    CHECK(a_blocks.size() == 1 && friend_one_change(a_blocks[0], "FriendsList", 1, 0) && friend_field(a_blocks[0], "PendingFriendRequests") == nullptr,
          "the restored entry is one changed item, not a second entry");
}

AOC_REGISTER_TEST(test_a_removed_friend_can_be_asked_and_accepted_again);
