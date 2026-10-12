// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_mounts_fly.cpp  (the mounts milestone, stage 4: flying and swimming in the move checker)
//
//  The checker of a possessed mount in the 3D modes (session/move_checker.h: `MoveLimits::air`, filled by Ride::limits from the mount's Blueprint speeds), on the retail captures:
//
//    the goldens   every ServerMovePacked of 871 (a Dawnbreaker, 358 s: takeoff by ordinary moves mode 1 -> 3 -> 5, the dash in the air, dives to 3,671 cm/s, the brake), 883 (the
//                  drake, 71 s, a dive to 6,004 cm/s at 68 degrees) and the water captures 915 (600 cm/s on land, 920 in the water, no dismount) and 899 (920 in 3D) goes through
//                  the checker the way the session judges them, with the MoveSpeedMult the server held at the time.  Every move passes except retail's own displaced samples (871:
//                  12,625 cm/s at 88.77 s, 6,506 at 88.88 s, the 5 km snap-back at 110.66 s); a refusal puts the client back, which the replay does by moving the rest of the capture
//                  by the same amount (the correction a refused move gets), so a displaced sample costs one refusal and no more
//    the law       the dive ceiling is MaxFlySpeed + (MaxFlyFallSpeed - MaxFlySpeed) x pitch / 90 times MoveSpeedMult, the allowance climbs at most 2,400 cm/s^2 and brakes
//    the rule      (stage 4 fix 1) the server never corrects legitimate flight: a move beyond that law is LOGGED (verdict.borderline) and corrected only when it is grossly beyond the
//                  client's physics (the carried speed + the climb the client can make, MaxFlyFallSpeed x the dash, 1.5 x the cruise) or when it stays beyond the law for seconds
//    exploits      a flying speed hack (level, a dash without the effect, a fake dive pitch), a vertical teleport, mode 5 on a mount that cannot fly, swimming at an aquatic mount's
//                  speed on a mount that is not aquatic
//    health        a flyer's Health is x5 of Mount_Base_HP, x10 for the Drake of the Canopy and the Thundering Bolt Beast (the item texts)
//    the gryphon   the GM Eagle Gryphon (Blueprint 800, text 1,290): its limits, and the 1,570 cm/s of the user's run
//    review A      the vertical credits are not refilled by a 3D move (alternating modes), the claimed pitch counts only `pitch_trust` degrees beyond the path
//
//  What the goldens prove (review B M4): the law and the allowances were FITTED to captures 871, 883, 915 and 899 (all a x1290 Dawnbreaker or Drake, or an aquatic mount) and then replay
//  them, so they show no false positives on those four flights, not that the law is right.  The exploit side is the synthetic tests; the gryphon the user flies (800/600) has no
//  capture and its numbers are modelled (checked against the user's own run only).
// ============================================================================
#include "capture_mount_fly.h"
#include "content/content_db.h"
#include "protocol/repl/move_packed.h"
#include "session/move_checker.h"
#include "session/riding.h"
#include "spawns/mount_spawn.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace aoc::worldd;
namespace repl = aoc::protocol::repl;
namespace cmf = capture_mount_fly;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

using Clock = std::chrono::steady_clock;
static const Clock::time_point t0{std::chrono::seconds(5000)};
static Clock::time_point at_ms(double ms) { return t0 + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(ms)); }

static std::string real_db_path() {
    const char* env = std::getenv("AOC_CONTENT_DB");
    return env && *env ? env : MOUNT_CONTENT_DB_PATH;
}

// ---- the mounts ------------------------------------------------------------------------------------------------------------------------
// CoA / C:Y:B

struct Mounts {
    std::optional<content::ContentDb> db;
    std::optional<spawns::MountBook> book;
    Mounts() {
        std::string error;
        db = content::ContentDb::open(real_db_path(), error);
        if (!db) {
            std::printf("  [FAIL] the real content.db does not open (%s): %s\n", real_db_path().c_str(), error.c_str());
            std::exit(2);
        }
        book = spawns::MountBook::load(*db, error);
        if (!book || !book->load_speeds(MOUNT_SPEEDS_PATH, error)) {
            std::printf("  [FAIL] the mounts do not load: %s\n", error.c_str());
            std::exit(2);
        }
    }
    const content::MountRow* by_blueprint_prefix(const std::string& prefix) const {
        for (const content::MountRow& r : book->data().mounts) {
            if (r.blueprint.rfind(prefix, 0) == 0) return &r;
        }
        return nullptr;
    }
    const content::MountRow* by_item(const std::string& name) const {
        for (const content::MountRow& r : book->data().mounts) {
            if (r.item_name == name) return &r;
        }
        return nullptr;
    }
};

/// The Ride the session builds for `row` (without a registry: only its limits are used) and the checker it gives the player on that mount.
struct Rig {
    Ride ride;
    explicit Rig(const Mounts& m, const content::MountRow& row) : ride(setup_of(m, row)) {}
    static Ride::Setup setup_of(const Mounts& m, const content::MountRow& row) {
        Ride::Setup s;
        s.row = &row;
        const spawns::MountBook::Speeds* sp = m.book->speeds_of(row.blueprint);
        s.run_speed = sp && sp->run > 0 ? sp->run : 600.0 * row.ground_pct / 100.0;
        Ride::fill_speeds(s, *m.book, row);
        return s;
    }
    MoveChecker checker(double x, double y, double z) const {
        GoodPosition start;
        start.x = x;
        start.y = y;
        start.z = z;
        return MoveChecker(ride.limits(MoveLimits{}, 0), start);
    }
};

static repl::NetworkMove move_at(float ts, double x, double y, double z, uint8_t mode, double pitch_degrees = 0, uint16_t yaw = 0) {
    repl::NetworkMove m;
    m.time_stamp = ts;
    m.acceleration = repl::quantize_vector(10, 0, 0, 0);
    m.location = repl::quantize_vector(100, x, y, z);
    m.control_rotation.pitch = static_cast<uint16_t>(static_cast<int>(std::lround(pitch_degrees * 65536.0 / 360.0)) & 0xFFFF);
    m.control_rotation.yaw = yaw;
    m.movement_mode = mode;
    return m;
}

// ---- the golden replay -----------------------------------------------------------------------------------------------------------------

static std::vector<uint8_t> bytes_of(const char* hex) {
    const auto nibble = [](char c) { return static_cast<uint8_t>(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10); };
    std::vector<uint8_t> out;
    for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) out.push_back(static_cast<uint8_t>(nibble(hex[i]) << 4 | nibble(hex[i + 1])));
    return out;
}

