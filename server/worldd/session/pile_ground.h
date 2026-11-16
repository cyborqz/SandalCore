// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/pile_ground.h
//
//  Where a pile of dropped items lies when a full bag leaves one behind.  A player's origin is the centre of its capsule; a pile lies on the ground, not at that
//  height, so the pile's z is the floor under the player's feet (the walking map's: a slope, a bridge, a roof, a terrace, a town's street), else the ground (the
//  terrain, or a town's floor) where it is the one the body stands on, else the feet themselves.  For feet at or over the ground the walking map's floors more than 40 cm
//  under it are not looked at: a cave or a cellar under a slope too steep to walk is not the floor of a player who stands on the slope (a player in the cave has feet in it).
// ============================================================================
#pragma once

namespace aoc::worldd {

class PlayerWorld;

/// The height of the ground under a body whose origin is at `body_z`, at (x, y): where a dropped pile lies.
double pile_ground(const PlayerWorld& world, double x, double y, double body_z);

}  // namespace aoc::worldd
