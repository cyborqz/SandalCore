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
//                     and CrowdControlState, the crowd control the creature is under (a snare's flags and strength), in the update that carries the effect
//    CombatInfo       bInCombat and ActiveTargets (the one object the NPC fights: obj:0 for none) flip together at the first exchange and clear at the
//                     end; bIsDead is 1 in the frame of the death (the client plays the death from it, no RPC); EliteRank
//
//  Every function runs `modify` on the registry (the version counters the actor view compares) and returns false for an actor that is not held or
//  has no such component.
// ============================================================================
#pragma once

#include "bootstrap/ability_replication.h"
#include "protocol/repl/replicator.h"
#include "stats/character_stats.h"
#include "world/actor_registry.h"

#include <array>
#include <cstdint>
#include <map>
#include <vector>

namespace aoc::worldd::combat {

/// Sets the EveryoneProxy stat items of the actor the NPC's stats have a value for, to the stats' current values (bits of the float32 values).  Items the
/// actor opened with are changed; none is added.  Returns how many items changed.
size_t write_stats(world::ActorRegistry& registry, uint64_t actor_id, const stats::CharacterStats& stats);

/// Sets one stat's EveryoneProxy item (false when the actor has no item for the record or it already holds the value).
bool write_stat(world::ActorRegistry& registry, uint64_t actor_id, int64_t record, float value);

/// CombatInfo: bInCombat, bIsDead and ActiveTargets (`target` the NetGUID of what it fights, in full: the ObjectId with the ServerId and Randomizer the client knows it by; an empty one for none).
/// False when the actor has no CombatInfo.
bool write_combat(world::ActorRegistry& registry, uint64_t actor_id, bool in_combat, bool dead, const ::aoc::protocol::emit::FIntrepidNetworkGUID& target);
/// The same with the target named by its ObjectId alone (0 for none).
inline bool write_combat(world::ActorRegistry& registry, uint64_t actor_id, bool in_combat, bool dead, uint64_t target_object) {
    return write_combat(registry, actor_id, in_combat, dead, ::aoc::protocol::emit::FIntrepidNetworkGUID::from_u64(target_object));
}

/// Gathering stage 3, "Hunting" (the done update of a corpse's hunt, the retail capture and four more): the actor is dead for its clients: CombatInfo.bIsDead set, Health 0 and a bNotTargetable it was
/// given cleared (never added).  Works on the actor itself, for a caller that is inside the registry's `modify`; what already holds the value changes nothing.  True when something changed.
bool mark_dead(world::WorldActor& actor);

/// CombatInfo.EliteRank.
bool write_elite_rank(world::ActorRegistry& registry, uint64_t actor_id, int rank);

/// What the stats component's CrowdControlState says of a creature: StatusFlags (the status bits of the crowd control it is under, content::kCcSnare ...), SnareValue (the strength of the strongest
/// snare; the speed it keeps is 1 minus it), whether it is rooted by another's hand (bRootedByOther) and whether its client's own movement is overridden, and the animation the client plays (a stun's or
/// a trip's: CurrentAnimationRequest, the record to animate, when it began on the effects' clock, its length and the moment it ends).  A creature that holds none has the defaults: all 0, no animation
/// (255) and an end of -1.
struct CcWire {
    int64_t flags = 0;
    double  snare = 0;
    bool    rooted_by_other = false, override_move = false;
    int     anim_request = 255;
    int64_t anim_record = 0;
    double  anim_start = 0, anim_seconds = 0, anim_end = -1;
    // the push the clients play on the creature (a crowd-control record with the displaced bit): the record, who applied it and where it came from (the four words of a character's guid), when it began
    // on the effects' clock, and the place and facing it works from (the origin cached when it began): none of it for a creature that is not pushed
    int64_t                 push_record = 0;
    std::array<uint32_t, 4> push_instigator = {0, 0, 0, 0}, push_source = {0, 0, 0, 0};
    double                  push_start = 0;
    bool                    has_origin = false;
    double                  origin_x = 0, origin_y = 0, origin_z = 0, origin_yaw_degrees = 0;
};

/// Writes the creature's CrowdControlState.  The creature's clients are told in the update that carries the effect items, so whoever changes its CurrentEffects writes this in the same call.  A field is
/// only written when it differs from what the client holds: one that was never written and is at its default stays out (a root says its flags and the override, not the animation fields).  False when
/// the actor has no such state.
bool write_cc_state(world::ActorRegistry& registry, uint64_t actor_id, const CcWire& wire);

/// The EveryoneProxy item's value for a stat of an actor opened from a template (what the client is told now); false when there is none.
bool read_stat(const world::WorldActor& actor, int64_t record, float& value);

/// Whether the actor's open has a stats component with the EveryoneProxy list (a creature or a person has; a banner, a tent and a quest sparkle have none).
bool has_stats(const world::WorldActor& actor);

/// A CurrentEffects item of a creature, shaped as a player's: the effect and its start on the synchronized clock, duration and tick, its stacks, and the character that applied it
/// (the four words of its guid; the item names it as its instigator and its source).
struct EffectItemWrite {
    bootstrap::EffectItem   item;
    std::array<uint32_t, 4> applier = {0, 0, 0, 0};
};
/// The array's own item id for each effect key the writer was given (the items are changed and removed by it).  One map per creature.
using EffectItemIds = std::map<int64_t, int32_t>;

/// Brings the actor's CurrentEffects array (the stats component's) to a new state: the items of `removed` keys go, then `added` ones are put at the end and `changed` ones are
/// replaced in place; `ids` follows.  False when the actor has no such array.
bool write_effect_items(world::ActorRegistry& registry, uint64_t actor_id, EffectItemIds& ids, const std::vector<int64_t>& removed, const std::vector<EffectItemWrite>& added,
                        const std::vector<EffectItemWrite>& changed);

/// What the actor's CurrentEffects array holds now, in the array's order (empty for an actor without one).
std::vector<EffectItemWrite> read_effect_items(const world::WorldActor& actor);

}  // namespace aoc::worldd::combat
