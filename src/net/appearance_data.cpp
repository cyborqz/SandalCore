// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/appearance_data.cpp  —  Phase D Step 2.1 (2026-05-05)
//
//  See appearance_data.h for design.  This file implements:
//      1. JSON → CharacterCustomizationData parser (tolerant, no external deps)
//      2. CharacterCustomizationData → UE5 wire bytes serializer
//      3. Full property-update payload builder for the V3 content block
// ============================================================================
#include "net/appearance_data.h"
#include "protocol/emit/bunch_writer.h"

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <spdlog/spdlog.h>

namespace aoc { namespace net {

namespace {

// ── JSON helpers (lightweight — match xclient_service.h's style) ─────────────

/// Find "key": followed by a JSON value.  Returns the position of the value's
/// first character, or std::string::npos if not found.
size_t find_json_key(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return std::string::npos;
    pos += needle.size();
    // Skip whitespace
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    return pos;
}

double extract_json_number(const std::string& json, const std::string& key,
                            double default_val) {
    auto pos = find_json_key(json, key);
    if (pos == std::string::npos) return default_val;
    char* end_ptr = nullptr;
    double v = std::strtod(json.c_str() + pos, &end_ptr);
    if (end_ptr == json.c_str() + pos) return default_val;
    return v;
}

int64_t extract_json_int64(const std::string& json, const std::string& key,
                             int64_t default_val) {
    auto pos = find_json_key(json, key);
    if (pos == std::string::npos) return default_val;
    // Strings (e.g. "presetGuid":"0") and numbers both parsed as int64.
    if (pos < json.size() && json[pos] == '"') ++pos;  // skip opening quote
    char* end_ptr = nullptr;
    int64_t v = std::strtoll(json.c_str() + pos, &end_ptr, 10);
    if (end_ptr == json.c_str() + pos) return default_val;
    return v;
}
// (C:Y:B ~ C.O.A)

bool extract_json_bool(const std::string& json, const std::string& key,
                        bool default_val) {
    auto pos = find_json_key(json, key);
    if (pos == std::string::npos) return default_val;
    if (json.compare(pos, 4, "true") == 0)  return true;
    if (json.compare(pos, 5, "false") == 0) return false;
    return default_val;
}

/// PD2.3 (2026-05-05) — extract a JSON string value (between double quotes).
/// Returns empty if the key is absent or the value isn't a quoted string.
std::string extract_json_string(const std::string& json, const std::string& key) {
    auto pos = find_json_key(json, key);
    if (pos == std::string::npos) return {};
    if (pos >= json.size() || json[pos] != '"') return {};
    ++pos;  // past opening quote
    auto end = json.find('"', pos);
    if (end == std::string::npos) return {};
    return json.substr(pos, end - pos);
}

/// PD2.3 — JSON race/gender/class come through as STRINGS ("Male", "Kaelar",
/// "Bard").  These mappers convert them to the numeric enum values UE5
/// expects on the wire.  Per the capture noted in xclient_service.h
/// comments (race ID 7=Dunir, 8=Empyrean, 2=Kaelar) and other empirical
/// observations.  Unknown names map to 0 (sensible default).
uint8_t map_gender_name_to_id(const std::string& name) {
    if (name == "Male")    return 0;
    if (name == "Female")  return 1;
    return 0;
}
uint8_t map_race_name_to_id(const std::string& name) {
    // Per xclient_service.h comments: "race ID (7=Dunir, 8=Empyrean, 2=Kaelar)".
    // Other races inferred from AOC's known roster.  Update as more values
    // are observed in capture logs.
    if (name == "Kaelar")    return 2;
    if (name == "Vaelune")   return 3;
    if (name == "Pyrai")     return 4;
    if (name == "Renkai")    return 5;
    if (name == "Empyreans") return 6;  // alt spelling
    if (name == "Dunir")     return 7;
    if (name == "Empyrean")  return 8;
    if (name == "Niküa")     return 9;
    if (name == "Tulnar")    return 10;
    return 0;
}
uint8_t map_class_name_to_id(const std::string& name) {
    // AOC's 8 archetypes — per the lobby's known classes list.  These IDs
    // are best-guesses by ordinal; if they're wrong the mesh still loads
    // (the SkeletalMesh comes from race+gender, not class) but the
    // class-specific outfit/equipment slot may be wrong.
    if (name == "Fighter")  return 0;
    if (name == "Tank")     return 1;
    if (name == "Mage")     return 2;
    if (name == "Cleric")   return 3;
    if (name == "Rogue")    return 4;
    if (name == "Ranger")   return 5;
    if (name == "Bard")     return 6;
    if (name == "Summoner") return 7;
    return 0;
}

}  // anonymous namespace

// ── Public API ──────────────────────────────────────────────────────────────

bool parse_customization_json(const std::string& json,
                                CharacterCustomizationData& out) {
    if (json.empty()) return false;

    // Sanity check: must look like a JSON object
    auto first = json.find_first_not_of(" \t\r\n");
    if (first == std::string::npos || json[first] != '{') return false;

    out.preset_guid              = extract_json_int64(json,  "presetGuid",              0);
    out.random_seed              = static_cast<int32_t>(extract_json_int64(json, "randomSeed", 0));
    out.skin_color_hue           = static_cast<float>(extract_json_number(json, "skinColorHue",          0.5));
    out.skin_color_pigmentation  = static_cast<float>(extract_json_number(json, "skinColorPigmentation", 0.5));
    out.normal_detail_strength01 = static_cast<float>(extract_json_number(json, "normalDetailStrength01", 0.0));
    out.normal_detail_strength02 = static_cast<float>(extract_json_number(json, "normalDetailStrength02", 0.0));
    out.skin_set                 = static_cast<int32_t>(extract_json_int64(json, "skinSet", 0));

    out.eye_color    = extract_json_int64(json, "eyeColor",    0);
    out.eye_shape    = extract_json_int64(json, "eyeShape",    0);
    out.sclera_shape = extract_json_int64(json, "scleraShape", 0);
    out.eyebrows     = extract_json_int64(json, "eyebrows",    0);

    out.head_hair             = extract_json_int64(json,  "headHair",          0);
    out.head_hair_root_color  = extract_json_int64(json,  "headHairRootColor", 0);
    out.head_hair_tip_color   = extract_json_int64(json,  "headHairTipColor",  0);
    out.head_hair_length      = static_cast<float>(extract_json_number(json, "headHairLength",   0.5));
    out.head_hair_contrast    = static_cast<float>(extract_json_number(json, "headHairContrast", 0.5));
    out.head_hair_gradient    = static_cast<float>(extract_json_number(json, "headHairGradient", 0.5));

    out.facial_hair_lip          = extract_json_int64(json, "facialHairLip",         0);
    out.facial_hair_chin         = extract_json_int64(json, "facialHairChin",        0);
    out.facial_hair_cheek        = extract_json_int64(json, "facialHairCheek",       0);
    out.facial_hair_lip_length   = static_cast<float>(extract_json_number(json, "facialHairLipLength",   0.0));
    out.facial_hair_chin_length  = static_cast<float>(extract_json_number(json, "facialHairChinLength",  0.0));
    out.facial_hair_cheek_length = static_cast<float>(extract_json_number(json, "facialHairCheekLength", 0.0));
    out.eyelash_length           = static_cast<float>(extract_json_number(json, "eyelashLength",         0.5));
    out.facial_hair_root_color   = extract_json_int64(json, "facialHairRootColor",  0);
    out.facial_hair_tip_color    = extract_json_int64(json, "facialHairTipColor",   0);
    out.facial_hair_contrast     = static_cast<float>(extract_json_number(json, "facialHairContrast", 0.5));
    out.facial_hair_gradient     = static_cast<float>(extract_json_number(json, "facialHairGradient", 0.5));

    out.racial_horns        = extract_json_int64(json, "racialHorns",       0);
    out.racial_horns_length = static_cast<float>(extract_json_number(json, "racialHornsLength", 0.0));

    out.nail_color   = extract_json_int64(json, "nailColor",   0);
    out.nail_opacity = static_cast<float>(extract_json_number(json, "nailOpacity", 0.0));

    out.is_helmet_visible     = extract_json_bool(json, "bIsHelmetVisible",   true);
    out.is_cape_visible       = extract_json_bool(json, "bIsCapeVisible",     true);
    out.force_hide_held_items = extract_json_bool(json, "bForceHideHeldItems", false);

    // 2026-05-05 — IDA RE confirmed Race/Gender/Class ARE on the wire as
    // TEnumAsByte (1 byte each).  Read them from JSON.
    //
    // PD2.3 (2026-05-05) — JSON ships these as STRING values
    // ("Male"/"Kaelar"/"Bard"), per extract_create_info in xclient_service.
    // Map names → numeric enum IDs.  Fallback: if a numeric value sneaks
    // through (different lobby version, or fields are quoted ints), the
    // int64 path covers that.
    {
        const std::string g = extract_json_string(json, "gender");
        const std::string r = extract_json_string(json, "race");
        const std::string c = extract_json_string(json, "class");
        if (!g.empty()) out.gender_enum = map_gender_name_to_id(g);
        else            out.gender_enum = static_cast<uint8_t>(extract_json_int64(json, "gender", 0));
        if (!r.empty()) out.race_enum = map_race_name_to_id(r);
        else            out.race_enum  = static_cast<uint8_t>(extract_json_int64(json, "race",   0));
        if (!c.empty()) out.class_enum = map_class_name_to_id(c);
        else            out.class_enum = static_cast<uint8_t>(extract_json_int64(json, "class",  0));
    }

    return true;
}


// ── UE5 wire-format helpers (little-endian, bit-packed) ─────────────────────

namespace {

void write_uint32_le(std::vector<uint8_t>& bits_out, uint32_t v) {
    // Append 32 bits little-endian to a byte-packed buffer (we're building
    // payload as bit-stream below; this helper assumes byte-aligned write).
    bits_out.push_back(static_cast<uint8_t>(v & 0xFF));
    bits_out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    bits_out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    bits_out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

void write_uint64_le(std::vector<uint8_t>& bits_out, uint64_t v) {
    write_uint32_le(bits_out, static_cast<uint32_t>(v & 0xFFFFFFFF));
    write_uint32_le(bits_out, static_cast<uint32_t>((v >> 32) & 0xFFFFFFFF));
}

void write_float_le(std::vector<uint8_t>& bits_out, float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    write_uint32_le(bits_out, u);
}

}  // anonymous namespace

void serialize_customization_to_wire(const CharacterCustomizationData& d,
                                       std::vector<uint8_t>& out) {
    out.clear();
    out.reserve(256);
// (C | O | A | Cyb)

    // Fields in SDK declaration order — UE5 net stream is sequential.
    write_uint64_le(out, static_cast<uint64_t>(d.preset_guid));
    write_uint32_le(out, static_cast<uint32_t>(d.random_seed));
    write_float_le (out, d.skin_color_hue);
    write_float_le (out, d.skin_color_pigmentation);
    write_float_le (out, d.normal_detail_strength01);
    write_float_le (out, d.normal_detail_strength02);
    write_uint32_le(out, static_cast<uint32_t>(d.skin_set));

    write_uint64_le(out, static_cast<uint64_t>(d.eye_color));
    write_uint64_le(out, static_cast<uint64_t>(d.eye_shape));
    write_uint64_le(out, static_cast<uint64_t>(d.sclera_shape));
    write_uint64_le(out, static_cast<uint64_t>(d.eyebrows));

    write_uint64_le(out, static_cast<uint64_t>(d.head_hair));
    write_uint64_le(out, static_cast<uint64_t>(d.head_hair_root_color));
    write_uint64_le(out, static_cast<uint64_t>(d.head_hair_tip_color));
    write_float_le (out, d.head_hair_length);
    write_float_le (out, d.head_hair_contrast);
    write_float_le (out, d.head_hair_gradient);
    // SDK has 4-byte pad after head_hair_gradient — UE5 net serialization
    // skips struct alignment padding (only writes used bytes), so we don't
    // emit the pad here.

    write_uint64_le(out, static_cast<uint64_t>(d.facial_hair_lip));
    write_uint64_le(out, static_cast<uint64_t>(d.facial_hair_chin));
    write_uint64_le(out, static_cast<uint64_t>(d.facial_hair_cheek));
    write_float_le (out, d.facial_hair_lip_length);
    write_float_le (out, d.facial_hair_chin_length);
    write_float_le (out, d.facial_hair_cheek_length);
    write_float_le (out, d.eyelash_length);
    write_uint64_le(out, static_cast<uint64_t>(d.facial_hair_root_color));
    write_uint64_le(out, static_cast<uint64_t>(d.facial_hair_tip_color));
    write_float_le (out, d.facial_hair_contrast);
    write_float_le (out, d.facial_hair_gradient);

    // ── 2026-05-05 SDK-VERIFIED REWRITE ─────────────────────────────────────
    //
    // PRIOR VERSION had 25 bytes of GARBAGE in the wire stream:
    //   • `racial_horns` (u64) + `racial_horns_length` (f32) = 12 bytes —
    //     these fields DO NOT EXIST in FCharacterCustomizationSaveData per
    //     the SDK dump (`GameSystemsPlugin_structs.hpp:11967`).  Whoever
    //     hand-typed this struct (PD2.1) hallucinated them.  Since they were
    //     inserted in the MIDDLE (between facial_hair_gradient and nail_color),
    //     EVERY subsequent field was shifted by 12 bytes, desyncing the
    //     client's parser permanently.
    //   • `bIsHelmetVisible` + `bIsCapeVisible` (1 byte each) — also FICTIONAL.
    //   • `Gender`, `Race`, `Class` + 2 empty TMap lengths (11 bytes total) —
    //     these DO exist in the SDK struct, but they are flagged `RepSkip`,
    //     meaning UE5's default struct net serialization EXCLUDES them from
    //     the wire stream.  Our prior IDA-RE comment (claiming they're on
    //     the wire per off_7FF6C4A83850) was a misreading — that table is
    //     the FULL property descriptor (including RepSkip ones), but the
    //     RepSkip flag on the property gates them out of the net stream.
    //
    // Per SDK ground truth, FCharacterCustomizationSaveData has exactly 32
    // wire-replicated fields (the 5 RepSkip fields go through other paths,
    // notably `RaceGenderAppearanceId` set locally via `SetRace()`).
    //
    // Expected wire size (with empty DecalData + empty DecalBlendGroups):
    //   8(PresetGuid) + 4(RandomSeed) + 4*4(SkinHue/Pig/Norm01/Norm02) +
    //   4(SkinSet) + 4*8(eyes/eyebrows) + 3*8(headhair guids) + 3*4(headhair flts) +
    //   3*8(facialhair guids) + 4*4(facialhair flts) + 2*8(facialhair colors) +
    //   2*4(facialhair contrast/gradient) + 8(nailcolor) + 4(nailopacity) +
    //   4(DecalData TArray len) + 4(DecalBlendGroups TArray len)
    //   = 8+4+16+4+32+24+12+24+16+16+8+8+4+4+4 = 184 bytes  ★
    //
    // THIS is what the client's struct deserializer will consume — exactly
    // the byte layout that matches its in-memory FCharacterCustomizationSaveData
    // up to offset 0xE0 (Gender, RepSkip).

    write_uint64_le(out, static_cast<uint64_t>(d.nail_color));
    write_float_le (out, d.nail_opacity);
    // SDK has 4-byte pad after nail_opacity — NOT on wire (UE5 skips align padding).

    // TArray<FCharacterCustomizationDecalData> DecalData — empty
    // UE5 TArray net format: [int32 NumElements][N × element bytes]
    write_uint32_le(out, 0);   // 0 elements

    // TArray<FCharacterDecalBlendGroup> DecalBlendGroups — empty
    write_uint32_le(out, 0);   // 0 elements

    // ── End of wire stream — RepSkip fields below are EXCLUDED ──
    // Gender, Race, Class, FaceMorphWeightMaps, SectionsValues all carry
    // the RepSkip flag in the SDK and are filled in locally on the client
    // by other code paths (typically `UCharacterAppearanceComponent::SetRace`
    // setting `RaceGenderAppearanceId` from the player's archetype).

    spdlog::info("[AppearanceData] serialized customization: {} bytes "
                 "(32 wire fields, SDK-verified; race/gender/class via SetRace path) "
                 "skin_hue={:.3f} eye={} hair={}",
                 out.size(), d.skin_color_hue, d.eye_color, d.head_hair);
}


// ── Property-update payload builder ─────────────────────────────────────────

uint32_t build_appearance_payload_bits(const CharacterCustomizationData& d,
                                          uint32_t handle_max,
                                          uint32_t customization_handle,
                                          uint32_t force_hide_handle,
                                          std::vector<uint8_t>& out_bits) {
    aoc::protocol::emit::BunchWriter bw(64);
// built by C O A with Cyb

    // ── Phase D Step 2.1 (2026-05-05) — minimal payload ──────────────────────
    //
    // REPLAY DECODER FINDING (replay_decoder.py on captured pkt#78):
    // Each captured subobject content block is only ~130 bits (~16 bytes).
    // Our previous attempt sent 1613 bits (full FCharacterCustomizationSaveData
    // = 408 bytes) — connection timed out because the parser couldn't reconcile.
    //
    // The captured AOC server appears to NOT send the full struct.  Instead it
    // sends a small trigger that makes the client load the lobby-selected
    // character's local saved data.  Mechanism unknown; we test by sending the
    // smallest possible content block and let the client's OnRep do the work.
    //
    // Probe knob `probe_appearance_payload_mode.txt` (default 0):
    //   0 = empty payload (just a content-block touch — relies on client
    //       falling back to local lobby data; ~0 bits payload)
    //   1 = bForceHideHeldItems only (1-bit value, ~12-15 bits total)
    //   2 = full FCharacterCustomizationSaveData — 41 fields, ~200 bytes
    //       (UE5 default per-property struct rep, RE'd 2026-05-05 from
    //       FStructParams at off_7FF6C6697768 + property table at
    //       off_7FF6C4A83850.  STRUCT_NetSerializeNative is OFF so this
    //       struct uses default streaming, no custom NetSerialize)
    //   3 = captured pkt#78 ranger payload (16 bytes, references captured-
    //       only NetGUIDs — black screens since PM117)
    int payload_mode = 0;
    if (std::FILE* fp = std::fopen("probe_appearance_payload_mode.txt", "r")) {
        std::fscanf(fp, "%d", &payload_mode);
        std::fclose(fp);
    }

    // Helper: write SerializeInt(value, max) — variable bit width based on
    // ceil(log2(max)).
    auto serialize_int = [&](uint32_t value, uint32_t max) {
        uint32_t num_bits = 0;
        if (max > 1) {
            uint32_t v = max - 1;
            while (v) { v >>= 1; ++num_bits; }
        }
        if (num_bits == 0) return;
        bw.write_bits(static_cast<uint64_t>(value), static_cast<int>(num_bits));
    };

    if (payload_mode == 0) {
        // Mode 0 — empty payload (just touches the subobject).  ActorBuilder
        // wraps this in a V3 stably-named header anyway, so the client's
        // PackageMap registers the subobject NetGUID and OnRep_CharacterCustomization
        // can fire on whatever local data the client already has.
        // No property updates; payload is 0 bits.
        spdlog::info("[AppearanceData] mode 0 — empty payload (relies on client "
                     "lobby fallback)");
    } else if (payload_mode == 1) {
        // Mode 1 — just bForceHideHeldItems = false.  Smallest meaningful
        // property update: handle bits + SIP(1) + 1-bit value.
        serialize_int(force_hide_handle, handle_max);
        bw.write_sip(1);
        bw.write_bit(d.force_hide_held_items ? 1 : 0);
    } else if (payload_mode == 2) {
        // Mode 2 — full struct (LIKELY BROKEN, kept for diagnostic).
        std::vector<uint8_t> struct_bytes;
        serialize_customization_to_wire(d, struct_bytes);
        const uint32_t struct_bits = static_cast<uint32_t>(struct_bytes.size()) * 8;

        serialize_int(customization_handle, handle_max);
        bw.write_sip(struct_bits);
        bw.write_bit_range(struct_bytes.data(), 0, struct_bits);

        serialize_int(force_hide_handle, handle_max);
        bw.write_sip(1);
        bw.write_bit(d.force_hide_held_items ? 1 : 0);
    } else if (payload_mode == 3) {
        // ────────────────────────────────────────────────────────────────────
        // Mode 3 — CAPTURED BYTES from pkt#78 bunch[2] subobject 7
        // (extracted via tools/extract_pkt78_subobjects.py 2026-05-05).
        //
        // The captured AOC server's payload for UCharacterAppearanceComponent:
        //   126 bits / 16 bytes
        //
        // We don't fully know what these bytes mean (probably a property
        // handle + small NetGUID reference + flags) but they're what the
        // captured client successfully consumed and rendered the captured
        // ranger's mesh from.
        //
        // Note: byte 15 has only 6 valid bits (126 bits total = 15 full
        // bytes + 6 bits).  bw.write_bits respects bit count, so the
        // trailing 2 padding bits in byte 15 are NOT emitted.
        //
        // The captured sub_guid (58) is NOT used here — actor_builder wraps
        // these payload bits in a V3 header with OUR minted sub_guid.  The
        // payload itself is byte-perfect with the capture.
        static const uint8_t kCapturedPayload[16] = {
            0x57, 0x53, 0xe6, 0x4e, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x1e, 0x00, 0x00, 0x00, 0x72, 0x3f, 0x34,
        };
        constexpr uint32_t kCapturedPayloadBits = 126;
        bw.write_bit_range(kCapturedPayload, 0, kCapturedPayloadBits);
        spdlog::warn("[AppearanceData] mode 3 — splicing captured pkt#78 "
                      "appearance bytes (126 bits / 16 bytes verbatim)");
    }
// C:Y:B.C.O.A

    const uint32_t total_bits = static_cast<uint32_t>(bw.bit_pos());

    if (total_bits > 0) {
        out_bits.assign(bw.data(), bw.data() + ((total_bits + 7) / 8));
    } else {
        out_bits.clear();
    }

    spdlog::info("[AppearanceData] property-update payload: {} bits "
                 "(mode={} h_custom={} h_force={} max={})",
                 total_bits, payload_mode, customization_handle,
                 force_hide_handle, handle_max);

    return total_bits;
}

}}  // namespace aoc::net
