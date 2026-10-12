// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_mounts.cpp  (the mounts milestone, stage 2: summoning and desummoning)
//
//  The mount as a world actor and as one player's controller, on the real content.db (the working tree's dist/content/content.db, or AOC_CONTENT_DB;
//  contentc version 11) and the retail donors (spawns/mount_donors.h: captures 911, 898, 871):
//
//    donors        the three donors parse; one per kind; the mount's blocks are the NPC layout plus the mount and interactable components
//    the open      a mount made from a donor with the capture's OWN values (its owner, its pawn, its place, its guids, its item) is the capture's open but for
//                  what a server of ours leaves out or sets by rule (the road effect, MoveSpeedMult, the ground object's based movement, the capture
//                  owner's guild guid): golden for captures 911 (ground), 898 (aquatic) and 871 (flying), whose Health is Mount_Base_HP at the owner's
//                  level times the item's factor (3572, and 24375 = 4875 x 5)
//    the GUIDs     the actor's id A, the cylinder A+2, the subobjects A+4 ... A+14, A+18 (the mount component), A+20 (the interactable component):
//                  the retail pattern of 93 exports
//    archetypes    a Blueprint a capture exported keeps its retail export; any other mount (the GM Eagle Gryphon) is made from its path, checksum 0,
//                  stable between calls, and does not clash with the captured ids
//    the owner     the owner's guid is where the capture's was: the alignments, the creature-owner guid, the interaction state's maps and states
//    the book      every one of the 101 mount items makes an actor that writes an open and reads back
//    the timeline  MountController: the summon's steps at retail's offsets (+80 ms the item and the bar, +104 ms the open, the summoning flag off 0.47 s
//                  after it, the cast's end at +1.54 s), the refusals (no ground, a height difference, a lock, a dead player) with the callout, one mount per
//                  player, summon spam, the cancel by a damaging hit (before and after the open), the desummon (flags +0.1 s, the run, +3.1 s, the close at
//                  +3.2 s), a desummon during the summon, a mount that left the world, the owner's session ending
// ============================================================================
#include "bootstrap/constants.h"
#include "content/content_db.h"
#include "protocol/repl/replicator.h"
#include "protocol/schema/generated/index.h"
#include "session/mounts.h"
#include "session/player_world.h"
#include "spawns/captured_spawns.h"
#include "spawns/mount_spawn.h"
#include "world/actor_registry.h"
#include "world/interaction_state.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace aoc::worldd;
namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;
using aoc::protocol::emit::FIntrepidNetworkGUID;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

static uint64_t g_next = 0x2000000;   // the next ObjectId a test mints (even)
static FIntrepidNetworkGUID mint() {
    FIntrepidNetworkGUID g;
    g.ObjectId = g_next;
    g_next += 2;
    g.ServerId = 60;
    g.Randomizer = 0x6ee87ee4u;
    return g;
}
// C O A : C:Y:B

/// Every handle whose value differs between two states, by name.
static std::set<std::string> differing(const repl::ObjectState& a, const repl::ObjectState& b) {
    std::set<std::string> out;
    std::set<uint16_t> handles;
    for (const auto& [h, v] : a.state.values()) handles.insert(h);
    for (const auto& [h, v] : b.state.values()) handles.insert(h);
    for (const uint16_t h : handles) {
        const repl::Value* x = a.state.get(h);
        const repl::Value* y = b.state.get(h);
        if (x && y && *x == *y) continue;
        const schema::RepCommand* cmd = nullptr;
        for (const schema::RepCommand& c : a.schema->commands) {
            if (c.handle == h) cmd = &c;
        }
        out.insert(cmd ? cmd->path : "#" + std::to_string(h));
    }
    // the fast arrays: an array whose items differ
    for (const auto& [field, arr] : a.arrays.all()) {
        const repl::FastArrayState* other = b.arrays.find(field);
        std::vector<repl::ElementState> x, y;
        for (const auto& it : arr.items()) x.push_back(it.values);
        if (other) {
            for (const auto& it : other->items()) y.push_back(it.values);
        }
        if (x != y) out.insert(std::string("[") + a.schema->fields[field].name + "]");
    }
    for (const auto& [field, arr] : b.arrays.all()) {
        if (!a.arrays.find(field) && !arr.items().empty()) out.insert(std::string("[") + b.schema->fields[field].name + "]");
    }
    return out;
}

static std::string join(const std::set<std::string>& s) {
    std::string out;
    for (const std::string& x : s) out += (out.empty() ? "" : ", ") + x;
    return out.empty() ? "nothing" : out;
}

static int64_t int_of(const repl::Value* v) {
    if (!v) return 0;
    if (const auto* i = v->get_if<int64_t>()) return *i;
    if (const auto* u = v->get_if<uint64_t>()) return static_cast<int64_t>(*u);
    return 0;
}

static const repl::Subobject* sub_named(const repl::Actor& a, const char* cls) {
    for (const repl::Subobject& s : a.subobjects) {
        if (s.obj.schema && std::string(s.obj.schema->name) == cls) return &s;
    }
    return nullptr;
}

static repl::GuidValue guid_of(const repl::ObjectState& o, const char* prefix) {
    repl::GuidValue g;
    const std::string p = prefix;
    g.a = static_cast<uint32_t>(int_of(o.state.get(*o.schema, p + ".A")));
    g.b = static_cast<uint32_t>(int_of(o.state.get(*o.schema, p + ".B")));
    g.c = static_cast<uint32_t>(int_of(o.state.get(*o.schema, p + ".C")));
    g.d = static_cast<uint32_t>(int_of(o.state.get(*o.schema, p + ".D")));
    return g;
}

/// The capture's own values of a donor: what a mount made from it must be given to be that open again.
struct DonorValues {
    spawns::MountParams p;
    double health = 0;
};

static std::optional<DonorValues> values_of(const spawns::OpenTemplate& d) {
    DonorValues out;
    spawns::MountParams& p = out.p;
    const repl::ObjectState& actor = d.actor_block().state;
    const repl::Subobject* dummy = nullptr;
    (void)dummy;
    const spawns::ParsedBlock* info = nullptr;
    const spawns::ParsedBlock* second = nullptr;
    const spawns::ParsedBlock* mount = nullptr;
    const spawns::ParsedBlock* stats = nullptr;
    for (const spawns::ParsedBlock& b : d.blocks) {
        if (b.cls == "UNPCInfo") info = &b;
        if (b.cls == "UNPCSecondaryInfo") second = &b;
        if (b.cls == "UMountComponent") mount = &b;
        if (b.cls == "UAoCStatsComponent") stats = &b;
    }
    if (!info || !second || !mount || !stats) return std::nullopt;
    p.owner_guid = guid_of(second->state, "CreatureOwnerGuid");
    const repl::Value* n = info->state.state.get(*info->state.schema, "CreatureOwnerName");
    if (n && n->get_if<std::string>()) p.owner_name = *n->get_if<std::string>();
    if (const repl::Value* v = actor.state.get(*actor.schema, "Instigator")) {
        if (const auto* g = v->get_if<FIntrepidNetworkGUID>()) p.owner_pawn = *g;
    }
    p.server_id = static_cast<uint32_t>(int_of(info->state.state.get(*info->state.schema, "ServerId")));
    if (const repl::Value* v = actor.state.get(*actor.schema, "ReplicatedServerLastTransformUpdateTimeStamp")) {
        if (const float* f = v->get_if<float>()) p.world_time = *f;
    }
    p.item_guid = guid_of(mount->state, "ItemInstanceGuid");
    p.mount_guid = guid_of(info->state, "CharacterGuid");
    if (const repl::Value* v = mount->state.state.get(*mount->state.schema, "DissolveTimer")) {
        if (const float* f = v->get_if<float>()) p.dissolve_timer = *f;
    }
    if (const repl::Value* v = actor.state.get(*actor.schema, "ReplicatedMovement")) {
        if (const auto* m = v->get_if<repl::RepMovement>()) {
            const repl::Vec3d loc = repl::dequantize_location(actor.opts, m->location);
            p.x = loc.x;
            p.y = loc.y;
            p.z = loc.z;
            p.yaw_degrees = d.spawn.rotation ? d.spawn.rotation->yaw * 360.0 / 65536.0 : 0;
        }
    }
    // Health: the capture's, read from its stats
    if (const repl::FastArrayState* arr = stats->state.arrays.find(stats->state.schema->field("StatRepInt32EveryoneProxy")->index)) {
        for (const repl::FastArrayItem& it : arr->items()) {
            const auto f = it.values.values.find(1);
            const auto* t = f == it.values.values.end() ? nullptr : f->second.get_if<repl::StatInt32Inst>();
            if (t && t->record == content::kGuidMaxHealth) out.health = std::bit_cast<float>(t->value);
        }
    }
    return out;
}
// -- C.O.A & Cyb --

static bool same_chain(const repl::ExportChain& a, const repl::ExportChain& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].guid.ObjectId != b[i].guid.ObjectId || a[i].name != b[i].name || a[i].checksum != b[i].checksum || a[i].has_checksum != b[i].has_checksum) return false;
    }
    return true;
}

/// The records of the StatRepInt32EveryoneProxy items whose values differ between two stats components.
static std::set<int64_t> stat_records_differing(const repl::ObjectState& a, const repl::ObjectState& b) {
    std::set<int64_t> out;
    const auto items_of = [](const repl::ObjectState& o) {
        std::map<int64_t, repl::StatInt32Inst> m;
        const schema::NetField* nf = o.schema->field("StatRepInt32EveryoneProxy");
        const repl::FastArrayState* arr = nf ? o.arrays.find(nf->index) : nullptr;
        if (arr) {
            for (const repl::FastArrayItem& it : arr->items()) {
                const auto f1 = it.values.values.find(1);
                if (f1 != it.values.values.end()) {
                    if (const auto* t = f1->second.get_if<repl::StatInt32Inst>()) m[t->record] = *t;
                }
            }
        }
        return m;
    };
    const auto x = items_of(a), y = items_of(b);
    for (const auto& [record, t] : x) {
        const auto it = y.find(record);
        if (it == y.end() || it->second.value != t.value) out.insert(record);
    }
    for (const auto& [record, t] : y) {
        if (!x.count(record)) out.insert(record);
    }
    return out;
}

