// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_gather.cpp  (Gathering stage 2, part "Harvesting")
//
//  The harvest of the resource nodes without a session: the shared node state, the node's wire against the retail captures, the requirements and the rolls, on the real
//  content.db (AOC_CONTENT_DB, else dist/content/content.db of the working tree; "[skip]" without it) and the captured node templates.
//
//  the data            gather_def, gather_tool and item_variation read; the 439 placed nodes' defs and the garden's 10 all have a gather row, a reward table and a formula for the
//                      channel; every tool item of the three professions has its slot, tier and requirement
//  the node's wire     capture 1427 replayed to the bit: the node as the harvester's client has it after the interact (the open's two blocks), the 96 heartbeats, the done update;
//                      capture 1454's open of a gathered node as another client sees it (the RepLayout part to the bit, the whole block in length)
//  the board           Available -> Channeling -> Gathered -> Available, and cancelled; the respawn is the data's (the actor def's override, else the foliage's EmptyDuration) times the
//                      multiplier; shared by two players (the second is refused); a channel that never completes is freed; a node removed during the channel; the exploits at this level
//  the requirements    certification, level, the tool's profession and tier, its durability, the character's right to the tool, combat
//  the rolls           the rarity per harvest against the analytic tables (100,000 harvests, standard and _T1_Only, ratings 0 and 8409); the experience 80/90/100/120 by tier and rarity;
//                      the quantity (the base at rating 0, 5 + 1 with a rating, under both readings of GetRolledItemQuality); the channel's length against the captured ones;
//                      capture 1470's bonus drops (Limestone x5 q6, Coal x3 q1, Sapphire x5 q31) reproduced with forced draws and the captured event order
// ============================================================================
#include "capture_gather.h"
#include "test_support.h"

#include "bootstrap/character_setup.h"
#include "gather/gathering.h"
#include "gather/node_board.h"
#include "gather/node_wire.h"
#include "gather/rules.h"
#include "protocol/repl/block_reader.h"
#include "protocol/repl/replicator.h"
#include "protocol/server_guid_allocator.h"
#include "session/player_world.h"
#include "spawns/captured_spawns.h"
#include "spawns/garden_defs.h"
#include "world/interaction_state.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace aoc::worldd;
namespace repl = aoc::protocol::repl;
namespace emit = aoc::protocol::emit;
namespace cg = capture_gather;
namespace fs = std::filesystem;
using tsupport::Bits;
using tsupport::bits_of_hex;
using std::chrono::milliseconds;
using std::chrono::seconds;

static int g_pass = 0, g_fail = 0;
#undef CHECK
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

static constexpr uint32_t kServerId = 60;
static constexpr uint32_t kRandomizer = 0x6ee87ee4u;
static const Clock::time_point t0{seconds(5000)};
// [C-O-A x Cyb]

// the professions, tiers and tools of the real file
namespace id {
inline constexpr int64_t kMining = 108752769996425ll, kLumberjacking = 108752769996426ll, kHerbalism = 108752770061963ll;
inline constexpr int64_t kT1Pickaxe = 6064629375589548039ll, kT3Pickaxe = 6064629376467927053ll, kT1Axe = 6064629375589613576ll, kT1Sickle = 6064629375589482502ll;
inline constexpr int64_t kT2Pickaxe = 6064630367571673324ll;
inline constexpr int64_t kDaffodil = 6064632138835886135ll;    // Plant_Daffodil_Medium_1: Herbalism, Novice, Sickle, _T1_Only
inline constexpr int64_t kMoonbell = 6064631483095515243ll;    // Plant_Moonbell_Medium_2: Herbalism, Apprentice
inline constexpr int64_t kLimestone = 6064631483017789494ll;   // Stone_Limestone_Large_1: Mining, Journeyman, with the bonus tables
inline constexpr int64_t kRividium = 6064631669497200654ll;    // Metal_Rividium_Large_1: Mining, Journeyman
inline constexpr int64_t kHalcyonite = 6064631483054948432ll; // Gem_Halcyonite_Large_1: Mining, Apprentice
}  // namespace id

static const char* db_path() {
    const char* env = std::getenv("AOC_CONTENT_DB");
    if (env && *env) return env;
#ifdef GATHER_CONTENT_DB_PATH
    if (fs::exists(GATHER_CONTENT_DB_PATH)) return GATHER_CONTENT_DB_PATH;
#endif
    return nullptr;
}

struct World {
    std::optional<content::ContentDb> db;
    aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
    std::unique_ptr<PlayerWorld> world;
    std::optional<spawns::CapturedData> captured;
    const spawns::OpenTemplate* donor = nullptr;
    gather::Gathering* g = nullptr;
    std::vector<int64_t> ratings;       // the artisanship rating stats of the bundle

    bool open(const char* path) {
        std::string error;
        db = content::ContentDb::open(path, error);
        if (!db) {
            std::printf("  [FAIL] the content.db does not open: %s\n", error.c_str());
            return false;
        }
        world = std::make_unique<PlayerWorld>(&*db, "", bootstrap::GameStateMode::Tier2, kServerId, guids);
        if (!world->ready()) {
            std::printf("  [FAIL] the world is not ready: %s\n", world->content_problem().c_str());
            return false;
        }
        auto c = spawns::CapturedData::load(error);
        if (!c) {
            std::printf("  [FAIL] the captured spawns do not load: %s\n", error.c_str());
            return false;
        }
        captured = std::move(*c);
        donor = spawns::garden_donor(*captured);
        if (!donor) {
            std::printf("  [FAIL] no captured gatherable open\n");
            return false;
        }
        gather::Config cfg;
        if (!world->start_gathering(cfg, error)) {
            std::printf("  [FAIL] gathering does not start: %s\n", error.c_str());
            return false;
        }
        g = world->gathering();
        const stats::StatBundle& bundle = *world->stat_bundle();
        for (size_t i = 0; i < bundle.stats.size(); ++i) {
            const std::string& n = bundle.stat_name(i);
            if (n.rfind("Artisan", 0) == 0 && n.size() > 7 && n.compare(n.size() - 7, 7, "_Rating") == 0) ratings.push_back(bundle.stats[i].guid);
        }
        return true;
    }

    world::ActorRegistry& registry() { return world->registry(); }
    emit::FIntrepidNetworkGUID mint() { return world->mint_guid(); }

    /// A resource node of the def `def_guid` at (x, y), made from the captured template (as the garden makes its nodes).
    uint64_t add_node(int64_t def_guid, uint32_t seed, double x = 0, double y = 0, std::string* error = nullptr) {
        std::string why;
        spawns::InstanceParams p;
        p.keep_captured_position = true;
        p.server_id = kServerId;
        p.seed = 0x6A12DE0ull ^ seed;
        auto node = spawns::instantiate(*donor, [this] { return world->mint_guid(); }, p, why);
        if (!node) {
            if (error) *error = why;
            return 0;
        }
        const content::GatherDefRow* def = g->def(def_guid);
        if (!def) {
            if (error) *error = "no gather def " + std::to_string(def_guid);
            return 0;
        }
        repl::ObjectState& actor = node->actor.obj;
        actor.state.set(*actor.schema, "GatherableActorDef.Guid", repl::Value::i64(def_guid));
        for (repl::Subobject& s : node->actor.subobjects) {
            if (!s.obj.schema || std::string(s.obj.schema->name) != "UInteractableComponent") continue;
            const repl::Value* data = s.obj.state.get(*s.obj.schema, "InteractionInputData");
            const auto* arr = data ? data->get_if<repl::ArrayValue>() : nullptr;
            if (arr && !arr->elements.empty()) {
                repl::ArrayValue copy = *arr;
                copy.elements[0].values[6] = repl::Value::i64(def->interaction_guid);
                s.obj.state.set(*s.obj.schema, "InteractionInputData", repl::Value(std::move(copy)));
            }
        }
        node->x = x;
        node->y = y;
        node->z = 0;
        node->npc.source = world::Source::Runtime;
        const uint64_t id = node->id;
        if (!registry().add(std::move(*node))) {
            if (error) *error = "the registry refuses the node";
            return 0;
        }
        return id;
    }

