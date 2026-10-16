// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_foliage_session.cpp  (Gathering stage 3, part "Foliage gathering")
//
//  The cutting of a painted tree on connections: sessions with fake clients (the harness of test_worldd_gather_session) on a PlayerWorld over the real content.db (AOC_CONTENT_DB, else
//  dist/content/content.db of the working tree) and dist/content/region_foliage.tsv ("[skip]" without them), the player standing where capture 1428's player stood, 148 cm from its Oak.
//
//  the delivery     (the fix after the user's test 1) a client model that builds retail's two lookups from the chains it was sent: a player at the login spot, then at the user's Oak, Daffodil and
//                   Stone, 6 km away and back, has every instance's cell actor with the component initialized, and every close it was sent was a Dormancy close; a cut on a cell it holds
//                   re-opens the actor with the actor block, every component and the cut one's IndexedStates (capture 1428's packet 901, block for block); the load on the view
//  a harvest        the client's start (the instance's index left out when 0); 116 ms later FoliageData (15 s at rating 0, the instance, its owner and component, state Adult, the tool);
//                   no ChannelingData and nothing else while the channel runs; the result frame in capture 1463's order: the tool's wear, the storage event and the confirm, the XPTracker,
//                   the cell's state actor opened with the instance Empty, FoliageData cleared; the logs in Resource0, the axe one lighter, Lumberjacking XP; the instance Empty on a SECOND
//                   client too; the regrowth (x0.001): Infancy, Juvenile, then a DELETE from the list, told to both clients; the tree is cut again
//  the refusals     an instance the map does not have (a town's runtime tree among them), a state the world does not agree with, an Empty instance, 200 m away, no axe, dead: logged, counted,
//                   the world unchanged, the client told with ClientInterruptInteractionWithFoliage and nothing else (it set its own FoliageData with the start); a start while walking: the
//                   callout and the same call (1464), taken off 3 s later; the retry when still runs
//  the cancels      the client's ServerInterruptChannel, a step of 70 cm (told with the interrupt), the axe taken off, a death (told), a disconnect: no reward, the lock released, FoliageData cleared
//  T1 (trees)       a tree's start in the bunch of the client's 550-bit call on a subobject it names by path reaches the handler (before it, behind it, behind thirteen of them); a second start
//                   while the channel runs is not answered with the interrupt; a cutting ended before FoliageData went out is (retests 3 and 4 of 2026-10-12)
//  exploits         two players on one tree, a replayed start, a flood of 400 starts, a full bag (the pile and the callout), a world without foliage (one info line)
//  A1 (the pre-merge review of 2026-10-16)  a tree's start while a NODE's channel runs (a hunt's is the same): refused as already channeling and the client IS told (exactly one
//                   ClientInterruptInteractionWithFoliage: it set its own FoliageData with the start), the node's channel untouched, a tree cut after the node
// ============================================================================
#include "capture_foliage.h"
#include "capture_gather.h"
#include "capture_in_place.h"
#include "capture_mount.h"
#include "content_db_fixture.h"
#include "cyb_appearance.h"
#include "fake_client.h"
#include "bootstrap/character_setup.h"
#include "gather/gathering.h"
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
#include "test_support.h"
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
// CoA / C.Y.B

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#endif

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
#undef CHECK
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
// -- Cyb x C-O-A --

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
    const spawns::OpenTemplate* donor = nullptr;
    RecordingSink sink;
    Clock::time_point t = t0;
    std::vector<std::unique_ptr<Player>> players;

    bool open(const char* path, double respawn_mult = 0.001) {
        std::string error;
        db = content::ContentDb::open(path, error);
        if (!db) {
            std::printf("  [FAIL] the content.db does not open: %s\n", error.c_str());
            return false;
        }
        world = std::make_unique<PlayerWorld>(&*db, "", bs::GameStateMode::Tier2, kServerId, guids);
        world->set_position_sink(&sink);
        auto c = spawns::CapturedData::load(error);
        if (!c) {
            std::printf("  [FAIL] the captured spawns do not load: %s\n", error.c_str());
            return false;
        }
        captured = std::move(*c);
        for (const spawns::OpenTemplate& o : captured->opens()) {
            if (o.category == "gatherable") {
                donor = &o;
                break;
            }
        }
        gather::Config cfg;
        cfg.board.respawn_mult = respawn_mult;
        if (!world->start_gathering(cfg, error)) {
            std::printf("  [FAIL] gathering does not start: %s\n", error.c_str());
            return false;
        }
        return donor != nullptr;
    }
    gather::Gathering& g() { return *world->gathering(); }
    gather::NodeBoard& board() { return g().board(); }
// (C-O-A + Cyb)

    uint64_t add_node(int64_t def_guid, double dx, double dy, uint32_t seed, double z = bs::kDefaultSpawn.z) {
        const content::GatherDefRow* def = g().def(def_guid);
        spawns::InstanceParams p;
        p.keep_captured_position = true;
        p.server_id = kServerId;
        p.seed = 0x6A12DE0ull ^ seed;
        std::string why;
        auto node = spawns::instantiate(*donor, [this] { return guids.next(); }, p, why);
        if (!node || !def) return 0;
        repl::ObjectState& actor = node->actor.obj;
        actor.state.set(*actor.schema, "GatherableActorDef.Guid", repl::Value::i64(def_guid));
        for (repl::Subobject& sub : node->actor.subobjects) {
            if (!sub.obj.schema || std::string(sub.obj.schema->name) != "UInteractableComponent") continue;
            const repl::Value* data = sub.obj.state.get(*sub.obj.schema, "InteractionInputData");
            const auto* arr = data ? data->get_if<repl::ArrayValue>() : nullptr;
            if (arr && !arr->elements.empty()) {
                repl::ArrayValue copy = *arr;
                copy.elements[0].values[6] = repl::Value::i64(def->interaction_guid);
                sub.obj.state.set(*sub.obj.schema, "InteractionInputData", repl::Value(std::move(copy)));
            }
        }
        const double x = kCx + dx, y = kCy + dy;
        node->x = x;
        node->y = y;
        node->z = z;
        node->npc.source = world::Source::Runtime;
        const uint64_t id = node->id;
        return world->registry().add(std::move(*node)) ? id : 0;
    }
    uint64_t component_of(uint64_t node) {
        const world::WorldActor* a = world->registry().find(node);
        return a ? a->npc.interact_component : 0;
    }

    Player& add_player(svc::Character rec) {
        players.push_back(std::make_unique<Player>(*this, std::move(rec)));
        players.back()->join();
        return *players.back();
    }
    void step(milliseconds d = milliseconds(20)) {
        t += d;
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
// (CoA : C.Y.B)

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
// (C-Y-B) (CoA)

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
// CoA.C | Y | B

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

// ---- the tests of the foliage ---------------------------------------------------------------------------------------------------------------

static const char* g_path = nullptr;
static const char* g_tsv = nullptr;

namespace fid {
inline constexpr int64_t kLumberjacking = 108752769996426ll;
// the retail pawn's position at the start of capture 1428 (148 cm from its Oak) and the Oak: Tree_Oak_Large_1, FISMC_0 index 0 of the cell (-41, 19)
inline constexpr double kOakX = -1032805.5, kOakY = 495823.7, kOakZ = 12846.0;
inline constexpr double kStandX = -1032839.01, kStandY = 495968.38, kStandZ = 12950.98;
inline constexpr const char* kOwner = "InstancedFoliageActor_25600_-41_19_0";
inline constexpr const char* kFismc0 = "IntrepidGatherableFISMC_0";
// Tree_Oak_Large_1 further in the same cell (FISMC_0 index 1 is another Oak of the cell)
inline constexpr int64_t kOakLog = 109162169652924ll;   // the Oak item 1428's frame shows
inline constexpr int64_t kCoal = 109162169652923ll;     // Resource_Raw_Stone_Coal: 1 x 1, stacks to 20 (a filler of the Sanctus bag's 24 cells)
}  // namespace fid

/// A Bard with the worn kit and the three tools, standing at (x, y, z).
static svc::Character record_at(const char* id_hex, const char* name, double x, double y, double z) {
    svc::Character c = record_of(id_hex, name);
    c.has_position = true;
    c.position.x = x;
    c.position.y = y;
    c.position.z = z;
    return c;
}

/// The pawn's UPlayerInfo.FoliageData the client was told in the chains `got[from..]`, in order: (the index of the chain in `got`, the FoliageData's values).
struct FoliageSeen {
    size_t   chain = 0;
    float    duration = 0;
    double   start_time = 0;
    int64_t  instance = 0;
    std::string owner, component;
    uint64_t state = 0;
    int64_t  tool_record = 0;
};
static std::string name_text(const repl::Value* v) {
    const repl::NameValue* n = v ? v->get_if<repl::NameValue>() : nullptr;
    return n ? repl::fname_text(*n) : std::string();
}
static std::vector<FoliageSeen> foliage_data_seen(const Player& p, size_t from) {
    std::vector<FoliageSeen> out;
    const schema::ClassSchema& info = class_named("UPlayerInfo");
    const bs::SessionGuids& g = p.s->bootstrap().setup().guids;
    for (size_t i = from; i < p.got.size(); ++i) {
        if (p.got[i].channel != p.pawn_ch || p.got[i].open || p.got[i].close) continue;
        const auto blocks = repl::read_content_blocks(p.got[i].content.data(), p.got[i].content.bit_pos(), 0);
        if (!blocks) continue;
        for (const auto& b : *blocks) {
            if (b.header.is_actor || !b.header.has_rep_layout || !(b.header.object == g.base_character_info)) continue;
            size_t pos = b.payload_pos;
            const auto st = repl::read_rep_block(info, p.got[i].content.data(), b.payload_pos + b.payload_bits, pos);
            if (!st) continue;
            const repl::Value* dur = st->get(info, "FoliageData.Duration");
            if (!dur) continue;
            FoliageSeen f;
            f.chain = i;
            f.duration = dur->get_if<float>() ? *dur->get_if<float>() : -1.0f;
            const repl::Value* t = st->get(info, "FoliageData.ServerStartTime");
            f.start_time = t && t->get_if<double>() ? *t->get_if<double>() : -1.0;
            const repl::Value* idx = st->get(info, "FoliageData.OriginalInstanceIndex");
            f.instance = idx && idx->get_if<int64_t>() ? *idx->get_if<int64_t>() : -2;
            f.owner = name_text(st->get(info, "FoliageData.FoliageName"));
            f.component = name_text(st->get(info, "FoliageData.FoliageComponentUUID"));
            const repl::Value* s = st->get(info, "FoliageData.FoliageState");
            f.state = s && s->get_if<uint64_t>() ? *s->get_if<uint64_t>() : 99;
            const repl::Value* tool = st->get(info, "FoliageData.ToolRecordId");
            f.tool_record = tool && tool->get_if<int64_t>() ? *tool->get_if<int64_t>() : -1;
            out.push_back(std::move(f));
        }
    }
    return out;
}
// [C | O | A + C | Y | B]

/// What the client saw of the foliage state actors in the chains `got[from..]`: one entry per chain on a channel that an AIntrepidStateIFA opened (until its close), with the owner's name
/// (from the open) and the IndexedStates of the components in it.
struct IfaSeen {
    size_t      chain = 0;
    bool        open = false, close = false;
    std::string owner;
    std::vector<std::pair<uint32_t, uint32_t>> changed;   // (index, state)
    std::vector<int32_t> deleted;                           // ElementIDs
    size_t      components = 0;                              // component blocks in the chain
};
/// The bits of SerializeNewActor at the start of an open of a state actor: its GUID, the archetype's and the level's (128 bits each), the quantized location, and no rotation, scale or velocity.
static size_t new_actor_prefix_bits(const emit::BunchWriter& content) {
    aoc::protocol::wire::PacketReader r(content.data(), (content.bit_pos() + 7) / 8, content.bit_pos());
    r.set_pos(384);
    if (r.read_bit()) {
        if (r.read_bit()) {
            if (!repl::read_quantized_vector(r)) return 0;
        } else {
            r.read_bits(64);
            r.read_bits(64);
            r.read_bits(64);
        }
    }
    r.read_bit();   // rotation
    r.read_bit();   // scale
    r.read_bit();   // velocity
    return r.pos();
}

static std::vector<IfaSeen> ifa_seen(const Player& p, size_t from) {
    std::vector<IfaSeen> out;
    const schema::ClassSchema& ifa = class_named("AIntrepidStateIFA");
    const schema::ClassSchema& comp = class_named("UIntrepidFoliageStateComponent");
    std::map<uint32_t, std::string> channels;   // channel -> owner, while open
    for (size_t i = from; i < p.got.size(); ++i) {
        const uint32_t ch = p.got[i].channel;
        if (p.got[i].open) {
            channels.erase(ch);
            const size_t prefix = new_actor_prefix_bits(p.got[i].content);
            const auto blocks = prefix ? repl::read_content_blocks(p.got[i].content.data(), p.got[i].content.bit_pos(), prefix) : std::nullopt;
            if (!blocks || blocks->empty() || !(*blocks)[0].header.is_actor || !(*blocks)[0].header.has_rep_layout) continue;
            size_t pos = (*blocks)[0].payload_pos;
            const auto st = repl::read_rep_block(ifa, p.got[i].content.data(), (*blocks)[0].payload_pos + (*blocks)[0].payload_bits, pos);
            const repl::Value* name = st ? st->get(ifa, "FoliageName") : nullptr;
            if (!name) continue;
            channels[ch] = name_text(name);
        }
        const auto it = channels.find(ch);
        if (it == channels.end()) continue;
        IfaSeen s;
        s.chain = i;
        s.open = p.got[i].open;
        s.close = p.got[i].close;
        s.owner = it->second;
        if (!p.got[i].close) {
            const auto blocks = repl::read_content_blocks(p.got[i].content.data(), p.got[i].content.bit_pos(), p.got[i].open ? new_actor_prefix_bits(p.got[i].content) : 0);
            if (blocks) {
                for (const auto& b : *blocks) {
                    if (b.header.is_actor) continue;
                    ++s.components;
                    size_t pos = b.payload_pos;
                    const size_t end = b.payload_pos + b.payload_bits;
                    if (b.header.has_rep_layout && !repl::read_rep_block(comp, p.got[i].content.data(), end, pos)) continue;
                    if (pos >= end) continue;
                    const auto fields = repl::read_fast_array_fields(comp, p.got[i].content.data(), end, pos);
                    if (!fields) continue;
                    for (const repl::FastArrayField& f : *fields) {
                        for (const int32_t d : f.delta.deleted) s.deleted.push_back(d);
                        for (const auto& ch2 : f.delta.changed) {
                            const auto st1 = ch2.values.values.find(1);
                            const auto key = ch2.values.values.find(2);
                            if (st1 != ch2.values.values.end() && key != ch2.values.values.end() && st1->second.get_if<uint64_t>() && key->second.get_if<uint64_t>()) {
                                s.changed.emplace_back(static_cast<uint32_t>(*key->second.get_if<uint64_t>()), static_cast<uint32_t>(*st1->second.get_if<uint64_t>()));
                            }
                        }
                    }
                }
            }
        }
        out.push_back(std::move(s));
        if (p.got[i].close) channels.erase(ch);
    }
    return out;
}
// Cyb - C O A

/// The client's ServerMovePacked of a walk: a held direction at (x, y).
static BunchWriter client_move(float ts, double x, double y, double z, bool moving) {
    repl::NetworkMove m;
    m.time_stamp = ts;
    m.acceleration = repl::quantize_vector(10, moving ? 100 : 0, 0, 0);
    m.location = repl::quantize_vector(100, x, y, z);
    m.movement_mode = repl::kMoveWalking;
    m.input_bits = moving ? 1 : 0;
    const schema::ClassSchema& cls = class_named("PlayerPawn_C");
    const schema::NetField* f = cls.field("ServerMovePacked");
    BunchWriter out;
    repl::MovePacked mp;
    mp.new_move = m;
    BunchWriter params;
    BunchWriter field;
    if (!f || !repl::write_move_packed(params, mp) || repl::write_field(cls, f->index, params, field) == 0) return out;
    repl::BlockHeader header;
    repl::write_content_block(out, header, field, /*server_form=*/false);
    return out;
}

/// ServerStartInteractionWithFoliage as the client sends it: the instance index is left out when it is 0 [R: capture 1428].
static BunchWriter foliage_start(const char* owner, const char* component, uint32_t index, uint64_t state) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerStartInteractionWithFoliage");
    repl::RpcParams p;
    if (!rpc) return BunchWriter();
    if (index != 0) repl::set_rpc_param(*rpc, p, "OriginalInstanceIndex", repl::Value::i64(index));
    repl::set_rpc_param(*rpc, p, "FoliageName", repl::Value(repl::fname_parse(owner)));
    repl::set_rpc_param(*rpc, p, "FoliageComponentUUID", repl::Value(repl::fname_parse(component)));
    repl::set_rpc_param(*rpc, p, "FoliageState", repl::Value::u64(state));
    return actor_rpc(pc, "ServerStartInteractionWithFoliage", p);
}

