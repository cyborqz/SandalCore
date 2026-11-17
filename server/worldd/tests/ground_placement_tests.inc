// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/ground_placement_tests.inc  (the community bug fixes, lane 4: a respawn and a teleport put the player under the ground)
//
//  The paths that put a player somewhere: a death's respawn (the Emberspring the window offers: the nearest discovered one, the home, the plain
//  Respawn's nearest point) and a game master's teleport (the admin API and worldctl's `tp`: a named place, a point with a height, a point without
//  one; the client's own ServerTeleport of a place).  Each places the player on the ground the data has at that spot, with the capsule's half height
//  (98 cm): a town's floor in a bowl, the floor of a building over it, the walking map's floor, or the landscape.  A height a game master names is
//  kept unless it is under the ground.  After a teleport the client is held at the place (a position correction again every half second) until its
//  own moves show it stands there, because the floor it was sent to may still be streaming in.
//
//    the real rows       the world's 64 Embersprings (the respawn points; four stand under the ground, in the Anvils' Dunzenkell), its 47 named places and its 64 fast travel destinations (the attune
//                        points): the height the respawn and the teleport set for each against the landscape and the five towns' platforms, on the
//                        real content.db and the real walking map's terrain; the retail respawn recorded at Joeva against the platform's floor
//    a hill              the nav fixture's hill: a spot whose data height is four metres under the hill top comes back on the top, not in it
//    a town              a spot in a platform bowl comes back on the floor, with no two metres of slack over it
//    near water          the Emberspring nearest the fishing water of the Riverlands: on the ground
//    a building          a raised floor of the walking map is the ground of a teleport with no height, not the landscape under it
//    a height given      under the ground: lifted onto it; above it: kept; the point asked twice: the same
//    the hold            a client that falls through the place it was sent to is put back there, and the hold ends once it stands; mounted; in a fight
//    the reconnect       a respawn hands its place to the saver, and the next session joins there
// ============================================================================
#include "spawns/area.h"
#include "spawns/population.h"

#include <optional>

// ---- the real rows -------------------------------------------------------------------------------------------------------------------

/// The surface the data has at (x, y) away from the walking map's floors: a town's platform floor in one of the five bowls, else the landscape.
static bool gp_ground_without_floors(const PlayerWorld& w, double x, double y, double* z) {
    if (const auto bowl = w.bowl_floor_at(x, y)) {
        *z = bowl->z;
        return true;
    }
    return w.nav() != nullptr && w.nav()->terrain_height(x, y, z);
}

