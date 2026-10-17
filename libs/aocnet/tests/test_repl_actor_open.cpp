// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  libs/aocnet/tests/test_repl_actor_open.cpp
//
//  protocol/repl/replicator.{h,cpp}: an actor with its subobjects, opened, updated
//  and called, against what the original server sent (capture_blocks.h,
//  fixtures/replay_data.bin).  The values of the small objects are set with
//  RepState::set from the schema's paths; those of the big ones (the ability
//  lists, the stats, the customization, the alignments) are loaded into the object
//  by reading the captured block through the codecs, and written by the replicator.
//  No captured bit goes into an output.
//
//    PlayerController open  ch 3 pkt#22 chSeq 954/955: the export bunch (archetype,
//                           level, the GlobalGMCommands class: 3545 bits) and the
//                           1314-bit payload (SerializeNewActor, the PC's fields,
//                           the actor's block, GlobalGMCommands as a dynamic subobject)
//    PlayerState open       ch 193 pkt#434: the level was exported before, so only the
//                           archetype is (ExportAcks); 1453 bits
//    pawn open              ch 19 pkt#78: exports of the archetype, the level and eight
//                           components (6321 bits); the actor's block and eight subobject
//                           blocks (29350 bits), the last of them Character Appearance
//                           with its 26,861-bit FCharacterCustomizationSaveData
//    pawn update            ch 19 pkt#107 from the shadow after the open: the actor's
//                           changed handles, AlignmentComponent, UPlayerInfo (RepLayout
//                           part and its HeldItems), CombatInfo, AbilityComponent,
//                           StatsComponent's four fast arrays, OwnerInfo, Character
//                           Appearance's AppearanceIDs and the dynamic
//                           ResourceStorageComponent -- 78235 bits and eight exports
//    ClientSetHUD           ch 3 pkt#22 chSeq 956/957 through call_rpc, with its export; the six PC RPCs of
//                           pkt#23 (chSeq 958 and 959) as two batched bunches through call_rpcs
//    exports                the 7289-bit export bunch of an NPC open (ch 16 pkt#66), a
//                           repeated package as GUID + flags 4; the packing limit
//    the shadow             nothing changed sends nothing (and copies nothing), a failed
//                           write leaves the shadow alone, acknowledged exports are not
//                           repeated
//    robustness             a later object's failure rolls the whole update back, a failed
//                           assembly restores the shadow, content blocks that claim more
//                           than the bunch holds, call_rpcs' checks, a chain whose first GUID
//                           the bunch holds, exports marked per acknowledged bunch, the
//                           per-connection parent mask
// ============================================================================
#include "capture_aoc_codecs.h"
#include "protocol/repl/replicator.h"
#include "protocol/schema/generated/index.h"
#include "protocol/wire/packet_reader.h"

#include <cstdio>
#include <cstdlib>
#include <new>
#include <span>
#include <string>
#include <vector>

using namespace aoc::protocol;
using namespace aoc::protocol::repl;
using emit::BunchWriter;
using emit::FIntrepidNetworkGUID;
namespace schema = aoc::protocol::schema;
namespace gen = aoc::protocol::schema::gen;
using capture_blocks::Bits;
using capture_blocks::Block;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

// ---- helpers -------------------------------------------------------------------

static const schema::ClassSchema& schema_named(const char* name) {
    const schema::ClassSchema* s = gen::find_class_schema(name);
    if (!s) { std::printf("  [FAIL] no schema %s\n", name); std::exit(2); }
    return *s;
}

static const schema::ClassSchema& component_schema(const char* name) {
    const schema::ClassSchema* s = gen::find_component_schema(name);
    if (!s) { std::printf("  [FAIL] no component schema %s\n", name); std::exit(2); }
    return *s;
}
// C.O.A and Cyb

static FIntrepidNetworkGUID guid(uint64_t id, uint32_t server = 0, uint32_t rnd = 0) {
    FIntrepidNetworkGUID g;
    g.ObjectId = id;
    g.ServerId = server;
    g.Randomizer = rnd;
    return g;
}

/// The captured session's dynamic objects: ServerId 60.
static FIntrepidNetworkGUID session_guid(uint64_t id) { return guid(id, 60, 0x6ee87ee4); }

/// Sets `path` of the object; a refusal is a failure of the test.
static void set(ObjectState& o, const char* path, Value v) {
    if (!o.state.set(*o.schema, path, std::move(v))) {
        std::printf("  [FAIL] %s.%s refused\n", o.schema->name, path);
        g_fail++;
    }
}

static ExportLink link(uint64_t id, const char* name, bool no_load, uint32_t checksum) {
    ExportLink l;
    l.guid = guid(id);
    l.name = name;
    l.no_load = no_load;
    l.has_checksum = true;   // every captured path is flagged with a checksum; a package's is 0
    l.checksum = checksum;
    return l;
}

/// PersistentLevel and its two outers, flags 7 each (pkt#22, #78, #66).
static ExportChain level_chain() {
    return {link(16442478405498561049ull, "PersistentLevel", true, 0), link(16604466839667550161ull, "Verra_World_Master", true, 0),
            link(12923414834320654503ull, "/Game/Levels/Verra_World_Master/Verra_World_Master", true, 0)};
}

static bool same_prefix(const BunchWriter& w, const Bits& b, size_t n) {
    if (w.bit_pos() < n || b.bits < n) return false;
    const auto want = capture_blocks::bytes_of(b.hex);
    size_t diff = 0;
    if (!capture_blocks::same_bit_range(w.data(), want.data(), n, &diff)) {
        std::printf("    first differing bit %zu (%s)\n", diff, b.name);
        return false;
    }
    return true;
}

static bool same_as(const BunchWriter& w, const Bits& b) {
    if (w.bit_pos() != b.bits) {
        std::printf("    wrote %zu bits, expected %zu (%s)\n", w.bit_pos(), b.bits, b.name);
        return false;
    }
    return same_prefix(w, b, b.bits);
}

static bool emitted_as(const std::optional<Emission>& e, std::initializer_list<const Bits*> exports, const Bits& content) {
    if (!e || e->exports.size() != exports.size()) {
        std::printf("    %zu export bunches, expected %zu\n", e ? e->exports.size() : 0, exports.size());
        return false;
    }
    size_t i = 0;
    for (const Bits* x : exports) {
        if (!same_as(e->exports[i++], *x)) return false;
    }
    return same_as(e->payload, content);
}

/// The RepLayout block of a captured Block read into a state.
static RepState state_of(const Block& b) {
    const schema::ClassSchema& s = schema_named(b.class_name);
    const auto bytes = capture_blocks::bytes_of(b.hex);
    size_t pos = 0;
    auto st = read_rep_block(s, bytes.data(), b.bits, pos);
    if (!st) { std::printf("  [FAIL] %s does not read\n", b.name); g_fail++; return {}; }
    return *st;
}

/// The RepLayout block of a captured Block equals the state the test set by hand.
static bool block_is(const ObjectState& o, const Block& b) {
    const schema::ClassSchema& s = schema_named(b.class_name);
    const auto bytes = capture_blocks::bytes_of(b.hex);
    size_t pos = 0;
    const auto st = read_rep_block(s, bytes.data(), b.bits, pos);
    return st && pos == b.rep_bits && *st == o.state;
}

/// The client's stat table is not on the wire (test_repl_fast_array.cpp): the captured pawn's four extended stats
/// have Records 0x5429e8c0xxxxxxxx and 0x5429e8c4xxxxxxxx.
static CodecOptions stat_options() {
    CodecOptions o;
    o.stat_int32_extended = [](int64_t record) {
        const uint32_t hi = static_cast<uint32_t>(static_cast<uint64_t>(record) >> 32);
        return hi == 0x5429e8c0u || hi == 0x5429e8c4u;
    };
    return o;
}

/// The fast-array fields of a captured block (from bit `from`), applied to `arrays`: state loaded from the capture.
static bool load_fast_arrays(const Block& b, size_t from, ObjectState& o) {
    const auto bytes = capture_blocks::bytes_of(b.hex);
    size_t pos = from;
    const auto fields = read_fast_array_fields(*o.schema, bytes.data(), b.bits, pos, o.opts);
    if (!fields) return false;
    for (const FastArrayField& f : *fields) {
        FastArrayState* st = o.arrays.get(*o.schema, f.field);
        if (!st || !apply_fast_array_delta(*st, f.delta)) return false;
    }
    return true;
}

