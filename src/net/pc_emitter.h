// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/pc_emitter.h
//
//  M1.2 — emits the PlayerController ActorOpen bunch natively.
//
//  Uses the existing ActorBuilder::build_spawn infrastructure which is
//  already byte-identical to captured pkt#22 (validated via
//  test_pc_spawn_diff, 4859/4864 bits match).
//
//  The wrapper here:
//    - Gathers the export entries (archetype + level + commands paths
//      from docs/native-bootstrap-sequence.md §2.3)
//    - Calls ActorBuilder with pc_schema + a runtime that carries the
//      custom character name from Config
//    - Wraps the output bunch in a full UDP packet
//    - Sends via IGameServerHost::send_to_client
//
//  Known limitations (captured in open questions):
//    - pkt#22 in captured replay is a multi-fragment partial bunch
//      (4 bunches total: 2 for PC, 2 for HUD subobject).  The current
//      ActorBuilder path produces a single non-partial bunch — which
//      may or may not be equivalent from the client's perspective.
//      Testing will tell.
//    - Actor/Archetype/Level NetGUIDs in our server-side implementation
//      won't match the captured ones (we generate our own).  That's
//      expected — only the structure matters for client acceptance.
//
//  LAYER:   net / connect-orchestration
//  OWNER:   Path B M1.2
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
// Cyb with C-O-A

struct sockaddr_in;

namespace aoc { namespace net {

class IGameServerHost;

class PcEmitter {
public:
    PcEmitter(IGameServerHost& host, const std::string& client_key);

    /// Emit the PC ActorOpen bunch.  Returns true if sent successfully.
    bool emit_open(const sockaddr_in& client_addr);

    /// Emit a property-delta bunch carrying the custom Name (and later
    /// other properties).  Called by do_send_pc_props.  M1.2 scope: Name
    /// only; richer property set in M1.2 continuation.
    bool emit_properties(const sockaddr_in& client_addr);

    /// ClientSetHUD(BP_AOCHUD_C) as reliable PC-channel bunch `ch_seq`,
    /// carrying the HUD class's package-map export, as the original server
    /// sent it right after opening the PC (replay pkt#22).  Without it the
    /// client has no HUD ("Failed to get AHudBase").
    bool emit_client_set_hud(const sockaddr_in& client_addr, uint32_t ch_seq);

    /// The PC RPCs the original server sent next (replay pkt#23: voice
    /// handshake, level streaming status, streaming flush, bandwidth cap,
    /// time): its captured ch=3 bunch payloads re-sent as reliable bunches
    /// numbered from `first_ch_seq`.  Returns how many were sent.
    uint32_t emit_bootstrap_rpcs(const sockaddr_in& client_addr, uint32_t first_ch_seq);

    /// A PC property update setting CharacterLoadTracker to `flags` ('0'/'1'
    /// per load step), as reliable PC-channel bunch `ch_seq`.  The original
    /// server sent 1011...1 before possession (pkt#72) and 0111...1 after it
    /// (pkt#229); the client only starts moving its pawn after the latter.
    bool emit_load_tracker(const sockaddr_in& client_addr, uint32_t ch_seq, const char* flags);

    /// ClientRestart(our pawn) as reliable PC-channel bunch `ch_seq`.
    /// Must be called AFTER PlayerPawnEmitter::emit_open lands so the
    /// Pawn NetGUID is registered in the client's PackageMap.
    bool emit_pawn_link(const sockaddr_in& client_addr, uint32_t ch_seq);

    /// Phase D Step 3 Iter4 (2026-05-05) — link PC.PlayerState.
    /// Sends a property-delta bunch on ch=3 carrying our minted PS NetGUID.
    /// Triggers `OnRep_PlayerState` on the client → PC's nameplate widget
    /// re-binds to our PS → PlayerNamePrivate update we sent on ch=21
    /// becomes visible.  Must be called AFTER PlayerStateEmitter::emit_open
    /// lands so the PS NetGUID is registered in the client's PackageMap.
    /// Probe-gated via probe_player_state_emit.txt (same as PlayerStateEmitter).
    bool emit_player_state_link(const sockaddr_in& client_addr);

    /// PM147 (2026-05-08) — World Partition cell keepalive RPC.
    ///
    /// AOC's `RestartPlayerAtTransform` (sub_7FF6BF22A040 in the client) calls
    /// a post-spawn hook `sub_7FF6BEC1C190` that sends
    /// `ClientUpdateLevelStreamingStatus` for World Partition cells around
    /// the spawn point.  Without these, the client's local GC sweep unloads
    /// every cell after the loading screen drops → black screen.
    ///
    /// This emit sends one keepalive RPC per call.  Handle is empirically
    /// iterated via `probe_streaming_keepalive_handle.txt` (default 70 based
    /// on alphabetical position in the dispatch table relative to ClientRestart=45).
    ///
    /// Probe-gated via `probe_streaming_keepalive.txt`.  Cell name from
    /// `probe_streaming_keepalive_package.txt` (default = persistent level).
    bool emit_client_update_level_streaming_status(const sockaddr_in& client_addr);

private:
    IGameServerHost& host_;
    std::string client_key_;
};

}} // namespace aoc::net