/// The effects of a CurrentEffects array that are a road's (their record guid is among the road effects of the data).
static size_t road_effects_in(const repl::ObjectState& o, const content::MountData& data, size_t* all) {
    size_t roads = 0, n = 0;
    const schema::NetField* nf = o.schema->field("CurrentEffects");
    const repl::FastArrayState* arr = nf ? o.arrays.find(nf->index) : nullptr;
    if (!arr) {
        if (all) *all = 0;
        return 0;
    }
    for (const repl::FastArrayItem& it : arr->items()) {
        ++n;
        for (const auto& [h, v] : it.values.values) {
            const auto* i = v.get_if<int64_t>();
            if (!i) continue;
            for (const content::MountEffectRow& e : data.effects) {
                if (e.kind == "road" && e.effect == *i) {
                    ++roads;
                    goto next;
                }
            }
        }
    next:;
    }
    if (all) *all = n;
    return roads;
}

/// An open's actors as the client reads them: the whole open written and the shadows' content (every block reads to its last bit) is the writer's own
/// check; here the open must write at all.
static bool writes_open(repl::Actor& a) {
    repl::ActorShadow shadow;
    repl::ExportAcks acks;
    bool failed = false;
    const auto e = repl::open_actor(a, shadow, acks, &failed);
    return e.has_value() && !failed && e->payload.bit_pos() > 0;
}
// (C Y B | C O A)

static std::string db_path() {
    const char* env = std::getenv("AOC_CONTENT_DB");
    return env && *env ? env : MOUNT_CONTENT_DB_PATH;
}

// ---- the donors and the open -------------------------------------------------------------------------------------------------------------

static void test_donors(const spawns::MountBook& book) {
    std::printf("the donors\n");
    const spawns::MountDonors& d = book.donors();
    CHECK(d.opens().size() == 3, "three donor opens (captures 911, 898, 871)");
    const spawns::OpenTemplate* ground = d.donor("ground");
    const spawns::OpenTemplate* aquatic = d.donor("aquatic");
    const spawns::OpenTemplate* flying = d.donor("flying");
    CHECK(ground && aquatic && flying && !d.donor("vehicle"), "a donor of each kind of mount, and none of another");
    CHECK(ground && ground->capture == 911 && aquatic && aquatic->capture == 898 && flying && flying->capture == 871, "the donors are the opens of 911, 898 and 871");
    bool layout = true;
    const char* expected[] = {"ANPCCharacter", "UAlignmentComponent", "UNPCSecondaryInfo", "UNPCInfo", "UNPCCombatInfo", "UAoCAbilityComponent", "UAoCStatsComponent", "UMountComponent", "UInteractableComponent"};
    for (const spawns::OpenTemplate* t : {ground, aquatic, flying}) {
        if (!t || t->blocks.size() != std::size(expected)) {
            layout = false;
            continue;
        }
        for (size_t i = 0; i < t->blocks.size(); ++i) layout = layout && t->blocks[i].cls == expected[i];
    }
    CHECK(layout, "each is the NPC layout (actor, alignment, creature info, base info, combat info, ability, stats) with the mount and the interactable component");
    CHECK(ground && ground->archetype_path.find("Mount_Grounded_TerrorBird_0") != std::string::npos && ground->archetype.size() == 2, "the ground donor's archetype is the terror bird's, with its package");
    CHECK(book.summon_ability() == 108436548376185ll && book.dash_ability() == 6064631555335526877ll, "Summon_Mount and MountAbility_Sprint are the data's");
}

struct Case {
    const char* name;
    int         capture;
    const char* kind;
    const char* blueprint;
    int         level;
    double      health;
};

static void test_open_golden(const spawns::MountBook& book) {
    std::printf("the open of a mount made from a donor with the capture's own values is the capture's open (captures 911, 898, 871)\n");
    const Case cases[] = {
        {"the terror bird of 911", 911, "ground", "Mount_Grounded_TerrorBird_0", 20, 3572},
        {"the shoalskipper of 898", 898, "aquatic", "Mount_Aquatic_Shoalskipper_1_ALT", 0, 0},
        {"the Dawnbreaker of 871", 871, "flying", "Mount_Flying_Dawnbreaker_1", 25, 24375},
    };
    // what a mount of ours leaves out or sets by rule, by block: the road effect and its MoveSpeedMult (no roads in stage 2), the ground object the mount
    // stands on (no export for it), the inherited alignments of the capture's owner (a guild, a party)
    const std::map<std::string, std::set<std::string>> allowed = {
        {"ANPCCharacter", {"ReplicatedBasedMovement.BaseID", "ReplicatedBasedMovement.MovementBase"}},
        {"UAlignmentComponent", {"InheritedAlignments", "Alignments"}},   // (the owner's, not the capturing player's: a node, a guild, 898's Combatant)
        {"UInteractableComponent", {"InteractionInputData"}},              // (the rider's AnimBP: the open exports a class of ours)
        {"UAoCStatsComponent", {"[StatRepInt32EveryoneProxy]", "[CurrentEffects]"}},
    };
    for (const Case& c : cases) {
        const spawns::OpenTemplate* donor = book.donors().donor(c.kind);
        const content::MountRow* row = nullptr;
        for (const content::MountRow& r : book.data().mounts) {
            if (r.blueprint == c.blueprint) row = &r;
        }
        std::string title = c.name;
        if (!donor || !row || donor->capture != c.capture) {
            CHECK(false, (title + ": the donor and the item are there").c_str());
            continue;
        }
        auto v = values_of(*donor);
        if (!v) {
            CHECK(false, (title + ": the donor's values read").c_str());
            continue;
        }
        spawns::MountParams p = v->p;
        // the owner's level: the one whose Mount_Base_HP times the item's factor is the capture's MaxHealth
        int level = 0;
        for (int l = 1; l <= 50; ++l) {
            if (std::fabs(book.max_health(*row, l) - v->health) < 0.5) {
                level = l;
                break;
            }
        }
        if (c.level != 0) CHECK(level == c.level, (title + ": MaxHealth " + std::to_string(static_cast<int>(v->health)) + " is Mount_Base_HP at owner level " + std::to_string(c.level) + " times the factor " + std::to_string(row->health_pct) + " %").c_str());
        else CHECK(level != 0, (title + ": the capture's MaxHealth is Mount_Base_HP at some owner level times the item's factor").c_str());
        p.owner_level = level ? level : 1;
        p.player_id = 7;
        uint64_t next = 0x3000000;
        const auto minter = [&]() {
            FIntrepidNetworkGUID g;
            g.ObjectId = next;
            next += 2;
            g.ServerId = 23;
            g.Randomizer = 0x11223344u;
            return g;
        };
        std::string error;
        auto made = book.make(*row, p, minter, error);
        if (!made) {
            CHECK(false, (title + ": the mount is made: " + error).c_str());
            continue;
        }
        const repl::Actor& a = made->actor.actor;
        bool same_blocks = a.subobjects.size() + 1 == donor->blocks.size();
        CHECK(same_blocks, (title + ": the actor has the donor's subobjects").c_str());
        // the actor's block and each subobject's, against the donor's
        std::map<std::string, std::set<std::string>> found;
        found["ANPCCharacter"] = differing(donor->actor_block().state, a.obj);
        for (size_t i = 0; same_blocks && i < a.subobjects.size(); ++i) {
            found[donor->blocks[i + 1].cls] = differing(donor->blocks[i + 1].state, a.subobjects[i].obj);
        }
        // the donor that never sent the timestamp in its open (898 sent it 49 ms later): we always set it
        if (!donor->actor_block().state.state.get(*donor->actor_block().state.schema, "ReplicatedServerLastTransformUpdateTimeStamp")) {
            found["ANPCCharacter"].erase("ReplicatedServerLastTransformUpdateTimeStamp");
        }
        bool exact = true;
        std::string report;
        for (const auto& [cls, diff] : found) {
            std::set<std::string> want;
            if (const auto it = allowed.find(cls); it != allowed.end()) want = it->second;
            for (const std::string& path : diff) {
                if (!want.count(path)) {
                    exact = false;
                    report += cls + "." + path + " ";
                }
            }
        }
        CHECK(exact, (title + ": every value is the capture's but for the road effect, the based movement and the guild (differing: " + join([&] {
                          std::set<std::string> all;
                          for (const auto& [cls, diff] : found) {
                              for (const std::string& x : diff) all.insert(cls + "." + x);
                          }
                          return all;
                      }()) + ")").c_str());
        if (!exact) std::printf("      unexpected: %s\n", report.c_str());
        // the stats: only MoveSpeedMult differs (the capture's mount stood on a road: x1.25 and Road_T3_Speed_Mount), and the effect the capture holds is a road's
        const spawns::ParsedBlock* donor_stats = nullptr;
        for (const spawns::ParsedBlock& b : donor->blocks) {
            if (b.cls == "UAoCStatsComponent") donor_stats = &b;
        }
        const repl::Subobject* our_stats = sub_named(a, "UAoCStatsComponent");
        if (donor_stats && our_stats) {
            const std::set<int64_t> stat_diff = stat_records_differing(donor_stats->state, our_stats->obj);
            const bool on_road = c.capture == 911;
            CHECK(on_road ? (stat_diff == std::set<int64_t>{spawns::kStatMoveSpeedMult}) : stat_diff.empty(),
                  (title + ": its stats are the capture's" + (on_road ? " but for MoveSpeedMult (1.25 on a road, 1 here)" : "")).c_str());
            size_t all = 0;
            const size_t roads = road_effects_in(donor_stats->state, book.data(), &all);
            size_t ours = 0;
            road_effects_in(our_stats->obj, book.data(), &ours);
            // (the capture's effects: a road's on 911; the owner's place's on the others, Lawless_Buff in 898 and Node_Factory_Default_Character in 871: worldd has no
            // zones or nodes to inherit them from)
            CHECK(all >= roads && all >= 1 && ours == 0, (title + ": the " + std::to_string(all) + " effect(s) the capture holds (" + std::to_string(roads) + " a road's, the rest the owner's place's) are none of ours: a new mount has no effect").c_str());
        }
        // the values that must be the capture's
        const repl::Subobject* mc = sub_named(a, "UMountComponent");
        CHECK(mc && guid_of(mc->obj, "ItemInstanceGuid").a == p.item_guid.a, (title + ": the mount component names the item").c_str());
        CHECK(writes_open(const_cast<repl::Actor&>(a)), (title + ": the open writes").c_str());
    }
}
// CoA - C-Y-B


// ---- the GUIDs, the archetypes, the owner, the whole book ------------------------------------------------------------------------------

