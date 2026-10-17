// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_city_nodes.cpp  (Nodes 1, stage 1)
//
//    the ZOI lookup   world/zoi_surface.h against the generated table: the login spot is Ruins of Aela, a point outside the table or not a number is no zone
//    the registry     world/city_nodes.h on the REAL content.db (contentc version 12): 81 nodes, 78 zones, the 7 ranks the captures show, names the way the
//                     service resolves them, every node that grows lies in its own zone, a point's node and biome
//    the live state   a snapshot replaces every level, a change moves one, the seed file's levels, a bad seed
//    the node reads   world::node_host_fn against the shared corpus's `nodehost` blocks (services/internal/statprog NodeWorld writes them): the same answers
//    the feed         svc/city_node_feed.h against an in-process fake of the citynode service: the token, the snapshot, the changes in order, a gap that
//                     calls again, an Aborted stream, an unreachable service (ONE warning, the seed's levels stay), and stop() while a stream is open
//  Without a content.db the registry part says so and is skipped.
// ============================================================================
#include "aoc/citynode/v1/citynode.grpc.pb.h"
#include "svc/city_node_feed.h"
#include "svc/service_token.h"
#include "stats/stat_program.h"
#include "world/city_nodes.h"
#include "spawns/region_data.h"
#include "world/zoi_surface.h"

#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <climits>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef NODES_CONTENT_DB_PATH
#define NODES_CONTENT_DB_PATH "dist/content/content.db"
#endif
#ifndef NODES_SEED_PATH
#define NODES_SEED_PATH "deploy/citynode_seed.json"
#endif
#ifndef STATS_CORPUS_PATH
#define STATS_CORPUS_PATH "server/worldd/stats/testdata/corpus.txt"
#endif

using namespace aoc::worldd;
namespace nodev1 = aoc::citynode::v1;
namespace fs = std::filesystem;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)
#define CHECKF(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { std::printf("  [FAIL] "); std::printf(__VA_ARGS__); std::printf("\n"); g_fail++; } \
} while (0)
// (C:Y:B ~ CoA)

