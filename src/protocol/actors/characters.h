// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/actors/characters.h
//
//  HYPOTHESIS: character-like actors (either BP_Character instances or
//              APlayerState — analysis pending).
//  CONFIDENCE: medium.
//
//  Evidence:
//   * 4326-bit ActorOpens, 7 instances in the bootstrap.
//   * Appear on sequentially-opened channels (14, 24, 40, 43, 51, 67) after
//     the PlayerController spawn.
//   * All have chSeq=1978 (first reliable on a fresh channel).
//   * Consistent bit size = same class type.
//
//  Could be:
//   (a) BP_Character — actor-based characters (most likely)
//   (b) APlayerState — per-player state actors (also plausible)
//   (c) Some AoC-custom base class wrapping character state
//
//  Phase 3 will resolve this by decoding ArchetypeGUID from one of these
//  bunches.  For now we treat them as a homogeneous class.
//
//  Phase 2 scope: metadata only.  Raw bytes in bootstrap_data.h.
// ============================================================================
#pragma once

#include "protocol/actors/actor_base.h"

namespace aoc { namespace protocol { namespace actors { namespace characters {

inline constexpr ActorSpec kSpec = {
    /* channel         */ 14,    // representative channel (first instance)
    /* bunch_data_bits */ 4326,
    /* ch_sequence     */ 1978,
    /* instance_count  */ 7,
    /* hypothesis      */ "BP_Character or APlayerState (7 instances)",
};

// The 7 channels this class appears on in the bootstrap.  Order matches the
// wire-send order.
inline constexpr uint16_t kChannels[] = { 14, 24, 40, 43, 51, 67 };
// (Note: 6 listed here; a 7th instance appears later in the bootstrap but
//  its channel number varies and is captured via packet scan in Phase 3.)

}}}} // namespace aoc::protocol::actors::characters
