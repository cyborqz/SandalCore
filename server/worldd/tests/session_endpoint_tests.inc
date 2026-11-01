// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/session_endpoint_tests.inc
//
//  Registered tests of test_worldd_session: two players at once, a kicked session, a join that cannot be sent, acknowledgements before requests, the login tail and the real content database.
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================

// ============================================================================================================================
//  through the endpoint: two players, a kicked session, a join that cannot be sent, acks before requests
// ============================================================================================================================

static void test_two_players() {
    std::printf("two players at once\n");
    Fixture fx;
    EndpointConfig cfg;
    cfg.players = fx.world.get();
    Rig rig(cfg);
    Joined a = handshake(rig, kA, t0);
    Joined b = handshake(rig, kB, t0 + milliseconds(1));
    log_in_as(rig, a, kA, cyb_record(), t0, kMachine);
    log_in_as(rig, b, kB, second_record(), t0 + milliseconds(3), "DESKTOP-OTHER-0000000000000000000000000000");
    CHECK(a.session != b.session, "two sessions");
    // both Join in the same tick
    const auto pa = a.client->packet({a.client->netspeed(200000), a.client->nmt(conn::Nmt::Join)});
    const auto pb = b.client->packet({b.client->netspeed(200000), b.client->nmt(conn::Nmt::Join)});
    rig.ep.on_datagram(kA, pa.data(), pa.size(), t0 + milliseconds(40));
    rig.ep.on_datagram(kB, pb.data(), pb.size(), t0 + milliseconds(40));
    rig.ep.tick(t0 + milliseconds(40));
    drain(rig, a, kA);
    drain(rig, b, kB);
    CHECK(a.got.size() == 16 && b.got.size() == 16, "each client has 16 chains");
    if (a.got.size() < 15 || b.got.size() < 15) return;
    const NewActor gsa = read_new_actor(a.got[4].content, false), gsb = read_new_actor(b.got[4].content, false);
    const NewActor pca = read_new_actor(a.got[0].content, true), pcb = read_new_actor(b.got[0].content, true);
    const NewActor pwa = read_new_actor(a.got[6].content, false), pwb = read_new_actor(b.got[6].content, false);
    const NewActor psa = read_new_actor(a.got[14].content, false), psb = read_new_actor(b.got[14].content, false);
    CHECK(gsa.actor == gsb.actor, "the GameState GUID is the same for both players");
    CHECK(!(pca.actor == pcb.actor) && !(pwa.actor == pwb.actor) && !(psa.actor == psb.actor), "PlayerController, pawn and PlayerState GUIDs differ");
    CHECK(!(psa.actor == pcb.actor) && !(pca.actor == pwb.actor), "no GUID of one is a GUID of the other");
    const schema::ClassSchema& ps_cls = class_named("AoCPlayerStateBP_C");
    const auto sa = block_of(a.got[14], psa.end, true, {}, ps_cls);
    const auto sb = block_of(b.got[14], psb.end, true, {}, ps_cls);
    const repl::Value* ida = sa ? sa->get(ps_cls, "PlayerId") : nullptr;
    const repl::Value* idb = sb ? sb->get(ps_cls, "PlayerId") : nullptr;
    CHECK(ida && idb && *ida == repl::Value::i64(1) && *idb == repl::Value::i64(2), "PlayerId 1 and 2");
    CHECK(rig.ep.session_count() == 2, "two sessions live");
}

AOC_REGISTER_TEST(test_two_players);

