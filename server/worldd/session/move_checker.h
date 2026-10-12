// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/move_checker.h
//
//  The plausibility checks of plan A6 (spec section 10), as pure logic: given the last
//  good position of a player and a move the client reports (protocol/repl/move_packed.h),
//  say whether the move is plausible or the client must be put back.  worldd does not
//  simulate movement: it trusts the client's position as long as that position is one the
//  character could have reached, and corrects it when not.
//
//  A move is plausible when, against the last good position:
//    every number is finite (TimeStamp, Location, Acceleration)                  NotFinite
//    the location is inside the world box (generous, around Verra)               OutOfWorld
//    the TimeStamp rises (the client's clock reset every 240 s is allowed)       TimeStampNotRising
//    the horizontal distance <= max_speed * dt + credit                          TooFast
//    the upward step <= max_rise_speed * dt + credit                             RisingTooFast
//    the downward step <= max_fall_speed * dt + credit                           FallingTooFast
//  where dt is the TimeStamp difference, at most max_time_step, at most what the server's own
//  clock has earned (below) and, for the first move, a first_move_seconds allowance from the
//  spawn point.  The TimeStamp is the client's own clock, so it is paced against the server's:
//  a bank of seconds earns pace_rate (1.25) per second of the server's clock, holds at most
//  pace_bank (5) and is spent by the dt of every accepted move; a move never gets more dt than
//  the bank holds.  A client that races its TimeStamp (a big step per move, or dozens of moves
//  in one datagram) gets the bank's few seconds once and then 1.25 x the speed limit over real
//  time, however many moves it sends; one that plays at its own pace (the captures, bursts
//  after packet loss, a clock 15% fast) never notices the bank.  The credit is the slack, once: each
//  direction starts with `slack` cm in hand and gains the allowance it does not use (up to
//  `slack` again), so a ledge or a bit of drift is absorbed, but a client cannot add `slack`
//  to every one of its sixty moves a second: over time the speed limit is max_speed.  (The upward
//  credit is the larger max_rise_burst: a climb onto a ledge lifts the character in a few big steps.)
//  An accepted move becomes the last
//  good position (location, yaw of the control rotation, movement mode, TimeStamp); a
//  corrected one changes nothing, so the player is put back to the last good position and
//  the next move is measured against it.
//
//  The numbers (kDefaults below), each with its evidence; "captures" are the retail
//  client's 2,483 ServerMovePacked moves (libs/aocnet/tests/capture_moves.h):
//    max_speed 2,135 cm/s     1.5 x 1,423 cm/s, the fastest step between consecutive moves in both
//                             captures (the dodge; sprint ~962, walking 600.1 cm/s).  The pawn's
//                             MaxWalkSpeed is not in the FModel export (it has no .uexp: no property
//                             values), so the spec's fallback applies: the fastest measured speed x 1.5.
//    slack 50 cm              a burst budget (the credit): the quantization (0.01 cm), a step up a 45 cm
//                             ledge (UE's MaxStepHeight) or a frame's drift are absorbed once, then the
//                             allowance has to earn it back
//    dash_speed 3,355 cm/s    1.5 x 2,237 cm/s, the Rogue's Dodge dash (retail capture 742: 14 moves at 2,237 over 0.23 s; the
//                             other 30 retail Dodges peak at 1,408-1,688), allowed instead of max_speed while a Dodge is on
//                             (check()'s `dash`: the Evasion and a margin, from the ability runtime).  The Rogue's dash is the
//                             one move faster than max_speed; running and sprinting stay far below it (600 and 720 cm/s with no
//                             talents, 834 with the best retail ones).  The retail Rogue dash is in test_worldd_move_checker
//    max_rise_speed 1,350     1.5 x 884 cm/s, the fastest upward step (a jump's launch) in the captures
//    max_rise_burst 500 cm    the upward credit (the other directions hold `slack`): climbing onto a ledge in
//                             game lifted the character 112, 132 and 131 cm in three moves 15 ms apart (375 cm
//                             in 45 ms; 1,350 cm/s x dt + 50 cm allowed about 70 each, so all three were refused
//                             as "rising too fast": a false positive, 2026-10-01).  The burst is spent once and
//                             earns back from the allowance not used: a sustained climb is held to max_rise_speed
//    max_fall_speed 6,000     1.5 x UE's terminal velocity (4,000 cm/s, APhysicsVolume's default; not read
//                             from the exe) ; the captures' fastest fall is 896 cm/s (a jump's descent)
//    max_time_step 5 s        longest gap between moves in the captures: 0.65 s
//    pace_rate 1.25, pace_bank 5 s
//                             the TimeStamp is paced against the server's clock: 1.25 s earned per second
//                             (25% for a client clock that runs fast, packet jitter), at most 5 s in hand (a
//                             stall of the client or the network, then the burst that follows).  All 2,483
//                             captured moves pass at their own pace, in 0.2 s and 1 s bursts, and with a
//                             client clock 15% fast (test_worldd_move_checker); a client that races its
//                             TimeStamp or sends 36 moves in one datagram gets the bank once and 1.25 x
//                             real time after that
//    first_move_seconds 1 s   the first move is measured against the spawn point as if 1 s had passed
//    world box                the envelope of the 21,796 location vectors in content.db's design records
//                             (SpawnLocation, NamedLocation, Teleportation, POI, WorldSpawn ... : X -2,008,064
//                             .. 432,254, Y -912,785 .. 1,381,376, Z -11,119 .. 100,352 once the tables'
//                             placeholder values at +-131,072 are left out) plus 5 km horizontally
//    time_stamp_reset_gap 120 s  UE's CharacterMovement resets the client's clock every MinTimeBetweenTimeStampResets
//                             and the server calls a TimeStamp that jumps by more than half of that a reset
//                             (IsClientTimeStampValid).  AoC's client does it every 240 s, not after UE's 4 hours: the retail
//                             captures 62, 68, 83, 85, 107 and 109 have moves at 239.97 .. 240.01 s followed by 0.01 .. 0.08 s (a
//                             session's clock starts at 0 at the spawn).  With the gap at 2 h every move for the next 240 s was
//                             dropped as a duplicate: no acknowledgement (the client's saved moves piled up: "Hit limit of 96
//                             saved moves"), no position, and the Sprint's drain and the regeneration pause never heard the player
//                             move: E1 stage 1's "Stamina stops updating about 4 minutes after the join".  Half the period is
//                             UE's own threshold; a duplicate or a reordered datagram falls by milliseconds.
//  Not checked: collision, and anything vertical beyond the two speeds.
//
//  The mounts milestone, stage 4: a possessed mount's three-dimensional modes (`MoveLimits::air`).  Swimming (movement mode 4) and flying (mode 5) are judged by the 3D distance
//  against a speed cap, not by the horizontal distance and the two vertical speeds; mode 5 is refused outright for a mount that cannot fly (ModeNotAllowed).  The numbers are the
//  Blueprint's (mount_speeds.tsv: MaxFlySpeed, MaxSwimSpeed, MaxFlyFallSpeed 7,500 of Mount_Flying_Base's movement component) and the retail captures' (871 Dawnbreaker 358 s,
//  883 drake 71 s, 915 and 899 aquatic):
//    cruise      3D speed = MaxFlySpeed x MoveSpeedMult exactly (1,290 / 1,741.5 with the dash in 4,500 of 4,757 samples); swimming = MaxSwimSpeed x MoveSpeedMult (920, 3D: 899 shows the
//                vertical share inside the same 920).
//    the dive    while the view is pitched down the speed climbs (about 1,580-1,620 cm/s^2) to a ceiling that is linear in the pitch, MaxFlySpeed at 0 degrees to MaxFlyFallSpeed at 90:
//                1,290 + 69 per degree (3,671 at 34.5 degrees, 871; 2,188 at 13; 6,004 at 68.3 in 883; the dash scales the whole ceiling by 1.35).  Released, it brakes
//                (BrakingDecelerationFlying 8,192; 8,500 measured) back to the cruise.  The checker keeps `air_speed_`, the allowance: it rises at `air.accel` towards the ceiling the move's
//                pitch gives and falls at `air.brake` towards it; a move is fair when its 3D distance is within the allowance times `air.margin` times dt (plus the slack, once).
//    carry       the allowance outlives mode 5: the first moves of a fall or a landing may still carry the flight's speed, decaying at `air.brake`.
//  Stage 4 fix 1 (the user's test 2: "the server relocates me and it feels really bad"; the run's refusals came from the stage-3 build, which judged mode 5 along the ground, but the
//  stage-4 law was just as strict): a correction in the air is far worse for the player than a missed cheat, so the flight is corrected for GROSS violations only.  Mode 5 is never
//  judged along the ground (a checker without `air` judges it in 3D at the plain speed).  Two judges (`AirLimits`):
//    the physics  the client's own numbers, from the mount Blueprints' CharMoveComp (Mount_Flying_Base; the gryphon inherits all of it): MaxFlySpeed 800 (the item text's 1,290 is the floor),
//                 MaxFlyFallSpeed 7,500 with bUseFlyFallSpeed, MaxFlyStrafeSpeed 750, MaxFlyBwdSpeed 400, BrakingDecelerationFlying 8,192, GravityScale 1.3 (980 x 1.3 = 1,274 cm/s^2), AirControl
//                 0.125; MaxAcceleration is left at UE's 2,048 (retail climbs at 1,600).  The checker carries the speed the client earned (`hold_speed_`: measured from accepted moves, limited to what
//                 the plain law allowed, so a hack cannot bootstrap itself) and lets a move go at most the carried speed (falling at `hold_brake`, a quarter of the client's braking) plus the climb
//                 `hard_accel` (4,096 = 2 x MaxAcceleration) plus gravity in proportion to how steeply the move descends; never less than `gross_cruise` (1.5) x the cruise, never more than
//                 MaxFlyFallSpeed x MoveSpeedMult (the dash) x `phys_margin` (1.10).  Beyond that: refused (TooFastInAir): a teleport, a speed hack, a jump of speed.
//    the plain law  the pitch's ceiling above (the path's steepness, or the view's pitch when the path backs it to within `pitch_trust` degrees, whichever is steeper, so a fall with the view level is covered and a faked pitch on a level path is not).  A move beyond it but inside the physics is
//                 ACCEPTED and flagged (`MoveVerdict::borderline`, `soft_allowed`): the session logs "would refuse" with the numbers, so a later run can tune the law.  A move beyond `sustain_margin`
//                 (1.3) x it counts as time over the law; more than `sustain_seconds` (2.5) of it is corrected (TooFastInAirForLong), every move that stays over, until moves that do not pay it back.
//  Retail's own outliers (single samples a correction or a lost packet displaced: 871's 12,625 and 6,506 cm/s at 88.8 s, the 5 km snap-back at 110.7 s) are refused like any other move;
//  the next move is measured from the last good one, so one refusal is all a displaced sample costs.
// ============================================================================
#pragma once
// -- Cyb . C.O.A --

