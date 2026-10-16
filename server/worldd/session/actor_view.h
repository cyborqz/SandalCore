// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/actor_view.h  (plan E1, stage 1)
//
//  What one connection has of the world's other actors (world/actor_registry.h): a channel for each one it holds open, the shadow of what the
//  client was sent, and the states of relevancy (world/relevancy.h).  The PlayerSession gives it the player's position and the Connection every
//  tick; the view does the rest:
//
//    pass       every RelevancyConfig::tick (67 ms) the registry is asked for the actors around the player and each is put in a ring
//               (awake, snapshot, out) with the hysteresis its state allows.  What the ring changes:
//
//                 none      + awake     open the channel with the actor's whole state (a channel is the lowest free index from 3)
//                 none      + snapshot  open it, and close it with Dormancy 134 ms later: the client keeps the actor as it was
//                 dormant   + awake     open it again (a new channel, the whole state again, from an empty shadow: the retail re-open)
//                 dormant   + snapshot  nothing, unless the actor changed since the snapshot (its version moved): a new snapshot
//                 dormant   + out       nothing: the client keeps the actor it has (retail and UE do not destroy a dormant actor by distance)
//                 awake     + snapshot  an update if it changed, then the Dormancy close: the actor is still there, drawn as last sent
//                 awake     + out       the Relevancy close: the client destroys the actor
//                 awake     + awake     an update when the actor's version moved: only the changed handles (the shadow knows), on an
//                                       unreliable chain when only its position changed since the last send
//
//    removal    an actor removed from the registry (killed, despawned, a lingering effect that ended) is closed with Destroyed on the
//               channel that holds it; one the client holds dormant is opened and closed with Destroyed so that the client forgets it.
//
//  The channel of an actor is held until its close is acknowledged (Connection::channel_closing: UE's GetFreeChannelIndex), so a re-open
//  never races its own close.  The first indexes of a connection belong to the player's four actors (`reserved`).
//
//  Lost packets.  The shadow is what was last SENT; UE's NAK handling (protocol/repl/send_ledger.h) keeps it from standing in for what the
//  client HAS.  Every update chain is entered in the entry's SendLedger (what it carried: handles, fast-array items, a subobject's first block)
//  and each AckEvent of the chain goes to it (on_ack): a lost unreliable chain puts its handles and items back so that the next update sends
//  their CURRENT values (a value that changed again goes out once, the newest; one a newer chain carried, or an acknowledged one, is left
//  alone).  A reliable chain is tracked so that it takes entries from older unreliable ones (the Connection sends it again itself), and while one
//  is in flight every update goes out reliably too, behind it: a lost reliable bunch is resent later than the unreliable chains sent since and
//  would overwrite them with older values.  A
//  lost update of an actor the view has closed with Dormancy leaves the client's snapshot stale: the entry's sent_version is reset, so the
//  next pass in the snapshot ring or the awake ring opens it again from the current state.  The table is bounded (SendLedger::kMaxRecords) and
//  dropped at every open of the channel and at every close but Dormancy's (whose late events it still serves).
//
//  Until the open of a channel is acknowledged (every part of its chain) the view sends the actor's updates on reliable chains whatever the
//  change was: a client that lost the open has no channel yet and drops an unreliable bunch for it (UE does), and a reliable bunch waits
//  behind the open.
//
//  The player's own interaction state.  An interactable actor (an NPC whose UInteractableComponent has an input with an interaction definition) is
//  drawn for this connection with `Config::player_guid` in its InteractVisibleStateMap and InteractGameplayStateMap (world/interaction_state.h):
//  the open carries it, and every update is built from the same per-connection view of the actor, so it is an ordinary update of an open actor
//  (changed handles, the NAK ledger, the right channel).  The registry's actor is never touched, so another connection never sees this player's
//  guid and nothing needs taking off when the session ends (retail lists the players near the NPC; the client looks for its own).
//
//  Atomic emission.  Every chain is built on a copy of the shadow and the shadow is kept only when the Connection accepted the chain
//  (libs/aocnet/README.md "Caller contracts"); a refused open leaves the actor as it was and is tried again next pass.  Exports are marked
//  acknowledged from the Connection's ack events (on_ack), once per object, as the replicator's ExportAcks needs.
//
//  Cost.  A pass visits the actors in the 350 m ring and the ones the view tracks: with a few hundred actors that is a few microseconds each
//  (the tests measure it: test_worldd_actor_view).  Opens per pass are capped (RelevancyConfig::max_opens_per_tick), nearest first.
// ============================================================================
#pragma once
// C.Y.B with C-O-A

#include "net/connection.h"
#include "protocol/repl/replicator.h"
#include "protocol/repl/send_ledger.h"
#include "world/actor_registry.h"
#include "world/relevancy.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aoc::worldd {