/// MoveSpeedMult as the session's checker is given it: the value in force, or the largest of the last second's (the client learns of a fall of it a trip later).
static double mult_at(const cmf::Case& c, double ms) {
    const auto value_at = [&](double t) {
        double m = 1.0;
        for (size_t i = 0; i < c.speeds; ++i) {
            const cmf::Speed& s = cmf::kSpeeds[c.first_speed + i];
            if (s.ms <= t) m = s.mult;
        }
        return m;
    };
    double best = std::max(value_at(ms), value_at(ms - 1000.0));
    for (size_t i = 0; i < c.speeds; ++i) {
        const cmf::Speed& s = cmf::kSpeeds[c.first_speed + i];
        if (s.ms <= ms && s.ms >= ms - 1000.0) best = std::max(best, static_cast<double>(s.mult));
    }
    return best;
}

struct Replay {
    size_t moves = 0, accepted = 0, unreadable = 0;
    std::vector<int> refused_ms;            // the packet's time of each refused move
    std::map<int, MoveFault> faults;
    double fastest = 0;                     // the highest 3D speed between two accepted moves of dt >= 30 ms
    double fastest_dash = 0;                // ... with MoveSpeedMult above 1
    size_t flight_moves = 0, mode_moves[8] = {0};
    size_t borderline = 0;                  // accepted moves the plain flight law would have refused (stage 4 fix 1: logged, not corrected)
};

static Replay replay(const cmf::Case& c, const Ride& ride, double extra_margin = 1.0) {
    (void)extra_margin;
    Replay out;
    MoveChecker checker(ride.limits(MoveLimits{}, 0), GoodPosition{});
    bool started = false;
    double off[3] = {0, 0, 0};            // the correction a refused move gets: the rest of the capture moves by it
    double last_x = 0, last_y = 0, last_z = 0;
    float last_ts = -1;
    for (size_t i = 0; i < c.moves; ++i) {
        const cmf::Move& mv = cmf::kMoves[c.first_move + i];
        const cmf::Payload& pl = cmf::kPayloads[mv.payload];
        const auto data = bytes_of(pl.hex);
        const auto parsed = repl::read_move_packed(data.data(), data.size(), 0, pl.bits);
        if (!parsed) {
            ++out.unreadable;
            continue;
        }
        repl::NetworkMove m = parsed->new_move;
        const repl::Vec3d at = m.location_cm();
        m.location = repl::quantize_vector(100, at.x + off[0], at.y + off[1], at.z + off[2]);
        if (!started) {
            started = true;
            GoodPosition start;
            start.x = at.x;
            start.y = at.y;
            start.z = at.z;
            start.movement_mode = m.movement_mode;
            checker = MoveChecker(ride.limits(MoveLimits{}, 0), start);
        }
        const double mult = mult_at(c, mv.ms);
        checker.set_speed_mult(mult);
        checker.set_max_speed(ride.speed_limit(m.movement_mode, 0) * mult);
        const MoveVerdict v = checker.check(m, at_ms(mv.ms));
        ++out.moves;
        if (m.movement_mode < 8) ++out.mode_moves[m.movement_mode];
        if (v.accepted) {
            ++out.accepted;
            if (v.borderline) ++out.borderline;
            const repl::Vec3d now = m.location_cm();
            if (last_ts >= 0 && m.time_stamp - last_ts >= 0.03f && m.time_stamp > last_ts) {
                const double dt = m.time_stamp - last_ts;
                const double speed = std::sqrt((now.x - last_x) * (now.x - last_x) + (now.y - last_y) * (now.y - last_y) + (now.z - last_z) * (now.z - last_z)) / dt;
                out.fastest = std::max(out.fastest, speed);
                if (mult > 1.0) out.fastest_dash = std::max(out.fastest_dash, speed);
            }
            last_x = now.x;
            last_y = now.y;
            last_z = now.z;
            last_ts = m.time_stamp;
        } else if (v.fault == MoveFault::TimeStampNotRising) {
            // a clock reset the checker did not take, or a duplicate: not a verdict on the position
            ++out.moves;
        } else {
            out.refused_ms.push_back(mv.ms);
            out.faults[mv.ms] = v.fault;
            const GoodPosition& g = checker.good();
            const repl::Vec3d now = m.location_cm();
            off[0] += g.x - now.x;
            off[1] += g.y - now.y;
            off[2] += g.z - now.z;
        }
    }
    return out;
}

static const cmf::Case& case_of(int capture) {
    for (const cmf::Case& c : cmf::kCases) {
        if (c.capture == capture) return c;
    }
    std::printf("  [FAIL] no capture %d in the header\n", capture);
    std::exit(2);
}
// [Cyb + C | O | A]

