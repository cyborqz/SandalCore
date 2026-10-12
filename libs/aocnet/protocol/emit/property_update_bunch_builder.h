// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/emit/property_update_bunch_builder.h
//
//  Native emitter for property-delta bunches (standalone Name update, etc.).
//  Sibling to ActorBuilder: ActorBuilder opens an actor channel (pkt#22
//  style bunches with bControl=1, bOpen=1), PropertyUpdateBunchBuilder
//  emits subsequent delta bunches on that already-open channel.
//
//  WHY A SEPARATE CLASS
//  --------------------
//  ActorBuilder is designed for the FULL actor-open bunch (exports +
//  SerializeNewActor + root + component content blocks).  A property
//  delta is much simpler:
//
//    [Bunch header]
//      bControl=0, bReliable=1, bPartial=0, bOpen=0, bClose=0
//      channel=<actor's channel>, ChSequence=<next reliable seq>
//      bHasPackageMapExports=0, bHasMustBeMappedGUIDs=0
//      BunchDataBits=<payload bit count>
//    [Payload]
//      Per-property encoding in the same per-cmd-index format as pkt#22's
//      content block: [uint32 cmd_index LSB-first][property body].
//
//  USAGE
//  -----
//    PropertyUpdateBunchBuilder b;
//    b.set_channel(3);                        // PC's actor channel
//    b.set_ch_sequence(next_seq);
//    b.add_name_update("MyHero");             // convenience
//    BunchWriter out;
//    size_t bits = b.build(out);              // writes header + payload
//
//  The caller wraps the resulting bytes in the UDP packet prefix
//  (handled by udp_packet_emitter / build_replay_packet).
//
//  CURRENT LIMITATIONS (Phase III M1 scope)
//  ----------------------------------------
//  - Only `add_name_update` is implemented.  Other property types follow
//    the same shape once we RE their cmd_index / prefix / encoding.
//  - The 16-byte "mystery prefix" used by Name is copied verbatim from
//    captured pkt#104 (see name_update_bunch.h).  Other properties may
//    have different prefixes.
//  - No `bHasRepLayout` style header bits; the Phase III RE of pkt#22's
//    property stream format showed AoC's bunches don't use those.
//
//  LAYER:   Protocol / emit
//  OWNER:   Phase III M1
//  SESSION: 2026-04-24
// ============================================================================
#pragma once

#include "protocol/emit/bunch_writer.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace aoc { namespace protocol { namespace emit {

class PropertyUpdateBunchBuilder {
public:
    PropertyUpdateBunchBuilder() = default;

    // ── Header configuration ──────────────────────────────────────────
    void set_channel(uint32_t ch)         { channel_ = ch; }
    void set_ch_sequence(uint32_t seq)    { ch_sequence_ = seq; }
    void set_reliable(bool r)             { is_reliable_ = r; }

    /// bHasMustBeMappedGUIDs flag (header bit). Empirical: every captured
    /// ch=3 property-update bunch from replay_data.bin sets this to 1.
    /// Setting it without including a MBG list in the payload is safe per
    /// the wire-format spec (bit is purely a "client may defer if GUIDs
    /// not yet mapped" hint — no inline payload follows the header bit).
    void set_has_mbg(bool v)              { has_mbg_ = v; }
// C | O | A - C_Y_B

    /// V3 inner-property format selector.
    /// MODERN (default): [SerializeInt handle, MAX][value bits — type-known size, NO NumBits prefix]
    ///   Per RE of sub_143F2DC60 modern path (line 169-220): parser reads SerializeInt(handle, MAX)
    ///   then looks up cmd metadata to know the value bit width (no per-property NumBits prefix).
    /// LEGACY: [SerializeInt handle, MAX][SIP NumBits][NumBits bits value]
    ///   Per RE backwards-compat path (line 50-148): linked-list walk with NumBits per entry.
    /// Empirical: V3 with LEGACY format silently drops; MODERN format is what AOC's modern path expects.
    void set_use_modern_inner_format(bool v) { v3_use_modern_inner_format_ = v; }

