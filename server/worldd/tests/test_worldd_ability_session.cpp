// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_ability_session.cpp: Sprint, Dodge and Rest through a PlayerSession, worldd plan A7
//
//  A fake client joins the captured way (the Harness of test_worldd_movement), sends the client's own
//  ServerTriggerAbility / ServerNotifyHeldAbilityCancelled (built as the captures are: ClientInstanceId counting every
//  trigger from 0, an empty ItemData) and ServerMovePacked, and reads the replies as the retail client does: the fast-array
//  deltas of the StatsComponent and the AbilityComponent and the ClientEndAbility calls.  The client's view (Mirror) is rebuilt
//  from those alone, so a test says what the HUD would show.
//
//  Sprint        a full bar lasts 20 s (80 charges, the first with the activation), the stamina falls 1.25 at a time, the
//                ability, its effect and MoveSpeedMult 1.2 appear (with the montage's export), and the run ends with the
//                items removed, MoveSpeedMult 1.0 and the DisableRegen effect, and then ClientEndAbility; the bar refills
//                in 20 s after the pause
//  Dodge         -50 at once with its items and cooldown; a second one in the cooldown and one under 50 are refused with
//                ClientEndAbility; the cooldown and the Evasion end on time; a Dodge in the middle of a Sprint leaves the
//                Sprint running, a Rest after one ends it with ClientEndAbility; the dash is allowed to the move checker
//  instances     ServerInstanceId is the server's counter, ClientEndAbility and cancels carry the client's id
//  a cancel      stops the drain without ClientEndAbility
//  Rest          the ability item with the montage's export, the effect after the sit-down and the regeneration it
//                raises, the end
//  refusals      an ability worldd does not run gets ClientEndAbility and nothing else; a trigger whose parameters do not
//                read is refused from its first two parameters when it is not Sprint, Dodge or Rest, and ignored when it is;
//                a flood of triggers is dropped for its budget
//  late acks     a client that hears nothing for 3 s: the montage's export rides in every update until it is acknowledged,
//                the updates arrive late and in order; a server loop that stalls for 15 s sends one update, not ten
//  windows       a trigger before the join is on the wire, and an update behind a full window, wait and are not lost
//  the log       the first 100 ability transitions at info; abilities off, with the pawn's state drifted, is a warning
//  endpoint      a whole Sprint driven by the Endpoint's own ticks and datagrams
//  hostile       a seeded hostile client: random operations with hostile ids, stalls and held acks, and triggers and
//                cancels damaged bit by bit; the invariants (Stamina in range, nothing failed, the client's view is the
//                runtime's once settled) hold
//
//  With AOC_CONTENT_DB set the sessions run on the real content.db instead of the small fixture (the numbers are the same).
// ============================================================================
#include "ability_rig.h"
#include "combat_rig.h"
#include "tooltip_key.h"
#include "bootstrap/captured_setup.h"
#include "bootstrap/constants.h"
#include "capture_abilities.h"
#include "capture_blocks.h"
#include "capture_packets.h"
#include "content_db_fixture.h"
#include "cyb_appearance.h"
#include "fake_client.h"
#include "net/endpoint.h"
#include "protocol/repl/ability_rpc.h"
#include "protocol/repl/hit_rpc.h"
#include "protocol/repl/move_packed.h"
#include "protocol/repl/replicator.h"
#include "protocol/repl/rpc_fields.h"
#include "protocol/repl/storage_rpc.h"
#include "protocol/repl/vendor_rpc.h"
#include "protocol/schema/generated/index.h"
#include "session/player_session.h"
#include "spawns/area.h"
#include "world/relevancy.h"
// C.O.A.C_Y_B

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <random>
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
using emit::BunchWriter;
using std::chrono::milliseconds;
using std::chrono::seconds;

static std::ostringstream g_log;
static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)
#define CHECKF(cond, ...) do { \
    if (cond) { std::printf("  [ok ] "); std::printf(__VA_ARGS__); std::printf("\n"); g_pass++; } \
    else { std::printf("  [FAIL] "); std::printf(__VA_ARGS__); std::printf("\n"); g_fail++; } \
} while (0)

static constexpr int64_t kTempHealth = 6064629069966082048ll;
static const std::string kCyb = "bbec5c86eb101fe5f38c87f0fa22e7d0";
static const std::string kMachine = "DESKTOP-EXAMPLE1-US-0123456789ABCDEF0123456789ABCDEF";
static const Clock::time_point t0{seconds(5000)};
static constexpr uint32_t kServerId = 60;
static constexpr uint32_t kRandomizer = 0x6ee87ee4u;

static constexpr double kX = -777664.5, kY = 616578.6875, kZ = 15952.0498046875;
static constexpr uint16_t kSpawnYaw = 16550;
// C:Y:B captures, C.O.A code

// the records and abilities of the captures
static constexpr int64_t kStamina = 6064629575339934476ll;
static constexpr int64_t kHealthRegen = 109343399171806ll;
static constexpr int64_t kManaRegen = 109343399171796ll;
static constexpr int64_t kSprint = 6064631641648332884ll;
static constexpr int64_t kDodge = 107258680465016ll;
static constexpr int64_t kRest = 109393746155128ll;
static constexpr int64_t kScepterCombo = 6064631845843960261ll;
static constexpr int64_t kQuickWit = 6064630736949673984ll;
static constexpr int64_t kNotRun = 4242424242ll;   // no table has it: an ability no runtime runs
static constexpr int64_t kStaminaRegeneration = 6064631663405498419ll;
static constexpr int64_t kRestEffect = 109394092054139ll;
static constexpr int64_t kSprintEffect = 6064631641826394197ll;
static constexpr int64_t kEvasionEffect = 106223481149050ll;
static constexpr int64_t kDisableRegenEffect = 6064631668308443138ll;
static constexpr int64_t kMoveSpeedMult = 109228544644729ll;

static const schema::ClassSchema& class_named(const char* name) {
    const schema::ClassSchema* c = gen::find_class_schema(name);
    if (!c) {
        std::printf("  [FAIL] no schema %s\n", name);
        std::exit(2);
    }
    return *c;
}

struct Fixture {
    fs::path dir;
    std::optional<content::ContentDb> content;
    aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
    std::unique_ptr<PlayerWorld> world;

