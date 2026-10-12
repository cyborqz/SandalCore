// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  vendor/gm_vendor.h  (plan E1, stage 1: the GM vendor)
//
//  A test realm's vendors that give the GM and PTR items away: pure logic over content.db, no sockets.  The retail flow is
//  (research of the retail captures 716, 863, 1531, 1536; libaocnet protocol/repl/vendor_rpc.h):
//
//    client   ServerInteractWithInteractableComponent(the NPC's InteractableComponent, counter, zone)
//    server   ClientOpenVendor(MerchSetId = a VendorDef guid, Vendor = the NPC)      the client's own window
//    client   ServerHandleVendorPurchase(FVendorPurchaseCart: the VendorDef, [(item record, quantity)])
//    server   ClientShowCurrencyCallout (the price), a ClientReceiveStorageEvent (Add) per item, the Currencies
//
//  What the window shows is the client's, not the server's: MerchSetId names a VendorDef of the client's own tables (the same design
//  data as content.db's vendor_def, vendor_def_inventory, vendor_listing) and the client draws its tabs, listings and prices from
//  it.  The game ships the vendors of its public test realm in that data: the "PTR_ONLY_Vendor_*" defs, whose listings are the GM and PTR
//  items, free (no offer).  So the GM vendor is not one window the server composes but these defs, each on an NPC of its own named for its
//  kind ("GM Gear 0-9", "GM Materials"...), grouped by kind in two arcs in front of the login spot:
//
//    stock     the listings of the def's inventories that are not hidden: the item records the player may buy there, nothing else
//    price     none: the PTR defs list no offer, and the server takes no currency (worldd has none yet); a price the client's window
//              would show is its own
//    purchase  a cart of items of the open def, a quantity each; each becomes stacks of the record's MaxStackSize (the use of an item
//              grants in the same way: session/item_use.h) in the lowest free bag slots, an Add event each; a bag with too few free
//              slots for the whole cart refuses it whole and the client's storages are sent again
//
//  `GmVendors::load` reads the defs once at start.  It also counts the item records content.db names GM% or %PTR% against the stock of
//  the defs, for the report: those no def lists cannot be bought (they are in the game's tables for other uses, e.g. a scroll's reward).
// ============================================================================
#pragma once

#include "content/content_db.h"
#include "protocol/repl/value.h"
#include "session/item_use.h"
#include "spawns/area.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace aoc::worldd::vendor {

/// The prefix of the public test realm's vendor defs.
inline constexpr const char* kGmVendorPrefix = "PTR_ONLY_Vendor_";

/// The def whose NPC ("GM Mounts") sells every mount item (mounts, stage 1): the game's own def lists 20 ground mounts, the other 81 are added to its stock
/// here, so a cart naming any of the 101 is accepted.  The window the client draws is its own copy of the def: it offers only what the def lists.
inline constexpr const char* kMountsVendorName = "PTR_ONLY_Vendor_Mounts";

/// The most elements a purchase cart may carry (a window shows far fewer; the bag has 70 slots).
inline constexpr size_t kMaxCartItems = 70;
/// The most of one item a cart element asks for (a stack of materials is 1,000).
inline constexpr int32_t kMaxQuantity = 100000;

struct GmVendor {
    int64_t              guid = 0;       // the VendorDef: ClientOpenVendor's MerchSetId
    std::string          name;           // "PTR_ONLY_Vendor_Gear_0-9"
    std::string          kind;           // "Gear"
    std::string          label;          // "GM Gear 0-9": what the NPC is called
    std::set<int64_t>    items;          // the item records of its shown listings
    size_t               listings = 0;   // shown listings (an item can be listed in two tabs)
    size_t               tabs = 0;
};
// C | Y | B and CoA

/// What a purchase cart comes to.
struct PurchasePlan {
    bool                    ok = false;
    std::string             refusal;
    std::vector<use::Granted> stacks;    // the fresh instances, in the cart's order
    std::string             summary;     // "3 item(s) in 4 stack(s)"
};

class GmVendors {
public:
    /// Reads every vendor def named kGmVendorPrefix* and the GM / PTR item records. nullopt, with `error` set, when a query fails or the
    /// file has no such def (a file of another shape: rebuild it with contentc).
    static std::optional<GmVendors> load(const content::ContentDb& db, std::string& error);

    /// A catalog of the given defs (a test's): the GM / PTR item counts are zero.
    static GmVendors from_defs(std::vector<GmVendor> defs);

    /// The defs, by name (the order the NPCs are laid out in).
    const std::vector<GmVendor>& vendors() const { return vendors_; }
    const GmVendor* find(int64_t guid) const;

    /// The item records named GM% or %PTR%, how many a def lists, and the names of the ones none does.
    size_t gm_item_count() const { return gm_items_; }
    size_t gm_items_stocked() const { return gm_stocked_; }
    const std::vector<std::string>& unstocked() const { return unstocked_; }
    /// Distinct item records over all the defs.
    size_t stocked_records() const { return all_items_.size(); }

    /// Mounts, stage 1: the mount items added to the mounts def's stock (the ones its own listings lack: 101 less what it lists), how many mounts content.db
    /// holds, and why the mount tables could not be read ("" when they were or when the load had none to read: from_defs).
    size_t mounts_added() const { return mounts_added_; }
    size_t mounts_total() const { return mounts_total_; }
    const std::string& mount_error() const { return mount_error_; }

    /// Whether the cart is one `vendor` sells and the bag has room for. Pure: nothing is changed.
    PurchasePlan plan_purchase(const content::ContentDb& db, const GmVendor& vendor, const protocol::repl::VendorPurchaseCart& cart, size_t free_slots) const;

private:
    std::vector<GmVendor>           vendors_;
    std::map<int64_t, size_t>       by_guid_;
    std::set<int64_t>               all_items_;
    size_t                          gm_items_ = 0, gm_stocked_ = 0;
    size_t                          mounts_added_ = 0, mounts_total_ = 0;
    std::string                     mount_error_;
    std::vector<std::string>        unstocked_;
};

/// The label of a def ("GM Gear 0-9" for PTR_ONLY_Vendor_Gear_0-9) and its kind ("Gear").
std::string label_of(const std::string& def_name);
std::string kind_of(const std::string& def_name);

/// Where the GM NPCs stand: `count` points in two arcs (a row of at most `inner` NPCs near and the rest further out) in front of the login
/// spot, each facing it.
struct GmPlacement {
    double x = 0, y = 0, z = 0, yaw_degrees = 0;
};
std::vector<GmPlacement> gm_layout(size_t count, const spawns::AreaConfig& area, double facing_degrees, double z);

}  // namespace aoc::worldd::vendor
