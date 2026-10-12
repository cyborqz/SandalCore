// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/emit/replayout/encoders/fobject_codec.cpp
//
//  FObject codec — uses FIntrepidNetworkGUID 128-bit inline layout.
//  See .h for rationale (we start with option B, 128-bit inline).
// ============================================================================
#include "protocol/emit/replayout/encoders/fobject_codec.h"
#include "protocol/emit/intrepid_netguid.h"

#include "spdlog/spdlog.h"

namespace aoc { namespace protocol { namespace emit { namespace replayout {

PropertyValue decode_fobject(::aoc::protocol::wire::PacketReader& reader) {
    // Read 128 bits as 4 × uint32 LSB-first.
    if (reader.overflowed()) return {};
    const uint32_t obj_lo = reader.read_uint32();
    if (reader.overflowed()) return {};
    const uint32_t obj_hi = reader.read_uint32();
    if (reader.overflowed()) return {};
    const uint32_t server_id = reader.read_uint32();
    if (reader.overflowed()) return {};
    const uint32_t randomiser = reader.read_uint32();
    if (reader.overflowed()) return {};

    ::aoc::protocol::emit::FIntrepidNetworkGUID g;
    g.ObjectId  = static_cast<uint64_t>(obj_lo) |
                  (static_cast<uint64_t>(obj_hi) << 32);
    g.ServerId  = server_id;
    g.Randomizer = randomiser;

    return PropertyValue::make_object(g);
}

bool encode_fobject(const PropertyValue& value, BunchWriter& writer) {
    const auto* g = std::get_if<::aoc::protocol::emit::FIntrepidNetworkGUID>(
        &value.payload);
    if (!g) {
        spdlog::error("[replayout/encode_fobject] payload is not "
                      "FIntrepidNetworkGUID");
        return false;
    }
    ::aoc::protocol::emit::write_intrepid_guid(writer, *g);
    return true;
}

}}}} // namespace aoc::protocol::emit::replayout
