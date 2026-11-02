// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/actor_view_update_stream_tests.inc: the bytes the view puts on the wire for the updates of the actors it holds open
//
//  A fixed scene on a simulated clock: sixteen creatures around a player, three moving a little on every step and eight changing a reliable value, an actor removed and
//  one added; the client's side is silent for two stretches (the first: movement only, so the chains it loses are unreliable ones and the NAK handling puts their values
//  back; the second, 300 steps: a creature changes a reliable value on every step, more than the 256 unacknowledged reliable bunches a channel takes, so the connection
//  refuses the rest until the client answers again).  Every datagram the server sends is folded into one 64-bit digest (FNV-1a).  The digest was taken from the view when it built each update on a copy
//  of the actor's shadow and kept the copy only if the connection took the chain; the view now builds it on the shadow itself and puts it back when the connection
//  refuses.  The two must send the same bytes, so this check holds the number.  (A change of the replicator's wire form on purpose changes the digest: the failing check
//  prints the new one.)
// ============================================================================
#include <cstdint>

namespace update_stream_checks {

struct Fnv {
    uint64_t h = 14695981039346656037ull;
    void add(const uint8_t* p, size_t n) {
        for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    }
    void add(uint64_t v) {
        for (int i = 0; i < 8; ++i) h = (h ^ ((v >> (8 * i)) & 0xff)) * 1099511628211ull;
    }
};

struct Result {
    uint64_t digest = 0;
    size_t   packets = 0;      // datagrams the server sent
    size_t   updates = 0;      // update chains the view sent (reliable and not)
    size_t   refused = 0;      // update chains the connection refused: the view tried them again at the next pass
    size_t   naks = 0;         // update chains the client lost whose values were put back
};

/// One step of the exchange: the server flushes, the client receives and answers when `deliver`; every datagram of the server is folded into the digest either way.
inline void stream_step(Pair& p, ActorView& v, Fnv& fnv, bool deliver, milliseconds dt) {
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

inline Result run_update_stream() {
    world::ActorRegistry reg;
    Pair p;
    ActorView::Config cfg = view_config();
    cfg.max_packets_in_flight = 1000000;   // (the view's own pacing is not what this scene is about: the connection's limits are)
    cfg.max_queued_bunches = 1000000;
    ActorView v(reg, cfg, reserved_3_to_6());
    int step = 0;
    constexpr int kActors = 16;
    for (int i = 0; i < kActors; ++i) {
        const double angle = 0.4 * i;
        reg.add(make_npc(1000 + 10 * static_cast<uint64_t>(i), 5000.0 + 400.0 * i * std::cos(angle), 5000.0 + 400.0 * i * std::sin(angle), i % 3 == 0 ? "Wolf" : "Boar"));
    }
    Fnv fnv;
    std::vector<bool> dead(kActors, false);
    constexpr int kSteps = 460;
    for (step = 0; step < kSteps; ++step) {
        const bool silent_moves = step >= 20 && step < 40;     // (the first silent stretch has movement only)
        const bool blackout = silent_moves || (step >= 60 && step < 360);
        const bool flips = step < 400 && !silent_moves;
        if (step < 400) {
            // movement: three creatures take a step each time (unreliable updates)
            for (int k = 0; k < 3; ++k) {
                const int i = (step * 3 + k * 5) % kActors;
                const uint64_t id = 1000 + 10 * static_cast<uint64_t>(i);
                const world::WorldActor* a = reg.find(id);
                if (!a) continue;
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
            // a reliable value: eight creatures flip bIsDead on every step, the first one on all of them (300 reliable chains on its channel in the second silent stretch)
            for (int k = 0; flips && k < 8; ++k) {
                const int i = k == 0 ? 0 : 1 + (step * 5 + k * 2) % (kActors - 1);
                const uint64_t id = 1000 + 10 * static_cast<uint64_t>(i);
                if (!reg.find(id)) continue;
                dead[static_cast<size_t>(i)] = !dead[static_cast<size_t>(i)];
                const bool now_dead = dead[static_cast<size_t>(i)];
                reg.modify(id, world::Change::State, [now_dead](world::WorldActor& w) {
                    for (repl::Subobject& s : w.actor.subobjects) {
                        if (std::string(s.name) == "CombatInfo") s.obj.state.set(*s.obj.schema, "bIsDead", repl::Value::boolean(now_dead));
                    }
                });
            }
        }
        if (step == 100 || step == 380) reg.remove(1000 + 10 * (step == 100 ? 7 : 9));
        if (step == 130) reg.add(make_npc(1300, 6000, -3000, "Wolf"));
        v.tick(p.s, 0, 0, p.t);
        stream_step(p, v, fnv, !blackout, milliseconds(65));
    }
    Result r;
    r.digest = fnv.h;
    r.packets = p.packets;
    r.updates = v.stats().updates + v.stats().updates_unreliable;
    r.refused = v.stats().refused_updates;
    r.naks = v.stats().naks;
    return r;
}

}  // namespace update_stream_checks

void test_view_update_stream_is_the_same_bytes() {
    using namespace update_stream_checks;
    std::printf("the view: the update chains it sends are the same bytes whether they are built on a copy of the shadow or on the shadow itself\n");
    const Result a = run_update_stream();
    const Result b = run_update_stream();
    char text[300];
    CHECK(a.digest == b.digest && a.packets == b.packets, "the scene is repeatable: two runs send the same datagrams");
    CHECK(a.updates > 400 && a.naks > 0, "...and it is a real one: hundreds of update chains, and lost ones put back for the next update");
    char refused[300];
    std::snprintf(refused, sizeof(refused), "the silent stretch fills the connection: it refused %zu update chains and the view built each of them again at a later pass", a.refused);
    CHECK(a.refused >= 20, refused);
    constexpr uint64_t kDigest = 0xa5cceeafbf606940ull;   // (taken from the view that built each update on a copy of the shadow)
    std::snprintf(text, sizeof(text), "the datagrams of the scene (%zu, %zu update chains, %zu put back after losses) fold to 0x%016llx; held: 0x%016llx", a.packets, a.updates, a.naks,
                  static_cast<unsigned long long>(a.digest), static_cast<unsigned long long>(kDigest));
    CHECK(a.digest == kDigest, text);
}
AOC_REGISTER_TEST(test_view_update_stream_is_the_same_bytes);
