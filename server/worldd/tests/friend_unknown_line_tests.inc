// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  tests/friend_unknown_line_tests.inc
//
//  Finding 10 of the friends-list review: the unknown-name system line when the reply queue is full.  Every unknown name gets its one line, not
//  only the ones that find room under the queue's limit.
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================

static void test_every_unknown_name_gets_its_line_when_the_reply_queue_is_full() {
    std::printf("330 unknown-name requests in one datagram: every one is answered with its line, though the reply queue is full past 64 steps\n");
    FriendWorld w;
    if (!w.enter(0, friend_character("Astra", kFriendA, 2, "Bard"))) {
        CHECK(false, "the sender joins");
        return;
    }
    const schema::ClassSchema& pc = FriendWorld::pc_schema();
    const size_t before = w.p[0].got.size();
    std::vector<fake::Bunch> bunches;
    bunches.reserve(330);
    const uint32_t channel = w.p[0].s->channel_of(bs::Role::PlayerController);
    for (int i = 0; i < 330; ++i) bunches.push_back(w.p[0].client.actor(channel, friend_send_call("Nobody_xyz"), true));
    // (330 bunches need several packets: the client splits them at the packet size, as a real client's flood does)
    for (const auto& packet : w.p[0].client.packets(std::move(bunches))) {
        const std::vector<InBunch> in = w.p[0].conn.receive(packet, w.t);
        w.p[0].s->take_acks(w.p[0].conn);
        for (const InBunch& b : in) w.p[0].s->on_bunch(w.p[0].conn, b, w.t);
        w.p[0].s->end_of_datagram(w.p[0].conn);
    }
    w.settle();
    for (int i = 0; i < 50; ++i) w.step();   // (the queue behind the window needs a few rounds of acknowledgements)
    size_t lines = 0;
    for (size_t i = before; i < w.p[0].got.size(); ++i) {
        if (auto line = friend_chat_of(w.p[0].got[i], pc)) {
            if (line->message == "Nobody_xyz does not exist." && line->from == "Server" && line->category == 124) ++lines;
        }
    }
    std::printf("    %zu lines, %zu lookups, %zu chains delivered\n", lines, w.service.looked_up.size(), w.p[0].got.size() - before);
    CHECK(lines == 330, "every one of the 330 requests got its one system line");
    CHECK(w.p[0].s->friend_requests().empty(), "and none made a pending entry");
}

AOC_REGISTER_TEST(test_every_unknown_name_gets_its_line_when_the_reply_queue_is_full);
