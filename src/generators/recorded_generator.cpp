// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// Phase D2 — pre-recorded bunch generator.
//
// Emits a single hard-coded bunch payload once, when the session clock reaches
// emit_at_ms.  Used to validate the end-to-end framing+send path on a known-
// good recorded channel (picked from Phase B/C classification output) before
// we start synthesizing bunches programmatically in D3+.
//
// Scope for D2:
//   - Single-shot emit (one bunch per session, no retransmission).
//   - Unreliable only (b_reliable=false, no ChSequence/ChName).
//   - No partial support.
//   - Caller supplies the payload bytes + bit_count verbatim from the
//     blueprint_bunches.csv row.

#include "generators/generators.h"

#include <memory>
#include <spdlog/spdlog.h>
#include <utility>
#include <vector>

namespace aoc::generators {

class RecordedBunchGenerator final : public BunchGenerator {
public:
    RecordedBunchGenerator(uint32_t channel,
                           std::vector<uint8_t> payload,
                           uint32_t bit_count,
                           uint64_t emit_at_ms,
                           std::string label)
        : channel_(channel),
          payload_(std::move(payload)),
          bit_count_(bit_count),
          emit_at_ms_(emit_at_ms),
          label_(std::move(label)) {}

    std::string_view name() const override { return "recorded"; }
    uint32_t channel() const override { return channel_; }

    std::optional<GeneratedBunch> tick(const TickContext& ctx) override {
        if (emitted_) return std::nullopt;
        if (ctx.session_ms < emit_at_ms_) {
            // Throttled "waiting" log: first few ticks then every 500th.
            if (ctx.tick_index < 3 || (ctx.tick_index % 500 == 0)) {
                spdlog::debug("[GEN] recorded ch={} waiting: session_ms={} < emit_at_ms={}",
                              channel_, ctx.session_ms, emit_at_ms_);
            }
            return std::nullopt;
        }
// [Cyb :: C-O-A]

        GeneratedBunch b;
        b.channel       = channel_;
        b.b_control     = false;
        b.b_reliable    = false;
        b.b_partial     = false;
        b.b_has_exports = false;
        b.b_has_must_map= false;
        b.b_is_replication_paused = false;
        b.ch_name       = "";   // unreliable: no name on wire
        b.payload       = payload_;
        b.bit_count     = bit_count_;

        emitted_ = true;
        spdlog::info("[GEN] recorded ch={} label='{}' EMIT tick={} session_ms={} "
                     "bits={} bytes={} out_seq_preview={}",
                     channel_, label_, ctx.tick_index, ctx.session_ms,
                     bit_count_, payload_.size(), ctx.out_seq_preview);
        return b;
    }

private:
    uint32_t channel_;
    std::vector<uint8_t> payload_;
    uint32_t bit_count_;
    uint64_t emit_at_ms_;
    std::string label_;
    bool emitted_ = false;
};

std::unique_ptr<BunchGenerator> make_recorded_generator(
    uint32_t channel,
    std::vector<uint8_t> payload,
    uint32_t bit_count,
    uint64_t emit_at_ms,
    std::string label) {
    return std::make_unique<RecordedBunchGenerator>(
        channel, std::move(payload), bit_count, emit_at_ms, std::move(label));
}

} // namespace aoc::generators