static Subobject component(uint64_t id, const char* name, uint32_t checksum) {
    Subobject s;
    s.guid = session_guid(id);
    s.name = name;
    s.checksum = checksum;
    s.obj.schema = &component_schema(name);
    return s;
}

static ExportChain gameplay_class(uint64_t id, const char* name, uint32_t checksum) {
    return {link(id, name, false, checksum), link(158953490572197689ull, "/Script/GameSystemsPlugin", false, 0)};
}
// Cyb - C-O-A

static Value object(uint64_t id) { return Value(session_guid(id)); }

static QuantVec session_location() {
    // (-777664.5, 616578.7, 15952.0) cm as WriteQuantizedVector's scaled integers, x10 (pkt#22, #78, #434)
    return QuantVec{QuantVec::Mode::Scaled, -7776645, 6165787, 159520};
}

// ---- the PlayerController ----------------------------------------------------------

static Actor make_pc() {
    Actor a;
    a.guid = session_guid(10341530);
    a.obj.schema = &schema_named("AoCPlayerControllerBP_C");
    set(a.obj, "AuthServerIDReplicated", Value::u64(0));
    set(a.obj, "RemoteRole", Value::u64(2));   // AutonomousProxy: the receiver swaps Role and RemoteRole
    set(a.obj, "Role", Value::u64(4));         // Authority
    set(a.obj, "PlayerState", object(10341528));
    set(a.obj, "SpawnLocation", Value(Vec3d{-777664.5, 616578.6875, 15952.0498046875}));
    set(a.obj, "CaravanLaunchNode", Value::i64(0));
    set(a.obj, "SummonCooldownTimer", Value::f32(1593462.375f));
    a.open.archetype = {link(3503756484819958835ull, "Default__AoCPlayerControllerBP_C", false, 0x6b62891c),
                        link(4074085207143396457ull, "/Game/ThirdPersonCPP/Blueprints/AoCPlayerControllerBP", false, 0)};
    a.open.level = level_chain();
    a.open.spawn.location = session_location();
    a.open.player_controller = PlayerControllerOpen{0, 0};
    Subobject gm;   // a dynamic subobject: no schema, nothing to say, spawned from its class
    gm.guid = session_guid(10341532);
    gm.dynamic_class = gameplay_class(485698175673737329ull, "GlobalGMCommands", 0xcaaaee3e);
    a.subobjects.push_back(std::move(gm));
    return a;
}

static void test_pc_open() {
    std::printf("PlayerController open (ch 3 pkt#22 chSeq 954/955)\n");
    Actor pc = make_pc();
    CHECK(block_is(pc.obj, capture_blocks::kPcOpenProps), "the values set by path are the ones the captured block carries");
    ActorShadow shadow;
    ExportAcks acks;
    bool failed = true;
    const auto e = open_actor(pc, shadow, acks, &failed);
    CHECK(e && !failed, "open_actor");
    CHECK(emitted_as(e, {&capture_blocks::kPcOpenExports0}, capture_blocks::kPcOpenContent),
          "export bunch (archetype, level, GlobalGMCommands class: 3545 bits) and payload (1314 bits) match the original server bit for bit");
    CHECK(e && e->exported.size() == 7 && e->exported[0].ObjectId == 3503756484819958835ull &&
              e->exported[2].ObjectId == 16442478405498561049ull && e->exported[5].ObjectId == 485698175673737329ull,
          "the GUIDs exported, each written with its path: the archetype and its package, the level and its two outers, the class and its package");
    CHECK(shadow.actor.rep.sent.size() == 7 && shadow.subobjects.size() == 1, "the shadow has the actor's 7 values and the subobject");
    CHECK(!update_actor(pc, shadow, acks, &failed) && !failed, "an update right after the open sends nothing");

    // Opening again (a new channel) resets the shadow: the same bits.
    const auto again = open_actor(pc, shadow, acks);
    CHECK(emitted_as(again, {&capture_blocks::kPcOpenExports0}, capture_blocks::kPcOpenContent), "opened again: the same bits");

    // Everything acknowledged: no export bunch.
    ExportAcks all;
    all.mark(e->exported);
    ActorShadow fresh;
    const auto quiet = open_actor(pc, fresh, all);
    CHECK(quiet && quiet->exports.empty() && same_as(quiet->payload, capture_blocks::kPcOpenContent),
          "with the exports acknowledged the open has no export bunch, the same payload");
}

// ---- the PlayerState ---------------------------------------------------------------

static void test_player_state_open() {
    std::printf("PlayerState open (ch 193 pkt#434)\n");
    Actor ps;
    ps.guid = session_guid(10341528);
    ps.obj.schema = &schema_named("AoCPlayerStateBP_C");
    set(ps.obj, "AuthServerIDReplicated", Value::u64(0));
    set(ps.obj, "RemoteRole", Value::u64(1));   // SimulatedProxy
    set(ps.obj, "Owner", object(10341530));
    set(ps.obj, "Role", Value::u64(4));
    set(ps.obj, "PlayerId", Value::i64(1874));
    set(ps.obj, "StartTime", Value::i64(1593462));
    UniqueNetId id;
    id.flags = 8;
    id.id = "DESKTOP-EXAMPLE1-US-45739BC044607BD9A50196AF41F22492";
    set(ps.obj, "UniqueId", Value(id));
    set(ps.obj, "PlayerNamePrivate", Value::str("DESKTOP-EXAMPLE1-US-"));
    ps.open.archetype = {link(16037722909354717791ull, "Default__AoCPlayerStateBP_C", false, 0xf4f44182),
                         link(1938806816082851941ull, "/Game/ThirdPersonCPP/Blueprints/AoCPlayerStateBP", false, 0)};
    ps.open.level = level_chain();
    ps.open.spawn.location = session_location();
    CHECK(block_is(ps.obj, capture_blocks::kPlayerStateOpen434), "the values set by path are the ones the captured block carries");

    ExportAcks acks;
    acks.mark(guid(16442478405498561049ull));   // PersistentLevel: exported by the opens of pkt#22 and #78, acknowledged since
    ActorShadow shadow;
    const auto e = open_actor(ps, shadow, acks);
    CHECK(emitted_as(e, {&capture_blocks::kPlayerStateOpenExports0}, capture_blocks::kPlayerStateOpenContent),
          "the archetype alone is exported (1177 bits); the payload (1453 bits) matches bit for bit");
    CHECK(e && e->exported.size() == 2, "one chain written: the archetype and its package");
}

// ---- the pawn ------------------------------------------------------------------------