static void test_goldens(const Mounts& m) {
    std::printf("the retail captures through the mount's move checker (871, 883, 915, 899)\n");
    const content::MountRow* dawn = m.by_blueprint_prefix("Mount_Flying_Dawnbreaker");
    const content::MountRow* drake = m.by_blueprint_prefix("Mount_Flying_DrakeOfTheCanopy");
    const content::MountRow* shoal = m.by_blueprint_prefix("Mount_Aquatic_Shoalskipper");
    if (!dawn || !drake || !shoal) {
        CHECK(false, "(the Dawnbreaker, the drake and the Shoalskipper are in the book)");
        return;
    }
    {
        Rig rig(m, *dawn);
        CHECK(rig.ride.can_fly() && std::fabs(rig.ride.speed_limit(5, 0) - 1290.0 * 1.5) < 1.0, "(the Dawnbreaker flies at 1,290)");
        const cmf::Case& c = case_of(871);
        const Replay r = replay(c, rig.ride);
        std::printf("    871: %zu moves, %zu accepted, refused at ms:", r.moves, r.accepted);
        for (int ms : r.refused_ms) std::printf(" %d", ms);
        std::printf("; fastest %.0f cm/s, with the dash %.0f; modes 1/3/4/5: %zu/%zu/%zu/%zu, %zu beyond the plain law (logged only)\n", r.fastest, r.fastest_dash, r.mode_moves[1], r.mode_moves[3], r.mode_moves[4], r.mode_moves[5], r.borderline);
        CHECK(r.unreadable == 0 && r.moves >= c.moves, "all 4,771 moves of 871 read with the real parser");
        CHECK(r.mode_moves[1] == 5 && r.mode_moves[3] == 4 && r.mode_moves[5] == 4762, "the takeoff by ordinary moves: 5 walking, 4 falling (the jump), then 4,762 in mode 5");
        const std::vector<int> glitches = {93994, 94103, 355893};
        CHECK(r.refused_ms == glitches, "every move passes except retail's own displaced samples (12,625 and 6,506 cm/s at 88.8 s, the 5 km snap-back at 110.66 s)");
        CHECK(r.fastest > 3600.0 && r.fastest < 3800.0, "the dive to 3,671 cm/s is accepted (the fastest accepted step is the capture's)");
        CHECK(r.fastest_dash > 1700.0, "the dash in the air (1,290 x 1.35 = 1,741.5) is accepted");
    }
    {
        Rig rig(m, *drake);
        const Replay r = replay(case_of(883), rig.ride);
        std::printf("    883: %zu moves, %zu accepted, fastest %.0f cm/s, %zu beyond the plain law (logged only)\n", r.moves, r.accepted, r.fastest, r.borderline);
        CHECK(r.unreadable == 0 && r.moves == case_of(883).moves, "all 1,123 moves of 883 read");
        CHECK(r.refused_ms.empty(), "every move of the drake's flight passes, its steep dives included");
        CHECK(r.fastest > 5900.0, "the dive to 6,004 cm/s at 68 degrees down is accepted");
    }
    {
        Rig rig(m, *shoal);
        const Replay r15 = replay(case_of(915), rig.ride);
        std::printf("    915: %zu moves, %zu accepted, modes 1/3/4: %zu/%zu/%zu, fastest %.0f cm/s%s", r15.moves, r15.accepted, r15.mode_moves[1], r15.mode_moves[3], r15.mode_moves[4], r15.fastest, "\n");
        CHECK(r15.unreadable == 0 && r15.refused_ms.empty() && r15.mode_moves[1] == 44 && r15.mode_moves[4] == 189,
              "915: 600 cm/s on land (44 moves) and 920 in the water (189 moves in mode 4, from 7 s on) pass; entering the water dismounts nobody");
        const Replay r99 = replay(case_of(899), rig.ride);
        std::printf("    899: %zu moves, %zu accepted, modes 1/3/4: %zu/%zu/%zu, fastest %.0f cm/s%s", r99.moves, r99.accepted, r99.mode_moves[1], r99.mode_moves[3], r99.mode_moves[4], r99.fastest, "\n");
        CHECK(r99.unreadable == 0 && r99.refused_ms.empty() && r99.mode_moves[4] == 418, "899: 418 moves in mode 4, 920 cm/s in 3D with the vertical share inside it, all pass");
        CHECK(r99.fastest > 915.0 && r99.fastest < 925.0, "(the fastest accepted step is the capture's 920)");
        // a ground mount does not swim like that
        const content::MountRow* bird = m.by_item("Mount_Grounded_TerrorBird_0");
        if (!bird) bird = m.by_blueprint_prefix("Mount_Grounded_");
        if (bird) {
            Rig g(m, *bird);
            const Replay bad = replay(case_of(899), g.ride);
            CHECK(bad.refused_ms.size() > 100, "the same swim on a ground mount (water speed 400, not 920) is refused again and again");
        }
    }
}

// ---- the law ---------------------------------------------------------------------------------------------------------------------------

/// Flies `seconds` of 0.07 s moves at `speed` cm/s along x at `pitch` (the view), from (x0, y, z); returns how many were refused.  The checker moves with its own good position.
struct Flight {
    MoveChecker c;
    float ts = 10.0f;
    double x, y, z;
    Clock::time_point now = t0;
    size_t refused = 0, accepted = 0;
    MoveFault last_fault = MoveFault::None;
    MoveVerdict last_verdict;
    double x0, y0, z0, elapsed = 0;
    Flight(const Rig& rig, double ax, double ay, double az) : c(rig.checker(ax, ay, az)), x(ax), y(ay), z(az), x0(ax), y0(ay), z0(az) {}
    /// How far the client got (a refused move puts it back, so a hack is held to the allowance over time, not move by move).
    double travelled() const { return std::sqrt((x - x0) * (x - x0) + (y - y0) * (y - y0) + (z - z0) * (z - z0)); }
    /// ... and the speed that makes of the time it spent trying.
    double average() const { return elapsed > 0 ? travelled() / elapsed : 0; }
    bool step(double dx, double dy, double dz, double dt, uint8_t mode, double pitch) {
        ts += static_cast<float>(dt);
        elapsed += dt;
        now += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(dt));
        const repl::NetworkMove m = move_at(ts, x + dx, y + dy, z + dz, mode, pitch);
        const MoveVerdict v = c.check(m, now);
        last_verdict = v;
        if (v.accepted) {
            x += dx;
            y += dy;
            z += dz;
            ++accepted;
        } else {
            ++refused;
            last_fault = v.fault;
        }
        return v.accepted;
    }
    /// A run at `speed` cm/s along the direction (cos a, 0, sin a) for `seconds`; the pitch is the view's.
    void run(double speed, double climb_degrees, double pitch, double seconds, uint8_t mode = repl::kMoveFlying, double dt = 0.07) {
        const double a = climb_degrees * 3.14159265358979323846 / 180.0;
        for (double t = 0; t < seconds; t += dt) step(speed * dt * std::cos(a), 0, speed * dt * std::sin(a), dt, mode, pitch);
    }
};

