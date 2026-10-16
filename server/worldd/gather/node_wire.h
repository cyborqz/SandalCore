// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  gather/node_wire.h  (Gathering stage 2, part "Harvesting")
//
//  What a harvest writes into a resource node's actor of the world's registry (world/actor_registry.h, ActorKind::Gatherable), as the retail server's wire shows it
//  [R: capture 1427, the Daffodil harvest from the interact at packet 61 to the node's last update at 674; 1454 packet 88 for a gathered node's reopen].  The node is one
//  AGatherableActor with one UInteractableComponent; the interaction is its input 0.
//
//    start         the component's InputEffectStatuses[0] = {status 0x08000001, bWasInteractedWith, InteractingPlayer the harvester's pawn} and
//                  PlayerGuidsActiveChanneling = [the harvester's CharacterGuid]  (the node's first open after the interact, packet 67)
//    heartbeat     about every 100 ms (7 of 8 captures: 100.0 to 101.2 ms; 1427: 96 updates in 11.1 s) InputEffectStatuses[0].InputEffectStatus goes up by 3 in its top five bits
//                  (bits 27 to 31, wrapping at 32: 0x08000001, 0x20000001, 0x38000001 ...); the low bits stay 1.  The first one also takes InteractingPlayer off (none) [R: 73]
//    done          one update: bGathered, InputEffectStatuses[0] = {the next counter step, low bits 4, InteractingPlayer the harvester's pawn again}, PlayerGuidsActiveChanneling
//                  empty, CanInputActivate = [false] (an array of four of which only the first is written), CurrentInputStates[0].CurrentState.InteractionState 1 -> 0
//                  (its CachedState stays 1) [R: 674].  A later open of the node carries all of that (1454 packet 88: the status 0xB0000004, InteractingPlayer none as another
//                  client sees it)
//    available     [modelled: no capture shows a node return] everything back to what a fresh node's open holds: bGathered off, the status 0, no one channeling,
//                  CurrentInputStates[0].CurrentState 1, and CanInputActivate = the four inputs allowed.  NOT an empty array: a fresh node's open carries no CanInputActivate at all (the
//                  client keeps its class default, which has entries), and the client's UInteractableComponent::CanInputActivate indexes the array without a bound, so an empty one is an
//                  access violation (Gathering stage 2 test 1: the client died 487 ms after a cancel; the respawn and the stuck-channel release wrote the same).  The only writer of the
//                  property (write_can_input) always writes four entries
//
//  Hunting (Gathering stage 3): the CORPSE of a huntable creature (an ActorKind::Npc whose InteractableComponent is the hunting interaction) channels the same way, on the same component
//  [R: captures 1456 1457 1458 1178 1179, tools/capture/gen_hunting.py], with these differences:
//    start       the same two properties (a counter of 1 in the status word); the corpse is not an AGatherableActor: there is no bGathered, ever
//    heartbeat   about every 67 ms (a game tick of 33 ms, every other one: 204 to 213 in a 13.9 to 14.8 s channel), the counter goes up by 2 each time
//    done        the counter one or two ticks on from the last heartbeat's (one when the done update follows the last heartbeat within 50 ms, else two), phase 4; CanInputActivate [false]; ALL
//                FOUR CurrentInputStates go to 0 (a node's changes the first); the corpse is dead for its clients (CombatInfo.bIsDead, Health 0: combat/npc_replicator.h mark_dead, which
//                changes nothing when the kill wrote them already)
//    reset       a cancelled hunt: the corpse as a living huntable's open has it: the status 0, no one channeling, CanInputActivate four entries (the first true), the states back to 1
//
//  InteractingPlayer is an object reference to the harvester's pawn, which only the harvester's client has: the registry holds none, and the harvester's own actor view
//  shows the pawn (WorldActor::gather, `show_interacting_player`).  The status word's low bits are the phase (1 channeling, 4 done), its top five bits a counter that
//  starts at 1 and goes up by 3 with every update of the harvest, the last (done) update included.
//
//  Every function here changes the actor only; the caller runs it inside ActorRegistry::modify so that the views hear of it.
// ============================================================================
#pragma once

