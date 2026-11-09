// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/items_session_interruption_tests.inc: an item use ends when the player moves or starts another ability
//  Registered with AOC_REGISTER_TEST; included from items_session_registered.inc.
// ============================================================================

void test_item_use_is_interrupted() {
    const char* path = professions_db_path();
    if (!path) {
        std::printf("Item interruption: skipped (no real content.db)\n");
        return;
    }
    std::printf("An item use is interrupted when the player moves or starts another ability\n");
    svc::Character rec = cyb_record();
    rec.level = 25;
    std::vector<svc::InventoryItem> items;
    for (const auto& it : rec.items)
        if (it.container == Worn) items.push_back(it);
    items.push_back(make_item(Bag, 4, gc::kFreshSalad, 901, 1));
    rec.items = items;
    Harness h(rec, true, true, path);
    const bs::PlayerSetup& setup = h.s->bootstrap().setup();
    const svc::InventoryItem* item = h.s->inventory().at(Bag, 4);
    if (!item) return;
    std::vector<fake::Bunch> sent;
    sent.push_back(h.client.actor(h.pawn_ch, item_used(3, 4, guid_n(901)), true));
    sent.push_back(h.client.actor(h.pawn_ch, trigger_with_item(setup.guids.ability, gc::kFreshSaladAbility, 76, 4, items::to_instance(*item), 3), true));
    sent.push_back(h.client.actor(h.pawn_ch, hit_request_for(setup, gc::kFreshSaladAbility, kSelfTargetingGuid, gc::kFreshSaladHit), true));
    h.client_sends(std::move(sent));
    for (int i = 0; i < 25; ++i) h.step();
    CHECK(effect_of(h, gc::kFreshSaladEffect) != nullptr, "(the meal's buff is on)");
    CHECK(effect_of(h, gc::kRestingEffect) != nullptr, "Resting is on while the character eats");
    if (h.s->pipeline()) const_cast<AbilityPipeline*>(h.s->pipeline())->end_item_casts(world_time_at(h.t), "the test interrupts the use");
    for (int i = 0; i < 5; ++i) h.step();
    CHECK(effect_of(h, gc::kRestingEffect) == nullptr, "the interruption takes Resting with it");
    CHECK(effect_of(h, gc::kFreshSaladEffect) != nullptr, "... and the meal's own buff stays");
    CHECK(!h.s->failed() && h.mirror.unreadable == 0, "(nothing failed)");
}
AOC_REGISTER_TEST(test_item_use_is_interrupted);
