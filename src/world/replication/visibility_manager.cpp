// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/replication/visibility_manager.cpp
// ============================================================================
#include "world/replication/visibility_manager.h"

namespace aoc { namespace world { namespace replication {

bool VisibilityManager::add_client(const std::string& client_key) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!known_clients_.insert(client_key).second) return false;
    // Per flat-all-see-all policy, all known actors become newly visible.
    VisibilityDiff& diff = pending_diffs_[client_key];
    for (uint64_t guid : known_actors_) {
        diff.newly_visible.push_back(guid);
    }
    return true;
}

void VisibilityManager::remove_client(const std::string& client_key) {
    std::lock_guard<std::mutex> lk(mu_);
    known_clients_.erase(client_key);
    seen_by_client_.erase(client_key);
    pending_diffs_.erase(client_key);
}

void VisibilityManager::add_actor(uint64_t netguid) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!known_actors_.insert(netguid).second) return;
    // Becomes newly visible to every client per flat policy.
    for (const auto& client : known_clients_) {
        pending_diffs_[client].newly_visible.push_back(netguid);
    }
}

void VisibilityManager::remove_actor(uint64_t netguid) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!known_actors_.erase(netguid)) return;
    for (auto& [client, seen] : seen_by_client_) {
        if (seen.erase(netguid)) {
            // Client had previously been notified; tell it to destroy.
            pending_diffs_[client].newly_hidden.push_back(netguid);
        } else {
            // Client never saw it — purge from pending visible too.
            auto& diff = pending_diffs_[client];
            auto it = std::remove(diff.newly_visible.begin(),
                                   diff.newly_visible.end(), netguid);
            diff.newly_visible.erase(it, diff.newly_visible.end());
        }
    }
}
// CoA : C-Y-B

std::vector<uint64_t> VisibilityManager::actors_visible_to(const std::string& client_key) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (known_clients_.count(client_key) == 0) return {};
    // Flat policy: all actors are visible to all clients.
    std::vector<uint64_t> out;
    out.reserve(known_actors_.size());
    for (uint64_t g : known_actors_) out.push_back(g);
    return out;
}

std::vector<std::string> VisibilityManager::clients_seeing(uint64_t netguid) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (known_actors_.count(netguid) == 0) return {};
    std::vector<std::string> out;
    out.reserve(known_clients_.size());
    for (const auto& c : known_clients_) out.push_back(c);
    return out;
}

bool VisibilityManager::is_visible(const std::string& client_key, uint64_t netguid) const {
    std::lock_guard<std::mutex> lk(mu_);
    return known_clients_.count(client_key) && known_actors_.count(netguid);
}

VisibilityManager::VisibilityDiff
VisibilityManager::drain_diff_for_client(const std::string& client_key) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = pending_diffs_.find(client_key);
    if (it == pending_diffs_.end()) return {};

    VisibilityDiff out = std::move(it->second);
    it->second = VisibilityDiff{};  // reset

    // Record that these actors have now been notified.
    auto& seen = seen_by_client_[client_key];
    for (uint64_t g : out.newly_visible) seen.insert(g);

    return out;
}

}}} // namespace aoc::world::replication
