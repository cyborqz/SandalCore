// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_region.cpp  (G1, stage 1, Part B)
//
//  The Riverlands as worldd's area, on the real content.db (the working tree's dist/content/content.db, or AOC_CONTENT_DB) and npc_motion.tsv (dist/content, or AOC_NPC_MOTION):
//
//    the index        spawns/region_index.h answers the footprint (401 rectangles, 1,873,943 cells of 512 cm) and the zone of influence of a point (12 zones, each with its own
//                     cell count) exactly as region_data.h holds them; the login spot is in Ruins of Aela; a rectangle's edge is half open; a hostile number is outside
//    the golden       the region's population through the real spawn filter equals the counts of an independent Python port (tools/capture/pop_oracle.py, gen_pop_golden.py:
//                     tests/pop_golden_region.h): the stage counts, the points dropped by reason, the NPCs by Blueprint and by zone, and a digest of the whole list.  With the
//                     zones off the counts are the research's pop.py (E1's rule applied to the whole region); with them on, the 11 filters that name Aela, Winstead or NewAela
//                     pass in their zone.  E1's circle is untouched (test_worldd_area, test_worldd_spawn_filter)
//    the region       load_area on the Riverlands: what it makes (data NPCs, the nodes retail's captures saw, the GM vendors), that every actor is inside the footprint and in the
//                     zone it was selected in, that two loads are the same world; the time and the memory it costs
//    the garden       the GM garden (spawns/gm_garden.h): real GatherableActorDefs, one for each profession and tier the data has an actor node for, 30 to 60 m from the login spot,
//                     clear of every other actor, on the ground a ground function gives, the same garden every time, and nothing when the donor is missing
// ============================================================================
#include "content/content_db.h"
#include "npc/motion_table.h"
#include "proc_stats.h"
#include "spawns/area_loader.h"
#include "spawns/captured_spawns.h"
#include "spawns/generic_npc.h"
#include "spawns/gm_garden.h"
#include "spawns/population.h"
#include "spawns/region_index.h"
#include "spawns/spawn_filter.h"
#include "vendor/gm_vendor.h"
#include "world/interaction_state.h"
#include "pop_golden_region.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>
// (C_Y_B : CoA)

using namespace aoc::worldd;
namespace repl = aoc::protocol::repl;
using aoc::protocol::emit::FIntrepidNetworkGUID;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

static uint64_t g_next = 4608;
static FIntrepidNetworkGUID mint() {
    FIntrepidNetworkGUID g;
    g.ObjectId = g_next;
    g_next += 2;
    g.ServerId = 60;
    g.Randomizer = 0x6ee87ee4u;
    return g;
}

static std::string ms_since(std::chrono::steady_clock::time_point t) {
    return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t).count()) + " ms";
}

// ---- the index ------------------------------------------------------------------------------------------------------------------

