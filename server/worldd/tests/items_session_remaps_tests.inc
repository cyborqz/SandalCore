// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/items_session_remaps_tests.inc: the Might Potion and the elixirs use the stats their items carry
//  Registered with AOC_REGISTER_TEST; included from items_session_registered.inc.
// ============================================================================

namespace gc {
inline constexpr int64_t kMightHit = 6064632316175778137ll,
                         kMightEffect = 6064632316175778138ll;   // the client's Arcane Surge effect (the item and the ability are the consumables' own)
inline constexpr int64_t kArcaneSurgePotion = 6064632111124054069ll;   // the same Arcane Surge ability: its effect must stay its own
inline constexpr int64_t kSurgingMightPotion = 6064632111115337779ll, kSurgingMightAbility = 6064632316168765774ll,
                         kSurgingMightHit = 6064632316168765775ll, kSurgingMightEffect = 6064632316168765776ll;   // the record the Might Potion is shown as
inline constexpr int64_t kMagicalPowerRating = 109183607068297ll;
}  // namespace gc

/// The effect record the client is shown a potion's buff as: the one effect the mirror carries after the use (-1: not exactly one, so the check fails loudly).
static int64_t shown_effect_guid(const char* path, int64_t item_guid, int64_t ability, int64_t hit, uint32_t instance, int32_t client_id) {
    svc::Character rec = cyb_record();
    rec.level = 25;
    std::vector<svc::InventoryItem> items;
    for (const auto& it : rec.items)
        if (it.container == Worn) items.push_back(it);
    svc::InventoryItem potion = make_item(Bag, 4, item_guid, instance, 1);
    potion.quality = 1;
    items.push_back(potion);
    rec.items = items;
    Harness h(rec, true, true, path);
    const bs::PlayerSetup& setup = h.s->bootstrap().setup();
    const svc::InventoryItem* item = h.s->inventory().at(Bag, 4);
    if (!item) return 0;
    std::vector<fake::Bunch> sent;
    sent.push_back(h.client.actor(h.pawn_ch, item_used(3, 4, guid_n(instance)), true));
    sent.push_back(h.client.actor(h.pawn_ch, trigger_with_item(setup.guids.ability, ability, client_id, 4, items::to_instance(*item), 3), true));
    sent.push_back(h.client.actor(h.pawn_ch, hit_request_for(setup, ability, kSelfTargetingGuid, hit), true));
    h.client_sends(std::move(sent));
    for (int i = 0; i < 25; ++i) h.step();
    if (h.mirror.effect_items.size() != 1) return -1;
    return h.mirror.effect_items.begin()->second.guid;
}

void test_consumable_remaps() {
    const char* path = professions_db_path();
    if (!path) {
        std::printf("Consumable remaps: skipped (no real content.db)\n");
        return;
    }
    std::printf("The Might Potion and the elixirs use the stats their items carry\n");
    svc::Character rec = cyb_record();
    rec.level = 25;
    std::vector<svc::InventoryItem> items;
    for (const auto& it : rec.items)
        if (it.container == Worn) items.push_back(it);
    svc::InventoryItem potion = make_item(Bag, 4, gc::kMightPotion, 700, 1);
    potion.quality = 1;
    items.push_back(potion);
    rec.items = items;
    Harness h(rec, true, true, path);
    const sg::CharacterStats* cs = h.s->character_stats();
    const bs::PlayerSetup& setup = h.s->bootstrap().setup();
    if (!cs) return;
    const float physical_before = cs->find(gc::kPhysicalPowerRating)->value;
    const float magical_before = cs->find(gc::kMagicalPowerRating)->value;
    const svc::InventoryItem* item = h.s->inventory().at(Bag, 4);
    if (!item) return;
    std::vector<fake::Bunch> sent;
    sent.push_back(h.client.actor(h.pawn_ch, item_used(3, 4, guid_n(700)), true));
    sent.push_back(h.client.actor(h.pawn_ch, trigger_with_item(setup.guids.ability, gc::kMightAbility, 70, 4, items::to_instance(*item), 3), true));
    sent.push_back(h.client.actor(h.pawn_ch, hit_request_for(setup, gc::kMightAbility, kSelfTargetingGuid, gc::kMightHit), true));
    h.client_sends(std::move(sent));
    for (int i = 0; i < 25; ++i) h.step();
    const float physical_gain = cs->find(gc::kPhysicalPowerRating)->value - physical_before;
    const float magical_gain = cs->find(gc::kMagicalPowerRating)->value - magical_before;
    CHECK(physical_gain == 50.0f, "the Might Potion raises PhysicalPowerRating by its Common 50");
    CHECK(magical_gain == 0.0f, "... and leaves MagicalPowerRating alone (the ability's stat is remapped, not misapplied)");
    CHECK(shown_effect_guid(path, gc::kMightPotion, gc::kMightAbility, gc::kMightHit, 720, 80) == gc::kSurgingMightEffect,
          "the Might Potion's buff is drawn as the physical-power effect (Surging Might's record), not \"Arcane Surge Potion\"");
    CHECK(shown_effect_guid(path, gc::kArcaneSurgePotion, gc::kMightAbility, gc::kMightHit, 721, 81) == gc::kMightEffect,
          "the Arcane Surge Potion keeps its own effect: the display fix is the item's, not the ability's");
    CHECK(shown_effect_guid(path, gc::kSurgingMightPotion, gc::kSurgingMightAbility, gc::kSurgingMightHit, 722, 82) == gc::kSurgingMightEffect,
          "the Surging Might Potion shows its own effect (nothing is remapped for it)");

    const std::pair<float, float> quality = elixir_gain(path, "Crafting Quality", 6064632105766944786ll, 6064632319994429485ll, 6064632319994429486ll,
                                                        6064630316223889411ll, 0);
    const std::pair<float, float> hunting = elixir_gain(path, "Hunting", 6064632105783263255ll, 6064632320008192070ll, 6064632320008192071ll,
                                                        6064630299331854394ll, 6064630299332902986ll);
    CHECK(quality.first == 40.0f, "the Crafting Quality elixir adds the item's Uncommon 40 as Artisan_Crafting_Quality_Rating (the block calls it Cost)");
    CHECK(hunting.first == 458.0f && hunting.second == 458.0f, "the Hunting elixir adds its Uncommon 458 quantity (the block calls it Quantity) and speed ratings");
    CHECK(!h.s->failed() && h.mirror.unreadable == 0, "(nothing failed)");
}
AOC_REGISTER_TEST(test_consumable_remaps);
