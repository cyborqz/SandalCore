// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/actors/player_controller.h
//
//  HYPOTHESIS: the APlayerController actor for the local player.
//  CONFIDENCE: very high.
//
//  Evidence:
//   * First ActorOpen in the bootstrap after NMT handshake completes.
//   * Unique in the capture (2 occurrences per session: spawn + reopen).
//   * Opened on channel 3 — UE5 reserves low channel numbers for critical
//     actors (0=control channel, 1=NMT, ~3=PlayerController is conventional).
//   * Bunch-data size 3302 bits fits an actor with ~411 RepLayout fields
//     and a subset of 68 initial property values (mask analysis).
//
//  This actor is THE player's own controller.  When we start synthesizing
//  in Phase 3, this is the most important actor to get right — the client
//  won't enter the world without accepting a PlayerController spawn.
//
//  Phase 2 scope: metadata only.  Raw bytes still live in
//  protocol/bootstrap/bootstrap_data.h (packet index 22 of the bootstrap).
//  Phase 3 will add a build_from_profile(profile) function here.
// ============================================================================
#pragma once

#include "protocol/actors/actor_base.h"

namespace aoc { namespace protocol { namespace actors { namespace player_controller {

inline constexpr ActorSpec kSpec = {
    /* channel         */ 3,
    /* bunch_data_bits */ 3302,
    /* ch_sequence     */ 1978,
    /* instance_count  */ 1,
    /* hypothesis      */ "APlayerController (local player controller)",
};
// C.O.A captures, Cyb code

// ── Builder (Phase 3.6) ─────────────────────────────────────────────────
// Emits a complete ActorOpen bunch for THIS player's PlayerController.
//
// What's synthesized from the profile vs. copied from captured data:
//   [SYNTHESIZED]
//     - Bunch header (bCtrl, bOpen, bReliable, chSeq, BunchDataBits, ...)
//     - bHasRepLayoutExport, NumExports, 411-bit export mask
//     - Actor/Archetype/Level NetGUIDs (fixed values: 1, 120, 10)
//     - 4 SerializeNewActor flags + optional compressed rotator
//   [COPIED from captured pkt 14287]
//     - The ~1,600 initial property-value bits (Phase M5 territory).
//       These get spliced in byte-identical until property formats are
//       decoded per-field.
//
// The builder's output is intended to be BIT-EQUIVALENT to the captured
// bunch when called with a profile that matches the captured player
// (name=RandomChar, default transform).  Differences in profile produce
// different bits in the known fields, but the unknown property tail
// stays the same.  This lets us validate incrementally.
}}}} // namespace aoc::protocol::actors::player_controller

// ── Outside the namespace, above the builder, to avoid circular deps.
#include "protocol/bunch_builder.h"
#include "protocol/character_profile.h"

namespace aoc { namespace protocol { namespace actors { namespace player_controller {

/// Build a full PlayerController ActorOpen bunch payload for a live
/// player.  Returns true on success.  The caller supplies:
///   * `out`      — empty BunchBuffer to receive the payload bits
///                  (everything AFTER the bunch header — the header is
///                  written by the caller via write_sc_packet_prefix).
///   * `profile`  — player identity + spawn state.
bool build(BunchBuffer& out, const CharacterProfile& profile);

}}}} // namespace aoc::protocol::actors::player_controller
