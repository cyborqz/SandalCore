// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_mounts_ride.cpp  (the mounts milestone, stage 3: riding, dismounting, the dash, mounted combat)
//
//  A mount ridden on a connection: a session with a fake client (the harness of test_worldd_mounts_session) on a PlayerWorld whose mounts are the real content.db's
//  (the working tree's dist/content/content.db, or AOC_CONTENT_DB) and whose character has mount items in its bag.
//
//    getting on      ServerInteractWithInteractableComponent on the mount's component: the PlayerController's ClientRestart(mount), camera mode and bank-18 bar; the mount's
//                    update (RemoteRole 2, its Owner, Controller, PlayerState, its rider's stats, KnownAbilities, RiderPtr, the interaction state); the rider's pawn attached
//                    at MountSocket; the PlayerController attached to the mount: the packets and values of capture 910
//    riding          ServerMovePacked on the MOUNT's channel: acknowledged there, checked against the mount's own limit (the Blueprint's speed times MoveSpeedMult times 1.5),
//                    the client's TimeStamp restarting at every possession; the mount's owner is sent no ReplicatedMovement of it; a move on the pawn's channel is dropped
//    the dash        MountAbility_Sprint on the mount's AbilityComponent: its reply 94 ms later (Cooldowns 30 s, the 5 s effect, MoveSpeedMult x1.35, the hit
//                    confirmation), the end after 5 s, the cooldown item after 30 s; refused while it cools down
//    the roads       the effect of the road under the mount, added and removed as it enters and leaves, and the limit that follows
//    getting off     ServerDismountInteraction: the pawn beside the mount at saddle height (RepDetachmentLocation), the PlayerController restarted on its pawn: capture 912;
//                    16 times in a row (capture 914); the mounted desummon's two restarts (capture 893)
//    combat          hits land on the mount's Health, the rider's untouched; at 0 the mount dies, the rider is dismounted, the item is locked; the filter of abilities
//    other cases     relog while mounted, the player's death, the summon of another mount, hostile requests
// ============================================================================
#include "content_db_fixture.h"
#include "cyb_appearance.h"
#include "fake_client.h"
#include "bootstrap/character_setup.h"
#include "content/content_db.h"
#include "items/inventory.h"
#include "nav_fixture.h"
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
// (C:Y:B | CoA)

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
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
// [Cyb x C O A]

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
// -- C:Y:B & CoA --

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
// [C-O-A - C.Y.B]

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
// [C_Y_B x CoA]

/// The player walks up to its mount (within 250 cm), as retail's did (227 cm in capture 910).
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
// CoA x C-Y-B

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

static void test_riding_moves() {
    std::printf("riding: moves on the mount's channel\n");
    Harness h(cyb_record());
    h.play();
    CHECK(ride_on(h), "(the player rides)");
    if (!h.s->mounted()) return;
    const uint32_t mch = h.mount_ch();
    const world::WorldActor* m = h.mount();
    const size_t from = h.got.size();
    Rider r = start_riding(h);
    // the client's TimeStamp restarted near 0 (the pawn's clock was past 1 s): the first moves are taken, each acknowledged on the mount's channel
    CHECK(g_pawn_ts > 0.5f, "(the pawn's clock is past 0.5 s: the mount's restarts)");
    for (int i = 0; i < 20; ++i) ride_step(h, r, 99, 0);   // 990 cm/s: the terror bird's speed
    CHECK(h.s->mount_moves_acknowledged() == 20 && acks_on(h, mch, from) == 20, "20 moves at 990 cm/s are acknowledged on the mount's channel, the TimeStamp restarting near 0");
    CHECK(acks_on(h, h.pawn_ch, from, "PlayerPawn_C") == 0, "and none on the pawn's");
    CHECK(h.s->moves_refused() == 0 && std::fabs(h.s->movement().good().x - r.x) < 1.0, "none is refused; the mount's last good position follows");
    CHECK(std::fabs(h.mount()->x - r.x) < 1.0, "the mount's place in the registry follows too");
    // the owner is sent no ReplicatedMovement of its possessed mount
    size_t movement_updates = 0;
    for (const fake::Delivered* d : chains_on(h, mch, from)) {
        const States st = states_of(*d, m->actor);
        const auto it = st.find(0);
        if (it != st.end() && it->second.state.get(7) != nullptr) ++movement_updates;
    }
    CHECK(movement_updates == 0, "the owner is sent no ReplicatedMovement of its possessed mount (910, 959, 871)");
    // a move on the pawn's channel is a client that is wrong: dropped, counted, not acknowledged
    const size_t acked = h.s->moves_acknowledged();
    move_pawn(h, move_at(5.0f, r.x, r.y, r.z));
    CHECK(h.s->moves_on_the_wrong_channel() == 1 && h.s->moves_acknowledged() == acked, "a move on the pawn's channel while the player rides is dropped");
    // too fast: 3,000 cm in 0.1 s
    const size_t corr_before = corrections_on(h, mch, from);
    const double good_x = h.s->movement().good().x;
    ride_step(h, r, 3000, 0);
    CHECK(h.s->moves_refused() == 1 && corrections_on(h, mch, from) == corr_before + 1, "a move of 3,000 cm in 0.1 s is refused and corrected on the mount's channel");
    CHECK(std::fabs(h.s->movement().good().x - good_x) < 1.0, "the last good position stays");
    r.x = good_x;   // the client is put back
    // 1,400 cm/s is under the limit (990 x 1.5 = 1,485), 1,700 cm/s is not, once the credit is spent
    const size_t refused_before = h.s->moves_refused();
    for (int i = 0; i < 30; ++i) ride_step(h, r, 140, 0);
    CHECK(h.s->moves_refused() == refused_before, "30 moves at 1,400 cm/s (under 990 x 1.5) are all accepted");
    for (int i = 0; i < 30; ++i) ride_step(h, r, 170, 0);
    CHECK(h.s->moves_refused() > refused_before, "30 moves at 1,700 cm/s are refused once the credit is spent");
    // a speed hack that stays just under the limit gains nothing more than the limit
    // the mount's limit is not the pawn's: a pawn at 1,400 cm/s would be accepted (limit 2,135) and the mount's 990 x 1.5 refuses 1,700
    CHECK(h.s->ride()->speed_limit(1, 0) < 1500, "the limit is the mount's own: 990 cm/s x 1.5");
    // getting off: the pawn's clock restarts near 0 as well
    get_off(h);
    h.run(milliseconds(100));
    const size_t acked_pawn = h.s->moves_acknowledged();
    g_pawn_ts = 0.05f;
    const GoodPosition landed = h.s->movement().good();
    move_pawn(h, move_at(g_pawn_ts, landed.x, landed.y, landed.z - 20, yaw16_of(landed.yaw_degrees), repl::kMoveFalling));
    CHECK(h.s->moves_acknowledged() == acked_pawn + 1, "the pawn's first move after the dismount, TimeStamp 0.05, is acknowledged (its clock restarted)");
    // a move on the mount's channel now: dropped (nobody rides)
    const size_t wrong = h.s->moves_on_the_wrong_channel();
    h.client_sends({h.client.actor(mch, client_move("ANPCCharacter", move_at(0.1f, r.x, r.y, r.z)), false)});
    CHECK(h.s->moves_on_the_wrong_channel() == wrong, "a move on the mount's channel after the dismount is nobody's (not routed)");
}

static void test_dash() {
    std::printf("the dash\n");
    Harness h(cyb_record());
    h.play();
    CHECK(ride_on(h), "(the player rides)");
    if (!h.s->mounted()) return;
    const world::WorldActor* m = h.mount();
    const uint32_t mch = h.mount_ch();
    const spawns::MountBook& book = *h.fx.world->mounts();
    const int64_t dash = book.dash_ability();
    const emit::FIntrepidNetworkGUID comp = ability_component_of(*m);
    Rider r = start_riding(h);
    CHECK(mult_of(*m).value_or(0) == 1.0 && h.s->ride()->dash_ability() == dash, "(MoveSpeedMult is 1, the mount's dash is MountAbility_Sprint)");
    const size_t from = h.got.size();
    const Clock::time_point trig = h.t;
    h.client_sends({h.client.actor(mch, trigger_on(h, comp, dash, 21), true)});
    CHECK(h.s->dashes_started() == 1, "the dash trigger on the mount's AbilityComponent starts the dash");
    // nothing yet; the reply comes 94 ms after the trigger
    h.step(milliseconds(40));
    CHECK(h.s->ride()->dash_pending() && !h.s->ride()->dash_active() && mult_of(*h.mount()).value_or(0) == 1.0, "nothing is changed before the reply");
    h.run(milliseconds(120));
    CHECK(h.s->ride()->dash_active() && std::fabs(mult_of(*h.mount()).value_or(0) - 1.35) < 1e-6, "94 ms after the trigger MoveSpeedMult is 1.35");
    m = h.mount();
    // the three arrays: Cooldowns on the ability component, MoveSpeedMult and CurrentEffects on the stats component (906 packet 67)
    const auto cds = items_of(*m, "UAoCAbilityComponent", "Cooldowns");
    const auto fx = items_of(*m, "UAoCStatsComponent", "CurrentEffects");
    CHECK(cds.size() == 1 && cds[0].values.at(1).get_if<int64_t>() && *cds[0].values.at(1).get_if<int64_t>() == dash && cds[0].values.at(2).get_if<bool>() && *cds[0].values.at(2).get_if<bool>() &&
              cds[0].values.at(3).get_if<float>() && *cds[0].values.at(3).get_if<float>() == 30.0f && cds[0].values.at(6).get_if<int64_t>() && *cds[0].values.at(6).get_if<int64_t>() == 0,
          "the Cooldowns item: the dash, triggered, 30 s, Charges 0");
    const content::MountEffectRow* dash_row = book.data().dash();
    CHECK(fx.size() == 1 && dash_row && fx[0].values.at(2).get_if<int64_t>() && *fx[0].values.at(2).get_if<int64_t>() == dash_row->effect && fx[0].values.at(4).get_if<float>() &&
              *fx[0].values.at(4).get_if<float>() == 5.0f,
          "the CurrentEffects item: the dash's effect for 5 s");
    // the client got them on the mount's channel, and the PlayerController the hit confirmation
    bool cooldown_sent = false, effect_sent = false, speed_sent = false;
    for (const fake::Delivered* d : chains_on(h, mch, from)) {
        const States st = states_of(*d, m->actor);
        for (const auto& [id, o] : st) {
            const std::string cls = o.schema ? o.schema->name : "";
            if (cls == "UAoCAbilityComponent") {
                const schema::NetField* f = o.schema->field("Cooldowns");
                const repl::FastArrayState* a = f ? o.arrays.find(f->index) : nullptr;
                cooldown_sent = cooldown_sent || (a && !a->items().empty());
            } else if (cls == "UAoCStatsComponent") {
                const schema::NetField* fe = o.schema->field("CurrentEffects");
                const repl::FastArrayState* ae = fe ? o.arrays.find(fe->index) : nullptr;
                effect_sent = effect_sent || (ae && !ae->items().empty());
                const schema::NetField* fs = o.schema->field("StatRepInt32EveryoneProxy");
                const repl::FastArrayState* as = fs ? o.arrays.find(fs->index) : nullptr;
                speed_sent = speed_sent || (as && !as->items().empty());
            }
        }
    }
    CHECK(cooldown_sent && effect_sent && speed_sent, "the client is sent the Cooldowns item, the effect and MoveSpeedMult on the mount's channel");
    size_t confirmations = 0;
    for (const fake::Delivered* d : chains_on(h, h.pc_ch, from)) {
        for (const repl::RpcCall& c : calls_of(*d, "AoCPlayerControllerBP_C")) confirmations += c.rpc && std::string(c.rpc->name) == "ClientHitConfirmationsReliable";
    }
    CHECK(confirmations == 1, "the PlayerController is sent the dash's hit confirmation (ClientHitConfirmationsReliable)");
    CHECK(h.since(trig) < 0.5, "(all within half a second)");
    // the limit follows the multiplier: 990 x 1.35 x 1.5
    CHECK(std::fabs(h.s->ride()->speed_limit(1, 0.2) - 990.0 * 1.35 * 1.5) < 1.0, "the move limit is 990 x 1.35 x 1.5 while the dash lasts");
    // a dash speed move is accepted: 1,336 cm/s
    const size_t refused = h.s->moves_refused();
    for (int i = 0; i < 20; ++i) ride_step(h, r, 133.65, 0);
    CHECK(h.s->moves_refused() == refused, "20 moves at 1,336 cm/s (990 x 1.35) are accepted");
    // the second trigger while it cools down is refused and the client's cast is ended on the mount's component
    const size_t ends_before = chains_on(h, mch, from).size();
    h.client_sends({h.client.actor(mch, trigger_on(h, comp, dash, 22), true)});
    h.run(milliseconds(200));
    CHECK(h.s->dashes_started() == 1 && h.s->dashes_refused() == 1, "the dash off cooldown is refused");
    size_t end_calls = 0;
    for (const fake::Delivered* d : chains_on(h, mch, from)) {
        for (const repl::RpcCall& c : calls_of(*d, "UAoCAbilityComponent")) end_calls += c.rpc && std::string(c.rpc->name) == "ClientEndAbility";
    }
    CHECK(end_calls == 1 && chains_on(h, mch, from).size() > ends_before, "the client's cast is ended with ClientEndAbility on the mount's AbilityComponent");
    // 5 s after the reply the effect and the multiplier go
    for (int i = 0; i < 60; ++i) ride_step(h, r, 99, 0);
    CHECK(!h.s->ride()->dash_active() && mult_of(*h.mount()).value_or(0) == 1.0 && items_of(*h.mount(), "UAoCStatsComponent", "CurrentEffects").empty(), "after 5 s the effect and MoveSpeedMult go");
    CHECK(items_of(*h.mount(), "UAoCAbilityComponent", "Cooldowns").size() == 1, "the cooldown item stays until 30 s after the start");
    // (the limit stays at the old value for a second: the client learns of the fall a trip later)
    // the cooldown item goes 30 s after its start
    for (int i = 0; i < 240; ++i) ride_step(h, r, 99, 0);
    CHECK(items_of(*h.mount(), "UAoCAbilityComponent", "Cooldowns").empty(), "30 s after its start the Cooldowns item is removed");
    // and the dash can be used again
    h.client_sends({h.client.actor(mch, trigger_on(h, comp, dash, 23), true)});
    h.run(milliseconds(200));
    CHECK(h.s->dashes_started() == 2 && h.s->ride()->dash_active(), "after the cooldown the dash starts again");
    // not the mount's dash: the same trigger on a pawn that nobody rides makes nothing (checked in the hostile tests)
}
// -- C:Y:B x CoA --

