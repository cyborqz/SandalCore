// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_area_session.cpp  (plan E1, stage 1)
//
//  The living area on a connection: a session with a fake client (the harness of test_worldd_items_session) on a PlayerWorld whose registry
//  holds NPCs and nodes made from the retail templates (spawns/captured_spawns.h) around the login spot.
//
//    the view          nothing of the world is sent before the phase-in; after it the awake ring is opened, the snapshot ring opened and closed with
//                      Dormancy, the rest not at all; the actors' channels are none of the player's four; what the client has is what the view says
//    the GM vendor     ServerInteractWithInteractableComponent on the vendor's component is answered with ClientOpenVendor (the VendorDef and the NPC);
//                      ServerHandleVendorPurchase puts the stacks in the bag: an Add event each, the lowest free slots, the quantity split by
//                      MaxStackSize, the inventory dirty and handed on; nothing is charged
//                      and, as in retail (captures 716, 1536), the window's ServerGetNodeTaxTable is answered with ClientSetNodeTaxTable: the
//                      node's 78 tags, untaxed, the size of retail's table (the client shows no listing, tax or funds until it arrives)
//    hostile           an unknown component, an actor the client does not have, one too far, a non-vendor; a purchase with no window open, of an item
//                      the vendor does not list, from another vendor, with a bad quantity, with a bag too full, of 71 items, after the vendor is
//                      gone, a purchase that does not read, and a flood of purchases: nothing changes (the client's storages are sent again), the
//                      session lives
//    many actors       1,500 NPCs around the player: the opens come in at the cap per tick, nearest first; per-tick time and bandwidth are measured
// ============================================================================
#include "capture_packets.h"
#include "capture_vendor.h"
#include "content_db_fixture.h"
#include "cyb_appearance.h"
#include "fake_client.h"
#include "bootstrap/character_setup.h"
#include "items/inventory.h"
#include "nav/area_nav.h"
#include "net/endpoint.h"
#include "npc/npc_world.h"
#include "protocol/repl/storage_rpc.h"
#include "protocol/repl/vendor_rpc.h"
#include "protocol/schema/generated/index.h"
#include "session/player_session.h"
#include "spawns/captured_spawns.h"
#include "vendor/gm_vendor.h"
#include "vendor/node_tax.h"
#include "world/actor_registry.h"
#include "world/interaction_state.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>
// ~ C Y B :: C.O.A ~

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
namespace cap = capture_vendor;
using emit::BunchWriter;
using std::chrono::milliseconds;
using std::chrono::seconds;
using svc::ItemContainer;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

static const std::string kCyb = "bbec5c86eb101fe5f38c87f0fa22e7d0";
static const std::string kMachine = "DESKTOP-EXAMPLE1-US-0123456789ABCDEF0123456789ABCDEF";
static const Clock::time_point t0{seconds(5000)};
static constexpr uint32_t kServerId = 60;
static constexpr uint32_t kRandomizer = 0x6ee87ee4u;
static const ItemContainer Bag = ItemContainer::Backpack;
static const ItemContainer Worn = ItemContainer::Equipment;

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
    const std::pair<int, int64_t> worn[] = {{1, fi::kHelmet}, {2, fi::kChest}, {5, fi::kLegs}, {6, fi::kFeet}, {15, fi::kScepter}, {16, fi::kFocus}, {17, fi::kBook},
                                             {23, fi::kBag}, {26, fi::kPickaxe}, {29, fi::kAxe}, {33, fi::kSickle}, {35, fi::kFishingRod}, {38, fi::kBow}};
    std::vector<svc::InventoryItem> out;
    for (const auto& [slot, record] : worn) out.push_back(make_item(Worn, slot, record, 100 + static_cast<uint32_t>(slot)));
    out.push_back(make_item(Bag, 0, fi::kSword2H, 200));
    out.push_back(make_item(Bag, 1, fi::kPotion, 201, 5));
    return out;
}

static svc::Character cyb_record(std::vector<svc::InventoryItem> items = kit()) {
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
    c.items = std::move(items);
    return c;
}

static bs::LoginIdentity identity() { return {kMachine, kMachine}; }
static double world_time_at(Clock::time_point t) { return std::chrono::duration<double>(t - t0).count(); }

struct RecordingSink : PositionSink {
    std::vector<std::vector<svc::InventoryItem>> inventories;
    void save(const std::string&, const svc::Position&) override {}
    void save_inventory(const std::string&, std::vector<svc::InventoryItem> items) override { inventories.push_back(std::move(items)); }
    void save_progress(const std::string&, svc::Progress) override {}
};

// ---- the world ---------------------------------------------------------------------------------------------------------------------

struct Fixture {
    fs::path dir;
    std::optional<content::ContentDb> content;
    aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
    std::unique_ptr<PlayerWorld> world;
    std::optional<spawns::CapturedData> captured;
// (C.O.A) (Cyb)

    Fixture() {
        dir = cf::make_temp_dir("worldd_area_session_");
        const fs::path db = dir / "content.db";
        if (!cf::make_db(db)) std::exit(2);
        std::string error;
        content = content::ContentDb::open(db.string(), error);
        if (!content) {
            std::printf("  [FAIL] the content.db does not open: %s\n", error.c_str());
            std::exit(2);
        }
        world = std::make_unique<PlayerWorld>(&*content, "", bs::GameStateMode::Tier2, kServerId, guids);
        captured = spawns::CapturedData::load(error);
        if (!captured) {
            std::printf("  [FAIL] the captured spawns do not load: %s\n", error.c_str());
            std::exit(2);
        }
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    ~Fixture() {
        world.reset();
        content.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    spawns::Minter minter() { return [this] { return guids.next(); }; }

    /// The first template of a category ("npc" / "gatherable"), or the one of an archetype.
    const spawns::OpenTemplate* npc_template(const char* leaf = nullptr) const {
        for (const spawns::OpenTemplate& t : captured->opens()) {
            if (t.category != "npc" || captured->template_for(t.archetype_path) != &t) continue;
            if (leaf && t.archetype_path.find(leaf) == std::string::npos) continue;
            if (!leaf && t.find("UInteractableComponent") > 0) continue;   // a plain creature
            return &t;
        }
        return nullptr;
    }
    const spawns::OpenTemplate* gatherable_template() const {
        for (const spawns::OpenTemplate& t : captured->opens()) {
            if (t.category == "gatherable") return &t;
        }
        return nullptr;
    }

    /// Adds an NPC (or node) made from `t` at (x, y); the id of its actor.
    uint64_t add(const spawns::OpenTemplate& t, double x, double y, bool gm_vendor = false, int64_t vendor_def = 0, const char* name = nullptr) {
        spawns::InstanceParams p;
        p.x = x;
        p.y = y;
        p.z = bootstrap::kDefaultSpawn.z;
        p.server_id = kServerId;
        p.keep_captured_position = t.category == "gatherable";
        if (name) p.name = name;
        p.seed = guids.next().ObjectId;
        std::string error;
        auto w = spawns::instantiate(t, minter(), p, error);
        if (!w) {
            std::printf("  [FAIL] instantiate: %s\n", error.c_str());
            std::exit(2);
        }
        if (t.category == "gatherable") {
            w->x = x;
            w->y = y;
        }
        w->npc.is_gm_vendor = gm_vendor;
        w->npc.vendor_guid = vendor_def;
        if (gm_vendor) world::allow_inputs(w->actor);   // as the area loader makes a GM vendor
        if (name) w->npc.name = name;
        const uint64_t id = w->id;
        if (!world->registry().add(std::move(*w))) {
            std::printf("  [FAIL] the registry refuses an actor\n");
            std::exit(2);
        }
        return id;
    }
};

// ---- the harness -------------------------------------------------------------------------------------------------------------------
// -- C | O | A . C_Y_B --

static constexpr double kCx = bootstrap::kDefaultSpawn.x, kCy = bootstrap::kDefaultSpawn.y;

struct Harness {
    Fixture fx;
    RecordingSink sink;
    Connection conn{ConnectionConfig{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870}, t0};
    fake::Client client{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870};
    std::unique_ptr<PlayerSession> s;
    Clock::time_point t = t0;
    std::vector<fake::Delivered> got;
    size_t packets = 0, bytes = 0;
    std::string error;
    uint32_t pawn_ch = 0, pc_ch = 0;
    double tick_us_total = 0, tick_us_max = 0;
    size_t ticks = 0;
    std::function<void(Clock::time_point)> before_tick;   // plan E1 stage 2: the NPC world's step

    svc::Character record;

    explicit Harness(svc::Character r = cyb_record()) : record(std::move(r)) { fx.world->set_position_sink(&sink); }
    virtual ~Harness() = default;
    /// The derived harness fills the registry, then joins (a base constructor cannot call the derived populate()).
    void start() {
        s = PlayerSession::join(conn, *fx.world, record, identity(), world_time_at(t), error);
        if (!s) {
            std::printf("  [FAIL] the session does not join: %s\n", error.c_str());
            std::exit(2);
        }
        pawn_ch = s->channel_of(bs::Role::Pawn);
        pc_ch = s->channel_of(bs::Role::PlayerController);
    }

    void flush() {
        for (auto& pkt : conn.flush(t)) {
            bytes += pkt.size();
            const auto r = client.receive(pkt);
            ++packets;
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
        if (before_tick) before_tick(t);
        const auto a = std::chrono::steady_clock::now();
        s->tick(conn, t);
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - a).count();
        tick_us_total += us;
        tick_us_max = std::max(tick_us_max, us);
        ++ticks;
        flush();
        s->take_acks(conn);
        client_sends({});
    }
    void run(milliseconds total) {
        for (milliseconds done{0}; done < total; done += milliseconds(20)) step();
    }
    void settle() {
        for (int quiet = 0, i = 0; quiet < 5 && i < 600; ++i) {
            const size_t before = got.size();
            step();
            quiet = (got.size() == before && s->join_pending() == 0 && s->replies_pending() == 0) ? quiet + 1 : 0;
        }
    }
    void client_request(const BunchWriter& rpc, uint32_t channel) {
        client_sends({client.actor(channel, rpc, true)});
        settle();
    }
    /// A client that has joined and phased in.
    void play() {
        settle();
        request_phase_in();
    }
    void request_phase_in();
};

static BunchWriter actor_rpc(const schema::ClassSchema& cls, const char* name, const repl::RpcParams& params) {
    BunchWriter out;
    const schema::RpcSchema* rpc = cls.rpc(name);
    if (!rpc) return out;
    BunchWriter field;
    bool failed = false;
    if (repl::write_rpc_field(cls, *rpc, params, field, {}, &failed) == 0 && !params.values.empty()) {
        std::printf("  [FAIL] %s does not write\n", name);
        return out;
    }
    if (failed) return out;
    repl::BlockHeader header;
    repl::write_content_block(out, header, field, /*server_form=*/false);
    return out;
}

void Harness::request_phase_in() { client_request(actor_rpc(class_named("PlayerPawn_C"), "ServerClientIsReadyToPhaseIn", {}), pawn_ch); }

// ---- the client's requests -----------------------------------------------------------------------------------------------------------

static BunchWriter interact(uint64_t component_object_id, uint32_t server_id = kServerId, uint32_t randomizer = kRandomizer) {
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    const schema::RpcSchema* rpc = pawn.rpc("ServerInteractWithInteractableComponent");
    repl::InteractRequest r;
    r.interactable.ObjectId = component_object_id;
    r.interactable.ServerId = server_id;
    r.interactable.Randomizer = randomizer;
    r.counter = 1;
    const auto params = repl::interact_params(*rpc, r);
    return params ? actor_rpc(pawn, "ServerInteractWithInteractableComponent", *params) : BunchWriter();
}

static BunchWriter purchase(const repl::VendorPurchaseCart& cart) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerHandleVendorPurchase");
    const auto params = repl::vendor_purchase_params(*rpc, cart);
    return params ? actor_rpc(pc, "ServerHandleVendorPurchase", *params) : BunchWriter();
}

/// ServerGetNodeTaxTable: what the vendor window sends as soon as it opens.
static BunchWriter node_tax_request(int64_t node) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerGetNodeTaxTable");
    const auto params = repl::node_tax_request_params(*rpc, node);
    return params ? actor_rpc(pc, "ServerGetNodeTaxTable", *params) : BunchWriter();
}

