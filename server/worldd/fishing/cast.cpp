// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  fishing/cast.cpp (see cast.h)
// ============================================================================
#include "fishing/cast.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace aoc::worldd::fishing {

const char* step_name(Step s) {
    switch (s) {
        case Step::Reply: return "reply";
        case Step::CooldownEnd: return "cooldown end";
        case Step::Bite: return "bite";
        case Step::Result: return "result";
        case Step::CastFail: return "cast fail";
        case Step::Cancel: return "cancel";
        case Step::BuffOff: return "buff off";
        case Step::Clear: return "clear";
    }
    return "?";
}

const char* stop_name(Stop s) {
    switch (s) {
        case Stop::None: return "none";
        case Stop::Moving: return "the player moved";
        case Stop::Combat: return "the player is in combat";
        case Stop::Tool: return "the rod is gone or broke";
        case Stop::Dead: return "the player died";
        case Stop::Mounted: return "the player mounted";
        case Stop::Replaced: return "another cast replaced it";
        case Stop::Ended: return "the session ended";
    }
    return "?";
}

Cast::Cast(double trigger_at, bool lands, double loop_seconds, Timing timing) : t_(timing), trigger_at_(trigger_at), loop_seconds_(std::max(0.0, loop_seconds)), lands_(lands) {
    queue(Step::Reply, trigger_at + t_.reply);
}

void Cast::queue(Step s, double at) {
    const Event e{s, at};
    const auto where = std::upper_bound(queue_.begin(), queue_.end(), e, [](const Event& a, const Event& b) { return a.at < b.at; });
    queue_.insert(where, e);
}

void Cast::remove(Step s) {
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [s](const Event& e) { return e.step == s; }), queue_.end());
}

/// The next bite: the channel's length from `from`, then a geometric number of ticks (a chance of 1 in `bite_one_in` each tick, the first test one tick after the channel).
void Cast::begin_loop(double from, stats::SplitMix& rng) {
    const double p = 1.0 / std::max(1, t_.bite_one_in);
    double ticks = 1.0;
    if (p < 1.0) {
        const double u = std::min(rng.rand_double(), 1.0 - 1e-12);   // [0, 1)
        ticks = std::floor(std::log(1.0 - u) / std::log(1.0 - p)) + 1.0;
    }
    queue(Step::Bite, from + loop_seconds_ + ticks * t_.tick);
}

std::vector<Event> Cast::advance(double now, stats::SplitMix& rng) {
    std::vector<Event> out;
    while (!queue_.empty() && queue_.front().at <= now) {
        const Event e = queue_.front();
        queue_.erase(queue_.begin());
        switch (e.step) {
            case Step::Reply:
                buff_on_ = true;
                cooling_ = true;
                first_check_ = e.at + t_.first_check;
                queue(Step::CooldownEnd, e.at + t_.cooldown);
                if (lands_) {
                    phase_ = Phase::Waiting;
                    begin_loop(e.at + t_.lure, rng);
                } else {
                    queue(Step::CastFail, e.at + t_.lure);
                }
                break;
            case Step::CooldownEnd:
                cooling_ = false;
                break;
            case Step::Bite:
                ++bites_;
                phase_ = Phase::Reeling;
                queue(Step::Result, e.at + t_.bite_to_result);
                break;
            case Step::Result:
                ++results_;
                phase_ = Phase::Waiting;
                begin_loop(e.at, rng);
                break;
            case Step::CastFail:
                phase_ = Phase::Ending;
                queue(Step::BuffOff, e.at + t_.fail_buff_off);
                queue(Step::Clear, e.at + t_.fail_clear);
                break;
            case Step::Cancel:
                phase_ = Phase::Ending;
                break;
            case Step::BuffOff:
                buff_on_ = false;
                break;
            case Step::Clear:
                phase_ = Phase::Done;
                buff_on_ = false;
                break;
        }
        out.push_back(e);
    }
    return out;
}

bool Cast::cancel(double now, Stop why) {
    if ((phase_ != Phase::Waiting && phase_ != Phase::Reeling) || now < first_check_) return false;
    remove(Step::Bite);
    remove(Step::Result);
    stop_ = why;
    queue(Step::Cancel, now);
    queue(Step::BuffOff, now + t_.buff_off_after_end);
    queue(Step::Clear, now + t_.clear_after_end);
    phase_ = Phase::Ending;
    return true;
}

void Cast::drop() {
    queue_.clear();
    phase_ = Phase::Done;
    buff_on_ = false;
    cooling_ = false;
}

bool Cast::result_due(double now) const { return !queue_.empty() && queue_.front().step == Step::Result && queue_.front().at <= now; }

double Cast::next_due() const { return queue_.empty() ? std::numeric_limits<double>::infinity() : queue_.front().at; }

}  // namespace aoc::worldd::fishing