static std::vector<std::string> pc_call_names_since(const Player& p, size_t from) {
    std::vector<std::string> out;
    for (const auto& [chain, name] : pc_chain_calls(p, from)) out.push_back(name);
    return out;
}

struct FRig {
    Rig rig;
    bool open(double regrow_mult = 0.01, bool all_cells = true) {
        if (!rig.open(g_path)) return false;
        std::string error;
        foliage::Config cfg;
        cfg.map_path = g_tsv;
        cfg.regrow_mult = regrow_mult;
        cfg.all_cells = all_cells;
        if (!rig.world->start_foliage(cfg, error)) {
            std::printf("  [FAIL] foliage does not start: %s\n", error.c_str());
            return false;
        }
        return true;
    }
    foliage::Foliage& f() { return *rig.world->foliage(); }
    uint32_t oak_row() { return *f().map().find(fid::kOwner, fid::kFismc0, 0); }
};

static Player& join_at_the_oak(FRig& fr, const char* id_hex, const char* name) {
    Player& p = fr.rig.add_player(record_at(id_hex, name, fid::kStandX, fid::kStandY, fid::kStandZ));
    play(p);
    return p;
}

static void test_a_harvest() {
    std::printf("a tree from the start to its return: capture 1428's Oak\n");
    FRig fr;
    if (!fr.open()) return;
    Player& p = join_at_the_oak(fr, "bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb");
    Player& other = join_at_the_oak(fr, "0000000000000000000000000000cc01", "carol");
    fr.rig.run(seconds(2));
    const uint32_t row = fr.oak_row();
    CHECK(fr.f().store().state_of(row) == foliage::kAdult && p.s->cutting_instance() == PlayerSession::kNoInstance, "the Oak is Adult and nobody cuts it");
    CHECK(p.s->inventory().at(Worn, 29) != nullptr, "the character wears a T1 axe in slot 29");
    const float wear_before = p.s->inventory().at(Worn, 29)->durability;
    const int64_t xp_before = p.s->professions().xp(fid::kLumberjacking);
    const size_t from = p.got.size(), other_from = other.got.size();
    p.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 2), p.pc_ch);
    CHECK(p.s->cutting_instance() == row && p.s->foliage_started() == 1 && fr.f().store().locked_by(row) != 0, "the start: this player cuts the instance, and it is locked");
    CHECK(foliage_data_seen(p, from).empty(), "FoliageData is not sent with the start itself (116 ms later in retail)");
    fr.rig.run(milliseconds(160));
    const auto set = foliage_data_seen(p, from);
    CHECK(set.size() == 1 && set[0].duration > 14.0f && set[0].duration < 15.0f && set[0].instance == 0 && set[0].owner == fid::kOwner && set[0].component == fid::kFismc0 && set[0].state == 2,
          "116 ms after the start the client was told FoliageData: 14.7 s (the base 15 s and the T1 axe's speed: capture 1428's player had 14.71), the instance, its owner and component, state Adult");
    CHECK(set[0].tool_record == p.s->inventory().at(Worn, 29)->item_record_id && set[0].start_time > 0.0, "... with the axe's record and the start on the effects' clock");
    CHECK(channeling_seen(p, from).empty(), "the pawn's ChannelingData is not set for a tree: its channel is FoliageData's [R: 1428 and 1464 have no ChannelingData]");
    CHECK(pc_call_names_since(p, from).empty(), "nothing else is sent while the channel runs");
    CHECK(ifa_seen(p, from).empty(), "and the cell's state actor is not sent again (the client has held it since the login)");
    // the channel runs its length
    CHECK(run_until(fr.rig, [&] { return p.s->foliage_completed() == 1; }, seconds(20)), "the channel completes by the clock, about 15 s after");
    fr.rig.run(milliseconds(400));
    const auto names = pc_call_names_since(p, from);
    CHECK(names.size() >= 3 && names[0] == "ClientUpdateItemDurability" && names[1] == "ClientReceiveStorageEvent" && names[2] == "ClientConfirmMultiInteractFoliage",
          "the result frame: the tool's wear, the storage event, the confirm (capture 1463's packet 833, in its order)");
    const auto chains = pc_chain_calls(p, from);
    size_t confirm_chain = static_cast<size_t>(-1), durability_chain = static_cast<size_t>(-1);
    for (const auto& [chain, name] : chains) {
        if (name == "ClientConfirmMultiInteractFoliage") confirm_chain = chain;
        if (name == "ClientUpdateItemDurability") durability_chain = chain;
    }
    const size_t xp_chain = xp_chain_since(p, from);
    const auto cleared = foliage_data_seen(p, from);
    const auto cells = ifa_seen(p, from);
    CHECK(xp_chain != static_cast<size_t>(-1) && xp_chain > confirm_chain && durability_chain < confirm_chain, "the XPTracker follows the confirm");
    CHECK(cleared.size() == 2 && cleared[1].duration == 0.0f && cleared[1].state == 3 && static_cast<uint32_t>(cleared[1].instance) == 0xFFFFFFFFu && cleared[1].owner.empty(),
          "FoliageData is cleared after the result: duration 0, index 0xFFFFFFFF, no name, state 3");
    CHECK(cells.size() >= 1 && cells[0].open && cells[0].owner == fid::kOwner && cells[0].changed.size() == 1 && cells[0].changed[0] == std::make_pair(0u, 3u),
          "the cell's state actor opened on this client with the instance Empty: index 0, state 3 (a first look lists Empty as 11 only for a cell with older Empty instances: none yet)");
    // the items, the wear, the XP, the state
    CHECK(p.s->inventory().count(R0) > 0, "the logs are in Resource0");
    const svc::InventoryItem* axe = p.s->inventory().at(Worn, 29);
    CHECK(axe && axe->durability == wear_before - 1.0f, "the axe lost 1 durability");
    CHECK(p.s->professions().xp(fid::kLumberjacking) > xp_before, "Lumberjacking XP came");
    CHECK(fr.f().store().state_of(row) == foliage::kEmpty && fr.f().store().locked_by(row) == 0 && p.s->cutting_instance() == PlayerSession::kNoInstance, "the instance is Empty and free again; this player cuts nothing");
    // a second client sees it
    const auto theirs = ifa_seen(other, other_from);
    CHECK(!theirs.empty() && theirs[0].open && theirs[0].owner == fid::kOwner && theirs[0].changed.size() == 1 && theirs[0].changed[0] == std::make_pair(0u, 3u),
          "a SECOND client near the tree was sent the same state actor with the instance Empty");
    bool closed = false;
    for (const IfaSeen& s : cells) closed = closed || s.close;
    CHECK(closed, "and the state actor is closed with Dormancy again (retail: 265 ms after the open)");
    // regrowth: x0.01, an Oak is Empty for 3 s, a sapling for 18 s, medium for 18 s; the delete reaches the clients that hold the cell
    const size_t regrow_from = p.got.size();
    fr.rig.run(seconds(40));
    const auto grown = ifa_seen(p, regrow_from);
    size_t deletes = 0, infancy = 0, juvenile = 0;
    for (const IfaSeen& s : grown) {
        deletes += s.deleted.size();
        for (const auto& c : s.changed) {
            infancy += c.second == 0 ? 1 : 0;
            juvenile += c.second == 1 ? 1 : 0;
        }
    }
    CHECK(fr.f().store().state_of(row) == foliage::kAdult && !fr.f().store().deviates(row), "after 39 s (x0.01) the Oak is whole: Adult, painted");
    CHECK(infancy == 1 && juvenile == 1 && deletes == 1, "the client was told Infancy, then Juvenile, then the instance was deleted from the list (a delete, not a state)");
    const auto last = ifa_seen(other, other_from);
    size_t other_deletes = 0;
    for (const IfaSeen& s : last) other_deletes += s.deleted.size();
    CHECK(other_deletes == 1, "the second client was told the delete as well");
    // the tree can be cut again
    const size_t second_from = p.got.size();
    p.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 2), p.pc_ch);
    CHECK(p.s->cutting_instance() == row && p.s->foliage_started() == 2, "a regrown tree is cut again");
    (void)second_from;
}
// [C.Y.B ~ CoA]

