// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/appearance_emitter.cpp  —  Phase D Step 2.3 (2026-05-05)
//
//  See appearance_emitter.h for high-level design and rationale.
//
//  PD2.3 (2026-05-05) — REWRITE for split-bunch mode 2.
//
//  Background: PD2.1 attempted to inline the FCharacterCustomizationSaveData
//  bytes inside the Pawn ActorOpen bunch.  That bunch already carries 8
//  subobject NetGUID exports (~6632 bits).  Adding the 1701-bit appearance
//  payload pushed the total to 8341 bits — over the UE5 SerializeInt
//  (MAX=8192) BunchDataBits cap by 149 bits, which truncated the bunch
//  shape and broke possession entirely (no ServerAcknowledgePossession).
//
//  PD2.3 fix: emit the appearance update as a SEPARATE reliable bunch on
//  ch=19 chSeq=956+, AFTER the pawn ActorOpen lands and possession is
//  acknowledged.  Wire format:
//
//      [bunch hdr ch=19 reliable=true chSeq=956 ChName=NAME_Actor(102)]
//      [V3 content block:
//          bHasRepLayout=1
//          bIsActor=0
//          SIP sub_guid = (pawn + 8 = 16777226 = CharacterAppearance subobject)
//          bStablyNamed=1
//          SIP NumPayloadBits=N
//          [N bits of property-update stream from build_appearance_payload_bits]
//      ]
//      [trailing actor-root content block:
//          bHasRepLayout=0
//          bIsActor=1
//          SIP NumPayloadBits=0
//      ]
//
//  This pattern matches what AOC's own server uses: minimal ActorOpen first,
//  then separate property-update bunches as state changes.  The 8192-bit
//  cap applies per-bunch, so splitting into two bunches solves it cleanly.
//
//  LAYER:   net
//  OWNER:   Phase D Step 2.3
//  SESSION: 2026-05-05
// ============================================================================
#include "net/appearance_emitter.h"
#include "net/native_connect_sequencer.h"   // IGameServerHost
#include "net/appearance_data.h"             // build_appearance_payload_bits
#include "protocol/emit/property_update_bunch_builder.h"
#include "protocol/emit/bunch_writer.h"
// (C | Y | B ~ CoA)

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#endif

#include <spdlog/spdlog.h>
#include <cstdio>

