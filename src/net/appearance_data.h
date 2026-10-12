// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/appearance_data.h
//
//  Phase D Step 2.1 — converts the server's stored customization JSON into a
//  UE5 wire-format byte stream that the client can apply via OnRep_
//  CharacterCustomization.
//
//  The customization JSON (~4.8 KB per character) is already parsed out of the
//  CreateCharacterInfo protobuf in xclient_service.h::extract_customization_json.
//  This module:
//      1. Parses the JSON into an FCharacterCustomizationSaveData mirror struct
//         (matches the SDK layout of the Dumper-7 dump, GameSystemsPlugin_structs.hpp).
//      2. Serializes that struct to UE5 net wire format (bit-packed binary).
//      3. The resulting bytes go into the V3 content-block payload of the
//         Pawn's `Character Appearance` subobject — actor_builder.cpp picks
//         it up when v3_mode == 3.
//
//  Per the SDK (FCharacterCustomizationSaveData @ 0x0190 = 408 bytes):
//      - 30+ POD fields (int32/int64/float)
//      - 2 TArrays (DecalData, DecalBlendGroups)
//      - 3 bools (helmet/cape visible + bForceHideHeldItems)
//      - 3 enums (Gender/Race/Class — RepSkip, NOT serialized over net)
//      - 2 TMaps (FaceMorphWeightMaps, SectionsValues — RepSkip too)
//
//  Only the Net-flagged fields cross the wire — UE5 strips RepSkip props.
//
//  LAYER:   net
//  OWNER:   Phase D Step 2.1
//  SESSION: 2026-05-05
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aoc { namespace net {

/// Mirror of `FCharacterCustomizationSaveData` from the SDK.  Field names
/// match the JSON keys 1-to-1 so JSON parsing is a direct mapping.
///
/// All numeric fields are kept in their native UE5 types so when we serialize
/// to wire we just memcpy the bits in the right order (UE5 is little-endian).
struct CharacterCustomizationData {
    // ── Sliders / colors / cosmetics ────────────────────────────────────────
    int64_t  preset_guid              = 0;       // SDK +0x008
    int32_t  random_seed              = 0;       // SDK +0x010
    float    skin_color_hue           = 0.5f;    // SDK +0x014
    float    skin_color_pigmentation  = 0.5f;    // SDK +0x018
    float    normal_detail_strength01 = 0.0f;    // SDK +0x01C
    float    normal_detail_strength02 = 0.0f;    // SDK +0x020
    int32_t  skin_set                 = 0;       // SDK +0x024

    // Eyes
    int64_t  eye_color                = 0;       // SDK +0x028
    int64_t  eye_shape                = 0;       // SDK +0x030
    int64_t  sclera_shape             = 0;       // SDK +0x038
    int64_t  eyebrows                 = 0;       // SDK +0x040

    // Head hair
    int64_t  head_hair                = 0;       // SDK +0x048
    int64_t  head_hair_root_color     = 0;       // SDK +0x050
    int64_t  head_hair_tip_color      = 0;       // SDK +0x058
    float    head_hair_length         = 0.5f;    // SDK +0x060
    float    head_hair_contrast       = 0.5f;    // SDK +0x064
    float    head_hair_gradient       = 0.5f;    // SDK +0x068

    // Facial hair
    int64_t  facial_hair_lip          = 0;       // SDK +0x070
    int64_t  facial_hair_chin         = 0;       // SDK +0x078
    int64_t  facial_hair_cheek        = 0;       // SDK +0x080
    float    facial_hair_lip_length   = 0.0f;    // SDK +0x088
    float    facial_hair_chin_length  = 0.0f;    // SDK +0x08C
    float    facial_hair_cheek_length = 0.0f;    // SDK +0x090
    float    eyelash_length           = 0.5f;    // SDK +0x094
    int64_t  facial_hair_root_color   = 0;       // SDK +0x098
    int64_t  facial_hair_tip_color    = 0;       // SDK +0x0A0
    float    facial_hair_contrast     = 0.5f;    // SDK +0x0A8
    float    facial_hair_gradient     = 0.5f;    // SDK +0x0AC
// (C Y B) (CoA)

    // Racial features
    int64_t  racial_horns             = 0;       // SDK +0x0B0
    float    racial_horns_length      = 0.0f;    // SDK +0x0B8

    // Nails
    int64_t  nail_color               = 0;       // SDK +0x0C0
    float    nail_opacity             = 0.0f;    // SDK +0x0C8

    // ── Visibility flags ────────────────────────────────────────────────────
    bool     is_helmet_visible        = true;    // SDK +0x0F0
    bool     is_cape_visible          = true;    // SDK +0x0F1

    // ── Race / Gender / Class (TEnumAsByte, 1 byte each) ────────────────────
    //  IDA-confirmed (2026-05-05) — these ARE serialized over the wire as
    //  part of the FCharacterCustomizationSaveData struct.  Earlier comments
    //  saying "RepSkip" were wrong: RepSkip applies to actor RepLayout, not
    //  to struct member serialization.  The 51-property registration table
    //  at off_7FF6C4A83850 includes Race/Gender/Class with TypeTag=0x1E (Enum).
    //  Per AOC's "ECharacterClassification" enum (Character=0, Mount=1,
    //  NPC=2, SiegeVehicle=3, Caravan=4, cCount=5).
    uint8_t  gender_enum              = 0;       // TEnumAsByte<EGender>
    uint8_t  race_enum                = 0;       // TEnumAsByte<ERace>
    uint8_t  class_enum               = 0;       // TEnumAsByte<EClass>

    // ── Maps (TMap) — empty by default; client uses morph defaults ──────────
    //  Wire format: [int32 NumElements] then N (key, value) pairs.
    //  We send empty (0 elements) — the client falls back to the racial
    //  morph defaults baked into the SkeletalMesh.
    //  FaceMorphWeightMaps : TMap<FName,        float>
    //  SectionsValues      : TMap<EEnumKey,    float>

    // ── Replicated companion bool (lives on the COMPONENT, not in this
    //     struct, but we serialize it alongside since it's always emitted in
    //     the same property-update payload).
    bool     force_hide_held_items    = false;
};


/// Parse the customization JSON (the ~4.8 KB blob xclient_service.h already
/// extracts) into the struct above.  Tolerant of missing fields — defaults
/// are kept for any key that isn't present.
///
/// Returns true if JSON looked structurally valid (matched at least one key);
/// false if the JSON was malformed or empty.
bool parse_customization_json(const std::string& json,
                                CharacterCustomizationData& out);


/// Serialize the customization data to UE5 net wire format (bit-packed bytes).
///
/// Format (matches FRepLayout::SendProperties for the FCharacterCustomization-
/// SaveData struct property — what the client expects to read at OnRep time):
///
///   For each replicated field, in declaration order:
///       <field bytes per UE5 type>
///   Empty TArrays for DecalData and DecalBlendGroups: just
///       [int32 NumElements = 0]
///   Empty TMaps for FaceMorphWeightMaps and SectionsValues: skipped because
///       they're RepSkip (won't appear over wire).
///
/// `out` receives the raw payload bits.  Caller wraps it in the UE5 property-
/// update outer structure (handle + bChanged + SIP NumPayloadBits + payload).
void serialize_customization_to_wire(const CharacterCustomizationData& data,
                                       std::vector<uint8_t>& out);

// C_Y_B : CoA

/// Build the FULL property-update payload for a CharacterAppearanceComponent
/// subobject content block.  This is what goes inside the V3 content block's
/// payload region (after [bHasRepLayout=1][bIsActor=0][SIP sub_guid][bStably-
/// Named=1][SIP NumPayloadBits]).
///
/// Wire format (stock UE5, bit 0 of UNetConnection+0xF0 = 0):
///       For each property:
///           [SerializeInt handle, MAX = handle_max]
///           [1 bit bDoChecksum = 0]    (some UE5 versions only)
///           [SIP NumPayloadBits]
///           [<NumPayloadBits> bits property data]
///       [SerializeInt(handle_max-1), MAX = handle_max]   ← end-of-properties
///
/// The two replicated properties on UCharacterAppearanceComponent are:
///       1. CharacterCustomization  (FCharacterCustomizationSaveData struct)
///       2. bForceHideHeldItems      (bool, 1 bit)
///
/// `customization_handle` and `force_hide_handle` are field handles (NetCache
/// indices) — currently unknown, so probe-driven via probe_app_*.txt.
///
/// Returns the number of BITS written to `out_bits`.
uint32_t build_appearance_payload_bits(const CharacterCustomizationData& data,
                                          uint32_t handle_max,
                                          uint32_t customization_handle,
                                          uint32_t force_hide_handle,
                                          std::vector<uint8_t>& out_bits);

}}  // namespace aoc::net
