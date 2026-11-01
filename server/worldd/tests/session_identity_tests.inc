// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/session_identity_tests.inc
//
//  Registered tests of test_worldd_session: the login identity, the content the setup needs and the stat program.
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================

// ============================================================================================================================
//  identity
// ============================================================================================================================

static void test_identity() {
    std::printf("the login identity\n");
    // The e2e capture's own NMT_Login, a real packet of the retail client
    const auto bytes = capture::bytes(capture::kClientLogin);
    const auto p = conn::read_data_packet(bytes.data(), bytes.size(), wire::Direction::ClientToServer);
    std::optional<conn::ControlMessage> m;
    if (p) {
        const auto bunches = conn::packet_bunches(bytes.data(), bytes.size(), *p);
        if (bunches.size() == 1) m = conn::parse_control_message(bytes.data(), bytes.size(), bunches[0].data_start_bit, bunches[0].bunch_data_bits);
    }
    CHECK(m && m->type == conn::Nmt::Login && m->has_unique_id && m->platform_name == "NULL",
          "the captured NMT_Login parses with its unique id (flags 8) and platform name 'NULL'");
    CHECK(m && m->request_url == "?Name=" + m->unique_id && m->unique_id.rfind("DESKTOP-EXAMPL2-", 0) == 0 && m->unique_id.size() == 48,
          "its unique id is the ?Name= value, the 48 characters the transcript names (section 2.3)");
// (C.O.A | C | Y | B)

    ControlChannel c(WorldInfo{}, "X");
    conn::ControlMessage hello;
    hello.type = conn::Nmt::Hello;
    c.on_message(hello);
    CHECK(!c.login_message(), "no Login yet: nothing kept");
    if (m) c.on_message(*m);
    CHECK(c.login_message() && c.login_message()->has_unique_id && c.login_message()->unique_id == m->unique_id &&
              c.login_message()->request_url == m->request_url,
          "the control channel keeps the Login's URL and unique id");

    CHECK(url_option("?Name=cyb", "Name") == "cyb" && url_option("?Name=cyb?Team=1", "name") == "cyb" && url_option("Map?Team=1?Name=a b", "Name") == "a b" &&
              url_option("?Team=1", "Name").empty() && url_option("", "Name").empty() && url_option("?Name", "Name").empty() && url_option("?Name=", "Name").empty(),
          "url_option: the value up to the next '?', the key without case, nothing when absent");

    std::vector<std::string> notes;
    const svc::Character rec = cyb_record();
    auto id = make_login_identity(LoginMessage{"?Name=" + kMachine, true, kMachine}, rec, &notes);
    CHECK(id.name_option == kMachine && id.unique_id == kMachine && notes.empty(), "a full Login: the name option and the unique id as sent, no note");
    id = make_login_identity(LoginMessage{"?Name=" + kMachine, false, ""}, rec, &notes);
    CHECK(id.name_option == kMachine && id.unique_id == kMachine && notes.size() == 1, "no unique id: the name option stands in, with a note");
    notes.clear();
    id = make_login_identity(LoginMessage{"?Team=1", true, "ID"}, rec, &notes);
    CHECK(id.name_option == "cyb" && id.unique_id == "ID" && notes.size() == 1, "no ?Name=: the character's name stands in, with a note");
    notes.clear();
    id = make_login_identity(std::nullopt, rec, &notes);
    CHECK(id.name_option == "cyb" && id.unique_id == "cyb" && notes.size() == 2, "no Login at all: both fall back to the character's name");
}

AOC_REGISTER_TEST(test_identity);

// ============================================================================================================================
//  through the endpoint
// ============================================================================================================================