static void test_kicked_session_answers_nothing() {
    std::printf("a duplicate login kicks a session whose join is still unacknowledged, and the kicked one answers nothing more\n");
    Fixture fx;
    EndpointConfig cfg;
    cfg.players = fx.world.get();
    Rig rig(cfg);
    Joined a = handshake(rig, kA, t0);
    log_in(rig, a, kA, cyb_record(), t0);
    const auto pa = a.client->packet({a.client->netspeed(200000), a.client->nmt(conn::Nmt::Join)});
    rig.ep.on_datagram(kA, pa.data(), pa.size(), t0 + milliseconds(40));
    rig.ep.tick(t0 + milliseconds(40));
    auto held = rig.take(kA);   // client A receives nothing yet
    // B logs in as the same character from another address
    Joined b = handshake(rig, kB, t0 + milliseconds(50));
    log_in(rig, b, kB, cyb_record(), t0 + milliseconds(60));
    CHECK(rig.ep.session_of(kCyb) && *rig.ep.session_of(kCyb) == kB, "the character is B's now");
    // A's client now receives everything, including the Failure
    bool failure = false, closed = false;
    const auto receive_all = [&](const std::vector<std::vector<uint8_t>>& pkts) {
        for (const auto& p : pkts) {
            auto r = a.client->receive(p);
            for (const auto& m : r.messages) failure = failure || m.type == conn::Nmt::Failure;
            closed = closed || r.closed;
        }
    };
    receive_all(held);
    receive_all(rig.take(kA));
    CHECK(failure && closed, "A got the Failure and the close after its join");
    // A, kicked, still sends requests on its channels
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    const auto pkt = a.client->packet({a.client->actor(3, client_rpc(pc_cls, "ServerRequestClientLoad"), true)});
    rig.ep.on_datagram(kA, pkt.data(), pkt.size(), t0 + milliseconds(100));
    rig.ep.tick(t0 + milliseconds(100));
    size_t chains = 0;
    for (const auto& p : rig.take(kA)) chains += a.client->receive(p).delivered.size();
    CHECK(chains == 0, "a kicked (closing) session answers no further requests");
    // B joins fine
    join_now(rig, b, kB, t0 + milliseconds(110));
    CHECK(b.got.size() == 16, "B receives the full join");
    // time passes: A is dropped
    for (int i = 1; i <= 40; ++i) rig.ep.tick(t0 + milliseconds(110 + i * 100));
    CHECK(rig.ep.session_count() == 1, "A is dropped, B stays");
}
// [C | O | A - Cyb]

AOC_REGISTER_TEST(test_kicked_session_answers_nothing);

static void test_unsendable_join() {
    std::printf("a join that can never be sent\n");
    Fixture fx;
    EndpointConfig cfg;
    cfg.players = fx.world.get();
    struct Case { const char* what; std::string json; };
    const Case cases[] = {{"100 decals", cyb_with_decals(100)}, {"5000 morphs", morphs_only(5000)}, {"1500 decals", cyb_with_decals(1500)}};
    for (const Case& c : cases) {
        Rig rig(cfg);
        g_log.str("");
        Joined a = handshake(rig, kA, t0);
        svc::Character rec = cyb_record();
        rec.appearance_json = c.json;
        log_in(rig, a, kA, rec, t0);
        auto got = exchange(rig, a, kA, {a.client->netspeed(200000), a.client->nmt(conn::Nmt::Join)}, t0 + milliseconds(40));
        auto more = exchange(rig, a, kA, {}, t0 + milliseconds(100));
        const conn::ControlMessage* failure = find_message(got, conn::Nmt::Failure);
        if (!failure) failure = find_message(more, conn::Nmt::Failure);
        spdlog::default_logger()->flush();
        const std::string log = g_log.str();
        std::printf("    %s: appearance JSON of %zu bytes, %zu chains arrived, Failure %d\n", c.what, rec.appearance_json.size(), a.got.size(), int(failure != nullptr));
        if (c.what == std::string("100 decals")) {
            CHECK(!failure && a.got.size() == 16, "(control: an appearance with 100 decals, a pawn open of many parts, joins)");
        } else if (c.what == std::string("5000 morphs")) {
            CHECK(rec.appearance_json.size() < bs::kMaxAppearanceBytes, "(5,000 morphs are under the appearance cap)");
            CHECK(failure && failure->error == login_text::kLoginFailed,
                  "a join that cannot be sent (the pawn open's content is over what a chain may carry) ends with the Failure");
            CHECK(log.find("the join cannot be sent: pawn open") != std::string::npos, "and the log says which chain was refused");
            for (int i = 1; i <= 40; ++i) rig.ep.tick(t0 + milliseconds(100 + i * 100));
            CHECK(rig.ep.session_count() == 0, "the session is dropped after the linger");
        } else {
            CHECK(failure && failure->error == login_text::kLoginFailed && a.got.empty() && log.find("appearance: too large") != std::string::npos,
                  "1,500 decals (271 KB of JSON) are refused at the setup, before anything is built: the Failure, nothing sent, 'too large' in the log");
        }
    }
}

