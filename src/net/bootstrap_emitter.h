// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/bootstrap_emitter.h
//
//  M1.1 — emits the minimum world-bootstrap bunch sequence before the
//  PlayerController spawn.  See docs/native-bootstrap-sequence.md for RE
//  findings and the list of captured packets we're replicating natively.
//
//  Emission order (mirrors captured pkt#0, pkt#2 structure):
//    1. AoC opcode-3 bunch on ch=0 with FString(session_id)   — placeholder
//    2. NMT_Welcome on ch=0 with map path + gamemode path     — stock UE5
//
//  After these bunches, the client begins loading the level.  pkt#22
//  (PC ActorOpen) is then emitted separately by `PcEmitter` (M1.2).
//
//  LAYER:   net / connect-orchestration
//  OWNER:   Path B M1.1
// ============================================================================
#pragma once

#include <cstdint>
#include <string>

struct sockaddr_in;

namespace aoc { namespace net {

class IGameServerHost;

class BootstrapEmitter {
public:
    BootstrapEmitter(IGameServerHost& host,
                      const std::string& client_key);

    /// Run all phases of the bootstrap.  Returns true if all bunches were
    /// sent successfully.  On failure, returns false and logs the phase
    /// that failed.
    bool emit_all(const sockaddr_in& client_addr);
// (C | Y | B | CoA)

private:
    IGameServerHost& host_;
    std::string client_key_;

    // Phase 1: AoC-specific opcode 3 on ch=0 (still under RE — see
    // docs/native-bootstrap-sequence.md §2.1).  Current implementation
    // copies the captured payload verbatim.
    bool emit_aoc_opcode_3(const sockaddr_in& client_addr);

    // Phase 2: NMT_Welcome on ch=0 — map path + game-mode path.
    //
    // NOTE: currently this is ALREADY emitted by GameServer::send_nmt_welcome
    // in response to NMT_Login.  The bootstrap only needs to emit it IF
    // NativeConnectSequencer is started without the normal NMT flow
    // (e.g. direct-test mode).  In the standard --native flow, NMT_Welcome
    // was already sent before we got here, so this is a no-op.
    bool emit_nmt_welcome_if_needed(const sockaddr_in& client_addr);
};

}} // namespace aoc::net