static void test_the_real_rows_on_the_ground() {
    std::printf("the real rows of the world: the respawn points, the named places and the fast travel destinations on the ground\n");
    std::string error;
    auto real = content::ContentDb::open(real_db_path(), error);
    CHECK(real.has_value(), "the real content.db opens");
    if (!real) return;
    auto population = real->population_data(error);
    CHECK(population.has_value(), "its population chain loads");
    auto respawn_data = real->respawn_data(error);
    CHECK(respawn_data.has_value(), "its Emberspring tables load");
    if (!population || !respawn_data) return;
    const spawns::AreaConfig riverlands = spawns::AreaConfig::riverlands();
    const std::vector<spawns::RespawnPoint> points = spawns::respawn_points(*population, riverlands);
    size_t shrines = 0, towns = 0;
    for (const spawns::RespawnPoint& p : points) {
        if (!p.attuneable) continue;
        ++shrines;
        const auto link = respawn_data->instance_respawn.find(p.instance);
        const auto def = link == respawn_data->instance_respawn.end() ? respawn_data->respawns.end() : respawn_data->respawns.find(link->second);
        if (def != respawn_data->respawns.end() && def->second.name.rfind("NODE_RVR_", 0) == 0) ++towns;
    }
    CHECK(shrines == 64, "64 Embersprings stand in the world (the respawn points the nearest, the home and the plain Respawn use): the Riverlands' 24 and the new continent's 40");
    CHECK(towns == 5, "... 5 of them the towns' own (NODE_RVR_*): Winstead, Joeva, New Aela, Halcyon, Miraleth");

    auto nav = nav::AreaNav::load("dist/content/area_nav.bin", &error);
    CHECK(nav != nullptr, "the Riverlands' walking map opens (its terrain, the landscape's own samples)");
    if (!nav) return;
    aoc::protocol::ServerGuidAllocator guids{60, 0x6ee87ee4u};
    PlayerWorld real_world(&*real, "", bs::GameStateMode::Tier3, 60, guids);
    real_world.set_city_nodes(g_nodes.get());
    CHECK(real_world.load_town_platforms("dist/content/town_platforms.bin", error), "the five towns' platforms load");
    CHECK(real_world.load_blueprints("dist/content/npc_motion.tsv", error), "the Blueprint table loads");
    CHECK(real_world.adopt_ground(std::move(nav), error), "the real ground is adopted");
    if (real_world.nav() == nullptr) return;

    // the respawn points: the height the shrine's respawn sets (spawn_height, the session's call) against the data's surface
    // (the walking map's window is empty here: no chunk of it is loaded, as for a player who died far from the shrine.  One shrine, the Wreckage of Carphin, stands on a hull 4 m
    // over the landscape, which only the walking map shows: its data's height stands, as it always did, instead of the landscape under the hull)
    // (the four shrines of the Anvils' Dunzenkell stand 300 to 500 m under the mountains: their data's height stands too, not the mountain's top over them)
    size_t on = 0, under = 0, above = 0, no_ground = 0, deep = 0;
    double worst = 0, structure_over = 0;
    for (const spawns::RespawnPoint& p : points) {
        if (!p.attuneable) continue;
        bool grounded = false;
        const double z = real_world.spawn_height(p.x, p.y, p.z, &grounded);
        double ground = 0;
        if (!grounded || !gp_ground_without_floors(real_world, p.x, p.y, &ground)) {
            ++no_ground;
            continue;
        }
        if (p.z < ground - PlayerWorld::kUndergroundCm) {
            if (std::fabs(z - p.z) <= 5.0) ++deep;
            else ++under;
            continue;
        }
        const double d = z - (ground + PlayerWorld::kCapsuleHalfHeightCm);
        if (d > 5.0) structure_over = p.z - ground;
        ++(d < -5.0 ? under : (d > 5.0 ? above : on));
        worst = std::max(worst, std::fabs(d));
    }
    char line[360];
    std::snprintf(line, sizeof line, "every Emberspring's respawn: the surface + the capsule's 98 cm (%zu on, %zu under, %zu above: the shrine on a hull %.0f cm over the landscape keeps its data's height; the worst %.1f cm)", on, under, above,
                  structure_over, worst);
    CHECK(on + deep == 63 && under == 0 && above == 1 && no_ground == 0 && structure_over > 400.0 && structure_over < 500.0, line);
    CHECK(deep == 4, "four Embersprings stand under the ground (the Anvils' Dunzenkell, 300 to 500 m under the mountains): their data's height stands");

    // the named places (the admin API's list, the city nodes and not "login")
    const std::vector<world::Place> places = real_world.places().all();
    size_t place_on = 0, place_off = 0, place_n = 0;
    for (const world::Place& p : places) {
        if (p.kind == "login" || !riverlands.contains(p.x, p.y)) continue;
        ++place_n;
        const double z = real_world.place_height(p);
        double ground = 0;
        if (gp_ground_without_floors(real_world, p.x, p.y, &ground) && std::fabs(z - (ground + PlayerWorld::kCapsuleHalfHeightCm)) <= 5.0) ++place_on;
        else ++place_off;
    }
    std::snprintf(line, sizeof line, "the %zu named places of the Riverlands: the surface + the capsule's 98 cm (%zu on, %zu not)", place_n, place_on, place_off);
    CHECK(place_n == 47 && place_off == 0, line);

    // the fast travel destinations: the attune points (the raven network's places), at the height the respawn puts a player
    // (an attune point under the ground, in the Anvils' Dunzenkell, keeps its data's height as its shrine does)
    size_t raven_n = 0, raven_on = 0, raven_off = 0, raven_data_under = 0, raven_deep = 0, raven_cave = 0, raven_cave_off = 0;
    double raven_data_under_worst = 0;
    for (const auto& [guid, p] : respawn_data->places) {
        (void)guid;
        if (p.number <= 0 || p.level_name != "Verra_World_Master" || !riverlands.contains(p.x, p.y)) continue;
        ++raven_n;
        bool grounded = false;
        const double z = real_world.spawn_height(p.x, p.y, p.z, &grounded);
        double ground = 0;
        if (gp_ground_without_floors(real_world, p.x, p.y, &ground) && p.z < ground - PlayerWorld::kUndergroundCm) {
            ++raven_cave;
            if (std::fabs(z - p.z) > 5.0) ++raven_cave_off;
            continue;
        }
        if (grounded && gp_ground_without_floors(real_world, p.x, p.y, &ground) && std::fabs(z - (ground + PlayerWorld::kCapsuleHalfHeightCm)) <= 5.0) ++raven_on;
        else ++raven_off;
        if (gp_ground_without_floors(real_world, p.x, p.y, &ground) && p.z < ground) {
            ++raven_data_under;   // the data's own height is under the surface: a fast travel that put the player at it would be under the ground
            raven_data_under_worst = std::min(raven_data_under_worst, p.z - ground);
            if (p.z < ground - 5.0) ++raven_deep;
        }
    }
    std::snprintf(line, sizeof line, "the %zu fast travel destinations (the attune points): the surface + 98 cm (%zu on, %zu not: the Wreckage of Carphin's hull; %zu under the ground at their data's height, %zu not); %zu of the others' own data heights are under the surface (the lowest %.0f cm, %zu more than 5 cm under)",
                  raven_n, raven_on, raven_off, raven_cave, raven_cave_off, raven_data_under, raven_data_under_worst, raven_deep);
    CHECK(raven_n == 64 && raven_on == 59 && raven_off == 1 && raven_cave == 4 && raven_cave_off == 0, line);
    std::printf("    (%zu of the others' own data heights under the surface, %zu more than 5 cm, the lowest %.0f cm)\n", raven_data_under, raven_deep, raven_data_under_worst);
    CHECK(raven_data_under == 14 && raven_deep == 3 && raven_data_under_worst > -650.0, "... the others' data heights under the landscape are a fraction of a centimetre to 6 m under it (Gravepeak 45 cm; the lowest 6 m, under the 10 m of a cave)");

    // the respawn recorded at Joeva (a level-19 character's Emberspring): where it appeared, where the floor caught it, and the platform's floor there
    const double rx = -1087033.02, ry = 510595.4;
    const auto floor = real_world.bowl_floor_at(rx, ry);
    CHECK(floor.has_value(), "Joeva's platform holds the recorded respawn point");
    if (floor) {
        const double appeared = 13720.94, landed = 13429.47;   // cm: the two positions of the recording
        const double over = landed - (floor->z + PlayerWorld::kCapsuleHalfHeightCm);
        const double drop = appeared - landed;
        std::printf("    the recorded respawn appeared at z %.1f and the floor caught it at z %.1f: %.1f cm above the platform's floor + 98, after a fall of %.1f cm\n",
                    appeared, landed, over, drop);
        CHECK(over > -100.0 && over < 100.0, "the recorded landing is on the platform's floor (+ the capsule, within a metre)");
        CHECK(drop > 100.0 && drop < 400.0, "... and the recorded respawn appeared one to four metres over it: the client fell the last stretch at its phase-in");
    }
}

