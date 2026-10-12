// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_npc_ai.cpp  (plan E1, stage 3, part 1)
//
//  The NPC's fight on a simulated clock, with no world: the threat table (npc/ai/threat.h) and the state machine (npc/ai/npc_ai.h).
//
//    threat    damage adds, a heal or time never subtracts below 0, the top is picked, a challenger needs 110 % of the current target's to take it, a
//              removed attacker is gone, the table is bounded
//    aggro     a hostile living player inside the aggro radius and in sight starts the fight; outside it, behind a wall, not hostile or dead it does not;
//              being hurt from afar starts it too
//    chase     it walks to the target, repaths only when the target moved or the interval passed, and attacks when the target is within range
//    attack    it stays in range until the target is farther than the range times the hysteresis
//    leash     farther than the leash radius from home it gives up; so does an empty table (the target left or died)
//    reset     it walks home at a run; when it is there it is idle again and says so once
//    dead      it thinks no more; a revive starts it afresh
//    hostile   NaN threat, a table full of attackers, thinking without a clock that moves, a target that is not in `known`
// ============================================================================
#include "npc/ai/npc_ai.h"
#include "npc/ai/threat.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace aoc::worldd;
using namespace aoc::worldd::npc::ai;
using combat::Vec3;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

namespace {

void test_threat() {
    std::printf("the threat table\n");
    ThreatTable t;
    CHECK(t.empty() && t.pick(0) == 0 && t.of(1) == 0 && !t.has(1), "an empty table has no target");
    CHECK(t.add(1, 10, 1.0) && t.add(2, 5, 1.0), "attackers are entered");
    CHECK(t.pick(0) == 1 && t.of(1) == 10 && t.of(2) == 5, "the one with the most threat is the target");
    t.add(2, 4.0, 2.0);   // 9 against 10
    CHECK(t.pick(1) == 1, "9 against 10: the target stays");
    t.add(2, 1.5, 3.0);   // 10.5 against 10: not yet 110 %
    CHECK(t.pick(1) == 1, "10.5 against 10 is not 110 %: it does not switch");
    t.add(2, 1.5, 4.0);   // 12 against 10
    CHECK(t.pick(1) == 2, "12 against 10 is: it switches");
    CHECK(t.pick(0) == 2, "with no current target the top is taken at once");
    t.add(1, 100, 5.0);
    CHECK(t.pick(2) == 1, "a big hit takes the target back");
    t.add(1, -1000, 6.0);
    CHECK(t.of(1) == 0, "threat never goes below 0");
    t.remove(1);
    CHECK(!t.has(1) && t.pick(1) == 2, "a removed attacker is gone: the current target that is no longer in the table is replaced");
    CHECK(!t.add(3, std::nan(""), 7.0) && !t.has(3), "a NaN threat is refused");
    t.clear();
    bool all = true;
    for (uint64_t i = 1; i <= ThreatTable::kMaxEntries; ++i) all = all && t.add(i, 1.0, 1.0);
    CHECK(all && !t.add(1000, 1.0, 2.0) && t.size() == ThreatTable::kMaxEntries, "the table is bounded: a stranger does not get in when it is full");
    CHECK(t.add(1, 5.0, 3.0), "...but a known attacker still adds");
    // a tie goes to the first
    ThreatTable tie;
    tie.add(7, 3.0, 1.0);
    tie.add(8, 3.0, 1.0);
    CHECK(tie.pick(0) == 7, "a tie goes to the one that came first");
}
// (CoA : C_Y_B)

Sense make_sense(double now, Vec3 self, Vec3 home, std::vector<Candidate> known, double range = 200.0) {
    Sense s;
    s.now = now;
    s.self = self;
    s.home = home;
    s.known = std::move(known);
    s.attack_range_cm = range;
    return s;
}

Candidate player(uint64_t id, double x, double y, bool alive = true, bool hostile = true) {
    Candidate c;
    c.id = id;
    c.pos = {x, y, 0};
    c.alive = alive;
    c.hostile = hostile;
    return c;
}

void test_aggro() {
    std::printf("aggro\n");
    Params p;
    p.aggro_radius_cm = 800;
    NpcAi ai(p);
    const Vec3 home{0, 0, 0};
    Orders o = ai.think(make_sense(0.0, home, home, {player(1, 2000, 0)}));
    CHECK(ai.state() == State::Idle && !o.in_combat && o.target == 0 && !o.began_fight, "a player 20 m away: idle");
    o = ai.think(make_sense(0.3, home, home, {player(1, 700, 0)}));
    CHECK(ai.state() == State::Chase && o.began_fight && o.in_combat && o.target == 1 && o.move == Orders::Move::ChaseTo, "7 m away: it aggros and chases");
    // not hostile, dead, behind a wall
    NpcAi friendly(p);
    friendly.think(make_sense(0.0, home, home, {player(1, 300, 0, true, false)}));
    CHECK(friendly.state() == State::Idle, "a player that is no enemy is left alone");
    NpcAi dead(p);
    dead.think(make_sense(0.0, home, home, {player(1, 300, 0, false)}));
    CHECK(dead.state() == State::Idle, "a dead player is left alone");
    NpcAi walled(p);
    Sense s = make_sense(0.0, home, home, {player(1, 300, 0)});
    s.line_of_sight = [](const Vec3&, const Vec3&) { return false; };
    walled.think(s);
    CHECK(walled.state() == State::Idle, "a player behind a wall is not noticed");
    s.now = 0.5;
    s.line_of_sight = [](const Vec3&, const Vec3&) { return true; };
    walled.think(s);
    CHECK(walled.in_combat(), "...and is when the wall is gone");
    // the radius includes the other body's radius
    NpcAi edge(p);
    Candidate c = player(1, 850, 0);
    c.radius_cm = 100;
    edge.think(make_sense(0.0, home, home, {c}));
    CHECK(edge.state() == State::Chase, "the player's own radius counts: 850 cm away with a body of 100 is inside 800 + 100");
    // being hurt from afar starts the fight
    NpcAi hurt(p);
    hurt.notice(5, 20.0, 0.0);
    o = hurt.think(make_sense(0.1, home, home, {player(5, 3000, 0)}));
    CHECK(hurt.state() == State::Chase && o.began_fight && o.target == 5, "a hit from 30 m away starts the fight");
    // the sense is rate limited: a player that steps in between two looks is seen at the next
    NpcAi slow(p);
    slow.think(make_sense(0.0, home, home, {player(1, 2000, 0)}));
    slow.think(make_sense(0.1, home, home, {player(1, 300, 0)}));
    CHECK(slow.state() == State::Idle, "an idle NPC looks every sense interval, not every call");
    slow.think(make_sense(0.3, home, home, {player(1, 300, 0)}));
    CHECK(slow.in_combat(), "...and sees the player at the next look");
    // beyond the leash proximity does nothing
    NpcAi far(p);
    far.think(make_sense(0.0, {5000, 0, 0}, home, {player(1, 5100, 0)}));
    CHECK(far.state() == State::Idle, "an NPC farther than its leash from home does not take a new fight");
}
// C.O.A with Cyb

void test_fight_and_leash() {
    std::printf("chase, attack, leash and reset\n");
    Params p;
    p.aggro_radius_cm = 800;
    p.leash_radius_cm = 3000;
    NpcAi ai(p);
    const Vec3 home{0, 0, 0};
    double now = 0;
    Vec3 self = home;
    double px = 600;
    // chase: the first order walks to the player
    Orders o = ai.think(make_sense(now, self, home, {player(1, px, 0)}, 200.0));
    CHECK(o.move == Orders::Move::ChaseTo && o.dest.x == 600 && o.speed_cm_s == p.chase_speed_cm_s, "the first order is a walk to the player at the chase speed");
    now += 0.1;
    o = ai.think(make_sense(now, self, home, {player(1, px, 0)}, 200.0));
    CHECK(o.move == Orders::Move::None && o.in_combat && !o.attack, "100 ms later and the player did not move: no new order");
    now += 0.6;
    o = ai.think(make_sense(now, self, home, {player(1, px, 0)}, 200.0));
    CHECK(o.move == Orders::Move::ChaseTo, "after the repath interval it orders again");
    now += 0.1;
    o = ai.think(make_sense(now, self, home, {player(1, px + 400, 0)}, 200.0));
    CHECK(o.move == Orders::Move::ChaseTo && o.dest.x == 1000, "a player that moved 4 m is followed at once");
    // in range: attack, standing
    self = {850, 0, 0};
    px = 1000;
    now += 0.1;
    o = ai.think(make_sense(now, self, home, {player(1, px, 0)}, 200.0));
    CHECK(ai.state() == State::Attack && o.attack && o.move == Orders::Move::Stand, "the player 150 cm away (surface 50): attack, standing");
    // hysteresis: 220 cm of surface distance is within 200 * 1.15 once attacking
    now += 0.1;
    o = ai.think(make_sense(now, self, home, {player(1, 850 + 220 + 84, 0)}, 200.0));
    CHECK(ai.state() == State::Attack && o.attack, "a target a bit past the range does not end the attack (hysteresis)");
    now += 0.1;
    o = ai.think(make_sense(now, self, home, {player(1, 850 + 400, 0)}, 200.0));
    CHECK(ai.state() == State::Chase && !o.attack && o.move == Orders::Move::ChaseTo, "farther than that: it chases again");
    // the target dies: the table empties, it resets
    now += 0.1;
    o = ai.think(make_sense(now, self, home, {player(1, 900, 0, false)}, 200.0));
    CHECK(ai.state() == State::Reset && o.began_reset && o.move == Orders::Move::Home && !o.in_combat, "the target died: it resets and walks home");
    CHECK(ai.threat().empty() && ai.target() == 0, "the threat table is clear");
    // walking home
    now += 0.5;
    o = ai.think(make_sense(now, {400, 0, 0}, home, {}, 200.0));
    CHECK(ai.state() == State::Reset && o.move == Orders::Move::Home && o.speed_cm_s == p.run_home_speed_cm_s && !o.reset_done, "still walking home at a run");
    now += 0.5;
    o = ai.think(make_sense(now, {100, 0, 0}, home, {}, 200.0));
    CHECK(ai.state() == State::Idle && o.reset_done && o.move == Orders::Move::Release, "within 1.5 m of home it is idle again and says the reset is done once");
    now += 0.5;
    o = ai.think(make_sense(now, home, home, {}, 200.0));
    CHECK(!o.reset_done && o.move == Orders::Move::None, "...once");
    // a reset NPC does not take threat while it walks home
    NpcAi busy(p);
    busy.think(make_sense(0.0, home, home, {player(1, 300, 0)}));
    busy.think(make_sense(0.2, {4000, 0, 0}, home, {player(1, 4100, 0)}));
    CHECK(busy.state() == State::Reset, "past the leash it gives up even with the player in reach");
    busy.notice(1, 100.0, 0.3);
    CHECK(busy.threat().empty(), "a resetting NPC takes no new threat");
    // the leash while chasing
    NpcAi chase(p);
    chase.think(make_sense(0.0, home, home, {player(1, 700, 0)}));
    o = chase.think(make_sense(2.0, {3100, 0, 0}, home, {player(1, 3500, 0)}));
    CHECK(chase.state() == State::Reset && o.began_reset, "the player led it past the leash: reset");
    // the target left the area: not in `known` any more
    NpcAi gone(p);
    gone.think(make_sense(0.0, home, home, {player(1, 700, 0)}));
    o = gone.think(make_sense(1.0, {500, 0, 0}, home, {}));
    CHECK(gone.state() == State::Reset && o.began_reset, "a target that is not known any more is dropped from the table: reset");
    // two attackers: it keeps the first until the second has 110 %
    NpcAi two(p);
    two.think(make_sense(0.0, home, home, {player(1, 300, 0), player(2, 400, 0)}));
    two.notice(1, 50.0, 0.1);
    two.notice(2, 50.0, 0.1);
    o = two.think(make_sense(0.3, home, home, {player(1, 300, 0), player(2, 400, 0)}));
    const uint64_t first = o.target;
    two.notice(first == 1 ? 2 : 1, 4.0, 0.4);
    o = two.think(make_sense(0.5, home, home, {player(1, 300, 0), player(2, 400, 0)}));
    CHECK(o.target == first, "a challenger a little ahead does not take the target");
    two.notice(first == 1 ? 2 : 1, 100.0, 0.6);
    o = two.think(make_sense(0.7, home, home, {player(1, 300, 0), player(2, 400, 0)}));
    CHECK(o.target != first, "...a big one does");
}
// C | O | A - C:Y:B

void test_dead_and_hostile() {
    std::printf("death and hostile input\n");
    Params p;
    NpcAi ai(p);
    const Vec3 home{0, 0, 0};
    ai.think(make_sense(0.0, home, home, {player(1, 300, 0)}));
    CHECK(ai.in_combat(), "in combat");
    ai.kill();
    Orders o = ai.think(make_sense(1.0, home, home, {player(1, 100, 0)}));
    CHECK(ai.state() == State::Dead && !o.in_combat && !o.attack && o.move == Orders::Move::None && ai.threat().empty(), "a dead NPC thinks no more");
    ai.notice(1, 100.0, 2.0);
    CHECK(ai.threat().empty(), "it takes no threat");
    ai.revive();
    CHECK(ai.state() == State::Idle && ai.threat().empty(), "a revived NPC starts afresh");
    o = ai.think(make_sense(3.0, home, home, {player(1, 300, 0)}));
    CHECK(ai.in_combat(), "and fights again");
    // a clock that stands still or goes back changes nothing and breaks nothing
    NpcAi odd(p);
    for (int i = 0; i < 5; ++i) odd.think(make_sense(10.0 - i, home, home, {player(1, 5000, 0)}));
    CHECK(odd.state() == State::Idle, "a clock that goes back: still idle, nothing breaks");
    // NaN positions
    NpcAi nan(p);
    nan.think(make_sense(0.0, home, home, {player(1, std::nan(""), 0)}));
    CHECK(nan.state() == State::Idle, "a player at NaN is no one to fight");
    nan.notice(2, std::nan(""), 0.0);
    CHECK(nan.threat().empty(), "a NaN threat adds nothing");
    CHECK(std::string(state_name(State::Chase)) == "chase" && std::string(state_name(State::Reset)) == "reset", "the states have names");
}

}  // namespace

int main() {
    test_threat();
    test_aggro();
    test_fight_and_leash();
    test_dead_and_hostile();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
