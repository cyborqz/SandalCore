// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// Phase D1 — pull-based bunch generator interface.
//
// A BunchGenerator is asked once per outbound tick whether it wants to emit
// a bunch on its assigned channel.  The generator is free to return
// std::nullopt (quiescent — generator does nothing this tick).  When it does
// return a bunch, the outbound builder is responsible for framing it (bunch
// header, packet header, sequence management) and sending it to the client.
//
// D1 scope intentionally keeps this interface tiny:
//   - One generator owns one channel index.
//   - The generator is invoked in the same thread/cadence as the replay
//     packet loop (one opportunity per replay packet iteration).
//   - No actor/object references — raw channel + raw payload bits only.
//   - No retransmission handling — D1 emits unreliable only.
//
// D2+ will extend this with:
//   - Reliable bunch support (ChSequence assignment, retransmission queue)
//   - Partial-multi-bunch support
//   - Richer TickContext (replica state, player position, world time)
//   - Multiple generators per channel (priority-ordered)

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aoc {

/// Minimal snapshot of server state handed to a generator on each tick.
/// The generator must NOT hold pointers into this struct past the call.
struct TickContext {
    /// Monotonically increasing tick counter for this client session.
    /// Starts at 0 on the first generator invocation for the session.
    uint64_t tick_index = 0;

    /// Wall-clock milliseconds since the session began sending replay/live
    /// data (i.e. since the map-load gate opened).  Generators that need to
    /// pace themselves (e.g. "emit every 500 ms") should use this, not
    /// tick_index, because tick cadence varies with replay pacing.
    uint64_t session_ms = 0;

    /// Outbound server sequence that will be assigned to the packet this
    /// bunch rides in, if the generator emits.  Informational only —
    /// generators should not depend on it for correctness.
    uint16_t out_seq_preview = 0;

    /// Last sequence the client has ACKed.  A generator may use this to
    /// back off when the client is behind, but D1 generators should not.
    uint16_t last_client_ack = 0;
};
// C Y B captures, C-O-A code

/// A bunch produced by a generator, in the form the outbound builder can
/// frame directly.  Bits are packed LSB-first per UE5 convention; the
/// builder will consume them as-is after writing the bunch header.
struct GeneratedBunch {
    /// Channel index the bunch is addressed to (must match the generator's
    /// configured channel — the builder validates).
    uint32_t channel = 0;

    /// Bunch flags, in the order used by parse_sc_bunch (AoC layout):
    ///   bControl, bCtrlOpen, bClose, close_reason (only when bClose),
    ///   bIsReplicationPaused, bReliable, bHasExports, bHasMustMap,
    ///   bPartial (+ bPartialInitial / bPartialCEF / bPartialFinal).
    /// D1 generators emit bControl=0, bReliable=0, bPartial=0, bExports=0,
    /// bMustMap=0, bIsReplicationPaused=0.  Fields not used when a flag is
    /// zero must be left at their defaults.
    bool b_control               = false;
    bool b_ctrl_open             = false;
    bool b_close                 = false;
    uint32_t close_reason        = 0;
    bool b_is_replication_paused = false;
    bool b_reliable              = false;
    bool b_has_exports           = false;
    bool b_has_must_map          = false;
    bool b_partial               = false;
    bool b_partial_initial       = false;
    bool b_partial_cef           = false;
    bool b_partial_final         = false;

    /// Optional channel name, used only when bReliable or bCtrlOpen.  D1
    /// generators always leave this empty (unreliable bunches carry no
    /// ChName).
    std::string ch_name;

    /// Payload, packed LSB-first.  bit_count is the authoritative size;
    /// payload.size() must be at least (bit_count + 7) / 8.
    std::vector<uint8_t> payload;
    uint32_t bit_count = 0;
};

/// Abstract pull-based bunch source.  One instance per registered channel.
class BunchGenerator {
public:
    virtual ~BunchGenerator() = default;

    /// Unique name used in logs ("[GEN:<name>] ch=N ...") and in the
    /// generated_channels.json allowlist.  Must be stable across runs.
    virtual std::string_view name() const = 0;

    /// Channel index this generator owns.  Zero means "unassigned / noop".
    virtual uint32_t channel() const = 0;

    /// Called once per outbound tick.  Return std::nullopt to stay silent
    /// this tick.  The returned bunch must have channel() matching the
    /// generator's configured channel (or the builder rejects it).
    virtual std::optional<GeneratedBunch> tick(const TickContext& ctx) = 0;
};

} // namespace aoc
