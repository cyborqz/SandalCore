// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/pc_emitter.cpp  —  M1.2
//
//  Emits the PlayerController ActorOpen + initial property values natively
//  using the existing ActorBuilder pipeline.
// ============================================================================
#include "net/pc_emitter.h"
#include "net/captured_identity.h"
#include "net/native_connect_sequencer.h"  // IGameServerHost
#include "net/captured_pc_tail_substituted.h"  // PM128 surgical-substituted tail

#include "protocol/emit/actor_builder.h"
#include "protocol/emit/actor_bunch.h"
#include "protocol/emit/bunch_writer.h"
#include "protocol/emit/intrepid_netguid.h"
#include "protocol/emit/net_serializers.h"
#include "protocol/emit/package_map_exporter.h"
#include "protocol/emit/rep_layout_writer.h"
#include "protocol/emit/schema_value.h"
#include "protocol/emit/property_update_bunch_builder.h"
#include "protocol/wire/packet_parser.h"
#include "protocol/wire/possession.h"
#include "protocol/schema/actor_rep_handles.h"
#include "protocol/schema/schema_registry.h"

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#endif

#include <spdlog/spdlog.h>
#include <memory>
#include <vector>
#include <atomic>
#include <cstdio>
#include <thread>
#include <chrono>

namespace aoc { namespace net {

using namespace aoc::protocol;

// ── M1.4.a — spliced RepLayout property tail ──────────────────────────────
//
// Our ActorBuilder emits the export section + SerializeNewActor body
// byte-identical to captured pkt#22 (proven by test_pc_spawn_diff), but
// the RepLayout handle-stream tail (initial property values) still needs
// per-property calibration we don't yet have.  Until Session I closes
// that gap, we splice 848 captured bits of the tail verbatim so the
// client sees a fully-formed PC with valid initial property state
// (Pawn ref, PlayerState ref, HP/MP/Stamina, etc.).
//
// Extracted from src/protocol/tools/captured_pc_spawn_reassembled.bin
// bits 4011..4858 via the same derivation as test_pc_spawn_diff.cpp
// lines 229-247.  848 bits = 106 bytes.
//
// This is a BRIDGING MEASURE — the spliced NetGUIDs inside this tail
// reference the captured session's Pawn/PlayerState, which means the
// client will show "Hatemost"'s state with our MyHero name overlay
// until M5 replaces this with server-authoritative property emission.
static constexpr uint8_t kCapturedPcTailBits[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0xfc, 0x80, 0x40,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x42, 0x20, 0x34,
    0xc1, 0x64, 0xee, 0x04, 0x00, 0x00, 0x00, 0x00,
    0xe0, 0x01, 0x00, 0x00, 0x20, 0xf7, 0x43, 0x77,
    0x63, 0x01, 0x00, 0x00, 0x00, 0x08, 0xdc, 0x3d,
    0x09, 0x06, 0x00, 0x00, 0x00, 0x2b, 0x88, 0x16,
    0x09, 0x02, 0x00, 0x00, 0x00, 0x33, 0x40, 0x79,
    0x06, 0x82, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x40, 0x9b, 0x1d, 0x14, 0x4e, 0x02,
    0x80, 0x93, 0xb9, 0x13, 0x00, 0x00, 0x00, 0x00,
    0x80, 0x07, 0x00, 0x00, 0x80, 0xdc, 0x0f, 0xdd,
    0x8d, 0x38, 0x50, 0x8b, 0x6e, 0xf5, 0xc5, 0x5e,
    0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x80, 0x00,
};
static constexpr size_t kCapturedPcTailBitCount = 848;
static_assert(sizeof(kCapturedPcTailBits) == (kCapturedPcTailBitCount + 7) / 8,
              "Captured PC tail byte count must match 848-bit declaration");
// ~ CoA - C-Y-B ~

PcEmitter::PcEmitter(IGameServerHost& host, const std::string& client_key)
    : host_(host), client_key_(client_key)
{}

// Helper: build a 2-level export chain (leaf + outer).  Mirrors
// test_pc_spawn_diff's build_two_level exactly.
static emit::ExportEntry build_two_level(uint64_t leaf_obj,
                                          const std::string& leaf_path,
                                          uint32_t leaf_checksum,
                                          uint64_t outer_obj,
                                          const std::string& outer_path,
                                          uint32_t outer_checksum,
                                          bool no_load = false) {
    emit::ExportEntry leaf = emit::ExportEntry::asset(
        leaf_obj, leaf_path, leaf_checksum, no_load);
    emit::ExportEntry outer = emit::ExportEntry::asset(
        outer_obj, outer_path, outer_checksum, no_load);
    leaf.outer = std::make_unique<emit::ExportEntry>(std::move(outer));
    return leaf;
}

static emit::ExportEntry build_three_level(uint64_t leaf_obj,
                                            const std::string& leaf_path,
                                            uint32_t leaf_checksum,
                                            uint64_t mid_obj,
                                            const std::string& mid_path,
                                            uint32_t mid_checksum,
                                            uint64_t outermost_obj,
                                            const std::string& outermost_path,
                                            uint32_t outermost_checksum,
                                            bool no_load = true) {
    emit::ExportEntry outermost = emit::ExportEntry::asset(
        outermost_obj, outermost_path, outermost_checksum, no_load);
    emit::ExportEntry mid = emit::ExportEntry::asset(
        mid_obj, mid_path, mid_checksum, no_load);
    mid.outer = std::make_unique<emit::ExportEntry>(std::move(outermost));
    emit::ExportEntry leaf = emit::ExportEntry::asset(
        leaf_obj, leaf_path, leaf_checksum, no_load);
    leaf.outer = std::make_unique<emit::ExportEntry>(std::move(mid));
    return leaf;
}

bool PcEmitter::emit_open(const sockaddr_in& client_addr) {
    spdlog::warn("[PcEmitter] emit_open — building PC ActorOpen bunch");

    // Load the PC schema (10 handles matching docs/pc-spawn-handle-catalog.md)
    schema::SchemaRegistry::instance().load_all();
    const auto* pc_schema = schema::SchemaRegistry::instance()
        .get_schema(schema::ActorType::PlayerController);
    if (!pc_schema) {
        spdlog::error("[PcEmitter] PC schema not loaded");
        return false;
    }

    // Build the same 3 export entries captured in pkt#22 (docs/native-bootstrap-sequence.md §2.3)
    std::vector<emit::ExportEntry> exports;

    // [0] AoCPlayerControllerBP
    {
        auto e = build_two_level(
            /*leaf_obj=*/       3503756484819958835ULL,
            /*leaf_path=*/      "Default__AoCPlayerControllerBP_C",
            /*leaf_checksum=*/  0x6b62891c,
            /*outer_obj=*/      4074085207143396457ULL,
            /*outer_path=*/     "/Game/ThirdPersonCPP/Blueprints/AoCPlayerControllerBP",
            /*outer_checksum=*/ 0x00000000,
            /*no_load=*/        false);
        exports.push_back(std::move(e));
    }
    // [1] PersistentLevel (3-level chain with bNoLoad)
    {
        auto e = build_three_level(
            /*leaf_obj=*/        16442478405498561049ULL,
            /*leaf_path=*/       "PersistentLevel",
            /*leaf_checksum=*/   0x00000000,
            /*mid_obj=*/         16604466839667550161ULL,
            /*mid_path=*/        "Verra_World_Master",
            /*mid_checksum=*/    0x00000000,
            /*outermost_obj=*/   12923414834320654503ULL,
            /*outermost_path=*/  "/Game/Levels/Verra_World_Master/Verra_World_Master",
            /*outermost_checksum=*/ 0x00000000,
            /*no_load=*/         true);
        exports.push_back(std::move(e));
    }
    // [2] GlobalGMCommands
    {
        auto e = build_two_level(
            /*leaf_obj=*/       485698175673737329ULL,
            /*leaf_path=*/      "GlobalGMCommands",
            /*leaf_checksum=*/  0xcaaaee3e,
            /*outer_obj=*/      158953490572197689ULL,
            /*outer_path=*/     "/Script/GameSystemsPlugin",
            /*outer_checksum=*/ 0x00000000,
            /*no_load=*/        false);
        exports.push_back(std::move(e));
    }
    // Mark all as having checksums (captured pkt#22 sets the flag even where
    // checksum value is 0 — test_pc_spawn_diff validated this exact pattern).
    exports[0].has_checksum = true;
    exports[0].outer->has_checksum = true;
    exports[1].has_checksum = true;
    exports[1].outer->has_checksum = true;
    exports[1].outer->outer->has_checksum = true;
    exports[2].has_checksum = true;
    exports[2].outer->has_checksum = true;

    // ── Runtime — server-minted PC NetGUID + static archetype/level GUIDs ──
    //
    // Phase A.3 (2026-04-26): the PC actor NetGUID is now MINTED by
    // GameServer's per-client NetGuidAllocator (block base ≥ 0x01000000)
    // instead of using the captured value 10341530.  This proves the
    // pipeline can carry server-authoritative GUIDs end-to-end.  The
    // ServerId stays at 60 (matches captured convention) and the
    // Randomizer is derived deterministically from the ObjectId via the
    // same hash NetGUIDAllocator uses (so repeated lookups for the same
    // PC return the same Randomizer — required so the client's NetGUID
    // cache doesn't drift mid-session).
    //
    // Archetype + Level NetGUIDs stay hardcoded — those are
    // content-addressable (Default__AoCPlayerControllerBP_C and
    // PersistentLevel/Verra_World_Master) and the SAME on every
    // client/server because they're derived from class+package hashes.
    auto block = host_.allocate_player_block(client_key_);
    auto rnd_for = [](uint64_t obj) -> uint32_t {
        // Same hash as bootstrap::NetGUIDAllocator::alloc_rnd_for —
        // deterministic per ObjectId so repeated encodes match.
        uint64_t h = obj * 0x9E3779B97F4A7C15ULL;
        h ^= (h >> 33);
        h *= 0xFF51AFD7ED558CCDULL;
        h ^= (h >> 33);
        return static_cast<uint32_t>(h);
    };
    const bool captured_identity = use_captured_player_identity();
    const uint64_t pc_obj = captured_identity ? kCapturedPcObjectId : block.player_controller;
// ~ C:Y:B :: C.O.A ~

    emit::ActorRuntime rt;
    rt.type                 = schema::ActorType::PlayerController;
    rt.actor_netguid        = pc_obj;             // MINTED (or captured: net/captured_identity.h)
    rt.actor_server_id      = 60;
    rt.actor_randomizer     = captured_identity ? captured_guid(pc_obj).Randomizer
                                                : rnd_for(pc_obj);
    rt.archetype_netguid    = 3503756484819958835ULL;
    rt.archetype_server_id  = 0;
    rt.archetype_randomizer = 0;
    rt.level_netguid        = 16442478405498561049ULL;
    rt.level_server_id      = 0;
    rt.level_randomizer     = 0;

    spdlog::warn("[PcEmitter] PC NetGUID minted: ObjectId={} ServerId={} "
                  "Randomizer={} (block base=0x{:x}, was captured=10341530)",
                  pc_obj, rt.actor_server_id, rt.actor_randomizer, block.base);

    // Transform — the captured PC's spawn location, world cm x10
    // (-777664.5, 616578.7, 15952.0).  The old values here (-5940754, ...)
    // were fitted so the UE4 packed-vector writer reproduced the captured
    // bits; the client reads UE5 quantized vectors, which ActorBuilder now
    // writes (test_serialize_new_actor).
    rt.serialize_location   = true;
    rt.quantize_location    = true;
    rt.location_scaled_x    = -7776645;
    rt.location_scaled_y    = 6165787;
    rt.location_scaled_z    = 159520;
    rt.serialize_rotation   = false;
    rt.serialize_scale      = false;
    rt.serialize_velocity   = false;

    // Build via ActorBuilder (byte-identical to captured pkt#22 per
    // test_pc_spawn_diff: 4859/4864 bits).
    emit::BunchWriter bw;
    emit::EmitContext ctx;
    ctx.channel              = 3;            // captured PC channel
    ctx.ch_sequence          = 954;          // captured ChSeq
    ctx.is_reliable          = true;
    ctx.is_partial           = false;        // our version is single-fragment
    ctx.partial_initial      = true;
    ctx.partial_final        = true;
    ctx.is_control           = true;         // channel-open control bunch
    ctx.b_open               = true;
    ctx.package_map_exports  = std::move(exports);
    ctx.ch_name_is_hardcoded = true;
    ctx.ch_name_ename_idx    = 102;          // NAME_Actor

    // ── PM48 (2026-04-30) — REVERTED PM47 tail re-enable ─────────────────
    //
    // PM47 hypothesis (re-enabling captured tail would fix CNSF) was WRONG.
    // Live test 11:36 showed PC bunch grew to 4909 bits with tail but client
    // STILL CNSF'd ("String is too large (Size: 55308082)") — meaning the
    // misalignment is in the PRE-TAIL portion (bunch header, exports, or
    // SerializeNewActor), not the missing tail.
    //
    // We've been silently failing PC ActorOpen since PM35 and didn't notice
    // because LoadMap streams the world independently of any successful
    // actor open.  "Empty world with rocks" = streamed level + zero actors.
    //
    // PM127/PM128 (2026-05-06) — captured PC tail splice with optional
    // surgical NetGUID substitution.
    //
    // Ground-truth verification by compare_pc_actoropen.py: our generated PC
    // content is 99% byte-identical to captured pkt#22 (494/502 matching
    // bytes), missing only the 848-bit trailing initial-property-state.
    // Without it the client opens our PC actor with EMPTY properties.
    //
    // probe_pc_tail_splice.txt modes:
    //   "0" or absent → DISABLED (PM99 stable)
    //   "1"           → PM127 verbatim splice (captured NetGUIDs unresolved
    //                   on our connection → Bunch.IsError, no mesh)
    //   "2"           → PM128 surgical splice — captured NetGUIDs replaced
    //                   with our minted ones at bit positions 131 and 581
    //                   (substitute_pc_tail_guids.py output).  Bit 131
    //                   captured ObjId 10341528 → our PC 16777216.
    //                   Bit 581 captured ObjId 10341532 → our Pawn 16777218.
    //                   Both retain Srv=60.  This should make the client's
    //                   property updates resolve to OUR registered objects
    //                   → OnRep callbacks fire → mesh assembly should work.
    int tail_mode = 0;
    if (std::FILE* fp = std::fopen("probe_pc_tail_splice.txt", "r")) {
        std::fscanf(fp, "%d", &tail_mode);
        std::fclose(fp);
    }
    // The stored tails were cut at the end of the old (2 bits short) spawn
    // record, so they begin with 2 location bits and the PlayerController's
    // NetPlayerIndex + ClientHandshakeId, which ActorBuilder now writes itself.
    // Skip those 42 bits; the GUID substitutions sit after them, unmoved.
    constexpr size_t kTailSkip = 2 + 8 + 32;
    if (tail_mode == 1) {
        ctx.spliced_tail_bits      = kCapturedPcTailBits;
        ctx.spliced_tail_start_bit = kTailSkip;
        ctx.spliced_tail_bit_count = kCapturedPcTailBitCount - kTailSkip;
        spdlog::warn("[PcEmitter] PM127 verbatim PC tail ENABLED — splicing "
                     "{} bits captured initial property state "
                     "(NetGUIDs unresolved on our connection)",
                     kCapturedPcTailBitCount);
    } else if (tail_mode == 2) {
        ctx.spliced_tail_bits      = kCapturedPcTailSubst;
        ctx.spliced_tail_start_bit = kTailSkip;
        ctx.spliced_tail_bit_count = kCapturedPcTailSubstBits - kTailSkip;
        spdlog::warn("[PcEmitter] PM128 SURGICAL PC tail ENABLED — splicing "
                     "{} bits with NetGUID substitution: "
                     "bit 131 captured 10341528 -> our PC 16777216, "
                     "bit 581 captured 10341532 -> our Pawn 16777218",
                     kCapturedPcTailSubstBits);
    } else {
        ctx.spliced_tail_bits      = nullptr;
        ctx.spliced_tail_bit_count = 0;
        (void)kCapturedPcTailBits;
        (void)kCapturedPcTailBitCount;
        (void)kCapturedPcTailSubst;
        (void)kCapturedPcTailSubstBits;
    }
// C | O | A : C-Y-B

    emit::ActorBuilder builder;
    size_t bits = builder.build_spawn(*pc_schema, rt, ctx, bw);
    if (bits == 0) {
        spdlog::error("[PcEmitter] ActorBuilder::build_spawn returned 0 bits");
        return false;
    }
    spdlog::info("[PcEmitter] ActorBuilder produced {} bits ({} bytes)",
                 bits, (bits + 7) / 8);

    // ── PM49 (2026-04-30) — DIAGNOSTIC: dump our outgoing bunch ─────────
    //
    // One-time write of the bunch bytes to disk so a Python tool can byte-
    // diff our output against captured pkt 22's bunch.  Drives the byte-
    // diff diagnostic that pinpoints the ActorBuilder misalignment.
    //
    // File contains exactly `(bits + 7) / 8` bytes.  Bit count is logged
    // above (so the diff tool knows how many bits to compare).  Static
    // bool guards against multiple writes per process.
    {
        static std::atomic_bool dumped{false};
        bool expected = false;
        if (dumped.compare_exchange_strong(expected, true)) {
            const size_t byte_count = (bits + 7) / 8;
            const char* paths[] = {
                "our_pc_bunch.bin",   // the working directory (dist/Release when launch_all.bat starts the stack)
                "/tmp/our_pc_bunch.bin",
            };
            for (const char* p : paths) {
                FILE* f = std::fopen(p, "wb");
                if (f) {
                    std::fwrite(bw.data(), 1, byte_count, f);
                    std::fclose(f);
                    spdlog::warn("[PcEmitter] DIAG: dumped {} bits ({} bytes) "
                                  "to {}", bits, byte_count, p);
                    break;
                }
            }
        }
    }

    bool ok = host_.send_bunch_packet(client_key_, client_addr,
                                        bw.data(), bits);
    if (!ok) {
        spdlog::error("[PcEmitter] send_bunch_packet failed");
        return false;
    }
    spdlog::warn("[PcEmitter] ★ PC ActorOpen sent");
    return true;
}

bool PcEmitter::emit_properties(const sockaddr_in& client_addr) {
    // PM57 (2026-04-30) — DISABLE Name update temporarily.
    //
    // PM56 fixed the channel-type mismatch (ENameIdx 103→102) so the
    // Name update bunch now arrives at the client.  But the Name update
    // uses V2 content-block format which AOC rejects with:
    //   Result=ContentBlockHeaderNoSubObjectClass
    //   Result=ContentBlockHeaderFail
    //   Result=ContentBlockFail
    //   → SendCloseReason → "Connection to the Realm timed out"
    //
    // Per the builder's own docstrings, V2 was a guess; V3 is the proper
    // post-RE content-block format.  Converting Name to V3 requires
    // knowing the right cmd_handle for the Name property (currently we
    // use 0x6A which was observed but may not match V3's handle space).
    //
    // For Phase C testing, skip Name update entirely — the goal right now
    // is to get PC.Pawn → AcknowledgePossession working.  Name can be
    // restored later via V3 once we confirm PC.Pawn lands cleanly.
    spdlog::warn("[PcEmitter] emit_properties: SKIPPED (PM57 — V2 format"
                 " causes ContentBlockFail; will revisit with V3 after"
                 " Phase C possession is confirmed)");
    (void)client_addr;
    return true;
// [C | O | A x C Y B]

    // ── Below: original V2 Name update path, kept for reference ─────────
    /*
    const std::string& name = host_.custom_name();
    if (name.empty()) {
        spdlog::info("[PcEmitter] emit_properties: no custom_name, skipping");
        return true;
    }

    spdlog::warn("[PcEmitter] emit_properties: sending Name='{}' via "
                 "PropertyUpdateBunchBuilder", name);
    */
}

#if 0 // PM57 — V2 Name update body parked behind #if 0; restore after V3 conversion
bool PcEmitter::emit_properties_legacy_v2(const sockaddr_in& client_addr) {
    const std::string& name = host_.custom_name();
    if (name.empty()) return true;

    aoc::protocol::emit::PropertyUpdateBunchBuilder b;
    b.set_channel(3);
    // M1.4.e — chSeq MUST be consecutive on a reliable channel.  Our PC
    // ActorOpen opened ch=3 at chSeq=954 (the captured value, required
    // for byte-identity per test_pc_spawn_diff).  Under UE5's reliable
    // delivery the next reliable bunch on ch=3 is chSeq=955, not some
    // arbitrary "safe" high value.  The earlier `set_ch_sequence(4095)`
    // caused the client to buffer this bunch waiting for the "missing"
    // bunches 955..4094 to arrive — explaining the 30-second disconnect
    // pattern observed in emu-20260424-135244.log.
    b.set_ch_sequence(955);
    b.set_reliable(true);
    // PM55b (2026-04-30) — ENameIdx must match channel's open-time type.
    // PC was opened with ENameIdx=102 (NAME_Actor) by ActorBuilder.  The
    // builder's default of 103 silently CNSF'd before PM54 (12-bit ChSeq
    // bug shielded the channel-type check).  Post-PM54 the header parses
    // cleanly → 103 = "ObjectRedirector" in AOC's name table mismatches
    // the open-time "Actor" → BunchWrongChannelType → connection closed.
    // Same mismatch as PM55 fixed for emit_pawn_link — applying here too.
    b.set_ch_name_hardcoded(102);  // NAME_Actor (matches PC open)
    // V2 with cmd_index=0x6A (observed in captured pkt#104).  If the
    // client rejects this we'll try 28 (our flat catalog index).
    b.add_name_update_v2(name, 0x6A);

    aoc::protocol::emit::BunchWriter bw;
    size_t bits = b.build(bw);
    if (bits == 0) {
        spdlog::error("[PcEmitter] property builder produced 0 bits");
        return false;
    }
    return host_.send_bunch_packet(client_key_, client_addr,
                                     bw.data(), bits);
}
#endif // PM57 #if 0 — V2 Name update parked

namespace {

constexpr uint32_t kPcChannel = 3;

emit::BunchWriter pc_channel_bunch(uint32_t ch_seq, bool has_exports,
                                   const uint8_t* src, size_t src_bit, size_t bits) {
    emit::BunchWriter bw;
    emit::write_reliable_actor_bunch(bw, kPcChannel, ch_seq, has_exports, src, src_bit, bits);
    return bw;
}

// BP_AOCHUD_C and its package: static GUIDs (ServerId and Randomizer 0).
constexpr uint64_t kHudClassGuid   = 8463636446363390223ULL;
constexpr uint64_t kHudPackageGuid = 7805116325763694055ULL;
constexpr uint32_t kHudClassChecksum = 0xac6f1251;

// The original server's PC bootstrap RPCs after ClientSetHUD.
constexpr uint32_t kBootstrapRpcsReplayIdx = 23;

}  // namespace

bool PcEmitter::emit_client_set_hud(const sockaddr_in& client_addr, uint32_t ch_seq) {
    // Export flags and checksums as the original server sent them; the
    // package is flagged with a zero checksum (test_pc_bootstrap).
    emit::ExportEntry package = emit::ExportEntry::asset(kHudPackageGuid, "/Game/UI/Widgets/BP_AOCHUD");
    package.has_checksum = true;
    emit::ExportEntry hud = emit::ExportEntry::asset(kHudClassGuid, "BP_AOCHUD_C", kHudClassChecksum);
    hud.outer = std::make_unique<emit::ExportEntry>(std::move(package));
    std::vector<emit::ExportEntry> exports;
    exports.push_back(std::move(hud));

    emit::BunchWriter payload;
    emit::PackageMapExporter::write_export_section(payload, exports);
    emit::FIntrepidNetworkGUID hud_class;
    hud_class.ObjectId = kHudClassGuid;
    uint8_t rpc[64] = {};
    size_t rpc_bits = 0;
    aoc::protocol::wire::write_pc_object_rpc(rpc, sizeof(rpc), rpc_bits,
                                             aoc::protocol::wire::kPcFieldIdx_ClientSetHUD, hud_class);
    payload.write_bit_range(rpc, 0, rpc_bits);

    emit::BunchWriter bw = pc_channel_bunch(ch_seq, /*has_exports=*/true,
                                            payload.data(), 0, payload.bit_pos());
    spdlog::warn("[PcEmitter] ClientSetHUD(BP_AOCHUD_C) (ch=3 chSeq={}, {} bits)",
                 ch_seq, payload.bit_pos());
    return host_.send_bunch_packet(client_key_, client_addr, bw.data(), bw.bit_pos());
}

uint32_t PcEmitter::emit_bootstrap_rpcs(const sockaddr_in& client_addr, uint32_t first_ch_seq) {
    // Captured payloads: none of these RPCs names a session object
    // (ClientVoiceHandshakeComplete, ClientEnableNetworkVoice,
    // ClientUpdateMultipleLevelsStreamingStatusCompressed,
    // ClientFlushLevelStreaming, ClientCapBandwidth, ClientSetTime).
    const std::vector<uint8_t>* raw = host_.captured_packet_raw(kBootstrapRpcsReplayIdx);
    if (!raw) {
        spdlog::warn("[PcEmitter] bootstrap RPCs: replay packet {} not loaded",
                     kBootstrapRpcsReplayIdx);
        return 0;
    }
    auto parsed = aoc::protocol::wire::parse_packet(raw->data(), raw->size(),
                                                    aoc::protocol::wire::Direction::ServerToClient);
    if (!parsed) {
        spdlog::warn("[PcEmitter] bootstrap RPCs: replay packet {} does not parse",
                     kBootstrapRpcsReplayIdx);
        return 0;
    }
    uint32_t sent = 0;
    for (const auto& b : parsed->bunches) {
        if (b.channel != kPcChannel || b.is_control || !b.is_reliable) continue;
        if (b.is_partial || b.has_package_map_exports || b.has_must_map_guids) {
            spdlog::warn("[PcEmitter] bootstrap RPCs: skipping a partial/export bunch");
            continue;
        }
        emit::BunchWriter bw = pc_channel_bunch(first_ch_seq + sent, /*has_exports=*/false,
                                                raw->data(), b.data_start_bit, b.bunch_data_bits);
        if (!host_.send_bunch_packet(client_key_, client_addr, bw.data(), bw.bit_pos())) break;
        ++sent;
    }
    spdlog::warn("[PcEmitter] bootstrap RPCs (replay pkt#{}): {} bunches from ch=3 chSeq {}",
                 kBootstrapRpcsReplayIdx, sent, first_ch_seq);
    return sent;
}
// (C-Y-B ~ C | O | A)

bool PcEmitter::emit_load_tracker(const sockaddr_in& client_addr, uint32_t ch_seq,
                                  const char* flags) {
    std::vector<bool> values;
    for (const char* c = flags; *c; ++c) values.push_back(*c == '1');
    emit::BunchWriter props_bits;
    {
        emit::RepLayoutWriter props(props_bits);
        emit::write_character_load_tracker(props, schema::pc_rep::CharacterLoadTracker, values);
        props.finish();
    }
    emit::BunchWriter block;
    emit::write_actor_property_block(block, props_bits);
    emit::BunchWriter bw = pc_channel_bunch(ch_seq, /*has_exports=*/false,
                                            block.data(), 0, block.bit_pos());
    spdlog::warn("[PcEmitter] CharacterLoadTracker = {} (ch=3 chSeq={})", flags, ch_seq);
    return host_.send_bunch_packet(client_key_, client_addr, bw.data(), bw.bit_pos());
}

bool PcEmitter::emit_pawn_link(const sockaddr_in& client_addr, uint32_t ch_seq) {
    // Possession: ClientRestart(Pawn) on the PC channel, as the original server
    // sends it after spawning the player pawn (replay_data.bin pkt#97, ch=3);
    // protocol/wire/possession.h reproduces that block bit for bit
    // (test_possession).  Replaces the PM79-PM123 probe bundle (8-bit field
    // handles, AoC "field-loop" parameters, CIC and PlayerState link), which the
    // client rejected with "ReadFieldHeaderAndPayload: Error reading payload".
    auto block = host_.allocate_player_block(client_key_);
    if (!block.is_valid()) {
        spdlog::error("[PcEmitter] emit_pawn_link: no NetGUID block for client");
        return false;
    }
    auto rnd_for = [](uint64_t obj) -> uint32_t {
        uint64_t h = obj * 0x9E3779B97F4A7C15ULL;
        h ^= (h >> 33);
        h *= 0xFF51AFD7ED558CCDULL;
        h ^= (h >> 33);
        return static_cast<uint32_t>(h);
    };
    emit::FIntrepidNetworkGUID pawn;
    pawn.ObjectId   = block.pawn;   // minted by PlayerPawnEmitter
    pawn.ServerId   = 60;
    pawn.Randomizer = rnd_for(block.pawn);
    if (use_captured_player_identity()) pawn = captured_guid(kCapturedPawnObjectId);

    uint8_t payload[64] = {};
    size_t payload_bits = 0;
    aoc::protocol::wire::write_pc_object_rpc(payload, sizeof(payload), payload_bits,
                                             aoc::protocol::wire::kPcFieldIdx_ClientRestart, pawn);

    emit::BunchWriter bw = pc_channel_bunch(ch_seq, /*has_exports=*/false, payload, 0, payload_bits);
    spdlog::warn("[PcEmitter] emit_pawn_link: ClientRestart pawn={}|{}|{} (ch=3 chSeq={}, {} bits)",
                 pawn.ObjectId, pawn.ServerId, pawn.Randomizer, ch_seq, payload_bits);
    return host_.send_bunch_packet(client_key_, client_addr, bw.data(), bw.bit_pos());
}


// ─── Phase D Step 3 Iter4 (2026-05-05) — PC.PlayerState link ────────────
//
// Sends a property-update bunch on ch=3 (PC channel) carrying our minted
// PlayerState NetGUID as the value of PC's PlayerState property.  Triggers
// `OnRep_PlayerState` on the client → the nameplate widget re-binds to
// our PS, and the PlayerNamePrivate update we sent on ch=21 becomes
// visible ("Koko" / "MaxPayne" / "Hate" instead of "Player").
//
// Probe knobs:
//   probe_player_state_emit.txt  — gate (matches PlayerStateEmitter)
//   probe_pc_ps_handle.txt       — handle of PC.PlayerState property (default 0
//                                   = AController slice's first prop, but
//                                   global RepIndex depends on parent class
//                                   counts — likely needs iteration)
//   probe_pc_ps_max.txt          — total replicated prop count for AAoCPlayerController
//                                   (default 32, may need iteration)
//   probe_pc_ps_chseq.txt        — chSeq for the bunch (default 956 after
//                                   954 open + 955 pawn_link)
bool PcEmitter::emit_player_state_link(const sockaddr_in& client_addr) {
    // Gate: dedicated probe so iter4 can be disabled independently of
    // iter2+3 (which we know work cleanly).  Default disabled — set
    // probe_pc_ps_link.txt = 1 to enable.
    //
    // First test (2026-05-05 17:59) showed iter4 with handle=9/max=32
    // crashed the client's OnRep_PlayerState 30 seconds post-possession
    // → "Connection to Realm timed out".  Iter2+3 alone are stable.
    {
        std::FILE* fp = std::fopen("probe_pc_ps_link.txt", "r");
        if (!fp) return true;
        int v = 0;
        std::fscanf(fp, "%d", &v);
        std::fclose(fp);
        if (v == 0) return true;
    }
    // Also require the broader PlayerStateEmitter probe to be on (no point
    // linking PC.PlayerState if our PS doesn't even exist on the client).
    {
        std::FILE* fp = std::fopen("probe_player_state_emit.txt", "r");
        if (!fp) return true;
        int v = 0;
        std::fscanf(fp, "%d", &v);
        std::fclose(fp);
        if (v == 0) {
            spdlog::info("[PcEmitter] emit_player_state_link skipped — "
                          "probe_pc_ps_link is on but probe_player_state_emit "
                          "is off (would link to a non-existent PS)");
            return true;
        }
    }

    auto block = host_.allocate_player_block(client_key_);
    if (!block.is_valid()) {
        spdlog::error("[PcEmitter] emit_player_state_link: no NetGUID block");
        return false;
    }

    // Same deterministic Randomizer hash used everywhere else.
    auto rnd_for = [](uint64_t obj) -> uint32_t {
        uint64_t h = obj * 0x9E3779B97F4A7C15ULL;
        h ^= (h >> 33);
        h *= 0xFF51AFD7ED558CCDULL;
        h ^= (h >> 33);
        return static_cast<uint32_t>(h);
    };

    const uint64_t ps_obj = block.player_state;
    const uint32_t ps_srv = 60;
    const uint32_t ps_rnd = rnd_for(ps_obj);

    auto read_uint = [](const char* path, uint32_t default_val) -> uint32_t {
        std::FILE* fp = std::fopen(path, "r");
        if (!fp) return default_val;
        uint32_t v = default_val;
        std::fscanf(fp, "%u", &v);
        std::fclose(fp);
        return v;
    };
// Cyb + C O A

    // ── 2026-05-05 Track 1 RE result (VERIFIED-FROM-CODE) ─────────────────
    //
    // Per docs/RE-APLAYERCONTROLLER-REPLAYOUT.md, walked from
    // AOCClient-Win64-Shipping.exe.asm:
    //   AActor                  NumReplicated = 13   (handles 1..13)
    //   AController             NumReplicated = 2    (handles 14..15)
    //     · handle 14 = PlayerState  (TObjectPtr<APlayerState>, OnRep_PlayerState)
    //     · handle 15 = Pawn         (TObjectPtr<APawn>,         OnRep_Pawn)
    //   APlayerController       NumReplicated = 2    (handles 16..17)
    //   AAoCPlayerController    NumReplicated = 19   (handles 18..36)
    //   ─────────────────────────────────────────────
    //   TOTAL NumReplicated = 36
    //   cmd_handle(PlayerState) = 14   (1-indexed in RE doc)
    //   cmd_handle(Pawn)        = 15
    //
    // Our wire writes 0-indexed values (mirrors PlayerStateEmitter's
    // PlayerNamePrivate at handle=9 for cmd_handle 10).  So the wire
    // value for PlayerState = 14 - 1 = 13, and MAX = 36.
    //
    // PRIOR DEFAULTS: handle=9, max=32 — both wrong (guesses), caused
    // the 30-second timeout we saw in the first iter4 test.
    const uint32_t pc_ps_max    = read_uint("probe_pc_ps_max.txt", 36u);
    const uint32_t pc_ps_handle = read_uint("probe_pc_ps_handle.txt", 13u);
    const uint32_t pc_ps_chseq  = read_uint("probe_pc_ps_chseq.txt", 956u);

    spdlog::warn("[PcEmitter] emit_player_state_link: PC.PlayerState = NetGUID "
                  "{}|{}|{} (handle={} max={} chSeq={})",
                  ps_obj, ps_srv, ps_rnd, pc_ps_handle, pc_ps_max, pc_ps_chseq);

    // ── 2026-05-05 POST-TIMEOUT FIX — mirror PlayerStateEmitter's pattern ──
    //
    // First iter4 attempt with handle=13/MAX=36 (Track 1 RE-verified) STILL
    // timed out at 30s.  Diagnosis: the V3 inner payload format differs
    // between iter4 (was: MODERN, no SIP NumBits) and PlayerStateEmitter
    // (LEGACY-with-SIP, plus a TRAILING terminator content block).  The
    // PlayerStateEmitter pattern is empirically proven to work on ch=21
    // (PlayerNamePrivate update lands cleanly).  Mirror it here.
    //
    // Wire structure built MANUALLY (bypassing v3_add_property_*):
    //   [V3 main content block]
    //     bit 0:    bOutermostEnd   = 0
    //     bit 1:    bIsChannelActor = 1
    //     SIP outer-NumPayloadBits  (= inner_bits below)
    //     [inner payload]:
    //       6 bits:  ceil(log2(MAX=36)) handle bits → 0-indexed = 13
    //       SIP(128): inner-NumPayloadBits = 128
    //       128 bits: FIntrepidNetGUID (4 × uint32 LSB-first)
    //   [V3 trailing terminator content block]
    //     bit 0:    bOutermostEnd   = 0
    //     bit 1:    bIsChannelActor = 1
    //     SIP(0):   NumPayloadBits = 0   (signals end of property updates)
    //
    // The trailing terminator was the key difference vs the previous iter4.

    auto serialize_int_to = [&](aoc::protocol::emit::BunchWriter& bw,
                                  uint32_t value, uint32_t max) {
        uint32_t n_bits = 0;
        if (max > 1) {
            uint32_t v = max - 1;
            while (v) { v >>= 1; ++n_bits; }
        }
        if (n_bits == 0) return;
        bw.write_bits(static_cast<uint64_t>(value), static_cast<int>(n_bits));
    };

    // PM121 (2026-05-06) — MODERN inner format per empirical capture analysis.
    //
    // Captured replay (memory: 2026-05-05 RE Session): pkt#127 b1 has a clean
    // V3 actor block with `handle=1, MAX=10, value=u8=66` — NO SIP between
    // handle and value.  Same pattern likely applies to iter4 (FObjectProperty
    // PlayerState).
    //
    // Wire format (MODERN — what AOC actually expects):
    //   [handle, ceil(log2(MAX)) bits]
    //   [128-bit FIntrepidNetGUID — type-known size, NO NumBits SIP]
    //
    // Probe: probe_pc_ps_modern.txt (default 1) — set to 0 to revert to
    // legacy `handle + SIP(128) + 128 bits` for A/B comparison.
    const uint32_t use_modern = read_uint("probe_pc_ps_modern.txt", 1u);

    aoc::protocol::emit::BunchWriter inner;
    serialize_int_to(inner, pc_ps_handle, pc_ps_max);   // 6 bits (MAX=36)
    if (!use_modern) {
        inner.write_sip(128);                            // legacy NumBits SIP
    }
    inner.write_uint32(static_cast<uint32_t>(ps_obj));
    inner.write_uint32(static_cast<uint32_t>(ps_obj >> 32));
    inner.write_uint32(ps_srv);
    inner.write_uint32(ps_rnd);
    const uint32_t inner_bits = static_cast<uint32_t>(inner.bit_pos());

    // V3 main content block ONLY — no trailing terminator (PM120-FIX/PM97).
    // AOC's outer loop exits on bunch.AtEnd() after the content block ends.
    // Adding a trailing block with bIsChannelActor=1 makes AOC try to read
    // more property fields and overflow on the empty content.
    aoc::protocol::emit::BunchWriter wrapped(256);
    wrapped.write_bit(0);                    // bOutermostEnd = 0
    wrapped.write_bit(1);                    // bIsChannelActor = 1
    wrapped.write_sip(inner_bits);
    wrapped.write_bit_range(inner.data(), 0, inner_bits);
    const uint32_t wrapped_bits = static_cast<uint32_t>(wrapped.bit_pos());

    spdlog::warn("[PcEmitter] emit_player_state_link [POST-TIMEOUT FIX]: "
                  "inner_bits={} wrapped_bits={}", inner_bits, wrapped_bits);

    aoc::protocol::emit::PropertyUpdateBunchBuilder b;
    b.set_channel(3);
    b.set_ch_sequence(pc_ps_chseq);
    b.set_reliable(true);
    b.set_ch_name_hardcoded(102);            // NAME_Actor (matches PC open)
    b.set_use_modern_inner_format(true);     // (unused — we add_raw_payload below)
    b.set_is_rep_paused(false);
    b.set_has_mbg(true);

    b.add_raw_payload(wrapped.data(), wrapped_bits);

    aoc::protocol::emit::BunchWriter bw(256);
    const size_t bits = b.build(bw);
    if (bits == 0) {
        spdlog::error("[PcEmitter] emit_player_state_link: builder returned 0 bits");
        return false;
    }
    const bool ok = host_.send_bunch_packet(client_key_, client_addr,
                                              bw.data(), bits);
    spdlog::warn("[PcEmitter] emit_player_state_link sent: bits={} ok={}",
                  bits, ok);
    return ok;
}
// C Y B and C O A

// ───────────────────────────────────────────────────────────────────────────
// PM147 (2026-05-08) — ClientUpdateLevelStreamingStatus keepalive
//
// Black-screen RE: after possession completes and the loading screen drops,
// the client's GC sweep unloads every World Partition cell because nothing
// holds them alive.  `LogLoadingScreen` shows the timeout, then dozens of
// `LogNet: NotifyStreamingLevelUnload` lines fire in a single frame.
//
// Stock UE5 fix: server tracks each client's level visibility via
// `ServerUpdateLevelVisibility` (the client→server RPC we see at 104.2 bytes
// in our test) and replies with `ClientUpdateLevelStreamingStatus` to confirm
// each cell should stay loaded.
//
// Wire format taken from the APlayerController.h declaration plus
// AOC's intern table position 0x3F0 (offset 0xB0 from ClientRestart=0x340 =
// 22 entries, candidate handle range 65-75 — default 70).
//
// UFunction signature (UE 5.x):
//   ClientUpdateLevelStreamingStatus(
//       FName    PackageName,                  // FName::NetSerialize
//       bool     bNewShouldBeLoaded,           // 1 bit
//       bool     bNewShouldBeVisible,          // 1 bit
//       bool     bNewShouldBlockOnLoad,        // 1 bit
//       int32    LODIndex                      // 32 bits LSB-first
//       // [Optional]: FNetLevelVisibilityTransactionId TransactionId (UE 5.1+)
//       // [Optional]: bool bNewShouldBlockOnUnload (UE 5.2+)
//   )
//
// Per UE 5.0 wire (most likely AOC's version): no transaction ID, no unload-
// block bool.  Total payload: ~9 bits FString length + N×8 bits chars + 32
// bits Number + 3 bits + 32 bits = roughly (FString_bytes*8) + 76 bits.
//
// Strategy: send ONE keepalive RPC for the persistent level
// `Verra_World_Master`.  If the cell-level granularity is required, we'll
// expand this to iterate captured cell hashes from a probe file.
// ───────────────────────────────────────────────────────────────────────────
bool PcEmitter::emit_client_update_level_streaming_status(
    const sockaddr_in& client_addr) {

    // Gate on probe.  Default disabled.
    {
        std::FILE* fp = std::fopen("probe_streaming_keepalive.txt", "r");
        if (!fp) return true;
        int v = 0;
        std::fscanf(fp, "%d", &v);
        std::fclose(fp);
        if (v == 0) return true;
    }

    auto read_uint = [](const char* path, uint32_t default_val) -> uint32_t {
        std::FILE* fp = std::fopen(path, "r");
        if (!fp) return default_val;
        uint32_t v = default_val;
        std::fscanf(fp, "%u", &v);
        std::fclose(fp);
        return v;
    };
    auto read_string = [](const char* path, const std::string& default_val) -> std::string {
        std::FILE* fp = std::fopen(path, "r");
        if (!fp) return default_val;
        char buf[256] = {0};
        if (std::fgets(buf, sizeof(buf), fp)) {
            std::fclose(fp);
            std::string s = buf;
            // trim trailing whitespace / newline
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                                    s.back() == ' ' || s.back() == '\t')) {
                s.pop_back();
            }
            return s;
        }
        std::fclose(fp);
        return default_val;
    };

