// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/native_connect_sequencer.h
//
//  Authoritative-server connect orchestrator.  Replaces `replay_thread` when
//  the server runs in --native mode.  Drives the post-NMT handshake flow
//  entirely from server-side state — no captured bytes played verbatim.
//
//  ────────────────────────────────────────────────────────────────────────
//  PROJECT GOAL (Path B — decided 2026-04-24)
//  ────────────────────────────────────────────────────────────────────────
//  "Enter the world without replay, custom character, real-time state,
//   multiplayer capable."
//
//  Replay is acknowledged as a debugging reference.  Going forward the
//  server is the authority for all game state.  Each packet it emits is
//  derived from local data structures, not byte-for-byte copy of captured
//  bytes.
//
//  ────────────────────────────────────────────────────────────────────────
//  MILESTONES
//  ────────────────────────────────────────────────────────────────────────
//  M1.0  Scaffolding (THIS FILE) — sequencer state machine + --native flag.
//        After NMT completes, sequencer logs "native flow would continue"
//        but doesn't yet emit world data.  DONE criterion:
//           server accepts connection, completes NMT, does NOT panic or
//           disconnect the client; client may timeout (expected for M1.0).
//
//  M1.1  World bootstrap — channel-0 control bunches, level reference,
//        GameState, initial NetGUID exports.
//
//  M1.2  PlayerController + PlayerState emission — native, from config.
//        Uses existing ActorBuilder (byte-identical for pkt#22).
//
//  M1.3  Keep-alive + ACK handling — periodic heartbeat, reliability
//        window advance.  Prevents the "client timeout" DONE state
//        from M1.0 becoming permanent.
//
//  M1.4  Movement input — parse ServerMove bunches, update server position,
//        broadcast corrections to client.
//
//  ────────────────────────────────────────────────────────────────────────
//  STATE MACHINE
//  ────────────────────────────────────────────────────────────────────────
//     ┌──────────────────┐  NMT_Join / NMT_GameSpecific fired by dispatcher
//     │  AWAIT_NMT_JOIN  │───────────────────────────────────┐
//     └──────────────────┘                                   │
//                                                             ▼
//     ┌──────────────────┐                          ┌─────────────────┐
//     │   SEND_BOOTSTRAP │  (M1.1 — channel 0, lvl) │  SEND_BOOTSTRAP │
//     │    [M1.0: stub]  │◀─────────────────────────│                 │
//     └──────────────────┘                          └─────────────────┘
//               │ (all bootstrap ACK'd)
//               ▼
//     ┌──────────────────┐
//     │ SEND_PC_OPEN     │  (M1.2 — ActorBuilder for PlayerController)
//     └──────────────────┘
//               │
//               ▼
//     ┌──────────────────┐
//     │ SEND_PC_PROPS    │  (M1.2 — Name, Pawn NetGUID, etc.)
//     └──────────────────┘
//               │
//               ▼
//     ┌──────────────────┐
//     │     MAINTAIN     │  (M1.3 — heartbeat + M1.4 input handling)
//     └──────────────────┘
//
//  LAYER:   net / connect-orchestration
//  OWNER:   Path B (authoritative server)
//  SESSION: M1.0
// ============================================================================
#pragma once

#include "protocol/net_guid_allocator.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>
// C.O.A : C:Y:B

struct sockaddr_in;

namespace aoc { namespace net {

/// State machine for the native-mode connect flow.
enum class NativeConnectState : uint8_t {
    Idle = 0,            ///< Not started yet (pre-NMT)
    AwaitNmtJoin,        ///< NMT dispatcher should have fired us
    SendBootstrap,       ///< M1.1 — channel 0 control + world refs
    SendPcOpen,          ///< M1.2 — PlayerController ActorOpen
    SendPcProps,         ///< M1.2 — Name, Pawn NetGUID, stats
    SendPawn,            ///< M1.4.b — Pawn ActorOpen (spliced pkt#78)
    Maintain,            ///< M1.3/M1.4 — heartbeat + input handling
    Done,
    Error,
};

/// Forward decl (the sequencer uses GameServer's send primitives via a
/// narrow callback interface; we don't include game_server.h here to avoid
/// circular-include pain).
class IGameServerHost {
public:
    virtual ~IGameServerHost() = default;

    /// Send a raw UDP packet to the connected client.  Returns bytes sent
    /// (>0) or <=0 on error.
    virtual int send_to_client(const uint8_t* buf, size_t n,
                                const sockaddr_in& addr) = 0;

    /// Read-only access to the server's Config — sequencer reads
    /// `custom_name`, character profile, etc.
    virtual const std::string& custom_name() const = 0;

