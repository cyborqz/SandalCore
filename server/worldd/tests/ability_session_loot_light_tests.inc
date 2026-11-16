// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/ability_session_loot_light_tests.inc: the loot pile's light through the world's own aftermath
//  (registered with AOC_REGISTER_TEST: test_worldd_ability_session.cpp runs it through aoc_tests::run; it is included from ability_session_registered.inc)
//
//  The aftermath of a world that started its fights asks the world's item catalog whether a stack is equipment (worn in a slot) when it makes a pile, and the pile's
//  loot component says so in CosmeticData.bHasEquipment, which the client draws the equipment's light for.  test_worldd_aftermath sets that question on its own
//  aftermath; here the world makes it, so a world that stopped wiring it would show no light for any pile.
// ============================================================================

namespace {

constexpr int64_t kLightWiringChest = 6064630214426886486ll;      // Gear_Armor_Light_Starter_Chest: worn in a slot
constexpr int64_t kLightWiringHelmet = 6064630736269279236ll;     // Gear_Armor_Light_Bard_Head: worn in the Helmet slot
constexpr int64_t kLightWiringDaffodil = 6064629184431849472ll;   // Resource_Raw_Plant_Daffodil: worn in none

/// CosmeticData.bHasEquipment of the actor's loot component: 1 sent set, 0 sent clear, -1 not sent (the client reads an absent bool as clear), -2 the actor has no loot component.
int equipment_light_of(const world::WorldActor* a) {
    if (!a) return -2;
    for (const repl::Subobject& s : a->actor.subobjects) {
        if (!s.obj.schema || std::string(s.obj.schema->name) != "ULootComponent") continue;
        const repl::Value* v = s.obj.state.get(*s.obj.schema, "CosmeticData.bHasEquipment");
        const bool* b = v ? v->get_if<bool>() : nullptr;
        return b ? (*b ? 1 : 0) : -1;
    }
    return -2;
}

/// CosmeticData.TopRarity of the actor's loot component (-1: none, -2: no loot component).
int top_rarity_of(const world::WorldActor* a) {
    if (!a) return -2;
    for (const repl::Subobject& s : a->actor.subobjects) {
        if (!s.obj.schema || std::string(s.obj.schema->name) != "ULootComponent") continue;
        const repl::Value* v = s.obj.state.get(*s.obj.schema, "CosmeticData.TopRarity");
        const uint64_t* u = v ? v->get_if<uint64_t>() : nullptr;
        return u ? static_cast<int>(*u) : -1;
    }
    return -2;
}

}  // namespace

void test_loot_light_through_the_world() {
    std::printf("a loot pile's light through the world's own aftermath: a stack worn in a slot gives the equipment light, a plant does not\n");
    Fixture fx(true);
    WolfWorld ww;
    ww.prepare(*fx.world);
    CHECK(ww.ground && fx.world->aftermath() != nullptr && fx.world->item_catalog() != nullptr, "a world with ground, its fights, the aftermath and the item catalog");
    if (!ww.ground || !fx.world->aftermath() || !fx.world->item_catalog()) return;
    combat::LootStack gear;
    gear.item = kLightWiringChest;
    gear.quantity = 1;
    gear.quality = 1;
    combat::LootStack plant;
    plant.item = kLightWiringDaffodil;
    plant.quantity = 3;
    plant.quality = 1;
    const repl::GuidValue owner{0x0DDD0001u, 0x0DDD0002u, 0x0DDD0003u, 0x0DDD0004u};
    std::string error;
    const uint64_t gear_pile = fx.world->drop_harvest_pile(kX, kY, kZ, owner, "Cyb", {gear}, error);
    const uint64_t plant_pile = fx.world->drop_harvest_pile(kX + 200, kY, kZ, owner, "Cyb", {plant}, error);
    const uint64_t mixed_pile = fx.world->drop_harvest_pile(kX + 400, kY, kZ, owner, "Cyb", {plant, gear}, error);
    CHECK(gear_pile != 0 && plant_pile != 0 && mixed_pile != 0, ("the three piles are made: " + error).c_str());
    if (!gear_pile || !plant_pile || !mixed_pile) return;
    const world::ActorRegistry& reg = fx.world->registry();
    CHECK(equipment_light_of(reg.find(gear_pile)) == 1, "a pile of one gear item has the equipment light (the world's aftermath asked its item catalog)");
    CHECK(equipment_light_of(reg.find(plant_pile)) < 1, "a pile of a plant has none");
    CHECK(equipment_light_of(reg.find(mixed_pile)) == 1, "a pile that holds a plant and a gear item has it");
}
AOC_REGISTER_TEST(test_loot_light_through_the_world);

