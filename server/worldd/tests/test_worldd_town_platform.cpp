// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_town_platform.cpp  (the floor of the five city nodes' bowls)
//
//    the grid        world/town_platform.h's PlatformGrid: the lattice, UE's diagonal split of a quad, a hole under any of a quad's four corners makes it no floor, the lattice's edge, NaN, the
//                    reach (the farthest vertex that is not a hole)
//    the file        what tools/mapc/TownFloors.cs writes, on bytes made here: components that share a border row are joined into one lattice whatever order the file lists them in, a hole
//                    a neighbour does not know is a hole, a vertex no component covers is a hole, the refusals (magic, version, cut short, trailing bytes, two steps, off the lattice)
//    the frames      a platform stands at its node record: world = record + R(yaw)(u, v), z = record z + the platform's z; yaw 0, 90 and a record's own; the footprint's edge; two nodes of one
//                    layout (the nearer wins); the node that is not placed
//    the real ones   (dist/content/town_platforms.bin, from mapc town-floors; skipped without it) the two platforms' numbers; the five bowls placed at the REAL city_node records of content.db;
//                    the platform's asset set among the replicator's items of every bowl; PlayerWorld on them (ground_at, place_height, lift_out_of_bowl from the pit, floor_rescue, spawn_height); with
//                    AOC_REAL_GROUND=1 the platform against the baked pit under each town (a constant 49 to 50 m, to a few centimetres)
// ============================================================================
#include "town_platform_fixture.h"
#include "content/content_db.h"
#include "nav/area_nav.h"
#include "session/player_world.h"
#include "world/city_nodes.h"
#include "world/node_layout_data.h"
#include "world/node_wire.h"
#include "world/town_platform.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace aoc::worldd;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)
#define CHECKF(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { std::printf("  [FAIL] "); std::printf(__VA_ARGS__); std::printf("\n"); g_fail++; } \
} while (0)
// [C O A :: Cyb]

#ifndef TOWN_PLATFORMS_PATH
#define TOWN_PLATFORMS_PATH "dist/content/town_platforms.bin"
#endif
#ifndef NODES_CONTENT_DB_PATH
#define NODES_CONTENT_DB_PATH "dist/content/content.db"
#endif
#ifndef NODES_NAV_PATH
#define NODES_NAV_PATH "dist/content/area_nav.bin"
#endif

static bool near_to(double a, double b, double eps = 0.01) { return std::fabs(a - b) <= eps; }

// ---- the grid ---------------------------------------------------------------------------------------------------------------------------------

