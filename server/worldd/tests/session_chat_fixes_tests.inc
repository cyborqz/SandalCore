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

static void test_tell_to_a_player_still_loading_is_held() {
    std::printf("a /tell to a player who has joined but is still loading gets the sender's copy only, and the line is read after the welcome line\n");
    ChatWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(w.enter(0, chat_record("cyb", kCyb.c_str(), kChatX, kChatY, kChatZ), identity()) &&
              w.join(1, chat_record("dee", "11112222333344445555666677778888", kChatX + 500, kChatY, kChatZ), identity()),
          "cyb enters the world and dee joins and stays loading");
    if (!w[0].s || !w[1].s) return;
    w.settle();
    CHECK(!w[1].s->welcome_line_sent(), "(dee is still loading: no welcome line yet)");
    const uint32_t pc_ch = w[0].s->channel_of(bs::Role::PlayerController);
    const size_t ba = w[0].got.size(), bb = w[1].got.size();
    w.client_sends(w[0], {w[0].client.actor(pc_ch, chat_call(pc_cls, "hi", 45, "dee"), true)});
    w.settle();
    {
        std::string why;
        CHECK(one_line(chat_lines_in(w[0].got, pc_cls, ba), "To dee: hi", "", 45, &why), ("the sender gets its copy only: " + why).c_str());
    }
    CHECK(chat_lines_in(w[1].got, pc_cls, bb).empty(), "dee's client has no chat line while it is loading");
    w.client_sends(w[1], {w[1].client.actor(w[1].s->channel_of(bs::Role::Pawn), hex_bits(capture_blocks::kClientReadyToPhaseIn), true)});
    w.settle();
    const auto lines = chat_lines_in(w[1].got, pc_cls, bb);
    std::string why;
    CHECK(lines.size() == 2, "dee reads two lines once its phase-in is built");
    if (lines.size() == 2) {
        CHECK(line_is(lines[0], "Welcome to SandalCore.", "Server", 124, &why), ("the first line is the welcome line from Server: " + why).c_str());
        why.clear();
        CHECK(line_is(lines[1], "hi", "cyb", 45, &why), ("the second line is the held tell from cyb: " + why).c_str());
    }
    CHECK(w[1].s->chat_lines_received() == 1, "the held tell counts in dee's received lines once it is delivered");
}

AOC_REGISTER_TEST(test_tell_to_a_player_still_loading_is_held);

static void test_a_loading_session_that_ends_drops_its_held_tell() {
    std::printf("a session that ends while loading drops the tell it held, counts it, and its name is not online after that\n");
    ChatWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(w.enter(0, chat_record("cyb", kCyb.c_str(), kChatX, kChatY, kChatZ), identity()) &&
              w.join(1, chat_record("dee", "11112222333344445555666677778888", kChatX + 500, kChatY, kChatZ), identity()),
          "cyb enters the world and dee joins and stays loading");
    if (!w[0].s || !w[1].s) return;
    w.settle();
    const uint32_t pc_ch = w[0].s->channel_of(bs::Role::PlayerController);
    const size_t ba = w[0].got.size(), bb = w[1].got.size();
    w.client_sends(w[0], {w[0].client.actor(pc_ch, chat_call(pc_cls, "hi", 45, "dee"), true)});
    w.settle();
    {
        std::string why;
        CHECK(one_line(chat_lines_in(w[0].got, pc_cls, ba), "To dee: hi", "", 45, &why), ("the sender got its copy only: " + why).c_str());
    }
    CHECK(chat_lines_in(w[1].got, pc_cls, bb).empty(), "dee read nothing while loading");
    const size_t dropped = w[1].s->chat_lines_dropped(), received = w[1].s->chat_lines_received();
    w[1].s->finish("the loading session ends");
    w.settle();
    CHECK(w[1].s->chat_lines_dropped() == dropped + 1, "the held line is counted as dropped when the session ends");
    CHECK(w[1].s->chat_lines_received() == received, "the held line was never counted as received");
    CHECK(chat_lines_in(w[1].got, pc_cls, bb).empty(), "dee's client reads nothing after the session ended");
    // (the name is not online any more: a second tell gets the sender's copy and the not-online line)
    const size_t ba2 = w[0].got.size();
    w.client_sends(w[0], {w[0].client.actor(pc_ch, chat_call(pc_cls, "again", 45, "dee"), true)});
    w.settle();
    const auto lines = chat_lines_in(w[0].got, pc_cls, ba2);
    std::string why;
    CHECK(lines.size() == 2 && line_is(lines[0], "To dee: again", "", 45, &why) && line_is(lines[1], "dee is not online.", "Server", 124, &why),
          ("a tell to the ended session gets the copy and the not-online line: " + why).c_str());
    CHECK(w[1].s->chat_lines_received() == received, "the ended session still read nothing");
}