// ---- the fixture's shapes -----------------------------------------------------------------------------------------------------------

/// A navmesh of the fixture with one more building on the plain: a house 20 x 20 m whose floor is 100 cm over the landscape (a floor of the walking
/// map over the ground, the shape a teleport into a building has).
static nav::NavFile gp_a_nav_with_a_floor() {
    nav::Geometry g = navfix::test_geometry();
    g.meshes.push_back(navfix::box_mesh(1, 1, 1, "house"));
    navfix::place_box(g, static_cast<uint32_t>(g.meshes.size() - 1), 12000, 14000, 8000, 10000, 1000, 1100);
    nav::BuildOptions opt;
    opt.threads = 2;
    opt.nav_margin_cm = 3000;
    nav::NavFile f;
    nav::BuildStats st;
    std::string err;
    if (!nav::build_nav(g, 7, opt, &f, &st, &err)) {
        std::printf("  [FAIL] the house's ground: %s\n", err.c_str());
        std::exit(2);
    }
    return f;
}

static void gp_adopt_a_nav(PlayerWorld& w, nav::NavFile file) {
    with_nodes(w);
    std::string err;
    auto nav = nav::AreaNav::from_file(std::move(file), &err);
    if (!nav || !w.adopt_ground(std::move(nav), err)) {
        std::printf("  [FAIL] the ground does not load: %s\n", err.c_str());
        std::exit(2);
    }
}

static void gp_with_a_house(PlayerWorld& w) { gp_adopt_a_nav(w, gp_a_nav_with_a_floor()); }

// ---- the respawn's placement ---------------------------------------------------------------------------------------------------------

static void test_a_respawn_on_a_hill_and_in_a_town() {
    std::printf("a respawn: on the nav fixture's hill and on a town's platform floor\n");
    {
        // the fixture's hill is 400 cm over the plain at (6,000, 0); the shrine's own data height is the plain's (1,000)
        Harness h(cyb_on_the_ground(), on_the_ground);
        bool grounded = false;
        const double z = h.fx.world->spawn_height(6000.0, 0.0, 1000.0, &grounded);
        CHECK(grounded && near_to(z, 1400.0 + PlayerWorld::kCapsuleHalfHeightCm, 2.0), "a spot on the hill whose data height is 4 m under the top comes back on the top (+ 98 cm), not in the hill");
        const double plain = h.fx.world->spawn_height(0.0, 0.0, 1000.0, &grounded);
        CHECK(grounded && near_to(plain, 1000.0 + PlayerWorld::kCapsuleHalfHeightCm, 1.0), "on the plain the same spot comes back on the plain");
    }
    {
        // a town's bowl: the platform's floor is the ground; no slack over it
        Harness h(cyb_on_the_ground(), a_platform_bowl_at_5000_3000);
        bool grounded = false;
        const double z = h.fx.world->spawn_height(5000.0, 3000.0, 7000.0, &grounded);
        CHECK(grounded && near_to(z, kTownFloor + PlayerWorld::kCapsuleHalfHeightCm, 2.0), "a respawn in a town comes back on the platform's floor (+ 98 cm), with no 2 m of slack over it");
        const auto floor = h.fx.world->bowl_floor_at(5000.0, 3000.0);
        CHECK(floor.has_value() && near_to(z, floor->z + PlayerWorld::kCapsuleHalfHeightCm, 2.0), "... on the floor the world has at the spot");
    }
}

// ---- the teleport's height -----------------------------------------------------------------------------------------------------------

