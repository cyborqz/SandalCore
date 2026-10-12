// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/emit/actor_bunch.h
//
//  A reliable, non-partial data bunch on an open actor channel: bControl 0,
//  not paused, reliable, the channel, bHasPackageMapExports, no must-map
//  GUIDs, not partial, the channel sequence, the hardcoded channel name
//  NAME_Actor, BunchDataBits, then the payload (starting with the export
//  section when `has_exports`).
// ============================================================================
#pragma once

#include "protocol/emit/bunch_writer.h"
#include "protocol/wire/packet_parser.h"
#include <cstddef>
#include <cstdint>

namespace aoc { namespace protocol { namespace emit {

/// Appends the bunch carrying `bits` bits of `src` from bit `src_bit`.
inline void write_reliable_actor_bunch(BunchWriter& out, uint32_t channel, uint32_t ch_seq,
                                       bool has_exports, const uint8_t* src, size_t src_bit,
                                       size_t bits) {
    out.write_bit(0);                       // bControl
    out.write_bit(0);                       // bIsReplicationPaused
    out.write_bit(1);                       // bReliable
    out.write_sip(channel);
    out.write_bit(has_exports ? 1 : 0);     // bHasPackageMapExports
    out.write_bit(0);                       // bHasMustBeMappedGUIDs
    out.write_bit(0);                       // bPartial
    out.write_serialize_int(ch_seq, wire::MAX_CHSEQ);
    out.write_bit(1);                       // ChName.bIsHardcoded
    out.write_sip(102);                     // NAME_Actor
    out.write_serialize_int(static_cast<uint32_t>(bits), wire::MAX_PKT_BITS);
    out.write_bit_range(src, src_bit, bits);
}

}}}  // namespace aoc::protocol::emit