    const uint32_t handle     = read_uint("probe_streaming_keepalive_handle.txt", 70u);
    const uint32_t handle_max = read_uint("probe_streaming_keepalive_max.txt", 1024u);
    const uint32_t chseq      = read_uint("probe_streaming_keepalive_chseq.txt", 957u);
    const std::string package = read_string(
        "probe_streaming_keepalive_package.txt",
        "/Game/Levels/Verra_World_Master/Verra_World_Master");

    spdlog::warn("[PcEmitter] emit_client_update_level_streaming_status: "
                  "handle={} max={} chSeq={} package=\"{}\"",
                  handle, handle_max, chseq, package);

    // ── Build the RPC inner payload ─────────────────────────────────────────
    aoc::protocol::emit::BunchWriter inner(256);

    // FName PackageName: AOC FName wire format = [bIsHardcoded(1)][...]
    // For non-hardcoded names: write [0][FString][int32 Number=0]
    inner.write_bit(0);                          // bIsHardcoded = 0
    // FString: int32 SaveNum (= 1 + chars.size() including NUL) + chars + NUL
    aoc::protocol::emit::PackageMapExporter::write_fstring(inner, package);
    inner.write_uint32(0);                       // Number = 0

    // Three bool bits (in declaration order):
    inner.write_bit(1);                          // bNewShouldBeLoaded   = true
    inner.write_bit(1);                          // bNewShouldBeVisible  = true
    inner.write_bit(0);                          // bNewShouldBlockOnLoad = false

