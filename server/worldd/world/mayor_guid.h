// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/mayor_guid.h: the FGuid a node's mayor's projects carry (a buy order's guid, a running commission's InstanceGuid)
// ============================================================================
#pragma once

#include <cstdint>

namespace aoc::worldd::world {

/// An FGuid: a buy order's or a running commission's InstanceGuid.
struct MayorGuid {
    uint32_t a = 0, b = 0, c = 0, d = 0;
    bool zero() const { return (a | b | c | d) == 0; }
    bool operator==(const MayorGuid&) const = default;
};

}  // namespace aoc::worldd::world
