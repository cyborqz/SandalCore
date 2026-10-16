// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  server/worldd/tests/test_worldd_bootstrap.cpp
//
//  worldd's native bootstrap (server/worldd/bootstrap, plan A5): the sequence of
//  one joining player built from a PlayerSetup, against what the original
//  server sent (libs/aocnet/tests/capture_blocks.h, capture_aoc_codecs.h and
//  tests/capture_bootstrap.h, all cut from fixtures/replay_data.bin by
//  tools/capture/gen_all.py) and decoded with libaocnet's readers.
//
//  Golden: with the captured character's setup (captured_character_setup(): the
//  values the original sent for its character, RandomChar, and the capture's
//  GUIDs) the builder's chains equal the captured bits:
//    PlayerController open   pkt#22  exports (3545 bits) and content (1314)     bit for bit
//    ClientSetHUD            pkt#22  its export (873) and block (173)           bit for bit
//    pkt#23's six RPCs       the 3550-bit bunch whole; the 263-bit one but the clock (see below)
//    GameState               pkt#28  the export bunch (3473 bits), SerializeNewActor and the
//                            RepLayout block (Tier 2: all 33 handles, 7324 bits). EXCLUDED: the
//                            five fast arrays (estates, nodes, quests: world state)
//    load tracker 1011...1   pkt#72  its block; EXCLUDED: the quest and FTUE subobjects
//    pawn open               pkt#78  exports (6321) and content (29350)         bit for bit
//    storage payloads        pkt#82, #93, #97 (+ #597, #607 on the PC): the three empty ones
//                            bit for bit; the Equipment payload holds 13 items in the capture
//                            and is checked for its size
//    ClientRestart ...       pkt#97  the 268-bit bunch                           bit for bit
//    PC after possession     pkt#103 the actor block (1552 bits); EXCLUDED: XPTracker and the
//                            quest cache
//    pawn second state       pkt#107 exports (4209) and content (78235)         bit for bit
//    PlayerState open        pkt#434 exports (1177) and content (1453)          bit for bit
//    phase-in, combat echo   pkt#2523 (194 bits), pkt#620 (281 bits), unreliable in the original
//    move acknowledgements   pkt#1900, #1913, #1927 (68 bits)
//  The one value that is not the capture's: ClientSetTime. Its two numbers follow from the world time by a
//  linear formula read off the capture that holds to about 0.06 s, so the test checks its structure and
//  that it is within 0.5 s of the captured values, not its bits.
//
//  Offline harness: the whole join_sequence() in each GameState mode is decoded with read_content_blocks,
//  read_rep_block, read_fast_array_fields and read_rpc_fields: every block reads to its last bit, the classes
//  and their order are the spec's, key values are right, and every chain fits Connection's limits.
// ============================================================================
#include "bootstrap/bootstrap.h"
#include "bootstrap/captured_gamestate.h"
#include "bootstrap/captured_level_list.h"
#include "bootstrap/captured_replies.h"
#include "bootstrap/captured_setup.h"
#include "capture_aoc_codecs.h"
#include "capture_bootstrap.h"
#include "capture_lingering.h"
#include "net/connection.h"
#include "protocol/repl/replicator.h"
#include "protocol/schema/generated/index.h"
#include "protocol/server_guid_allocator.h"
#include "protocol/wire/packet_reader.h"

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace bs = aoc::worldd::bootstrap;
using namespace aoc::protocol;
using namespace aoc::protocol::repl;
using emit::BunchWriter;
using emit::FIntrepidNetworkGUID;
namespace schema = aoc::protocol::schema;
namespace gen = aoc::protocol::schema::gen;
namespace captured = aoc::worldd::bootstrap::captured;
using bs::PlayerBootstrap;
using bs::Role;
using bs::Send;
using bs::StorageKind;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

// ---- comparing with the capture ------------------------------------------------------------------------------------------------
// [Cyb ~ C O A]

struct Ref {
    const char* name;
    size_t      bits;
    const char* hex;
};

template <class B>
static Ref ref(const B& b) { return {b.name, b.bits, b.hex}; }

static bool same_as(const BunchWriter& w, const Ref& b) {
    const auto want = capture_blocks::bytes_of(b.hex);
    size_t diff = 0;
    if (w.bit_pos() != b.bits) {
        std::printf("    wrote %zu bits, expected %zu (%s)", w.bit_pos(), b.bits, b.name);
        const size_t n = w.bit_pos() < b.bits ? w.bit_pos() : b.bits;
        if (!capture_blocks::same_bit_range(w.data(), want.data(), n, &diff)) std::printf("; first differing bit %zu", diff);
        std::printf("\n");
        return false;
    }
    if (!capture_blocks::same_bit_range(w.data(), want.data(), b.bits, &diff)) {
        std::printf("    first differing bit %zu (%s)\n", diff, b.name);
        return false;
    }
    return true;
}

/// The first `n` bits of `w` and of `b` are equal.
static bool same_prefix(const BunchWriter& w, const Ref& b, size_t n) {
    if (w.bit_pos() < n || b.bits < n) {
        std::printf("    %zu / %zu bits, wanted a prefix of %zu (%s)\n", w.bit_pos(), b.bits, n, b.name);
        return false;
    }
    const auto want = capture_blocks::bytes_of(b.hex);
    size_t diff = 0;
    if (!capture_blocks::same_bit_range(w.data(), want.data(), n, &diff)) {
        std::printf("    first differing bit %zu (%s)\n", diff, b.name);
        return false;
    }
    return true;
}

/// `s` carries exactly these export bunches and this payload.
static bool emitted_as(const std::optional<Send>& s, std::initializer_list<Ref> exports, const Ref& content) {
    if (!s || s->emission.exports.size() != exports.size()) {
        std::printf("    %zu export bunches, expected %zu\n", s ? s->emission.exports.size() : 0, exports.size());
        return false;
    }
    size_t i = 0;
    for (const Ref& x : exports) {
        if (!same_as(s->emission.exports[i++], x)) return false;
    }
    return same_as(s->emission.payload, content);
}

static BunchWriter joined(std::initializer_list<Ref> parts) {
    BunchWriter w;
    for (const Ref& b : parts) {
        const auto bytes = capture_blocks::bytes_of(b.hex);
        w.write_bit_range(bytes.data(), 0, b.bits);
    }
    return w;
}

static FIntrepidNetworkGUID guid(uint64_t id, uint32_t server = 0, uint32_t rnd = 0) {
    FIntrepidNetworkGUID g;
    g.ObjectId = id;
    g.ServerId = server;
    g.Randomizer = rnd;
    return g;
}

static bs::PlayerSetup captured_setup() {
    auto s = bs::captured_character_setup();
    if (!s) {
        std::printf("  [FAIL] the captured character does not build\n");
        std::exit(2);
    }
    return std::move(*s);
}

/// A bootstrap of the captured character; every step fresh, with no export acknowledged.
static PlayerBootstrap captured_bootstrap(bs::GameStateMode mode = bs::GameStateMode::Tier2) {
    PlayerBootstrap pb(captured_setup(), mode);
    if (!pb.ok()) {
        std::printf("  [FAIL] the captured character's bootstrap does not build: %s\n", pb.error().c_str());
        std::exit(2);
    }
    return pb;
}

// ---- the golden chains ---------------------------------------------------------------------------------------------------------