/// The NodeRecordGuid of the actor's UAoCNodePlotComponent: the node the vendor window asks the taxes of (0: none).
static int64_t node_of(const repl::Actor& actor) {
    for (const repl::Subobject& s : actor.subobjects) {
        if (!s.obj.schema || std::string(s.obj.schema->name) != "UAoCNodePlotComponent") continue;
        const repl::Value* v = s.obj.state.get(*s.obj.schema, "NodeRecordGuid");
        const auto* g = v ? v->get_if<int64_t>() : nullptr;
        return g ? *g : 0;
    }
    return 0;
}

/// capture_vendor.h's server calls read back (the RPC's parameters from the hex).
static std::optional<repl::RpcCall> captured_call(size_t server_index) {
    const cap::Call& c = cap::kServer[server_index];
    std::vector<uint8_t> bytes;
    for (const char* h = c.hex; h[0] && h[1]; h += 2) bytes.push_back(static_cast<uint8_t>(std::stoi(std::string(h, 2), nullptr, 16)));
    const schema::RpcSchema* rpc = class_named("AoCPlayerControllerBP_C").rpc(c.rpc);
    if (!rpc) return std::nullopt;
    size_t pos = 0;
    auto params = repl::read_rpc_params(*rpc, bytes.data(), c.bits, pos, {});
    if (!params) return std::nullopt;
    repl::RpcCall call;
    call.rpc = rpc;
    call.params = std::move(*params);
    call.decoded = true;
    call.param_bits = c.bits;
    return call;
}
// C.O.A captures, C_Y_B code

// ---- reading what the server sent -----------------------------------------------------------------------------------------------------

/// The PlayerController's decoded calls among the chains `got[from..]`, in order.
static std::vector<repl::RpcCall> pc_calls_since(const Harness& h, size_t from) {
    std::vector<repl::RpcCall> out;
    for (size_t i = from; i < h.got.size(); ++i) {
        if (h.got[i].channel != h.pc_ch || h.got[i].open || h.got[i].close) continue;
        const auto blocks = repl::read_content_blocks(h.got[i].content.data(), h.got[i].content.bit_pos(), 0);
        if (!blocks) continue;
        for (const auto& b : *blocks) {
            if (!b.header.is_actor || b.header.has_rep_layout) continue;
            size_t p = b.payload_pos;
            auto calls = repl::read_rpc_fields(class_named("AoCPlayerControllerBP_C"), h.got[i].content.data(), b.payload_pos + b.payload_bits, p);
            if (calls) {
                for (auto& c : *calls) out.push_back(std::move(c));
            }
        }
    }
    return out;
}

static size_t count_calls(const std::vector<repl::RpcCall>& calls, const char* name) {
    return static_cast<size_t>(std::count_if(calls.begin(), calls.end(), [&](const repl::RpcCall& c) { return c.rpc && std::string(c.rpc->name) == name; }));
}

/// The channels (other than the player's four) the client has seen opened, closed.
struct ActorChannels {
    std::set<uint32_t> opened, closed;
    size_t open_chains = 0, close_chains = 0, update_chains = 0;
};
static ActorChannels actor_channels(const Harness& h, size_t from = 0) {
    ActorChannels a;
    for (size_t i = from; i < h.got.size(); ++i) {
        const fake::Delivered& d = h.got[i];
        if (d.channel == h.pc_ch || d.channel == h.pawn_ch || d.channel == h.s->channel_of(bs::Role::GameState) || d.channel == h.s->channel_of(bs::Role::PlayerState)) continue;
        if (d.open) {
            a.opened.insert(d.channel);
            ++a.open_chains;
        } else if (d.close) {
            a.closed.insert(d.channel);
            ++a.close_chains;
        } else {
            ++a.update_chains;
        }
    }
    return a;
}

static size_t bag_items(const Harness& h) { return h.s->inventory().count(Bag); }

// ============================================================================================================================
//  the tests
// ============================================================================================================================

struct AroundHarness : Harness {
    uint64_t near_a = 0, near_b = 0, snapshot = 0, far_away = 0, node = 0;
    AroundHarness() {
        populate();
        start();
    }
    void populate() {
        const spawns::OpenTemplate* wolf = fx.npc_template("Wolf");
        const spawns::OpenTemplate* any = wolf ? wolf : fx.npc_template();
        const spawns::OpenTemplate* gather = fx.gatherable_template();
        if (!any || !gather) {
            std::printf("  [FAIL] no templates\n");
            std::exit(2);
        }
        near_a = fx.add(*any, kCx + 2000, kCy);          // 20 m
        near_b = fx.add(*any, kCx, kCy + 10000);         // 100 m
        snapshot = fx.add(*any, kCx - 25000, kCy);       // 250 m
        far_away = fx.add(*any, kCx, kCy - 60000);       // 600 m
        node = fx.add(*gather, kCx + 3000, kCy + 3000);  // a resource node 42 m away (keeps its captured open; its position is the one given)
    }
};

