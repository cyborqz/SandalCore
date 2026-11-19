// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/items_session_remaps_tests.inc: the items whose ability reads a stat their block carries under another name
//  (the number lands on the stat the ability reads — the one the tooltip names — with the value the item carries).
//  Registered with AOC_REGISTER_TEST; included from items_session_registered.inc.
// ============================================================================

namespace gc {
inline constexpr int64_t kMightEffect = 6064632316175778138ll;   // the client's Arcane Surge effect (the item and the ability are the consumables' own)
inline constexpr int64_t kArcaneSurgePotion = 6064632111124054069ll;   // the real Arcane potion: its block carries MagicalPowerRating of its own
inline constexpr int64_t kSurgingMightPotion = 6064632111115337779ll, kSurgingMightAbility = 6064632316168765774ll,
                         kSurgingMightEffect = 6064632316168765776ll;   // the physical-power potion: its own ability reads its own PhysicalPowerRating
inline constexpr int64_t kMagicalPowerRating = 109183607068297ll;
inline constexpr int64_t kMagicalPenetration = 109342908372605ll, kPhysicalPenetration = 109342856795769ll;
inline constexpr int64_t kMagicalSpeed = 109183607068299ll, kPhysicalSpeed = 109342866232955ll;
inline constexpr int64_t kCraftingQualityRating = 6064630316223889411ll, kCraftingCostRating = 6064630316223627265ll;
inline constexpr int64_t kHuntingRarityRating = 6064630299332247618ll, kHuntingQuantityRating = 6064630299331854394ll,
                         kHuntingSpeedRating = 6064630299332902986ll;
}  // namespace gc

/// One use of `item` at `quality` (its effects come with the cast, when its drinking or eating ends): what `read_stat` gained, and the value the item's stat
/// block carries under `carried_stat` at that quality.  A fixed item gives the same two numbers.
static std::pair<float, float> remap_gain(const char* path, int64_t item, int64_t ability, int64_t read_stat, int64_t carried_stat, int quality = 6,
                                          uint32_t instance = 730) {
    svc::Character rec = cyb_record();
    rec.level = 25;
    std::vector<svc::InventoryItem> items;
    for (const auto& it : rec.items)
        if (it.container == Worn) items.push_back(it);
    svc::InventoryItem used = make_item(Bag, 4, item, instance, 1);
    used.quality = quality;
    items.push_back(used);
    rec.items = items;
    Harness h(rec, true, true, path);
    const sg::CharacterStats* cs = h.s->character_stats();
    const bs::PlayerSetup& setup = h.s->bootstrap().setup();
    if (!cs) return {-1.0f, -1.0f};
    const float before = cs->find(read_stat) ? cs->find(read_stat)->value : 0.0f;
    double carried = 0;
    for (const stats::StatMod& m : cs->item_stats(item, quality))
        if (m.stat == carried_stat) carried = m.value;
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 4);
    if (!held) return {-1.0f, -1.0f};
    h.client_sends({h.client.actor(h.pawn_ch, item_used(3, 4, guid_n(instance)), true),
                    h.client.actor(h.pawn_ch, trigger_with_item(setup.guids.ability, ability, 76, 4, items::to_instance(*held), 3), true)});
    for (int i = 0; i < 110; ++i) h.step();   // 2.2 s: the use runs its course
    const int64_t server = h.s->pipeline() ? h.s->pipeline()->instance_of(76) : -1;
    if (server >= 0) {
        h.client_sends({h.client.actor(h.pawn_ch, end_ability(setup, server), true)});   // the bar completes: the client ends its use (a food's or drink's buff waits for it)
        for (int i = 0; i < 15; ++i) h.step();
    }
    const sg::StatValue* after = cs->find(read_stat);
    return {after ? after->value - before : -1.0f, static_cast<float>(carried)};
}

/// The effect record the client is shown the item's buff as: the one effect the mirror carries after the use (-1: not exactly one).
static int64_t shown_effect_guid(const char* path, int64_t item, int64_t ability, uint32_t instance, int32_t client_id) {
    svc::Character rec = cyb_record();
    rec.level = 25;
    std::vector<svc::InventoryItem> items;
    for (const auto& it : rec.items)
        if (it.container == Worn) items.push_back(it);
    svc::InventoryItem used = make_item(Bag, 4, item, instance, 1);
    used.quality = 1;
    items.push_back(used);
    rec.items = items;
    Harness h(rec, true, true, path);
    const bs::PlayerSetup& setup = h.s->bootstrap().setup();
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 4);
    if (!held) return 0;
    h.client_sends({h.client.actor(h.pawn_ch, item_used(3, 4, guid_n(instance)), true),
                    h.client.actor(h.pawn_ch, trigger_with_item(setup.guids.ability, ability, client_id, 4, items::to_instance(*held), 3), true)});
    for (int i = 0; i < 110; ++i) h.step();
    const int64_t server = h.s->pipeline() ? h.s->pipeline()->instance_of(client_id) : -1;
    if (server >= 0) {
        h.client_sends({h.client.actor(h.pawn_ch, end_ability(setup, server), true)});   // the bar completes: the client ends its use
        for (int i = 0; i < 15; ++i) h.step();
    }
    if (h.mirror.effect_items.size() != 1) return -1;
    return h.mirror.effect_items.begin()->second.guid;
}

