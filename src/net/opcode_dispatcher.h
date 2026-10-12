// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/opcode_dispatcher.h
//
//  The router.  Every incoming UDP datagram ends here first.  The dispatcher
//  decides what kind of packet it is (handshake, NMT control, game data),
//  looks up (or creates) the ClientSession, validates the phase, and calls
//  the appropriate handler method.
//
//  For Session E the scope is the SESSION STATE MACHINE: we drive a client
//  through AWAITING_HANDSHAKE → HANDSHAKE_IN_PROGRESS → NMT_NEGOTIATING →
//  AUTHENTICATED → LOADING_MAP → SPAWNING → IN_WORLD → DISCONNECTING in
//  response to synthetic packet types.
//
//  The actual wire-format parsing of each NMT message + the actor bunch
//  dispatch are stubs at this layer.  Session F swaps the stubs for real
//  parsing (OpcodeDispatcher sits on top of packet_parser + bunch_parser).
//
//  Architectural notes:
//    * The dispatcher is STATELESS itself.  All state lives in the
//      SessionRegistry.  Multiple dispatcher threads can safely process
//      different sessions in parallel.
//    * Handlers emit world events (via the optional events::EventBus
//      pointer) for observers — notably, the BroadcastManager watches
//      ActorSpawned to trigger outgoing bunches.  This keeps the dispatcher
//      decoupled from replication.
//    * NetGUID allocation happens inside handle_nmt_login.  If no allocator
//      is wired, that step is skipped and the session advances anyway — the
//      state machine is the primary contract, NetGUID allocation is a
//      side-effect for Session F.
//
//  LAYER:   Net / session (consumer of wire, producer of world events)
//  SESSION: E
// ============================================================================
#pragma once

#include "net/client_session.h"
#include "net/session_registry.h"
#include "protocol/net_guid_allocator.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Forward declare to avoid dragging the whole world_events.h in here
// (keeps the Session layer loosely coupled to the World layer).
namespace aoc { namespace world { namespace events { class EventBus; }}}

namespace aoc { namespace net {

// ─── Synthetic opcode used by tests and by real wire parsing ───────────────
//
// These enum values correspond to NMT_* control-channel messages plus a few
// pseudo-types the dispatcher uses to drive its state machine (e.g.
// HANDSHAKE_INITIAL covers the StatelessConnect Initial packet type).
//
// In Session F, the packet_parser + bunch_parser produce one of these
// synthetic opcodes per input packet/bunch.  For now, tests construct
// them directly.
enum class DispatchOp : uint16_t {
    // Pre-login / handshake pseudo-opcodes
    HANDSHAKE_INITIAL     = 0,   // StatelessConnect Initial (C→S)
    HANDSHAKE_CHALLENGE_RESPONSE = 1, // StatelessConnect Response (C→S)
// == CoA | C:Y:B ==

    // NMT control channel (indices match UE5 ENetControlMessage values)
    NMT_HELLO             = 100,
    NMT_WELCOME           = 101,
    NMT_UPGRADE           = 102,
    NMT_CHALLENGE         = 103,
    NMT_NETSPEED          = 104,
    NMT_LOGIN             = 105,
    NMT_FAILURE           = 106,
    NMT_JOIN              = 107,
    NMT_JOINSPLIT         = 108,
    NMT_SKIP              = 109,
    NMT_ABORT             = 110,
    NMT_PCSWAP            = 111,
    NMT_ACTORCHANNELFAIL  = 112,
    NMT_DEBUGTEXT         = 113,
    NMT_NETGUIDASSIGN     = 114,
    NMT_ENCRYPTIONACK     = 115,
    NMT_GAMESPECIFIC      = 116,
    NMT_SECURITYVIOLATION = 117,

    // Actor-channel bunches
    ACTOR_MOVEMENT        = 200,
    ACTOR_RPC             = 201,
    ACTOR_PROPERTY_DELTA  = 202,

    // Client-initiated cleanup
    CLIENT_DISCONNECT     = 300,

    // Unknown / unhandled
    UNKNOWN               = 0xFFFF,
};

/// A single payload to route.  Session E uses this as its primary input so
/// tests can construct packets without a full UDP framer.  Session F wraps
/// the packet_parser output in this same type.
struct DispatchPacket {
    DispatchOp op = DispatchOp::UNKNOWN;
    std::string client_key;         // "ip:port"
    std::vector<uint8_t> payload;   // opcode-specific bytes (parsed lazily by the handler)

