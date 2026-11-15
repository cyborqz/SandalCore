// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  fishing/cast.h  (Fishing, milestone 1)
//
//  The timeline of one cast of Economy_FishingCast, as pure logic on a clock of seconds: no sockets, no session, no inventory.  The client sends ONE ServerTriggerAbility; everything after
//  it is the server's, and the server's loop is what the data and the retail recordings show:
//
//    reply       the ability item (the montage from its start), the Cooldowns item (1 s) and the Fishing_Buff effect, about 100 ms after the trigger [R: 91 to 104 ms]
//    lure        the montage's AoCFishingStart notify, 1.673 s into the montage (the montage asset): the lure lands.  On water the gathering loop starts; anywhere else the cast fails
//                (the branch Economy_Fishing_Cast_Fail: HasAbilityGatheringFailed) and the montage jumps to its CastFail section
//    loop        the interaction's own channel length (BaseGatheringDuration x the speed formula of the player's ratings: 15 s at rating 0), then, every tick, the branch
//                Economy_Fishing_LoopComplete: `RandIntRange(1, 100) == 1 && AbilityInstanceGatheringLoopsComplete(...)`: a chance of one in a hundred each tick, so the wait after the channel
//                is a geometric number of ticks [R: the waits of 46 cycles: at least the channel, mean excess 7.4 s]; the tick is modelled at 15 a second
//    bite        the montage jumps to its LoopEnd section (the fish takes the lure; the item changes, bBranchPredictedOnAutonomous 0, MayJumpToPosition 1)
//    result      4.37 s after the bite [R: 4.31 to 4.48 s, mean 4.37] the rewards are rolled and given: the tool wears whatever the roll gave (a catch or nothing), the client gets the
//                storage event, the popup and the experience.  The next loop starts at once
//    cancel      the branches Economy_Fishing_Cancel (`IsMoving || IsInCombat`) and Economy_Fishing_Durability_Cancel (the rod's durability is gone) are tested every tick from 0.11 s into the
//                montage [the montage's conditional-branch notify]: the montage jumps to AbilityEnd, the buff goes about 100 ms later and the montage is cleared 2.0 s after the jump [R]
//    fail        the montage jumps to CastFail; the buff goes with the montage's effect notify (2.35 s later) and the montage is cleared 4.3 s after the jump [the montage's sections: CastFail
//                21.5 to 23.8, AbilityEnd 23.805 to its end; the effect notify ends at 23.754; modelled with the retail cancel's delays]
//
//  The wire is the session's (session/player_fishing.cpp).  This class only says WHICH step falls due WHEN; a step is a plain value.  The random draw of a loop's wait comes from the caller's
//  generator (the world's, so a test can force it).
// ============================================================================
#pragma once

#include "stats/stat_engine.h"

#include <cstdint>
#include <vector>

