// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/simulation/world_clock.h
//
//  Configurable tick rates for the 4 separate timing domains in the server.
//  Per architectural correction 3: simulation and replication DO NOT share a
//  single tick rate.  Each can be tuned independently.
//
//  LAYER:  World / simulation
//  SESSION: D
// ============================================================================
#pragma once

#include <chrono>
#include <cstdint>

namespace aoc { namespace world { namespace simulation {

/// Tick-rate configuration.  Defaults are MMO-standard; adjust in config
/// or via launcher flags when tuning.
struct TickConfig {
    uint32_t simulation_hz  = 30;   // physics, AI, combat math
    uint32_t replication_hz = 20;   // outgoing delta packets
    uint32_t movement_hz    = 30;   // FFastActorLocationArray batches
    uint32_t persistence_hz = 1;    // DB writes, autosave
};

/// Monotonic clock source for world time.  Not tied to wall-clock; resets
/// on server start.  Timestamps are uint64 milliseconds since start.
class WorldClock {
public:
    WorldClock() : start_(std::chrono::steady_clock::now()) {}

    /// Milliseconds since start.
    uint64_t now_ms() const {
        auto delta = std::chrono::steady_clock::now() - start_;
        return std::chrono::duration_cast<std::chrono::milliseconds>(delta).count();
    }
// CoA and C | Y | B

    /// Tick interval for a given frequency (returns std::chrono::duration).
    static std::chrono::microseconds interval_us(uint32_t hz) {
        if (hz == 0) return std::chrono::microseconds::zero();
        return std::chrono::microseconds(1'000'000 / hz);
    }

    /// Manual testing hook: advance the clock by an explicit amount.
    /// NOT used in production; lets unit tests set simulation_hz=0 and
    /// step deterministically.
    void advance_test_ms(uint64_t ms) { manual_advance_ms_ += ms; }

    uint64_t test_advance_ms() const { return manual_advance_ms_; }

private:
    std::chrono::steady_clock::time_point start_;
    uint64_t manual_advance_ms_ = 0;
};

}}} // namespace aoc::world::simulation