    /// bIsReplicationPaused flag (header bit, position 1).
    /// EMPIRICAL DISCOVERY 2026-04-26: every captured ch=3 update bunch
    /// targeting subobject GUID 7193 (the PlayerState) has this bit SET.
    /// More generally, captured subobject-targeting bunches set rp=1 33%
    /// of the time vs 0.4% for root-actor bunches — strong correlation.
    /// In stock UE5, bIsReplicationPaused signals a channel pause/unpause
    /// transition.  In AoC the bit appears repurposed (or simply always
    /// set when targeting certain subobject classes).  Without setting
    /// it, our V3 emit was silently dropped despite header parsing OK.
    /// inject_v3_property_update auto-sets this to true when targeting a
    /// subobject (v3_subobject_guid != 0).
    void set_is_rep_paused(bool v)        { is_rep_paused_ = v; }

    /// ChName configuration (only emitted on reliable, non-partial bunches —
    /// per parse_sc_bunch in net/sc_bunch_parser.h).
    ///
    /// EMPIRICAL DEFAULT (verified 2026-04-26 by decoding replay_data.bin):
    ///   - 285 bunches with hardcoded ChName across the capture
    ///   - EName=103 dominant (148 occurrences) → "Actor"-like default
    ///   - EName=71  (37×) for specific classes (PlayerController?, Pawn?)
    ///   - EName=255 for ch=0 NMT control channel
    ///   - EName=102 (the prior codebase default in actor_builder.h:115 and
    ///     pc_emitter.cpp:215) NEVER appears — it was an unverified guess.
    ///
    /// Default 103 is right for ordinary actor channels.  For ch=3 PC
    /// channel callers should set 71 to match captured PC ActorOpen
    /// EName histogram.  For ch=0 NMT, set 255.
    void set_ch_name_hardcoded(uint32_t ename_idx) {
        ch_name_is_hardcoded_ = true;
        ch_name_ename_idx_    = ename_idx;
    }
    void set_ch_name_string(const std::string& s, int32_t number = 0) {
        ch_name_is_hardcoded_ = false;
        ch_name_string_       = s;
        ch_name_number_       = number;
    }

    // ── Property-update accumulators ──────────────────────────────────
    /// V1 — queue a mid-bunch-region Name payload.  Byte-identical to
    /// the captured pkt#104 Name region; DO NOT USE FOR LIVE SENDS —
    /// the client rejects it because the payload starts with what
    /// parses as `NumGUIDsInBunch = 16M`.  Kept for the bit-identity
    /// test (test_name_update_bunch).
    void add_name_update(const std::string& name);

    /// V2 — queue a proper property-delta Name payload.  Use this for
    /// live sends.  Structure:
    ///    [bHasRepLayoutExport=0][NumGUIDs=0][cmd_index][FString]
    /// `cmd_index` candidates: 28 (our catalog) or 106/0x6A (observed).
    void add_name_update_v2(const std::string& name, uint32_t cmd_index);

    // ── Generic property updates (Path B, Tier 2 — added 2026-04-26) ──
    //
    // Each method queues a single property-update payload of the form:
    //    [bHasRepLayoutExport=0]      (1 bit)
    //    [NumGUIDsInBunch=0]          (32 bits)
    //    [cmd_index]                   (32 bits — caller specifies)
    //    [property body]               (type-specific bits)
    //
    // The cmd_index identifies which property to update.  Candidates per
    // property come from RE / empirical discovery (see
    // docs/PATH-B-SYNTHETIC-PROPERTY-UPDATE.md).
    //
    // For multiple property updates, call multiple add_*_update_v2() — each
    // creates a separate Blob in the queue, all written into the same bunch
    // payload at build() time.

    /// Queue an int32 property update.  Used for Level, Gold, XP, etc.
    void add_int32_update_v2(int32_t value, uint32_t cmd_index);

    /// Queue a float property update.  Used for Health, Mana, Stamina, etc.
    void add_float_update_v2(float value, uint32_t cmd_index);

    /// Queue a bool property update.  Used for flag-style properties.
    void add_bool_update_v2(bool value, uint32_t cmd_index);

    /// Queue a uint8/byte property update.  Used for class enums, etc.
    void add_uint8_update_v2(uint8_t value, uint32_t cmd_index);

