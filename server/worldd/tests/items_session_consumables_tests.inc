// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/items_session_consumables_tests.inc: a meal, a drink, a potion and a salve used through their abilities
//  Registered with AOC_REGISTER_TEST; included from items_session_registered.inc.
// ============================================================================

#include "protocol/repl/move_packed.h"

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
inline constexpr int64_t kHealthPotionT3 = 6064632105597730816ll, kHealthPotionT3Ability = 6064632316163064133ll, kHealthPotionT3Hit = 6064632316163064134ll;
inline constexpr int64_t kAnglersSmokedDelight = 6064631986998804533ll, kAnglersSmokedDelightAbility = 6064632320297009632ll,
                         kAnglersSmokedDelightHit = 6064632320297009633ll, kAnglersSmokedDelightEffect = 6064632320297009634ll;
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
    sent.push_back(h.client.actor(h.pawn_ch, hit_request_for(setup, gc::kHealthPotionAbility, kSelfTargetingGuid, gc::kHealthPotionHit), true));
    h.client_sends(std::move(sent));
    for (int i = 0; i < 6 && requests > 1; ++i) h.step();   // 120 ms: past the 0.1 s least gap between two requests of one hit, so only the item cast's one request a hit refuses the second
    for (int i = 1; i < requests; ++i) h.client_sends({h.client.actor(h.pawn_ch, hit_request_for(setup, gc::kHealthPotionAbility, kSelfTargetingGuid, gc::kHealthPotionHit), true)});
    for (int i = 0; i < 25; ++i) h.step();
    return cs->find(gc::kHealthStat)->value - before;
}

/// ServerEndAbility(ServerInstanceId): the client ends the item cast it was given.  A subobject bunch of the AbilityComponent, the shape of the trigger's.
static BunchWriter end_ability(const bs::PlayerSetup& setup, int64_t server_instance) {
    const schema::ClassSchema& cls = class_named("UAoCAbilityComponent");
    const schema::RpcSchema* rpc = cls.rpc("ServerEndAbility");
    repl::RpcParams p;
    BunchWriter out;
    const bool ok = rpc && repl::set_rpc_param(*rpc, p, "ServerInstanceId", repl::Value::i64(server_instance));
    if (!ok) return out;
    BunchWriter field;
    bool failed = false;
    if (repl::write_rpc_field(cls, *rpc, p, field, client_options(), &failed) == 0 || failed) return out;
    repl::BlockHeader header;
    header.is_actor = false;
    header.has_rep_layout = false;
    header.object = setup.guids.ability;
    repl::write_content_block(out, header, field, /*server_form=*/false);
    return out;
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
        for (int i = 0; i < 110; ++i) h.step();   // 2.2 s: the eating or drinking runs its course
        const int64_t server = h.s->pipeline() ? h.s->pipeline()->instance_of(id) : -1;
        if (server >= 0) {
            h.client_sends({h.client.actor(h.pawn_ch, end_ability(setup, server), true)});   // the bar completes: the client ends its use
            for (int i = 0; i < 15; ++i) h.step();
        }
        return true;
    };

    // the meal: the Legendary salad's own 311 PhysicalPowerRating for 3,600 s and Resting while the character eats (no Nourishment: nothing gives a meal one)
    const float power_before = cs->find(gc::kPhysicalPowerRating)->value;
    {
        const svc::InventoryItem* item = h.s->inventory().at(Bag, 4);
        CHECK(item != nullptr, "(the salad is in the bag)");
        if (!item) return;
        h.client_sends({h.client.actor(h.pawn_ch, item_used(3, 4, guid_n(340)), true),
                        h.client.actor(h.pawn_ch, trigger_with_item(setup.guids.ability, gc::kFreshSaladAbility, 71, 4, items::to_instance(*item), 3), true),
                        h.client.actor(h.pawn_ch, hit_request_for(setup, gc::kFreshSaladAbility, kSelfTargetingGuid, gc::kFreshSaladHit), true)});
        for (int i = 0; i < 110; ++i) h.step();   // 2.2 s: the eating runs its course
        CHECK(effect_of(h, gc::kRestingEffect) != nullptr, "Resting is on while the character eats (the meal's own lasting state)");
        const int64_t server = h.s->pipeline() ? h.s->pipeline()->instance_of(71) : -1;
        CHECK(server >= 0, "(the use's item names its ServerInstanceId)");
        if (server >= 0) {
            h.client_sends({h.client.actor(h.pawn_ch, end_ability(setup, server), true)});   // the bar completes: the client ends its use
            for (int i = 0; i < 15; ++i) h.step();
        }
        CHECK(effect_of(h, gc::kRestingEffect) == nullptr, "Resting ended with the eating");
        CHECK(effect_of(h, gc::kFreshSaladNourishment) == nullptr, "no Nourishment comes with the eating: the Food montage has no notify for it and the apply hit does not give it");
    }
    CHECK(h.s->inventory().at(Bag, 4) == nullptr && h.s->uses_applied() == 1 && h.s->uses_refused() == 0 && mirror_matches(h), "the salad is taken from the bag, no refusal");
    const Mirror::EffectSeen* salad_buff = effect_of(h, gc::kFreshSaladEffect);
    CHECK(salad_buff && salad_buff->duration == 3600.0f, "the meal's buff is on the pawn for 3,600 s");
    CHECK(cs->find(gc::kPhysicalPowerRating)->value == power_before + 311.0f, "PhysicalPowerRating rose by the Legendary salad's own 311 (not the Common 155)");
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
    CHECK(potion_once == potion_common, "a second hit request for the same potion, past the least gap between requests, is still refused: one use heals once");
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

void test_item_use_ends() {
    const char* path = professions_db_path();
    if (!path) {
        std::printf("Item use end: skipped (no real content.db)\n");
        return;
    }
    std::printf("A meal's use ends: the client's ServerEndAbility ends it, and a hit request that arrives later applies nothing\n");
    // the client ends the use: Resting goes with it, the meal's own buff stays
    {
        svc::Character rec = cyb_record();
        rec.level = 25;
        std::vector<svc::InventoryItem> items;
        for (const auto& it : rec.items)
            if (it.container == Worn) items.push_back(it);
        items.push_back(make_item(Bag, 4, gc::kFreshSalad, 905, 1));
        rec.items = items;
        Harness h(rec, true, true, path);
        const bs::PlayerSetup& setup = h.s->bootstrap().setup();
        const svc::InventoryItem* item = h.s->inventory().at(Bag, 4);
        if (!item) return;
        std::vector<fake::Bunch> sent;
        sent.push_back(h.client.actor(h.pawn_ch, item_used(3, 4, guid_n(905)), true));
        sent.push_back(h.client.actor(h.pawn_ch, trigger_with_item(setup.guids.ability, gc::kFreshSaladAbility, 81, 4, items::to_instance(*item), 3), true));
        sent.push_back(h.client.actor(h.pawn_ch, hit_request_for(setup, gc::kFreshSaladAbility, kSelfTargetingGuid, gc::kFreshSaladHit), true));
        h.client_sends(std::move(sent));
        for (int i = 0; i < 110; ++i) h.step();   // 2.2 s: Resting comes with the eating's end
        CHECK(effect_of(h, gc::kRestingEffect) != nullptr, "Resting is on while the character eats");
        CHECK(effect_of(h, gc::kFreshSaladEffect) != nullptr, "(the meal's buff is on)");
        const int64_t server = h.s->pipeline() ? h.s->pipeline()->instance_of(81) : -1;
        CHECK(server >= 0, "(the use's item names its ServerInstanceId)");
        const size_t ends = h.s->ability_ends_sent();
        if (server >= 0) {
            h.client_sends({h.client.actor(h.pawn_ch, end_ability(setup, server), true)});
            for (int i = 0; i < 15; ++i) h.step();   // past the 259 ms the end is answered in
        }
        CHECK(effect_of(h, gc::kRestingEffect) == nullptr, "the client's end takes Resting with it");
        CHECK(effect_of(h, gc::kFreshSaladEffect) != nullptr, "... and the meal's 3,600 s buff stays");
        CHECK(h.s->ability_ends_sent() == ends + 1, "the end is answered with ClientEndAbility");
        CHECK(!h.s->failed() && h.mirror.unreadable == 0, "(nothing failed)");
    }
    // the use ends before the meal's hit request arrives: the request applies nothing
    {
        svc::Character rec = cyb_record();
        rec.level = 25;
        std::vector<svc::InventoryItem> items;
        for (const auto& it : rec.items)
            if (it.container == Worn) items.push_back(it);
        items.push_back(make_item(Bag, 4, gc::kFreshSalad, 906, 1));
        rec.items = items;
        Harness h(rec, true, true, path);
        const bs::PlayerSetup& setup = h.s->bootstrap().setup();
        const svc::InventoryItem* item = h.s->inventory().at(Bag, 4);
        if (!item) return;
        std::vector<fake::Bunch> sent;
        sent.push_back(h.client.actor(h.pawn_ch, item_used(3, 4, guid_n(906)), true));
        sent.push_back(h.client.actor(h.pawn_ch, trigger_with_item(setup.guids.ability, gc::kFreshSaladAbility, 82, 4, items::to_instance(*item), 3), true));
        h.client_sends(std::move(sent));
        for (int i = 0; i < 8; ++i) h.step();   // 160 ms: past the item step, so the use's instance is on its item; the effects (2 s) are still pending
        const int64_t server = h.s->pipeline() ? h.s->pipeline()->instance_of(82) : -1;
        CHECK(server >= 0, "(the use runs)");
        if (server >= 0) {
            h.client_sends({h.client.actor(h.pawn_ch, end_ability(setup, server), true)});
            for (int i = 0; i < 3; ++i) h.step();
        }
        CHECK(effect_of(h, gc::kRestingEffect) == nullptr, "(the meal's Resting comes when its montage reaches the notify that gives it, not before: a use ended sooner never gets it)");
        for (int i = 0; i < 15; ++i) h.step();   // past the 259 ms the end is answered in: the use is over, and its buff is given at its end
        const Mirror::EffectSeen* buff = effect_of(h, gc::kFreshSaladEffect);
        const float buff_duration = buff ? buff->duration : -1.0f;
        const size_t added = static_cast<size_t>(std::count(h.mirror.sequence.begin(), h.mirror.sequence.end(), std::string("E+")));
        // (a hit that gave the buff again would refresh it in place, with the same 3,600 s: only its end on the server moves)
        const std::optional<double> buff_end = h.s->pipeline() ? h.s->pipeline()->effect_end(gc::kFreshSaladEffect) : std::nullopt;
        h.client_sends({h.client.actor(h.pawn_ch, hit_request_for(setup, gc::kFreshSaladAbility, kSelfTargetingGuid, gc::kFreshSaladHit), true)});
        for (int i = 0; i < 125; ++i) h.step();   // 2.5 s: past the 2.0 s before which a meal's hit cannot land
        CHECK(effect_of(h, gc::kRestingEffect) == nullptr, "a hit request after the use ended applies nothing: no Resting is left on for good");
        CHECK(buff != nullptr && effect_of(h, gc::kFreshSaladEffect) == buff && buff->duration == buff_duration &&
                  static_cast<size_t>(std::count(h.mirror.sequence.begin(), h.mirror.sequence.end(), std::string("E+"))) == added,
              "... and its hit does not give the meal's buff again: no effect is added after the end");
        CHECK(buff_end.has_value() && h.s->pipeline() && h.s->pipeline()->effect_end(gc::kFreshSaladEffect) == buff_end,
              "... and the buff is not refreshed: its end is where the use's end put it");
        CHECK(!h.s->failed() && h.mirror.unreadable == 0, "(nothing failed)");
    }
}
AOC_REGISTER_TEST(test_item_use_ends);

