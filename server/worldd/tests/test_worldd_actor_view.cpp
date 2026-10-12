// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_actor_view.cpp  (plan E1, stage 1)
//
//  The actor system on a simulated clock and a real Connection pair (the server's chains reach a second Connection that acknowledges them):
//
//    registry     add and remove, a duplicate or odd id refused, the grid's radius query across cell edges and negative coordinates, a move
//                 that changes cells, the two counters (a position alone moves `version` only), listeners on removal
//    relevancy    classify: the awake ring, the snapshot ring and their hysteresis, a class's own radius, an actor that is never dormant
//    view         the first channel is the lowest free from 3 that is not the session's own; an awake actor opens with its whole state; a
//                 snapshot opens and closes with Dormancy 134 ms later with no update; walking up to a dormant actor opens it again from an empty
//                 shadow (the same bits); an update carries only the changed handles, reliable unless the position alone changed; leaving the awake
//                 ring closes with Dormancy, leaving the snapshot ring sends nothing, a never-dormant actor leaving closes with Relevancy;
//                 a removed actor is closed with Destroyed (a dormant one opened and closed); a channel is held until its close is acknowledged;
//                 the cap on opens per pass, nearest first; exports sent once the client acknowledged them
//    many actors  1,800 actors around a walking player for a simulated minute: the time of a pass and the bandwidth, measured
// ============================================================================
#include "bootstrap/constants.h"
#include "net/connection.h"
#include "protocol/repl/replicator.h"
#include "protocol/schema/generated/index.h"
#include "session/actor_view.h"
#include "world/actor_registry.h"
#include "world/relevancy.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace aoc::worldd;
namespace conn = aoc::protocol::conn;
namespace emit = aoc::protocol::emit;
namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;
using emit::FIntrepidNetworkGUID;
using std::chrono::milliseconds;
using std::chrono::seconds;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

static const conn::OuterHeader kOuter{0x500c7696u, 0, 5};
static const conn::CustomField kCf{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d};
static const Clock::time_point t0{seconds(1000)};

static ConnectionConfig server_config() {
    ConnectionConfig c;
    c.outer = kOuter;
    c.custom_field = kCf;
    c.server_initial_seq = 14265;
    c.client_initial_seq = 15870;
    return c;
}
// [C_Y_B ~ CoA]

static ConnectionConfig client_config() {
    ConnectionConfig c = server_config();
    std::swap(c.server_initial_seq, c.client_initial_seq);
    return c;
}

/// What the client saw of one chain on an actor channel.
struct Seen {
    uint32_t channel;
    bool     open, close, reliable;
    uint32_t reason;
    size_t   bits;
};

struct Pair {
    Connection s{server_config(), t0};
    Connection cl{client_config(), t0};
    Clock::time_point t = t0;
    std::vector<Seen> seen;
    std::vector<AckEvent> events;
    size_t packets = 0, bytes = 0;

    /// Flushes the server, delivers every packet and returns the acks; the events go to `view`.
    void step(ActorView* view = nullptr, milliseconds dt = milliseconds(60)) {
        t += dt;
        auto pkts = s.flush(t);
        packets += pkts.size();
        for (const auto& p : pkts) {
            bytes += p.size();
            for (auto& b : cl.receive(p, t)) {
                if (b.channel == 0) continue;
                seen.push_back({b.channel, b.open, b.close, b.reliable, b.close_reason, b.bits});
            }
        }
        t += dt;
        for (const auto& p : cl.flush(t)) s.receive(p, t);
        for (const AckEvent& e : s.take_ack_events()) {
            events.push_back(e);
            if (view) view->on_ack(e);
        }
    }
};

// ---- the actors ----------------------------------------------------------------------------------------

static const schema::ClassSchema& schema_named(const char* name) {
    const schema::ClassSchema* s = schema::gen::find_class_schema(name);
    if (!s) { std::printf("  [FAIL] no schema %s\n", name); std::exit(2); }
    return *s;
}

static FIntrepidNetworkGUID guid(uint64_t id, uint32_t server = 60, uint32_t rnd = 0x6ee87ee4) {
    FIntrepidNetworkGUID g;
    g.ObjectId = id;
    g.ServerId = server;
    g.Randomizer = rnd;
    return g;
}

static repl::ExportLink link(uint64_t id, const char* name, bool no_load, uint32_t checksum) {
    repl::ExportLink l;
    l.guid = guid(id, 0, 0);
    l.name = name;
    l.no_load = no_load;
    l.has_checksum = true;
    l.checksum = checksum;
    return l;
}

