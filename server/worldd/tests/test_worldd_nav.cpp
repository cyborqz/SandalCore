// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_nav.cpp  (plan E1, stage 2)
//
//  The ground: nav/nav_build (Recast), nav/nav_file and nav/area_nav (Detour), on a small world built in memory (tests/nav_fixture.h: a flat
//  plain with a hill, a wall, a floating platform and a closed pen) and, when dist/content/area_nav.bin exists, on the real area.
//
//    build         the same geometry gives the same bytes whatever the thread count; the file round trips; short, wrong and padded files are refused
//    ground        the terrain's height is exact; a floor above the ground is a surface only when the height asked is near it
//    paths         a path goes round a wall (longer than the straight line, on the ground all the way), an enclosed point is reached as closely as
//                  the navmesh gets (partial), a point far outside has no path
//    rays          a ray along the ground stops at the wall, goes through open ground
//    hostile       NaN and enormous coordinates, a missing file, a geometry file cut short or with a wrong index, a tile of the wrong size
//    border marks  every polygon the builder makes has a detail edge marked on its border (near the origin and 300,000 cm out), a file whose polygons have none is repaired as it
//                  loads and answers questions next to them, a polygon without a detail triangle is hidden (nav/tile_repair.h)
//    the real area (dist/content/area_nav.bin, or AOC_AREA_NAV): the answer key of the extraction (tests/ground_key.h: the captured ground
//                  positions of the area) lies on the heightfield or the navmesh; the navmesh is one piece around the login spot
// ============================================================================
#include "nav/area_nav.h"
#include "nav/nav_build.h"
#include "nav/tile_repair.h"
#include "nav_fixture.h"
#include "ground_key.h"
#include "ground_key_region.h"
#include "spawns/region_data.h"

#include <DetourNavMesh.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace aoc::worldd;
using aoc::worldd::nav::Vec3;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

static void test_build() {
    std::printf("build\n");
    const auto a = nav::serialize_nav_file(navfix::build_file(1));
    const auto b = nav::serialize_nav_file(navfix::build_file(4));
    const auto c = nav::serialize_nav_file(navfix::build_file(1));
    CHECK(!a.empty() && a == b, "one thread or four: the same bytes");
    CHECK(a == c, "built twice: the same bytes");
    nav::NavFile f;
    std::string err;
    CHECK(nav::parse_nav_file(a.data(), a.size(), &f, &err) && nav::serialize_nav_file(f) == a, "the file parses and writes back the same bytes");
    CHECK(f.tiles.size() > 50 && f.terrain.size() == 16, "tiles over the area and the 16 terrain grids");

    std::vector<uint8_t> cut(a.begin(), a.end() - 3);
    CHECK(!nav::parse_nav_file(cut.data(), cut.size(), &f, &err) && !err.empty(), "a cut-short file is refused");
    std::vector<uint8_t> bad = a;
    bad[0] = 'X';
    CHECK(!nav::parse_nav_file(bad.data(), bad.size(), &f, &err), "a wrong magic is refused");
    std::vector<uint8_t> pad = a;
    pad.push_back(0);
    CHECK(!nav::parse_nav_file(pad.data(), pad.size(), &f, &err), "a trailing byte is refused");
    CHECK(!nav::parse_nav_file(a.data(), 20, &f, &err), "a header cut short is refused");
    nav::NavFile broken;
    {
        std::string e2;
        nav::parse_nav_file(a.data(), a.size(), &broken, &e2);
        broken.tiles[0].data.resize(broken.tiles[0].data.size() / 2);   // a tile Detour cannot read
    }
    std::string lerr;
    CHECK(nav::AreaNav::from_file(std::move(broken), &lerr) == nullptr && !lerr.empty(), "a tile of the wrong size is refused with a message");
    std::string merr;
    CHECK(nav::AreaNav::load("no/such/area_nav.bin", &merr) == nullptr && merr.find("cannot open") != std::string::npos, "a missing file: no navmesh, 'cannot open'");
// C-Y-B - C-O-A

    // the geometry file
    const auto gb = nav::serialize_geometry(navfix::geometry());
    nav::Geometry g2;
    CHECK(nav::parse_geometry(gb.data(), gb.size(), &g2, &err) && g2.placements.size() == navfix::geometry().placements.size() && g2.meshes[0].i.size() == 36,
          "the geometry file parses");
    std::vector<uint8_t> gcut(gb.begin(), gb.end() - 1);
    CHECK(!nav::parse_geometry(gcut.data(), gcut.size(), &g2, &err), "a geometry file cut short is refused");
    nav::Geometry wrong = navfix::geometry();
    wrong.meshes[0].i[5] = 99;   // an index past the vertices
    const auto wb = nav::serialize_geometry(wrong);
    CHECK(!nav::parse_geometry(wb.data(), wb.size(), &g2, &err) && err.find("index") != std::string::npos, "a triangle index out of range is refused");
    nav::Geometry wrongp = navfix::geometry();
    wrongp.placements[0].mesh = 5;
    const auto wpb = nav::serialize_geometry(wrongp);
    CHECK(!nav::parse_geometry(wpb.data(), wpb.size(), &g2, &err), "a placement of an unknown mesh is refused");
    auto excessive = nav::serialize_geometry(nav::Geometry{});
    const uint32_t placements = 100000000;
    std::memcpy(excessive.data() + 52, &placements, sizeof placements);
    CHECK(!nav::parse_geometry(excessive.data(), excessive.size(), &g2, &err) && err.find("counts") != std::string::npos,
          "a tiny geometry file cannot allocate millions of placements from its header");
}

