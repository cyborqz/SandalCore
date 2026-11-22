// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_fishing_session.cpp  (Fishing, milestone 1)
//
//  Fishing on connections: sessions with fake clients on a PlayerWorld over the real content.db (AOC_CONTENT_DB, else dist/content/content.db of the working tree; "[skip]" without it),
//  its gathering tables and fishing data, and a small lake of known shape next to the login spot (the water's own geometry is test_worldd_fishing's).
//
//  a cast           the client's one ServerTriggerAbility, and what the server does alone: the reply (the ability item with its montage, the Cooldowns item, the Fishing_Buff) 100 ms on, the
//                   cooldown's end, the bite that jumps the montage to LoopEnd after the channel's 15 s and the geometric wait, the result 4.37 s later: the rod's durability one lighter at EVERY
//                   result, a storage event and the experience with a catch, the fish in the resource bag, the next loop at once
//  where            a cast that comes down on dry land fails: the montage jumps to CastFail, the buff and the montage go, nothing is worn
//  the rules        no rod, a broken rod, the cooldown, a trigger that comes twice, a cast while one runs, an ability the character does not know: refused (ClientEndAbility) or dropped
//  the branches     movement ends the cast (AbilityEnd, the buff, the montage cleared), as does the rod taken off, and the loop's fish is lost with it
//  exploits         a flood of triggers, a client that forges the same instance, a trigger the world cannot serve (no water file): the generic runtime answers as before
//  two players      two players fish side by side: their items, instances and results are their own, one's cancel is not the other's
//  a reconnect      the session ends in the middle of a cast: nothing is delivered after it, the rod's durability is what the last result saved, and a new session casts again
// ============================================================================
#include "content_db_fixture.h"
#include "cyb_appearance.h"
#include "fake_client.h"
#include "test_registry.h"
#include "shared_world_data.h"
#include "bootstrap/character_setup.h"
#include "bootstrap/constants.h"
#include "fishing/fishing.h"
#include "gather/gathering.h"
#include "items/inventory.h"
#include "net/endpoint.h"
#include "protocol/repl/ability_rpc.h"
#include "protocol/repl/block_reader.h"
#include "protocol/repl/move_packed.h"
#include "protocol/repl/replicator.h"
#include "protocol/repl/rpc_fields.h"
#include "protocol/repl/storage_rpc.h"
#include "protocol/schema/generated/index.h"
#include "session/player_session.h"

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
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
#define CHECKF(cond, ...) do { \
    if (cond) { std::printf("  [ok ] "); std::printf(__VA_ARGS__); std::printf("\n"); g_pass++; } \
    else { std::printf("  [FAIL] "); std::printf(__VA_ARGS__); std::printf("\n"); g_fail++; } \
} while (0)

static const std::string kMachine = "DESKTOP-EXAMPLE1-US-0123456789ABCDEF0123456789ABCDEF";
static const Clock::time_point t0{seconds(5000)};
static constexpr uint32_t kServerId = 60;
static constexpr uint32_t kRandomizer = 0x6ee87ee4u;
static const ItemContainer Worn = ItemContainer::Equipment;

namespace id {
inline constexpr int64_t kFishingCast = 6064630882622308352ll;
inline constexpr int64_t kFishingBuff = 6064632082445762560ll;
inline constexpr int64_t kPoleStarter = 6064631034410303488ll;   // Gear_Artisan_Fishing_Tool_Pole_Starter (tier 0)
inline constexpr int64_t kPoleT2 = 6064632236645351446ll;         // Gear_Artisan_Fishing_Tool_Pole_T2_0 (tier 1)
inline constexpr int64_t kBluegill = 6064629326176452611ll, kBass = 6064629326234910728ll, kSalmon = 6064629326234976266ll;
inline constexpr int64_t kLakeData = 6064631029846245378ll;       // Fish_Riverlands_Lakes_T1
inline constexpr int64_t kFishing = 108752770061964ll;
inline constexpr uint64_t kMontage = 17322920347082018633ull;
}  // namespace id