void test_consumable_remaps() {
    const char* path = professions_db_path();
    if (!path) {
        std::printf("Consumable remaps: skipped (no real content.db)\n");
        return;
    }
    std::printf("The items whose ability reads a stat their block carries under another name give that number where the game puts it\n");

    // the Might Potion: Arcane Surge reads MagicalPowerRating and the item carries the number as PhysicalPowerRating — the buff is Physical Power, drawn as the
    // physical-power effect
    {
        const auto might_physical = remap_gain(path, gc::kMightPotion, gc::kMightAbility, gc::kPhysicalPowerRating, gc::kPhysicalPowerRating, 1, 731);
        const auto might_magical = remap_gain(path, gc::kMightPotion, gc::kMightAbility, gc::kMagicalPowerRating, gc::kPhysicalPowerRating, 1, 736);
        std::printf("    the Might Potion raised PhysicalPowerRating by %.1f and MagicalPowerRating by %.1f (its block's PhysicalPowerRating: %.1f)\n",
                    might_physical.first, might_magical.first, might_physical.second);
        CHECK(might_physical.first == 50.0f && might_physical.second == 50.0f, "the Might Potion raises PhysicalPowerRating by its Common 50: the number its block carries");
        CHECK(might_magical.first == 0.0f, "... and leaves MagicalPowerRating alone (the ability's stat is remapped, not misapplied)");
        CHECK(shown_effect_guid(path, gc::kMightPotion, gc::kMightAbility, 732, 80) == gc::kSurgingMightEffect,
              "... and the buff is drawn as the physical-power effect (Surging Might's record), not \"Arcane Surge Potion\"");
    }
    // the Arcane Surge and Surging Might potions keep their own numbers (nothing is remapped for them)
    CHECK(shown_effect_guid(path, gc::kArcaneSurgePotion, gc::kMightAbility, 733, 81) == gc::kMightEffect, "the Arcane Surge Potion keeps its own effect");
    CHECK(shown_effect_guid(path, gc::kSurgingMightPotion, gc::kSurgingMightAbility, 734, 82) == gc::kSurgingMightEffect,
          "the Surging Might Potion shows its own effect (nothing is remapped for it)");

    // the Elixir of Crafting Quality: the ability reads Quality, the item carries the number as Cost
    {
        const auto quality = remap_gain(path, 6064632105766944786ll, 6064632319994429485ll, gc::kCraftingQualityRating, gc::kCraftingCostRating);
        CHECK(quality.first == quality.second && quality.first == 40.0f, "the Crafting Quality elixir raises Quality by its Uncommon 40 (the block calls it Cost)");
    }
    // the Elixir of Hunting: the ability reads Rarity (and Speed), the item carries the Rarity number as Quantity
    {
        const auto rarity = remap_gain(path, 6064632105783263255ll, 6064632320008192070ll, gc::kHuntingRarityRating, gc::kHuntingQuantityRating);
        const auto speed = remap_gain(path, 6064632105783263255ll, 6064632320008192070ll, gc::kHuntingSpeedRating, gc::kHuntingSpeedRating, 6, 735);
        CHECK(rarity.first == rarity.second && rarity.first == 458.0f, "the Hunting elixir raises Rarity by its Uncommon 458 (the block calls it Quantity)");
        CHECK(speed.first == speed.second && speed.first == 458.0f, "... and Speed by its own 458");
    }
    // the food and drinks of the same shape: the number lands under the stat the ability reads
    {
        const auto tar_tar = remap_gain(path, 6064631986960924712ll, 6064632320263520601ll, gc::kPhysicalPenetration, gc::kPhysicalSpeed);
        CHECK(tar_tar.first == tar_tar.second && tar_tar.first > 0, "Beef Tar Tar raises PhysicalPenetrationRating by the number it carries as PhysicalSpeedRating");
        const auto goblet = remap_gain(path, 6064632586725298078ll, 6064632587116347463ll, gc::kMagicalPenetration, gc::kMagicalSpeed);
        CHECK(goblet.first == goblet.second && goblet.first > 0, "the Glyphic Goblet raises MagicalPenetrationRating by the number it carries as MagicalSpeedRating");
    }
}
AOC_REGISTER_TEST(test_consumable_remaps);
