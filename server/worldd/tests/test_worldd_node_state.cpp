// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_node_state.cpp  (Nodes 1, stage 2: the world knows its nodes)
//
//  What the client is told about the city nodes, on the REAL content.db (contentc version 12) and against capture 1198's login and capture 623's hops:
//
//    the maps       world/node_wire.h NodeWire::maps with the registry at retail's levels: the same 46 nodes with the same levels, parents, deploy radii, bounty commodity and
//                   tag, the same progress and citizens ranks, the same 244 estates; with the seed's levels (Joeva 4, Halcyon 3) the two nodes of the progress and citizens maps
//    the summaries  NodeWire::summary for all 49 nodes the client asks about, against the 49 retail summaries: the layout, the level, the plots of the layout at that level,
//                   the essence and the relic slots; the siege summaries rebuilt from the node to retail's exact bits
//    the session    a Tier3 join sends the maps in the GameState's open and the account's characters in OwnerInfo; the client's bunch of 49 + 49 requests is answered in retail's order
//                   (every siege summary, then every node summary); a repeated request is one answer; an unknown node and the citizenship window are not answered; the cooldowns are
//                   one empty answer; a level change reaches the client as a delta of the maps; a client that joins later has the new levels (a server hop re-sends everything)
//    the console    the client's ServerSetNodeLevel, ServerAddNodeExperience, ServerGMGetNodeData ...: received and logged with the GM switch off, forwarded to the node service
//                   with it on; GrpcNodeControl over gRPC against an in-process fake of the service: the token, the calls, the answers, an unreachable service
// ============================================================================
#include "aoc/citynode/v1/citynode.grpc.pb.h"
#include "capture_blocks.h"
#include "capture_nodes.h"
#include "content_db_fixture.h"
#include "cyb_appearance.h"
#include "fake_client.h"
#include "bootstrap/character_setup.h"
#include "content/content_db.h"
#include "net/endpoint.h"
#include "protocol/repl/block_reader.h"
#include "protocol/repl/fast_array.h"
#include "protocol/repl/node_rpc.h"
#include "protocol/repl/rep_block.h"
#include "protocol/repl/rpc.h"
#include "protocol/schema/generated/index.h"
#include "session/player_session.h"
#include "svc/city_node_control.h"
#include "svc/service_token.h"
#include "world/city_nodes.h"
#include "world/node_constants.h"
#include "world/node_wire.h"

#include <grpcpp/grpcpp.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace aoc::worldd;
namespace bs = aoc::worldd::bootstrap;
namespace conn = aoc::protocol::conn;
namespace emit = aoc::protocol::emit;
namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;
namespace gen = aoc::protocol::schema::gen;
namespace fs = std::filesystem;
namespace cf = content_fixture;
namespace fi = content_fixture::item;
namespace nodev1 = aoc::citynode::v1;
using emit::BunchWriter;
using std::chrono::milliseconds;
using std::chrono::seconds;
using svc::ItemContainer;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)
#define CHECKF(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { std::printf("  [FAIL] "); std::printf(__VA_ARGS__); std::printf("\n"); g_fail++; } \
} while (0)
// C.O.A and C | Y | B

#ifndef NODES_CONTENT_DB_PATH
#define NODES_CONTENT_DB_PATH "dist/content/content.db"
#endif
#ifndef NODES_SEED_PATH
#define NODES_SEED_PATH "deploy/citynode_seed.json"
#endif

static std::string db_path() {
    const char* env = std::getenv("AOC_CONTENT_DB");
    return env && *env ? env : NODES_CONTENT_DB_PATH;
}

static const std::string kCyb = "bbec5c86eb101fe5f38c87f0fa22e7d0";
static const std::string kSister = "0123456789abcdef0123456789abcdef";
static const std::string kMachine = "DESKTOP-EXAMPLE1-US-0123456789ABCDEF0123456789ABCDEF";
static const Clock::time_point t0{seconds(5000)};
static constexpr uint32_t kServerId = 60;
static constexpr uint32_t kRandomizer = 0x6ee87ee4u;
static const ItemContainer Bag = ItemContainer::Backpack;
static const ItemContainer Worn = ItemContainer::Equipment;
static constexpr int64_t kJoeva = 108646121231993ll, kHalcyon = 108646610851452ll;

static const schema::ClassSchema& class_named(const char* name) {
    const schema::ClassSchema* c = gen::find_class_schema(name);
    if (!c) {
        std::printf("  [FAIL] no schema %s\n", name);
        std::exit(2);
    }
    return *c;
}

static std::string guid_n(uint32_t n) {
    char buf[33];
    std::snprintf(buf, sizeof buf, "%08x%08x%08x%08x", 0xA0000000u + n, 0x1000u + n, 0x2000u + n, n);
    return buf;
}

static svc::InventoryItem make_item(ItemContainer c, int slot, int64_t record, uint32_t n, int32_t quantity = 1) {
    svc::InventoryItem it;
    it.container = c;
    it.slot = slot;
    it.item_record_id = record;
    it.instance_guid = guid_n(n);
    it.quantity = quantity;
    it.quality = 1;
    it.durability = 11;
    it.created_at_ms = 1'771'299'474'000ll;
    return it;
}

static std::vector<svc::InventoryItem> kit() {
    const std::pair<int, int64_t> worn[] = {{1, fi::kHelmet}, {2, fi::kChest}, {5, fi::kLegs}, {6, fi::kFeet}, {15, fi::kScepter}, {16, fi::kFocus}, {17, fi::kBook}, {23, fi::kBag}};
    std::vector<svc::InventoryItem> out;
    for (const auto& [slot, record] : worn) out.push_back(make_item(Worn, slot, record, 100 + static_cast<uint32_t>(slot)));
    out.push_back(make_item(Bag, 0, fi::kSword2H, 200));
    return out;
}

static svc::Character cyb_record(bool with_sister = true) {
    svc::Character c;
    c.id = kCyb;
    c.account_id = 3;
    c.name = "cyb";
    c.race_id = 0;
    c.gender_id = 0;
    c.class_id = 2;
    c.archetype_id = 17747;
    c.class_name = "Bard";
    c.appearance_json = cyb::kAppearanceJsonb;
    c.level = 1;
    c.items = kit();
    if (with_sister) c.account_characters = {{kCyb, "cyb"}, {kSister, "sister"}};
    return c;
}

static bs::LoginIdentity identity() { return {kMachine, kMachine}; }
static double world_time_at(Clock::time_point t) { return std::chrono::duration<double>(t - t0).count(); }

struct RecordingSink : PositionSink {
    void save(const std::string&, const svc::Position&) override {}
    void save_inventory(const std::string&, std::vector<svc::InventoryItem>) override {}
    void save_progress(const std::string&, svc::Progress) override {}
};

/// A node control that remembers what the console asked.
struct RecordingControl : world::NodeControl {
    std::vector<std::string> calls;
    void set_level(const std::string& n, int level, bool dv, const std::string& by) override {
        calls.push_back("set_level " + n + " " + std::to_string(level) + " " + (dv ? "dv" : "-") + " | " + by);
    }
    void add_experience(const std::string& n, int64_t amount, const std::string& by) override { calls.push_back("add_xp " + n + " " + std::to_string(amount) + " | " + by); }
    void node_data(const std::string& n, const std::string& by) override { calls.push_back("node_data " + n + " | " + by); }
    void clock(const std::string& by) override { calls.push_back("clock | " + by); }
    void advance_clock(int64_t s, const std::string& by) override { calls.push_back("advance " + std::to_string(s) + " | " + by); }
};

// ---- the registry on the real content.db ------------------------------------------------------------------------------------------------

static std::unique_ptr<world::CityNodes> g_nodes;
static std::unique_ptr<world::NodeWire> g_wire;