    /// The small content.db of content_db_fixture.h, or, with AOC_CONTENT_DB set, the real file (the numbers are the same; the
    /// session then runs on contentc's own rows).
    /// `real`: the real content.db of the working tree (plan D2's tests: the ability tables are only there), whatever AOC_CONTENT_DB says.
    explicit Fixture(bool use_real = false) {
        dir = content_fixture::make_temp_dir("worldd_ability_session_");
        fs::path db = dir / "content.db";
        const char* real = std::getenv("AOC_CONTENT_DB");
        if (use_real) db = ability_rig::real_db_path();
        else if (real && *real) db = real;
        else if (!content_fixture::make_db(db)) std::exit(2);
        std::string error;
        content = content::ContentDb::open(db.string(), error);
        if (!content) {
            std::printf("  [FAIL] the content.db does not open: %s\n", error.c_str());
            std::exit(2);
        }
        world = std::make_unique<PlayerWorld>(&*content, "", bs::GameStateMode::Tier2, kServerId, guids);
        if (!world->ready()) {
            std::printf("  [FAIL] the world is not ready: %s\n", world->content_problem().c_str());
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
};

static svc::Character cyb_record() {
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
    return c;
}

static bs::LoginIdentity identity() { return {kMachine, kMachine}; }
/// The server's world time (what the endpoint passes to the join and to PlayerWorld::tick_world: seconds since worldd started).  A test that wants worldd
/// older than the session sets the offset (E1 stage 3 fix 2: the live server had run 102 s when the user joined; the harness's t0 hid that).
static double g_world_time_offset = 0.0;
static double world_time_at(Clock::time_point t) { return std::chrono::duration<double>(t - t0).count() + g_world_time_offset; }

// ---- the client's side ------------------------------------------------------------------------------------------------------

static repl::CodecOptions client_options() {
    repl::CodecOptions o;
    o.inline_objects = true;
    return o;
}

static repl::NetworkMove move_at(float ts, double x, bool moving) {
    repl::NetworkMove m;
    m.time_stamp = ts;
    m.acceleration = repl::quantize_vector(10, moving ? 100 : 0, 0, 0);
    m.location = repl::quantize_vector(100, x, kY, kZ);
    m.control_rotation.yaw = kSpawnYaw;
    m.movement_mode = repl::kMoveWalking;
    m.input_bits = moving ? 1 : 0;   // the input direction: forward
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
// -- CoA x C_Y_B --

/// ServerTriggerAbility as the retail client sends it: the ClientInstanceId omitted when 0, no animation, an input of (1, 0), no
/// targets and ItemData "no item" (Slot -1, the empty FItemInstance, Location 24).
static BunchWriter trigger_bunch(const emit::FIntrepidNetworkGUID& ability, int64_t guid, int32_t id) {
    const schema::RpcSchema* rpc = class_named("UAoCAbilityComponent").rpc("ServerTriggerAbility");
    repl::RpcParams p;
    bool ok = rpc && repl::set_rpc_param(*rpc, p, "AbilityGuid", repl::Value::i64(guid));
    if (ok && id != 0) ok = repl::set_rpc_param(*rpc, p, "ClientInstanceId", repl::Value::i64(id));
    ok = ok && repl::set_rpc_param(*rpc, p, "InputVector", repl::Value(repl::Vec2d{1, 0}));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.Slot", repl::Value::i64(-1));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.ItemInstance", repl::Value(repl::ItemInstance{}));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.Location", repl::Value::u64(24));
    if (!ok) {
        std::printf("  [FAIL] the trigger's parameters do not fit\n");
        return BunchWriter();
    }
    return ability_rpc(ability, "ServerTriggerAbility", p);
}

static BunchWriter cancel_bunch(const emit::FIntrepidNetworkGUID& ability, int32_t id) {
    const schema::RpcSchema* rpc = class_named("UAoCAbilityComponent").rpc("ServerNotifyHeldAbilityCancelled");
    repl::RpcParams p;
    bool ok = rpc && repl::set_rpc_param(*rpc, p, "CancelledTime", repl::Value::f32(0.25f));
    if (ok && id != 0) ok = repl::set_rpc_param(*rpc, p, "ClientInstanceId", repl::Value::i64(id));
    ok = ok && repl::set_rpc_param(*rpc, p, "CancelSectionIndex", repl::Value::i64(-1));
    if (!ok) {
        std::printf("  [FAIL] the cancel's parameters do not fit\n");
        return BunchWriter();
    }
    return ability_rpc(ability, "ServerNotifyHeldAbilityCancelled", p);
}


/// RequestHitVerification_Server as the retail client sends it for a hit of `ability`: the caster's collision cylinder as its one component (or none).
static BunchWriter hit_request_bunch(const bs::PlayerSetup& setup, int64_t ability, int64_t targeting, int64_t hit, bool with_component = true) {
    const schema::RpcSchema* rpc = class_named("UAoCAbilityComponent").rpc("RequestHitVerification_Server");
    repl::HitRequest req;
    if (with_component) {
        repl::HitComponentRef c;
        c.object = setup.guids.collision_cylinder;
        c.server_timestamp = 1.0f;
        req.components.push_back(c);
    }
    req.ability_guid = ability;
    req.targeting_guid = targeting;
    req.hit_guid = hit;
    const auto params = rpc ? repl::hit_request_params(*rpc, req) : std::nullopt;
    if (!params) {
        std::printf("  [FAIL] the hit request's parameters do not fit\n");
        return BunchWriter();
    }
    return ability_rpc(setup.guids.ability, "RequestHitVerification_Server", *params);
}

// ---- the client's view, rebuilt from what the server sent ----------------------------------------------------------------

static repl::CodecOptions stat_options() {
    repl::CodecOptions o;
    o.stat_int32_extended = [](int64_t record) {
        const uint32_t hi = static_cast<uint32_t>(static_cast<uint64_t>(record) >> 32);
        return hi == 0x5429e8c0u || hi == 0x5429e8c4u;
    };
    return o;
}

static std::vector<int32_t> ints(std::initializer_list<int32_t> l) { return l; }

struct Mirror {
    bs::PlayerSetup setup;
    std::map<int64_t, float> stats;                 // record -> value
    struct ActiveAbility {
        int64_t ability = 0, instance = 0;
        float   montage_position = 0;
        bool    may_jump = false;
        int32_t animation_counter = 0;
        bool    has_target = false, has_montage = false;
        std::string montage_group;
        uint16_t    mesh_yaw = 0;
        uint64_t    montage = 0;
    };
    std::map<int32_t, ActiveAbility> abilities;      // ElementID -> the item
    std::map<int32_t, int64_t> effects;              // ElementID -> EffectGUID
    std::map<int32_t, int64_t> cooldowns;            // ElementID -> AbilityGuid
    std::vector<std::pair<double, float>> stamina;   // (time, value) of every Stamina item received
    std::vector<std::pair<double, int32_t>> ends;    // (time, ClientInstanceId) of every ClientEndAbility
    bool   end_before_removal = false;               // a ClientEndAbility came while its instance was still listed
    std::vector<repl::HitInstance> hits;             // plan D2, stage 2: every hit the client was confirmed (ClientHitConfirmationsReliable), in order
    std::vector<std::pair<float, float>> shields;    // the TempHealth item's elements: (Base, Value)
    size_t hit_calls = 0;
    size_t unreadable = 0, chains = 0, chains_with_exports = 0;
    /// ClientInstanceId -> ServerInstanceId of the triggers the server accepted (the Run records them): the item has the server's
    /// own number, ClientEndAbility the client's.
    std::map<int32_t, int64_t> server_of;
    /// plan D2, review: a chain on a channel that is none of the four players' actors: a lingering zone's actor (its open, its bFadingOut update, its close)
    struct ChannelEvent {
        uint32_t channel = 0;
        bool     open = false, close = false;
        double   at = 0;
        size_t   bits = 0, exports = 0;
    };
    std::vector<ChannelEvent> other;
// [C | O | A + Cyb]

    // ---- plan E1, stage 3, part 2: what the client was told of a kill, a loot window and its own death
    std::vector<repl::LootPayload>      loot_windows;     // ClientReceiveLootStoragePayload, in order
    std::vector<repl::ItemStorageEvent> storage_events;   // ClientReceiveStorageEvent, in order
    std::vector<std::string>            pc_calls;         // the PlayerController's decoded calls, in order of arrival
    std::vector<double>                 set_times;        // E1 stage 3 fix: every ClientSetTime's NewSynchronizedTime, in order
    std::vector<std::string>            pawn_calls;       // the pawn's own decoded calls, in order of arrival
    size_t listeners_removed = 0, xp_updates = 0;
    std::vector<std::array<double, 3>>  death_locations;  // ClientSetLastDeathLocation
    struct Choices {
        std::vector<int64_t> closest;
        int64_t              home = 0;
        bool                 previous_invalid = false;
    };
    std::vector<Choices> choices;                          // SendClientRespawnChoices

    explicit Mirror(const bs::PlayerSetup& s) : setup(s) {
        for (const bs::StatArray* a : {&s.stats.owner, &s.stats.owner_proxy, &s.stats.everyone_proxy}) {
            for (const bs::StatItem& it : a->items) stats[it.record] = std::bit_cast<float>(it.value);
        }
    }

    float stamina_now() const {
        const auto it = stats.find(kStamina);
        return it == stats.end() ? -1.0f : it->second;
    }
    bool has_ability(int64_t guid) const {
        for (const auto& [id, a] : abilities)
            if (a.ability == guid) return true;
        return false;
    }
    bool has_effect(int64_t guid) const {
        for (const auto& [id, g] : effects)
            if (g == guid) return true;
        return false;
    }
    std::vector<int32_t> end_ids() const {
        std::vector<int32_t> out;
        for (const auto& e : ends) out.push_back(e.second);
        return out;
    }

    /// A chain of the PlayerController's channel: its ClientHitConfirmations calls.
    void absorb_pc(const fake::Delivered& d) {
        if (d.open || d.close || d.content.bit_pos() <= 8) return;
        const auto views = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
        if (!views) return;
        for (const repl::BlockView& v : *views) {
            if (!v.header.is_actor && v.header.object == setup.guids.xp_tracker) ++xp_updates;
            if (!v.header.is_actor || v.header.has_rep_layout) continue;
            size_t p = v.payload_pos;
            const auto calls = repl::read_rpc_fields(class_named("AoCPlayerControllerBP_C"), d.content.data(), v.payload_pos + v.payload_bits, p);
            if (!calls) continue;
            for (const repl::RpcCall& c : *calls) {
                if (c.decoded) pc_calls.push_back(c.rpc->name);
                if (c.decoded && std::string(c.rpc->name) == "ClientSetTime") {
                    const repl::Value* v = repl::rpcf::value_of(*c.rpc, c.params, "NewSynchronizedTime");
                    if (v && v->get_if<double>()) set_times.push_back(*v->get_if<double>());
                    else ++unreadable;
                    continue;
                }
                if (c.decoded && std::string(c.rpc->name) == "ClientReceiveLootStoragePayload") {
                    if (const auto w = repl::parse_loot_payload(c)) loot_windows.push_back(*w);
                    else ++unreadable;
                    continue;
                }
                if (c.decoded && std::string(c.rpc->name) == "ClientReceiveStorageEvent") {
                    if (const auto e = repl::parse_storage_event(c)) storage_events.push_back(*e);
                    else ++unreadable;
                    continue;
                }
                if (c.decoded && std::string(c.rpc->name) == "ClientRemoveListenerFromStorage") {
                    ++listeners_removed;
                    continue;
                }
                if (!c.decoded || std::string(c.rpc->name) != "ClientHitConfirmationsReliable") continue;
                const auto conf = repl::parse_hit_confirmations(c);
                if (!conf) {
                    ++unreadable;
                    continue;
                }
                ++hit_calls;
                for (const repl::HitInstance& h : conf->hits) hits.push_back(h);
            }
        }
    }
// Cyb + C.O.A

    void absorb(const fake::Delivered& d, double at) {
        const auto views = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
        if (!views) {
            ++unreadable;
            return;
        }
        ++chains;
        if (!d.exports.empty()) ++chains_with_exports;
        for (const repl::BlockView& v : *views) {
            if (v.header.is_actor) {   // the pawn's own blocks: the move acknowledgements, and since E1 stage 3 the death's calls
                if (!v.header.has_rep_layout) pawn_calls_of(d, v);
                continue;
            }
            const size_t end = v.payload_pos + v.payload_bits;
            size_t p = v.payload_pos;
            if (v.header.object == setup.guids.stats) {
                const auto fields = repl::read_fast_array_fields(class_named("UAoCStatsComponent"), d.content.data(), end, p, stat_options());
                if (!fields) {
                    ++unreadable;
                    continue;
                }
                for (const repl::FastArrayField& f : *fields) stats_field(f, at);
            } else if (v.header.object == setup.guids.ability) {
                const auto fields = repl::read_fast_array_fields(class_named("UAoCAbilityComponent"), d.content.data(), end, p, stat_options());
                if (fields) {
                    for (const repl::FastArrayField& f : *fields) ability_field(f);
                    continue;
                }
                p = v.payload_pos;
                const auto calls = repl::read_rpc_fields(class_named("UAoCAbilityComponent"), d.content.data(), end, p);
                if (!calls) {
                    ++unreadable;
                    continue;
                }
                for (const repl::RpcCall& c : *calls) {
                    if (!c.decoded || std::string(c.rpc->name) != "ClientEndAbility") continue;
                    const schema::RepCommand* cmd = repl::rpc_command(*c.rpc, "ClientInstanceId");
                    const auto it = cmd ? c.params.values.find(cmd->handle) : c.params.values.end();
                    const int32_t id = it != c.params.values.end() && it->second.get_if<int64_t>() ? static_cast<int32_t>(*it->second.get_if<int64_t>()) : 0;
                    const auto known = server_of.find(id);
                    for (const auto& [e, a] : abilities)
                        if (known != server_of.end() && a.instance == known->second) end_before_removal = true;
                    ends.emplace_back(at, id);
                }
            }
        }
    }

private:
    void pawn_calls_of(const fake::Delivered& d, const repl::BlockView& v) {
        size_t p = v.payload_pos;
        const auto calls = repl::read_rpc_fields(class_named("PlayerPawn_C"), d.content.data(), v.payload_pos + v.payload_bits, p);
        if (!calls) return;
        for (const repl::RpcCall& c : *calls) {
            if (!c.decoded) continue;
            const std::string name = c.rpc->name;
            pawn_calls.push_back(name);
            const auto param = [&](const char* n) -> const repl::Value* {
                const schema::RepCommand* cmd = repl::rpc_command(*c.rpc, n);
                const auto it = cmd ? c.params.values.find(cmd->handle) : c.params.values.end();
                return it == c.params.values.end() ? nullptr : &it->second;
            };
            if (name == "ClientSetLastDeathLocation") {
                const repl::Value* l = param("Location");
                const repl::Vec3d* v3 = l ? l->get_if<repl::Vec3d>() : nullptr;
                if (v3) death_locations.push_back({v3->x, v3->y, v3->z});
            } else if (name == "SendClientRespawnChoices") {
                Choices ch;
                const repl::Value* list = param("RespawnChoices.ClosestLegalRespawns");
                const repl::ArrayValue* arr = list ? list->get_if<repl::ArrayValue>() : nullptr;
                if (arr) {
                    for (const repl::ElementState& e : arr->elements) ch.closest.push_back(i64(e, 1));
                }
                const repl::Value* home = param("RespawnChoices.HomeRespawn");
                ch.home = home && home->get_if<int64_t>() ? *home->get_if<int64_t>() : 0;
                const repl::Value* inv = param("RespawnChoices.bPreviousChoiceInvalid");
                ch.previous_invalid = inv && inv->get_if<bool>() && *inv->get_if<bool>();
                choices.push_back(std::move(ch));
            }
        }
    }

    static const repl::Value* val(const repl::ElementState& e, uint16_t handle) {
        const auto it = e.values.find(handle);
        return it == e.values.end() ? nullptr : &it->second;
    }
    static int64_t i64(const repl::ElementState& e, uint16_t handle) {
        const repl::Value* v = val(e, handle);
        return v && v->get_if<int64_t>() ? *v->get_if<int64_t>() : 0;
    }

    void stats_field(const repl::FastArrayField& f, double at) {
        const std::string name = class_named("UAoCStatsComponent").field(f.field)->name;
        if (name == "CurrentEffects") {
            for (const int32_t id : f.delta.deleted) effects.erase(id);
            for (const auto& c : f.delta.changed) effects[c.id] = i64(c.values, 2);
            return;
        }
        if (name == "StatRepFloatArrayEveryoneProxy") {
            for (const auto& c : f.delta.changed) {
                if (i64(c.values, 1) != kTempHealth) continue;
                shields.clear();
                const repl::Value* values = val(c.values, 2);
                const repl::ArrayValue* arr = values ? values->get_if<repl::ArrayValue>() : nullptr;
                if (!arr) continue;
                for (const repl::ElementState& e : arr->elements) {
                    const repl::Value* base = val(e, 5);
                    const repl::Value* value = val(e, 6);
                    shields.emplace_back(base && base->get_if<float>() ? *base->get_if<float>() : -1.0f, value && value->get_if<float>() ? *value->get_if<float>() : -1.0f);
                }
            }
            return;
        }
        for (const auto& c : f.delta.changed) {
            const repl::Value* v = val(c.values, 1);
            const repl::StatInt32Inst* item = v ? v->get_if<repl::StatInt32Inst>() : nullptr;
            if (!item) continue;
            stats[item->record] = std::bit_cast<float>(item->value);
            if (item->record == kStamina) stamina.emplace_back(at, std::bit_cast<float>(item->value));
        }
    }
// CoA : C.Y.B

    void ability_field(const repl::FastArrayField& f) {
        const std::string name = class_named("UAoCAbilityComponent").field(f.field)->name;
        if (name == "Cooldowns") {
            for (const int32_t id : f.delta.deleted) cooldowns.erase(id);
            for (const auto& c : f.delta.changed) cooldowns[c.id] = i64(c.values, 1);
            return;
        }
        if (name != "ReplicatedActiveAbilities") return;
        for (const int32_t id : f.delta.deleted) abilities.erase(id);
        for (const auto& c : f.delta.changed) {
            ActiveAbility a;
            a.ability = i64(c.values, 1);
            a.instance = i64(c.values, 2);
            const repl::Value* targets = val(c.values, 3);
            a.has_target = targets && targets->get_if<repl::ArrayValue>() && !targets->get_if<repl::ArrayValue>()->elements.empty();
            const repl::Value* montage = val(c.values, 11);
            a.has_montage = montage && montage->get_if<emit::FIntrepidNetworkGUID>() && montage->get_if<emit::FIntrepidNetworkGUID>()->ObjectId != 0;
            const repl::Value* group = val(c.values, 10);
            a.montage_group = group && group->get_if<repl::NameValue>() ? group->get_if<repl::NameValue>()->text : std::string();
            a.montage = montage && montage->get_if<emit::FIntrepidNetworkGUID>() ? montage->get_if<emit::FIntrepidNetworkGUID>()->ObjectId : 0;
            const repl::Value* yaw = val(c.values, 16);
            a.mesh_yaw = yaw && yaw->get_if<uint64_t>() ? static_cast<uint16_t>(*yaw->get_if<uint64_t>()) : 0;
            const repl::Value* pos = val(c.values, 13);
            a.montage_position = pos && pos->get_if<float>() ? *pos->get_if<float>() : 0.0f;
            const repl::Value* jump = val(c.values, 14);
            a.may_jump = jump && jump->get_if<bool>() && *jump->get_if<bool>();
            a.animation_counter = static_cast<int32_t>(i64(c.values, 15));
            abilities[c.id] = a;
        }
    }
};

// ---- the harness: a session, a connection and a fake client --------------------------------------------------------------

struct Harness {
    Fixture fx;
    Connection conn{ConnectionConfig{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870}, t0};
    fake::Client client{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870};
    std::unique_ptr<PlayerSession> s;
    Clock::time_point t = t0;
    std::vector<fake::Delivered> got;
    std::vector<std::vector<uint8_t>> held;   // server packets the client has not been given yet (hold)
    bool hold = false;
    std::string error;
    uint32_t pawn_ch = 0, pc_ch = 0;

    /// `clog_pc_channel` bunches of filler on channel 3, held (the client acknowledges nothing): the PlayerController's chains of
    /// the join do not all fit the window, so the join stays queued until release() and settle().
    explicit Harness(size_t clog_pc_channel = 0, bool real = false, const svc::Character* record = nullptr, const std::function<void(PlayerWorld&)>& prepare = {}) : fx(real) {
        if (prepare) prepare(*fx.world);   // (plan E1, stage 3: the NPCs' ground and the fights are the world's before the session joins)
        if (clog_pc_channel) {
            hold = true;
            filler(3, clog_pc_channel);
        }
        s = PlayerSession::join(conn, *fx.world, record ? *record : cyb_record(), identity(), world_time_at(t), error);
        if (!s) {
            std::printf("  [FAIL] the session does not join: %s\n", error.c_str());
            std::exit(2);
        }
        pawn_ch = s->channel_of(bs::Role::Pawn);
        pc_ch = s->channel_of(bs::Role::PlayerController);
        if (!clog_pc_channel) settle();
    }
    /// `n` raw one-byte reliable bunches on `channel` (Connection::send queues them without opening anything): the client
    /// gets them and ignores them, and while they are unacknowledged they take up the channel's window.
    void filler(uint32_t channel, size_t n) {
        conn::BunchHeader header;
        header.channel = channel;
        header.reliable = true;
        header.name_index = Connection::kActorChannelName;
        BunchWriter one;
        one.write_bits(0xA5, 8);
        for (size_t i = 0; i < n; ++i) conn.send(header, one);
    }
    void deliver(const std::vector<uint8_t>& pkt) {
        if (lose_to_client && lose_to_client(pkt)) {   // lost on the way: the client never saw it
            ++lost_to_client;
            return;
        }
        const auto r = client.receive(pkt);
        for (const auto& d : r.delivered) got.push_back(d);
    }
    void flush() {
        for (auto& pkt : conn.flush(t)) {
            if (hold) held.push_back(pkt);
            else deliver(pkt);
        }
    }
    /// A link that loses packets (a test sets these): called for each datagram on its way, true = lost.
    std::function<bool(const std::vector<uint8_t>&)> lose_to_client, lose_to_server;
    size_t lost_to_client = 0, lost_to_server = 0;
    /// The client is given what it had not seen.
    void release() {
        hold = false;
        for (const auto& pkt : held) deliver(pkt);
        held.clear();
    }
    void client_sends(std::vector<fake::Bunch> bunches) {
        const auto pkt = client.packet(std::move(bunches));
        if (lose_to_server && lose_to_server(pkt)) {
            ++lost_to_server;
            return;
        }
        const std::vector<InBunch> in = conn.receive(pkt, t);
        s->take_acks(conn);
        for (const InBunch& b : in) s->on_bunch(conn, b, t);
        s->end_of_datagram(conn);
    }
    void step(milliseconds d = milliseconds(20)) {
        t += d;
        s->tick(conn, t);
        flush();
        s->take_acks(conn);
        client_sends({});
    }
    void settle() {
        for (int quiet = 0, i = 0; quiet < 3 && i < 400; ++i) {
            const size_t before = got.size();
            step();
            quiet = (got.size() == before && s->join_pending() == 0 && s->replies_pending() == 0) ? quiet + 1 : 0;
        }
    }
    const emit::FIntrepidNetworkGUID& ability_guid() const { return s->bootstrap().setup().guids.ability; }
};

/// A harness and the client's view of it, with a move source and the client's own ClientInstanceId counter.
struct Run {
    Harness h;
    Mirror m;
    size_t seen;
    float ts = 100.0f;
    double x = kX;
    int32_t next_id = 0;   // the client counts every trigger of any ability from 0

    explicit Run(size_t clog_pc_channel = 0, bool real = false, const svc::Character* record = nullptr, const std::function<void(PlayerWorld&)>& prepare = {})
        : h(clog_pc_channel, real, record, prepare), m(h.s->bootstrap().setup()), seen(h.got.size()) {}
// built by C O A with Cyb

    double now() const { return world_time_at(h.t); }
    void absorb() {
        for (; seen < h.got.size(); ++seen) {
            // (the fillers of a test that clogs a window are one byte: not a chain of the server's)
            if (h.got[seen].channel == h.pawn_ch && h.got[seen].content.bit_pos() != 8) m.absorb(h.got[seen], now());
            if (h.got[seen].channel == h.pc_ch) m.absorb_pc(h.got[seen]);
            const uint32_t ch = h.got[seen].channel;
            if (ch != h.pawn_ch && ch != h.pc_ch && ch != h.s->channel_of(bs::Role::GameState) && ch != h.s->channel_of(bs::Role::PlayerState) &&
                (h.got[seen].close || h.got[seen].content.bit_pos() != 8)) {
                m.other.push_back({ch, h.got[seen].open, h.got[seen].close, now(), h.got[seen].content.bit_pos(), h.got[seen].exports.size()});
            }
        }
    }
    void send_reliable(const BunchWriter& b) {
        h.client_sends({h.client.actor(h.pawn_ch, b, /*reliable=*/true)});
        h.flush();
        absorb();
    }
    /// A ServerTriggerAbility of the client's own (the id is the client's, wherever it counts from): what the runtime
    /// accepted as an instance is recorded for the mirror.
    void send_trigger(int64_t guid, int32_t id, const BunchWriter* bunch = nullptr) {
        send_reliable(bunch ? *bunch : trigger_bunch(h.ability_guid(), guid, id));
        if (h.s->abilities()) {
            if (const auto instance = h.s->abilities()->instance_of(id)) m.server_of[id] = *instance;
        }
    }
    int32_t trigger(int64_t guid) {
        const int32_t id = next_id++;
        send_trigger(guid, id);
        return id;
    }
    void cancel(int32_t id) { send_reliable(cancel_bunch(h.ability_guid(), id)); }
    /// 100 ms of play: the client's move, then the server's tick.
    void tick(bool moving, milliseconds d = milliseconds(100)) {
        ts += static_cast<float>(d.count()) / 1000.0f;
        if (moving) x += 10.0;
        h.client_sends({h.client.actor(h.pawn_ch, client_move(move_at(ts, x, moving)), /*reliable=*/false)});
        h.step(d);
        absorb();
    }
    void play(double seconds, bool moving) {
        const int n = static_cast<int>(std::llround(seconds * 10));
        for (int i = 0; i < n; ++i) tick(moving);
    }
    /// A move at `cm_per_s` along the ground, then the server's tick: `d` of play.
    void tick_at(double cm_per_s, milliseconds d = milliseconds(100)) {
        ts += static_cast<float>(d.count()) / 1000.0f;
        x += cm_per_s * static_cast<double>(d.count()) / 1000.0;
        h.client_sends({h.client.actor(h.pawn_ch, client_move(move_at(ts, x, true)), /*reliable=*/false)});
        h.step(d);
        absorb();
    }
    /// What the runtime has, for comparing with the client's view.
    const AbilityRuntime& rt() const { return *h.s->abilities(); }
};

static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// ============================================================================================================================

static void test_sprint() {
    std::printf("Sprint: a full bar\n");
    Run r;
    CHECK(r.m.stamina_now() == 100.0f && r.m.abilities.empty() && r.m.effects.empty(), "joined: Stamina 100, no active ability, no effect");
    r.play(0.5, true);   // walking
    const double t_trigger = r.now();
    const int32_t id = r.trigger(kSprint);
    CHECK(id == 0, "(the first trigger of the session is ClientInstanceId 0)");
    CHECK(r.m.has_ability(kSprint) && r.m.has_effect(kSprintEffect) && r.m.chains_with_exports == 1,
          "the Sprint appears at once as an active ability and an effect, with the export of its montage (the first item that names it)");
    bool item_ok = !r.m.abilities.empty();
    for (const auto& [eid, a] : r.m.abilities) item_ok = item_ok && a.instance == 0 && a.has_montage && a.montage_group == "Sprint";
    CHECK(item_ok, "its item is instance 0 (the server's counter), with the Sprint montage in MontageGroup Sprint");
    CHECK(r.m.stats[kMoveSpeedMult] == 1.2f, "MoveSpeedMult is 1.2 on the client in the same update: 600 x 1.2 = 720 cm/s (the retail server sends it with the Sprint)");
    CHECK(near(r.m.stamina_now(), 98.75, 0.001), "the first charge comes with the activation: 98.75 on the client's HUD at once");
    r.play(0.3, true);
    CHECK(near(r.m.stamina_now(), 97.5, 0.001), "after 0.3 s the second: 97.5");
    r.play(9.7, true);
    CHECK(near(r.m.stamina_now(), 50.0, 1.3), "after 10 s of Sprint the bar is about half (50 within one charge)");
    CHECK(r.m.ends.empty() && r.rt().sprinting(), "(still sprinting)");
    r.play(10.5, true);
    CHECK(r.m.stamina_now() == 0.0f && !r.rt().sprinting(), "after 20.5 s the client's Stamina is 0 and the Sprint is over");
    CHECK(!r.m.has_ability(kSprint) && !r.m.has_effect(kSprintEffect) && r.m.stats[kMoveSpeedMult] == 1.0f && r.m.has_effect(kDisableRegenEffect),
          "the Sprint's ability item and effect are removed, MoveSpeedMult is 1.0 again and the DisableRegen effect is on (one update, as retail's reply)");
    CHECK(r.m.ends.size() == 1 && r.m.ends[0].second == 0, "the server ended instance 0 on the client: one ClientEndAbility(0)");
    CHECK(!r.m.end_before_removal, "it came after the update that removed the items (reliable, in order)");
    const double ended_at = r.m.ends.empty() ? 0 : r.m.ends[0].first;
    CHECK(ended_at - t_trigger >= 19.9 && ended_at - t_trigger <= 20.4, "19.75 s plus the 163 ms (within a tick or two) after the trigger");
    bool steps_ok = r.m.stamina.size() == 80;
    float prev = 100.0f;
    for (const auto& [at, v] : r.m.stamina) {
        steps_ok = steps_ok && near(prev - v, 1.25, 1e-6);
        prev = v;
    }
    CHECK(steps_ok, "the HUD saw 80 steps of 1.25, one per charge (98.75 ... 0)");
    CHECK(r.h.s->ability_ends_sent() == 1 && !r.h.s->abilities_failed() && r.m.unreadable == 0, "(one ClientEndAbility sent, nothing failed, every chain reads)");

    // regeneration: a pause of 1 s after the Sprint, then 5 a second, a full bar 20 s after the end
    const size_t before = r.m.stamina.size();
    r.play(0.3, false);   // 0.8 s after the end
    CHECK(r.m.stamina_now() == 0.0f && r.m.stamina.size() == before, "regeneration waits out a pause of 1 s after the Sprint");
    r.play(19.1, false);   // 19.9 s after the end: at most 19 ticks
    CHECK(r.m.stamina_now() >= 85.0f && r.m.stamina_now() < 100.0f, "then it gives 5 a second: not full 19.9 s after the end");
    CHECK(!r.m.has_effect(kDisableRegenEffect), "(the DisableRegen effect was removed when the pause was over)");
    r.play(1.3, false);   // 21.2 s
    CHECK(r.m.stamina_now() == 100.0f, "the bar is full again 20 s after the Sprint ended (a pause of 1 s, then 19 ticks and a bit)");
    bool regen_ok = true;
    for (size_t i = before + 1; i < r.m.stamina.size(); ++i) regen_ok = regen_ok && near(r.m.stamina[i].second - r.m.stamina[i - 1].second, 5.0, 1e-6);
    CHECK(regen_ok && r.m.stamina.size() - before == 20, "in 20 steps of 5");
    const size_t full = r.m.stamina.size();
    r.play(5.0, false);
    CHECK(r.m.stamina.size() == full && r.rt().stamina() == 100.0, "a full bar sends nothing more");
    CHECK(near(r.m.stamina_now(), r.rt().stamina(), 0), "(the client's view is the runtime's)");
}

static void test_sprint_cancel() {
    std::printf("Sprint: a cancel\n");
    Run r;
    r.play(0.3, true);
    const int32_t id = r.trigger(kSprint);
    r.play(2.0, true);
    CHECK(near(r.m.stamina_now(), 88.75, 1.3), "2 s of Sprint: about 88.75");
    r.cancel(id);
    CHECK(!r.m.has_ability(kSprint) && !r.m.has_effect(kSprintEffect) && !r.rt().sprinting() && r.m.stats[kMoveSpeedMult] == 1.0f,
          "ServerNotifyHeldAbilityCancelled(0): the items are removed at once, MoveSpeedMult is 1.0 again and the Sprint is over");
    const float held = r.m.stamina_now();
    r.play(0.8, true);
    CHECK(r.m.stamina_now() == held && r.m.ends.empty(), "the player keeps moving: the drain has stopped, and no ClientEndAbility was sent (the client ended its own)");
    r.play(1.5, true);
    CHECK(r.m.stamina_now() > held, "after the pause the bar refills");
    // a stale cancel is ignored
    r.cancel(id);
    r.cancel(99);
    r.play(0.2, true);
    CHECK(r.m.ends.empty() && r.m.abilities.empty() && r.m.effects.empty(), "a cancel of a finished or an unknown instance changes nothing and is not answered");
}

static void test_sprint_standing() {
    std::printf("Sprint: not moving\n");
    Run r;
    r.play(0.3, false);
    r.trigger(kSprint);
    r.play(3.0, false);
    CHECK(r.rt().sprinting() && r.m.stamina_now() == 100.0f && r.m.stamina.empty(), "a Sprint held while the player stands still costs nothing");
    r.play(2.0, true);
    CHECK(r.m.stamina_now() < 100.0f && r.m.stamina_now() > 85.0f, "the moves that carry an input charge it");
    r.play(0.5, false);   // the moves stop carrying an input and the displacement is zero
    const float at_stop = r.m.stamina_now();
    r.play(2.0, false);
    CHECK(r.m.stamina_now() >= at_stop, "standing again, the drain stops (regeneration is not blocked while standing)");
}
// (C:Y:B : C | O | A)

static void test_dodge() {
    std::printf("Dodge\n");
    Run r;
    r.play(0.5, false);
    const int32_t first = r.trigger(kDodge);
    CHECK(r.m.stamina_now() == 50.0f, "a Dodge costs 50, and the HUD sees it at once");
    CHECK(r.m.has_ability(kDodge) && r.m.has_effect(kEvasionEffect) && r.m.cooldowns.size() == 1 && r.m.cooldowns.begin()->second == kDodge,
          "one update: the Dodge's ability item, its Evasion effect and its cooldown");
    r.play(0.3, false);
    const int32_t second = r.trigger(kDodge);
    CHECK(r.m.end_ids() == std::vector<int32_t>{second} && r.m.stamina_now() == 50.0f,
          "a second Dodge 0.3 s later is in the cooldown: ClientEndAbility(its id), Stamina unchanged");
    CHECK(r.rt().refused() == 1 && first == 0 && second == 1, "(counted as refused)");
    r.play(0.3, false);   // 0.6 s after the first Dodge: the Evasion ended at 0.5
    CHECK(!r.m.has_effect(kEvasionEffect), "the Evasion effect ends after 0.5 s");
    CHECK(r.m.cooldowns.size() == 1, "(the cooldown is still on)");
    r.play(0.2, false);   // 0.8 s: the cooldown (0.65 s) is over
    CHECK(r.m.cooldowns.empty() && !r.m.has_ability(kDodge), "the cooldown and the ability item go after 0.65 s");
    const int32_t third = r.trigger(kDodge);
    CHECK(r.m.stamina_now() == 0.0f && third == 2, "after the cooldown a second Dodge is allowed: 50 left, so 0");
    r.play(0.9, false);
    const size_t ends = r.m.ends.size();
    const int32_t fourth = r.trigger(kDodge);
    CHECK(r.m.ends.size() == ends + 1 && r.m.ends.back().second == fourth && r.m.stamina_now() == 0.0f,
          "a Dodge with less than 50 Stamina is refused with ClientEndAbility");
    CHECK(r.m.stamina.size() == 2 && r.m.stamina[0].second == 50.0f && r.m.stamina[1].second == 0.0f, "(the HUD saw 50 and 0, nothing else yet)");
    r.play(0.5, false);   // 1.4 s after the second Dodge: the Evasion (0.5 s) and the pause (1 s) end at 1.5
    CHECK(r.m.stamina_now() == 0.0f, "regeneration stays off for 1.5 s after a Dodge");
    r.play(1.7, false);
    CHECK(r.m.stamina_now() >= 5.0f, "and then it is back");
    CHECK(!r.m.end_before_removal && !r.h.s->abilities_failed() && r.m.unreadable == 0, "(nothing failed)");
}

static void test_rest() {
    std::printf("Rest\n");
    Run r;
    r.play(0.5, false);
    const size_t chains = r.m.chains;
    const int32_t id = r.trigger(kRest);
    CHECK(r.m.has_ability(kRest) && r.m.chains == chains + 1 && r.m.chains_with_exports == 1,
          "Rest: one update, with the export of its montage (the client has to learn the path)");
    bool item_ok = false;
    for (const auto& [eid, a] : r.m.abilities) item_ok = a.ability == kRest && a.instance == id && a.has_montage && a.has_target && a.animation_counter == 0;
    CHECK(item_ok, "its item: the Rest montage, the caster as target, AnimationCounter 0");
    CHECK(!r.m.has_effect(kRestEffect) && r.m.stats[kHealthRegen] == 0.6f, "the effect is not there during the sit-down");
    r.play(2.2, false);
    CHECK(r.m.has_effect(kRestEffect) && r.m.stats[kHealthRegen] == 2.5f && r.m.stats[kManaRegen] == 2.5f,
          "after the sit-down the Rest effect appears and Health and Mana regeneration are 2.5");
    r.cancel(id);
    bool jumped = false;
    for (const auto& [eid, a] : r.m.abilities) jumped = a.may_jump && a.montage_position == 5.82606792f && a.animation_counter == 1;
    CHECK(jumped && r.m.has_effect(kRestEffect), "a cancel: the montage jumps to its end section (the effect is still on)");
    r.play(0.3, false);
    CHECK(!r.m.has_effect(kRestEffect) && r.m.stats[kHealthRegen] == 0.6f && r.m.stats[kManaRegen] == 0.6f, "a moment later the effect goes and the regeneration is back to 0.6");
    CHECK(r.m.ends.empty() && r.m.stamina_now() == 100.0f, "(no ClientEndAbility, Stamina untouched)");
    // the montage's export is not repeated once acknowledged
    r.trigger(kRest);
    CHECK(r.m.chains_with_exports == 1, "a second Rest does not export the montage again: the first export was acknowledged");
}

/// Flips bit `bit` of a bunch.
static BunchWriter flipped(const BunchWriter& in, size_t bit) {
    std::vector<uint8_t> bytes(in.data(), in.data() + in.byte_size());
    bytes[bit >> 3] ^= static_cast<uint8_t>(1u << (bit & 7));
    BunchWriter out;
    out.write_bit_range(bytes.data(), 0, in.bit_pos());
    return out;
}

static void test_sprint_and_dodge() {
    std::printf("a Dodge in the middle of a Sprint, a Rest after one\n");
    {
        Run r;
        r.play(0.5, true);
        const int32_t sprint = r.trigger(kSprint);
        r.play(1.0, true);
        const float at_dodge = r.m.stamina_now();
        r.trigger(kDodge);
        CHECK(r.m.has_ability(kSprint) && r.m.has_effect(kSprintEffect) && r.m.has_ability(kDodge) && r.m.has_effect(kEvasionEffect) && r.m.end_ids().empty(),
              "a Dodge does not end a Sprint (retail capture 745): both are on, and no ClientEndAbility went out");
        CHECK(r.m.stats[kMoveSpeedMult] == 1.2f && near(r.m.stamina_now(), at_dodge - 50.0, 1.3), "MoveSpeedMult stays 1.2, and the Dodge cost 50 on top of the Sprint's steps");
        const float after_dodge = r.m.stamina_now();
        r.play(2.0, true);
        CHECK(r.rt().sprinting() && r.m.has_effect(kSprintEffect) && r.m.stats[kMoveSpeedMult] == 1.2f && r.m.stamina_now() < after_dodge - 5.0f,
              "the Sprint goes on through the Dodge: its effect, its speed and its drain (1.25 every 0.25 s)");
        r.cancel(sprint);
        CHECK(!r.rt().sprinting() && !r.m.has_effect(kSprintEffect) && r.m.stats[kMoveSpeedMult] == 1.0f && r.m.end_ids().empty(),
              "the release the client sends afterwards (its own id) ends it as usual: MoveSpeedMult back to 1.0, still no ClientEndAbility");
        CHECK(!r.m.end_before_removal && !r.h.s->abilities_failed() && r.m.unreadable == 0, "(nothing failed)");
    }
    {
        Run r;
        r.play(0.5, true);
        const int32_t sprint = r.trigger(kSprint);
        r.play(0.5, true);
        r.trigger(kRest);
        CHECK(!r.rt().sprinting() && r.rt().resting() && r.m.end_ids() == std::vector<int32_t>{sprint} && !r.m.has_effect(kSprintEffect) &&
                  r.m.stats[kMoveSpeedMult] == 1.0f && r.m.has_ability(kRest),
              "a Rest ends the Sprint and tells the client: ClientEndAbility(the Sprint's id), MoveSpeedMult back to 1.0 (no retail capture has a Rest)");
        CHECK(!r.m.end_before_removal, "(after the update that removed the Sprint's items)");
    }
}
// C | O | A / C.Y.B

static void test_instance_ids() {
    std::printf("ServerInstanceId is the server's counter, ClientEndAbility the client's id\n");
    Run r;
    r.play(0.5, true);
    const int32_t refused = r.trigger(kScepterCombo);
    const int32_t sprint = r.trigger(kSprint);
    CHECK(refused == 0 && sprint == 1, "(the client counted a refused trigger: the Sprint is ClientInstanceId 1)");
    bool item_ok = false;
    for (const auto& [eid, a] : r.m.abilities) item_ok = a.ability == kSprint && a.instance == 0;
    CHECK(item_ok, "its item says ServerInstanceId 0: the server's own count, the refused trigger took none");
    r.cancel(0);
    CHECK(r.rt().sprinting() && r.m.has_effect(kSprintEffect), "a cancel of 0 is the client's id 0 (the refused trigger), not the instance number: the Sprint runs on");
    r.play(21.0, true);
    CHECK(!r.rt().sprinting() && r.m.end_ids() == ints({refused, sprint}) && !r.m.end_before_removal,
          "the Sprint that runs out is ended with ClientEndAbility(1), the client's id (not the instance's 0), after the refusal's ClientEndAbility(0)");
}

static void test_dodge_dash() {
    std::printf("the Dodge's dash against the move checker\n");
    Run control;
    control.play(0.5, true);
    for (int i = 0; i < 8; ++i) control.tick_at(2800);   // 2,800 cm/s: over max_speed (2,135), under the dash's 3,355
    CHECK(control.h.s->moves_refused() > 0, "(without a Dodge, 8 moves at 2,800 cm/s are refused: over the limit of 2,135)");
    Run r;
    r.play(0.5, true);
    r.trigger(kDodge);
    for (int i = 0; i < 6; ++i) r.tick_at(2800);   // 0.6 s: inside the Evasion plus 0.3 s
    CHECK(r.h.s->moves_refused() == 0 && r.h.s->corrections_sent() == 0, "after a Dodge the same speed is accepted for the length of the dash: no move refused, no correction");
    r.play(1.0, true);   // the window (0.5 s + 0.3 s) is over
    for (int i = 0; i < 8; ++i) r.tick_at(2800);
    CHECK(r.h.s->moves_refused() > 0, "once the window is over the limit is 2,135 again: the same speed is refused");
    Run too_fast;
    too_fast.play(0.5, true);
    too_fast.trigger(kDodge);
    for (int i = 0; i < 6; ++i) too_fast.tick_at(4500);   // over even the dash's limit
    CHECK(too_fast.h.s->moves_refused() > 0, "and not even a Dodge allows 4,500 cm/s");
}

/// A trigger that arrives while the join is still queued: the runtime answers, nothing goes out before the join's chains (the pawn's
/// second state is theirs).
static void test_ability_waits_for_the_join() {
    std::printf("an ability before the join is on the wire\n");
    Run r(/*clog the PlayerController's window*/ 248);
    CHECK(r.h.s->join_pending() > 0, "(the join is queued behind the PlayerController's full window)");
    r.trigger(kSprint);
    for (int i = 0; i < 5; ++i) r.tick(true);
    CHECK(r.h.s->join_pending() > 0 && r.h.s->ability_updates_sent() == 0 && r.h.s->ability_ends_sent() == 0 && !r.h.s->abilities_failed(),
          "the Sprint is started, but no ability update goes out while the join waits");
    r.trigger(kScepterCombo);
    CHECK(r.h.s->ability_updates_sent() == 0 && r.h.s->ability_ends_sent() == 0 && r.h.s->ability_ends_pending() == 1,
          "and a refusal's ClientEndAbility waits too");
    // The client gets the join's first chains, and its next datagram carries their acknowledgements AND another trigger: the window has
    // room by the time the trigger is handled, but the session has not ticked, so the rest of the join is still queued.  The ability
    // update must not overtake it.
    r.h.release();
    r.trigger(kDodge);
    CHECK(r.h.s->join_pending() > 0 && r.h.s->ability_updates_sent() == 0 && r.h.s->ability_ends_sent() == 0,
          "an acknowledgement and a trigger in one datagram: the window has room, but the join's chains still go first (nothing of the abilities is sent)");
    r.h.settle();
    r.play(0.5, true);
    CHECK(r.h.s->join_pending() == 0 && r.h.s->ability_updates_sent() >= 1 && r.h.s->ability_ends_sent() == 1 && r.m.has_ability(kSprint) && r.m.has_effect(kSprintEffect) &&
              r.m.end_ids() == std::vector<int32_t>{1},
          "once the join is out the client gets the Sprint and then the refusal, both");
}

/// The pawn channel's window is full when a Sprint starts: the update waits for an acknowledgement, goes out once, with the state
/// as it is then.
static void test_ability_update_waits_for_the_window() {
    std::printf("an ability update waits for a full window\n");
    Run r;
    r.play(0.5, true);
    const size_t room = Connection::kMaxUnackedReliable - r.h.conn.unacked_reliable(r.h.pawn_ch);
    r.h.hold = true;   // the client acknowledges nothing for a while
    r.h.filler(r.h.pawn_ch, room);
    CHECK(r.h.conn.unacked_reliable(r.h.pawn_ch) == Connection::kMaxUnackedReliable, "(the pawn channel's window is full)");
    r.trigger(kSprint);
    r.play(1.5, true);
    CHECK(r.h.s->ability_updates_sent() == 0 && !r.m.has_ability(kSprint) && !r.h.s->abilities_failed() && r.rt().sprinting(),
          "the Sprint runs on the server, and its update waits for the window (nothing is dropped, nothing fails)");
    r.h.release();
    r.play(1.0, true);
    CHECK(r.h.s->ability_updates_sent() >= 1 && r.m.has_ability(kSprint) && r.m.has_effect(kSprintEffect) && r.m.stats[kMoveSpeedMult] == 1.2f,
          "once the client acknowledges, the update goes out: the Sprint, its effect and MoveSpeedMult");
    bool decreasing = !r.m.stamina.empty();
    for (size_t i = 1; i < r.m.stamina.size(); ++i) decreasing = decreasing && r.m.stamina[i].second < r.m.stamina[i - 1].second;
    CHECK(decreasing && near(r.m.stamina_now(), r.rt().stamina(), 1.3) && r.m.unreadable == 0,
          "with the Stamina as it was by then, the later steps one by one in order, and the client within a charge of the runtime");
}
// (C | Y | B) (C-O-A)

static size_t count_lines(const std::string& text, const char* a, const char* b) {
    size_t n = 0, pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(pos, end - pos);
        if (line.find(a) != std::string::npos && line.find(b) != std::string::npos) ++n;
        pos = end + 1;
    }
    return n;
}

static void test_ability_log() {
    std::printf("the ability log\n");
    {
        // a client that Sprints in a loop: the first kLoggedAbilityTransitions transitions are at info, the rest at debug
        Run r;
        r.play(0.3, true);
        g_log.str("");
        for (int i = 0; i < 60; ++i) {   // 60 x (a Sprint that starts, a release) = 120 transitions, 1.25 Stamina each
            const int32_t id = r.trigger(kSprint);
            r.play(0.1, true);
            r.cancel(id);
            r.play(0.1, true);
        }
        spdlog::default_logger()->flush();
        const std::string text = g_log.str();
        const size_t info = count_lines(text, "[info]", "(Stamina "), debug = count_lines(text, "[debug]", "the Sprint");
        CHECK(info == PlayerSession::kLoggedAbilityTransitions && debug >= 20,
              "120 transitions: the first 100 are at info (one line each), the rest at debug");
        CHECK(r.h.s->ability_triggers() == 60 && r.h.s->ability_triggers_dropped() == 0 && !r.h.s->abilities_failed(), "(all 60 triggers were handled)");
    }
    {
        // the pawn's state and the runtime drift apart (an effect gone behind the runtime's back): abilities are off, and the log says what
        // the client is left with
        Run r;
        r.play(0.5, true);
        const int32_t id = r.trigger(kSprint);
        r.play(0.3, true);
        bs::AbilityChange gone;
        gone.effects_removed.push_back(0);   // the Sprint's effect (EffectId 0)
        CHECK(const_cast<bs::PlayerBootstrap&>(r.h.s->bootstrap()).apply_ability_change(gone), "(the test removes the Sprint's effect from the pawn behind the runtime's back)");
        g_log.str("");
        r.cancel(id);
        spdlog::default_logger()->flush();
        const std::string text = g_log.str();
        CHECK(r.h.s->abilities_failed() && text.find("abilities are off for this session") != std::string::npos && text.find("the pawn lists 1 ability item(s)") != std::string::npos &&
                  text.find("[warning]") != std::string::npos,
              "the runtime ends the Sprint, the pawn does not take it: abilities are off, and a warning says what the pawn still lists");
        const size_t updates = r.h.s->ability_updates_sent();
        r.trigger(kDodge);
        r.play(0.5, true);
        CHECK(r.h.s->ability_updates_sent() == updates && r.m.stamina_now() > 90.0f && !r.m.has_ability(kDodge) && r.h.s->ability_triggers() == 1,
              "afterwards abilities are not run: a Dodge costs nothing, adds nothing and is not even counted");
    }
}

static void test_refusals() {
    std::printf("what is refused\n");
    {
        Run r;
        r.play(0.5, false);
        const int32_t a = r.trigger(kScepterCombo);
        const int32_t b = r.trigger(kQuickWit);
        CHECK(r.m.end_ids() == ints({a, b}) && r.m.stamina_now() == 100.0f && r.m.abilities.empty() && r.m.effects.empty(),
              "abilities worldd does not run (a combo, QuickWit) are answered with ClientEndAbility(their ids) and nothing else");
        CHECK(r.h.s->ability_triggers() == 2 && r.h.s->ability_updates_sent() == 0 && r.h.s->ability_ends_sent() == 2, "(two triggers handled, no update, two ends)");
    }
    {
        // a trigger that does not read whole: answered from its first two parameters when it is none of the three
        Run r;
        r.play(0.5, false);
        // (an ability that is no row of any table: with the real content.db QuickWit is the generic runtime's, which does not start an ability from half a call and
        // answers nothing, so the answer to a half call of an ability worldd does not run is read off one that no table has)
        const BunchWriter good = trigger_bunch(r.h.ability_guid(), kNotRun, 4);
        const auto resolver = [&](const emit::FIntrepidNetworkGUID& g) -> const schema::ClassSchema* {
            return g == r.h.ability_guid() ? &class_named("UAoCAbilityComponent") : nullptr;
        };
        const auto parsed = repl::read_client_bunch(class_named("PlayerPawn_C"), resolver, good.data(), good.bit_pos(), client_options());
        const bool found = parsed && parsed->rpcs.size() == 1 && parsed->rpcs[0].call.decoded;
        CHECK(found, "(the trigger of an ability worldd does not run reads whole)");
        if (found) {
            // the FItemInstance's bit is the sixth from the end of the parameters: ItemData ends with Slot, the item and 5 bits of Location
            const size_t bit = parsed->rpcs[0].call.param_pos + parsed->rpcs[0].call.param_bits - 6;
            const BunchWriter bad = flipped(good, bit);
            const auto reread = repl::read_client_bunch(class_named("PlayerPawn_C"), resolver, bad.data(), bad.bit_pos(), client_options());
            CHECK(reread && reread->rpcs.size() == 1 && !reread->rpcs[0].call.decoded, "(with the empty item's bit set it does not decode)");
            g_log.str("");
            r.send_reliable(bad);
            CHECK(r.m.end_ids() == std::vector<int32_t>{4}, "an ability worldd does not run, with a parameter that does not read: ClientEndAbility(4), from its first two parameters");
            // a Sprint that does not read is not started, and not answered
            const BunchWriter sprint = trigger_bunch(r.h.ability_guid(), kSprint, 5);
            const auto sp = repl::read_client_bunch(class_named("PlayerPawn_C"), resolver, sprint.data(), sprint.bit_pos(), client_options());
            const size_t sbit = sp && sp->rpcs.size() == 1 ? sp->rpcs[0].call.param_pos + sp->rpcs[0].call.param_bits - 6 : 0;
            r.send_reliable(flipped(sprint, sbit));
            CHECK(!r.rt().sprinting() && !r.m.has_ability(kSprint) && r.m.end_ids() == std::vector<int32_t>{4},
                  "a Sprint that does not read is not started: half a call is not acted on, and there is nothing to answer");
            spdlog::default_logger()->flush();
            CHECK(g_log.str().find("does not read") != std::string::npos, "and it is logged");
        }
    }
    {
        // a session that has finished (the client quit, a second login took over) answers nothing more
        Run r;
        r.play(0.5, false);
        r.trigger(kSprint);
        r.h.s->finish("the test");
        const size_t steps = r.m.stamina.size();
        r.trigger(kScepterCombo);
        r.cancel(0);
        r.play(1.0, true);
        CHECK(r.m.stamina.size() == steps && r.m.ends.empty() && r.m.has_ability(kSprint) && r.h.s->ability_triggers() == 1,
              "after finish() a trigger and a cancel are not acted on (the Sprint is neither ended nor answered) and its drain stops");
    }
    {
        // the budget: a burst of 8, then 10 a second
        Run r;
        r.play(0.5, false);
        for (int i = 0; i < 20; ++i) r.trigger(kScepterCombo);
        CHECK(r.h.s->ability_triggers() == 8 && r.h.s->ability_triggers_dropped() == 12 && r.m.ends.size() == 8,
              "20 triggers in an instant: 8 handled (and answered), 12 dropped for the budget and counted");
        r.play(1.0, false);
        r.trigger(kScepterCombo);
        CHECK(r.h.s->ability_triggers() == 9 && r.m.ends.size() == 9, "a second later the budget allows more");
    }
}
// C O A + C | Y | B

static void test_laggy_client() {
    std::printf("a client whose acknowledgements are late\n");
    Run r;
    r.play(0.5, false);
    r.h.hold = true;   // the client is given nothing, so it acknowledges nothing, for a few seconds
    const int32_t id = r.trigger(kRest);
    r.play(1.0, false);
    r.cancel(id);
    r.play(0.5, false);
    CHECK(r.m.abilities.empty() && r.h.s->ability_updates_sent() == 2, "(two updates went out while the client heard nothing: the Rest and its cancel)");
    r.h.release();
    r.play(1.0, false);
    CHECK(r.m.chains_with_exports == 2, "the montage's export rides in both updates: the first was not acknowledged when the second was built");
    bool jumped = false;
    for (const auto& [eid, a] : r.m.abilities) jumped = a.ability == kRest && a.may_jump && a.animation_counter == 1;
    CHECK(jumped && r.m.unreadable == 0 && !r.h.s->abilities_failed(), "the client reads both, in order: the Rest, then its jump to the end section");
    r.trigger(kRest);
    CHECK(r.m.chains_with_exports == 2, "once the export is acknowledged it is not sent again");

    // a laggy client in the middle of a Sprint: the updates arrive late and in order
    Run s;
    s.play(0.3, true);
    s.trigger(kSprint);
    s.h.hold = true;
    s.play(3.0, true);
    CHECK(s.m.stamina_now() == 98.75f, "(nothing reached the client for 3 s but the activation's charge)");
    s.h.release();
    s.play(0.3, true);
    CHECK(near(s.m.stamina_now(), static_cast<float>(s.rt().stamina()), 1.3) && s.m.has_ability(kSprint) && s.m.stamina.size() >= 12 && s.m.unreadable == 0,
          "released: the client sees the Sprint and every step the server took, and is within a charge of the runtime");
}

static void test_server_stall() {
    std::printf("a server loop that stalls\n");
    Run r;
    r.play(0.5, false);
    r.trigger(kDodge);
    CHECK(r.m.stamina_now() == 50.0f, "(a Dodge: 50)");
    const size_t chains = r.m.chains;
    r.h.step(seconds(15));   // one tick, 15 s later
    r.absorb();
    CHECK(r.rt().stamina() == 100.0 && r.m.stamina_now() == 100.0f && r.m.abilities.empty() && r.m.effects.empty() && r.m.cooldowns.empty(),
          "one tick 15 s later runs the timers in order (Evasion, cooldown, pause, ten regeneration ticks): the bar is full and every item is gone");
    CHECK(r.m.chains <= chains + 2, "and the client gets what changed as one update, not the ten steps in between: the state is the queue");
    CHECK(!r.m.end_before_removal && r.m.unreadable == 0 && !r.h.s->abilities_failed(), "(nothing failed)");
}

// ============================================================================================================================
//  through the endpoint: its own clock ticks the runtime
// ============================================================================================================================

static const Address kA{0x7F000001u, 50510};
static uint8_t g_counter = 1;
static void counting_random(uint8_t* out, size_t n) {
    for (size_t i = 0; i < n; ++i) out[i] = g_counter++;
}

struct FakeLogins : LoginQueue {
    std::vector<std::pair<uint64_t, LoginRequest>> submitted;
    void submit(uint64_t id, LoginRequest r) override { submitted.emplace_back(id, std::move(r)); }
};

struct Rig {
    CookieSigner signer{counting_random};
    FakeLogins logins;
    std::vector<std::pair<Address, std::vector<uint8_t>>> sent;
    Endpoint ep;