    const content::GatherDefRow* def_named(const std::string& name) const {
        for (const auto& [guid, d] : g->data().defs) {
            if (d.name == name) return &d;
        }
        return nullptr;
    }

    /// The player's stat run: a Bard at `level` with nothing worn, the professions at `levels`.
    std::unique_ptr<stats::CharacterStats> make_stats(int level = 25) {
        const bootstrap::StatContext ctx = world->stat_context();
        auto cs = std::make_unique<stats::CharacterStats>(*ctx.bundle, *ctx.gear, 2, level, ctx.rounding);
        std::string err;
        if (!cs->start({}, err)) {
            std::printf("  [FAIL] the character's stats do not start: %s\n", err.c_str());
            std::exit(2);
        }
        return cs;
    }
    void rate_all(double rating) {
        std::map<int64_t, double> m;
        for (const int64_t g2 : ratings) m[g2] = rating;
        g->set_ratings(std::move(m));
    }
};

static int rarity_band(int quality) {
    static const int floors[] = {1, 6, 16, 31, 51, 71, 101};
    int r = 0;
    for (int i = 0; i < 7; ++i) {
        if (quality >= floors[i]) r = i;
    }
    return r;
}
// ~ Cyb :: C.O.A ~

// ---- the node's wire against capture 1427 ------------------------------------------------------------------------------------------

static bool payload_is(const repl::ContentBlock& b, const cg::Block& want) {
    if (b.has_rep_layout != want.rep) return false;
    const Bits mine(b.payload), theirs = bits_of_hex(want.hex, want.bits);
    if (mine == theirs) return true;
    size_t at = 0;
    while (at < mine.size() && at < theirs.size() && mine.b[at] == theirs.b[at]) ++at;
    std::printf("      (differs: %zu bits against the capture's %zu, the first difference at bit %zu)\n", mine.size(), theirs.size(), at);
    return false;
}

static std::optional<repl::ObjectState> load(const cg::Block& b, const char* cls) {
    return repl::load_object_state_hex(tsupport::schema_named(cls), b.hex, b.bits, b.rep);
}

static const repl::ElementState* element(const repl::ObjectState& o, const char* path, size_t index) {
    const repl::Value* v = o.state.get(*o.schema, path);
    const auto* a = v ? v->get_if<repl::ArrayValue>() : nullptr;
    return a && index < a->elements.size() ? &a->elements[index] : nullptr;
}

static void test_run_1427(World& w) {
    std::printf("capture 1427, replayed: the node's open, 96 heartbeats and the done update, to the bit\n");
    const auto cap_actor = load(cg::kRunOpenActor, "AGatherableActor");
    const auto cap_comp = load(cg::kRunOpenComponent, "UInteractableComponent");
    CHECK(cap_actor && cap_comp, "the captured blocks load");
    if (!cap_actor || !cap_comp) return;
    std::string why;
    const uint64_t node_id = w.add_node(id::kDaffodil, 1427, 0, 0, &why);
    CHECK(node_id != 0, "a Plant_Daffodil_Medium_1 node is made from the captured template");
    if (!node_id) return;
    // the node as 1427's: the capture's own actor values (the class default AuthServerIDReplicated is 0 there, the position travels in SerializeNewActor)
    w.registry().modify(node_id, world::Change::State, [&](world::WorldActor& a) {
        repl::ObjectState& o = a.actor.obj;
        for (const char* path : {"AuthServerIDReplicated", "RemoteRole", "Role", "GatherableGuid.A", "GatherableGuid.B", "GatherableGuid.C", "GatherableGuid.D"}) {
            if (const repl::Value* v = cap_actor->state.get(*cap_actor->schema, path)) o.state.set(*o.schema, path, *v);
        }
        o.state.erase(7);   // ReplicatedMovement: not in this open
        // the interaction's input data is what the capture's component says (the def's PlayerInteractDefinition)
        for (repl::Subobject& s : a.actor.subobjects) {
            if (!s.obj.schema || std::string(s.obj.schema->name) != "UInteractableComponent") continue;
            if (const repl::Value* v = cap_comp->state.get(*cap_comp->schema, "InteractionInputData")) s.obj.state.set(*s.obj.schema, "InteractionInputData", *v);
            s.obj.arrays = repl::FastArrays();   // the captured template's own state maps: this open has none (the actor view adds the viewer's)
            if (const repl::Value* v = cap_comp->state.get(*cap_comp->schema, "CurrentInputStates")) {
                // the open before the harvest: available (the capture's CurrentInputStates is what a fresh node holds)
                s.obj.state.set(*s.obj.schema, "CurrentInputStates", *v);
            }
        }
    });
    // the harvester: the capture's pawn (the whole netguid), character and player
    const repl::ElementState* statuses = element(*cap_comp, "InputEffectStatuses", 0);
    const auto pawn_it = statuses ? statuses->values.find(3) : decltype(statuses->values.end()){};
    CHECK(statuses && pawn_it != statuses->values.end() && pawn_it->second.get_if<emit::FIntrepidNetworkGUID>(), "the capture's pawn reference is in the open");
    if (!statuses || pawn_it == statuses->values.end()) return;
    gather::Channeler by;
    by.player_id = 7;
    by.character = repl::GuidValue{cg::kRunCharacter[0], cg::kRunCharacter[1], cg::kRunCharacter[2], cg::kRunCharacter[3]};
    by.pawn = *pawn_it->second.get_if<emit::FIntrepidNetworkGUID>();
    gather::NodeBoard board(w.registry(), gather::NodeBoard::Config{});
    CHECK(board.begin(node_id, by, t0, seconds(12), why), "the board starts the channel");
// C.O.A - C | Y | B

    const auto view_of = [&](uint64_t player) {
        const world::WorldActor* a = w.registry().find(node_id);
        repl::Actor v = a->actor;
        gather::show_interacting_player(*a, player, v);
        return v;
    };
    repl::ActorShadow shadow;
    std::vector<repl::ContentBlock> blocks;
    repl::Actor view = view_of(7);
    CHECK(repl::collect_open_blocks(view, shadow, blocks) && blocks.size() == 2, "the open has two blocks: the actor's and the interactable's");
    if (blocks.size() != 2) return;
    CHECK(payload_is(blocks[0], cg::kRunOpenActor), "the actor block is the capture's, to the bit (303 bits)");
    CHECK(payload_is(blocks[1], cg::kRunOpenComponent), "the component block is the capture's, to the bit (625 bits): the status 0x08000001, the pawn, who channels");

    // the heartbeats
    size_t same = 0, one_block = 0;
    Clock::time_point now = t0;
    for (size_t i = 0; i < cg::kRunBeatCount; ++i) {
        now += milliseconds(100);
        if (!board.beat(node_id, now)) break;
        view = view_of(7);
        std::vector<repl::ContentBlock> update;
        if (!repl::collect_update_blocks(view, shadow, update)) break;
        if (update.size() != 1) continue;
        ++one_block;
        same += payload_is(update[0], cg::kRunBeats[i].block) ? 1 : 0;
    }
    CHECK(one_block == cg::kRunBeatCount, "every heartbeat is one block: the interactable's");
    CHECK(same == cg::kRunBeatCount, "all 96 heartbeats are the capture's, to the bit (81 bits each; the first, 217, also takes InteractingPlayer off)");

    // the done update
    now += milliseconds(100);
    CHECK(board.complete(node_id, now, seconds(1800), why), "the board completes the channel");
    view = view_of(7);
    std::vector<repl::ContentBlock> update;
    CHECK(repl::collect_update_blocks(view, shadow, update) && update.size() == 2, "the done update is two blocks");
    if (update.size() == 2) {
        CHECK(payload_is(update[0], cg::kRunDoneActor), "the actor block (bGathered) is the capture's, to the bit (18 bits)");
        CHECK(payload_is(update[1], cg::kRunDoneComponent), "the component block (the status 0x20000004, the pawn, nobody channels, CanInputActivate, the state 0) is the capture's, to the bit (334 bits)");
    }
    CHECK(board.state_of(node_id) == gather::NodeState::Gathered, "the node is Gathered");
    // nothing more to send once it was
    CHECK(repl::collect_update_blocks(view_of(7), shadow, update) && update.empty(), "and nothing changes until the respawn");

    // the same node as another client holds it: no InteractingPlayer (capture 1454's)
    const repl::Actor other = view_of(99);
    const auto o_comp = [&]() -> std::optional<uint32_t> {
        for (const repl::Subobject& s : other.subobjects) {
            if (!s.obj.schema || std::string(s.obj.schema->name) != "UInteractableComponent") continue;
            const repl::Value* v = s.obj.state.get(*s.obj.schema, "InputEffectStatuses");
            const auto* a = v ? v->get_if<repl::ArrayValue>() : nullptr;
            const auto it = a ? a->elements[0].values.find(3) : decltype(a->elements[0].values.end()){};
            const auto* g = a && it != a->elements[0].values.end() ? it->second.get_if<emit::FIntrepidNetworkGUID>() : nullptr;
            return g ? static_cast<uint32_t>(g->ObjectId) : 0xffffffffu;
        }
        return std::nullopt;
    }();
    CHECK(o_comp && *o_comp == 0, "another player's connection sees InteractingPlayer as none");
}
// -- Cyb & C O A --