static void test_grid() {
    std::printf("the grid: the lattice, UE's diagonal, the holes, the edge\n");
    world::PlatformGrid g = tpfix::grid(30, 500.0, 0.1, 2700.0);
    double z = 0;
    CHECK(g.height(0, 0, &z) && near_to(z, 500.0), "the floor at the origin is the base");
    CHECK(g.height(1000, 700, &z) && near_to(z, 600.0), "a slope of 0.1 along u: 100 cm up for 1,000 cm along");
    CHECK(g.height(-1234.5, -2.5, &z) && near_to(z, 500.0 - 123.45, 1e-3), "between vertices the floor is interpolated");
    CHECK(g.reach > 2600.0 && g.reach <= 2700.0, "the reach is the farthest vertex that is not a hole, from the origin");
    CHECK(g.holes() > 0 && g.holes() < g.z.size(), "there are holes, and floor");
    CHECK(!g.height(2900, 0, &z) && !g.height(0, -2900, &z), "past the ring of holes there is no floor");
    CHECK(!g.height(-3001, 0, &z) && !g.height(0, 3001, &z) && !g.height(1e9, 0, &z), "outside the lattice there is none");
    CHECK(!g.height(std::nan(""), 0, &z) && !g.height(0, std::nan(""), &z), "a NaN is not on the floor");

    // UE splits each quad along its (0,0)-(1,1) diagonal: one quad of a grid whose far corner is off the plane
    world::PlatformGrid q;
    q.nx = q.ny = 2;
    q.step_x = q.step_y = 100.0;
    q.x0 = q.y0 = 0;
    q.z = {0.0f, 100.0f, 200.0f, 1000.0f};   // z00, z10, z01, z11
    q.hole = {0, 0, 0, 0};
    q.finish();
    CHECK(q.height(75, 25, &z) && near_to(z, 0 + 100 * 0.75 + (1000 - 100) * 0.25), "below the diagonal (tx >= ty): z00 + (z10 - z00) tx + (z11 - z10) ty");
    CHECK(q.height(25, 75, &z) && near_to(z, 0 + (1000 - 200) * 0.25 + 200 * 0.75), "above the diagonal (tx < ty): z00 + (z11 - z01) tx + (z01 - z00) ty");
    CHECK(q.height(50, 50, &z) && near_to(z, 500.0), "on the diagonal both agree");
    CHECK(q.height(100, 100, &z) && near_to(z, 1000.0) && q.height(0, 0, &z) && near_to(z, 0.0), "the corners (the edge of the lattice is inside it)");

    // a hole under any one corner takes the quad (and only the quads that corner belongs to)
    world::PlatformGrid h = tpfix::grid(5, 500.0, 0.0, 1e9);
    h.hole[5 * h.nx + 5] = 1;   // the vertex at (0, 0)
    h.finish();
    CHECK(!h.height(50, 50, &z) && !h.height(-50, 50, &z) && !h.height(50, -50, &z) && !h.height(-50, -50, &z), "the four quads round a hole vertex are no floor");
    CHECK(h.height(150, 50, &z) && h.height(-150, -150, &z), "...the quads beyond them are");
    CHECK(h.holes() == 1, "(one hole)");
}

// ---- the file ---------------------------------------------------------------------------------------------------------------------------------

static bool same_grid(const world::PlatformGrid& a, const world::PlatformGrid& b) {
    if (a.nx != b.nx || a.ny != b.ny || !near_to(a.x0, b.x0) || !near_to(a.y0, b.y0) || !near_to(a.step_x, b.step_x) || !near_to(a.step_y, b.step_y)) return false;
    for (size_t i = 0; i < a.z.size(); ++i) {
        if (std::fabs(a.z[i] - b.z[i]) > 0.01f || a.hole[i] != b.hole[i]) return false;
    }
    return true;
}