// ---- the dash is usable: the real client's gate (stage 3 fix 3) ---------------------------------------------------------------------------

/// The pawn's InteractInfo.MountablePet as the client holds it: the last value of any chain the client received on the pawn's channel (nullopt: none was ever sent).
/// The exe (AAoCPlayerController::UseActionbarSlot 0x6b12700, CanEverUseActionbarSlot 0x6a91a10, GetActionbarInstigator 0x6aba180) resolves a bank-18 slot whose
/// Instigator is Mount through the player character's GetMountablePet() (virtual slot 0xe28: the replicated, RepNotify property UCharacterSecondaryInfo.MountablePet):
/// the retail server sets it to the summoned mount in the packet of the Summon_Mount ability item (871 packet 210, 898 packet 158) and to 0 when the mount closes (898 packet 601).
static std::optional<emit::FIntrepidNetworkGUID> client_mountable_pet(const Harness& h) {
    const repl::Actor& pawn = h.s->bootstrap().pawn();
    const repl::Subobject* info = nullptr;
    for (const repl::Subobject& s : pawn.subobjects) {
        if (s.name == "InteractInfo") info = &s;
    }
    if (!info) return std::nullopt;
    std::optional<emit::FIntrepidNetworkGUID> out;
    for (const fake::Delivered& d : h.got) {
        if (d.channel != h.pawn_ch || d.open || d.close) continue;
        const States st = states_of(d, pawn);
        const auto it = st.find(info->guid.ObjectId);
        if (it == st.end()) continue;
        if (const auto g = object_of(it->second, "MountablePet")) out = *g;
    }
    return out;
}

/// The client's decision to send a bank-18 Mount slot's ability: the instigator (the pawn's MountablePet) must be a live mount, whose AbilityComponent KNOWS the
/// ability (UAoCAbilityComponent::KnowsAbility 0x6bb3540 over KnownAbilities).  Otherwise it sends nothing and shows nothing: the user's "the dash can't be used".
static bool client_can_use_mount_slot(const Harness& h, int64_t ability) {
    const auto pet = client_mountable_pet(h);
    const world::WorldActor* m = h.mount();
    if (!pet || pet->ObjectId == 0 || !m || m->actor.guid.ObjectId != pet->ObjectId) return false;
    const repl::Subobject* ab = spawns::sub_of(*m, "UAoCAbilityComponent");
    const repl::Value* v = ab ? ab->obj.state.get(*ab->obj.schema, "KnownAbilities") : nullptr;
    const auto* arr = v ? v->get_if<repl::ArrayValue>() : nullptr;
    if (!arr) return false;
    for (const repl::ElementState& e : arr->elements) {
        const auto it = e.values.find(1);
        if (it != e.values.end() && it->second.get_if<int64_t>() && *it->second.get_if<int64_t>() == ability) return true;
    }
    return false;
}

static void test_dash_gate() {
    std::printf("the dash: the client's gate (the pawn's MountablePet)\n");
    // every kind of mount: the summon names the mount it can mount, in the packets of the summon, and the close takes it away
    for (const int slot : {kMountSlot, kAquaticSlot, kFlyingSlot}) {
        Harness h(cyb_record());
        h.play();
        const svc::InventoryItem* item = h.s->inventory().at(Bag, slot);
        const std::string kind = item ? std::to_string(item->item_record_id) : std::string("?");
        CHECK(!client_mountable_pet(h).has_value() || client_mountable_pet(h)->ObjectId == 0, "before any summon the pawn has no mountable pet");
        summon_with(h, slot);
        h.run(milliseconds(1900));
        const world::WorldActor* m = h.mount();
        CHECK(m != nullptr, ("(the mount of item " + kind + " is out)").c_str());
        if (!m) continue;
        const auto pet = client_mountable_pet(h);
        CHECK(pet.has_value() && pet->ObjectId == m->actor.guid.ObjectId && pet->ObjectId != 0,
              ("the summon of item " + kind + " tells the client its mountable pet: the pawn's InteractInfo.MountablePet is the mount").c_str());
        // the desummon: the pet is let go when the mount closes (not before: the mount is still on the client)
        const size_t before = h.got.size();
        desummon_with(h, slot);
        h.run(milliseconds(2800));
        const auto still = client_mountable_pet(h);
        CHECK(still.has_value() && still->ObjectId == m->actor.guid.ObjectId, "while the mount dissolves it is still the pet");
        h.run(milliseconds(1000));
        const auto gone = client_mountable_pet(h);
        CHECK(h.mount() == nullptr && gone.has_value() && gone->ObjectId == 0, "when the mount closes the pawn's MountablePet is 0");
        (void)before;
    }
    // the dash: the client's gate is open once the rider is on (the mount's KnownAbilities) and only then; the trigger it sends is the dash
    {
        Harness h(cyb_record());
        h.play();
        const int64_t dash = h.fx.world->mounts()->dash_ability();
        summon_with(h, kMountSlot);
        h.run(milliseconds(1900));
        CHECK(!client_can_use_mount_slot(h, dash), "summoned and not mounted: the mount does not know the dash yet (retail sends KnownAbilities at the mount-up)");
        walk_up_to_mount(h);
        get_on(h);
        h.run(milliseconds(200));
        CHECK(h.s->mounted(), "(the player rides)");
        if (!h.s->mounted()) return;
        CHECK(client_can_use_mount_slot(h, dash), "mounted: the client's gate for the bank-18 dash is open (the pet is the mount, which knows MountAbility_Sprint)");
        if (client_can_use_mount_slot(h, dash)) {
            const world::WorldActor* m = h.mount();
            h.client_sends({h.client.actor(h.mount_ch(), trigger_on(h, ability_component_of(*m), dash, 31), true)});
            h.run(milliseconds(200));
            CHECK(h.s->dashes_started() == 1, "the trigger the client sends starts the dash");
        }
    }
}

// ---- review A: the dash's 30 s belong to the rider (I3), the pace bank to the session (I1) ------------------------------------------------

static void test_dash_survives_a_resummon() {
    std::printf("the dash's cooldown survives a desummon and a summon (review A I3: the tooltip says 30 s)\n");
    Harness h(cyb_record());
    h.play();
    CHECK(ride_on(h), "(the player rides)");
    if (!h.s->mounted()) return;
    const int64_t dash = h.fx.world->mounts()->dash_ability();
    h.client_sends({h.client.actor(h.mount_ch(), trigger_on(h, ability_component_of(*h.mount()), dash, 21), true)});
    h.run(milliseconds(300));
    CHECK(h.s->dashes_started() == 1 && h.s->ride()->dash_active(), "(the dash starts)");
    // the player gets off, desummons the mount, waits for it to go and summons it again
    get_off(h);
    h.run(milliseconds(100));
    g_pawn_ts = 0.0f;
    desummon_with(h, kMountSlot);
    h.run(milliseconds(3700));
    CHECK(mount_count(h) == 0, "(the mount is gone)");
    summon_with(h, kMountSlot, 41);
    h.run(milliseconds(1900));
    CHECK(h.mount() != nullptr, "(summoned again: a new actor, a new Ride)");
    walk_up_to_mount(h);
    get_on(h);
    h.run(milliseconds(200));
    CHECK(h.s->mounted(), "(on the new mount, about 7 s after the first dash)");
    if (!h.s->mounted()) return;
    CHECK(items_of(*h.mount(), "UAoCAbilityComponent", "Cooldowns").size() == 1, "the new mount's bar shows the dash still cooling down (a Cooldowns item)");
    const size_t started = h.s->dashes_started(), refused = h.s->dashes_refused();
    h.client_sends({h.client.actor(h.mount_ch(), trigger_on(h, ability_component_of(*h.mount()), dash, 22), true)});
    h.run(milliseconds(200));
    CHECK(h.s->dashes_started() == started && h.s->dashes_refused() == refused + 1 && !h.s->ride()->dash_active(), "the dash on the new mount is refused: it cools down for 30 s from the first");
    // once the 30 s are over it works again
    h.run(milliseconds(24500));
    CHECK(items_of(*h.mount(), "UAoCAbilityComponent", "Cooldowns").empty(), "30 s after the first dash's start the Cooldowns item goes");
    h.client_sends({h.client.actor(h.mount_ch(), trigger_on(h, ability_component_of(*h.mount()), dash, 23), true)});
    h.run(milliseconds(200));
    CHECK(h.s->dashes_started() == started + 1 && h.s->ride()->dash_active(), "and the dash works again");
}
// (C.O.A) (Cyb)

