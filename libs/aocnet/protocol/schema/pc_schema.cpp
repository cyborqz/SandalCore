// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/schema/pc_schema.cpp
//
//  PlayerController schema.  Properties sourced from:
//    - docs/re-review-2026-04-22.md (276 OnRep_* catalog)
//    - docs/re-aoc-client.md Parts 1 + 2 (RE findings)
//    - Session 2 walker output (ch=3 identified as PlayerController)
//
//  HANDLE ASSIGNMENT NOTE:
//  The property handles below are our OWN ordering — they don't (yet)
//  correspond to AoC's exact wire handles.  Session C's byte-identity
//  test will pin them down.  For now we use sequential 1..N.
// ============================================================================
#include "protocol/schema/schema_registry.h"

namespace aoc { namespace protocol { namespace schema {

ActorSchema SchemaRegistry::build_pc_schema() {
    ActorSchema s;
    s.type = ActorType::PlayerController;
    s.class_name = "AAoCPlayerController";
    s.default_blueprint_path = "/Game/ThirdPersonCPP/Blueprints/AoCPlayerControllerBP";
    s.archetype_netguid = 120;  // captured Archetype NetGUID from pkt 22 RE
    s.level_netguid     = 10;   // captured Level NetGUID

    // ── Root PC properties ──
    // Core actor replication
    s.root_properties = {
        // Identity references (NetGUIDs to owned subobjects)
        {1,  "PlayerState",              PropType::NetGUID, {}, true},
        {2,  "Pawn",                     PropType::NetGUID, {}, true},

        // GM / admin flags (discovered via RE, see re-aoc-client.md Part 2)
        {3,  "bIsGM",                    PropType::Bool,    {}, true},
        {4,  "bIsDev",                   PropType::Bool,    {}, true},

        // Role / network owner
        {5,  "RemoteRole",               PropType::UInt8,   {}, true},
        {6,  "bNetOwner",                PropType::Bool,    {}, false},

        // View target
        {7,  "ViewTarget",               PropType::NetGUID, {}, true},

        // Input / camera
        {8,  "PlayerCameraManager",      PropType::NetGUID, {}, true},
        {9,  "ControlRotation",          PropType::FRotator, {}, false},

        // Spectator mode
        {10, "bIsSpectator",             PropType::Bool,    {}, true},
        {11, "SpectatorState",           PropType::NetGUID, {}, true},
// built with Cyb and C O A

        // Session state
        {12, "PlayerIndex",              PropType::Int32,   {}, true},

        // AoC-specific from re-aoc-client.md
        {13, "CharacterArchetype",       PropType::UInt32,  {}, true},
        {14, "CharacterGuildName",       PropType::FString, {}, true},
        {15, "CharacterCitizenNodeId",   PropType::NetGUID, {}, true},

        // Combat settings reference
        {16, "CombatSettings",           PropType::NetGUID, {}, true},
    };

    s.components = {};  // PC itself does not have subobject components on its own channel
                        // (its subobjects live on the Pawn)
    return s;
}

}}} // namespace aoc::protocol::schema