static void test_refusals() {
    std::printf("the refusals of a start: logged, counted, the world unchanged, and the client told to end its own cutting\n");
    FRig fr;
    if (!fr.open()) return;
    Player& p = join_at_the_oak(fr, "bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb");
    const uint32_t row = fr.oak_row();
    const auto refused = [&](const char* kind) { return p.s->foliage_refusals().count(kind) ? p.s->foliage_refusals().at(kind) : size_t(0); };
    size_t from = p.got.size();
    // a refusal changes nothing in the world and answers the client with ClientInterruptInteractionWithFoliage and nothing else (the client sets its own FoliageData with the start and the call
    // clears it; before the trees' retest 4 every refusal but a start while walking was silent and left the client "gathering"): `answered(n)` is n refusals since `from`, each so answered
    const auto answered = [&](size_t n) {
        const auto names = pc_call_names_since(p, from);
        return foliage_data_seen(p, from).empty() && names.size() == n && static_cast<size_t>(std::count(names.begin(), names.end(), "ClientInterruptInteractionWithFoliage")) == n &&
               p.s->cutting_instance() == PlayerSession::kNoInstance && fr.f().store().locks() == 0;
    };
    p.send_now(foliage_start(fid::kOwner, fid::kFismc0, 999, 2), p.pc_ch);
    CHECK(refused("unknown instance") == 1 && answered(1), "an instance the map does not have");
    p.send_now(foliage_start("IFA_NodeLayoutReplicator_108646108518008_4_0", "IntrepidGatherableFISMC_2", 12, 1), p.pc_ch);
    CHECK(refused("unknown instance") == 2 && answered(2), "a town's runtime tree (no row in the map) is not harvestable");
    p.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 1), p.pc_ch);
    CHECK(refused("wrong state") == 1 && answered(3), "the client says Juvenile, the Oak is Adult");
    p.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 0), p.pc_ch);
    CHECK(refused("wrong state") == 2 && answered(4), "the client says Infancy");
    // no tool: the axe is taken off
    p.send_now(item_used(4, 29, guid_n(129)), p.pawn_ch);
    fr.rig.run(milliseconds(100));
    CHECK(!p.s->inventory().at(Worn, 29), "(the axe is taken off)");
    from = p.got.size();
    p.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 2), p.pc_ch);
    CHECK(refused("no tool") == 1 && answered(1), "nothing in the axe slot");
    int axe_slot = -1;
    for (int slot = 0; slot < items::kBackpackSlots; ++slot) {
        const svc::InventoryItem* it = p.s->inventory().at(Bag, slot);
        if (it && it->instance_guid == guid_n(129)) axe_slot = slot;
    }
    if (axe_slot >= 0) p.send_now(item_used(3, axe_slot, guid_n(129)), p.pawn_ch);
    fr.rig.run(milliseconds(100));
    CHECK(p.s->inventory().at(Worn, 29) != nullptr, "(the axe is worn again)");
    // out of range
    p.s->gm_teleport(p.conn, fid::kStandX + 20000, fid::kStandY, fid::kStandZ, fr.rig.t, "the test: far away");
    fr.rig.run(milliseconds(400));
    from = p.got.size();
    p.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 2), p.pc_ch);
    CHECK(refused("out of range") == 1 && answered(1), "200 m from the tree");
    p.s->gm_teleport(p.conn, fid::kStandX, fid::kStandY, fid::kStandZ, fr.rig.t, "the test: back");
    fr.rig.run(milliseconds(400));
    // cut by someone else meanwhile: the client's Adult is stale, and an Empty instance has no harvest
    fr.f().harvested(row, fr.rig.t);
    from = p.got.size();
    p.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 2), p.pc_ch);
    CHECK(refused("wrong state") == 3 && answered(1), "the Oak was cut meanwhile: the client's Adult is stale");
    p.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 3), p.pc_ch);
    CHECK(refused("no interaction") == 1 && answered(2), "and an Empty instance has no harvest");
    // dead
    p.s->take_damage(p.conn, 1.0e9, fr.rig.t);
    fr.rig.run(milliseconds(300));
    from = p.got.size();
    const uint32_t row2 = *fr.f().map().find(fid::kOwner, fid::kFismc0, 1);
    p.send_now(foliage_start(fid::kOwner, fid::kFismc0, 1, static_cast<uint64_t>(fr.f().store().state_of(row2))), p.pc_ch);
    {
        const auto names = pc_call_names_since(p, from);
        CHECK(refused("dead") == 1 && foliage_data_seen(p, from).empty() && std::count(names.begin(), names.end(), "ClientInterruptInteractionWithFoliage") == 1, "a dead player cuts nothing (and its client is told to end its own)");
    }
    CHECK(p.s->foliage_started() == 0 && p.s->foliage_completed() == 0, "no cutting ever started");
    CHECK(count_in_log("the cutting of InstancedFoliageActor_25600_-41_19_0 / IntrepidGatherableFISMC_0 #0 (Adult) is refused (no tool)") == 1, "the log says what was refused and why");
}

// ---- T1 of the trees that start and never finish (the retests of 2026-10-12) ---------------------------------------------------------------------------

/// The Oak's start as capture 1428's client sent it (packet 53): the field of the capture in a content block of the PlayerController's channel (the actor block).
static BunchWriter retail_oak_start() {
    for (const capture_foliage::Start& s : capture_foliage::kStarts) {
        if (s.capture != 1428 || s.packet != 53) continue;
        const std::vector<uint8_t> bytes = tsupport::bits_of_hex(s.field.hex, s.field.bits).bytes();
        BunchWriter payload;
        payload.write_bit_range(bytes.data(), 0, s.field.bits);
        BunchWriter out;
        repl::write_content_block(out, repl::BlockHeader{}, payload, /*server_form=*/false);
        return out;
    }
    return BunchWriter();
}

static BunchWriter joined(std::initializer_list<const BunchWriter*> parts) {
    BunchWriter out;
    for (const BunchWriter* p : parts) out.write_bit_range(p->data(), 0, p->bit_pos());
    return out;
}
// (CoA + C:Y:B)