// The Resting effect is one: the Rest ability's own and a meal's or a drink's own use the same record, and the character never carries two of it — the meal's
// joins the Rest's while the Rest runs, and the Rest's takes a running use's over when its own start comes after it (the sit-down's delay can put it there).
void test_resting_effect_is_one() {
    const char* path = professions_db_path();
    if (!path) {
        std::printf("Resting effect is one: skipped (no real content.db)\n");
        return;
    }
    const auto resting_count = [](const Mirror& m) {
        size_t n = 0;
        for (const auto& [id, seen] : m.effect_items)
            if (seen.guid == gc::kRestingEffect) ++n;
        return n;
    };
    // the Rest first: a meal eaten while the Rest runs adds no second Resting, and its own end takes nothing off
    {
        svc::Character rec = cyb_record();
        rec.level = 25;
        std::vector<svc::InventoryItem> items;
        for (const auto& it : rec.items)
            if (it.container == Worn) items.push_back(it);
        items.push_back(make_item(Bag, 4, gc::kFreshSalad, 921, 1));
        rec.items = items;
        Harness h(rec, true, true, path);
        const bs::PlayerSetup& setup = h.s->bootstrap().setup();
        const svc::InventoryItem* item = h.s->inventory().at(Bag, 4);
        if (!item) return;
        h.client_sends({h.client.actor(h.pawn_ch, ability_trigger(setup.guids.ability, content::kGuidRest, 100, 0), true)});   // sit: the Rest starts
        for (int i = 0; i < 110; ++i) h.step();   // 2.2 s: past the sit-down, so the Rest's own Resting is on
        CHECK(resting_count(h.mirror) == 1, "(the Rest's own Resting is on)");
        h.client_sends({h.client.actor(h.pawn_ch, item_used(3, 4, guid_n(921)), true),
                        h.client.actor(h.pawn_ch, trigger_with_item(setup.guids.ability, gc::kFreshSaladAbility, 101, 4, items::to_instance(*item), 3), true)});
        for (int i = 0; i < 110; ++i) h.step();   // the eating runs its course
        CHECK(resting_count(h.mirror) == 1, "a meal eaten while the Rest runs adds no second Resting: the meal's joins the Rest's");
        const int64_t server = h.s->pipeline() ? h.s->pipeline()->instance_of(101) : -1;
        if (server >= 0) {
            h.client_sends({h.client.actor(h.pawn_ch, end_ability(setup, server), true)});   // the bar completes: the client ends its use
            for (int i = 0; i < 15; ++i) h.step();
        }
        CHECK(resting_count(h.mirror) == 1, "... and the eating's end takes nothing off: the Rest's stays for as long as the Rest runs");
        h.client_sends({h.client.actor(h.pawn_ch, ability_trigger(setup.guids.ability, content::kGuidRest, 102, 0), true)});   // the toggle's second press: stand up
        for (int i = 0; i < 15; ++i) h.step();
        CHECK(resting_count(h.mirror) == 0, "the Rest's own end takes the one Resting off");
        CHECK(!h.s->failed() && h.mirror.unreadable == 0, "(nothing failed)");
    }
    // the meal first: the Rest's own start takes its Resting over, and the eating's end still leaves the Rest's on
    {
        svc::Character rec = cyb_record();
        rec.level = 25;
        std::vector<svc::InventoryItem> items;
        for (const auto& it : rec.items)
            if (it.container == Worn) items.push_back(it);
        items.push_back(make_item(Bag, 4, gc::kFreshSalad, 922, 1));
        rec.items = items;
        Harness h(rec, true, true, path);
        const bs::PlayerSetup& setup = h.s->bootstrap().setup();
        const svc::InventoryItem* item = h.s->inventory().at(Bag, 4);
        if (!item) return;
        h.client_sends({h.client.actor(h.pawn_ch, item_used(3, 4, guid_n(922)), true),
                        h.client.actor(h.pawn_ch, trigger_with_item(setup.guids.ability, gc::kFreshSaladAbility, 103, 4, items::to_instance(*item), 3), true)});
        for (int i = 0; i < 110; ++i) h.step();   // the eating runs, and its own Resting is on
        CHECK(resting_count(h.mirror) == 1, "(the meal's own Resting is on)");
        h.client_sends({h.client.actor(h.pawn_ch, ability_trigger(setup.guids.ability, content::kGuidRest, 104, 0), true)});   // sit: the Rest starts under the eating
        for (int i = 0; i < 110; ++i) h.step();   // past the sit-down, so the Rest's own Resting comes
        CHECK(resting_count(h.mirror) == 1, "the Rest's own start takes the meal's Resting over: still one, never two");
        const int64_t server = h.s->pipeline() ? h.s->pipeline()->instance_of(103) : -1;
        if (server >= 0) {
            h.client_sends({h.client.actor(h.pawn_ch, end_ability(setup, server), true)});   // the bar completes
            for (int i = 0; i < 15; ++i) h.step();
        }
        CHECK(resting_count(h.mirror) == 1, "the eating's end finds no instance of its own: the Rest's stays on");
        h.client_sends({h.client.actor(h.pawn_ch, ability_trigger(setup.guids.ability, content::kGuidRest, 105, 0), true)});   // stand up
        for (int i = 0; i < 15; ++i) h.step();
        CHECK(resting_count(h.mirror) == 0, "the Rest's own end takes the one Resting off");
        CHECK(!h.s->failed() && h.mirror.unreadable == 0, "(nothing failed)");
    }
}
AOC_REGISTER_TEST(test_resting_effect_is_one);

static size_t count_text(const std::string& text, const std::string& needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) ++n;
    return n;
}

/// The montage NetGUID of the server's ability item for a client instance (0: no item, or its montage is cleared).
static uint64_t active_montage(const Harness& h, int64_t instance) {
    const bs::SessionGuids& g = h.s->bootstrap().setup().guids;
    const repl::Subobject* s = h.s->bootstrap().pawn().find(g.ability);
    if (!s || !s->obj.schema) return 0;
    repl::FastArrays arrays = s->obj.arrays;
    const repl::FastArrayState* arr = arrays.get(*s->obj.schema, "ReplicatedActiveAbilities");
    if (!arr) return 0;
    for (const repl::FastArrayItem& it : arr->items()) {
        const auto id = it.values.values.find(2);
        if (id == it.values.values.end() || !id->second.get_if<int64_t>() || *id->second.get_if<int64_t>() != instance) continue;
        const auto m = it.values.values.find(11);
        return m != it.values.values.end() && m->second.get_if<emit::FIntrepidNetworkGUID>() ? m->second.get_if<emit::FIntrepidNetworkGUID>()->ObjectId : 0;
    }
    return 0;
}

/// What the server's ability item for a client instance tells the client: whether it is there, its montage (0: cleared), the position the montage is told to
/// jump to, whether it may jump there, its animation counter, and whether it says the client took the branch itself (bBranchPredictedOnAutonomous).  `key` is the item's
/// ReplicationKey: it goes one on with every change the item is sent in, so two reads with the same key mean the item was not sent again between them.
struct ActiveItem {
    bool     present = false;
    uint64_t montage = 0;
    float    position = 0;
    bool     jump = false;
    int64_t  counter = 0;
    bool     predicted = false;
    int32_t  key = 0;
};

static ActiveItem active_item(const Harness& h, int64_t instance) {
    ActiveItem out;
    const bs::SessionGuids& g = h.s->bootstrap().setup().guids;
    const repl::Subobject* s = h.s->bootstrap().pawn().find(g.ability);
    if (!s || !s->obj.schema) return out;
    repl::FastArrays arrays = s->obj.arrays;
    const repl::FastArrayState* arr = arrays.get(*s->obj.schema, "ReplicatedActiveAbilities");
    if (!arr) return out;
    for (const repl::FastArrayItem& it : arr->items()) {
        const auto id = it.values.values.find(2);
        if (id == it.values.values.end() || !id->second.get_if<int64_t>() || *id->second.get_if<int64_t>() != instance) continue;
        out.present = true;
        out.key = it.key;
        const auto value = [&](int handle) -> const repl::Value* {
            const auto v = it.values.values.find(handle);
            return v == it.values.values.end() ? nullptr : &v->second;
        };
        if (const repl::Value* v = value(11); v && v->get_if<emit::FIntrepidNetworkGUID>()) out.montage = v->get_if<emit::FIntrepidNetworkGUID>()->ObjectId;
        if (const repl::Value* v = value(13); v && v->get_if<float>()) out.position = *v->get_if<float>();
        if (const repl::Value* v = value(9); v && v->get_if<bool>()) out.predicted = *v->get_if<bool>();
        if (const repl::Value* v = value(14); v && v->get_if<bool>()) out.jump = *v->get_if<bool>();
        if (const repl::Value* v = value(15); v && v->get_if<int64_t>()) out.counter = *v->get_if<int64_t>();
        break;
    }
    return out;
}

/// Where the montage of a meal or a drink is told to jump when the character stands up: the start of its last section, the one the eating ability's own branch for
/// walking or taking damage names.  A ration's montage is another asset with the same sections, and its last section starts later.
constexpr float kEatingEnd = 17.890932f;
constexpr float kRationEnd = 28.802214f;
constexpr int64_t kFlourishAbility = 6064630877217018486ll;   // Bard_Misc_Flourish: an ordinary ability of the Bard's, not an item's

/// The client's own moves: 10 cm along the ground, 0.1 s after the one before (a player walking).
struct Walker {
    float  ts = 100.0f;
    double x = bs::kDefaultSpawn.x;
};

static BunchWriter walk_bunch(const Walker& w) {
    repl::NetworkMove m;
    m.time_stamp = w.ts;
    m.acceleration = repl::quantize_vector(10, 100, 0, 0);
    m.location = repl::quantize_vector(100, w.x, bs::kDefaultSpawn.y, bs::kDefaultSpawn.z);
    m.control_rotation.yaw = static_cast<uint16_t>(std::llround(bs::kDefaultSpawn.yaw_degrees / 360.0 * 65536.0) & 0xffff);
    m.movement_mode = repl::kMoveWalking;
    m.input_bits = 1;
    const schema::ClassSchema& cls = class_named("PlayerPawn_C");
    const schema::NetField* f = cls.field("ServerMovePacked");
    BunchWriter out;
    repl::MovePacked p;
    p.new_move = m;
    BunchWriter params;
    BunchWriter field;
    if (!f || !repl::write_move_packed(params, p) || repl::write_field(cls, f->index, params, field) == 0) return out;
    repl::BlockHeader header;
    repl::write_content_block(out, header, field, /*server_form=*/false);
    return out;
}

/// One move of the walk, sent now (the server's tick that takes the answer is the caller's).
static void walk(Harness& h, Walker& w) {
    w.ts += 0.1f;
    w.x += 10.0;
    h.client_sends({h.client.actor(h.pawn_ch, walk_bunch(w), /*reliable=*/false)});
}

static void run_steps(Harness& h, int n) {
    for (int i = 0; i < n; ++i) h.step();
}

static size_t effects_added(const Harness& h) { return static_cast<size_t>(std::count(h.mirror.sequence.begin(), h.mirror.sequence.end(), std::string("E+"))); }

/// How many instances of an effect the client's CurrentEffects holds.
static size_t effect_count_of(const Harness& h, int64_t guid) {
    size_t n = 0;
    for (const auto& [id, seen] : h.mirror.effect_items) n += seen.guid == guid ? 1 : 0;
    return n;
}

