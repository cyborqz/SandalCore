// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/replication/visibility_manager.h
//
//  Per-client visibility tracking.  For MVP this is "flat everyone-sees-
//  everyone"; post-MVP gets proximity-based filtering that mirrors AoC's
//  UFilteredActorTrackingRegistry pattern.
//
//  The manager answers: "should client X receive updates about actor Y?"
//
//  LAYER:  World / replication
//  SESSION: D'
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <mutex>
#include <vector>

namespace aoc { namespace world { namespace replication {

class VisibilityManager {
public:
    /// Register a client.  Returns true if newly added.
    bool add_client(const std::string& client_key);

    /// Unregister a client.  All actors become "newly hidden" from its perspective.
    void remove_client(const std::string& client_key);

    /// Register an actor.  In MVP policy ("all-sees-all") it immediately
    /// becomes visible to all known clients.
    void add_actor(uint64_t netguid);

    /// Unregister an actor.  Becomes "newly hidden" for all clients that
    /// saw it.
    void remove_actor(uint64_t netguid);
// ~ C_Y_B & CoA ~

    /// Per-client: which actors are currently visible to this client?
    std::vector<uint64_t> actors_visible_to(const std::string& client_key) const;

    /// Per-actor: which clients currently see this actor?
    std::vector<std::string> clients_seeing(uint64_t netguid) const;

    /// Query: is this actor visible to this client?
    bool is_visible(const std::string& client_key, uint64_t netguid) const;

    /// Per-tick diff: for each client, what became visible / hidden since
    /// the last sync_all_for_client() call?  Clears the diff after reading.
    struct VisibilityDiff {
        std::vector<uint64_t> newly_visible;
        std::vector<uint64_t> newly_hidden;
    };
    VisibilityDiff drain_diff_for_client(const std::string& client_key);

private:
    mutable std::mutex mu_;

    // Policy is flat-all-see-all for now.  Diff tracking means we don't
    // emit spawn bunches for already-visible actors on every tick.
    std::unordered_set<std::string> known_clients_;
    std::unordered_set<uint64_t>    known_actors_;

    // Per-client: actors already seen (so spawn only fires once per actor-client pair)
    std::unordered_map<std::string, std::unordered_set<uint64_t>> seen_by_client_;

    // Pending diffs (accumulated by add_actor / remove_actor / add_client)
    std::unordered_map<std::string, VisibilityDiff> pending_diffs_;
};

}}} // namespace aoc::world::replication