/// The tree retests of 2026-10-12 (retest 3 and the 13:34 run): worldd logged "a bunch of 550 bits on channel 3 (PlayerController) does not read: ignored" ten times a second and not one
/// foliage start, and the client, which sets its own FoliageData with a start, answered "Unable to interact with Item while gathering!" to everything.  The 550-bit call is the client's own
/// block on a subobject it names by path (capture_in_place.h); the reader refused ObjectId 1 in a block's header and dropped the whole bunch, a start that shared it among the calls.
/// The start reaches the foliage handler now, in front of the call, behind it and behind thirteen of them; and every refusal and cancel of a start gives the client its gathering back.
static void test_start_beside_an_unnamed_call() {
    std::printf("T1: a tree's start in the bunch of the client's 550-bit call on a subobject it names by path reaches the foliage handler\n");
    FRig fr;
    if (!fr.open()) return;
    Player& p = join_at_the_oak(fr, "bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb");
    const uint32_t row = fr.oak_row();
    const BunchWriter call = capture_in_place::block();
    const BunchWriter start = retail_oak_start();
    CHECK(call.bit_pos() == capture_in_place::kBlockBits && start.bit_pos() > 0, "(the client's 550-bit call, and capture 1428's retail start of the Oak)");
    const size_t unread_before = count_in_log("does not read: ignored");
    const auto free_the_player = [&] {
        p.send_now(interrupt_channel(), p.pawn_ch);
        fr.rig.run(milliseconds(300));
    };
    // the call alone: nothing starts, nothing is sent, and the log says once what it was
    size_t from = p.got.size();
    p.send_now(call, p.pc_ch);
    CHECK(p.s->foliage_started() == 0 && pc_call_names_since(p, from).empty() && count_in_log("does not read: ignored") == unread_before, "the call alone: read and skipped, nothing starts, nothing is sent, no 'does not read'");
    CHECK(count_in_log("names by path (\"UnmappedComponent_0\", outer 17025330/60/") == 1, "the log says what it was: the path it names and the outer's GUID");
    CHECK(count_in_log("the bunch is reliable, 550 bits, and starts 0400000000000000000000000000000014c8240f04000000") == 1,
          "and whether the bunch was reliable, its size, and its bytes: the 24 the real client's log held come out the same");
    // the call, then the start, in one bunch (the order a client's frame wrote them in)
    from = p.got.size();
    p.send_now(joined({&call, &start}), p.pc_ch);
    CHECK(p.s->foliage_started() == 1 && p.s->cutting_instance() == row && fr.f().store().locked_by(row) != 0,
          "the call and the start in one bunch: the start reaches the foliage handler, this player cuts the Oak (before: the whole bunch was dropped, the start with it)");
    CHECK(count_in_log("does not read: ignored") == unread_before, "no 'does not read' line for that bunch");
    CHECK(count_in_log("names by path (\"UnmappedComponent_0\"") == 1, "and the name is said once, not again for the same call");
    CHECK(count_in_log("held 1 calls named by path and 1 others (ServerStartInteractionWithFoliage)") == 1, "a bunch that shares the call with another is said, with the other's name (the next run's log shows whether a start ever does)");
    fr.rig.run(milliseconds(160));
    const auto set = foliage_data_seen(p, from);
    CHECK(set.size() == 1 && set[0].duration > 14.0f && set[0].duration < 15.0f && set[0].owner == fid::kOwner && set[0].component == fid::kFismc0 && set[0].state == 2,
          "116 ms after, the client is told FoliageData: the channel runs (14.7 s, the Oak, Adult)");
    // a second start while the channel runs, with a call in front: refused as 'already channeling' and NOT answered with the interrupt (it would end the running channel on the client)
    const size_t sent_before = p.s->foliage_interrupts_sent();
    from = p.got.size();
    p.send_now(joined({&call, &start}), p.pc_ch);
    CHECK(p.s->foliage_refusals().count("already channeling") == 1 && p.s->foliage_interrupts_sent() == sent_before && pc_call_names_since(p, from).empty(),
          "a second start while the channel runs: refused (already channeling), and the client's running channel is left alone (no interrupt)");
    free_the_player();
    CHECK(p.s->cutting_instance() == PlayerSession::kNoInstance && fr.f().store().locked_by(row) == 0, "(the client ends the channel)");
    // the start, then the call
    p.send_now(joined({&start, &call}), p.pc_ch);
    CHECK(p.s->foliage_started() == 2 && p.s->cutting_instance() == row, "the start, then the call: the start reaches the handler");
    free_the_player();
    // thirteen calls (a 7150-bit bunch: the client's timer fires once per interval a long frame spans) and the start behind them
    BunchWriter many;
    for (int i = 0; i < 13; ++i) many.write_bit_range(call.data(), 0, call.bit_pos());
    CHECK(many.bit_pos() == 7150, "(thirteen of the calls are 7150 bits, a size the logs had)");
    p.send_now(joined({&many, &start}), p.pc_ch);
    CHECK(p.s->foliage_started() == 3 && p.s->cutting_instance() == row, "thirteen calls and the start behind them: the start reaches the handler");
    CHECK(count_in_log("does not read: ignored") == unread_before && count_in_log("names by path (\"UnmappedComponent_0\"") == 1, "still no 'does not read', and the name is still said once");
    free_the_player();
    // every refusal gives the client its gathering back: the client set its own FoliageData with the start, a clear of ours is an empty diff, the interrupt is what clears it
    from = p.got.size();
    const size_t callouts_before = p.s->bootstrap().callouts();
    p.send_now(foliage_start(fid::kOwner, fid::kFismc0, 999, 2), p.pc_ch);
    fr.rig.run(milliseconds(200));
    const auto after_refusal = pc_call_names_since(p, from);
    CHECK(p.s->foliage_refusals().count("unknown instance") == 1 && std::count(after_refusal.begin(), after_refusal.end(), "ClientInterruptInteractionWithFoliage") == 1 && after_refusal.size() == 1 &&
              p.s->bootstrap().callouts() == callouts_before,
          "a refused start (an instance the map does not have): ClientInterruptInteractionWithFoliage, once, and no callout (before: silent, the client stayed locked)");
    // a cutting worldd ends in the first 116 ms, before it ever told the client FoliageData (the tool taken off, here): the client set its own, so the interrupt goes (no callout: no reason to give)
    from = p.got.size();
    p.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 2), p.pc_ch);
    CHECK(p.s->cutting_instance() == fr.oak_row() && p.s->foliage_started() == 4, "(a fourth cutting starts)");
    p.send_now(item_used(4, 29, guid_n(129)), p.pawn_ch);
    fr.rig.run(milliseconds(300));
    const auto after_cancel = pc_call_names_since(p, from);
    CHECK(!p.s->inventory().at(Worn, 29) && p.s->cutting_instance() == PlayerSession::kNoInstance && p.s->foliage_cancelled() >= 1 && foliage_data_seen(p, from).empty() &&
              std::count(after_cancel.begin(), after_cancel.end(), "ClientInterruptInteractionWithFoliage") == 1 && p.s->bootstrap().callouts() == callouts_before,
          "the axe taken off within 116 ms of the start: no FoliageData was ever sent, so the client is told with the interrupt, once, and no callout");
    // a bunch that does not read at all (garbage) takes every call in it away, so none goes unseen: the first three of a session are said whole, and so is the first of every other KIND
    // (its size and first bytes) after the budget of three -- the fourth of a kind is not said at info
    const size_t unread_mid = count_in_log("does not read: ignored");
    const auto garbage = [](int words) {
        BunchWriter g;
        for (int i = 0; i < words; ++i) g.write_bits(0xFFFFFFFFu, 32);
        return g;
    };
    for (int i = 0; i < 4; ++i) p.send_now(garbage(12), p.pc_ch);
    CHECK(count_in_log("does not read: ignored") == unread_mid + 3, "garbage that does not read: the first three of a session are said (with their bytes), the fourth of the same kind is not");
    p.send_now(garbage(13), p.pc_ch);
    CHECK(count_in_log("does not read: ignored") == unread_mid + 4, "and a bunch of another kind (its size) is said even after the budget of three: a new kind is not hidden by the old ones");
    // the summary line of the session's end counts what the reader skipped
    p.s->finish("the test ends the session");
    CHECK(count_in_log("17 calls on subobjects it names by path (skipped, in 5 bunches, 1 names)") == 1, "the session's summary says how many calls on subobjects named by path were skipped, in how many bunches, with how many names");
    CHECK(count_in_log("the client's calls by name (") == 1 && count_in_log("ServerStartInteractionWithFoliage 6") == 1,
          "and the client's calls by name: the six starts that reached the handler are counted (a start worldd never logged is told from one that never arrived)");
}

/// The first item of the CalloutQueueReplication update the client was told in the chains `got[from..]`.
static std::optional<repl::ElementState> callout_item_seen(const Player& p, size_t from) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const auto layout = repl::fast_array_layout(pc, "CalloutQueueReplication");
    for (size_t i = from; layout && i < p.got.size(); ++i) {
        if (p.got[i].channel != p.pc_ch || p.got[i].open || p.got[i].close) continue;
        const auto blocks = repl::read_content_blocks(p.got[i].content.data(), p.got[i].content.bit_pos(), 0);
        if (!blocks) continue;
        for (const auto& b : *blocks) {
            if (!b.header.is_actor || b.header.has_rep_layout) continue;
            size_t pos = b.payload_pos;
            const auto fields = repl::read_fast_array_fields(pc, p.got[i].content.data(), b.payload_pos + b.payload_bits, pos);
            if (!fields) continue;
            for (const repl::FastArrayField& f : *fields) {
                if (f.field == layout->field && !f.delta.changed.empty()) return f.delta.changed[0].values;
            }
        }
    }
    return std::nullopt;
}
// C-O-A - C Y B

static void test_moving() {
    std::printf("a start while walking: refused with the callout and the interrupt [R: capture 1464]\n");
    FRig fr;
    if (!fr.open()) return;
    Player& p = join_at_the_oak(fr, "bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb");
    const size_t from = p.got.size();
    // a walk: the client holds a direction and its location changes
    float ts = 5.0f;
    p.client_sends({p.client.actor(p.pawn_ch, client_move(ts, fid::kStandX, fid::kStandY, fid::kStandZ, true), true)});
    fr.rig.run(milliseconds(40));
    p.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 2), p.pc_ch);
    CHECK(p.s->foliage_refusals().count("moving") == 1 && p.s->cutting_instance() == PlayerSession::kNoInstance && fr.f().store().locks() == 0, "a start 40 ms after a walking move is refused as moving");
    fr.rig.run(milliseconds(200));
    const auto names = pc_call_names_since(p, from);
    CHECK(std::count(names.begin(), names.end(), "ClientInterruptInteractionWithFoliage") == 1, "ClientInterruptInteractionWithFoliage was sent, once");
    CHECK(p.s->foliage_interrupts_sent() == 1 && p.s->bootstrap().callouts() == 1, "and a callout (Economy / GatheringInterruption_Movement) is shown");
    CHECK(foliage_data_seen(p, from).empty(), "no FoliageData was set [R: 1464]");
    CHECK(count_in_log("(moving)") >= 1, "the log says why");
    {   // the callout is the retail server's item of capture 1464 packet 191 in every field but the time and the key
        const auto mine = callout_item_seen(p, from);
        std::optional<repl::ElementState> golden;
        for (const capture_mount::Callout& c : capture_mount::kCallouts) {
            if (c.capture != 1464 || c.packet != 191) continue;
            const capture_mount::Payload& pay = capture_mount::kPayloads[c.payload];
            const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
            const auto o = repl::load_object_state_hex(pc, pay.hex, pay.bits, c.rep);
            const auto layout = repl::fast_array_layout(pc, "CalloutQueueReplication");
            const repl::FastArrayState* fa = o && layout ? o->arrays.find(layout->field) : nullptr;
            if (fa && !fa->items().empty()) golden = fa->items()[0].values;
        }
        bool same = mine && golden && mine->values.size() == golden->values.size();
        std::string differs;
        for (const auto& [handle, value] : golden ? golden->values : std::map<uint16_t, repl::Value>()) {
            if (handle == 21 || handle >= 25) continue;   // CreationTime and the item's key (a random GUID)
            const auto it = mine ? mine->values.find(handle) : decltype(mine->values.end()){};
            if (!mine || it == mine->values.end() || !(it->second == value)) {
                same = false;
                differs += " " + std::to_string(handle) + (value.get_if<uint64_t>() ? "(retail " + std::to_string(*value.get_if<uint64_t>()) + ")" : std::string());
            }
        }
        CHECK(same, ("the callout item is capture 1464's in every field but the time and the key (message empty, Economy / GatheringInterruption_Movement, type 4, point size 32)" + (differs.empty() ? std::string() : "; differs in handles" + differs)).c_str());
    }
    // the callout goes away 3 s later [R: 1464 packet 397]
    fr.rig.run(seconds(4));
    CHECK(p.s->bootstrap().callouts() == 0, "the callout is taken off after 3 s");
    // standing still, the retry runs [R: 1464's second start, 650 ms after the last move]
    ts += 1.0f;
    p.client_sends({p.client.actor(p.pawn_ch, client_move(ts, fid::kStandX, fid::kStandY, fid::kStandZ, false), true)});
    fr.rig.run(milliseconds(700));
    p.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 2), p.pc_ch);
    CHECK(p.s->cutting_instance() == fr.oak_row() && p.s->foliage_started() == 1, "700 ms after the last walking move the start is accepted");
}