#include "protocol/repl/move_packed.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace aoc { namespace worldd {

struct WorldBox {
    double min_x, max_x, min_y, max_y, min_z, max_z;   // cm
    bool contains(double x, double y, double z) const {
        return x >= min_x && x <= max_x && y >= min_y && y <= max_y && z >= min_z && z <= max_z;
    }
};

/// Verra with a margin (the header: evidence).
inline constexpr WorldBox kVerraBox = {-2600000.0, 1000000.0, -1500000.0, 2000000.0, -200000.0, 400000.0};

/// A possessed mount's three-dimensional modes (see the header): swimming (movement mode 4) and flying (mode 5).  `active` false (the pawn's checker): every mode is judged by
/// the horizontal and vertical rules above.
struct AirLimits {
    bool   active = false;
    bool   can_fly = false;          // mode 5 is allowed (the mount is a flyer)
    double fly_speed = 0;            // cm/s at MoveSpeedMult 1: MaxFlySpeed
    double dive_speed = 7500.0;      // cm/s: MaxFlyFallSpeed, the ceiling at a pitch of 90 degrees down
    double swim_speed = 400.0;       // cm/s at MoveSpeedMult 1: MaxSwimSpeed
    double margin = 1.10;            // over the retail numbers (the data fits at 1.0; 10 % for the client's drift and the quantization)
    double accel = 2400.0;           // cm/s^2 the allowance may climb towards the dive's ceiling (retail 1,580-1,620, x1.5)
    double brake = 4000.0;           // cm/s^2 it falls at (BrakingDecelerationFlying 8,192; retail 8,500; half of it, so a late brake is not a fault)
    // Stage 4 fix 1: the corrections are for gross violations only (see the header).  The numbers above are the plain flight law (the pitch's ceiling); a move beyond it is logged, not
    // corrected, unless it is also beyond the physics below or it stays beyond the law for too long.
    double hard_accel = 4096.0;      // cm/s^2 the carried speed may climb by: twice UE's MaxAcceleration (2,048; the mount Blueprints leave it), the retail climb is 1,600
    double gravity = 1274.0;         // cm/s^2 added to the climb in proportion to how steeply the move descends: 980 x GravityScale 1.3 (Mount_Flying_Base)
    double hold_brake = 2000.0;      // cm/s^2 the carried speed falls at: a quarter of BrakingDecelerationFlying 8,192 (retail 8,500), so a coast is never cut short
    double gross_cruise = 1.5;       // x the cruise: the speed any move may have, whatever the pitch or the history
    double phys_margin = 1.10;       // x MaxFlyFallSpeed x MoveSpeedMult: the physical maximum
    double sustain_margin = 1.30;    // x the plain law's allowance: beyond it a move counts as time over the law ...
    double sustain_seconds = 2.5;    // ... and this many seconds over it are corrected
    // Review A I2: the view's pitch is the CLIENT's claim, so it only counts as far as the move itself backs it: the dive's ceiling uses min(view pitch, the path's steepness + this).
    // The client's velocity lags its view while it turns into a dive (the captures 871 and 883 stay inside the law at every move from 20 degrees up, and 10 flags a move or two), so a real dive is never short of it; a faked -90 degrees on a
    // level path gets this many degrees of ceiling and no more (25 here: a margin over the 20 the two retail flights need).
    double pitch_trust = 25.0;       // degrees
};

