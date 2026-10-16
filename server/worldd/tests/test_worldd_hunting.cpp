// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_hunting.cpp  (Gathering stage 3, part "Hunting")
//
//  The hunt of a creature without a session, on the real content.db (AOC_CONTENT_DB, else dist/content/content.db of the working tree; "[skip]" without it), the captured NPC
//  templates and dist/content/npc_motion.tsv.
//
//  the template     a huntable creature (an asset whose PlayerInteractionDef is a Hunting one) is made with the eighth block, a UInteractableComponent, the Raven's captured one with the
//                   asset's two ids put in: the Spider's and the Tidesnapper's come out to the bit of their captures (capture_hunting.h); every other creature has its seven blocks; the
//                   two assets that share the Hunting_Wolf Blueprint stay two templates; the interaction component's id is minted and found (find_by_component), the interaction def is kept
//  the data         the 53 hunting interactions: Novice 13, Apprentice 22, Journeyman 18; the Bow (slot 38), 15 s, 200 + 30 cm; each has its reward table; the 45 that give an item roll it
//                   (a carcass or a trainable, Hunter XP 160 / 200 / 240 by tier), the 8 that cannot (the RETIRED_ and NOTUSED_ items, an empty table) are named; the captured items of the five
//                   hunts (a Bear's carcass, a Raven's, a Wolf's trainable, a Ram's, a Hawk's) are among the rolls; the channel at rating 0
//  a Timid Grem    (retest 3) the normal Rabid_Timid against the hostile Rabid of capture 623 (no capture opened a Timid one): the same alignments, none of the huntable's; its instances override the aggro
//                  radius with 0 (aggro_off); the AI of a creature that does not notice players stays idle with a player at 1 m and fights back once hurt
//  the corpse wire  capture 1457 replayed through the corpse writers to the bit: the start, 204 heartbeats (the counter +2 every 67 ms), the done update; the harvester sees its pawn, a
//                   second connection does not; a cancelled hunt puts the corpse back as it was; a corpse is never "gathered" (no bGathered)
// ============================================================================
#include "capture_hunting.h"
#include "test_support.h"

#include "combat/combat_data.h"
#include "combat/combat_world.h"
#include "combat/npc_replicator.h"
#include "gather/gathering.h"
#include "gather/node_board.h"
#include "gather/node_wire.h"
#include "gather/rules.h"
#include "npc/ai/npc_ai.h"
#include "npc/motion_table.h"
#include "protocol/repl/block_reader.h"
#include "protocol/repl/replicator.h"
#include "protocol/server_guid_allocator.h"
#include "session/player_world.h"
#include "spawns/area_loader.h"
#include "spawns/captured_spawns.h"
#include "spawns/generic_npc.h"
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

#ifndef NPC_MOTION_PATH
#define NPC_MOTION_PATH "dist/content/npc_motion.tsv"
#endif
// (C Y B) (C.O.A)

using namespace aoc::worldd;
namespace repl = aoc::protocol::repl;
namespace emit = aoc::protocol::emit;
namespace ch = capture_hunting;
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

static const char* db_path() {
    const char* env = std::getenv("AOC_CONTENT_DB");
    if (env && *env) return env;
#ifdef HUNT_CONTENT_DB_PATH
    if (fs::exists(HUNT_CONTENT_DB_PATH)) return HUNT_CONTENT_DB_PATH;
#endif
    return nullptr;
}

static const char* kRavenPath = "/Game/DesignAssets/Artisanship/Gathering/Hunting/Hunting_Creatures/Hunting_Raven.Default__Hunting_Raven_C";

struct World {
    std::optional<content::ContentDb> db;
    aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
    std::unique_ptr<PlayerWorld> world;
    std::optional<spawns::CapturedData> captured;
    npc::MotionTable motion;
    content::PopulationData pop;
    std::set<int64_t> hunting_defs;
    std::map<int64_t, std::string> hunting_names;
    gather::Gathering* g = nullptr;

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
        if (!motion.load(NPC_MOTION_PATH, error)) {
            std::printf("  [FAIL] the Blueprint table does not load: %s\n", error.c_str());
            return false;
        }
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
        g = world->gathering();
        for (const auto& [def, kind] : g->hunting()) hunting_defs.insert(def);
        std::string fe;
        if (const auto foliage = db->foliage_data(fe)) hunting_names = gather::hunting_names(*foliage);
        return true;
    }
    world::ActorRegistry& registry() { return world->registry(); }
    emit::FIntrepidNetworkGUID mint() { return world->mint_guid(); }
// [Cyb :: C O A]

    const content::NpcAssetRow* asset(const std::string& name) const {
        for (const auto& [guid, a] : pop.assets) {
            if (a.name == name) return &a;
        }
        return nullptr;
    }
    spawns::GenericConfig config(bool hunting) const {
        spawns::GenericConfig c;
        if (hunting) {
            c.hunting_defs = hunting_defs;
            c.hunting_names = hunting_names;
        }
        return c;
    }
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
};

static const repl::ElementState* element(const repl::ObjectState& o, const char* path, size_t index) {
    const repl::Value* v = o.state.get(*o.schema, path);
    const auto* a = v ? v->get_if<repl::ArrayValue>() : nullptr;
    return a && index < a->elements.size() ? &a->elements[index] : nullptr;
}

static std::optional<int64_t> int_of(const repl::Value* v) {
    if (!v) return std::nullopt;
    if (const int64_t* i = v->get_if<int64_t>()) return *i;
    if (const uint64_t* u = v->get_if<uint64_t>()) return static_cast<int64_t>(*u);
    return std::nullopt;
}

static bool payload_is(const repl::ContentBlock& b, const ch::Block& want) {
    if (b.has_rep_layout != want.rep) return false;
    const Bits mine(b.payload), theirs = bits_of_hex(want.hex, want.bits);
    if (mine == theirs) return true;
    size_t at = 0;
    while (at < mine.size() && at < theirs.size() && mine.b[at] == theirs.b[at]) ++at;
    std::printf("      (differs: %zu bits against the capture's %zu, the first difference at bit %zu)\n", mine.size(), theirs.size(), at);
    return false;
}

static std::optional<repl::ObjectState> load(const ch::Block& b, const char* cls) {
    return repl::load_object_state_hex(tsupport::schema_named(cls), b.hex, b.bits, b.rep);
}

/// The interactable component of an OpenTemplate or an actor.
static const repl::ObjectState* interactable(const spawns::OpenTemplate& t) {
    const int i = t.find("UInteractableComponent");
    return i < 0 ? nullptr : &t.blocks[static_cast<size_t>(i)].state;
}