class ActorView {
public:
    struct Config {
        world::RelevancyConfig relevancy;
        /// The highest channel index the view may use (UE's default is far above; retail peaked at 324 and reached index 459).
        uint32_t max_channel = 4000;
        /// The player this connection belongs to: an actor whose `owner` is another player's is never sent (a player's lingering zones).
        uint64_t player_id = 0;
        /// This player's CharacterGuid (its four words): put in the interaction state maps of every interactable actor the view draws. None: the
        /// actors are drawn as the registry has them.
        std::optional<protocol::repl::GuidValue> player_guid;
        /// Pacing.  The world's actors share the connection with the player's own four (the pawn's stat updates, the move acknowledgements, the ability
        /// updates are unreliable chains that queue behind every open already queued, and are dropped from the queue when the 256-packet window is full):
        /// while this many packets are unacknowledged or this many bunches wait for a packet, the view starts nothing new (it tries again next pass).
        size_t max_packets_in_flight = 96;
        size_t max_queued_bunches = 128;
    };

    /// What the view did, for the log and the tests.
    struct Stats {
        size_t passes = 0;
        size_t opens = 0;            // full opens (awake)
        size_t snapshots = 0;        // opens that close with Dormancy
        size_t wakes = 0;            // a dormant actor opened again, awake or as a new snapshot
        size_t updates = 0, updates_unreliable = 0;
        size_t naks = 0;             // update chains the client lost (an unreliable chain with a lost part) that put something back
        size_t nak_entries = 0;      // handles, fast-array items and subobject blocks put back by them
        size_t stale_snapshots = 0;  // dormant actors whose last update was lost: opened again at the next chance
        size_t closes_dormancy = 0, closes_relevancy = 0, closes_destroyed = 0;
        size_t forgotten = 0;        // dormant actors the view stopped tracking (the client keeps them)
        size_t refused_opens = 0;    // an open the connection could not take (window) or the replicator could not build
        size_t refused_closes = 0;
        size_t broken = 0;           // actors whose state cannot be written (never retried)
        size_t capped = 0;           // passes in which the opens cap held an actor back
        size_t no_channel = 0;
        size_t throttled = 0;        // passes in which the connection was too busy for the view to send anything
        size_t peak_channels = 0;
        size_t bits_sent = 0;        // payload bits of the chains the view sent (exports and content)
    };

    /// `reserved(index)` is true for a channel index of the session's own (the player's four actors): the view never takes one.
    ActorView(world::ActorRegistry& registry, Config config, std::function<bool(uint32_t)> reserved);
    ~ActorView();
    ActorView(const ActorView&) = delete;
    ActorView& operator=(const ActorView&) = delete;

    /// Does what is due at `now`: the Dormancy closes whose time has come, the pending destroys and, when the pass is due, a relevancy pass
    /// around (x, y). Chains that the connection cannot take now are tried again next call.
    void tick(Connection& conn, double x, double y, Clock::time_point now);

    /// The mounts milestone, stage 3: sends what changed on an actor the client holds awake now, without waiting for the next pass (the three packets of getting on a mount
    /// go out together: retail sends them within a millisecond). False when the actor is not awake on this connection, or the update could not go out (the pass sends it).
    bool update_now(Connection& conn, uint64_t actor);

    /// An AckEvent of the connection: marks the exports the client acknowledged. (The session forwards every event it takes.)
    void on_ack(const AckEvent& e);

    /// The session ends: nothing more is sent, and the view stops listening to the registry.
    void finish();

    const Stats& stats() const { return stats_; }
    /// The channel an actor is open on (0: none).
    uint32_t channel_of(uint64_t actor) const;
    world::ClientState state_of(uint64_t actor) const;
    size_t count(world::ClientState state) const;
    size_t tracked() const { return entries_.size(); }
    size_t channels_in_use() const { return channel_owner_.size(); }
    /// One line for the log.
    std::string describe() const;
    const world::RelevancyConfig& relevancy() const { return config_.relevancy; }
    /// The radius the next pass asks the registry's grid for (tests): the snapshot ring, or the largest awake radius seen among the actors that are not wide, plus the hysteresis.
    double query_radius_cm() const { return world::query_radius(config_.relevancy, largest_awake_); }
    /// Update chains whose events the view still waits for, over all actors (their ledger records), and the routes of those events: both are
    /// bounded and end at 0 once every chain's fate was reported (tests).
    size_t ledger_records() const {
        size_t n = 0;
        for (const auto& [id, e] : entries_) n += e.ledger.records();
        return n;
    }
    size_t routes() const { return chain_owner_.size(); }
    /// What the view last sent of an actor (tests): nullptr when it holds none.
    const protocol::repl::ActorShadow* shadow_of(uint64_t actor) const {
        const auto it = entries_.find(actor);
        return it == entries_.end() ? nullptr : &it->second.shadow;
    }
    /// The exports the view knows the client has (tests).
    const protocol::repl::ExportAcks& export_acks() const { return acks_; }
    /// A hook the tests use: every chain the view sends, after the connection accepted it (channel, open flag, reliable, content bits).
    void set_trace(std::function<void(uint32_t, bool, bool, size_t)> trace) { trace_ = std::move(trace); }
    /// A hook for the tests: every update chain the ledger is given (chain, actor, reliable, parts, entries carried).
    void set_chain_trace(std::function<void(ChainId, uint64_t, bool, size_t, size_t)> trace) { chain_trace_ = std::move(trace); }

private:
    struct Entry {
        world::ClientState        state = world::ClientState::None;
        uint32_t                  channel = 0;
        protocol::repl::ActorShadow shadow;
        uint32_t                  sent_version = 0;
        uint32_t                  sent_reliable = 0;
        Clock::time_point         close_due{};
        bool                      broken = false;
        protocol::repl::SendLedger ledger;   // what the update chains in flight carried (UE's NAK handling)
        /// The open's chain and the parts of it not acknowledged yet.  Until every one is, the client may not have the channel (an open that was
        /// lost), and a client drops an unreliable bunch for a channel it does not have: the updates go out reliably until then, behind the open.
        ChainId                   open_chain = 0;
        size_t                    open_parts_left = 0;
        bool                      open_acked = true;
        std::vector<ChainId>      chains;    // the chains the ledger was given and the view routes events of (chain_owner_)
    };
// [C_Y_B + C-O-A]