static const content::MountRow* row_named(const spawns::MountBook& book, const char* item_name) {
    for (const content::MountRow& r : book.data().mounts) {
        if (r.item_name == item_name) return &r;
    }
    return nullptr;
}

static const content::MountRow* first_of_kind(const spawns::MountBook& book, const char* kind) {
    for (const content::MountRow& r : book.data().mounts) {
        if (r.kind == kind && !r.mule) return &r;
    }
    return nullptr;
}

static spawns::MountParams plain_params() {
    spawns::MountParams p;
    p.player_id = 3;
    p.owner_guid = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
    p.owner_name = "Mounty";
    p.owner_pawn = mint();
    p.owner_level = 12;
    p.server_id = 60;
    p.world_time = 1234.5;
    p.item_guid = {0xA1u, 0xA2u, 0xA3u, 0xA4u};
    p.x = -777000;
    p.y = 616000;
    p.z = 5000;
    p.yaw_degrees = 90;
    return p;
}

/// The values of the StatRepInt32EveryoneProxy items of a stats component, by record.
static std::map<int64_t, double> stat_values(const repl::Subobject* stats) {
    std::map<int64_t, double> out;
    if (!stats) return out;
    const repl::FastArrayState* arr = stats->obj.arrays.find(stats->obj.schema->field("StatRepInt32EveryoneProxy")->index);
    if (!arr) return out;
    for (const repl::FastArrayItem& it : arr->items()) {
        const auto f = it.values.values.find(1);
        const auto* t = f == it.values.values.end() ? nullptr : f->second.get_if<repl::StatInt32Inst>();
        if (t) out[t->record] = std::bit_cast<float>(t->value);
    }
    return out;
}

static void test_guids(const spawns::MountBook& book) {
    std::printf("the NetGUIDs follow the retail pattern\n");
    const content::MountRow* row = first_of_kind(book, "ground");
    spawns::MountParams p = plain_params();
    uint64_t next = 0x5000000;
    const auto minter = [&]() {
        FIntrepidNetworkGUID g;
        g.ObjectId = next;
        next += 2;
        g.ServerId = 60;
        g.Randomizer = 1;
        return g;
    };
    std::string error;
    auto made = row ? book.make(*row, p, minter, error) : std::nullopt;
    CHECK(made.has_value(), "a ground mount is made");
    if (!made) return;
    const uint64_t A = 0x5000000;
    const world::WorldActor& w = made->actor;
    CHECK(w.id == A && w.actor.guid.ObjectId == A && w.kind == world::ActorKind::Mount, "the actor's id is A, its kind Mount");
    CHECK(w.npc.collision_cylinder == A + 2, "the CollisionCylinder is A+2");
    const uint64_t want[] = {A + 4, A + 6, A + 8, A + 10, A + 12, A + 14, A + 18, A + 20};
    bool ids = w.actor.subobjects.size() == std::size(want);
    for (size_t i = 0; ids && i < std::size(want); ++i) ids = w.actor.subobjects[i].guid.ObjectId == want[i];
    CHECK(ids, "the subobjects are A+4, +6, +8, +10, +12, +14 (the stats), then A+18 (the mount component) and A+20 (the interactable component): A+16 is never exported");
    CHECK(w.npc.interact_component == A + 20 && next == A + 22, "the interactable component is A+20 and the actor used 11 ids");
    CHECK(w.never_dormant && w.owner == 3 && w.x == p.x && w.y == p.y && w.z == p.z && w.yaw_degrees == 90, "never dormant, owned by its player, at its place");
    const repl::Value* inst = w.actor.obj.state.get(*w.actor.obj.schema, "Instigator");
    CHECK(inst && inst->get_if<FIntrepidNetworkGUID>() && *inst->get_if<FIntrepidNetworkGUID>() == p.owner_pawn, "the Instigator is the owner's pawn");
    CHECK(!w.actor.obj.state.get(*w.actor.obj.schema, "Owner") && !w.actor.obj.state.get(*w.actor.obj.schema, "Controller"), "no Owner and no Controller (they are set when someone rides it)");
    const repl::Subobject* mc = sub_named(w.actor, "UMountComponent");
    const repl::Value* summoning = mc ? mc->obj.state.get(*mc->obj.schema, "bIsSummoning") : nullptr;
    CHECK(summoning && summoning->get_if<bool>() && *summoning->get_if<bool>(), "bIsSummoning is 1 at the open");
    const repl::Subobject* info = sub_named(w.actor, "UNPCInfo");
    CHECK(info && guid_of(info->obj, "CharacterGuid") == made->mount_guid && !(made->mount_guid == repl::GuidValue{}), "the mount has a CharacterGuid of its own");
    const repl::Value* name = info ? info->obj.state.get(*info->obj.schema, "CreatureOwnerName") : nullptr;
    CHECK(name && name->get_if<std::string>() && *name->get_if<std::string>() == "Mounty", "its creature owner name is the owner's");
    const double want_hp = book.max_health(*row, 12);
    CHECK(made->max_health == want_hp && want_hp > 1000, "MaxHealth is Mount_Base_HP at the owner's level times the item's factor");
    const auto stats = stat_values(sub_named(w.actor, "UAoCStatsComponent"));
    CHECK(stats.count(content::kGuidHealth) && stats.at(content::kGuidHealth) == want_hp && stats.at(content::kGuidMaxHealth) == want_hp &&
              stats.at(spawns::kStatStealthLevel) == 12 && stats.at(spawns::kStatMoveSpeedMult) == 1.0,
          "Health and MaxHealth are full, StealthLevel is the owner's level, MoveSpeedMult is 1");
    // a hurt mount (the saved Health of a mount that came back) starts at its Health
    p.health = 100;
    next = 0x6000000;
    auto hurt = book.make(*row, p, minter, error);
    const auto hs = hurt ? stat_values(sub_named(hurt->actor.actor, "UAoCStatsComponent")) : std::map<int64_t, double>{};
    CHECK(hs.count(content::kGuidHealth) && hs.at(content::kGuidHealth) == 100 && hs.at(content::kGuidMaxHealth) == want_hp, "a saved Health below the maximum is the mount's Health");
}

static void test_archetypes(const spawns::MountBook& book) {
    std::printf("the archetype of a mount: the captured export or one made from the Blueprint's path\n");
    const spawns::OpenTemplate* ground = book.donors().donor("ground");
    const content::MountRow* terror = row_named(book, "Mount_Grounded_TerrorBird_0");
    CHECK(terror && ground && same_chain(book.archetype_of(*terror, *ground), ground->archetype), "the terror bird keeps the export retail sent (its id, the checksum 47039926)");
    const content::MountRow* other = nullptr;
    for (const content::MountRow& r : book.data().mounts) {
        if (r.kind == "ground" && r.blueprint != "Mount_Grounded_TerrorBird_0") {
            other = &r;
            break;
        }
    }
    CHECK(other != nullptr, "another ground mount of the data");
    if (!other || !ground) return;
    const repl::ExportChain a1 = book.archetype_of(*other, *ground);
    const repl::ExportChain a2 = book.archetype_of(*other, *ground);
    CHECK(a1.size() == 2 && same_chain(a1, a2), "a made export is an object and its package, the same at every call (the client keeps what it acknowledged)");
    CHECK(a1.size() == 2 && a1[0].name == "Default__" + other->blueprint + "_C" && a1[1].name == "/Game/GameBlueprints/Mounts/" + other->blueprint && a1[0].has_checksum && a1[0].checksum == 0 && a1[1].checksum == 0,
          "the object is the class default object of the Blueprint by its path, in its package, with checksum 0 (not compared by the client)");
    bool clash = false;
    for (const auto& donor : book.donors().opens()) {
        for (const repl::ExportLink& l : donor.archetype) clash = clash || (l.guid.ObjectId == a1[0].guid.ObjectId || l.guid.ObjectId == a1[1].guid.ObjectId);
    }
    CHECK(!clash && a1.size() == 2 && (a1[0].guid.ObjectId & 1) == 1, "its NetGUIDs are static ids (odd) that are none of the captured ones");
    const content::MountRow* gryphon = row_named(book, "Mount_Flying_PTR_ONLY");
    const spawns::OpenTemplate* flying = book.donors().donor("flying");
    CHECK(gryphon && flying && gryphon->blueprint == "Mount_Flying_Gryphon_5_Armored", "the GM Eagle Gryphon is the Armored gryphon's Blueprint");
    if (gryphon && flying) {
        const repl::ExportChain g = book.archetype_of(*gryphon, *flying);
        CHECK(g.size() == 2 && g[0].name == "Default__Mount_Flying_Gryphon_5_Armored_C" && !same_chain(g, flying->archetype), "it opens with the flying donor's shape and an export made from its path");
        spawns::MountParams p = plain_params();
        std::string error;
        auto made = book.make(*gryphon, p, mint, error);
        CHECK(made && same_chain(made->actor.actor.open.archetype, g) && sub_named(made->actor.actor, "UMountComponent") != nullptr, "the Eagle Gryphon makes an actor with that export");
    }
}
// [CoA - C.Y.B]

static void test_owner(const spawns::MountBook& book) {
    std::printf("the owner is where the capture's was\n");
    const spawns::OpenTemplate* donor = book.donors().donor("ground");
    const content::MountRow* row = first_of_kind(book, "ground");
    if (!donor || !row) return;
    const auto v = values_of(*donor);
    spawns::MountParams p = plain_params();
    std::string error;
    auto made = book.make(*row, p, mint, error);
    if (!v || !made) {
        CHECK(false, "a mount is made");
        return;
    }
    const repl::GuidValue capture_owner = v->p.owner_guid, capture_mount = *v->p.mount_guid;
    size_t left_owner = 0, left_mount = 0, ours = 0;
    for (const repl::Subobject& s : made->actor.actor.subobjects) {
        repl::ObjectState copy = s.obj;
        left_owner += spawns::replace_guid(copy, capture_owner, repl::GuidValue{9, 9, 9, 9});
        left_mount += spawns::replace_guid(copy, capture_mount, repl::GuidValue{9, 9, 9, 9});
        repl::ObjectState again = s.obj;
        ours += spawns::replace_guid(again, p.owner_guid, repl::GuidValue{9, 9, 9, 9});
    }
    CHECK(left_owner == 0 && left_mount == 0, "the capture's owner and mount guids stand nowhere (alignments, interaction states)");
    CHECK(ours >= 5, "the owner's guid stands in the alignments and the interaction state (two maps and the input state)");
    const repl::Subobject* al = sub_named(made->actor.actor, "UAlignmentComponent");
    const repl::Value* av = al ? al->obj.state.get(*al->obj.schema, "Alignments") : nullptr;
    const auto* c = av ? av->get_if<repl::AlignmentContainer>() : nullptr;
    bool both = false;
    if (c) {
        for (const auto& inst : c->instances) {
            bool m = false, o = false;
            for (const repl::GuidValue& g : inst.guids) {
                m = m || g == made->mount_guid;
                o = o || g == p.owner_guid;
            }
            both = both || (m && o);
        }
    }
    CHECK(both, "the Alignments hold the mount and its owner in one instance");
    repl::ObjectState o = made->actor.actor.subobjects[0].obj;
    CHECK(spawns::replace_guid(o, p.owner_guid, p.owner_guid) == 0 && spawns::replace_guid(o, repl::GuidValue{5, 6, 7, 8}, repl::GuidValue{1, 2, 3, 4}) == 0,
          "replacing a guid by itself, or one that is not there, changes nothing");
}