static const char* db_path() {
    const char* env = std::getenv("AOC_CONTENT_DB");
    if (env && *env) return env;
#ifdef FISHING_CONTENT_DB_PATH
    if (fs::exists(FISHING_CONTENT_DB_PATH)) return FISHING_CONTENT_DB_PATH;
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

static constexpr double kCx = bs::kDefaultSpawn.x, kCy = bs::kDefaultSpawn.y, kCz = bs::kDefaultSpawn.z;

/// A Bard with the worn kit of the fixture and, in the fishing rod slot, a pole (`rod_record`: the Starter's unless said) with `rod_durability` (none: no pole).  It stands at the login spot,
/// facing +x (yaw 0) with its feet at the surface of the test lake, `dx` and `dy` away.
static svc::Character record_of(const char* id_hex, const char* name, float rod_durability = 150, double dx = 0, double dy = 0, float yaw = 0, int64_t rod_record = id::kPoleStarter) {
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
    c.has_position = true;
    c.position.x = kCx + dx;
    c.position.y = kCy + dy;
    c.position.z = kCz;
    c.position.yaw = yaw;
    const std::pair<int, int64_t> worn[] = {{1, fi::kHelmet}, {2, fi::kChest}, {5, fi::kLegs}, {6, fi::kFeet}, {15, fi::kScepter}, {16, fi::kFocus}, {17, fi::kBook}, {23, fi::kBag}};
    for (const auto& [slot, record] : worn) c.items.push_back(make_item(Worn, slot, record, 100 + static_cast<uint32_t>(slot)));
    if (rod_durability >= 0) c.items.push_back(make_item(Worn, 35, rod_record, 135, 1, rod_durability));
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

// ---- what the client saw -----------------------------------------------------------------------------------------------------------------------------

static repl::CodecOptions stat_options() {
    repl::CodecOptions o;
    o.stat_int32_extended = [](int64_t record) {
        const uint32_t hi = static_cast<uint32_t>(static_cast<uint64_t>(record) >> 32);
        return hi == 0x5429e8c0u || hi == 0x5429e8c4u;
    };
    return o;
}

static repl::CodecOptions client_options() {
    repl::CodecOptions o;
    o.inline_objects = true;
    return o;
}

struct SeenItem {
    double   at = 0;
    int64_t  ability = 0, instance = 0;
    uint64_t montage = 0;
    float    position = 0;
    bool     jump = false;
    int32_t  counter = 0;
    std::string group;
};
struct SeenEffect {
    double  at = 0;
    int32_t key = 0;
    int64_t guid = 0;
    float   duration = 0;
};
struct SeenCooldown {
    double  at = 0;
    int32_t key = 0;
    int64_t ability = 0;
};
struct SeenCall {
    double      at = 0;
    std::string name;
};

/// The client's view of one player's pawn and controller, rebuilt from what the server sent: the fast arrays of the ability and stats components and the PlayerController's calls.
struct Mirror {
    bs::PlayerSetup setup;
    std::vector<SeenItem>     items;            // every ReplicatedActiveAbilities item added or changed, in order
    std::vector<int32_t>      items_removed;    // the keys of the removed ones
    std::vector<SeenEffect>   effects_added;
    std::vector<int32_t>      effects_removed;
    std::vector<SeenCooldown> cooldowns_set;
    std::vector<int32_t>      cooldowns_removed;
    std::vector<SeenCall>     calls;            // the PlayerController's calls (ClientUpdateItemDurability, ClientReceiveStorageEvent, ClientEndAbility ...)
    std::vector<std::pair<double, int32_t>> ends;   // ClientEndAbility(ClientInstanceId)
    std::map<int32_t, int64_t> key_ability;     // an item's key -> its ability
    std::map<int32_t, int64_t> key_effect;
    size_t xp_updates = 0, unreadable = 0;
    size_t seen = 0;

    explicit Mirror(const bs::PlayerSetup& s) : setup(s) {}

    static const repl::Value* val(const repl::ElementState& e, uint16_t handle) {
        const auto it = e.values.find(handle);
        return it == e.values.end() ? nullptr : &it->second;
    }
    static int64_t i64(const repl::ElementState& e, uint16_t handle) {
        const repl::Value* v = val(e, handle);
        return v && v->get_if<int64_t>() ? *v->get_if<int64_t>() : 0;
    }

    void absorb_pc(const fake::Delivered& d, double at) {
        if (d.open || d.close || d.content.bit_pos() <= 8) return;
        const auto views = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
        if (!views) return;
        for (const repl::BlockView& v : *views) {
            if (!v.header.is_actor && v.header.object == setup.guids.xp_tracker) ++xp_updates;
            if (!v.header.is_actor || v.header.has_rep_layout) continue;
            size_t p = v.payload_pos;
            const auto rpcs = repl::read_rpc_fields(class_named("AoCPlayerControllerBP_C"), d.content.data(), v.payload_pos + v.payload_bits, p);
            if (!rpcs) continue;
            for (const repl::RpcCall& c : *rpcs) {
                if (c.decoded) calls.push_back({at, c.rpc->name});
            }
        }
    }

    void absorb(const fake::Delivered& d, double at) {
        const auto views = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
        if (!views) {
            ++unreadable;
            return;
        }
        for (const repl::BlockView& v : *views) {
            if (v.header.is_actor) continue;
            const size_t end = v.payload_pos + v.payload_bits;
            size_t p = v.payload_pos;
            if (v.header.object == setup.guids.stats) {
                const auto fields = repl::read_fast_array_fields(class_named("UAoCStatsComponent"), d.content.data(), end, p, stat_options());
                if (!fields) continue;
                for (const repl::FastArrayField& f : *fields) {
                    if (std::string(class_named("UAoCStatsComponent").field(f.field)->name) != "CurrentEffects") continue;
                    for (const int32_t key : f.delta.deleted) effects_removed.push_back(key);
                    for (const auto& c : f.delta.changed) {
                        const repl::Value* dur = val(c.values, 4);
                        effects_added.push_back({at, c.id, i64(c.values, 2), dur && dur->get_if<float>() ? *dur->get_if<float>() : (dur && dur->get_if<double>() ? static_cast<float>(*dur->get_if<double>()) : -1.0f)});
                        key_effect[c.id] = i64(c.values, 2);
                    }
                }
            } else if (v.header.object == setup.guids.ability) {
                const auto fields = repl::read_fast_array_fields(class_named("UAoCAbilityComponent"), d.content.data(), end, p, stat_options());
                if (fields) {
                    for (const repl::FastArrayField& f : *fields) ability_field(f, at);
                    continue;
                }
                p = v.payload_pos;
                const auto rpcs = repl::read_rpc_fields(class_named("UAoCAbilityComponent"), d.content.data(), end, p);
                if (!rpcs) continue;
                for (const repl::RpcCall& c : *rpcs) {
                    if (!c.decoded || std::string(c.rpc->name) != "ClientEndAbility") continue;
                    const schema::RepCommand* cmd = repl::rpc_command(*c.rpc, "ClientInstanceId");
                    const auto it = cmd ? c.params.values.find(cmd->handle) : c.params.values.end();
                    const int32_t id = it != c.params.values.end() && it->second.get_if<int64_t>() ? static_cast<int32_t>(*it->second.get_if<int64_t>()) : 0;
                    ends.emplace_back(at, id);
                }
            }
        }
    }

    void ability_field(const repl::FastArrayField& f, double at) {
        const std::string name = class_named("UAoCAbilityComponent").field(f.field)->name;
        if (name == "Cooldowns") {
            for (const int32_t key : f.delta.deleted) cooldowns_removed.push_back(key);
            for (const auto& c : f.delta.changed) cooldowns_set.push_back({at, c.id, i64(c.values, 1)});
            return;
        }
        if (name != "ReplicatedActiveAbilities") return;
        for (const int32_t key : f.delta.deleted) items_removed.push_back(key);
        for (const auto& c : f.delta.changed) {
            SeenItem it;
            it.at = at;
            it.ability = i64(c.values, 1);
            it.instance = i64(c.values, 2);
            const repl::Value* montage = val(c.values, 11);
            it.montage = montage && montage->get_if<emit::FIntrepidNetworkGUID>() ? montage->get_if<emit::FIntrepidNetworkGUID>()->ObjectId : 0;
            const repl::Value* group = val(c.values, 10);
            it.group = group && group->get_if<repl::NameValue>() ? group->get_if<repl::NameValue>()->text : std::string();
            const repl::Value* pos = val(c.values, 13);
            it.position = pos && pos->get_if<float>() ? *pos->get_if<float>() : 0.0f;
            const repl::Value* jump = val(c.values, 14);
            it.jump = jump && jump->get_if<bool>() && *jump->get_if<bool>();
            it.counter = static_cast<int32_t>(i64(c.values, 15));
            items.push_back(it);
            key_ability[c.id] = it.ability;
        }
    }

    size_t count_calls(const char* name) const { return static_cast<size_t>(std::count_if(calls.begin(), calls.end(), [&](const SeenCall& c) { return c.name == name; })); }
    /// The items of the fishing ability.
    std::vector<SeenItem> fishing_items() const {
        std::vector<SeenItem> out;
        for (const SeenItem& i : items) {
            if (i.ability == id::kFishingCast) out.push_back(i);
        }
        return out;
    }
};

// ---- the world and its players ---------------------------------------------------------------------------------------------------------------------

struct Rig;

struct Player {
    Rig& rig;
    svc::Character record;
    Connection conn{ConnectionConfig{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870}, t0};
    fake::Client client{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870};
    std::unique_ptr<PlayerSession> s;
    std::unique_ptr<Mirror> mirror;
    std::vector<fake::Delivered> got;
    size_t absorbed = 0;
    uint32_t pawn_ch = 0, pc_ch = 0;
    std::string error;
    float ts = 100.0f;
    double x = 0, y = 0;
    double pitch = 0;      // the camera's pitch the moves carry (degrees; the retail client's moves carry it too)
    int32_t next_id = 0;   // the client counts every trigger of any ability from 0

    Player(Rig& r, svc::Character rec) : rig(r), record(std::move(rec)) {}
    void join();
    void flush();
    void client_sends(std::vector<fake::Bunch> bunches);
    void step_session();
    void absorb();
    void send_reliable(const BunchWriter& b);
    /// ServerTriggerAbility of the fishing ability with the client's own instance id; returns the id.
    int32_t cast(int32_t instance = -1);
    /// A move 100 ms on, at the speed `cm_s` along +x (0: standing).
    void move(double cm_s);
    void item_off(int slot, uint32_t n);
};

struct Rig {
    const content::ContentDb* db = nullptr;
    aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
    std::unique_ptr<PlayerWorld> world;
    RecordingSink sink;
    Clock::time_point t = t0;
    std::vector<std::unique_ptr<Player>> players;
    std::string water_file;

    /// `water`: whether the world fishes (a water file and the fishing start).  The lake: 100 m square, its surface at the feet of a player at the login spot, its near edge 3 m ahead of him
    /// (x from the spot + 300 to + 10,300, y from - 5,000 to + 5,000).
    bool open(const char* path, bool water = true, uint64_t seed = 0x6A7E5EEDull) {
        std::string error;
        shared_world::Entry& entry = shared_world::get(path);
        db = entry.db ? &*entry.db : nullptr;
        if (!db || !entry.data) {
            std::printf("  [FAIL] the content.db does not open: %s\n", entry.problem.c_str());
            return false;
        }
        world = std::make_unique<PlayerWorld>(entry.data, entry.problem, bs::GameStateMode::Tier2, kServerId, guids);
        world->set_position_sink(&sink);
        gather::Config cfg;
        cfg.seed = seed;
        if (!world->start_gathering(cfg, error)) {
            std::printf("  [FAIL] gathering does not start: %s\n", error.c_str());
            return false;
        }
        if (water) {
            const double surface = kCz - PlayerWorld::kCapsuleHalfHeightCm;
            std::ostringstream text;
            text.precision(10);
            text << "body\t0\tlake\tTestLake\t" << id::kLakeData << "\t" << kCx + 5300 << "\t" << kCy << "\t" << surface << "\n";
            text << "outline\t0\t" << kCx + 300 << "," << kCy - 5000 << "\t" << kCx + 10300 << "," << kCy - 5000 << "\t" << kCx + 10300 << "," << kCy + 5000 << "\t" << kCx + 300 << "," << kCy + 5000 << "\n";
            water_file = (fs::temp_directory_path() / "fishing_session_water.tsv").string();
            std::ofstream(water_file, std::ios::binary) << text.str();
            fishing::Config fc;
            fc.water_path = water_file;
            if (!world->start_fishing(fc, error)) {
                std::printf("  [FAIL] fishing does not start: %s\n", error.c_str());
                return false;
            }
        }
        return true;
    }
    ~Rig() {
        if (!water_file.empty()) fs::remove(water_file);
    }
    gather::Gathering& g() { return *world->gathering(); }

    Player& add_player(svc::Character rec) {
        players.push_back(std::make_unique<Player>(*this, std::move(rec)));
        players.back()->join();
        return *players.back();
    }
    void step(milliseconds d = milliseconds(20)) {
        t += d;
        for (auto& p : players) {
            if (p->s) {
                p->step_session();
                p->absorb();
            }
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
    double now() const { return world_time_at(t); }
};

void Player::join() {
    s = PlayerSession::join(conn, *rig.world, record, identity(), world_time_at(rig.t), error);
    if (!s) {
        std::printf("  [FAIL] the session does not join: %s\n", error.c_str());
        std::exit(2);
    }
    pawn_ch = s->channel_of(bs::Role::Pawn);
    pc_ch = s->channel_of(bs::Role::PlayerController);
    mirror = std::make_unique<Mirror>(s->bootstrap().setup());
    x = record.position.x;
    y = record.position.y;
}
void Player::flush() {
    for (auto& pkt : conn.flush(rig.t)) {
        const auto r = client.receive(pkt);
        for (const auto& d : r.delivered) got.push_back(d);
    }
}
void Player::absorb() {
    for (; absorbed < got.size(); ++absorbed) {
        const fake::Delivered& d = got[absorbed];
        if (d.channel == pawn_ch && d.content.bit_pos() != 8) mirror->absorb(d, rig.now());
        if (d.channel == pc_ch) mirror->absorb_pc(d, rig.now());
    }
}
void Player::client_sends(std::vector<fake::Bunch> bunches) {
    for (const auto& pkt : client.packets(std::move(bunches))) {
        const std::vector<InBunch> in = conn.receive(pkt, rig.t);
        s->take_acks(conn);
        for (const InBunch& b : in) s->on_bunch(conn, b, rig.t);
        s->end_of_datagram(conn);
    }
}
void Player::step_session() {
    s->tick(conn, rig.t);
    flush();
    s->take_acks(conn);
    client_sends({});
}

/// The client's bunch for an RPC of the pawn's AbilityComponent: a content block of the subobject, client form.
static BunchWriter ability_rpc(const emit::FIntrepidNetworkGUID& ability, const char* name, const repl::RpcParams& params) {
    const schema::ClassSchema& cls = class_named("UAoCAbilityComponent");
    const schema::RpcSchema* rpc = cls.rpc(name);
    BunchWriter out;
    if (!rpc) {
        std::printf("  [FAIL] no RPC %s\n", name);
        return out;
    }
    BunchWriter field;
    bool failed = false;
    if (repl::write_rpc_field(cls, *rpc, params, field, client_options(), &failed) == 0 || failed) {
        std::printf("  [FAIL] %s does not write\n", name);
        return out;
    }
    repl::BlockHeader header;
    header.is_actor = false;
    header.has_rep_layout = false;
    header.object = ability;
    repl::write_content_block(out, header, field, /*server_form=*/false);
    return out;
}

/// ServerTriggerAbility as the retail client sends it: no targets, ItemData "no item" (Slot -1, the empty FItemInstance, Location 24).
static BunchWriter trigger_bunch(const emit::FIntrepidNetworkGUID& ability, int64_t guid, int32_t instance) {
    const schema::RpcSchema* rpc = class_named("UAoCAbilityComponent").rpc("ServerTriggerAbility");
    repl::RpcParams p;
    bool ok = rpc && repl::set_rpc_param(*rpc, p, "AbilityGuid", repl::Value::i64(guid));
    if (ok && instance != 0) ok = repl::set_rpc_param(*rpc, p, "ClientInstanceId", repl::Value::i64(instance));
    ok = ok && repl::set_rpc_param(*rpc, p, "InputVector", repl::Value(repl::Vec2d{0, 0}));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.Slot", repl::Value::i64(-1));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.ItemInstance", repl::Value(repl::ItemInstance{}));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.Location", repl::Value::u64(24));
    if (!ok) {
        std::printf("  [FAIL] the trigger's parameters do not fit\n");
        return BunchWriter();
    }
    return ability_rpc(ability, "ServerTriggerAbility", p);
}

static repl::NetworkMove move_at(float ts, double x, double y, double yaw_degrees, bool moving, double pitch_degrees = 0) {
    repl::NetworkMove m;
    m.time_stamp = ts;
    m.acceleration = repl::quantize_vector(10, moving ? 100 : 0, 0, 0);
    m.location = repl::quantize_vector(100, x, y, kCz);
    m.control_rotation.yaw = static_cast<uint16_t>(std::llround(yaw_degrees / 360.0 * 65536.0) & 0xffff);
    m.control_rotation.pitch = static_cast<uint16_t>(std::llround(pitch_degrees / 360.0 * 65536.0) & 0xffff);
    m.movement_mode = repl::kMoveWalking;
    m.input_bits = moving ? 1 : 0;
    return m;
}

static BunchWriter client_move(const repl::NetworkMove& mv) {
    const schema::ClassSchema& cls = class_named("PlayerPawn_C");
    const schema::NetField* f = cls.field("ServerMovePacked");
    BunchWriter out;
    repl::MovePacked p;
    p.new_move = mv;
    BunchWriter params;
    BunchWriter field;
    if (!f || !repl::write_move_packed(params, p) || repl::write_field(cls, f->index, params, field) == 0) return out;
    repl::BlockHeader header;
    repl::write_content_block(out, header, field, /*server_form=*/false);
    return out;
}

void Player::send_reliable(const BunchWriter& b) {
    client_sends({client.actor(pawn_ch, b, /*reliable=*/true)});
    flush();
    absorb();
}

int32_t Player::cast(int32_t instance) {
    const int32_t id = instance >= 0 ? instance : next_id++;
    send_reliable(trigger_bunch(s->bootstrap().setup().guids.ability, id::kFishingCast, id));
    return id;
}

void Player::move(double cm_s) {
    ts += 0.1f;
    x += cm_s * 0.1;
    client_sends({client.actor(pawn_ch, client_move(move_at(ts, x, y, record.position.yaw, cm_s != 0, pitch)), /*reliable=*/false)});
    rig.step(milliseconds(100));
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
    BunchWriter out;
    BunchWriter field;
    bool failed = false;
    if (repl::write_rpc_field(pawn, *rpc, p, field, {}, &failed) == 0 || failed) return out;
    repl::BlockHeader header;
    repl::write_content_block(out, header, field, /*server_form=*/false);
    return out;
}

void Player::item_off(int slot, uint32_t n) {
    client_sends({client.actor(pawn_ch, item_used(4, slot, guid_n(n)), true)});
    rig.run(milliseconds(100));
}

// ---- reading the state -------------------------------------------------------------------------------------------------------------------------------

static std::ostringstream g_log;
static size_t count_in_log(const char* what) {
    const std::string text = g_log.str();
    size_t n = 0;
    for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
    return n;
}

static float rod_durability(const Player& p) {
    const svc::InventoryItem* rod = p.s->inventory().at(Worn, 35);
    return rod ? rod->durability : -1.0f;
}

/// The fish (Bluegill, Largemouth Bass) the character carries, in the resource bags and the backpack.
static int fish_held(const Player& p) {
    int n = 0;
    for (const svc::InventoryItem& it : p.s->inventory().items()) {
        if (it.item_record_id == id::kBluegill || it.item_record_id == id::kBass) n += it.quantity;
    }
    return n;
}

/// How many of the item `record` the character carries.
static int held_of(const Player& p, int64_t record) {
    int n = 0;
    for (const svc::InventoryItem& it : p.s->inventory().items()) n += it.item_record_id == record ? it.quantity : 0;
    return n;
}

static int64_t fishing_xp(const Player& p) {
    const professions::Professions& pr = p.s->professions();
    return pr.valid() ? pr.state(id::kFishing).xp : -1;
}

/// How many of the fishing ability's items changed to the montage position `pos` (a jump).
static size_t jumps_to(const Mirror& m, float pos) {
    size_t n = 0;
    for (const SeenItem& i : m.fishing_items()) n += (i.jump && i.montage != 0 && std::fabs(i.position - pos) < 0.01f) ? 1 : 0;
    return n;
}
static const SeenItem* jump_at(const std::vector<SeenItem>& v, float pos, size_t nth = 0) {
    size_t n = 0;
    for (const SeenItem& i : v) {
        if (i.jump && i.montage != 0 && std::fabs(i.position - pos) < 0.01f && n++ == nth) return &i;
    }
    return nullptr;
}

static constexpr float kLoopEnd = 11.166666f, kCastFail = 21.5f, kAbilityEnd = 23.805052f;

// ---- the tests -----------------------------------------------------------------------------------------------------------------------------------------------

/// A cast on the lake: the reply, then the loop alone for 100 s.
static void test_a_cast(const char* path) {
    std::printf("a cast on the lake: the reply, the bites and the results\n");
    Rig rig;
    if (!rig.open(path)) return;
    Player& p = rig.add_player(record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb"));
    rig.settle();
    const Mirror& m = *p.mirror;
    const double rod0 = rod_durability(p);
    CHECK(rod0 == 150.0f, "the Starter pole is worn, 150 durability");
    const double trigger_at = rig.now();
    const int32_t client_id = p.cast();
    CHECK(client_id == 0 && p.s->fishing_casts() == 1, "the client's one trigger is a cast");
    // the reply
    rig.run(milliseconds(300));
    const auto first = m.fishing_items();
    CHECK(!first.empty() && first[0].ability == id::kFishingCast && first[0].montage == id::kMontage && first[0].group == "DefaultGroup" && !first[0].jump && std::fabs(first[0].position) < 1e-3f,
          "the ability item names the fishing montage (its static id), DefaultGroup, position 0, no jump");
    CHECKF(!first.empty() && std::fabs(first[0].at - trigger_at - 0.1) < 0.05, "the reply comes 100 ms after the trigger (%.3f s)", first.empty() ? -1.0 : first[0].at - trigger_at);
    CHECK(m.cooldowns_set.size() == 1 && m.cooldowns_set[0].ability == id::kFishingCast, "the Cooldowns item is the fishing ability's");
    CHECK(m.effects_added.size() == 1 && m.effects_added[0].guid == id::kFishingBuff && m.effects_added[0].duration > 1.0e30f, "the Fishing_Buff is on, with no end");
    CHECK(m.cooldowns_removed.empty(), "the cooldown is still running");
    // the cooldown ends a second after the reply
    rig.run(milliseconds(1000));
    CHECK(m.cooldowns_removed.size() == 1 && m.cooldowns_removed[0] == m.cooldowns_set[0].key, "the cooldown's item goes after its second");
    // the loop: 15 s of channel, a geometric wait, the bite, 4.37 s, the result
    const size_t storage_before = m.count_calls("ClientReceiveStorageEvent");
    const double xp0 = static_cast<double>(fishing_xp(p));
    rig.run(seconds(100));
    const auto items = m.fishing_items();
    size_t bites = jumps_to(m, kLoopEnd);
    CHECK(bites >= 2, "two bites or more in 100 s");
    const SeenItem* b0 = jump_at(items, kLoopEnd, 0);
    CHECKF(b0 && b0->at - trigger_at >= 0.1 + 1.673 + 15.0 && b0->at - trigger_at < 0.1 + 1.673 + 15.0 + 60.0, "the first bite comes after the lure's moment and the 15 s channel, plus the wait (%.1f s)", b0 ? b0->at - trigger_at : -1.0);
    CHECK(b0 && !b0->jump == false && b0->counter > first[0].counter, "the bite is a jump with a higher animation counter");
    // each result: the durability, 4.37 s after its bite
    std::vector<double> dur_at;
    for (const SeenCall& c : m.calls) {
        if (c.name == "ClientUpdateItemDurability") dur_at.push_back(c.at);
    }
    CHECK(dur_at.size() >= 1, "a result came");
    bool each_after = !dur_at.empty();
    for (size_t i = 0; i < dur_at.size() && i < bites; ++i) {
        const SeenItem* bite = jump_at(items, kLoopEnd, i);
        if (!bite || std::fabs(dur_at[i] - bite->at - 4.37) > 0.15) each_after = false;
    }
    CHECK(each_after, "the rod's durability update comes 4.37 s after each bite");
    const double rod1 = rod_durability(p);
    CHECKF(rod0 - rod1 == static_cast<double>(dur_at.size()), "the rod wore by exactly one at every result (%zu results: %.0f -> %.0f), a catch or not", dur_at.size(), rod0, rod1);
    const int fish = fish_held(p);
    const size_t storage = m.count_calls("ClientReceiveStorageEvent") - storage_before;
    CHECKF(static_cast<size_t>(fish) == storage && storage == p.s->fishing_catches(), "a storage event for each fish and no other: %zu events, %d fish in the bags, %zu catches counted", storage, fish, p.s->fishing_catches());
    CHECK(static_cast<double>(fishing_xp(p)) > xp0 || fish == 0, "the fishing experience rose with the catches");
    CHECK(count_in_log("[fishing]") > 0 && count_in_log("casts the line") == 1, "the log tells the cast once");
    std::printf("      (in 100 s: %zu bites, %zu results, %d fish, fishing XP %lld -> %lld)\n", bites, dur_at.size(), fish, static_cast<long long>(xp0), static_cast<long long>(fishing_xp(p)));
}

/// A cast that comes down on dry land.
static void test_dry_land(const char* path) {
    std::printf("a cast away from the water\n");
    Rig rig;
    if (!rig.open(path)) return;
    Player& p = rig.add_player(record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb", 150, 0, 0, /*yaw=*/180.0f));
    rig.settle();
    const Mirror& m = *p.mirror;
    const double at = rig.now();
    p.cast();
    rig.run(seconds(8));
    const SeenItem* fail = jump_at(m.fishing_items(), kCastFail);
    CHECKF(fail && std::fabs(fail->at - at - (0.1 + 1.673)) < 0.06, "the montage jumps to CastFail at the lure's moment (%.2f s)", fail ? fail->at - at : -1.0);
    CHECK(jumps_to(m, kLoopEnd) == 0, "no bite");
    CHECK(m.effects_removed.size() == 1, "the buff goes");
    // the buff goes about 2.35 s after the jump and the montage is cleared 4.3 s after it
    const SeenItem* cleared = nullptr;
    for (const SeenItem& i : m.fishing_items()) {
        if (i.montage == 0 && i.jump) cleared = &i;
    }
    CHECKF(cleared && fail && std::fabs(cleared->at - fail->at - 4.305) < 0.08, "the montage is cleared 4.3 s after the CastFail jump (%.2f s)", cleared && fail ? cleared->at - fail->at : -1.0);
    CHECK(rod_durability(p) == 150.0f && m.count_calls("ClientUpdateItemDurability") == 0, "the rod did not wear");
    CHECK(count_in_log("the cast failed") == 1 && count_in_log("came down on dry land") >= 1, "the log says where the line fell");
    // the player turns to the water: a second cast, once the first one's end is over, lands
}

// ---- the rest of the tests ------------------------------------------------------------------------------------------------------------------------------

/// Plays on until `done` or `max_s` seconds; true when `done` came first.
static bool play_until(Rig& rig, double max_s, const std::function<bool()>& done) {
    for (double s = 0; s < max_s; s += 0.02) {
        if (done()) return true;
        rig.step();
    }
    return done();
}

static bool ended_with(const Mirror& m, int32_t client_id) {
    for (const auto& e : m.ends) {
        if (e.second == client_id) return true;
    }
    return false;
}

/// The rules of the cast: what is refused (ClientEndAbility, nothing else), what is dropped, and what replaces a cast that runs.
static void test_the_rules(const char* path) {
    std::printf("the rules of the cast\n");
    {   // no rod
        Rig rig;
        if (!rig.open(path)) return;
        Player& p = rig.add_player(record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb", /*rod=*/-1));
        rig.settle();
        const int32_t id0 = p.cast();
        rig.run(seconds(2));
        const Mirror& m = *p.mirror;
        CHECK(ended_with(m, id0) && m.fishing_items().empty() && m.effects_added.empty() && p.s->fishing_refused() == 1 && p.s->fishing_casts() == 0, "no rod: the cast is ended (ClientEndAbility), no item, no buff");
        CHECK(count_in_log("the cast is refused (no rod)") == 1, "the log says why");
    }
    {   // a broken rod
        Rig rig;
        if (!rig.open(path)) return;
        Player& p = rig.add_player(record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb", /*rod=*/0));
        rig.settle();
        const int32_t id0 = p.cast();
        rig.run(seconds(2));
        CHECK(ended_with(*p.mirror, id0) && p.mirror->fishing_items().empty() && p.s->fishing_refused() == 1, "a rod with no durability: the cast is ended");
    }
    {   // the cooldown, the replay, the replacement
        Rig rig;
        if (!rig.open(path)) return;
        Player& p = rig.add_player(record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb"));
        rig.settle();
        const Mirror& m = *p.mirror;
        const int32_t first = p.cast();
        rig.run(milliseconds(400));
        const int32_t too_soon = p.cast();   // 0.4 s later: the cooldown of 1 s
        rig.run(milliseconds(300));
        CHECK(ended_with(m, too_soon) && !ended_with(m, first) && p.s->fishing_refused() == 1 && p.s->fishing_casts() == 1, "a second cast inside the cooldown is ended, the first goes on");
        p.cast(first);   // the same ClientInstanceId again: a replay
        rig.run(milliseconds(200));
        CHECK(p.s->fishing_casts() == 1 && p.s->fishing_refused() == 1 && m.fishing_items().size() == 1, "the same instance twice is one cast: nothing is answered, nothing is added");
        rig.run(seconds(1));
        const size_t items_before = m.fishing_items().size();
        const int32_t again = p.cast();      // after the cooldown, while the first runs: it replaces the first
        rig.run(milliseconds(400));
        CHECK(!ended_with(m, again) && p.s->fishing_casts() == 2 && p.s->fishing_ended() == 1, "a cast after the cooldown, while one runs, is taken and replaces it");
        const auto items = m.fishing_items();
        CHECK(items.size() > items_before && m.items_removed.size() == 1 && m.effects_removed.size() == 1, "the first cast's item and its buff are taken off in the new reply");
        CHECK(items.size() >= 2 && items.back().instance != items.front().instance && m.effects_added.size() == 2 && m.effects_added[1].key != m.effects_added[0].key, "the new cast has an instance and a buff of its own");
        CHECK(count_in_log("a new cast replaces the one that runs") == 1, "the log says it");
        // only the new cast bites
        rig.run(seconds(60));
        CHECK(p.s->fishing_bites() >= 1 && p.s->fishing_casts() == 2, "the loop that goes on is the second cast's alone");
    }
    {   // a flood of triggers: the budget of the ability calls holds, the one cast runs once
        Rig rig;
        if (!rig.open(path)) return;
        Player& p = rig.add_player(record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb"));
        rig.settle();
        for (int i = 0; i < 40; ++i) p.cast(100 + i);
        rig.run(seconds(3));
        CHECK(p.s->fishing_casts() == 1 && p.s->fishing_casts() + p.s->fishing_refused() <= 40, "forty triggers at once: one cast, the rest ended by the cooldown or dropped");
        CHECK(p.mirror->fishing_items().size() >= 1 && p.mirror->effects_added.size() == 1, "one item and one buff on the pawn");
    }
    {   // a world that cannot fish: the generic runtime answers the ability as before
        Rig rig;
        if (!rig.open(path, /*water=*/false)) return;
        Player& p = rig.add_player(record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb"));
        rig.settle();
        p.cast();
        rig.run(seconds(2));
        CHECK(p.s->fishing_casts() == 0 && p.s->fishing_refused() == 0 && !p.s->fishing_active(), "no water file: fishing took nothing");
        rig.run(seconds(60));
        CHECK(jumps_to(*p.mirror, kLoopEnd) == 0 && p.mirror->count_calls("ClientUpdateItemDurability") == 0 && rod_durability(p) == 150.0f, "...and no loop runs: the generic runtime's cast is an item and a cooldown, with no bite and no catch");
    }
}

/// What ends a cast: movement, the rod taken off, a teleport, the fish lost while reeling.
static void test_the_branches(const char* path) {
    std::printf("the branches that end a cast\n");
    {   // movement
        Rig rig;
        if (!rig.open(path)) return;
        Player& p = rig.add_player(record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb"));
        rig.settle();
        const Mirror& m = *p.mirror;
        p.cast();
        rig.run(seconds(1));
        CHECK(p.s->fishing_active() && jumps_to(m, kAbilityEnd) == 0, "the cast runs while the player stands");
        const double at = rig.now();
        for (int i = 0; i < 5; ++i) p.move(300);   // walks for half a second
        rig.run(seconds(4));
        const SeenItem* end = jump_at(m.fishing_items(), kAbilityEnd);
        CHECKF(end && end->at - at < 0.5, "the montage jumps to AbilityEnd as the player walks (%.2f s after the first step)", end ? end->at - at : -1.0);
        CHECK(m.effects_removed.size() == 1, "the buff goes");
        const SeenItem* cleared = nullptr;
        for (const SeenItem& i : m.fishing_items()) {
            if (i.montage == 0 && i.jump) cleared = &i;
        }
        CHECKF(cleared && end && std::fabs(cleared->at - end->at - 2.0) < 0.1, "the montage is cleared 2 s after the jump (%.2f s)", cleared && end ? cleared->at - end->at : -1.0);
        CHECK(!p.s->fishing_active() && p.s->fishing_ended() == 1, "the cast is over");
        rig.run(seconds(40));
        CHECK(jumps_to(m, kLoopEnd) == 0 && m.count_calls("ClientUpdateItemDurability") == 0 && rod_durability(p) == 150.0f, "no bite, no wear: the loop did not start again");
        CHECK(count_in_log("the player moved") == 1, "the log names the branch");
    }
    {   // the rod taken off
        Rig rig;
        if (!rig.open(path)) return;
        Player& p = rig.add_player(record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb"));
        rig.settle();
        const Mirror& m = *p.mirror;
        p.cast();
        rig.run(seconds(3));
        p.item_off(35, 135);
        rig.run(seconds(1));
        CHECK(!p.s->inventory().at(Worn, 35) && jumps_to(m, kAbilityEnd) == 1, "the rod is taken off: the cast ends with its branch");
        rig.run(seconds(40));
        CHECK(jumps_to(m, kLoopEnd) == 0 && count_in_log("the rod is gone or broke") == 1, "no bite follows");
    }
    {   // moved away (a teleport): the lure lies far from the player
        Rig rig;
        if (!rig.open(path)) return;
        Player& p = rig.add_player(record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb"));
        rig.settle();
        p.cast();
        rig.run(seconds(2));
        const auto moved = p.s->gm_teleport(p.conn, kCx - 30000, kCy, kCz, rig.t, "the test");
        CHECK(moved.status == PlayerSession::GmMoveResult::Status::Moved, "the player is moved 300 m away");
        rig.run(milliseconds(300));
        CHECK(jumps_to(*p.mirror, kAbilityEnd) == 1, "moved away from the spot he cast from, the cast ends");
    }
    {   // the fish is lost: a move after the bite and before the result
        Rig rig;
        if (!rig.open(path)) return;
        Player& p = rig.add_player(record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb"));
        rig.settle();
        const Mirror& m = *p.mirror;
        p.cast();
        CHECK(play_until(rig, 120, [&] { return jumps_to(m, kLoopEnd) >= 1; }), "a fish bites");
        const size_t durability_calls = m.count_calls("ClientUpdateItemDurability");
        rig.run(seconds(1));
        for (int i = 0; i < 3; ++i) p.move(300);
        rig.run(seconds(8));
        CHECK(m.count_calls("ClientUpdateItemDurability") == durability_calls && rod_durability(p) == 150.0f && p.s->fishing_catches() + p.s->fishing_empty() == 0, "the player walked away before the result: the fish is lost, the rod did not wear");
        CHECK(jumps_to(m, kAbilityEnd) == 1, "the cast ended with the branch");
    }
}

/// Two players fish side by side.
static void test_two_players(const char* path) {
    std::printf("two players on the same lake\n");
    Rig rig;
    if (!rig.open(path, true, 0xC0FFEEull)) return;
    Player& a = rig.add_player(record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "alice", 150, 0, -300));
    Player& b = rig.add_player(record_of("0000000000000000000000000000cc01", "bob", 150, 0, 300));
    rig.settle();
    a.cast();
    rig.run(milliseconds(500));
    b.cast();
    rig.run(seconds(1));
    CHECK(a.s->fishing_casts() == 1 && b.s->fishing_casts() == 1 && a.mirror->fishing_items().size() == 1 && b.mirror->fishing_items().size() == 1, "each player has his own cast, item and buff");
    CHECK(a.mirror->effects_added.size() == 1 && b.mirror->effects_added.size() == 1, "each is told only of his own buff");
    CHECK(play_until(rig, 150, [&] { return a.s->fishing_bites() >= 2 && b.s->fishing_bites() >= 2; }), "both fish: two bites each");
    rig.run(seconds(5));
    const size_t a_results = a.s->fishing_catches() + a.s->fishing_empty();
    const size_t b_results = b.s->fishing_catches() + b.s->fishing_empty();
    CHECK(150.0f - rod_durability(a) == static_cast<float>(a_results) && 150.0f - rod_durability(b) == static_cast<float>(b_results), "each rod wore by its own player's results");
    CHECK(static_cast<size_t>(fish_held(a)) == a.s->fishing_catches() && static_cast<size_t>(fish_held(b)) == b.s->fishing_catches(), "each player holds his own fish");
    // alice walks away: bob's cast goes on
    const size_t b_bites = b.s->fishing_bites();
    for (int i = 0; i < 4; ++i) a.move(300);
    rig.run(seconds(2));
    CHECK(!a.s->fishing_active() && b.s->fishing_active(), "one player's movement ends his cast and not his neighbour's");
    CHECK(play_until(rig, 150, [&] { return b.s->fishing_bites() > b_bites; }), "bob's loop goes on");
}

/// The session ends in the middle of a cast; the player comes back.
static void test_reconnect(const char* path) {
    std::printf("a reconnect in the middle of a cast\n");
    Rig rig;
    if (!rig.open(path)) return;
    Player& p = rig.add_player(record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb"));
    rig.settle();
    p.cast();
    CHECK(play_until(rig, 120, [&] { return p.s->fishing_catches() + p.s->fishing_empty() >= 1; }), "a first result is saved");
    const float saved_rod = rod_durability(p);
    const size_t saved_fish = static_cast<size_t>(fish_held(p));
    // the next bite, then the session ends before its result
    const size_t bites = p.s->fishing_bites();
    CHECK(play_until(rig, 120, [&] { return p.s->fishing_bites() > bites; }), "a second fish bites");
    p.s->finish("the test ends the session");
    const size_t results_at_end = p.s->fishing_catches() + p.s->fishing_empty();
    rig.run(seconds(10));
    CHECK(p.s->fishing_catches() + p.s->fishing_empty() == results_at_end && !p.s->fishing_active(), "nothing is delivered after the session ended");
    CHECK(!rig.sink.inventories.empty(), "the inventory was saved");
    // the new session: the character as the service saved it
    svc::Character back = record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb", -1);
    if (!rig.sink.inventories.empty()) back.items = rig.sink.inventories.back().second;
    const svc::InventoryItem* rod = nullptr;
    for (const svc::InventoryItem& it : back.items) {
        if (it.container == Worn && it.slot == 35) rod = &it;
    }
    CHECKF(rod && rod->durability == saved_rod, "the rod comes back with the durability the last result left (%.0f)", rod ? rod->durability : -1.0f);
    Player& q = rig.add_player(back);
    rig.settle();
    CHECK(q.mirror->fishing_items().empty() && q.mirror->effects_added.empty() && !q.s->fishing_active(), "the new session starts with no cast, no item, no buff");
    CHECK(static_cast<size_t>(fish_held(q)) == saved_fish, "the fish caught before are in the bag");
    q.cast();
    rig.run(milliseconds(300));
    CHECK(q.s->fishing_casts() == 1 && q.mirror->fishing_items().size() == 1 && q.mirror->effects_added.size() == 1, "and he can cast again at once");
}

/// The experience: a catch raises the profession's experience by the data's amount and a level when it reaches one.
static void test_experience(const char* path) {
    std::printf("the profession's experience and level\n");
    Rig rig;
    if (!rig.open(path)) return;
    svc::Character rec = record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb");
    const professions::Book* book = rig.world->professions();
    const professions::Profession* fishing = book ? book->named("Fishing") : nullptr;
    CHECK(fishing != nullptr, "the world has the Fishing profession");
    if (!fishing) return;
    // one point short of the second level
    int64_t need = 0;
    for (int64_t xp = 1; xp < 100000; ++xp) {
        if (book->level_of(*fishing, xp, 0) >= 2) {
            need = xp;
            break;
        }
    }
    CHECK(need > 1, "the second level needs experience");
    svc::ProfessionState ps;
    ps.profession_guid = id::kFishing;
    ps.certification_tier = 0;
    ps.level = 1;
    ps.xp = need - 1;
    rec.professions.push_back(ps);
    Player& p = rig.add_player(rec);
    rig.settle();
    CHECK(fishing_xp(p) == need - 1 && p.s->professions().level(id::kFishing) == 1, "the character starts one point short of level 2");
    const size_t xp_before = p.mirror->xp_updates;
    p.cast();
    CHECK(play_until(rig, 300, [&] { return p.s->fishing_catches() >= 1; }), "a fish is caught");
    rig.run(seconds(1));
    CHECK(fishing_xp(p) > need - 1 && p.s->professions().level(id::kFishing) == 2, "the catch's experience takes the profession to level 2");
    CHECK(p.mirror->xp_updates > xp_before, "the XPTracker update told the client");
    CHECK(count_in_log("Fishing +") >= 1, "the log names the profession");
}

/// A Fishing row for a character certified at `tier` (0 Novice ... 4 Grand Master), at the tier's first level with the experience that level needs.
static svc::ProfessionState fishing_row(const professions::Book& book, int tier) {
    svc::ProfessionState ps;
    ps.profession_guid = id::kFishing;
    ps.certification_tier = tier;
    ps.level = book.tier(tier).min_level;
    if (const professions::Profession* pf = book.named("Fishing")) {
        int64_t xp = 0;
        while (book.level_of(*pf, xp, tier) < ps.level && xp < 50'000'000) xp += 25;
        ps.xp = xp;
    }
    return ps;
}

/// The certification and the rod's tier, through real sessions: the tier tables' predicates read the character's Fishing certification and the worn rod's tier, so an Apprentice with the
/// tier 1 rod brings Salmon up and one with the Starter rod does not; a Novice may not use the tier 1 rod at all.
static void test_certification(const char* path) {
    std::printf("the certification and the rod's tier, through a session\n");
    const size_t unusable0 = count_in_log("the cast is refused (tool not usable)");
    {   // a Novice with the tier 1 rod: the character may not use it
        Rig rig;
        if (!rig.open(path)) return;
        Player& p = rig.add_player(record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb", 150, 0, 0, 0, id::kPoleT2));
        rig.settle();
        const int32_t id0 = p.cast();
        rig.run(seconds(2));
        CHECK(ended_with(*p.mirror, id0) && p.s->fishing_casts() == 0 && p.s->fishing_refused() == 1 && p.mirror->fishing_items().empty(), "a Novice with the tier 1 rod: the cast is ended, no item, no buff");
        CHECK(count_in_log("the cast is refused (tool not usable)") == unusable0 + 1, "the log says the rod needs a higher certification");
    }
    {   // an Apprentice with the tier 1 rod: the second table, Salmon among the fish
        Rig rig;
        if (!rig.open(path, true, 0x5A1A0ull)) return;
        svc::Character rec = record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb", 150, 0, 0, 0, id::kPoleT2);
        rec.professions.push_back(fishing_row(*rig.world->professions(), 1));
        Player& p = rig.add_player(rec);
        rig.settle();
        CHECK(p.s->professions().state(id::kFishing).tier == 1, "the character is an Apprentice in Fishing");
        p.cast();
        const bool salmon = play_until(rig, 2500, [&] { return held_of(p, id::kSalmon) >= 1; });
        CHECKF(salmon, "an Apprentice with the tier 1 rod catches Salmon (%zu results, %zu catches)", p.s->fishing_catches() + p.s->fishing_empty(), p.s->fishing_catches());
        const int fish = held_of(p, id::kBluegill) + held_of(p, id::kBass) + held_of(p, id::kSalmon);
        int stray = 0;
        for (const svc::InventoryItem& it : p.s->inventory().items()) {
            if (it.container != Worn && it.item_record_id != id::kBluegill && it.item_record_id != id::kBass && it.item_record_id != id::kSalmon && items::is_resource(it.container)) ++stray;
        }
        CHECK(fish >= 1 && stray == 0, "...and nothing but Bluegill, Largemouth Bass and Salmon in the resource bags");
    }
    {   // an Apprentice with the Starter rod: the first table, never a Salmon
        Rig rig;
        if (!rig.open(path, true, 0x5A1A1ull)) return;
        svc::Character rec = record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb");
        rec.professions.push_back(fishing_row(*rig.world->professions(), 1));
        Player& p = rig.add_player(rec);
        rig.settle();
        p.cast();
        play_until(rig, 700, [&] { return p.s->fishing_catches() >= 8; });
        CHECKF(p.s->fishing_catches() >= 8 && held_of(p, id::kSalmon) == 0, "an Apprentice with the Starter rod fishes the first table: %zu catches, no Salmon", p.s->fishing_catches());
    }
}

/// A promotion while a cast runs: the session's profession moves (the GM artisan scroll, the same state a certifier changes) and the result path reads it at that moment; the
/// new tier is the one the next cast's rod check asks (the tier 1 rod a Novice may not use).
static void test_promotion_while_fishing(const char* path) {
    std::printf("a promotion while a cast runs\n");
    constexpr int64_t kFishingScroll = 6064634616667242496ll;   // GM_Scroll_MakeMeLevel15Artisanship_Fishing
    Rig rig;
    if (!rig.open(path)) return;
    svc::Character rec = record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb");
    rec.items.push_back(make_item(ItemContainer::Backpack, 0, kFishingScroll, 300));
    rec.items.push_back(make_item(ItemContainer::Backpack, 1, id::kPoleT2, 301));
    Player& p = rig.add_player(rec);
    rig.settle();
    CHECK(p.s->professions().valid() && p.s->professions().state(id::kFishing).tier == 0, "the character is a Novice in Fishing");
    p.cast();
    rig.run(seconds(1));
    CHECK(p.s->fishing_active() && p.s->fishing_casts() == 1, "a cast is running");
    const size_t applied = p.s->uses_applied();
    p.client_sends({p.client.actor(p.pawn_ch, item_used(3, 0, guid_n(300)), true)});
    rig.run(seconds(1));
    CHECK(p.s->uses_applied() == applied + 1 && p.s->professions().state(id::kFishing).tier == 1,
          "the Fishing Level 15 scroll is used while the cast runs: the profession is an Apprentice now");
    CHECK(p.s->fishing_casts() == 1 && p.s->fishing_active(), "the running cast was not cancelled by the promotion");
    const bool result = play_until(rig, 4000, [&] { return p.s->fishing_catches() + p.s->fishing_empty() >= 1; });
    CHECKF(result && p.s->fishing_casts() == 1, "the result comes after the promotion (%zu catch(es), %zu with nothing)", p.s->fishing_catches(), p.s->fishing_empty());
    // the tier the promotion reached is the one the rod check reads: a tier 1 rod is refused to a Novice, taken at Apprentice
    p.item_off(35, 135);   // the Starter pole comes off (the cast ends with it)
    rig.run(seconds(3));
    p.client_sends({p.client.actor(p.pawn_ch, item_used(3, 1, guid_n(301)), true)});
    rig.run(milliseconds(300));
    const int32_t again = p.cast();
    rig.run(seconds(2));
    (void)again;
    CHECK(p.s->fishing_casts() == 2 && p.s->fishing_refused() == 0 && p.s->fishing_active(),
          "a cast with the tier 1 rod, refused to a Novice, is accepted at Apprentice");
}

/// The rod's last point of durability: the result that takes it ends the cast (the branch of a rod that is gone).
static void test_rod_wears_out(const char* path) {
    std::printf("a rod that wears out in the middle of a cast\n");
    const size_t gone0 = count_in_log("the rod is gone or broke"), broken0 = count_in_log("the cast is refused (tool broken)");   // (the log holds every test's lines)
    Rig rig;
    if (!rig.open(path)) return;
    Player& p = rig.add_player(record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb", /*rod=*/1));
    rig.settle();
    const Mirror& m = *p.mirror;
    p.cast();
    CHECK(play_until(rig, 150, [&] { return p.s->fishing_catches() + p.s->fishing_empty() >= 1; }), "the one result comes");
    rig.run(seconds(4));
    CHECK(rod_durability(p) == 0.0f, "the rod has no durability left");
    CHECK(jumps_to(m, kAbilityEnd) == 1 && !p.s->fishing_active() && count_in_log("the rod is gone or broke") == gone0 + 1, "the cast ends with the branch of the rod that is gone");
    CHECK(m.effects_removed.size() == 1, "the buff goes");
    rig.run(seconds(60));
    CHECK(p.s->fishing_bites() == 1 && p.s->fishing_catches() + p.s->fishing_empty() == 1, "no second bite, no second result");
    const int32_t again = p.cast();
    rig.run(seconds(2));
    CHECK(ended_with(m, again) && count_in_log("the cast is refused (tool broken)") == broken0 + 1, "a cast with the broken rod is refused");
}

/// A death in the middle of a cast, and the camera's pitch.
static void test_death_and_the_look(const char* path) {
    std::printf("a death in the middle of a cast\n");
    {
        Rig rig;
        if (!rig.open(path)) return;
        Player& p = rig.add_player(record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb"));
        rig.settle();
        const Mirror& m = *p.mirror;
        p.cast();
        rig.run(seconds(5));
        CHECK(p.s->fishing_active() && m.effects_added.size() == 1, "the cast runs");
        p.s->take_damage(p.conn, 1.0e9, rig.t);
        rig.run(seconds(6));
        CHECK(p.s->dead(), "the character dies");
        CHECK(!p.s->fishing_active() && jumps_to(m, kAbilityEnd) == 1 && count_in_log("the player died") >= 1, "the cast ends with the branch");
        CHECK(!p.s->abilities_failed(), "the pawn's items took the cast's end: the abilities are still on");
        CHECK(m.effects_removed.size() == 1, "the fishing buff goes");
        rig.run(seconds(60));
        CHECK(p.s->fishing_bites() == 0 && p.s->fishing_catches() + p.s->fishing_empty() == 0 && rod_durability(p) == 150.0f, "nothing is caught or worn after the death");
    }
    std::printf("the camera's pitch\n");
    {   // the moves carry the camera's pitch (as the retail client's do); the lure's landing does not use it
        Rig rig;
        if (!rig.open(path)) return;
        Player& p = rig.add_player(record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb"));
        rig.settle();
        p.pitch = -28.0;
        p.move(0);
        p.move(0);
        CHECK(p.s->movement().good().pitch16 == static_cast<uint16_t>(std::llround(-28.0 / 360.0 * 65536.0) & 0xffff), "the move's control pitch is kept (-28 degrees)");
        p.cast();
        rig.run(seconds(1));
        CHECK(p.s->fishing_casts() == 1 && count_in_log("it comes down") >= 1 && p.s->fishing_failed() == 0, "a cast with the camera looking down still lands on the lake: the yaw alone aims it");
    }
}

int main() {
    spdlog::set_level(spdlog::level::info);
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(g_log);
    spdlog::default_logger()->sinks().push_back(sink);
    std::printf("test_worldd_fishing_session\n");
    const char* path = db_path();
    if (!path) {
        std::printf("  [skip] the content.db is not there (run scripts/extract_game_data.ps1)\n");
        return 0;
    }
    test_a_cast(path);
    test_dry_land(path);
    test_the_rules(path);
    test_the_branches(path);
    test_two_players(path);
    test_reconnect(path);
    test_experience(path);
    test_certification(path);
    test_promotion_while_fishing(path);
    test_rod_wears_out(path);
    test_death_and_the_look(path);
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
