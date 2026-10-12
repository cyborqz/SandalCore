// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/session_registry.h
//
//  Thread-safe owner of all active ClientSession objects, keyed by "ip:port".
//
//  The SessionRegistry is the one object that owns ClientSession lifetimes.
//  Everything else (OpcodeDispatcher, BroadcastManager, persistence) borrows
//  pointers or handles.
//
//  Concurrency model:
//    * The registry has a single mutex protecting the sessions map.
//    * Handlers lock the registry briefly to look up their session, then
//      release the map lock and operate on the session directly (the session
//      itself is not protected by the registry's lock — handlers should hold
//      the dispatcher's per-session lock for that).
//    * for_each() locks the map for the duration of the callback.  This is
//      used by BroadcastManager's tick to iterate connected clients.
//
//  LAYER:   Net / session
//  SESSION: E
// ============================================================================
#pragma once

#include "net/client_session.h"
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace aoc { namespace net {

class SessionRegistry {
public:
    SessionRegistry() = default;
    SessionRegistry(const SessionRegistry&) = delete;
    SessionRegistry& operator=(const SessionRegistry&) = delete;
// C O A with C.Y.B

    /// Look up an existing session; return nullptr if not found.
    /// NOT thread-safe against concurrent remove() on the same key.
    ClientSession* get(const std::string& client_key);

    /// Look up a session; if missing, create a new one in AWAITING_HANDSHAKE.
    /// Returns a stable pointer valid until remove(client_key).
    ClientSession* get_or_create(const std::string& client_key);

    /// True if a session exists for this key.
    bool contains(const std::string& client_key) const;

    /// Remove a session.  Safe to call concurrently with handlers on other
    /// keys, but races with handlers on this same key; caller must ensure
    /// no dispatcher thread is mid-handle for that key.
    void remove(const std::string& client_key);

    /// Number of active sessions.
    size_t size() const;

    /// Iterate all sessions holding the registry lock.  Callback must not
    /// mutate the registry (add/remove sessions) or it will deadlock.
    void for_each(std::function<void(ClientSession&)> fn);
    void for_each_const(std::function<void(const ClientSession&)> fn) const;

    /// Snapshot all client_keys currently registered.  Useful when the
    /// caller wants to operate on each session WITHOUT holding the lock.
    std::vector<std::string> list_keys() const;

    /// Snapshot all sessions into a vector of (key, phase) pairs for
    /// diagnostics / tests — no pointer leakage.
    struct SessionSummary {
        std::string client_key;
        ClientPhase phase;
        std::string player_name;
    };
    std::vector<SessionSummary> summarize() const;

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::unique_ptr<ClientSession>> sessions_;
};

}} // namespace aoc::net
