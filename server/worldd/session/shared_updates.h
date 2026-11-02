// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/shared_updates.h
//
//  One update, built once, sent to every player who holds the actor in the same state.
//
//  The views of the players around one place hold the same mobs, and when a mob changes, every view builds the same change for its own connection: it diffs the mob's
//  values against the shadow of what that connection was sent and writes the handles that differ.  That diff is the dearer part of an update (ActorView::send_update), and it does not
//  depend on who looks.  What does depend on the connection is small: the exports its client has not acknowledged, the choice of a reliable or an unreliable chain, the channel, the NAK
//  ledger.  So the first view to build the update for an actor's state keeps what it built (an UpdateMemo, on the actor), and a view whose shadow stands where that one's stood takes the
//  blocks as they are, assembles its own exports around them and gives its shadow what the update wrote (repl::ShadowRedo) instead of diffing the actor against it again.
//
//  When two shadows stand in the same place.  Every condition is checked for every update; any doubt builds the update the plain way, for that view alone:
//    - the actor is in the state the update was built for (its version, and its shape epoch: world/actor_registry.h);
//    - the view's shadow is at the version the first one's was at (Entry::sent_version) and a clean copy of the actor as it stood then: no lost chain's values were put back into it
//      (the entry would be at version 0, or `tainted`), it holds no per-connection form of an interactable actor (`private_form`), and it was started under the actor's present shape epoch
//      (no value or subobject was taken away since, and no array lost an element value it had: a shadow keeps an entry for a handle that was sent once, and for an element value that was
//      sent as a zero, and compares a value set again against it, where a shadow started later has no entry and sends it as new);
//    - the actor has no first-open form and keeps no fast-array shadow between opens (a foliage cell's state actor), and is not interactable for this player (an interactable actor is
//      drawn with the player's own guid in its interaction maps);
//    - the shadow holds no property back from its connection (every ParentMask is all-active: ActorShadow::all_active).
//  Under these a shadow holds, for every value the actor has, the value the last update to that version left, and the same entries as any other: the next update is the same blocks.
//  (The property test in tests/actor_view_shadow_redo_tests.inc runs random changes against two shadows, one started by an open and one by updates, and compares what each would send.)
//
//  The option is `--share-updates off|on|verify` (worldd); `verify` also builds one shared update in N the plain way on a copy of the shadow and compares the bytes and the shadow after;
//  on a difference it says which actor and turns sharing off for the rest of the run.  Off is today's behaviour exactly.
// ============================================================================
#pragma once
// C.O.A ~ C-Y-B

#include "protocol/repl/replicator.h"
#include "session/view_worker.h"
#include "world/actor_registry.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace aoc::worldd {

namespace repl = ::aoc::protocol::repl;

enum class ShareMode : uint8_t { Off, On, Verify };

/// The update built for one state of one actor, for the views whose shadows stood at the version it was built from.  It lives on the actor (WorldActor::update_memo), is replaced the first time
/// a view builds an update for a newer state, and is freed with the actor.
struct UpdateMemo {
    /// What a view that built the update for this state from a shadow at `from_version` keeps.
    struct Variant {
        uint32_t                        from_version = 0;
        std::vector<repl::ContentBlock> blocks;   // empty: the version moved and no value changed (nothing is sent)
        repl::SentRecord                record;   // what the update carried, for a view's NAK ledger
        repl::ShadowRedo                redo;     // what it wrote into the shadow
    };
    static constexpr size_t kMaxVariants = 4;

    uint32_t             version = 0;       // the actor's version the variants were built for
    uint32_t             shape_epoch = 0;   // ... and its shape epoch
    std::vector<Variant> variants;
};

class SharedUpdates {
public:
    struct Stats {
        uint64_t hits = 0;            // updates sent from the blocks another view built
        uint64_t builds = 0;          // updates built for sharing (kept on the actor)
        uint64_t private_builds = 0;  // updates built for one view only because sharing could not hold for it (a lost chain's values put back, an interactable actor ...)
        uint64_t verified = 0;        // shared updates checked against a plain build (verify)
        uint64_t mismatches = 0;      // ... that differed
    };

    /// `verify_every`: in verify mode one shared update in this many is checked.
    explicit SharedUpdates(ShareMode mode, unsigned verify_every = 8) : mode_(mode), verify_every_(verify_every == 0 ? 1 : verify_every) {}

    ShareMode mode() const { return mode_.load(std::memory_order_relaxed); }
    bool      active() const { return mode() != ShareMode::Off; }
    /// The counts of all the threads that run view passes, added up (read it when no pass runs).
    Stats stats() const;
    /// The calling thread's own counts: the figures are written without a lock, one set for each thread (view_worker()).
    Stats& mine() { return local_[view_worker()].stats; }

    /// "off", "on" or "verify"; false (and Off) for another text.
    static ShareMode parse(std::string_view text, bool* ok = nullptr);
    static const char* name(ShareMode mode);

    /// The sharing the test programs use, so that the existing suites run the way worldd does: the instance of the mode AOC_TEST_SHARE_UPDATES names (`on`, `off` or `verify`), and with
    /// no such variable `verify`: sharing on, and every shared update also built the plain way and compared.  `off` gives null (every view builds its own).  A test program that ends
    /// with a shared update that differed from the plain build prints a `[FAIL]` line.  worldd itself never reads it: main.cpp sets the views' sharing from `--share-updates`.
    static std::shared_ptr<SharedUpdates> from_test_environment();

    /// The update kept for the actor's present state that a view with a shadow at `from_version` can take, or null.
    const UpdateMemo::Variant* find(const world::WorldActor& actor, uint32_t from_version) const;
    /// Keeps the update a view built (from a shadow at `from_version`) for the actor's present state; returns it.
    const UpdateMemo::Variant& publish(const world::WorldActor& actor, uint32_t from_version, std::vector<repl::ContentBlock>&& blocks, repl::SentRecord&& record, repl::ShadowRedo&& redo);

    /// In verify mode: true for one call in `verify_every`.
    bool verify_due() { return mode() == ShareMode::Verify && ++local_[view_worker()].verify_calls % verify_every_ == 0; }
    /// A shared update was not what the plain build gave (or the actor's shape changed unseen): says so, counts it and turns sharing off.
    void mismatch(uint64_t actor, const char* what);

    /// For the tests: the next update published is damaged on purpose (the verify mode must catch it).
    enum class Fault : uint8_t { None, DropLastBlock, SkewRedo };
    void fault_for_test(Fault f) { fault_ = f; }

private:
    std::atomic<ShareMode> mode_;
    unsigned  verify_every_;
    struct alignas(64) Local {
        Stats    stats;
        uint64_t verify_calls = 0;
    };
    std::array<Local, kViewWorkers> local_;
    Fault     fault_ = Fault::None;
};

}  // namespace aoc::worldd