    explicit Rig(EndpointConfig cfg)
        : ep(std::move(cfg), signer, logins, [this](const Address& a, const std::vector<uint8_t>& p) { sent.emplace_back(a, p); }, counting_random, t0) {}

    std::vector<std::vector<uint8_t>> take(const Address& a) {
        std::vector<std::vector<uint8_t>> out;
        std::vector<std::pair<Address, std::vector<uint8_t>>> rest;
        for (auto& [to, p] : sent) {
            if (to == a) out.push_back(p);
            else rest.emplace_back(to, p);
        }
        sent.swap(rest);
        return out;
    }
};

struct Joined {
    std::optional<conn::HandshakePacket> challenge;
    std::optional<fake::Client> client;
    uint64_t session = 0;
    std::vector<fake::Delivered> got;
};

static Joined handshake(Rig& rig, const Address& addr, Clock::time_point now) {
    Joined j;
    rig.take(addr);
    const auto initial = capture::bytes(capture::kInitial);
    rig.ep.on_datagram(addr, initial.data(), initial.size(), now);
    auto out = rig.take(addr);
    if (out.size() != 1) return j;
    j.challenge = conn::parse_handshake(out[0].data(), out[0].size());
    conn::HandshakePacket resp = *j.challenge;
    resp.type = conn::HandshakeType::Response;
    resp.sent_count = 2;
    resp.padding.assign(11, 0x5A);
    const auto bytes = conn::build_handshake(resp);
    rig.ep.on_datagram(addr, bytes.data(), bytes.size(), now);
    out = rig.take(addr);
    if (out.size() != 1) return j;
    const auto seqs = conn::initial_sequences(j.challenge->cookie);
    j.client.emplace(conn::outer_header_of(*j.challenge), j.challenge->ext->custom_field(), seqs.server, seqs.client);
    j.session = rig.logins.submitted.empty() ? 0 : rig.logins.submitted.back().first;
    return j;
}
// Cyb captures, C | O | A code

static void drain(Rig& rig, Joined& j, const Address& addr) {
    for (const auto& p : rig.take(addr)) {
        const auto r = j.client->receive(p);
        for (const auto& d : r.delivered) j.got.push_back(d);
    }
}

static void exchange(Rig& rig, Joined& j, const Address& addr, std::vector<fake::Bunch> bunches, Clock::time_point now) {
    const auto pkt = j.client->packet(std::move(bunches));
    rig.ep.on_datagram(addr, pkt.data(), pkt.size(), now);
    rig.ep.tick(now);
    drain(rig, j, addr);
}

/// Hello, login, Join: the player is in the world.
static void join_world(Rig& rig, Joined& j, const Address& addr, const svc::Character& rec, Clock::time_point now) {
    exchange(rig, j, addr, {j.client->hello()}, now);
    LoginResult r;
    r.accepted = true;
    r.character = rec;
    rig.ep.post_login_result(j.session, r);
    rig.ep.tick(now + milliseconds(1));
    drain(rig, j, addr);
    exchange(rig, j, addr, {j.client->login(kMachine, kMachine)}, now + milliseconds(2));
    exchange(rig, j, addr, {j.client->netspeed(200000), j.client->nmt(conn::Nmt::Join)}, now + milliseconds(40));
}

static void test_through_the_endpoint() {
    std::printf("through the endpoint\n");
    Fixture fx;
    EndpointConfig cfg;
    cfg.players = fx.world.get();
    Rig rig(cfg);
    Joined j = handshake(rig, kA, t0);
    join_world(rig, j, kA, cyb_record(), t0);
    const PlayerSession* p = rig.ep.player_at(kA);
    CHECK(rig.ep.session_count() == 1 && p != nullptr, "(a player is in the world)");
    if (!p) return;
    Mirror m(p->bootstrap().setup());
    const uint32_t ch = p->channel_of(bs::Role::Pawn);
    const emit::FIntrepidNetworkGUID ability = p->bootstrap().setup().guids.ability;
    size_t seen = j.got.size();
    Clock::time_point t = t0 + milliseconds(100);
    const auto absorb = [&] {
        for (; seen < j.got.size(); ++seen)
            if (j.got[seen].channel == ch) m.absorb(j.got[seen], std::chrono::duration<double>(t - t0).count());
    };
    for (int i = 0; i < 30; ++i) {   // the join's chains settle
        exchange(rig, j, kA, {}, t);
        t += milliseconds(100);
    }
    seen = j.got.size();
    CHECK(m.stamina_now() == 100.0f && p->abilities() != nullptr, "Stamina 100; the runtime exists once the Endpoint has ticked the session");

    exchange(rig, j, kA, {j.client->actor(ch, trigger_bunch(ability, kSprint, 0), /*reliable=*/true)}, t);
    absorb();
    const double started = std::chrono::duration<double>(t - t0).count();
    CHECK(m.has_ability(kSprint) && m.has_effect(kSprintEffect), "a Sprint trigger sent through the Endpoint starts the Sprint");
    float ts = 10.0f;
    double x = kX;
    for (int i = 0; i < 215; ++i) {   // 21.5 s, a move every 100 ms
        t += milliseconds(100);
        ts += 0.1f;
        x += 10.0;
        exchange(rig, j, kA, {j.client->actor(ch, client_move(move_at(ts, x, true)), /*reliable=*/false)}, t);
        absorb();
    }
    CHECK(m.stamina_now() == 0.0f && m.stamina.size() == 80 && !m.has_ability(kSprint) && !m.has_effect(kSprintEffect),
          "driven by the Endpoint's own ticks the bar empties in 80 steps and the Sprint's items are gone");
    CHECK(m.ends.size() == 1 && m.ends[0].second == 0 && m.ends[0].first - started >= 19.9 && m.ends[0].first - started <= 20.5 && !m.end_before_removal,
          "ClientEndAbility(0) arrives about 20 s after the trigger, after the removal");
    CHECK(rig.ep.session_count() == 1 && !p->abilities_failed() && m.unreadable == 0, "(the session is still there and nothing failed)");
}

// ============================================================================================================================
//  a seeded hostile client (the review's fuzz harness): random operations with hostile ids, stalls and held acknowledgements, and
//  bit-level mutations of triggers and cancels.  After every few steps the invariants must hold: Stamina within [0, max] on the
//  server and on the client, nothing failed, every chain reads, the item counts bounded, the pending ClientEndAbility calls
//  bounded, and, once settled, the client's view is the runtime's (Stamina, the Sprint, MoveSpeedMult).
// ============================================================================================================================

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    uint32_t u(uint32_t n) { return static_cast<uint32_t>(next() % n); }
};

static int g_violations = 0;
static void violate(const std::string& msg) {
    if (g_violations++ < 25) std::printf("  [FAIL] fuzz: %s\n", msg.c_str());
}

/// A ServerTriggerAbility with hostile Targets (plain GUIDs the pawn does not have, or the CollisionCylinder named in place) and
/// TargetTransforms, as far as the schema lets a client write them.
static repl::RpcParams hostile_params(int64_t guid, int32_t id, size_t n_targets, bool inline_targets, size_t n_transforms, const emit::FIntrepidNetworkGUID& pawn) {
    const schema::RpcSchema* rpc = class_named("UAoCAbilityComponent").rpc("ServerTriggerAbility");
    repl::RpcParams p;
    repl::set_rpc_param(*rpc, p, "AbilityGuid", repl::Value::i64(guid));
    if (id != 0) repl::set_rpc_param(*rpc, p, "ClientInstanceId", repl::Value::i64(id));
    repl::set_rpc_param(*rpc, p, "InputVector", repl::Value(repl::Vec2d{1, 0}));
    repl::set_rpc_param(*rpc, p, "ItemData.Slot", repl::Value::i64(-1));
    repl::set_rpc_param(*rpc, p, "ItemData.ItemInstance", repl::Value(repl::ItemInstance{}));
    repl::set_rpc_param(*rpc, p, "ItemData.Location", repl::Value::u64(24));
    if (n_targets) {
        repl::ArrayValue arr;
        for (size_t i = 0; i < n_targets; ++i) {
            repl::ElementState e;
            if (inline_targets) {
                repl::InlineObject o;
                o.flags = 5;
                o.outer = pawn;
                o.path = "CollisionCylinder";
                o.checksum = 0xb876932f;
                e.values[1] = repl::Value(o);
            } else {
                emit::FIntrepidNetworkGUID g;
                g.ObjectId = 1000 + i;
                g.ServerId = 60;
                e.values[1] = repl::Value(g);
            }
            arr.elements.push_back(std::move(e));
        }
        repl::set_rpc_param(*rpc, p, "Targets", repl::Value(std::move(arr)));
    }
    if (n_transforms) {
        const schema::RepCommand* cmd = repl::rpc_command(*rpc, "TargetTransforms");
        const auto elems = rpc->element_commands.subspan(cmd->elem_begin, cmd->elem_end - cmd->elem_begin);
        repl::ArrayValue arr;
        for (size_t i = 0; i < n_transforms; ++i) {
            repl::ElementState e;
            for (size_t k = 0; k < elems.size(); ++k) {
                auto z = repl::zero_value(elems[k]);
                if (z) e.values[static_cast<uint16_t>(k + 1)] = *z;
            }
            arr.elements.push_back(std::move(e));
        }
        repl::set_rpc_param(*rpc, p, "TargetTransforms", repl::Value(std::move(arr)));
    }
    return p;
}
// C Y B + C.O.A

/// One damaged copy of a bunch: bits flipped, cut short, a byte at an edge value, random bits appended, a byte run replaced, a run of ones.
static BunchWriter mutate(const BunchWriter& in, Rng& rng) {
    std::vector<uint8_t> bytes(in.data(), in.data() + in.byte_size());
    size_t bits = in.bit_pos();
    switch (rng.u(6)) {
        case 0: {
            const int n = 1 + static_cast<int>(rng.u(4));
            for (int i = 0; i < n; ++i) {
                const size_t b = rng.u(static_cast<uint32_t>(bits));
                bytes[b >> 3] ^= static_cast<uint8_t>(1u << (b & 7));
            }
            break;
        }
        case 1:
            bits = 1 + rng.u(static_cast<uint32_t>(bits));
            break;
        case 2: {
            static const uint8_t edge[] = {0x00, 0xff, 0x7f, 0x80};
            bytes[rng.u(static_cast<uint32_t>(bytes.size()))] = edge[rng.u(4)];
            break;
        }
        case 3: {
            const size_t extra = 1 + rng.u(96);
            for (size_t i = 0; i < (extra + 7) / 8; ++i) bytes.push_back(static_cast<uint8_t>(rng.u(256)));
            bits += extra;
            break;
        }
        case 4: {
            const size_t b = rng.u(static_cast<uint32_t>(bytes.size()));
            const size_t n = 1 + rng.u(4);
            for (size_t i = 0; i < n && b + i < bytes.size(); ++i) bytes[b + i] = static_cast<uint8_t>(rng.u(256));
            break;
        }
        default: {
            const size_t b = rng.u(static_cast<uint32_t>(bits));
            for (size_t i = 0; i < 16 && b + i < bits; ++i) bytes[(b + i) >> 3] |= static_cast<uint8_t>(1u << ((b + i) & 7));
            break;
        }
    }
    BunchWriter out;
    out.write_bit_range(bytes.data(), 0, bits);
    return out;
}

static void check_invariants(Run& r, const char* where, bool settled) {
    const AbilityRuntime* rt = r.h.s->abilities();
    if (!rt) return;
    const std::string w = where;
    if (!(rt->stamina() >= 0.0 && rt->stamina() <= rt->max_stamina())) violate(w + ": the runtime's Stamina is outside [0, max]");
    if (r.h.s->abilities_failed()) violate(w + ": abilities failed");
    if (r.m.unreadable) violate(w + ": the client could not read a chain");
    if (r.m.abilities.size() > 3 || r.m.effects.size() > 5 || r.m.cooldowns.size() > 1) violate(w + ": the client's item counts are out of bounds");
    for (const auto& s : r.m.stamina) {
        if (s.second < 0.0f || s.second > 100.0f) {
            violate(w + ": the client saw a Stamina outside [0, 100]");
            break;
        }
    }
    if (settled && !r.h.hold) {
        if (!(std::fabs(r.m.stamina_now() - static_cast<float>(rt->stamina())) < 1e-3)) violate(w + ": settled, the client's Stamina is not the runtime's");
        if (rt->sprinting() != r.m.has_ability(kSprint)) violate(w + ": settled, the runtime's Sprint and the client's differ");
        if (r.m.stats[kMoveSpeedMult] != (rt->sprinting() ? 1.2f : 1.0f)) violate(w + ": settled, MoveSpeedMult is not 1.2 exactly while the runtime sprints");
    }
    if (r.h.s->ability_ends_pending() > 64) violate(w + ": more than 64 ClientEndAbility calls pending");
}

static void scenario_random_ops(uint64_t seed, int iterations, bool with_holds) {
    Run r;
    Rng rng(seed);
    r.play(0.5, false);
    int32_t last_id = 0;
    int refused_unanswered = 0;
    for (int it = 0; it < iterations; ++it) {
        const uint32_t op = rng.u(20);
        const int64_t guids[] = {kSprint, kSprint, kDodge, kRest, kScepterCombo, kQuickWit, 12345, 0, -1, INT64_MIN, INT64_MAX};
        if (op < 7) {   // a trigger, with an id the client might send
            const int64_t guid = guids[rng.u(11)];
            int32_t id;
            switch (rng.u(8)) {
                case 0: id = last_id; break;
                case 1: id = static_cast<int32_t>(rng.next()); break;
                case 2: id = INT32_MIN; break;
                case 3: id = INT32_MAX; break;
                case 4: id = -1; break;
                case 5: id = 0; break;
                default: id = r.next_id++; break;
            }
            last_id = id;
            const size_t handled_before = r.h.s->ability_triggers();
            const size_t refused_before = r.rt().refused();
            const size_t ends_before = r.m.ends.size();
            r.send_trigger(guid, id);
            r.h.step(milliseconds(20));
            r.absorb();
            const bool handled = r.h.s->ability_triggers() > handled_before;
            if (handled && r.rt().refused() > refused_before && !r.h.hold) {
                if (r.m.ends.size() == ends_before || r.m.ends.back().second != id) ++refused_unanswered;
            }
        } else if (op < 10) {   // a cancel
            const int32_t id = rng.u(3) == 0 ? static_cast<int32_t>(rng.next()) : (r.next_id > 0 ? static_cast<int32_t>(rng.u(r.next_id)) : 0);
            r.cancel(id);
        } else if (op < 12) {   // a stall of the server loop
            r.h.step(milliseconds(1000 + rng.u(30000)));
            r.absorb();
        } else if (op < 13 && with_holds) {   // the client hears nothing for a while
            r.h.hold = true;
            r.play(0.3 + rng.u(30) / 10.0, rng.u(2) == 0);
            r.h.release();
            r.absorb();
        } else {
            r.play(0.1 + rng.u(25) / 10.0, rng.u(3) != 0);
        }
        for (int i = 0; i < 3; ++i) {
            r.h.step(milliseconds(20));
            r.absorb();
        }
        check_invariants(r, "random operations", it % 4 == 0);
        if (g_violations > 20) break;
    }
    r.play(2.0, false);
    check_invariants(r, "random operations, at the end", true);
    if (refused_unanswered) violate("a refused trigger was not answered (seed " + std::to_string(seed) + ")");
}
// built by CoA with C_Y_B

static void scenario_bit_fuzz(uint64_t seed, int iterations) {
    Run r;
    Rng rng(seed);
    r.play(0.5, false);
    const auto pawn = r.h.s->bootstrap().setup().guids.pawn;
    for (int it = 0; it < iterations; ++it) {
        const int64_t guids[] = {kSprint, kDodge, kRest, kScepterCombo};
        BunchWriter good;
        switch (rng.u(5)) {
            case 0: good = trigger_bunch(r.h.ability_guid(), guids[rng.u(4)], r.next_id++); break;
            case 1: good = cancel_bunch(r.h.ability_guid(), static_cast<int32_t>(rng.u(8))); break;
            case 2: good = ability_rpc(r.h.ability_guid(), "ServerTriggerAbility", hostile_params(guids[rng.u(4)], r.next_id++, rng.u(6), rng.u(2), rng.u(3), pawn)); break;
            case 3: good = trigger_bunch(r.h.ability_guid(), kSprint, r.next_id++); break;
            default: good = trigger_bunch(r.h.ability_guid(), kDodge, r.next_id++); break;
        }
        if (good.bit_pos() == 0) continue;
        const BunchWriter bad = rng.u(8) == 0 ? good : mutate(good, rng);
        r.h.client_sends({r.h.client.actor(r.h.pawn_ch, bad, /*reliable=*/rng.u(4) != 0)});
        r.h.flush();
        r.absorb();
        for (int i = 0; i < 2; ++i) {
            r.h.step(milliseconds(20 + rng.u(400)));
            r.absorb();
        }
        if (rng.u(5) == 0) r.play(0.2, rng.u(2) == 0);
        check_invariants(r, "bit mutations", false);
        if (g_violations > 20) break;
    }
    r.play(2.0, false);
    check_invariants(r, "bit mutations, at the end", false);
}

static void test_hostile_client() {
    std::printf("a seeded hostile client\n");
    // the log would hold hundreds of thousands of lines: off for the scenarios
    const spdlog::level::level_enum level = spdlog::default_logger()->level();
    spdlog::default_logger()->set_level(spdlog::level::off);
    g_violations = 0;
    for (uint64_t seed = 1; seed <= 3; ++seed) scenario_random_ops(seed, 150, seed % 2 == 0);
    const int random_violations = g_violations;
    for (uint64_t seed = 101; seed <= 103; ++seed) scenario_bit_fuzz(seed, 450);
    spdlog::default_logger()->set_level(level);
    CHECK(random_violations == 0, "random operations (hostile ids, guids, stalls, held acknowledgements): every invariant holds, 3 seeds of 150 operations");
    CHECK(g_violations == random_violations, "triggers and cancels damaged bit by bit (and hostile Targets): every invariant holds, 3 seeds of 450 mutated bunches");
}

// ============================================================================================================================
//  plan D2, stage 1: the generic ability runtime in a session, on the real content.db (the ability tables are only there)
// ============================================================================================================================

static constexpr int64_t kFlourish = 6064630877217018486ll, kFortissimo = 6064632972407039861ll, kMaddening = 6064630735633055744ll;
static constexpr int64_t kGleeEffect = 6064630759601406637ll, kPepEffect = 6064631156112822511ll, kResonanceEffect = 6064632468947286527ll;
static constexpr int64_t kHealthStat = 108986352686712ll, kManaStat = 109183576135288ll, kMaxManaStat = 109183578756729ll, kResonanceStat = 6064632920367763592ll;
static constexpr int64_t kMitigationStat = 6064628617716498440ll;
static constexpr int64_t kShielding = 6064630809885999134ll, kLingerTick = 6064630809886064675ll, kAoeTargeting = 6064630809886064674ll;
static constexpr int64_t kTargetingSelf = 6064628968549646338ll;

/// A level-25 Bard: the ranks the game grants at 25 (QuickWit, Flourish, Fortissimo ...) and what a level gives.
static svc::Character bard25() {
    svc::Character c = cyb_record();
    c.level = 25;
    return c;
}

/// A ServerTriggerAbility that names the caster's own collision cylinder as its one target (what the retail client sends for an ability aimed at
/// an ally), or none.
static BunchWriter trigger_aimed(const bs::PlayerSetup& setup, int64_t guid, int32_t id, bool target) {
    const schema::RpcSchema* rpc = class_named("UAoCAbilityComponent").rpc("ServerTriggerAbility");
    repl::RpcParams p;
    bool ok = rpc && repl::set_rpc_param(*rpc, p, "AbilityGuid", repl::Value::i64(guid));
    if (ok && id != 0) ok = repl::set_rpc_param(*rpc, p, "ClientInstanceId", repl::Value::i64(id));
    ok = ok && repl::set_rpc_param(*rpc, p, "InputVector", repl::Value(repl::Vec2d{1, 0}));
    if (ok && target) {
        repl::ArrayValue targets;
        repl::ElementState e;
        e.values[1] = repl::Value(setup.guids.collision_cylinder);
        targets.elements.push_back(std::move(e));
        ok = repl::set_rpc_param(*rpc, p, "Targets", repl::Value(std::move(targets)));
    }
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.Slot", repl::Value::i64(-1));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.ItemInstance", repl::Value(repl::ItemInstance{}));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.Location", repl::Value::u64(24));
    if (!ok) {
        std::printf("  [FAIL] the aimed trigger's parameters do not fit\n");
        return BunchWriter();
    }
    return ability_rpc(setup.guids.ability, "ServerTriggerAbility", p);
}

static int32_t trigger_aimed_now(Run& r, int64_t guid, bool target) {
    const int32_t id = r.next_id++;
    r.send_reliable(trigger_aimed(r.h.s->bootstrap().setup(), guid, id, target));
    return id;
}

static size_t cooldown_items(const Run& r) {
    std::set<int64_t> abilities;
    for (const auto& [id, guid] : r.m.cooldowns) abilities.insert(guid);
    return abilities.size();
}

/// "Glee goes on cooldown but does not heal" (the in-game test of D1): Quick Wit cast at the caster costs Mana, starts its cooldown, gives Glee,
/// and the caster's Health rises, at once and with each of Glee's ticks, up to the maximum.
static void test_d2_glee_heals() {
    std::printf("D2: Quick Wit (Glee) heals the caster\n");
    const svc::Character rec = bard25();
    Run r(0, true, &rec);
    r.play(0.5, true);
    CHECK(r.h.s->pipeline() != nullptr && r.h.s->pipeline()->handles(kQuickWit) && r.h.s->pipeline()->known(kQuickWit), "the generic runtime runs Quick Wit, which the level-25 Bard knows");
    const float max_health = r.m.stats[kHealthStat];
    CHECK(r.h.s->set_current_value(kHealthStat, 300.0f), "(a hit took the Bard down to 300 Health)");
    r.tick(false);
    CHECK(r.m.stats[kHealthStat] >= 300.0f && r.m.stats[kHealthStat] < 320.0f, "(the client has it, with at most a regeneration tick on top)");
    const float mana_before = r.m.stats[kManaStat];
    const float health_before = r.m.stats[kHealthStat];
    const size_t ends_before = r.m.ends.size();
    trigger_aimed_now(r, kQuickWit, true);
    CHECK(r.m.ends.size() == ends_before && !r.m.has_ability(kQuickWit), "the trigger is accepted: no ClientEndAbility, and nothing at the trigger itself (the montage's first notify is ~0.3 s on)");
    r.play(0.5, false);
    CHECK(r.m.has_ability(kQuickWit), "0.5 s later the ability item is there");
    bool cooldown = false;
    for (const auto& [id, guid] : r.m.cooldowns) cooldown = cooldown || guid == kQuickWit;
    CHECK(cooldown && r.m.has_effect(kResonanceEffect), "...with its Cooldowns item and the Bard's Resonance");
    CHECK(near(mana_before - r.m.stats[kManaStat], 56.875, 8.0), "Mana fell by 10 x ManaCostMult(25) = 56.875 (a regeneration tick may have given some back)");
    r.play(0.5, false);
    CHECK(r.m.has_effect(kGleeEffect), "Glee is on the caster");
    CHECK(r.m.stats[kHealthStat] > health_before + 20.0f, "and the Health has risen: the projectile's heal landed (the retail Glee heals ~0.34 s after the cost)");
    CHECK(r.m.stats[kMitigationStat] < 1.0f && r.m.stats[kMitigationStat] > 0.9f, "Mitigation is x (1 - Glee's value): a little under 1");
    const float after_cast = r.m.stats[kHealthStat];
    r.play(4.0, false);
    CHECK(r.m.stats[kHealthStat] > after_cast + 20.0f, "every 2 s Glee's tick heals again, over and above the regeneration");
    r.play(14.0, false);
    CHECK(!r.m.has_effect(kGleeEffect) && r.m.stats[kMitigationStat] == 1.0f, "after its 15 s Glee ends and Mitigation is back");
    CHECK(r.m.stats[kHealthStat] <= max_health && !r.h.s->abilities_failed(), "(never above the maximum; the session is fine)");
    r.play(100.0, false);
    CHECK(r.m.stats[kHealthStat] == max_health, "Health ends at the maximum");
}

