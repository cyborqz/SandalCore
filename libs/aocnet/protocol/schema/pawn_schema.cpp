// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/schema/pawn_schema.cpp
//
//  Character Pawn schema.  This is where the PLAYER'S VISIBLE IDENTITY
//  lives (name, class, race, gender).  Uses 6 subobject components:
//
//    AlignmentComponent
//    InteractInfo
//    CharacterInformationComponent   ← name/class/race/gender here
//    CombatInfo
//    AbilityComponent
//    StatsComponent
//
//  Discovered via Session 2 walker (docs/re-review-2026-04-22.md) on
//  ch=78, 90, 94, 100, 101, 104, 108, 110, 111, 127 — all character
//  actors share this 6-component pattern.
//
// ── 2026-04-24 RE UPDATE (direct binary + pkt#78 fixture) ────────────────
//
//  Confirmed from binary + captured pkt#78:
//    - AoC player pawn is a BLUEPRINT class, not C++.
//    - Archetype leaf path: "Default__PlayerPawn_C"   (FString length 22)
//    - Outer path:          "/Game/ThirdPersonCPP/Blueprints/PlayerPawn"
//                           (FString length 43)
//    - Parent C++ class: ACharacter (44 props total, 11 replicated).
//    - No dedicated AAoCPawn FPropertyParams table exists in the binary
//      (confirmed by exhaustive search of all 6285 pointer tables).
//    - Replication follows ACharacter + APawn + AActor inheritance chain.
//
//  Fixture: src/protocol/tools/captured_pkt_78.bin (816 bytes, raw packet).
//  For byte-identity calibration against captured:
//    1. Extract actor_netguid + archetype/level NetGUIDs from pkt#78 bunch
//       (same methodology as test_pc_spawn_diff).
//    2. Confirm partial-bunch status; if multi-fragment, reassemble first
//       (see reassemble_pc_spawn.py for the pattern).
//    3. Write test_pawn_spawn_diff.cpp and iterate handle/value alignment.
//
//  Current state: ActorBuilder produces 1084 bits (136 bytes) for this
//  schema.  NOT yet bit-compared against captured pkt#78 — the 844-bit
//  gap between our output and captured (6528 bits packet vs 1084 bits
//  our bunch) suggests captured pkt#78 has significant tail we don't
//  yet emit.  Calibration work remaining.
// ============================================================================
#include "protocol/schema/schema_registry.h"

