// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/actor_view_scan_tests.inc: the relevancy scan of a view (ActorView::pass)
//
//  Every 67 ms the view of every player looks at the actors round the player: the grid's, the wide ones and the ones it already holds, each with its distance, nearest first (a tie
//  goes by id).  That order is part of what the client is sent: the cap on opens takes the nearest, and a channel's number follows the order of the opens.
//
//  1. A fixed scene on a simulated clock: about 700 creatures round a player who walks an octagon and is moved 200 m once, creatures added, removed and moving, a town-sized actor,
//     creatures that are only ever snapshots, another player's effects (skipped), and mirrored pairs that are exactly as far as each other.  Every datagram the server sends is
//     folded into one 64-bit digest (FNV-1a).  The digest was taken from the scan that sorted the ids and the candidates in full at every pass; the scan that keeps the last
//     pass's order must send the same bytes.  (A change of the replicator's wire form on purpose changes the digest: the failing check prints the new one.)
//  2. The ordering helper of the scan (session/scan_order.h) against std::sort: lists of every size with many equal distances, started from the order of an earlier list that the
//     distances have moved away from by a little or by a lot, with actors that are new.
//  3. The scan's cost: the time of a pass at 650 creatures with the player standing and walking, for one view and for 16 that share the machine's cache, printed, with generous bounds (a
//     PC that is also building).  It is the work of the server for the views, not a count of players.
// ============================================================================
#include "net/tick_profile.h"
#include "session/scan_order.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace scan_checks {

struct Lcg {
    uint64_t s;
    uint32_t next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<uint32_t>(s >> 33);
    }
    /// 0 .. n-1
    int64_t below(int64_t n) { return static_cast<int64_t>(next()) % n; }
};

struct Fnv {
    uint64_t h = 14695981039346656037ull;
    void add(const uint8_t* p, size_t n) {
        for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    }
    void add(uint64_t v) {
        for (int i = 0; i < 8; ++i) h = (h ^ ((v >> (8 * i)) & 0xff)) * 1099511628211ull;
    }
};

/// `count` creatures from `first_id` on (10 apart: an actor's id and its subobjects'), at whole centimetres round (cx, cy): two in five within the awake ring (140 m), the rest out to
/// the edge of the snapshot ring (350 m).  Integers only, so that the scene is the same on every machine.
inline void add_creatures(world::ActorRegistry& reg, int count, Lcg& rng, uint64_t first_id, int64_t cx = 0, int64_t cy = 0) {
    for (int i = 0; i < count; ++i) {
        const int64_t r_min = (i % 5 < 2) ? 0 : 14000, r_max = (i % 5 < 2) ? 14000 : 35000;
        int64_t x = 0, y = 0;
        for (;;) {
            x = rng.below(2 * r_max + 1) - r_max;
            y = rng.below(2 * r_max + 1) - r_max;
            const int64_t d2 = x * x + y * y;
            if (d2 >= r_min * r_min && d2 <= r_max * r_max) break;
        }
        reg.add(make_npc(first_id + 10 * static_cast<uint64_t>(i), static_cast<double>(cx + x), static_cast<double>(cy + y), i % 3 == 0 ? "Wolf" : "Boar"));
    }
}

/// A creature takes a step: its place and its replicated movement change (an unreliable update for the views that hold it awake).
inline void step_creature(world::ActorRegistry& reg, uint64_t id, int step, int k) {
    const world::WorldActor* a = reg.find(id);
    if (!a) return;
    const double x = a->x + ((step + k) % 2 ? 12.0 : -12.0), y = a->y + 7.0;
    reg.move(id, x, y, a->z, static_cast<float>((step * 13) % 360));
    reg.modify(id, world::Change::Movement, [x, y](world::WorldActor& w) {
        repl::RepMovement m;
        m.location = repl::QuantVec{repl::QuantVec::Mode::Scaled, static_cast<int64_t>(x * 100), static_cast<int64_t>(y * 100), 1200000};
        m.rotation = repl::Rot{0, 64, 0};
        m.linear_velocity = repl::QuantVec{repl::QuantVec::Mode::Scaled, 100, 0, 0};
        w.actor.obj.state.set(*w.actor.obj.schema, "ReplicatedMovement", repl::Value(m));
    });
}

/// The player walks an octagon (eight legs of `leg` steps, 42 cm a step: 6 m a second at 14 passes a second).
inline void walk_one(int64_t& x, int64_t& y, int step, int leg) {
    static const int dx[8] = {42, 30, 0, -30, -42, -30, 0, 30};
    static const int dy[8] = {0, 30, 42, 30, 0, -30, -42, -30};
    const int d = (step / leg) % 8;
    x += dx[d];
    y += dy[d];
}