static void test_player_controller() {
    std::printf("PlayerController open and its RPCs (pkt#22, #23)\n");
    PlayerBootstrap pb = captured_bootstrap();
    const auto open = pb.player_controller_open();
    CHECK(open && open->role == Role::PlayerController && open->opens_channel && open->reliable(), "the open is the PlayerController's first chain, reliable");
    CHECK(emitted_as(open, {ref(capture_blocks::kPcOpenExports0)}, ref(capture_blocks::kPcOpenContent)),
          "pkt#22: the 3545-bit export bunch and the 1314-bit payload (7 properties, GlobalGMCommands) match bit for bit");
    CHECK(open && open->emission.exported.size() == 7 && open->emission.exported_by_bunch.size() == 1, "7 GUIDs exported with their path, in one bunch");

    const auto hud = pb.hud();
    CHECK(hud && hud->role == Role::PlayerController && !hud->opens_channel &&
              emitted_as(hud, {ref(capture_blocks::kRpcClientSetHUDExports)}, ref(capture_blocks::kRpcClientSetHUD)),
          "pkt#22: ClientSetHUD, the 873-bit class export and the 173-bit block, bit for bit");

    const auto first = pb.voice_and_level_list();
    const BunchWriter want958 = joined({ref(capture_blocks::kRpcClientVoiceHandshakeComplete), ref(capture_blocks::kRpcClientEnableNetworkVoice),
                                        ref(capture_blocks::kRpcClientUpdateMultipleLevelsStreamingStatusCompressed)});
    CHECK(first && first->emission.exports.empty() && first->reliable() && first->emission.payload.bit_pos() == 3550 &&
              capture_blocks::same_bit_range(first->emission.payload.data(), want958.data(), 3550, nullptr),
          "pkt#23 chSeq 958: voice handshake, voice off and the captured 429-byte level list, 3550 bits, bit for bit");

    const auto second = pb.flush_bandwidth_and_time();
    const BunchWriter want959 = joined({ref(capture_blocks::kRpcClientFlushLevelStreaming), ref(capture_blocks::kRpcClientCapBandwidth),
                                        ref(capture_blocks::kRpcClientSetTime)});
    CHECK(second && second->emission.payload.bit_pos() == 263 &&
              capture_blocks::same_bit_range(second->emission.payload.data(), want959.data(), 89, nullptr),
          "pkt#23 chSeq 959: 263 bits; ClientFlushLevelStreaming and ClientCapBandwidth(200000) equal the capture's 89 bits");
    // ClientSetTime: the values within the formula's accuracy of the captured ones
    if (second) {
        const schema::ClassSchema& cls = *pb.player_controller().obj.schema;
        const auto blocks = read_content_blocks(second->emission.payload.data(), 263, 0);
        size_t p = blocks && blocks->size() == 3 ? (*blocks)[2].payload_pos : 0;
        const auto calls = p ? read_rpc_fields(cls, second->emission.payload.data(), p + (*blocks)[2].payload_bits, p) : std::nullopt;
        const RpcCall* set_time = calls && calls->size() == 1 ? &(*calls)[0] : nullptr;
        const double* g = set_time && set_time->decoded ? set_time->params.values.at(1).get_if<double>() : nullptr;
        const double* y = set_time && set_time->decoded ? set_time->params.values.at(2).get_if<double>() : nullptr;
        CHECK(g && y && std::string(set_time->rpc->name) == "ClientSetTime" && std::fabs(*g - 174944068.06996086) < 0.5 &&
                  std::fabs(*y - 15061647.01444424) < 0.5,
              "ClientSetTime from the world time: NewGameTime and NewSynchronizedTime within 0.5 s of pkt#23's (the formula's accuracy)");
    }
    const bs::ClientTimes t = bs::client_times(1593462.375);
    CHECK(std::fabs(t.synchronized_time - (1593462.375 + 13468184.6)) < 1e-6 && std::fabs(t.game_time - (9.0 * t.synchronized_time + 39389245.0)) < 1e-6,
          "client_times: synchronized = world + 13468184.6, game = 9 x synchronized + 39389245");
}

static void test_game_state() {
    std::printf("GameState open (pkt#28)\n");
    // The original's world time at the open (80 ms after the PlayerController's).
    bs::PlayerSetup setup = captured_setup();
    setup.world_time = std::bit_cast<double>(captured::kGameStateWorldTimeBits);
    PlayerBootstrap pb(setup, bs::GameStateMode::Tier2);
    CHECK(pb.ok(), "the Tier 2 GameState builds");
    const auto open = pb.game_state_open();
    CHECK(open && open->role == Role::GameState && open->opens_channel && open->reliable(), "the open is the GameState's first chain, reliable");
    CHECK(open && open->emission.exports.size() == 1 && same_as(open->emission.exports[0], ref(captured::kGameStateOpenExports)),
          "the export bunch (the CDO, the level, AoCGameModeBaseBP_C) equals the original's 3473 bits");
    CHECK(open && open->emission.exported.size() == 7, "7 GUIDs exported with their path");
    if (!open) return;
    // SerializeNewActor is 3 GUIDs and 4 absent bits; then the actor's block: the RepLayout part up to the fast arrays
    const auto ours = read_content_blocks(open->emission.payload.data(), open->emission.payload.bit_pos(), 388);
    const auto bytes = captured::bytes_of(captured::kGameStateOpenContent);
    const auto theirs = read_content_blocks(bytes.data(), captured::kGameStateOpenContent.bits, 388);
    CHECK(ours && theirs && ours->size() == 1 && theirs->size() == 1 && (*ours)[0].header.is_actor && (*theirs)[0].header.is_actor,
          "SerializeNewActor is 388 bits and one actor block follows, in both");
    CHECK(capture_blocks::same_bit_range(open->emission.payload.data(), bytes.data(), 388, nullptr),
          "SerializeNewActor (the GameState GUID, the archetype, the level, nothing else) equals the original's");
    CHECK(ours && (*ours)[0].payload_bits == captured::kGameStateRepLayoutBits, "Tier 2's block is the whole RepLayout part: 7324 bits");
    if (ours && theirs) {
        // compare the two RepLayout blocks bit by bit, each from its own payload start
        BunchWriter a, b;
        a.write_bit_range(open->emission.payload.data(), (*ours)[0].payload_pos, captured::kGameStateRepLayoutBits);
        b.write_bit_range(bytes.data(), (*theirs)[0].payload_pos, captured::kGameStateRepLayoutBits);
        CHECK(a.bit_pos() == b.bit_pos() && capture_blocks::same_bit_range(a.data(), b.data(), a.bit_pos(), nullptr),
              "the RepLayout block (33 handles: the 6 of Tier 1, GameTimeScalar, the 25 CombatSettings values, ServiceConnectionFlags) equals the original's, bit for bit");
        CHECK((*theirs)[0].payload_bits > (*ours)[0].payload_bits, "excluded: the original's actor block goes on with five fast arrays (123,427 bits in all)");
    }
}
// C.O.A captures, C_Y_B code

static void test_load_tracker_and_pawn_open() {
    std::printf("CharacterLoadTracker 1011...1 (pkt#72), the pawn's open (pkt#78) and its storage payloads\n");
    PlayerBootstrap pb = captured_bootstrap();
    pb.player_controller_open();   // the update is what changed since the open
    const auto tracker = pb.load_tracker_before_possession();
    CHECK(tracker && tracker->role == Role::PlayerController && tracker->emission.exports.empty() &&
              same_as(tracker->emission.payload, ref(capture_bootstrap::kPcTrackerBlock)),
          "pkt#72: the PlayerController's block with CharacterLoadTracker 1011...1 (841 bits) equals the original's; excluded: the quest and FTUE subobjects after it");

    const auto open = pb.pawn_open();
    CHECK(open && open->role == Role::Pawn && open->opens_channel, "the pawn's open is the pawn channel's first chain");
    CHECK(emitted_as(open, {ref(capture_blocks::kPawnOpenExports0)}, ref(capture_blocks::kPawnOpenContent)),
          "pkt#78: the 6321-bit export bunch (8 components) and the 29350-bit payload (the actor, 8 components, the 26,861-bit customization) match bit for bit");

    auto payload = [&](StorageKind k, Role r) { return pb.storage_payload(k, r); };
    // the parameters' bits: 692 header bits + one 178-bit slot per MaximumItems + 145 trailing bits (transcript section 1.3)
    auto param_bits = [&](const std::optional<Send>& s) -> size_t {
        if (!s) return 0;
        const auto blocks = read_content_blocks(s->emission.payload.data(), s->emission.payload.bit_pos(), 0);
        if (!blocks || blocks->size() != 1) return 0;
        size_t pos = (*blocks)[0].payload_pos;
        const auto h = read_field_header(*pb.pawn().obj.schema, s->emission.payload.data(), pos + (*blocks)[0].payload_bits, pos);
        return h ? h->payload_bits : 0;
    };
    const auto backpack = payload(StorageKind::Backpack, Role::Pawn);
    CHECK(param_bits(backpack) == 692 + 70 * 178 + 145 && param_bits(payload(StorageKind::Equipment, Role::Pawn)) == 692 + 83 * 178 + 145,
          "the parameters of an empty storage are 692 + slots x 178 + 145 bits (13297 for the Backpack's 70 slots)");
    CHECK(emitted_as(backpack, {ref(capture_bootstrap::kPawnBackpackPayloadExports0)}, ref(capture_bootstrap::kPawnBackpackPayloadContent)),
          "pkt#82: the empty Backpack payload (692 + 70 x 178 + 145 bits) and the component's export equal the original's");
    const auto equipment = payload(StorageKind::Equipment, Role::Pawn);
    // the capture's Equipment holds 13 items: the empty one is checked for its size alone
    CHECK(equipment && equipment->emission.exports.size() == 1 && equipment->emission.payload.bit_pos() == 18 + 23 + 692 + 83 * 178 + 145,
          "pkt#89: the empty Equipment payload (83 slots) is 692 + 83 x 178 + 145 bits in an 18-bit block header and a 23-bit field header (the original's holds 13 items: excluded)");
    const auto quest = payload(StorageKind::QuestStorage, Role::Pawn);
    CHECK(emitted_as(quest, {ref(capture_bootstrap::kPawnQuestPayloadExports0)}, ref(capture_bootstrap::kPawnQuestPayloadContent)),
          "pkt#93: the empty QuestStorage payload (100 slots) equals the original's");
    const auto resource = payload(StorageKind::Resource0, Role::Pawn);
    CHECK(emitted_as(resource, {}, ref(capture_bootstrap::kPawnResourcePayloadContent)),
          "pkt#97: the empty resource storage payload (24 slots, Dimension 6 x 4, no export) equals the original's");

    // the same payloads on the PlayerController, the replies to ServerRequestClientLoad
    const auto pc_backpack = payload(StorageKind::Backpack, Role::PlayerController);
    CHECK(pc_backpack && pc_backpack->role == Role::PlayerController &&
              emitted_as(pc_backpack, {ref(capture_bootstrap::kPcBackpackPayloadExports0)}, ref(capture_bootstrap::kPcBackpackPayloadContent)),
          "pkt#597: the Backpack payload on the PlayerController equals the original's");
    const auto pc_resource = payload(StorageKind::Resource0, Role::PlayerController);
    CHECK(emitted_as(pc_resource, {}, ref(capture_bootstrap::kPcResourcePayloadContent)), "pkt#607: the resource payload on the PlayerController equals the original's");
    const auto reply = payload(StorageKind::Resource0, Role::Pawn);
    CHECK(emitted_as(reply, {}, ref(capture_bootstrap::kPawnResourceReplyContent)), "pkt#773: the reply to ServerRequestStorageData equals the original's");
    CHECK(!pb.storage_payload(StorageKind::Backpack, Role::PlayerState) && !pb.storage_payload(StorageKind::Backpack, Role::GameState),
          "a storage payload goes to the pawn or the PlayerController only");

    // once the component's export is acknowledged only the block goes
    pb.mark_exported(backpack->emission.exported_by_bunch[0]);
    const auto quiet = payload(StorageKind::Backpack, Role::Pawn);
    CHECK(quiet && quiet->emission.exports.empty() && quiet->emission.payload.bit_pos() == capture_bootstrap::kPawnBackpackPayloadContent.bits,
          "with the export acknowledged the payload has no export bunch");
}

