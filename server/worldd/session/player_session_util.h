// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_session_util.h
//
//  File-scope helpers that more than one of the player session's files use (the quest calls' values, bags, stacks of items, the dialogue range ...).
// ============================================================================
#pragma once

#include "session/player_session.h"

#include "bootstrap/character_setup.h"
#include "combat/combat_world.h"
#include "crafting/job.h"
#include "crafting/rewards.h"
#include "crafting/rules.h"
#include "crafting/station.h"
#include "content/content_db.h"
#include "dialogue/talk.h"
#include "protocol/repl/ability_rpc.h"
#include "protocol/repl/dialogue_rpc.h"
#include "world/city_nodes.h"
#include "protocol/repl/hit_rpc.h"
#include "protocol/repl/item_rpc.h"
#include "protocol/repl/move_packed.h"
#include "protocol/repl/storage_rpc.h"
#include "session/gm_travel.h"
#include "protocol/repl/rpc_fields.h"
#include "items/acquired.h"
#include "vendor/node_tax.h"
#include "vendor/sell_rules.h"
#include "quest/quest_wire.h"
#include "quest/registry_points.h"
#include "world/zoi_surface.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <map>
#include <random>
#include <set>

namespace aoc { namespace worldd { namespace session_util {

namespace repl = ::aoc::protocol::repl;
namespace schema = ::aoc::protocol::schema;
namespace wire = ::aoc::protocol::wire;
using protocol::emit::FIntrepidNetworkGUID;

/// The storages a player may take items out of for good (destroy) or sell from: the item bag and the material bags.
inline bool is_bag(svc::ItemContainer c) { return c == svc::ItemContainer::Backpack || c >= svc::ItemContainer::Resource0; }

inline bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

inline std::optional<svc::ItemContainer> container_of_location(uint8_t location) {
    if (location == static_cast<uint8_t>(repl::ItemLocation::Backpack)) return svc::ItemContainer::Backpack;
    if (location == static_cast<uint8_t>(repl::ItemLocation::Equipment)) return svc::ItemContainer::Equipment;
    return std::nullopt;
}

/// The NetGUID of a component of an actor of the registry: the actor's own server id and randomizer (they were minted together) with the component's ObjectId.
inline FIntrepidNetworkGUID component_guid_of(const world::WorldActor& a, uint64_t object_id) {
    FIntrepidNetworkGUID g = a.actor.guid;
    g.ObjectId = object_id;
    return g;
}

/// How far from its NPC a conversation may go before the server ends it (the client closes its own window sooner: the interaction range is 25 m).
inline constexpr double kDialogueRangeCm = 4000.0;

/// The value of the RPC parameter named `name`, null when the call was not decoded or the parameter is absent (a parameter at its default is not sent).
inline const repl::Value* quest_rpc_value(const repl::ClientRpc& rpc, std::string_view name) {
    if (!rpc.call.decoded || !rpc.call.rpc) return nullptr;
    const schema::RepCommand* c = repl::rpc_command(*rpc.call.rpc, name);
    if (!c) return nullptr;
    const auto it = rpc.call.params.values.find(c->handle);
    return it == rpc.call.params.values.end() ? nullptr : &it->second;
}

/// An integer parameter, whatever its width, exact (a record id is 63 bits: a double would round it): 0 when absent.
inline int64_t quest_int_value(const repl::Value* v) {
    if (!v) return 0;
    if (const auto* i = v->get_if<int64_t>()) return *i;
    if (const auto* u = v->get_if<uint64_t>()) return static_cast<int64_t>(*u);
    return 0;
}

/// The stacks of `count` of an item: the record's stack size each, at `quality` (0: its default quality) and with the full durability of that quality, as the GM vendors' goods are made.
inline void stacks_of(int64_t record, int32_t count, const content::ItemRecord& rec, std::vector<svc::InventoryItem>& out, int64_t created_ms, int32_t quality = 0) {
    const int32_t stack = rec.max_stack_size > 0 ? rec.max_stack_size : 1;
    for (int32_t left = count; left > 0;) {
        svc::InventoryItem it;
        it.container = svc::ItemContainer::Backpack;   // Inventory::grant routes it
        it.item_record_id = record;
        it.quantity = std::min(left, stack);
        it.quality = quality > 0 ? quality : rec.default_quality;
        it.durability = rec.max_durability_at(it.quality);
        it.created_at_ms = created_ms;
        left -= it.quantity;
        out.push_back(std::move(it));
    }
}

}  // namespace session_util
using namespace session_util;

}}  // namespace aoc::worldd