static void test_gathered_1454(World& w) {
    std::printf("capture 1454, replayed: a gathered node as another client opens it\n");
    std::string why;
    const uint64_t node_id = w.add_node(6064631483095515243ll, 1454, 0, 0, &why);
    if (!node_id) return;
    // the node of 1454's open: its interaction's def and no state maps of the template's
    const auto cap_comp = load(cg::kGatheredComponent, "UInteractableComponent");
    w.registry().modify(node_id, world::Change::State, [&](world::WorldActor& a) {
        for (repl::Subobject& s : a.actor.subobjects) {
            if (!s.obj.schema || std::string(s.obj.schema->name) != "UInteractableComponent") continue;
            if (cap_comp) {
                if (const repl::Value* v = cap_comp->state.get(*cap_comp->schema, "InteractionInputData")) s.obj.state.set(*s.obj.schema, "InteractionInputData", *v);
            }
            s.obj.arrays = repl::FastArrays();
        }
    });
    gather::NodeBoard board(w.registry(), gather::NodeBoard::Config{});
    gather::Channeler by;
    by.player_id = 5;
    by.character = repl::GuidValue{1, 2, 3, 4};
    by.pawn.ObjectId = 4242;
    Clock::time_point now = t0;
    CHECK(board.begin(node_id, by, now, seconds(12), why), "a channel starts");
    for (int i = 0; i < 6; ++i) {
        now += milliseconds(100);
        board.beat(node_id, now);
    }
    CHECK(board.complete(node_id, now + milliseconds(100), seconds(100), why), "and completes after six heartbeats: the done step is 7");
    const world::WorldActor* a = w.registry().find(node_id);
    CHECK(a && gather::node_status(*a) == (((1u + 3u * 7u) & 31u) << 27 | 4u) && gather::node_status(*a) == 2952790020u, "the status is 0xB0000004, the capture's");
    repl::Actor view = a->actor;                     // another player's view: nothing shown
    gather::show_interacting_player(*a, 99, view);
    repl::ActorShadow shadow;
    std::vector<repl::ContentBlock> blocks;
    CHECK(repl::collect_open_blocks(view, shadow, blocks) && blocks.size() == 2, "the open has two blocks");
    if (blocks.size() != 2) return;
    // the component's RepLayout block is the capture's, and the two state maps follow in the view's own fast arrays
    const std::vector<uint8_t> cap = tsupport::bytes_of_hex(cg::kGatheredComponent.hex);
    size_t pos = 0;
    const auto st = repl::read_rep_block(tsupport::schema_named("UInteractableComponent"), cap.data(), cg::kGatheredComponent.bits, pos, {});
    CHECK(st.has_value() && pos > 0 && pos < cg::kGatheredComponent.bits, "the captured block reads: a RepLayout block and the state maps after it");
    if (!st) return;
    const Bits mine(blocks[1].payload);
    const Bits theirs = bits_of_hex(cg::kGatheredComponent.hex, cg::kGatheredComponent.bits);
    CHECK(mine.size() >= pos && mine.slice(0, pos) == theirs.slice(0, pos), "our RepLayout block is the capture's, to the bit");
    // the two state maps the actor view adds (an item for input 0 each) follow the RepLayout block as the capture's do; their length is the replicator's (the view's maps are
    // the vendors' and the corpses' too; retail's are 96 bits shorter each)
    repl::Actor with_maps = view;
    world::set_player(with_maps, repl::GuidValue{1, 2, 3, 4}, true);
    repl::ActorShadow shadow2;
    std::vector<repl::ContentBlock> blocks2;
    const bool collected = repl::collect_open_blocks(with_maps, shadow2, blocks2) && blocks2.size() == 2;
    CHECK(collected && blocks2[1].payload.bit_pos() > blocks[1].payload.bit_pos() && Bits(blocks2[1].payload).slice(0, pos) == theirs.slice(0, pos),
          "with the viewer's guid in the two state maps the block is our RepLayout block, which is the capture's, and the maps after it");
    // the actor: bGathered is set
    const auto actor_state = repl::load_object_state(*a->actor.obj.schema, blocks[0].payload.bytes().data(), blocks[0].payload.bit_pos(), blocks[0].has_rep_layout);
    const repl::Value* gathered = actor_state ? actor_state->state.get(*actor_state->schema, "bGathered") : nullptr;
    CHECK(gathered && gathered->get_if<bool>() && *gathered->get_if<bool>(), "the actor block carries bGathered");
}

// ---- the data -------------------------------------------------------------------------------------------------------------------------