static Bits rep_block_of(const repl::ObjectState& o) {
    repl::RepShadow shadow;
    emit::BunchWriter w;
    bool failed = false;
    if (repl::write_rep_block(*o.schema, o.state, shadow, w, {}, &failed) == 0 || failed) return Bits();
    return Bits(w);
}

// ---- the template ------------------------------------------------------------------------------------------------------------------------

static void test_template(World& w) {
    std::printf("the huntable creature's template\n");
    const content::NpcAssetRow* wolf = w.asset("Hunting_Wolf");
    const content::NpcAssetRow* test_wolf = w.asset("HuntingTest");
    const content::NpcAssetRow* plain = w.asset("Pop_Wolf");
    const content::NpcAssetRow* spider = w.asset("Hunting_Spider");
    const content::NpcAssetRow* tide = w.asset("Hunting_Tidensapper");
    const content::NpcAssetRow* raven = w.asset("Hunting_Raven");
    CHECK(wolf && test_wolf && plain && spider && tide && raven, "the assets are in the data");
    if (!(wolf && test_wolf && plain && spider && tide && raven)) return;
    CHECK(wolf->archetype_path == test_wolf->archetype_path && wolf->interaction_def != test_wolf->interaction_def, "(Hunting_Wolf and HuntingTest share one Blueprint and differ in their interaction)");
// C:Y:B - CoA

    spawns::GenericNpcs off(*w.captured, &w.motion, w.config(false));
    spawns::GenericNpcs on(*w.captured, &w.motion, w.config(true));
    CHECK(w.hunting_defs.size() == 53, "53 hunting interaction defs are known");
    const spawns::OpenTemplate* old_wolf = off.template_for(*wolf);
    CHECK(old_wolf && old_wolf->blocks.size() == 7 && old_wolf->find("UInteractableComponent") < 0, "without the hunting defs a Hunting_Wolf has the seven blocks of a plain creature (as before)");

    spawns::Verdict v = spawns::Verdict::NoPath;
    const spawns::OpenTemplate* hunting_wolf = on.template_for(*wolf, &v);
    CHECK(hunting_wolf && v == spawns::Verdict::Generic && hunting_wolf->blocks.size() == 8 && hunting_wolf->blocks.back().cls == "UInteractableComponent" && hunting_wolf->blocks.back().kind == 2 &&
              !hunting_wolf->blocks.back().class_chain.empty(),
          "a Hunting_Wolf has eight blocks, the eighth a dynamic UInteractableComponent with its class export");
    const spawns::OpenTemplate* plain_wolf = on.template_for(*plain);
    CHECK(plain_wolf && plain_wolf->blocks.size() == 7 && on.template_for(*plain) == plain_wolf, "a plain Wolf has its seven blocks, and the template is the same object when asked again");
    const spawns::OpenTemplate* test_tpl = on.template_for(*test_wolf);
    CHECK(test_tpl && test_tpl != hunting_wolf && test_tpl->blocks.size() == 7 && on.template_for(*wolf) == hunting_wolf && on.template_for(*test_wolf) == test_tpl,
          "HuntingTest (its def is no hunting one) stays a plain creature beside the Hunting_Wolf, whatever order they are asked in");
    if (hunting_wolf && test_tpl) {
        bool same_export = hunting_wolf->archetype.size() == test_tpl->archetype.size();
        for (size_t i = 0; same_export && i < hunting_wolf->archetype.size(); ++i) same_export = hunting_wolf->archetype[i].guid.ObjectId == test_tpl->archetype[i].guid.ObjectId;
        CHECK(same_export, "and both name the Blueprint's export by the same NetGUIDs");
    }
    const repl::ObjectState* o = hunting_wolf ? interactable(*hunting_wolf) : nullptr;
    CHECK(o != nullptr, "the Hunting_Wolf's interactable reads");
    if (o) {
        const auto comp = int_of(o->state.get(*o->schema, "ComponentDefId.Guid"));
        const repl::ElementState* input = element(*o, "InteractionInputData", 0);
        const auto def = input && input->values.count(6) ? int_of(&input->values.at(6)) : std::nullopt;
        CHECK(comp && *comp == wolf->interact_component && def && *def == wolf->interaction_def, "it holds the asset's ComponentDefId (ICD_Hunting_Wolf) and PlayerInteractDefinition (Hunting_Wolf)");
        bool ones = true;
        for (size_t i = 0; i < 4; ++i) {
            const repl::ElementState* s = element(*o, "CurrentInputStates", i);
            const auto cur = s && s->values.count(1) ? int_of(&s->values.at(1)) : std::nullopt;
            ones = ones && cur && *cur == 1;
        }
        CHECK(ones, "and four interaction states of 1, as a living huntable's");
    }
    // the Spider's and the Tidesnapper's eighth block are the captures' to the bit
    const char* names[] = {"Hunting_Spider", "Hunting_Tidesnapper"};
    const content::NpcAssetRow* assets[] = {spider, tide};
    for (int i = 0; i < 2; ++i) {
        const spawns::OpenTemplate* t = on.template_for(*assets[i]);
        const repl::ObjectState* block = t && t->blocks.size() == 8 ? interactable(*t) : nullptr;
        const ch::Living* golden = nullptr;
        for (const ch::Living& l : ch::kLiving) {
            if (std::string(l.kind) == names[i]) golden = &l;
        }
        CHECK(block && golden && golden->component_def == static_cast<uint64_t>(assets[i]->interact_component) && golden->interaction_def == static_cast<uint64_t>(assets[i]->interaction_def) &&
                  rep_block_of(*block) == bits_of_hex(golden->block.hex, golden->block.bits),
              (std::string("the ") + names[i] + "'s eighth block, made from the Raven's, is the capture's, to the bit (276 bits)").c_str());
    }
    // the captured Raven keeps its own open (TemplateSource: captured first), eight blocks, and its ids are the asset's
    const spawns::TemplateSource source{&*w.captured, &on};
    bool is_generic = true;
    const spawns::OpenTemplate* cap = source.find(*raven, &is_generic);
    CHECK(cap && !is_generic && cap->blocks.size() == 8 && cap == w.captured->template_for(kRavenPath), "the Raven's template is the captured open (eight blocks) when a capture opened it");

    // an instance: the component's id is minted and found, the interaction def is kept
    spawns::InstanceParams p;
    p.x = 100;
    p.y = 200;
    p.z = 300;
    p.level = 7;
    p.server_id = kServerId;
    std::string why;
    auto made = hunting_wolf ? spawns::instantiate(*hunting_wolf, [&] { return w.mint(); }, p, why) : std::nullopt;
    CHECK(made && made->npc.interact_component != 0 && made->npc.interact_def == wolf->interaction_def && made->actor.subobjects.size() == 7, "an instance has the component's id in NpcInfo and the asset's interaction def");
    if (made) {
        const uint64_t id = made->id, comp = made->npc.interact_component;
        CHECK(w.registry().add(std::move(*made)), "it joins the registry");
        const world::WorldActor* found = w.registry().find_by_component(comp);
        CHECK(found && found->id == id, "and find_by_component finds it");
    }
    auto cap_made = cap ? spawns::instantiate(*cap, [&] { return w.mint(); }, p, why) : std::nullopt;
    CHECK(cap_made && cap_made->npc.interact_def == raven->interaction_def && cap_made->npc.interact_component != 0, "the captured Raven's instance reads its interaction def from its block too");
    auto plain_made = plain_wolf ? spawns::instantiate(*plain_wolf, [&] { return w.mint(); }, p, why) : std::nullopt;
    CHECK(plain_made && plain_made->npc.interact_def == 0 && plain_made->npc.interact_component == 0, "a plain creature has neither");
}