/// The pawn as the original server opened it at pkt#78: BaseCharacterInfo, CombatInfo, OwnerInfo, the four storages
/// and Character Appearance, in that order.
static Actor make_pawn() {
    Actor a;
    a.guid = session_guid(10341538);
    a.obj.schema = &schema_named("PlayerPawn_C");
    set(a.obj, "AuthServerIDReplicated", Value::u64(0));
    RepMovement m;   // standing still: location x100 as integers, yaw 65 in byte components
    m.location = QuantVec{QuantVec::Mode::Scaled, -77766450, 61657869, 1595205};
    m.rotation = Rot{0, 65, 0};
    m.linear_velocity = QuantVec{QuantVec::Mode::Scaled, 0, 0, 0};
    set(a.obj, "ReplicatedMovement", Value(m));
    set(a.obj, "RemoteRole", Value::u64(1));
    set(a.obj, "Owner", object(10341530));
    set(a.obj, "Role", Value::u64(4));
    a.open.archetype = {link(9327572450991073355ull, "Default__PlayerPawn_C", false, 0x2cdc5b6),
                        link(9839620482321143831ull, "/Game/ThirdPersonCPP/Blueprints/PlayerPawn", false, 0)};
    a.open.level = level_chain();
    a.open.spawn.location = session_location();
    a.open.spawn.rotation = Rot{0, 16550, 0};
// (C O A) (C_Y_B)

    Subobject base = component(10341540, "BaseCharacterInfo", 0xaff9746f);
    set(base.obj, "CharacterGuid.A", Value::i64(static_cast<int32_t>(3635609113u)));
    set(base.obj, "CharacterGuid.B", Value::i64(static_cast<int32_t>(2516274681u)));
    set(base.obj, "CharacterGuid.C", Value::i64(static_cast<int32_t>(2602449240u)));
    set(base.obj, "CharacterGuid.D", Value::i64(static_cast<int32_t>(1814768596u)));
    set(base.obj, "CharacterName", Value::str("RandomChar"));
    set(base.obj, "CurrentBreathingTime", Value::f32(30.0f));
    Subobject combat = component(10341542, "CombatInfo", 0xb9f1a7bc);
    set(combat.obj, "bIsDead", Value::boolean(false));
    set(combat.obj, "PrimaryClassId.Guid", Value::i64(108640158570105ll));
    a.subobjects.push_back(std::move(base));
    a.subobjects.push_back(std::move(combat));
    a.subobjects.push_back(component(10341544, "OwnerInfo", 0xf05c630d));
    a.subobjects.push_back(component(10341546, "BackpackComponent", 0x24e4d927));
    a.subobjects.push_back(component(10341548, "EquipmentComponent", 0x24e4d927));
    a.subobjects.push_back(component(10341550, "QuestStorageComponent", 0x24e4d927));
    a.subobjects.push_back(component(10341552, "RewardStorageComponent", 0x24e4d927));
    Subobject appearance = component(10341554, "Character Appearance", 0x864dde37);
    // SharedAppearanceInfoId and CharacterCustomization (20 scalar deltas, 2 decals, 2 blend groups, 30 SectionsValues, 83
    // FaceMorphWeightMaps): data, loaded through the FCharacterCustomizationSaveData codec from the captured block.
    appearance.obj.state = state_of(capture_blocks::kCustomizationPawn78);
    a.subobjects.push_back(std::move(appearance));
    return a;
}

static void test_pawn_open() {
    std::printf("pawn open (ch 19 pkt#78)\n");
    Actor pawn = make_pawn();
    CHECK(block_is(pawn.obj, capture_blocks::kPawnOpenActor), "the actor's values are the captured block's");
    CHECK(block_is(pawn.subobjects[0].obj, capture_blocks::kPlayerInfoOpen), "BaseCharacterInfo's values are the captured block's");
    CHECK(block_is(pawn.subobjects[1].obj, capture_blocks::kPlayerCombatInfoOpen), "CombatInfo's values are the captured block's");

    ActorShadow shadow;
    ExportAcks acks;
    const auto whole = open_actor(pawn, shadow, acks);
    CHECK(whole && whole->exports.size() == 1 && same_as(whole->exports[0], capture_blocks::kPawnOpenExports0),
          "the export bunch: archetype, level and eight components, 6321 bits, bit for bit");
    CHECK(whole && whole->exported_by_bunch.size() == 1 && whole->exported_by_bunch[0] == whole->exported && whole->exported.size() == 13,
          "its GUIDs, per export bunch: the archetype and its package, the level and its outers, eight components");
    const auto bytes = capture_blocks::bytes_of(capture_blocks::kPawnOpenContent.hex);
    const auto views = read_content_blocks(bytes.data(), capture_blocks::kPawnOpenContent.bits, 487);   // SerializeNewActor: 487 bits
    CHECK(views && views->size() == 9 && (*views)[8].header.object == session_guid(10341554) && (*views)[8].start == 2245 &&
              (*views)[8].payload_bits == 26950,
          "the captured chain: SerializeNewActor 487 bits, nine blocks, Character Appearance's 26950 bits last from bit 2245");
    CHECK(whole && same_prefix(whole->payload, capture_blocks::kPawnOpenContent, 2245),
          "the payload up to Character Appearance's block (SerializeNewActor, the actor, BaseCharacterInfo, CombatInfo and the five empty blocks) matches bit for bit");
    CHECK(emitted_as(whole, {&capture_blocks::kPawnOpenExports0}, capture_blocks::kPawnOpenContent),
          "the whole chain matches bit for bit: 6321 bits of exports, 29350 of content, Character Appearance's payload included");
    std::vector<ContentBlock> blocks;
    ActorShadow shadow2;
    CHECK(collect_open_blocks(pawn, shadow2, blocks) && blocks.size() == 9 && !blocks[0].sub && blocks[8].has_rep_layout &&
              blocks[8].payload.bit_pos() == 26950 && !blocks[3].has_rep_layout && blocks[3].payload.bit_pos() == 0,
          "collect_open_blocks: the actor's block and one per subobject; the four storages' are empty, Character Appearance's 26950 bits");
}

/// A value the way the pawn's PlayerInfo takes it: WorldAttunements.AttunedBlocks = [{AttunedIndices = [1]}].
static Value attuned_blocks() {
    ArrayValue inner;
    ElementState idx;
    idx.values[1] = Value::u64(1);
    inner.elements.push_back(idx);
    ArrayValue outer;
    ElementState block;
    block.values[1] = Value(inner);
    outer.elements.push_back(block);
    return Value(outer);
}

static void test_pawn_update() {
    std::printf("the pawn after possession, from the shadow (ch 19 pkt#107)\n");
    Actor pawn = make_pawn();
    ActorShadow shadow;
    ExportAcks acks;   // pkt#78's exports were not acknowledged yet 48 ms later: pkt#107 exports them again
    CHECK(open_actor(pawn, shadow, acks).has_value(), "the open");

    // What changed since: the actor
    set(pawn.obj, "RemoteRole", Value::u64(2));   // the receiver swaps it: the pawn is now the autonomous proxy
    set(pawn.obj, "bHidden", Value::boolean(true));
    set(pawn.obj, "Instigator", object(10341538));
    set(pawn.obj, "PlayerState", object(10341528));
    set(pawn.obj, "Controller", object(10341530));
    // the original server sent AuthServerIDReplicated's 0 and CombatInfo's bIsDead = 0 again, values the client already had
    shadow.actor.invalidate(1);
    shadow.subobjects[10341542].invalidate(5);

    // Subobjects the actor gained: AlignmentComponent first, AbilityComponent and StatsComponent after CombatInfo, the
    // dynamic ResourceStorageComponent last.  The order is the original server's.
    Subobject alignment = component(10341560, "AlignmentComponent", 0x3fbd829c);
    alignment.obj.state = state_of(capture_blocks::kAlignmentPawn107);   // Alignments: four instances (FAlignmentContainer)
    Subobject ability = component(10341564, "AbilityComponent", 0x9e63ae5c);
    ability.obj.state = state_of(capture_blocks::kAbilityPawn107);   // KnownAbilities (57), ReplicatedRecordsToLoad (58): loaded from the capture
    Subobject stats = component(10341566, "StatsComponent", 0xf04938d2);
    stats.obj.opts = stat_options();
    CHECK(load_fast_arrays(capture_blocks::kStatsPawn107, 0, stats.obj), "the StatsComponent's four fast arrays are loaded from the capture (335 + 96 + 11 + 1 items)");
    Subobject resource;   // not stably named: the client spawns it from its class, /Script/GameSystemsPlugin.ResourceStorageComponent
    resource.guid = session_guid(10341556);
    resource.dynamic_class = gameplay_class(11144499416456600109ull, "ResourceStorageComponent", 0xf3927ccd);
    resource.obj.schema = &schema_named("UResourceStorageComponent");
    set(resource.obj, "ItemLocationType", Value::u64(11));
    pawn.subobjects.insert(pawn.subobjects.begin(), std::move(alignment));
    pawn.subobjects.insert(pawn.subobjects.begin() + 3, std::move(ability));    // after Alignment, BaseCharacterInfo, CombatInfo
    pawn.subobjects.insert(pawn.subobjects.begin() + 4, std::move(stats));
    pawn.subobjects.push_back(std::move(resource));
    Subobject* base = pawn.find(session_guid(10341540));
    Subobject* owner = pawn.find(session_guid(10341544));
    Subobject* appearance = pawn.find(session_guid(10341554));
    set(base->obj, "ServerId", Value::i64(60));
    set(base->obj, "bIsPhasedOut", Value::boolean(true));
    set(base->obj, "bClientIsReadyToPhaseIn", Value::boolean(false));
    set(base->obj, "bHasBeenPossessed", Value::boolean(true));
    set(base->obj, "bShouldSendOrgsAtWarWithToast", Value::boolean(true));
    set(base->obj, "WorldAttunements.AttunedBlocks", attuned_blocks());
    CHECK(load_fast_arrays(capture_blocks::kPlayerInfoPossessed, capture_blocks::kPlayerInfoPossessed.rep_bits, base->obj),
          "HeldItems (23 items) is loaded from the capture");
    owner->obj.state = state_of(capture_blocks::kOwnerInfo107);
    // Character Appearance gains AppearanceIDs (four FAppearanceId) beside the customization it already had.
    const RepState ids = state_of(capture_blocks::kAppearanceIdsPawn107);
    bool ids_ok = !ids.empty();
    for (const auto& [handle, value] : ids.values()) ids_ok = appearance->obj.state.set(*appearance->obj.schema, handle, value) && ids_ok;
    CHECK(ids_ok, "AppearanceIDs is added to Character Appearance's state");

    bool failed = true;
    const auto e = update_actor(pawn, shadow, acks, &failed);
    CHECK(e && !failed, "update_actor");
    CHECK(emitted_as(e, {&capture_blocks::kPawnPossessedExports0}, capture_blocks::kPawnPossessedContent),
          "exports (8 entries, 4209 bits) and the 78235-bit payload match the original server bit for bit");
    CHECK(e && e->exported.size() == 9, "nine GUIDs exported: seven components, the class and its package");
    CHECK(update_actor(pawn, shadow, acks, &failed) == std::nullopt && !failed,
          "the shadow now has everything: the next update sends nothing");
}
// -- C.O.A . C:Y:B --

