// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  market/market.h
//
//  The Marketplace Manager and the market's window: the population instance the data places at a town's marketplace
//  building (Node_Vendor_Gear_marketplace), his player interaction (ID_MarketplaceVendor) and the value the client echoes
//  in every later marketplace call (MarketplaceActorGuid).  The Manager is the gear vendor's Blueprint (ANPCCharacter), so
//  his open is the recorded one with the interactable's two ids replaced; the market is the node he stands in.
// ============================================================================
#pragma once

#include "protocol/repl/value.h"
#include "spawns/captured_spawns.h"
#include "world/actor_registry.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace aoc::worldd::market {

/// The Manager's PopulationInstance, InteractableComponentDef and PlayerInteractionDef (the data's records).
inline constexpr int64_t kManagerInstance = 6064632772964253696ll;     // Node_Vendor_Gear_marketplace
inline constexpr int64_t kManagerComponent = 6064633962143220653ll;    // ICD_ID_MarketplaceVendor
inline constexpr int64_t kManagerInteraction = 6064632772967071745ll;  // ID_MarketplaceVendor

/// The steps (purchases and removals) a session holds in line behind the one it has in flight: the Buy panel sends one purchase for each line of its cart in one frame, and the
/// session runs them one at a time, each with all its checks when it starts.  A line past this many is answered 3 (the client's cart has no known limit; 10 is a storefront's whole
/// listings).
inline constexpr size_t kMaxQueuedSteps = 10;

/// The storefronts on one page the window's open sends, and the most it sends (the Refresh button and the search reach the rest): 30 pages stay inside a session's reply queue.
inline constexpr size_t kStorefrontPageSize = 100;
inline constexpr size_t kMaxPushedStorefronts = 30 * kStorefrontPageSize;

/// Whether the NPC is the Marketplace Manager (its instance, or the interaction its data names).
inline bool is_manager(const world::NpcInfo& npc) { return npc.instance_guid == kManagerInstance || npc.interact_def == kManagerInteraction; }
/// The same for a PopulationInstance before its actor is made.
inline bool is_manager_instance(int64_t instance_guid, int64_t interact_response) { return instance_guid == kManagerInstance || interact_response == kManagerInteraction; }

/// The Manager's open: the donor's (the recorded gear vendor, the same ANPCCharacter class) with the interactable component's
/// definition and its first input's PlayerInteractionDef replaced.  nullopt when the donor has no such component.
inline std::optional<spawns::OpenTemplate> manager_open(const spawns::OpenTemplate& donor, int64_t component, int64_t interaction) {
    spawns::OpenTemplate open = donor;
    bool found = false;
    for (spawns::ParsedBlock& b : open.blocks) {
        if (b.cls != "UInteractableComponent" || !b.schema) continue;
        if (!b.state.state.set(*b.schema, "ComponentDefId.Guid", ::aoc::protocol::repl::Value::i64(component))) return std::nullopt;
        const ::aoc::protocol::repl::Value* data = b.state.state.get(*b.schema, "InteractionInputData");
        const auto* arr = data ? data->get_if<::aoc::protocol::repl::ArrayValue>() : nullptr;
        if (!arr || arr->elements.empty()) return std::nullopt;
        ::aoc::protocol::repl::ArrayValue input = *arr;
        input.elements[0].values[6] = ::aoc::protocol::repl::Value::i64(interaction);   // InteractionInputData.PlayerInteractDefinition.Guid
        if (!b.state.state.set(*b.schema, "InteractionInputData", ::aoc::protocol::repl::Value(std::move(input)))) return std::nullopt;
        found = true;
    }
    if (!found) return std::nullopt;
    return open;
}

/// The client's MarketplaceActorGuid for a node: the node's guid words and a fixed market tag, so every login and start
/// gets the same one and two nodes cannot share it.
inline ::aoc::protocol::repl::GuidValue marketplace_guid(int64_t node) {
    ::aoc::protocol::repl::GuidValue g;
    g.a = static_cast<uint32_t>(node);
    g.b = static_cast<uint32_t>(static_cast<uint64_t>(node) >> 32);
    g.c = 0x4D4B5450u;
    g.d = 0x00000001u;
    return g;
}

/// The node a market guid names (the reverse of marketplace_guid); nullopt for a guid that is no market's.
inline std::optional<int64_t> market_node(const ::aoc::protocol::repl::GuidValue& guid) {
    if (guid.c != 0x4D4B5450u || guid.d != 0x00000001u) return std::nullopt;
    const int64_t node = static_cast<int64_t>((static_cast<uint64_t>(guid.b) << 32) | guid.a);
    if (node == 0) return std::nullopt;
    return node;
}

/// The NodeRecordGuid of the actor's UAoCNodePlotComponent (0: none).
inline int64_t node_of(const ::aoc::protocol::repl::Actor& actor) {
    for (const ::aoc::protocol::repl::Subobject& s : actor.subobjects) {
        if (!s.obj.schema || std::string(s.obj.schema->name) != "UAoCNodePlotComponent") continue;
        if (const ::aoc::protocol::repl::Value* v = s.obj.state.get(*s.obj.schema, "NodeRecordGuid")) {
            if (const int64_t* i = v->get_if<int64_t>()) return *i;
        }
    }
    return 0;
}

}  // namespace aoc::worldd::market
