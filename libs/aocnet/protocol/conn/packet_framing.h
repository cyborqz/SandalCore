// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/conn/packet_framing.h
//
//  Data packets after the handshake, in both directions:
//
//      MagicHeader:32 SessionId:2 ClientId:3 bHandshake:1 (= 0)
//      notify header (packet_notify.h)
//      AoC custom field: 6 bytes, the first 6 of the server's handshake
//        extension, the same in every packet of the connection
//      PacketInfo: bHasPacketInfoPayload:1, and if set JitterClockTimeMS
//        (SerializeInt 1024), bHasServerFrameTime:1 (+ FrameTimeByte:8 when
//        the server sets it)
//      bunches
//      sentinel bit (1), then the termination bit
//
//  and the bunch header both sides use for the control channel (the C>S
//  format; the original server sends its NMT bunches in it too):
//
//      bControl:1 [bOpen:1 bClose:1 [CloseReason: SerializeInt 15]]
//      bIsReplicationPaused:1 bReliable:1 ChIndex:SerializeIntPacked
//      bHasPackageMapExports:1 bHasMustBeMappedGUIDs:1 bPartial:1
//      [ChSequence: SerializeInt 1024, if reliable]
//      [bPartialInitial:1 bPartialCustomExportsFinal:1 bPartialFinal:1]
//      [ChName: bHardcoded:1 + SerializeIntPacked index, if reliable or open]
//      BunchDataBits: SerializeInt(1028 * 8), payload
//
//  Verified bit for bit against the 2026-09-28 capture (test_packet_framing).
//
//  LAYER: Protocol / conn
// ============================================================================
#pragma once
// -- C-O-A . C:Y:B --

#include "protocol/conn/handshake.h"
#include "protocol/conn/packet_notify.h"
#include "protocol/emit/bunch_writer.h"
#include "protocol/wire/bunch_types.h"
#include "protocol/wire/packet_info.h"
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace aoc { namespace protocol { namespace conn {

constexpr size_t kCustomFieldBytes = 6;
using CustomField = std::array<uint8_t, kCustomFieldBytes>;

/// The largest datagram either side sends (AoC's MaxPacket).
constexpr size_t kMaxPacketBytes = 1028;

/// The hardcoded name of the control channel (EName 255).
constexpr uint32_t kControlChannelName = 255;

/// The first 38 bits of every packet.
struct OuterHeader {
    uint32_t magic_header = 0;
    uint8_t  session_id = 0;  // 2 bits
    uint8_t  client_id = 0;   // 3 bits
};

inline OuterHeader outer_header_of(const HandshakePacket& p) {
    return {p.magic_header, p.session_id, p.client_id};
}

/// Starts a server data packet: outer header, notify header, custom field,
/// and the PacketInfo the original server always sends (payload present,
/// jitter 1023, no frame time; without it the client disconnects).
void begin_server_packet(emit::BunchWriter& w, const OuterHeader& outer,
                         const NotifyHeader& notify, const CustomField& cf);

/// Starts a client data packet (for tests and tools that play the client).
void begin_client_packet(emit::BunchWriter& w, const OuterHeader& outer,
                         const NotifyHeader& notify, const CustomField& cf,
                         uint32_t jitter_ms = 1023);

/// Ends a data packet with the sentinel and termination bits.
std::vector<uint8_t> finish_data_packet(const emit::BunchWriter& w);

/// A bunch header in the control-channel format described above.
struct BunchHeader {
    bool     control = false;
    bool     open = false;
    bool     close = false;
    uint32_t close_reason = 0;
    bool     reliable = false;
    uint32_t channel = 0;
    bool     has_exports = false;
    bool     has_must_map_guids = false;
    bool     partial = false;
    bool     partial_initial = false;
    bool     partial_custom_exports_final = false;
    bool     partial_final = false;
    uint16_t ch_sequence = 0;                   // written modulo 1024
    uint32_t name_index = kControlChannelName;  // written when reliable or open
};

/// Writes a bunch header and `payload_bits` bits of `payload` (LSB-first).
void write_bunch(emit::BunchWriter& w, const BunchHeader& h,
                 const uint8_t* payload, size_t payload_bits);

/// Bits a bunch with this header and payload size takes.
size_t bunch_bits(const BunchHeader& h, size_t payload_bits);

/// A received data packet.  Bunches lie in [bunches_begin, bunches_end);
/// the end excludes the sentinel bit.
struct DataPacket {
    OuterHeader outer;
    NotifyHeader notify;
    CustomField custom_field{};
    wire::PacketInfo info;
    size_t bunches_begin = 0;
    size_t bunches_end = 0;
};

/// Reads a data packet's headers (`dir` decides whether a frame-time byte
/// can follow the packet info); nullopt for a handshake packet or a
/// malformed one.
std::optional<DataPacket> read_data_packet(const uint8_t* data, size_t len, wire::Direction dir);

/// Every bunch header of a data packet, in order (wire::walk_bunches).
std::vector<wire::ParsedBunch> packet_bunches(const uint8_t* data, size_t len, const DataPacket& p);

}}} // namespace aoc::protocol::conn