// ---- exports ----------------------------------------------------------------------------

static uint32_t export_count(const BunchWriter& b) {
    wire::PacketReader r(b.data(), b.byte_size(), b.bit_pos());
    r.read_bit();
    return r.read_uint32();
}

static void test_exports() {
    std::printf("exports: the NPC open of ch 16 pkt#66, packing and repeated GUIDs\n");
    const FIntrepidNetworkGUID npc = session_guid(2926);
    std::vector<ExportChain> chains;
    chains.push_back({link(15775025627955649865ull, "Default__CSQ0006_EmissaryVaeluneEmpire_C", false, 0x2cdc5b6),
                      link(17259758083969458627ull,
                           "/Game/DesignAssets/Quests/StoryArcAssets/Side_Quests_Alpha01/CSQ_FTU/CSQ0006_EmissaryVaeluneEmpire", false, 0)});
    chains.push_back(level_chain());
    chains.push_back(component_export(npc, session_guid(2932), "AlignmentComponent", 0x3fbd829c));
    chains.push_back(component_export(npc, session_guid(2934), "InteractInfo", 0xe2e9ad91));
    chains.push_back(component_export(npc, session_guid(2936), "BaseCharacterInfo", 0xcc5f18a9));
    chains.push_back(component_export(npc, session_guid(2938), "CombatInfo", 0x38e78c41));
    chains.push_back(component_export(npc, session_guid(2940), "AbilityComponent", 0x9e63ae5c));
    chains.push_back(component_export(npc, session_guid(2942), "StatsComponent", 0xf04938d2));
    chains.push_back(gameplay_class(8542084888825683641ull, "InteractableComponent", 0x8163c481));
    chains.push_back(gameplay_class(4937921067877173103ull, "NPCAppearanceComponent", 0x8523f385));

    ExportAcks none;
    const PackedExports p = pack_exports(chains, none);
    CHECK(p.ok && p.bunches.size() == 1 && same_as(p.bunches[0], capture_blocks::kNpcOpenExports16),
          "ten chains pack into the captured 7289-bit bunch bit for bit: the actor's outer entries are flags 6, "
          "the second /Script/GameSystemsPlugin is its GUID and flags 4");
    CHECK(p.exported.size() == 14, "fourteen GUIDs written with a path: the package of the second class is not");
    CHECK(p.bunches.size() == 1 && export_count(p.bunches[0]) == 10, "the count in the header is 10");

    // The limit is the bunch's: one bit less and the last chain (whose package would then be written in full) goes alone.
    const PackedExports split = pack_exports(chains, none, 7288);
    CHECK(split.ok && split.bunches.size() == 2 && export_count(split.bunches[0]) == 9 && export_count(split.bunches[1]) == 1,
          "with room for 7288 bits the last chain starts a second bunch");
    CHECK(split.bunches.size() == 2 && split.bunches[0].bit_pos() <= 7288 && split.bunches[1].bit_pos() > 33 + 128 + 8 + 32,
          "each bunch within the limit; the second one carries the package in full again");
    {
        const PackedExports big = pack_exports(chains, none, 100);
        CHECK(!big.ok, "a chain that does not fit an empty bunch: refused");
    }
    // Acknowledged chains are skipped.
    ExportAcks acks;
    acks.mark(guid(16442478405498561049ull));
    acks.mark(session_guid(2936));
    const PackedExports skipped = pack_exports(chains, acks);
    CHECK(skipped.ok && skipped.bunches.size() == 1 && export_count(skipped.bunches[0]) == 8 && skipped.exported.size() == 10,
          "with the level and one component acknowledged, eight chains are written");
    {   // an acknowledged outer is its GUID and flags, and the chain ends there (ShouldSendFullPath)
        const ExportChain cls = gameplay_class(4937921067877173103ull, "NPCAppearanceComponent", 0x8523f385);
        ExportAcks plugin;
        plugin.mark(guid(158953490572197689ull));
        const PackedExports with_ack = pack_exports(std::span<const ExportChain>(&cls, 1), plugin);
        const PackedExports without = pack_exports(std::span<const ExportChain>(&cls, 1), none);
        // Without: the package's entry, the null GUID after it (128), its name (32 + 26 bytes) and its checksum (32).
        CHECK(with_ack.ok && without.ok && with_ack.bunches.size() == 1 && without.bunches.size() == 1 &&
                  with_ack.bunches[0].bit_pos() + 128 + 32 + 8 * 26 + 32 == without.bunches[0].bit_pos() && with_ack.exported.size() == 1,
              "an acknowledged package is not written again with its path: 128 + 32 + 8 * 26 + 32 bits less");
    }
    CHECK(pack_exports(chains, [&] { ExportAcks a; for (const auto& c : chains) a.mark(c.front().guid); return a; }()).bunches.empty(),
          "everything acknowledged: no bunch");
    CHECK(component_export(npc, session_guid(1), "X", 5).size() == 2 && component_export(npc, session_guid(1), "X", 5)[1].name.empty(),
          "a component's chain is the component and the actor without a name");
}

// ---- RPCs through the replicator ------------------------------------------------------------------

