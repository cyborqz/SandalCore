// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/live_world.h
//
//  THE Session F deliverable.  One object that owns:
//    - EventBus          (world/events)
//    - WorldClock        (world/simulation)
//    - ActorRegistry     (world/simulation)
//    - VisibilityManager (world/replication)
//    - BroadcastManager  (world/replication)
//    - IPacketEmitter    (net/udp_packet_emitter — recording or UDP)
//    - SchemaRegistry    (protocol/schema, singleton — held by reference)
//    - SessionRegistry   (net)
//    - NetGuidAllocator  (protocol)
//    - OpcodeDispatcher  (net)
//    - A replication tick thread at config.replication_hz
//
//  In other words: plug in a packet source on one side, plug in a packet sink
//  on the other, and you have a working live server — minus the UDP framing
//  which GameServer still owns.
//
//  The GameServer embeds a LiveWorld when Config::enable_live_world is true.
//  All post-handshake data-channel traffic flows through LiveWorld::on_packet,
//  and every world change fans back out through the emitter.
//
//  Thread model:
//    * LiveWorld::on_packet() is called from GameServer's recv thread.  It
//      calls dispatcher_.dispatch() which locks the session registry briefly.
//    * A dedicated replication-tick thread calls broadcast_.tick() at
//      config.replication_hz.  tick() is the only thread that emits packets.
//    * Start and stop are idempotent and thread-safe.
//
//  LAYER:   Net / integration (the Session F glue)
//  SESSION: F
// ============================================================================
#pragma once

#include "net/client_session.h"
#include "net/session_registry.h"
#include "net/opcode_dispatcher.h"
#include "net/udp_packet_emitter.h"
#include "protocol/net_guid_allocator.h"
#include "protocol/schema/schema_registry.h"
#include "world/events/world_events.h"
#include "world/simulation/world_clock.h"
#include "world/simulation/actor_registry.h"
#include "world/replication/visibility_manager.h"
#include "world/replication/broadcast_manager.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

namespace aoc { namespace net {

/// Configuration for LiveWorld.  Sane defaults for a single-server dev box.
struct LiveWorldConfig {
    /// How often BroadcastManager::tick() runs.  Per architectural correction
    /// 3, this is INDEPENDENT of simulation_hz.  20 Hz is the UE5-standard
    /// NetUpdateFrequency default for most gameplay.
    uint32_t replication_hz = 20;

    /// If true, LiveWorld uses a RecordingPacketEmitter (no real UDP).  Used
    /// by tests and by --replay-mode runs that still want to exercise the
    /// pipeline for diagnostics without sending bytes.
    bool use_recording_emitter = false;
};
// (C | O | A ~ Cyb)

class LiveWorld {
public:
    /// Construct with shared schema registry.  The schema registry is a
    /// singleton elsewhere; we keep a reference to avoid re-init costs.
    ///
    /// `outer_fn` — when non-null, LiveWorld builds COMPLETE UE5 S>C packets
    /// (Session G). When null, LiveWorld falls back to a RecordingEmitter.
    /// `send_fn` — receives the finished byte buffer.  In dry-run mode
    /// GameServer wires this to a log-only callback; in active-send mode
    /// it forwards to sendto.
    ///
    /// Either pass both callbacks (active emission) or neither
    /// (recording-only, tests).  Passing only one is treated as "recording".
    LiveWorld(const protocol::schema::SchemaRegistry& schemas,
              UdpPacketEmitter::OuterStateFn outer_fn,
              UdpPacketEmitter::SendFn send_fn,
              LiveWorldConfig config = {});

    ~LiveWorld();

    LiveWorld(const LiveWorld&) = delete;
    LiveWorld& operator=(const LiveWorld&) = delete;

    /// Start the replication tick thread.  Idempotent.
    void start();

    /// Stop the tick thread and wait for it to exit.  Idempotent.
    void stop();

    /// Called from GameServer when a new client's handshake completes.
    /// Creates the session if it doesn't exist and registers the client
    /// with VisibilityManager so future spawns fan out to it.
    void on_client_connected(const std::string& client_key);

    /// Called from GameServer when a client disconnects.  Emits destroy
    /// bunches for that client's actors and cleans up the session.
    void on_client_disconnected(const std::string& client_key);

