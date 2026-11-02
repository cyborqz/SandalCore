// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/movement_view_threads_tests.inc: a joined player through the Endpoint with the view passes on the pool (net/endpoint_phases.cpp)
//
//  The programs of the Endpoint tests run with AOC_TEST_VIEW_THREADS set pass in the three phases of the tick.  This test walks a joined player with a mob 20 m away through the Endpoint with
//  no pool (the loop as it was), with one thread (the reference of the pool) and with three: the pool must run the player's passes, the mob must be opened on the player's client, the registry
//  must be thawed afterwards, and every datagram the server sends the player must be the same bytes in the three runs.
// ============================================================================
static BunchWriter hex_bits(const capture_blocks::Bits& b) {
    const auto bytes = capture_blocks::bytes_of(b.hex);
    BunchWriter w;
    w.write_bit_range(bytes.data(), 0, b.bits);
    return w;
}

/// A mob with a few of the values the retail ones have (the actor block and its info), enough for a view to open it.
static world::WorldActor view_test_mob(uint64_t id, double x, double y, double z) {
    const auto guid_of = [](uint64_t object, uint32_t server) {
        emit::FIntrepidNetworkGUID g;
        g.ObjectId = object;
        g.ServerId = server;
        g.Randomizer = server == 0 ? 0 : kRandomizer;
        return g;
    };
    const auto class_schema = [](const char* name) -> const schema::ClassSchema* {
        const schema::ClassSchema* s = schema::gen::find_class_schema(name);
        if (!s) {
            std::printf("  [FAIL] no schema %s\n", name);
            std::exit(2);
        }
        return s;
    };
    const auto set_value = [](repl::ObjectState& o, const char* path, repl::Value v) {
        if (!o.state.set(*o.schema, path, std::move(v))) {
            std::printf("  [FAIL] %s.%s refused\n", o.schema->name, path);
            g_fail++;
        }
    };
    const auto link = [&](uint64_t object, const char* name, uint32_t checksum) {
        repl::ExportLink l;
        l.guid = guid_of(object, 0);
        l.name = name;
        l.no_load = false;
        l.has_checksum = true;
        l.checksum = checksum;
        return l;
    };
    world::WorldActor w;
    w.id = id;
    w.kind = world::ActorKind::Npc;
    w.x = x;
    w.y = y;
    w.z = z;
    repl::Actor& a = w.actor;
    a.guid = guid_of(id, kServerId);
    a.obj.schema = class_schema("ANPCCharacter");
    set_value(a.obj, "AuthServerIDReplicated", repl::Value::u64(kServerId));
    repl::RepMovement m;
    m.location = repl::QuantVec{repl::QuantVec::Mode::Scaled, static_cast<int64_t>(x * 100), static_cast<int64_t>(y * 100), static_cast<int64_t>(z * 100)};
    m.rotation = repl::Rot{0, 64, 0};
    m.linear_velocity = repl::QuantVec{repl::QuantVec::Mode::Scaled, 0, 0, 0};
    set_value(a.obj, "ReplicatedMovement", repl::Value(m));
    set_value(a.obj, "RemoteRole", repl::Value::u64(1));
    set_value(a.obj, "Owner", repl::Value(guid_of(id + 2, kServerId)));
    set_value(a.obj, "Role", repl::Value::u64(4));
    set_value(a.obj, "Controller", repl::Value(guid_of(id + 2, kServerId)));
    set_value(a.obj, "ReplicatedMovementMode", repl::Value::u64(10));
    set_value(a.obj, "CurrentMovementInput", repl::Value(repl::PlayerMovementInput{0}));
    a.open.archetype = {link(0xA001, "Default__Wolf_C", 0x1234), link(0xB001, "/Game/DesignAssets/Population/NPC/Beast/Wolf/Wolf", 0)};
    a.open.level = bs::persistent_level_export();
    a.open.spawn.location = repl::QuantVec{repl::QuantVec::Mode::Scaled, static_cast<int64_t>(x * 10), static_cast<int64_t>(y * 10), static_cast<int64_t>(z * 10)};
    a.open.spawn.rotation = repl::Rot{0, 11000, 0};
    repl::Subobject info;
    info.guid = guid_of(id + 6, kServerId);
    info.name = "BaseCharacterInfo";
    info.checksum = 0xaff9746f;
    info.obj.schema = class_schema("UNPCInfo");
    set_value(info.obj, "CharacterGuid.A", repl::Value::i64(static_cast<int32_t>(id)));
    set_value(info.obj, "Level", repl::Value::i64(14));
    a.subobjects.push_back(std::move(info));
    w.npc.name = "A Wolf";
    w.npc.level = 14;
    return w;
}

