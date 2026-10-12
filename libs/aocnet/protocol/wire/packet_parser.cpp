// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/wire/packet_parser.cpp
//
//  Implementation of packet_parser.h.  Direct port of phase1_parser.py.
//  Algorithm reference: Python lines 169-266 for parse_packet; 258-339 for
//  parse_bunch_header.
// ============================================================================
#include "protocol/wire/packet_parser.h"
#include <cstring>

namespace aoc { namespace protocol { namespace wire {

std::optional<std::string> parse_static_name(PacketReader& r) {
    // UE5 static_parse_name:
    //   bit 1: is_hardcoded
    //   if hardcoded: SerializeIntPacked → EName index
    //   else: FString + uint32 number
    if (r.remaining_bits() < 1) return std::nullopt;
    int is_hardcoded = r.read_bit();
    if (is_hardcoded) {
        auto idx = r.read_sip();
        if (!idx) return std::nullopt;
        return "EName[" + std::to_string(*idx) + "]";
    }
    auto name = r.read_fstring();
    if (!name) return std::nullopt;
    // UE5 also appends a uint32 number suffix
    if (r.remaining_bits() < 32) return std::nullopt;
    uint32_t num = r.read_uint32();
    if (num != 0) {
        return *name + "_" + std::to_string(num);
    }
    return *name;
}

std::optional<ParsedBunch> parse_bunch_header(PacketReader& r,
                                               size_t content_end_bits) {
    PacketReader::Snapshot start = r.save();
// built by C O A with C_Y_B

    ParsedBunch b;
    b.data_start_bit = r.pos();  // will be overwritten at the end

    // ── Control flags ──
    b.is_control = r.read_bit() != 0;

    // UE5: bOpen/bClose only read inside bControl branch
    if (b.is_control) {
        b.is_open = r.read_bit() != 0;
        b.is_close = r.read_bit() != 0;
        if (b.is_close) {
            b.close_reason = r.read_serialize_int(15);
        }
    }

    // bIsReplicationPaused — ALWAYS read (UE5 reads unconditionally)
    b.is_paused = r.read_bit() != 0;

    // bReliable
    b.is_reliable = r.read_bit() != 0;

    // ChIndex via SerializeIntPacked
    auto ch_idx = r.read_sip();
    if (!ch_idx || *ch_idx > 100000) {
        r.restore(start);
        return std::nullopt;
    }
    b.channel = static_cast<uint32_t>(*ch_idx);

    // Flags
    b.has_package_map_exports = r.read_bit() != 0;
    b.has_must_map_guids = r.read_bit() != 0;
    b.is_partial = r.read_bit() != 0;

    // Channel sequence (if reliable)
    if (b.is_reliable) {
        b.ch_sequence = r.read_serialize_int(MAX_CHSEQ);
    }

    // Partial sub-flags
    if (b.is_partial) {
        b.partial_initial = r.read_bit() != 0;
        b.partial_has_comp_export = r.read_bit() != 0;
        b.partial_final = r.read_bit() != 0;
    }

    // Channel name (only if reliable or open)
    if (b.is_reliable || b.is_open) {
        auto name = parse_static_name(r);
        if (!name) {
            r.restore(start);
            return std::nullopt;
        }
        b.channel_name = *name;
    }

    // BunchDataBits via SerializeInt (variable-length adaptive)
    b.bunch_data_bits = r.read_serialize_int(MAX_PKT_BITS);

    // Sanity: BDB must fit in remaining content (with 16 bits grace per Python)
    if (b.bunch_data_bits > (content_end_bits - r.pos() + 16)) {
        r.restore(start);
        return std::nullopt;
    }

    b.data_start_bit = r.pos();
    b.header_bits = r.pos() - start.pos;

    // NOTE: This function does NOT advance past the payload.  Caller is
    // expected to `r.set_pos(b.data_start_bit + b.bunch_data_bits)` to
    // move to the next bunch.  This matches Python's pattern.
    return b;
}

std::optional<ParsedPacket> parse_packet(const uint8_t* data, size_t byte_len,
                                         Direction dir) {
    if (byte_len < 4) return std::nullopt;

    // Find the effective end of content (strip UE5 termination bit)
    size_t outer_bits = ::ue5::strip_termination(data, byte_len);
    if (outer_bits < OUTER_HDR_BITS + 64) return std::nullopt;

    // NOTE: phase1_parser.py does a bit-shift "realignment" that copies the
    // inner stream into a new buffer aligned to bit 0.  We skip that step
    // by using a PacketReader directly against `data` with a starting
    // position AFTER the outer header.  Saves a 1500-byte copy per packet.
    PacketReader r(data, byte_len, outer_bits);
    r.set_pos(OUTER_HDR_BITS);

    ParsedPacket pkt;
    pkt.inner_start_bit = OUTER_HDR_BITS;
    pkt.inner_end_bit = outer_bits;

    // ── Packed header (32 bits) ──
    if (r.remaining_bits() < 32) return std::nullopt;
    uint32_t packed_hdr = r.read_uint32();
    pkt.hist_word_count = (packed_hdr & 0xF) + 1;
    pkt.ack_seq = (packed_hdr >> 4) & 0x3FFF;
    pkt.seq = (packed_hdr >> 18) & 0x3FFF;

    // ── History words ──
    pkt.history.reserve(pkt.hist_word_count);
    for (uint32_t i = 0; i < pkt.hist_word_count; ++i) {
        if (r.remaining_bits() < 32) return std::nullopt;
        pkt.history.push_back(r.read_uint32());
    }

    // ── AoC custom field (48 bits) ──
    if (r.remaining_bits() < CUSTOM_FIELD_BITS) return std::nullopt;
    pkt.custom_field = r.read_bits(CUSTOM_FIELD_BITS);

    // ── PacketInfo ──
    if (r.remaining_bits() == 0) {
        // Empty packet after custom field — no bunches
        return pkt;
    }

    // Direction-dependent layout — see protocol/wire/packet_info.h.
    size_t pi_off = r.pos();
    PacketInfo pi = read_packet_info(data, byte_len, pi_off, dir);
    r.set_pos(pi_off);
    pkt.has_pkt_info    = pi.has_pkt_info;
    pkt.jitter_ms       = pi.jitter_ms;
    pkt.has_srv_frame   = pi.has_srv_frame;
    pkt.frame_time_byte = pi.frame_time_byte;

    walk_bunches(data, byte_len, r.pos(), outer_bits,
                 [&](const ParsedBunch& b) { pkt.bunches.push_back(b); });

    return pkt;
}

}}} // namespace aoc::protocol::wire