/// A level-25 character with one of `item` in bag slot 4 (and the same of `second` in slot 5, when it is named).
static svc::Character bag_of(int64_t item, uint32_t guid, int64_t second = 0, uint32_t second_guid = 0) {
    svc::Character rec = cyb_record();
    rec.level = 25;
    std::vector<svc::InventoryItem> items;
    for (const auto& it : rec.items)
        if (it.container == Worn) items.push_back(it);
    items.push_back(make_item(Bag, 4, item, guid, 1));
    if (second != 0) items.push_back(make_item(Bag, 5, second, second_guid, 1));
    rec.items = items;
    return rec;
}

/// The client uses the item in `slot`: the item use and the trigger of its ability, in one datagram.
static bool use_item(Harness& h, int slot, uint32_t guid, int64_t ability, int32_t id) {
    const svc::InventoryItem* item = h.s->inventory().at(Bag, slot);
    if (!item) return false;
    const bs::PlayerSetup& setup = h.s->bootstrap().setup();
    h.client_sends({h.client.actor(h.pawn_ch, item_used(3, slot, guid_n(guid)), true),
                    h.client.actor(h.pawn_ch, trigger_with_item(setup.guids.ability, ability, id, slot, items::to_instance(*item), 3), true)});
    return true;
}

static void ask_hit(Harness& h, int64_t ability, int64_t hit) {
    h.client_sends({h.client.actor(h.pawn_ch, hit_request_for(h.s->bootstrap().setup(), ability, kSelfTargetingGuid, hit), true)});
}

void test_consumables_meal_stays_seated() {
    const char* path = professions_db_path();
    if (!path) {
        std::printf("Consumables: a meal and a drink stay seated: skipped (no real content.db)\n");
        return;
    }
    std::printf("Consumables: a meal and a drink give their buff at the apply hit, and the character stays seated until they walk\n");
    // The meal: the recorded apply request comes 6.95 s after the use (the bar is the Food montage's length; the ability record carries no cast time).
    {
        Harness h(bag_of(gc::kAnglersSmokedDelight, 950), true, true, path);
        CHECK(h.mirror.unreadable == 0 && !h.s->failed(), "(joined)");
        if (!use_item(h, 4, 950, gc::kAnglersSmokedDelightAbility, 951)) return;
        run_steps(h, 347);   // 6.94 s
        const int64_t server = h.s->pipeline() ? h.s->pipeline()->instance_of(951) : -1;
        CHECK(effect_of(h, gc::kRestingEffect) != nullptr && server >= 0 && active_item(h, server).montage != 0, "Resting is on and the item carries the eating montage while the bar runs");
        CHECK(!active_item(h, server).jump && effect_of(h, gc::kAnglersSmokedDelightEffect) == nullptr, "(the montage has not been told to jump, and the buff waits for the hit)");
        const size_t ends = h.s->ability_ends_sent();
        const size_t added = effects_added(h);
        ask_hit(h, gc::kAnglersSmokedDelightAbility, gc::kAnglersSmokedDelightHit);
        run_steps(h, 6);   // 120 ms: the hit's answer
        const Mirror::EffectSeen* buff = effect_of(h, gc::kAnglersSmokedDelightEffect);
        CHECK(buff != nullptr, "the apply hit gives the meal's buff");
        CHECK(effect_of(h, gc::kFreshSaladNourishment) == nullptr, "... and no Nourishment comes with it: the hit does not give it and no notify of the montage does");
        CHECK(effects_added(h) == added + 1, "the buff is applied once, and nothing else");
        run_steps(h, 250);   // 5 s later, the client sending nothing more
        CHECK(h.s->pipeline() && h.s->pipeline()->instance_of(951) == server, "5 s after the hit the use is still there: the hit does not end it");
        const ActiveItem seated = active_item(h, server);
        CHECK(seated.present && seated.montage != 0 && !seated.jump && seated.position == 0.0f, "... its item still carries the eating montage, not told to jump");
        CHECK(effect_of(h, gc::kRestingEffect) != nullptr, "... and Resting is still on");
        CHECK(h.s->ability_ends_sent() == ends, "no ClientEndAbility was sent");
        CHECK(effect_of(h, gc::kAnglersSmokedDelightEffect) == buff && effects_added(h) == added + 1 && effect_of(h, gc::kFreshSaladNourishment) == nullptr,
              "the buff is still the one the hit applied, and there is still no Nourishment");
        CHECK(h.s->inventory().at(Bag, 4) == nullptr && h.s->uses_applied() == 1, "the meal was taken once");
        // The player walks: the montage jumps to its end, Resting goes 0.067 s later, and the use is over.
        Walker w;
        const size_t moves = h.s->moves_acknowledged();
        walk(h, w);
        h.step();   // 20 ms
        CHECK(h.s->moves_acknowledged() == moves + 1, "(the move was taken)");
        const ActiveItem jumped = active_item(h, server);
        CHECK(jumped.present && jumped.jump && jumped.position == kEatingEnd && jumped.counter == seated.counter + 1,
              "walking: the item's montage jumps to its end section, AnimationCounter one on");
        CHECK(jumped.montage == seated.montage, "... the item keeps its montage, which plays its way out");
        CHECK(effect_of(h, gc::kRestingEffect) != nullptr, "Resting is still on in the update of the jump: it goes 0.067 s later");
        run_steps(h, 4);   // 100 ms
        CHECK(effect_of(h, gc::kRestingEffect) == nullptr, "Resting is gone 0.067 s after the jump");
        CHECK(h.s->pipeline() && h.s->pipeline()->instance_of(951) == -1, "the use is over");
        CHECK(effect_of(h, gc::kAnglersSmokedDelightEffect) == buff && effect_of(h, gc::kFreshSaladNourishment) == nullptr && effects_added(h) == added + 1,
              "the buff stays, applied once, and there is no Nourishment");
        CHECK(h.s->ability_ends_sent() == ends, "(still no ClientEndAbility)");
        CHECK(active_item(h, server).counter == jumped.counter, "(one jump, not one for every move)");
        walk(h, w);
        run_steps(h, 6);
        CHECK(active_item(h, server).counter == jumped.counter && !h.s->failed(), "a second move of the walk jumps nothing again");
        ask_hit(h, gc::kAnglersSmokedDelightAbility, gc::kAnglersSmokedDelightHit);
        run_steps(h, 15);
        CHECK(effects_added(h) == added + 1 && effect_of(h, gc::kRestingEffect) == nullptr && effect_of(h, gc::kAnglersSmokedDelightEffect) == buff,
              "a late repeated hit does nothing: no Resting, no second buff");
    }
    // The drink shares the Food montage and the same shape: its apply hit is the bar's end, and it too stays seated until the walk.
    {
        Harness h(bag_of(gc::kAlchemicalAmber, 952), true, true, path);
        if (!use_item(h, 4, 952, gc::kAlchemicalAmberAbility, 953)) return;
        run_steps(h, 347);   // 6.94 s
        const int64_t server = h.s->pipeline() ? h.s->pipeline()->instance_of(953) : -1;
        const size_t ends = h.s->ability_ends_sent();
        CHECK(effect_of(h, gc::kRestingEffect) != nullptr && server >= 0 && active_item(h, server).montage != 0, "(the drink is running and its item carries the montage at its bar's end)");
        ask_hit(h, gc::kAlchemicalAmberAbility, gc::kAlchemicalAmberHit);
        run_steps(h, 256);   // 5.1 s
        CHECK(effect_of(h, gc::kAlchemicalAmberEffect) && effect_of(h, gc::kAlchemicalAmberEffect)->duration == 3600.0f, "the drink's buff is on for 3,600 s");
        CHECK(h.s->pipeline() && h.s->pipeline()->instance_of(953) == server && effect_of(h, gc::kRestingEffect) != nullptr && active_item(h, server).montage != 0,
              "the drink's use is still there 5 s on, with Resting and its montage");
        Walker w;
        walk(h, w);
        run_steps(h, 6);
        const ActiveItem jumped = active_item(h, server);
        CHECK(jumped.jump && jumped.position == kEatingEnd, "walking: the drink's montage jumps to the same end section");
        CHECK(effect_of(h, gc::kRestingEffect) == nullptr && h.s->pipeline() && h.s->pipeline()->instance_of(953) == -1, "Resting is gone and the use is over");
        CHECK(effect_of(h, gc::kAlchemicalAmberEffect) != nullptr && h.s->ability_ends_sent() == ends, "the buff stays, and no ClientEndAbility was sent");
    }
    // A client that ends the use itself still works: its ServerEndAbility ends it and is answered.
    {
        Harness h(bag_of(gc::kAnglersSmokedDelight, 954), true, true, path);
        const bs::PlayerSetup& setup = h.s->bootstrap().setup();
        if (!use_item(h, 4, 954, gc::kAnglersSmokedDelightAbility, 955)) return;
        run_steps(h, 110);   // 2.2 s: the bar is not at its end, so the use still runs
        CHECK(effect_of(h, gc::kRestingEffect) != nullptr, "(Resting is on while the use runs)");
        const int64_t server = h.s->pipeline() ? h.s->pipeline()->instance_of(955) : -1;
        CHECK(server >= 0, "(the use's item names its ServerInstanceId)");
        const size_t ends = h.s->ability_ends_sent();
        if (server >= 0) {
            h.client_sends({h.client.actor(h.pawn_ch, end_ability(setup, server), true)});
            run_steps(h, 15);   // past the 259 ms the end is answered in
        }
        CHECK(h.s->pipeline() && h.s->pipeline()->instance_of(955) == -1 && effect_of(h, gc::kRestingEffect) == nullptr,
              "the client's end ends the use and takes Resting with it");
        CHECK(active_montage(h, server) == 0, "the item's montage is cleared when the client's end is answered");
        CHECK(effect_of(h, gc::kAnglersSmokedDelightEffect) != nullptr && effect_of(h, gc::kFreshSaladNourishment) == nullptr,
              "the buff is granted at the end the client asked for, and no Nourishment");
        CHECK(h.s->ability_ends_sent() == ends + 1, "the end is answered with ClientEndAbility");
    }
    // A client that ends the use after the hit, while the character is seated: the same end.
    {
        Harness h(bag_of(gc::kAnglersSmokedDelight, 956), true, true, path);
        const bs::PlayerSetup& setup = h.s->bootstrap().setup();
        if (!use_item(h, 4, 956, gc::kAnglersSmokedDelightAbility, 957)) return;
        run_steps(h, 347);
        ask_hit(h, gc::kAnglersSmokedDelightAbility, gc::kAnglersSmokedDelightHit);
        run_steps(h, 50);   // 1 s past the hit
        const int64_t server = h.s->pipeline() ? h.s->pipeline()->instance_of(957) : -1;
        CHECK(server >= 0 && effect_of(h, gc::kRestingEffect) != nullptr, "(the character is seated 1 s after the hit)");
        const size_t ends = h.s->ability_ends_sent();
        const size_t added = effects_added(h);
        if (server >= 0) {
            h.client_sends({h.client.actor(h.pawn_ch, end_ability(setup, server), true)});
            run_steps(h, 15);
        }
        CHECK(h.s->pipeline() && h.s->pipeline()->instance_of(957) == -1 && effect_of(h, gc::kRestingEffect) == nullptr, "the client's end ends the seated use and takes Resting with it");
        CHECK(active_montage(h, server) == 0 && h.s->ability_ends_sent() == ends + 1, "... clears the item's montage and is answered with ClientEndAbility");
        CHECK(effect_of(h, gc::kAnglersSmokedDelightEffect) != nullptr && effects_added(h) == added, "the buff stays and nothing is applied a second time");
    }
}
AOC_REGISTER_TEST(test_consumables_meal_stays_seated);

