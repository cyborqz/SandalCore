// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  crafting/station.h  (crafting, step 0: "the stations stand and open")
//
//  What worldd knows of a crafting or processing station that is not in the actor (world/actor_registry.h, StationInfo): how close a player must be for the window to open,
//  and the service tax the window is sent.
//
//  The window.  The client opens it from its own copy of the station's Blueprint and design data: the recipes of the station's sets, the prices, the player's certification and bags.  The
//  server answers a click with ClientOpenCraftingStationUI(station actor, tax, jobs); this step sends no jobs.
//
//  The tax.  A station's definition names a tax tag ("NodeTax.ArtisanTax.WoodshopBaseTax.ServiceTax.LumberMillServiceTax"); the window is sent the row of the node's tax table that
//  tag finds, climbing the tag tree to the first row (the shops' rule, vendor/town_shops.h).  A node nobody has changed holds the default rows: the TaxDefaultValue record, 50 percent of the
//  price, the owning node keeping all of it; a station with no tag or a tag the table does not hold gets the root row's numbers.  Both flags are set, as in every window the retail
//  server opened.
// ============================================================================
#pragma once

#include "protocol/repl/craft_rpc.h"
#include "vendor/node_tax_table.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace aoc::worldd::crafting {

/// How close (cm, in three dimensions) a player's last good position must be to a station for its window to open: the interaction's radius (400) and tolerance (30), and a margin for the
/// distance between the station's origin and the point the client measures from and for a pawn that has moved on since the position the server holds.
inline constexpr double kStationRangeCm = 700.0;

/// The window's tax for a station whose definition names `tax_tag` ("" for none), from the default node table.
inline protocol::repl::StationTax station_tax(std::string_view tax_tag) {
    namespace tt = vendor::tax_table;
    const tt::Row* found = nullptr;
    std::string t(tax_tag);
    while (!t.empty() && !found) {
        for (const tt::Row& r : tt::kRows) {
            if (t == r.tag) {
                found = &r;
                break;
            }
        }
        const size_t dot = t.rfind('.');
        if (dot == std::string::npos) break;
        t.resize(dot);
    }
    if (!found) found = &tt::kRows[0];   // the root row ("NodeTax")
    protocol::repl::StationTax out;
    out.record = found->details;
    out.flat = found->flat_tax;
    out.percent = found->tax_percent;
    out.owning_share = found->owning_node_percent;
    out.unlocked = true;
    out.use_override = true;
    return out;
}

}  // namespace aoc::worldd::crafting