static void test_index() {
    std::printf("the Riverlands' index\n");
    namespace R = spawns::region;
    const R::Index& idx = R::Index::get();
    CHECK(idx.rect_count() == 401 && idx.zoi_count() == 12, "401 rectangles and 12 zones of influence");
    // every cell of the bounding box: inside the footprint exactly as often as the data says, each inside cell in exactly one zone, and the zones' cell counts
    std::vector<uint64_t> per_zone(R::kZoiCount, 0);
    uint64_t inside = 0, zoned_outside = 0, unzoned_inside = 0;
    for (int32_t y = idx.min_y(); y < idx.max_y(); y += R::kCellCm) {
        for (int32_t x = idx.min_x(); x < idx.max_x(); x += R::kCellCm) {
            const double cx = x + 100.0, cy = y + 100.0;   // (a point inside the cell, off its corner)
            const bool in = idx.contains(cx, cy);
            const int row = idx.zoi_row(cx, cy);
            if (in) {
                ++inside;
                if (row < 0) ++unzoned_inside;
                else ++per_zone[static_cast<size_t>(row)];
            } else if (row >= 0) {
                ++zoned_outside;
            }
        }
    }
    CHECK(inside == R::kRiverlandsAreaCells, "the footprint holds 1,873,943 cells of 512 cm (49.12 km2)");
    CHECK(unzoned_inside == 0 && zoned_outside == 0, "every cell of the footprint is in a zone, and no zone has a cell outside it");
    bool counts = true;
    for (size_t z = 0; z < R::kZoiCount; ++z) counts = counts && per_zone[z] == R::kZois[z].cells;
    CHECK(counts, "each zone holds the number of cells its record says");
    // the half-open edge
    const R::Rect& r0 = R::kRiverlandsRects[0];
    CHECK(idx.contains(r0.x0, r0.y0) && !idx.contains(r0.x0 - 1.0, r0.y0 - 1.0), "a rectangle's lower corner is inside, the point just beyond it is not");
    // the corners of the footprint's bounding box are outside; hostile numbers are
    CHECK(!idx.contains(idx.min_x() - 1, idx.min_y() - 1) && !idx.contains(idx.max_x(), idx.max_y()), "outside the bounding box is outside");
    const double inf = std::numeric_limits<double>::infinity(), nan = std::numeric_limits<double>::quiet_NaN();
    CHECK(!idx.contains(nan, 0) && !idx.contains(0, inf) && !idx.contains(-1e300, 1e300) && idx.zoi_guid(nan, nan) == 0, "NaN, infinity and absurd numbers are outside and own no zone");
    // the login spot
    const spawns::AreaConfig region = spawns::AreaConfig::riverlands();
    const uint64_t aela = 6064629127135166465ull;
    CHECK(region.contains(region.center_x, region.center_y) && region.zoi_at(region.center_x, region.center_y) == aela, "the login spot is inside, in Ruins of Aela (the research: all 140 player starts)");
    const char* name = region.zoi_name_at(region.center_x, region.center_y);
    CHECK(name && std::string(name) == "Verra_RVR_StartingArea_RuinsOfAela", "...and the zone's name is its record's");
    spawns::AreaConfig no_zones = region;
    no_zones.zones = false;
    CHECK(no_zones.contains(region.center_x, region.center_y) && no_zones.zoi_at(region.center_x, region.center_y) == 0 && !no_zones.zoi_name_at(region.center_x, region.center_y),
          "with the zones off a point has none");
    const spawns::AreaConfig circle;
    CHECK(!circle.region && circle.zoi_at(circle.center_x, circle.center_y) == 0 && circle.contains(circle.center_x, circle.center_y) && !circle.contains(circle.center_x + 100001, circle.center_y),
          "E1's circle is still a 1 km disc with no zones");
    // a small hand-made index: edges and the zone lookup
    const R::Rect a[] = {{0, 0, 1024, 512}, {0, 512, 512, 1024}};
    const R::ZoiRows zo[] = {{11, "one", 0, 1, 2}, {22, "two", 1, 1, 1}};
    const R::Rect za[] = {{0, 0, 1024, 512}, {0, 512, 512, 1024}};
    const R::Index small(a, 2, zo, 2, za, 2);
    CHECK(small.contains(0, 0) && small.contains(1023.9, 511.9) && !small.contains(1024, 0) && !small.contains(512, 512) && small.contains(511.9, 1023.9) && !small.contains(-0.1, 0),
          "a small index: half-open edges, and an L-shaped footprint");
    CHECK(small.zoi_guid(10, 10) == 11 && small.zoi_guid(10, 600) == 22 && small.zoi_guid(900, 600) == 0 && std::string(small.zoi_name(10, 600)) == "two", "...and its zones");
}

// ---- the golden -----------------------------------------------------------------------------------------------------------------

static const char* why_id(spawns::Why w) {
    using spawns::Why;
    switch (w) {
        case Why::Spawns: return "Spawns";
        case Why::DevOnly: return "DevOnly";
        case Why::NotWorldMap: return "NotWorldMap";
        case Why::Blacklisted: return "Blacklisted";
        case Why::Gated: return "Gated";
        case Why::NodeLevel: return "NodeLevel";
        case Why::Culture: return "Culture";
        case Why::NodeOrZone: return "NodeOrZone";
        case Why::NodeType: return "NodeType";
        case Why::Biome: return "Biome";
        case Why::Season: return "Season";
        case Why::Weather: return "Weather";
        case Why::TimeOfDay: return "TimeOfDay";
        case Why::RequiredTag: return "RequiredTag";
        case Why::Prop: return "Prop";
        case Why::Hidden: return "Hidden";
        case Why::Placeholder: return "Placeholder";
        case Why::NoMesh: return "NoMesh";
        case Why::NoAnimation: return "NoAnimation";
        case Why::Appearance: return "Appearance";
    }
    return "?";
}