static void test_data(World& w) {
    std::printf("the gather data of the real content.db\n");
    CHECK(w.g->defs() == 339 && w.g->tools() == 87 && w.g->data().quality_expr.size() == 47, "339 node defs, 87 tools, 47 item variations");
    {
        std::set<int64_t> roots;
        for (const auto& [guid, d] : w.g->data().defs) roots.insert(d.reward_guid);
        CHECK(roots.size() > 100 && w.g->tables() >= roots.size(), "the reward tables of the nodes (and their sub tables) are loaded");
    }
    size_t gathering = 0, placed = 0, with_row = 0, garden = 0, garden_rows = 0;
    std::set<int64_t> placed_defs;
    for (const spawns::CapturedSpawn& s : w.captured->spawns()) {
        if (!s.open || s.open->category != "gatherable") continue;
        ++placed;
        const auto actor = s.open->actor_block();
        const repl::Value* v = actor.state.state.get(*actor.state.schema, "GatherableActorDef.Guid");
        const int64_t guid = v && v->get_if<int64_t>() ? *v->get_if<int64_t>() : 0;
        with_row += w.g->def(guid) != nullptr;
        placed_defs.insert(guid);
        (void)gathering;
    }
    CHECK(placed >= 439 && with_row == placed, "every captured node's def has a gather row (the 439 of the Riverlands and the E1 area's)");
    for (size_t i = 0; i < spawns::garden::kGardenDefCount; ++i) {
        ++garden;
        garden_rows += w.g->def(static_cast<int64_t>(spawns::garden::kGardenDefs[i].guid)) != nullptr;
    }
    CHECK(garden == 10 && garden_rows == 10, "the GM garden's 10 defs have gather rows");
    size_t formulas = 0, rewards = 0, tools_ok = 0, professions_ok = 0;
    for (const auto& [guid, d] : w.g->data().defs) {
        if (d.duration_expr != 0 && w.world->ability_catalog()->program(d.duration_expr) >= 0) ++formulas;
        if (d.reward_guid != 0) ++rewards;
        professions_ok += (d.profession_guid == id::kMining || d.profession_guid == id::kLumberjacking || d.profession_guid == id::kHerbalism) ? 1 : 0;
    }
    CHECK(formulas == 339 && rewards == 339 && professions_ok == 339, "all 339 defs: a compiled channel formula, a reward table, a gathering profession");
    for (const auto& [item, t] : w.g->data().tools) {
        tools_ok += (t.tool_slot == 26 || t.tool_slot == 29 || t.tool_slot == 33 || t.tool_slot == 35 || t.tool_slot == 38 || t.tool_slot == 36 || t.tool_slot == 37 || t.tool_slot == 34 || t.tool_slot == 16) ? 1 : 0;
    }
    CHECK(tools_ok > 0 && w.g->tool(id::kT1Pickaxe) && w.g->tool(id::kT1Pickaxe)->cert_tier == 0 && w.g->tool(id::kT3Pickaxe)->cert_tier == 2, "the T1 pickaxe reaches Novice nodes, the T3 pickaxe Journeyman");
    // the three equation variants' items: every tool-slot line of the node tables has a Quality formula that compiles
    size_t compiled = 0;
    for (const auto& [var, expr] : w.g->data().quality_expr) compiled += (expr != 0 && w.world->ability_catalog()->program(expr) >= 0) ? 1 : 0;
    CHECK(compiled >= 44, "44 of the 47 variations' Quality formulas compile (the Hunting prototype's and the loot variations' are not all gathering)");
    // a def's respawn
    const content::GatherDefRow* oak = w.def_named("Tree_Oak_Large_2");
    const content::GatherDefRow* stone = w.def_named("Stone_Basalt_Large_3");
    CHECK(oak && oak->override_respawn == 14400.0 && oak->empty_duration == 300.0, "Tree_Oak_Large_2: override 14,400 s, EmptyDuration 300 s");
    CHECK(stone && stone->override_respawn == 0.0 && stone->empty_duration == 840.0, "Stone_Basalt_Large_3: no override, EmptyDuration 840 s");
}

// ---- the board ------------------------------------------------------------------------------------------------------------------------

static gather::Channeler channeler(uint64_t player, uint32_t n) {
    gather::Channeler by;
    by.player_id = player;
    by.character = repl::GuidValue{n, n + 1, n + 2, n + 3};
    by.pawn.ObjectId = 1000 + player * 2;
    return by;
}
// [C-O-A x C.Y.B]

static void test_board(World& w) {
    std::printf("the board: the shared state of the nodes\n");
    gather::NodeBoard::Config cfg;
    cfg.respawn_mult = 0.5;
    gather::NodeBoard board(w.registry(), cfg);
    std::string why;
    const uint64_t a = w.add_node(id::kDaffodil, 1), b = w.add_node(id::kMoonbell, 2);
    CHECK(board.state_of(a) == gather::NodeState::Available && board.channeling() == 0 && board.gathered() == 0, "a fresh node is Available");
    Clock::time_point now = t0;
    CHECK(board.begin(a, channeler(1, 10), now, seconds(10), why), "player 1 starts a channel on node a");
    CHECK(board.state_of(a) == gather::NodeState::Channeling && board.channel_of(a) && board.channel_of(a)->player_id == 1, "node a is Channeling for player 1");
    CHECK(!board.begin(a, channeler(2, 20), now, seconds(10), why) && why.find("channeling") != std::string::npos, "player 2 is refused: the state is shared (the node is channeling)");
    CHECK(!board.begin(a, channeler(1, 10), now, seconds(10), why), "so is player 1 asking again: nothing is started twice");
    CHECK(board.begin(b, channeler(2, 20), now, seconds(10), why), "player 2 harvests the other node");
    CHECK(board.channels_of(1) == std::vector<uint64_t>{a} && board.channels_of(2) == std::vector<uint64_t>{b}, "each player's channels");
    CHECK(!board.begin(999999, channeler(3, 30), now, seconds(1), why), "a node that is not in the world is refused");
    // the heartbeats: one per 100 ms, never before, never after the channel
    CHECK(!board.beat(a, now + milliseconds(99)), "no heartbeat before 100 ms");
    CHECK(board.beat(a, now + milliseconds(100)) && board.channel_of(a)->step == 1, "the first heartbeat at 100 ms");
    CHECK(!board.beat(a, now + milliseconds(150)), "none at 150 ms");
    CHECK(board.beat(a, now + milliseconds(200)) && board.channel_of(a)->step == 2, "the second at 200 ms");
    CHECK(board.beat(a, now + seconds(5)) && board.channel_of(a)->step == 3 && !board.beat(a, now + seconds(5) + milliseconds(50)), "a stall makes one update, not a burst");
    CHECK(!board.beat(a, now + seconds(10)), "no heartbeat once the channel's time is over: the completion is next");
    // completion
    CHECK(board.complete(a, now + seconds(10), board.respawn_of(14400.0, 300.0), why), "the channel completes: Gathered");
    CHECK(board.state_of(a) == gather::NodeState::Gathered && board.gathered_until(a) && std::chrono::duration<double>(*board.gathered_until(a) - (now + seconds(10))).count() == 7200.0,
          "the respawn is the data's 14,400 s (the actor def's override) times 0.5");
    CHECK(!board.complete(a, now, seconds(1), why), "a second completion of the same channel is refused");
    CHECK(!board.begin(a, channeler(2, 20), now + seconds(11), seconds(10), why) && why.find("gathered") != std::string::npos, "a gathered node cannot be started");
    CHECK(board.respawn_of(0.0, 840.0) == std::chrono::duration<double>(420.0) && board.respawn_of(0.0, 0.0) == std::chrono::duration<double>(1.0), "no override: the foliage's EmptyDuration; never under a second");
    CHECK(board.cancel(b, "the second player walks away") && board.state_of(b) == gather::NodeState::Available, "player 2 walks away from node b: it is Available again");
    CHECK(board.advance(now + seconds(7000)) == 0 && board.state_of(a) == gather::NodeState::Gathered, "a gathered node stays gathered until its time");
    CHECK(board.advance(now + seconds(7211)) == 1 && board.state_of(a) == gather::NodeState::Available, "it is Available again when its time has come");
    const world::WorldActor* na = w.registry().find(a);
    CHECK(na && !gather::node_gathered(*na) && gather::node_status(*na) == 0, "and the actor is the fresh node's again: bGathered off, no status");
    CHECK(board.begin(a, channeler(1, 10), now + seconds(7300), seconds(10), why), "so the node can be harvested again");
    // cancel
    CHECK(board.cancel(a, "a test") && board.state_of(a) == gather::NodeState::Available && !board.cancel(a, "twice"), "a cancel makes the node Available once");
    CHECK(board.begin(b, channeler(2, 20), now + seconds(7300), seconds(10), why), "player 2 starts at node b again");
    CHECK(board.cancel_player(2, "the session ends") == 1 && board.state_of(b) == gather::NodeState::Available && board.cancel_player(2, "again") == 0, "the channels of a player who leaves are cancelled");
    // a stuck channel
    const Clock::time_point later = now + seconds(8000);
    CHECK(board.begin(a, channeler(3, 30), later, seconds(10), why), "a channel starts");
    CHECK(board.advance(later + seconds(39)) == 0 && board.state_of(a) == gather::NodeState::Channeling, "30 s after it was due it is still the channel's");
    CHECK(board.advance(later + seconds(41)) == 1 && board.state_of(a) == gather::NodeState::Available && board.stats().stuck == 1, "a session that never completes it is overtaken: the node is Available");
    // a node removed from the world during the channel
    const uint64_t c = w.add_node(id::kDaffodil, 3);
    CHECK(board.begin(c, channeler(4, 40), later, seconds(10), why), "a channel starts on a third node");
    w.registry().remove(c);
    CHECK(!board.beat(c, later + milliseconds(200)), "a heartbeat to a node that left the world makes nothing");
    CHECK(!board.complete(c, later + seconds(10), seconds(1), why) && board.state_of(c) == gather::NodeState::Available && board.stats().gone >= 1, "a node removed during the channel cannot be completed, and is forgotten");
}