// ---- a living huntable's open against the captures (the friendly nameplate) ------------------------------------------------------------------

/// The CharacterName of an instance's UNPCInfo.
static std::string name_of(const repl::Actor& a) {
    const repl::Subobject* info = nullptr;
    for (const repl::Subobject& s : a.subobjects) {
        if (s.obj.schema && std::string(s.obj.schema->name) == "UNPCInfo") info = &s;
    }
    const repl::Value* v = info ? info->obj.state.get(*info->obj.schema, "CharacterName") : nullptr;
    const auto* s = v ? v->get_if<std::string>() : nullptr;
    return s ? *s : std::string();
}

static const repl::Subobject* find_sub(const repl::Actor& a, const char* cls) {
    for (const repl::Subobject& s : a.subobjects) {
        if (s.obj.schema && std::string(s.obj.schema->name) == cls) return &s;
    }
    return nullptr;
}

static void test_living_open(World& w) {
    std::printf("a living huntable's open against the captures: what makes the client draw it friendly\n");
    spawns::GenericNpcs on(*w.captured, &w.motion, w.config(true));
    constexpr uint64_t kSelf = 6064630243140894720ull;   // the alignment key of a creature's own instigator record
    for (const ch::Living& l : ch::kLiving) {
        const content::NpcAssetRow* asset = nullptr;
        for (const auto& [guid, row] : w.pop.assets) {
            if (static_cast<uint64_t>(row.interaction_def) == l.interaction_def && !asset) asset = &row;
        }
        const spawns::OpenTemplate* tpl = asset ? on.template_for(*asset) : nullptr;   // (the generic template, as for the kinds no capture opened)
        spawns::InstanceParams p;
        p.server_id = kServerId;
        p.level = 12;
        std::string why;
        auto made = tpl ? spawns::instantiate(*tpl, [&] { return w.mint(); }, p, why) : std::nullopt;
        const std::string label = std::string("the ") + l.kind;
        CHECK(made.has_value(), (label + " is made from a generic template").c_str());
        if (!made) continue;
        CHECK(name_of(made->actor) == l.name, (label + "'s UNPCInfo.CharacterName is the capture's (the nameplate's name: \"" + l.name + "\")").c_str());
        const repl::Subobject* al = find_sub(made->actor, "UAlignmentComponent");
        const repl::Value* av = al ? al->obj.state.get(*al->obj.schema, "Alignments") : nullptr;
        const auto* cont = av ? av->get_if<repl::AlignmentContainer>() : nullptr;
        bool one_friendly = cont && cont->instances.size() == 2;
        bool own = false;
        if (cont) {
            for (const auto& inst : cont->instances) {
                if (inst.key == kSelf) own = true;
                else one_friendly = one_friendly && inst.key == l.alignment;
            }
        }
        CHECK(one_friendly && own, (label + "'s alignments are the capture's: the one Friendly Blueprint alignment and its own record (a hostile Raven of the same instance lists two others)").c_str());
        const repl::Subobject* combat = find_sub(made->actor, "UNPCCombatInfo");
        CHECK(combat && rep_block_of(combat->obj) == bits_of_hex(l.combat.hex, l.combat.bits), (label + "'s CombatInfo is the capture's to the bit (bIsDead, bInvulnerable, bNotTargetable written: 36 bits)").c_str());
        const repl::Subobject* ability = find_sub(made->actor, "UAoCAbilityComponent");
        CHECK(ability && l.ability.bits == 0 && rep_block_of(ability->obj).size() == 0, (label + "'s AbilityComponent is the capture's: nothing (it fights nothing)").c_str());
    }
    // the names of the kinds the captures name (the four living opens: Raven, Spider, Tidesnapper and the Bear of 1458's packet 316)
    const std::pair<const char*, const char*> known[] = {{"Hunting_Raven", "Raven"}, {"Hunting_Spider", "Spider"}, {"Hunting_Tidesnapper", "Tidesnapper"}, {"Hunting_Bear", "Bear"}, {"Hunting_PallasCat", "Pallas Cat"}};
    for (const auto& [kind, want] : known) {
        std::string got;
        for (const auto& [def, k] : w.g->hunting()) {
            if (k.name == kind) got = w.hunting_names.count(def) ? w.hunting_names.at(def) : std::string("(none)");
        }
        CHECK(got == want, (std::string(kind) + " is named \"" + want + "\" (got \"" + got + "\")").c_str());
    }
    CHECK(w.hunting_names.size() == w.hunting_defs.size(), "every hunting def has a nameplate name");
}
// (C-Y-B ~ CoA)

// ---- the normal Timid Grem (retest 3) -----------------------------------------------------------------------------------------------------

/// The alignment keys of an actor besides its own instigator record, in order.
static std::vector<uint64_t> alignment_keys(const repl::Actor& a) {
    std::vector<uint64_t> out;
    const repl::Subobject* al = find_sub(a, "UAlignmentComponent");
    const repl::Value* av = al ? al->obj.state.get(*al->obj.schema, "Alignments") : nullptr;
    const auto* cont = av ? av->get_if<repl::AlignmentContainer>() : nullptr;
    if (cont) {
        for (const auto& inst : cont->instances) {
            if (inst.key != 6064630243140894720ull) out.push_back(inst.key);   // (a creature's own instigator record)
        }
    }
    return out;
}

