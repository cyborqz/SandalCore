// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_move_checker.cpp: the plausibility checks of plan A6 (spec section 10)
//
//    a walk (600 cm/s), a sprint (962), a dodge (1,423) and a jump's launch and descent are accepted
//    a teleport, a non-finite number, a TimeStamp going back or standing still, a location outside the world,
//      a climb or a fall faster than the limits get a correction, each with its own reason
//    a refused move changes nothing: the last good position stays, and the next move is measured from it
//    the time a move may claim is bounded (a raced clock buys no more than max_time_step)
//    the time is paced against the server's clock: racing the TimeStamp, 36 moves at one instant and a client
//      whose clock runs 8x fast are held to 1.25 x the speed limit over real time (a bank of 5 s, once); the
//      captured moves, at their own pace, in 0.2 s and 1 s bursts and with a client clock 15% fast, are not
//    the first move is measured against the spawn point; the client's clock reset every 240 s is allowed
//    the limits are configurable (--max-move-speed): at 300 cm/s a walk is refused
//    every one of the 2,483 captured moves is accepted at the defaults, in the order the session judges them
//      (a packet's pending move, then its new one); at 900 cm/s (1.5 x the walking speed) the dodge and
//      sprint of the native run are refused: why the default is 1.5 x the fastest measured speed
//    the Verra box: the content envelope's corners are inside, a centimetre beyond any face is OutOfWorld
//    the upward burst: the in-game ledge (112, 132, 131 cm in three moves 15 ms apart) is accepted, a sustained climb
//      is held to max_rise_speed once the burst (max_rise_burst, 500 cm) is spent, and the burst refills
//    the Dodge's dash (plan A7): the retail Rogue's dash (capture 742: 2,237 cm/s, above max_speed) is accepted while a Dodge is
//      on (check()'s dash, limit 3,355 cm/s), a dash that lasts is refused without it, and no Dodge allows 4,500
// ============================================================================
#include "capture_moves.h"
#include "capture_rogue_dash.h"
#include "session/move_checker.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <limits>
#include <vector>

using namespace aoc::worldd;
namespace repl = aoc::protocol::repl;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

static repl::NetworkMove move_at(float ts, double x, double y, double z, uint16_t yaw = 16384, uint8_t mode = repl::kMoveWalking) {
    repl::NetworkMove m;
    m.time_stamp = ts;
    m.acceleration = repl::quantize_vector(10, 0, 0, 0);
    m.location = repl::quantize_vector(100, x, y, z);
    m.control_rotation.yaw = yaw;
    m.movement_mode = mode;
    return m;
}

static GoodPosition start_at(double x, double y, double z) {
    GoodPosition g;
    g.x = x;
    g.y = y;
    g.z = z;
    return g;
}

using Clock = std::chrono::steady_clock;
static const Clock::time_point t0{std::chrono::seconds(5000)};
static Clock::time_point at_s(double s) { return t0 + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(s)); }
// CoA x C Y B

/// For the tests that are not about pacing: every move arrives long after the one before, so the bank is always full and
/// the allowance is the move's own claim (up to max_time_step).
static Clock::time_point g_roomy = t0;
static MoveVerdict chk(MoveChecker& c, const repl::NetworkMove& m) {
    g_roomy += std::chrono::seconds(10);
    return c.check(m, g_roomy);
}

// the spawn point of the captures
static constexpr double kX = -777664.5, kY = 616578.69, kZ = 15952.05;

static void test_plausible_moves() {
    std::printf("plausible moves\n");
    MoveChecker c({}, start_at(kX, kY, kZ));
    // walking east at 600 cm/s for a second, a frame (17 ms) at a time
    float ts = 10.0f;
    double x = kX;
    bool all = true;
    for (int i = 0; i < 60; ++i) {
        ts += 0.017f;
        x += 600.0 * 0.017;
        const MoveVerdict v = chk(c, move_at(ts, x, kY, kZ));
        all = all && v.accepted;
    }
    CHECK(all, "60 frames of walking at 600 cm/s are accepted");
    CHECK(std::fabs(c.good().x - x) < 0.01 && c.good().has_time_stamp && c.good().time_stamp == ts, "each accepted move is the last good position: location and TimeStamp");

    // sprint 962 cm/s, the dodge's 1,423 cm/s at the shortest frame the captures show (9.4 ms)
    all = true;
    for (int i = 0; i < 30; ++i) {
        ts += 0.0094f;
        x += 1423.0 * 0.0094;
        all = all && chk(c, move_at(ts, x, kY, kZ)).accepted;
    }
    CHECK(all, "the dodge's 1,423 cm/s at 9.4 ms frames is accepted (the default limit is 2,135)");

    // a jump: launched at 884 cm/s up, falling back at 896 cm/s, one frame each
    ts += 0.017f;
    CHECK(chk(c, move_at(ts, x, kY, kZ + 884.0 * 0.017, 16384, repl::kMoveFalling)).accepted, "a jump's launch (884 cm/s up) is accepted");
    ts += 0.017f;
    CHECK(chk(c, move_at(ts, x, kY, c.good().z - 896.0 * 0.017, 16384, repl::kMoveFalling)).accepted, "and its descent (896 cm/s down)");

    // the yaw and the mode of the control rotation and the move are kept
    ts += 0.017f;
    chk(c, move_at(ts, x, kY, c.good().z, 0x4000 + 1024, repl::kMoveWalking));
    CHECK(std::fabs(c.good().yaw_degrees - (16384 + 1024) * 360.0 / 65536.0) < 1e-9 && c.good().movement_mode == repl::kMoveWalking,
          "the yaw of the control rotation (in degrees) and the movement mode are kept with the position");
}

