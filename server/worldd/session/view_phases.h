// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/view_phases.h
//
//  Which tick of its cycle each player's view pass runs on.
//
//  A view pass is due every n-th tick (n = 2 at 30 ticks a second: 15 passes a second, the original server's; tick_view_timing in net/tick_loop.h) and costs
//  about the same for every pass of a player.  Left to the clock alone, the tick of a player's first pass is the tick of all its later ones, and players who
//  join on the same tick (or an even number of ticks apart: a batch of joins comes 200 ms apart) all pass together: one tick of each pair carries every pass,
//  the other none, so the heavy one runs late while the server is far from busy.
//
//  The server therefore gives each view a phase, 0 to n-1, and the view passes on the ticks whose number is that phase modulo n.  A view takes the phase that
//  holds the fewest views when it first passes (the phase of the tick it is on when they tie, so a lone view passes at once), gives it back when it ends, and
//  moves to the lightest phase at a pass of its own when its phase holds two or more views than that one.  The ticks are counted by the world step (once per
//  endpoint tick, before the sessions'), so a tick that ran late and was caught up, or a restart of the tick grid, changes nobody's phase.
//
//  One thread uses it (the network thread's tick), so nothing here is locked.
// ============================================================================
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace aoc { namespace worldd {

class ViewPhases {
public:
    /// `ticks_per_pass`: the ticks between two passes of one view (1: every tick, nothing to spread).
    explicit ViewPhases(unsigned ticks_per_pass) : cycle_(std::max(1u, ticks_per_pass)), views_(cycle_, 0) {}

    unsigned cycle() const { return cycle_; }
    uint64_t tick() const { return tick_; }
    size_t   views(unsigned phase) const { return phase < cycle_ ? views_[phase] : 0; }

    /// A new tick begins: called once per endpoint tick, before the sessions' ticks.
    void begin_tick() { ++tick_; }

    /// A view starts passing: the phase it takes (one more view there).
    unsigned join() {
        const unsigned phase = lightest();
        ++views_[phase];
        return phase;
    }

    /// A view that took `phase` ends.
    void leave(unsigned phase) {
        if (phase < cycle_ && views_[phase] > 0) --views_[phase];
    }

    /// True when a view of `phase` passes on this tick.
    bool due(unsigned phase) const { return tick_ % cycle_ == phase; }

    /// A view of `phase` that is passing now: the phase it passes on from here.  It moves to the lightest phase when its own holds at least two more views.
    unsigned balance(unsigned phase) {
        if (cycle_ < 2 || phase >= cycle_) return phase;
        const unsigned light = lightest();
        if (views_[phase] < views_[light] + 2) return phase;
        --views_[phase];
        ++views_[light];
        return light;
    }

private:
    /// The phase with the fewest views; on a tie the one whose tick comes first (this tick's, then the next ones).
    unsigned lightest() const {
        unsigned best = static_cast<unsigned>(tick_ % cycle_);
        for (unsigned k = 1; k < cycle_; ++k) {
            const unsigned phase = static_cast<unsigned>((tick_ + k) % cycle_);
            if (views_[phase] < views_[best]) best = phase;
        }
        return best;
    }

    unsigned              cycle_;
    uint64_t              tick_ = 0;
    std::vector<size_t>   views_;
};

}}  // namespace aoc::worldd