static void test_book(const spawns::MountBook& book) {
    std::printf("every mount item makes an actor that opens\n");
    size_t made_n = 0, opens = 0, wrong_kind = 0;
    std::set<uint64_t> archetype_ids;
    std::map<std::string, size_t> by_kind;
    for (const content::MountRow& row : book.data().mounts) {
        spawns::MountParams p = plain_params();
        p.owner_level = 25;
        std::string error;
        auto made = book.make(row, p, mint, error);
        if (!made) {
            std::printf("    %s: %s\n", row.item_name.c_str(), error.c_str());
            continue;
        }
        ++made_n;
        ++by_kind[row.kind];
        if (writes_open(made->actor.actor)) ++opens;
        const repl::Subobject* mc = sub_named(made->actor.actor, "UMountComponent");
        const std::string want_name = row.kind == "ground" ? "GroundMountComponent" : row.kind == "flying" ? "FlyingMountComponent" : "AquaticMountComponent";
        if (!mc || mc->name != want_name) ++wrong_kind;
        archetype_ids.insert(made->actor.actor.open.archetype[0].guid.ObjectId);
        if (made->max_health != book.max_health(row, 25)) ++wrong_kind;
    }
    CHECK(made_n == book.data().mounts.size() && made_n == 101, "all 101 mount items make an actor");
    CHECK(opens == made_n, "every actor's open writes");
    CHECK(wrong_kind == 0, "each has the mount component of its kind (Ground, Flying, Aquatic) and its MaxHealth");
    CHECK(by_kind["ground"] == 51 && by_kind["flying"] == 46 && by_kind["aquatic"] == 4, "51 ground, 46 flying and 4 aquatic mounts");
    std::set<std::string> blueprints;
    for (const content::MountRow& row : book.data().mounts) blueprints.insert(row.blueprint);
    CHECK(archetype_ids.size() == blueprints.size(), "one archetype export per Blueprint (items that share a Blueprint share it)");
}


// ---- stage 3 fix 2: the owner's alignments (no PvP flag), the rider's AnimBP --------------------------------------------------------------------
// built by C-Y-B with C O A

static const repl::AlignmentContainer* alignments_in(const repl::ObjectState& o, const char* field) {
    const repl::Value* v = o.state.get(*o.schema, field);
    return v ? v->get_if<repl::AlignmentContainer>() : nullptr;
}

static const repl::AlignmentContainer::Instance* instance_of_key(const repl::AlignmentContainer& c, uint64_t key) {
    for (const auto& i : c.instances) {
        if (i.key == key) return &i;
    }
    return nullptr;
}

static std::vector<uint64_t> keys_of(const repl::AlignmentContainer& c) {
    std::vector<uint64_t> out;
    for (const auto& i : c.instances) out.push_back(i.key);
    return out;
}

static bool flag_off(const repl::ObjectState& o, const char* path) {
    const repl::Value* v = o.state.get(*o.schema, path);
    if (!v) return true;
    if (const auto* b = v->get_if<bool>()) return !*b;
    return int_of(v) == 0;
}

static const content::MountRow* row_of_blueprint(const spawns::MountBook& book, const char* blueprint) {
    for (const content::MountRow& r : book.data().mounts) {
        if (r.blueprint == blueprint) return &r;
    }
    return nullptr;
}

/// A mount of `row` made from the donor of its kind, owned by `p`'s player.
static std::optional<spawns::MountMade> make_plain(const spawns::MountBook& book, const content::MountRow& row, const spawns::MountParams& p, std::string& error) {
    uint64_t next = 0x6000000;
    const auto minter = [&]() {
        FIntrepidNetworkGUID g;
        g.ObjectId = next;
        next += 2;
        g.ServerId = 60;
        g.Randomizer = 0x6ee87ee4u;
        return g;
    };
    return book.make(row, p, minter, error);
}

static void test_alignments_are_the_owners(const spawns::MountBook& book) {
    std::printf("a mount's alignments are its owner's, never a donor's (stage 3 fix 2: the PvP flag)\n");
    constexpr uint64_t kCombatant = 6064629443707731976ull, kNodeCitizen = 6064631051897143296ull, kGuildMember = 6064630560782155776ull;
    // the evidence: what the donors' owners were.  898's stood in a Lawless zone as a Combatant (the PvP flag); 911's and 871's had a node and a guild
    const auto donor_inherited = [&](const char* kind) -> const repl::AlignmentContainer* {
        const spawns::OpenTemplate* d = book.donors().donor(kind);
        const repl::ObjectState* al = nullptr;
        if (d) {
            for (const spawns::ParsedBlock& b : d->blocks) {
                if (b.cls == "UAlignmentComponent") al = &b.state;
            }
        }
        return al ? alignments_in(*al, "InheritedAlignments") : nullptr;
    };
    const repl::AlignmentContainer* aq = donor_inherited("aquatic");
    const repl::AlignmentContainer* gr = donor_inherited("ground");
    const repl::AlignmentContainer* fl = donor_inherited("flying");
    CHECK(aq && instance_of_key(*aq, kCombatant), "(the aquatic donor's owner, capture 898, was a Combatant: flagged for PvP)");
    CHECK(gr && fl && instance_of_key(*gr, kNodeCitizen) && instance_of_key(*gr, kGuildMember) && instance_of_key(*fl, kNodeCitizen) && instance_of_key(*fl, kGuildMember) &&
              !instance_of_key(*gr, kCombatant) && !instance_of_key(*fl, kCombatant),
          "(the ground and flying donors' owners, 911 and 871, had a node and a guild)");
// -- CoA x C | Y | B --

    const char* blueprints[] = {"Mount_Grounded_TerrorBird_0", "Mount_Aquatic_Shoalskipper_1_ALT", "Mount_Flying_Dawnbreaker_1", "Mount_Grounded_Scorpix_1", "Mount_Grounded_VerdantVoyager_0",
                                "Mount_Flying_Gryphon_5_Armored"};
    for (const char* name : blueprints) {
        const content::MountRow* row = row_of_blueprint(book, name);
        if (!row) {
            CHECK(false, (std::string(name) + ": the item is in the data").c_str());
            continue;
        }
        const std::string title = name;
        spawns::MountParams p = plain_params();
        std::string error;
        auto made = make_plain(book, *row, p, error);
        if (!made) {
            CHECK(false, (title + ": the mount is made: " + error).c_str());
            continue;
        }
        const repl::Subobject* al = sub_named(made->actor.actor, "UAlignmentComponent");
        const repl::AlignmentContainer* inherited = al ? alignments_in(al->obj, "InheritedAlignments") : nullptr;
        const repl::AlignmentContainer* own = al ? alignments_in(al->obj, "Alignments") : nullptr;
        // an unflagged player with no node and no guild: Player, NonCombatant, its own Instigator and PVPSuppressed_Perceiver, the alignments of worldd's pawn
        const std::vector<uint64_t> want = {bootstrap::kAlignmentKeyA, bootstrap::kAlignmentKeyB, bootstrap::kAlignmentKeyCharacter, bootstrap::kAlignmentKeyC};
        CHECK(inherited && keys_of(*inherited) == want, (title + ": it inherits Player, NonCombatant, the owner's Instigator and PVPSuppressed_Perceiver, and nothing else").c_str());
        CHECK(inherited && !instance_of_key(*inherited, kCombatant) && !instance_of_key(*inherited, kNodeCitizen) && !instance_of_key(*inherited, kGuildMember),
              (title + ": no Combatant (the PvP flag), no node and no guild").c_str());
        const repl::AlignmentContainer::Instance* self = inherited ? instance_of_key(*inherited, bootstrap::kAlignmentKeyCharacter) : nullptr;
        CHECK(self && self->guids.size() == 1 && self->guids[0] == p.owner_guid, (title + ": its Instigator group is the owner").c_str());
        bool zeros = inherited != nullptr;
        for (const uint64_t k : {bootstrap::kAlignmentKeyA, bootstrap::kAlignmentKeyB, bootstrap::kAlignmentKeyC}) {
            const auto* i = inherited ? instance_of_key(*inherited, k) : nullptr;
            zeros = zeros && i && i->guids.size() == 1 && i->guids[0] == repl::GuidValue{};
        }
        CHECK(zeros, (title + ": the groups of Player, NonCombatant and PVPSuppressed_Perceiver are the zero group, as the owner's").c_str());
        CHECK(own && own->instances.size() == 1 && own->instances[0].key == bootstrap::kAlignmentKeyCharacter && own->instances[0].guids.size() == 2 &&
                  own->instances[0].guids[0] == made->mount_guid && own->instances[0].guids[1] == p.owner_guid && own->timers.empty() && inherited && inherited->timers.empty(),
              (title + ": its own Alignments are one Instigator group, the mount and its owner [R: 911, 898, 871, 872]").c_str());
        // the combat state: no PvP flag, no PvP combat, no suppression area, whatever the donor's owner was
        const repl::Subobject* ci = sub_named(made->actor.actor, "UNPCCombatInfo");
        CHECK(ci && flag_off(ci->obj, "bPvPForceFlag") && flag_off(ci->obj, "bInPVPCombat") && flag_off(ci->obj, "InPvpSuppressionArea") && flag_off(ci->obj, "bInCombat"),
              (title + ": its combat info holds no PvP flag, no PvP combat and no suppression area").c_str());
        const repl::Subobject* stats = sub_named(made->actor.actor, "UAoCStatsComponent");
        bool pvp_effect = false;
        if (stats) {
            const schema::NetField* nf = stats->obj.schema->field("CurrentEffects");
            const repl::FastArrayState* arr = nf ? stats->obj.arrays.find(nf->index) : nullptr;
            if (arr) {
                for (const repl::FastArrayItem& it : arr->items()) {
                    for (const auto& [h, v] : it.values.values) {
                        if (const auto* b = v.get_if<bool>()) pvp_effect = pvp_effect || *b;
                    }
                }
            }
        }
        CHECK(stats && !pvp_effect, (title + ": it holds no effect of a flagged source (a new mount has no effect)").c_str());
    }

    // an owner that IS flagged hands its flag on, and exactly its own alignments, nothing of a donor's
    {
        const content::MountRow* row = row_of_blueprint(book, "Mount_Grounded_TerrorBird_0");
        spawns::MountParams p = plain_params();
        repl::AlignmentContainer flagged;
        const repl::GuidValue none;
        flagged.instances = {{bootstrap::kAlignmentKeyA, {none}}, {kCombatant, {repl::GuidValue{9, 8, 7, 6}}}, {bootstrap::kAlignmentKeyCharacter, {p.owner_guid}}};
        p.owner_alignments = flagged;
        std::string error;
        auto made = row ? make_plain(book, *row, p, error) : std::nullopt;
        const repl::Subobject* al = made ? sub_named(made->actor.actor, "UAlignmentComponent") : nullptr;
        const repl::AlignmentContainer* inherited = al ? alignments_in(al->obj, "InheritedAlignments") : nullptr;
        CHECK(inherited && *inherited == flagged, "a flagged owner's alignments (a Combatant) are inherited exactly: the owner's state, not the donor's");
    }
}

