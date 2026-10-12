// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/callout_codec.cpp
//
//  See callout_codec.h.
// ============================================================================
#include "protocol/repl/callout_codec.h"

#include "protocol/repl/codecs.h"

namespace aoc { namespace protocol { namespace repl {

bool localized_message_fits(const LocalizedMessage& m) { return fstring_fits(m.string_table_id) && fstring_fits(m.key); }

std::optional<LocalizedMessage> read_localized_message(wire::PacketReader& r) {
    LocalizedMessage m;
    auto table = read_fstring(r);
    if (!table || overran(r)) return std::nullopt;
    auto key = read_fstring(r);
    if (!key || overran(r)) return std::nullopt;
    const uint32_t args = r.read_uint32();
    if (overran(r) || args != 0) return std::nullopt;   // an argument's layout is in no capture
    m.string_table_id = std::move(*table);
    m.key = std::move(*key);
    return m;
}

bool write_localized_message(emit::BunchWriter& out, const LocalizedMessage& m) {
    if (!localized_message_fits(m)) return false;
    if (!write_fstring(out, m.string_table_id) || !write_fstring(out, m.key)) return false;
    out.write_uint32(0);   // FormatArgs.Num
    return true;
}

}}}  // namespace aoc::protocol::repl