#include "protocol/repl/replicator.h"
#include "world/actor_registry.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace aoc::worldd::gather {

namespace repl = ::aoc::protocol::repl;
// Cyb / C.O.A

/// The interaction input of a node (its InteractableComponent's first).
inline constexpr int kInput = 0;
/// The inputs of an interactable component (its static arrays of four).
inline constexpr size_t kInputs = 4;
/// The phase in the status word's low bits.
inline constexpr uint32_t kPhaseChannel = 1, kPhaseDone = 4;
/// The counter goes up by this much with every update of a harvest.
inline constexpr uint32_t kCounterStep = 3;

/// The InputEffectStatus of update number `step` (0: the start, 1 ... n: the heartbeats, n + 1: the done update) in `phase`.
inline constexpr uint32_t status_word(uint32_t step, uint32_t phase) { return (((1u + kCounterStep * step) & 31u) << 27) | phase; }

/// Who harvests a node.
struct Channeler {
    uint64_t                             player_id = 0;   // the PlayerId
    repl::GuidValue                      character;       // the CharacterGuid
    protocol::emit::FIntrepidNetworkGUID pawn;            // the harvester's pawn
};

/// The start.  False, with `error`, when the actor is no node or its component does not take the state.
bool node_begin(world::WorldActor& node, const Channeler& by, std::string& error);
/// A heartbeat: update number `step` (1 or more).  The first one takes InteractingPlayer off the harvester's view.
bool node_beat(world::WorldActor& node, uint32_t step, std::string& error);
/// The done update: number `step`.  The node is gathered.
bool node_done(world::WorldActor& node, uint32_t step, std::string& error);
/// Available again (a respawn, or a harvest that was cancelled).
bool node_reset(world::WorldActor& node, std::string& error);

// ---- Gathering stage 3, "Hunting": a huntable creature's corpse (see above) -----------------------------------------------------------------------------------------------------------

/// The heartbeat's counter step and period, and the split of the done update's counter (one tick on from a heartbeat younger than kCorpseDoneSplit, else two).
inline constexpr uint32_t kCorpseCounterStep = 2;
inline constexpr std::chrono::milliseconds kCorpseBeat{67}, kCorpseDoneSplit{50};

/// The InputEffectStatus of a corpse's update: the 5-bit counter in the top bits, the phase in the low ones.
inline constexpr uint32_t corpse_status_word(uint32_t counter, uint32_t phase) { return ((counter & 31u) << 27) | phase; }

/// The start: the counter 1.  False, with `error`, when the actor is no NPC or has no InteractableComponent.
bool corpse_begin(world::WorldActor& corpse, const Channeler& by, std::string& error);
/// A heartbeat with the counter `counter`.  The first one takes InteractingPlayer off the harvester's view.
bool corpse_beat(world::WorldActor& corpse, uint32_t counter, std::string& error);
/// The done update with the counter `counter`: the corpse is dead for its clients and its interaction is over.
bool corpse_done(world::WorldActor& corpse, uint32_t counter, std::string& error);
/// A cancelled hunt: the corpse as it was before it.
bool corpse_reset(world::WorldActor& corpse, std::string& error);

/// bGathered of the node.
bool node_gathered(const world::WorldActor& node);
/// The InputEffectStatus of the node's input 0 (0 when it has none).
uint32_t node_status(const world::WorldActor& node);

/// The actor as the connection of the player `player_id` sees it: the harvester's own connection has its pawn in InteractingPlayer while `node.gather.show`.  Returns false (and
/// leaves `actor` alone) when nothing is to be shown to that player.
bool show_interacting_player(const world::WorldActor& node, uint64_t player_id, repl::Actor& actor);

}  // namespace aoc::worldd::gather