    /// Append a pre-rendered payload blob.  Use this when you have a
    /// property body encoded elsewhere (e.g. from replay_mutator's
    /// snippets or a different emitter).
    void add_raw_payload(const uint8_t* data, size_t bit_count);

    // ─── V3 — CORRECT content-block-framed format (2026-04-26) ─────────
    //
    // After RE'ing sub_143F2C340 (ReadContentBlockHeader),
    // sub_143F2DA40 (ReadContentBlockPayload) and
    // sub_143F2DC60 (ReadPropertyChangeHeader), we know the actual wire
    // format the client expects for property updates on an open channel:
    //
    //   Per-content-block:
    //     [1 bit bOutermostEnd=0]
    //     [1 bit bIsChannelActor]   ← if 1, no NetGUID; targets the channel's actor
    //     [if !bIsChannelActor: NetGUID via PackageMap]
    //     [SerializeIntPacked NumPayloadBits]
    //     [Inner bunch — NumPayloadBits bits:]
    //       Per property:
    //         [SerializeInt(max=NumProperties) cmd_handle]
    //         [SerializeIntPacked NumBits]
    //         [NumBits bits value]
    //
    //   End marker (after all blocks):
    //     [1 bit bOutermostEnd=1]
    //
    // V1/V2 above used a DIFFERENT format (matching pkt#22's PME-section
    // format) which the client rejected.  V3 uses the proper content-block
    // framing per ProcessBunch.
    //
    // USAGE:
    //   builder.set_channel(N);
    //   builder.set_ch_sequence(seq);
    //   builder.set_reliable(true);
    //   builder.v3_begin_content_block_channel_actor(num_props_for_class);
    //   builder.v3_add_property_int32(cmd_handle_level, 25);
    //   builder.v3_add_property_float(cmd_handle_health, 9999.0f);
    //   builder.v3_add_property_fstring(cmd_handle_name, "MyHero");
    //   builder.v3_end_content_block();
    //   builder.v3_finish_bunch();        // appends end marker
    //   builder.build(out);

    /// Begin a new content block targeting the channel's main actor.
    /// `num_properties_in_class` is the class's NumReplicated count, used
    /// to compute SerializeInt bit width for cmd_handles.  Common values:
    /// PlayerController ~80, PlayerState ~30, AttributeSet ~20.  Wrong
    /// value causes parse drift.  When unsure, try 256 (8 bits) first.
    void v3_begin_content_block_channel_actor(uint32_t num_properties_in_class);
// ~ CoA & C Y B ~

    /// Begin a content block targeting a subobject (component / state /
    /// attribute set) by its CACHED NetGUID.
    ///
    /// Encoding (verified empirically by decoding ch=3 pkt#30 from
    /// replay_data.bin and confirming exact-fit consumption):
    ///   [1 bit] bOutermostEnd      = 0
    ///   [1 bit] bIsChannelActor    = 0
    ///   [SIP]   subobject_netguid
    ///   [SIP]   NumPayloadBits     (computed at v3_end_content_block)
    ///
    /// `subobject_netguid` is the SIP-encoded reference to a previously-
    /// declared NetGUID — the client must already have the GUID cached
    /// (typically from a prior ActorOpen export).  Capture analysis:
    ///   - ch=3 (PC's actor channel): 6 bunches reference subobject GUID
    ///     7193, with payload sizes up to 2755 bits.  This is almost
    ///     certainly the PlayerState (where Name lives).
    ///   - ch=4: GUID 64 (Pawn?)
    ///   - ch=12: GUID 13523 (large object, 2486-bit payloads)
    ///   - GUID 0 appears frequently with tiny payloads — likely an
    ///     "actor heartbeat" alias for the channel root.
    ///
    /// `num_properties_in_class` controls the SerializeInt bit width for
    /// cmd_handle (CeilLog2(N)).  Subobject classes (PlayerState,
    /// AttributeSets) typically have fewer properties than the PC root —
    /// try 64, 128, 256 in order.
    void v3_begin_content_block_subobject(uint32_t subobject_netguid,
                                          uint32_t num_properties_in_class);

    /// Add an int32 property update inside the current content block.
    void v3_add_property_int32(uint32_t cmd_handle, int32_t value);

    /// Add a float property update.
    void v3_add_property_float(uint32_t cmd_handle, float value);