static void test_file() {
    std::printf("the file: components joined into one lattice, the holes, the refusals\n");
    const world::PlatformGrid a = tpfix::grid(30, 500.0, 0.1, 2700.0), b = tpfix::grid(30, 800.0, -0.05, 1500.0);
    std::vector<world::PlatformLayout> got;
    std::string err;
    std::vector<uint8_t> bytes = tpfix::file_of({{"Layout_A", a}, {"Layout_B", b}});
    CHECK(world::parse_town_platforms(bytes.data(), bytes.size(), &got, &err) && got.size() == 2, "a file of two layouts parses");
    if (got.size() != 2) {
        std::printf("    %s\n", err.c_str());
        return;
    }
    CHECK(got[0].name == "Layout_A" && got[1].name == "Layout_B" && got[0].package.find("Layout_A") != std::string::npos, "the layouts' names and packages");
    CHECK(same_grid(got[0].grid, a) && same_grid(got[1].grid, b), "16 x 16 components that share their border rows are one 61 x 61 lattice with the heights and holes they were cut from");
    CHECK(near_to(got[0].grid.reach, a.reach) && near_to(got[1].grid.reach, b.reach), "...and the same reach");
    // the order the file lists the components in does not matter
    std::vector<uint8_t> flipped = tpfix::file_of({{"Layout_A", a}, {"Layout_B", b}}, 16, 0.0, true);
    std::vector<world::PlatformLayout> got2;
    CHECK(flipped != bytes && world::parse_town_platforms(flipped.data(), flipped.size(), &got2, &err) && got2.size() == 2 && same_grid(got2[0].grid, a) && same_grid(got2[1].grid, b),
          "a file that lists its components in another order gives the same lattice");
    // a z base: the stored value is zBase + (raw - 32768) * 0.25
    std::vector<uint8_t> based = tpfix::file_of({{"Layout_A", a}}, 16, 2801.5);
    std::vector<world::PlatformLayout> got3;
    CHECK(world::parse_town_platforms(based.data(), based.size(), &got3, &err) && got3.size() == 1 && same_grid(got3[0].grid, a), "a component's zBase is added back (the real platforms have one)");

    // a hole one component knows and its neighbour does not is a hole in the lattice (fewer false floors); a vertex no component covers is a hole too
    const world::PlatformGrid open = tpfix::grid(30, 500.0, 0.1, 1e9);   // (no ring of holes: the lattice is all floor)
    std::vector<uint8_t> tweaked = tpfix::file_of({{"Layout_A", open}}, 16, 0.0, false, [](uint32_t ci, uint32_t vx, uint32_t vy, bool hole) { return (ci == 5 && vx == 15 && vy == 7) ? true : hole; });
    std::vector<world::PlatformLayout> got4;
    CHECK(world::parse_town_platforms(tweaked.data(), tweaked.size(), &got4, &err) && got4.size() == 1, "(a file with one hole bit that only the component on its left knows)");
    if (got4.size() == 1) {
        const world::PlatformGrid& t = got4[0].grid;
        const uint32_t ix = 1 * 15 + 15, iy = 1 * 15 + 7;   // component 5 = column 1, row 1 of 4 x 4; its vertex (15, 7) is the lattice's (30, 22)
        CHECK(t.hole[iy * t.nx + ix] == 1 && t.hole[iy * t.nx + ix + 1] == 0 && t.hole[iy * t.nx + ix - 1] == 0, "the shared vertex is a hole in the lattice, its neighbours are not");
        CHECK(t.holes() == 1, "(and nothing else changed)");
    }
    std::vector<uint8_t> gap = tpfix::file_of({{"Layout_A", open}}, 16, 0.0, false, nullptr, {5});   // component 5 left out
    std::vector<world::PlatformLayout> got5;
    CHECK(world::parse_town_platforms(gap.data(), gap.size(), &got5, &err) && got5.size() == 1, "(a file without one component)");
    if (got5.size() == 1) {
        const world::PlatformGrid& t = got5[0].grid;
        double z = 0;
        // component 5 covers lattice x 15..30, y 15..30; its inner vertices (16..29) are covered by no one, its border rows by the neighbours
        CHECK(t.holes() == 14 * 14 && t.hole[22 * t.nx + 22] == 1 && !t.height(-3000 + 2250.0, -3000 + 2250.0, &z), "a vertex no component covers is a hole (the 14 x 14 inside the missing component), and no quad that touches one is a floor");
        CHECK(t.hole[22 * t.nx + 15] == 0 && t.hole[10 * t.nx + 10] == 0 && t.height(-3000 + 850.0, -3000 + 850.0, &z), "...the covered ones are not");
    }

    // the refusals
    std::vector<world::PlatformLayout> none;
    CHECK(!world::parse_town_platforms(nullptr, 0, &none, &err) && err.find("magic") != std::string::npos, "nothing is not a file");
    std::vector<uint8_t> bad = bytes;
    bad[0] = 'X';
    CHECK(!world::parse_town_platforms(bad.data(), bad.size(), &none, &err) && err.find("AOCTPF01") != std::string::npos, "another magic is refused");
    bad = bytes;
    bad[8] = 2;
    CHECK(!world::parse_town_platforms(bad.data(), bad.size(), &none, &err) && err.find("version 2") != std::string::npos, "another version is refused");
    bool all_cut = true;
    for (const size_t cut : {size_t{16}, size_t{30}, size_t{100}, bytes.size() / 2, bytes.size() - 1}) {
        all_cut = all_cut && !world::parse_town_platforms(bytes.data(), cut, &none, &err);
    }
    CHECK(all_cut, "a file cut short anywhere is refused");
    bad = bytes;
    bad.push_back(0);
    CHECK(!world::parse_town_platforms(bad.data(), bad.size(), &none, &err) && err.find("after its last layout") != std::string::npos, "bytes after the last layout are refused");
    // the first component of the first layout: patch its step and its place
    const size_t first = 16 + 2 + std::string("Layout_A").size() + 2 + std::string("AOC/Content/ENV/Test/Layout_A.umap").size() + 4;
    bad = bytes;
    const double wide = 200.0;
    std::memcpy(bad.data() + first + 16, &wide, 8);
    CHECK(!world::parse_town_platforms(bad.data(), bad.size(), &none, &err) && err.find("different steps") != std::string::npos, "components with two steps are refused");
    bad = bytes;
    double x0 = 0;
    std::memcpy(&x0, bad.data() + first, 8);
    x0 += 0.5;
    std::memcpy(bad.data() + first, &x0, 8);
    CHECK(!world::parse_town_platforms(bad.data(), bad.size(), &none, &err) && err.find("off the lattice") != std::string::npos, "a component half a centimetre off the lattice is refused");
    CHECK(!world::read_town_platforms("no/such/file.bin", &none, &err) && err.find("cannot open") != std::string::npos, "a missing file says so");
}
// built by Cyb with C | O | A

