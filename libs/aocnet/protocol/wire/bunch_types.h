// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/wire/bunch_types.h
//
//  Data structures for parsed UE5 bunches.  Pure data — no parsing logic
//  lives here.  Shared between bunch_parser (producer) and opcode_dispatcher
//  (consumer).
//
//  LAYER:  Protocol / wire
//  SESSION: A (the parser)
// ============================================================================
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <optional>

namespace aoc { namespace protocol { namespace wire {

/// Outer packet structure (UE5 UDP payload after PacketHandler strip).
struct ParsedPacket {
    uint16_t seq = 0;              // 14-bit sender seq
    uint16_t ack_seq = 0;          // 14-bit last-seen receiver seq
    uint32_t hist_word_count = 0;  // ack history word count
    std::vector<uint32_t> history; // raw ack-history words

    bool     has_pkt_info = false;
    uint32_t jitter_ms = 0;
    bool     has_srv_frame = false;
    uint8_t  frame_time_byte = 0;

    uint64_t custom_field = 0;     // AoC's 48-bit custom field

    // The realigned inner data (bits starting at 0 of the inner stream).
    // Keep a copy or a span into the original — we'll use the original
    // packet buffer and track positions instead.
    size_t inner_start_bit = 0;    // bit offset in the original buffer where inner data begins
    size_t inner_end_bit = 0;      // effective end (from strip_termination)
// (Cyb ~ C | O | A)

    // Parsed bunches
    std::vector<class ParsedBunch> bunches;
};

/// A single bunch extracted from a packet.
struct ParsedBunch {
    bool    is_control = false;        // bControl
    bool    is_open = false;           // bOpen
    bool    is_close = false;          // bClose
    uint32_t close_reason = 0;         // only valid if is_close
    bool    is_paused = false;         // bIsReplicationPaused
    bool    is_reliable = false;       // bReliable

    uint32_t channel = 0;              // ChIndex
    std::string channel_name;          // resolved from static_parse_name (e.g. "EName[102]")

    bool    has_package_map_exports = false;  // bHasPackageMapExports
    bool    has_must_map_guids = false;       // bHasMustBeMappedGUIDs
    bool    is_partial = false;                // bPartial
    bool    partial_initial = false;           // first fragment
    bool    partial_has_comp_export = false;   // mid-fragment marker
    bool    partial_final = false;             // last fragment

    uint32_t ch_sequence = 0;          // reliable channel sequence

    uint32_t bunch_data_bits = 0;      // size of payload IN BITS
    size_t   data_start_bit = 0;       // offset IN OUTER PACKET where payload begins
    size_t   header_bits = 0;          // size of bunch header in bits (for diagnostics)

    /// True if this bunch is the only fragment of its logical bunch
    /// (NOT partial, OR partial_initial && partial_final both set).
    bool is_complete_bunch() const {
        return !is_partial || (partial_initial && partial_final);
    }
};

}}} // namespace aoc::protocol::wire
