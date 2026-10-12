// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_npc_move.cpp  (plan E1, stage 2)
//
//  The NPCs' movement on a simulated clock, over the small world of tests/nav_fixture.h (a plain with a hill, a wall, a floating platform and a
//  closed pen) and the registry's actors:
//
//    standing     an NPC is put on the ground (the terrain's exact height, a floor's, a hill's) one capsule half height up; no ground in reach: it
//                 stays where it was; the same inputs give the same world
//    dormant      nobody near: nothing moves, however long the clock runs; a player near wakes it; leaving every ring stops it (zero velocity written)
//    wander       it keeps within its radius of home, walks at its speed, on the ground all the way, idles between legs; the replicated values follow
//                 (ReplicatedMovement at 0.01 cm, whole cm/s, one yaw byte; the timestamp rises; CurrentMovementInput 1 while walking)
//    patrol       the waypoints in order and round (a loop), there and back (no loop), the pause at each; a route that cannot be walked is skipped
//    return home  an NPC sent home walks back at its run speed; an unreachable home or destination is refused and nothing moves
//    the service  order_move / release / order_return_home: what stage 3's AI calls
//    hostile      NaN and huge numbers, an unknown or non-NPC actor, a second add, a clock that goes back or jumps, 900 NPCs stepped (time)
//    no ground    without a navmesh nothing is taken and the NPCs stay where they are
// ============================================================================
#include "bootstrap/constants.h"
#include "nav_fixture.h"
#include "npc/area_npcs.h"
#include "npc/motion_table.h"
#include "npc/mover.h"
#include "npc/npc_world.h"
#include "protocol/repl/codecs.h"
#include "protocol/schema/generated/index.h"
#include "world/actor_registry.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace aoc::worldd;
namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;
namespace emit = aoc::protocol::emit;
using nav::Vec3;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

static const schema::ClassSchema& schema_named(const char* name) {
    const schema::ClassSchema* s = schema::gen::find_class_schema(name);
    if (!s) { std::printf("  [FAIL] no schema %s\n", name); std::exit(2); }
    return *s;
}

static emit::FIntrepidNetworkGUID guid(uint64_t id) {
    emit::FIntrepidNetworkGUID g;
    g.ObjectId = id;
    g.ServerId = 60;
    g.Randomizer = 0x6ee87ee4u;
    return g;
}
// -- C | Y | B x CoA --

static world::WorldActor make_npc(uint64_t id, double x, double y, double z, world::ActorKind kind = world::ActorKind::Npc) {
    world::WorldActor w;
    w.id = id;
    w.kind = kind;
    w.x = x;
    w.y = y;
    w.z = z;
    repl::Actor& a = w.actor;
    a.guid = guid(id);
    a.obj.schema = &schema_named("ANPCCharacter");
    repl::RepMovement m;
    m.location = repl::quantize_location(a.obj.opts, x, y, z);
    m.rotation = repl::Rot{0, 64, 0};
    m.linear_velocity = repl::QuantVec{repl::QuantVec::Mode::Scaled, 0, 0, 0};
    a.obj.state.set(*a.obj.schema, "ReplicatedMovement", repl::Value(m));
    a.obj.state.set(*a.obj.schema, "ReplicatedServerLastTransformUpdateTimeStamp", repl::Value::f32(0.f));
    a.obj.state.set(*a.obj.schema, "CurrentMovementInput", repl::Value(repl::PlayerMovementInput{0}));
    a.open.spawn.location = repl::quantize_vector(10, x, y, z);
    a.open.spawn.rotation = repl::Rot{0, 11000, 0};
    w.npc.name = "test npc";
    return w;
}

struct Rm {
    Vec3 loc;
    Vec3 vel;
    int  yaw8 = 0;
    float stamp = 0;
    int  input = -1;
};

static Rm read_rm(const world::WorldActor& w) {
    Rm r;
    const repl::ObjectState& o = w.actor.obj;
    if (const repl::Value* v = o.state.get(*o.schema, "ReplicatedMovement")) {
        if (const auto* m = v->get_if<repl::RepMovement>()) {
            const auto l = repl::dequantize_location(o.opts, m->location);
            const auto vel = repl::dequantize_velocity(o.opts, m->linear_velocity);
            r.loc = {l.x, l.y, l.z};
            r.vel = {vel.x, vel.y, vel.z};
            r.yaw8 = m->rotation.yaw;
        }
    }
    if (const repl::Value* v = o.state.get(*o.schema, "ReplicatedServerLastTransformUpdateTimeStamp")) {
        if (const auto* f = v->get_if<float>()) r.stamp = *f;
    }
    if (const repl::Value* v = o.state.get(*o.schema, "CurrentMovementInput")) {
        if (const auto* p = v->get_if<repl::PlayerMovementInput>()) r.input = p->bits;
    }
    return r;
}

static npc::NpcMotionSetup wander_setup(double radius = 1500.0) {
    npc::NpcMotionSetup s;
    s.ambient = npc::Ambient::Wander;
    s.foot_offset = 96.0;
    s.wander_min = 200.0;
    s.wander_max = radius;
    s.walk_speed = 150.0;
    s.run_speed = 300.0;
    s.wander_delay = 0.4;
    s.wander_delay_deviation = 0.1;
    return s;
}

/// Runs the world for `seconds` of simulated time at 15 Hz with a player noted at (px, py) every tick; calls `each(t)` after every tick.
template <class F>
static double run(npc::NpcWorld& w, double t0, double seconds, double px, double py, F&& each, bool player = true) {
    double t = t0;
    const double end = t0 + seconds;
    while (t < end) {
        t += 1.0 / 15.0;
        if (player) w.note_player(1, px, py, t);
        w.tick(t);
        each(t);
    }
    return t;
}