static void test_ground() {
    std::printf("ground\n");
    auto& n = navfix::shared_nav();
    double z = 0;
    CHECK(n.terrain_height(1000, 1000, &z) && std::fabs(z - 1000.0) < 1e-6, "the plain is 1,000 exactly");
    CHECK(n.terrain_height(6000, 0, &z) && std::fabs(z - 1400.0) < 1e-6, "the hill's top is 1,400");
    CHECK(n.terrain_height(7500, 0, &z) && std::fabs(z - 1200.0) < 1e-6, "halfway down the hill: 1,200");
    CHECK(!n.terrain_height(500000, 0, &z), "no terrain far outside");
    CHECK(n.ground_height(1000, 1000, 1100, &z) && std::fabs(z - 1000.0) < 1e-6, "ground height on the plain: the terrain");
    CHECK(n.ground_height(6000, 0, 1500, &z) && std::fabs(z - 1400.0) < 1e-6, "ground height on the hill: the terrain");
    CHECK(n.ground_height(9000, -5000, 1560, &z) && std::fabs(z - 1540.0) < 15.0, "on the platform: its top (1,540), with the hint near it");
    CHECK(n.ground_height(9000, -5000, 1000, &z) && std::fabs(z - 1000.0) < 1e-6, "under the platform with the hint at the ground: the ground");
    CHECK(!n.ground_height(1000, 1000, 5000, &z), "a hint far above any surface: none within reach");
    CHECK(!n.ground_height(500000, 500000, 0, &z), "far outside: no ground");
    CHECK(!n.ground_height(std::nan(""), 0, 0, &z), "NaN: no ground");
    CHECK(n.on_navmesh({1000, 1000, 1000}), "a point on the plain is on the navmesh");
    CHECK(!n.on_navmesh({-3000, 0, 1300}), "a point inside the wall (half way up) is not");
    CHECK(!n.on_navmesh({1000, 1000, 3000}), "a point 2,000 cm above the plain is not");
    CHECK(n.on_navmesh({9000, -5000, 1540}), "the platform's top is on the navmesh");
    Vec3 np;
    CHECK(n.nearest_point({-3000, 0, 1300}, 300, 400, &np) && (std::fabs(np.z - 1000) < 40 || std::fabs(np.z - 1600) < 40), "the nearest navmesh point to one in the wall is on the ground under it or the wall's top");
    CHECK(!n.nearest_point({1e12, 0, 0}, 300, 300, &np), "a point at 1e12 has no nearest point");
    CHECK(n.polygon_count() > 100, "the navmesh has polygons");
}

static void test_floor_stack() {
    std::printf("floor stack (every floor at a place)\n");
    auto& n = navfix::shared_nav();
    // the platform at (9000, -5000) floats 500 cm over the plain: two floors, the platform's top over the plain
    auto stack = n.floors_at(9000, -5000, 0, 3000);
    CHECK(stack.size() == 2 && std::fabs(stack[0] - 1540.0) < 15.0 && std::fabs(stack[1] - 1000.0) < 15.0, "under and on the floating platform: two floors, the top first (1,540) and the plain under it (1,000)");
    stack = n.floors_at(9000, -5000, 1200, 3000);
    CHECK(stack.size() == 1 && std::fabs(stack[0] - 1540.0) < 15.0, "a range from 1,200 up: the platform's top alone");
    stack = n.floors_at(9000, -5000, 0, 1300);
    CHECK(stack.size() == 1 && std::fabs(stack[0] - 1000.0) < 15.0, "a range up to 1,300: the plain alone");
    stack = n.floors_at(1000, 1000, 0, 3000);
    CHECK(stack.size() == 1 && std::fabs(stack[0] - 1000.0) < 15.0, "on the open plain: one floor");
    stack = n.floors_at(6000, 0, 0, 3000);
    CHECK(stack.size() == 1 && stack[0] > 1340.0 && stack[0] < 1420.0, "on the hill: one floor, near its top (1,400)");
    CHECK(n.floors_at(-3000, 0, 1100, 1550).empty(), "inside the wall (half way up): none");
    CHECK(n.floors_at(1000, 1000, 3000, 5000).empty(), "a range far over every surface: none");
    CHECK(n.floors_at(500000, 500000, 0, 3000).empty(), "far outside: none");
    CHECK(n.floors_at(std::nan(""), 0, 0, 3000).empty() && n.floors_at(0, 0, 3000, 0).empty() && n.floors_at(0, 0, std::nan(""), 3000).empty(), "NaN and a reversed range: none");
    // the nearest floor to a hint is the stack's member nearest to it
    double z = 0;
    stack = n.floors_at(9000, -5000, 0, 3000);
    CHECK(n.ground_height(9000, -5000, 1560, &z) && stack.size() == 2 && std::fabs(z - stack[0]) < 1e-3 && n.ground_height(9000, -5000, 1000, &z) && std::fabs(z - stack[1]) < 15.0,
          "ground_height picks from the same stack: the hint chooses the floor");
}

static double path_ground_error(nav::AreaNav& n, const nav::PathResult& p) {
    double worst = 0;
    for (const auto& pt : p.points) {
        double z = 0;
        if (!n.ground_height(pt.x, pt.y, pt.z, &z)) return 1e9;
        worst = std::max(worst, std::fabs(z - pt.z));
    }
    return worst;
}