static void test_a_teleport_to_a_named_place_stands_on_the_floor() {
    std::printf("a teleport to a named place: the surface the data has, with the capsule\n");
    Harness h(cyb_on_the_ground(), a_platform_bowl_at_5000_3000);
    h.play();
    GmTravel travel = make_travel(h);
    const world::Place place{"joeva", "Joeva", "place", 5000.0, 3000.0, kTownFloor};
    const double want = kTownFloor + PlayerWorld::kCapsuleHalfHeightCm;
    CHECK(near_to(h.fx.world->place_height(place), want, 2.0), "a place on a town's platform: the floor + 98 cm (no slack)");
    const GmTeleportResult r = travel.teleport(to_point(5000.0, 3000.0), "the test");
    CHECK(r.status == GmStatus::Ok && near_to(r.to.z, want, 2.0), "... and a teleport to the same point is put at that height");
}

static void test_a_teleport_into_a_building() {
    std::printf("a teleport with no height into a building: the floor of the building, not the landscape under it\n");
    Harness h(cyb_on_the_ground(), gp_with_a_house);
    h.play();
    GmTravel travel = make_travel(h);
    GmTeleportResult r = travel.teleport(to_point(13000.0, 9000.0), "the test");   // (the middle of the house)
    CHECK(r.status == GmStatus::Ok && r.ground_height && near_to(r.to.z, 1100.0 + PlayerWorld::kCapsuleHalfHeightCm, 15.0), "the house's floor is 100 cm over the landscape: the player is put on it (+ 98 cm)");
    CHECK(near_to(h.s->movement().good().z, 1100.0 + PlayerWorld::kCapsuleHalfHeightCm, 15.0), "(the move checker is there)");
    r = travel.teleport(to_point(13000.0 + 3000.0, 9000.0), "the test");   // (beside it)
    CHECK(r.status == GmStatus::Ok && near_to(r.to.z, 1000.0 + PlayerWorld::kCapsuleHalfHeightCm, 2.0), "beside the house the landscape is the ground");
}

static void test_a_given_height_is_kept_unless_under_the_ground() {
    std::printf("a teleport with a height: kept as given unless it is under the ground\n");
    Harness h(cyb_on_the_ground(), on_the_ground);
    h.play();
    GmTravel travel = make_travel(h);
    const double on_ground = 1000.0 + PlayerWorld::kCapsuleHalfHeightCm;
    GmTeleportResult r = travel.teleport(to_point_z(3000.0, 2000.0, 500.0), "the test");
    CHECK(r.status == GmStatus::Ok && !r.ground_height && near_to(r.to.z, on_ground, 2.0), "a height 5 m under the plain is lifted onto the ground (+ 98 cm)");
    r = travel.teleport(to_point_z(3000.0, 2000.0, 1500.0), "the test");
    CHECK(r.status == GmStatus::Ok && near_to(r.to.z, 1500.0, 0.01), "a height 4 m over the plain is kept as given");
    r = travel.teleport(to_point_z(6000.0, 0.0, 1050.0), "the test");
    CHECK(r.status == GmStatus::Ok && near_to(r.to.z, 1400.0 + PlayerWorld::kCapsuleHalfHeightCm, 2.0), "a height under the hill's top is lifted onto the hill");
    r = travel.teleport(to_point_z(6000.0, 0.0, 1400.0 + PlayerWorld::kCapsuleHalfHeightCm), "the test");
    CHECK(r.status == GmStatus::Ok && near_to(r.to.z, 1400.0 + PlayerWorld::kCapsuleHalfHeightCm, 2.0), "a height on the hill's top is kept");
    r = travel.teleport(to_point_z(6000.0, 0.0, 2600.0), "the test");
    CHECK(r.status == GmStatus::Ok && near_to(r.to.z, 2600.0, 0.01), "a height 12 m over the hill's top is kept");
}

// ---- the hold -------------------------------------------------------------------------------------------------------------------------

/// A falling move of the pawn's client: `below` cm under `target` after `dt` seconds.
static void gp_fall(Harness& h, const GmTeleportResult& target, double below, double dt) {
    g_pawn_ts += static_cast<float>(dt);
    move_pawn(h, move_at(g_pawn_ts, target.to.x, target.to.y, target.to.z - below, yaw16_of(h.s->movement().good().yaw_degrees), repl::kMoveFalling));
}