static void test_refusals() {
    std::printf("refusals\n");
    const GoodPosition start = start_at(kX, kY, kZ);
    MoveChecker c({}, start);
    MoveVerdict v = chk(c, move_at(1.0f, kX, kY, kZ));
    CHECK(v.accepted && v.fault == MoveFault::None, "(a first move at the spawn point)");
    const GoodPosition before = c.good();

    v = chk(c, move_at(1.017f, kX + 1000.0, kY, kZ));
    CHECK(!v.accepted && v.fault == MoveFault::TooFast && v.step > 990 && v.allowed < 100, "a teleport of 10 m in 17 ms: TooFast, with the step and what was allowed");
    CHECK(c.good() == before, "a refused move changes nothing: the last good position, yaw and TimeStamp are as they were");
    v = chk(c, move_at(1.017f, kX + 30.0, kY, kZ));
    CHECK(v.accepted, "the next move, near the last good position, is accepted: measured from it");

    v = chk(c, move_at(std::numeric_limits<float>::quiet_NaN(), kX, kY, kZ));
    CHECK(!v.accepted && v.fault == MoveFault::NotFinite, "a TimeStamp that is NaN: NotFinite");
    v = chk(c, move_at(std::numeric_limits<float>::infinity(), kX, kY, kZ));
    CHECK(!v.accepted && v.fault == MoveFault::NotFinite, "a TimeStamp of infinity: NotFinite");
    repl::NetworkMove nan_loc = move_at(2.0f, kX, kY, kZ);
    nan_loc.location.mode = repl::QuantVec::Mode::RawDouble;
    nan_loc.location.x = static_cast<int64_t>(std::bit_cast<uint64_t>(std::numeric_limits<double>::quiet_NaN()));
    v = chk(c, nan_loc);
    CHECK(!v.accepted && v.fault == MoveFault::NotFinite, "a Location with a NaN component (a full-precision vector): NotFinite");
    repl::NetworkMove inf_acc = move_at(2.0f, kX, kY, kZ);
    inf_acc.acceleration.mode = repl::QuantVec::Mode::RawDouble;
    inf_acc.acceleration.y = static_cast<int64_t>(std::bit_cast<uint64_t>(std::numeric_limits<double>::infinity()));
    v = chk(c, inf_acc);
    CHECK(!v.accepted && v.fault == MoveFault::NotFinite, "an Acceleration of infinity: NotFinite");
    repl::NetworkMove raw_float = move_at(2.0f, kX, kY, kZ);
    raw_float.location.mode = repl::QuantVec::Mode::RawFloat;
    raw_float.location.x = std::bit_cast<uint32_t>(std::numeric_limits<float>::infinity());
    raw_float.location.y = std::bit_cast<uint32_t>(1.0f);
    raw_float.location.z = std::bit_cast<uint32_t>(1.0f);
    CHECK(!chk(c, raw_float).accepted, "a single-precision infinity too");

    v = chk(c, move_at(1.017f, kX + 30.0, kY, kZ));
    CHECK(!v.accepted && v.fault == MoveFault::TimeStampNotRising, "the same TimeStamp again: TimeStampNotRising");
    v = chk(c, move_at(0.5f, kX + 30.0, kY, kZ));
    CHECK(!v.accepted && v.fault == MoveFault::TimeStampNotRising, "a TimeStamp going back: TimeStampNotRising");
    v = chk(c, move_at(0.0f, kX + 30.0, kY, kZ));
    CHECK(!v.accepted && v.fault == MoveFault::TimeStampNotRising, "a TimeStamp of 0: TimeStampNotRising (UE's IsClientTimeStampValid)");
    v = chk(c, move_at(-3.0f, kX + 30.0, kY, kZ));
    CHECK(!v.accepted && v.fault == MoveFault::TimeStampNotRising, "a negative TimeStamp: TimeStampNotRising");

    v = chk(c, move_at(3.0f, 2000000.0, kY, kZ));
    CHECK(!v.accepted && v.fault == MoveFault::OutOfWorld, "a location 28 km east of the spawn, outside the box: OutOfWorld (before the speed is looked at)");
    v = chk(c, move_at(3.0f, kX, kY, 900000.0));
    CHECK(!v.accepted && v.fault == MoveFault::OutOfWorld, "9 km up: OutOfWorld");
    v = chk(c, move_at(3.0f, kX, kY, -900000.0));
    CHECK(!v.accepted && v.fault == MoveFault::OutOfWorld, "and down");

    // vertical: well inside the box, too fast for a jump or a fall
    v = chk(c, move_at(1.034f, c.good().x, c.good().y, c.good().z + 600.0));
    CHECK(!v.accepted && v.fault == MoveFault::RisingTooFast && v.allowed > 500 && v.allowed < 600,
          "6 m up in 17 ms: RisingTooFast (what was allowed: the 500 cm burst and 23 cm of speed)");
    v = chk(c, move_at(1.034f, c.good().x, c.good().y, c.good().z - 1500.0));
    CHECK(!v.accepted && v.fault == MoveFault::FallingTooFast, "15 m down in 17 ms: FallingTooFast");
    v = chk(c, move_at(2.034f, c.good().x, c.good().y, c.good().z - 3000.0));
    CHECK(v.accepted, "30 m of fall in a second (3,000 cm/s: a long drop, below UE's terminal velocity of 4,000) is accepted");
}

