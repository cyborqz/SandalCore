// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/rpc_table.h
//
//  The table of the client calls a PlayerSession answers.  PlayerSession::dispatch() works out where a call came from (RpcScope), looks its name up here and runs the
//  handler.  Each feature registers its calls in a function of its own file, PlayerSession::register_<feature>_rpcs, and lists that function once in session/rpc_registered.inc,
//  so that a new feature adds no line to dispatch() or to player_session.cpp.
//
//  A name is registered once per scope: a second registration throws std::logic_error when the table is built, which every session test reaches.  A prefix (ServerGM) is tried after
//  all the exact names.  A call that is in no table is logged once per name and ignored.
// ============================================================================
#pragma once

#include "bootstrap/bootstrap.h"
#include "net/connection.h"
#include "protocol/repl/rpc.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace aoc { namespace worldd {

class PlayerSession;

/// The places a client call can come from: the PlayerController's own calls, the pawn's own calls, the calls on the pawn's ability component and on the PlayerController's tutorial component.
enum class RpcScope : uint8_t { PlayerController, Pawn, AbilityComponent, FtueComponent };

/// What one call brings to its handler.
struct RpcContext {
    Connection&                      conn;
    bootstrap::Role                  role;
    const protocol::repl::ClientRpc& rpc;
    const InBunch&                   bunch;
    Clock::time_point                now;
};

using RpcHandler = void (*)(PlayerSession&, const RpcContext&);

class RpcTable {
public:
    /// Registers `handler` for the call `name` of `scope`.  Throws std::logic_error when that name is registered there already.
    void add(RpcScope scope, std::string_view name, RpcHandler handler) {
        for (const Entry& e : names_) {
            if (e.scope == scope && e.name == name) throw std::logic_error("the client call " + std::string(name) + " is registered twice");
        }
        names_.push_back({scope, std::string(name), handler});
    }
    /// Registers `handler` for every call of `scope` whose name starts with `prefix` and has no exact entry.
    void add_prefix(RpcScope scope, std::string_view prefix, RpcHandler handler) { prefixes_.push_back({scope, std::string(prefix), handler}); }

    /// The handler for a call, or nullptr: an exact name first, then the prefixes in the order they were registered.
    RpcHandler find(RpcScope scope, std::string_view name) const {
        for (const Entry& e : names_) {
            if (e.scope == scope && name == e.name) return e.handler;
        }
        for (const Entry& e : prefixes_) {
            if (e.scope == scope && name.substr(0, e.name.size()) == e.name) return e.handler;
        }
        return nullptr;
    }
    size_t size() const { return names_.size() + prefixes_.size(); }

private:
    struct Entry {
        RpcScope    scope;
        std::string name;
        RpcHandler  handler;
    };
    std::vector<Entry> names_, prefixes_;
};

}}  // namespace aoc::worldd