void test_consumables_stand_up() {
    const char* path = professions_db_path();
    if (!path) {
        std::printf("Consumables: standing up from a meal: skipped (no real content.db)\n");
        return;
    }
    std::printf("Consumables: a seated eater stands up when they walk before the bar fills, use another ability, eat again or log in again\n");
    // A walk before the bar fills: the use ends, the hit that comes later gives nothing, and the meal stays spent.
    {
        Harness h(bag_of(gc::kAnglersSmokedDelight, 960), true, true, path);
        if (!use_item(h, 4, 960, gc::kAnglersSmokedDelightAbility, 961)) return;
        run_steps(h, 150);   // 3 s: Resting is on, the bar is not full
        const int64_t server = h.s->pipeline() ? h.s->pipeline()->instance_of(961) : -1;
        CHECK(server >= 0 && effect_of(h, gc::kRestingEffect) != nullptr, "(the character is eating and Resting is on)");
        const ActiveItem seated = active_item(h, server);
        const size_t ends = h.s->ability_ends_sent();
        const size_t added = effects_added(h);
        Walker w;
        walk(h, w);
        run_steps(h, 5);   // 100 ms
        const ActiveItem jumped = active_item(h, server);
        CHECK(jumped.jump && jumped.position == kEatingEnd && jumped.counter == seated.counter + 1, "walking before the bar fills: the montage jumps to its end section");
        CHECK(effect_of(h, gc::kRestingEffect) == nullptr && h.s->pipeline() && h.s->pipeline()->instance_of(961) == -1, "... Resting is gone and the use is over");
        run_steps(h, 192);   // up to 6.95 s after the use
        ask_hit(h, gc::kAnglersSmokedDelightAbility, gc::kAnglersSmokedDelightHit);
        run_steps(h, 15);
        CHECK(effect_of(h, gc::kAnglersSmokedDelightEffect) == nullptr && effect_of(h, gc::kFreshSaladNourishment) == nullptr && effects_added(h) == added,
              "the apply hit that comes after the walk gives neither the buff nor the Nourishment");
        CHECK(effect_of(h, gc::kRestingEffect) == nullptr && h.s->ability_ends_sent() == ends, "... and does not sit the character down again; no ClientEndAbility was sent");
        CHECK(h.s->inventory().at(Bag, 4) == nullptr && h.s->uses_applied() == 1, "the meal stays spent");
    }
    // Another ability while seated: a Dodge (the runtime's), and an ordinary ability of the Bard's (the pipeline's).
    for (int which = 0; which < 2; ++which) {
        const bool dodge = which == 0;
        Harness h(bag_of(gc::kAnglersSmokedDelight, static_cast<uint32_t>(970 + which)), true, true, path);
        const bs::PlayerSetup& setup = h.s->bootstrap().setup();
        const int32_t meal_id = 1000 + which, other_id = 1010 + which;
        if (!use_item(h, 4, static_cast<uint32_t>(970 + which), gc::kAnglersSmokedDelightAbility, meal_id)) return;
        run_steps(h, 347);
        ask_hit(h, gc::kAnglersSmokedDelightAbility, gc::kAnglersSmokedDelightHit);
        run_steps(h, 256);   // 5.1 s: seated
        const int64_t server = h.s->pipeline() ? h.s->pipeline()->instance_of(meal_id) : -1;
        CHECK(server >= 0 && effect_of(h, gc::kRestingEffect) != nullptr, dodge ? "(seated 5 s after the hit, before the Dodge)" : "(seated 5 s after the hit, before the other ability)");
        const ActiveItem seated = active_item(h, server);
        if (!dodge) {
            CHECK(h.s->pipeline() && h.s->pipeline()->known(kFlourishAbility), "(the pipeline knows Flourish)");
        }
        const size_t ends = h.s->ability_ends_sent();
        h.client_sends({h.client.actor(h.pawn_ch, ability_trigger(setup.guids.ability, dodge ? content::kGuidDodge : kFlourishAbility, other_id, 0), true)});
        h.step();
        const ActiveItem jumped = active_item(h, server);
        CHECK(h.s->ability_ends_sent() == ends, dodge ? "(the Dodge is taken)" : "(the other ability is taken)");
        CHECK(jumped.jump && jumped.position == kEatingEnd && jumped.counter == seated.counter + 1,
              dodge ? "a Dodge: the eating montage jumps to its end section" : "another ability: the eating montage jumps to its end section");
        run_steps(h, 5);
        CHECK(effect_of(h, gc::kRestingEffect) == nullptr && h.s->pipeline() && h.s->pipeline()->instance_of(meal_id) == -1,
              dodge ? "... Resting is gone and the use is over" : "... Resting is gone and the use is over, whatever the other ability's item does");
        CHECK(effect_count_of(h, gc::kAnglersSmokedDelightEffect) == 1 && effect_count_of(h, gc::kFreshSaladNourishment) == 0, "the buff stays, one of it, and there is no Nourishment");
    }
    // A second meal while seated: the first Resting goes in the update of the second use's item, and the second meal's buff replaces the first's.
    {
        Harness h(bag_of(gc::kAnglersSmokedDelight, 990, gc::kBread, 991), true, true, path);
        if (!use_item(h, 4, 990, gc::kAnglersSmokedDelightAbility, 992)) return;
        run_steps(h, 347);
        ask_hit(h, gc::kAnglersSmokedDelightAbility, gc::kAnglersSmokedDelightHit);
        run_steps(h, 256);   // 5.1 s: seated
        const int64_t first = h.s->pipeline() ? h.s->pipeline()->instance_of(992) : -1;
        CHECK(first >= 0 && effect_of(h, gc::kRestingEffect) != nullptr && effect_of(h, gc::kAnglersSmokedDelightEffect) != nullptr, "(the first meal is eaten and the character is seated)");
        const ActiveItem seated = active_item(h, first);
        if (!use_item(h, 5, 991, gc::kBreadAbility, 993)) return;
        int64_t second = -1;
        int steps = 0;
        for (; steps < 20 && second < 0; ++steps) {
            h.step();
            const int64_t id = h.s->pipeline() ? h.s->pipeline()->instance_of(993) : -1;
            if (id >= 0 && active_item(h, id).present) second = id;
        }
        CHECK(second >= 0, "(the second meal's item comes)");
        CHECK(!active_item(h, first).present && h.s->pipeline() && h.s->pipeline()->instance_of(992) == -1, "the first use's item went with the second one's coming, and the first use is over");
        CHECK(effect_of(h, gc::kRestingEffect) == nullptr, "the first Resting is gone in the same update as the second use's item");
        CHECK(seated.present && !active_item(h, second).jump, "(the second meal's item is a new one, not told to jump)");
        run_steps(h, 110);   // 2.2 s
        size_t resting = 0;
        for (const auto& [id, seen] : h.mirror.effect_items) resting += seen.guid == gc::kRestingEffect ? 1 : 0;
        CHECK(resting == 1, "the second meal sits the character down again: one Resting");
        run_steps(h, 237);   // up to 6.95 s after the second use
        ask_hit(h, gc::kBreadAbility, gc::kBreadHit);
        run_steps(h, 6);
        CHECK(effect_of(h, gc::kBreadEffect) && effect_of(h, gc::kBreadEffect)->duration == 3600.0f && effect_of(h, gc::kAnglersSmokedDelightEffect) == nullptr,
              "the second meal's buff replaces the first's: one food buff at a time");
        CHECK(h.s->pipeline() && h.s->pipeline()->instance_of(993) == second && effect_of(h, gc::kRestingEffect) != nullptr, "the second meal's use is seated");
    }
    // A new login while seated: the meal that was eaten is gone, and nothing of the seat or the buff comes back.
    {
        const svc::Character rec = bag_of(gc::kAnglersSmokedDelight, 995);
        Harness h(rec, true, true, path);
        if (!use_item(h, 4, 995, gc::kAnglersSmokedDelightAbility, 996)) return;
        run_steps(h, 347);
        ask_hit(h, gc::kAnglersSmokedDelightAbility, gc::kAnglersSmokedDelightHit);
        run_steps(h, 106);   // 2.1 s past the hit
        CHECK(h.s->pipeline() && h.s->pipeline()->instance_of(996) >= 0 && effect_of(h, gc::kRestingEffect) != nullptr && effect_of(h, gc::kAnglersSmokedDelightEffect) != nullptr,
              "(the character is seated with the buff on when they log out)");
        svc::Character saved = rec;
        saved.items = h.s->inventory().items();
        Harness g(saved, true, true, path);
        CHECK(g.s->inventory().at(Bag, 4) == nullptr, "a new login does not bring the eaten meal back");
        CHECK(effect_of(g, gc::kAnglersSmokedDelightEffect) == nullptr && effect_of(g, gc::kFreshSaladNourishment) == nullptr && effect_of(g, gc::kRestingEffect) == nullptr,
              "... and does not bring back its buff, its Nourishment or Resting");
        CHECK(g.s->pipeline() && g.s->pipeline()->instance_of(996) == -1, "... and the new session has no use running");
        Walker w;
        walk(g, w);
        run_steps(g, 10);
        CHECK(!g.s->failed() && g.mirror.unreadable == 0, "(a walk in the new session is nothing to stand up from)");
    }
    // Rations are another montage with the same sections: their end section starts later.
    {
        Harness h(bag_of(gc::kRations, 997), true, true, path);
        if (!use_item(h, 4, 997, gc::kRationsAbility, 998)) return;
        run_steps(h, 150);   // 3 s
        const int64_t server = h.s->pipeline() ? h.s->pipeline()->instance_of(998) : -1;
        CHECK(server >= 0 && effect_of(h, gc::kRestingEffect) != nullptr, "(the character eats the rations, Resting is on)");
        Walker w;
        walk(h, w);
        run_steps(h, 5);
        const ActiveItem jumped = active_item(h, server);
        CHECK(jumped.jump && jumped.position == kRationEnd, "walking: the rations' montage jumps to its own end section");
        CHECK(effect_of(h, gc::kRestingEffect) == nullptr && h.s->pipeline() && h.s->pipeline()->instance_of(998) == -1, "... Resting is gone and the use is over");
    }
}
AOC_REGISTER_TEST(test_consumables_stand_up);

