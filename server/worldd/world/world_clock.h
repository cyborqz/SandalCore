// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/world_clock.h: the game day, one clock for the sky and for the hour the spawns are judged at (header only)
//
//  The game client keeps its own game clock between the server's pushes (ClientSetTime) and runs it by the game's rule, so the server has to run the same rule or the sky jumps at every
//  push.  The rule:
//    * the day, from hour 6 up to hour 18, runs 6 game seconds per real second;
//    * the night, from hour 18 up to hour 6, runs 9 game seconds per real second (the day's 6 times the client's night factor of 1.5).
//  An edge belongs to the period that starts there: hour 6 is day, hour 18 is night.  A game day is 86,400 game seconds and takes 200 real minutes (7,200 s of day and 4,800 s of night);
//  a game hour is 10 real minutes by day and 6 minutes 40 seconds by night.
//
//  Game time is a number of seconds since the game's year 1, in years of 365 days: the number ClientSetTime carries.  The clock is a pure function of the world time (the seconds since
//  worldd started): the date and hour it started at, advanced by the rule.  Nothing is saved and the PC's clock is not read, so every start begins at the same hour of the same date.
//
//  The closed form.  A cycle is one game day counted from hour 6 and takes 12,000 real seconds.  With c the real seconds since the cycle the clock began in started, k = floor(c / 12,000)
//  and p = c - 12,000 k (0 <= p < 12,000):
//      game time = (first day + k) x 86,400 + 21,600 + (p < 7,200 ? 6 p : 43,200 + 9 (p - 7,200)).
//  So the game time rises by 86,400 over every 12,000 s, from whatever hour the clock started at.
// ============================================================================
#pragma once

#include <cmath>
#include <cstdio>
#include <string>

namespace aoc::worldd::world {

/// The hour every start of worldd begins at, unless --world-hour (or its old name, --spawn-hour) asks for another: the game's morning.  This constant is the ONE place the default is chosen.
inline constexpr double kDefaultStartHour = 7.0;

class WorldClock {
public:
    // ---- the rule ---------------------------------------------------------------------------------------------------
    static constexpr double kGameDay = 86400.0;                                          // game seconds in a game day
    static constexpr double kDayStart = 6.0 * 3600.0;                                    // the second of a game day at which the day's rate starts (hour 6)
    static constexpr double kNightStart = 18.0 * 3600.0;                                 // ... and the night's (hour 18)
    static constexpr double kDayRate = 6.0;                                              // game seconds per real second from hour 6 up to hour 18
    static constexpr double kNightRate = 9.0;                                            // ... from hour 18 up to hour 6
    static constexpr double kDayGameSeconds = kNightStart - kDayStart;                   // 43,200 game seconds of day
    static constexpr double kDayReal = kDayGameSeconds / kDayRate;                       // 7,200 real seconds
    static constexpr double kNightReal = (kGameDay - kDayGameSeconds) / kNightRate;      // 4,800 real seconds
    static constexpr double kCycle = kDayReal + kNightReal;                              // 12,000 real seconds: one game day
    /// The day every start begins on, counted from the game's year 1 (year 5, day 33 of years of 365 days).
    static constexpr double kStartDay = 1858.0;

    // ---- making a clock ---------------------------------------------------------------------------------------------
    /// The clock that reads `game_time` (seconds since year 1) when the world time is `at` seconds.
    static WorldClock reading(double game_time, double at = 0.0) {
        const double day = std::floor(game_time / kGameDay);
        const double second = game_time - day * kGameDay;                // 0 <= second < 86,400
        const bool after_start = second >= kDayStart;                    // before hour 6 the clock still belongs to the cycle that started the day before
        const double cycle_day = after_start ? day : day - 1.0;
        const double since = after_start ? second - kDayStart : second + (kGameDay - kDayStart);   // game seconds since hour 6
        const double p = since < kDayGameSeconds ? since / kDayRate : kDayReal + (since - kDayGameSeconds) / kNightRate;
        return WorldClock((cycle_day - kStartDay) * kCycle + p - at);
    }

    /// The clock that reads `hour` (0 up to 24) on the start date when the world time is 0: what worldd starts with.
    static WorldClock starting_at(double hour) {
        const double h = hour - 24.0 * std::floor(hour / 24.0);
        return reading(kStartDay * kGameDay + h * 3600.0, 0.0);
    }

    // ---- reading it -------------------------------------------------------------------------------------------------
    /// The game time (seconds since year 1) when the world time is `world_seconds`.
    double game_time(double world_seconds) const {
        const double c = cycle_ + world_seconds;
        const double k = std::floor(c / kCycle);
        const double p = c - k * kCycle;
        const double into = p < kDayReal ? kDayRate * p : kDayGameSeconds + kNightRate * (p - kDayReal);
        return (kStartDay + k) * kGameDay + kDayStart + into;
    }

    /// The game hour, 0 up to 24 (7.5 is half past seven in the morning), when the world time is `world_seconds`.
    double hour(double world_seconds) const {
        double h = std::fmod(game_time(world_seconds), kGameDay) / 3600.0;
        if (h < 0.0) h += 24.0;
        if (h >= 24.0) h -= 24.0;
        return h;
    }

    /// True from hour 18 up to hour 6 (the night's edges: hour 18 is night, hour 6 is day).
    static bool is_night(double hour) { return hour >= 18.0 || hour < 6.0; }

private:
    explicit WorldClock(double cycle) : cycle_(cycle) {}
    double cycle_ = 0;   // the real seconds from the start of the cycle that began on kStartDay to world time 0 (negative: world time 0 falls before it)
};

/// The clock a start of worldd gets when nobody asks for another hour: every start begins at kDefaultStartHour.
inline WorldClock default_world_clock() { return WorldClock::starting_at(kDefaultStartHour); }

/// An hour as the game's map window shows it: "7:35 AM".  Whole game minutes, rounded down.
inline std::string hour_text(double hour) {
    const int minutes = static_cast<int>(std::floor(hour * 60.0 + 1e-6));
    const int h = (minutes / 60) % 24, m = minutes % 60;
    const int h12 = h % 12 == 0 ? 12 : h % 12;
    char text[32];
    std::snprintf(text, sizeof(text), "%d:%02d %s", h12, m, h >= 12 ? "PM" : "AM");
    return text;
}

}  // namespace aoc::worldd::world