namespace aoc::worldd::fishing {

inline constexpr int64_t kGuidFishingCast = 6064630882622308352ll;   // Economy_FishingCast
inline constexpr int64_t kGuidFishingBuff = 6064632082445762560ll;   // Fishing_Buff

/// Every time of a cast, in seconds, with its source.
struct Timing {
    double reply = 0.100;                 // after the trigger: the reply's item, cooldown and buff [R: 91 to 104 ms]
    double cooldown = 1.0;                // the ability's Cooldown
    double first_check = 0.110;           // into the montage: where the conditional-branch notify state begins (the montage asset)
    double lure = 1.673;                  // into the montage: AoCFishingStart (the montage asset)
    double tick = 1.0 / 15.0;             // the branch is tested every tick [modelled from the waits of 46 cycles]
    int    bite_one_in = 100;             // RandIntRange(1, 100) == 1 (the branch's own text)
    double bite_to_result = 4.37;         // [R: 46 cycles, 4.31 to 4.48 s]
    double buff_off_after_end = 0.098;    // after a cancel's jump [R: 66 and 130 ms in two recordings]
    double clear_after_end = 2.002;       // [R: 1.968 and 2.036 s]
    double fail_buff_off = 2.352;         // [the montage: the effect notify ends at 23.754 and the jump lands on 21.5, plus the buff delay above]
    double fail_clear = 4.305;            // [the montage: AbilityEnd begins at 23.805, so the clear comes 2.002 s after it, less the jump's 21.5]
    // where the montage jumps to (its sections, in seconds into the montage)
    float  loop_end_position = 11.166666f;
    float  fail_position = 21.5f;
    float  end_position = 23.805052f;
};

enum class Step : uint8_t {
    Reply,         // the ability item, the cooldown and the buff
    CooldownEnd,   // the Cooldowns item goes
    Bite,          // the montage jumps to LoopEnd
    Result,        // the rewards: the session rolls and delivers
    CastFail,      // the montage jumps to CastFail
    Cancel,        // the montage jumps to AbilityEnd
    BuffOff,       // the Fishing_Buff goes
    Clear,         // the montage is cleared: the cast is over
};
const char* step_name(Step s);

struct Event {
    Step   step = Step::Reply;
    double at = 0;
};

/// Why a cast's loop ends before its time; the session's checks.
enum class Stop : uint8_t { None, Moving, Combat, Tool, Dead, Mounted, Replaced, Ended };
const char* stop_name(Stop s);

class Cast {
public:
    /// `trigger_at`: the clock's seconds when the client's call came.  `lands`: the lure falls on water.  `loop_seconds`: the channel's length for this player.
    Cast(double trigger_at, bool lands, double loop_seconds, Timing timing = {});

    /// The steps that fell due up to `now`, oldest first.  A bite draws the wait of its loop from `rng`.
    std::vector<Event> advance(double now, stats::SplitMix& rng);

    /// The loop's end by a branch (movement, combat, the rod gone): possible from the first check on, while the loop runs or a catch is being reeled in (the fish is lost: no result).
    /// Queues the Cancel, BuffOff and Clear steps (the next advance returns them).  False when it cannot be taken: before the first check, or when the cast is ending already.
    bool cancel(double now, Stop why);

    /// The cast ends at once with no branch: another cast replaces it, the session ends.  Drops everything that is queued.
    void drop();

    bool   active() const { return phase_ != Phase::Done; }
    bool   ending() const { return phase_ == Phase::Ending || phase_ == Phase::Done; }
    bool   lands() const { return lands_; }
    bool   buff_on() const { return buff_on_; }
    bool   cooling() const { return cooling_; }
    bool   reeling() const { return phase_ == Phase::Reeling; }
    Stop   stopped_by() const { return stop_; }
    /// The next queued step falls due at or before `now` and is a Result: the session checks the room in its reply queue before it calls advance().
    bool   result_due(double now) const;
    /// When the next step falls due, or +infinity.
    double next_due() const;
    double loop_seconds() const { return loop_seconds_; }
    double trigger_at() const { return trigger_at_; }
    /// The loops finished (results given) and the bites so far.
    int    results() const { return results_; }
    int    bites() const { return bites_; }
    /// The first moment the branches are tested.
    double first_check_at() const { return first_check_; }
    const Timing& timing() const { return t_; }

private:
    enum class Phase : uint8_t { Start, Waiting, Reeling, Ending, Done };
    void   queue(Step s, double at);
    void   remove(Step s);
    void   begin_loop(double from, stats::SplitMix& rng);

    Timing              t_;
    double              trigger_at_ = 0, loop_seconds_ = 0, first_check_ = 0;
    bool                lands_ = false, buff_on_ = false, cooling_ = false;
    Phase               phase_ = Phase::Start;
    Stop                stop_ = Stop::None;
    int                 results_ = 0, bites_ = 0;
    std::vector<Event>  queue_;   // sorted by time, oldest first
};

}  // namespace aoc::worldd::fishing
