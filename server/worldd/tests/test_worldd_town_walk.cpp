// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_town_walk.cpp  (towns, slice 2a: the streets)
//
//  What a town person needs to walk, on values made in the test (no game files, no database):
//
//    the floor     a synthetic platform (a layout's grid with a hole) goes into a geometry with nav/town_geometry, the navmesh is built from it, and the ground queries find the platform's
//                  floor at the node's place, in the node's yaw, and nothing at the hole or at the pit's terrain 25 m under it
//    the routes    a route placed by a city node is walked at the node's record (world = record + R(yaw) (x, y), z = record z + z); an open-world route is not moved
//    the kinds     vendors, clerks and stations stand whatever their instance says; a villager and a guard keep the instance's life; a beast, and a flyer among the beasts, is told by its archetype's path
//    the speeds    a guard's patrol is MaxWalkSpeed x the expression (no gain), a villager's has the 1.05 gain
// ============================================================================
#include "nav/area_nav.h"
#include "nav/nav_build.h"
#include "nav/town_geometry.h"
#include "nav_fixture.h"
#include "npc/area_npcs.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace aoc::worldd;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

// ---- a platform ---------------------------------------------------------------------------------------------------------------------------

namespace {

constexpr uint32_t kN = 131;               // vertices a side: 130 quads, three blocks a side (63, 63, 4)
constexpr double kStep = 100.0, kOrigin = -6500.0;
constexpr double kPlatformZ = 500.0;       // the platform's own z (flat)
constexpr double kNodeX = 5000.0, kNodeY = -3000.0, kNodeZ = 3000.0, kNodeYaw = 90.0;   // the node's record: the floor is at 3,500, the baked terrain (1,000) 25 m under it

world::PlatformLayout make_layout() {
    world::PlatformLayout l;
    l.name = "Layout_Test";
    world::PlatformGrid& g = l.grid;
    g.step_x = g.step_y = kStep;
    g.x0 = g.y0 = kOrigin;
    g.nx = g.ny = kN;
    g.z.assign(static_cast<size_t>(kN) * kN, static_cast<float>(kPlatformZ));
    g.hole.assign(g.z.size(), 0);
    // a hole of 5 x 5 vertices off the centre: u -2,500..-2,100, v -500..-100 (its quads cover u -2,600..-2,000, v -600..0)
    for (uint32_t iy = 60; iy <= 64; ++iy) {
        for (uint32_t ix = 40; ix <= 44; ++ix) g.hole[static_cast<size_t>(iy) * kN + ix] = 1;
    }
    g.finish();
    return l;
}

nav::Geometry make_geometry(nav::TownGeometryStats* stats) {
    nav::Geometry g = navfix::test_geometry();
    const std::vector<world::PlatformLayout> layouts{make_layout()};
    nav::TownFrame f;
    f.node = 1;
    f.layout = "Layout_Test";
    f.x = kNodeX; f.y = kNodeY; f.z = kNodeZ;
    f.yaw_degrees = kNodeYaw;
    std::string err;
    if (!nav::add_town_platforms(&g, layouts, {f}, stats, &err)) {
        std::fprintf(stderr, "town geometry: %s\n", err.c_str());
        std::abort();
    }
    return g;
}

/// The node's frame of a point of the platform: world = record + R(yaw) (u, v).
void to_world(double u, double v, double* x, double* y) {
    const double yaw = kNodeYaw * 3.14159265358979323846 / 180.0;
    *x = kNodeX + u * std::cos(yaw) - v * std::sin(yaw);
    *y = kNodeY + u * std::sin(yaw) + v * std::cos(yaw);
}

}  // namespace

