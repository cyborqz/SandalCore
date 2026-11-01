// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_gm_travel.cpp  (the GM travel tools: teleport and run speed)
//
//  The harness of test_worldd_mounts_ride (a session with a fake client on a PlayerWorld whose mounts are the real content.db's) and the real content.db's city nodes:
//
//    teleport      a point (the ground's height when no z is given, a refusal asking for z where the server has no ground), a place by name (the city nodes the way nodectl
//                  matches them, and login), the position, the move checker rebased (the next move from the new place is not corrected, one from the old place is), the client's
//                  position correction, unknown characters and places, a dead player, a point outside the world; a mounted player is dismounted first and the mount desummons by distance
//    run speed     the pawn's MoveSpeedMult stat, the move checker's limits scaled (accepts moves at the multiplied speed, still refuses beyond it), the Sprint on top, the grace after
//                  the speed goes down, the reset; on a mount the mount's MoveSpeedMult and limit; survives the dismount; a new session starts at 1
//    client RPCs   ServerTeleportXYZ, ServerTeleport and ServerTeleportPlayer: received and logged with the switch off, answered with it on
//    GM stats      the game master's stat overrides: the stats the session holds change, the override is remembered and cleared, a refused change leaves nothing behind; the current value of a
//                  held stat goes to the client
//    admin API     WorldAdminService over a real gRPC server: the service token (no token, a wrong one, a bare Bearer, the token without Bearer: refused), the queue (every call is
//                  applied on the draining thread, never the gRPC thread; a call the network thread does not take in time is Unavailable and never applied), the game master's stat
//                  calls (the token is still the door, an unknown character is refused, a player's stats come back with the override on them)
// ============================================================================
#include "content_db_fixture.h"
#include "cyb_appearance.h"
#include "fake_client.h"
#include "bootstrap/character_setup.h"
#include "content/content_db.h"
#include "items/inventory.h"
#include "nav_fixture.h"
#include "session/gm_travel.h"
#include "svc/world_admin.h"
#include "town_platform_fixture.h"
#include "world/city_nodes.h"
#include "world/places.h"
#include "aoc/worldadmin/v1/worldadmin.grpc.pb.h"

#include <grpcpp/grpcpp.h>
// ~ C.O.A - Cyb ~

#include <atomic>
#include <limits>
#include <stdexcept>
#include <thread>
#include "net/endpoint.h"
#include "protocol/repl/ability_rpc.h"
#include "protocol/repl/block_reader.h"
#include "protocol/repl/fast_array.h"
#include "protocol/repl/item_rpc.h"
#include "protocol/repl/move_packed.h"
#include "protocol/repl/rep_block.h"
#include "protocol/repl/skill_rpc.h"
#include "protocol/repl/vendor_rpc.h"
#include "protocol/schema/generated/index.h"
#include "session/player_session.h"
#include "spawns/mount_spawn.h"
#include "spawns/mount_state.h"
#include "world/actor_registry.h"
#include "world/roads.h"

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <optional>
#include <set>
#include <string>
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
using emit::BunchWriter;
using std::chrono::milliseconds;
using std::chrono::seconds;
using svc::ItemContainer;

static int g_pass = 0, g_fail = 0;
/// The pawn's clock: the TimeStamp of its next move (a client's clock restarts near 0 at every possession).
static float g_pawn_ts = 0;
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

static constexpr int kMountSlot = 5, kOtherMountSlot = 6, kLockedSlot = 7, kAquaticSlot = 8, kFlyingSlot = 9, kPotionSlot = 1, kSwordSlot = 0;
static int64_t g_mount_record = 0, g_other_mount_record = 0, g_aquatic_record = 0, g_flying_record = 0;