static void set_value(repl::ObjectState& o, const char* path, repl::Value v) {
    if (!o.state.set(*o.schema, path, std::move(v))) {
        std::printf("  [FAIL] %s.%s refused\n", o.schema->name, path);
        g_fail++;
    }
}

/// An NPC with a few of the values the retail ones have: the actor block and two components, small enough to read the log by.
static world::WorldActor make_npc(uint64_t id, double x, double y, const char* archetype = "Wolf") {
    world::WorldActor w;
    w.id = id;
    w.kind = world::ActorKind::Npc;
    w.x = x;
    w.y = y;
    w.z = 12000;
    repl::Actor& a = w.actor;
    a.guid = guid(id);
    a.obj.schema = &schema_named("ANPCCharacter");
    set_value(a.obj, "AuthServerIDReplicated", repl::Value::u64(60));
    repl::RepMovement m;
    m.location = repl::QuantVec{repl::QuantVec::Mode::Scaled, static_cast<int64_t>(x * 100), static_cast<int64_t>(y * 100), 1200000};
    m.rotation = repl::Rot{0, 64, 0};
    m.linear_velocity = repl::QuantVec{repl::QuantVec::Mode::Scaled, 0, 0, 0};
    set_value(a.obj, "ReplicatedMovement", repl::Value(m));
    set_value(a.obj, "RemoteRole", repl::Value::u64(1));
    set_value(a.obj, "Owner", repl::Value(guid(id + 2)));
    set_value(a.obj, "Role", repl::Value::u64(4));
    set_value(a.obj, "Controller", repl::Value(guid(id + 2)));
    set_value(a.obj, "ReplicatedMovementMode", repl::Value::u64(10));
    set_value(a.obj, "CurrentMovementInput", repl::Value(repl::PlayerMovementInput{0}));
    const std::string path = std::string("/Game/DesignAssets/Population/NPC/Beast/") + archetype + "/" + archetype + ".Default__" + archetype + "_C";
    a.open.archetype = {link(0xA000 + std::hash<std::string>{}(archetype) % 1000 * 2 + 1, (std::string("Default__") + archetype + "_C").c_str(), false, 0x1234),
                        link(0xB000 + std::hash<std::string>{}(archetype) % 1000 * 2 + 1, path.substr(0, path.rfind('.')).c_str(), false, 0)};
    a.open.level = aoc::worldd::bootstrap::persistent_level_export();
    a.open.spawn.location = repl::QuantVec{repl::QuantVec::Mode::Scaled, static_cast<int64_t>(x * 10), static_cast<int64_t>(y * 10), 120000};
    a.open.spawn.rotation = repl::Rot{0, 11000, 0};
    repl::Subobject info;
    info.guid = guid(id + 6);
    info.name = "BaseCharacterInfo";
    info.checksum = 0xaff9746f;
    info.obj.schema = &schema_named("UNPCInfo");
    set_value(info.obj, "CharacterGuid.A", repl::Value::i64(static_cast<int32_t>(id)));
    set_value(info.obj, "CreatureOwnerName", repl::Value::str(std::string("A ") + archetype));
    set_value(info.obj, "Level", repl::Value::i64(14));
    a.subobjects.push_back(std::move(info));
    repl::Subobject combat;
    combat.guid = guid(id + 8);
    combat.name = "CombatInfo";
    combat.checksum = 0xb9f1a7bc;
    combat.obj.schema = &schema_named("UNPCCombatInfo");
    set_value(combat.obj, "bIsDead", repl::Value::boolean(false));
    set_value(combat.obj, "EliteRank", repl::Value::i64(0));
    a.subobjects.push_back(std::move(combat));
    w.npc.name = std::string("A ") + archetype;
    w.npc.level = 14;
    w.npc.archetype = path;
    return w;
}

// ---- the registry ----------------------------------------------------------------------------------------