static void test_time() {
    std::printf("the time a move may claim\n");
    MoveChecker c({}, start_at(kX, kY, kZ));
    chk(c, move_at(100.0f, kX, kY, kZ));
    // a client that races its clock to buy distance: 1,000 s of TimeStamp, 200 m of teleport
    MoveVerdict v = chk(c, move_at(1100.0f, kX + 20000.0, kY, kZ));
    CHECK(!v.accepted && v.fault == MoveFault::TooFast && v.dt == 5.0 && v.allowed < 11000.0, "1,000 s of TimeStamp buy 5 s of allowance, not 1,000: 200 m refused");
    chk(c, move_at(1100.0f, kX, kY, kZ));
    CHECK(c.good().time_stamp == 1100.0f, "(a move at the raced TimeStamp, back at the position, is accepted: the clock is the client's)");
    // a legitimate stall: 3 s without a move, 60 m on (a hitch while sprinting)
    v = chk(c, move_at(1103.0f, kX + 6000.0, kY, kZ));
    CHECK(v.accepted, "a 3 s gap and 60 m (2,000 cm/s) is accepted: dt 3 s buys 6,455 cm");

    // the first move: against the spawn, with a one second allowance
    MoveChecker first({}, start_at(kX, kY, kZ));
    v = chk(first, move_at(0.8f, kX + 300.0, kY, kZ));
    CHECK(v.accepted && v.dt == 1.0, "the first move is measured as one second after the spawn: a 3 m offset is accepted");
    MoveChecker far({}, start_at(kX, kY, kZ));
    v = chk(far, move_at(0.8f, kX + 50000.0, kY, kZ));
    CHECK(!v.accepted && v.fault == MoveFault::TooFast, "a first move 500 m from the spawn point: refused, the client is put back at the spawn");
    CHECK(far.good().x == kX && !far.good().has_time_stamp, "(and nothing was learned from it)");
    CHECK(!far.is_stale(0.5f) && first.is_stale(0.8f) && first.is_stale(0.5f) && !first.is_stale(0.81f),
          "is_stale: nothing is stale before a move was accepted; after one, a TimeStamp not above its own is");
// -- C:Y:B x C | O | A --

    // a client clock reset: the retail client starts its clock again at 0 every 240 s (captures 62, 68, 83, 85, 107, 109: 239.98, then 0.02), and a fall of
    // more than half of that (120 s) is a reset, measured like a first move
    MoveChecker reset({}, start_at(kX, kY, kZ));
    chk(reset, move_at(239.98f, kX, kY, kZ));
    CHECK(reset.is_stale(239.98f) && reset.is_stale(239.0f) && !reset.is_stale(0.02f), "is_stale: a TimeStamp a second back is a duplicate, one 240 s back is a new move");
    v = chk(reset, move_at(0.02f, kX + 10.0, kY, kZ));
    CHECK(v.accepted && reset.good().time_stamp == 0.02f && v.dt == 1.0, "a TimeStamp that falls from 239.98 s to 0.02 s is a clock reset: accepted, dt as for a first move");
    v = chk(reset, move_at(0.036f, kX + 20.0, kY, kZ));
    CHECK(v.accepted && reset.good().time_stamp == 0.036f, "the moves after it rise normally");
    v = chk(reset, move_at(0.02f, kX + 10.0, kY, kZ));
    CHECK(!v.accepted && v.fault == MoveFault::TimeStampNotRising, "but a fall of a few ms after that is a TimeStamp going back");
    // a far-from-the-position reset is still a teleport
    v = chk(reset, move_at(50000.0f, kX + 10.0, kY, kZ));
    CHECK(v.accepted, "(a leap forward in the clock with the position unchanged is fine)");
    v = chk(reset, move_at(3.0f, kX + 90000.0, kY, kZ));
    CHECK(!v.accepted && v.fault == MoveFault::TooFast, "a reset does not excuse a teleport");
}

static void test_limits() {
    std::printf("the limits are configurable\n");
    MoveLimits slow;
    slow.max_speed = 300.0;
    MoveChecker c(slow, start_at(kX, kY, kZ));
    chk(c, move_at(1.0f, kX, kY, kZ));
    float ts = 1.0f;
    double x = kX;
    int refused = 0, accepted = 0;
    for (int i = 0; i < 20; ++i) {
        ts += 0.1f;
        const MoveVerdict v = chk(c, move_at(ts, x + 600.0 * 0.1 * (i + 1), kY, kZ));
        (v.accepted ? accepted : refused)++;
    }
    CHECK(refused >= 15, "at --max-move-speed 300 a walk at 600 cm/s is refused (the corrections show in game)");
    CHECK(accepted >= 1 && accepted <= 3, "(the first step, inside the slack, still passes)");

    // the slack is a credit, not an allowance on every move: 55 cm every 17 ms (3,200 cm/s) is held to the limit over time
    MoveChecker creeper({}, start_at(kX, kY, kZ));
    chk(creeper, move_at(1.0f, kX, kY, kZ));
    int passed = 0;
    float cts = 1.0f;
    double cx = kX;
    for (int i = 0; i < 120; ++i) {
        cts += 0.017f;
        const MoveVerdict v = chk(creeper, move_at(cts, cx + 55.0, kY, kZ));
        if (v.accepted) {
            cx += 55.0;
            ++passed;
        }
    }
    const double elapsed = cts - 1.0f;
    CHECK(passed > 10 && passed < 100 && cx - kX <= 2135.0 * elapsed + 50.0 + 0.5,
          "55 cm every 17 ms (3,200 cm/s, over the 2,135 limit): some moves are refused, and the distance covered stays within limit x time + slack");
    std::printf("    %d of 120 moves passed: %.0f cm in %.2f s (%.0f cm/s)\n", passed, cx - kX, elapsed, (cx - kX) / elapsed);

    // and the credit refills when the allowance is not used: a ledge now and again is absorbed
    MoveChecker ledges({}, start_at(kX, kY, kZ));
    chk(ledges, move_at(1.0f, kX, kY, kZ));
    float lts = 1.0f;
    bool all_up = true;
    for (int i = 0; i < 20; ++i) {
        lts += 0.017f;
        all_up = all_up && chk(ledges, move_at(lts, kX, kY, ledges.good().z + 45.0)).accepted;   // a 45 cm step up (MaxStepHeight)...
        for (int k = 0; k < 4; ++k) {
            lts += 0.017f;
            chk(ledges, move_at(lts, kX, kY, ledges.good().z));   // ...and a few frames on the flat to earn it back
        }
    }
    CHECK(all_up, "a 45 cm step up every five frames (single-frame ledges) is accepted: the credit refills between them");

    MoveLimits tight;
    tight.slack = 0;
    tight.max_speed = 1000.0;
    MoveChecker t(tight, start_at(0, 0, 0));
    chk(t, move_at(1.0f, 0, 0, 0));
    CHECK(chk(t, move_at(2.0f, 1000.0, 0, 0)).accepted && !chk(t, move_at(3.0f, 2000.0 + 1.0, 0, 0)).accepted,
          "slack 0: exactly speed x dt passes, a centimetre more does not");
    CHECK(std::string(fault_name(MoveFault::TooFast)) == "too fast along the ground" && std::string(fault_name(MoveFault::None)) == "none",
          "every fault has a name for the log");
}