AOC_REGISTER_TEST(test_a_loading_session_that_ends_drops_its_held_tell);

static void test_a_loading_session_holds_at_most_64_tells() {
    std::printf("a loading session holds 64 tells in order; the 65th is dropped and counted\n");
    ChatWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(w.enter(0, chat_record("cyb", kCyb.c_str(), kChatX, kChatY, kChatZ), identity()) &&
              w.join(1, chat_record("dee", "11112222333344445555666677778888", kChatX + 500, kChatY, kChatZ), identity()),
          "cyb enters the world and dee joins and stays loading");
    if (!w[0].s || !w[1].s) return;
    w.settle();
    const uint32_t pc_ch = w[0].s->channel_of(bs::Role::PlayerController);
    const size_t bb = w[1].got.size();
    for (int i = 0; i < 65; ++i) {
        w.client_sends(w[0], {w[0].client.actor(pc_ch, chat_call(pc_cls, "line " + std::to_string(i), 45, "dee"), true)});
        w.settle();
    }
    CHECK(w[1].s->chat_lines_dropped() == 1, "the 65th line found no room in the held lines and was counted as dropped");
    CHECK(chat_lines_in(w[1].got, pc_cls, bb).empty(), "dee read nothing while loading");
    w.client_sends(w[1], {w[1].client.actor(w[1].s->channel_of(bs::Role::Pawn), hex_bits(capture_blocks::kClientReadyToPhaseIn), true)});
    w.settle();
    const auto lines = chat_lines_in(w[1].got, pc_cls, bb);
    std::string why;
    CHECK(lines.size() == 65, "dee reads the welcome line and the 64 held tells");
    if (lines.size() == 65) {
        CHECK(line_is(lines[0], "Welcome to SandalCore.", "Server", 124, &why), ("the welcome line is first: " + why).c_str());
        why.clear();
        CHECK(line_is(lines[1], "line 0", "cyb", 45, &why) && line_is(lines[64], "line 63", "cyb", 45, &why),
              "the first held tell is first and the 64th held tell is last");
        size_t at = 1;
        for (; at < 65; ++at) {
            why.clear();
            if (!line_is(lines[at], "line " + std::to_string(at - 1), "cyb", 45, &why)) break;
        }
        CHECK(at == 65, ("the held tells arrive in the order they were sent, and the 65th is not among them: " + why).c_str());
    }
    CHECK(w[1].s->chat_lines_received() == 64, "the 64 held tells count as received");
}

AOC_REGISTER_TEST(test_a_loading_session_holds_at_most_64_tells);

