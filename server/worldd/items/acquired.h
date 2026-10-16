// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  items/acquired.h: the pick-up popup of a harvest (Gathering stage 3, "the pick-up popup")
//
//  The retail server puts an FCalloutQueueInfo (protocol/repl/value.h, CalloutQueueInfo; the layout is item_codec.h's) into every ClientReceiveStorageEvent of Type Add that a
//  harvest or a hunt sends for the item it put into a Resource container.  The client's event apply hands it to the item feed, which shows the pick-up popup: the item's icon
//  (from the callout's ItemAssociated) and the rich text "Acquired <a ...> [Coconut Wood] </>x5".  Without it the item arrives in the bag and no popup appears.
//
//  What the 24 retail events (tests/capture_resource.h, 22 captures: 18 into Resource0, 6 into Resource1) say, and this header writes bit for bit:
//    * the text is `Acquired <a id="Item" style="Item_<rarity>" metaType="ItemLink" metaGUID="<record>" metaItemQuality="<quality>" metaEnchantLevel="0" metaTemperingPercent="0"
//      metaDurability="0" metaSockets=""> [<name>] </>x<quantity>`, and for a quantity of 1 `... [<name>]</>` (no space before the closing tag and no "x1"): 5 of the 24 events have
//      a quantity of 1 (the carcass portions and the Wolf), 19 have 2 to 6.  <rarity> is the band of the quality (Common 1-5, Uncommon 6-15, Rare 16-30, Heroic 31-50; the events
//      show Common, Uncommon, Rare and Heroic, qualities 1, 6, 16 and 31); <name> is the record's ItemName ("Coconut Wood": content.db's item.display_name, contentc version 16).
//    * the callout holds that text twice (Message and FormattedOnScreenMessage), the stored item again (ItemAssociated: the same instance, guid and quantity), the callout type 6,
//      the point size 36 and the chat category 141, and nothing else but the time it was made (CreationTime, FDateTime ticks): no title, no localized message, no FText, no
//      emblems, ids 0, location 0.
//  What is inferred: the style names of the bands above Heroic (Item_Epic, Item_Legendary, Item_Artifact); that a top-up of an existing stack is told the quantity that was added
//  (no capture shows one: the events are all new stacks); that the metadata after the quality is zero for an item that has an enchantment, a tempering or sockets (resources have none).
// ============================================================================
#pragma once

#include "protocol/repl/value.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace aoc::worldd::items {

/// The callout's type, point size and chat category in all 24 retail events.
inline constexpr uint8_t kAcquiredCalloutType = 6;
inline constexpr int32_t kAcquiredPointSize = 36;
inline constexpr uint8_t kAcquiredChatCategory = 141;

/// The name of the rarity band of an item quality: Common 1-5, Uncommon 6-15, Rare 16-30, Heroic 31-50, Epic 51-70, Legendary 71-100, Artifact 101-200 (any other quality reads Common).
inline std::string_view rarity_band_name(uint32_t quality) {
    if (quality >= 101 && quality <= 200) return "Artifact";
    if (quality >= 71) return "Legendary";
    if (quality >= 51) return "Epic";
    if (quality >= 31) return "Heroic";
    if (quality >= 16) return "Rare";
    if (quality >= 6) return "Uncommon";
    return "Common";
}

/// The popup's rich text for `item` (its record, quality and quantity) named `name`.
inline std::string acquired_text(const ::aoc::protocol::repl::ItemInstance& item, std::string_view name) {
    std::string s = "Acquired <a id=\"Item\" style=\"Item_";
    s += rarity_band_name(item.quality);
    s += "\" metaType=\"ItemLink\" metaGUID=\"";
    s += std::to_string(item.record_id);
    s += "\" metaItemQuality=\"";
    s += std::to_string(item.quality);
    s += "\" metaEnchantLevel=\"0\" metaTemperingPercent=\"0\" metaDurability=\"0\" metaSockets=\"\"> [";
    s += name;
    if (item.quantity > 1) {
        s += "] </>x";
        s += std::to_string(item.quantity);
    } else {
        s += "]</>";
    }
    return s;
}
// C.Y.B / CoA

/// The FCalloutQueueInfo of a harvest's storage event for `item` as it is stored (its quantity is what was acquired), made at `creation_ticks` (FDateTime ticks).
inline ::aoc::protocol::repl::CalloutQueueInfo acquired_callout(const ::aoc::protocol::repl::ItemInstance& item, std::string_view name, int64_t creation_ticks) {
    ::aoc::protocol::repl::CalloutQueueInfo c;
    c.message = acquired_text(item, name);
    c.formatted_on_screen = c.message;
    c.callout_type = kAcquiredCalloutType;
    c.point_size = kAcquiredPointSize;
    c.chat_category = kAcquiredChatCategory;
    c.item = item;
    c.creation_time.ticks = creation_ticks;
    return c;
}

}  // namespace aoc::worldd::items