static void test_pace_bank_survives_a_dismount() {
    std::printf("the pace bank is the session's: a dismount does not refill it (review A I1)\n");
    Harness h(cyb_record());
    h.play();
    CHECK(ride_on(h), "(the player rides)");
    if (!h.s->mounted()) return;
    Rider r = start_riding(h);
    for (int i = 0; i < 6; ++i) ride_step(h, r, 99, 0);
    // a client that races its clock: 4 s of claimed time per move, no time passing on the server's
    for (int i = 0; i < 4; ++i) {
        r.ts += 4.0f;
        move_mount(h, move_at(r.ts, r.x, r.y, r.z, r.yaw));
        h.step(milliseconds(20));
    }
    CHECK(h.s->movement().pace_bank() < 1.0, "(the bank is spent)");
    get_off(h);
    h.step(milliseconds(20));
    CHECK(!h.s->mounted() && h.s->movement().pace_bank() < 1.5, "after the dismount the pawn's checker has the same bank, not a full one");
    const size_t refused = h.s->moves_refused();
    g_pawn_ts = 0.05f;
    const GoodPosition landed = h.s->movement().good();
    move_pawn(h, move_at(g_pawn_ts, landed.x + 2000.0, landed.y, landed.z - 20, yaw16_of(landed.yaw_degrees), repl::kMoveFalling));
    CHECK(h.s->moves_refused() == refused + 1, "so the first move of the pawn cannot claim the first-move second: 2,000 cm in an instant is refused");
}

static void test_roads() {
    std::printf("roads\n");
    Harness h(cyb_record());
    h.play();
    summon_with(h, kMountSlot);
    h.run(milliseconds(1900));
    const world::WorldActor* m0 = h.mount();
    CHECK(m0 && h.s->mounts()->phase() == MountController::Phase::Out, "(the mount is out)");
    if (!m0) return;
    // a tier-2 road along x through the mount's place, 600 cm wide, 30 m long; a tier-4 road crossing it further on
    world::RoadMap::Polyline road;
    road.tier = 2;
    road.half_width_cm = 300;
    road.xy = {m0->x - 500, m0->y, m0->x + 3000, m0->y};
    CHECK(h.fx.world->roads().add(road), "(a road is added)");
    world::RoadMap::Polyline road4;
    road4.tier = 4;
    road4.half_width_cm = 300;
    road4.xy = {m0->x + 3000, m0->y - 3000, m0->x + 3000, m0->y + 3000};
    CHECK(h.fx.world->roads().add(road4), "(a second road is added)");
    walk_up_to_mount(h);
    get_on(h);
    h.run(milliseconds(200));
    CHECK(h.s->mounted(), "(the player rides)");
    if (!h.s->mounted()) return;
    const uint32_t mch = h.mount_ch();
    const content::MountData& data = h.fx.world->mounts()->data();
    Rider r = start_riding(h);
    // the mount stands on the road: the effect is on it and MoveSpeedMult is 1.2
    const auto fx = items_of(*h.mount(), "UAoCStatsComponent", "CurrentEffects");
    CHECK(fx.size() == 1 && data.road(2) && fx[0].values.at(2).get_if<int64_t>() && *fx[0].values.at(2).get_if<int64_t>() == data.road(2)->effect, "the mount starts on the tier-2 road: Road_T2_Speed_Mount is on it");
    CHECK(fx[0].values.at(4).get_if<float>() && *fx[0].values.at(4).get_if<float>() > 3.0e38f, "the road's effect has no end (Duration FLT_MAX)");
    CHECK(std::fabs(mult_of(*h.mount()).value_or(0) - 1.2) < 1e-6 && h.s->ride()->road_tier() == 2, "MoveSpeedMult is 1.2 (990 -> 1,188 cm/s) [R: 959]");
    CHECK(std::fabs(h.s->ride()->speed_limit(1, 0) - 990 * 1.2 * 1.5) < 1.0, "the move limit is 990 x 1.2 x 1.5");
    // 1,188 cm/s along the road is accepted
    const size_t refused = h.s->moves_refused();
    for (int i = 0; i < 20; ++i) ride_step(h, r, 118.8, 0);
    CHECK(h.s->moves_refused() == refused, "20 moves at 1,188 cm/s along the road are accepted");
    // the road crossing at +3000 is tier 4: the effect changes (the tier-2 item goes, the tier-4 comes: +0.30)
    for (int i = 0; i < 10 && h.s->ride()->road_tier() != 4; ++i) ride_step(h, r, 118.8, 0);
    CHECK(h.s->ride()->road_tier() == 4 && std::fabs(mult_of(*h.mount()).value_or(0) - 1.3) < 1e-6, "on the tier-4 crossing MoveSpeedMult is 1.3");
    const auto fx4 = items_of(*h.mount(), "UAoCStatsComponent", "CurrentEffects");
    CHECK(fx4.size() == 1 && data.road(4) && fx4[0].values.at(2).get_if<int64_t>() && *fx4[0].values.at(2).get_if<int64_t>() == data.road(4)->effect, "the tier-2 item is replaced by the tier-4's");
    // off the road: the effect goes and the multiplier is 1
    for (int i = 0; i < 20; ++i) ride_step(h, r, 99, 0);
    CHECK(h.s->ride()->road_tier() == -1 && mult_of(*h.mount()).value_or(0) == 1.0 && items_of(*h.mount(), "UAoCStatsComponent", "CurrentEffects").empty(), "off the road the effect goes, MoveSpeedMult is 1");
    // the limit held the old value for a second, then falls
    h.run(milliseconds(1200));
    CHECK(h.s->ride()->speed_limit(1, 1000) < 1500, "(the old limit is gone a second later)");
    // the client was sent the changes on the mount's channel
    size_t cur_effect_updates = 0;
    for (const fake::Delivered* d : chains_on(h, mch, 0)) {
        const States st = states_of(*d, h.mount()->actor);
        for (const auto& [id, o] : st) {
            const schema::NetField* fe = o.schema ? o.schema->field("CurrentEffects") : nullptr;
            if (fe && o.arrays.find(fe->index)) ++cur_effect_updates;
        }
    }
    CHECK(cur_effect_updates >= 3, "the mount's channel carried the effects' items (added, replaced, removed)");
    // a mount that is not ridden loses its road effect when the rider gets off
    get_off(h);
    h.run(milliseconds(100));
    CHECK(items_of(*h.mount(), "UAoCStatsComponent", "CurrentEffects").empty(), "nothing is left on the mount after the dismount");
}

static void test_repeats_and_desummon() {
    std::printf("getting on and off 16 times (capture 914), and the mounted desummon (capture 893)\n");
    Harness h(cyb_record());
    h.play();
    summon_with(h, kMountSlot);
    h.run(milliseconds(1900));
    walk_up_to_mount(h);
    bool all = true;
    const uint32_t mch = h.mount_ch();
    for (int i = 0; i < 16; ++i) {
        get_on(h);
        h.run(milliseconds(120));
        all = all && h.s->mounted();
        Rider r = start_riding(h);
        for (int k = 0; k < 3; ++k) ride_step(h, r, 60, 0);
        get_off(h);
        h.run(milliseconds(120));
        all = all && !h.s->mounted();
        g_pawn_ts = 0.0f;
    }
    CHECK(all && h.s->mount_ups() == 16 && h.s->dismounts() == 16, "16 times on and off: every one takes");
    CHECK(!h.s->failed() && mount_count(h) == 1 && h.mount_ch() == mch, "the mount is the same one, on the same channel; the session lives");
    CHECK(h.s->mount_moves_acknowledged() == 48, "each ride's three moves were acknowledged (TimeStamp restarting each time)");
    // the mounted desummon: ServerDismountInteraction and ServerDesummonMount (and the item) in one packet
    for (const int order : {0, 1}) {
        Harness g(cyb_record());
        g.play();
        CHECK(ride_on(g), "(the player rides)");
        if (!g.s->mounted()) continue;
        const size_t from = g.got.size();
        const svc::InventoryItem* item = g.s->inventory().at(Bag, kMountSlot);
        const std::string guid = item ? item->instance_guid : std::string();
        std::vector<fake::Bunch> bunches;
        bunches.push_back(g.client.actor(g.pawn_ch, item_used(3, kMountSlot, guid), true));
        if (order == 0) {
            bunches.push_back(g.client.actor(g.pc_ch, dismount_rpc(), true));
            bunches.push_back(g.client.actor(g.pc_ch, desummon_rpc(), true));
        } else {
            bunches.push_back(g.client.actor(g.pc_ch, desummon_rpc(), true));
            bunches.push_back(g.client.actor(g.pc_ch, dismount_rpc(), true));
        }
        g.client_sends(std::move(bunches));
        g.run(milliseconds(300));
        std::vector<std::string> seq;
        for (const fake::Delivered* d : chains_on(g, g.pc_ch, from)) {
            for (const repl::RpcCall& c : calls_of(*d, "AoCPlayerControllerBP_C")) {
                if (c.rpc && (std::string(c.rpc->name) == "ClientRestart" || std::string(c.rpc->name) == "ClientSetCameraMode")) seq.push_back(c.rpc->name);
            }
        }
        const bool two = seq == std::vector<std::string>({"ClientRestart", "ClientSetCameraMode", "ClientRestart", "ClientSetCameraMode"});
        CHECK(two, order == 0 ? "dismount then desummon in one packet: two ClientRestarts and two camera modes (893 packet 142)" : "desummon then dismount in one packet: the same two (893)");
        CHECK(!g.s->mounted() && g.s->dismounts() == 1 && g.s->mounts()->phase() == MountController::Phase::Desummoning, "the rider is off and the mount dissolves");
        g.run(milliseconds(3600));
        CHECK(mount_count(g) == 0, "the mount is gone 3.2 s later");
    }
    // a desummon alone while mounted: one restart (the rider is put off), the mount dissolves
    Harness k(cyb_record());
    k.play();
    CHECK(ride_on(k), "(the player rides)");
    const size_t from = k.got.size();
    k.client_sends({k.client.actor(k.pc_ch, desummon_rpc(), true)});
    k.run(milliseconds(300));
    size_t restarts = 0;
    for (const fake::Delivered* d : chains_on(k, k.pc_ch, from)) {
        for (const repl::RpcCall& c : calls_of(*d, "AoCPlayerControllerBP_C")) restarts += c.rpc && std::string(c.rpc->name) == "ClientRestart";
    }
    CHECK(restarts == 1 && !k.s->mounted() && k.s->mounts()->phase() == MountController::Phase::Desummoning, "a desummon of a ridden mount alone: one ClientRestart, the mount dissolves");
}
// C | Y | B captures, C.O.A code

// ---- combat --------------------------------------------------------------------------------------------------------------------------------

static double pawn_health(const Harness& h) {
    const stats::StatValue* v = h.s->character_stats() ? h.s->character_stats()->find(content::kGuidHealth) : nullptr;
    return v ? v->value : -1;
}

