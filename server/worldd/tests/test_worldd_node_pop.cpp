// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_node_pop.cpp  (Nodes 1, stage 3b: the towns' people and the roads by node level)
//
//  On the REAL content.db (contentc version 12, dist/content/content.db or AOC_CONTENT_DB), npc_motion.tsv and the retail templates (spawns/area_spawns.h):
//
//    the filter      the siege tags a PopulationFilter requires are any-of (the 45 filters that require Declared, Inactive and Prep pass in a world that carries SiegeInactive; a filter
//                    that requires Active does not; every other tag is all-of), a context's own zone stands in for a point that has none (the zone refusal is dropped for a node, kept for the
//                    area's circle), a set gated by the Layout source spawns only when the source runs
//    the frames      a node's layout sets are placed at the node RECORD's location and yaw, a building's sets at the plot's transform in that frame; the 10 retail NPCs capture 721 shows
//                    at Joeva (a town, level 4) - guards, villagers, animals, the warehouse clerk and a gear vendor - stand within 1 m of a point of our selection
//    the golden      for every city node inside the Riverlands at each of the 7 node levels (70 cases) the selection equals the independent Python port's counts (tests/node_pop_golden.h,
//                    tools/capture/node_pop_oracle.py): the sets and points of the layout and the buildings, the NPCs from each, the points lost by reason, the NPCs by Blueprint and the digest of
//                    the whole list
//    the manager     spawns/node_populations.h: at the seed's levels each node has the golden number of NPCs; a level change spawns the NPCs the new level adds, removes the ones it no longer
//                    has and leaves the rest alone (their actors keep their ids); the hooks see every spawn and removal; a respawn comes back from the node's current selection, and a point the
//                    level took away does not come back; the people of a node outside the area are not made
//    the ground      an NPC whose chunk of the ground's window is not resident is added pending (it stands on the terrain and waits), a level change that removes it takes it off the count, and
//                    the chunk's arrival grounds the NPCs of the chunk (the G1 rules; the NPC world's)
//    the bowls       (the test-2 fix) the five city nodes sit in pits: the baked terrain is 50 m under the town's floor.  A node's NPC keeps its data height (ground_by_data; on a fixture whose town
//                    is 100 m over the plain, waiting or grounded, in the registry and in the open the client is sent; retail's z on the 9 anchors); world/town_floor.h (the pure class; PlayerWorld
//                    on the fixture: ground_at, place_height and lift_out_of_bowl; the floor follows a level change); opt-in AOC_REAL_GROUND=1: the five bowls on dist/content/area_nav.bin's terrain
//    the world       PlayerWorld::start_node_people on a circle around Joeva: the number of NPCs, tick_world following a GM level change, respawn_npc of a node reference, the roads following
//                    the nodes' levels
//    the roads       world/roads.h's by-level roads (roadn lines): a road is worth what its level says, apply_levels moves the tiers and widths, the real roads_levels.tsv agrees with the
//                    one-level roads.tsv at Wilderness and a node's roads change when its level does
// ============================================================================
#include "nav_chunk_fixture.h"
#include "node_pop_golden.h"
#include "content/content_db.h"
#include "nav/area_nav.h"
#include "npc/motion_table.h"
#include "npc/npc_world.h"
#include "session/player_world.h"
#include "spawns/captured_spawns.h"
#include "spawns/generic_npc.h"
#include "spawns/node_population.h"
#include "spawns/node_populations.h"
#include "spawns/population.h"
#include "spawns/spawn_filter.h"
#include "world/actor_registry.h"
#include "world/city_nodes.h"
#include "world/node_origins.h"
#include "world/node_wire.h"
#include "world/roads.h"
#include "world/town_floor.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
// [C | O | A :: C Y B]

using namespace aoc::worldd;
namespace bs = aoc::worldd::bootstrap;
namespace fs = std::filesystem;
using aoc::protocol::emit::FIntrepidNetworkGUID;
using spawns::Why;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)
#define CHECKF(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { std::printf("  [FAIL] "); std::printf(__VA_ARGS__); std::printf("\n"); g_fail++; } \
} while (0)

#ifndef NODES_CONTENT_DB_PATH
#define NODES_CONTENT_DB_PATH "dist/content/content.db"
#endif
#ifndef NODES_SEED_PATH
#define NODES_SEED_PATH "deploy/citynode_seed.json"
#endif
#ifndef NODES_MOTION_PATH
#define NODES_MOTION_PATH "dist/content/npc_motion.tsv"
#endif
#ifndef NODES_ROADS_LEVELS_PATH
#define NODES_ROADS_LEVELS_PATH "dist/content/roads_levels.tsv"
#endif

static std::string db_path() {
    const char* env = std::getenv("AOC_CONTENT_DB");
    return env && *env ? env : NODES_CONTENT_DB_PATH;
}
static std::string motion_path() {
    const char* env = std::getenv("AOC_NPC_MOTION");
    return env && *env ? env : NODES_MOTION_PATH;
}

static constexpr int64_t kJoeva = 108646121231993ll, kHalcyon = 108646610851452ll, kWinstead = 108646108518008ll;

// ---- the world the tests share -------------------------------------------------------------------------------------------------------------

struct Env {
    std::optional<content::ContentDb>        db;
    std::shared_ptr<const content::PopulationData> pop;
    std::optional<spawns::CapturedData>      captured;
    npc::MotionTable                         table;
    std::optional<spawns::GenericNpcs>       generic;
    std::unique_ptr<world::CityNodes>        nodes;
    std::unique_ptr<world::NodeWire>         wire;
    std::unique_ptr<spawns::NodePopulationData> data;
    spawns::SpawnContext                     base;
    std::set<std::string>                    shown;
    spawns::TemplateSource                   templates;
};
static Env g_env;

static std::unique_ptr<world::CityNodes> fresh_nodes() {
    std::string error;
    auto nodes = world::CityNodes::load(db_path(), error);
    if (!nodes) {
        std::printf("  [FAIL] city nodes: %s\n", error.c_str());
        std::exit(2);
    }
    size_t applied = 0;
    nodes->apply_seed_file(NODES_SEED_PATH, error, &applied);
    return nodes;
}

static void set_level(world::CityNodes& nodes, int64_t guid, int level) {
    world::LiveNode ln;
    ln.guid = guid;
    ln.level = level;
    nodes.apply_change(ln);
}

static std::vector<spawns::NodeSpawn> select(const world::CityNodes& nodes, const world::NodeWire& wire, const world::CityNodeInfo& info, int level, spawns::NodePopStats& st,
                                              const spawns::AreaConfig& area) {
    const spawns::NodeSite site = spawns::node_site_of(nodes, wire, info, level);
    const spawns::SpawnContext ctx = spawns::node_spawn_context(g_env.base, site, g_env.shown);
    const spawns::SpawnFilter filter(ctx, &g_env.table);
    return spawns::select_node_spawns(*g_env.pop, g_env.templates, *g_env.data, wire, site, area, filter, st);
}

static std::vector<spawns::NodeSpawn> select_real(const world::CityNodeInfo& info, int level, spawns::NodePopStats& st) {
    return select(*g_env.nodes, *g_env.wire, info, level, st, spawns::AreaConfig::riverlands());
}

static const char* why_id(Why w) {
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

static uint64_t digest_of(const std::vector<spawns::NodeSpawn>& list) {
    uint64_t total = 0;
    for (const spawns::NodeSpawn& n : list) {
        const spawns::DataSpawn& s = n.spawn;
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%lld,%lld,%d,%s", static_cast<long long>(std::nearbyint(s.x)), static_cast<long long>(std::nearbyint(s.y)), s.level,
                      spawns::archetype_leaf(s.asset->archetype_path).c_str());
        uint64_t h = 0xCBF29CE484222325ull;
        for (const char* p = buf; *p; ++p) h = (h ^ static_cast<unsigned char>(*p)) * 0x100000001B3ull;
        total += h;
    }
    return total;
}

static std::string join_counts(const std::map<std::string, size_t>& m) {
    std::string out;
    for (const auto& [k, v] : m) {
        if (v == 0) continue;
        out += (out.empty() ? "" : ";") + k + ":" + std::to_string(v);
    }
    return out;
}

static const tests::NodePopCase* golden_of(const std::string& node, int level) {
    for (size_t i = 0; i < tests::kNodePopCaseCount; ++i) {
        if (node == tests::kNodePopCases[i].node && level == tests::kNodePopCases[i].level) return &tests::kNodePopCases[i];
    }
    return nullptr;
}
static size_t golden_npcs(const std::string& node, int level) {
    const tests::NodePopCase* c = golden_of(node, level);
    return c ? c->spawned_layout + c->spawned_building : 0;
}

// ---- the filter ---------------------------------------------------------------------------------------------------------------------------
// (C_Y_B : CoA)