static uint64_t digest_of(const std::vector<spawns::DataSpawn>& list) {
    uint64_t total = 0;
    for (const spawns::DataSpawn& s : list) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%lld,%lld,%d,%s", static_cast<long long>(std::nearbyint(s.x)), static_cast<long long>(std::nearbyint(s.y)), s.level,
                      spawns::archetype_leaf(s.asset->archetype_path).c_str());
        uint64_t h = 0xCBF29CE484222325ull;
        for (const char* p = buf; *p; ++p) h = (h ^ static_cast<unsigned char>(*p)) * 0x100000001B3ull;
        total += h;
    }
    return total;
}
// == Cyb | C-O-A ==

static const char* zone_name_of(uint64_t guid) {
    for (size_t z = 0; z < spawns::region::kZoiCount; ++z)
        if (spawns::region::kZois[z].guid == guid) return spawns::region::kZois[z].name;
    return "";
}

static void compare_golden(const char* label, const tests::PopGolden& g, const std::vector<spawns::DataSpawn>& list, const spawns::PopulationStats& st) {
    std::printf("the golden counts, %s\n", label);
    const std::map<std::string, uint64_t> mine = {{"placements", st.placements}, {"points_in_area", st.points_in_area}, {"area_points", st.area_points}, {"area_sets", st.area_sets},
                                                  {"no_asset", st.no_asset}, {"no_template", st.no_template}, {"spawned", st.spawned}, {"area_spawned", st.area_spawned}};
    std::string diff;
    for (size_t i = 0; i < g.stage_count; ++i) {
        const auto it = mine.find(g.stage[i].name);
        if (it == mine.end() || it->second != g.stage[i].value) diff += std::string(" ") + g.stage[i].name + "=" + (it == mine.end() ? "?" : std::to_string(it->second)) + " (golden " + std::to_string(g.stage[i].value) + ")";
    }
    CHECK(diff.empty(), "the stage counts equal the oracle's: placements, points, area points and sets, no asset, no template, spawned");
    if (!diff.empty()) std::printf("    differs:%s\n", diff.c_str());
    diff.clear();
    std::map<std::string, uint64_t> filtered;
    for (const auto& [w, n] : st.filtered) filtered[why_id(w)] += n;
    std::map<std::string, uint64_t> want;
    for (size_t i = 0; i < g.filtered_count; ++i) want[g.filtered[i].name] = g.filtered[i].value;
    for (const auto& [k, v] : want) if (filtered[k] != v) diff += " " + k + "=" + std::to_string(filtered[k]) + " (golden " + std::to_string(v) + ")";
    for (const auto& [k, v] : filtered) if (!want.count(k) && v) diff += " " + k + "=" + std::to_string(v) + " (golden none)";
    CHECK(diff.empty(), "the points dropped, by reason, equal the oracle's");
    if (!diff.empty()) std::printf("    differs:%s\n", diff.c_str());
    diff.clear();
    std::map<std::string, uint64_t> bp;
    for (const spawns::DataSpawn& s : list) ++bp[spawns::archetype_leaf(s.asset->archetype_path)];
    std::map<std::string, uint64_t> bwant;
    for (size_t i = 0; i < g.blueprint_count; ++i) bwant[g.blueprints[i].name] = g.blueprints[i].value;
    size_t shown = 0;
    for (const auto& [k, v] : bwant) if (bp[k] != v && shown++ < 8) diff += " " + k + "=" + std::to_string(bp[k]) + " (golden " + std::to_string(v) + ")";
    for (const auto& [k, v] : bp) if (!bwant.count(k) && shown++ < 8) diff += " " + k + "=" + std::to_string(v) + " (golden none)";
    CHECK(diff.empty() && bp.size() == bwant.size(), "the NPCs by Blueprint equal the oracle's");
    if (!diff.empty()) std::printf("    differs:%s\n", diff.c_str());
    if (g.zone_count) {
        std::map<std::string, uint64_t> zones;
        for (const spawns::DataSpawn& s : list) if (s.zoi) ++zones[zone_name_of(s.zoi)];
        bool same = true;
        for (size_t i = 0; i < g.zone_count; ++i) same = same && zones[g.zones[i].name] == g.zones[i].value;
        CHECK(same, "the NPCs by zone of influence equal the oracle's");
    }
    const uint64_t d = digest_of(list);
    CHECK(d == g.digest, "the digest of the whole list (every NPC's place in cm, level and Blueprint) equals the oracle's");
    if (d != g.digest) std::printf("    digest %llu, golden %llu\n", static_cast<unsigned long long>(d), static_cast<unsigned long long>(g.digest));
}