static void test_paths() {
    std::printf("paths and rays\n");
    auto& n = navfix::shared_nav();
    const Vec3 a{-5000, 0, 1000}, b{-1000, 0, 1000};
    auto p = n.find_path(a, b);
    CHECK(p.found && p.complete && p.points.size() >= 3, "a path round the wall exists and has corners");
    CHECK(p.length > 5000 && p.length < 7500, "it is longer than the straight line (4,000) and goes round the wall's ends");
    CHECK(std::hypot(p.points.front().x - a.x, p.points.front().y - a.y) < 100 && std::hypot(p.points.back().x - b.x, p.points.back().y - b.y) < 100, "it starts and ends at the points asked");
    CHECK(path_ground_error(n, p) < 25.0, "every point of it is on the ground (within 25 cm of the surface)");
    bool through_wall = false;
    for (size_t i = 0; i + 1 < p.points.size(); ++i)
        if ((p.points[i].x < -3000) != (p.points[i + 1].x < -3000) && std::fabs(0.5 * (p.points[i].y + p.points[i + 1].y)) < 1900) through_wall = true;
    CHECK(!through_wall, "no segment of it crosses the wall");

    auto q = n.find_path({-5000, 6600, 1000}, {-8400, 6600, 1000});
    CHECK(q.found && !q.complete, "a point inside the closed pen: a partial path to the closest point outside");
    auto r = n.find_path({-5000, 0, 1000}, {900000, 0, 1000});
    CHECK(!r.found && r.points.empty(), "a point far outside: no path");
    auto s = n.find_path({std::nan(""), 0, 0}, b);
    CHECK(!s.found, "a NaN start: no path");
    auto same = n.find_path(a, a);
    CHECK(same.found && same.complete && same.length < 1.0, "from a point to itself: a path of no length");
    auto hill = n.find_path({3000, -2000, 1000}, {9000, 2000, 1000});
    CHECK(hill.found && hill.complete && path_ground_error(n, hill) < 25.0, "a path over the hill follows its surface");

    Vec3 hit;
    CHECK(!n.raycast({-5000, 0, 1000}, {-1000, 0, 1000}, &hit) && hit.x > -3400 && hit.x < -3000, "a ray along the ground across the wall stops before it");
    CHECK(n.raycast({-5000, 3000, 1000}, {-1000, 3000, 1000}, &hit), "a ray along open ground reaches its end");
    CHECK(n.line_of_sight({0, 0, 1000}, {2000, 0, 1000}) && !n.line_of_sight({-5000, 0, 1000}, {-1000, 0, 1000}), "line of sight along the ground: open yes, through the wall no");
    CHECK(!n.raycast({900000, 0, 0}, {0, 0, 0}, &hit), "a ray from outside the navmesh does not start");

    int ok = 0;
    for (int i = 0; i < 50; ++i) {
        Vec3 out;
        const double u1 = (i * 0.137) - std::floor(i * 0.137), u2 = (i * 0.271) - std::floor(i * 0.271), u3 = (i * 0.419) - std::floor(i * 0.419);
        (void)u3;
        if (n.random_point({0, 0, 1000}, 500, u1, u2, &out) && std::hypot(out.x, out.y) <= 500 && n.on_navmesh(out, 60, 120)) ++ok;
    }
    CHECK(ok >= 45, "50 random points within 500 cm of the centre: (nearly) all land on the navmesh and inside the circle");
    Vec3 o1, o2;
    n.random_point({0, 0, 1000}, 500, 0.3, 0.6, &o1);
    n.random_point({0, 0, 1000}, 500, 0.3, 0.6, &o2);
    CHECK(o1.x == o2.x && o1.y == o2.y, "the same numbers give the same point");
}
// CoA : C Y B