// ---- the captures ----------------------------------------------------------------------------------------------------------

static std::vector<uint8_t> bytes_of(const char* hex) {
    std::vector<uint8_t> out;
    auto nibble = [](char ch) { return ch <= '9' ? ch - '0' : ch - 'a' + 10; };
    for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) out.push_back(static_cast<uint8_t>(nibble(hex[i]) << 4 | nibble(hex[i + 1])));
    return out;
}

/// Judges a capture's moves in the order the session does: a packet's pending move (when not seen), then its new move.
/// The server's clock follows the TimeStamps (a client plays one second of TimeStamp per second of the server's), except that
/// with `burst_seconds` > 0 every move waits for the end of its window of that length and arrives with the rest of it (a stall,
/// then a burst), and `clock_skew` is how many times as fast as the server's the client's clock runs.
template <size_t N>
static void replay(const capture_moves::Move (&moves)[N], const MoveLimits& limits, const GoodPosition& start, size_t& refused, size_t& total, MoveFault* first_fault,
                   double burst_seconds = 0, double clock_skew = 1.0) {
    MoveChecker c(limits, start);
    double first_ts = -1;
    for (const capture_moves::Move& mv : moves) {
        const auto data = bytes_of(mv.hex);
        const auto m = repl::read_move_packed(data.data(), data.size(), 0, mv.bits);
        if (!m) continue;
        if (first_ts < 0) first_ts = m->new_move.time_stamp;
        double arrives = (m->new_move.time_stamp - first_ts) / clock_skew;
        if (burst_seconds > 0) arrives = std::ceil(arrives / burst_seconds) * burst_seconds;
        const Clock::time_point now = at_s(arrives);
        if (m->pending && !c.is_stale(m->pending->time_stamp)) c.check(*m->pending, now);
        const MoveVerdict v = c.check(m->new_move, now);
        ++total;
        if (!v.accepted) {
            if (refused == 0 && first_fault) *first_fault = v.fault;
            ++refused;
        }
    }
}

static void test_captures() {
    std::printf("the captured moves\n");
    const GoodPosition spawn = start_at(kX, kY, kZ);
    size_t refused = 0, total = 0;
    MoveFault fault = MoveFault::None;
    replay(capture_moves::kE2e, {}, spawn, refused, total, &fault);
    CHECK(total == 640 && refused == 0, "the e2e capture: all 640 moves accepted at the defaults, arriving at the pace of their own TimeStamps");
    refused = total = 0;
    replay(capture_moves::kNative, {}, spawn, refused, total, &fault);
    CHECK(total == 1843 && refused == 0, "the 2026-09-29 run: all 1,843 moves accepted at the defaults (walking, sprint, dodge, jumps), at their own pace");
// [CoA :: C.Y.B]

    // what the pacing must not touch: a stall and the burst after it, and a client clock that runs fast
    for (const double burst : {0.2, 1.0}) {
        size_t r_e = 0, t_e = 0, r_n = 0, t_n = 0;
        replay(capture_moves::kE2e, {}, spawn, r_e, t_e, &fault, burst, 1.0);
        replay(capture_moves::kNative, {}, spawn, r_n, t_n, &fault, burst, 1.0);
        char msg[200];
        std::snprintf(msg, sizeof(msg), "all 2,483 captured moves arriving in %.1f s bursts (a stall, then everything at once): none refused (e2e %zu, native %zu)", burst, r_e, r_n);
        CHECK(t_e + t_n == 2483 && r_e == 0 && r_n == 0, msg);
    }
    {
        size_t r = 0, t = 0;
        replay(capture_moves::kNative, {}, spawn, r, t, &fault, 0.0, 1.15);
        char msg[200];
        std::snprintf(msg, sizeof(msg), "the native run with the client's clock 15%% fast (1.15 s of TimeStamp per second of the server's): %zu refused (the rate of 1.25 leaves room)", r);
        CHECK(t == 1843 && r == 0, msg);
        r = t = 0;
        replay(capture_moves::kNative, {}, spawn, r, t, &fault, 0.0, 3.0);
        std::snprintf(msg, sizeof(msg), "(control: a client clock 3x fast is not a clock: %zu of its moves refused)", r);
        CHECK(r > 100, msg);
    }

    MoveLimits naive;
    naive.max_speed = 900.0;   // 1.5 x the walking speed alone
    refused = total = 0;
    replay(capture_moves::kNative, naive, spawn, refused, total, &fault);
    CHECK(refused > 100 && fault == MoveFault::TooFast, "at 900 cm/s (1.5 x walking) the run's sprint and dodge are refused: the default is built on the fastest measured speed");
    std::printf("    at 900 cm/s the native run has %zu of %zu moves refused\n", refused, total);

    // the same moves with the first one offset 300 m: the first move is refused and the client put back, as the spec says
    GoodPosition elsewhere = spawn;
    elsewhere.x += 30000.0;
    refused = total = 0;
    replay(capture_moves::kE2e, {}, elsewhere, refused, total, &fault);
    CHECK(refused >= 1 && fault == MoveFault::TooFast, "(control: a spawn point 300 m from where the client is: its first moves are refused)");
}