static void test_filter() {
    std::printf("the spawn filter: the siege tags are any-of, the context's zone, the sources\n");
    const content::PopFilter open{};
    const std::set<std::string> none;
    spawns::SpawnContext ctx;
    spawns::SpawnFilter f(ctx, nullptr);
    content::PopFilter pf = open;
    pf.required_tags = {"War.Siege.States.SiegeDeclared", "War.Siege.States.SiegeInactive", "War.Siege.States.SiegePrep"};
    CHECK(f.filter(pf, none) == Why::Spawns, "a filter that requires Declared, Inactive and Prep passes in a world that carries SiegeInactive (any-of; E1's all-of never passed)");
    pf.required_tags = {"War.Siege.States.SiegeActive"};
    CHECK(f.filter(pf, none) == Why::RequiredTag, "a filter that requires Active does not");
    pf.required_tags = {"War.Siege.States.SiegeDeclared", "War.Siege.States.SiegePrep"};
    CHECK(f.filter(pf, none) == Why::RequiredTag, "...nor one that requires only states the world does not carry");
    pf.required_tags = {"War.Siege.States.SiegeInactive", "npc.stature.rareSpawn"};
    CHECK(f.filter(pf, none) == Why::RequiredTag, "every other required tag is still all-of: the rare spawn tag is missing");
    CHECK(f.filter(pf, {"npc.stature.rareSpawn"}) == Why::Spawns, "...and the spawner's own tags count");
    pf.required_tags = {"War.Siege.States.SiegeDeclared", "War.Siege.States.SiegeInactive", "npc.stature.rareSpawn"};
    CHECK(f.filter(pf, {"npc.stature.rareSpawn"}) == Why::Spawns, "siege states any-of and the other tag present");
    spawns::SpawnContext war = ctx;
    war.world_tags = {"War.Siege.States.SiegeActive"};
    pf.required_tags = {"War.Siege.States.SiegeDeclared", "War.Siege.States.SiegeInactive", "War.Siege.States.SiegePrep"};
    CHECK(spawns::SpawnFilter(war, nullptr).filter(pf, none) == Why::RequiredTag, "a world at war (SiegeActive) does not spawn the idle-state filters");
    pf.required_tags = {"War.Siege.States.SiegeActive"};
    CHECK(spawns::SpawnFilter(war, nullptr).filter(pf, none) == Why::Spawns, "...and spawns the ones that require Active");

    // the zone
    content::PopFilter zf = open;
    zf.zones = {42};
    CHECK(f.filter(zf, none, 0) == Why::NodeOrZone, "a filter that names a zone never passes for a point with none (E1's rule, the area's circle)");
    CHECK(f.filter(zf, none, 42) == Why::Spawns && f.filter(zf, none, 43) == Why::NodeOrZone, "...a point in the zone passes, in another zone does not");
    spawns::SpawnContext in42 = ctx;
    in42.zone = 42;
    spawns::SpawnFilter g(in42, nullptr);
    CHECK(g.filter(zf, none, 0) == Why::Spawns, "a context that stands in a zone (a node's) answers for a point with none: the zone refusal is dropped");
    CHECK(g.filter(zf, none, 43) == Why::NodeOrZone, "...but a point's own zone wins");

    // the sources: a set only the Layout source spawns
    const content::NpcSetRow* gated = nullptr;
    for (const auto& [guid, sources] : g_env.pop->gates) {
        if (sources.size() != 1 || sources[0] != spawns::kLayoutGate) continue;
        const auto set = g_env.pop->sets.find(guid);
        if (set == g_env.pop->sets.end() || set->second.blacklisted || set->second.dev_only || set->second.members.empty()) continue;
        const auto inst = g_env.pop->instances.find(set->second.members.front().instance);
        if (inst == g_env.pop->instances.end() || inst->second.blacklisted || inst->second.dev_only) continue;
        gated = &set->second;
        break;
    }
    CHECK(gated != nullptr, "the data has sets that only the Layout source spawns");
    if (gated) {
        const content::NpcInstanceRow& inst = g_env.pop->instances.at(gated->members.front().instance);
        CHECK(f.point(*g_env.pop, nullptr, *gated, inst) == Why::Gated, "...the world does not spawn one (no layout source runs)");
        spawns::SpawnContext layout = ctx;
        layout.active_gates = {spawns::kLayoutGate};
        CHECK(spawns::SpawnFilter(layout, nullptr).point(*g_env.pop, nullptr, *gated, inst) == Why::Spawns, "...a node's context does");
        spawns::SpawnContext building = ctx;
        building.active_gates = {spawns::kBuildingGate};
        CHECK(spawns::SpawnFilter(building, nullptr).point(*g_env.pop, nullptr, *gated, inst) == Why::Gated, "...and the building source does not run a layout's set");
    }
    // the names the filter reads
    CHECK(std::string(spawns::node_level_name(4)) == "Town" && std::string(spawns::node_type_name(1)) == "Scientific" && std::string(spawns::biome_name(14)) == "Riverlands" &&
              std::string(spawns::node_level_name(9)).empty() && std::string(spawns::biome_name(-1)).empty(),
          "ENodeLevel, ENodeType and EBiomeType by number");
    const spawns::NodeSite site = spawns::node_site_of(*g_env.nodes, *g_env.wire, *g_env.nodes->by_guid(kJoeva), 4);
    const spawns::SpawnContext jc = spawns::node_spawn_context(g_env.base, site, {"Node_Villager_F"});
    CHECK(jc.node_level == "Town" && jc.node_id == kJoeva && jc.node_type == "Scientific" && jc.biome == "Riverlands" && jc.zone == site.zoi && jc.zone != 0 &&
              jc.active_gates.count(spawns::kLayoutGate) && jc.active_gates.count(spawns::kBuildingGate) && jc.shown_humanoids.count("Node_Villager_F") && jc.shown_humanoids.count("Guard_Soldier"),
          "Joeva's context: Town, its id, Scientific, the Riverlands, its zone, the two sources, the shown humanoids added to the base's");
}

// ---- the data and the frames --------------------------------------------------------------------------------------------------------------

static void test_data() {
    std::printf("the node tables and the frames\n");
    const double r2 = std::sqrt(0.5);
    CHECK(std::fabs(spawns::quaternion_yaw(0, 0, r2, r2) - 90.0) < 1e-9 && std::fabs(spawns::quaternion_yaw(0, 0, 1, 0) - 180.0) < 1e-9 && std::fabs(spawns::quaternion_yaw(0, 0, -r2, r2) + 90.0) < 1e-9 &&
              std::fabs(spawns::quaternion_yaw(0, 0, 0, 1)) < 1e-12,
          "a quaternion's yaw: 90, 180, -90 and 0 degrees");
    CHECK(g_env.data->layouts() == 44 && g_env.data->buildings() == 59, "the layouts' population sets and the buildings' populations read (44 layouts with sets, 59 buildings with populations)");
    const world::CityNodeInfo* joeva = g_env.nodes->by_guid(kJoeva);
    const int64_t layout = g_env.wire->layout_of(kJoeva);
    CHECK(joeva && layout != 0 && g_env.data->layout_sets(layout).size() == 68, "Joeva's layout (Flat_D) names 68 population sets");
    // the level key of a building: the highest key at or below the node's level
    bool keyed = false;
    for (const world::PlotRow& row : g_env.wire->plot_rows(layout, 4)) {
        const std::vector<int> levels = row.building ? g_env.data->levels_of(row.building) : std::vector<int>();
        if (levels.size() < 2 || levels.front() < 1) continue;
        const int lo = levels.front(), hi = levels.back();
        keyed = g_env.data->spawn_for(row.building, lo - 1) == 0 && g_env.data->spawn_for(row.building, lo) != 0 && g_env.data->spawn_for(row.building, 6) == g_env.data->spawn_for(row.building, hi);
        if (keyed) break;
    }
    CHECK(keyed, "a building runs the BuildingLevelSpawn of the highest level key at or below the node's: nothing below its first key");

    // the sets placed
    const std::vector<spawns::PlacedSet> placed = spawns::placed_sets(*g_env.data, *g_env.wire, spawns::node_site_of(*g_env.nodes, *g_env.wire, *joeva, 4));
    size_t layout_sets = 0, building_sets = 0, expect_building = 0;
    bool layout_at_node = true, building_frame = true, ids = true;
    for (const spawns::PlacedSet& p : placed) {
        if (p.source == spawns::PopSource::Layout) {
            ++layout_sets;
            layout_at_node = layout_at_node && p.x == joeva->x && p.y == joeva->y && p.z == joeva->z && p.yaw == joeva->yaw && p.placement == kJoeva && p.plot == 0;
        } else {
            ++building_sets;
            ids = ids && p.plot != 0 && p.placement != kJoeva;
        }
    }
    for (const world::PlotRow& row : g_env.wire->plot_rows(layout, 4)) {
        if (row.building == 0 || (row.state != 1 && row.state != 2)) continue;
        const int64_t spawn = g_env.data->spawn_for(row.building, 4);
        if (spawn == 0) continue;
        expect_building += g_env.data->spawn_sets(spawn, row.state).size();
    }
    CHECK(layout_sets == 68 && layout_at_node, "Joeva at Town: its 68 layout sets are placed at the node record's location and yaw, placement id the node's guid");
    CHECK(building_sets == expect_building && building_sets > 0 && ids, "...and a set for each population set of each plot's building in its state, with the plot in the placement id");
    // a building set stands at its plot's transform in the node's frame
    const double kPi = 3.14159265358979323846;
    bool checked = false;
    for (const world::PlotRow& row : g_env.wire->plot_rows(layout, 4)) {
        if (row.building == 0 || (row.state != 1 && row.state != 2)) continue;
        const int64_t spawn = g_env.data->spawn_for(row.building, 4);
        if (spawn == 0 || g_env.data->spawn_sets(spawn, row.state).empty()) continue;
        const double yaw = joeva->yaw * kPi / 180.0;
        const double ex = joeva->x + row.tx * std::cos(yaw) - row.ty * std::sin(yaw), ey = joeva->y + row.tx * std::sin(yaw) + row.ty * std::cos(yaw);
        const double eyaw = joeva->yaw + spawns::quaternion_yaw(row.qx, row.qy, row.qz, row.qw);
        for (const spawns::PlacedSet& p : placed) {
            if (p.plot != row.plot) continue;
            building_frame = building_frame && std::fabs(p.x - ex) < 1e-6 && std::fabs(p.y - ey) < 1e-6 && std::fabs(p.z - (joeva->z + row.tz)) < 1e-6 && std::fabs(p.yaw - eyaw) < 1e-9;
            checked = true;
        }
    }
    CHECK(checked && building_frame, "a building's sets are placed at the plot's translation rotated by the node's yaw, at the node's height plus the plot's, with the plot's yaw added to the node's");
}

// ---- retail's NPCs --------------------------------------------------------------------------------------------------------------------------

static void test_anchors() {
    std::printf("retail's NPCs at Joeva (capture 721, a Town)\n");
    struct Anchor { const char* leaf; double x, y, z; bool building; };   // z: the height retail's actor stood at (its replicated origin)
    const Anchor anchors[] = {
        {"Guard_Wizard", -1081432, 516570, 14753, false},
        {"Node_Villager_M", -1097474, 509458, 14024, false},
        {"Guard_Soldier", -1091333, 510020, 13408, false},
        {"Cow_StateTree", -1085254, 503627, 12608, false},
        {"Horse_withCollision", -1089840, 503696, 12625, false},
        {"Pig_0", -1078157, 512855, 13046, false},
        {"Chicken_Base", -1087121, 515470, 14042, false},
        {"Node_Warehouse_StorageClerk_KAE", -1081577, 505744, 12638, true},
        {"General_Gear_Base_Vendor", -1079600, 509538, 13290, true},
    };
    spawns::NodePopStats st;
    const auto list = select_real(*g_env.nodes->by_guid(kJoeva), 4, st);
    CHECK(list.size() == golden_npcs("Verra_RVR_Joeva", 4) && !list.empty(), "Joeva at Town has the oracle's number of NPCs");
    for (const Anchor& a : anchors) {
        double best = 1e18, best_dz = 1e18;
        for (const spawns::NodeSpawn& n : list) {
            if (spawns::archetype_leaf(n.spawn.asset->archetype_path) != a.leaf) continue;
            if ((n.source == spawns::PopSource::Building) != a.building) continue;
            const double d = std::hypot(n.spawn.x - a.x, n.spawn.y - a.y);
            if (d < best) {
                best = d;
                best_dz = n.spawn.z - a.z;
            }
        }
        char msg[240];
        std::snprintf(msg, sizeof(msg), "a %s of our %s sets stands within 1 m of where retail's stood (%.2f m)", a.leaf, a.building ? "building" : "layout", best / 100.0);
        CHECK(best < 100.0, msg);
        // (the test-2 fix) the height is the data's, and the data's is retail's: the town's NPCs must stand there, not on the baked terrain 50 m below
        std::snprintf(msg, sizeof(msg), "...and at retail's height: %.0f cm, ours %+.0f cm (within 2 m)", a.z, best_dz);
        CHECK(std::fabs(best_dz) < 200.0, msg);
    }
}
// ~ CoA & C Y B ~