namespace aoc { namespace net {

namespace {

uint32_t read_uint_file(const char* path, uint32_t default_val) {
    std::FILE* fp = std::fopen(path, "r");
    if (!fp) return default_val;
    uint32_t n = default_val;
    std::fscanf(fp, "%u", &n);
    std::fclose(fp);
    return n;
}

/// Inline mirror of actor_builder.cpp::write_v3_subobject_stably_named.
///
/// PM131-REV2 (2026-05-07) — empirical wire format confirmed via test logs:
///   bit 0: bOutermostEnd     (0 = more content blocks; 1 = exit loop)
///   bit 1: bIsChannelActor   (0 = subobject; 1 = the channel's actor)
///   bits: SIP(SubobjectNetGUID)  (only when bIsChannelActor=0)
///   bit:  bStablyNamed       (1 = lookup by name; 0 = read class_guid SIP next)
///   bits: SIP(NumPayloadBits)
///   bits: NumPayloadBits worth of payload (property updates)
///
/// PM120 was a partial fix that nailed the bOutermostEnd issue but dropped
/// the bStablyNamed bit entirely, causing the receiver to read the LSB of
/// SIP NumPayloadBits as bStablyNamed=0 → expect class_guid → garbage →
/// ContentBlockFail.  PM131-REV2 reinstates bStablyNamed=1 (since these
/// are stably-named subobject paths registered via PM117).
void write_v3_stably_named_block(aoc::protocol::emit::BunchWriter& out,
                                   uint64_t sub_guid,
                                   const uint8_t* payload_bits,
                                   uint32_t num_payload_bits) {
    out.write_bit(0);                         // bOutermostEnd = 0
    out.write_bit(0);                         // bIsChannelActor = 0 (subobject)
    out.write_sip(sub_guid);                  // SIP SubobjectNetGUID
    out.write_bit(1);                         // bStablyNamed = 1 (PM131-rev2)
    out.write_sip(num_payload_bits);          // SIP NumPayloadBits
    if (num_payload_bits > 0 && payload_bits != nullptr) {
        out.write_bit_range(payload_bits, 0, num_payload_bits);
    }
}

/// PM136 (2026-05-07) — V3 DYNAMIC-CREATION content block, FINAL FORM.
///
/// Per F5 RE of sub_143F2C340 (ReadContentBlockHeader) lines 369-471:
/// the post-`bStablyNamed=0` parsing branches on `EngineNetVer`:
///   - if engine_ver < 0x1E (30, AOC-added history slot): NO extra bit,
///     SerializeObject reads the class GUID DIRECTLY.
///   - if engine_ver >= 0x1E: read `bSkipClass` bit, then optional 8-bit
///     class index instead of full GUID.
///
/// `EngineNetVer` is per-connection, written by the NMT_Hello handshake.
/// Stock UE 5.6 max history is 21; 30 is an AOC-specific addition.  Our
/// server's NMT_Hello announces a stock value (< 30), so AOC takes the
/// legacy branch — no extra bit.  The `bSkipClass` bit added in PM135 was
/// a phantom that shifted the FIntrepidNetGUID by 1 bit on the wire.
///
/// PM135 BUG: wrote `bSkipClass=0` between bStablyNamed and class_guid.
/// AOC didn't read that bit (engine_ver branch), so it consumed the first
/// bit of our 128-bit class_guid as part of the (non-existent) gate field
/// — misalignment cascaded through the FIntrepidNetGUID → ClassNetGUID
/// resolved to garbage → SerializeObject returned class=nullptr → fallback
/// path tried to read a stably-named outer chain → garbage → Bunch.IsError.
/// Empirically confirmed in 2026-05-07 16:28:43 test: bunch arrives, no
/// `Instantiating sub-object` log fires.
///
/// PM141 (2026-05-07) — V3 DYNAMIC-CREATION content block, FROM LIVE PCAP.
///
/// Source: live production AOC server pcap
///   archive/misc/PCAPRepo-main/character/aoc_ranger_respawn_home_point_j_20260205_230233.pcap
/// Specifically pkt#122 ch=11288 ActorOpen Block 9 (bit 1735, 8 bytes
/// `3c 0f c8 20 d2 64 1c e3`).  Captured BEFORE the AOC server shutdown.
///
/// THREE CORRECTIONS vs PM138:
///   1. `bHasRepLayout = 0` (was 1) — live wire bit 0 is 0 even when
///      NumPayloadBits IS present.  AOC always emits NumPayloadBits in
///      dynamic-creation blocks regardless of bHasRepLayout.  The parallel
///      emu's doc §5.1.1 was overly aggressive about this gate.
///   2. ADD `bIsDestroyMessage = 0` between bStablyNamed and class_guid.
///      Live wire HAS this bit; the parallel emu's actor_replication_v3.py
///      defaulted has_destroy_flag=False, but live AOC sends the bit.
///   3. ADD `bActorIsOuter = 1` between class_guid and NumPayloadBits.
///      Same source: live wire HAS this bit; parser default was False.
///
/// Without bits (2) and (3), the receiver consumed the LSBs of our SIP
/// class_guid as those gates → SIP read at wrong offset → garbage class
/// → bail before `Instantiating sub-object` log.  Confirmed from live
/// capture decode that toggling has_destroy_flag/has_outer_chain to True
/// makes the bit count reconcile exactly to bunch-end (without them, 21
/// bits dangle).
///
/// CORRECTED FORMAT:
///   [1 bit bHasRepLayout    = 0]
///   [1 bit bIsActor         = 0]
///   [SIP   sub_guid]
///   [1 bit bStablyNamed     = 0]
///   [1 bit bIsDestroyMessage = 0]   ← PM141 ADDED
///   [SIP   class_guid]
///   [1 bit bActorIsOuter    = 1]    ← PM141 ADDED (1 = outer is the channel actor)
///   [SIP   NumPayloadBits]
///   [<NumPayloadBits> bits payload]
void write_v3_dynamic_creation_block(aoc::protocol::emit::BunchWriter& out,
                                       uint64_t sub_guid,
                                       uint64_t class_obj_id,
                                       uint32_t /*class_server_id*/,
                                       uint32_t /*class_randomizer*/,
                                       const uint8_t* payload_bits,
                                       uint32_t num_payload_bits) {
    out.write_bit(0);                         // PM141: bHasRepLayout = 0
    out.write_bit(0);                         // bIsActor = 0 (subobject)
    out.write_sip(sub_guid);                  // SIP SubobjectNetGUID
    out.write_bit(0);                         // bStablyNamed = 0 (dynamic)
    out.write_bit(0);                         // PM141: bIsDestroyMessage = 0
    out.write_sip(class_obj_id);              // SIP class NetGUID
    out.write_bit(1);                         // PM141: bActorIsOuter = 1 (channel actor)
    out.write_sip(num_payload_bits);          // SIP NumPayloadBits
    if (num_payload_bits > 0 && payload_bits != nullptr) {
        out.write_bit_range(payload_bits, 0, num_payload_bits);
    }
}
// == C_Y_B | C O A ==

/// PM120-FIX — DO NOT append a trailing block.  Per PM97 v3_finish_bunch
/// comment, AOC reads 2 bits before deciding: any extra bits get parsed as
/// another content block header.  bIsChannelActor=1 with SIP(0) makes AOC
/// expect more property update fields and overflows on the empty content.
/// Leaving this stub for callers that still reference it; it's now a no-op.
void write_trailing_actor_root_block(aoc::protocol::emit::BunchWriter& out) {
    (void)out;
    // intentionally empty — see comment above
}

}  // namespace

AppearanceEmitter::AppearanceEmitter(IGameServerHost& host,
                                       const std::string& client_key)
    : host_(host), client_key_(client_key) {}

bool AppearanceEmitter::emit_seed(const sockaddr_in& client_addr,
                                    const AppearanceSeed& seed) {
    // PD2.3 (2026-05-05) — gate on probe_appearance_emit.txt.
    //
    // Default disabled while we test the wire-shape changes.  Set to "1"
    // to enable the separate-bunch CharacterAppearance update.
    {
        std::FILE* fp = std::fopen("probe_appearance_emit.txt", "r");
        bool enabled = false;
        if (fp) {
            int v = 0;
            std::fscanf(fp, "%d", &v);
            std::fclose(fp);
            enabled = (v != 0);
        }
        if (!enabled) {
            spdlog::info("[AppearanceEmitter] disabled "
                         "(probe_appearance_emit.txt absent or \"0\") — "
                         "possession path stays clean");
            return true;  // no-op success
        }
    }

    auto block = host_.allocate_player_block(client_key_);
    if (!block.pawn) {
        spdlog::error("[AppearanceEmitter] no NetGUID block for client");
        return false;
    }

    // ── 2026-05-05 SDK-verified RE result (UCharacterAppearanceComponent) ──
    //
    // Source: the Dumper-7 SDK dump of the client, GameSystemsPlugin_classes.hpp
    //   line 17811 (UCharacterAppearanceComponent), 17720 (UBaseModularAppearanceComponent)
    //         + Engine_classes.hpp line 897 (UActorComponent base class).
    //
    // Full CPF_Net property chain (parent → leaf, 1-indexed cmd_handle):
    //   1. bReplicates                (UActorComponent — bit 1)
    //   2. bIsActive                  (UActorComponent — bit 1)
    //   3. AppearanceIDs              (UBaseModularAppearanceComponent — TArray<FAppearanceId>)
    //   4. SharedAppearanceInfoId     (UBaseModularAppearanceComponent — FAppearanceInfoId)
    //   5. CharacterCustomization     (UCharacterAppearanceComponent — FCharacterCustomizationSaveData)
    //   6. bForceHideHeldItems        (UCharacterAppearanceComponent — bit 1)
    //
    // NumReplicated = 6.  Wire MAX = 6.  Handle bits = ceil(log2(6)) = 3.
    // 0-indexed wire handles (matches our PlayerStateEmitter convention):
    //   CharacterCustomization → 4
    //   bForceHideHeldItems    → 5
    //
    // PRIOR DEFAULTS (guesses): max=4, handle_custom=0, handle_force_hide=1/3.
    // Both wrong — likely caused the appearance OnRep to dispatch to the
    // wrong field, contributing to the asset-load failures observed in
    // PD2.3.1 strip-assets fallback.
    const uint32_t pawn_channel  = read_uint_file("probe_app_channel.txt", 19u);
    const uint32_t pawn_chseq    = read_uint_file("probe_app_chseq.txt",   956u);
    const uint32_t handle_max    = read_uint_file("probe_appearance_max.txt", 6u);
    const uint32_t handle_custom = read_uint_file("probe_appearance_handle_custom.txt",     4u);
    const uint32_t handle_force_hide = read_uint_file("probe_appearance_handle_force_hide.txt", 5u);

    // PM117 confirmed: subobject NetGUIDs are pawn_obj + ci + 1, where ci
    // is the slot index.  CharacterAppearance is slot 7 → +8 offset.
    // Verified in PlayerPawnEmitter::emit_open log line "Character Appearance
    // (slot 7) netguid=16777226 outer_ref=16777218".
    const uint64_t appearance_subobj_guid = block.pawn + 8;

    // ── Build the appearance property-update payload ─────────────────────
    //
    // PD2.3 (2026-05-05): pull the customization JSON for the currently
    // selected character from XClient's store via the IGameServerHost
    // accessor.  parse_customization_json populates the struct with REAL
    // per-character data (skin/hair/eye/race/etc) so OnRep_Character-
    // Customization on the client assembles the actual chosen mesh
    // instead of an empty default body.
    aoc::net::CharacterCustomizationData cc;
    cc.force_hide_held_items = seed.force_hide_held_items;
// C Y B / CoA

    const std::string json = host_.current_character_customization_json();
    if (!json.empty()) {
        const bool ok = aoc::net::parse_customization_json(json, cc);
        spdlog::warn("[AppearanceEmitter] PD2.3 loaded JSON {}B parsed={} "
                     "(class='{}'-derived skin_hue={:.3f} head_hair={} eye_color={})",
                     json.size(), ok,
                     cc.class_enum, cc.skin_color_hue,
                     cc.head_hair, cc.eye_color);

        // PD2.3.1 (2026-05-05) — REGRESSION RECOVERY.
        //
        // Empirical finding from the previous test: when the JSON-derived
        // fields contain real Race/asset-hash values (race=2 Kaelar,
        // head_hair=6064632019650478080, etc), the client's OnRep_Character-
        // Customization tries to LOAD the corresponding SkeletalMesh /
        // appearance assets.  Without the lobby-side asset registry preload
        // that the real AOC server does, those loads fail silently → the
        // appearance subobject's state corrupts → ClientRestart's
        // AcknowledgePossession never fires.
        //
        // Probe knob `probe_appearance_strip_assets.txt` (default 1):
        //   1 = ZERO out asset-reference fields (race, head_hair, eye_color,
        //       skin_set, etc.) before serializing.  Keeps possession alive
        //       while we figure out asset preload.  Floats and bools pass
        //       through unchanged so customization still has SOME visible
        //       effect.
        //   0 = pass JSON values through verbatim (will likely break
        //       possession until asset preload is wired).
        const uint32_t strip = read_uint_file("probe_appearance_strip_assets.txt", 1u);
        if (strip != 0) {
            // Asset-reference fields cleared.  Float sliders + bools + flags
            // are kept (the client's mesh-defaults handler will use them
            // to nudge the auto-generated body).
            cc.preset_guid          = 0;
            cc.skin_set             = 0;
            cc.eye_color            = 0;
            cc.eye_shape            = 0;
            cc.sclera_shape         = 0;
            cc.eyebrows             = 0;
            cc.head_hair            = 0;
            cc.head_hair_root_color = 0;
            cc.head_hair_tip_color  = 0;
            cc.facial_hair_lip      = 0;
            cc.facial_hair_chin     = 0;
            cc.facial_hair_cheek    = 0;
            cc.facial_hair_root_color = 0;
            cc.facial_hair_tip_color  = 0;
            cc.racial_horns         = 0;
            cc.nail_color           = 0;
            cc.gender_enum          = 0;
            cc.race_enum            = 0;
            cc.class_enum           = 0;
            spdlog::warn("[AppearanceEmitter] PD2.3.1 STRIP_ASSETS=1 — "
                         "asset-reference fields zeroed (possession-safe). "
                         "Set probe_appearance_strip_assets.txt=0 to send "
                         "real asset IDs once preload is implemented.");
        }
    } else {
        spdlog::warn("[AppearanceEmitter] PD2.3 no customization JSON available "
                     "from host — falling back to neutral defaults");
        // Sensible defaults so the client doesn't get NaN-ish floats.
        cc.skin_color_hue          = 0.5f;
        cc.skin_color_pigmentation = 0.5f;
        cc.head_hair_length        = 0.5f;
        cc.is_helmet_visible       = true;
        cc.is_cape_visible         = true;
    }

    std::vector<uint8_t> appearance_bits;
    const uint32_t num_payload_bits = aoc::net::build_appearance_payload_bits(
        cc,
        handle_max,
        handle_custom,
        handle_force_hide,
        appearance_bits);

    if (num_payload_bits == 0) {
        spdlog::warn("[AppearanceEmitter] PD2.3 — payload mode produced 0 "
                     "bits; sending empty V3 wrap (subobject touch only)");
    }

    // ── PM133 (2026-05-07) — Dynamic-creation mode selector ─────────────
    //
    // probe_appearance_v3_mode.txt:
    //   "0" or absent → STABLY-NAMED (PM131-rev2 form, default; subobject
    //                   must already exist on the channel actor's BP CDO
    //                   for OnRep to dispatch — empirically failing for
    //                   PlayerPawn_C 'Character Appearance' lookup)
    //   "1"           → DYNAMIC-CREATION (PM133 form, mirrors GlobalGMCommands
    //                   on the PC channel; client INSTANTIATES a fresh
    //                   UCharacterAppearanceComponent and routes properties
    //                   to it).  Requires the class to be exported via
    //                   PackageMap first — gated by probe_class_exports.txt
    //                   on the PlayerPawnEmitter side.
    int v3_mode = 0;
    if (std::FILE* fp = std::fopen("probe_appearance_v3_mode.txt", "r")) {
        std::fscanf(fp, "%d", &v3_mode);
        std::fclose(fp);
    }

    // The CharacterAppearanceComponent class NetGUID is generated by
    // hash_class_path() in player_pawn_emitter.cpp's component schema
    // setup.  Hard-coded here to the same deterministic value so we
    // don't need a live cross-emitter accessor.  hash_class_path mints
    // ObjectId = (deterministic 64-bit hash with high bit set).
    //
    // Sanity: the value MUST match what player_pawn_emitter computes.
    // Both use FNV-1a-style hashing of the class_path; we just call the
    // same function here.  See hash_class_path in package_map_exporter.h
    // (or the inline equivalent if it isn't exposed).
    // EXACTLY mirror player_pawn_emitter.cpp::hash_class_path (PM142).
    // Small Static ObjectId in [17, 32785], odd.  See player_pawn_emitter.cpp
    // for full rationale (live AOC uses tiny ObjectIds for class GUIDs).
    auto hash_class_path = [](const char* p) -> uint64_t {
        uint64_t h = 0xCBF29CE484222325ULL;     // FNV-1a offset basis
        for (const char* s = p; *s; ++s) {
            h ^= static_cast<uint64_t>(static_cast<unsigned char>(*s));
            h *= 0x100000001B3ULL;              // FNV-1a prime
        }
        h ^= (h >> 33);
        h *= 0xFF51AFD7ED558CCDULL;
        h ^= (h >> 33);
        h &= 0x3FFFULL;                         // 14 bits
        h <<= 1;                                // shift up
        h |= 1ULL;                              // bit 0 = Static
        h |= 0x10ULL;                           // > 16
        return h;
    };
    const uint64_t appearance_class_guid =
        hash_class_path("/Script/GameSystemsPlugin.CharacterAppearanceComponent");

    aoc::protocol::emit::BunchWriter wrapped(256);
    if (v3_mode == 1) {
        // PM135 — class_guid is a FULL FIntrepidNetGUID (128 bits), not SIP.
        // ServerId=0/Randomizer=0 matches what player_pawn_emitter exports
        // via build_two_level (leaf has those zeroed in the export entry).
        write_v3_dynamic_creation_block(
            wrapped,
            appearance_subobj_guid,
            appearance_class_guid,           // ObjectId (low 64 bits)
            /*server_id=*/  0u,
            /*randomizer=*/ 0u,
            appearance_bits.empty() ? nullptr : appearance_bits.data(),
            num_payload_bits);
        spdlog::warn("[AppearanceEmitter] PM135 DYNAMIC-CREATION mode: "
                      "sub_guid={} class_guid={{ObjectId={}, ServerId=0, Randomizer=0}} "
                      "(UCharacterAppearanceComponent)",
                      appearance_subobj_guid, appearance_class_guid);
    } else {
        write_v3_stably_named_block(
            wrapped,
            appearance_subobj_guid,
            appearance_bits.empty() ? nullptr : appearance_bits.data(),
            num_payload_bits);
    }
    write_trailing_actor_root_block(wrapped);

    const uint32_t total_wrapped_bits = static_cast<uint32_t>(wrapped.bit_pos());
// == CoA | C | Y | B ==

    // ── Build the bunch ──────────────────────────────────────────────────
    aoc::protocol::emit::PropertyUpdateBunchBuilder b;
    b.set_channel(pawn_channel);
    b.set_ch_sequence(pawn_chseq);
    b.set_reliable(true);
    b.set_ch_name_hardcoded(102);   // NAME_Actor — same as Pawn channel
    b.set_use_modern_inner_format(true);
    // Empirical from captured bunches: subobject-targeting property
    // updates set bIsReplicationPaused=1.  Without it, V3 silent-drops.
    b.set_is_rep_paused(true);
    // Most captured ch=3 property updates set bHasMustBeMappedGUIDs=1.
    // Setting it without an inline MBG list is a safe hint per UE5 spec.
    b.set_has_mbg(true);

    // Inject our pre-rendered V3 + trailer bytes as the bunch payload.
    b.add_raw_payload(wrapped.data(), total_wrapped_bits);

    spdlog::warn("[AppearanceEmitter] PD2.3 SEPARATE BUNCH ch={} chSeq={} "
                  "sub_guid={} max={} h_custom={} h_force={} "
                  "appearance_bits={} wrapped_bits={}",
                  pawn_channel, pawn_chseq,
                  appearance_subobj_guid, handle_max,
                  handle_custom, handle_force_hide,
                  num_payload_bits, total_wrapped_bits);

    // ── Send ─────────────────────────────────────────────────────────────
    aoc::protocol::emit::BunchWriter bw(512);
    const size_t bunch_bits = b.build(bw);
    const bool ok = host_.send_bunch_packet(client_key_, client_addr,
                                              bw.data(), bunch_bits);
    spdlog::warn("[AppearanceEmitter] PD2.3 bunch sent: total_bits={} "
                  "wrapped_inner_bits={} ok={}",
                  bunch_bits, total_wrapped_bits, ok);
    return ok;
}

}}  // namespace aoc::net
