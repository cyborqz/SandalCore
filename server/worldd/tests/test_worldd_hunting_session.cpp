// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_hunting_session.cpp  (Gathering stage 3, part "Hunting")
//
//  The hunt of a creature on connections: sessions with fake clients (the harness of test_worldd_foliage_session) on a PlayerWorld over the real content.db (AOC_CONTENT_DB, else
//  dist/content/content.db of the working tree; "[skip]" without it) with the combat world and the aftermath on the nav fixture's ground, the huntable creatures made as the area makes
//  them (spawns/generic_npc.h) a few metres from the player.
//
//  a hunt           the huntable is ALIVE when the player interacts with it (no kill before it, as the five captured hunts start): the pawn's ChannelingData (the creature, the Bow, the clock),
//                   its status 0x08000001 and a heartbeat counter that goes up by 2 about every 67 ms, 15 s at rating 0, then the result frame in capture 1457's order (the Bow's wear, the storage
//                   events into Resource0, ChannelingData cleared, the XPTracker with Hunter XP) and the done update (phase 4, CanInputActivate [false], nobody channeling, the creature dead for its
//                   clients); it is gone 4 s later; a second interaction in between is refused.  A creature that was killed some other way is hunted as well (test_refusals' kill)
//  the refusals     a Novice with an Apprentice kind, no Bow, 12 m away, a plain creature's corpse still opens its loot; each logged ("[hunt]") and counted, nothing sent
//  the cancels      the client's ServerInterruptChannel, a step of 70 cm, a death, a disconnect: no reward, the creature is alive as it was and can be hunted again at once
//  exploits         two players on one creature, a flood of 400 interactions, a full resource bag (the pile and the callout), a kind that gives no item (the experience only)
// ============================================================================
#include "capture_hunting.h"
#include "capture_gather.h"
#include "content_db_fixture.h"
#include "cyb_appearance.h"
#include "fake_client.h"
#include "nav_fixture.h"
#include "bootstrap/character_setup.h"
#include "combat/aftermath.h"
#include "combat/combat_world.h"
#include "combat/hit_resolver.h"
#include "combat/npc_replicator.h"
#include "gather/gathering.h"
#include "gather/node_wire.h"
#include "npc/motion_table.h"
#include "spawns/generic_npc.h"
#include "items/inventory.h"
#include "net/endpoint.h"
#include "protocol/repl/block_reader.h"
#include "protocol/repl/fname.h"
#include "protocol/repl/move_packed.h"
#include "protocol/wire/packet_reader.h"
#include "protocol/repl/item_rpc.h"
#include "protocol/repl/leveling.h"
#include "protocol/repl/rep_block.h"
#include "protocol/repl/storage_rpc.h"
#include "protocol/repl/vendor_rpc.h"
#include "protocol/schema/generated/index.h"
#include "session/player_session.h"
#include "spawns/captured_spawns.h"
#include "world/actor_registry.h"

#include <spdlog/sinks/ostream_sink.h>
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
#include <sstream>
#include <string>
#include <vector>
// built by C-O-A with Cyb

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

static const std::string kMachine = "DESKTOP-EXAMPLE1-US-0123456789ABCDEF0123456789ABCDEF";
static const Clock::time_point t0{seconds(5000)};
static constexpr uint32_t kServerId = 60;
static constexpr uint32_t kRandomizer = 0x6ee87ee4u;
static const ItemContainer Bag = ItemContainer::Backpack;
static const ItemContainer Worn = ItemContainer::Equipment;
static const ItemContainer R0 = ItemContainer::Resource0;   // Gathering stage 3: a harvest lands in the resource bag, not in the Backpack

namespace id {
inline constexpr int64_t kMining = 108752769996425ll, kLumberjacking = 108752769996426ll, kHerbalism = 108752770061963ll;
inline constexpr int64_t kLtHerbalist = 109460724995707ll;
inline constexpr int64_t kT1Pickaxe = 6064629375589548039ll, kT3Pickaxe = 6064629376467927053ll;
inline constexpr int64_t kDaffodil = 6064632138835886135ll, kMoonbell = 6064631483095515243ll, kRividium = 6064631669497200654ll, kHalcyonite = 6064631483054948432ll;
inline constexpr int64_t kLimestone = 6064631483017789494ll, kOakLarge2 = 6064632138726178834ll;
inline constexpr int64_t kDaffodilItem = 6064629184431849472ll;   // Resource_Raw_Plant_Daffodil
}  // namespace id

static const char* db_path() {
    const char* env = std::getenv("AOC_CONTENT_DB");
    if (env && *env) return env;
#ifdef GATHER_CONTENT_DB_PATH
    if (fs::exists(GATHER_CONTENT_DB_PATH)) return GATHER_CONTENT_DB_PATH;
#endif
    return nullptr;
}

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

static svc::InventoryItem make_item(ItemContainer c, int slot, int64_t record, uint32_t n, int32_t quantity = 1, float durability = 11) {
    svc::InventoryItem it;
    it.container = c;
    it.slot = slot;
    it.item_record_id = record;
    it.instance_guid = guid_n(n);
    it.quantity = quantity;
    it.quality = 1;
    it.durability = durability;
    it.created_at_ms = 1'771'299'474'000ll;
    return it;
}

/// A Bard with the worn kit of the fixture (its three tools are the real FTUE tools) and, in slots 26, 29 and 33, tools of the real file: a T1 pickaxe, axe and sickle with 150.
static svc::Character record_of(const char* id_hex, const char* name, bool tools = true) {
    svc::Character c;
    c.id = id_hex;
    c.account_id = 3;
    c.name = name;
    c.race_id = 0;
    c.gender_id = 0;
    c.class_id = 2;
    c.archetype_id = 17747;
    c.class_name = "Bard";
    c.appearance_json = cyb::kAppearanceJsonb;
    c.level = 1;
    const std::pair<int, int64_t> worn[] = {{1, fi::kHelmet}, {2, fi::kChest}, {5, fi::kLegs}, {6, fi::kFeet}, {15, fi::kScepter}, {16, fi::kFocus}, {17, fi::kBook}, {23, fi::kBag}};
    for (const auto& [slot, record] : worn) c.items.push_back(make_item(Worn, slot, record, 100 + static_cast<uint32_t>(slot)));
    if (tools) {
        c.items.push_back(make_item(Worn, 26, id::kT1Pickaxe, 126, 1, 150));
        c.items.push_back(make_item(Worn, 29, 6064629375589613576ll, 129, 1, 150));   // Gear_Artisan_Lumberjacking_Tool_Axe_T1_0
        c.items.push_back(make_item(Worn, 33, 6064629375589482502ll, 133, 1, 150));   // Gear_Artisan_Herbalism_Tool_Sickle_T1_0
        c.items.push_back(make_item(Worn, 38, 6064630140303376483ll, 138, 1, 150));   // Gear_Artisan_Hunting_Tool_Bow_T1_0
    }
    return c;
}

static bs::LoginIdentity identity() { return {kMachine, kMachine}; }
static double world_time_at(Clock::time_point t) { return std::chrono::duration<double>(t - t0).count(); }

struct RecordingSink : PositionSink {
    struct Progress {
        std::string id;
        svc::Progress progress;
    };
    std::vector<std::pair<std::string, std::vector<svc::InventoryItem>>> inventories;
    std::vector<Progress> progress;
    void save(const std::string&, const svc::Position&) override {}
    void save_inventory(const std::string& id, std::vector<svc::InventoryItem> items) override { inventories.emplace_back(id, std::move(items)); }
    void save_progress(const std::string& id, svc::Progress p) override { progress.push_back({id, std::move(p)}); }
};

// ---- the world and its players -------------------------------------------------------------------------------------------------------

static constexpr double kCx = bs::kDefaultSpawn.x, kCy = bs::kDefaultSpawn.y;

struct Rig;
// [C Y B :: CoA]

struct Player {
    Rig& rig;
    svc::Character record;
    Connection conn{ConnectionConfig{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870}, t0};
    fake::Client client{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870};
    std::unique_ptr<PlayerSession> s;
    std::vector<fake::Delivered> got;
    uint32_t pawn_ch = 0, pc_ch = 0;
    std::string error;

    Player(Rig& r, svc::Character rec) : rig(r), record(std::move(rec)) {}
    void join();
    void flush();
    void client_sends(std::vector<fake::Bunch> bunches);
    void step_session();
    void request(const BunchWriter& rpc, uint32_t channel);
    /// Sends a request and lets the world run 60 ms (not to quiet: a channel's heartbeats never let the world go quiet).
    void send_now(const BunchWriter& rpc, uint32_t channel);
};