    /// Add a bool property update (1 bit value).
    void v3_add_property_bool(uint32_t cmd_handle, bool value);

    /// Add a uint8/byte property update.
    void v3_add_property_uint8(uint8_t cmd_handle, uint8_t value);

    /// Add an FString (ANSI) property update.  Length-prefixed + NUL-terminated.
    void v3_add_property_fstring(uint32_t cmd_handle, const std::string& s);

    /// PM53 (2026-04-30) — Phase C: add a NetGUID (FIntrepidNetworkGUID,
    /// 128 bits = 4×u32) property update.  Used for Pawn, PlayerState,
    /// and other actor-reference properties on UE5 ObjectProperty fields.
    /// Wire format mirrors `write_intrepid_guid`: ObjLo, ObjHi, Srv, Rnd
    /// each as 32 bits LSB-first.
    void v3_add_property_netguid(uint32_t cmd_handle,
                                  uint64_t object_id,
                                  uint32_t server_id,
                                  uint32_t randomizer);

    /// PM123 (2026-05-06) — captured-format-correct property NetGUID write.
    /// Always writes the SIP(128) NumValueBits prefix regardless of the
    /// v3_use_modern_inner_format_ flag (which is wrong for properties; the
    /// captured AOC replay clearly uses LEGACY-with-SIP for property updates).
    /// See decode_pc_property_fixtures.py output for empirical evidence.
    void v3_add_property_netguid_with_sip(uint32_t cmd_handle,
                                            uint64_t object_id,
                                            uint32_t server_id,
                                            uint32_t randomizer);

    /// PM68 (2026-04-30) — Phase C: RPC call with NO parameters (zero-arg).
    /// Writes just the handle bits.  Used for probing whether a function
    /// takes 0 params (clean exit) vs N>0 params (Reader.IsError).
    void v3_add_rpc_handle_only(uint32_t cmd_handle);

    /// 2026-05-06 — full AOC custom-mode field-loop wire for a NO-PARAM RPC.
    /// Mirrors `v3_add_rpc_pawn_param_aoc_intrepid` but skips the per-param
    /// fields, going straight from the 1-bit advance to the SIP(0) terminator.
    /// This is the format `ClientInitializeCharacter()` and other 0-param
    /// AAoCPlayerController Client* RPCs use.
    ///
    /// Wire (inside V3 content block):
    ///     SerializeInt(handle, MAX)   ceil(log2(MAX)) bits
    ///     1-bit advance               1 bit  (consumed by sub_7FF6BD814D20)
    ///     SIP(0) terminator           8 bits (immediately exits sub_7FF6BD8155B0 field loop)
    /// Total: handle_bits + 9 bits.
    void v3_add_rpc_no_params(uint32_t cmd_handle);

    /// PM75 (2026-04-30) — SIP-based inner format from captured replay RE.
    /// AOC's actual wire format inside the V3 content block is:
    ///   SIP(handle+1) + 128-bit IntrepidNetGUID + SIP(0) terminator
    /// NOT SerializeInt(handle, MAX) like our other v3_add_property_* methods.
    /// This matches the format observed in the captured replay's 174-bit
    /// bunches with NumPayloadBits=156 on Channel 3 (PC).
    /// Use this for property updates (e.g., setting Pawn property to trigger
    /// OnRep_Pawn → AcknowledgePossession).
    void v3_add_property_netguid_sip(uint32_t cmd_handle,
                                       uint64_t object_id,
                                       uint32_t server_id,
                                       uint32_t randomizer);