static void test_timid_grem(World& w) {
    std::printf("a normal Timid Grem (Rabid_Timid) against capture 623's hostile Rabid: the same creature to the client, and its data says it never notices a player\n");
    // No retail capture opened a Timid creature (0 of 15,004 NPC opens in the 1,562 captures carry a Timid asset or one of the 59 instances of the Timid sets).  The Rabid of capture 623 is
    // the nearest: the same family, and the Blueprint table gives Rabid_Timid the same two alignments.
    const content::NpcAssetRow* timid = w.asset("Pop_TimidGrem_RVR");
    const content::NpcAssetRow* hunting = w.asset("Hunting_Rabid");
    const content::NpcAssetRow* normal = w.asset("Pop_Rabid_RVR");
    CHECK(timid && hunting && normal && timid->archetype_path.find("Rabid_Timid") != std::string::npos, "the assets are in the data: Pop_TimidGrem_RVR is the Rabid_Timid Blueprint");
    if (!timid || !hunting || !normal) return;
    const ch::Hostile& rabid = ch::kHostile[0];
    spawns::GenericNpcs on(*w.captured, &w.motion, w.config(true));
    spawns::InstanceParams p;
    p.server_id = kServerId;
    p.level = 2;
    std::string why;
    const spawns::OpenTemplate* tt = on.template_for(*timid);
    const spawns::OpenTemplate* ht = on.template_for(*hunting);
    const spawns::OpenTemplate* nt = on.template_for(*normal);
    auto timid_actor = tt ? spawns::instantiate(*tt, [&] { return w.mint(); }, p, why) : std::nullopt;
    auto hunting_actor = ht ? spawns::instantiate(*ht, [&] { return w.mint(); }, p, why) : std::nullopt;
    CHECK(timid_actor.has_value() && hunting_actor.has_value() && nt, "a Timid Grem and the huntable Grem are made from their templates");
    if (!timid_actor || !hunting_actor || !nt) return;

    // what the client reads to draw the nameplate: the alignments (and the CombatInfo), as the hostile Rabid of the capture has them
    const std::vector<uint64_t> want(rabid.alignments, rabid.alignments + rabid.alignment_count);
    const std::vector<uint64_t> got = alignment_keys(timid_actor->actor);
    CHECK(got == want, "the Timid Grem lists the capture's Rabid alignments, in order: Rabid and NPC_PlayerEnemy (6064631663140143148, 6064629454762803230)");
    const std::vector<uint64_t> hunting_keys = alignment_keys(hunting_actor->actor);
    CHECK(hunting_keys.size() == 1 && hunting_keys[0] == 6064632310208593979ull && got != hunting_keys, "the huntable Grem's one Friendly alignment (HuntingCreature) did not leak onto the Timid Grem");
    const repl::Subobject* combat = find_sub(timid_actor->actor, "UNPCCombatInfo");
    CHECK(combat && rep_block_of(combat->obj) == bits_of_hex(rabid.combat.hex, rabid.combat.bits), "its CombatInfo is the hostile Rabid's to the bit (18 bits), not the living huntable's 36");
    const repl::Subobject* ability = find_sub(timid_actor->actor, "UAoCAbilityComponent");
    const repl::Subobject* hunting_ability = find_sub(hunting_actor->actor, "UAoCAbilityComponent");
    CHECK(ability && hunting_ability && rep_block_of(hunting_ability->obj).size() == 0, "the huntable's AbilityComponent is empty (the Timid Grem keeps its template's)");

    // who the data and the alignments make hostile: the relation table, as the combat world reads it
    std::string error;
    const auto rows = w.db->combat_rows(error);
    const bootstrap::StatContext ctx = w.world->stat_context();
    auto data = rows ? combat::CombatData::build(*rows, *ctx.bundle, error) : std::nullopt;
    CHECK(data.has_value(), "the combat data builds");
    if (!data) return;
    const std::vector<int64_t> timid_al(got.begin(), got.end()), hunting_al(hunting_keys.begin(), hunting_keys.end());
    const bool timid_hostile = data->relation(timid_al, combat::CombatData::player_alignments()) == combat::Relation::Hostile;
    const bool hunting_hostile = data->relation(hunting_al, combat::CombatData::player_alignments()) == combat::Relation::Hostile;
    CHECK(timid_hostile && !hunting_hostile, "the relation table: a Timid Grem is Hostile to players (NPC_PlayerEnemy), the huntable Grem is not");

    // the data: every instance of the Timid Grem set overrides the aggro radius with 0
    int64_t timid_set = 0;
    for (const auto& [set, members] : w.pop.asset_sets) {
        for (const auto& [asset, weight] : members) {
            if (asset == timid->guid) timid_set = set;
        }
    }
    size_t instances = 0, off = 0, plain_instances = 0, plain_off = 0, radius_instances = 0, radius_ok = 0;
    for (const auto& [guid, inst] : w.pop.instances) {
        const bool in_timid = std::find(inst.asset_sets.begin(), inst.asset_sets.end(), timid_set) != inst.asset_sets.end();
        spawns::DataSpawn s;
        s.x = 1000;
        s.y = 2000;
        s.z = 3000;
        s.level = 2;
        s.instance = &inst;
        s.open = in_timid ? tt : nt;
        s.asset = in_timid ? timid : normal;
        if (!in_timid && !((!inst.override_aggro && plain_instances < 20) || (inst.override_aggro && inst.aggro_radius > 0 && radius_instances < 20))) continue;
        auto actor = spawns::make_data_actor(s, [&] { return w.mint(); }, kServerId, 0.0, nullptr, why);
        if (!actor) continue;
        if (in_timid) {
            ++instances;
            off += actor->npc.aggro_off && actor->npc.aggro_radius_cm == 0 && inst.override_aggro && inst.aggro_radius == 0 ? 1 : 0;
        } else if (!inst.override_aggro) {
            ++plain_instances;
            plain_off += actor->npc.aggro_off ? 1 : 0;
        } else {
            ++radius_instances;
            radius_ok += !actor->npc.aggro_off && actor->npc.aggro_radius_cm == inst.aggro_radius ? 1 : 0;
        }
    }
    CHECK(instances >= 31 && off == instances, "all instances of the Timid Grem set (31) override the aggro radius with 0, and each makes an actor with aggro_off");
    CHECK(plain_instances >= 5 && plain_off == 0, "an instance with no override keeps the Blueprint's radius (no aggro_off): the ordinary Rabid still aggros");
    CHECK(radius_instances >= 5 && radius_ok == radius_instances, "an instance that overrides with a radius keeps it, and is not switched off");
// [C.O.A x C-Y-B]

    // who may start a fight: a hostile creature whose data does not switch it off, nothing else
    world::NpcInfo off_info, on_info;
    off_info.aggro_off = true;
    CHECK(!combat::notices_players(timid_hostile, off_info) && combat::notices_players(timid_hostile, on_info) && !combat::notices_players(hunting_hostile, on_info),
          "a Timid Grem (aggro off) and a huntable (friendly) never notice a player; a hostile creature with an aggro radius does");
    npc::ai::Params attack_params, calm_params;
    calm_params.proximity_aggro = false;
    npc::ai::Sense sense;
    sense.now = 1.0;
    sense.self = {0, 0, 0};
    sense.home = {0, 0, 0};
    npc::ai::Candidate player;
    player.id = 1;
    player.pos = {100, 0, 0};
    player.hostile = true;
    sense.known.push_back(player);
    npc::ai::NpcAi aggressive(attack_params), timid_ai(calm_params);
    const npc::ai::Orders a1 = aggressive.think(sense);
    npc::ai::Orders a2;
    for (int i = 0; i < 40; ++i) {   // ten seconds of a player one metre away
        sense.now = 1.0 + 0.25 * i;
        a2 = timid_ai.think(sense);
        if (a2.began_fight || a2.attack) break;
    }
    CHECK(a1.began_fight, "(an NPC with proximity aggro starts the fight at once at 1 m)");
    CHECK(!a2.began_fight && !a2.attack && timid_ai.state() == npc::ai::State::Idle, "a Timid Grem's AI stays idle for ten seconds with a player standing one metre away");
    timid_ai.notice(1, 50.0, 12.0);
    sense.now = 12.0;
    const npc::ai::Orders a3 = timid_ai.think(sense);
    CHECK(a3.began_fight, "...and fights back once the player hurts it");
}

