// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/actor_view_view_threads_tests.inc: the players' view passes on several threads (session/view_pool.h, net/endpoint_phases.cpp)
//
//  1. The registry is frozen while the passes run: every call that changes it throws, the reads work, and a thaw gives the writes back.
//  2. The pool: every job runs once, and every worker takes part (the first jobs wait for each other, so each thread holds one), a job knows its thread, the figures the threads write are
//     added up, an exception stays with its job (also one that carries no text), one thread runs the jobs in order.
//  3. A call of the pool ends with its jobs, not with its workers: with every worker held back (as a busy machine can hold a thread), the calling thread runs the jobs and the call returns; the
//     workers take part again once they are let go.
//  4. The same datagrams whatever the number of threads.  A scene of 12 players around 48 mobs (walking, changing a reliable value), packets lost on some connections, players
//     who join late, is run in the three phases of the server's tick (every player's decision whether its pass is due, in turn; all the due passes at once on the pool with the registry
//     frozen; every player's exchange, in turn).  One thread runs the passes in order and is the reference; 2, 4 and 8 threads, each job waiting a pseudo-random time before it starts so that the
//     threads interleave differently on every run, 200 runs in all, with the updates shared between the views and not, must send every connection exactly the reference's datagrams.
//     No pass may throw, and in all the runs together some passes must run on a worker (a pool whose workers never take a job would pass the rest).
//  5. What the passes of 24 players cost on one thread and on four (printed; the PC also builds and plays, so the bound is generous).
// ============================================================================
#include "net/tick_profile.h"
#include "session/view_pool.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace thread_checks {

struct Fnv {
    uint64_t h = 14695981039346656037ull;
    void add(const uint8_t* p, size_t n) {
        for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    }
    void add(uint64_t v) {
        for (int i = 0; i < 8; ++i) h = (h ^ ((v >> (8 * i)) & 0xff)) * 1099511628211ull;
    }
};

struct Rng {
    uint64_t s;
    uint32_t next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<uint32_t>(s >> 33);
    }
    int64_t below(int64_t n) { return static_cast<int64_t>(next()) % n; }
};

struct Viewer {
    Pair                       p;
    Fnv                        fnv;
    std::unique_ptr<ActorView> v;
    int                        join_step = 0;
    bool                       lossy = false;
    double                     x = 0, y = 0;
    bool                       due = false;
};

struct Result {
    uint64_t              digest = 0;
    std::vector<uint64_t> per_viewer;
    size_t                updates = 0, shared = 0, naks = 0;
    size_t                job_errors = 0;     // passes that threw
    uint64_t              worker_jobs = 0;    // passes that ran on a worker thread, not on the calling one
    double                pass_seconds = 0;   // the wall time of phase 2, summed over the steps
};

