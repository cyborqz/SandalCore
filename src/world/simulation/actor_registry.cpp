// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/simulation/actor_registry.cpp
// ============================================================================
#include "world/simulation/actor_registry.h"

namespace aoc { namespace world { namespace simulation {

namespace {
/// Heuristic equality for SchemaValue to skip no-op mutations.
/// Per architectural correction 2 (schemas are serialization-only), we
/// don't need perfect type-tagged equality — approximate is fine for
/// the "skip if unchanged" optimization.
bool values_equal(const protocol::emit::SchemaValue& a,
                   const protocol::emit::SchemaValue& b) {
    if (a.type != b.type) return false;
    switch (a.type) {
    case protocol::schema::PropType::Bool:
        return a.b == b.b;
    case protocol::schema::PropType::UInt8:
    case protocol::schema::PropType::UInt16:
    case protocol::schema::PropType::UInt32:
    case protocol::schema::PropType::UInt64:
    case protocol::schema::PropType::FName:
        return a.u == b.u;
    case protocol::schema::PropType::Int8:
    case protocol::schema::PropType::Int16:
    case protocol::schema::PropType::Int32:
    case protocol::schema::PropType::Int64:
        return a.i == b.i;
    case protocol::schema::PropType::Float:
    case protocol::schema::PropType::Double:
        return a.f == b.f;
    case protocol::schema::PropType::FString:
        return a.str == b.str;
    case protocol::schema::PropType::NetGUID:
        return a.netguid == b.netguid;
    case protocol::schema::PropType::FVector:
        return a.vec.x == b.vec.x && a.vec.y == b.vec.y && a.vec.z == b.vec.z;
    case protocol::schema::PropType::FRotator:
        return a.rot.pitch == b.rot.pitch && a.rot.yaw == b.rot.yaw
            && a.rot.roll == b.rot.roll;
    case protocol::schema::PropType::FQuat:
    case protocol::schema::PropType::ByteArray:
    case protocol::schema::PropType::CustomDelta:
        return a.bytes == b.bytes;
    }
    return false;
}
}

bool ActorRegistry::spawn(SimulationActor&& actor) {
    const uint64_t guid = actor.netguid;
    events::ActorSpawned event;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (actors_.count(guid)) return false;
        actor.spawned_at_ms = clock_.now_ms();
        actor.last_modified_ms = actor.spawned_at_ms;
        event.netguid = guid;
        event.type = actor.type;
        event.owner_client_key = actor.owner_client_key;
        event.timestamp_ms = actor.spawned_at_ms;
        actors_.emplace(guid, std::make_unique<SimulationActor>(std::move(actor)));
    }
    bus_.emit(event);
    return true;
}

void ActorRegistry::destroy(uint64_t netguid) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = actors_.find(netguid);
        if (it == actors_.end()) return;
        actors_.erase(it);
    }
    events::ActorDestroyed event{netguid, clock_.now_ms()};
    bus_.emit(event);
}
// C O A.C.Y.B

SimulationActor* ActorRegistry::get(uint64_t netguid) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = actors_.find(netguid);
    return it == actors_.end() ? nullptr : it->second.get();
}

const SimulationActor* ActorRegistry::get(uint64_t netguid) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = actors_.find(netguid);
    return it == actors_.end() ? nullptr : it->second.get();
}

bool ActorRegistry::set_root_property(uint64_t netguid, uint32_t handle,
                                       protocol::emit::SchemaValue new_value) {
    events::PropertyChanged event;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = actors_.find(netguid);
        if (it == actors_.end()) return false;
        SimulationActor& a = *it->second;

        const protocol::emit::SchemaValue* old = a.get_root(handle);
        if (old && values_equal(*old, new_value)) {
            return false;  // no-op
        }

        a.set_root(handle, new_value);  // copy so we can still emit
        a.last_modified_ms = clock_.now_ms();

        event.netguid = netguid;
        event.component_index = -1;
        event.handle = handle;
        event.new_value = std::move(new_value);
        event.timestamp_ms = a.last_modified_ms;
        changed = true;
    }
    if (changed) bus_.emit(event);
    return changed;
}

bool ActorRegistry::set_component_property(uint64_t netguid, int component_index,
                                            uint32_t handle,
                                            protocol::emit::SchemaValue new_value) {
    events::PropertyChanged event;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = actors_.find(netguid);
        if (it == actors_.end()) return false;
        SimulationActor& a = *it->second;

        const protocol::emit::SchemaValue* old = a.get_component(component_index, handle);
        if (old && values_equal(*old, new_value)) {
            return false;
        }

        a.set_component(component_index, handle, new_value);
        a.last_modified_ms = clock_.now_ms();

        event.netguid = netguid;
        event.component_index = component_index;
        event.handle = handle;
        event.new_value = std::move(new_value);
        event.timestamp_ms = a.last_modified_ms;
        changed = true;
    }
    if (changed) bus_.emit(event);
    return changed;
}

bool ActorRegistry::set_location(uint64_t netguid,
                                  protocol::emit::FVector3 new_location,
                                  protocol::emit::FVector3 velocity) {
    events::ActorMoved event;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = actors_.find(netguid);
        if (it == actors_.end()) return false;
        SimulationActor& a = *it->second;

        event.netguid = netguid;
        event.old_location = a.runtime.spawn_location;
        event.new_location = new_location;
        event.velocity = velocity;
        event.timestamp_ms = clock_.now_ms();

        a.runtime.spawn_location = new_location;
        a.last_modified_ms = event.timestamp_ms;
    }
    bus_.emit(event);
    return true;
}

void ActorRegistry::for_each(std::function<void(const SimulationActor&)> fn) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& [_, a] : actors_) fn(*a);
}

std::vector<SimulationActor> ActorRegistry::snapshot() const {
    std::vector<SimulationActor> out;
    std::lock_guard<std::mutex> lk(mu_);
    out.reserve(actors_.size());
    for (const auto& [_, a] : actors_) out.push_back(*a);
    return out;
}

size_t ActorRegistry::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return actors_.size();
}

}}} // namespace aoc::world::simulation
