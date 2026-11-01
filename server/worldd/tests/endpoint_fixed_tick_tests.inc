// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/endpoint_fixed_tick_tests.inc: the server's tick at a fixed rate over a drained socket (net/tick_loop.h)
//
//  The loop runs on a fake clock and a fake socket: a datagram arrives at a time of the test's choosing, handling one takes 0.1 ms, a wait
//  for a datagram ends when one arrives or at its timeout.  The checks: the datagrams queued between two due ticks run ONE tick; a slow
//  tick is caught up within the cap and a stall restarts the grid; datagrams are still read when every tick takes longer than a period;
//  the rate is the one asked for; and the game's timers keep their real-time lengths at 15, 20 and 30 ticks a second (the NPC world's
//  step, each player's view pass, a connection's keep-alive, and the `now + period` gates of the fights and the regeneration).
// ============================================================================
#include "net/tick_loop.h"
#include "npc/npc_world.h"
#include "session/actor_view.h"
#include "world/actor_registry.h"
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace fixed_tick_checks {

using std::chrono::duration;
using std::chrono::microseconds;
using std::chrono::nanoseconds;

const Clock::time_point kStart{std::chrono::seconds(100)};

/// The fake socket and clock.
struct FakeNet {
    Clock::time_point now = kStart;
    std::deque<Clock::time_point> arrivals;   // the datagrams not read yet, by arrival time
    std::vector<Clock::time_point> arrived;   // the arrival time of each datagram handled, in order
    std::vector<Clock::time_point> handled;   // the time each datagram was handled at
    std::vector<Clock::time_point> ticks;     // the time each tick ran at
    std::vector<size_t> handled_before_tick;  // datagrams handled before each tick
    std::function<void(Clock::time_point)> work;   // the tick's own work (may move the clock: a slow tick)
    Clock::time_point last_read{};

    TickLoop::Ports ports() {
        TickLoop::Ports p;
        p.now = [this] { return now; };
        p.receive = [this](microseconds timeout) -> std::optional<size_t> {
            if (!arrivals.empty() && arrivals.front() <= now + timeout) {
                if (arrivals.front() > now) now = arrivals.front();
                last_read = arrivals.front();
                arrivals.pop_front();
                return size_t{40};
            }
            now += timeout;   // a wait that ends with nothing
            return size_t{0};
        };
        p.datagram = [this](size_t, Clock::time_point t) {
            arrived.push_back(last_read);
            handled.push_back(t);
            now += microseconds(100);
        };
        p.tick = [this](Clock::time_point t) {
            ticks.push_back(t);
            handled_before_tick.push_back(handled.size());
            if (work) work(t);
        };
        return p;
    }
};

void run_until(TickLoop& loop, FakeNet& net, Clock::time_point end) {
    TickLoop::Ports p = net.ports();
    while (net.now < end) {
        if (!loop.run_once(p)) return;
    }
}

double seconds_of(Clock::time_point t) { return duration<double>(t - kStart).count(); }

template <class... A>
std::string text(const char* format, A... a) {
    char buf[400];
    std::snprintf(buf, sizeof(buf), format, a...);
    return buf;
}

}  // namespace fixed_tick_checks

void test_fixed_tick_one_tick_for_many_datagrams() {
    using namespace fixed_tick_checks;
    std::printf("the fixed-rate tick: the datagrams between two due ticks run one tick\n");
    TickLoop loop(TickLoop::Config{30, 4, 256}, kStart);
    const Clock::duration period = loop.schedule().period();
    CHECK(period == nanoseconds(33333334), "30 a second is a period of 33,333,334 ns (1/30 s and a hair)");
    FakeNet net;
    for (int i = 0; i < 50; ++i) net.arrivals.push_back(kStart + std::chrono::milliseconds(5));
    run_until(loop, net, kStart + period + std::chrono::milliseconds(10));
    CHECK(net.handled.size() == 50 && net.ticks.size() == 1, "50 datagrams that arrive between two due ticks are handled and run ONE tick (the old loop ran 50)");
    CHECK(net.ticks.size() == 1 && net.ticks[0] == kStart + period && net.handled_before_tick[0] == 50,
          "the tick runs at its time on the grid, after the 50 datagrams");
    bool own_time = !net.handled.empty() && net.handled.front() == kStart + std::chrono::milliseconds(5);
    for (size_t i = 1; i < net.handled.size(); ++i) own_time = own_time && net.handled[i] == net.handled[i - 1] + microseconds(100);
    CHECK(own_time, "each datagram is handled the moment it is read, at its own time (not at a tick's)");
    const TickLoop::Stats s = loop.take_stats();
    CHECK(s.datagrams == 50 && s.ticks == 1 && s.max_datagrams_per_tick == 50, "the loop counts them: 50 datagrams, 1 tick, 50 between two ticks");

    // a tick that falls due while datagrams are read runs before the next one: the endpoint's times never go back
    TickLoop loop2(TickLoop::Config{30, 4, 256}, kStart);
    FakeNet net2;
    for (int i = 0; i < 5; ++i) net2.arrivals.push_back(kStart + period - microseconds(150));
    run_until(loop2, net2, kStart + period + std::chrono::milliseconds(5));
    bool ordered = net2.ticks.size() == 1 && net2.handled.size() == 5;
    std::vector<Clock::time_point> seen;   // every time the endpoint was given, in order
    for (size_t i = 0, d = 0; ordered && i <= net2.ticks.size(); ++i) {
        const size_t upto = i < net2.ticks.size() ? net2.handled_before_tick[i] : net2.handled.size();
        for (; d < upto; ++d) seen.push_back(net2.handled[d]);
        if (i < net2.ticks.size()) seen.push_back(net2.ticks[i]);
    }
    for (size_t i = 1; ordered && i < seen.size(); ++i) ordered = seen[i] >= seen[i - 1];
    CHECK(ordered && net2.handled_before_tick[0] == 2, "a tick due in the middle of a batch runs between its datagrams, and the times go forward only");
}
AOC_REGISTER_TEST(test_fixed_tick_one_tick_for_many_datagrams);