/// The maps of capture 1198's login (kGameStateFastMaps28), typed.
struct Rebuilt {
    std::vector<uint8_t>             bytes;
    std::vector<repl::FastArrayField> fields;
    bool                             ok = false;
};

static Rebuilt read_maps(const char* hex, size_t bits) {
    Rebuilt r;
    r.bytes = [&] {
        std::vector<uint8_t> out;
        auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
        for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) out.push_back(static_cast<uint8_t>(nib(hex[i]) << 4 | nib(hex[i + 1])));
        return out;
    }();
    size_t pos = 0;
    auto fields = repl::read_fast_array_fields(class_named("AoCGameStateBP_C"), r.bytes.data(), bits, pos, {});
    if (!fields || pos != bits) return r;
    r.fields = std::move(*fields);
    r.ok = true;
    return r;
}

static repl::NodeStateMaps retail_login() {
    const auto& b = capture_blocks::kGameStateFastMaps28;
    const Rebuilt r = read_maps(b.hex, b.bits);
    return r.ok ? repl::node_maps_of_fields(r.fields) : repl::NodeStateMaps();
}

/// The registry set to retail's levels: each node at its level and parent, its XP where the progress map's percent says.
static void set_retail_levels(world::CityNodes& nodes, const repl::NodeStateMaps& m) {
    std::map<uint32_t, int> percent;
    for (const auto& [rank, p] : m.percent) percent[rank] = p;
    std::vector<world::LiveNode> live;
    for (const repl::NodeDataEntry& n : m.nodes) {
        world::LiveNode l;
        l.guid = n.guid;
        l.level = n.level;
        l.parent_guid = n.parent;
        const world::CityNodeInfo* info = nodes.by_guid(n.guid);
        const auto p = info ? percent.find(static_cast<uint32_t>(info->rank)) : percent.end();
        if (p != percent.end() && n.level < 4) {
            const int64_t lo = world::node_constants::kExpToLevel[n.level], hi = world::node_constants::kExpToLevel[n.level + 1];
            l.experience = p->second >= 100 ? hi : lo + (static_cast<int64_t>(p->second) * (hi - lo) + 99) / 100;
        }
        live.push_back(l);
    }
    nodes.apply_snapshot(live, world::LevelSource::Service);
}

// ---- the tests: the maps ------------------------------------------------------------------------------------------------------------------
// (C-Y-B ~ CoA)

static void test_maps() {
    std::printf("the GameState's node maps against capture 1198\n");
    const repl::NodeStateMaps retail = retail_login();
    CHECK(retail.nodes.size() == 46 && retail.percent.size() == 26 && retail.citizens.size() == 14 && retail.estates.size() == 244, "(capture 1198's login maps: 46 nodes, 26 progress, 14 citizens, 244 estates)");
    CHECK(g_wire->roster().size() == 46, "the registry's roster is retail's 46 nodes (the Verra_* records with a map)");
    set_retail_levels(*g_nodes, retail);
    const repl::NodeStateMaps mine = g_wire->maps(*g_nodes);
    CHECK(mine.nodes.size() == 46 && mine.estates.size() == 244, "the maps list 46 nodes and 244 estates");
    std::map<int64_t, repl::NodeDataEntry> by_guid;
    for (const repl::NodeDataEntry& n : mine.nodes) by_guid[n.guid] = n;
    size_t found = 0, level = 0, parent = 0, radius = 0, bounty = 0, tags = 0, culture = 0, mayors = 0;
    for (const repl::NodeDataEntry& r : retail.nodes) {
        const auto it = by_guid.find(r.guid);
        if (it == by_guid.end()) continue;
        const repl::NodeDataEntry& o = it->second;
        ++found;
        level += o.level == r.level;
        parent += o.parent == r.parent;
        radius += o.deploy_radius == r.deploy_radius;
        bounty += o.bounty == r.bounty;
        tags += o.tags == r.tags;
        culture += o.culture == r.culture;
        mayors += o.mayor_name.empty() && o.mayor_guid == std::array<uint32_t, 4>{0, 0, 0, 0};
    }
    CHECKF(found == 46 && level == 46 && parent == 46, "all 46 of retail's nodes are there at retail's level and under retail's lord (%zu, %zu, %zu)", found, level, parent);
    CHECKF(radius == 46, "the vehicle deploy radius (the level's modification: 40000 from Village, 20000 for harbours) equals retail's for all 46 (%zu)", radius);
    CHECKF(bounty == 46 && tags == 46 && culture == 46, "the bounty commodity (0 for the ocean), the SiegeInactive tag and the culture equal retail's (%zu, %zu, %zu)", bounty, tags, culture);
    CHECK(mayors == 46, "no node has a mayor (nobody is one yet)");
    std::map<uint32_t, uint8_t> rp, mp, rc, mc;
    for (const auto& [rank, v] : retail.percent) rp[rank] = v;
    for (const auto& [rank, v] : mine.percent) mp[rank] = v;
    for (const auto& [rank, v] : retail.citizens) rc[rank] = v;
    for (const auto& [rank, v] : mine.citizens) mc[rank] = v;
    CHECK(rp == mp, "the progress map holds the same 26 ranks with the same percents (the nodes at level >= 1; a Town shows 0)");
    std::set<uint32_t> rc_ranks, mc_ranks;
    for (const auto& [r, v] : rc) rc_ranks.insert(r);
    for (const auto& [r, v] : mc) mc_ranks.insert(r);
    CHECK(rc_ranks == mc_ranks && mc.size() == 14, "the citizens map holds the same 14 ranks (the nodes at level >= 3); our counts are all 0");
    bool zeros = true;
    for (const auto& [r, v] : mc) zeros = zeros && v == 0;
    CHECK(zeros, "(no citizens yet)");
    std::set<int64_t> re, me;
    for (const auto& [k, o] : retail.estates) re.insert(k);
    for (const auto& [k, o] : mine.estates) me.insert(k);
    CHECK(re == me, "the 244 estates are retail's (the six test estates of the 250 records are left out)");
    CHECK(world::NodeWire::is_test_estate("Test_Freehold") && world::NodeWire::is_test_estate("FreeholdEstate_Playground_Bucktown_Small_01") && !world::NodeWire::is_test_estate("Miraleth_S_101"),
          "(the estate rule)");

    // the seed: Joeva a Town, Halcyon a Village under it
    std::string load_error;
    auto seeded_ptr = world::CityNodes::load(db_path(), load_error);
    if (!seeded_ptr) {
        std::printf("  [FAIL] the registry does not load: %s\n", load_error.c_str());
        ++g_fail;
        return;
    }
    world::CityNodes& seeded = *seeded_ptr;
    std::string seed_error;
    size_t applied = 0;
    CHECK(seeded.apply_seed_file(NODES_SEED_PATH, seed_error, &applied) && applied >= 2, "(the deploy seed loads)");
    const repl::NodeStateMaps sm = g_wire->maps(seeded);
    size_t at_joeva = 0, at_halcyon = 0;
    int64_t halcyon_lord = -1;
    for (const repl::NodeDataEntry& n : sm.nodes) {
        if (n.guid == kJoeva) at_joeva = n.level;
        if (n.guid == kHalcyon) {
            at_halcyon = n.level;
            halcyon_lord = n.parent;
        }
    }
    CHECK(halcyon_lord == kJoeva, "Halcyon is Joeva's vassal in the maps (ParentNodeGuid), as in retail's capture 1198");
    CHECK(sm.nodes.size() == 46 && at_joeva == 4 && at_halcyon == 3, "with the seed's levels the maps say Joeva 4 and Halcyon 3 (the rest Wilderness)");
    const world::CityNodeInfo* joeva = seeded.by_guid(kJoeva);
    const world::CityNodeInfo* halcyon = seeded.by_guid(kHalcyon);
    CHECK(sm.percent.size() == 2 && sm.citizens.size() == 2 && joeva && halcyon, "... two nodes in the progress map and two in the citizens map");
    CHECK(joeva && halcyon && sm.percent[0].first == static_cast<uint32_t>(joeva->rank) && sm.percent[1].first == static_cast<uint32_t>(halcyon->rank) && sm.citizens[0].second == 0,
          "... keyed by RANK (Joeva 6, Halcyon 8), with no citizens");
    CHECK(world::NodeWire::percent_of(1, 0) == 0 && world::NodeWire::percent_of(1, 6592335) == 50 && world::NodeWire::percent_of(1, 10547736) == 100 &&
              world::NodeWire::percent_of(4, 999999999) == 0 && world::NodeWire::percent_of(3, 73834150) == 100 && world::NodeWire::percent_of(3, 31643207) == 0,
          "(percent_of is the service's Progress: 50 % halfway, 100 % at the next threshold, 0 at Town)");
}