static void test_the_teleport_holds_the_client() {
    std::printf("a teleport holds the client at the place until its own moves show it stands there\n");
    Harness h(cyb_on_the_ground(), on_the_ground);
    h.play();
    GmTravel travel = make_travel(h);
    const size_t from = h.got.size();
    const GmTeleportResult r = travel.teleport(to_point(5000.0, 3000.0), "the test");
    CHECK(r.status == GmStatus::Ok && near_to(r.to.z, 1098.0, 1.0), "(the player is put on the plain)");
    h.run(milliseconds(100));
    CHECK(corrections_on(h, h.pawn_ch, from, "PlayerPawn_C") == 1, "the teleport sends one correction");
    // its client has no floor there yet and falls: its own move says it is 2 m under the place
    gp_fall(h, r, 200.0, 0.2);
    h.run(milliseconds(700));
    CHECK(near_to(h.s->movement().good().z, r.to.z, 1.0), "the falling client is put back at the place (the hold)");
    CHECK(corrections_on(h, h.pawn_ch, from, "PlayerPawn_C") == 2, "... with a second correction, the hold's");
    h.run(milliseconds(700));
    CHECK(near_to(h.s->movement().good().z, r.to.z, 1.0), "(it stays there while the client sends nothing)");
    // it falls again: held again
    gp_fall(h, r, 260.0, 0.3);
    h.run(milliseconds(900));
    CHECK(near_to(h.s->movement().good().z, r.to.z, 1.0) && corrections_on(h, h.pawn_ch, from, "PlayerPawn_C") == 3, "a second fall: put back again");
    // now the client has the floor and stands there: the hold is over
    g_pawn_ts += 0.2f;
    move_pawn(h, move_at(g_pawn_ts, r.to.x + 20.0, r.to.y, r.to.z, yaw16_of(h.s->movement().good().yaw_degrees)));
    h.run(milliseconds(1000));
    CHECK(near_to(h.s->movement().good().x, r.to.x + 20.0, 1.0) && corrections_on(h, h.pawn_ch, from, "PlayerPawn_C") == 3,
          "once the client walks at the place its move is not corrected, and the hold sends nothing more");
    // a fall long after the hold is over is a fall of the game's, not the hold's
    const size_t acked = h.s->moves_acknowledged();
    gp_fall(h, r, 200.0, 0.2);
    h.run(milliseconds(700));
    CHECK(h.s->moves_acknowledged() > acked && corrections_on(h, h.pawn_ch, from, "PlayerPawn_C") == 3, "a fall after the hold is not corrected");
}

static void test_the_same_teleport_twice() {
    std::printf("the same teleport twice: the second one is placed and held the same\n");
    Harness h(cyb_on_the_ground(), on_the_ground);
    h.play();
    GmTravel travel = make_travel(h);
    const size_t from = h.got.size();
    const GmTeleportResult a = travel.teleport(to_point(5000.0, 3000.0), "the test");
    const GmTeleportResult b = travel.teleport(to_point(5000.0, 3000.0), "the test");
    h.run(milliseconds(100));
    CHECK(a.status == GmStatus::Ok && b.status == GmStatus::Ok && near_to(a.to.x, b.to.x, 0.01) && near_to(a.to.z, b.to.z, 0.01), "the same place, at the same height");
    CHECK(corrections_on(h, h.pawn_ch, from, "PlayerPawn_C") == 2, "one correction each");
    gp_fall(h, b, 200.0, 0.2);
    h.run(milliseconds(900));
    CHECK(near_to(h.s->movement().good().z, b.to.z, 1.0) && corrections_on(h, h.pawn_ch, from, "PlayerPawn_C") == 3, "the second teleport's hold holds it");
    // and a teleport somewhere else moves the hold with it
    const GmTeleportResult c = travel.teleport(to_point(4000.0, 2000.0), "the test");
    h.run(milliseconds(100));
    gp_fall(h, c, 200.0, 0.2);
    h.run(milliseconds(900));
    CHECK(near_to(h.s->movement().good().x, 4000.0, 1.0) && near_to(h.s->movement().good().z, c.to.z, 1.0), "a teleport elsewhere: the hold's place is the new one");
}

static void test_the_teleport_hold_mounted() {
    std::printf("a teleport of a mounted player: the ride is over, the player is held on the ground\n");
    Harness h(cyb_record(), with_nodes);
    h.play();
    GmTravel travel = make_travel(h);
    CHECK(ride_on(h), "(the player rides)");
    if (!h.s->mounted()) return;
    Rider ride = start_riding(h);
    for (int i = 0; i < 5; ++i) ride_step(h, ride, 60, 0);
    const GoodPosition on_mount = h.s->movement().good();
    const size_t from = h.got.size();
    const GmTeleportResult r = travel.teleport(to_point_z(on_mount.x + 20000.0, on_mount.y, on_mount.z), "the test");
    CHECK(r.status == GmStatus::Ok && r.dismounted && !h.s->mounted(), "(the player is dismounted and moved)");
    h.run(milliseconds(100));
    CHECK(corrections_on(h, h.pawn_ch, from, "PlayerPawn_C") == 1, "(one correction)");
    gp_fall(h, r, 200.0, 0.2);
    h.run(milliseconds(900));
    CHECK(near_to(h.s->movement().good().z, r.to.z, 1.0) && corrections_on(h, h.pawn_ch, from, "PlayerPawn_C") == 2, "the dismounted player is held at the place it was put at");
}

// ---- the reconnect --------------------------------------------------------------------------------------------------------------------

static BunchWriter gp_respawn_choices() { return actor_rpc(class_named("PlayerPawn_C"), "RequestServerRespawnChoices", {}); }

static BunchWriter gp_respawn_choice(int64_t record) {
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    const schema::RpcSchema* rpc = pawn.rpc("SendServerRespawnChoice");
    repl::RpcParams p;
    if (!rpc || !repl::set_rpc_param(*rpc, p, "RespawnChoice", repl::Value::i64(record))) {
        std::printf("  [FAIL] the respawn choice's parameters do not fit\n");
        return BunchWriter();
    }
    return actor_rpc(pawn, "SendServerRespawnChoice", p);
}