static void test_mounted_combat() {
    std::printf("mounted combat: hits land on the mount, its death, the lock\n");
    Harness h(cyb_record());
    h.play();
    CHECK(ride_on(h), "(the player rides)");
    if (!h.s->mounted()) return;
    const double max_mount = h.s->ride()->max_health();
    const double mount_before = h.s->ride()->health();
    const double rider_before = pawn_health(h);
    CHECK(max_mount > 400 && max_mount < 600 && mount_before == max_mount, "(the mount's Health is Mount_Base_HP at the owner's level: 500 at level 1)");
    // damage that reaches the player while it rides lands on the mount
    h.s->take_damage(h.conn, 120.0, h.t);
    h.run(milliseconds(100));
    CHECK(std::fabs(h.s->ride()->health() - (mount_before - 120)) < 0.01 && pawn_health(h) == rider_before, "120 damage comes off the mount's Health; the rider's is untouched");
    CHECK(h.s->mount_hits() == 1 && h.s->mounted(), "the rider stays on");
    // an NPC's hit (the combat world's IncomingHit) lands on the mount and is confirmed with the mount as its target
    const size_t from = h.got.size();
    combat::IncomingHit hit;
    hit.instigator = 0;
    hit.hit_guid = 6064632423136905040ll;
    hit.source_type = content::kSourceTypeAbility;
    hit.source_guid = 108436548376185ll;
    combat::ResolvedMod mod;
    mod.stat_mod_guid = 1;
    mod.stat_guid = content::kGuidHealth;
    mod.value = -30.0;
    hit.mods.push_back(mod);
    h.s->take_hit(h.conn, hit, h.t);
    h.run(milliseconds(200));
    CHECK(std::fabs(h.s->ride()->health() - (mount_before - 150)) < 0.01 && pawn_health(h) == rider_before, "an NPC's hit of 30 comes off the mount's Health too");
    size_t confirmations = 0;
    for (const fake::Delivered* d : chains_on(h, h.pc_ch, from)) {
        for (const repl::RpcCall& c : calls_of(*d, "AoCPlayerControllerBP_C")) confirmations += c.rpc && std::string(c.rpc->name) == "ClientHitConfirmationsReliable";
    }
    CHECK(confirmations == 1, "the client is sent the hit's confirmation (the floating number)");
    // a miss does nothing
    combat::IncomingHit miss = hit;
    miss.flags = stats::kHitMiss;
    h.s->take_hit(h.conn, miss, h.t);
    CHECK(std::fabs(h.s->ride()->health() - (mount_before - 150)) < 0.01, "a missed hit takes no Health");
    // the mount's Health goes to the client (EveryoneProxy item)
    h.run(milliseconds(100));
    // the death: the rest of its Health
    const size_t from_death = h.got.size();
    const uint32_t mch = h.mount_ch();
    h.s->take_damage(h.conn, 10000.0, h.t);
    h.run(milliseconds(300));
    CHECK(h.s->mount_deaths() == 1 && !h.s->mounted() && h.s->dismounts() == 1, "the mount dies under its rider: the rider is dismounted");
    CHECK(pawn_health(h) == rider_before, "the rider's Health is still untouched");
    CHECK(h.mount() != nullptr && h.s->mounts()->phase() == MountController::Phase::Dying, "the dead mount is still there for a moment");
    // the item: Health 0 and a lock (modelled 300 s), the client told the item changed
    const svc::InventoryItem* item = h.s->inventory().at(Bag, kMountSlot);
    const int64_t now_unix = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    CHECK(item && item->persisted_health && *item->persisted_health == 0.0f && item->lock_expiration >= now_unix + 290 && item->lock_expiration <= now_unix + 310,
          "the item keeps Health 0 and a lock of 300 s");
    size_t events = 0;
    for (const fake::Delivered* d : chains_on(h, h.pc_ch, from_death)) {
        for (const repl::RpcCall& c : calls_of(*d, "AoCPlayerControllerBP_C")) events += c.rpc && std::string(c.rpc->name) == "ClientReceiveStorageEvent";
    }
    CHECK(events == 2, "the client is told the item changed (a Remove and an Add)");
    // review A M4: at once, not at the next periodic save: the saver holds the dead mount's Health 0 and its lock
    CHECK(!h.sink.inventories.empty() && !h.s->inventory_dirty(), "the inventory is handed to the saver at once (a crash before the periodic save must not bring the mount back)");
    bool saved_dead = false;
    if (!h.sink.inventories.empty()) {
        for (const svc::InventoryItem& si : h.sink.inventories.back()) {
            if (si.container == Bag && si.slot == kMountSlot) saved_dead = si.persisted_health && *si.persisted_health == 0.0f && si.lock_expiration >= now_unix + 290;
        }
    }
    CHECK(saved_dead, "and what it holds is the mount item with Health 0 and its lock");
    h.run(milliseconds(1700));
    CHECK(mount_count(h) == 0 && h.s->mounts()->phase() == MountController::Phase::Idle, "the dead mount closes (Destroyed) 1.5 s later");
    bool closed = false;
    for (size_t i = from_death; i < h.got.size(); ++i) closed = closed || (h.got[i].channel == mch && h.got[i].close);
    CHECK(closed, "its channel is closed on the client");
    // the item cannot be summoned while locked: refused with the callout
    const size_t refused = h.s->mounts()->stats().refused;
    summon_with(h, kMountSlot, 31);
    h.run(milliseconds(1800));
    CHECK(h.s->mounts()->stats().refused == refused + 1 && mount_count(h) == 0, "a summon of the locked item is refused (the callout)");
    // another mount item still works
    summon_with(h, kOtherMountSlot, 32);
    h.run(milliseconds(1900));
    CHECK(mount_count(h) == 1, "another mount item summons");
}

static void test_health_comes_back_and_is_kept() {
    std::printf("a mount summoned again starts from the Health its item kept\n");
    svc::Character rec = cyb_record();
    for (svc::InventoryItem& it : rec.items) {
        if (it.container == Bag && it.slot == kMountSlot) it.persisted_health = 200.0f;
    }
    Harness h(rec);
    h.play();
    summon_with(h, kMountSlot);
    h.run(milliseconds(1900));
    CHECK(h.mount() && spawns::stat_value(*h.mount(), content::kGuidHealth).value_or(-1) == 200.0, "the mount starts at the item's Health (200 of 500)");
    walk_up_to_mount(h);
    get_on(h);
    h.run(milliseconds(150));
    h.s->take_damage(h.conn, 50.0, h.t);
    get_off(h);
    h.run(milliseconds(150));
    desummon_with(h, kMountSlot);
    h.run(milliseconds(300));
    const svc::InventoryItem* item = h.s->inventory().at(Bag, kMountSlot);
    CHECK(item && item->persisted_health && std::fabs(*item->persisted_health - 150.0f) < 0.01f, "desummoned, the item keeps the Health the mount had (150)");
    CHECK(item && item->lock_expiration == 0, "no lock: it did not die");
    h.run(milliseconds(3400));
    summon_with(h, kMountSlot, 41);
    h.run(milliseconds(1900));
    CHECK(h.mount() && spawns::stat_value(*h.mount(), content::kGuidHealth).value_or(-1) == 150.0, "summoned again it starts from 150");
    // the session's end keeps it as well
    h.s->finish("the test");
    const svc::InventoryItem* after = h.s->inventory().at(Bag, kMountSlot);
    CHECK(after && after->persisted_health && *after->persisted_health == 150.0f, "(the session's end keeps the Health too)");
    CHECK(!h.sink.inventories.empty(), "and the inventory was handed to the saver");
}

static void test_filter() {
    std::printf("the ability filter while mounted\n");
    Harness h(cyb_record());
    h.play();
    CHECK(ride_on(h), "(the player rides)");
    if (!h.s->mounted()) return;
    const bs::SessionGuids& g = h.s->bootstrap().setup().guids;
    const content::MountData& data = h.fx.world->mounts()->data();
    const content::MountAbilityRow* blink = nullptr;
    const content::MountAbilityRow* unstuck = nullptr;
    for (const content::MountAbilityRow& r : data.abilities) {
        if (r.name == "Mage_Blink") blink = &r;
        if (r.name == "General_Unstuck") unstuck = &r;
    }
    CHECK(blink && blink->auto_dismount && unstuck && unstuck->usable_while_mounted && !unstuck->auto_dismount, "(Mage_Blink dismounts, General_Unstuck is usable mounted, in the data)");
    if (!blink || !unstuck) return;
    // an ability that is neither (General_Sprint): refused, the client's cast ended on the pawn's component, and the player stays on
    const int64_t sprint = 6064629110265938944ll;   // an arbitrary ability the data does not list as usable while mounted
    const size_t ends = h.s->ability_ends_sent();
    h.client_sends({h.client.actor(h.pawn_ch, trigger_on(h, g.ability, sprint, 51), true)});
    h.run(milliseconds(100));
    CHECK(h.s->mounted() && h.s->mount_triggers_refused() == 1 && h.s->ability_ends_sent() > ends, "an ability that is not usable while mounted is refused (ClientEndAbility) and the rider stays on");
    // a usable one goes on to the runtime (the character does not know it: refused there, still mounted)
    h.client_sends({h.client.actor(h.pawn_ch, trigger_on(h, g.ability, unstuck->ability, 52), true)});
    h.run(milliseconds(100));
    CHECK(h.s->mounted() && h.s->mount_triggers_refused() == 1, "General_Unstuck is not refused by the filter and does not dismount");
    // an auto-dismount ability dismounts the rider first
    h.client_sends({h.client.actor(h.pawn_ch, trigger_on(h, g.ability, blink->ability, 53), true)});
    h.run(milliseconds(150));
    CHECK(!h.s->mounted() && h.s->dismounts() == 1, "Mage_Blink (bAutoDismount) dismounts the rider first");
    // the dash is not the pawn's: while nobody rides it makes nothing
    const int64_t dash = h.fx.world->mounts()->dash_ability();
    const size_t refused = h.s->mount_triggers_refused();
    const size_t abilities_before = h.s->ability_triggers();
    h.client_sends({h.client.actor(h.pawn_ch, trigger_on(h, g.ability, dash, 54), true)});
    h.run(milliseconds(100));
    CHECK(h.s->mount_triggers_refused() == refused + 1 && h.s->dashes_started() == 0 && h.s->ability_triggers() == abilities_before + 1, "the dash with nobody riding is refused and runs nowhere");
    // a trigger on the mount's component while nobody rides it: ended on the mount
    const world::WorldActor* m = h.mount();
    const size_t refused2 = h.s->mount_triggers_refused();
    h.client_sends({h.client.actor(h.mount_ch(), trigger_on(h, ability_component_of(*m), dash, 55), true)});
    h.run(milliseconds(100));
    CHECK(h.s->dashes_started() == 0, "(a dash on a mount nobody rides starts nothing)");
    (void)refused2;
}
// [C.O.A - Cyb]

static void test_death_and_relog() {
    std::printf("the player's death and a relog while mounted\n");
    {
        Harness h(cyb_record());
        h.play();
        CHECK(ride_on(h), "(the player rides)");
        if (!h.s->mounted()) return;
        h.s->set_current_value(content::kGuidHealth, 0.0f);
        h.run(milliseconds(300));
        CHECK(h.s->dead() && !h.s->mounted() && h.s->dismounts() == 1, "the player's death dismounts it");
        CHECK(h.s->mounts()->phase() == MountController::Phase::Desummoning, "and its mount is desummoned");
        h.run(milliseconds(3400));
        CHECK(mount_count(h) == 0, "the mount is gone 3.2 s later");
    }
    {
        // relog while mounted: the saved position is where the rider would be put beside its mount
        Harness h(cyb_record());
        h.play();
        CHECK(ride_on(h), "(the player rides)");
        if (!h.s->mounted()) return;
        Rider r = start_riding(h);
        for (int i = 0; i < 20; ++i) ride_step(h, r, 99, 0);
        const GoodPosition mounted_at = h.s->movement().good();
        const GoodPosition saved = h.s->last_position();
        const double yaw = mounted_at.yaw_degrees * 3.14159265358979 / 180.0;
        CHECK(std::fabs(saved.x - (mounted_at.x + 54 * std::cos(yaw))) < 1.0 && std::fabs(saved.y - (mounted_at.y + 54 * std::sin(yaw))) < 1.0 && std::fabs(saved.z - (mounted_at.z + 41)) < 1.0,
              "while it rides, the position to save is 54 cm ahead of the mount, on its feet");
        h.s->finish("the test");
        CHECK(!h.sink.positions.empty() && std::fabs(h.sink.positions.back().x - saved.x) < 0.01 && std::fabs(h.sink.positions.back().z - saved.z) < 0.01,
              "the session's end hands that position to the saver: it logs in dismounted at the mount's last good position");
        CHECK(h.fx.world->registry().count(world::ActorKind::Mount) == 0, "(the mount left the world with the session)");
    }
}