static void test_possession() {
    std::printf("possession: ClientRestart (pkt#97), the PlayerController (pkt#103), the pawn's second state (pkt#107)\n");
    PlayerBootstrap pb = captured_bootstrap();
    pb.player_controller_open();
    pb.load_tracker_before_possession();
    pb.pawn_open();
    const auto restart = pb.restart_camera_rotation();
    CHECK(restart && restart->emission.exports.empty() && restart->emission.payload.bit_pos() == 268 &&
              same_as(restart->emission.payload, ref(capture_bootstrap::kPcRestartContent)),
          "pkt#97: ClientRestart(pawn), ClientSetCameraMode(EName 204) and ClientSetRotation((0, 16550, 0), true): 268 bits, bit for bit");
    CHECK(restart && same_prefix(restart->emission.payload, ref(capture_blocks::kRpcClientRestart), 173), "ClientRestart's block is the captured 173 bits");

    const auto update = pb.possession_update();
    CHECK(update && update->role == Role::PlayerController && update->emission.exports.size() == 1 && update->emission.exported.size() == 1 &&
              update->emission.exported[0].ObjectId == 10341558,
          "pkt#103: the update exports CollisionCylinder (the component the PlayerController attaches to) and nothing else");
    CHECK(update && same_as(update->emission.payload, ref(capture_bootstrap::kPcPossessionBlock)),
          "pkt#103: the actor block (Pawn, ControllersOriginalPawn, Name, AttachmentReplication, CharacterLoadTracker 0111...1; 1552 bits) equals the original's; excluded: XPTracker and the quest cache");
    CHECK(!pb.possession_update(), "a second update has nothing to send");

    const auto second = pb.pawn_second_state();
    CHECK(emitted_as(second, {ref(capture_blocks::kPawnPossessedExports0)}, ref(capture_blocks::kPawnPossessedContent)),
          "pkt#107: the 4209-bit export bunch (8 entries) and the 78235-bit payload (RemoteRole 2, Instigator, PlayerState, Controller, bHidden 1, Alignment, "
          "BaseCharacterInfo, Ability, Stats with 442 stats, OwnerInfo, appearance ids, the dynamic resource storage) match bit for bit");
    CHECK(second && second->emission.exported.size() == 9, "nine GUIDs exported with their path: seven components, the resource storage class and its package");
    CHECK(!pb.pawn_second_state(), "the second state is sent once");
}

static void test_player_state() {
    std::printf("PlayerState open (pkt#434)\n");
    PlayerBootstrap pb = captured_bootstrap();
    pb.export_acks().mark(guid(16442478405498561049ull));   // PersistentLevel: exported by the opens before, acknowledged by pkt#434
    const auto open = pb.player_state_open();
    CHECK(open && open->role == Role::PlayerState && open->opens_channel &&
              emitted_as(open, {ref(capture_blocks::kPlayerStateOpenExports0)}, ref(capture_blocks::kPlayerStateOpenContent)),
          "pkt#434: the archetype's export (1177 bits) and the 1453-bit payload (PlayerId 1874, the login's name and unique id) match bit for bit");

    // the name is cut to 20 characters; the unique id is whole
    bs::PlayerSetup s = captured_setup();
    s.login.name_option = "SHORT";
    s.login.unique_id = "SHORT-ID";
    PlayerBootstrap other(s, bs::GameStateMode::Tier1);
    const Value* name = other.player_state().obj.state.get(*other.player_state().obj.schema, "PlayerNamePrivate");
    CHECK(name && *name == Value::str("SHORT"), "a name shorter than 20 characters is whole");
    s.login.name_option = std::string(19, 'a') + "\xC3\xA9" + "bcd";   // 20 characters, the 20th two bytes
    PlayerBootstrap utf(s, bs::GameStateMode::Tier1);
    const Value* cut = utf.player_state().obj.state.get(*utf.player_state().obj.schema, "PlayerNamePrivate");
    CHECK(cut && *cut == Value::str(std::string(19, 'a') + "\xC3\xA9"), "the name is cut at 20 characters, never inside a UTF-8 sequence");
}

static void test_replies() {
    std::printf("the replies: phase-in, combat settings, group and action bar, move acknowledgements\n");
    PlayerBootstrap pb = captured_bootstrap();
    CHECK(!pb.phase_in(), "no phase-in before the pawn's second state");
    pb.player_controller_open();   // later updates are what changed since the open
    pb.pawn_open();
    pb.pawn_second_state();
    pb.mark_exported(std::vector<FIntrepidNetworkGUID>{guid(10341540, 60, 1860730596u), guid(10341538, 60, 1860730596u)});   // BaseCharacterInfo acknowledged
    const auto phase = pb.phase_in();
    CHECK(phase && phase->role == Role::Pawn && !phase->opens_channel && phase->emission.exports.empty() &&
              same_as(phase->emission.payload, ref(capture_bootstrap::kPhaseIn)),
          "pkt#2523: bHidden 0 and BaseCharacterInfo bIsPhasedOut 0 / bClientIsReadyToPhaseIn 1: the original's 194 bits");
    CHECK(phase && phase->reliable(), "sent reliable (the original's was unreliable): a lost update could not be recovered from the shadow");
    CHECK(!pb.phase_in(), "nothing changed: no second phase-in");
// C.O.A and C Y B

    // the nine settings pkt#620 carries, as the client's request would give them
    std::vector<bs::PropertyValue> values = {
        {"CharacterInGameSettings.CombatSettings.Targeting.TabTarget.MayHarmAngle", Value::f32(140)},
        {"CharacterInGameSettings.CombatSettings.Targeting.TabTarget.ProximityRadius", Value::f32(400)},
        {"CharacterInGameSettings.CombatSettings.Targeting.TabTarget.MaxDistance", Value::f32(3500)},
        {"CharacterInGameSettings.CombatSettings.Targeting.TabTarget.ForwardOffset", Value::f32(0)},
        {"CharacterInGameSettings.CombatSettings.Targeting.TabTarget.TabTargetMethod", Value::u64(3)},
        {"CharacterInGameSettings.CombatSettings.Targeting.bEnableHoverCasting", Value::boolean(false)},
        {"CharacterInGameSettings.CombatSettings.Targeting.SoftLock.VisualsMask", Value::u64(3)},
        {"CharacterInGameSettings.CombatSettings.Input.BufferDuration", Value::f32(0.5f)},
        {"CharacterInGameSettings.CombatSettings.Rendering.PartyVFX", Value::u64(1)},
    };
    const auto echo = pb.combat_settings_echo(values);
    CHECK(echo && echo->role == Role::PlayerController && echo->emission.exports.empty() && same_as(echo->emission.payload, ref(capture_bootstrap::kCombatEcho)),
          "pkt#620: the nine CombatSettings values: the original's 281 bits");
    CHECK(!pb.combat_settings_echo(values), "the same values again: nothing to send (the original resent them at pkt#774; its shadow is the replicator's)");
    std::vector<bs::PropertyValue> bad = {{"NoSuchProperty", Value::f32(1)}};
    CHECK(!pb.combat_settings_echo(bad), "a path the PlayerController does not have is refused");

    // the group and the action bar are the original's blocks
    const auto group = pb.empty_group();
    const auto bar = pb.default_action_bar();
    auto rpc_name = [&](const std::optional<Send>& s) -> std::string {
        if (!s) return "";
        const auto blocks = read_content_blocks(s->emission.payload.data(), s->emission.payload.bit_pos(), 0);
        if (!blocks || blocks->size() != 1) return "";
        size_t pos = (*blocks)[0].payload_pos;
        const auto calls = read_rpc_fields(*pb.player_controller().obj.schema, s->emission.payload.data(), pos + (*blocks)[0].payload_bits, pos);
        return calls && calls->size() == 1 ? (*calls)[0].rpc->name : "";
    };
    CHECK(group && group->emission.payload.bit_pos() == 527 && rpc_name(group) == "ClientUpdateGroup", "ClientUpdateGroup: the empty group, 527 bits (pkt#606)");
    CHECK(bar && bar->emission.payload.bit_pos() == 2537 && rpc_name(bar) == "ClientReceiveActionbarSlotPayload", "ClientReceiveActionbarSlotPayload: the default action bar, 2537 bits (pkt#77)");

    // ClientMoveResponsePacked, the acknowledgement of a good move: the original's three first
    for (const Ref& r : {ref(capture_bootstrap::kMoveResponse1), ref(capture_bootstrap::kMoveResponse2), ref(capture_bootstrap::kMoveResponse3)}) {
        const auto bytes = capture_blocks::bytes_of(r.hex);
        wire::PacketReader rd(bytes.data(), bytes.size(), r.bits);
        rd.read_bits(36);   // block header (2 + 8), field header (8 + 8), the 10-bit prefix is the next 10: 36 bits in all, 26 + 10
        const uint32_t bits = static_cast<uint32_t>(rd.read_bits(32));
        const auto ack = pb.good_move_ack(std::bit_cast<float>(bits));
        CHECK(ack && ack->role == Role::Pawn && !ack->reliable() && ack->emission.exports.empty() && same_as(ack->emission.payload, r),
              (std::string("the acknowledgement of a good move equals the original's 68 bits (") + r.name + ")").c_str());
    }
}