namespace aoc { namespace protocol { namespace schema {

static ComponentSchema build_alignment_component() {
    ComponentSchema c;
    c.class_name = "AlignmentComponent";
    c.default_blueprint_path = "";  // resolved at runtime via NetGUID
    c.properties = {
        {1, "Alignment",              PropType::UInt32, {}, true},
        {2, "CorruptionLevel",        PropType::UInt32, {}, true},
        {3, "CurrentCorruption",      PropType::UInt32, {}, true},
        {4, "FactionId",              PropType::UInt32, {}, true},
    };
    return c;
}

static ComponentSchema build_interact_info_component() {
    ComponentSchema c;
    c.class_name = "InteractInfo";
    c.properties = {
        {1, "bCanInteract",           PropType::Bool,   {}, false},
        {2, "InteractType",           PropType::UInt8,  {}, false},
        {3, "InteractRange",          PropType::Float,  {}, false},
    };
    return c;
}

static ComponentSchema build_character_information_component() {
    ComponentSchema c;
    c.class_name = "CharacterInformationComponent";
    // ── The critical identity fields — confirmed via RE (re-aoc-client.md) ──
    c.properties = {
        // Primary identity
        {1,  "CharacterName",         PropType::FString, {}, true},
        {2,  "PrimaryArchetype",      PropType::UInt32,  {}, true},
        {3,  "CharacterRace",         PropType::UInt32,  {}, true},
        {4,  "CharacterGender",       PropType::UInt32,  {}, true},
        {5,  "CharacterAlignment",    PropType::UInt32,  {}, true},
// (C:Y:B) (CoA)

        // Level / progression
        {6,  "CharacterLevel",        PropType::UInt32,  {}, true},
        {7,  "AdventureLevel",        PropType::UInt32,  {}, true},
        {8,  "CharacterTitle",        PropType::FString, {}, true},

        // Social identity
        {9,  "CharacterGuildName",    PropType::FString, {}, true},
        {10, "CharacterCitizenNodeId", PropType::NetGUID, {}, true},

        // Appearance (the 16-float morph array discovered in pkt 104)
        {11, "CharacterCustomization", PropType::ByteArray, {}, true},
        {12, "AppearanceIDs",         PropType::ByteArray, {}, true},
    };
    return c;
}

static ComponentSchema build_combat_info_component() {
    ComponentSchema c;
    c.class_name = "CombatInfo";
    c.properties = {
        {1, "CombatState",            PropType::UInt8,  {}, true},
        {2, "InCombat",               PropType::Bool,   {}, true},
        {3, "CurrentTarget",          PropType::NetGUID, {}, true},
        {4, "CombatSkills",           PropType::CustomDelta, {}, true},
        {5, "ActiveTargets",          PropType::CustomDelta, {}, true},
    };
    return c;
}

static ComponentSchema build_ability_component() {
    ComponentSchema c;
    c.class_name = "AbilityComponent";
    c.properties = {
        {1, "ActiveAbilities",        PropType::CustomDelta, {}, true},
        {2, "Cooldowns",              PropType::CustomDelta, {}, true},
        {3, "HotbarSlots",            PropType::CustomDelta, {}, true},
        {4, "ChannelingData",         PropType::CustomDelta, {}, true},
    };
    return c;
}

static ComponentSchema build_stats_component() {
    ComponentSchema c;
    c.class_name = "StatsComponent";
    c.properties = {
        {1, "HealthData",             PropType::CustomDelta, {}, true},
        {2, "ManaData",               PropType::CustomDelta, {}, true},
        {3, "StaminaData",            PropType::CustomDelta, {}, true},
        {4, "CurrentHealth",          PropType::Float,  {}, true},
        {5, "MaxHealth",              PropType::Float,  {}, true},
        {6, "CurrentMana",            PropType::Float,  {}, true},
        {7, "MaxMana",                PropType::Float,  {}, true},
        {8, "CurrentStamina",         PropType::Float,  {}, true},
        {9, "MaxStamina",             PropType::Float,  {}, true},
    };
    return c;
}

ActorSchema SchemaRegistry::build_pawn_schema() {
    ActorSchema s;
    s.type = ActorType::Pawn;
    s.class_name = "AAoCCharacter";
    s.default_blueprint_path = "/Game/ThirdPersonCPP/Blueprints/PlayerPawn";
    // Archetype/Level NetGUIDs for Pawn — not same as PC; need to be
    // confirmed via Session C byte-identity testing against a captured Pawn.
    s.archetype_netguid = 0;  // TODO(Session-C): confirm from pkt 79/104
    s.level_netguid     = 10; // shared level

    // ── Root Pawn properties (on the actor itself, not components) ──
    // Most character state lives on the 6 subobject components.
    // Pawn root has movement + actor-level replication.
    s.root_properties = {
        {1, "ActorLocation",          PropType::FVector,  {}, true},
        {2, "ActorRotation",          PropType::FRotator, {}, true},
        {3, "Velocity",               PropType::FVector,  {}, true},
        {4, "AttachmentReplication",  PropType::ByteArray, {}, true},

        // Controller reference (back-pointer to owning PC)
        {5, "Controller",             PropType::NetGUID,  {}, true},
        {6, "PlayerState",            PropType::NetGUID,  {}, true},

        // Vehicle / mount
        {7, "AttachedPawn",           PropType::NetGUID,  {}, true},
        {8, "MountedPawn",            PropType::NetGUID,  {}, true},

        // Status
        {9,  "ActorStatus",           PropType::UInt32,   {}, true},
        {10, "bIsDead",               PropType::Bool,     {}, true},
    };

    // ── Subobject components ──
    s.components = {
        build_alignment_component(),
        build_interact_info_component(),
        build_character_information_component(),  // ← THE identity component
        build_combat_info_component(),
        build_ability_component(),
        build_stats_component(),
    };

    return s;
}

}}} // namespace aoc::protocol::schema