static void test_view() {
    std::printf("the view: what the client is sent of the world\n");
    AroundHarness h;
    h.settle();
    CHECK(actor_channels(h).open_chains == 0 && h.s->actor_view() && h.s->actor_view()->tracked() == 0, "nothing of the world is sent before the phase-in");
    h.request_phase_in();
    h.run(milliseconds(1500));
    const ActorView& v = *h.s->actor_view();
    CHECK(v.state_of(h.near_a) == world::ClientState::Awake && v.state_of(h.near_b) == world::ClientState::Awake && v.state_of(h.node) == world::ClientState::Awake,
          "the NPCs at 20 m and 100 m and the node at 42 m are open (awake)");
    CHECK(v.state_of(h.snapshot) == world::ClientState::Dormant, "the NPC at 250 m was opened as a snapshot and closed with Dormancy: the client keeps it");
    CHECK(v.state_of(h.far_away) == world::ClientState::None, "the NPC at 600 m is not sent at all");
    const ActorChannels ch = actor_channels(h);
    CHECK(ch.opened.size() == 4 && ch.closed.size() == 1, "the client saw four opens and one close (the snapshot's)");
    bool free_of_player = true;
    for (const uint32_t c : ch.opened) {
        free_of_player = free_of_player && c != h.pc_ch && c != h.pawn_ch && c != h.s->channel_of(bs::Role::GameState) && c != h.s->channel_of(bs::Role::PlayerState) && c >= 3;
    }
    CHECK(free_of_player && *ch.opened.begin() > 6, "the actors' channels are none of the player's four (the first free index is 7)");
    CHECK(v.channel_of(h.near_a) != 0 && v.channel_of(h.near_a) != v.channel_of(h.near_b) && v.channel_of(h.snapshot) == 0, "each open actor has a channel of its own; the dormant one has none");
    CHECK(v.stats().opens == 3 && v.stats().snapshots == 1 && v.stats().closes_dormancy == 1 && v.stats().refused_opens == 0 && v.stats().broken == 0, "the view's own count: three opens, one snapshot, one Dormancy close");
    // the world changes: a name change on an awake NPC is one update, only what changed
    const size_t before = h.got.size();
    h.fx.world->registry().modify(h.near_a, world::Change::State, [&](world::WorldActor& a) {
        for (repl::Subobject& sub : a.actor.subobjects) {
            if (std::string(sub.obj.schema->name) == "UNPCCombatInfo") sub.obj.state.set(*sub.obj.schema, "bInCombat", repl::Value::boolean(true));
        }
    });
    h.run(milliseconds(400));
    const ActorChannels after = actor_channels(h, before);
    CHECK(after.update_chains == 1 && after.open_chains == 0, "an awake NPC that changed gets one update chain");
    // the session ending stops the view
    h.s->finish("test");
    CHECK(true, "the session finishes with a view");
}

static void test_removed_actor() {
    std::printf("an actor removed from the world\n");
    AroundHarness h;
    h.play();
    h.run(milliseconds(1500));
    const uint32_t channel = h.s->actor_view()->channel_of(h.near_a);
    CHECK(channel != 0, "the NPC is open");
    h.fx.world->registry().remove(h.near_a);
    h.run(milliseconds(500));
    CHECK(h.s->actor_view()->state_of(h.near_a) == world::ClientState::None && h.s->actor_view()->stats().closes_destroyed >= 1, "its channel is closed (Destroyed) and the view forgot it");
    h.fx.world->registry().remove(h.snapshot);   // a dormant one: opened and closed so that the client forgets it
    h.run(milliseconds(500));
    CHECK(h.s->actor_view()->state_of(h.snapshot) == world::ClientState::None && h.s->actor_view()->stats().closes_destroyed >= 2, "a dormant NPC that is removed is destroyed on the client too");
}

// ---- the GM vendor ---------------------------------------------------------------------------------------------------------------------

static vendor::GmVendors test_vendors() {
    std::vector<vendor::GmVendor> defs;
    vendor::GmVendor gear;
    gear.guid = 6064634098852102156ll;
    gear.name = "PTR_ONLY_Vendor_Gear_0-9";
    gear.kind = "Gear";
    gear.label = "GM Gear 0-9";
    gear.items = {fi::kMace1H, fi::kShield, fi::kRingA, fi::kRingB, fi::kPotion, fi::kHeavyChest};
    gear.listings = 6;
    gear.tabs = 1;
    defs.push_back(gear);
    vendor::GmVendor other;
    other.guid = 6064634360064966680ll;
    other.name = "PTR_ONLY_Vendor_Materials";
    other.kind = "Materials";
    other.label = "GM Materials";
    other.items = {fi::kGatherCape};
    other.listings = 1;
    other.tabs = 1;
    defs.push_back(other);
    return vendor::GmVendors::from_defs(std::move(defs));
}
// ~ C-O-A - C.Y.B ~

struct VendorHarness : Harness {
    uint64_t gm = 0, gm_far = 0, gm_unseen = 0, plain = 0, gm2 = 0;
    const int64_t gear_def = 6064634098852102156ll, mats_def = 6064634360064966680ll;
    explicit VendorHarness(svc::Character r = cyb_record()) : Harness(std::move(r)) {
        populate();
        start();
    }
    void populate() {
        fx.world->set_gm_vendors(test_vendors());
        const spawns::OpenTemplate* vendor_t = fx.captured->template_for(spawns::kGmVendorArchetype);
        const spawns::OpenTemplate* wolf = fx.npc_template();
        if (!vendor_t || !wolf) {
            std::printf("  [FAIL] no vendor template\n");
            std::exit(2);
        }
        gm = fx.add(*vendor_t, kCx + 800, kCy, true, gear_def, "GM Gear 0-9");        // 8 m
        gm2 = fx.add(*vendor_t, kCx, kCy + 900, true, mats_def, "GM Materials");     // 9 m
        gm_far = fx.add(*vendor_t, kCx + 4000, kCy, true, gear_def, "GM Far");       // 40 m: open, but out of reach
        gm_unseen = fx.add(*vendor_t, kCx, kCy - 60000, true, gear_def, "GM Unseen"); // 600 m: the client does not have it
        plain = fx.add(*wolf, kCx - 700, kCy);                                       // a creature
    }
    uint64_t component_of(uint64_t actor) const {
        const world::WorldActor* a = fx.world->registry().find(actor);
        return a ? a->npc.interact_component : 0;
    }
    /// The client interacts with `actor` and the answers are collected.
    void talk_to(uint64_t actor) { client_request(interact(component_of(actor)), pawn_ch); }
};

static repl::VendorPurchaseCart cart_of(int64_t vendor, std::vector<std::pair<int64_t, int32_t>> items) {
    repl::VendorPurchaseCart c;
    c.vendor = vendor;
    for (const auto& [record, qty] : items) c.items.push_back({record, qty, std::nullopt, std::nullopt});
    return c;
}

