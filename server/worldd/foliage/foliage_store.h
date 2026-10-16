// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  foliage/foliage_store.h  (Gathering stage 3, part "Foliage gathering")
//
//  The shared state of the world's painted trees, plants and stones: which instance is harvested, growing or whole, and who is cutting it.  Pure logic over row numbers
//  (foliage_map.h) and seconds: it reads no clock (the caller passes `now`), sends nothing and knows no player but by id.
//
//  State.  An instance is whole until it is harvested: only the DEVIATIONS from the state its def paints it in are kept (a harvest leaves 1 in 10,000).  A harvest makes it Empty
//  (EFoliageState 3); regrowth is the data's [modelled: no capture spans one; the data names the durations and the observed IndexedStates the states]:
//
//      Empty --EmptyDuration--> Infancy --its LifecycleDuration--> Juvenile --its LifecycleDuration--> Adult      (a state the def has no record of is skipped)
//
//  and the chain ends at the def's PaintState, where the instance is whole again (an Oak: painted Adult, 300 + 1800 + 1800 s; a plant: painted Juvenile, 1800 + 300 s; a stone has only
//  Adult: Empty for its EmptyDuration, then whole).  A state's LifecycleDuration of 0 is permanent.  `regrow_mult` scales EVERY phase (the launcher's 0.01 makes an Oak 3 s empty, 18 s
//  a sapling, 18 s medium).  The seasonal factor of the data (GatheringDynamicDef) is not applied.
//
//  Changes.  `harvest` and `advance` return the instance's new state (`Change`): the cell actors show it to the clients.  `painted` says the instance is back at its painted state, which
//  the cell drops from its list.  `advance(now)` runs every transition due by `now` in time order, so a long pause brings an instance through all its phases at once (the changes are
//  listed for each phase, in order).
//
//  Locks.  An instance is cut by one player at a time: `lock` fails for a second player, and a player's lock is released when the channel ends however it ends.
// ============================================================================
#pragma once

#include <cstdint>
#include <optional>
#include <queue>
#include <unordered_map>
#include <vector>

namespace aoc::worldd::foliage {

/// EFoliageState (content::kFoliageInfancy ...).
inline constexpr int kInfancy = 0, kJuvenile = 1, kAdult = 2, kEmpty = 3;
inline constexpr const char* kStateNames[4] = {"Infancy", "Juvenile", "Adult", "Empty"};
// [Cyb ~ C.O.A]

/// What the store needs of a def: the data's GatheringFoliageData and its states.
struct DefTimes {
    int    paint = kAdult;                 // the state the map paints the instances in
    double empty_seconds = 0;              // Empty for this long after a harvest
    bool   has[3] = {false, false, false}; // the def has a GatheringFoliageState for Infancy, Juvenile, Adult
    double life[3] = {0, 0, 0};            // seconds in the state before the next one (0: permanent)
};

/// An instance's new state.
struct Change {
    uint32_t row = 0;
    int      state = kEmpty;               // the state it is in now
    bool     painted = false;              // it is back at its painted state: it is whole, and the cell drops it
    bool operator==(const Change&) const = default;
};

class FoliageStore {
public:
    struct Config {
        double regrow_mult = 1.0;          // scales every phase; > 0
    };
    /// `def_of_row[row]` is the row's index into `defs`.
    FoliageStore(std::vector<uint32_t> def_of_row, std::vector<DefTimes> defs, Config config);

    size_t rows() const { return def_of_row_.size(); }
    const DefTimes& def_of(uint32_t row) const { return defs_[def_of_row_[row]]; }
    const Config& config() const { return config_; }

    /// The instance's state now (its painted state while it deviates from nothing); kEmpty..kAdult.
    int  state_of(uint32_t row) const;
    bool deviates(uint32_t row) const { return dev_.count(row) != 0; }
    size_t deviations() const { return dev_.size(); }

    /// The instance is harvested at `now`: Empty until its def's EmptyDuration (times the multiplier) has passed.  nullopt for a row the store has not or one that is Empty already.
    std::optional<Change> harvest(uint32_t row, double now);

    /// Every transition due by `now`, in time order.
    std::vector<Change> advance(double now);
    /// When the next transition is due (nullopt: none).
    std::optional<double> next_due() const;
    /// Seconds from `now` until an instance is whole again (0 for a whole one): the sum of its remaining phases.
    double seconds_until_whole(uint32_t row, double now) const;
    /// Seconds from a harvest to the instance being whole again: every phase, scaled (the log's "back in 3,900 s").
    double regrow_seconds(uint32_t row) const;

    /// The states a def passes after Empty, in order, ending at its painted state (the model above).
    static std::vector<int> chain(const DefTimes& def);

    // ---- who is cutting what ----
    /// True when `player` holds the lock after the call (it already did, or nobody did); false when another player holds it.
    bool     lock(uint32_t row, uint64_t player);
    /// Releases the lock if `player` holds it.
    void     unlock(uint32_t row, uint64_t player);
    /// The player holding the lock (0: none).
    uint64_t locked_by(uint32_t row) const;
    size_t   locks() const { return locks_.size(); }

private:
    struct Dev {
        int    state = kEmpty;
        double due = 0;                    // when the state ends; the next state follows
    };
    struct Due {
        double   at;
        uint32_t row;
        bool operator>(const Due& o) const { return at != o.at ? at > o.at : row > o.row; }
    };
    /// The state after `state` for a def, nullopt when `state` is the end of the chain.
    std::optional<int> next_state(const DefTimes& def, int state) const;
    double             duration_of(const DefTimes& def, int state) const;   // seconds in `state`, scaled; 0 = permanent

    std::vector<uint32_t>                    def_of_row_;
    std::vector<DefTimes>                    defs_;
    Config                                   config_;
    std::unordered_map<uint32_t, Dev>        dev_;
    std::priority_queue<Due, std::vector<Due>, std::greater<Due>> due_;
    std::unordered_map<uint32_t, uint64_t>   locks_;
};

}  // namespace aoc::worldd::foliage
