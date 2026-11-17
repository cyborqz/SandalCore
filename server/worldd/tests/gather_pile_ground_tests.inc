// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/gather_pile_ground_tests.inc  (included by test_worldd_gather_session.cpp, after the rig)
//
//  Where the pile of a full bag lies against the ground the data has: never under the landscape where the landscape is (a cave, a cellar or a tunnel floor that the walking
//  map has under a slope too steep to walk is not the ground of a player standing on the slope), and in a cave when the player is in it; never far from the player's feet
//  when the data has no floor for them.  First on the test ground (a ramp over a cave), then on the Riverlands' real ground at the places the data has plants.
// ============================================================================
#include "nav/area_nav.h"
#include "session/pile_ground.h"

#include <fstream>
#include <sstream>

/// The test ground's plain, with a ramp too steep to walk (1.5 cm up for each cm along: 56 degrees) from x 10,000 to the plateau at 12,000, and a cave under it: a floor 3.3 m
/// under the ramp's surface at x 10,750.
static nav::Geometry ramp_over_a_cave() {
    nav::Geometry g = navfix::test_geometry();
    for (nav::TerrainGrid& t : g.terrain) {
        for (uint32_t iy = 0; iy < t.ny; ++iy)
            for (uint32_t ix = 0; ix < t.nx; ++ix) {
                const double wx = t.x0 + ix * t.step_x;
                if (wx < 10000.0) continue;
                const double h = std::min(3000.0, 1.5 * (wx - 10000.0));
                t.raw[iy * t.nx + ix] = static_cast<uint16_t>(32768 + std::lround(h));
            }
    }
    navfix::place_box(g, 0, 10600, 10900, -300, 300, 1500, 1800);   // the cave's floor: its top is 1,800, the ramp's surface over it 1,900 to 2,300
    return g;
}

/// A world on `file` with the player's pile asked for at (x, y) for a body at z: where it lies.
static double pile_height(PlayerWorld& world, double x, double y, double body_z) { return pile_ground(world, x, y, body_z); }

static void test_pile_not_in_a_cave_under_the_slope() {
    std::printf("a full bag's pile on a slope too steep to walk lies on the slope, not on the cave floor under it; in the cave it lies in the cave\n");
    nav::BuildOptions options;
    options.threads = 2;
    options.nav_margin_cm = 3000;
    nav::NavFile file;
    nav::BuildStats stats;
    std::string error;
    if (!nav::build_nav(ramp_over_a_cave(), 7, options, &file, &stats, &error)) {
        std::printf("  [FAIL] the test ground builds: %s\n", error.c_str());
        return;
    }
    Rig rig;
    if (!rig.open(g_path, 0.001, &file)) return;
    PlayerWorld& world = *rig.world;
    const double x = 10750.0, y = 0.0;
    double ground = 0;
    CHECK(world.nav() != nullptr && world.nav()->terrain_height(x, y, &ground), "(the landscape has a height at the spot)");
    char msg[260];
    std::snprintf(msg, sizeof msg, "(the landscape at (%.0f, %.0f) is %.0f cm: the ramp 1,000 + 1.5 x 750)", x, y, ground);
    CHECK(std::fabs(ground - 2125.0) < 1.0, msg);
    const std::vector<double> on_the_slope = world.floors_at(x, y, ground - 40.0, ground + 150.0);
    const std::vector<double> in_the_cave = world.floors_at(x, y, ground - 400.0, ground - 40.0);
    CHECK(on_the_slope.empty(), "(the walking map has no floor on the slope: it is too steep to walk)");
    CHECK(in_the_cave.size() == 1 && std::fabs(in_the_cave.front() - 1800.0) < 25.0, "(and one under it, within 4 m: the cave's floor, the box's top at 1,800)");
    if (in_the_cave.empty()) return;
    const double cave = in_the_cave.front();

    const double slope_pile = pile_height(world, x, y, ground + PlayerWorld::kCapsuleHalfHeightCm);
    std::snprintf(msg, sizeof msg, "a player on the slope (feet %.0f cm): the pile lies on the slope at %.0f cm, not on the cave floor at %.0f cm under it", ground, slope_pile, cave);
    CHECK(std::fabs(slope_pile - ground) <= 1.0, msg);

    // a player standing 1.2 m over the slope's surface, on something the walking map does not show: the pile is at his feet, not on the landscape under them
    const double rock_pile = pile_height(world, x, y, ground + 120.0 + PlayerWorld::kCapsuleHalfHeightCm);
    std::snprintf(msg, sizeof msg, "a player 1.2 m over the slope's surface on something the map does not show: the pile lies at his feet (%.0f cm), not on the landscape (%.0f cm)", ground + 120.0, rock_pile);
    CHECK(std::fabs(rock_pile - (ground + 120.0)) <= 1.0, msg);

    const double cave_pile = pile_height(world, x, y, cave + PlayerWorld::kCapsuleHalfHeightCm);
    std::snprintf(msg, sizeof msg, "a player standing in the cave (feet %.0f cm, 3 m under the slope): the pile lies on the cave floor at %.0f cm", cave, cave_pile);
    CHECK(std::fabs(cave_pile - cave) <= 1.0, msg);

    // a body in the air over the plain (a GM's teleport to a height): no floor within reach, the ground 20 m under it: the pile is at its feet, not 20 m down
    const double air = 3000.0;
    const double air_pile = pile_height(world, 0.0, 0.0, air);
    std::snprintf(msg, sizeof msg, "a player 20 m over the plain with no floor under him: the pile lies at his feet (%.0f cm), not on the plain 20 m down (%.0f cm)", air - PlayerWorld::kCapsuleHalfHeightCm, air_pile);
    CHECK(std::fabs(air_pile - (air - PlayerWorld::kCapsuleHalfHeightCm)) <= 1.0, msg);
}