// ---- the golden -----------------------------------------------------------------------------------------------------------------------------

static void test_golden() {
    std::printf("the golden counts: %zu cases (10 nodes inside the Riverlands, 7 levels)\n", tests::kNodePopCaseCount);
    size_t mismatches = 0, total_npcs = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < tests::kNodePopCaseCount; ++i) {
        const tests::NodePopCase& c = tests::kNodePopCases[i];
        const world::CityNodeInfo* info = g_env.nodes->by_name(c.node);
        if (!info) {
            CHECKF(false, "no node %s", c.node);
            continue;
        }
        spawns::NodePopStats st;
        const auto list = select_real(*info, c.level, st);
        total_npcs += list.size();
        std::map<std::string, size_t> filtered, blueprints;
        for (const auto& [why, n] : st.pop.filtered) filtered[why_id(why)] += n;
        for (const spawns::NodeSpawn& n : list) ++blueprints[spawns::archetype_leaf(n.spawn.asset->archetype_path)];
        std::string diff;
        const auto cmp = [&](const char* name, uint64_t got, uint64_t want) {
            if (got != want) diff += std::string(" ") + name + "=" + std::to_string(got) + " (golden " + std::to_string(want) + ")";
        };
        cmp("sets_layout", st.sets[0], c.sets_layout);
        cmp("sets_building", st.sets[1], c.sets_building);
        cmp("points_layout", st.points[0], c.points_layout);
        cmp("points_building", st.points[1], c.points_building);
        cmp("spawned_layout", st.spawned[0], c.spawned_layout);
        cmp("spawned_building", st.spawned[1], c.spawned_building);
        cmp("no_template", st.pop.no_template, c.no_template);
        cmp("no_asset", st.pop.no_asset, c.no_asset);
        cmp("outside", st.outside, c.outside);
        if (join_counts(filtered) != c.filtered) diff += " filtered=[" + join_counts(filtered) + "] (golden [" + c.filtered + "])";
        if (join_counts(blueprints) != c.blueprints) diff += " blueprints differ";
        const uint64_t d = digest_of(list);
        if (d != c.digest) diff += " digest " + std::to_string(d) + " (golden " + std::to_string(c.digest) + ")";
        CHECKF(diff.empty(), "%s at level %d differs from the oracle:%s", c.node, c.level, diff.c_str());
        if (!diff.empty() && ++mismatches <= 3) std::printf("    %s L%d:%s\n", c.node, c.level, diff.c_str());
    }
    std::printf("    %zu cases selected in %lld ms (%zu NPCs in all)\n", tests::kNodePopCaseCount, static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()),
                total_npcs);
    CHECK(mismatches == 0, "every node at every level has the oracle's sets, points, NPCs, losses, Blueprints and digest");
    // the numbers the user will see
    CHECK(golden_npcs("Verra_RVR_Joeva", 4) > golden_npcs("Verra_RVR_Joeva", 3) && golden_npcs("Verra_RVR_Joeva", 3) > golden_npcs("Verra_RVR_Joeva", 0) &&
              golden_npcs("Verra_RVR_Joeva", 5) < golden_npcs("Verra_RVR_Joeva", 3) && golden_npcs("Verra_RVR_Joeva", 5) == golden_npcs("Verra_RVR_Joeva", 6),
          "(the data) Joeva has more people at Town than at Village, more at Village than at Wilderness, and at City (as at Metropolis) fewer than at Village: the Town-only sets are gone");
    CHECK(golden_of("Verra_RVR_Joeva", 4)->spawned_building > 0 && golden_of("Verra_RVR_ArtisanPOI_AvensEnd", 0)->spawned_building > 0 && golden_of("Verra_RVR_ArtisanPOI_AvensEnd", 0)->spawned_layout == 0,
          "(the data) a town has building NPCs (clerks, vendors) and Aven's End's NPCs, at Wilderness, are all its building's");
}

// ---- the manager --------------------------------------------------------------------------------------------------------------------------

static uint64_t g_next = 8192;
static FIntrepidNetworkGUID mint() {
    FIntrepidNetworkGUID g;
    g.ObjectId = g_next;
    g_next += 2;
    g.ServerId = 60;
    g.Randomizer = 0x6ee87ee4u;
    return g;
}

struct Rig {
    world::ActorRegistry           registry;
    std::vector<uint64_t>          spawned, despawned;
    std::unique_ptr<spawns::NodePopulations> pops;
// -- C | O | A & C Y B --

    Rig(const world::CityNodes& nodes, const world::NodeWire& wire, const spawns::AreaConfig& area, std::function<void(uint64_t, const content::NpcInstanceRow*, const content::NpcRouteRow*)> extra = {},
        std::shared_ptr<const content::PopulationData> population = nullptr, const spawns::NodePopulationData* data = nullptr) {
        spawns::NodePopulations::Hooks hooks;
        hooks.spawned = [this, extra](uint64_t id, const content::NpcInstanceRow* inst, const content::NpcRouteRow* route) {
            spawned.push_back(id);
            if (extra) extra(id, inst, route);
        };
        hooks.despawned = [this](uint64_t id) { despawned.push_back(id); };
        spawns::NodePopulations::Config cfg;
        cfg.shown_humanoids = g_env.shown;
        pops = std::make_unique<spawns::NodePopulations>(registry, nodes, wire, data ? *data : *g_env.data, population ? population : g_env.pop, g_env.templates, g_env.base, &g_env.table, area,
                                                         [] { return mint(); }, 60, std::move(hooks), cfg);
    }
    size_t npcs_of(int64_t node) const { return pops->actors_of(node).size(); }
};

static std::set<uint64_t> as_set(const std::vector<uint64_t>& v) { return {v.begin(), v.end()}; }

