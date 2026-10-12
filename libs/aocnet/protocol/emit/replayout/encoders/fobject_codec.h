// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/emit/replayout/encoders/fobject_codec.h
//
//  FObjectProperty codec.  AoC replaces stock UE5's FNetworkGUID (uint32)
//  with FIntrepidNetworkGUID (16 bytes: uint64 ObjectId + uint32 ServerId +
//  uint32 Randomizer).
//
//  OPEN QUESTION — wire format variant:
//    UE5 stock FObjectProperty::NetSerializeItem writes ONLY the uint32
//    NetGUID index via SerializeIntPacked.  The full GUID lives in an
//    export bunch that's transmitted separately.
//
//    AoC pkt#22 (ActorOpen) has a "bHasRepLayoutExport" flag and carries
//    its exports inline via PackageMapExporter.  We don't yet know if,
//    inside the replicated property payload, object refs are:
//
//      (A) SIP-packed uint64 ObjectId (32-96 bits variable)
//          — matches stock UE5's "index-only in property, GUID in export"
//          split adapted for 64-bit.
//
//      (B) Full 128-bit FIntrepidNetworkGUID inline
//          — matches the "raw struct in archive" pattern seen in
//          FIntrepidNetworkGUID::NetSerialize (verified in
//          sub_14141E960).
//
//    We start with option (B) because:
//      1. It matches the observed `write_intrepid_guid` format in
//         intrepid_netguid.h that our outer bunch framing already uses.
//      2. It's unambiguous (fixed 128 bits, no length variance).
//      3. If round-trip diverges, the failure will be localised to the
//         object property bit range — we'll see exactly which bits
//         changed and can adapt.
//
//  LAYER:  Protocol / emit / replayout / encoders
// ============================================================================
#pragma once

#include "protocol/emit/replayout/property_value.h"
#include "protocol/emit/bunch_writer.h"
#include "protocol/wire/packet_reader.h"

namespace aoc { namespace protocol { namespace emit { namespace replayout {

/// Decode 128 bits from `reader` as an FIntrepidNetworkGUID.  Returns an
/// Object PropertyValue on success, empty value on reader overflow.
PropertyValue decode_fobject(::aoc::protocol::wire::PacketReader& reader);

/// Encode an Object PropertyValue as 128 bits via write_intrepid_guid.
/// Returns true on success.
bool encode_fobject(const PropertyValue& value, BunchWriter& writer);

}}}} // namespace aoc::protocol::emit::replayout
