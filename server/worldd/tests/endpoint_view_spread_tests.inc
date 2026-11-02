// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/endpoint_view_spread_tests.inc: the players' view passes are spread over the ticks of their cycle (session/view_phases.h)
//
//  Forty players join 200 ms apart (a batch of the load test's bots).  At 30 ticks a second every view passes every second tick.  Without a phase the tick of a
//  view's first pass is the tick of all its later ones, and joins an even number of ticks apart all pass on the same tick of the pair; with the phases the passes
//  split evenly, also after a stall of the tick grid and after many players left.  The views are the real ActorView, ticked on the real tick grid.
// ============================================================================
#include "net/tick_loop.h"
#include "session/actor_view.h"
#include "session/view_phases.h"
#include "world/actor_registry.h"
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace view_spread_checks {

const Clock::time_point kOrigin{std::chrono::seconds(500)};

template <class... A>
std::string text(const char* format, A... a) {
    char buf[400];
    std::snprintf(buf, sizeof(buf), format, a...);
    return buf;
}

/// Players with a view each, ticked on the tick grid of `hz` ticks a second.
struct Crowd {
    world::ActorRegistry                     registry;
    ActorView::Config                        config;
    std::shared_ptr<ViewPhases>              phases;
    std::vector<std::unique_ptr<Connection>> conns;
    std::vector<std::unique_ptr<ActorView>>  views;
    Clock::duration                          period;
    int64_t                                  k = 0;       // the grid tick the last tick ran at
    std::vector<size_t>                      per_tick;    // the passes made on each tick
    int64_t                                  per_pass = 1;

    explicit Crowd(bool spread, int hz = 30) : period(TickSchedule::period_of(hz)) {
        per_pass = tick_view_timing(config.relevancy, period);
        if (spread && per_pass > 1) {
            phases = std::make_shared<ViewPhases>(static_cast<unsigned>(per_pass));
            config.phases = phases;
        }
    }
    void join() {
        conns.push_back(std::make_unique<Connection>(ConnectionConfig{}, kOrigin));
        views.push_back(std::make_unique<ActorView>(registry, config, [](uint32_t) { return false; }));
    }
    size_t passes() const {
        size_t n = 0;
        for (const auto& v : views) n += v->stats().passes;
        return n;
    }
    /// One tick (the world step counts it, every view ticks); `skip` grid ticks are dropped first, as a stall of the loop does.
    void tick(int64_t skip = 0) {
        k += 1 + skip;
        if (phases) phases->begin_tick();
        const size_t before = passes();
        const Clock::time_point t = kOrigin + period * k;
        for (size_t i = 0; i < views.size(); ++i) views[i]->tick(*conns[i], 0, 0, t);
        per_tick.push_back(passes() - before);
    }
    /// 40 joins, one every 6 ticks (200 ms at 30 ticks a second), then `after` ticks more.
    void batch(int joins, int every, int after) {
        for (int j = 0; j < joins; ++j) {
            join();
            for (int e = 0; e < every; ++e) tick();
        }
        for (int a = 0; a < after; ++a) tick();
    }
    /// The passes of the last `n` ticks: the smallest and the largest number on one tick.
    std::pair<size_t, size_t> spread_of_last(size_t n) const {
        const auto from = per_tick.end() - static_cast<std::ptrdiff_t>(std::min(n, per_tick.size()));
        return {*std::min_element(from, per_tick.end()), *std::max_element(from, per_tick.end())};
    }
};

}  // namespace view_spread_checks

void test_view_phases_hand_out_the_lightest() {
    using namespace view_spread_checks;
    std::printf("view phases: a new view takes the phase with the fewest views; a view of a heavier phase moves at its pass\n");
    ViewPhases p(2);
    CHECK(p.cycle() == 2 && p.views(0) == 0 && p.views(1) == 0, "two phases, none held");
    p.begin_tick();   // tick 1: phase 1's tick
    CHECK(p.join() == 1 && p.due(1) && !p.due(0), "the first view takes the phase of the tick it is on and passes on it at once");
    CHECK(p.join() == 0 && p.join() == 1 && p.join() == 0, "the next ones alternate");
    CHECK(p.views(0) == 2 && p.views(1) == 2, "two and two");
    p.leave(0);
    p.leave(0);
    CHECK(p.views(0) == 0 && p.views(1) == 2, "two left phase 0");
    CHECK(p.balance(1) == 0 && p.views(0) == 1 && p.views(1) == 1, "a view of phase 1 that passes moves to the empty phase 0 (two more views than it)");
    CHECK(p.balance(1) == 1 && p.balance(0) == 0, "and a difference of one or none moves nobody");
    p.leave(1);
    p.leave(1);
    p.leave(7);
    CHECK(p.views(0) == 1 && p.views(1) == 0, "a phase that does not exist is no phase to leave");
    ViewPhases one(1);
    CHECK(one.join() == 0 && one.due(0) && one.balance(0) == 0, "a cycle of one tick has nothing to spread: its only phase is always due");
    ViewPhases four(4);
    for (int i = 0; i < 40; ++i) four.join();
    CHECK(four.views(0) == 10 && four.views(1) == 10 && four.views(2) == 10 && four.views(3) == 10, "four phases: forty views split ten by ten");
}
AOC_REGISTER_TEST(test_view_phases_hand_out_the_lightest);