/// "Flourish's forward dash is put back by the move checker unless the player jumps": the slide is allowed for its window.
static void test_d2_flourish_dash() {
    std::printf("D2: Flourish's slide against the move checker\n");
    const svc::Character rec = bard25();
    Run control(0, true, &rec);
    control.play(0.5, true);
    for (int i = 0; i < 6; ++i) control.tick_at(3600);   // 3,600 cm/s: the speed Flourish's slide reaches in the retail captures (3,659)
    CHECK(control.h.s->moves_refused() > 0, "(without the cast, 3,600 cm/s is over the limit of 2,135: refused)");
    Run r(0, true, &rec);
    r.play(0.5, true);
    r.trigger(kFlourish);
    for (int i = 0; i < 5; ++i) r.tick_at(3600);   // 0.5 s of slide
    CHECK(r.h.s->moves_refused() == 0 && r.h.s->corrections_sent() == 0, "after Flourish the slide is accepted: no move refused, no correction");
    CHECK(r.m.has_ability(kFlourish), "(the cast went through: its item is on the client)");
    r.play(1.5, true);   // the window (1 s) is over
    for (int i = 0; i < 6; ++i) r.tick_at(3600);
    CHECK(r.h.s->moves_refused() > 0, "once the window is over the speed is refused again");
    Run too_fast(0, true, &rec);
    too_fast.play(0.5, true);
    too_fast.trigger(kFlourish);
    for (int i = 0; i < 6; ++i) too_fast.tick_at(9000);
    CHECK(too_fast.h.s->moves_refused() > 0, "and not even Flourish allows 9,000 cm/s");
}
// ~ C | O | A & C_Y_B ~

/// "Dodging resets the other abilities' cooldowns": the server's Cooldowns array holds every ability on cooldown, so a Dodge's update leaves
/// the others' items where they are.
static void test_d2_dodge_keeps_cooldowns() {
    std::printf("D2: a Dodge after other casts leaves their cooldowns\n");
    const svc::Character rec = bard25();
    Run r(0, true, &rec);
    r.play(0.5, true);
    r.trigger(kFlourish);
    r.play(0.8, false);
    trigger_aimed_now(r, kQuickWit, true);
    r.play(1.2, false);
    CHECK(cooldown_items(r) == 2 && r.h.s->bootstrap().listed_cooldowns() == 2, "Flourish and Quick Wit are on cooldown: the server's array and the client's both list them");
    r.trigger(kDodge);
    r.play(0.4, true);
    CHECK(r.m.has_ability(kDodge) && cooldown_items(r) == 3 && r.h.s->bootstrap().listed_cooldowns() == 3,
          "the Dodge adds its own item: all three are listed (the client's predicted cooldowns are not dropped)");
    bool still = false;
    for (const auto& [id, guid] : r.m.cooldowns) still = still || guid == kFlourish;
    CHECK(still, "Flourish's cooldown is still there after the Dodge's update");
    r.play(1.0, false);   // the Dodge's 0.65 s ends
    CHECK(cooldown_items(r) == 2, "when the Dodge's cooldown ends only its item goes");
    r.play(9.0, false);   // Quick Wit's 8 s
    CHECK(cooldown_items(r) == 1, "Quick Wit's goes at the end of its 8 s");
    r.play(12.0, false);
    CHECK(cooldown_items(r) == 0 && !r.h.s->abilities_failed(), "and Flourish's at the end of its 20 s");
}

/// "Heals show no floating number": Quick Wit's heal at full Health is confirmed to the client with its amount (the Value the formula gave, an
/// EffectiveValue of 0) in ClientHitConfirmationsReliable, the message the retail client draws its combat text from.
static void test_d2_floating_numbers() {
    std::printf("D2: a heal at full Health sends its amount in a hit confirmation\n");
    const svc::Character rec = bard25();
    Run r(0, true, &rec);
    r.play(0.5, true);
    CHECK(r.m.hits.empty() && r.m.hit_calls == 0, "(nothing confirmed before a cast)");
    trigger_aimed_now(r, kQuickWit, true);
    r.play(1.5, false);
    CHECK(r.m.hit_calls >= 1 && !r.m.hits.empty(), "the cast's projectile hit is confirmed with a ClientHitConfirmationsReliable");
    const repl::HitInstance* heal = nullptr;
    for (const repl::HitInstance& h : r.m.hits)
        if (h.source_record_type == 979581256175327151ll && !h.targets.empty() && !h.targets[0].stat_mods.empty()) heal = &h;
    CHECK(heal != nullptr, "one hit is a projectile's, with a stat mod on its target");
    if (!heal) return;
    const bs::SessionGuids& g = r.h.s->bootstrap().setup().guids;
    CHECK(heal->targets.size() == 1 && heal->targets[0].hit_comp == g.pawn && heal->instigator == g.pawn && heal->predicted_on_autonomous && heal->predicted_on_simulated,
          "it names the Bard's pawn as the target and the instigator, as retail's (HitComp = Instigator)");
    CHECK(heal->source == emit::FIntrepidNetworkGUID{} && heal->source_record_guid != 0, "a projectile's hit has no Source actor, and the projectile record's guid");
    const auto& mod = heal->targets[0].stat_mods[0];
    CHECK(mod.value > 10.0f && mod.effective_value == 0.0f, "the stat mod has the heal's amount and an effective value of 0: the client draws the number although Health is full");
    CHECK(r.h.s->hit_confirmations_sent() == r.m.hits.size() && r.h.s->hit_confirmations_pending() == 0, "(every confirmation went out, none waits)");
    const int64_t heal_inst = heal->inst_guid;   // (copied: `heal` points into r.m.hits, which the next cast grows and may reallocate)
    // with Health missing, the same hit's effective value is what fitted
    CHECK(r.h.s->set_current_value(kHealthStat, r.m.stats[kHealthStat] - 20.0f), "(Health is 20 short)");
    r.play(9.0, false);
    const size_t before = r.m.hits.size();
    trigger_aimed_now(r, kQuickWit, true);
    r.play(1.0, false);
    const repl::HitInstance* second = nullptr;
    for (size_t i = before; i < r.m.hits.size(); ++i)
        if (r.m.hits[i].source_record_type == 979581256175327151ll && !r.m.hits[i].targets.empty() && !r.m.hits[i].targets[0].stat_mods.empty()) second = &r.m.hits[i];
    CHECK(second && second->targets[0].stat_mods[0].effective_value <= second->targets[0].stat_mods[0].value && second->inst_guid != heal_inst, "the next one has its own InstGuid");
}

/// "Shielding Dance shows no shield amount": the cast gives the TempHP effect and its shield in the TempHealth array, the client's LingerTick request is
/// confirmed, damage goes into the shield first, and the shield leaves with its effect.
static void test_d2_shielding() {
    std::printf("D2: Shielding Dance's shield and the client's LingerTick request\n");
    const svc::Character rec = bard25();
    Run r(0, true, &rec);
    r.play(0.5, true);
    r.h.s->teach_ability(kShielding);   // (bought with an Archetype point)
    r.trigger(kShielding);
    r.play(0.7, false);
    CHECK(r.m.shields.size() == 1 && r.m.shields[0].first > 30.0f && r.m.shields[0].first == r.m.shields[0].second, "the TempHealth array holds one shield: Base = Value = its amount");
    const float shield = r.m.shields.empty() ? 0.0f : r.m.shields[0].second;
    CHECK(r.m.has_effect(6064631234954461268ll), "(its TempHP effect is on the Bard)");
    const bs::PlayerSetup& setup = r.h.s->bootstrap().setup();
    r.send_reliable(hit_request_bunch(setup, kShielding, kAoeTargeting, kLingerTick));
    r.play(0.4, false);
    const repl::HitInstance* tick = nullptr;
    for (const repl::HitInstance& h : r.m.hits)
        if (h.record_guid == kLingerTick) tick = &h;
    CHECK(tick && tick->source_record_type == 1841048597116038327ll && tick->source_record_guid == kShielding && tick->source == setup.guids.pawn && tick->targets.size() == 1 && tick->targets[0].stat_mods.size() == 1,
          "the request is answered with a confirmation of the LingerTick hit: the ability as its source, the pawn as its Source, one stat mod");
    CHECK(tick && tick->targets[0].stat_mods[0].value > 0 && tick->predicted_on_autonomous && !tick->predicted_on_simulated, "(the heal's amount, predicted on the autonomous client only)");
    CHECK(r.m.shields.size() == 1 && r.m.shields[0].second == shield, "no second shield: the internal cooldown is on");
    // damage goes into the shield first
    const float health = r.m.stats[kHealthStat];
    const double through = r.h.s->take_damage(r.h.conn, shield * 0.5, r.h.t);
    r.h.flush();
    r.absorb();
    r.tick(false);
    CHECK(near(through, 0.0, 1e-9) && r.m.shields.size() == 1 && near(r.m.shields[0].second, shield * 0.5, 0.05) && r.m.shields[0].first == shield && r.m.stats[kHealthStat] >= health,
          "half the shield's amount in damage: the client's array shows the half left, Health is not touched");
    r.play(9.0, false);
    CHECK(r.m.shields.empty() && !r.m.has_effect(6064631234954461268ll), "after its 8 s the effect and the shield are gone");
    CHECK(!r.h.s->abilities_failed(), "(the session is fine)");
}
// -- C.Y.B x C.O.A --

/// Forged requests, a flood of them, and a flood of held-ability cancels: dropped and counted, nothing breaks.
static void test_d2_hit_hostile() {
    std::printf("D2: forged hit requests and cancel spam\n");
    const svc::Character rec = bard25();
    Run r(0, true, &rec);
    r.play(0.5, true);
    const bs::PlayerSetup& setup = r.h.s->bootstrap().setup();
    r.send_reliable(hit_request_bunch(setup, kShielding, kAoeTargeting, kLingerTick));
    r.play(0.5, false);
    CHECK(r.m.hits.empty() && r.h.s->hit_requests() == 1, "a request for an ability that was not cast is read and dropped: nothing confirmed");
    r.send_reliable(hit_request_bunch(setup, 12345, 678, 910));
    r.send_reliable(hit_request_bunch(setup, kQuickWit, kTargetingSelf, kLingerTick));
    r.play(0.3, false);
    CHECK(r.m.hits.empty(), "a request for an unknown ability, and for a hit that is none of the ability's, are dropped");
    r.h.s->teach_ability(kShielding);
    r.trigger(kShielding);
    r.play(0.4, false);
    for (int i = 0; i < 300; ++i) r.send_reliable(hit_request_bunch(setup, kShielding, kAoeTargeting, kLingerTick, i % 2 == 0));
    r.play(1.0, false);
    CHECK(r.h.s->hit_requests_dropped() > 100, "300 requests at once: the budget drops most of them");
    CHECK(r.m.hits.size() <= 12, "and a cast answers at most a handful of the ones it takes");
    CHECK(!r.h.s->abilities_failed(), "(the session is fine after the flood)");
    Run c(0, true, &rec);
    c.play(0.5, true);
    for (int i = 0; i < 200; ++i) c.cancel(i + 1);
    c.play(0.5, false);
    CHECK(c.h.s->held_cancels_dropped() > 100 && c.h.s->held_cancels() <= 216, "200 held-ability cancels at once: the budget drops most of them");
    CHECK(!c.h.s->abilities_failed(), "(the session is fine)");
}

/// General_Sprint's formula counts the Universal sprint ranks the character holds: 1.3 for a level-25 Bard (retail: 1.3), 1.2 with none.
static void test_d2_sprint_speed() {
    std::printf("D2: the Sprint's speed from its formula\n");
    const svc::Character rec = bard25();
    Run r(0, true, &rec);
    r.play(0.5, true);
    r.trigger(kSprint);
    CHECK(r.m.has_effect(kSprintEffect) && near(r.m.stats[kMoveSpeedMult], 1.3, 1e-6), "a level-25 Bard sprints at MoveSpeedMult 1.3: 780 cm/s (retail's)");
    r.play(0.5, true);
    r.cancel(0);
    r.play(0.5, true);
    CHECK(r.m.stats[kMoveSpeedMult] == 1.0f, "and it is 1.0 again when the Sprint ends");
    const svc::Character one = cyb_record();
    Run low(0, true, &one);
    low.play(0.5, true);
    low.trigger(kSprint);
    CHECK(near(low.m.stats[kMoveSpeedMult], 1.2, 1e-6), "a level-1 Bard (no Sprint Training yet) sprints at 1.2");
}
// CoA : C Y B

/// What a client can do to the generic runtime: unknown abilities, an ability on cooldown, one it cannot pay for, a burst, a trigger while
/// another is being cast, a ClientInstanceId it used before, an ability that needs a target with none.
static void test_d2_hostile() {
    std::printf("D2: hostile triggers\n");
    const svc::Character rec = bard25();
    Run r(0, true, &rec);
    r.play(0.5, true);
    const bs::PlayerSetup setup = r.h.s->bootstrap().setup();
    // an ability that is nothing: ClientEndAbility and nothing else
    const size_t items = r.m.abilities.size();
    const int32_t unknown = r.next_id++;
    r.send_reliable(trigger_aimed(setup, 4242424242ll, unknown, false));
    r.play(0.5, false);
    CHECK(r.m.end_ids() == std::vector<int32_t>{unknown} && r.m.abilities.size() == items && !r.h.s->abilities_failed(), "an unknown ability is answered with ClientEndAbility, nothing changes");
    // a real ability the character does not know (Maddening is bought with a talent point)
    const int32_t not_known = r.next_id++;
    r.send_reliable(trigger_aimed(setup, kMaddening, not_known, false));
    r.play(0.5, false);
    CHECK(r.m.end_ids().size() == 2 && r.m.end_ids()[1] == not_known && !r.m.has_ability(kMaddening), "a real ability the character does not know is refused the same way");
    // one that needs a target, with none
    const int32_t no_target = r.next_id++;
    r.send_reliable(trigger_aimed(setup, kQuickWit, no_target, false));
    r.play(0.5, false);
    CHECK(r.m.end_ids().size() == 3 && r.m.end_ids()[2] == no_target && !r.m.has_ability(kQuickWit), "Quick Wit with no target is refused (it needs one)");
    // Flourish; a second ability at once is another cast during the first's: refused
    const int32_t first = r.trigger(kFlourish);
    r.play(0.1, false);
    const int32_t during = r.trigger(kFortissimo);   // while Flourish's cost is still due
    r.play(0.6, false);
    CHECK(r.m.has_ability(kFlourish) && r.m.end_ids().size() == 4 && r.m.end_ids()[3] == during, "a trigger while another ability's cast has not finished is refused (the first goes on)");
    // a stale prediction key: the id of the cast that was accepted, again
    const size_t ends = r.m.end_ids().size();
    r.send_trigger(kFlourish, first);
    r.play(0.5, false);
    CHECK(r.m.end_ids().size() == ends && !r.h.s->abilities_failed(), "an id the session used before is dropped with no answer (ClientEndAbility would end the real instance)");
    // out of charges: Flourish has two, the third use is refused
    r.play(1.0, false);
    r.trigger(kFlourish);
    r.play(1.0, false);
    const int32_t third = r.trigger(kFlourish);
    r.play(1.0, false);
    CHECK(r.m.end_ids().back() == third, "with both of Flourish's charges spent a third use is refused");
    // one it cannot pay for: Mana 10
    Run poor(0, true, &rec);
    poor.play(0.5, true);
    poor.h.s->set_current_value(kManaStat, 10.0f);
    poor.tick(false);
    const int32_t broke = poor.trigger(kFlourish);
    poor.play(0.6, false);
    CHECK(!poor.m.has_ability(kFlourish) && poor.m.end_ids() == std::vector<int32_t>{broke} && poor.m.stats[kManaStat] < 30.0f, "an ability that costs more Mana than the character has is refused: nothing is spent");
    // a burst within and beyond the budget: nothing breaks
    Run spam(0, true, &rec);
    spam.play(0.5, true);
    for (int i = 0; i < 40; ++i) spam.trigger(kFlourish);
    spam.play(1.0, false);
    CHECK(!spam.h.s->abilities_failed() && spam.h.s->ability_triggers_dropped() > 0 && cooldown_items(spam) == 1 && spam.h.s->pipeline()->charges_left(kFlourish) == 0,
          "40 triggers in one frame: the budget drops the surplus, the casts that wait take Flourish's two charges and the rest end at their cost step, the session is fine");
    CHECK(spam.m.unreadable == 0, "(every chain the server sent reads)");
}

// ---- plan D2, stage 2: songs, held abilities, the client's end of an ability, class resources ---------------------------------------------

static constexpr int64_t kCheerful = 6064630831284420608ll, kCheerfulSelf = 6064630831382003720ll;
static constexpr int64_t kLightningStrike = 6064628894041374720ll;   // a Mage's held ability (it has a hold animation)
static constexpr int64_t kFocus = 6064633175093543338ll;             // the Ranger's class resource

static svc::Character class25(int class_id, const char* name, int64_t archetype) {
    svc::Character c = cyb_record();
    c.class_id = class_id;
    c.class_name = name;
    c.archetype_id = archetype;
    c.level = 25;
    return c;
}

/// ServerEndAbility(ServerInstanceId): the client ends an ability instance of the server's.
static BunchWriter end_bunch(const emit::FIntrepidNetworkGUID& ability, int64_t server_instance) {
    const schema::RpcSchema* rpc = class_named("UAoCAbilityComponent").rpc("ServerEndAbility");
    repl::RpcParams p;
    const bool ok = rpc && repl::set_rpc_param(*rpc, p, "ServerInstanceId", repl::Value::i64(server_instance));
    if (!ok) {
        std::printf("  [FAIL] the end's parameters do not fit\n");
        return BunchWriter();
    }
    return ability_rpc(ability, "ServerEndAbility", p);
}

static size_t effect_hits(const Run& r, int64_t source_guid) {
    size_t n = 0;
    for (const repl::HitInstance& h : r.m.hits)
        if (h.source_record_type == 3377240744094973029ll && h.source_record_guid == source_guid) ++n;
    return n;
}

/// A song: the effects come 2.3 s after the press, its loop effect ticks and each tick's hits are confirmed, a second press ends it and nothing is paid.
static void test_d2_song() {
    std::printf("D2: a song plays, ticks and ends with a second press\n");
    const svc::Character rec = bard25();
    Run r(0, true, &rec);
    r.play(0.5, true);
    r.h.s->teach_ability(kCheerful);
    const int32_t first = trigger_aimed_now(r, kCheerful, false);
    r.play(1.0, false);
    CHECK(r.m.has_ability(kCheerful) && !r.m.has_effect(kCheerfulSelf), "the press plays the song's montage; its effects are not on yet");
    r.play(2.0, false);
    CHECK(r.m.has_effect(kCheerfulSelf), "2.3 s after the press the song's own effect is on (it has no end)");
    const size_t at_3 = r.m.hits.size();
    r.play(6.0, false);
    CHECK(r.m.has_effect(kCheerfulSelf) && r.m.hits.size() > at_3, "the song goes on ticking: its hits are confirmed as it plays");
    CHECK(r.h.s->hit_confirmations_pending() == 0 && !r.h.s->abilities_failed(), "(nothing waits, nothing failed)");
    const size_t ends = r.m.end_ids().size();
    trigger_aimed_now(r, kCheerful, false);
    r.play(1.0, false);
    CHECK(!r.m.has_effect(kCheerfulSelf) && !r.h.s->abilities_failed() && r.m.end_ids().size() >= ends, "a second press ends the song: its effect is gone");
    const size_t after_end = r.m.hits.size();
    r.play(5.0, false);
    CHECK(r.m.hits.size() == after_end && r.m.unreadable == 0, "...and nothing ticks any more (every chain the server sent reads)");
    (void)first;
}

/// A held ability: the client's cancel ends it quietly (the item goes, no ClientEndAbility); a cancel again, or of an id that is not held, is ignored.
static void test_d2_held_cancel() {
    std::printf("D2: a held ability is ended by the client's cancel, quietly\n");
    const svc::Character rec = class25(8, "Mage", 17750);
    Run r(0, true, &rec);
    r.play(0.5, true);
    r.h.s->teach_ability(kLightningStrike);
    const int32_t id = trigger_aimed_now(r, kLightningStrike, false);
    r.play(0.6, false);
    const bool held = r.m.has_ability(kLightningStrike);
    CHECK(held, "the Mage's held ability is cast: its item is on the pawn");
    const size_t ends = r.m.end_ids().size();
    r.cancel(id);
    r.play(0.5, false);
    CHECK(r.m.end_ids().size() == ends && !r.h.s->abilities_failed(), "the cancel is quiet: the server sends no ClientEndAbility for it");
    r.cancel(id);
    r.cancel(id);
    r.cancel(12345);
    r.play(0.5, false);
    CHECK(r.m.end_ids().size() == ends && !r.h.s->abilities_failed() && r.m.unreadable == 0, "a cancel again and a cancel of an unknown id change nothing");
}
// C-O-A.C_Y_B

/// ServerEndAbility: the client ends an instance it was given; 259 ms later the item's montage is cleared and the client's instance is ended.
static void test_d2_end_ability() {
    std::printf("D2: ServerEndAbility is answered with ClientEndAbility\n");
    const svc::Character rec = bard25();
    Run r(0, true, &rec);
    r.play(0.5, true);
    const int32_t id = trigger_aimed_now(r, kQuickWit, true);
    r.play(0.4, false);
    int64_t server_id = -1;
    for (const auto& [k, a] : r.m.abilities)
        if (a.ability == kQuickWit) server_id = a.instance;
    CHECK(server_id >= 0, "the cast's item names its ServerInstanceId");
    const size_t ends = r.m.end_ids().size();
    r.send_reliable(end_bunch(r.h.ability_guid(), server_id));
    r.play(0.1, false);
    CHECK(r.m.end_ids().size() == ends, "(the answer is not at once)");
    r.play(0.5, false);
    CHECK(r.m.end_ids().size() == ends + 1 && r.m.end_ids().back() == id, "259 ms on the client's own instance id is ended");
    r.send_reliable(end_bunch(r.h.ability_guid(), server_id));
    r.send_reliable(end_bunch(r.h.ability_guid(), 987654321));
    r.play(1.0, false);
    CHECK(r.m.end_ids().size() == ends + 1 && !r.h.s->abilities_failed() && r.m.unreadable == 0, "an end of the same instance again, or of one that is not ours, is ignored");
}

/// A Ranger's Focus is a held resource: it is in the Owner array, the session moves it with its regeneration (+3 a second) and the client sees it.
static void test_d2_class_resource() {
    std::printf("D2: a Ranger's Focus ticks\n");
    const svc::Character rec = class25(1, "Ranger", 17751);
    Run r(0, true, &rec);
    r.play(0.5, true);
    CHECK(r.m.stats.count(kFocus) == 1, "Focus is one of the Ranger's Owner stats");
    CHECK(r.h.s->set_current_value(kFocus, 10.0f), "(Focus is set to 10)");
    r.tick(false);
    const float start = r.m.stats[kFocus];
    r.play(5.0, false);
    const float later = r.m.stats[kFocus];
    CHECK(near(start, 10.0, 1.0) && later > start + 8.0f && later <= 100.0f, "Focus rises with its regeneration (+3 a second, 10 -> about 25 in 5 s)");
    CHECK(!r.h.s->abilities_failed() && r.m.unreadable == 0, "(nothing failed)");
}

/// "Sprint no longer consumes Stamina" (the in-game test of D2's stage 2): a Sprint that moves drains 5 a second, and the stat the client regenerates by
/// (StaminaRegeneration, which the retail server sets to 0 with the Sprint's first charge, at its end and at a Dodge, and back to 5 when the pause is over)
/// says so: nothing regenerates while it runs.
static void test_d2_sprint_drain() {
    std::printf("D2: a Sprint drains Stamina, and the client's StaminaRegeneration is 0 while it does\n");
    const svc::Character rec = bard25();
    Run r(0, true, &rec);
    r.play(0.5, true);
    CHECK(r.m.stats.count(kStaminaRegeneration) != 0 && near(r.m.stats[kStaminaRegeneration], 5.0, 1e-6), "joined: the client's StaminaRegeneration is 5");
    const float full = r.m.stamina_now();
    const size_t before = r.m.stamina.size();
    const int32_t id = r.trigger(kSprint);
    CHECK(r.m.stats[kStaminaRegeneration] == 0.0f, "the Sprint's first packet sets it to 0 (retail: with the first charge, while the player moves)");
    r.play(5.0, true);
    CHECK(near(full - r.m.stamina_now(), 26.25, 2.6), "5 s of Sprint while moving: Stamina fell by about 25 (5 a second)");
    bool only_down = r.m.stamina.size() > before + 15;
    for (size_t i = before + 1; i < r.m.stamina.size(); ++i) only_down = only_down && near(r.m.stamina[i - 1].second - r.m.stamina[i].second, 1.25, 1e-6);
    CHECK(only_down, "...in steps of 1.25 and never a step up: nothing regenerated meanwhile");
    CHECK(r.m.stats[kStaminaRegeneration] == 0.0f, "the client's StaminaRegeneration stayed 0 the whole time");
    // the player stops while the Sprint is held: nothing is spent, and the regeneration the stat gives is back (the formula: 0 only while the player moves)
    r.play(1.0, false);
    CHECK(near(r.m.stats[kStaminaRegeneration], 5.0, 1e-6), "the player stands in a held Sprint: StaminaRegeneration is 5 again");
    r.play(1.0, true);
    CHECK(r.m.stats[kStaminaRegeneration] == 0.0f, "...and 0 when it moves on");
    const float low = r.m.stamina_now();
    r.cancel(id);
    r.play(0.5, true);
    CHECK(r.m.stats[kStaminaRegeneration] == 0.0f && r.m.stamina_now() == low, "the Sprint is released: the 1 s pause keeps StaminaRegeneration at 0 and the bar still");
    r.play(1.2, true);
    CHECK(near(r.m.stats[kStaminaRegeneration], 5.0, 1e-6) && r.m.stamina_now() > low, "once the pause is over it is 5 again and the bar rises");
    // a Dodge: 0 with it (the Evasion and the pause that follows), 5 after
    r.play(8.0, false);
    r.trigger(kDodge);
    r.play(0.2, true);
    CHECK(r.m.stats[kStaminaRegeneration] == 0.0f, "a Dodge sets it to 0 with its first packet [R: capture 745]");
    r.play(2.0, false);
    CHECK(near(r.m.stats[kStaminaRegeneration], 5.0, 1e-6) && !r.h.s->abilities_failed(), "and it is 5 once the Evasion and the pause are over");
}

/// plan E1 stage 1 fix: "Sprint no longer drains Stamina on the client".  The pawn's own stat updates (the Stamina drop items and the StaminaRegeneration 0) must
/// reach the owner after the phase-in with the whole living area loaded and the actor system opening hundreds of NPCs, exactly as without it.
static void test_e1_sprint_with_world() {
    std::printf("E1: a Sprint after the phase-in, with the living area around the player\n");
    const svc::Character rec = bard25();
    Run r(0, true, &rec);
    std::string error;
    CHECK(r.h.fx.world->load_area(spawns::AreaConfig{}, error), ("the living area loads (" + error + ")").c_str());
    {
        const auto bytes = capture_blocks::bytes_of(capture_blocks::kClientReadyToPhaseIn.hex);
        BunchWriter phase;
        phase.write_bit_range(bytes.data(), 0, capture_blocks::kClientReadyToPhaseIn.bits);
        r.send_reliable(phase);
    }
    r.play(3.0, true);
    CHECK(r.h.s->actor_view() && r.h.s->actor_view()->count(world::ClientState::Awake) > 20, "(the world's actors are open on the client)");
    CHECK(r.m.stats.count(kStaminaRegeneration) != 0 && near(r.m.stats[kStaminaRegeneration], 5.0, 1e-6), "joined: the client's StaminaRegeneration is 5");
    const float full = r.m.stamina_now();
    const size_t before = r.m.stamina.size();
    const int32_t id = r.trigger(kSprint);
    CHECK(r.m.stats[kStaminaRegeneration] == 0.0f, "the Sprint's first packet sets StaminaRegeneration to 0 on the client");
    r.play(5.0, true);
    CHECK(near(full - r.m.stamina_now(), 26.25, 2.6), "5 s of Sprint while moving: the client's Stamina fell by about 25 (5 a second)");
    bool only_down = r.m.stamina.size() > before + 15;
    for (size_t i = before + 1; i < r.m.stamina.size(); ++i) only_down = only_down && near(r.m.stamina[i - 1].second - r.m.stamina[i].second, 1.25, 1e-6);
    CHECK(only_down, "...in steps of 1.25, each one an item the client received");
    CHECK(r.m.stats[kStaminaRegeneration] == 0.0f, "StaminaRegeneration stayed 0 the whole time");
    r.cancel(id);
    r.play(0.5, true);
    CHECK(r.m.stats[kStaminaRegeneration] == 0.0f, "the Sprint is released: the pause keeps StaminaRegeneration at 0");
    CHECK(!r.h.s->abilities_failed() && r.m.unreadable == 0, "(nothing failed)");
}