void test_fixed_tick_catch_up() {
    using namespace fixed_tick_checks;
    std::printf("the fixed-rate tick: a slow tick is caught up within the cap; a stall starts the grid again; datagrams are read under overload\n");
    TickLoop loop(TickLoop::Config{30, 4, 256}, kStart);
    const Clock::duration period = loop.schedule().period();
    FakeNet net;
    net.work = [&](Clock::time_point) {
        if (net.ticks.size() == 3) net.now += period * 7 / 2;   // the 3rd tick takes 3.5 periods
        if (net.ticks.size() == 10) net.now += period * 21 / 2;  // the 10th stalls 10.5 periods
    };
    run_until(loop, net, kStart + period * 30);
    bool grid = net.ticks.size() >= 10;
    for (size_t i = 0; grid && i < 10; ++i) grid = net.ticks[i] == kStart + period * static_cast<int64_t>(i + 1);
    CHECK(grid, "the three ticks behind the slow one run back to back at their own times on the grid (4, 5, 6), then the grid goes on");
    CHECK(loop.schedule().caught_up() == 2, "two of them ran a whole period or more late (caught up)");
    const Clock::time_point stall_end = kStart + period * 10 + period * 21 / 2;   // (the fake wait ends on a whole microsecond: up to 1 us later)
    CHECK(net.ticks.size() > 11 && net.ticks[10] >= stall_end && net.ticks[10] - stall_end < microseconds(1) && net.ticks[11] == net.ticks[10] + period,
          "after a stall of 10.5 periods (more than the cap of 4) the next tick runs at once and the grid starts again from it");
    CHECK(loop.schedule().skipped() == 9 && loop.schedule().restarts() == 1, "the 9 ticks the stall missed are counted as skipped, not run");

    // every tick takes 1.5 periods: at most 5 run in a row, and the datagrams are read between them
    TickLoop busy(TickLoop::Config{30, 4, 256}, kStart);
    FakeNet net2;
    net2.work = [&](Clock::time_point) { net2.now += period * 3 / 2; };
    for (int i = 0; i < 200; ++i) net2.arrivals.push_back(kStart + std::chrono::milliseconds(10 * i));
    run_until(busy, net2, kStart + std::chrono::seconds(2) + std::chrono::milliseconds(500));
    double worst = 0;
    for (size_t i = 0; i < net2.handled.size(); ++i) worst = std::max(worst, duration<double, std::milli>(net2.handled[i] - net2.arrived[i]).count());
    const double bound = duration<double, std::milli>(period * 15 / 2 + period * 3 / 2 + period).count();
    CHECK(net2.handled.size() == 200 && worst <= bound,
          text("every tick 1.5 periods long: all 200 datagrams are still read, the longest waits %.0f ms (at most 5 ticks in a row and one more: %.0f ms)", worst, bound).c_str());
    CHECK(busy.schedule().restarts() > 0 && net2.ticks.size() > 30,
          text("it keeps ticking (%zu ticks in 2.5 s) and drops the ticks it cannot run (%llu skipped)", net2.ticks.size(),
               static_cast<unsigned long long>(busy.schedule().skipped())).c_str());
}
AOC_REGISTER_TEST(test_fixed_tick_catch_up);

