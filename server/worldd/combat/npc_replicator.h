// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  combat/npc_replicator.h  (plan E1, stage 3, part 1)
//
//  Writes an NPC's combat state into the replicated state of its actor (world/actor_registry.h), the way the retail server replicates it
//  (research/combat 1, 7):
//
//    StatsComponent   the StatRepInt32EveryoneProxy items (Health, MaxHealth, Mana, MaxMana, MoveSpeedMult, the penetrations ...): a stat's float
//                     bits as the engine settled them; Health moves with every hit and is 0 in the frame of a death
//    CombatInfo       bInCombat and ActiveTargets (the one object the NPC fights: obj:0 for none) flip together at the first exchange and clear at the
//                     end; bIsDead is 1 in the frame of the death (the client plays the death from it, no RPC); EliteRank
//
//  Every function runs `modify` on the registry (the version counters the actor view compares) and returns false for an actor that is not held or
//  has no such component.
// ============================================================================
#pragma once

#include "protocol/repl/replicator.h"
#include "stats/character_stats.h"
#include "world/actor_registry.h"

#include <cstdint>

namespace aoc::worldd::combat {

/// Sets the EveryoneProxy stat items of the actor the NPC's stats have a value for, to the stats' current values (bits of the float32 values).  Items the
/// actor opened with are changed; none is added.  Returns how many items changed.
size_t write_stats(world::ActorRegistry& registry, uint64_t actor_id, const stats::CharacterStats& stats);

/// Sets one stat's EveryoneProxy item (false when the actor has no item for the record or it already holds the value).
bool write_stat(world::ActorRegistry& registry, uint64_t actor_id, int64_t record, float value);

/// CombatInfo: bInCombat, bIsDead and ActiveTargets (`target_object` the ObjectId of what it fights, 0 for none).  False when the actor has no CombatInfo.
bool write_combat(world::ActorRegistry& registry, uint64_t actor_id, bool in_combat, bool dead, uint64_t target_object);

/// Gathering stage 3, "Hunting" (the done update of a corpse's hunt, capture 1457 and four more): the actor is dead for its clients: CombatInfo.bIsDead set, Health 0 and a bNotTargetable it was
/// given cleared (never added).  Works on the actor itself, for a caller that is inside the registry's `modify`; what already holds the value changes nothing.  True when something changed.
bool mark_dead(world::WorldActor& actor);

/// CombatInfo.EliteRank.
bool write_elite_rank(world::ActorRegistry& registry, uint64_t actor_id, int rank);

/// The EveryoneProxy item's value for a stat of an actor opened from a template (what the client is told now); false when there is none.
bool read_stat(const world::WorldActor& actor, int64_t record, float& value);

}  // namespace aoc::worldd::combat
