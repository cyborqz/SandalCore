// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/interaction_state.h  (plan E1, stage 1, fix 3)
//
//  The per-player interaction state of an interactable NPC.  The client offers an interaction (the prompt, then ServerInteractWith
//  InteractableComponent) only when the NPC's UInteractableComponent says so for ITS player, from two places the retail server fills:
//
//    CanInputActivate        a replicated array of bools, one per input index: the server lets that input be activated at all
//    InteractVisibleStateMap / InteractGameplayStateMap
//                            fast arrays keyed by input index (only the inputs that have an interaction definition) whose value is a
//                            GuidToBoolWrapper: the CharacterGuids of the players the interaction is visible / usable for, each true
//
//  Retail lists the players near the NPC (capture 758's and 1030's vendors list other players' guids); the client only looks for its own.  worldd
//  keeps the registry's actor free of any player (it is shared) and gives each connection its own copy of the maps: the ActorView (session/
//  actor_view.h) draws every interactable actor with ITS player's guid in both maps (`with_player`) when it opens the actor for that connection and
//  at every update, so a player never sees another's guid, and nothing is left behind when the session ends (the view's entry goes with it).
//
//  The registry's GM vendors carry `CanInputActivate` true for the four inputs (`allow_inputs`), the same for every player.
// ============================================================================
#pragma once

#include "protocol/repl/replicator.h"

#include <cstdint>
#include <vector>

namespace aoc::worldd::world {

/// The input indexes of the actor's UInteractableComponent that have an interaction definition (the maps' keys), empty when it has no such
/// component or no input with a definition.
std::vector<int64_t> interaction_inputs(const protocol::repl::Actor& actor);

/// True when the actor has an interactable component with at least one input that has a definition.
bool is_interactable(const protocol::repl::Actor& actor);

/// Sets CanInputActivate true for `inputs` entries (the component's four inputs). False when the actor has no such component or the value is refused.
bool allow_inputs(protocol::repl::Actor& actor, int inputs = 4);

/// Adds (or, `on` false, takes) `player` (a CharacterGuid as its four words) in both state maps, true, for each input that has a definition.
/// Idempotent: the same call on the same actor gives the same state, item ids included.  Returns false when the actor has none to change.
bool set_player(protocol::repl::Actor& actor, const protocol::repl::GuidValue& player, bool on);

}  // namespace aoc::worldd::world