// ---- a huntable stands -------------------------------------------------------------------------------------------------------------------

static void test_stands(World& w) {
    std::printf("a huntable creature stands: no wander, no patrol\n");
    const content::NpcAssetRow* wolf = w.asset("Hunting_Wolf");
    const content::NpcAssetRow* plain = w.asset("Pop_Wolf");
    if (!wolf || !plain) {
        CHECK(false, "the assets are in the data");
        return;
    }
    spawns::GenericNpcs on(*w.captured, &w.motion, w.config(true));
    spawns::InstanceParams p;
    p.server_id = kServerId;
    std::string why;
    const spawns::OpenTemplate* hw = on.template_for(*wolf);
    const spawns::OpenTemplate* pw = on.template_for(*plain);
    auto hunt = hw ? spawns::instantiate(*hw, [&] { return w.mint(); }, p, why) : std::nullopt;
    auto pl = pw ? spawns::instantiate(*pw, [&] { return w.mint(); }, p, why) : std::nullopt;
    if (!hunt || !pl) {
        CHECK(false, "both creatures are made");
        return;
    }
    content::NpcInstanceRow inst;
    inst.ambient_behavior = "Default";   // the instance of retail's Pop_Raven, Pop_Hunting_Bear: Default = a wander
    npc::SpeedModel standing;
    standing.standing_defs = &w.hunting_defs;
    CHECK(npc::make_setup(*hunt, w.motion, &inst, nullptr, npc::SpeedModel{}).ambient == npc::Ambient::Wander, "(without the rule an instance of Default wanders)");
    CHECK(npc::make_setup(*hunt, w.motion, &inst, nullptr, standing).ambient == npc::Ambient::Stationary, "a huntable creature of that instance stands");
    inst.ambient_behavior = "Patrol";
    CHECK(npc::make_setup(*hunt, w.motion, &inst, nullptr, standing).ambient == npc::Ambient::Stationary, "and a huntable of a Patrol instance stands too");
    inst.ambient_behavior = "Default";
    CHECK(npc::make_setup(*pl, w.motion, &inst, nullptr, standing).ambient == npc::Ambient::Wander, "a plain Wolf of the same instance still wanders");
}

// ---- the data ----------------------------------------------------------------------------------------------------------------------------
// == C-O-A | Cyb ==

static void test_data(World& w) {
    std::printf("the data: the 53 hunting interactions\n");
    const auto& kinds = w.g->hunting();
    CHECK(kinds.size() == 53, "53 hunting kinds");
    size_t tier[5] = {0, 0, 0, 0, 0}, bow = 0, fifteen = 0, reach = 0, loaded = 0, nothing = 0;
    std::set<std::string> nothing_names;
    for (const auto& [def, k] : kinds) {
        const content::GatherDefRow& row = *k.row;
        if (row.cert_tier >= 0 && row.cert_tier <= 4) ++tier[row.cert_tier];
        bow += row.tool_slot == 38 && row.profession_guid == content::kProfessionHunting ? 1 : 0;
        fifteen += row.base_duration == 15.0 ? 1 : 0;
        reach += row.interact_radius == 200.0 && row.interact_tolerance == 30.0 && gather::reach_cm(row) == 330.0 ? 1 : 0;
        loaded += w.g->interaction(row.guid) == &row ? 1 : 0;
        if (k.gives_nothing) {
            ++nothing;
            nothing_names.insert(k.name);
        }
    }
    CHECK(tier[0] == 13 && tier[1] == 22 && tier[2] == 18, "Novice 13, Apprentice 22, Journeyman 18");
    CHECK(bow == 53 && fifteen == 53, "all use the Bow (slot 38) of the Hunting profession, 15 s base");
    CHECK(reach == 53, "radius 200 cm and tolerance 30 cm: a start reaches 330 cm (with the leeway)");
    CHECK(loaded == 53, "each is a gather interaction Gathering holds");
    char msg[200];
    std::snprintf(msg, sizeof msg, "%zu kinds give nothing: the RETIRED_ and NOTUSED_ items (Ebon Omen, Kirin, Midnight Mare, Obsidian Stallion, Plumed Haleberder, Runic Steed, Terrapin), an empty table (Shell), a weight of 0 (Wyrmling), "
                  "predicates on the creature's wounds (the CanTame prototype)", nothing);
    CHECK(nothing == 10 && nothing_names.count("Hunting_ShellOfTheAncients") && nothing_names.count("Hunting_Kirin") && nothing_names.count("Hunting_MidnightMare") && nothing_names.count("Hunting_Wyrmling") &&
              nothing_names.count("Hunting_Grem_Prototype_CanTame"),
          msg);
    std::snprintf(msg, sizeof msg, "the data's NumberOfSubtablesToSelect of 2 is read as 1 for the %zu roots with two subtables (a carcass or a trainable)", w.g->hunting_one_of());
    CHECK(w.g->hunting_one_of() >= 10 && w.g->hunting_one_of() <= 30, msg);
    for (const char* name : {"Hunting_Wolf", "Hunting_Raven", "Hunting_Spider", "Hunting_Rabid", "Hunting_Bear", "Hunting_Horse"}) {
        bool found = false;
        for (const auto& [def, k] : kinds) found = found || (k.name == name && !k.gives_nothing && k.row->cert_tier == 0);
        CHECK(found, (std::string(name) + " is a Novice kind that gives items").c_str());
    }
    // the Riverlands' 17 kinds all give something
    const char* riverlands[] = {"Hunting_Wolf", "Hunting_Bear", "Hunting_Rabid", "Hunting_SilkwormLarvae", "Hunting_Otter", "Hunting_Horse", "Hunting_Raven", "Hunting_Spider", "Hunting_Bullywog",
                                "Hunting_PallasCat", "Hunting_Gryphon", "Hunting_Deer", "Hunting_Bork", "Hunting_Tidesnapper", "Hunting_Skinwalker", "Hunting_Swiftclaw", "Hunting_TerrorBird"};
    size_t present = 0;
    for (const char* name : riverlands) {
        for (const auto& [def, k] : kinds) present += k.name == name && !k.gives_nothing ? 1 : 0;
    }
    CHECK(present == 17 || present == 16, "the Riverlands' kinds (17 spawn today) are all there and give items");
}