    /// PM81 (2026-05-02) — Phase C: RPC object-reference param with EXPLICIT
    /// SIP(NumBits) prefix and optional null-indicator bit.
    ///
    /// Wire format (always — independent of MODERN/LEGACY content block):
    ///   [SerializeInt(cmd_handle, MAX)]    field handle
    ///   [SIP(param_num_bits)]              per-RPC-param SIP prefix
    ///   [optional 1 bit "non-null" = 1]    if include_null_bit
    ///   [4 × uint32 LSB-first]             128-bit FIntrepidNetGUID
    ///
    /// Why a separate method from v3_add_property_netguid:
    /// AOC's `ReceivePropertiesForRPC` ALWAYS reads SIP(NumBits) per RPC
    /// param, regardless of content-block format.  The SIP value must
    /// match the actual bits APawn* deserialization will consume.
    ///
    /// PM79 (LEGACY content block, no null bit, SIP=128) → Mismatch
    /// PM80 (MODERN content block, no SIP, no null bit) → Mismatch
    /// PM81 attempt: SIP=129, include_null_bit=true (1-bit non-null +
    /// 128-bit GUID = 129 total bits).  Hypothesis from log analysis.
    /// PM81+: leading_null_bit: -1=none, 0=write '0', 1=write '1'.
    ///        trailing_pad_bits: 0..32 zero bits after the 128-bit GUID.
    /// SIP value must equal: (leading_null_bit != -1 ? 1 : 0) + 128 + trailing_pad_bits.
    void v3_add_rpc_object_param(uint32_t cmd_handle,
                                  uint64_t object_id,
                                  uint32_t server_id,
                                  uint32_t randomizer,
                                  uint32_t param_num_bits,
                                  int32_t leading_null_bit,
                                  uint32_t trailing_pad_bits);

    /// PM84 (2026-05-03) — DEBUGGER-CONFIRMED wire format for AOC ClientRestart.
    ///
    /// Breakpoint at sub_7FF6BD263E80 confirmed AOC sizes the bit reader for
    /// ClientRestart's APawn* param to EXACTLY 8 bits = 1 SIP byte.
    ///
    /// Wire format:
    ///   [SerializeInt(cmd_handle, MAX)]       12 bits — function handle
    ///   [SIP-encoded netguid = 1 byte]         8 bits — APawn* reference
    ///
    /// Total inner bits: 20.  No extra framing, no terminator.
    ///
    /// The `netguid` value should be the AOC PackageMap's short alias for
    /// the target Pawn — typically a small sequential value (1, 2, 3, ...).
    /// The exact alias is assigned at Pawn registration; for our minted
    /// Pawn (= first dynamic actor), try 1 first, then 2, 3 if Mismatch.
    void v3_add_rpc_short_netguid(uint32_t cmd_handle, uint32_t netguid);
// ~ C.O.A :: C_Y_B ~

    /// PM83 (2026-05-02) — AOC CUSTOM MODE per-field RPC param format.
    ///
    /// RE'd from sub_7FF6BD814D20 + sub_7FF6BD8155B0 in AOC client binary:
    ///   [SerializeInt(cmd_handle, MAX)]   — function handle (outer)
    ///   [1 bit prefix]                     — consumed by sub_7FF6BD814D20 line 109
    ///   [SIP(field_handle + 1)]            — per-field handle (line 179)
    ///   [SIP(field_size)]                  — per-field bit count (line 225)
    ///   [field_size bits]                  — value bits (sub-reader)
    ///   [SIP(0)]                           — field-list terminator
    ///
    /// For ClientRestart(APawn* NewPawn) — single param at field index 0:
    ///   field_handle = 0, so we write SIP(1).
    ///
    /// `value_num_bits` is the SIZE in bits of the value payload (AOC copies
    /// this many bits to a sub-reader for APawn* deserialization).  Try 128
    /// (full FIntrepidNetGUID), 80 (SIP-encoded GUID per our values), 32
    /// (standard UE5 NetGUID size), etc.
    void v3_add_rpc_object_param_aoc_custom(uint32_t cmd_handle,
                                             uint64_t object_id,
                                             uint32_t server_id,
                                             uint32_t randomizer,
                                             uint32_t value_num_bits);