struct Rig {
    std::optional<content::ContentDb> db;
    aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
    std::unique_ptr<PlayerWorld> world;
    std::optional<spawns::CapturedData> captured;
    std::optional<spawns::GenericNpcs> generic;
    content::PopulationData pop;
    RecordingSink sink;
    Clock::time_point t = t0;
    std::vector<std::unique_ptr<Player>> players;
    uint64_t seed = 1;

    bool open(const char* path) {
        std::string error;
        db = content::ContentDb::open(path, error);
        if (!db) {
            std::printf("  [FAIL] the content.db does not open: %s\n", error.c_str());
            return false;
        }
        world = std::make_unique<PlayerWorld>(&*db, "", bs::GameStateMode::Tier2, kServerId, guids);
        world->set_position_sink(&sink);
        if (!world->ready()) {
            std::printf("  [FAIL] the world is not ready: %s\n", world->content_problem().c_str());
            return false;
        }
        // the NPCs' ground, the combat world and the aftermath (adopt_ground starts them), as the ability tests have them
        if (!world->load_blueprints(NPC_MOTION_PATH, error)) {
            std::printf("  [FAIL] the Blueprint table: %s\n", error.c_str());
            return false;
        }
        auto nav = nav::AreaNav::from_file(navfix::build_file(), &error);
        if (!nav || !world->adopt_ground(std::move(nav), error)) {
            std::printf("  [FAIL] the ground: %s\n", error.c_str());
            return false;
        }
        auto c = spawns::CapturedData::load(error);
        if (!c) {
            std::printf("  [FAIL] the captured spawns do not load: %s\n", error.c_str());
            return false;
        }
        captured = std::move(*c);
        auto p = db->population_data(error);
        if (!p) {
            std::printf("  [FAIL] the population does not read: %s\n", error.c_str());
            return false;
        }
        pop = std::move(*p);
        gather::Config cfg;
        if (!world->start_gathering(cfg, error)) {
            std::printf("  [FAIL] gathering does not start: %s\n", error.c_str());
            return false;
        }
        spawns::GenericConfig gc;
        for (const auto& [def, kind] : g().hunting()) gc.hunting_defs.insert(def);
        generic.emplace(*captured, &world->blueprints(), gc);
        return world->combat() != nullptr && world->aftermath() != nullptr;
    }
    gather::Gathering& g() { return *world->gathering(); }
    gather::NodeBoard& board() { return g().board(); }
    double now() const { return world_time_at(t); }
// ~ CoA & C.Y.B ~

    const content::NpcAssetRow* asset(const std::string& name) const {
        for (const auto& [guid, a] : pop.assets) {
            if (a.name == name) return &a;
        }
        return nullptr;
    }
    /// A creature of the asset `name` standing (dx, dy) from the login spot; a huntable asset gets its eighth block.  0 when it cannot be made.
    uint64_t spawn(const std::string& name, double dx, double dy, int level = 5) {
        const content::NpcAssetRow* a = asset(name);
        const spawns::OpenTemplate* tpl = a ? generic->template_for(*a) : nullptr;
        if (!tpl) return 0;
        spawns::InstanceParams p;
        p.x = kCx + dx;
        p.y = kCy + dy;
        p.z = bs::kDefaultSpawn.z;
        p.level = level;
        p.server_id = kServerId;
        p.seed = 0x4F0D5ull + seed++;
        std::string why;
        auto w = spawns::instantiate(*tpl, [this] { return world->mint_guid(); }, p, why);
        if (!w) return 0;
        w->npc.name = a->name;
        w->npc.archetype = a->archetype_path;
        w->npc.level = level;
        w->npc.home_x = p.x;
        w->npc.home_y = p.y;
        w->npc.home_z = p.z;
        w->npc.aggro_radius_cm = 1;
        const uint64_t id = w->id;
        if (!world->registry().add(std::move(*w))) return 0;
        npc::NpcMotionSetup setup;
        setup.ambient = npc::Ambient::Stationary;
        setup.foot_offset = 88.0;
        world->npcs()->add(id, setup);
        return id;
    }
    uint64_t component_of(uint64_t npc) {
        const world::WorldActor* a = world->registry().find(npc);
        return a ? a->npc.interact_component : 0;
    }
    /// The player's hit kills the creature (a hit of -1e9 Health, as the aftermath tests do).
    void kill(uint64_t npc, uint64_t by_pawn) {
        combat::Resolved k;
        k.mods.push_back({1, content::kGuidHealth, -1.0e9});
        world->combat()->apply_hit_to_npc(npc, by_pawn, k, now());
    }

    Player& add_player(svc::Character rec) {
        players.push_back(std::make_unique<Player>(*this, std::move(rec)));
        players.back()->join();
        return *players.back();
    }
    void step(milliseconds d = milliseconds(20)) {
        t += d;
        world->tick_world(now());
        for (auto& p : players) {
            if (p->s) p->step_session();
        }
    }
    void run(milliseconds total) {
        for (milliseconds done{0}; done < total; done += milliseconds(20)) step();
    }
    void settle() {
        for (int quiet = 0, i = 0; quiet < 5 && i < 600; ++i) {
            size_t before = 0;
            for (auto& p : players) before += p->got.size();
            step();
            size_t after = 0;
            bool busy = false;
            for (auto& p : players) {
                after += p->got.size();
                busy = busy || (p->s && (p->s->join_pending() != 0 || p->s->replies_pending() != 0));
            }
            quiet = (after == before && !busy) ? quiet + 1 : 0;
        }
    }
};

// [C.O.A :: C:Y:B]

void Player::join() {
    s = PlayerSession::join(conn, *rig.world, record, identity(), world_time_at(rig.t), error);
    if (!s) {
        std::printf("  [FAIL] the session does not join: %s\n", error.c_str());
        std::exit(2);
    }
    pawn_ch = s->channel_of(bs::Role::Pawn);
    pc_ch = s->channel_of(bs::Role::PlayerController);
}
void Player::flush() {
    for (auto& pkt : conn.flush(rig.t)) {
        const auto r = client.receive(pkt);
        for (const auto& d : r.delivered) got.push_back(d);
    }
}
void Player::client_sends(std::vector<fake::Bunch> bunches) {
    const auto pkt = client.packet(std::move(bunches));
    const std::vector<InBunch> in = conn.receive(pkt, rig.t);
    s->take_acks(conn);
    for (const InBunch& b : in) s->on_bunch(conn, b, rig.t);
    s->end_of_datagram(conn);
}
void Player::step_session() {
    s->tick(conn, rig.t);
    flush();
    s->take_acks(conn);
    client_sends({});
}
void Player::request(const BunchWriter& rpc, uint32_t channel) {
    client_sends({client.actor(channel, rpc, true)});
    rig.settle();
}
void Player::send_now(const BunchWriter& rpc, uint32_t channel) {
    client_sends({client.actor(channel, rpc, true)});
    rig.run(milliseconds(60));
}

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

static BunchWriter interact(uint64_t component_object_id) {
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    const schema::RpcSchema* rpc = pawn.rpc("ServerInteractWithInteractableComponent");
    repl::InteractRequest r;
    r.interactable.ObjectId = component_object_id;
    r.interactable.ServerId = kServerId;
    r.interactable.Randomizer = kRandomizer;
    r.counter = 1;
    const auto params = repl::interact_params(*rpc, r);
    return params ? actor_rpc(pawn, "ServerInteractWithInteractableComponent", *params) : BunchWriter();
}

static BunchWriter interrupt_channel() { return actor_rpc(class_named("PlayerPawn_C"), "ServerInterruptChannel", {}); }

/// What the session logged (info and above): the test reads the lines the user's test would.
static std::ostringstream g_log;
static size_t count_in_log(const char* what) {
    const std::string text = g_log.str();
    size_t n = 0;
    for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
    return n;
}

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
// built with Cyb and C-O-A

// ---- reading what the server sent -------------------------------------------------------------------------------------------------------