static void test_rolls(World& w) {
    std::printf("the rolls: every kind that gives, the captured items\n");
    auto owner = w.make_stats();
    std::string why;
    auto run = owner->evaluation({}, why);
    if (!run) {
        std::printf("  [FAIL] the stat run does not start: %s\n", why.c_str());
        return;
    }
    w.g->set_ratings({});
    size_t checked = 0, items_ok = 0, xp_ok = 0, adv_ok = 0;
    std::map<std::string, std::set<std::pair<uint64_t, int>>> outcomes;
    for (const auto& [def, k] : w.g->hunting()) {
        if (k.gives_nothing) continue;
        ++checked;
        stats::SplitMix rng;
        rng.state = 11 + static_cast<uint64_t>(def & 0xffff);
        bool items = true, xp = true, adv = true;
        std::set<int64_t> xps;
        size_t empty_rolls = 0;
        const double base = k.row->cert_tier == 0 ? 160.0 : k.row->cert_tier == 1 ? 200.0 : 240.0;
        for (int i = 0; i < 300; ++i) {
            const auto h = w.g->harvest(*k.row, *run, 25, world::NodeSite{}, nullptr, rng, why);
            if (!h) {
                items = false;
                break;
            }
            xps.insert(h->profession_xp * 1000 + h->primary_rarity);
            empty_rolls += h->items.empty() ? 1 : 0;
            items = items && !h->items.empty() && h->items.size() <= 2;
            for (const combat::RolledItem& it : h->items) {
                outcomes[k.name].emplace(static_cast<uint64_t>(it.item), static_cast<int>(it.quantity));
                items = items && it.quantity >= 1 && it.quantity <= 20;
            }
            // Hunter XP: the tier's base, +50 % of it for each rarity above Common [R: +160 in the captured Novice hunts]; Adventuring XP is there too
            xp = xp && h->profession_xp == static_cast<int64_t>(base * (1.0 + 0.5 * h->primary_rarity));
            adv = adv && h->adventuring_xp > 0;
        }
        if (!items || !xp || !adv) {
            std::printf("      (%s: items %d, xp %d, adventuring %d, empty rolls %zu, tier %d, xp*1000+rarity seen:", k.name.c_str(), items ? 1 : 0, xp ? 1 : 0, adv ? 1 : 0, empty_rolls, k.row->cert_tier);
            for (const int64_t v : xps) std::printf(" %lld", static_cast<long long>(v));
            std::printf(")\n");
        }
        items_ok += items ? 1 : 0;
        xp_ok += xp ? 1 : 0;
        adv_ok += adv ? 1 : 0;
    }
    char msg[160];
    std::snprintf(msg, sizeof msg, "the %zu kinds that give: 300 rolls each, every roll gives an item (1 to 20), of one subtable (one or two stacks)", checked);
    CHECK(checked == 43 && items_ok == checked, msg);
    CHECK(xp_ok == checked, "and Hunter XP of 160 (Novice), 200 (Apprentice) or 240 (Journeyman) for a Common item, +50 % of that for each better rarity: once, not once for each subtable");
    CHECK(adv_ok == checked, "and Adventuring XP");
    // the kinds that give nothing roll no item but still give experience
    size_t empty_ok = 0, empty_n = 0;
    for (const auto& [def, k] : w.g->hunting()) {
        if (!k.gives_nothing) continue;
        ++empty_n;
        stats::SplitMix rng;
        rng.state = 5;
        const auto h = w.g->harvest(*k.row, *run, 25, world::NodeSite{}, nullptr, rng, why);
        bool retired_only = h.has_value();
        if (h) {
            for (const combat::RolledItem& it : h->items) {
                std::string err;
                const auto rec = w.db->item(it.item, err);
                retired_only = retired_only && rec && (rec->name.rfind("RETIRED_", 0) == 0 || rec->name.rfind("NOTUSED_", 0) == 0);
            }
        }
        empty_ok += retired_only ? 1 : 0;
    }
    CHECK(empty_n == 10 && empty_ok == 10, "the ten that give nothing roll only retired items (or none): the session drops them");
    // the five captured hunts: the item each gave is among the rolls of its kind
    struct Cap {
        int capture;
        const char* kind;
    };
    const Cap caps[] = {{1456, "Hunting_Bear"}, {1457, "Hunting_Raven"}, {1458, "Hunting_Wolf"}, {1178, "Hunting_Ram"}, {1179, "Hunting_Hawk"}};
    for (const Cap& c : caps) {
        const gather::Gathering::HuntKind* kind = nullptr;
        for (const auto& [def, k] : w.g->hunting()) {
            if (k.name == c.kind) kind = &k;
        }
        const ch::FrameEvent* ev = nullptr;
        for (const ch::FrameEvent& e : ch::kFrameEvents) {
            if (e.capture == c.capture) ev = &e;
        }
        bool found = false;
        if (kind && ev) {
            stats::SplitMix rng;
            rng.state = 99;
            for (int i = 0; i < 400 && !found; ++i) {
                const auto h = w.g->harvest(*kind->row, *run, 25, world::NodeSite{}, nullptr, rng, why);
                for (const combat::RolledItem& it : h ? h->items : std::vector<combat::RolledItem>{}) {
                    found = found || (static_cast<uint64_t>(it.item) == ev->event.record && it.quantity == static_cast<int64_t>(ev->event.quantity));
                }
            }
        }
        char what[160];
        std::snprintf(what, sizeof what, "%d (%s): the captured item x%u is among the kind's rolls", c.capture, c.kind, ev ? ev->event.quantity : 0u);
        CHECK(found, what);
    }
    // the channel: the base 15 s through the Speed curve at rating 0 (the captured players had a rating: 13.91 s)
    const gather::Gathering::HuntKind* any = nullptr;
    for (const auto& [def, k] : w.g->hunting()) {
        if (k.name == "Hunting_Wolf") any = &k;
    }
    const auto secs = any ? w.g->channel_seconds(*any->row, *run, why) : std::nullopt;
    std::snprintf(msg, sizeof msg, "the channel at rating 0 is %.2f s (base 15 s, the captured hunts: 13.9 to 14.8 s with their ratings)", secs ? *secs : -1.0);
    CHECK(secs && *secs >= 13.5 && *secs <= 15.0, msg);
}
// C O A with C_Y_B