// ---- the decoder ---------------------------------------------------------------------------------------------------------------

static CodecOptions stat_options() {
    CodecOptions o;
    o.stat_int32_extended = [](int64_t record) {
        const uint32_t hi = static_cast<uint32_t>(static_cast<uint64_t>(record) >> 32);
        return hi == 0x5429e8c0u || hi == 0x5429e8c4u;
    };
    return o;
}

struct Block {
    bool                       is_actor = true;
    bool                       has_rep = false;
    bool                       dynamic = false;
    FIntrepidNetworkGUID       object;
    std::string                name;          // the component's name, "" for the actor
    std::optional<RepState>    rep;
    std::vector<FastArrayField> fast;
    std::vector<RpcCall>       rpcs;
    size_t                     payload_bits = 0;
    bool                       read_to_end = false;
    std::vector<std::pair<uint32_t, size_t>> raw_fields;   // a field loop the readers do not model: (field, bits)
};

struct Decoded {
    bool                 ok = false;
    FIntrepidNetworkGUID actor;
    std::vector<Block>   blocks;
    size_t               new_actor_bits = 0;
    std::optional<QuantVec> location;
    std::optional<Rot>   rotation;
};

static const schema::ClassSchema* class_of_role(Role r) {
    switch (r) {
        case Role::PlayerController: return gen::find_class_schema("AoCPlayerControllerBP_C");
        case Role::GameState: return gen::find_class_schema("AoCGameStateBP_C");
        case Role::Pawn: return gen::find_class_schema("PlayerPawn_C");
        case Role::PlayerState: return gen::find_class_schema("AoCPlayerStateBP_C");
    }
    return nullptr;
}

/// UPackageMapClient::SerializeNewActor (replicator.cpp write_new_actor, tools/decode_actor_open.py read_new_actor) and a
/// PlayerController's two extra fields.
static bool read_new_actor(const BunchWriter& w, bool player_controller, Decoded& d) {
    wire::PacketReader r(w.data(), w.byte_size(), w.bit_pos());
    auto read_guid = [&] {
        FIntrepidNetworkGUID g;
        const uint64_t lo = r.read_uint32(), hi = r.read_uint32();
        g.ObjectId = lo | (hi << 32);
        g.ServerId = r.read_uint32();
        g.Randomizer = r.read_uint32();
        return g;
    };
    d.actor = read_guid();
    read_guid();   // the archetype
    read_guid();   // the level
    auto vector = [&](std::optional<QuantVec>* out) {
        if (!r.read_bit()) return true;
        if (!r.read_bit()) return false;   // every captured open quantizes
        const auto v = read_quantized_vector(r);
        if (!v) return false;
        if (out) *out = *v;
        return true;
    };
    if (!vector(&d.location)) return false;
    if (r.read_bit()) {
        const auto rot = read_struct("FRotator", r);
        const Rot* p = rot ? rot->get_if<Rot>() : nullptr;
        if (!p) return false;
        d.rotation = *p;
    }
    if (!vector(nullptr) || !vector(nullptr)) return false;
    if (player_controller) {
        r.read_uint8();
        r.read_uint32();
    }
    if (r.overflowed()) return false;
    d.new_actor_bits = r.pos();
    return true;
}

static const schema::ClassSchema* subobject_class(const PlayerBootstrap& pb, const FIntrepidNetworkGUID& g, std::string* name, bool* dynamic) {
    for (const Actor* a : {&pb.pawn(), &pb.player_controller()}) {
        if (const Subobject* s = a->find(g)) {
            *dynamic = s->dynamic_class.has_value();
            *name = s->dynamic_class ? s->dynamic_class->front().name : s->name;
            return s->obj.schema;
        }
    }
    return nullptr;
}
// -- C | O | A x C.Y.B --

/// Every block of the send, read to its last bit with libaocnet's readers.
static Decoded decode(const PlayerBootstrap& pb, const Send& s) {
    Decoded d;
    const BunchWriter& w = s.emission.payload;
    size_t pos = 0;
    if (s.opens_channel) {
        if (!read_new_actor(w, s.role == Role::PlayerController, d)) return d;
        pos = d.new_actor_bits;
    }
    const auto views = read_content_blocks(w.data(), w.bit_pos(), pos);
    if (!views) return d;
    const CodecOptions opts = stat_options();
    d.ok = true;
    for (const BlockView& v : *views) {
        Block b;
        b.is_actor = v.header.is_actor;
        b.has_rep = v.header.has_rep_layout;
        b.object = v.header.object;
        b.payload_bits = v.payload_bits;
        const schema::ClassSchema* cls = nullptr;
        if (v.header.is_actor) {
            cls = class_of_role(s.role);
        } else {
            cls = subobject_class(pb, v.header.object, &b.name, &b.dynamic);
        }
        size_t p = v.payload_pos;
        const size_t end = v.payload_pos + v.payload_bits;
        if (!cls) {
            b.read_to_end = v.payload_bits == 0;   // GlobalGMCommands: no schema, nothing to say
        } else if (v.header.has_rep_layout) {
            b.rep = read_rep_block(*cls, w.data(), end, p, opts);
            if (b.rep && p < end) {
                size_t q = p;
                const auto f = read_fast_array_fields(*cls, w.data(), end, q, opts);
                if (f) {
                    b.fast = *f;
                    p = q;
                } else {   // a field loop the readers do not model (the captured GameState's quest cache): list the fields
                    size_t r = p;
                    while (r < end) {
                        const auto h = read_field_header(*cls, w.data(), end, r);
                        if (!h) break;
                        b.raw_fields.emplace_back(h->field, h->payload_bits);
                        r += h->payload_bits;
                    }
                    p = r;
                }
            }
            b.read_to_end = b.rep.has_value() && p == end;
        } else if (v.payload_bits == 0) {
            b.read_to_end = true;
        } else {
            size_t q = p;
            if (const auto f = read_fast_array_fields(*cls, w.data(), end, q, opts)) {
                b.fast = *f;
                p = q;
            } else {
                q = p;
                if (const auto r = read_rpc_fields(*cls, w.data(), end, q, opts)) {
                    b.rpcs = *r;
                    p = q;
                }
            }
            b.read_to_end = p == end;
        }
        if (!v.header.is_actor && b.name.empty()) b.name = "?";
        d.blocks.push_back(std::move(b));
    }
    return d;
}

