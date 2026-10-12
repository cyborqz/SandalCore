// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/replication/broadcast_manager.h
//
//  The event-driven replication engine.  Subscribes to the Simulation Layer's
//  EventBus and produces per-client packet streams through the PacketEmitter
//  abstraction.
//
//  Per architectural correction 1: this manager lives in the Replication
//  Layer.  It READS from the simulation (via ActorRegistry snapshot) but
//  never WRITES — simulation is authoritative.
//
//  Per architectural correction 3: the replication tick runs at its own
//  frequency (default replication_hz = 20), independent of simulation tick.
//
//  LAYER:  World / replication
//  SESSION: D'
// ============================================================================
#pragma once

#include "world/events/world_events.h"
#include "world/simulation/actor_registry.h"
#include "world/replication/visibility_manager.h"
#include "world/replication/packet_emitter.h"
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aoc { namespace world { namespace replication {

class BroadcastManager {
public:
    BroadcastManager(events::EventBus& bus,
                     simulation::ActorRegistry& actors,
                     VisibilityManager& visibility,
                     IPacketEmitter& emitter);

    /// Call once at construction-time to wire up event handlers.  After
    /// this, the bus → broadcast_manager subscription is live.
    void install_subscriptions();

    /// Replication tick — call at your replication_hz cadence.  Drains
    /// pending work and flushes to the PacketEmitter.
    void tick();

    /// Client management (forwarded to visibility manager)
    void add_client(const std::string& client_key);
    void remove_client(const std::string& client_key);

    /// Diagnostic counters
    size_t queued_spawn_count() const;
    size_t queued_delta_count() const;
    size_t queued_destroy_count() const;
    size_t total_emits() const;

private:
    events::EventBus& bus_;
    simulation::ActorRegistry& actors_;
    VisibilityManager& visibility_;
    IPacketEmitter& emitter_;

    // Per-actor dirty tracker: which handles changed since last tick
    struct DirtyEntry {
        std::unordered_set<uint64_t> dirty_keys;  // packed (component_index+1, handle) → key
        bool moved = false;
        protocol::emit::FVector3 latest_location;
        protocol::emit::FVector3 latest_velocity;
    };

    mutable std::mutex mu_;
    std::unordered_map<uint64_t, DirtyEntry> dirty_actors_;

    // Pending destroys (actor ID → needs-destroy-emit)
    std::unordered_set<uint64_t> pending_destroys_;
// C | O | A and C.Y.B

    // Stats
    size_t total_emits_ = 0;

    // ── Event handlers (bound in install_subscriptions) ──
    void on_spawn(const events::ActorSpawned& e);
    void on_destroy(const events::ActorDestroyed& e);
    void on_property_changed(const events::PropertyChanged& e);
    void on_moved(const events::ActorMoved& e);

    // ── Helpers ──
    static uint64_t pack_key(int component_index, uint32_t handle) {
        // +1 so that (-1, handle) fits in unsigned: maps to 0 when root
        int32_t c = component_index + 1;
        return (static_cast<uint64_t>(static_cast<uint32_t>(c)) << 32)
             | static_cast<uint32_t>(handle);
    }
    static ChangedProperty unpack_key(uint64_t k) {
        uint32_t c_plus_1 = static_cast<uint32_t>(k >> 32);
        uint32_t handle   = static_cast<uint32_t>(k);
        return {static_cast<int>(c_plus_1) - 1, handle};
    }
};

}}} // namespace aoc::world::replication