static void test_manager() {
    std::printf("the live manager: the people follow the nodes' levels\n");
    auto nodes = fresh_nodes();
    Rig rig(*nodes, *g_env.wire, spawns::AreaConfig::riverlands());
    const size_t made = rig.pops->start(0.0);
    size_t want = 0;
    for (const world::CityNodeInfo& n : nodes->nodes()) {
        if (const tests::NodePopCase* c = golden_of(n.name, nodes->level_of(n.guid))) want += c->spawned_layout + c->spawned_building;
    }
    CHECK(made == want && rig.pops->made() == want && rig.registry.count(world::ActorKind::Npc) == want && rig.spawned.size() == want, "start: every node inside the area has the oracle's number of NPCs at its seed level");
    CHECK(rig.pops->nodes() == 10 && rig.pops->start(0.0) == 0, "10 nodes inside the Riverlands; started once");
    CHECK(rig.npcs_of(kJoeva) == golden_npcs("Verra_RVR_Joeva", 4) && rig.npcs_of(kHalcyon) == golden_npcs("Verra_RVR_Halcyon", 3) && rig.npcs_of(kWinstead) == golden_npcs("Verra_RVR_Winstead", 0),
          "Joeva (Town), Halcyon (Village) and Winstead (Wilderness) have their level's people");
    bool tagged = true, in_range = true, grounding = true;
    size_t by_data = 0, by_terrain = 0;
    rig.registry.for_each([&](const world::WorldActor& a) {
        tagged = tagged && a.npc.spawn_ref >= spawns::kNodeRefBase && a.npc.source == world::Source::Data && a.kind == world::ActorKind::Npc;
        in_range = in_range && a.npc.home_x == a.x && a.npc.home_y == a.y;
        grounding = grounding && a.npc.ground_by_data != a.npc.ground_by_terrain;   // (one or the other)
        by_data += a.npc.ground_by_data ? 1 : 0;
        by_terrain += a.npc.ground_by_terrain ? 1 : 0;
    });
    CHECK(tagged && in_range, "each NPC carries a spawn reference in the node range, comes from the data and has its place as its home");
    // (the test-2 fix) a node's NPC has a height of its own (the layout's, retail's): it keeps it; only a random point of a PopulationArea (no height) is stood on the ground
    char grounding_msg[200];
    std::snprintf(grounding_msg, sizeof(grounding_msg), "each node NPC keeps its data height (ground_by_data: %zu) unless its point is a PopulationArea's random one (ground_by_terrain: %zu)", by_data, by_terrain);
    CHECK(grounding && by_data == want - by_terrain && by_data > want / 2, grounding_msg);
    CHECK(rig.pops->tick(1.0) == 0, "nothing changed: the manager does nothing");

    // Joeva Town -> Village
    const std::vector<uint64_t> before = rig.pops->actors_of(kJoeva);
    const std::vector<uint64_t> halcyon_before = rig.pops->actors_of(kHalcyon);
    std::map<uint64_t, uint32_t> ref_of;
    for (const uint64_t id : before) ref_of[id] = rig.registry.find(id)->npc.spawn_ref;
    const size_t spawned_before = rig.spawned.size(), despawned_before = rig.despawned.size();
    set_level(*nodes, kJoeva, 3);
    CHECK(rig.pops->entry_of(kJoeva)->level == 4, "(a level change is not followed until the world ticks)");
    CHECK(rig.pops->tick(2.0) == 1, "Joeva Town -> Village: one node rebuilt");
    const std::vector<uint64_t> after = rig.pops->actors_of(kJoeva);
    const std::set<uint64_t> b = as_set(before), a = as_set(after);
    size_t kept = 0, gone = 0, arrived = 0;
    for (const uint64_t id : b) (a.count(id) ? kept : gone)++;
    for (const uint64_t id : a) arrived += b.count(id) ? 0 : 1;
    CHECK(rig.pops->entry_of(kJoeva)->level == 3 && after.size() == golden_npcs("Verra_RVR_Joeva", 3), "Joeva has the oracle's people for Village");
    CHECK(kept > 0 && gone > 0 && arrived > 0 && kept + gone == before.size() && kept + arrived == after.size(), "some NPCs stay, some leave and some arrive (a Town's sets go, a Village's building sets come)");
    CHECK(rig.spawned.size() - spawned_before == arrived && rig.despawned.size() - despawned_before == gone, "the hooks saw each arrival and each departure once");
    bool leavers_gone = true, stayers_there = true, hook_ids = true;
    for (const uint64_t id : b) {
        if (!a.count(id)) leavers_gone = leavers_gone && rig.registry.find(id) == nullptr;
        else stayers_there = stayers_there && rig.registry.find(id) != nullptr && rig.registry.find(id)->npc.spawn_ref == ref_of[id];
    }
    for (size_t i = despawned_before; i < rig.despawned.size(); ++i) hook_ids = hook_ids && b.count(rig.despawned[i]) && !a.count(rig.despawned[i]);
    CHECK(leavers_gone && stayers_there && hook_ids, "the NPCs that left are out of the registry; the ones that stay keep their actors and their references");
    CHECK(as_set(rig.pops->actors_of(kHalcyon)) == as_set(halcyon_before), "Halcyon's people were not touched");
    CHECK(rig.registry.count(world::ActorKind::Npc) == want - golden_npcs("Verra_RVR_Joeva", 4) + golden_npcs("Verra_RVR_Joeva", 3), "the registry holds exactly the nodes' people (nothing leaked, nothing doubled)");

    // a respawn: the point that stays comes back, the one the level took away does not
    uint64_t gone_id = 0, stay_id = 0;
    for (const uint64_t id : before) {
        if (!a.count(id) && gone_id == 0) gone_id = id;
        if (a.count(id) && stay_id == 0 && rig.registry.find(id)->npc.respawn_seconds >= 0) stay_id = id;
    }
    CHECK(rig.pops->respawn(ref_of[gone_id], 3.0) == 0, "an NPC the new level has no point for does not come back (its reference is closed)");
    CHECK(rig.pops->respawn(0x80000000u + 4000000u, 3.0) == 0 && rig.pops->respawn(5, 3.0) == 0, "a reference that is no slot, or not a node's, is nothing");
    const world::WorldActor stayer = *rig.registry.find(stay_id);
    rig.registry.remove(stay_id);   // (it died and its corpse went)
    const size_t npcs_without = rig.registry.count(world::ActorKind::Npc);
    const uint64_t back = rig.pops->respawn(ref_of[stay_id], 4.0);
    const world::WorldActor* b2 = rig.registry.find(back);
    CHECK(back != 0 && b2 && rig.registry.count(world::ActorKind::Npc) == npcs_without + 1 && b2->npc.asset_guid == stayer.npc.asset_guid && b2->npc.instance_guid == stayer.npc.instance_guid &&
              b2->npc.spawn_ref == ref_of[stay_id] && b2->x == stayer.x && b2->y == stayer.y && b2->npc.level == stayer.npc.level,
          "the NPC of a point that stays comes back from the node's selection: the same Blueprint, place, level and reference");
    CHECK(rig.pops->respawned() == 1 && rig.pops->entry_of(kJoeva)->npcs == after.size(), "(counted; the node's count is whole again)");

    // Joeva down to Wilderness, then back up
    CHECK(rig.pops->tick(5.0) == 0, "(nothing to follow)");
    set_level(*nodes, kJoeva, 0);
    rig.pops->tick(6.0);
    CHECK(rig.pops->actors_of(kJoeva).size() == golden_npcs("Verra_RVR_Joeva", 0), "Joeva at Wilderness has the people of a Wilderness node");
    set_level(*nodes, kJoeva, 4);
    rig.pops->tick(7.0);
    CHECK(rig.pops->actors_of(kJoeva).size() == golden_npcs("Verra_RVR_Joeva", 4) && rig.pops->entry_of(kJoeva)->level == 4, "...and at Town again, the Town's");
    size_t building_npcs = 0;
    for (const uint64_t id : rig.pops->actors_of(kJoeva)) building_npcs += rig.registry.find(id)->npc.vendor_guid != 0 ? 1 : 0;
    CHECK(building_npcs > 0, "(the Town's people include vendors again)");
    CHECK(rig.pops->rebuilds() == 3, "three rebuilds in all");
    // two nodes change in one tick
    set_level(*nodes, kHalcyon, 4);
    set_level(*nodes, kWinstead, 3);
    CHECK(rig.pops->tick(8.0) == 2 && rig.npcs_of(kHalcyon) == golden_npcs("Verra_RVR_Halcyon", 4) && rig.npcs_of(kWinstead) == golden_npcs("Verra_RVR_Winstead", 3), "two nodes that changed together are both rebuilt");
// ~ C-O-A :: C-Y-B ~

    // a point whose Blueprint another level draws: replaced, and the dead one's reference is closed
    spawns::NodePopStats s3, s4;
    const auto l3 = select_real(*nodes->by_guid(kJoeva), 3, s3);
    const auto l4 = select_real(*nodes->by_guid(kJoeva), 4, s4);
    size_t differing = 0;
    for (const spawns::NodeSpawn& x : l4) {
        for (const spawns::NodeSpawn& y : l3) {
            if (x.ident == y.ident && x.spawn.asset->guid != y.spawn.asset->guid) ++differing;
        }
    }
    std::printf("    (%zu points of Joeva name another Blueprint at Village than at Town)\n", differing);
    CHECK(true, "(counted)");
}

// ---- a point that draws another Blueprint at another level (made by hand: the data has none) -----------------------------------------------

static void test_replacement() {
    std::printf("a point that draws another Blueprint at another level\n");
    const char* kGuard = "/Game/DesignAssets/Population/NPC/PCRaces/Guards/Guard_Soldier.Default__Guard_Soldier_C";
    const char* kVillager = "/Game/DesignAssets/Population/NPC/PCRaces/Node/Filler/Node_Villager_F.Default__Node_Villager_F_C";
    if (!g_env.captured->template_for(kGuard) || !g_env.captured->template_for(kVillager)) {
        CHECK(false, "the retail captures opened a Guard_Soldier and a Node_Villager_F");
        return;
    }
    auto pd = std::make_shared<content::PopulationData>();
    const auto asset = [&](int64_t guid, const char* name, const char* path, const char* lo, const char* hi) {
        content::NpcAssetRow a;
        a.guid = guid;
        a.name = name;
        a.archetype_path = path;
        a.filter.node_level_min = lo;
        a.filter.node_level_max = hi;
        pd->assets[guid] = a;
    };
    asset(9001, "Guard", kGuard, "Wilderness", "Encampment");
    asset(9002, "Villager", kVillager, "Village", "Metropolis");
    asset(9003, "VillageOnly", kVillager, "Village", "Metropolis");
    pd->asset_sets[9100] = {{9001, 1.0}, {9002, 1.0}};   // the post: a guard below Village, a villager from it
    pd->asset_sets[9101] = {{9003, 1.0}};                // a point only a Village has
    const auto instance = [&](int64_t guid, int64_t asset_set) {
        content::NpcInstanceRow i;
        i.guid = guid;
        i.name = "Post";
        i.level_min = i.level_max = 1;
        i.ambient_behavior = "Stationary";
        i.respawn_time = 10;
        i.asset_sets = {asset_set};
        pd->instances[guid] = i;
    };
    instance(9200, 9100);
    instance(9201, 9101);
    content::NpcSetRow set;
    set.guid = 9300;
    set.name = "Post set";
    content::NpcSetMember m1;
    m1.key = 1;
    m1.instance = 9200;
    m1.x = 100;
    m1.y = 200;
    content::NpcSetMember m2 = m1;
    m2.key = 2;
    m2.instance = 9201;
    m2.x = 300;
    set.members = {m1, m2};
    pd->sets[9300] = set;

    std::string error;
    const world::CityNodeInfo* real = g_env.nodes->by_guid(kJoeva);
    std::vector<world::CityZoneInfo> zones{*g_env.nodes->zone_by_guid(real->zoi_guid)};
    world::CityNodes synth({*real}, zones);
    set_level(synth, kJoeva, 2);
    auto wire = world::NodeWire::load(db_path(), synth, error);
    if (!wire) {
        CHECK(false, "the node wire loads for a registry of one node");
        return;
    }
    const auto data = spawns::NodePopulationData::make({{wire->layout_of(kJoeva), {9300}}});
    spawns::AreaConfig everywhere;
    everywhere.center_x = real->x;
    everywhere.center_y = real->y;
    everywhere.radius_cm = 1.0e8;
    Rig rig(synth, *wire, everywhere, {}, pd, data.get());
    CHECK(rig.pops->start(0.0) == 1, "at Encampment the post has a guard and the point only a Village has has nobody");
    std::vector<uint64_t> ids = rig.pops->actors_of(kJoeva);
    const world::WorldActor guard = *rig.registry.find(ids.at(0));
    const uint32_t guard_ref = guard.npc.spawn_ref;
    CHECK(guard.npc.asset_guid == 9001 && guard_ref >= spawns::kNodeRefBase, "...the guard, with a node reference");
    // the guard is killed and its corpse goes; the level rises before its respawn timer runs out
    rig.registry.remove(guard.id);
    set_level(synth, kJoeva, 3);
    CHECK(rig.pops->tick(1.0) == 1, "Encampment -> Village");
    ids = rig.pops->actors_of(kJoeva);
    CHECK(ids.size() == 2, "the post has a villager now and the Village's own point has one too");
    uint64_t villager = 0;
    for (const uint64_t id : ids) {
        if (rig.registry.find(id)->npc.asset_guid == 9002) villager = id;
    }
    const world::WorldActor* v = rig.registry.find(villager);
    CHECK(v && v->npc.spawn_ref != guard_ref && v->npc.spawn_ref >= spawns::kNodeRefBase && v->x == guard.x && v->y == guard.y,
          "the dead guard's point is a villager at the same place (a new reference: the guard's is closed)");
    CHECK(rig.pops->respawn(guard_ref, 20.0) == 0 && rig.pops->actors_of(kJoeva).size() == 2, "the respawn timer of the dead guard runs out: nothing comes back (no guard beside the villager)");
    // the level falls again: the villager is a guard again, the Village's point is gone, the villager's reference is closed
    const uint32_t villager_ref = v->npc.spawn_ref;
    set_level(synth, kJoeva, 2);
    CHECK(rig.pops->tick(2.0) == 1 && rig.pops->actors_of(kJoeva).size() == 1, "Village -> Encampment: one NPC");
    const world::WorldActor* g2 = rig.registry.find(rig.pops->actors_of(kJoeva).at(0));
    CHECK(g2 && g2->npc.asset_guid == 9001 && rig.registry.find(villager) == nullptr, "...a guard again; the villager left");
    CHECK(rig.pops->respawn(villager_ref, 21.0) == 0, "the villager's reference is closed: it does not come back");
    // this guard dies, and its respawn comes back as a guard (the level is still Encampment)
    const uint32_t ref2 = g2->npc.spawn_ref;
    rig.registry.remove(g2->id);
    const uint64_t back = rig.pops->respawn(ref2, 22.0);
    CHECK(back != 0 && rig.registry.find(back)->npc.asset_guid == 9001 && rig.registry.find(back)->npc.spawn_ref == ref2, "a guard that dies at Encampment comes back as a guard, with its reference");
    // it dies again and the level rises: the respawn that was waiting does nothing, the villager is already there
    rig.registry.remove(back);
    set_level(synth, kJoeva, 3);
    rig.pops->tick(3.0);
    CHECK(rig.pops->actors_of(kJoeva).size() == 2 && rig.pops->respawn(ref2, 23.0) == 0 && rig.pops->actors_of(kJoeva).size() == 2,
          "a guard that died, then the level rose: the villager stands there already, and the guard's respawn does nothing");
}

