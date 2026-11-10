// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/items_session_consumables_tests.inc: a meal, a drink, a potion and a salve used through their abilities
//  Registered with AOC_REGISTER_TEST; included from items_session_registered.inc.
// ============================================================================

/// The self-targeting record a potion's heal asks for (the same id the program's gc::kSelfTargeting names).
constexpr int64_t kSelfTargetingGuid = 6064628968549646338ll;

namespace gc {
// the meals, drinks, potions and salves the ability path serves (worldd's consumable_ability: Food, Drink, Elixir, Potion, Salve, Scroll)
inline constexpr int64_t kFreshSalad = 6064630417660641280ll, kFreshSaladAbility = 6064632310090891469ll, kFreshSaladHit = 6064632310090891470ll,
                         kFreshSaladEffect = 6064632310090891471ll;
inline constexpr int64_t kFreshSaladNourishment = 6064630689631830077ll, kRestingEffect = 109394092054139ll;
inline constexpr int64_t kBread = 6064630417660837910ll, kBreadAbility = 6064632320234029296ll, kBreadHit = 6064632320234029297ll,
                         kBreadEffect = 6064632320234029298ll;
inline constexpr int64_t kAlchemicalAmber = 6064632586725298067ll, kAlchemicalAmberAbility = 6064632587121328239ll, kAlchemicalAmberHit = 6064632587121328240ll,
                         kAlchemicalAmberEffect = 6064632587121328241ll;
inline constexpr int64_t kHealthPotion = 6064629336879989989ll, kHealthPotionAbility = 6064629336911381734ll, kHealthPotionHit = 6064629336937923816ll;
inline constexpr int64_t kMightPotion = 6064634675081576599ll, kMightAbility = 6064632316175778136ll;   // the client's Arcane Surge ability
inline constexpr int64_t kHealthSalve = 6064632105621258242ll, kHealthSalveAbility = 6064632316126953734ll, kHealthSalveHit = 6064632316126953735ll,
                         kHealthSalveEffect = 6064632316126953736ll;
inline constexpr int64_t kRations = 6064632586725298082ll, kRationsAbility = 6064632587347886421ll, kRationsEffect = 6064632587347886424ll;
inline constexpr int64_t kPhysicalPowerRating = 109183603267200ll, kMagicalCriticalPowerRating = 109342909421182ll,
                         kAlchemyRarityRating = 6064629302956786184ll;
inline constexpr int64_t kHealthStat = 108986352686712ll, kManaStat = 109183576135288ll, kMaxHealthStat = 108986356618873ll;
}  // namespace gc

static float potion_heal(const char* path, int quality, int requests = 1) {
    svc::Character rec = cyb_record();
    rec.level = 25;
    std::vector<svc::InventoryItem> items;
    for (const auto& it : rec.items)
        if (it.container == Worn) items.push_back(it);
    svc::InventoryItem potion = make_item(Bag, 4, gc::kHealthPotion, static_cast<uint32_t>(500 + quality), 1);
    potion.quality = quality;
    items.push_back(potion);
    rec.items = items;
    Harness h(rec, true, true, path);
    const sg::CharacterStats* cs = h.s->character_stats();
    const bs::PlayerSetup& setup = h.s->bootstrap().setup();
    if (!cs || !h.s->set_current_value(gc::kHealthStat, 1.0f)) return -1.0f;
    const float before = cs->find(gc::kHealthStat)->value;
    const svc::InventoryItem* item = h.s->inventory().at(Bag, 4);
    if (!item) return -1.0f;
    std::vector<fake::Bunch> sent;
    sent.push_back(h.client.actor(h.pawn_ch, item_used(3, 4, guid_n(static_cast<uint32_t>(500 + quality))), true));
    sent.push_back(h.client.actor(h.pawn_ch, trigger_with_item(setup.guids.ability, gc::kHealthPotionAbility, 60 + quality, 4, items::to_instance(*item), 3), true));
    for (int i = 0; i < requests; ++i) sent.push_back(h.client.actor(h.pawn_ch, hit_request_for(setup, gc::kHealthPotionAbility, kSelfTargetingGuid, gc::kHealthPotionHit), true));
    h.client_sends(std::move(sent));
    for (int i = 0; i < 25; ++i) h.step();
    return cs->find(gc::kHealthStat)->value - before;
}