static void test_cancels() {
    std::printf("a cutting that ends with no reward: the instance free at once, FoliageData cleared, nothing granted\n");
    FRig fr;
    if (!fr.open()) return;
    Player& p = join_at_the_oak(fr, "bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb");
    const uint32_t row = fr.oak_row();
    const auto started = [&] {
        p.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 2), p.pc_ch);
        fr.rig.run(milliseconds(300));
        return p.s->cutting_instance() == row && fr.f().store().locked_by(row) != 0;
    };
    const auto nothing_granted = [&](size_t bag_before, int64_t xp_before) {
        return p.s->inventory().count(R0) == bag_before && p.s->professions().xp(fid::kLumberjacking) == xp_before && fr.f().store().state_of(row) == foliage::kAdult && p.s->foliage_completed() == 0;
    };
    const size_t bag0 = p.s->inventory().count(R0);
    const int64_t xp0 = p.s->professions().xp(fid::kLumberjacking);
    // the client's own interrupt
    size_t from = p.got.size();
    CHECK(started(), "a cutting starts");
    p.send_now(interrupt_channel(), p.pawn_ch);
    CHECK(p.s->cutting_instance() == PlayerSession::kNoInstance && fr.f().store().locked_by(row) == 0 && p.s->foliage_cancelled() == 1, "ServerInterruptChannel: the cutting ends and the lock is released");
    fr.rig.run(milliseconds(300));
    auto fd = foliage_data_seen(p, from);
    CHECK(fd.size() == 2 && fd[0].duration > 0 && fd[1].duration == 0.0f && fd[1].state == 3, "FoliageData was cleared");
    CHECK(p.s->foliage_interrupts_sent() == 0, "nothing is said to the client of its own interrupt");
    fr.rig.run(seconds(16));
    CHECK(nothing_granted(bag0, xp0), "and nothing comes when the time would have been up");
    // walking away: a step of 70 cm
    from = p.got.size();
    CHECK(started(), "a cutting starts again");
    p.s->gm_teleport(p.conn, fid::kStandX + 70, fid::kStandY, fid::kStandZ, fr.rig.t, "the test: a step away");
    fr.rig.run(milliseconds(300));
    CHECK(p.s->cutting_instance() == PlayerSession::kNoInstance && fr.f().store().locked_by(row) == 0 && p.s->foliage_cancelled() == 2, "70 cm from where it began: the cutting ends");
    const auto names = pc_call_names_since(p, from);
    CHECK(std::count(names.begin(), names.end(), "ClientInterruptInteractionWithFoliage") == 1 && p.s->foliage_interrupts_sent() == 1, "the client is told with the interrupt and the Movement callout, as a refused start is");
    CHECK(count_in_log("the player moved 70 cm since the channel began (a cutting needs standing still") == 1, "the log says why");
    fr.rig.run(seconds(16));
    CHECK(nothing_granted(bag0, xp0), "no reward");
    // the axe taken off
    p.s->gm_teleport(p.conn, fid::kStandX, fid::kStandY, fid::kStandZ, fr.rig.t, "the test: back");
    fr.rig.run(milliseconds(400));
    CHECK(started(), "a cutting starts a third time");
    p.send_now(item_used(4, 29, guid_n(129)), p.pawn_ch);
    fr.rig.run(milliseconds(100));
    CHECK(!p.s->inventory().at(Worn, 29) && p.s->cutting_instance() == PlayerSession::kNoInstance && fr.f().store().locked_by(row) == 0, "the axe is taken off: the cutting ends");
    fr.rig.run(seconds(16));
    CHECK(p.s->foliage_completed() == 0 && p.s->professions().xp(fid::kLumberjacking) == xp0 && fr.f().store().state_of(row) == foliage::kAdult, "...with no reward");
    // put the axe back (it is in the Backpack), then a death
    int axe_slot = -1;
    for (int slot = 0; slot < items::kBackpackSlots; ++slot) {
        const svc::InventoryItem* it = p.s->inventory().at(Bag, slot);
        if (it && it->instance_guid == guid_n(129)) axe_slot = slot;
    }
    CHECK(axe_slot >= 0, "the axe is in the bag");
    if (axe_slot >= 0) p.send_now(item_used(3, axe_slot, guid_n(129)), p.pawn_ch);
    fr.rig.run(milliseconds(200));
    CHECK(p.s->inventory().at(Worn, 29) != nullptr, "the axe is worn again");
    from = p.got.size();
    CHECK(started(), "a cutting starts a fourth time");
    p.s->take_damage(p.conn, 1.0e9, fr.rig.t);
    fr.rig.run(milliseconds(300));
    CHECK(p.s->dead() && p.s->cutting_instance() == PlayerSession::kNoInstance && fr.f().store().locked_by(row) == 0, "the player dies: the cutting ends and the instance is free");
    const auto after_death = pc_call_names_since(p, from);
    CHECK(std::count(after_death.begin(), after_death.end(), "ClientInterruptInteractionWithFoliage") == 1, "(the death is told as the Death interruption)");
    fr.rig.run(seconds(16));
    CHECK(p.s->foliage_completed() == 0 && fr.f().store().state_of(row) == foliage::kAdult, "...with no reward");
    // a disconnect
    Player& q = join_at_the_oak(fr, "0000000000000000000000000000cc01", "carol");
    q.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 2), q.pc_ch);
    fr.rig.run(milliseconds(300));
    CHECK(q.s->cutting_instance() == row && fr.f().store().locked_by(row) != 0, "a second player cuts the tree");
    q.s->finish("the test ends the session");
    CHECK(fr.f().store().locked_by(row) == 0, "the session ends mid-cutting: the instance is free at once");
    fr.rig.run(seconds(16));
    CHECK(q.s->foliage_completed() == 0 && fr.f().store().state_of(row) == foliage::kAdult, "...and nothing is granted");
}

static void test_one_cutter() {
    std::printf("two players on one tree: the second is refused until the first is done\n");
    FRig fr;
    if (!fr.open()) return;
    Player& a = join_at_the_oak(fr, "bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb");
    Player& b = join_at_the_oak(fr, "0000000000000000000000000000cc01", "carol");
    const uint32_t row = fr.oak_row();
    a.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 2), a.pc_ch);
    CHECK(a.s->cutting_instance() == row, "the first player cuts the tree");
    fr.rig.run(milliseconds(300));
    b.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 2), b.pc_ch);
    CHECK(b.s->cutting_instance() == PlayerSession::kNoInstance && b.s->foliage_refusals().count("instance busy") == 1, "the second is refused: instance busy");
    CHECK(fr.f().store().locked_by(row) != 0 && a.s->cutting_instance() == row, "the first goes on");
    a.send_now(interrupt_channel(), a.pawn_ch);
    b.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 2), b.pc_ch);
    CHECK(b.s->cutting_instance() == row, "the first player stops: the second can start");
    // the same player twice
    b.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 2), b.pc_ch);
    CHECK(b.s->foliage_started() == 1 && b.s->foliage_refusals().count("already channeling") == 1, "a replayed start of the same player changes nothing");
}
// -- C | Y | B x CoA --

/// Review A1 of Gathering 2+3 (2026-10-16): a tree's start that reaches the server while this player's NODE channel runs (a HUNT is the same channel, `gather_.node`: begin_channel is
/// shared).  The client sets its own FoliageData the moment it sends the start, and nothing on its way from the key to the RPC reads ChannelingData, a montage or an interact state (the exe's
/// AAoCPlayerController::CanInteractWithFoliage checks FoliageData, standing still, alive, on foot, not crowd-controlled, the requirements and the geometry), so a refusal that says nothing
/// leaves every later tree answering "Unable to interact with Item while gathering!" until the relog.  Not the player's own TREE channel (test_start_beside_an_unnamed_call): there the
/// client's FoliageData is the running channel's and the interrupt would end it on the client alone.
static void test_a_tree_start_during_a_node_channel() {
    std::printf("a tree's start while a NODE channel runs (review A1): refused, the client told to end its own FoliageData, the node untouched, a tree cut after it\n");
    FRig fr;
    if (!fr.open()) {
        ++g_fail;   // (a setup that fails must fail the test: the older helpers only print)
        return;
    }
    // a Daffodil 1 m from the Oak's cutter and at the cutter's height, put in before the login as the gather tests do (the client has it open from the start)
    const uint64_t node = fr.rig.add_node(id::kDaffodil, fid::kStandX + 100.0 - kCx, fid::kStandY - kCy, 41, fid::kStandZ);
    CHECK(node != 0, "a Daffodil node stands 1 m from the player (the Oak is 1.5 m away)");
    if (node == 0) return;
    Player& p = join_at_the_oak(fr, "bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb");
    const uint32_t row = fr.oak_row();
    p.send_now(interact(fr.rig.component_of(node)), p.pawn_ch);
    CHECK(p.s->harvests_started() == 1 && fr.rig.board().state_of(node) == gather::NodeState::Channeling && p.s->cutting_instance() == PlayerSession::kNoInstance,
          "the node's channel runs (the pawn's ChannelingData; FoliageData is the trees' and is not set)");
    if (p.s->harvests_started() != 1) return;
    const gather::Channel before = *fr.rig.board().channel_of(node);
    fr.rig.run(milliseconds(400));
    // the player turns to the Oak and presses the key: the client's own FoliageData is set with the start
    const size_t from = p.got.size();
    p.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 2), p.pc_ch);
    const auto names = pc_call_names_since(p, from);
    CHECK(p.s->foliage_refusals().count("already channeling") == 1 && p.s->foliage_started() == 0, "the start is refused: already channeling");
    CHECK(names.size() == 1 && names[0] == "ClientInterruptInteractionWithFoliage" && p.s->foliage_interrupts_sent() == 1,
          "the client is told to end its own FoliageData: exactly one ClientInterruptInteractionWithFoliage, and nothing else on the PlayerController");
    const gather::Channel* during = fr.rig.board().channel_of(node);
    CHECK(during && during->started == before.started && during->completes == before.completes && fr.rig.board().state_of(node) == gather::NodeState::Channeling &&
              p.s->harvests_started() == 1 && p.s->harvests_cancelled() == 0,
          "the node's channel is untouched: the same clock, not cancelled");
    CHECK(channeling_seen(p, from).empty(), "the pawn's ChannelingData is not touched: the node's bar goes on");
    CHECK(p.s->cutting_instance() == PlayerSession::kNoInstance && fr.f().store().locks() == 0 && fr.f().store().state_of(row) == foliage::kAdult && foliage_data_seen(p, from).empty(),
          "the Oak is untouched: nobody cuts it, no lock, no FoliageData");
    // the node finishes by the clock, once; a tree's start after it is accepted
    CHECK(run_until(fr.rig, [&] { return p.s->harvests_completed() == 1; }, seconds(30)), "the node's channel completes by the clock");
    fr.rig.run(milliseconds(500));
    CHECK(p.s->harvests_completed() == 1 && p.s->harvests_cancelled() == 0 && p.s->inventory().count(R0) > 0, "and its harvest is granted (the unlock did not end it)");
    const size_t interrupts_before = p.s->foliage_interrupts_sent();
    p.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 2), p.pc_ch);
    CHECK(p.s->cutting_instance() == row && p.s->foliage_started() == 1 && p.s->foliage_refusals().at("already channeling") == 1, "a tree's start after the node is accepted");
    CHECK(p.s->foliage_interrupts_sent() == interrupts_before, "... and not answered with an interrupt");
}