static void test_rider_anim(spawns::MountBook& book) {
    std::printf("the rider's AnimBP: named in the mount's interaction data and exported with its open (stage 3 fix 2: the rider sat nowhere)\n");
    // without the Blueprints' file a mount names the AnimBP of its kind's retail donor (captures 911, 898, 871)
    {
        struct K {
            const char* blueprint;
            const char* want;
        } fallbacks[] = {{"Mount_Grounded_Scorpix_1", "/Game/Characters/Animations/Mounts_Rider/Swiftclaw/ABP_PC_MNT_Swiftclaw.ABP_PC_MNT_Swiftclaw_C"},
                         {"Mount_Grounded_VerdantVoyager_0", "/Game/Characters/Animations/Mounts_Rider/Tidesnapper/ABP_PC_MNT_Tidesnapper.ABP_PC_MNT_Tidesnapper_C"},
                         {"Mount_Flying_Gryphon_5_Armored", "/Game/Characters/Animations/Mounts_Rider/Dawnbreaker/ABP_PC_MNT_Dawnbreaker.ABP_PC_MNT_Dawnbreaker_C"}};
        std::string error;
        auto opened = content::ContentDb::open(db_path(), error);
        auto fresh = opened ? spawns::MountBook::load(*opened, error) : std::nullopt;
        bool ok = fresh.has_value() && fresh->riders_known() == 0;
        for (const K& k : fallbacks) {
            const content::MountRow* row = fresh ? row_of_blueprint(*fresh, k.blueprint) : nullptr;
            ok = ok && row && fresh->rider_anim_of(*row) == k.want;
        }
        CHECK(ok, "with no riders file a mount names its kind's donor AnimBP (ground Swiftclaw, aquatic Tidesnapper, flying Dawnbreaker)");
        std::string bad;
        CHECK(fresh && !fresh->load_riders("no_such_file.tsv", bad) && !bad.empty(), "a missing riders file is an error, not a crash");
    }
    std::string error;
    CHECK(book.load_riders(MOUNT_RIDERS_PATH, error), ("dist/content/mount_riders.tsv loads (tools/mapc mount-speeds): " + error).c_str());
    CHECK(book.riders_known() > 100, "it names the AnimBP of over a hundred Blueprints");
// Cyb x C O A

    // each Blueprint's own AnimBP; the three donors' Blueprints name the AnimBP their captures exported (911 Swiftclaw, 898 Tidesnapper, 871 Dawnbreaker)
    struct Case {
        const char* blueprint;
        const char* klass;
        const char* package;
    } cases[] = {
        {"Mount_Grounded_TerrorBird_0", "ABP_PC_MNT_Swiftclaw_C", "/Game/Characters/Animations/Mounts_Rider/Swiftclaw/ABP_PC_MNT_Swiftclaw"},
        {"Mount_Aquatic_Shoalskipper_1_ALT", "ABP_PC_MNT_Tidesnapper_C", "/Game/Characters/Animations/Mounts_Rider/Tidesnapper/ABP_PC_MNT_Tidesnapper"},
        {"Mount_Flying_Dawnbreaker_1", "ABP_PC_MNT_Dawnbreaker_C", "/Game/Characters/Animations/Mounts_Rider/Dawnbreaker/ABP_PC_MNT_Dawnbreaker"},
        {"Mount_Grounded_Scorpix_1", "ABP_PC_MNT_VerdantVoyager_C", "/Game/Characters/Animations/Mounts_Rider/VerdantVoyager/ABP_PC_MNT_VerdantVoyager"},
        {"Mount_Grounded_Horse_0", "ABP_PC_MNT_Horse_C", "/Game/Characters/Animations/Mounts_Rider/Horse/ABP_PC_MNT_Horse"},
        {"Mount_Flying_Gryphon_5_Armored", "ABP_PC_MNT_Gryphon_C", "/Game/Characters/Animations/Mounts_Rider/Gryphon/ABP_PC_MNT_Gryphon"},
    };
    for (const Case& c : cases) {
        const std::string title = c.blueprint;
        const content::MountRow* row = row_of_blueprint(book, c.blueprint);
        if (!row) {
            CHECK(false, (title + ": the item is in the data").c_str());
            continue;
        }
        CHECK(book.rider_anim_of(*row) == std::string(c.package) + "." + c.klass, (title + ": its rider AnimBP is " + c.klass).c_str());
        std::string merror;
        auto made = make_plain(book, *row, plain_params(), merror);
        if (!made) {
            CHECK(false, (title + ": the mount is made: " + merror).c_str());
            continue;
        }
        const world::WorldActor& w = made->actor;
        CHECK(w.open_exports.size() == 1 && w.open_exports[0].size() == 2, (title + ": the open carries one export: the AnimBP class and its package").c_str());
        const repl::ExportChain chain = w.open_exports.empty() ? repl::ExportChain{} : w.open_exports[0];
        CHECK(chain.size() == 2 && chain[0].name == c.klass && chain[1].name == c.package && chain[0].has_path() && chain[1].has_path() && chain[0].has_checksum && chain[0].checksum == 0 &&
                  chain[1].has_checksum && chain[1].checksum == 0 && chain[0].guid.is_valid() && chain[1].guid.is_valid() && chain[0].guid.ObjectId != chain[1].guid.ObjectId,
              (title + ": the class is exported by its name in its package, flags 5, checksum 0 [R: capture 911's export of ABP_PC_MNT_Swiftclaw_C]").c_str());
        // the interactable's AttachmentAnimation is that export; no other object is named
        const repl::Subobject* inter = sub_named(w.actor, "UInteractableComponent");
        const repl::Value* data = inter ? inter->obj.state.get(*inter->obj.schema, "InteractionInputData") : nullptr;
        const auto* arr = data ? data->get_if<repl::ArrayValue>() : nullptr;
        size_t named = 0, ours = 0;
        if (arr) {
            for (const repl::ElementState& e : arr->elements) {
                for (const auto& [h, v] : e.values) {
                    if (const auto* g = v.get_if<FIntrepidNetworkGUID>()) {
                        ++named;
                        ours += !chain.empty() && g->ObjectId == chain[0].guid.ObjectId;
                    }
                }
            }
        }
        CHECK(named == 1 && ours == 1, (title + ": the interaction data's one object reference, AttachmentAnimation, is the exported class").c_str());
        // the same AnimBP is the same export in a second mount (the client has it already)
        auto second = make_plain(book, *row, plain_params(), merror);
        CHECK(second && second->actor.open_exports.size() == 1 && same_chain(second->actor.open_exports[0], chain), (title + ": a second mount exports the same objects (stable between calls)").c_str());
        CHECK(writes_open(const_cast<repl::Actor&>(w.actor)), (title + ": the open writes").c_str());
    }

    // every mount item: a rider AnimBP, one export, in the class path form
    size_t named = 0, exported = 0;
    std::set<std::string> anims;
    for (const content::MountRow& row : book.data().mounts) {
        const std::string anim = book.rider_anim_of(row);
        named += anim.rfind("/Game/", 0) == 0 && anim.size() > 2 && anim.compare(anim.size() - 2, 2, "_C") == 0 && anim.find('.') != std::string::npos;
        anims.insert(anim);
        std::string e;
        auto made = make_plain(book, row, plain_params(), e);
        exported += made && made->actor.open_exports.size() == 1;
    }
    CHECK(named == book.data().mounts.size() && exported == book.data().mounts.size(), "all 101 mount items name a rider AnimBP and export it");
    CHECK(anims.size() > 20, "(and they are not all one AnimBP: the Blueprints name their own)");
}


// ---- the controller's timeline -------------------------------------------------------------------------------------------------------

namespace {

struct Seen {
    double item = -1, bar = -1, callout = -1, callout_off = -1, item_end = -1, open = -1, summoning_off = -1, flags = -1, run = -1, flags_off = -1, input_off = -1, gone = -1;
    std::vector<int32_t> ends;
    size_t items = 0, bars = 0, callouts = 0;
    size_t max_mounts = 0;
    double last_x = 0, last_y = 0, last_z = 0, speed_seen = 0;
    bool input_forward = false, invulnerable = false, untargetable = false;
};

struct Rig {
    world::ActorRegistry registry;
    const spawns::MountBook& book;
    std::unique_ptr<MountController> mc;
    std::vector<uint64_t> removed;
    uint32_t listener = 0;
    uint64_t seen_actor = 0;
    uint64_t next_id = 0x7000000;

