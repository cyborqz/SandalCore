// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/emit/actor_builder.h
//
//  The schema-driven actor emitter.  Turns (ActorSchema + ActorRuntime)
//  into bunch bytes.
//
//  Three production methods:
//    - build_spawn:     full actor spawn bunch (for initial replication)
//    - build_delta:     property delta bunch (for periodic updates)
//    - build_destroy:   actor destroy bunch
//
//  Session C scope: build_spawn with byte-round-trip fidelity —
//  output must be re-parseable by our wire/packet_parser.
//
//  LAYER:  Protocol / emit
//  SESSION: C
// ============================================================================
#pragma once

#include "protocol/emit/bunch_writer.h"
#include "protocol/emit/package_map_exporter.h"
#include "protocol/emit/schema_value.h"
#include "protocol/schema/actor_schema.h"
#include <cstdint>
#include <vector>

namespace aoc { namespace protocol { namespace emit {

/// Per-emit context — client-specific details the builder needs.
/// Kept minimal for Session C; grows when we add visibility / multi-client.
struct EmitContext {
    uint32_t channel = 0;               // actor's replication channel
    uint32_t ch_sequence = 0;           // reliable channel sequence (incremented by caller)
    bool     is_reliable = true;
    bool     is_partial = false;
    bool     partial_initial = true;
    bool     partial_final = true;
    // AoC-specific 3rd partial flag (bPartialCustomExportsFinal, the middle
    // bit of the three; "last fragment still carrying exports"
    // semantically).  Not set by the original server: 0 in all 452,536
    // bunches of fixtures/replay_data.bin (see the actor-channel notes in
    // server/worldd/net/connection.h), so new emitters leave it false.
    // (docs/world-bootstrap-findings.md §7 wrote it up from nine early
    // bunches and says otherwise.)  It stays settable per bunch so a
    // parsed bunch can be re-emitted with its flags (round-trip parity).
    bool     partial_custom_exports_final = false;
// (C.O.A | Cyb)

    // Control-bunch fields (captured AoC PC spawns are control bunches
    // with bOpen=1 — see docs/world-bootstrap-findings.md §7).  Default
    // is_control=false for data bunches (existing ActorBuilder callers
    // don't need to change).  When is_control=true, b_open/b_close
    // are serialized into the bunch header per UE5 wire format.
    bool     is_control = false;
    bool     b_open = false;
    bool     b_close = false;

    // Full-payload splice mode.  When true AND spliced_tail_bits is
    // provided, `ActorBuilder::build_spawn` writes:
    //   [Bunch header] + [spliced_tail_bits verbatim]
    // — skipping BOTH the export section AND the SerializeNewActor body.
    // Use this for bunches whose payload format we don't yet fully
    // parse (bHasRepLayoutExport=1, RepLayout cmd_index catalog not
    // yet RE'd) — we splice the entire captured payload to produce
    // byte-identical output.
    bool     splice_full_payload = false;

    // Explicit bHasPackageMapExports bit value for the header.  When
    // `splice_full_payload` is true, we don't populate `package_map_exports`
    // (the spliced payload already contains the export section), but we
    // still need the header bit to be set correctly.  This field lets
    // callers set the bit directly.  When false, the existing behavior
    // (derive from !package_map_exports.empty()) is used.
    bool     explicit_has_pme = false;
    bool     has_pme_value    = false;

    // Explicit bHasMustBeMappedGUIDs bit (similarly for full-splice mode
    // or other cases where we need to reproduce a specific value).
    bool     has_mbg_value = false;

    /// Session H.3d: inline NetGUID export entries to write at the start of
    /// the bunch payload.  When non-empty, the bunch header's
    /// `bHasPackageMapExports` bit is set to 1 and the export section is
    /// emitted before SerializeNewActor.  Leave empty for bunches targeting
    /// clients that already have the NetGUID cache populated (subsequent
    /// references become bare 128-bit GUIDs).
    std::vector<ExportEntry> package_map_exports;

    /// Session H.4: splice raw bits for the RepLayout property stream tail.
    /// When `spliced_tail_bit_count > 0`, `ActorBuilder::build_spawn` SKIPS
    /// emitting the content-block (property stream) and subobject blocks;
    /// instead it appends these raw bits directly.
    ///
    /// Use case: the export section + SerializeNewActor structure is
    /// byte-identical with captured (100% proven), but the RepLayout tail
    /// needs per-property-value calibration we don't yet have.  Splice
    /// captured bytes from a proven-good session to close the gap while
    /// still generating everything else ourselves.
    ///
    /// `spliced_tail_bits` must point to `ceil(spliced_tail_bit_count / 8)`
    /// bytes of data, read LSB-first (same format as `BunchWriter`).
    /// Caller retains ownership.  Pass both as 0/nullptr to disable.
    const uint8_t* spliced_tail_bits = nullptr;
    size_t         spliced_tail_bit_count = 0;
    size_t         spliced_tail_start_bit = 0;   // first bit of spliced_tail_bits to copy