static double dist2(const world::WorldActor& a, const Vec3& b) { return std::hypot(a.x - b.x, a.y - b.y); }

// ---- standing ---------------------------------------------------------------------------------------------------------------------------
// C | O | A + C_Y_B

static void test_standing() {
    std::printf("standing\n");
    auto& n = navfix::shared_nav();
    world::ActorRegistry reg;
    reg.add(make_npc(1000, 1000, 1000, 1200));       // 100 cm too high, on the plain
    reg.add(make_npc(1002, 6000, 0, 800));           // the hill's top (1,400)
    reg.add(make_npc(1004, 9000, -5000, 1700));      // the platform (1,540)
    reg.add(make_npc(1006, 1000, 1000, 9000));       // nothing within 15 m of that height: the terrain under it is the ground
    reg.add(make_npc(1008, 500000, 0, 1000));        // far outside
    reg.add(make_npc(1010, 0, 0, 1000, world::ActorKind::Gatherable));
    npc::NpcWorld w(reg, &n);
    npc::NpcMotionSetup s = wander_setup();
    s.ambient = npc::Ambient::Stationary;
    bool found = false;
    CHECK(w.add(1000, s, &found) && found, "an NPC on the plain is added and found its ground");
    CHECK(std::fabs(reg.find(1000)->z - 1096.0) < 0.5, "it stands on the plain: ground 1,000 + the half height 96");
    const Rm r = read_rm(*reg.find(1000));
    CHECK(std::fabs(r.loc.z - 1096.0) < 0.02 && std::fabs(r.loc.x - 1000) < 0.02 && r.vel.x == 0 && r.input == 0, "ReplicatedMovement says so too (0.01 cm), standing");
    CHECK(reg.find(1000)->version > 1, "the change counted (the view sends it)");
    CHECK(w.add(1002, s, &found) && found && std::fabs(reg.find(1002)->z - (1400.0 + 96.0)) < 0.5, "on the hill: the terrain's height");
    CHECK(w.add(1004, s, &found) && found && std::fabs(reg.find(1004)->z - (1540.0 + 96.0)) < 12.0, "on the platform: its top (the navmesh's, within 10 cm)");
    CHECK(w.add(1006, s, &found) && found && std::fabs(reg.find(1006)->z - 1096.0) < 0.5,
          "a height no surface is near (a placement drawn at the wrong height): the terrain's is the ground, the NPC stands on the plain");
    CHECK(w.add(1008, s, &found) && !found, "far outside the terrain: no ground, stands");
    CHECK(!w.add(1010, s), "a gatherable is not an NPC");
    CHECK(!w.add(9999, s), "an unknown actor is refused");
    CHECK(!w.add(1000, s), "a second add is refused");
    npc::NpcMotionSetup nan = s;
    nan.walk_speed = std::nan("");
    reg.add(make_npc(1012, 0, 0, 1000));
    CHECK(!w.add(1012, nan), "a setup with a NaN is refused");
    nan = s;
    nan.foot_offset = 1e9;
    CHECK(!w.add(1012, nan), "a half height of 1e9 is refused");
    CHECK(w.size() == 5, "five NPCs under its care");
    // the first tick starts the NPCs' clock: their timestamps (the loader's 0) become the clock's, and nothing is sent for it
    const uint32_t version_before = reg.find(1000)->version;
    w.tick(5000.0);
    CHECK(read_rm(*reg.find(1000)).stamp == 5000.0f && reg.find(1000)->version == version_before, "the first tick sets every NPC's timestamp to the clock without a change to send");
}

// ---- an NPC whose data height is right ------------------------------------------------------------------------------------------------------

static void test_ground_by_data() {
    std::printf("an NPC that keeps its data height (a city node's, in a bowl)\n");
    auto& n = navfix::shared_nav();
    world::ActorRegistry reg;
    world::WorldActor bowl = make_npc(1100, 1000, 1000, 9000);   // 80 m above the plain: no surface within 15 m of it, as a town's floor over the baked ground of its bowl
    bowl.npc.ground_by_data = true;
    world::WorldActor reach = make_npc(1102, 1000, 1000, 1200);  // 100 cm too high, a surface within reach
    reach.npc.ground_by_data = true;
    reg.add(std::move(bowl));
    reg.add(std::move(reach));
    reg.add(make_npc(1104, 1000, 1000, 9000));                   // the open world's NPC at the same height: the terrain is the ground (unchanged)
    npc::NpcWorld w(reg, &n);
    npc::NpcMotionSetup s = wander_setup();
    bool found = true;
    CHECK(w.add(1100, s, &found) && !found, "out of reach of every surface: no ground was found for it");
    CHECK(std::fabs(reg.find(1100)->z - 9000.0) < 0.01 && std::fabs(read_rm(*reg.find(1100)).loc.z - 9000.0) < 0.02 && w.stands(1100),
          "...it keeps the height the data gave it (not the terrain's 1,096), here and in the replicated movement, and it stands (a wanderer has no navmesh to wander on)");
    CHECK(w.add(1102, s, &found) && found && std::fabs(reg.find(1102)->z - 1096.0) < 0.5, "a surface within reach of its height: it stands on it as any NPC does");
    CHECK(w.add(1104, s, &found) && found && std::fabs(reg.find(1104)->z - 1096.0) < 0.5, "the open world's NPC (no flag) at the first one's height: still moved to the terrain");
}

