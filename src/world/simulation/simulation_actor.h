// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/simulation/simulation_actor.h
//
//  An actor instance in the simulated world.  Owns authoritative state:
//  property values, position, ownership metadata.
//
//  Contains an emit::ActorRuntime internally — SimulationActor IS the
//  source of truth for what ends up in a spawn bunch when the emitter
//  needs one.  But SimulationActor ALSO carries simulation-only data
//  (e.g. "who damaged me last") that doesn't belong in replication.
//
//  Per architectural correction 1: this type lives in the Simulation Layer,
//  and DOES NOT know about clients, channels, or per-client replication
//  keys — that's the Replication Layer's job.
//
//  LAYER:  World / simulation
//  SESSION: D
// ============================================================================
#pragma once

#include "protocol/emit/schema_value.h"
#include "protocol/schema/actor_schema.h"
#include <cstdint>
#include <string>
#include <chrono>

namespace aoc { namespace world { namespace simulation {

/// An authoritative world actor.  Wraps an emit::ActorRuntime with sim-only
/// lifecycle + ownership metadata.
struct SimulationActor {
    // Identity
    uint64_t netguid = 0;
    protocol::schema::ActorType type = protocol::schema::ActorType::StaticWorld;
// [Cyb x C O A]

    // Ownership — which client "owns" this actor (empty = server-owned)
    std::string owner_client_key;

    // The serializable state (property values, spawn transform, NetGUIDs)
    protocol::emit::ActorRuntime runtime;

    // Lifecycle timestamps (from WorldClock)
    uint64_t spawned_at_ms = 0;
    uint64_t last_modified_ms = 0;

    // Simulation-only bookkeeping (NOT replicated)
    uint64_t last_damage_source_netguid = 0;   // "who hit me last" for xp/loot
    uint32_t tick_counter = 0;                 // how many sim ticks this actor has seen

    // ── Helpers (simple forwarders to runtime) ──
    void set_root(uint32_t handle, protocol::emit::SchemaValue v) {
        runtime.set_root(handle, std::move(v));
    }
    void set_component(int component_index, uint32_t handle, protocol::emit::SchemaValue v) {
        runtime.set_component(component_index, handle, std::move(v));
    }
    const protocol::emit::SchemaValue* get_root(uint32_t handle) const {
        return runtime.get_root(handle);
    }
    const protocol::emit::SchemaValue* get_component(int component_index, uint32_t handle) const {
        return runtime.get_component(component_index, handle);
    }
};

}}} // namespace aoc::world::simulation