// ---- the tests: the summaries ---------------------------------------------------------------------------------------------------------------

static const schema::RpcSchema& pc_rpc(const char* name) {
    const schema::RpcSchema* r = class_named("AoCPlayerControllerBP_C").rpc(name);
    if (!r) {
        std::printf("  [FAIL] no RPC %s\n", name);
        std::exit(2);
    }
    return *r;
}

static std::vector<uint8_t> bytes_of(const char* hex) {
    std::vector<uint8_t> out;
    auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) out.push_back(static_cast<uint8_t>(nib(hex[i]) << 4 | nib(hex[i + 1])));
    return out;
}

static std::optional<repl::RpcCall> read_call(const schema::RpcSchema& rpc, const char* hex, size_t bits) {
    const std::vector<uint8_t> b = bytes_of(hex);
    std::unique_ptr<uint8_t[]> exact(new uint8_t[b.size() ? b.size() : 1]);
    std::copy(b.begin(), b.end(), exact.get());
    size_t pos = 0;
    auto p = repl::read_rpc_params(rpc, exact.get(), bits, pos, {});
    if (!p) return std::nullopt;
    repl::RpcCall c;
    c.rpc = &rpc;
    c.params = std::move(*p);
    c.decoded = true;
    c.param_bits = bits;
    return c;
}
// (Cyb | C | O | A)

static bool bits_equal(const emit::BunchWriter& w, const char* hex, size_t bits) {
    if (w.bit_pos() != bits) return false;
    const std::vector<uint8_t> b = bytes_of(hex);
    const std::vector<uint8_t> a = w.bytes();
    for (size_t i = 0; i < bits; ++i) {
        if (((a[i >> 3] >> (i & 7)) & 1) != ((b[i >> 3] >> (i & 7)) & 1)) return false;
    }
    return true;
}

static void test_summaries() {
    std::printf("the node summaries against capture 1198\n");
    // the registry at retail's levels (the nodes the client asks about but the GameState does not list are Wilderness)
    set_retail_levels(*g_nodes, retail_login());
    const schema::RpcSchema& rpc = pc_rpc("ClientReceiveNodeSummary");
    size_t asked = 0, layout = 0, level = 0, plots = 0, plot_set = 0, essence = 0, relics = 0, culture = 0, kinds = 0, unknown = 0;
    size_t operational = 0, empty = 0, bytes_ok = 0;
    for (const capture_nodes::Call& c : capture_nodes::kSummaries) {
        const auto call = read_call(rpc, c.hex, c.bits);
        const auto retail = call ? repl::parse_node_summary(*call) : std::nullopt;
        if (!retail) continue;
        ++asked;
        const auto mine = g_wire->summary(c.node, *g_nodes, 1771000000);
        if (!mine) {
            ++unknown;
            continue;
        }
        layout += mine->layout == retail->layout;
        level += mine->level == retail->level;
        // the culture is the node's winning culture in retail (Kaelar for most); ours is Kaelar, and the records that are no node say 9
        culture += retail->culture == mine->culture || (retail->culture != 0 && retail->culture != 9);
        std::multiset<int64_t> rp, mp;
        for (const repl::PlotEntry& p : retail->plots) rp.insert(p.plot);
        for (const repl::PlotEntry& p : mine->plots) mp.insert(p.plot);
        plots += mine->plots.size() == retail->plots.size();
        plot_set += rp == mp;
        essence += mine->max_node_essence == retail->max_node_essence;
        relics += mine->active_relic_slots == retail->active_relic_slots;
        for (const repl::PlotEntry& p : mine->plots) {
            operational += p.state == 2;
            empty += p.state == 0;
        }
        // the summary is a parameter set that writes: the same RPC, the same layout as retail's
        const auto params = repl::node_summary_params(rpc, *mine);
        BunchWriter w;
        bool failed = false;
        if (params) repl::write_rpc_params(rpc, *params, w, {}, &failed);
        bytes_ok += params && !failed && w.bit_pos() > 0;
        kinds += mine->node == c.node;
    }
    CHECKF(asked == 49 && unknown == 0, "all 49 nodes the client asks about have a summary (%zu asked, %zu unknown)", asked, unknown);
    CHECKF(layout == 49 && level == 49, "the layout and the level equal retail's for all 49 (%zu, %zu)", layout, level);
    CHECKF(plots == 49 && plot_set == 49, "the plots are the layout's plots at the node's level: the same count and the same plot records as retail's for all 49 (%zu, %zu)", plots, plot_set);
    CHECKF(essence == 49 && relics == 49, "the node essence and the relic slots (the level's modifications) equal retail's for all 49 (%zu, %zu)", essence, relics);
    CHECKF(culture == 49, "the culture is retail's wherever retail's is Kaelar, and 9 for a record that is no node (%zu)", culture);
    CHECKF(bytes_ok == 49 && kinds == 49, "each writes as a ClientReceiveNodeSummary (%zu)", bytes_ok);
    CHECK(operational > 0 && empty > 0, "the pre-built plots are Operational and the mayor's service plots are Empty");
    // Joeva and Halcyon at the seed's levels: the plots at the level, the essence
    const auto joeva = g_wire->summary(kJoeva, *g_nodes, 0);
    CHECK(joeva && joeva->level == 4 && joeva->plots.size() == 31 && joeva->max_node_essence == 70 && joeva->active_relic_slots == 3 && joeva->next_mayor_message_edit_time == 60,
          "a Town has 31 plots, 70 essence and 3 relic slots, and a message nobody can edit before 60");
    const auto lionhold = g_wire->summary(6064630651307950121ll, *g_nodes, 0);
    CHECK(lionhold && lionhold->level == 0 && lionhold->plots.size() == 1 && lionhold->max_node_essence == 0, "a node at Wilderness has its one plot and no essence");
    const auto none = g_wire->summary(6064634132054344390ll, *g_nodes, 0);
    CHECK(none && none->culture == 9 && none->layout == 0 && none->plots.empty() && none->next_mayor_message_edit_time == INT64_MAX && none->time_until_next_maintenance == 0,
          "a dungeon record is retail's small default (culture 9, no layout, no plots)");
    CHECK(!g_wire->summary(12345, *g_nodes, 0) && !g_wire->siege(12345, *g_nodes), "a guid that is no node record has neither");

    // the siege summaries: the idle default, rebuilt from the node to retail's exact bits
    const schema::RpcSchema& srpc = pc_rpc("ClientReceiveSiegeSummary");
    size_t exact = 0, read = 0;
    for (const capture_nodes::Call& c : capture_nodes::kSieges) {
        const auto call = read_call(srpc, c.hex, c.bits);
        const auto retail = call ? repl::parse_siege_summary(*call) : std::nullopt;
        if (!retail) continue;
        ++read;
        const int64_t node = (static_cast<int64_t>(retail->target_org_guid[2]) << 32) | retail->target_org_guid[3];
        const auto mine = g_wire->siege(node, *g_nodes);
        if (!mine) continue;
        const auto params = repl::siege_summary_params(srpc, *mine);
        BunchWriter w;
        bool failed = false;
        if (params) repl::write_rpc_params(srpc, *params, w, {}, &failed);
        exact += params && !failed && bits_equal(w, c.hex, c.bits);
    }
    CHECKF(read == 46 && exact == 46, "the idle siege summary of the node (its display name and guid) is retail's exact bits for all 46 retail answers (%zu read, %zu exact)", read, exact);
}