    /// M1.3: emit a keepalive packet for the given client.  This is a
    /// tiny (< 32 B) data-less packet that carries a PacketInfo header
    /// and a termination sentinel — just enough to prevent the client's
    /// UE5 NetConnection from timing out (default 20 s inactivity).
    /// Handles seq/ack advancement internally via the host's ClientState.
    /// Returns true on successful send.
    virtual bool send_keepalive_for(const std::string& client_key,
                                     const sockaddr_in& addr) = 0;

    /// M1.2: wrap a bit-packed bunch payload in a complete UDP packet
    /// (SC packet prefix + PacketInfo + bunch bits + termination) and
    /// send it.  The host handles seq/ack advancement internally.
    ///
    /// `bunch_data` is the raw bit-packed bunch (header + payload) as
    /// produced by e.g. ActorBuilder or BunchWriter; `bunch_bits` is
    /// the number of valid bits.  Returns true on successful send.
    virtual bool send_bunch_packet(const std::string& client_key,
                                    const sockaddr_in& addr,
                                    const uint8_t* bunch_data,
                                    size_t bunch_bits) = 0;

    /// M1.4.d — Fix #36 equivalent.  True once the client has signalled
    /// that its game thread has finished `LoadMap()` (i.e. it sent a
    /// post-NMT packet with any bunch bits and no NMT opcodes).  The
    /// sequencer MUST wait for this before emitting world-data bunches
    /// (PC ActorOpen, GameState, etc.).  If we emit earlier, bunches
    /// are dropped while the client's game thread is still loading and
    /// the player spawns underwater at world-origin with an empty
    /// world — exactly the symptom observed in emu-20260424-130202.log.
    virtual bool has_client_finished_map_load() const = 0;

    /// Phase A.1 (2026-04-26) — emit a control-channel (ch=0) reliable
    /// data bunch carrying an arbitrary byte payload (e.g. AoC opcode
    /// bytes).  The host:
    ///   - Builds the bunch header with the client's current
    ///     `reliable_seq` (ch=0, 10-bit ChSeq, hardcoded ChName=255)
    ///   - Appends the caller's payload bytes
    ///   - Wraps in a UDP packet (prefix + PacketInfo + termination)
    ///   - Sends it and advances `reliable_seq`
    /// Returns true on successful send.  This is the foundation for
    /// every ch=0 control opcode we native-emit (opcode-3, etc.).
    virtual bool send_ch0_reliable_payload(const std::string& client_key,
                                            const sockaddr_in& addr,
                                            const uint8_t* payload,
                                            size_t payload_bytes) = 0;

    /// Phase A.3 (2026-04-26) — allocate (or fetch existing) per-client
    /// NetGUID block.  Returns a struct containing minted PC, Pawn, and
    /// PlayerState NetGUIDs (and room for components).  Idempotent: the
    /// first call for a given client_key allocates a fresh block from a
    /// monotonic counter; subsequent calls return the same block.
    ///
    /// Used by PcEmitter/PawnEmitter to replace the captured PC NetGUID
    /// (10341530) with a server-minted GUID in the safe range
    /// (≥0x01000000), so we can incrementally retire the captured
    /// fixtures.  Block layout:
    ///   .player_controller   = base + 0    (PC actor)
    ///   .pawn                = base + 1
    ///   .player_state        = base + 2
    ///   .ability_component   = base + 3
    ///   ...etc up to base + 8
    virtual aoc::protocol::PlayerNetGuidBlock allocate_player_block(
        const std::string& client_key) = 0;

    /// PD2.3 (2026-05-05) — Get the customization JSON for the currently
    /// selected character.  Used by AppearanceEmitter to populate
    /// FCharacterCustomizationSaveData fields with real per-character
    /// values instead of zero defaults.  Returns empty string when no
    /// character is selected or no JSON could be extracted.
    virtual std::string current_character_customization_json() const = 0;
// [C O A ~ C_Y_B]

    /// PD3 (2026-05-05) — Get the currently-selected character's display
    /// name (e.g. "MaxPayne").  Used by PlayerStateEmitter to populate
    /// PlayerNamePrivate in the PlayerState ActorOpen / property update.
    /// Returns empty string when no character is selected.
    virtual std::string current_character_name() const = 0;

    /// 2026-05-05 — emit `ClientInitializeCharacter()` RPC (no-param) on
    /// the PC channel.  Wire handle 142 per
    /// `docs/RE-CLIENTINITIALIZECHARACTER-HANDLE.md`; probe-overridable
    /// via `dist/Release/probe_cic_handle.txt`.  Used by
    /// `WorldBootstrapEmitter::emit_all` (post-iter4) to trigger the
    /// client's local `SetRace()`/`SetGender()` flow that populates
    /// `RaceGenderAppearanceId` and unblocks
    /// `OnRep_CharacterCustomization` mesh assembly.
    virtual bool emit_client_initialize_character(
            const std::string& client_key,
            const sockaddr_in& addr) = 0;

