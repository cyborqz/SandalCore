// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/client_session.h
//
//  Per-client session state — the Session Layer of the 4-layer architecture.
//
//  A ClientSession is the server's memory of one connected player.  It tracks
//  where the client is in the connect → play → disconnect lifecycle (phase),
//  what actors belong to the client (PC / Pawn / PlayerState NetGUIDs),
//  authentication and identity fields, and reliability sequence bookkeeping.
//
//  Per the implementation plan Section 2, this struct is intentionally plain
//  data.  The state *transitions* happen inside OpcodeDispatcher.  The
//  SessionRegistry is what owns these objects and addresses them by
//  "ip:port".
//
//  Design notes:
//    * MVP field set — what we can actually populate/consume today.  Some
//      fields (reliable_ch_seq, unacked_packets) are stubbed for Session F
//      where they get populated by the real UDP emitter.
//    * Copyable?  No.  Sessions contain mutexes and OS handles; move-only
//      via unique_ptr in the registry.
//    * Thread-safety: a ClientSession is protected by a per-session mutex
//      owned by the registry.  Handlers lock once at the top of dispatch
//      and operate freely inside.  Cross-session work (broadcast) reads
//      snapshots, never holds two session locks at once.
//
//  LAYER:   Net / session
//  SESSION: E
// ============================================================================
#pragma once

#include "protocol/character_profile.h"
#include "protocol/net_guid_allocator.h"
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <netinet/in.h>
#endif

namespace aoc { namespace net {

/// Where this client is in the connect → in-world → disconnect pipeline.
/// Transitions happen INSIDE OpcodeDispatcher as a response to specific
/// packets (or internal timeouts).  Handlers validate their expected phase
/// before acting; unexpected-phase packets are logged + dropped.
enum class ClientPhase : uint8_t {
    AWAITING_HANDSHAKE,        // server has seen zero packets from this client yet
    HANDSHAKE_IN_PROGRESS,     // StatelessConnect Initial/Challenge/Response in flight
    NMT_NEGOTIATING,           // post-handshake, pre-login (NMT_Hello / NMT_Welcome)
    AUTHENTICATED,             // NMT_Login accepted, profile + NetGUID block allocated
    LOADING_MAP,               // NMT_Welcome(level) sent, client is loading
    SPAWNING,                  // NMT_Join received, streaming initial actors
    IN_WORLD,                  // gameplay
    DISCONNECTING,             // close bunches pending ack, or graceful timeout
};

/// Stringify helper — handy for log output and test assertions.
inline const char* to_string(ClientPhase p) {
    switch (p) {
        case ClientPhase::AWAITING_HANDSHAKE:    return "AWAITING_HANDSHAKE";
        case ClientPhase::HANDSHAKE_IN_PROGRESS: return "HANDSHAKE_IN_PROGRESS";
        case ClientPhase::NMT_NEGOTIATING:       return "NMT_NEGOTIATING";
        case ClientPhase::AUTHENTICATED:         return "AUTHENTICATED";
        case ClientPhase::LOADING_MAP:           return "LOADING_MAP";
        case ClientPhase::SPAWNING:              return "SPAWNING";
        case ClientPhase::IN_WORLD:              return "IN_WORLD";
        case ClientPhase::DISCONNECTING:         return "DISCONNECTING";
    }
    return "UNKNOWN";
}
// [C | Y | B :: C.O.A]

/// One queued outgoing UDP packet awaiting ack or retransmit.  Populated by
/// the Emitter Layer in Session F; declared here so ClientSession has
/// somewhere to hold them.
struct OutgoingPacket {
    uint16_t seq = 0;
    std::vector<uint8_t> bytes;
    std::chrono::steady_clock::time_point sent_at;
};

/// The per-client session record.  One instance per (ip, port) currently
/// connected to the server.
struct ClientSession {
    // ─── Identity ──────────────────────────────────────────────────────────
    std::string client_key;              // "192.0.2.4:56789" — primary key in SessionRegistry
    sockaddr_in remote_addr{};           // populated at first-packet time

    // ─── Phase ─────────────────────────────────────────────────────────────
    ClientPhase phase = ClientPhase::AWAITING_HANDSHAKE;
    std::chrono::steady_clock::time_point phase_entered_at =
        std::chrono::steady_clock::now();

    // ─── NMT / identity (populated progressively by the dispatcher) ───────
    std::string player_name;             // from NMT_Login URL (?name=Hatemost)
    std::string online_id;               // platform ID from NMT_Login
    std::string auth_token;              // from StatelessConnect custom extension or NMT_Login

    // Character profile — loaded from XClientService after NMT_Login accepts.
    // The CharacterProfile itself is the handshake with the emitter (Session C).
    protocol::CharacterProfile profile;

    // ─── Assigned resources ───────────────────────────────────────────────
    protocol::PlayerNetGuidBlock netguid_block;  // allocated at NMT_Login time
    uint64_t pc_netguid          = 0;   // mirror of block.player_controller for convenience
    uint64_t pawn_netguid        = 0;   // mirror of block.pawn
    uint64_t player_state_netguid = 0;  // mirror of block.player_state

    // ─── Handshake state (StatelessConnect) ───────────────────────────────
    uint8_t secret[32]        = {};      // per-session random secret
    uint8_t challenge_cookie[20] = {};   // HMAC we issued in Challenge

    // ─── Sequence / reliability tracking ──────────────────────────────────
    uint16_t in_seq_last_ack = 0;        // last packet seq we acked
    uint16_t out_seq_next    = 1;        // next outgoing seq we'll send
    std::unordered_map<uint32_t, uint32_t> reliable_ch_seq;  // per-channel, for ACK matching
    std::deque<OutgoingPacket> unacked_packets;              // retransmit queue (Session F)

    // ─── Liveness / timeouts ──────────────────────────────────────────────
    std::chrono::steady_clock::time_point last_activity =
        std::chrono::steady_clock::now();

    // ─── Visibility set ───────────────────────────────────────────────────
    // NetGUIDs of actors this client has received spawn bunches for.  The
    // BroadcastManager reads this + the VisibilityManager to compute diffs.
    std::unordered_set<uint64_t> visible_actors;

    // ─── Active ability state ─────────────────────────────────────────────
    std::string active_ability_guid_str;
    int32_t     active_ability_instance_id = 0;
    int32_t     active_ability_anim_index  = 0;
    uint64_t    total_abilities_triggered  = 0;
    uint64_t    total_abilities_ended      = 0;
    uint64_t    total_hits_verified        = 0;
    uint64_t    total_hits_confirmed       = 0;
    uint64_t    last_verified_ability_guid = 0;
    uint64_t    last_verified_hit_guid     = 0;
    std::string last_verified_hit_component;

    // ─── Diagnostics ──────────────────────────────────────────────────────
    uint64_t packets_received = 0;
    uint64_t packets_sent     = 0;

    // ─── Phase helpers (no logic, just bookkeeping) ───────────────────────
    void transition_to(ClientPhase next) {
        phase = next;
        phase_entered_at = std::chrono::steady_clock::now();
    }
    void touch() { last_activity = std::chrono::steady_clock::now(); }

    // Non-copyable by convention (holds OS handles + state machine).
    ClientSession() = default;
    ClientSession(const ClientSession&) = delete;
    ClientSession& operator=(const ClientSession&) = delete;
    ClientSession(ClientSession&&) = default;
    ClientSession& operator=(ClientSession&&) = default;
};

}} // namespace aoc::net