static std::pair<float, float> elixir_gain(const char* path, const char* name, int64_t item, int64_t ability, int64_t hit, int64_t s1, int64_t s2) {
    svc::Character rec = cyb_record();
    rec.level = 25;
    std::vector<svc::InventoryItem> items;
    for (const auto& it : rec.items)
        if (it.container == Worn) items.push_back(it);
    svc::InventoryItem elixir = make_item(Bag, 4, item, 710, 1);
    elixir.quality = 6;   // Uncommon
    items.push_back(elixir);
    rec.items = items;
    Harness h(rec, true, true, path);
    const sg::CharacterStats* cs = h.s->character_stats();
    const bs::PlayerSetup& setup = h.s->bootstrap().setup();
    if (!cs) return {-1.0f, -1.0f};
    const sg::StatValue* a = cs->find(s1);
    const sg::StatValue* b = s2 ? cs->find(s2) : nullptr;
    const float a0 = a ? a->value : 0.0f, b0 = b ? b->value : 0.0f;
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 4);
    if (!held) return {-1.0f, -1.0f};
    std::vector<fake::Bunch> sent;
    sent.push_back(h.client.actor(h.pawn_ch, item_used(3, 4, guid_n(710)), true));
    sent.push_back(h.client.actor(h.pawn_ch, trigger_with_item(setup.guids.ability, ability, 71, 4, items::to_instance(*held), 3), true));
    sent.push_back(h.client.actor(h.pawn_ch, hit_request_for(setup, ability, kSelfTargetingGuid, hit), true));
    h.client_sends(std::move(sent));
    for (int i = 0; i < 25; ++i) h.step();
    const sg::StatValue* a1 = cs->find(s1);
    const sg::StatValue* b1 = s2 ? cs->find(s2) : nullptr;
    const float ga = (a1 ? a1->value : 0.0f) - a0, gb = (b1 ? b1->value : 0.0f) - b0;
    std::printf("  %s: %+.1f / %+.1f (effects %zu)\n", name, ga, gb, h.mirror.effect_items.size());
    return {ga, gb};
}

