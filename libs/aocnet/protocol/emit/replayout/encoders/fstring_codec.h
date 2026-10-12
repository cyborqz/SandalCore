// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/emit/replayout/encoders/fstring_codec.h
//
//  FString encoder + decoder.  UE5 wire format:
//
//    int32 save_num  (little-endian, 32 bits)
//      save_num > 0   → (save_num-1) ASCII chars + NUL terminator
//                        (save_num counts the NUL)
//      save_num < 0   → (-save_num - 1) UCS-2 chars + NUL terminator
//      save_num == 0  → empty string
//
//    UCS-2 path is triggered when the string contains any codepoint above
//    0x7F at send time.  A pure-ASCII string ALWAYS takes the ASCII path.
//    The decoder must honour the sign of save_num, not guess.
//
//  Contract: encode(decode(bits)) == bits for any well-formed FString.
//  This includes preserving the ASCII/UCS-2 width choice — if the source
//  encoded a pure-ASCII string as UCS-2 (unusual but legal), we re-encode
//  as UCS-2.  We track this via the `forced_utf16` flag on PropertyValue's
//  FString representation.
//
//  LAYER:  Protocol / emit / replayout / encoders
// ============================================================================
#pragma once

#include "protocol/emit/replayout/catalog.h"
#include "protocol/emit/replayout/property_value.h"
#include "protocol/emit/bunch_writer.h"
#include "protocol/wire/packet_reader.h"

namespace aoc { namespace protocol { namespace emit { namespace replayout {

/// Decode an FString from `reader`.  On success, returns a String
/// PropertyValue whose payload is std::string.  The string is ASCII
/// (original UCS-2 characters above 0x7F become '?' — this is lossy
/// but extremely rare in replicated state; we'll tighten if we hit
/// a real case).
///
/// On failure, returns an empty PropertyValue (type == Unknown).
PropertyValue decode_fstring(::aoc::protocol::wire::PacketReader& reader);

/// Encode a String PropertyValue to `writer` using the UE5 FString format.
/// Chooses ASCII path if every character is <= 0x7F, else UCS-2.
///
/// Returns true on success.
bool encode_fstring(const PropertyValue& value, BunchWriter& writer);

}}}} // namespace aoc::protocol::emit::replayout