static void test_the_law(const Mounts& m) {
    std::printf("the flight law: cruise, the dive's ceiling, its climb and its brake\n");
    const content::MountRow* dawn = m.by_blueprint_prefix("Mount_Flying_Dawnbreaker");
    if (!dawn) return;
    Rig rig(m, *dawn);
    {
        Flight f(rig, 0, 0, 10000);
        f.run(1290.0, 0, 0, 10.0);
        CHECK(f.refused == 0, "10 s of level flight at the cruise (1,290 cm/s) passes");
    }
    {
        Flight f(rig, 0, 0, 10000);
        f.run(1290.0 * 1.09, 0, 0, 10.0);
        CHECK(f.refused == 0, "and at 1.09 x (the margin is 10 %)");
        // stage 4 fix 1: past the law (1.1 x) but inside the physics (1.5 x the cruise) a move is logged, not corrected
        Flight g(rig, 0, 0, 10000);
        size_t flagged = 0;
        for (double t = 0; t < 10.0; t += 0.07) {
            g.step(1290.0 * 1.25 * 0.07, 0, 0, 0.07, repl::kMoveFlying, 0);
            if (g.last_verdict.borderline) ++flagged;
        }
        CHECK(g.refused == 0 && flagged > 100, "at 1.25 x the cruise nothing is corrected, every move is flagged 'would refuse' (the law's numbers are for tuning)");
        Flight h(rig, 0, 0, 10000);
        h.run(1290.0 * 1.8, 0, 0, 10.0);
        std::printf("    (1.8 x for 10 s: %zu refused, %zu accepted, average %.0f cm/s)\n", h.refused, h.accepted, h.average());
        CHECK(h.refused >= 10 && h.average() < 1290.0 * 1.5 + 60.0, "at 1.8 x it is grossly past the physics: refused, and the client gets no further than 1.5 x the cruise");
    }
    {
        // the dive: 34.5 degrees down, climbing from the cruise at 1,600 cm/s^2 to 3,671 and holding it
        Flight f(rig, 0, 0, 100000);
        double speed = 1290.0;
        for (int i = 0; i < 40; ++i) {
            speed = std::min(3671.0, speed + 1600.0 * 0.07);
            f.step(speed * 0.07 * std::cos(0.45), 0, -speed * 0.07 * std::sin(0.45), 0.07, repl::kMoveFlying, -34.5);
        }
        CHECK(f.refused == 0 && speed > 3670.0, "a dive at 34.5 degrees down gaining 1,600 cm/s^2 to the 3,671 ceiling is accepted all the way");
        // released: the brake (8,500 cm/s^2) back to the cruise, pitch held
        double v = speed;
        for (int i = 0; i < 12; ++i) {
            v = std::max(1290.0, v - 8500.0 * 0.07);
            f.step(v * 0.07, 0, 0, 0.07, repl::kMoveFlying, -34.5);
        }
        CHECK(f.refused == 0, "and the brake that follows it, at 8,500 cm/s^2, is accepted");
    }
    {
        // the ceiling is the pitch's: 13 degrees down tops out at 2,188
        Flight f(rig, 0, 0, 100000);
        double v = 1290.0;
        for (int i = 0; i < 30; ++i) {
            v = std::min(2188.0, v + 1600.0 * 0.07);
            f.step(v * 0.07, 0, 0, 0.07, repl::kMoveFlying, -13.0);
        }
        CHECK(f.refused == 0, "13 degrees down: the climb to 2,188 cm/s is accepted");
        Flight g(rig, 0, 0, 100000);
        double vg = 1290.0;
        for (int i = 0; i < 40; ++i) {
            vg = std::min(2800.0, vg + 1600.0 * 0.07);
            g.step(vg * 0.07, 0, 0, 0.07, repl::kMoveFlying, -13.0);
        }
        CHECK(g.refused == 0, "at 13 degrees down 2,800 cm/s is past the ceiling (2,188 x 1.1) but inside 1.3 x it: logged, not corrected");
        Flight k(rig, 0, 0, 100000);
        double vk = 1290.0;
        for (int i = 0; i < 140; ++i) {
            vk = std::min(4000.0, vk + 1600.0 * 0.07);
            k.step(vk * 0.07, 0, 0, 0.07, repl::kMoveFlying, -13.0);
        }
        CHECK(k.refused >= 1, "4,000 cm/s level under a 13-degree pitch (1.8 x the law, and past the carried speed's climb) is corrected");
    }
    {
        // the dash scales the whole ceiling by MoveSpeedMult
        Flight f(rig, 0, 0, 100000);
        f.c.set_speed_mult(1.35);
        f.run(1741.5, 0, 0, 5.0);
        CHECK(f.refused == 0, "with MoveSpeedMult 1.35 the cruise is 1,741.5 cm/s");
        Flight g(rig, 0, 0, 100000);
        g.run(1741.5, 0, 0, 12.0);
        std::printf("    (1,741 cm/s for 12 s, no dash: %zu refused, %zu accepted, last fault %s)\n", g.refused, g.accepted, fault_name(g.last_fault));
        CHECK(g.refused >= 1 && g.last_fault == MoveFault::TooFastInAirForLong, "the same speed without the effect (a dash that was never started) is logged, then corrected once it has stayed over the law for 2.5 s");
    }
    {
        // the fall of the multiplier: the session holds the last second's (effective_mult); a checker told 1.0 at once refuses the client that has not heard
        Flight f(rig, 0, 0, 100000);
        f.c.set_speed_mult(1.35);
        f.run(1741.5, 0, 0, 2.0);
        f.c.set_speed_mult(1.0);
        const size_t before = f.refused;
        f.run(1741.5, 0, 0, 1.0);
        CHECK(f.refused == before, "(a multiplier that fell is not a correction at once: 1,741 cm/s is inside 1.5 x the cruise)");
        f.run(1741.5, 0, 0, 5.0);
        CHECK(f.refused > before, "(but a dash that has ended is not kept: after the sustain window the speed is corrected; the session's grace is Ride::effective_mult)");
    }
    {
        // after a flight the allowance is carried into a fall and a landing, and brakes
        Flight f(rig, 0, 0, 100000);
        double v = 1290.0;
        for (int i = 0; i < 40; ++i) {
            v = std::min(3671.0, v + 1600.0 * 0.07);
            f.step(v * 0.07, 0, -v * 0.07 * 0.3, 0.07, repl::kMoveFlying, -34.5);
        }
        const size_t before = f.refused;
        for (int i = 0; i < 3; ++i) f.step(3000.0 * 0.07, 0, -200.0 * 0.07, 0.07, repl::kMoveFalling, -10);
        CHECK(f.refused == before, "the first moves of a fall after a dive may still carry 3,000 cm/s along (the allowance brakes, it does not vanish)");
        for (int i = 0; i < 40; ++i) f.step(0, 0, -50.0 * 0.07, 0.07, repl::kMoveFalling, -10);   // 2.8 s of falling slowly
        const size_t mid = f.refused;
        for (int i = 0; i < 20; ++i) f.step(3000.0 * 0.07, 0, 0, 0.07, repl::kMoveWalking, 0);
        CHECK(f.refused > mid, "and 3 s later 3,000 cm/s on foot is refused again (the ground limit is 990 x 1.5)");
    }
}
// C.O.A captures, C:Y:B code