/// The entries of the node's CanInputActivate that a content block tells the client: nullopt when the block does not carry the property; else one entry per array element, the bool
/// the element's update writes (nullopt: the update leaves that element as the client has it).
static std::optional<std::vector<std::optional<bool>>> can_input_on_wire(const repl::ContentBlock& b) {
    if (!b.sub || !b.sub->obj.schema || std::string(b.sub->obj.schema->name) != "UInteractableComponent" || !b.has_rep_layout) return std::nullopt;
    const Bits bits(b.payload);
    return tsupport::with_exact(bits, [&](const uint8_t* d, size_t, size_t n) -> std::optional<std::vector<std::optional<bool>>> {
        size_t pos = 0;
        const auto st = repl::read_rep_block(*b.sub->obj.schema, d, n, pos, {});
        const repl::Value* v = st ? st->get(*b.sub->obj.schema, "CanInputActivate") : nullptr;
        const auto* a = v ? v->get_if<repl::ArrayValue>() : nullptr;
        if (!a) return std::nullopt;
        std::vector<std::optional<bool>> out;
        for (const repl::ElementState& e : a->elements) {
            const auto it = e.values.find(1);
            const bool* x = it == e.values.end() ? nullptr : it->second.get_if<bool>();
            out.push_back(x ? std::optional<bool>(*x) : std::nullopt);
        }
        return out;
    });
}

/// Test 1 of the Gathering stage 2 user test: the client CRASHED 487 ms after the Daffodil cancel (an access violation in UInteractableComponent::CanInputActivate at read address 0).  The
/// function indexes the replicated array without a bound (`index < Num & Data[index]`), so a node that tells the client an EMPTY array leaves it with a null Data: node_reset (the cancel, the
/// respawn and the stuck-channel release) wrote exactly that.  Whatever the node does, what the client is told about CanInputActivate has its four entries, the first one the interaction's.
static void test_input_never_empty(World& w) {
    std::printf("the node's CanInputActivate is never empty on the wire (the client indexes the array without a bound: an empty one crashed it)\n");
    gather::NodeBoard::Config cfg;
    gather::NodeBoard board(w.registry(), cfg);
    std::string why;
    const uint64_t a = w.add_node(id::kDaffodil, 41);
    repl::ActorShadow shadow;
    std::vector<repl::ContentBlock> opened;
    CHECK(a != 0 && repl::collect_open_blocks(w.registry().find(a)->actor, shadow, opened), "a fresh node opens (retail's open carries no CanInputActivate: the client keeps its class default)");
    bool open_has_it = false;
    for (const repl::ContentBlock& b : opened) open_has_it = open_has_it || can_input_on_wire(b).has_value();
    CHECK(!open_has_it, "a fresh node's open does not carry CanInputActivate (1427's does not)");
    // what an update tells the client about CanInputActivate now (nullopt: nothing)
    const auto told = [&]() -> std::optional<std::vector<std::optional<bool>>> {
        const world::WorldActor* n = w.registry().find(a);
        std::vector<repl::ContentBlock> update;
        if (!n || !repl::collect_update_blocks(n->actor, shadow, update)) return std::nullopt;
        for (const repl::ContentBlock& b : update) {
            if (auto c = can_input_on_wire(b)) return c;
        }
        return std::nullopt;
    };
    const auto four_and_first_is = [](const std::optional<std::vector<std::optional<bool>>>& c, bool first) { return c && c->size() == 4 && (*c)[0] && *(*c)[0] == first; };
    Clock::time_point now = t0;

    // a cancel (the player walks away): the node is available again
    CHECK(board.begin(a, channeler(1, 10), now, seconds(10), why), "a channel starts");
    told();
    CHECK(board.cancel(a, "the player walks away"), "and is cancelled");
    const auto after_cancel = told();
    CHECK(four_and_first_is(after_cancel, true), "the cancel tells the client CanInputActivate with four entries, the first true: the interaction is open again (an empty array was the crash)");
// built with C-O-A and Cyb

    // a completed harvest, then the respawn
    CHECK(board.begin(a, channeler(1, 10), now + seconds(1), seconds(10), why), "a channel starts again");
    told();
    CHECK(board.complete(a, now + seconds(11), seconds(100), why), "and completes");
    const auto after_done = told();
    CHECK(four_and_first_is(after_done, false), "the done update tells the client four entries of which the first is false (retail's 33 bits)");
    CHECK(board.advance(now + seconds(112)) == 1 && board.state_of(a) == gather::NodeState::Available, "the node returns after its respawn");
    const auto after_respawn = told();
    CHECK(four_and_first_is(after_respawn, true), "the respawn tells the client four entries, the first true: the node can be harvested again");

    // a channel nobody completes: the board frees it
    CHECK(board.begin(a, channeler(2, 20), now + seconds(200), seconds(10), why), "a channel starts a third time");
    told();
    CHECK(board.advance(now + seconds(200 + 10 + 31)) == 1 && board.state_of(a) == gather::NodeState::Available, "its session never completes it: the board frees the node 30 s after its time");
    const auto after_release = told();   // the client already holds the respawn's four entries, so the release may have nothing to add
    CHECK(!after_release || four_and_first_is(after_release, true), "the release tells the client nothing new, or four entries with the first true: never an empty array");

    // the actor the registry holds never has the empty array
    const world::WorldActor* n = w.registry().find(a);
    size_t held = 99;
    if (n) {
        for (const repl::Subobject& s : n->actor.subobjects) {
            if (!s.obj.schema || std::string(s.obj.schema->name) != "UInteractableComponent") continue;
            const repl::Value* v = s.obj.state.get(*s.obj.schema, "CanInputActivate");
            const auto* arr = v ? v->get_if<repl::ArrayValue>() : nullptr;
            held = arr ? arr->elements.size() : 0;
        }
    }
    CHECK(held == 4, "and the node in the registry holds four entries");
}

