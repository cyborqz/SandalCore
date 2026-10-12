// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/actor_registry.cpp
//
//  See actor_registry.h.
// ============================================================================
#include "world/actor_registry.h"

#include <algorithm>
#include <cmath>

namespace aoc::worldd::world {

int32_t ActorRegistry::cell_of(double v) const {
    const double c = std::floor(v / config_.cell_cm);
    if (c < -2147483648.0) return INT32_MIN;
    if (c > 2147483647.0) return INT32_MAX;
    return static_cast<int32_t>(c);
}

void ActorRegistry::grid_add(const WorldActor& a) { grid_[cell_key(cell_of(a.x), cell_of(a.y))].push_back(a.id); }

void ActorRegistry::grid_remove(const WorldActor& a) {
    const auto it = grid_.find(cell_key(cell_of(a.x), cell_of(a.y)));
    if (it == grid_.end()) return;
    auto& v = it->second;
    v.erase(std::remove(v.begin(), v.end(), a.id), v.end());
    if (v.empty()) grid_.erase(it);
}

bool ActorRegistry::add(WorldActor actor) {
    if (actor.id == 0 || (actor.id & 1) != 0 || actor.id != actor.actor.guid.ObjectId) return false;
    if (!std::isfinite(actor.x) || !std::isfinite(actor.y) || !std::isfinite(actor.z) || !std::isfinite(actor.yaw_degrees)) return false;
    if (actors_.count(actor.id) != 0) return false;
    if (actor.npc.interact_component != 0 && by_component_.count(actor.npc.interact_component) != 0) return false;
    if (actor.npc.collision_cylinder != 0 && (by_component_.count(actor.npc.collision_cylinder) != 0 || actors_.count(actor.npc.collision_cylinder) != 0)) return false;
    const uint64_t id = actor.id;
    const auto it = actors_.emplace(id, std::move(actor)).first;
    grid_add(it->second);
    if (it->second.awake_radius_cm > kWideCm) wide_.push_back(id);
    if (it->second.npc.interact_component != 0) by_component_[it->second.npc.interact_component] = id;
    if (it->second.npc.collision_cylinder != 0) by_component_[it->second.npc.collision_cylinder] = id;
    return true;
}
// ~ C | Y | B - C.O.A ~

bool ActorRegistry::remove(uint64_t id) {
    const auto it = actors_.find(id);
    if (it == actors_.end()) return false;
    // a listener may add or remove listeners (a session ending): walk a copy
    const auto listeners = listeners_;
    for (const auto& [handle, fn] : listeners) fn(it->second);
    grid_remove(it->second);
    wide_.erase(std::remove(wide_.begin(), wide_.end(), id), wide_.end());
    if (it->second.npc.interact_component != 0) by_component_.erase(it->second.npc.interact_component);
    if (it->second.npc.collision_cylinder != 0) by_component_.erase(it->second.npc.collision_cylinder);
    actors_.erase(id);
    return true;
}

bool ActorRegistry::set_interact_component(uint64_t id, uint64_t component) {
    WorldActor* a = find(id);
    if (!a || component == 0 || by_component_.count(component) != 0) return false;
    if (a->npc.interact_component != 0) by_component_.erase(a->npc.interact_component);
    a->npc.interact_component = component;
    by_component_[component] = id;
    return true;
}

const WorldActor* ActorRegistry::find_by_component(uint64_t component) const {
    const auto it = by_component_.find(component);
    return it == by_component_.end() ? nullptr : find(it->second);
}

WorldActor* ActorRegistry::find(uint64_t id) {
    const auto it = actors_.find(id);
    return it == actors_.end() ? nullptr : &it->second;
}

const WorldActor* ActorRegistry::find(uint64_t id) const {
    const auto it = actors_.find(id);
    return it == actors_.end() ? nullptr : &it->second;
}

size_t ActorRegistry::count(ActorKind kind) const {
    size_t n = 0;
    for (const auto& [id, a] : actors_) n += a.kind == kind ? 1 : 0;
    return n;
}

void ActorRegistry::touch(WorldActor& a, Change kind) {
    ++a.version;
    if (kind != Change::Movement) ++a.reliable_version;
}

bool ActorRegistry::move(uint64_t id, double x, double y, double z, double yaw_degrees) {
    WorldActor* a = find(id);
    if (!a || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(yaw_degrees)) return false;
    const int32_t ocx = cell_of(a->x), ocy = cell_of(a->y), ncx = cell_of(x), ncy = cell_of(y);
    if (ocx != ncx || ocy != ncy) grid_remove(*a);
    a->x = x;
    a->y = y;
    a->z = z;
    a->yaw_degrees = yaw_degrees;
    if (ocx != ncx || ocy != ncy) grid_add(*a);
    touch(*a, Change::Movement);
    return true;
}

void ActorRegistry::query(double x, double y, double radius_cm, std::vector<uint64_t>& out) const {
    if (!std::isfinite(x) || !std::isfinite(y) || !(radius_cm >= 0)) return;
    const int32_t x0 = cell_of(x - radius_cm), x1 = cell_of(x + radius_cm), y0 = cell_of(y - radius_cm), y1 = cell_of(y + radius_cm);
    // a radius that spans more cells than the grid holds is cheaper walked as the whole grid
    const int64_t span = (static_cast<int64_t>(x1) - x0 + 1) * (static_cast<int64_t>(y1) - y0 + 1);
    const double r2 = radius_cm * radius_cm;
    const auto take = [&](const std::vector<uint64_t>& ids) {
        for (const uint64_t id : ids) {
            const WorldActor& a = actors_.find(id)->second;
            const double dx = a.x - x, dy = a.y - y;
            if (dx * dx + dy * dy <= r2) out.push_back(id);
        }
    };
    if (span > static_cast<int64_t>(grid_.size()) * 2) {
        for (const auto& [key, ids] : grid_) take(ids);
        return;
    }
    for (int32_t cx = x0; cx <= x1; ++cx) {
        for (int32_t cy = y0; cy <= y1; ++cy) {
            const auto it = grid_.find(cell_key(cx, cy));
            if (it != grid_.end()) take(it->second);
        }
    }
}
// CoA - C Y B

void ActorRegistry::query_wide(double x, double y, double margin_cm, std::vector<uint64_t>& out) const {
    if (!std::isfinite(x) || !std::isfinite(y)) return;
    for (const uint64_t id : wide_) {
        const WorldActor& a = actors_.find(id)->second;
        const double reach = a.awake_radius_cm + margin_cm;
        const double dx = a.x - x, dy = a.y - y;
        if (dx * dx + dy * dy <= reach * reach) out.push_back(id);
    }
}

uint32_t ActorRegistry::on_remove(Listener l) {
    const uint32_t handle = next_listener_++;
    listeners_.emplace_back(handle, std::move(l));
    return handle;
}

void ActorRegistry::off_remove(uint32_t handle) {
    listeners_.erase(std::remove_if(listeners_.begin(), listeners_.end(), [handle](const auto& p) { return p.first == handle; }), listeners_.end());
}

}  // namespace aoc::worldd::world