static void test_the_gryphon(const Mounts& m) {
    std::printf("the GM Eagle Gryphon (Blueprint 800, text 215 %%): the user's run flew it at about 1,570 cm/s\n");
    const content::MountRow* g = m.by_item("Mount_Flying_PTR_ONLY");
    if (!g) {
        CHECK(false, "(the Eagle Gryphon is in the book)");
        return;
    }
    const spawns::MountBook::Speeds* sp = m.book->speeds_of(g->blueprint);
    CHECK(sp && std::fabs(sp->fly - 800.0) < 1e-6 && std::fabs(sp->run - 600.0) < 1e-6, "its Blueprint (a plain child of Mount_Flying_Base) says MaxFlySpeed 800 and MaxRunSpeed 600");
    Rig rig(m, *g);
    CHECK(rig.ride.can_fly(), "it flies");
    // the old limit: 800 x 1.5 = 1,200 refused the dive the user made; the cruise is the text's 1,290 and the dive follows the Blueprint's 800 -> 7,500
    Flight f(rig, 0, 0, 20000);
    f.run(1290.0, 0, 0, 4.0);
    CHECK(f.refused == 0, "its cruise is at least the item text's 1,290 cm/s");
    double v = 1290.0;
    for (int i = 0; i < 30; ++i) {
        v = std::min(1570.0, v + 1600.0 * 0.052);
        f.step(v * 0.052, 0, -v * 0.052 * 0.1, 0.052, repl::kMoveFlying, -10.4);
    }
    CHECK(f.refused == 0, "a gentle dive (10 degrees down) at 1,570 cm/s, the speed the refused moves of the run had, is accepted");
    Flight h(rig, 0, 0, 20000);
    h.run(2200.0, 0, 0, 3.0);
    std::printf("    (2,200 for 3 s: %zu refused, %zu accepted, average %.0f cm/s)\n", h.refused, h.accepted, h.average());
    CHECK(h.refused >= 5 && h.average() < 1290.0 * 1.5 + 60.0, "2,200 cm/s level is still a hack on it: refused, held to 1.5 x the cruise");

    // G1 stage 1, the user's run of 2026-10-10: movement mode 3 (falling) on a flyer -- the take-off, a stall -- was judged along the ground at the mount's ground limit, and moves of
    // 69 cm in 0.069 s and 96 cm in 0.078 s (1,000 and 1,230 cm/s) were refused by 1 and 19 cm and the gryphon put back mid-flight
    {
        Flight t(rig, 0, 0, 20000);
        t.run(1000.0, 0, 0, 3.0, repl::kMoveFalling, 0.069);
        Flight u(rig, 0, 0, 20000);
        u.run(1230.0, 0, 0, 3.0, repl::kMoveFalling, 0.078);
        std::printf("    (mode 3 level at 1,000 cm/s: %zu refused of %zu; at 1,230 cm/s: %zu of %zu)\n", t.refused, t.refused + t.accepted, u.refused, u.refused + u.accepted);
        CHECK(t.refused == 0 && u.refused == 0, "a flyer in movement mode 3 at 1,000 and at 1,230 cm/s level is not refused: it travels at flight speed in every mode of the air");
        Flight hack(rig, 0, 0, 20000);
        hack.run(4000.0, 0, 0, 3.0, repl::kMoveFalling, 0.07);
        CHECK(hack.refused >= 5 && hack.average() < 1290.0 * 1.1 + 60.0, "but 4,000 cm/s in mode 3 is a hack: held to the cruise times the margin, as the flying mode is");
        Flight climb(rig, 0, 0, 20000);
        for (int i = 0; i < 20; ++i) climb.step(0, 0, 3000.0 * 0.07, 0.07, repl::kMoveFalling, 0);
        CHECK(climb.refused >= 5, "and mode 3 does not climb: the rising rules stay (3,000 cm/s straight up is refused once the 500 cm burst is spent)");
    }
    // a mount that cannot fly keeps the ground's law in mode 3 (nothing was loosened for it)
    {
        const content::MountRow* ground = nullptr;
        for (const content::MountRow& row : m.book->data().mounts) {
            if (row.kind != "flying" && row.kind != "aquatic") { ground = &row; break; }
        }
        if (ground) {
            Rig horse(m, *ground);
            Flight f(horse, 0, 0, 20000);
            f.run(5000.0, 0, 0, 2.0, repl::kMoveFalling, 0.07);
            CHECK(f.refused >= 5, "a mount that cannot fly is still held to its ground limit in mode 3");
        } else {
            CHECK(false, "(a ground mount is in the book)");
        }
    }
}
// C Y B - CoA

// ---- exploits --------------------------------------------------------------------------------------------------------------------------

static void test_exploits(const Mounts& m) {
    std::printf("exploits: a flying speed hack, a vertical teleport, flight on a ground mount, swimming on a mount that is not aquatic\n");
    const content::MountRow* dawn = m.by_blueprint_prefix("Mount_Flying_Dawnbreaker");
    const content::MountRow* bird = m.by_item("Mount_Grounded_TerrorBird_0");
    if (!bird) bird = m.by_blueprint_prefix("Mount_Grounded_");
    const content::MountRow* shoal = m.by_blueprint_prefix("Mount_Aquatic_Shoalskipper");
    if (!dawn || !bird || !shoal) {
        CHECK(false, "(the mounts are in the book)");
        return;
    }
    Rig fly(m, *dawn), ground(m, *bird), water(m, *shoal);
    {
        Flight f(fly, 0, 0, 10000);
        f.run(3000.0, 0, 0, 2.0);
        CHECK(f.refused >= 10 && f.last_fault == MoveFault::TooFastInAir && f.average() < 1290.0 * 1.5 + 60.0, "a flying speed hack (3,000 cm/s level, no dive before) is refused as too fast in the air, and gets nowhere faster than 1.5 x the cruise");
    }
    {
        Flight f(fly, 0, 0, 10000);
        f.run(1290.0, 0, 0, 1.0);
        const size_t before = f.refused;
        f.step(0, 0, 5000.0, 0.07, repl::kMoveFlying, 0);
        CHECK(f.refused == before + 1 && f.last_fault == MoveFault::TooFastInAir, "a vertical teleport (+5,000 cm in 0.07 s) is refused");
        f.step(0, 0, -5000.0, 0.07, repl::kMoveFlying, -90);
        CHECK(f.refused == before + 2, "and 5,000 cm straight down with the view pitched 90 degrees down: the dive is a ramp (2,400 cm/s^2), not a switch");
    }
    {
        // a fake dive pitch: a view pitched 90 degrees down raises the ceiling to 7,500, but the allowance climbs only 2,400 cm/s^2 towards it, so the hack gains a ramp, not a
        // switch (the client's real direction of travel cannot be asked for more: in the captures it lags the view by up to 40 degrees and even opposes it in a pull-out)
        Flight f(fly, 0, 0, 10000);
        const size_t before = f.refused;
        f.step(5000.0 * 0.07, 0, 0, 0.07, repl::kMoveFlying, -90.0);
        f.step(5000.0 * 0.07, 0, 0, 0.07, repl::kMoveFlying, -90.0);
        CHECK(f.refused > before, "5,000 cm/s level under a faked 90-degree pitch is refused at once");
        Flight g(fly, 0, 0, 10000);
        for (int i = 0; i < 60; ++i) g.step(5000.0 * 0.07, 0, 0, 0.07, repl::kMoveFlying, -90.0);
        const double bound = 1.1 * (1290.0 * g.elapsed + 0.5 * 4096.0 * g.elapsed * g.elapsed) + 100.0;
        std::printf("    (4.2 s of 5,000 cm/s under a faked pitch: %zu refused, the client got %.0f cm, the ramp's bound is %.0f)\n", g.refused, g.travelled(), bound);
        CHECK(g.refused >= 1 && g.travelled() <= bound, "and held to the climb the client can make: it gets no further than 1.1 x (cruise x t + 2,048 x t^2)");
    }
    {
        Flight f(ground, 0, 0, 10000);
        f.step(60, 0, 0, 0.07, repl::kMoveFlying, 0);
        CHECK(f.refused == 1 && f.last_fault == MoveFault::ModeNotAllowed, "movement mode 5 on a ground mount is refused (ModeNotAllowed)");
        Flight a(water, 0, 0, 10000);
        a.step(60, 0, 0, 0.07, repl::kMoveFlying, 0);
        CHECK(a.refused == 1 && a.last_fault == MoveFault::ModeNotAllowed, "and on an aquatic mount");
        // ... and the pawn's own checker has no such rule (a Cleric's Wings of Salvation): it judges mode 5 like any other, by the old horizontal rules
        MoveChecker pawn(MoveLimits{}, GoodPosition{});
        CHECK(pawn.check(move_at(1.0f, 0, 0, 0, repl::kMoveFlying), t0).accepted, "(the pawn's checker, without `air`, does not refuse mode 5: the rule is the mount's)");
    }
    {
        // swimming: an aquatic mount's 920 is not a ground mount's
        Flight w(water, 0, 0, 1000);
        w.run(920.0, 0, 0, 5.0, repl::kMoveSwimming);
        CHECK(w.refused == 0, "an aquatic mount swims at its 920 cm/s (mode 4)");
        Flight w2(water, 0, 0, 1000);
        w2.run(1400.0, 0, 0, 3.0, repl::kMoveSwimming);
        CHECK(w2.refused >= 10 && w2.average() < 920.0 * 1.1 + 60.0, "and not at 1,400");
        Flight g(ground, 0, 0, 1000);
        g.run(920.0, 0, 0, 3.0, repl::kMoveSwimming);
        CHECK(g.refused >= 10 && g.average() < 400.0 * 1.1 + 60.0, "a ground mount claiming mode 4 at 920 cm/s is refused: its water speed is its Blueprint's 400");
        Flight g2(ground, 0, 0, 1000);
        g2.run(400.0, 0, 0, 3.0, repl::kMoveSwimming);
        CHECK(g2.refused == 0, "(a ground mount that really swims, at its 400, is not refused)");
        // the swim is 3D: a climb of 900 cm/s with 700 along is 1,140: past 920
        Flight v(water, 0, 0, 1000);
        v.run(1400.0, 52.0, 0, 3.0, repl::kMoveSwimming);
        CHECK(v.refused >= 10 && v.average() < 920.0 * 1.1 + 60.0, "the water speed is a 3D speed: 1,100 cm/s up with 860 along (1,400) is refused, with the climb counted in");
        Flight v2(water, 0, 0, 1000);
        v2.run(920.0, 45.0, 0, 3.0, repl::kMoveSwimming);
        CHECK(v2.refused == 0, "while 920 cm/s on a slant is fine");
    }
    {
        // a mount's walking limit is its own: 990 x 1.5 on the ground, whatever the air rules say
        Flight w(ground, 0, 0, 1000);
        w.run(1400.0, 0, 0, 2.0, repl::kMoveWalking);
        CHECK(w.refused == 0, "(the ground mount's walking limit is unchanged: 1,400 cm/s passes)");
        Flight w2(ground, 0, 0, 1000);
        w2.run(1800.0, 0, 0, 3.0, repl::kMoveWalking);
        CHECK(w2.refused >= 5 && w2.average() < 990.0 * 1.5 + 60.0, "(and 1,800 cm/s does not: held to 990 x 1.5)");
    }
}
// (C.O.A ~ C-Y-B)

