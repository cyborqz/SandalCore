// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/bootstrap/character_profile.h
//
//  The single source of truth for every dynamic value that feeds into
//  the 100-packet bootstrap emission.  When a client logs in, the
//  server populates a CharacterProfile from the character database
//  (or XClient session data), then the BootstrapRunner walks the
//  recipe list and each PatchedPacketRecipe reads its values from here.
//
//  Rule: if a field varies per character/per session, it goes here.
//  Static class hashes, archetype CDOs, map paths — NOT here (they're
//  baked into the recipes).
//
//  LAYER:   net / bootstrap
//  OWNER:   Path B
//  SESSION: M2 (post-hybrid-working)
// ============================================================================
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace aoc { namespace net { namespace bootstrap {

/// World coordinates.  AoC uses cm in world space; captured example
/// for Hatemost at Verra spawn: (-5940754, -502674, -7750527) when
/// scaled through the 24-bit offset-binary quantizer.  Those RAW
/// scaled integers are what go on the wire; real-world floats are
/// float_value * 100 (UE5 default quantization scale 100).
struct FVectorScaled {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;

    /// Captured Hatemost spawn — use as a fallback default.
    static constexpr FVectorScaled kCapturedHatemostSpawn() {
        return FVectorScaled{-5940754, -502674, -7750527};
    }
};

struct FRotatorCompressed {
    uint16_t pitch = 0;   // 0 if not serialized
    uint16_t yaw = 0;
    uint16_t roll = 0;
    bool     has_pitch = false;
    bool     has_yaw = false;
    bool     has_roll = false;
};

/// AoC character class IDs discovered via RE.  Extend as more are
/// identified.  Source: docs/re-aoc-client.md Part 2.
enum class AoCArchetype : uint32_t {
    Unknown     = 0,
    Fighter     = 17747,
    Tank        = 17748,
    Rogue       = 17749,
    Ranger      = 17750,
    Mage        = 17751,
    Cleric      = 17752,
    Summoner    = 17753,
    Bard        = 17754,
};
// C-Y-B x C O A

/// AoC races.  Source: docs/re-aoc-client.md.
enum class AoCRace : uint32_t {
    Unknown     = 0,
    Kaelar      = 2,
    Vaelune     = 3,
    Empyrean    = 4,
    Pyrai       = 5,
    Renkai      = 6,
    Vek         = 7,
    Nikua       = 8,
    Dunzenkell  = 9,
    Dunir       = 10,
    Tulnar      = 11,
};

enum class AoCGender : uint32_t {
    Unknown = 0,
    Male    = 1,
    Female  = 2,
};

/// Everything dynamic about a character's spawn.
struct CharacterProfile {
    // ── Identity (replicated on Pawn's CharacterInformationComponent) ──
    std::string   name;              ///< e.g. "MyHero"
    AoCArchetype  archetype     = AoCArchetype::Fighter;
    AoCRace       race          = AoCRace::Kaelar;
    AoCGender     gender        = AoCGender::Male;
    uint32_t      level         = 1;
    uint32_t      adventure_level = 1;
    std::string   title;             ///< optional
    std::string   guild_name;        ///< "" if unguilded

    // ── Appearance ──
    /// 16-float morph array (CharacterCustomization ByteArray).
    /// Captured "Hatemost" session values can be copied when unknown.
    std::array<float, 16> customization{};
    /// Equipped cosmetic item references (AppearanceIDs ByteArray).
    std::vector<uint32_t> appearance_ids;

    // ── World placement ──
    FVectorScaled      spawn_location = FVectorScaled::kCapturedHatemostSpawn();
    FRotatorCompressed spawn_rotation;
    /// `location_max_bits` — the offset-binary quantizer width.
    /// Captured PC uses 24.  If spawn_location fits in fewer bits we
    /// could compress, but 24 is always safe.
    uint8_t            location_bits = 24;

    // ── Stats (initial values; real values come from character DB) ──
    float  max_health     = 100.0f;
    float  cur_health     = 100.0f;
    float  max_mana       = 50.0f;
    float  cur_mana       = 50.0f;
    float  max_stamina    = 100.0f;
    float  cur_stamina    = 100.0f;

    // ── Node citizenship (per-player persistent) ──
    uint64_t citizen_node_id_obj = 0;   // 0 = unaffiliated
    uint32_t citizen_node_id_srv = 0;
    uint32_t citizen_node_id_rnd = 0;

    // ── PlayerState fields ──
    int32_t player_id          = 0;
    std::string platform_name;          // EOS/Steam display name
    std::array<uint8_t, 16> character_guid{};  // 16-byte FGuid

    // ── NetGUIDs allocated at spawn time (populated by NetGUIDAllocator) ──
    // Each FIntrepidNetworkGUID = 128 bits = {ObjectId(64), ServerId(32), Randomizer(32)}.
    // These three fields store the Obj portion; the Srv+Rnd are set by the allocator
    // based on server identity.
    uint64_t pc_netguid_obj             = 0;
    uint64_t pawn_netguid_obj           = 0;
    uint64_t player_state_netguid_obj   = 0;

    /// Convenience: populate with the captured Hatemost values for
    /// testing "does the pipeline emit identical output".  In real
    /// usage the login path fills these from the character DB.
    void fill_from_captured_hatemost() {
        name            = "Hatemost";
        archetype       = AoCArchetype::Fighter;
        race            = AoCRace::Kaelar;
        gender          = AoCGender::Male;
        level           = 1;
        spawn_location  = FVectorScaled::kCapturedHatemostSpawn();
        location_bits   = 24;
        // TODO(Phase D): extract the captured morph floats + appearance IDs
        // from captured_pc_spawn_reassembled.bin and embed as defaults.
    }
};

}}} // namespace aoc::net::bootstrap