struct World {
    content::PopulationData              data;
    npc::MotionTable                     table;
    std::optional<spawns::GenericNpcs>   generic;
};

static void test_golden(const content::ContentDb& db, const spawns::CapturedData& captured, World& w) {
    std::string error;
    auto data = db.population_data(error);
    CHECK(data.has_value(), "content.db's population chain reads");
    if (!data) return;
    w.data = std::move(*data);
    spawns::SpawnContext ctx;
    ctx.hour = 11.0;
    spawns::SpawnFilter filter(ctx, &w.table);
    const spawns::TemplateSource templates{&captured, &*w.generic};

    spawns::AreaConfig plain = spawns::AreaConfig::riverlands();
    plain.zones = false;
    spawns::PopulationStats st_plain;
    auto t0 = std::chrono::steady_clock::now();
    const auto list_plain = spawns::select_data_spawns(w.data, templates, plain, st_plain, &filter);
    std::printf("    selected in %s: %zu NPCs\n", ms_since(t0).c_str(), list_plain.size());
    compare_golden("zones off (the research's pop.py: E1's rule over the whole footprint)", tests::kGoldenPlain, list_plain, st_plain);
    CHECK(st_plain.spawned == 10554 && st_plain.points_in_area + st_plain.area_points == 22122,
          "10,554 NPCs from 22,122 points (the research's ~10,850 NPCs and 22,421 points were the 55 km2 upper bound of the chunks)");

    const spawns::AreaConfig zoned = spawns::AreaConfig::riverlands();
    spawns::PopulationStats st_zoi;
    t0 = std::chrono::steady_clock::now();
    const auto list_zoi = spawns::select_data_spawns(w.data, templates, zoned, st_zoi, &filter);
    std::printf("    selected in %s: %zu NPCs\n", ms_since(t0).c_str(), list_zoi.size());
    compare_golden("zones on (worldd's default)", tests::kGoldenZoi, list_zoi, st_zoi);
    std::printf("    zones off: %zu points dropped, %zu without a template; zones on: %zu dropped, %zu without a template\n", st_plain.filtered_total(), st_plain.no_template, st_zoi.filtered_total(), st_zoi.no_template);
    CHECK(st_zoi.filtered_total() < st_plain.filtered_total() && st_zoi.filtered_total() + st_zoi.no_template == st_plain.filtered_total() + st_plain.no_template,
          "the zone rule lets the zone-named points through the filter (fewer dropped); the points it frees are Blueprints with no NPC to make, so they move to 'no template' and the NPCs are the same");
    bool in_zone = true;
    for (const spawns::DataSpawn& s : list_zoi) in_zone = in_zone && s.zoi != 0 && s.zoi == zoned.zoi_at(s.x, s.y) && zoned.contains(s.x, s.y);
    CHECK(in_zone, "every NPC is inside the footprint and carries the zone it stands in");
    size_t named = 0;
    for (const auto& [guid, a] : w.data.assets) named += !a.filter.zones.empty();
    std::printf("    %zu assets name a zone of influence in their own filter\n", named);

    // E1's circle: the same selection as before (no zones: a filter that names one never passes)
    spawns::AreaConfig circle;
    spawns::PopulationStats st_circle;
    const auto list_circle = spawns::select_data_spawns(w.data, templates, circle, st_circle, &filter);
    ctx.hour = 11.0;
    CHECK(list_circle.size() == 1803 && st_circle.points_in_area == 3781 && st_circle.area_points == 817, "E1's 1 km circle still selects its 1,803 NPCs from 3,781 + 817 points (pop.py, hour 11)");
}