/// The scene (see the top of the file; 12 players, 48 mobs and 24 steps unless told otherwise) on a pool of `threads` threads (0: no pool, every pass run in order by the loop below, which is the order of the pool's first thread).
inline Result run_scene(unsigned threads, uint64_t jitter, bool share, int viewers = 12, int steps = 24, int creatures = 48) {
    world::ActorRegistry reg;
    for (int i = 0; i < creatures; ++i) {
        const double angle = 0.37 * i, r = 800.0 + 120.0 * (i % 60);
        reg.add(make_npc(1000 + 20 * static_cast<uint64_t>(i), r * std::cos(angle), r * std::sin(angle), i % 3 == 0 ? "Wolf" : "Boar"));
    }
    std::shared_ptr<SharedUpdates> sharing = share ? std::make_shared<SharedUpdates>(ShareMode::On) : nullptr;
    std::vector<std::unique_ptr<Viewer>> vs;
    for (int k = 0; k < viewers; ++k) {
        ActorView::Config cfg = view_config();
        cfg.shared = sharing;
        cfg.max_packets_in_flight = 1000000;
        cfg.max_queued_bunches = 1000000;
        cfg.relevancy.max_opens_per_tick = 60;
        auto w = std::make_unique<Viewer>();
        w->v = std::make_unique<ActorView>(reg, cfg, reserved_3_to_6());
        w->join_step = (k % 6) * steps / 14;
        w->lossy = k % 3 == 1;
        w->x = 30.0 * k;
        w->y = 15.0 * (k % 4);
        vs.push_back(std::move(w));
    }
    ViewPool pool(threads == 0 ? 1 : threads);
    pool.set_jitter(jitter);
    Result r;
    for (int s = 0; s < steps; ++s) {
        const Clock::time_point now = t0 + std::chrono::milliseconds(130) * s;
        // the world: mobs walk, change a reliable value (a flip of bIsDead), one is removed and one added (serial, before the passes)
        for (int k = 0; k < 10; ++k) {
            const uint64_t id = 1000 + 20 * static_cast<uint64_t>((s * 7 + k * 13) % creatures);
            const world::WorldActor* a = reg.find(id);
            if (!a) continue;
            const double x = a->x + ((s + k) % 2 ? 12.0 : -12.0), y = a->y + 7.0;
            reg.move(id, x, y, a->z, static_cast<float>((s * 13) % 360));
            reg.modify(id, world::Change::Movement, [x, y](world::WorldActor& w) {
                repl::RepMovement m;
                m.location = repl::QuantVec{repl::QuantVec::Mode::Scaled, static_cast<int64_t>(x * 100), static_cast<int64_t>(y * 100), 1200000};
                m.rotation = repl::Rot{0, 64, 0};
                m.linear_velocity = repl::QuantVec{repl::QuantVec::Mode::Scaled, 100, 0, 0};
                w.actor.obj.state.set(*w.actor.obj.schema, "ReplicatedMovement", repl::Value(m));
            });
        }
        if (s % 2 == 0) {
            for (int k = 0; k < 4; ++k) {
                const uint64_t id = 1000 + 20 * static_cast<uint64_t>((s * 5 + k * 11) % creatures);
                reg.modify(id, world::Change::State, [](world::WorldActor& w) {
                    for (repl::Subobject& sub : w.actor.subobjects) {
                        if (sub.name != "CombatInfo") continue;
                        const repl::Value* cur = sub.obj.state.get(*sub.obj.schema, "bIsDead");
                        const bool* was = cur ? cur->get_if<bool>() : nullptr;
                        sub.obj.state.set(*sub.obj.schema, "bIsDead", repl::Value::boolean(was ? !*was : true));
                    }
                });
            }
        }
        if (s == steps / 2) reg.remove(1000 + 20 * 5);
        if (s == steps / 2 + 2) reg.add(make_npc(9000, 1500, 400, "Wolf"));
        // phase 1: every player's decision whether its pass is due, in turn
        std::vector<Viewer*> jobs;
        for (auto& w : vs) {
            if (s < w->join_step) continue;
            if (s == w->join_step) w->p.t = now;
            w->due = w->v->plan(now);
            jobs.push_back(w.get());
        }
        std::vector<ViewPool::JobError> errors(jobs.size());
        // phase 2: the passes, all at once, with the registry frozen
        const auto a = std::chrono::steady_clock::now();
        {
            reg.freeze(true);
            pool.run(jobs.size(), [&](size_t i) { jobs[i]->v->run(jobs[i]->p.s, jobs[i]->x, jobs[i]->y, now, jobs[i]->due); }, &errors);
            reg.freeze(false);
        }
        for (const ViewPool::JobError& e : errors) r.job_errors += e.failed ? 1 : 0;
        r.pass_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - a).count();
        // phase 3: every player's exchange, in turn
        for (Viewer* w : jobs) {
            Pair& p = w->p;
            p.t += std::chrono::milliseconds(65);
            const auto pkts = p.s.flush(p.t);
            w->fnv.add(static_cast<uint64_t>(pkts.size()));
            size_t k = 0;
            for (const auto& pk : pkts) {
                w->fnv.add(pk.data(), pk.size());
                const bool lost = w->lossy && s >= steps * 3 / 10 && s < steps * 7 / 10 && (static_cast<uint64_t>(s) * 104729 + k * 31 + w->join_step) % 7 == 0;
                if (!lost) p.cl.receive(pk, p.t);
                ++k;
            }
            p.t += std::chrono::milliseconds(65);
            for (const auto& pk : p.cl.flush(p.t)) p.s.receive(pk, p.t);
            for (const AckEvent& e : p.s.take_ack_events()) w->v->on_ack(e);
        }
    }
    Fnv all;
    for (auto& w : vs) {
        r.per_viewer.push_back(w->fnv.h);
        all.add(w->fnv.h);
        r.updates += w->v->stats().updates + w->v->stats().updates_unreliable;
        r.shared += w->v->stats().shared_updates;
        r.naks += w->v->stats().naks;
    }
    r.digest = all.h;
    r.worker_jobs = pool.worker_jobs();
    return r;
}

}  // namespace thread_checks

