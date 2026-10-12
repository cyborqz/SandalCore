// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/wire/packet_parser.h
//
//  Port of phase1_parser.py's parse_packet() and parse_bunch_header() to C++.
//  Consumes a raw UDP payload, produces a ParsedPacket with a vector of
//  ParsedBunch entries.
//
//  The port is MECHANICAL — same algorithm, same field order, same
//  validation rules.  The goal is byte-identical structural output vs the
//  Python reference (tested via dist/Release/re_* scripts).
//
//  Reference: src/protocol/tools/phase1_parser.py lines 165-265 (packet
//  header), 250-300 (bunch header).  Algorithm notes inline.
//
//  LAYER:  Protocol / wire
//  SESSION: A (the parser)
// ============================================================================
#pragma once

#include "protocol/wire/packet_reader.h"
#include "protocol/wire/bunch_types.h"
#include "protocol/wire/packet_info.h"
#include <optional>
#include <string>
#include <cstdint>
#include <utility>

namespace aoc { namespace protocol { namespace wire {

/// Parse a single UDP payload into structured bunches.
/// Returns std::nullopt if the packet is malformed or too short.
std::optional<ParsedPacket> parse_packet(const uint8_t* data, size_t byte_len,
                                         Direction dir);

/// Parse a single bunch header at the reader's current position.
/// On success, the reader is advanced to the start of bunch payload data.
/// On failure, returns nullopt; reader position is unchanged.
std::optional<ParsedBunch> parse_bunch_header(PacketReader& r,
                                               size_t content_end_bits);

/// How a walk over a packet's bunches ended.
struct WalkResult {
    size_t visited = 0;   // bunches passed to the callback
    size_t end_pos = 0;   // the bit where the walk stopped: the end of the last bunch visited
    bool   complete = false;  // the bunches end exactly at `end_bit`; false: the walk stopped early
};

/// The most bunches a walk visits.  A bunch takes at least 28 bits (an unreliable one on channel
/// 0..127 without data: 1+1+1+8+1+1+1 flag and channel bits, then the 14-bit BunchDataBits of a
/// value below 32), so a packet of the 1028 bytes AoC allows holds at most 293: a packet that
/// parses never reaches the limit.  It was 64, which lost the tail of a packet with more bunches
/// (and the packet was acked all the same).
constexpr size_t kMaxBunchesPerPacket = 1024;

namespace detail {
// [CoA + C:Y:B]

/// The walk.  `strict`: a header whose reads ran past `end_bit` is unreadable.  PacketReader answers such a read
/// with 0 and does not advance (overflowed() says so), so parse_bunch_header takes the cut-off header of a packet
/// with a damaged tail for a bunch of no data, a few bits short of the end; the legacy callers have always
/// walked like that.
template <bool Strict, class OnBunch>
WalkResult walk(const uint8_t* data, size_t byte_len, size_t start_bit, size_t end_bit, OnBunch&& on_bunch) {
    PacketReader r(data, byte_len, end_bit);
    r.set_pos(start_bit);
    WalkResult result;
    result.end_pos = start_bit;
    while (result.end_pos < end_bit && result.visited < kMaxBunchesPerPacket) {
        auto bunch = parse_bunch_header(r, end_bit);
        if (!bunch) break;
        if (Strict && r.overflowed()) break;
        const size_t next = bunch->data_start_bit + bunch->bunch_data_bits;
        if (next > end_bit) break;
        r.set_pos(next);
        on_bunch(*bunch);
        ++result.visited;
        result.end_pos = next;
    }
    result.complete = result.end_pos >= end_bit;
    return result;
}

}  // namespace detail

/// Visits every bunch between `start_bit` and `end_bit`, calling
/// on_bunch(const ParsedBunch&) for each, and says where and why it stopped.
///
/// Walking stops when a header is unreadable (it reads past `end_bit`) or its BunchDataBits
/// overruns the packet -- never because of what a bunch contains (the live loop used to stop
/// after any RPC bunch).  Zero-bit bunches (e.g. a control Close) are valid.
/// `complete` is false when it stopped before `end_bit`: UE closes the connection on such a
/// packet (UNetConnection::ReceivedPacket, "Bunch header overflowed"); worldd does.
template <class OnBunch>
WalkResult walk_bunches_ex(const uint8_t* data, size_t byte_len,
                           size_t start_bit, size_t end_bit, OnBunch&& on_bunch) {
    return detail::walk<true>(data, byte_len, start_bit, end_bit, std::forward<OnBunch>(on_bunch));
}

/// Visits every bunch between `start_bit` and `end_bit`, calling
/// on_bunch(const ParsedBunch&) for each.  Returns the number visited.
///
/// Shared by parse_packet() and the live C>S path in net/game_server.h.  The callers that
/// ignore how a walk ended (the legacy server, the tools) walk as they always did: a header cut
/// off by the end of the packet still reads as a bunch (see detail::walk); only the limit
/// of bunches per packet is higher.  worldd uses walk_bunches_ex.
template <class OnBunch>
size_t walk_bunches(const uint8_t* data, size_t byte_len,
                    size_t start_bit, size_t end_bit, OnBunch&& on_bunch) {
    return detail::walk<false>(data, byte_len, start_bit, end_bit, std::forward<OnBunch>(on_bunch)).visited;
}

/// Helper: parse a UE5 hardcoded/indexed name ("static_parse_name" in UE5).
/// Returns either "EName[N]" for hardcoded names or the FString itself for
/// dynamic names.
std::optional<std::string> parse_static_name(PacketReader& r);

// ─── Constants ──────────────────────────────────────────────────────────────

// AoC outer packet header bits: Magic(32) + SessionID(2) + ClientID(3) + HandshakeBit(1) = 38
constexpr size_t OUTER_HDR_BITS      = 38;

// Custom field between ack-history and PacketInfo — 48 bits (6 bytes)
constexpr size_t CUSTOM_FIELD_BITS   = 48;

// BunchDataBits is SerializeInt(v, MaxPacket * 8).  AoC's MaxPacket is 1028
// bytes (the largest captured packet is 1028B), so the field is 13 bits for
// v >= 32 and 14 bits for v < 32.  With 8224, all 8102 C>S data packets in
// the 2026-09 captures walk exactly to the sentinel bit (71 did not with
// 8192); for S>C, 744 more captured packets parse cleanly and none regress.
constexpr uint32_t MAX_PKT_BITS      = 1028 * 8;

/// Writes BunchDataBits the way the peer reads it: SerializeInt(v,
/// MAX_PKT_BITS) — 13 bits for v >= 32, 14 bits below.
inline void write_bunch_data_bits(uint8_t* buf, size_t cap, size_t& off, uint32_t v) {
    ::ue5::write_serialize_int(buf, cap, off, v, MAX_PKT_BITS);
}

// Maximum ChSequence value (reliable channel)
constexpr uint32_t MAX_CHSEQ         = 1024;

// Per UE5 control-channel behavior — channel names are hardcoded FNames for
// common types; the "EName index" path reads via SerializeIntPacked.
constexpr uint32_t MAX_NETWORKED_HARDCODED_NAME = 511;

}}} // namespace aoc::protocol::wire
