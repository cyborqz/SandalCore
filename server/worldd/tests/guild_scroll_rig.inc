// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  tests/guild_scroll_rig.inc: the Guild Creation Scroll (item_use activation OpenGuildCreationMenu) of content.db as the character store
//  returns it in a bag, for the guild tests that make a guild through the client's ServerCreateGuild.  Included after session_party_tests.inc
//  (party_record) by the guild test files; the create it pays for is in session_guild_tests.inc and session_guild_scroll_tests.inc.
// ============================================================================
#pragma once

/// The guild creation scroll of content.db (item_use activation OpenGuildCreationMenu).
constexpr int64_t kTestGuildScroll = 6064632203848384541ll;

static std::string test_guild_scroll_guid(uint32_t n) {
    char text[33];
    std::snprintf(text, sizeof text, "%08x%08x%08x%08x", 0x60646322u + n, 0x03848384u, 0x54510100u + n, n);
    return text;
}

static svc::InventoryItem test_guild_scroll_item(int slot, const std::string& guid) {
    svc::InventoryItem item;
    item.container = svc::ItemContainer::Backpack;
    item.slot = slot;
    item.item_record_id = kTestGuildScroll;
    item.instance_guid = guid;
    item.quantity = 1;
    item.quality = 1;
    item.durability = 0;
    item.created_at_ms = 1'771'299'474'000ll;
    return item;
}

/// A character of the guild tests, level 25, with `scrolls` guild creation scrolls in the item bag from slot 4 on.
static svc::Character test_guild_scroll_record(const char* name, const std::array<uint32_t, 4>& words, int scrolls, uint32_t first_guid) {
    svc::Character c = party_record(name, words);
    c.level = 25;
    for (int i = 0; i < scrolls; ++i) c.items.push_back(test_guild_scroll_item(4 + i, test_guild_scroll_guid(first_guid + static_cast<uint32_t>(i))));
    return c;
}