void test_registry_is_frozen_while_the_passes_run() {
    std::printf("the registry: frozen, it is read by all and written by none\n");
    world::ActorRegistry reg;
    reg.add(make_npc(1000, 0, 0));
    reg.add(make_npc(1020, 500, 0));
    const auto throws = [&](const char* what, auto&& fn) {
        bool threw = false;
        try {
            fn();
        } catch (const std::logic_error&) {
            threw = true;
        }
        char text[160];
        std::snprintf(text, sizeof(text), "frozen: %s throws", what);
        CHECK(threw, text);
    };
    reg.freeze(true);
    std::vector<uint64_t> ids;
    reg.query(0, 0, 10000, ids);
    CHECK(reg.frozen() && reg.size() == 2 && reg.find(1000) != nullptr && ids.size() == 2, "frozen: find, query and size work");
    throws("add", [&] { reg.add(make_npc(1040, 0, 0)); });
    throws("remove", [&] { reg.remove(1000); });
    throws("modify", [&] { reg.modify(1000, world::Change::State, [](world::WorldActor&) {}); });
    throws("move", [&] { reg.move(1000, 1, 1, 1, 1); });
    throws("touch", [&] { reg.touch(*reg.find(1000), world::Change::State); });
    throws("set_interact_component", [&] { reg.set_interact_component(1000, 77); });
    throws("on_remove", [&] { reg.on_remove([](const world::WorldActor&) {}); });
    throws("off_remove", [&] { reg.off_remove(1); });
    CHECK(reg.size() == 2 && reg.find(1000)->version == 1, "...and nothing of the actors changed");
    reg.freeze(false);
    CHECK(reg.add(make_npc(1040, 0, 0)) && reg.modify(1000, world::Change::State, [](world::WorldActor&) {}) && reg.find(1000)->version == 2 && reg.remove(1040), "thawed: the writes work again");
}
AOC_REGISTER_TEST(test_registry_is_frozen_while_the_passes_run);

/// Runs the first `threads` jobs of a run side by side: each waits (up to 10 s) until all of them have started, so each sits on a thread of its own and every worker takes a job.
inline void meet_on_threads(std::atomic<unsigned>& arrived, size_t job, unsigned threads) {
    if (job >= threads) return;
    arrived.fetch_add(1);
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (arrived.load() < threads && std::chrono::steady_clock::now() < limit) std::this_thread::yield();
}

void test_view_pool_runs_every_job_once() {
    std::printf("the view pool: every job once, on a thread that knows its number, every worker at work, the threads' figures added up\n");
    char text[360];
    for (const unsigned threads : {1u, 2u, 4u, 8u}) {
        ViewPool pool(threads);
        pool.set_jitter(threads == 1 ? 0 : 99);
        constexpr size_t kJobs = 500;
        std::vector<std::atomic<int>> ran(kJobs);
        std::atomic<unsigned> highest{0};
        std::atomic<unsigned> arrived{0};
        std::vector<size_t> order;
        TickProfile& profile = tick_profile();
        profile.on = true;
        profile.restart();
        std::vector<ViewPool::JobError> errors(kJobs);
        pool.run(
            kJobs,
            [&](size_t i) {
                meet_on_threads(arrived, i, threads);
                ran[i].fetch_add(1);
                unsigned seen = highest.load();
                while (view_worker() > seen && !highest.compare_exchange_weak(seen, view_worker())) {}
                if (threads == 1) order.push_back(i);
                tick_profile().updates += 1;
                tick_profile().view_us += 2.0;
                if (i == 7) throw std::runtime_error("job seven");
                if (i == 9) throw std::runtime_error("");   // an exception with no text still fails its job
                if (i == 11) throw 11;                      // ... and so does one that is not a std::exception
            },
            &errors);
        const bool once = std::all_of(ran.begin(), ran.end(), [](const std::atomic<int>& r) { return r.load() == 1; });
        const bool errors_ok = errors[7].failed && errors[7].what == "job seven" && errors[9].failed && errors[9].what.empty() && errors[11].failed && !errors[11].what.empty() &&
                               std::count_if(errors.begin(), errors.end(), [](const ViewPool::JobError& e) { return e.failed; }) == 3;
        std::snprintf(text, sizeof(text), "%u thread(s): %zu jobs, each ran once, the highest thread number was %u, %llu jobs ran on a worker, the figures added up to %llu updates and %.0f us (3 jobs threw: \"%s\", \"\" and a number)",
                      threads, kJobs, highest.load(), static_cast<unsigned long long>(pool.worker_jobs()), static_cast<unsigned long long>(profile.updates), profile.view_us, errors[7].what.c_str());
        CHECK(once && highest.load() == threads - 1 && pool.worker_jobs() >= threads - 1 && profile.updates == kJobs && profile.view_us == 2.0 * kJobs && errors_ok && pool.jobs_run() == kJobs, text);
        if (threads == 1) {
            bool in_order = order.size() == kJobs;
            for (size_t i = 0; in_order && i < order.size(); ++i) in_order = order[i] == i;
            CHECK(in_order && pool.worker_jobs() == 0, "...one thread runs the jobs in order, all of them on the calling thread");
        }
        profile.on = false;
        profile.restart();
    }
}
AOC_REGISTER_TEST(test_view_pool_runs_every_job_once);

