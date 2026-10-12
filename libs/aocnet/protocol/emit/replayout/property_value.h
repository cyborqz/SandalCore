// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/emit/replayout/property_value.h
//
//  Tagged-union value type for a decoded FProperty instance.  One of these
//  is produced by every IPropertyDecoder::decode() and consumed by every
//  IPropertyEncoder::encode() — this is the pivot point of the
//  Decode-then-Re-encode (DtRE) pipeline.
//
//  For struct/array types, the value stores nested PropertyValues so we
//  never need to flatten or lose type info.
//
//  Opaque-bit fallback: `RawBits` lets a decoder yield "I don't know how
//  to interpret these N bits, but preserve them" — so round-trip works
//  even for properties whose encoder isn't written yet.  This is the
//  template-for-unowned-state mechanism Option D depends on.
//
//  LAYER:  Protocol / emit / replayout
//  OWNER:  Phase II synthesizer
// ============================================================================
#pragma once

#include "protocol/emit/replayout/property_type.h"
#include "protocol/emit/intrepid_netguid.h"

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace aoc { namespace protocol { namespace emit { namespace replayout {

class PropertyValue;  // fwd

/// Opaque run of bits preserved verbatim from decode for later re-encode.
/// Used when we don't yet have an encoder for some type — the decoder
/// captures the raw bit range, the encoder writes it back unchanged.
struct RawBits {
    std::vector<uint8_t> bytes;   // LSB-first, as on the wire
    size_t               bit_len = 0;
};

/// A struct value is a flat sequence of its sub-cmd PropertyValues in
/// declaration order.  Nesting is supported — PropertyValue inside a
/// StructValue can itself be another struct.
struct StructValue {
    std::vector<PropertyValue> fields;
};

/// Array of PropertyValues (all same type; enforced by the encoder).
struct ArrayValue {
    std::vector<PropertyValue> elements;
};

using ObjectRef = ::aoc::protocol::emit::FIntrepidNetworkGUID;
// (C-Y-B | CoA)

/// Value payload.  Kind is discriminated by the parent PropertyDesc.type
/// at serialize time; std::variant enforces exhaustive handling.
using ValuePayload = std::variant<
    std::monostate,        // Unknown / empty
    bool,                  // Bool
    uint8_t,               // Byte / Enum(underlying)
    int32_t,               // Int
    int64_t,               // Int64
    float,                 // Float
    double,                // Double
    std::string,           // String (ASCII + UCS2 down-converted)
    ObjectRef,             // Object (FIntrepidNetworkGUID)
    StructValue,           // Struct
    ArrayValue,            // Array
    RawBits                // Opaque-bit fallback
>;

class PropertyValue {
public:
    FPropertyType type = FPropertyType::Unknown;
    ValuePayload  payload;

    PropertyValue() = default;
    PropertyValue(FPropertyType t, ValuePayload p)
        : type(t), payload(std::move(p)) {}

    /// Convenience factories — one per common type.
    static PropertyValue make_bool(bool b) {
        return {FPropertyType::Bool, b};
    }
    static PropertyValue make_int(int32_t v) {
        return {FPropertyType::Int, v};
    }
    static PropertyValue make_string(std::string s) {
        return {FPropertyType::String, std::move(s)};
    }
    static PropertyValue make_object(ObjectRef g) {
        return {FPropertyType::Object, std::move(g)};
    }
    static PropertyValue make_struct(StructValue sv) {
        return {FPropertyType::Struct, std::move(sv)};
    }
    static PropertyValue make_array(ArrayValue av) {
        return {FPropertyType::Array, std::move(av)};
    }
    static PropertyValue make_raw(std::vector<uint8_t> bytes, size_t bit_len) {
        return {FPropertyType::Unknown, RawBits{std::move(bytes), bit_len}};
    }
};

}}}} // namespace aoc::protocol::emit::replayout