/// The item a seated eater stands up with is one the server decided, not one the client took the branch of: it does not say bBranchPredictedOnAutonomous, as the item of
/// any eating use the server decides does not.  And the montage it jumped to its last section reaches its end by itself: the item then loses its montage, and stays in the
/// list.  The montage of a meal or a drink is 18.93513 s long and its jump goes to 17.890932 s, so it ends 1.044 s after the jump; the montage of rations is 29.97513 s long
/// with its jump to 28.802214 s: it ends 1.173 s after.
void test_consumables_stand_up_item() {
    const char* path = professions_db_path();
    if (!path) {
        std::printf("Consumables: the item a seated eater stands up with: skipped (no real content.db)\n");
        return;
    }
    std::printf("Consumables: a seated eater stands up with an item the server decided, and its montage is cleared when it reaches its end\n");
    struct Eater {
        const char* what;
        int64_t     item, ability;
        float       end;             // where its montage is told to jump
        bool        hit;             // its apply hit came before the walk (the bar of the meal was full)
        int         before, after;   // the 20 ms steps from the jump (the step of the walk is the first) by which the montage is still there, and the steps more by which it is cleared
    };
    const Eater eaters[] = {
        {"a meal stood up from before its bar is full", gc::kAnglersSmokedDelight, gc::kAnglersSmokedDelightAbility, kEatingEnd, false, 52, 1},   // 1.04 s, then 1.06 s
        {"a meal stood up from after its apply hit", gc::kAnglersSmokedDelight, gc::kAnglersSmokedDelightAbility, kEatingEnd, true, 52, 1},
        {"rations", gc::kRations, gc::kRationsAbility, kRationEnd, false, 58, 1},   // 1.16 s, then 1.18 s
    };
    for (size_t k = 0; k < sizeof(eaters) / sizeof(eaters[0]); ++k) {
        const Eater& e = eaters[k];
        const uint32_t guid = static_cast<uint32_t>(1030 + k);
        const int32_t  id = static_cast<int32_t>(1040 + k);
        Harness h(bag_of(e.item, guid), true, true, path);
        if (!use_item(h, 4, guid, e.ability, id)) return;
        if (e.hit) {
            run_steps(h, 347);   // 6.94 s
            ask_hit(h, e.ability, gc::kAnglersSmokedDelightHit);
            run_steps(h, 50);    // 1 s past the hit: seated
        } else {
            run_steps(h, 150);   // 3 s: Resting is on, the bar is not full
        }
        const int64_t server = h.s->pipeline() ? h.s->pipeline()->instance_of(id) : -1;
        const ActiveItem seated = active_item(h, server);
        std::printf("  -- %s\n", e.what);
        CHECK(server >= 0 && seated.present && seated.montage != 0 && effect_of(h, gc::kRestingEffect) != nullptr, "(the character is seated, with the eating montage on its item and Resting on)");
        CHECK(seated.predicted, "(the item of a seated eater is the client own ability: it says the client took the branch itself)");
        const size_t ends = h.s->ability_ends_sent();
        Walker w;
        const size_t moves = h.s->moves_acknowledged();
        walk(h, w);
        h.step();
        CHECK(h.s->moves_acknowledged() == moves + 1, "(the move was taken)");
        const ActiveItem jumped = active_item(h, server);
        CHECK(jumped.present && jumped.jump && jumped.position == e.end && jumped.counter == seated.counter + 1 && jumped.montage == seated.montage,
              "walking: the montage of the item jumps to its end section, AnimationCounter one on");
        CHECK(!jumped.predicted, "... and the item of the jump does not say the client took the branch itself: the server decided it");
        run_steps(h, 2);   // 0.06 s after the jump
        CHECK(effect_of(h, gc::kRestingEffect) != nullptr, "Resting is still on 0.06 s after the jump");
        run_steps(h, 1);   // 0.08 s
        CHECK(effect_of(h, gc::kRestingEffect) == nullptr && h.s->pipeline() && h.s->pipeline()->instance_of(id) == -1, "Resting is gone 0.067 s after the jump, and the use is over");
        run_steps(h, e.before - 4);
        const ActiveItem playing = active_item(h, server);
        CHECK(playing.present && playing.montage == seated.montage && playing.counter == jumped.counter && playing.jump && !playing.predicted,
              "the montage plays out its jump: until it ends the item still carries it, unchanged");
        run_steps(h, e.after);
        const ActiveItem cleared = active_item(h, server);
        CHECK(cleared.present && cleared.montage == 0, "when the montage reaches its end the item loses it, and stays in the list");
        CHECK(!cleared.predicted && cleared.counter == jumped.counter + 1, "... the item does not say the client took the branch itself, and its AnimationCounter is one on");
        CHECK(cleared.jump && cleared.position == e.end, "... its jump position is left as it was");
        run_steps(h, 150);   // 3 s more
        const ActiveItem later = active_item(h, server);
        CHECK(later.present && later.montage == 0 && later.counter == cleared.counter && later.position == e.end && later.jump && !later.predicted,
              "the cleared item stays as it is: one clear, and no deletion");
        CHECK(h.s->ability_ends_sent() == ends && !h.s->failed() && h.mirror.unreadable == 0, "no ClientEndAbility was sent, and the session took every update");
    }
    // Another ability used 0.5 s after the stand-up takes the item on the pawn: the clear of the item it replaced comes to nothing and never touches the new one.
    {
        Harness h(bag_of(gc::kAnglersSmokedDelight, 1050, gc::kBread, 1051), true, true, path);
        if (!use_item(h, 4, 1050, gc::kAnglersSmokedDelightAbility, 1052)) return;
        run_steps(h, 150);   // 3 s
        const int64_t first = h.s->pipeline() ? h.s->pipeline()->instance_of(1052) : -1;
        const ActiveItem seated = active_item(h, first);
        Walker w;
        walk(h, w);
        h.step();
        const ActiveItem jumped = active_item(h, first);
        CHECK(first >= 0 && jumped.jump && jumped.position == kEatingEnd && jumped.counter == seated.counter + 1 && !jumped.predicted,
              "(the character stood up from the first meal: its item jumped, not saying the client took the branch)");
        run_steps(h, 25);   // 0.5 s after the jump
        CHECK(active_item(h, first).present && active_item(h, first).montage == seated.montage, "(0.5 s after the jump the first item still carries its montage)");
        if (!use_item(h, 5, 1051, gc::kBreadAbility, 1053)) return;
        int64_t second = -1;
        for (int i = 0; i < 20 && second < 0; ++i) {
            h.step();
            const int64_t id = h.s->pipeline() ? h.s->pipeline()->instance_of(1053) : -1;
            if (id >= 0 && active_item(h, id).present) second = id;
        }
        CHECK(second >= 0 && second != first, "(the item of the second meal comes)");
        CHECK(!active_item(h, first).present, "the first item went with the coming of the second one");
        const ActiveItem fresh = active_item(h, second);
        run_steps(h, 30);   // 0.6 s on: the montage of the first item would have ended by now (1.044 s after its jump)
        const ActiveItem now = active_item(h, second);
        CHECK(now.present && now.montage == fresh.montage && now.montage != 0 && now.counter == fresh.counter && now.predicted == fresh.predicted && !now.jump,
              "the second item is not overwritten: it keeps its montage, its counter and its branch flag");
        CHECK(!active_item(h, first).present && !h.s->failed() && h.mirror.unreadable == 0, "... and the late clear of the first item changes nothing: it is not brought back, and every update was taken");
        walk(h, w);
        h.step();
        const ActiveItem again = active_item(h, second);
        CHECK(again.jump && again.position == kEatingEnd && again.counter == fresh.counter + 1 && !again.predicted,
              "(the late clear of the first item took no AnimationCounter: the jump of the second meal is one on from its own item)");
    }
}
AOC_REGISTER_TEST(test_consumables_stand_up_item);

void test_consumables_health_potion_at_full_health() {
    const char* path = professions_db_path();
    if (!path) {
        std::printf("Consumables: a health potion at full Health: skipped (no real content.db)\n");
        return;
    }
    std::printf("Consumables: a health potion is refused at full Health and still heals when Health is missing\n");
    {
        svc::Character rec = cyb_record();
        rec.level = 25;
        std::vector<svc::InventoryItem> items;
        for (const auto& it : rec.items)
            if (it.container == Worn) items.push_back(it);
        items.push_back(make_item(Bag, 4, gc::kHealthPotionT3, 960, 2));
        rec.items = items;
        Harness h(rec, true, true, path);
        const sg::CharacterStats* cs = h.s->character_stats();
        const bs::PlayerSetup& setup = h.s->bootstrap().setup();
        CHECK(h.mirror.unreadable == 0 && !h.s->failed() && cs, "(joined)");
        if (!cs) return;
        const float max_health = cs->find(gc::kMaxHealthStat)->value;
        CHECK(h.s->set_current_value(gc::kHealthStat, max_health), "(Health is at its maximum)");
        const svc::InventoryItem* item = h.s->inventory().at(Bag, 4);
        if (!item) return;
        const size_t applied = h.s->uses_applied();
        const size_t ends = h.s->ability_ends_sent();
        const size_t effects = h.s->pipeline() ? h.s->pipeline()->effect_count() : 0;
        h.client_sends({h.client.actor(h.pawn_ch, item_used(3, 4, guid_n(960)), true),
                        h.client.actor(h.pawn_ch, trigger_with_item(setup.guids.ability, gc::kHealthPotionT3Ability, 961, 4, items::to_instance(*item), 3), true),
                        h.client.actor(h.pawn_ch, hit_request_for(setup, gc::kHealthPotionT3Ability, kSelfTargetingGuid, gc::kHealthPotionT3Hit), true)});
        for (int i = 0; i < 20; ++i) h.step();
        CHECK(h.s->inventory().at(Bag, 4) && h.s->inventory().at(Bag, 4)->quantity == 2, "the stack keeps both potions");
        CHECK(h.s->uses_applied() == applied && h.s->item_reservations() == 0, "nothing is taken or reserved");
        CHECK(h.s->pipeline() && h.s->pipeline()->charges_left(gc::kHealthPotionT3Ability) == -1, "no cooldown starts");
        CHECK(cs->find(gc::kHealthStat)->value == max_health, "Health is unchanged");
        CHECK(h.s->ability_ends_sent() == ends + 1, "the client gets the refusal");
        CHECK(h.s->pipeline() && h.s->pipeline()->effect_count() == effects, "no effect is applied");
        const svc::InventoryItem* again = h.s->inventory().at(Bag, 4);
        if (again) {
            h.client_sends({h.client.actor(h.pawn_ch, item_used(3, 4, guid_n(960)), true),
                            h.client.actor(h.pawn_ch, trigger_with_item(setup.guids.ability, gc::kHealthPotionT3Ability, 962, 4, items::to_instance(*again), 3), true)});
            for (int i = 0; i < 20; ++i) h.step();
            CHECK(h.s->inventory().at(Bag, 4) && h.s->inventory().at(Bag, 4)->quantity == 2 && h.s->pipeline() &&
                      h.s->pipeline()->charges_left(gc::kHealthPotionT3Ability) == -1 && h.s->uses_applied() == applied,
                  "a repeated use at full Health is refused as well, once");
            CHECK(h.s->ability_ends_sent() == ends + 2, "the repeated use is answered with the same refusal");
        }
        // A new login starts from the saved bag: the refused use left the stack there and no cooldown behind.
        svc::Character saved = rec;
        saved.items = h.s->inventory().items();
        Harness g(saved, true, true, path);
        const svc::InventoryItem* held = g.s->inventory().at(Bag, 4);
        CHECK(held && held->quantity == 2, "a new login still holds both potions");
        CHECK(g.s->pipeline() && g.s->pipeline()->charges_left(gc::kHealthPotionT3Ability) == -1, "... and no cooldown");
    }
    const float healed = potion_heal(path, 1);
    std::printf("    the Common potion heals %.1f Health from 1\n", healed);
    CHECK(healed == 1052.0f, "the Common Health Potion still heals its 1,052 when Health is missing");
}
AOC_REGISTER_TEST(test_consumables_health_potion_at_full_health);

