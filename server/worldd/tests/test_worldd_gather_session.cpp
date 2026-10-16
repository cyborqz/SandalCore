// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_gather_session.cpp  (Gathering stage 2, part "Harvesting")
//
//  The harvest on connections: sessions with fake clients (the harness of test_worldd_area_session) on a PlayerWorld over the real content.db (AOC_CONTENT_DB, else
//  dist/content/content.db of the working tree; "[skip]" without it) whose registry holds resource nodes made from the captured template around the login spot.
//
//  a harvest        the interact on a Daffodil node: the node's channel updates (the status counter +3 every ~100 ms, the done update), 11.25 s after the interact the result
//                   frame in the captured order (ClientUpdateItemDurability, the storage event, the XPTracker with the Herbalist's XP and Adventuring's), the tool one durability
//                   lighter, the bag one stack heavier, the node Gathered and back after the respawn; the progress and the bag are saved; the next harvest works
//  the checks       a refused start is logged and counted and sends nothing: no tool, a Novice at an Apprentice node, an Apprentice node with a T1 tool, out of range, a node the
//                   client does not have open
//  exploits         the interact again while channeling (the channel goes on, once), on a node that is gathered, on a node another player channels (a two-session test), a flood of
//                   interacts, a replayed interact; a client that never cancels (the channel ends by the clock, the rewards once); a disconnect mid-channel, the tool taken off
//                   or swapped, a teleport away, a death, the client's ServerInterruptChannel, a node removed from the world: no reward, the node Available, nothing stuck
//  a full bag       the result has no storage event, a callout says why, and 100 ms after the experience frame an APlayerLootPile holds the stacks
// ============================================================================
#include "capture_gather.h"
#include "content_db_fixture.h"
#include "cyb_appearance.h"
#include "fake_client.h"
#include "bootstrap/character_setup.h"
#include "gather/gathering.h"
#include "items/inventory.h"
#include "net/endpoint.h"
#include "protocol/repl/block_reader.h"
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
// (C.O.A) (Cyb)

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
// [CoA x C-Y-B]

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

    uint64_t add_node(int64_t def_guid, double dx, double dy, uint32_t seed) {
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
        node->z = bs::kDefaultSpawn.z;
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
// C:Y:B / CoA

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

/// ServerStartInteractionWithFoliage, the PlayerController's call for a tree or plant of the world's foliage (Gathering stage 3; the client sends it when F is pressed on one).
static BunchWriter foliage_interact() {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerStartInteractionWithFoliage");
    repl::RpcParams p;
    if (!rpc) return BunchWriter();
    repl::set_rpc_param(*rpc, p, "InteractionIndex", repl::Value::i64(0));
    repl::set_rpc_param(*rpc, p, "OriginalInstanceIndex", repl::Value::i64(12));
    repl::set_rpc_param(*rpc, p, "FoliageName", repl::Value(repl::NameValue::plain("Tree_Oak_Large_2")));
    repl::set_rpc_param(*rpc, p, "FoliageComponentUUID", repl::Value(repl::NameValue::plain("FoliageISMC_0")));
    repl::set_rpc_param(*rpc, p, "FoliageState", repl::Value::u64(0));
    return actor_rpc(pc, "ServerStartInteractionWithFoliage", p);
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
// C-O-A and C | Y | B

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
// (C Y B | CoA)

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

// ---- the tests ----------------------------------------------------------------------------------------------------------------------
// C:Y:B captures, CoA code

static void test_a_harvest(Rig& rig) {
    std::printf("a harvest from the interact to the node's return\n");
    const uint64_t node = rig.add_node(id::kDaffodil, 200, 0, 1);
    Player& p = rig.add_player(record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb"));
    play(p);
    CHECK(p.s->actor_view()->state_of(node) == world::ClientState::Awake, "the node is open on the client's connection");
    CHECK(p.s->professions().valid() && p.s->professions().tier(id::kHerbalism) == 0 && p.s->professions().level(id::kHerbalism) == 1, "the character is a Novice Herbalist, level 1");
    const svc::InventoryItem* sickle = p.s->inventory().at(Worn, 33);
    CHECK(sickle && sickle->durability == 150.0f, "a T1 sickle with 150 durability is worn in slot 33");
    const size_t bag_before = p.s->inventory().count(Bag), resource_before = p.s->inventory().count(R0);
    CHECK(p.s->inventory().grid(R0).cells() == 24, "the Sanctus bag in slot 23 opens Resource0, 6 x 4 = 24 cells");
    const size_t from = p.got.size();
    p.send_now(interact(rig.component_of(node)), p.pawn_ch);
    CHECK(rig.board().state_of(node) == gather::NodeState::Channeling && p.s->harvesting_node() == node && p.s->harvests_started() == 1, "the interact starts the channel: the node is Channeling for this player");
    const NodeSeen first = node_seen(p, node, from);
    CHECK(!first.statuses.empty() && first.statuses[0] == 0x08000001u, "the client was sent the start at once: the status 0x08000001");
    {   // the pawn's ChannelingData: what the client plays the gather animation and shows the bar from [R: 1427 packet 67; ten harvests]
        const auto cd = channeling_seen(p, from);
        const world::WorldActor* n = rig.world->registry().find(node);
        const gather::Channel* ch = rig.board().channel_of(node);
        const double length = ch ? std::chrono::duration<double>(ch->completes - ch->started).count() : 0.0;
        const auto sickle_guid = items::guid_of(guid_n(133));
        CHECK(cd.size() == 1, "the start told the client the pawn's ChannelingData, once (the gather animation, the bar and the client's own cancel come from it)");
        if (cd.size() == 1 && n && sickle_guid) {
            const repl::ChannelingData& d = cd[0].second;
            CHECK(d.target == n->actor.guid && d.flags == 6 && d.active && d.timed, "it names the node and has Flags 6, active (1427: the node 2259212, 6, 1)");
            CHECK(length > 10.0 && std::fabs(static_cast<double>(d.duration) - length) < 0.01, "its duration is the channel's length (11.25 s for a Novice Daffodil)");
            CHECK(d.tool_record == 6064629375589482502ll && d.tool_guid == *sickle_guid, "it names the sickle in the hand: its item record and its instance guid (the one of the durability update)");
            CHECK(d.start_time == std::floor(d.start_time) && std::fabs(d.start_time - bs::effect_clock(world_time_at(rig.t))) < 2.0,
                  "its start is a whole number of seconds on the synchronized clock of the effects (retail's 13788648.0 beside CurrentEffects' 13788648.65)");
        }
    }
    // the channel: 11.25 s at rating 0; the heartbeats every 100 ms, the counter +3 each
    const bool done = run_until(rig, [&] { return rig.board().state_of(node) == gather::NodeState::Gathered; }, seconds(20));
    CHECK(done, "the channel completes by the clock");
    const double elapsed = std::chrono::duration<double>(rig.t - (t0 + milliseconds(0))).count();
    (void)elapsed;
    rig.run(milliseconds(400));
    const NodeSeen seen = node_seen(p, node, from);
    // statuses: the start, then the heartbeats (about 112 of them), then the done update 0x...4
    size_t steps3 = 0;
    for (size_t i = 1; i < seen.statuses.size(); ++i) steps3 += (((seen.statuses[i] >> 27) + 32 - (seen.statuses[i - 1] >> 27)) & 31u) == 3 ? 1 : 0;
    char msg[200];
    std::snprintf(msg, sizeof msg, "the client saw %zu status updates (the start, about 112 heartbeats, the done update); the counter went up by 3 each time (%zu of %zu)", seen.statuses.size(), steps3,
                  seen.statuses.empty() ? 0 : seen.statuses.size() - 1);
    CHECK(seen.statuses.size() > 105 && seen.statuses.size() < 120 && steps3 == seen.statuses.size() - 1, msg);
    CHECK((seen.statuses.back() & 0x07ffffffu) == 4, "and the last is the done update: phase 4");
    CHECK(seen.gathered, "bGathered reached the client");
    // the result frame, in the captured order
    const auto calls = pc_calls_since(p, from);
    const auto names = call_names(calls);
    CHECK(count_calls(calls, "ClientUpdateItemDurability") == 1 && count_calls(calls, "ClientReceiveStorageEvent") == 1, "the result frame: one durability update and one storage event");
    size_t at_durability = 99, at_event = 99;
    for (size_t i = 0; i < names.size(); ++i) {
        if (names[i] == "ClientUpdateItemDurability") at_durability = i;
        if (names[i] == "ClientReceiveStorageEvent") at_event = i;
    }
    CHECK(at_durability < at_event, "the durability update comes before the item (as 1427's packet 673)");
    for (const auto& c : calls) {
        if (c.rpc && std::string(c.rpc->name) == "ClientUpdateItemDurability") {
            const auto u = repl::parse_item_durability(c);
            CHECK(u && u->items.size() == 1 && u->items[0].slot == 33 && u->items[0].durability == 149.0f && u->container == p.s->bootstrap().setup().guids.equipment && items::guid_hex(u->items[0].item) == guid_n(133),
                  "the update names the Equipment, the sickle's instance in slot 33, and 149");
        }
        if (c.rpc && std::string(c.rpc->name) == "ClientReceiveStorageEvent") {
            const auto ev = repl::parse_storage_event(c);
            CHECK(ev && ev->type == static_cast<uint8_t>(repl::StorageEventType::Add) && ev->item.record_id == static_cast<uint64_t>(id::kDaffodilItem) && ev->item.quantity == 3 && ev->item.quality == 1 &&
                      ev->targets.size() == 1 && ev->targets[0].object == p.s->bootstrap().setup().guids.resource_storage[0] && ev->targets[0].slots.size() == 1 &&
                      ev->targets[0].slots[0].slot == 0 && ev->targets[0].slots[0].count == 3 && ev->targets[0].status == repl::kStorageNoError,
                  "the storage event adds Resource_Raw_Plant_Daffodil x3 q1 to the Resource container at slot 0 (1427's: Equipment + 8, slot 0, status 14)");
            // the pick-up popup (the fix after the user's retest 2): the Add carries the FCalloutQueueInfo retail's does, with 1427's text word for word, the stored item, and type 6 / size 36 / category 141
            CHECK(ev && ev->callout && ev->callout->message == "Acquired <a id=\"Item\" style=\"Item_Common\" metaType=\"ItemLink\" metaGUID=\"6064629184431849472\" metaItemQuality=\"1\" metaEnchantLevel=\"0\" metaTemperingPercent=\"0\" metaDurability=\"0\" metaSockets=\"\"> [Daffodil] </>x3" && ev->callout->formatted_on_screen == ev->callout->message &&
                      ev->callout->item == ev->item && ev->callout->callout_type == 6 && ev->callout->point_size == 36 && ev->callout->chat_category == 141 && ev->callout->creation_time.ticks > 600000000000000000ll,
                  "and carries the pick-up popup's callout: 1427's text (Acquired <a ...> [Daffodil] </>x3), the stored item for the icon, type 6, size 36, category 141, and the time");
        }
    }
    {   // the result clears the pawn's ChannelingData, once, between the items and the XPTracker [R: 1427 packets 673 and 674: the clear is in the packet of the XPTracker, 2 ms after the frame]
        const auto cd = channeling_seen(p, from);
        CHECK(cd.size() == 2 && cd[1].second.flags == 0x106 && !cd[1].second.active && !cd[1].second.timed && cd[0].second.target == cd[1].second.target,
              "the result cleared the pawn's ChannelingData, once, naming the same node (Flags 0x106, nothing else)");
        const auto calls_at = pc_chain_calls(p, from);
        size_t at_dur = static_cast<size_t>(-1), at_ev = static_cast<size_t>(-1);
        for (const auto& [chain, name] : calls_at) {
            if (name == "ClientUpdateItemDurability") at_dur = chain;
            if (name == "ClientReceiveStorageEvent") at_ev = chain;
        }
        const size_t at_xp = xp_chain_since(p, at_dur == static_cast<size_t>(-1) ? from : at_dur);
        CHECK(cd.size() == 2 && at_ev < cd[1].first && cd[1].first < at_xp, "the clear comes after the storage event and before the XPTracker's chain (retail's pawn block precedes the PlayerController's in that packet)");
    }
    CHECK(p.s->inventory().count(Bag) == bag_before && p.s->inventory().count(R0) == resource_before + 1 && p.s->inventory().at(R0, 0) &&
              p.s->inventory().at(R0, 0)->item_record_id == id::kDaffodilItem && p.s->inventory().at(R0, 0)->quantity == 3,
          "the Backpack is untouched; Resource0 holds the daffodils, one stack of 3 at slot 0");
    const svc::InventoryItem* tool = p.s->inventory().at(Worn, 33);
    CHECK(tool && tool->durability == 149.0f, "the sickle has 149");
    // the experience
    Tracker tracker;
    tracker.absorb(p, from);
    const auto herbalist = tracker.state.experience(leveling_id(id::kLtHerbalist));
    CHECK(herbalist && *herbalist == 80, "the XPTracker: the Herbalist has 80 XP (1427's +80)");
    CHECK(p.s->professions().xp(id::kHerbalism) == 80 && p.s->professions().level(id::kHerbalism) == 1, "the session's professions: 80 XP, still level 1 (the curve's level 2 is at 1,600)");
    CHECK(p.s->progress().xp > 0 || p.s->progress().debt >= 0, "and Adventuring's experience was offered (the debt first)");
    CHECK(p.s->harvests_completed() == 1 && p.s->harvesting_node() == 0, "one harvest completed; the session harvests nothing now");
    // saved
    bool saved_xp = false, saved_bag = false;
    for (const auto& pr : rig.sink.progress) {
        for (const svc::ProfessionState& ps : pr.progress.professions) saved_xp = saved_xp || (ps.xp == 80);
    }
    for (const auto& inv : rig.sink.inventories) {
        for (const svc::InventoryItem& it : inv.second) saved_bag = saved_bag || (it.item_record_id == id::kDaffodilItem && it.container == R0);
    }
    CHECK(saved_xp && saved_bag, "the progress (the Herbalist's 80 XP) and the inventory (the Daffodil in Resource0) were handed to the saver");
    // the node: Gathered, then back after the respawn (14,400 s x 0.001 = 14.4 s)
    CHECK(rig.board().state_of(node) == gather::NodeState::Gathered, "the node is Gathered");
    const size_t chains_before = p.got.size();
    CHECK(run_until(rig, [&] { return rig.board().state_of(node) == gather::NodeState::Available; }, seconds(20)), "it is Available again after the respawn (the data's 14,400 s x 0.001)");
    rig.run(milliseconds(300));
    const NodeSeen back = node_seen(p, node, chains_before);
    CHECK(!back.statuses.empty() && back.statuses.back() == 0, "the client was sent the reset: the status 0");
    CHECK(can_input_ok(back, true), "the respawn told the client CanInputActivate with four entries, the first true (an empty array crashed the client in test 1)");
    // and again
    const size_t second_from = p.got.size();
    p.send_now(interact(rig.component_of(node)), p.pawn_ch);
    CHECK(rig.board().state_of(node) == gather::NodeState::Channeling && p.s->harvests_started() == 2, "the node can be harvested again");
    run_until(rig, [&] { return p.s->harvests_completed() == 2; }, seconds(20));
    Tracker second;
    second.absorb(p, from);
    const auto h2 = second.state.experience(leveling_id(id::kLtHerbalist));
    CHECK(p.s->harvests_completed() == 2 && h2 && *h2 == 160 && p.s->inventory().at(Worn, 33)->durability == 148.0f, "the second harvest: 160 XP in all, the sickle at 148");
    CHECK(p.s->inventory().count(R0) == 1 && p.s->inventory().at(R0, 0) && p.s->inventory().at(R0, 0)->quantity == 6, "...and the second daffodils (x3 q1) topped the stack up to 6: one stack in Resource0");
    (void)second_from;
}
// (C-O-A + C Y B)

// ---- helpers for the next tests --------------------------------------------------------------------------------------------------------

namespace id2 {
inline constexpr int64_t kT3Sickle = 6064629376480313362ll;      // Gear_Artisan_Herbalism_Tool_Sickle_T3_0
inline constexpr int64_t kT1Sickle = 6064629375589482502ll;
inline constexpr int64_t kPotion = fi::kPotion;                  // a made-up record the real file does not know: a bag filler
}  // namespace id2

static svc::ProfessionState prof(int64_t guid, int tier, int level, int64_t xp) {
    svc::ProfessionState s;
    s.profession_guid = guid;
    s.certification_tier = tier;
    s.level = level;
    s.xp = xp;
    return s;
}

/// The refusals by kind, as a short text for a failure message.
static std::string refusals_of(const Player& p) {
    std::string out;
    for (const auto& [kind, n] : p.s->harvest_refusals()) out += (out.empty() ? "" : ", ") + kind + " x" + std::to_string(n);
    return out.empty() ? "none" : out;
}

static size_t refusal_count(const Player& p, const char* kind) {
    const auto it = p.s->harvest_refusals().find(kind);
    return it == p.s->harvest_refusals().end() ? 0 : it->second;
}

/// The pawn's decoded calls among the chains `got[from..]`.
static std::vector<repl::RpcCall> pawn_calls_since(const Player& p, size_t from) {
    std::vector<repl::RpcCall> out;
    for (size_t i = from; i < p.got.size(); ++i) {
        if (p.got[i].channel != p.pawn_ch || p.got[i].open || p.got[i].close) continue;
        const auto blocks = repl::read_content_blocks(p.got[i].content.data(), p.got[i].content.bit_pos(), 0);
        if (!blocks) continue;
        for (const auto& b : *blocks) {
            if (!b.header.is_actor || b.header.has_rep_layout) continue;
            size_t pos = b.payload_pos;
            auto calls = repl::read_rpc_fields(class_named("PlayerPawn_C"), p.got[i].content.data(), b.payload_pos + b.payload_bits, pos);
            if (calls) {
                for (auto& c : *calls) out.push_back(std::move(c));
            }
        }
    }
    return out;
}

struct Snap {
    size_t bag = 0, completed = 0, cancelled = 0, started = 0;
    float  durability = 0;
    int64_t xp = 0;
};
static Snap snap(const Player& p, int slot = 33, int64_t profession = id::kHerbalism) {
    Snap s;
    s.bag = p.s->inventory().count(Bag) + p.s->inventory().count(R0) + p.s->inventory().count(ItemContainer::Resource1) + p.s->inventory().count(ItemContainer::Resource2);
    s.completed = p.s->harvests_completed();
    s.cancelled = p.s->harvests_cancelled();
    s.started = p.s->harvests_started();
    const svc::InventoryItem* t = p.s->inventory().at(Worn, slot);
    s.durability = t ? t->durability : -1.0f;
    s.xp = p.s->professions().xp(profession);
    return s;
}
static bool same_rewards(const Snap& a, const Snap& b) { return a.bag == b.bag && a.completed == b.completed && a.durability == b.durability && a.xp == b.xp; }
// [C.Y.B ~ CoA]

static const char* g_path = nullptr;

// ---- the start's checks ----------------------------------------------------------------------------------------------------------------

static void test_refusals() {
    std::printf("a start that is refused: each check logged and counted, nothing sent, nothing changed\n");
    Rig rig;
    if (!rig.open(g_path)) return;
    const uint64_t daffodil = rig.add_node(id::kDaffodil, 200, 0, 11);
    const uint64_t moonbell = rig.add_node(id::kMoonbell, 0, 200, 12);
    const uint64_t far_node = rig.add_node(id::kDaffodil, 1000, 0, 13);     // 10 m: open for the client, out of the interaction's reach (350 cm)
    const uint64_t dormant = rig.add_node(id::kDaffodil, 25000, 0, 14);     // 250 m: dormant for the client
    Player& bare = rig.add_player(record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "bare", /*tools=*/false));
    Player& novice = rig.add_player(record_of("0000000000000000000000000000aa01", "novice"));
    svc::Character ar = record_of("0000000000000000000000000000aa02", "apprentice");
    ar.professions.push_back(prof(id::kHerbalism, 1, 10, 40000));
    Player& apprentice = rig.add_player(std::move(ar));
    for (Player* p : {&bare, &novice, &apprentice}) play(*p);
    CHECK(novice.s->actor_view()->state_of(far_node) == world::ClientState::Awake && novice.s->actor_view()->state_of(dormant) != world::ClientState::Awake,
          "the 10 m node is open on the client, the 250 m node is only dormant there");
    const size_t pc_before = pc_calls_since(novice, 0).size();
    // no tool
    bare.send_now(interact(rig.component_of(daffodil)), bare.pawn_ch);
    CHECK(refusal_count(bare, "no tool") == 1 && bare.s->harvests_started() == 0 && rig.board().state_of(daffodil) == gather::NodeState::Available, "nothing worn in the Sickle's slot: refused (no tool)");
    // a Novice at an Apprentice node
    novice.send_now(interact(rig.component_of(moonbell)), novice.pawn_ch);
    CHECK(refusal_count(novice, "not certified") == 1 && rig.board().state_of(moonbell) == gather::NodeState::Available, "a Novice Herbalist at an Apprentice node: not certified");
    // out of range, and not open
    novice.send_now(interact(rig.component_of(far_node)), novice.pawn_ch);
    CHECK(refusal_count(novice, "out of range") == 1 && rig.board().state_of(far_node) == gather::NodeState::Available, "a node 10 m away: out of range");
    novice.send_now(interact(rig.component_of(dormant)), novice.pawn_ch);
    CHECK(refusal_count(novice, "not open") == 1, "a node the client has only dormant: not open");
    // an Apprentice with a T1 sickle at an Apprentice node: the tool's tier
    apprentice.send_now(interact(rig.component_of(moonbell)), apprentice.pawn_ch);
    CHECK(apprentice.s->professions().tier(id::kHerbalism) == 1 && refusal_count(apprentice, "tool tier too low") == 1, "an Apprentice Herbalist with a T1 sickle at an Apprentice node: the tool's tier is too low");
    // a node that is not one: the component of an NPC would be ignored; an id no actor has is ignored by the generic path
    novice.send_now(interact(999999999), novice.pawn_ch);
    CHECK(novice.s->harvests_started() == 0 && novice.s->interactions_ignored() >= 1, "a component no actor owns is ignored");
    CHECK(pc_calls_since(novice, 0).size() == pc_before, "and none of this sent the PlayerController a call");
    std::string all = "bare: " + refusals_of(bare) + "; novice: " + refusals_of(novice) + "; apprentice: " + refusals_of(apprentice);
    std::printf("      (%s)\n", all.c_str());
    // the positive controls: a Journeyman Herbalist with the T3 sickle harvests the Apprentice node (the tool is a higher tier)
    svc::Character jr = record_of("0000000000000000000000000000aa03", "journeyman", false);
    jr.professions.push_back(prof(id::kHerbalism, 2, 20, 160000));
    jr.items.push_back(make_item(Worn, 33, id2::kT3Sickle, 133, 1, 350));
    Player& journeyman = rig.add_player(std::move(jr));
    play(journeyman);
    journeyman.send_now(interact(rig.component_of(moonbell)), journeyman.pawn_ch);
    CHECK(journeyman.s->harvests_started() == 1 && rig.board().state_of(moonbell) == gather::NodeState::Channeling, "a Journeyman Herbalist with the T3 sickle starts at the Apprentice node");
    const int64_t xp_before = journeyman.s->professions().xp(id::kHerbalism);
    run_until(rig, [&] { return journeyman.s->harvests_completed() == 1; }, seconds(20));
    const int64_t gain = journeyman.s->professions().xp(id::kHerbalism) - xp_before;
    CHECK(journeyman.s->harvests_completed() == 1 && gain >= 90 && gain <= 270, "and gets the Apprentice tier's 90 profession XP at least (the T3 sickle's experience rating raises it: its stats are in the rating, equipment included)");
    CHECK(journeyman.s->inventory().at(Worn, 33)->durability == 349.0f, "the T3 sickle: 349");
}

// ---- exploits ---------------------------------------------------------------------------------------------------------------------------

static void test_exploits() {
    std::printf("exploits: a repeated or flooded interact, a gathered node, another player's node; everything is granted once\n");
    Rig rig;
    if (!rig.open(g_path)) return;
    const uint64_t a = rig.add_node(id::kDaffodil, 200, 0, 21);
    const uint64_t b = rig.add_node(id::kDaffodil, 0, 200, 22);
    Player& p = rig.add_player(record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb"));
    play(p);
    const Snap s0 = snap(p);
    p.send_now(interact(rig.component_of(a)), p.pawn_ch);
    CHECK(p.s->harvests_started() == 1 && rig.board().state_of(a) == gather::NodeState::Channeling, "the first interact starts the channel");
    const gather::Channel before = *rig.board().channel_of(a);
    // the same interact again, three times (a replayed packet), and another node's
    for (int i = 0; i < 3; ++i) p.send_now(interact(rig.component_of(a)), p.pawn_ch);
    p.send_now(interact(rig.component_of(b)), p.pawn_ch);
    CHECK(refusal_count(p, "already channeling") == 4 && p.s->harvests_started() == 1, "the interact again (x3) and another node's: refused, the channel is one");
    CHECK(rig.board().state_of(b) == gather::NodeState::Available && rig.board().channel_of(a)->started == before.started && rig.board().channel_of(a)->completes == before.completes,
          "the channel was not restarted or moved");
    // a flood in one datagram
    std::vector<fake::Bunch> flood;
    for (int i = 0; i < 60; ++i) flood.push_back(p.client.actor(p.pawn_ch, interact(rig.component_of(a)), true));
    const size_t dropped_before = p.s->requests_dropped();
    p.client_sends(std::move(flood));
    rig.run(milliseconds(60));
    CHECK(p.s->requests_dropped() > dropped_before && p.s->harvests_started() == 1 && rig.board().state_of(a) == gather::NodeState::Channeling, "a flood of 60 interacts is cut at the budget and changes nothing");
    // the client never cancels and never says anything else: the channel ends by the clock, once
    const bool ended = run_until(rig, [&] { return p.s->harvests_completed() == 1; }, seconds(20));
    CHECK(ended, "the channel ends by the server's clock");
    rig.run(seconds(1));
    const Snap s1 = snap(p);
    CHECK(s1.completed == 1 && s1.bag == s0.bag + 1 && s1.durability == s0.durability - 1 && s1.xp == s0.xp + 80, "granted once: one stack, one durability, 80 XP");
    // the node is gathered: an interact on it is refused
    p.send_now(interact(rig.component_of(a)), p.pawn_ch);
    p.send_now(interact(rig.component_of(a)), p.pawn_ch);
    CHECK(refusal_count(p, "node gathered") == 2 && p.s->harvests_started() == 1, "an interact on the gathered node (and its replay): refused");
    rig.run(seconds(2));
    const Snap s2 = snap(p);
    CHECK(same_rewards(s1, s2), "and nothing more was granted");
    // another player's node: the second player is refused while the first channels
    Player& q = rig.add_player(record_of("0000000000000000000000000000bb01", "bob"));
    play(q);
    p.send_now(interact(rig.component_of(b)), p.pawn_ch);
    CHECK(rig.board().state_of(b) == gather::NodeState::Channeling && rig.board().channel_of(b)->player_id == static_cast<uint64_t>(p.s->bootstrap().setup().player_id), "the first player channels the other node");
    q.send_now(interact(rig.component_of(b)), q.pawn_ch);
    CHECK(refusal_count(q, "node busy") == 1 && q.s->harvests_started() == 0, "the second player is refused: the state is shared");
    const Snap q0 = snap(q);
    run_until(rig, [&] { return p.s->harvests_completed() == 2; }, seconds(20));
    rig.run(seconds(1));
    const Snap q1 = snap(q);
    CHECK(p.s->harvests_completed() == 2 && same_rewards(q0, q1), "the first player gets the reward and the second nothing");
    q.send_now(interact(rig.component_of(b)), q.pawn_ch);
    CHECK(refusal_count(q, "node gathered") == 1, "and the node is gathered for the second player too");
    // both come back with the respawn
    CHECK(run_until(rig, [&] { return rig.board().state_of(a) == gather::NodeState::Available && rig.board().state_of(b) == gather::NodeState::Available; }, seconds(30)), "both nodes are back after the respawn");
    q.send_now(interact(rig.component_of(b)), q.pawn_ch);
    CHECK(q.s->harvests_started() == 1, "and the second player harvests it now");
}

// ---- cancellations -----------------------------------------------------------------------------------------------------------------------
// -- C_Y_B & C | O | A --

static void test_cancellations() {
    std::printf("a channel that ends with no reward: the node is Available at once and nothing is granted\n");
    Rig rig;
    if (!rig.open(g_path)) return;
    const uint64_t node = rig.add_node(id::kDaffodil, 200, 0, 31);
    Player& p = rig.add_player(record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb"));
    play(p);
    const auto started = [&] {
        p.send_now(interact(rig.component_of(node)), p.pawn_ch);
        return rig.board().state_of(node) == gather::NodeState::Channeling && p.s->harvesting_node() == node;
    };
    // the client's own interrupt
    Snap s0 = snap(p);
    const size_t first_from = p.got.size();
    CHECK(started(), "a channel starts");
    const size_t interrupt_from = p.got.size();
    p.send_now(interrupt_channel(), p.pawn_ch);
    CHECK(rig.board().state_of(node) == gather::NodeState::Available && p.s->harvesting_node() == 0 && p.s->harvests_cancelled() == s0.cancelled + 1, "ServerInterruptChannel: the channel ends, the node is Available");
    rig.run(milliseconds(300));
    CHECK(can_input_ok(node_seen(p, node, interrupt_from), true), "the cancel told the client CanInputActivate with four entries, the first true (an empty array crashed the client in test 1: its access violation 487 ms after the cancel)");
    {   // the channel the client believes it is in ends with the cancel: ChannelingData cleared, the same form as the result's [inferred: no capture shows a cancel]
        const auto cd = channeling_seen(p, first_from);
        CHECK(cd.size() == 2 && cd[0].second.flags == 6 && cd[0].second.timed && cd[1].second.flags == 0x106 && !cd[1].second.active && !cd[1].second.timed && cd[0].second.target == cd[1].second.target,
              "the cancel cleared the pawn's ChannelingData (the client's animation and bar end), once");
    }
    rig.run(seconds(13));
    CHECK(same_rewards(s0, snap(p)) && rig.board().state_of(node) == gather::NodeState::Available, "...and nothing was granted when the time would have come");
    // the tool taken off
    s0 = snap(p);
    CHECK(started(), "a channel starts again");
    p.send_now(item_used(4, 33, guid_n(133)), p.pawn_ch);
    rig.run(milliseconds(100));
    CHECK(!p.s->inventory().at(Worn, 33) && rig.board().state_of(node) == gather::NodeState::Available && p.s->harvesting_node() == 0, "the sickle is taken off: the channel ends");
    rig.run(seconds(13));
    const Snap s1 = snap(p);
    CHECK(s1.completed == s0.completed && s1.xp == s0.xp && s1.bag == s0.bag + 1, "no reward (the bag holds only the sickle that was taken off)");
    // put it back; a swapped tool
    // find the sickle in the bag and wear it again
    int sickle_slot = -1;
    for (int slot = 0; slot < items::kBackpackSlots; ++slot) {
        const svc::InventoryItem* it = p.s->inventory().at(Bag, slot);
        if (it && it->instance_guid == guid_n(133)) sickle_slot = slot;
    }
    CHECK(sickle_slot >= 0, "the sickle is in the bag");
    if (sickle_slot >= 0) p.send_now(item_used(3, sickle_slot, guid_n(133)), p.pawn_ch);
    rig.run(milliseconds(100));
    CHECK(p.s->inventory().at(Worn, 33) && p.s->inventory().at(Worn, 33)->instance_guid == guid_n(133), "the sickle is worn again");
    // a teleport away
    s0 = snap(p);
    CHECK(started(), "a channel starts a third time");
    const auto moved = p.s->gm_teleport(p.conn, kCx + 30000, kCy, bs::kDefaultSpawn.z, rig.t, "the test");
    CHECK(moved.status == PlayerSession::GmMoveResult::Status::Moved, "the player is moved 300 m away");
    rig.run(milliseconds(200));
    CHECK(rig.board().state_of(node) == gather::NodeState::Available && p.s->harvesting_node() == 0, "moving away from the node ends the channel");
    rig.run(seconds(13));
    CHECK(same_rewards(s0, snap(p)), "...with no reward");
    // the node removed from the world
    p.s->gm_teleport(p.conn, kCx, kCy, bs::kDefaultSpawn.z, rig.t, "the test back");
    play(p);
    const uint64_t node2 = rig.add_node(id::kDaffodil, 200, 0, 32);
    rig.run(seconds(2));
    s0 = snap(p);
    p.send_now(interact(rig.component_of(node2)), p.pawn_ch);
    CHECK(rig.board().state_of(node2) == gather::NodeState::Channeling, "a channel starts on a new node");
    rig.world->registry().remove(node2);
    rig.run(milliseconds(200));
    CHECK(p.s->harvesting_node() == 0 && rig.board().state_of(node2) == gather::NodeState::Available, "the node leaves the world: the channel ends");
    rig.run(seconds(13));
    CHECK(same_rewards(s0, snap(p)), "...with no reward");
    // a death
    const uint64_t node3 = rig.add_node(id::kDaffodil, 0, 200, 33);
    rig.run(seconds(2));
    s0 = snap(p);
    p.send_now(interact(rig.component_of(node3)), p.pawn_ch);
    CHECK(rig.board().state_of(node3) == gather::NodeState::Channeling, "a channel starts on a third node");
    p.s->take_damage(p.conn, 1.0e9, rig.t);
    rig.run(milliseconds(300));
    CHECK(p.s->dead() && p.s->harvesting_node() == 0 && rig.board().state_of(node3) == gather::NodeState::Available, "the player dies: the channel ends, the node is Available");
    rig.run(seconds(13));
    CHECK(p.s->harvests_completed() == s0.completed && p.s->professions().xp(id::kHerbalism) == s0.xp, "...with no reward");
    // a disconnect
    Player& q = rig.add_player(record_of("0000000000000000000000000000cc01", "carol"));
    play(q);
    const Snap qs = snap(q);
    q.send_now(interact(rig.component_of(node3)), q.pawn_ch);
    CHECK(rig.board().state_of(node3) == gather::NodeState::Channeling && q.s->harvesting_node() == node3, "a second player channels the node");
    q.s->finish("the test ends the session");
    CHECK(rig.board().state_of(node3) == gather::NodeState::Available, "the session ends mid-channel: the node is Available at once");
    rig.run(seconds(13));
    CHECK(q.s->harvests_completed() == 0 && same_rewards(qs, snap(q)), "...and nothing is granted");
    // a stalled session whose channel nobody completes: the board frees the node
    const uint64_t node4 = rig.add_node(id::kDaffodil, 0, -200, 34);
    gather::Channeler ghost;
    ghost.player_id = 424242;
    ghost.character = repl::GuidValue{1, 2, 3, 4};
    ghost.pawn.ObjectId = 4242;
    std::string why;
    CHECK(rig.board().begin(node4, ghost, rig.t, seconds(5), why), "a channel of a player whose session is gone (no session completes it)");
    rig.run(seconds(40));
    CHECK(rig.board().state_of(node4) == gather::NodeState::Available, "is freed 30 s after its time: no node is stuck");
}

// ---- standing still ----------------------------------------------------------------------------------------------------------------------
// C O A / Cyb

/// Test 1 of stage 2: "even after walking around, the tree was chopped".  The channel's only server-side condition was the interaction's reach plus a 100 cm slack (about 10 m for a
/// tree), so walking around within 10 m never ended it; the client, not knowing it was channeling, sent no ServerInterruptChannel either.  Retail's player stood perfectly still in
/// 8 of 10 captured harvests (the others: a 32 cm settle).  The server now ends the channel, with no reward, when the player has drifted more than 60 cm from where it began.
static void test_stand_still() {
    std::printf("a channel needs standing still: a step away ends it with no reward, a fidget does not\n");
    Rig rig;
    if (!rig.open(g_path)) return;
    const uint64_t node = rig.add_node(id::kDaffodil, 200, 0, 51);
    Player& p = rig.add_player(record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb"));
    play(p);
    const auto channeling = [&] { return rig.board().state_of(node) == gather::NodeState::Channeling && p.s->harvesting_node() == node; };
    const Snap s0 = snap(p);
    const size_t from = p.got.size();
    p.send_now(interact(rig.component_of(node)), p.pawn_ch);
    CHECK(channeling(), "a channel starts, 200 cm from the node");
    // a fidget of 20 cm is not leaving
    p.s->gm_teleport(p.conn, kCx + 20, kCy, bs::kDefaultSpawn.z, rig.t, "the test: a fidget");
    rig.run(milliseconds(400));
    CHECK(channeling(), "a step of 20 cm does not end the channel");
    // the drift is from where the channel began, not from the last step: 20 cm and then 50 more is 70 cm
    p.s->gm_teleport(p.conn, kCx + 70, kCy, bs::kDefaultSpawn.z, rig.t, "the test: a step away");
    rig.run(milliseconds(300));
    CHECK(!channeling() && rig.board().state_of(node) == gather::NodeState::Available && p.s->harvests_cancelled() == s0.cancelled + 1,
          "70 cm from where it began (still 130 cm from the node, well within its reach) the channel ends and the node is free");
    CHECK(count_in_log("the player moved 70 cm since the channel began") == 1, "the log says why: the player moved 70 cm since the channel began");
    const auto cd = channeling_seen(p, from);
    CHECK(cd.size() == 2 && cd[1].second.flags == 0x106 && !cd[1].second.timed, "and the client's ChannelingData is cleared: its animation and bar end");
    rig.run(seconds(13));
    CHECK(same_rewards(s0, snap(p)), "no reward comes when the time would have been up");
    // standing still for the whole channel gives the reward
    const Snap s1 = snap(p);
    p.send_now(interact(rig.component_of(node)), p.pawn_ch);
    CHECK(channeling(), "a new channel starts from where the player stands now");
    CHECK(run_until(rig, [&] { return p.s->harvests_completed() == s1.completed + 1; }, seconds(20)), "standing still, the channel completes by the clock");
}

// ---- foliage -----------------------------------------------------------------------------------------------------------------------------

/// The client sends ServerStartInteractionWithFoliage when F is pressed on a tree or plant of the world's foliage (not a placed resource node).  Gathering stage 3 answers it when the world
/// has foliage (test_worldd_foliage_session); this rig has none, so the call is ignored, and the log says so once per session so that a test that tries foliage reads why nothing happened.
static void test_foliage_line() {
    std::printf("foliage trees are Gathering stage 3: one info line per session\n");
    Rig rig;
    if (!rig.open(g_path)) return;
    Player& p = rig.add_player(record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb"));
    play(p);
    const size_t before = count_in_log("foliage gathering is off in this world");
    p.request(foliage_interact(), p.pc_ch);
    p.request(foliage_interact(), p.pc_ch);
    p.request(foliage_interact(), p.pc_ch);
    CHECK(count_in_log("foliage gathering is off in this world") == before + 1, "three interactions with foliage log one line");
    CHECK(count_in_log("cyb: a tree or plant of the world's foliage was interacted with") >= 1, "naming the player and saying what it was");
    CHECK(p.s->harvests_started() == 0 && p.s->harvesting_node() == 0, "and no harvest starts");
}

// ---- a full bag --------------------------------------------------------------------------------------------------------------------------

struct Dropped2 {
    std::vector<combat::LootStack> stacks;
};

namespace id3 {
inline constexpr int64_t kCoal = 109162169652923ll;   // Resource_Raw_Stone_Coal: 1 x 1, stacks to 20 (a filler of the Sanctus bag's 24 cells)
}  // namespace id3

static void test_full_bag() {
    std::printf("a full resource bag: no storage event, a callout, a pile 100 ms after the experience frame\n");
    Rig rig;
    if (!rig.open(g_path)) return;
    struct Dropped {
        double x, y, z;
        repl::GuidValue owner;
        std::string name;
        std::vector<combat::LootStack> stacks;
        Clock::time_point at;
    };
    std::vector<Dropped> piles;
    rig.world->set_pile_hook([&](double x, double y, double z, const repl::GuidValue& owner, const std::string& name, const std::vector<combat::LootStack>& stacks) -> uint64_t {
        piles.push_back({x, y, z, owner, name, stacks, rig.t});
        return 7777 + piles.size();
    });
    const uint64_t node = rig.add_node(id::kDaffodil, 200, 0, 41);
    svc::Character full = record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb");
    for (int slot = 0; slot < 24; ++slot) full.items.push_back(make_item(R0, slot, id3::kCoal, 300 + static_cast<uint32_t>(slot), 20));
    Player& p = rig.add_player(std::move(full));
    play(p);
    CHECK(p.s->inventory().count(R0) == 24 && p.s->inventory().count(Bag) == 0, "Resource0 holds 24 stacks of coal, one in each cell of the 6 x 4 grid");
    const size_t from = p.got.size();
    const Snap s0 = snap(p);
    p.send_now(interact(rig.component_of(node)), p.pawn_ch);
    const Clock::time_point frame_at = [&] {
        run_until(rig, [&] { return p.s->harvests_completed() == 1; }, seconds(20));
        return rig.t;
    }();
    CHECK(p.s->harvests_completed() == 1, "the harvest completes");
    rig.run(milliseconds(40));
    const auto calls = pc_calls_since(p, from);
    CHECK(count_calls(calls, "ClientUpdateItemDurability") == 1 && count_calls(calls, "ClientReceiveStorageEvent") == 0, "the result frame has the tool's wear and no storage event (1461)");
    CHECK(snap(p).bag == s0.bag && p.s->inventory().count(Bag) == 0 && snap(p).durability == s0.durability - 1 && snap(p).xp == s0.xp + 80,
          "the resource bag is as it was (and the Backpack is not used: the daffodil 1 x 2 has no free rectangle); the tool wore and the experience came");
    CHECK(p.s->bootstrap().callouts() == 1, "a callout tells the player why (\"Unable to loot, not enough inventory space.\")");
    CHECK(piles.empty() || piles[0].at - frame_at >= milliseconds(60), "the pile is not made in the frame of the result");
    rig.run(milliseconds(200));
    CHECK(piles.size() == 1 && p.s->harvest_piles() == 1, "a pile is made");
    if (!piles.empty()) {
        const Dropped& d = piles[0];
        const auto& cg = p.s->bootstrap().setup().character_guid;
        CHECK(d.stacks.size() == 1 && d.stacks[0].item == id::kDaffodilItem && d.stacks[0].quantity == 3 && d.stacks[0].quality == 1, "it holds the harvest: Daffodil x3 q1");
        const repl::GuidValue me{cg[0], cg[1], cg[2], cg[3]};
        CHECK(d.owner == me && d.name == "cyb", "for the harvester");
        const GoodPosition at = p.s->movement().good();
        CHECK(std::hypot(d.x - at.x, d.y - at.y) < 1.0 && std::fabs(d.z - at.z) < 1.0, "where the player stands");
        const double after_frame = std::chrono::duration<double>(d.at - frame_at).count();
        CHECK(after_frame >= 0.1 && after_frame < 0.2, "100 ms after the experience frame (the world steps 20 ms)");
    }
    rig.run(seconds(4));
    CHECK(p.s->bootstrap().callouts() == 0, "the callout is taken off after 3 s");
    // a bag with room for part of it is filled first: 69 items, a Moonbell... the stacks that fit go in, the rest to the pile
}
// (C | O | A) (Cyb)

// a Backpack full of other items blocks nothing of a harvest: the daffodils go to the resource bag
static void test_backpack_full_blocks_nothing() {
    std::printf("a full Backpack does not block a harvest\n");
    Rig rig;
    if (!rig.open(g_path)) return;
    std::vector<Dropped2> piles;
    rig.world->set_pile_hook([&](double, double, double, const repl::GuidValue&, const std::string&, const std::vector<combat::LootStack>& stacks) -> uint64_t {
        piles.push_back({stacks});
        return 8888 + piles.size();
    });
    const uint64_t node = rig.add_node(id::kDaffodil, 200, 0, 43);
    svc::Character full = record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb");
    for (int slot = 0; slot < items::kBackpackSlots; ++slot) full.items.push_back(make_item(Bag, slot, id2::kPotion, 300 + static_cast<uint32_t>(slot), 5));
    Player& p = rig.add_player(std::move(full));
    play(p);
    CHECK(p.s->inventory().count(Bag) == 70 && p.s->inventory().count(R0) == 0, "the Backpack holds 70 potions; Resource0 is empty");
    const size_t from = p.got.size();
    p.send_now(interact(rig.component_of(node)), p.pawn_ch);
    run_until(rig, [&] { return p.s->harvests_completed() == 1; }, seconds(20));
    rig.run(milliseconds(400));
    const auto calls = pc_calls_since(p, from);
    CHECK(count_calls(calls, "ClientReceiveStorageEvent") == 1 && p.s->inventory().count(R0) == 1 && p.s->inventory().count(Bag) == 70, "the daffodils are in Resource0; the Backpack still holds its 70");
    CHECK(piles.empty() && p.s->bootstrap().callouts() == 0, "no pile, no callout");
}

// the character's items are saved with their containers and a relog gives the same layout (the second player is the same record with what the saver was handed)
static void test_relog_keeps_the_resource_bag() {
    std::printf("a relog keeps the resource bag\n");
    Rig rig;
    if (!rig.open(g_path)) return;
    const uint64_t node = rig.add_node(id::kDaffodil, 200, 0, 44);
    Player& p = rig.add_player(record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb"));
    play(p);
    p.send_now(interact(rig.component_of(node)), p.pawn_ch);
    run_until(rig, [&] { return p.s->harvests_completed() == 1; }, seconds(20));
    rig.run(milliseconds(400));
    CHECK(!rig.sink.inventories.empty(), "the harvest handed the inventory to the saver");
    if (rig.sink.inventories.empty()) return;
    svc::Character again = record_of("bbec5c86eb101fe5f38c87f0fa22e7d1", "cyb2");
    again.items = rig.sink.inventories.back().second;
    bool in_resource = false;
    for (const svc::InventoryItem& it : again.items) in_resource = in_resource || (it.container == R0 && it.item_record_id == id::kDaffodilItem && it.slot == 0 && it.quantity == 3);
    CHECK(in_resource, "the saved items hold the daffodils in Resource0 slot 0");
    Player& q = rig.add_player(std::move(again));
    play(q);
    const svc::InventoryItem* it = q.s->inventory().at(R0, 0);
    CHECK(it && it->item_record_id == id::kDaffodilItem && it->quantity == 3 && q.s->inventory().count(Bag) == p.s->inventory().count(Bag), "a relog: the daffodils are in Resource0 slot 0, the Backpack has the same items");
    // the join told the client: a storage event for the resource container before its payload
    size_t adds_to_resource = 0, resource_payloads = 0;
    for (const repl::RpcCall& c : pc_calls_since(q, 0)) {
        if (!c.rpc) continue;
        if (std::string(c.rpc->name) == "ClientReceiveStorageEvent") {
            const auto ev = repl::parse_storage_event(c);
            if (ev && ev->targets.size() == 1 && ev->targets[0].object == q.s->bootstrap().setup().guids.resource_storage[0]) ++adds_to_resource;
        }
    }
    for (const repl::RpcCall& c : pawn_calls_since(q, 0)) {
        if (c.rpc && std::string(c.rpc->name) == "ClientReceiveStoragePayload") {
            const auto pl = repl::parse_storage_payload(c);
            if (pl && pl->target == q.s->bootstrap().setup().guids.resource_storage[0] && pl->maximum_items == 24) ++resource_payloads;
        }
    }
    CHECK(adds_to_resource == 1 && resource_payloads >= 1, "the join sent the Add into Resource0 and its 24-slot payload");
}
// ~ C | O | A & Cyb ~

// ---- a level and a tier ------------------------------------------------------------------------------------------------------------------

static void test_levels() {
    std::printf("a profession's level-up in a harvest, and the tiers' experience\n");
    Rig rig;
    if (!rig.open(g_path)) return;
    const uint64_t node = rig.add_node(id::kDaffodil, 200, 0, 51);
    svc::Character rec = record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb");
    rec.professions.push_back(prof(id::kHerbalism, 0, 1, 1560));   // 40 XP short of level 2 (1,600)
    Player& p = rig.add_player(std::move(rec));
    play(p);
    CHECK(p.s->professions().level(id::kHerbalism) == 1 && p.s->professions().xp(id::kHerbalism) == 1560, "a Novice Herbalist level 1 with 1,560 XP");
    const size_t pawn_from = p.got.size();
    p.send_now(interact(rig.component_of(node)), p.pawn_ch);
    run_until(rig, [&] { return p.s->harvests_completed() == 1; }, seconds(20));
    rig.run(milliseconds(300));
    CHECK(p.s->professions().xp(id::kHerbalism) == 1640 && p.s->professions().level(id::kHerbalism) == 2, "+80 XP: 1,640, level 2");
    const auto calls = pawn_calls_since(p, pawn_from);
    size_t level_ups = 0;
    for (const repl::RpcCall& c : calls) {
        if (c.rpc && std::string(c.rpc->name) == "BroadcastPlayLevelUpEffects") {
            const schema::RepCommand* lv = repl::rpc_command(*c.rpc, "InLevel");
            const schema::RepCommand* cat = repl::rpc_command(*c.rpc, "Category");
            const auto l = lv ? c.params.values.find(lv->handle) : c.params.values.end();
            const auto k = cat ? c.params.values.find(cat->handle) : c.params.values.end();
            if (l != c.params.values.end() && l->second.get_if<int64_t>() && *l->second.get_if<int64_t>() == 2 && k != c.params.values.end() && k->second.get_if<int64_t>() &&
                *k->second.get_if<int64_t>() == id::kLtHerbalist) {
                ++level_ups;
            }
        }
    }
    CHECK(level_ups == 1, "BroadcastPlayLevelUpEffects(Herbalist, 2) was sent once");
    bool saved = false;
    for (const auto& pr : rig.sink.progress) {
        for (const svc::ProfessionState& s : pr.progress.professions) saved = saved || (s.profession_guid == id::kHerbalism && s.xp == 1640 && s.level == 2);
    }
    CHECK(saved, "and the level and XP were handed to the saver");
    // a Novice at the cap (level 10, 40,000 XP) keeps harvesting; the XP is held at the cap rule
    const uint64_t node2 = rig.add_node(id::kDaffodil, 0, 200, 52);
    svc::Character cap = record_of("0000000000000000000000000000dd01", "capped");
    cap.professions.push_back(prof(id::kHerbalism, 0, 10, 40000 + 5000));
    Player& q = rig.add_player(std::move(cap));
    play(q);
    const int64_t held = q.s->professions().xp(id::kHerbalism);
    q.send_now(interact(rig.component_of(node2)), q.pawn_ch);
    run_until(rig, [&] { return q.s->harvests_completed() == 1; }, seconds(20));
    CHECK(q.s->professions().level(id::kHerbalism) == 10 && q.s->professions().xp(id::kHerbalism) >= held && q.s->professions().xp(id::kHerbalism) <= 49999, "a Novice at level 10 stays at level 10; the XP is held below the Apprentice's first level");
}

int main() {
    {   // the lines of the log (info and above) go to g_log: the tests read what the user's test would
        auto logger = std::make_shared<spdlog::logger>("test", std::make_shared<spdlog::sinks::ostream_sink_mt>(g_log));
        logger->set_level(spdlog::level::info);
        spdlog::set_default_logger(logger);
    }
    const char* path = db_path();
    if (!path) {
        std::printf("[skip] test_worldd_gather_session: no content.db (set AOC_CONTENT_DB, or build dist/content/content.db)\n");
        return 0;
    }
    g_path = path;
    std::printf("content.db: %s\n", path);
    {
        Rig rig;
        if (!rig.open(path)) return 1;
        test_a_harvest(rig);
    }
    test_refusals();
    test_exploits();
    test_cancellations();
    test_stand_still();
    test_foliage_line();
    test_full_bag();
    test_backpack_full_blocks_nothing();
    test_relog_keeps_the_resource_bag();
    test_levels();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
