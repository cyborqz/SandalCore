// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/shown_to_others.h  (Social, seeing other players)
//
//  WHERE "what another player is sent" lives.  A player's character reaches the players around it through the world's registry, in two kinds of actor:
//
//    its pawn       the registry's Player actor (ActorKind::Player), the pawn as a simulated proxy shows it: built once the phase-in is out (bootstrap/observer_actor.cpp), kept where the
//                   player is and in line with what the player does by its own session, once a tick, in the first part of the server's tick (session/player_presence.cpp: the place and the
//                   walk; session/player_emotes.cpp: the emotes; session/player_shown.cpp: the mount it has out and the mount it rides, the held items and the look, the weapon set in hand, the
//                   level with the level-up call the game multicasts, the live Health, MaxHealth, Mana and MaxMana, the death bit and the combat stance (the draw, the fight, the PvP flag and
//                   the name colour it gives).  Every part of seeing other players writes into this pawn through PlayerSession::show_in_pawn.  Modelled: no recording shows another player's
//                   pawn receive these, so the shape is the owner's own reply's, kept current on the pawn the watchers hold; the inactive weapon pair's back placement is animation, not replicated data.
//    its mount      the registry's Mount actor (ActorKind::Mount), summoned and ridden by its owner's session in the OWNER's form (session/mounts.h, session/riding.h).  Every other player's
//                   view is sent the form below: the same actor, as a simulated proxy that moves.
//
//  The actor view asks `form_for_others` for an actor whose owner is not the connection's player.  It is a function of the registry's actor alone: no clock, no session, so the views can
//  call it on any thread while the registry is frozen.  Retail replicates one mount to every connection and lets the replication conditions decide what each is sent (the owner-only
//  properties, the owner's autonomous role, no ReplicatedMovement for the autonomous proxy); the registry's mount is the owner's form, so the other form is made from it here.
// ============================================================================
#pragma once

#include "protocol/repl/replicator.h"
#include "world/actor_registry.h"

namespace aoc::worldd {

/// The actor as a connection of another player is sent it: `a.actor` itself when nothing differs, else `scratch`, made from it.
///
/// A mount that its owner has ridden (WorldActor::mount_shown.rode):
///   - its stat arrays for the owner (StatRepInt32Owner, StatRepInt32OwnerProxy), the dash in its AbilityComponent (KnownAbilities, Cooldowns) are left out;
///   - while the owner rides it (mount_shown.possessed): RemoteRole 1 (a simulated proxy), no Owner, Controller or PlayerState (objects only the owner's connection has), and its
///     ReplicatedMovement, timestamp, movement mode and move input from the motion the rider's session keeps (the actor's own say nothing: the rider's client moves it), also as the place
///     it opens at.
const protocol::repl::Actor& form_for_others(const world::WorldActor& a, protocol::repl::Actor& scratch);

/// The movement mode another player's client is shown for a character: a mode a character moves in (walking, falling, swimming, flying and the custom modes 16, 17 and 18); a move's mode is
/// the byte the client sent and the move checker keeps it as sent, so a modified client could put any number into every nearby client's copy of its pawn.  The client's 0 (none) and any other
/// number are shown as walking.
uint8_t shown_movement_mode(uint8_t mode);

}  // namespace aoc::worldd
