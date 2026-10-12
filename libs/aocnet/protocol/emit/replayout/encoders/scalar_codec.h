// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/emit/replayout/encoders/scalar_codec.h
//
//  Scalar FProperty codecs — bool / int / byte / float / int64 / double.
//  These are all trivial fixed-width primitives in UE5's wire format:
//
//    FBoolProperty       1 bit
//    FByteProperty       8 bits   (also underlies FEnumProperty)
//    FIntProperty        32 bits
//    FInt64Property      64 bits
//    FFloatProperty      32 bits (IEEE 754)
//    FDoubleProperty     64 bits (IEEE 754)
//
//  NOTE on FBoolProperty: the UE5 wire serialiser for a bool property
//  in replication is *just a single bit*.  It is NOT byte-aligned and
//  does NOT use SerializeInt(1).  Writing a byte here is a common
//  mistake that passes unit tests but desyncs when multiple bools are
//  adjacent in a struct.
//
//  NOTE on FIntProperty: UE5 has TWO paths — fixed 32-bit (default) and
//  SerializeIntPacked (when the property is tagged with a specific flag).
//  In practice, replicated int properties on actor classes use fixed 32-bit.
//  We'll use fixed 32-bit until we see counter-evidence in a round-trip
//  diff.
//
//  LAYER:  Protocol / emit / replayout / encoders
// ============================================================================
#pragma once

#include "protocol/emit/replayout/property_value.h"
#include "protocol/emit/bunch_writer.h"
#include "protocol/wire/packet_reader.h"

namespace aoc { namespace protocol { namespace emit { namespace replayout {

// ─── Bool ─────────────────────────────────────────────────────────────
PropertyValue decode_fbool (::aoc::protocol::wire::PacketReader& r);
bool          encode_fbool (const PropertyValue& v, BunchWriter& w);
// == Cyb | C O A ==

// ─── Byte (also used for Enum underlying) ─────────────────────────────
PropertyValue decode_fbyte (::aoc::protocol::wire::PacketReader& r);
bool          encode_fbyte (const PropertyValue& v, BunchWriter& w);

// ─── Int (32-bit) ─────────────────────────────────────────────────────
PropertyValue decode_fint  (::aoc::protocol::wire::PacketReader& r);
bool          encode_fint  (const PropertyValue& v, BunchWriter& w);

// ─── Int64 (64-bit) ───────────────────────────────────────────────────
PropertyValue decode_fint64(::aoc::protocol::wire::PacketReader& r);
bool          encode_fint64(const PropertyValue& v, BunchWriter& w);

// ─── Float (32-bit IEEE) ──────────────────────────────────────────────
PropertyValue decode_ffloat (::aoc::protocol::wire::PacketReader& r);
bool          encode_ffloat (const PropertyValue& v, BunchWriter& w);

// ─── Double (64-bit IEEE) ─────────────────────────────────────────────
PropertyValue decode_fdouble(::aoc::protocol::wire::PacketReader& r);
bool          encode_fdouble(const PropertyValue& v, BunchWriter& w);

}}}} // namespace aoc::protocol::emit::replayout