// ---- review A: the credits and the claimed pitch ------------------------------------------------------------------------------------------

/// The most a faked pitch on a level path may sustain, in cruises (AirLimits::pitch_trust 25 degrees: the ceiling at 25 degrees is 2.3 cruises and the sustained pace 1.1 x it; the goldens 871 and 883 need 20 to have no move beyond the law at all).
static constexpr double kFakePitchSustainedMax = 2.8;

static void test_review_a(const Mounts& m) {
    std::printf("review A: alternating modes (C1), a faked pitch on a level path (I2)\n");
    const content::MountRow* dawn = m.by_blueprint_prefix("Mount_Flying_Dawnbreaker");
    const content::MountRow* shoal = m.by_blueprint_prefix("Mount_Aquatic_Shoalskipper");
    if (!dawn || !shoal) {
        CHECK(false, "(the mounts are in the book)");
        return;
    }
    Rig fly(m, *dawn), water(m, *shoal);
    // C1: a 3D move (flying, swimming) refilled the vertical credits to the full burst, so [3D move, climb of 500 cm on the ground rule] pairs climbed 500 cm each, at no cost in time.
    for (const bool flying : {true, false}) {
        Flight f(flying ? fly : water, 0, 0, 10000);
        f.step(0, 0, 0, 0.07, repl::kMoveWalking, 0);
        const uint8_t mode = flying ? repl::kMoveFlying : repl::kMoveSwimming;
        size_t refused = 0;
        for (int i = 0; i < 300; ++i) {
            f.step(0, 0, 0, 0.001, mode, 0);
            if (!f.step(0, 0, 500.0, 0.001, repl::kMoveWalking, 0)) ++refused;
        }
        const double climbed = f.z - f.z0;
        const double bound = 1350.0 * f.elapsed + 500.0 + 100.0;
        char msg[200];
        std::snprintf(msg, sizeof(msg), "%s: 300 pairs of (mode %d standing still, 500 cm up on the ground) in %.2f s climb %.0f cm, at most %.0f (%zu refused)", flying ? "flying" : "swimming",
                      static_cast<int>(mode), f.elapsed, climbed, bound, refused);
        CHECK(climbed <= bound && refused > 250, msg);
    }
    // I2: the view's pitch is a claim.  8,000 cm/s on a LEVEL path under a faked -90 degree pitch, for 20 s: the ceiling the pitch gives counts only to `pitch_trust` degrees beyond the path
    {
        Flight f(fly, 0, 0, 10000);
        f.run(1290.0, 0, 0, 1.0);
        const double x_at = f.x, t_at = f.elapsed;
        for (int i = 0; i < 300; ++i) f.step(8000.0 * 0.07, 0, 0, 0.07, repl::kMoveFlying, -90.0);
        const double sustained = (f.x - x_at) / (f.elapsed - t_at);
        const double cruise = 1290.0;
        std::printf("    (20 s of a faked pitch on a level path: %zu refused, sustained %.0f cm/s = %.2f x the cruise)\n", f.refused, sustained, sustained / cruise);
        CHECK(sustained < cruise * kFakePitchSustainedMax, "a faked pitch on a level path buys no sustained speed beyond the trusted share of the dive's ceiling");
    }
    {
        // ... and the same pitch with a path that backs it is a dive: straight down at the pitch, the ceiling is the whole 7,500
        Flight f(fly, 0, 0, 350000);
        f.run(1290.0, 0, 0, 1.0);
        double v = 1290.0, top = 0;
        float last_ts = f.ts;
        double last_z = f.z;
        for (int i = 0; i < 100; ++i) {
            v = std::min(7000.0, v + 3500.0 * 0.07);
            const size_t before = f.refused;
            f.step(0, 0, -v * 0.07, 0.07, repl::kMoveFlying, -90.0);
            if (f.refused == before) {
                top = std::max(top, (last_z - f.z) / (f.ts - last_ts));
                last_ts = f.ts;
                last_z = f.z;
            }
        }
        CHECK(top > 6500.0, "(control: a real dive straight down under -90 degrees still reaches MaxFlyFallSpeed)");
    }
}

