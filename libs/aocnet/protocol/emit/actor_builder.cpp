// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/emit/actor_builder.cpp
//
//  Implementation of the schema-driven bunch builder.  See header for
//  the overall output layout.
//
//  NOTE on byte-identity: this builder produces well-formed UE5 bunches
//  that parse back cleanly via our wire/packet_parser.  Achieving BYTE-
//  identity with a CAPTURED packet 22 (the original RandomChar spawn)
//  additionally requires the schema's handle numbers to match AoC's
//  actual wire handles — which is a Session C.1 calibration task.  For
//  now we round-trip our own output and use our own handle numbering.
// ============================================================================
#include "protocol/emit/actor_builder.h"
#include "protocol/emit/intrepid_netguid.h"
#include "protocol/emit/package_map_exporter.h"
#include <algorithm>
#include <cstdio>   // PM112: probe_v3_subobjects.txt parsing (FILE/fscanf)
#include <cstring>
#include <spdlog/spdlog.h>

namespace aoc { namespace protocol { namespace emit {

namespace {

/// Write the bunch header per AoC S>C wire format (matches
/// pc_spawn_parser.cpp::parse_one_bunch_header exactly).
/// See docs/world-bootstrap-findings.md §7 for the 44-bit pkt#22 header
/// layout this must reproduce.
void write_bunch_header(BunchWriter& out, const EmitContext& ctx,
                         const schema::ActorSchema& schema,
                         uint32_t bunch_data_bits) {
    // bControl — 0 for plain data bunches, 1 for control (channel-open
    // bunches carry bControl=1 even though they convey actor data).
    out.write_bit(ctx.is_control ? 1 : 0);
    if (ctx.is_control) {
        out.write_bit(ctx.b_open  ? 1 : 0);
        out.write_bit(ctx.b_close ? 1 : 0);
        // CloseReason SerializeInt(max=7) = 3 bits.  Only written when
        // bClose=1.  For channel-open bunches we don't reach this.
        if (ctx.b_close) {
            out.write_serialize_int(0u, 7);  // EChannelCloseReason::None=0
        }
    }

    // bIsReplicationPaused (always written per UE5)
    out.write_bit(0);

    // bReliable
    out.write_bit(ctx.is_reliable ? 1 : 0);

    // ChIndex via SerializeIntPacked
    out.write_sip(ctx.channel);

    // bHasPackageMapExports — either explicit from ctx (splice_full_payload
    // mode sets it directly) or derived from whether we have exports to
    // serialize (ordinary path).
    const bool has_pme = ctx.explicit_has_pme
                             ? ctx.has_pme_value
                             : !ctx.package_map_exports.empty();
    out.write_bit(has_pme ? 1 : 0);
    // bHasMustBeMappedGUIDs
    out.write_bit(ctx.has_mbg_value ? 1 : 0);
    out.write_bit(ctx.is_partial ? 1 : 0);
// [Cyb :: C O A]

    // ── PM50 (2026-04-30) — ChSeq 12→10 bit fix ──────────────────────────
    //
    // Bug found by diff_pc_bunch.py: client reads ChSeq as 10 bits via
    // SerializeInt(MAX=1024), confirmed by PM14 RE of sub_144230D50 line
    // 1441.  We were writing 12 bits → 2 extra bits drift the client's
    // bit cursor → ChName.bIsHardcoded reads from a wrong position → the
    // 32-bit SaveNum read returns garbage → CNSF cascade.
    //
    // PM20 (2026-04-28) fixed this in send_client_restart_native and
    // scan_outgoing_packet_chseq + CALV STUB, but missed write_bunch_header
    // here.  This is the root cause of "empty world with floating rocks"
    // for every native session since PM35: PC ActorOpen has been silently
    // CNSF'ing on the client → no actor channels ever opened → world
    // streams via LoadMap but no PC, no Pawn, nothing.
    //
    // Old comment about "ch=3 ChSeq=1978 requires 12 bits" was wrong —
    // 1978 wraps to 1978 & 0x3FF = 954 in 10 bits, which is what client
    // sees.  10-bit ChSeq always works.
    if (ctx.is_reliable) {
        out.write_serialize_int(ctx.ch_sequence, 1024);
    }

    // Partial sub-flags (only if partial) — 3 bits AoC extension:
    //   bPartialInitial + bPartialCustomExportsFinal + bPartialFinal
    // All three must be set from ctx to round-trip captured bunches.
    if (ctx.is_partial) {
        out.write_bit(ctx.partial_initial              ? 1 : 0);
        out.write_bit(ctx.partial_custom_exports_final ? 1 : 0);
        out.write_bit(ctx.partial_final                ? 1 : 0);
    }

    // ChName is present only on the INITIAL fragment of a logical bunch,
    // or on non-partial reliable bunches.  Continuation fragments
    // inherit their ChName from the initial fragment.  Match
    // pc_spawn_parser.cpp:parse_one_bunch_header exactly.
    const bool chname_present =
        ctx.is_reliable && (!ctx.is_partial || ctx.partial_initial);
    if (chname_present) {
        out.write_bit(ctx.ch_name_is_hardcoded ? 1 : 0);
        if (ctx.ch_name_is_hardcoded) {
            out.write_sip(ctx.ch_name_ename_idx);
        } else {
            out.write_fstring_ansi(ctx.ch_name_string);
            out.write_int32(ctx.ch_name_number);
        }
        (void)schema;  // schema unused here (name comes from channel type)
    }

    // BunchDataBits via SerializeInt(MAX_PKT_BITS = 1024*8)
    out.write_serialize_int(bunch_data_bits, 1024 * 8);
}

/// Write a single property-handle stream entry: [handle SIP][property bits].
/// Handles are emitted as SerializeIntPacked.  Handle 0 is the terminator.
void emit_handle_stream_entry(uint32_t handle,
                               const schema::PropertySchema& prop,
                               const SchemaValue& value,
                               BunchWriter& out) {
    // In UE5 backwards-compatible format, stream is:
    //   [handle SIP] [NumBits SIP] [NumBits bits of data]
    // We use the compat format since AoC uses it for replay-friendly NetDriver.
    out.write_sip(handle);

    // Render the property value into a temporary writer so we can compute
    // its bit length for the SIP-prefixed NumBits field.
    BunchWriter tmp(64);
    ActorBuilder::emit_property(prop, value, tmp);
    uint32_t num_bits = static_cast<uint32_t>(tmp.bit_pos());
    out.write_sip(num_bits);
    // Copy the rendered bits into `out`.
    out.write_bit_range(tmp.data(), 0, num_bits);
}

/// Write a V3 subobject CREATION content block — registers a NEW dynamic
/// subobject on the channel's actor, with its class identified via NetGUID.
///
/// PM111 path: bStablyNamed=0 + SIP class_guid.  AOC's parser does NOT
/// like this format (PM111 test failed silently → no possession ack →
/// loading screen loop).  Kept here as opt-in for diagnostic comparison.
/// PM141 — V3 dynamic-creation form, derived from LIVE production pcap.
/// Source: aoc_ranger_respawn pkt#122 ch=11288 Block 9, captured pre-shutdown.
/// Adds bIsDestroyMessage and bActorIsOuter bits that the parallel emu's
/// parser defaulted off but the actual wire DOES include.
void write_v3_subobject_creation_dynamic(BunchWriter& out,
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
    out.write_bit(1);                         // PM141: bActorIsOuter = 1
    out.write_sip(num_payload_bits);          // SIP NumPayloadBits
    if (num_payload_bits > 0 && payload_bits != nullptr) {
        out.write_bit_range(payload_bits, 0, num_payload_bits);
    }
}
// C Y B with C.O.A

/// Write a V3 STABLY-NAMED subobject content block (PM118 — captured-format).
///
/// PM118 (2026-05-04) — corrected via PCAP analysis of captured ranger
/// respawn (`aoc_ranger_respawn_home_point_j_20260205_230233.pcap`).  Real
/// captured ActorOpen content block bit-level dump:
///
///   bit 127: bHasRepLayout = 1   (explicit-size form; NumPayloadBits follows)
///   bit 128: bIsActor      = 0
///   bits 129..144: sub_guid SIP64 = 0x3FFD → val=8190 dyn=1 (16 bits)
///   bit 145: bStablyNamed  = 1   (skip class_guid)
///   bits 146..153: NumPayloadBits SIP = 79 (8 bits)
///   bits 154..232: 79 bits payload (RepLayout: SIP handle then cmd payload)
///
/// PRIOR ATTEMPTS THAT FAILED:
/// - PM114: SIP sub_guid + wrong bit-0 (=bOutermostEnd=0 instead of
///          bHasRepLayout=1) → ContentBlockFail
/// - PM115: 128-bit FIntrepidNetGUID for sub_guid → ContentBlockHeaderObjFail
///   (my prior RE agent was wrong; real captured format uses SIP64)
///
/// CORRECT WIRE FORMAT (PM131-rev2 — 2026-05-07):
///   [1 bit  bOutermostEnd  = 0]    ← MUST be 0; 1 means "no more content blocks"
///   [1 bit  bIsChannelActor = 0]   ← subobject (1 = the channel's actor)
///   [SIP64  sub_guid]              ← variable-length packed int
///   [1 bit  bStablyNamed   = 1]    ← REQUIRED; 1 = lookup by name (no class_guid)
///                                  ← if 0, class_guid SIP follows for dynamic instantiation
///                                    (see write_v3_subobject_creation_dynamic)
///   [SIP    NumPayloadBits]
///   [<NumPayloadBits> bits payload]
///
/// PM131-rev1 BUG (2026-05-07): I removed the bStablyNamed bit entirely thinking
/// the appearance_emitter.cpp PM120 format was authoritative — but the receiver
/// always reads bStablyNamed after sub_guid.  Without our explicit "1" the next
/// bit (LSB of SIP NumPayloadBits=0 = literal 0) was read as bStablyNamed=0,
/// making AOC expect a class_guid SIP next → read garbage as class_guid → fail
/// with "ReadContentBlockPayload FAILED" → close reason ContentBlockFail.
/// Empirically confirmed in test 2026-05-07 13:17:31.064 client error log.
///
/// PM131-rev2 fix: keep all 5 fields; only the original bug was the FIRST bit.
/// Old broken: bit 1 (read as bOutermostEnd=1, wrong → exit loop)
/// New fixed:  bit 0 (read as bOutermostEnd=0, correct → continue parse)
///
/// Stably-named resolution: when sub_guid was registered via path (PM117
/// stably-named export), the client looks up the subobject by that name
/// on the channel's actor (the BP CDO has it as a subobject).  No PackageMap
/// pre-reg of an instantiated UObject required; the channel actor's
/// instantiation auto-creates the subobjects.
void write_v3_subobject_stably_named(BunchWriter& out,
                                       uint64_t sub_guid,
                                       const uint8_t* payload_bits,
                                       uint32_t num_payload_bits) {
    // PM145 (2026-05-07) — restore PM118's original RE'd format.
    //
    // PM131-rev2 broke this by changing the first bit from 1 to 0, thinking
    // it was a "bOutermostEnd" sentinel.  PM118's bit-level decoded capture
    // (see comment block in this file at line 397+) showed:
    //
    //   bit 127: bHasRepLayout = 1   (explicit-size form; NumPayloadBits follows)
    //   bit 128: bIsActor      = 0
    //   ...
    //
    // Empirical confirmation 2026-05-07 22:41 test:
    //   With first bit = 0: client logs `ReadContentBlockHeader FAILED.
    //   Bunch.IsError() == TRUE. Closing connection.` (line 3866 of test).
    //   Channel 19 then closes the entire connection → Realm timeout.
    //
    // The first bit IS bHasRepLayout (per PM118 RE) and MUST be 1 for
    // the explicit-size form (where NumPayloadBits SIP follows after the
    // header bits).  bHasRepLayout=0 would mean "no NumPayloadBits, payload
    // extends to end of bunch" — which is fine when this is the LAST block
    // but fatal when followed by another content block (our PM144 trailing
    // terminator) because the payload would gobble the terminator's bits.
    out.write_bit(1);                         // PM145: bHasRepLayout = 1 (explicit size)
    out.write_bit(0);                         // bIsChannelActor = 0 (subobject)
    out.write_sip(sub_guid);                  // SIP SubobjectNetGUID
    out.write_bit(1);                         // bStablyNamed = 1 (skip class_guid)
    out.write_sip(num_payload_bits);          // SIP NumPayloadBits
    if (num_payload_bits > 0 && payload_bits != nullptr) {
        out.write_bit_range(payload_bits, 0, num_payload_bits);
    }
}

/// Write the content block for the actor root or one subobject.
///
/// H.3f AoC-specific format (from sub_14504F1A0 + sub_145057C30 RE):
///
///   For each property with a non-default value:
///     [uint32 cmd_index]            — 32 bits LSB-first, bit-contiguous
///     [per-property data]           — via emit_property (type-dispatched)
///
/// Differences from stock UE5:
///   - NO `bHasRepLayout` / `bIsActor` header bits
///   - NO SIP-encoded payload_bits prefix
///   - NO handle=0 terminator (bunch BDB defines the end)
///   - Each property is prefixed by a full 32-bit cmd_index, not a SIP handle
///   - Function J rollback: if a property's data writes zero bits (e.g. all
///     default sub-elements), both the cmd_index AND data are rolled back.
///
/// NOTE on the legacy `is_actor` + `subobject_netguid` args: captured
/// pkt#22 has a SINGLE contiguous property stream for the actor — no
/// separate subobject headers.  Subobject components likely ride on the
/// actor's root property stream via the RepLayout tree (with their cmds
/// appearing at predefined indices).  Kept the args for API compat.
void write_content_block(BunchWriter& out, bool is_actor,
                          uint64_t subobject_netguid,
                          const std::vector<schema::PropertySchema>& props,
                          const std::unordered_map<uint32_t, SchemaValue>& values) {
    (void)is_actor;
    (void)subobject_netguid;

    for (const auto& prop : props) {
        auto it = values.find(prop.handle);
        if (it == values.end()) continue;  // skip unset (would rollback anyway)

        // Function J rollback semantic: write cmd_index + data into a
        // temp writer first.  If the data write advanced the archive,
        // commit the whole thing (cmd_index + data) to `out`.  Otherwise
        // drop it entirely.  Our `emit_property` is deterministic per
        // schema type — so "advanced" is just "wrote at least 1 bit".
        BunchWriter prop_data(64);
        ActorBuilder::emit_property(prop, it->second, prop_data);
        if (prop_data.bit_pos() == 0) continue;  // nothing written → rollback

        out.write_uint32(prop.handle);  // cmd_index (32 bits LSB-first)
        out.write_bit_range(prop_data.data(), 0, prop_data.bit_pos());
    }

    // No terminator: the bunch's BunchDataBits (in the bunch header) tells
    // the receiver how many bits belong to this bunch's content.  When the
    // bit cursor reaches BDB, Function G's outer loop terminates.
}

} // anonymous namespace

void write_quantized_vector(BunchWriter& out, int64_t x, int64_t y, int64_t z) {
    // GetBitsNeeded: the two's-complement width of the widest component.
    auto bits_needed = [](int64_t v) -> uint32_t {
        uint64_t m = static_cast<uint64_t>(v ^ (v >> 63));
        uint32_t n = 1;
        while (m) { m >>= 1; ++n; }
        return n;
    };
    const uint32_t bits = std::max(bits_needed(x), std::max(bits_needed(y), bits_needed(z)));
    out.write_serialize_int((1u << 6) | bits, 1u << 7);   // scaled flag + component width
    const uint64_t mask = (bits >= 64) ? ~0ull : ((1ull << bits) - 1);
    out.write_bits(static_cast<uint64_t>(x) & mask, static_cast<int>(bits));
    out.write_bits(static_cast<uint64_t>(y) & mask, static_cast<int>(bits));
    out.write_bits(static_cast<uint64_t>(z) & mask, static_cast<int>(bits));
}

void write_player_controller_open_fields(BunchWriter& out, uint8_t net_player_index,
                                         uint32_t client_handshake_id) {
    out.write_bits(net_player_index, 8);
    out.write_bits(client_handshake_id, 32);
}
// (C | O | A + Cyb)

/// Write SerializeNewActor for an actor spawn.
///
/// H.3c correction (2026-04-22): decoding the captured pkt #22 PC spawn
/// via phase1_parser.decode_new_actor shows the bare SerializeNewActor
/// payload is exactly 28 bits for a dynamic actor with no transform:
///   - Actor NetGUID (SIP)           ≤ 8 bits
///   - Archetype NetGUID (SIP)       ≤ 8 bits
///   - Level NetGUID (SIP)           ≤ 8 bits
///   - bSerializeLocation / Rotation / Scale / Velocity = 4 × 1 bit
///
/// Previous implementation erroneously wrote a flag byte after each
/// NetGUID (0x00 / 0x72 / 0xd6) — those bytes only appear in EXPORT
/// mode when a NetGUID's path is inlined.  For bare references (where
/// the client already has the NetGUID cached), no flag byte is written.
/// Removing those saves 24 bits and gets us closer to captured output.
///
/// NOTE: if the client DOESN'T have a NetGUID cached, UE5 falls back to
/// inline-export mode which writes a larger payload (bool + FString path
/// per exported NetGUID).  That larger form is what the captured pkt #22
/// actually uses for its first-time-seen PC, archetype, and level refs.
/// Our emitter currently assumes bare-reference mode only — sufficient
/// for dry-run byte-identity on re-spawns, and sufficient for active-send
/// when replay has already populated the client's NetGUID cache.
size_t write_serialize_new_actor(BunchWriter& out, const ActorRuntime& runtime,
                                  const schema::ActorSchema& schema) {
    size_t start = out.bit_pos();

    // H.3d CORRECTION: AoC replaced stock UE5's 32-bit FNetworkGUID with a
    // 128-bit FIntrepidNetworkGUID struct (ObjectId u64 + ServerId u32 +
    // Randomizer u32).  Confirmed by RE of sub_14141E960 in the shipping
    // client, which reads NetGUIDs as 4 consecutive uint32s.
    //
    // The three GUIDs below MUST match the exports the client has cached —
    // either from a prior export bunch or from the inline export section
    // present at the head of this bunch (when ctx.package_map_exports is
    // non-empty).  Captured pkt#22 uses ServerId=60 only for the Actor
    // itself (a dynamic server-assigned NetGUID); the Archetype and Level
    // use ServerId=0 because they're inline-exported in this same bunch.

    // Actor NetGUID — 128 bits
    FIntrepidNetworkGUID actor{
        runtime.actor_netguid, runtime.actor_server_id, runtime.actor_randomizer};
    write_intrepid_guid(out, actor);

    // Archetype NetGUID — 128 bits.  Prefer runtime.archetype_* if non-zero,
    // otherwise fall back to the schema's class-level default.
    FIntrepidNetworkGUID archetype{
        runtime.archetype_netguid ? runtime.archetype_netguid : schema.archetype_netguid,
        runtime.archetype_server_id,
        runtime.archetype_randomizer};
    write_intrepid_guid(out, archetype);

    // Level NetGUID — 128 bits
    FIntrepidNetworkGUID level{
        runtime.level_netguid ? runtime.level_netguid : schema.level_netguid,
        runtime.level_server_id,
        runtime.level_randomizer};
    write_intrepid_guid(out, level);

    // bSerializeLocation
    const bool has_loc = runtime.serialize_location ||
                          runtime.spawn_location.x != 0.0f ||
                          runtime.spawn_location.y != 0.0f ||
                          runtime.spawn_location.z != 0.0f ||
                          runtime.quantize_location;
    out.write_bit(has_loc ? 1 : 0);
    if (has_loc) {
        // bQuantizeLocation — captured pkt#22 uses the quantized path:
        // UE::Net::WriteQuantizedVector (the client reads it when its net
        // version is above 22).  Otherwise 3 doubles.
        out.write_bit(runtime.quantize_location ? 1 : 0);
        if (runtime.quantize_location) {
            write_quantized_vector(out,
                                   runtime.location_scaled_x,
                                   runtime.location_scaled_y,
                                   runtime.location_scaled_z);
        } else {
            const double dx = runtime.spawn_location.x;
            const double dy = runtime.spawn_location.y;
            const double dz = runtime.spawn_location.z;
            uint64_t u;
            std::memcpy(&u, &dx, 8); out.write_uint64(u);
            std::memcpy(&u, &dy, 8); out.write_uint64(u);
            std::memcpy(&u, &dz, 8); out.write_uint64(u);
        }
    }

    // bSerializeRotation, then FRotator::NetSerialize (SerializeCompressedShort):
    // per axis a bit saying the value is non-zero, then its 16 bits.  (Validated
    // on all 743 captured actor opens by tools/decode_actor_open.py.)  Callers
    // pass pitch/yaw/roll already compressed: degrees * 65536 / 360.
    auto write_compressed_short = [&](int16_t v) {
        out.write_bit(v != 0 ? 1 : 0);
        if (v != 0) out.write_uint16(static_cast<uint16_t>(v));
    };
    const bool has_rot = runtime.serialize_rotation ||
                          runtime.spawn_rotation.pitch != 0 ||
                          runtime.spawn_rotation.yaw   != 0 ||
                          runtime.spawn_rotation.roll  != 0;
    out.write_bit(has_rot ? 1 : 0);
    if (has_rot) {
        write_compressed_short(runtime.spawn_rotation.pitch);
        write_compressed_short(runtime.spawn_rotation.yaw);
        write_compressed_short(runtime.spawn_rotation.roll);
    }
// Cyb.C O A

    // bSerializeScale — flag bit; if true, writes vector data (we don't
    // support setting non-default scale yet).  Setting to true without
    // writing the data caused PM100's bug.
    out.write_bit(runtime.serialize_scale ? 1 : 0);
    if (runtime.serialize_scale) {
        // Stock UE5 uses FVector_NetQuantize10 for scale (same as location).
        // Default scale is (1, 1, 1).  Encode as packed vector with 24-bit/axis.
        // Caller must populate runtime.scale_scaled_x/y/z (not yet wired);
        // for now, write three packed-vector zeros so we don't drift.
        write_quantized_vector(out, 0, 0, 0);
    }

    // bSerializeVelocity — flag bit + optional packed vector.
    // PM100's bug: setting flag=1 without writing vector data caused 75+
    // bit drift on the wire.  Now writes proper packed vector when enabled.
    out.write_bit(runtime.serialize_velocity ? 1 : 0);
    if (runtime.serialize_velocity) {
        // Default zero velocity (Pawn at rest).  When movement replication
        // is implemented, runtime.velocity_scaled_xyz will be populated.
        write_quantized_vector(out, 0, 0, 0);
    }

    return out.bit_pos() - start;
}


// ── Property-type emitters ──────────────────────────────────────────────────

void ActorBuilder::emit_property(const schema::PropertySchema& prop,
                                  const SchemaValue& value,
                                  BunchWriter& out) {
    using schema::PropType;
    switch (prop.type) {
    case PropType::Bool:
        out.write_bit(value.b ? 1 : 0);
        break;
    case PropType::UInt8:
        out.write_uint8(static_cast<uint8_t>(value.u));
        break;
    case PropType::UInt16:
        out.write_uint16(static_cast<uint16_t>(value.u));
        break;
    case PropType::UInt32:
        out.write_uint32(static_cast<uint32_t>(value.u));
        break;
    case PropType::UInt64:
        out.write_uint64(value.u);
        break;
    case PropType::Int8:
    case PropType::Int16:
    case PropType::Int32:
        out.write_int32(static_cast<int32_t>(value.i));
        break;
    case PropType::Int64:
        out.write_uint64(static_cast<uint64_t>(value.i));
        break;
    case PropType::Float: {
        float f = static_cast<float>(value.f);
        uint32_t u;
        std::memcpy(&u, &f, 4);
        out.write_uint32(u);
        break;
    }
    case PropType::Double: {
        double d = value.f;
        uint64_t u;
        std::memcpy(&u, &d, 8);
        out.write_uint64(u);
        break;
    }
    case PropType::FString:
        out.write_fstring_ansi(value.str);
        break;
    case PropType::FName:
        out.write_sip(value.u);  // FName = SerializeIntPacked index
        break;
    case PropType::FVector: {
        // Simple float-triple for Session C; quantized form is later work.
        uint32_t u;
        std::memcpy(&u, &value.vec.x, 4); out.write_uint32(u);
        std::memcpy(&u, &value.vec.y, 4); out.write_uint32(u);
        std::memcpy(&u, &value.vec.z, 4); out.write_uint32(u);
        break;
    }
    case PropType::FRotator: {
        // bSerialize=1 + per-axis (1 flag + optional 16-bit short)
        out.write_bit(1);
        out.write_bit(value.rot.pitch != 0 ? 1 : 0);
        if (value.rot.pitch != 0) out.write_uint16(value.rot.pitch);
        out.write_bit(value.rot.yaw != 0 ? 1 : 0);
        if (value.rot.yaw != 0) out.write_uint16(value.rot.yaw);
        out.write_bit(value.rot.roll != 0 ? 1 : 0);
        if (value.rot.roll != 0) out.write_uint16(value.rot.roll);
        break;
    }
    case PropType::FQuat: {
        // Quaternion compression: 3 × 16-bit + sign bit.  MVP: emit raw 4 floats.
        uint32_t u;
        float q[4] = {0.0f, 0.0f, 0.0f, 1.0f};  // identity if not stored
        for (int i = 0; i < 4; ++i) {
            std::memcpy(&u, &q[i], 4);
            out.write_uint32(u);
        }
        (void)value;
        break;
    }
    case PropType::NetGUID:
        out.write_sip(value.netguid);
        break;
    case PropType::ByteArray:
        // H.3g: dynamic-array wire format per Function C (sub_145035420):
        //   [uint16 count]  (LSB-first, bit-contiguous — max 65534)
        //   [count × uint8 bytes]
        //
        // Our previous int32-length form was wrong for UE5 UArrayProperty —
        // real TArrays always prefix with uint16 count.  The int32 form was
        // a placeholder before Function C was decoded.
        if (value.bytes.size() >= 0xFFFFu) {
            spdlog::warn("[ActorBuilder] ByteArray too large ({} bytes) for "
                          "UE5 uint16 length prefix", value.bytes.size());
        }
        out.write_uint16(static_cast<uint16_t>(value.bytes.size()));
        for (uint8_t b : value.bytes) out.write_uint8(b);
        break;
    case PropType::CustomDelta:
        // CustomDelta / FastArraySerializer uses its own format that's
        // not a plain TArray.  UE5 writes an int32 BaseHistoryNum + int32
        // NumChanges + per-change content.  We preserve the legacy int32
        // length behavior here until a CustomDelta test fixture lands.
        out.write_int32(static_cast<int32_t>(value.bytes.size()));
        for (uint8_t b : value.bytes) out.write_uint8(b);
        break;
    }
}
// -- C-O-A . C:Y:B --

// ── Public builder methods ──────────────────────────────────────────────────

size_t ActorBuilder::build_spawn(const schema::ActorSchema& schema,
                                  const ActorRuntime& runtime,
                                  const EmitContext& ctx,
                                  BunchWriter& out) {
    size_t start = out.bit_pos();

    // Render the bunch payload into a temp writer (SerializeNewActor +
    // all content blocks), so we can compute BunchDataBits for the header.
    BunchWriter payload(2048);

    // Full-payload-splice mode — used when we don't yet understand the
    // payload format (e.g. bHasRepLayoutExport=1 compact field-mask) and
    // need to reproduce it verbatim for byte-identical round-trip or
    // first-cut Phase II send.  Skips exports + SNA + content blocks.
    if (ctx.splice_full_payload
        && ctx.spliced_tail_bits != nullptr
        && ctx.spliced_tail_bit_count > 0)
    {
        payload.write_bit_range(ctx.spliced_tail_bits, ctx.spliced_tail_start_bit,
                                ctx.spliced_tail_bit_count);
        spdlog::debug("[ActorBuilder] Using FULL-payload splice ({} bits) "
                      "— header emitted from parsed fields, rest spliced",
                      ctx.spliced_tail_bit_count);
        const uint32_t bunch_data_bits = static_cast<uint32_t>(payload.bit_pos());
        write_bunch_header(out, ctx, schema, bunch_data_bits);
        out.write_bit_range(payload.data(), 0, bunch_data_bits);
        const size_t total = out.bit_pos() - start;
        return total;
    }

    // 0. Session H.3d: inline NetGUID export section.  Present only when
    //    `ctx.package_map_exports` is non-empty (correspondingly bunch
    //    header flips bHasPackageMapExports=1).  Layout mirrors
    //    sub_1450360E0: [1-bit bHasRepLayoutExport=0][u32 num][entries...]
    if (!ctx.package_map_exports.empty()) {
        PackageMapExporter::write_export_section(payload, ctx.package_map_exports);
    }

    // 1. SerializeNewActor
    write_serialize_new_actor(payload, runtime, schema);
    if (runtime.type == schema::ActorType::PlayerController)
        write_player_controller_open_fields(payload, /*net_player_index=*/0,
                                            /*client_handshake_id=*/0);

    // H.4: splice mode — when spliced_tail_bits is provided, append those
    // bits verbatim (they represent the RepLayout property stream which we
    // can't fully regenerate from schema yet).  Skip our own content blocks.
    if (ctx.spliced_tail_bits != nullptr && ctx.spliced_tail_bit_count > 0) {
        payload.write_bit_range(ctx.spliced_tail_bits, ctx.spliced_tail_start_bit,
                                ctx.spliced_tail_bit_count);
        spdlog::debug("[ActorBuilder] Using spliced RepLayout tail "
                      "({} bits) instead of content-block emission",
                      ctx.spliced_tail_bit_count);
    } else {
        // 2. Root content block (AoC per-property [cmd_index][data] stream)
        write_content_block(payload, /*is_actor=*/true, /*subobj=*/0,
                             schema.root_properties, runtime.root_values);

        // 3. Per-component content blocks
        //
        // Modes, gated by `probe_v3_subobjects.txt`:
        //
        //   "0" or absent → legacy per-property stream only (PM107 baseline,
        //                   possession works, no body)
        //   "1"           → PM111 dynamic-class form (bStablyNamed=0 + class_guid
        //                   as SIP) — known broken: ContentBlockFail
        //   "2"           → PM114 stably-named form with SIP sub_guid — also
        //                   broken with ContentBlockFail (wrong sub_guid type)
        //   "3"           → PM115 stably-named form with 128b FIntrepidNetGUID
        //                   sub_guid + correct field order (NumPayloadBits
        //                   before bStablyNamed).  Per RE of sub_143F2C340.
        int v3_mode = 0;
        if (std::FILE* fp = std::fopen("probe_v3_subobjects.txt", "r")) {
            std::fscanf(fp, "%d", &v3_mode);
            std::fclose(fp);
        }

        // Helper to derive a per-subobject FIntrepidNetGUID from the actor's.
        // Pattern: ObjectId = actor.ObjectId + (ci + 1), ServerId same as
        // actor, Randomizer = derived hash so the value is stable.
        auto rnd_for = [](uint64_t obj) -> uint32_t {
            uint64_t h = obj * 0x9E3779B97F4A7C15ULL;
            h ^= (h >> 33);
            h *= 0xFF51AFD7ED558CCDULL;
            h ^= (h >> 33);
            return static_cast<uint32_t>(h);
        };
// C-O-A + C Y B

        for (size_t ci = 0; ci < schema.components.size(); ++ci) {
            const auto& comp = schema.components[ci];
            std::unordered_map<uint32_t, SchemaValue> comp_vals;
            for (const auto& prop : comp.properties) {
                uint64_t key = (static_cast<uint64_t>(ci) << 32) | prop.handle;
                auto it = runtime.component_values.find(key);
                if (it != runtime.component_values.end()) {
                    comp_vals[prop.handle] = it->second;
                }
            }
            uint64_t subobj_obj_id = runtime.actor_netguid + ci + 1;

            // Render the per-property payload first (empty in Phase 1)
            BunchWriter prop_payload(128);
            write_content_block(prop_payload, /*is_actor=*/false, subobj_obj_id,
                                 comp.properties, comp_vals);
            uint32_t payload_bits =
                static_cast<uint32_t>(prop_payload.bit_pos());

            // ── Phase D Step 2.1 (2026-05-05) — Appearance payload override ──
            //
            // When the caller (PlayerPawnEmitter) supplied a pre-serialized
            // FCharacterCustomizationSaveData payload AND we're emitting the
            // CharacterAppearanceComponent subobject (ci == appearance_subobject_index),
            // REPLACE the empty payload with the supplied bits.
            //
            // The bits encode a UE5 property-update stream targeting the two
            // replicated fields (CharacterCustomization, bForceHideHeldItems).
            // When the client receives this V3 content block:
            //   1. ReadContentBlockHeader resolves `subobj_obj_id` to the
            //      live UCharacterAppearanceComponent on the spawned Pawn
            //   2. ReadContentBlockPayload reads NumPayloadBits worth of bits
            //   3. FRepLayout::ReceiveProperties processes the property updates
            //   4. OnRep_CharacterCustomization fires → mesh assembles
            //
            // This is what makes the visible character body appear in-world
            // (replacing the floating "Player" nameplate from the empty path).
            if (ci == ctx.appearance_subobject_index
                && ctx.appearance_payload_bit_count > 0
                && ctx.appearance_payload_bits != nullptr) {
                prop_payload.reset();
                prop_payload.write_bit_range(ctx.appearance_payload_bits,
                                              0,
                                              ctx.appearance_payload_bit_count);
                payload_bits = ctx.appearance_payload_bit_count;
                spdlog::warn("[ActorBuilder] PD2.1 — CharacterAppearance subobject "
                              "(ci={}) using supplied payload: {} bits",
                              ci, payload_bits);
            }

            // PD2.1 (2026-05-05) — when this is the appearance subobject AND
            // we have a payload to deliver, ALWAYS wrap it in a V3 stably-
            // named content block (regardless of v3_mode).  Without the
            // V3 header the client interprets the bits as actor-root
            // properties → garbage parse → connection timeout.
            const bool is_appearance_with_payload =
                (ci == ctx.appearance_subobject_index &&
                 ((ctx.appearance_payload_bit_count > 0 &&
                   ctx.appearance_payload_bits != nullptr)
                  || ctx.appearance_force_v3_wrap));

            if (v3_mode == 3 || is_appearance_with_payload) {
                // PM118 — captured-format wire: SIP64 sub_guid (NOT 128-bit
                // FIntrepidNetGUID), bHasRepLayout=1, bStablyNamed=1.
                // Per PCAP decode of captured ranger respawn ActorOpen.
                (void)rnd_for;
                write_v3_subobject_stably_named(payload, subobj_obj_id,
                                                  prop_payload.data(), payload_bits);
            } else if (v3_mode == 1 && comp.class_netguid != 0) {
                // PM135 — class GUID is full FIntrepidNetGUID; ServerId/Randomizer
                // are 0 since our class export uses the path-only form.
                write_v3_subobject_creation_dynamic(payload, subobj_obj_id,
                                                      comp.class_netguid,
                                                      /*server_id=*/0u,
                                                      /*randomizer=*/0u,
                                                      prop_payload.data(), payload_bits);
            } else {
                // Mode 0 (and unsupported modes 2): append legacy property
                // bits as-is (no V3 header)
                if (payload_bits > 0) {
                    payload.write_bit_range(prop_payload.data(), 0, payload_bits);
                }
            }
        }

        // PM120 (2026-05-04) — PROPER trailing actor-root content block.
        //
        // PM119 wrote only [bHasRepLayout=0][bIsActor=1] (2 bits) — but per
        // F5 decomp of UActorChannel::ReadContentBlockPayload (sub_7FF6BD25DA40),
        // the caller ALWAYS reads SIP NumPayloadBits via vtable+408 AFTER
        // ReadContentBlockHeader returns, regardless of the bHasRepLayout
        // value.  Our 2-bit-only PM119 trailing block ran the parser past
        // the end of the bunch on the NumPayloadBits read → bunch overflow →
        // IsError set → close reason 91 (ContentBlockPayloadBitsFail) but
        // chained-up to 90 (ContentBlockHeaderFail) before it reached us.
        //
        // PM120 fix: write [bHasRepLayout=0][bIsActor=1][SIP NumPayloadBits=0]
        // = 10 bits.  The parser reads our explicit-zero NumPayloadBits, the
        // payload-skip helper consumes 0 bits, the bunch ends exactly there,
        // the bunch-loop's "while bits_left > 0" check terminates cleanly.
        //
        // Without the trailing block the bunch ends right after the N
        // subobject content blocks — the loop tries one more ReadContent-
        // BlockHeader call, hits end-of-bunch on the bHasRepLayout bit
        // read → close reason 79 → propagated.  So the trailing block IS
        // required, just bigger than I thought.
        // PD2.1 (2026-05-05) — emit trailing block when ANY subobject
        // got V3-wrapped via the appearance override path.  Otherwise the
        // parser hits bunch overflow on the next ReadContentBlockHeader call.
        //
        // PM146 (2026-05-07) — REVERTED PM144's unconditional terminator.
        //
        // PM144 made the [bHasRepLayout=0][bIsActor=1][SIP NumPayloadBits=0]
        // trailing block unconditional, expecting it would suppress the
        // Bunch.IsError warning we observed after every from-scratch bunch.
        // It did NOT fix that warning.  Worse, between PM98/PM99 (when the
        // user CONFIRMED visible Verra terrain — "rivers, vegetation, node
        // structures") and the current black-screen regression, PM144 is
        // the ONLY structural change to every actor's wire shape.
        //
        // Every World Partition cell is GC'd right after loading screen
        // drops (line 6482+ NotifyStreamingLevelUnload spam).  Possible
        // mechanism: the unconditional terminator's bIsActor=1 + 0-bit
        // payload may be interpreted by AOC as "actor-root content block
        // with zero-bit RepLayout payload" — which under FObjectReplicator
        // semantics is "all properties at default" and could clobber
        // a property like StreamingSourceEnabled or AutoManageActiveCameraTarget
        // that a real bunch would never reset.  Reverting to PM118 logic.
        const bool need_trailing =
            (v3_mode == 3) ||
            ((ctx.appearance_payload_bit_count > 0 &&
              ctx.appearance_payload_bits != nullptr &&
              ctx.appearance_subobject_index < schema.components.size())
             ||
             (ctx.appearance_force_v3_wrap &&
              ctx.appearance_subobject_index < schema.components.size()));
// CoA.C Y B

        if (need_trailing && !schema.components.empty()) {
            payload.write_bit(0);              // bHasRepLayout = 0
            payload.write_bit(1);              // bIsActor = 1 (ACTOR ROOT)
            payload.write_sip(0);              // SIP NumPayloadBits = 0
            // No payload bytes after — NumPayloadBits=0 means zero-bit payload.
        }
    }

    uint32_t bunch_data_bits = static_cast<uint32_t>(payload.bit_pos());

    // 4. Bunch header (knows the bunch data size now)
    write_bunch_header(out, ctx, schema, bunch_data_bits);

    // 5. Append the payload bits
    out.write_bit_range(payload.data(), 0, bunch_data_bits);

    size_t total = out.bit_pos() - start;
    spdlog::debug("[ActorBuilder] build_spawn: class={} channel={} total={} bits "
                  "(header={}, payload={})", schema.class_name, ctx.channel,
                  total, total - bunch_data_bits, bunch_data_bits);
    return total;
}

size_t ActorBuilder::build_delta(const schema::ActorSchema& schema,
                                  const ActorRuntime& runtime,
                                  const std::vector<uint32_t>& changed_root_handles,
                                  const EmitContext& ctx,
                                  BunchWriter& out) {
    (void)schema; (void)runtime; (void)changed_root_handles; (void)ctx; (void)out;
    // Deferred to Session D (replication tick).
    spdlog::warn("[ActorBuilder] build_delta not yet implemented");
    return 0;
}

size_t ActorBuilder::build_destroy(const EmitContext& ctx, BunchWriter& out) {
    size_t start = out.bit_pos();
    // Close bunch: bControl=0, bReplicationPaused=0, bReliable=1, channel, etc.
    // With bClose — but UE5 puts bClose behind bControl in parser.  For actor
    // channel close, UE uses an empty bunch with bClose=1.  Simplified here:
    out.write_bit(1);  // bControl
    out.write_bit(0);  // bOpen
    out.write_bit(1);  // bClose
    out.write_serialize_int(0, 15);  // close_reason (0 = default)
    out.write_bit(0);  // bIsReplicationPaused
    out.write_bit(1);  // bReliable
    out.write_sip(ctx.channel);
    out.write_bit(0);  // bHasPackageMapExports
    out.write_bit(0);  // bHasMustBeMappedGUIDs
    out.write_bit(0);  // bPartial
    out.write_serialize_int(ctx.ch_sequence, 1024);
    // No channel name for close bunch in UE5 compat mode
    out.write_serialize_int(0, 1024 * 8);  // BunchDataBits = 0
    return out.bit_pos() - start;
}

}}} // namespace aoc::protocol::emit