/// One step of the exchange: the server flushes, the client receives and answers (when `deliver`: a silent client lets the connection fill up); every datagram of the server is folded into the digest.
inline void exchange(Pair& p, ActorView& v, Fnv& fnv, milliseconds dt, bool deliver = true) {
    p.t += dt;
    const auto pkts = p.s.flush(p.t);
    p.packets += pkts.size();
    fnv.add(static_cast<uint64_t>(pkts.size()));
    for (const auto& pk : pkts) {
        fnv.add(pk.data(), pk.size());
        if (deliver) p.cl.receive(pk, p.t);
    }
    p.t += dt;
    if (deliver) {
        for (const auto& pk : p.cl.flush(p.t)) p.s.receive(pk, p.t);
    }
    for (const AckEvent& e : p.s.take_ack_events()) v.on_ack(e);
}

struct Result {
    uint64_t digest = 0;
    size_t   packets = 0;      // datagrams the server sent
    size_t   passes = 0;       // passes the view made
    size_t   opens = 0, snapshots = 0, wakes = 0, forgotten = 0, closes = 0;
    size_t   updates = 0;      // update chains the view sent (reliable and not)
    size_t   throttled = 0;    // passes (or the rest of one) the view left to a busy connection
};

/// The fixed scene (see the top of the file).  `paced`: the view's pacing is tight (20 packets in flight, 24 queued bunches) and the client is silent for two stretches, so that passes are held
/// back and many end half way (the order of the last pass is then only partly renewed); not `paced`: the pacing is off and the client answers throughout.
inline Result run_scan_scene(bool paced) {
    world::ActorRegistry reg;
    Pair p;
    ActorView::Config cfg = view_config();
    cfg.max_packets_in_flight = paced ? 20 : 1000000;
    cfg.max_queued_bunches = paced ? 24 : 1000000;
    ActorView v(reg, cfg, reserved_3_to_6());
    Lcg rng{0x5ca17e57ull};
    constexpr int kCreatures = 600, kMirrored = 60;
    add_creatures(reg, kCreatures, rng, 1000);
    // mirrored pairs: the same distance from the origin to the last centimetre, so that the first passes of a player at the origin order them by id alone
    for (int i = 0; i < kMirrored; ++i) {
        const world::WorldActor* a = reg.find(1000 + 10 * static_cast<uint64_t>(i));
        reg.add(make_npc(20000 + 10 * static_cast<uint64_t>(i), -a->x, -a->y, "Wolf"));
    }
    world::WorldActor town = make_npc(9000, 8000, 3000);   // a town's replicator: 1.45 km, found by the wide query
    town.awake_radius_cm = 145000;
    reg.add(std::move(town));
    world::WorldActor big = make_npc(9010, -12000, 9000);   // a large creature: 380 m
    big.awake_radius_cm = 38000;
    reg.add(std::move(big));
    for (int i = 0; i < 4; ++i) {   // snapshots wherever they are in range
        world::WorldActor w = make_npc(9020 + 10 * static_cast<uint64_t>(i), 3000.0 + 1500.0 * i, -2000);
        w.snapshot_only = true;
        reg.add(std::move(w));
    }
    for (int i = 0; i < 5; ++i) {   // another player's lingering effects: skipped
        world::WorldActor w = make_npc(9100 + 10 * static_cast<uint64_t>(i), -1500.0 * i, 2500);
        w.owner = 77;
        reg.add(std::move(w));
    }
    Fnv fnv;
    int64_t px = 0, py = 0;
    constexpr int kSteps = 960, kLeg = 120;
    constexpr int kStill = 40;   // (the player stands at the origin while the first 640 actors open, 16 a pass: the mirrored pairs are tied)
    for (int step = 0; step < kSteps; ++step) {
        if (step >= kStill) {
            walk_one(px, py, step - kStill, kLeg);
            for (int k = 0; k < 40; ++k) step_creature(reg, 1000 + 10 * static_cast<uint64_t>((step * 7 + k * 13) % kCreatures), step, k);
        }
        if (step == 700) {   // a jump of 200 m: the order of the last pass says nothing about this one
            px = -18000;
            py = 12000;
        }
        if (step == 300) add_creatures(reg, 20, rng, 30000, px, py);   // new ones round the player
        if (step == 500) {
            for (int i = 100; i < 115; ++i) reg.remove(1000 + 10 * static_cast<uint64_t>(i));
        }
        v.tick(p.s, static_cast<double>(px), static_cast<double>(py), p.t);
        const bool silent = paced && ((step >= 100 && step < 140) || (step >= 520 && step < 545));
        exchange(p, v, fnv, milliseconds(35), !silent);
    }
    Result r;
    r.digest = fnv.h;
    r.packets = p.packets;
    r.passes = v.stats().passes;
    r.opens = v.stats().opens;
    r.snapshots = v.stats().snapshots;
    r.wakes = v.stats().wakes;
    r.forgotten = v.stats().forgotten;
    r.closes = v.stats().closes_dormancy + v.stats().closes_relevancy + v.stats().closes_destroyed;
    r.updates = v.stats().updates + v.stats().updates_unreliable;
    r.throttled = v.stats().throttled;
    return r;
}

