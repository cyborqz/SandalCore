// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  tests/friend_wire_edges_tests.inc
//
//  Finding 12 of the friends-list review: the wire and call edges that had no test.  The first block with 100 friends and 100 pending requests, a
//  name outside ASCII, and an accept and a removal whose other side is offline.
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================

static void test_the_first_block_holds_100_friends_and_100_requests() {
    std::printf("the first block holds 100 friends and 100 pending requests, whole and in order\n");
    FriendWorld w;
    svc::Character a = friend_character("Astra", kFriendA, 2, "Bard");
    for (int i = 0; i < 100; ++i) a.friends.push_back(friend_entry(i));
    for (int i = 0; i < 100; ++i) a.friend_requests.push_back(friend_request(i, kFriendA));
    if (!w.enter(0, a)) {
        CHECK(false, "the character with 100 friends and 100 requests joins");
        return;
    }
    const auto blocks = friend_blocks(w, 0, 0);
    CHECK(blocks.size() == 1, "one SocialComponent block holds the first state");
    if (blocks.size() != 1) return;
    const repl::FastArrayField* friends = friend_field(blocks[0], "FriendsList");
    const repl::FastArrayField* pending = friend_field(blocks[0], "PendingFriendRequests");
    CHECK(friends && friends->delta.base_key == 0 && friends->delta.changed.size() == 100, "100 friends, base key 0");
    CHECK(pending && pending->delta.base_key == 0 && pending->delta.changed.size() == 100, "100 pending requests, base key 0");
    if (!friends || !pending || friends->delta.changed.size() != 100 || pending->delta.changed.size() != 100) return;
    bool order = true, fields = true;
    for (size_t i = 0; i < 100; ++i) {
        FriendItem f, q;
        const bool read = friend_item_of(friends->delta.changed[i].values, false, f) && friend_item_of(pending->delta.changed[i].values, true, q);
        fields = fields && read && f.guid == friend_words(a.friends[i].guid) && f.name == a.friends[i].name && f.level == a.friends[i].level &&
                 f.class_id == static_cast<uint64_t>(a.friends[i].class_id) && f.last_online == 0 && !f.online && f.still_friend;
        fields = fields && q.sender_guid == friend_words(a.friend_requests[i].sender_guid) && q.sender_name == a.friend_requests[i].sender_name &&
                 q.recipient_guid == friend_words(kFriendA) && q.expires == kFriendLiveExpiryMs / 1000;
        order = order && read;
    }
    CHECK(order && fields, "every one of the 200 items reads back in the list's order, field by field");
    size_t chain_parts = 0;
    for (const fake::Delivered& d : w.p[0].got) {
        if (!friend_block_of(d, *friend_social_of(*w.p[0].s))) continue;
        chain_parts = d.exports.size() + (d.content.bit_pos() <= Connection::kMaxSingleBunchBits ? 1 : (d.content.bit_pos() + 7615) / 7616);
    }
    CHECK(chain_parts > 0 && chain_parts <= Connection::kMaxUnackedReliable, "the 200-item block goes out in one chain of at most 256 reliable parts");
    std::printf("    the first block: %zu payload bits, %zu chain parts\n", blocks[0].payload_bits, chain_parts);
}

AOC_REGISTER_TEST(test_the_first_block_holds_100_friends_and_100_requests);

static void test_a_name_outside_ascii_reads_back_on_the_wire() {
    std::printf("a friend's name outside ASCII goes on the wire and reads back whole\n");
    FriendWorld w;
    svc::Character a = friend_character("Astra", kFriendA, 2, "Bard");
    a.friends = {friend_entry(1)};
    a.friends[0].name = "Zo\xc3\xab";   // (UTF-8: Z o e with a diaeresis)
    if (!w.enter(0, a)) {
        CHECK(false, "the character joins");
        return;
    }
    const auto blocks = friend_blocks(w, 0, 0);
    const repl::FastArrayField* friends = blocks.size() == 1 ? friend_field(blocks[0], "FriendsList") : nullptr;
    FriendItem item;
    const bool read = friends && friends->delta.changed.size() == 1 && friend_item_of(friends->delta.changed[0].values, false, item);
    CHECK(read && item.name == "Zo\xc3\xab", "the non-ASCII name is the one the store has, field by field");
}

AOC_REGISTER_TEST(test_a_name_outside_ascii_reads_back_on_the_wire);

static void test_an_accept_and_a_removal_whose_other_side_is_offline_are_stored() {
    std::printf("the other side of an accept and of a removal is written when it has no session here\n");
    FriendWorld w;
    svc::FriendRequest from_b = friend_request(0, kFriendA);
    from_b.sender_guid = kFriendB;
    from_b.sender_name = "Brannoc";
    svc::Character a = friend_character("Astra", kFriendA, 2, "Bard");
    a.friend_requests = {from_b};
    svc::Character b = friend_character("Brannoc", kFriendB, 7, "Tank");
    w.service.characters[kFriendB] = b;
    if (!w.enter(0, a)) {
        CHECK(false, "the accepter joins");
        return;
    }
    w.request(0, friend_name_call("ServerAcceptFriendRequest", "Brannoc"));
    CHECK(w.p[0].s->friends().size() == 1 && w.p[0].s->friends()[0].guid == kFriendB, "the accepter lists the offline sender");
    {
        const auto& stored = w.service.characters[kFriendB];
        CHECK(stored.friends.size() == 1 && stored.friends[0].guid == kFriendA && stored.friends[0].still_friend,
              "the offline sender's stored record lists the accepter");
    }
    // both list each other; the remover has no session for the other side
    svc::Friend entry_for_a = friend_entry(0);
    entry_for_a.guid = kFriendA;
    entry_for_a.name = "Astra";
    entry_for_a.level = 1;
    entry_for_a.class_id = 2;
    entry_for_a.guild_name.clear();
    svc::Friend entry_for_b = entry_for_a;
    entry_for_b.guid = kFriendB;
    entry_for_b.name = "Brannoc";
    entry_for_b.class_id = 7;
    a.friends = {entry_for_b};
    b.friends = {entry_for_a};
    w.service.characters[kFriendA] = a;
    w.service.characters[kFriendB] = b;
    w.p[0].s->finish("the accept test ends");
    w.p[0].s.reset();
    if (!w.enter(0, a)) {
        CHECK(false, "the remover joins");
        return;
    }
    w.request(0, friend_remove_call(kFriendB));
    CHECK(w.p[0].s->friends().empty(), "the remover's entry is gone");
    const auto& stored = w.service.characters[kFriendB];
    CHECK(stored.friends.size() == 1 && stored.friends[0].guid == kFriendA && !stored.friends[0].still_friend,
          "the offline other side keeps its entry with still friend false");
}

AOC_REGISTER_TEST(test_an_accept_and_a_removal_whose_other_side_is_offline_are_stored);
