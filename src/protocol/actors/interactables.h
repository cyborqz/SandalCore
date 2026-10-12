// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/actors/interactables.h
//
//  HYPOTHESIS: interactable world objects (gathering nodes, quest items,
//              doors, loot chests, or similar).
//  CONFIDENCE: low-to-medium.
//
//  Evidence:
//   * 1254-bit ActorOpens, 8 instances.
//   * Size is between the 4326-bit characters and the tiny subobjects —
//     consistent with "actor with some state but no animation/AI".
//   * Opened after the world-actor waves (NPCs + characters) — matches the
//     order you'd expect for environment pickups / interactables to land.
//   * Channels scattered (164, 198, 247, 260, 114, ...).
//
//  Could be any of:
//   - Gathering nodes (ore veins, herb plants — AoC has many)
//   - Quest-given items / breadcrumb sparkles (we saw "DiscardedWeapon*"
//     and "Sparkly" strings in the replay)
//   - Doors, levers, signs
//   - Loot chests
//   - Resource-node markers
//
//  Without ArchetypeGUID decode we can't confirm.  Will revisit in Phase 3.
//
//  Phase 2 scope: metadata only.  Raw bytes in bootstrap_data.h.
// ============================================================================
#pragma once

#include "protocol/actors/actor_base.h"

namespace aoc { namespace protocol { namespace actors { namespace interactables {

inline constexpr ActorSpec kSpec = {
    /* channel         */ 114,   // representative channel
    /* bunch_data_bits */ 1254,
    /* ch_sequence     */ 1978,
    /* instance_count  */ 8,
    /* hypothesis      */ "Interactable world objects (nodes / items / props)",
};

inline constexpr uint16_t kChannels[] = { 114, 164, 198, 247, 260 };
// (5 listed; 3 additional instances scattered later in the bootstrap —
//  Phase 3 will enumerate via full packet scan.)

}}}} // namespace aoc::protocol::actors::interactables