namespace {

constexpr int64_t kRuinsOfAela = 6064629127135166465ll;   // ZOI Verra_RVR_StartingArea_RuinsOfAela
constexpr int64_t kJoevaZoi = 6064629126905528345ll;
constexpr int kRiverlands = 14, kTropics = 18, kDesert = 6;   // EBiomeType

std::string db_path() {
    if (const char* env = std::getenv("AOC_CONTENT_DB")) return env;
    return NODES_CONTENT_DB_PATH;
}

// ---- the zone lookup --------------------------------------------------------------------------------------------------------------------------

void test_zoi_surface() {
    std::printf("the zone of influence lookup\n");
    const world::ZoiSurfaceInfo info = world::zoi_surface_info();
    const std::vector<uint64_t> palette = world::zoi_surface_palette();
    CHECKF(info.chunks == 4635 && info.uniform == 3240, "the table has %d chunk columns (%d one owner), want 4635 and 3240", info.chunks, info.uniform);
    CHECKF(palette.size() == 58 && palette[0] == 0, "%zu owners, 0 (no ZOI) first", palette.size());
    CHECK(std::is_sorted(palette.begin(), palette.end()), "the owners are ascending");
    CHECK(world::zoi_at(-777664.5, 616578.7) == static_cast<uint64_t>(kRuinsOfAela), "the login spot is in Ruins of Aela");
    CHECK(world::zoi_at(-1094396.0, 521336.0) == static_cast<uint64_t>(kJoevaZoi), "Joeva's location is in Joeva's zone");
    CHECK(world::zoi_at(1.0e8, 1.0e8) == 0 && world::zoi_at(-1.0e8, 5.0) == 0, "far outside the table: no zone");
    CHECK(world::zoi_at(std::nan(""), 5.0) == 0 && world::zoi_at(5.0, std::numeric_limits<double>::infinity()) == 0, "not a number or infinite: no zone");
    // every owner of the table is found by some cell: walk the Riverlands' box at the chunk step and see the five node zones
    std::set<uint64_t> seen;
    for (double x = -1354752; x < -284672; x += 4096) {
        for (double y = 169984; y < 921088; y += 4096) seen.insert(world::zoi_at(x, y));
    }
    CHECKF(seen.count(6064629126900678679ull) && seen.count(6064629126902120472ull) && seen.count(6064629126905528345ull) &&
               seen.count(6064629126906904602ull) && seen.count(6064629126805651478ull),
           "the Riverlands' box holds Winstead, Halcyon, Joeva, New Aela and Miraleth (%zu owners seen)", seen.size());
    // a cell is 512 cm: two points of one cell give one owner
    CHECK(world::zoi_at(-777664.5, 616578.7) == world::zoi_at(-777664.5 + 1, 616578.7 + 1), "two points a centimetre apart are in one zone");
}

// The two ZOI tables stay side by side for now (a later step unifies them): G1's spawns/region_data.h (the Riverlands only, what spawning uses) and
// world/zoi_surface_table.h (the whole world, what the node lookups use).  They must agree on every Riverlands ZOI, cell for cell.
void test_zoi_tables_agree() {
    std::printf("the two ZOI tables agree on the Riverlands\n");
    namespace region = spawns::region;
    const double cell = region::kCellCm;
    std::set<uint64_t> guids;
    uint64_t rows_cells = 0, wrong_cells = 0, rect_cells = 0;
    int bad_zones = 0;
    int32_t x_min = INT32_MAX, y_min = INT32_MAX, x_max = INT32_MIN, y_max = INT32_MIN;
    for (size_t i = 0; i < region::kZoiCount; ++i) {
        const region::ZoiRows& z = region::kZois[i];
        guids.insert(z.guid);
        uint64_t cells = 0, wrong = 0;
        for (uint32_t r = z.first; r < z.first + z.count; ++r) {
            const region::Rect& rc = region::kZoiRects[r];
            x_min = std::min(x_min, rc.x0); y_min = std::min(y_min, rc.y0); x_max = std::max(x_max, rc.x1); y_max = std::max(y_max, rc.y1);
            for (int32_t y = rc.y0; y < rc.y1; y += region::kCellCm) {
                for (int32_t x = rc.x0; x < rc.x1; x += region::kCellCm) {
                    ++cells;
                    if (world::zoi_at(x + cell / 2, y + cell / 2) != z.guid) ++wrong;
                }
            }
        }
        if (cells != z.cells || wrong != 0) {
            ++bad_zones;
            std::printf("  [zone] %s: %llu cells counted, the row says %u; the surface table disagrees on %llu\n", z.name, static_cast<unsigned long long>(cells), z.cells,
                        static_cast<unsigned long long>(wrong));
        }
        rows_cells += cells;
        wrong_cells += wrong;
    }
    CHECKF(region::kZoiCount == 12 && guids.size() == 12, "G1's table has %zu zones, want 12", region::kZoiCount);
    CHECKF(bad_zones == 0 && wrong_cells == 0, "%d zones differ (%llu cells)", bad_zones, static_cast<unsigned long long>(wrong_cells));
    CHECKF(rows_cells == region::kRiverlandsAreaCells, "the zones' cells add up to %llu, the Riverlands has %u", static_cast<unsigned long long>(rows_cells), region::kRiverlandsAreaCells);
    // the other way: inside the Riverlands' box, every cell the surface table gives to one of the 12 zones is a cell of G1's table (no extra cell)
    for (int32_t y = y_min; y < y_max; y += region::kCellCm) {
        for (int32_t x = x_min; x < x_max; x += region::kCellCm) {
            if (guids.count(world::zoi_at(x + cell / 2, y + cell / 2))) ++rect_cells;
        }
    }
    CHECKF(rect_cells == rows_cells, "the surface table gives the 12 zones %llu cells in the Riverlands' box, G1's table %llu", static_cast<unsigned long long>(rect_cells),
           static_cast<unsigned long long>(rows_cells));
}
// [C.O.A - Cyb]

// ---- the registry on the real content.db ---------------------------------------------------------------------------------------------------------

void test_registry(world::CityNodes& nodes) {
    std::printf("the registry (the real content.db)\n");
    CHECKF(nodes.size() == 81 && nodes.zones() == 78, "%zu nodes and %zu zones, want 81 and 78", nodes.size(), nodes.zones());
    for (const auto& [name, rank] : std::map<std::string, int>{{"Verra_RVR_Winstead", 5}, {"Verra_RVR_Joeva", 6}, {"Verra_RVR_NewAela", 7}, {"Verra_RVR_Halcyon", 8},
                                                                  {"Verra_RVR_Miraleth", 9}, {"Verra_JUN_Wildport", 43}, {"Verra_ANV_KalTorhum", 63}}) {
        const world::CityNodeInfo* n = nodes.by_name(name);
        CHECKF(n && n->rank == rank, "%s has rank %d (the key the captures show)", name.c_str(), rank);
    }
    // names, the way the citynode service resolves them
    const world::CityNodeInfo* aela = nodes.by_name("New Aela");
    CHECK(aela && aela->name == "Verra_RVR_NewAela" && aela->display_name == "New Aela", "\"New Aela\" is Verra_RVR_NewAela");
    CHECK(nodes.by_name("NewAela") == aela && nodes.by_name("new_aela") == aela && nodes.by_name("RVR_NewAela") == aela && nodes.by_name("Verra_RVR_NewAela") == aela,
          "NewAela, new_aela, RVR_NewAela and the record name are the same node");
    CHECK(nodes.by_name("") == nullptr && nodes.by_name("Atlantis") == nullptr, "an empty or unknown name is no node");
    CHECK(nodes.by_rank(-1) == nullptr && nodes.by_rank(81) == nullptr && nodes.by_rank(6) && nodes.by_rank(6)->display_name == "Joeva", "by_rank");
    const world::CityNodeInfo* joeva = nodes.by_name("Joeva");
    CHECK(joeva && joeva->guid == 108646121231993ll && joeva->type == 1 && joeva->levelable && joeva->zoi_guid == kJoevaZoi, "Joeva: guid, Scientific, grows, its zone");
    CHECK(joeva && nodes.by_guid(joeva->guid) == joeva && nodes.by_guid(1) == nullptr, "by_guid");

    // a node that grows lies in its own zone
    size_t grows = 0, inside = 0;
    for (const world::CityNodeInfo& n : nodes.nodes()) {
        if (!n.levelable) continue;
        ++grows;
        const world::NodeSite s = nodes.node_at(n.x, n.y);
        if (s.zoi_guid == n.zoi_guid && s.node_rank == n.rank) ++inside;
    }
    CHECKF(grows == 26 && inside == 26, "%zu nodes grow, %zu of them lie in their own zone with their own rank", grows, inside);
    // the login spot: Ruins of Aela, Lionhold's zone, Riverlands
    const world::NodeSite login = nodes.node_at(-777664.5, 616578.7);
    const world::CityNodeInfo* lionhold = nodes.by_name("Verra_RVR_Lionhold");
    CHECK(login.zoi_guid == kRuinsOfAela && lionhold && login.node_rank == lionhold->rank && login.biome == kRiverlands, "the login spot: Ruins of Aela, Lionhold, Riverlands");
    CHECK(nodes.node_at(nodes.by_name("Windansea")->x, nodes.by_name("Windansea")->y).biome == kTropics, "Windansea is in the Tropics");
    const world::NodeSite nowhere = nodes.node_at(1.0e8, 0);
    CHECK(nowhere.zoi_guid == 0 && nowhere.node_rank == -1 && nowhere.biome == 0, "no zone: no node, the Default biome");
    const world::NodeSite sunhaven = nodes.node_at(nodes.by_name("Sunhaven")->x, nodes.by_name("Sunhaven")->y);
    CHECK(sunhaven.biome == 21, "Sunhaven's zone is Highlands (EBiomeType 21)");
    // the types: the observed ones (data_core appendix 17)
    CHECK(nodes.type_of_rank(nodes.by_name("Halcyon")->rank) == 2 && nodes.type_of_rank(nodes.by_name("Winstead")->rank) == 3 && nodes.type_of_rank(nodes.by_name("NewAela")->rank) == 4 &&
              nodes.type_of_rank(-1) == 0 && nodes.type_of_rank(500) == 0,
          "Halcyon is Divine, Winstead Martial, New Aela Economic; no node: Default");
}

void test_live(world::CityNodes& nodes) {
    std::printf("the live state\n");
    const world::CityNodeInfo* joeva = nodes.by_name("Joeva");
    const world::CityNodeInfo* halcyon = nodes.by_name("Halcyon");
    const world::CityNodeInfo* aela = nodes.by_name("NewAela");
    if (!joeva || !halcyon || !aela) {
        CHECK(false, "the Riverlands nodes");
        return;
    }
    CHECK(nodes.source() == world::LevelSource::Default && nodes.level_of_rank(joeva->rank) == 0 && nodes.version() == 0, "a new registry has every node at Wilderness");
    CHECK(nodes.describe_levels() == "every node at Wilderness", "describe_levels says so");

    // the seed file
    size_t applied = 0, skipped = 0;
    std::string error;
    CHECK(nodes.apply_seed_file(NODES_SEED_PATH, error, &applied, &skipped), "deploy/citynode_seed.json is read");
    CHECKF(applied == 2 && skipped == 0, "two levels from the seed (%zu, %zu skipped): %s", applied, skipped, error.c_str());
    CHECK(nodes.source() == world::LevelSource::Seed && nodes.level_of(joeva->guid) == 4 && nodes.level_of(halcyon->guid) == 3 && nodes.level_of(aela->guid) == 0,
          "the seed: Joeva at Town (4), Halcyon at Village (3), the rest at Wilderness");
    {
        const auto live = nodes.live();
        bool parent = false;
        for (const world::LiveNode& l : live) parent = parent || (l.guid == halcyon->guid && l.parent_guid == joeva->guid && l.level == 3);
        CHECK(parent, "Halcyon is Joeva's vassal in the seed");
    }
    CHECK(nodes.describe_levels() == "Joeva 4, Halcyon 3" || nodes.describe_levels() == "Halcyon 3, Joeva 4", "describe_levels lists the nodes above Wilderness");

    // a bad seed
    const fs::path dir = fs::temp_directory_path() / "worldd_city_nodes_test";
    fs::create_directories(dir);
    auto write = [&](const char* name, const std::string& text) {
        const fs::path p = dir / name;
        std::ofstream(p, std::ios::binary) << text;
        return p.string();
    };
    CHECK(!nodes.apply_seed_file(write("nojson.json", "{ nope"), error) && !error.empty(), "a seed that is no JSON is refused");
    CHECK(!nodes.apply_seed_file(write("nonodes.json", "{\"comment\": \"x\"}"), error) && error.find("nodes") != std::string::npos, "a seed with no nodes object is refused");
    CHECK(!nodes.apply_seed_file((dir / "missing.json").string(), error), "a missing seed is refused");
    CHECK(nodes.level_of(joeva->guid) == 4, "a refused seed changes nothing");
    CHECK(nodes.apply_seed_file(write("some.json", "{\"nodes\": {\"Joeva\": {\"level\": 2}, \"Atlantis\": {\"level\": 1}, \"Halcyon\": {\"level\": 9}, \"Miraleth\": {\"level\": 4294967297}, \"New Aela\": {\"level\": 3}}}"),
                                error, &applied, &skipped),
          "a seed with entries the registry cannot use is read");
    CHECKF(applied == 2 && skipped == 3, "two applied, three skipped (an unknown node and invalid levels): %zu, %zu", applied, skipped);
    CHECK(nodes.level_of(joeva->guid) == 2 && nodes.level_of(aela->guid) == 3 && nodes.level_of(halcyon->guid) == 0 && nodes.level_of(nodes.by_name("Miraleth")->guid) == 0,
            "and invalid levels are not narrowed into valid ones");

    // the service's state
    const uint64_t v0 = nodes.version();
    nodes.apply_snapshot({{joeva->guid, 4, 100, 0}, {halcyon->guid, 3, 7, joeva->guid}, {123456, 2, 0, 0}});
    CHECK(nodes.source() == world::LevelSource::Service && nodes.version() == v0 + 1, "a snapshot is one change, from the service");
    CHECK(nodes.level_of(joeva->guid) == 4 && nodes.level_of(halcyon->guid) == 3 && nodes.level_of(aela->guid) == 0, "it replaces every level (New Aela, not in it, is back at Wilderness; a node of no content is skipped)");
    CHECK(nodes.apply_change({aela->guid, 2, 5, 0}) && nodes.level_of(aela->guid) == 2 && nodes.version() == v0 + 2, "a change moves one node");
    CHECK(!nodes.apply_change({42, 3, 0, 0}) && nodes.version() == v0 + 2, "a change of a node that is no node of the registry is refused");
    nodes.apply_snapshot({{joeva->guid, 99, 0, 0}, {halcyon->guid, -5, 0, 0}});
    CHECK(nodes.level_of(joeva->guid) == 6 && nodes.level_of(halcyon->guid) == 0, "a level is held between 0 and 6");
    CHECK(nodes.level_of_rank(-1) == 0 && nodes.level_of_rank(10000) == 0 && nodes.level_of(7) == 0, "a rank or guid that is no node is at Wilderness");

    // the people, the scenery and the roads read the levels with no lock, acting on the version they saw: a snapshot that repeats what the registry holds (a stream that
    // reconnects) must never show a node at Wilderness meanwhile (Nodes 1 review A, M4: set_all used to zero every level first)
    nodes.apply_snapshot({{joeva->guid, 4, 0, 0}, {halcyon->guid, 3, 0, 0}});
    {
        std::atomic<bool> stop{false};
        std::atomic<long> wrong{0}, reads{0};
        std::thread reader([&] {
            while (!stop.load()) {
                if (nodes.level_of_rank(joeva->rank) != 4 || nodes.level_of_rank(halcyon->rank) != 3) ++wrong;
                ++reads;
            }
        });
        for (int i = 0; i < 20000; ++i) nodes.apply_snapshot({{joeva->guid, 4, 0, 0}, {halcyon->guid, 3, 0, 0}});
        stop = true;
        reader.join();
        CHECKF(wrong.load() == 0 && reads.load() > 0, "a snapshot that repeats the levels never shows a node at Wilderness meanwhile (%ld wrong reads of %ld)", wrong.load(), reads.load());
    }
}

// ---- the node reads against the corpus -----------------------------------------------------------------------------------------------------------

struct HostQuery {
    int fn = 0;
    std::vector<double> args;
    long want = 0;
};
struct HostBlock {
    std::string name;
    std::vector<int> types, levels;
    world::NodeContext ctx;
    std::vector<HostQuery> queries;
};
// ~ C.O.A :: C Y B ~

bool load_blocks(const std::string& path, std::vector<HostBlock>& out, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "cannot open " + path;
        return false;
    }
    std::string line;
    HostBlock* cur = nullptr;
    size_t no = 0;
    while (std::getline(f, line)) {
        ++no;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        std::istringstream is(line);
        std::vector<std::string> w;
        for (std::string x; is >> x;) w.push_back(x);
        if (w.empty() || w[0][0] == '#') continue;
        if (w[0] == "nodehost") {
            out.emplace_back();
            cur = &out.back();
            cur->name = w.size() > 1 ? w[1] : "";
        } else if (cur && w[0] == "end") {
            cur = nullptr;
        } else if (cur && w[0] == "nodes") {
            cur->types.assign(std::stoul(w[1]), 0);
            cur->levels.assign(std::stoul(w[1]), 0);
        } else if (cur && w[0] == "node") {
            const size_t r = std::stoul(w[1]);
            if (r >= cur->types.size()) {
                err = "line " + std::to_string(no) + ": a rank past nodes N";
                return false;
            }
            cur->types[r] = std::stoi(w[2]);
            cur->levels[r] = std::stoi(w[3]);
        } else if (cur && w[0] == "ctx") {
            cur->ctx = {std::stoi(w[1]), std::stoi(w[2]), std::stoi(w[3]), std::stoi(w[4])};
        } else if (cur && w[0] == "q") {
            HostQuery q;
            size_t eq = 0;
            for (size_t i = 0; i < w.size(); ++i) {
                if (w[i] == "=") eq = i;
            }
            if (eq < 2 || eq + 2 != w.size()) {
                err = "line " + std::to_string(no) + ": q FN ARGS = WANT";
                return false;
            }
            q.fn = std::stoi(w[1]);
            for (size_t i = 2; i < eq; ++i) q.args.push_back(std::stod(w[i]));
            q.want = std::stol(w[eq + 1]);
            cur->queries.push_back(std::move(q));
        }
    }
    return true;
}

