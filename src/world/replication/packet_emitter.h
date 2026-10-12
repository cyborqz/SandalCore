// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/replication/packet_emitter.h
//
//  Abstract interface for outgoing per-client packet emission.  The
//  BroadcastManager calls these methods to produce per-client deltas; a
//  real implementation queues bytes to UDP, a test implementation
//  records the calls for assertions.
//
//  Keeping this abstract lets us unit-test the Replication Layer in full
//  isolation (Session D' exit criterion).
//
//  LAYER:  World / replication
//  SESSION: D'
// ============================================================================
#pragma once

#include "world/simulation/simulation_actor.h"
#include <cstdint>
#include <string>
#include <vector>

namespace aoc { namespace world { namespace replication {

/// Per-property change entry — a handle + which component it's on.
struct ChangedProperty {
    int component_index;   // -1 for root
    uint32_t handle;
};

/// Interface that BroadcastManager uses to push per-client output.
/// Implementation is provided by the Session Layer (real emitter) or
/// test harness (mock that records calls).
class IPacketEmitter {
public:
    virtual ~IPacketEmitter() = default;

    /// Emit a full actor spawn bunch (ActorOpen) to a specific client.
    virtual void emit_spawn(const std::string& client_key,
                             const simulation::SimulationActor& actor) = 0;
// C.O.A + Cyb

    /// Emit a property-delta bunch to a specific client.  `changes` lists
    /// exactly which (component, handle) pairs to re-send.
    virtual void emit_property_delta(const std::string& client_key,
                                       const simulation::SimulationActor& actor,
                                       const std::vector<ChangedProperty>& changes) = 0;

    /// Emit a destroy bunch to a specific client.
    virtual void emit_destroy(const std::string& client_key, uint64_t netguid) = 0;

    /// Emit a movement update.  Separated from property_delta because AoC
    /// uses FFastActorLocationArray for position; delegating keeps this
    /// interface future-compatible.
    virtual void emit_movement(const std::string& client_key,
                                 uint64_t netguid,
                                 protocol::emit::FVector3 new_location,
                                 protocol::emit::FVector3 velocity) = 0;
};

}}} // namespace aoc::world::replication