// ---- the real ground -----------------------------------------------------------------------------------------------------------------

/// The (x, y) of the plants the data places (region_foliage.tsv: the gatherable foliage of the Riverlands), every `step`-th row.
static std::vector<std::pair<double, double>> real_plants(const std::string& path, size_t step) {
    std::vector<std::pair<double, double>> out;
    std::ifstream in(path);
    std::string line;
    size_t row = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (row++ % step != 0) continue;
        std::vector<std::string> cells;
        std::stringstream ss(line);
        std::string cell;
        while (std::getline(ss, cell, '\t')) cells.push_back(cell);
        if (cells.size() < 9) continue;
        out.emplace_back(std::atof(cells[6].c_str()), std::atof(cells[7].c_str()));
    }
    return out;
}

static void test_pile_on_the_real_ground() {
    std::printf("a full bag's pile on the Riverlands' real ground: round the plants of the data, never under the landscape\n");
    const std::string nav_path = "dist/content/area_nav.bin", plants_path = "dist/content/region_foliage.tsv";
    if (!std::filesystem::exists(nav_path) || !std::filesystem::exists(plants_path)) {
        std::printf("  [skip] no dist/content/area_nav.bin or region_foliage.tsv: the real ground is not checked\n");
        return;
    }
    std::string error;
    auto nav = nav::AreaNav::load(nav_path, &error);
    CHECK(nav != nullptr, "the Riverlands' walking map opens");
    if (!nav) return;
    nav::AreaNav* map = nav.get();
    Rig rig;
    if (!rig.open(g_path)) return;
    CHECK(rig.world->adopt_ground(std::move(nav), error), "the real ground is adopted");
    if (rig.world->nav() == nullptr) return;
    PlayerWorld& world = *rig.world;
    // a player stands 2 m from a plant, on the landscape, in each of eight directions: the 4,000 plants the data has in a stride, each with its eight places
    const std::vector<std::pair<double, double>> plants = real_plants(plants_path, 20);
    CHECK(plants.size() > 3000, "the data has plants to stand round");
    size_t spots = 0, under = 0, off = 0, on = 0, load_failed = 0;
    double worst = 0, worst_x = 0, worst_y = 0;
    for (const auto& [px, py] : plants) {
        if (!map->load_around(px, py, 1, 90)) ++load_failed;
        for (int a = 0; a < 360; a += 45) {
            const double x = px + 200.0 * std::cos(a * 3.14159265358979 / 180.0), y = py + 200.0 * std::sin(a * 3.14159265358979 / 180.0);
            double terrain = 0;
            if (!map->terrain_height(x, y, &terrain)) continue;
            if (world.town_ground_at(x, y)) continue;   // (a town's bowl has its own floor)
            ++spots;
            const double pile = pile_height(world, x, y, terrain + PlayerWorld::kCapsuleHalfHeightCm);
            if (pile < terrain - 40.0) {
                ++under;
                if (pile - terrain < worst) {
                    worst = pile - terrain;
                    worst_x = x;
                    worst_y = y;
                }
            } else if (pile > terrain + 150.0) {
                ++off;
            } else {
                ++on;
            }
        }
    }
    char msg[360];
    std::snprintf(msg, sizeof msg, "%zu places 2 m from a plant: %zu of the piles under the landscape by more than 40 cm (the worst %.0f cm at (%.0f, %.0f)), %zu more than 1.5 m over it, %zu within 40 cm under to 1.5 m over", spots, under, worst,
                  worst_x, worst_y, off, on);
    CHECK(spots > 20000 && under == 0 && off == 0, msg);
    CHECK(load_failed == 0, "the chunks round every plant load");
}