static void test_two_writers_tell_a_loading_player_in_order() {
    std::printf("two writers tell a loading player; the held lines keep the order they were sent in\n");
    ChatWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(w.enter(0, chat_record("cyb", kCyb.c_str(), kChatX, kChatY, kChatZ), identity()) &&
              w.enter(1, chat_record("ayla", "aaaabbbbccccddddeeeeffff00001111", kChatX + 500, kChatY, kChatZ), identity()) &&
              w.join(2, chat_record("dee", "11112222333344445555666677778888", kChatX + 1000, kChatY, kChatZ), identity()),
          "cyb and ayla enter the world and dee joins and stays loading");
    if (!w[0].s || !w[1].s || !w[2].s) return;
    w.settle();
    const uint32_t pc_ch = w[0].s->channel_of(bs::Role::PlayerController);
    const size_t ba = w[0].got.size(), bb = w[1].got.size(), bc = w[2].got.size();
    w.client_sends(w[0], {w[0].client.actor(pc_ch, chat_call(pc_cls, "from cyb", 45, "dee"), true)});
    w.settle();
    w.client_sends(w[1], {w[1].client.actor(w[1].s->channel_of(bs::Role::PlayerController), chat_call(pc_cls, "from ayla", 45, "dee"), true)});
    w.settle();
    {
        std::string why;
        CHECK(one_line(chat_lines_in(w[0].got, pc_cls, ba), "To dee: from cyb", "", 45, &why), ("cyb gets its copy only: " + why).c_str());
        why.clear();
        CHECK(one_line(chat_lines_in(w[1].got, pc_cls, bb), "To dee: from ayla", "", 45, &why), ("ayla gets its copy only: " + why).c_str());
    }
    CHECK(chat_lines_in(w[2].got, pc_cls, bc).empty(), "dee read nothing while loading");
    w.client_sends(w[2], {w[2].client.actor(w[2].s->channel_of(bs::Role::Pawn), hex_bits(capture_blocks::kClientReadyToPhaseIn), true)});
    w.settle();
    const auto lines = chat_lines_in(w[2].got, pc_cls, bc);
    std::string why;
    CHECK(lines.size() == 3, "dee reads the welcome line and the two held tells");
    if (lines.size() == 3) {
        CHECK(line_is(lines[0], "Welcome to SandalCore.", "Server", 124, &why), ("the welcome line is first: " + why).c_str());
        why.clear();
        CHECK(line_is(lines[1], "from cyb", "cyb", 45, &why), ("cyb's line comes next: " + why).c_str());
        why.clear();
        CHECK(line_is(lines[2], "from ayla", "ayla", 45, &why), ("ayla's line comes last: " + why).c_str());
    }
}

AOC_REGISTER_TEST(test_two_writers_tell_a_loading_player_in_order);

static void test_a_second_login_receives_the_tell_held_for_it() {
    std::printf("a tell to a player whose first login ended while loading is held by the second login and read after its welcome line\n");
    ChatWorld w;
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    const std::string dee_id = "11112222333344445555666677778888";
    CHECK(w.enter(0, chat_record("cyb", kCyb.c_str(), kChatX, kChatY, kChatZ), identity()) &&
              w.join(1, chat_record("dee", dee_id.c_str(), kChatX + 500, kChatY, kChatZ), identity()),
          "cyb enters the world and dee's first session joins and stays loading");
    if (!w[0].s || !w[1].s) return;
    w.settle();
    w[1].s->finish("the first login ends");
    w.settle();
    CHECK(!w[2].s && w.join(2, chat_record("dee", dee_id.c_str(), kChatX + 500, kChatY, kChatZ), identity()), "a second session of dee joins");
    if (!w[2].s) return;
    w.settle();
    CHECK(!w[2].s->welcome_line_sent(), "(the second session is still loading)");
    const uint32_t pc_ch = w[0].s->channel_of(bs::Role::PlayerController);
    const size_t ba = w[0].got.size(), b1 = w[1].got.size(), b2 = w[2].got.size();
    w.client_sends(w[0], {w[0].client.actor(pc_ch, chat_call(pc_cls, "hi", 45, "dee"), true)});
    w.settle();
    {
        std::string why;
        CHECK(one_line(chat_lines_in(w[0].got, pc_cls, ba), "To dee: hi", "", 45, &why), ("the sender got its copy only: " + why).c_str());
    }
    CHECK(chat_lines_in(w[1].got, pc_cls, b1).empty(), "the ended first session reads nothing");
    CHECK(chat_lines_in(w[2].got, pc_cls, b2).empty(), "the second session has not read the tell yet");
    w.client_sends(w[2], {w[2].client.actor(w[2].s->channel_of(bs::Role::Pawn), hex_bits(capture_blocks::kClientReadyToPhaseIn), true)});
    w.settle();
    const auto lines = chat_lines_in(w[2].got, pc_cls, b2);
    std::string why;
    CHECK(lines.size() == 2, "the second session reads the welcome line and the held tell");
    if (lines.size() == 2) {
        CHECK(line_is(lines[0], "Welcome to SandalCore.", "Server", 124, &why), ("the welcome line is first: " + why).c_str());
        why.clear();
        CHECK(line_is(lines[1], "hi", "cyb", 45, &why), ("the held tell from cyb comes next: " + why).c_str());
    }
}

AOC_REGISTER_TEST(test_a_second_login_receives_the_tell_held_for_it);