void test_fixed_tick_rate() {
    using namespace fixed_tick_checks;
    std::printf("the fixed-rate tick: the rate asked for is the rate run\n");
    CHECK(TickSchedule::kMinHz == 10 && TickSchedule::kMaxHz == 120, "the start option allows 10 to 120 ticks a second (0: the old loop)");
    for (const int hz : {10, 15, 20, 30, 60, 120}) {
        TickLoop loop(TickLoop::Config{hz, 4, 256}, kStart);
        FakeNet net;
        run_until(loop, net, kStart + std::chrono::seconds(10) + std::chrono::milliseconds(1));
        const Clock::duration period = TickSchedule::period_of(hz);
        bool exact = period == nanoseconds(1000000000LL / hz + 1);
        for (size_t i = 0; exact && i < net.ticks.size(); ++i) exact = net.ticks[i] == kStart + period * static_cast<int64_t>(i + 1);
        CHECK(net.ticks.size() == static_cast<size_t>(hz) * 10 && exact, text("%d ticks a second: %zu ticks in 10 s, each at its time on the grid", hz, net.ticks.size()).c_str());
    }
}
AOC_REGISTER_TEST(test_fixed_tick_rate);

void test_fixed_tick_game_timers() {
    using namespace fixed_tick_checks;
    std::printf("the fixed-rate tick: the game's timers keep their real-time lengths at 15, 20 and 30 ticks a second\n");
    for (const int hz : {15, 20, 30}) {
        TickLoop loop(TickLoop::Config{hz, 4, 256}, kStart);
        const Clock::duration period = loop.schedule().period();
        world::ActorRegistry registry;
        npc::NpcWorld npcs(registry, nullptr);
        ActorView::Config view_config;
        tick_view_timing(view_config.relevancy, period);
        ActorView view(registry, view_config, [](uint32_t) { return false; });
        ActorView::Config old_config;   // the struct's 67 ms gate, as the old loop used it
        ActorView old_view(registry, old_config, [](uint32_t) { return false; });
        Connection conn(ConnectionConfig{}, kStart);
        Connection old_conn(ConnectionConfig{}, kStart);
        uint64_t keepalives = 0, think = 0, regen = 0, step_ticks_two = 0, last_steps = 0;
        double next_think = -1, next_regen = -1;
        FakeNet net;
        net.work = [&](Clock::time_point t) {
            const double s = seconds_of(t);
            npcs.tick(s);
            const uint64_t steps = npcs.stats().steps;
            if (steps - last_steps > 1) ++step_ticks_two;
            last_steps = steps;
            view.tick(conn, 0, 0, t);
            old_view.tick(old_conn, 0, 0, t);
            keepalives += conn.flush(t).size();
            if (s >= next_think) {   // CombatWorld's AI gate: next = now + 0.2 s
                ++think;
                next_think = s + 0.2;
            }
            if (s >= next_regen) {   // a 1 s gate set from the tick's time
                ++regen;
                next_regen = s + 1.0;
            }
        };
        run_until(loop, net, kStart + std::chrono::seconds(10) + std::chrono::milliseconds(1));
        const uint64_t steps = npcs.stats().steps;
        CHECK(steps >= 149 && steps <= 151 && step_ticks_two == 0,
              text("%d Hz: the NPC world steps %llu times in 10 s (15 a second), never twice in one tick", hz, static_cast<unsigned long long>(steps)).c_str());
        const size_t want_passes = hz == 20 ? 200 : 150;   // every tick at 15 and 20 Hz, every second tick at 30 Hz
        CHECK(view.stats().passes + 1 >= want_passes && view.stats().passes <= want_passes + 1,
              text("%d Hz: each player's view passes %zu times in 10 s (its gate %lld ms on the grid)", hz, view.stats().passes,
                   static_cast<long long>(view_config.relevancy.tick.count())).c_str());
        CHECK(keepalives >= 49 && keepalives <= 51,
              text("%d Hz: a quiet connection's keep-alive goes out %llu times in 10 s (every 200 ms)", hz, static_cast<unsigned long long>(keepalives)).c_str());
        CHECK(think >= 49 && think <= 51 && regen >= 10 && regen <= 11,
              text("%d Hz: a 0.2 s gate opens %llu times in 10 s and a 1 s gate %llu times (never a tick late)", hz, static_cast<unsigned long long>(think),
                   static_cast<unsigned long long>(regen)).c_str());
        const Clock::duration close = std::chrono::duration_cast<Clock::duration>(view_config.relevancy.dormancy_delay);
        const int64_t close_ticks = (close + period - Clock::duration(1)) / period;   // the first tick at or after the delay
        const int64_t want_close = (134 * hz + 500) / 1000;                           // the tick nearest 134 ms
        CHECK(close_ticks == want_close, text("%d Hz: a Dormancy close goes out %lld ticks after its open, the tick nearest 134 ms", hz, static_cast<long long>(close_ticks)).c_str());
        if (hz == 30) {
            CHECK(old_view.stats().passes <= 101,
                  text("(30 Hz with the struct's 67 ms gate the view would pass only %zu times in 10 s: why the server sets its own)", old_view.stats().passes).c_str());
        }
    }
}
AOC_REGISTER_TEST(test_fixed_tick_game_timers);
