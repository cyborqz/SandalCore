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
//    the frames      a node's layout sets are placed at the node RECORD's location and yaw, a building's sets at the plot's transform in that frame; the 10 retail NPCs the retail capture shows
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
#include "fake_client.h"
#include "nav_chunk_fixture.h"
#include "node_pop_golden.h"
#include "content/content_db.h"
#include "nav/area_nav.h"
#include "npc/area_npcs.h"
#include "npc/motion_table.h"
#include "npc/npc_world.h"
#include "protocol/schema/generated/index.h"
#include "session/actor_view.h"
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

#include <nlohmann/json.hpp>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <bit>
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
namespace conn = aoc::protocol::conn;
namespace repl = aoc::protocol::repl;
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
    std::unique_ptr<world::CityNodes>        shipped_nodes;   // the same nodes with the shipped seed's workshops
    std::unique_ptr<world::NodeWire>         shipped_wire;
    std::unique_ptr<spawns::NodePopulationData> data;
    spawns::SpawnContext                     base;
    std::set<std::string>                    shown;
    spawns::TemplateSource                   templates;
};
static Env g_env;

/// The seed's levels with the file's plot list (the workshops the nodes start with) taken out, in a file of the temp directory: the oracle's counts are of towns that start with no workshop.
static std::string seed_without_plots() {
    std::ifstream in(NODES_SEED_PATH);
    nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        std::printf("  [FAIL] the seed file does not read: %s\n", NODES_SEED_PATH);
        std::exit(2);
    }
    j.erase("plots");
    const std::string path = (std::filesystem::temp_directory_path() / "node_pop_seed_no_plots.json").string();
    std::ofstream(path) << j.dump();
    return path;
}

/// The nodes with the seed's levels.  `with_plots` keeps the shipped seed's workshops; the baseline (the oracle's world) has none.
static std::unique_ptr<world::CityNodes> fresh_nodes(bool with_plots = false) {
    std::string error;
    auto nodes = world::CityNodes::load(db_path(), error);
    if (!nodes) {
        std::printf("  [FAIL] city nodes: %s\n", error.c_str());
        std::exit(2);
    }
    size_t applied = 0;
    nodes->apply_seed_file(with_plots ? std::string(NODES_SEED_PATH) : seed_without_plots(), error, &applied);
    return nodes;
}

/// What the nodes' people hold in a registry: their NPCs and (crafting, step 0) their crafting stations, which the same selection places.
static size_t people_of(const world::ActorRegistry& registry) { return registry.count(world::ActorKind::Npc) + registry.count(world::ActorKind::Station); }

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

// ---- the workshops the seed starts the towns with ------------------------------------------------------------------------------------------

/// The NPCs a selection put at a plot, by their Blueprint's leaf (name and path of the asset): {villagers, hammer smiths, shovel farmers, others}.
struct PlotFolk { size_t total = 0, villager_f = 0, hammer = 0, shovel = 0; };
static PlotFolk folk_at(const std::vector<spawns::NodeSpawn>& list, int64_t plot) {
    PlotFolk f;
    for (const spawns::NodeSpawn& s : list) {
        if (plot == 0 || s.plot != plot || !s.spawn.asset) continue;
        const std::string id = s.spawn.asset->name + " " + s.spawn.asset->archetype_path;
        ++f.total;
        if (id.find("Villager_M_Hammer") != std::string::npos) ++f.hammer;
        else if (id.find("Villager_M_Shovel") != std::string::npos) ++f.shovel;
        else if (id.find("Villager_F") != std::string::npos || id.find("Villager_M") != std::string::npos) ++f.villager_f;   // a man or a woman of the filler pair
    }
    return f;
}

static void test_workshops() {
    std::printf("the workshops the seed starts the towns with\n");
    world::CityNodes& shipped = *g_env.shipped_nodes;
    const world::NodeWire& swire = *g_env.shipped_wire;
    CHECK(shipped.plot_choices(kJoeva).size() == 5 && shipped.plot_choices(kHalcyon).size() == 1 && shipped.plot_choices(kWinstead).empty() && g_env.nodes->plot_choices(kJoeva).empty(),
          "the shipped seed lists 5 workshops for Joeva and 1 for Halcyon, none for Winstead; the baseline nodes (the seed without its plots) have none");

    struct Town { const char* name; int64_t guid; int level; size_t factories, farms; };
    const Town towns[] = {{"Joeva", kJoeva, 4, 4, 1}, {"Halcyon", kHalcyon, 3, 1, 0}};
    for (const Town& t : towns) {
        const world::CityNodeInfo& info = *g_env.nodes->by_guid(t.guid);
        const std::vector<world::PlotRow> rows = swire.plot_rows_of(t.guid, t.level), base_rows = g_env.wire->plot_rows_of(t.guid, t.level);
        size_t changed = 0;
        for (size_t i = 0; i < rows.size() && i < base_rows.size(); ++i) changed += rows[i].building != base_rows[i].building || rows[i].state != base_rows[i].state;
        char msg[240];
        std::snprintf(msg, sizeof msg, "%s at level %d: %zu plots change (the workshops of the list), the others are as the layout has them", t.name, t.level, t.factories + t.farms);
        CHECK(rows.size() == base_rows.size() && changed == t.factories + t.farms, msg);

        spawns::NodePopStats st0, st1;
        const auto base = select(*g_env.nodes, *g_env.wire, info, t.level, st0, spawns::AreaConfig::riverlands());
        const auto with = select(shipped, swire, info, t.level, st1, spawns::AreaConfig::riverlands());
        size_t villagers = 0, hammers = 0, shovels = 0, at_plots = 0;
        for (size_t i = 0; i < rows.size() && i < base_rows.size(); ++i) {
            if (rows[i].building == base_rows[i].building && rows[i].state == base_rows[i].state) continue;
            const PlotFolk was = folk_at(base, rows[i].plot), now = folk_at(with, rows[i].plot);
            villagers += now.villager_f - was.villager_f;
            hammers += now.hammer - was.hammer;
            shovels += now.shovel - was.shovel;
            at_plots += now.total - was.total;
        }
        std::printf("    %s: %zu NPCs without workshops, %zu with (%zu at the workshops: %zu villagers, %zu hammer smiths, %zu farmers)\n", t.name, base.size(), with.size(), at_plots, villagers, hammers, shovels);
        std::snprintf(msg, sizeof msg, "%s: a factory brings 2 villagers and 2 hammer smiths, a farm 2 shovel farmers", t.name);
        CHECK(villagers == 2 * t.factories && hammers == 2 * t.factories && shovels == 2 * t.farms, msg);
        std::snprintf(msg, sizeof msg, "%s has %zu NPCs more than with no workshop", t.name, 4 * t.factories + 2 * t.farms);
        CHECK(with.size() == base.size() + 4 * t.factories + 2 * t.farms, msg);
    }

    // a node the list does not name is the baseline's
    spawns::NodePopStats a, b;
    const world::CityNodeInfo& winstead = *g_env.nodes->by_guid(kWinstead);
    CHECK(select(shipped, swire, winstead, 3, a, spawns::AreaConfig::riverlands()).size() == select(*g_env.nodes, *g_env.wire, winstead, 3, b, spawns::AreaConfig::riverlands()).size(),
          "Winstead (no list) has the same people with the shipped seed");
}