static void test_other_mount_item() {
    std::printf("another mount item used while riding\n");
    Harness h(cyb_record());
    h.play();
    CHECK(ride_on(h), "(the player rides)");
    if (!h.s->mounted()) return;
    summon_with(h, kOtherMountSlot, 61);
    h.run(milliseconds(300));
    CHECK(!h.s->mounted() && h.s->mounts()->phase() == MountController::Phase::Desummoning && h.s->mounts()->stats().summons == 1, "a different mount item: the rider is put off, the old mount is desummoned, nothing new is summoned");
    h.run(milliseconds(3400));
    CHECK(mount_count(h) == 0, "(the old one is gone)");
}

static void test_flyers_and_aquatic_on_the_ground() {
    std::printf("a flying and an aquatic mount ridden on the ground\n");
    {
        Harness h(cyb_record());
        h.play();
        CHECK(ride_on(h, kFlyingSlot), "(the player rides a flying mount)");
        if (!h.s->mounted()) return;
        const Ride* ride = h.s->ride();
        // the Blueprint's speeds: it runs at 990 and flies at 1,290 (215 %)
        CHECK(std::fabs(ride->speed_limit(1, 0) - 990 * 1.5) < 1.0 && std::fabs(ride->speed_limit(5, 0) - 1290 * 1.5) < 1.0, "the ground limit is 990 x 1.5 and the flying limit (movement mode 5) 1,290 x 1.5");
        Rider r = start_riding(h);
        const size_t refused = h.s->moves_refused();
        // a climb at 1,000 cm/s with 700 along (3D 1,220: past the pawn's 1,350 rise limit it would not be, but under the flyer's 1,290 x 1.1 in the air) is taken; stage 4 judges
        // the air in three dimensions, and (stage 4 fix 1) corrects only gross violations: 1,800 up and 800 along (3D 1,970, 1.53 x the cruise) is logged, 2,500 up and 1,500 along (3D 2,915)
        // is refused (test_worldd_mounts_fly has the rest)
        for (int i = 0; i < 10; ++i) ride_step(h, r, 70, 0, 100, 0.1, 5);
        CHECK(h.s->moves_refused() == refused, "a climb in flight mode (5) at 1,000 cm/s up and 700 along (3D 1,220) is taken (the flyer's own limits)");
        for (int i = 0; i < 10; ++i) ride_step(h, r, 150, 0, 250, 0.1, 5);
        CHECK(h.s->moves_refused() > refused, "a climb at 2,500 cm/s up and 1,500 along (3D 2,915, past 1.5 x 1,290) is refused");
    }
    {
        Harness h(cyb_record());
        h.play();
        CHECK(ride_on(h, kAquaticSlot), "(the player rides an aquatic mount)");
        if (!h.s->mounted()) return;
        const Ride* ride = h.s->ride();
        CHECK(ride->speed_limit(4, 0) > ride->speed_limit(1, 0) * 1.3, "its swimming limit (mode 4) is its water speed's, above its ground speed's");
    }
}

// ---- hostile requests --------------------------------------------------------------------------------------------------------------------
// Cyb / C | O | A

static void test_hostile() {
    std::printf("hostile requests\n");
    {
        // another player's mount: an actor of the registry that is not this player's
        Harness h(cyb_record());
        h.play();
        const spawns::MountBook& book = *h.fx.world->mounts();
        const content::MountRow* row = nullptr;
        for (const content::MountRow& r : book.data().mounts) {
            if (r.item == g_mount_record) row = &r;
        }
        spawns::MountParams p;
        p.player_id = 999;
        p.owner_guid = repl::GuidValue{1, 2, 3, 4};
        p.owner_name = "other";
        p.owner_level = 20;
        const GoodPosition g = h.s->movement().good();
        p.x = g.x + 200;
        p.y = g.y;
        p.z = g.z - 41;
        std::string error;
        auto made = row ? book.make(*row, p, [&] { return h.fx.world->mint_guid(); }, error) : std::nullopt;
        CHECK(made.has_value(), "(another player's mount is made)");
        if (made) {
            const emit::FIntrepidNetworkGUID comp = component_of(made->actor, made->actor.npc.interact_component);
            CHECK(h.fx.world->registry().add(std::move(made->actor)), "(it is added to the world)");
            h.run(milliseconds(300));
            h.client_sends({h.client.actor(h.pawn_ch, interact_with(comp), true)});
            h.run(milliseconds(100));
            CHECK(!h.s->mounted() && h.s->mount_interactions_refused() >= 1, "getting on another player's mount does nothing (the connection does not even have it)");
        }
    }
    {
        // a mount that is not out, a component that is nobody's, an interaction while mounted
        Harness h(cyb_record());
        h.play();
        CHECK(ride_on(h), "(the player rides)");
        if (!h.s->mounted()) return;
        const world::WorldActor* m = h.mount();
        const size_t ignored = h.s->interactions_ignored();
        h.client_sends({h.client.actor(h.pawn_ch, interact_with(component_of(*m, m->npc.interact_component)), true)});
        h.run(milliseconds(100));
        CHECK(h.s->mounted() && h.s->mount_ups() == 1 && h.s->interactions_ignored() == ignored + 1, "an interaction while mounted is ignored (it does not get on twice)");
        // ServerDismountInteraction spam
        for (int i = 0; i < 40; ++i) {
            get_off(h);
            h.step(milliseconds(5));
        }
        CHECK(!h.s->mounted() && h.s->dismounts() == 1 && !h.s->failed(), "40 ServerDismountInteractions in 0.2 s dismount once and do no harm");
        // getting on and off, faster than the budget allows
        for (int i = 0; i < 40; ++i) {
            get_on(h);
            h.step(milliseconds(5));
            get_off(h);
            h.step(milliseconds(5));
        }
        CHECK(!h.s->failed() && h.s->mount_ups() <= 1 + 10, "40 mount/dismount pairs in 0.4 s: the budget holds the rest back, the session lives");
        // summon and desummon spam while the player rides
        h.run(milliseconds(1000));
        if (!h.s->mounted()) {
            get_on(h);
            h.run(milliseconds(200));
        }
        for (int i = 0; i < 30; ++i) {
            summon_with(h, kMountSlot, 200 + i);
            h.client_sends({h.client.actor(h.pc_ch, desummon_rpc(), true)});
            h.step(milliseconds(10));
        }
        h.run(milliseconds(4500));
        CHECK(!h.s->failed() && !h.s->mounted() && mount_count(h) <= 1, "summon and desummon spam while riding settles: no mount ridden, at most one mount, no failure");
    }
    {
        // a client that lies about the mount's channel and its moves
        Harness h(cyb_record());
        h.play();
        CHECK(ride_on(h), "(the player rides)");
        if (!h.s->mounted()) return;
        const uint32_t mch = h.mount_ch();
        Rider r = start_riding(h);
        // a move far away, a TimeStamp that goes back, NaN: refused or dropped, the mount stays where it is
        ride_step(h, r, 0, 0);
        const GoodPosition good = h.s->movement().good();
        move_mount(h, move_at(0.5f, r.x + 500000, r.y, r.z));
        move_mount(h, move_at(0.01f, r.x, r.y, r.z));
        move_mount(h, move_at(std::nanf(""), r.x, r.y, r.z));
        CHECK(std::fabs(h.s->movement().good().x - good.x) < 1.0 && std::fabs(h.mount()->x - good.x) < 1.0 && !h.s->failed(), "a far move, a TimeStamp that goes back and a NaN leave the mount where it was");
        // bytes on the mount's channel that read as nothing
        BunchWriter junk;
        for (int i = 0; i < 40; ++i) junk.write_bits(0x5a5a5a5a, 32);
        h.client_sends({h.client.actor(mch, junk, false)});
        h.run(milliseconds(100));
        CHECK(!h.s->failed() && h.s->mounted(), "a bunch of junk on the mount's channel is ignored");
    }
}

/// How many ClientEndAbility calls the client got on the pawn's AbilityComponent from `from` on, and whether one named `instance`.
static size_t pawn_ends(const Harness& h, size_t from, int32_t instance, bool* named) {
    size_t n = 0;
    if (named) *named = false;
    for (const fake::Delivered* d : chains_on(h, h.pawn_ch, from)) {
        for (const repl::RpcCall& c : calls_of(*d, "UAoCAbilityComponent")) {
            if (!c.rpc || std::string(c.rpc->name) != "ClientEndAbility") continue;
            ++n;
            for (const auto& [handle, v] : c.params.values) {
                const auto* i = v.get_if<int64_t>();
                const auto* u = v.get_if<uint64_t>();
                if ((i && *i == instance) || (u && *u == static_cast<uint64_t>(instance))) {
                    if (named) *named = true;
                }
            }
        }
    }
    return n;
}
// (C-O-A) (C | Y | B)

static void test_sprint_does_not_ride() {
    std::printf("a Sprint does not ride: it ends at mount-up, nothing drains the mounted rider, the pawn's state is back at dismount (stage 3 fix 2)\n");
    Harness h(cyb_record());
    h.play();
    summon_with(h, kMountSlot);
    h.run(milliseconds(1900));
    CHECK(h.mount() != nullptr && h.s->mounts()->phase() == MountController::Phase::Out, "(the mount is out)");
    if (!h.mount()) return;
    const bs::SessionGuids& g = h.s->bootstrap().setup().guids;
    constexpr int64_t kMoveSpeedMult = 109228544644729ll;
    // Shift: the client holds the Sprint while it walks up to the mount; the Sprint drains Stamina and raises MoveSpeedMult
    h.client_sends({h.client.actor(h.pawn_ch, trigger_on(h, g.ability, content::kGuidSprint, 31), true)});
    h.run(milliseconds(100));
    const AbilityRuntime* ab = h.s->abilities();
    CHECK(ab && ab->sprinting(), "(the player sprints)");
    if (!ab) return;
    walk_up_to_mount(h);
    CHECK(ab->sprinting() && ab->stamina() < ab->max_stamina() && ab->stat(kMoveSpeedMult) > 1.1, "(it drains Stamina while the player moves, and MoveSpeedMult is up)");
    const size_t from = h.got.size();
    get_on(h);
    h.run(milliseconds(200));
    CHECK(h.s->mounted(), "(the player is on the mount)");
    bool named = false;
    const size_t ends = pawn_ends(h, from, 31, &named);
    CHECK(!ab->sprinting() && ends >= 1 && named, "getting on ends the Sprint the way a Sprint ends: ClientEndAbility to the client's instance, on the pawn's AbilityComponent");
    CHECK(ab->stat(kMoveSpeedMult) == 1.0, "MoveSpeedMult is back to 1.0 on the pawn (the Sprint's effect is gone)");
    CHECK(ab->regen_pause_until() < 0, "no regeneration pause is put on a rider (the exertion effect is not applied at mount-up)");
    h.client_sends({h.client.actor(h.pc_ch, possession_ack(h.mount()->actor.guid), true)});
    h.run(milliseconds(100));

    // riding: nothing drains Stamina, however far the mount goes
    const double at_mount = ab->stamina();
    Rider r = start_riding(h);
    for (int i = 0; i < 40; ++i) ride_step(h, r, 60, 0);   // 4 s at 600 cm/s
    CHECK(h.s->mounted() && ab->stamina() >= at_mount, "4 s of riding take no Stamina");
    // a Sprint asked for while mounted (the pawn's AbilityComponent, a client that lies or races) is refused with ClientEndAbility and starts nothing
    const size_t from2 = h.got.size();
    h.client_sends({h.client.actor(h.pawn_ch, trigger_on(h, g.ability, content::kGuidSprint, 32), true)});
    h.run(milliseconds(100));
    named = false;
    pawn_ends(h, from2, 32, &named);
    CHECK(!ab->sprinting() && named && h.s->mounted(), "a Sprint asked for while mounted is refused (ClientEndAbility) and does not start");
    for (int i = 0; i < 20; ++i) ride_step(h, r, 60, 0);
    CHECK(ab->stamina() >= at_mount, "(and the moves that follow take no Stamina)");

    // dismounted: the pawn's own Sprint works again
    get_off(h);
    h.run(milliseconds(200));
    CHECK(!h.s->mounted() && !ab->mounted(), "(the player is on foot)");
    h.client_sends({h.client.actor(h.pawn_ch, trigger_on(h, g.ability, content::kGuidSprint, 33), true)});
    h.run(milliseconds(100));
    CHECK(ab->sprinting(), "on foot again the Sprint starts: the normal state is back");
}

