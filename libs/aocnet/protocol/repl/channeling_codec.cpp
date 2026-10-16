// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/channeling_codec.cpp
//
//  See channeling_codec.h.
// ============================================================================
#include "protocol/repl/channeling_codec.h"

#include "protocol/repl/codecs.h"

#include <bit>
#include <cmath>

namespace aoc { namespace protocol { namespace repl {

bool channeling_data_fits(const ChannelingData& d) {
    if (d.target.ObjectId == 1) return false;   // the export-flags form, which a property never names
    if (d.timed) return true;
    // a clear has nothing after its fourth word group: a duration or a tool set on it would not read back
    return d.duration == 0.0f && !std::signbit(d.duration) && d.tool_record == 0 && d.tool_guid == GuidValue{};
}

std::optional<ChannelingData> read_channeling_data(wire::PacketReader& r) {
    ChannelingData d;
    auto target = read_object(r);
    if (!target || overran(r)) return std::nullopt;
    d.target = *target;
    d.flags = r.read_uint32();
    const uint32_t active = r.read_uint32();
    d.start_time = std::bit_cast<double>(r.read_uint64());
    const uint32_t timed = r.read_uint32();
    if (overran(r) || active > 1 || timed > 1) return std::nullopt;   // a UBOOL is 0 or 1
    d.active = active != 0;
    d.timed = timed != 0;
    if (!d.timed) return d;
    d.duration = std::bit_cast<float>(r.read_uint32());
    const uint32_t record_flag = r.read_uint32();
    d.tool_record = static_cast<int64_t>(r.read_uint64());
    const uint32_t guid_flag = r.read_uint32();
    d.tool_guid.a = r.read_uint32();
    d.tool_guid.b = r.read_uint32();
    d.tool_guid.c = r.read_uint32();
    d.tool_guid.d = r.read_uint32();
    if (overran(r) || record_flag != 1 || guid_flag != 1) return std::nullopt;   // no capture has either unflagged
    return d;
}
// [Cyb x C | O | A]

bool write_channeling_data(emit::BunchWriter& out, const ChannelingData& d) {
    if (!channeling_data_fits(d) || !write_object(out, d.target)) return false;
    out.write_uint32(d.flags);
    out.write_uint32(d.active ? 1u : 0u);
    out.write_uint64(std::bit_cast<uint64_t>(d.start_time));
    out.write_uint32(d.timed ? 1u : 0u);
    if (!d.timed) return true;
    out.write_uint32(std::bit_cast<uint32_t>(d.duration));
    out.write_uint32(1u);
    out.write_uint64(static_cast<uint64_t>(d.tool_record));
    out.write_uint32(1u);
    out.write_uint32(d.tool_guid.a);
    out.write_uint32(d.tool_guid.b);
    out.write_uint32(d.tool_guid.c);
    out.write_uint32(d.tool_guid.d);
    return true;
}

}}}  // namespace aoc::protocol::repl
