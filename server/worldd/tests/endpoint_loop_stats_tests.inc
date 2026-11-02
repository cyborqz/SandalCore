// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/endpoint_loop_stats_tests.inc: what the tick loop reports about itself (net/tick_loop.h) and the socket it reads (net/udp_socket.h)
//
//  A tick that starts the tick grid again (more than four periods late) is the latest one: its lateness is in the loop's report, not 0.  The counts of ticks caught up,
//  dropped and of restarts of the grid are the interval's, not the totals since the start.  The socket asks the system for a receive buffer bigger than the default.
// ============================================================================
#include "net/tick_loop.h"
#include "net/udp_socket.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace loop_stats_checks {

const Clock::time_point kOrigin{std::chrono::seconds(300)};

template <class... A>
std::string text(const char* format, A... a) {
    char buf[400];
    std::snprintf(buf, sizeof(buf), format, a...);
    return buf;
}

/// A loop on a fake clock: no datagrams, a wait ends at its timeout, a tick takes what `work` makes the clock go forward by.
struct Run {
    Clock::time_point            now = kOrigin;
    uint64_t                     ticks = 0;
    std::function<void(uint64_t)> work;   // the tick's own work, given the number of the tick (1, 2 ...)

    void run(TickLoop& loop, Clock::time_point end) {
        TickLoop::Ports p;
        p.now = [this] { return now; };
        p.receive = [this](std::chrono::microseconds timeout) -> std::optional<size_t> {
            now += timeout;
            return size_t{0};
        };
        p.datagram = [](size_t, Clock::time_point) {};
        p.tick = [this](Clock::time_point) {
            ++ticks;
            if (work) work(ticks);
        };
        while (now < end) {
            if (!loop.run_once(p)) return;
        }
    }
};

}  // namespace loop_stats_checks

void test_late_max_counts_the_tick_that_restarts_the_grid() {
    using namespace loop_stats_checks;
    std::printf("the tick loop: the worst stall, the one that starts the grid again, is in the latest-tick figure\n");
    TickSchedule s(30, kOrigin);
    const Clock::duration period = s.period();
    const Clock::time_point t = s.take(kOrigin + period * 13);   // the tick due at 1 period runs at 13: twelve periods late, more than the cap of four
    CHECK(t == kOrigin + period * 13 && s.restarts() == 1 && s.skipped() == 12, "a tick twelve periods late starts the grid again at the time it is (12 ticks dropped)");
    CHECK(s.late_max() == period * 12, text("... and its lateness, %.0f ms, is what the schedule reports (it recorded 0 before)", std::chrono::duration<double, std::milli>(s.late_max()).count()).c_str());
    s.reset_late_max();
    CHECK(s.late_max() == Clock::duration::zero(), "reset");
    TickSchedule r(30, kOrigin);
    r.restart(kOrigin + period * 6 + std::chrono::milliseconds(5));   // the loop drops the tick that was due at 1 period after running five in a row
    CHECK(r.restarts() == 1 && r.late_max() == period * 5 + std::chrono::milliseconds(5), "a restart by the loop records the lateness of the tick it dropped");

    // through the loop: the tenth tick stalls ten and a half periods
    TickLoop loop(TickLoop::Config{30, 4, 256}, kOrigin);
    Run run;
    run.work = [&](uint64_t n) {
        if (n == 10) run.now += period * 21 / 2;
    };
    run.run(loop, kOrigin + period * 30);
    CHECK(loop.schedule().late_max() >= period * 9, text("a stall of 10.5 periods in the tenth tick: the loop reports %.0f ms late",
                                                         std::chrono::duration<double, std::milli>(loop.schedule().late_max()).count()).c_str());
}
AOC_REGISTER_TEST(test_late_max_counts_the_tick_that_restarts_the_grid);

void test_loop_counts_are_the_intervals() {
    using namespace loop_stats_checks;
    std::printf("the tick loop: ticks caught up, ticks dropped and restarts of the grid are counted per interval\n");
    TickLoop loop(TickLoop::Config{30, 4, 256}, kOrigin);
    const Clock::duration period = loop.schedule().period();
    Run run;
    run.work = [&](uint64_t n) {
        if (n == 3) run.now += period * 7 / 2;     // 3.5 periods: caught up
        if (n == 12) run.now += period * 21 / 2;   // 10.5 periods: the grid starts again
    };
    run.run(loop, kOrigin + period * 40);
    const TickLoop::Stats first = loop.take_stats();
    CHECK(first.caught_up == 2 && first.skipped == 9 && first.restarts == 1, text("the first interval: %llu caught up, %llu dropped, %llu restart(s)",
          static_cast<unsigned long long>(first.caught_up), static_cast<unsigned long long>(first.skipped), static_cast<unsigned long long>(first.restarts)).c_str());
    run.run(loop, run.now + period * 30);
    const TickLoop::Stats second = loop.take_stats();
    CHECK(second.caught_up == 0 && second.skipped == 0 && second.restarts == 0 && second.ticks >= 29,
          "the next interval had no stall: all three counts are 0 (they were the totals since the start before)");
    run.work = [&](uint64_t n) {
        if (n % 7 == 0) run.now += period * 6;   // every seventh tick stalls six periods
    };
    run.run(loop, run.now + period * 60);
    const TickLoop::Stats third = loop.take_stats();
    CHECK(third.restarts > 0 && third.skipped >= third.restarts * 5 && third.restarts == loop.schedule().restarts() - first.restarts,
          text("a third interval with stalls counts its own: %llu restarts, %llu dropped", static_cast<unsigned long long>(third.restarts),
               static_cast<unsigned long long>(third.skipped)).c_str());
}
AOC_REGISTER_TEST(test_loop_counts_are_the_intervals);

void test_socket_asks_for_a_big_receive_buffer() {
    using namespace loop_stats_checks;
    std::printf("the UDP socket: it asks the system for a receive buffer of 1 MB and reports what it got\n");
    std::string err;
    auto s = UdpSocket::open(*parse_address("127.0.0.1:0"), &err);
    CHECK(s != nullptr, "a socket opens on a port the system chooses");
    if (!s) return;
    CHECK(UdpSocket::kReceiveBufferBytes == 1024 * 1024, "the buffer asked for is 1 MB");
    CHECK(s->receive_buffer_bytes() >= 128 * 1024, text("the system granted %d KB (the default of about 64 KB is not enough for a stalled tick)", s->receive_buffer_bytes() / 1024).c_str());
}
AOC_REGISTER_TEST(test_socket_asks_for_a_big_receive_buffer);