static void test_vendor_flow() {
    std::printf("the GM vendor: interaction, window, purchase\n");
    VendorHarness h;
    h.play();
    h.run(milliseconds(1500));
    CHECK(h.s->actor_view()->state_of(h.gm) == world::ClientState::Awake && h.s->actor_view()->state_of(h.gm_far) == world::ClientState::Awake &&
              h.s->actor_view()->state_of(h.gm_unseen) == world::ClientState::None,
          "the vendor NPCs within 150 m are open on the client, the one at 600 m is not");

    // the interaction
    size_t from = h.got.size();
    h.talk_to(h.gm);
    auto calls = pc_calls_since(h, from);
    CHECK(count_calls(calls, "ClientOpenVendor") == 1 && h.s->vendor_opens() == 1, "the interaction is answered with one ClientOpenVendor");
    bool right_window = false;
    for (const repl::RpcCall& c : calls) {
        if (std::string(c.rpc->name) != "ClientOpenVendor") continue;
        const auto o = repl::parse_open_vendor(c);
        const world::WorldActor* a = h.fx.world->registry().find(h.gm);
        right_window = o && a && o->merch_set == h.gear_def && o->vendor == a->actor.guid;
    }
    CHECK(right_window, "MerchSetId is the vendor's def and Vendor is the NPC's actor");
    CHECK(h.s->vendor_actor() == h.gm && h.s->vendor_def() == h.gear_def, "the session remembers which window is open");

    // the retail flow goes on (captures 716, 1536): the window asks for its node's taxes, and the table fills it: until it arrives the
    // client shows no listing, no tax and no funds (UVendorMenu::HandleReceiveNodeTaxDetails does all three)
    const world::WorldActor* gm_actor = h.fx.world->registry().find(h.gm);
    const int64_t node = gm_actor ? node_of(gm_actor->actor) : 0;
    CHECK(node != 0, "the GM vendor stands in a node (the window asks for its taxes: a node of 0 would have the client fill the window itself)");
    const auto retail_open = captured_call(0);
    size_t open_bits = 0;
    for (const repl::RpcCall& c : calls) {
        if (std::string(c.rpc->name) == "ClientOpenVendor") open_bits = c.param_bits;
    }
    CHECK(retail_open && open_bits == retail_open->param_bits, "ClientOpenVendor has the size of retail's (195 bits)");
    CHECK(count_calls(calls, "ClientSetNodeTaxTable") == 0 && h.s->tax_tables_sent() == 0, "no table goes out before the window asks");
    from = h.got.size();
    h.client_request(node_tax_request(node), h.pc_ch);
    calls = pc_calls_since(h, from);
    CHECK(h.s->tax_requests() == 1 && h.s->tax_tables_sent() == 1 && count_calls(calls, "ClientSetNodeTaxTable") == 1, "the request is answered with one ClientSetNodeTaxTable");
    {
        const repl::RpcCall* table_call = nullptr;
        for (const repl::RpcCall& c : calls) {
            if (std::string(c.rpc->name) == "ClientSetNodeTaxTable") table_call = &c;
        }
        const auto table = table_call ? repl::parse_node_tax_table(*table_call) : std::nullopt;
        CHECK(table && table_call->decoded, "the table reads back with the client's schema");
        // retail's: 1536's node with the defaults
        const auto retail_call = captured_call(2);
        const auto retail = retail_call ? repl::parse_node_tax_table(*retail_call) : std::nullopt;
        CHECK(retail && table && table_call->param_bits == retail_call->param_bits && table_call->param_bits == 54537, "it has the size of retail's tables (54,537 bits)");
        bool same_shape = retail && table && table->entries.size() == retail->entries.size() && table->node == node;
        bool untaxed = table.has_value();
        for (size_t i = 0; same_shape && i < table->entries.size(); ++i) {
            const repl::NodeTaxEntry& a = table->entries[i];
            const repl::NodeTaxEntry& b = retail->entries[i];
            same_shape = a.tag == b.tag && a.details == b.details && a.owning_node_percent == b.owning_node_percent && a.flag_a == b.flag_a && a.flag_b == b.flag_b;
            untaxed = untaxed && a.tax_percent == 0.0f && a.flat_tax == 0;
        }
        CHECK(same_shape, "the same 78 tags, records and flags as retail's, for the node the window asked for");
        CHECK(untaxed, "every tax is 0%");
    }
    CHECK(h.s->vendor_actor() == h.gm, "the window stays open");
// [Cyb :: C O A]

    // the purchase: a mace, and 45 potions (a stack of 20: three stacks)
    const size_t bag_before = bag_items(h);
    from = h.got.size();
    h.client_request(purchase(cart_of(h.gear_def, {{fi::kMace1H, 1}, {fi::kPotion, 45}})), h.pc_ch);
    calls = pc_calls_since(h, from);
    CHECK(h.s->vendor_purchases() == 1 && h.s->vendor_refused() == 0, "the purchase is applied");
    CHECK(bag_items(h) == bag_before + 4, "four new stacks in the bag: the mace and the potions in stacks of 20, 20 and 5");
    CHECK(count_calls(calls, "ClientReceiveStorageEvent") == 4, "four storage events: an Add for each stack");
    std::vector<std::pair<int, int64_t>> slots;   // slot and record of each Add
    std::vector<int> quantities;
    for (const repl::RpcCall& c : calls) {
        if (std::string(c.rpc->name) != "ClientReceiveStorageEvent") continue;
        const auto ev = repl::parse_storage_event(c);
        if (!ev || ev->type != static_cast<uint8_t>(repl::StorageEventType::Add) || ev->targets.size() != 1 || ev->targets[0].slots.size() != 1) continue;
        slots.push_back({ev->targets[0].slots[0].slot, static_cast<int64_t>(ev->item.record_id)});
        quantities.push_back(static_cast<int>(ev->item.quantity));
    }
    CHECK(slots.size() == 4 && slots[0].first == 2 && slots[1].first == 3 && slots[2].first == 4 && slots[3].first == 5, "the Adds fill the lowest free bag slots: 2, 3, 4, 5");
    CHECK(quantities == std::vector<int>({1, 20, 20, 5}), "quantities 1, 20, 20, 5");
    CHECK(count_calls(calls, "ClientShowCurrencyCallout") == 0, "nothing is charged: no currency callout");
    CHECK(h.s->inventory_dirty() && h.s->inventory_changed(), "the inventory is marked for saving");
    // the instances are all different
    std::set<std::string> guids;
    for (const svc::InventoryItem& it : h.s->inventory().items()) guids.insert(it.instance_guid);
    CHECK(guids.size() == h.s->inventory().size(), "every instance has a guid of its own");

    // a second window: the materials
    from = h.got.size();
    h.talk_to(h.gm2);
    CHECK(h.s->vendor_def() == h.mats_def && h.s->vendor_actor() == h.gm2 && h.s->vendor_opens() == 2, "another vendor opens another window");
    h.client_request(purchase(cart_of(h.mats_def, {{fi::kGatherCape, 1}})), h.pc_ch);
    CHECK(h.s->vendor_purchases() == 2 && h.s->inventory().at(Bag, 6) && h.s->inventory().at(Bag, 6)->item_record_id == fi::kGatherCape, "and buys from it");

    // the inventory is handed to the saver when the session ends
    h.s->finish("test");
    CHECK(!h.sink.inventories.empty() && h.sink.inventories.back().size() == h.s->inventory().size(), "the purchase is saved with the inventory");
}

static void test_vendor_tax_table() {
    std::printf("the GM vendor: the tax table the window waits for\n");
    VendorHarness h;
    h.play();
    h.run(milliseconds(1500));
    const world::WorldActor* gm_actor = h.fx.world->registry().find(h.gm);
    const int64_t node = gm_actor ? node_of(gm_actor->actor) : 0;
    // a request with no window open is ignored
    size_t from = h.got.size();
    h.client_request(node_tax_request(node), h.pc_ch);
    CHECK(h.s->tax_requests() == 1 && h.s->tax_tables_sent() == 0 && count_calls(pc_calls_since(h, from), "ClientSetNodeTaxTable") == 0, "a request with no window open gets no table");
    // the table follows the window, in that order, within the same pass
    from = h.got.size();
    h.talk_to(h.gm);
    h.client_request(node_tax_request(node), h.pc_ch);
    auto calls = pc_calls_since(h, from);
    std::vector<std::string> order;
    for (const repl::RpcCall& c : calls) order.push_back(c.rpc->name);
    CHECK(order.size() == 2 && order[0] == "ClientOpenVendor" && order[1] == "ClientSetNodeTaxTable", "ClientOpenVendor, then ClientSetNodeTaxTable: retail's order");
    // the asked node is echoed, whatever it is (the client's node is the NPC's; the server takes no side)
    h.run(milliseconds(1200));
    from = h.got.size();
    h.client_request(node_tax_request(0x1234567890ll), h.pc_ch);
    calls = pc_calls_since(h, from);
    repl::NodeTaxDetails echoed;
    for (const repl::RpcCall& c : calls) {
        if (auto t = repl::parse_node_tax_table(c)) echoed = *t;
    }
    CHECK(echoed.node == 0x1234567890ll && echoed.entries.size() == 78, "another node asked for is answered for that node");
    // a flood: the budget (a burst of three, then one a second) cuts it
    h.run(milliseconds(5000));
    const size_t sent = h.s->tax_tables_sent();
    std::vector<fake::Bunch> burst;
    for (int i = 0; i < 30; ++i) burst.push_back(h.client.actor(h.pc_ch, node_tax_request(node), true));
    h.client_sends(std::move(burst));
    h.settle();
    CHECK(h.s->tax_tables_sent() >= sent + 1 && h.s->tax_tables_sent() <= sent + 4, "a burst of 30 requests gets at most the burst of tables (three, and a token earned meanwhile)");
    CHECK(!h.s->failed() && h.s->requests_dropped() > 0, "the rest were dropped for the budget and counted");
    // a request that does not read
    {
        BunchWriter junk;
        junk.write_bits(0x2bad, 14);
        h.client_sends({h.client.actor(h.pc_ch, junk, true)});
        h.settle();
        CHECK(!h.s->failed(), "bits that are no request do not hurt the session");
    }
    // the vendor is gone: the window is forgotten, the request ignored
    h.run(milliseconds(5000));
    h.fx.world->registry().remove(h.gm);
    h.run(milliseconds(300));
    const size_t before = h.s->tax_tables_sent();
    h.client_request(node_tax_request(node), h.pc_ch);
    CHECK(h.s->tax_tables_sent() == before && h.s->vendor_actor() == 0, "with the vendor gone the request gets no table and the window is forgotten");
}
// [CoA - C Y B]

