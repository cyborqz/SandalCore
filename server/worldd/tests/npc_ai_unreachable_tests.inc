// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/npc_ai_unreachable_tests.inc: the creature's count of the time it has found no way to its target (included by test_worldd_npc_ai.cpp)
//
//  The game's creatures leash when the seconds they have spent trying to find a way to their target reach SecondsTryingToFindAPath: 5.0 s (the class's own
//  default; the trees of every creature but the guards' keep it), the count grows by a second for each second the way is missing and falls by a quarter of a
//  second for each second it is there, and the leash starts at once (the check's delay is 0 s).  What the tests hold: the 5 s, the fall, a creature whose
//  target is in reach again, and a count that is only as old as the last order that found no way.
// ============================================================================

namespace {

/// The creature of the walk below: a target 600 cm away that the world finds no way to (`reachable` false) or finds one (true), thought of every 0.2 s as the
/// combat world does; a walk is ordered at the AI's own rate and its result is told to the AI as the combat world tells it.
struct WayRun {
    Params   p;
    NpcAi    ai;
    Vec3     home{0, 0, 0};
    double   now = 0;
    bool     leashed = false;
    double   leashed_at = -1;
    std::string why;

    WayRun() : p(make_params()), ai(p) {}
    static Params make_params() {
        Params q;
        q.aggro_radius_cm = 800;
        q.leash_radius_cm = 30000;   // (the leash of distance is not what these tests are about)
        return q;
    }
    /// One look: `target_x` is where the player stands; `way` whether the walk ordered at this look finds a way.
    Orders look(double target_x, bool way) {
        const Orders o = ai.think(make_sense(now, home, home, {player(1, target_x, 0)}, 200.0));
        if (o.began_reset && !leashed) {
            leashed = true;
            leashed_at = now;
            why = o.reset_reason ? o.reset_reason : "";
        }
        if (o.move == Orders::Move::ChaseTo) {
            if (way) ai.path_to_target_found();
            else ai.no_path_to_target(now);
        }
        return o;
    }
    /// Runs the looks of `seconds` (every 0.2 s) with the target at `target_x` and the way as given; stops at the leash.
    void run(double seconds, double target_x, bool way) {
        const double end = now + seconds;
        while (now < end - 1e-9 && !leashed) {
            look(target_x, way);
            now += 0.2;
        }
    }
};

void test_nav_abuse() {
    std::printf("a target the world finds no way to: the creature's count of the seconds it has tried\n");
    {
        Params p;
        CHECK(p.unreachable_give_up_seconds == 5.0, "the creature gives up after 5.0 s: the check's SecondsTryingToFindAPath");
    }
    {   // no way, all the time: it keeps the fight for 5 s and then leashes
        WayRun w;
        w.run(4.6, 600, false);
        CHECK(!w.leashed && w.ai.in_combat() && w.ai.target() == 1, "4.6 s with no way: it still fights its target");
        w.run(1.0, 600, false);
        CHECK(w.leashed && w.leashed_at >= 4.8 && w.leashed_at <= 5.4, "5.0 s with no way: it leashes (within a look of it)");
        CHECK(w.why == "its target cannot be reached", "...and says why");
        CHECK(w.ai.state() == State::Reset && w.ai.threat().empty() && w.ai.target() == 0 && !w.ai.in_combat(), "the threat is cleared and it walks home");
    }
    {   // the way comes back for a while: the count falls by a quarter of a second each second, it does not start over
        WayRun w;
        w.run(2.0, 600, false);   // 2 s counted
        CHECK(!w.leashed, "2 s with no way: still fighting");
        w.run(4.0, 600, true);    // 4 s with a way: 2 - 4 x 0.25 = 1 s left
        CHECK(!w.leashed, "4 s with the way back: still fighting");
        w.run(5.0, 600, false);   // about 3.3 more seconds reach the 5 (the first walk ordered after the way is back is 0.6 s on: the count grows till then)
        CHECK(w.leashed && w.leashed_at >= 9.3 && w.leashed_at <= 10.5, "the way lost again: it leashes at about 9.6 s, not at 11.2 s (a count that started over) nor at 8.6 s (one that never fell)");
    }
    {   // a brief loss of the way is forgotten bit by bit
        WayRun w;
        w.run(1.0, 600, false);
        w.run(60.0, 600, true);
        w.run(4.0, 600, false);
        CHECK(!w.leashed, "1 s with no way a minute ago and 4 s now: it does not leash (the count fell back to nothing)");
    }
    {   // the target comes within reach after a walk found no way: no order is given any more, and the count does not run on
        WayRun w;
        w.run(1.0, 600, false);
        w.run(10.0, 150, true);   // the player is in reach (surface 50 cm): the creature attacks, no walk is ordered
        CHECK(!w.leashed && w.ai.in_combat() && w.ai.state() == State::Attack, "a target in reach is fought for 10 s after one failed walk: no leash");
    }
    {   // an order that found no way counts for a second: a creature that is not ordered to walk any more is not counted against
        WayRun w;
        w.look(600, false);       // the order at 0.0 found no way
        w.now = 0.2;
        w.run(30.0, 150, true);
        CHECK(!w.leashed, "one failed order and 30 s in reach: no leash");
    }
    {   // the count goes with the creature to its reset: a new fight starts from nothing
        WayRun w;
        w.run(10.0, 600, false);
        CHECK(w.leashed, "(the first fight ends in the leash)");
        // it walks home, is idle, and the player is there again: 4.6 s of no way do not leash it
        const Orders back = w.ai.think(make_sense(w.now + 0.2, w.home, w.home, {}, 200.0));
        CHECK(back.reset_done && w.ai.state() == State::Idle, "(it is home and idle)");
        w.now += 0.4;
        w.leashed = false;
        w.run(4.6, 600, false);
        CHECK(!w.leashed, "a new fight: 4.6 s with no way do not leash it");
    }
    {   // death and a revive clear it
        WayRun w;
        w.run(4.4, 600, false);
        w.ai.kill();
        w.ai.revive();
        w.run(4.0, 600, false);
        CHECK(!w.leashed, "a revived creature starts the count from nothing");
    }
}

}  // namespace