static void test_flood() {
    std::printf("a flood of starts: bounded work, one cutting, the rest counted\n");
    FRig fr;
    if (!fr.open()) return;
    Player& p = join_at_the_oak(fr, "bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb");
    const auto begin = std::chrono::steady_clock::now();
    for (int i = 0; i < 400; ++i) {   // (the reliable window of a connection is 256 bunches: the world runs a little every 50 starts, as a real client's acks would)
        p.client_sends({p.client.actor(p.pc_ch, foliage_start(fid::kOwner, fid::kFismc0, static_cast<uint32_t>(i % 5), 2), true)});
        if (i % 50 == 49) fr.rig.run(milliseconds(40));
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    fr.rig.run(milliseconds(200));
    CHECK(p.s->foliage_started() == 1 && p.s->foliage_refused() == 399, "400 starts in a row: one cutting, 399 refusals (already channeling), each counted");
    char msg[100];
    std::snprintf(msg, sizeof msg, "(400 starts took %.0f ms)", ms);
    CHECK(ms < 2000.0, msg);
    CHECK(fr.f().store().locks() == 1, "one lock is held");
}

static void test_full_bag() {
    std::printf("a full bag: the rest of the logs go to a pile, the tree is cut\n");
    FRig fr;
    if (!fr.open()) return;
    // a character whose Resource0 is full of coal (a filler that is not an Oak log)
    svc::Character rec = record_at("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb", fid::kStandX, fid::kStandY, fid::kStandZ);
    for (int slot = 0; slot < 24; ++slot) rec.items.push_back(make_item(R0, slot, fid::kCoal, 300 + static_cast<uint32_t>(slot), 20, 0));
    Player& p = fr.rig.add_player(std::move(rec));
    play(p);
    std::vector<combat::LootStack> dropped;
    fr.rig.world->set_pile_hook([&](double, double, double, const repl::GuidValue&, const std::string&, const std::vector<combat::LootStack>& stacks) -> uint64_t {
        dropped = stacks;
        return 1;
    });
    const uint32_t row = fr.oak_row();
    const size_t from = p.got.size();
    p.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 2), p.pc_ch);
    CHECK(run_until(fr.rig, [&] { return p.s->foliage_completed() == 1; }, seconds(20)), "the cutting completes");
    fr.rig.run(milliseconds(500));
    const auto names = pc_call_names_since(p, from);
    CHECK(std::count(names.begin(), names.end(), "ClientReceiveStorageEvent") == 0 && std::count(names.begin(), names.end(), "ClientConfirmMultiInteractFoliage") == 1, "no storage event, the confirm all the same");
    CHECK(!dropped.empty() && dropped[0].item == fid::kOakLog && p.s->harvest_piles() == 1, "the logs are in a pile where the player stands");
    CHECK(fr.f().store().state_of(row) == foliage::kEmpty, "the tree is cut");
    CHECK(p.s->bootstrap().callouts() >= 0, "(a callout says why)");
}

static void test_foliage_off() {
    std::printf("a world without foliage: one info line, no cutting\n");
    Rig rig;
    if (!rig.open(g_path)) return;
    Player& p = rig.add_player(record_at("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb", fid::kStandX, fid::kStandY, fid::kStandZ));
    play(p);
    const size_t before = count_in_log("foliage gathering is off in this world");
    p.request(foliage_start(fid::kOwner, fid::kFismc0, 0, 2), p.pc_ch);
    p.request(foliage_start(fid::kOwner, fid::kFismc0, 0, 2), p.pc_ch);
    CHECK(count_in_log("foliage gathering is off in this world") == before + 1, "two interactions with foliage log one line");
    CHECK(count_in_log("cyb: a tree or plant of the world's foliage was interacted with") >= 1, "naming the player and saying what it was");
    CHECK(p.s->foliage_started() == 0 && p.s->cutting_instance() == PlayerSession::kNoInstance, "and no cutting starts");
}

// ---- every cell's state actor reaches the client: what the real client needs ------------------------------------------------------------------
//
//  The user's test 1 (2026-10-12): no tree, plant or stone of the world showed an interact option, because the server sent a cell's state actor only after the cell's first cut and the client
//  drops a foliage hit unless FoliageStateActorLookup[the IFA's FName] holds a state actor whose FoliageStateComponentLookup[the FISMC's FName] has bInitialized (0x1433dbb40, 0x1433dd7b0).
//  Retail sends them at login (capture 1198, packets 244 to 313) and closes each with Dormancy 265 ms later, so that the client keeps it.  The model below is that client: it builds its two
//  lookups from the bits of the chains it was sent and drops an actor on any close but Dormancy's.

using ClientName = std::pair<std::string, int32_t>;
// -- C.O.A & C_Y_B --

static std::optional<ClientName> client_name(const repl::Value* v) {
    const repl::NameValue* n = v ? v->get_if<repl::NameValue>() : nullptr;
    if (!n || n->hardcoded) return std::nullopt;
    return ClientName{n->text, n->number};
}

/// UE's FName(const TCHAR*) for the names the foliage uses (a final "_<digits>" of 1 to 9 digits, no leading zero unless it is the only digit, becomes number = digits + 1), written apart from
/// the code under test.
static ClientName client_fname(const std::string& s) {
    const size_t us = s.rfind('_');
    if (us == std::string::npos || us == 0 || us + 1 >= s.size()) return {s, 0};
    const std::string tail = s.substr(us + 1);
    if (tail.size() > 9 || (tail.size() > 1 && tail[0] == '0') || tail.find_first_not_of("0123456789") != std::string::npos) return {s, 0};
    return {s.substr(0, us), std::stoi(tail) + 1};
}

struct ClientFoliage {
    struct Cell {
        std::map<ClientName, bool> components;                             // FoliageStateComponentLookup: FISMC FName -> bInitialized
        std::map<ClientName, std::map<uint32_t, uint32_t>> states;         // FISMC FName -> (instance index -> the state the client was last told)
    };
    /// One chain that opened a state actor's channel.
    struct Open {
        size_t                chain = 0;
        ClientName            name;
        std::vector<tsupport::Bits> blocks;   // the payload of each block, the actor's first
        size_t                with_states = 0;                           // component blocks with an IndexedStates field
        std::vector<std::pair<uint32_t, uint32_t>> changed;               // (instance index, state) of every item in the chain
        size_t                deleted = 0;
        int                   channel = 0;
    };
    std::map<ClientName, Cell>        lookup;      // FoliageStateActorLookup
    std::map<uint32_t, ClientName>    channels;    // channel -> actor, while it is open
    std::map<uint64_t, std::pair<ClientName, ClientName>> object_names;   // a component's ObjectId -> (its actor, its FISMC), from the opens
    std::vector<Open>                 opens;
    size_t closes_dormancy = 0, closes_destroyed = 0, closes_relevancy = 0, closes_other = 0;
    size_t updates = 0;   // chains on a state actor's open channel that are neither its open nor its close

    void absorb(const Player& p, size_t from);
    bool resolves(const std::string& ifa, const std::string& fismc) const {
        const auto cell = lookup.find(client_fname(ifa));
        if (cell == lookup.end()) return false;
        const auto comp = cell->second.components.find(client_fname(fismc));
        return comp != cell->second.components.end() && comp->second;
    }
    size_t closes() const { return closes_dormancy + closes_destroyed + closes_relevancy + closes_other; }
};

void ClientFoliage::absorb(const Player& p, size_t from) {
    const schema::ClassSchema& ifa = class_named("AIntrepidStateIFA");
    const schema::ClassSchema& comp = class_named("UIntrepidFoliageStateComponent");
    for (size_t i = from; i < p.got.size(); ++i) {
        const fake::Delivered& d = p.got[i];
        const uint32_t ch = d.channel;
        if (d.open) {
            channels.erase(ch);
            const size_t prefix = new_actor_prefix_bits(d.content);
            const auto blocks = prefix ? repl::read_content_blocks(d.content.data(), d.content.bit_pos(), prefix) : std::nullopt;
            if (!blocks || blocks->empty() || !(*blocks)[0].header.is_actor || !(*blocks)[0].header.has_rep_layout) continue;
            size_t pos = (*blocks)[0].payload_pos;
            const auto st = repl::read_rep_block(ifa, d.content.data(), (*blocks)[0].payload_pos + (*blocks)[0].payload_bits, pos);
            const auto name = st ? client_name(st->get(ifa, "FoliageName")) : std::nullopt;
            if (!name) continue;   // not a state actor
            channels[ch] = *name;
            Open open;
            open.chain = i;
            open.name = *name;
            open.channel = static_cast<int>(ch);
            const tsupport::Bits all(d.content.bytes(), d.content.bit_pos());
            for (const auto& b : *blocks) open.blocks.push_back(all.slice(b.payload_pos, b.payload_bits));
            Cell& cell = lookup[*name];
            for (size_t k = 1; k < blocks->size(); ++k) {
                const auto& b = (*blocks)[k];
                if (b.header.is_actor) continue;
                size_t p2 = b.payload_pos;
                const size_t end = b.payload_pos + b.payload_bits;
                std::optional<ClientName> fismc;
                if (b.header.has_rep_layout) {
                    const auto c = repl::read_rep_block(comp, d.content.data(), end, p2);
                    if (!c) continue;
                    fismc = client_name(c->get(comp, "FoliageComponentUUID"));
                    const repl::Value* init = c->get(comp, "bInitialized");
                    if (fismc) {
                        cell.components[*fismc] = init && init->get_if<bool>() && *init->get_if<bool>();
                        object_names[b.header.object.ObjectId] = {*name, *fismc};
                    }
                }
                if (p2 >= end) continue;
                const auto fields = repl::read_fast_array_fields(comp, d.content.data(), end, p2);
                if (!fields) continue;
                ++open.with_states;
                const auto known = object_names.find(b.header.object.ObjectId);
                for (const repl::FastArrayField& f : *fields) {
                    open.deleted += f.delta.deleted.size();
                    for (const auto& item : f.delta.changed) {
                        const auto s1 = item.values.values.find(1);
                        const auto key = item.values.values.find(2);
                        if (s1 == item.values.values.end() || key == item.values.values.end() || !s1->second.get_if<uint64_t>() || !key->second.get_if<uint64_t>()) continue;
                        open.changed.emplace_back(static_cast<uint32_t>(*key->second.get_if<uint64_t>()), static_cast<uint32_t>(*s1->second.get_if<uint64_t>()));
                        if (known != object_names.end()) cell.states[known->second.second][static_cast<uint32_t>(*key->second.get_if<uint64_t>())] = static_cast<uint32_t>(*s1->second.get_if<uint64_t>());
                    }
                }
            }
            opens.push_back(std::move(open));
        }
        else if (!d.close && channels.count(ch)) ++updates;
        if (d.close) {
            const auto held = channels.find(ch);
            if (held == channels.end()) continue;
            switch (d.close_reason) {
                case 1: ++closes_dormancy; break;   // the client keeps the actor
                case 0: ++closes_destroyed; lookup.erase(held->second); break;
                case 3: ++closes_relevancy; lookup.erase(held->second); break;
                default: ++closes_other; lookup.erase(held->second); break;
            }
            channels.erase(held);
        }
    }
}

struct Spot {
    const char* what;
    double x, y, z;
    const char* def;
};