// ---- the session -------------------------------------------------------------------------------------------------------------------------

struct Fixture {
    fs::path dir;
    std::optional<content::ContentDb> content;
    aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
    std::unique_ptr<PlayerWorld> world;
    std::unique_ptr<world::CityNodes> nodes;
    RecordingControl control;

    explicit Fixture(bool seeded = true, bool with_control = true) {
        dir = cf::make_temp_dir("worldd_node_state_");
        const fs::path db = dir / "content.db";
        if (!cf::make_db(db)) std::exit(2);
        std::string error;
        content = content::ContentDb::open(db.string(), error);
        if (!content) {
            std::printf("  [FAIL] the content.db does not open: %s\n", error.c_str());
            std::exit(2);
        }
        world = std::make_unique<PlayerWorld>(&*content, "", bs::GameStateMode::Tier3, kServerId, guids);
        nodes = world::CityNodes::load(db_path(), error);
        if (!nodes) {
            std::printf("  [FAIL] the real content.db has no city nodes: %s\n", error.c_str());
            std::exit(2);
        }
        if (seeded) {
            size_t applied = 0;
            if (!nodes->apply_seed_file(NODES_SEED_PATH, error, &applied)) {
                std::printf("  [FAIL] the seed does not load: %s\n", error.c_str());
                std::exit(2);
            }
        }
        world->set_city_nodes(nodes.get());
        world->set_node_wire(g_wire.get());
        if (with_control) world->set_node_control(&control);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    ~Fixture() {
        world.reset();
        nodes.reset();
        content.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

struct Harness {
    Fixture fx;
    RecordingSink sink;
    Connection conn{ConnectionConfig{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870}, t0};
    fake::Client client{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870};
    std::unique_ptr<PlayerSession> s;
    Clock::time_point t = t0;
    std::vector<fake::Delivered> got;
    std::string error;
    uint32_t pawn_ch = 0, pc_ch = 0, gs_ch = 0;
    svc::Character record;

    explicit Harness(svc::Character r, bool seeded = true, bool with_control = true) : fx(seeded, with_control), record(std::move(r)) {
        fx.world->set_position_sink(&sink);
        s = PlayerSession::join(conn, *fx.world, record, identity(), world_time_at(t), error);
        if (!s) {
            std::printf("  [FAIL] the session does not join: %s\n", error.c_str());
            std::exit(2);
        }
        pawn_ch = s->channel_of(bs::Role::Pawn);
        pc_ch = s->channel_of(bs::Role::PlayerController);
        gs_ch = s->channel_of(bs::Role::GameState);
    }
// C-Y-B : CoA

    void flush() {
        for (auto& pkt : conn.flush(t)) {
            const auto r = client.receive(pkt);
            for (const auto& d : r.delivered) got.push_back(d);
        }
    }
    void client_sends(std::vector<fake::Bunch> bunches) {
        const auto pkt = client.packet(std::move(bunches));
        const std::vector<InBunch> in = conn.receive(pkt, t);
        s->take_acks(conn);
        for (const InBunch& b : in) s->on_bunch(conn, b, t);
        s->end_of_datagram(conn);
    }
    void step(milliseconds d = milliseconds(20)) {
        t += d;
        fx.world->tick_world(world_time_at(t));
        s->tick(conn, t);
        flush();
        s->take_acks(conn);
        client_sends({});
    }
    void run(milliseconds total) {
        for (milliseconds done{0}; done < total; done += milliseconds(20)) step();
    }
    void settle() {
        for (int quiet = 0, i = 0; quiet < 8 && i < 1500; ++i) {
            const size_t before = got.size();
            step();
            quiet = (got.size() == before && s->join_pending() == 0 && s->replies_pending() == 0) ? quiet + 1 : 0;
        }
    }
    void play();
};

/// The client's bunch with several RPCs of one class, each its own content block (the retail client's possession bunch has 72, the node requests 98).
static BunchWriter rpcs_bunch(const schema::ClassSchema& cls, const std::vector<std::pair<const char*, repl::RpcParams>>& calls) {
    BunchWriter out;
    for (const auto& [name, params] : calls) {
        const schema::RpcSchema* rpc = cls.rpc(name);
        BunchWriter field;
        bool failed = false;
        if (!rpc || (repl::write_rpc_field(cls, *rpc, params, field, {}, &failed) == 0 && !params.values.empty()) || failed) {
            std::printf("  [FAIL] %s does not write\n", name);
            continue;
        }
        repl::BlockHeader header;
        repl::write_content_block(out, header, field, /*server_form=*/false);
    }
    return out;
}

static BunchWriter actor_rpc(const schema::ClassSchema& cls, const char* name, const repl::RpcParams& params) { return rpcs_bunch(cls, {{name, params}}); }

void Harness::play() {
    settle();
    client_sends({client.actor(pawn_ch, actor_rpc(class_named("PlayerPawn_C"), "ServerClientIsReadyToPhaseIn", {}), true)});
    settle();
    run(milliseconds(400));
}

/// The calls of a chain's RPC blocks, by name, as the client reads them with the class's schema.
static std::vector<repl::RpcCall> calls_of(const fake::Delivered& d, const char* cls) {
    std::vector<repl::RpcCall> out;
    const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
    if (!blocks) return out;
    for (const auto& b : *blocks) {
        if (b.header.has_rep_layout) continue;
        size_t p = b.payload_pos;
        auto calls = repl::read_rpc_fields(class_named(cls), d.content.data(), b.payload_pos + b.payload_bits, p);
        if (calls) {
            for (auto& c : *calls) out.push_back(std::move(c));
        }
    }
    return out;
}

/// UPackageMapClient::SerializeNewActor (test_worldd_session.cpp read_new_actor): the bit after it, 0 when it does not read.
static size_t new_actor_end(const BunchWriter& w, bool player_controller) {
    aoc::protocol::wire::PacketReader r(w.data(), w.byte_size(), w.bit_pos());
    for (int i = 0; i < 3; ++i) {   // the actor, its archetype and the level
        r.read_uint32();
        r.read_uint32();
        r.read_uint32();
        r.read_uint32();
    }
    auto vector = [&] {
        if (!r.read_bit()) return true;
        if (!r.read_bit()) return false;
        return repl::read_quantized_vector(r).has_value();
    };
    if (!vector()) return 0;
    if (r.read_bit() && !repl::read_struct("FRotator", r)) return 0;
    if (!vector() || !vector()) return 0;
    if (player_controller) {
        r.read_uint8();
        r.read_uint32();
    }
    return r.overflowed() ? 0 : r.pos();
}

using States = std::map<uint64_t, repl::ObjectState>;
/// The states a chain's property blocks carry (an open's blocks start after SerializeNewActor).
static States states_of(const fake::Delivered& d, const repl::Actor& actor, bool player_controller = false) {
    States out;
    const size_t start = d.open ? new_actor_end(d.content, player_controller) : 0;
    const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), start);
    if (!blocks) return out;
    for (const repl::BlockView& b : *blocks) {
        const schema::ClassSchema* cls = nullptr;
        uint64_t id = 0;
        if (b.header.is_actor) {
            cls = actor.obj.schema;
        } else {
            const repl::Subobject* sub = actor.find(b.header.object);
            cls = sub ? sub->obj.schema : nullptr;
            id = b.header.object.ObjectId;
        }
        if (!cls) continue;
        BunchWriter w;
        w.write_bit_range(d.content.data(), b.payload_pos, b.payload_bits);
        auto st = repl::load_object_state(*cls, w.data(), b.payload_bits, b.header.has_rep_layout);
        if (st) out.emplace(id, std::move(*st));
    }
    return out;
}
// Cyb - C-O-A

/// The chains of a channel from `from` on.
static std::vector<const fake::Delivered*> chains_on(const Harness& h, uint32_t channel, size_t from, bool include_open = true) {
    std::vector<const fake::Delivered*> out;
    for (size_t i = from; i < h.got.size(); ++i) {
        if (h.got[i].channel == channel && !h.got[i].close && (include_open || !h.got[i].open)) out.push_back(&h.got[i]);
    }
    return out;
}

static std::vector<std::string> pc_calls_from(const Harness& h, size_t from, std::vector<repl::RpcCall>* keep = nullptr) {
    std::vector<std::string> names;
    for (const fake::Delivered* d : chains_on(h, h.pc_ch, from, false)) {
        for (repl::RpcCall& c : calls_of(*d, "AoCPlayerControllerBP_C")) {
            if (c.rpc && std::string(c.rpc->name) == "ClientSetTime") continue;   // the clock sync the session sends on its own schedule
            names.push_back(c.rpc ? c.rpc->name : "?");
            if (keep) keep->push_back(std::move(c));
        }
    }
    return names;
}

static void test_join() {
    std::printf("a Tier3 join: the maps in the GameState's open, the account's characters in OwnerInfo\n");
    Harness h(cyb_record());
    h.settle();
    const std::vector<const fake::Delivered*> gs = chains_on(h, h.gs_ch, 0);
    CHECK(!gs.empty() && gs[0]->open, "the GameState's channel opens");
    // what the client has of the maps: read from the chains it received
    repl::NodeStateMaps client_maps;
    const schema::ClassSchema& gs_schema = class_named("AoCGameStateBP_C");
    for (const fake::Delivered* d : gs) {
        const States st = states_of(*d, h.s->bootstrap().game_state());
        const auto it = st.find(0);
        if (it == st.end()) {
            const auto blocks = repl::read_content_blocks(d->content.data(), d->content.bit_pos(), d->open ? new_actor_end(d->content, false) : 0);
            for (const repl::BlockView& b : blocks ? *blocks : std::vector<repl::BlockView>()) {
                BunchWriter w;
                w.write_bit_range(d->content.data(), b.payload_pos, b.payload_bits);
                std::string why;
                const auto one = repl::load_object_state(gs_schema, w.data(), b.payload_bits, b.header.has_rep_layout, &why);
                std::printf("    (a block of %zu bits, rep layout %d, actor %d: %s)\n", static_cast<size_t>(b.payload_bits), b.header.has_rep_layout ? 1 : 0, b.header.is_actor ? 1 : 0, one ? "reads" : why.c_str());
            }
            continue;
        }
        const repl::NodeStateMaps part = repl::read_node_maps(gs_schema, it->second.arrays);
        client_maps.nodes.insert(client_maps.nodes.end(), part.nodes.begin(), part.nodes.end());
        client_maps.percent.insert(client_maps.percent.end(), part.percent.begin(), part.percent.end());
        client_maps.citizens.insert(client_maps.citizens.end(), part.citizens.begin(), part.citizens.end());
        client_maps.estates.insert(client_maps.estates.end(), part.estates.begin(), part.estates.end());
    }
    const repl::NodeStateMaps want = g_wire->maps(*h.fx.nodes);
    if (!(client_maps == want)) {
        std::printf("    (chains on the GameState channel: %zu; the client has %zu nodes, %zu progress, %zu citizens, %zu estates; the server meant %zu, %zu, %zu, %zu)\n", gs.size(),
                    client_maps.nodes.size(), client_maps.percent.size(), client_maps.citizens.size(), client_maps.estates.size(), want.nodes.size(), want.percent.size(),
                    want.citizens.size(), want.estates.size());
    }
    CHECK(client_maps == want, "the client's GameState holds the maps of the registry's levels: 46 nodes, 2 progress entries, 2 citizens, 244 estates");
    CHECK(client_maps.nodes.size() == 46 && client_maps.estates.size() == 244 && client_maps.percent.size() == 2, "(46, 244 and 2)");
    CHECK(h.s->node_maps_version() == h.fx.nodes->version(), "(the session knows which version of the registry the join had)");

    // OwnerInfo: the account's two characters, the joining one first, with the upper-case 32-digit guid retail sends
    bool owner_ok = false;
    for (const fake::Delivered* d : chains_on(h, h.pawn_ch, 0)) {
        const States st = states_of(*d, h.s->bootstrap().pawn());
        const uint64_t owner = h.s->bootstrap().setup().guids.owner_info.ObjectId;
        const auto it = st.find(owner);
        if (it == st.end()) continue;
        const repl::Value* v = it->second.state.get(*it->second.schema, "CharactersNodeData");
        const auto* list = v ? v->get_if<repl::ArrayValue>() : nullptr;
        if (!list || list->elements.size() != 2) continue;
        const auto str = [&](size_t i, uint16_t handle) {
            const auto f = list->elements[i].values.find(handle);
            const std::string* s = f == list->elements[i].values.end() ? nullptr : f->second.get_if<std::string>();
            return s ? *s : std::string();
        };
        owner_ok = str(0, 1).empty() && str(0, 2) == "BBEC5C86EB101FE5F38C87F0FA22E7D0" && str(0, 3) == "cyb" && str(1, 2) == "0123456789ABCDEF0123456789ABCDEF" && str(1, 3) == "sister";
    }
    CHECK(owner_ok, "OwnerInfo.CharactersNodeData lists the account's characters: an empty CharacterId, the guid in upper case, the name");
    {
        Harness lone(cyb_record(/*with_sister=*/false));
        lone.settle();
        CHECK(lone.s->bootstrap().setup().account_characters.size() == 1 && lone.s->bootstrap().setup().account_characters[0].name == "cyb",
              "an account whose list is not known lists the joining character alone");
    }
    CHECK(h.s->bootstrap().setup().citizen_node == 0, "(no citizen node: OwnerInfo.CitizenNodeGuid and MayorNode stay 0, CaravanLaunchNode stays 0)");
}
// ~ C Y B :: C | O | A ~

static repl::RpcParams node_param(const char* rpc_name, const char* param, int64_t node) {
    repl::RpcParams p;
    repl::set_rpc_param(pc_rpc(rpc_name), p, param, repl::Value::i64(node));
    return p;
}

static void test_requests() {
    std::printf("the client's requests: 49 siege + 49 summary requests in one bunch, answered in retail's order\n");
    Harness h(cyb_record());
    h.play();
    const size_t from = h.got.size();
    // the retail client's bunch: for each of the 49 nodes, ServerRequestCurrentSiegeSummary then ServerRequestNodeSummary (capture 1198 line 466/467)
    std::vector<int64_t> asked;
    for (const capture_nodes::Call& c : capture_nodes::kSummaries) asked.push_back(c.node);
    std::vector<std::pair<const char*, repl::RpcParams>> calls;
    for (const int64_t node : asked) {
        calls.emplace_back("ServerRequestCurrentSiegeSummary", node_param("ServerRequestCurrentSiegeSummary", "NodeId", node));
        calls.emplace_back("ServerRequestNodeSummary", node_param("ServerRequestNodeSummary", "Node", node));
    }
    // the same node twice in the bunch is one answer; node 0 and a guid worldd does not know are not answered
    calls.emplace_back("ServerRequestNodeSummary", node_param("ServerRequestNodeSummary", "Node", asked[3]));
    calls.emplace_back("ServerRequestNodeSummary", node_param("ServerRequestNodeSummary", "Node", 0));
    calls.emplace_back("ServerRequestNodeSummary", node_param("ServerRequestNodeSummary", "Node", 987654321));
    // (the retail client's 98 fit one bunch of 7,192 bits; this fake client's packets are smaller, so the same calls go in datagrams of 30)
    for (size_t i = 0; i < calls.size(); i += 30) {
        const std::vector<std::pair<const char*, repl::RpcParams>> part(calls.begin() + static_cast<std::ptrdiff_t>(i),
                                                                       calls.begin() + static_cast<std::ptrdiff_t>(std::min(calls.size(), i + 30)));
        h.client_sends({h.client.actor(h.pc_ch, rpcs_bunch(class_named("AoCPlayerControllerBP_C"), part), true)});
    }
    h.settle();
    std::vector<repl::RpcCall> kept;
    const std::vector<std::string> names = pc_calls_from(h, from, &kept);
    size_t sieges = 0, summaries = 0, other = 0, last_siege = 0, first_summary = names.size();
    for (size_t i = 0; i < names.size(); ++i) {
        if (names[i] == "ClientReceiveSiegeSummary") {
            ++sieges;
            last_siege = i;
        } else if (names[i] == "ClientReceiveNodeSummary") {
            ++summaries;
            first_summary = std::min(first_summary, i);
        } else {
            ++other;
        }
    }
    CHECKF(sieges == 46 && summaries == 49, "46 siege summaries and 49 node summaries come back (%zu, %zu): retail answers no siege request of the two dungeons and the jungle, and the repeated and the unknown requests get none", sieges, summaries);
    CHECK(other == 0 && last_siege < first_summary, "every siege summary comes before every node summary, as retail answers (capture 1198: lines 505-516, then 540)");
    CHECK(h.s->node_requests() == 101 && h.s->siege_summaries_sent() == 46 && h.s->node_summaries_sent() == 49, "(the session counted 101 requests and 95 answers)");
    // the answers are in the order asked, and are the registry's
    bool in_order = true, match = true;
    size_t si = 0, ni = 0;
    std::vector<int64_t> asked_sieges;
    for (const int64_t node : asked) {
        if (g_wire->is_real_node(node)) asked_sieges.push_back(node);
    }
    for (const repl::RpcCall& c : kept) {
        if (std::string(c.rpc->name) == "ClientReceiveSiegeSummary") {
            const auto s = repl::parse_siege_summary(c);
            const int64_t node = s ? (static_cast<int64_t>(s->target_org_guid[2]) << 32) | s->target_org_guid[3] : 0;
            in_order = in_order && si < asked_sieges.size() && node == asked_sieges[si];
            ++si;
        } else if (std::string(c.rpc->name) == "ClientReceiveNodeSummary") {
            const auto s = repl::parse_node_summary(c);
            in_order = in_order && s && ni < asked.size() && s->node == asked[ni];
            const auto mine = s ? g_wire->summary(s->node, *h.fx.nodes, 0) : std::nullopt;
            match = match && s && mine && s->level == mine->level && s->layout == mine->layout && s->plots.size() == mine->plots.size() && s->max_node_essence == mine->max_node_essence;
            ++ni;
        }
    }
    CHECK(in_order, "each answer is for the node asked, in the order asked");
    CHECK(match, "and is the summary of the registry's level (the seed's: Joeva a Town with 31 plots, Halcyon a Village with 23)");
// C.O.A with Cyb

    // the node window asks again: one answer for each node asked for, however often
    const size_t again_from = h.got.size();
    h.client_sends({h.client.actor(h.pc_ch, rpcs_bunch(class_named("AoCPlayerControllerBP_C"),
                                                      {{"ServerRequestNodeSummary", node_param("ServerRequestNodeSummary", "Node", kJoeva)},
                                                       {"ServerRequestNodeSummary", node_param("ServerRequestNodeSummary", "Node", kJoeva)},
                                                       {"ServerRequestNodeSummary", node_param("ServerRequestNodeSummary", "Node", kHalcyon)}}),
                                  true)});
    h.settle();
    const std::vector<std::string> again = pc_calls_from(h, again_from);
    CHECK(std::count(again.begin(), again.end(), std::string("ClientReceiveNodeSummary")) == 2, "the node window's repeated request is answered once per node (Joeva, Halcyon)");

    // the cooldowns and the citizenship window
    const size_t cd_from = h.got.size();
    h.client_sends({h.client.actor(h.pc_ch, rpcs_bunch(class_named("AoCPlayerControllerBP_C"), {{"ServerRequestAllNodeSiegeCooldowns", {}}, {"ServerSetEssentialCitizenshipWindowData", {}}}), true)});
    h.settle();
    std::vector<repl::RpcCall> cd;
    const std::vector<std::string> cd_names = pc_calls_from(h, cd_from, &cd);
    if (cd_names != std::vector<std::string>{"ClientReceiveAllCurrentNodeSiegeCooldowns"}) {
        std::printf("    (the PlayerController got:");
        for (const std::string& n : cd_names) std::printf(" %s", n.c_str());
        std::printf("; cooldown answers sent %zu, requests %zu)\n", h.s->siege_cooldowns_sent(), h.s->node_requests());
    }
    CHECK(cd_names == std::vector<std::string>{"ClientReceiveAllCurrentNodeSiegeCooldowns"}, "ServerRequestAllNodeSiegeCooldowns is answered (the empty array); the citizenship window is not");
    CHECK(cd.size() == 1 && cd[0].param_bits == 1, "the cooldown answer is one bit, as retail's");
    CHECK(h.s->citizenship_requests() == 1 && h.s->siege_cooldowns_sent() == 1, "(the citizenship request is counted and logged once)");
}

static void test_level_change() {
    std::printf("a level change reaches every client as a delta of the maps; a client that joins later has the new levels\n");
    Harness h(cyb_record());
    h.play();
    const schema::ClassSchema& gs_schema = class_named("AoCGameStateBP_C");
    const uint64_t before_version = h.s->node_maps_version();
    const size_t from = h.got.size();
    // the node service says Joeva is a Village now
    world::LiveNode joeva;
    joeva.guid = kJoeva;
    joeva.level = 3;
    CHECK(h.fx.nodes->apply_change(joeva), "(the registry takes the change)");
    h.run(milliseconds(300));
    CHECK(h.s->node_maps_version() != before_version && h.s->node_map_updates() == 1, "the session saw the registry change and sent one update");
    std::vector<repl::NodeStateMaps> deltas;
    for (const fake::Delivered* d : chains_on(h, h.gs_ch, from, false)) {
        const States st = states_of(*d, h.s->bootstrap().game_state());
        const auto it = st.find(0);
        if (it != st.end()) deltas.push_back(repl::read_node_maps(gs_schema, it->second.arrays));
    }
    CHECK(deltas.size() == 1 && deltas[0].nodes.size() == 1 && deltas[0].nodes[0].guid == kJoeva && deltas[0].nodes[0].level == 3 && deltas[0].estates.empty(),
          "the client gets Joeva's node data at level 3 and nothing else of the 244 estates and 46 nodes");
    // a second change: Halcyon to Wilderness takes it out of the progress and citizens maps
    world::LiveNode halcyon;
    halcyon.guid = kHalcyon;
    halcyon.level = 0;
    h.fx.nodes->apply_change(halcyon);
    const size_t from2 = h.got.size();
    h.run(milliseconds(300));
    CHECK(h.s->node_map_updates() == 2 && !chains_on(h, h.gs_ch, from2, false).empty(), "a second change is a second update");
    // a client that joins after the changes has the new levels in its open: a server hop re-sends everything
    Connection conn2{ConnectionConfig{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870}, t0};
    std::string join_error;
    auto later = PlayerSession::join(conn2, *h.fx.world, h.record, identity(), world_time_at(h.t), join_error);
    CHECK(later && later->bootstrap().setup().nodes == g_wire->maps(*h.fx.nodes) && later->node_maps_version() == h.fx.nodes->version(), "a session that joins later has the registry's levels as of now in its open");
    const repl::NodeStateMaps now = g_wire->maps(*h.fx.nodes);
    size_t joeva_level = 9, halcyon_level = 9;
    for (const repl::NodeDataEntry& n : now.nodes) {
        if (n.guid == kJoeva) joeva_level = n.level;
        if (n.guid == kHalcyon) halcyon_level = n.level;
    }
    CHECK(joeva_level == 3 && halcyon_level == 0 && now.percent.size() == 1 && now.citizens.size() == 1, "(the maps: Joeva a Village, Halcyon Wilderness, one node in each small map)");
}

static void test_restore() {
    std::printf("a node update the connection does not take is built again, identically\n");
    Harness h(cyb_record());
    h.settle();
    bs::PlayerBootstrap pb(h.s->bootstrap().setup(), bs::GameStateMode::Tier3);
    const auto join = pb.join_sequence();
    CHECK(pb.ok() && join && !join->empty(), "(an offline bootstrap of the same setup)");
    if (!join) return;
    repl::NodeStateMaps changed = pb.game_state().obj.schema ? repl::read_node_maps(*pb.game_state().obj.schema, pb.game_state().obj.arrays) : repl::NodeStateMaps();
    CHECK(changed.nodes.size() == 46, "(its GameState holds the 46 nodes)");
    changed.nodes[3].level = 2;
    const auto checkpoint = pb.checkpoint();
    const auto first = pb.node_maps_update(changed);
    CHECK(first.has_value(), "the first update builds");
    pb.restore(checkpoint);
    const auto again = pb.node_maps_update(changed);
    CHECK(first && again && first->emission.payload.bit_pos() == again->emission.payload.bit_pos() && first->emission.payload.bytes() == again->emission.payload.bytes(),
          "after a restore the same update builds to the same bits (the arrays and the shadow went back)");
    const auto nothing = pb.node_maps_update(changed);
    CHECK(!nothing.has_value(), "and once built and kept, the same maps again send nothing");
    // only the checkpoint taken just before an update puts that update's arrays back: the restore of ANOTHER step (refused later) must not undo a node update that was sent
    changed.nodes[4].level = 3;
    const auto sent = pb.node_maps_update(changed);
    CHECK(sent.has_value(), "(a second update builds and is kept)");
    const auto other_step = pb.checkpoint();
    pb.restore(other_step);
    CHECK(!pb.node_maps_update(changed).has_value(), "the restore of a later checkpoint leaves the sent node maps as they are");
    bs::PlayerBootstrap tier2(h.s->bootstrap().setup(), bs::GameStateMode::Tier2);
    CHECK(!tier2.node_maps_update(changed).has_value(), "a tier-2 bootstrap has no node maps to update");
}
// C | O | A and Cyb

static void test_console() {
    std::printf("the client's node GM console\n");
    Harness h(cyb_record());
    h.play();
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const auto send = [&](const char* name, const std::function<void(const schema::RpcSchema&, repl::RpcParams&)>& fill) {
        repl::RpcParams p;
        fill(pc_rpc(name), p);
        h.client_sends({h.client.actor(h.pc_ch, actor_rpc(pc, name, p), true)});
        h.run(milliseconds(300));
    };
    const auto set_level = [&](const char* node, int level) {
        send("ServerSetNodeLevel", [&](const schema::RpcSchema& rpc, repl::RpcParams& p) {
            repl::set_rpc_param(rpc, p, "InNode", repl::Value::str(node));
            repl::set_rpc_param(rpc, p, "InLevel", repl::Value::i64(level));
            repl::set_rpc_param(rpc, p, "bDisableVassalshipSystem", repl::Value::boolean(true));
        });
    };
    CHECK(!h.fx.world->gm_commands(), "(the GM switch is off by default)");
    set_level("Verra_RVR_Joeva", 3);
    send("ServerAddNodeExperience", [&](const schema::RpcSchema& rpc, repl::RpcParams& p) {
        repl::set_rpc_param(rpc, p, "InNode", repl::Value::str("Halcyon"));
        repl::set_rpc_param(rpc, p, "Experience", repl::Value::i64(5000));
    });
    CHECK(h.s->node_gm_rpcs() == 2 && h.fx.control.calls.empty(), "with the switch off the two RPCs are received (and logged) and reach no one");
    h.fx.world->set_gm_commands(true);
    set_level("Verra_RVR_Joeva", 3);
    CHECK(h.fx.control.calls.size() == 1 && h.fx.control.calls[0].rfind("set_level Verra_RVR_Joeva 3 dv", 0) == 0, "ServerSetNodeLevel with the switch on reaches the node service with its node, level and flag");
    CHECK(h.fx.control.calls[0].find("by cyb") != std::string::npos, "... saying who asked");
    send("ServerAddNodeExperience", [&](const schema::RpcSchema& rpc, repl::RpcParams& p) {
        repl::set_rpc_param(rpc, p, "InNode", repl::Value::str("Halcyon"));
        repl::set_rpc_param(rpc, p, "Experience", repl::Value::i64(-250));
    });
    CHECK(h.fx.control.calls.size() == 2 && h.fx.control.calls[1].rfind("add_xp Halcyon -250", 0) == 0, "ServerAddNodeExperience reaches it, a negative amount negative");
    send("ServerGMGetNodeData", [&](const schema::RpcSchema& rpc, repl::RpcParams& p) { repl::set_rpc_param(rpc, p, "NodeName", repl::Value::str("Miraleth")); });
    CHECK(h.fx.control.calls.size() == 3 && h.fx.control.calls[2].rfind("node_data Miraleth", 0) == 0, "ServerGMGetNodeData reaches it");
    send("ServerGMWhatTimeIsIt", [&](const schema::RpcSchema&, repl::RpcParams&) {});
    CHECK(h.fx.control.calls.size() == 4 && h.fx.control.calls[3].rfind("clock", 0) == 0, "ServerGMWhatTimeIsIt asks for the node clock");
    send("ServerGMFastForwardRealTime", [&](const schema::RpcSchema& rpc, repl::RpcParams& p) {
        repl::Ticks ticks;
        ticks.ticks = 3600ll * 10000000ll;   // an hour of FTimespan
        repl::set_rpc_param(rpc, p, "FastForwardDuration", repl::Value(ticks));
    });
    CHECK(h.fx.control.calls.size() == 5 && h.fx.control.calls[4].rfind("advance 3600", 0) == 0, "ServerGMFastForwardRealTime skips the node clock by its seconds");
    send("ServerGMSetMayor", [&](const schema::RpcSchema& rpc, repl::RpcParams& p) {
        repl::set_rpc_param(rpc, p, "NodeName", repl::Value::str("Joeva"));
        repl::set_rpc_param(rpc, p, "CharacterName", repl::Value::str("cyb"));
    });
    CHECK(h.fx.control.calls.size() == 5 && h.s->node_gm_rpcs() == 8, "any other ServerGM* call is received and logged, and goes nowhere (its milestone is later)");

    // no node service connected: the console says so and nothing crashes
    Harness bare(cyb_record(), /*seeded=*/true, /*with_control=*/false);
    bare.play();
    bare.fx.world->set_gm_commands(true);
    repl::RpcParams p;
    repl::set_rpc_param(pc_rpc("ServerSetNodeLevel"), p, "InNode", repl::Value::str("Joeva"));
    repl::set_rpc_param(pc_rpc("ServerSetNodeLevel"), p, "InLevel", repl::Value::i64(2));
    bare.client_sends({bare.client.actor(bare.pc_ch, actor_rpc(pc, "ServerSetNodeLevel", p), true)});
    bare.run(milliseconds(200));
    CHECK(bare.s->node_gm_rpcs() == 1 && bare.s->gm_rpcs_answered() == 0, "without a node service the RPC is received and refused");
}

// ---- the GM control over gRPC -----------------------------------------------------------------------------------------------------------------------

struct FakeService final : nodev1::CityNodeService::Service {
    std::mutex mu;
    std::string token_seen;
    std::vector<std::string> calls;