// ---- dormant ----------------------------------------------------------------------------------------------------------------------------

static void test_dormant() {
    std::printf("dormant and awake\n");
    auto& n = navfix::shared_nav();
    world::ActorRegistry reg;
    reg.add(make_npc(1000, 1000, 1000, 1096));
    npc::NpcWorld w(reg, &n);
    CHECK(w.add(1000, wander_setup()), "a wanderer is added");
    const double x0 = reg.find(1000)->x, y0 = reg.find(1000)->y;
    const uint32_t v0 = reg.find(1000)->version;
    double t = run(w, 0, 120, 0, 0, [](double) {}, /*player=*/false);
    CHECK(reg.find(1000)->x == x0 && reg.find(1000)->y == y0 && reg.find(1000)->version == v0, "no player anywhere: 120 s later it has not moved and nothing was written");
    // a player far away (more than the ring and the margin)
    t = run(w, t, 60, 100000, 100000, [](double) {});
    CHECK(reg.find(1000)->x == x0 && reg.find(1000)->y == y0, "a player 1.4 km away: still still");
    // a player near
    size_t moved = 0;
    t = run(w, t, 60, 1000, 1000 + 8000, [&](double) { moved += reg.find(1000)->x != x0 || reg.find(1000)->y != y0 ? 1 : 0; });
    CHECK(moved > 100, "a player 80 m away wakes it: it walks");
    // the player leaves: it stops at once and says so
    t = run(w, t, 1.0, 0, 0, [](double) {}, /*player=*/false);   // (the note expires after 2 s: leave the clock running without notes)
    t = run(w, t, 5.0, 0, 0, [](double) {}, /*player=*/false);
    const Rm r = read_rm(*reg.find(1000));
    CHECK(r.vel.x == 0 && r.vel.y == 0 && r.input == 0, "when the last player is gone it is still, and its last write says zero velocity and no input");
    const double px = reg.find(1000)->x, py = reg.find(1000)->y;
    t = run(w, t, 30, 0, 0, [](double) {}, /*player=*/false);
    CHECK(reg.find(1000)->x == px && reg.find(1000)->y == py, "and stays where it stopped");
    // woken again it resumes
    size_t moved_again = 0;
    run(w, t, 60, 1000, 1000 + 8000, [&](double) { moved_again += (reg.find(1000)->x != px || reg.find(1000)->y != py) ? 1 : 0; });
    CHECK(moved_again > 50, "a player back near wakes it again");
}

// ---- wander -----------------------------------------------------------------------------------------------------------------------------
// [C Y B ~ C | O | A]

static void test_wander() {
    std::printf("wander\n");
    auto& n = navfix::shared_nav();
    world::ActorRegistry reg;
    reg.add(make_npc(1000, 1000, 1000, 1096));
    npc::NpcWorld w(reg, &n);
    const npc::NpcMotionSetup s = wander_setup(1500.0);
    w.add(1000, s);
    const Vec3 home{1000, 1000, 1000};
    double max_dist = 0, worst_ground = 0, max_speed = 0, walk_time = 0, idle_time = 0, last_stamp = -1;
    size_t input_mismatch = 0, legs = 0, steps = 0;
    bool was_moving = false, stamp_back = false;
    double max_step = 0;
    double px = reg.find(1000)->x, py = reg.find(1000)->y;
    run(w, 0, 300, 1000, 1000 + 6000, [&](double) {
        const world::WorldActor& a = *reg.find(1000);
        max_dist = std::max(max_dist, dist2(a, home));
        double tz = 0;
        n.terrain_height(a.x, a.y, &tz);
        worst_ground = std::max(worst_ground, std::fabs(a.z - 96.0 - tz));   // (the wall is far from home: the ground here is the terrain)
        const double step = std::hypot(a.x - px, a.y - py);
        max_step = std::max(max_step, step);
        px = a.x; py = a.y;
        const Rm r = read_rm(a);
        const double sp = std::hypot(r.vel.x, r.vel.y);
        max_speed = std::max(max_speed, sp);
        const bool moving = sp > 1.0;
        (moving ? walk_time : idle_time) += 1.0 / 15.0;
        if (moving && !was_moving) ++legs;
        was_moving = moving;
        if ((moving && r.input != 1) || (!moving && r.input == 1 && step == 0)) ++input_mismatch;
        if (r.stamp < last_stamp - 1e-6f) stamp_back = true;
        last_stamp = r.stamp;
        ++steps;
    });
    std::printf("    300 s: %zu legs, walked %.0f s, idle %.0f s, farthest from home %.0f cm, top speed %.1f cm/s, biggest step %.1f cm\n", legs, walk_time, idle_time, max_dist, max_speed, max_step);
    CHECK(legs >= 8, "it walked at least eight legs in five minutes");
    CHECK(max_dist <= 1500.0 + 120.0 && max_dist > 600.0, "it kept within its radius of home (plus the navmesh's snap) and used much of it");
    CHECK(worst_ground < 1.5, "its feet were on the ground all the way: the origin was always the terrain's height + 96 (within 1.5 cm)");
    CHECK(max_speed > 140.0 && max_speed < 160.0, "it walked at its speed (150 cm/s, whole cm/s on the wire)");
    CHECK(max_step < 150.0 / 15.0 * 1.2, "and a step of one tick was at most its speed's (10 cm)");
    CHECK(idle_time > 20.0 && walk_time > 60.0, "and idled between legs");
    CHECK(input_mismatch == 0, "CurrentMovementInput was 1 while it walked");
    CHECK(!stamp_back && last_stamp > 290.0, "the timestamp only rose, up to the clock's");
    const Rm r = read_rm(*reg.find(1000));
    const double yaw = r.yaw8 * 360.0 / 256.0;
    (void)yaw;
    CHECK(std::fabs(r.loc.x - reg.find(1000)->x) < 0.01 && std::fabs(r.loc.y - reg.find(1000)->y) < 0.01 && std::fabs(r.loc.z - reg.find(1000)->z) < 0.01, "ReplicatedMovement's location is the registry's to 0.01 cm");
    CHECK(steps >= 4499 && steps <= 4501, "300 s of 15 Hz steps");
}

