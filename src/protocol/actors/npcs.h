// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/actors/npcs.h
//
//  HYPOTHESIS: non-player characters (monsters / enemies / ambient mobs).
//  CONFIDENCE: medium.
//
//  Evidence:
//   * 5350-bit ActorOpens — the most common recurring class in the bootstrap.
//   * 19 instances, spread across mid-range channels 71..111 and 127, 182.
//   * Slightly larger than the 4326-bit `characters` class — consistent with
//     NPC actors carrying AI / combat state in addition to the basic
//     character replication.
//
//  Likely candidates (pending ArchetypeGUID decode in Phase 3):
//   - BP_NPC base class
//   - BP_Enemy / BP_Monster / BP_Creature specializations
//   - AI-controlled actors around the player's spawn point
//
//  Phase 2 scope: metadata only.  Raw bytes in bootstrap_data.h.
// ============================================================================
#pragma once

#include "protocol/actors/actor_base.h"

namespace aoc { namespace protocol { namespace actors { namespace npcs {

inline constexpr ActorSpec kSpec = {
    /* channel         */ 71,    // first NPC channel in the bootstrap
    /* bunch_data_bits */ 5350,
    /* ch_sequence     */ 1978,
    /* instance_count  */ 19,
    /* hypothesis      */ "NPC / enemy class (19 instances in starter zone)",
};

// Known channels carrying NPC ActorOpens in the captured bootstrap.
inline constexpr uint16_t kChannels[] = {
    71, 78, 90, 94, 100, 101, 104, 108, 110, 111, 127, 182
};
// (12 listed; additional 7 instances appear on other channels — will be
//  enumerated via packet scan in Phase 3.)

}}}} // namespace aoc::protocol::actors::npcs