static void test_floor() {
    std::printf("the floor: a platform in the walking map\n");
    nav::TownGeometryStats st;
    const nav::Geometry g = make_geometry(&st);
    const size_t quads = 130 * 130, left_out = 6 * 6;
    CHECK(st.layouts == 1 && st.nodes == 1 && st.blocks == 9 && st.placements == 9 && st.unplaced.empty(), "one layout of nine blocks (63 + 63 + 4 quads a side), nine placements, none unplaced");
    CHECK(st.holes_skipped == left_out && st.triangles == (quads - left_out) * 2, "the quads around the hole are left out, the rest are two triangles each");
    const size_t base_meshes = navfix::test_geometry().meshes.size();
    CHECK(g.meshes.size() == base_meshes + 9 && g.placements.size() == navfix::test_geometry().placements.size() + 9, "the platform's blocks were appended to the map's own meshes and placements");

    // the same inputs, the same signature; a different place, another one
    nav::TownGeometryStats st2;
    const nav::Geometry g2 = make_geometry(&st2);
    CHECK(nav::town_geometry_hash(g, base_meshes, navfix::test_geometry().placements.size()) == nav::town_geometry_hash(g2, base_meshes, navfix::test_geometry().placements.size()),
          "the signature of the added geometry is the same for the same inputs");
    nav::Geometry moved = g2;
    moved.placements.back().t[2] += 1.0f;
    CHECK(nav::town_geometry_hash(moved, base_meshes, navfix::test_geometry().placements.size()) != nav::town_geometry_hash(g, base_meshes, navfix::test_geometry().placements.size()),
          "a platform moved by a centimetre has another signature");

    nav::BuildOptions opt;
    opt.threads = 2;
    opt.nav_margin_cm = 3000;
    nav::NavFile f;
    nav::BuildStats bs;
    std::string err;
    CHECK(nav::build_nav(g, 7, opt, &f, &bs, &err), "the navmesh builds with the platform in it");
    if (bs.failed_tiles != 0) std::printf("  (failed tiles: %d)\n", bs.failed_tiles);
    CHECK(bs.failed_tiles == 0, "no tile failed (none held more polygons than a tile may)");
    auto navmesh = nav::AreaNav::from_file(f, &err);
    CHECK(navmesh != nullptr, "the file loads");
    if (!navmesh) return;

    double x, y, z = 0;
    const double floor_z = kNodeZ + kPlatformZ;
    to_world(1000, 2000, &x, &y);
    CHECK(navmesh->ground_height(x, y, floor_z, &z, 300, 150) && std::fabs(z - floor_z) < 12.0, "the floor is at the record's z plus the platform's, at the place the node's yaw gives (u 1,000, v 2,000)");
    CHECK(navmesh->ground_height(x, y, 1000, &z, 1500, 1500) && std::fabs(z - 1000.0) < 12.0, "from the height of the baked terrain 25 m under it, the terrain is the ground (the platform is out of reach)");
    CHECK(navmesh->ground_height(x, y, floor_z + 60, &z, 300, 150) && std::fabs(z - floor_z) < 12.0, "an origin 60 cm over the floor still finds it in the tight window");
    CHECK(!navmesh->ground_height(x, y, floor_z + 600, &z, 300, 150), "a point 6 m over it (a roof's height) is not on the floor in the tight window");

    // the hole: u -2,300, v -300
    to_world(-2300, -300, &x, &y);
    CHECK(!navmesh->ground_height(x, y, floor_z, &z, 300, 150), "over the hole there is no floor in the tight window (the pit is 25 m down)");
    // the yaw: the hole is not where the unrotated platform would have it
    const double ux = kNodeX - 2300, uy = kNodeY - 300;
    CHECK(navmesh->ground_height(ux, uy, floor_z, &z, 300, 150) && std::fabs(z - floor_z) < 12.0, "and the place the hole would have with no yaw has a floor (the yaw turns the platform counter-clockwise)");
    // outside the platform
    CHECK(!navmesh->ground_height(kNodeX + 8000, kNodeY, floor_z, &z, 300, 150), "beyond the platform's edge there is no floor at its height");
}

// ---- the routes and the kinds ---------------------------------------------------------------------------------------------------------------

static world::WorldActor person(const char* leaf) {
    world::WorldActor a;
    a.kind = world::ActorKind::Npc;
    a.npc.archetype = std::string("/Game/Test/") + leaf + ".Default__" + leaf + "_C";
    a.yaw_degrees = 0;
    return a;
}

