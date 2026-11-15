// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/shown_to_others.h  (Social, seeing other players: riders on their mounts)
//
//  What the world's registry knows about an actor that only the players who do NOT own it are sent.  A summoned mount is one registry actor (ActorKind::Mount) in its OWNER's form: while
//  the owner rides it, its replicated state says what the rider's own client needs (a possessed actor: RemoteRole 2, Owner, Controller, PlayerState, the rider's stats, the dash) and
//  no ReplicatedMovement, because the rider's client moves it itself.  Every other connection is sent the form session/shown_to_others.h makes from it: a simulated proxy that
//  moves, with the motion kept here.
//
//  Written by the rider's session (session/player_shown.cpp) once a tick while the owner rides, and by the possession (spawns/mount_state.cpp); read by the actor view.
// ============================================================================
#pragma once

#include <cstdint>

namespace aoc::worldd::world {

/// A mount as the players who do not own it are shown it.
struct MountShown {
    /// The owner rides it now: the actor's own state is the owner's form, and the motion below stands for the one the actor does not hold.
    bool possessed = false;
    /// It has been ridden at least once: the owner's form holds what only the owner is sent (its stat arrays, its dash), which the other players' form leaves out.
    bool rode = false;
    /// The mount's ground velocity (cm/s) and whether its rider holds a move input, from the moves the rider's client sent (the TimeStamps of the moves, as a walking pawn's).
    double vx = 0, vy = 0, vz = 0;
    bool   moving = false;
    /// ReplicatedMovementMode (1 walking, 3 falling, 4 swimming, 5 flying) of the last move.
    uint8_t mode = 1;
    /// ReplicatedServerLastTransformUpdateTimeStamp: the world's clock at the last motion written.
    double stamp = 0;
};

}  // namespace aoc::worldd::world
