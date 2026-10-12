// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/bootstrap_emitter.cpp
//
//  M1.1 — see header for overview.
//
//  This implementation produces a placeholder for pkt#0 (the AoC-specific
//  opcode-3 bunch) by emitting the captured 42 bytes verbatim.  Session IDs
//  naturally differ between captures, so in M1.1+ we'd parameterize this —
//  for now the goal is byte-identity against the fixture so we can confirm
//  the pipeline is wired correctly end-to-end.
// ============================================================================
#include "net/bootstrap_emitter.h"
#include "net/native_connect_sequencer.h"  // IGameServerHost

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#endif

#include <spdlog/spdlog.h>
#include <cstring>

namespace aoc { namespace net {

BootstrapEmitter::BootstrapEmitter(IGameServerHost& host,
                                     const std::string& client_key)
    : host_(host), client_key_(client_key)
{}

bool BootstrapEmitter::emit_all(const sockaddr_in& client_addr) {
    spdlog::warn("[BootstrapEmitter] === begin minimum bootstrap ===");

    if (!emit_aoc_opcode_3(client_addr)) {
        spdlog::error("[BootstrapEmitter] emit_aoc_opcode_3 failed");
        return false;
    }
    if (!emit_nmt_welcome_if_needed(client_addr)) {
        spdlog::error("[BootstrapEmitter] emit_nmt_welcome_if_needed failed");
        return false;
    }

    spdlog::warn("[BootstrapEmitter] === bootstrap complete ===");
    return true;
}
// C.O.A / C.Y.B

// ─── pkt#0 placeholder ───────────────────────────────────────────────────
//
// Captured pkt#0 (seq=14265) bytes, 42 total:
//    96 76 0c 50 0c 4c 12 b9 37 00 00 00 00 65 af 09
//    53 ec d1 ff 11 00 ba ff 17 80 03 03 09 00 00 00
//    35 30 39 39 35 33 34 34 00 03
//
// Layout (decoded via decode_ctrl_bunches.py):
//   bytes 0..21: SC packet prefix (outer header + notify + custom field)
//   bytes 22..: bit-packed bunch header + payload
//     bunch: ch=0 reliable data, bdb=112 bits
//     payload: 03 | 09 00 00 00 | "50995344" | 00
//              ^    ^            ^
//              |    FString len  ASCII + NUL
//              AoC opcode 3 (unknown semantic — see docs/native-bootstrap-sequence.md §2.1)
//
// For M1.1, we emit this byte-for-byte verbatim.  In M1.2+ we parameterize
// the session string (replace "50995344" with a value derived from server
// state) and rewrite the outer packet header fields (seq/ack/custom) to
// match our live client's state.
//
// TODO(M1.1 RE): capture the ACTUAL purpose of this bunch via IDA.
// Investigation target: UControlChannel::ReceivedBunch case 3 in the
// client binary.
bool BootstrapEmitter::emit_aoc_opcode_3(const sockaddr_in& client_addr) {
    // Phase A.1 (2026-04-26) — REAL implementation. Builds the 14-byte
    // payload that captured pkt#0 carries:
    //
    //   byte 0       : 0x03                   (AoC opcode 3)
    //   bytes 1-4    : 0x09 0x00 0x00 0x00    (FString length = 9 LE u32)
    //   bytes 5-12   : "50995344"             (8 ASCII chars)
    //   byte 13      : 0x00                   (FString NUL terminator)
    //
    // The session string "50995344" is the captured value — semantic
    // unknown until IDA RE confirms (likely build/session ID since it's
    // 8 decimal digits). For Phase A.1 we replay the captured value;
    // future iteration parameterizes from server state.
    //
    // The host's send_ch0_reliable_payload() handles all the bunch
    // header construction (ChSeq, ChName=EName[255], BDB) using our
    // live session's reliable_seq counter, so the ChSeq advances
    // monotonically alongside our NMT bunches.

    static const std::string kSessionId = "50995344";  // captured value
    const uint32_t save_num = static_cast<uint32_t>(kSessionId.size() + 1);

    uint8_t payload[256] = {};
    size_t off = 0;
    payload[off++] = 0x03;                                  // opcode
    payload[off++] = static_cast<uint8_t>(save_num & 0xFF); // FString len LE u32
    payload[off++] = static_cast<uint8_t>((save_num >> 8) & 0xFF);
    payload[off++] = static_cast<uint8_t>((save_num >> 16) & 0xFF);
    payload[off++] = static_cast<uint8_t>((save_num >> 24) & 0xFF);
    for (char c : kSessionId) payload[off++] = static_cast<uint8_t>(c);
    payload[off++] = 0x00;  // NUL terminator

    spdlog::warn("[BootstrapEmitter] emit_aoc_opcode_3: sending native ch=0 "
                 "bunch (opcode 3 + session_id='{}', {}B payload)",
                 kSessionId, off);
    bool ok = host_.send_ch0_reliable_payload(client_key_, client_addr,
                                                payload, off);
    if (!ok) {
        spdlog::error("[BootstrapEmitter] send_ch0_reliable_payload failed");
        return false;
    }
    spdlog::warn("[BootstrapEmitter] ★ opcode-3 bunch sent natively");
    return true;
}

bool BootstrapEmitter::emit_nmt_welcome_if_needed(const sockaddr_in& client_addr) {
    // In the standard --native flow, NMT_Welcome was ALREADY emitted by
    // GameServer::send_nmt_welcome in response to NMT_Login.  The
    // sequencer is only started AFTER NMT completes, so by the time we
    // get here, this phase is a no-op.
    //
    // Kept as a named phase in case a future mode starts the sequencer
    // pre-NMT (direct-test scenarios).
    spdlog::info("[BootstrapEmitter] NMT_Welcome already sent by NMT handler — skipping");
    (void)client_addr;
    return true;
}

}} // namespace aoc::net