// ---- pacing against the server's clock ------------------------------------------------------------------------------------------

static void test_pacing() {
    std::printf("the TimeStamp is paced against the server's clock\n");
    const MoveLimits lim;
    {
        // a client that adds 5 s to its TimeStamp with every move, at 60 moves a second of the server's clock, 100 m a move
        MoveChecker c(lim, start_at(kX, kY, kZ));
        float ts = 1.0f;
        double x = kX;
        int accepted = 0, refused = 0;
        c.check(move_at(ts, x, kY, kZ), at_s(0));
        for (int i = 1; i <= 176; ++i) {
            ts += 5.0f;
            const double nx = x + 10000.0;
            if (c.check(move_at(ts, nx, kY, kZ), at_s(i / 60.0)).accepted) {
                ++accepted;
                x = nx;
            } else {
                ++refused;
            }
        }
        const double seconds = 176 / 60.0;
        const double covered = x - kX;
        std::printf("    racing the TimeStamp for %.2f s: %d moves accepted, %d refused, %.0f cm covered (%.0f cm/s)\n", seconds, accepted, refused, covered, covered / seconds);
        CHECK(refused >= 150 && accepted <= 10, "a client that claims 5 s per move at 60 moves a second: nearly every move is refused (unpaced, all 176 were accepted: 17.6 km in 2.9 s)");
        CHECK(covered <= lim.max_speed * (lim.pace_bank + lim.pace_rate * seconds) + lim.slack + 1.0,
              "the distance covered is the bank once plus 1.25 x the speed limit over the real time that passed");
    }
    {
        // the same client in one instant: 36 moves that share one `now` (one datagram)
        MoveChecker c(lim, start_at(kX, kY, kZ));
        float ts = 1.0f;
        double x = kX;
        c.check(move_at(ts, x, kY, kZ), at_s(0));
        int accepted = 0;
        for (int i = 0; i < 36; ++i) {
            ts += 5.0f;
            if (c.check(move_at(ts, x + 10000.0, kY, kZ), at_s(1.0)).accepted) {
                ++accepted;
                x += 10000.0;
            }
        }
        CHECK(accepted <= 2 && x - kX <= lim.max_speed * (lim.pace_bank + lim.pace_rate * 1.0) + lim.slack + 1.0,
              "36 racing moves at one instant: the bank's worth at most (about 100 m), not 3.6 km");
    }
    {
        // a time-dilation speed hack: the TimeStamp runs 8x, the position keeps 600 cm/s of the TimeStamp (4,800 cm/s of real time)
        MoveChecker c(lim, start_at(kX, kY, kZ));
        double x = kX;
        float ts = 1.0f;
        int accepted = 0, refused = 0;
        const int moves = 1200;   // 20 s at 60 moves a second
        for (int i = 0; i < moves; ++i) {
            ts += 8.0f / 60.0f;
            x += 8.0 * 600.0 / 60.0;
            const MoveVerdict v = c.check(move_at(ts, x, kY, kZ), at_s(i / 60.0));
            (v.accepted ? accepted : refused)++;
            if (!v.accepted) x = c.good().x;   // put back
        }
        const double seconds = moves / 60.0;
        const double covered = c.good().x - kX;
        std::printf("    8x time dilation for %.0f s: %d accepted, %d refused, %.0f cm covered (%.0f cm/s of real time; the client wants 4,800)\n", seconds, accepted, refused,
                    covered, covered / seconds);
        CHECK(refused >= 300, "a client whose TimeStamp runs 8x fast has hundreds of its moves refused, and is held well below the 4,800 cm/s it wants (unpaced, all 1,200 were accepted)");
        CHECK(covered <= lim.max_speed * (lim.pace_bank + lim.pace_rate * seconds) + lim.slack + 1.0, "and it covers no more than the pacing allows: 1.25 x the speed limit plus the bank once");
    }
    {
        // a stall: 20 s of nothing, then a client that plays honestly again (its TimeStamps kept running): one 5 s step is accepted
        MoveChecker c(lim, start_at(kX, kY, kZ));
        c.check(move_at(1.0f, kX, kY, kZ), at_s(0));
        const MoveVerdict v = c.check(move_at(6.0f, kX + 6000.0, kY, kZ), at_s(20.0));
        CHECK(v.accepted && v.dt == 5.0, "after a 20 s stall one step of 5 s (60 m: 1,200 cm/s) is accepted: the bank was full");
        const MoveVerdict again = c.check(move_at(11.0f, kX + 12000.0, kY, kZ), at_s(20.016));
        CHECK(!again.accepted && again.fault == MoveFault::TooFast, "but a second one at once is not: the bank is spent and refills at 1.25 s per second");
    }
    {
        // a refused move does not spend the bank
        MoveChecker c(lim, start_at(kX, kY, kZ));
        c.check(move_at(1.0f, kX, kY, kZ), at_s(0));
        c.check(move_at(1.5f, kX + 90000.0, kY, kZ), at_s(0.1));   // refused: a teleport
        const MoveVerdict v = c.check(move_at(1.6f, kX + 100.0, kY, kZ), at_s(0.2));
        CHECK(v.accepted && v.dt > 0.5 && v.dt <= 0.6 + 1e-6, "a refused move spends nothing: the next one is measured from the last good TimeStamp with its full time");
    }
}
// (CoA ~ C.Y.B)

// ---- the Verra box ----------------------------------------------------------------------------------------------------------------

