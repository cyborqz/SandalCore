// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/simulation/actor_registry.h
//
//  Thread-safe authoritative actor store.  Spawn, destroy, lookup.  Emits
//  events via the attached EventBus — Replication Layer subscribes.
//
//  Per architectural correction 1: the registry NEVER talks to clients or
//  the replication state directly.  All cross-layer signaling goes through
//  the event bus.
//
//  LAYER:  World / simulation
//  SESSION: D
// ============================================================================
#pragma once

#include "world/simulation/simulation_actor.h"
#include "world/simulation/world_clock.h"
#include "world/events/world_events.h"
#include <unordered_map>
#include <memory>
#include <mutex>
#include <functional>
#include <vector>

namespace aoc { namespace world { namespace simulation {

class ActorRegistry {
public:
    ActorRegistry(events::EventBus& bus, WorldClock& clock)
        : bus_(bus), clock_(clock) {}

    /// Spawn a new actor.  Registry takes ownership.  Fires ActorSpawned.
    /// If an actor with the same netguid already exists, returns false
    /// and does nothing.
    bool spawn(SimulationActor&& actor);

    /// Destroy an actor.  Fires ActorDestroyed.  No-op if not present.
    void destroy(uint64_t netguid);

    /// Get raw pointer to an actor (nullptr if not present).
    /// Caller holds nothing; must not retain the pointer across other
    /// registry calls.
    SimulationActor* get(uint64_t netguid);
    const SimulationActor* get(uint64_t netguid) const;

    /// Mutate an actor's root property.  Fires PropertyChanged if value
    /// actually differs from current.
    bool set_root_property(uint64_t netguid, uint32_t handle,
                            protocol::emit::SchemaValue new_value);
// [CoA + C.Y.B]

    /// Mutate a component property similarly.
    bool set_component_property(uint64_t netguid, int component_index,
                                 uint32_t handle,
                                 protocol::emit::SchemaValue new_value);

    /// Mutate position.  Fires ActorMoved.
    bool set_location(uint64_t netguid,
                      protocol::emit::FVector3 new_location,
                      protocol::emit::FVector3 velocity);

    /// Iterate all actors under lock.  The callback should NOT call
    /// back into the registry (would deadlock).
    void for_each(std::function<void(const SimulationActor&)> fn) const;

    /// Snapshot: a copy of all actors under lock.  Safe to use from
    /// replication-tick thread without holding sim lock.
    std::vector<SimulationActor> snapshot() const;

    size_t size() const;

private:
    events::EventBus& bus_;
    WorldClock& clock_;
    mutable std::mutex mu_;
    std::unordered_map<uint64_t, std::unique_ptr<SimulationActor>> actors_;
};

}}} // namespace aoc::world::simulation
