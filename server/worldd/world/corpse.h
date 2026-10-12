// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/corpse.h  (plan E1, stage 3, part 2)
//
//  What a dead NPC's actor gains when its killer may loot it, as the retail server does (research/combat 2): in the update that follows the death, three
//  dynamic subobjects on the corpse's own channel,
//
//    LootComponent          OwnerGuid (the player that may loot), CosmeticData.TopRarity (the best rarity of the loot: a number the client draws the
//                           sparkle from; 7 once it is emptied), ServerLootContainer (the next one)
//    ItemStorageComponent   the loot container the window's events name; it has no replicated state
//    InteractableComponent  ICD_DefaultLoot with the loot interaction (LootPileInteraction) on its first input, whose CurrentInputStates say the
//                           interaction is available (state 2) to the players in StateGuids; when the loot is emptied the state is 0
//
//  and the NPC's UNPCSecondaryInfo (InteractInfo) points at them (LootComponent, LootContainerComponent).  The three blocks are the retail ones (the bits of
//  capture 552's Lancer kill, spawns/corpse_exports.h) with the players' guids and the new objects' ids put in.  The classes' export chains come with them.
//  A dynamic subobject cannot be taken off a client (replicator.h), so a corpse keeps its components until its channel closes.
//
//  Nothing here reads a clock or sends anything: it changes the registry's actor, and the connections that have the NPC open are sent the change by their
//  ActorView.
// ============================================================================
#pragma once

#include "protocol/emit/intrepid_netguid.h"
#include "protocol/repl/value.h"
#include "world/actor_registry.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace aoc::worldd::world {

/// The ObjectIds of a corpse's loot components.
struct CorpseLoot {
    uint64_t loot_component = 0;   // the LootComponent
    uint64_t container = 0;        // the ItemStorageComponent: the loot container the window's storage events name
    uint64_t interactable = 0;     // the InteractableComponent: what ServerInteractWithInteractableComponent names
    protocol::emit::FIntrepidNetworkGUID container_guid;   // the container's full guid (an event names it as an object)
};
// built by C.O.A with Cyb

using Minter = std::function<protocol::emit::FIntrepidNetworkGUID()>;

/// CosmeticData.TopRarity of an emptied loot.
inline constexpr int kLootEmptied = 7;

/// Gives the NPC `npc` its loot components (above): `owner` is the guid of the player the loot is for, `may_loot` the guids of everyone who may take
/// from it (the interaction's StateGuids; the owner first), `top_rarity` the best rarity of what is in it.  nullopt, with `error`, for an NPC that is not
/// held or one that has the components already, or when the retail blocks do not load.
std::optional<CorpseLoot> add_loot(ActorRegistry& registry, uint64_t npc, const Minter& mint, const protocol::repl::GuidValue& owner,
                                   const std::vector<protocol::repl::GuidValue>& may_loot, int top_rarity, std::string& error);

/// A dead player's loot pile (APlayerLootPile, retail: capture 758; research/combat 5): an actor of its own where the player fell, with the same three
/// components a corpse gets (the LootComponent is public: bPublicLoot), holding what the death took from the player's bag.
struct Pile {
    uint64_t  actor = 0;
    CorpseLoot parts;
};

/// Makes the pile at (x, y, z): a new actor of kind Pile in the registry, owned by `player` (its CharacterGuid and name), that `may_loot` may take from
/// (the owner first).  `top_rarity` as add_loot.  nullopt, with `error`, when the retail blocks do not load or the registry refuses the actor.
std::optional<Pile> add_pile(ActorRegistry& registry, const Minter& mint, double x, double y, double z, const protocol::repl::GuidValue& player, const std::string& name,
                             const std::vector<protocol::repl::GuidValue>& may_loot, int top_rarity, std::string& error);

/// The loot is gone: TopRarity 7 and the interaction closed (state 0, nobody in StateGuids).  False for an actor that holds no such components.
bool empty_loot(ActorRegistry& registry, uint64_t npc);

}  // namespace aoc::worldd::world
