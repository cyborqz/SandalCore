// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// Phase D1 — no-op bunch generator.
//
// This generator exists to exercise the Phase D plumbing without putting
// any new bytes on the wire.  On every tick it:
//   - is invoked by the outbound builder,
//   - logs "[GEN] noop tick=N session_ms=M" at a throttled cadence,
//   - returns std::nullopt.
//
// If we ever see the [GEN] log line in the server output during a live
// client session, the pull interface is wired correctly.  When D2 swaps
// this for a real generator, no other code needs to change.

#include "generators/generators.h"

#include <memory>
#include <spdlog/spdlog.h>

namespace aoc::generators {

class NoopGenerator final : public BunchGenerator {
public:
    explicit NoopGenerator(uint32_t channel) : channel_(channel) {}

    std::string_view name() const override { return "noop"; }
    uint32_t channel() const override { return channel_; }

    std::optional<GeneratedBunch> tick(const TickContext& ctx) override {
        // Throttled log: first five ticks, then every 500th tick.  Enough to
        // prove the generator was invoked without flooding the server log
        // during a full replay run (~29k ticks).
        if (ctx.tick_index < 5 || (ctx.tick_index % 500 == 0)) {
            spdlog::info("[GEN] noop ch={} tick={} session_ms={} preview_seq={} cli_ack={}",
                         channel_, ctx.tick_index, ctx.session_ms,
                         ctx.out_seq_preview, ctx.last_client_ack);
        }
        return std::nullopt;
    }

private:
    uint32_t channel_ = 0;
};

// ── Factory ──────────────────────────────────────────────────────────────
// Kept out of the header so the generator class itself is private to the
// translation unit.  game_server.h includes bunch_generator.h and calls
// make_noop_generator() via the small factory table.
std::unique_ptr<BunchGenerator> make_noop_generator(uint32_t channel) {
    return std::make_unique<NoopGenerator>(channel);
}

} // namespace aoc::generators
