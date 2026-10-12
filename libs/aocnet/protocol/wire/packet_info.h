// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/wire/packet_info.h
//
//  PacketInfo block reader, shared by wire::parse_packet() and the live
//  C>S path in net/game_server.h so the two cannot drift apart.
//
//  Layout (read right after the AoC 48-bit custom field):
//
//    C>S  bHasPacketInfoPayload(1)
//         if 1: Jitter(SerializeInt 1024 = 10b) + bHasServerFrameTime(1)
//         if 0: nothing else — stock UE5 ReadPacketInfo early-return.
//         Verified on logs/run_20260928_040905: 19/19 single-bunch
//         PktInfo=0 packets decode as contiguous ch=3 reliable bunches only
//         without the frame-time bit (test_cs_packet_info).
//
//    S>C  bHasPacketInfoPayload(1)
//         if 1: Jitter(10b) + bHasServerFrameTime(1) + FrameTimeByte(8) if set
//         if 0: nothing else, as for C>S.  Verified 2026-09-29 on
//         fixtures/replay_data.bin (test_sc_packet_info): with the old
//         "frame-time bit always" rule every PacketInfo-less packet (the
//         bootstrap right after the PC ActorOpen, e.g. pkt#23-47) started
//         its bunches 1 or 9 bits late.
// ============================================================================
#pragma once

#include "protocol/wire/ue5_primitives.h"
#include <cstddef>
#include <cstdint>

namespace aoc { namespace protocol { namespace wire {

enum class Direction { ServerToClient, ClientToServer };

struct PacketInfo {
    bool     has_pkt_info    = false;
    uint32_t jitter_ms       = 0;
    bool     has_srv_frame   = false;
    uint8_t  frame_time_byte = 0;
};

/// Reads the PacketInfo block at `off` and advances `off` past it.
inline PacketInfo read_packet_info(const uint8_t* data, size_t len,
                                   size_t& off, Direction dir) {
    PacketInfo pi;
    pi.has_pkt_info = ::ue5::read_bits(data, len, off, 1) != 0;
    if (!pi.has_pkt_info) return pi;   // stock ReadPacketInfo early-return, both directions

    pi.jitter_ms = ::ue5::read_serialize_int(data, len, off, 1024);
    pi.has_srv_frame = ::ue5::read_bits(data, len, off, 1) != 0;
    // Only the server writes FrameTimeByte.
    if (dir == Direction::ServerToClient && pi.has_srv_frame)
        pi.frame_time_byte = static_cast<uint8_t>(::ue5::read_bits(data, len, off, 8));
    return pi;
}

}}} // namespace aoc::protocol::wire