/// "actor(rep) sub:BaseCharacterInfo(rep+fast) ..." / "actor.ClientSetHUD".
static std::string summary(const Decoded& d) {
    std::string out;
    for (const Block& b : d.blocks) {
        if (!out.empty()) out += ' ';
        out += b.is_actor ? "actor" : "sub:" + b.name;
        if (!b.rpcs.empty()) {
            for (const RpcCall& c : b.rpcs) out += std::string(".") + c.rpc->name;
        } else if (b.payload_bits == 0) {
            out += "(empty)";
        } else if (b.has_rep) {
            out += (b.fast.empty() && b.raw_fields.empty()) ? "(rep)" : "(rep+fast)";
        } else {
            out += "(fast)";
        }
    }
    return out;
}

static bool all_read(const Decoded& d) {
    if (!d.ok) return false;
    for (const Block& b : d.blocks) {
        if (!b.read_to_end) return false;
    }
    return !d.blocks.empty();
}
// == C.Y.B | C O A ==

static const Value* value_of(const Block& b, const schema::ClassSchema& cls, std::string_view path) {
    return b.rep ? b.rep->get(cls, path) : nullptr;
}

/// A chain's parts as Connection splits it: each export bunch, then the content whole up to kMaxSingleBunchBits, else
/// parts of kMaxPartialBunchBits.
static size_t parts_of(const Send& s) {
    const size_t bits = s.emission.payload.bit_pos();
    const size_t content = bits <= aoc::worldd::Connection::kMaxSingleBunchBits ? 1 : (bits + 7615) / 7616;
    return s.emission.exports.size() + content;
}

static std::optional<std::vector<Send>> sequence_of(PlayerBootstrap& pb) {
    auto seq = pb.join_sequence();
    if (!seq) std::printf("    join_sequence failed (%s)\n", pb.error().c_str());
    return seq;
}

static void check_limits(const std::vector<Send>& seq, const char* what) {
    bool parts_ok = true, export_ok = true, shape_ok = true;
    std::map<Role, size_t> per_channel;
    static const char* const kRole[] = {"PC", "GameState", "pawn", "PlayerState"};
    size_t total_bits = 0;
    for (const Send& s : seq) {
        size_t export_bits = 0;
        for (const BunchWriter& e : s.emission.exports) export_bits += e.bit_pos();
        total_bits += export_bits + s.emission.payload.bit_pos();
        std::printf("    %-11s %-40s exports %5zu bits, payload %6zu bits, %2zu parts\n", kRole[static_cast<size_t>(s.role)], s.what,
                    export_bits, s.emission.payload.bit_pos(), parts_of(s));
    }
    std::printf("    in all %zu bits (%.1f KB)\n", total_bits, total_bits / 8192.0);
    for (const Send& s : seq) {
        const size_t parts = parts_of(s);
        if (parts > aoc::worldd::Connection::kMaxUnackedReliable) parts_ok = false;
        for (const BunchWriter& e : s.emission.exports) {
            if (e.bit_pos() > aoc::worldd::Connection::kMaxSingleBunchBits || e.bit_pos() == 0) export_ok = false;
        }
        if (s.emission.exported_by_bunch.size() != s.emission.exports.size()) shape_ok = false;
        if (s.emission.payload.bit_pos() == 0 || s.emission.payload.bit_pos() > aoc::worldd::Connection::kMaxPartialBits) shape_ok = false;
        if (s.reliable()) per_channel[s.role] += parts;
    }
    bool window_ok = true;
    size_t worst = 0;
    for (const auto& [role, parts] : per_channel) {
        worst = std::max(worst, parts);
        if (parts > aoc::worldd::Connection::kMaxUnackedReliable) window_ok = false;
    }
    char msg[200];
    std::snprintf(msg, sizeof msg, "%s: every chain has at most 256 parts and every export bunch at most 7621 bits", what);
    CHECK(parts_ok && export_ok && shape_ok, msg);
    std::snprintf(msg, sizeof msg, "%s: no channel has more than 256 reliable parts in flight if nothing is acknowledged (the most: %zu)", what, worst);
    CHECK(window_ok, msg);
}

// ---- the whole sequence ------------------------------------------------------------------------------------------------