void test_node_reads() {
    std::printf("the node reads against the shared corpus\n");
    std::vector<HostBlock> blocks;
    std::string err;
    CHECKF(load_blocks(STATS_CORPUS_PATH, blocks, err), "the corpus is read: %s", err.c_str());
    CHECKF(blocks.size() >= 3, "the corpus has %zu nodehost blocks, want 3", blocks.size());
    size_t asked = 0, right = 0;
    for (const HostBlock& b : blocks) {
        // the registry of the block: nodes of guid 1000 + rank, the levels as the service would give them
        std::vector<world::CityNodeInfo> infos(b.types.size());
        for (size_t i = 0; i < infos.size(); ++i) {
            infos[i].guid = 1000 + static_cast<int64_t>(i);
            infos[i].type = b.types[i];
            infos[i].name = "Node_" + std::to_string(i);
            infos[i].display_name = infos[i].name;
        }
        world::CityNodes nodes(std::move(infos), {});
        std::vector<world::LiveNode> live;
        for (size_t i = 0; i < b.levels.size(); ++i) live.push_back({1000 + static_cast<int64_t>(i), b.levels[i], 0, 0});
        nodes.apply_snapshot(live);
        for (const HostQuery& q : b.queries) {
            double out = -12345;
            const bool known = world::node_host_fn(&nodes, b.ctx, q.fn, q.args.data(), static_cast<int>(q.args.size()), out);
            ++asked;
            if (known && out == static_cast<double>(q.want)) {
                ++right;
            } else {
                std::printf("  [FAIL] %s: fn %d (%zu args) = %g (known %d), the corpus says %ld\n", b.name.c_str(), q.fn, q.args.size(), out, known ? 1 : 0, q.want);
                ++g_fail;
            }
        }
        // the same block over no registry at all: a world with no nodes answers 0 for the reads of a node
        double out = 0;
        CHECK(world::node_host_fn(nullptr, b.ctx, stats::kHostNodeLevel, std::vector<double>{0.0}.data(), 1, out) && out == 0, "a read of a node over no registry is 0");
    }
    CHECKF(asked >= 60 && right == asked, "%zu reads asked, %zu as the corpus says", asked, right);
    double out = 0;
    CHECK(!world::node_host_fn(nullptr, {}, stats::kHostPlayerLevel, nullptr, 0, out) && !world::node_host_fn(nullptr, {}, 178, nullptr, 0, out) &&
              !world::node_host_fn(nullptr, {}, 169, nullptr, 0, out),
          "a function outside 170..177 is no node read");
}