// ---- the Riverlands' key (G1 stage 1): where the retail captures' players walked, anywhere in the region ----------------------------------------------
static void test_region_key(nav::AreaNav& n) {
    std::printf("the region's answer key\n");
    constexpr size_t kCount = sizeof(tests::kGroundKeyRegion) / sizeof(tests::kGroundKeyRegion[0]);
    // the places, a few chunks at a time (the window holds 4,096 tiles: the whole region's key does not fit)
    std::vector<size_t> order(kCount);
    for (size_t i = 0; i < kCount; ++i) order[i] = i;
    auto chunk_of = [&](size_t i) { return n.chunk_at(tests::kGroundKeyRegion[i].x, tests::kGroundKeyRegion[i].y); };
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { const auto ca = chunk_of(a), cb = chunk_of(b); return ca < cb || (ca == cb && a < b); });
    int strict = 0, relaxed = 0, on_nav = 0, outside_terrain = 0, total = 0, loaded_ok = 0, batches = 0;
    int near_total = 0, near_hit = 0, near_nav = 0, high_total = 0;   // near: within 30 m of the heightfield; high: more than 30 m above it
    std::map<std::string, int> misses_by;
    std::vector<std::string> misses;
    // (review B I3) the key per zone of influence: where it is, and how well the ground agrees with it there
    namespace rg = aoc::worldd::spawns::region;
    struct ZoneTally { int total = 0, high = 0, near_total = 0, near_hit = 0; };
    std::vector<ZoneTally> zones(rg::kZoiCount + 1);   // the last: in no zone
    const auto zone_of = [&](double x, double y) {
        for (size_t z = 0; z < rg::kZoiCount; ++z)
            for (uint32_t r = rg::kZois[z].first; r < rg::kZois[z].first + rg::kZois[z].count; ++r) {
                const rg::Rect& q = rg::kZoiRects[r];
                if (x >= q.x0 && x < q.x1 && y >= q.y0 && y < q.y1) return z;
            }
        return static_cast<size_t>(rg::kZoiCount);
    };
    for (size_t at = 0; at < order.size();) {
        std::vector<std::pair<double, double>> pts;
        size_t end = at;
        std::set<std::pair<int, int>> chunks;
        while (end < order.size()) {
            const auto c = chunk_of(order[end]);
            if (!chunks.count({c.x, c.y}) && chunks.size() >= 12) break;
            chunks.insert({c.x, c.y});
            pts.emplace_back(tests::kGroundKeyRegion[order[end]].x, tests::kGroundKeyRegion[order[end]].y);
            ++end;
        }
        if (n.windowed() && n.load_around(pts, 0, 120)) ++loaded_ok;
        ++batches;
        for (size_t i = at; i < end; ++i) {
            const auto& k = tests::kGroundKeyRegion[order[i]];
            ++total;
            double tz = 0, gz = 0;
            const bool has_t = n.terrain_height(k.x, k.y, &tz);
            if (!has_t) ++outside_terrain;
            const bool terrain_strict = has_t && k.z - tz >= 90.0 && k.z - tz <= 108.0;
            bool floor_strict = false;
            if (!terrain_strict && n.ground_height(k.x, k.y, k.z - 98.0, &gz, 60.0, 60.0) && k.z - gz >= 90.0 && k.z - gz <= 108.0) floor_strict = true;
            bool hit_relaxed = terrain_strict || floor_strict;
            // a rider on a mount: the mount's capsule is lower (a terror bird 57 cm), a wider band keeps them
            if (!hit_relaxed && has_t && k.z - tz >= 40.0 && k.z - tz <= 125.0) hit_relaxed = true;
            if (!hit_relaxed && n.ground_height(k.x, k.y, k.z - 70.0, &gz, 70.0, 70.0) && k.z - gz >= 40.0 && k.z - gz <= 125.0) hit_relaxed = true;
            if (terrain_strict || floor_strict) ++strict;
            if (hit_relaxed) ++relaxed;
            const bool on = n.on_navmesh({k.x, k.y, k.z - 98.0}, 80, 120);
            if (on) ++on_nav;
            ZoneTally& zt = zones[zone_of(k.x, k.y)];
            ++zt.total;
            if (has_t && k.z - tz > 3000.0) { ++high_total; ++zt.high; }
            else if (has_t) { ++near_total; ++zt.near_total; if (hit_relaxed) { ++near_hit; ++zt.near_hit; } if (on) ++near_nav; }
            if (!hit_relaxed) {
                char buf[200];
                std::snprintf(buf, sizeof buf, "capture %d (%.0f, %.0f, %.1f): terrain %s%.1f below", k.capture, k.x, k.y, k.z, has_t ? "" : "none ", has_t ? k.z - tz : 0.0);
                misses.push_back(buf);
            }
        }
        at = end;
    }
    std::printf("  region key: %d walking positions (the header holds %zu), in %d batches of chunks (%d loaded in full); no terrain under %d\n", total, kCount, batches, loaded_ok, outside_terrain);
    std::printf("  on the ground within a player's capsule (90 to 108 cm): %d / %d = %.1f %%; with a mount's lower capsule (40 to 125 cm): %d = %.1f %%; on the navmesh within 80 cm: %d = %.1f %%\n", strict, total,
                total ? 100.0 * strict / total : 0.0, relaxed, total ? 100.0 * relaxed / total : 0.0, on_nav, total ? 100.0 * on_nav / total : 0.0);
    std::printf("  %d positions stand more than 30 m above the heightfield (the retail players were 40 to 57 m above it, idle for minutes: 115 in the node Joeva's ZOI, 38 in Winstead's; the node towns' own ground, or a map of another patch: it is not in the cooked map's heightfield); of the other %d, %d lie on the heightfield or a floor (%.1f %%) and %d on the navmesh\n",
                high_total, near_total, near_hit, near_total ? 100.0 * near_hit / near_total : 0.0, near_nav);
    for (size_t z = 0; z <= rg::kZoiCount; ++z) {
        const ZoneTally& zt = zones[z];
        if (zt.total) std::printf("    %-46s %3d positions: %3d more than 30 m above the heightfield, %3d near it, %3d of those on it or a floor\n", z < rg::kZoiCount ? rg::kZois[z].name : "(in no zone)", zt.total, zt.high, zt.near_total, zt.near_hit);
    }
    int zones_with_key = 0;
    for (size_t z = 0; z < rg::kZoiCount; ++z) zones_with_key += zones[z].total > 0 ? 1 : 0;
    std::printf("  the key has positions in %d of the footprint's %zu zones of influence: the extraction is checked against the heightfield in those only (the in-game walks are the rest of the evidence)\n", zones_with_key, rg::kZoiCount);
    for (size_t i = 0; i < misses.size() && i < 25; ++i) std::printf("    miss: %s\n", misses[i].c_str());
    CHECK(total > 200, "the region's key has hundreds of walking positions");
    CHECK(outside_terrain == 0, "there is terrain under every one (the extraction covers the footprint)");
    CHECK(near_total > 150 && near_hit >= near_total * 0.90, "of the positions within 30 m of the heightfield, at least 90 % lie on it or a floor of the navmesh (E1's tolerance, a mount's capsule allowed)");
    CHECK(near_nav >= near_total * 0.85, "and 85 % on the navmesh within 80 cm (the rest stands near a navmesh edge: steep ground, the capsule's reach)");
    CHECK(high_total * 2 <= total, "the positions far above the heightfield are at most half of the key (they are reported per zone, not judged)");
    bool each_zone = true;
    for (size_t z = 0; z < rg::kZoiCount; ++z)
        if (zones[z].near_total >= 5 && zones[z].near_hit * 10 < zones[z].near_total * 9) { each_zone = false; std::printf("    zone %s: %d of %d on the ground\n", rg::kZois[z].name, zones[z].near_hit, zones[z].near_total); }
    CHECK(each_zone, "and in every zone that has at least five positions within 30 m of the heightfield, 90 % of them lie on it or a floor (not only in the total)");
}