static void test_a_respawn_hands_over_its_place() {
    std::printf("a respawn hands its place to the saver, and a new session joins there\n");
    const int64_t fallback = 6064632337807835941ll;   // the one choice of an area with no Emberspring (the login spot)
    Harness h(cyb_on_the_ground(), on_the_ground);
    h.play();
    GmTravel travel = make_travel(h);
    const GmTeleportResult moved = travel.teleport(to_point_z(5000.0, 3000.0, 1500.0), "the test");
    CHECK(moved.status == GmStatus::Ok && !h.sink.positions.empty() && near_to(h.sink.positions.back().x, 5000.0, 1.0), "(the teleport's place is handed over)");
    const double login_x = h.fx.world->area().center_x, login_y = h.fx.world->area().center_y;
    h.s->set_current_value(content::kGuidHealth, 0.0f);
    h.run(milliseconds(400));
    CHECK(h.s->dead(), "(the player is dead)");
    h.client_sends({h.client.actor(h.pawn_ch, gp_respawn_choices(), true)});
    h.run(milliseconds(300));
    h.client_sends({h.client.actor(h.pawn_ch, gp_respawn_choice(fallback), true)});
    h.run(milliseconds(500));
    CHECK(!h.s->dead() && h.s->respawns() == 1, "(it comes back at the fallback: the login spot)");
    CHECK(near_to(h.s->movement().good().x, login_x, 1.0) && near_to(h.s->movement().good().y, login_y, 1.0), "(the move checker is at the login spot)");
    CHECK(!h.sink.positions.empty() && near_to(h.sink.positions.back().x, login_x, 1.0) && near_to(h.sink.positions.back().y, login_y, 1.0),
          "the respawn's place is handed to the saver at once (a crash or a second login must not take the player back)");
    // the floor it comes back on may still be streaming in: a client that falls under the place it respawned at is put back there
    const GoodPosition respawned = h.s->movement().good();
    const size_t corrections = corrections_on(h, h.pawn_ch, 0, "PlayerPawn_C");
    g_pawn_ts += 0.2f;
    move_pawn(h, move_at(g_pawn_ts, respawned.x, respawned.y, respawned.z - 200.0, yaw16_of(respawned.yaw_degrees), repl::kMoveFalling));
    h.run(milliseconds(900));
    CHECK(near_to(h.s->movement().good().z, respawned.z, 1.0) && corrections_on(h, h.pawn_ch, 0, "PlayerPawn_C") == corrections + 1,
          "a client that falls under the place it respawned at is put back there (the respawn's hold)");
    // the next session starts where the respawn left it
    svc::Character next = cyb_on_the_ground();
    if (!h.sink.positions.empty()) {
        next.has_position = true;
        next.position = {h.sink.positions.back().x, h.sink.positions.back().y, h.sink.positions.back().z, h.sink.positions.back().yaw};
    }
    Harness again(next, on_the_ground);
    const GoodPosition start = again.s->movement().good();
    CHECK(near_to(start.x, login_x, 1.0) && near_to(start.y, login_y, 1.0) && near_to(start.z, spawns::kLoginSpotZ, 1.0),
          "a session opened after it joins at the respawn's place");
}

// ---- the walking map round the real rows ---------------------------------------------------------------------------------------------