static void test_registry() {
    std::printf("the registry\n");
    world::ActorRegistry reg;
    CHECK(reg.add(make_npc(1000, 0, 0)) && reg.size() == 1, "an actor is added");
    CHECK(!reg.add(make_npc(1000, 5, 5)), "the same id twice is refused");
    CHECK(!reg.add(make_npc(1001, 0, 0)), "an odd id (a static object's) is refused");
    world::WorldActor bad = make_npc(1002, 0, 0);
    bad.id = 1004;
    CHECK(!reg.add(std::move(bad)), "an id that is not the actor's NetGUID is refused");
    world::WorldActor nan = make_npc(1006, 0, 0);
    nan.x = std::nan("");
    CHECK(!reg.add(std::move(nan)), "a position that is not a number is refused");
    CHECK(reg.add(make_npc(2000, 9999, 0)) && reg.add(make_npc(2002, 10001, 0)) && reg.add(make_npc(2004, -1, -1)) && reg.add(make_npc(2006, -10001, -10001)),
          "actors on either side of a cell edge and at negative coordinates");

    std::vector<uint64_t> ids;
    reg.query(0, 0, 10000, ids);
    std::set<uint64_t> got(ids.begin(), ids.end());
    CHECK(got == std::set<uint64_t>({1000, 2000, 2004}), "a 100 m query around the origin finds the three within it (9,999 m is in, 10,001 is out, -1 is in)");
    ids.clear();
    reg.query(10000, 0, 100, ids);
    CHECK(ids.size() == 2, "a small query across a cell edge finds both neighbours");
    ids.clear();
    reg.query(-10000, -10000, 2, ids);
    CHECK(ids.size() == 1 && ids[0] == 2006, "a query at negative coordinates");
    ids.clear();
    reg.query(0, 0, 1e12, ids);
    CHECK(ids.size() == reg.size(), "a radius larger than the world walks the grid whole");
    ids.clear();
    reg.query(std::nan(""), 0, 100, ids);
    reg.query(0, 0, -5, ids);
    CHECK(ids.empty(), "a query with no number in it finds nothing");
// [C-Y-B :: CoA]

    const uint32_t v = reg.find(1000)->version, rv = reg.find(1000)->reliable_version;
    CHECK(reg.move(1000, 25000, 0, 12000, 90) && reg.find(1000)->version == v + 1 && reg.find(1000)->reliable_version == rv, "a move counts as a change that need not be reliable");
    ids.clear();
    reg.query(0, 0, 10000, ids);
    CHECK(std::set<uint64_t>(ids.begin(), ids.end()) == std::set<uint64_t>({2000, 2004}), "the grid follows a move to another cell");
    ids.clear();
    reg.query(25000, 0, 100, ids);
    CHECK(ids.size() == 1 && ids[0] == 1000, "...and finds the actor at its new place");
    CHECK(reg.modify(1000, world::Change::State, [](world::WorldActor& a) { a.npc.level = 15; }) && reg.find(1000)->npc.level == 15 &&
              reg.find(1000)->version == v + 2 && reg.find(1000)->reliable_version == rv + 1,
          "a state change moves both counters");
    CHECK(!reg.modify(77, world::Change::State, [](world::WorldActor&) {}) && !reg.move(77, 0, 0, 0, 0) && !reg.move(1000, std::nan(""), 0, 0, 0), "an unknown actor or a bad position changes nothing");

    std::vector<uint64_t> told;
    const uint32_t h1 = reg.on_remove([&](const world::WorldActor& a) { told.push_back(a.id); });
    const uint32_t h2 = reg.on_remove([&](const world::WorldActor& a) { told.push_back(a.id + 1); });
    CHECK(reg.remove(2000) && told == std::vector<uint64_t>({2000, 2001}) && reg.find(2000) == nullptr && reg.size() == 4, "removing tells the listeners first, in order");
    reg.off_remove(h1);
    told.clear();
    CHECK(reg.remove(2002) && told == std::vector<uint64_t>({2003}) && !reg.remove(2002), "a listener taken off hears nothing; removing twice is refused");
    reg.off_remove(h2);
    CHECK(reg.count(world::ActorKind::Npc) == 3, "the count by kind");
}

// ---- relevancy ------------------------------------------------------------------------------------------

static void test_classify() {
    std::printf("relevancy rings\n");
    using world::ClientState;
    using world::Ring;
    const world::RelevancyConfig cfg;   // 150 m, 350 m, 10 m
    const auto c = [&](double d, ClientState prior, double awake = 0, bool never = false) { return world::classify(cfg, d, awake, never, prior); };
    CHECK(c(14999, ClientState::None) == Ring::Awake && c(15001, ClientState::None) == Ring::Snapshot && c(35001, ClientState::None) == Ring::Out, "the edges: 150 m and 350 m");
    CHECK(c(15500, ClientState::Awake) == Ring::Awake && c(16001, ClientState::Awake) == Ring::Snapshot && c(15500, ClientState::Dormant) == Ring::Snapshot,
          "an open actor keeps its ring 10 m past the edge; one that is not open does not get it");
    CHECK(c(35500, ClientState::Dormant) == Ring::Snapshot && c(36001, ClientState::Dormant) == Ring::Out && c(35500, ClientState::None) == Ring::Out,
          "a dormant one stays in the snapshot ring 10 m past its edge, a new one does not enter past it");
    CHECK(c(11901, ClientState::None, 11900) == Ring::Snapshot && c(11899, ClientState::None, 11900) == Ring::Awake, "a class with its own radius (119 m) uses it");
    CHECK(c(15001, ClientState::None, 0, true) == Ring::Out && c(15500, ClientState::Awake, 0, true) == Ring::Awake && c(16001, ClientState::Awake, 0, true) == Ring::Out,
          "an actor that is never dormant is awake or out, with the same hysteresis");
    CHECK(world::query_radius(cfg) == 36000 && world::query_radius(cfg, 50000) == 51000, "the registry query reaches the outermost ring and the largest class radius");
}