    /// PM86 (2026-05-03) — APawn* RPC param with leading bIsNullActor bit.
    ///
    /// PM85 sent a 128-bit raw FIntrepidNetGUID as the param value and AOC
    /// still fired Mismatch.  PM79 had already tried the same.  Per UE5
    /// FObjectProperty::NetSerializeItem convention (and the comment block
    /// at pc_emitter.cpp lines 569-571), the param value is laid out as:
    ///
    ///   [1 bit bIsNullActor]   — 0 = non-null, 1 = null
    ///   [128 bit FIntrepidNetGUID]   — read iff bIsNullActor == 0
    ///
    /// Total = 129 bits.  This helper emits the AOC-custom RPC framing with
    /// value_num_bits=129 and writes the 1-bit non-null indicator BEFORE the
    /// 128-bit GUID (the existing _aoc_custom helper writes any pad after,
    /// which puts the indicator at the wrong offset for AOC's reader).
    ///
    /// Wire layout:
    ///   [SerializeInt(cmd_handle, MAX=4096)]   13 bits
    ///   [1 bit prefix]                          1 bit
    ///   [SIP(1) = field_handle+1]               8 bits
    ///   [SIP(129) = value_num_bits]            16 bits
    ///   [1 bit bIsNullActor = 0]                1 bit
    ///   [128 bit FIntrepidNetGUID]            128 bits
    ///   [SIP(0) = field-list terminator]        8 bits
    ///   ──────────────────────────────────
    ///   Total inner = 175 bits.
    void v3_add_rpc_pawn_param_aoc(uint32_t cmd_handle,
                                    uint64_t object_id,
                                    uint32_t server_id,
                                    uint32_t randomizer);

    /// PM87 (2026-05-03) — brute-force probe for unknown value_num_bits.
    ///
    /// Like v3_add_rpc_pawn_param_aoc but with caller-controlled total size
    /// and number of leading zero bits.  Used to fire multiple probes in a
    /// single bunch, each testing a different hypothesis for AOC's expected
    /// FObjectProperty wire size.
    ///
    /// Wire layout:
    ///   [SerializeInt(cmd_handle, MAX=4096)]   13 bits
    ///   [1 bit prefix = 0]                      1 bit
    ///   [SIP(field_handle+1) = SIP(1)]          8 bits
    ///   [SIP(value_num_bits)]               8 or 16 bits
    ///   [leading_zero_bits zero bits]
    ///   [128 bit FIntrepidNetGUID, truncated to (value_num_bits - leading_zero_bits)]
    ///   [SIP(0) field-list terminator]          8 bits
    ///
    /// Special cases:
    ///   value_num_bits = 0  →  no value bits, no leading bits, just framing
    ///   leading_zero_bits >= value_num_bits  →  all zeros, no GUID
    void v3_add_rpc_pawn_param_brute(uint32_t cmd_handle,
                                      uint64_t object_id,
                                      uint32_t server_id,
                                      uint32_t randomizer,
                                      uint32_t value_num_bits,
                                      uint32_t leading_zero_bits);

    /// PM89 (2026-05-03) — BARE-WIRE RPC param.  Per IDA dump of
    /// sub_7FF6BD814D20 (param deserializer): AOC iterates over the
    /// UFunction's STATIC param descriptors and calls NetSerializeItem on
    /// each, passing the body reader directly with no per-param SIP framing.
    /// The Mismatch comparison at sub_7FF6BD263E80+0x7A7 checks that the
    /// body reader's PosBits exactly matches MaxBits (= our V3 NumPayloadBits).
    ///
    /// So the correct wire format is just:
    ///   [SerializeInt(handle, MAX=4096)]   13 bits
    ///   [1 bit prefix] (consumed by param iterator)
    ///   [raw value bits]                   N bits, written by APawn*
    ///                                      NetSerializeItem
    ///   ───────────────────────────────────
    ///   Total inner = 14 + N bits
    ///
    /// Caller specifies total raw value bits (typical N values to test):
    ///   128  — raw FIntrepidNetGUID
    ///   129  — 1-bit null + 128-bit GUID
    ///   144  — 16-bit name-idx + 128-bit GUID
    ///   137  —  9-bit AOC ENameIdx + 128-bit GUID
    ///
    /// leading_zero_bits = how many of the N value bits are leading zeros
    /// before the GUID payload (e.g. 1 for null indicator, 16 for name idx).
    void v3_add_rpc_pawn_param_bare(uint32_t cmd_handle,
                                     uint64_t object_id,
                                     uint32_t server_id,
                                     uint32_t randomizer,
                                     uint32_t value_num_bits,
                                     uint32_t leading_zero_bits);
// -- CoA . C-Y-B --

