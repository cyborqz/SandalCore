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
//
//  The key (crafting, step 1).  A job is saved with the station it is at and must find it again after worldd restarts.  A station is made from a point of the world's data, and the point's
//  seed (a hash of the point's instance, set, placement and key: the data's, never the actor's id, which depends on the order the actors were made in) is the same on every start.  The
//  station's key is that seed in 16 hex digits, and the four words of its StationGuid (what the window and the player's list of active stations name it by) are made from the same seed,
//  so the key and the GUID are one identity.
// ============================================================================
#pragma once

#include "protocol/repl/craft_rpc.h"
#include "spawns/spawn_util.h"
#include "vendor/node_tax_table.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace aoc::worldd::crafting {

/// How close (cm, in three dimensions) a player's last good position must be to a station for its window to open: the interaction's radius (400) and tolerance (30), and a margin for the
/// distance between the station's origin and the point the client measures from and for a pawn that has moved on since the position the server holds.
inline constexpr double kStationRangeCm = 700.0;

/// The four words of a station's StationGuid (A, B, C, D), made from the seed of the point it stands at.
inline std::array<int32_t, 4> station_guid_words(uint64_t seed) {
    std::array<int32_t, 4> w{};
    uint64_t s = seed;
    for (int32_t& x : w) x = static_cast<int32_t>(spawns::util::splitmix(s) >> 16);
    return w;
}

/// The key a station's jobs are saved under: its seed in 16 lower-case hex digits ("" for a station with no seed, which has no key that survives a restart).
inline std::string station_key(uint64_t seed) {
    if (seed == 0) return {};
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(seed));
    return buf;
}

/// The seed a key names; nullopt for text that is not a key.
inline std::optional<uint64_t> station_seed(std::string_view key) {
    if (key.size() != 16) return std::nullopt;
    uint64_t v = 0;
    for (const char c : key) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else return std::nullopt;
        v = (v << 4) | static_cast<uint64_t>(d);
    }
    return v == 0 ? std::nullopt : std::optional<uint64_t>(v);
}

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