/// The row of `def` nearest to (x, y) on the plane.
static std::optional<uint32_t> nearest_row(const foliage::FoliageMap& map, const Spot& s, double* distance) {
    std::optional<uint32_t> best;
    double least = 1e18;
    for (uint32_t r = 0; r < map.rows().size(); ++r) {
        const auto& row = map.rows()[r];
        if (map.def_names()[row.def] != s.def) continue;
        const double d = std::hypot(row.x - s.x, row.y - s.y);
        if (d < least) {
            least = d;
            best = r;
        }
    }
    *distance = least;
    return best;
}

static bool client_resolves(const ClientFoliage& client, const foliage::FoliageMap& map, uint32_t row) {
    const auto& r = map.rows()[row];
    return client.resolves(map.owners()[r.owner].name, map.owners()[r.owner].components[r.component]);
}
// (C.Y.B + CoA)

/// The user's test 1: a player at the login spot, then at the Oak, the Daffodil and the Stone he walked up to; the client has the state actor of the cell that holds each instance, with the
/// instance's component initialized, and every close it was sent was a Dormancy close.
static void test_delivery() {
    std::printf("every cell's state actor reaches the player, and the client keeps it (the login spot, then the user's three spots)\n");
    FRig fr;
    if (!fr.open()) return;
    const foliage::FoliageMap& map = fr.f().map();
    const Spot spots[] = {{"Oak", -775619, 617693, 16200, "Tree_Oak_Large_2"}, {"Daffodil", -813562, 616567, 7960, "Plant_Daffodil_1"}, {"Stone", -786101, 621592, 15690, "Stone_Basalt_Medium_1"}};
    std::optional<uint32_t> rows[3];
    bool found = true;
    for (int i = 0; i < 3; ++i) {
        double d = 0;
        rows[i] = nearest_row(map, spots[i], &d);
        found = found && rows[i] && d < 200.0;
    }
    CHECK(found, "the instance nearest each of the user's three spots (within 2 m) is the Oak, the Daffodil and the Stone he named");
    if (!found) return;
    Player& p = fr.rig.add_player(record_at("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb", -777947.0, 617946.0, 15952.05));
    play(p);
    ClientFoliage client;
    client.absorb(p, 0);
    char msg[240];
    std::snprintf(msg, sizeof msg, "at the login spot (-777947, 617946) the client was sent %zu state actors, with %zu components, and closed %zu with Dormancy", client.opens.size(), client.object_names.size(), client.closes_dormancy);
    CHECK(client.opens.size() >= 3 && client.closes_dormancy == client.opens.size(), msg);
    CHECK(client_resolves(client, map, *rows[0]) && client_resolves(client, map, *rows[2]), "the Oak and the Stone, 23 m and 89 m from the login spot, resolve: their cell's actor and the instance's component (initialized)");
    const bool daffodil_at_login = client_resolves(client, map, *rows[1]);
    std::printf("    (the Daffodil's cell, 299 m from the login spot, was %s at login)\n", daffodil_at_login ? "sent" : "not sent");
    size_t uninitialized = 0, components = 0;
    for (const auto& [name, cell] : client.lookup) {
        for (const auto& [comp, init] : cell.components) {
            ++components;
            uninitialized += init ? 0 : 1;
        }
    }
    CHECK(components > 0 && uninitialized == 0, "every component the client holds is initialized");
    // the three spots in turn: teleport, let the view run, and the instance resolves
    for (int i = 0; i < 3; ++i) {
        const size_t from = p.got.size();
        p.s->gm_teleport(p.conn, spots[i].x, spots[i].y, spots[i].z, fr.rig.t, "the test: the user's spot");
        fr.rig.run(seconds(3));
        client.absorb(p, from);
        std::snprintf(msg, sizeof msg, "after a teleport to the %s (%.0f, %.0f, %.0f) its cell's actor is on the client and the instance's component is initialized", spots[i].what, spots[i].x, spots[i].y, spots[i].z);
        CHECK(client_resolves(client, map, *rows[i]), msg);
    }
    // away and back: the client keeps what it was sent
    const size_t far_from = p.got.size();
    p.s->gm_teleport(p.conn, spots[0].x + 600000.0, spots[0].y, spots[0].z, fr.rig.t, "the test: far away");
    fr.rig.run(seconds(4));
    client.absorb(p, far_from);
    CHECK(client_resolves(client, map, *rows[0]) && client_resolves(client, map, *rows[1]) && client_resolves(client, map, *rows[2]), "6 km away the client still has the three cells (a Dormancy close keeps the actor)");
    const size_t back_from = p.got.size();
    p.s->gm_teleport(p.conn, -777947.0, 617946.0, 15952.05, fr.rig.t, "the test: back at the login spot");
    fr.rig.run(seconds(4));
    client.absorb(p, back_from);
    CHECK(client_resolves(client, map, *rows[0]) && client_resolves(client, map, *rows[2]), "back at the login spot the Oak and the Stone resolve");
    fr.rig.run(seconds(1));
    client.absorb(p, p.got.size());
    std::snprintf(msg, sizeof msg, "all %zu opens of state actors were closed with Dormancy (%zu); none with Destroyed (%zu) or Relevancy (%zu)", client.opens.size(), client.closes_dormancy, client.closes_destroyed,
                  client.closes_relevancy);
    CHECK(client.closes_destroyed == 0 && client.closes_relevancy == 0 && client.closes_other == 0 && client.closes_dormancy == client.opens.size() && client.updates == 0, msg);
}

/// A cut on a cell the client already holds (capture 1428: the start at packet 53, the cell's re-open at 901): the cell's actor is opened again on a new channel with the actor block,
/// every component, and the one that changed carrying its IndexedStates; it is the retail re-open's blocks to the bit but the array's keys.
static void test_cut_on_a_held_cell() {
    std::printf("a cut on a cell the client holds: the actor opens again with the change [R: capture 1428 packet 901]\n");
    FRig fr;
    if (!fr.open()) return;
    Player& p = join_at_the_oak(fr, "bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb");
    ClientFoliage client;
    client.absorb(p, 0);
    const ClientName cell_name = client_fname(fid::kOwner);
    CHECK(client.lookup.count(cell_name) == 1 && client.resolves(fid::kOwner, fid::kFismc0), "before the cut the client holds the Oak's cell (sent at login, closed with Dormancy)");
    const size_t held_opens = client.opens.size();
    const size_t from = p.got.size();
    p.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 2), p.pc_ch);
    CHECK(run_until(fr.rig, [&] { return p.s->foliage_completed() == 1; }, seconds(20)), "the Oak is cut");
    fr.rig.run(milliseconds(600));
    client.absorb(p, from);
    CHECK(client.opens.size() == held_opens + 1, "the cut made the server open the cell's actor once more, on a new channel");
    if (client.opens.size() != held_opens + 1) return;
    const ClientFoliage::Open& open = client.opens.back();
    const capture_foliage::IfaOpen* golden = nullptr;
    for (const auto& g : capture_foliage::kIfaOpens) {
        if (g.capture == 1428 && g.packet == 901) golden = &g;
    }
    CHECK(golden != nullptr && std::string(golden->name) == fid::kOwner && golden->component_count == 8, "capture 1428's re-open (packet 901) is the same cell: 8 components");
    if (!golden) return;
    CHECK(open.name == cell_name && open.blocks.size() == 1 + golden->component_count, "the re-open has the actor block and a block for each of the cell's 8 components, as the retail one");
    CHECK(open.with_states == 1 && open.changed.size() == 1 && open.changed[0] == std::make_pair(0u, 3u) && open.deleted == 0, "only one component carries an IndexedStates field: the instance 0 is Empty (3), nothing deleted");
    // the blocks against 1428's: the actor block, the seven untouched components, and the cut component's RepLayout block, bit for bit; its IndexedStates field has the retail size
    size_t same = 0;
    if (open.blocks.size() == 1 + golden->component_count) {
        same += open.blocks[0] == tsupport::bits_of_hex(golden->actor.hex, golden->actor.bits) ? 1 : 0;
        for (uint32_t i = 0; i < golden->component_count; ++i) {
            const capture_foliage::Component& c = golden->components[i];
            const tsupport::Bits& ours = open.blocks[1 + i];
            if (c.has_states) same += ours.size() == c.block.bits && ours.slice(0, 283) == tsupport::bits_of_hex(c.block.hex, 283) ? 1 : 0;
            else same += ours == tsupport::bits_of_hex(c.block.hex, c.block.bits) ? 1 : 0;
        }
    }
    char msg[300];
    std::snprintf(msg, sizeof msg, "the actor block (520 bits), the 7 untouched components (283 bits each) and the cut one (487 bits, its first 283 bits) equal capture 1428's re-open (%zu of 9 blocks)", same);
    CHECK(same == 1 + golden->component_count, msg);
    CHECK(client.lookup[cell_name].states[client_fname(fid::kFismc0)][0] == 3u, "so the client lists the instance as Empty");
    // the actor is closed with Dormancy again
    CHECK(client.closes_destroyed == 0 && client.closes_relevancy == 0 && client.closes_dormancy == client.opens.size(), "and every state actor close so far was a Dormancy close");
    // after the regrowth (x0.01: 39 s) the instance is deleted from the client's list by one more re-open
    const size_t regrow_from = p.got.size();
    fr.rig.run(seconds(40));
    client.absorb(p, regrow_from);
    CHECK(client.opens.size() == held_opens + 4, "the regrowth (Infancy, Juvenile, back to its painted state) opened the cell three more times");
    CHECK(client.opens.size() == held_opens + 4 && client.opens.back().deleted == 1 && client.opens.back().changed.empty() && client.opens.back().with_states == 1, "the last one deletes the instance from the list: it is whole again");
}
// C | O | A - Cyb

static size_t working_set_bytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) return static_cast<size_t>(pmc.WorkingSetSize);
#endif
    return 0;
}