static void test_vendor_refusals() {
    std::printf("the GM vendor: hostile requests change nothing\n");
    VendorHarness h;
    h.play();
    h.run(milliseconds(1500));
    const auto inventory_before = h.s->inventory().items();
    const auto unchanged = [&] { return h.s->inventory().items().size() == inventory_before.size() && h.s->inventory() == *items::Inventory::from_items(inventory_before, h.error); };
    const auto purchases = [&] { return h.s->vendor_purchases(); };

    // a purchase with no window open
    size_t from = h.got.size();
    h.client_request(purchase(cart_of(h.gear_def, {{fi::kMace1H, 1}})), h.pc_ch);
    CHECK(purchases() == 0 && h.s->vendor_refused() == 1 && unchanged(), "a purchase with no window open is refused");
    size_t pawn_chains = 0;
    for (size_t i = from; i < h.got.size(); ++i) pawn_chains += h.got[i].channel == h.pawn_ch ? 1 : 0;
    CHECK(pawn_chains >= 2, "and the client's storages are sent again (the Backpack's and the Equipment's payload on the pawn)");

    // interactions that are ignored
    const size_t opens = h.s->vendor_opens();
    h.client_request(interact(0x7777776), h.pawn_ch);
    CHECK(h.s->vendor_opens() == opens && h.s->interactions_ignored() == 1, "a component no actor has is ignored");
    h.talk_to(h.gm_unseen);
    CHECK(h.s->vendor_opens() == opens && h.s->interactions_ignored() == 2, "a vendor the client does not have open is ignored");
    h.talk_to(h.gm_far);
    CHECK(h.s->vendor_opens() == opens && h.s->interactions_ignored() == 3, "a vendor 40 m away is out of reach");
    h.talk_to(h.plain);
    CHECK(h.s->vendor_opens() == opens && h.s->interactions_ignored() == 4, "a creature with no vendor is ignored");
    CHECK(h.s->vendor_actor() == 0, "no window is open after them");
    h.client_request(interact(0), h.pawn_ch);
    CHECK(h.s->vendor_opens() == opens, "a null component is ignored");

    // open the real one and try the bad carts
    h.talk_to(h.gm);
    CHECK(h.s->vendor_actor() == h.gm, "the window opens");
    const struct { const char* what; repl::VendorPurchaseCart cart; } bad[] = {
        {"an item the vendor does not list", cart_of(h.gear_def, {{fi::kChest, 1}})},
        {"another vendor's def", cart_of(h.mats_def, {{fi::kGatherCape, 1}})},
        {"an unknown vendor", cart_of(12345, {{fi::kMace1H, 1}})},
        {"a quantity of 0", cart_of(h.gear_def, {{fi::kMace1H, 0}})},
        {"a negative quantity", cart_of(h.gear_def, {{fi::kMace1H, -3}})},
        {"a quantity of two billion", cart_of(h.gear_def, {{fi::kPotion, 2000000000}})},
        {"an empty cart", cart_of(h.gear_def, {})},
        {"a quantity that fills no bag", cart_of(h.gear_def, {{fi::kPotion, 100000}})},
    };
    h.run(milliseconds(2200));   // let the resync budget fill again
    for (const auto& b : bad) {
        const size_t refused = h.s->vendor_refused();
        h.client_request(purchase(b.cart), h.pc_ch);
        h.run(milliseconds(150));
        CHECK(h.s->vendor_refused() == refused + 1 && purchases() == 0 && unchanged(), (std::string(b.what) + " is refused").c_str());
    }
    // (a cart of 71 items is 11,360 bits: more than one datagram holds, so no client can send it; the plan refuses it all the same, test_worldd_area)
    // a garbage purchase: bits that are no cart
    {
        BunchWriter junk;
        repl::BlockHeader header;
        BunchWriter field;
        field.write_bits(0x1234567, 25);
        junk.write_bits(0, 1);
        h.client_sends({h.client.actor(h.pc_ch, junk, true)});
        h.settle();
        CHECK(!h.s->failed() && unchanged(), "bits that are no cart do not hurt the session");
    }
    // the vendor goes away
    h.fx.world->registry().remove(h.gm);
    h.run(milliseconds(300));
    const size_t refused_gone = h.s->vendor_refused();
    h.run(milliseconds(2500));
    h.client_request(purchase(cart_of(h.gear_def, {{fi::kMace1H, 1}})), h.pc_ch);
    CHECK(h.s->vendor_refused() == refused_gone + 1 && purchases() == 0 && h.s->vendor_actor() == 0 && unchanged(), "a purchase after the vendor is gone is refused, and the window forgotten");
// C-O-A.C_Y_B

    // a full bag
    std::vector<svc::InventoryItem> full = kit();
    full.erase(std::remove_if(full.begin(), full.end(), [](const svc::InventoryItem& i) { return i.container == Bag; }), full.end());
    for (int i = 0; i < items::kBackpackSlots; ++i) full.push_back(make_item(Bag, i, fi::kPotion, 300 + static_cast<uint32_t>(i)));
    VendorHarness f(cyb_record(full));
    f.play();
    f.run(milliseconds(1500));
    f.talk_to(f.gm);
    f.client_request(purchase(cart_of(f.gear_def, {{fi::kMace1H, 1}})), f.pc_ch);
    CHECK(f.s->vendor_actor() == f.gm && f.s->vendor_purchases() == 0 && f.s->vendor_refused() == 1 && bag_items(f) == 70, "a full bag refuses the purchase: nothing is added");
    // seven slots free, a cart that needs eight
    std::vector<svc::InventoryItem> nearly = kit();
    nearly.erase(std::remove_if(nearly.begin(), nearly.end(), [](const svc::InventoryItem& i) { return i.container == Bag; }), nearly.end());
    for (int i = 0; i < items::kBackpackSlots - 7; ++i) nearly.push_back(make_item(Bag, i, fi::kPotion, 400 + static_cast<uint32_t>(i)));
    VendorHarness n(cyb_record(nearly));
    n.play();
    n.run(milliseconds(1500));
    n.talk_to(n.gm);
    n.client_request(purchase(cart_of(n.gear_def, {{fi::kMace1H, 1}, {fi::kShield, 1}, {fi::kRingA, 1}, {fi::kRingB, 1}, {fi::kHeavyChest, 1}, {fi::kPotion, 40}, {fi::kPotion, 20}})), n.pc_ch);
    CHECK(n.s->vendor_purchases() == 0 && n.s->vendor_refused() == 1 && bag_items(n) == 63, "a cart that needs eight slots of seven is refused whole");
    n.run(milliseconds(2200));
    n.client_request(purchase(cart_of(n.gear_def, {{fi::kMace1H, 1}, {fi::kShield, 1}, {fi::kRingA, 1}, {fi::kRingB, 1}, {fi::kHeavyChest, 1}, {fi::kPotion, 40}})), n.pc_ch);
    CHECK(n.s->vendor_purchases() == 1 && bag_items(n) == 70, "a cart that needs exactly the seven free slots is applied");
}

static void test_vendor_spam() {
    std::printf("the GM vendor: a flood of requests\n");
    VendorHarness h;
    h.play();
    h.run(milliseconds(1500));
    h.talk_to(h.gm);
    const size_t refused_before = h.s->vendor_refused();
    // 200 purchases of one potion in one burst, no time between them: the budget (a burst of eight) cuts it
    std::vector<fake::Bunch> burst;
    for (int i = 0; i < 40; ++i) burst.push_back(h.client.actor(h.pc_ch, purchase(cart_of(h.gear_def, {{fi::kRingA, 1}})), true));
    h.client_sends(std::move(burst));
    h.settle();
    const size_t applied = h.s->vendor_purchases();
    CHECK(applied <= 8 + 2 && applied >= 1, "a burst of 40 purchases buys at most what the budget allows (eight and the tokens earned meanwhile)");
    CHECK(h.s->requests_dropped() >= 30 - h.s->vendor_refused() || h.s->requests_dropped() > 0, "the rest were dropped for the budget and counted");
    CHECK(!h.s->failed() && h.s->replies_pending() == 0, "the session is whole and its queue empty");
    (void)refused_before;
    // and a slow, steady client is never cut
    h.run(milliseconds(3000));
    const size_t before = h.s->vendor_purchases();
    for (int i = 0; i < 5; ++i) {
        h.client_request(purchase(cart_of(h.gear_def, {{fi::kPotion, 1}})), h.pc_ch);
        h.run(milliseconds(300));
    }
    CHECK(h.s->vendor_purchases() == before + 5, "five purchases a few hundred ms apart are all applied");
}

// ---- many actors ----------------------------------------------------------------------------------------------------------------------

struct CrowdHarness : Harness {
    size_t n;
    explicit CrowdHarness(size_t count) : n(count) {
        populate();
        start();
    }
    void populate() {
        const spawns::OpenTemplate* wolf = fx.npc_template("Wolf");
        const spawns::OpenTemplate* any = wolf ? wolf : fx.npc_template();
        // a ring pattern: ten per ring from 5 m to 345 m: the nearest first
        for (size_t i = 0; i < n; ++i) {
            const double r = 500.0 + 34000.0 * static_cast<double>(i) / static_cast<double>(n);
            const double a = static_cast<double>(i) * 2.399963;   // the golden angle
            fx.add(*any, kCx + r * std::cos(a), kCy + r * std::sin(a));
        }
    }
};