/// The PlayerController's decoded calls among the chains `got[from..]`, in order.
static std::vector<repl::RpcCall> pc_calls_since(const Player& p, size_t from) {
    std::vector<repl::RpcCall> out;
    for (size_t i = from; i < p.got.size(); ++i) {
        if (p.got[i].channel != p.pc_ch || p.got[i].open || p.got[i].close) continue;
        const auto blocks = repl::read_content_blocks(p.got[i].content.data(), p.got[i].content.bit_pos(), 0);
        if (!blocks) continue;
        for (const auto& b : *blocks) {
            if (!b.header.is_actor || b.header.has_rep_layout) continue;
            size_t pos = b.payload_pos;
            auto calls = repl::read_rpc_fields(class_named("AoCPlayerControllerBP_C"), p.got[i].content.data(), b.payload_pos + b.payload_bits, pos);
            if (calls) {
                for (auto& c : *calls) out.push_back(std::move(c));
            }
        }
    }
    return out;
}

static std::vector<std::string> call_names(const std::vector<repl::RpcCall>& calls) {
    std::vector<std::string> out;
    for (const auto& c : calls) out.push_back(c.rpc ? c.rpc->name : "?");
    return out;
}

static size_t count_calls(const std::vector<repl::RpcCall>& calls, const char* name) {
    return static_cast<size_t>(std::count_if(calls.begin(), calls.end(), [&](const repl::RpcCall& c) { return c.rpc && std::string(c.rpc->name) == name; }));
}

/// What the client saw of a node: the statuses of its InputEffectStatuses[0] in the update chains, whether bGathered was set, the chains of its channel.
struct NodeSeen {
    std::vector<uint32_t> statuses;
    bool                  gathered = false;
    size_t                chains = 0;
    /// Every CanInputActivate the client was told, in order: one entry per array element, the bool the update writes (nullopt: the element is left as the client has it).  The client
    /// indexes the array without a bound, so an empty one crashed it (test 1 of stage 2): none of these may be empty.
    std::vector<std::vector<std::optional<bool>>> can_input;
};

static NodeSeen node_seen(const Player& p, uint64_t node, size_t from) {
    NodeSeen out;
    const uint32_t ch = p.s->actor_view() ? p.s->actor_view()->channel_of(node) : 0;
    const world::WorldActor* a = p.rig.world->registry().find(node);
    if (ch == 0 || !a) return out;
    const schema::ClassSchema& inter = class_named("UInteractableComponent");
    const schema::ClassSchema& actor_class = class_named("AGatherableActor");
    for (size_t i = from; i < p.got.size(); ++i) {
        if (p.got[i].channel != ch || p.got[i].open || p.got[i].close) continue;
        const auto blocks = repl::read_content_blocks(p.got[i].content.data(), p.got[i].content.bit_pos(), 0);
        if (!blocks) continue;
        ++out.chains;
        for (const auto& b : *blocks) {
            size_t pos = b.payload_pos;
            const size_t end = b.payload_pos + b.payload_bits;
            if (b.header.is_actor && b.header.has_rep_layout) {
                const auto st = repl::read_rep_block(actor_class, p.got[i].content.data(), end, pos);
                const repl::Value* v = st ? st->get(actor_class, "bGathered") : nullptr;
                if (v && v->get_if<bool>() && *v->get_if<bool>()) out.gathered = true;
            } else if (!b.header.is_actor && b.header.object.ObjectId == a->npc.interact_component && b.header.has_rep_layout) {
                const auto st = repl::read_rep_block(inter, p.got[i].content.data(), end, pos);
                const repl::Value* v = st ? st->get(inter, "InputEffectStatuses") : nullptr;
                const auto* arr = v ? v->get_if<repl::ArrayValue>() : nullptr;
                if (arr && !arr->elements.empty()) {
                    const auto it = arr->elements[0].values.find(1);
                    if (it != arr->elements[0].values.end() && it->second.get_if<uint64_t>()) out.statuses.push_back(static_cast<uint32_t>(*it->second.get_if<uint64_t>()));
                }
                const repl::Value* can = st ? st->get(inter, "CanInputActivate") : nullptr;
                const auto* can_arr = can ? can->get_if<repl::ArrayValue>() : nullptr;
                if (can_arr) {
                    std::vector<std::optional<bool>> entries;
                    for (const repl::ElementState& e : can_arr->elements) {
                        const auto w = e.values.find(1);
                        const bool* x = w == e.values.end() ? nullptr : w->second.get_if<bool>();
                        entries.push_back(x ? std::optional<bool>(*x) : std::nullopt);
                    }
                    out.can_input.push_back(std::move(entries));
                }
            }
        }
    }
    return out;
}

/// The pawn's InteractInfo.ChannelingData the client was told in the chains `got[from..]`, in order: (the index of the chain in `got`, the value).  The retail server sets it with the
/// node's wake and clears it with the result; without it the client plays no gather animation, shows no bar and never sends its own cancel (test 1 of stage 2).
static std::vector<std::pair<size_t, repl::ChannelingData>> channeling_seen(const Player& p, size_t from) {
    std::vector<std::pair<size_t, repl::ChannelingData>> out;
    const schema::ClassSchema& info = class_named("UCharacterSecondaryInfo");
    for (size_t i = from; i < p.got.size(); ++i) {
        if (p.got[i].channel != p.pawn_ch || p.got[i].open || p.got[i].close) continue;
        const auto blocks = repl::read_content_blocks(p.got[i].content.data(), p.got[i].content.bit_pos(), 0);
        if (!blocks) continue;
        for (const auto& b : *blocks) {
            if (b.header.is_actor || !b.header.has_rep_layout) continue;
            size_t pos = b.payload_pos;
            const auto st = repl::read_rep_block(info, p.got[i].content.data(), b.payload_pos + b.payload_bits, pos);
            const repl::Value* v = st ? st->get(info, "ChannelingData") : nullptr;
            if (v && v->get_if<repl::ChannelingData>()) out.emplace_back(i, *v->get_if<repl::ChannelingData>());
        }
    }
    return out;
}

/// The index in `got` of each chain of the PlayerController that carries an RPC, with the RPC's name, in order; and the index of the first chain with an XPTracker block.
static std::vector<std::pair<size_t, std::string>> pc_chain_calls(const Player& p, size_t from) {
    std::vector<std::pair<size_t, std::string>> out;
    for (size_t i = from; i < p.got.size(); ++i) {
        if (p.got[i].channel != p.pc_ch || p.got[i].open || p.got[i].close) continue;
        const auto blocks = repl::read_content_blocks(p.got[i].content.data(), p.got[i].content.bit_pos(), 0);
        if (!blocks) continue;
        for (const auto& b : *blocks) {
            if (!b.header.is_actor || b.header.has_rep_layout) continue;
            size_t pos = b.payload_pos;
            auto calls = repl::read_rpc_fields(class_named("AoCPlayerControllerBP_C"), p.got[i].content.data(), b.payload_pos + b.payload_bits, pos);
            if (calls) {
                for (const auto& c : *calls) out.emplace_back(i, c.rpc ? c.rpc->name : "?");
            }
        }
    }
    return out;
}

static size_t xp_chain_since(const Player& p, size_t from) {
    const bs::SessionGuids& g = p.s->bootstrap().setup().guids;
    for (size_t i = from; i < p.got.size(); ++i) {
        if (p.got[i].channel != p.pc_ch || p.got[i].open || p.got[i].close) continue;
        const auto blocks = repl::read_content_blocks(p.got[i].content.data(), p.got[i].content.bit_pos(), 0);
        if (!blocks) continue;
        for (const auto& b : *blocks) {
            if (!b.header.is_actor && b.header.object == g.xp_tracker) return i;
        }
    }
    return static_cast<size_t>(-1);
}

/// True when every CanInputActivate the client was told has its four entries (never an empty array), and `last_first` is the bool the last one writes for the first entry.
static bool can_input_ok(const NodeSeen& n, bool last_first) {
    for (const auto& c : n.can_input) {
        if (c.size() != 4) return false;
    }
    return !n.can_input.empty() && n.can_input.back()[0] && *n.can_input.back()[0] == last_first;
}
// [CoA - C Y B]