void test_view_pool_call_ends_with_its_jobs_not_with_its_workers() {
    std::printf("the view pool: a call ends when its jobs have ended, with every worker held back; the workers take part again when they are let go\n");
    char text[360];
    for (const unsigned threads : {2u, 4u, 8u}) {
        std::mutex mu;   // (declared before the pool: the gate uses them, and the pool's workers end first)
        std::condition_variable cv;
        bool open = false;
        ViewPool pool(threads);
        pool.set_worker_gate([&](unsigned) {   // every worker that wakes waits here, before it can take a job
            std::unique_lock<std::mutex> lock(mu);
            cv.wait(lock, [&] { return open; });
        });
        constexpr size_t kJobs = 60;
        std::vector<int> ran(kJobs, 0);   // (plain counts: while the workers are held, only the calling thread runs jobs)
        std::atomic<bool> ended{false};
        std::thread caller([&] {
            pool.run(kJobs, [&](size_t i) { ++ran[i]; });
            ended.store(true);
        });
        const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!ended.load() && std::chrono::steady_clock::now() < limit) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const bool ended_held = ended.load();   // (a pool that waited for its workers would still be waiting here)
        {
            std::lock_guard<std::mutex> lock(mu);
            open = true;
        }
        cv.notify_all();
        caller.join();
        const bool all_once = std::all_of(ran.begin(), ran.end(), [](int n) { return n == 1; });
        const uint64_t on_workers_before = pool.worker_jobs();
        // let go, the workers take part again: the first jobs of a call wait for each other, so each worker takes one
        std::vector<std::atomic<int>> again(kJobs);
        std::atomic<unsigned> arrived{0};
        pool.run(kJobs, [&](size_t i) {
            meet_on_threads(arrived, i, threads);
            again[i].fetch_add(1);
        });
        const bool again_once = std::all_of(again.begin(), again.end(), [](const std::atomic<int>& r) { return r.load() == 1; });
        std::snprintf(text, sizeof(text), "%u thread(s): with every worker held back the call ended after its %zu jobs (%s), all on the calling thread (%llu on a worker); let go, the next call ran its jobs once each, %llu of them on workers",
                      threads, kJobs, ended_held ? "yes" : "NO: it waited for the workers", static_cast<unsigned long long>(on_workers_before), static_cast<unsigned long long>(pool.worker_jobs() - on_workers_before));
        CHECK(ended_held && all_once && on_workers_before == 0 && again_once && pool.worker_jobs() - on_workers_before >= threads - 1, text);
    }
}
AOC_REGISTER_TEST(test_view_pool_call_ends_with_its_jobs_not_with_its_workers);

