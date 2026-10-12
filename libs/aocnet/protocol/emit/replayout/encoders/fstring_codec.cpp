// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/emit/replayout/encoders/fstring_codec.cpp
//
//  FString encoder + decoder — see .h for wire format spec.
//
//  Implementation notes:
//    * save_num is a signed int32 written LSB-first via write_int32 /
//      read_uint32.  A negative value triggers the UCS-2 path.
//    * The length INCLUDES the NUL terminator.  For a 10-character ASCII
//      string like "RandomChar", save_num is 11.
//    * NUL byte is always written/read — both paths.
//    * For ASCII, each character is written as uint8 (8 bits).
//    * For UCS-2, each character is written as uint16 LE (16 bits).
//
//  Validated against UE5 FString::NetSerialize at UE 5.2.1 source.
//
//  LAYER:  Protocol / emit / replayout / encoders
// ============================================================================
#include "protocol/emit/replayout/encoders/fstring_codec.h"

#include "spdlog/spdlog.h"

namespace aoc { namespace protocol { namespace emit { namespace replayout {

PropertyValue decode_fstring(::aoc::protocol::wire::PacketReader& reader) {
    // int32 length prefix
    if (reader.overflowed()) return {};
    int32_t save_num = static_cast<int32_t>(reader.read_uint32());
    if (reader.overflowed()) return {};

    // Empty string → return ""
    if (save_num == 0) {
        return PropertyValue::make_string(std::string{});
    }

    const bool is_utf16 = (save_num < 0);
    const int32_t char_count = is_utf16 ? -save_num : save_num;

    // Sanity: AoC bunches are << 2KB; a 2048-char string should trigger
    // an abort rather than allocate wildly.
    constexpr int32_t kMaxFStringChars = 2048;
    if (char_count < 1 || char_count > kMaxFStringChars) {
        spdlog::warn("[replayout/decode_fstring] pathological length "
                     "save_num={} (char_count={}); bailing",
                     save_num, char_count);
        return {};
    }

    std::string out;
    out.reserve(static_cast<size_t>(char_count));
// (Cyb | C.O.A)

    if (!is_utf16) {
        // ASCII path: char_count bytes, last one is NUL terminator.
        for (int32_t i = 0; i < char_count; ++i) {
            uint8_t c = reader.read_uint8();
            if (reader.overflowed()) return {};
            if (i == char_count - 1) {
                // NUL terminator — don't append
                if (c != 0) {
                    spdlog::debug("[replayout/decode_fstring] ASCII path: "
                                  "expected NUL at position {}, got 0x{:02x}",
                                  i, c);
                }
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    } else {
        // UCS-2 LE path: char_count * 2 bytes, last u16 is NUL.
        for (int32_t i = 0; i < char_count; ++i) {
            uint16_t c16 = static_cast<uint16_t>(reader.read_bits(16));
            if (reader.overflowed()) return {};
            if (i == char_count - 1) {
                if (c16 != 0) {
                    spdlog::debug("[replayout/decode_fstring] UCS-2 path: "
                                  "expected NUL at position {}, got 0x{:04x}",
                                  i, c16);
                }
            } else {
                // Lossy downcast — log and replace non-ASCII with '?'.
                if (c16 <= 0x7F) {
                    out.push_back(static_cast<char>(c16));
                } else {
                    out.push_back('?');
                    spdlog::debug("[replayout/decode_fstring] UCS-2 char "
                                  "0x{:04x} at pos {} not representable in "
                                  "std::string, replaced with '?'",
                                  c16, i);
                }
            }
        }
    }

    return PropertyValue::make_string(std::move(out));
}

bool encode_fstring(const PropertyValue& value, BunchWriter& writer) {
    // Expect a String variant.
    const std::string* str = std::get_if<std::string>(&value.payload);
    if (!str) {
        spdlog::error("[replayout/encode_fstring] PropertyValue payload is "
                      "not a string");
        return false;
    }

    // Empty string: save_num = 0, no trailing bytes.
    if (str->empty()) {
        writer.write_int32(0);
        return true;
    }

    // Decide ASCII vs UCS-2 by character range.  Pure-ASCII → ASCII path.
    bool needs_utf16 = false;
    for (unsigned char c : *str) {
        if (c > 0x7F) { needs_utf16 = true; break; }
    }

    const int32_t char_count = static_cast<int32_t>(str->size()) + 1;  // +NUL
    const int32_t save_num   = needs_utf16 ? -char_count : char_count;
    writer.write_int32(save_num);

    if (!needs_utf16) {
        // ASCII: str->size() bytes + NUL
        for (char c : *str) writer.write_uint8(static_cast<uint8_t>(c));
        writer.write_uint8(0);
    } else {
        // UCS-2 LE: each char as u16, + NUL u16
        for (char c : *str) {
            writer.write_bits(static_cast<uint16_t>(static_cast<unsigned char>(c)),
                              16);
        }
        writer.write_bits(0, 16);
    }
    return true;
}

}}}} // namespace aoc::protocol::emit::replayout
