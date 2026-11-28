// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  market/undo.h
//
//  What a refused purchase or removal takes back: exactly the units its placement added to the bags (each Add event of the placement, where the item is now), and nothing else.
//  The player may have moved, split, used or destroyed other things while the step was in flight, and those changes stay.  A top-up of a stack that is already there is taken
//  out of that stack, a new stack goes whole, wherever the player has put it since.
// ============================================================================
#pragma once

#include "items/inventory.h"

#include <algorithm>
#include <string>
#include <vector>

namespace aoc::worldd::market {

/// Takes out of `inventory` the units the Add events of a placement put in, latest first.  An event whose item is not where it was is looked up by its instance guid; one that is
/// gone (spent or destroyed meanwhile) takes nothing.
inline void undo_added(items::Inventory& inventory, const std::vector<items::Event>& events) {
    for (auto it = events.rbegin(); it != events.rend(); ++it) {
        if (it->type != ::aoc::protocol::repl::StorageEventType::Add) continue;
        const std::string& guid = it->item.instance_guid;
        if (inventory.consume(it->container, it->slot, guid, it->item.quantity).ok) continue;
        const svc::InventoryItem* now = inventory.find_guid(guid);
        if (!now) continue;   // taken out of the bags meanwhile, or topped up into a stack that has another guid
        const svc::ItemContainer container = now->container;
        const int slot = now->slot;
        const int32_t count = std::min(now->quantity, it->item.quantity);
        inventory.consume(container, slot, guid, count);
    }
}

}  // namespace aoc::worldd::market