// ---- the real area -----------------------------------------------------------------------------------------------------------------
static void test_real_area() {
    std::printf("the real area\n");
    const char* env = std::getenv("AOC_AREA_NAV");
    const std::string path = env && *env ? env : "dist/content/area_nav.bin";
    std::string err;
    auto n = nav::AreaNav::load(path, &err);
    if (!n) {
        std::printf("  [skip] %s: %s (run tools/mapc: mapc build)\n", path.c_str(), err.c_str());
        return;
    }
    if (n->windowed()) {
        // a chunked file (G1): its window is empty; the key's places are what the test reads, so the chunks around them come in first (the key lies in E1's circle: a few dozen chunks)
        std::vector<std::pair<double, double>> places;
        for (const auto& k : tests::kGroundKey) places.emplace_back(k.x, k.y);
        places.emplace_back(n->center_x(), n->center_y());   // the login spot
        const bool ok = n->load_around(places, 0, 120);
        std::printf("  chunked file: %zu chunks, %zu tiles in the file; the window holds %zu tiles after loading around the key's %zu places\n", n->chunk_count(), n->total_tiles(), n->tile_count(), places.size());
        CHECK(ok, "the chunks around the answer key's places load");
    }
    std::printf("  area_nav.bin: %zu terrain grids, %zu tiles, %zu polygons\n", n->terrain_grids(), n->tile_count(), n->polygon_count());
    CHECK(std::fabs(n->center_x() - (-777664.5)) < 1e-6 && std::fabs(n->center_y() - 616578.6875) < 1e-6 && n->radius() == 100000.0,
          "the file was cut for the login spot and 1 km (spawns/area.h)");

    // the answer key: captured ground positions of the area (tests/ground_key.h).  A walking pawn's origin is a capsule's half height above
    // its ground (players 98 cm; an NPC's depends on its class, 62 to 144 cm in the captures), so the ground under a player must be 98 cm below
    // its origin; for an NPC any surface 40 to 160 cm below counts.
    int players_ground = 0, players_hit_terrain = 0, players_hit_nav = 0, players_miss = 0, air = 0;
    int npc_ground = 0, npc_hit = 0, npc_miss = 0;
    std::map<std::string, int> why;
    std::vector<std::string> misses;
    for (const auto& k : tests::kGroundKey) {
        const bool person = k.kind == tests::GroundKind::Player || k.kind == tests::GroundKind::OtherPlayer || k.kind == tests::GroundKind::Mount;
        if (k.kind == tests::GroundKind::Player && k.mode != 1) { ++air; continue; }   // not walking (falling in a jump's arc, mode 0 and 16 samples of the same arcs): not ground
        double tz = 0;
        const bool has_t = n->terrain_height(k.x, k.y, &tz);
        double gz = 0;
        const double lo = person ? 90.0 : 40.0, hi = person ? 108.0 : 160.0;
        bool hit = false, on_terrain = false;
        // the surfaces under the point: the terrain, and the navmesh floors (any height within reach)
        if (has_t && k.z - tz >= lo && k.z - tz <= hi) { hit = true; on_terrain = true; }
        if (!hit) {
            if (n->ground_height(k.x, k.y, k.z - 98.0, &gz, 60.0, 60.0) && k.z - gz >= lo && k.z - gz <= hi) hit = true;
        }
        if (!hit && !person) {
            for (double hint = k.z - 62.0; hint >= k.z - 160.0 && !hit; hint -= 20.0)
                if (n->ground_height(k.x, k.y, hint, &gz, 12.0, 12.0) && k.z - gz >= lo && k.z - gz <= hi) hit = true;
        }
        if (person) {
            ++players_ground;
            if (hit) (on_terrain ? players_hit_terrain : players_hit_nav)++; else { ++players_miss; }
        } else {
            ++npc_ground;
            if (hit) ++npc_hit; else ++npc_miss;
        }
        if (!hit) {
            char buf[200];
            std::snprintf(buf, sizeof buf, "kind %d capture %d (%.0f, %.0f, %.1f) mode %d: terrain %s%.1f below", static_cast<int>(k.kind), k.capture, k.x, k.y, k.z, k.mode,
                          has_t ? "" : "none ", has_t ? k.z - tz : 0.0);
            misses.push_back(buf);
        }
    }
    const int total_ground = players_ground + npc_ground;
    const int total_hit = players_hit_terrain + players_hit_nav + npc_hit;
    std::printf("  key: %zu positions; not walking (jumps) %d; on the ground %d: players/mounts %d (terrain %d, a floor %d, miss %d), NPCs %d (hit %d, miss %d)\n",
                sizeof(tests::kGroundKey) / sizeof(tests::kGroundKey[0]), air, total_ground, players_ground, players_hit_terrain, players_hit_nav, players_miss, npc_ground, npc_hit,
                npc_miss);
    std::printf("  ground hit rate: %d / %d = %.1f %%\n", total_hit, total_ground, total_ground ? 100.0 * total_hit / total_ground : 0.0);
    for (const auto& m : misses) std::printf("    miss: %s\n", m.c_str());
    CHECK(total_ground > 250, "the key has hundreds of ground positions");
    CHECK(total_hit >= total_ground * 0.97, "at least 97 % of the captured ground positions lie on the heightfield or a floor of the navmesh");
// [C | O | A ~ C.Y.B]

    // on the navmesh: the login spot and what the captured players walked on
    const Vec3 login{n->center_x(), n->center_y(), 15952.05};
    Vec3 np;
    CHECK(n->nearest_point(login, 200, 400, &np) && std::hypot(np.x - login.x, np.y - login.y) < 200, "the login spot is on the navmesh (within 2 m)");
    int on_nav = 0, probed = 0;
    for (const auto& k : tests::kGroundKey) {
        if (k.mode != 1 || k.kind != tests::GroundKind::Player) continue;
        ++probed;
        if (n->on_navmesh({k.x, k.y, k.z - 98.0}, 80, 120)) ++on_nav;
    }
    std::printf("  walking players: %d of %d on the navmesh within 80 cm\n", on_nav, probed);
    CHECK(probed > 100 && on_nav >= probed * 0.90, "at least 90 % of the walking players' ground positions lie on the navmesh (the rest stands near a navmesh edge: steep ground, the capsule's reach)");

    // paths between captured places: two walking players' positions of one capture 5 to 80 m apart lie on one walked route, so a path joins them and it is
    // not far longer than the straight line; two NPCs of one retail capture to 60 m apart are on ground the navmesh joins (creatures wander within their radius)
    struct Pair { int pairs = 0, found = 0, short_enough = 0; double ratio_sum = 0; };
    Pair walked, npcs;
    std::map<int, std::vector<const tests::GroundKey*>> by_capture;
    for (const auto& k : tests::kGroundKey) {
        const bool player_walk = k.kind == tests::GroundKind::Player && k.mode == 1;
        if (player_walk || k.kind == tests::GroundKind::Npc) by_capture[k.capture * 2 + (player_walk ? 0 : 1)].push_back(&k);
    }
    for (const auto& [id, rows] : by_capture) {
        const bool is_player = id % 2 == 0;
        Pair& acc = is_player ? walked : npcs;
        const double lo = is_player ? 500.0 : 1000.0, hi = is_player ? 8000.0 : 6000.0;
        for (size_t a = 0; a < rows.size(); a += (is_player ? 3 : 1)) {
            const tests::GroundKey* from = rows[a];
            const tests::GroundKey* to = nullptr;
            double best = 1e30;
            for (size_t b = 0; b < rows.size(); ++b) {
                const double d = std::hypot(rows[b]->x - from->x, rows[b]->y - from->y);
                if (d >= lo && d <= hi && d < best) { best = d; to = rows[b]; }
            }
            if (!to) continue;
            ++acc.pairs;
            const double foot = is_player ? 98.0 : 96.0;
            auto ground_of = [&](const tests::GroundKey* k) {
                double gz = 0;
                if (!n->ground_height(k->x, k->y, k->z - foot, &gz, 150, 150)) gz = k->z - foot;
                return Vec3{k->x, k->y, gz};
            };
            const auto path = n->find_path(ground_of(from), ground_of(to), 150, 300);
            if (path.found && path.complete) {
                ++acc.found;
                acc.ratio_sum += path.length / best;
                if (path.length <= best * 3.0 + 500.0) ++acc.short_enough;
            }
        }
    }
    std::printf("  paths between captured places: walking players %d pairs, %d joined, %d at most 3x the straight line (mean ratio %.2f); NPCs %d pairs, %d joined\n", walked.pairs, walked.found,
                walked.short_enough, walked.found ? walked.ratio_sum / walked.found : 0.0, npcs.pairs, npcs.found);
    CHECK(walked.pairs > 25 && walked.found >= walked.pairs * 0.95, "a path joins 95 % of the pairs of walked places of one capture");
    CHECK(walked.short_enough >= walked.pairs * 0.90, "and 90 % of those paths are at most three times the straight line (plus 5 m)");
    CHECK(npcs.pairs > 5 && npcs.found >= npcs.pairs * 0.80, "a path joins 80 % of the pairs of NPC places of one capture");

    if (n->has_region()) test_region_key(*n);

    // A polygon of the Riverlands' file whose triangles had no marked border edge (Recast's float test, nav/tile_repair.h): a question beside it, from a wandering NPC at (-622,437, 594,734), killed
    // worldd.exe with a null dereference in Detour.  The chunk comes in repaired (or, in a file made after the fix, needed nothing) and the question is answered.
    if (n->windowed() && n->has_region()) {
        const bool in = n->load_around(-622436.6, 594733.7, 0, 60);
        nav::Vec3 np;
        const bool found = in && n->nearest_point({-622436.6, 594733.7, 10545.9}, 300, 500, &np);
        CHECK(in && found, "the question that once killed worldd.exe (beside a polygon without a marked border edge) is answered");
        const nav::TileRepair tr = n->window_stats().tile_repair;
        std::printf("    the chunks that came in needed %llu border edges marked, %llu polygons had none, %llu hidden\n", static_cast<unsigned long long>(tr.edges_flagged),
                    static_cast<unsigned long long>(tr.polys_repaired), static_cast<unsigned long long>(tr.polys_hidden));
        CHECK(tr.polys_hidden == 0, "and no polygon of them is hidden");
    }
}