// ---- the ground's window ------------------------------------------------------------------------------------------------------------------

/// How many of the registry's NPCs (those in `data_z`) stand more than a centimetre from the z the data gave them, in the registry and in the open the client is sent.
struct HeightKept {
    size_t npcs = 0, moved = 0, open_moved = 0;
    double worst = 0;
};
static HeightKept height_kept(const world::ActorRegistry& reg, const std::map<uint64_t, double>& data_z) {
    HeightKept h;
    reg.for_each([&](const world::WorldActor& a) {
        const auto it = data_z.find(a.id);
        if (a.kind != world::ActorKind::Npc || it == data_z.end()) return;
        ++h.npcs;
        const double d = std::fabs(a.z - it->second);
        h.worst = std::max(h.worst, d);
        if (d > 1.0) ++h.moved;
        if (a.actor.open.spawn.location && std::fabs(aoc::protocol::repl::dequantize_vector(*a.actor.open.spawn.location, 10).z - it->second) > 1.0) ++h.open_moved;
    });
    return h;
}
// C.Y.B x CoA

static void test_pending() {
    std::printf("the ground's window: NPCs that wait for their chunk\n");
    std::string error;
    const world::CityNodeInfo* real = g_env.nodes->by_guid(kJoeva);
    world::CityNodeInfo j = *real;
    j.x = 500;
    j.y = 500;
    j.z = 11000;   // (a BOWL: the fixture's baked ground is a flat plain at 1,000 and the town's floor is 100 m above it, as under Joeva's: the data's heights are out of every surface's reach)
    std::vector<world::CityZoneInfo> zones{*g_env.nodes->zone_by_guid(j.zoi_guid)};
    world::CityNodes synth({j}, zones);
    set_level(synth, kJoeva, 4);
    auto wire = world::NodeWire::load(db_path(), synth, error);
    if (!wire) {
        CHECK(false, "the node wire loads for a registry of one node");
        std::printf("    %s\n", error.c_str());
        return;
    }
    const std::string path = navfix::write_chunk_file("nodepop");
    auto nav = nav::AreaNav::load(path, &error);
    if (!nav) {
        CHECK(false, "the chunked fixture opens");
        return;
    }
    nav->load_around(0, 0, 0);   // the centre's chunk (2, 2) only
    world::ActorRegistry* reg = nullptr;
    spawns::AreaConfig everywhere;
    everywhere.center_x = 0;
    everywhere.center_y = 0;
    everywhere.radius_cm = 1.0e8;
    std::unique_ptr<npc::NpcWorld> npcs;
    npc::NpcMotionSetup stand;
    stand.foot_offset = 88;
    std::map<uint64_t, double> data_z;   // each NPC's z as the data made it (read before the NPC world touches it)
    Rig rig(synth, *wire, everywhere, [&](uint64_t id, const content::NpcInstanceRow*, const content::NpcRouteRow*) {
        if (reg) data_z[id] = reg->find(id)->z;
        if (npcs) npcs->add(id, stand);
    });
    reg = &rig.registry;
    npcs = std::make_unique<npc::NpcWorld>(*reg, nav.get());
    const size_t made = rig.pops->start(0.0);
    const auto waiting_now = [&] {
        size_t w = 0;
        reg->for_each([&](const world::WorldActor& a) { w += a.kind == world::ActorKind::Npc && !nav->ready_at(a.x, a.y) ? 1 : 0; });
        return w;
    };
    const size_t waiting = waiting_now();
    CHECK(made == golden_npcs("Verra_RVR_Joeva", 4) && npcs->size() == made, "a town beside a chunked ground: every NPC is under the NPC world's care");
    CHECK(waiting > 0 && waiting < made && npcs->pending_ground() == waiting, "the NPCs whose chunk is not resident wait for it (the count is the NPC world's): the others are grounded at once");
    // (the test-2 fix) a town in a bowl: the baked ground is far under the town's floor, so the people stand at the heights the data gave them, waiting or grounded
    {
        const HeightKept h = height_kept(*reg, data_z);
        char msg[240];
        std::snprintf(msg, sizeof(msg), "bowl: all %zu people keep their data height while %zu wait for their chunk (%zu moved, %zu opens moved, the most %.1f cm)", h.npcs, waiting, h.moved, h.open_moved, h.worst);
        CHECK(h.npcs == made && h.moved == 0 && h.open_moved == 0, msg);
    }
    // a level change takes waiting NPCs away and brings new ones: the count follows
    set_level(synth, kJoeva, 3);
    rig.pops->tick(1.0);
    CHECK(rig.pops->actors_of(kJoeva).size() == golden_npcs("Verra_RVR_Joeva", 3) && npcs->size() == rig.pops->actors_of(kJoeva).size() && npcs->pending_ground() == waiting_now(),
          "Town -> Village: the NPC world holds exactly the town's people and its pending count follows (an NPC removed while it waits stops waiting)");
    // the chunks around the town arrive: the NPCs of the chunks are grounded
    std::vector<nav::ChunkKey> arrived;
    nav->set_interest({{500.0, 500.0}});   // (one player at the town's centre: the window loads the chunks within 2 of it and keeps them)
    for (int i = 0; i < 800; ++i) {
        const auto got = nav->pump(50);
        arrived.insert(arrived.end(), got.begin(), got.end());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    npcs->chunks_resident(arrived);
    CHECK(!arrived.empty() && npcs->pending_ground() == waiting_now(), "the chunks that arrived ground the NPCs standing in them: the count is the NPCs still outside the window");
    {
        const HeightKept h = height_kept(*reg, data_z);
        char msg[240];
        std::snprintf(msg, sizeof(msg), "bowl: after the chunks arrived, %zu people still stand at their data height (%zu moved, %zu opens moved, the most %.1f cm)", h.npcs - h.moved, h.moved, h.open_moved, h.worst);
        CHECK(h.npcs == npcs->size() && h.moved == 0 && h.open_moved == 0, msg);
    }
    // an NPC added after the chunk is here is grounded at once
    set_level(synth, kJoeva, 4);
    rig.pops->tick(2.0);
    CHECK(rig.pops->actors_of(kJoeva).size() == golden_npcs("Verra_RVR_Joeva", 4) && npcs->pending_ground() == waiting_now(), "Village -> Town: the new NPCs wait or stand on the ground, by their chunk");
    {
        const HeightKept h = height_kept(*reg, data_z);
        char msg[240];
        std::snprintf(msg, sizeof(msg), "bowl: the people the level change brought keep their data height too (%zu of %zu moved, the most %.1f cm)", h.moved, h.npcs, h.worst);
        CHECK(h.npcs == npcs->size() && h.moved == 0 && h.open_moved == 0, msg);
    }
    npcs.reset();   // (before the registry it listens to, and before the ground's file is closed and removed)
    nav.reset();
    std::error_code ec;
    fs::remove(path, ec);
}
// CoA : C Y B

// ---- the world ------------------------------------------------------------------------------------------------------------------------------

struct RoadFile {
    struct Road { int64_t zoi = 0; int tiers[7] = {0}; };
    std::vector<Road> roads;
};
static bool read_roads_levels(const std::string& path, RoadFile& out) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string cell;
        while (std::getline(ss, cell, '\t')) f.push_back(cell);
        if (f.size() < 6 || f[0] != "roadn") return false;
        RoadFile::Road r;
        r.zoi = static_cast<int64_t>(std::strtoull(f[1].c_str(), nullptr, 10));
        std::stringstream ts(f[2]);
        std::string t;
        int k = 0;
        while (std::getline(ts, t, ',') && k < 7) r.tiers[k++] = std::atoi(t.c_str());
        out.roads.push_back(r);
    }
    return true;
}