/// The real Riverlands with the walking map's window the way a session has it: the chunks round a spot are there when asked for them (load_around), and not otherwise.
static void test_the_real_rows_with_the_walking_map() {
    std::printf("the real Embersprings with the walking map loaded round them: no respawn spot and no point is under the landscape, and a shrine's place does not depend on the window\n");
    std::string error;
    auto real = content::ContentDb::open(real_db_path(), error);
    CHECK(real.has_value(), "the real content.db opens");
    if (!real) return;
    auto population = real->population_data(error);
    CHECK(population.has_value(), "its population chain loads");
    if (!population) return;
    const spawns::AreaConfig riverlands = spawns::AreaConfig::riverlands();
    const std::vector<spawns::RespawnPoint> points = spawns::respawn_points(*population, riverlands);
    auto nav = nav::AreaNav::load("dist/content/area_nav.bin", &error);
    CHECK(nav != nullptr, "the Riverlands' walking map opens (its window empty)");
    if (!nav) return;
    nav::AreaNav* map = nav.get();
    aoc::protocol::ServerGuidAllocator guids{60, 0x6ee87ee4u};
    PlayerWorld world(&*real, "", bs::GameStateMode::Tier3, 60, guids);
    world.set_city_nodes(g_nodes.get());
    CHECK(world.load_town_platforms("dist/content/town_platforms.bin", error), "the five towns' platforms load");
    CHECK(world.load_blueprints("dist/content/npc_motion.tsv", error), "the Blueprint table loads");
    CHECK(world.adopt_ground(std::move(nav), error), "the real ground is adopted");
    if (world.nav() == nullptr) return;

    // the same shrines asked twice: when the chunk of the walking map is not here (a player who died far away), and when it is
    std::vector<double> alone(points.size(), 0.0);
    std::vector<bool> was_ready(points.size(), false);
    for (size_t i = 0; i < points.size(); ++i) {
        if (!points[i].attuneable) continue;
        was_ready[i] = map->ready_at(points[i].x, points[i].y);
        alone[i] = world.spawn_height(points[i].x, points[i].y, points[i].z);
    }
    size_t shrines = 0, apart = 0, not_here = 0, ring_n = 0, ring_under = 0, scan_n = 0, scan_under = 0, hint_under = 0, loads_failed = 0, on_a_structure = 0;
    size_t cave_shrines = 0, ring_cave = 0, ring_cave_off = 0;
    double apart_worst = 0, ring_worst = 0, scan_worst = 0;
    std::string apart_name;
    for (size_t i = 0; i < points.size(); ++i) {
        const spawns::RespawnPoint& p = points[i];
        if (!p.attuneable) continue;
        ++shrines;
        if (!map->load_around(p.x, p.y, 1, 90)) ++loads_failed;
        const double with = world.spawn_height(p.x, p.y, p.z);
        double shrine_ground = 0;
        if (gp_ground_without_floors(world, p.x, p.y, &shrine_ground) && with - (shrine_ground + PlayerWorld::kCapsuleHalfHeightCm) > 100.0) ++on_a_structure;
        if (!was_ready[i]) {
            ++not_here;
            const double d = std::fabs(with - alone[i]);
            if (d > 100.0) {
                ++apart;
                if (d > apart_worst) {
                    apart_worst = d;
                    apart_name = p.name;
                }
            }
        }
        // the respawn ring (the spots a session puts the player at: 2 to 5 m round the shrine, its data height the hint); a shrine under the ground (the Anvils' Dunzenkell) has its
        // ring on its cave's floor, within a few metres of its own height
        const bool cave = p.z < shrine_ground - PlayerWorld::kUndergroundCm;
        if (cave) ++cave_shrines;
        for (int a = 0; a < 360; a += 10)
            for (const double r : {200.0, 350.0, 500.0}) {
                const double x = p.x + r * std::cos(a * 3.14159265358979 / 180.0), y = p.y + r * std::sin(a * 3.14159265358979 / 180.0);
                double ground = 0;
                if (!gp_ground_without_floors(world, x, y, &ground)) continue;
                const double z = world.spawn_height(x, y, p.z);
                if (cave) {
                    ++ring_cave;
                    if (std::fabs(z - p.z) > 300.0) ++ring_cave_off;
                    continue;
                }
                ++ring_n;
                if (z < ground + PlayerWorld::kCapsuleHalfHeightCm - 40.0) {
                    ++ring_under;
                    ring_worst = std::min(ring_worst, z - (ground + PlayerWorld::kCapsuleHalfHeightCm));
                }
            }
        // a point of the 100 m square round the shrine, with no height and with the height of a standing player
        for (double dx = -5000.0; dx <= 5000.0; dx += 250.0)
            for (double dy = -5000.0; dy <= 5000.0; dy += 250.0) {
                double ground = 0, s = 0, sh = 0;
                if (!gp_ground_without_floors(world, p.x + dx, p.y + dy, &ground) || !world.surface_at(p.x + dx, p.y + dy, 0.0, false, &s)) continue;
                ++scan_n;
                if (s < ground - 40.0) {
                    ++scan_under;
                    scan_worst = std::min(scan_worst, s - ground);
                }
                if (world.surface_at(p.x + dx, p.y + dy, ground + PlayerWorld::kCapsuleHalfHeightCm, true, &sh) && sh < ground - 40.0) ++hint_under;
            }
    }
    char line[360];
    CHECK(shrines == 64 && loads_failed == 0, "64 Embersprings, the chunks round each of them load");
    std::snprintf(line, sizeof line, "the respawn ring of every Emberspring (2 to 5 m round it): %zu spots, %zu of them more than 40 cm under the landscape or the town's floor (the worst %.0f cm)", ring_n, ring_under, ring_worst);
    CHECK(ring_n > 2000 && ring_under == 0, line);
    std::snprintf(line, sizeof line, "the ring of the %zu Embersprings under the ground (the Anvils' Dunzenkell): %zu spots, %zu of them more than 3 m from the shrine's own height", cave_shrines, ring_cave, ring_cave_off);
    CHECK(cave_shrines == 4 && ring_cave > 0 && ring_cave_off == 0, line);
    std::snprintf(line, sizeof line, "a point with no height, 100 m square round every Emberspring: %zu points, %zu under the landscape (the worst %.0f cm)", scan_n, scan_under, scan_worst);
    CHECK(scan_n > 30000 && scan_under == 0, line);
    std::snprintf(line, sizeof line, "... and with the height of a standing player: %zu of them under the landscape", hint_under);
    CHECK(hint_under == 0, line);
    std::snprintf(line, sizeof line, "a shrine is put in the same place with the chunk of the walking map not here and here: %zu of %zu differ by more than a metre (the worst %.0f cm, %s)", apart, not_here, apart_worst, apart_name.c_str());
    CHECK(not_here > 10 && apart == 0, line);
    CHECK(on_a_structure == 1, "with the chunk here exactly one Emberspring (the Wreckage of Carphin) stands on a structure over the landscape: the walking map shows its hull's deck");
}

// ---- the hold --------------------------------------------------------------------------------------------------------------------