    explicit Rig(const spawns::MountBook& b) : book(b) {
        MountController::Owner owner;
        owner.player_id = 5;
        owner.guid = {0xAAAAAAAAu, 0xBBBBBBBBu, 0xCCCCCCCCu, 0xDDDDDDDDu};
        owner.name = "Rider";
        owner.pawn = mint();
        owner.server_id = 60;
        mc = std::make_unique<MountController>(registry, book, [this] {
            FIntrepidNetworkGUID g;
            g.ObjectId = next_id;
            next_id += 2;
            g.ServerId = 60;
            g.Randomizer = 9;
            return g;
        }, owner);
        mc->set_instance_source([this] { return instance++; });
        listener = registry.on_remove([this](const world::WorldActor& a) { removed.push_back(a.id); });
    }
    ~Rig() { registry.off_remove(listener); }
    int64_t instance = 40;

    MountController::Request request(const content::MountRow& row, double world_time = 500.0) const {
        MountController::Request r;
        r.item_record = row.item;
        r.item_guid = {1, 2, 3, 4};
        r.client_instance = 77;
        r.now_unix = 2'000'000'000;
        r.place = {-777000, 616000, 5000, 90};
        r.owner_level = 20;
        r.world_time = world_time;
        return r;
    }

    const world::WorldActor* mount() const { return mc->mount(); }

    static bool flag(const world::WorldActor& a, const char* cls, const char* path) {
        for (const repl::Subobject& s : a.actor.subobjects) {
            if (!s.obj.schema || std::string(s.obj.schema->name) != cls) continue;
            const repl::Value* v = s.obj.state.get(*s.obj.schema, path);
            return v && v->get_if<bool>() && *v->get_if<bool>();
        }
        return false;
    }
// built by C-O-A with Cyb

    /// Advances from `from` to `to` (inclusive) in `step` seconds, noting when each thing first happened.
    void run(double from, double to, Seen& seen, double step = 0.005) {
        for (double t = from; t <= to + 1e-9; t += step) {
            MountController::Effects fx = mc->advance(t);
            if (!fx.ability.abilities_added.empty()) {
                if (seen.item < 0) seen.item = t;
                ++seen.items;
            }
            if (!fx.ability.abilities_changed.empty() && seen.item_end < 0) seen.item_end = t;
            if (fx.bar) {
                if (seen.bar < 0) seen.bar = t;
                ++seen.bars;
            }
            if (!fx.callouts.empty()) {
                if (seen.callout < 0) seen.callout = t;
                seen.callouts += fx.callouts.size();
            }
            if (!fx.callouts_removed.empty() && seen.callout_off < 0) seen.callout_off = t;
            for (const int32_t e : fx.ends) seen.ends.push_back(e);
            seen.max_mounts = std::max(seen.max_mounts, registry.count(world::ActorKind::Mount));
            const world::WorldActor* m = mount();
            if (m) {
                seen_actor = m->id;
                if (seen.open < 0) seen.open = t;
                if (seen.summoning_off < 0 && !flag(*m, "UMountComponent", "bIsSummoning")) seen.summoning_off = t;
                if (seen.flags < 0 && flag(*m, "UMountComponent", "bIsDesummoning")) seen.flags = t;
                if (seen.flags >= 0 && seen.flags_off < 0 && !flag(*m, "UMountComponent", "bIsDesummoning")) seen.flags_off = t;
                seen.invulnerable = seen.invulnerable || flag(*m, "UNPCCombatInfo", "bInvulnerable");
                seen.untargetable = seen.untargetable || flag(*m, "UNPCCombatInfo", "bNotTargetable");
                const repl::Value* in = m->actor.obj.state.get(*m->actor.obj.schema, "CurrentMovementInput");
                const bool forward = in && in->get_if<repl::PlayerMovementInput>() && in->get_if<repl::PlayerMovementInput>()->bits != 0;
                if (forward && seen.run < 0) seen.run = t;
                if (seen.input_forward && !forward && seen.input_off < 0) seen.input_off = t;
                seen.input_forward = forward;
                seen.last_x = m->x;
                seen.last_y = m->y;
                seen.last_z = m->z;
            } else if (seen_actor != 0 && seen.gone < 0) {
                seen.gone = t;
            }
        }
    }
};

}  // namespace

static bool near_to(double got, double want, double tol = 0.0125) { return got >= 0 && std::fabs(got - want) <= tol; }

static void test_summon_timeline(const spawns::MountBook& book) {
    std::printf("a summon at retail's times\n");
    const content::MountRow* row = first_of_kind(book, "ground");
    Rig rig(book);
    const double t0 = 100.0;
    const MountController::Request r = rig.request(*row);
    CHECK(rig.mc->summon(r, t0) == MountController::Outcome::Started && rig.mc->phase() == MountController::Phase::Summoning, "the request starts a summon");
    CHECK(rig.mc->actor_id() == 0 && rig.registry.size() == 0, "nothing is in the world yet");
    Seen seen;
    rig.run(t0, t0 + 2.2, seen);
    CHECK(near_to(seen.item - t0, 0.080) && near_to(seen.bar - t0, 0.080) && seen.items == 1 && seen.bars == 1, "the ability item and the bank-18 bar come at +80 ms, once each");
    CHECK(near_to(seen.open - t0, 0.104), "the mount opens at +104 ms");
    CHECK(near_to(seen.summoning_off - t0, 0.104 + 0.470), "bIsSummoning goes off 0.47 s after the open");
    CHECK(near_to(seen.item_end - t0, 1.541), "the ability item ends (no montage) at +1.54 s");
    CHECK(seen.ends.empty() && seen.max_mounts == 1, "no ClientEndAbility, one mount");
    CHECK(rig.mc->phase() == MountController::Phase::Out && !rig.mc->busy() && rig.mount() && rig.mc->item_record() == row->item, "the mount is out, nothing is pending");
    const world::WorldActor* m = rig.mount();
    if (m) {
        const double yaw = 90.0 * 3.14159265358979 / 180.0;
        CHECK(std::fabs(m->x - (r.place.x + 575.0 * std::cos(yaw))) < 1e-6 && std::fabs(m->y - (r.place.y + 575.0 * std::sin(yaw))) < 1e-6, "it stands 575 cm in front of the player along its yaw");
        CHECK(std::fabs(m->z - (r.place.z - 41.0)) < 1e-6 && m->yaw_degrees == 90.0, "at the height retail's was (41 cm under the player's origin, with no ground to read) and with the player's yaw");
        CHECK(m->owner == 5 && m->never_dormant, "owned by the player, never dormant");
    }
    // the item's content
    Rig again(book);
    again.mc->summon(again.request(*row), 10.0);
    MountController::Effects item_fx;
    for (double t = 10.0; t <= 10.2; t += 0.005) {
        MountController::Effects fx = again.mc->advance(t);
        if (!fx.ability.abilities_added.empty()) item_fx = std::move(fx);
    }
    const bootstrap::ActiveAbilityItem* it = item_fx.ability.abilities_added.empty() ? nullptr : &item_fx.ability.abilities_added[0];
    CHECK(it && it->ability_guid == book.summon_ability() && it->server_instance_id == 40 && it->montage_path == MountController::kMontagePath &&
              it->montage_guid == MountController::kMontageGuid && !it->targets_self && item_fx.bar,
          "the item is Summon_Mount with a ServerInstanceId of the pawn's counter and the Summon_Mount montage; the bar goes with it");
    // a second summon replaces the first item
    Rig two(book);
    two.mc->summon(two.request(*row), 10.0);
    Seen s2;
    two.run(10.0, 12.0, s2);
    two.mc->desummon(12.0);
    two.run(12.0, 16.0, s2);
    two.mc->summon(two.request(*row), 17.0);
    MountController::Effects second;
    for (double t = 17.0; t <= 17.2; t += 0.005) {
        MountController::Effects fx = two.mc->advance(t);
        if (!fx.ability.abilities_added.empty()) second = std::move(fx);
    }
    CHECK(second.ability.abilities_removed.size() == 1 && second.ability.abilities_removed[0] == 40 && second.ability.abilities_added.size() == 1 && second.ability.abilities_added[0].server_instance_id == 41,
          "the next summon's item replaces the last one (removed in the same update, the new one has the next instance id)");
    // the ground: the mount stands on it
    Rig g(book);
    g.mc->set_ground([](double, double, double) -> std::optional<double> { return 1000.0; });
    MountController::Request gr = g.request(*row);
    gr.place.z = 1098;   // standing on ground at 1000 (a capsule's half height above it)
    g.mc->summon(gr, 5.0);
    Seen s3;
    g.run(5.0, 5.3, s3);
    CHECK(g.mount() && std::fabs(g.mount()->z - (1000.0 + MountController::kMountFootOffsetCm)) < 1e-6, "with a ground, the mount stands on it (its origin a capsule's half height above)");
}
// Cyb.C O A

static void test_one_mount(const spawns::MountBook& book) {
    std::printf("one mount per player, summon spam\n");
    const content::MountRow* row = first_of_kind(book, "ground");
    const content::MountRow* other = nullptr;
    for (const content::MountRow& r : book.data().mounts) {
        if (r.kind == "ground" && r.item != row->item) {
            other = &r;
            break;
        }
    }
    {
        Rig rig(book);
        rig.mc->summon(rig.request(*row), 100.0);
        Seen seen;
        rig.run(100.0, 102.0, seen);
        const uint64_t first = rig.mc->actor_id();
        // another mount item while one is out: the old one is desummoned and nothing new is summoned
        const auto outcome = rig.mc->summon(rig.request(*other), 102.5);
        CHECK(outcome == MountController::Outcome::Desummoning && rig.mc->phase() == MountController::Phase::Desummoning, "another mount item used while one is out is a desummon of the old one");
        Seen s2;
        rig.run(102.5, 107.0, s2);
        CHECK(s2.items == 0 && s2.max_mounts == 1 && rig.registry.count(world::ActorKind::Mount) == 0 && rig.removed.size() == 1 && rig.removed[0] == first && rig.mc->phase() == MountController::Phase::Idle,
              "the old mount is gone after its dissolve and nothing else was summoned");
        CHECK(rig.mc->summon(rig.request(*other), 107.5) == MountController::Outcome::Started, "the next use summons");
        Seen s3;
        rig.run(107.5, 110.0, s3);
        CHECK(rig.mount() && rig.mc->item_record() == other->item && rig.registry.count(world::ActorKind::Mount) == 1, "the new mount is the other item's");
        // while it dissolves a summon is ignored
        rig.mc->desummon(110.0);
        CHECK(rig.mc->summon(rig.request(*row), 111.0) == MountController::Outcome::Ignored, "a summon while the old mount dissolves is ignored");
    }
    {
        // a flood: 60 requests 10 ms apart: never two mounts at once, and the state stays one of the machine's
        Rig rig(book);
        size_t started = 0, desummoned = 0, ignored = 0;
        Seen seen;
        double t = 200.0;
        for (int i = 0; i < 60; ++i, t += 0.01) {
            rig.run(t, t, seen);
            switch (rig.mc->summon(rig.request(*row), t)) {
                case MountController::Outcome::Started: ++started; break;
                case MountController::Outcome::Desummoning: ++desummoned; break;
                case MountController::Outcome::Ignored: ++ignored; break;
                case MountController::Outcome::Refused: break;
            }
        }
        rig.run(t, t + 8.0, seen);
        CHECK(seen.max_mounts <= 1 && started + desummoned + ignored == 60, "60 summon requests in 0.6 s: never more than one mount in the world");
        CHECK(rig.registry.count(world::ActorKind::Mount) <= 1 && !rig.mc->busy(), "and the controller settles (nothing pending after the dissolves)");
    }
    {
        // an item that is no mount
        Rig rig(book);
        MountController::Request r = rig.request(*row);
        r.item_record = 12345;
        CHECK(rig.mc->summon(r, 1.0) == MountController::Outcome::Ignored && !rig.mc->is_mount_item(12345) && rig.mc->is_mount_item(row->item), "an item that is no mount is ignored");
        CHECK(!rig.mc->desummon(1.0), "a desummon with no mount (and no summon in progress) does nothing");
    }
}