// ---- the feed ------------------------------------------------------------------------------------------------------------------------------------
// [C.O.A x C.Y.B]

/// A fake CityNodeService: every call to WatchNodes sends the snapshot of the script's current round, then what the test pushes, until it ends the stream.
struct FakeCityNode final : nodev1::CityNodeService::Service {
    std::mutex mu;
    std::condition_variable cv;
    std::string auth;
    int calls = 0;
    bool end_stream = false;          // end the stream in flight (status below)
    grpc::StatusCode end_code = grpc::StatusCode::UNAVAILABLE;
    std::vector<std::pair<int64_t, int>> snapshot;   // guid, level
    uint64_t snapshot_seq = 5;
    std::deque<nodev1::WatchNodesResponse> push;     // events to send to the stream in flight

    grpc::Status WatchNodes(grpc::ServerContext* ctx, const nodev1::WatchNodesRequest*, grpc::ServerWriter<nodev1::WatchNodesResponse>* w) override {
        std::unique_lock<std::mutex> lock(mu);
        const auto it = ctx->client_metadata().find(svc::kAuthorizationMetadata);
        auth = it == ctx->client_metadata().end() ? "" : std::string(it->second.data(), it->second.size());
        ++calls;
        nodev1::WatchNodesResponse snap;
        snap.set_seq(snapshot_seq);
        for (const auto& [guid, level] : snapshot) {
            nodev1::Node* n = snap.mutable_snapshot()->add_nodes();
            n->set_guid(guid);
            n->set_level(static_cast<uint32_t>(level));
        }
        end_stream = false;
        lock.unlock();
        if (!w->Write(snap)) return grpc::Status::CANCELLED;
        lock.lock();
        while (!ctx->IsCancelled()) {
            while (!push.empty()) {
                nodev1::WatchNodesResponse m = std::move(push.front());
                push.pop_front();
                lock.unlock();
                const bool ok = w->Write(m);
                lock.lock();
                if (!ok) return grpc::Status::CANCELLED;
            }
            if (end_stream) return grpc::Status(end_code, "scripted end");
            cv.wait_for(lock, std::chrono::milliseconds(20));
        }
        return grpc::Status::CANCELLED;
    }
    void send_change(uint64_t seq, int64_t guid, int level, int previous) {
        nodev1::WatchNodesResponse m;
        m.set_seq(seq);
        nodev1::NodeChange* c = m.mutable_node_changed();
        c->mutable_node()->set_guid(guid);
        c->mutable_node()->set_level(static_cast<uint32_t>(level));
        c->mutable_node()->set_display_name("N");
        c->set_previous_level(static_cast<uint32_t>(previous));
        std::lock_guard<std::mutex> lock(mu);
        push.push_back(std::move(m));
    }
    void send_clock(uint64_t seq) {
        nodev1::WatchNodesResponse m;
        m.set_seq(seq);
        m.mutable_clock_changed()->set_speed(1);
        std::lock_guard<std::mutex> lock(mu);
        push.push_back(std::move(m));
    }
    void end(grpc::StatusCode code) {
        std::lock_guard<std::mutex> lock(mu);
        end_code = code;
        end_stream = true;
    }
    int calls_now() {
        std::lock_guard<std::mutex> lock(mu);
        return calls;
    }
};

