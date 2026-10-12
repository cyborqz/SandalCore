// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/events/world_events.h
//
//  Typed event bus connecting the Simulation Layer (authoritative state) to
//  the Replication Layer (what gets sent to whom) — per architectural
//  correction 1 in the implementation plan.
//
//  Simulation code emits events; Replication code subscribes to them.
//  They do NOT call each other directly.
//
//  Event types:
//    ActorSpawned     - a new actor entered the world
//    ActorDestroyed   - an actor was removed
//    PropertyChanged  - a property value changed on an existing actor
//    ActorMoved       - position/rotation/velocity updated (high-frequency;
//                       separate from PropertyChanged for batching)
//
//  Design:
//    - Events are POD structs (copyable, no side effects in constructors)
//    - Listeners are std::function callbacks
//    - Single-threaded dispatch by default; callers wrap in a mutex if
//      they need cross-thread emission (world tick typically fires
//      events on its own thread, listener should be reentrancy-safe)
//
//  LAYER:  World / events
//  SESSION: D
// ============================================================================
#pragma once

#include "protocol/schema/actor_schema.h"
#include "protocol/emit/schema_value.h"
#include <cstdint>
#include <functional>
#include <vector>
#include <mutex>

namespace aoc { namespace world { namespace events {

// ─── Event payload structs ──────────────────────────────────────────────────

struct ActorSpawned {
    uint64_t netguid;
    protocol::schema::ActorType type;
    std::string owner_client_key;  // empty = server-owned (NPC, world actor)
    uint64_t timestamp_ms;
};

struct ActorDestroyed {
    uint64_t netguid;
    uint64_t timestamp_ms;
};

struct PropertyChanged {
    uint64_t netguid;
    // -1 if root, else component index
    int component_index;
    uint32_t handle;
    // Snapshot of new value; replication consumer diffs against last-sent.
    protocol::emit::SchemaValue new_value;
    uint64_t timestamp_ms;
};

struct ActorMoved {
    uint64_t netguid;
    protocol::emit::FVector3 old_location;
    protocol::emit::FVector3 new_location;
    protocol::emit::FVector3 velocity;
    uint64_t timestamp_ms;
};

// ─── Event bus ──────────────────────────────────────────────────────────────
// ~ C.Y.B - C O A ~

class EventBus {
public:
    // Subscribe to each event type; returns a subscriber ID for later
    // unregistration (or ignore it for lifetime-of-bus subscriptions).
    using SubId = uint64_t;

    template <typename EventT>
    SubId subscribe(std::function<void(const EventT&)> cb);

    // Dispatch — synchronous; callback runs on the emitting thread.
    void emit(const ActorSpawned& e);
    void emit(const ActorDestroyed& e);
    void emit(const PropertyChanged& e);
    void emit(const ActorMoved& e);

    // Diagnostics
    size_t listener_count() const;

private:
    mutable std::mutex mu_;
    SubId next_id_ = 1;

    std::vector<std::pair<SubId, std::function<void(const ActorSpawned&)>>>  on_spawn_;
    std::vector<std::pair<SubId, std::function<void(const ActorDestroyed&)>>> on_destroy_;
    std::vector<std::pair<SubId, std::function<void(const PropertyChanged&)>>> on_prop_changed_;
    std::vector<std::pair<SubId, std::function<void(const ActorMoved&)>>> on_moved_;
};

// ─── Template specializations (header-defined) ──────────────────────────────

template<> inline EventBus::SubId EventBus::subscribe<ActorSpawned>(
    std::function<void(const ActorSpawned&)> cb) {
    std::lock_guard<std::mutex> lk(mu_);
    SubId id = next_id_++;
    on_spawn_.emplace_back(id, std::move(cb));
    return id;
}
template<> inline EventBus::SubId EventBus::subscribe<ActorDestroyed>(
    std::function<void(const ActorDestroyed&)> cb) {
    std::lock_guard<std::mutex> lk(mu_);
    SubId id = next_id_++;
    on_destroy_.emplace_back(id, std::move(cb));
    return id;
}
template<> inline EventBus::SubId EventBus::subscribe<PropertyChanged>(
    std::function<void(const PropertyChanged&)> cb) {
    std::lock_guard<std::mutex> lk(mu_);
    SubId id = next_id_++;
    on_prop_changed_.emplace_back(id, std::move(cb));
    return id;
}
template<> inline EventBus::SubId EventBus::subscribe<ActorMoved>(
    std::function<void(const ActorMoved&)> cb) {
    std::lock_guard<std::mutex> lk(mu_);
    SubId id = next_id_++;
    on_moved_.emplace_back(id, std::move(cb));
    return id;
}

}}} // namespace aoc::world::events