void test_loot_light_after_a_take() {
    std::printf("the loot pile's light after the player takes from it: the Common helmet's ribbon, then the Rare material's regular spark\n");
    const svc::Character rec = bard25_bagged();   // the Sanctus resource bag: the Daffodil has a resource container to go to
    uint64_t pile = 0;
    bool ground = false;
    std::string pile_error;
    auto prepare = [&pile, &ground, &pile_error](PlayerWorld& w) {
        std::string err;
        auto nav = nav::AreaNav::from_file(navfix::build_file(), &err);
        ground = nav && w.adopt_ground(std::move(nav), err);
        combat::LootStack helmet;
        helmet.item = kLightWiringHelmet;
        helmet.quantity = 1;
        helmet.quality = 1;
        combat::LootStack material;
        material.item = kLightWiringDaffodil;
        material.quantity = 2;
        material.quality = 16;
        const repl::GuidValue owner{0x0BCD0001u, 0x0BCD0002u, 0x0BCD0003u, 0x0BCD0004u};
        pile = w.drop_harvest_pile(kX, kY, kZ, owner, "Cyb", {helmet, material}, pile_error);
    };
    Run r(0, true, &rec, prepare);
    CHECK(ground && pile != 0 && r.h.fx.world->aftermath() != nullptr, ("the pile stands where the player joins: " + pile_error).c_str());
    if (!pile || !r.h.fx.world->aftermath()) return;
    r.play(0.5, false);
    phase_in(r);
    world_play(r, 1.0);
    const bs::PlayerSetup setup = r.h.s->bootstrap().setup();
    const world::ActorRegistry& reg = r.h.fx.world->registry();
    CHECK(equipment_light_of(reg.find(pile)) == 1 && top_rarity_of(reg.find(pile)) == 0,
          "when the player joins, the pile shows the Common helmet's light (the ribbon), not the Rare material's colour");
    const combat::Corpse* c = r.h.fx.world->aftermath()->corpse_of(pile);
    CHECK(c && c->parts && c->remaining() == 2, "the pile's window has the helmet and the material");
    if (!c || !c->parts) return;
    send_on_pawn(r, interact_bunch(c->parts->interactable));
    world_play(r, 0.3);
    CHECK(r.m.loot_windows.size() == 1, "the player opens the pile's window");
    if (r.m.loot_windows.empty()) return;
    send_on_pc(r, loot_bunch({take_event(r.m.loot_windows.back(), 0, c->parts->container_guid, setup)}));
    world_play(r, 0.3);
    const combat::Corpse* left = r.h.fx.world->aftermath()->corpse_of(pile);
    CHECK(r.h.s->loot_taken() == 1 && left && left->remaining() == 1, "the player takes the helmet; the material stays");
    CHECK(equipment_light_of(reg.find(pile)) == 0 && top_rarity_of(reg.find(pile)) == 2,
          "the server works the light out again and sends it: the Rare material's regular spark, no equipment light");
    send_on_pc(r, loot_bunch({take_event(r.m.loot_windows.back(), 1, c->parts->container_guid, setup)}));
    world_play(r, 0.3);
    CHECK(r.h.s->loot_taken() == 2 && r.h.fx.world->aftermath()->corpse_of(pile) && r.h.fx.world->aftermath()->corpse_of(pile)->remaining() == 0,
          "the player takes the material too");
    CHECK(top_rarity_of(reg.find(pile)) == 7 && equipment_light_of(reg.find(pile)) <= 1, "the emptied pile keeps its end: TopRarity 7 (the light is off)");
}
AOC_REGISTER_TEST(test_loot_light_after_a_take);