// ---- retail's NPCs --------------------------------------------------------------------------------------------------------------------------

static void test_anchors() {
    std::printf("retail's NPCs at Joeva (the retail capture, a Town)\n");
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
    // the counts of people by node level and by source (layout or building) in the data
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
    /// Set before start(): sees each NPC with the node it was made for.
    std::function<void(uint64_t, const content::NpcInstanceRow*, const content::NpcRouteRow*, const world::CityNodeInfo&, int64_t)> on_spawn;
// -- C | O | A & C Y B --

    Rig(const world::CityNodes& nodes, const world::NodeWire& wire, const spawns::AreaConfig& area, std::function<void(uint64_t, const content::NpcInstanceRow*, const content::NpcRouteRow*)> extra = {},
        std::shared_ptr<const content::PopulationData> population = nullptr, const spawns::NodePopulationData* data = nullptr) {
        spawns::NodePopulations::Hooks hooks;
        hooks.spawned = [this, extra](uint64_t id, const content::NpcInstanceRow* inst, const content::NpcRouteRow* route, const world::CityNodeInfo& node, int64_t plot) {
            spawned.push_back(id);
            if (extra) extra(id, inst, route);
            if (on_spawn) on_spawn(id, inst, route, node, plot);
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

/// How the people of the world made with the shipped seed live: what the walking world is given for each of them (the same call the world server makes), by node.
static void test_walkers() {
    std::printf("how the towns' people live (make_setup, the shipped seed's workshops)\n");
    struct Count { size_t total = 0, patrol = 0, wander = 0, stand = 0, guards_patrol = 0, service_walking = 0, patrol_unplaced = 0, stationary_routed = 0, villager_patrol = 0, carriers_walk = 0, workers_walk = 0, farmers_stand = 0,
                 services = 0, crate_vendors = 0, crate_vendors_walking = 0, building_wanderers = 0, workers_off_plot = 0, workers_far = 0; };
    std::map<int64_t, Count> by_node;
    Rig rig(*g_env.shipped_nodes, *g_env.shipped_wire, spawns::AreaConfig::riverlands());
    const npc::SpeedModel speeds;
    rig.on_spawn = [&](uint64_t id, const content::NpcInstanceRow* inst, const content::NpcRouteRow* route, const world::CityNodeInfo& node, int64_t plot) {
        const world::WorldActor* a = rig.registry.find(id);
        if (!a) return;
        npc::NpcMotionSetup s = npc::make_setup(*a, g_env.table, inst, route, speeds, nullptr, &node);
        if (plot != 0) npc::keep_to_workshop(&s, npc::kWorkshopRadiusCm);   // as the world server does
        const std::string leaf = spawns::blueprint_leaf(a->npc.archetype);
        Count& c = by_node[node.guid];
        ++c.total;
        // everyone who sells, serves or runs a station, found by the words of the Blueprint (wider than the rule) and by the data's vendor: none may move
        static const char* const kServiceWords[] = {"Vendor", "Clerk", "Manager", "_KAE", "Salesman", "Caravan", "Station", "Board", "ServiceBuilding", "TurnInBox", "Mailbox", "ServiceDesk"};
        bool service = inst && inst->vendor != 0;
        for (const char* w : kServiceWords) service = service || leaf.find(w) != std::string::npos;
        if (service) {
            ++c.services;
            if (s.ambient != npc::Ambient::Stationary) { ++c.service_walking; std::printf("    WALKS: %s at %s\n", leaf.c_str(), node.name.c_str()); }
        }
        if (leaf == "ServiceBuilding_Caravansary_CrateSalesman") { ++c.crate_vendors; c.crate_vendors_walking += s.ambient != npc::Ambient::Stationary; }
        if (plot != 0 && s.ambient == npc::Ambient::Wander) {
            // a workshop worker: its anchor (where it stands) lies at its own plot and its stroll is a few metres
            double ox = node.x, oy = node.y;
            bool found = false;
            const double yaw = node.yaw * 3.14159265358979323846 / 180.0;
            for (const world::PlotRow& r : g_env.shipped_wire->plot_rows_of(node.guid, g_env.shipped_nodes->level_of(node.guid))) {
                if (r.plot != plot) continue;
                ox = node.x + r.tx * std::cos(yaw) - r.ty * std::sin(yaw);
                oy = node.y + r.tx * std::sin(yaw) + r.ty * std::cos(yaw);
                found = true;
            }
            ++c.building_wanderers;
            if (!found || std::hypot(a->x - ox, a->y - oy) > 3000.0) ++c.workers_off_plot;
            if (s.wander_max > npc::kWorkshopRadiusCm) ++c.workers_far;
        }
        const bool patrol = s.ambient == npc::Ambient::Patrol && s.route.size() >= 2;
        c.patrol += patrol ? 1 : 0;
        c.wander += (s.ambient == npc::Ambient::Wander || (s.ambient == npc::Ambient::Patrol && !patrol)) ? 1 : 0;
        c.stand += s.ambient == npc::Ambient::Stationary ? 1 : 0;
        if (patrol && npc::is_guard_leaf(leaf)) ++c.guards_patrol;
        if (patrol && !npc::is_guard_leaf(leaf)) ++c.villager_patrol;
        if (patrol && leaf.find("Carrying") != std::string::npos) ++c.carriers_walk;
        const bool workshop = inst && inst->name.rfind("Node_Filler_Villager", 0) == 0;   // the instances a Factory's and a Farm's people live by
        if (workshop && s.ambient == npc::Ambient::Wander && leaf.find("Shovel") == std::string::npos) ++c.workers_walk;
        if (workshop && s.ambient == npc::Ambient::Stationary && leaf.find("Shovel") != std::string::npos) ++c.farmers_stand;
        if (npc::stands_by_kind(leaf) && s.ambient != npc::Ambient::Stationary) ++c.service_walking;
        if (s.ambient == npc::Ambient::Stationary && !s.route.empty()) ++c.stationary_routed;
        // a placed route is in the node's frame: its first stop is within a few hundred metres of the record
        if (patrol && std::hypot(s.route.front().x - node.x, s.route.front().y - node.y) > 60000.0) ++c.patrol_unplaced;
    };
    rig.pops->start(0.0);
    size_t service_walking = 0, stationary_routed = 0, unplaced = 0;
    for (const auto& [guid, c] : by_node) {
        const world::CityNodeInfo* n = g_env.shipped_nodes->by_guid(guid);
        std::printf("    %s (level %d): %zu NPCs: %zu walk a route (%zu guards, %zu carriers, %zu animals and others), %zu wander (%zu of them workshop workers), %zu stand (%zu farmers)\n",
                    n ? n->name.c_str() : "?", n ? g_env.shipped_nodes->level_of(guid) : -1, c.total, c.patrol, c.guards_patrol, c.carriers_walk, c.villager_patrol - c.carriers_walk, c.wander, c.workers_walk, c.stand,
                    c.farmers_stand);
        service_walking += c.service_walking;
        stationary_routed += c.stationary_routed;
        unplaced += c.patrol_unplaced;
    }
    const Count& joeva = by_node[kJoeva];
    const Count& halcyon = by_node[kHalcyon];
    CHECK(joeva.total == 624 && halcyon.total == 371, "Joeva (Town) has 603 NPCs and 21 crafting stations and Halcyon (Village) 350 and 21 with the workshops");
    CHECK(joeva.patrol > 0 && joeva.guards_patrol > 0 && joeva.wander > 0 && joeva.stand > 0, "Joeva has people that walk a route, guards among them, people that wander and people that stand");
    CHECK(joeva.workers_walk == 16 && joeva.farmers_stand == 2 && halcyon.workers_walk == 4, "the workshops' people: 16 workers walk and 2 farmers stand at Joeva, 4 workers walk at Halcyon");
    CHECK(joeva.carriers_walk == 0 && joeva.guards_patrol == 1 && halcyon.carriers_walk == 2 && halcyon.guards_patrol == 2, "Joeva's carriers stand (its guard walks); Halcyon's 2 carriers and 2 guards walk their routes");
    CHECK(service_walking == 0 && joeva.services > 0 && halcyon.services > 0, "no vendor, clerk, merchant stand or station of any node walks (Joeva's and Halcyon's, the building sets' included)");
    CHECK(joeva.crate_vendors > 0 && joeva.crate_vendors_walking == 0 && halcyon.crate_vendors_walking == 0, "the construction crate vendor (the caravansary's crate salesman) stands in Joeva and Halcyon");
    CHECK(joeva.building_wanderers == 16 && halcyon.building_wanderers == 4, "the workshops' wanderers are the 16 of Joeva and the 4 of Halcyon");
    CHECK(joeva.workers_off_plot == 0 && halcyon.workers_off_plot == 0, "every workshop worker stands within 30 m of its own plot (its anchor)");
    CHECK(joeva.workers_far == 0 && halcyon.workers_far == 0, "every workshop worker strolls at most 6 m from its anchor");
    CHECK(stationary_routed == 0, "nobody that stands has a route");
    CHECK(unplaced == 0, "every route that is walked starts within 600 m of its node's record (the node-local waypoints were placed)");
}

static void test_manager() {
    std::printf("the live manager: the people follow the nodes' levels\n");
    auto nodes = fresh_nodes();
    Rig rig(*nodes, *g_env.wire, spawns::AreaConfig::riverlands());
    const size_t made = rig.pops->start(0.0);
    size_t want = 0;
    for (const world::CityNodeInfo& n : nodes->nodes()) {
        if (const tests::NodePopCase* c = golden_of(n.name, nodes->level_of(n.guid))) want += c->spawned_layout + c->spawned_building;
    }
    CHECK(made == want && rig.pops->made() == want && people_of(rig.registry) == want && rig.spawned.size() == want, "start: every node inside the area has the oracle's number of NPCs at its seed level");
    CHECK(rig.pops->nodes() == 10 && rig.pops->start(0.0) == 0, "10 nodes inside the Riverlands; started once");
    CHECK(rig.npcs_of(kJoeva) == golden_npcs("Verra_RVR_Joeva", 4) && rig.npcs_of(kHalcyon) == golden_npcs("Verra_RVR_Halcyon", 3) && rig.npcs_of(kWinstead) == golden_npcs("Verra_RVR_Winstead", 0),
          "Joeva (Town), Halcyon (Village) and Winstead (Wilderness) have their level's people");
    bool tagged = true, in_range = true, grounding = true;
    size_t by_data = 0, by_terrain = 0;
    rig.registry.for_each([&](const world::WorldActor& a) {
        tagged = tagged && a.npc.spawn_ref >= spawns::kNodeRefBase && a.npc.source == world::Source::Data && (a.kind == world::ActorKind::Npc || a.kind == world::ActorKind::Station);
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
    CHECK(people_of(rig.registry) == want - golden_npcs("Verra_RVR_Joeva", 4) + golden_npcs("Verra_RVR_Joeva", 3), "the registry holds exactly the nodes' people (nothing leaked, nothing doubled)");

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
    const size_t npcs_without = people_of(rig.registry);
    const uint64_t back = rig.pops->respawn(ref_of[stay_id], 4.0);
    const world::WorldActor* b2 = rig.registry.find(back);
    CHECK(back != 0 && b2 && people_of(rig.registry) == npcs_without + 1 && b2->npc.asset_guid == stayer.npc.asset_guid && b2->npc.instance_guid == stayer.npc.instance_guid &&
              b2->npc.spawn_ref == ref_of[stay_id] && b2->x == stayer.x && b2->y == stayer.y && b2->npc.level == stayer.npc.level,
          "the NPC of a point that stays comes back from the node's selection: the same Blueprint, place, level and reference");
    CHECK(rig.pops->respawned() == 1 && rig.pops->entry_of(kJoeva)->npcs + rig.pops->entry_of(kJoeva)->stations == after.size(), "(counted; the node's count is whole again)");

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
    // quests slice 0: the Town Criers of the Town carry the conversation the data gives them
    constexpr int64_t kTownCrierRoot = 6064634349883577684ll;
    size_t criers = 0;
    for (const uint64_t id : rig.pops->actors_of(kJoeva)) criers += rig.registry.find(id)->npc.dialogue_root == kTownCrierRoot ? 1 : 0;
    CHECK(criers == 2, "(and its two Town Criers open the Town Crier's conversation)");
    CHECK(rig.pops->rebuilds() == 3, "three rebuilds in all");
    // two nodes change in one tick
    set_level(*nodes, kHalcyon, 4);
    set_level(*nodes, kWinstead, 3);
    CHECK(rig.pops->tick(8.0) == 2 && rig.npcs_of(kHalcyon) == golden_npcs("Verra_RVR_Halcyon", 4) && rig.npcs_of(kWinstead) == golden_npcs("Verra_RVR_Winstead", 3), "two nodes that changed together are both rebuilt");
    size_t halcyon_criers = 0;
    for (const uint64_t id : rig.pops->actors_of(kHalcyon)) halcyon_criers += rig.registry.find(id)->npc.dialogue_root == kTownCrierRoot ? 1 : 0;
    CHECK(halcyon_criers == 1, "(Halcyon's Town Crier opens the same conversation)");
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
        if ((a.kind != world::ActorKind::Npc && a.kind != world::ActorKind::Station) || it == data_z.end()) return;   // (the stations stand on the ground like the people)
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
        reg->for_each([&](const world::WorldActor& a) { w += (a.kind == world::ActorKind::Npc || a.kind == world::ActorKind::Station) && !nav->ready_at(a.x, a.y) ? 1 : 0; });
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

// ---- the town people's health ---------------------------------------------------------------------------------------------------------------

/// The EveryoneProxy stat items an actor holds (record -> value), the ones its open carries.
static std::map<int64_t, float> stat_items(const world::WorldActor& a) {
    std::map<int64_t, float> out;
    for (const repl::Subobject& sub : a.actor.subobjects) {
        if (!sub.obj.schema || std::string(sub.obj.schema->name) != "UAoCStatsComponent") continue;
        const auto* arr = const_cast<repl::FastArrays&>(sub.obj.arrays).get(*sub.obj.schema, "StatRepInt32EveryoneProxy");
        if (!arr) continue;
        for (const repl::FastArrayItem& it : arr->items()) {
            const auto v = it.values.values.find(1);
            const auto* s = v == it.values.values.end() ? nullptr : v->second.get_if<repl::StatInt32Inst>();
            if (s) out[s->record] = std::bit_cast<float>(s->value);
        }
    }
    return out;
}

static bool has_full_health(const std::map<int64_t, float>& items) {
    const auto h = items.find(content::kGuidHealth);
    const auto m = items.find(content::kGuidMaxHealth);
    return h != items.end() && m != items.end() && m->second > 0 && h->second == m->second;
}

/// The stat items a client reads in the open chain `d` of actor `a`: what the wire carries, not what the registry holds.
static std::map<int64_t, float> stat_items_in_chain(const fake::Delivered& d, const world::WorldActor& a) {
    std::map<int64_t, float> out;
    const repl::Subobject* comp = nullptr;
    for (const repl::Subobject& s : a.actor.subobjects) {
        if (s.obj.schema && std::string(s.obj.schema->name) == "UAoCStatsComponent") comp = &s;
    }
    if (!comp || d.close) return out;
    const size_t end = d.content.bit_pos();
    std::optional<std::vector<repl::BlockView>> blocks;
    for (size_t start = 0; start < (d.open ? 2000u : 1u) && start < end; ++start) {
        auto got = repl::read_content_blocks(d.content.data(), end, start);
        if (!got || got->empty() || !(*got)[0].header.is_actor) continue;
        if (d.open && got->size() != a.actor.subobjects.size() + 1) continue;
        blocks = std::move(got);
        break;
    }
    if (!blocks) return out;
    for (const repl::BlockView& b : *blocks) {
        if (b.header.is_actor || b.header.object.ObjectId != comp->guid.ObjectId) continue;
        const size_t block_end = b.payload_pos + b.payload_bits;
        size_t pos = b.payload_pos;
        if (b.header.has_rep_layout && !repl::read_rep_block(*comp->obj.schema, d.content.data(), block_end, pos, comp->obj.opts)) return out;
        if (pos >= block_end) return out;
        const auto fields = repl::read_fast_array_fields(*comp->obj.schema, d.content.data(), block_end, pos, comp->obj.opts);
        const auto* f = comp->obj.schema->field("StatRepInt32EveryoneProxy");
        if (!fields || !f) return out;
        for (const repl::FastArrayField& field : *fields) {
            if (field.field != f->index) continue;
            for (const auto& ch : field.delta.changed) {
                const auto v = ch.values.values.find(1);
                const auto* s = v == ch.values.values.end() ? nullptr : v->second.get_if<repl::StatInt32Inst>();
                if (s) out[s->record] = std::bit_cast<float>(s->value);
            }
        }
    }
    return out;
}

/// Every node NPC in the registry: its Health item exists, Health == MaxHealth > 0, and two people of one Blueprint and level hold the same number.
static void check_town_health(PlayerWorld& world, const char* when) {
    size_t total = 0, lacking = 0, bad = 0, wizards = 0, villagers = 0, unequal = 0;
    std::map<std::pair<std::string, int>, float> by_kind;
    world.registry().for_each([&](const world::WorldActor& a) {
        if (a.kind != world::ActorKind::Npc || a.npc.spawn_ref < spawns::kNodeRefBase) return;
        ++total;
        const std::string leaf = spawns::blueprint_leaf(a.npc.archetype);
        wizards += leaf == "Guard_Wizard" ? 1 : 0;
        villagers += leaf.rfind("Node_Villager", 0) == 0 ? 1 : 0;
        const auto items = stat_items(a);
        if (!items.count(content::kGuidHealth) || !items.count(content::kGuidMaxHealth)) {
            ++lacking;
            return;
        }
        if (!has_full_health(items)) ++bad;
        const auto key = std::make_pair(leaf, static_cast<int>(a.npc.level));
        const auto [it, fresh] = by_kind.emplace(key, items.at(content::kGuidMaxHealth));
        if (!fresh && it->second != items.at(content::kGuidMaxHealth)) ++unequal;
    });
    for (const auto& [kind, health] : by_kind) {
        if (kind.first == "Guard_Wizard" || kind.first == "Guard_Captain" || kind.first.rfind("Node_Villager", 0) == 0) std::printf("    %s L%d: Health %.1f\n", kind.first.c_str(), kind.second, health);
    }
    std::printf("    %s: %zu town people, %zu without a Health item, %zu with Health != MaxHealth or 0, %zu guard wizards, %zu villagers\n", when, total, lacking, bad, wizards, villagers);
    CHECK(total > 0 && wizards > 0 && villagers > 0, "(the town's people include guard wizards and villagers, two templates that have no stats of their own)");
    CHECK(lacking == 0, "after the node people start, no node NPC lacks a Health item");
    CHECK(bad == 0, "every node NPC has Health == MaxHealth > 0");
    CHECK(unequal == 0, "people of one Blueprint and level hold one and the same MaxHealth");
}

/// A fake client takes the awake ring around a guard wizard: the open it receives carries its Health, full.
static void check_open_carries_health(PlayerWorld& world) {
    const world::WorldActor* wiz = nullptr;
    world.registry().for_each([&](const world::WorldActor& a) {
        if (!wiz && a.kind == world::ActorKind::Npc && a.npc.spawn_ref >= spawns::kNodeRefBase && spawns::blueprint_leaf(a.npc.archetype) == "Guard_Wizard") wiz = &a;
    });
    CHECK(wiz != nullptr, "(a guard wizard stands in the town)");
    if (!wiz) return;
    const conn::OuterHeader outer{0x500c7696u, 0, 5};
    const conn::CustomField cf{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d};
    const Clock::time_point t0{std::chrono::seconds(5000)};
    Connection server{ConnectionConfig{outer, cf, 14265, 15870}, t0};
    fake::Client client{outer, cf, 14265, 15870};
    ActorView view(world.registry(), ActorView::Config{}, [](uint32_t i) { return i >= 3 && i <= 6; });
    std::vector<fake::Delivered> got;
    Clock::time_point t = t0;
    const uint64_t id = wiz->id;
    const auto pass = [&] {
        t += std::chrono::milliseconds(60);
        view.tick(server, wiz->x, wiz->y, t);
        for (const auto& pkt : server.flush(t)) {
            for (const auto& d : client.receive(pkt).delivered) got.push_back(d);
        }
        for (const auto& pkt : client.packets({})) server.receive(pkt, t);
        for (const AckEvent& e : server.take_ack_events()) view.on_ack(e);
    };
    uint32_t channel = 0;
    for (int i = 0; i < 400 && channel == 0; ++i) {
        pass();
        channel = view.channel_of(id);
    }
    for (int i = 0; i < 20; ++i) pass();   // (the open's parts arrive over a few passes)
    CHECK(channel != 0, "the view opens the guard wizard for a client standing at it");
    std::map<int64_t, float> seen;
    for (const fake::Delivered& d : got) {
        if (d.channel != channel || !d.open) continue;
        seen = stat_items_in_chain(d, *wiz);
        break;
    }
    std::printf("    the open of the guard wizard on channel %u carries %zu stat items\n", channel, seen.size());
    CHECK(has_full_health(seen), "the open a fake client receives for a guard wizard carries Health == MaxHealth > 0");
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
    const size_t before_npcs = people_of(world.registry());
    const size_t made = world.start_node_people(cfg);
    CHECK(made == circle_npcs(4) && made > 400 && world.node_people() && people_of(world.registry()) == before_npcs + made,
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
    check_town_health(world, "after start_node_people");
    check_open_carries_health(world);
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
    CHECK(e && e->level == 3 && e->npcs + e->stations == circle_npcs(3) && people_of(world.registry()) == before_npcs + e->npcs + e->stations,
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
    CHECK(back && has_full_health(stat_items(*back)), "...and the NPC that came back has its Health, full");
    check_town_health(world, "after the level change and the respawn");
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
// CoA captures, C.Y.B code

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
    auto too_large = plain;
    too_large.xy[0] = 1e300;
    CHECK(!bad.add(too_large), "finite coordinates outside float storage are refused before indexing");
    too_large = plain;
    too_large.half_width_cm = 1e300;
    CHECK(!bad.add(too_large), "finite widths outside float storage are refused before indexing");
    CHECK(!bad.add(by_level_road(1, {1, 1, 1, 1, 1, 1, 1}, {500, 500, 1e300, 500, 500, 500, 500}, 0, 0, 1, 1)),
          "a future active level's width must also fit float storage");
    CHECK(bad.roads() == 0, "(none was added)");
    world::RoadMap long_road;
    CHECK(long_road.add(by_level_road(11, {1, 1, 2, 3, 4, 4, 4}, {500, 500, 500, 500, 500, 500, 500}, 0, 0, 1e30, 0)),
          "a long road that fits float storage is accepted without indexing its enormous box");
    CHECK(long_road.tier_at(1e25, 0) == 1 && long_road.tier_at(1e25, 501) == -1,
          "a road outside the integer grid is queried directly with its proper width");
    CHECK(long_road.apply_levels([](int64_t) { return 4; }, 0) == 1 && long_road.tier_at(1e25, 0) == 4,
          "an exceptionally long road still follows its node's level");
    auto local = plain;
    local.xy = {0, 0, 10000, 0};
    CHECK(long_road.add(local) && long_road.tier_at(5000, 0) == 4,
          "indexed and exceptional segments are queried together, keeping the highest tier");
    CHECK(long_road.apply_levels(nullptr, 0) == 1 && long_road.tier_at(5000, 0) == 3 && long_road.tier_at(1e25, 0) == 1,
          "an indexed road can provide the winning tier when the exceptional road's node level falls");
    world::RoadMap many_cells;
    auto broad_box = local;
    broad_box.xy = {0, 0, 1e8, 0};
    CHECK(many_cells.add(broad_box) && many_cells.tier_at(1e7, 0) == 3,
          "a segment covering more than 4096 valid grid cells is queried without filling them all");
    world::RoadMap wide_road;
    auto wide = local;
    wide.half_width_cm = 1e20;
    CHECK(wide_road.add(wide) && wide_road.tier_at(0, 1e19) == 3,
          "a wide road that fits float storage is accepted and queried without allocating billions of cells");
    world::RoadMap inactive_width;
    CHECK(inactive_width.add(by_level_road(1, {-1, 1, 1, 1, 1, 1, 1}, {1e300, 500, 500, 500, 500, 500, 500}, 0, 0, 10000, 0)) &&
              inactive_width.bounds().max_y == 500 && inactive_width.tier_at(5000, 0) == -1 &&
              inactive_width.apply_levels(nullptr, 1) == 1 && inactive_width.tier_at(5000, 0) == 1,
          "widths of inactive levels do not enlarge the index, while later active levels still work");
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
    for (const char* line : {
             "road\t3\t500\tnot-a-number,0\t10000,0\n",
             "road\t3\t500\t0,0\t10000,0junk\n",
             "road\t4294967296\t500\t0,0\t10000,0\n",
             "roadn\t-1\t1,1,2,3,4,4,4\t500,500,500,500,600,900,900\t0,0\t10000,0\n",
             "roadn\t11\t1.5,1,2,3,4,4,4\t500,500,500,500,600,900,900\t0,0\t10000,0\n",
             "roadn\t11\tnan,1,2,3,4,4,4\t500,500,500,500,600,900,900\t0,0\t10000,0\n"}) {
        {
            std::ofstream f(file);
            f << line;
        }
        world::RoadMap malformed;
        CHECK(!malformed.load(file.string(), error) && !error.empty(), "malformed road coordinates, tiers and zones are refused without numeric coercion");
    }
    CHECK(fm.tier_at(1e300, 1e300) == -1 && fm.tier_at(-1e300, -1e300) == -1, "queries far outside the road bounds avoid out-of-range cell conversions");
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
        if (a.kind != world::ActorKind::Npc || a.npc.spawn_ref < spawns::kNodeRefBase) return;   // (the floor is worked out from the people; the stations stand on it)
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

    // the towns' check: one [towns] line once the people stand and their numbers have been still, none while they change, one more for the new level after a level change
    auto count_lines = [](const std::string& text) {
        size_t n = 0;
        for (size_t at = text.find("[towns]"); at != std::string::npos; at = text.find("[towns]", at + 1)) ++n;
        return n;
    };
    std::ostringstream captured;
    auto logger = std::make_shared<spdlog::logger>("towns_check", std::make_shared<spdlog::sinks::ostream_sink_mt>(captured));
    logger->set_level(spdlog::level::info);
    const auto previous = spdlog::default_logger();
    spdlog::set_default_logger(logger);
    world.tick_world(100.0);   // the people were noted at the tick above (95 s ago) and have not changed: the line
    world.tick_world(105.0);   // nothing changed: no second line
    world.tick_world(110.0);
    const std::string first = captured.str();
    set_level(synth, kJoeva, 4);
    world.tick_world(120.0);   // the people of the new level are made and noted
    world.tick_world(125.0);   // still: the line of the new level, though the last one is not a minute old
    const std::string second = captured.str();
    spdlog::set_default_logger(previous);
    CHECK(count_lines(first) == 1 && first.find("Joeva (Wilderness)") != std::string::npos, "the towns' check makes one line for the town (Wilderness) once its numbers are still, and not another while they stay so");
    CHECK(count_lines(second) == 2 && second.find("Joeva (Town)") != std::string::npos, "after a level change: one more line, for the new level, as soon as its people are on the ground");
    CHECK(first.find(" people, ") != std::string::npos && first.find("on a floor within 30 cm of where the data puts them") != std::string::npos && first.find("would walk if they had ground") != std::string::npos,
          "the line counts the people on a floor, on a floor farther away, without a floor, with no walkable ground near, and those that would walk");
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
            // a position saved under Joeva's town (a rider's last good position was the baked ground + 98)
            const double ux = -1079493.1, uy = 508464.4, uz = 7748.6, ut = terrain(ux, uy);
            const auto user = tf.at(ux, uy, ut);
            std::snprintf(msg, sizeof(msg), "Joeva: the user's saved spot (z %.1f, baked ground %.1f) is in the bowl: the town's floor %.0f, %.0f m over it", uz, ut, user ? ut + user->lift : 0.0,
                          user ? (ut + user->lift - uz) / 100.0 : 0.0);
            CHECK(user && ut + user->lift + 98.0 - uz > 4000.0, msg);
            std::printf("    %s\n", msg);
            // The login fix: where a login in Joeva lands.  The client's pawn phases in when a trace of 1,000 cm straight down from its origin finds ground, so
            // the join z must be within [floor + 98, floor + 1,000] of the town's floor.  PlayerWorld::lift_out_of_bowl puts a character saved on the baked ground under the town at the
            // town's floor + the capsule's half height (98) + kBowlSlackCm (200), and a GM teleport to a point without a z lands at the same height.
            struct JoinSpot { const char* what; double x, y; };
            const JoinSpot join_spots[] = {{"the node's place (worldctl tp <character> joeva)", info->x, info->y},
                                           {"Joeva's market square (the Market_v3 plot in the node record's frame)", -1087417.6, 503591.5},
                                           {"a spot saved on the baked ground under the town", ux, uy}};
            for (const JoinSpot& sp : join_spots) {
                const double ground = terrain(sp.x, sp.y);
                const auto floor_here = tf.at(sp.x, sp.y, ground);
                if (!floor_here) {
                    CHECK(false, sp.what);
                    continue;
                }
                const double model_floor = ground + floor_here->lift;
                const bool lifted = model_floor + PlayerWorld::kCapsuleHalfHeightCm - (ground + 98.0) >= world::TownFloor::kBowlCm;   // (saved on the baked ground, as the pit's characters are)
                const double join_z = model_floor + PlayerWorld::kCapsuleHalfHeightCm + PlayerWorld::kBowlSlackCm;
                std::snprintf(msg, sizeof(msg), "Joeva, %s (%.0f, %.0f): baked ground %.1f, the model's floor %.1f; joined from the pit at z %.1f = floor + %.0f cm (the trace needs 98 to 1,000)", sp.what,
                              sp.x, sp.y, ground, model_floor, join_z, join_z - model_floor);
                CHECK(lifted && join_z - model_floor >= 98.0 && join_z - model_floor <= 1000.0, msg);
                std::printf("    %s\n", msg);
            }
            // retail's 9 anchors: the model's height over the terrain against the height retail's NPC stood at
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
// ---- the two seed towns with their buildings on the real walking map ----------------------------------------------------------------------------

/// Whether a step of a walker (its origin, 96 cm over its feet or so) stays on the walkable ground: a ray along the map from feet to feet; or, when the step runs along the edge of the walkable ground (a path
/// that hugs a corner is on the edge, where a float puts the ray a hair outside), its middle is on the map within the agent's radius.  A step through a wall has its middle in the wall.
static bool step_on_ground(const nav::AreaNav& map, const nav::Vec3& a, const nav::Vec3& b) {
    const nav::Vec3 fa{a.x, a.y, a.z - 96.0}, fb{b.x, b.y, b.z - 96.0};
    if (map.raycast(fa, fb)) return true;
    return map.on_navmesh({(fa.x + fb.x) / 2, (fa.y + fb.y) / 2, (fa.z + fb.z) / 2}, 40.0, 150.0);
}

/// A file of dist/content next to content.db.
static std::string content_file(const char* name) { return (std::filesystem::path(db_path()).parent_path() / name).string(); }

struct RealTown {
    int64_t     node;
    int         level;
    const char* routes[3];   // routes of the town (names): the guard's round and two walks of the town's other people
    size_t      snapped_far; // creatures the data draws more than 3 m from the street that come to it, at least (0: none is moved that far)
};

/// Opt-in (AOC_REAL_GROUND=1: it opens dist/content/area_nav.bin and loads the chunks round the town): a town at its seed level with its people on the real walking map, built with the buildings
/// of that level.  What it checks: nobody walks through a wall, the people who stand on raised floors stand on them, the guard's round is on the ground, a mount is summoned where there is
/// room and refused over a drop, and an animal the data draws far from the street stands on it.
static void test_real_town_buildings() {
    const char* env = std::getenv("AOC_REAL_GROUND");
    if (!env || !*env) {
        std::printf("the seed towns with their buildings on the real map: opt-in (AOC_REAL_GROUND=1 reads dist/content/area_nav.bin)\n");
        return;
    }
    const RealTown towns[] = {{kJoeva, 4, {"Patrol_Node_Flat_D_L4_Middle_Guard_02", "Node_Flat_D_L4_Upper_Filler_Scholar_01", "Node_Flat_D_L4_Upper_Filler_Carry_01"}, 9},
                              {kHalcyon, 3, {"Node_River_C_L3_GuardMidPatrol", "Node_River_C_Upper_L3_Filler_Carry_01", "Node_River_C_Upper_L3_Filler_Guard_02"}, 0}};
    for (const RealTown& rt : towns) {
        std::string error;
        const world::CityNodeInfo* real = g_env.nodes->by_guid(rt.node);
        if (!real) {
            CHECK(false, "the town is in the registry");
            continue;
        }
        std::printf("%s at level %d on the real walking map (%s)\n", real->display_name.c_str(), rt.level, content_file("area_nav.bin").c_str());
        world::CityNodeInfo info = *real;
        std::vector<world::CityZoneInfo> zones{*g_env.nodes->zone_by_guid(info.zoi_guid)};
        world::CityNodes synth({info}, zones);
        set_level(synth, rt.node, rt.level);
        auto wire = world::NodeWire::load(db_path(), synth, error);
        auto nav = nav::AreaNav::load(content_file("area_nav.bin"), &error);
        if (!wire || !nav) {
            CHECK(false, "the node wire and the walking map load");
            std::printf("    %s\n", error.c_str());
            continue;
        }
        aoc::protocol::ServerGuidAllocator guids{60, 0x6ee87ee4u};
        PlayerWorld world(&*g_env.db, "", bs::GameStateMode::Tier3, 60, guids);
        world.set_spawn_context(g_env.base);
        if (!world.ready() || !world.load_blueprints(motion_path(), error)) {
            CHECK(false, "the world is ready and the Blueprint table loads");
            continue;
        }
        world.set_city_nodes(&synth);
        world.set_node_wire(wire.get());
        world.set_node_population_data(g_env.data.get());
        if (!world.load_town_platforms(content_file("town_platforms.bin"), error)) {
            CHECK(false, "the platforms load");
            std::printf("    %s\n", error.c_str());
            continue;
        }
        spawns::AreaConfig circle;
        circle.center_x = info.x;
        circle.center_y = info.y;
        circle.radius_cm = 90000.0;
        if (!world.load_area(circle, error) || !world.adopt_ground(std::move(nav), error)) {
            CHECK(false, "the area loads and the real ground is adopted");
            std::printf("    %s\n", error.c_str());
            continue;
        }
        const size_t made = world.start_node_people({});
        CHECK(made == golden_npcs(real->name, rt.level), "the town's people are made");
        const std::vector<uint64_t> ids = world.node_people()->actors_of(rt.node);
        npc::NpcWorld& npcs = *world.npcs();
        const nav::AreaNav& map = *world.nav();

        // how they stand: the report the log line makes
        const npc::NpcWorld::StandReport rep = npcs.stand_report(ids);
        std::printf("    %zu people: %zu on a floor within %.0f cm of where the data puts them, %zu on a floor up to %.0f cm from it, %zu with none (%zu of them with no walkable ground within %.0f cm; %zu would walk), %zu waiting\n",
                    rep.people, rep.on_floor, npc::NpcWorld::kStandNearCm, rep.moved, rep.max_moved_cm, rep.no_floor, rep.blocked, npc::NpcWorld::kBlockedCm, rep.stuck, rep.waiting);
        CHECK(rep.people == made && rep.waiting == 0, "all of them have their ground (the area's chunks are resident)");
        {
            // who has no floor and would walk (a person the data gives a life, left standing for want of ground)
            std::map<std::string, size_t> stuck_by_kind;
            for (const uint64_t id : ids) {
                const npc::NpcWorld::StandReport one = npcs.stand_report({id});
                if (one.stuck == 0) continue;
                const world::WorldActor* a = world.registry().find(id);
                ++stuck_by_kind[a ? spawns::blueprint_leaf(a->npc.archetype) : std::string("?")];
            }
            std::string list;
            for (const auto& [kind, n] : stuck_by_kind) list += (list.empty() ? "" : ", ") + kind + " x" + std::to_string(n);
            std::printf("    no floor, would walk: %s\n", list.empty() ? "none" : list.c_str());
        }
        CHECK(rep.on_floor * 100 >= rep.people * 60, "at least six in ten stand on a floor within 30 cm of where the data puts them");
        CHECK(rep.no_floor * 100 <= rep.people * 8, "at most 8 in 100 have no floor in reach");

        // the creatures the data says to snap to the ground (NpcInfo::snaps_to_ground): none is without a floor, the ones the data draws far from the street come to it, and a person is never one
        {
            size_t snapping = 0, flying = 0, without_floor = 0, persons = 0, far_moved = 0, far_moved_at_street = 0;
            for (const uint64_t id : ids) {
                const world::WorldActor* a = world.registry().find(id);
                if (!a || !a->npc.snaps_to_ground) continue;
                ++snapping;
                flying += a->npc.flies ? 1 : 0;
                persons += npc::is_beast_archetype(a->npc.archetype) ? 0 : 1;
                const npc::NpcWorld::StandReport one = npcs.stand_report({id});
                without_floor += one.no_floor;
                if (one.max_moved_cm <= 300.0) continue;
                // it was moved more than 3 m: the data drew it over the street or under it
                const double foot = g_env.table.get(a->npc.archetype).half_height * npc::actor_scale_z(*a);
                const auto street_floor = world.bowl_floor_at(a->npc.home_x, a->npc.home_y);
                const double was = street_floor ? a->npc.home_z - foot - street_floor->z : 0.0, now = street_floor ? a->z - foot - street_floor->z : 0.0;
                ++far_moved;
                far_moved_at_street += street_floor && std::fabs(now) <= 250.0 ? 1 : 0;
                std::printf("      %s (%s): the data puts its feet at %+.0f cm to the street, it stands at %+.0f cm\n", spawns::blueprint_leaf(a->npc.archetype).c_str(), a->npc.flies ? "flies" : "walks", was, now);
            }
            std::printf("    %zu creatures the data says to snap to the ground (%zu of them fly): %zu without a floor, %zu moved more than 3 m, %zu of those now within 2.5 m of the street\n", snapping, flying, without_floor,
                        far_moved, far_moved_at_street);
            CHECK(snapping > 0 && persons == 0, "creatures of the town snap to the ground, and no person is one");
            CHECK(without_floor == 0, "every one of them has a floor of the walking map under it");
            char msg[200];
            std::snprintf(msg, sizeof(msg), "%s: the creatures the data draws more than 3 m from the street come to it (%zu, %s %zu), each within 2.5 m of it", real->display_name.c_str(), far_moved, rt.snapped_far ? "at least" : "exactly",
                          rt.snapped_far);
            CHECK(far_moved_at_street == far_moved && (rt.snapped_far ? far_moved >= rt.snapped_far : far_moved == 0), msg);
        }

        // the raised people (feet over 3 m above the platform) and the others
        size_t raised = 0, raised_on_floor = 0, street = 0, street_on_floor = 0;
        std::vector<std::array<double, 3>> raised_spots, street_spots;   // x, y, feet
        for (const uint64_t id : ids) {
            const world::WorldActor* a = world.registry().find(id);
            if (!a) continue;
            const double feet = a->npc.home_z - 88.0;
            const auto plat = world.bowl_floor_at(a->npc.home_x, a->npc.home_y);
            if (!plat) continue;
            const npc::NpcWorld::StandReport one = npcs.stand_report({id});
            if (feet - plat->z > 300.0) {
                ++raised;
                raised_on_floor += one.on_floor;
                raised_spots.push_back({a->npc.home_x, a->npc.home_y, feet});
            } else if (feet - plat->z > -100.0) {
                ++street;
                street_on_floor += one.on_floor;
                street_spots.push_back({a->npc.home_x, a->npc.home_y, feet});
            }
        }
        std::printf("    people over 3 m above the platform: %zu, of them on a floor within 30 cm: %zu (the 2a map had none); at the street: %zu, on a floor within 30 cm: %zu\n", raised, raised_on_floor,
                    street, street_on_floor);
        CHECK(raised > 0 && raised_on_floor * 100 >= raised * 55, "more than half of the people on terraces and towers stand on a floor of the map");
        // the same people on an earlier walking map (AOC_NAV_BEFORE: the file the town had before its buildings were in it), for the before and after
        if (const char* before_path = std::getenv("AOC_NAV_BEFORE"); before_path && *before_path) {
            std::string before_error;
            auto before = nav::AreaNav::load(before_path, &before_error);
            if (before && before->load_around(info.x, info.y, 2)) {
                size_t before_on_floor = 0;
                for (const auto& s : raised_spots) {
                    double gz = 0;
                    if (before->ground_height(s[0], s[1], s[2], &gz, npc::NpcWorldConfig{}.data_ground_below_cm, npc::NpcWorldConfig{}.data_ground_above_cm) && std::fabs(gz - s[2]) <= npc::NpcWorld::kStandNearCm) ++before_on_floor;
                }
                std::printf("    the same %zu raised people on the walking map of %s: %zu on a floor within 30 cm (now %zu)\n", raised_spots.size(), before_path, before_on_floor, raised_on_floor);
            } else {
                std::printf("    (AOC_NAV_BEFORE %s does not load: %s)\n", before_path, before_error.c_str());
            }
        }

        // the routes: the ground along each, and a path between the waypoints
        for (const char* name : rt.routes) {
            const content::NpcRouteRow* route = nullptr;
            for (const auto& [guid, r] : g_env.pop->routes) {
                if (r.name == name) route = &r;
            }
            if (!route || route->waypoints.size() < 2) {
                CHECK(false, name);
                continue;
            }
            std::vector<nav::Vec3> pts;
            for (const auto& w : route->waypoints) {
                nav::Vec3 p;
                npc::place_node_route(info, w[0], w[1], w[2], &p.x, &p.y, &p.z);
                pts.push_back(p);
            }
            if (route->looping) pts.push_back(pts.front());
            size_t samples = 0, covered = 0, legs = 0, legs_found = 0;
            double worst_ratio = 0;
            for (size_t i = 0; i + 1 < pts.size(); ++i) {
                const double len = std::hypot(pts[i + 1].x - pts[i].x, pts[i + 1].y - pts[i].y);
                const int k = std::max(1, static_cast<int>(len / 50.0));
                for (int s = 0; s <= k; ++s) {
                    const double t = static_cast<double>(s) / k;
                    double gz = 0;
                    ++samples;
                    if (map.ground_height(pts[i].x + (pts[i + 1].x - pts[i].x) * t, pts[i].y + (pts[i + 1].y - pts[i].y) * t, pts[i].z + (pts[i + 1].z - pts[i].z) * t, &gz, 100.0, 300.0)) ++covered;
                }
                ++legs;
                const nav::PathResult path = map.find_path(pts[i], pts[i + 1], 150.0, 300.0, 60000);
                if (path.complete) {
                    ++legs_found;
                    if (len > 300.0) worst_ratio = std::max(worst_ratio, path.length / len);
                }
            }
            std::printf("    route %s: %zu waypoints, ground within -1..+3 m of its height on %.0f %% of %zu samples; a path between the waypoints for %zu of %zu legs (the longest is %.2f times the straight line)\n", name,
                        route->waypoints.size(), 100.0 * static_cast<double>(covered) / static_cast<double>(samples), samples, legs_found, legs, worst_ratio);
        }

        // a summon at the places people stand: the answer of the mount's ground rule (the point 575 cm ahead; a floor from 1.6 m over the feet to 10 m under them, within 2.5 m of the feet)
        const auto summon_answers = [&](const std::vector<std::array<double, 3>>& spots, size_t* allowed, size_t* total, size_t* spots_with_none) {
            *allowed = *total = *spots_with_none = 0;
            for (const auto& s : spots) {
                size_t ok_here = 0;
                for (int k = 0; k < 8; ++k) {
                    const double a = k * 3.14159265358979323846 / 4.0;
                    const auto g = world.floor_under(s[0] + 575.0 * std::cos(a), s[1] + 575.0 * std::sin(a), s[2], 1000.0, 160.0);
                    ++*total;
                    if (g && std::fabs(*g - s[2]) <= 250.0) { ++*allowed; ++ok_here; }
                }
                *spots_with_none += ok_here == 0 ? 1 : 0;
            }
        };
        size_t allowed = 0, total = 0, none = 0;
        summon_answers(street_spots, &allowed, &total, &none);
        std::printf("    a summon at %zu street places, 8 headings each: allowed %zu of %zu, no heading allowed at %zu places\n", street_spots.size(), allowed, total, none);
        CHECK(total > 0 && allowed * 100 >= total * 90 && none * 100 <= street_spots.size() * 3, "on the street a summon is allowed in nine headings in ten (not facing a drop)");
        std::vector<std::array<double, 3>> terraces, towers;
        for (const auto& s : raised_spots) {
            const auto plat = world.bowl_floor_at(s[0], s[1]);
            (s[2] - plat->z > 1500.0 ? towers : terraces).push_back(s);
        }
        summon_answers(terraces, &allowed, &total, &none);
        std::printf("    at %zu places 3 to 15 m up (terraces, upper floors): allowed %zu of %zu, no heading allowed at %zu places\n", terraces.size(), allowed, total, none);
        summon_answers(towers, &allowed, &total, &none);
        std::printf("    at %zu places over 15 m up (towers, high walls): allowed %zu of %zu, no heading allowed at %zu places\n", towers.size(), allowed, total, none);

        // the walkers: two minutes with a player in the town's square; a ray along every step stays on the walkable ground and nobody ends up in a wall
        struct Track { nav::Vec3 last; bool have = false; };
        std::map<uint64_t, Track> tracks;
        std::vector<uint64_t> walkers;
        world.registry().for_each([&](const world::WorldActor& a) {
            if (a.kind == world::ActorKind::Npc && std::hypot(a.x - info.x, a.y - info.y) < 14000.0 && !npcs.stands(a.id)) walkers.push_back(a.id);
        });
        size_t rays = 0, off_ground = 0, walking_at_all = 0;
        std::set<uint64_t> moved;
        double t = world.nav()->windowed() ? 1.0 : 0.0;
        for (int tick = 0; tick < 15 * 120; ++tick) {
            t += 1.0 / 15.0;
            world.note_player(7, info.x, info.y);
            world.tick_world(t);
            for (const uint64_t id : walkers) {
                const world::WorldActor* a = world.registry().find(id);
                if (!a) continue;
                Track& tr = tracks[id];
                const nav::Vec3 p{a->x, a->y, a->z};
                if (tr.have && std::hypot(p.x - tr.last.x, p.y - tr.last.y) > 0.5) {
                    ++rays;
                    moved.insert(id);
                    if (!step_on_ground(map, tr.last, p)) ++off_ground;
                }
                tr.last = p;
                tr.have = true;
            }
        }
        walking_at_all = moved.size();
        std::printf("    %zu walkers (people and animals) near the square; in 2 minutes %zu of them walked, %zu steps, %zu of them not along the walkable ground\n", walkers.size(), walking_at_all, rays, off_ground);
        CHECK(walking_at_all > 0 && rays > 100, "people and animals walk");
        CHECK(off_ground * 1000 <= rays, "no more than one step in a thousand leaves the walkable ground (a ray along it stays on the map)");
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
    g_env.shipped_nodes = fresh_nodes(true);
    g_env.shipped_wire = world::NodeWire::load(db_path(), *g_env.shipped_nodes, error);
    if (!g_env.shipped_wire) {
        std::printf("  [FAIL] the node wire of the shipped seed does not load: %s\n", error.c_str());
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
    test_workshops();
    test_walkers();
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
    test_real_town_buildings();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