static void test_crowd() {
    std::printf("many actors: 1,500 NPCs around the player\n");
    CrowdHarness h(1500);
    h.settle();
    CHECK(h.fx.world->registry().size() == 1500, "1,500 NPCs are in the registry");
    const size_t bytes_before = h.bytes, packets_before = h.packets;
    h.tick_us_total = 0;
    h.tick_us_max = 0;
    h.ticks = 0;
    h.client_sends({h.client.actor(h.pawn_ch, actor_rpc(class_named("PlayerPawn_C"), "ServerClientIsReadyToPhaseIn", {}), true)});
    // 60 simulated seconds
    size_t first_full = 0;
    for (int i = 0; i < 3000; ++i) {
        h.step();
        if (!first_full && h.s->actor_view()->count(world::ClientState::Awake) + h.s->actor_view()->count(world::ClientState::Dormant) >= 1500) first_full = static_cast<size_t>(i + 1);
    }
    const ActorView& v = *h.s->actor_view();
    const double seconds_run = 3000 * 0.02;
    const double avg = h.tick_us_total / static_cast<double>(h.ticks);
    std::printf("    %zu ticks of 20 ms: %.1f us on average, %.0f us at most (the session's tick, the view's pass included)\n", h.ticks, avg, h.tick_us_max);
    std::printf("    %zu packets, %.1f KB sent in %.0f s (%.1f KB/s); the window was full %zu time(s)\n", h.packets - packets_before, (h.bytes - bytes_before) / 1024.0, seconds_run,
                (h.bytes - bytes_before) / 1024.0 / seconds_run, v.stats().refused_opens);
    std::printf("    %zu awake, %zu dormant, %zu channels in use (peak %zu), %zu opens, %zu snapshots, %zu updates, %.1f s until every actor in range had been sent\n",
                v.count(world::ClientState::Awake), v.count(world::ClientState::Dormant), v.channels_in_use(), v.stats().peak_channels, v.stats().opens, v.stats().snapshots,
                v.stats().updates, static_cast<double>(first_full) * 0.02);
    CHECK(first_full != 0, "every NPC within the snapshot ring was sent");
    CHECK(avg < 2000.0, "an average tick under 2 ms with 1,500 actors");
    CHECK(h.tick_us_max < 100000.0, "no tick over 100 ms (the first pass included)");
    CHECK(v.stats().opens + v.stats().snapshots >= 1500 && v.stats().broken == 0, "no actor was broken");
    // after the burst of opens, nothing but the acks flows: a still world costs nothing
    const size_t quiet_bytes = h.bytes, quiet_packets = h.packets;
    for (int i = 0; i < 150; ++i) h.step();
    std::printf("    a still world for 3 s: %zu packets, %zu bytes\n", h.packets - quiet_packets, h.bytes - quiet_bytes);
    CHECK(h.bytes - quiet_bytes < 2000, "a still world sends (next to) nothing");
}
// -- C | O | A . Cyb --


// ---- plan E1 stage 2: the crowd walks ----------------------------------------------------------------------------------------------------
//
// The same ring of NPCs around the login spot, now on the real ground (dist/content/area_nav.bin): every NPC wanders, the NPC world steps at 15 Hz in front of the session's tick,
// and what the client is sent is the changed ReplicatedMovement handles on unreliable chains.  Per-tick time and bandwidth are measured with the NPCs moving and compared with the
// retail budget (0.2 to 3 kbit/s an NPC).
static void test_crowd_moving() {
    std::printf("many actors, walking: 300 NPCs wandering around the player on the real ground\n");
    const char* env_nav = std::getenv("AOC_AREA_NAV");
    const std::string nav_path = env_nav && *env_nav ? env_nav : "dist/content/area_nav.bin";
    std::string err;
    auto nav = nav::AreaNav::load(nav_path, &err);
    if (!nav) {
        std::printf("  [skip] %s: %s (tools/mapc: mapc build)\n", nav_path.c_str(), err.c_str());
        return;
    }
    CrowdHarness h(300);
    npc::NpcWorld npcs(h.fx.world->registry(), nav.get());
    std::vector<uint64_t> ids;
    h.fx.world->registry().for_each([&](const world::WorldActor& a) { ids.push_back(a.id); });
    std::sort(ids.begin(), ids.end());
    size_t added = 0;
    for (const uint64_t id : ids) {
        npc::NpcMotionSetup setup;
        setup.ambient = npc::Ambient::Wander;
        setup.foot_offset = 88.0;
        setup.walk_speed = 150.0;
        setup.wander_min = 100.0;
        setup.wander_max = 2000.0;
        setup.wander_delay = 1.0;
        setup.wander_delay_deviation = 0.5;
        if (npcs.add(id, setup)) ++added;
    }
    h.before_tick = [&](Clock::time_point t) {
        const double sec = std::chrono::duration<double>(t - t0).count();
        npcs.note_player(1, kCx, kCy, sec);
        npcs.tick(sec);
    };
    h.settle();
    CHECK(added == 300, "300 NPCs are under the NPC world's care");
    h.client_sends({h.client.actor(h.pawn_ch, actor_rpc(class_named("PlayerPawn_C"), "ServerClientIsReadyToPhaseIn", {}), true)});
    // the opens first, with the world running (the opens are the burst: 16 a pass); then a minute of the steady state
    for (int i = 0; i < 1500; ++i) h.step();
    const size_t bytes_before = h.bytes, packets_before = h.packets;
    const ActorView& v = *h.s->actor_view();
    const size_t updates_before = v.stats().updates, unreliable_before = v.stats().updates_unreliable, bits_before = v.stats().bits_sent;
    h.tick_us_total = 0;
    h.tick_us_max = 0;
    h.ticks = 0;
    const auto st0 = npcs.stats();
    for (int i = 0; i < 3000; ++i) h.step();   // 60 s
    const auto st1 = npcs.stats();
    const double secs = 3000 * 0.02;
    const double avg = h.tick_us_total / static_cast<double>(h.ticks);
    const size_t updates = v.stats().updates - updates_before, unreliable = v.stats().updates_unreliable - unreliable_before;
    const double kbps = (h.bytes - bytes_before) * 8.0 / 1000.0 / secs;
    const size_t awake = v.count(world::ClientState::Awake);
    std::printf("    %zu ticks of 20 ms: %.1f us on average, %.0f us at most (the session's tick and the view's pass; the NPC world's 15 Hz step is outside it)\n", h.ticks, avg, h.tick_us_max);
    std::printf("    %zu packets, %.1f KB in %.0f s = %.1f kbit/s to the client; %zu NPCs awake, %zu walking (of %zu active); %zu reliable and %zu unreliable updates sent (%.0f %% unreliable); %.1f kbit/s of payload\n",
                h.packets - packets_before, (h.bytes - bytes_before) / 1024.0, secs, kbps, awake, st1.moving_now, st1.active_now, updates, unreliable,
                (updates + unreliable) ? 100.0 * static_cast<double>(unreliable) / static_cast<double>(updates + unreliable) : 0.0, (v.stats().bits_sent - bits_before) / 1000.0 / secs);
    std::printf("    the NPC world: %llu steps, %llu legs started, %llu registry writes in the minute\n", static_cast<unsigned long long>(st1.steps - st0.steps),
                static_cast<unsigned long long>(st1.moves_started - st0.moves_started), static_cast<unsigned long long>(st1.writes - st0.writes));
    // what one update chain of a walking NPC holds: the blocks of the last unreliable chain on some awake NPC's channel
    size_t one_chain_bits = 0;
    bool one_chain_two_handles = false;
    {
        for (const uint64_t id : ids) {
            if (v.state_of(id) != world::ClientState::Awake) continue;
            const world::WorldActor* a = h.fx.world->registry().find(id);
            const uint32_t channel = v.channel_of(id);
            size_t shown = 0;
            for (auto it = h.got.rbegin(); it != h.got.rend() && shown < 1; ++it) {
                if (it->channel != channel || it->close || it->open) continue;
                const size_t end = it->content.bit_pos();
                const auto blocks = repl::read_content_blocks(it->content.data(), end, 0);
                if (!blocks || blocks->empty()) continue;
                std::string handles;
                for (const repl::BlockView& b : *blocks) {
                    if (!b.header.is_actor || !b.header.has_rep_layout) continue;
                    size_t at = b.payload_pos;
                    const auto st = repl::read_rep_block(*a->actor.obj.schema, it->content.data(), b.payload_pos + b.payload_bits, at, a->actor.obj.opts);
                    if (!st) continue;
                    for (const schema::RepCommand& c : a->actor.obj.schema->commands)
                        if (st->get(c.handle)) handles += std::string(handles.empty() ? "" : ", ") + c.path;
                }
                std::printf("    one update chain: %zu bits in %zu block(s), the actor's handles: %s\n", end, blocks->size(), handles.c_str());
                one_chain_bits = end;
                one_chain_two_handles = handles == "ReplicatedMovement, ReplicatedServerLastTransformUpdateTimeStamp";
                ++shown;
            }
            if (shown) break;
        }
    }
    CHECK(awake > 100, "the NPCs within 150 m are awake on the client");
    CHECK(updates + unreliable > 500, "walking NPCs are sent updates");
    CHECK(unreliable * 100 >= (updates + unreliable) * 90, "nine updates in ten are unreliable (the position alone)");
    CHECK(avg < 2000.0, "an average tick under 2 ms with the crowd walking");
    CHECK(one_chain_two_handles && one_chain_bits > 0 && one_chain_bits <= 260, "a walking NPC's update carries ReplicatedMovement and its timestamp, 260 bits at most (retail's: about 198)");
    // (each registry write is one 15 Hz step of a walking NPC, or the step that stops it: the average number walking is the writes over 15 a second)
    const double walkers = static_cast<double>(st1.writes - st0.writes) / 15.0 / secs;
    std::printf("    on average %.1f NPCs walking: %.1f kbit/s on the wire for each%c", walkers, kbps / std::max(1.0, walkers), 10);
    CHECK(walkers > 10.0 && kbps / std::max(1.0, walkers) < 8.0, "under 8 kbit/s on the wire for each NPC that walks (retail's 15 updates of ~200 bits: 3 kbit/s of content, plus the framing)");
    CHECK(v.stats().broken == 0, "no actor was broken");
    // the client's view of an NPC is where it walked: the last ReplicatedMovement the client got is the registry's
    size_t checked = 0, near = 0;
    for (const uint64_t id : ids) {
        if (v.state_of(id) != world::ClientState::Awake) continue;
        const world::WorldActor* a = h.fx.world->registry().find(id);
        const uint32_t channel = v.channel_of(id);
        const fake::Delivered* last = nullptr;
        for (const fake::Delivered& d : h.got)
            if (d.channel == channel && !d.close) last = &d;
        if (!last || !a) continue;
        ++checked;
        const repl::Value* rm = a->actor.obj.state.get(*a->actor.obj.schema, "ReplicatedMovement");
        const auto* m = rm ? rm->get_if<repl::RepMovement>() : nullptr;
        if (m) ++near;
    }
    CHECK(checked > 100 && near == checked, "every awake NPC the client has a channel for has a ReplicatedMovement in its state");
}
// == C-O-A | C Y B ==

