// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/session_chat_fixes_tests.inc
//
//  Registered tests of test_worldd_session, after session_chat_tests.inc (whose chat world and helpers they use): a /say's log counts only the
//  players its line was queued for; a /tell's name is read without the spaces around it and a name of spaces alone is no name; a /tell asks for
//  the room it needs in the sender's queue, one step when the named player is there and two when the "not online" line follows.
// ============================================================================

namespace chat_fixes {

/// Fills the reliable window of player `i`'s PlayerController channel with bunches its client is never given: every reply step that is queued for
/// him from now on waits in his session (as when a client stops acknowledging).
static void clog_player(ChatWorld& w, size_t i) {
    conn::BunchHeader header;
    header.channel = w[i].s->channel_of(bs::Role::PlayerController);
    header.reliable = true;
    header.name_index = Connection::kActorChannelName;
    BunchWriter filler;
    filler.write_bits(0xA5, 8);
    for (size_t n = 0; n < 400; ++n) w[i].conn.send(header, filler);
}

/// 20 ms of the world in which the client of player `deaf` is given nothing (and so acknowledges nothing); the other players step as usual.
static void step_without(ChatWorld& w, size_t deaf) {
    w.t += milliseconds(20);
    for (size_t i = 0; i < w.p.size(); ++i) {
        ChatWorld::Player& pl = w[i];
        if (!pl.s) continue;
        pl.s->tick(pl.conn, w.t);
        if (i == deaf) {
            (void)pl.conn.flush(w.t);   // (sent and lost)
        } else {
            w.flush(pl);
            pl.s->take_acks(pl.conn);
        }
    }
    for (size_t i = 0; i < w.p.size(); ++i) {
        if (w[i].s && i != deaf) w.client_sends(w[i], {});
    }
}

}  // namespace chat_fixes

static void test_say_log_counts_only_the_players_the_line_was_queued_for() {
    std::printf("a /say's log line counts the players its line was queued for, and says how many had no room for it\n");
    ChatWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(w.enter(0, chat_record("cyb", kCyb.c_str(), kChatX, kChatY, kChatZ), identity()) &&
              w.enter(1, chat_record("dee", "11112222333344445555666677778888", kChatX + 500, kChatY, kChatZ), identity()),
          "two players join and enter the world, 5 m apart");
    if (!w[0].s || !w[1].s) return;
    const uint32_t pc_ch = w[0].s->channel_of(bs::Role::PlayerController);
    // dee's client hears nothing any more: the lines said to him wait in his session until it holds as many as it may
    chat_fixes::clog_player(w, 1);
    size_t said = 0;
    for (; said < 80 && w[1].s->chat_lines_dropped() == 0; ++said) {
        w.client_sends(w[0], {w[0].client.actor(pc_ch, chat_call(pc_cls, "line " + std::to_string(said), 9), true)});
        chat_fixes::step_without(w, 1);
    }
    CHECK(said > 1 && w[1].s->chat_lines_dropped() == 1 && w[1].s->chat_lines_received() == said - 1, "dee's session took the lines until its queue was full; the last one found no room");
    const std::string log = g_log.str();
    const size_t at = log.rfind("the say reached ");
    CHECK(at != std::string::npos && log.compare(at, 31, "the say reached 0 other player(") == 0, "the log of that say counts 0 players reached, not the one whose queue was full");
    CHECK(at != std::string::npos && log.find("1 had no room", at) != std::string::npos, "and says that one player had no room for the line");
}

AOC_REGISTER_TEST(test_say_log_counts_only_the_players_the_line_was_queued_for);

static void test_tell_name_is_read_without_the_spaces_around_it() {
    std::printf("a /tell's name is read without the spaces around it, and a name of spaces alone is no name\n");
    ChatWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(w.enter(0, chat_record("cyb", kCyb.c_str(), kChatX, kChatY, kChatZ), identity()) &&
              w.enter(1, chat_record("dee", "11112222333344445555666677778888", kChatX + 500, kChatY, kChatZ), identity()),
          "two players join and enter the world");
    if (!w[0].s || !w[1].s) return;
    const uint32_t pc_ch = w[0].s->channel_of(bs::Role::PlayerController);
    std::string why;
    {
        // spaces before and after the name: the player named is found, and both copies carry the name without them
        const size_t ba = w[0].got.size(), bb = w[1].got.size();
        w.client_sends(w[0], {w[0].client.actor(pc_ch, chat_call(pc_cls, "hi", 45, "  dee \t"), true)});
        w.settle();
        CHECK(one_line(chat_lines_in(w[0].got, pc_cls, ba), "To dee: hi", "", 45, &why), ("the sender's copy names the player without the spaces: " + why).c_str());
        CHECK(one_line(chat_lines_in(w[1].got, pc_cls, bb), "hi", "cyb", 45, &why), ("the player named reads the tell: " + why).c_str());
    }
    {
        // a padded name nobody has: the "not online" line names it without the spaces too
        const size_t ba = w[0].got.size();
        w.client_sends(w[0], {w[0].client.actor(pc_ch, chat_call(pc_cls, "hi", 45, " zed  "), true)});
        w.settle();
        const auto lines = chat_lines_in(w[0].got, pc_cls, ba);
        CHECK(lines.size() == 2 && line_is(lines[0], "To zed: hi", "", 45, &why) && line_is(lines[1], "zed is not online.", "Server", 124, &why),
              "a padded name nobody has: the sender's copy and the not-online line name it without the spaces");
    }
    {
        // spaces alone are no name: refused like an empty one
        const size_t ba = w[0].got.size(), bb = w[1].got.size(), refused = w[0].s->chat_lines_refused(), sent = w[0].s->chat_lines_sent();
        w.client_sends(w[0], {w[0].client.actor(pc_ch, chat_call(pc_cls, "hi", 45, "   "), true)});
        w.settle();
        CHECK(chat_lines_in(w[0].got, pc_cls, ba).empty() && chat_lines_in(w[1].got, pc_cls, bb).empty(), "a name of spaces alone: nobody reads anything");
        CHECK(w[0].s->chat_lines_refused() == refused + 1 && w[0].s->chat_lines_sent() == sent, "it is counted as a refused line, like a tell with no name");
    }
}