// ---- the detail meshes' border edges (nav/tile_repair.h) -------------------------------------------------------------------------------------------------
//  Detour finds the point of a polygon nearest to a point outside it from the detail triangle edges marked as on the polygon's border, and dereferences null for a polygon with none
//  (worldd.exe died of it when an NPC began to wander next to one).  Recast's float test for the mark loses edges in centimetres far from the origin.

namespace {

/// The arrays of a Detour tile that the checks read and damage (dtCreateNavMeshData's layout).
struct TileView {
    dtMeshHeader h{};
    dtPoly* polys = nullptr;
    dtPolyDetail* dmeshes = nullptr;
    unsigned char* dtris = nullptr;
};

TileView view_of(std::vector<uint8_t>& t) {
    TileView v;
    std::memcpy(&v.h, t.data(), sizeof v.h);
    const auto al4 = [](size_t x) { return (x + 3) & ~static_cast<size_t>(3); };
    const size_t o_polys = al4(sizeof(dtMeshHeader)) + al4(sizeof(float) * 3 * static_cast<size_t>(v.h.vertCount));
    const size_t o_dm = o_polys + al4(sizeof(dtPoly) * static_cast<size_t>(v.h.polyCount)) + al4(sizeof(dtLink) * static_cast<size_t>(v.h.maxLinkCount));
    const size_t o_dt = o_dm + al4(sizeof(dtPolyDetail) * static_cast<size_t>(v.h.detailMeshCount)) + al4(sizeof(float) * 3 * static_cast<size_t>(v.h.detailVertCount));
    v.polys = reinterpret_cast<dtPoly*>(t.data() + o_polys);
    v.dmeshes = reinterpret_cast<dtPolyDetail*>(t.data() + o_dm);
    v.dtris = t.data() + o_dt;
    return v;
}

constexpr unsigned char kAnyBorder = 0x01 | 0x04 | 0x10;   // DT_DETAIL_EDGE_BOUNDARY in each of a triangle's three edges

/// Ground polygons of a tile, and how many of them have no border mark in any triangle.
void count_unmarked(std::vector<uint8_t>& t, int* ground, int* unmarked) {
    const TileView v = view_of(t);
    for (int i = 0; i < v.h.polyCount && i < v.h.detailMeshCount; ++i) {
        if (v.polys[i].getType() != DT_POLYTYPE_GROUND) continue;
        ++*ground;
        bool any = false;
        for (int k = 0; k < v.dmeshes[i].triCount; ++k) any = any || (v.dtris[(v.dmeshes[i].triBase + k) * 4 + 3] & kAnyBorder) != 0;
        if (!any) ++*unmarked;
    }
}

/// Takes every border mark off every ground polygon of a tile: what the float test did to 27 polygons of the Riverlands.  Returns the marks taken off.
int clear_marks(std::vector<uint8_t>& t) {
    const TileView v = view_of(t);
    int cleared = 0;
    for (int i = 0; i < v.h.polyCount && i < v.h.detailMeshCount; ++i) {
        if (v.polys[i].getType() != DT_POLYTYPE_GROUND) continue;
        for (int k = 0; k < v.dmeshes[i].triCount; ++k) {
            unsigned char& f = v.dtris[(v.dmeshes[i].triBase + k) * 4 + 3];
            for (int e = 0; e < 3; ++e) if (f & (1 << (e * 2))) ++cleared;
            f = static_cast<unsigned char>(f & ~kAnyBorder);
        }
    }
    return cleared;
}

size_t tile_with_most_polys(const nav::NavFile& f) {
    size_t best = 0;
    int most = -1;
    for (size_t i = 0; i < f.tiles.size(); ++i) {
        dtMeshHeader h;
        std::memcpy(&h, f.tiles[i].data.data(), sizeof h);
        if (h.polyCount > most) { most = h.polyCount; best = i; }
    }
    return best;
}

/// A grid of questions over the test world (the wall's and the pen's footprints included: points next to polygons, over none): nothing may take the process down.
void ask_the_grid(nav::AreaNav& n, int* asked, int* answered) {
    for (double x = -4000; x <= 11000; x += 250)
        for (double y = -9000; y <= 9000; y += 250)
            for (double z : {1000.0, 1300.0}) {
                nav::Vec3 np;
                double h = 0;
                ++*asked;
                if (n.nearest_point({x, y, z}, 300, 500, &np)) ++*answered;
                n.ground_height(x, y, z, &h);
                n.on_navmesh({x, y, z});
            }
}

/// Opt-in (AOC_NAV_SWEEP=1; dist/content/area_nav.bin or AOC_AREA_NAV): every tile of the real file, read and repaired on a copy: how many polygons had no mark, how many edges the repair
/// marks (none, in a file made by a builder that marks them), and that no polygon has to be hidden.  About twenty seconds for the Riverlands' 31,000 tiles.
void sweep_real_file() {
    const char* env = std::getenv("AOC_AREA_NAV");
    const std::string path = env && *env ? env : "dist/content/area_nav.bin";
    nav::NavFile idx;
    std::string err;
    if (!nav::read_nav_index(path, &idx, &err)) { std::printf("  [skip] sweep of %s: %s\n", path.c_str(), err.c_str()); return; }
    std::ifstream in(path, std::ios::binary);
    nav::TileRepair total;
    size_t tiles = 0;
    int ground = 0, unmarked = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (const nav::NavChunkEntry& e : idx.chunks) {
        std::vector<nav::NavTileBlob> blobs;
        if (!nav::read_nav_chunk(in, e, &blobs, &err, idx.chunk_tiles)) { std::printf("  [FAIL] chunk (%d, %d): %s\n", e.cx, e.cy, err.c_str()); ++g_fail; return; }
        for (nav::NavTileBlob& b : blobs) {
            count_unmarked(b.data, &ground, &unmarked);
            total += nav::repair_detail_boundaries(b.data.data(), b.data.size());
            ++tiles;
        }
    }
    std::printf("    sweep: %zu tiles, %d ground polygons, %d without a marked border edge; the repair marked %llu edges (%llu polygons had none), hid %llu; %.1f s\n", tiles, ground, unmarked,
                static_cast<unsigned long long>(total.edges_flagged), static_cast<unsigned long long>(total.polys_repaired), static_cast<unsigned long long>(total.polys_hidden),
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    CHECK(tiles == static_cast<size_t>(idx.tile_count_v2) && total.polys_hidden == 0 && total.polys_repaired == static_cast<uint64_t>(unmarked), "the whole real file: no polygon is hidden, and every polygon without a mark was given one");
}

}  // namespace

static void test_border_marks() {
    std::printf("detail edges on polygon borders\n");
    nav::BuildOptions opt;
    opt.threads = 2;
    opt.nav_margin_cm = 3000;
    std::string err;

    // the builder leaves every ground polygon marked, near the origin and 300,000 cm out
    nav::NavFile near_file, far_file;
    nav::BuildStats near_stats, far_stats;
    CHECK(nav::build_nav(navfix::geometry(), 7, opt, &near_file, &near_stats, &err), "the test world builds");
    CHECK(nav::build_nav(navfix::shifted_geometry(300000.0), 7, opt, &far_file, &far_stats, &err), "the same world 300,000 cm out builds");
    for (nav::NavFile* f : {&near_file, &far_file}) {
        int ground = 0, unmarked = 0;
        nav::TileRepair again;
        for (auto& t : f->tiles) {
            count_unmarked(t.data, &ground, &unmarked);
            std::vector<uint8_t> copy = t.data;
            again += nav::repair_detail_boundaries(copy.data(), copy.size());
            if (copy != t.data) again.edges_flagged += 1;   // (it must not change a finished tile)
        }
        std::printf("    %s: %d ground polygons, %d without a mark\n", f == &near_file ? "near" : "far ", ground, unmarked);
        CHECK(ground > 100 && unmarked == 0, f == &near_file ? "near the origin: every ground polygon has a marked border edge" : "300,000 cm out: every ground polygon has a marked border edge");
        CHECK(again.edges_flagged == 0 && again.polys_repaired == 0 && again.polys_hidden == 0, "a built tile needs nothing more, and a second pass changes no byte");
    }
    std::printf("    the builder marked %llu edges (%llu polygons had none) near the origin, %llu edges (%llu polygons had none) 300,000 cm out\n",
                static_cast<unsigned long long>(near_stats.repair.edges_flagged), static_cast<unsigned long long>(near_stats.repair.polys_repaired),
                static_cast<unsigned long long>(far_stats.repair.edges_flagged), static_cast<unsigned long long>(far_stats.repair.polys_repaired));
    CHECK(far_stats.repair.polys_hidden == 0 && near_stats.repair.polys_hidden == 0, "no polygon of either world is hidden");

    // a file damaged the way Recast's test damaged 27 polygons of the Riverlands: one tile without a single mark
    nav::NavFile damaged = near_file;
    const size_t big = tile_with_most_polys(damaged);
    int ground = 0, unmarked = 0;
    const int cleared = clear_marks(damaged.tiles[big].data);
    count_unmarked(damaged.tiles[big].data, &ground, &unmarked);
    std::printf("    the busiest tile has %d ground polygons; the damage took off %d marks\n", ground, cleared);
    CHECK(cleared > 0 && ground >= 3 && unmarked == ground, "the damage takes every mark off every ground polygon of the busiest tile");
    // (the repair, tile by tile: every mark that was there comes back, and no polygon is hidden)
    {
        std::vector<uint8_t> fixed = damaged.tiles[big].data;
        const nav::TileRepair r = nav::repair_detail_boundaries(fixed.data(), fixed.size());
        int g2 = 0, u2 = 0;
        count_unmarked(fixed, &g2, &u2);
        CHECK(r.polys_repaired == static_cast<uint64_t>(ground) && r.polys_hidden == 0 && u2 == 0, "the repair gives every one of them a marked edge, and hides none");
        CHECK(r.edges_flagged >= static_cast<uint64_t>(cleared), "and marks at least the edges that were marked before");
        std::vector<uint8_t> orig = near_file.tiles[big].data;
        const TileView vo = view_of(orig), vf = view_of(fixed);
        bool superset = true;
        for (int k = 0; k < vo.h.detailTriCount; ++k) superset = superset && (vo.dtris[k * 4 + 3] & ~vf.dtris[k * 4 + 3] & kAnyBorder) == 0;
        CHECK(superset, "no mark of the built tile is missing from the repaired one");
    }
    // the same file loaded: the loader repairs it, and the questions next to the polygons' borders are answered
    nav::NavFile damaged_copy = damaged;
    auto n = nav::AreaNav::from_file(std::move(damaged_copy), &err);
    CHECK(n != nullptr, "the damaged file loads");
    if (n) {
        CHECK(n->window_stats().tile_repair.polys_repaired == static_cast<uint64_t>(ground), "the loader repaired the polygons that had no mark");
        int asked = 0, answered = 0;
        ask_the_grid(*n, &asked, &answered);
        CHECK(asked > 5000 && answered > asked / 2, "thousands of questions around the damaged tile (points over no polygon among them) are answered, and the process is still here");
        nav::Vec3 np;
        CHECK(n->nearest_point({-3050, 0, 1300}, 300, 500, &np) && (std::fabs(np.z - 1000) < 60 || std::fabs(np.z - 1600) < 60), "a point beside the wall: its nearest navmesh point is the ground or the wall's top");
    }

    // a polygon with no detail triangle cannot give a closest point at all: it is hidden from the queries
    nav::NavFile holed = near_file;
    {
        const TileView v = view_of(holed.tiles[big].data);
        int hole = -1;
        for (int i = 0; i < v.h.polyCount && i < v.h.detailMeshCount && hole < 0; ++i)
            if (v.polys[i].getType() == DT_POLYTYPE_GROUND && v.dmeshes[i].triCount > 0) hole = i;
        if (hole >= 0) v.dmeshes[hole].triCount = 0;
        auto m = nav::AreaNav::from_file(std::move(holed), &err);
        CHECK(hole >= 0 && m != nullptr && m->window_stats().tile_repair.polys_hidden == 1, "a polygon without a detail triangle is hidden, once");
        if (m) {
            int asked = 0, answered = 0;
            ask_the_grid(*m, &asked, &answered);
            CHECK(answered > asked / 2, "and the questions around it are answered");
        }
    }

    // buffers that are not tiles are left alone
    std::vector<uint8_t> junk(300, 0xAB), cut = near_file.tiles[big].data;
    cut.resize(cut.size() - 7);
    const std::vector<uint8_t> junk0 = junk, cut0 = cut;
    const nav::TileRepair a = nav::repair_detail_boundaries(junk.data(), junk.size());
    const nav::TileRepair b = nav::repair_detail_boundaries(cut.data(), cut.size());
    const nav::TileRepair c = nav::repair_detail_boundaries(nullptr, 0);
    CHECK(a.edges_flagged + a.polys_repaired + a.polys_hidden + b.edges_flagged + b.polys_repaired + b.polys_hidden + c.edges_flagged + c.polys_repaired + c.polys_hidden == 0 && junk == junk0 && cut == cut0,
          "a buffer of junk, a tile cut short and a null pointer are left alone");

    if (const char* sweep = std::getenv("AOC_NAV_SWEEP")) if (*sweep && *sweep != '0') sweep_real_file();
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto t0 = std::chrono::steady_clock::now();
    test_build();
    test_ground();
    test_floor_stack();
    test_paths();
    test_border_marks();
    test_real_area();
    std::printf("test_worldd_nav: %d passed, %d failed (%.1f s)\n", g_pass, g_fail, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return g_fail == 0 ? 0 : 1;
}