// ---- the region -----------------------------------------------------------------------------------------------------------------

static void test_region(const content::ContentDb& db, const spawns::CapturedData& captured, World& w, world::ActorRegistry& registry, spawns::AreaReport& out_report) {
    std::printf("load_area: the Riverlands\n");
    std::string error;
    auto gm = vendor::GmVendors::load(db, error);
    CHECK(gm.has_value(), "the GM vendors load");
    if (!gm) return;
    spawns::SpawnContext ctx;
    ctx.hour = 11.0;
    spawns::SpawnFilter filter(ctx, &w.table);
    spawns::AreaSources sources;
    sources.db = &db;
    sources.captured = &captured;
    sources.gm = &*gm;
    sources.generic = &*w.generic;
    sources.filter = &filter;
    const spawns::AreaConfig area = spawns::AreaConfig::riverlands();
    const tests::MemoryNow before = tests::memory_now();
    const auto t0 = std::chrono::steady_clock::now();
    auto report = spawns::load_area(registry, sources, area, mint, 60, 0.0, error);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(report.has_value(), "the Riverlands load");
    if (!report) {
        std::printf("    %s\n", error.c_str());
        return;
    }
    const tests::MemoryNow after = tests::memory_now();
    std::printf("    loaded in %.2f s; %s\n", secs, report->describe().c_str());
    std::printf("    memory: %.0f MB resident before, %.0f MB after (+%.0f MB for %zu actors: %.1f KB an actor)\n", before.working_set_mb, after.working_set_mb, after.working_set_mb - before.working_set_mb,
                registry.size(), (after.working_set_mb - before.working_set_mb) * 1024.0 / static_cast<double>(registry.size()));
    for (const std::string& n : report->notes) std::printf("    note: %s\n", n.c_str());
    CHECK(report->refused == 0, "no spawn was refused");
    CHECK(registry.size() == report->total(), "the registry holds every actor the report counts");
    CHECK(report->data_npcs == tests::kGoldenZoi.stage[6].value && std::string(tests::kGoldenZoi.stage[6].name) == "spawned", "the data NPCs are the oracle's 'spawned' count (zones on)");
    CHECK(report->gatherables > 100, "the nodes retail's captures saw in the Riverlands stand in the registry (more than the 28 of E1's circle)");
    CHECK(report->gm_vendors == gm->vendors().size(), "one GM vendor for each PTR vendor def");
    bool even = true, inside = true, controllers = true;
    std::set<uint64_t> ids;
    size_t nodes_in_circle = 0;
    const spawns::AreaConfig circle;
    registry.for_each([&](const world::WorldActor& a) {
        even = even && (a.id & 1) == 0 && a.id == a.actor.guid.ObjectId && ids.insert(a.id).second;
        if (a.npc.is_gm_vendor) return;
        inside = inside && area.contains(a.x, a.y);
        if (a.kind == world::ActorKind::Npc) controllers = controllers && a.npc.controller != 0;
        if (a.kind == world::ActorKind::Gatherable && circle.contains(a.x, a.y)) ++nodes_in_circle;
    });
    CHECK(even, "every id is even, unique and the actor's own ObjectId");
    CHECK(inside, "every actor but the GM vendors is inside the footprint");
    CHECK(controllers, "every NPC has an AI controller object");
    std::printf("    %zu resource nodes inside E1's circle (E1 had 28)\n", nodes_in_circle);
    // the same world twice
    world::ActorRegistry again;
    g_next = 900000;
    const uint64_t saved = g_next;
    (void)saved;
    // (the first load used ids from 4608: compare positions and kinds, not ids)
    auto second = spawns::load_area(again, sources, area, mint, 60, 0.0, error);
    bool same = second && again.size() == registry.size();
    if (same) {
        std::vector<std::tuple<double, double, int>> a, b;
        registry.for_each([&](const world::WorldActor& x) { a.emplace_back(x.x, x.y, static_cast<int>(x.kind)); });
        again.for_each([&](const world::WorldActor& x) { b.emplace_back(x.x, x.y, static_cast<int>(x.kind)); });
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        same = a == b;
    }
    CHECK(same, "two loads make the same world (every actor's place and kind)");
    out_report = std::move(*report);
}
// [C.O.A + C | Y | B]

