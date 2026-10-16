// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_mounts_session.cpp  (the mounts milestone, stage 2: summoning and desummoning)
//
//  A mount on a connection: a session with a fake client (the harness of test_worldd_area_session) on a PlayerWorld whose mounts are the real
//  content.db's (the working tree's dist/content/content.db, or AOC_CONTENT_DB) and whose character has a mount item in its bag.
//
//    the summon      ServerItemUsed and ServerTriggerAbility(Summon_Mount, the item) in one packet (retail's): the ability item and the bank-18 bar on the
//                    pawn and the PlayerController at +80 ms, the mount's open on a channel of its own ~104 ms in, bIsSummoning 0 after 0.47 s, the item's
//                    end at +1.54 s; nothing is consumed and no storage event or resync is sent for the item's use
//    the desummon    ServerDesummonMount on the PlayerController: bIsDesummoning, the run, the close (Destroyed) 3.2 s after the request; the channel is
//                    released; the item again summons a new one
//    refusals        a lock on the item: the callout on the PlayerController (and taken off 3 s later), no actor; a trigger that names no mount item, no
//                    item, another storage: ClientEndAbility and nothing else; summon spam; ServerDesummonMount with no mount
//    the cancel      a damaging hit during the cast
//    the world       the mount is the owner's alone (another player's connection would not be sent it), it leaves with the session
//    golden          the pawn's ability item and the PlayerController's bar, read as retail's (captures 911, 896, 898, 871), and the refusal's callout
// ============================================================================
#include "capture_mount_session.h"
#include "content_db_fixture.h"
#include "cyb_appearance.h"
#include "fake_client.h"
#include "bootstrap/captured_setup.h"
#include "bootstrap/character_setup.h"
#include "content/content_db.h"
#include "items/inventory.h"
#include "net/endpoint.h"
#include "protocol/repl/ability_rpc.h"
#include "protocol/repl/block_reader.h"
#include "protocol/repl/fast_array.h"
#include "protocol/repl/item_rpc.h"
#include "protocol/repl/rep_block.h"
#include "protocol/repl/skill_rpc.h"
#include "protocol/schema/generated/index.h"
#include "session/player_session.h"
#include "spawns/mount_spawn.h"
#include "world/actor_registry.h"

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
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)
// (Cyb : C O A)

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
    // the mounts: two of the data's (a terror bird, a horse), and one under lock (the terror bird's twin)
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
    void save(const std::string&, const svc::Position&) override {}
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

    explicit Fixture(bool real_world = false) {
        dir = cf::make_temp_dir("worldd_mounts_session_");
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
        // (a real world: the real content.db's talents catalog, so the join's lists are the production path's, not the captured Bard's)
        world = std::make_unique<PlayerWorld>(real_world ? &*real : &*content, "", bs::GameStateMode::Tier2, kServerId, guids);
        auto book = spawns::MountBook::load(*real, error);
        if (!book) {
            std::printf("  [FAIL] the mounts do not load: %s\n", error.c_str());
            std::exit(2);
        }
        for (const content::MountRow& r : book->data().mounts) {
            if (r.item_name == "Mount_Grounded_TerrorBird_0") g_mount_record = r.item;
            if (r.item_name == "Mount_Grounded_Horse_0") g_other_mount_record = r.item;
            if (r.kind == "aquatic" && !g_aquatic_record) g_aquatic_record = r.item;
            if (r.kind == "flying" && !g_flying_record) g_flying_record = r.item;
        }
        if (!g_mount_record || !g_other_mount_record) {
            std::printf("  [FAIL] the data has no terror bird or horse\n");
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

    explicit Harness(svc::Character r, bool real_world = false) : fx(real_world), record(std::move(r)) {
        fx.world->set_position_sink(&sink);
        // no NPCs and no ground: the world is the player's
        s = PlayerSession::join(conn, *fx.world, record, identity(), world_time_at(t), error);
        if (!s) {
            std::printf("  [FAIL] the session does not join: %s\n", error.c_str());
            std::exit(2);
        }
        pawn_ch = s->channel_of(bs::Role::Pawn);
        pc_ch = s->channel_of(bs::Role::PlayerController);
    }
// [C.Y.B - CoA]

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
    /// A client that has joined and phased in.
    void play();
    double since(Clock::time_point from) const { return std::chrono::duration<double>(t - from).count(); }
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

void Harness::play() {
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

/// ServerTriggerAbility(Summon_Mount) as the retail client sends it for a mount item: the item's slot, its whole instance and its location.
static BunchWriter summon_trigger(Harness& h, int32_t client_instance, int slot, const svc::InventoryItem* item, uint8_t location = 3) {
    const schema::ClassSchema& cls = class_named("UAoCAbilityComponent");
    const schema::RpcSchema* rpc = cls.rpc("ServerTriggerAbility");
    repl::RpcParams p;
    bool ok = rpc && repl::set_rpc_param(*rpc, p, "AbilityGuid", repl::Value::i64(108436548376185ll));
    if (ok && client_instance != 0) ok = repl::set_rpc_param(*rpc, p, "ClientInstanceId", repl::Value::i64(client_instance));
    ok = ok && repl::set_rpc_param(*rpc, p, "InputVector", repl::Value(repl::Vec2d{1, 0}));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.Slot", repl::Value::i64(slot));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.ItemInstance", repl::Value(item ? items::to_instance(*item) : repl::ItemInstance{}));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.Location", repl::Value::u64(location));
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
    header.object = h.s->bootstrap().setup().guids.ability;
    repl::write_content_block(out, header, field, /*server_form=*/false);
    return out;
}
// [C | O | A x Cyb]

static BunchWriter desummon_rpc() { return actor_rpc(class_named("AoCPlayerControllerBP_C"), "ServerDesummonMount", {}); }

/// The retail client's summon: ServerItemUsed and ServerTriggerAbility in one datagram, on the pawn's channel.
static void summon_with(Harness& h, int slot, int32_t client_instance = 7) {
    const svc::InventoryItem* item = h.s->inventory().at(Bag, slot);
    const std::string guid = item ? item->instance_guid : std::string();
    h.client_sends({h.client.actor(h.pawn_ch, item_used(3, slot, guid), true), h.client.actor(h.pawn_ch, summon_trigger(h, client_instance, slot, item), true)});
}

/// ServerItemUsed and ServerDesummonMount (on the PlayerController) in one datagram.
static void desummon_with(Harness& h, int slot) {
    const svc::InventoryItem* item = h.s->inventory().at(Bag, slot);
    h.client_sends({h.client.actor(h.pawn_ch, item_used(3, slot, item ? item->instance_guid : std::string()), true), h.client.actor(h.pc_ch, desummon_rpc(), true)});
}

// ---- what the client saw ---------------------------------------------------------------------------------------------------------------

struct Saw {
    double ability_item = -1, bar = -1, open = -1, summoning_off = -1, flags = -1, close = -1, item_end = -1, callout = -1;
    uint32_t mount_channel = 0;
    size_t opens = 0, closes = 0, updates_on_mount = 0;
};

static bool is_player_channel(const Harness& h, uint32_t ch) {
    return ch == h.pc_ch || ch == h.pawn_ch || ch == h.s->channel_of(bs::Role::GameState) || ch == h.s->channel_of(bs::Role::PlayerState);
}

static std::vector<repl::RpcCall> calls_of(const Harness& h, const fake::Delivered& d, const char* cls) {
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
    (void)h;
    return out;
}

/// The ReplicatedActiveAbilities items of the pawn's AbilityComponent as the session holds them: ability guid and montage object id by ServerInstanceId.
struct AbilityItem {
    int64_t ability = 0;
    int64_t instance = 0;
    uint64_t montage = 0;
    int64_t animation_counter = 0;
};
static std::vector<AbilityItem> ability_items(const Harness& h) {
    std::vector<AbilityItem> out;
    const repl::Actor& pawn = h.s->bootstrap().pawn();
    const repl::Subobject* ab = pawn.find(h.s->bootstrap().setup().guids.ability);
    if (!ab) return out;
    const repl::FastArrayState* arr = ab->obj.arrays.find(ab->obj.schema->field("ReplicatedActiveAbilities")->index);
    if (!arr) return out;
    for (const repl::FastArrayItem& it : arr->items()) {
        AbilityItem a;
        const auto& v = it.values.values;
        if (const auto f = v.find(1); f != v.end() && f->second.get_if<int64_t>()) a.ability = *f->second.get_if<int64_t>();
        if (const auto f = v.find(2); f != v.end() && f->second.get_if<int64_t>()) a.instance = *f->second.get_if<int64_t>();
        if (const auto f = v.find(11); f != v.end() && f->second.get_if<emit::FIntrepidNetworkGUID>()) a.montage = f->second.get_if<emit::FIntrepidNetworkGUID>()->ObjectId;
        if (const auto f = v.find(15); f != v.end() && f->second.get_if<int64_t>()) a.animation_counter = *f->second.get_if<int64_t>();
        out.push_back(a);
    }
    return out;
}

static size_t summon_items(const Harness& h, bool with_montage = false) {
    size_t n = 0;
    for (const AbilityItem& a : ability_items(h)) {
        if (a.ability == 108436548376185ll && (!with_montage || a.montage != 0)) ++n;
    }
    return n;
}

static size_t mount_count(const Harness& h) { return h.fx.world->registry().count(world::ActorKind::Mount); }

/// Runs `ms` milliseconds, noting when the client first saw each thing; `from` is the moment of the request.
static Saw watch(Harness& h, Clock::time_point from, milliseconds ms, Saw saw = {}) {
    for (milliseconds done{0}; done < ms; done += milliseconds(20)) {
        const size_t before = h.got.size();
        h.step();
        const double now = h.since(from);
        for (size_t i = before; i < h.got.size(); ++i) {
            const fake::Delivered& d = h.got[i];
            if (d.channel == h.pc_ch && !d.open && !d.close) {
                for (const repl::RpcCall& c : calls_of(h, d, "AoCPlayerControllerBP_C")) {
                    if (c.rpc && std::string(c.rpc->name) == "ClientClearActionbarsForInstigators" && saw.bar < 0) saw.bar = now;
                }
            }
            if (is_player_channel(h, d.channel)) continue;
            if (d.open) {
                ++saw.opens;
                if (saw.open < 0) {
                    saw.open = now;
                    saw.mount_channel = d.channel;
                }
            } else if (d.close) {
                ++saw.closes;
                if (saw.close < 0) saw.close = now;
            } else if (d.channel == saw.mount_channel) {
                ++saw.updates_on_mount;
            }
        }
        if (saw.ability_item < 0 && summon_items(h, true) > 0) saw.ability_item = now;
        if (saw.ability_item >= 0 && saw.item_end < 0 && summon_items(h, true) == 0 && summon_items(h) > 0) saw.item_end = now;
        if (saw.callout < 0 && h.s->bootstrap().callouts() > 0) saw.callout = now;
        if (const world::WorldActor* m = h.s->mounts() ? h.s->mounts()->mount() : nullptr) {
            for (const repl::Subobject& sub : m->actor.subobjects) {
                if (!sub.obj.schema || std::string(sub.obj.schema->name) != "UMountComponent") continue;
                const repl::Value* v = sub.obj.state.get(*sub.obj.schema, "bIsSummoning");
                if (saw.open >= 0 && saw.summoning_off < 0 && v && v->get_if<bool>() && !*v->get_if<bool>()) saw.summoning_off = now;
                const repl::Value* f = sub.obj.state.get(*sub.obj.schema, "bIsDesummoning");
                if (saw.flags < 0 && f && f->get_if<bool>() && *f->get_if<bool>()) saw.flags = now;
            }
        }
    }
    return saw;
}

static bool within(double got, double want, double tol) { return got >= 0 && std::fabs(got - want) <= tol; }

// ---- the tests -------------------------------------------------------------------------------------------------------------------------
// built by CoA with C | Y | B

static void test_summon_and_desummon() {
    std::printf("the summon and the desummon of a mount on a connection\n");
    Harness h(cyb_record());
    h.play();
    const size_t bag_before = h.s->inventory().count(Bag);
    const size_t applied_before = h.s->item_requests_applied(), refused_before = h.s->item_requests_refused();
    const size_t abilities_before = ability_items(h).size();
    const Clock::time_point req = h.t;
    const size_t got_before = h.got.size();
    summon_with(h, kMountSlot);
    CHECK(h.s->mount_item_uses() == 1 && h.s->mount_summon_requests() == 1, "the session saw the item used and the summon trigger");
    Saw saw = watch(h, req, milliseconds(2600));
    CHECK(saw.ability_item >= 0 && within(saw.ability_item, 0.08, 0.03), "the Summon_Mount ability item is on the pawn at about +80 ms");
    CHECK(within(saw.bar, 0.08, 0.04), "the bank-18 action bar goes out with it (ClientClearActionbarsForInstigators, then the slot)");
    CHECK(saw.open >= 0 && saw.open >= 0.10 && saw.open <= 0.30, "the mount's actor opens on a channel of its own (about +104 ms, the view's next pass)");
    CHECK(saw.mount_channel != 0 && !is_player_channel(h, saw.mount_channel) && saw.opens == 1, "one open, on a channel none of the player's four");
    CHECK(saw.summoning_off >= 0 && within(saw.summoning_off, 0.104 + 0.47, 0.05), "bIsSummoning goes off 0.47 s after the open");
    CHECK(saw.item_end >= 0 && within(saw.item_end, 1.541, 0.05), "the ability item ends (no montage) at +1.54 s");
    CHECK(mount_count(h) == 1 && h.s->mounts()->phase() == MountController::Phase::Out, "one mount is out");
    CHECK(saw.updates_on_mount >= 1, "the client got the bIsSummoning update on the mount's channel");
    // the use consumed nothing and asked for no resync
    CHECK(h.s->inventory().count(Bag) == bag_before && h.s->inventory().at(Bag, kMountSlot) != nullptr && h.s->item_requests_applied() == applied_before && h.s->item_requests_refused() == refused_before,
          "the mount item is still in the bag, the item request is neither applied nor refused");
    CHECK(abilities_before + 1 >= ability_items(h).size(), "(one ability item added)");
    // the mount's place: 575 cm in front of the player
    const world::WorldActor* m = h.s->mounts()->mount();
    const auto& good = h.s->movement().good();
    const double yaw = good.yaw_degrees * 3.14159265358979 / 180.0;
    CHECK(m && std::hypot(m->x - (good.x + 575 * std::cos(yaw)), m->y - (good.y + 575 * std::sin(yaw))) < 1.0 && m->owner == static_cast<uint64_t>(h.s->bootstrap().setup().player_id),
          "it stands 575 cm in front of the player, its owner's");
    // a mount item used again with the mount out is the client's desummon: ServerItemUsed + ServerDesummonMount
    const Clock::time_point req2 = h.t;
    desummon_with(h, kMountSlot);
    CHECK(h.s->mount_desummon_requests() == 1 && h.s->mounts()->phase() == MountController::Phase::Desummoning, "the desummon begins");
    Saw init;
    init.mount_channel = saw.mount_channel;
    Saw gone = watch(h, req2, milliseconds(3600), init);
    CHECK(within(gone.flags, 0.1, 0.04), "bIsDesummoning goes on at about +0.1 s");
    CHECK(gone.close >= 0 && within(gone.close, 3.2, 0.06), "the channel is closed 3.2 s after the request");
    CHECK(mount_count(h) == 0 && h.s->mounts()->phase() == MountController::Phase::Idle, "the mount is gone from the world");
    CHECK(gone.updates_on_mount >= 10, "the client got the dissolve's flags and the run's ReplicatedMovement on the mount's channel");
    // the same item summons again, on a channel that is free again
    const Clock::time_point req3 = h.t;
    summon_with(h, kMountSlot, 9);
    Saw again = watch(h, req3, milliseconds(500));
    CHECK(again.open >= 0 && mount_count(h) == 1, "the item summons a new mount");
    (void)got_before;
}

static void test_refusals_and_hostile() {
    std::printf("refusals and hostile requests\n");
    {
        Harness h(cyb_record());
        h.play();
        const Clock::time_point req = h.t;
        summon_with(h, kLockedSlot, 11);
        Saw saw = watch(h, req, milliseconds(4500));
        CHECK(saw.open < 0 && mount_count(h) == 0, "a mount item under lock summons nothing");
        CHECK(saw.ability_item >= 0 && within(saw.ability_item, 0.105, 0.04) && saw.item_end >= 0 && within(saw.item_end, 1.6, 0.05), "the ability item shows at +105 ms and ends at +1.6 s");
        CHECK(saw.callout >= 0 && within(saw.callout, 0.105, 0.04), "the callout is on the PlayerController");
        CHECK(saw.bar < 0, "no mount bar");
        CHECK(h.s->bootstrap().callouts() == 0, "the callout is taken off again (3 s later)");
        CHECK(h.s->mounts()->stats().refused == 1, "counted");
    }
    {
        // nothing the client can name makes a mount, or leaves the session in a state it cannot leave
        Harness h(cyb_record());
        h.play();
        size_t ends_before = h.s->ability_ends_sent();
        // a trigger naming a sword, an empty slot, the equipment, a negative slot
        const svc::InventoryItem* sword = h.s->inventory().at(Bag, kSwordSlot);
        h.client_sends({h.client.actor(h.pawn_ch, summon_trigger(h, 21, kSwordSlot, sword), true)});
        h.settle();
        h.client_sends({h.client.actor(h.pawn_ch, summon_trigger(h, 22, 40, nullptr), true)});
        h.settle();
        h.client_sends({h.client.actor(h.pawn_ch, summon_trigger(h, 23, kMountSlot, h.s->inventory().at(Bag, kMountSlot), 4), true)});   // the Equipment
        h.settle();
        h.client_sends({h.client.actor(h.pawn_ch, summon_trigger(h, 24, -1, nullptr), true)});
        h.settle();
        CHECK(mount_count(h) == 0 && h.s->mounts()->stats().summons == 0, "a trigger that names a sword, an empty slot, the equipment or no item makes no mount");
        CHECK(h.s->ability_ends_sent() >= ends_before + 4, "each is answered with ClientEndAbility (the client's own cast is ended)");
        // ServerDesummonMount with no mount
        h.client_sends({h.client.actor(h.pc_ch, desummon_rpc(), true)});
        h.settle();
        CHECK(mount_count(h) == 0 && h.s->mounts()->phase() == MountController::Phase::Idle, "ServerDesummonMount with no mount does nothing");
    }
    {
        // summon spam: 30 summons in a burst: at most one mount at once, the session lives
        Harness h(cyb_record());
        h.play();
        size_t max_mounts = 0;
        for (int i = 0; i < 30; ++i) {
            summon_with(h, kMountSlot, 100 + i);
            h.step(milliseconds(10));
            max_mounts = std::max(max_mounts, mount_count(h));
        }
        for (int i = 0; i < 400; ++i) {
            h.step();
            max_mounts = std::max(max_mounts, mount_count(h));
        }
        CHECK(max_mounts <= 1 && !h.s->failed(), "30 summons in 0.3 s never make two mounts and do not fail the session");
        CHECK(h.s->requests_dropped() > 0 || h.s->mount_requests_ignored() > 0, "the budget and the one-mount rule dropped the rest");
        // desummon spam
        for (int i = 0; i < 30; ++i) {
            h.client_sends({h.client.actor(h.pc_ch, desummon_rpc(), true)});
            h.step(milliseconds(10));
        }
        h.run(milliseconds(4000));
        CHECK(mount_count(h) == 0 && !h.s->failed() && h.s->mounts()->phase() == MountController::Phase::Idle, "30 desummons settle: no mount, no failure");
    }
    {
        // one mount per player: another mount item while one is out desummons it
        Harness h(cyb_record());
        h.play();
        summon_with(h, kMountSlot, 31);
        h.run(milliseconds(1800));
        CHECK(mount_count(h) == 1, "(a mount is out)");
        summon_with(h, kOtherMountSlot, 32);
        h.run(milliseconds(3800));
        CHECK(mount_count(h) == 0 && h.s->mounts()->stats().summons == 1, "a trigger for another mount item while one is out desummons the old and summons nothing");
    }
}
// [C-Y-B - CoA]

static void test_cancel_by_damage() {
    std::printf("a damaging hit during the cast\n");
    Harness h(cyb_record());
    h.play();
    const Clock::time_point req = h.t;
    summon_with(h, kMountSlot, 41);
    h.step();
    h.step();
    h.step();   // +60 ms: the item is shown (+80 ms) next
    Connection& c = h.conn;
    h.s->take_damage(c, 10.0, h.t);
    Saw saw = watch(h, req, milliseconds(1500));
    CHECK(saw.open < 0 && mount_count(h) == 0, "a hit before the open: no mount");
    CHECK(h.s->mounts()->stats().cancelled == 1 && h.s->mounts()->phase() == MountController::Phase::Idle, "the summon is cancelled");
    // after the cast is over a hit leaves the mount
    const Clock::time_point req2 = h.t;
    summon_with(h, kMountSlot, 42);
    watch(h, req2, milliseconds(1800));
    h.s->take_damage(c, 10.0, h.t);
    h.run(milliseconds(500));
    CHECK(mount_count(h) == 1 && h.s->mounts()->stats().cancelled == 1, "a hit after the cast leaves the mount");
    // stage 4, the user's rule: a hit NEVER desummons a mount that has appeared (the run of 2026-10-08: mobs on the player, every summon died 3 s later)
    summon_with(h, kMountSlot, 43);   // a second summon while one is out desummons it: start from nothing
    h.run(milliseconds(3800));
    CHECK(mount_count(h) == 0, "(the first mount is gone)");
    const size_t desummons_before = h.s->mounts()->stats().desummons, cancelled_before = h.s->mounts()->stats().cancelled;
    summon_with(h, kMountSlot, 44);
    h.run(milliseconds(300));   // the mount is open (+104 ms), the cast has about 1.2 s to go
    CHECK(mount_count(h) == 1, "(the mount has appeared)");
    for (int i = 0; i < 12; ++i) {   // mobs on the player: a hit every 500 ms for 6 s, through the end of the cast and well past 3 s
        h.s->take_damage(c, 1.0, h.t);   // (a level 1 character's Health is small: a hit of 10 twelve times would kill it, and its death desummons the mount)
        h.run(milliseconds(500));
    }
    CHECK(mount_count(h) == 1 && h.s->mounts()->phase() == MountController::Phase::Out && h.s->mounts()->stats().cancelled == cancelled_before && h.s->mounts()->stats().desummons == desummons_before,
          "hits during and after the cast, for 6 s: the mount that appeared stays, nothing is cancelled or desummoned");
}

static void test_world() {
    std::printf("the mount belongs to its owner and leaves with the session\n");
    Harness h(cyb_record());
    h.play();
    summon_with(h, kMountSlot, 51);
    h.run(milliseconds(1800));
    const world::ActorRegistry& reg = h.fx.world->registry();
    const world::WorldActor* m = h.s->mounts()->mount();
    CHECK(m && m->kind == world::ActorKind::Mount && m->owner != 0 && m->never_dormant && reg.count(world::ActorKind::Mount) == 1, "the registry holds one Mount actor with an owner, never dormant");
    const uint64_t id = m ? m->id : 0;
    // a session that is not its owner's: the view skips an actor owned by another player (the lingering zones' rule), checked on the actor's own field
    CHECK(m && m->owner == static_cast<uint64_t>(h.s->bootstrap().setup().player_id), "its owner is this player: another player's connection would not be sent it");
    h.s->finish("the test");
    CHECK(reg.find(id) == nullptr && reg.count(world::ActorKind::Mount) == 0, "the mount leaves the world when its owner's session ends");
}

/// Whether a bunch's bits hold `text` as consecutive bytes at any bit alignment (an FString inside an export, which does not start on a byte).
static bool bits_contain_text(const emit::BunchWriter& w, const std::string& text) {
    if (text.empty()) return false;
    const size_t nbits = w.bit_pos();
    const uint8_t* d = w.data();
    for (size_t shift = 0; shift < 8; ++shift) {
        std::string bytes;
        for (size_t bit = shift; bit + 8 <= nbits; bit += 8) {
            const size_t i = bit / 8, o = bit % 8;
            uint8_t b = static_cast<uint8_t>(d[i] >> o);
            if (o != 0) b = static_cast<uint8_t>(b | (d[i + 1] << (8 - o)));
            bytes.push_back(static_cast<char>(b));
        }
        if (bytes.find(text) != std::string::npos) return true;
    }
    return false;
}
// C.Y.B with C-O-A

static void test_open_on_the_wire() {
    std::printf("the open the client received\n");
    Harness h(cyb_record());
    h.play();
    const size_t from = h.got.size();
    summon_with(h, kMountSlot, 61);
    h.run(milliseconds(500));
    const world::WorldActor* m = h.s->mounts()->mount();
    const fake::Delivered* open = nullptr;
    for (size_t i = from; i < h.got.size(); ++i) {
        if (h.got[i].open && !is_player_channel(h, h.got[i].channel)) open = &h.got[i];
    }
    CHECK(open != nullptr && m != nullptr, "an open reached the client");
    if (!open || !m) return;
    // the exports carry the archetype (the terror bird's retail export) and the subobjects' names
    CHECK(!open->exports.empty(), "the open carries its exports (the archetype and the subobjects' names)");
    // stage 3 fix 2: the rider's AnimBP.  The InteractableComponent names it (AttachmentAnimation) and retail's open exports it [R: 911, 898, 871]; without the export the
    // client cannot find the class, the rider keeps its on-foot AnimBP and stands on the mount ("Unexpected invalid anim instance when initializing rider anim proxy")
    {
        const content::MountRow* row = h.s->mounts()->book().find(h.s->mounts()->item_record());
        const std::string anim = row ? h.s->mounts()->book().rider_anim_of(*row) : std::string();
        const size_t dot = anim.rfind('.');
        const std::string package = dot == std::string::npos ? std::string() : anim.substr(0, dot);
        const std::string klass = dot == std::string::npos ? std::string() : anim.substr(dot + 1);
        bool has_class = false, has_package = false;
        for (const emit::BunchWriter& x : open->exports) {
            has_class = has_class || bits_contain_text(x, klass);
            has_package = has_package || bits_contain_text(x, package);
        }
        CHECK(!klass.empty() && has_class && has_package, "the export bunches name the rider's AnimBP class and its package");
    }
    // stage 3 fix 2: the mount inherits its owner's alignments as the pawn holds them: the unflagged player's, with no node, no guild and no PvP flag
    {
        const bs::SessionGuids& g = h.s->bootstrap().setup().guids;
        const repl::Subobject* pa = h.s->bootstrap().pawn().find(g.alignment);
        const repl::Value* pv = pa ? pa->obj.state.get(*pa->obj.schema, "Alignments") : nullptr;
        const auto* pawn_set = pv ? pv->get_if<repl::AlignmentContainer>() : nullptr;
        const repl::Subobject* ma = spawns::sub_of(*m, "UAlignmentComponent");
        const repl::Value* mv = ma ? ma->obj.state.get(*ma->obj.schema, "InheritedAlignments") : nullptr;
        const auto* mount_set = mv ? mv->get_if<repl::AlignmentContainer>() : nullptr;
        CHECK(pawn_set && mount_set && *pawn_set == *mount_set, "the mount's InheritedAlignments are the pawn's Alignments, instance for instance");
        bool flagged = false;
        if (mount_set) {
            for (const auto& i : mount_set->instances) flagged = flagged || i.key == 6064629443707731976ull || i.key == 6064631051897143296ull || i.key == 6064630560782155776ull;
        }
        CHECK(mount_set && !flagged, "(and so hold no Combatant, no node and no guild)");
    }
    const auto blocks = repl::read_content_blocks(open->content.data(), open->content.bit_pos(), 0);
    size_t start = 0;
    std::optional<std::vector<repl::BlockView>> found;
    for (start = 0; start < 2500 && !found; ++start) {
        auto got = repl::read_content_blocks(open->content.data(), open->content.bit_pos(), start);
        if (got && got->size() == m->actor.subobjects.size() + 1 && !got->empty() && (*got)[0].header.is_actor) found = std::move(got);
    }
    CHECK(found.has_value(), "its content reads as the actor's block and one block per subobject (9)");
    (void)blocks;
}


// ---- golden: what retail sent for the same requests -------------------------------------------------------------------------------------

namespace cm = capture_mount_session;

static std::vector<uint8_t> bytes_of_hex(const char* hex) {
    std::vector<uint8_t> out;
    for (const char* h = hex; h[0] && h[1]; h += 2) out.push_back(static_cast<uint8_t>(std::stoi(std::string(h, 2), nullptr, 16)));
    return out;
}

static const cm::Payload& payload_of(const cm::Block& b) { return cm::kPayloads[b.payload]; }

/// A retail block read into values with the schema of its class (the generator checked that it reads to its last bit).
static std::optional<repl::ObjectState> load_block(const cm::Block& b) {
    const schema::ClassSchema* cls = gen::find_class_schema(b.cls);
    if (!cls) return std::nullopt;
    std::string why;
    return repl::load_object_state_hex(*cls, payload_of(b).hex, payload_of(b).bits, b.rep, &why);
}

/// The values of the item of a ReplicatedActiveAbilities update (the one changed item).
static std::optional<repl::ElementState> ability_item_of(const repl::ObjectState& o) {
    const schema::NetField* f = o.schema->field("ReplicatedActiveAbilities");
    const repl::FastArrayState* a = f ? o.arrays.find(f->index) : nullptr;
    if (!a || a->items().empty()) return std::nullopt;
    return a->items().back().values;
}

/// worldd's item of Summon_Mount: the one with (`with_montage`) or without a montage.
static std::optional<repl::ElementState> our_item(const Harness& h, bool with_montage) {
    const repl::Actor& pawn = h.s->bootstrap().pawn();
    const repl::Subobject* ab = pawn.find(h.s->bootstrap().setup().guids.ability);
    const repl::FastArrayState* arr = ab ? ab->obj.arrays.find(ab->obj.schema->field("ReplicatedActiveAbilities")->index) : nullptr;
    if (!arr) return std::nullopt;
    std::optional<repl::ElementState> out;
    for (const repl::FastArrayItem& it : arr->items()) {
        const auto ab_id = it.values.values.find(1);
        const auto mt = it.values.values.find(11);
        if (ab_id == it.values.values.end() || !ab_id->second.get_if<int64_t>() || *ab_id->second.get_if<int64_t>() != 108436548376185ll) continue;
        const bool has = mt != it.values.values.end() && mt->second.get_if<emit::FIntrepidNetworkGUID>() && mt->second.get_if<emit::FIntrepidNetworkGUID>()->ObjectId != 0;
        if (has == with_montage) out = it.values;
    }
    return out;
}

/// The handles whose values differ between two ability items (by handle number).
static std::set<uint16_t> item_diff(const repl::ElementState& a, const repl::ElementState& b) {
    std::set<uint16_t> out;
    std::set<uint16_t> all;
    for (const auto& [h, v] : a.values) all.insert(h);
    for (const auto& [h, v] : b.values) all.insert(h);
    for (const uint16_t h : all) {
        const auto x = a.values.find(h), y = b.values.find(h);
        if (x == a.values.end() || y == b.values.end() || !(x->second == y->second)) out.insert(h);
    }
    return out;
}
// ~ Cyb - C-O-A ~

/// The PlayerController's calls in a chain, by name.
static std::vector<repl::RpcCall> pc_calls_of(const Harness& h, const fake::Delivered& d) { return calls_of(h, d, "AoCPlayerControllerBP_C"); }

/// The calls of a retail PlayerController block (a field loop of RPCs).
static std::vector<repl::RpcCall> retail_pc_calls(const cm::Block& b) {
    const auto bytes = bytes_of_hex(payload_of(b).hex);
    size_t pos = 0;
    auto calls = repl::read_rpc_fields(class_named("AoCPlayerControllerBP_C"), bytes.data(), payload_of(b).bits, pos);
    return calls ? std::move(*calls) : std::vector<repl::RpcCall>{};
}

static const cm::Block* retail(uint16_t capture, const char* role) {
    const cm::Block* b = cm::find(capture, role);
    if (!b) std::printf("  [FAIL] the header has no %s of capture %u\n", role, capture);
    return b;
}

/// One summon on a fresh session: the item as it showed, as it ended, the PlayerController's calls and when the client saw what.
struct OurSummon {
    std::optional<repl::ElementState> shown, ended;
    std::vector<repl::RpcCall> pc_calls;
    Saw saw;
};

static OurSummon run_our_summon(int slot) {
    OurSummon o;
    Harness h(cyb_record());
    h.play();
    const size_t from = h.got.size();
    const Clock::time_point req = h.t;
    summon_with(h, slot, 7);
    for (milliseconds done{0}; done < milliseconds(2200); done += milliseconds(20)) {
        o.saw = watch(h, req, milliseconds(20), o.saw);
        if (!o.shown) o.shown = our_item(h, true);
    }
    o.ended = our_item(h, false);
    for (size_t i = from; i < h.got.size(); ++i) {
        if (h.got[i].channel != h.pc_ch || h.got[i].open || h.got[i].close) continue;
        for (auto& c : pc_calls_of(h, h.got[i])) o.pc_calls.push_back(std::move(c));
    }
    return o;
}

/// True when every difference between two ability items is one of the handles that are an instance's own: the ServerInstanceId (2), the animation counter (15)
/// and the instigator (17: a NetGUID of that player's pawn).
static bool only_instance_values_differ(const repl::ElementState& a, const repl::ElementState& b, std::string* what = nullptr, bool prediction_flag = false) {
    const std::set<uint16_t> d = item_diff(a, b);
    bool ok = true;
    for (const uint16_t x : d) {
        if (x != 2 && x != 15 && x != 17 && !(prediction_flag && x == 9)) ok = false;
        if (what) *what += std::to_string(x) + " ";
    }
    return ok;
}
// C-Y-B and CoA

static void test_golden_summon() {
    std::printf("golden: the summon of captures 911, 896, 898 and 871\n");
    // one session per kind of mount: the aquatic one's bar has another slot and ability (898)
    const OurSummon by_kind[3] = {run_our_summon(kMountSlot), run_our_summon(kAquaticSlot), run_our_summon(kFlyingSlot)};
    const OurSummon& ours = by_kind[0];
    CHECK(ours.shown.has_value() && ours.ended.has_value() && by_kind[1].shown.has_value() && by_kind[2].shown.has_value(), "worldd shows the Summon_Mount item and ends it, for a ground, an aquatic and a flying mount");
    int item_lo = 100000, item_hi = 0, open_lo = 100000, open_hi = 0, end_lo = 100000, end_hi = 0, off_lo = 100000, off_hi = 0;
    size_t same_item = 0, same_end = 0, same_bar = 0, read = 0;
    for (const uint16_t capture : {911, 896, 898, 871}) {
        const OurSummon& kind_run = capture == 898 ? by_kind[1] : capture == 871 ? by_kind[2] : by_kind[0];
        const cm::Block* item = retail(capture, "item");
        const cm::Block* end = retail(capture, "end");
        const cm::Block* clear = retail(capture, "bar_clear");
        const cm::Block* bar = retail(capture, "bar");
        const cm::Block* open = retail(capture, "open");
        const cm::Block* off = retail(capture, "off");
        if (!item || !end || !clear || !bar || !open || !off) continue;
        item_lo = std::min(item_lo, item->ms);
        item_hi = std::max(item_hi, item->ms);
        open_lo = std::min(open_lo, open->ms);
        open_hi = std::max(open_hi, open->ms);
        end_lo = std::min(end_lo, end->ms);
        end_hi = std::max(end_hi, end->ms);
        const int off_after_open = off->ms - open->ms;
        off_lo = std::min(off_lo, off_after_open);
        off_hi = std::max(off_hi, off_after_open);
        const auto ri = load_block(*item);
        const auto re = load_block(*end);
        const auto ri_item = ri ? ability_item_of(*ri) : std::nullopt;
        const auto re_item = re ? ability_item_of(*re) : std::nullopt;
        if (!ri_item || !re_item) {
            CHECK(false, (std::string("the retail item and end of ") + std::to_string(capture) + " read").c_str());
            continue;
        }
        ++read;
        // retail's own two items differ in the montage and the animation counter (and, in 871's, in bBranchPredictedOnAutonomous: the client's prediction flag)
        {
            const std::set<uint16_t> d = item_diff(*ri_item, *re_item);
            CHECK(d == std::set<uint16_t>({11, 15}) || (capture == 871 && d == std::set<uint16_t>({9, 11, 15})),
                  (std::string("capture ") + std::to_string(capture) + ": its end differs from its item in the montage and the animation counter").c_str());
        }
        std::string what;
        if (ours.shown && only_instance_values_differ(*ri_item, *ours.shown, &what)) ++same_item;
        else if (ours.shown) std::printf("      capture %u: the item differs in handles %s\n", capture, what.c_str());
        what.clear();
        if (ours.ended && only_instance_values_differ(*re_item, *ours.ended, &what, capture == 871)) ++same_end;
        else if (ours.ended) std::printf("      capture %u: the end differs in handles %s\n", capture, what.c_str());
        const auto rc = retail_pc_calls(*clear);
        const auto rb = retail_pc_calls(*bar);
        bool ok = rc.size() == 1 && rb.size() == 1;
        size_t ours_clear = 0, ours_bar = 0;
        for (const repl::RpcCall& c : kind_run.pc_calls) {
            if (!c.rpc || !ok) continue;
            if (std::string(c.rpc->name) == "ClientClearActionbarsForInstigators") {
                ++ours_clear;
                ok = ok && c.params.values == rc[0].params.values;
            } else if (std::string(c.rpc->name) == "ClientReceiveActionbarSlotPayload") {
                ++ours_bar;
                ok = ok && c.params.values == rb[0].params.values;
            }
        }
        if (ok && ours_clear == 1 && ours_bar == 1) ++same_bar;
    }
    CHECK(read == 4, "the four retail summons read");
    CHECK(same_item == 4, "the Summon_Mount item worldd shows is retail's but for the instance, the animation counter and the instigator (4 of 4)");
    CHECK(same_end == 4, "the item worldd ends is retail's end item but for the same three (and 871's prediction flag) (4 of 4)");
    CHECK(same_bar == 4, "the bank-18 bar is retail's two calls with retail's values (4 of 4)");
    std::printf("    retail: the item at +%d to +%d ms, the open at +%d to +%d ms, bIsSummoning off %d to %d ms after the open, the end at +%d to +%d ms\n", item_lo, item_hi, open_lo, open_hi,
                off_lo, off_hi, end_lo, end_hi);
    CHECK(ours.saw.ability_item * 1000 >= item_lo - 25 && ours.saw.ability_item * 1000 <= item_hi + 25, "worldd's item comes within retail's range");
    CHECK(ours.saw.open * 1000 >= open_lo - 30 && ours.saw.open * 1000 <= open_hi + 120, "the open comes within retail's range (plus the view's pass: a connection looks for new actors every 67 ms)");
    const double off_after = (ours.saw.summoning_off - ours.saw.open) * 1000;
    CHECK(off_after >= off_lo - 40 && off_after <= off_hi + 40, "bIsSummoning goes off within retail's range after the open");
    CHECK(ours.saw.item_end * 1000 >= end_lo - 60 && ours.saw.item_end * 1000 <= end_hi + 60, "the end of the cast is within retail's range");
}

/// The handles set in each content block of a chain, by the object the block names (0: the actor), for a chain on the mount's channel.
static std::vector<std::pair<uint64_t, std::set<uint16_t>>> handles_of(const fake::Delivered& d, const world::WorldActor& m) {
    std::vector<std::pair<uint64_t, std::set<uint16_t>>> out;
    const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
    if (!blocks) return out;
    for (const repl::BlockView& b : *blocks) {
        const repl::ObjectState* obj = nullptr;
        uint64_t id = 0;
        if (b.header.is_actor) {
            obj = &m.actor.obj;
        } else {
            for (const repl::Subobject& s : m.actor.subobjects) {
                if (s.guid.ObjectId == b.header.object.ObjectId) {
                    obj = &s.obj;
                    id = s.guid.ObjectId;
                }
            }
        }
        if (!obj || !b.header.has_rep_layout) continue;
        size_t pos = b.payload_pos;
        const auto st = repl::read_rep_block(*obj->schema, d.content.data(), b.payload_pos + b.payload_bits, pos, obj->opts);
        if (!st) continue;
        std::set<uint16_t> hs;
        for (const auto& [handle, v] : st->values()) hs.insert(handle);
        out.emplace_back(id, std::move(hs));
    }
    return out;
}

static std::set<uint16_t> handles_in(const repl::ObjectState& s) {
    std::set<uint16_t> out;
    for (const auto& [h, v] : s.state.values()) out.insert(h);
    return out;
}
// C:Y:B captures, C O A code

static void test_golden_desummon() {
    std::printf("golden: the desummon of captures 874, 893 and 898\n");
    // what retail's blocks carry
    struct Seen {
        std::set<uint16_t> flags, combat, run, flags_off, input_off;
        int flags_ms = 0, run_ms = 0, off_ms = 0, input_off_ms = 0, close_ms = 0;
    };
    std::map<uint16_t, Seen> retail_seen;
    for (const uint16_t capture : {874, 893, 898}) {
        Seen s;
        const cm::Block* b[] = {retail(capture, "flags"), retail(capture, "combat"), retail(capture, "run"), retail(capture, "flags_off"), retail(capture, "input_off"), retail(capture, "close")};
        bool all = true;
        for (const cm::Block* x : b) all = all && x != nullptr;
        if (!all) continue;
        const auto flags = load_block(*b[0]);
        const auto combat = load_block(*b[1]);
        const auto run = load_block(*b[2]);
        const auto off = load_block(*b[3]);
        const auto in_off = load_block(*b[4]);
        CHECK(flags && combat && run && off && in_off, (std::string("retail's blocks of ") + std::to_string(capture) + " read").c_str());
        if (!(flags && combat && run && off && in_off)) continue;
        s.flags = handles_in(*flags);
        s.combat = handles_in(*combat);
        s.run = handles_in(*run);
        s.flags_off = handles_in(*off);
        s.input_off = handles_in(*in_off);
        s.flags_ms = b[0]->ms;
        s.run_ms = b[2]->ms;
        s.off_ms = b[3]->ms;
        s.input_off_ms = b[4]->ms;
        s.close_ms = b[5]->ms;
        retail_seen[capture] = s;
        // the values: bIsDesummoning 1, a DissolveTimer of 0.03 to 0.04, invulnerable and not targetable, a movement input forward
        const repl::Value* d = flags->state.get(*flags->schema, "bIsDesummoning");
        const repl::Value* t = flags->state.get(*flags->schema, "DissolveTimer");
        const repl::Value* inv = combat->state.get(*combat->schema, "bInvulnerable");
        const repl::Value* nt = combat->state.get(*combat->schema, "bNotTargetable");
        const repl::Value* in = run->state.get(*run->schema, "CurrentMovementInput");
        CHECK(d && d->get_if<bool>() && *d->get_if<bool>() && t && t->get_if<float>() && *t->get_if<float>() > 0.02f && *t->get_if<float>() < 0.06f &&
                  inv && inv->get_if<bool>() && *inv->get_if<bool>() && nt && nt->get_if<bool>() && *nt->get_if<bool>() && in && in->get_if<repl::PlayerMovementInput>() &&
                  in->get_if<repl::PlayerMovementInput>()->bits != 0,
              (std::string("capture ") + std::to_string(capture) + ": bIsDesummoning, a short DissolveTimer, invulnerable, untargetable, the input forward").c_str());
    }
    // worldd's, on the connection: the same updates, handle for handle, at times in retail's range
    Harness h(cyb_record());
    h.play();
    summon_with(h, kMountSlot, 7);
    h.run(milliseconds(2000));
    const world::WorldActor* m = h.s->mounts()->mount();
    if (!m) {
        CHECK(false, "(a mount is out)");
        return;
    }
    const uint64_t channel = h.s->actor_view()->channel_of(m->id);
    const world::WorldActor snapshot = *m;
    const Clock::time_point req = h.t;
    const size_t from = h.got.size();
    desummon_with(h, kMountSlot);
    struct Our {
        double flags = -1, run = -1, off = -1, input_off = -1, close = -1;
        std::set<uint16_t> flags_h, combat_h, run_h, off_h, input_off_h;
    } ours;
    size_t scanned = from;
    for (milliseconds done{0}; done < milliseconds(3600); done += milliseconds(20)) {
        h.step();
        const double now = h.since(req);
        for (; scanned < h.got.size(); ++scanned) {
            const fake::Delivered& d = h.got[scanned];
            if (d.channel != channel) continue;
            if (d.close) {
                if (ours.close < 0) ours.close = now;
                continue;
            }
            const world::WorldActor* cur = h.s->mounts()->mount();
            const world::WorldActor& ref = cur ? *cur : snapshot;
            for (const auto& [id, hs] : handles_of(d, ref)) {
                const bool is_actor = id == 0;
                const bool mount_comp = !is_actor && [&] {
                    for (const repl::Subobject& s : ref.actor.subobjects)
                        if (s.guid.ObjectId == id) return std::string(s.obj.schema->name) == "UMountComponent";
                    return false;
                }();
                const bool combat_comp = !is_actor && [&] {
                    for (const repl::Subobject& s : ref.actor.subobjects)
                        if (s.guid.ObjectId == id) return std::string(s.obj.schema->name) == "UNPCCombatInfo";
                    return false;
                }();
                const schema::ClassSchema& mc = class_named("UMountComponent");
                const uint16_t desum = static_cast<uint16_t>(mc.command("bIsDesummoning")->handle);
                const uint16_t move_in = static_cast<uint16_t>(class_named("ANPCCharacter").command("CurrentMovementInput")->handle);
                if (mount_comp && hs.count(desum) && ours.flags < 0) {
                    ours.flags = now;
                    ours.flags_h = hs;
                } else if (mount_comp && ours.flags >= 0 && ours.off < 0 && hs.count(desum)) {
                    ours.off = now;
                    ours.off_h = hs;
                } else if (combat_comp && ours.combat_h.empty()) {
                    ours.combat_h = hs;
                } else if (is_actor && hs.count(move_in)) {
                    if (ours.run < 0) {
                        ours.run = now;
                        ours.run_h = hs;
                    } else if (ours.input_off < 0) {   // (the run's other updates carry the movement and its timestamp, not the input, which does not change)
                        ours.input_off = now;
                        ours.input_off_h = hs;
                    }
                }
            }
        }
    }
    std::set<uint16_t> r874_flags, r874_combat;
    const Seen* s874 = retail_seen.count(874) ? &retail_seen[874] : nullptr;
    CHECK(s874 && ours.flags_h == s874->flags && ours.combat_h == s874->combat && ours.off_h == s874->flags_off,
          "the flags update has the handles retail's has (bIsDesummoning and DissolveTimer; invulnerable and untargetable; bIsDesummoning off)");
    const auto show = [](const std::set<uint16_t>& hs) {
        std::string out;
        for (const uint16_t x : hs) out += std::to_string(x) + " ";
        return out;
    };
    if (s874) {
        std::printf("      run: ours %s| retail %s; input off: ours %s| retail %s\n", show(ours.run_h).c_str(), show(s874->run).c_str(), show(ours.input_off_h).c_str(), show(s874->input_off).c_str());
        std::printf("      times: flags %.3f run %.3f off %.3f input_off %.3f close %.3f\n", ours.flags, ours.run, ours.off, ours.input_off, ours.close);
    }
    CHECK(s874 && ours.run_h == s874->run && ours.input_off_h == s874->input_off, "the run's updates carry the handles retail's do (the movement, its timestamp, the input)");
    int lo[5] = {100000, 100000, 100000, 100000, 100000}, hi[5] = {0, 0, 0, 0, 0};
    for (const auto& [capture, s] : retail_seen) {
        const int v[5] = {s.flags_ms, s.run_ms, s.off_ms, s.input_off_ms, s.close_ms};
        for (int i = 0; i < 5; ++i) {
            lo[i] = std::min(lo[i], v[i]);
            hi[i] = std::max(hi[i], v[i]);
        }
    }
    const double mine[5] = {ours.flags, ours.run, ours.off, ours.input_off, ours.close};
    const char* what[5] = {"bIsDesummoning on", "the run", "bIsDesummoning off", "the input off", "the close"};
    for (int i = 0; i < 5; ++i) {
        const double slack = i == 0 || i == 1 ? 110 : 80;   // our updates go out with the view's pass and the test's 20 ms step
        CHECK(mine[i] >= 0 && mine[i] * 1000 >= lo[i] - slack && mine[i] * 1000 <= hi[i] + slack, (std::string(what[i]) + " comes within retail's range (" + std::to_string(lo[i]) + " to " + std::to_string(hi[i]) + " ms)").c_str());
    }
}
// (Cyb ~ C-O-A)

static void test_golden_refusal() {
    std::printf("golden: the refused summon of captures 876 and 870\n");
    for (const uint16_t capture : {876, 870}) {
        const cm::Block* item = retail(capture, "item");
        const cm::Block* end = retail(capture, "end");
        const cm::Block* callout = retail(capture, "callout");
        const cm::Block* off = cm::find(capture, "callout_off");
        if (!item || !end || !callout) continue;
        const auto ri = load_block(*item);
        const auto rc = load_block(*callout);
        const auto ri_item = ri ? ability_item_of(*ri) : std::nullopt;
        CHECK(ri_item && rc, (std::string("retail's item and callout of ") + std::to_string(capture) + " read").c_str());
        if (!ri_item || !rc) continue;
        Harness h(cyb_record());
        h.play();
        const Clock::time_point req = h.t;
        const size_t from = h.got.size();
        summon_with(h, kLockedSlot, 11);
        std::optional<repl::ElementState> shown;
        std::optional<repl::ElementState> our_callout;
        Saw saw;
        for (milliseconds done{0}; done < milliseconds(1200); done += milliseconds(20)) {
            h.step();
            if (!shown) shown = our_item(h, true);
        }
        for (size_t i = from; i < h.got.size(); ++i) {
            const fake::Delivered& d = h.got[i];
            if (d.channel != h.pc_ch || d.open || d.close) continue;
            const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
            if (!blocks) continue;
            for (const repl::BlockView& b : *blocks) {
                if (!b.header.is_actor) continue;
                BunchWriter w;
                w.write_bit_range(d.content.data(), b.payload_pos, b.payload_bits);
                auto st = repl::load_object_state(class_named("AoCPlayerControllerBP_C"), w.data(), b.payload_bits, b.header.has_rep_layout);
                if (!st) continue;
                const schema::NetField* f = st->schema->field("CalloutQueueReplication");
                const repl::FastArrayState* a = f ? st->arrays.find(f->index) : nullptr;
                if (a && !a->items().empty()) our_callout = a->items().back().values;
            }
        }
        CHECK(shown.has_value(), "worldd shows the item of the refused summon");
        if (shown) {
            const std::set<uint16_t> d = item_diff(*ri_item, *shown);
            CHECK(std::includes(std::set<uint16_t>({2, 15, 17}).begin(), std::set<uint16_t>({2, 15, 17}).end(), d.begin(), d.end()), (std::string("the refusal's item is retail's ") + std::to_string(capture) + " but for the instance, the counter and the instigator").c_str());
        }
        const schema::NetField* f = rc->schema->field("CalloutQueueReplication");
        const repl::FastArrayState* ra = f ? rc->arrays.find(f->index) : nullptr;
        CHECK(ra && !ra->items().empty() && our_callout.has_value(), "worldd sends a callout, and so did retail");
        if (ra && !ra->items().empty() && our_callout) {
            // every member of the callout but its key (25-28) and its creation time (21) is retail's
            const std::set<uint16_t> d = item_diff(ra->items().back().values, *our_callout);
            const std::set<uint16_t> allowed = {21, 25, 26, 27, 28};
            bool subset = true;
            for (const uint16_t x : d) subset = subset && allowed.count(x);
            std::string s;
            for (const uint16_t x : d) s += std::to_string(x) + " ";
            CHECK(subset, (std::string("the callout is retail's but for its key and creation time (differing: ") + s + ")").c_str());
        }
        // the times
        const double item_s = saw.ability_item;
        (void)item_s;
        (void)off;
        (void)end;
    }
    // the refusal's times against the two captures: the item at +95 and +116 ms, its end at +1.58 to +1.62 s, the callout off 3.01 s after it was shown
    Harness h(cyb_record());
    h.play();
    const Clock::time_point req = h.t;
    summon_with(h, kLockedSlot, 11);
    const Saw saw = watch(h, req, milliseconds(4500));
    const cm::Block* a = cm::find(876, "item");
    const cm::Block* b = cm::find(870, "item");
    const cm::Block* e1 = cm::find(876, "end");
    const cm::Block* e2 = cm::find(870, "end");
    CHECK(a && b && e1 && e2 && saw.ability_item * 1000 >= std::min(a->ms, b->ms) - 40 && saw.ability_item * 1000 <= std::max(a->ms, b->ms) + 40, "the item comes within retail's range (+95 to +116 ms)");
    CHECK(a && b && e1 && e2 && saw.item_end * 1000 >= std::min(e1->ms, e2->ms) - 50 && saw.item_end * 1000 <= std::max(e1->ms, e2->ms) + 50, "the end comes within retail's range (+1.58 to +1.62 s)");
}

// ---- the ability the client needs before it can use a mount item (stage 3 fix 1) --------------------------------------------------------------
//
//  The client's APlayerCharacter::ActivateItem (the exe's 0x6bf71c0) runs a SummonMount or CastAbility item through the AbilityComponent's
//  TriggerAbility (0x6bd2060), and that sends ServerTriggerAbility only when the ability is loaded there; one that is not shows "Ability not loaded"
//  (EAbilityFailureType::Unloaded 35, text at the exe's 0x6962ba0) and sends nothing.  The AbilityComponent loads what ReplicatedRecordsToLoad lists,
//  and retail lists the ability of every item in the bag: Summon_Mount is in the login capture 1198's records (92 of them, with 24 mounts in the bag)
//  and not in its KnownAbilities, and the record of a bought consumable is appended to the list in the packet after the storage event (capture 821).
//  The first in-game test (17:32 on 2026-10-08) had the client send ServerItemUsed for three mount uses and no trigger: worldd listed only the
//  class's abilities and the static tail.

/// The client's AbilityComponent lists, rebuilt from what the server sent on the pawn's channel (an update carries the new count and the elements
/// from the first changed one; the client keeps the elements an update does not name).
struct ClientLists {
    std::vector<int64_t> known, records;
    size_t updates = 0;   // the blocks that carried either list
};
// ~ C O A :: C_Y_B ~

static ClientLists client_lists(const Harness& h) {
    ClientLists out;
    const repl::Subobject* ab = h.s->bootstrap().pawn().find(h.s->bootstrap().setup().guids.ability);
    if (!ab) return out;
    const schema::ClassSchema& cls = *ab->obj.schema;
    for (const fake::Delivered& d : h.got) {
        if (d.channel != h.pawn_ch || d.open || d.close || d.content.bit_pos() <= 8) continue;
        const auto views = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
        if (!views) continue;
        for (const repl::BlockView& v : *views) {
            if (!(v.header.object == h.s->bootstrap().setup().guids.ability) || !v.header.has_rep_layout) continue;
            size_t p = v.payload_pos;
            const auto st = repl::read_rep_block(cls, d.content.data(), v.payload_pos + v.payload_bits, p);
            if (!st) continue;
            bool changed = false;
            const std::pair<const char*, std::vector<int64_t>*> lists[] = {{"KnownAbilities", &out.known}, {"ReplicatedRecordsToLoad", &out.records}};
            for (const auto& [name, list] : lists) {
                const repl::Value* val = st->get(cls, name);
                const repl::ArrayValue* a = val ? val->get_if<repl::ArrayValue>() : nullptr;
                if (!a) continue;
                list->resize(a->elements.size());
                for (size_t i = 0; i < a->elements.size(); ++i) {
                    const auto it = a->elements[i].values.find(1);
                    if (it != a->elements[i].values.end() && it->second.get_if<int64_t>()) (*list)[i] = *it->second.get_if<int64_t>();
                }
                changed = true;
            }
            if (changed) ++out.updates;
        }
    }
    return out;
}

static constexpr int64_t kSummonMountRecord = 108436548376185ll;

static size_t times_in(const std::vector<int64_t>& v, int64_t x) { return static_cast<size_t>(std::count(v.begin(), v.end(), x)); }

/// The retail client's use of a mount item: ServerItemUsed always; ServerTriggerAbility(Summon_Mount, the item) only when Summon_Mount is loaded in its
/// AbilityComponent (otherwise TriggerAbility fails with "Ability not loaded" and sends nothing).  False when the trigger was held back.
static bool use_mount_item_like_retail(Harness& h, int slot, int32_t client_instance = 7) {
    const svc::InventoryItem* item = h.s->inventory().at(Bag, slot);
    const std::string guid = item ? item->instance_guid : std::string();
    if (times_in(client_lists(h).records, kSummonMountRecord) == 0) {
        h.client_sends({h.client.actor(h.pawn_ch, item_used(3, slot, guid), true)});
        return false;
    }
    summon_with(h, slot, client_instance);
    return true;
}

static std::vector<svc::InventoryItem> kit_without_mounts() {
    std::vector<svc::InventoryItem> out;
    for (svc::InventoryItem& it : kit()) {
        if (it.item_record_id == g_mount_record || it.item_record_id == g_other_mount_record || it.item_record_id == g_aquatic_record || it.item_record_id == g_flying_record) continue;
        out.push_back(std::move(it));
    }
    return out;
}

static void test_item_abilities() {
    std::printf("the pawn's ability records for the mount items it carries: the client needs Summon_Mount loaded to send the trigger\n");
    std::vector<int64_t> with_mounts;
    {
        Harness h(cyb_record());
        h.play();
        const ClientLists l = client_lists(h);
        with_mounts = l.records;
        const std::vector<int64_t> tail(bs::static_known_tail().begin(), bs::static_known_tail().end());
        CHECK(times_in(l.records, kSummonMountRecord) == 1, "a character with mounts in the bag has Summon_Mount in ReplicatedRecordsToLoad, once");
        CHECK(times_in(l.known, kSummonMountRecord) == 0, "and not in KnownAbilities (retail's login: 92 records with it, 75 known without it)");
        CHECK(l.records.size() >= tail.size() && std::equal(tail.begin(), tail.end(), l.records.end() - static_cast<std::ptrdiff_t>(tail.size())),
              "the static tail still ends the records (the item's ability comes before it, as in retail's login)");
        CHECK(l.records == h.s->record_abilities() && l.known == h.s->known_abilities(), "the client's lists are the session's");
        // the whole point: the client sends the trigger with the item use, and the mount comes
        const Clock::time_point req = h.t;
        CHECK(use_mount_item_like_retail(h, kMountSlot), "the client has the ability loaded: it sends ServerTriggerAbility with ServerItemUsed");
        const Saw saw = watch(h, req, milliseconds(2600));
        CHECK(h.s->mount_summon_requests() == 1 && saw.open >= 0 && mount_count(h) == 1 && saw.ability_item >= 0,
              "the summon starts on the client's trigger: the ability item, then the mount's open");
    }
    {
        Harness h(cyb_record(kit_without_mounts()));
        h.play();
        const ClientLists l = client_lists(h);
        std::vector<int64_t> expected = with_mounts;
        expected.erase(std::remove(expected.begin(), expected.end(), kSummonMountRecord), expected.end());
        CHECK(!l.records.empty() && times_in(l.records, kSummonMountRecord) == 0, "a character with no mount item does not load Summon_Mount");
        CHECK(l.records == h.s->record_abilities() && l.records == expected, "and its records are the other character's but for that one");
    }
}

static void test_item_abilities_in_a_real_world() {
    std::printf("the same on the real content.db's world (its talents catalog: the production path of the join's lists)\n");
    Harness h(cyb_record(), /*real_world=*/true);
    if (!h.fx.world->ready() || !h.fx.world->talents()) {
        std::printf("  [skip] the real world is not ready or has no talents: %s\n", h.fx.world->content_problem().c_str());
        return;
    }
    h.play();
    const ClientLists l = client_lists(h);
    const std::vector<int64_t> tail(bs::static_known_tail().begin(), bs::static_known_tail().end());
    CHECK(times_in(l.records, kSummonMountRecord) == 1 && times_in(l.known, kSummonMountRecord) == 0, "Summon_Mount is in the records once and in no KnownAbilities");
    CHECK(l.records.size() >= tail.size() && std::equal(tail.begin(), tail.end(), l.records.end() - static_cast<std::ptrdiff_t>(tail.size())), "before the static tail");
    const Clock::time_point req = h.t;
    CHECK(use_mount_item_like_retail(h, kMountSlot), "the client sends the trigger with the item use");
    const Saw saw = watch(h, req, milliseconds(2600));
    CHECK(saw.open >= 0 && mount_count(h) == 1, "and the mount is summoned");
}
// ~ CoA - C:Y:B ~

/// A catalog that knows no record: the grant routes everything to the Backpack (Gathering stage 3: grant() takes the catalog).
struct NoItemCatalog : items::ItemCatalog {
    std::optional<items::ItemInfo> info(int64_t) const override { return std::nullopt; }
};

static void test_item_ability_arrives_with_the_item() {
    std::printf("a mount item that reaches the bag during the session brings its ability record right after it (retail 821: the record is appended)\n");
    Harness h(cyb_record(kit_without_mounts()));
    h.play();
    const ClientLists before = client_lists(h);
    CHECK(times_in(before.records, kSummonMountRecord) == 0, "the bag holds no mount: no Summon_Mount");
    // a mount item is bought, looted or granted: the inventory takes it (these tests have no vendor; the grant is the inventory's own)
    items::Inventory& inv = const_cast<items::Inventory&>(h.s->inventory());
    svc::InventoryItem fresh = make_item(Bag, 0, g_mount_record, 900);
    fresh.slot = inv.first_free(Bag);
    const int slot = fresh.slot;
    CHECK(inv.grant({fresh}, NoItemCatalog()).ok, "the mount item is granted into the bag");
    h.run(milliseconds(300));
    const ClientLists after = client_lists(h);
    CHECK(times_in(after.records, kSummonMountRecord) == 1 && after.records.size() == before.records.size() + 1 && after.records.back() == kSummonMountRecord,
          "Summon_Mount is appended to the client's records, once");
    CHECK(after.updates == before.updates + 1 && after.known == before.known, "in one update, and KnownAbilities does not change");
    CHECK(after.records == h.s->record_abilities(), "the client's records are the session's");
    // a second mount item brings nothing new, and the lists stay as they are
    svc::InventoryItem second = make_item(Bag, 0, g_other_mount_record, 901);
    second.slot = inv.first_free(Bag);
    CHECK(inv.grant({second}, NoItemCatalog()).ok, "a second mount item is granted");
    h.run(milliseconds(300));
    const ClientLists again = client_lists(h);
    CHECK(again.updates == after.updates && again.records == after.records, "a second mount item adds no record and sends no update");
    // the item can be used now: the client sends the trigger and the mount comes
    const Clock::time_point req = h.t;
    CHECK(use_mount_item_like_retail(h, slot), "the client sends ServerTriggerAbility with the item use");
    const Saw saw = watch(h, req, milliseconds(2600));
    CHECK(saw.open >= 0 && mount_count(h) == 1 && h.s->mount_summon_requests() == 1, "and the mount is summoned");
}

/// The mount items of the data the bag holds (read before any character is made: the kit names them).
static void find_mount_records() {
    std::string error;
    auto real = content::ContentDb::open(real_db_path(), error);
    const auto data = real ? real->mount_data(error) : std::nullopt;
    if (!data) {
        std::printf("  [FAIL] the real content.db has no mount tables (%s): %s\n", real_db_path().c_str(), error.c_str());
        std::exit(2);
    }
    for (const content::MountRow& r : data->mounts) {
        if (r.item_name == "Mount_Grounded_TerrorBird_0") g_mount_record = r.item;
        if (r.item_name == "Mount_Grounded_Horse_0") g_other_mount_record = r.item;
        if (r.kind == "aquatic" && !g_aquatic_record) g_aquatic_record = r.item;
        if (r.kind == "flying" && !g_flying_record) g_flying_record = r.item;
    }
}

int main() {
    spdlog::set_level(spdlog::level::off);
    find_mount_records();
    test_summon_and_desummon();
    test_refusals_and_hostile();
    test_cancel_by_damage();
    test_world();
    test_open_on_the_wire();
    test_golden_summon();
    test_golden_desummon();
    test_golden_refusal();
    test_item_abilities();
    test_item_ability_arrives_with_the_item();
    test_item_abilities_in_a_real_world();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