// ---- the view -----------------------------------------------------------------------------------------

static ActorView::Config view_config() { return ActorView::Config{}; }

static std::function<bool(uint32_t)> reserved_3_to_6() { return [](uint32_t i) { return i >= 3 && i <= 6; }; }

/// One pass at `now` with the player at (x, y); then the exchange with the client.
static void run(ActorView& v, Pair& p, double x, double y, int steps = 1, milliseconds dt = milliseconds(130)) {
    for (int i = 0; i < steps; ++i) {
        v.tick(p.s, x, y, p.t);
        p.step(&v, dt / 2);
    }
}

static std::vector<Seen> take(Pair& p) {
    std::vector<Seen> out;
    out.swap(p.seen);
    return out;
}

static void test_view_rings() {
    std::printf("the view: awake, snapshot and out\n");
    world::ActorRegistry reg;
    Pair p;
    ActorView v(reg, view_config(), reserved_3_to_6());
    reg.add(make_npc(1000, 10000, 0));    // 100 m: awake
    reg.add(make_npc(1002, 20000, 0));    // 200 m: snapshot
    reg.add(make_npc(1004, 40000, 0));    // 400 m: out
    run(v, p, 0, 0, 1);
    auto s = take(p);
    CHECK(v.channel_of(1000) == 7 && v.state_of(1000) == world::ClientState::Awake, "the first free channel is 7: 3-6 are the session's own");
    CHECK(v.state_of(1002) == world::ClientState::Snapshot && v.state_of(1004) == world::ClientState::None && v.tracked() == 2, "the snapshot is opened, the actor at 400 m is not even tracked");
    size_t opens = 0, closes = 0;
    for (const Seen& x : s) { opens += x.open ? 1 : 0; closes += x.close ? 1 : 0; }
    CHECK(opens == 2 && closes == 0, "two opens went out and no close yet (the Dormancy close is 134 ms after the open)");
    CHECK(v.stats().opens == 1 && v.stats().snapshots == 1, "counted as one open and one snapshot");

    p.t += milliseconds(10);   // 140 ms after the open (a step of the exchange is 130 ms)
    run(v, p, 0, 0, 1);
    s = take(p);
    closes = 0;
    uint32_t reason = 99, closed_channel = 0;
    for (const Seen& x : s) if (x.close) { ++closes; reason = x.reason; closed_channel = x.channel; }
    CHECK(closes == 1 && reason == kCloseDormancy && closed_channel == 8, "the snapshot is closed with Dormancy on its channel");
    CHECK(v.state_of(1002) == world::ClientState::Dormant && v.channel_of(1002) == 0 && v.state_of(1000) == world::ClientState::Awake, "it is dormant now; the awake one is unchanged");

    // no updates, no opens, as time goes on
    run(v, p, 0, 0, 20);
    s = take(p);
    CHECK(s.empty(), "nothing is sent while nothing changes");
    // E1 review A2: a dormant actor that only MOVED is not sent again (a walking NPC moves its version every 67 ms step; its position comes whole
    // when it wakes); one whose state changed (a reliable change) gets a new snapshot, closed with Dormancy again
    reg.move(1002, 20100, 0, reg.find(1002)->z, 45);
    reg.move(1002, 20200, 0, reg.find(1002)->z, 45);
    run(v, p, 0, 0, 5);
    s = take(p);
    CHECK(s.empty() && v.stats().snapshots == 1 && v.state_of(1002) == world::ClientState::Dormant, "a dormant actor that walked within the snapshot ring is not sent again");
    reg.modify(1002, world::Change::State, [](world::WorldActor& a) { a.npc.level = 9; });
    run(v, p, 0, 0, 1);
    s = take(p);
    opens = 0;
    for (const Seen& x : s) opens += x.open ? 1 : 0;
    CHECK(opens == 1 && v.stats().snapshots == 2 && v.stats().wakes == 0 && v.state_of(1002) == world::ClientState::Snapshot, "a dormant actor whose state changed is sent as a new snapshot (not a wake)");
    p.t += milliseconds(10);
    run(v, p, 0, 0, 3);
    s = take(p);
    closes = 0;
    for (const Seen& x : s) closes += x.close ? 1 : 0;
    CHECK(closes == 1 && v.state_of(1002) == world::ClientState::Dormant && v.channel_of(1002) == 0, "...and closed with Dormancy 134 ms later");

    // walk to the dormant actor: it opens again, awake, on a free channel, with the whole state
    run(v, p, 12000, 0, 1);   // now 80 m from 1002, 120 m from 1000
    s = take(p);
    // (the actor at 400 m, 280 m away now, got its snapshot too)
    const uint32_t reopened = v.channel_of(1002);
    size_t reopen_bits = 0;
    for (const Seen& x : s) if (x.open && x.channel == reopened) reopen_bits = x.bits;
    CHECK(reopened != 0 && v.state_of(1002) == world::ClientState::Awake && v.stats().wakes == 1 && reopen_bits > 0, "walking up to a dormant actor opens it again");
    CHECK(v.state_of(1004) == world::ClientState::Snapshot, "the actor that came into the snapshot ring got its snapshot");
    CHECK(reopened == 8, "the index of the Dormancy close was acknowledged, so the lowest free channel is that one");
    // the same bits as the first snapshot: from an empty shadow
    world::WorldActor* a = reg.find(1002);
    repl::ActorShadow fresh;
    repl::ExportAcks all;
    auto em = repl::open_actor(a->actor, fresh, all);
    size_t expect_bits = 0;
    if (em) {
        expect_bits = em->payload.bit_pos();
        for (const auto& x : em->exports) expect_bits += x.bit_pos();
    }
    auto em_acked = repl::open_actor(a->actor, fresh, v.export_acks());
    size_t expect_acked = 0;
    if (em_acked) {
        expect_acked = em_acked->payload.bit_pos();
        for (const auto& x : em_acked->exports) expect_acked += x.bit_pos();
    }
    CHECK(em && em_acked && (reopen_bits == expect_acked || reopen_bits == expect_bits) && expect_acked <= expect_bits,
          "the re-open is the whole state again: the bits of an open from an empty shadow (with the exports the client acknowledged left out)");
// [C O A ~ Cyb]

    // 1000 is at x = 10000: from x = -20000 it is 300 m away, in the snapshot ring
    run(v, p, -20000, 0, 1);
    s = take(p);
    reason = 99;
    for (const Seen& x : s) if (x.close && x.channel == 7) reason = x.reason;
    CHECK(v.state_of(1000) == world::ClientState::Dormant && reason == kCloseDormancy, "an awake actor that leaves the awake ring is closed with Dormancy: the client keeps it");
    run(v, p, -20000, 0, 3);   // (the snapshot of the farthest actor, opened on the way, closes with Dormancy)
    take(p);
    run(v, p, -50000, 0, 1);   // 600 m from 1000
    s = take(p);
    CHECK(v.state_of(1000) == world::ClientState::None && s.empty() && v.stats().forgotten >= 1, "a dormant one that leaves the snapshot ring is forgotten with no message (the client keeps the actor)");
    // and coming back
    run(v, p, 10000, 0, 1);
    CHECK(v.state_of(1000) == world::ClientState::Awake, "coming back opens it again");
}

