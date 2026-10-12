// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/replication/broadcast_manager.cpp
// ============================================================================
#include "world/replication/broadcast_manager.h"

namespace aoc { namespace world { namespace replication {

BroadcastManager::BroadcastManager(events::EventBus& bus,
                                    simulation::ActorRegistry& actors,
                                    VisibilityManager& visibility,
                                    IPacketEmitter& emitter)
    : bus_(bus), actors_(actors), visibility_(visibility), emitter_(emitter) {}

void BroadcastManager::install_subscriptions() {
    bus_.subscribe<events::ActorSpawned>(
        [this](const events::ActorSpawned& e) { on_spawn(e); });
    bus_.subscribe<events::ActorDestroyed>(
        [this](const events::ActorDestroyed& e) { on_destroy(e); });
    bus_.subscribe<events::PropertyChanged>(
        [this](const events::PropertyChanged& e) { on_property_changed(e); });
    bus_.subscribe<events::ActorMoved>(
        [this](const events::ActorMoved& e) { on_moved(e); });
}

void BroadcastManager::add_client(const std::string& client_key) {
    visibility_.add_client(client_key);
}

void BroadcastManager::remove_client(const std::string& client_key) {
    visibility_.remove_client(client_key);
}

// ─── Event handlers (run on emitting thread; we just queue under mu_) ──────

void BroadcastManager::on_spawn(const events::ActorSpawned& e) {
    visibility_.add_actor(e.netguid);
    // No need to mark dirty — spawn bunch carries all current properties.
}

void BroadcastManager::on_destroy(const events::ActorDestroyed& e) {
    std::lock_guard<std::mutex> lk(mu_);
    // Remember to emit destroy bunches to whoever had this actor visible.
    pending_destroys_.insert(e.netguid);
    dirty_actors_.erase(e.netguid);
    // Note: we call visibility_.remove_actor() in tick() AFTER emitting
    // destroys so we still know who saw it.
}
// C_Y_B with C.O.A

void BroadcastManager::on_property_changed(const events::PropertyChanged& e) {
    std::lock_guard<std::mutex> lk(mu_);
    uint64_t key = pack_key(e.component_index, e.handle);
    dirty_actors_[e.netguid].dirty_keys.insert(key);
}

void BroadcastManager::on_moved(const events::ActorMoved& e) {
    std::lock_guard<std::mutex> lk(mu_);
    auto& entry = dirty_actors_[e.netguid];
    entry.moved = true;
    entry.latest_location = e.new_location;
    entry.latest_velocity = e.velocity;
}

// ─── Tick — flush queues to per-client packets ─────────────────────────────

void BroadcastManager::tick() {
    // Step 1: snapshot all state we'll work with, under a single lock,
    // so the event bus can continue firing events while we emit.
    std::unordered_map<uint64_t, DirtyEntry> dirty_snapshot;
    std::unordered_set<uint64_t> destroy_snapshot;
    {
        std::lock_guard<std::mutex> lk(mu_);
        dirty_snapshot = std::move(dirty_actors_);
        dirty_actors_.clear();
        destroy_snapshot = std::move(pending_destroys_);
        pending_destroys_.clear();
    }

    // Step 2: for every known client, compute its visibility diff and emit
    //        spawn bunches for newly-visible actors.
    auto clients_at_start = [&]() {
        // Reuse visibility_.clients_seeing() trick: since no actor may exist,
        // we need the full client set.  We'll infer it from actors visible
        // to "any" — but simpler is to just iterate through the per-client
        // diff lookups.  We do that inside the actors_registry snapshot loop.
        return std::vector<std::string>{};  // placeholder; real iteration below
    };
    (void)clients_at_start;

    // A snapshot of actors gives us "look up actor state by netguid" for
    // emitting spawn bunches.
    auto actor_snapshot = actors_.snapshot();
    std::unordered_map<uint64_t, const simulation::SimulationActor*> actor_lookup;
    for (const auto& a : actor_snapshot) actor_lookup[a.netguid] = &a;

    // ── Emit spawns for newly-visible actors per client ──
    // We iterate clients through visibility's API.  Simplest way: for each
    // client that ever received a diff, drain it.  But we don't track that
    // top-down.  For MVP we iterate all known actors' clients-seeing set
    // which recovers the client list.
    std::unordered_set<std::string> seen_clients;
    for (const auto& [guid, actor_ptr] : actor_lookup) {
        for (const auto& c : visibility_.clients_seeing(guid)) {
            seen_clients.insert(c);
        }
    }

    for (const auto& client : seen_clients) {
        auto diff = visibility_.drain_diff_for_client(client);
        for (uint64_t guid : diff.newly_visible) {
            auto it = actor_lookup.find(guid);
            if (it == actor_lookup.end()) continue;  // actor gone already
            emitter_.emit_spawn(client, *it->second);
            ++total_emits_;
        }
        for (uint64_t guid : diff.newly_hidden) {
            emitter_.emit_destroy(client, guid);
            ++total_emits_;
        }
    }

    // ── Emit property deltas per (dirty actor × visible client) ──
    for (const auto& [guid, entry] : dirty_snapshot) {
        auto it = actor_lookup.find(guid);
        if (it == actor_lookup.end()) continue;  // destroyed mid-tick
        const auto& actor = *it->second;

        // Build change list from packed keys
        std::vector<ChangedProperty> changes;
        changes.reserve(entry.dirty_keys.size());
        for (uint64_t k : entry.dirty_keys) changes.push_back(unpack_key(k));

        // Fan out to each client that can see this actor
        for (const auto& client : visibility_.clients_seeing(guid)) {
            if (!changes.empty()) {
                emitter_.emit_property_delta(client, actor, changes);
                ++total_emits_;
            }
            if (entry.moved) {
                emitter_.emit_movement(client, guid,
                                        entry.latest_location,
                                        entry.latest_velocity);
                ++total_emits_;
            }
        }
    }

    // ── Emit destroy bunches then purge from visibility ──
    for (uint64_t guid : destroy_snapshot) {
        for (const auto& client : visibility_.clients_seeing(guid)) {
            emitter_.emit_destroy(client, guid);
            ++total_emits_;
        }
        visibility_.remove_actor(guid);
    }
}
// built by Cyb with C O A

// ─── Diagnostics ───────────────────────────────────────────────────────────

size_t BroadcastManager::queued_spawn_count() const {
    // Visibility manager would have to expose this; for tests we just use
    // total_emits_ and the test's mock emitter to count categorically.
    return 0;
}
size_t BroadcastManager::queued_delta_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return dirty_actors_.size();
}
size_t BroadcastManager::queued_destroy_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return pending_destroys_.size();
}
size_t BroadcastManager::total_emits() const { return total_emits_; }

}}} // namespace aoc::world::replication
