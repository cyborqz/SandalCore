// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/emit/replayout/property_type.h
//
//  Enum of UE5 FProperty subclass kinds we care about for replication.
//  Each replicated property on an AAoC* class maps to one of these.
//
//  Derived from UE5 CoreUObject FProperty hierarchy, trimmed to the subset
//  that actually appears in AoC's replicated properties (observed via IDA
//  dumps — see src/protocol/tools/ida_dump_*.idc).
//
//  LAYER:  Protocol / emit / replayout
//  OWNER:  Phase II synthesizer
// ============================================================================
#pragma once

#include <cstdint>
#include <string_view>

namespace aoc { namespace protocol { namespace emit { namespace replayout {

enum class FPropertyType : uint8_t {
    Unknown = 0,

    // ─── Scalar types ─────────────────────────────────────────────────
    Bool,          // FBoolProperty       — single bit
    Byte,          // FByteProperty       — 8 bits
    Int,           // FIntProperty        — 32 bits (sometimes SerializeIntPacked)
    Int64,         // FInt64Property      — 64 bits
    Float,         // FFloatProperty      — 32 bits IEEE
    Double,        // FDoubleProperty     — 64 bits IEEE
    Name,          // FNameProperty       — FName (package-map-exported)
    String,        // FStrProperty        — FString (length + UCS2/ANSI bytes)
    Text,          // FTextProperty       — FText (rare in replication)

    // ─── Reference types ──────────────────────────────────────────────
    Object,        // FObjectProperty     — UObject* (AoC: FIntrepidNetworkGUID)
    SoftObject,    // FSoftObjectProperty — soft reference by path
// built with C Y B and C | O | A

    // ─── Composite types ──────────────────────────────────────────────
    Struct,        // FStructProperty     — recurses into sub_cmds
    Array,         // FArrayProperty      — count + elements
    Map,           // FMapProperty        — (unsupported yet)
    Set,           // FSetProperty        — (unsupported yet)

    // ─── Enum ─────────────────────────────────────────────────────────
    Enum,          // FEnumProperty       — underlying is ByteProperty
};

/// Human-readable name for logging.
constexpr std::string_view to_string(FPropertyType t) {
    switch (t) {
        case FPropertyType::Unknown:    return "Unknown";
        case FPropertyType::Bool:       return "Bool";
        case FPropertyType::Byte:       return "Byte";
        case FPropertyType::Int:        return "Int";
        case FPropertyType::Int64:      return "Int64";
        case FPropertyType::Float:      return "Float";
        case FPropertyType::Double:     return "Double";
        case FPropertyType::Name:       return "Name";
        case FPropertyType::String:     return "String";
        case FPropertyType::Text:       return "Text";
        case FPropertyType::Object:     return "Object";
        case FPropertyType::SoftObject: return "SoftObject";
        case FPropertyType::Struct:     return "Struct";
        case FPropertyType::Array:      return "Array";
        case FPropertyType::Map:        return "Map";
        case FPropertyType::Set:        return "Set";
        case FPropertyType::Enum:       return "Enum";
    }
    return "Invalid";
}

}}}} // namespace aoc::protocol::emit::replayout