static void test_routes() {
    std::printf("the routes: placed at the node\n");
    world::CityNodeInfo node;
    node.x = 5000; node.y = -3000; node.z = 3000; node.yaw = 90;
    double x, y, z;
    npc::place_node_route(node, 100, 0, 7, &x, &y, &z);
    CHECK(std::fabs(x - 5000) < 1e-6 && std::fabs(y - (-2900)) < 1e-6 && std::fabs(z - 3007) < 1e-6, "a quarter turn: (100, 0) is 100 cm along +y of the record, z added");
    node.yaw = 0;
    npc::place_node_route(node, 100, 50, -3, &x, &y, &z);
    CHECK(std::fabs(x - 5100) < 1e-6 && std::fabs(y - (-2950)) < 1e-6 && std::fabs(z - 2997) < 1e-6, "no yaw: the offset is added as it is");
    node.yaw = 180;
    npc::place_node_route(node, 100, 50, 0, &x, &y, &z);
    CHECK(std::fabs(x - 4900) < 1e-6 && std::fabs(y - (-3050)) < 1e-6, "a half turn: both offsets change sign");

    npc::MotionTable table;   // empty: the Blueprint defaults (MaxWalkSpeed 600)
    const npc::SpeedModel speeds;
    content::NpcInstanceRow patrol;
    patrol.ambient_behavior = "Patrol";
    patrol.speed_expr = "0.3";
    patrol.patrol_route = 1;
    content::NpcRouteRow route;
    route.relative_city_node = true;
    route.waypoints = {{0, 0, 0}, {1000, 0, 0}, {1000, 1000, 10}};
    const world::WorldActor villager = person("Test_Villager");
    node.yaw = 90;
    const npc::NpcMotionSetup placed = npc::make_setup(villager, table, &patrol, &route, speeds, nullptr, &node);
    CHECK(placed.ambient == npc::Ambient::Patrol && placed.route.size() == 3, "a node's route with its node is a patrol of three stops");
    CHECK(placed.route.size() == 3 && std::fabs(placed.route[1].x - 5000) < 1e-6 && std::fabs(placed.route[1].y - (-2000)) < 1e-6 && std::fabs(placed.route[2].x - 4000) < 1e-6 &&
              std::fabs(placed.route[2].y - (-2000)) < 1e-6 && std::fabs(placed.route[2].z - 3010) < 1e-6,
          "its stops are the record plus the turned offsets, the height the record's plus the stop's");
    const npc::NpcMotionSetup nowhere = npc::make_setup(villager, table, &patrol, &route, speeds);
    CHECK(nowhere.route.empty(), "the same route with no node has no place: nothing to walk");

    content::NpcRouteRow open = route;
    open.relative_city_node = false;
    const npc::NpcMotionSetup in_world = npc::make_setup(villager, table, &patrol, &open, speeds, nullptr, &node);
    CHECK(in_world.route.size() == 3 && in_world.route[1].x == 1000 && in_world.route[1].y == 0 && in_world.route[2].z == 10, "an open-world route (already placed) is left as it is, node or not");

    content::NpcRouteRow encounter = route;
    encounter.relative_city_node = false;
    encounter.relative_encounter = true;
    CHECK(npc::make_setup(villager, table, &patrol, &encounter, speeds, nullptr, &node).route.empty(), "a route of an encounter is not walked");

    // the node's own stationary instance (a carrier the data marks as standing) with a long route stands
    content::NpcInstanceRow posted = patrol;
    posted.ambient_behavior = "Stationary";
    const npc::NpcMotionSetup stands = npc::make_setup(villager, table, &posted, &route, speeds, nullptr, &node);
    CHECK(stands.ambient == npc::Ambient::Stationary && stands.route.empty(), "a villager the data marks Stationary stands, with its long route or without");
}

