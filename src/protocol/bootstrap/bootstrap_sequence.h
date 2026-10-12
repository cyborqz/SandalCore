// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/bootstrap/bootstrap_sequence.h
//
//  Entry point for the "no-replay" bootstrap source.  In Phase 1 this reads
//  from an embedded byte array (generated from replay_data.bin by
//  tools/extract_bootstrap.py).  In Phase 3 it will orchestrate calls into
//  per-actor builders that synthesize packets from a CharacterProfile.
//
//  Integration strategy: populate the EXISTING ReplayData struct so the
//  rest of the GameServer code path stays unchanged.  This keeps the diff
//  small and makes the file/embedded switch invisible to downstream code.
// ============================================================================
#pragma once

// Forward declaration: ReplayData lives in net/game_server.h.  We avoid
// including that large header here to minimise compile-time coupling.
struct ReplayData;

namespace aoc { namespace protocol {

// Forward declaration: CharacterProfile lives in protocol/character_profile.h.
// Pulled in at call sites — keeping it out of this header keeps the
// bootstrap sequence loadable without dragging in all synthesis headers.
struct CharacterProfile;

class BootstrapSequence {
public:
    /// Populate `out` with the bootstrap packets from the embedded data
    /// (Phase 1: raw bytes extracted from replay_data.bin).  Returns true
    /// on success.
    ///
    /// The GameServer uses this as a drop-in alternative to
    /// ReplayData::load(path) when --use-embedded-bootstrap is set.
    static bool fill(ReplayData& out);

    /// Phase 3.8 — Legacy: overwrite a 3302-bit region inside pkt[22]
    /// with synthesized PlayerController payload.  Kept for the spare
    /// `--use-embedded-bootstrap` path for now.  The proper Phase II
    /// approach is to build a FULL logical ActorOpen bunch from scratch
    /// (see GameServer::spawn_player_controller_for_client) — not patch
    /// a fragment of a multi-packet logical bunch.
    ///
    /// Returns true if the splice succeeded (default profile is byte-
    /// identical so this is a no-op in practice when the profile is
    /// zeroed).
    static bool apply_synthesis(ReplayData& data,
                                const CharacterProfile& profile);
// CoA captures, C.Y.B code

    // REMOVED: apply_live_pc_spawn / apply_live_pawn_spawn — they tried
    // to SPLICE synthesized bits over captured fragments (conceptually
    // broken, see bootstrap_sequence.cpp header comment).
    //
    // Phase II will generate the full logical ActorOpen from scratch
    // and deliver it via the send path.  For that we first need to
    // prove our synthesis produces byte-identical output to captured
    // when given the captured field values — that's what
    // `test_pc_spawn_round_trip` does.

    /// Option 1 / Phase II Stage 2.0 — DRY-RUN round-trip test.
    ///
    /// Parses the captured PC spawn bunch (pkt#22 bunch[0]), re-emits
    /// it via ActorBuilder using the parsed field values + spliced
    /// property stream tail, and compares bit-by-bit to captured.
    ///
    /// Purpose: validate that parser + emitter + ActorBuilder produce
    /// bit-identical output to real AoC wire format.  This is a
    /// prerequisite for actually SENDING synthesized bunches to the
    /// client.
    ///
    /// Returns true on byte-identical match.  Logs detailed diff on
    /// divergence.  NEVER mutates `data` (pure test function).
    ///
    /// Non-goals:
    ///   - Not an integration test.  We're not sending bytes anywhere.
    ///   - Not a multi-packet reassembly test.  We only test ONE
    ///     fragment (bunch[0] of pkt#22).
    ///   - Not a property-stream regeneration test.  The RepLayout
    ///     tail is spliced verbatim from captured.
    static bool test_pc_spawn_round_trip(const ReplayData& data);

    /// Phase II Stage 2.1 — splice the synthesized PC spawn bunch into
    /// the replay stream, so the client receives bytes produced by our
    /// ActorBuilder pipeline (rather than the captured bytes).
    ///
    /// This is **infrastructure-only** while synthesis is byte-identical:
    /// with no divergence, the client can't tell the difference.  But
    /// once we have per-property RE (cmd_index catalog) and start
    /// diverging values (custom name, class, etc.), the same splice
    /// mechanism delivers the diverged bytes — no additional send-path
    /// work needed.
    ///
    /// Safety gate: the synthesized bytes MUST be bit-identical to the
    /// captured pkt#22 bunch[0] before we splice.  If they diverge,
    /// the function aborts the write and logs a warning (replay
    /// continues with captured bytes, unchanged).  This prevents
    /// shipping corrupted packets while our synthesis is still being
    /// proven.
    ///
    /// Returns true on successful splice (which is a bit-level no-op
    /// under the safety gate, but proves the write path works).
    static bool splice_pc_spawn_synthesis(ReplayData& data);
};

}} // namespace aoc::protocol
