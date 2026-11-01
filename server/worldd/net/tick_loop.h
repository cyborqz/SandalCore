// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/tick_loop.h
//
//  worldd's network thread: a tick at a fixed rate over a drained socket.
//
//  A datagram is handled the moment it is read, at its own arrival time: the handshake is answered at once, the connection's
//  acknowledgements are applied, the client's requests are judged and started, and what they answer is queued on the connection.
//  Nothing else runs for a datagram.  The tick (Endpoint::tick: the logins that finished, the timeouts, every session's due work,
//  every connection's sends and the world's step) runs `hz` times a second.  The original server sent to each client every 33 ms
//  (30 Hz) whether the player stood, walked or fought, and moved its world on a 15 Hz step: the world here keeps its own 15 Hz step
//  inside the tick (the NPC world's step, each player's view pass: tick_view_timing below), so a faster tick sends sooner without
//  moving the world more often.
//
//    the grid    tick k is due at origin + k x period, and runs at that time (the times its timers see are exact, whatever the
//                system's wake-up delay).  The period is floor(1 s / hz) + 1 ns, a hair longer than 1/hz: a timer of n/hz seconds
//                set at a tick falls due at the n-th tick after it, never one tick later through rounding.
//    a pass      the ticks that are due; a wait for a datagram until the next tick is due; then the datagrams queued, at most
//                `max_batch`, each handled at once.  A tick that falls due while datagrams are read runs before the next one is
//                handled, so the times the endpoint is given never go back.
//    catching up a late tick runs at its grid time and the ones after it run back to back, up to `max_catch_up` periods behind and
//                `max_catch_up` + 1 ticks in a row; further behind (a stall, or ticks that each take longer than a period), the missed
//                ticks are dropped (counted as skipped) and the grid starts again from the time it is: a server that cannot keep up
//                runs fewer ticks and still reads its datagrams between them, never a growing backlog of ticks.
//
//  The loop does no I/O itself: main.cpp gives it the socket, the endpoint and the clock (TickLoop::Ports), the tests give fakes.
// ============================================================================
#pragma once

#include "net/connection.h"
#include "world/relevancy.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

namespace aoc { namespace worldd {

/// When the ticks are due (the grid above).
class TickSchedule {
public:
    /// The rates the start option allows (0 there is the old loop: a tick after every datagram).
    static constexpr int kMinHz = 10;
    static constexpr int kMaxHz = 120;

    static Clock::duration period_of(int hz) {
        return std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(1000000000LL / std::max(hz, 1) + 1));
    }

    TickSchedule(int hz, Clock::time_point start, int max_catch_up = 4)
        : period_(period_of(hz)), max_catch_up_(std::max(max_catch_up, 0)), origin_(start), next_(start + period_) {}

    Clock::duration period() const { return period_; }
    /// The time the next tick is due.
    Clock::time_point next() const { return next_; }
    bool due(Clock::time_point now) const { return now >= next_; }

    /// Takes the due tick: the time it runs at (its grid time; `now` when the grid starts again) and moves on to the next one.
    Clock::time_point take(Clock::time_point now) {
        Clock::time_point t = next_;
        if (now > t) {
            const int64_t behind = static_cast<int64_t>((now - t) / period_);   // whole periods
            if (behind > max_catch_up_) {
                skipped_ += static_cast<uint64_t>(behind);
                ++restarts_;
                origin_ = now;
                k_ = 0;
                t = now;
            } else if (behind >= 1) {
                ++caught_up_;
            }
            late_max_ = std::max(late_max_, now - t);
        }
        ++k_;
        next_ = origin_ + period_ * k_;
        ++ticks_;
        return t;
    }

    /// Drops the ticks due by `now` (counted as skipped) and starts the grid again at `now`: the next tick is due a period later.
    void restart(Clock::time_point now) {
        if (now >= next_) skipped_ += static_cast<uint64_t>((now - next_) / period_) + 1;
        ++restarts_;
        origin_ = now;
        k_ = 1;
        next_ = now + period_;
    }