struct MoveLimits {
    double   max_speed = 2135.0;               // cm/s, horizontal (--max-move-speed lowers it for testing)
    double   dash_speed = 3355.0;              // cm/s, horizontal, while a Dodge is on: 1.5 x the Rogue dash's 2,237 (the larger of this and max_speed)
    double   slack = 50.0;                     // cm: the credit each direction starts with and may hold at most
    double   max_rise_speed = 1350.0;          // cm/s
    double   max_rise_burst = 500.0;           // cm: the upward credit starts with this and may hold at most this (a climb onto a ledge)
    double   max_fall_speed = 6000.0;          // cm/s
    double   max_time_step = 5.0;              // s: the most dt that counts
    double   first_move_seconds = 1.0;         // s: dt of the first move (and of the first after a clock reset)
    double   time_stamp_reset_gap = 120.0;     // s: a TimeStamp this far below the last one is a clock reset (half of the client's 240 s period)
    double   pace_rate = 1.25;                 // s of TimeStamp earned per second of the server's clock
    double   pace_bank = 5.0;                  // s: the most that can be in hand (a stall, then a burst after packet loss)
    WorldBox world = kVerraBox;
    AirLimits air;                             // the mounts milestone, stage 4
};

/// What the checker keeps: the last position that passed.
struct GoodPosition {
    double  x = 0, y = 0, z = 0;       // cm
    double  yaw_degrees = 0;           // the control rotation's
    uint8_t movement_mode = protocol::repl::kMoveWalking;
    uint16_t pitch16 = 0;              // the control rotation's pitch (a mount's RemoteViewPitch16: stage 4)
    bool    has_time_stamp = false;    // false until a move has been accepted
    float   time_stamp = 0;