template <typename Pred>
bool wait_for(Pred pred, std::chrono::milliseconds limit = std::chrono::seconds(10)) {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < until) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

std::unique_ptr<world::CityNodes> small_registry() {
    std::vector<world::CityNodeInfo> infos(3);
    for (size_t i = 0; i < infos.size(); ++i) {
        infos[i].guid = 1000 + static_cast<int64_t>(i);
        infos[i].name = "Verra_RVR_N" + std::to_string(i);
        infos[i].display_name = "N" + std::to_string(i);
    }
    return std::make_unique<world::CityNodes>(std::move(infos), std::vector<world::CityZoneInfo>{});
}

void test_feed() {
    std::printf("the citynode feed\n");
    const std::string token(64, 'b');

    // 1. the service is not there: the seed's levels stay, ONE warning, the thread keeps retrying
    {
        auto nodes = small_registry();
        nodes->apply_snapshot({{1001, 3, 0, 0}}, world::LevelSource::Seed);
        svc::CityNodeFeed::Options o;
        o.address = "127.0.0.1:1";   // nothing listens there
        o.token = token;
        o.initial_backoff = std::chrono::milliseconds(50);
        o.max_backoff = std::chrono::milliseconds(100);
        svc::CityNodeFeed feed(*nodes, o);
        const auto t0 = std::chrono::steady_clock::now();
        feed.start();
        CHECK(wait_for([&] { return feed.attempts() >= 4; }), "an unreachable service is called again and again");
        CHECK(!feed.connected() && feed.warnings() == 1, "... with ONE warning for the outage");
        CHECK(nodes->level_of(1001) == 3 && nodes->source() == world::LevelSource::Seed, "... and the registry keeps the seed's levels");
        const auto before_stop = std::chrono::steady_clock::now();
        feed.stop();
        CHECK(std::chrono::steady_clock::now() - before_stop < std::chrono::seconds(2), "stop() ends the thread promptly");
        feed.stop();   // twice is fine
        (void)t0;
    }

    // 2. the service is there
    FakeCityNode fake;
    int port = 0;
    grpc::ServerBuilder b;
    b.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    b.RegisterService(&fake);
    auto server = b.BuildAndStart();
    CHECK(server && port > 0, "the fake citynode listens");
    if (!server) return;
    const std::string addr = "127.0.0.1:" + std::to_string(port);
    fake.snapshot = {{1000, 4}, {1001, 3}};
    fake.snapshot_seq = 5;
// Cyb x C-O-A

    {
        auto nodes = small_registry();
        nodes->apply_snapshot({{1002, 4, 0, 0}}, world::LevelSource::Seed);   // the seed said something else
        svc::CityNodeFeed::Options o;
        o.address = addr;
        o.token = token;
        o.initial_backoff = std::chrono::milliseconds(20);
        o.max_backoff = std::chrono::milliseconds(50);
        svc::CityNodeFeed feed(*nodes, o);
        feed.start();
        CHECK(wait_for([&] { return feed.connected(); }), "the feed connects");
        CHECK(nodes->source() == world::LevelSource::Service && nodes->level_of(1000) == 4 && nodes->level_of(1001) == 3 && nodes->level_of(1002) == 0,
              "the snapshot replaces every level at once (the seed's Level 4 of node 2 is gone)");
        CHECK(feed.seq() == 5 && feed.snapshots() == 1 && feed.warnings() == 0, "seq 5, one snapshot, no warning");
        {
            std::lock_guard<std::mutex> lock(fake.mu);
            CHECK(fake.auth == "Bearer " + token, "the call carries the service token");
        }
        // the changes, in order
        fake.send_change(6, 1000, 3, 4);
        fake.send_change(7, 1002, 2, 0);
        fake.send_clock(8);
        fake.send_change(9, 1001, 4, 3);
        CHECK(wait_for([&] { return feed.seq() == 9; }), "the changes are applied in order");
        CHECK(nodes->level_of(1000) == 3 && nodes->level_of(1002) == 2 && nodes->level_of(1001) == 4 && feed.changes() == 3 && fake.calls_now() == 1,
              "three changes moved three levels, on the one stream (a clock change is only counted)");

        // a gap: seq 12 after 9. The feed drops the stream and calls again; the snapshot is the whole state
        {
            std::lock_guard<std::mutex> lock(fake.mu);
            fake.snapshot = {{1000, 1}};
            fake.snapshot_seq = 20;
        }
        fake.send_change(12, 1000, 2, 3);
        CHECK(wait_for([&] { return feed.snapshots() == 2 && feed.seq() == 20; }), "a change that skips a number makes the feed call again");
        CHECK(fake.calls_now() == 2 && nodes->level_of(1000) == 1 && nodes->level_of(1001) == 0 && nodes->level_of(1002) == 0, "the new snapshot is the whole state (the skipped change was not applied)");

        // the service says Aborted (the reader fell behind): call again
        CHECK(feed.warnings() == 1, "(the gap was warned of once)");
        fake.end(grpc::StatusCode::ABORTED);
        CHECK(wait_for([&] { return feed.snapshots() == 3; }), "an Aborted stream is called again");
        CHECK(feed.warnings() == 2, "(the end of a stream that had delivered its snapshot is warned of once)");
        // the service shuts down
        fake.end(grpc::StatusCode::UNAVAILABLE);
        CHECK(wait_for([&] { return feed.snapshots() == 4; }), "an Unavailable stream is called again too");
        CHECK(feed.warnings() == 3, "... and each outage warns once");
        // a stop while the stream is open
        const auto before_stop = std::chrono::steady_clock::now();
        feed.stop();
        CHECK(std::chrono::steady_clock::now() - before_stop < std::chrono::seconds(3) && !feed.connected(), "stop() cancels the stream in flight");
    }
    server->Shutdown();
}

}  // namespace

int main() {
    test_zoi_surface();
    test_zoi_tables_agree();
    test_node_reads();
    test_feed();
    std::string error;
    auto nodes = world::CityNodes::load(db_path(), error);
    if (!nodes) {
        std::printf("test_worldd_city_nodes: the registry on %s: %s: skipped\n", db_path().c_str(), error.c_str());
    } else {
        test_registry(*nodes);
        test_live(*nodes);
    }
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