struct Item {
    double   d;
    uint64_t id;
};

inline bool item_nearer(const Item& l, const Item& r) { return l.d != r.d ? l.d < r.d : l.id < r.id; }

/// sort_after_small_changes against std::sort over many lists; returns how many lists were tried and counts those that came out in another order.
inline size_t check_scan_order(size_t& wrong) {
    Lcg rng{0x0dd5ull};
    std::vector<Item> spare;
    size_t cases = 0;
    const size_t sizes[] = {0, 1, 2, 3, 7, 8, 9, 10, 25, 100, 650};
    for (int round = 0; round < 12; ++round) {
        for (const size_t n : sizes) {
            for (const int64_t moved_cm : {0, 40, 400, 100000}) {   // how far the distances moved since the earlier list (the last: nothing is where it was)
                for (const size_t fresh : {size_t{0}, size_t{1}, size_t{5}, size_t{9}, n / 3}) {
                    if (fresh > n) continue;
                    const size_t held = n - fresh;
                    // the earlier list: distances in whole metres (many equal), in order
                    std::vector<Item> list;
                    for (size_t i = 0; i < held; ++i) list.push_back({static_cast<double>(rng.below(2000) * 100), 1000 + 10 * static_cast<uint64_t>(i)});
                    std::sort(list.begin(), list.end(), item_nearer);
                    // now: each distance has moved (and is a whole metre again, so that ties stay common), then the new actors, in no order
                    for (Item& it : list) {
                        const int64_t d = static_cast<int64_t>(it.d) + rng.below(2 * moved_cm + 1) - moved_cm;
                        it.d = static_cast<double>((d < 0 ? 0 : d) / 100 * 100);
                    }
                    for (size_t i = 0; i < fresh; ++i) list.push_back({static_cast<double>(rng.below(2000) * 100), 50000 + 10 * static_cast<uint64_t>(i)});
                    std::vector<Item> expect = list;
                    std::sort(expect.begin(), expect.end(), item_nearer);
                    sort_after_small_changes(list, held, item_nearer, spare);
                    ++cases;
                    bool same = list.size() == expect.size();
                    for (size_t i = 0; same && i < list.size(); ++i) same = list[i].id == expect[i].id && list[i].d == expect[i].d;
                    if (!same) ++wrong;
                }
            }
        }
    }
    return cases;
}

struct Cost {
    double scan_us = 0;     // the scan of a pass (registry query, rings, ordering): the median of five stretches
    double tick_us = 0;     // the whole tick of the view: the same, with the opens, updates and closes of the pass
    size_t tracked = 0;     // actors the view holds
};

/// One player of the cost scene: a view with a connection and a client of its own.
struct Viewer {
    Pair p;
    Fnv  fnv;
    ActorView v;
    Viewer(world::ActorRegistry& reg, const ActorView::Config& cfg) : v(reg, cfg, reserved_3_to_6()) {}
};

/// The time of a pass at 650 creatures, 40 of them walking, that `views` players hold together (they stand about one place, as test bots do, and their views share the cache of the
/// machine as they do on a server): with the players `walking` or standing.  Five stretches of 60 passes; the median of each part.
inline Cost run_scan_cost(bool walking, int views) {
    world::ActorRegistry reg;
    ActorView::Config cfg = view_config();
    cfg.max_packets_in_flight = 1000000;
    cfg.max_queued_bunches = 1000000;
    std::vector<std::unique_ptr<Viewer>> viewers;
    for (int i = 0; i < views; ++i) viewers.push_back(std::make_unique<Viewer>(reg, cfg));
    Lcg rng{0xbe5c4ull};
    constexpr int kCreatures = 650;
    add_creatures(reg, kCreatures, rng, 1000);
    int64_t px = 0, py = 0;
    int step = 0;
    const auto tick_all = [&]() {
        for (size_t i = 0; i < viewers.size(); ++i) {
            Viewer& w = *viewers[i];
            w.v.tick(w.p.s, static_cast<double>(px + 300 * static_cast<int64_t>(i)), static_cast<double>(py), w.p.t);
            exchange(w.p, w.v, w.fnv, milliseconds(35));
        }
    };
    for (; step < 120; ++step) tick_all();   // the opens: 16 a pass
    TickProfile& profile = tick_profile();
    profile.on = true;
    std::vector<double> scan, tick;
    for (int stretch = 0; stretch < 5; ++stretch) {
        profile.restart();
        for (int i = 0; i < 60; ++i, ++step) {
            if (walking) walk_one(px, py, step, 40);
            for (int k = 0; k < 40; ++k) step_creature(reg, 1000 + 10 * static_cast<uint64_t>((step * 7 + k * 13) % kCreatures), step, k);
            tick_all();
        }
        if (profile.view_passes > 0) {
            scan.push_back(profile.view_scan_us / static_cast<double>(profile.view_passes));
            tick.push_back(profile.view_us / static_cast<double>(profile.view_passes));
        }
    }
    profile.on = false;
    profile.restart();
    Cost c;
    if (!scan.empty()) {
        std::sort(scan.begin(), scan.end());
        std::sort(tick.begin(), tick.end());
        c.scan_us = scan[scan.size() / 2];
        c.tick_us = tick[tick.size() / 2];
    }
    c.tracked = viewers.empty() ? 0 : viewers.front()->v.tracked();
    return c;
}

}  // namespace scan_checks