static void test_world() {
    std::printf("PlayerWorld: start_node_people, tick_world, respawn_npc and the roads\n");
    std::string error;
    aoc::protocol::ServerGuidAllocator guids{60, 0x6ee87ee4u};
    PlayerWorld world(&*g_env.db, "", bs::GameStateMode::Tier3, 60, guids);
    CHECK(world.ready(), "a PlayerWorld on the real content.db is ready");
    if (!world.ready()) return;
    auto nodes = fresh_nodes();
    spawns::NodePopulations::Config cfg;
    CHECK(world.start_node_people(cfg) == 0 && world.node_people() == nullptr, "without an area, the nodes' wire and the population data there are no people");
    world.set_spawn_context(g_env.base);
    CHECK(world.load_blueprints(motion_path(), error), "the Blueprint table loads");
    world.set_city_nodes(nodes.get());
    world.set_node_wire(g_env.wire.get());
    world.set_node_population_data(g_env.data.get());
    const world::CityNodeInfo* joeva = nodes->by_guid(kJoeva);
    spawns::AreaConfig circle;
    circle.center_x = joeva->x;
    circle.center_y = joeva->y;
    circle.radius_cm = 150000.0;
    CHECK(world.load_area(circle, error), "a 1.5 km circle around Joeva loads");
    // the roads: the by-level file, with the world's default level for a zone with no node
    RoadFile roads;
    const bool have_roads = read_roads_levels(NODES_ROADS_LEVELS_PATH, roads);
    world.set_road_default_level(0);
    if (have_roads) {
        CHECK(world.load_roads(NODES_ROADS_LEVELS_PATH, error) && world.roads().level_roads() == roads.roads.size() && roads.roads.size() > 100, "the by-level roads load into the world (145 roads)");
    }
    const auto circle_npcs = [&](int level) {
        spawns::NodePopStats st;
        return select(*nodes, *g_env.wire, *joeva, level, st, circle).size();
    };
    const size_t before_npcs = world.registry().count(world::ActorKind::Npc);
    const size_t made = world.start_node_people(cfg);
    CHECK(made == circle_npcs(4) && made > 400 && world.node_people() && world.registry().count(world::ActorKind::Npc) == before_npcs + made,
          "start_node_people: the circle holds Joeva only (a Town): the oracle's number of NPCs, added to the area's");
    CHECK(world.start_node_people(cfg) == 0, "once");
    size_t vendors = 0, with_ref = 0;
    world.registry().for_each([&](const world::WorldActor& a) {
        if (a.npc.spawn_ref >= spawns::kNodeRefBase) {
            ++with_ref;
            vendors += a.npc.vendor_guid != 0 ? 1 : 0;
        }
    });
    CHECK(with_ref == made && vendors > 0, "the town's NPCs carry node references, and some are vendors");
// [C | Y | B ~ CoA]

    // the roads at the seed's levels: a road of Joeva's zone is worth what Town gives it
    auto tier_expectations = [&](int joeva_level) {
        size_t bad = 0, joeva_roads = 0;
        for (size_t i = 0; i < roads.roads.size(); ++i) {
            const RoadFile::Road& r = roads.roads[i];
            const world::CityZoneInfo* z = nodes->zone_by_guid(r.zoi);
            int level = 0;
            if (z && z->node_guid != 0) level = nodes->level_of(z->node_guid);
            if (r.zoi == joeva->zoi_guid) ++joeva_roads;
            if (world.roads().tier_of_road(i) != r.tiers[level]) ++bad;
        }
        (void)joeva_level;
        return std::pair<size_t, size_t>{bad, joeva_roads};
    };
    if (have_roads) {
        const auto [bad, joeva_roads] = tier_expectations(4);
        CHECK(bad == 0 && joeva_roads > 0, "every road is worth what its zone's node level gives it (Joeva's roads at Town)");
        size_t differ = 0;
        for (const RoadFile::Road& r : roads.roads) differ += r.zoi == joeva->zoi_guid && r.tiers[4] != r.tiers[3] ? 1 : 0;
        std::printf("    (%zu of Joeva's roads change tier between Town and Village)\n", differ);
    }

    // a GM level change: the world's tick follows it
    set_level(*nodes, kJoeva, 3);
    world.tick_world(10.0);
    const spawns::NodePopulations::Entry* e = world.node_people()->entry_of(kJoeva);
    CHECK(e && e->level == 3 && e->npcs == circle_npcs(3) && world.registry().count(world::ActorKind::Npc) == before_npcs + e->npcs,
          "tick_world follows Joeva's level change: Village's people, the registry holds the area's NPCs and these");
    if (have_roads) {
        const auto [bad, joeva_roads] = tier_expectations(3);
        CHECK(bad == 0 && joeva_roads > 0, "...and the roads of Joeva's zone changed with it");
    }
    // respawn_npc of a node reference
    uint64_t victim = 0;
    uint32_t ref = 0;
    world.registry().for_each([&](const world::WorldActor& a) {
        if (a.npc.spawn_ref >= spawns::kNodeRefBase && victim == 0 && a.npc.vendor_guid == 0) {
            victim = a.id;
            ref = a.npc.spawn_ref;
        }
    });
    const int64_t asset = world.registry().find(victim)->npc.asset_guid;
    world.registry().remove(victim);
    const uint64_t again = world.respawn_npc(ref, 11.0);
    const world::WorldActor* back = world.registry().find(again);
    CHECK(again != 0 && back && back->npc.spawn_ref == ref && back->npc.asset_guid == asset, "respawn_npc brings a node NPC back through the node manager");
    CHECK(world.respawn_npc(spawns::kNodeRefBase + 7777777u, 11.0) == 0, "...and a reference that is no slot is nothing");
}

// ---- the roads -------------------------------------------------------------------------------------------------------------------------------

static world::RoadMap::Polyline by_level_road(int64_t zoi, std::array<int, 7> tiers, std::array<double, 7> halves, double x0, double y0, double x1, double y1) {
    world::RoadMap::Polyline r;
    r.by_level = true;
    r.zoi = zoi;
    r.tiers = tiers;
    r.halves = halves;
    r.xy = {x0, y0, x1, y1};
    return r;
}

static void test_roads() {
    std::printf("roads by node level\n");
    world::RoadMap m;
    // an Artery's tiers and widths: T1 T1 T2 T3 T4 T4 T4, wider as it grows; a Capillary: nothing until Village
    CHECK(m.add(by_level_road(11, {1, 1, 2, 3, 4, 4, 4}, {500, 500, 500, 500, 600, 900, 900}, 0, 0, 10000, 0), 0), "a by-level road is added");
    CHECK(m.add(by_level_road(22, {-1, -1, -1, 2, 4, 4, 4}, {0, 0, 0, 500, 500, 500, 500}, 0, 5000, 10000, 5000), 0), "...and one with nothing for a mount below Village");
    world::RoadMap::Polyline plain;
    plain.tier = 3;
    plain.half_width_cm = 500;
    plain.xy = {0, 9000, 10000, 9000};
    CHECK(m.add(plain), "...and a plain road");
    CHECK(m.level_roads() == 2 && m.roads() == 3 && m.level_of_road(0) == 0 && m.level_of_road(2) == -1, "two of the three follow a level; they start at level 0");
    CHECK(m.tier_at(5000, 0) == 1 && m.tier_at(5000, 5000) == -1 && m.tier_at(5000, 9000) == 3, "level 0: the artery is T1, the capillary gives a mount nothing, the plain road T3");
    CHECK(m.tier_of_road(0) == 1 && m.tier_of_road(1) == -1 && m.tier_of_road(2) == 3, "(the roads' tiers by index)");
    // the levels of the zones
    const auto level_of = [](int64_t zoi) { return zoi == 11 ? 4 : zoi == 22 ? 3 : -1; };
    CHECK(m.apply_levels(level_of, 0) == 2, "two roads change when their zones' nodes level up");
    CHECK(m.tier_at(5000, 0) == 4 && m.tier_at(5000, 5000) == 2 && m.tier_at(5000, 9000) == 3, "Town: the artery is T4, the capillary T2 (Village), the plain road is untouched");
    CHECK(m.tier_at(5000, 550) == 4 && m.tier_at(5000, 650) == -1, "...and an artery's half width grows with its level (600 cm at Town)");
    CHECK(m.apply_levels(level_of, 0) == 0, "nothing changes the second time");
    CHECK(m.apply_levels([](int64_t) { return 6; }, 0) == 2 && m.tier_at(5000, 700) == 4 && m.tier_at(5000, 950) == -1, "Metropolis: 900 cm of half width");
    CHECK(m.apply_levels([](int64_t) { return -1; }, 2) == 2 && m.level_of_road(0) == 2 && m.tier_at(5000, 0) == 2 && m.tier_at(5000, 5000) == -1, "a zone with no node takes the default level (2): the capillary has nothing there");
    CHECK(m.apply_levels(nullptr, 0) == 1 && m.tier_at(5000, 0) == 1 && m.level_of_road(1) == 0, "(no function: every road at the default level; the capillary's level changed, its tier did not)");
    // bad roads
    world::RoadMap bad;
    CHECK(!bad.add(by_level_road(1, {5, 0, 0, 0, 0, 0, 0}, {500, 500, 500, 500, 500, 500, 500}, 0, 0, 1, 1)), "a tier above 4 is refused");
    CHECK(!bad.add(by_level_road(1, {-1, -1, -1, -1, -1, -1, -1}, {0, 0, 0, 0, 0, 0, 0}, 0, 0, 1, 1)), "a road with nothing at any level is refused");
    CHECK(!bad.add(by_level_road(1, {1, 1, 1, 1, 1, 1, 1}, {500, 500, 0, 500, 500, 500, 500}, 0, 0, 1, 1)), "a level with a tier and no width is refused");
    CHECK(!bad.add(by_level_road(1, {1, 1, 1, 1, 1, 1, 1}, {500, 500, 500, 500, 500, 500, 500}, 0, 0, 1, 1), 7), "a start level beyond Metropolis is refused");
    CHECK(bad.roads() == 0, "(none was added)");
    // a file
    const fs::path dir = fs::temp_directory_path() / "worldd_node_pop_roads";
    fs::create_directories(dir);
    const fs::path file = dir / "roads_levels.tsv";
    {
        std::ofstream f(file);
        f << "# roads v2: test\n"
          << "roadn\t11\t1,1,2,3,4,4,4\t500,500,500,500,600,900,900\t0,0\t10000,0\n"
          << "road\t3\t500\t0,9000\t10000,9000\n";
    }
    world::RoadMap fm;
    std::string error;
    CHECK(fm.load(file.string(), error, 4) && fm.roads() == 2 && fm.level_roads() == 1 && fm.level_of_road(0) == 4 && fm.tier_at(5000, 0) == 4 && fm.tier_at(5000, 9000) == 3,
          "a file with a roadn line and a road line loads; the by-level road starts at the level the loader was given (Town)");
    {
        std::ofstream f(file);
        f << "roadn\t11\t1,1,2,3,4,4\t500,500,500,500,600,900,900\t0,0\t10000,0\n";
    }
    world::RoadMap short_file;
    CHECK(!short_file.load(file.string(), error) && error.find("numbers each") != std::string::npos, "a roadn line with six tiers is refused");
    {
        std::ofstream f(file);
        f << "roadn\tx\t1,1,2,3,4,4,4\t500,500,500,500,600,900,900\t0,0\t10000,0\n";
    }
    CHECK(!short_file.load(file.string(), error), "...and one whose zone is no number");
    fs::remove_all(dir);

    // the real file
    RoadFile real;
    if (!read_roads_levels(NODES_ROADS_LEVELS_PATH, real)) {
        std::printf("  [skip] no %s (contentc roads --levels)\n", NODES_ROADS_LEVELS_PATH);
        return;
    }
    world::RoadMap rm;
    CHECK(rm.load(NODES_ROADS_LEVELS_PATH, error, 0) && rm.roads() == real.roads.size() && rm.level_roads() == rm.roads() && rm.roads() > 100, "the real roads_levels.tsv loads: every road follows a level");
    const std::string one_level = std::string(NODES_ROADS_LEVELS_PATH).substr(0, std::string(NODES_ROADS_LEVELS_PATH).rfind("roads_levels.tsv")) + "roads.tsv";
    world::RoadMap wilderness;
    if (wilderness.load(one_level, error)) {
        // along every road of the one-level file the by-level map (all at level 0) gives the same tier
        size_t sampled = 0, same = 0;
        std::ifstream in(one_level);
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::vector<std::string> f;
            std::stringstream ss(line);
            std::string cell;
            while (std::getline(ss, cell, '\t')) f.push_back(cell);
            for (size_t i = 3; i < f.size(); i += 6) {
                const size_t comma = f[i].find(',');
                const double x = std::atof(f[i].substr(0, comma).c_str()), y = std::atof(f[i].substr(comma + 1).c_str());
                ++sampled;
                same += wilderness.tier_at(x, y) == rm.tier_at(x, y) ? 1 : 0;
            }
        }
        CHECK(sampled > 100 && same == sampled, "at Wilderness the by-level map and the one-level roads.tsv give the same tier along every road");
    }
    // Joeva's zone to Town: its roads' tiers are the file's t4, the others' stay at t0
    const world::CityNodeInfo* joeva = g_env.nodes->by_guid(kJoeva);
    rm.apply_levels([&](int64_t zoi) { return zoi == joeva->zoi_guid ? 4 : 0; }, 0);
    size_t wrong = 0, joeva_roads = 0, changed = 0;
    for (size_t i = 0; i < real.roads.size(); ++i) {
        const int want = real.roads[i].zoi == joeva->zoi_guid ? real.roads[i].tiers[4] : real.roads[i].tiers[0];
        wrong += rm.tier_of_road(i) != want ? 1 : 0;
        if (real.roads[i].zoi == joeva->zoi_guid) {
            ++joeva_roads;
            changed += real.roads[i].tiers[4] != real.roads[i].tiers[0] ? 1 : 0;
        }
    }
    CHECK(wrong == 0 && joeva_roads > 0 && changed > 0, "Joeva's zone at Town: its roads are worth their Town tier (some are worth more than at Wilderness), every other road is untouched");
}
// [C.O.A - C Y B]

