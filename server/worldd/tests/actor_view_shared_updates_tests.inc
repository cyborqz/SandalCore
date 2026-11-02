// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/actor_view_shared_updates_tests.inc: the update that is built once for the players who hold a mob in the same state (session/shared_updates.h)
//
//  The same scene is run with every view building its own updates and with the updates shared; every datagram of every connection is folded into a digest per connection, and the digests
//  of the two runs must be the same, connection by connection.  The scenes are made of what makes the shadows of two connections differ: players who join at different times, packets lost on
//  some connections (the NAK handling puts values back into their shadows), connections that hear nothing for a stretch and ones that are held back by their own pacing, mobs that
//  change a reliable value, take a step or cast, a value taken away and set again, mobs removed and added, and mobs the player can interact with (drawn for each player with
//  the player's own guid).  A test that never shares proves nothing, so each run also has to show that updates were taken from another view's blocks.
//
//  1. Six players around sixteen mobs.
//  2. Forty players around six hundred and fifty mobs; the same scene in verify mode, where every shared update is also built the plain way and compared (nothing may differ).
//  3. Verify mode finds a shared update that was damaged on purpose, says so, turns sharing off, and the connections still get what the plain build sends.
//  4. The cost of an update with forty players holding the same mobs, with and without sharing (printed; bot numbers are the work of the server for the views, not a count of players).
// ============================================================================
#include "actor_view_scene.h"
#include "net/tick_profile.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace shared_checks {

using namespace view_scene;

struct Fnv {
    uint64_t h = 14695981039346656037ull;
    void add(const uint8_t* p, size_t n) {
        for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    }
    void add(uint64_t v) {
        for (int i = 0; i < 8; ++i) h = (h ^ ((v >> (8 * i)) & 0xff)) * 1099511628211ull;
    }
};