    /// PM90 (2026-05-03) — bare wire with caller-controlled prefix-bit count.
    ///
    /// Stock UE5 source confirms:
    ///   FObjectPropertyBase::NetSerializeItem → Map->SerializeObject(Ar, ...)
    ///   UPackageMapClient::SerializeObject → InternalLoadObject (load path)
    ///   InternalLoadObject reads:  Ar << NetGUID  (just the GUID)
    ///                              NET_CHECKSUM_OR_END  (NO-OP in Shipping)
    ///                              (ExportFlags only if IsDefault() — not our case)
    ///
    /// → Stock UE5 wire = ONLY the NetGUID.  No leading bit, no trailing.
    /// → AOC custom: 128-bit raw FIntrepidNetGUID, no framing.
    ///
    /// This helper allows toggling the optional "1-bit prefix" we previously
    /// always emitted (which IDA suggested was a 1-bit advance, but stock UE5
    /// has no such bit on the wire).
    ///
    ///   prefix_bits = 0  →  handle(13) + 128-bit GUID         = 141 bits
    ///   prefix_bits = 1  →  handle(13) + 1bit + 128-bit GUID  = 142 bits
    void v3_add_rpc_pawn_param_v2(uint32_t cmd_handle,
                                   uint64_t object_id,
                                   uint32_t server_id,
                                   uint32_t randomizer,
                                   uint32_t value_num_bits,
                                   uint32_t leading_zero_bits,
                                   uint32_t prefix_bits);

    /// PM92 (2026-05-03) — STOCK UE5 ReadFieldHeaderAndPayload format.
    ///
    /// Per Engine/Private/DataChannel.cpp:5174 ReadFieldHeaderAndPayload:
    ///   const int32 RepIndex = Bunch.ReadInt( ClassCache->GetMaxIndex() );
    ///   uint32 NumPayloadBits = 0;
    ///   Bunch.SerializeIntPacked( NumPayloadBits );
    ///   OutPayload.SetData( Bunch, NumPayloadBits );
    ///
    /// Then RepLayout::ReceivePropertiesForRPC reads from OutPayload.  For
    /// each non-FBoolProperty param:
    ///   if (Reader.ReadBit())  // per-prop "is present" flag
    ///       SerializeProperties_r → NetSerializeItem
    ///
    /// And finally Reader.GetBitsLeft() must == 0 → MaxBits set by SIP must
    /// equal what params consume.
    ///
    /// Wire layout for ClientRestart(APawn* NewPawn):
    ///   [SerializeInt(handle, MAX=4096)]   13 bits
    ///   [SIP(NumPayloadBits = 129)]        16 bits  (129 ≥ 128 → 2 SIP bytes)
    ///   [1-bit per-prop flag = 1]           1 bit   (must be true to read)
    ///   [128-bit FIntrepidNetGUID]        128 bits  (NetSerializeItem)
    ///   ─────────────────────────────────
    ///   Total inner                        158 bits
    ///
    /// `payload_num_bits` should be 1 + GUID_bits.  Caller specifies
    /// guid_bits (typically 128) to stay flexible.
    void v3_add_rpc_pawn_param_field(uint32_t cmd_handle,
                                      uint64_t object_id,
                                      uint32_t server_id,
                                      uint32_t randomizer,
                                      uint32_t guid_bits,
                                      uint32_t leading_zero_bits);

    /// PM95 (2026-05-03) — AOC custom-mode wire per IDA decomp of
    /// sub_7FF6BD8155B0 (the field-loop reader called when AOC custom flag is set).
    ///
    /// Inside the sub-reader, AOC reads:
    ///   [1-bit advance]               (consumed by sub_7FF6BD814D20)
    ///   [SIP(field_idx + 1)]          (loop iteration: 0 = terminator)
    ///   [SIP(field_size in bits)]     (reads K = bits in sub-sub-reader)
    ///   [K bits of value]             (NetSerializeItem reads from sub-sub-reader)
    ///   [SIP(0)]                       (terminator on next loop)
    ///
    /// For ClientRestart's APawn* param:
    ///   field_idx = 0 → wire = 1 (1 SIP byte)
    ///   field_size = 136 (128 NetGUID + 8 ExportFlags)
    ///   value = 128-bit FIntrepidNetGUID + 8 zero bits
    ///   terminator = 0 (1 SIP byte)
    ///
    /// Total in sub-reader = 1 + 8 + 16 + 136 + 8 = 169 bits.
    void v3_add_rpc_pawn_param_aoc_intrepid(uint32_t cmd_handle,
                                             uint64_t object_id,
                                             uint32_t server_id,
                                             uint32_t randomizer,
                                             uint32_t value_bits);

