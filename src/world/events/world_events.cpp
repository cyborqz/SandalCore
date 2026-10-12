// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/events/world_events.cpp
// ============================================================================
#include "world/events/world_events.h"

namespace aoc { namespace world { namespace events {

void EventBus::emit(const ActorSpawned& e) {
    std::vector<std::function<void(const ActorSpawned&)>> listeners;
    {
        std::lock_guard<std::mutex> lk(mu_);
        listeners.reserve(on_spawn_.size());
        for (const auto& [_, cb] : on_spawn_) listeners.push_back(cb);
    }
    for (auto& cb : listeners) cb(e);
}

void EventBus::emit(const ActorDestroyed& e) {
    std::vector<std::function<void(const ActorDestroyed&)>> listeners;
    {
        std::lock_guard<std::mutex> lk(mu_);
        listeners.reserve(on_destroy_.size());
        for (const auto& [_, cb] : on_destroy_) listeners.push_back(cb);
    }
    for (auto& cb : listeners) cb(e);
}

void EventBus::emit(const PropertyChanged& e) {
    std::vector<std::function<void(const PropertyChanged&)>> listeners;
    {
        std::lock_guard<std::mutex> lk(mu_);
        listeners.reserve(on_prop_changed_.size());
        for (const auto& [_, cb] : on_prop_changed_) listeners.push_back(cb);
    }
    for (auto& cb : listeners) cb(e);
}

void EventBus::emit(const ActorMoved& e) {
    std::vector<std::function<void(const ActorMoved&)>> listeners;
    {
        std::lock_guard<std::mutex> lk(mu_);
        listeners.reserve(on_moved_.size());
        for (const auto& [_, cb] : on_moved_) listeners.push_back(cb);
    }
    for (auto& cb : listeners) cb(e);
}

size_t EventBus::listener_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return on_spawn_.size() + on_destroy_.size()
         + on_prop_changed_.size() + on_moved_.size();
}

}}} // namespace aoc::world::events
