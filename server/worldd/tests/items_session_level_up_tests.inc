// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/items_session_level_up_tests.inc: a level gained gives Health, Mana and Stamina back
//  (registered with AOC_REGISTER_TEST: test_worldd_items_session.cpp runs it through aoc_tests::run; it is included from items_session_registered.inc)
// ============================================================================

void test_level_up_refills() {
    std::printf("a level gained gives Health, Mana and Stamina back\n");
    Harness h(record_with_scrolls());
    const int64_t kHealth = guid_named(h, "Health"), kMaxHealth = guid_named(h, "MaxHealth"), kMana = guid_named(h, "Mana"), kMaxMana = guid_named(h, "MaxMana");
    const emit::FIntrepidNetworkGUID ability = h.s->bootstrap().setup().guids.ability;
    const svc::InventoryItem* scepter = h.s->inventory().at(Worn, 15);
    CHECK(scepter != nullptr, "(the Bard wears the Scepter)");
    if (!scepter) return;
    h.settle();
    CHECK(h.s->set_current_value(kHealth, 1.0f) && h.s->set_current_value(kMana, 1.0f), "(Health and Mana are spent)");
    h.request(trigger_with_item(ability, content::kGuidDodge, 6, 15, items::to_instance(*scepter), 4), h.pawn_ch);
    CHECK(h.s->abilities() && h.s->abilities()->stamina() < h.s->abilities()->max_stamina(), "(a Dodge spends Stamina)");
    h.request_both(item_used(3, 4, guid_n(210)), item_activate(3, 4, guid_n(210)));
    CHECK(h.s->progress().level == 10, "(the scroll takes the character to level 10)");
    h.settle();
    const sg::CharacterStats* cs = h.s->character_stats();
    CHECK(cs->find(kHealth)->value == cs->find(kMaxHealth)->value && client_stat(h, kHealth) == client_stat(h, kMaxHealth) && client_stat(h, kMaxHealth) > 1.0f, "Health is at the new maximum, on the server and the client");
    CHECK(cs->find(kMana)->value == cs->find(kMaxMana)->value && client_stat(h, kMana) == client_stat(h, kMaxMana) && client_stat(h, kMaxMana) > 1.0f, "Mana is at the new maximum, on the server and the client");
    CHECK(h.s->abilities()->stamina() == h.s->abilities()->max_stamina() && client_stat(h, content::kGuidStamina) == client_stat(h, content::kGuidMaxStamina), "Stamina is at its maximum, in the runtime and on the client");
    for (int i = 0; i < 300; ++i) h.step(milliseconds(20));   // (the Dodge's pause of the recovery ends)
    std::string why;
    const bool agree = stats_match(h, &why);
    if (!agree) std::printf("  [info] %s\n", why.c_str());
    CHECK(agree && !h.s->abilities_failed(), "the session's stats agree with the client's and the abilities stay on");
}
AOC_REGISTER_TEST(test_level_up_refills);