static void test_sequence(bs::GameStateMode mode, const char* name) {
    std::printf("the join sequence, GameState %s\n", name);
    PlayerBootstrap pb = captured_bootstrap(mode);
    const auto seq = sequence_of(pb);
    CHECK(seq.has_value() && seq->size() == 16, "join_sequence: sixteen chains (plan D1 added the XPTracker)");
    if (!seq || seq->size() != 16) return;
    const std::vector<Send>& s = *seq;

    // order, roles and which chains open a channel
    const Role roles[] = {Role::PlayerController, Role::PlayerController, Role::PlayerController, Role::PlayerController, Role::GameState,
                          Role::PlayerController, Role::Pawn, Role::Pawn, Role::Pawn, Role::Pawn, Role::Pawn, Role::PlayerController,
                          Role::PlayerController, Role::Pawn, Role::PlayerState, Role::PlayerController};
    bool order = true;
    for (size_t i = 0; i < s.size(); ++i) order = order && s[i].role == roles[i];
    CHECK(order, "order: PlayerController open, HUD, two RPC bunches, GameState, tracker, pawn open, four storage payloads, restart, possession, pawn second state, PlayerState, the XPTracker");
    CHECK(s[0].opens_channel && s[4].opens_channel && s[6].opens_channel && s[14].opens_channel,
          "the four actors' first chains open their channels");
    int opens = 0;
    for (const Send& x : s) opens += x.opens_channel ? 1 : 0;
    CHECK(opens == 4, "and no other chain does");
    bool reliable = true;
    for (const Send& x : s) reliable = reliable && x.reliable();
    CHECK(reliable, "every chain of the join is reliable");

    std::vector<Decoded> d;
    for (const Send& x : s) d.push_back(decode(pb, x));
    bool all = true;
    for (size_t i = 0; i < d.size(); ++i) {
        if (!all_read(d[i])) {
            std::printf("    chain %zu (%s) does not read to its last bit\n", i, s[i].what);
            all = false;
        }
    }
    CHECK(all, "every block of every chain reads to its last bit with read_content_blocks / read_rep_block / read_fast_array_fields / read_rpc_fields");
// [C O A x Cyb]

    // classes and order of the blocks
    CHECK(summary(d[0]) == "actor(rep) sub:GlobalGMCommands(empty)", "PlayerController open: the actor's properties and the dynamic GlobalGMCommands");
    CHECK(summary(d[1]) == "actor.ClientSetHUD", "ClientSetHUD");
    // one content block per RPC call
    CHECK(summary(d[2]) == "actor.ClientVoiceHandshakeComplete actor.ClientEnableNetworkVoice actor.ClientUpdateMultipleLevelsStreamingStatusCompressed",
          "voice handshake, voice, the level list");
    CHECK(summary(d[3]) == "actor.ClientFlushLevelStreaming actor.ClientCapBandwidth actor.ClientSetTime", "flush, bandwidth cap, clock");
    CHECK(summary(d[4]) == "actor(rep)" || summary(d[4]) == "actor(rep+fast)", "GameState: one actor block");
    CHECK(summary(d[5]) == "actor(rep)", "the load tracker: the PlayerController's block");
    CHECK(summary(d[6]) == "actor(rep) sub:BaseCharacterInfo(rep) sub:CombatInfo(rep) sub:OwnerInfo(empty) sub:BackpackComponent(empty) "
                           "sub:EquipmentComponent(empty) sub:QuestStorageComponent(empty) sub:RewardStorageComponent(empty) sub:Character Appearance(rep)",
          "pawn open: the actor and its eight components in the original's order");
    for (size_t i = 7; i <= 10; ++i) CHECK(summary(d[i]) == "actor.ClientReceiveStoragePayload", "a ClientReceiveStoragePayload on the pawn");
    CHECK(summary(d[11]) == "actor.ClientRestart actor.ClientSetCameraMode actor.ClientSetRotation", "ClientRestart, camera mode, rotation");
    CHECK(summary(d[12]) == "actor(rep)", "the PlayerController after possession");
    CHECK(summary(d[13]) == "actor(rep) sub:AlignmentComponent(rep) sub:BaseCharacterInfo(rep+fast) sub:CombatInfo(rep) sub:AbilityComponent(rep) "
                            "sub:StatsComponent(fast) sub:OwnerInfo(rep) sub:Character Appearance(rep) sub:ResourceStorageComponent(rep)",
          "pawn second state: the actor, Alignment, BaseCharacterInfo with HeldItems, CombatInfo, Ability, Stats, OwnerInfo, appearance ids, the resource storage");
    CHECK(summary(d[14]) == "actor(rep)", "PlayerState open: the actor");
    CHECK(summary(d[15]).find("sub:XPTracker") != std::string::npos && !s[15].opens_channel, "the XPTracker: the PlayerController's last chain, a component block on the open channel");

    // key values
    const schema::ClassSchema& pc_cls = *class_of_role(Role::PlayerController);
    const schema::ClassSchema& pawn_cls = *class_of_role(Role::Pawn);
    const schema::ClassSchema& ps_cls = *class_of_role(Role::PlayerState);
    const schema::ClassSchema& gs_cls = *class_of_role(Role::GameState);
    {
        const Block& a = d[0].blocks[0];
        const Value* spawn = value_of(a, pc_cls, "SpawnLocation");
        const Vec3d* v = spawn ? spawn->get_if<Vec3d>() : nullptr;
        const Value* ps = value_of(a, pc_cls, "PlayerState");
        const auto* psg = ps ? ps->get_if<FIntrepidNetworkGUID>() : nullptr;
        CHECK(d[0].actor == guid(10341530, 60, 1860730596u) && v && v->x == -777664.5 && v->y == 616578.6875 && psg && psg->ObjectId == 10341528,
              "PlayerController open: the actor GUID, SpawnLocation (-777664.5, 616578.6875, 15952.05) and the PlayerState's GUID");
        CHECK(d[0].location && d[0].location->x == -7776645 && d[0].location->y == 6165787 && d[0].location->z == 159520,
              "SerializeNewActor carries the spawn point (x10 quantized)");
    }
    {
        const Block& info = d[6].blocks[1];
        const schema::ClassSchema& info_cls = *gen::find_component_schema("BaseCharacterInfo");
        const Value* name = value_of(info, info_cls, "CharacterName");
        const Value* a = value_of(info, info_cls, "CharacterGuid.A");
        const Value* d4 = value_of(info, info_cls, "CharacterGuid.D");
        CHECK(name && *name == Value::str("RandomChar") && a && *a == Value::i64(static_cast<int32_t>(3635609113u)) && d4 &&
                  *d4 == Value::i64(static_cast<int32_t>(1814768596u)),
              "pawn open: CharacterName and the four CharacterGuid words");
        CHECK(!value_of(info, info_cls, "Race") && !value_of(info, info_cls, "Gender") && !value_of(info, info_cls, "Level"),
              "a level-1 Kaelar male sends no Race, Gender or Level (the class defaults)");
        const schema::ClassSchema& combat_cls = *gen::find_component_schema("CombatInfo");
        const Value* cls = value_of(d[6].blocks[2], combat_cls, "PrimaryClassId.Guid");
        CHECK(cls && *cls == Value::i64(108640158570105ll), "pawn open: CombatInfo.PrimaryClassId is the Bard's record Guid");
        const schema::ClassSchema& app_cls = *gen::find_component_schema("Character Appearance");
        const Value* custom = value_of(d[6].blocks[8], app_cls, "CharacterCustomization");
        const auto* cc = custom ? custom->get_if<CharacterCustomization>() : nullptr;
        CHECK(cc && cc->random_seed && *cc->random_seed == 1454 && cc->sections_values && cc->sections_values->size() == 30 &&
                  cc->face_morph_weights && cc->face_morph_weights->size() == 83,
              "pawn open: the customization (RandomSeed 1454, 30 SectionsValues, 83 FaceMorphWeightMaps)");
        const Block& actor = d[6].blocks[0];
        const Value* owner = value_of(actor, pawn_cls, "Owner");
        const Value* remote = value_of(actor, pawn_cls, "RemoteRole");
        const Value* move = value_of(actor, pawn_cls, "ReplicatedMovement");
        const auto* o = owner ? owner->get_if<FIntrepidNetworkGUID>() : nullptr;
        const auto* m = move ? move->get_if<RepMovement>() : nullptr;
        CHECK(o && o->ObjectId == 10341530 && remote && *remote == Value::u64(1) && m && m->location.x == -77766450 && m->rotation.yaw == 65,
              "pawn open: Owner is the PlayerController, RemoteRole 1 (simulated), ReplicatedMovement at the spawn point, yaw byte 65");
        CHECK(d[6].rotation && d[6].rotation->yaw == 16550, "SerializeNewActor's rotation is the yaw as a compressed short, 16550");
    }
    {
        const Block& stats = d[13].blocks[5];
        size_t items = 0, fields = 0;
        std::map<uint32_t, size_t> by_field;
        for (const FastArrayField& f : stats.fast) {
            items += f.delta.changed.size();
            by_field[f.field] = f.delta.changed.size();
            ++fields;
        }
        CHECK(fields == 4 && items == 443 && by_field[3] == 335 && by_field[5] == 96 && by_field[6] == 11 && by_field[10] == 1,
              "pawn second state: StatsComponent has 335 Owner, 96 OwnerProxy and 11 EveryoneProxy stats and the float array's item (442 stats)");
        const schema::ClassSchema& ab_cls = *gen::find_component_schema("AbilityComponent");
        const Value* known = value_of(d[13].blocks[4], ab_cls, "KnownAbilities");
        const Value* records = value_of(d[13].blocks[4], ab_cls, "ReplicatedRecordsToLoad");
        const auto* ka = known ? known->get_if<ArrayValue>() : nullptr;
        const auto* ra = records ? records->get_if<ArrayValue>() : nullptr;
        CHECK(ka && ra && ka->elements.size() == 57 && ra->elements.size() == 58, "pawn second state: 57 KnownAbilities and 58 ReplicatedRecordsToLoad");
        const Value* hidden = value_of(d[13].blocks[0], pawn_cls, "bHidden");
        const Value* remote = value_of(d[13].blocks[0], pawn_cls, "RemoteRole");
        CHECK(hidden && *hidden == Value::boolean(true) && remote && *remote == Value::u64(2),
              "pawn second state: bHidden 1, RemoteRole 2 (autonomous)");
        const schema::ClassSchema& info_cls = *gen::find_component_schema("BaseCharacterInfo");
        const Value* server = value_of(d[13].blocks[2], info_cls, "ServerId");
        const Value* phased = value_of(d[13].blocks[2], info_cls, "bIsPhasedOut");
        const Value* possessed = value_of(d[13].blocks[2], info_cls, "bHasBeenPossessed");
        CHECK(server && *server == Value::i64(60) && phased && *phased == Value::boolean(true) && possessed && *possessed == Value::boolean(true),
              "pawn second state: ServerId 60, bIsPhasedOut 1, bHasBeenPossessed 1");
    }
    {
        const Value* name = value_of(d[12].blocks[0], pc_cls, "Name");
        const Value* tracker = value_of(d[12].blocks[0], pc_cls, "CharacterLoadTracker");
        const auto* t = tracker ? tracker->get_if<LoadTracker>() : nullptr;
        CHECK(name && *name == Value::str("RandomChar") && t && t->flags.size() == 25 && !t->flags[0] && t->flags[1] && t->flags[24],
              "after possession: Name and CharacterLoadTracker 0111...1");
        const Value* tr0 = value_of(d[5].blocks[0], pc_cls, "CharacterLoadTracker");
        const auto* t0 = tr0 ? tr0->get_if<LoadTracker>() : nullptr;
        CHECK(t0 && t0->flags.size() == 25 && t0->flags[0] && !t0->flags[1] && t0->flags[2],
              "before possession: CharacterLoadTracker 1011...1");
        const Value* pid = value_of(d[14].blocks[0], ps_cls, "PlayerId");
        const Value* uid = value_of(d[14].blocks[0], ps_cls, "UniqueId");
        const auto* u = uid ? uid->get_if<UniqueNetId>() : nullptr;
        CHECK(pid && *pid == Value::i64(1874) && u && u->flags == 8 && u->id == "DESKTOP-EXAMPLE1-US-45739BC044607BD9A50196AF41F22492",
              "PlayerState: PlayerId and the login's unique id");
    }
    {   // the GameState by mode
        const Block& g = d[4].blocks[0];
        if (mode == bs::GameStateMode::Captured) {
            CHECK(d[4].actor == guid(4608, 60, 1860730596u) && g.rep && g.raw_fields.size() == 5 && g.read_to_end,
                  "Captured: the original's actor block (GUID 4608), its RepLayout part and the five fast-array fields up to the last bit");
            CHECK(g.raw_fields.size() == 5 && g.raw_fields[0].first == 25 && g.raw_fields[1].first == 26 && g.raw_fields[2].first == 28 &&
                      g.raw_fields[3].first == 29 && g.raw_fields[4].first == 32,
                  "Captured: EstateIDToOwnerGuid, NodeIDToClientNodeData, the two node maps and ReplicatedGlobalQuestCache");
        } else {
            const Value* has_begun = value_of(g, gs_cls, "bReplicatedHasBegunPlay");
            const Value* mode_class = value_of(g, gs_cls, "GameModeClass");
            const auto* mc = mode_class ? mode_class->get_if<FIntrepidNetworkGUID>() : nullptr;
            const Value* time = value_of(g, gs_cls, "ReplicatedWorldTimeSecondsDouble");
            const size_t handles = g.rep ? g.rep->values().size() : 0;
            CHECK(d[4].actor == guid(4608, 60, 1860730596u) && has_begun && *has_begun == Value::boolean(true) && mc && mc->ObjectId == 17588721769397045729ull &&
                      time && *time == Value::f64(1593462.375),
                  "GameState: bReplicatedHasBegunPlay 1, GameModeClass AoCGameModeBaseBP_C, the world time");
            if (mode == bs::GameStateMode::Tier1) {
                CHECK(handles == 6 && !value_of(g, gs_cls, "GameTimeScalar") && !value_of(g, gs_cls, "ServiceConnectionFlags") && g.fast.empty(),
                      "Tier 1: six handles (AuthServerIDReplicated, RemoteRole, Role, GameModeClass, bReplicatedHasBegunPlay, the world time), no combat settings, no world arrays");
            } else {
                const Value* scalar = value_of(g, gs_cls, "GameTimeScalar");
                const Value* flags = value_of(g, gs_cls, "ServiceConnectionFlags");
                const Value* combat = value_of(g, gs_cls, "CombatSettings.General.InCombatTimer");
                CHECK(handles == 33 && scalar && *scalar == Value::f32(6) && flags && *flags == Value::u64(18357) && combat && *combat == Value::f32(6) && g.fast.empty(),
                      "Tier 2: 33 handles: GameTimeScalar 6, ServiceConnectionFlags 18357 and the 25 combat settings, no world arrays");
            }
        }
    }
    check_limits(s, name);
}
// [C_Y_B :: C O A]