void test_consumables_one_application_per_use() {
    const char* path = professions_db_path();
    if (!path) {
        std::printf("Consumables: one application of an effect per use: skipped (no real content.db)\n");
        return;
    }
    std::printf("Consumables: a salve whose hit comes with the trigger, late, twice or never applies its effect once\n");
    struct SalveRun {
        float delta = 0;
        size_t ticks = 0;
        int stacks = 0;
    };
    const auto run = [&](const std::vector<int>& hit_steps, SalveRun& out) {
        svc::Character rec = cyb_record();
        rec.level = 25;
        std::vector<svc::InventoryItem> items;
        for (const auto& it : rec.items)
            if (it.container == Worn) items.push_back(it);
        items.push_back(make_item(Bag, 4, gc::kHealthSalve, 970, 1));
        rec.items = items;
        Harness h(rec, true, true, path);
        const sg::CharacterStats* cs = h.s->character_stats();
        const bs::PlayerSetup& setup = h.s->bootstrap().setup();
        if (!cs || !h.s->set_current_value(gc::kHealthStat, 1.0f)) return false;
        const svc::InventoryItem* item = h.s->inventory().at(Bag, 4);
        if (!item) return false;
        const float before = cs->find(gc::kHealthStat)->value;
        const size_t log_from = g_log.str().size();
        std::vector<fake::Bunch> start;
        start.push_back(h.client.actor(h.pawn_ch, item_used(3, 4, guid_n(970)), true));
        start.push_back(h.client.actor(h.pawn_ch, trigger_with_item(setup.guids.ability, gc::kHealthSalveAbility, 971, 4, items::to_instance(*item), 3), true));
        if (std::find(hit_steps.begin(), hit_steps.end(), 0) != hit_steps.end())
            start.push_back(h.client.actor(h.pawn_ch, hit_request_for(setup, gc::kHealthSalveAbility, kSelfTargetingGuid, gc::kHealthSalveHit), true));
        h.client_sends(std::move(start));
        for (int i = 1; i <= 20; ++i) {
            if (std::find(hit_steps.begin(), hit_steps.end(), i) != hit_steps.end())
                h.client_sends({h.client.actor(h.pawn_ch, hit_request_for(setup, gc::kHealthSalveAbility, kSelfTargetingGuid, gc::kHealthSalveHit), true)});
            h.step();   // 20 steps: 0.4 s, the first tick's window
        }
        out.delta = cs->find(gc::kHealthStat)->value - before;
        const std::string first = g_log.str().substr(log_from);
        out.stacks = h.s->pipeline() ? h.s->pipeline()->stacks_of(gc::kHealthSalveEffect) : -1;
        for (int i = 0; i < 1000; ++i) h.step();   // 20 s more: the effect's whole life
        out.ticks = count_text(g_log.str().substr(log_from), "the effect Consumable_Salve_Health_T2 ticks");
        CHECK(out.ticks > 0 && count_text(first, "the effect Consumable_Salve_Health_T2 ticks") == 1, "the first tick lands once");
        return true;
    };
    SalveRun same, late, none, control;
    CHECK(run({0}, same), "the salve whose hit comes in the trigger's datagram runs");
    CHECK(run({7, 15}, late), "the salve whose hit comes 150 ms later, twice, runs");
    CHECK(run({}, none), "the salve whose hit never comes runs");
    {
        // A new login during the use (its montage still runs): the taken salve is gone and its effect is not applied again.
        svc::Character rec = cyb_record();
        rec.level = 25;
        std::vector<svc::InventoryItem> items;
        for (const auto& it : rec.items)
            if (it.container == Worn) items.push_back(it);
        items.push_back(make_item(Bag, 4, gc::kHealthSalve, 973, 1));
        rec.items = items;
        Harness h(rec, true, true, path);
        const bs::PlayerSetup& setup = h.s->bootstrap().setup();
        const svc::InventoryItem* item = h.s->inventory().at(Bag, 4);
        if (!item) return;
        h.client_sends({h.client.actor(h.pawn_ch, item_used(3, 4, guid_n(973)), true),
                        h.client.actor(h.pawn_ch, trigger_with_item(setup.guids.ability, gc::kHealthSalveAbility, 974, 4, items::to_instance(*item), 3), true),
                        h.client.actor(h.pawn_ch, hit_request_for(setup, gc::kHealthSalveAbility, kSelfTargetingGuid, gc::kHealthSalveHit), true)});
        for (int i = 0; i < 20; ++i) h.step();
        CHECK(effect_of(h, gc::kHealthSalveEffect) != nullptr && h.s->pipeline() && h.s->pipeline()->instance_of(974) >= 0,
              "(the salve's effect is on while its montage runs)");
        svc::Character saved = rec;
        saved.items = h.s->inventory().items();
        Harness g(saved, true, true, path);
        CHECK(g.s->inventory().at(Bag, 4) == nullptr, "a new login during the use does not bring the salve back");
        CHECK(effect_of(g, gc::kHealthSalveEffect) == nullptr, "... and does not reapply its effect");
    }
    float salve_tick = 0;
    {
        svc::Character rec = cyb_record();
        rec.level = 25;
        std::vector<svc::InventoryItem> items;
        for (const auto& it : rec.items)
            if (it.container == Worn) items.push_back(it);
        items.push_back(make_item(Bag, 4, gc::kHealthSalve, 972, 1));
        rec.items = items;
        Harness h(rec, true, true, path);
        const sg::CharacterStats* cs = h.s->character_stats();
        if (!cs || !h.s->set_current_value(gc::kHealthStat, 1.0f)) return;
        for (const stats::StatMod& m : cs->item_stats(gc::kHealthSalve, 1))
            if (m.stat == gc::kHealthStat) salve_tick = m.value;
        const float before = cs->find(gc::kHealthStat)->value;
        for (int i = 0; i < 20; ++i) h.step();
        control.delta = cs->find(gc::kHealthStat)->value - before;   // the regeneration of the same 0.4 s
    }
    std::printf("    the salve's first tick: same datagram %.1f, late %.1f, never %.1f (regeneration %.1f)\n", same.delta, late.delta, none.delta, control.delta);
    CHECK(salve_tick == 162.0f, "the salve's item carries 162 Health a tick at Common");
    CHECK(std::fabs((same.delta - control.delta) - salve_tick) <= 1.0f, "with the hit in the trigger's datagram the first tick heals the item's 162 once");
    CHECK(std::fabs((late.delta - control.delta) - salve_tick) <= 1.0f, "with the hit 150 ms later the first tick still heals 162 once");
    CHECK(std::fabs((none.delta - control.delta) - salve_tick) <= 1.0f, "with no hit at all the effect still applies once");
    CHECK(same.stacks == 1 && late.stacks == 1 && none.stacks == 1, "one instance of the effect is on");
    CHECK(same.ticks == none.ticks && late.ticks == none.ticks, "over the 20 s the same number of ticks as one application");
}
AOC_REGISTER_TEST(test_consumables_one_application_per_use);

/// The ServerTriggerAbility of an item's ability with the AnimationIndex the client chose: 0 is the montage row that starts at PreSit (the client takes it while the
/// character already has Resting), 1 the one that starts at the montage's first section.
static BunchWriter trigger_with_item_anim(const emit::FIntrepidNetworkGUID& ability, int64_t guid, int32_t id, int slot, const repl::ItemInstance& item, uint8_t location, int32_t anim) {
    const schema::ClassSchema& cls = class_named("UAoCAbilityComponent");
    const schema::RpcSchema* rpc = cls.rpc("ServerTriggerAbility");
    BunchWriter out;
    repl::RpcParams p;
    bool ok = rpc && repl::set_rpc_param(*rpc, p, "AbilityGuid", repl::Value::i64(guid));
    if (ok && id != 0) ok = repl::set_rpc_param(*rpc, p, "ClientInstanceId", repl::Value::i64(id));
    if (ok && anim != 0) ok = repl::set_rpc_param(*rpc, p, "AnimationIndex", repl::Value::i64(anim));
    ok = ok && repl::set_rpc_param(*rpc, p, "InputVector", repl::Value(repl::Vec2d{1, 0}));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.Slot", repl::Value::i64(slot));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.ItemInstance", repl::Value(item));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.Location", repl::Value::u64(location));
    BunchWriter field;
    bool failed = false;
    if (!ok || repl::write_rpc_field(cls, *rpc, p, field, client_options(), &failed) == 0 || failed) return out;
    repl::BlockHeader header;
    header.is_actor = false;
    header.has_rep_layout = false;
    header.object = ability;
    repl::write_content_block(out, header, field, /*server_form=*/false);
    return out;
}

/// The client uses the item in `slot` with the montage row `anim` (see trigger_with_item_anim).
static bool use_item_anim(Harness& h, int slot, uint32_t guid, int64_t ability, int32_t id, int32_t anim) {
    const svc::InventoryItem* item = h.s->inventory().at(Bag, slot);
    if (!item) return false;
    const bs::PlayerSetup& setup = h.s->bootstrap().setup();
    h.client_sends({h.client.actor(h.pawn_ch, item_used(3, slot, guid_n(guid)), true),
                    h.client.actor(h.pawn_ch, trigger_with_item_anim(setup.guids.ability, ability, id, slot, items::to_instance(*item), 3, anim), true)});
    return true;
}

/// The three abilities on the Regen montage: the rations, and the food and the drink of the same kind.  Each has its own 20 s effect (a tick every second) and Resting.
struct RegenKind {
    const char* name;
    int64_t     item, ability, effect, tick_hit;
};
constexpr RegenKind kRegenKinds[] = {
    {"the rations", gc::kRations, gc::kRationsAbility, gc::kRationsEffect, 6064632587347886422ll},
    {"the Regen food", 6064632586725298084ll, 6064632587313283378ll, 6064632587313283381ll, 0},
    {"the Regen drink", 6064632586725298090ll, 6064632587122704500ll, 6064632587122704502ll, 0},
};