static void test_refusals(const spawns::MountBook& book) {
    std::printf("a refused summon: no actor, the ability item and the callout\n");
    const content::MountRow* row = first_of_kind(book, "ground");
    {
        Rig rig(book);
        rig.mc->set_ground([](double, double, double) -> std::optional<double> { return std::nullopt; });
        const double t0 = 50.0;
        CHECK(rig.mc->summon(rig.request(*row), t0) == MountController::Outcome::Refused && rig.mc->phase() == MountController::Phase::Idle, "no ground in front of the player: refused");
        Seen seen;
        rig.run(t0, t0 + 4.0, seen);
        CHECK(near_to(seen.item - t0, 0.105) && near_to(seen.callout - t0, 0.105) && seen.callouts == 1 && seen.bar < 0, "the ability item and the callout come together at +105 ms, with no bar");
        CHECK(near_to(seen.item_end - t0, 1.600), "the item ends at +1.6 s");
        CHECK(near_to(seen.callout_off - t0, 0.105 + 3.0, 0.02), "the callout is taken off 3 s after it was shown");
        CHECK(seen.open < 0 && rig.registry.size() == 0 && seen.ends.empty(), "no mount, no ClientEndAbility");
        CHECK(rig.mc->stats().refused == 1 && rig.mc->stats().summons == 0, "counted as a refusal");
    }
    {
        // the callout's text and key
        Rig rig(book);
        rig.mc->set_ground([](double, double, double) -> std::optional<double> { return std::nullopt; });
        rig.mc->summon(rig.request(*row), 1.0);
        std::vector<MountController::Callout> got;
        std::vector<uint64_t> off;
        for (double t = 1.0; t < 6.0; t += 0.01) {
            MountController::Effects fx = rig.mc->advance(t);
            for (auto& c : fx.callouts) got.push_back(c);
            for (const uint64_t k : fx.callouts_removed) off.push_back(k);
        }
        CHECK(got.size() == 1 && got[0].text == "Cannot summon mount here." && got[0].key != 0 && off.size() == 1 && off[0] == got[0].key, "the callout says Cannot summon mount here. and is taken off by its key");
    }
    {
        // a summon during the refusal is ignored; after it the next one is made
        Rig rig(book);
        bool ground = false;
        rig.mc->set_ground([&](double, double, double) -> std::optional<double> { return ground ? std::optional<double>(4902.0) : std::nullopt; });
        rig.mc->summon(rig.request(*row), 1.0);
        Seen seen;
        rig.run(1.0, 1.5, seen);
        CHECK(rig.mc->summon(rig.request(*row), 1.5) == MountController::Outcome::Ignored, "a summon while the refusal plays is ignored");
        rig.run(1.5, 3.0, seen);
        ground = true;
        CHECK(rig.mc->summon(rig.request(*row), 3.0) == MountController::Outcome::Started, "once it is over (and the callout still on) the next summon is made");
    }
    {
        // the height rule: the ground in front must be within 250 cm of the player's feet (its origin is 98 cm above them)
        const auto outcome_with = [&](double ground_z) {
            Rig rig(book);
            rig.mc->set_ground([=](double, double, double) -> std::optional<double> { return ground_z; });
            MountController::Request r = rig.request(*row);   // origin 5000, feet 4902
            return rig.mc->summon(r, 1.0);
        };
        CHECK(outcome_with(4902.0) == MountController::Outcome::Started && outcome_with(4902.0 + 249.0) == MountController::Outcome::Started && outcome_with(4902.0 - 249.0) == MountController::Outcome::Started,
              "ground within 250 cm above or below the player's feet: summoned");
        CHECK(outcome_with(4902.0 + 251.0) == MountController::Outcome::Refused && outcome_with(4902.0 - 251.0) == MountController::Outcome::Refused, "a ledge or a drop of more than 250 cm: refused");
    }
    {
        // the lock: the item's LockExpiration (Unix seconds) in the future refuses; in the past does not
        Rig rig(book);
        MountController::Request r = rig.request(*row);
        r.lock_expiration = r.now_unix + 60;
        CHECK(rig.mc->summon(r, 1.0) == MountController::Outcome::Refused, "a mount item under lock cannot be summoned");
        Rig past(book);
        MountController::Request p = past.request(*row);
        p.lock_expiration = p.now_unix - 60;
        CHECK(past.mc->summon(p, 1.0) == MountController::Outcome::Started, "a lock that ended does not");
        Rig dead(book);
        MountController::Request d = dead.request(*row);
        d.dead = true;
        CHECK(dead.mc->summon(d, 1.0) == MountController::Outcome::Refused, "a dead player summons nothing");
    }
}
// [C.O.A - C:Y:B]

static void test_cancel(const spawns::MountBook& book) {
    std::printf("a damaging hit cancels a summon in progress\n");
    const content::MountRow* row = first_of_kind(book, "ground");
    const double t0 = 300.0;
    {
        Rig rig(book);
        rig.mc->summon(rig.request(*row), t0);
        Seen seen;
        rig.run(t0, t0 + 0.04, seen);
        rig.mc->on_damage(t0 + 0.04);
        rig.run(t0 + 0.04, t0 + 5.0, seen);
        CHECK(seen.item < 0 && seen.bar < 0 && seen.open < 0 && rig.registry.size() == 0 && rig.mc->phase() == MountController::Phase::Idle && !rig.mc->busy(), "a hit before the item: nothing is shown, nothing spawns, nothing is pending");
        CHECK(rig.mc->stats().cancelled == 1, "counted");
    }
    {
        Rig rig(book);
        rig.mc->summon(rig.request(*row), t0);
        Seen seen;
        rig.run(t0, t0 + 0.09, seen);   // the item is shown, the mount is not open (+104 ms)
        CHECK(seen.item >= 0 && seen.open < 0, "(the item is shown and the mount is not open yet)");
        rig.mc->on_damage(t0 + 0.09);
        rig.run(t0 + 0.09, t0 + 5.0, seen);
        CHECK(seen.item_end >= 0 && near_to(seen.item_end, t0 + 0.09, 0.011) && seen.open < 0 && rig.registry.size() == 0, "a hit between the item and the open: the item ends at once and the mount never opens");
    }
    {
        Rig rig(book);
        rig.mc->summon(rig.request(*row), t0);
        Seen seen;
        rig.run(t0, t0 + 0.3, seen);   // open, still summoning, the cast has 1.2 s to go
        CHECK(rig.mount() != nullptr, "(the mount is open)");
        rig.mc->on_damage(t0 + 0.3);
        rig.run(t0 + 0.3, t0 + 5.0, seen);
        CHECK(rig.mount() != nullptr && rig.registry.count(world::ActorKind::Mount) == 1 && rig.mc->phase() == MountController::Phase::Out && seen.flags < 0,
              "a hit after the open: the mount stays (the user's rule: mounts remain during fights), nothing dissolves");
        CHECK(rig.mc->stats().cancelled == 0 && rig.mc->stats().desummons == 0, "(not counted as a cancel or a desummon)");
        CHECK(seen.summoning_off >= 0, "(the cast runs on: bIsSummoning goes off as without the hit)");
    }
    {
        Rig rig(book);
        rig.mc->summon(rig.request(*row), t0);
        Seen seen;
        rig.run(t0, t0 + 1.7, seen);   // the cast is over
        rig.mc->on_damage(t0 + 1.7);
        rig.run(t0 + 1.7, t0 + 4.0, seen);
        CHECK(rig.mount() != nullptr && rig.mc->phase() == MountController::Phase::Out && rig.mc->stats().cancelled == 0, "a hit after the cast is over does not touch the mount");
        Rig idle(book);
        idle.mc->on_damage(1.0);
        CHECK(idle.mc->phase() == MountController::Phase::Idle && idle.mc->stats().cancelled == 0, "a hit with nothing summoned does nothing");
    }
    {
        // during a refusal the item ends at once; the callout stays its 3 s
        Rig rig(book);
        rig.mc->set_ground([](double, double, double) -> std::optional<double> { return std::nullopt; });
        rig.mc->summon(rig.request(*row), t0);
        Seen seen;
        rig.run(t0, t0 + 0.5, seen);
        rig.mc->on_damage(t0 + 0.5);
        rig.run(t0 + 0.5, t0 + 4.0, seen);
        CHECK(seen.item_end >= 0 && near_to(seen.item_end, t0 + 0.5, 0.011) && seen.callout_off >= 0, "a hit during a refusal ends its item at once, the callout still goes off");
    }
}