static void test_rpcs() {
    std::printf("RPCs through the replicator\n");
    Actor pc = make_pc();
    const schema::RpcSchema& hud = *pc.obj.schema->rpc("ClientSetHUD");
    RpcParams params;
    set_rpc_param(hud, params, "NewHUDClass", Value(guid(8463636446363390223ull)));
    const ExportChain hud_class = {link(8463636446363390223ull, "BP_AOCHUD_C", false, 0xac6f1251),
                                   link(7805116325763694055ull, "/Game/UI/Widgets/BP_AOCHUD", false, 0)};
    ExportAcks acks;
    const auto e = call_rpc(pc, nullptr, "ClientSetHUD", params, std::span<const ExportChain>(&hud_class, 1), acks);
    CHECK(emitted_as(e, {&capture_blocks::kRpcClientSetHUDExports}, capture_blocks::kRpcClientSetHUD),
          "ClientSetHUD: the 873-bit export of the HUD class and the 173-bit block, as the original server sent them (pkt#22 chSeq 956/957)");
    acks.mark(guid(8463636446363390223ull));
    const auto quiet = call_rpc(pc, nullptr, "ClientSetHUD", params, std::span<const ExportChain>(&hud_class, 1), acks);
    CHECK(quiet && quiet->exports.empty() && same_as(quiet->payload, capture_blocks::kRpcClientSetHUD), "once the export is acknowledged only the block goes");
    CHECK(!call_rpc(pc, nullptr, "NoSuchRpc", params, {}, acks), "an RPC the class does not have: refused");
    RpcParams bad;
    bad.values[9] = Value::i64(1);
    CHECK(!call_rpc(pc, nullptr, "ClientSetHUD", bad, {}, acks), "parameters that do not match: refused");
// (C O A : Cyb)

    // The original server batches RPCs in one bunch: pkt#23's chSeq 958 (three, 3550 bits) and 959 (three, 263 bits).
    {
        auto values = [&](const Bits& b) {   // the parameters of the block's RPC, read from the capture
            const auto bytes = capture_blocks::bytes_of(b.hex);
            const auto blocks = read_content_blocks(bytes.data(), b.bits, 0);
            size_t pos = blocks ? (*blocks)[0].payload_pos : 0;
            const auto calls = blocks ? read_rpc_fields(*pc.obj.schema, bytes.data(), pos + (*blocks)[0].payload_bits, pos) : std::nullopt;
            return calls && calls->size() == 1 && (*calls)[0].decoded ? (*calls)[0].params : RpcParams{};
        };
        auto joined = [&](std::initializer_list<const Bits*> parts) {
            BunchWriter w;
            for (const Bits* b : parts) {
                const auto bytes = capture_blocks::bytes_of(b->hex);
                w.write_bit_range(bytes.data(), 0, b->bits);
            }
            return w;
        };
        auto equal = [](const BunchWriter& a, const BunchWriter& b) {
            return a.bit_pos() == b.bit_pos() && capture_blocks::same_bit_range(a.data(), b.data(), a.bit_pos(), nullptr);
        };
        std::vector<RpcRequest> second(3);
        second[0].rpc = "ClientFlushLevelStreaming";
        second[1].rpc = "ClientCapBandwidth";
        second[1].params = values(capture_blocks::kRpcClientCapBandwidth);
        second[2].rpc = "ClientSetTime";
        second[2].params = values(capture_blocks::kRpcClientSetTime);
        const auto b959 = call_rpcs(pc, second, acks);
        CHECK(b959 && b959->exports.empty() &&
                  equal(b959->payload, joined({&capture_blocks::kRpcClientFlushLevelStreaming, &capture_blocks::kRpcClientCapBandwidth,
                                               &capture_blocks::kRpcClientSetTime})) &&
                  b959->payload.bit_pos() == 263,
              "three RPCs in one bunch: pkt#23 chSeq 959's 263 bits (ClientFlushLevelStreaming, ClientCapBandwidth, ClientSetTime)");
        std::vector<RpcRequest> first(3);
        first[0].rpc = "ClientVoiceHandshakeComplete";
        first[1].rpc = "ClientEnableNetworkVoice";
        first[1].params = values(capture_blocks::kRpcClientEnableNetworkVoice);
        first[2].rpc = "ClientUpdateMultipleLevelsStreamingStatusCompressed";
        first[2].params = values(capture_blocks::kRpcClientUpdateMultipleLevelsStreamingStatusCompressed);
        const auto b958 = call_rpcs(pc, first, acks);
        CHECK(b958 && b958->exports.empty() &&
                  equal(b958->payload, joined({&capture_blocks::kRpcClientVoiceHandshakeComplete, &capture_blocks::kRpcClientEnableNetworkVoice,
                                               &capture_blocks::kRpcClientUpdateMultipleLevelsStreamingStatusCompressed})) &&
                  b958->payload.bit_pos() == 3550,
              "and chSeq 958's 3550 bits (ClientVoiceHandshakeComplete, ClientEnableNetworkVoice, the 429-byte level statuses)");
        CHECK(!call_rpcs(pc, {}, acks), "no calls: nothing");
    }

    // On a subobject (the capture has none S>C on the player channels: round trip through the client's reader)
    Actor pawn = make_pawn();
    pawn.subobjects.push_back(component(10341564, "AbilityComponent", 0x9e63ae5c));
    const Subobject* ability = pawn.find(session_guid(10341564));
    const schema::RpcSchema* end = ability->obj.schema->rpc("ClientEndAbility");
    RpcParams ep;
    set_rpc_param(*end, ep, end->params[0].name, Value::i64(77));
    const auto s = call_rpc(pawn, ability, "ClientEndAbility", ep, {}, ExportAcks{});
    CHECK(s && s->exports.size() == 1 && export_count(s->exports[0]) == 1, "on the AbilityComponent: its export bunch (name, checksum, the pawn as outer) first");
    if (!s) return;
    const auto blocks = read_content_blocks(s->payload.data(), s->payload.bit_pos(), 0);
    CHECK(blocks && blocks->size() == 1 && !(*blocks)[0].header.is_actor && !(*blocks)[0].header.has_rep_layout &&
              (*blocks)[0].header.stably_named && (*blocks)[0].header.object == session_guid(10341564),
          "the block is the server's subobject form: [0][0][GUID][stably named 1]");
    size_t pos = (*blocks)[0].payload_pos;
    const auto calls = read_rpc_fields(*ability->obj.schema, s->payload.data(), pos + (*blocks)[0].payload_bits, pos);
    CHECK(calls && calls->size() == 1 && (*calls)[0].decoded && std::string((*calls)[0].rpc->name) == "ClientEndAbility" &&
              (*calls)[0].params == ep,
          "and its RPC field reads back to the values");
}

// ---- the shadow ---------------------------------------------------------------------------------------
// -- C | O | A x C | Y | B --

static void test_shadow() {
    std::printf("the shadow\n");
    ExportAcks acks;
    {   // an update of one value sends that handle alone, then nothing
        Actor pc = make_pc();
        ActorShadow shadow;
        open_actor(pc, shadow, acks);
        set(pc.obj, "Role", Value::u64(3));
        bool failed = true;
        const auto e = update_actor(pc, shadow, acks, &failed);
        CHECK(e && !failed && e->exports.empty(), "one changed value: an update without exports");
        if (e) {
            const auto views = read_content_blocks(e->payload.data(), e->payload.bit_pos(), 0);
            size_t pos = views ? (*views)[0].payload_pos : 0;
            const auto st = views ? read_rep_block(*pc.obj.schema, e->payload.data(), e->payload.bit_pos(), pos) : std::nullopt;
            CHECK(views && views->size() == 1 && (*views)[0].header.is_actor && (*views)[0].header.has_rep_layout && st &&
                      st->values().size() == 1 && st->get(16) && *st->get(16) == Value::u64(3),
                  "the block is the actor's, RepLayout, handle 16 (Role) alone");
        }
        CHECK(!update_actor(pc, shadow, acks, &failed) && !failed, "and the next update has nothing");
    }
    {   // a value the codecs cannot carry: nothing sent, nothing forgotten
        Actor pc = make_pc();
        ActorShadow shadow;
        open_actor(pc, shadow, acks);
        set(pc.obj, "bHidden", Value::boolean(true));   // handle 3
        RepMovement wide;   // a rotation component beyond a byte: RepState::set cannot know the object's rotation quantization
        wide.rotation = Rot{0, 300, 0};
        set(pc.obj, "ReplicatedMovement", Value(wide));   // (handle 7), and the byte form of the wire cannot carry it
        const ActorShadow before = shadow;
        bool failed = false;
        CHECK(!update_actor(pc, shadow, acks, &failed) && failed, "a value that cannot be written fails the update");
        CHECK(shadow.actor.rep.sent == before.actor.rep.sent && shadow.subobjects.size() == before.subobjects.size(),
              "and the shadow is as it was");
        RepMovement fine;
        fine.rotation = Rot{0, 30, 0};
        set(pc.obj, "ReplicatedMovement", Value(fine));
        const auto e = update_actor(pc, shadow, acks, &failed);
        size_t pos = 0;
        const auto views = e ? read_content_blocks(e->payload.data(), e->payload.bit_pos(), 0) : std::nullopt;
        pos = views ? (*views)[0].payload_pos : 0;
        const auto st = views ? read_rep_block(*pc.obj.schema, e->payload.data(), e->payload.bit_pos(), pos) : std::nullopt;
        CHECK(e && !failed && st && st->get(3) && *st->get(3) == Value::boolean(true) && st->get(7),
              "the corrected update still carries the handle that came before the bad one");
    }
    {   // an export that does not fit a bunch fails the open and leaves the shadow empty
        Actor pc = make_pc();
        pc.open.archetype[0].name = std::string(1200, 'x');
        ActorShadow shadow;
        bool failed = false;
        CHECK(!open_actor(pc, shadow, acks, &failed) && failed && shadow.actor.rep.sent.empty() && shadow.subobjects.empty(),
              "an export too big for a bunch fails the open, the shadow stays empty");
        Actor odd = make_pc();
        odd.guid = guid(10341531, 60, 1);   // odd ObjectId: a static object, not supported
        failed = false;
        CHECK(!open_actor(odd, shadow, acks, &failed) && failed, "an odd (static) actor GUID is refused");
    }
    {   // a subobject the shadow has not seen is sent in full with its export; acknowledged ones are not exported
        Actor pawn = make_pawn();
        ActorShadow shadow;
        open_actor(pawn, shadow, acks);
        Subobject stats = component(10341566, "StatsComponent", 0xf04938d2);
        pawn.subobjects.push_back(std::move(stats));
        ExportAcks acked;
        acked.mark(session_guid(10341566));
        bool failed = false;
        const auto quiet = update_actor(pawn, shadow, acked, &failed);
        CHECK(quiet && quiet->exports.empty() && quiet->payload.bit_pos() == 139,
              "a new empty subobject: its 139-bit block, no export once acknowledged");
        pawn.subobjects.push_back(component(10341564, "AbilityComponent", 0x9e63ae5c));
        const auto loud = update_actor(pawn, shadow, acks, &failed);
        CHECK(loud && loud->exports.size() == 1 && export_count(loud->exports[0]) == 1, "an unacknowledged new subobject brings its export");
    }
}

