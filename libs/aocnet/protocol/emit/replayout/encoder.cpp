// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/emit/replayout/encoder.cpp
//
//  Central encoder dispatch + RawBits fallback.  Per-type encoders live in
//  encoders/<type>.cpp and are linked into the dispatcher via the switch
//  below (kept in one place so adding a type is a single edit).
//
//  CURRENT STATE: dispatcher compiles; all types currently fall through
//  to the RawBits path because no per-type encoders are wired in yet.
//  This means decode(x) → encode() reproduces `x` bit-for-bit as long as
//  the decoder yields RawBits — which is exactly the first round-trip
//  milestone of Phase II.
//
//  LAYER:  Protocol / emit / replayout
// ============================================================================
#include "protocol/emit/replayout/encoder.h"
#include "protocol/emit/replayout/encoders/fstring_codec.h"
#include "protocol/emit/replayout/encoders/scalar_codec.h"
#include "protocol/emit/replayout/encoders/fobject_codec.h"
#include "protocol/emit/replayout/encoders/fstruct_codec.h"

#include "spdlog/spdlog.h"

namespace aoc { namespace protocol { namespace emit { namespace replayout {

namespace {

/// Write a RawBits payload verbatim to the writer (LSB-first per byte,
/// matching every other UE5 bit stream).
bool encode_raw_bits(const RawBits& rb, BunchWriter& w) {
    for (size_t i = 0; i < rb.bit_len; ++i) {
        size_t byte_idx = i >> 3;
        if (byte_idx >= rb.bytes.size()) {
            spdlog::error("[replayout/encode_raw_bits] RawBits claims {} bits "
                          "but only {} bytes provided",
                          rb.bit_len, rb.bytes.size());
            return false;
        }
        int bit = (rb.bytes[byte_idx] >> (i & 7)) & 1;
        w.write_bit(bit);
    }
    return true;
}

} // namespace

bool encode_property(const ReplicatedPropertyDesc& desc,
                     const PropertyValue&          value,
                     BunchWriter&                  writer) {
    // Fast path: opaque RawBits always win — the decoder's round-trip
    // contract is "preserve these bits exactly".
    if (auto* rb = std::get_if<RawBits>(&value.payload)) {
        return encode_raw_bits(*rb, writer);
    }
// C | O | A captures, Cyb code

    // Per-type dispatch.
    switch (desc.type) {
        case FPropertyType::String: return encode_fstring(value, writer);
        case FPropertyType::Bool:   return encode_fbool  (value, writer);
        case FPropertyType::Byte:
        case FPropertyType::Enum:   return encode_fbyte  (value, writer);
        case FPropertyType::Int:    return encode_fint   (value, writer);
        case FPropertyType::Int64:  return encode_fint64 (value, writer);
        case FPropertyType::Float:  return encode_ffloat (value, writer);
        case FPropertyType::Double: return encode_fdouble(value, writer);
        case FPropertyType::Object: return encode_fobject(value, writer);
        case FPropertyType::Struct: return encode_fstruct(desc, value, writer);

        case FPropertyType::Name:
        case FPropertyType::Text:
        case FPropertyType::SoftObject:
        case FPropertyType::Array:
        case FPropertyType::Map:
        case FPropertyType::Set:
            spdlog::warn("[replayout/encode_property] no encoder yet for type "
                         "{} (property '{}'); value must be RawBits to survive "
                         "round-trip",
                         to_string(desc.type), desc.name);
            return false;

        case FPropertyType::Unknown:
        default:
            spdlog::error("[replayout/encode_property] unknown property type "
                          "for '{}'", desc.name);
            return false;
    }
}

}}}} // namespace aoc::protocol::emit::replayout
