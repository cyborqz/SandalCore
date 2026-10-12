// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/emit/replayout/decoder.cpp
//
//  Central decoder dispatch.  Mirrors encoder.cpp.
//
//  Fallback strategy: if we don't yet have a decoder for `desc.type`,
//  the caller must provide the bit-length of this property separately
//  (because without type knowledge we can't know how many bits to read).
//  For now the dispatcher returns an empty PropertyValue and logs a
//  warning — callers that hit this path should be using the
//  `decode_raw_bits` helper directly instead, until per-type decoders
//  are written.
//
//  LAYER:  Protocol / emit / replayout
// ============================================================================
#include "protocol/emit/replayout/decoder.h"
#include "protocol/emit/replayout/encoders/fstring_codec.h"
#include "protocol/emit/replayout/encoders/scalar_codec.h"
#include "protocol/emit/replayout/encoders/fobject_codec.h"
#include "protocol/emit/replayout/encoders/fstruct_codec.h"

#include "spdlog/spdlog.h"

namespace aoc { namespace protocol { namespace emit { namespace replayout {

PropertyValue decode_property(const ReplicatedPropertyDesc& desc,
                              ::aoc::protocol::wire::PacketReader& reader) {
    switch (desc.type) {
        case FPropertyType::String: return decode_fstring(reader);
        case FPropertyType::Bool:   return decode_fbool  (reader);
        case FPropertyType::Byte:
        case FPropertyType::Enum:   return decode_fbyte  (reader);
        case FPropertyType::Int:    return decode_fint   (reader);
        case FPropertyType::Int64:  return decode_fint64 (reader);
        case FPropertyType::Float:  return decode_ffloat (reader);
        case FPropertyType::Double: return decode_fdouble(reader);
        case FPropertyType::Object: return decode_fobject(reader);
        case FPropertyType::Struct: return decode_fstruct(desc, reader);
// (C-Y-B | C O A)

        case FPropertyType::Name:
        case FPropertyType::Text:
        case FPropertyType::SoftObject:
        case FPropertyType::Array:
        case FPropertyType::Map:
        case FPropertyType::Set:
            spdlog::warn("[replayout/decode_property] no decoder yet for type "
                         "{} (property '{}'); returning empty value. "
                         "Use decode_raw_bits(n_bits) for now.",
                         to_string(desc.type), desc.name);
            return PropertyValue{};

        case FPropertyType::Unknown:
        default:
            spdlog::error("[replayout/decode_property] unknown property type "
                          "for '{}'", desc.name);
            return PropertyValue{};
    }
}

/// Helper: read exactly `n_bits` bits from `reader` and package them as a
/// RawBits PropertyValue.  Used during catalog bootstrapping — a decoder
/// for an unsupported type can call this to yield a round-trip-clean
/// placeholder.
PropertyValue decode_raw_bits(::aoc::protocol::wire::PacketReader& reader,
                              size_t n_bits) {
    RawBits rb;
    rb.bit_len = n_bits;
    rb.bytes.assign((n_bits + 7) / 8, 0);
    for (size_t i = 0; i < n_bits; ++i) {
        int bit = reader.read_bit();
        if (bit) rb.bytes[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
    }
    PropertyValue v;
    v.type = FPropertyType::Unknown;
    v.payload = std::move(rb);
    return v;
}

}}}} // namespace aoc::protocol::emit::replayout