// ---- robustness: hostile input, atomic updates, exports ---------------------------------------------------------------

// ---- T1: a failure in a later object rolls the actor's commit back and a fast-array failure fails the update ---------------
static void t1_rollback_across_objects() {
    std::printf("a failure in a subobject rolls back the whole update\n");
    Actor pawn = make_pawn();
    ActorShadow shadow;
    ExportAcks acks;
    open_actor(pawn, shadow, acks);
    set(pawn.obj, "bHidden", Value::boolean(true));                 // the actor's block is written (and committed) first
    Subobject* base = pawn.find(session_guid(10341540));            // BaseCharacterInfo -> UPlayerInfo, HeldItems is a fast array
    FastArrayState* held = base->obj.arrays.get(*base->obj.schema, "HeldItems");
    bool built = held != nullptr;
    std::vector<int32_t> ids;
    for (int i = 0; built && i < 2049; ++i) {                       // one more than net.MaxNumberOfAllowedTArrayChangesPerUpdate
        ElementState item;
        item.values[1] = Value::u64(i & 0x7F);
        item.values[2] = Value::i64(i);
        item.values[3] = Value::u64(0);
        const auto id = held->add(item);
        built = id.has_value();
        if (id) ids.push_back(*id);
    }
    CHECK(built, "a HeldItems array of 2049 items is built");
    const ActorShadow before = shadow;
    bool failed = false;
    CHECK(!update_actor(pawn, shadow, acks, &failed) && failed, "the update fails: the writer refuses 2049 changes");
    CHECK(shadow.actor.rep.sent == before.actor.rep.sent && shadow.subobjects.size() == before.subobjects.size(),
          "and the actor's shadow is as it was (its bHidden write was rolled back)");
    CHECK(shadow.subobjects.at(10341540).rep.sent == before.subobjects.at(10341540).rep.sent &&
              shadow.subobjects.at(10341540).fast.by_field.size() == before.subobjects.at(10341540).fast.by_field.size(),
          "and so is the failing subobject's");
    for (size_t i = 0; i < 100; ++i) held->remove(ids[i]);          // back to 1949
    const auto e = update_actor(pawn, shadow, acks, &failed);
    size_t pos = 0;
    const auto views = e ? read_content_blocks(e->payload.data(), e->payload.bit_pos(), 0) : std::nullopt;
    pos = views ? (*views)[0].payload_pos : 0;
    const auto st = views ? read_rep_block(*pawn.obj.schema, e->payload.data(), e->payload.bit_pos(), pos) : std::nullopt;
    CHECK(e && !failed && st && st->get(3) && *st->get(3) == Value::boolean(true),
          "once fixed, the update still carries the actor's bHidden (it was not lost with the failed attempt)");
}
// [CoA :: C-Y-B]

// ---- T2: an assembly failure in an update rolls the shadow back and an unnamed stably-named subobject is refused ------------
static void t2_assembly_failure_rolls_back() {
    std::printf("an update whose assembly fails leaves the shadow alone\n");
    Actor pawn = make_pawn();
    ActorShadow shadow;
    ExportAcks acks;
    open_actor(pawn, shadow, acks);
    set(pawn.obj, "bHidden", Value::boolean(true));
    Subobject nameless = component(10341560, "AlignmentComponent", 0x3fbd829c);
    nameless.name.clear();                                           // stably named, but with no name to export
    pawn.subobjects.push_back(std::move(nameless));
    const ActorShadow before = shadow;
    bool failed = false;
    CHECK(!update_actor(pawn, shadow, acks, &failed) && failed, "a subobject that cannot be exported fails the update");
    CHECK(shadow.actor.rep.sent == before.actor.rep.sent && shadow.subobjects.size() == before.subobjects.size(),
          "and the shadow is as it was: no bHidden committed, the new subobject not recorded");
    pawn.subobjects.back().name = "AlignmentComponent";
    const auto e = update_actor(pawn, shadow, acks, &failed);
    const auto views = e ? read_content_blocks(e->payload.data(), e->payload.bit_pos(), 0) : std::nullopt;
    CHECK(e && !failed && views && views->size() == 2 && (*views)[0].header.is_actor && !(*views)[1].header.is_actor,
          "named, the retry carries the actor's change and the new subobject's block");

    // The same through the pieces a caller that assembles itself uses: ShadowUndo puts the shadow back.
    Actor again = make_pawn();
    ActorShadow s2;
    open_actor(again, s2, acks);
    set(again.obj, "bHidden", Value::boolean(true));
    again.subobjects.push_back(component(10341560, "AlignmentComponent", 0x3fbd829c));
    const ActorShadow b2 = s2;
    std::vector<ContentBlock> blocks;
    ShadowUndo undo;
    CHECK(collect_update_blocks(again, s2, blocks, &undo) && blocks.size() == 2 && s2.actor.rep.sent.count(3) == 1 &&
              s2.subobjects.count(10341560) == 1 && s2.subobjects.at(10341560).opened,
          "collect_update_blocks commits the actor's bHidden and records the new subobject as opened");
    undo.rollback(s2);
    CHECK(s2.actor.rep.sent == b2.actor.rep.sent && s2.subobjects.size() == b2.subobjects.size(),
          "ShadowUndo::rollback puts both back");
    // An open's undo is the whole shadow it replaced.
    ActorShadow s3 = b2;
    ShadowUndo open_undo;
    CHECK(collect_open_blocks(again, s3, blocks, &open_undo) && open_undo.whole.has_value(), "an open keeps the shadow it replaced");
    open_undo.rollback(s3);
    CHECK(s3.actor.rep.sent == b2.actor.rep.sent && s3.subobjects.size() == b2.subobjects.size(), "and the rollback restores it");
}

// ---- T3: content blocks that claim more than the bunch holds ---------------------------------------------------------------
static void t3_block_past_the_bunch() {
    std::printf("a content block whose payload runs past the bunch\n");
    BunchWriter w;
    w.write_bit(0);    // bHasRepLayout
    w.write_bit(1);    // bIsActor
    w.write_sip(1000); // 1000 payload bits claimed, none present
    size_t pos = 0;
    CHECK(!read_content_block(w.data(), w.bit_pos(), pos, false).has_value() && pos == 0,
          "read_content_block (client form) refuses it and leaves pos");
    CHECK(!read_content_blocks(w.data(), w.bit_pos(), 0, true).has_value(), "read_content_blocks (server form) refuses it");
    const schema::ClassSchema& pawn = schema_named("PlayerPawn_C");
    CHECK(!read_client_bunch(pawn, nullptr, w.data(), w.bit_pos()).has_value(), "and so does read_client_bunch");
    // the same with the length lying by exactly one bit
    BunchWriter v;
    v.write_bit(0);
    v.write_bit(1);
    v.write_sip(9);
    for (int i = 0; i < 8; ++i) v.write_bit(0);
    size_t p2 = 0;
    CHECK(!read_content_block(v.data(), v.bit_pos(), p2, false).has_value(), "a payload one bit past the end is refused");
}