    /// What an AckEvent of a chain needs: the GUIDs of each export bunch.
    struct ChainExports {
        std::vector<std::vector<protocol::emit::FIntrepidNetworkGUID>> by_bunch;
        bool   reliable = true;
        size_t outstanding = 0;
    };

    /// An actor removed from the registry that the client may still hold.
    struct Removed {
        uint64_t           id = 0;
        world::ClientState state = world::ClientState::None;
        protocol::repl::Actor actor;   // a dormant one: to open and close
    };

    void     pass(Connection& conn, double x, double y, Clock::time_point now);
    /// The actor as this connection sees it: `a.actor`, or `scratch` made from it with the player's guid in the interaction state maps.
    const protocol::repl::Actor& view_of(const world::WorldActor& a, protocol::repl::Actor& scratch) const;
    /// The open of `a` on a new channel (the shadow `work` is reset first), with the exports its blocks need.
    /// `first`: the connection has never had the actor (a kept shadow is what it had), so the actor's `first_open_form` is what it is shown.
    std::optional<protocol::repl::Emission> build_open(const world::WorldActor& a, protocol::repl::ActorShadow& work, bool first = true);
    uint32_t allocate_channel(const Connection& conn) const;
    void     release(Entry& e);
    /// Sends the open of `a` on a fresh channel (`snapshot`: its Dormancy close is due after the delay). False when it could not go out now.
    bool     open(Connection& conn, world::WorldActor& a, Entry& e, bool snapshot, Clock::time_point now);
    bool     send_update(Connection& conn, world::WorldActor& a, Entry& e);
    bool     close(Connection& conn, uint64_t id, Entry& e, uint32_t reason, world::ClientState next);
    bool     send(Connection& conn, uint32_t channel, bool open, bool reliable, protocol::repl::Emission& em, ChainId* id = nullptr, size_t* parts = nullptr);
    /// Drops what the ledger and the routing of events hold for an entry: its channel closed for good or opens anew.
    void     forget_chains(Entry& e);
    void     on_removed(const world::WorldActor& a);
    void     flush_removed(Connection& conn);

    world::ActorRegistry&               registry_;
    Config                              config_;
    std::function<bool(uint32_t)>       reserved_;
    uint32_t                            listener_ = 0;
    std::unordered_map<uint64_t, Entry> entries_;
    /// Gathering stage 3: what the connection last had of the fast arrays of an actor flagged `keep_fast_shadow` (a foliage cell's state actor), by actor: kept when its channel closes,
    /// whatever the reason and even when the view forgets the actor, and handed back at its next open so that only the changes since go out (UE keeps a dormant replicator's state;
    /// the client keeps the actor and its list of cut instances).  Dropped with the actor.
    std::unordered_map<uint64_t, protocol::repl::ActorShadow> kept_;
    size_t                              foliage_opens_logged_ = 0;   // the state actors' opens of untouched cells said at info so far (the first 100)
    std::map<uint32_t, uint64_t>        channel_owner_;      // channel -> actor ObjectId, while the view holds it (open, or closed and waiting)
    std::vector<Removed>                removed_;
    protocol::repl::ExportAcks          acks_;
    std::unordered_map<ChainId, ChainExports> exports_;
    std::unordered_map<ChainId, uint64_t>     chain_owner_;   // an update chain in a ledger -> its actor
    Clock::time_point                   last_pass_{};
    bool                                passed_ = false;
    bool                                finished_ = false;
    Stats                               stats_;
    std::deque<uint64_t>                pending_snapshots_;   // opened as snapshots, their Dormancy close not sent yet, in the order they were opened
    double                              largest_awake_ = 0;   // the largest awake radius of any actor seen (the registry query must reach it)
    std::vector<uint64_t>               scratch_;
    std::function<void(uint32_t, bool, bool, size_t)> trace_;
    std::function<void(ChainId, uint64_t, bool, size_t, size_t)> chain_trace_;
};

}  // namespace aoc::worldd