    // int32 LODIndex
    inner.write_uint32(0);                       // LODIndex = 0
// -- C O A x Cyb --

    const uint32_t inner_bits = static_cast<uint32_t>(inner.bit_pos());

    // ── Wrap in RPC content block ──────────────────────────────────────────
    auto serialize_int_to = [&](aoc::protocol::emit::BunchWriter& bw,
                                  uint32_t value, uint32_t max) {
        uint32_t n_bits = 0;
        if (max > 1) {
            uint32_t v = max - 1;
            while (v) { v >>= 1; ++n_bits; }
        }
        if (n_bits == 0) return;
        bw.write_bits(static_cast<uint64_t>(value), static_cast<int>(n_bits));
    };

    aoc::protocol::emit::BunchWriter rpc_payload(512);
    serialize_int_to(rpc_payload, handle, handle_max);   // 10 bits if max=1024
    rpc_payload.write_sip(inner_bits);                   // SIP NumPayloadBits
    rpc_payload.write_bit_range(inner.data(), 0, inner_bits);
    const uint32_t rpc_bits = static_cast<uint32_t>(rpc_payload.bit_pos());

    aoc::protocol::emit::BunchWriter wrapped(512);
    wrapped.write_bit(0);                        // bOutermostEnd  = 0
    wrapped.write_bit(1);                        // bIsChannelActor = 1 (PC)
    wrapped.write_sip(rpc_bits);                 // outer NumPayloadBits
    wrapped.write_bit_range(rpc_payload.data(), 0, rpc_bits);
    const uint32_t wrapped_bits = static_cast<uint32_t>(wrapped.bit_pos());

    spdlog::info("[PcEmitter] keepalive: inner_bits={} rpc_bits={} wrapped_bits={}",
                  inner_bits, rpc_bits, wrapped_bits);

    // ── Build the bunch ────────────────────────────────────────────────────
    aoc::protocol::emit::PropertyUpdateBunchBuilder b;
    b.set_channel(3);
    b.set_ch_sequence(chseq);
    b.set_reliable(true);
    b.set_ch_name_hardcoded(102);                // NAME_Actor (PC channel)
    b.set_use_modern_inner_format(true);
    b.set_is_rep_paused(false);
    b.set_has_mbg(true);
    b.add_raw_payload(wrapped.data(), wrapped_bits);

    aoc::protocol::emit::BunchWriter bw(512);
    const size_t bits = b.build(bw);
    if (bits == 0) {
        spdlog::error("[PcEmitter] keepalive: builder returned 0 bits");
        return false;
    }

    const bool ok = host_.send_bunch_packet(client_key_, client_addr,
                                              bw.data(), bits);
    spdlog::warn("[PcEmitter] keepalive sent: bits={} ok={}", bits, ok);
    return ok;
}

}} // namespace aoc::net