static void test_world_box() {
    std::printf("the Verra box\n");
    // the envelope of content.db's design-record locations (move_checker.h): every corner is inside the box and a player can stand there
    const double xs[2] = {-2008064.0, 432254.0}, ys[2] = {-912785.0, 1381376.0}, zs[2] = {-11119.0, 100352.0};
    bool corners = true;
    for (double x : xs)
        for (double y : ys)
            for (double z : zs) {
                MoveChecker c({}, start_at(x, y, z));
                corners = corners && kVerraBox.contains(x, y, z) && chk(c, move_at(1.0f, x, y, z)).accepted;
            }
    CHECK(corners, "the eight corners of the content envelope (X -2,008,064 .. 432,254, Y -912,785 .. 1,381,376, Z -11,119 .. 100,352) are inside");

    // each face (the documented numbers, not the constant: a box that moved must fail here): exactly on it is inside, a
    // centimetre beyond it is OutOfWorld
    const WorldBox b = {-2600000.0, 1000000.0, -1500000.0, 2000000.0, -200000.0, 400000.0};
    CHECK(kVerraBox.min_x == b.min_x && kVerraBox.max_x == b.max_x && kVerraBox.min_y == b.min_y && kVerraBox.max_y == b.max_y &&
              kVerraBox.min_z == b.min_z && kVerraBox.max_z == b.max_z,
          "kVerraBox is X -2,600,000 .. 1,000,000, Y -1,500,000 .. 2,000,000, Z -200,000 .. 400,000 cm (move_checker.h, the spec)");
    struct Face {
        const char* name;
        double x, y, z;
        int axis;
        double dir;
    };
    const double mx = (b.min_x + b.max_x) / 2, my = (b.min_y + b.max_y) / 2, mz = (b.min_z + b.max_z) / 2;
    const Face faces[] = {
        {"west (min X)", b.min_x, my, mz, 0, -1},  {"east (max X)", b.max_x, my, mz, 0, +1}, {"south (min Y)", mx, b.min_y, mz, 1, -1},
        {"north (max Y)", mx, b.max_y, mz, 1, +1}, {"floor (min Z)", mx, my, b.min_z, 2, -1}, {"ceiling (max Z)", mx, my, b.max_z, 2, +1},
    };
    for (const Face& f : faces) {
        MoveChecker on({}, start_at(f.x, f.y, f.z));
        const MoveVerdict v_on = chk(on, move_at(1.0f, f.x, f.y, f.z));
        MoveChecker over({}, start_at(f.x, f.y, f.z));
        double ox = f.x, oy = f.y, oz = f.z;
        (f.axis == 0 ? ox : f.axis == 1 ? oy : oz) += f.dir * 1.0;
        const MoveVerdict v_over = chk(over, move_at(1.0f, ox, oy, oz));
        char msg[160];
        std::snprintf(msg, sizeof(msg), "%s: on the face is inside, 1 cm beyond it is OutOfWorld", f.name);
        CHECK(v_on.accepted && !v_over.accepted && v_over.fault == MoveFault::OutOfWorld, msg);
    }
}

/// The ledge of 2026-10-01 (worldd.log, 19:52:51): three moves that rise 112, 132 and 131 cm, 15-16 ms apart, standing still
/// otherwise. `ts` and `now` (seconds) advance with the moves, which arrive at the pace of their own TimeStamps.
static std::vector<MoveVerdict> ledge_climb(MoveChecker& c, float& ts, double& now) {
    static const double kRise[3] = {112.0, 132.0, 131.0};
    static const double kDt[3] = {0.015, 0.016, 0.016};
    std::vector<MoveVerdict> out;
    for (int i = 0; i < 3; ++i) {
        ts += static_cast<float>(kDt[i]);
        now += kDt[i];
        out.push_back(c.check(move_at(ts, c.good().x, c.good().y, c.good().z + kRise[i]), at_s(now)));
    }
    return out;
}

