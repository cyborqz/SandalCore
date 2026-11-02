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
//               (awake, snapshot, out) with the hysteresis its state allows (a server that ticks at a fixed rate gives the views a phase of their
//               own, session/view_phases.h, so that the players' passes spread over the ticks).  What the ring changes:
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
//    scenery    open_scenery() is the same open for the towns' replicators (ActorKind::NodeLayout) alone, and the session runs it in the join, before the phase-in;
//               the passes, which start after the phase-in, then treat those actors like any open one.
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
//  Atomic emission.  The shadow is kept only when the Connection accepted the chain (libs/aocnet/README.md "Caller contracts"): an open is built on a
//  new shadow that replaces the entry's when the chain went out, an update is collected on the entry's shadow itself and put back with the replicator's undo
//  record when the Connection refuses it (no copy of the shadow per update).  A refused open or update leaves the actor as it was and is tried again next
//  pass.  Exports are marked acknowledged from the Connection's ack events (on_ack), once per object, as the replicator's ExportAcks needs.
//
//  Cost.  A pass visits the actors in the 350 m ring and the ones the view tracks: with a few hundred actors that is a few microseconds each
//  (the tests measure it: test_worldd_actor_view).  Opens per pass are capped (RelevancyConfig::max_opens_per_tick), nearest first.
// ============================================================================
#pragma once
// C.Y.B with C-O-A

