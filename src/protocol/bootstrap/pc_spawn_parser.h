// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/bootstrap/pc_spawn_parser.h
//
//  Option B / B.1a — parse embedded pkt[22]'s PC ActorOpen bunch into
//  structured fields so our emitter can rebuild it parametrically.
//
//  The parser is a C++ port of src/protocol/tools/decode_pc_spawn_v2.py
//  and decode_transform_body.py, operating directly on the bit-stream
//  of the bunch data (no allocation, no copies).
//
//  Once we parse an embedded packet into `PcSpawnFields`, the emitter
//  can:
//    1. Re-emit with identical fields → byte-identical output (no-op
//       validation that parametric path works).
//    2. Modify a field (e.g. actor GUID, location, class) and re-emit →
//       client sees the modified value if it accepts the bunch.
// ============================================================================
#pragma once

#include "protocol/emit/intrepid_netguid.h"
#include "protocol/emit/package_map_exporter.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace aoc { namespace protocol { namespace bootstrap {

// ─── Parsed export entry (recursive) ──────────────────────────────────

struct ParsedExport {
    emit::FIntrepidNetworkGUID guid;
    bool        has_path       = false;
    bool        no_load        = false;
    bool        has_checksum   = false;
    std::string path;
    uint32_t    checksum       = 0;
    std::unique_ptr<ParsedExport> outer;   // nullptr terminates
};

// ─── Parsed bunch header (subset we care about) ───────────────────────

struct ParsedBunchHeader {
    bool      is_control        = false;
    bool      b_open            = false;  // only valid when is_control=true
    bool      b_close           = false;  // only valid when is_control=true
    bool      is_replication_paused = false;
    bool      is_reliable       = false;
    uint32_t  channel           = 0;
    bool      has_package_map_exports = false;
    bool      has_must_be_mapped_guids = false;
    bool      is_partial        = false;
    bool      partial_initial   = true;
    bool      partial_final     = true;
    // AoC-specific 3rd partial flag ("last fragment still carrying
    // exports").  Observed patterns per docs/world-bootstrap-findings.md
    // §7: [0,1,0] / [0,1,1] / [1,0,1] — the middle bit is set on every
    // partial bunch that's not a channel-close.
    bool      partial_custom_exports_final = false;
    uint16_t  ch_sequence       = 0;

    // ChName — UE5 FName wire format.  Only present when bReliable || bOpen.
    //   If is_hardcoded=true:  ename_idx is the index into UE5's hardcoded
    //                          EName table (e.g. 102 = NAME_Actor).
    //   If is_hardcoded=false: ch_name_string + ch_name_number encode a
    //                          custom FName (e.g. AoC's custom Pawn class).
    bool      ch_name_is_hardcoded = true;
    uint32_t  ch_name_ename_idx    = 0;
    std::string ch_name_string;         // only valid when !ch_name_is_hardcoded
    int32_t   ch_name_number       = 0; // FName Number, usually 0
    std::string channel_name;   // pretty-printed, e.g. "EName[102]" or "AshesPawn"

    // Bit positions (within raw) where we stopped reading — payload begins here
    size_t    payload_start_bit = 0;
    uint32_t  bunch_data_bits   = 0;   // BDB field value
};

// ─── Parsed SerializeNewActor block ───────────────────────────────────
// C Y B x CoA

struct ParsedSerializeNewActor {
    emit::FIntrepidNetworkGUID actor_guid;
    emit::FIntrepidNetworkGUID archetype_guid;
    emit::FIntrepidNetworkGUID level_guid;

    bool      serialize_location  = false;
    bool      quantize_location   = false;
    int32_t   loc_max_bits        = 24;  // from packed-vector header
    int32_t   loc_scaled_x        = 0;
    int32_t   loc_scaled_y        = 0;
    int32_t   loc_scaled_z        = 0;
    // TODO: non-quantized location (3×double), rotation body, scale/velocity

    bool      serialize_rotation  = false;
    bool      serialize_scale     = false;
    bool      serialize_velocity  = false;

    // Bit position within bunch where this block ends — RepLayout tail
    // begins here.
    size_t    end_bit_in_bunch    = 0;
};

// ─── Parsed full PC spawn bunch ───────────────────────────────────────

struct PcSpawnFields {
    ParsedBunchHeader        header;
    std::vector<ParsedExport> exports;
    ParsedSerializeNewActor  sna;

    // Bit position where the PC ActorOpen bunch STARTS within raw packet.
    // For multi-bunch packets (e.g. embedded pkt[22] has a channel-open
    // control bunch followed by the PC ActorOpen data bunch), this is
    // AFTER the preceding bunches — NOT the same as pkt.bunch_start_bit.
    size_t    bunch_start_bit_in_raw = 0;

    // Total size (in bits) of the PC ActorOpen bunch (header + payload).
    // This is what our emitter's output should equal for a bit-identical
    // splice.
    size_t    bunch_total_bits = 0;

    // RepLayout tail: bit range within the bunch (not parsed, just copied).
    size_t    tail_start_bit_in_bunch = 0;
    size_t    tail_bit_count          = 0;

    // AoC compact field-mask format indicator.  When the bunch payload
    // starts with bHasRepLayoutExport=1 (see docs/world-bootstrap-
    // findings.md), this flag is true and `exports`/`sna` are NOT
    // populated — the entire payload is opaque from our parser's
    // perspective and must be spliced verbatim.  For round-trip testing
    // and initial Phase II synthesis, we treat the whole payload as tail
    // (`tail_start_bit_in_bunch = payload_start`, `tail_bit_count = BDB`).
    //
    // Proper per-property parsing is deferred until we have the RepLayout
    // cmd_index table (needs IDA RE of the AoC client's class metadata).
    bool      is_rep_layout_export = false;

    // When is_rep_layout_export=true, the NumExports field tells us the
    // total number of replicated properties in the class (e.g. 411 for
    // AAoCPlayerController).  The bitmask that follows is NumExports bits
    // wide.
    uint32_t  rep_layout_num_exports = 0;
};

// ─── Entry points ─────────────────────────────────────────────────────

/// Parse embedded pkt[22]'s PC ActorOpen bunch.  `raw` is the full
/// packet bytes; `bunch_start_bit` / `bunch_bits` come from
/// ReplayPacketInfo.  Returns nullopt on parse failure.
std::optional<PcSpawnFields> parse_pc_spawn_bunch(const uint8_t* raw,
                                                     size_t raw_size_bytes,
                                                     size_t bunch_start_bit,
                                                     size_t bunch_bits);

}}} // namespace aoc::protocol::bootstrap