/// The XPTracker's experience and debt as the client holds them after everything it was sent.
struct Tracker {
    repl::LevelingState state;
    size_t updates = 0;
    void absorb(const Player& p, size_t from) {
        const bs::SessionGuids& g = p.s->bootstrap().setup().guids;
        for (size_t i = from; i < p.got.size(); ++i) {
            if (p.got[i].channel != p.pc_ch || p.got[i].open || p.got[i].close) continue;
            const auto blocks = repl::read_content_blocks(p.got[i].content.data(), p.got[i].content.bit_pos(), 0);
            if (!blocks) continue;
            for (const auto& b : *blocks) {
                if (b.header.is_actor || !(b.header.object == g.xp_tracker)) continue;
                size_t pos = b.payload_pos;
                const size_t end = b.payload_pos + b.payload_bits;
                const schema::ClassSchema& cls = state.schema();
                if (b.header.has_rep_layout && !repl::read_rep_block(cls, p.got[i].content.data(), end, pos)) continue;
                if (pos >= end) continue;
                const auto fields = repl::read_fast_array_fields(cls, p.got[i].content.data(), end, pos);
                if (!fields) continue;
                for (const repl::FastArrayField& f : *fields) state.apply(f);
                ++updates;
            }
        }
    }
};

static repl::DesignDataId leveling_id(int64_t guid) {
    repl::DesignDataId d;
    d.guid = guid;
    d.type_id = content::kTypeLevelingTypeDef;
    return d;
}

/// Everything after the join and the phase-in: the actors around the player are opened and the player may interact.
static void play(Player& p) {
    p.rig.settle();
    p.request(actor_rpc(class_named("PlayerPawn_C"), "ServerClientIsReadyToPhaseIn", {}), p.pawn_ch);
    p.rig.run(seconds(2));
    p.rig.settle();
}

/// Runs the world until `done()` or `limit` passes.
static bool run_until(Rig& rig, const std::function<bool()>& done, milliseconds limit = seconds(30)) {
    for (milliseconds t{0}; t < limit; t += milliseconds(20)) {
        if (done()) return true;
        rig.step();
    }
    return done();
}



// ---- the tests of the hunt ------------------------------------------------------------------------------------------------------------------

static const char* g_path = nullptr;

namespace hid {
inline constexpr int64_t kHunting = 108752770061965ll;
inline constexpr int64_t kBow = 6064630140303376483ll;   // Gear_Artisan_Hunting_Tool_Bow_T1_0
inline constexpr int64_t kCoal = 109162169652923ll;      // Resource_Raw_Stone_Coal: 1 x 1, stacks to 20 (a filler of the Sanctus bag's 24 cells)
}  // namespace hid

static std::vector<std::string> pc_call_names_since(const Player& p, size_t from) {
    std::vector<std::string> out;
    for (const auto& [chain, name] : pc_chain_calls(p, from)) out.push_back(name);
    return out;
}

static size_t count_names(const std::vector<std::string>& v, const char* name) { return static_cast<size_t>(std::count(v.begin(), v.end(), name)); }

/// What the client saw of a hunted corpse's interactable: the status words of InputEffectStatuses[0], every CanInputActivate told (one entry per array element) and the four interaction states
/// of the last CurrentInputStates, in the chains `got[from..]`.
struct CorpseSeen {
    std::vector<uint32_t> statuses;
    std::vector<std::vector<std::optional<bool>>> can_input;
    std::vector<uint64_t> last_states;
    size_t chains = 0;
};

static CorpseSeen corpse_seen(const Player& p, uint64_t comp_id, uint32_t channel, size_t from) {
    CorpseSeen out;
    if (channel == 0) return out;
    const schema::ClassSchema& inter = class_named("UInteractableComponent");
    for (size_t i = from; i < p.got.size(); ++i) {
        if (p.got[i].channel != channel || p.got[i].open || p.got[i].close) continue;
        const auto blocks = repl::read_content_blocks(p.got[i].content.data(), p.got[i].content.bit_pos(), 0);
        if (!blocks) continue;
        ++out.chains;
        for (const auto& b : *blocks) {
            if (b.header.is_actor || b.header.object.ObjectId != comp_id || !b.header.has_rep_layout) continue;
            size_t pos = b.payload_pos;
            const auto st = repl::read_rep_block(inter, p.got[i].content.data(), b.payload_pos + b.payload_bits, pos);
            if (!st) continue;
            const repl::Value* v = st->get(inter, "InputEffectStatuses");
            const auto* arr = v ? v->get_if<repl::ArrayValue>() : nullptr;
            if (arr && !arr->elements.empty()) {
                const auto it = arr->elements[0].values.find(1);
                if (it != arr->elements[0].values.end() && it->second.get_if<uint64_t>()) out.statuses.push_back(static_cast<uint32_t>(*it->second.get_if<uint64_t>()));
            }
            const repl::Value* can = st->get(inter, "CanInputActivate");
            const auto* can_arr = can ? can->get_if<repl::ArrayValue>() : nullptr;
            if (can_arr) {
                std::vector<std::optional<bool>> entries;
                for (const repl::ElementState& e : can_arr->elements) {
                    const auto w = e.values.find(1);
                    const bool* x = w == e.values.end() ? nullptr : w->second.get_if<bool>();
                    entries.push_back(x ? std::optional<bool>(*x) : std::nullopt);
                }
                out.can_input.push_back(std::move(entries));
            }
            const repl::Value* states = st->get(inter, "CurrentInputStates");
            const auto* sa = states ? states->get_if<repl::ArrayValue>() : nullptr;
            if (sa) {
                std::vector<uint64_t> s;
                for (const repl::ElementState& e : sa->elements) {
                    const auto w = e.values.find(1);
                    s.push_back(w != e.values.end() && w->second.get_if<uint64_t>() ? *w->second.get_if<uint64_t>() : 99);
                }
                out.last_states = std::move(s);
            }
        }
    }
    return out;
}

static svc::Character record_here(const char* id_hex, const char* name) {
    svc::Character c = record_of(id_hex, name);
    c.has_position = true;
    c.position.x = kCx;
    c.position.y = kCy;
    c.position.z = bs::kDefaultSpawn.z;
    return c;
}

static uint64_t pawn_id(const Player& p) { return p.s->bootstrap().setup().guids.pawn.ObjectId; }

/// The player, a huntable Wolf 150 cm away and the clients' view of both.
struct HuntRig {
    Rig rig;
    Player* p = nullptr;
    uint64_t wolf = 0;
    bool open(const char* kind = "Hunting_Wolf") {
        if (!rig.open(g_path)) return false;
        p = &rig.add_player(record_here("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb"));
        wolf = rig.spawn(kind, 150, 0);
        if (!wolf) {
            std::printf("  [FAIL] no %s could be made\n", kind);
            return false;
        }
        play(*p);
        return true;
    }
    void interact_with(Player& who, uint64_t npc) { who.send_now(interact(rig.component_of(npc)), who.pawn_ch); }
    void kill_it(uint64_t npc) {
        rig.kill(npc, pawn_id(*p));
        rig.run(milliseconds(500));
    }
    /// The creature stands for half a second (the client has it open, the world has ticked): it is hunted alive.
    void settle() { rig.run(milliseconds(500)); }
};
// -- C O A x C.Y.B --

/// Whether the creature is dead for its clients: its CombatInfo's bIsDead.
static bool dead_for_clients(const world::WorldActor* a) {
    if (!a) return false;
    for (const aoc::protocol::repl::Subobject& s : a->actor.subobjects) {
        if (!s.obj.schema || std::string(s.obj.schema->name) != "UNPCCombatInfo") continue;
        const aoc::protocol::repl::Value* v = s.obj.state.get(*s.obj.schema, "bIsDead");
        const bool* b = v ? v->get_if<bool>() : nullptr;
        return b && *b;
    }
    return false;
}