static void test_wander_determinism() {
    std::printf("determinism\n");
    auto& n = navfix::shared_nav();
    std::vector<std::vector<double>> traces;
    for (int run_no = 0; run_no < 2; ++run_no) {
        world::ActorRegistry reg;
        reg.add(make_npc(1000, 1000, 1000, 1096));
        reg.add(make_npc(1002, -1000, 3000, 1096));
        npc::NpcWorld w(reg, &n);
        w.add(1000, wander_setup());
        w.add(1002, wander_setup(800));
        std::vector<double> trace;
        run(w, 0, 90, 0, 0, [&](double) {
            trace.push_back(reg.find(1000)->x);
            trace.push_back(reg.find(1002)->y);
        });
        traces.push_back(trace);
    }
    CHECK(traces[0] == traces[1] && traces[0].size() == 2700, "the same world gives the same walks (a per-NPC random stream)");
}

/// The stage-2 fix: NPCs that can never find a path (a patrol whose waypoints lie in a closed pen) must not keep the others from being asked, and a
/// wander leg is the length the retail captures show (a median of 250 cm, 740 at the 90th percentile), turning back at the radius.
static void test_budget_and_legs() {
    std::printf("the path-finding budget and the length of a leg\n");
    auto& n = navfix::shared_nav();
    {
        world::ActorRegistry reg;
        npc::NpcWorldConfig cfg;
        cfg.max_pathfinds_per_step = 3;   // (a small budget: a few hopeless patrols used all of it, every step, in the draft)
        npc::NpcWorld w(reg, &n, cfg);
        std::vector<uint64_t> wanderers;
        for (int i = 0; i < 30; ++i) {
            const uint64_t id = 2000 + 2 * static_cast<uint64_t>(i);
            reg.add(make_npc(id, 500 + 40.0 * i, 800, 1096));
            npc::NpcMotionSetup s;
            if (i < 12) {
                // an unreachable route: the pen's (every waypoint is Blocked)
                s.ambient = npc::Ambient::Patrol;
                s.foot_offset = 96.0;
                s.route = {{-8400, 6600, 1000}, {-8500, 6600, 1000}, {-8600, 6600, 1000}};
                s.pauses = {0, 0, 0};
                s.patrol_speed = 200.0;
            } else {
                s = wander_setup(900);
                wanderers.push_back(id);
            }
            w.add(id, s);
        }
        std::set<uint64_t> walked;
        run(w, 0, 60, 1000, 1000, [&](double) {
            for (const uint64_t id : wanderers)
                if (std::hypot(reg.find(id)->x - (500 + 40.0 * ((id - 2000) / 2)), reg.find(id)->y - 800) > 100.0) walked.insert(id);
        });
        const auto st = w.stats();
        std::printf("    12 hopeless patrols and 18 wanderers, 3 path-finds a step: %zu wanderers walked, %llu patrol asks (%llu without a path), %llu wander asks, %llu deferred\n", walked.size(),
                    static_cast<unsigned long long>(st.patrol_asks), static_cast<unsigned long long>(st.patrol_no_path), static_cast<unsigned long long>(st.wander_asks),
                    static_cast<unsigned long long>(st.wander_deferred));
        CHECK(st.patrol_no_path > 0, "(control) the patrols found no path");
        CHECK(walked.size() == wanderers.size(), "every wanderer was asked and walked, whatever the patrols spent: the budget goes round");
    }
    {
        world::ActorRegistry reg;
        reg.add(make_npc(1000, 1000, 1000, 1096));
        npc::NpcWorld w(reg, &n);
        npc::NpcMotionSetup s = wander_setup(2000.0);
        s.wander_min = 100.0;
        s.wander_delay = 0.2;
        s.wander_delay_deviation = 0.05;
        w.add(1000, s);
        std::vector<double> legs;
        const Vec3 home{1000, 1000, 1000};
        double max_home = 0, sx = 0, sy = 0;
        bool was_moving = false;
        run(w, 0, 1500, 1000, 1000, [&](double) {
            const world::WorldActor& a = *reg.find(1000);
            const Rm r = read_rm(a);
            const bool moving = std::hypot(r.vel.x, r.vel.y) > 1.0;
            if (moving && !was_moving) { sx = a.x; sy = a.y; }
            if (!moving && was_moving) legs.push_back(std::hypot(a.x - sx, a.y - sy));
            was_moving = moving;
            max_home = std::max(max_home, dist2(a, home));
        });
        std::sort(legs.begin(), legs.end());
        const double med = legs.empty() ? 0 : legs[legs.size() / 2], p90 = legs.empty() ? 0 : legs[legs.size() * 9 / 10];
        std::printf("    %zu legs in 25 minutes: median %.0f cm, 90th percentile %.0f cm, farthest from home %.0f cm\n", legs.size(), med, p90, max_home);
        CHECK(legs.size() > 80, "it walks leg after leg");
        CHECK(med > 150 && med < 450 && p90 > 450 && p90 < 1300, "a leg is a few metres, as retail's: a median of about 250 cm, the 90th percentile of about 740");
        CHECK(max_home <= 2000.0 + 150.0, "and it never leaves its radius: a leg that would turns back towards home");
    }
}
// C Y B + CoA

