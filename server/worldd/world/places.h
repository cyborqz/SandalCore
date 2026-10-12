// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/places.h  (the GM travel tools)
//
//  The places a GM can send a player to by name: every city node that has a place on the Verra map, "login" (the default spawn), and "garden" (the centre of
//  G1's GM garden, when --gm-garden placed one).  The node's place is the
//  location content.db's city_node table gives it (contentc version 12).  A node is left out when
//    * its location is the origin (0, 0, 0): the table has no place for it (the test nodes, "Mt. Dun", "Open Ocean", the dungeons), or
//    * its location is on another map than Verra (map_name is not "Verra_World_Master" or empty: the 25 or so test levels of the design data, whose
//      coordinates are meaningless in this world: a player sent there falls into the void).
//  A name is matched the way the citynode service and nodectl match a node: the record name ("Verra_RVR_Joeva"), the same without "Verra_" ("RVR_Joeva") or the
//  display name ("Joeva", "New Aela"); case, spaces and punctuation do not count.  "login" (and "login spot", "spawn") is the default spawn.  A name two places
//  answer to is no match.
// ============================================================================
#pragma once

#include "world/city_nodes.h"

#include <string>
#include <vector>

namespace aoc::worldd::world {

struct Place {
    std::string name;           // "Verra_RVR_Joeva", or "login"
    std::string display_name;   // "Joeva"
    std::string kind;           // "node" or "login"
    double      x = 0, y = 0, z = 0;   // cm
};

/// Whether a node has a place a player can be sent to (the header).
bool has_travel_place(const CityNodeInfo& node);

class Places {
public:
    /// `nodes` may be null (a world with no nodes: only "login").  The login spot is where a new character starts.
    static Places build(const CityNodes* nodes, double login_x, double login_y, double login_z);

    const std::vector<Place>& all() const { return places_; }
    /// Adds one more place (the GM garden's centre, once it is placed: kind "garden").
    void add(Place place) { places_.push_back(std::move(place)); }
    /// The place a name stands for; null for none or for a name two places answer to.
    const Place* find(const std::string& name) const;
    /// The nodes the table has no usable place for (counted, for the log).
    size_t nodes_without_place() const { return skipped_; }

private:
    std::vector<Place> places_;
    size_t             skipped_ = 0;
};

/// A name for comparing: lower case letters and digits only ("New Aela" = "NewAela" = "new_aela").
std::string normalize_place_name(const std::string& s);

}  // namespace aoc::worldd::world