void test_view_passes_spread_over_both_ticks() {
    using namespace view_spread_checks;
    std::printf("view passes: forty players joining 200 ms apart pass half on each tick of the pair\n");
    Crowd old_gate(/*spread=*/false);
    old_gate.batch(40, 6, 60);
    const auto bunched = old_gate.spread_of_last(60);
    CHECK(bunched.first == 0 && bunched.second == 40,
          text("without phases (the clock alone) the batch passes together: %zu passes on one tick of the pair, %zu on the other", bunched.second, bunched.first).c_str());

    Crowd crowd(/*spread=*/true);
    CHECK(crowd.per_pass == 2 && crowd.phases && crowd.phases->cycle() == 2, "30 ticks a second: a pass every second tick, two phases");
    crowd.batch(40, 6, 60);
    const auto even = crowd.spread_of_last(60);
    CHECK(even.first >= 19 && even.second <= 21, text("with phases the same batch splits: %zu to %zu passes on a tick (20 each)", even.first, even.second).c_str());
    CHECK(crowd.phases->views(0) == 20 && crowd.phases->views(1) == 20, "twenty views on each phase");
    // 60 ticks (30 pairs) after the last join: each of the 40 views makes 30 passes in them (15 a second, as before)
    Crowd steady(/*spread=*/true);
    steady.batch(40, 6, 4);
    const size_t before = steady.passes();
    for (int i = 0; i < 60; ++i) steady.tick();
    CHECK(steady.passes() - before == 40 * 30, text("40 views make %zu passes in 60 ticks (40 x 30): the rate is the same as without phases", steady.passes() - before).c_str());
}
AOC_REGISTER_TEST(test_view_passes_spread_over_both_ticks);

void test_view_passes_stay_spread_after_a_stall_and_departures() {
    using namespace view_spread_checks;
    std::printf("view passes: a stall of the tick grid changes nobody's phase; players who leave do not leave one tick heavy\n");
    Crowd crowd(/*spread=*/true);
    crowd.batch(40, 6, 20);
    crowd.tick(/*skip=*/9);   // a stall: ten periods go by in one tick (the loop starts its grid again)
    for (int i = 0; i < 40; ++i) crowd.tick();
    const auto after_stall = crowd.spread_of_last(30);
    CHECK(after_stall.first >= 19 && after_stall.second <= 21,
          text("after a stall of ten periods the passes are still split: %zu to %zu on a tick", after_stall.first, after_stall.second).c_str());

    // the twenty views of phase 0 leave at once
    std::vector<size_t> keep;
    for (size_t i = 0; i < crowd.views.size(); ++i) {
        if (crowd.views[i]->phase() != 0) keep.push_back(i);
    }
    CHECK(keep.size() == 20, "twenty views are on phase 1");
    std::vector<std::unique_ptr<ActorView>> staying;
    std::vector<std::unique_ptr<Connection>> staying_conns;
    for (size_t i = 0; i < crowd.views.size(); ++i) {
        if (crowd.views[i]->phase() == 0) {
            crowd.views[i].reset();   // (its phase is given back)
        } else {
            staying.push_back(std::move(crowd.views[i]));
            staying_conns.push_back(std::move(crowd.conns[i]));
        }
    }
    crowd.views = std::move(staying);
    crowd.conns = std::move(staying_conns);
    CHECK(crowd.phases->views(0) == 0 && crowd.phases->views(1) == 20, "the phase of the departed holds none");
    for (int i = 0; i < 60; ++i) crowd.tick();
    const auto settled = crowd.spread_of_last(20);
    CHECK(settled.first >= 9 && settled.second <= 11 && crowd.phases->views(0) == 10 && crowd.phases->views(1) == 10,
          text("the twenty who stayed move to the empty tick at their passes and settle ten and ten: %zu to %zu passes on a tick", settled.first, settled.second).c_str());
}
AOC_REGISTER_TEST(test_view_passes_stay_spread_after_a_stall_and_departures);

void test_view_passes_spread_at_other_rates() {
    using namespace view_spread_checks;
    std::printf("view passes: the cycle follows the rate (60 ticks a second: a pass every fourth tick, four phases; 20 ticks a second: every tick)\n");
    Crowd fast(/*spread=*/true, 60);
    CHECK(fast.per_pass == 4 && fast.phases && fast.phases->cycle() == 4, "60 ticks a second: four ticks to a pass");
    fast.batch(40, 12, 80);
    const auto split = fast.spread_of_last(80);
    CHECK(split.first >= 9 && split.second <= 11, text("forty players over four ticks: %zu to %zu passes on a tick (10 each)", split.first, split.second).c_str());
    Crowd slow(/*spread=*/true, 20);
    CHECK(slow.per_pass == 1 && !slow.phases, "20 ticks a second: every tick is a pass, there is nothing to spread (no phases)");
    slow.batch(10, 4, 20);
    const auto all = slow.spread_of_last(20);
    CHECK(all.first == 10 && all.second == 10, "every view passes on every tick");
}
AOC_REGISTER_TEST(test_view_passes_spread_at_other_rates);
