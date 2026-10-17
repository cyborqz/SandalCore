// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/conn/packet_framing.cpp
// ============================================================================
#include "protocol/conn/packet_framing.h"
#include "protocol/wire/packet_parser.h"
#include "protocol/wire/ue5_primitives.h"

namespace aoc { namespace protocol { namespace conn {

namespace {

constexpr uint32_t kJitterMax = 1024;
constexpr uint32_t kCloseReasonMax = 15;

/// Bits SerializeInt(v, max) takes: the width depends on the value, because
/// the writer stops once no higher bit can still be set below `max`.
size_t serialize_int_bits(uint32_t v, uint32_t max) {
    size_t n = 0;
    uint32_t acc = 0, m = 1;
    while (acc + m < max && m) {
        if (v & m) acc |= m;
        m <<= 1;
        ++n;
    }
    return n;
}

void write_outer(emit::BunchWriter& w, const OuterHeader& o) {
    w.write_uint32(o.magic_header);
    w.write_bits(o.session_id, 2);
    w.write_bits(o.client_id, 3);
    w.write_bit(0);  // not a handshake packet
}

void write_prefix(emit::BunchWriter& w, const OuterHeader& outer,
                  const NotifyHeader& notify, const CustomField& cf) {
    write_outer(w, outer);
    write_notify_header(w, notify);
    for (uint8_t b : cf) w.write_uint8(b);
}

}  // namespace

void begin_server_packet(emit::BunchWriter& w, const OuterHeader& outer,
                         const NotifyHeader& notify, const CustomField& cf) {
    write_prefix(w, outer, notify, cf);
    w.write_bit(1);                             // bHasPacketInfoPayload
    w.write_serialize_int(1023, kJitterMax);    // JitterClockTimeMS
    w.write_bit(0);                             // bHasServerFrameTime
}

void begin_client_packet(emit::BunchWriter& w, const OuterHeader& outer,
                         const NotifyHeader& notify, const CustomField& cf,
                         uint32_t jitter_ms) {
    write_prefix(w, outer, notify, cf);
    w.write_bit(1);
    w.write_serialize_int(jitter_ms, kJitterMax);
    w.write_bit(0);
}
// C Y B : C | O | A

std::vector<uint8_t> finish_data_packet(const emit::BunchWriter& in) {
    emit::BunchWriter w = in;
    w.write_bit(1);  // AoC sentinel
    w.write_bit(1);  // termination
    return std::vector<uint8_t>(w.data(), w.data() + w.byte_size());
}

void write_bunch(emit::BunchWriter& w, const BunchHeader& h,
                 const uint8_t* payload, size_t payload_bits) {
    w.write_bit(h.control ? 1 : 0);
    if (h.control) {
        w.write_bit(h.open ? 1 : 0);
        w.write_bit(h.close ? 1 : 0);
        if (h.close) w.write_serialize_int(h.close_reason, kCloseReasonMax);
    }
    w.write_bit(0);  // bIsReplicationPaused
    w.write_bit(h.reliable ? 1 : 0);
    w.write_sip(h.channel);
    w.write_bit(h.has_exports ? 1 : 0);
    w.write_bit(h.has_must_map_guids ? 1 : 0);
    w.write_bit(h.partial ? 1 : 0);
    if (h.reliable) w.write_serialize_int(h.ch_sequence % wire::MAX_CHSEQ, wire::MAX_CHSEQ);
    if (h.partial) {
        w.write_bit(h.partial_initial ? 1 : 0);
        w.write_bit(h.partial_custom_exports_final ? 1 : 0);
        w.write_bit(h.partial_final ? 1 : 0);
    }
    if (h.reliable || h.open) {
        w.write_bit(1);  // bHardcoded
        w.write_sip(h.name_index);
    }
    w.write_serialize_int(static_cast<uint32_t>(payload_bits), wire::MAX_PKT_BITS);
    if (payload_bits) w.write_bit_range(payload, 0, payload_bits);
}

size_t bunch_bits(const BunchHeader& h, size_t payload_bits) {
    emit::BunchWriter w(64);
    write_bunch(w, h, nullptr, 0);
    // write_bunch(.., 0) wrote BunchDataBits as 0; swap in the width of the real value.
    const size_t header = w.bit_pos() - serialize_int_bits(0, wire::MAX_PKT_BITS) +
                          serialize_int_bits(static_cast<uint32_t>(payload_bits), wire::MAX_PKT_BITS);
    return header + payload_bits;
}

std::optional<DataPacket> read_data_packet(const uint8_t* data, size_t len, wire::Direction dir) {
    if (!data || len == 0 || len > kMaxPacketBytes || data[len - 1] == 0) return std::nullopt;
    const size_t eff = ::ue5::strip_termination(data, len);
    if (eff < 38 + 32 + kCustomFieldBytes * 8 + 1 + 1) return std::nullopt;
    const size_t end = eff - 1;  // the sentinel bit
    if (::ue5::read_bits_at(data, len, end, 1) == 0) return std::nullopt;
    size_t off = 0;
    DataPacket p;
    p.outer.magic_header = static_cast<uint32_t>(::ue5::read_bits(data, len, off, 32));
    p.outer.session_id = static_cast<uint8_t>(::ue5::read_bits(data, len, off, 2));
    p.outer.client_id = static_cast<uint8_t>(::ue5::read_bits(data, len, off, 3));
    if (::ue5::read_bits(data, len, off, 1) != 0) return std::nullopt;  // a handshake packet
    auto notify = read_notify_header(data, len, end, off);
    if (!notify) return std::nullopt;
    p.notify = *notify;
    if (off + kCustomFieldBytes * 8 + 1 > end) return std::nullopt;
    for (auto& b : p.custom_field) b = static_cast<uint8_t>(::ue5::read_bits(data, len, off, 8));
    p.info = wire::read_packet_info(data, len, off, dir);
    if (off > end) return std::nullopt;
    p.bunches_begin = off;
    p.bunches_end = end;
    return p;
}

std::vector<wire::ParsedBunch> packet_bunches(const uint8_t* data, size_t len, const DataPacket& p) {
    std::vector<wire::ParsedBunch> out;
    wire::walk_bunches(data, len, p.bunches_begin, p.bunches_end,
                       [&](const wire::ParsedBunch& b) { out.push_back(b); });
    return out;
}

}}} // namespace aoc::protocol::conn