void test_view_scan_sends_the_same_bytes() {
    using namespace scan_checks;
    std::printf("the view: the relevancy scan sends the same bytes whichever way it orders the actors\n");
    char text[400];
    for (const bool paced : {false, true}) {
        const Result a = run_scan_scene(paced);
        const Result b = run_scan_scene(paced);
        std::snprintf(text, sizeof(text), "%s: the scene is repeatable: two runs send the same datagrams", paced ? "paced" : "unpaced");
        CHECK(a.digest == b.digest && a.packets == b.packets, text);
        std::snprintf(text, sizeof(text), "...and it is a real one: %zu passes, %zu opens, %zu snapshots, %zu wakes, %zu closes, %zu forgotten, %zu update chains, %zu datagrams, %zu passes held back",
                      a.passes, a.opens, a.snapshots, a.wakes, a.closes, a.forgotten, a.updates, a.packets, a.throttled);
        CHECK(a.passes >= 900 && a.opens > 100 && a.snapshots > 300 && a.wakes > 20 && a.closes > 100 && a.forgotten > 20 && a.updates > 100 && (!paced || a.throttled > 100), text);
        // (taken from the scan that sorted the ids and the candidates in full at every pass)
        constexpr uint64_t kDigest = 0x84fd4b8c384e6db3ull, kPacedDigest = 0xe6ba2049b1aaadd6ull;
        const uint64_t held = paced ? kPacedDigest : kDigest;
        std::snprintf(text, sizeof(text), "%s: the datagrams of the scene fold to 0x%016llx; held: 0x%016llx", paced ? "paced" : "unpaced", static_cast<unsigned long long>(a.digest),
                      static_cast<unsigned long long>(held));
        CHECK(a.digest == held, text);
    }
}
AOC_REGISTER_TEST(test_view_scan_sends_the_same_bytes);

void test_view_scan_order_is_the_order_of_a_full_sort() {
    using namespace scan_checks;
    std::printf("the view: the scan's ordering starts from the last pass's order and gives the order a full sort gives\n");
    size_t wrong = 0;
    const size_t cases = check_scan_order(wrong);
    char text[300];
    std::snprintf(text, sizeof(text), "%zu lists of 0 to 650 actors (ties on the distance, new actors, distances moved by 0 cm to 1 km): %zu came out in another order than std::sort's", cases, wrong);
    CHECK(cases > 2000 && wrong == 0, text);
}
AOC_REGISTER_TEST(test_view_scan_order_is_the_order_of_a_full_sort);

void test_view_scan_cost() {
    using namespace scan_checks;
    std::printf("the view: what one pass costs at 650 creatures (the work of the server for one view; not a count of players)\n");
    char text[400];
    for (const int views : {1, 16}) {
        const Cost standing = run_scan_cost(false, views);
        const Cost walking = run_scan_cost(true, views);
        std::snprintf(text, sizeof(text), "%d view(s), the players standing: a pass scans in %.1f us of a tick of %.1f us (a view holds %zu actors)", views, standing.scan_us, standing.tick_us, standing.tracked);
        CHECK(standing.tracked >= 600 && standing.scan_us > 0 && standing.scan_us < 5000, text);
        std::snprintf(text, sizeof(text), "%d view(s), the players walking 6 m a second: a pass scans in %.1f us of a tick of %.1f us (a view holds %zu actors)", views, walking.scan_us, walking.tick_us, walking.tracked);
        CHECK(walking.tracked >= 600 && walking.scan_us > 0 && walking.scan_us < 5000, text);
    }
}
AOC_REGISTER_TEST(test_view_scan_cost);
