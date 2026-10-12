// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/schema/actor_schema.cpp
// ============================================================================
#include "protocol/schema/actor_schema.h"
#include <unordered_set>
#include <sstream>

namespace aoc { namespace protocol { namespace schema {

ActorSchema::PropLocation ActorSchema::find_by_name(const std::string& name) const {
    PropLocation loc;
    // Search root first
    for (size_t i = 0; i < root_properties.size(); ++i) {
        if (root_properties[i].name == name) {
            loc.component_index = -1;
            loc.property_index = static_cast<int>(i);
            loc.prop = &root_properties[i];
            return loc;
        }
    }
    // Then components, in declaration order
    for (size_t c = 0; c < components.size(); ++c) {
        for (size_t p = 0; p < components[c].properties.size(); ++p) {
            if (components[c].properties[p].name == name) {
                loc.component_index = static_cast<int>(c);
                loc.property_index = static_cast<int>(p);
                loc.prop = &components[c].properties[p];
                return loc;
            }
        }
    }
    return loc;  // not found: prop == nullptr
}

std::string ActorSchema::validate() const {
    std::ostringstream err;
// [C Y B - CoA]

    // Check 1: no handle collisions within root_properties
    std::unordered_set<uint32_t> root_handles;
    for (const auto& p : root_properties) {
        if (p.handle == 0) {
            err << "root property '" << p.name << "' has handle=0 (reserved for terminator)\n";
        }
        if (!root_handles.insert(p.handle).second) {
            err << "root property '" << p.name << "' handle " << p.handle << " collides\n";
        }
    }

    // Check 2: no handle collisions within each component
    for (const auto& comp : components) {
        std::unordered_set<uint32_t> comp_handles;
        for (const auto& p : comp.properties) {
            if (p.handle == 0) {
                err << "component '" << comp.class_name << "' property '" << p.name
                    << "' has handle=0 (reserved for terminator)\n";
            }
            if (!comp_handles.insert(p.handle).second) {
                err << "component '" << comp.class_name << "' property '" << p.name
                    << "' handle " << p.handle << " collides\n";
            }
        }
    }

    // Check 3: no duplicate property NAMES (cross-component) — allowed in UE5 but
    // catches copy-paste errors.  Warn only if explicitly duplicated in root.
    // (Not considered an error; left as a courtesy check.)

    // Check 4: class_name not empty
    if (class_name.empty()) {
        err << "class_name is empty\n";
    }

    return err.str();
}

}}} // namespace aoc::protocol::schema
