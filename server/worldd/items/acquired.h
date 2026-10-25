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
//  What the 24 retail events (tests/capture_resource.h, 22 retail captures into Resource0, 6 into Resource1) say, and this header writes bit for bit:
//    * the text is `Acquired <a id="Item" style="Item_<rarity>" metaType="ItemLink" metaGUID="<record>" metaItemQuality="<quality>" metaEnchantLevel="0" metaTemperingPercent="0"
//      metaDurability="0" metaSockets=""> [<name>] </>x<quantity>`, and for a quantity of 1 `... [<name>]</>` (no space before the closing tag and no "x1"): 5 of the 24 events have
//      a quantity of 1 (the carcass portions and the Wolf), 19 have 2 to 6.  <rarity> is the rarity the item shows (items/rarity.h): the band of the quality (Common 1-5, Uncommon
//      6-15, Rare 16-30, Heroic 31-50 ...) held inside the record's own RarityMin..RarityMax, so a Glint of tier 2 (quality 21, always Uncommon) reads Item_Uncommon and Bee's Wax
//      (quality 1, never below Rare) reads Item_Rare; the 24 events show Common, Uncommon, Rare and Heroic, qualities 1, 6, 16 and 31; <name> is the record's ItemName ("Coconut
//      Wood": content.db's item.display_name, contentc version 16).
//    * the callout holds that text twice (Message and FormattedOnScreenMessage), the stored item again (ItemAssociated: the same instance, guid and quantity), the callout type 6,
//      the point size 36 and the chat category 141, and nothing else but the time it was made (CreationTime, FDateTime ticks): no title, no localized message, no FText, no
//      emblems, ids 0, location 0.
//  What is inferred: the style names of the rarities above Epic (Item_Legendary, Item_Artifact); that a top-up of an existing stack is told the quantity that was added
//  (no capture shows one: the events are all new stacks); that the metadata after the quality is zero for an item that has an enchantment, a tempering or sockets (resources have none).
//
//  The vendors milestone, step 1: the same callout rides the other storage events the item feed shows.  A purchase's Add event carries the type 6 "Acquired" form, as a harvest's does;
//  the Remove event of an item sold to a shop or destroyed by dragging it out carries the type 16 "Removed" form, with the item's durability in its link (metaDurability="14").
//  Without it the item arrives in or leaves the bag and no toast appears.  Nothing else differs from the harvest's callout.
// ============================================================================
#pragma once

#include "items/rarity.h"
#include "protocol/repl/value.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace aoc::worldd::items {

/// The callout's type, point size and chat category in all 24 retail events.
inline constexpr uint8_t kAcquiredCalloutType = 6;
inline constexpr int32_t kAcquiredPointSize = 36;
inline constexpr uint8_t kAcquiredChatCategory = 141;

/// The item link's durability as the text shows it: a whole number without decimals ("14"), else up to two ("13.5").
inline std::string durability_text(float durability) {
    if (!(durability > 0.0f)) return "0";
    const long long whole = static_cast<long long>(durability);
    if (static_cast<float>(whole) == durability) return std::to_string(whole);
    std::string s = std::to_string(std::lround(durability * 100.0f));   // hundredths
    while (s.size() < 3) s.insert(s.begin(), '0');
    s.insert(s.size() - 2, ".");
    while (s.back() == '0') s.pop_back();
    return s;
}

/// The rich text of a toast about `item` named `name`, led by `word` ("Acquired", "Removed"): the item link with the rarity the item shows (`facts`: its record's quality curve and
/// RarityMin..RarityMax), its record, quality and durability, and "x<quantity>" after it for a stack of more than one.
inline std::string item_toast_text(std::string_view word, const ::aoc::protocol::repl::ItemInstance& item, std::string_view name, const ItemRarityFacts& facts) {
    std::string s(word);
    s += " <a id=\"Item\" style=\"Item_";
    s += rarity_name(facts.rarity_of(static_cast<double>(item.quality)));
    s += "\" metaType=\"ItemLink\" metaGUID=\"";
    s += std::to_string(item.record_id);
    s += "\" metaItemQuality=\"";
    s += std::to_string(item.quality);
    s += "\" metaEnchantLevel=\"0\" metaTemperingPercent=\"0\" metaDurability=\"";
    s += durability_text(item.durability);
    s += "\" metaSockets=\"\"> [";
    s += name;
    if (item.quantity > 1) {
        s += "] </>x";
        s += std::to_string(item.quantity);
    } else {
        s += "]</>";
    }
    return s;
}

/// The popup's rich text for `item` (its record, quality and quantity) named `name`, in the rarity `facts` give it.
inline std::string acquired_text(const ::aoc::protocol::repl::ItemInstance& item, std::string_view name, const ItemRarityFacts& facts) {
    return item_toast_text("Acquired", item, name, facts);
}
// C.Y.B / CoA

/// The FCalloutQueueInfo of a storage event for `item` as it is stored, toasting `text`, made at `creation_ticks` (FDateTime ticks).
inline ::aoc::protocol::repl::CalloutQueueInfo item_callout(std::string text, uint8_t type, const ::aoc::protocol::repl::ItemInstance& item, int64_t creation_ticks) {
    ::aoc::protocol::repl::CalloutQueueInfo c;
    c.message = std::move(text);
    c.formatted_on_screen = c.message;
    c.callout_type = type;
    c.point_size = kAcquiredPointSize;
    c.chat_category = kAcquiredChatCategory;
    c.item = item;
    c.creation_time.ticks = creation_ticks;
    return c;
}

/// The FCalloutQueueInfo of a harvest's storage event for `item` as it is stored (its quantity is what was acquired), made at `creation_ticks` (FDateTime ticks).
inline ::aoc::protocol::repl::CalloutQueueInfo acquired_callout(const ::aoc::protocol::repl::ItemInstance& item, std::string_view name, const ItemRarityFacts& facts, int64_t creation_ticks) {
    return item_callout(acquired_text(item, name, facts), kAcquiredCalloutType, item, creation_ticks);
}

/// The callout type of the toast of an item that leaves a bag: sold to a shop, or destroyed.
inline constexpr uint8_t kRemovedCalloutType = 16;

/// The FCalloutQueueInfo of the Remove event of an item that was sold or destroyed ("Removed <link> [Name]"): the same struct as the pick-up toast, type 16.
inline ::aoc::protocol::repl::CalloutQueueInfo removed_callout(const ::aoc::protocol::repl::ItemInstance& item, std::string_view name, const ItemRarityFacts& facts, int64_t creation_ticks) {
    return item_callout(item_toast_text("Removed", item, name, facts), kRemovedCalloutType, item, creation_ticks);
}

}  // namespace aoc::worldd::items