    /// Phase D Step 2.1 (2026-05-05) — Appearance payload override.
    ///
    /// When `appearance_payload_bit_count > 0`, the ActorBuilder REPLACES
    /// the (empty) per-property payload of the subobject at index
    /// `appearance_subobject_index` (default = 7, the 8th subobject =
    /// CharacterAppearanceComponent in PlayerPawn's schema) with the
    /// pre-serialized bits supplied here.
    ///
    /// The bits are produced by aoc::net::build_appearance_payload_bits
    /// in `net/appearance_data.h`.  They contain a property-update wire
    /// stream targeting `CharacterCustomization` and `bForceHideHeldItems`
    /// — the two replicated properties on UCharacterAppearanceComponent
    /// per the SDK dump (Dumper-7).
    ///
    /// Caller retains ownership of `appearance_payload_bits`.
    /// Pass count=0 to keep the legacy (empty payload) behavior.
    const uint8_t* appearance_payload_bits      = nullptr;
    uint32_t       appearance_payload_bit_count = 0;
    uint32_t       appearance_subobject_index   = 7;
    /// PD2.1 — when true, ALSO wrap the appearance subobject in a V3
    /// stably-named content block even if the payload is 0 bits.  Lets
    /// us register the subobject's NetGUID with the client's PackageMap
    /// without sending property data — relies on client falling back to
    /// its locally-loaded lobby character data via OnRep_CharacterCustomization.
    bool           appearance_force_v3_wrap     = false;
// ~ C Y B & C | O | A ~

    /// Session B.2: ChName wire format.  Only serialized when
    /// `is_reliable || bOpen`.  Two modes matching UE5's
    /// UPackageMap::SerializeName:
    ///
    ///   ch_name_is_hardcoded=true : EName-table index (default: 102 =
    ///                               NAME_Actor for standard UE5 actors).
    ///   ch_name_is_hardcoded=false: FString name + int32 Number, used
    ///                               for AoC-specific classes like
    ///                               "PlayerCharacter" or "AshesPawn"
    ///                               that aren't in the hardcoded enum.
    ///
    /// apply_live_*_spawn() populates these from the parser so we
    /// round-trip channel names correctly for any actor type.
    bool        ch_name_is_hardcoded = true;
    uint32_t    ch_name_ename_idx    = 102;  // NAME_Actor
    std::string ch_name_string;              // only when !ch_name_is_hardcoded
    int32_t     ch_name_number       = 0;
};

class ActorBuilder {
public:
    /// Build a full actor spawn bunch for an actor described by `schema`
    /// with runtime values from `runtime`.  Writes into `out`.
    ///
    /// Output layout (matches UE5 ActorOpen):
    ///   [Bunch header]
    ///   [SerializeNewActor: actor NetGUID + flags + archetype + level + transform]
    ///   [Content block]
    ///     [bHasRepLayout=1, bIsActor=1]
    ///     [SerializeIntPacked payload size]
    ///     [RepLayout handle stream: handle + bits for each root property]
    ///     [terminator handle=0]
    ///   [Per-component content blocks]
    ///     [bHasRepLayout=1, bIsActor=0]
    ///     [subobject NetGUID]
    ///     [SerializeIntPacked payload size]
    ///     [RepLayout handle stream for component's properties]
    ///     [terminator handle=0]
    ///
    /// Returns number of bits written.  On error (schema/runtime mismatch)
    /// returns 0 and the writer is in an indeterminate state — caller should
    /// reset and log.
    size_t build_spawn(const schema::ActorSchema& schema,
                        const ActorRuntime& runtime,
                        const EmitContext& ctx,
                        BunchWriter& out);

    /// Build a property-delta bunch for a subset of changed handles.
    /// `changed_root_handles` and `changed_component_values` specify which
    /// properties to re-emit (rest are assumed unchanged client-side).
    size_t build_delta(const schema::ActorSchema& schema,
                        const ActorRuntime& runtime,
                        const std::vector<uint32_t>& changed_root_handles,
                        const EmitContext& ctx,
                        BunchWriter& out);

    /// Build a destroy bunch (bunch with bClose=1, no payload).
    size_t build_destroy(const EmitContext& ctx, BunchWriter& out);

    // ── Property-type emitters (public for testing) ──
    static void emit_property(const schema::PropertySchema& prop,
                               const SchemaValue& value,
                               BunchWriter& out);
};

/// UPackageMapClient::SerializeNewActor: actor, archetype and level GUIDs,
/// then location / rotation / scale / velocity, each behind a presence bit.
/// Returns bits written.  (Checked against the original server's
/// PlayerController open in test_serialize_new_actor.)
size_t write_serialize_new_actor(BunchWriter& out, const ActorRuntime& runtime,
                                 const schema::ActorSchema& schema);

/// APlayerController::OnSerializeNewActor, written after the spawn record
/// of a PlayerController channel open: NetPlayerIndex (8) and
/// ClientHandshakeId (32).  AoC's network version predates
/// LocalPlayerConnectionIdentifier, so nothing else follows.
void write_player_controller_open_fields(BunchWriter& out, uint8_t net_player_index,
                                         uint32_t client_handshake_id);

/// UE::Net::WriteQuantizedVector for already-scaled integer components:
/// SerializeInt(1 << 6 | bits, 128) (the scaled flag plus the component
/// width) and three two's-complement components of `bits` bits, where
/// `bits` is the smallest width that holds all three (GetBitsNeeded).
void write_quantized_vector(BunchWriter& out, int64_t x, int64_t y, int64_t z);

}}} // namespace aoc::protocol::emit