    bool operator==(const GoodPosition&) const = default;
};

enum class MoveFault : uint8_t { None, NotFinite, OutOfWorld, TimeStampNotRising, TooFast, RisingTooFast, FallingTooFast, ModeNotAllowed, TooFastInAir, TooFastInAirForLong };

const char* fault_name(MoveFault f);

struct MoveVerdict {
    bool      accepted = true;
    MoveFault fault = MoveFault::None;
    /// For the log: the step the move made and the most the limits allowed (the speed times dt, plus the credit in hand),
    /// in cm (TooFast, RisingTooFast, FallingTooFast); 0 for the others.
    double    step = 0;
    double    allowed = 0;
    double    dt = 0;                  // the seconds the allowance was computed for
    /// Stage 4 fix 1: an accepted flying move beyond the plain flight law (the pitch's ceiling) but inside the physics: not corrected, logged ("would refuse"), `soft_allowed` cm
    /// being what the plain law allowed (`step` is the move's 3D distance).
    bool      borderline = false;
    double    soft_allowed = 0;
};

class MoveChecker {
public:
    MoveChecker(const MoveLimits& limits, const GoodPosition& start)
        : limits_(limits), good_(start), credit_along_(limits.slack), credit_up_(limits.max_rise_burst), credit_down_(limits.slack) {}

