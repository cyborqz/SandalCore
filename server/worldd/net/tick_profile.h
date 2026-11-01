// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/tick_profile.h
//
//  Where worldd's tick spends its time, for the `[tick]` log line (worldd --loop-stats): the world's step and its parts, every session's
//  tick, each player's view of the world (the relevancy scan apart from the sends), every connection's sends, and the costliest session.
//  The network thread adds to it while `on` is set; main.cpp prints it and starts it again every interval.  Off (the default, and in
//  every test) nothing is measured and nothing changes.
// ============================================================================
#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace aoc { namespace worldd {

struct TickProfile {
    bool on = false;
    // the world's step (PlayerWorld::tick_world) and its parts
    double world_us = 0;
    double npcs_us = 0;        // the NPC world's steps and the towns' checks
    double combat_us = 0;      // the fights: pending hits, missiles, effects, the creatures' decisions
    double people_us = 0;      // the towns' people and roads following the nodes' levels
    double window_us = 0;      // the ground window (the chunks around the players)
    double aftermath_us = 0;   // corpses, loot and respawns
    // the sessions
    double sessions_us = 0;    // PlayerSession::tick of every session (its view included)
    double view_us = 0;        // ActorView::tick (the view of the world of every player)
    double view_scan_us = 0;   // of which the passes' registry queries, rings and ordering (the rest is opens, updates and closes)
    uint64_t view_passes = 0;
    double open_us = 0;        // of which the opens (an actor's whole state, new to the client, and its exports)
    uint64_t opens = 0;
    double update_us = 0;      // of which the updates of actors the client holds awake that changed
    double update_build_us = 0;  // ... of which the building of what changed (the rest: the sending)
    uint64_t updates = 0;
    double flush_us = 0;       // Connection::flush of every session: the packets built and sent
    // the session that cost the most since the profile started (its tick and its sends)
    std::string top_name;
    double top_us = 0, top_view_us = 0;
    size_t top_awake = 0, top_dormant = 0;

    /// Starts a new interval: everything at zero, `on` as it was.
    void restart() {
        const bool keep = on;
        *this = TickProfile{};
        on = keep;
    }
};

/// The network thread's profile (one per process).
inline TickProfile& tick_profile() {
    static TickProfile profile;
    return profile;
}

/// Microseconds from `from` to now, on the steady clock.
inline double tick_profile_us_since(std::chrono::steady_clock::time_point from) {
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - from).count();
}

/// Adds the time of its scope to `*slot` while the profile is on.
class TickProfileSpan {
public:
    explicit TickProfileSpan(double* slot) : slot_(tick_profile().on ? slot : nullptr) {
        if (slot_) started_ = std::chrono::steady_clock::now();
    }
    ~TickProfileSpan() {
        if (slot_) *slot_ += tick_profile_us_since(started_);
    }
    TickProfileSpan(const TickProfileSpan&) = delete;
    TickProfileSpan& operator=(const TickProfileSpan&) = delete;

private:
    double* slot_;
    std::chrono::steady_clock::time_point started_{};
};

}}  // namespace aoc::worldd
