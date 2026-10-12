// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/emit/replayout/decoder.h
//
//  IPropertyDecoder — reads wire bits into a PropertyValue.  The mirror
//  image of IPropertyEncoder.  One implementation per FPropertyType lives
//  in encoders/<type>.cpp (same file — encoder + decoder are tightly
//  coupled and validated as a pair).
//
//  Dispatch: decode_property() routes by ReplicatedPropertyDesc.type.
//
//  Fallback: if no decoder is registered for a type (or decoding fails),
//  the dispatcher reads `predicted_bits` raw bits and returns a RawBits
//  PropertyValue.  The encoder will pass those bits through verbatim.
//  This is how the DtRE pipeline stays round-trip clean even before we've
//  written every decoder.
//
//  LAYER:  Protocol / emit / replayout
//  OWNER:  Phase II synthesizer
// ============================================================================
#pragma once

#include "protocol/emit/replayout/catalog.h"
#include "protocol/emit/replayout/property_value.h"
#include "protocol/wire/packet_reader.h"

namespace aoc { namespace protocol { namespace emit { namespace replayout {

class IPropertyDecoder {
public:
    virtual ~IPropertyDecoder() = default;

    /// Read a value of `desc.type` from `reader`, returning it as a
    /// PropertyValue.  On failure, returns an empty PropertyValue
    /// (type == Unknown, monostate payload); reader is left at the
    /// position of the failure.
    virtual PropertyValue decode(const ReplicatedPropertyDesc& desc,
                                 ::aoc::protocol::wire::PacketReader& reader) const = 0;
};

/// Central dispatch.  Looks up the decoder for `desc.type` and delegates.
PropertyValue decode_property(const ReplicatedPropertyDesc& desc,
                              ::aoc::protocol::wire::PacketReader& reader);

/// Read exactly `n_bits` bits and return them as a RawBits PropertyValue.
/// Used as a round-trip-clean fallback for properties whose type is still
/// Unknown in the catalog.  The encoder's `encode_property` handles a
/// RawBits-payload value by emitting the bits verbatim.
PropertyValue decode_raw_bits(::aoc::protocol::wire::PacketReader& reader,
                              size_t n_bits);

}}}} // namespace aoc::protocol::emit::replayout