// ---- stage 4: flying, swimming, mounts that remain, the distance ---------------------------------------------------------------------------

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
// -- C-O-A . Cyb --

/// The mount's RemoteViewPitch16 as the registry holds it and as the last chain the client got on the mount's channel has it.
static std::optional<int64_t> registry_view_pitch(const Harness& h) {
    const world::WorldActor* m = h.mount();
    return m ? int_of(m->actor.obj, "RemoteViewPitch16") : std::nullopt;
}
static std::optional<int64_t> client_view_pitch(const Harness& h, size_t from) {
    const world::WorldActor* m = h.mount();
    std::optional<int64_t> out;
    if (!m) return out;
    for (const fake::Delivered* d : chains_on(h, h.mount_ch(), from)) {
        const States st = states_of(*d, m->actor);
        const auto it = st.find(0);
        if (it != st.end()) {
            if (const auto v = int_of(it->second, "RemoteViewPitch16")) out = *v;
        }
    }
    return out;
}

/// Takes the rider up: walks, jumps (mode 3), the second jump (mode 5) and climbs to `altitude` cm above where it started, at 1,200 cm/s (3D), along +y (the nav fixture's
/// hill is on the x axis).
static void take_off(Harness& h, Rider& r, double altitude) {
    for (int i = 0; i < 4; ++i) ride_step(h, r, 0, 90, 0, 0.1, repl::kMoveWalking);
    for (int i = 0; i < 3; ++i) ride_step(h, r, 0, 60, 55, 0.1, repl::kMoveFalling);   // the jump: vz +550
    const double z0 = r.z;
    for (int i = 0; i < 4; ++i) ride_step(h, r, 0, 90, 80, 0.1, repl::kMoveFlying);
    while (r.z - z0 < altitude) ride_step(h, r, 0, 90, 80, 0.1, repl::kMoveFlying);
}

static void test_flight_session() {
    std::printf("stage 4: flying: takeoff by ordinary moves, the dive, the dash in the air, RemoteViewPitch16, landing\n");
    Harness h(cyb_on_the_ground(), adopt_the_ground);
    h.play();
    CHECK(ride_on(h, kFlyingSlot), "(the player rides a flyer)");
    if (!h.s->mounted()) return;
    Rider r = start_riding(h);
    const size_t from = h.got.size();
    take_off(h, r, 4000.0);
    CHECK(h.s->moves_refused() == 0 && h.s->movement().good().movement_mode == repl::kMoveFlying, "takeoff by ordinary moves (mode 1, the jump in mode 3, the second jump in mode 5) and a climb at 1,200 cm/s are all accepted");
    CHECK(h.s->mount_moves_acknowledged() >= 40, "(each acknowledged on the mount's channel)");
    // RemoteViewPitch16 follows the rider's view pitch: 20 degrees down is the 16-bit angle 61,895
    ride_step(h, r, 90, 0, 0, 0.1, repl::kMoveFlying, pitch16_of(-20.0));
    h.run(milliseconds(200));
    CHECK(registry_view_pitch(h) == static_cast<int64_t>(pitch16_of(-20.0)) && client_view_pitch(h, from) == static_cast<int64_t>(pitch16_of(-20.0)),
          "RemoteViewPitch16 of the mount is the move's pitch, in the registry and on the wire (the only thing retail sends besides the move responses)");
    // the dive: 34.5 degrees down, the speed climbing at 1,600 cm/s^2 to 3,671
    double v = 1290.0;
    for (int i = 0; i < 28; ++i) {
        v = std::min(3671.0, v + 160.0);
        ride_step(h, r, v * 0.1 * std::cos(0.45), 0, -v * 0.1 * std::sin(0.45), 0.1, repl::kMoveFlying, pitch16_of(-34.5));
    }
    CHECK(h.s->moves_refused() == 0, "a dive at 34.5 degrees down climbing to 3,671 cm/s is accepted");
    // pull out and cruise
    for (int i = 0; i < 12; ++i) ride_step(h, r, std::max(1290.0, 3671.0 - 850.0 * (i + 1)) * 0.1, 0, 0, 0.1, repl::kMoveFlying, 0);
    CHECK(h.s->moves_refused() == 0, "the brake that follows is accepted");
    // a speed hack: 3,000 cm/s level
    const size_t refused = h.s->moves_refused();
    for (int i = 0; i < 12; ++i) ride_step(h, r, 300, 0, 0, 0.1, repl::kMoveFlying, 0);
    CHECK(h.s->moves_refused() > refused, "3,000 cm/s level is refused in flight");
    const GoodPosition good = h.s->movement().good();
    r.x = good.x;
    r.y = good.y;
    r.z = good.z;
    // the dash in the air: 1,290 x 1.35
    const int64_t dash = h.fx.world->mounts()->dash_ability();
    const emit::FIntrepidNetworkGUID comp = ability_component_of(*h.mount());
    h.client_sends({h.client.actor(h.mount_ch(), trigger_on(h, comp, dash, 31), true)});
    h.run(milliseconds(300));
    CHECK(h.s->ride()->dash_active() && std::fabs(mult_of(*h.mount()).value_or(0) - 1.35) < 1e-6, "the dash starts in the air: MoveSpeedMult 1.35");
    const size_t refused2 = h.s->moves_refused();
    for (int i = 0; i < 20; ++i) ride_step(h, r, 174.15, 0, 0, 0.1, repl::kMoveFlying, 0);
    CHECK(h.s->moves_refused() == refused2, "20 moves at 1,741.5 cm/s (1,290 x 1.35) are accepted while the dash lasts");
    // landing: a dive to the ground, then mode 3, then walking
    v = 1290.0;
    int guard = 0;
    while (r.z > 1057.0 + 800.0 && guard++ < 200) {
        v = std::min(4500.0, v + 160.0);
        ride_step(h, r, v * 0.1 * 0.5, 0, -v * 0.1 * 0.866, 0.1, repl::kMoveFlying, pitch16_of(-60.0));
    }
    const size_t refused3 = h.s->moves_refused();
    for (int i = 0; i < 4; ++i) ride_step(h, r, 150, 0, -std::min(r.z - 1057.0, 200.0), 0.1, repl::kMoveFalling);
    const double ground_z = 1057.0;
    r.z = ground_z;
    for (int i = 0; i < 6; ++i) ride_step(h, r, 99, 0, 0, 0.1, repl::kMoveWalking);
    CHECK(h.s->moves_refused() == refused3 && h.s->movement().good().movement_mode == repl::kMoveWalking && std::fabs(h.s->movement().good().z - ground_z) < 2.0,
          "landing: mode 5 to falling to walking on the ground, the carried speed of the dive and the 990 x 1.5 walking limit all accepted");
    // and on foot again the flight's speed is gone
    const size_t refused4 = h.s->moves_refused();
    for (int i = 0; i < 20; ++i) ride_step(h, r, 300, 0, 0, 0.1, repl::kMoveWalking);
    CHECK(h.s->moves_refused() > refused4, "3,000 cm/s on the ground is refused again once the flight's allowance has braked away");
}

/// Review A M2: the road bonus is a ground overlap volume in retail; a flyer high above a road (movement mode 5) gets none.
static void test_no_road_bonus_in_the_air() {
    std::printf("review A M2: a flyer in the air over a road gets no road bonus\n");
    Harness h(cyb_on_the_ground(), adopt_the_ground);
    h.play();
    summon_with(h, kFlyingSlot);
    h.run(milliseconds(1900));
    const world::WorldActor* m0 = h.mount();
    CHECK(m0 && h.s->mounts()->phase() == MountController::Phase::Out, "(the flyer is out)");
    if (!m0) return;
    world::RoadMap::Polyline road;
    road.tier = 3;
    road.half_width_cm = 500;
    road.xy = {m0->x - 5000, m0->y, m0->x + 5000, m0->y};
    CHECK(h.fx.world->roads().add(road), "(a road is added under the flyer)");
    walk_up_to_mount(h);
    get_on(h);
    h.run(milliseconds(200));
    CHECK(h.s->mounted(), "(the player rides it)");
    if (!h.s->mounted()) return;
    Rider r = start_riding(h);
    CHECK(h.s->ride()->road_tier() == 3, "on foot (mode 1) it is on the road");
    ride_step(h, r, 0, 60, 0, 0.1, repl::kMoveWalking);
    CHECK(h.s->ride()->road_tier() == 3, "(and stays on it)");
    ride_step(h, r, 0, 90, 80, 0.1, repl::kMoveFlying);
    CHECK(h.s->ride()->road_tier() == -1 && mult_of(*h.mount()).value_or(0) == 1.0, "in flight (mode 5) the road effect goes: MoveSpeedMult is 1");
}
// (Cyb ~ C-O-A)

static void test_flight_on_a_ground_mount() {
    std::printf("stage 4: movement mode 5 on a mount that cannot fly, mode 4 at an aquatic mount's speed on one that is not aquatic\n");
    Harness h(cyb_record());
    h.play();
    CHECK(ride_on(h, kMountSlot), "(the player rides the terror bird)");
    if (!h.s->mounted()) return;
    Rider r = start_riding(h);
    for (int i = 0; i < 5; ++i) ride_step(h, r, 90, 0, 0, 0.1, repl::kMoveWalking);
    const size_t refused = h.s->moves_refused();
    const size_t corrections = h.s->corrections_sent();
    ride_step(h, r, 90, 0, 80, 0.1, repl::kMoveFlying);
    CHECK(h.s->moves_refused() == refused + 1 && h.s->corrections_sent() == corrections + 1, "a ground mount's move in mode 5 is refused and corrected");
    r.x = h.s->movement().good().x;
    r.z = h.s->movement().good().z;
    for (int i = 0; i < 5; ++i) ride_step(h, r, 90, 0, 0, 0.1, repl::kMoveWalking);
    CHECK(h.s->moves_refused() == refused + 1, "(it walks on)");
    const size_t refused2 = h.s->moves_refused();
    for (int i = 0; i < 12; ++i) ride_step(h, r, 92, 0, 0, 0.1, repl::kMoveSwimming);
    CHECK(h.s->moves_refused() > refused2, "swimming at 920 cm/s on a ground mount (its own water speed is 400) is refused");
}