/// Plan E1, stage 3 fix (the user's run of 2026-10-06: every cooldown ended about 3 s early on the client; Jest showed 5 s of its 8). The client's
/// synchronized clock (UAoCGameInstance, exe 0x145efcf70 / 0x145efe650 / 0x145ef0fc0): ClientSetTime stores NewSynchronizedTime, a core-ticker
/// delegate adds every frame's FApp::DeltaTime to it, and GetSynchronizedTime is the larger of the stored value before the last set and the
/// ticking one. The join's ClientSetTime is processed in the first frame after the client's 3.3 s LoadMap (the user's client log: frame 531 loads
/// 26.15 to 29.41, frame 532 takes the join), and the ticker then adds that frame's whole 3.3 s delta after the set: the client's clock runs ~3.3 s
/// ahead of the server's stamps for the rest of the session. Retail repeats ClientSetTime every 120.0 s (pkt#23, 5128, 8807 ...); worldd sent it
/// once. Now: again 1 s and 5 s after the phase-in (the loading frames are over), then every 120 s.
static void test_e1_clock_resync() {
    std::printf("E1 stage 3 fix: ClientSetTime again after the phase-in and every 120 s\n");
    const svc::Character rec = bard25();
    Run r(0, true, &rec);
    const size_t joined = r.m.set_times.size();   // (the join's own ClientSetTime went out before the mirror started: test_worldd_bootstrap holds it)
    r.play(2.0, false);
    CHECK(r.m.set_times.size() == joined, "nothing more before the phase-in");
    {
        const auto bytes = capture_blocks::bytes_of(capture_blocks::kClientReadyToPhaseIn.hex);
        BunchWriter phase;
        phase.write_bit_range(bytes.data(), 0, capture_blocks::kClientReadyToPhaseIn.bits);
        r.send_reliable(phase);
    }
    r.play(0.5, false);
    CHECK(r.m.set_times.size() == joined, "not with the phase-in itself (the client's frames still hitch)");
    r.play(1.0, false);
    CHECK(r.m.set_times.size() == joined + 1 && near(r.m.set_times.back(), bs::effect_clock(r.now()), 1.6),
          "1 s after the phase-in: ClientSetTime again, with the world time of its send on the synchronized clock");
    r.play(4.0, false);
    CHECK(r.m.set_times.size() == joined + 2 && near(r.m.set_times.back(), bs::effect_clock(r.now()), 1.6), "...and again 5 s after it");
    r.play(60.0, false);
    CHECK(r.m.set_times.size() == joined + 2, "...then not for a while");
    r.play(61.0, false);   // 126 s after the phase-in: the 120 s after the second one has passed
    std::printf("  (%zu sent since the join, the last %.2f s behind the clock)\n", r.m.set_times.size() - joined,
                r.m.set_times.empty() ? 0.0 : bs::effect_clock(r.now()) - r.m.set_times.back());
    CHECK(r.m.set_times.size() == joined + 3 && near(r.m.set_times.back(), bs::effect_clock(r.now()), 2.0), "...then every 120 s, as retail (pkt#23 and every 120.0 s after)");
    CHECK(r.m.unreadable == 0, "(every chain reads)");
}
// [CoA x C-Y-B]

/// plan E1 stage 1 fix: the world's actors share the connection with the player's own chains.  A client that is busy (it acknowledges nothing for a while,
/// as a client spawning a hundred NPCs does) must not have its pawn's unreliable stat updates queued behind a flood of opens and dropped when the window
/// fills: the actor system paces itself by what is in flight.
static void test_e1_sprint_under_load() {
    std::printf("E1: a Sprint while the client is busy with the world's actors\n");
    const svc::Character rec = bard25();
    Run r(0, true, &rec);
    std::string error;
    CHECK(r.h.fx.world->load_area(spawns::AreaConfig{}, error), "the living area loads");
    {
        const auto bytes = capture_blocks::bytes_of(capture_blocks::kClientReadyToPhaseIn.hex);
        BunchWriter phase;
        phase.write_bit_range(bytes.data(), 0, capture_blocks::kClientReadyToPhaseIn.bits);
        r.send_reliable(phase);
    }
    r.play(0.3, true);
    // the client stops acknowledging for 3 s (the packets are held), while the player runs a Sprint
    r.h.hold = true;
    size_t peak_flight = 0;
    const int32_t id = r.trigger(kSprint);
    for (int i = 0; i < 30; ++i) {
        r.tick(true);
        peak_flight = std::max(peak_flight, r.h.conn.packets_in_flight());
    }
    CHECK(peak_flight <= 140, "the connection never has more than about a hundred packets in flight while the client acknowledges nothing (the window is 256)");
    CHECK(r.h.conn.unreliable_dropped() == 0, "no unreliable chain of the pawn was dropped from the queue");
    CHECK(r.h.s->actor_view() && r.h.s->actor_view()->stats().throttled > 0, "the actor system held back (throttled passes)");
    r.h.release();
    r.h.settle();
    r.absorb();
    r.play(1.0, true);
    CHECK(r.m.stats[kStaminaRegeneration] == 0.0f, "once the client is back: StaminaRegeneration is 0 on it");
    CHECK(near(r.m.stamina_now(), r.rt().stamina(), 1.3), "...and its Stamina is the runtime's (every drop item arrived)");
    r.cancel(id);
    CHECK(!r.h.s->abilities_failed() && r.m.unreadable == 0, "(nothing failed)");
}

/// E1 stage 1, fix 2: the pawn's updates are reliable, so a lost packet costs the client a Stamina step for a moment and no more.  A long session at the
/// client's frame rate (a move and a server packet every 16 ms), across the 14-bit packet sequence wrap (16,384 packets, about 4.4 minutes at 62 packets a
/// second: the user's Stamina froze 250 s after each join, which first looked like a packet-number wrap and was the movement clock: test_e1_clock_reset),
/// with random loss in bursts both ways: every 16 s the client's Stamina equals the runtime's (a Dodge, then the regeneration back to full).
static void test_e1_long_session_with_loss(unsigned loss_percent, unsigned burst) {
    std::printf("E1: a long session across the packet sequence wrap, %u%% loss, bursts of %u\n", loss_percent, burst);
    const svc::Character rec = bard25();
    Run r(0, true, &rec);
    std::string error;
    r.h.fx.world->load_area(spawns::AreaConfig{}, error);
    {
        const auto bytes = capture_blocks::bytes_of(capture_blocks::kClientReadyToPhaseIn.hex);
        BunchWriter phase;
        phase.write_bit_range(bytes.data(), 0, capture_blocks::kClientReadyToPhaseIn.bits);
        r.send_reliable(phase);
    }
    std::mt19937 rng(7);
    auto link = [&, left = std::make_shared<unsigned>(0)](const std::vector<uint8_t>&) {
        if (*left > 0) {
            --*left;
            return true;
        }
        if (rng() % 100 < loss_percent) {
            *left = burst - 1;
            return true;
        }
        return false;
    };
    if (loss_percent > 0) {
        r.h.lose_to_client = link;
        r.h.lose_to_server = link;
    }
    size_t wraps = 0;
    uint16_t last_seq = r.h.conn.notify().out_seq();
    size_t bad_cycles = 0, cycles = 0;
    for (int i = 0; i < 24000; ++i) {
        r.ts += 0.016f;
        r.h.client_sends({r.h.client.actor(r.h.pawn_ch, client_move(move_at(r.ts, r.x, false)), false)});
        r.h.step(milliseconds(16));
        r.absorb();
        const uint16_t seq = r.h.conn.notify().out_seq();
        if (seq < last_seq) ++wraps;
        last_seq = seq;
        if (i % 1000 == 100) r.trigger(kDodge);
        if (i % 1000 == 999) {
            ++cycles;
            // 16 s later the bar is full again, on the runtime and on the client
            if (!near(r.m.stamina_now(), r.rt().stamina(), 1.3)) {
                ++bad_cycles;
                std::printf("    cycle %zu at step %d: client Stamina %.2f, runtime %.2f (out seq %u)\n", cycles, i, r.m.stamina_now(), r.rt().stamina(), seq);
            }
        }
    }
    std::printf("    wraps %zu, cycles %zu, bad %zu, packets in flight %zu, unreliable dropped %zu, lost to client %zu, to server %zu\n", wraps, cycles, bad_cycles, r.h.conn.packets_in_flight(), r.h.conn.unreliable_dropped(), r.h.lost_to_client, r.h.lost_to_server);
    CHECK(wraps >= 1 && bad_cycles == 0, "the client's Stamina follows the runtime's across the packet sequence wrap");
}

/// "Stamina works at the join, then stops updating; a relog fixes it" (the in-game test of E1 stage 1, fix 2).  The retail client resets its movement
/// clock (the TimeStamp of every ServerMovePacked) to 0 each time it passes 240 s (retail captures 62, 68, 83, 85, 107, 109: 239.98, then 0.02).  The
/// checker took a fall of the clock for a duplicate unless it was 2 h (the UE default): every move for the next 240 s was dropped unanswered, so the
/// client's saved moves piled up ("Hit limit of 96 saved moves"), the server never heard the player move again (the Sprint's drain, the regeneration
/// pause, the saved position) and the HUD's Stamina stood still.
static void test_e1_clock_reset() {
    std::printf("E1: the client's movement clock resets at 240 s\n");
    Run r;
    r.ts = 238.0f;   // a session of 238 s
    r.play(1.0, true);
    const size_t acked_before = r.h.s->moves_acknowledged();
    CHECK(acked_before >= 9, "(the moves of the first 239 s are acknowledged)");
    r.ts = 239.9f;
    r.play(0.1, true);   // 240.0: the client wraps
    r.ts = 0.0f;
    const size_t before_reset = r.h.s->moves_acknowledged();
    r.play(1.0, true);
    CHECK(r.h.s->moves_acknowledged() >= before_reset + 9, "the moves after the reset (TimeStamp 0.1 ...) are acknowledged, one by one");
    CHECK(r.h.s->moves_stale() == 0 && r.h.s->moves_refused() == 0, "none is dropped as a duplicate, none refused");
    CHECK(r.h.s->movement().good().time_stamp < 5.0f, "the last good TimeStamp follows the client's new clock");
    const int32_t id = r.trigger(kSprint);
    r.play(3.0, true);
    CHECK(r.m.stamina_now() < 96.0f && r.rt().sprinting(), "a Sprint run after the reset drains the client's Stamina (the server hears the player move)");
    CHECK(r.m.stamina.size() >= 10, "...in steps the HUD sees");
    r.cancel(id);
    // a second reset, 240 s later
    r.ts = 239.95f;
    r.play(0.2, true);
    r.ts = 0.0f;
    const size_t before_second = r.h.s->moves_acknowledged();
    r.play(1.0, true);
    CHECK(r.h.s->moves_acknowledged() >= before_second + 9, "and again at the next 240 s");
    // a clock that really runs backwards by seconds (a duplicate, a datagram out of order) is still dropped
    const size_t acked = r.h.s->moves_acknowledged();
    r.ts -= 2.0f;
    r.play(0.1, true);
    CHECK(r.h.s->moves_acknowledged() == acked && r.h.s->moves_stale() >= 1, "a TimeStamp 2 s back is no reset: dropped without a reply");
}
// (C-Y-B | C-O-A)

/// "The dances' visual effects do not show" (the in-game test of D2's stage 2): the animation and the effects of the abilities were there, the aura was not:
/// it is an actor the server replicates, which the client draws the zone's effect from.
static void test_d2_dance_actor() {
    std::printf("D2: a dance opens the zone's actor on a channel of its own and destroys it when the dance ends\n");
    const svc::Character rec = bard25();
    Run r(0, true, &rec);
    // the client is ready to phase in, as the retail client is long before its first dance (plan E1: the world's actors, a dance's zone among them, are
    // sent to a client that has phased in)
    {
        const auto bytes = capture_blocks::bytes_of(capture_blocks::kClientReadyToPhaseIn.hex);
        BunchWriter phase;
        phase.write_bit_range(bytes.data(), 0, capture_blocks::kClientReadyToPhaseIn.bits);
        r.send_reliable(phase);
    }
    r.play(0.5, true);
    r.h.s->teach_ability(kShielding);
    r.h.s->teach_ability(kMaddening);
    CHECK(r.m.other.empty() && r.h.s->zone_actors() == 0, "(no actor before a dance)");
    const double t_trigger = r.now();
    r.trigger(kMaddening);
    r.play(0.2, false);
    CHECK(r.m.other.empty(), "nothing at the trigger and 200 ms on: the montage's first notify comes at 300 ms");
    r.play(0.4, false);
    CHECK(r.m.other.size() == 1 && r.m.other[0].open && !r.m.other[0].close, "Maddening Dance: one actor channel opens");
    if (r.m.other.empty()) return;
    const uint32_t channel = r.m.other[0].channel;
    const double opened_at = r.m.other[0].at - t_trigger;
    CHECK(opened_at >= 0.3 && opened_at <= 0.55, "within 300 to 500 ms of the trigger, as retail's (+362 ms)");
    CHECK(r.m.other[0].exports >= 1 && r.m.other[0].bits > 1500 && r.m.other[0].bits < 2600, "it is the retail actor's size: an export bunch (the archetype, the pawn's mesh, its three components) and about 2,000 bits");
    CHECK(channel != r.h.pawn_ch && channel != r.h.pc_ch && r.h.s->zones_opened() == 1 && r.h.s->zone_actors() == 1, "(on a channel of its own; the session holds it)");
    // the dance lasts as long as retail's did (5,383 ms to the actor's destruction), whatever the Bard casts meanwhile
    r.play(3.0, false);
    CHECK(r.m.other.size() == 1 && r.h.s->zone_actors() == 1 && !r.h.s->abilities_failed(), "3.6 s on: no close, no update");
    r.h.s->teach_ability(kFlourish);
    r.trigger(kFlourish);
    r.play(0.8, false);
    CHECK(r.m.other.size() == 1, "(a Flourish meanwhile does not end it)");
    r.play(1.8, false);
    bool faded = false, closed = false;
    double closed_at = 0;
    for (size_t i = 1; i < r.m.other.size(); ++i) {
        const Mirror::ChannelEvent& e = r.m.other[i];
        if (e.channel != channel) continue;
        faded = faded || (!e.open && !e.close && e.bits > 0);
        if (e.close) {
            closed = true;
            closed_at = e.at - t_trigger;
        }
    }
    CHECK(faded && closed, "6.2 s after the trigger: the actor got its fade-out update (bFadingOut) and its channel is closed");
    CHECK(closed && closed_at > 5.1 && closed_at < 5.8, "the close came about 5.4 s after the trigger [R: 5,383 ms in capture 44]");
    CHECK(r.h.s->zone_actors() == 0 && r.h.s->zones_closed() == 1 && r.h.s->zone_ops_pending() == 0, "(the session holds none: one opened, one closed)");
    // the Shielding Dance: its Aura goes with the dance (the effect the zone put on the Bard, which had no end)
    r.play(20.0, false);
    const double t_shield = r.now();
    r.trigger(kShielding);
    r.play(1.0, false);
    CHECK(r.m.has_effect(6064631046974416627ll) && r.m.other.size() > 2, "Shielding Dance: its Aura is on the Bard and a second actor is open");
    const size_t events = r.m.other.size();
    r.play(3.0, false);
    CHECK(r.m.has_effect(6064631046974416627ll), "4 s on: the Aura is still there");
    r.play(2.0, false);
    CHECK(!r.m.has_effect(6064631046974416627ll), "6 s on: the Aura is gone (it stayed for good before)");
    bool second_closed = false;
    double second_at = 0;
    for (size_t i = events; i < r.m.other.size(); ++i) {
        if (r.m.other[i].close) {
            second_closed = true;
            second_at = r.m.other[i].at - t_shield;
        }
    }
    CHECK(second_closed && second_at > 5.0 && second_at < 5.6 && r.h.s->zone_actors() == 0 && !r.h.s->abilities_failed(), "and the second actor is closed about 5.2 s after the trigger [R: 5,123 and 5,371 ms]");
}


// ============================================================================================================================
//  plan E1, stage 3 (part 1): a player's fight with an NPC through the session: the cast names the NPC's collision cylinder, the hit request names its
//  component, the NPC loses Health, comes for the player, bites it, and the player's in-combat flag follows
// ============================================================================================================================

/// ServerTriggerAbility with one target: `target` (an NPC's collision cylinder, as a GUID or named in place).
static BunchWriter trigger_at_value(const bs::PlayerSetup& setup, int64_t guid, int32_t id, const repl::Value& target, int anim_index = 0) {
    const schema::RpcSchema* rpc = class_named("UAoCAbilityComponent").rpc("ServerTriggerAbility");
    repl::RpcParams p;
    bool ok = rpc && repl::set_rpc_param(*rpc, p, "AbilityGuid", repl::Value::i64(guid));
    if (ok && id != 0) ok = repl::set_rpc_param(*rpc, p, "ClientInstanceId", repl::Value::i64(id));
    if (ok && anim_index != 0) ok = repl::set_rpc_param(*rpc, p, "AnimationIndex", repl::Value::i64(anim_index));   // the combo's step (its montage)
    ok = ok && repl::set_rpc_param(*rpc, p, "InputVector", repl::Value(repl::Vec2d{1, 0}));
    if (ok) {
        repl::ArrayValue targets;
        repl::ElementState e;
        e.values[1] = target;
        targets.elements.push_back(std::move(e));
        ok = repl::set_rpc_param(*rpc, p, "Targets", repl::Value(std::move(targets)));
    }
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.Slot", repl::Value::i64(-1));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.ItemInstance", repl::Value(repl::ItemInstance{}));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.Location", repl::Value::u64(24));
    if (!ok) {
        std::printf("  [FAIL] the trigger's parameters do not fit\n");
        return BunchWriter();
    }
    return ability_rpc(setup.guids.ability, "ServerTriggerAbility", p);
}

static BunchWriter trigger_at_object(const bs::PlayerSetup& setup, int64_t guid, int32_t id, const emit::FIntrepidNetworkGUID& object) {
    return trigger_at_value(setup, guid, id, repl::Value(object));
}

/// RequestHitVerification_Server with `c` as its one component.
static BunchWriter hit_request_with(const bs::PlayerSetup& setup, int64_t ability, int64_t targeting, int64_t hit, repl::HitComponentRef c) {
    const schema::RpcSchema* rpc = class_named("UAoCAbilityComponent").rpc("RequestHitVerification_Server");
    repl::HitRequest req;
    c.server_timestamp = 1.0f;
    req.components.push_back(c);
    req.ability_guid = ability;
    req.targeting_guid = targeting;
    req.hit_guid = hit;
    const auto params = rpc ? repl::hit_request_params(*rpc, req) : std::nullopt;
    if (!params) {
        std::printf("  [FAIL] the hit request's parameters do not fit\n");
        return BunchWriter();
    }
    return ability_rpc(setup.guids.ability, "RequestHitVerification_Server", *params);
}

/// RequestHitVerification_Server with `object` as its one component.
static BunchWriter hit_request_at_object(const bs::PlayerSetup& setup, int64_t ability, int64_t targeting, int64_t hit, const emit::FIntrepidNetworkGUID& object) {
    repl::HitComponentRef c;
    c.object = object;
    return hit_request_with(setup, ability, targeting, hit, c);
}

/// The world of the E1 combat tests: ground, the captured data and a Wolf next to the player (off the ground fixture: it stands).
struct WolfWorld {
    uint64_t wolf = 0;
    uint64_t next_actor_id = 5000000;
    bool ground = false;
    int level = 8;
    int extra = 0;                   // E1 stage 3 fix 3: more Wolves, minted right after the first (their blocks follow its), 1 m apart
    std::vector<uint64_t> wolves;    // their ids
    void prepare(PlayerWorld& w) {
        std::string err;
        if (!w.load_blueprints(NPC_MOTION_PATH, err)) {
            std::printf("  [FAIL] the Blueprint table: %s\n", err.c_str());
            return;
        }
        auto nav = nav::AreaNav::from_file(navfix::build_file(), &err);
        ground = nav && w.adopt_ground(std::move(nav), err);
        if (!ground) {
            std::printf("  [FAIL] the ground: %s\n", err.c_str());
            return;
        }
        auto captured = spawns::CapturedData::load(err);
        if (!captured) {
            std::printf("  [FAIL] the captured data: %s\n", err.c_str());
            return;
        }
        std::string path;
        for (const auto& [p, row] : w.blueprints().rows()) {
            if (p.find("/Beast/Canine/Wolf.Default__Wolf_C") != std::string::npos) path = p;
        }
        wolf = combat_rig::spawn_npc(w.registry(), w.npcs(), *captured, next_actor_id, path, kX + 120, kY, kZ, level, 800);
        for (int i = 0; i < extra; ++i) {
            const uint64_t id = combat_rig::spawn_npc(w.registry(), w.npcs(), *captured, next_actor_id, path, kX + 120, kY + 100 * (i + 1), kZ, level, 800);
            if (id != 0) wolves.push_back(id);
        }
    }
};
// [CoA :: C.Y.B]

/// Plan E1, stage 3 fix (the user's run of 2026-10-06: every ability did 0 damage, the weapon swings nothing, Mock healed the caster). The client names an
/// NPC's collision cylinder IN PLACE (ObjectId 1, the export flags, the NPC as the outer, the name "CollisionCylinder"): the server never exported the
/// cylinder, so the client has no GUID for it, as it names its own cylinder in 8 of the e2e capture's 16 Scepter combos and a CharacterMesh0 in retail
/// request 0514. The run's log: "a hit request for 6064631914305224705 / 6064631914305224706 is found nobody" (Lament, at a Wolf), "the hit ... (the
/// client asked): it found nobody" for every hit, and Mock's projectile fired at the caster (the only actor the call resolved to), whose Bard_QuickWit_Apply
/// heal reaches the caster.
static void test_e1_inline_target_session() {
    std::printf("E1 stage 3 fix: a target named in place is the Wolf; Mock harms it and heals nobody\n");
    const svc::Character rec = bard25();
    WolfWorld ww;
    Run r(0, true, &rec, [&](PlayerWorld& w) { ww.prepare(w); });
    CHECK(ww.ground && ww.wolf != 0 && r.h.fx.world->combat() != nullptr, "a world with ground, a Wolf next to the player and its fights");
    if (!ww.ground || ww.wolf == 0 || !r.h.fx.world->combat()) return;
    combat::CombatWorld& cw = *r.h.fx.world->combat();
    const uint64_t wolf = ww.wolf;
    r.play(0.5, false);
    const bs::PlayerSetup setup = r.h.s->bootstrap().setup();
    const world::WorldActor* wa = r.h.fx.world->registry().find(wolf);
    CHECK(wa != nullptr, "(the Wolf is in the registry)");
    if (!wa) return;
    // the cylinder as the client names it when it has no GUID for it: flags 5 (path and checksum), the Wolf as the outer, the component's name
    repl::InlineObject cyl;
    cyl.flags = 5;
    cyl.outer = wa->actor.guid;
    cyl.path = "CollisionCylinder";
    cyl.checksum = 0xb876932f;

    // 1. a weapon swing at it
    const abilities::AbilityDef* axe = r.h.fx.world->ability_catalog()->ability_named("WeaponCombo_Melee_1H_Axe");
    CHECK(axe != nullptr && !axe->hits.empty(), "the 1H axe combo is in the tables");
    if (!axe || axe->hits.empty()) return;
    int64_t targeting = 0;
    for (const content::TargetingRow& t : axe->targetings) {
        if (t.key == axe->hits[0].key) targeting = t.guid;
    }
    r.h.s->teach_ability(axe->row.guid);
    r.h.fx.world->tick_world(r.now());
    const double max_hp = cw.max_health_of(wolf);
    const int32_t id = r.next_id++;
    const BunchWriter trig = trigger_at_value(setup, axe->row.guid, id, repl::Value(cyl));
    r.send_trigger(axe->row.guid, id, &trig);
    r.play(0.6, false);
    const std::vector<int32_t> ends = r.m.end_ids();
    CHECK(r.m.has_ability(axe->row.guid) && std::find(ends.begin(), ends.end(), id) == ends.end(), "the swing at the Wolf named in place is accepted");
    repl::HitComponentRef comp;
    comp.inline_object = cyl;
    r.send_reliable(hit_request_with(setup, axe->row.guid, targeting, axe->hits[0].hit_guid, comp));
    for (int i = 0; i < 5; ++i) {
        r.play(0.1, false);
        r.h.fx.world->tick_world(r.now());
    }
    const repl::HitInstance* conf = nullptr;
    for (const repl::HitInstance& h : r.m.hits) {
        if (!h.targets.empty()) conf = &h;
    }
    CHECK(conf != nullptr, "the hit request naming the cylinder in place is confirmed with a target");
    const double hp1 = cw.health_of(wolf);
    const bool missed = conf && (conf->targets[0].flags & stats::kHitMiss);
    std::printf("  (the Wolf: %.1f of %.1f Health after the swing)\n", hp1, max_hp);
    CHECK(conf && (missed || hp1 < max_hp), "the Wolf lost Health to the swing (or the hit missed)");

    // 2. Mock at it: its Mockery projectile harms the Wolf; the Quick Wit projectile (Jest's heal and Glee) reaches no ally, the caster included
    //    [R: capture 40 line 295: Mock at another actor confirms Bard_Mockery_Apply alone, no Bard_QuickWit_Apply, no Glee]
    constexpr int64_t kMock = 6064632575971298233ll, kMockeryApply = 6064630758756712681ll, kQuickWitApply = 6064632501410136576ll;
    r.h.s->teach_ability(kMock);
    const float health_before = r.m.stats[content::kGuidHealth];
    const size_t hits_before = r.m.hits.size();
    const int32_t mock_id = r.next_id++;
    const BunchWriter mock = trigger_at_value(setup, kMock, mock_id, repl::Value(cyl));
    r.send_trigger(kMock, mock_id, &mock);
    for (int i = 0; i < 20; ++i) {
        r.play(0.1, false);
        r.h.fx.world->tick_world(r.now());
    }
    const std::vector<int32_t> ends2 = r.m.end_ids();
    CHECK(std::find(ends2.begin(), ends2.end(), mock_id) == ends2.end(), "Mock at the Wolf is accepted");
    bool mockery = false, quickwit = false;
    for (size_t i = hits_before; i < r.m.hits.size(); ++i) {
        if (r.m.hits[i].record_guid == kMockeryApply && !r.m.hits[i].targets.empty()) mockery = true;
        if (r.m.hits[i].record_guid == kQuickWitApply) quickwit = true;
    }
    const double hp2 = cw.health_of(wolf);
    std::printf("  (the Wolf: %.1f Health after Mock)\n", hp2);
    CHECK(mockery, "Bard_Mockery_Apply is confirmed on the Wolf");
    CHECK(!quickwit && !r.m.has_effect(kGleeEffect), "Bard_QuickWit_Apply is not confirmed and Glee is not on the caster: Mock heals nobody");
    (void)health_before;   // (the caster's Health regenerates out of combat: it is no measure of a heal; the confirmations and Glee are)
    CHECK(!r.h.s->abilities_failed() && r.m.unreadable == 0, "(the session is fine, every chain reads)");
}
// C.O.A : C-Y-B

static void world_play(Run& r, double seconds);   // (defined with the whole-fight tests below)

/// True when one of the chain's export bunches carries `guid` (an export writes the GUID whole: four uint32 at some bit offset).
static bool exports_carry(const fake::Delivered& d, const emit::FIntrepidNetworkGUID& guid) {
    for (const BunchWriter& e : d.exports) {
        const size_t bits = e.bit_pos();
        for (size_t start = 0; start + 128 <= bits; ++start) {
            size_t pos = start;
            if (emit::read_intrepid_guid(e.data(), e.byte_size() + 1, pos) == guid) return true;
        }
    }
    return false;
}

/// The cylinder of an NPC as the client names it when it has no GUID for it: flags 5 (path and checksum), the NPC as the outer, the component's name.
static repl::InlineObject inline_cylinder_of(const world::WorldActor& wa) {
    repl::InlineObject cyl;
    cyl.flags = 5;
    cyl.outer = wa.actor.guid;
    cyl.path = "CollisionCylinder";
    cyl.checksum = 0xb876932f;
    return cyl;
}

/// The GUID of an NPC's collision cylinder: the registry's (the actor's + 4, retail's slot), with the actor's ServerId and Randomizer.
static emit::FIntrepidNetworkGUID cylinder_guid_of(const world::WorldActor& wa) {
    emit::FIntrepidNetworkGUID g = wa.actor.guid;
    g.ObjectId = wa.npc.collision_cylinder;
    return g;
}

/// E1 stage 3 fix 3: an export record as UE's client reads it (UPackageMapClient::ReceiveNetGUIDBunch: a bit, the count, then per entry the GUID, the
/// export flags and, with a path, the outer's entry, the FString path and the checksum). The client caches every entry that has a path under (path,
/// outer) and drops the connection (NetGuidMismatch, "Outer mismatch" / "Path mismatch") when a later export of the same GUID differs
/// (FNetGUIDCache::RegisterNetGUIDFromPath_Client): the user's disconnect of 2026-10-06 22:18.
struct ExportRecord {
    emit::FIntrepidNetworkGUID guid;
    uint8_t                    flags = 0;
    emit::FIntrepidNetworkGUID outer;   // with the path flag: the outer's GUID (0: none)
    std::string                path;
    uint32_t                   checksum = 0;
};
static bool read_export_entry(const uint8_t* data, size_t len, size_t& pos, std::vector<ExportRecord>& out, emit::FIntrepidNetworkGUID* guid_out, int depth) {
    if (depth > 8) return false;
    ExportRecord r;
    r.guid = emit::read_intrepid_guid(data, len, pos);
    if (guid_out) *guid_out = r.guid;
    if (r.guid.ObjectId == 0) return true;
    r.flags = static_cast<uint8_t>(ue5::read_bits(data, len, pos, 8));
    if (r.flags & 1) {
        if (!read_export_entry(data, len, pos, out, &r.outer, depth + 1)) return false;
        const int32_t n = static_cast<int32_t>(ue5::read_bits(data, len, pos, 32));
        if (n < 0 || n > 512) return false;
        for (int32_t i = 0; i < n; ++i) {
            const char c = static_cast<char>(ue5::read_bits(data, len, pos, 8));
            if (c != 0) r.path.push_back(c);
        }
        if (r.flags & 4) r.checksum = static_cast<uint32_t>(ue5::read_bits(data, len, pos, 32));
    }
    out.push_back(std::move(r));
    return true;
}
static std::vector<ExportRecord> export_records(const fake::Delivered& d) {
    std::vector<ExportRecord> out;
    for (const BunchWriter& e : d.exports) {
        const size_t bits = e.bit_pos();
        const uint8_t* data = e.data();
        const size_t len = e.byte_size() + 1;
        size_t pos = 0;
        (void)ue5::read_bits(data, len, pos, 1);
        const uint32_t count = static_cast<uint32_t>(ue5::read_bits(data, len, pos, 32));
        for (uint32_t i = 0; i < count && pos < bits; ++i) {
            if (!read_export_entry(data, len, pos, out, nullptr, 0)) break;
        }
    }
    return out;
}

