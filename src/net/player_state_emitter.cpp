// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/player_state_emitter.cpp  —  Phase D Step 3 (2026-05-05)
//
//  See player_state_emitter.h for high-level design.
//
//  ── Iteration 2 (this file): real ActorOpen ─────────────────────────
//
//  Builds and sends a PlayerState ActorOpen bunch on ch=21 (next free
//  channel after Pawn ch=19).  Mirrors PlayerPawnEmitter's pattern but
//  much simpler since PlayerState is not spawned in world (no transform
//  data).
//
//  Wire structure (single ActorOpen bunch on ch=21):
//    [Bunch header: bControl=1 bOpen=1 reliable=1 ch=21 chSeq=954 ChName=NAME_Actor]
//    [PackageMap exports:
//        [0] Default__AoCPlayerStateBP_C + outer /Game/.../AoCPlayerStateBP
//        [1] PersistentLevel chain (same as PC/Pawn)
//    ]
//    [SerializeNewActor:
//        actor_netguid = block.player_state
//        archetype_netguid = AAoCPlayerStateBP_C class CDO hash
//        level_netguid = 16442478405498561049 (PersistentLevel)
//        no location/rotation/scale (PlayerState is a non-spawned actor)
//    ]
//    [No spliced tail — PlayerState opens with CDO default property values]
//
//  ── Future iterations (after baseline confirmed) ────────────────────
//
//  Iter 3 — emit_properties: send property update for PlayerName, Character-
//           Archetype, CharacterGuid via PropertyUpdateBunchBuilder on ch=21
//           chSeq=955.
//  Iter 4 — Add PcEmitter::emit_player_state_link to wire PC.PlayerState →
//           our minted PS NetGUID (mirrors emit_pawn_link).
//
//  LAYER:   net
//  OWNER:   Phase D Step 3
//  SESSION: 2026-05-05
// ============================================================================
#include "net/player_state_emitter.h"
#include "net/native_connect_sequencer.h"   // IGameServerHost

#include "protocol/emit/actor_builder.h"
#include "protocol/emit/bunch_writer.h"
#include "protocol/emit/intrepid_netguid.h"
#include "protocol/emit/property_update_bunch_builder.h"
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
#include <cstdio>