static std::vector<svc::InventoryItem> kit() {
    const std::pair<int, int64_t> worn[] = {{1, fi::kHelmet}, {2, fi::kChest}, {5, fi::kLegs}, {6, fi::kFeet}, {15, fi::kScepter}, {16, fi::kFocus}, {17, fi::kBook},
                                             {23, fi::kBag}, {26, fi::kPickaxe}, {29, fi::kAxe}, {33, fi::kSickle}, {35, fi::kFishingRod}, {38, fi::kBow}};
    std::vector<svc::InventoryItem> out;
    for (const auto& [slot, record] : worn) out.push_back(make_item(Worn, slot, record, 100 + static_cast<uint32_t>(slot)));
    out.push_back(make_item(Bag, kSwordSlot, fi::kSword2H, 200));
    out.push_back(make_item(Bag, kPotionSlot, fi::kPotion, 201, 5));
    svc::InventoryItem a = make_item(Bag, kMountSlot, g_mount_record, 300);
    a.persisted_health = 3572;
    a.persisted_mana = 1;
    out.push_back(a);
    out.push_back(make_item(Bag, kOtherMountSlot, g_other_mount_record, 301));
    svc::InventoryItem locked = make_item(Bag, kLockedSlot, g_mount_record, 302);
    locked.lock_expiration = 4'000'000'000ll;   // the year 2096
    out.push_back(locked);
    out.push_back(make_item(Bag, kAquaticSlot, g_aquatic_record, 303));
    out.push_back(make_item(Bag, kFlyingSlot, g_flying_record, 304));
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
// ~ Cyb :: C.O.A ~

struct RecordingSink : PositionSink {
    std::vector<std::vector<svc::InventoryItem>> inventories;
    std::vector<svc::Position> positions;
    void save(const std::string&, const svc::Position& p) override { positions.push_back(p); }
    void save_inventory(const std::string&, std::vector<svc::InventoryItem> items) override { inventories.push_back(std::move(items)); }
    void save_progress(const std::string&, svc::Progress) override {}
};

static std::string real_db_path() {
    const char* env = std::getenv("AOC_CONTENT_DB");
    return env && *env ? env : MOUNT_CONTENT_DB_PATH;
}

// ---- the world -------------------------------------------------------------------------------------------------------------------

struct Fixture {
    fs::path dir;
    std::optional<content::ContentDb> content;
    std::optional<content::ContentDb> real;
    aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
    std::unique_ptr<PlayerWorld> world;

    Fixture() {
        dir = cf::make_temp_dir("worldd_mounts_ride_");
        const fs::path db = dir / "content.db";
        if (!cf::make_db(db)) std::exit(2);
        std::string error;
        content = content::ContentDb::open(db.string(), error);
        if (!content) {
            std::printf("  [FAIL] the content.db does not open: %s\n", error.c_str());
            std::exit(2);
        }
        real = content::ContentDb::open(real_db_path(), error);
        if (!real) {
            std::printf("  [FAIL] the real content.db does not open (%s): %s\n", real_db_path().c_str(), error.c_str());
            std::exit(2);
        }
        world = std::make_unique<PlayerWorld>(&*content, "", bs::GameStateMode::Tier2, kServerId, guids);
        auto book = spawns::MountBook::load(*real, error);
        if (!book) {
            std::printf("  [FAIL] the mounts do not load: %s\n", error.c_str());
            std::exit(2);
        }
        // the Blueprints' speeds, as dist/content/mount_speeds.tsv has them (the terror bird 990, the Dawnbreaker's flight 1,290)
        std::string speeds_error;
        if (!book->load_speeds(MOUNT_SPEEDS_PATH, speeds_error)) {
            std::printf("  [FAIL] the mount speeds do not load: %s\n", speeds_error.c_str());
            std::exit(2);
        }
        world->set_mounts(std::move(*book));
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    ~Fixture() {
        world.reset();
        content.reset();
        real.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

// ---- the harness -------------------------------------------------------------------------------------------------------------------

struct Harness {
    Fixture fx;
    RecordingSink sink;
    Connection conn{ConnectionConfig{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870}, t0};
    fake::Client client{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870};
    std::unique_ptr<PlayerSession> s;
    Clock::time_point t = t0;
    std::vector<fake::Delivered> got;
    std::vector<Clock::time_point> got_at;
    std::string error;
    uint32_t pawn_ch = 0, pc_ch = 0;
    svc::Character record;

    explicit Harness(svc::Character r, const std::function<void(PlayerWorld&)>& setup = nullptr) : record(std::move(r)) {
        fx.world->set_position_sink(&sink);
        if (setup) setup(*fx.world);
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
            const auto r = client.receive(pkt);
            for (const auto& d : r.delivered) {
                got.push_back(d);
                got_at.push_back(t);
            }
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
        for (int quiet = 0, i = 0; quiet < 5 && i < 600; ++i) {
            const size_t before = got.size();
            step();
            quiet = (got.size() == before && s->join_pending() == 0 && s->replies_pending() == 0) ? quiet + 1 : 0;
        }
    }
    void play();
    double since(Clock::time_point from) const { return std::chrono::duration<double>(t - from).count(); }
    const world::WorldActor* mount() const { return s->mounts() ? s->mounts()->mount() : nullptr; }
    uint32_t mount_ch() const {
        const world::WorldActor* m = mount();
        return m && s->actor_view() ? s->actor_view()->channel_of(m->id) : 0;
    }
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
// -- C-O-A . Cyb --

void Harness::play() {
    g_pawn_ts = 0;
    settle();
    client_sends({client.actor(pawn_ch, actor_rpc(class_named("PlayerPawn_C"), "ServerClientIsReadyToPhaseIn", {}), true)});
    settle();
    run(milliseconds(400));
}

// ---- the client's requests -------------------------------------------------------------------------------------------------------------

static BunchWriter item_used(int storage, int slot, const std::string& guid_hex) {
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    const schema::RpcSchema* rpc = pawn.rpc("ServerItemUsed");
    repl::RpcParams p;
    const auto g = items::guid_of(guid_hex);
    const repl::GuidValue gv = g ? *g : repl::GuidValue{};
    repl::set_rpc_param(*rpc, p, "Storage", repl::Value::u64(static_cast<uint64_t>(storage)));
    repl::set_rpc_param(*rpc, p, "Slot", repl::Value::i64(slot));
    repl::set_rpc_param(*rpc, p, "ExpectedItem.A", repl::Value::i64(static_cast<int32_t>(gv.a)));
    repl::set_rpc_param(*rpc, p, "ExpectedItem.B", repl::Value::i64(static_cast<int32_t>(gv.b)));
    repl::set_rpc_param(*rpc, p, "ExpectedItem.C", repl::Value::i64(static_cast<int32_t>(gv.c)));
    repl::set_rpc_param(*rpc, p, "ExpectedItem.D", repl::Value::i64(static_cast<int32_t>(gv.d)));
    repl::set_rpc_param(*rpc, p, "RequestedEquipSlot", repl::Value::i64(static_cast<int32_t>(0xFFFFFFFFu)));
    return actor_rpc(pawn, "ServerItemUsed", p);
}

static BunchWriter trigger_on(Harness& h, const emit::FIntrepidNetworkGUID& component, int64_t ability, int32_t client_instance, int slot = -1, const svc::InventoryItem* item = nullptr,
                              uint8_t location = 3, int32_t animation_index = 0) {
    const schema::ClassSchema& cls = class_named("UAoCAbilityComponent");
    const schema::RpcSchema* rpc = cls.rpc("ServerTriggerAbility");
    repl::RpcParams p;
    bool ok = rpc && repl::set_rpc_param(*rpc, p, "AbilityGuid", repl::Value::i64(ability));
    if (ok && client_instance != 0) ok = repl::set_rpc_param(*rpc, p, "ClientInstanceId", repl::Value::i64(client_instance));
    if (ok && animation_index != 0) ok = repl::set_rpc_param(*rpc, p, "AnimationIndex", repl::Value::i64(animation_index));
    ok = ok && repl::set_rpc_param(*rpc, p, "InputVector", repl::Value(repl::Vec2d{1, 0}));
    if (ok && slot >= 0) {
        ok = repl::set_rpc_param(*rpc, p, "ItemData.Slot", repl::Value::i64(slot)) &&
             repl::set_rpc_param(*rpc, p, "ItemData.ItemInstance", repl::Value(item ? items::to_instance(*item) : repl::ItemInstance{})) &&
             repl::set_rpc_param(*rpc, p, "ItemData.Location", repl::Value::u64(location));
    }
    if (!ok) {
        std::printf("  [FAIL] the trigger's parameters do not fit\n");
        return BunchWriter();
    }
    BunchWriter out;
    BunchWriter field;
    bool failed = false;
    if (repl::write_rpc_field(cls, *rpc, p, field, {}, &failed) == 0 || failed) {
        std::printf("  [FAIL] ServerTriggerAbility does not write\n");
        return out;
    }
    repl::BlockHeader header;
    header.is_actor = false;
    header.has_rep_layout = false;
    header.object = component;
    repl::write_content_block(out, header, field, /*server_form=*/false);
    (void)h;
    return out;
}

/// ServerTriggerAbility(Summon_Mount) as the retail client sends it for a mount item, on the pawn's AbilityComponent.
static BunchWriter summon_trigger(Harness& h, int32_t client_instance, int slot, const svc::InventoryItem* item, uint8_t location = 3) {
    return trigger_on(h, h.s->bootstrap().setup().guids.ability, 108436548376185ll, client_instance, slot, item, location);
}

static BunchWriter desummon_rpc() { return actor_rpc(class_named("AoCPlayerControllerBP_C"), "ServerDesummonMount", {}); }
static BunchWriter dismount_rpc() { return actor_rpc(class_named("AoCPlayerControllerBP_C"), "ServerDismountInteraction", {}); }

static BunchWriter possession_ack(const emit::FIntrepidNetworkGUID& actor) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    repl::RpcParams p;
    repl::set_rpc_param(*pc.rpc("ServerAcknowledgePossession"), p, "P", repl::Value(actor));
    return actor_rpc(pc, "ServerAcknowledgePossession", p);
}

static emit::FIntrepidNetworkGUID component_of(const world::WorldActor& m, uint64_t object_id) {
    emit::FIntrepidNetworkGUID g = m.actor.guid;
    g.ObjectId = object_id;
    return g;
}

static BunchWriter interact_with(const emit::FIntrepidNetworkGUID& component) {
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    const schema::RpcSchema* rpc = pawn.rpc("ServerInteractWithInteractableComponent");
    repl::InteractRequest r;
    r.interactable = component;
    r.counter = 1;
    r.zone = 2;
    const auto params = repl::interact_params(*rpc, r);
    return params ? actor_rpc(pawn, "ServerInteractWithInteractableComponent", *params) : BunchWriter();
}

static repl::NetworkMove move_at(float ts, double x, double y, double z, uint16_t yaw = 0, uint8_t mode = repl::kMoveWalking, uint16_t pitch = 0) {
    repl::NetworkMove m;
    m.time_stamp = ts;
    m.control_rotation.pitch = pitch;
    m.acceleration = repl::quantize_vector(10, 0, 0, 0);
    m.location = repl::quantize_vector(100, x, y, z);
    m.control_rotation.yaw = yaw;
    m.movement_mode = mode;
    return m;
}

/// The client's bunch payload for a ServerMovePacked of `move` on an actor of class `cls_name`.
static BunchWriter client_move(const char* cls_name, const repl::NetworkMove& m) {
    const schema::ClassSchema& cls = class_named(cls_name);
    const schema::NetField* f = cls.field("ServerMovePacked");
    BunchWriter out;
    if (!f) return out;
    repl::MovePacked p;
    p.new_move = m;
    BunchWriter params;
    if (!repl::write_move_packed(params, p)) return out;
    BunchWriter field;
    if (repl::write_field(cls, f->index, params, field) == 0) return out;
    repl::BlockHeader header;
    repl::write_content_block(out, header, field, /*server_form=*/false);
    return out;
}

static void move_pawn(Harness& h, const repl::NetworkMove& m) { h.client_sends({h.client.actor(h.pawn_ch, client_move("PlayerPawn_C", m), false)}); }
static void move_mount(Harness& h, const repl::NetworkMove& m) {
    const uint32_t ch = h.s->mount_channel();
    h.client_sends({h.client.actor(ch ? ch : 999, client_move("ANPCCharacter", m), false)});
}

/// The retail client's summon: ServerItemUsed and ServerTriggerAbility in one datagram, on the pawn's channel.
static void summon_with(Harness& h, int slot, int32_t client_instance = 7) {
    const svc::InventoryItem* item = h.s->inventory().at(Bag, slot);
    const std::string guid = item ? item->instance_guid : std::string();
    h.client_sends({h.client.actor(h.pawn_ch, item_used(3, slot, guid), true), h.client.actor(h.pawn_ch, summon_trigger(h, client_instance, slot, item), true)});
}

static void desummon_with(Harness& h, int slot) {
    const svc::InventoryItem* item = h.s->inventory().at(Bag, slot);
    h.client_sends({h.client.actor(h.pawn_ch, item_used(3, slot, item ? item->instance_guid : std::string()), true), h.client.actor(h.pc_ch, desummon_rpc(), true)});
}

/// The client's interaction with its mount.
static void get_on(Harness& h) {
    const world::WorldActor* m = h.mount();
    if (!m) {
        std::printf("  [FAIL] (no mount to get on)\n");
        return;
    }
    h.client_sends({h.client.actor(h.pawn_ch, interact_with(component_of(*m, m->npc.interact_component)), true)});
}

static void get_off(Harness& h) { h.client_sends({h.client.actor(h.pc_ch, dismount_rpc(), true)}); }

static uint16_t yaw16_of(double degrees) { return static_cast<uint16_t>(static_cast<int>(std::lround(degrees * 65536.0 / 360.0)) & 0xFFFF); }

/// The pawn walks in a straight line to (x, y) at `speed` cm/s, a move every 0.2 s of the test's clock (what the checker's pacing wants).
static void walk_pawn_to(Harness& h, double x, double y, double speed = 600.0) {
    const GoodPosition g = h.s->movement().good();
    const double dx = x - g.x, dy = y - g.y, dist = std::hypot(dx, dy);
    const int steps = std::max(1, static_cast<int>(std::ceil(dist / (speed * 0.2))));
    for (int i = 1; i <= steps; ++i) {
        g_pawn_ts += 0.2f;
        move_pawn(h, move_at(g_pawn_ts, g.x + dx * i / steps, g.y + dy * i / steps, g.z, yaw16_of(g.yaw_degrees)));
        h.step(milliseconds(200));
    }
}
// CoA and C | Y | B

/// The player walks up to its mount (within 250 cm), as retail's did (227 cm).
static void walk_up_to_mount(Harness& h) {
    const world::WorldActor* m = h.mount();
    if (!m) return;
    const GoodPosition g = h.s->movement().good();
    const double dx = m->x - g.x, dy = m->y - g.y, dist = std::hypot(dx, dy);
    if (dist <= 250) return;
    walk_pawn_to(h, g.x + dx * (dist - 250) / dist, g.y + dy * (dist - 250) / dist);
}

/// A summoned mount that is out and ready, and the player on it (`mounted` false: only out).
static bool ride_on(Harness& h, int slot = kMountSlot) {
    summon_with(h, slot);
    h.run(milliseconds(1900));
    if (!h.mount() || h.s->mounts()->phase() != MountController::Phase::Out) return false;
    walk_up_to_mount(h);
    get_on(h);
    h.run(milliseconds(200));
    return h.s->mounted();
}

// ---- what the client saw ---------------------------------------------------------------------------------------------------------------

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

/// The states a chain's property blocks carry, by the object they name (0: the actor), read with the schema of `actor`'s class and subobjects.
using States = std::map<uint64_t, repl::ObjectState>;
static States states_of(const fake::Delivered& d, const repl::Actor& actor) {
    States out;
    const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
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

static std::optional<int64_t> int_of(const repl::ObjectState& o, const char* path) {
    const repl::Value* v = o.schema ? o.state.get(*o.schema, path) : nullptr;
    if (!v) return std::nullopt;
    if (const auto* i = v->get_if<int64_t>()) return *i;
    if (const auto* u = v->get_if<uint64_t>()) return static_cast<int64_t>(*u);
    if (const auto* b = v->get_if<bool>()) return *b ? 1 : 0;
    return std::nullopt;
}
// (C-O-A : C | Y | B)

static std::optional<emit::FIntrepidNetworkGUID> object_of(const repl::ObjectState& o, const char* path) {
    const repl::Value* v = o.schema ? o.state.get(*o.schema, path) : nullptr;
    if (!v) return std::nullopt;
    if (const auto* g = v->get_if<emit::FIntrepidNetworkGUID>()) return *g;
    return std::nullopt;
}

/// Every chain the client received on `channel` from `from` on.
static std::vector<const fake::Delivered*> chains_on(const Harness& h, uint32_t channel, size_t from) {
    std::vector<const fake::Delivered*> out;
    for (size_t i = from; i < h.got.size(); ++i) {
        if (h.got[i].channel == channel && !h.got[i].open && !h.got[i].close) out.push_back(&h.got[i]);
    }
    return out;
}

static size_t acks_on(const Harness& h, uint32_t channel, size_t from, const char* cls = "ANPCCharacter") {
    size_t n = 0;
    for (const fake::Delivered* d : chains_on(h, channel, from)) {
        for (const repl::RpcCall& c : calls_of(*d, cls)) n += c.rpc && std::string(c.rpc->name) == "ClientMoveResponsePacked";
    }
    return n;
}

static size_t corrections_on(const Harness& h, uint32_t channel, size_t from, const char* cls = "ANPCCharacter") {
    size_t n = 0;
    for (const fake::Delivered* d : chains_on(h, channel, from)) {
        for (const repl::RpcCall& c : calls_of(*d, cls)) n += c.rpc && std::string(c.rpc->name) == "ClientAdjustPosition";
    }
    return n;
}

static size_t mount_count(const Harness& h) { return h.fx.world->registry().count(world::ActorKind::Mount); }
static bool within(double got, double want, double tol) { return got >= 0 && std::fabs(got - want) <= tol; }

// ---- the tests -------------------------------------------------------------------------------------------------------------------------

static void test_get_on_and_off() {
    std::printf("getting on and off a mount\n");
    Harness h(cyb_record());
    h.play();
    summon_with(h, kMountSlot);
    h.run(milliseconds(300));
    CHECK(h.mount() != nullptr && h.s->mounts()->phase() == MountController::Phase::Summoning, "(the mount is being summoned)");
    // during the summon the mount cannot be mounted
    get_on(h);
    h.run(milliseconds(100));
    CHECK(!h.s->mounted() && h.s->mount_interactions_refused() == 1, "an interaction while the mount is being summoned is refused");
    h.run(milliseconds(1500));
    CHECK(h.s->mounts()->phase() == MountController::Phase::Out, "(the mount is out)");
    const world::WorldActor* m = h.mount();
    const uint32_t mch = h.mount_ch();
    CHECK(m && mch != 0 && spawns::sub_of(*m, "UInteractableComponent") != nullptr, "(the mount is open on a channel)");
    if (!m || mch == 0) return;
    const bs::SessionGuids& g = h.s->bootstrap().setup().guids;
    const emit::FIntrepidNetworkGUID mount_guid = m->actor.guid;
    emit::FIntrepidNetworkGUID cylinder = mount_guid;
    cylinder.ObjectId = m->npc.collision_cylinder;
    // too far: the mount is 575 cm ahead, the interaction reaches 400 cm and a tolerance
    get_on(h);
    h.run(milliseconds(100));
    CHECK(!h.s->mounted() && h.s->mount_interactions_refused() == 2, "an interaction 575 cm from the mount is refused (too far)");
    walk_up_to_mount(h);
    const size_t from = h.got.size();
    get_on(h);
    h.run(milliseconds(200));
    CHECK(h.s->mounted() && h.s->mount_ups() == 1, "the player is on the mount");
    m = h.mount();
    // the PlayerController's first packet: ClientRestart(mount), camera mode 204, the bank-18 clear and the dash's slot
    bool restart_mount = false, camera = false, clear = false, slot = false;
    size_t order_restart = 99, order_camera = 99, order_clear = 99, order_slot = 99, n = 0;
    for (const fake::Delivered* d : chains_on(h, h.pc_ch, from)) {
        for (const repl::RpcCall& c : calls_of(*d, "AoCPlayerControllerBP_C")) {
            if (!c.rpc) continue;
            const std::string name = c.rpc->name;
            if (name == "ClientRestart") {
                const auto it = c.params.values.find(1);
                const auto* o = it != c.params.values.end() ? it->second.get_if<emit::FIntrepidNetworkGUID>() : nullptr;
                restart_mount = o && *o == mount_guid;
                order_restart = n;
            } else if (name == "ClientSetCameraMode") {
                camera = true;
                order_camera = n;
            } else if (name == "ClientClearActionbarsForInstigators") {
                clear = true;
                order_clear = n;
            } else if (name == "ClientReceiveActionbarSlotPayload") {
                slot = true;
                order_slot = n;
            }
            ++n;
        }
    }
    CHECK(restart_mount && camera && clear && slot, "the PlayerController gets ClientRestart(the mount), the camera mode and the bank-18 bar");
    CHECK(order_restart < order_camera && order_camera < order_clear && order_clear < order_slot, "in retail's order (910 packet 306)");
    // the mount's update: possession and its rider's stats
    bool mount_actor = false, stats_rider = false, known = false, rider_ptr = false, interaction = false, owner_arrays = false;
    for (const fake::Delivered* d : chains_on(h, mch, from)) {
        const States st = states_of(*d, m->actor);
        for (const auto& [id, o] : st) {
            const std::string cls = o.schema ? o.schema->name : "";
            if (id == 0) {
                mount_actor = int_of(o, "RemoteRole") == 2 && object_of(o, "Owner") == g.player_controller && object_of(o, "Controller") == g.player_controller &&
                              object_of(o, "PlayerState") == g.player_state;
            } else if (cls == "UAoCStatsComponent") {
                stats_rider = true;
                const schema::NetField* f = o.schema->field("StatRepInt32Owner");
                const repl::FastArrayState* a = f ? o.arrays.find(f->index) : nullptr;
                owner_arrays = owner_arrays || (a && a->items().size() > 20);
            } else if (cls == "UAoCAbilityComponent") {
                known = known || o.state.get(*o.schema, "KnownAbilities") != nullptr;
            } else if (cls == "UNPCSecondaryInfo") {
                rider_ptr = rider_ptr || object_of(o, "RiderPtr") == g.pawn;
            } else if (cls == "UInteractableComponent") {
                interaction = interaction || o.state.get(*o.schema, "CurrentInputStates") != nullptr;
            }
        }
    }
    CHECK(mount_actor, "the mount's actor: RemoteRole 2, Owner and Controller the PlayerController, the PlayerState (910 packet 307)");
    CHECK(stats_rider && owner_arrays, "its StatsComponent: the rider's owner stat arrays");
    CHECK(known && rider_ptr && interaction, "its KnownAbilities, RiderPtr and interaction state");
    // the rider's pawn: attached at MountSocket
    bool pawn_attached = false, mount_ptr = false;
    for (const fake::Delivered* d : chains_on(h, h.pawn_ch, from)) {
        const States st = states_of(*d, h.s->bootstrap().pawn());
        for (const auto& [id, o] : st) {
            if (id == 0) {
                pawn_attached = int_of(o, "RemoteRole") == 1 && object_of(o, "AttachmentReplication.AttachParent") == mount_guid &&
                                object_of(o, "PlayerState") == emit::FIntrepidNetworkGUID{} && object_of(o, "Controller") == emit::FIntrepidNetworkGUID{} &&
                                int_of(o, "AttachmentState.bAttachParentValid") == 1 && int_of(o, "AttachmentState.InteractableZoneID") == 2;
            } else if (o.schema && o.schema->command("MountPtr")) {
                mount_ptr = object_of(o, "MountPtr") == mount_guid;
            }
        }
    }
    CHECK(pawn_attached, "the rider's pawn is attached to the mount (MountSocket), with no PlayerState or Controller (910 packet 310)");
    CHECK(mount_ptr, "its InteractInfo names the mount (MountPtr)");
    bool pc_attached = false;
    for (const fake::Delivered* d : chains_on(h, h.pc_ch, from)) {
        const States st = states_of(*d, h.s->bootstrap().player_controller());
        const auto it = st.find(0);
        if (it != st.end()) {
            pc_attached = pc_attached || (object_of(it->second, "Pawn") == mount_guid && object_of(it->second, "AttachmentReplication.AttachParent") == mount_guid &&
                                          object_of(it->second, "AttachmentReplication.AttachComponent") == cylinder);
        }
    }
    CHECK(pc_attached, "the PlayerController is attached to the mount and its Pawn is the mount");
    // the client acknowledges the possession of the mount; the load follows
    h.client_sends({h.client.actor(h.pc_ch, possession_ack(mount_guid), true)});
    h.run(milliseconds(100));
    CHECK(!h.s->failed(), "the session takes the acknowledgement of the mount's possession");
    // the controlled actor's limit
    CHECK(h.s->ride() && h.s->ride()->speed_limit(1, 0) > 1400 && h.s->ride()->speed_limit(1, 0) < 1500, "the mount's move limit is its Blueprint's 990 cm/s times 1.5");
    CHECK(h.s->movement().good().x == m->x && h.s->movement().good().y == m->y, "the session's last good position is the mount's");
// [C_Y_B :: CoA]

    // getting off
    const size_t from2 = h.got.size();
    get_off(h);
    h.run(milliseconds(200));
    CHECK(!h.s->mounted() && h.s->dismounts() == 1, "ServerDismountInteraction puts the rider off the mount");
    bool restart_pawn = false;
    size_t restarts = 0;
    for (const fake::Delivered* d : chains_on(h, h.pc_ch, from2)) {
        for (const repl::RpcCall& c : calls_of(*d, "AoCPlayerControllerBP_C")) {
            if (!c.rpc || std::string(c.rpc->name) != "ClientRestart") continue;
            const auto it = c.params.values.find(1);
            const auto* o = it != c.params.values.end() ? it->second.get_if<emit::FIntrepidNetworkGUID>() : nullptr;
            restart_pawn = o && *o == g.pawn;
            ++restarts;
        }
    }
    CHECK(restart_pawn && restarts == 1, "the PlayerController is restarted on its own pawn, once (912 packet 139)");
    bool detached = false, detach_place = false, unmounted_ptr = false;
    const auto yaw = m->yaw_degrees * 3.14159265358979 / 180.0;
    for (const fake::Delivered* d : chains_on(h, h.pawn_ch, from2)) {
        const States st = states_of(*d, h.s->bootstrap().pawn());
        for (const auto& [id, o] : st) {
            if (id == 0) {
                detached = int_of(o, "RemoteRole") == 2 && object_of(o, "AttachmentReplication.AttachParent") == emit::FIntrepidNetworkGUID{} &&
                           object_of(o, "PlayerState") == g.player_state && object_of(o, "Controller") == g.player_controller && int_of(o, "AttachmentState.bAttachParentValid") == 0;
                const repl::Value* loc = o.state.get(*o.schema, "RepDetachmentLocation");
                const auto* v = loc ? loc->get_if<repl::Vec3d>() : nullptr;
                detach_place = v && std::fabs(v->x - (m->x + 54 * std::cos(yaw))) < 1.0 && std::fabs(v->y - (m->y + 54 * std::sin(yaw))) < 1.0 && std::fabs(v->z - (m->z + 197)) < 1.0;
            } else if (o.schema && o.schema->command("MountPtr")) {
                unmounted_ptr = object_of(o, "MountPtr") == emit::FIntrepidNetworkGUID{};
            }
        }
    }
    CHECK(detached && unmounted_ptr, "the pawn is detached: RemoteRole 2, its PlayerState and Controller back, MountPtr cleared (912 packet 139)");
    CHECK(detach_place, "RepDetachmentLocation is 54 cm ahead of the mount and 197 cm above its origin (912)");
    bool mount_released = false;
    for (const fake::Delivered* d : chains_on(h, mch, from2)) {
        const States st = states_of(*d, h.mount()->actor);
        const auto it = st.find(0);
        if (it != st.end()) mount_released = int_of(it->second, "RemoteRole") == 1 && object_of(it->second, "Owner") == emit::FIntrepidNetworkGUID{} && object_of(it->second, "Controller") == emit::FIntrepidNetworkGUID{};
    }
    CHECK(mount_released, "the mount's Owner, Controller and PlayerState are released, RemoteRole 1 (912 packet 139)");
    CHECK(h.s->mounts()->phase() == MountController::Phase::Out && mount_count(h) == 1, "the mount stays");
    CHECK(h.s->movement().good().z > m->z + 150, "the pawn's checker starts where it lands");
}


// ---- riding --------------------------------------------------------------------------------------------------------------------------------

/// The rider on its mount: where its client has the mount and its clock (a possession starts the TimeStamp near 0).
struct Rider {
    double x = 0, y = 0, z = 0;
    float  ts = 0;
    uint16_t yaw = 0;
};

static Rider start_riding(Harness& h) {
    Rider r;
    const GoodPosition g = h.s->movement().good();
    r.x = g.x;
    r.y = g.y;
    r.z = g.z;
    r.yaw = yaw16_of(g.yaw_degrees);
    return r;
}

/// One move of the mounted client: `dt` seconds on its clock and the test's, `(dx, dy, dz)` cm.
static void ride_step(Harness& h, Rider& r, double dx, double dy, double dz = 0, double dt = 0.1, uint8_t mode = repl::kMoveWalking, uint16_t pitch = 0) {
    r.ts += static_cast<float>(dt);
    r.x += dx;
    r.y += dy;
    r.z += dz;
    move_mount(h, move_at(r.ts, r.x, r.y, r.z, r.yaw, mode, pitch));
    h.step(milliseconds(static_cast<int>(std::lround(dt * 1000))));
}

static emit::FIntrepidNetworkGUID ability_component_of(const world::WorldActor& m) {
    const repl::Subobject* a = spawns::sub_of(m, "UAoCAbilityComponent");
    return a ? a->guid : emit::FIntrepidNetworkGUID{};
}

/// A fast array's items of an object of the mount's actor (the registry's), by the field's name.
static std::vector<repl::ElementState> items_of(const world::WorldActor& m, const char* cls, const char* field) {
    std::vector<repl::ElementState> out;
    const repl::Subobject* s = spawns::sub_of(m, cls);
    const schema::NetField* f = s ? s->obj.schema->field(field) : nullptr;
    const repl::FastArrayState* a = f ? s->obj.arrays.find(f->index) : nullptr;
    if (a) {
        for (const repl::FastArrayItem& it : a->items()) out.push_back(it.values);
    }
    return out;
}

static std::optional<double> mult_of(const world::WorldActor& m) { return spawns::stat_value(m, spawns::kStatMoveSpeedMult); }

static uint16_t pitch16_of(double degrees) { return static_cast<uint16_t>(static_cast<int>(std::lround(degrees * 65536.0 / 360.0)) & 0xFFFF); }

/// The character on the nav fixture's flat ground (terrain z 1,000, a mount's origin 1,057, a player's 1,098) near the world's centre.
static svc::Character cyb_on_the_ground() {
    svc::Character c = cyb_record();
    c.has_position = true;
    c.position = {0.0, 0.0, 1098.0, 0.0f};
    return c;
}

static void adopt_the_ground(PlayerWorld& w) {
    std::string err;
    auto nav = aoc::worldd::nav::AreaNav::from_file(aoc::worldd::navfix::build_file(), &err);
    if (!nav || !w.adopt_ground(std::move(nav), err)) {
        std::printf("  [FAIL] the ground does not load: %s\n", err.c_str());
        std::exit(2);
    }
}

// ---- the GM travel tools ------------------------------------------------------------------------------------------------------------------------------

static constexpr int64_t kMoveSpeedMult = 109228544644729ll;
static std::unique_ptr<world::CityNodes> g_nodes;   // the real content.db's city nodes (places)

static bool near_to(double got, double want, double tol = 0.5) { return std::fabs(got - want) <= tol; }

/// The GM travel tools over the harness's one player (the Endpoint's directory, in a real worldd).
static GmTravel make_travel(Harness& h) {
    return GmTravel(*h.fx.world, [&h] { return std::vector<PlayerHandle>{PlayerHandle{h.s.get(), &h.conn}}; }, [&h] { return h.t; });
}

static void with_nodes(PlayerWorld& w) { w.set_city_nodes(g_nodes.get()); }

static void on_the_ground(PlayerWorld& w) {
    adopt_the_ground(w);
    with_nodes(w);
}

/// One move of the pawn's client: `dt` seconds on its clock and the test's, to (x, y, z).
static void pawn_move_to(Harness& h, double x, double y, double z, double dt = 0.2) {
    g_pawn_ts += static_cast<float>(dt);
    move_pawn(h, move_at(g_pawn_ts, x, y, z, yaw16_of(h.s->movement().good().yaw_degrees)));
    h.step(milliseconds(static_cast<int>(std::lround(dt * 1000))));
}

static GmTeleportRequest to_point(double x, double y) {
    GmTeleportRequest rq;
    rq.character = "cyb";
    rq.x = x;
    rq.y = y;
    return rq;
}

static GmTeleportRequest to_point_z(double x, double y, double z) {
    GmTeleportRequest rq = to_point(x, y);
    rq.z = z;
    rq.has_z = true;
    return rq;
}
// (C O A : C | Y | B)

static GmTeleportRequest to_place(const char* place) {
    GmTeleportRequest rq;
    rq.character = "cyb";
    rq.place = place;
    return rq;
}

static void test_teleport_to_a_point() {
    std::printf("teleport on foot: a point, the ground's height, the checker rebased\n");
    Harness h(cyb_on_the_ground(), on_the_ground);
    h.play();
    GmTravel travel = make_travel(h);
    walk_pawn_to(h, 600, 0);   // the checker holds a TimeStamp
    CHECK(h.s->moves_refused() == 0 && h.s->moves_acknowledged() > 0, "(the player walks)");
    const size_t from = h.got.size();
    const GmTeleportResult r = travel.teleport(to_point(5000, 3000), "the test");   // no z: the ground's
    CHECK(r.status == GmStatus::Ok && r.character == "cyb" && r.ground_height && !r.dismounted, "a point with no z, over ground the server has: done, the height is the ground's");
    CHECK(near_to(r.to.x, 5000) && near_to(r.to.y, 3000) && near_to(r.to.z, 1098), "the player stands on the ground (terrain 1,000 + the capsule's 98 cm)");
    CHECK(near_to(r.from.x, 600, 5) && near_to(r.from.y, 0, 5), "it says where the player was");
    const GoodPosition g = h.s->movement().good();
    CHECK(near_to(g.x, 5000) && near_to(g.y, 3000) && near_to(g.z, 1098), "the move checker's last good position is the new place");
    h.run(milliseconds(100));
    CHECK(corrections_on(h, h.pawn_ch, from, "PlayerPawn_C") == 1, "the client is sent one ClientAdjustPosition");
    CHECK(h.s->gm_teleports() == 1, "(the teleport is counted)");
    // the checker was rebased: the next move from the new place is accepted and not corrected ...
    const size_t acked = h.s->moves_acknowledged(), refused = h.s->moves_refused();
    pawn_move_to(h, 5060, 3000, 1098);
    CHECK(h.s->moves_acknowledged() == acked + 1 && h.s->moves_refused() == refused, "the next move, from the new place, is accepted and not corrected");
    // ... and a move from the old place is not
    pawn_move_to(h, 660, 0, 1098);
    CHECK(h.s->moves_refused() == refused + 1, "a move from the old place is refused (it is a teleport back)");
    // saved at once
    CHECK(!h.sink.positions.empty() && near_to(h.sink.positions.back().x, 5000) && near_to(h.sink.positions.back().y, 3000), "the new position is handed to the saver at once");

    // a point with z: z is taken as given
    const GmTeleportResult with_z = travel.teleport(to_point_z(-4000, 1200, 1500), "the test");
    CHECK(with_z.status == GmStatus::Ok && !with_z.ground_height && near_to(with_z.to.z, 1500), "a point with z: z as given");
    // no ground there, and no z: refused, asking for z
    const GmTeleportResult none = travel.teleport(to_point(100000, 100000), "the test");
    CHECK(none.status == GmStatus::InvalidArgument && none.message.find("give z") != std::string::npos, "a point with no ground under it and no z is refused, asking for z");
    CHECK(near_to(h.s->movement().good().x, -4000), "(and nothing moved)");
    const GmTeleportResult far_with_z = travel.teleport(to_point_z(100000, 100000, 12000), "the test");
    CHECK(far_with_z.status == GmStatus::Ok && near_to(far_with_z.to.z, 12000), "the same point with z is done");
    // refusals
    CHECK(travel.teleport(to_point_z(5.0e6, 0, 1000), "the test").status == GmStatus::InvalidArgument, "a point outside the world is refused");
    CHECK(travel.teleport(to_point_z(std::numeric_limits<double>::quiet_NaN(), 0, 1000), "the test").status == GmStatus::InvalidArgument, "a NaN is refused");
    CHECK(travel.teleport(to_point_z(0, std::numeric_limits<double>::infinity(), 1000), "the test").status == GmStatus::InvalidArgument, "an infinity is refused");
    GmTeleportRequest nobody = to_point_z(0, 0, 1000);
    nobody.character = "nobody";
    CHECK(travel.teleport(nobody, "the test").status == GmStatus::NotFound, "an unknown character is NotFound");
    nobody.character = "";
    CHECK(travel.teleport(nobody, "the test").status == GmStatus::InvalidArgument, "no character is InvalidArgument");
    GmTeleportRequest upper = to_point_z(0, 0, 1100);
    upper.character = "CyB";
    CHECK(travel.teleport(upper, "the test").status == GmStatus::Ok, "the character's name is matched without case");
}

static void test_teleport_to_a_place() {
    std::printf("teleport to a place: the city nodes by name, and login\n");
    Harness h(cyb_on_the_ground(), on_the_ground);
    h.play();
    GmTravel travel = make_travel(h);
    const world::CityNodeInfo* joeva = g_nodes->by_name("Joeva");
    const world::CityNodeInfo* aela = g_nodes->by_name("New Aela");
    CHECK(joeva && aela, "(the registry has Joeva and New Aela)");
    if (!joeva || !aela) return;
    GmTeleportResult r = travel.teleport(to_place("joeva"), "the test");
    CHECK(r.status == GmStatus::Ok && r.place == "Verra_RVR_Joeva", "'joeva' is Verra_RVR_Joeva");
    CHECK(near_to(r.to.x, joeva->x, 0.01) && near_to(r.to.y, joeva->y, 0.01) && near_to(r.to.z, joeva->z + PlayerWorld::kPlaceLiftCm, 0.01),
          "the player is put at the node's recorded place, 150 cm above its z where the server has no ground");
    CHECK(near_to(h.s->movement().good().x, joeva->x, 0.01), "(the checker is there)");
    for (const char* spelling : {"New Aela", "Verra_RVR_NewAela", "RVR_NewAela", "newaela", "NEW_AELA"}) {
        r = travel.teleport(to_place(spelling), "the test");
        CHECK(r.status == GmStatus::Ok && r.place == "Verra_RVR_NewAela" && near_to(r.to.x, aela->x, 0.01), (std::string("'") + spelling + "' is New Aela").c_str());
    }
    r = travel.teleport(to_place("login"), "the test");
    const world::Places places = h.fx.world->places();
    const world::Place* login = places.find("login");
    CHECK(r.status == GmStatus::Ok && login && near_to(r.to.x, login->x, 0.01) && near_to(r.to.y, login->y, 0.01), "'login' is the default spawn");
    CHECK(travel.teleport(to_place("Login Spot"), "the test").status == GmStatus::Ok, "'Login Spot' is the same place");
    // places that are not there
    for (const char* bad : {"Narnia", "Bucktown", "Mt. Dun", "Open Ocean", "RVR"}) {
        const GmTeleportResult nope = travel.teleport(to_place(bad), "the test");
        CHECK(nope.status == GmStatus::NotFound, (std::string("'") + bad + "' is no place (test maps, nodes with no location, unknown names)").c_str());
    }
    // the list
    const std::vector<world::Place> list = travel.list_places();
    size_t expected = 1;
    for (const world::CityNodeInfo& n : g_nodes->nodes()) expected += world::has_travel_place(n) ? 1 : 0;
    CHECK(list.size() == expected && list.size() > 30 && list.size() < 81, "the places are the login spot and the nodes that have a place on the Verra map");
    bool has_joeva = false, has_lionhold = false, has_test = false, has_origin = false;
    for (const world::Place& p : list) {
        has_joeva = has_joeva || p.name == "Verra_RVR_Joeva";
        has_lionhold = has_lionhold || p.name == "Verra_RVR_Lionhold";
        has_test = has_test || p.name == "Node_Playground_Bucktown";
        has_origin = has_origin || (p.kind == "node" && p.x == 0 && p.y == 0);
    }
    CHECK(has_joeva && has_lionhold && !has_test && !has_origin, "Joeva and Lionhold are there; the test maps' nodes and the nodes at the origin are not");
    // the GM garden's centre is one more place once the world has placed the garden (PlayerWorld::places adds it); here the Places side of it
    world::Places with_garden = world::Places::build(g_nodes.get(), 0, 0, 1098);
    CHECK(with_garden.find("garden") == nullptr, "no garden place before one is added");
    world::Place garden;
    garden.name = garden.display_name = garden.kind = "garden";
    garden.x = 1234;
    garden.y = 5678;
    garden.z = 90;
    with_garden.add(garden);
    const world::Place* found_garden = with_garden.find("Garden");
    CHECK(found_garden && near_to(found_garden->x, 1234, 0.01) && near_to(found_garden->y, 5678, 0.01) && found_garden->kind == "garden", "'garden' is the added place");
}
// [C | O | A - C:Y:B]

// ---- a town in a bowl (the test-2 fix: world/town_floor.h) -------------------------------------------------------------------------------------

/// The fixture's plain (terrain 1,000) with a town on it: 7 x 7 people on a 20 m grid round (cx, cy), standing 50.6 m over the terrain, as Joeva's do over its bowl.
static void a_bowl_at(PlayerWorld& w, double cx, double cy) {
    on_the_ground(w);
    std::vector<world::TownFloor::Person> people;
    for (int i = -3; i <= 3; ++i) {
        for (int j = -3; j <= 3; ++j) people.push_back({cx + i * 2000.0, cy + j * 2000.0, 1000.0, 1000.0 + 5060.0});
    }
    w.town_floor().set(4242, people);
}
static void a_bowl_at_5000_3000(PlayerWorld& w) { a_bowl_at(w, 5000, 3000); }
static void a_bowl_at_the_origin(PlayerWorld& w) { a_bowl_at(w, 0, 0); }

static constexpr double kTownFloor = 1000.0 + 5060.0;   // the town's floor (the baked plain + the people's 50.6 m)

static void test_teleport_into_a_bowl() {
    std::printf("teleport into a town's bowl: the town's floor, not the baked ground 50 m under it\n");
    Harness h(cyb_on_the_ground(), a_bowl_at_5000_3000);
    h.play();
    GmTravel travel = make_travel(h);
    // a point in the town, no z: the floor + the slack + the capsule
    GmTeleportResult r = travel.teleport(to_point(5000, 3000), "the test");
    CHECK(r.status == GmStatus::Ok && r.ground_height && near_to(r.to.z, kTownFloor + PlayerWorld::kBowlSlackCm + PlayerWorld::kCapsuleHalfHeightCm),
          "a point in the town with no z: the player is put on the town's floor (50.6 m over the baked plain) plus the slack");
    CHECK(near_to(h.s->movement().good().z, kTownFloor + PlayerWorld::kBowlSlackCm + PlayerWorld::kCapsuleHalfHeightCm), "(the checker is there)");
    // the same point asked with a z: believed
    r = travel.teleport(to_point_z(5000, 3000, 1500), "the test");
    CHECK(r.status == GmStatus::Ok && !r.ground_height && near_to(r.to.z, 1500), "a GM who names z is believed (in a bowl too)");
    // a point 200 m from the town: the plain
    r = travel.teleport(to_point(5000 + 20000, 3000), "the test");
    CHECK(r.status == GmStatus::Ok && near_to(r.to.z, 1098), "200 m from the town: the baked ground + the capsule, as before");
    // the client's own GM teleport (ServerTeleportXYZ) goes through the same ground
    CHECK(h.fx.world->ground_at(5000, 3000) && near_to(*h.fx.world->ground_at(5000, 3000), kTownFloor + PlayerWorld::kBowlSlackCm), "(the world's ground_at says so for every caller)");
}

static void test_login_under_a_town() {
    std::printf("login: a character saved on the baked ground under a town is put on its floor\n");
    std::ostringstream log;
    auto previous = spdlog::default_logger();
    auto logger = std::make_shared<spdlog::logger>("bowl_login", std::make_shared<spdlog::sinks::ostream_sink_mt>(log));
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(logger);
    {
        // the user's: saved at the terrain + the capsule under Joeva's town
        Harness h(cyb_on_the_ground(), a_bowl_at_the_origin);
        const double want = kTownFloor + PlayerWorld::kCapsuleHalfHeightCm + PlayerWorld::kBowlSlackCm;
        CHECK(near_to(h.s->movement().good().z, want) && near_to(h.s->movement().good().x, 0, 0.01), "a character saved at the plain's height under the town joins on the town's floor (+ slack)");
        std::istringstream lines(log.str());
        std::string line, said;
        size_t said_count = 0;
        while (std::getline(lines, line)) {
            if (line.find("under the floor of a town in a bowl") != std::string::npos) {
                said = line;
                ++said_count;
            }
        }
        CHECK(said_count == 1 && said.find("[session] cyb") != std::string::npos, "...and exactly one line says so");
        std::printf("    %s\n", said.c_str());
    }
    {
        // saved standing on the floor: left alone
        svc::Character c = cyb_on_the_ground();
        c.position.z = kTownFloor + PlayerWorld::kCapsuleHalfHeightCm;
        log.str("");
        Harness h(c, a_bowl_at_the_origin);
        CHECK(near_to(h.s->movement().good().z, kTownFloor + PlayerWorld::kCapsuleHalfHeightCm) && log.str().find("under the floor of a town in a bowl") == std::string::npos, "a character saved on the town's floor joins where it was, with no line");
    }
    {
        // saved on the open ground, far from the town: left alone
        svc::Character c = cyb_on_the_ground();
        c.position.x = 30000;
        Harness h(c, a_bowl_at_the_origin);
        CHECK(near_to(h.s->movement().good().z, 1098.0), "a character saved on the open ground 300 m from the town joins where it was");
    }
    {
        // a world with no town: nothing to lift
        Harness h(cyb_on_the_ground(), on_the_ground);
        CHECK(near_to(h.s->movement().good().z, 1098.0), "a world with no bowl: the character joins where it was");
    }
    spdlog::set_default_logger(previous);
}
// (CoA) (C-Y-B)

// ---- the towns' real ground: a bowl whose floor is the layout's platform (world/town_platform.h) ---------------------------------------

/// The fixture's plain (terrain 1,000 everywhere: the pit) with a platform bowl on it: a disc of 27 m round (cx, cy) whose floor is `floor` + `slope` * u.
static void a_platform_bowl(PlayerWorld& w, double cx, double cy, double floor, double slope = 0.0) {
    on_the_ground(w);
    w.set_town_platforms(tpfix::platforms_at(4242, cx, cy, floor, 0.0, tpfix::grid(30, 0.0, slope, 2700.0)));
}
static void a_platform_bowl_at_5000_3000(PlayerWorld& w) { a_platform_bowl(w, 5000, 3000, kTownFloor, 0.05); }
static void a_platform_bowl_at_the_origin(PlayerWorld& w) { a_platform_bowl(w, 0, 0, kTownFloor); }

static void test_teleport_into_a_platform_bowl() {
    std::printf("teleport into a bowl whose floor is the platform: its height at the point, not the baked pit, not the model\n");
    Harness h(cyb_on_the_ground(), a_platform_bowl_at_5000_3000);
    h.play();
    GmTravel travel = make_travel(h);
    GmTeleportResult r = travel.teleport(to_point(5000, 3000), "the test");
    CHECK(r.status == GmStatus::Ok && r.ground_height && near_to(r.to.z, kTownFloor + PlayerWorld::kBowlSlackCm + PlayerWorld::kCapsuleHalfHeightCm),
          "the platform's centre: the floor + the slack + the capsule (a pit of 1,000 under it)");
    r = travel.teleport(to_point(5000 + 2000, 3000 - 500), "the test");
    CHECK(r.status == GmStatus::Ok && near_to(r.to.z, kTownFloor + 100.0 + PlayerWorld::kBowlSlackCm + PlayerWorld::kCapsuleHalfHeightCm),
          "20 m along the slope (5 cm per metre): the floor there is 100 cm higher, and the player is put on that");
    CHECK(h.fx.world->ground_at(5000, 3000) && near_to(*h.fx.world->ground_at(5000, 3000), kTownFloor + PlayerWorld::kBowlSlackCm), "(the world's ground_at says so for every caller)");
    r = travel.teleport(to_point(5000 + 20000, 3000), "the test");
    CHECK(r.status == GmStatus::Ok && near_to(r.to.z, 1098), "200 m from the town: the baked ground + the capsule, as before");
    r = travel.teleport(to_point_z(5000, 3000, 9000), "the test");
    CHECK(r.status == GmStatus::Ok && !r.ground_height && near_to(r.to.z, 9000), "a z that is given is believed (a tower)");
    h.run(milliseconds(4000));
    CHECK(h.s->floor_net_rescues() == 0 && near_to(h.s->movement().good().z, 9000), "...and the net leaves a player who is above the floor alone");
}

static void test_a_hill_through_a_platform_town() {
    std::printf("a hill of the landscape that pokes through a platform town is the ground where it is higher\n");
    // the nav fixture's hill (a cone of 400 cm over 3,000 cm at (6000, 0), the plain at 1,000) under a platform bowl whose floor is 1,200
    Harness h(cyb_on_the_ground(), [](PlayerWorld& w) { a_platform_bowl(w, 6000, 0, 1200.0); });
    h.play();
    GmTravel travel = make_travel(h);
    GmTeleportResult r = travel.teleport(to_point(6000, 0), "the test");
    CHECK(r.status == GmStatus::Ok && near_to(r.to.z, 1400.0 + PlayerWorld::kBowlSlackCm + PlayerWorld::kCapsuleHalfHeightCm), "on the hill top (the landscape 1,400, the platform 1,200) the player is put on the hill");
    r = travel.teleport(to_point(6000 + 2500, 0), "the test");
    CHECK(r.status == GmStatus::Ok && near_to(r.to.z, 1200.0 + PlayerWorld::kBowlSlackCm + PlayerWorld::kCapsuleHalfHeightCm), "past the hill's foot (the landscape at 1,067) on the platform");
    h.run(milliseconds(2500));
    CHECK(h.s->floor_net_rescues() == 0, "(the net does not touch either)");
}

static void test_login_under_a_platform_town() {
    std::printf("login: a character saved on the baked pit under a platform town joins on the platform's floor\n");
    std::ostringstream log;
    auto previous = spdlog::default_logger();
    auto logger = std::make_shared<spdlog::logger>("platform_login", std::make_shared<spdlog::sinks::ostream_sink_mt>(log));
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(logger);
    {
        Harness h(cyb_on_the_ground(), a_platform_bowl_at_the_origin);
        const double want = kTownFloor + PlayerWorld::kCapsuleHalfHeightCm + PlayerWorld::kBowlSlackCm;
        CHECK(near_to(h.s->movement().good().z, want) && near_to(h.s->movement().good().x, 0, 0.01), "saved at the pit's height (1,098) under the town: joins at the floor + the capsule + the slack");
        CHECK(log.str().find("under the floor of a town in a bowl") != std::string::npos, "...and the log says so");
    }
    {
        svc::Character c = cyb_on_the_ground();
        c.position.z = kTownFloor + PlayerWorld::kCapsuleHalfHeightCm + 300.0;   // a few metres up (a stair, a rooftop)
        Harness h(c, a_platform_bowl_at_the_origin);
        CHECK(near_to(h.s->movement().good().z, kTownFloor + PlayerWorld::kCapsuleHalfHeightCm + 300.0), "saved above the floor: joins where it was");
    }
    {
        svc::Character c = cyb_on_the_ground();
        c.position.z = kTownFloor + PlayerWorld::kCapsuleHalfHeightCm - 2000.0;   // 20 m under the floor: the outer ring, where the landscape is the natural slope at the rim's foot
        Harness h(c, a_platform_bowl_at_the_origin);
        CHECK(near_to(h.s->movement().good().z, kTownFloor + PlayerWorld::kCapsuleHalfHeightCm - 2000.0), "saved 20 m under the floor: left alone (only more than 25 m under it is the pit under the town)");
    }
    {
        svc::Character c = cyb_on_the_ground();
        c.position.z = kTownFloor + PlayerWorld::kCapsuleHalfHeightCm - 3000.0;   // 30 m under the floor
        Harness h(c, a_platform_bowl_at_the_origin);
        CHECK(near_to(h.s->movement().good().z, kTownFloor + PlayerWorld::kCapsuleHalfHeightCm + PlayerWorld::kBowlSlackCm), "saved 30 m under the floor: put on it");
    }
    spdlog::set_default_logger(previous);
}
// built with C-Y-B and CoA

static void test_the_floor_net() {
    std::printf("the floor net: a player far under a town's floor, or under the kill plane, is put back on it\n");
    std::ostringstream log;
    auto previous = spdlog::default_logger();
    auto logger = std::make_shared<spdlog::logger>("floor_net", std::make_shared<spdlog::sinks::ostream_sink_mt>(log));
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(logger);
    {
        Harness h(cyb_on_the_ground(), a_platform_bowl_at_5000_3000);
        h.play();
        GmTravel travel = make_travel(h);
        const double on_floor = kTownFloor + PlayerWorld::kCapsuleHalfHeightCm + PlayerWorld::kBowlSlackCm;   // the town's centre has a slope of 0.05 * u = 0 there
        // a long teleport into the town: the client had no floor yet and fell; here a GM says the player is 500 m under the town
        const size_t from = h.got.size();
        GmTeleportResult r = travel.teleport(to_point_z(5000, 3000, -50000), "the test");
        CHECK(r.status == GmStatus::Ok && near_to(r.to.z, -50000), "(a GM puts the player 500 m under the town: a z that is given is believed)");
        h.run(milliseconds(800));
        CHECK(h.s->floor_net_rescues() == 0 && near_to(h.s->movement().good().z, -50000), "0.8 s later it is still there (the net waits a second: one bad position is not a fall)");
        h.run(milliseconds(500));
        CHECK(h.s->floor_net_rescues() == 1 && near_to(h.s->movement().good().z, on_floor) && near_to(h.s->movement().good().x, 5000, 0.01) && near_to(h.s->movement().good().y, 3000, 0.01),
              "a second later it is back on the floor (floor + 98 + 200), at the same x and y");
        CHECK(corrections_on(h, h.pawn_ch, from, "PlayerPawn_C") == 2, "the client is sent two position corrections: the GM's and the net's");
        CHECK(!h.sink.positions.empty() && near_to(h.sink.positions.back().z, on_floor), "the saved position is the floor's (a crash must not take the player back under)");
        CHECK(log.str().find("[floor-net] cyb:") != std::string::npos && log.str().find("under the floor of") != std::string::npos, "the log says who, where and whose floor");
        std::istringstream lines(log.str());
        std::string line;
        while (std::getline(lines, line)) {
            if (line.find("[floor-net] cyb: its last") != std::string::npos) std::printf("    %s\n", line.c_str());
        }
        // the client fell again at once (its floor had not streamed in yet): the net waits 3 s from the last rescue, then puts it back again
        r = travel.teleport(to_point_z(5000, 3000, -50000), "the test");
        h.run(milliseconds(1500));
        CHECK(h.s->floor_net_rescues() == 1, "a player that is under again 0.1 s after a rescue is not rescued again within 3 s of it");
        h.run(milliseconds(2000));
        CHECK(h.s->floor_net_rescues() == 2 && near_to(h.s->movement().good().z, on_floor), "...but is, once the 3 s have passed");
        // 90 m under the floor is not far enough (the pit is 50 m); 101 m is
        travel.teleport(to_point_z(5000, 3000, kTownFloor - 9000.0), "the test");
        h.run(milliseconds(4500));
        CHECK(h.s->floor_net_rescues() == 2 && near_to(h.s->movement().good().z, kTownFloor - 9000.0), "90 m under the floor: left alone");
        travel.teleport(to_point_z(5000, 3000, kTownFloor - 10100.0), "the test");
        h.run(milliseconds(1500));
        CHECK(h.s->floor_net_rescues() == 3 && near_to(h.s->movement().good().z, on_floor), "101 m under the floor: put back");
        // standing in the pit: the landscape's collision caught the pawn 50 m under the town (the fixture's plain, 1,000, is 50.6 m under the floor); in the air above it, or under it, it is not
        // standing on it
        travel.teleport(to_point_z(5000, 3000, 1000.0 + PlayerWorld::kCapsuleHalfHeightCm + 1000.0), "the test");
        h.run(milliseconds(4500));
        CHECK(h.s->floor_net_rescues() == 3 && near_to(h.s->movement().good().z, 1000.0 + PlayerWorld::kCapsuleHalfHeightCm + 1000.0), "10 m over the pit, in the air: left alone (a fall through that height is not standing on it)");
        travel.teleport(to_point_z(5000, 3000, 1000.0 + PlayerWorld::kCapsuleHalfHeightCm), "the test");
        h.run(milliseconds(1500));
        CHECK(h.s->floor_net_rescues() == 4 && near_to(h.s->movement().good().z, on_floor), "standing on the baked pit 50 m under the town for a second: put back on the floor");
        CHECK(log.str().find("stands on the pit") != std::string::npos, "...and the log says it was the pit");
        // outside every bowl: the kill plane (z -10,000) and the ground under the point
        travel.teleport(to_point_z(5000 + 20000, 3000, -9000.0), "the test");
        h.run(milliseconds(4500));
        CHECK(h.s->floor_net_rescues() == 4 && near_to(h.s->movement().good().z, -9000.0), "over the open plain, above the kill plane: left alone");
        travel.teleport(to_point_z(5000 + 20000, 3000, -20000.0), "the test");
        h.run(milliseconds(1500));
        CHECK(h.s->floor_net_rescues() == 5 && near_to(h.s->movement().good().z, 1000.0 + PlayerWorld::kCapsuleHalfHeightCm + PlayerWorld::kBowlSlackCm) && near_to(h.s->movement().good().x, 25000, 0.01),
              "over the open plain under the kill plane: put back on the baked ground (+ 98 + 200)");
        // a dead player is left to its respawn
        travel.teleport(to_point_z(5000, 3000, -50000), "the test");
        h.s->set_current_value(content::kGuidHealth, 0.0f);
        h.run(milliseconds(1800));
        CHECK(h.s->dead() && h.s->floor_net_rescues() == 5 && near_to(h.s->movement().good().z, -50000), "a dead player under the town is not touched by the net (it comes back at a shrine)");
    }
    {
        // without any platform (the file is missing) the kill plane still holds in a bowl of the interim model's
        Harness h(cyb_on_the_ground(), a_bowl_at_5000_3000);
        h.play();
        GmTravel travel = make_travel(h);
        travel.teleport(to_point_z(5000, 3000, -50000), "the test");
        h.run(milliseconds(1500));
        CHECK(h.s->floor_net_rescues() == 1 && near_to(h.s->movement().good().z, kTownFloor + PlayerWorld::kBowlSlackCm + PlayerWorld::kCapsuleHalfHeightCm), "without a platform the kill plane puts the player on the interim model's floor");
    }
    {
        // before the phase-in the pawn is held at the join height: the net does not run
        Harness h(cyb_on_the_ground(), a_platform_bowl_at_5000_3000);
        h.run(milliseconds(1500));
        CHECK(h.s->floor_net_rescues() == 0, "(no rescue before the client's pawn has phased in)");
    }
    spdlog::set_default_logger(previous);
}

static void test_teleport_refusals() {
    std::printf("teleport: the dead are not moved; the player list\n");
    Harness h(cyb_on_the_ground(), on_the_ground);
    h.play();
    GmTravel travel = make_travel(h);
    std::vector<GmPlayerInfo> players = travel.list_players();
    CHECK(players.size() == 1 && players[0].name == "cyb" && players[0].character_id == kCyb && !players[0].mounted && !players[0].dead && players[0].move_speed_multiplier == 1.0,
          "the player list: name, guid, not mounted, speed 1");
    CHECK(near_to(players[0].position.x, 0, 1) && near_to(players[0].position.z, 1098, 1), "(and its position)");
    h.s->set_current_value(content::kGuidHealth, 0.0f);
    h.run(milliseconds(300));
    CHECK(h.s->dead(), "(the player is dead)");
    const GoodPosition before = h.s->movement().good();
    const GmTeleportResult r = travel.teleport(to_point_z(5000, 3000, 1100), "the test");
    CHECK(r.status == GmStatus::FailedPrecondition && near_to(h.s->movement().good().x, before.x), "a dead player is not moved");
    players = travel.list_players();
    CHECK(players.size() == 1 && players[0].dead, "(the list says it is dead)");
}

static void test_teleport_mounted() {
    std::printf("teleport while mounted: the ride is over first, the mount desummons by distance\n");
    Harness h(cyb_record(), with_nodes);
    h.play();
    GmTravel travel = make_travel(h);
    CHECK(ride_on(h), "(the player rides)");
    if (!h.s->mounted()) return;
    Rider r = start_riding(h);
    for (int i = 0; i < 5; ++i) ride_step(h, r, 60, 0);
    CHECK(travel.list_players().size() == 1 && travel.list_players()[0].mounted, "(the list says it is mounted)");
    const GoodPosition on_mount = h.s->movement().good();
    const size_t from = h.got.size();
    // 200 m from the mount: the ride is over, the mount stays
    GmTeleportResult t = travel.teleport(to_point_z(on_mount.x + 20000, on_mount.y, on_mount.z), "the test");
    CHECK(t.status == GmStatus::Ok && t.dismounted && !h.s->mounted() && h.s->dismounts() == 1, "the player is dismounted first (the ride is over)");
    CHECK(near_to(h.s->movement().good().x, on_mount.x + 20000) && near_to(h.s->movement().good().z, on_mount.z), "and put at the target");
    h.run(milliseconds(300));
    CHECK(corrections_on(h, h.pawn_ch, from, "PlayerPawn_C") == 1, "the client is sent the position correction");
    CHECK(h.s->mounts()->phase() == MountController::Phase::Out && h.s->distance_desummons() == 0, "the mount stays where it was (200 m away is within its distance)");
    // the pawn moves on from the new place
    const size_t refused = h.s->moves_refused();
    pawn_move_to(h, on_mount.x + 20060, on_mount.y, on_mount.z);
    CHECK(h.s->moves_refused() == refused, "(the pawn's next move from there is not corrected)");
    // 800 m from the mount: it desummons by distance, as today
    t = travel.teleport(to_point_z(on_mount.x + 80000, on_mount.y, on_mount.z), "the test");
    h.run(milliseconds(300));
    CHECK(t.status == GmStatus::Ok && !t.dismounted && h.s->distance_desummons() == 1 && h.s->mounts()->phase() == MountController::Phase::Desummoning,
          "800 m away the mount is desummoned by distance");
}
// -- C.O.A x Cyb --

static void test_speed_on_foot() {
    std::printf("run speed on foot: the pawn's MoveSpeedMult, the checker's limits, the reset\n");
    Harness h(cyb_on_the_ground(), on_the_ground);
    h.play();
    GmTravel travel = make_travel(h);
    walk_pawn_to(h, 300, 0);
    const double base_limit = h.s->movement().limits().max_speed;
    // before: 600 cm in 0.2 s (3,000 cm/s) is beyond the limit
    size_t refused = h.s->moves_refused();
    pawn_move_to(h, h.s->movement().good().x + 600, 0, 1098);
    CHECK(h.s->moves_refused() == refused + 1, "(without the speed, 3,000 cm/s is refused)");
    const size_t from = h.got.size();
    GmSpeedResult s = travel.set_speed("cyb", 5, "the test");
    CHECK(s.status == GmStatus::Ok && s.character == "cyb" && s.previous == 1.0 && s.multiplier == 5.0 && h.s->gm_speed() == 5.0, "the multiplier is set: x5 (was x1)");
    const AbilityRuntime* ab = h.s->abilities();
    CHECK(ab && near_to(ab->stat(kMoveSpeedMult), 5.0, 1e-4), "the pawn's MoveSpeedMult stat is 5 (the stat the Sprint's effect raises)");
    h.run(milliseconds(100));
    size_t pawn_chains = 0;
    for (const fake::Delivered* d : chains_on(h, h.pawn_ch, from)) pawn_chains += d ? 1 : 0;
    CHECK(pawn_chains >= 1, "(the change went out on the pawn's channel)");
    CHECK(near_to(h.s->movement().limits().max_speed, base_limit * 5, 1e-6), "the move checker's horizontal limit is five times larger");
    CHECK(near_to(h.s->movement().limits().dash_speed, 3355.0 * 5, 1e-6), "(and the dash's)");
    // now 3,000 cm/s is accepted
    refused = h.s->moves_refused();
    const size_t acked = h.s->moves_acknowledged();
    for (int i = 0; i < 4; ++i) pawn_move_to(h, h.s->movement().good().x + 600, 0, 1098);
    CHECK(h.s->moves_refused() == refused && h.s->moves_acknowledged() == acked + 4, "the checker accepts moves at the multiplied speed (3,000 cm/s)");
    // and a speed beyond it is still refused
    pawn_move_to(h, h.s->movement().good().x + 3000, 0, 1098);   // 15,000 cm/s
    CHECK(h.s->moves_refused() == refused + 1, "a move beyond the multiplied limit (15,000 cm/s) is still refused");
    // a Sprint on top: the Sprint's +0.2 rides on the GM's base
    const bs::SessionGuids& g = h.s->bootstrap().setup().guids;
    h.client_sends({h.client.actor(h.pawn_ch, trigger_on(h, g.ability, content::kGuidSprint, 31), true)});
    h.run(milliseconds(100));
    CHECK(ab && ab->sprinting() && ab->stat(kMoveSpeedMult) > 5.1 && ab->stat(kMoveSpeedMult) < 5.5, "a Sprint adds its own 0.2 on top of the GM's base");
    // the reset, with the Sprint still on
    s = travel.set_speed("cyb", 1, "the test");
    CHECK(s.status == GmStatus::Ok && s.previous == 5.0 && s.multiplier == 1.0 && h.s->gm_speed() == 1.0, "x1 resets");
    CHECK(ab && ab->sprinting() && ab->stat(kMoveSpeedMult) > 1.1 && ab->stat(kMoveSpeedMult) < 1.5, "(the Sprint's own raise stays: 1.2)");
    // the limit does not fall at once: the client still has the old speed for a moment
    CHECK(near_to(h.s->movement().limits().max_speed, base_limit * 5, 1e-6), "(a second's grace: the limit is still the old one)");
    h.run(milliseconds(1500));
    pawn_move_to(h, h.s->movement().good().x + 60, 0, 1098);
    CHECK(near_to(h.s->movement().limits().max_speed, base_limit, 1e-6), "after the grace the limits are the default again");
    // bounds
    CHECK(travel.set_speed("cyb", 0.4, "the test").status == GmStatus::InvalidArgument, "0.4 is refused");
    CHECK(travel.set_speed("cyb", 10.5, "the test").status == GmStatus::InvalidArgument, "10.5 is refused");
    CHECK(travel.set_speed("cyb", std::numeric_limits<double>::quiet_NaN(), "the test").status == GmStatus::InvalidArgument, "NaN is refused");
    CHECK(travel.set_speed("nobody", 2, "the test").status == GmStatus::NotFound, "an unknown character is NotFound");
    CHECK(travel.set_speed("cyb", 0.5, "the test").status == GmStatus::Ok && travel.set_speed("cyb", 10, "the test").status == GmStatus::Ok, "0.5 and 10 are the bounds");
    // a slower speed never lowers the limits (a Dodge's dash is not slowed)
    travel.set_speed("cyb", 0.5, "the test");
    h.run(milliseconds(1500));
    pawn_move_to(h, h.s->movement().good().x + 60, 0, 1098);
    CHECK(near_to(h.s->movement().limits().max_speed, base_limit, 1e-6) && near_to(h.s->movement().limits().dash_speed, 3355.0, 1e-6), "x0.5 never lowers the checker's limits");
    CHECK(ab && near_to(ab->stat(kMoveSpeedMult), 0.5 + 0.0, 0.31), "(the pawn's stat is 0.5 plus the Sprint's)");
}

static void test_speed_mounted() {
    std::printf("run speed on a mount: the mount's MoveSpeedMult, the mount's limits, survives the dismount\n");
    Harness h(cyb_record());
    h.play();
    GmTravel travel = make_travel(h);
    CHECK(ride_on(h), "(the player rides)");
    if (!h.s->mounted()) return;
    Rider r = start_riding(h);
    const auto resync = [&] {
        const GoodPosition g = h.s->movement().good();
        r.x = g.x;
        r.y = g.y;
        r.z = g.z;
    };
    for (int i = 0; i < 6; ++i) ride_step(h, r, 60, 0);   // (the first move after a possession has a second's allowance: ride a little first)
    const double limit_before = h.s->ride()->speed_limit(1, 1.0e6);
    size_t refused = h.s->moves_refused();
    ride_step(h, r, 250, 0);   // 2,500 cm/s on a 990 cm/s bird: beyond 1.5 x
    CHECK(h.s->moves_refused() == refused + 1, "(without the speed, 2,500 cm/s on the mount is refused)");
    resync();
    const GmSpeedResult s = travel.set_speed("cyb", 3, "the test");
    CHECK(s.status == GmStatus::Ok, "(the speed is set while riding)");
    CHECK(h.mount() && near_to(mult_of(*h.mount()).value_or(0), 3.0, 1e-6) && h.s->ride()->speed_multiplier() == 3.0, "the mount's MoveSpeedMult is 3 (the stat the road's and the dash's effects raise)");
    CHECK(near_to(h.s->ride()->speed_limit(1, 1.0e6), limit_before * 3, 1e-6), "the mount's move limit is three times larger");
    CHECK(h.s->abilities() && near_to(h.s->abilities()->stat(kMoveSpeedMult), 3.0, 1e-4), "(the rider's own stat is 3 too: it is the same when it gets off)");
    refused = h.s->moves_refused();
    const size_t acked = h.s->mount_moves_acknowledged();
    for (int i = 0; i < 4; ++i) ride_step(h, r, 250, 0);
    CHECK(h.s->moves_refused() == refused && h.s->mount_moves_acknowledged() == acked + 4, "the checker accepts the mount at the multiplied speed (2,500 cm/s)");
    ride_step(h, r, 700, 0);   // 7,000 cm/s: beyond 3 x 990 x 1.5
    CHECK(h.s->moves_refused() == refused + 1, "a move beyond the multiplied limit is still refused");
    resync();
    // the dash multiplies with it: (1 + 0.35) x 3
    // the reset: a second's grace, then the limit is the default again
    travel.set_speed("cyb", 1, "the test");
    CHECK(near_to(mult_of(*h.mount()).value_or(0), 1.0, 1e-6), "x1: the mount's MoveSpeedMult is 1 again");
    resync();
    refused = h.s->moves_refused();
    ride_step(h, r, 250, 0);
    CHECK(h.s->moves_refused() == refused, "(within the grace second the old limit holds)");
    for (int i = 0; i < 15; ++i) ride_step(h, r, 0, 0);
    ride_step(h, r, 250, 0);
    CHECK(h.s->moves_refused() == refused + 1, "after it 2,500 cm/s is refused again");
    resync();
    // set again, get off: the pawn keeps it
    travel.set_speed("cyb", 3, "the test");
    get_off(h);
    h.run(milliseconds(300));
    CHECK(!h.s->mounted() && h.s->abilities() && near_to(h.s->abilities()->stat(kMoveSpeedMult), 3.0, 1e-4), "dismounted, the pawn's MoveSpeedMult is still 3");
    pawn_move_to(h, h.s->movement().good().x + 10, h.s->movement().good().y, h.s->movement().good().z);
    CHECK(h.s->movement().limits().max_speed > 2135.0 * 2.9, "(and the pawn's limits are scaled)");
}
// -- CoA . C | Y | B --

static void test_speed_before_mounting() {
    std::printf("run speed set on foot: the mount starts at it; a relog starts at 1\n");
    {
        Harness h(cyb_record());
        h.play();
        GmTravel travel = make_travel(h);
        travel.set_speed("cyb", 2, "the test");
        CHECK(ride_on(h), "(the player rides)");
        CHECK(h.mount() && near_to(mult_of(*h.mount()).value_or(0), 2.0, 1e-6), "a mount ridden after the speed was set starts at MoveSpeedMult 2");
    }
    {
        Harness h(cyb_record());
        h.play();
        CHECK(h.s->gm_speed() == 1.0 && (!h.s->abilities() || near_to(h.s->abilities()->stat(kMoveSpeedMult), 1.0, 1e-6)), "a new session (a relog) starts at x1");
    }
}

// ---- the client's own GM RPCs ---------------------------------------------------------------------------------------------------------------

static BunchWriter gm_rpc(const char* name, const std::function<void(const schema::RpcSchema&, repl::RpcParams&)>& fill) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc(name);
    if (!rpc) {
        std::printf("  [FAIL] no RPC %s\n", name);
        return BunchWriter();
    }
    repl::RpcParams p;
    fill(*rpc, p);
    return actor_rpc(pc, name, p);
}

static void send_xyz(Harness& h, int x, int y, int z) {
    h.client_sends({h.client.actor(h.pc_ch, gm_rpc("ServerTeleportXYZ", [&](const schema::RpcSchema& rpc, repl::RpcParams& p) {
                                       repl::set_rpc_param(rpc, p, "X", repl::Value::i64(x));
                                       repl::set_rpc_param(rpc, p, "Y", repl::Value::i64(y));
                                       repl::set_rpc_param(rpc, p, "Z", repl::Value::i64(z));
                                       repl::set_rpc_param(rpc, p, "OptionalRadius", repl::Value::f32(0));
                                   }), true)});
    h.run(milliseconds(300));
}

static void send_key(Harness& h, const char* key) {
    h.client_sends({h.client.actor(h.pc_ch, gm_rpc("ServerTeleport", [&](const schema::RpcSchema& rpc, repl::RpcParams& p) {
                                       repl::set_rpc_param(rpc, p, "DestinationLookupKey", repl::Value::str(key));
                                       repl::set_rpc_param(rpc, p, "OptionalRadius", repl::Value::f32(0));
                                   }), true)});
    h.run(milliseconds(300));
}

static void send_player(Harness& h, const char* names, double x, double y, double z) {
    h.client_sends({h.client.actor(h.pc_ch, gm_rpc("ServerTeleportPlayer", [&](const schema::RpcSchema& rpc, repl::RpcParams& p) {
                                       repl::set_rpc_param(rpc, p, "CharacterNames", repl::Value::str(names));
                                       repl::set_rpc_param(rpc, p, "X", repl::Value::f64(x));
                                       repl::set_rpc_param(rpc, p, "Y", repl::Value::f64(y));
                                       repl::set_rpc_param(rpc, p, "Z", repl::Value::f64(z));
                                   }), true)});
    h.run(milliseconds(300));
}

static void test_client_rpcs() {
    std::printf("the client's own GM RPCs: ServerTeleportXYZ, ServerTeleport, ServerTeleportPlayer, behind the switch\n");
    Harness h(cyb_on_the_ground(), on_the_ground);
    h.play();
    GmTravel travel = make_travel(h);
    h.fx.world->set_gm_port(&travel);
    const GoodPosition start = h.s->movement().good();
    // the switch is off by default: every RPC is received and logged, none is answered
    CHECK(!h.fx.world->gm_commands(), "(the GM switch is off by default)");
    send_xyz(h, 5000, 3000, 1500);
    send_key(h, "Verra_RVR_Joeva");
    send_player(h, "cyb", 7000, 2000, 1500);
    CHECK(h.s->gm_rpcs_received() == 3, "with the switch off the three RPCs are received (and logged) ...");
    CHECK(h.s->gm_rpcs_answered() == 0 && h.s->gm_teleports() == 0 && near_to(h.s->movement().good().x, start.x) && near_to(h.s->movement().good().y, start.y), "... and none moves the player");
    // on
    h.fx.world->set_gm_commands(true);
    send_xyz(h, 5000, 3000, 1500);
    CHECK(h.s->gm_rpcs_answered() == 1 && near_to(h.s->movement().good().x, 5000) && near_to(h.s->movement().good().y, 3000) && near_to(h.s->movement().good().z, 1500),
          "ServerTeleportXYZ with the switch on: the player is at (5000, 3000, 1500)");
    const world::CityNodeInfo* joeva = g_nodes->by_name("Joeva");
    send_key(h, "Verra_RVR_Joeva");
    CHECK(joeva && h.s->gm_rpcs_answered() == 2 && near_to(h.s->movement().good().x, joeva->x, 0.01) && near_to(h.s->movement().good().y, joeva->y, 0.01),
          "ServerTeleport 'Verra_RVR_Joeva': the player is at the node");
    send_key(h, "Atlantis");
    CHECK(h.s->gm_rpcs_answered() == 2 && near_to(h.s->movement().good().x, joeva ? joeva->x : 0, 0.01), "ServerTeleport with a key worldd does not know moves no one");
    send_player(h, "nobody", 100, 100, 1500);
    CHECK(h.s->gm_rpcs_answered() == 2, "ServerTeleportPlayer naming no one in the world moves no one");
    send_player(h, "nobody, cyb", 7000, 2000, 1500);
    CHECK(h.s->gm_rpcs_answered() == 3 && near_to(h.s->movement().good().x, 7000) && near_to(h.s->movement().good().y, 2000), "ServerTeleportPlayer 'nobody, cyb' moves cyb");
    CHECK(h.s->gm_rpcs_received() == 8, "(every RPC was received and counted)");
    // a dead player is not moved by its own console
    h.fx.world->set_gm_commands(false);
    send_xyz(h, 100, 100, 1500);
    CHECK(near_to(h.s->movement().good().x, 7000), "switched off again, the next one is refused");
}
// Cyb - C-O-A

// A player's text must not reach the log as it is: a newline in it would start a line of the player's making (Nodes 1 review A, M3).
static void test_client_text_in_the_log() {
    std::printf("the client's text is cleaned before it reaches the log, with the GM switch off and on\n");
    std::ostringstream log;
    auto previous = spdlog::default_logger();
    auto logger = std::make_shared<spdlog::logger>("gm_text", std::make_shared<spdlog::sinks::ostream_sink_mt>(log));
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(logger);
    {
        Harness h(cyb_on_the_ground(), on_the_ground);
        h.play();
        GmTravel travel = make_travel(h);
        h.fx.world->set_gm_port(&travel);
        const char* forged = "Atlantis\n[2026-10-10 20:00:00.000] [gm_text] [info] [gm] nobody: a line the player made";
        const auto lines_that = [&](const std::string& begins) {
            std::istringstream in(log.str());
            std::string line;
            size_t n = 0;
            while (std::getline(in, line)) n += line.rfind(begins, 0) == 0 ? 1 : 0;
            return n;
        };
        const auto has_line_with = [&](const std::string& a, const std::string& b) {
            std::istringstream in(log.str());
            std::string line;
            while (std::getline(in, line)) {
                if (line.find(a) != std::string::npos && line.find(b) != std::string::npos) return true;
            }
            return false;
        };
        log.str("");
        send_key(h, forged);
        send_player(h, forged, 0, 0, 0);
        CHECK(lines_that("[2026-10-10 20:00:00.000]") == 0, "switch off: no line of the player's making reaches the log");
        CHECK(has_line_with("the client sent ServerTeleport (", "a line the player made") && has_line_with("the client sent ServerTeleportPlayer (", "a line the player made"),
              "... the text is there, on the one line of the RPC");
        h.fx.world->set_gm_commands(true);
        log.str("");
        send_key(h, forged);
        send_player(h, forged, 0, 0, 0);
        CHECK(lines_that("[2026-10-10 20:00:00.000]") == 0, "switch on: the warnings that name the key and the names are clean too");
        CHECK(has_line_with("worldd knows no such place", "a line the player made") && has_line_with("moved no one", "a line the player made"), "... and still name them");
    }
    spdlog::set_default_logger(previous);
}

// ---- the game master's stat overrides -------------------------------------------------------------------------------------------------------------

/// The guid of a stat of the real graph, by its name (0 when the graph does not have it).
static int64_t guid_named(const Harness& h, const char* name) {
    const stats::StatBundle* bundle = h.fx.world->stat_bundle();
    if (!bundle) return 0;
    for (size_t i = 0; i < bundle->stats.size(); ++i) {
        if (bundle->stat_name(i) == name) return bundle->stats[i].guid;
    }
    return 0;
}

/// The value the session holds for a stat (NaN when it has none).
static double session_value_of(const Harness& h, int64_t guid) {
    const stats::CharacterStats* cs = h.s->character_stats();
    const stats::StatValue* v = cs ? cs->find(guid) : nullptr;
    return v ? v->value : std::numeric_limits<double>::quiet_NaN();
}

/// The value the client has for a stat (NaN when it has none): the stat's item in the pawn's stat arrays, as the last chain that carried it.
static float client_stat(const Harness& h, int64_t guid) {
    float value = std::numeric_limits<float>::quiet_NaN();
    const bs::SessionGuids& guids = h.s->bootstrap().setup().guids;
    const repl::Subobject* stats = h.s->bootstrap().pawn().find(guids.stats);
    const stats::StatBundle* bundle = h.fx.world->stat_bundle();
    if (!stats || !stats->obj.schema || !bundle) return value;
    const schema::ClassSchema& cls = *stats->obj.schema;
    repl::CodecOptions opts;
    // the client's stat table is not on the wire: the stats that carry base and equipment are the bundle's bucket ones
    opts.stat_int32_extended = [bundle](int64_t record) {
        const auto it = bundle->by_guid.find(record);
        return it != bundle->by_guid.end() && (bundle->stats[it->second].flags & stats::kStatBuckets) != 0;
    };
    for (const fake::Delivered* d : chains_on(h, h.pawn_ch, 0)) {
        const auto blocks = repl::read_content_blocks(d->content.data(), d->content.bit_pos(), 0);
        if (!blocks) continue;
        for (const repl::BlockView& b : *blocks) {
            if (b.header.is_actor || b.header.object != guids.stats) continue;
            size_t p = b.payload_pos;
            const size_t end = b.payload_pos + b.payload_bits;
            if (b.header.has_rep_layout && !repl::read_rep_block(cls, d->content.data(), end, p, opts)) continue;
            if (p >= end) continue;
            const auto fields = repl::read_fast_array_fields(cls, d->content.data(), end, p, opts);
            if (!fields) continue;
            for (const repl::FastArrayField& f : *fields) {
                for (const auto& c : f.delta.changed) {
                    const auto it = c.values.values.find(1);
                    const auto* s = it != c.values.values.end() ? it->second.get_if<repl::StatInt32Inst>() : nullptr;
                    if (s && s->record == guid) value = std::bit_cast<float>(s->value);
                }
            }
        }
    }
    return value;
}

/// Takes the item of a stat out of the pawn's arrays: the pawn then refuses every change of that stat.
static bool drop_stat_item(const Harness& h, int64_t record) {
    repl::Subobject* stats = const_cast<repl::Subobject*>(h.s->bootstrap().pawn().find(h.s->bootstrap().setup().guids.stats));
    for (const char* field : {"StatRepInt32Owner", "StatRepInt32OwnerProxy", "StatRepInt32EveryoneProxy"}) {
        repl::FastArrayState* a = stats ? stats->obj.arrays.get(*stats->obj.schema, field) : nullptr;
        if (!a) continue;
        int32_t id = 0;
        for (const repl::FastArrayItem& it : a->items()) {
            const auto found = it.values.values.find(1);
            const auto* t = found != it.values.values.end() ? found->second.get_if<repl::StatInt32Inst>() : nullptr;
            if (t && t->record == record) id = it.id;
        }
        if (id != 0 && a->remove(id)) return true;
    }
    return false;
}

// The game master's stat overrides reach the character: the stats the session
// holds change, and the override is remembered.
static void test_gm_effects(Harness& h) {
    std::printf("the game master's stat overrides: the stats the session holds change, and the override is remembered\n");
    PlayerSession* session = h.s.get();
    if (!session || !session->character_stats()) { std::printf("  [SKIP] the session has no stats\n"); return; }

    // the stats by name, not by their place in the list: MaxHealth is additive (the override adds to it), Health is the held one
    // (it has a current value), MaxHealth is not
    const int64_t max_health = guid_named(h, "MaxHealth");
    const int64_t health = guid_named(h, "Health");
    CHECK(max_health != 0 && health != 0, "(MaxHealth and Health are in the real graph)");
    const stats::StatValue* before = session->character_stats()->find(max_health);
    CHECK(before != nullptr, "the character has MaxHealth");
    const float value = before ? before->value : 0.0f;

    // the override travels as an effect: one stack, one mod on the stat
    stats::ActiveEffect effect;
    effect.effect = {stats::kRefTypeEffect, 0};
    effect.stacks = 1;
    effect.mods = {{max_health, 5.0}};

    std::string err;
    CHECK(session->set_gm_effects({effect}, err), "the override is accepted");
    CHECK(session->gm_effects().size() == 1, "the override is remembered");
    const stats::StatValue* after = session->character_stats()->find(max_health);
    CHECK(after != nullptr && std::fabs(after->value - (value + 5.0f)) < 0.001f, "the value grew");
    h.run(milliseconds(100));
    CHECK(std::fabs(client_stat(h, max_health) - (value + 5.0f)) < 0.001f, "and the client has the new value");

    session->clear_gm_effects();
    CHECK(session->gm_effects().empty(), "the override is cleared");
    const stats::StatValue* cleared = session->character_stats()->find(max_health);
    CHECK(cleared != nullptr && cleared->value == value, "and the stat is what it was");

    // a stat the character does not have, and one it holds no current value for, are refused
    std::string err_missing;
    CHECK(!session->set_player_current_stat(424242424242ll, 10.0, err_missing) && !err_missing.empty(),
          "a stat the character does not have is refused");
    std::string err_not_held;
    CHECK(!session->set_player_current_stat(max_health, 10.0, err_not_held) && !err_not_held.empty(),
          "a stat with no current value to set is refused");
    const stats::StatValue* untouched = session->character_stats()->find(max_health);
    CHECK(untouched != nullptr && untouched->value == value, "and the stat is untouched");

    // the current value of a held stat goes to the client, like a gear change does
    const stats::StatValue* held = session->character_stats()->find(health);
    CHECK(held != nullptr && held->held(), "(Health is a held stat: it has a current value)");
    const float want = held ? held->value / 2.0f : 0.0f;
    std::string err_current;
    CHECK(session->set_player_current_stat(health, want, err_current), "the current value is set");
    const stats::StatValue* now = session->character_stats()->find(health);
    CHECK(now != nullptr && std::fabs(now->value - want) < 0.001f, "the session's Health is the new value");
    h.run(milliseconds(100));
    const stats::StatValue* ticked = session->character_stats()->find(health);
    const float client = client_stat(h, health);
    CHECK(ticked != nullptr && std::fabs(client - ticked->value) < 0.001f, "and the client has it too (the client's Health is the session's)");
    CHECK(client >= want - 0.001f && client < want + 20.0f, "and it is the value that was set (a regeneration tick may have added a little)");

    // a change the pawn refuses leaves the overrides as they were: nothing is left behind
    std::string err_again;
    CHECK(session->set_gm_effects({effect}, err_again), "the override is set again");
    CHECK(drop_stat_item(h, max_health), "(the pawn's array lost the stat's item)");
    stats::ActiveEffect other;
    other.effect = {stats::kRefTypeEffect, 0};
    other.stacks = 1;
    other.mods = {{max_health, 7.0}};
    const size_t failed = session->stat_updates_failed();
    std::string err_refused;
    CHECK(!session->set_gm_effects({other}, err_refused) && !err_refused.empty(), "the change the pawn refuses is refused");
    CHECK(session->stat_updates_failed() == failed + 1, "(the session counted the refusal)");
    CHECK(session->gm_effects().size() == 1 && session->gm_effects()[0].mods[0].value == 5.0,
          "and the override that was there is still there: nothing is left behind");
}

// The GM's stat tools: the catalog, the overrides and the current value.
static void test_stat_tools(Harness& h, GmTravel& gm, const std::string& name) {
    const GmStatsResult stats = gm.get_player_stats(name);
    CHECK(stats.status == GmStatus::Ok, "the stats are read");
    CHECK(stats.character == name, "the answer names the character");
    CHECK(!stats.stats.empty(), "the character has stats");

    // a stat is named as the graph names it, and its value is the session's
    const int64_t max_health = guid_named(h, "MaxHealth");
    const GmStatInfo* info = nullptr;
    for (const GmStatInfo& s : stats.stats) {
        if (s.guid == max_health) info = &s;
    }
    CHECK(info != nullptr && info->name == "MaxHealth", "the stat is named as the graph names it");
    CHECK(info != nullptr && std::fabs(info->value - session_value_of(h, max_health)) < 0.001, "its value is the session's");

    if (!stats.stats.empty()) {
        const int64_t guid = stats.stats.front().guid;
        const GmStatModsResult set = gm.set_stat_mods(name, {{guid, 3.0}}, "the test");
        CHECK(set.status == GmStatus::Ok, "the override is accepted");
        bool found = false;
        for (const GmStatInfo& s : set.stats) {
            if (s.guid == guid) { found = true; CHECK(s.has_override, "the override shows"); }
        }
        CHECK(found, "the overridden stat is in the answer");

        const GmStatModsResult cleared = gm.clear_stat_mods(name, "the test");
        CHECK(cleared.status == GmStatus::Ok, "the clear is accepted");
        for (const GmStatInfo& s : cleared.stats) {
            if (s.guid == guid) CHECK(!s.has_override, "the override is gone");
        }
    }

    // the override's value is the one asked for, a set replaces the overrides that were there, and the override reaches the client
    const double base_value = session_value_of(h, max_health);
    GmStatModsResult set = gm.set_stat_mods(name, {{max_health, 3.0}}, "the test");
    CHECK(set.status == GmStatus::Ok, "the override on MaxHealth is accepted");
    bool shows = false;
    for (const GmStatInfo& s : set.stats) {
        if (s.guid != max_health) continue;
        shows = true;
        CHECK(s.has_override, "the override shows");
        CHECK(std::fabs(s.override - 3.0) < 0.0001, "and it is the value that was asked for");
    }
    CHECK(shows, "the overridden stat is in the answer");
    CHECK(std::fabs(session_value_of(h, max_health) - (base_value + 3.0)) < 0.001, "and the stat grew by it (MaxHealth is additive)");
    h.run(milliseconds(100));
    CHECK(std::fabs(client_stat(h, max_health) - (base_value + 3.0)) < 0.001, "and the client has it");

    // a set replaces the overrides that were there: the old one is gone
    set = gm.set_stat_mods(name, {{max_health, 5.0}}, "the test");
    CHECK(set.status == GmStatus::Ok, "the second override is accepted");
    int overrides = 0;
    double applied = 0;
    for (const GmStatInfo& s : set.stats) {
        if (s.guid != max_health) continue;
        if (s.has_override) ++overrides;
        applied = s.override;
    }
    CHECK(overrides == 1 && std::fabs(applied - 5.0) < 0.0001, "the set replaced the overrides that were there (one, worth 5)");
    CHECK(std::fabs(session_value_of(h, max_health) - (base_value + 5.0)) < 0.001, "and the stat follows the new one");
    CHECK(gm.clear_stat_mods(name, "the test").status == GmStatus::Ok, "the clear is accepted");
    CHECK(std::fabs(session_value_of(h, max_health) - base_value) < 0.001, "and the stat is what it was");

    // a guid the character does not have is refused, and the answer says which character it was about
    const GmStatModsResult unknown = gm.set_stat_mods(name, {{424242424242ll, 1.0}}, "the test");
    CHECK(unknown.status == GmStatus::FailedPrecondition, "a stat the character does not have is refused");
    CHECK(unknown.message.find("424242424242") != std::string::npos, "and the message names the guid");
    CHECK(unknown.character == name, "and the answer names the character");

    // a guid named twice is refused: two mods on one stat would compound, and the answer would report only one
    const GmStatModsResult twice = gm.set_stat_mods(name, {{max_health, 1.0}, {max_health, 2.0}}, "the test");
    CHECK(twice.status == GmStatus::InvalidArgument, "a guid named twice is refused");
    CHECK(twice.message.find("MaxHealth") != std::string::npos, "and the message names the stat");
    CHECK(std::fabs(session_value_of(h, max_health) - base_value) < 0.001, "and the stat is untouched");
    bool leftover = false;
    for (const GmStatInfo& s : gm.get_player_stats(name).stats) {
        if (s.guid == max_health && s.has_override) leftover = true;
    }
    CHECK(!leftover, "and no override was left behind");

    // the current value of a held stat: it is taken as it is, and a value above the stat's maximum is refused, not clamped
    const GmStatInfo* limited = nullptr;
    for (const GmStatInfo& s : stats.stats) {
        if (s.max > 0.0) { limited = &s; break; }
    }
    CHECK(limited != nullptr, "(a stat of this character declares a maximum)");
    if (limited) {
        const double half = limited->max / 2.0;
        const GmCurrentStatResult at = gm.set_current_stat(name, limited->guid, half, "the test");
        CHECK(at.status == GmStatus::Ok && std::fabs(at.value - half) < 0.001, "a value under the maximum is taken as it is");
        CHECK(std::fabs(session_value_of(h, limited->guid) - half) < 0.001, "and the session's stat holds it");
        const GmCurrentStatResult over = gm.set_current_stat(name, limited->guid, limited->max + 1.0, "the test");
        CHECK(over.status == GmStatus::InvalidArgument, "a value above the maximum is refused, not clamped");
        CHECK(std::fabs(session_value_of(h, limited->guid) - half) < 0.001, "and the refused value changed nothing");
    }

    // a stat the character does not have is refused
    const GmCurrentStatResult none = gm.set_current_stat(name, 424242424242ll, 10.0, "the test");
    CHECK(none.status == GmStatus::FailedPrecondition, "a stat the character does not have is refused");

    // the current value of Health goes to the client with the next update
    const int64_t health = guid_named(h, "Health");
    if (health != 0) {
        const GmCurrentStatResult r = gm.set_current_stat(name, health, 1.0, "the test");
        CHECK(r.status == GmStatus::Ok, "the current value of Health is set");
        h.run(milliseconds(100));
        const float client = client_stat(h, health);
        CHECK(client >= 1.0f - 0.001f && client < 1.0f + 20.0f, "and the client has it with the next update");
    }

    const GmStatsResult missing = gm.get_player_stats("nobody at all");
    CHECK(missing.status == GmStatus::NotFound, "an unknown character is not found");
}

// ---- the admin API ------------------------------------------------------------------------------------------------------------------------------

namespace adminv1 = ::aoc::worldadmin::v1;

static const std::string kToken = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

struct Admin {
    std::unique_ptr<svc::AdminQueue> queue = std::make_unique<svc::AdminQueue>();
    std::unique_ptr<GmTravel> travel;
    std::unique_ptr<svc::WorldAdminServer> server;
    std::unique_ptr<adminv1::WorldAdminService::Stub> stub;
    std::thread::id net_thread = std::this_thread::get_id();
    std::atomic<bool> directory_off_thread{false};

    explicit Admin(Harness& h, std::chrono::milliseconds timeout = std::chrono::milliseconds(4000)) {
        travel = std::make_unique<GmTravel>(
            *h.fx.world,
            [this, &h] {
                if (std::this_thread::get_id() != net_thread) directory_off_thread = true;   // the session list is read on the network thread only
                return std::vector<PlayerHandle>{PlayerHandle{h.s.get(), &h.conn}};
            },
            [&h] { return h.t; });
        svc::WorldAdminServer::Options o;
        o.address = "127.0.0.1:0";
        o.token = kToken;
        o.call_timeout = timeout;
        server = std::make_unique<svc::WorldAdminServer>(*travel, *queue, o);
        std::string error;
        if (!server->start(&error)) {
            std::printf("  [FAIL] the admin server does not start: %s\n", error.c_str());
            std::exit(2);
        }
        stub = adminv1::WorldAdminService::NewStub(grpc::CreateChannel("127.0.0.1:" + std::to_string(server->port()), grpc::InsecureChannelCredentials()));
    }

    /// Runs `call` on another thread (a gRPC client's) and drains the queue on this one (the network thread) until it returns.
    template <class F>
    grpc::Status via_net_thread(F&& call) {
        std::atomic<bool> done{false};
        grpc::Status status;
        std::thread t([&] {
            status = call();
            done = true;
        });
        while (!done) {
            queue->drain();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        t.join();
        return status;
    }

    static void authorize(grpc::ClientContext& ctx, const std::string& token) { ctx.AddMetadata("authorization", "Bearer " + token); }
};

static void test_admin_api() {
    std::printf("the admin API: the service token, the queue, the network thread\n");
    Harness h(cyb_on_the_ground(), on_the_ground);
    h.play();
    Admin a(h);
    const GoodPosition start = h.s->movement().good();
// [C.O.A :: C_Y_B]

    // no token, a wrong token, a malformed header: refused, and nothing happens
    {
        grpc::ClientContext ctx;   // no token
        adminv1::TeleportRequest rq;
        rq.set_character("cyb");
        rq.mutable_point()->set_x(5000);
        rq.mutable_point()->set_y(3000);
        rq.mutable_point()->set_z(1500);
        rq.mutable_point()->set_has_z(true);
        adminv1::TeleportResponse rs;
        const grpc::Status s = a.via_net_thread([&] { return a.stub->Teleport(&ctx, rq, &rs); });
        CHECK(s.error_code() == grpc::StatusCode::UNAUTHENTICATED, "a call with no token is refused (Unauthenticated)");
        grpc::ClientContext wrong;
        Admin::authorize(wrong, std::string(64, 'f'));
        const grpc::Status s2 = a.via_net_thread([&] { return a.stub->Teleport(&wrong, rq, &rs); });
        CHECK(s2.error_code() == grpc::StatusCode::UNAUTHENTICATED, "a call with a wrong token is refused");
        grpc::ClientContext bare;
        bare.AddMetadata("authorization", "Bearer ");
        const grpc::Status s3 = a.via_net_thread([&] { return a.stub->Teleport(&bare, rq, &rs); });
        CHECK(s3.error_code() == grpc::StatusCode::UNAUTHENTICATED, "a bare 'Bearer ' is refused");
        grpc::ClientContext raw;
        raw.AddMetadata("authorization", kToken);
        const grpc::Status s4 = a.via_net_thread([&] { return a.stub->Teleport(&raw, rq, &rs); });
        CHECK(s4.error_code() == grpc::StatusCode::UNAUTHENTICATED, "the token without 'Bearer ' is refused");
        CHECK(a.server->refused() == 4 && near_to(h.s->movement().good().x, start.x) && h.s->gm_teleports() == 0, "four refusals counted, the player did not move");
        CHECK(s.error_message().find(kToken) == std::string::npos && s2.error_message().find(kToken) == std::string::npos, "(the token is not in the answer)");
    }
    // a good call
    {
        grpc::ClientContext ctx;
        Admin::authorize(ctx, kToken);
        adminv1::ListPlayersResponse rs;
        const grpc::Status s = a.via_net_thread([&] { return a.stub->ListPlayers(&ctx, adminv1::ListPlayersRequest(), &rs); });
        CHECK(s.ok() && rs.players_size() == 1 && rs.players(0).name() == "cyb" && rs.players(0).character_id() == kCyb && !rs.players(0).mounted(), "ListPlayers: name, guid, position, mounted");
        CHECK(!a.directory_off_thread, "the session list was read on the network thread, not the gRPC thread");
    }
    {
        grpc::ClientContext ctx;
        Admin::authorize(ctx, kToken);
        adminv1::ListPlacesResponse rs;
        const grpc::Status s = a.via_net_thread([&] { return a.stub->ListPlaces(&ctx, adminv1::ListPlacesRequest(), &rs); });
        CHECK(s.ok() && static_cast<size_t>(rs.places_size()) == a.travel->list_places().size() && rs.places(0).name() == "login", "ListPlaces: the login spot and the nodes");
    }
    {
        grpc::ClientContext ctx;
        Admin::authorize(ctx, kToken);
        adminv1::TeleportRequest rq;
        rq.set_character("cyb");
        rq.set_place("joeva");
        adminv1::TeleportResponse rs;
        const grpc::Status s = a.via_net_thread([&] { return a.stub->Teleport(&ctx, rq, &rs); });
        const world::CityNodeInfo* joeva = g_nodes->by_name("Joeva");
        CHECK(s.ok() && rs.place() == "Verra_RVR_Joeva" && joeva && near_to(rs.to().x(), joeva->x, 0.01) && near_to(h.s->movement().good().x, joeva->x, 0.01), "Teleport to a place: applied");
        CHECK(!a.directory_off_thread, "(and applied on the network thread)");
    }
    {
        grpc::ClientContext ctx;
        Admin::authorize(ctx, kToken);
        adminv1::TeleportRequest rq;
        rq.set_character("cyb");
        rq.mutable_point()->set_x(100000);
        rq.mutable_point()->set_y(100000);
        adminv1::TeleportResponse rs;
        const grpc::Status s = a.via_net_thread([&] { return a.stub->Teleport(&ctx, rq, &rs); });
        CHECK(s.error_code() == grpc::StatusCode::INVALID_ARGUMENT && s.error_message().find("give z") != std::string::npos, "a point with no ground and no z: InvalidArgument, asking for z");
        grpc::ClientContext ctx2;
        Admin::authorize(ctx2, kToken);
        adminv1::TeleportRequest none;
        none.set_character("cyb");
        const grpc::Status s2 = a.via_net_thread([&] { return a.stub->Teleport(&ctx2, none, &rs); });
        CHECK(s2.error_code() == grpc::StatusCode::INVALID_ARGUMENT, "no target: InvalidArgument");
        grpc::ClientContext ctx3;
        Admin::authorize(ctx3, kToken);
        none.set_character("nobody");
        none.set_place("joeva");
        const grpc::Status s3 = a.via_net_thread([&] { return a.stub->Teleport(&ctx3, none, &rs); });
        CHECK(s3.error_code() == grpc::StatusCode::NOT_FOUND, "an unknown character: NotFound");
        grpc::ClientContext ctx4;
        Admin::authorize(ctx4, kToken);
        none.set_character("cyb");
        none.set_place("Narnia");
        const grpc::Status s4 = a.via_net_thread([&] { return a.stub->Teleport(&ctx4, none, &rs); });
        CHECK(s4.error_code() == grpc::StatusCode::NOT_FOUND, "an unknown place: NotFound");
    }
    {
        grpc::ClientContext ctx;
        Admin::authorize(ctx, kToken);
        adminv1::SetMoveSpeedRequest rq;
        rq.set_character("cyb");
        rq.set_multiplier(4);
        adminv1::SetMoveSpeedResponse rs;
        const grpc::Status s = a.via_net_thread([&] { return a.stub->SetMoveSpeed(&ctx, rq, &rs); });
        CHECK(s.ok() && rs.multiplier() == 4 && rs.previous() == 1 && h.s->gm_speed() == 4.0, "SetMoveSpeed: applied");
        grpc::ClientContext ctx2;
        Admin::authorize(ctx2, kToken);
        rq.set_multiplier(11);
        const grpc::Status s2 = a.via_net_thread([&] { return a.stub->SetMoveSpeed(&ctx2, rq, &rs); });
        CHECK(s2.error_code() == grpc::StatusCode::INVALID_ARGUMENT && h.s->gm_speed() == 4.0, "a multiplier of 11: InvalidArgument, nothing changed");
    }
    CHECK(!a.directory_off_thread, "(no call touched the sessions off the network thread)");
}

// The new administrative calls: the token is still the door, an unknown character
// is refused, and a player's stats come back with the game master's override on them.
static void test_admin_stat_calls() {
    std::printf("the administrative stat calls: the token, an unknown character, a player\n");
    Harness h(cyb_record());
    h.play();
    Admin a(h);

    // no token: refused
    {
        grpc::ClientContext ctx;
        adminv1::GetPlayerStatsRequest rq;
        rq.set_character("cyb");
        adminv1::GetPlayerStatsResponse rs;
        const grpc::Status s = a.via_net_thread([&] { return a.stub->GetPlayerStats(&ctx, rq, &rs); });
        CHECK(s.error_code() == grpc::StatusCode::UNAUTHENTICATED, "GetPlayerStats without the token is refused (Unauthenticated)");
    }
    // with the token, a character that is not there: NotFound
    {
        grpc::ClientContext ctx;
        Admin::authorize(ctx, kToken);
        adminv1::GetPlayerStatsRequest rq;
        rq.set_character("nobody at all");
        adminv1::GetPlayerStatsResponse rs;
        const grpc::Status s = a.via_net_thread([&] { return a.stub->GetPlayerStats(&ctx, rq, &rs); });
        CHECK(s.error_code() == grpc::StatusCode::NOT_FOUND, "an unknown character: NotFound");
    }
    // with the token, a player: the stats come back
    adminv1::GetPlayerStatsResponse stat_list;
    {
        grpc::ClientContext ctx;
        Admin::authorize(ctx, kToken);
        adminv1::GetPlayerStatsRequest rq;
        rq.set_character("cyb");
        const grpc::Status s = a.via_net_thread([&] { return a.stub->GetPlayerStats(&ctx, rq, &stat_list); });
        CHECK(s.ok() && stat_list.stats_size() > 0, "GetPlayerStats: the player's stats come back");
        CHECK(!a.directory_off_thread, "(read on the network thread)");
    }
    const int64_t max_health = guid_named(h, "MaxHealth");
    bool have_max_health = false;
    for (const adminv1::StatInfo& st : stat_list.stats()) {
        if (st.guid() == max_health) {
            have_max_health = true;
            CHECK(st.name() == "MaxHealth", "the stat is named as the graph names it");
            CHECK(std::fabs(st.value() - session_value_of(h, max_health)) < 0.001, "its value is the session's");
        }
    }
    CHECK(have_max_health, "the answer holds the stat by its guid");

    // the overrides: set, shown, and taken off again
    if (max_health != 0) {
        grpc::ClientContext ctx;
        Admin::authorize(ctx, kToken);
        adminv1::SetStatModsRequest rq;
        rq.set_character("cyb");
        adminv1::StatMod* mod = rq.add_mods();
        mod->set_stat(max_health);
        mod->set_value(5.0);
        adminv1::SetStatModsResponse rs;
        const grpc::Status s = a.via_net_thread([&] { return a.stub->SetStatMods(&ctx, rq, &rs); });
        bool overridden = false;
        for (const adminv1::StatInfo& st : rs.stats()) {
            if (st.guid() == max_health) overridden = st.has_override() && std::fabs(st.override() - 5.0) < 0.0001;
        }
        CHECK(s.ok() && overridden, "SetStatMods: the override is on the stat in the answer");

        grpc::ClientContext ctx2;
        Admin::authorize(ctx2, kToken);
        adminv1::ClearStatModsRequest crq;
        crq.set_character("cyb");
        adminv1::ClearStatModsResponse crs;
        const grpc::Status s2 = a.via_net_thread([&] { return a.stub->ClearStatMods(&ctx2, crq, &crs); });
        bool cleared = !crs.stats().empty();
        for (const adminv1::StatInfo& st : crs.stats()) {
            if (st.has_override()) cleared = false;
        }
        CHECK(s2.ok() && cleared, "ClearStatMods: the override is gone");
    }

    // the current value of a stat the character holds: a stat with a maximum takes a value under it
    const adminv1::StatInfo* limited = nullptr;
    for (const adminv1::StatInfo& st : stat_list.stats()) {
        if (st.max() > 0.0) { limited = &st; break; }
    }
    CHECK(limited != nullptr, "(a stat of this character declares a maximum)");
    if (limited) {
        const double half = limited->max() / 2.0;
        grpc::ClientContext ctx;
        Admin::authorize(ctx, kToken);
        adminv1::SetCurrentStatRequest rq;
        rq.set_character("cyb");
        rq.set_stat(limited->guid());
        rq.set_value(half);
        adminv1::SetCurrentStatResponse rs;
        const grpc::Status s = a.via_net_thread([&] { return a.stub->SetCurrentStat(&ctx, rq, &rs); });
        CHECK(s.ok() && std::fabs(rs.value() - half) < 0.001, "SetCurrentStat: the value comes back");
        CHECK(std::fabs(session_value_of(h, limited->guid()) - half) < 0.001, "and the session's stat holds it");
    }
}

static void test_admin_queue() {
    std::printf("the admin queue: a call the network thread does not take is not applied\n");
    {
        Harness h(cyb_on_the_ground(), on_the_ground);
        h.play();
        Admin a(h, std::chrono::milliseconds(200));
        const GoodPosition start = h.s->movement().good();
        grpc::ClientContext ctx;
        Admin::authorize(ctx, kToken);
        adminv1::TeleportRequest rq;
        rq.set_character("cyb");
        rq.mutable_point()->set_x(5000);
        rq.mutable_point()->set_y(3000);
        rq.mutable_point()->set_z(1500);
        rq.mutable_point()->set_has_z(true);
        adminv1::TeleportResponse rs;
        const grpc::Status s = a.stub->Teleport(&ctx, rq, &rs);   // nobody drains: the call times out
        CHECK(s.error_code() == grpc::StatusCode::UNAVAILABLE, "a call the network thread does not take in time ends with Unavailable");
        CHECK(a.queue->abandoned() == 1, "(it is counted as given up)");
        CHECK(a.queue->drain() == 0 && near_to(h.s->movement().good().x, start.x) && h.s->gm_teleports() == 0, "and it is never applied, even when the network thread comes round later");
        // closing the queue releases a waiting caller
        a.server->stop();
        CHECK(!a.queue->run([] {}, std::chrono::milliseconds(50)), "(a stopped server's queue takes nothing)");
    }
    {
        svc::AdminQueue q;
        int ran = 0;
        std::atomic<bool> in_task{false};
        std::thread t([&] { q.run([&] { in_task = true; std::this_thread::sleep_for(std::chrono::milliseconds(150)); ++ran; }, std::chrono::milliseconds(100)); });
        while (!in_task) q.drain();
        t.join();
        CHECK(ran == 1, "a task the network thread has started is waited for, whatever the timeout");
        q.run([] {}, std::chrono::milliseconds(1));
        std::thread t2([&] { q.run([&] { throw std::runtime_error("boom"); }, std::chrono::milliseconds(2000)); });
        size_t drained = 0;
        for (int i = 0; i < 2000 && drained == 0; ++i) {
            drained = q.drain();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        t2.join();
        CHECK(drained == 1, "a task that throws is answered, not left to hang its caller");
    }
}
// C-Y-B and C O A

// The world's drop rules: the game master reads them, sets them, and a value outside its
// range is refused with a reason (the rules stay as they were).
static void test_admin_loot_rules() {
    std::printf("the administrative drop rules: read, set, refuse\n");
    Harness h(cyb_record());
    h.play();
    Admin a(h);

    // the rules as they stand
    adminv1::GetLootRulesResponse rules;
    {
        grpc::ClientContext ctx;
        Admin::authorize(ctx, kToken);
        adminv1::GetLootRulesRequest rq;
        const grpc::Status s = a.via_net_thread([&] { return a.stub->GetLootRules(&ctx, rq, &rules); });
        CHECK(s.ok(), "GetLootRules: the rules come back");
        CHECK(rules.chance_mult() == 1.0 && rules.rarity_boost() == 1.0, "the rules are the data's own to start with");
        CHECK(rules.quality_floor() == 0, "no floor to start with");
        CHECK(!a.directory_off_thread, "(read on the network thread)");
    }

    // the game master sets them
    {
        grpc::ClientContext ctx;
        Admin::authorize(ctx, kToken);
        adminv1::SetLootRulesRequest rq;
        rq.set_chance_mult(5.0);
        rq.set_rarity_boost(20.0);
        rq.set_quality_floor(5);   // Legendary
        adminv1::SetLootRulesResponse rs;
        const grpc::Status s = a.via_net_thread([&] { return a.stub->SetLootRules(&ctx, rq, &rs); });
        CHECK(s.ok(), "SetLootRules: the rules are set");
        CHECK(rs.chance_mult() == 5.0 && rs.rarity_boost() == 20.0 && rs.quality_floor() == 5, "the answer carries the rules now in force");
    }

    // they stay
    {
        grpc::ClientContext ctx;
        Admin::authorize(ctx, kToken);
        adminv1::GetLootRulesRequest rq;
        adminv1::GetLootRulesResponse rs;
        const grpc::Status s = a.via_net_thread([&] { return a.stub->GetLootRules(&ctx, rq, &rs); });
        CHECK(s.ok() && rs.chance_mult() == 5.0 && rs.rarity_boost() == 20.0 && rs.quality_floor() == 5, "the rules are in force until they are changed");
    }

    // a value outside its range is refused, and the rules stay as they were
    {
        grpc::ClientContext ctx;
        Admin::authorize(ctx, kToken);
        adminv1::SetLootRulesRequest rq;
        rq.set_chance_mult(1.0);
        rq.set_rarity_boost(1.0);
        rq.set_quality_floor(9);   // past Artifact
        adminv1::SetLootRulesResponse rs;
        const grpc::Status s = a.via_net_thread([&] { return a.stub->SetLootRules(&ctx, rq, &rs); });
        CHECK(s.error_code() == grpc::StatusCode::INVALID_ARGUMENT, "a floor past Artifact: InvalidArgument");
        CHECK(std::string(s.error_message()).find("quality floor") != std::string::npos, "the refusal says which value is wrong");
    }
    {
        grpc::ClientContext ctx;
        Admin::authorize(ctx, kToken);
        adminv1::SetLootRulesRequest rq;
        rq.set_chance_mult(1.0);
        rq.set_rarity_boost(0.5);   // under 1: the data's own odds are the floor
        rq.set_quality_floor(1);
        adminv1::SetLootRulesResponse rs;
        const grpc::Status s = a.via_net_thread([&] { return a.stub->SetLootRules(&ctx, rq, &rs); });
        CHECK(s.error_code() == grpc::StatusCode::INVALID_ARGUMENT, "a boost under 1: InvalidArgument");
    }
    {
        grpc::ClientContext ctx;
        Admin::authorize(ctx, kToken);
        adminv1::SetLootRulesRequest rq;
        rq.set_chance_mult(2000.0);   // past 1000
        rq.set_rarity_boost(1.0);
        rq.set_quality_floor(1);
        adminv1::SetLootRulesResponse rs;
        const grpc::Status s = a.via_net_thread([&] { return a.stub->SetLootRules(&ctx, rq, &rs); });
        CHECK(s.error_code() == grpc::StatusCode::INVALID_ARGUMENT, "a chance multiplier past 1000: InvalidArgument");
    }
    {
        grpc::ClientContext ctx;
        Admin::authorize(ctx, kToken);
        adminv1::GetLootRulesRequest rq;
        adminv1::GetLootRulesResponse rs;
        const grpc::Status s = a.via_net_thread([&] { return a.stub->GetLootRules(&ctx, rq, &rs); });
        CHECK(s.ok() && rs.chance_mult() == 5.0 && rs.quality_floor() == 5, "a refused call changes nothing");
    }

    // a number that is not a number: the calls refuse it and say why
    {
        grpc::ClientContext ctx;
        Admin::authorize(ctx, kToken);
        adminv1::SetLootRulesRequest rq;
        rq.set_chance_mult(std::numeric_limits<double>::quiet_NaN());
        rq.set_rarity_boost(1.0);
        rq.set_quality_floor(0);
        adminv1::SetLootRulesResponse rs;
        const grpc::Status s = a.via_net_thread([&] { return a.stub->SetLootRules(&ctx, rq, &rs); });
        CHECK(s.error_code() == grpc::StatusCode::INVALID_ARGUMENT, "a NaN chance multiplier: InvalidArgument");
        CHECK(std::string(s.error_message()).find("chance multiplier") != std::string::npos, "the refusal names the value that is wrong");
    }
    {
        grpc::ClientContext ctx;
        Admin::authorize(ctx, kToken);
        adminv1::SetLootRulesRequest rq;
        rq.set_chance_mult(1.0);
        rq.set_rarity_boost(std::numeric_limits<double>::quiet_NaN());
        rq.set_quality_floor(0);
        adminv1::SetLootRulesResponse rs;
        const grpc::Status s = a.via_net_thread([&] { return a.stub->SetLootRules(&ctx, rq, &rs); });
        CHECK(s.error_code() == grpc::StatusCode::INVALID_ARGUMENT, "a NaN rarity boost: InvalidArgument");
    }

    // back to the data's own odds
    {
        grpc::ClientContext ctx;
        Admin::authorize(ctx, kToken);
        adminv1::SetLootRulesRequest rq;
        rq.set_chance_mult(1.0);
        rq.set_rarity_boost(1.0);
        rq.set_quality_floor(0);
        adminv1::SetLootRulesResponse rs;
        const grpc::Status s = a.via_net_thread([&] { return a.stub->SetLootRules(&ctx, rq, &rs); });
        CHECK(s.ok() && rs.quality_floor() == 0, "the rules go back to the data's own odds");
    }
}

int main() {
    spdlog::set_level(std::getenv("AOC_TEST_LOG") ? spdlog::level::debug : spdlog::level::off);
    std::string error;
    {
        auto real = content::ContentDb::open(real_db_path(), error);
        const auto data = real ? real->mount_data(error) : std::nullopt;
        if (!data) {
            std::printf("  [FAIL] the real content.db has no mount tables (%s): %s\n", real_db_path().c_str(), error.c_str());
            return 2;
        }
        for (const content::MountRow& r : data->mounts) {
            if (r.item_name == "Mount_Grounded_TerrorBird_0") g_mount_record = r.item;
            if (r.item_name == "Mount_Grounded_Horse_0") g_other_mount_record = r.item;
            if (r.kind == "aquatic" && !g_aquatic_record) g_aquatic_record = r.item;
            if (r.kind == "flying" && !g_flying_record) g_flying_record = r.item;
        }
        g_nodes = world::CityNodes::load(real_db_path(), error);
        if (!g_nodes) {
            std::printf("  [FAIL] the real content.db has no city nodes (%s): %s\n", real_db_path().c_str(), error.c_str());
            return 2;
        }
    }
    test_teleport_to_a_point();
    test_teleport_to_a_place();
    test_teleport_into_a_bowl();
    test_login_under_a_town();
    test_teleport_into_a_platform_bowl();
    test_a_hill_through_a_platform_town();
    test_login_under_a_platform_town();
    test_the_floor_net();
    test_teleport_refusals();
    test_teleport_mounted();
    test_speed_on_foot();
    test_speed_mounted();
    test_speed_before_mounting();
    test_client_rpcs();
    test_client_text_in_the_log();
    {
        Harness h(cyb_record());
        h.play();
        test_gm_effects(h);
    }
    {
        Harness h(cyb_record());
        h.play();
        GmTravel travel = make_travel(h);
        test_stat_tools(h, travel, "cyb");
    }
    test_admin_api();
    test_admin_stat_calls();
    test_admin_loot_rules();
    test_admin_queue();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