// ---- main -----------------------------------------------------------------------------------------------------------------------------------


// ---- the town floor ---------------------------------------------------------------------------------------------------------------------------

using FloorPerson = world::TownFloor::Person;

/// A town's people on a grid of 20 m round (cx, cy), all standing `lift` over a ground at 1,000 (a hand's breadth of relief).
static std::vector<FloorPerson> floor_grid(double cx, double cy, double lift, int half = 3) {
    std::vector<FloorPerson> v;
    for (int i = -half; i <= half; ++i) {
        for (int j = -half; j <= half; ++j) v.push_back({cx + i * 2000.0, cy + j * 2000.0, 1000.0, 1000.0 + lift + ((i + j) % 3 == 0 ? 20.0 : -10.0)});
    }
    return v;
}

static void test_town_floor_class() {
    std::printf("the town floor: a town stands over the baked ground of its bowl\n");
    world::TownFloor tf;
    CHECK(tf.towns() == 0 && !tf.at(0, 0, 1000), "no town: nowhere is a bowl");
    std::vector<FloorPerson> people = floor_grid(0, 0, 5060);   // 49 on the town's floor
    for (int i = 0; i < 6; ++i) people.push_back({i * 1000.0, 30000.0, 1000.0, 8000.0});   // on a building's upper floor (70 m over the ground), 300 m off
    people.push_back({500, 0, 1000.0, 3000.0});                  // 20 m over the ground: on the wall of the pit, not on the town's floor
    people.push_back({-500, 0, 1000.0, 1100.0});                 // 1 m over the ground: not in a bowl at all
    tf.set(7, people);
    const std::optional<double> lift = tf.lift_of(7);
    CHECK(tf.towns() == 1 && lift && std::fabs(*lift - 5060.0) < 25.0, "the town's lift is the median of its people's heights over the ground (50.6 m)");
    CHECK(tf.floor_persons(7) == 49, "the floor is the people within 3 m of it: not the upper floors, the wall of the pit or the one on the open ground");
    const auto in = tf.at(100, 100, 1000);
    CHECK(in && in->node == 7 && std::fabs(in->lift - *lift) < 1e-9, "a point in the town: its floor is the ground under it plus the lift, and it names the node");
    CHECK(!tf.at(100, 100, 3000), "...but not where the ground under the point is 20 m off the people's (the wall of the pit)");
    CHECK(tf.at(100, 100, 1000 - 900) && tf.at(100, 100, 1000 + 900), "...a ground 9 m off is inside the relief");
    CHECK(tf.at(6000 + 7000, 0, 1000) && !tf.at(6000 + 8000, 0, 1000), "the town reaches 75 m from the last person of its floor");
    CHECK(!tf.at(20000, 20000, 1000) && !tf.at(0, 28000, 1000), "...not to a point far from its floor (the upper floors 300 m off do not make a floor)");
    // the artisan outposts' people stand 1 m over the ground: no town; two towns; a town whose people are gone is dropped
    tf.set(8, floor_grid(100000, 0, 100));
    CHECK(tf.towns() == 1 && !tf.at(100000, 0, 1000) && !tf.lift_of(8), "people on the open ground (+1 m) make no town");
    tf.set(9, floor_grid(200000, 0, 4990));
    CHECK(tf.towns() == 2 && tf.at(200000, 0, 1000) && std::fabs(tf.at(200000, 0, 1000)->lift - 4990.0) < 25.0 && std::fabs(tf.at(0, 0, 1000)->lift - *lift) < 1e-9,
          "two towns: each point has its own town's lift");
    tf.set(7, floor_grid(0, 0, 100));
    CHECK(tf.towns() == 1 && !tf.at(0, 0, 1000), "the node's people replaced by ones on the open ground: its town is gone");
    tf.set(9, {});
    CHECK(tf.towns() == 0, "no people: no town");
}

static void test_bowl_world() {
    std::printf("PlayerWorld in a bowl (the fixture's plain at 1,000; the town's people 100 m over it)\n");
    std::string error;
    const world::CityNodeInfo* real = g_env.nodes->by_guid(kJoeva);
    world::CityNodeInfo j = *real;
    j.x = 500;
    j.y = 500;
    j.z = 11000;
    std::vector<world::CityZoneInfo> zones{*g_env.nodes->zone_by_guid(j.zoi_guid)};
    world::CityNodes synth({j}, zones);
    set_level(synth, kJoeva, 4);
    auto wire = world::NodeWire::load(db_path(), synth, error);
    if (!wire) {
        CHECK(false, "the node wire loads for a registry of one node");
        return;
    }
    const std::string path = navfix::write_chunk_file("nodebowlworld");
    struct RemoveFile {
        const std::string& p;
        ~RemoveFile() {
            std::error_code ec;
            fs::remove(p, ec);
        }
    } remove_file{path};   // (declared before the world, which holds the file open: it goes after it, on every return)
    aoc::protocol::ServerGuidAllocator guids{60, 0x6ee87ee4u};
    PlayerWorld world(&*g_env.db, "", bs::GameStateMode::Tier3, 60, guids);
    world.set_spawn_context(g_env.base);
    if (!world.ready() || !world.load_blueprints(motion_path(), error)) {
        CHECK(false, "the world is ready and the Blueprint table loads");
        return;
    }
    world.set_city_nodes(&synth);
    world.set_node_wire(wire.get());
    world.set_node_population_data(g_env.data.get());
    spawns::AreaConfig circle;
    circle.center_x = 500;
    circle.center_y = 500;
    circle.radius_cm = 150000.0;
    auto nav = nav::AreaNav::load(path, &error);
    if (!world.load_area(circle, error) || !nav || !world.adopt_ground(std::move(nav), error)) {
        CHECK(false, "the area loads and the fixture's ground is adopted");
        std::printf("    %s\n", error.c_str());
        return;
    }
    CHECK(!world.town_ground_at(500, 500) && world.town_floor().towns() == 0, "before the node people exist, no point is in a bowl");
    spawns::NodePopulations::Config cfg;
    const size_t made = world.start_node_people(cfg);
    CHECK(made == golden_npcs("Verra_RVR_Joeva", 4), "the town's people are made on the adopted ground");
    // what the floor must be, worked out here from the people: their heights over the baked ground under them (the town's z is 11,000 + the layout's)
    const auto terrain = [&](double x, double y) {
        double t = 0;
        world.nav()->terrain_height(x, y, &t);
        return t;
    };
    std::vector<double> overs;
    std::vector<std::array<double, 3>> people;   // x, y, over
    world.registry().for_each([&](const world::WorldActor& a) {
        if (a.npc.spawn_ref < spawns::kNodeRefBase) return;
        double t = 0;
        if (!world.nav()->terrain_height(a.npc.home_x, a.npc.home_y, &t)) return;   // (a person outside the extracted terrain is no part of the floor)
        const double over = a.npc.home_z - t;
        if (over > world::TownFloor::kBowlCm) {
            overs.push_back(over);
            people.push_back({a.npc.home_x, a.npc.home_y, over});
        }
    });
    std::sort(overs.begin(), overs.end());
    const double median = overs.empty() ? 0.0 : overs[overs.size() / 2];
    const auto on_floor = std::find_if(people.begin(), people.end(), [&](const std::array<double, 3>& p) { return std::fabs(p[2] - median) <= world::TownFloor::kSameCm; });
    CHECK(!overs.empty() && on_floor != people.end(), "(the people stand over the plain, and some of them at the median's height)");
    if (overs.empty() || on_floor == people.end()) return;
    const double px = (*on_floor)[0], py = (*on_floor)[1];
    const double base = terrain(px, py);   // the baked ground under that person
    const auto town = world.town_ground_at(px, py);
    char msg[260];
    std::snprintf(msg, sizeof(msg), "the town's floor at a person of its floor: the baked ground there (%.0f) + the median %.0f cm of the people's heights over it", base, median);
    CHECK(town && town->node == kJoeva && std::fabs(town->z - (base + median)) < 0.01, msg);
    const auto ground = world.ground_at(px, py);
    CHECK(ground && std::fabs(*ground - (base + median + PlayerWorld::kBowlSlackCm)) < 0.01, "ground_at there: the town's floor + the slack (a GM teleport and the saved rider land on it), not the baked plain");
    world::Place place;
    place.name = "Joeva";
    place.x = px;
    place.y = py;
    place.z = 11000;
    CHECK(std::fabs(world.place_height(place) - (base + median + PlayerWorld::kBowlSlackCm + PlayerWorld::kCapsuleHalfHeightCm)) < 0.01,
          "a GM teleport to a place in the bowl is put on the town's floor (+ the capsule), not 50 m under it");
    // far from the town the ground is the plain's
    double plain = 0;
    const auto far_ground = world.ground_at(25000, 500);
    CHECK(world.nav()->terrain_height(25000, 500, &plain) && !world.town_ground_at(25000, 500) && far_ground && std::fabs(*far_ground - plain) < 1e-9, "far from the town: the baked ground, as before");
    // the login: a character saved on the baked plain under the town is lifted; one on the floor, and one far away, are not
    const auto lifted = world.lift_out_of_bowl(px, py, base + 98.0);
    CHECK(lifted && lifted->node == kJoeva && std::fabs(lifted->z - (base + median + 98.0 + PlayerWorld::kBowlSlackCm)) < 0.01 && std::fabs(lifted->under_cm - median) < 1.0,
          "a character saved on the baked plain under the town is put on the town's floor (+ capsule + slack)");
    CHECK(!world.lift_out_of_bowl(px, py, base + median + 98.0) && !world.lift_out_of_bowl(px, py, base + median + 98.0 - 1400.0),
          "...not one that stands on the floor, nor one less than 15 m under it (a terrace)");
    CHECK(!world.lift_out_of_bowl(25000, 500, terrain(25000, 500) + 98.0), "...nor one saved on the open ground far from the town");
    // the floor follows the people: a level change brings other people
    const size_t town_persons = world.town_floor().floor_persons(kJoeva);
    set_level(synth, kJoeva, 0);
    world.tick_world(5.0);
    world.town_ground_at(px, py);   // (asking is what rebuilds it)
    CHECK(world.town_floor().floor_persons(kJoeva) != town_persons, "after a level change the floor is rebuilt from the people the new level has");
}