static void test_wide_actor_leaves_the_query_alone() {
    std::printf("the view: a wide actor (a town's replicator) does not widen the registry query\n");
    world::ActorRegistry reg;
    Pair p;
    ActorView v(reg, view_config(), reserved_3_to_6());
    const world::RelevancyConfig cfg = view_config().relevancy;
    CHECK(v.query_radius_cm() == world::query_radius(cfg), "(a new view asks the registry for the snapshot ring and its hysteresis)");
    world::WorldActor wide = make_npc(1000, 100000, 0);   // 1 km away
    wide.awake_radius_cm = 145000;                        // 1.45 km: Nodes 1's replicator of a town
    reg.add(std::move(wide));
    reg.add(make_npc(1010, 10000, 0));
    run(v, p, 0, 0, 3);
    CHECK(reg.wide_count() == 1 && v.state_of(1000) == world::ClientState::Awake, "the wide actor is found at 1 km (query_wide) and awake");
    CHECK(v.query_radius_cm() == world::query_radius(cfg), "having seen it leaves the grid query at 360 m (a 1.46 km query would visit a quarter of the world's cells for good)");
    world::WorldActor big = make_npc(1020, 30000, 0);
    big.awake_radius_cm = 38000;   // a large creature: under the wide limit, so the grid has to reach it
    reg.add(std::move(big));
    run(v, p, 0, 0, 3);
    CHECK(v.query_radius_cm() == world::query_radius(cfg, 38000), "an actor of 380 m still raises it (only the grid finds that one)");
}