// ---- the corpse's wire against capture 1457 -------------------------------------------------------------------------------------------------

static const repl::Subobject* sub_of(const repl::Actor& a, const char* cls) {
    for (const repl::Subobject& s : a.subobjects) {
        if (s.obj.schema && std::string(s.obj.schema->name) == cls) return &s;
    }
    return nullptr;
}

static void test_corpse_wire(World& w) {
    std::printf("capture 1457, replayed: the corpse's start, 204 heartbeats and the done update, to the bit\n");
    const spawns::OpenTemplate* raven_tpl = w.captured->template_for(kRavenPath);
    CHECK(raven_tpl != nullptr, "the captured Raven open is the corpse's template");
    if (!raven_tpl) return;
    spawns::InstanceParams p;
    p.x = 5000;
    p.y = 6000;
    p.z = 7000;
    p.level = 5;
    p.server_id = kServerId;
    std::string why;
    auto made = spawns::instantiate(*raven_tpl, [&] { return w.mint(); }, p, why);
    CHECK(made.has_value(), "a Raven is made");
    if (!made) return;
    made->npc.name = "Raven";
    const uint64_t id = made->id;
    CHECK(w.registry().add(std::move(*made)), "it joins the registry");
    // the pre-state of 1457's done update: the corpse is dead, untargetable, with Health above 0 in its stats (the retail kill left it so; ours writes Health 0 at the death)
    w.registry().modify(id, world::Change::State, [&](world::WorldActor& a) {
        repl::Subobject* c = nullptr;
        for (repl::Subobject& s : a.actor.subobjects) {
            if (s.obj.schema && std::string(s.obj.schema->name) == "UNPCCombatInfo") c = &s;
        }
        if (c) c->obj.state.set(*c->obj.schema, "bNotTargetable", repl::Value::boolean(true));
    });

    const auto cap_start = load(ch::kRun1457Start, "UInteractableComponent");
    const repl::ElementState* statuses = cap_start ? element(*cap_start, "InputEffectStatuses", 0) : nullptr;
    const auto pawn_it = statuses ? statuses->values.find(3) : decltype(statuses->values.end()){};
    CHECK(statuses && pawn_it != statuses->values.end() && pawn_it->second.get_if<emit::FIntrepidNetworkGUID>(), "the capture's pawn reference is in the start block");
    if (!statuses || pawn_it == statuses->values.end()) return;
    gather::Channeler by;
    by.player_id = 7;
    by.character = repl::GuidValue{ch::kRuns[1].character[0], ch::kRuns[1].character[1], ch::kRuns[1].character[2], ch::kRuns[1].character[3]};
    by.pawn = *pawn_it->second.get_if<emit::FIntrepidNetworkGUID>();

    gather::NodeBoard::Config bc;
    gather::NodeBoard board(w.registry(), bc);
    const auto view_of = [&](uint64_t player) {
        const world::WorldActor* a = w.registry().find(id);
        repl::Actor v = a->actor;
        gather::show_interacting_player(*a, player, v);
        return v;
    };
    repl::ActorShadow shadow;
    std::vector<repl::ContentBlock> blocks;
    repl::Actor view = view_of(7);
    CHECK(repl::collect_open_blocks(view, shadow, blocks) && blocks.size() == 8, "the living corpse opens with its eight blocks");
    CHECK(board.begin(id, by, t0, milliseconds(13915), why), "the board starts the channel (the corpse is no gatherable)");
    CHECK(board.state_of(id) == gather::NodeState::Channeling && !gather::node_gathered(*w.registry().find(id)), "it is Channeling, and a corpse is never 'gathered' (no bGathered)");
    view = view_of(7);
    std::vector<repl::ContentBlock> update;
    CHECK(repl::collect_update_blocks(view, shadow, update) && update.size() == 1, "the start is one block: the interactable's");
    if (update.size() == 1) CHECK(payload_is(update[0], ch::kRun1457Start), "which is the capture's start, to the bit (418 bits: the status 0x08000001, the pawn, who channels)");

    size_t same = 0, one_block = 0;
    Clock::time_point now = t0;
    for (size_t i = 0; i < ch::kRun1457BeatCount; ++i) {
        now += milliseconds(67);
        if (!board.beat(id, now)) break;
        view = view_of(7);
        update.clear();
        if (!repl::collect_update_blocks(view, shadow, update)) break;
        if (update.size() != 1) continue;
        ++one_block;
        same += payload_is(update[0], ch::kRun1457Beats[i].block) ? 1 : 0;
    }
    CHECK(one_block == ch::kRun1457BeatCount, "every heartbeat is one block: the interactable's");
    CHECK(same == ch::kRun1457BeatCount, "all 204 heartbeats are the capture's, to the bit (the counter +2 each; the first, 217 bits, also takes InteractingPlayer off)");

    // the done update 36 ms after the last heartbeat: one tick on (the counter 26)
    now += milliseconds(36);
    CHECK(board.complete(id, now, seconds(3600), why), "the board completes the channel");
    view = view_of(7);
    update.clear();
    CHECK(repl::collect_update_blocks(view, shadow, update), "the done update builds");
    bool inter_ok = false, combat_ok = false;
    for (const repl::ContentBlock& b : update) {
        if (b.payload.bit_pos() == ch::kRun1457Done.bits) inter_ok = payload_is(b, ch::kRun1457Done);
        if (b.payload.bit_pos() == ch::kRun1457DoneCombat.bits) combat_ok = payload_is(b, ch::kRun1457DoneCombat);
    }
    CHECK(inter_ok, "the interactable's block is the capture's done update, to the bit (370 bits: the status 0xD0000004, the pawn, nobody channels, CanInputActivate, all four states 0)");
    CHECK(combat_ok, "the corpse's CombatInfo block is the capture's (27 bits: bIsDead 1, bNotTargetable 0)");
    const world::WorldActor* done = w.registry().find(id);
    float health = -1;
    const bool have_health = done && combat::read_stat(*done, content::kGuidHealth, health);
    CHECK(!have_health || health == 0.0f, "and Health is 0 (an item of the stats, when the actor has one)");
    CHECK(board.state_of(id) == gather::NodeState::Gathered && !gather::node_gathered(*done), "the corpse is Gathered for the board and still has no bGathered");
    update.clear();
    CHECK(repl::collect_update_blocks(view_of(7), shadow, update) && update.empty(), "nothing changes after");

    // another connection: no InteractingPlayer
    const repl::Actor other = view_of(99);
    const repl::Subobject* comp = sub_of(other, "UInteractableComponent");
    const repl::ElementState* e = comp ? element(comp->obj, "InputEffectStatuses", 0) : nullptr;
    const auto it = e ? e->values.find(3) : decltype(e->values.end()){};
    const auto* pg = e && it != e->values.end() ? it->second.get_if<emit::FIntrepidNetworkGUID>() : nullptr;
    CHECK(pg && pg->ObjectId == 0, "another player's connection sees InteractingPlayer as none");
}
// [C.O.A ~ C | Y | B]

