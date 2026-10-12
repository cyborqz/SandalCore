// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/emit/net_serializers.h
//
//  Values of AoC structs that replicate through their own NetSerialize
//  (ICppStructOps vtable slot 10) rather than as flattened members.
//
//  FCharacterLoadTracker (AAoCPlayerController::CharacterLoadTracker):
//    NetSerialize 0x1463e7600 copies its flags into a TArray<uint8> and calls
//    0x1463d5eb0, a bounded bool-array serializer: the count in 6 bits
//    (FloorLog2(49) + 1; at most 25 flags), then every flag as FArchive's
//    bool, a 32-bit 0 or 1.  The original server stepped it from 1000...0
//    (pkt#46) through 1011...1 (pkt#72, before possession) to 0111...1
//    (pkt#229); the client began moving right after the last one.
// ============================================================================
#pragma once

#include "protocol/emit/rep_layout_writer.h"
#include <cstdint>
#include <vector>

namespace aoc { namespace protocol { namespace emit {

constexpr size_t kCharacterLoadTrackerMaxFlags = 25;

/// Writes the value alone: `flags` (at most kCharacterLoadTrackerMaxFlags).
inline void write_character_load_tracker_value(BunchWriter& out, const std::vector<bool>& flags) {
    const size_t n = flags.size() < kCharacterLoadTrackerMaxFlags ? flags.size()
                                                                  : kCharacterLoadTrackerMaxFlags;
    out.write_bits(n, 6);
    for (size_t i = 0; i < n; ++i) out.write_uint32(flags[i] ? 1u : 0u);
}

/// Writes `flags` (at most kCharacterLoadTrackerMaxFlags) as property `handle`.
inline void write_character_load_tracker(RepLayoutWriter& props, uint32_t handle,
                                         const std::vector<bool>& flags) {
    write_character_load_tracker_value(props.write_handle(handle), flags);
}

}}}  // namespace aoc::protocol::emit