// ---- patrol -----------------------------------------------------------------------------------------------------------------------------

static void test_patrol() {
    std::printf("patrol\n");
    auto& n = navfix::shared_nav();
    npc::NpcMotionSetup s;
    s.ambient = npc::Ambient::Patrol;
    s.foot_offset = 96;
    s.patrol_speed = 200;
    s.run_speed = 400;
    s.route = {{0, 5000, 1000}, {3000, 5000, 1000}, {3000, 8000, 1000}};
    s.pauses = {0.0, 3.0, 1.0};
    {
        world::ActorRegistry reg;
        reg.add(make_npc(1000, 0, 5000, 1096));
        npc::NpcWorld w(reg, &n);
        s.loop = true;
        w.add(1000, s);
        std::vector<int> order;
        int last_wp = -1;
        double pause_t = 0, pause_max = 0;
        run(w, 0, 140, 0, 5000, [&](double) {
            const world::WorldActor& a = *reg.find(1000);
            for (size_t i = 0; i < s.route.size(); ++i) {
                if (dist2(a, s.route[i]) < 8.0 && static_cast<int>(i) != last_wp) {
                    order.push_back(static_cast<int>(i));
                    last_wp = static_cast<int>(i);
                    pause_t = 0;
                }
            }
            const Rm r = read_rm(a);
            if (last_wp == 1 && std::hypot(r.vel.x, r.vel.y) < 1.0 && dist2(a, s.route[1]) < 8.0) { pause_t += 1.0 / 15.0; pause_max = std::max(pause_max, pause_t); }
        });
        std::string seq;
        for (int i : order) seq += std::to_string(i);
        std::printf("    a loop visited: %s\n", seq.c_str());
        CHECK(order.size() >= 7 && seq.substr(0, 7) == "0120120", "a looping route goes round: 0 1 2 0 1 2 0 ...");
        CHECK(pause_max > 2.7 && pause_max < 3.6, "it waited about the 3 s the route says at the second waypoint");
    }
    {
        world::ActorRegistry reg;
        reg.add(make_npc(1000, 0, 5000, 1096));
        npc::NpcWorld w(reg, &n);
        s.loop = false;
        w.add(1000, s);
        std::vector<int> order;
        int last_wp = -1;
        run(w, 0, 200, 0, 5000, [&](double) {
            const world::WorldActor& a = *reg.find(1000);
            for (size_t i = 0; i < s.route.size(); ++i) {
                if (dist2(a, s.route[i]) < 8.0 && static_cast<int>(i) != last_wp) { order.push_back(static_cast<int>(i)); last_wp = static_cast<int>(i); }
            }
        });
        std::string seq;
        for (int i : order) seq += std::to_string(i);
        std::printf("    there and back visited: %s\n", seq.c_str());
        CHECK(seq.size() >= 6 && seq.substr(0, 6) == "012101", "a route that does not loop goes there and back: 0 1 2 1 0 1 ...");
    }
    {
        // a waypoint nobody can walk to (inside the closed pen) is skipped
        world::ActorRegistry reg;
        reg.add(make_npc(1000, 0, 5000, 1096));
        npc::NpcWorld w(reg, &n);
        npc::NpcMotionSetup bad = s;
        bad.loop = true;
        bad.route = {{0, 5000, 1000}, {-8400, 6600, 1000}, {3000, 5000, 1000}};
        w.add(1000, bad);
        std::vector<int> order;
        int last_wp = -1;
        run(w, 0, 120, 0, 5000, [&](double) {
            const world::WorldActor& a = *reg.find(1000);
            for (size_t i = 0; i < bad.route.size(); ++i) {
                if (dist2(a, bad.route[i]) < 8.0 && static_cast<int>(i) != last_wp) { order.push_back(static_cast<int>(i)); last_wp = static_cast<int>(i); }
            }
        });
        bool visited_pen = false;
        for (int i : order) visited_pen = visited_pen || i == 1;
        CHECK(!visited_pen && order.size() >= 4, "a waypoint in the closed pen is never reached: the patrol goes on between the others");
        CHECK(w.stats().no_path > 0, "and the failures are counted");
    }
}

// ---- return home and the service -----------------------------------------------------------------------------------------------------------
// ~ C | Y | B :: C.O.A ~