AOC_REGISTER_TEST(test_unsendable_join);

static void test_acks_before_requests() {
    std::printf("acknowledgements and a request in the same datagram\n");
    Fixture fx;
    EndpointConfig cfg;
    cfg.players = fx.world.get();
    Rig rig(cfg);
    Joined a = handshake(rig, kA, t0);
    log_in(rig, a, kA, cyb_record(), t0);
    // Join; the client receives the 15 chains (this acknowledges nothing yet: its next packet will)
    join_now(rig, a, kA, t0 + milliseconds(40));
    CHECK(a.got.size() == 16, "(the join arrived)");
    // one packet: the acks of all of it + the request for the load
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    const size_t before = a.got.size();
    const auto req = a.client->packet({a.client->actor(3, client_rpc(pc_cls, "ServerRequestClientLoad"), true)});
    rig.ep.on_datagram(kA, req.data(), req.size(), t0 + milliseconds(100));
    rig.ep.tick(t0 + milliseconds(100));
    drain(rig, a, kA);
    size_t exports = 0, chains = 0;
    for (size_t i = before; i < a.got.size(); ++i) { ++chains; exports += a.got[i].exports.size(); }
    CHECK(chains == 6 && exports == 0, "the acks in a datagram are applied before its requests: the reply leaves out the exports the datagram acknowledged");
}

AOC_REGISTER_TEST(test_acks_before_requests);

static void test_login_tail() {
    std::printf("NMT_Login's unique id in hostile and odd forms\n");
    const auto base = [](BunchWriter& w) {
        w.write_uint8(static_cast<uint8_t>(conn::Nmt::Login));
        w.write_fstring_ansi("0");
        w.write_fstring_ansi("?Name=cyb");
    };
    const auto is_login_without_id = [](const std::optional<conn::ControlMessage>& m) {
        return m && m->type == conn::Nmt::Login && !m->has_unique_id && m->request_url == "?Name=cyb";
    };
    {   // the capture's form
        BunchWriter w; base(w);
        w.write_uint8(8); w.write_fstring_ansi("cyb-id"); w.write_fstring_ansi("NULL");
        const auto m = parse_bits(w);
        CHECK(m && m->has_unique_id && m->unique_id == "cyb-id" && m->platform_name == "NULL" && m->request_url == "?Name=cyb", "the capture's form: flags 8, the id, the platform");
    }
    {   // truncated id: count says 500, 3 bytes follow
        BunchWriter w; base(w);
        w.write_uint8(8); w.write_int32(500); w.write_uint8('a'); w.write_uint8('b'); w.write_uint8('c');
        CHECK(is_login_without_id(parse_bits(w)), "a truncated id (count 500, 3 bytes): still a Login, without the unique id (the reader is restored)");
    }
    {   // id but no platform
        BunchWriter w; base(w);
        w.write_uint8(8); w.write_fstring_ansi("cyb-id");
        CHECK(is_login_without_id(parse_bits(w)), "an id without a platform: a Login, no unique id");
    }
    {   // flags 9 (encoded), junk after
        BunchWriter w; base(w);
        w.write_uint8(9); w.write_uint32(0xdeadbeef); w.write_uint32(0xdeadbeef);
        CHECK(is_login_without_id(parse_bits(w)), "flags 9 (encoded): a Login, no unique id");
    }
    {   // flags 0x10: another subsystem's id
        BunchWriter w; base(w);
        w.write_uint8(0x10); w.write_fstring_ansi("x"); w.write_fstring_ansi("NULL");
        CHECK(is_login_without_id(parse_bits(w)), "flags 0x10 (another subsystem): a Login, no unique id (only the NULL form is read)");
    }
    {   // UTF-16 id with a non-ASCII character
        BunchWriter w; base(w);
        w.write_uint8(8); write_fstring_utf16(w, u"Müller-PC"); w.write_fstring_ansi("NULL");
        const auto m = parse_bits(w);
        CHECK(m && m->has_unique_id && m->unique_id.size() == 9, "a UTF-16 id parses (its non-ASCII character as read_fstring maps it)");
    }
    {   // count 1025
        BunchWriter w; base(w);
        w.write_uint8(8); w.write_int32(1025);
        for (int i = 0; i < 1025; ++i) w.write_uint8('a');
        w.write_fstring_ansi("NULL");
        CHECK(is_login_without_id(parse_bits(w)), "an id over 1024 characters: a Login, no unique id");
    }
    {   // INT32_MIN as the length
        BunchWriter w; base(w);
        w.write_uint8(8); w.write_int32(std::numeric_limits<int32_t>::min()); w.write_fstring_ansi("NULL");
        CHECK(is_login_without_id(parse_bits(w)), "an INT32_MIN length: a Login, no unique id");
    }
    {   // empty id, empty platform
        BunchWriter w; base(w);
        w.write_uint8(8); w.write_int32(0); w.write_int32(0);
        const auto m = parse_bits(w);
        CHECK(m && m->has_unique_id && m->unique_id.empty(), "an empty id and platform: has_unique_id with an empty id");
        std::vector<std::string> notes;
        const auto id = make_login_identity(LoginMessage{m ? m->request_url : "", m && m->has_unique_id, m ? m->unique_id : ""}, cyb_record(), &notes);
        CHECK(id.unique_id == "cyb", "...and the identity falls back to the name option");
    }
    {   // trailing garbage after the platform
        BunchWriter w; base(w);
        w.write_uint8(8); w.write_fstring_ansi("id"); w.write_fstring_ansi("NULL");
        for (int i = 0; i < 40; ++i) w.write_uint8(0xAB);
        const auto m = parse_bits(w);
        CHECK(m && m->has_unique_id && m->unique_id == "id", "trailing garbage after the platform is ignored");
    }
    {   // an embedded NUL then padding (count 10)
        BunchWriter w; base(w);
        w.write_uint8(8); w.write_int32(10);
        const uint8_t raw[10] = {'a', 'b', 0, 'z', 'z', 'z', 'z', 'z', 'z', 'z'};
        for (uint8_t c : raw) w.write_uint8(c);
        w.write_fstring_ansi("NULL");
        const auto m = parse_bits(w);
        CHECK(m && m->type == conn::Nmt::Login, "an id with an embedded NUL still parses as a Login");
    }
    {   // 7 stray bits after the URL
        BunchWriter w; base(w);
        w.write_bits(1, 7);
        CHECK(is_login_without_id(parse_bits(w)), "7 stray bits after the URL: a Login, no unique id");
    }
    {   // the URL itself truncated: not a Login
        BunchWriter w;
        w.write_uint8(static_cast<uint8_t>(conn::Nmt::Login));
        w.write_fstring_ansi("0");
        w.write_int32(300);
        CHECK(!parse_bits(w), "a truncated URL: unreadable");
    }
}
// C-O-A.C.Y.B