struct ThreadedEndpoints {
    Fixture                          fx;
    FakeCharacters                   api;
    std::unique_ptr<CharacterSaver>  saver;
    EndpointConfig                   cfg;
    std::unique_ptr<Rig>             rig;
    explicit ThreadedEndpoints(unsigned threads) {
        saver = std::make_unique<CharacterSaver>(api, quick());
        fx.world->set_position_sink(saver.get());
        cfg.players = fx.world.get();
        cfg.view_threads = threads;
        rig = std::make_unique<Rig>(cfg);
    }
    ~ThreadedEndpoints() {
        rig.reset();
        saver.reset();
    }
};

/// The datagrams the server sent the player, folded into one number (FNV-1a over every byte and every length).
struct SentDigest {
    uint64_t h = 14695981039346656037ull;
    size_t   datagrams = 0;
    void add(const std::vector<uint8_t>& p) {
        for (const uint8_t b : p) h = (h ^ b) * 1099511628211ull;
        h = (h ^ p.size()) * 1099511628211ull;
        ++datagrams;
    }
};

/// exchange(), with every datagram the server answers folded into `digest`.
static void exchange_digest(Rig& rig, Joined& j, const Address& addr, std::vector<fake::Bunch> bunches, Clock::time_point now, SentDigest& digest) {
    const auto pkt = j.client->packet(std::move(bunches));
    rig.ep.on_datagram(addr, pkt.data(), pkt.size(), now);
    rig.ep.tick(now);
    for (const auto& p : rig.take(addr)) {
        digest.add(p);
        const auto r = j.client->receive(p);
        for (const auto& d : r.delivered) j.got.push_back(d);
    }
}

static void test_endpoint_runs_the_view_passes_on_the_pool() {
    std::printf("through the endpoint with the view passes on the pool, a mob 20 m from the player\n");
    char text[360];
    uint64_t digests[3] = {};
    size_t datagrams[3] = {};
    size_t index = 0;
    for (const unsigned threads : {0u, 1u, 3u}) {
        g_counter = 1;   // (the handshake's random bytes start again, so the three runs can be compared byte for byte)
        ThreadedEndpoints e(threads);
        Rig& rig = *e.rig;
        CHECK(e.fx.world->registry().add(view_test_mob(77000, kX + 2000.0, kY, kZ)), "the mob joins the registry, 20 m from where the player spawns");
        Joined j = handshake(rig, kA, t0);
        join_world(rig, j, kA, cyb_record(), t0);
        exchange(rig, j, kA, {j.client->actor(5, hex_bits(capture_blocks::kClientReadyToPhaseIn), true)}, t0 + milliseconds(60));   // (the client is ready to phase in: the view of the world starts)
        SentDigest digest;
        for (int i = 1; i <= 40; ++i) {
            exchange_digest(rig, j, kA, {j.client->actor(5, client_move(move_at(1.0f + 0.001f * static_cast<float>(i), kX + 10.0 + i, kY, kZ, 8192)), false)}, t0 + milliseconds(100 + 40 * i), digest);
        }
        const PlayerSession* player = rig.ep.player_at(kA);
        const ActorView* view = player ? player->actor_view() : nullptr;
        const size_t opened = view ? view->stats().opens + view->stats().snapshots : 0;
        std::snprintf(text, sizeof(text), "%u view thread(s): the player's session lives, %s, the mob was opened on its client (%zu opens), %zu datagrams sent in the walk, and the registry is thawed", threads,
                      threads == 0 ? "no pool runs" : "its passes ran on the pool", opened, digest.datagrams);
        const bool pooled = threads == 0 ? rig.ep.view_jobs() == 0 : rig.ep.view_jobs() > 0;
        CHECK(rig.ep.view_threads() == threads && rig.ep.session_count() == 1 && pooled && opened > 0 && digest.datagrams > 0 && !e.fx.world->registry().frozen(), text);
        digests[index] = digest.h;
        datagrams[index] = digest.datagrams;
        ++index;
    }
    std::snprintf(text, sizeof(text), "the datagrams the player got are the same bytes with no pool, one thread and three (%zu, %zu and %zu datagrams; 0x%016llx, 0x%016llx and 0x%016llx)", datagrams[0], datagrams[1], datagrams[2],
                  static_cast<unsigned long long>(digests[0]), static_cast<unsigned long long>(digests[1]), static_cast<unsigned long long>(digests[2]));
    CHECK(digests[0] == digests[1] && digests[0] == digests[2] && datagrams[0] == datagrams[1] && datagrams[0] == datagrams[2], text);
}
AOC_REGISTER_TEST(test_endpoint_runs_the_view_passes_on_the_pool);