/// The client's GUID cache over the chains [from, to) of a run: every export with a path under (path, outer); `mismatches` counts the exports the
/// client would refuse (the same GUID under another path or outer), with the first few described.
struct GuidCache {
    std::map<uint64_t, std::pair<std::string, uint64_t>> held;   // ObjectId -> (path, outer ObjectId)
    size_t mismatches = 0;
    std::vector<std::string> described;
    /// The GUID held under (path, outer); 0 when none is.
    uint64_t find_path(const std::string& path, uint64_t outer) const {
        for (const auto& [id, po] : held) {
            if (po.first == path && po.second == outer) return id;
        }
        return 0;
    }
    void take(const fake::Delivered& d) {
        for (const ExportRecord& r : export_records(d)) {
            if (!(r.flags & 1)) continue;
            const auto it = held.find(r.guid.ObjectId);
            if (it == held.end()) {
                held.emplace(r.guid.ObjectId, std::make_pair(r.path, r.outer.ObjectId));
                continue;
            }
            if (it->second.first != r.path || it->second.second != r.outer.ObjectId) {
                ++mismatches;
                if (described.size() < 4) {
                    described.push_back("GUID " + std::to_string(r.guid.ObjectId) + " exported as " + r.path + " of " + std::to_string(r.outer.ObjectId) +
                                        " but held as " + it->second.first + " of " + std::to_string(it->second.second));
                }
            }
        }
    }
};

/// E1 stage 3 fix 2 (the user's run of 2026-10-06 20:20: "projectiles do not visibly hit the enemy, no damage numbers above enemies, the book's auto
/// attacks do not count"). The ability item's Targets and the confirmation's HitComp named the Wolf's collision cylinder by a GUID (the actor's + 24) the
/// server never exported: the client resolves it to nothing, its homing projectile has nothing to fly at, no impact, hit reaction or floating number is drawn,
/// and the book's projectile never reaches the Wolf, so the client never asks for the Split hit that does the book's damage. Retail exports the target's
/// "CollisionCylinder" (flags 7, checksum 0xb876932f, the NPC as the outer) in the bunch that first names it; so does worldd now, with the update that
/// carries the item and with the confirmation, until the client acknowledges the export.
static void test_e1_npc_cylinder_export() {
    std::printf("E1 stage 3 fix 2: the Wolf's collision cylinder is exported with the item and the confirmation that name it\n");
    const svc::Character rec = bard25();
    WolfWorld ww;
    Run r(0, true, &rec, [&](PlayerWorld& w) { ww.prepare(w); });
    CHECK(ww.ground && ww.wolf != 0 && r.h.fx.world->combat() != nullptr, "a world with ground, a Wolf next to the player and its fights");
    if (!ww.ground || ww.wolf == 0 || !r.h.fx.world->combat()) return;
    r.play(0.5, false);
    const bs::PlayerSetup setup = r.h.s->bootstrap().setup();
    const world::WorldActor* wa = r.h.fx.world->registry().find(ww.wolf);
    CHECK(wa != nullptr, "(the Wolf is in the registry)");
    if (!wa) return;
    // E1 stage 3 fix 3: the cylinder's GUID is the actor's + 4, retail's slot (the AlignmentComponent's - 2 in 211 of 211 captured exports), inside the
    // actor's own block; the draft's + 24 was the next-minted object's GUID (the user's disconnect: NetGuidMismatch)
    const emit::FIntrepidNetworkGUID cylinder = cylinder_guid_of(*wa);
    {
        const repl::Subobject* alignment = nullptr;
        for (const repl::Subobject& s : wa->actor.subobjects) {
            if (s.name == "AlignmentComponent") alignment = &s;
        }
        CHECK(cylinder.ObjectId == wa->id + 4 && alignment && alignment->guid.ObjectId == cylinder.ObjectId + 2 && cylinder.ServerId == wa->actor.guid.ServerId,
              "the Wolf's cylinder GUID is its actor's + 4 = its AlignmentComponent's - 2, with the actor's ServerId and Randomizer [R: 211 of 211 retail exports]");
    }
    const repl::InlineObject cyl = inline_cylinder_of(*wa);
// (C | Y | B : CoA)

    const abilities::AbilityDef* axe = r.h.fx.world->ability_catalog()->ability_named("WeaponCombo_Melee_1H_Axe");
    CHECK(axe != nullptr && !axe->hits.empty(), "the 1H axe combo is in the tables");
    if (!axe || axe->hits.empty()) return;
    int64_t targeting = 0;
    for (const content::TargetingRow& t : axe->targetings) {
        if (t.key == axe->hits[0].key) targeting = t.guid;
    }
    r.h.s->teach_ability(axe->row.guid);
    r.h.fx.world->tick_world(r.now());
    // the client acknowledges nothing until the swing is over: both the item's update and the confirmation are built with the cylinder unacknowledged
    r.h.hold = true;
    const size_t first = r.h.got.size();
    const int32_t id = r.next_id++;
    const BunchWriter trig = trigger_at_value(setup, axe->row.guid, id, repl::Value(cyl));
    r.send_trigger(axe->row.guid, id, &trig);
    r.play(0.6, false);
    repl::HitComponentRef comp;
    comp.inline_object = cyl;
    r.send_reliable(hit_request_with(setup, axe->row.guid, targeting, axe->hits[0].hit_guid, comp));
    for (int i = 0; i < 5; ++i) {
        r.play(0.1, false);
        r.h.fx.world->tick_world(r.now());
    }
    r.h.release();
    r.absorb();
    // the item names the cylinder, and the chain that carries it exports it
    bool item_named = false, item_exported = false, conf_named = false, conf_exported = false;
    for (size_t i = first; i < r.h.got.size(); ++i) {
        const fake::Delivered& d = r.h.got[i];
        if (d.channel == r.h.pawn_ch && exports_carry(d, cylinder)) item_exported = true;
        if (d.channel == r.h.pc_ch && exports_carry(d, cylinder)) conf_exported = true;
    }
    for (const auto& [key, a] : r.m.abilities) item_named = item_named || (a.ability == axe->row.guid && a.has_target);
    bool conf_at_wolf = false;
    for (const repl::HitInstance& h : r.m.hits) {
        for (const repl::HitTarget& t : h.targets) {
            conf_named = conf_named || t.hit_comp == wa->actor.guid;
            const double dx = t.impact_location.x - wa->x, dy = t.impact_location.y - wa->y;
            const double nlen = std::sqrt(t.impact_normal.x * t.impact_normal.x + t.impact_normal.y * t.impact_normal.y + t.impact_normal.z * t.impact_normal.z);
            if (t.hit_comp == wa->actor.guid && std::sqrt(dx * dx + dy * dy) < 100.0 && std::fabs(nlen - 1.0) < 1e-6) conf_at_wolf = true;
        }
    }
    CHECK(item_named, "the swing's item names a target (the Wolf's cylinder)");
    CHECK(item_exported, "the pawn update that carries the item exports the Wolf's collision cylinder (the actor's GUID + 4, its ServerId and Randomizer)");
    CHECK(conf_named, "the hit's confirmation names the Wolf ACTOR as its HitComp (retail: the actor, never the cylinder)");
    CHECK(conf_at_wolf, "its ImpactLocation is at the Wolf and its ImpactNormal a unit vector (retail: the target's position, a unit normal)");
    CHECK(conf_exported, "the confirmation's chain exports the cylinder too (the client had not acknowledged the item's export)");
    // a second swing, acknowledged: the cylinder is not exported again
    r.absorb();
    const size_t second = r.h.got.size();
    const int32_t id2 = r.next_id++;
    const BunchWriter trig2 = trigger_at_value(setup, axe->row.guid, id2, repl::Value(cylinder));   // the client knows the GUID now: it names it so
    r.send_trigger(axe->row.guid, id2, &trig2);
    r.play(0.6, false);
    r.send_reliable(hit_request_at_object(setup, axe->row.guid, targeting, axe->hits[0].hit_guid, cylinder));
    for (int i = 0; i < 5; ++i) {
        r.play(0.1, false);
        r.h.fx.world->tick_world(r.now());
    }
    bool again = false;
    size_t confirmed_by_guid = 0;
    for (size_t i = second; i < r.h.got.size(); ++i) again = again || exports_carry(r.h.got[i], cylinder);
    for (const repl::HitInstance& h : r.m.hits) {
        for (const repl::HitTarget& t : h.targets) confirmed_by_guid += t.hit_comp == wa->actor.guid ? 1 : 0;
    }
    CHECK(!again, "once acknowledged the cylinder is not exported again (UE's ShouldSendFullPath)");
    CHECK(confirmed_by_guid >= 2, "a request naming the cylinder by its GUID is confirmed on the Wolf too");
    CHECK(!r.h.s->abilities_failed() && r.m.unreadable == 0, "(the session is fine, every chain reads)");
}

/// E1 stage 3 fix 2 (the user's run: "enemies die and instantly respawn in the same spot", "no death animation"). The ability runtime counts seconds from
/// the session's start and the combat world and the aftermath are ticked with the server's world time (seconds since worldd started: 102 s at the user's
/// join), and a player's hit reached the combat world with the runtime's time: the death's time was 102 s in the past, so the corpse closed and the spawn
/// point refilled at the very next world tick (the log: every "died ... closes in 15 s" was followed by nothing visible for 15 s). The hit port translates.
static void test_e1_one_clock() {
    std::printf("E1 stage 3 fix 2: a kill through the session is on the world's clock (worldd older than the session)\n");
    const svc::Character rec = bard25();
    WolfWorld ww;
    ww.level = 1;
    g_world_time_offset = 100.0;   // worldd has run 100 s when the session joins
    Run r(0, true, &rec, [&](PlayerWorld& w) { ww.prepare(w); });
    CHECK(ww.ground && ww.wolf != 0 && r.h.fx.world->combat() != nullptr && r.h.fx.world->aftermath() != nullptr, "a world with ground, a level-1 Wolf next to the player, its fights and the aftermath");
    if (!ww.ground || ww.wolf == 0 || !r.h.fx.world->combat() || !r.h.fx.world->aftermath()) {
        g_world_time_offset = 0.0;
        return;
    }
    combat::CombatWorld& cw = *r.h.fx.world->combat();
    combat::Aftermath& after = *r.h.fx.world->aftermath();
    CHECK(r.h.s->bootstrap().setup().world_time >= 100.0, "the join's world time is the server's (100 s)");
    r.play(0.5, false);
    const bs::PlayerSetup setup = r.h.s->bootstrap().setup();
    const world::WorldActor* wa = r.h.fx.world->registry().find(ww.wolf);
    CHECK(wa != nullptr, "(the Wolf is in the registry)");
    if (!wa) {
        g_world_time_offset = 0.0;
        return;
    }
    const repl::InlineObject cyl = inline_cylinder_of(*wa);
    const abilities::AbilityDef* axe = r.h.fx.world->ability_catalog()->ability_named("WeaponCombo_Melee_1H_Axe");
    CHECK(axe != nullptr && !axe->hits.empty(), "the 1H axe combo is in the tables");
    if (!axe || axe->hits.empty()) {
        g_world_time_offset = 0.0;
        return;
    }
    int64_t targeting = 0;
    for (const content::TargetingRow& t : axe->targetings) {
        if (t.key == axe->hits[0].key) targeting = t.guid;
    }
    r.h.s->teach_ability(axe->row.guid);
    r.h.fx.world->tick_world(r.now());
    // one swing: the player is in combat for the data's 6 s (the exchange is stamped on the world's clock)
    const auto swing = [&] {
        const int32_t id = r.next_id++;
        const BunchWriter trig = trigger_at_value(setup, axe->row.guid, id, repl::Value(cyl));
        r.send_trigger(axe->row.guid, id, &trig);
        r.play(0.6, false);
        repl::HitComponentRef comp;
        comp.inline_object = cyl;
        r.send_reliable(hit_request_with(setup, axe->row.guid, targeting, axe->hits[0].hit_guid, comp));
        world_play(r, 0.4);
    };
    const double max_hp = cw.max_health_of(ww.wolf);
    swing();
    world_play(r, 1.0);
    CHECK(cw.health_of(ww.wolf) < max_hp, "the swing hurt the Wolf");
    CHECK(r.h.s->character_stats() && r.h.s->character_stats()->in_combat(), "a second after the swing the player is still in combat (the 6 s window is on the world's clock)");
    // the kill, swing by swing
    double died_at_world = -1;
    for (int i = 0; i < 400 && !cw.dead(ww.wolf); ++i) {
        swing();
        if (cw.dead(ww.wolf)) died_at_world = r.now();
    }
    std::printf("  (the level-1 Wolf: %.1f Health at the start, dead %d)\n", max_hp, cw.dead(ww.wolf) ? 1 : 0);
    CHECK(cw.dead(ww.wolf), "the Wolf dies of the swings");
    if (!cw.dead(ww.wolf)) {
        g_world_time_offset = 0.0;
        return;
    }
    const combat::Corpse* corpse = after.corpse_of(ww.wolf);
    CHECK(corpse != nullptr && std::fabs(corpse->died_at - died_at_world) < 1.5, "the death is stamped on the world's clock (the aftermath's corpse has the world time of the kill)");
    if (corpse) std::printf("  (died_at %.1f, the world's clock %.1f)\n", corpse->died_at, died_at_world);
    world_play(r, 2.0);
    CHECK(r.h.fx.world->registry().find(ww.wolf) != nullptr && after.stats().corpses_closed == 0, "two seconds after the death the corpse still stands (it closes 15 s after the death)");
    world_play(r, 14.0);
    CHECK(r.h.fx.world->registry().find(ww.wolf) == nullptr && after.stats().corpses_closed == 1, "sixteen seconds after the death the corpse is gone");
    CHECK(!r.h.s->abilities_failed() && r.m.unreadable == 0, "(the session is fine, every chain reads)");
    g_world_time_offset = 0.0;
}
// -- C.Y.B x C O A --

/// E1 stage 3 fix 2: the book (the user: "the ranged book does no damage; its auto attacks do not count"). Retail resolves a ranged weapon combo's
/// projectile on the server: 1,272 book swings, not one RequestHitVerification, and ONE confirmation 0.5-1.3 s after the trigger with TWO records at the
/// target: Weapon_Book_Hit_n (no StatMods: its stat mod is the data's "0 ... here for the primary FX ... not to do damage"; predicted on both sides) and
/// Book_Projectile_Split_Hit_n (the damage; predicted on neither), the step's pair [R: research/hitfx 4 and 9, capture 45 packets 101 / 148]. The draft
/// fired every projectile row of the combo (ten FX hits, 0 damage each) and left the Split hits for a request the client never sends: no damage.
static void test_e1_ranged_book_at_wolf() {
    std::printf("E1 stage 3 fix 2: the book's auto attack at a Wolf (the step's FX hit and its Split hit, server-resolved, in one confirmation)\n");
    const svc::Character rec = bard25();
    WolfWorld ww;
    Run r(0, true, &rec, [&](PlayerWorld& w) { ww.prepare(w); });
    CHECK(ww.ground && ww.wolf != 0 && r.h.fx.world->combat() != nullptr, "a world with ground, a Wolf next to the player and its fights");
    if (!ww.ground || ww.wolf == 0 || !r.h.fx.world->combat()) return;
    combat::CombatWorld& cw = *r.h.fx.world->combat();
    r.play(0.5, false);
    const bs::PlayerSetup setup = r.h.s->bootstrap().setup();
    const world::WorldActor* wa = r.h.fx.world->registry().find(ww.wolf);
    CHECK(wa != nullptr, "(the Wolf is in the registry)");
    if (!wa) return;
    const repl::InlineObject cyl = inline_cylinder_of(*wa);
    const abilities::Catalog* cat = r.h.fx.world->ability_catalog();
    const abilities::AbilityDef* book = cat->ability_named("WeaponCombo_Ranged_Book");
    const abilities::HitDef* fx1 = cat->hit_named("Weapon_Book_Hit_1");
    const abilities::HitDef* split1 = cat->hit_named("Book_Projectile_Split_Hit_1");
    const abilities::HitDef* fx2 = cat->hit_named("Weapon_Book_Hit_2");
    const abilities::HitDef* split2 = cat->hit_named("Book_Projectile_Split_Hit_2");
    CHECK(book != nullptr && fx1 && split1 && fx2 && split2, "the book combo and its FX and Split hits of steps 1 and 2 are in the tables");
    if (!book || !fx1 || !split1 || !fx2 || !split2) return;
    CHECK(!split1->fires.empty() && fx1->fires.empty(), "the Split hit fires the split projectile when it lands (ab_hit_projectile), the FX hit fires none");
    r.h.s->teach_ability(book->row.guid);
    r.h.fx.world->tick_world(r.now());
    const double max_hp = cw.max_health_of(ww.wolf);

    // one swing (the first montage): the client sends the trigger and nothing else
    const auto swing = [&](int anim_index, const abilities::HitDef* fx, const abilities::HitDef* split, const char* step) {
        const size_t hits_before = r.m.hits.size();
        const double hp_before = cw.health_of(ww.wolf);
        const int32_t id = r.next_id++;
        const BunchWriter trig = trigger_at_value(setup, book->row.guid, id, repl::Value(cyl), anim_index);
        r.send_trigger(book->row.guid, id, &trig);
        for (int i = 0; i < 15; ++i) {
            r.play(0.1, false);
            r.h.fx.world->tick_world(r.now());
        }
        const std::vector<int32_t> ends = r.m.end_ids();
        CHECK(r.m.has_ability(book->row.guid) && std::find(ends.begin(), ends.end(), id) == ends.end(), (std::string("step ") + step + ": the book's swing at the Wolf is accepted").c_str());
        const repl::HitInstance* fxc = nullptr;
        const repl::HitInstance* spc = nullptr;
        size_t others = 0;
        std::set<int64_t> combo_hits;
        for (const content::AbilityHitRow& h : book->hits) combo_hits.insert(h.hit_guid);
        for (size_t i = hits_before; i < r.m.hits.size(); ++i) {
            const repl::HitInstance& h = r.m.hits[i];
            if (h.record_guid == fx->row.guid) fxc = &h;
            else if (h.record_guid == split->row.guid) spc = &h;
            else if (combo_hits.count(h.record_guid)) {   // (the Wolf's own bites on the player are not the combo's)
                ++others;
                const abilities::HitDef* o = cat->hit(h.record_guid);
                std::printf("  (step %s: another confirmation: %s, %zu target(s), pa %d ps %d)\n", step, o ? o->row.name.c_str() : "?", h.targets.size(),
                            h.predicted_on_autonomous ? 1 : 0, h.predicted_on_simulated ? 1 : 0);
            }
        }
        if (fxc && !fxc->targets.empty()) {
            std::printf("  (step %s: the FX hit's confirmation: %zu StatMod(s), pa %d ps %d", step, fxc->targets[0].stat_mods.size(), fxc->predicted_on_autonomous ? 1 : 0,
                        fxc->predicted_on_simulated ? 1 : 0);
            for (const repl::HitStatMod& m : fxc->targets[0].stat_mods) std::printf(" [%lld %.2f/%.2f]", static_cast<long long>(m.guid), m.value, m.effective_value);
            std::printf(")\n");
        }
        CHECK(fxc && fxc->targets.size() == 1 && fxc->targets[0].hit_comp == wa->actor.guid && fxc->targets[0].stat_mods.empty() && fxc->predicted_on_autonomous && fxc->predicted_on_simulated,
              (std::string("step ") + step + ": the FX hit is confirmed on the Wolf actor with no StatMods, predicted on both sides (retail: pa 1 ps 1)").c_str());
        CHECK(spc && spc->targets.size() == 1 && spc->targets[0].hit_comp == wa->actor.guid && !spc->targets[0].stat_mods.empty() && !spc->predicted_on_autonomous && !spc->predicted_on_simulated,
              (std::string("step ") + step + ": the Split hit is confirmed on the Wolf actor with its StatMod, predicted on neither side (retail: pa 0 ps 0)").c_str());
        CHECK(others == 0, (std::string("step ") + step + ": no other hit of the combo is confirmed (one projectile per swing, not ten)").c_str());
        // E1 stage 3 fix 3 (the user's run of 2026-10-06 22:16: no projectile drawn; the client logged "UCombatHelper::GetTransform(): Failed to find
        // origin location. EAbilitySystemOrigin::Source ... Source , Target CollisionCylinder" and "SpawnProjectile(): Failed to find spawn transform"
        // 13 ms after every book confirmation): the Split hit fires the split projectile FROM ITS SOURCE, which retail names as the target actor the
        // book's projectile landed on, with the projectile that landed as the SourceRecordGuid; the FX hit the projectile carries has no Source
        // [R: 45 packet 148: Weapon_Book_Hit_1 {Weapon_Book_Projectile_1, Source 0}, Book_Projectile_Split_Hit_1 {Weapon_Book_Projectile_1, Source = the
        // target actor 361088}; 1,289 of 1,289 split confirmations name the target, 2,177 of 2,177 carried hits none]
        const content::AbilityHitRow* carrier = nullptr;
        for (const content::AbilityHitRow& h : book->hits) {
            if (h.source == "projectile" && h.hit_guid == fx->row.guid) carrier = &h;
        }
        CHECK(carrier != nullptr, (std::string("step ") + step + ": (the FX hit is carried by a projectile row of the combo)").c_str());
        CHECK(fxc && carrier && fxc->source.ObjectId == 0 && fxc->source_record_type == content::kSourceTypeProjectile && fxc->source_record_guid == carrier->owner_guid,
              (std::string("step ") + step + ": the FX hit has no Source and the carrying projectile as its SourceRecordGuid (retail: Source 0, Weapon_Book_Projectile_n)").c_str());
        CHECK(spc && carrier && spc->source == wa->actor.guid && spc->source_record_type == content::kSourceTypeProjectile && spc->source_record_guid == carrier->owner_guid,
              (std::string("step ") + step + ": the Split hit's Source is the Wolf ACTOR and its SourceRecordGuid the projectile that landed (retail 45/148: the origin of the split projectile)").c_str());
        const double hp = cw.health_of(ww.wolf);
        const bool missed = spc && !spc->targets.empty() && (spc->targets[0].flags & stats::kHitMiss);
        std::printf("  (step %s: the Wolf %.1f -> %.1f of %.1f Health)\n", step, hp_before, hp, max_hp);
        CHECK(spc && (missed || hp < hp_before), (std::string("step ") + step + ": the Wolf lost Health to the Split hit (or it missed)").c_str());
    };
    swing(0, fx1, split1, "1");
    CHECK(r.h.s->character_stats() && r.h.s->character_stats()->in_combat(), "the swing puts the player in combat");
    swing(1, fx2, split2, "2");
    CHECK(!r.h.s->abilities_failed() && r.m.unreadable == 0, "(the session is fine, every chain reads)");
}
// built by C Y B with C.O.A

/// E1 stage 3 fix 3 (the user's run of 2026-10-06 22:18: "casting Maddening Dance disconnected the user"; the client: "NetGuidMismatch: Outer mismatch.
/// Path: CollisionCylinder, Outer: 16790986, Expected: 16791004, NetGUID: 16791010"). The draft named an NPC's collision cylinder as the actor's GUID
/// + 24, OUTSIDE the actor's block of nine GUIDs (the actor, its controller, the cylinder's slot, six components): that GUID is the next-minted
/// object's. A Wolf respawned (16790986 .. 16791002), the Lament's zone actor was minted right after it (16791004: its LingeringEffectMovementComp
/// 16791010 = the Wolf's + 24), the zone opened, and the first hit on that Wolf exported 16791010 as its "CollisionCylinder": the client held it as
/// the zone's component and dropped the connection 12 ms after the kill (22:18:53.169 / .181). For the area's NPCs the same GUID is the next
/// NPC's AlignmentComponent. Retail mints the cylinder INSIDE the block, the AlignmentComponent's - 2 (211 of 211 captured exports): so does worldd
/// now (NpcInfo::collision_cylinder, the actor's + 4). The rule the client enforces: a GUID exported with a path is never exported again under
/// another path or outer.
static void test_e1_cylinder_in_own_block() {
    std::printf("E1 stage 3 fix 3: the Wolf's cylinder GUID is inside its own block: a hit never exports a GUID another actor's open exported\n");
    const svc::Character rec = bard25();
    WolfWorld ww;
    ww.extra = 1;
    Run r(0, true, &rec, [&](PlayerWorld& w) { ww.prepare(w); });
    CHECK(ww.ground && ww.wolf != 0 && ww.wolves.size() == 1 && r.h.fx.world->combat() != nullptr, "a world with ground, two Wolves minted back to back next to the player");
    if (!ww.ground || ww.wolf == 0 || ww.wolves.size() != 1 || !r.h.fx.world->combat()) return;
    {
        const auto bytes = capture_blocks::bytes_of(capture_blocks::kClientReadyToPhaseIn.hex);
        BunchWriter phase;
        phase.write_bit_range(bytes.data(), 0, capture_blocks::kClientReadyToPhaseIn.bits);
        r.send_reliable(phase);
    }
    r.play(1.0, true);
    const bs::PlayerSetup setup = r.h.s->bootstrap().setup();
    const world::WorldActor* w1 = r.h.fx.world->registry().find(ww.wolf);
    const world::WorldActor* w2 = r.h.fx.world->registry().find(ww.wolves[0]);
    CHECK(w1 && w2, "(both Wolves are in the registry)");
    if (!w1 || !w2) return;
    const uint64_t block = 2 * (3 + w1->actor.subobjects.size());
    CHECK(w2->id == w1->id + block, "the second Wolf's block follows the first's (the actor, its controller, its cylinder, its components)");
    const emit::FIntrepidNetworkGUID cylinder = cylinder_guid_of(*w1);
    CHECK(cylinder.ObjectId == w1->id + 4 && cylinder.ObjectId > w1->id && cylinder.ObjectId < w2->id, "the first Wolf's cylinder GUID is inside its own block (the actor's + 4)");
    CHECK(r.h.fx.world->combat()->actor_of_object(cylinder.ObjectId) == w1->id && r.h.fx.world->combat()->actor_of_object(w2->id + 6) != w1->id &&
              r.h.fx.world->combat()->actor_of_object(w1->id + 24) != w1->id,
          "the combat world maps the cylinder's GUID to the first Wolf, and the second Wolf's AlignmentComponent (the first's + 24) never to the first (no arithmetic on ids)");
    // the client's cache after the opens: the second Wolf's components are in it
    GuidCache cache;
    for (const fake::Delivered& d : r.h.got) cache.take(d);
    const auto w2_alignment = cache.find_path("AlignmentComponent", w2->id);
    CHECK(w2_alignment != 0 && w2_alignment == w2->id + 6, "the second Wolf's open exported its AlignmentComponent (the actor's + 6) to the client");
    CHECK(w2_alignment == w1->id + 24, "(the draft's cylinder GUID, the first Wolf's + 24, IS that component: the user's collision)");
    // the first hit on the first Wolf, its cylinder named in place, acknowledged nothing until the swing is over
    const abilities::AbilityDef* axe = r.h.fx.world->ability_catalog()->ability_named("WeaponCombo_Melee_1H_Axe");
    CHECK(axe != nullptr && !axe->hits.empty(), "the 1H axe combo is in the tables");
    if (!axe || axe->hits.empty()) return;
    int64_t targeting = 0;
    for (const content::TargetingRow& t : axe->targetings) {
        if (t.key == axe->hits[0].key) targeting = t.guid;
    }
    r.h.s->teach_ability(axe->row.guid);
    r.h.fx.world->tick_world(r.now());
    r.h.hold = true;
    const size_t first = r.h.got.size();
    const repl::InlineObject cyl = inline_cylinder_of(*w1);
    const int32_t id = r.next_id++;
    const BunchWriter trig = trigger_at_value(setup, axe->row.guid, id, repl::Value(cyl));
    r.send_trigger(axe->row.guid, id, &trig);
    r.play(0.6, false);
    repl::HitComponentRef comp;
    comp.inline_object = cyl;
    r.send_reliable(hit_request_with(setup, axe->row.guid, targeting, axe->hits[0].hit_guid, comp));
    for (int i = 0; i < 5; ++i) {
        r.play(0.1, false);
        r.h.fx.world->tick_world(r.now());
    }
    r.h.release();
    r.absorb();
    bool confirmed = false;
    for (const repl::HitInstance& h : r.m.hits) {
        for (const repl::HitTarget& t : h.targets) confirmed = confirmed || t.hit_comp == w1->actor.guid;
    }
    CHECK(confirmed, "the hit is confirmed on the first Wolf");
    // what the swing exported, read as the client reads it
    uint64_t exported_cylinder = 0, exported_outer = 0;
    size_t cylinder_exports = 0;
    for (size_t i = first; i < r.h.got.size(); ++i) {
        for (const ExportRecord& e : export_records(r.h.got[i])) {
            if ((e.flags & 1) && e.path == "CollisionCylinder") {
                ++cylinder_exports;
                exported_cylinder = e.guid.ObjectId;
                exported_outer = e.outer.ObjectId;
            }
        }
        cache.take(r.h.got[i]);
    }
    CHECK(cylinder_exports >= 1 && exported_cylinder == cylinder.ObjectId && exported_outer == w1->id,
          "the item's and the confirmation's chains export the first Wolf's cylinder, its GUID the actor's + 4, the actor as the outer");
    for (const std::string& s : cache.described) std::printf("  (the client would refuse: %s)\n", s.c_str());
    CHECK(cache.mismatches == 0, "no GUID is exported under a second path or outer: nothing the client refuses (NetGuidMismatch)");
    CHECK(!r.h.s->abilities_failed() && r.m.unreadable == 0, "(the session is fine, every chain reads)");
}