static void test_a_hunt() {
    std::printf("a creature from its living interactable to the corpse's end: a Hunting_Wolf, Novice, the Bow\n");
    HuntRig h;
    if (!h.open()) return;
    Player& p = *h.p;
    Rig& rig = h.rig;
    const world::WorldActor* body = rig.world->registry().find(h.wolf);
    CHECK(body && body->actor.subobjects.size() == 7 && body->npc.interact_def != 0 && rig.g().hunt_kind(body->npc.interact_def) != nullptr, "the Wolf has the eight blocks of a huntable and its hunting kind");
    const uint64_t comp = rig.component_of(h.wolf);
    const uint32_t ch = p.s->actor_view() ? p.s->actor_view()->channel_of(h.wolf) : 0;
    CHECK(ch != 0, "the client has the Wolf open");
    CHECK(p.s->inventory().at(Worn, 38) != nullptr, "the character wears a T1 Bow in slot 38");
    const float wear_before = p.s->inventory().at(Worn, 38)->durability;
    const int64_t xp_before = p.s->professions().xp(hid::kHunting);

    // the creature is alive and stays so until the hunt is done: no kill, no loot, no kill experience
    h.settle();
    combat::Aftermath& after = *rig.world->aftermath();
    CHECK(after.corpse_of(h.wolf) == nullptr && !dead_for_clients(rig.world->registry().find(h.wolf)) && after.stats().deaths == 0, "the Wolf is alive: no corpse, no record, not dead for its clients");

    // the hunt starts at the client's interact, on the living creature [R: the five captured hunts]
    size_t from = p.got.size();
    h.interact_with(p, h.wolf);
    CHECK(p.s->harvesting_node() == h.wolf && p.s->harvests_started() == 1 && p.s->harvest_refusals().count("alive") == 0, "the interaction with the LIVING creature starts the hunt (nothing is refused as alive)");
    rig.run(milliseconds(300));
    CHECK(!dead_for_clients(rig.world->registry().find(h.wolf)) && p.s->kills_rewarded() == 0 && after.stats().piles == 0, "it is still alive while the bar runs; no kill's experience, no pile");
    const auto set = channeling_seen(p, from);
    CHECK(set.size() == 1 && set[0].second.active && set[0].second.timed && set[0].second.duration > 14.0f && set[0].second.duration < 15.01f &&
              set[0].second.tool_record == p.s->inventory().at(Worn, 38)->item_record_id && set[0].second.target.ObjectId == rig.world->registry().find(h.wolf)->actor.guid.ObjectId,
          "the pawn's ChannelingData: the corpse, the Bow, the channel's length (15 s at rating 0)");
    const CorpseSeen early = corpse_seen(p, comp, ch, from);
    CHECK(!early.statuses.empty() && early.statuses[0] == 0x08000001u, "the corpse's interactable was told: the status 0x08000001 (counter 1, channeling)");
    CHECK(early.statuses.size() >= 3 && ((early.statuses[1] >> 27) & 31u) == 3u && ((early.statuses[2] >> 27) & 31u) == 5u, "then heartbeats whose counter goes up by 2 (3, 5 ...)");
    CHECK(rig.board().state_of(h.wolf) == gather::NodeState::Channeling && after.corpse_of(h.wolf) && after.corpse_of(h.wolf)->living && after.corpse_of(h.wolf)->close_at > rig.now() + 14.0,
          "the board has the creature Channeling, and it stays for the channel (its record is a living one)");
    CHECK(count_in_log("starts hunting Hunting_Wolf") == 1 && count_in_log("a Novice Hunting kill, with") == 1, "the log: starts hunting, a Novice Hunting kill, with the Bow");   // (the log's words)
    // a second player interaction on the same corpse by the same player: refused
    h.interact_with(p, h.wolf);
    CHECK(p.s->harvest_refusals().count("already channeling") == 1 && p.s->harvests_started() == 1, "a replayed interaction changes nothing (already channeling)");

    // the channel runs its length
    CHECK(run_until(rig, [&] { return p.s->harvests_completed() == 1; }, seconds(20)), "the hunt completes by the clock, about 15 s after");
    rig.run(milliseconds(300));
    const auto chains = pc_chain_calls(p, from);
    size_t durability_chain = static_cast<size_t>(-1), event_chain = static_cast<size_t>(-1);
    for (const auto& [chain, name] : chains) {
        if (name == "ClientUpdateItemDurability" && durability_chain == static_cast<size_t>(-1)) durability_chain = chain;
        if (name == "ClientReceiveStorageEvent" && event_chain == static_cast<size_t>(-1)) event_chain = chain;
    }
    const auto channeling = channeling_seen(p, from);
    const size_t xp_chain = xp_chain_since(p, from);
    CHECK(durability_chain != static_cast<size_t>(-1) && event_chain != static_cast<size_t>(-1) && durability_chain <= event_chain, "the result frame: the Bow's wear, then the storage event(s)");
    CHECK(channeling.size() == 2 && channeling[1].second.flags == 0x106 && !channeling[1].second.active && channeling[1].first > event_chain && channeling[1].first < xp_chain,
          "the pawn's ChannelingData is cleared after the items and before the XPTracker [R: 1457 packet 995]");
    CHECK(xp_chain != static_cast<size_t>(-1) && xp_chain > durability_chain, "and the XPTracker (Hunter XP) closes the frame");
    // the player's side
    const svc::InventoryItem* bow = p.s->inventory().at(Worn, 38);
    CHECK(bow && bow->durability == wear_before - 1.0f, "the Bow lost 1 durability");
    const int64_t gained = p.s->professions().xp(hid::kHunting) - xp_before;
    CHECK(gained >= 160 && gained % 80 == 0, "Hunter XP of 160 for a Common item, 80 more for each better rarity");
    const int64_t carcass = 6064630406019547147ll, trainable = 6064629326302543894ll;   // Resource_Raw_Carcass_Wolf, Resource_Raw_Trainable_Adult_Wolf
    int64_t held = 0;
    for (ItemContainer c : {R0, Bag}) {
        for (const svc::InventoryItem& it : p.s->inventory().items()) {
            if (it.container == c && (it.item_record_id == carcass || it.item_record_id == trainable)) held += it.quantity;
        }
    }
    int64_t in_bag = 0;
    for (const svc::InventoryItem& it : p.s->inventory().items()) {
        if (it.container == Bag && (it.item_record_id == carcass || it.item_record_id == trainable)) in_bag += it.quantity;
    }
    CHECK(held >= 1 && in_bag == 0, "a carcass or a trainable is in Resource0 (and none in the Backpack)");
    CHECK(count_in_log("hunted Hunting_Wolf") == 1 || count_in_log("[hunt] cyb: harvested Hunting_Wolf") == 1, "the log: harvested Hunting_Wolf ...");
    // the corpse's done update
    const CorpseSeen done = corpse_seen(p, comp, ch, from);
    CHECK(!done.statuses.empty() && (done.statuses.back() & 7u) == 4u && done.statuses.back() >> 27 != 0, "the corpse's last update has the status of phase 4 (done)");
    bool can_ok = !done.can_input.empty();
    for (const auto& c : done.can_input) can_ok = can_ok && c.size() == 4;
    CHECK(can_ok && done.can_input.back()[0] && !*done.can_input.back()[0], "CanInputActivate was told with four entries, the first false (never an empty array)");
    CHECK(done.last_states.size() == 4 && done.last_states[0] == 0 && done.last_states[1] == 0 && done.last_states[2] == 0 && done.last_states[3] == 0, "and all four interaction states 0");
    CHECK(!gather::node_gathered(*rig.world->registry().find(h.wolf)) && rig.board().state_of(h.wolf) == gather::NodeState::Gathered, "the creature is Gathered for the board and has no bGathered");
    CHECK(dead_for_clients(rig.world->registry().find(h.wolf)) && after.corpse_of(h.wolf) && !after.corpse_of(h.wolf)->living && after.corpse_of(h.wolf)->hunted,
          "the done update killed it for its clients (bIsDead), and its record is a hunted one now");
    CHECK(p.s->harvesting_node() == 0 && p.s->loot_opens() == 0, "this player hunts nothing now; no loot window was ever opened");
    // a second interaction before the corpse goes
    h.interact_with(p, h.wolf);
    CHECK(p.s->harvest_refusals().count("corpse hunted") == 1 && p.s->harvests_started() == 1, "the corpse is hunted already: refused");
    CHECK(after.corpse_of(h.wolf) && after.corpse_of(h.wolf)->hunted && after.corpse_of(h.wolf)->close_at <= rig.now() + 4.0 + 0.5, "the corpse goes at most 4 s after the hunt");
    rig.run(seconds(5));
    CHECK(rig.world->registry().find(h.wolf) == nullptr && after.corpse_of(h.wolf) == nullptr && rig.board().state_of(h.wolf) == gather::NodeState::Available, "5 s later the corpse is gone, and the board forgot it");
}

namespace chunt = capture_hunting;

// ---- T1 (retest 3): the end of a hunt against the five captured hunts -----------------------------------------------------------------------

/// One content block of a chain the client got: the object it names, whether it is the actor's, its RepLayout flag, its payload as hex (LSB first, packed from bit 0: the capture headers' form).
struct SeenBlock {
    uint64_t    object = 0;
    bool        is_actor = false;
    bool        rep = false;
    size_t      bits = 0;
    std::string hex;
};
// ~ C:Y:B :: C O A ~