    /// Called from GameServer's recv thread with a post-handshake packet.
    /// For Session F, opcode is extracted by the caller (GameServer) based
    /// on packet framing; LiveWorld just routes it to the dispatcher.
    /// Session H.2: after the dispatcher accepts, LiveWorld may trigger
    /// outgoing NMT replies (e.g. NMT_Login → NMT_Welcome) via UdpEmitter.
    DispatchResult on_packet(const DispatchPacket& pkt);

    /// Session H.2 config — what level / gamemode / challenge-string to
    /// put into our synthesized NMT replies.  Defaults match what the
    /// captured real AoC server sent in the bootstrap; override via
    /// setter if you want to test with different content.
    struct NmtReplyConfig {
        std::string welcome_level     = "/Game/Levels/Verra_World_Master/Verra_World_Master";
        std::string welcome_gamemode  = "/Game/GameBlueprints/AoCGameModeBaseBP.AoCGameModeBaseBP_C";
        std::string welcome_redirect  = "";
        std::string challenge_string  = "50995344";
    };
    void set_nmt_reply_config(NmtReplyConfig cfg) { nmt_reply_cfg_ = std::move(cfg); }
    const NmtReplyConfig& nmt_reply_config() const { return nmt_reply_cfg_; }

    /// Observer-mode hook: called from GameServer for every post-handshake
    /// data packet while Session G wire-format parsing isn't yet plumbed in.
    /// Touches session activity + increments the stats counter so the
    /// heartbeat log reflects real traffic.  No dispatch happens here.
    void on_data_packet_observed(const std::string& client_key);

    // ── Accessors (for tests and diagnostics) ─────────────────────────
    SessionRegistry&                          sessions() { return sessions_; }
    const SessionRegistry&                    sessions() const { return sessions_; }
    OpcodeDispatcher&                         dispatcher() { return dispatcher_; }
    world::simulation::ActorRegistry&         actors() { return actors_; }
    world::replication::VisibilityManager&    visibility() { return visibility_; }
    world::replication::BroadcastManager&     broadcast() { return broadcast_; }
    world::events::EventBus&                  events() { return events_; }
    protocol::NetGuidAllocator&               netguid_allocator() { return netguid_; }

    /// If the LiveWorld was constructed with a null send_fn (or
    /// use_recording_emitter = true), this returns a non-null pointer to
    /// the recording emitter.  Otherwise returns nullptr.
    RecordingPacketEmitter* recording_emitter() { return recording_.get(); }
// CoA x C.Y.B

    /// Runtime stats — handy for tests.
    struct Stats {
        uint64_t tick_count        = 0;
        uint64_t total_packets_in  = 0;
        uint64_t total_rejects     = 0;
        std::chrono::steady_clock::time_point started_at;
    };
    Stats stats() const;

private:
    LiveWorldConfig                                config_;

    // Construction order matters — clocks/events first, then state,
    // then replication, finally session/dispatcher.  Member declaration
    // order mirrors the constructor initializer list.
    world::events::EventBus                         events_;
    world::simulation::WorldClock                   clock_;
    world::simulation::ActorRegistry                actors_;
    world::replication::VisibilityManager           visibility_;

    // Emitter lives behind unique_ptr because IPacketEmitter is polymorphic.
    std::unique_ptr<RecordingPacketEmitter>         recording_;
    std::unique_ptr<UdpPacketEmitter>               udp_;
    world::replication::IPacketEmitter*             emitter_ = nullptr;

    world::replication::BroadcastManager            broadcast_;
    protocol::NetGuidAllocator                      netguid_;
    SessionRegistry                                  sessions_;
    OpcodeDispatcher                                 dispatcher_;

    // Replication tick thread
    std::atomic<bool>                                running_{false};
    std::thread                                      tick_thread_;
    mutable std::mutex                               stats_mu_;
    Stats                                            stats_;

    NmtReplyConfig                                   nmt_reply_cfg_;

    void tick_loop();

    /// Session H.2 — dispatcher post-hook: trigger outgoing NMT replies
    /// based on the accepted op + phase transition.  No-op if no UDP
    /// emitter is wired (recording mode or null).
    void maybe_send_nmt_reply(const DispatchPacket& pkt,
                                const DispatchResult& r);
};

}} // namespace aoc::net