static void test_swimming_session() {
    std::printf("stage 4: an aquatic mount rides into the water: 600 on land, 920 in mode 4, no dismount\n");
    Harness h(cyb_record());
    h.play();
    CHECK(ride_on(h, kAquaticSlot), "(the player rides an aquatic mount)");
    if (!h.s->mounted()) return;
    Rider r = start_riding(h);
    for (int i = 0; i < 20; ++i) ride_step(h, r, 59.9, 0, 0, 0.1, repl::kMoveWalking);
    CHECK(h.s->moves_refused() == 0, "20 moves on land at 599 cm/s (its Blueprint's run speed) are accepted");
    // into the water: the ground falls away, the mount swims at 920 in 3D (915: mode 4 from 7 s, no dismount)
    for (int i = 0; i < 20; ++i) ride_step(h, r, 92, 0, i < 3 ? -30 : 0, 0.1, repl::kMoveSwimming);
    CHECK(h.s->moves_refused() == 0 && h.s->mounted() && h.s->dismounts() == 0 && h.s->movement().good().movement_mode == repl::kMoveSwimming,
          "20 moves in the water at 920 cm/s (mode 4) are accepted and the rider stays on: entering water does not dismount");
    for (int i = 0; i < 20; ++i) ride_step(h, r, 40, 0, -60, 0.1, repl::kMoveSwimming);   // diving: 3D 720
    CHECK(h.s->moves_refused() == 0, "a dive and a climb inside the 920 are accepted (the water speed is a 3D speed)");
    const size_t refused = h.s->moves_refused();
    for (int i = 0; i < 12; ++i) ride_step(h, r, 140, 0, 0, 0.1, repl::kMoveSwimming);
    CHECK(h.s->moves_refused() > refused, "1,400 cm/s in the water is refused");
    // the dash in the water (AllowSwimming): 920 x 1.35 = 1,242
    const GoodPosition good = h.s->movement().good();
    r.x = good.x;
    r.y = good.y;
    r.z = good.z;
    const world::WorldActor* m = h.mount();
    const emit::FIntrepidNetworkGUID comp = ability_component_of(*m);
    const int64_t dash = h.s->ride()->dash_ability();
    h.client_sends({h.client.actor(h.mount_ch(), trigger_on(h, comp, dash, 41), true)});
    h.run(milliseconds(300));
    const size_t refused2 = h.s->moves_refused();
    for (int i = 0; i < 20; ++i) ride_step(h, r, 124.2, 0, 0, 0.1, repl::kMoveSwimming);
    CHECK(h.s->ride()->dash_active() && h.s->moves_refused() == refused2, "the dash in the water: 920 x 1.35 = 1,242 cm/s is accepted");
}

/// The mount's ReplicatedMovementMode (-1: not written).
static int mount_mode(const world::WorldActor& m) {
    const auto v = int_of(m.actor.obj, "ReplicatedMovementMode");
    return v ? static_cast<int>(*v) : -1;
}
// (Cyb + C-O-A)

static void test_water_dismount() {
    std::printf("stage 4 fix 2: an aquatic mount dismounted in the water stays where it is, swimming; getting on again works\n");
    Harness h(cyb_record());
    h.play();
    CHECK(ride_on(h, kAquaticSlot), "(the player rides an aquatic mount)");
    if (!h.s->mounted()) return;
    Rider r = start_riding(h);
    for (int i = 0; i < 20; ++i) ride_step(h, r, 92, 0, i < 3 ? -30 : 0, 0.1, repl::kMoveSwimming);
    const GoodPosition at = h.s->movement().good();
    CHECK(at.movement_mode == repl::kMoveSwimming, "(the rider swims)");
    get_off(h);
    h.run(milliseconds(300));
    const world::WorldActor* m = h.mount();
    CHECK(!h.s->mounted() && m != nullptr, "(the rider is off, the mount is still there)");
    if (!m) return;
    CHECK(std::fabs(m->x - at.x) < 2.0 && std::fabs(m->y - at.y) < 2.0 && std::fabs(m->z - at.z) < 2.0, "the aquatic mount stays at its position in the water");
    CHECK(mount_mode(*m) == repl::kMoveSwimming, "and keeps its swimming movement mode (4)");
    h.run(milliseconds(2000));
    m = h.mount();
    CHECK(m && std::fabs(m->z - at.z) < 2.0, "it does not sink afterwards");
    get_on(h);
    h.run(milliseconds(300));
    CHECK(h.s->mounted(), "getting on again works from the swimmer's place beside it");
}

static void test_air_dismount_and_relog() {
    std::printf("stage 4 (modelled): dismount in the air, relog while flying, landing\n");
    {
        Harness h(cyb_on_the_ground(), adopt_the_ground);
        h.play();
        CHECK(ride_on(h, kFlyingSlot), "(the player rides a flyer)");
        if (!h.s->mounted()) return;
        Rider r = start_riding(h);
        take_off(h, r, 5000.0);
        const GoodPosition up = h.s->movement().good();
        CHECK(up.z > 1057.0 + 4800.0, "(the rider is 50 m up)");
        // relog while flying: the position to save is the ground below it, on its feet (a player's origin is 98 cm above its ground)
        const GoodPosition saved = h.s->last_position();
        CHECK(std::fabs(saved.z - (1000.0 + 98.0)) < 2.0 && std::hypot(saved.x - up.x, saved.y - up.y) < 100.0, "relog while flying: the character is saved on the ground below its last good position");
        const size_t from = h.got.size();
        get_off(h);
        h.run(milliseconds(300));
        const world::WorldActor* m = h.mount();
        CHECK(!h.s->mounted() && h.s->air_dismounts() == 1, "dismounting in the air is counted and puts the rider off");
        CHECK(m && std::fabs(m->z - up.z) < 2.0 && std::hypot(m->x - up.x, m->y - up.y) < 2.0, "the flyer stays where its rider left it, at its altitude (it does not settle on the ground)");
        CHECK(m && mount_mode(*m) == repl::kMoveFlying, "and keeps its flying movement mode (5) in its replicated movement mode");
        bool detach_in_the_air = false;
        for (const fake::Delivered* d : chains_on(h, h.pawn_ch, from)) {
            const States st = states_of(*d, h.s->bootstrap().pawn());
            for (const auto& [id, o] : st) {
                const repl::Value* loc = id == 0 ? o.state.get(*o.schema, "RepDetachmentLocation") : nullptr;
                const auto* v = loc ? loc->get_if<repl::Vec3d>() : nullptr;
                if (v && std::fabs(v->z - (up.z + 197.0)) < 1.0) detach_in_the_air = true;
            }
        }
        CHECK(detach_in_the_air, "the rider is detached 197 cm above where the mount was in the air: it falls from there");
        CHECK(h.s->movement().good().movement_mode == repl::kMoveFalling && std::fabs(h.s->movement().good().z - (up.z + 197.0)) < 1.0, "the pawn's moves start in falling mode from the detachment point");
        // the fall: the pawn's own checker takes a fall up to 6,000 cm/s (its clock restarted)
        g_pawn_ts = 0.05f;
        GoodPosition at = h.s->movement().good();
        const size_t refused = h.s->moves_refused();
        double vz = 0;
        for (int i = 0; i < 12 && at.z > 1100.0; ++i) {
            vz = std::min(4000.0, vz + 980.0 * 0.1 * 1.3);
            at.z -= vz * 0.1;
            g_pawn_ts += 0.1f;
            move_pawn(h, move_at(g_pawn_ts, at.x, at.y, at.z, yaw16_of(at.yaw_degrees), repl::kMoveFalling));
            h.step(milliseconds(100));
        }
        CHECK(h.s->moves_refused() == refused, "the fall from the detachment point (up to 4,000 cm/s) is accepted by the pawn's own checker");
        at.z -= 900.0;   // 9,000 cm/s: past UE's terminal velocity x 1.5
        g_pawn_ts += 0.1f;
        move_pawn(h, move_at(g_pawn_ts, at.x, at.y, at.z, yaw16_of(at.yaw_degrees), repl::kMoveFalling));
        CHECK(h.s->moves_refused() == refused + 1, "(and a fall of 9,000 cm/s is not)");
    }
    {
        // the relog itself: the session's end hands the ground position to the saver
        Harness h(cyb_on_the_ground(), adopt_the_ground);
        h.play();
        CHECK(ride_on(h, kFlyingSlot), "(the player rides a flyer)");
        if (!h.s->mounted()) return;
        Rider r = start_riding(h);
        take_off(h, r, 3000.0);
        h.s->finish("the test");
        CHECK(!h.sink.positions.empty() && std::fabs(h.sink.positions.back().z - 1098.0) < 2.0, "the session's end saves the character on the ground, not in the air");
    }
    {
        // the same without a ground to ask: the rider is saved beside its mount (nothing below it is known)
        Harness h(cyb_record());
        h.play();
        CHECK(ride_on(h, kFlyingSlot), "(a flyer, no ground in the world)");
        if (!h.s->mounted()) return;
        Rider r = start_riding(h);
        take_off(h, r, 3000.0);
        const GoodPosition saved = h.s->last_position();
        CHECK(saved.z > r.z - 300.0, "(without a ground the saved position stays beside the mount)");
    }
}

/// The fixture's plain with a town in a bowl on it: 7 x 7 people on a 20 m grid round (0, 15,000), standing 30 m over the terrain.  The flyer starts at the origin, more than 75 m from the
/// town (a character that joins in a bowl is put on its floor), and takes off along +y: it is over the town when it is 50 m up.
static void adopt_a_bowl(PlayerWorld& w) {
    adopt_the_ground(w);
    std::vector<world::TownFloor::Person> people;
    for (int i = -3; i <= 3; ++i) {
        for (int j = -3; j <= 3; ++j) people.push_back({i * 2000.0, 15000.0 + j * 2000.0, 1000.0, 1000.0 + 3000.0});
    }
    w.town_floor().set(4242, people);
}

static void test_relog_flying_over_a_bowl() {
    std::printf("test-2 fix: relog while flying over a town's bowl: saved on the town's floor, not on the baked ground 30 m under it\n");
    {
        Harness h(cyb_on_the_ground(), adopt_a_bowl);
        h.play();
        CHECK(ride_on(h, kFlyingSlot), "(the player rides a flyer)");
        if (!h.s->mounted()) return;
        Rider r = start_riding(h);
        take_off(h, r, 5000.0);
        const GoodPosition up = h.s->movement().good();
        const GoodPosition saved = h.s->last_position();
        const double want = 1000.0 + 3000.0 + PlayerWorld::kBowlSlackCm + 98.0;
        CHECK(up.z > want + 1000.0 && std::fabs(saved.z - want) < 2.0 && std::hypot(saved.x - up.x, saved.y - up.y) < 100.0,
              "the rider 50 m up over the town is saved on the town's floor (the plain + 30 m + the slack + the capsule), not at 1,098 under it");
        h.s->finish("the test");
        CHECK(!h.sink.positions.empty() && std::fabs(h.sink.positions.back().z - want) < 2.0, "the session's end hands that position to the saver");
    }
    {
        // the same flight over the open plain (no town): the baked ground, as before
        Harness h(cyb_on_the_ground(), adopt_the_ground);
        h.play();
        CHECK(ride_on(h, kFlyingSlot), "(a flyer, a plain with no town)");
        if (!h.s->mounted()) return;
        Rider r = start_riding(h);
        take_off(h, r, 5000.0);
        CHECK(std::fabs(h.s->last_position().z - 1098.0) < 2.0, "over the open plain the rider is saved on the baked ground, 1,098");
    }
}

static void test_mounts_remain() {
    std::printf("stage 4: a mount that has appeared stays through a fight (a hit never desummons it)\n");
    Harness h(cyb_record());
    h.play();
    summon_with(h, kMountSlot, 71);
    h.run(milliseconds(300));
    CHECK(h.mount() != nullptr, "(the mount has opened, 104 ms in; its cast has 1.2 s to go)");
    const size_t cancelled = h.s->mounts()->stats().cancelled, desummons = h.s->mounts()->stats().desummons;
    for (int i = 0; i < 16; ++i) {
        h.s->take_damage(h.conn, 1.0, h.t);   // mobs on the player (a level 1 character's Health is small: the hits are too)
        h.run(milliseconds(500));
    }
    CHECK(mount_count(h) == 1 && h.s->mounts()->phase() == MountController::Phase::Out && h.s->mounts()->stats().cancelled == cancelled && h.s->mounts()->stats().desummons == desummons,
          "8 s of hits, through the end of the cast: the mount is still out, nothing cancelled or desummoned");
    walk_up_to_mount(h);
    get_on(h);
    h.run(milliseconds(200));
    CHECK(h.s->mounted(), "(and it can be ridden)");
    for (int i = 0; i < 10; ++i) {
        h.s->take_damage(h.conn, 1.0, h.t);
        h.run(milliseconds(300));
    }
    CHECK(h.s->mounted() && mount_count(h) == 1 && h.s->dismounts() == 0, "hits while mounted land on the mount and do not put the rider off");
}
// ~ CoA & C_Y_B ~