static std::string hex_of_bits(const uint8_t* data, size_t pos, size_t bits) {
    static const char* const kHex = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < bits; i += 8) {
        unsigned byte = 0;
        for (size_t k = 0; k < 8 && i + k < bits; ++k) {
            const size_t at = pos + i + k;
            if ((data[at >> 3] >> (at & 7)) & 1u) byte |= 1u << k;
        }
        out += kHex[byte >> 4];
        out += kHex[byte & 15];
    }
    return out;
}

/// Every content block the client got on `channel` in the chains `got[from..]`, chain by chain (a close chain has none: it is listed with `closes`).
struct ChannelLog {
    std::vector<std::vector<SeenBlock>> chains;   // the content chains, in order
    std::vector<uint32_t> closes;                 // the reasons of the close chains
};

static ChannelLog channel_log(const Player& p, uint32_t channel, size_t from) {
    ChannelLog out;
    for (size_t i = from; i < p.got.size(); ++i) {
        const auto& d = p.got[i];
        if (d.channel != channel || d.open) continue;
        if (d.close) {
            out.closes.push_back(d.close_reason);
            continue;
        }
        const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
        if (!blocks) continue;
        std::vector<SeenBlock> chain;
        for (const auto& b : *blocks) {
            SeenBlock s;
            s.object = b.header.object.ObjectId;
            s.is_actor = b.header.is_actor;
            s.rep = b.header.has_rep_layout;
            s.bits = b.payload_bits;
            s.hex = hex_of_bits(d.content.data(), b.payload_pos, b.payload_bits);
            chain.push_back(std::move(s));
        }
        out.chains.push_back(std::move(chain));
    }
    return out;
}

static unsigned hex_byte(const std::string& hex, size_t index) {
    if (2 * index + 1 >= hex.size()) return 0;
    return static_cast<unsigned>(std::stoul(hex.substr(2 * index, 2), nullptr, 16));
}

static repl::CodecOptions stat_options() {
    repl::CodecOptions o;
    o.stat_int32_extended = [](int64_t record) {
        const uint32_t hi = static_cast<uint32_t>(static_cast<uint64_t>(record) >> 32);
        return hi == 0x5429e8c0u || hi == 0x5429e8c4u;
    };
    return o;
}

/// A stats block (hex, LSB first) read as the client reads it: the StatRepInt32EveryoneProxy delta it holds.
struct StatsDelta {
    bool     ok = false;
    int32_t  key = 0, base = 0, id = 0;
    size_t   changed = 0, deleted = 0;
    int64_t  record = 0;
    uint32_t value = 99;
};

static StatsDelta stats_delta_of(const std::string& hex, size_t bits) {
    StatsDelta out;
    std::vector<uint8_t> bytes((bits + 7) / 8);
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>(hex_byte(hex, i));
    size_t pos = 0;
    const auto fields = repl::read_fast_array_fields(class_named("UAoCStatsComponent"), bytes.data(), bits, pos, stat_options());
    if (!fields || fields->size() != 1) return out;
    const repl::FastArrayDelta& d = (*fields)[0].delta;
    out.ok = true;
    out.key = d.array_key;
    out.base = d.base_key;
    out.changed = d.changed.size();
    out.deleted = d.deleted.size();
    if (d.changed.size() == 1) {
        out.id = d.changed[0].id;
        const auto it = d.changed[0].values.values.find(1);
        const repl::StatInt32Inst* s = it == d.changed[0].values.values.end() ? nullptr : it->second.get_if<repl::StatInt32Inst>();
        if (s) {
            out.record = s->record;
            out.value = s->value;
        }
    }
    return out;
}
// [C | O | A - Cyb]

static void test_end_of_hunt() {
    std::printf("T1: the end of a hunt against the five captured hunts (1456 1457 1458 1178 1179): the creature's done packet, the pawn's clear, what follows\n");
    HuntRig h;
    if (!h.open()) return;
    Player& p = *h.p;
    Rig& rig = h.rig;
    const uint32_t ch = p.s->actor_view() ? p.s->actor_view()->channel_of(h.wolf) : 0;
    const world::WorldActor* body = rig.world->registry().find(h.wolf);
    const uint64_t comp = rig.component_of(h.wolf);
    CHECK(ch != 0 && body, "the client has the Wolf open");
    if (!ch || !body) return;
    h.settle();
    const size_t from = p.got.size();
    h.interact_with(p, h.wolf);
    CHECK(run_until(rig, [&] { return p.s->harvests_completed() == 1; }, seconds(20)), "the hunt completes by the clock");
    rig.run(milliseconds(300));
    const size_t after_done = p.got.size();
    rig.run(seconds(6));   // the creature goes 4 s after the hunt

    // the creature's channel: heartbeats are one 81-bit block of the interactable, the done packet is ONE chain of three blocks in the order of the object ids [R: 1456 packet 922, 1457 packet 995]
    const ChannelLog creature = channel_log(p, ch, from);
    const std::vector<SeenBlock>* done_chain = nullptr;
    size_t done_chains = 0;
    for (const auto& chain : creature.chains) {
        if (chain.size() > 1) {
            ++done_chains;
            done_chain = &chain;
        }
    }
    CHECK(done_chains == 1 && done_chain && done_chain->size() == 3, "the creature's done packet is one chain of three blocks (as the captures' CombatInfo, stats, interactable)");
    if (!done_chain || done_chain->size() != 3) return;
    const SeenBlock& dc = (*done_chain)[0];
    const SeenBlock& ds = (*done_chain)[1];
    const SeenBlock& di = (*done_chain)[2];
    CHECK(dc.object < ds.object && ds.object < di.object && di.object == comp, "... in the order of their object ids, the interactable last [R: 1456: 436126 436130 436136]");
    const chunt::DoneBlocks& retail = chunt::kDoneBlocks[0];   // 1456, the Bear: bIsDead alone changes
    CHECK(dc.rep && dc.bits == retail.combat.bits && dc.hex == retail.combat.hex, "the CombatInfo block is the capture's to the bit: 18 bits, bIsDead 1 [R: 1456 1458 1178 1179; 1457's 27 also clears bNotTargetable]");
    bool stats_same = true;
    for (const chunt::DoneBlocks& d : chunt::kDoneBlocks) stats_same = stats_same && std::string(d.stats.hex) == retail.stats.hex && d.stats.bits == retail.stats.bits;
    CHECK(stats_same, "(the five captured hunts write the same stats block: 278 bits, the Health item to 0)");
    // the block, read as the client reads it: one StatRepInt32EveryoneProxy delta, nothing deleted, the array key one above its base, one changed item: Health, 0.  The ids and keys come
    // from the array the open gave: the captured creatures' lists run to 25 stats (the Health item is id 17, the keys 25 -> 26), ours to fewer (a list the stat book fills, the same for
    // every creature of ours), so the three counters differ and every other bit is the capture's
    const StatsDelta ours = stats_delta_of(ds.hex, ds.bits), theirs = stats_delta_of(retail.stats.hex, retail.stats.bits);
    std::printf("      (the Health item is id %d, the array key %d -> %d; the capture's: id %d, key %d -> %d)\n", ours.id, ours.base, ours.key, theirs.id, theirs.base, theirs.key);
    CHECK(!ds.rep && ds.bits == retail.stats.bits && ours.ok && theirs.ok && ours.changed == 1 && ours.deleted == 0 && ours.key == ours.base + 1 && ours.record == content::kGuidHealth && ours.value == 0u &&
              theirs.record == ours.record && theirs.value == ours.value && theirs.key == theirs.base + 1 && theirs.changed == 1 && theirs.deleted == 0,
          "the stats block is the capture's: 278 bits, one StatRepInt32EveryoneProxy delta that changes the one Health item to 0 [R: 1456 1457 1458 1178 1179]");
    std::vector<size_t> differing;
    for (size_t b = 0; b < ds.bits && b < static_cast<size_t>(retail.stats.bits); ++b) {
        if (((hex_byte(ds.hex, b >> 3) >> (b & 7)) & 1u) != ((hex_byte(retail.stats.hex, b >> 3) >> (b & 7)) & 1u)) differing.push_back(b);
    }
    CHECK(differing.size() <= 6, "(only the id and the two keys differ from the capture's block: a few bits)");
    CHECK(di.rep && di.bits == chunt::kRun1457Done.bits, "the interactable's block is the done update's 370 bits (its content: test_corpse_wire, to the bit)");
    size_t other_chains = 0, odd_chains = 0;
    for (const auto& chain : creature.chains) {
        if (chain.size() != 1) continue;
        ++other_chains;
        if (!(chain[0].object == comp && chain[0].rep && (chain[0].bits == 418 || chain[0].bits == 217 || chain[0].bits == 81))) ++odd_chains;
    }
    CHECK(other_chains >= 100 && odd_chains == 0, "every other chain of the creature is the interactable's start (418 bits), its first beat (217) or a heartbeat (81 bits) [R: 1457]");
    CHECK(creature.closes.size() == 1 && creature.closes[0] == 0, "and 4 s later the channel closes once, Destroyed");

    // the pawn: the start (593 bits) and the clear (305 bits) of ChannelingData, nothing else
    const ChannelLog pawn = channel_log(p, p.pawn_ch, from);
    size_t blocks_on_pawn = 0;
    std::vector<size_t> pawn_bits;
    for (const auto& chain : pawn.chains) {
        for (const SeenBlock& b : chain) {
            if (b.is_actor) continue;   // (the movement acknowledgements)
            ++blocks_on_pawn;
            pawn_bits.push_back(b.bits);
        }
    }
    CHECK(blocks_on_pawn == 2 && pawn_bits.size() == 2 && pawn_bits[0] == 593 && pawn_bits[1] == 305, "the pawn gets two component blocks in the whole run: ChannelingData's start (593 bits) and its clear (305 bits) [R: 1457 packets 134 and 995; nothing else, no FoliageData]");
    const auto channeling = channeling_seen(p, from);
    CHECK(channeling.size() == 2 && channeling[1].second.flags == 0x106 && !channeling[1].second.active && !channeling[1].second.timed &&
              channeling[1].second.target.ObjectId == body->actor.guid.ObjectId,
          "the clear is the retail one (flags 0x106, not active, not timed) and names the creature");

    // after the done packet the creature is quiet and the pawn has nothing more to say
    size_t pawn_after = 0;
    for (const auto& chain : channel_log(p, p.pawn_ch, after_done).chains) {
        for (const SeenBlock& b : chain) pawn_after += b.is_actor ? 0 : 1;
    }
    CHECK(pawn_after == 0, "no component block of the pawn follows the done packet");
    size_t creature_after = 0;
    for (const auto& chain : channel_log(p, ch, after_done).chains) creature_after += chain.size();
    CHECK(creature_after == 0, "no block of the creature follows the done packet (it is only closed)");
}