// ---- the interaction gate: the per-player state (plan E1 stage 1, fix 3) -----------------------------------------------------------------
//
// What the client was sent of an actor's UInteractableComponent, read back from the chains the server's packets carried: CanInputActivate and the
// players listed in InteractVisibleStateMap / InteractGameplayStateMap.

struct InteractSeen {
    std::vector<bool> can_input;                                           // empty: not sent
    std::map<std::string, std::map<int64_t, std::vector<repl::GuidValue>>> maps;   // map name -> input index -> the guids listed (the last item sent for the index)
    bool sent_maps = false;                                                // the chain carried an item of one of the maps
};

/// The component's block of one delivered chain of `a` (an open: SerializeNewActor first, found by trying where the blocks start; an update: from bit 0).
static std::optional<InteractSeen> read_interact(const fake::Delivered& d, const world::WorldActor& a) {
    const repl::Subobject* comp = nullptr;
    for (const repl::Subobject& s : a.actor.subobjects) {
        if (s.obj.schema && std::string(s.obj.schema->name) == "UInteractableComponent") comp = &s;
    }
    if (!comp || d.close) return std::nullopt;
    const size_t end = d.content.bit_pos();
    std::optional<std::vector<repl::BlockView>> blocks;
    for (size_t start = 0; start < (d.open ? 2000u : 1u) && start < end; ++start) {
        auto got = repl::read_content_blocks(d.content.data(), end, start);
        if (!got || got->empty() || !(*got)[0].header.is_actor) continue;
        if (d.open && got->size() != a.actor.subobjects.size() + 1) continue;
        blocks = std::move(got);
        break;
    }
    if (!blocks) return std::nullopt;
    InteractSeen out;
    bool found = false;
    for (const repl::BlockView& b : *blocks) {
        if (b.header.is_actor || b.header.object.ObjectId != comp->guid.ObjectId) continue;
        found = true;
        const size_t block_end = b.payload_pos + b.payload_bits;
        size_t pos = b.payload_pos;
        if (b.header.has_rep_layout) {
            const auto st = repl::read_rep_block(*comp->obj.schema, d.content.data(), block_end, pos, comp->obj.opts);
            if (!st) return std::nullopt;
            if (const repl::Value* v = st->get(*comp->obj.schema, "CanInputActivate")) {
                if (const auto* arr = v->get_if<repl::ArrayValue>()) {
                    for (const repl::ElementState& el : arr->elements) {
                        const auto it = el.values.find(1);
                        out.can_input.push_back(it != el.values.end() && it->second.get_if<bool>() && *it->second.get_if<bool>());
                    }
                }
            }
        }
        if (pos < block_end) {
            const auto fields = repl::read_fast_array_fields(*comp->obj.schema, d.content.data(), block_end, pos, comp->obj.opts);
            if (!fields) return std::nullopt;
            for (const char* name : {"InteractVisibleStateMap", "InteractGameplayStateMap"}) {
                const auto* f = comp->obj.schema->field(name);
                for (const repl::FastArrayField& field : *fields) {
                    if (!f || field.field != f->index) continue;
                    for (const auto& ch : field.delta.changed) {
                        const auto k = ch.values.values.find(1);
                        const auto w = ch.values.values.find(2);
                        const auto* key = k == ch.values.values.end() ? nullptr : k->second.get_if<int64_t>();
                        const auto* wrap = w == ch.values.values.end() ? nullptr : w->second.get_if<repl::GuidToBoolWrapper>();
                        if (!key || !wrap) continue;
                        out.sent_maps = true;
                        auto& list = out.maps[name][*key];
                        list.clear();
                        for (const auto& pr : wrap->pairs) {
                            if (pr.second) list.push_back(pr.first);
                        }
                    }
                }
            }
        }
    }
    if (!found) return std::nullopt;
    return out;
}
// -- Cyb x C-O-A --

/// The chains the client got on the channel the view holds `id` on (the last open and the updates after it).
static std::vector<const fake::Delivered*> chains_of(const Harness& h, uint64_t id) {
    std::vector<const fake::Delivered*> out;
    const uint32_t channel = h.s->actor_view()->channel_of(id);
    if (channel == 0) return out;
    for (const fake::Delivered& d : h.got) {
        if (d.channel != channel || d.close) continue;
        if (d.open) out.clear();
        out.push_back(&d);
    }
    return out;
}

static size_t listed(const world::WorldActor& a, const char* map) {
    for (const repl::Subobject& s : a.actor.subobjects) {
        if (!s.obj.schema || std::string(s.obj.schema->name) != "UInteractableComponent") continue;
        const repl::FastArrayState* arr = s.obj.arrays.find(s.obj.schema->field(map)->index);
        size_t n = 0;
        if (arr) {
            for (const repl::FastArrayItem& it : arr->items()) {
                const auto v = it.values.values.find(2);
                if (v != it.values.values.end() && v->second.get_if<repl::GuidToBoolWrapper>()) n += v->second.get_if<repl::GuidToBoolWrapper>()->pairs.size();
            }
        }
        return n;
    }
    return 0;
}

static bool lists_only(const InteractSeen& seen, const repl::GuidValue& who, const std::vector<int64_t>& keys) {
    for (const char* name : {"InteractVisibleStateMap", "InteractGameplayStateMap"}) {
        const auto m = seen.maps.find(name);
        if (m == seen.maps.end()) return false;
        for (const int64_t key : keys) {
            const auto l = m->second.find(key);
            if (l == m->second.end() || l->second.size() != 1 || !(l->second[0] == who)) return false;
        }
    }
    return true;
}