/// Prints how long a test took when it ends (these scenes are the long ones of the program).
struct Stopwatch {
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    ~Stopwatch() { std::printf("  (took %.1f s)\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()); }
};

struct Params {
    int  viewers = 6;
    int  creatures = 16;
    bool awake_only = true;      // every mob within the awake ring (else the rings of a real area: two in five awake, the rest snapshots)
    int  steps = 130;
    int  walkers = 3;            // mobs that take a step on every step (an unreliable update)
    int  flips = 4;              // mobs that change a reliable value every other step
    bool joins = true;           // viewer k first looks at step (k % 8) * 5
    bool loss = false;           // every third viewer loses packets (deterministically) from step 40 to 70
    bool blackouts = false;      // viewers 1 and 4 hear nothing for a stretch
    bool paced = false;          // every fifth viewer has a tight pacing: its passes are held back
    bool interactables = false;  // every 20th mob is one a player can interact with, and the viewers are players
    bool shape_events = false;   // values taken away and set again, mobs removed and added
    bool casts = false;          // mobs cast: a fast array, and a montage's export
    int  real_every = 1;         // mob i has the components of a mob the loader builds when i is a multiple of this (the others have the actor, its info and its combat info)
    int  interactable_every = 4; // ... and every this many mobs of them is one a player can interact with (when `interactables`)
    bool refusals = false;       // one mob changes a reliable value on every step for 340 steps and viewer 1 hears nothing: the connection refuses the chains it cannot take
    std::shared_ptr<SharedUpdates> shared;   // none: every view builds its own
    SharedUpdates::Fault fault = SharedUpdates::Fault::None;   // damages the first update published from fault_step on
    int  fault_step = -1;
};

struct ViewerResult {
    uint64_t digest = 0;
    size_t   packets = 0;
    size_t   updates = 0, unreliable = 0, shared = 0, refused = 0, naks = 0, opens = 0, passes = 0;
};

struct SceneResult {
    std::vector<ViewerResult> viewers;
    uint64_t digest = 0;          // the viewers' digests folded
    size_t   updates = 0, shared = 0, refused = 0, naks = 0;
};

struct Viewer {
    Pair                       p;
    Fnv                        fnv;
    std::unique_ptr<ActorView> v;
    int                        join_step = 0;
    bool                       lossy = false;
    int                        blackout_from = -1, blackout_to = -1;
    double                     x = 0, y = 0;
    uint64_t                   salt = 0;
};

/// Whether the packet `k` of a lossy connection's step is lost.
inline bool lost(const Viewer& w, int step, size_t k) { return (w.salt * 7919 + static_cast<uint64_t>(step) * 104729 + k * 31) % 11 == 0; }

/// One step of the exchange: the server flushes, the client receives (a lossy connection loses some packets) and answers when `deliver`; every datagram the server sends is folded into the digest.
inline void exchange(Viewer& w, int step, std::chrono::milliseconds dt, bool deliver) {
    Pair& p = w.p;
    p.t += dt;
    const auto pkts = p.s.flush(p.t);
    p.packets += pkts.size();
    w.fnv.add(static_cast<uint64_t>(pkts.size()));
    size_t k = 0;
    for (const auto& pk : pkts) {
        w.fnv.add(pk.data(), pk.size());
        if (deliver && !(w.lossy && step >= 40 && step < 70 && lost(w, step, k))) p.cl.receive(pk, p.t);
        ++k;
    }
    p.t += dt;
    if (deliver) {
        for (const auto& pk : p.cl.flush(p.t)) p.s.receive(pk, p.t);
    }
    for (const AckEvent& e : p.s.take_ack_events()) w.v->on_ack(e);
}

inline uint64_t creature_id(int i) { return 1000 + 20 * static_cast<uint64_t>(i); }

inline void flip(world::ActorRegistry& reg, uint64_t id) {
    reg.modify(id, world::Change::State, [](world::WorldActor& w) {
        for (repl::Subobject& s : w.actor.subobjects) {
            if (s.name != "CombatInfo") continue;
            const repl::Value* cur = s.obj.state.get(*s.obj.schema, "bIsDead");
            const bool* was = cur ? cur->get_if<bool>() : nullptr;
            s.obj.state.set(*s.obj.schema, "bIsDead", repl::Value::boolean(was ? !*was : true));
        }
    });
}

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

/// The world for the viewers: the mobs, and what happens to them on each step (the same for every run of a scene).
struct World {
    world::ActorRegistry reg;
    Params               pr;
    Rng                  rng{0x5eed5};
    std::unordered_map<uint64_t, combat::NpcCastState> casts;
    struct Taken {
        uint64_t    id;
        uint16_t    handle;
        repl::Value value;
    };
    std::vector<Taken> taken;

    void add_creature(uint64_t id, int index) {
        const bool awake = pr.awake_only || index % 5 < 2;
        const int64_t r_min = awake ? 300 : 14000, r_max = awake ? 11000 : 35000;
        int64_t x = 0, y = 0;
        for (;;) {
            x = rng.below(2 * r_max + 1) - r_max;
            y = rng.below(2 * r_max + 1) - r_max;
            const int64_t d2 = x * x + y * y;
            if (d2 >= r_min * r_min && d2 <= r_max * r_max) break;
        }
        const bool real = index % pr.real_every == 0;
        world::WorldActor w = real ? make_real_npc(id, static_cast<double>(x), static_cast<double>(y), rng) : make_npc(id, static_cast<double>(x), static_cast<double>(y), index % 3 == 0 ? "Wolf" : "Boar");
        if (pr.interactables && real && (index / pr.real_every) % pr.interactable_every == 0) make_interactable(w, 500 + index);
        reg.add(std::move(w));
    }

    void build() {
        for (int i = 0; i < pr.creatures; ++i) add_creature(creature_id(i), i);
    }

    void step(int s) {
        const int n = pr.creatures;
        for (int k = 0; k < pr.walkers; ++k) step_creature(reg, creature_id((s * 7 + k * 13) % n), s, k);
        if (s % 2 == 0) {
            for (int k = 0; k < pr.flips; ++k) flip(reg, creature_id((s * 5 + k * 11) % n));
        }
        if (pr.refusals && s < 340) flip(reg, creature_id(0));
        if (pr.casts && s % 10 == 0) {
            for (int k = 0; k < 3; ++k) {
                const uint64_t id = creature_id((s / 10 * 3 + k) % n);
                combat::NpcCastState& st = casts[id];
                combat::NpcCast c;
                c.ability = 1000 + k;
                c.montage = 0x9000 + 2 * static_cast<uint64_t>(k);
                if (!st.playing) combat::start_npc_cast(reg, id, st, c, repl::ExportChain{link(c.montage, "Montage", false, 0x55)});
                else combat::end_npc_cast(reg, id, st);
            }
        }
        if (pr.shape_events) {
            if (s == 30) {   // a value taken away from five mobs ...
                for (int k = 0; k < 5; ++k) {
                    const uint64_t id = creature_id(k * 3);
                    reg.modify(id, world::Change::State, [&](world::WorldActor& w) {
                        auto it = w.actor.obj.state.values().begin();
                        if (it == w.actor.obj.state.values().end() || it->second.get_if<repl::ArrayValue>()) return;
                        taken.push_back({id, it->first, it->second});
                        w.actor.obj.state.erase(it->first);
                    });
                }
            }
            if (s == 34) {   // ... and set again, to what it was
                for (const Taken& t : taken) {
                    reg.modify(t.id, world::Change::State, [&](world::WorldActor& w) { w.actor.obj.state.set(*w.actor.obj.schema, t.handle, t.value); });
                }
            }
            if (s == 60) {   // six mobs removed, six added
                for (int k = 0; k < 6; ++k) reg.remove(creature_id(n - 1 - k));
                for (int k = 0; k < 6; ++k) add_creature(90000 + 20 * static_cast<uint64_t>(k), k);
            }
        }
    }
};

/// The scene (see the top of the file).
inline SceneResult run_scene(const Params& pr) {
    World world;
    world.pr = pr;
    world.build();
    std::vector<std::unique_ptr<Viewer>> viewers;
    for (int k = 0; k < pr.viewers; ++k) {
        ActorView::Config cfg = view_config();
        cfg.shared = pr.shared;
        cfg.relevancy.max_opens_per_tick = 60;   // (the opens of a few hundred mobs take a few passes, not forty)
        const bool tight = pr.paced && k % 5 == 2;
        cfg.max_packets_in_flight = tight ? 20 : 1000000;
        cfg.max_queued_bunches = tight ? 24 : 1000000;
        if (pr.interactables) cfg.player_guid = repl::GuidValue{0x1000u + static_cast<uint32_t>(k), 0x2000u + static_cast<uint32_t>(k), 0x3000u, 0x4000u};
        cfg.player_id = 100 + static_cast<uint64_t>(k);
        auto w = std::make_unique<Viewer>();
        w->v = std::make_unique<ActorView>(world.reg, cfg, reserved_3_to_6());
        w->join_step = pr.joins ? (k % 8) * 5 : 0;
        w->lossy = pr.loss && k % 3 == 1;
        w->salt = static_cast<uint64_t>(k) + 1;
        if (pr.blackouts && k == 1) {
            w->blackout_from = pr.refusals ? 20 : 60;
            w->blackout_to = pr.refusals ? 330 : 100;
        }
        if (pr.blackouts && k == 4) {
            w->blackout_from = 20;
            w->blackout_to = 40;
        }
        w->x = 25.0 * k;
        w->y = 10.0 * (k % 5);
        viewers.push_back(std::move(w));
    }
    for (int s = 0; s < pr.steps; ++s) {
        if (pr.fault != SharedUpdates::Fault::None && s == pr.fault_step && pr.shared) pr.shared->fault_for_test(pr.fault);
        world.step(s);
        for (auto& w : viewers) {
            if (s < w->join_step) continue;
            if (s == w->join_step) w->p.t = t0 + std::chrono::milliseconds(130) * s;   // (all the viewers share one clock)
            w->v->tick(w->p.s, w->x, w->y, w->p.t);
            exchange(*w, s, std::chrono::milliseconds(65), !(s >= w->blackout_from && s < w->blackout_to));
        }
    }
    SceneResult r;
    Fnv all;
    for (auto& w : viewers) {
        const ActorView::Stats& st = w->v->stats();
        ViewerResult v;
        v.digest = w->fnv.h;
        v.packets = w->p.packets;
        v.updates = st.updates + st.updates_unreliable;
        v.unreliable = st.updates_unreliable;
        v.shared = st.shared_updates;
        v.refused = st.refused_updates;
        v.naks = st.naks;
        v.opens = st.opens + st.snapshots;
        v.passes = st.passes;
        all.add(v.digest);
        r.updates += v.updates;
        r.shared += v.shared;
        r.refused += v.refused;
        r.naks += v.naks;
        r.viewers.push_back(v);
    }
    r.digest = all.h;
    return r;
}

/// How many viewers got the same datagrams, and the same counts, in the two runs.
inline int same_viewers(const SceneResult& a, const SceneResult& b) {
    int same = 0;
    for (size_t i = 0; i < a.viewers.size() && i < b.viewers.size(); ++i) {
        const ViewerResult& x = a.viewers[i];
        const ViewerResult& y = b.viewers[i];
        if (x.digest == y.digest && x.packets == y.packets && x.updates == y.updates && x.unreliable == y.unreliable && x.refused == y.refused && x.naks == y.naks && x.opens == y.opens) ++same;
    }
    return a.viewers.size() == b.viewers.size() ? same : 0;
}

/// The small scene.
inline Params small_scene() {
    Params p;
    p.viewers = 6;
    p.creatures = 16;
    p.steps = 350;
    p.loss = true;
    p.blackouts = true;
    p.paced = true;
    p.shape_events = true;
    p.casts = true;
    p.interactables = true;
    p.refusals = true;
    return p;
}

/// The large scene: forty players, six hundred and fifty mobs in the rings of a real area, forty of them walking.
inline Params large_scene() {
    Params p;
    p.viewers = 40;
    p.creatures = 650;
    p.awake_only = false;
    p.steps = 70;
    p.walkers = 40;
    p.flips = 8;
    p.loss = true;
    p.blackouts = true;
    p.paced = true;
    p.shape_events = true;
    p.casts = true;
    p.interactables = true;
    p.real_every = 10;
    p.interactable_every = 2;
    return p;
}

struct Cost {
    double build_us = 0;   // the building of an update (the part sharing changes)
    double update_us = 0;  // the whole update: building and sending
    double view_us = 0;    // a view's whole tick, per pass
    size_t updates = 0;
    double hit_rate = 0;
};

/// The cost of an update with `viewer_count` players who stand about one place and hold the same 160 mobs, twenty of them walking and four changing a reliable value every other step.
inline Cost run_cost(bool share, int viewer_count) {
    World world;
    world.pr.viewers = viewer_count;
    world.pr.creatures = 160;
    world.pr.awake_only = false;
    world.pr.walkers = 20;
    world.pr.flips = 4;
    world.build();
    std::shared_ptr<SharedUpdates> sharing = share ? std::make_shared<SharedUpdates>(ShareMode::On) : nullptr;
    std::vector<std::unique_ptr<Viewer>> viewers;
    for (int k = 0; k < viewer_count; ++k) {
        ActorView::Config cfg = view_config();
        cfg.shared = sharing;
        cfg.relevancy.max_opens_per_tick = 60;
        cfg.max_packets_in_flight = 1000000;
        cfg.max_queued_bunches = 1000000;
        auto w = std::make_unique<Viewer>();
        w->v = std::make_unique<ActorView>(world.reg, cfg, reserved_3_to_6());
        w->x = 20.0 * k;
        viewers.push_back(std::move(w));
    }
    int step = 0;
    const auto tick_all = [&]() {
        world.step(step);
        for (auto& w : viewers) {
            w->v->tick(w->p.s, w->x, w->y, w->p.t);
            exchange(*w, step, std::chrono::milliseconds(35), true);
        }
        ++step;
    };
    for (; step < 30;) tick_all();   // the opens
    TickProfile& profile = tick_profile();
    profile.on = true;
    std::vector<double> build, update, view;
    size_t updates = 0;
    const uint64_t hits_before = sharing ? sharing->stats().hits : 0, built_before = sharing ? sharing->stats().builds : 0, private_before = sharing ? sharing->stats().private_builds : 0;
    for (int stretch = 0; stretch < 3; ++stretch) {
        profile.restart();
        for (int i = 0; i < 25; ++i) tick_all();
        if (profile.updates > 0) {
            build.push_back(profile.update_build_us / static_cast<double>(profile.updates));
            update.push_back(profile.update_us / static_cast<double>(profile.updates));
            view.push_back(profile.view_us / static_cast<double>(std::max<uint64_t>(profile.view_passes, 1)));
            updates += profile.updates;
        }
    }
    profile.on = false;
    profile.restart();
    Cost c;
    if (!build.empty()) {
        std::sort(build.begin(), build.end());
        std::sort(update.begin(), update.end());
        std::sort(view.begin(), view.end());
        c.build_us = build[build.size() / 2];
        c.update_us = update[update.size() / 2];
        c.view_us = view[view.size() / 2];
    }
    c.updates = updates;
    if (sharing) {
        const uint64_t h = sharing->stats().hits - hits_before, b = sharing->stats().builds - built_before, p = sharing->stats().private_builds - private_before;
        c.hit_rate = h + b + p > 0 ? static_cast<double>(h) / static_cast<double>(h + b + p) : 0;
    }
    return c;
}

}  // namespace shared_checks

void test_shared_updates_small_scene_sends_the_same_bytes() {
    using namespace shared_checks;
    Stopwatch watch;
    std::printf("shared updates: six players, sixteen mobs, joins at different times, lost packets, silent stretches, pacing, casts, a value taken away, interactable mobs: the same bytes either way\n");
    char text[500];
    Params plain = small_scene();
    const SceneResult a = run_scene(plain);
    const SceneResult again = run_scene(plain);
    std::snprintf(text, sizeof(text), "the scene is repeatable: two runs without sharing send the same datagrams (%zu update chains, %zu lost and put back, %zu refused)", a.updates, a.naks, a.refused);
    CHECK(same_viewers(a, again) == 6 && a.digest == again.digest, text);
    CHECK(a.updates > 600 && a.naks > 0 && a.refused > 0, "...and it is a real one: hundreds of update chains, some of them lost and put back, some refused by a full connection");
    Params shared = small_scene();
    shared.shared = std::make_shared<SharedUpdates>(ShareMode::On);
    const SceneResult b = run_scene(shared);
    const SharedUpdates::Stats& st = shared.shared->stats();
    std::snprintf(text, sizeof(text), "with the updates shared: %d of 6 connections get exactly the datagrams they got without (%zu of %zu update chains were built by another view; %llu built for sharing, %llu built for one view)", same_viewers(a, b),
                  b.shared, b.updates, static_cast<unsigned long long>(st.builds), static_cast<unsigned long long>(st.private_builds));
    CHECK(same_viewers(a, b) == 6 && a.digest == b.digest, text);
    CHECK(b.shared > 50 && st.hits > 50 && st.builds > 50 && st.private_builds > 0 && st.mismatches == 0, "...and the sharing was real: updates were taken from another view's blocks, others had to be built for one view alone");
}
AOC_REGISTER_TEST(test_shared_updates_small_scene_sends_the_same_bytes);

void test_shared_updates_large_scene_sends_the_same_bytes() {
    using namespace shared_checks;
    Stopwatch watch;
    std::printf("shared updates: forty players around 650 mobs, the same kinds of trouble: the same bytes either way, and in verify mode nothing differs\n");
    char text[500];
    Params plain = large_scene();
    const SceneResult a = run_scene(plain);
    std::snprintf(text, sizeof(text), "without sharing: %zu update chains sent to 40 connections (%zu lost and put back)", a.updates, a.naks);
    CHECK(a.updates > 5000 && a.naks > 0, text);
    Params shared = large_scene();
    shared.shared = std::make_shared<SharedUpdates>(ShareMode::On);
    const SceneResult b = run_scene(shared);
    const SharedUpdates::Stats st = shared.shared->stats();
    std::snprintf(text, sizeof(text), "with the updates shared: %d of 40 connections get exactly the datagrams they got without; %zu of %zu update chains were built by another view (%llu built for sharing, %llu built for one view)",
                  same_viewers(a, b), b.shared, b.updates, static_cast<unsigned long long>(st.builds), static_cast<unsigned long long>(st.private_builds));
    CHECK(same_viewers(a, b) == 40 && a.digest == b.digest, text);
    CHECK(b.shared > b.updates / 3 && st.private_builds > 0 && st.mismatches == 0, "...a large part of the updates were not built again, and some had to be (a lost chain's values put back, interactable mobs, a value taken away)");
    Params verify = large_scene();
    verify.shared = std::make_shared<SharedUpdates>(ShareMode::Verify, 4);   // one shared update in four is also built the plain way, from a copy of the shadow, and compared
    const SceneResult c = run_scene(verify);
    const SharedUpdates::Stats vs = verify.shared->stats();
    std::snprintf(text, sizeof(text), "in verify mode %llu shared updates were built the plain way as well and compared, with the same bytes, records and shadows: %llu differed; the connections get the same datagrams (%d of 40)",
                  static_cast<unsigned long long>(vs.verified), static_cast<unsigned long long>(vs.mismatches), same_viewers(a, c));
    CHECK(vs.verified > 1000 && vs.mismatches == 0 && verify.shared->mode() == ShareMode::Verify && same_viewers(a, c) == 40, text);
}
AOC_REGISTER_TEST(test_shared_updates_large_scene_sends_the_same_bytes);

void test_shared_updates_verify_finds_a_damaged_update() {
    using namespace shared_checks;
    Stopwatch watch;
    std::printf("shared updates: a shared update damaged on purpose is found by the verify mode, which turns sharing off; the connections still get what the plain build sends\n");
    char text[500];
    Params plain = small_scene();
    const SceneResult base = run_scene(plain);
    const struct {
        SharedUpdates::Fault fault;
        const char*          what;
    } faults[] = {{SharedUpdates::Fault::DropLastBlock, "its last content block dropped"}, {SharedUpdates::Fault::SkewRedo, "a handle missing from what it writes into the shadow"}};
    for (const auto& f : faults) {
        Params p = small_scene();
        p.shared = std::make_shared<SharedUpdates>(ShareMode::Verify, 1);
        p.fault = f.fault;
        p.fault_step = 50;
        const SceneResult r = run_scene(p);
        const SharedUpdates::Stats& st = p.shared->stats();
        std::snprintf(text, sizeof(text), "an update published with %s: found (%llu difference reported), sharing is %s, %llu updates were shared before", f.what, static_cast<unsigned long long>(st.mismatches),
                      SharedUpdates::name(p.shared->mode()), static_cast<unsigned long long>(st.hits));
        CHECK(st.mismatches == 1 && p.shared->mode() == ShareMode::Off && st.hits > 0, text);
        std::snprintf(text, sizeof(text), "...and the connections got the same datagrams as without sharing (%d of 6)", same_viewers(base, r));
        CHECK(same_viewers(base, r) == 6 && base.digest == r.digest, text);
    }
    // the same damage in on mode (no verify) is not found: the test above is what finds it
    Params blind = small_scene();
    blind.shared = std::make_shared<SharedUpdates>(ShareMode::On);
    blind.fault = SharedUpdates::Fault::DropLastBlock;
    blind.fault_step = 50;
    const SceneResult unseen = run_scene(blind);
    std::snprintf(text, sizeof(text), "the same damage with sharing on and no verify goes out unseen (%d of 6 connections differ from the plain run): verify is what protects", 6 - same_viewers(base, unseen));
    CHECK(same_viewers(base, unseen) < 6, text);
}
AOC_REGISTER_TEST(test_shared_updates_verify_finds_a_damaged_update);

void test_shared_updates_cost() {
    using namespace shared_checks;
    Stopwatch watch;
    std::printf("shared updates: what an update costs with 40 players holding the same mobs (the work of the server for the views, not a count of players)\n");
    char text[500];
    std::vector<double> plain_build, shared_build, plain_update, shared_update, plain_view, shared_view;
    double hit_rate = 0;
    size_t updates = 0;
    for (int round = 0; round < 2; ++round) {   // (alternating: the two kinds meet the same load of the machine)
        const Cost p = run_cost(false, 40);
        const Cost s = run_cost(true, 40);
        plain_build.push_back(p.build_us);
        shared_build.push_back(s.build_us);
        plain_update.push_back(p.update_us);
        shared_update.push_back(s.update_us);
        plain_view.push_back(p.view_us);
        shared_view.push_back(s.view_us);
        hit_rate = s.hit_rate;
        updates = s.updates;
    }
    const auto median = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    const double pb = median(plain_build), sb = median(shared_build), pu = median(plain_update), su = median(shared_update), pv = median(plain_view), sv = median(shared_view);
    std::snprintf(text, sizeof(text), "building an update: %.1f us for each view alone, %.1f us shared (%.0f%% of the updates taken from another view's blocks); the whole update, sent as well: %.1f us, %.1f us shared; a view's tick: %.0f us, %.0f us shared (%zu updates)",
                  pb, sb, hit_rate * 100.0, pu, su, pv, sv, updates);
    CHECK(pb > 0 && sb > 0 && hit_rate > 0.5, text);   // (the times are printed, not compared: the gate runs on a PC that is busy with other work)
    // the worst case: one player, nothing to share (every update is built, kept for a second view that never comes, and used by its own view)
    std::vector<double> alone_plain, alone_shared;
    for (int round = 0; round < 3; ++round) {
        alone_plain.push_back(run_cost(false, 1).update_us);
        alone_shared.push_back(run_cost(true, 1).update_us);
    }
    const double ap = median(alone_plain), as = median(alone_shared);
    std::snprintf(text, sizeof(text), "one player alone (nothing to share): an update costs %.1f us, %.1f us with sharing on (%+.0f%%)", ap, as, ap > 0 ? (as - ap) / ap * 100.0 : 0.0);
    CHECK(ap > 0 && as > 0, text);   // (printed, not compared: see above)
}
AOC_REGISTER_TEST(test_shared_updates_cost);

void test_shared_updates_a_copy_of_an_actor_starts_without_the_kept_update() {
    using namespace shared_checks;
    std::printf("shared updates: a copy of an actor does not take the update kept for the views of the actor it was copied from\n");
    world::ActorRegistry reg;
    Rng rng{7};
    reg.add(make_real_npc(6000, 0, 0, rng));
    world::WorldActor* a = reg.find(6000);
    SharedUpdates sharing(ShareMode::On);
    sharing.publish(*a, 1, {}, repl::SentRecord{}, repl::ShadowRedo{});
    world::WorldActor copied = *a;
    world::WorldActor moved = std::move(*a);
    CHECK(sharing.find(*a, 1) == nullptr && moved.update_memo[0].ptr != nullptr && copied.update_memo[0].ptr == nullptr, "...the copy has none, a moved actor keeps its own");
}
AOC_REGISTER_TEST(test_shared_updates_a_copy_of_an_actor_starts_without_the_kept_update);