static void test_native_character() {
    std::printf("a character that is not the capture's\n");
    bs::PlayerSetup s = captured_setup();
    s.name = "cyb";
    s.race = static_cast<uint8_t>(bs::ClientRace::Dunir);
    s.gender = static_cast<uint8_t>(bs::ClientGender::Female);
    s.level = 7;
    s.x = 1000.25;
    s.y = -2000.5;
    s.z = 300;
    s.yaw_degrees = -90;
    s.held_items = {};          // unequipped: no HeldItems, no AppearanceIDs
    s.appearance_ids.clear();
    s.weapon_skill_line = {};
    s.character_guid = {1, 0x80000000u, 3, 0xFFFFFFFFu};
    s.server_id = 7;
    s.world_time = 1000.5;
    s.player_id = 3;
    s.login.name_option = "dev-machine";
    s.login.unique_id = "dev-machine";
    PlayerBootstrap pb(s, bs::GameStateMode::Tier2);
    CHECK(pb.ok(), "a native setup builds");
    const auto seq = sequence_of(pb);
    CHECK(seq.has_value() && seq->size() == 16, "the whole sequence builds");
    if (!seq) return;
    std::vector<Decoded> d;
    for (const Send& x : *seq) d.push_back(decode(pb, x));
    bool all = true;
    for (const Decoded& x : d) all = all && all_read(x);
    CHECK(all, "every block reads to its last bit");
    const schema::ClassSchema& info_cls = *gen::find_component_schema("BaseCharacterInfo");
    const Block& info = d[6].blocks[1];
    const Value* race = value_of(info, info_cls, "Race");
    const Value* gender = value_of(info, info_cls, "Gender");
    const Value* level = value_of(info, info_cls, "Level");
    const Value* b = value_of(info, info_cls, "CharacterGuid.B");
    CHECK(race && *race == Value::u64(6) && gender && *gender == Value::u64(1) && level && *level == Value::i64(7) && b && *b == Value::i64(static_cast<int32_t>(0x80000000u)),
          "BaseCharacterInfo: Race (Dunir 6), Gender 1, Level 7 and the Guid words as int32");
    CHECK(summary(d[13]) == "actor(rep) sub:AlignmentComponent(rep) sub:BaseCharacterInfo(rep) sub:CombatInfo(rep) sub:AbilityComponent(rep) "
                            "sub:StatsComponent(fast) sub:OwnerInfo(rep) sub:ResourceStorageComponent(rep)",
          "unequipped: no HeldItems and no AppearanceIDs in the second state");
    CHECK(d[0].location && d[0].location->x == 10003 && d[0].location->y == -20005 && d[0].location->z == 3000,
          "the spawn point is quantized x10, halves rounded away from zero: 1000.25 -> 10003, -2000.5 -> -20005");
    const schema::ClassSchema& pawn_cls = *class_of_role(Role::Pawn);
    const Value* move = value_of(d[6].blocks[0], pawn_cls, "ReplicatedMovement");
    const auto* m = move ? move->get_if<RepMovement>() : nullptr;
    CHECK(m && m->rotation.yaw == 192 && d[6].rotation && d[6].rotation->yaw == 49152,
          "yaw -90 degrees: the byte 192 and the compressed short 49152");
    const Value* ps_name = value_of(d[14].blocks[0], *class_of_role(Role::PlayerState), "PlayerNamePrivate");
    CHECK(ps_name && *ps_name == Value::str("dev-machine"), "the PlayerState carries the login name, not the character's");
    const Value* name = value_of(d[12].blocks[0], *class_of_role(Role::PlayerController), "Name");
    CHECK(name && *name == Value::str("cyb"), "the PlayerController's Name is the character's");

    // a value the replicator will not carry is reported when the bootstrap is built, not in the middle of a join
    bs::PlayerSetup bad = captured_setup();
    bad.name = std::string("a\0b", 3);
    PlayerBootstrap broken(bad, bs::GameStateMode::Tier1);
    CHECK(!broken.ok() && broken.error().find("CharacterName") != std::string::npos && !broken.join_sequence(),
          "a character name with a NUL is refused at construction, naming the property");
    {
        bs::PlayerSetup nan = captured_setup();
        nan.x = std::nan("");
        bs::PlayerSetup late = captured_setup();
        late.world_time = 3e9;
        bs::PlayerSetup race = captured_setup();
        race.race = 9;   // ECharacterRace::cCount
        bs::PlayerSetup gender = captured_setup();
        gender.gender = 2;
        CHECK(!PlayerBootstrap(nan, bs::GameStateMode::Tier1).ok() && !PlayerBootstrap(late, bs::GameStateMode::Tier1).ok() &&
                  !PlayerBootstrap(race, bs::GameStateMode::Tier1).ok() && !PlayerBootstrap(gender, bs::GameStateMode::Tier1).ok(),
              "a spawn point that is not finite, a world time past an int32 and a race or gender the client does not have are refused");
    }

    // checkpoint and restore: a refused chain is built again as it was
    PlayerBootstrap again(captured_setup(), bs::GameStateMode::Tier2);
    again.pawn_open();
    const auto cp = again.checkpoint();
    const auto first = again.pawn_second_state();
    again.restore(cp);
    // the components are added once; the shadow is back, so the same update comes out again
    const auto second = again.pawn_second_state();
    CHECK(first && second && first->emission.payload.bit_pos() == second->emission.payload.bit_pos() &&
              capture_blocks::same_bit_range(first->emission.payload.data(), second->emission.payload.data(), first->emission.payload.bit_pos(), nullptr),
          "restore(checkpoint) puts the shadows back: the refused update is built again, the same bits");
}

static void test_guids_and_parts() {
    std::printf("minted GUIDs and the pieces\n");
    ServerGuidAllocator alloc(60, 1860730596u);
    const auto g = bs::mint_session_guids(alloc, guid(4608, 60, 1860730596u));
    CHECK(g.player_state.ObjectId == ServerGuidAllocator::kFirstObjectId && g.player_controller.ObjectId == g.player_state.ObjectId + 2 &&
              g.stats.ObjectId == g.player_state.ObjectId + 2 * 16 && g.game_state.ObjectId == 4608 && g.pawn.ServerId == 60 && g.pawn.Randomizer == 1860730596u,
          "mint_session_guids: consecutive even ids under the ServerId, the GameState's passed through");
    bool even = true;
    for (const auto* x : {&g.player_state, &g.player_controller, &g.global_gm_commands, &g.pawn, &g.base_character_info, &g.combat_info, &g.owner_info,
                          &g.backpack, &g.equipment, &g.quest_storage, &g.reward_storage, &g.appearance, &g.resource_storage[0], &g.resource_storage[1], &g.resource_storage[2], &g.collision_cylinder,
                          &g.alignment, &g.ability, &g.stats}) {
        even = even && (x->ObjectId % 2) == 0;
    }
    CHECK(even, "every minted ObjectId is even");

    const bs::StatsSetup stats = bs::captured_bard_level1_stats();
    CHECK(stats.owner.items.size() == 335 && stats.owner_proxy.items.size() == 96 && stats.everyone_proxy.items.size() == 11 &&
              stats.float_everyone_proxy.size() == 1 && stats.owner.array_key == 417,
          "the captured Bard's stats: 335 + 96 + 11 and one float item, Owner key 417");
    size_t extended = 0;
    for (const auto& it : stats.owner.items) extended += it.extended ? 1 : 0;
    CHECK(extended == 4, "four Owner stats carry Base and EquipmentValue");
    CHECK(bs::captured_bard_known_abilities().size() == 57 && bs::captured_bard_records_to_load().size() == 58, "the captured Bard's abilities: 57 known, 58 to load");
    CHECK(std::size(captured::kLevelStatuses) == 429 && captured::kLevelStatuses[8] == 0x8c && captured::kLevelStatuses[9] == 0x0a,
          "the level list: 429 bytes, the 8-byte prefix then 8C 0A");
    CHECK(bs::load_tracker(bs::kLoadTrackerSecond).flags.size() == 25 && bs::load_tracker(bs::kLoadTrackerSecond).flags[2] && !bs::load_tracker(bs::kLoadTrackerSecond).flags[9],
          "the four load tracker values are 25 flags each");
    CHECK(bs::game_state_tier2_values() && bs::game_state_tier2_values()->values().size() == 27, "the Tier 2 values read: 27 handles");
}