namespace aoc { namespace net {
// [CoA :: C-Y-B]

using namespace aoc::protocol;

namespace {

/// Read probe_player_state_emit.txt.  Default disabled (returns false).
bool is_enabled() {
    std::FILE* fp = std::fopen("probe_player_state_emit.txt", "r");
    if (!fp) return false;
    int v = 0;
    std::fscanf(fp, "%d", &v);
    std::fclose(fp);
    return v != 0;
}

/// Deterministic per-ObjectId Randomizer (same hash PcEmitter / Player-
/// PawnEmitter use).  Required so the client's NetGUID cache binds the
/// SAME randomizer for repeated lookups of this PlayerState.
uint32_t rnd_for(uint64_t obj) {
    uint64_t h = obj * 0x9E3779B97F4A7C15ULL;
    h ^= (h >> 33);
    h *= 0xFF51AFD7ED558CCDULL;
    h ^= (h >> 33);
    return static_cast<uint32_t>(h);
}

/// Build a 2-level export chain (leaf class CDO + outer asset path).
/// Mirrors PcEmitter::build_two_level / PlayerPawnEmitter::build_two_level.
emit::ExportEntry build_two_level(uint64_t leaf_obj,
                                    const std::string& leaf_path,
                                    uint32_t leaf_checksum,
                                    uint64_t outer_obj,
                                    const std::string& outer_path,
                                    uint32_t outer_checksum,
                                    bool no_load = false) {
    emit::ExportEntry leaf  = emit::ExportEntry::asset(
        leaf_obj, leaf_path, leaf_checksum, no_load);
    emit::ExportEntry outer = emit::ExportEntry::asset(
        outer_obj, outer_path, outer_checksum, no_load);
    leaf.outer = std::make_unique<emit::ExportEntry>(std::move(outer));
    return leaf;
}

/// Build a 3-level export chain (PersistentLevel inside Verra_World_Master
/// inside the level package).  Same chain PcEmitter uses for PC and
/// PlayerPawnEmitter uses for Pawn — content-addressable, identical NetGUIDs
/// across all three callers.
emit::ExportEntry build_persistent_level_chain() {
    emit::ExportEntry outermost = emit::ExportEntry::asset(
        12923414834320654503ULL,
        "/Game/Levels/Verra_World_Master/Verra_World_Master",
        0x00000000, /*no_load=*/true);
    emit::ExportEntry mid = emit::ExportEntry::asset(
        16604466839667550161ULL, "Verra_World_Master",
        0x00000000, /*no_load=*/true);
    mid.outer = std::make_unique<emit::ExportEntry>(std::move(outermost));
    emit::ExportEntry leaf = emit::ExportEntry::asset(
        16442478405498561049ULL, "PersistentLevel",
        0x00000000, /*no_load=*/true);
    leaf.outer = std::make_unique<emit::ExportEntry>(std::move(mid));
    return leaf;
}

}  // namespace

PlayerStateEmitter::PlayerStateEmitter(IGameServerHost& host,
                                         const std::string& client_key)
    : host_(host), client_key_(client_key) {}

bool PlayerStateEmitter::emit_open(const sockaddr_in& client_addr) {
    if (!is_enabled()) {
        spdlog::info("[PlayerStateEmitter] disabled "
                     "(probe_player_state_emit.txt absent or \"0\") — "
                     "Phase D Step 3 staging");
        return true;  // no-op success
    }

    spdlog::warn("[PlayerStateEmitter] PD3 Iter2 — building PlayerState "
                  "ActorOpen bunch");

    // ── Schema ─────────────────────────────────────────────────────────
    schema::SchemaRegistry::instance().load_all();
    const auto* ps_schema = schema::SchemaRegistry::instance()
        .get_schema(schema::ActorType::PlayerState);
    if (!ps_schema) {
        spdlog::error("[PlayerStateEmitter] PlayerState schema not loaded");
        return false;
    }

    // ── NetGUID block ──────────────────────────────────────────────────
    auto block = host_.allocate_player_block(client_key_);
    if (!block.player_state) {
        spdlog::error("[PlayerStateEmitter] no PS NetGUID in block");
        return false;
    }
    const uint64_t ps_obj = block.player_state;

    // ── Archetype NetGUID ─────────────────────────────────────────────
    //
    // The class CDO for AAoCPlayerStateBP_C.  Mirrors how PcEmitter uses
    // 3503756484819958835 for PC and PlayerPawnEmitter uses
    // 9327572450991073355 for Pawn — content-addressable, deterministic
    // hash of the asset path.  We pick a unique value (sibling of the PC
    // and Pawn class CDOs).
    static constexpr uint64_t kAoCPlayerStateBP_CDO   = 3503756484819958836ULL;  // PC + 1
    static constexpr uint64_t kAoCPlayerStateBP_Outer = 4074085207143396459ULL;  // Pawn outer + 1

    // ── Export entries ─────────────────────────────────────────────────
    std::vector<emit::ExportEntry> exports;

    // [0] Default__AoCPlayerStateBP_C → its outer BP package
    {
        auto e = build_two_level(
            /*leaf_obj=*/       kAoCPlayerStateBP_CDO,
            /*leaf_path=*/      "Default__AoCPlayerStateBP_C",
            /*leaf_checksum=*/  0x00000000,
            /*outer_obj=*/      kAoCPlayerStateBP_Outer,
            /*outer_path=*/     "/Game/ThirdPersonCPP/Blueprints/AoCPlayerStateBP",
            /*outer_checksum=*/ 0x00000000,
            /*no_load=*/        false);
        exports.push_back(std::move(e));
    }

    // [1] PersistentLevel (3-level chain — identical to PC/Pawn)
    exports.push_back(build_persistent_level_chain());

    // ── Runtime: minimal PlayerState (no transform) ────────────────────
    emit::ActorRuntime rt;
    rt.type                 = schema::ActorType::PlayerState;
    rt.actor_netguid        = ps_obj;
    rt.actor_server_id      = 60;
    rt.actor_randomizer     = rnd_for(ps_obj);
    rt.archetype_netguid    = kAoCPlayerStateBP_CDO;
    rt.archetype_server_id  = 0;
    rt.archetype_randomizer = 0;
    rt.level_netguid        = 16442478405498561049ULL;
    rt.level_server_id      = 0;
    rt.level_randomizer     = 0;

    // PlayerState is a non-spawned actor — no transform data on the wire.
    rt.serialize_location = false;
    rt.serialize_rotation = false;
    rt.serialize_scale    = false;
    rt.serialize_velocity = false;

    spdlog::warn("[PlayerStateEmitter] PS NetGUID minted: ObjectId={} "
                  "ServerId={} Randomizer={} (block base=0x{:x})",
                  ps_obj, rt.actor_server_id, rt.actor_randomizer, block.base);

    // ── Bunch context ──────────────────────────────────────────────────
    emit::BunchWriter bw;
    emit::EmitContext ctx;
    ctx.channel              = 21;       // fresh channel (PC=3, Pawn=19, PS=21)
    // First reliable bunch on a fresh channel must equal InitInReliable + 1
    // = 953 + 1 = 954.  Same convention as PC/Pawn.
    ctx.ch_sequence          = 954;
    ctx.is_reliable          = true;
    ctx.is_partial           = false;
    ctx.partial_initial      = true;
    ctx.partial_final        = true;
    ctx.is_control           = true;     // channel-open control bunch
    ctx.b_open               = true;
    ctx.package_map_exports  = std::move(exports);
    ctx.ch_name_is_hardcoded = true;
    ctx.ch_name_ename_idx    = 102;      // NAME_Actor (matches PC/Pawn)
// C:Y:B and C.O.A

    // No captured tail splice — PlayerState opens with default property
    // values.  Property updates (PlayerName, CharacterArchetype, etc.)
    // come as SEPARATE bunches on ch=21 chSeq=955+ in iteration 3.
    ctx.spliced_tail_bits      = nullptr;
    ctx.spliced_tail_bit_count = 0;

    // ── Build the bunch ────────────────────────────────────────────────
    emit::ActorBuilder builder;
    const size_t bits = builder.build_spawn(*ps_schema, rt, ctx, bw);
    if (bits == 0) {
        spdlog::error("[PlayerStateEmitter] ActorBuilder::build_spawn returned 0 bits");
        return false;
    }
    spdlog::info("[PlayerStateEmitter] ActorBuilder produced {} bits ({} bytes)",
                 bits, (bits + 7) / 8);

    // ── Send ───────────────────────────────────────────────────────────
    const bool ok = host_.send_bunch_packet(client_key_, client_addr,
                                              bw.data(), bits);
    if (!ok) {
        spdlog::error("[PlayerStateEmitter] send_bunch_packet failed");
        return false;
    }
    spdlog::warn("[PlayerStateEmitter] ★ PlayerState ActorOpen sent "
                  "(ch=21, NetGUID {}, archetype=AoCPlayerStateBP_C)",
                  ps_obj);
    return true;
}


// ── Iteration 3: PlayerName property update ─────────────────────────────

bool PlayerStateEmitter::emit_player_name(const sockaddr_in& client_addr) {
    if (!is_enabled()) {
        return true;  // no-op when probe disabled
    }

    const std::string name = host_.current_character_name();
    if (name.empty()) {
        spdlog::info("[PlayerStateEmitter] no character name available — "
                     "skipping PlayerName update");
        return true;
    }

    // ── Probe knobs (file-driven so we can iterate handle/MAX without rebuild) ──
    auto read_uint = [](const char* path, uint32_t default_val) -> uint32_t {
        std::FILE* fp = std::fopen(path, "r");
        if (!fp) return default_val;
        uint32_t v = default_val;
        std::fscanf(fp, "%u", &v);
        std::fclose(fp);
        return v;
    };

    // Stock UE5 APlayerState has 10 replicated props; PlayerNamePrivate is
    // RepIdx 9.  AAoCPlayerState may add more (currently unknown) — when
    // it does, total goes up and PlayerNamePrivate's effective handle
    // stays at 9 (parent props come first in RepLayout).
    //
    // Wire format confirmed via UCharacterAppearanceComponent test 7:
    //   AOC uses 0-indexed handles, MAX = NumProps (no +1, no end-marker).
    //   serialize_int(handle, MAX) writes ceil(log2(MAX)) bits.
    //
    // probe_ps_max.txt        — total replicated prop count (default 10)
    // probe_ps_handle_name.txt — RepIdx for PlayerNamePrivate (default 9)
    const uint32_t ps_max         = read_uint("probe_ps_max.txt", 10u);
    const uint32_t handle_name    = read_uint("probe_ps_handle_name.txt", 9u);
    const uint32_t pawn_chseq     = read_uint("probe_ps_chseq.txt", 955u);

    // ── Build the PlayerNamePrivate property update payload ──
    aoc::protocol::emit::BunchWriter inner;

    // Handle bits via SerializeInt(handle, MAX)
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

    // FString wire format:
    //   int32 length  (positive = ANSI, count includes null terminator)
    //   chars + null
    aoc::protocol::emit::BunchWriter value_bw(64);
    const int32_t fstring_len = static_cast<int32_t>(name.size() + 1);  // +1 for null
    value_bw.write_bits(static_cast<uint32_t>(fstring_len), 32);
    for (char c : name) {
        value_bw.write_bits(static_cast<uint8_t>(c), 8);
    }
    value_bw.write_bits(0u, 8);  // null terminator
    const uint32_t value_bits = static_cast<uint32_t>(value_bw.bit_pos());

    // Property-update inner format (per AOC's confirmed wire):
    //   [SerializeInt(handle, MAX)]      — variable bits
    //   [SIP NumPayloadBits]             — varint
    //   [<value_bits> bits of value]     — FString here
    serialize_int_to(inner, handle_name, ps_max);
    inner.write_sip(value_bits);
    inner.write_bit_range(value_bw.data(), 0, value_bits);

    const uint32_t inner_bits = static_cast<uint32_t>(inner.bit_pos());
// C-Y-B : C O A

    // ── Wrap in a V3 stably-named-actor content block ──
    //
    // For a property update on the channel's main actor (ch=21 = our
    // PlayerState), we use bIsActor=1 (no sub_guid needed — the channel
    // itself targets the actor).
    aoc::protocol::emit::BunchWriter wrapped(128);
    wrapped.write_bit(1);                    // bHasRepLayout = 1
    wrapped.write_bit(1);                    // bIsActor = 1 (main actor)
    wrapped.write_sip(inner_bits);           // SIP NumPayloadBits
    wrapped.write_bit_range(inner.data(), 0, inner_bits);

    // Trailing actor-root content block to terminate the bunch cleanly
    // (per PM120 finding for the appearance bunch).
    wrapped.write_bit(0);                    // bHasRepLayout = 0
    wrapped.write_bit(1);                    // bIsActor = 1
    wrapped.write_sip(0);                    // SIP NumPayloadBits = 0

    const uint32_t wrapped_bits = static_cast<uint32_t>(wrapped.bit_pos());

    spdlog::warn("[PlayerStateEmitter] PD3 Iter3 — emitting PlayerName='{}' "
                  "(handle={} max={} value_bits={} wrapped_bits={})",
                  name, handle_name, ps_max, value_bits, wrapped_bits);

    // ── Build the bunch ──
    aoc::protocol::emit::PropertyUpdateBunchBuilder b;
    b.set_channel(21);              // PlayerState channel (matches emit_open)
    b.set_ch_sequence(pawn_chseq);  // chSeq=955 by default (after open=954)
    b.set_reliable(true);
    b.set_ch_name_hardcoded(102);   // NAME_Actor (matches PS open)
    b.set_use_modern_inner_format(true);
    // Empirical from captured: actor-root property updates DON'T set
    // is_rep_paused (only subobject-targeting ones do).
    b.set_is_rep_paused(false);
    b.set_has_mbg(true);

    b.add_raw_payload(wrapped.data(), wrapped_bits);

    aoc::protocol::emit::BunchWriter bw(256);
    const size_t bunch_bits = b.build(bw);
    const bool ok = host_.send_bunch_packet(client_key_, client_addr,
                                              bw.data(), bunch_bits);
    spdlog::warn("[PlayerStateEmitter] PD3 PlayerName bunch sent: "
                  "total_bits={} wrapped_inner_bits={} ok={}",
                  bunch_bits, wrapped_bits, ok);
    return ok;
}

}}  // namespace aoc::net
