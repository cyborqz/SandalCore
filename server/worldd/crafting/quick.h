// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  crafting/quick.h  (crafting, step 3: "craft", the quick make; header only)
//
//  What the session remembers of the craft a player has under way at a crafting station.  A quick make is not a job: nothing is stored, nothing is taken while it runs, and a craft that does
//  not finish leaves the player's bags and purse as they were.
//
//  The course.  The player presses Craft; the start is checked (rules.h, `quick`) and the craft begins: the client shows its own progress and plays the station's animation, and the server
//  sends nothing.  When the recipe's base duration is over the checks run again on what the player holds now, and the materials, the price and the product move together: the materials
//  leave the bags, the price leaves the purse, the product arrives, the experience is paid, and the client is told in that order.  The craft ends without any of it, and the client is told so
//  (ClientInterruptCraft), when the player moves away from where the craft began, is hurt to death, mounts, leaves the station's range, closes the window or presses Cancel
//  (ServerInterruptStationCraft), or when the station leaves the world.  A player has one craft at a time; a second start while one runs is refused and changes nothing.
//
//  The client's part.  When it sends a start the client marks itself as crafting and refuses a second press ("You are already crafting an item") until the craft ends: by the Add of the
//  product carrying the id of its start request (the ClientTransactionId, in the Add's fourth optional guid), or by ClientInterruptCraft.  A batch ("craft 5") is the client sending the next
//  start itself when the product of the one before arrives.  A start the server refuses therefore also sends ClientInterruptCraft, so that the client does not stay "crafting".
// ============================================================================
#pragma once

#include "crafting/rules.h"
#include "protocol/repl/craft_rpc.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace aoc::worldd::crafting {

/// How far (cm, in three dimensions) a crafter may be from where the craft began before it is interrupted.  A craft needs standing still; this is a fidget's step, not a walk (the same
/// distance a harvest allows).
inline constexpr double kQuickDriftCm = 60.0;

/// The craft a player has under way.
struct QuickMake {
    bool                                   active = false;
    uint64_t                               station = 0;      // the station's actor id
    std::string                            station_name;     // its Blueprint's name, for the log
    std::string                            recipe_name;
    protocol::repl::StartCraft             request;          // the start as the client sent it: the checks run again when the craft ends
    std::chrono::steady_clock::time_point  began{}, ready{};
    double                                 x = 0, y = 0, z = 0;   // where the player stood when it began
};

}  // namespace aoc::worldd::crafting