static void test_view_updates() {
    std::printf("the view: updates, reliability, hysteresis\n");
    world::ActorRegistry reg;
    Pair p;
    ActorView v(reg, view_config(), reserved_3_to_6());
    reg.add(make_npc(1000, 5000, 0));
    run(v, p, 0, 0, 2);
    take(p);
    const uint32_t ch = v.channel_of(1000);
    CHECK(ch == 7, "opened on 7");
    // a state change: only the changed handle goes out, reliably
    reg.modify(1000, world::Change::State, [](world::WorldActor& a) {
        for (repl::Subobject& s : a.actor.subobjects) {
            if (std::string(s.name) == "CombatInfo") s.obj.state.set(*s.obj.schema, "bIsDead", repl::Value::boolean(true));
        }
    });
    run(v, p, 0, 0, 2);
    auto s = take(p);
    CHECK(s.size() == 1 && s[0].channel == ch && !s[0].open && s[0].reliable && s[0].bits < 200, "a state change is one small reliable update on the actor's channel");
    CHECK(v.stats().updates == 1, "counted");
    // a position change alone: unreliable
    reg.move(1000, 5100, 0, 12000, 0);
    reg.modify(1000, world::Change::Movement, [](world::WorldActor& a) {
        repl::RepMovement m;
        m.location = repl::QuantVec{repl::QuantVec::Mode::Scaled, 5100 * 100, 0, 1200000};
        m.rotation = repl::Rot{0, 64, 0};
        m.linear_velocity = repl::QuantVec{repl::QuantVec::Mode::Scaled, 100, 0, 0};
        a.actor.obj.state.set(*a.actor.obj.schema, "ReplicatedMovement", repl::Value(m));
    });
    run(v, p, 0, 0, 2);
    s = take(p);
    CHECK(s.size() == 1 && !s[0].reliable && !s[0].open && s[0].bits < 400 && v.stats().updates_unreliable == 1, "a movement alone is an unreliable update");
    // nothing changed: nothing sent
    run(v, p, 0, 0, 5);
    CHECK(take(p).empty(), "an unchanged actor sends nothing");
    // a version bump with no value change sends nothing (the version is only a hint)
    reg.touch(*reg.find(1000), world::Change::State);
    run(v, p, 0, 0, 2);
    CHECK(take(p).empty() && v.stats().updates == 1, "a change that left every value as it was sends nothing");

    // hysteresis at the edge: the player pacing across 150 m does not make the actor flap
    reg.add(make_npc(1002, 15000, 0));
    size_t flaps = 0;
    run(v, p, 0, 0, 2);
    take(p);
    for (int i = 0; i < 60; ++i) {
        const double x = (i % 2) ? -400.0 : 400.0;   // 148 m and 154 m from the actor
        run(v, p, x, 0, 1);
        for (const Seen& e : take(p)) flaps += (e.open || e.close) ? 1 : 0;
    }
    CHECK(flaps <= 2, "pacing across the edge of the awake ring (148 m, 154 m) opens and closes at most once");
}