// ---- the requirements -------------------------------------------------------------------------------------------------------------------

static void test_rules(World& w) {
    std::printf("the requirements of a harvest\n");
    const professions::Book& book = *w.world->professions();
    professions::Professions novice(book);
    const content::GatherDefRow* basalt = w.def_named("Stone_Basalt_Large_3");                // Mining, Novice, Pickaxe
    const content::GatherDefRow* halcyonite = &w.g->data().defs.at(id::kHalcyonite);          // Mining, Apprentice
    const content::GatherDefRow* rividium = &w.g->data().defs.at(id::kRividium);              // Mining, Journeyman
    const auto tool = [&](int64_t item, float durability = 100.0f) {
        gather::ToolInfo t;
        t.worn = true;
        t.tool = w.g->tool(item);
        t.durability = durability;
        return t;
    };
    CHECK(basalt && basalt->cert_tier == 0 && halcyonite->cert_tier == 1 && rividium->cert_tier == 2, "Novice, Apprentice and Journeyman Mining nodes");
    CHECK(gather::check_requirements(*basalt, novice, tool(id::kT1Pickaxe), false).ok(), "a Novice with a T1 pickaxe harvests a Novice node");
    CHECK(gather::check_requirements(*basalt, novice, gather::ToolInfo{}, false).why == gather::Refusal::NoTool, "no tool in the slot: refused");
    gather::ToolInfo junk;
    junk.worn = true;
    junk.durability = 10;
    CHECK(gather::check_requirements(*basalt, novice, junk, false).why == gather::Refusal::NotATool, "a worn item that is no gathering tool: refused");
    CHECK(gather::check_requirements(*basalt, novice, tool(id::kT1Axe), false).why == gather::Refusal::WrongToolProfession, "an axe in the pickaxe's place: refused (the tool is another profession's)");
    CHECK(gather::check_requirements(*basalt, novice, tool(id::kT1Pickaxe, 0.0f), false).why == gather::Refusal::ToolBroken, "a tool with no durability: refused");
    CHECK(gather::check_requirements(*halcyonite, novice, tool(id::kT1Pickaxe), false).why == gather::Refusal::NotCertified, "a Novice at an Apprentice node: not certified");
    professions::Professions apprentice = novice;
    apprentice.raise_xp_to(id::kMining, book.max_xp(*book.find(id::kMining), 0));
    apprentice.promote(id::kMining);
    CHECK(apprentice.tier(id::kMining) == 1 && apprentice.level(id::kMining) == 10, "an Apprentice Miner at level 10 (a Promote at the Novice cap)");
    CHECK(gather::check_requirements(*halcyonite, apprentice, tool(id::kT1Pickaxe), false).why == gather::Refusal::ToolTierTooLow, "an Apprentice with a T1 pickaxe at an Apprentice node: the tool's tier is too low");
    CHECK(gather::check_requirements(*halcyonite, apprentice, tool(id::kT2Pickaxe), false).ok(), "with a T2 pickaxe it harvests");
    CHECK(gather::check_requirements(*halcyonite, apprentice, tool(id::kT3Pickaxe), false).why == gather::Refusal::ToolNotUsable, "a T3 pickaxe needs Journeyman: the Apprentice cannot use it (worldd's equip rules let it be worn)");
    CHECK(gather::check_requirements(*rividium, apprentice, tool(id::kT2Pickaxe), false).why == gather::Refusal::NotCertified, "an Apprentice at a Journeyman node: not certified");
    professions::Professions journeyman = apprentice;
    journeyman.raise_xp_to(id::kMining, book.max_xp(*book.find(id::kMining), 1));
    journeyman.promote(id::kMining);
    CHECK(journeyman.tier(id::kMining) == 2 && gather::check_requirements(*rividium, journeyman, tool(id::kT3Pickaxe), false).ok(), "a Journeyman with the T3 pickaxe harvests a Journeyman node");
    CHECK(gather::check_requirements(*halcyonite, journeyman, tool(id::kT3Pickaxe), false).ok(), "...and an Apprentice node (a higher tool reaches lower nodes)");
    CHECK(gather::check_requirements(*rividium, journeyman, tool(id::kT2Pickaxe), false).why == gather::Refusal::ToolTierTooLow, "...but not a Journeyman node with a T2 pickaxe");
    content::GatherDefRow no_combat = *basalt;
    no_combat.allow_in_combat = false;
    CHECK(gather::check_requirements(no_combat, novice, tool(id::kT1Pickaxe), true).why == gather::Refusal::InCombat && gather::check_requirements(*basalt, novice, tool(id::kT1Pickaxe), true).ok(),
          "in combat: refused where the interaction does not allow it, harvested where it does");
    CHECK(gather::check_requirements(*basalt, professions::Professions(), tool(id::kT1Pickaxe), false).why == gather::Refusal::NoProfessions, "a world with no profession tables refuses");
    CHECK(gather::reach_cm(*basalt) == basalt->interact_radius + basalt->interact_tolerance + 100.0, "the reach is the radius, the tolerance and 100 cm");
}
// C | O | A : Cyb

// ---- the rolls --------------------------------------------------------------------------------------------------------------------------

struct Rig {
    World& w;
    std::unique_ptr<stats::CharacterStats> cs;
    std::unique_ptr<stats::CharacterStats::Evaluation> run;
    stats::SplitMix rng;
    explicit Rig(World& world, uint64_t seed = 1) : w(world), cs(world.make_stats()) {
        std::string err;
        run = cs->evaluation({}, err);
        if (!run) {
            std::printf("  [FAIL] the stat run does not start: %s\n", err.c_str());
            std::exit(2);
        }
        rng.state = seed;
    }
    std::optional<gather::Harvest> harvest(int64_t def_guid, std::string* why = nullptr) {
        std::string e;
        const content::GatherDefRow* d = w.g->def(def_guid);
        auto h = w.g->harvest(*d, *run, 25, world::NodeSite{}, nullptr, rng, e);
        if (why) *why = e;
        return h;
    }
};