static void test_refusals() {
    std::printf("the refusals of a hunt: logged, counted, nothing sent\n");
    HuntRig h;
    if (!h.open("Hunting_Bork")) return;   // an Apprentice kind
    Player& p = *h.p;
    Rig& rig = h.rig;
    const auto refused = [&](const char* kind) { return p.s->harvest_refusals().count(kind) ? p.s->harvest_refusals().at(kind) : size_t(0); };
    h.settle();
    size_t from = p.got.size();
    h.interact_with(p, h.wolf);
    CHECK(refused("not certified") == 1 && p.s->harvesting_node() == 0 && channeling_seen(p, from).empty(), "a Novice hunter and an Apprentice kind (a Bork): not certified");
    CHECK(count_in_log("is refused (not certified)") >= 1, "the log says so");
    // a Novice kind: no Bow
    const uint64_t wolf = rig.spawn("Hunting_Wolf", 160, 40);
    h.settle();
    p.send_now(item_used(4, 38, guid_n(138)), p.pawn_ch);
    rig.run(milliseconds(100));
    CHECK(!p.s->inventory().at(Worn, 38), "(the Bow is taken off)");
    from = p.got.size();
    h.interact_with(p, wolf);
    CHECK(refused("no tool") == 1 && channeling_seen(p, from).empty() && p.s->harvests_started() == 0, "no Bow in slot 38: no tool");
    int slot = -1;
    for (int s = 0; s < items::kBackpackSlots; ++s) {
        const svc::InventoryItem* it = p.s->inventory().at(Bag, s);
        if (it && it->instance_guid == guid_n(138)) slot = s;
    }
    if (slot >= 0) p.send_now(item_used(3, slot, guid_n(138)), p.pawn_ch);
    rig.run(milliseconds(100));
    CHECK(p.s->inventory().at(Worn, 38) != nullptr, "(the Bow is worn again)");
    // out of reach: 12 m away is inside the interaction's 25 m, outside the hunt's 330 cm
    const uint64_t far = rig.spawn("Hunting_Wolf", 1200, 0);
    h.settle();
    h.interact_with(p, far);
    CHECK(refused("out of range") == 1 && p.s->harvests_started() == 0, "12 m from the corpse: out of range");
    CHECK(count_in_log("[hunt] cyb: the hunt of") >= 3, "(each refusal is a [hunt] line)");
// -- C | O | A & C-Y-B --

    // a plain creature's corpse still opens its loot window
    uint64_t looted = 0;
    for (int i = 0; i < 60 && !looted; ++i) {
        const uint64_t id = rig.spawn("Pop_Wolf", 200, -100 - 10 * i, 4);
        if (!id) break;
        h.kill_it(id);
        const combat::Corpse* c = rig.world->aftermath()->corpse_of(id);
        if (c && c->parts) looted = id;
    }
    CHECK(looted != 0, "a plain Wolf's kill leaves loot within sixty tries");
    if (looted) {
        const size_t started = p.s->harvests_started(), refused_before = p.s->harvests_refused();
        h.interact_with(p, looted);
        CHECK(p.s->loot_opens() == 1 && p.s->harvests_started() == started && p.s->harvests_refused() == refused_before && !rig.world->registry().find(looted)->npc.interact_def,
              "a plain creature's corpse opens its loot window and starts no hunt");
    }
}

static void test_cancels() {
    std::printf("a hunt that ends with no reward: the corpse as it was, hunted again at once\n");
    HuntRig h;
    if (!h.open()) return;
    Player& p = *h.p;
    Rig& rig = h.rig;
    h.settle();
    const uint64_t comp = rig.component_of(h.wolf);
    const uint32_t ch = p.s->actor_view()->channel_of(h.wolf);
    const size_t items0 = p.s->inventory().count(R0);
    const int64_t xp0 = p.s->professions().xp(hid::kHunting);
    const auto started = [&] {
        h.interact_with(p, h.wolf);
        rig.run(milliseconds(300));
        return p.s->harvesting_node() == h.wolf && rig.board().state_of(h.wolf) == gather::NodeState::Channeling;
    };
    const auto nothing_granted = [&] {
        return p.s->inventory().count(R0) == items0 && p.s->professions().xp(hid::kHunting) == xp0 && p.s->harvests_completed() == 0 && rig.board().state_of(h.wolf) == gather::NodeState::Available;
    };
    // the client's own interrupt
    size_t from = p.got.size();
    CHECK(started(), "a hunt starts");
    p.send_now(interrupt_channel(), p.pawn_ch);
    CHECK(p.s->harvesting_node() == 0 && rig.board().state_of(h.wolf) == gather::NodeState::Available && p.s->harvests_cancelled() == 1, "ServerInterruptChannel: the hunt ends and the creature is free");
    CHECK(rig.world->aftermath()->corpse_of(h.wolf) == nullptr && rig.world->registry().find(h.wolf) != nullptr && !dead_for_clients(rig.world->registry().find(h.wolf)), "and it is alive as it was: no record, still in the world");
    rig.run(milliseconds(300));
    const auto ch1 = channeling_seen(p, from);
    CHECK(ch1.size() == 2 && ch1[1].second.flags == 0x106 && !ch1[1].second.active, "the pawn's ChannelingData was cleared");
    const CorpseSeen back = corpse_seen(p, comp, ch, from);
    bool can_ok = !back.can_input.empty();
    for (const auto& c : back.can_input) can_ok = can_ok && c.size() == 4;
    CHECK(!back.statuses.empty() && back.statuses.back() == 0u && can_ok && back.can_input.back()[0] && *back.can_input.back()[0],
          "the corpse was told: status 0, CanInputActivate four entries (the first true)");
    CHECK(back.last_states.empty() || back.last_states == std::vector<uint64_t>({1, 1, 1, 1}), "(and its four interaction states are 1: they were never changed, so none is sent)");
    rig.run(seconds(16));
    CHECK(nothing_granted(), "and nothing comes when the time would have been up");
    // a step of 70 cm
    from = p.got.size();
    CHECK(started(), "a hunt starts again");
    p.s->gm_teleport(p.conn, kCx + 70, kCy, bs::kDefaultSpawn.z, rig.t, "the test: a step away");
    rig.run(milliseconds(300));
    CHECK(p.s->harvesting_node() == 0 && p.s->harvests_cancelled() == 2 && rig.board().state_of(h.wolf) == gather::NodeState::Available, "70 cm from where it began: the hunt ends");
    CHECK(count_in_log("the player moved 70 cm since the channel began") == 1, "the log says why");
    rig.run(seconds(16));
    CHECK(nothing_granted(), "no reward");
    p.s->gm_teleport(p.conn, kCx, kCy, bs::kDefaultSpawn.z, rig.t, "the test: back");
    rig.run(milliseconds(400));
    // the Bow taken off
    CHECK(started(), "a hunt starts a third time");
    p.send_now(item_used(4, 38, guid_n(138)), p.pawn_ch);
    rig.run(milliseconds(100));
    CHECK(!p.s->inventory().at(Worn, 38) && p.s->harvesting_node() == 0 && rig.board().state_of(h.wolf) == gather::NodeState::Available, "the Bow is taken off: the hunt ends");
    int slot = -1;
    for (int s = 0; s < items::kBackpackSlots; ++s) {
        const svc::InventoryItem* it = p.s->inventory().at(Bag, s);
        if (it && it->instance_guid == guid_n(138)) slot = s;
    }
    if (slot >= 0) p.send_now(item_used(3, slot, guid_n(138)), p.pawn_ch);
    rig.run(milliseconds(200));
    CHECK(p.s->inventory().at(Worn, 38) != nullptr, "(the Bow is worn again)");
    // the player's death
    CHECK(started(), "a hunt starts a fourth time");
    p.s->take_damage(p.conn, 1.0e9, rig.t);
    rig.run(milliseconds(300));
    CHECK(p.s->dead() && p.s->harvesting_node() == 0 && rig.board().state_of(h.wolf) == gather::NodeState::Available, "the player dies: the hunt ends and the corpse is free");
    rig.run(seconds(16));
    CHECK(p.s->harvests_completed() == 0 && p.s->professions().xp(hid::kHunting) == xp0, "...with no reward");
    // a disconnect: another player takes over the corpse
    Player& q = rig.add_player(record_here("0000000000000000000000000000cc01", "carol"));
    play(q);
    h.interact_with(q, h.wolf);
    rig.run(milliseconds(300));
    CHECK(q.s->harvesting_node() == h.wolf && rig.board().state_of(h.wolf) == gather::NodeState::Channeling, "a second player hunts the corpse");
    q.s->finish("the test ends the session");
    CHECK(rig.board().state_of(h.wolf) == gather::NodeState::Available, "the session ends mid-hunt: the corpse is free at once");
    CHECK(count_in_log("the hunt of Hunting_Wolf") >= 4, "(each cancel is a [hunt] line)");
}
// ~ C | O | A - C:Y:B ~