static void test_distance_desummon() {
    std::printf("stage 4 (modelled): a mount left far behind is desummoned\n");
    Harness h(cyb_record());
    h.play();
    h.s->set_desummon_distance(3000.0);
    summon_with(h, kMountSlot, 81);
    h.run(milliseconds(1900));
    CHECK(h.mount() && h.s->mounts()->phase() == MountController::Phase::Out, "(a mount is out)");
    const GoodPosition g = h.s->movement().good();
    walk_pawn_to(h, g.x - 1800.0, g.y);
    h.run(milliseconds(300));
    CHECK(h.s->distance_desummons() == 0 && h.s->mounts()->phase() == MountController::Phase::Out, "18 m away (under the 30 m limit) the mount stays");
    walk_pawn_to(h, g.x - 4200.0, g.y);
    h.run(milliseconds(300));
    CHECK(h.s->distance_desummons() == 1 && h.s->mounts()->phase() == MountController::Phase::Desummoning, "past the limit the mount is desummoned");
    h.run(milliseconds(3400));
    CHECK(mount_count(h) == 0, "(it dissolves and is gone)");
}

// ---- stage 4: the real roads (dist/content/roads.tsv: mapc roads, contentc roads) --------------------------------------------------------------

struct RealRoad {
    int tier = 0;
    double half = 0;
    std::vector<std::pair<double, double>> xy;
};

static std::vector<RealRoad> read_real_roads(const std::string& path) {
    std::vector<RealRoad> out;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    std::string text;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    std::fclose(f);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        const std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> cells;
        size_t a = 0;
        while (a <= line.size()) {
            size_t b = line.find('\t', a);
            if (b == std::string::npos) b = line.size();
            cells.push_back(line.substr(a, b - a));
            a = b + 1;
        }
        if (cells.size() < 5 || cells[0] != "road") continue;
        RealRoad r;
        r.tier = std::atoi(cells[1].c_str());
        r.half = std::atof(cells[2].c_str());
        for (size_t i = 3; i < cells.size(); ++i) {
            const size_t comma = cells[i].find(',');
            if (comma == std::string::npos) continue;
            r.xy.emplace_back(std::atof(cells[i].substr(0, comma).c_str()), std::atof(cells[i].substr(comma + 1).c_str()));
        }
        out.push_back(std::move(r));
    }
    return out;
}

static void test_real_roads() {
    std::printf("stage 4: the roads of the area (mapc roads, contentc roads): the real file, and a mount riding along one\n");
    const std::string path = MOUNT_ROADS_PATH;
    const std::vector<RealRoad> roads = read_real_roads(path);
    CHECK(roads.size() >= 30, "dist/content/roads.tsv holds the area's roads (42 at Wilderness: 11 Artery T1, 31 Lions Hold T2)");
    if (roads.empty()) return;
    world::RoadMap map;
    std::string error;
    CHECK(map.load(path, error) && map.roads() == roads.size() && map.node_level() == 0, "worldd's loader reads it, and the header says the tiers were resolved at node level 0");
    size_t t1 = 0, t2 = 0, other = 0;
    double metres = 0, narrow = 1e9;
    for (const RealRoad& r : roads) {
        (r.tier == 1 ? t1 : r.tier == 2 ? t2 : other)++;
        narrow = std::min(narrow, r.half);
        for (size_t i = 1; i < r.xy.size(); ++i) metres += std::hypot(r.xy[i].first - r.xy[i - 1].first, r.xy[i].second - r.xy[i - 1].second) / 100.0;
    }
    CHECK(t1 >= 5 && t2 >= 20 && other == 0, "the area's roads are tier 1 (the Riverlands Artery at level 0) and tier 2 (Lions Hold's)");
    CHECK(metres > 12000.0 && metres < 20000.0, "about 14 km of road in all (8.2 km of spline within 1 km of the centre, the rest near it)");
    CHECK(narrow >= 500.0, "no road is narrower than half of max(RoadWidth, 1000): the overlap's half width is at least 500 cm");
    // every vertex of every road is on a road of at least its own tier; a point 3 km from all of them is on none
    bool self = true;
    for (const RealRoad& r : roads) {
        for (const auto& [x, y] : r.xy) self = self && map.tier_at(x, y) >= r.tier;
    }
    CHECK(self, "every sample of every road lies on a road of at least its tier");
    CHECK(map.tier_at(map.bounds().max_x + 300000.0, map.bounds().max_y + 300000.0) == -1, "a point 3 km outside the roads' extent is on none");
    // the sample nearest the E1 login spot among the tier-2 roads that has 30 samples (6 km... 60 m) after it
    const double cx = -777664.5, cy = 616578.69;
    const RealRoad* best = nullptr;
    size_t best_i = 0;
    double best_d = 1e18;
    for (const RealRoad& r : roads) {
        if (r.tier != 2 || r.xy.size() < 40) continue;
        for (size_t i = 0; i + 30 < r.xy.size(); ++i) {
            const double d = std::hypot(r.xy[i].first - cx, r.xy[i].second - cy);
            if (d < best_d) {
                best_d = d;
                best = &r;
                best_i = i;
            }
        }
    }
    CHECK(best != nullptr, "(a tier-2 road with 30 samples ahead of its nearest point)");
    if (!best) return;
    const auto& p0 = best->xy[best_i];
    const auto& p1 = best->xy[best_i + 1];
    const double yaw = std::atan2(p1.second - p0.second, p1.first - p0.first) * 180.0 / 3.14159265358979;
    svc::Character rec = cyb_record();
    rec.has_position = true;
    rec.position = {p0.first, p0.second, 3000.0, static_cast<float>(yaw)};
    Harness h(rec, [&](PlayerWorld& w) {
        std::string e;
        if (!w.load_roads(path, e)) std::printf("  [FAIL] the roads do not load into the world: %s\n", e.c_str());
    });
    h.play();
    CHECK(h.fx.world->roads().roads() == roads.size(), "(the world holds the real roads)");
    CHECK(ride_on(h, kMountSlot), "(the player rides the terror bird, summoned in front of it on the road)");
    if (!h.s->mounted()) return;
    Rider r = start_riding(h);
    const uint32_t mch = h.mount_ch();
    const content::MountData& data = h.fx.world->mounts()->data();
    // ride the polyline's samples, 99 cm a step (990 cm/s), from the nearest one ahead of the mount
    size_t from_vertex = best_i;
    double nearest = 1e18;
    for (size_t i = best_i; i < best->xy.size(); ++i) {
        const double d = std::hypot(best->xy[i].first - r.x, best->xy[i].second - r.y);
        if (d < nearest) {
            nearest = d;
            from_vertex = i;
        }
    }
    const auto go_to = [&](double tx, double ty) {
        int guard = 0;
        while (std::hypot(tx - r.x, ty - r.y) > 1.0 && guard++ < 400) {
            const double dist = std::hypot(tx - r.x, ty - r.y);
            const double step = std::min(dist, 99.0);
            ride_step(h, r, (tx - r.x) / dist * step, (ty - r.y) / dist * step);
        }
    };
    go_to(best->xy[from_vertex].first, best->xy[from_vertex].second);
    CHECK(h.s->ride()->road_tier() == 2 && std::fabs(mult_of(*h.mount()).value_or(0) - 1.2) < 1e-6, "on the real road the mount has Road_T2_Speed_Mount: MoveSpeedMult 1.2");
    const auto fx = items_of(*h.mount(), "UAoCStatsComponent", "CurrentEffects");
    CHECK(fx.size() == 1 && data.road(2) && fx[0].values.at(2).get_if<int64_t>() && *fx[0].values.at(2).get_if<int64_t>() == data.road(2)->effect, "the effect is the road's tier-2 effect, one item");
    const size_t refused = h.s->moves_refused();
    for (size_t i = from_vertex + 1; i < std::min(best->xy.size(), from_vertex + 25); ++i) go_to(best->xy[i].first, best->xy[i].second);
    CHECK(h.s->moves_refused() == refused && h.s->ride()->road_tier() >= 1, "riding along 25 samples of the road (5 km... 50 m each 200 cm... 48 m): no move refused, the effect stays on");
    size_t effect_updates = 0;
    for (const fake::Delivered* d : chains_on(h, mch, 0)) {
        const States st = states_of(*d, h.mount()->actor);
        for (const auto& [id, o] : st) {
            const schema::NetField* fe = o.schema ? o.schema->field("CurrentEffects") : nullptr;
            if (fe && o.arrays.find(fe->index)) ++effect_updates;
        }
    }
    CHECK(effect_updates >= 1 && effect_updates <= 3, "the effect was added once (at the mount-up, the window starts there) and not re-sent as the mount rode along the road (no flapping at the samples)");
    // off the road: 25 m to the side of the line (more than the half width): the effect goes at the first move that leaves it
    const auto& q0 = best->xy[std::min(best->xy.size() - 2, from_vertex + 24)];
    const auto& q1 = best->xy[std::min(best->xy.size() - 1, from_vertex + 25)];
    const double qlen = std::hypot(q1.first - q0.first, q1.second - q0.second);
    const double nx = -(q1.second - q0.second) / qlen, ny = (q1.first - q0.first) / qlen;
    // review B M3: a point that is on NO road (the dense Lions Hold has roads on both sides at 25 m): both sides, further and further, else the check fails instead of passing vacuously
    double off_x = 0, off_y = 0;
    bool found_off = false;
    for (const double away : {2500.0, 4000.0, 6000.0, 9000.0}) {
        for (const double side : {1.0, -1.0}) {
            if (!found_off && map.tier_at(r.x + nx * away * side, r.y + ny * away * side) < 0) {
                off_x = r.x + nx * away * side;
                off_y = r.y + ny * away * side;
                found_off = true;
            }
        }
    }
    CHECK(found_off, "(a point within 90 m of the road that is on no road)");
    if (!found_off) return;
    go_to(off_x, off_y);
    CHECK(map.tier_at(off_x, off_y) < 0 && h.s->ride()->road_tier() == -1 && mult_of(*h.mount()).value_or(0) == 1.0 && items_of(*h.mount(), "UAoCStatsComponent", "CurrentEffects").empty(),
          "off every road the effect is removed, MoveSpeedMult is back to 1");
    // and back on
    go_to(q1.first, q1.second);
    CHECK(h.s->ride()->road_tier() >= 1, "back on the road it is added again");
}
// -- Cyb x C | O | A --

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
    }
    test_get_on_and_off();
    test_riding_moves();
    test_dash();
    test_dash_gate();
    test_dash_survives_a_resummon();
    test_pace_bank_survives_a_dismount();
    test_roads();
    test_repeats_and_desummon();
    test_mounted_combat();
    test_health_comes_back_and_is_kept();
    test_filter();
    test_death_and_relog();
    test_other_mount_item();
    test_flyers_and_aquatic_on_the_ground();
    test_sprint_does_not_ride();
    test_hostile();
    test_flight_session();
    test_no_road_bonus_in_the_air();
    test_flight_on_a_ground_mount();
    test_swimming_session();
    test_water_dismount();
    test_air_dismount_and_relog();
    test_relog_flying_over_a_bowl();
    test_mounts_remain();
    test_distance_desummon();
    test_real_roads();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