    static nodev1::Node node(const std::string& name, uint32_t level, int64_t xp) {
        nodev1::Node n;
        n.set_guid(kJoeva);
        n.set_name("Verra_RVR_" + name);
        n.set_display_name(name);
        n.set_level(level);
        n.set_level_name(level == 4 ? "Town" : "Village");
        n.set_experience(xp);
        n.set_percent(level == 4 ? 0 : 12);
        return n;
    }
    void note(grpc::ServerContext* ctx, const std::string& what) {
        std::lock_guard<std::mutex> lock(mu);
        const auto it = ctx->client_metadata().find(svc::kAuthorizationMetadata);
        token_seen = it == ctx->client_metadata().end() ? "" : std::string(it->second.data(), it->second.size());
        calls.push_back(what);
    }
    grpc::Status SetNodeLevel(grpc::ServerContext* ctx, const nodev1::SetNodeLevelRequest* req, nodev1::SetNodeLevelResponse* resp) override {
        note(ctx, "SetNodeLevel " + req->node().name() + " " + std::to_string(req->level()) + (req->disable_vassalship() ? " dv" : ""));
        if (req->node().name() == "Atlantis") return grpc::Status(grpc::StatusCode::NOT_FOUND, "no node named Atlantis");
        if (req->level() > 4) return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "level 9 is above the highest, 4");
        *resp->mutable_node() = node("Joeva", static_cast<uint32_t>(req->level()), 0);
        resp->set_changed(true);
        return grpc::Status::OK;
    }
    grpc::Status AddNodeExperience(grpc::ServerContext* ctx, const nodev1::AddNodeExperienceRequest* req, nodev1::AddNodeExperienceResponse* resp) override {
        note(ctx, "AddNodeExperience " + req->node().name() + " " + std::to_string(req->amount()));
        *resp->mutable_node() = node("Joeva", 3, 1200);
        return grpc::Status::OK;
    }
    grpc::Status GetNode(grpc::ServerContext* ctx, const nodev1::GetNodeRequest* req, nodev1::GetNodeResponse* resp) override {
        note(ctx, "GetNode " + req->node().name());
        *resp->mutable_node() = node("Joeva", 4, 0);
        return grpc::Status::OK;
    }
    grpc::Status GetClock(grpc::ServerContext* ctx, const nodev1::GetClockRequest*, nodev1::GetClockResponse* resp) override {
        note(ctx, "GetClock");
        resp->mutable_clock()->set_speed(2.0);
        resp->mutable_clock()->set_node_unix_ms(1771000000000ll);
        return grpc::Status::OK;
    }
    grpc::Status AdvanceClock(grpc::ServerContext* ctx, const nodev1::AdvanceClockRequest* req, nodev1::AdvanceClockResponse*) override {
        note(ctx, "AdvanceClock " + std::to_string(req->seconds()));
        return grpc::Status::OK;
    }
};
// == C Y B | C.O.A ==

