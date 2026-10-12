// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/schema/actor_schema.h
//
//  Serialization schema for replicated actors.  PURE DATA DESCRIPTION —
//  does not contain gameplay logic, conditional replication, or state
//  transitions (per architectural correction 2 in the implementation plan).
//
//  Schemas describe ONLY:
//    - Property name (for logs / debug)
//    - Property handle (wire-format index)
//    - Property type (how to serialize the value)
//    - Optional default-value encoding parameters
//
//  Game rules, RPC handlers, and RepNotify callbacks are the job of
//  `src/game_logic/systems/` and `src/net/opcode_dispatcher`, not schemas.
//
//  LAYER:  Protocol / schema
//  SESSION: B
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aoc { namespace protocol { namespace schema {

enum class ActorType {
    PlayerController,
    Pawn,
    PlayerState,
    NPC,
    Interactable,
    StaticWorld,
    GameState,
    // Future: CaravanActor, NodeActor, LootContainer, ...
    COUNT
};

enum class PropType {
    Bool,       // 1 bit
    UInt8,      // 8 bits
    UInt16,     // 16 bits
    UInt32,     // 32 bits
    UInt64,     // 64 bits
    Int8,
    Int16,
    Int32,
    Int64,
    Float,      // 32-bit IEEE float
    Double,     // 64-bit IEEE float
    FString,    // int32 length + ASCII/UCS-2
    FName,      // SerializeIntPacked
    FVector,    // 3 × float (quantized; scale configurable per-property)
    FRotator,   // 3 × uint16 (compressed short)
    FQuat,
    NetGUID,    // SerializeIntPacked64
    ByteArray,  // length-prefixed raw bytes (fallback for unknown types)
    CustomDelta, // FastArraySerializer (not standard RepLayout)
};

/// Optional type-specific encoding parameters.  Used only for types that
/// need per-property tuning (FVector scale, FString max length, etc.)
struct PropEncodingHints {
    float   fvector_scale = 100.0f;   // UE5 FVector_NetQuantize default
    int32_t fvector_bits  = 20;       // bits per component in quantized vector
    int32_t fstring_max_chars = 512;
    uint32_t serialize_int_max = 0;   // for SerializeInt-typed numeric fields
};
// -- C O A . Cyb --

struct PropertySchema {
    uint32_t handle = 0;
    std::string name;                   // "CharacterName", "PrimaryArchetype", etc.
    PropType type = PropType::UInt32;
    PropEncodingHints hints;

    // Metadata-only flags; consumers interpret:
    bool is_rep_notify = false;         // client fires OnRep_* on change
    bool is_server_only = false;        // this property's value is not part of
                                        //   on-wire replication (server-only state)
};

struct ComponentSchema {
    std::string class_name;             // "CharacterInformationComponent"
    std::string default_blueprint_path; // path the client has pre-loaded

    // PM111 (2026-05-04) — for native subobject registration in V3 content
    // blocks.  When non-empty, the actor builder emits an InternalLoadObject
    // for this class path in the package map exports + a V3 subobject content
    // block targeting the minted sub_guid with this class_guid.
    //
    // Example: "/Script/GameSystemsPlugin.CharacterAppearanceComponent"
    std::string class_path;

    // Content-addressable NetGUID for the class CDO.  Filled in by the
    // emitter when class_path is non-empty (deterministic hash of the path
    // string — same hash on every server/client).
    uint64_t class_netguid = 0;

    std::vector<PropertySchema> properties;
};

struct ActorSchema {
    ActorType type = ActorType::StaticWorld;
    std::string class_name;             // "AAoCPlayerController"
    std::string default_blueprint_path;

    // Static NetGUIDs that are shared across all instances of this actor
    // (class BP, level, etc.).  Dynamic per-instance GUIDs come from
    // NetGuidAllocator at spawn time — NOT stored here.
    uint64_t archetype_netguid = 0;
    uint64_t level_netguid = 0;

    std::vector<PropertySchema> root_properties;      // on the actor itself
    std::vector<ComponentSchema> components;          // subobject components

    /// Convenience: find a property by name (across root + components).
    /// Returns pair (component_index, property_index); component_index == -1
    /// means the property is on the actor root.  Both -1 if not found.
    struct PropLocation {
        int component_index = -1;  // -1 = root
        int property_index = -1;   // -1 = not found
        const PropertySchema* prop = nullptr;
    };
    PropLocation find_by_name(const std::string& name) const;

    /// Sanity: verify no handle collisions within this schema (per correction 2,
    /// schemas must be internally consistent even without runtime validation).
    /// Returns empty string if OK, otherwise description of the violation.
    std::string validate() const;
};

}}} // namespace aoc::protocol::schema