/// What the state actors cost the view: the opens at login and when the last of them was sent, the time of a step with and without them, and what a player who walks through every cell of
/// the map makes the server hold (the shadows the view keeps of the arrays of each cell it has seen) and send.
static void test_view_load() {
    std::printf("the load on the view: 671 state actors, tracked and sent to one player\n");
    double step_us[2] = {0, 0};
    size_t tracked[2] = {0, 0}, opens_at_login[2] = {0, 0};
    double last_open_ms[2] = {0, 0};
    size_t snapshots[2] = {0, 0}, capped[2] = {0, 0};
    size_t tour_cells = 0, tour_components = 0, tour_opens = 0, tour_uninitialized = 0, tour_tracked = 0, tour_dormancy = 0, tour_other_closes = 0;
    double tour_ms = 0, tour_mb = 0, tour_kb_per_open = 0;
    for (int with = 0; with < 2; ++with) {
        FRig fr;
        if (!fr.open(0.01, with == 1)) return;
        Player& p = fr.rig.add_player(record_at("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb", -777947.0, 617946.0, 15952.05));
        fr.rig.settle();
        p.client_sends({p.client.actor(p.pawn_ch, actor_rpc(class_named("PlayerPawn_C"), "ServerClientIsReadyToPhaseIn", {}), true)});   // the pawn is possessed: the world is sent
        ClientFoliage client;
        size_t absorbed = 0;
        const Clock::time_point joined = fr.rig.t;
        double last = 0;
        size_t counted = 0;
        for (int s = 0; s < 300; ++s) {   // the first 6 s after the phase-in: when does the last state actor of the login arrive?
            fr.rig.step();
            client.absorb(p, absorbed);
            absorbed = p.got.size();
            if (client.opens.size() != counted) {
                counted = client.opens.size();
                last = std::chrono::duration<double, std::milli>(fr.rig.t - joined).count();
            }
        }
        opens_at_login[with] = client.opens.size();
        last_open_ms[with] = last;
        fr.rig.settle();
        const auto t0 = std::chrono::steady_clock::now();
        const int steps = 1500;   // 30 s of a standing player: every third step is a relevancy pass
        for (int s = 0; s < steps; ++s) fr.rig.step();
        step_us[with] = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / steps;
        tracked[with] = p.s->actor_view()->tracked();
        snapshots[with] = p.s->actor_view()->stats().snapshots;
        capped[with] = p.s->actor_view()->stats().capped;
        if (with == 1) {   // the tour: a stop at the centre of each cell of the map, 300 ms each (the view passes every 67 ms)
            const foliage::FoliageMap& map = fr.f().map();
            p.got.clear();
            ClientFoliage tourist;
            const size_t ws0 = working_set_bytes();
            const auto t1 = std::chrono::steady_clock::now();
            size_t bits_total = 0;
            for (uint16_t o = 0; o < map.owners().size(); ++o) {
                double cx = 0, cy = 0;
                map.cell_center(o, cx, cy);
                p.s->gm_teleport(p.conn, cx, cy, static_cast<double>(map.owners()[o].centre_z_cm()) + 500.0, fr.rig.t, "the test: the tour");
                fr.rig.run(milliseconds(300));
                tourist.absorb(p, 0);
                for (const fake::Delivered& d : p.got) bits_total += d.content.bit_pos();
                p.got.clear();
            }
            fr.rig.run(seconds(1));   // the last stop's Dormancy closes
            tourist.absorb(p, 0);
            p.got.clear();
            tour_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
            const size_t ws1 = working_set_bytes();
            tour_mb = ws1 > ws0 ? static_cast<double>(ws1 - ws0) / (1024.0 * 1024.0) : 0.0;
            tour_cells = tourist.lookup.size();
            for (const auto& [name, cell] : tourist.lookup) {
                for (const auto& [comp, init] : cell.components) {
                    ++tour_components;
                    tour_uninitialized += init ? 0 : 1;
                }
            }
            tour_opens = tourist.opens.size();
            tour_kb_per_open = tour_opens ? static_cast<double>(bits_total) / 8.0 / 1024.0 / static_cast<double>(tour_opens) : 0.0;
            tour_tracked = p.s->actor_view()->tracked();
            tour_dormancy = tourist.closes_dormancy;
            tour_other_closes = tourist.closes_destroyed + tourist.closes_relevancy + tourist.closes_other;
        }
    }
    std::printf("    without state actors: %.1f us a 20 ms step (%zu actors tracked, %zu snapshots, %zu passes capped by the 16 opens)\n", step_us[0], tracked[0], snapshots[0], capped[0]);
    std::printf("    with 671 state actors: %.1f us a 20 ms step (%zu actors tracked, %zu snapshots, %zu passes capped); %zu state actors sent at login, the last %.0f ms after the phase-in\n", step_us[1],
                tracked[1], snapshots[1], capped[1], opens_at_login[1], last_open_ms[1]);
    std::printf("    a tour of all %zu cells (a stop at each centre): %zu cells and %zu components delivered, %zu opens (%.1f KB each on the wire, exports included), the view tracking %zu at the end; "
                "%.0f ms of real time; the process grew by %.1f MB\n", tour_cells, tour_cells, tour_components, tour_opens, tour_kb_per_open, tour_tracked, tour_ms, tour_mb);
    CHECK(opens_at_login[0] == 0 && opens_at_login[1] >= 3, "the player is sent the state actors of the cells around him at login (none without them)");
    CHECK(last_open_ms[1] < 4000.0, "the last of them arrives within four seconds of the phase-in (retail: 4.1 to 4.7 s after its capture began; the view paces 16 opens a pass)");
    CHECK(step_us[1] < step_us[0] + 2000.0 && step_us[1] < 20000.0, "a standing player's step costs less than 2 ms more with all the cells tracked");
    CHECK(tour_cells == 671 && tour_components == 10613 && tour_uninitialized == 0, "a player who stops in every cell is sent all 671 state actors, 10,613 components, each initialized");
    std::printf("    (the tour: %zu opens, %zu Dormancy closes, %zu other closes)\n", tour_opens, tour_dormancy, tour_other_closes);
    CHECK(tour_other_closes == 0 && tour_dormancy == tour_opens, "... each closed with Dormancy, none destroyed");
    CHECK(tour_mb < 200.0, "and the server holds under 200 MB for him (the shadows of 671 cells and what he was sent)");
}
// [C | O | A + C.Y.B]

/// The login in the real world: the Riverlands' creatures and nodes (spawns::AreaConfig::riverlands(), worldd's default area) in the registry beside the 671 state actors, one player at the
/// login spot.  The view paces its opens (16 a pass, nearest first), so the state actors, 96 to 340 m away, wait behind the creatures nearer than they: when do they arrive?
static void test_login_in_the_real_world() {
    std::printf("the login in the real world: the Riverlands' creatures and nodes beside the 671 state actors\n");
    Rig rig;
    std::string open_error;
    rig.db = content::ContentDb::open(g_path, open_error);
    if (!rig.db) {
        std::printf("  [skip] the content.db does not open: %s\n", open_error.c_str());
        return;
    }
    rig.world = std::make_unique<PlayerWorld>(&*rig.db, "", bs::GameStateMode::Tier2, kServerId, rig.guids);
    rig.world->set_position_sink(&rig.sink);
    std::string error;
    const std::string motion = (fs::path(g_tsv).parent_path() / "npc_motion.tsv").string();
    if (!rig.world->load_blueprints(motion, error)) std::printf("    (no Blueprint table: %s)\n", error.c_str());
    const auto loading = std::chrono::steady_clock::now();
    if (!rig.world->load_area(spawns::AreaConfig::riverlands(), error)) {
        std::printf("  [skip] the Riverlands do not load here: %s\n", error.c_str());
        return;
    }
    const double load_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - loading).count();
    gather::Config gcfg;
    if (!rig.world->start_gathering(gcfg, error)) {
        std::printf("  [FAIL] gathering does not start: %s\n", error.c_str());
        return;
    }
    foliage::Config cfg;
    cfg.map_path = g_tsv;
    cfg.regrow_mult = 0.01;
    if (!rig.world->start_foliage(cfg, error)) {
        std::printf("  [FAIL] foliage does not start: %s\n", error.c_str());
        return;
    }
    const size_t registry_size = rig.world->registry().size();
    const size_t npcs = rig.world->registry().count(world::ActorKind::Npc), nodes = rig.world->registry().count(world::ActorKind::Gatherable);
    Player& p = rig.add_player(record_at("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb", -777947.0, 617946.0, 15952.05));
    rig.settle();
    p.client_sends({p.client.actor(p.pawn_ch, actor_rpc(class_named("PlayerPawn_C"), "ServerClientIsReadyToPhaseIn", {}), true)});
    ClientFoliage client;
    size_t absorbed = 0, counted = 0;
    const Clock::time_point joined = rig.t;
    double first_ms = -1, last_ms = 0;
    for (int s = 0; s < 500; ++s) {   // the first 10 s
        rig.step();
        client.absorb(p, absorbed);
        absorbed = p.got.size();
        if (client.opens.size() != counted) {
            counted = client.opens.size();
            const double ms = std::chrono::duration<double, std::milli>(rig.t - joined).count();
            if (first_ms < 0) first_ms = ms;
            last_ms = ms;
        }
    }
    const auto& st = p.s->actor_view()->stats();
    std::printf("    the area: %zu actors in the registry (%zu creatures, %zu nodes), loaded in %.1f s\n", registry_size, npcs, nodes, load_s);
    std::printf("    in the first 10 s after the phase-in the view opened %zu actors (%zu as snapshots, %zu awake), %zu state actors among them, the first at %.0f ms and the last at %.0f ms; "
                "%zu passes held opens back at the cap of 16, %zu were throttled by the connection\n",
                st.opens + st.snapshots, st.snapshots, st.opens, client.opens.size(), first_ms, last_ms, st.capped, st.throttled);
    const auto t0 = std::chrono::steady_clock::now();
    const int steps = 1500;
    for (int s = 0; s < steps; ++s) rig.step();
    const double step_us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / steps;
    std::printf("    a standing player's 20 ms step takes %.0f us with the view tracking %zu actors (%zu awake, %zu dormant)\n", step_us, p.s->actor_view()->tracked(),
                p.s->actor_view()->count(world::ClientState::Awake), p.s->actor_view()->count(world::ClientState::Dormant));
    CHECK(client.opens.size() >= 3 && last_ms < 4000.0, "the state actors of the cells around the login spot all arrive within four seconds of the phase-in, behind the creatures nearer than they");
    double oak_distance = 0;
    const Spot oak{"Oak", -775619, 617693, 16200, "Tree_Oak_Large_2"};
    const auto oak_row = nearest_row(rig.world->foliage()->map(), oak, &oak_distance);
    CHECK(oak_row && client_resolves(client, rig.world->foliage()->map(), *oak_row), "and the user's Oak, 23 m away, resolves on his client by then");
    CHECK(step_us < 5000.0, "and a step takes under 5 ms");
}

int main() {
    {   // the lines of the log (info and above) go to g_log: the tests read what the user's test would
        auto logger = std::make_shared<spdlog::logger>("test", std::make_shared<spdlog::sinks::ostream_sink_mt>(g_log));
        logger->set_level(spdlog::level::info);
        spdlog::set_default_logger(logger);
    }
    const char* path = db_path();
    if (!path) {
        std::printf("[skip] test_worldd_foliage_session: no content.db (set AOC_CONTENT_DB, or build dist/content/content.db)\n");
        return 0;
    }
#ifdef FOLIAGE_TSV_PATH
    if (fs::exists(FOLIAGE_TSV_PATH)) g_tsv = FOLIAGE_TSV_PATH;
#endif
    if (const char* env = std::getenv("AOC_FOLIAGE_MAP"); env && *env) g_tsv = env;
    if (!g_tsv) {
        std::printf("[skip] test_worldd_foliage_session: no dist/content/region_foliage.tsv\n");
        return 0;
    }
    g_path = path;
    std::printf("content.db: %s\nmap: %s\n", path, g_tsv);
    if (std::getenv("FOLIAGE_ONLY_UNNAMED_CALL")) {   // T1 of the trees' retest 4 alone: a start in the bunch of the client's 550-bit call on a subobject it names by path
        test_start_beside_an_unnamed_call();
        std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
        return g_fail == 0 ? 0 : 1;
    }
    test_delivery();
    test_cut_on_a_held_cell();
    test_view_load();
    test_login_in_the_real_world();
    test_a_harvest();
    test_refusals();
    test_start_beside_an_unnamed_call();
    test_moving();
    test_cancels();
    test_one_cutter();
    test_a_tree_start_during_a_node_channel();
    test_flood();
    test_full_bag();
    test_foliage_off();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