    /// Road A — Phase B.0 (2026-04-26) — splice a captured packet from
    /// loaded ReplayData and send it to the client with our session's
    /// outer header (seq/ack/PacketInfo).  Used by WorldBootstrapEmitter
    /// to splice the ~75 bootstrap packets we don't yet emit natively.
    ///
    /// `replay_idx` is the 0-based index into the loaded replay's packet
    /// vector (e.g. 0 = post-NMT opcode-3, 22 = PC ActorOpen).  Returns
    /// true on successful send.  Returns false if:
    ///   - No replay loaded (--replay was not set)
    ///   - Index out of range
    ///   - Captured packet has bunch_bits == 0 (sentinel keepalive — the
    ///     plan should mark these as Skip, not Splice)
    ///   - Send-to-socket failed
    ///
    /// This is the only packet-emission primitive a "splice mode" entry
    /// needs.  Client sees byte-equivalent content to what the legacy
    /// replay_loop would have sent, so the world loads identically.
    virtual bool send_captured_packet(const std::string& client_key,
                                       const sockaddr_in& addr,
                                       uint32_t replay_idx) = 0;

    /// Road A — Phase B.0 — total number of loaded replay packets
    /// (so the bootstrap plan can iterate without hardcoding the count).
    /// Returns 0 if no replay is loaded.
    virtual uint32_t loaded_replay_packet_count() const = 0;

    /// The full captured S>C packet `replay_idx`, or nullptr when no replay
    /// is loaded or the index is out of range.  For native emitters that
    /// re-send a captured bunch payload under their own channel sequence.
    virtual const std::vector<uint8_t>* captured_packet_raw(uint32_t replay_idx) const = 0;

    /// Road A — Phase B.0f (2026-04-26) — emit native ClientRestart RPC.
    /// Constructs a bunch on the PC's actor channel (ch=3 reliable)
    /// carrying the ClientRestart RPC: function index (varint) + Pawn
    /// NetGUID (128 bits FIntrepidNetGUID).
    ///
    /// Function index hypothesis: APlayerController's RPC table position
    /// for ClientRestart is 26 (filtered by FUNC_Net), with a +5 offset
    /// for reserved system RPCs giving wire index 31 (varint byte 0x3E).
    /// Validated against ServerNotifyLoadedWorld where table pos 62 + 5 =
    /// 67 matches the captured first byte 0x86 (=(67<<1)|0 in AoC SIP).
    ///
    /// Returns true on successful send.  Used by the NMT-DETECT
    /// recognizer to react to ServerNotifyLoadedWorld.
    virtual bool send_client_restart_native(const std::string& client_key,
                                              const sockaddr_in& addr,
                                              uint64_t pawn_obj_id,
                                              uint32_t pawn_server_id,
                                              uint32_t pawn_randomizer) = 0;

    /// Phase B.0e2 (2026-04-27) — called by WorldBootstrapEmitter::emit_all
    /// once it has finished walking the plan.  Lets the host drain any
    /// queued post-bootstrap work — currently used to flush
    /// cs.pending_sulv_acks now that all spliced ch=3 reliable bunches
    /// have shipped (so cs.last_outgoing_reliable_chseq[3] is stable and
    /// our ACK can use the next contiguous chSeq).
    ///
    /// Default no-op so existing hosts that don't need it can skip it.
    virtual void on_world_bootstrap_complete(const std::string& client_key,
                                              const sockaddr_in& addr) {
        (void)client_key; (void)addr;
    }

    // NOTE: richer client-state access (seq counters etc.) intentionally
    // omitted; when a phase needs it, we'll add narrow accessors.
};

class NativeConnectSequencer {
public:
    NativeConnectSequencer(IGameServerHost& host,
                            std::string client_key,
                            sockaddr_in client_addr);
    ~NativeConnectSequencer();

    /// Called by GameServer when NMT_Join or NMT_GameSpecific arrives,
    /// signalling the client is ready to receive world data.  Kicks off
    /// the sequencer thread.
    void start();

    /// Signal the sequencer to wind down (client disconnected / server
    /// shutting down).  Joins the internal thread.
    void stop();

    /// Current state — safe to read from any thread.
    NativeConnectState state() const { return state_.load(); }

private:
    IGameServerHost& host_;
    std::string client_key_;

    // Storage for sockaddr_in — avoid pulling winsock2.h into the header.
    // The source file casts this to sockaddr_in.
    alignas(16) uint8_t client_addr_storage_[32]{};

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<NativeConnectState> state_{NativeConnectState::Idle};

    void run();  // thread body — walks state machine

    // ── Per-state handlers (stubs in M1.0) ────────────────────────────
    void do_await_nmt_join();
    void do_send_bootstrap();   // M1.1
    void do_send_pc_open();     // M1.2
    void do_send_pc_props();    // M1.2
    void do_send_pawn();        // M1.4.b — spliced Pawn ActorOpen
    void do_maintain();         // M1.3 / M1.4
};

}} // namespace aoc::net