    /// Add a field-list terminator inside the current content block.
    /// Per PM6 RE of captured ch=3 bunches: AOC's parser reads inner
    /// (handle, value) pairs in a loop, expecting handle=0 (encoded via
    /// SerializeInt(0, MAX)) as the terminator.  Without it, the parser
    /// reads leftover bits from the trailing payload and fails with
    /// "Invalid replicated field 0" or similar.
    /// PM67 (2026-04-30): added to fix Phase C ClientInitializeCharacter
    /// which was correctly RPC-dispatched but trailing-bit-misparsed.
    void v3_add_terminator();

    /// Close the current content block and emit it as a queued payload.
    void v3_end_content_block();

    /// Finalize the bunch: appends the end marker bit (bOutermostEnd=1).
    /// Call AFTER all content blocks are added and before build().
    void v3_finish_bunch();

    // ── Inspection (before build) ─────────────────────────────────────
    size_t queued_payload_bits() const { return payload_bits_; }
// -- CoA x C.Y.B --

    // ── Output ────────────────────────────────────────────────────────
    /// Build the complete bunch (header + payload) into `out`.  Returns
    /// the number of bits written.  After build() the builder is still
    /// usable (call reset() to queue another bunch with the same channel).
    size_t build(BunchWriter& out) const;

    /// Reset queued updates but keep header config (channel/sequence).
    void reset_queue() {
        queued_payloads_.clear();
        payload_bits_ = 0;
    }

private:
    uint32_t channel_     = 0;
    uint32_t ch_sequence_ = 0;
    bool     is_reliable_ = true;
    bool     has_mbg_     = false;   // bHasMustBeMappedGUIDs (header hint)
    bool     is_rep_paused_ = false; // bIsReplicationPaused (capture: 1 for subobj targeting)

    // ChName fields — only written when is_reliable_ (matches the parser
    // condition in sc_bunch_parser.h).
    // Default 103 is empirically the most common EName index across captured
    // replay_data.bin (148 / 285 hardcoded ChName bunches).  PC actor
    // channel (ch=3) uses 71 — callers targeting that channel should
    // override via set_ch_name_hardcoded(71).  See setter docstring.
    bool        ch_name_is_hardcoded_ = true;
    uint32_t    ch_name_ename_idx_    = 103;   // empirical default (Actor-like)
    std::string ch_name_string_;               // only when !ch_name_is_hardcoded_
    int32_t     ch_name_number_       = 0;     // FName Number, default 0

    /// Queue of pre-rendered property blobs.  Each blob is a (bytes, bit_len)
    /// pair stored as a byte-vector with explicit bit count so we can splice
    /// sub-byte lengths correctly.
    struct Blob {
        std::vector<uint8_t> bytes;
        size_t               bit_count = 0;
    };
    std::vector<Blob> queued_payloads_;
    size_t            payload_bits_ = 0;

    // V3 state — the in-progress content block being built
    bool          v3_block_open_ = false;
    bool          v3_block_is_channel_actor_ = true;
    uint32_t      v3_subobject_netguid_ = 0;  // only used when !channel_actor
    uint32_t      v3_num_properties_ = 256;   // class NumReplicated, for SerializeInt
    bool          v3_use_modern_inner_format_ = true;  // 2026-04-26: skip SIP NumBits per RE
    bool          v3_perprop_bit_ = true;     // PM93: write 1-bit per-prop flag in v3_add_rpc_pawn_param_field
    BunchWriter   v3_inner_payload_;          // accumulating inner bunch bits

public:
    void set_perprop_bit(bool v) { v3_perprop_bit_ = v; }
private:

    /// Write the bunch header for a non-partial, non-control reliable
    /// data bunch.  Caller supplies the pre-computed BunchDataBits value.
    void write_bunch_header(BunchWriter& out, uint32_t bunch_data_bits) const;
};

}}} // namespace aoc::protocol::emit