// ---- the five bowls on the real ground ---------------------------------------------------------------------------------------------------------

#ifndef NODES_NAV_PATH
#define NODES_NAV_PATH "dist/content/area_nav.bin"
#endif
// (Cyb + C O A)

/// Opt-in (AOC_REAL_GROUND=1: it opens dist/content/area_nav.bin, which loads the 161 MB heightfield and the chunk directory in a fraction of a second and no tile): the town floor built
/// from the REAL people of the five city nodes over the REAL baked terrain.
static void test_real_bowls() {
    const char* env = std::getenv("AOC_REAL_GROUND");
    if (!env || !*env) {
        std::printf("the five bowls on the real ground: opt-in (AOC_REAL_GROUND=1 reads the heightfield of %s)\n", NODES_NAV_PATH);
        return;
    }
    std::printf("the five bowls on the real ground (%s)\n", NODES_NAV_PATH);
    std::string error;
    auto nav = nav::AreaNav::load(NODES_NAV_PATH, &error);
    CHECK(nav != nullptr, "the chunked ground opens");
    if (!nav) {
        std::printf("    %s\n", error.c_str());
        return;
    }
    const auto terrain = [&](double x, double y) {
        double t = 0;
        nav->terrain_height(x, y, &t);
        return t;
    };
    const char* names[] = {"Verra_RVR_Winstead", "Verra_RVR_Joeva", "Verra_RVR_NewAela", "Verra_RVR_Halcyon", "Verra_RVR_Miraleth"};
    for (const char* name : names) {
        const world::CityNodeInfo* info = nullptr;
        for (const world::CityNodeInfo& n : g_env.nodes->nodes()) {
            if (n.name == name) info = &n;
        }
        CHECK(info != nullptr, name);
        if (!info) continue;
        for (const int level : {0, 4}) {
            spawns::NodePopStats st;
            const auto list = select_real(*info, level, st);
            std::vector<FloorPerson> persons;
            size_t open = 0, open_in = 0;
            world::TownFloor tf;
            for (const spawns::NodeSpawn& n : list) {
                FloorPerson p{n.spawn.x, n.spawn.y, 0, n.spawn.z};
                if (!nav->terrain_height(p.x, p.y, &p.ground)) continue;
                persons.push_back(p);
            }
            tf.set(info->guid, persons);
            const std::optional<double> lift = tf.lift_of(info->guid);
            for (const FloorPerson& p : persons) {
                if (p.z - p.ground > world::TownFloor::kBowlCm) continue;
                ++open;   // standing on the open ground (the rim, the wild land beyond the pit): the truth is "not in the bowl"
                if (tf.at(p.x, p.y, p.ground)) ++open_in;
            }
            char msg[300];
            std::snprintf(msg, sizeof(msg), "%s at level %d: %zu people, %zu on the town's floor, lift %.0f cm; of the %zu on open ground (+1 m) %zu are taken for the bowl", name, level, persons.size(),
                          tf.floor_persons(info->guid), lift.value_or(0.0), open, open_in);
            CHECK(lift && *lift > 4800.0 && *lift < 5200.0 && tf.floor_persons(info->guid) >= (level == 4 ? 250u : 100u), msg);
            std::printf("    %s\n", msg);
            if (level != 4) continue;
            // the node's own place (a GM teleport): in the bowl, on the floor 50 m over the baked ground
            const double rt = terrain(info->x, info->y);
            const auto at_record = tf.at(info->x, info->y, rt);
            std::snprintf(msg, sizeof(msg), "%s: the node's place (%.0f, %.0f), baked ground %.0f, is in the bowl: the floor is %.0f", name, info->x, info->y, rt, at_record ? rt + at_record->lift : 0.0);
            CHECK(at_record.has_value(), msg);
            std::printf("    %s\n", msg);
            // where retail opened the town's layout (its origin: the layout's offsets are from there), for the 13 nodes a capture shows
            for (const world::node_origins::Origin& o : world::node_origins::kOrigins) {
                if (o.node != info->guid) continue;
                const double ot = terrain(o.x, o.y);
                const auto of = tf.at(o.x, o.y, ot);
                std::snprintf(msg, sizeof(msg), "%s: retail opened the layout at (%.0f, %.0f, %.0f), baked ground %.0f: the floor there is %.0f (%+.0f cm)", name, o.x, o.y, o.z, ot, of ? ot + of->lift : 0.0,
                              of ? o.z - (ot + of->lift) : 0.0);
                CHECK(of.has_value(), msg);
                std::printf("    %s\n", msg);
            }
            if (std::string(name) != "Verra_RVR_Joeva") continue;
            // the user's saved position (the rider's last good position was the baked ground + 98), under Joeva's town
            const double ux = -1079493.1, uy = 508464.4, uz = 7748.6, ut = terrain(ux, uy);
            const auto user = tf.at(ux, uy, ut);
            std::snprintf(msg, sizeof(msg), "Joeva: the user's saved spot (z %.1f, baked ground %.1f) is in the bowl: the town's floor %.0f, %.0f m over it", uz, ut, user ? ut + user->lift : 0.0,
                          user ? (ut + user->lift - uz) / 100.0 : 0.0);
            CHECK(user && ut + user->lift + 98.0 - uz > 4000.0, msg);
            std::printf("    %s\n", msg);
            // retail's 9 anchors (capture 721): the model's height over the terrain against the height retail's NPC stood at
            struct Anchor { double x, y, z; };
            const Anchor anchors[] = {{-1081432, 516570, 14753}, {-1097474, 509458, 14024}, {-1091333, 510020, 13408}, {-1085254, 503627, 12608}, {-1089840, 503696, 12625},
                                      {-1078157, 512855, 13046}, {-1087121, 515470, 14042}, {-1081577, 505744, 12638}, {-1079600, 509538, 13290}};
            size_t close = 0, within = 0;
            double worst = 0;
            for (const Anchor& a : anchors) {
                const double t = terrain(a.x, a.y);
                const auto f = tf.at(a.x, a.y, t);
                if (!f) continue;
                const double err = std::fabs(t + f->lift - a.z);
                worst = std::max(worst, err);
                close += err <= 200.0 ? 1 : 0;
                within += err <= 1300.0 ? 1 : 0;
            }
            std::snprintf(msg, sizeof(msg), "Joeva: against retail's 9 anchors the floor (terrain + lift) is within 2 m of %zu and within 13 m of %zu (the most %.0f cm: a tower, a terrace)", close, within, worst);
            CHECK(close >= 7 && within == 9, msg);
            std::printf("    %s\n", msg);
        }
    }
}
// == C O A | C_Y_B ==

int main() {
    spdlog::set_level(std::getenv("AOC_TEST_LOG") ? spdlog::level::debug : spdlog::level::off);
    std::string error;
    g_env.db = content::ContentDb::open(db_path(), error);
    if (!g_env.db) {
        std::printf("  [FAIL] the real content.db does not open (%s): %s\n", db_path().c_str(), error.c_str());
        return 2;
    }
    auto pop = g_env.db->population_data(error);
    if (!pop) {
        std::printf("  [FAIL] the population chain does not read: %s\n", error.c_str());
        return 2;
    }
    g_env.pop = std::make_shared<const content::PopulationData>(std::move(*pop));
    g_env.captured = spawns::CapturedData::load(error);
    if (!g_env.captured) {
        std::printf("  [FAIL] the captured spawns do not load: %s\n", error.c_str());
        return 2;
    }
    if (!g_env.table.load(motion_path(), error)) {
        std::printf("  [FAIL] the Blueprint table does not load (%s): %s\n", motion_path().c_str(), error.c_str());
        return 2;
    }
    g_env.generic.emplace(*g_env.captured, &g_env.table, spawns::GenericConfig{});
    g_env.templates = spawns::TemplateSource{&*g_env.captured, &*g_env.generic};
    g_env.nodes = fresh_nodes();
    g_env.wire = world::NodeWire::load(db_path(), *g_env.nodes, error);
    if (!g_env.wire) {
        std::printf("  [FAIL] the node wire does not load: %s\n", error.c_str());
        return 2;
    }
    g_env.data = spawns::NodePopulationData::load(db_path(), error);
    if (!g_env.data) {
        std::printf("  [FAIL] the node population data does not load: %s\n", error.c_str());
        return 2;
    }
    g_env.base.hour = 11.0;
    g_env.shown = g_env.captured->npc_leaves();
    test_filter();
    test_data();
    test_anchors();
    test_golden();
    test_manager();
    test_replacement();
    test_pending();
    test_world();
    test_roads();
    test_town_floor_class();
    test_bowl_world();
    test_real_bowls();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
