// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/wire/bunch_reassembler.h
//
//  Reassembles partial bunch chains.  UE5 splits large bunches across
//  multiple packets when they exceed MTU; each fragment has partial_initial
//  / partial_final flags.  A "complete chain" is:
//
//      partial_initial=1, partial_final=0  (first fragment)
//      partial_initial=0, partial_final=0  (zero or more middle fragments)
//      partial_initial=0, partial_final=1  (last fragment)
//
//  The reassembler walks a sequence of parsed packets and produces synthetic
//  bunches representing full logical bunches.  Partial fragments that are
//  part of a complete chain get marked for skipping in downstream processing.
//
//  This is the C++ port of reassemble_partial_bunches() from phase1_parser.py
//  (lines 932-1028).
//
//  LAYER:  Protocol / wire
//  SESSION: B (reassembly + schema)
// ============================================================================
#pragma once

#include "protocol/wire/bunch_types.h"
#include "protocol/wire/packet_parser.h"
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <functional>

namespace aoc { namespace protocol { namespace wire {

/// A fully reassembled logical bunch, produced by walking partial chains.
struct ReassembledBunch {
    ParsedBunch header;                // copy of first fragment's header, BDB = total
    std::vector<uint8_t> data;         // packed payload bytes (LSB-first per byte)
    size_t total_bits = 0;
    Direction direction = Direction::ServerToClient;
    size_t source_pkt_first = 0;       // index of first fragment's source packet
    size_t source_pkt_last = 0;
};
// [C O A ~ C_Y_B]

/// Fragment key = (packet_index, bunch_index_within_packet).  Identifies
/// a specific parsed bunch that should be suppressed from normal handling
/// because it's part of a reassembled chain.
struct FragmentKey {
    size_t pkt_index;
    size_t bunch_index;
    bool operator==(const FragmentKey& o) const {
        return pkt_index == o.pkt_index && bunch_index == o.bunch_index;
    }
};

struct FragmentKeyHash {
    size_t operator()(const FragmentKey& k) const noexcept {
        return std::hash<size_t>()(k.pkt_index) ^
               (std::hash<size_t>()(k.bunch_index) << 1);
    }
};

struct ReassemblyResult {
    std::vector<ReassembledBunch> bunches;
    std::unordered_set<FragmentKey, FragmentKeyHash> skip_set;
};

/// Input: a sequence of (parsed_packet, original_raw_buffer) pairs.
/// The raw buffer must stay alive during reassembly (we copy out payload bits).
///
/// Returns: reassembled full bunches + skip_set of fragments to suppress.
///
/// Single-fragment bunches (partial=0, OR partial_initial=1 AND partial_final=1)
/// are NOT considered fragments and are NOT added to skip_set.
struct ReassemblyInput {
    const ParsedPacket* parsed;
    const uint8_t* raw_buf;
    size_t raw_len;
};

ReassemblyResult reassemble_partial_bunches(const std::vector<ReassemblyInput>& packets);

}}} // namespace aoc::protocol::wire