// ---- T9: call_rpc on a subobject without a schema ---------------------------------------------------------------------------
static void t9_rpc_on_schemaless_target() {
    std::printf("an RPC on a subobject that has no schema\n");
    Actor pc = make_pc();   // its GlobalGMCommands subobject is dynamic and has no schema
    ExportAcks acks;
    CHECK(!call_rpc(pc, &pc.subobjects[0], "ClientSetHUD", RpcParams{}, {}, acks).has_value(), "refused, no crash");
}

// ---- call_rpcs: one reliability per bunch, a target of the actor's ---------------------------------------------------------------
static void test_rpc_calls_checked() {
    std::printf("call_rpcs: reliability, targets\n");
    Actor pc = make_pc();
    ExportAcks acks;
    RpcRequest reliable;   // a reliable RPC (FUNC_NetReliable) ...
    reliable.rpc = "ClientFlushLevelStreaming";
    RpcRequest unreliable;   // ... and an unreliable one
    unreliable.rpc = "ClientHitConfirmations";
    const auto r = call_rpcs(pc, std::span<const RpcRequest>(&reliable, 1), acks);
    const auto u = call_rpcs(pc, std::span<const RpcRequest>(&unreliable, 1), acks);
    CHECK(r && r->reliable && u && !u->reliable, "a reliable call gives a reliable emission, an unreliable one an unreliable emission");
    const RpcRequest both[2] = {reliable, unreliable};
    CHECK(!call_rpcs(pc, both, acks), "a bunch that mixes the two is refused (one bunch has one reliability)");
    const RpcRequest two[2] = {reliable, reliable};
    const auto rr = call_rpcs(pc, two, acks);
    CHECK(rr && rr->reliable, "two reliable calls batch");
    ActorShadow pc_shadow;
    const auto pc_open = open_actor(pc, pc_shadow, acks);
    CHECK(pc_open && pc_open->reliable, "an open is reliable");

    Actor other = make_pc();   // a subobject of another actor, and one that is nobody's
    RpcRequest foreign;
    foreign.rpc = "ClientFlushLevelStreaming";
    foreign.target = &other.subobjects[0];
    CHECK(!call_rpcs(pc, std::span<const RpcRequest>(&foreign, 1), acks), "a target that is not one of the actor's subobjects is refused");
    Subobject stray = component(10341564, "AbilityComponent", 0x9e63ae5c);
    foreign.target = &stray;
    CHECK(!call_rpcs(pc, std::span<const RpcRequest>(&foreign, 1), acks), "nor one that belongs to no actor");
    Actor pawn = make_pawn();
    pawn.subobjects.push_back(component(10341564, "AbilityComponent", 0x9e63ae5c));
    RpcRequest own;
    own.target = &pawn.subobjects.back();
    own.rpc = "ClientEndAbility";
    const schema::RpcSchema* end = own.target->obj.schema->rpc("ClientEndAbility");
    set_rpc_param(*end, own.params, end->params[0].name, Value::i64(77));
    CHECK(call_rpcs(pawn, std::span<const RpcRequest>(&own, 1), acks).has_value(), "the actor's own subobject is fine");
}
// -- C | Y | B . C-O-A --

// ---- exports: packing, the first GUID, what an acknowledgement marks -----------------------------------------------------------
static void test_export_packing_rules() {
    std::printf("exports: a first GUID the bunch already holds, per-bunch GUIDs, marking on acknowledgement\n");
    const FIntrepidNetworkGUID npc = session_guid(2926);
    const ExportChain level = level_chain();
    ExportAcks none;
    {   // ShouldSendFullPath: a chain whose first GUID went into the bunch is not exported again, not even without its path
        std::vector<ExportChain> chains = {level, level};
        const PackedExports twice = pack_exports(chains, none);
        const PackedExports once = pack_exports(std::span<const ExportChain>(&chains[0], 1), none);
        CHECK(twice.ok && once.ok && twice.bunches.size() == 1 && export_count(twice.bunches[0]) == 1 &&
                  twice.bunches[0].bit_pos() == once.bunches[0].bit_pos() &&
                  capture_blocks::same_bit_range(twice.bunches[0].data(), once.bunches[0].data(), once.bunches[0].bit_pos(), nullptr) &&
                  twice.exported == once.exported && twice.exported.size() == 3,
              "two chains with the same first GUID give one entry, count 1, exactly the bits of the single chain");
        // The same holds for a first GUID the bunch holds as an outer: the actor of a component written before.
        const ExportChain alignment = component_export(npc, session_guid(2932), "AlignmentComponent", 0x3fbd829c);
        const ExportChain actor_alone = {ExportLink{npc, "", true, true, 0}};
        std::vector<ExportChain> with_actor = {alignment, actor_alone};
        const PackedExports q = pack_exports(with_actor, none);
        CHECK(q.ok && q.bunches.size() == 1 && export_count(q.bunches[0]) == 1,
              "a chain whose first GUID the bunch holds as an outer (the actor of a component) is not exported again");
        // The bunch is what counts: in another bunch the GUID is written again.
        const PackedExports both = pack_exports(std::vector<ExportChain>{level, alignment}, none);
        const size_t just_under = both.bunches[0].bit_pos() - 1;   // level and component do not fit one bunch, each does alone
        const PackedExports split = pack_exports(std::vector<ExportChain>{level, alignment, level}, none, just_under);
        CHECK(split.ok && split.bunches.size() == 3 && export_count(split.bunches[0]) == 1 && export_count(split.bunches[1]) == 1 &&
                  export_count(split.bunches[2]) == 1,
              "after a flush the first GUID is exported again: the record is per bunch");
    }
    {   // the GUIDs of each export bunch, for marking them when that bunch is acknowledged
        std::vector<ExportChain> chains;
        chains.push_back({link(15775025627955649865ull, "Default__CSQ0006_EmissaryVaeluneEmpire_C", false, 0x2cdc5b6),
                          link(17259758083969458627ull,
                               "/Game/DesignAssets/Quests/StoryArcAssets/Side_Quests_Alpha01/CSQ_FTU/CSQ0006_EmissaryVaeluneEmpire", false, 0)});
        chains.push_back(level_chain());
        chains.push_back(component_export(npc, session_guid(2932), "AlignmentComponent", 0x3fbd829c));
        chains.push_back(gameplay_class(4937921067877173103ull, "NPCAppearanceComponent", 0x8523f385));
        const PackedExports one = pack_exports(chains, none);
        CHECK(one.ok && one.bunches.size() == 1 && one.exported_by_bunch.size() == 1 && one.exported_by_bunch[0] == one.exported,
              "one bunch: its GUIDs are all the exported ones");
        // a bunch that holds the first three chains and another for the last
        size_t fits = 0;
        for (size_t bits = 200; bits < 7621; bits += 8) {
            const PackedExports t = pack_exports(chains, none, bits);
            if (t.ok && t.bunches.size() == 2) { fits = bits; break; }
        }
        const PackedExports two = pack_exports(chains, none, fits);
        std::vector<FIntrepidNetworkGUID> all;
        for (const auto& b : two.exported_by_bunch) all.insert(all.end(), b.begin(), b.end());
        CHECK(fits != 0 && two.bunches.size() == 2 && two.exported_by_bunch.size() == 2 && all == two.exported &&
                  !two.exported_by_bunch[0].empty() && !two.exported_by_bunch[1].empty(),
              "two bunches: the GUIDs per bunch, parallel to the bunches, in the order they were written");
    }
    {   // the pawn's open: marking per bunch, and what a NAK leaves behind
        Actor pawn = make_pawn();
        ActorShadow shadow;
        ExportAcks acks;
        const auto open = open_actor(pawn, shadow, acks);
        CHECK(open && open->exported_by_bunch.size() == open->exports.size() && open->exports.size() == 1 &&
                  open->exported_by_bunch[0] == open->exported,
              "an open's Emission has the GUIDs per export bunch");
        // NAK (nothing marked): the next chain that names them exports them again
        ActorShadow other;
        const auto again = open_actor(pawn, other, acks);
        CHECK(again && again->exports.size() == 1 && again->exported == open->exported, "unacknowledged: the same exports again");
        acks.mark(open->exported_by_bunch[0]);   // the packet carrying the export bunch was acknowledged
        ActorShadow third;
        const auto quiet = open_actor(pawn, third, acks);
        CHECK(quiet && quiet->exports.empty() && quiet->exported.empty(), "acknowledged: no export bunch");
    }
}