/// E1 stage 3 fix 3: every dance and lingering-zone ability of every class, cast with a Wolf minted for each, its zone actor opened and the Wolf hit while
/// it stands: nothing any of them sends is a GUID the client holds under another path or outer (the fault of test_e1_cylinder_in_own_block, for
/// every zone actor's block), every chain reads, and every Bard dance opens its zone.
static void test_e1_zone_abilities_export_rule() {
    std::printf("E1 stage 3 fix 3: every class's dances and lingering-zone abilities, each with a Wolf hit while its zone stands: nothing the client refuses\n");
    struct Class { int id; const char* name; int64_t archetype; };
    const Class classes[] = {{1, "Ranger", 17751}, {2, "Bard", 17747}, {3, "Cleric", 17748}, {4, "Fighter", 17749},
                             {5, "Rogue", 17752}, {6, "Summoner", 17753}, {7, "Tank", 17754}, {8, "Mage", 17750}};
    size_t total = 0, zones_opened = 0, refused = 0, bard_dances = 0, bard_zones = 0;
    constexpr int kSpareWolves = 8;
    for (const Class& cls : classes) {
        const svc::Character rec = class25(cls.id, cls.name, cls.archetype);
        WolfWorld ww;
        ww.extra = kSpareWolves;
        Run r(0, true, &rec, [&](PlayerWorld& w) { ww.prepare(w); });
        CHECKF(ww.ground && ww.wolf != 0 && ww.wolves.size() == kSpareWolves && r.h.fx.world->combat() != nullptr, "%s: a world with a Wolf for each zone ability", cls.name);
        if (!ww.ground || ww.wolf == 0 || ww.wolves.size() != kSpareWolves || !r.h.fx.world->combat()) continue;
        // the class's abilities that spawn a lingering zone themselves or through a hit (the tooltip table lists every ability of every class)
        std::vector<const abilities::AbilityDef*> defs;
        {
            const abilities::Catalog* cat = r.h.fx.world->ability_catalog();
            for (const tooltip_key::Key& k : tooltip_key::kKeys) {
                if (std::strcmp(k.group, cls.name) != 0 || !cat) continue;
                const abilities::AbilityDef* def = cat->ability(k.guid);
                if (!def) continue;
                bool zone = !def->zones.empty();
                for (const content::AbilityHitRow& h : def->hits) {
                    const abilities::HitDef* hit = cat->hit(h.hit_guid);
                    zone = zone || (hit && !hit->zones.empty());
                }
                if (zone && defs.size() < static_cast<size_t>(kSpareWolves)) defs.push_back(def);
            }
        }
        if (defs.empty()) continue;
        {
            const auto bytes = capture_blocks::bytes_of(capture_blocks::kClientReadyToPhaseIn.hex);
            BunchWriter phase;
            phase.write_bit_range(bytes.data(), 0, capture_blocks::kClientReadyToPhaseIn.bits);
            r.send_reliable(phase);
        }
        r.play(1.0, true);
        const bs::PlayerSetup setup = r.h.s->bootstrap().setup();
        const abilities::AbilityDef* axe = r.h.fx.world->ability_catalog()->ability_named("WeaponCombo_Melee_1H_Axe");
        if (!axe || axe->hits.empty()) {
            CHECK(false, "the 1H axe combo is in the tables");
            continue;
        }
        int64_t targeting = 0;
        for (const content::TargetingRow& t : axe->targetings) {
            if (t.key == axe->hits[0].key) targeting = t.guid;
        }
        r.h.s->teach_ability(axe->row.guid);
        std::vector<uint64_t> targets = ww.wolves;
        for (size_t i = 0; i < defs.size(); ++i) {
            const abilities::AbilityDef* def = defs[i];
            ++total;
            const bool dance = def->row.name.find("_Dance_") != std::string::npos && cls.id == 2;
            if (dance) ++bard_dances;
            r.h.s->teach_ability(def->row.guid);
            r.h.fx.world->tick_world(r.now());
            const size_t others_before = r.m.other.size();
            const int32_t id = r.trigger(def->row.guid);
            r.play(0.8, false);
            r.h.fx.world->tick_world(r.now());
            size_t opened = 0;
            for (size_t k = others_before; k < r.m.other.size(); ++k) opened += r.m.other[k].open ? 1 : 0;
            const std::vector<int32_t> ends = r.m.end_ids();
            const bool ended = std::find(ends.begin(), ends.end(), id) != ends.end();
            if (ended) ++refused;
            if (opened > 0) ++zones_opened;
            if (dance) {
                bard_zones += opened > 0 ? 1 : 0;
                CHECKF(opened > 0 && !ended, "Bard: %s opens its zone's actor (%zu opened, %s)", def->row.name.c_str(), opened, ended ? "ended" : "accepted");
            }
            // the Wolf of this ability, hit while the zone stands: its cylinder named in place, then exported
            const world::WorldActor* wa = r.h.fx.world->registry().find(targets[i]);
            if (!wa) continue;
            const repl::InlineObject cyl = inline_cylinder_of(*wa);
            const int32_t swing = r.next_id++;
            const BunchWriter trig = trigger_at_value(setup, axe->row.guid, swing, repl::Value(cyl));
            r.send_trigger(axe->row.guid, swing, &trig);
            r.play(0.5, false);
            repl::HitComponentRef comp;
            comp.inline_object = cyl;
            r.send_reliable(hit_request_with(setup, axe->row.guid, targeting, axe->hits[0].hit_guid, comp));
            for (int k = 0; k < 6; ++k) {
                r.play(0.1, false);
                r.h.fx.world->tick_world(r.now());
            }
        }
        r.play(6.0, false);   // the zones end and close
        r.absorb();
        GuidCache cache;
        for (const fake::Delivered& d : r.h.got) cache.take(d);
        for (const std::string& s : cache.described) std::printf("  (%s: the client would refuse: %s)\n", cls.name, s.c_str());
        CHECKF(cache.mismatches == 0, "%s: no GUID of any chain (the zones' actors, the Wolves' opens, the items, the confirmations) is exported under a second path or outer", cls.name);
        CHECKF(!r.h.s->abilities_failed() && r.m.unreadable == 0, "%s: the session is fine, every chain reads", cls.name);
        std::printf("  (%s: %zu zone abilities cast)\n", cls.name, defs.size());
    }
    std::printf("  (%zu zone abilities of every class: %zu opened a zone actor, %zu were ended by the server; the Bard's dances: %zu of %zu opened)\n", total, zones_opened, refused, bard_zones, bard_dances);
    CHECK(total >= 8 && bard_dances >= 4 && bard_zones == bard_dances, "every Bard dance opened its zone; the other classes' zone abilities were cast");
}

static void test_e1_combat_session() {
    std::printf("E1 stage 3: a player's fight with a Wolf through the session\n");
    const svc::Character rec = bard25();
    uint64_t wolf = 0;
    uint64_t next_actor_id = 5000000;
    bool ground = false;
    auto prepare = [&](PlayerWorld& w) {
        std::string err;
        if (!w.load_blueprints(NPC_MOTION_PATH, err)) {
            std::printf("  [FAIL] the Blueprint table: %s\n", err.c_str());
            return;
        }
        auto nav = nav::AreaNav::from_file(navfix::build_file(), &err);
        ground = nav && w.adopt_ground(std::move(nav), err);
        if (!ground) {
            std::printf("  [FAIL] the ground: %s\n", err.c_str());
            return;
        }
        auto captured = spawns::CapturedData::load(err);
        if (!captured) {
            std::printf("  [FAIL] the captured data: %s\n", err.c_str());
            return;
        }
        std::string path;
        for (const auto& [p, row] : w.blueprints().rows()) {
            if (p.find("/Beast/Canine/Wolf.Default__Wolf_C") != std::string::npos) path = p;
        }
        wolf = combat_rig::spawn_npc(w.registry(), w.npcs(), *captured, next_actor_id, path, kX + 120, kY, kZ, 8, 800);
    };
    Run r(0, true, &rec, prepare);
    CHECK(ground && wolf != 0 && r.h.fx.world->combat() != nullptr, "a world with ground, a Wolf next to the player (off the ground fixture: it stands) and its fights");
    if (!ground || wolf == 0 || !r.h.fx.world->combat()) return;
    combat::CombatWorld& cw = *r.h.fx.world->combat();
    r.play(0.5, false);
    const bs::PlayerSetup setup = r.h.s->bootstrap().setup();
    CHECK(r.h.s->character_stats() && !r.h.s->character_stats()->in_combat(), "the player is not in combat to begin with");

    const abilities::AbilityDef* axe = r.h.fx.world->ability_catalog()->ability_named("WeaponCombo_Melee_1H_Axe");
    CHECK(axe != nullptr && !axe->hits.empty(), "the 1H axe combo is in the tables");
    if (!axe || axe->hits.empty()) return;
    const int64_t ability = axe->row.guid;
    const int64_t hit = axe->hits[0].hit_guid;
    int64_t targeting = 0;
    for (const content::TargetingRow& t : axe->targetings) {
        if (t.key == axe->hits[0].key) targeting = t.guid;
    }
    r.h.s->teach_ability(ability);
    emit::FIntrepidNetworkGUID cylinder;
    cylinder.ObjectId = wolf + 4;   // the registry's slot for the cylinder (E1 stage 3 fix 3: inside the actor's block, retail's AlignmentComponent - 2)
    cylinder.ServerId = kServerId;
    cylinder.Randomizer = kRandomizer;
// ~ C-O-A :: C:Y:B ~

    r.h.fx.world->tick_world(r.now());
    const double max_hp = cw.max_health_of(wolf);
    const int32_t id = r.next_id++;
    const BunchWriter trig = trigger_at_object(setup, ability, id, cylinder);
    r.send_trigger(ability, id, &trig);
    r.play(0.6, false);
    const std::vector<int32_t> ends = r.m.end_ids();
    CHECK(r.m.has_ability(ability) && std::find(ends.begin(), ends.end(), id) == ends.end(), "the cast at the Wolf is accepted: its ability item is there, no ClientEndAbility");
    const BunchWriter req = hit_request_at_object(setup, ability, targeting, hit, cylinder);
    r.send_reliable(req);
    for (int i = 0; i < 5; ++i) {
        r.play(0.1, false);
        r.h.fx.world->tick_world(r.now());
    }
    const repl::HitInstance* conf = nullptr;
    for (const repl::HitInstance& h : r.m.hits) {
        if (!h.targets.empty()) conf = &h;
    }
    CHECK(conf != nullptr, "the hit is confirmed to the client with a target");
    const double hp = cw.health_of(wolf);
    const bool missed = conf && (conf->targets[0].flags & stats::kHitMiss);
    std::printf("  (the Wolf: %.1f of %.1f Health)\n", hp, max_hp);
    CHECK(missed || hp < max_hp, "the Wolf lost Health (or the hit missed)");

    // it comes for the player and bites; the player's in-combat flag follows
    double bitten_at = -1;
    const float full = r.m.stats[content::kGuidMaxHealth];
    for (int i = 0; i < 80 && bitten_at < 0; ++i) {
        r.play(0.1, false);
        r.h.fx.world->tick_world(r.now());
        if (r.m.stats[content::kGuidHealth] < full) bitten_at = r.now();
    }
    CHECK(bitten_at >= 0, "the Wolf, hurt by the player, goes for it and its bite takes the player's Health");
    CHECK(r.h.s->character_stats() && r.h.s->character_stats()->in_combat(), "the player is in combat");
    const auto ai = cw.ai_state(wolf);
    CHECK(ai.has_value() && *ai != npc::ai::State::Idle, "the Wolf's AI is fighting (Chase or Attack)");
    CHECK(!r.h.s->abilities_failed() && r.m.unreadable == 0, "(the session is fine, every chain reads)");

    // six seconds without an exchange: the flag ends (the Wolf is killed first so none comes)
    combat::Resolved kill;
    kill.mods.push_back({1, content::kGuidHealth, -1.0e9});
    cw.apply_hit_to_npc(wolf, 1, kill, r.now());
    for (int i = 0; i < 90; ++i) {
        r.play(0.1, false);
        r.h.fx.world->tick_world(r.now());
    }
    CHECK(r.h.s->character_stats() && !r.h.s->character_stats()->in_combat(), "with the Wolf dead and no exchange for six seconds the player is out of combat");
}


// ============================================================================================================================
//  plan E1, stage 3 (part 2): the aftermath through the session: a whole fight (aggro, the hits, the kill, the experience, the loot window, the items
//  into the bag, the corpse closing), the player's own death and respawn, and the hostile requests of each
// ============================================================================================================================

static constexpr int64_t kCopper = 109162169652920ll;       // Resource_Raw_Metal_Copper (Item.Resource.Raw.Metal, stacks of 20)
static constexpr int64_t kOak = 109162169652924ll;          // Resource_Raw_Wood_Oak
static constexpr int64_t kGlintT1 = 6064628753953390592ll;  // Certificate_T1_Glint (stacks of 10, tagged item.resource.cert)
static constexpr int64_t kMount = 107411158881912ll;        // Mount_Flying_Wyrmling_0 (no resource tag)

static BunchWriter actor_bunch(const schema::ClassSchema& cls, const char* name, const repl::RpcParams& params) {
    BunchWriter out;
    const schema::RpcSchema* rpc = cls.rpc(name);
    if (!rpc) {
        std::printf("  [FAIL] no RPC %s\n", name);
        return out;
    }
    BunchWriter field;
    bool failed = false;
    if ((repl::write_rpc_field(cls, *rpc, params, field, client_options(), &failed) == 0 && !params.values.empty()) || failed) {
        std::printf("  [FAIL] %s does not write\n", name);
        return out;
    }
    repl::BlockHeader header;
    repl::write_content_block(out, header, field, /*server_form=*/false);
    return out;
}

static BunchWriter interact_bunch(uint64_t component) {
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    const schema::RpcSchema* rpc = pawn.rpc("ServerInteractWithInteractableComponent");
    repl::InteractRequest q;
    q.interactable.ObjectId = component;
    q.interactable.ServerId = kServerId;
    q.interactable.Randomizer = kRandomizer;
    q.counter = 1;
    const auto params = rpc ? repl::interact_params(*rpc, q) : std::nullopt;
    return params ? actor_bunch(pawn, "ServerInteractWithInteractableComponent", *params) : BunchWriter();
}

/// ServerRequestLootStorageEvents(ItemEvents).
static BunchWriter loot_bunch(const std::vector<repl::ItemStorageEvent>& events) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerRequestLootStorageEvents");
    repl::RpcParams p;
    repl::ArrayValue arr;
    for (const repl::ItemStorageEvent& e : events) {
        repl::ElementState es;
        es.values[1] = repl::Value(e);
        arr.elements.push_back(std::move(es));
    }
    if (!rpc || !repl::set_rpc_param(*rpc, p, "ItemEvents", repl::Value(std::move(arr)))) {
        std::printf("  [FAIL] the loot request's parameters do not fit\n");
        return BunchWriter();
    }
    return actor_bunch(pc, "ServerRequestLootStorageEvents", p);
}

/// What the retail client sends to take the stack in `slot` of the window it was shown: a type-3 event, the container's slot as the source, the bag as the target.
static repl::ItemStorageEvent take_event(const repl::LootPayload& w, size_t slot, const emit::FIntrepidNetworkGUID& container, const bs::PlayerSetup& setup) {
    repl::ItemStorageEvent e;
    e.type = static_cast<uint8_t>(repl::StorageEventType::Fill);
    e.move_source.object = container;
    e.item = w.data.storage[slot].item;
    e.move_source.slots.push_back({static_cast<int32_t>(slot), static_cast<int32_t>(e.item.quantity), e.item.guid});
    repl::StorageEventContainer bag;
    bag.object = setup.guids.backpack;
    e.targets.push_back(bag);
    return e;
}

static BunchWriter respawn_choices_bunch() { return actor_bunch(class_named("PlayerPawn_C"), "RequestServerRespawnChoices", {}); }

static BunchWriter respawn_choice_bunch(int64_t record) {
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    const schema::RpcSchema* rpc = pawn.rpc("SendServerRespawnChoice");
    repl::RpcParams p;
    if (!rpc || !repl::set_rpc_param(*rpc, p, "RespawnChoice", repl::Value::i64(record))) {
        std::printf("  [FAIL] the respawn choice's parameters do not fit\n");
        return BunchWriter();
    }
    return actor_bunch(pawn, "SendServerRespawnChoice", p);
}

static void send_on_pc(Run& r, const BunchWriter& b) {
    if (b.bit_pos() == 0) return;
    r.h.client_sends({r.h.client.actor(r.h.pc_ch, b, /*reliable=*/true)});
    r.h.flush();
    r.absorb();
}
// (Cyb + C | O | A)

static void send_on_pawn(Run& r, const BunchWriter& b) {
    if (b.bit_pos() == 0) return;
    r.send_reliable(b);
}

/// The client is ready to phase in: the session starts to open the world's actors (the Wolves) on it.
static void phase_in(Run& r) {
    send_on_pawn(r, actor_bunch(class_named("PlayerPawn_C"), "ServerClientIsReadyToPhaseIn", {}));
}

/// `seconds` of play with the world's clock following.
static void world_play(Run& r, double seconds) {
    const int n = static_cast<int>(std::llround(seconds * 10));
    for (int i = 0; i < n; ++i) {
        r.play(0.1, false);
        r.h.fx.world->tick_world(r.now());
        if (std::getenv("AOC_TEST_TRACE")) std::printf("    t=%.1f Health %.0f dead %d\n", r.now(), r.m.stats[content::kGuidHealth], r.h.s->dead() ? 1 : 0);
    }
}

/// `count` Wolves of `level` around the login spot (the first of level `first_level` -- a level-24 Wolf kills the level-25 Bard in about two seconds --
/// with the aggro radius `aggro_first`; the others see 1 cm: an aggro radius of 0 is the data's default, and a pack comes for the player at once) and one 30 m off;
/// their ids into `out`.
static void spawn_wolves(PlayerWorld& w, std::vector<uint64_t>& out, uint64_t& far_wolf, size_t count, int level, double aggro_first, bool& ground, uint64_t& next_actor_id, int first_level = 8) {
    std::string err;
    if (!w.load_blueprints(NPC_MOTION_PATH, err)) {
        std::printf("  [FAIL] the Blueprint table: %s\n", err.c_str());
        return;
    }
    auto nav = nav::AreaNav::from_file(navfix::build_file(), &err);
    ground = nav && w.adopt_ground(std::move(nav), err);
    if (!ground) {
        std::printf("  [FAIL] the ground: %s\n", err.c_str());
        return;
    }
    auto captured = spawns::CapturedData::load(err);
    if (!captured) {
        std::printf("  [FAIL] the captured data: %s\n", err.c_str());
        return;
    }
    std::string path;
    for (const auto& [p, row] : w.blueprints().rows()) {
        if (p.find("/Beast/Canine/Wolf.Default__Wolf_C") != std::string::npos) path = p;
    }
    for (size_t i = 0; i < count; ++i) {
        const double x = kX + 120 + static_cast<double>(i % 7) * 70.0;
        const double y = kY + (i == 0 ? 0.0 : (i < 7 ? -220.0 : 220.0));
        const uint64_t id = combat_rig::spawn_npc(w.registry(), w.npcs(), *captured, next_actor_id, path, x, y, kZ, i == 0 ? first_level : level, i == 0 ? aggro_first : 1.0);
        if (id) out.push_back(id);
    }
    far_wolf = combat_rig::spawn_npc(w.registry(), w.npcs(), *captured, next_actor_id, path, kX + 3000, kY, kZ, level, 1.0);
}

static void kill_npc(Run& r, uint64_t npc, uint64_t by) {
    combat::Resolved kill;
    kill.mods.push_back({1, content::kGuidHealth, -1.0e9});
    r.h.fx.world->combat()->apply_hit_to_npc(npc, by, kill, r.now());
}

static std::string guid_hex_n(uint32_t n) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%08x%08x%08x%08x", 0x5000u + n, 0x11u, 0x22u, 0x33u);
    return buf;
}

static svc::InventoryItem stack_of(int64_t record, int32_t slot, int32_t quantity, uint32_t serial, int32_t quality = 1) {
    svc::InventoryItem it;
    it.container = svc::ItemContainer::Backpack;
    it.slot = slot;
    it.item_record_id = record;
    it.instance_guid = guid_hex_n(serial);
    it.quantity = quantity;
    it.quality = quality;
    it.durability = 0;
    it.created_at_ms = 1700000000000ll;
    return it;
}

static int64_t held_of(const PlayerSession& s, int64_t record) {
    int64_t n = 0;
    for (const svc::InventoryItem& it : s.inventory().items()) {
        if (it.item_record_id == record) n += it.quantity;
    }
    return n;
}

static const char* kAftermathNote = "(plan E1, stage 3, part 2)";

