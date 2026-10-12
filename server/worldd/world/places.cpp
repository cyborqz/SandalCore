// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/places.cpp (see places.h)
// ============================================================================
#include "world/places.h"

#include <cctype>
#include <cmath>

namespace aoc::worldd::world {

std::string normalize_place_name(const std::string& s) {
    std::string out;
    for (const unsigned char c : s) {
        if (std::isalnum(c)) out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

bool has_travel_place(const CityNodeInfo& node) {
    if (!std::isfinite(node.x) || !std::isfinite(node.y) || !std::isfinite(node.z)) return false;
    if (node.x == 0.0 && node.y == 0.0 && node.z == 0.0) return false;
    return node.map_name.empty() || node.map_name == "Verra_World_Master";
}
// C-Y-B.CoA

Places Places::build(const CityNodes* nodes, double login_x, double login_y, double login_z) {
    Places out;
    Place login;
    login.name = "login";
    login.display_name = "login";
    login.kind = "login";
    login.x = login_x;
    login.y = login_y;
    login.z = login_z;
    out.places_.push_back(login);
    if (nodes) {
        for (const CityNodeInfo& n : nodes->nodes()) {
            if (!has_travel_place(n)) {
                ++out.skipped_;
                continue;
            }
            Place p;
            p.name = n.name;
            p.display_name = n.display_name;
            p.kind = "node";
            p.x = n.x;
            p.y = n.y;
            p.z = n.z;
            out.places_.push_back(std::move(p));
        }
    }
    return out;
}

const Place* Places::find(const std::string& name) const {
    const std::string want = normalize_place_name(name);
    if (want.empty()) return nullptr;
    const Place* found = nullptr;
    for (const Place& p : places_) {
        std::string bare = p.name;
        if (bare.rfind("Verra_", 0) == 0) bare = bare.substr(6);
        bool hit = normalize_place_name(p.name) == want || normalize_place_name(bare) == want || normalize_place_name(p.display_name) == want;
        if (p.kind == "login" && (want == "loginspot" || want == "spawn")) hit = true;
        if (!hit) continue;
        if (found && found != &p) return nullptr;   // two places answer to it
        found = &p;
    }
    return found;
}

}  // namespace aoc::worldd::world
