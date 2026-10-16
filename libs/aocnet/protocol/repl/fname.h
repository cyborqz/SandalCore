// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/fname.h  (Gathering stage 3, part "Foliage gathering")
//
//  UE's FName splits a trailing instance number off its text: "InstancedFoliageActor_25600_-41_19_0" travels as the text "InstancedFoliageActor_25600_-41_19" and the
//  number 1 (the suffix plus one; 0 = no suffix), "IntrepidGatherableFISMC_2" as "IntrepidGatherableFISMC" and 3, and a bare "IntrepidGatherableFISMC" as number 0.  The
//  foliage RPCs, FoliageData and the state actors name their owner and component this way [R: capture_foliage.h, every name of the thirteen starts].
//
//    fname_text    the name as UE prints it (FName::ToString): the text, and "_<number - 1>" after it for a number above 0; empty for a hardcoded name (EName)
//    fname_parse   the split of FName(const char*) (FName::ParseNumber): a final "_<digits>" with at least one digit, at most 9, no leading zero (a lone 0 is fine) and not the whole
//                  string becomes Number = digits + 1; any other string is the text with number 0
// ============================================================================
#pragma once

#include "protocol/repl/value.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace aoc { namespace protocol { namespace repl {

inline std::string fname_text(const NameValue& n) {
    if (n.hardcoded) return std::string();
    return n.number > 0 ? n.text + "_" + std::to_string(static_cast<int64_t>(n.number) - 1) : n.text;
}

inline NameValue fname_parse(std::string_view s) {
    const size_t us = s.rfind('_');
    if (us != std::string_view::npos && us > 0 && us + 1 < s.size()) {
        const std::string_view digits = s.substr(us + 1);
        bool ok = digits.size() <= 9 && !(digits.size() > 1 && digits[0] == '0');
        int64_t value = 0;
        for (const char c : digits) {
            if (c < '0' || c > '9') {
                ok = false;
                break;
            }
            value = value * 10 + (c - '0');
        }
        if (ok) return NameValue::plain(std::string(s.substr(0, us)), static_cast<int32_t>(value + 1));
    }
    return NameValue::plain(std::string(s));
}

}}}  // namespace aoc::protocol::repl
