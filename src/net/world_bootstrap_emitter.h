// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/world_bootstrap_emitter.h
//
//  Road A — Phase B.0 (2026-04-26).  Drives the post-NMT world bootstrap
//  flow as a SINGLE actor instead of the legacy hybrid+native split
//  (separate replay thread + native sequencer running in parallel).
//
//  ── Why this exists ──────────────────────────────────────────────────────
//
//  Before this class:
//    - replay_loop sent the first 150 captured packets verbatim
//    - NativeConnectSequencer ran in parallel and ALSO sent native
//      opcode-3 + PC ActorOpen + Pawn ActorOpen
//    - The two paths overlapped: client received DUPLICATE PC + Pawn
//      bunches, one captured-NetGUID and one server-minted-NetGUID
//    - Toggling "what runs natively" required editing both paths
//
//  After this class:
//    - WorldBootstrapEmitter is the SINGLE driver of the bootstrap
//      stream.  It walks an ordered plan; each entry says either
//        "splice captured packet N from ReplayData"
//      or
//        "call native emitter X (PcEmitter, PawnEmitter, ...)"
//      or
//        "skip (sentinel keepalive — our Maintain path covers it)"
//    - Promoting a packet from splice → native is a one-line change:
//      flip one EmissionMode in the plan and add a dispatch case.
//    - The replay thread is no longer needed for native mode.
//      `--replay-max-packets 0` (or omitting --replay) is the default
//      pure-native path; we still load the replay file because the
//      `Splice` plan rows reach into it for captured byte content.
//
//  ── Architecture (mirrors docs/NATIVE-EMISSION-ARCHITECTURE.md) ─────────
//
//      AwaitNmtJoin → SendBootstrap → Maintain
//                          │
//                          ▼
//                ┌─────────────────────────┐
//                │ WorldBootstrapEmitter   │
//                │   walks kDefaultPlan    │
//                │     ↓ for each entry    │
//                │   ┌─────────────────┐   │
//                │   │  EmissionMode   │   │
//                │   │  ─────────────  │   │
//                │   │  Skip           │ ← keepalive; sleep, advance
//                │   │  Splice         │ ← host_.send_captured_packet
//                │   │  NativePc0      │ ← BootstrapEmitter   (pkt#0)
//                │   │  NativePc22     │ ← PcEmitter::emit_open + props
//                │   │  NativePawn78   │ ← PawnEmitter::emit_captured
//                │   └─────────────────┘   │
//                └─────────────────────────┘
//
//  ── Pacing ───────────────────────────────────────────────────────────────
//  Captured session spaced bootstrap packets at ~15 ms.  Sending faster
//  risks overflowing the client's receive buffer (Fix #36 territory).
//  Each plan entry carries a `delay_ms_before` field; we sleep that long
//  before emitting it.  Default: 15 ms.
//
//  LAYER:   net / connect-orchestration
//  OWNER:   Road A
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>
// C | Y | B + C-O-A

struct sockaddr_in;

namespace aoc { namespace net {

class IGameServerHost;

/// What to do for each plan entry.
enum class EmissionMode : uint8_t {
    Skip,            ///< Don't emit (sentinel/keepalive).  Maintain covers it.
    Splice,          ///< host_.send_captured_packet(replay_idx)
    NativePc0,       ///< BootstrapEmitter::emit_aoc_opcode_3
    NativePc22,      ///< PcEmitter::emit_open  (also emits Name property update)
    NativePawn78,    ///< PawnEmitter::emit_captured (Guard NPC — still spliced bytes)
    NativePlayerPawn,///< PlayerPawnEmitter::emit_open — PM39 (2026-04-30) Phase 2.
                     ///< Native player Pawn ActorOpen with minted NetGUID.
    SpliceChannel,   ///< Re-send only `channel`'s bunches of captured packet
                     ///< replay_idx, byte for byte (headers and chSeq kept).
};

/// One entry in the bootstrap plan.
struct PacketEmissionSpec {
    uint32_t      replay_idx;        ///< For Splice: index into ReplayData
    EmissionMode  mode;
    const char*   description;       ///< For logs: "PC ActorOpen", "ch=85 GUIDExport #1"
    uint32_t      delay_ms_before;   ///< Pacing — sleep this long before emit
    uint32_t      channel = 0;       ///< For SpliceChannel: the channel to re-send
};

/// The default plan that reproduces the first ~100 packets of the captured
/// session.  Defined in world_bootstrap_plan.cpp.  The plan is `extern` so
/// tests / future modes can substitute alternative plans.
extern const std::vector<PacketEmissionSpec> kDefaultBootstrapPlan;

/// Drives the bootstrap plan to completion.  Owns no state beyond what
/// the plan dictates; all emission state (chSeq, NetGUID block, etc.)
/// lives in the host's per-client structures.
class WorldBootstrapEmitter {
public:
    WorldBootstrapEmitter(IGameServerHost& host,
                           const std::string& client_key);

    /// Walk the given plan in order, emitting each entry.  Returns true
    /// on full completion; false on first hard failure (e.g. send_to
    /// returned an error).  Soft failures (Splice for a packet not in
    /// loaded ReplayData, NativeBuild for a packet whose schema isn't
    /// ready) log a warning and continue — partial bootstrap is better
    /// than none, and the user-visible test will reveal what's missing.
    bool emit_all(const sockaddr_in& addr,
                   const std::vector<PacketEmissionSpec>& plan);

private:
    bool dispatch_one(const sockaddr_in& addr,
                       const PacketEmissionSpec& spec);
    bool splice_channel(const sockaddr_in& addr, uint32_t replay_idx, uint32_t channel);

    IGameServerHost& host_;
    std::string client_key_;
    // Next reliable chSeq on the PC channel (ch=3); the ActorOpen is 954.
    uint32_t pc_ch_seq_ = 955;
};

}} // namespace aoc::net