static void test_view_never_dormant_and_removal() {
    std::printf("the view: a never-dormant actor, removal, channels held\n");
    world::ActorRegistry reg;
    Pair p;
    ActorView v(reg, view_config(), reserved_3_to_6());
    world::WorldActor fx = make_npc(3000, 5000, 0, "Effect");
    fx.kind = world::ActorKind::Effect;
    fx.never_dormant = true;
    reg.add(std::move(fx));
    run(v, p, 0, 0, 2);
    take(p);
    CHECK(v.state_of(3000) == world::ClientState::Awake, "a never-dormant actor in range is awake");
    run(v, p, 30000, 0, 1);   // 250 m
    auto s = take(p);
    uint32_t reason = 99;
    for (const Seen& x : s) if (x.close) reason = x.reason;
    CHECK(v.state_of(3000) == world::ClientState::None && reason == kCloseRelevancy, "it leaves with a Relevancy close, never a Dormancy one");
    run(v, p, 0, 0, 1);
    CHECK(v.state_of(3000) == world::ClientState::Awake, "and opens again when the player returns");

    // a channel is held until its close is acknowledged: the close is queued but the packet is not flushed yet
    world::ActorRegistry reg2;
    Pair p2;
    ActorView v2(reg2, view_config(), reserved_3_to_6());
    reg2.add(make_npc(1000, 5000, 0));
    reg2.add(make_npc(1002, 90000, 0));
    v2.tick(p2.s, 0, 0, p2.t);
    p2.step(&v2);
    CHECK(v2.channel_of(1000) == 7, "the first actor is on 7");
    v2.tick(p2.s, 100000, 0, p2.t + milliseconds(100));   // the first is 950 m away: Relevancy close (queued, not flushed)
    CHECK(p2.s.channel_closing(7) && v2.channel_of(1000) == 0, "the close is queued: the channel is closing");
    // a new actor appears next to the player: the lowest free index must not be 7 while its close is not acknowledged
    reg2.add(make_npc(1004, 99000, 0));
    v2.tick(p2.s, 100000, 0, p2.t + milliseconds(200));
    CHECK(v2.channel_of(1004) != 7 && v2.channel_of(1004) >= 8, "a closing channel's index is not reused until the close is acknowledged");
    p2.step(&v2);
    p2.step(&v2);
    CHECK(!p2.s.channel_closing(7), "the acknowledgement frees it");
    reg2.add(make_npc(1006, 99500, 0));
    v2.tick(p2.s, 100000, 0, p2.t + milliseconds(400));
    CHECK(v2.channel_of(1006) == 7, "and the next open takes it: the lowest free index");
// ~ C:Y:B & C O A ~

    // removal: an open actor is closed with Destroyed, a dormant one is opened and closed
    world::ActorRegistry reg3;
    Pair p3;
    ActorView v3(reg3, view_config(), reserved_3_to_6());
    reg3.add(make_npc(1000, 5000, 0));
    reg3.add(make_npc(1002, 20000, 0));
    run(v3, p3, 0, 0, 5);
    take(p3);
    CHECK(v3.state_of(1000) == world::ClientState::Awake && v3.state_of(1002) == world::ClientState::Dormant, "an awake actor and a dormant one");
    reg3.remove(1000);
    run(v3, p3, 0, 0, 1);
    s = take(p3);
    reason = 99;
    for (const Seen& x : s) if (x.close) reason = x.reason;
    CHECK(reason == kCloseDestroyed && v3.tracked() == 1 && v3.stats().closes_destroyed == 1, "an awake actor that is removed is closed with Destroyed");
    reg3.remove(1002);
    run(v3, p3, 0, 0, 2);
    s = take(p3);
    size_t opens = 0, destroyed = 0;
    for (const Seen& x : s) { opens += x.open ? 1 : 0; destroyed += (x.close && x.reason == kCloseDestroyed) ? 1 : 0; }
    CHECK(opens == 1 && destroyed == 1 && v3.tracked() == 0, "a dormant one is opened once more and closed with Destroyed so that the client forgets it");

    // a view that finishes stops listening
    ActorView* v4 = new ActorView(reg3, view_config(), reserved_3_to_6());
    delete v4;
    reg3.add(make_npc(1010, 0, 0));
    CHECK(reg3.remove(1010), "a finished view leaves no listener behind");
}

static void test_view_cap_and_exports() {
    std::printf("the view: the cap on opens, nearest first; exports\n");
    world::ActorRegistry reg;
    Pair p;
    ActorView::Config cfg = view_config();
    ActorView v(reg, cfg, reserved_3_to_6());
    for (int i = 0; i < 100; ++i) reg.add(make_npc(10000 + i * 100, 5000 + i * 100, 0));   // 50 m to 150 m
    v.tick(p.s, 0, 0, p.t);
    CHECK(v.stats().opens == 16 && v.stats().capped == 1, "the first pass opens 16 of 100 (the cap)");
    bool nearest = true;
    for (int i = 0; i < 100; ++i) nearest = nearest && ((i < 16) == (v.state_of(10000 + i * 100) == world::ClientState::Awake));
    CHECK(nearest, "the 16 are the nearest");
    for (int i = 0; i < 7; ++i) run(v, p, 0, 0, 1);
    CHECK(v.stats().opens == 100 && v.count(world::ClientState::Awake) == 100, "seven more passes open the rest");
    CHECK(v.channels_in_use() == 100, "100 channels in use");
    std::set<uint32_t> channels;
    for (int i = 0; i < 100; ++i) channels.insert(v.channel_of(10000 + i * 100));
    CHECK(channels.size() == 100 && *channels.begin() == 7 && *channels.rbegin() == 106, "they are 7 to 106, each once");

    // exports: the same archetype is exported until acknowledged, then not
    world::ActorRegistry reg2;
    Pair p2;
    ActorView v2(reg2, view_config(), reserved_3_to_6());
    reg2.add(make_npc(1000, 5000, 0, "Wolf"));
    reg2.add(make_npc(1002, 6000, 0, "Wolf"));
    std::vector<size_t> bits;
    v2.set_trace([&](uint32_t, bool open, bool, size_t b) { if (open) bits.push_back(b); });
    v2.tick(p2.s, 0, 0, p2.t);   // both opens in one pass: neither export was acknowledged
    CHECK(bits.size() == 2 && !v2.export_acks().acked(reg2.find(1000)->actor.open.archetype[0].guid), "both opens of the first pass carry the archetype's export (nothing acknowledged yet)");
    p2.step(&v2);
    p2.step(&v2);
    CHECK(v2.export_acks().acked(reg2.find(1000)->actor.open.archetype[0].guid), "the archetype is marked once the client acknowledged the packet");
    const size_t first_bits = bits[0];
    reg2.add(make_npc(1004, 7000, 0, "Wolf"));
    bits.clear();
    run(v2, p2, 0, 0, 1);
    CHECK(bits.size() == 1 && bits[0] < first_bits, "a later open of the same archetype is smaller: the export the client acknowledged is left out");
}