// ---- the frames -------------------------------------------------------------------------------------------------------------------------------

static void test_frames() {
    std::printf("the frames: a platform stands at its node record\n");
    const double kPi = 3.14159265358979323846;
    // floor = 500 + 0.1 u, so the height tells where in the node's frame a point is
    const world::PlatformGrid g = tpfix::grid(30, 500.0, 0.1, 2700.0);
    {
        auto set = tpfix::platforms_at(7, 10000, 20000, 3000, 0.0, g);
        const auto here = set->floor_at(10000, 20000);
        CHECK(here && here->node == 7 && near_to(here->z, 3500.0) && near_to(here->u, 0) && near_to(here->v, 0), "yaw 0: the record's own place is the platform's origin, at the record's z + the platform's z");
        const auto east = set->floor_at(11000, 20000);
        CHECK(east && near_to(east->z, 3600.0) && near_to(east->u, 1000) && near_to(east->v, 0), "yaw 0: +x is +u (100 cm up per 1,000 cm)");
        const auto north = set->floor_at(10000, 21000);
        CHECK(north && near_to(north->z, 3500.0) && near_to(north->v, 1000), "yaw 0: +y is +v (no slope)");
        CHECK(!set->floor_at(10000 + 2800, 20000) && !set->floor_at(10000, 20000 + 2800) && !set->floor_at(99999, 99999), "past the ring of holes, and far away: no floor");
        CHECK(set->floor_at(10000 + 2500, 20000).has_value(), "just inside the ring: floor");
        CHECK(set->has_node(7) && !set->has_node(8) && set->frame_of(7) && near_to(set->frame_of(7)->z, 3000) && set->layout_of(7) == "Layout_Test" && set->layout_of(8).empty(), "the node is placed; another is not");
        CHECK(!set->place(9, "Layout_Nowhere", {0, 0, 0, 0}) && set->nodes() == 1 && set->layouts() == 1, "a layout the file does not have places nothing");
    }
    {
        auto set = tpfix::platforms_at(7, 10000, 20000, 3000, 90.0, g);
        const auto north = set->floor_at(10000, 21000);
        CHECK(north && near_to(north->u, 1000, 1e-6) && near_to(north->v, 0, 1e-6) && near_to(north->z, 3600.0), "yaw 90: +y is +u");
        const auto east = set->floor_at(11000, 20000);
        CHECK(east && near_to(east->u, 0, 1e-6) && near_to(east->v, -1000, 1e-6) && near_to(east->z, 3500.0), "yaw 90: +x is -v");
    }
    {
        // a record's own yaw (Joeva's): the point (u, v) = (1500, -800) of the node's frame is at record + R(yaw)(u, v)
        const double yaw = -67.37449352909202;
        auto set = tpfix::platforms_at(7, -1094396.042325, 521336.340728, 11836.445459, yaw, g);
        const double a = yaw * kPi / 180.0;
        const double wx = -1094396.042325 + 1500.0 * std::cos(a) - (-800.0) * std::sin(a), wy = 521336.340728 + 1500.0 * std::sin(a) + (-800.0) * std::cos(a);
        const auto hit = set->floor_at(wx, wy);
        CHECK(hit && near_to(hit->u, 1500.0, 1e-4) && near_to(hit->v, -800.0, 1e-4) && near_to(hit->z, 11836.445459 + 500.0 + 150.0, 1e-3), "a record's own yaw: the point maps to (u, v) = (1500, -800) and the floor is the record's z + 650");
    }
    {
        // two nodes of one layout: each point is the nearer node's
        auto set = tpfix::platforms_at(1, 0, 0, 1000, 0.0, g);
        CHECK(set->place(2, "Layout_Test", {3000, 0, 5000, 0.0}), "(a second node of the layout, 30 m east of the first)");
        const auto near_first = set->floor_at(1000, 0), near_second = set->floor_at(2200, 0), both = set->floor_at(1500, 0);
        CHECK(near_first && near_first->node == 1 && near_second && near_second->node == 2, "points near each node answer with that node");
        CHECK(both && both->node == 1 && near_to(both->z, 1000 + 500 + 150), "a point inside both footprints is the nearer node's (the first, 15 m from it and 15 m from the second: nearer by the tie rule)");
        CHECK(set->place(2, "Layout_Test", {3000, 0, 7000, 0.0}) && set->nodes() == 2 && near_to(set->floor_at(2200, 0)->z, 7000 + 500 - 80), "a node placed again is moved, not added");
    }
}