    uint64_t ticks() const { return ticks_; }
    /// Ticks run a whole period or more after their grid time (the backlog of a slow tick, caught up).
    uint64_t caught_up() const { return caught_up_; }
    /// Ticks dropped because the loop was more than `max_catch_up` periods behind, and how often the grid started again for it.
    uint64_t skipped() const { return skipped_; }
    uint64_t restarts() const { return restarts_; }
    /// The latest a tick ran after its grid time (since the last reset_late_max).
    Clock::duration late_max() const { return late_max_; }
    void reset_late_max() { late_max_ = Clock::duration::zero(); }

private:
    Clock::duration period_;
    int64_t max_catch_up_;
    Clock::time_point origin_;
    int64_t k_ = 1;
    Clock::time_point next_;
    uint64_t ticks_ = 0, caught_up_ = 0, skipped_ = 0, restarts_ = 0;
    Clock::duration late_max_{};
};

/// The view timing of a server ticking every `period`: each player's view pass and its Dormancy closes keep their real-time lengths
/// (the config's: 67 ms, 134 ms) on the tick's grid.  The pass runs every n-th tick, n = the pass's length in ticks, rounded, at least 1
/// (every tick at 15 and 20 Hz, every second tick at 30 Hz: 15 a second, the original server's), and a Dormancy close goes out at the
/// tick nearest its length.  The gates are set half a period short of those ticks, so a tick on the grid always meets them.
inline void tick_view_timing(world::RelevancyConfig& r, Clock::duration period) {
    using std::chrono::duration;
    using std::chrono::milliseconds;
    const double ratio = duration<double>(r.tick).count() / duration<double>(period).count();
    const int64_t n = std::max<int64_t>(1, static_cast<int64_t>(std::llround(ratio)));
    r.tick = std::chrono::duration_cast<milliseconds>(period * n - period / 2);
    const Clock::duration close = std::chrono::duration_cast<Clock::duration>(r.dormancy_delay) - period / 2;
    r.dormancy_delay = std::chrono::duration_cast<milliseconds>(std::max(close, Clock::duration::zero()));
}

/// The loop (a pass above).
class TickLoop {
public:
    struct Config {
        int hz = 30;
        int max_catch_up = 4;
        size_t max_batch = 256;
    };

    /// What the loop drives.
    struct Ports {
        std::function<Clock::time_point()> now;
        /// Waits up to `timeout` for a datagram (zero: does not wait).  Its size; 0 when none came; nullopt when the socket failed.
        std::function<std::optional<size_t>(std::chrono::microseconds timeout)> receive;
        /// Handles the datagram just received, at `now`.
        std::function<void(size_t size, Clock::time_point now)> datagram;
        /// The endpoint's tick at `t`.
        std::function<void(Clock::time_point t)> tick;
        /// After each tick (the admin queue, the world stream; they read the clock themselves).  May be empty.
        std::function<void()> after_tick;
    };

    /// Counts since the last take_stats.
    struct Stats {
        uint64_t passes = 0;
        uint64_t ticks = 0;
        uint64_t datagrams = 0;
        size_t max_batch = 0;                // the most datagrams one pass read
        size_t max_datagrams_per_tick = 0;   // the most datagrams handled between two ticks
    };

    TickLoop(Config config, Clock::time_point start) : config_(config), schedule_(config.hz, start, config.max_catch_up) {}

    /// One pass.  False when the socket failed.
    bool run_once(Ports& p) {
        ++stats_.passes;
        Clock::time_point now = p.now();
        run_due(p, now);
        const Clock::duration left = schedule_.next() - now;
        const auto wait = left > Clock::duration::zero() ? std::chrono::ceil<std::chrono::microseconds>(left) : std::chrono::microseconds(0);
        std::optional<size_t> n = p.receive(wait);
        size_t batch = 0;
        while (n && *n > 0) {
            now = p.now();
            run_due(p, now);
            p.datagram(*n, now);
            ++stats_.datagrams;
            ++since_tick_;
            if (++batch >= config_.max_batch) break;
            n = p.receive(std::chrono::microseconds(0));
        }
        stats_.max_batch = std::max(stats_.max_batch, batch);
        return n.has_value();
    }

    const TickSchedule& schedule() const { return schedule_; }
    TickSchedule& schedule() { return schedule_; }
    Stats take_stats() {
        const Stats s = stats_;
        stats_ = Stats{};
        return s;
    }

private:
    /// Runs the ticks due by `now`, `now` read again after each.  At most max_catch_up + 1 in a row: a tick that is still due after them
    /// (every tick takes longer than a period) is dropped and the grid starts again, so the datagrams are read between.
    void run_due(Ports& p, Clock::time_point& now) {
        int in_a_row = 0;
        while (schedule_.due(now)) {
            if (in_a_row > config_.max_catch_up) {
                schedule_.restart(now);
                break;
            }
            ++in_a_row;
            const Clock::time_point t = schedule_.take(now);
            stats_.max_datagrams_per_tick = std::max(stats_.max_datagrams_per_tick, since_tick_);
            since_tick_ = 0;
            ++stats_.ticks;
            p.tick(t);
            if (p.after_tick) p.after_tick();
            now = p.now();
        }
    }

    Config config_;
    TickSchedule schedule_;
    Stats stats_;
    size_t since_tick_ = 0;
};

}}  // namespace aoc::worldd