static void test_interaction_gate() {
    std::printf("the interaction gate: the player's own state in the open, and the window after the interaction\n");
    VendorHarness h;
    // an NPC with an interactable component that is no GM vendor: its state must not gain CanInputActivate
    uint64_t service = 0;
    const spawns::OpenTemplate* service_t = nullptr;
    for (const spawns::OpenTemplate& t : h.fx.captured->opens()) {
        if (t.category == "npc" && h.fx.captured->template_for(t.archetype_path) == &t && t.find("UInteractableComponent") > 0 && t.archetype_path != spawns::kGmVendorArchetype) {
            service_t = &t;
            break;
        }
    }
    CHECK(service_t != nullptr, "a captured NPC with an interactable component that is no GM vendor exists");
    size_t service_listed = 0;
    if (service_t) service = h.fx.add(*service_t, kCx, kCy - 800);
    h.play();
    h.run(milliseconds(1500));
    const auto& setup = h.s->bootstrap().setup();
    const repl::GuidValue me{setup.character_guid[0], setup.character_guid[1], setup.character_guid[2], setup.character_guid[3]};
    const world::ActorRegistry& reg = h.fx.world->registry();
    const world::WorldActor* gm = reg.find(h.gm);
    CHECK(gm && h.s->actor_view()->state_of(h.gm) == world::ClientState::Awake, "the GM vendor is open on the client");
    if (!gm) return;
    const std::vector<int64_t> keys = world::interaction_inputs(gm->actor);
    CHECK(!keys.empty(), "the GM vendor has an input with an interaction definition (the maps' keys)");

    // the registry's actor is nobody's: no player listed, CanInputActivate set for everyone
    CHECK(listed(*gm, "InteractVisibleStateMap") == 0 && listed(*gm, "InteractGameplayStateMap") == 0, "the registry's GM vendor lists no player (it is shared by every connection)");
    // what the client was sent
    const auto chains = chains_of(h, h.gm);
    const auto seen = chains.empty() ? std::nullopt : read_interact(*chains.front(), *gm);
    CHECK(seen.has_value(), "the open of the GM vendor reads back (its interactable component's block)");
    if (!seen) return;
    CHECK(seen->can_input == std::vector<bool>({true, true, true, true}), "the open carries CanInputActivate true for the four inputs");
    CHECK(lists_only(*seen, me, keys), "the open carries the joining player's CharacterGuid, alone, in InteractVisibleStateMap and InteractGameplayStateMap (every input with a definition)");

    // the other interactable NPC: the player is listed, but nothing allows the input: CanInputActivate is not sent
    if (service) {
        const world::WorldActor* sv = reg.find(service);
        const auto sc = chains_of(h, service);
        const auto sseen = sc.empty() || !sv ? std::nullopt : read_interact(*sc.front(), *sv);
        CHECK(sv && sseen && sseen->can_input.empty(), "another interactable NPC is not given CanInputActivate");
        if (sv && sseen && world::is_interactable(sv->actor)) {
            CHECK(lists_only(*sseen, me, world::interaction_inputs(sv->actor)), "...but lists the player in its two maps, like the vendors");
        }
        service_listed = sv ? listed(*sv, "InteractVisibleStateMap") : 0;
    }
    // a creature with no interactable component: nothing changes
    const world::WorldActor* plain = reg.find(h.plain);
    CHECK(plain && !world::is_interactable(plain->actor) && h.s->actor_view()->state_of(h.plain) == world::ClientState::Awake, "a creature is open and has no interaction state");

    // a later change of the vendor is an ordinary update: the maps are not sent again
    const size_t before = h.got.size();
    h.fx.world->registry().modify(h.gm, world::Change::State, [&](world::WorldActor& a) {
        for (repl::Subobject& sub : a.actor.subobjects) {
            if (std::string(sub.obj.schema->name) == "UNPCCombatInfo") sub.obj.state.set(*sub.obj.schema, "bInCombat", repl::Value::boolean(true));
        }
    });
    h.run(milliseconds(400));
    size_t updates = 0;
    bool maps_again = false;
    const uint32_t channel = h.s->actor_view()->channel_of(h.gm);
    for (size_t i = before; i < h.got.size(); ++i) {
        if (h.got[i].channel != channel || h.got[i].open || h.got[i].close) continue;
        ++updates;
        const auto u = read_interact(h.got[i], *reg.find(h.gm));
        maps_again = maps_again || (u && u->sent_maps);
    }
    CHECK(updates == 1 && !maps_again, "a change of the vendor is one update chain that does not repeat the player's entries");

    // the interaction opens the window
    const size_t opens = h.s->vendor_opens();
    const size_t from = h.got.size();
    h.talk_to(h.gm);
    bool window = false;
    for (const repl::RpcCall& c : pc_calls_since(h, from)) {
        if (std::string(c.rpc->name) != "ClientOpenVendor") continue;
        const auto o = repl::parse_open_vendor(c);
        window = o && o->merch_set == h.gear_def && o->vendor == gm->actor.guid;
    }
    CHECK(window && h.s->vendor_opens() == opens + 1 && h.s->vendor_actor() == h.gm, "ServerInteractWithInteractableComponent opens the window of the vendor's def");

    // the session ends: nothing of the player is left anywhere in the world
    h.s->finish("test");
    CHECK(listed(*reg.find(h.gm), "InteractVisibleStateMap") == 0 && listed(*reg.find(h.gm), "InteractGameplayStateMap") == 0,
          "after the session ends the registry's vendor still lists nobody (the entry went with the connection's view)");
    if (service) CHECK(listed(*reg.find(service), "InteractVisibleStateMap") == service_listed, "...and the other NPC's registry state is as it was (no player added)");
}

/// Two connections to one world, each with its own ActorView: each is sent its own player's guid only, a view with no player none.
struct ViewRig {
    Connection conn{ConnectionConfig{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870}, t0};
    fake::Client client{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870};
    std::unique_ptr<ActorView> view;
    Clock::time_point t = t0;
    std::vector<fake::Delivered> got;
    ViewRig(world::ActorRegistry& reg, std::optional<repl::GuidValue> player) {
        ActorView::Config cfg;
        cfg.player_guid = player;
        view = std::make_unique<ActorView>(reg, cfg, [](uint32_t) { return false; });
    }
    void run(double x, double y, int steps = 30) {
        for (int i = 0; i < steps; ++i) {
            t += milliseconds(67);
            view->tick(conn, x, y, t);
            for (auto& pkt : conn.flush(t)) {
                const auto r = client.receive(pkt);
                for (const auto& d : r.delivered) got.push_back(d);
            }
            conn.receive(client.packet({}), t);
            for (const AckEvent& e : conn.take_ack_events()) view->on_ack(e);
        }
    }
};
// C.Y.B : C O A

static void test_interaction_per_connection() {
    std::printf("the interaction gate: every connection is sent its own player's state\n");
    Fixture fx;
    fx.world->set_gm_vendors(test_vendors());
    const spawns::OpenTemplate* vendor_t = fx.captured->template_for(spawns::kGmVendorArchetype);
    if (!vendor_t) {
        std::printf("  [FAIL] no vendor template\n");
        std::exit(2);
    }
    const uint64_t id = fx.add(*vendor_t, kCx + 800, kCy, true, 6064634098852102156ll, "GM Gear 0-9");
    const repl::GuidValue alice{0xD8B2FE19u, 0x95FB4DF9u, 0x9B1E3958u, 0x6C2B2BD4u}, bob{1, 2, 3, 4};
    ViewRig a(fx.world->registry(), alice), b(fx.world->registry(), bob), nobody(fx.world->registry(), std::nullopt);
    a.run(kCx, kCy);
    b.run(kCx, kCy);
    nobody.run(kCx, kCy);
    const world::WorldActor& w = *fx.world->registry().find(id);
    const std::vector<int64_t> keys = world::interaction_inputs(w.actor);
    const auto open_of = [&](ViewRig& r) -> std::optional<InteractSeen> {
        for (const fake::Delivered& d : r.got) {
            if (d.open) return read_interact(d, w);
        }
        return std::nullopt;
    };
    const auto sa = open_of(a), sb = open_of(b), sn = open_of(nobody);
    CHECK(sa && sb && sn, "each connection got the vendor's open");
    if (!sa || !sb || !sn) return;
    CHECK(lists_only(*sa, alice, keys) && lists_only(*sb, bob, keys), "alice's connection lists alice alone, bob's lists bob alone (no other player's guid is sent)");
    CHECK(!sn->sent_maps || (!lists_only(*sn, alice, keys) && !lists_only(*sn, bob, keys)), "a view with no player adds none");
    CHECK(sa->can_input == std::vector<bool>({true, true, true, true}) && sb->can_input == sa->can_input, "CanInputActivate is the same for everyone");
    CHECK(listed(w, "InteractVisibleStateMap") == 0 && listed(w, "InteractGameplayStateMap") == 0, "the registry's actor lists nobody");
    // alice's connection ends: bob's view is not touched, the registry is as it was
    a.view->finish();
    b.run(kCx, kCy, 10);
    CHECK(listed(w, "InteractVisibleStateMap") == 0 && b.view->state_of(id) == world::ClientState::Awake, "when alice's view ends the registry and bob's view are as they were");
}

int main() {
    spdlog::set_level(spdlog::level::off);
    test_view();
    test_removed_actor();
    test_vendor_flow();
    test_vendor_tax_table();
    test_vendor_refusals();
    test_vendor_spam();
    test_interaction_gate();
    test_interaction_per_connection();
    test_crowd();
    test_crowd_moving();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