void test_view_passes_send_the_same_bytes_on_any_number_of_threads() {
    using namespace thread_checks;
    std::printf("view threads: 12 players, 48 mobs: the same datagrams on 1 thread and on 2, 4 and 8, with the threads interleaved differently on every run\n");
    char text[400];
    const auto started = std::chrono::steady_clock::now();
    const Result ref_plain = run_scene(1, 0, false);
    const Result ref_shared = run_scene(1, 0, true);
    CHECK(ref_plain.updates > 1500 && ref_plain.naks > 0, "the scene is a real one: thousands of update chains, some lost chains put back");
    CHECK(ref_plain.digest == ref_shared.digest && ref_plain.per_viewer == ref_shared.per_viewer && ref_shared.shared > 100 && ref_plain.job_errors == 0 && ref_shared.job_errors == 0,
          "one thread: sharing the updates between the views sends the same datagrams as not sharing, and no pass threw");
    size_t runs = 0, differ = 0, threw = 0;
    uint64_t on_workers[3] = {0, 0, 0};   // passes that ran on a worker, with 2, 4 and 8 threads
    const unsigned counts[] = {2, 4, 8};
    for (int run = 0; run < 200; ++run) {
        const unsigned threads = counts[run % 3];
        const bool share = (run / 3) % 2 == 1;
        const Result r = run_scene(threads, 0x1234567 + static_cast<uint64_t>(run) * 7919, share);
        ++runs;
        threw += r.job_errors;
        on_workers[run % 3] += r.worker_jobs;
        const Result& ref = share ? ref_shared : ref_plain;
        if (r.digest != ref.digest || r.per_viewer != ref.per_viewer || r.updates != ref.updates || r.naks != ref.naks) {
            ++differ;
            std::printf("  (run %d, %u threads, sharing %s: digest 0x%016llx, the reference's 0x%016llx)\n", run, threads, share ? "on" : "off", static_cast<unsigned long long>(r.digest),
                        static_cast<unsigned long long>(ref.digest));
        }
    }
    std::snprintf(text, sizeof(text), "%zu runs on 2, 4 and 8 threads (jitter on every job, updates shared in half of them): %zu sent other datagrams than one thread, %zu passes threw, passes ran on workers (%llu, %llu and %llu with 2, 4 and 8 threads) (%.1f s)",
                  runs, differ, threw, static_cast<unsigned long long>(on_workers[0]), static_cast<unsigned long long>(on_workers[1]), static_cast<unsigned long long>(on_workers[2]),
                  std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    CHECK(runs == 200 && differ == 0 && threw == 0 && on_workers[0] > 0 && on_workers[1] > 0 && on_workers[2] > 0, text);
    // the scale of a crowd once: 24 players around 650 mobs
    const Result big_ref = run_scene(1, 0, false, 24, 40, 650);
    size_t big_differ = 0, big_threw = big_ref.job_errors;
    for (const unsigned threads : {4u, 8u}) {
        for (const bool share : {false, true}) {
            const Result r = run_scene(threads, 0xb16 + threads, share, 24, 40, 650);
            if (r.digest != big_ref.digest || r.per_viewer != big_ref.per_viewer) ++big_differ;
            big_threw += r.job_errors;
        }
    }
    std::snprintf(text, sizeof(text), "24 players around 650 mobs (%zu update chains, %zu lost chains put back): on 4 and 8 threads, sharing the updates and not, %zu of 4 runs sent other datagrams than one thread, %zu passes threw", big_ref.updates,
                  big_ref.naks, big_differ, big_threw);
    CHECK(big_ref.updates > 2000 && big_ref.naks > 0 && big_differ == 0 && big_threw == 0, text);
}
AOC_REGISTER_TEST(test_view_passes_send_the_same_bytes_on_any_number_of_threads);

void test_view_passes_cost_on_one_thread_and_on_four() {
    using namespace thread_checks;
    std::printf("view threads: what the passes of 24 players cost on one thread and on four (the work of the server for the views, not a count of players)\n");
    char text[400];
    const unsigned cores = std::thread::hardware_concurrency();
    std::vector<double> one, four;
    for (int round = 0; round < 3; ++round) {   // (alternating, so that both meet the same load of the machine)
        one.push_back(run_scene(1, 0, false, 24, 40, 400).pass_seconds);
        four.push_back(run_scene(4, 0, false, 24, 40, 400).pass_seconds);
    }
    std::sort(one.begin(), one.end());
    std::sort(four.begin(), four.end());
    const double t1 = one[one.size() / 2], t4 = four[four.size() / 2];
    std::snprintf(text, sizeof(text), "the passes of 24 players (40 steps, 400 mobs): %.0f ms on one thread, %.0f ms on four (%.2fx; the PC has %u logical cores and was also building)", t1 * 1000.0, t4 * 1000.0,
                  t4 > 0 ? t1 / t4 : 0.0, cores);
    CHECK(t1 > 0 && t4 > 0 && t4 < t1 * 2.0, text);
}
AOC_REGISTER_TEST(test_view_passes_cost_on_one_thread_and_on_four);