static void test_rise_burst() {
    std::printf("the upward burst (a climb onto a ledge)\n");
    {
        MoveChecker c({}, start_at(kX, kY, kZ));
        float ts = 10.0f;
        double now = 0;
        CHECK(c.check(move_at(ts, kX, kY, kZ), at_s(now)).accepted, "(a first move at the spawn point)");
        const double z0 = c.good().z;
        const auto v = ledge_climb(c, ts, now);
        CHECK(v[0].accepted && v[1].accepted && v[2].accepted,
              "the in-game ledge, 112, 132 and 131 cm up in three moves 15 ms apart (375 cm in 45 ms), is accepted");
        CHECK(std::fabs(c.good().z - z0 - 375.0) < 0.05, "and the player is 375 cm higher");
    }
    {
        MoveLimits as_before;
        as_before.max_rise_burst = as_before.slack;   // the credit the draft had: the same as every other direction
        MoveChecker c(as_before, start_at(kX, kY, kZ));
        float ts = 10.0f;
        double now = 0;
        c.check(move_at(ts, kX, kY, kZ), at_s(now));
        const auto v = ledge_climb(c, ts, now);
        CHECK(!v[0].accepted && v[0].fault == MoveFault::RisingTooFast && v[0].step > 111 && v[0].allowed > 69 && v[0].allowed < 71,
              "(control: with the 50 cm credit the first move is refused, 112 cm against the 70 the log shows: the false positive)");
    }

    // a sustained climb is not a ledge: the burst is spent once
    {
        MoveChecker c({}, start_at(kX, kY, kZ));
        float ts = 10.0f;
        double now = 0;
        c.check(move_at(ts, kX, kY, kZ), at_s(now));
        const double z0 = c.good().z;
        size_t accepted = 0, refused = 0;
        for (int i = 1; i <= 20; ++i) {   // a client that claims 130 cm more every 15 ms (8,700 cm/s), 2.6 m in all
            ts += 0.015f;
            now += 0.015;
            const MoveVerdict v = c.check(move_at(ts, kX, kY, z0 + 130.0 * i), at_s(now));
            (v.accepted ? accepted : refused)++;
        }
        CHECK(accepted >= 3 && accepted <= 6 && refused >= 14, "20 moves of 130 cm every 15 ms: only the first few pass (the burst), the rest are refused");
        CHECK(c.good().z - z0 < 700, "and the player has not climbed beyond what the burst and the speed allow");
    }
    {
        // a client that is put back when refused and tries again from where it was put, climbing 3,000 cm/s for 2 s
        MoveChecker c({}, start_at(kX, kY, kZ));
        float ts = 10.0f;
        double now = 0;
        c.check(move_at(ts, kX, kY, kZ), at_s(now));
        const double z0 = c.good().z;
        size_t refused = 0;
        for (int i = 0; i < 120; ++i) {
            ts += 0.0167f;
            now += 0.0167;
            if (!c.check(move_at(ts, c.good().x, c.good().y, c.good().z + 50.0), at_s(now)).accepted) ++refused;
        }
        const double climbed = c.good().z - z0;
        char msg[200];
        std::snprintf(msg, sizeof(msg), "3,000 cm/s of climbing for 2 s: %.0f cm climbed, the burst (500) and max_rise_speed (1,350 cm/s) over 2 s, %zu moves refused",
                      climbed, refused);
        CHECK(climbed > 2500 && climbed < 500 + 1350.0 * 2.0 + 150 && refused > 40, msg);
    }

    // the burst refills: from a quiet moment (an accepted move that does not rise) the ledge passes again
    {
        MoveChecker c({}, start_at(kX, kY, kZ));
        float ts = 10.0f;
        double now = 0;
        c.check(move_at(ts, kX, kY, kZ), at_s(now));
        size_t climbs = 0;
        for (int i = 0; i < 40; ++i) {   // 130 cm every 15 ms from where the player is, until a move is refused
            ts += 0.015f;
            now += 0.015;
            if (!c.check(move_at(ts, c.good().x, c.good().y, c.good().z + 130.0), at_s(now)).accepted) break;
            ++climbs;
        }
        CHECK(climbs >= 3 && climbs <= 6, "the burst is spent by a few moves of 130 cm: the next one in 15 ms is refused");
        ts += 0.5f;
        now += 0.5;
        CHECK(c.check(move_at(ts, c.good().x, c.good().y, c.good().z), at_s(now)).accepted, "(0.5 s of quiet, then a move that stands still)");
        const auto v = ledge_climb(c, ts, now);
        CHECK(v[0].accepted && v[1].accepted && v[2].accepted, "the burst has refilled: the ledge (112, 132, 131 cm in 45 ms) passes again");
    }
}
// -- C-Y-B & CoA --

/// Review A C1: a move in a 3D mode (mode 5 here: a Cleric's Wings of Salvation judges it in 3D) used to refill the vertical credits, so a client that alternated such a move with a
/// climb of the full burst on the ground rule climbed 500 cm per pair, whatever the time.  The climb of the pair loop must stay within what the speed and the one burst allow.
static void test_mode_alternation() {
    std::printf("alternating movement modes does not refill the vertical credits (review A C1)\n");
    for (const bool up : {true, false}) {
        MoveChecker c({}, start_at(kX, kY, kZ));
        float ts = 10.0f;
        Clock::time_point now = t0;
        CHECK(c.check(move_at(ts, kX, kY, kZ), now).accepted, "(a first move at the spawn point)");
        double z = c.good().z;
        const double z0 = z;
        const double step = up ? 500.0 : -50.0;   // (a fall's credit is the 50 cm slack, a climb's the 500 cm burst)
        size_t refused = 0;
        const int pairs = 300;
        for (int i = 0; i < pairs; ++i) {
            ts += 0.001f;
            // the 3D move: standing still in mode 5 (accepted: distance 0)
            c.check(move_at(ts, kX, kY, z, 16384, repl::kMoveFlying), now);
            ts += 0.001f;
            // the ground move: the climb (or fall) of the whole burst, in 1 ms
            if (c.check(move_at(ts, kX, kY, z + step, 16384, repl::kMoveWalking), now).accepted) z += step;
            else ++refused;
        }
        const double seconds = ts - 10.0;
        const double moved = std::fabs(z - z0);
        const double bound = (up ? 1350.0 : 6000.0) * seconds + (up ? 500.0 : 50.0) + 100.0;
        char msg[240];
        std::snprintf(msg, sizeof(msg), "%d pairs of (mode 5 standing still, %s %.0f cm on the ground) in %.2f s: %.0f cm %s (at most %.0f), %zu refused", pairs, up ? "up" : "down", std::fabs(step), seconds, moved,
                      up ? "climbed" : "fallen", bound, refused);
        CHECK(moved <= bound && refused > 200, msg);
    }
}