#include "net/connection.h"
#include "protocol/repl/replicator.h"
#include "protocol/repl/send_ledger.h"
#include "session/shared_updates.h"
#include "session/view_phases.h"
#include "world/actor_registry.h"
#include "world/relevancy.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
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
        /// The tick of the cycle each view passes on, shared by the views of one server (session/view_phases.h): the server gives it when its tick passes every n-th tick, so that the
        /// players' passes do not all land on one tick.  None: a pass is due once the relevancy config's tick has gone by since the last (the tests, and the old loop).
        std::shared_ptr<ViewPhases> phases;
        /// The update built once for the views that hold an actor in the same state (session/shared_updates.h), shared by the views of one server.  None: every view builds its own.  The test
        /// programs get the sharing AOC_TEST_SHARE_UPDATES names (verify without it); worldd sets this from `--share-updates`.
        std::shared_ptr<SharedUpdates> shared = SharedUpdates::from_test_environment();
    };

    /// What the view did, for the log and the tests.
    struct Stats {
        size_t passes = 0;
        size_t opens = 0;            // full opens (awake)
        size_t snapshots = 0;        // opens that close with Dormancy
        size_t wakes = 0;            // a dormant actor opened again, awake or as a new snapshot
        size_t updates = 0, updates_unreliable = 0;
        size_t shared_updates = 0;   // of them, the ones sent from blocks another view built (Config::shared)
        size_t naks = 0;             // update chains the client lost (an unreliable chain with a lost part) that put something back
        size_t nak_entries = 0;      // handles, fast-array items and subobject blocks put back by them
        size_t stale_snapshots = 0;  // dormant actors whose last update was lost: opened again at the next chance
        size_t closes_dormancy = 0, closes_relevancy = 0, closes_destroyed = 0;
        size_t forgotten = 0;        // dormant actors the view stopped tracking (the client keeps them)
        size_t refused_opens = 0;    // an open the connection could not take (window) or the replicator could not build
        size_t refused_updates = 0;  // an update chain the connection could not take (tried again at the next pass)
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

    /// The same tick in two halves, for a server that runs the players' passes on several threads (net/endpoint.h).  `plan` is the serial half: it decides whether a relevancy pass is due now and,
    /// if the view has a phase (ViewPhases, shared by the views), takes or moves it, so it is never called by two threads at once.  `run` is the half that can run on any thread, alone for its
    /// view: the due Dormancy closes, then the pass when `pass_is_due`.  `plan` then `run` do what `tick` does.
    bool plan(Clock::time_point now);
    void run(Connection& conn, double x, double y, Clock::time_point now, bool pass_is_due);

    /// Opens, now, the towns' scenery around (x, y) and nothing else (the login fix).  Every ActorKind::NodeLayout actor (a town's replicator, awake within
    /// 1.45 km) whose radius holds the point and that the view does not hold yet is opened awake, nearest first, on the first free channel, by the pass's own rules for it (the opens cap,
    /// the pacing of a busy connection).  The NPCs and every other actor are left to tick(), which the session runs after the phase-in; when it does, it finds the replicators open and
    /// sends them only what changes.  The reason: the client's pawn phases in when a trace down finds ground, and in a town's bowl the only ground the client has is what the replicator
    /// builds, so the replicator is opened in the join.
    /// True when nothing is left to open (none within reach, or all open); false when the connection was too busy or refused an open: call again.  `opened` gets the count of this call.
    bool open_scenery(Connection& conn, double x, double y, Clock::time_point now, size_t* opened = nullptr);

    /// The mounts milestone, stage 3: sends what changed on an actor the client holds awake now, without waiting for the next pass (the three packets of getting on a mount
    /// go out together: retail sends them within a millisecond). False when the actor is not awake on this connection, or the update could not go out (the pass sends it).
    bool update_now(Connection& conn, uint64_t actor);

    /// An AckEvent of the connection: marks the exports the client acknowledged. (The session forwards every event it takes.)
    void on_ack(const AckEvent& e);

    /// The session ends: nothing more is sent, and the view stops listening to the registry.
    void finish();

    const Stats& stats() const { return stats_; }
    /// The phase this view passes on (Config::phases; 0 until its first tick, and when it has none).
    unsigned phase() const { return phase_; }
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
    /// An Entry's place in the list of the scan that visited it last: none.
    static constexpr uint32_t kNoRank = ~0u;

    struct Entry {
        world::ClientState        state = world::ClientState::None;
        uint32_t                  channel = 0;
        uint32_t                  scan_stamp = 0;        // the scan that last listed this actor (scan_gen_)
        uint32_t                  scan_rank = kNoRank;   // its place in the list of the pass that visited it last: the next scan starts from that order
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
        /// Shared updates (shared_updates.h): what the shadow lacks to stand where the other views' shadows of the same version do.
        uint32_t                  shape_epoch = 0;        // the actor's shape epoch when this shadow was started by the open
        bool                      private_form = false;   // the shadow holds the per-connection form of an interactable actor, not the registry's
        bool                      tainted = false;        // the ledger put a lost chain's values back after the last update (it dropped the oldest chain as a NAK): the next update sends them
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
    /// The actors a pass looks at (what the registry finds round (x, y), the wide ones and every actor the view holds), each once, with its ring and its distance, nearest first
    /// (a tie by id), in cands_.  The order of the last pass is its starting point (see actor_view.cpp).
    void     scan(double x, double y);
    /// tick() without its timing (--loop-stats).
    void     tick_unprofiled(Connection& conn, double x, double y, Clock::time_point now);
    /// The pending destroys and the Dormancy closes whose time has come.
    void     run_due_closes(Connection& conn, Clock::time_point now);
    /// Whether the relevancy pass is due at `now` (and, if it is, counts it and takes the phase).
    bool     pass_due(Clock::time_point now);
    /// The actor as this connection sees it: `a.actor`, or `scratch` made from it with the player's guid in the interaction state maps.
    const protocol::repl::Actor& view_of(const world::WorldActor& a, protocol::repl::Actor& scratch) const;
    /// The open of `a` on a new channel (the shadow `work` is reset first), with the exports its blocks need.
    /// `first`: the connection has never had the actor (a kept shadow is what it had), so the actor's `first_open_form` is what it is shown.
    /// `private_form` (when given) is set to whether the open shows this connection a form of the actor of its own (the interaction state, the first-open form), not the registry's.
    std::optional<protocol::repl::Emission> build_open(const world::WorldActor& a, protocol::repl::ActorShadow& work, bool first = true, bool* private_form = nullptr);
    uint32_t allocate_channel(const Connection& conn) const;
    void     release(Entry& e);
    /// Sends the open of `a` on a fresh channel (`snapshot`: its Dormancy close is due after the delay). False when it could not go out now.
    bool     open(Connection& conn, const world::WorldActor& a, Entry& e, bool snapshot, Clock::time_point now);
    bool     send_update(Connection& conn, const world::WorldActor& a, Entry& e);
    /// How send_update's update was made (below).
    struct Built {
        std::optional<protocol::repl::Emission> em;      // none: nothing changed (or `failed`)
        bool                                    failed = false;
        bool                                    private_form = false;   // made from this connection's own form of the actor: the shadow then holds that form
        protocol::repl::SentRecord              record;                  // what the chain carries, when it was made here
        const protocol::repl::SentRecord*       shared_record = nullptr; // ... or the shared update's
        protocol::repl::ShadowUndo              undo;                    // the shadow was changed: put back when the connection refuses the chain
        const protocol::repl::ShadowRedo*       redo = nullptr;          // the shadow was not: given the update's outcome once the connection takes the chain
    };
    /// The update of `a` for this connection: made from the shadow here, or, with sharing (shared_updates.h) and a shadow that stands where another view's did, from the blocks that view
    /// built.
    void     build_update(const world::WorldActor& a, Entry& e, Built& built);
    void     build_update_shared(SharedUpdates& sharing, const world::WorldActor& a, Entry& e, Built& built);
    /// Verify mode: the shared update `hit` against the plain build of the same update on a copy of the shadow; true when they are the same bytes, the same record and the same shadow after.
    bool     shared_update_holds(const world::WorldActor& a, const Entry& e, const UpdateMemo::Variant& hit);
    bool     close(Connection& conn, uint64_t id, Entry& e, uint32_t reason, world::ClientState next);
    bool     send(Connection& conn, uint32_t channel, bool open, bool reliable, protocol::repl::Emission& em, ChainId* id = nullptr, size_t* parts = nullptr);
    /// Drops what the ledger and the routing of events hold for an entry: its channel closed for good or opens anew.
    void     forget_chains(Entry& e);
    void     on_removed(const world::WorldActor& a);
    void     flush_removed(Connection& conn);

    /// The registry as the view reads it: a pass changes nothing in it (it is frozen while the passes run on several threads, world/actor_registry.h).
    const world::ActorRegistry& reg() const { return registry_; }
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
    unsigned                            phase_ = 0;               // the tick of the cycle this view passes on (Config::phases)
    bool                                phase_taken_ = false;     // ... and whether it holds one
    bool                                finished_ = false;
    Stats                               stats_;
    std::deque<uint64_t>                pending_snapshots_;   // opened as snapshots, their Dormancy close not sent yet, in the order they were opened
    double                              largest_awake_ = 0;   // the largest awake radius of any actor seen (the registry query must reach it)
    std::vector<uint64_t>               scratch_;
    /// One actor of a scan: its distance from the player, its ring, and the place it had in the list of the pass that visited it last (kNoRank: the view did not hold it).
    struct Cand {
        double      d;
        uint64_t    id;
        world::Ring ring;
        uint32_t    rank;
    };
    std::vector<Cand>                   cands_, sorted_, merged_;   // the scan's list, and the two buffers that its ordering works in
    std::vector<uint32_t>               place_;                     // by rank: the index in cands_ of the actor that had it
    uint32_t                            scan_gen_ = 0;              // counts the scans: an Entry's scan_stamp names the one that listed it
    size_t                              scan_listed_ = 0;           // the length of the last scan's list (the ranks run to it)
    std::function<void(uint32_t, bool, bool, size_t)> trace_;
    std::function<void(ChainId, uint64_t, bool, size_t, size_t)> chain_trace_;
};

}  // namespace aoc::worldd