static void test_a_place_in_the_air_is_not_held() {
    std::printf("a teleport to a height over the ground: the client falls from it, and the hold does not pull it back up\n");
    Harness h(cyb_on_the_ground(), on_the_ground);
    h.play();
    GmTravel travel = make_travel(h);
    const size_t from = h.got.size();
    const GmTeleportResult r = travel.teleport(to_point_z(5000.0, 3000.0, 3000.0), "the test");   // (the plain is 1,000)
    CHECK(r.status == GmStatus::Ok && near_to(r.to.z, 3000.0, 0.01), "(a height 20 m over the plain is kept as given)");
    h.run(milliseconds(100));
    gp_fall(h, r, 200.0, 0.2);   // (its own fall: 2 m)
    h.run(milliseconds(900));
    CHECK(near_to(h.s->movement().good().z, r.to.z - 200.0, 1.0), "the client's fall is its own: it is not put back up at the height");
    CHECK(corrections_on(h, h.pawn_ch, from, "PlayerPawn_C") == 1, "the teleport sent one correction and the hold none");
    gp_fall(h, r, 1000.0, 0.4);
    h.run(milliseconds(900));
    CHECK(near_to(h.s->movement().good().z, r.to.z - 1000.0, 1.0) && corrections_on(h, h.pawn_ch, from, "PlayerPawn_C") == 1, "... nor after the next metres of the fall");
}

static void test_the_hold_ends_by_itself() {
    std::printf("a teleport's hold ends by itself after 20 seconds, and a client that sends no move is sent nothing\n");
    Harness h(cyb_on_the_ground(), on_the_ground);
    h.play();
    GmTravel travel = make_travel(h);
    const size_t from = h.got.size();
    const GmTeleportResult r = travel.teleport(to_point(5000.0, 3000.0), "the test");
    CHECK(r.status == GmStatus::Ok && near_to(r.to.z, 1098.0, 1.0), "(the player is put on the plain)");
    h.run(milliseconds(21000));
    CHECK(corrections_on(h, h.pawn_ch, from, "PlayerPawn_C") == 1, "a client that sends no move in 21 seconds is sent no second correction");
    gp_fall(h, r, 200.0, 0.2);
    h.run(milliseconds(900));
    CHECK(near_to(h.s->movement().good().z, r.to.z - 200.0, 1.0) && corrections_on(h, h.pawn_ch, from, "PlayerPawn_C") == 1, "a fall after the hold's 20 seconds is the game's, not the hold's");
}

static void test_a_walking_client_under_the_place_has_a_floor() {
    std::printf("a client that lands walking a little under the place: it stands on something there, the hold is over\n");
    Harness h(cyb_on_the_ground(), on_the_ground);
    h.play();
    GmTravel travel = make_travel(h);
    const size_t from = h.got.size();
    const GmTeleportResult r = travel.teleport(to_point(5000.0, 3000.0), "the test");
    CHECK(r.status == GmStatus::Ok && near_to(r.to.z, 1098.0, 1.0), "(the player is put on the plain)");
    h.run(milliseconds(100));
    // the client's floor is a metre lower than the server's (the landscape's collision between two samples): its first walking move lands there
    g_pawn_ts += 0.2f;
    move_pawn(h, move_at(g_pawn_ts, r.to.x, r.to.y, r.to.z - 100.0, yaw16_of(h.s->movement().good().yaw_degrees), repl::kMoveWalking));
    h.run(milliseconds(1500));
    CHECK(near_to(h.s->movement().good().z, r.to.z - 100.0, 1.0), "the client's own height is not fought for (no correction puts it back up a metre)");
    CHECK(corrections_on(h, h.pawn_ch, from, "PlayerPawn_C") == 1, "the teleport sent one correction and the hold none");
}

// ---- the client's own GM calls -----------------------------------------------------------------------------------------------

static void test_the_clients_gm_calls_stand_on_the_ground() {
    std::printf("the client's own ServerTeleportXYZ and ServerTeleportPlayer: a height under the ground is lifted onto it, a height over it is kept\n");
    Harness h(cyb_on_the_ground(), on_the_ground);
    h.play();
    GmTravel travel = make_travel(h);
    h.fx.world->set_gm_port(&travel);
    h.fx.world->set_gm_commands(true);
    send_xyz(h, 3000, 2000, 500);
    CHECK(h.s->gm_rpcs_answered() == 1 && near_to(h.s->movement().good().x, 3000) && near_to(h.s->movement().good().z, 1098.0, 1.0), "ServerTeleportXYZ with a Z 5 m under the plain: the player stands on the plain (+ 98 cm)");
    send_xyz(h, 3000, 2000, 0);
    CHECK(near_to(h.s->movement().good().z, 1098.0, 1.0), "... also with a Z of 0 (the call's empty value)");
    send_player(h, "cyb", 3500, 2000, 500);
    CHECK(h.s->gm_rpcs_answered() == 3 && near_to(h.s->movement().good().x, 3500) && near_to(h.s->movement().good().z, 1098.0, 1.0), "ServerTeleportPlayer with a Z under the plain: the same");
    send_xyz(h, 3000, 2000, 2500);
    CHECK(near_to(h.s->movement().good().z, 2500.0, 0.01), "a Z over the plain is kept as given");
}
