// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/emit/replayout/encoder.h
//
//  IPropertyEncoder — serialises a decoded PropertyValue back to wire bits.
//  One implementation per FPropertyType lives in encoders/<type>.cpp.
//
//  Dispatch: encode_property() routes to the correct encoder based on the
//  ReplicatedPropertyDesc.type.  Callers never instantiate encoders
//  directly — they just pass (desc, value, writer).
//
//  Symmetry with IPropertyDecoder is intentional: every type that has a
//  decoder must also have an encoder, and together they MUST satisfy
//  encode(decode(bits)) == bits for the round-trip validator.
//
//  LAYER:  Protocol / emit / replayout
//  OWNER:  Phase II synthesizer
// ============================================================================
#pragma once

#include "protocol/emit/replayout/catalog.h"
#include "protocol/emit/replayout/property_value.h"
#include "protocol/emit/bunch_writer.h"

namespace aoc { namespace protocol { namespace emit { namespace replayout {

class IPropertyEncoder {
public:
    virtual ~IPropertyEncoder() = default;

    /// Serialise `value` to `writer` per this encoder's type rules.
    /// `desc` provides context (e.g. struct sub-cmds for a struct encoder,
    /// element type for an array encoder).
    ///
    /// Returns true on success.  On failure, `writer` may be in a partial
    /// state — caller should discard and retry from a checkpoint.
    virtual bool encode(const ReplicatedPropertyDesc& desc,
                        const PropertyValue&          value,
                        BunchWriter&                  writer) const = 0;
};

/// Central dispatch.  Looks up the encoder for `desc.type` and delegates.
///
/// Special case: if `value.type == Unknown` and payload holds RawBits,
/// we emit the raw bits verbatim regardless of desc.type.  This is the
/// template-for-unowned-state path that keeps round-trip clean while we're
/// still building out encoders.
bool encode_property(const ReplicatedPropertyDesc& desc,
                     const PropertyValue&          value,
                     BunchWriter&                  writer);

}}}} // namespace aoc::protocol::emit::replayout