static void expect_dist(const char* name, const int counts[7], int n, const double want[6]) {
    // 3 sigma of each band's share, and 0.05 % for the table's own rounding
    bool ok = true;
    char line[512];
    int len = std::snprintf(line, sizeof line, "%s:", name);
    for (int i = 0; i < 6; ++i) {
        const double p = want[i] / 100.0;
        const double got = static_cast<double>(counts[i]) / n;
        const double tol = 3.0 * std::sqrt(p * (1 - p) / n) + 0.0005;
        ok = ok && std::fabs(got - p) <= tol;
        len += std::snprintf(line + len, sizeof line - static_cast<size_t>(len), " %.2f", got * 100.0);
    }
    std::snprintf(line + len, sizeof line - static_cast<size_t>(len), " (want %.2f %.2f %.2f %.2f %.3f %.3f)", want[0], want[1], want[2], want[3], want[4], want[5]);
    CHECK(ok && counts[6] == 0, line);
}

static void test_rarity(World& w) {
    std::printf("the rarity of a harvest: 100,000 harvests per case against the analytic tables\n");
    constexpr int N = 100000;
    struct Case {
        const char* name;
        int64_t def;
        double rating;
        double want[6];
    };
    // rarity_dist.json (the research) for Artisan_Gathering_Rarity; the _T1_Only variant's from the Go test (statprog, 200,000 harvests) and t1dist.py
    const Case cases[] = {
        {"standard, rating 0", id::kMoonbell, 0, {81.9, 11.056, 3.437, 2.096, 1.01, 0.501}},
        {"standard, rating 8409", id::kMoonbell, 8409, {45.5, 43.278, 7.186, 2.481, 1.049, 0.505}},
        {"_T1_Only, rating 0", id::kDaffodil, 0, {85.365, 10.805, 2.221, 1.098, 0.51, 0.001}},
        {"_T1_Only, rating 8409", id::kDaffodil, 8409, {47.4, 44.4, 6.1, 1.5, 0.55, 0.005}},
    };
    Rig rig(w, 77);
    for (const Case& c : cases) {
        w.rate_all(c.rating);
        int counts[7] = {0, 0, 0, 0, 0, 0, 0};
        for (int i = 0; i < N; ++i) {
            const auto h = rig.harvest(c.def);
            if (!h || h->items.empty()) {
                ++counts[6];
                continue;
            }
            ++counts[std::min(h->primary_rarity, 6)];
        }
        expect_dist(c.name, counts, N, c.want);
    }
    w.rate_all(0);
    w.g->set_ratings({});
}
// ~ Cyb & C-O-A ~

static void test_rules_of_rolls(World& w) {
    std::printf("what a harvest gives: the quantity, the experience, the wear\n");
    w.g->set_ratings({});
    Rig rig(w, 5);
    const std::vector<double> common_roll = {0.99, 0.5, 0.99};   // no rarity bonus, a Common roll, no quantity bonus
    // the experience by tier at rating 0: Novice 80, Apprentice 90, Journeyman 100 (the captures: +80, +90, +100), and the quantity at its base
    const struct {
        int64_t def;
        const char* name;
        int xp;
        int64_t quantity;
        int adventuring_min;
    } tiers[] = {{id::kDaffodil, "Daffodil (Novice)", 80, 3, 1}, {id::kMoonbell, "Moonbell (Apprentice)", 90, 3, 1}, {id::kHalcyonite, "Halcyonite (Apprentice)", 90, 5, 1},
                 {id::kRividium, "Rividium (Journeyman)", 100, 5, 1}};
    for (const auto& t : tiers) {
        rig.rng.script = &common_roll;
        rig.rng.scripted = 0;
        std::string why;
        const auto h = rig.harvest(t.def, &why);
        const bool ok = h && h->items.size() == 1 && h->items[0].quantity == t.quantity && h->items[0].quality == 1 && h->profession_xp == t.xp && h->primary_rarity == 0;
        char msg[200];
        std::snprintf(msg, sizeof msg, "%s: x%lld q1, +%lld profession XP (want x%lld, +%d)", t.name, h && !h->items.empty() ? static_cast<long long>(h->items[0].quantity) : -1ll,
                      h ? static_cast<long long>(h->profession_xp) : -1ll, static_cast<long long>(t.quantity), t.xp);
        CHECK(ok, msg);
        CHECK(h && h->adventuring_xp > 0, (std::string(t.name) + ": and Adventuring experience").c_str());
        CHECK(rig.rng.scripted == common_roll.size(), (std::string(t.name) + ": the three scripted draws are all it used (the quality formula's two, the quantity's one)").c_str());
    }
    // an Uncommon item (quality 6) gives 1.5 times: 90 x 1.5 = 135 at the Apprentice tier, 80 x 1.5 = 120 at Novice (1463's +120)
    const std::vector<double> uncommon = {0.99, 0.925, 0.99};
    rig.rng.script = &uncommon;
    rig.rng.scripted = 0;
    auto h = rig.harvest(id::kMoonbell);
    CHECK(h && h->items.size() == 1 && h->items[0].quality == 6 && h->primary_rarity == 1 && h->profession_xp == 135, "an Uncommon (q6) Moonbell gives +135 (90 x 1.5)");
    // the experience follows the rarity of the FIRST item (a bonus drop does not change it): Limestone q6 with a Heroic bonus gem
    // the quantity: a rating gives a chance of a bonus (1428: Oak x6 = 5 + 1), under both readings of GetRolledItemQuality
    for (const bool literal : {false, true}) {
        gather::Config cfg;
        cfg.quality_literal = literal;
        std::string error;
        // (the world's gathering is one; the reading is its config: a second Gathering on the same registry)
        auto g2 = gather::Gathering::make(*w.db, w.registry(), *w.world->ability_catalog(), cfg, error);
        CHECK(g2 != nullptr, literal ? "a Gathering with the literal reading" : "a Gathering with the band reading");
        if (!g2) continue;
        std::map<int64_t, double> r;
        for (const int64_t guid : w.ratings) r[guid] = 8409;
        g2->set_ratings(r);
        // Oak: rating 8409 gives a 50 % chance of a bonus; the draws: the rarity's chance (fails), its roll (Common), the quantity's chance (succeeds), RandIntRange(1, max) (the first)
        const std::vector<double> bonus = {0.99, 0.5, 0.0, 0.0};
        stats::SplitMix r2;
        r2.script = &bonus;
        const content::GatherDefRow* oak = w.def_named("Tree_Oak_Large_2");
        std::string why;
        auto run = w.make_stats();
        auto ev = run->evaluation({}, why);
        const auto x = oak ? g2->harvest(*oak, *ev, 25, world::NodeSite{}, nullptr, r2, why) : std::nullopt;
        char msg[160];
        std::snprintf(msg, sizeof msg, "an Oak at rating 8409 with a lucky quantity roll gives %lld (5 + 1), %s reading", x && !x->items.empty() ? static_cast<long long>(x->items[0].quantity) : -1ll,
                      literal ? "literal" : "band");
        CHECK(x && x->items.size() == 1 && x->items[0].quantity == 6 && x->items[0].quality == 1, msg);
    }
    // the tool's wear: 1, and 2 for a Medium axe node (Axe_DurabilityLoss_Medium)
    rig.rng.script = &common_roll;
    rig.rng.scripted = 0;
    h = rig.harvest(id::kDaffodil);
    CHECK(h && h->durability_loss == 1.0, "a plant wears the tool by 1");
    size_t twos = 0, ones = 0, others = 0;
    for (const auto& [guid, d] : w.g->data().defs) {
        rig.rng.script = nullptr;
        const auto x = rig.harvest(guid);
        if (!x) continue;
        if (x->durability_loss == 2.0) ++twos;
        else if (x->durability_loss == 1.0) ++ones;
        else ++others;
    }
    CHECK(ones > 300 && others == 0, "every node wears the tool by 1 or 2");
    std::printf("      (%zu nodes wear the tool by 2, %zu by 1)\n", twos, ones);
}