AOC_REGISTER_TEST(test_login_tail);

/// With AOC_CONTENT_DB naming the real content.db (cd services && go run ./cmd/contentc): the same join against it, in each mode.
static void test_real_content_db() {
    std::printf("against the real content.db (AOC_CONTENT_DB)\n");
    const char* env = std::getenv("AOC_CONTENT_DB");
    if (!env || !*env) {
        std::printf("  [skip] AOC_CONTENT_DB is not set (cd services && go run ./cmd/contentc; AOC_CONTENT_DB=dist\\content\\content.db)\n");
        return;
    }
    for (const auto mode : {bs::GameStateMode::Tier1, bs::GameStateMode::Tier2, bs::GameStateMode::Captured}) {
        Harness h(mode, env);
        const bool joined = h.join();
        h.settle();
        auto pb = expected_bootstrap(*h.fx.content, cyb_record(), world_time_at(t0), mode);
        auto seq = pb->join_sequence();
        const std::string diff = joined && seq ? compare_sends(h.got, 0, *seq, kChannels) : "no join";
        const char* name = mode == bs::GameStateMode::Tier1 ? "tier1" : mode == bs::GameStateMode::Tier2 ? "tier2" : "captured";
        std::string msg = std::string("the join against the real content.db, GameState ") + name + ": 16 chains equal to the bootstrap's, in order";
        CHECK(joined && !h.s->failed() && h.got.size() == 16 && diff.empty(), msg.c_str());
        if (!diff.empty()) std::printf("    %s\n", diff.c_str());
    }
}

AOC_REGISTER_TEST(test_real_content_db);