// ---- the garden -----------------------------------------------------------------------------------------------------------------

static void test_garden(const content::ContentDb& db, const spawns::CapturedData& captured, world::ActorRegistry& registry) {
    std::printf("the GM garden\n");
    // the defs are real records of content.db
    sqlite3* h = nullptr;
    int found = 0;
    bool types_ok = true, all_ok = false;
    if (sqlite3_open_v2(db.path().c_str(), &h, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(h, "SELECT type_id, schema, dev_only FROM record WHERE guid = ?", -1, &st, nullptr) == SQLITE_OK) {
            all_ok = true;
            for (size_t i = 0; i < spawns::garden::kGardenDefCount; ++i) {
                const spawns::garden::Def& d = spawns::garden::kGardenDefs[i];
                sqlite3_reset(st);
                sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(d.guid));
                if (sqlite3_step(st) == SQLITE_ROW) {
                    ++found;
                    types_ok = types_ok && static_cast<uint64_t>(sqlite3_column_int64(st, 0)) == d.type_id && std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 1))) == "GatherableActorDefRecordBase" &&
                               sqlite3_column_int(st, 2) == 0;
                }
                sqlite3_reset(st);
                sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(d.interaction));
                if (sqlite3_step(st) != SQLITE_ROW || std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 1))) != "PlayerInteractionDefRecordBase") all_ok = false;
            }
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(h);
    CHECK(spawns::garden::kGardenDefCount == 10 && spawns::garden::kGardenGapCount == 5, "10 of the 15 profession and tier slots have an actor node in the data; Master (three professions) and GrandMaster (Mining, Lumberjacking) have none");
    CHECK(found == static_cast<int>(spawns::garden::kGardenDefCount) && types_ok && all_ok, "every garden def is a live GatherableActorDef record, and its interaction a PlayerInteractionDef record");
    std::set<std::string> slots;
    for (size_t i = 0; i < spawns::garden::kGardenDefCount; ++i) slots.insert(std::string(spawns::garden::kGardenDefs[i].profession) + "/" + std::to_string(spawns::garden::kGardenDefs[i].tier));
    for (size_t i = 0; i < spawns::garden::kGardenGapCount; ++i) slots.insert(std::string(spawns::garden::kGardenGaps[i].profession) + "/" + std::to_string(spawns::garden::kGardenGaps[i].tier));
    CHECK(slots.size() == 15, "the defs and the gaps are the 15 slots, each once");

    const spawns::OpenTemplate* donor = spawns::garden_donor(captured);
    CHECK(donor != nullptr && donor->category == "gatherable", "the captures hold a gatherable open to make the garden from");
    if (!donor) return;
    const spawns::AreaConfig area = spawns::AreaConfig::riverlands();
    const size_t before = registry.size();
    // a flat ground 5 m under the login spot's height for every point but a hole east of the spot
    const double ground_z = spawns::kLoginSpotZ - 500.0;
    const spawns::GardenGround ground = [&](double x, double y, double, double* z) {
        if (x > area.center_x + 5500 && std::fabs(y - area.center_y) < 1500) return false;
        *z = ground_z;
        return true;
    };
    g_next = 7000000;
    const spawns::GardenReport r = spawns::place_gm_garden(registry, donor, area, mint, 60, 0.0, ground);
    std::printf("    %s\n", r.describe().c_str());
    CHECK(r.nodes.size() == 10 && r.unplaced == 0 && r.gaps == 5 && registry.size() == before + 10, "ten nodes placed, none left over, five gaps counted");
    bool distance = true, ground_ok = true, clear = true, defs_ok = true, hole = true, kind = true;
    std::set<uint64_t> actor_ids;
    for (const spawns::GardenNode& n : r.nodes) {
        const double d = std::hypot(n.x - area.center_x, n.y - area.center_y);
        distance = distance && d >= spawns::kGardenMinCm - 1 && d <= spawns::kGardenMaxCm + 1;
        ground_ok = ground_ok && n.grounded && std::fabs(n.z - ground_z) < 1e-6;
        hole = hole && !(n.x > area.center_x + 5500 && std::fabs(n.y - area.center_y) < 1500);
        const world::WorldActor* a = registry.find(n.actor_id);
        kind = kind && a && a->kind == world::ActorKind::Gatherable && actor_ids.insert(n.actor_id).second && a->x == n.x && a->y == n.y && a->z == n.z;
        if (a) {
            const repl::Value* guid = a->actor.obj.state.get(*a->actor.obj.schema, "GatherableActorDef.Guid");
            const auto* gv = guid ? guid->get_if<int64_t>() : nullptr;
            defs_ok = defs_ok && gv && static_cast<uint64_t>(*gv) == n.def->guid;
            // the interaction of the def sits in the component's first input
            const std::vector<int64_t> inputs = world::interaction_inputs(a->actor);
            defs_ok = defs_ok && !inputs.empty();
            for (const repl::Subobject& s : a->actor.subobjects) {
                if (!s.obj.schema || std::string(s.obj.schema->name) != "UInteractableComponent") continue;
                const repl::Value* data = s.obj.state.get(*s.obj.schema, "InteractionInputData");
                const auto* arr = data ? data->get_if<repl::ArrayValue>() : nullptr;
                const int64_t* gi = nullptr;
                if (arr && !arr->elements.empty()) {
                    const auto g6 = arr->elements[0].values.find(6);
                    if (g6 != arr->elements[0].values.end()) gi = g6->second.get_if<int64_t>();
                }
                defs_ok = defs_ok && gi && static_cast<uint64_t>(*gi) == n.def->interaction;
            }
        }
        // clear of every other actor
        std::vector<uint64_t> around;
        registry.query(n.x, n.y, spawns::kGardenActorClearCm - 1, around);
        for (const uint64_t id : around) if (id != n.actor_id && !(actor_ids.count(id))) clear = false;
        for (const spawns::GardenNode& o : r.nodes) if (&o != &n && std::hypot(o.x - n.x, o.y - n.y) < spawns::kGardenNodeClearCm - 1) clear = false;
    }
    CHECK(distance, "every node stands 30 to 60 m from the login spot");
    CHECK(ground_ok && hole, "every node stands on the ground the ground function gave, and none where it gave none");
    CHECK(clear, "no node is within 6 m of another actor or 8 m of another node");
    CHECK(defs_ok, "each node carries its def's Guid, and its def's interaction in the component's first input");
    CHECK(kind, "each is a gatherable actor of the registry at the place the report says");
    // sectors: the three professions apart, the tiers outwards
    bool tiers_out = true;
    std::map<std::string, double> last_radius;
    for (const spawns::GardenNode& n : r.nodes) {
        const double d = std::hypot(n.x - area.center_x, n.y - area.center_y);
        const std::string p = n.def->profession;
        if (last_radius.count(p)) tiers_out = tiers_out && d + 801 >= last_radius[p];   // (a candidate nudged for room may sit a ring in)
        last_radius[p] = d;
    }
    CHECK(tiers_out, "along a profession's sector the tiers run outwards");

    // the same garden every time
    world::ActorRegistry copy;
    g_next = 8000000;
    const spawns::GardenReport r2 = spawns::place_gm_garden(copy, donor, area, mint, 60, 0.0, ground);
    bool same = r2.nodes.size() == r.nodes.size();
    for (size_t i = 0; same && i < r.nodes.size(); ++i) same = r.nodes[i].x == r2.nodes[i].x && r.nodes[i].y == r2.nodes[i].y && r.nodes[i].def == r2.nodes[i].def;
    // (that copy is an empty world; the first was crowded: only a node that did not need to move must agree)
    size_t agree = 0;
    for (size_t i = 0; i < r.nodes.size() && i < r2.nodes.size(); ++i) agree += r.nodes[i].x == r2.nodes[i].x && r.nodes[i].y == r2.nodes[i].y;
    CHECK(r2.nodes.size() == 10 && agree >= 8, "an empty world gets the same garden but for the nodes that had to move for the NPCs and vendors of the crowded one");
    (void)same;
    // two empty worlds are the same
    world::ActorRegistry copy2;
    g_next = 8000000;
    const spawns::GardenReport r3 = spawns::place_gm_garden(copy2, donor, area, mint, 60, 0.0, ground);
    bool identical = r3.nodes.size() == r2.nodes.size();
    for (size_t i = 0; identical && i < r2.nodes.size(); ++i) identical = r2.nodes[i].x == r3.nodes[i].x && r2.nodes[i].y == r3.nodes[i].y && r2.nodes[i].yaw == r3.nodes[i].yaw && r2.nodes[i].actor_id == r3.nodes[i].actor_id;
    CHECK(identical, "the same world, the same donor and the same ids give the same garden, place and facing");