static void test_two_players() {
    std::printf("two players on one corpse: the second is refused until the first stops\n");
    HuntRig h;
    if (!h.open()) return;
    Rig& rig = h.rig;
    Player& a = *h.p;
    Player& b = rig.add_player(record_here("0000000000000000000000000000cc01", "carol"));
    play(b);
    h.settle();
    h.interact_with(a, h.wolf);
    rig.run(milliseconds(300));
    CHECK(a.s->harvesting_node() == h.wolf, "the first player hunts the corpse");
    h.interact_with(b, h.wolf);
    CHECK(b.s->harvesting_node() == 0 && b.s->harvest_refusals().count("corpse busy") == 1, "the second is refused: corpse busy");
    CHECK(rig.board().state_of(h.wolf) == gather::NodeState::Channeling && a.s->harvesting_node() == h.wolf, "the first goes on");
    a.send_now(interrupt_channel(), a.pawn_ch);
    h.interact_with(b, h.wolf);
    rig.run(milliseconds(200));
    CHECK(b.s->harvesting_node() == h.wolf, "the first stops: the second can start");
}

static void test_flood() {
    std::printf("a flood of interactions: bounded work, one hunt, the rest dropped or counted\n");
    HuntRig h;
    if (!h.open()) return;
    Rig& rig = h.rig;
    Player& p = *h.p;
    h.settle();
    const uint64_t comp = rig.component_of(h.wolf);
    const auto begin = std::chrono::steady_clock::now();
    for (int i = 0; i < 400; ++i) {
        p.client_sends({p.client.actor(p.pawn_ch, interact(comp), true)});
        if (i % 50 == 49) rig.run(milliseconds(40));
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    rig.run(milliseconds(200));
    CHECK(p.s->harvests_started() == 1, "400 interactions in a row: one hunt");
    CHECK(p.s->harvests_refused() <= 30, "the budget drops the rest before the hunt's checks (at most a burst and a few per second are counted)");
    char msg[100];
    std::snprintf(msg, sizeof msg, "(400 interactions took %.0f ms)", ms);
    CHECK(ms < 3000.0, msg);
}

static void test_full_bag() {
    std::printf("a full resource bag: the rest of the hunt goes to a pile, the corpse is hunted all the same\n");
    Rig rig;
    if (!rig.open(g_path)) return;
    svc::Character rec = record_here("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb");
    for (int slot = 0; slot < 24; ++slot) rec.items.push_back(make_item(R0, slot, hid::kCoal, 300 + static_cast<uint32_t>(slot), 20, 0));
    Player& p = rig.add_player(std::move(rec));
    const uint64_t wolf = rig.spawn("Hunting_Wolf", 150, 0);
    play(p);
    rig.run(milliseconds(500));
    const size_t from = p.got.size();
    p.send_now(interact(rig.component_of(wolf)), p.pawn_ch);
    CHECK(run_until(rig, [&] { return p.s->harvests_completed() == 1; }, seconds(20)), "the hunt completes");
    rig.run(milliseconds(500));
    const auto names = pc_call_names_since(p, from);
    CHECK(count_names(names, "ClientReceiveStorageEvent") == 0 && count_names(names, "ClientUpdateItemDurability") == 1, "no storage event; the Bow's wear all the same");
    CHECK(p.s->harvest_piles() == 1 && rig.world->aftermath()->stats().piles == 1, "the carcass is in a pile where the player stands");
    CHECK(p.s->bootstrap().callouts() >= 1, "and a callout says why");
}

static void test_gives_nothing() {
    std::printf("a kind that gives no item: the experience only\n");
    HuntRig h;
    if (!h.open("Hunting_Grem_Prototype")) return;   // the CanTame prototype: its table needs the creature's wounds, which the roller cannot read
    Player& p = *h.p;
    Rig& rig = h.rig;
    const world::WorldActor* a = rig.world->registry().find(h.wolf);
    const gather::Gathering::HuntKind* kind = a ? rig.g().hunt_kind(a->npc.interact_def) : nullptr;
    CHECK(kind && kind->gives_nothing, "(the kind is one that gives nothing)");
    if (!kind || !kind->gives_nothing) return;
    h.settle();
    const size_t items0 = p.s->inventory().items().size();
    const int64_t xp0 = p.s->professions().xp(hid::kHunting);
    h.interact_with(p, h.wolf);
    CHECK(run_until(rig, [&] { return p.s->harvests_completed() == 1; }, seconds(20)), "the hunt completes");
    rig.run(milliseconds(300));
    CHECK(p.s->inventory().items().size() == items0 && p.s->harvest_piles() == 0, "no item, no pile");
    CHECK(p.s->professions().xp(hid::kHunting) >= xp0, "(Hunter XP is the table's: none when its experience lines are gated)");
    CHECK(count_in_log("this kind gives no item") == 1, "and the start's log line says the kind gives no item");
}

int main() {
    {   // the lines of the log (info and above) go to g_log: the tests read what the user's test would
        auto logger = std::make_shared<spdlog::logger>("test", std::make_shared<spdlog::sinks::ostream_sink_mt>(g_log));
        logger->set_level(spdlog::level::info);
        spdlog::set_default_logger(logger);
    }
    const char* path = db_path();
    if (!path) {
        std::printf("[skip] test_worldd_hunting_session: no content.db (set AOC_CONTENT_DB, or build dist/content/content.db)\n");
        return 0;
    }
    g_path = path;
    std::printf("content.db: %s\n", path);
    if (std::getenv("HUNT_ONLY_END")) {   // the end of a hunt alone (T1 of retest 3)
        test_end_of_hunt();
        std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
        return g_fail == 0 ? 0 : 1;
    }
    test_a_hunt();
    test_refusals();
    test_cancels();
    test_two_players();
    test_flood();
    test_full_bag();
    test_gives_nothing();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