// ---- stage 4 fix 1: momentum, gravity, and corrections for gross violations only --------------------------------------------------------
// [C_Y_B + C.O.A]

/// The user test 2 (the PTR Eagle Gryphon, run_20261009_010314): the refused moves were 141 cm in 0.074 s (1,905 cm/s), 134 cm in 0.070 s (1,914) and 129 cm in 0.081 s (1,593) in
/// movement mode 5, aiming down and then levelling out; the stage-3 build judged them "along the ground" against 1,743.  Here the same moves, in place: a cruise, a dive at 10 degrees to
/// 1,905, and the levelling out with the speed still carried (it coasts down at 2,500 cm/s^2, a third of what the client brakes at).
static void test_the_users_dive(const Mounts& m) {
    std::printf("stage 4 fix 1: the user's dive and level-out (the logged moves), momentum, gravity\n");
    const content::MountRow* g = m.by_item("Mount_Flying_PTR_ONLY");
    if (!g) {
        CHECK(false, "(the Eagle Gryphon is in the book)");
        return;
    }
    Rig rig(m, *g);
    {
        Flight f(rig, -804000, 621000, 16000);
        f.run(1290.0, 0, 0, 3.0, repl::kMoveFlying, 0.074);
        double v = 1290.0;
        const double down = 10.4 * 3.14159265358979323846 / 180.0;
        for (int i = 0; i < 10; ++i) {   // aiming down: the speed climbs at the client's 1,600 cm/s^2 to the 1,905 of the log
            v = std::min(1905.0, v + 1600.0 * 0.074);
            f.step(v * 0.074 * std::cos(down), 0, -v * 0.074 * std::sin(down), 0.074, repl::kMoveFlying, -10.4);
        }
        CHECK(f.refused == 0 && v > 1900.0, "the dive to 1,905 cm/s (141 cm in 0.074 s, the first refused move of the log) passes");
        f.step(134.0, 0, -134.0 * std::tan(down), 0.070, repl::kMoveFlying, -10.4);
        CHECK(f.refused == 0, "134 cm in 0.070 s (1,914 cm/s, the second) passes");
        // levelling out: the view comes back to level, the speed is carried and decays at 2,500 cm/s^2
        size_t level_refused = 0;
        for (int i = 0; i < 10; ++i) {
            v = std::max(1290.0, v - 2500.0 * 0.081);
            const size_t before = f.refused;
            f.step(v * 0.081, 0, 0, 0.081, repl::kMoveFlying, 0.0);
            level_refused += f.refused - before;
        }
        CHECK(level_refused == 0, "levelling out at high speed (the carried 1,905 cm/s coasting down, the third refused move of the log among it) is never corrected");
    }
    {
        // the same on the checker without the stage-4 air limits (the pawn's, or any mount whose air rules are not on): mode 5 is not judged along the ground
        MoveChecker plain(MoveLimits{}, GoodPosition{});
        plain.set_max_speed(1743.0);
        plain.check(move_at(1.0f, 0, 0, 0, repl::kMoveFlying), t0);
        const MoveVerdict w = plain.check(move_at(1.074f, 5000.0, 0, 0, repl::kMoveFlying), t0 + std::chrono::milliseconds(74));
        CHECK(!w.accepted && w.fault == MoveFault::TooFastInAir, "mode 5 is never judged 'along the ground': a mode-5 teleport is refused as too fast in the air");
    }
    {
        // gravity in flight: the move descends steeply, the view is level; the speed climbs 1,100 cm/s^2 (gravity 1,274 x the steepness) - a free fall from the cruise
        Flight f(rig, 0, 0, 100000);
        double v = 1290.0;
        const double slope = 60.0 * 3.14159265358979323846 / 180.0;
        for (int i = 0; i < 28; ++i) {
            v = std::min(3500.0, v + 1100.0 * 0.07);
            f.step(v * 0.07 * std::cos(slope), 0, -v * 0.07 * std::sin(slope), 0.07, repl::kMoveFlying, 0.0);
        }
        CHECK(f.refused == 0 && v > 3400.0, "a steep fall with the view level (gravity helps the climb of the speed) is accepted up to 3,500 cm/s");
        Flight h(rig, 0, 0, 100000);
        double w = 1290.0;
        for (int i = 0; i < 40; ++i) {
            w = std::min(3500.0, w + 1100.0 * 0.07);
            h.step(w * 0.07, 0, 0, 0.07, repl::kMoveFlying, 0.0);
        }
        std::printf("    (level climb of the speed: %zu refused, %zu accepted)\n", h.refused, h.accepted);
        CHECK(h.refused >= 1, "while the same climb of the speed in level flight (no descent, no dive) is corrected: it is not the client's physics");
    }
    {
        // the client acceleration may be a good deal more than the 1,600 cm/s^2 of the captures (MaxAcceleration 2,048, the dash scales things): 3,000 cm/s^2 down to 3,671
        Flight f(rig, 0, 0, 100000);
        double v = 1290.0;
        for (int i = 0; i < 20; ++i) {
            v = std::min(3671.0, v + 3000.0 * 0.07);
            f.step(v * 0.07 * std::cos(0.6), 0, -v * 0.07 * std::sin(0.6), 0.07, repl::kMoveFlying, -34.5);
        }
        CHECK(f.refused == 0, "a dive that climbs at 3,000 cm/s^2 (twice the retail 1,600, inside the client's physics) is accepted");
    }
    {
        // a coast: after the dive the speed decays at a third of the client's braking; the carried speed outlives the pitch
        Flight f(rig, 0, 0, 100000);
        double v = 1290.0;
        for (int i = 0; i < 40; ++i) {
            v = std::min(3671.0, v + 1600.0 * 0.07);
            f.step(v * 0.07 * std::cos(0.45), 0, -v * 0.07 * std::sin(0.45), 0.07, repl::kMoveFlying, -34.5);
        }
        for (int i = 0; i < 20; ++i) {
            v = std::max(1290.0, v - 2500.0 * 0.07);
            f.step(v * 0.07, 0, 0, 0.07, repl::kMoveFlying, 0.0);
        }
        CHECK(f.refused == 0, "after a dive to 3,671 the carried speed coasts down at 2,500 cm/s^2 with the view level: never corrected");
    }
}