void test_consumables_rations_give_their_effect() {
    const char* path = professions_db_path();
    if (!path) {
        std::printf("Consumables: rations give their effect: skipped (no real content.db)\n");
        return;
    }
    std::printf("Consumables: rations, and the food and drink of their kind, give their effect and Resting when their montage reaches them, with no hit request\n");
    // The whole life of a use with no hit request and no end from the client: nothing for 2 s, then the effect and Resting together, then the character stays seated.
    {
        Harness h(bag_of(gc::kRations, 1100), true, true, path);
        CHECK(h.mirror.unreadable == 0 && !h.s->failed(), "(joined)");
        if (!use_item_anim(h, 4, 1100, gc::kRationsAbility, 1101, 1)) return;
        run_steps(h, 100);   // 2.0 s
        CHECK(effect_of(h, gc::kRationsEffect) == nullptr && effect_of(h, gc::kRestingEffect) == nullptr,
              "nothing is given in the first 2 s: the montage that starts at its first section reaches its effect notifies then");
        const int64_t server = h.s->pipeline() ? h.s->pipeline()->instance_of(1101) : -1;
        CHECK(server >= 0 && active_item(h, server).montage != 0, "(the use runs and its item carries the eating montage)");
        const size_t ends = h.s->ability_ends_sent();
        const size_t added = effects_added(h);
        run_steps(h, 2);   // 2.04 s
        const Mirror::EffectSeen* feast = effect_of(h, gc::kRationsEffect);
        CHECK(feast && feast->duration == 20.0f, "the rations' own 20 s effect comes at 2 s, though the client asked for no hit");
        CHECK(effect_of(h, gc::kRestingEffect) != nullptr, "... and Resting with it");
        CHECK(effects_added(h) == added + 2, "... the two together, once each");
        run_steps(h, 250);   // 5 s later
        CHECK(h.s->pipeline() && h.s->pipeline()->instance_of(1101) == server && active_item(h, server).montage != 0 && !active_item(h, server).jump,
              "5 s on the use is still there, its item with the eating montage");
        CHECK(effect_of(h, gc::kRationsEffect) == feast && effect_of(h, gc::kRestingEffect) != nullptr && effects_added(h) == added + 2,
              "the effect and Resting are still the ones given, applied once");
        run_steps(h, 770);   // 15.4 s more: 22.5 s after the use, past the effect's 20 s
        CHECK(effect_of(h, gc::kRationsEffect) == nullptr, "the 20 s effect is over after its 20 s");
        CHECK(h.s->pipeline() && h.s->pipeline()->instance_of(1101) == server && effect_of(h, gc::kRestingEffect) != nullptr && active_item(h, server).montage != 0,
              "... and the character is still seated, with Resting and the eating montage");
        CHECK(h.s->ability_ends_sent() == ends && h.s->inventory().at(Bag, 4) == nullptr && h.s->uses_applied() == 1, "no ClientEndAbility was sent, and the rations were taken once");
        const ActiveItem seated = active_item(h, server);
        Walker w;
        walk(h, w);
        run_steps(h, 6);
        const ActiveItem jumped = active_item(h, server);
        CHECK(jumped.jump && jumped.position == kRationEnd && jumped.counter == seated.counter + 1, "walking: the rations' montage jumps to its end section");
        CHECK(effect_of(h, gc::kRestingEffect) == nullptr && h.s->pipeline() && h.s->pipeline()->instance_of(1101) == -1, "... Resting is gone and the use is over");
    }
    // The food and the drink of the same kind, and a character that already rests when it starts: the montage row that starts at PreSit reaches its notifies at 1 s.
    for (int i = 0; i < 3; ++i) {
        const RegenKind& k = kRegenKinds[i];
        Harness h(bag_of(k.item, static_cast<uint32_t>(1110 + i)), true, true, path);
        if (!use_item_anim(h, 4, static_cast<uint32_t>(1110 + i), k.ability, 1120 + i, 0)) {
            CHECK(false, k.name);
            return;
        }
        run_steps(h, 50);   // 1.0 s
        CHECK(effect_of(h, k.effect) == nullptr && effect_of(h, gc::kRestingEffect) == nullptr, "a use that starts at PreSit gives nothing in its first second");
        run_steps(h, 2);
        const Mirror::EffectSeen* effect = effect_of(h, k.effect);
        CHECK(effect && effect->duration == 20.0f && effect_of(h, gc::kRestingEffect) != nullptr, "... and its own 20 s effect and Resting at 1 s");
        const int64_t server = h.s->pipeline() ? h.s->pipeline()->instance_of(1120 + i) : -1;
        CHECK(server >= 0, "... the use goes on");
        Walker w;
        walk(h, w);
        run_steps(h, 6);
        CHECK(active_item(h, server).jump && active_item(h, server).position == kRationEnd, "walking: the montage jumps to its end section");
        CHECK(effect_of(h, gc::kRestingEffect) == nullptr && effect_of(h, k.effect) == effect && h.s->pipeline() && h.s->pipeline()->instance_of(1120 + i) == -1,
              "... Resting goes, the use is over and the 20 s effect stays");
    }
    // Standing up before the effect's time: nothing is given, and the item stays spent.
    {
        Harness h(bag_of(gc::kRations, 1130), true, true, path);
        if (!use_item_anim(h, 4, 1130, gc::kRationsAbility, 1131, 1)) return;
        run_steps(h, 50);   // 1 s
        const int64_t server = h.s->pipeline() ? h.s->pipeline()->instance_of(1131) : -1;
        const size_t added = effects_added(h);
        CHECK(server >= 0 && effect_of(h, gc::kRationsEffect) == nullptr && effect_of(h, gc::kRestingEffect) == nullptr, "(the character is eating the rations; nothing is given yet)");
        Walker w;
        walk(h, w);
        run_steps(h, 6);
        CHECK(active_item(h, server).jump && active_item(h, server).position == kRationEnd && h.s->pipeline() && h.s->pipeline()->instance_of(1131) == -1,
              "a walk before the effect's time: the montage jumps to its end section and the use is over");
        run_steps(h, 150);   // 4.2 s after the use
        CHECK(effect_of(h, gc::kRationsEffect) == nullptr && effect_of(h, gc::kRestingEffect) == nullptr && effects_added(h) == added,
              "... and the rations give neither their effect nor Resting later");
        CHECK(h.s->inventory().at(Bag, 4) == nullptr && h.s->uses_applied() == 1, "... the rations stay spent");
    }
    // A hit request or the client's end after the effect applies nothing twice.
    {
        Harness h(bag_of(gc::kRations, 1140), true, true, path);
        if (!use_item_anim(h, 4, 1140, gc::kRationsAbility, 1141, 1)) return;
        run_steps(h, 120);   // 2.4 s
        const Mirror::EffectSeen* feast = effect_of(h, gc::kRationsEffect);
        CHECK(feast != nullptr && effect_of(h, gc::kRestingEffect) != nullptr, "(the effect and Resting are on)");
        const size_t added = effects_added(h);
        const int64_t server = h.s->pipeline() ? h.s->pipeline()->instance_of(1141) : -1;
        ask_hit(h, gc::kRationsAbility, kRegenKinds[0].tick_hit);
        run_steps(h, 15);
        CHECK(effect_count_of(h, gc::kRationsEffect) == 1 && effects_added(h) == added && effect_of(h, gc::kRationsEffect) == feast, "a hit request after the effect gives it no second time");
        const size_t ends = h.s->ability_ends_sent();
        if (server >= 0) {
            h.client_sends({h.client.actor(h.pawn_ch, end_ability(h.s->bootstrap().setup(), server), true)});
            run_steps(h, 15);
        }
        CHECK(h.s->pipeline() && h.s->pipeline()->instance_of(1141) == -1 && effect_of(h, gc::kRestingEffect) == nullptr && h.s->ability_ends_sent() == ends + 1,
              "the client's end ends the use, takes Resting off and is answered with ClientEndAbility");
        CHECK(effect_count_of(h, gc::kRationsEffect) == 1 && effects_added(h) == added && effect_of(h, gc::kRationsEffect) == feast, "... and gives the 20 s effect no second time");
    }
    // A new login while seated: nothing comes back.
    {
        const svc::Character rec = bag_of(gc::kRations, 1150);
        Harness h(rec, true, true, path);
        if (!use_item_anim(h, 4, 1150, gc::kRationsAbility, 1151, 1)) return;
        run_steps(h, 150);   // 3 s
        CHECK(h.s->pipeline() && h.s->pipeline()->instance_of(1151) >= 0 && effect_of(h, gc::kRationsEffect) != nullptr && effect_of(h, gc::kRestingEffect) != nullptr,
              "(the character is seated with the effect on when they log out)");
        svc::Character saved = rec;
        saved.items = h.s->inventory().items();
        Harness g(saved, true, true, path);
        CHECK(g.s->inventory().at(Bag, 4) == nullptr, "a new login does not bring the eaten rations back");
        CHECK(effect_of(g, gc::kRationsEffect) == nullptr && effect_of(g, gc::kRestingEffect) == nullptr && g.s->pipeline() && g.s->pipeline()->instance_of(1151) == -1,
              "... nor their effect, nor Resting, nor a use that runs");
        run_steps(g, 150);
        CHECK(effect_of(g, gc::kRationsEffect) == nullptr && effect_of(g, gc::kRestingEffect) == nullptr && !g.s->failed(), "... not even later");
    }
}
AOC_REGISTER_TEST(test_consumables_rations_give_their_effect);

void test_consumables_meal_resting_at_its_notify() {
    const char* path = professions_db_path();
    if (!path) {
        std::printf("Consumables: a meal's Resting at its notify: skipped (no real content.db)\n");
        return;
    }
    std::printf("Consumables: a meal's and a drink's Resting comes when the Food montage reaches its notify (2 s after the use, 1 s from PreSit), and no Nourishment comes\n");
    // The montage that starts at its first section reaches the notify after 2 s, the one that starts at PreSit (the character already rests) after 1 s.
    for (int32_t anim = 1; anim >= 0; --anim) {
        const bool first = anim == 1;
        Harness h(bag_of(gc::kAnglersSmokedDelight, static_cast<uint32_t>(1200 + anim)), true, true, path);
        if (!use_item_anim(h, 4, static_cast<uint32_t>(1200 + anim), gc::kAnglersSmokedDelightAbility, 1210 + anim, anim)) return;
        run_steps(h, first ? 100 : 50);   // 2.0 s, or 1.0 s
        CHECK(effect_of(h, gc::kRestingEffect) == nullptr,
              first ? "a meal that starts at the first section has no Resting in its first 2 s: the montage has not reached its notify"
                    : "a meal that starts at PreSit has no Resting in its first second: the montage has not reached its notify");
        const int64_t server = h.s->pipeline() ? h.s->pipeline()->instance_of(1210 + anim) : -1;
        CHECK(server >= 0 && active_item(h, server).montage != 0, "(the use runs and its item carries the eating montage)");
        run_steps(h, 2);
        CHECK(effect_count_of(h, gc::kRestingEffect) == 1, first ? "... Resting comes at 2 s" : "... Resting comes at 1 s");
        CHECK(effect_of(h, gc::kAnglersSmokedDelightEffect) == nullptr && effect_of(h, gc::kFreshSaladNourishment) == nullptr, "... and nothing else: the buff waits for the hit");
        if (!first) continue;
        run_steps(h, 245);   // 6.94 s
        const size_t added = effects_added(h);
        ask_hit(h, gc::kAnglersSmokedDelightAbility, gc::kAnglersSmokedDelightHit);
        run_steps(h, 6);
        CHECK(effect_of(h, gc::kAnglersSmokedDelightEffect) != nullptr, "the apply hit gives the buff");
        run_steps(h, 50);
        CHECK(effect_of(h, gc::kFreshSaladNourishment) == nullptr && effects_added(h) == added + 1, "... and nothing else, not even later: a meal gives no Nourishment");
    }
    // A drink has the same notify.
    {
        Harness h(bag_of(gc::kAlchemicalAmber, 1220), true, true, path);
        if (!use_item_anim(h, 4, 1220, gc::kAlchemicalAmberAbility, 1221, 1)) return;
        run_steps(h, 100);
        CHECK(effect_of(h, gc::kRestingEffect) == nullptr, "a drink that starts at the first section has no Resting in its first 2 s");
        run_steps(h, 2);
        CHECK(effect_count_of(h, gc::kRestingEffect) == 1, "... Resting comes at 2 s");
    }
    // Standing up before the notify: no Resting is ever put on, the montage jumps to its end, the apply hit that comes later gives nothing, and the meal stays spent.
    {
        Harness h(bag_of(gc::kAnglersSmokedDelight, 1230), true, true, path);
        if (!use_item_anim(h, 4, 1230, gc::kAnglersSmokedDelightAbility, 1231, 1)) return;
        run_steps(h, 50);   // 1 s
        const int64_t server = h.s->pipeline() ? h.s->pipeline()->instance_of(1231) : -1;
        CHECK(server >= 0 && effect_of(h, gc::kRestingEffect) == nullptr, "(the character is eating and has no Resting yet)");
        const size_t added = effects_added(h);
        Walker w;
        walk(h, w);
        run_steps(h, 6);
        CHECK(active_item(h, server).jump && active_item(h, server).position == kEatingEnd && h.s->pipeline() && h.s->pipeline()->instance_of(1231) == -1,
              "a walk before the notify: the montage jumps to its end section and the use is over");
        run_steps(h, 150);   // 4.2 s after the use
        CHECK(effect_of(h, gc::kRestingEffect) == nullptr && effects_added(h) == added, "... and Resting never comes");
        run_steps(h, 140);   // 7 s after the use
        ask_hit(h, gc::kAnglersSmokedDelightAbility, gc::kAnglersSmokedDelightHit);
        run_steps(h, 10);
        CHECK(effect_of(h, gc::kAnglersSmokedDelightEffect) == nullptr && effects_added(h) == added, "the apply hit that comes later gives nothing");
        CHECK(h.s->inventory().at(Bag, 4) == nullptr && h.s->uses_applied() == 1, "the meal stays spent");
    }
}
AOC_REGISTER_TEST(test_consumables_meal_resting_at_its_notify);

// ---- the montage clear of a meal the character stood up from changes that meal's own item only ------------------------------------------------------------------------

/// The 20 ms steps from the jump's (the step that took the walk is the first) by which the montage of a meal the character stood up from is still on its item, and by which
/// it is cleared: it reaches its end 1.044 s after the jump.
constexpr int kMontageStillThere = 52;
constexpr int kMontageCleared = 53;

/// A meal eaten for 3 s and then a walk: the montage of its item jumps to its end section.  `steps` counts the 20 ms steps from the jump's; `go` runs the harness to a step.
struct StoodUpMeal {
    int64_t    server = -1;
    int        steps = 0;
    ActiveItem seated, jumped;
};