// built with Cyb and C O A

    // no ground: the login spot's height, and the nodes say so; no donor: no garden
    world::ActorRegistry flat;
    const spawns::GardenReport r4 = spawns::place_gm_garden(flat, donor, area, mint, 60, 0.0, spawns::GardenGround{});
    bool ungrounded = r4.nodes.size() == 10;
    for (const spawns::GardenNode& n : r4.nodes) ungrounded = ungrounded && !n.grounded && n.z == spawns::kLoginSpotZ;
    CHECK(ungrounded, "without a ground the nodes stand at the login spot's height and say they are not grounded");
    world::ActorRegistry none;
    const spawns::GardenReport r5 = spawns::place_gm_garden(none, nullptr, area, mint, 60, 0.0, ground);
    CHECK(r5.nodes.empty() && none.size() == 0 && r5.unplaced == 10 && !r5.notes.empty(), "without a gatherable open there is no garden, and the report says why");
    // a ground that is nowhere: nothing placed, ten left over
    world::ActorRegistry hole_world;
    const spawns::GardenReport r6 = spawns::place_gm_garden(hole_world, donor, area, mint, 60, 0.0, [](double, double, double, double*) { return false; });
    CHECK(r6.nodes.empty() && r6.unplaced == 10, "where no ground exists nothing is placed");
}