// ---- the real platforms -----------------------------------------------------------------------------------------------------------------------

static double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size())))];
}

static void test_real_platforms() {
    std::string err;
    std::vector<world::PlatformLayout> layouts;
    if (!world::read_town_platforms(TOWN_PLATFORMS_PATH, &layouts, &err)) {
        std::printf("the real platforms: [skip] %s (run: dotnet tools/mapc/bin/Release/net8.0/mapc.dll town-floors)\n", err.c_str());
        return;
    }
    std::printf("the real platforms (%s)\n", TOWN_PLATFORMS_PATH);
    CHECK(layouts.size() == 2 && layouts[0].name == "Layout_Flat_D" && layouts[1].name == "Layout_River_C", "two layouts: Flat_D (Winstead, Joeva, New Aela) and River_C (Halcyon, Miraleth)");
    for (const world::PlatformLayout& l : layouts) {
        const world::PlatformGrid& g = l.grid;
        char msg[300];
        std::snprintf(msg, sizeof msg, "%s: %u x %u vertices at %.0f cm, %zu holes (%.1f %%), reach %.0f m", l.name.c_str(), g.nx, g.ny, g.step_x, g.holes(), 100.0 * g.holes() / g.z.size(), g.reach / 100.0);
        std::printf("    %s\n", msg);
        CHECK(g.nx == 1009 && g.ny == 1009 && g.step_x == 100.0 && g.step_y == 100.0, "a lattice of 1,009 x 1,009 vertices at 100 cm (256 components of 64 x 64 sharing their border rows)");
        CHECK(g.holes() > 253000 && g.holes() < 254500, "about a quarter of it is hole (253,873 and 253,869 vertices)");
        double nearest = 1e18;
        for (uint32_t iy = 0; iy < g.ny; ++iy) {
            for (uint32_t ix = 0; ix < g.nx; ++ix) {
                if (g.hole[iy * g.nx + ix]) nearest = std::min(nearest, std::hypot(g.x0 + ix * g.step_x, g.y0 + iy * g.step_y));
            }
        }
        CHECK(nearest > 45000.0 && nearest < 45200.0, "no hole nearer than 450 m to the origin: the holes are the outside, not the town");
        CHECK(g.reach > 54000.0 && g.reach < 56000.0, "the footprint reaches 540 to 560 m (the corners of a rounded square)");
    }

    // the five bowls at the REAL node records
    auto nodes = world::CityNodes::load(NODES_CONTENT_DB_PATH, err);
    CHECK(nodes != nullptr, "the city nodes load from content.db");
    if (!nodes) return;
    world::TownPlatforms set;
    for (world::PlatformLayout& l : layouts) set.add_layout(l.name, std::move(l.grid));
    for (const world::TownBowl& bowl : world::kTownBowls) {
        const world::CityNodeInfo* n = nodes->by_guid(bowl.node);
        CHECKF(n && n->name == bowl.name, "kTownBowls names a node that content.db has: %s", bowl.name);
        if (!n) continue;
        CHECKF(set.place(bowl.node, bowl.layout, {n->x, n->y, n->z, n->yaw}), "%s is placed", bowl.name);
    }
    CHECK(set.nodes() == 5, "five bowls placed");

    // the client gets the platform: its asset set is among the replicator's items of every bowl at every node level (so the floor is there at the seed's levels, which are
    // Wilderness for Winstead, New Aela and Miraleth, and after a level change), with an identity transform: the node record's frame, nothing else
    {
        auto wire = world::NodeWire::load(NODES_CONTENT_DB_PATH, *nodes, err);
        auto data = world::NodeLayoutData::load(NODES_CONTENT_DB_PATH, err);
        CHECK(wire != nullptr && data != nullptr, "the layout data and the node wire load");
        if (wire && data) {
            for (const world::TownBowl& bowl : world::kTownBowls) {
                const world::CityNodeInfo* n = nodes->by_guid(bowl.node);
                size_t with_platform = 0, counted = 0;
                double worst_offset = 0;
                for (int level = 0; level <= 6; ++level) {
                    world::LayoutRequest req;
                    req.node = bowl.node;
                    req.context.level = level;
                    req.context.culture = 0;
                    req.context.node_type = n->type;
                    req.context.season = world::kSpring;
                    const world::CityZoneInfo* zone = n->zoi_guid != 0 ? nodes->zone_by_guid(n->zoi_guid) : nullptr;
                    req.context.biome = zone ? zone->biome : -1;
                    const auto items = data->items(*wire, req);
                    ++counted;
                    for (const auto& it : items) {
                        if (it.asset_set != bowl.asset_set) continue;
                        ++with_platform;
                        worst_offset = std::max({worst_offset, std::fabs(it.translation.x), std::fabs(it.translation.y), std::fabs(it.translation.z)});
                    }
                }
                char msg[260];
                std::snprintf(msg, sizeof msg, "%s: the platform's asset set is one of the replicator's items at all 7 node levels (%zu of %zu), its transform at most %.2f cm from the node record's", bowl.name, with_platform,
                              counted, worst_offset);
                CHECK(with_platform == 7 && worst_offset < 1.0, msg);
            }
        }
    }
// [C Y B x CoA]

    // PlayerWorld on them: the world's own ground queries
    {
        auto db = content::ContentDb::open(NODES_CONTENT_DB_PATH, err);
        CHECK(db.has_value(), "content.db opens");
        if (!db) return;
        aoc::protocol::ServerGuidAllocator guids{60, 0x6ee87ee4u};
        PlayerWorld world(&*db, "", bootstrap::GameStateMode::Tier3, 60, guids);
        world.set_city_nodes(nodes.get());
        CHECK(world.load_town_platforms(TOWN_PLATFORMS_PATH, err) && world.town_platforms() && world.town_platforms()->nodes() == 5, "PlayerWorld::load_town_platforms places the five bowls at the registry's records");
        const world::CityNodeInfo* joeva = nodes->by_guid(world::kTownBowls[1].node);
        const double mx = -1087417.6, my = 503591.5;   // Joeva's market square
        const auto square = world.bowl_floor_at(mx, my);
        CHECK(square && square->platform && square->node == joeva->guid && std::fabs(square->z - 12527.7) < 5.0, "Joeva's market square: the floor is 12,527.7");
        const auto ground = world.ground_at(mx, my);
        CHECK(ground && std::fabs(*ground - (square->z + PlayerWorld::kBowlSlackCm)) < 1e-6, "ground_at (a tp with no z): the floor + the 2 m slack, with or without the baked ground");
        CHECK(!world.ground_at(mx + 100000.0, my) && !world.bowl_floor_at(mx + 100000.0, my), "a kilometre outside the footprint: not in the bowl (and this world has no baked ground: none)");
        // the login: a spot on the baked ground under the town (z 7,748.6); a character on the floor, or only 20 m under it (the natural slope at the rim's foot), stays; one 30 m under is put back
        const double ux = -1079493.1, uy = 508464.4;
        const auto ufloor = world.bowl_floor_at(ux, uy);
        const auto lifted = world.lift_out_of_bowl(ux, uy, 7748.6);
        CHECK(ufloor && lifted && lifted->node == joeva->guid && std::fabs(lifted->z - (ufloor->z + 98.0 + PlayerWorld::kBowlSlackCm)) < 1e-6 && lifted->under_cm > 4800.0 && lifted->under_cm < 5100.0,
              "login: a character saved on the baked pit under Joeva (49 m down) joins on the platform's floor + the capsule + the slack, and is told how far under it was");
        CHECK(ufloor && !world.lift_out_of_bowl(ux, uy, ufloor->z + 98.0) && !world.lift_out_of_bowl(ux, uy, ufloor->z + 98.0 - 2000.0) && world.lift_out_of_bowl(ux, uy, ufloor->z + 98.0 - 3000.0) &&
              !world.lift_out_of_bowl(ux, uy, ufloor->z + 5000.0),
              "login: on the floor, above it (a tower) or 20 m under it: left alone; 30 m under it: put back");
        // the safety net
        const double floor_z = square->z;
        CHECK(!world.floor_rescue(mx, my, floor_z + 298.0) && !world.floor_rescue(mx, my, floor_z - 9000.0) && !world.floor_rescue(mx, my, floor_z + 30000.0), "the net: on the floor, 90 m under it and 300 m over it: nothing");
        const auto r1 = world.floor_rescue(mx, my, floor_z - 10100.0);
        CHECK(r1 && r1->node == joeva->guid && std::fabs(r1->x - mx) < 1e-9 && std::fabs(r1->z - (floor_z + 98.0 + PlayerWorld::kBowlSlackCm)) < 1e-6 && r1->where.find("Joeva") != std::string::npos,
              "the net: more than 100 m under Joeva's floor: put back on it (floor + 98 + 200), in the same x and y, and the log says whose floor");
        const auto r2 = world.floor_rescue(mx, my, -50000.0);
        CHECK(r2 && std::fabs(r2->z - (floor_z + 298.0)) < 1e-6, "the net: z -50,000 inside the bowl (a fall without end): the same");
        CHECK(!world.floor_rescue(mx + 100000.0, my, 5000.0) && !world.floor_rescue(mx + 100000.0, my, -9999.0), "the net: outside every bowl nothing happens above the kill plane (-10,000)");
        const auto r3 = world.floor_rescue(mx + 100000.0, my, -20000.0);
        CHECK(r3 && r3->node == 0 && r3->where == "the login spot", "the net: under the kill plane where the server has no ground at all: the login spot");
        // a respawn point inside a bowl's footprint
        bool grounded = false;
        const double shrine_z = world.spawn_height(mx, my, 7000.0, &grounded);
        CHECK(grounded && std::fabs(shrine_z - (floor_z + 98.0 + PlayerWorld::kBowlSlackCm)) < 1e-6, "respawn: a shrine in a bowl comes back on the floor + the capsule + the slack, not the pit's height");
        CHECK(std::fabs(world.spawn_height(mx + 100000.0, my, 7000.0, &grounded) - 7000.0) < 1e-9 && !grounded, "respawn: a shrine outside every bowl keeps its data height when the world has no ground");
    }

    // with AOC_REAL_GROUND=1 the platform against the baked pit under each town: the pit is the platform's shape, 49 to 50 m down
    const char* real_ground = std::getenv("AOC_REAL_GROUND");
    if (!real_ground || !*real_ground) {
        std::printf("the platform against the baked pit: opt-in (AOC_REAL_GROUND=1 reads the heightfield of %s)\n", NODES_NAV_PATH);
        return;
    }
    std::printf("the platform against the baked pit under each town (%s)\n", NODES_NAV_PATH);
    auto nav = nav::AreaNav::load(NODES_NAV_PATH, &err);
    CHECK(nav != nullptr, "the chunked ground opens");
    if (!nav) return;
    for (const world::TownBowl& bowl : world::kTownBowls) {
        const world::CityNodeInfo* n = nodes->by_guid(bowl.node);
        std::vector<double> d;
        for (double r = 2500.0; r <= 35000.0; r += 2500.0) {
            for (double deg = 0; deg < 360.0; deg += 6.0) {
                const double a = deg * 3.14159265358979323846 / 180.0;
                const double x = n->x + r * std::cos(a), y = n->y + r * std::sin(a);
                double baked = 0;
                const auto floor = set.floor_at(x, y);
                if (!floor || !nav->terrain_height(x, y, &baked)) continue;
                d.push_back(floor->z - baked);
            }
        }
        const double k = percentile(d, 0.5);
        std::vector<double> dev;
        for (const double v : d) dev.push_back(std::fabs(v - k));
        const double mad = percentile(dev, 0.5);
        char msg[260];
        std::snprintf(msg, sizeof msg, "%s: the platform's floor is %.2f m over the baked pit (%zu points within 350 m of the node's place), the median deviation %.1f cm", bowl.name, k / 100.0, d.size(), mad);
        std::printf("    %s\n", msg);
        CHECK(d.size() > 800 && k > 4800.0 && k < 5050.0 && mad < 25.0, msg);
    }
    // ... except where a hill of the landscape pokes through Joeva's town: at (u, v) = (-90 m, -80 m) the platform is flat at +47 m and the landscape reaches +162 m over the record's z,
    // so PlayerWorld::bowl_floor_at takes the higher of the two there
    {
        const world::CityNodeInfo* n = nodes->by_guid(world::kTownBowls[1].node);
        const double a = n->yaw * 3.14159265358979323846 / 180.0, u = -9000.0, v = -8000.0;
        const double x = n->x + u * std::cos(a) - v * std::sin(a), y = n->y + u * std::sin(a) + v * std::cos(a);
        double baked = 0;
        const auto floor = set.floor_at(x, y);
        const bool have = floor && nav->terrain_height(x, y, &baked);
        char msg[260];
        std::snprintf(msg, sizeof msg, "Joeva: at (-90 m, -80 m) from the node the platform is %.1f m over the record and the landscape %.1f m: the landscape is %.0f m higher (a hill through the town)",
                      have ? (floor->z - n->z) / 100.0 : 0.0, have ? (baked - n->z) / 100.0 : 0.0, have ? (baked - floor->z) / 100.0 : 0.0);
        std::printf("    %s\n", msg);
        CHECK(have && baked - floor->z > 10000.0 && baked - floor->z < 13000.0, msg);
    }
}

int main() {
    spdlog::set_level(spdlog::level::warn);
    test_grid();
    test_file();
    test_frames();
    test_real_platforms();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