/// The character stands up with a walk, or with the ability `by_ability` (its client instance `by_id`) when one is given: a Sprint or a Dodge stands a seated eater up as it starts.
static StoodUpMeal stand_up_from_meal(Harness& h, Walker& w, uint32_t guid, int32_t id, int64_t by_ability = 0, int32_t by_id = 0) {
    StoodUpMeal m;
    if (!use_item(h, 4, guid, gc::kAnglersSmokedDelightAbility, id)) return m;
    run_steps(h, 150);   // 3 s: Resting is on, the bar is not full
    m.server = h.s->pipeline() ? h.s->pipeline()->instance_of(id) : -1;
    m.seated = active_item(h, m.server);
    if (by_ability != 0) h.client_sends({h.client.actor(h.pawn_ch, ability_trigger(h.s->bootstrap().setup().guids.ability, by_ability, by_id, 0), true)});
    else walk(h, w);
    h.step();
    m.steps = 1;
    m.jumped = active_item(h, m.server);
    return m;
}

static void go(Harness& h, StoodUpMeal& m, int steps) {
    if (steps > m.steps) run_steps(h, steps - m.steps);
    m.steps = std::max(m.steps, steps);
}

/// The montage a meal's item loses when it reaches its end is that meal's: a Rest, a Sprint or a Dodge the character starts meanwhile has an item of its own, and the clear
/// neither clears nor overwrites it.  The meal's own item still loses its montage, once, and stays in the list.
void test_consumables_montage_clear_leaves_other_items() {
    const char* path = professions_db_path();
    if (!path) {
        std::printf("Consumables: the montage clear of a meal stood up from, beside a Rest, a Sprint and a Dodge: skipped (no real content.db)\n");
        return;
    }
    std::printf("Consumables: the montage clear of a meal the character stood up from changes that meal's item only: a Rest, a Sprint or a Dodge started meanwhile keeps its own\n");
    struct Other {
        const char* what;
        int64_t     ability;   // 0: none
        int         at;        // the step, counted from the jump's, that its trigger comes in
        bool        stands;    // its trigger is what stands the character up (the jump comes with it)
    };
    const Other others[] = {
        {"nothing else is started", 0, 0, false},
        {"a Rest started 0.3 s after the jump", content::kGuidRest, 15, false},
        {"a Sprint started 0.3 s after the jump", content::kGuidSprint, 15, false},
        {"a Dodge started 0.5 s after the jump (its item stays until its 0.65 s cooldown is over, past the montage's end)", content::kGuidDodge, 26, false},
        {"a Sprint that stands the character up: its item is on the pawn from the jump", content::kGuidSprint, 0, true},
    };
    for (size_t k = 0; k < sizeof(others) / sizeof(others[0]); ++k) {
        const Other& o = others[k];
        const uint32_t guid = static_cast<uint32_t>(1300 + k);
        const int32_t meal_id = static_cast<int32_t>(1310 + k), other_id = static_cast<int32_t>(1320 + k);
        Harness h(bag_of(gc::kAnglersSmokedDelight, guid), true, true, path);
        const bs::PlayerSetup& setup = h.s->bootstrap().setup();
        Walker w;
        StoodUpMeal m = o.stands ? stand_up_from_meal(h, w, guid, meal_id, o.ability, other_id) : stand_up_from_meal(h, w, guid, meal_id);
        std::printf("  -- %s\n", o.what);
        CHECK(m.server >= 0 && m.seated.present && m.seated.montage != 0, "(the character ate: the item of the meal carries the eating montage)");
        CHECK(m.jumped.present && m.jumped.jump && m.jumped.position == kEatingEnd && m.jumped.counter == m.seated.counter + 1 && !m.jumped.predicted && m.jumped.montage == m.seated.montage,
              "(the character stood up: the item's montage jumped to its end section and plays out)");
        std::optional<int64_t> other;
        ActiveItem own;
        if (o.ability != 0) {
            if (!o.stands) {
                go(h, m, o.at);
                h.client_sends({h.client.actor(h.pawn_ch, ability_trigger(setup.guids.ability, o.ability, other_id, 0), true)});
                h.step();
                ++m.steps;
            }
            other = h.s->abilities() ? h.s->abilities()->instance_of(other_id) : std::nullopt;
            own = active_item(h, other.value_or(-1));
            CHECK(other && *other != m.server && own.present, "(the ability was taken: its item is on the pawn, with an instance of its own)");
        }
        go(h, m, kMontageStillThere);
        const ActiveItem playing = active_item(h, m.server);
        CHECK(playing.present && playing.montage == m.seated.montage && playing.key == m.jumped.key, "(the montage of the meal's item still plays, and nothing has been sent about it)");
        go(h, m, kMontageCleared + 1);
        const ActiveItem cleared = active_item(h, m.server);
        CHECK(cleared.present && cleared.montage == 0 && !cleared.predicted && cleared.counter == m.jumped.counter + 1 && cleared.key == m.jumped.key + 1,
              "when the montage reaches its end the meal's own item loses it: one change, and the item stays in the list");
        if (o.ability != 0) {
            const ActiveItem after = active_item(h, other.value_or(-1));
            CHECK(after.present && after.key == own.key && after.counter == own.counter && after.montage == own.montage && after.predicted == own.predicted && after.jump == own.jump &&
                      after.position == own.position,
                  "the clear changed nothing of the item of the ability started meanwhile: not sent again, and its montage, counter and branch flag are its own");
        }
        run_steps(h, 100);
        CHECK(active_item(h, m.server).key == cleared.key, "(nothing clears the meal's item a second time)");
        CHECK(!h.s->failed() && h.mirror.unreadable == 0 && h.s->abilities() && h.s->pipeline(), "(the session took every update: the abilities are still on)");
    }
}
AOC_REGISTER_TEST(test_consumables_montage_clear_leaves_other_items);

/// A clear the montage's end would send comes to nothing once the use it belongs to has ended or been replaced, and an item that was cleared is not cleared again: the client's end
/// of a stood-up use is answered with ClientEndAbility and clears nothing a second time.
void test_consumables_montage_clear_after_the_end() {
    const char* path = professions_db_path();
    if (!path) {
        std::printf("Consumables: the montage clear after the use ended or was replaced: skipped (no real content.db)\n");
        return;
    }
    std::printf("Consumables: the montage clear of a meal stood up from never comes after its use ended or was replaced, and a cleared item is not cleared again\n");
    // The client ends the use after the character stood up and before the montage reaches its end: its end clears the item and is answered once, and the clear that was due
    // later does not come.  A second end of the same use (a double submit) is answered by nothing.
    {
        Harness h(bag_of(gc::kAnglersSmokedDelight, 1340), true, true, path);
        const bs::PlayerSetup& setup = h.s->bootstrap().setup();
        Walker w;
        StoodUpMeal m = stand_up_from_meal(h, w, 1340, 1341);
        CHECK(m.server >= 0 && m.jumped.jump && m.jumped.position == kEatingEnd, "(the character stood up from the meal)");
        go(h, m, 15);   // 0.3 s
        const size_t ends = h.s->ability_ends_sent();
        h.client_sends({h.client.actor(h.pawn_ch, end_ability(setup, m.server), true), h.client.actor(h.pawn_ch, end_ability(setup, m.server), true)});   // (sent twice at once)
        go(h, m, 30);   // 0.3 s more: past the 259 ms the end is answered in
        const ActiveItem ended = active_item(h, m.server);
        CHECK(ended.present && ended.montage == 0 && h.s->ability_ends_sent() == ends + 1, "the client's end clears the item's montage and is answered with ClientEndAbility, once");
        h.client_sends({h.client.actor(h.pawn_ch, end_ability(setup, m.server), true)});
        go(h, m, kMontageCleared + 30);   // past the moment the montage would have reached its end
        const ActiveItem late = active_item(h, m.server);
        CHECK(late.present && late.montage == 0 && late.key == ended.key && late.counter == ended.counter,
              "the clear that was due 1.044 s after the jump does not come: the item is as the client's end left it");
        CHECK(h.s->ability_ends_sent() == ends + 1 && !h.s->failed() && h.mirror.unreadable == 0, "(the second end of the same use was answered by nothing, and the session took every update)");
    }
    // The client ends the use after the montage reached its end: the item was cleared by then.  The end is answered, and the item is not cleared a second time.
    {
        Harness h(bag_of(gc::kAnglersSmokedDelight, 1342), true, true, path);
        const bs::PlayerSetup& setup = h.s->bootstrap().setup();
        Walker w;
        StoodUpMeal m = stand_up_from_meal(h, w, 1342, 1343);
        go(h, m, kMontageCleared + 5);
        const ActiveItem cleared = active_item(h, m.server);
        CHECK(m.server >= 0 && cleared.present && cleared.montage == 0 && cleared.key == m.jumped.key + 1, "(the montage reached its end and the item lost it)");
        const size_t ends = h.s->ability_ends_sent();
        h.client_sends({h.client.actor(h.pawn_ch, end_ability(setup, m.server), true)});
        go(h, m, kMontageCleared + 25);   // 0.4 s: past the 259 ms the end is answered in
        const ActiveItem after = active_item(h, m.server);
        CHECK(h.s->ability_ends_sent() == ends + 1, "the client's end is answered with ClientEndAbility");
        CHECK(after.present && after.key == cleared.key && after.counter == cleared.counter && after.montage == 0 && after.jump && after.position == kEatingEnd && !after.predicted,
              "... and the item is not sent a second time: it is as the clear left it");
        CHECK(!h.s->failed() && h.mirror.unreadable == 0, "(the session took every update)");
    }
    // Another meal takes the item on the pawn before the montage reaches its end: the first item is gone, the clear never comes back for it and never touches the new item, and a
    // late end of the first use is answered and changes no item.
    {
        Harness h(bag_of(gc::kAnglersSmokedDelight, 1344, gc::kBread, 1345), true, true, path);
        const bs::PlayerSetup& setup = h.s->bootstrap().setup();
        Walker w;
        StoodUpMeal m = stand_up_from_meal(h, w, 1344, 1346);
        go(h, m, 25);   // 0.5 s after the jump
        if (!use_item(h, 5, 1345, gc::kBreadAbility, 1347)) return;
        int64_t second = -1;
        for (int i = 0; i < 20 && second < 0; ++i) {
            h.step();
            ++m.steps;
            const int64_t id = h.s->pipeline() ? h.s->pipeline()->instance_of(1347) : -1;
            if (id >= 0 && active_item(h, id).present) second = id;
        }
        CHECK(m.server >= 0 && second >= 0 && second != m.server && !active_item(h, m.server).present, "(the second meal's item came and took the first one's place)");
        const ActiveItem fresh = active_item(h, second);
        go(h, m, kMontageCleared + 60);   // well past the first item's montage
        const ActiveItem now = active_item(h, second);
        CHECK(!active_item(h, m.server).present && now.present && now.key == fresh.key && now.counter == fresh.counter && now.montage == fresh.montage && now.predicted == fresh.predicted,
              "the first item is not brought back and the second is not sent again: it keeps its montage, its counter and its branch flag");
        const size_t ends = h.s->ability_ends_sent();
        h.client_sends({h.client.actor(h.pawn_ch, end_ability(setup, m.server), true)});
        go(h, m, kMontageCleared + 90);
        CHECK(h.s->ability_ends_sent() == ends + 1 && !active_item(h, m.server).present && active_item(h, second).key == fresh.key && !h.s->failed() && h.mirror.unreadable == 0,
              "a late end of the replaced use is answered with ClientEndAbility and changes no item");
    }
}
AOC_REGISTER_TEST(test_consumables_montage_clear_after_the_end);
