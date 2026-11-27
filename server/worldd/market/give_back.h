// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  market/give_back.h
//
//  What a listing's refused save gives back to the seller: exactly the units the listing took and its fee, and nothing else.  The seller may have moved, split, used or
//  destroyed the rest of the stack while the save was in flight, so the units do not go back to a copy of the stack as it was:
//    a part of a stack was listed   the units join the rest of the stack (the instance that kept the stack's guid) wherever it is now; when it is gone they come back under
//                                   the stack's guid at the stack's slot, and when that slot is taken, at a free place; when the rest is full they are a stack of their own
//    a whole stack was listed       the stack comes back under its guid at its slot, or at a free place when the slot is taken
//  No other item of the bags changes.  The session gives back to its own bags, and the world to the bags a session left when it ended.
// ============================================================================
#pragma once

#include "items/inventory.h"
#include "items/wallet.h"
#include "market/listings.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>

namespace aoc::worldd::market {

/// A catalog that knows no record: a bag handed back with no content database still takes its units back, by the routing that places an item of an unknown record.
class NoItemCatalog final : public items::ItemCatalog {
public:
    std::optional<items::ItemInfo> info(int64_t) const override { return std::nullopt; }
};

namespace give_back_detail {

/// Puts `units` (an instance whose container and slot are where it was) in the bags: at its own slot, else at a free place of its kind (the first container with room), else
/// where the routing puts it (which may top up a stack).  False when nothing has room.
inline bool place_units(items::Inventory& inventory, const items::ItemCatalog& catalog, const svc::InventoryItem& units) {
    if (inventory.add_at(units.container, units.slot, units, catalog).ok) return true;
    const std::optional<items::ItemInfo> info = catalog.info(units.item_record_id);
    if (info && info->resource) {
        for (int i = 0; i < items::kResourceContainers; ++i) {
            const svc::ItemContainer container = items::resource_container(i);
            const int slot = inventory.place(container, info->inv_w, info->inv_h, catalog);
            if (slot >= 0 && inventory.add_at(container, slot, units, catalog).ok) return true;
        }
    } else {
        const int slot = inventory.first_free(svc::ItemContainer::Backpack);
        if (slot >= 0 && inventory.add_at(svc::ItemContainer::Backpack, slot, units, catalog).ok) return true;
    }
    return inventory.grant({units}, catalog).ok;
}

}  // namespace give_back_detail

/// Gives the seller back what the listing `job` took: `job.quantity` units of its stack and `job.fee` copper.  False when the units found no place (the fee is back in any
/// case).
inline bool give_back(items::Inventory& inventory, items::Wallet& wallet, const items::ItemCatalog& catalog, const PendingListing& job) {
    wallet.give(job.fee);
    svc::InventoryItem units = job.taken;
    units.quantity = job.quantity;
    if (job.quantity < job.taken.quantity) {
        // a part of the stack was listed: the rest kept the stack's guid
        if (const svc::InventoryItem* rest = inventory.find_guid(job.taken.instance_guid)) {
            const std::optional<items::ItemInfo> info = catalog.info(rest->item_record_id);
            if (info && rest->quantity + job.quantity <= std::max(1, info->max_stack)) {
                svc::InventoryItem joined = *rest;
                joined.quantity += job.quantity;
                items::Inventory trial = inventory;   // the rest comes out and goes back with the units added, at its own slot: all or nothing
                if (trial.consume(rest->container, rest->slot, rest->instance_guid, rest->quantity).ok && trial.add_at(joined.container, joined.slot, joined, catalog).ok) {
                    inventory = std::move(trial);
                    return true;
                }
            }
            units.instance_guid = job.listing.item.instance_guid;   // a stack of their own: the rest holds the stack's guid, and the listing was never made
        }
    }
    return give_back_detail::place_units(inventory, catalog, units);
}

}  // namespace aoc::worldd::market