    // Convenience fields populated by specific handlers; redundant with
    // `payload` but saves re-parsing in the minimal Session E test path.
    std::string str_arg;            // e.g. NMT_Login player name
    std::string str_arg2;           // e.g. NMT_Login online_id
    uint64_t    u64_arg = 0;        // e.g. actor netguid on movement packets
    uint32_t    fn_idx = 0;         // RPC function index (FieldNetIndex)
    std::string ability_guid;       // Formatted GUID for ability trigger
    int32_t     instance_id = 0;    // Trigger instance ID or End server instance ID
    int32_t     anim_index = 0;     // Combo/animation index
    uint64_t    hit_guid = 0;       // Hit Record GUID (RequestHitVerification_Server)
    std::string hit_component;      // Hit Component name (e.g. CollisionCylinder)
};

/// Outcome of a single dispatch call — lets tests assert what happened.
struct DispatchResult {
    bool        accepted = false;   // handler ran to completion
    std::string error;              // populated if rejected (wrong phase, bad input)
    ClientPhase old_phase = ClientPhase::AWAITING_HANDSHAKE;
    ClientPhase new_phase = ClientPhase::AWAITING_HANDSHAKE;
    bool        phase_changed = false;
};

class OpcodeDispatcher {
public:
    /// Construct with references to the session registry and (optionally)
    /// the NetGUID allocator + event bus.  Allocator and bus are optional
    /// because the Session E tests don't wire them — the state machine
    /// operates in isolation.
    OpcodeDispatcher(SessionRegistry& sessions,
                     protocol::NetGuidAllocator* netguid_alloc = nullptr,
                     world::events::EventBus* event_bus = nullptr);

    /// Primary entry point.  Creates the session if missing, routes to a
    /// handler based on op, returns the outcome.
    DispatchResult dispatch(const DispatchPacket& pkt);

    /// Diagnostic counters — number of times each handler ran.
    struct Stats {
        size_t handshake_initial = 0;
        size_t handshake_response = 0;
        size_t nmt_hello = 0;
        size_t nmt_login = 0;
        size_t nmt_welcome = 0;
        size_t nmt_join = 0;
        size_t nmt_abort = 0;
        size_t nmt_game_specific = 0;
        size_t actor_movement = 0;
        size_t actor_rpc = 0;
        size_t ability_trigger = 0;
        size_t ability_end = 0;
        size_t hit_verification = 0;
        size_t client_disconnect = 0;
        size_t rejected_wrong_phase = 0;
        size_t rejected_missing_session = 0;
        size_t unknown_op = 0;
    };
    const Stats& stats() const { return stats_; }

private:
    SessionRegistry&               sessions_;
    protocol::NetGuidAllocator*    netguid_alloc_;
    world::events::EventBus*       event_bus_;
    Stats                          stats_;

    // ── Handlers ──
    // Each handler receives a borrowed session pointer (guaranteed non-null
    // for handlers called below) and the full packet.  They validate the
    // current phase internally; on success they call transition_to() to
    // advance the state machine.
    DispatchResult handle_handshake_initial(ClientSession& cs, const DispatchPacket& pkt);
    DispatchResult handle_handshake_response(ClientSession& cs, const DispatchPacket& pkt);

    DispatchResult handle_nmt_hello(ClientSession& cs, const DispatchPacket& pkt);
    DispatchResult handle_nmt_login(ClientSession& cs, const DispatchPacket& pkt);
    DispatchResult handle_nmt_welcome(ClientSession& cs, const DispatchPacket& pkt);
    DispatchResult handle_nmt_join(ClientSession& cs, const DispatchPacket& pkt);
    DispatchResult handle_nmt_abort(ClientSession& cs, const DispatchPacket& pkt);
    DispatchResult handle_nmt_game_specific(ClientSession& cs, const DispatchPacket& pkt);

    DispatchResult handle_actor_movement(ClientSession& cs, const DispatchPacket& pkt);
    DispatchResult handle_actor_rpc(ClientSession& cs, const DispatchPacket& pkt);
    DispatchResult handle_client_disconnect(ClientSession& cs, const DispatchPacket& pkt);

    // ── Helpers ──
    DispatchResult make_reject(ClientSession& cs, const std::string& reason);
    DispatchResult make_accept(ClientSession& cs, ClientPhase before);
};

}} // namespace aoc::net