static void test_desummon(const spawns::MountBook& book) {
    std::printf("a desummon: the dissolve, the run, the close at +3.2 s\n");
    const content::MountRow* row = first_of_kind(book, "ground");
    Rig rig(book);
    rig.mc->set_speeds([](const std::string&) { return 990.0; });
    rig.mc->summon(rig.request(*row), 400.0);
    Seen warm;
    rig.run(400.0, 403.0, warm);
    const world::WorldActor* m = rig.mount();
    CHECK(m != nullptr && rig.mc->phase() == MountController::Phase::Out, "(a mount is out)");
    if (!m) return;
    const uint64_t id = m->id;
    const double x0 = m->x, y0 = m->y;
    const uint32_t v0 = m->version;
    const double d0 = 410.0;
    CHECK(rig.mc->desummon(d0) && rig.mc->phase() == MountController::Phase::Desummoning, "the request begins the desummon");
    CHECK(!rig.mc->desummon(d0 + 0.01) && rig.mc->summon(rig.request(*row), d0 + 0.02) == MountController::Outcome::Ignored, "a second desummon, or a summon, while it dissolves does nothing");
    Seen seen;
    rig.run(d0, d0 + 4.0, seen);
    CHECK(near_to(seen.flags - d0, 0.100), "bIsDesummoning goes on at +0.1 s (retail +72 and +118 ms)");
    CHECK(seen.invulnerable && seen.untargetable, "invulnerable and untargetable");
    CHECK(near_to(seen.run - d0, 0.200), "the run begins at +0.2 s (CurrentMovementInput forward)");
    CHECK(near_to(seen.flags_off - d0, 3.100), "bIsDesummoning goes off at +3.1 s");
    CHECK(near_to(seen.input_off - d0, 3.100), "the movement input ends with the flag, at +3.1 s (the close is 100 ms later: a connection's pass is 67 ms, so no update is lost to it)");
    CHECK(near_to(seen.gone - d0, 3.200), "the channel closes (the actor leaves the registry) at +3.2 s");
    CHECK(rig.removed.size() == 1 && rig.removed[0] == id && rig.mc->phase() == MountController::Phase::Idle && !rig.mc->busy() && rig.mc->actor_id() == 0, "removed once, the controller is idle");
    // the run: 990 cm/s along its yaw (90 degrees: +y) for 2.9 s
    const double dx = seen.last_x - x0, dy = seen.last_y - y0;
    CHECK(std::fabs(dx) < 1.0 && std::fabs(dy - 990.0 * 2.9) < 990.0 * 0.15, "it ran forward along its yaw at its ground speed: 990 cm/s for about 2.9 s");
    CHECK(v0 > 0, "(the actor's version moved with every step)");
    // the retail desummon of a mount whose item has no speed text: 600 cm/s times the percentage
    Rig slow(book);
    slow.mc->summon(slow.request(*row), 1.0);
    Seen w;
    slow.run(1.0, 3.0, w);
    const double sx = slow.mount()->x, sy = slow.mount()->y;
    slow.mc->desummon(3.0);
    Seen w2;
    slow.run(3.0, 3.0 + 3.15, w2);
    const double expect = 600.0 * (row->ground_pct > 0 ? row->ground_pct / 100.0 : 1.0) * 2.9;
    CHECK(std::fabs((w2.last_y - sy) - expect) < expect * 0.1 && std::fabs(w2.last_x - sx) < 1.0, "with no speed table it runs at 600 cm/s times the item's ground percentage");
}

static void test_desummon_early(const spawns::MountBook& book) {
    std::printf("a desummon during the summon, a mount that left, the session's end\n");
    const content::MountRow* row = first_of_kind(book, "ground");
    {
        Rig rig(book);
        rig.mc->summon(rig.request(*row), 10.0);
        Seen seen;
        rig.run(10.0, 10.09, seen);
        CHECK(rig.mc->desummon(10.09), "a desummon before the open (the item is shown, the mount is not there)");
        rig.run(10.09, 15.0, seen);
        CHECK(seen.open < 0 && rig.registry.size() == 0 && rig.mc->phase() == MountController::Phase::Idle && seen.item_end >= 0, "no mount ever opens and the item ends");
    }
    {
        Rig rig(book);
        rig.mc->summon(rig.request(*row), 10.0);
        Seen seen;
        rig.run(10.0, 10.2, seen);
        CHECK(rig.mount() != nullptr && rig.mc->desummon(10.2), "a desummon while the mount is summoning (open, the cast not over)");
        rig.run(10.2, 15.0, seen);
        CHECK(seen.flags >= 0 && seen.gone >= 0 && rig.registry.count(world::ActorKind::Mount) == 0 && seen.item_end >= 0, "it dissolves and goes, and the cast's item ends");
    }
    {
        // the registry loses the mount by itself (another system removes it)
        Rig rig(book);
        rig.mc->summon(rig.request(*row), 10.0);
        Seen seen;
        rig.run(10.0, 12.0, seen);
        rig.registry.remove(rig.mc->actor_id());
        rig.run(12.0, 12.1, seen);
        CHECK(rig.mc->phase() == MountController::Phase::Idle && rig.mc->actor_id() == 0 && !rig.mc->desummon(12.2), "a mount that left the world is no mount: idle, nothing to desummon");
        CHECK(rig.mc->summon(rig.request(*row), 13.0) == MountController::Outcome::Started, "and the next summon is made");
    }
    {
        // the owner's session ends: the mount leaves at once
        Rig rig(book);
        rig.mc->summon(rig.request(*row), 10.0);
        Seen seen;
        rig.run(10.0, 12.0, seen);
        const uint64_t id = rig.mc->actor_id();
        rig.mc->shutdown();
        CHECK(rig.registry.size() == 0 && rig.removed.size() == 1 && rig.removed[0] == id && rig.mc->phase() == MountController::Phase::Idle && !rig.mc->busy(), "shutdown removes the mount (every view closes it) and clears the steps");
        Rig mid(book);
        mid.mc->summon(mid.request(*row), 10.0);
        mid.mc->shutdown();
        Seen s2;
        mid.run(10.0, 12.0, s2);
        CHECK(s2.open < 0 && mid.registry.size() == 0, "shutdown during the cast: nothing spawns afterwards");
    }
}
// built with C | O | A and C-Y-B

static void test_world_loads(const content::ContentDb& db) {
    std::printf("the world loads the mounts\n");
    aoc::protocol::ServerGuidAllocator guids(60, 1);
    PlayerWorld world(&db, "", aoc::worldd::bootstrap::GameStateMode::Tier2, 60, guids);
    CHECK(world.ready() && world.mounts() != nullptr && world.mounts()->data().mounts.size() == 101, "a PlayerWorld on the real content.db holds the 101 mounts and the donors");
    std::string error;
    const bool speeds = world.load_mount_speeds(MOUNT_SPEEDS_PATH, error);
    CHECK(speeds, ("the Blueprints' speeds load (dist/content/mount_speeds.tsv): " + error).c_str());
    const spawns::MountBook::Speeds* terror = world.mounts() ? world.mounts()->speeds_of("Mount_Grounded_TerrorBird_0") : nullptr;
    const spawns::MountBook::Speeds* shoal = world.mounts() ? world.mounts()->speeds_of("Mount_Aquatic_Shoalskipper_1_ALT") : nullptr;
    const spawns::MountBook::Speeds* dawn = world.mounts() ? world.mounts()->speeds_of("Mount_Flying_Dawnbreaker_1") : nullptr;
    CHECK(terror && terror->run == 990 && shoal && shoal->swim == 920 && dawn && dawn->fly == 1290, "the terror bird runs 990, the shoalskipper swims 920, the Dawnbreaker flies 1290 (captures 959, 898, 871)");
    CHECK(world.mounts() && world.mounts()->speeds_known() >= 140, "140 Blueprints' speeds");
    std::string bad;
    CHECK(!world.load_mount_speeds("no_such_file.tsv", bad) && !bad.empty(), "a missing file is an error, not a crash (the mounts then run at 600 cm/s times the item's ground percentage)");
    // review B M2: the columns are read by position, so a file whose header is not the one mapc writes, or a row that does not parse, is an error and not different numbers
    {
        const std::string header = "blueprint\tkind\tparents\tmax_walk_speed\tmax_run_speed\tmax_fly_speed\tmax_swim_speed\tdefaults";
        const auto try_file = [&](const std::string& text, std::string& why) {
            const std::string path = "mount_speeds_test_" + std::to_string(g_pass + g_fail) + ".tsv";
            {
                std::FILE* f = std::fopen(path.c_str(), "wb");
                if (f) {
                    std::fwrite(text.data(), 1, text.size(), f);
                    std::fclose(f);
                }
            }
            std::string e;
            auto fresh = spawns::MountBook::load(db, e);
            const bool ok = fresh && fresh->load_speeds(path, why);
            std::remove(path.c_str());
            return ok;
        };
        std::string why;
        CHECK(try_file(header + "\nMount_X\t\tMount_Base_C\t220\t600\t600\t400\tfly\n", why), "(control: a one-row file with the right header loads)");
        CHECK(!try_file(std::string("blueprint\tkind\tmax_run_speed\tmax_walk_speed\tparents\tmax_fly_speed\tmax_swim_speed\tdefaults\nMount_X\t\t600\t220\tMount_Base_C\t600\t400\tfly\n"), why) &&
                  why.find("columns") != std::string::npos,
              "a file with its columns reordered is refused (the speeds would be read as other numbers)");
        CHECK(!try_file(header + "\nMount_X\t\tMount_Base_C\tfast\t600\t600\t400\tfly\n", why) && why.find("not a number") != std::string::npos, "a row whose speed is not a number is an error naming its line");
        CHECK(!try_file(header + "\nMount_X\t\tMount_Base_C\n", why) && why.find("line 2") != std::string::npos, "and so is a row with too few columns");
    }
}

int main() {
    std::string error;
    auto db = content::ContentDb::open(db_path(), error);
    if (!db) {
        std::printf("  [FAIL] the content.db does not open (%s): %s\n", db_path().c_str(), error.c_str());
        return 2;
    }
    auto book = spawns::MountBook::load(*db, error);
    if (!book) {
        std::printf("  [FAIL] the mount tables do not load: %s\n", error.c_str());
        return 2;
    }
    test_world_loads(*db);
    test_donors(*book);
    test_open_golden(*book);
    test_guids(*book);
    test_archetypes(*book);
    test_owner(*book);
    test_book(*book);
    test_alignments_are_the_owners(*book);
    test_rider_anim(*book);
    test_summon_timeline(*book);
    test_one_mount(*book);
    test_refusals(*book);
    test_cancel(*book);
    test_desummon(*book);
    test_desummon_early(*book);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