static void test_corpse_board(World& w) {
    std::printf("the board and a corpse: a second player, a cancel, a done-counter rule\n");
    const spawns::OpenTemplate* raven_tpl = w.captured->template_for(kRavenPath);
    if (!raven_tpl) return;
    const auto make = [&]() -> uint64_t {
        spawns::InstanceParams p;
        p.x = 1;
        p.y = 2;
        p.z = 3;
        p.server_id = kServerId;
        std::string why;
        auto m = spawns::instantiate(*raven_tpl, [&] { return w.mint(); }, p, why);
        if (!m) return 0;
        const uint64_t id = m->id;
        return w.registry().add(std::move(*m)) ? id : 0;
    };
    const uint64_t a = make(), b = make();
    gather::NodeBoard board(w.registry(), gather::NodeBoard::Config{});
    gather::Channeler one, two;
    one.player_id = 1;
    one.character = {1, 2, 3, 4};
    two.player_id = 2;
    two.character = {5, 6, 7, 8};
    std::string why;
    CHECK(board.begin(a, one, t0, seconds(14), why), "player 1 starts a hunt of corpse A");
    CHECK(!board.begin(a, two, t0, seconds(14), why) && why.find("channeling") != std::string::npos, "player 2 is refused the same corpse (it is channeling)");
    CHECK(board.begin(b, two, t0, seconds(14), why), "but takes the other one");
    // the interactable before the cancel and after: back to a corpse that can be hunted
    const auto status_of_actor = [&](uint64_t id) { return gather::node_status(*w.registry().find(id)); };
    CHECK(status_of_actor(a) == 0x08000001u, "the status of a channeling corpse is 0x08000001");
    CHECK(board.cancel(a, "test"), "a cancel");
    const world::WorldActor* wa = w.registry().find(a);
    const repl::Subobject* comp = wa ? sub_of(wa->actor, "UInteractableComponent") : nullptr;
    const repl::ElementState* can = comp ? element(comp->obj, "CanInputActivate", 0) : nullptr;
    const repl::Value* can_array = comp ? comp->obj.state.get(*comp->obj.schema, "CanInputActivate") : nullptr;
    CHECK(can_array && can_array->get_if<repl::ArrayValue>() && can_array->get_if<repl::ArrayValue>()->elements.size() == 4 && can && can->values.count(1) && *can->values.at(1).get_if<bool>(),
          "CanInputActivate is four entries, the first true again (never an empty array: the client crashes on one)");
    const repl::ElementState* s0 = comp ? element(comp->obj, "CurrentInputStates", 0) : nullptr;
    const auto cur = s0 && s0->values.count(1) ? int_of(&s0->values.at(1)) : std::nullopt;
    CHECK(cur && *cur == 1 && status_of_actor(a) == 0 && !gather::node_gathered(*wa), "the state is 1 again, the status 0, no bGathered");
    CHECK(board.begin(a, two, t0 + seconds(20), seconds(14), why), "and the corpse can be hunted again");
    // the done counter: one tick after a beat that came less than 50 ms before, two after a later one
    Clock::time_point now = t0 + seconds(20);
    for (int i = 0; i < 3; ++i) {
        now += milliseconds(67);
        board.beat(a, now);
    }
    CHECK(status_of_actor(a) == (7u << 27 | 1u), "three heartbeats: the counter 1 + 2 x 3");
    CHECK(board.complete(a, now + milliseconds(40), seconds(3600), why) && status_of_actor(a) == (8u << 27 | 4u), "the done update 40 ms after a beat: one on (8), phase 4");
    Clock::time_point now2 = t0;
    for (int i = 0; i < 3; ++i) {
        now2 += milliseconds(67);
        board.beat(b, now2);
    }
    CHECK(board.complete(b, now2 + milliseconds(70), seconds(3600), why) && status_of_actor(b) == (9u << 27 | 4u), "70 ms after a beat: two on (9), phase 4");
    // a node is not a corpse: a NPC that is no huntable still cannot be begun when it has no interactable
    spawns::InstanceParams p;
    p.server_id = kServerId;
    const content::NpcAssetRow* plain = w.asset("Pop_Wolf");
    spawns::GenericNpcs off(*w.captured, &w.motion, w.config(false));
    const spawns::OpenTemplate* plain_tpl = plain ? off.template_for(*plain) : nullptr;
    auto pm = plain_tpl ? spawns::instantiate(*plain_tpl, [&] { return w.mint(); }, p, why) : std::nullopt;
    uint64_t plain_id = 0;
    if (pm) {
        plain_id = pm->id;
        w.registry().add(std::move(*pm));
    }
    CHECK(plain_id != 0 && !board.begin(plain_id, one, t0, seconds(14), why), "a creature with no interactable cannot be begun");
}

int main() {
    spdlog::set_level(spdlog::level::off);
    const char* path = db_path();
    if (!path) {
        std::printf("[skip] no content.db (AOC_CONTENT_DB, or dist/content/content.db): the hunting test needs the real file\n");
        return 0;
    }
    World w;
    if (!w.open(path)) return 1;
    test_template(w);
    test_living_open(w);
    test_timid_grem(w);
    test_stands(w);
    test_data(w);
    test_rolls(w);
    test_corpse_wire(w);
    test_corpse_board(w);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
