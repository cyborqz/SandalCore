// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/session_registry.cpp
// ============================================================================
#include "net/session_registry.h"

namespace aoc { namespace net {

ClientSession* SessionRegistry::get(const std::string& client_key) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = sessions_.find(client_key);
    return it == sessions_.end() ? nullptr : it->second.get();
}

ClientSession* SessionRegistry::get_or_create(const std::string& client_key) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = sessions_.find(client_key);
    if (it != sessions_.end()) return it->second.get();

    auto cs = std::make_unique<ClientSession>();
    cs->client_key = client_key;
    cs->phase = ClientPhase::AWAITING_HANDSHAKE;
    cs->phase_entered_at = std::chrono::steady_clock::now();
    cs->last_activity    = cs->phase_entered_at;

    ClientSession* raw = cs.get();
    sessions_.emplace(client_key, std::move(cs));
    return raw;
}

bool SessionRegistry::contains(const std::string& client_key) const {
    std::lock_guard<std::mutex> lk(mu_);
    return sessions_.count(client_key) != 0;
}

void SessionRegistry::remove(const std::string& client_key) {
    std::lock_guard<std::mutex> lk(mu_);
    sessions_.erase(client_key);
}

size_t SessionRegistry::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return sessions_.size();
}
// C | O | A + C:Y:B

void SessionRegistry::for_each(std::function<void(ClientSession&)> fn) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [key, cs] : sessions_) fn(*cs);
}

void SessionRegistry::for_each_const(std::function<void(const ClientSession&)> fn) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& [key, cs] : sessions_) fn(*cs);
}

std::vector<std::string> SessionRegistry::list_keys() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> out;
    out.reserve(sessions_.size());
    for (const auto& [key, _] : sessions_) out.push_back(key);
    return out;
}

std::vector<SessionRegistry::SessionSummary> SessionRegistry::summarize() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<SessionSummary> out;
    out.reserve(sessions_.size());
    for (const auto& [key, cs] : sessions_) {
        out.push_back({key, cs->phase, cs->player_name});
    }
    return out;
}

}} // namespace aoc::net
