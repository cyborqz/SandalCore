// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/actors/game_state.h
//
//  HYPOTHESIS: the AGameState (or AGameStateBase) actor.
//  CONFIDENCE: high.
//
//  Evidence:
//   * 2278-bit ActorOpens, 3 instances.  UE5's AGameState is replicated to
//     all clients and typically opens early in the bootstrap.
//   * Unique bit size compared to characters/NPCs.
//   * Low channel numbers (32, 134, 222) — early-lifecycle actors.
//   * Every UE5 multiplayer game has exactly one GameState actor per world,
//     but AoC might have auxiliary actors (GameMode-replicated, team state,
//     match state) which could account for multiple 2278-bit opens.
//
//  Likely candidates:
//   - AGameState / AGameStateBase
//   - A custom AoC replicated-state actor (match info, node / castle state)
//   - A player session singleton
//
//  Phase 2 scope: metadata only.  Raw bytes in bootstrap_data.h.
// ============================================================================
#pragma once

#include "protocol/actors/actor_base.h"

namespace aoc { namespace protocol { namespace actors { namespace game_state {

inline constexpr ActorSpec kSpec = {
    /* channel         */ 32,    // representative channel
    /* bunch_data_bits */ 2278,
    /* ch_sequence     */ 1978,
    /* instance_count  */ 3,
    /* hypothesis      */ "AGameState or similar world-level replicated actor",
};

inline constexpr uint16_t kChannels[] = { 32, 134, 222 };

}}}} // namespace aoc::protocol::actors::game_state