    /// Judges `move` against the last good position; an accepted move becomes the last good position. `now` is the
    /// server's clock when the move arrived (the Endpoint's): a move cannot claim more time (dt) than the bank the clock
    /// has filled holds (pace_rate x elapsed, at most pace_bank); an accepted move spends its dt from it. Moves that arrive
    /// together (one datagram) share one `now`.  `dash`: a Dodge is on (the ability runtime's dash_allowed), so the horizontal
    /// limit is dash_speed instead of max_speed (when that is the larger).
    MoveVerdict check(const protocol::repl::NetworkMove& move, std::chrono::steady_clock::time_point now, bool dash = false);

    /// The same with the horizontal limit `allow_speed` cm/s instead of max_speed when that is the larger (plan D2: a movement ability's
    /// window: Flourish's slide, a Blink; `allow_speed` 0 is max_speed). The Dodge's `dash` is allow_speed = dash_speed.
    MoveVerdict check_allowing(const protocol::repl::NetworkMove& move, std::chrono::steady_clock::time_point now, double allow_speed);

    /// Whether a move with this TimeStamp has been seen already (it is not newer than the last good one): the pending and
    /// old moves that ride along with each new move are skipped when they are.
    bool is_stale(float time_stamp) const {
        if (!good_.has_time_stamp || time_stamp > good_.time_stamp) return false;
        return !(static_cast<double>(good_.time_stamp) - time_stamp > limits_.time_stamp_reset_gap);   // a clock reset is a new move
    }
// built by C | O | A with Cyb

    /// The server moved the player (a respawn, a GM teleport): the last good position is `x, y, z` now, the credits start over; the TimeStamp state is kept (the client's
    /// next moves go on from it: true for a correction only; a death also restarts the client's clock, see `restart_clock`).
    void teleport(double x, double y, double z, double yaw_degrees) {
        good_.x = x;
        good_.y = y;
        good_.z = z;
        good_.yaw_degrees = yaw_degrees;
        credit_along_ = limits_.slack;
        credit_up_ = limits_.max_rise_burst;
        credit_down_ = limits_.slack;
        air_speed_ = 0;   // (stage 4) a flight does not carry its speed through a server-side move
        hold_speed_ = 0;
        over_law_ = 0;
    }