static void test_view_owner_and_component_index() {
    std::printf("the view: a player's own actors; the registry's component index\n");
    // an effect owned by player 7 is for player 7's connection only, and its open carries the export the actor names (the pawn's mesh)
    for (const uint64_t player : {7ull, 8ull}) {
        world::ActorRegistry reg;
        Pair p;
        ActorView::Config cfg = view_config();
        cfg.player_id = player;
        ActorView v(reg, cfg, reserved_3_to_6());
        world::WorldActor fx = make_npc(3000, 5000, 0, "Effect");
        fx.kind = world::ActorKind::Effect;
        fx.never_dormant = true;
        fx.owner = 7;
        fx.open_exports.push_back({link(0xC001, "CharacterMesh0", false, 0x77aa)});
        reg.add(std::move(fx));
        run(v, p, 0, 0, 2);
        const auto s = take(p);
        if (player == 7) {
            CHECK(v.state_of(3000) == world::ClientState::Awake && !s.empty() && s[0].open, "player 7's effect is opened for player 7");
// C-Y-B x CoA

        } else {
            CHECK(v.state_of(3000) == world::ClientState::None && s.empty() && v.tracked() == 0, "...and never sent to player 8");
        }
    }
    // the export is in the chain: a chain with the mesh export has an export bunch more than the same actor without
    {
        size_t with = 0, without = 0;
        for (const bool has : {true, false}) {
            world::ActorRegistry reg;
            Pair p;
            ActorView v(reg, view_config(), reserved_3_to_6());
            world::WorldActor fx = make_npc(3000, 5000, 0, "Effect");
            fx.kind = world::ActorKind::Effect;
            fx.never_dormant = true;
            if (has) fx.open_exports.push_back({link(0xC001, "CharacterMesh0", false, 0x77aa)});
            reg.add(std::move(fx));
            std::vector<size_t> bits;
            v.set_trace([&](uint32_t, bool open, bool, size_t b) { if (open) bits.push_back(b); });
            v.tick(p.s, 0, 0, p.t);
            (has ? with : without) = bits.empty() ? 0 : bits[0];
        }
        CHECK(with > without && without > 0, "an actor with an export to carry has a longer open than the same actor without");
    }
    // the component index
    world::ActorRegistry reg;
    world::WorldActor a = make_npc(1000, 0, 0);
    a.npc.interact_component = 1016;
    world::WorldActor b = make_npc(1100, 0, 0);
    b.npc.interact_component = 1016;
    world::WorldActor c = make_npc(1200, 0, 0);
    c.npc.interact_component = 1216;
    CHECK(reg.add(std::move(a)) && !reg.add(std::move(b)) && reg.add(std::move(c)), "two actors cannot claim one component");
    CHECK(reg.find_by_component(1016) && reg.find_by_component(1016)->id == 1000 && reg.find_by_component(1216)->id == 1200 && !reg.find_by_component(1017), "a component finds its actor");
    reg.remove(1000);
    CHECK(!reg.find_by_component(1016) && reg.find_by_component(1216), "a removed actor's component is gone");
    world::WorldActor d = make_npc(1300, 0, 0);
    d.npc.interact_component = 1016;
    CHECK(reg.add(std::move(d)) && reg.find_by_component(1016)->id == 1300, "and can be claimed again");
}

int main() {
    test_view_owner_and_component_index();
    test_registry();
    test_classify();
    test_view_rings();
    test_wide_actor_leaves_the_query_alone();
    test_view_updates();
    test_view_never_dormant_and_removal();
    test_view_cap_and_exports();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