static void test_bonus_1470(World& w) {
    std::printf("capture 1470: the bonus drops of a Limestone node, with forced draws\n");
    w.g->set_ratings({});
    Rig rig(w, 9);
    // Limestone x5 q6 (Uncommon), then the bonus container's 3 % (Coal x3 q1) and the second pick of two among Iron, Sapphire and nothing (Sapphire x5 q31):
    //   the item: rarity chance (fails), roll .925 = Uncommon, quantity chance (fails)       the container: .01 = its first element
    //   Coal: RandIntRange(1,1000) 0.1 = Common, quantity chance (fails)                      the tables: .3 = the second of [25, 25, 50], .9 = the third of what is left
    //   Sapphire: rarity chance (fails), roll .975 = Heroic, quantity chance (fails)
    const std::vector<double> draws = {0.99, 0.925, 0.99, 0.01, 0.1, 0.99, 0.3, 0.9, 0.99, 0.975, 0.99};
    rig.rng.script = &draws;
    std::string why;
    const auto h = rig.harvest(id::kLimestone, &why);
    CHECK(h.has_value(), "the harvest rolls");
    if (!h) return;
    CHECK(h->items.size() == 3, "three stacks");
    if (h->items.size() == 3) {
        std::vector<std::tuple<uint64_t, int64_t, int>> got, want;
        for (const combat::RolledItem& it : h->items) got.emplace_back(static_cast<uint64_t>(it.item), it.quantity, it.quality);
        for (const cg::FrameEvent& e : cg::kFrameEvents) {
            if (e.capture == 1470) want.emplace_back(e.event.record, e.event.quantity, e.event.quality);
        }
        CHECK(got == want, "Limestone x5 q6, Coal x3 q1 and Sapphire x5 q31: the captured records, quantities and qualities, in the captured order");
    }
    CHECK(rig.rng.scripted == draws.size(), "and every scripted draw was used, none more drawn");
    CHECK(h->primary_rarity == 1 && h->primary_quality == 6 && h->profession_xp == 150, "the experience is the first item's: Uncommon at a Journeyman node, 100 x 1.5 = 150 (the Heroic bonus gem adds nothing)");
    // the bonus tables are drawn from at the data's odds: Coal about 3 % of the harvests
    Rig stat(w, 31);
    int coal = 0, sapphire = 0, iron = 0;
    constexpr int N = 60000;
    for (int i = 0; i < N; ++i) {
        const auto x = stat.harvest(id::kLimestone);
        if (!x) continue;
        for (const combat::RolledItem& it : x->items) {
            coal += it.item == 109162169652923ll;
            sapphire += it.item == 6064629252182507530ll;
            iron += it.item == 109162169652928ll;
        }
    }
    char msg[200];
    std::snprintf(msg, sizeof msg, "over %d harvests: Coal %.2f %% (3 %%), Iron %.2f %% and Sapphire %.2f %% (about 50 %% each: two of three picks)", N, coal * 100.0 / N, iron * 100.0 / N, sapphire * 100.0 / N);
    CHECK(coal > N * 0.025 && coal < N * 0.036 && iron > N * 0.40 && iron < N * 0.60 && sapphire > N * 0.40 && sapphire < N * 0.60, msg);
}
// C-Y-B captures, C-O-A code

static void test_durations(World& w) {
    std::printf("the channel's length against the captured harvests\n");
    Rig rig(w, 3);
    w.g->set_ratings({});
    std::string why;
    struct Row {
        int64_t def;
        double want;
    };
    const struct {
        int64_t def;
        double base;
    } bases[] = {{id::kDaffodil, 11.25}, {id::kMoonbell, 11.25}, {id::kHalcyonite, 15.0}, {id::kRividium, 15.0}, {id::kLimestone, 15.0}};
    bool bases_ok = true;
    for (const auto& b : bases) {
        const content::GatherDefRow* d = w.g->def(b.def);
        const auto s = d ? w.g->channel_seconds(*d, *rig.run, why) : std::nullopt;
        bases_ok = bases_ok && s && std::fabs(*s - b.base) < 1e-9 && d->base_duration == b.base;
    }
    CHECK(bases_ok, "at rating 0 the channel is the base: 11.25 s for a plant, 15 s for a large stone (and 7.5 s for a medium one)");
    // with a rating the channel shortens by the GatheringSpeed curve (1.0 at 0 ... 0.5 at 8409), and never below half
    double last = 100;
    bool monotone = true;
    for (const double rating : {0.0, 50.0, 107.5, 500.0, 2000.0, 4204.5, 10000.0}) {
        w.rate_all(rating);
        const content::GatherDefRow* d = w.g->def(id::kHalcyonite);
        const auto s = w.g->channel_seconds(*d, *rig.run, why);
        monotone = monotone && s && *s <= last + 1e-9 && *s >= 7.5 - 1e-9;
        if (s) last = *s;
        if (rating == 107.5) CHECK(s && std::fabs(*s / 15.0 - 0.927) < 0.003, "ratings of 107.5 (the equation sums two: 215) give the 0.927 of the captures (13.9 s of 15)");
    }
    CHECK(monotone, "a higher rating never lengthens the channel, and it never goes under half the base");
    w.g->set_ratings({});
    // the captured channels are base x a factor between a half and 1 (the players had ratings)
    bool all_in = true;
    size_t seen = 0;
    for (const cg::Harvest& h : cg::kHarvests) {
        if (h.beats < 20) continue;
        const content::GatherDefRow* d = w.g->def(h.def);
        if (!d) {
            all_in = false;
            continue;
        }
        ++seen;
        // the node's open to the result frame, less the 87 ms of the open: the channel the harvester saw
        const double measured = (h.frame_ms - h.open_ms + 87) / 1000.0;
        const double factor = measured / d->base_duration;
        all_in = all_in && factor > 0.5 && factor <= 1.0;
    }
    CHECK(seen == 9 && all_in, "the nine captured channels are 0.5 to 1.0 of their node's base duration");
}

int main() {
    spdlog::set_level(spdlog::level::warn);
    const char* path = db_path();
    if (!path) {
        std::printf("[skip] test_worldd_gather: no content.db (set AOC_CONTENT_DB, or build dist/content/content.db)\n");
        return 0;
    }
    std::printf("content.db: %s\n", path);
    World w;
    if (!w.open(path)) return 1;
    std::printf("%zu artisanship rating stats in the bundle\n", w.ratings.size());
    test_data(w);
    test_run_1427(w);
    test_gathered_1454(w);
    test_board(w);
    test_input_never_empty(w);
    test_rules(w);
    test_rules_of_rolls(w);
    test_bonus_1470(w);
    test_durations(w);
    test_rarity(w);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