    /// Review A I1: the controlled actor changes (a mount-up, a dismount): new limits and a new last good position (the TimeStamp restarts near 0, the flight's speed does not carry
    /// from one body to the other), but the pacing state is the SESSION's and stays: the pace bank (and whether it has been filled), the server's clock of its last refill, and the credits
    /// (cut to the new limits).  A fresh checker per possession refilled the bank (5 s of claimed time) and the first-move second every time, so mounting and dismounting in a loop was a
    /// hop of that many cm each round.
    void rebase(const MoveLimits& limits, const GoodPosition& start) {
        limits_ = limits;
        good_ = start;
        credit_along_ = std::min(credit_along_, limits_.slack);
        credit_up_ = std::min(credit_up_, limits_.max_rise_burst);
        credit_down_ = std::min(credit_down_, limits_.slack);
        mult_ = 1.0;
        air_speed_ = 0;
        hold_speed_ = 0;
        over_law_ = 0;
    }

    /// The seconds of claimed time in hand (the pace bank; 0 until the first check): for the tests and the log.
    double pace_bank() const { return paced_ ? bank_ : limits_.pace_bank; }

    /// The client's pawn restarted (a death's ClientRestart): its movement clock (the TimeStamp of every move) starts again near 0, so the last good TimeStamp means nothing any more and
    /// every move below it would be dropped as TimeStampNotRising until the new clock passed the old value (the user's death of 2026-10-10: 66 s of frozen movement).  Forgets the TimeStamp
    /// only: the position, the credits and the pace bank stay (`rebase` is the possession's, which starts everything over).
    void restart_clock() {
        good_.has_time_stamp = false;
        good_.time_stamp = 0;
    }

    /// The mounts milestone, stage 3: a possession has its own limits (a mount's speed is its Blueprint's times its MoveSpeedMult: session/riding.h): the
    /// horizontal limit now.  The credits, the bank and the last good position stay.
    void set_max_speed(double v) { limits_.max_speed = v; }
    /// The GM travel tools (session/gm_travel.h): the pawn's horizontal limits now, `max_speed` for a move and `dash_speed` while a Dodge is on, scaled by the GM's run speed
    /// multiplier.  The credits, the bank and the last good position stay.
    void set_speed_limits(double max_speed, double dash_speed) {
        limits_.max_speed = max_speed;
        limits_.dash_speed = dash_speed;
    }

    /// Stage 4: MoveSpeedMult now (with the grace after a fall of it: session/riding.h), which scales the 3D modes' speeds and the dive's ceiling.
    void set_speed_mult(double m) { mult_ = std::isfinite(m) && m > 0 ? m : 1.0; }
    double speed_mult() const { return mult_; }
    /// The speed (cm/s) the flight may carry now: after a dive it falls back to the cruise at `air.brake`.
    double air_speed() const { return air_speed_; }

    const GoodPosition& good() const { return good_; }
    const MoveLimits&   limits() const { return limits_; }

private:
    MoveVerdict judge(const protocol::repl::NetworkMove& move, std::chrono::steady_clock::time_point now, double speed);
    MoveLimits   limits_;
    GoodPosition good_;
    double       credit_along_, credit_up_, credit_down_;   // cm in hand per direction, 0 .. slack (up: 0 .. max_rise_burst)
    double       mult_ = 1.0;                                // MoveSpeedMult (stage 4)
    double       air_speed_ = 0;                             // the flight's speed allowance, cm/s (0: not flying); see the header
    double       hold_speed_ = 0;                            // stage 4 fix 1: the speed the client may carry (momentum), cm/s; see the header
    double       over_law_ = 0;                              // stage 4 fix 1: seconds spent beyond the plain flight law by more than the sustain margin
    double       bank_ = 0;                                  // s of claimed time the server's clock has earned and the moves have not spent
    bool         paced_ = false;                             // the bank is filled at the first check
    std::chrono::steady_clock::time_point last_now_{};
};

}}  // namespace aoc::worldd