/// Borderline moves are logged, gross ones corrected.
static void test_gross_only(const Mounts& m) {
    std::printf("stage 4 fix 1: only gross violations are corrected\n");
    const content::MountRow* dawn = m.by_blueprint_prefix("Mount_Flying_Dawnbreaker");
    if (!dawn) return;
    Rig rig(m, *dawn);
    {
        Flight f(rig, 0, 0, 10000);
        f.run(1290.0 * 1.2, 0, 0, 1.0);
        CHECK(f.refused == 0 && f.last_verdict.accepted && f.last_verdict.borderline && f.last_verdict.soft_allowed > 0.0 && f.last_verdict.step > f.last_verdict.soft_allowed,
              "a move over the plain law (1.2 x the cruise) is accepted and flagged with the law's number (`borderline`, `soft_allowed`)");
        Flight g(rig, 0, 0, 10000);
        g.run(1290.0, 0, 0, 1.0);
        CHECK(!g.last_verdict.borderline, "a move inside the law is not flagged");
    }
    {
        // the physical maximum: MaxFlyFallSpeed x MoveSpeedMult x 1.1 = 8,250; a straight dive climbs at 3,500 cm/s^2 (gravity included) to 7,500 and goes on to 12,000
        Flight f(rig, 0, 0, 350000);
        double v = 1290.0, top = 0, top_refused_at = 0;
        float last_ts = f.ts;
        double last_z = f.z;
        for (int i = 0; i < 160; ++i) {
            v = std::min(12000.0, v + 3500.0 * 0.07);
            const size_t before = f.refused;
            f.step(0, 0, -v * 0.07, 0.07, repl::kMoveFlying, -90.0);
            if (f.refused == before) {
                top = std::max(top, (last_z - f.z) / (f.ts - last_ts));
                last_ts = f.ts;
                last_z = f.z;
            } else {
                top_refused_at = std::max(top_refused_at, v);
            }
        }
        std::printf("    (straight dive: fastest accepted step %.0f cm/s, %zu refused, refusals up to a claimed %.0f)\n", top, f.refused, top_refused_at);
        CHECK(top > 6500.0 && top <= 7500.0 * 1.1 + 50.0 / 0.07, "a straight dive may reach MaxFlyFallSpeed (7,500) and no more than 1.1 x it (and the 50 cm slack)");
        CHECK(f.refused >= 1 && top_refused_at > 8250.0, "... and a speed above that physical maximum is refused");
        Flight h(rig, 0, 0, 350000);
        h.run(1290.0, 0, 0, 1.0);
        h.step(0, 0, -9000.0 * 0.07, 0.07, repl::kMoveFlying, -90.0);
        CHECK(h.last_verdict.accepted == false && h.last_fault == MoveFault::TooFastInAir, "9,000 cm/s straight down out of the cruise is refused at once");
    }
    {
        // the sustain window: 1.45 x the cruise level is inside the physics (1.5 x) but 1.45 / 1.3 over the law: flagged and counted, corrected after 2.5 s, and then every move
        Flight f(rig, 0, 0, 10000);
        size_t flagged = 0;
        double first_refusal_at = -1;
        for (int i = 0; i < 200; ++i) {
            f.step(1290.0 * 1.45 * 0.07, 0, 0, 0.07, repl::kMoveFlying, 0.0);
            if (f.last_verdict.accepted && f.last_verdict.borderline) ++flagged;
            if (!f.last_verdict.accepted && first_refusal_at < 0) first_refusal_at = f.elapsed;
        }
        std::printf("    (1.45 x level for 14 s: %zu refused, %zu flagged, first refusal at %.2f s, average %.0f cm/s)\n", f.refused, flagged, first_refusal_at, f.average());
        CHECK(f.refused >= 1 && f.last_fault == MoveFault::TooFastInAirForLong && flagged >= 10 && first_refusal_at > 2.3 && first_refusal_at < 3.0,
              "1.45 x the cruise level is flagged for 2.5 s and then corrected (the sustain window)");
        CHECK(f.average() < 1290.0 * 1.3, "and after it the client is held to the law: its average pace over 14 s is below 1.3 x the cruise");
    }
    {
        // a ramp at a legitimate pace that does not stop at the law: held where the physics ends (1.5 x), never at 5,000
        Flight f(rig, 0, 0, 10000);
        double v = 1290.0;
        for (int i = 0; i < 200; ++i) {
            v = std::min(5000.0, v + 2000.0 * 0.07);
            f.step(v * 0.07, 0, 0, 0.07, repl::kMoveFlying, 0.0);
        }
        std::printf("    (a level ramp to 5,000: %zu refused, average %.0f cm/s)\n", f.refused, f.average());
        CHECK(f.refused >= 20 && f.average() < 1290.0 * 1.5, "a level ramp to 5,000 cm/s is held below 1.5 x the cruise (nothing earned it)");
    }
}
// (C Y B ~ C O A)

// ---- health ----------------------------------------------------------------------------------------------------------------------------

static void test_health(const Mounts& m) {
    std::printf("flyers' Health: x5 of Mount_Base_HP, x10 for the Drake of the Canopy and the Thundering Bolt Beast (the item texts)\n");
    size_t flyers = 0, x5 = 0, x10 = 0, wrong = 0;
    std::set<std::string> tens;
    for (const content::MountRow& r : m.book->data().mounts) {
        if (r.kind != "flying") continue;
        ++flyers;
        const double base = m.book->max_health(content::MountRow{}, 30);   // (a row with a factor of 1: Mount_Base_HP at level 30)
        const double hp = m.book->max_health(r, 30);
        if (std::fabs(hp - base * 5.0) < 0.5) ++x5;
        else if (std::fabs(hp - base * 10.0) < 0.5) {
            ++x10;
            tens.insert(r.item_name);
        } else {
            ++wrong;
        }
    }
    CHECK(flyers >= 46 && wrong == 0, "every flying mount has x5 or x10 Health");
    CHECK(x10 == 3 && tens.count("Mount_Flying_DrakeOfTheCanopy_0") == 1 && tens.count("Mount_Flying_ThunderingBoltBeast_0") == 1 && tens.count("Mount_Flying_ThunderingBoltBeast_1") == 1,
          "x10 is the Drake of the Canopy and the Thundering Bolt Beast (3 items), x5 the other 43");
    CHECK(x5 == flyers - 3, "(x5 for all the rest)");
}

int main() {
    Mounts m;
    test_goldens(m);
    test_the_law(m);
    test_the_gryphon(m);
    test_exploits(m);
    test_review_a(m);
    test_the_users_dive(m);
    test_gross_only(m);
    test_health(m);
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