static void test_control() {
    std::printf("the GM control over gRPC against a fake of the node service\n");
    const std::string token(64, 'c');
    FakeService service;
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
    if (!server || port == 0) {
        std::printf("  [FAIL] the fake service does not start\n");
        ++g_fail;
        return;
    }
    std::mutex lines_mu;
    std::vector<std::string> lines;
    svc::GrpcNodeControl::Options o;
    o.address = "127.0.0.1:" + std::to_string(port);
    o.token = token;
    o.deadline_ms = 2000;
    {
        svc::GrpcNodeControl control(o, [&](const std::string& l) {
            std::lock_guard<std::mutex> lock(lines_mu);
            lines.push_back(l);
        });
        control.set_level("Verra_RVR_Joeva", 3, true, "ServerSetNodeLevel by cyb");
        control.add_experience("Halcyon", 1200, "ServerAddNodeExperience by cyb");
        control.node_data("Joeva", "ServerGMGetNodeData by cyb");
        control.clock("ServerGMWhatTimeIsIt by cyb");
        control.advance_clock(3600, "ServerGMFastForwardRealTime by cyb");
        control.set_level("Atlantis", 2, false, "ServerSetNodeLevel by cyb");
        control.set_level("Joeva", 9, false, "ServerSetNodeLevel by cyb");
        control.drain();
        CHECK(control.calls_made() == 7, "the seven calls were made, one after the other, on the worker thread");
        {
            std::lock_guard<std::mutex> lock(service.mu);
            CHECK(service.calls.size() == 7 && service.calls[0] == "SetNodeLevel Verra_RVR_Joeva 3 dv" && service.calls[1] == "AddNodeExperience Halcyon 1200" &&
                      service.calls[2] == "GetNode Joeva" && service.calls[3] == "GetClock" && service.calls[4] == "AdvanceClock 3600",
                  "the service got them in order with the node names, the level, the flag and the amounts");
            CHECK(service.token_seen == "Bearer " + token, "every call carries the service token");
        }
        std::lock_guard<std::mutex> lock(lines_mu);
        CHECK(lines.size() == 7, "each call ended in one line");
        if (lines.size() == 7) {
            CHECK(lines[0].find("Joeva") != std::string::npos && lines[0].find("level 3") != std::string::npos && lines[0].find("(changed)") != std::string::npos, "the level change says the node and its new level");
            CHECK(lines[1].find("1,200 XP added") != std::string::npos, "the experience says the amount with its thousands");
            CHECK(lines[2].find("level 4 (Town)") != std::string::npos, "the node data says the level and its name");
            CHECK(lines[3].find("node time 1771000000") != std::string::npos && lines[3].find("speed 2") != std::string::npos, "the clock says the node time and speed");
            CHECK(lines[5].find("refused") != std::string::npos && lines[5].find("no node named Atlantis") != std::string::npos, "an unknown node is refused with the service's reason");
            CHECK(lines[6].find("refused") != std::string::npos && lines[6].find("above the highest") != std::string::npos, "a level the service refuses is refused with its reason");
        }
    }
    server->Shutdown();
    // the service is gone: the console says so
    svc::GrpcNodeControl::Options dead = o;
    dead.address = "127.0.0.1:1";
    dead.deadline_ms = 400;
    std::vector<std::string> dead_lines;
    std::mutex dead_mu;
    {
        svc::GrpcNodeControl control(dead, [&](const std::string& l) {
            std::lock_guard<std::mutex> lock(dead_mu);
            dead_lines.push_back(l);
        });
        control.set_level("Joeva", 3, false, "ServerSetNodeLevel by cyb");
        control.drain();
    }
    std::lock_guard<std::mutex> lock(dead_mu);
    CHECK(dead_lines.size() == 1 && dead_lines[0].find("the node service is unavailable") != std::string::npos, "with the service unreachable the console says \"the node service is unavailable\"");
}

int main() {
    spdlog::set_level(std::getenv("AOC_TEST_LOG") ? spdlog::level::debug : spdlog::level::off);
    std::string error;
    g_nodes = world::CityNodes::load(db_path(), error);
    if (!g_nodes) {
        std::printf("  [FAIL] the real content.db has no city nodes (%s): %s\n", db_path().c_str(), error.c_str());
        return 2;
    }
    g_wire = world::NodeWire::load(db_path(), *g_nodes, error);
    if (!g_wire) {
        std::printf("  [FAIL] the node wire does not load (%s): %s\n", db_path().c_str(), error.c_str());
        return 2;
    }
    test_maps();
    test_summaries();
    test_join();
    test_requests();
    test_level_change();
    test_restore();
    test_console();
    test_control();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