/// Review A I1: the pacing state is the session's.  A client that spent its pace bank (5 s of claimed time at one instant) and then mounts (or dismounts) must not get the bank and the
/// first-move second again: the checker of the new body takes over the bank.
static void test_rebase() {
    std::printf("a new body (a mount-up, a dismount) keeps the pace bank (review A I1)\n");
    const auto spent = [&](MoveChecker& c, float& ts, Clock::time_point now) {
        c.check(move_at(ts, kX, kY, kZ), now);
        for (int i = 0; i < 4; ++i) {   // 4 s of claimed time per move, all at one instant: the bank (5 s) is gone
            ts += 4.0f;
            c.check(move_at(ts, kX, kY, kZ), now);
        }
    };
    MoveChecker c({}, start_at(kX, kY, kZ));
    float ts = 10.0f;
    spent(c, ts, t0);
    CHECK(c.pace_bank() < 0.01, "(the racing client has spent the bank)");
    c.rebase({}, start_at(kX + 100, kY, kZ));
    CHECK(c.pace_bank() < 0.01, "after rebase the bank is still empty");
    const MoveVerdict v = c.check(move_at(0.1f, kX + 100 + 2000, kY, kZ), t0);
    CHECK(!v.accepted && v.fault == MoveFault::TooFast, "so the first move of the new body may not claim the first-move second: 2,000 cm in an instant is refused");
    // the control: a fresh checker (what a possession used to make) takes the same move
    MoveChecker fresh({}, start_at(kX + 100, kY, kZ));
    CHECK(fresh.check(move_at(0.1f, kX + 100 + 2000, kY, kZ), t0).accepted, "(control: a new checker would have accepted it: it starts with a full bank)");
    // the server's clock still earns the bank for the new body
    const MoveVerdict later = c.check(move_at(0.2f, kX + 100 + 20, kY, kZ), t0 + std::chrono::seconds(2));
    CHECK(later.accepted, "and an honest move after 2 s of the server's clock passes (the bank refills at 1.25 per second)");
    // the new body's limits and position are used, the flight state is not carried
    MoveLimits slow;
    slow.max_speed = 300.0;
    c.rebase(slow, start_at(kX, kY, kZ));
    CHECK(c.limits().max_speed == 300.0 && c.good().x == kX && !c.good().has_time_stamp, "rebase takes the new limits and the new last good position (no TimeStamp yet)");
}

/// The retail Rogue's Dodge dash (capture 742, tests/capture_rogue_dash.h): 14 moves at 2,236-2,237 cm/s, above max_speed (2,135).  The
/// checker takes the dash speed while a Dodge is on (check()'s `dash`): the retail dash is accepted with it, a dash that goes on
/// longer than the retail one is refused without it (the 50 cm slack covers the first 0.3 s), and no Dodge allows more than 3,355 cm/s.
static void test_rogue_dash() {
    std::printf("the retail Rogue's Dodge dash (capture 742)\n");
    using capture_rogue_dash::kMoves;
    constexpr double kWindow = 800.0;   // ms from the trigger: the Evasion (500) and AbilityRuntime::kDashMargin (300)
    const auto judge = [&](bool use_window, size_t* refused, double* peak) {
        MoveChecker c({}, start_at(kMoves[0].x, kMoves[0].y, kMoves[0].z));
        size_t bad = 0;
        double fastest = 0;
        float last_ts = 0;
        double last_x = 0, last_y = 0;
        bool first = true;
        for (const auto& m : kMoves) {
            const bool dash = use_window && m.ms >= capture_rogue_dash::kDodgeTriggerMs && m.ms <= capture_rogue_dash::kDodgeTriggerMs + kWindow;
            repl::NetworkMove mv = move_at(m.time_stamp, m.x, m.y, m.z, m.yaw, m.mode);
            if (!c.check(mv, t0 + std::chrono::milliseconds(m.ms), dash).accepted) ++bad;
            if (!first && m.time_stamp > last_ts) fastest = std::max(fastest, std::hypot(m.x - last_x, m.y - last_y) / (m.time_stamp - last_ts));
            first = false;
            last_ts = m.time_stamp;
            last_x = m.x;
            last_y = m.y;
        }
        *refused = bad;
        *peak = fastest;
        return bad == 0;
    };
    size_t refused = 0;
    double peak = 0;
    const bool with_window = judge(true, &refused, &peak);
    CHECK(peak > 2230 && peak < 2240, "(the capture's fastest step: 2,236-2,237 cm/s, above the limit of 2,135)");
    CHECK(with_window && refused == 0, "every move of the retail dash is accepted with the Dodge's dash window");
    const bool without_window = judge(false, &refused, &peak);
    CHECK(without_window && refused == 0,
          "(the retail dash alone, about 25 cm over the limit in all, fits the 50 cm slack and passes even with no window: only a dash that lasts needs it)");

    // a dash that goes on at 2,237 cm/s for a second, 17.5 ms frames: the first 0.3 s are inside the slack, then it is over the limit
    const auto sustained = [&](bool dash, double cm_per_s, size_t* bad) {
        MoveChecker c({}, start_at(kX, kY, kZ));
        float ts = 10.0f;
        double x = kX;
        Clock::time_point now = t0;
        *bad = 0;
        for (int i = 0; i < 60; ++i) {
            ts += 0.0175f;
            x += cm_per_s * 0.0175;
            now += std::chrono::microseconds(17500);
            if (!c.check(move_at(ts, x, kY, kZ), now, dash).accepted) ++*bad;
        }
        return *bad == 0;
    };
    size_t bad = 0;
    CHECK(!sustained(false, 2237.0, &bad) && bad > 0, "2,237 cm/s for a second without a Dodge is refused: the limit is 2,135 and the slack is spent after 0.3 s");
    CHECK(sustained(true, 2237.0, &bad), "the same with the Dodge's window is accepted");
    CHECK(sustained(true, 3300.0, &bad), "up to the dash's limit (3,355 cm/s) a Dodge allows it");
    CHECK(!sustained(true, 4500.0, &bad) && bad > 0, "and not 4,500: a Dodge buys no teleport");
    // the allowance is the larger of the two limits: a lowered max_speed (--max-move-speed) is not a lower dash
    MoveLimits lowered;
    lowered.max_speed = 4000.0;
    MoveChecker fast(lowered, start_at(kX, kY, kZ));
    CHECK(fast.check(move_at(1.0f, kX + 300.0, kY, kZ), t0, false).accepted && fast.check(move_at(1.1f, kX + 600.0, kY, kZ), t0 + std::chrono::milliseconds(100), true).accepted,
          "a max_speed above the dash's is not lowered by the window");
}

int main() {
    test_plausible_moves();
    test_refusals();
    test_time();
    test_limits();
    test_captures();
    test_pacing();
    test_world_box();
    test_rise_burst();
    test_mode_alternation();
    test_rebase();
    test_rogue_dash();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