static void test_kinds_and_speeds() {
    std::printf("the kinds and the speeds\n");
    CHECK(npc::stands_by_kind("CraftingStation_Carpentry_T1_C") && npc::stands_by_kind("CraftingStation_Commodity_Starter_C"), "crafting stations stand");
    CHECK(npc::stands_by_kind("Node_Mayor_Vendor_C") && npc::stands_by_kind("Node_GuildCommodityVendor_KAE_C") && npc::stands_by_kind("ArtisanshipPOI_Smithy_Vendor_C") && npc::stands_by_kind("Node_BlackMarket_Vendor_C"),
          "vendors stand");
    CHECK(npc::stands_by_kind("Node_Warehouse_StorageClerk_KAE_C") && npc::stands_by_kind("Node_BuyOrderManager_KAE_C") && npc::stands_by_kind("Node_TownHall_GuildManager_KAE_C") &&
              npc::stands_by_kind("Node_CitizenshipManager_KAE_C"),
          "clerks and managers stand");
    CHECK(npc::stands_by_kind("ProcessingStation_Test_C") && npc::stands_by_kind("CommissionBoard_Node_C") && npc::stands_by_kind("CommissionTurnInBox_NodeDefault_C") && npc::stands_by_kind("Node_Mayor_Vendor_C") &&
              npc::stands_by_kind("Node_MemorialSite_Priest_KAE_C"),
          "boards, boxes, processing stations and the town's service NPCs stand");
    CHECK(!npc::stands_by_kind("Node_Villager_F_C") && !npc::stands_by_kind("Node_Villager_M_Hammer_C") && !npc::stands_by_kind("Guard_Soldier_C") && !npc::stands_by_kind("Wolf_C") && !npc::stands_by_kind("") && !npc::stands_by_kind("Node_Merchant_M_C") &&
              !npc::stands_by_kind("Node_Scholar_F_C") && !npc::stands_by_kind("Node_Villager_M_Carrying_C") && !npc::stands_by_kind("Node_Priest_M_C"),
          "villagers, carriers, fillers (merchant, scholar, priest), guards and animals do not");
    CHECK(npc::stands_by_kind("ServiceBuilding_Caravansary_CrateSalesman_C") && npc::stands_by_kind("ServiceBuilding_Caravansary_CaravanMaster_C") && npc::stands_by_kind("ServiceBuilding_Smithy_General_C") &&
              npc::stands_by_kind("Commission_Board_Lionhold_Temp_C") && npc::stands_by_kind("KAE_L3_ServiceDesk_C"),
          "the construction crate vendor (the caravansary's crate salesman), the caravan master, a service building's general NPC and the boards stand");
    CHECK(npc::is_guard_leaf("Guard_Soldier_C") && npc::is_guard_leaf("Guard_Wizard_C") && !npc::is_guard_leaf("Node_Villager_F_C") && !npc::is_guard_leaf("Wolf_C"), "guards are told by their Blueprint");

    // the beasts (the creatures a SnapToGround instance stands on the ground) and the flyers among them, told by the archetype's path
    const std::string npc_dir = "/Game/DesignAssets/Population/NPC/";
    const std::string wolf = npc_dir + "Beast/Canine/Wolf.Default__Wolf_C", rabid = npc_dir + "Beast/Rabid/Rabid.Default__Rabid_C", pig = npc_dir + "Beast/Pig/Pig_0.Default__Pig_0_C";
    const std::string raven = npc_dir + "Beast/Bird/Raven/Raven.Default__Raven_C", bat = npc_dir + "Beast/Bat/Bat.Default__Bat_C", gryphon = npc_dir + "Beast/Gryphon/Gryphon.Default__Gryphon_C";
    const std::string dragon = npc_dir + "Beast/ElderDragon/ElderDragon_Base.Default__ElderDragon_Base_C", insects = npc_dir + "Beast/InsectCloud/InsectCloud.Default__InsectCloud_C";
    const std::string guard_wizard = npc_dir + "PCRaces/Guards/Guard_Wizard.Default__Guard_Wizard_C", goblin = npc_dir + "Humanoid/Goblin/Goblin_Gravehide.Default__Goblin_Gravehide_C";
    const std::string zom = npc_dir + "Monster/Aberration/Beholder/Ocular/BloodshotZom.Default__BloodshotZom_C";
    CHECK(npc::is_beast_archetype(wolf) && npc::is_beast_archetype(rabid) && npc::is_beast_archetype(pig) && npc::is_beast_archetype(raven) && npc::is_beast_archetype(bat) && npc::is_beast_archetype(gryphon),
          "wolves, rabids, pigs, ravens, bats and gryphons are beasts");
    CHECK(!npc::is_beast_archetype(guard_wizard) && !npc::is_beast_archetype(goblin) && !npc::is_beast_archetype(zom) && !npc::is_beast_archetype("Wolf_C") && !npc::is_beast_archetype(""),
          "a guard, a goblin, a monster, a bare class name and nothing are not");
    CHECK(npc::is_flying_beast_archetype(raven) && npc::is_flying_beast_archetype(bat) && npc::is_flying_beast_archetype(gryphon) && npc::is_flying_beast_archetype(dragon) && npc::is_flying_beast_archetype(insects),
          "ravens, bats, gryphons, elder dragons and insect clouds fly");
    CHECK(!npc::is_flying_beast_archetype(wolf) && !npc::is_flying_beast_archetype(rabid) && !npc::is_flying_beast_archetype(pig) && !npc::is_flying_beast_archetype(guard_wizard) && !npc::is_flying_beast_archetype(""),
          "wolves, rabids, pigs, a guard and nothing do not");

    npc::MotionTable table;
    const npc::SpeedModel speeds;
    content::NpcInstanceRow inst;
    inst.ambient_behavior = "Default";
    CHECK(npc::make_setup(person("Node_TestVendor_Vendor"), table, &inst, nullptr, speeds).ambient == npc::Ambient::Stationary, "a vendor of a Default instance stands");
    CHECK(npc::make_setup(person("CraftingStation_Test_T1"), table, &inst, nullptr, speeds).ambient == npc::Ambient::Stationary, "a station of a Default instance stands");
    CHECK(npc::make_setup(person("Node_Test_StorageClerk_KAE"), table, &inst, nullptr, speeds).ambient == npc::Ambient::Stationary, "a clerk of a Default instance stands");
    CHECK(npc::make_setup(person("Node_Test_Villager_F"), table, &inst, nullptr, speeds).ambient == npc::Ambient::Wander, "a villager of a Default instance wanders");
    inst.ambient_behavior = "Patrol";
    CHECK(npc::make_setup(person("Node_TestVendor_Vendor"), table, &inst, nullptr, speeds).ambient == npc::Ambient::Stationary, "a vendor of a Patrol instance stands too");

    // a town person the data gives a vendor (its instance's or its template's) stands whatever its Blueprint is called; a guard with an interaction keeps its life
    {
        world::CityNodeInfo town;
        content::NpcInstanceRow wander;
        wander.ambient_behavior = "Default";
        content::NpcInstanceRow seller = wander;
        seller.vendor = 77;
        CHECK(npc::make_setup(person("Kaelar_Test_Citizen"), table, &seller, nullptr, speeds, nullptr, &town).ambient == npc::Ambient::Stationary, "a town person whose instance has a vendor stands");
        world::WorldActor talker = person("Kaelar_Test_Citizen");
        talker.npc.interact_def = 5;
        CHECK(npc::make_setup(talker, table, &wander, nullptr, speeds, nullptr, &town).ambient == npc::Ambient::Stationary, "a town person whose template has an interaction stands");
        world::WorldActor guard = person("Guard_Test");
        guard.npc.interact_def = 5;
        CHECK(npc::make_setup(guard, table, &wander, nullptr, speeds, nullptr, &town).ambient == npc::Ambient::Wander, "a guard with an interaction keeps the instance's life");
        CHECK(npc::make_setup(person("Kaelar_Test_Citizen"), table, &seller, nullptr, speeds).ambient == npc::Ambient::Wander, "the data rule is for town people (a node is given): the open world is unchanged");
        npc::NpcMotionSetup worker = npc::make_setup(person("Node_Test_Villager_Hammer"), table, &wander, nullptr, speeds, nullptr, &town);
        worker.wander_max = 2000;
        npc::keep_to_workshop(&worker, npc::kWorkshopRadiusCm);
        CHECK(worker.ambient == npc::Ambient::Wander && worker.wander_max == npc::kWorkshopRadiusCm && worker.wander_min <= worker.wander_max, "a workshop worker's stroll is cut to a few metres round its workshop");
    }

    inst.speed_expr = "0.10";
    const double guard = npc::make_setup(person("Guard_Test"), table, &inst, nullptr, speeds).patrol_speed;
    CHECK(std::fabs(guard - 60.0) < 0.01, "a guard's patrol with the expression 0.10 walks 60 cm/s of a Blueprint speed of 600 (no gain)");
    inst.speed_expr = "0.3";
    const double carrier = npc::make_setup(person("Node_Test_Villager_Carrying"), table, &inst, nullptr, speeds).patrol_speed;
    CHECK(std::fabs(carrier - 189.0) < 0.01, "a villager's with 0.3 walks 189 (the 1.05 gain stays)");
    inst.speed_expr = "0.2";
    CHECK(std::fabs(npc::make_setup(person("Node_Test_Villager_Carrying"), table, &inst, nullptr, speeds).patrol_speed - 126.0) < 0.01, "and with 0.2 walks 126");
    CHECK(std::fabs(npc::make_setup(person("Wolf"), table, &inst, nullptr, speeds).patrol_speed - 126.0) < 0.01, "an animal keeps the gain");
}

int main() {
    test_floor();
    test_routes();
    test_kinds_and_speeds();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