static void test_e1_whole_fight() {
    std::printf("E1 stage 3, part 2: a whole fight through the session (aggro, hits, kill, experience, loot, corpse) %s\n", kAftermathNote);
    const svc::Character rec = bard25();
    std::vector<uint64_t> wolves;
    uint64_t far_wolf = 0, next_actor_id = 5000000;
    bool ground = false;
    auto prepare = [&](PlayerWorld& w) { spawn_wolves(w, wolves, far_wolf, 14, 24, 800, ground, next_actor_id); };
    Run r(0, true, &rec, prepare);
    CHECK(ground && wolves.size() == 14 && far_wolf != 0 && r.h.fx.world->combat() && r.h.fx.world->aftermath(), "a world with ground, fourteen Wolves of level 24 around the player (the first one aggressive), one 30 m away, and the aftermath");
    if (!ground || wolves.size() != 14 || !r.h.fx.world->aftermath()) return;
    combat::CombatWorld& cw = *r.h.fx.world->combat();
    combat::Aftermath& after = *r.h.fx.world->aftermath();
    world::ActorRegistry& reg = r.h.fx.world->registry();
    r.play(0.5, false);
    phase_in(r);
    world_play(r, 1.0);
    const bs::PlayerSetup setup = r.h.s->bootstrap().setup();
    const uint64_t pawn = setup.guids.pawn.ObjectId;

    // ---- the fight: a hit of the player's, the Wolf's bite, then the killing blow
    const abilities::AbilityDef* axe = r.h.fx.world->ability_catalog()->ability_named("WeaponCombo_Melee_1H_Axe");
    CHECK(axe != nullptr && !axe->hits.empty(), "the 1H axe combo is in the tables");
    if (!axe || axe->hits.empty()) return;
    const int64_t ability = axe->row.guid;
    const int64_t hit = axe->hits[0].hit_guid;
    int64_t targeting = 0;
    for (const content::TargetingRow& t : axe->targetings) {
        if (t.key == axe->hits[0].key) targeting = t.guid;
    }
    r.h.s->teach_ability(ability);
    const auto cylinder_of = [&](uint64_t npc) {
        emit::FIntrepidNetworkGUID g;
        g.ObjectId = npc + 4;   // the registry's slot for the cylinder (E1 stage 3 fix 3)
        g.ServerId = kServerId;
        g.Randomizer = kRandomizer;
        return g;
    };
    r.h.fx.world->tick_world(r.now());
    const int32_t id = r.next_id++;
    const BunchWriter trig = trigger_at_object(setup, ability, id, cylinder_of(wolves[0]));
    r.send_trigger(ability, id, &trig);
    r.play(0.6, false);
    r.send_reliable(hit_request_at_object(setup, ability, targeting, hit, cylinder_of(wolves[0])));
    world_play(r, 0.5);
    const double hp0 = cw.health_of(wolves[0]);
    double bitten_at = -1;
    const float full = r.m.stats[content::kGuidMaxHealth];
    for (int i = 0; i < 80 && bitten_at < 0; ++i) {
        world_play(r, 0.1);
        if (r.m.stats[content::kGuidHealth] < full) bitten_at = r.now();
    }
    CHECK(bitten_at >= 0 && hp0 < cw.max_health_of(wolves[0]) + 1, "the Wolf, hurt by the player, comes for it and bites");
    const int64_t xp_before = r.h.s->progress().xp;
    const int level_before = r.h.s->progress().level;
    const int64_t debt_before = r.h.s->progress().debt;
    CHECK(r.h.s->kills_rewarded() == 0 && after.stats().deaths == 0, "nothing has died yet");
// C O A : C:Y:B

    // ---- the kills: the first Wolf (the one that fought) and thirteen more
    kill_npc(r, wolves[0], pawn);
    world_play(r, 0.5);
    CHECK(r.h.s->kills_rewarded() == 1, "the killing blow: the session was given the kill's experience once");
    CHECK(r.h.s->progress().xp > xp_before || r.h.s->progress().level > level_before || r.h.s->progress().debt < debt_before,
          "the character's Adventuring experience grew (or paid down its debt: D1's rule)");
    CHECK(r.h.s->progress().debt <= debt_before, "(no debt from a kill)");
    CHECK(r.m.xp_updates >= 1, "the XPTracker was updated to the client");
    const combat::Corpse* c0 = after.corpse_of(wolves[0]);
    CHECK(c0 != nullptr && c0->rights.count(pawn) == 1, "the Wolf is a corpse and the player has the right to its loot");
    for (size_t i = 1; i < wolves.size(); ++i) kill_npc(r, wolves[i], pawn);
    world_play(r, 0.5);
    CHECK(r.h.s->kills_rewarded() == wolves.size() && after.stats().deaths == wolves.size(), "fourteen kills, fourteen rewards");
    uint64_t looted = 0;
    size_t with_loot = 0;
    for (const uint64_t w : wolves) {
        const combat::Corpse* c = after.corpse_of(w);
        if (c && c->parts) {
            ++with_loot;
            if (!looted) looted = w;
        }
    }
    std::printf("  (%zu of the fourteen corpses have loot)\n", with_loot);
    CHECK(looted != 0, "some corpse has loot (the Wolves' tables give Glint about 38 times in a hundred)");
    CHECK(with_loot < wolves.size(), "and some have none");
    CHECK(r.m.unreadable == 0 && !r.h.s->abilities_failed(), "(every chain reads, the session is fine)");
    if (!looted) return;
    const combat::Corpse* corpse = after.corpse_of(looted);
    const world::WorldActor* actor = reg.find(looted);
    CHECK(corpse && actor && corpse->parts && corpse->remaining() == corpse->loot.size() && !corpse->loot.empty(), "the loot corpse holds its stacks");
    if (!corpse || !actor || !corpse->parts) return;
    size_t big_updates = 0;
    for (const Mirror::ChannelEvent& e : r.m.other) big_updates += (!e.open && !e.close && e.bits > 1000) ? 1 : 0;
    CHECK(big_updates >= with_loot, "each corpse with loot sent its three new components to the client (updates of the NPCs' channels, over 1,000 bits)");
    const double t_died = after.corpse_of(looted)->died_at;
    const uint64_t container_id = corpse->parts->container;
    const emit::FIntrepidNetworkGUID container_guid = corpse->parts->container_guid;
    const uint64_t interactable = corpse->parts->interactable;
    const std::vector<combat::LootStack> expect = corpse->loot;

    // ---- hostile requests before the window is open
    const size_t refused0 = r.h.s->loot_refused();
    send_on_pawn(r, interact_bunch(0xdeadbeefull));
    world_play(r, 0.2);
    CHECK(r.h.s->loot_opens() == 0 && r.m.loot_windows.empty(), "an interaction with a component that is none opens nothing");
    {
        repl::LootPayload fake;
        fake.data.storage.resize(1);
        fake.data.storage[0].item.has_item = true;
        fake.data.storage[0].item.record_id = static_cast<uint64_t>(kGlintT1);
        fake.data.storage[0].item.quantity = 3;
        send_on_pc(r, loot_bunch({take_event(fake, 0, container_guid, setup)}));
        world_play(r, 0.2);
    }
    CHECK(r.h.s->loot_refused() == refused0 + 1 && r.h.s->loot_taken() == 0 && r.m.storage_events.empty(), "a take with no window open is refused, nothing leaves the corpse");

    // ---- the window
    send_on_pawn(r, interact_bunch(interactable));
    world_play(r, 0.3);
    CHECK(r.h.s->loot_opens() == 1 && r.m.loot_windows.size() == 1 && r.h.s->loot_actor() == looted, "interacting with the corpse's InteractableComponent opens the loot window");
    if (r.m.loot_windows.empty()) return;
    const repl::LootPayload win = r.m.loot_windows.back();
    CHECK(win.actor == actor->actor.guid, "the window names the corpse's actor (LootActor)");
    CHECK(win.data.storage.size() == expect.size() && win.data.client_retrieve_only, "one slot per stack, retrieve-only (retail: bClientRetrieveOnly 1)");
    bool shapes = win.data.storage.size() == expect.size();
    for (size_t i = 0; shapes && i < expect.size(); ++i) {
        const repl::StorageSlot& s = win.data.storage[i];
        shapes = s.item.has_item && s.item.record_id == static_cast<uint64_t>(expect[i].item) && s.item.quantity == static_cast<uint32_t>(expect[i].quantity) &&
                 s.item.quality == static_cast<uint32_t>(expect[i].quality) && s.slot == static_cast<int32_t>(i) && s.check_loot_permission && s.loot_permission.size() == 1;
    }
    CHECK(shapes, "each slot is the rolled item, its quantity and quality, with the loot permission of the looting player (retail: bCheckItemLootPermission 1)");
    CHECK(win.allocation.default_method == 1 && win.allocation.by_rarity[0] == 0, "LootAllocationMethods: Default 1, the rest 0 (retail's)");

    // ---- hostile requests with the window open: every one is refused and nothing leaves the corpse
    const auto refused_by = [&](const std::vector<repl::ItemStorageEvent>& evs, const char* what) {
        const size_t before = r.h.s->loot_refused();
        send_on_pc(r, loot_bunch(evs));
        world_play(r, 0.2);
        const bool ok = r.h.s->loot_refused() == before + 1 && r.h.s->loot_taken() == 0 && r.m.storage_events.empty() && after.corpse_of(looted)->remaining() == expect.size();
        CHECK(ok, what);
    };
    {
        auto wrong_container = take_event(win, 0, container_guid, setup);
        wrong_container.move_source.object.ObjectId += 2;
        refused_by({wrong_container}, "an event that names another container is refused");
        auto bad_slot = take_event(win, 0, container_guid, setup);
        bad_slot.move_source.slots[0].slot = 99;
        refused_by({bad_slot}, "an event that names a slot that is none is refused");
        auto bad_item = take_event(win, 0, container_guid, setup);
        bad_item.item.record_id += 1;
        refused_by({bad_item}, "an event whose item is not the stack of its slot is refused");
        auto bad_guid = take_event(win, 0, container_guid, setup);
        bad_guid.item.guid.a ^= 0xffu;
        refused_by({bad_guid}, "an event whose instance guid is forged is refused");
        refused_by({take_event(win, 0, container_guid, setup), take_event(win, 0, container_guid, setup)}, "the same slot twice in one request is refused");
        repl::ItemStorageEvent light;   // (no item: a bunch of seventeen whole events does not fit the connection's bunch size at all)
        light.type = static_cast<uint8_t>(repl::StorageEventType::Fill);
        light.move_source.object = container_guid;
        std::vector<repl::ItemStorageEvent> many(17, light);
        refused_by(many, "seventeen events in one request are refused");
        refused_by({}, "a request with no events is refused");
    }
    // spam: forty interactions and forty takes within one tick come to the budget (eight, then ten a second)
    {
        const size_t opens = r.h.s->loot_opens();
        for (int i = 0; i < 40; ++i) send_on_pawn(r, interact_bunch(interactable));
        CHECK(r.h.s->loot_opens() - opens <= PlayerSession::kLootBurst + 2, "forty interactions at once open the window at most the burst allows");
        auto bad = take_event(win, 0, container_guid, setup);
        bad.item.record_id += 1;
        const size_t refused_before = r.h.s->loot_refused();
        for (int i = 0; i < 40; ++i) send_on_pc(r, loot_bunch({bad}));
        CHECK(r.h.s->loot_refused() - refused_before <= PlayerSession::kLootBurst + 2 && r.h.s->loot_taken() == 0 && !r.h.s->abilities_failed(), "forty forged takes at once: the budget drops the rest, nothing is taken");
        world_play(r, 1.5);
        CHECK(r.h.s->loot_actor() == looted, "(the window is still the corpse's)");
    }
// [C Y B :: CoA]

    // ---- the real take: every stack
    std::vector<repl::ItemStorageEvent> all;
    const repl::LootPayload win2 = r.m.loot_windows.back();
    for (size_t i = 0; i < expect.size(); ++i) all.push_back(take_event(win2, i, container_guid, setup));
    std::map<int64_t, int64_t> held_before;
    for (const combat::LootStack& s : expect) held_before[s.item] = held_of(*r.h.s, s.item);
    const size_t events_before = r.m.storage_events.size();
    const size_t calls_before = r.m.pc_calls.size();
    const double t_take = r.now();
    send_on_pc(r, loot_bunch(all));
    world_play(r, 0.3);
    CHECK(r.h.s->loot_taken() == expect.size() && after.corpse_of(looted) && after.corpse_of(looted)->remaining() == 0, "taking every slot empties the corpse");
    bool grew = true;
    std::map<int64_t, int64_t> want;
    for (const combat::LootStack& s : expect) want[s.item] += s.quantity;
    for (const auto& [rec_id, n] : want) grew = grew && held_of(*r.h.s, rec_id) == held_before[rec_id] + n;
    CHECK(grew, "the bag holds what the corpse did (every stack's quantity added)");
    int removes = 0, adds = 0;
    size_t last_remove = 0, first_add = ~size_t{0};
    for (size_t i = events_before; i < r.m.storage_events.size(); ++i) {
        const repl::ItemStorageEvent& e = r.m.storage_events[i];
        // [R] the removal's source is the empty container, its target the loot container with the slot
        if (e.type == static_cast<uint8_t>(repl::StorageEventType::Remove) && !e.targets.empty() && e.targets[0].object.ObjectId == container_id) {
            removes += static_cast<int>(e.targets[0].slots.size());
            last_remove = i;
        } else if (e.type == static_cast<uint8_t>(repl::StorageEventType::Add)) {
            ++adds;
            if (first_add == ~size_t{0}) first_add = i;
        }
    }
    std::printf("  (%zu storage events: %d slots removed from the container, %d adds)\n", r.m.storage_events.size() - events_before, removes, adds);
    CHECK(removes == static_cast<int>(expect.size()) && adds >= 1, "the client was told: the stacks removed from the loot container, then added to the bag [retail's order]");
    CHECK(first_add != ~size_t{0} && first_add > last_remove, "(the removals come before the adds)");
    int listener_at = -1, last_event_at = -1;
    for (size_t i = calls_before; i < r.m.pc_calls.size(); ++i) {
        if (r.m.pc_calls[i] == "ClientRemoveListenerFromStorage") listener_at = static_cast<int>(i);
        if (r.m.pc_calls[i] == "ClientReceiveStorageEvent") last_event_at = static_cast<int>(i);
    }
    CHECK(r.m.listeners_removed == 1 && listener_at > last_event_at && r.h.s->loot_actor() == 0, "the window is closed with ClientRemoveListenerFromStorage after the last event, once");
    // a second take of the same stacks: nothing left
    const size_t refused1 = r.h.s->loot_refused();
    send_on_pc(r, loot_bunch(all));
    world_play(r, 0.2);
    CHECK(r.h.s->loot_taken() == expect.size() && r.h.s->loot_refused() == refused1 + 1 && grew, "the same request again: refused (the window is gone), no second helping");
    // the corpse closes 4 s after it was emptied [retail: 3.97 to 4.08 s]
    double closed_at = -1;
    for (int i = 0; i < 70 && closed_at < 0; ++i) {
        world_play(r, 0.1);
        if (!reg.find(looted)) closed_at = r.now() - t_take;
    }
    CHECK(closed_at >= 3.8 && closed_at <= 4.6, "the emptied corpse is closed about 4 s later [R: 3.97 to 4.08 s]");
    world_play(r, 0.5);
    bool closed_to_client = false;
    for (const Mirror::ChannelEvent& e : r.m.other) closed_to_client = closed_to_client || (e.close && e.at >= t_take);
    CHECK(closed_to_client, "its channel is closed to the client (Destroyed)");
    // corpses nobody loots are closed 15 s after the death [retail: 7 of 7 corpses with nothing to loot]
    uint64_t bare = 0;
    for (const uint64_t w : wolves) {
        if (w != looted && !(after.corpse_of(w) && after.corpse_of(w)->parts) && after.corpse_of(w)) bare = w;
    }
    CHECK(bare != 0 && reg.find(bare) != nullptr, "a corpse with no loot is still there 5 s after its death");
    world_play(r, 11.0);
    CHECK(bare != 0 && reg.find(bare) == nullptr && r.now() - t_died >= 15.0, "and it is closed 15 s after its death [R: 15.0 s]");

    // ---- hostile hits: on a dead target, on a forged one, on a far one, on the player's own cylinder
    const uint64_t dead = wolves[0] == looted ? wolves[1] : wolves[0];
    const size_t rewards = r.h.s->kills_rewarded();
    const float my_hp = r.m.stats[content::kGuidHealth];
    {
        const int32_t i1 = r.next_id++;
        const BunchWriter t1 = trigger_at_object(setup, ability, i1, cylinder_of(dead));
        r.send_trigger(ability, i1, &t1);
        world_play(r, 0.6);
        r.send_reliable(hit_request_at_object(setup, ability, targeting, hit, cylinder_of(dead)));
        world_play(r, 0.6);
    }
    CHECK(r.h.s->kills_rewarded() == rewards && cw.health_of(dead) <= 0, "a hit on a dead creature changes nothing and pays nothing a second time");
    const double far_hp = cw.health_of(far_wolf);
    {
        const int32_t i2 = r.next_id++;
        const BunchWriter t2 = trigger_at_object(setup, ability, i2, cylinder_of(far_wolf));
        r.send_trigger(ability, i2, &t2);
        world_play(r, 0.6);
        r.send_reliable(hit_request_at_object(setup, ability, targeting, hit, cylinder_of(far_wolf)));
        world_play(r, 0.6);
    }
    CHECK(cw.health_of(far_wolf) == far_hp, "a hit on a Wolf 30 m away (out of the axe's range) takes none of its Health");
    {
        emit::FIntrepidNetworkGUID forged = cylinder_of(wolves[3]);
        forged.ObjectId += 0x7777777;
        const int32_t i3 = r.next_id++;
        const BunchWriter t3 = trigger_at_object(setup, ability, i3, forged);
        r.send_trigger(ability, i3, &t3);
        world_play(r, 0.6);
        r.send_reliable(hit_request_at_object(setup, ability, targeting, hit, forged));
        world_play(r, 0.6);
    }
    CHECK(!r.h.s->abilities_failed() && r.m.unreadable == 0, "a trigger and a hit on an object that is no actor: the session is fine");
    {
        const int32_t i4 = r.next_id++;
        const BunchWriter t4 = trigger_at_object(setup, ability, i4, setup.guids.collision_cylinder);
        r.send_trigger(ability, i4, &t4);
        world_play(r, 0.6);
        r.send_reliable(hit_request_at_object(setup, ability, targeting, hit, setup.guids.collision_cylinder));
        world_play(r, 0.6);
    }
    CHECK(r.m.stats[content::kGuidHealth] >= my_hp - 0.5f && !r.h.s->dead(), "an axe swing at the player's own cylinder (not a hostile target) hurts nobody");
    CHECK(r.h.s->kills_rewarded() == rewards, "(no kill came of any of them)");
}
// C_Y_B + CoA

/// Kills the Wolves one by one (the pawn as the killer) until one leaves a corpse with loot; its id (0: none of them).
static uint64_t kill_for_loot(Run& r, const std::vector<uint64_t>& wolves) {
    combat::Aftermath* after = r.h.fx.world->aftermath();
    for (const uint64_t w : wolves) {
        kill_npc(r, w, r.h.s->bootstrap().setup().guids.pawn.ObjectId);
        world_play(r, 0.2);
        const combat::Corpse* c = after->corpse_of(w);
        if (c && c->parts) return w;
    }
    return 0;
}

static void test_e1_loot_with_full_bag() {
    std::printf("E1 stage 3, part 2: a full bag leaves the loot in the corpse %s\n", kAftermathNote);
    svc::Character rec = bard25();
    for (int i = 0; i < 70; ++i) rec.items.push_back(stack_of(kGlintT1, i, 10, static_cast<uint32_t>(i)));   // seventy full stacks: no room, nothing to merge into
    std::vector<uint64_t> wolves;
    uint64_t far_wolf = 0, next_actor_id = 5000000;
    bool ground = false;
    auto prepare = [&](PlayerWorld& w) { spawn_wolves(w, wolves, far_wolf, 14, 24, 0, ground, next_actor_id); };
    Run r(0, true, &rec, prepare);
    CHECK(ground && wolves.size() == 14 && r.h.fx.world->aftermath(), "a world with Wolves and a character whose bag is full");
    if (!ground || wolves.size() != 14 || !r.h.fx.world->aftermath()) return;
    CHECK(r.h.s->inventory().count(svc::ItemContainer::Backpack) == 70, "the bag holds seventy stacks");
    combat::Aftermath& after = *r.h.fx.world->aftermath();
    r.play(0.5, false);
    phase_in(r);
    world_play(r, 1.0);
    const bs::PlayerSetup setup = r.h.s->bootstrap().setup();
    const uint64_t looted = kill_for_loot(r, wolves);
    CHECK(looted != 0, "a kill leaves loot");
    if (!looted) return;
    const combat::Corpse* c = after.corpse_of(looted);
    const size_t stacks = c->loot.size();
    send_on_pawn(r, interact_bunch(c->parts->interactable));
    world_play(r, 0.3);
    CHECK(r.m.loot_windows.size() == 1, "the window opens");
    if (r.m.loot_windows.empty()) return;
    std::vector<repl::ItemStorageEvent> all;
    for (size_t i = 0; i < stacks; ++i) all.push_back(take_event(r.m.loot_windows.back(), i, c->parts->container_guid, setup));
    send_on_pc(r, loot_bunch(all));
    world_play(r, 0.3);
    CHECK(r.h.s->loot_taken() == 0 && after.corpse_of(looted)->remaining() == stacks, "taking with no room takes nothing: every stack stays in the corpse");
    CHECK(r.m.storage_events.empty() && r.m.listeners_removed == 0 && r.h.s->loot_actor() == looted, "the client was told of no removal, the window stays open");
    CHECK(r.h.s->inventory().count(svc::ItemContainer::Backpack) == 70 && !r.h.s->abilities_failed(), "the bag is as it was");
}

static void test_e1_player_death() {
    std::printf("E1 stage 3, part 2: the player's death and respawn through the session %s\n", kAftermathNote);
    svc::Character rec = bard25();
    rec.items.push_back(stack_of(kCopper, 0, 20, 1));
    rec.items.push_back(stack_of(kOak, 1, 7, 2));
    rec.items.push_back(stack_of(kGlintT1, 2, 5, 3));       // item.resource.cert: a resource, by the tags (modelled: half of it is lost, too)
    rec.items.push_back(stack_of(kMount, 3, 1, 4));         // no resource tag: never lost
    std::vector<uint64_t> wolves;
    uint64_t far_wolf = 0, next_actor_id = 5000000;
    bool ground = false;
    auto prepare = [&](PlayerWorld& w) { spawn_wolves(w, wolves, far_wolf, 2, 24, 800, ground, next_actor_id); };
    Run r(0, true, &rec, prepare);
    CHECK(ground && wolves.size() == 2 && r.h.fx.world->aftermath(), "a world with a Wolf at the player's side and a character with copper, oak and Glint in the bag");
    if (!ground || wolves.size() != 2 || !r.h.fx.world->aftermath()) return;
    combat::CombatWorld& cw = *r.h.fx.world->combat();
    combat::Aftermath& after = *r.h.fx.world->aftermath();
    world::ActorRegistry& reg = r.h.fx.world->registry();
    r.play(0.5, false);
    phase_in(r);
    world_play(r, 1.0);
    const bs::PlayerSetup setup = r.h.s->bootstrap().setup();
    const uint64_t pawn = setup.guids.pawn.ObjectId;
    const int64_t debt0 = r.h.s->progress().debt;
    CHECK(held_of(*r.h.s, kCopper) == 20 && held_of(*r.h.s, kOak) == 7 && held_of(*r.h.s, kGlintT1) == 5 && held_of(*r.h.s, kMount) == 1, "the bag holds 20 copper, 7 oak, 5 Glint and a mount");
    // the user's death of 2026-10-10: a session that has played for a while (the harness's clock starts at 100 s; the user's death was at 66 s) when the player dies
    CHECK(r.h.s->movement().good().has_time_stamp && r.h.s->movement().good().time_stamp > 100.0f, "(the client's movement clock is past 100 s: that is the checker's last good TimeStamp)");
// -- C.O.A & Cyb --

    // ---- the death: a hit of the Wolf's that takes all the Health
    combat::IncomingHit lethal;
    lethal.instigator = wolves[0];
    lethal.hit_guid = 1;
    lethal.mods.push_back({1, content::kGuidHealth, -1.0e9});
    lethal.at = r.now();
    combat::PlayerPort* port = cw.find_player(pawn);
    CHECK(port != nullptr, "the combat world knows the player");
    if (!port) return;
    const double t_death = r.now();
    port->receive_hit(lethal, r.now());
    r.ts = 0.0f;   // the death's ClientRestart restarts the client's pawn, and its movement clock with it: the TimeStamp of its moves starts again near 0
    world_play(r, 0.6);
    CHECK(r.h.s->dead() && r.h.s->deaths() == 1, "the session reads the Health at 0: the player is dead");
    CHECK(r.m.stats[content::kGuidHealth] <= 0.0f && r.m.stats[content::kGuidMana] <= 0.0f, "the client's Health and Mana are 0");
    CHECK(r.m.death_locations.size() == 1 && std::fabs(r.m.death_locations[0][0] - kX) < 100 && std::fabs(r.m.death_locations[0][1] - kY) < 100, "ClientSetLastDeathLocation names where the player fell");
    size_t restarts = 0, cams = 0;
    for (const std::string& n : r.m.pc_calls) {
        restarts += n == "ClientRestart" ? 1 : 0;
        cams += n == "ClientSetCameraMode" ? 1 : 0;
    }
    CHECK(restarts == 2 && cams == 2, "the PlayerController got ClientRestart and ClientSetCameraMode twice (the retail death frame)");
    CHECK(r.h.s->progress().debt > debt0 && r.m.xp_updates >= 1, "the death put experience debt on the character and the client's XPTracker says so");
    CHECK(held_of(*r.h.s, kCopper) == 10 && held_of(*r.h.s, kOak) == 3 && held_of(*r.h.s, kGlintT1) == 2 && held_of(*r.h.s, kMount) == 1,
          "about half of each stack of materials is gone (10 copper, 3 oak, 2 Glint certificates), the mount is not a material");
    const uint64_t pile = r.h.s->pile_actor();
    const world::WorldActor* pa = pile ? reg.find(pile) : nullptr;
    CHECK(pa != nullptr && pa->kind == world::ActorKind::Pile && std::hypot(pa->x - kX, pa->y - kY) < 100, "a loot pile stands where the player fell");
    const combat::Corpse* pc = pile ? after.corpse_of(pile) : nullptr;
    int64_t pile_copper = 0, pile_oak = 0;
    if (pc) {
        for (const combat::LootStack& s : pc->loot) {
            pile_copper += s.item == kCopper ? s.quantity : 0;
            pile_oak += s.item == kOak ? s.quantity : 0;
        }
    }
    CHECK(pc != nullptr && pc->is_pile && pile_copper == 10 && pile_oak == 4, "the pile holds the other half (10 copper, 4 oak, 3 Glint certificates)");
    bool opened = false;
    for (const Mirror::ChannelEvent& e : r.m.other) opened = opened || (e.open && e.at >= t_death);
    CHECK(opened, "the client was sent the pile's actor");
    // the wolf lets the dead go
    world_play(r, 3.0);
    const auto ai = cw.ai_state(wolves[0]);
    CHECK(ai.has_value() && *ai != npc::ai::State::Attack && *ai != npc::ai::State::Chase, "the Wolf that killed the player stops attacking it");

    // ---- the dead do nothing
    const size_t opens0 = r.h.s->loot_opens();
    if (pc && pc->parts) send_on_pawn(r, interact_bunch(pc->parts->interactable));
    world_play(r, 0.2);
    CHECK(r.h.s->loot_opens() == opens0, "a dead player's interaction is ignored");
    const int32_t sid = r.next_id++;
    r.send_trigger(kSprint, sid);
    world_play(r, 0.5);
    CHECK(!r.m.has_ability(kSprint), "a dead player's trigger starts nothing");

    // ---- the respawn choice
    send_on_pawn(r, respawn_choices_bunch());
    world_play(r, 0.3);
    CHECK(r.m.choices.size() == 1 && r.m.choices[0].closest.size() == 1 && r.m.choices[0].home == r.m.choices[0].closest[0] && !r.m.choices[0].previous_invalid, "RequestServerRespawnChoices is answered with the places the player may come back (one) and the home");
    if (r.m.choices.empty() || r.m.choices[0].closest.empty()) return;
    const int64_t record = r.m.choices[0].closest[0];
    send_on_pawn(r, respawn_choice_bunch(record + 12345));
    world_play(r, 0.3);
    CHECK(r.h.s->dead() && r.m.choices.size() == 2 && r.m.choices[1].previous_invalid, "a choice the server did not offer: the player stays dead and the choices come again, marked invalid");
    send_on_pawn(r, respawn_choice_bunch(0));
    world_play(r, 0.3);
    CHECK(r.h.s->dead() && r.h.s->respawns() == 0, "a choice of 0: still dead");
    send_on_pawn(r, respawn_choice_bunch(record));
    world_play(r, 0.5);
    CHECK(!r.h.s->dead() && r.h.s->respawns() == 1, "the offered choice brings the player back");
    const PlayerWorld::RespawnSpot spot = r.h.fx.world->respawn_spot(kX, kY);
    const GoodPosition& good = r.h.s->movement().good();
    CHECK(std::fabs(good.x - spot.x) < 1 && std::fabs(good.y - spot.y) < 1, "at the respawn point (the login spot when no shrine is nearer in the area)");
    CHECK(r.m.stats[content::kGuidHealth] >= r.m.stats[content::kGuidMaxHealth] - 0.5f && r.m.stats[content::kGuidMana] >= r.m.stats[content::kGuidMaxMana] - 0.5f, "with full Health and Mana");
    // the client's moves go on with its restarted clock (0.1 s, 0.2 s ...), from where the respawn put it: they are heard, not dropped for 66 s as TimeStampNotRising
    {
        const size_t acked = r.h.s->moves_acknowledged(), stale = r.h.s->moves_stale();
        for (int i = 0; i < 10; ++i) {
            r.ts += 0.1f;
            repl::NetworkMove mv = move_at(r.ts, spot.x, true);
            mv.location = repl::quantize_vector(100, spot.x + 10.0 * (i + 1), spot.y, spot.z);
            r.h.client_sends({r.h.client.actor(r.h.pawn_ch, client_move(mv), /*reliable=*/false)});
            r.h.step(milliseconds(100));
            r.absorb();
        }
        std::printf("    (after the respawn: %zu of 10 moves at the restarted clock's TimeStamps acknowledged, %zu dropped as stale; the last good TimeStamp %.2f, x %.1f against the shrine's %.1f)\n",
                    r.h.s->moves_acknowledged() - acked, r.h.s->moves_stale() - stale, r.h.s->movement().good().time_stamp, r.h.s->movement().good().x, spot.x);
        CHECK(r.h.s->moves_acknowledged() >= acked + 9 && r.h.s->moves_stale() == stale, "the moves after the respawn, at the restarted clock's TimeStamps, are acknowledged (the death forgot the old clock)");
        CHECK(r.h.s->movement().good().time_stamp < 10.0f && r.h.s->movement().good().x > spot.x + 50.0, "and the player walks on from the shrine on the new clock: the last good move is the client's (TimeStamp under 10 s, not over 100), not the shrine's");
    }
    // a second choice when alive: ignored
    const size_t choices_n = r.m.choices.size();
    send_on_pawn(r, respawn_choice_bunch(record));
    send_on_pawn(r, respawn_choices_bunch());
    world_play(r, 0.3);
    CHECK(r.h.s->respawns() == 1 && r.m.choices.size() == choices_n, "choices of a player who is alive are ignored");
    // the dead's pile is the world's: the player takes back what it lost
    if (pc && pc->parts && std::hypot(spot.x - pa->x, spot.y - pa->y) < PlayerSession::kInteractRangeCm - 100) {
        const size_t windows = r.m.loot_windows.size();
        send_on_pawn(r, interact_bunch(pc->parts->interactable));
        world_play(r, 0.3);
        CHECK(r.m.loot_windows.size() == windows + 1, "the pile's window opens for its owner");
        if (r.m.loot_windows.size() == windows + 1) {
            const repl::LootPayload w = r.m.loot_windows.back();
            std::vector<repl::ItemStorageEvent> all;
            for (size_t i = 0; i < w.data.storage.size(); ++i) all.push_back(take_event(w, i, pc->parts->container_guid, setup));
            send_on_pc(r, loot_bunch(all));
            world_play(r, 0.3);
            CHECK(held_of(*r.h.s, kCopper) == 20 && held_of(*r.h.s, kOak) == 7 && held_of(*r.h.s, kGlintT1) == 5, "taking the pile gives back the materials (20 copper, 7 oak, 5 Glint)");
            CHECK(r.m.listeners_removed == 1 && after.corpse_of(pile) && after.corpse_of(pile)->emptied, "the emptied pile closes its window and is marked emptied");
        }
    } else {
        CHECK(false, "(the respawn point is within reach of the pile)");
    }
    CHECK(!r.h.s->abilities_failed() && r.m.unreadable == 0, "(the session is fine, every chain reads)");
}
// built by C Y B with CoA

static void test_e1_death_without_loss() {
    std::printf("E1 stage 3, part 2: with --no-death-loss a death costs experience only %s\n", kAftermathNote);
    svc::Character rec = bard25();
    rec.items.push_back(stack_of(kCopper, 0, 20, 1));
    std::vector<uint64_t> wolves;
    uint64_t far_wolf = 0, next_actor_id = 5000000;
    bool ground = false;
    auto prepare = [&](PlayerWorld& w) {
        combat::AftermathConfig cfg = w.aftermath_config();
        cfg.death_loss = false;
        w.set_aftermath_config(cfg);
        spawn_wolves(w, wolves, far_wolf, 1, 24, 0, ground, next_actor_id);
    };
    Run r(0, true, &rec, prepare);
    if (!ground || wolves.empty() || !r.h.fx.world->combat()) {
        CHECK(false, "the world");
        return;
    }
    r.play(0.5, false);
    combat::PlayerPort* port = r.h.fx.world->combat()->find_player(r.h.s->bootstrap().setup().guids.pawn.ObjectId);
    combat::IncomingHit lethal;
    lethal.instigator = wolves[0];
    lethal.hit_guid = 1;
    lethal.mods.push_back({1, content::kGuidHealth, -1.0e9});
    if (port) port->receive_hit(lethal, r.now());
    world_play(r, 0.6);
    CHECK(r.h.s->dead() && r.h.s->progress().debt > 0, "the player died with debt");
    CHECK(held_of(*r.h.s, kCopper) == 20 && r.h.s->pile_actor() == 0, "the copper is all there and no pile stands");
}

int main() {
    auto logger = std::make_shared<spdlog::logger>("test", std::make_shared<spdlog::sinks::ostream_sink_mt>(g_log));
    logger->set_level(spdlog::level::debug);
    spdlog::set_default_logger(logger);

    test_sprint();
    test_sprint_cancel();
    test_sprint_standing();
    test_dodge();
    test_rest();
    test_sprint_and_dodge();
    test_instance_ids();
    test_dodge_dash();
    test_ability_waits_for_the_join();
    test_ability_update_waits_for_the_window();
    test_ability_log();
    test_refusals();
    test_laggy_client();
    test_server_stall();
    test_through_the_endpoint();
    test_hostile_client();
    if (ability_rig::real_db_present()) {
        test_d2_glee_heals();
        test_d2_flourish_dash();
        test_d2_dodge_keeps_cooldowns();
        test_d2_sprint_speed();
        test_d2_sprint_drain();
        test_d2_dance_actor();
        test_e1_sprint_with_world();
        test_e1_sprint_under_load();
        test_e1_clock_reset();
        test_e1_long_session_with_loss(0, 1);
        test_e1_long_session_with_loss(5, 4);
        test_e1_long_session_with_loss(10, 20);
        test_d2_floating_numbers();
        test_d2_shielding();
        test_d2_hit_hostile();
        test_d2_hostile();
        test_d2_song();
        test_d2_held_cancel();
        test_d2_end_ability();
        test_d2_class_resource();
        test_e1_combat_session();
        test_e1_inline_target_session();
        test_e1_npc_cylinder_export();
        test_e1_one_clock();
        test_e1_ranged_book_at_wolf();
        test_e1_cylinder_in_own_block();
        test_e1_zone_abilities_export_rule();
        test_e1_clock_resync();
        test_e1_whole_fight();
        test_e1_loot_with_full_bag();
        test_e1_player_death();
        test_e1_death_without_loss();
    } else {
        std::printf("no content.db at %s: the plan D2 session tests are skipped\n", ability_rig::real_db_path().c_str());
    }

    if (const char* f = std::getenv("AOC_TEST_LOG_FILTER")) {   // (a debugging aid: the log lines that contain the text)
        std::istringstream in(g_log.str());
        for (std::string line; std::getline(in, line);) {
            if (line.find(f) != std::string::npos) std::printf("  | %s\n", line.c_str());
        }
    }
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
