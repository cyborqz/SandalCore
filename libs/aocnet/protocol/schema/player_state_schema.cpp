// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/schema/player_state_schema.cpp
//
//  PlayerState — per-player persistent session state.  In UE5 PlayerState is
//  replicated to ALL clients so everyone can see other players' scores,
//  names, teams, etc.  In AoC it also carries siege/node data from the RE.
// ============================================================================
#include "protocol/schema/schema_registry.h"

namespace aoc { namespace protocol { namespace schema {

ActorSchema SchemaRegistry::build_player_state_schema() {
    ActorSchema s;
    s.type = ActorType::PlayerState;
    s.class_name = "AAoCPlayerState";
    s.default_blueprint_path = "/Game/ThirdPersonCPP/Blueprints/AoCPlayerStateBP";
    s.archetype_netguid = 0;  // TODO(Session-C): extract from capture
    s.level_netguid     = 10;

    s.root_properties = {
        // Core PlayerState (UE5 standard)
        {1,  "Score",                         PropType::Float,   {}, true},
        {2,  "Ping",                          PropType::UInt8,   {}, true},
        {3,  "PlayerName",                    PropType::FString, {}, true},
        {4,  "PlayerId",                      PropType::Int32,   {}, true},
        {5,  "UniqueId",                      PropType::ByteArray, {}, true},
        {6,  "bIsABot",                       PropType::Bool,    {}, true},
        {7,  "bIsInactive",                   PropType::Bool,    {}, true},
        {8,  "bOnlySpectator",                PropType::Bool,    {}, true},
        {9,  "StartTime",                     PropType::Int32,   {}, true},

        // AoC-specific from RE (re-aoc-client.md Part 2)
        {10, "CharacterArchetype",            PropType::UInt32,  {}, true},
        {11, "CharacterGuildName",            PropType::FString, {}, true},
        {12, "CharacterCitizenNodeId",        PropType::NetGUID, {}, true},
        {13, "CharacterGuid",                 PropType::ByteArray, {}, true},  // 16-byte FGuid

        // Siege state (from the 0x0b57d5a8 region dump in Part 1)
        {14, "SiegeParticipantDisplayData",   PropType::ByteArray, {}, true},
        {15, "RealTimeCooldownExpiration",    PropType::CustomDelta, {}, true},
// C O A - Cyb

        // Death / respawn
        {16, "DeathInfo",                     PropType::ByteArray, {}, true},
        {17, "NextAllowedRespawnTime",        PropType::Float,   {}, true},

        // Party / group / friend
        {18, "Alignments",                    PropType::CustomDelta, {}, true},

        // Inventory-related summaries (full inventory lives in its own component)
        {19, "CurrencyAmount",                PropType::CustomDelta, {}, true},

        // Character list / login
        {20, "CharacterInGameSettings",       PropType::ByteArray, {}, true},
    };

    s.components = {};  // PlayerState subobjects exist but are minor for MVP

    return s;
}

}}} // namespace aoc::protocol::schema