// ---- a lingering zone's actor (plan D2, review) -------------------------------------------------------------------------------------------

static FIntrepidNetworkGUID guid_of(const capture_lingering::Guid& g) { return guid(g.object_id, g.server_id, g.randomizer); }

/// The values of the retail actor's block, read with the generated AAoCAbilityLingeringEffect schema: the spec the server would have built it from.
static bool spec_from_capture(const capture_lingering::Open& open, bs::LingeringSpec& spec) {
    const auto bytes = capture_blocks::bytes_of(open.content_hex);
    const auto views = read_content_blocks(bytes.data(), open.content_bits, open.blocks_at);
    const schema::ClassSchema* cls = gen::find_class_schema("AAoCAbilityLingeringEffect");
    if (!views || views->size() != 4 || !cls || !(*views)[0].header.is_actor || !(*views)[0].header.has_rep_layout) return false;
    size_t pos = (*views)[0].payload_pos;
    const auto st = read_rep_block(*cls, bytes.data(), (*views)[0].payload_pos + (*views)[0].payload_bits, pos);
    if (!st || pos != (*views)[0].payload_pos + (*views)[0].payload_bits) return false;
    const auto word = [&](uint16_t h) -> uint32_t {
        const auto it = st->values().find(h);
        const int64_t* v = it == st->values().end() ? nullptr : it->second.get_if<int64_t>();
        return v ? static_cast<uint32_t>(*v) : 0u;
    };
    const auto i64 = [&](uint16_t h) -> int64_t {
        const auto it = st->values().find(h);
        const int64_t* v = it == st->values().end() ? nullptr : it->second.get_if<int64_t>();
        return v ? *v : 0;
    };
    const auto object = [&](uint16_t h) -> FIntrepidNetworkGUID {
        const auto it = st->values().find(h);
        const FIntrepidNetworkGUID* v = it == st->values().end() ? nullptr : it->second.get_if<FIntrepidNetworkGUID>();
        return v ? *v : FIntrepidNetworkGUID{};
    };
    spec.actor = guid_of(open.actor);
    spec.movement = guid_of(open.subobjects[0]);
    spec.ability = guid_of(open.subobjects[1]);
    spec.stats = guid_of(open.subobjects[2]);
    spec.pawn_mesh = object(14);                                  // AttachmentReplication.AttachComponent
    spec.zone = i64(24);                                          // LingeringEffectId
    spec.source = i64(25);                                        // SourceRecordGuid
    for (uint16_t i = 0; i < 4; ++i) {
        spec.effect_guid[i] = word(static_cast<uint16_t>(19 + i));      // LingeringEffectGuid.A..D
        spec.instance_guid[i] = word(static_cast<uint16_t>(28 + i));    // LingeringZoneInstGuid.A..D
    }
    // SerializeNewActor's numbers: the location as the wire holds it (cm x 10), the yaw, the velocity
    spec.x = static_cast<double>(open.location.x) / 10.0;
    spec.y = static_cast<double>(open.location.y) / 10.0;
    spec.z = static_cast<double>(open.location.z) / 10.0;
    spec.yaw_degrees = open.rotation[1] * 360.0 / 65536.0;
    if (open.velocity.present) spec.velocity = repl::QuantVec{repl::QuantVec::Mode::Scaled, open.velocity.x, open.velocity.y, open.velocity.z};
    return true;
}
// C_Y_B and C-O-A

static void test_lingering_actor() {
    std::printf("a lingering zone's actor (retail capture 44: the opens of the Shielding and Mesmerizing dances, bit for bit)\n");
    const capture_lingering::Open& first = capture_lingering::kOpens[0];
    bs::PlayerSetup setup = captured_setup();
    setup.guids.pawn = guid(701178, first.actor.server_id, first.actor.randomizer);   // the retail pawn the actor attaches to and names as its Instigator
    PlayerBootstrap pb(std::move(setup), bs::GameStateMode::Tier2);
    CHECK(pb.ok() && pb.join_sequence().has_value(), "a bootstrap whose pawn is the capture's, joined");
    pb.export_acks().mark(guid(16442478405498561049ull));   // PersistentLevel and the plugin package: acknowledged by the opens before
    pb.export_acks().mark(guid(158953490572197689ull));
    CHECK(pb.lingering_count() == 0 && !pb.lingering_open(1).has_value(), "no actor, nothing to open");
    for (const capture_lingering::Open& open : capture_lingering::kOpens) {
        bs::LingeringSpec spec;
        CHECK(spec_from_capture(open, spec), "the capture's actor block reads with the generated schema (a rep layout block and three subobject blocks)");
        CHECK(spec.zone != 0 && spec.source != 0 && spec.pawn_mesh.ObjectId != 0, "...and names the zone, the ability and the pawn's mesh");
        CHECK(pb.lingering_add(spec) && !pb.lingering_add(spec), "the actor is added once");
        const std::optional<Send> send = pb.lingering_open(open.actor.object_id);
        CHECK(send && send->opens_channel && send->reliable() && send->effect_actor == open.actor.object_id, "its open is a reliable chain that opens a channel of its own");
        Ref export_ref{"the export bunch", open.export_bits, open.export_hex};
        Ref content_ref{"the content", open.content_bits, open.content_hex};
        const bool exports_ok = send && send->emission.exports.size() == 1 && same_as(send->emission.exports[0], export_ref);
        CHECK(exports_ok, "the export bunch equals the original's (packet 302: the archetype, the pawn's CharacterMesh0 and the three components; packet 856: the components)");
        CHECK(send && same_as(send->emission.payload, content_ref),
              "SerializeNewActor (location, rotation, velocity) and the four content blocks equal the original's: 2,044 bits, bit for bit");
        // a chain the connection did not take is built again from the checkpoint
        {
            const PlayerBootstrap::Checkpoint checkpoint = pb.checkpoint();
            const std::optional<Send> again = pb.lingering_open(open.actor.object_id);
            pb.restore(checkpoint);
            const std::optional<Send> third = pb.lingering_open(open.actor.object_id);
            CHECK(again && third && same_as(third->emission.payload, content_ref), "an open built again (a chain the connection refused) is the same");
        }
        if (send) pb.mark_exported(send->emission.exported);   // acknowledged: the next open names less
        // the fade-out: bFadingOut as one 28-bit block, as the retail actor's first update
        if (open.packet == 302) {
            const std::optional<Send> fade = pb.lingering_fade(open.actor.object_id);
            CHECK(fade && !fade->opens_channel && fade->effect_actor == open.actor.object_id && fade->emission.exports.empty() && fade->emission.payload.bit_pos() == 28,
                  "the fade-out is one 28-bit update on the actor's channel, as retail's (bFadingOut)");
            CHECK(!pb.lingering_fade(open.actor.object_id).has_value(), "(nothing changed, nothing to send twice)");
        }
    }
    CHECK(pb.lingering_count() == 2, "two actors held");
    pb.lingering_remove(capture_lingering::kOpens[0].actor.object_id);
    pb.lingering_remove(capture_lingering::kOpens[1].actor.object_id);
    CHECK(pb.lingering_count() == 0 && !pb.lingering_open(capture_lingering::kOpens[0].actor.object_id).has_value(), "a closed actor is forgotten");
}

int main() {
    test_lingering_actor();
    test_player_controller();
    test_game_state();
    test_load_tracker_and_pawn_open();
    test_possession();
    test_player_state();
    test_replies();
    test_sequence(bs::GameStateMode::Tier1, "Tier 1");
    test_sequence(bs::GameStateMode::Tier2, "Tier 2");
    test_sequence(bs::GameStateMode::Captured, "Captured");
    test_native_character();
    test_guids_and_parts();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