static void test_content_missing() {
    std::printf("content.db missing\n");
    Fixture fx(bs::GameStateMode::Tier2, false);
    CHECK(!fx.world->ready() && !fx.world->content_problem().empty(), "the world is not ready and says why");
    EndpointConfig cfg;
    cfg.players = fx.world.get();
    Rig rig(cfg);
    g_log.str("");
    Joined j = handshake(rig, kA, t0);
    CHECK(j.client && rig.ep.session_count() == 1 && rig.logins.submitted.empty(),
          "the handshake completes but no login is submitted: the join ticket is not redeemed for a refusal");
    bool closed = false;
    auto got = exchange(rig, j, kA, {j.client->hello()}, t0 + milliseconds(10));
    const conn::ControlMessage* failure = find_message(got, conn::Nmt::Failure);
    closed = closed_by_server(got);
    CHECK(failure && failure->error == login_text::kContentMissing && closed, "Hello gets NMT_Failure with the content.db text, and the channel closes");
    CHECK(std::string(login_text::kContentMissing).find("content.db") != std::string::npos, "the text names content.db for the operator");
    CHECK(!rig.ep.session_of(kCyb), "the character is not logged in");
    CHECK(g_log.str().find("worldd has no content.db (no such file)") != std::string::npos && g_log.str().find(kTicket) == std::string::npos,
          "the log says why, without the ticket");
}

AOC_REGISTER_TEST(test_content_missing);

/// A content.db built before plan B2 has no item or item_equip_slot table: ContentDb::open fails (main.cpp then runs worldd with no content and the
/// reason), and every login is refused exactly as with a missing file; the reason the log gives says to rebuild the file.
static void test_content_built_before_b2() {
    std::printf("a content.db built before plan B2\n");
    const fs::path dir = content_fixture::make_temp_dir("worldd_session_old_");
    content_fixture::DbOptions old_schema;
    old_schema.item_tables = false;
    const fs::path path = dir / "content.db";
    if (!content_fixture::make_db(path, old_schema)) std::exit(2);
    std::string error;
    std::optional<content::ContentDb> content = content::ContentDb::open(path.string(), error);
    CHECK(!content && error.find("does not have the tables this worldd reads") != std::string::npos && error.find("item") != std::string::npos &&
              error.find("rebuild it with this checkout's contentc") != std::string::npos,
          "the file does not open: it lacks the item tables, and the error says to rebuild it with contentc");
    aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
    // as main.cpp builds it when the file did not open
    PlayerWorld world(content ? &*content : nullptr, error, bs::GameStateMode::Tier2, kServerId, guids);
    CHECK(!world.ready() && world.content_problem() == error, "the world is not ready, and its reason is the one open() gave");
    EndpointConfig cfg;
    cfg.players = &world;
    Rig rig(cfg);
    g_log.str("");
    Joined j = handshake(rig, kA, t0);
    CHECK(j.client && rig.ep.session_count() == 1 && rig.logins.submitted.empty(), "the handshake completes but no login is submitted: the join ticket is not redeemed");
    auto got = exchange(rig, j, kA, {j.client->hello()}, t0 + milliseconds(10));
    const conn::ControlMessage* failure = find_message(got, conn::Nmt::Failure);
    CHECK(failure && failure->error == login_text::kContentMissing && closed_by_server(got), "Hello gets NMT_Failure with the content.db text, and the channel closes: every login is refused");
    CHECK(!rig.ep.session_of(kCyb), "the character is not logged in");
    // a second player is refused the same way: the refusal is not a one-off
    Joined k = handshake(rig, kB, t0 + milliseconds(20));
    auto got_k = exchange(rig, k, kB, {k.client->hello()}, t0 + milliseconds(30));
    const conn::ControlMessage* failure_k = find_message(got_k, conn::Nmt::Failure);
    CHECK(failure_k && failure_k->error == login_text::kContentMissing && rig.logins.submitted.empty(), "and so is the next client");
    CHECK(g_log.str().find("worldd has no content.db (") != std::string::npos && g_log.str().find("rebuild it with this checkout's contentc") != std::string::npos &&
              g_log.str().find(kTicket) == std::string::npos,
          "the log says why and how to rebuild the file, without the ticket");
    content.reset();
    std::error_code ec;
    fs::remove_all(dir, ec);
}
// ~ C-O-A :: C | Y | B ~

AOC_REGISTER_TEST(test_content_built_before_b2);