// ---- per-connection conditions: an inactive parent is not sent -------------------------------------------------------------------
/// The actor's RepLayout block of an emission whose content blocks start at bit `from` (an open: SerializeNewActor's 487 bits).
static RepState actor_block_of(const Actor& a, const Emission& e, size_t from) {
    const auto views = read_content_blocks(e.payload.data(), e.payload.bit_pos(), from);
    size_t pos = views && !views->empty() ? (*views)[0].payload_pos : 0;
    const auto st = views && !views->empty() && (*views)[0].header.is_actor && (*views)[0].header.has_rep_layout
                        ? read_rep_block(*a.obj.schema, e.payload.data(), e.payload.bit_pos(), pos)
                        : std::nullopt;
    return st ? *st : RepState{};
}

static void test_active_parents() {
    std::printf("per-connection conditions: a parent the connection does not have is left out, sent when it turns active\n");
    Actor pawn = make_pawn();   // ReplicatedMovement (handle 7) is set
    const schema::ClassSchema& cls = *pawn.obj.schema;
    ExportAcks acks;
    ActorShadow shadow;
    CHECK(shadow.actor.active.set_active(cls, "ReplicatedMovement", false) && !shadow.actor.active.set_active(cls, "NoSuchProperty", false),
          "a mask is set by property path (an unknown path is refused)");
    const auto open = open_actor(pawn, shadow, acks);
    const RepState opened = open ? actor_block_of(pawn, *open, 487) : RepState{};
    CHECK(open && opened.values().size() == 4 && opened.get(1) && !opened.get(7) && shadow.actor.rep.sent.count(7) == 0,
          "an open leaves the inactive parent's handle out (4 of the actor's 5 values), and out of the shadow");
    CHECK(!shadow.actor.active.active(cls.command("ReplicatedMovement")->parent) && shadow.actor.active.active(0),
          "the mask survives the open (it is the connection's, not something that was sent)");

    RepMovement moved;   // the pawn walks: the value changes while the connection does not get it
    moved.location = QuantVec{QuantVec::Mode::Scaled, -77766000, 61657869, 1595205};
    moved.rotation = Rot{0, 70, 0};
    moved.linear_velocity = QuantVec{QuantVec::Mode::Scaled, 120, 0, 0};
    set(pawn.obj, "ReplicatedMovement", Value(moved));
    bool failed = true;
    CHECK(!update_actor(pawn, shadow, acks, &failed) && !failed, "a change to an inactive parent sends nothing");

    shadow.actor.active.set_active(cls, "ReplicatedMovement", true);
    const auto e = update_actor(pawn, shadow, acks, &failed);
    const RepState st = e ? actor_block_of(pawn, *e, 0) : RepState{};
    CHECK(e && !failed && st.values().size() == 1 && st.get(7) && *st.get(7) == Value(moved),
          "once active, the update carries it: what differs from the shadow (the whole value: it was never sent)");
    CHECK(!update_actor(pawn, shadow, acks, &failed) && !failed, "and then nothing");

    // Deactivated again, it changes, reactivated: the difference to what was sent.
    shadow.actor.active.set_active(cls, "ReplicatedMovement", false);
    RepMovement moved2 = moved;
    moved2.location.x += 100;
    set(pawn.obj, "ReplicatedMovement", Value(moved2));
    CHECK(!update_actor(pawn, shadow, acks, &failed), "inactive again: nothing for the new value");
    shadow.actor.active.set_active(cls, "ReplicatedMovement", true);
    const auto e2 = update_actor(pawn, shadow, acks, &failed);
    const RepState st2 = e2 ? actor_block_of(pawn, *e2, 0) : RepState{};
    CHECK(e2 && st2.get(7) && *st2.get(7) == Value(moved2), "reactivated: the changed value goes out");
    shadow.actor.active.set_active(cls, "ReplicatedMovement", false);
    set(pawn.obj, "ReplicatedMovement", Value(moved));
    set(pawn.obj, "ReplicatedMovement", Value(moved2));
    shadow.actor.active.set_active(cls, "ReplicatedMovement", true);
    CHECK(!update_actor(pawn, shadow, acks, &failed) && !failed, "a value that is back at what the connection has sends nothing on activation");

    // A subobject's mask, made before the open: the subobject is still new and gets its block, without the property.
    ActorShadow s2;
    Subobject* combat = pawn.find(session_guid(10341542));
    s2.subobjects[10341542].active.set_active(*combat->obj.schema, "bIsDead", false);
    const auto open2 = open_actor(pawn, s2, acks);
    const auto views = open2 ? read_content_blocks(open2->payload.data(), open2->payload.bit_pos(), 487) : std::nullopt;
    bool without = false;
    bool found = false;
    if (views) {
        for (const BlockView& v : *views) {
            if (v.header.is_actor || !(v.header.object == session_guid(10341542))) continue;
            found = true;
            size_t pos = v.payload_pos;
            const auto cs = read_rep_block(*combat->obj.schema, open2->payload.data(), open2->payload.bit_pos(), pos);
            without = cs && !cs->get(*combat->obj.schema, "bIsDead") && cs->get(*combat->obj.schema, "PrimaryClassId.Guid");
        }
    }
    CHECK(found && without, "a subobject with a mask made before the open gets its block, with the inactive property left out");
    CHECK(s2.subobjects.at(10341542).opened && s2.subobjects.at(10341542).rep.sent.count(combat->obj.schema->command("bIsDead")->handle) == 0,
          "and is opened in the shadow");
    // A new channel: the mask stays, what was sent is forgotten.
    s2.forget_sent();
    CHECK(s2.subobjects.size() == 1 && !s2.subobjects.at(10341542).opened && !s2.subobjects.at(10341542).active.all_active() &&
              s2.subobjects.at(10341542).rep.sent.empty() && s2.actor.rep.sent.empty(),
          "forget_sent keeps a subobject that has a mask (forgotten) and drops the rest");
}

// ---- B-M2: an update that sends nothing copies nothing ------------------------------------------------------------------------------
static size_t g_allocated = 0;   // bytes the allocator was asked for since it was last reset

void* operator new(size_t n) {
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    g_allocated += n;
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
// C.O.A / Cyb

static void test_noop_update_copies_nothing() {
    std::printf("an update that sends nothing does not copy the shadow\n");
    Actor pawn = make_pawn();
    ActorShadow shadow;
    ExportAcks acks;
    open_actor(pawn, shadow, acks);
    g_allocated = 0;
    {
        const ActorShadow copy = shadow;
        (void)copy;
    }
    const size_t copy_cost = g_allocated;
    g_allocated = 0;
    bool failed = true;
    const auto e = update_actor(pawn, shadow, acks, &failed);
    const size_t noop_cost = g_allocated;
    std::printf("    copying the pawn's shadow allocates %zu bytes, an update that sends nothing %zu\n", copy_cost, noop_cost);
    CHECK(!e && !failed, "nothing changed: no emission");
    CHECK(copy_cost > 10000 && noop_cost * 8 < copy_cost, "and it allocates a fraction of what copying the shadow once would (the old code copied it twice)");
}

int main() {
    ObjectShadow invalidation;
    invalidation.rep.sent[1] = Value::boolean(true);
    invalidation.invalidate(65537);
    CHECK(invalidation.rep.sent.count(1) == 1, "out-of-range invalidation cannot alias a valid shadow handle");
    invalidation.invalidate(1);
    CHECK(invalidation.rep.sent.empty(), "valid invalidation still clears the shadow value");
    test_pc_open();
    test_player_state_open();
    test_pawn_open();
    test_pawn_update();
    test_exports();
    test_rpcs();
    test_shadow();
    t1_rollback_across_objects();
    t2_assembly_failure_rolls_back();
    t3_block_past_the_bunch();
    t9_rpc_on_schemaless_target();
    test_rpc_calls_checked();
    test_export_packing_rules();
    test_active_parents();
    test_noop_update_copies_nothing();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