int main() {
    const char* env = std::getenv("AOC_CONTENT_DB");
    const std::string path = env && *env ? env : REGION_CONTENT_DB_PATH;
    std::string error;
    auto db = content::ContentDb::open(path, error);
    if (!db) {
        std::printf("  [FAIL] content.db %s does not open: %s\n", path.c_str(), error.c_str());
        return 2;
    }
    std::printf("content.db %s (contentc version %s)\n", path.c_str(), db->meta().contentc_version.c_str());
    auto captured = spawns::CapturedData::load(error);
    if (!captured) {
        std::printf("  [FAIL] the captured spawns do not load: %s\n", error.c_str());
        return 2;
    }
    std::printf("%zu captured opens, %zu templates, %zu captured spawns\n", captured->opens().size(), captured->template_count(), captured->spawns().size());
    const char* env_motion = std::getenv("AOC_NPC_MOTION");
    const std::string motion_path = env_motion && *env_motion ? env_motion : REGION_MOTION_PATH;
    World w;
    if (!w.table.load(motion_path, error)) {
        std::printf("  [FAIL] %s: the region tests need npc_motion.tsv\n", error.c_str());
        return 2;
    }
    w.generic.emplace(*captured, &w.table, spawns::GenericConfig{});

    test_index();
    test_golden(*db, *captured, w);
    world::ActorRegistry registry;
    spawns::AreaReport report;
    test_region(*db, *captured, w, registry, report);
    if (registry.size() > 0) test_garden(*db, *captured, registry);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