/// Plan B3: a content.db whose stat program is missing, damaged, of another graph, or made by an older contentc is no content.db to serve players
/// with: the world is not ready, its reason says what and how to rebuild, and every login is refused as with a missing file.
static void test_stat_program_problems() {
    std::printf("a content.db with no usable stat program (plan B3)\n");
    const fs::path dir = content_fixture::make_temp_dir("worldd_session_statprog_");
    std::vector<uint8_t> damaged = stats_fixture::main_bundle();
    damaged[damaged.size() / 2] ^= 0x01;   // a byte of the body: the crc says so
    const std::vector<uint8_t> none;
    const std::vector<uint8_t>& main_bytes = stats_fixture::main_bundle();
    const std::vector<uint8_t>& tiny = stats_fixture::corpus().bundles.at("tiny");   // a valid bundle of another graph
    struct Case {
        const char* what;
        const std::vector<uint8_t>* bytes;
        const char* want;
        int64_t crc_xor = 0;
    };
    const Case cases[] = {
        {"a damaged bundle (one flipped byte)", &damaged, "does not verify"},
        {"no row in stat_program", &none, "no stat program"},
        {"a valid bundle of another graph (the tiny one of the tests)", &tiny, "not one this worldd can run"},
        {"a row whose crc32 is not the bundle header's", &main_bytes, "bundle's header says", 0x5a5a},
    };
    int n = 0;
    for (const Case& c : cases) {
        content_fixture::DbOptions o;
        o.stat_program_bytes = c.bytes;
        o.stat_program_crc_xor = c.crc_xor;
        const fs::path path = dir / (std::string("c") + std::to_string(n++) + ".db");
        if (!content_fixture::make_db(path, o)) std::exit(2);
        std::string error;
        std::optional<content::ContentDb> content = content::ContentDb::open(path.string(), error);
        CHECK(content.has_value(), (std::string(c.what) + ": the file opens (its tables are there)").c_str());
        if (!content) continue;
        aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
        PlayerWorld world(&*content, "", bs::GameStateMode::Tier2, kServerId, guids);
        const auto hint_count = [&] {
            size_t k = 0;
            for (size_t at = world.content_problem().find("go run ./cmd/contentc"); at != std::string::npos; at = world.content_problem().find("go run ./cmd/contentc", at + 1)) ++k;
            return k;
        };
        CHECK(hint_count() == 1, (std::string(c.what) + ": the way out is said once, not twice").c_str());
        CHECK(!world.ready() && world.content_problem().find(c.want) != std::string::npos &&
                  world.content_problem().find("go run ./cmd/contentc") != std::string::npos && world.stat_bundle() == nullptr && world.content() == nullptr,
              (std::string(c.what) + ": the world is not ready, and says '" + c.want + "' and how to rebuild").c_str());
        if (n == 1) {
            EndpointConfig cfg;
            cfg.players = &world;
            Rig rig(cfg);
            Joined j = handshake(rig, kA, t0);
            auto got = exchange(rig, j, kA, {j.client->hello()}, t0 + milliseconds(10));
            const conn::ControlMessage* failure = find_message(got, conn::Nmt::Failure);
            CHECK(failure && failure->error == login_text::kContentMissing && rig.logins.submitted.empty(), "a login is refused with the content.db text, before the ticket is redeemed");
        }
    }
    {
        content_fixture::DbOptions ok;
        const fs::path path = dir / "ok.db";
        if (!content_fixture::make_db(path, ok)) std::exit(2);
        std::string error;
        std::optional<content::ContentDb> content = content::ContentDb::open(path.string(), error);
        aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
        PlayerWorld world(content ? &*content : nullptr, "", bs::GameStateMode::Tier2, kServerId, guids);
        const bs::StatContext ctx = world.stat_context();
        CHECK(world.ready() && world.stat_bundle() && ctx.bundle == world.stat_bundle() && ctx.gear && ctx.rounding == stats::Rounding::HalfUp &&
                  world.stat_bundle()->classes.size() == 10,
              "(control) the fixture's real stat program makes a ready world with a bundle of ten classes (the eight player classes and the two NPC profiles) and the item data");
        content.reset();
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

AOC_REGISTER_TEST(test_stat_program_problems);