static void test_consumables_meal_drink_potion_salve() {
    const char* path = professions_db_path();
    if (!path) {
        std::printf("Consumables: a meal, a drink, a potion and a salve: skipped (no real content.db)\n");
        return;
    }
    std::printf("Consumables: a meal, a drink, a potion and a salve are used through their ability\n");
    svc::Character rec = cyb_record();
    rec.level = 25;
    std::vector<svc::InventoryItem> items;
    for (const auto& it : rec.items)
        if (it.container == Worn) items.push_back(it);
    svc::InventoryItem salad = make_item(Bag, 4, gc::kFreshSalad, 340, 1);
    salad.quality = 71;   // Legendary: the effect's numbers come from the item's stat block at this rarity
    items.push_back(salad);
    items.push_back(make_item(Bag, 5, gc::kBread, 341, 1));
    items.push_back(make_item(Bag, 6, gc::kAlchemicalAmber, 342, 1));
    items.push_back(make_item(Bag, 7, gc::kHealthPotion, 343, 2));
    items.push_back(make_item(Bag, 8, gc::kHealthSalve, 344, 1));
    items.push_back(make_item(Bag, 9, gc::kRations, 345, 1));
    rec.items = items;
    Harness h(rec, true, true, path);
    const sg::CharacterStats* cs = h.s->character_stats();
    const bs::PlayerSetup& setup = h.s->bootstrap().setup();
    CHECK(h.mirror.unreadable == 0 && !h.s->failed(), "(joined)");
    if (!cs) return;
    const auto listed = [&](int64_t a) { return std::find(h.mirror.records.begin(), h.mirror.records.end(), a) != h.mirror.records.end(); };
    CHECK(listed(gc::kFreshSaladAbility) && listed(gc::kBreadAbility) && listed(gc::kAlchemicalAmberAbility) && listed(gc::kHealthPotionAbility) &&
              listed(gc::kHealthSalveAbility) && listed(gc::kRationsAbility),
          "the join lists the abilities of the meal, the drink, the potion, the salve and the rations (Food, Drink, Potion, Salve are the kinds the server now runs)");
    {
        std::string error;
        CHECK(h.fx.content && h.fx.content->consumable_ability(gc::kFreshSalad, error) == gc::kFreshSaladAbility, "ContentDb::consumable_ability answers for a meal");
        CHECK(h.fx.content && h.fx.content->consumable_ability(gc::kMightPotion, error) == gc::kMightAbility, "the Might Potion casts the ability the client names (Arcane Surge's)");
        CHECK(h.fx.content && h.fx.content->consumable_ability(fi::kSword2H, error) == 0, "... and is 0 for an item that casts no ability");
        CHECK(error.empty(), "(no query failed)");
    }

    const auto use = [&](int slot, int64_t ability, int64_t hit, uint32_t guid, int32_t id) {
        const svc::InventoryItem* item = h.s->inventory().at(Bag, slot);
        if (!item) return false;
        std::vector<fake::Bunch> sent;
        sent.push_back(h.client.actor(h.pawn_ch, item_used(3, slot, guid_n(guid)), true));
        sent.push_back(h.client.actor(h.pawn_ch, trigger_with_item(setup.guids.ability, ability, id, slot, items::to_instance(*item), 3), true));
        if (hit != 0) sent.push_back(h.client.actor(h.pawn_ch, hit_request_for(setup, ability, kSelfTargetingGuid, hit), true));
        h.client_sends(std::move(sent));
        for (int i = 0; i < 25; ++i) h.step();
        return true;
    };

    // the meal: the Legendary salad's own 311 PhysicalPowerRating for 3,600 s, Nourishment for 20 s and Resting while the character eats
    const float power_before = cs->find(gc::kPhysicalPowerRating)->value;
    CHECK(use(4, gc::kFreshSaladAbility, gc::kFreshSaladHit, 340, 71), "the Fresh Salad is used");
    CHECK(h.s->inventory().at(Bag, 4) == nullptr && h.s->uses_applied() == 1 && h.s->uses_refused() == 0 && mirror_matches(h), "the salad is taken from the bag, no refusal");
    const Mirror::EffectSeen* salad_buff = effect_of(h, gc::kFreshSaladEffect);
    CHECK(salad_buff && salad_buff->duration == 3600.0f, "the meal's buff is on the pawn for 3,600 s");
    CHECK(cs->find(gc::kPhysicalPowerRating)->value == power_before + 311.0f, "PhysicalPowerRating rose by the Legendary salad's own 311 (not the Common 155)");
    const Mirror::EffectSeen* nourishment = effect_of(h, gc::kFreshSaladNourishment);
    CHECK(nourishment && nourishment->duration == 20.0f, "Nourishment is on for its 20 s");
    const Mirror::EffectSeen* resting = effect_of(h, gc::kRestingEffect);
    CHECK(resting && resting->duration > 1e30f, "Resting is on while the character eats");
    CHECK(stats_match(h), "the client's stat arrays are the session's");
    // a second meal replaces the first (one food buff at a time): Bread's Common 1,214 MaxHealth, the salad's rating back
    const float max_health_before = cs->find(gc::kMaxHealthStat)->value;
    CHECK(use(5, gc::kBreadAbility, gc::kBreadHit, 341, 72), "Bread is used");
    CHECK(effect_of(h, gc::kFreshSaladEffect) == nullptr, "the second meal replaced the first: one food buff at a time (Consumable_Food)");
    CHECK(cs->find(gc::kPhysicalPowerRating)->value == power_before, "... so the salad's 311 are gone");
    CHECK(effect_of(h, gc::kBreadEffect) && effect_of(h, gc::kBreadEffect)->duration == 3600.0f, "Bread's buff lasts 3,600 s");
    CHECK(cs->find(gc::kMaxHealthStat)->value == max_health_before + 1214.0f, "Bread adds its Common 1,214 MaxHealth");
    CHECK(h.s->inventory().at(Bag, 5) == nullptr && mirror_matches(h), "the bread is taken");

    // the drink: Alchemical Amber's Common 296 Alchemy rating and 1,184 MagicalCriticalPower for 3,600 s
    const float alchemy_before = cs->find(gc::kAlchemyRarityRating)->value;
    const float crit_power_before = cs->find(gc::kMagicalCriticalPowerRating)->value;
    CHECK(use(6, gc::kAlchemicalAmberAbility, gc::kAlchemicalAmberHit, 342, 73), "Alchemical Amber is used");
    CHECK(effect_of(h, gc::kAlchemicalAmberEffect) && effect_of(h, gc::kAlchemicalAmberEffect)->duration == 3600.0f, "the drink's buff lasts 3,600 s");
    CHECK(cs->find(gc::kAlchemyRarityRating)->value == alchemy_before + 296.0f, "Artisan_Processing_Alchemy_Rarity_Rating rose by the Common 296");
    CHECK(cs->find(gc::kMagicalCriticalPowerRating)->value == crit_power_before + 1184.0f, "... and MagicalCriticalPowerRating by 1,184");
    CHECK(h.s->inventory().at(Bag, 6) == nullptr && stats_match(h), "the drink is taken and the client agrees");

    // the potion: an instant heal whose number is the item's Health at its quality (the healing formula reads the item through GetAbilityItemInstance)
    const float potion_common = potion_heal(path, 1);
    std::printf("    the Common potion heals %.1f Health from 1\n", potion_common);
    CHECK(potion_common == 1052.0f, "the Common Health Potion heals its item's 1,052 Health: the formula read the item the cast was made from");
    const float potion_once = potion_heal(path, 1, 2);
    CHECK(potion_once == potion_common, "a second hit request for the same potion is refused: one use heals once");
    double potion_legendary_health = 0;
    for (const stats::StatMod& m : cs->item_stats(gc::kHealthPotion, 71))
        if (m.stat == gc::kHealthStat) potion_legendary_health = m.value;
    CHECK(potion_legendary_health == 2111.0, "the same item at Legendary (quality 71) carries 2,111 Health in its stat block, which is what a Legendary dose heals with");

    // the elixirs whose abilities read their item's stat block
    const std::pair<float, float> carpentry = elixir_gain(path, "Carpentry", 6064632106055368744ll, 6064632320025952411ll, 6064632320025952412ll,
                                                          6064630248357101597ll, 0);
    const std::pair<float, float> wisdom = elixir_gain(path, "Wisdom", 6064632106059694123ll, 6064632320036110506ll, 6064632320036110507ll, 109183598614142ll, 0);
    const std::pair<float, float> weaving = elixir_gain(path, "Weaving", 6064632105802530845ll, 6064632320012058724ll, 6064632320012058725ll,
                                                        6064630299633385503ll, 6064630299632271367ll);
    const std::pair<float, float> tailoring = elixir_gain(path, "Tailoring", 6064632105978822693ll, 6064632320024182924ll, 6064632320024182925ll,
                                                          6064630248539947061ll, 0);
    CHECK(carpentry.first == 458.0f && tailoring.first == 458.0f, "the Carpentry and Tailoring elixirs add their Uncommon 458 quality rating");
    CHECK(wisdom.first == 229.0f, "the Wisdom elixir adds its Uncommon 229 Wisdom");
    CHECK(weaving.first == 458.0f && weaving.second == 458.0f, "the Weaving elixir adds its Uncommon 458 time and quantity ratings");

    // the salve: 20 s of a tick every 2 s, each tick the item's Health; the potion montage ends at 1.86 s, the salve's effect must outlive it
    CHECK(h.s->set_current_value(gc::kHealthStat, 1.0f), "(Health is set to 1 before the salve)");
    const float salve_before = cs->find(gc::kHealthStat)->value;
    CHECK(use(8, gc::kHealthSalveAbility, gc::kHealthSalveHit, 344, 74), "the Apprentice Health Salve is used");
    const Mirror::EffectSeen* salve = effect_of(h, gc::kHealthSalveEffect);
    CHECK(salve && salve->duration == 20.0f, "the salve's effect is on for 20 s");
    const float salve_first = cs->find(gc::kHealthStat)->value;
    std::printf("    the salve's first tick heals %.1f Health\n", salve_first - salve_before);
    CHECK(salve_first > salve_before, "its first tick heals");
    for (int i = 0; i < 200; ++i) h.step();   // 4 s more: the montage is long over
    CHECK(effect_of(h, gc::kHealthSalveEffect) != nullptr, "the effect is still on 4.5 s later: the montage's end does not cut the 20 s of ticks");
    std::printf("    it ticks again: %.1f Health more after 4.5 s\n", cs->find(gc::kHealthStat)->value - salve_first);
    CHECK(cs->find(gc::kHealthStat)->value > salve_first, "and it keeps ticking");
    CHECK(h.s->inventory().at(Bag, 8) == nullptr && mirror_matches(h), "the salve is taken");

    // the rations: a Nourishment that ticks every second for 20 s, each tick the item's own 121 Health and Mana (GetConsumedItemStat on a later tick)
    CHECK(h.s->set_current_value(gc::kHealthStat, 1.0f) && h.s->set_current_value(gc::kManaStat, 1.0f), "(Health and Mana are set to 1 before the rations)");
    const float food_health_before = cs->find(gc::kHealthStat)->value;
    const float food_mana_before = cs->find(gc::kManaStat)->value;
    CHECK(use(9, gc::kRationsAbility, 0, 345, 75), "Apprentice Rations are used");
    const Mirror::EffectSeen* feast = effect_of(h, gc::kRationsEffect);
    CHECK(feast && feast->duration == 20.0f, "Feasting is on for 20 s");
    std::printf("    the first ration tick added %.1f Health and %.1f Mana\n", cs->find(gc::kHealthStat)->value - food_health_before,
                cs->find(gc::kManaStat)->value - food_mana_before);
    CHECK(cs->find(gc::kHealthStat)->value >= food_health_before + 121.0f, "a tick added the rations' 121 Health");
    CHECK(cs->find(gc::kManaStat)->value >= food_mana_before + 121.0f, "... and 121 Mana");
    CHECK(h.s->inventory().at(Bag, 9) == nullptr && mirror_matches(h), "the rations are taken");
}

AOC_REGISTER_TEST(test_consumables_meal_drink_potion_salve);