static void test_service() {
    std::printf("the service (return home, orders)\n");
    auto& n = navfix::shared_nav();
    world::ActorRegistry reg;
    reg.add(make_npc(1000, 1000, 1000, 1096));
    reg.add(make_npc(1002, -8400, 6600, 1096));   // inside the pen
    npc::NpcWorld w(reg, &n);
    npc::NpcMotionSetup s = wander_setup(1500);
    s.ambient = npc::Ambient::Stationary;
    s.run_speed = 400;
    w.add(1000, s);
    w.add(1002, s);
    // order a walk far away: the AI has the feet
    CHECK(w.order_move(1000, {-5000, 0, 1000}, 500), "an order to walk to (-5000, 0) round the wall is accepted");
    CHECK(w.is_ordered(1000) && w.mover(1000)->moving(), "it is ordered and on its way");
    double max_speed = 0;
    double t = run(w, 0, 40, 1000, 1000, [&](double) {
        const Rm r = read_rm(*reg.find(1000));
        max_speed = std::max(max_speed, std::hypot(r.vel.x, r.vel.y));
    });
    CHECK(max_speed > 480 && max_speed < 520, "at the speed ordered (500 cm/s)");
    CHECK(dist2(*reg.find(1000), {-5000, 0, 0}) < 5.0 && !w.mover(1000)->moving(), "and it stood there");
    CHECK(w.is_ordered(1000), "it stays ordered (the AI's) until released");
    // home: it walks back at its run speed
    CHECK(w.order_return_home(1000), "an order home is accepted (it is across the wall: a path round it)");
    double max_home_speed = 0;
    t = run(w, t, 60, 1000, 1000, [&](double) {
        const Rm r = read_rm(*reg.find(1000));
        max_home_speed = std::max(max_home_speed, std::hypot(r.vel.x, r.vel.y));
    });
    CHECK(max_home_speed > 390 && max_home_speed < 410, "it ran at its run speed (400 cm/s)");
    CHECK(dist2(*reg.find(1000), {1000, 1000, 0}) < 5.0 && !w.is_ordered(1000), "it is home, and its own behaviour (standing) has it again");
    // an unreachable destination
    const double x1 = reg.find(1000)->x;
    CHECK(!w.order_move(1000, {-8400, 6600, 1000}, 300), "a point in the closed pen: refused");
    CHECK(!w.order_move(1000, {900000, 0, 0}, 300), "a point far outside the navmesh: refused");
    CHECK(!w.order_move(1000, {std::nan(""), 0, 0}, 300), "a NaN destination: refused");
    CHECK(!w.order_move(1000, {0, 0, 1000}, 0) && !w.order_move(1000, {0, 0, 1000}, -5) && !w.order_move(1000, {0, 0, 1000}, std::nan("")), "a speed of zero, negative or NaN: refused");
    run(w, t, 5, 1000, 1000, [](double) {});
    CHECK(reg.find(1000)->x == x1, "nothing moved");
    CHECK(!w.order_move(7777, {0, 0, 1000}, 300) && !w.release(7777) && !w.order_return_home(7777), "orders to an unknown NPC are refused");
    // the pen NPC: a home it cannot leave from; its walk out is refused
    CHECK(!w.order_move(1002, {0, 0, 1000}, 300), "an NPC inside the closed pen cannot be sent out: no path");
    CHECK(w.order_move(1002, {-8000, 6800, 1000}, 300), "but it can walk inside the pen");
    t = run(w, t, 10, -8400, 6600, [](double) {});
    CHECK(dist2(*reg.find(1002), {-8000, 6800, 0}) < 150.0 || !w.mover(1002)->moving(), "and did");
    CHECK(w.release(1002) && !w.release(1002), "released once");
    // a wanderer that is taken far from home (stage 3's chase ends there) walks back by itself once it is released, at a run
    {
        reg.add(make_npc(1004, 1000, -3000, 1096));
        npc::NpcMotionSetup ws = wander_setup(500);
        ws.run_speed = 450;
        w.add(1004, ws);
        const auto returns0 = w.stats().returns_home;
        CHECK(w.order_move(1004, {-1000, 3000, 1000}, 600), "a wanderer is ordered 66 m from home");
        t = run(w, t, 30, 1000, -3000, [](double) {});
        CHECK(dist2(*reg.find(1004), {-1000, 3000, 0}) < 5.0, "it got there");
        CHECK(w.release(1004), "released");
        double max_speed = 0;
        bool home_again = false;
        t = run(w, t, 60, 1000, -3000, [&](double) {
            const Rm r = read_rm(*reg.find(1004));
            max_speed = std::max(max_speed, std::hypot(r.vel.x, r.vel.y));
            home_again = home_again || dist2(*reg.find(1004), {1000, -3000, 0}) < 700.0;
        });
        CHECK(w.stats().returns_home == returns0 + 1, "it began walking home on its own (one return)");
        CHECK(max_speed > 440 && max_speed < 460 && home_again, "at its run speed, and it is within its radius of home again");
    }
    // a home nobody can walk to (re-set inside the closed pen): the order home is refused and the NPC stays where it is
    {
        const double hx = reg.find(1004)->x, hy = reg.find(1004)->y;
        CHECK(w.set_home(1004, {-8400, 6600, 1000}) && w.home_of(1004).x == -8400, "a home is re-set");
        CHECK(!w.set_home(1004, {std::nan(""), 0, 0}) && !w.set_home(7777, {0, 0, 0}), "a home that is not a number, or of an unknown NPC, is refused");
        CHECK(!w.order_return_home(1004), "an unreachable home: the order is refused");
        run(w, t, 5, 1000, -3000, [](double) {});
        CHECK(std::hypot(reg.find(1004)->x - hx, reg.find(1004)->y - hy) < 1500.0, "and the NPC did not go anywhere in particular (its wander is about the old place only until it is far: it walks the radius it has)");
        w.set_home(1004, {hx, hy, 1000});
    }
    // stop short
    CHECK(w.order_move(1000, {-1000, 3000, 1000}, 400, 300.0), "a walk that stops 300 cm short of its point");
    t = run(w, t, 30, 1000, 1000, [](double) {});
    const double d = dist2(*reg.find(1000), {-1000, 3000, 0});
    CHECK(d > 250 && d < 340, "ends about 300 cm short");
}

// ---- the mover on its own ---------------------------------------------------------------------------------------------------------------