AOC_REGISTER_TEST(test_tell_name_is_read_without_the_spaces_around_it);

static void test_tell_asks_for_the_room_it_needs() {
    std::printf("a /tell needs one free step in the sender's queue when the named player is there, two when the not-online line follows\n");
    ChatWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(w.enter(0, chat_record("cyb", kCyb.c_str(), kChatX, kChatY, kChatZ), identity()) &&
              w.enter(1, chat_record("dee", "11112222333344445555666677778888", kChatX + 500, kChatY, kChatZ), identity()),
          "two players join and enter the world");
    if (!w[0].s || !w[1].s) return;
    const uint32_t pc_ch = w[0].s->channel_of(bs::Role::PlayerController);
    // cyb's own client hears nothing any more, so his echoes wait in his session: he says lines until one step is free
    chat_fixes::clog_player(w, 0);
    const size_t limit = PlayerSession::kMaxPendingReplies;
    for (int i = 0; i < 200 && w[0].s->replies_pending() < limit - 1; ++i) {
        w.client_sends(w[0], {w[0].client.actor(pc_ch, chat_call(pc_cls, "filler", 9), true)});
        chat_fixes::step_without(w, 0);
    }
    CHECK(w[0].s->replies_pending() == limit - 1, "(the sender's queue has exactly one free step)");
    if (w[0].s->replies_pending() != limit - 1) return;
    // a tell to a name nobody has needs two steps (the copy and the not-online line): refused
    size_t sent = w[0].s->chat_lines_sent(), refused = w[0].s->chat_lines_refused();
    w.client_sends(w[0], {w[0].client.actor(pc_ch, chat_call(pc_cls, "hi", 45, "zed"), true)});
    chat_fixes::step_without(w, 0);
    CHECK(w[0].s->chat_lines_sent() == sent && w[0].s->chat_lines_refused() == refused + 1 && w[0].s->replies_pending() == limit - 1,
          "a tell to a name nobody has is refused: its two lines do not fit, and none of them is queued");
    // a tell to dee needs one step on the sender's side: taken
    sent = w[0].s->chat_lines_sent();
    refused = w[0].s->chat_lines_refused();
    const size_t dee_got = w[1].s->chat_lines_received();
    w.client_sends(w[0], {w[0].client.actor(pc_ch, chat_call(pc_cls, "hi", 45, "dee"), true)});
    chat_fixes::step_without(w, 0);
    CHECK(w[0].s->chat_lines_sent() == sent + 1 && w[0].s->chat_lines_refused() == refused, "a tell to a player who is there is taken with one free step");
    CHECK(w[1].s->chat_lines_received() == dee_got + 1 && w[0].s->replies_pending() == limit, "dee is sent the tell, and the sender's copy took the last step");
}

AOC_REGISTER_TEST(test_tell_asks_for_the_room_it_needs);

static void test_chat_skips_a_player_whose_connection_is_going_away() {
    std::printf("a /say and a /tell skip a player whose connection the server is closing\n");
    ChatWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(w.enter(0, chat_record("cyb", kCyb.c_str(), kChatX, kChatY, kChatZ), identity()) &&
              w.enter(1, chat_record("dee", "11112222333344445555666677778888", kChatX + 500, kChatY, kChatZ), identity()),
          "two players join and enter the world, 5 m apart");
    if (!w[0].s || !w[1].s) return;
    const uint32_t pc_ch = w[0].s->channel_of(bs::Role::PlayerController);
    // dee is being dropped: the close of his connection is queued (a kick does that), and his client hears nothing more
    CHECK(w[1].conn.send_close(), "(the server closes dee's connection)");
    const size_t dee_got = w[1].s->chat_lines_received(), dee_dropped = w[1].s->chat_lines_dropped();
    std::string why;
    {
        const size_t ba = w[0].got.size();
        w.client_sends(w[0], {w[0].client.actor(pc_ch, chat_call(pc_cls, "hi", 45, "dee"), true)});
        for (int i = 0; i < 6; ++i) chat_fixes::step_without(w, 1);
        const auto lines = chat_lines_in(w[0].got, pc_cls, ba);
        CHECK(lines.size() == 2 && line_is(lines[0], "To dee: hi", "", 45, &why) && line_is(lines[1], "dee is not online.", "Server", 124, &why),
              "a tell to him is answered with the not-online line");
    }
    {
        w.client_sends(w[0], {w[0].client.actor(pc_ch, chat_call(pc_cls, "anyone here", 9), true)});
        for (int i = 0; i < 6; ++i) chat_fixes::step_without(w, 1);
        const std::string log = g_log.str();
        const size_t at = log.rfind("the say reached ");
        CHECK(at != std::string::npos && log.compare(at, 31, "the say reached 0 other player(") == 0, "a say 5 m from him reaches nobody");
    }
    CHECK(w[1].s->chat_lines_received() == dee_got && w[1].s->chat_lines_dropped() == dee_dropped, "no line was queued for him or dropped at his queue");
}

AOC_REGISTER_TEST(test_chat_skips_a_player_whose_connection_is_going_away);
