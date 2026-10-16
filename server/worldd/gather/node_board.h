// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  gather/node_board.h  (Gathering stage 2, part "Harvesting")
//
//  The state of the world's resource nodes, shared by every player and kept for the worldd run (the user's decision, spec section 1: a restart resets the nodes):
//
//      Available --begin--> Channeling(player, start, length) --complete--> Gathered(until) --time--> Available
//                                     \--cancel--> Available
//
//  A node nobody has touched is Available and has no record here.  The board owns the state machine and writes the shared actor in the registry as it moves
//  (gather/node_wire.h: bGathered, the status counter, who channels), so every connection that has the node open hears of it.  It reads no data and sends nothing; the
//  harvest itself (the checks, the rolls, the player's items and experience) is the channeling player's session's.
//
//  Time is the caller's clock (the Endpoint's, Clock::time_point).  A channel is server-timed: it completes `length` after it began whatever the client does.  A heartbeat is due
//  every `beat` (retail: one update every 100 ms, 96 to 145 of them in a channel).  A channel whose player's session never completes it (the session stalled or was dropped without finish()) is cancelled
//  `stuck_grace` after it was due, so a node is never stuck: advance() does it, and also returns a Gathered node to Available when its time has come.
//
//  Hunting (Gathering stage 3): the CORPSE of a huntable creature (an ActorKind::Npc actor) goes through the same states and the board writes it with the corpse writers of node_wire.h (a
//  heartbeat every 67 ms whose counter goes up by 2, no bGathered).  A corpse that was hunted stays Gathered until it is removed from the world, which forgets it.
//
//  The respawn time is the data's: the actor def's OverrideRespawnDuration when it is more than 0, else the foliage data's EmptyDuration (the seasonal def is ignored), times
//  `respawn_mult` (worldd --gather-respawn-mult / AOC_GATHER_RESPAWN_MULT; 1: the data's own), at least one second.
// ============================================================================
#pragma once

#include "gather/node_wire.h"
#include "net/connection.h"
#include "world/actor_registry.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace aoc::worldd::gather {

enum class NodeState : uint8_t { Available, Channeling, Gathered };

const char* state_name(NodeState s);

/// A channel in progress.
struct Channel {
    uint64_t          player_id = 0;
    Clock::time_point started{}, completes{}, next_beat{};
    uint32_t          step = 0;      // the updates sent since the start (0: only the start)
    bool              corpse = false;            // a hunted creature's corpse: its beats are Config::corpse_beat apart and its status counter goes up by 2
    uint32_t          counter = 0;               // ... the counter of the last update (a corpse's)
    Clock::time_point last_beat{};               // ... when the last update was sent (the start's time before the first heartbeat)
};
// -- CoA . C Y B --

class NodeBoard {
public:
    struct Config {
        double                    respawn_mult = 1.0;
        std::chrono::milliseconds beat{100};            // [R] 100.0 to 101.2 ms between updates in 7 of the 8 captured harvests (1427's 115 ms is the exception)
        std::chrono::milliseconds corpse_beat{67};      // [R] a hunted corpse's: 60 to 80 ms (every other game tick of 33 ms) in the five captured hunts
        std::chrono::seconds      stuck_grace{30};
    };
    struct Stats {
        uint64_t started = 0, completed = 0, cancelled = 0, beats = 0, respawned = 0, stuck = 0, gone = 0;
    };

    NodeBoard(world::ActorRegistry& registry, Config config) : registry_(registry), cfg_(config) {}

    const Config& config() const { return cfg_; }
    const Stats&  stats() const { return stats_; }

    /// The state of a node (Available for one nobody has touched or that is not held).
    NodeState state_of(uint64_t node) const;
    /// The channel of a node that is Channeling; nullptr otherwise.
    const Channel* channel_of(uint64_t node) const;
    /// When a Gathered node returns (nullopt for another state).
    std::optional<Clock::time_point> gathered_until(uint64_t node) const;
    size_t channeling() const;
    size_t gathered() const;
    /// The nodes `player` channels (at most one in practice; the board does not assume it).
    std::vector<uint64_t> channels_of(uint64_t player_id) const;

    /// Available -> Channeling for `by`, completing `length` after `now`.  False, with `why`, for a node that is not held, not Available or whose state the registry refuses.
    bool begin(uint64_t node, const Channeler& by, Clock::time_point now, std::chrono::duration<double> length, std::string& why);
    /// The next heartbeat when it is due: updates the node (one step, however late) and returns true.  False when none is due or the node is not channeling.
    bool beat(uint64_t node, Clock::time_point now);
    /// Channeling -> Gathered until `now` + `respawn`; writes the done update.  False, with `why`, for a node that is not Channeling.
    bool complete(uint64_t node, Clock::time_point now, std::chrono::duration<double> respawn, std::string& why);
    /// Channeling -> Available (no reward).  False when the node was not Channeling.  `why` is for the log.
    bool cancel(uint64_t node, const std::string& why);
    /// Cancels every channel `player_id` has (its session ends): how many.
    size_t cancel_player(uint64_t player_id, const std::string& why);
    /// Gathered nodes whose time has come become Available, and channels that are long overdue are cancelled: how many nodes changed.  Forgets nodes the registry no longer holds.
    size_t advance(Clock::time_point now);

    /// The respawn of a node of this def: `override_respawn` when it is more than 0, else `empty_duration`, times the multiplier, at least one second.
    std::chrono::duration<double> respawn_of(double override_respawn, double empty_duration) const;

private:
    struct Node {
        NodeState         state = NodeState::Available;
        Channel           channel;
        Clock::time_point until{};
    };

    world::ActorRegistry&                      registry_;
    Config                                     cfg_;
    std::unordered_map<uint64_t, Node>         nodes_;
    Stats                                      stats_;
};

}  // namespace aoc::worldd::gather