static void test_mover() {
    std::printf("the mover\n");
    auto& n = navfix::shared_nav();
    npc::Mover m;
    m.init(&n, 96.0, 360.0);
    m.place(-5000, 0, 1000, 0);
    CHECK(m.status() == npc::Mover::Status::Idle && !m.moving(), "idle after place");
    CHECK(m.move_to({-1000, 0, 1000}, 200.0) && m.moving() && m.remaining() > 5000, "a walk round the wall is longer than the straight 4,000 cm");
    size_t n_steps = 0;
    double total = 0, yaw_swing = 0, last_yaw = m.yaw_degrees();
    Vec3 prev = m.ground();
    while (m.moving() && n_steps < 100000) {
        m.advance(1.0 / 15.0);
        total += std::hypot(m.ground().x - prev.x, m.ground().y - prev.y);
        prev = m.ground();
        yaw_swing = std::max(yaw_swing, std::fabs(m.yaw_degrees() - last_yaw));
        last_yaw = m.yaw_degrees();
        ++n_steps;
    }
    CHECK(m.status() == npc::Mover::Status::Arrived && std::hypot(m.ground().x + 1000, m.ground().y) < 1.0, "it arrives at the point");
    CHECK(std::fabs(total / (n_steps / 15.0) - 200.0) < 6.0, "at the speed asked, on average (the last step is short)");
    CHECK(m.velocity().x == 0 && m.velocity().y == 0 && m.speed() == 0, "and has no velocity once there");
    CHECK(!m.move_to({-8400, 6600, 1000}, 200.0) && m.status() == npc::Mover::Status::Blocked, "a point in the pen: Blocked");
    CHECK(m.move_to({-8400, 6600, 1000}, 200.0, 0, 0, /*allow_partial=*/true) && m.moving(), "or, partial allowed, a walk to the closest point it can reach");
    npc::Mover none;
    none.init(nullptr, 96.0, 360.0);
    none.place(0, 0, 1000, 0);
    CHECK(!none.move_to({100, 0, 1000}, 100.0) && none.status() == npc::Mover::Status::Blocked, "without a navmesh every order is Blocked");
    CHECK(!m.follow({}, 100.0) && !m.follow({{1, 2, 3}}, 0.0) && !m.follow({{std::nan(""), 0, 0}}, 100.0), "an empty path, a zero speed and a NaN point are refused");
    m.place(0, 0, 1000, 0);
    CHECK(m.follow({{0, 0, 1000}, {300, 0, 1000}}, 100.0) && m.advance(0.0) == npc::Mover::Status::Moving && m.advance(std::nan("")) == npc::Mover::Status::Moving,
          "a zero or NaN step does nothing");
    CHECK(m.advance(1.0) == npc::Mover::Status::Moving && std::fabs(m.ground().x - 100.0) < 1e-6, "a step of a second at 100 cm/s: 100 cm");
    CHECK(m.advance(10.0) == npc::Mover::Status::Arrived && std::fabs(m.ground().x - 300.0) < 1e-6, "a long step ends at the end of the path");
}

// ---- the table, hostile inputs, no navmesh, the cost ---------------------------------------------------------------------------------------------

