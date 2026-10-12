// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/emit/nmt_builder.h
//
//  Pure bunch-bit builders for UE5 NMT control-channel messages.
//
//  These functions produce the BUNCH portion of a packet — bunch header
//  (ctrl/open/close/reliable/chIndex/partial flags, chSeq, ChName, BDB)
//  plus the NMT opcode byte and its type-specific FString payload.
//  The OUTER packet framing (MagicHeader, FNetPacketNotify packed header,
//  history words, AoC custom field, PacketInfo, termination) is handled
//  by UdpPacketEmitter::wrap_and_send — this file only cares about bunch
//  bits that go into the outer packet.
//
//  Source of the wire format:  the existing send_nmt() in game_server.h
//  (lines ~2472-2544) written from real-AoC-server captures.
//  Session H.2 ports that logic into a standalone builder so it can be
//  called from LiveWorld's OpcodeDispatcher in response to incoming NMT
//  messages, replacing the legacy replay bytes for these specific opcodes.
//
//  LAYER:   Protocol / emit
//  SESSION: H.2
// ============================================================================
#pragma once

#include "protocol/emit/bunch_writer.h"
#include <cstdint>
#include <string>

namespace aoc { namespace protocol { namespace emit {

/// Per-bunch context for building NMT messages.  Matches the fields set
/// by game_server.h's send_nmt (with the channel-already-open variant,
/// since the control channel opens via NMT_Challenge which is the first
/// S>C NMT and stays open through the entire connection).
struct NmtBunchContext {
    /// Channel sequence number (10-bit for NMT-format bunches).  This is
    /// per-control-channel-reliable-bunch counter; increment for each
    /// subsequent NMT sent.  Typical first value after channel open: 955.
    uint16_t ch_sequence = 0;
    /// When true, this is the FIRST bunch on the control channel and
    /// carries bControl=1 + bOpen=1.  NMT_Challenge is the only case
    /// where this is typical.  All later NMTs (Welcome, NetGUIDAssign,
    /// etc.) pass false.
    bool     opens_channel = false;
};
// (C.O.A) (Cyb)

class NmtBuilder {
public:
    /// Build an NMT_Welcome bunch into `out`.  The caller is responsible
    /// for the outer packet framing.
    ///
    /// Payload format (from UE5 DataChannel.h line 174):
    ///   NMT_Welcome = FString level + FString gameMode + FString redirectUrl
    ///
    /// Returns the number of bits written to `out` (bunch header + payload
    /// with no termination bit).  0 on error.
    static size_t build_welcome(BunchWriter& out,
                                  const NmtBunchContext& ctx,
                                  const std::string& level,
                                  const std::string& game_mode,
                                  const std::string& redirect_url);

    /// Build an NMT_Challenge bunch into `out`.
    /// Payload format (from UE5 DataChannel.h line 176):
    ///   NMT_Challenge = FString challenge
    /// Captured-AoC challenge strings look like short decimal tokens
    /// (e.g. "50995344") rather than 20-byte HMAC hex; the builder is
    /// content-agnostic and just writes whatever the caller supplies.
    static size_t build_challenge(BunchWriter& out,
                                    const NmtBunchContext& ctx,
                                    const std::string& challenge);

    /// Build an NMT_NetGUIDAssign bunch into `out`.
    /// Payload format (from UE5 DataChannel.h line 187):
    ///   NMT_NetGUIDAssign = FNetworkGUID netguid + FString path
    /// The NetGUID is serialised via SerializeIntPacked (variable-length
    /// 7-bit-per-byte continuation encoding).  The path is a standard
    /// ANSI FString.
    static size_t build_netguid_assign(BunchWriter& out,
                                          const NmtBunchContext& ctx,
                                          uint32_t netguid,
                                          const std::string& asset_path);

    // ── Helpers (public for testing) ──

    /// Write an NMT-format bunch header: ctrl/open/close + reliable +
    /// chIndex=0 (SIP) + flags + ch_sequence (10-bit) + ChName=EName[255]
    /// (bHardcoded=1, packed 0xFF 0x02).  Writes everything EXCEPT the
    /// BunchDataBits field — callers fill that after computing payload size.
    /// Returns the bit position where BDB must be written.
    static size_t write_nmt_bunch_header(BunchWriter& out,
                                           const NmtBunchContext& ctx);

    /// Write a 13-bit BunchDataBits field at `bit_pos` WITHOUT advancing
    /// the writer's position.  Used to back-fill BDB after computing the
    /// payload size.
    static void patch_bunch_data_bits(BunchWriter& out,
                                        size_t bit_pos,
                                        uint16_t bdb);

    /// Write a UE5 FString: int32 save_num (incl. NUL) + ASCII bytes + NUL.
    /// Empty string → int32(0), no bytes.
    static void write_fstring_ansi(BunchWriter& out, const std::string& s);
};

}}} // namespace aoc::protocol::emit