static void test_table_and_hostile() {
    std::printf("the motion table and hostile inputs\n");
    npc::MotionTable t;
    std::string err;
    const std::string head = "archetype\tparents\tdefault_scale\tstarting_alignments\thalf_height\tradius\tmax_walk_speed\tmax_run_speed\tmax_acceleration\trotation_rate_yaw\tmove_mode\tfly_speed\ttags\n";
    const auto line = [](const char* path, const char* hh, const char* r, const char* walk, const char* run, const char* acc, const char* yaw) {
        return std::string(path) + "\tNPCPawn_C\t1\t\t" + hh + "\t" + r + "\t" + walk + "\t" + run + "\t" + acc + "\t" + yaw + "\tEMovementMode::MOVE_NavWalking\t0\t\n";
    };
    CHECK(t.parse(head + line("/Game/A.Default__A_C", "88", "34", "600", "800", "2048", "360") + line("/Game/B.Default__B_C", "66.5", "55", "300", "300", "1024", "720"), err), "a table parses");
    CHECK(t.size() == 2 && t.find("/Game/A.Default__A_C")->max_run_speed == 800 && t.get("/Game/B.Default__B_C").half_height == 66.5, "its lines are found");
    CHECK(!t.find("/Game/Z.Default__Z_C") && t.get("/Game/Z.Default__Z_C").half_height == 88.0, "an archetype with no line: the NPCPawn defaults");
    CHECK(!t.parse("", err) && !t.parse("a\tb\n", err) && !t.parse(head + "/A\t1\t2\n", err), "an empty table, a wrong header and a short line are refused");
    CHECK(!t.parse(head + line("/A", "-1", "1", "1", "1", "1", "1"), err) && !t.parse(head + line("/A", "nan", "1", "1", "1", "1", "1"), err), "a negative or NaN number is refused");
    CHECK(t.size() == 2, "a refused parse leaves the table as it was");
    {
        // what a Blueprint is made of: its parents, scale, alignments and tags
        npc::MotionTable c;
        const std::string row = "/Game/W.Default__W_C\tBeast_C,NPCPawn_C\t1.09\t6064632310208593979,0,42\t88\t34\t600\t600\t2048\t360\tEMovementMode::MOVE_NavWalking\t0\tAnimal,Hunted\n";
        CHECK(c.parse(head + row, err), "a line with parents, a scale, alignments and tags parses");
        const auto m = c.find("/Game/W.Default__W_C");
        CHECK(m && m->parents == "Beast_C,NPCPawn_C" && m->default_scale == 1.09 && m->alignments.size() == 2 && m->alignments[0] == 6064632310208593979LL && m->alignments[1] == 42 &&
                  m->tags == "Animal,Hunted" && m->move_mode == "EMovementMode::MOVE_NavWalking",
              "its facts are read (an unset alignment slot, 0, is dropped)");
        CHECK(!c.parse(head + "/A\tNPCPawn_C\t0\t\t88\t34\t600\t600\t2048\t360\tx\t0\t\n", err) && !c.parse(head + "/A\tNPCPawn_C\t1\tnot-a-guid\t88\t34\t600\t600\t2048\t360\tx\t0\t\n", err),
              "a zero scale and an alignment that is no guid are refused");
    }
    CHECK(!t.load("no/such/npc_motion.tsv", err) && err.find("cannot open") != std::string::npos, "a missing file: 'cannot open'");
// C.O.A captures, C Y B code

    auto& n = navfix::shared_nav();
    world::ActorRegistry reg;
    reg.add(make_npc(1000, 1000, 1000, 1096));
    npc::NpcWorld w(reg, &n);
    w.add(1000, wander_setup());
    w.note_player(1, std::nan(""), 0, 1.0);
    w.note_player(2, 1e300, -1e300, 1.0);
    w.tick(1.0);
    w.tick(0.5);   // a clock that went back
    w.tick(std::nan(""));
    CHECK(w.now() == 1.0, "a NaN or earlier clock is ignored");
    const auto steps0 = w.stats().steps;
    w.note_player(1, 1000, 1000, 2000.0);
    w.tick(2000.0);   // a jump of 33 minutes
    CHECK(w.stats().steps - steps0 <= static_cast<uint64_t>(w.config().max_catch_up_steps), "a clock that jumped ahead is not made up for beyond the catch-up cap");

    // an actor removed from the registry (a dead NPC) leaves the NPC world, in the middle of a walk
    {
        reg.add(make_npc(1006, 1500, 1500, 1096));
        w.add(1006, wander_setup());
        double tt = 3000.0;
        for (int i = 0; i < 300 && w.has(1006); ++i) {
            tt += 1.0 / 15.0;
            w.note_player(1, 1500, 1500, tt);
            w.tick(tt);
            if (i == 150) reg.remove(1006);
        }
        CHECK(!w.has(1006) && w.mover(1006) == nullptr && !w.order_move(1006, {0, 0, 1000}, 100), "an NPC the registry removes is dropped (no dangling state) and refuses orders");
        CHECK(!w.remove(1006), "and removing it again says so");
    }
    // no navmesh: nothing is taken, the NPCs stay
    world::ActorRegistry reg2;
    reg2.add(make_npc(1000, 1000, 1000, 1096));
    npc::NpcWorld none(reg2, nullptr);
    CHECK(!none.add(1000, wander_setup()) && none.size() == 0, "no navmesh: add refuses");
    none.note_player(1, 1000, 1000, 1.0);
    none.tick(1.0);
    CHECK(reg2.find(1000)->x == 1000 && reg2.find(1000)->z == 1096 && reg2.find(1000)->version == 1, "and the NPC stands where it was, unchanged");
}

static void test_cost() {
    std::printf("the cost of 900 NPCs\n");
    auto& n = navfix::shared_nav();
    world::ActorRegistry reg;
    const int count = 900;
    std::vector<uint64_t> ids;
    for (int i = 0; i < count; ++i) {
        const uint64_t id = 2000 + static_cast<uint64_t>(i) * 2;
        const double x = -12000 + (i % 30) * 800.0, y = -12000 + (i / 30) * 800.0;
        reg.add(make_npc(id, x, y, 1096));
        ids.push_back(id);
    }
    npc::NpcWorld w(reg, &n);
    size_t added = 0;
    for (const uint64_t id : ids) {
        npc::NpcMotionSetup s = wander_setup(1200);
        s.wander_max = 2000;
        if (w.add(id, s)) ++added;
    }
    CHECK(added > 800, "most of 900 NPCs found their ground on the fixture's plain (the rest stand on the wall, in the pen or off the area)");
    // a player in the middle: the NPCs within 170 m are awake
    double t = 0;
    double total_us = 0, max_us = 0;
    size_t ticks = 0;
    for (int i = 0; i < 900; ++i) {   // 60 s
        t += 1.0 / 15.0;
        w.note_player(1, 0, 0, t);
        const auto t0 = std::chrono::steady_clock::now();
        w.tick(t);
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        total_us += us;
        max_us = std::max(max_us, us);
        ++ticks;
    }
    const auto st = w.stats();
    std::printf("    %zu steps: %.0f us on average, %.0f us at most; %zu awake, %zu walking now; %llu legs started, %llu writes\n", ticks, total_us / static_cast<double>(ticks), max_us,
                st.active_now, st.moving_now, static_cast<unsigned long long>(st.moves_started), static_cast<unsigned long long>(st.writes));
    CHECK(total_us / static_cast<double>(ticks) < 2000.0, "a step takes under 2 ms on average with the area's crowd awake");
    CHECK(st.active_now > 100 && st.moves_started > 100, "the NPCs around the player were awake and walking");
}
// Cyb with C.O.A

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto t0 = std::chrono::steady_clock::now();
    test_standing();
    test_ground_by_data();
    test_dormant();
    test_wander();
    test_wander_determinism();
    test_budget_and_legs();
    test_patrol();
    test_service();
    test_mover();
    test_table_and_hostile();
    test_cost();
    std::printf("test_worldd_npc_move: %d passed, %d failed (%.1f s)\n", g_pass, g_fail, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return g_fail == 0 ? 0 : 1;
}
