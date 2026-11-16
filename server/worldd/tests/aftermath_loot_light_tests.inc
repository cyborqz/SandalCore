// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  tests/aftermath_loot_light_tests.inc: the light a loot pile shows (included by test_worldd_aftermath.cpp)
//
//  The client draws a loot pile's light from its ULootComponent: CosmeticData.TopRarity names the rarity record whose P_Color and emitters it
//  applies, and CosmeticData.bHasEquipment picks the equipment's set (the ember burst, the item spike and the ribbon: the thin stream of light).
//  The checks: a pile of one Common item (no equipment), a Rare among Commons (the rarity's colour), a gear item (the equipment's light), the
//  corpse of a kill with two fighters and a bystander who may not loot it, a pile opened and emptied, two piles side by side, a client that opens
//  a pile later, a pile that expires, the size of the loot component's block (the field is sent only for loot that holds equipment; the bits around it are
//  the retail block's), and the light after a take: the best equipment's rarity while the pile holds equipment, then the best of what is left, sent to
//  every connection that holds the pile; a stack put back (the bag had no room); a take that changes the equipment light and not the rarity; and a mob's corpse
//  of real rolled loot, checked against the rule worked out here at its death and after every take.
// ============================================================================

#include "protocol/repl/block_reader.h"
#include "protocol/repl/replicator.h"
#include "protocol/schema/generated/index.h"

#include "items/inventory.h"

#include <algorithm>
#include <string>
#include <vector>

namespace {

constexpr int64_t kLightDaffodil = 6064629184431849472ll;   // Resource_Raw_Plant_Daffodil: no equip slot
constexpr int64_t kLightChest = 6064630214426886486ll;      // Gear_Armor_Light_Starter_Chest: equip slot 2 (Chest)
constexpr int64_t kLightHelmet = 6064630736269279236ll;     // Gear_Armor_Light_Bard_Head: equip slot 1 (Helmet)
constexpr int64_t kLightGlint = 6064628753953390592ll;      // Certificate_T1_Glint: no equip slot
constexpr int64_t kLightGlintT2 = 6064629147780775936ll;    // Certificate_T2_Glint: no equip slot, always Uncommon

combat::LootStack light_stack(int64_t item, int64_t quantity, int quality) {
    combat::LootStack s;
    s.item = item;
    s.quantity = quantity;
    s.quality = quality;
    return s;
}

/// CosmeticData.bHasEquipment of the actor's loot component (false when it has none, as the client reads an absent bool).
bool has_equipment_of(const world::WorldActor* a) {
    if (!a) return false;
    for (const repl::Subobject& s : a->actor.subobjects) {
        if (s.obj.schema && std::string(s.obj.schema->name) == "ULootComponent") return bool_of(s.obj, "CosmeticData.bHasEquipment", false);
    }
    return false;
}

/// The state of one subobject class as a connection that opens the actor now receives it: the block decoded from its own bits.
bool open_state_of(const world::WorldActor& a, const char* cls, repl::ObjectState& out) {
    repl::ActorShadow shadow;
    std::vector<repl::ContentBlock> blocks;
    if (!repl::collect_open_blocks(a.actor, shadow, blocks)) return false;
    for (const repl::ContentBlock& b : blocks) {
        if (!b.sub || !b.sub->obj.schema || std::string(b.sub->obj.schema->name) != cls) continue;
        std::string why;
        auto state = repl::load_object_state(*b.sub->obj.schema, b.payload.data(), b.payload.bit_pos(), b.has_rep_layout, &why);
        if (!state) return false;
        out = std::move(*state);
        return true;
    }
    return false;
}

/// The ULootComponent's block in the update the actor's change makes for a connection that holds it (`shadow`): false when the update carries none.
bool loot_update_of(const world::WorldActor& a, repl::ActorShadow& shadow, repl::ObjectState& out) {
    std::vector<repl::ContentBlock> blocks;
    if (!repl::collect_update_blocks(a.actor, shadow, blocks)) return false;
    for (const repl::ContentBlock& b : blocks) {
        if (!b.sub || !b.sub->obj.schema || std::string(b.sub->obj.schema->name) != "ULootComponent") continue;
        std::string why;
        auto state = repl::load_object_state(*b.sub->obj.schema, b.payload.data(), b.payload.bit_pos(), b.has_rep_layout, &why);
        if (!state) return false;
        out = std::move(*state);
        return true;
    }
    return false;
}

/// The players the decoded interactable's first input names (StateGuids), as the first word of each guid.
std::vector<int64_t> state_guids_in(const repl::ObjectState& o) {
    std::vector<int64_t> out;
    const repl::Value* v = o.state.get(*o.schema, "CurrentInputStates");
    const auto* arr = v ? v->get_if<repl::ArrayValue>() : nullptr;
    if (!arr || arr->elements.empty()) return out;
    const auto it = arr->elements[0].values.find(2);   // CurrentInputStates.CurrentState.StateGuids
    const auto* list = it != arr->elements[0].values.end() ? it->second.get_if<repl::ArrayValue>() : nullptr;
    if (!list) return out;
    for (const repl::ElementState& g : list->elements) {
        const auto w = g.values.find(1);
        const int64_t* word = w != g.values.end() ? w->second.get_if<int64_t>() : nullptr;
        out.push_back(word ? *word : -1);
    }
    return out;
}

}  // namespace

void test_loot_light_piles(Fixture& f) {
    std::printf("a loot pile's light: the rarity's record, and the equipment's embers, spike and ribbon only when the loot holds equipment\n");
    Scene s(f, 25);
    CHECK_OK(s.ok(), ("the scene: " + s.rig.problem + " " + s.w.problem).c_str());
    if (!s.ok()) return;
    items::ContentItemCatalog catalog(*f.db);
    s.after->set_equipment_of([&catalog](int64_t item) { return items::is_equipment(catalog, item); });
    repl::GuidValue owner{0x0CCC0001u, 0x0CCC0002u, 0x0CCC0003u, 0x0CCC0004u};
    std::string error;
    // one Common item: the rarity record Common, no equipment
    const uint64_t plain = s.after->drop_pile(100, 0, 1000, owner, "Cyb", {light_stack(kLightDaffodil, 3, 1)}, s.now, error, /*item_drop=*/true);
    // a Rare among Commons: TopRarity Rare (the blue light), no equipment
    const uint64_t rare = s.after->drop_pile(300, 0, 1000, owner, "Cyb", {light_stack(kLightDaffodil, 2, 1), light_stack(kLightDaffodil, 1, 16)}, s.now, error, true);
    // a Common gear item: the equipment's light at the Common colour
    const uint64_t gear = s.after->drop_pile(500, 0, 1000, owner, "Cyb", {light_stack(kLightChest, 1, 1)}, s.now, error, true);
    CHECK_OK(plain && rare && gear, ("the three piles are made: " + error).c_str());
    if (!plain || !rare || !gear) return;
    const world::WorldActor* pa = s.w.registry.find(plain);
    const world::WorldActor* ra = s.w.registry.find(rare);
    const world::WorldActor* ga = s.w.registry.find(gear);
    CHECK_OK(pa && top_rarity_of(*pa) == 0 && !has_equipment_of(pa), "one Common item: TopRarity Common and no equipment light");
    CHECK_OK(ra && top_rarity_of(*ra) == 2 && !has_equipment_of(ra), "a Rare among Commons: TopRarity Rare and no equipment light");
    CHECK_OK(ga && top_rarity_of(*ga) == 0 && has_equipment_of(ga), "one Common gear item: TopRarity Common and the equipment light");
    CHECK_OK(items::is_equipment(catalog, kLightChest) && !items::is_equipment(catalog, kLightDaffodil) && !items::is_equipment(catalog, kLightGlint),
             "the rule on the real rows: the chest is worn in a slot, the daffodil and the Glint are not");
    // two piles side by side, each with its own light: emptying one leaves the other's
    s.after->take(plain, 0);
    s.after->emptied(plain, s.now);
    CHECK_OK(top_rarity_of(*s.w.registry.find(plain)) == 7 && state_guids_of(s.w.registry.find(plain)).empty(), "an emptied pile says emptied (TopRarity 7) and is offered to nobody");
    CHECK_OK(top_rarity_of(*s.w.registry.find(rare)) == 2 && state_guids_of(s.w.registry.find(rare)).size() == 1, "the pile beside it keeps its light and its offer");
    // a client that opens a pile later receives the light's fields in the open (a reconnect near a pile)
    repl::ObjectState open_loot, open_inter, open_emptied_loot, open_emptied_inter;
    CHECK_OK(open_state_of(*s.w.registry.find(gear), "ULootComponent", open_loot), "a new connection's open carries the gear pile's loot component");
    CHECK_OK(has_equipment_of(s.w.registry.find(gear)) && bool_of(open_loot, "CosmeticData.bHasEquipment", false) && number_of(open_loot, "CosmeticData.TopRarity") == 0,
             "the open says TopRarity Common and bHasEquipment 1 (the equipment light)");
    CHECK_OK(open_state_of(*s.w.registry.find(gear), "UInteractableComponent", open_inter) && state_guids_in(open_inter).size() == 1,
             "and its interaction is offered to the one player");
    CHECK_OK(open_state_of(*s.w.registry.find(plain), "ULootComponent", open_emptied_loot) && number_of(open_emptied_loot, "CosmeticData.TopRarity") == 7,
             "an emptied pile's open says TopRarity 7 (the light is off)");
    CHECK_OK(open_state_of(*s.w.registry.find(plain), "UInteractableComponent", open_emptied_inter) && state_guids_in(open_emptied_inter).empty(),
             "and its interaction is offered to nobody");
    // the light goes with the pile when its time comes
    s.run(4.5);
    CHECK_OK(s.w.registry.find(plain) == nullptr && s.w.registry.find(rare) != nullptr, "4.5 s after it was emptied the pile is gone; the other pile is still there");
}

void test_loot_light_corpse(Fixture& f) {
    std::printf("a mob's kill: its corpse carries the rarity's and the equipment's light, offered to the fighters and not to a bystander\n");
    combat::AftermathConfig cfg;
    cfg.loot_chance_mult = 100.0;   // the gear's percent rolls always pass: a kill's loot holds equipment
    cfg.loot_rarity_boost = 20.0;
    Scene s(f, 25, cfg);
    CHECK_OK(s.ok(), ("the scene: " + s.rig.problem + " " + s.w.problem).c_str());
    if (!s.ok()) return;
    items::ContentItemCatalog catalog(*f.db);
    s.after->set_equipment_of([&catalog](int64_t item) { return items::is_equipment(catalog, item); });
    // a second fighter and a bystander who never fights (their own character guids: the rig's default is the same for every player)
    Rig rig88(4, 25, {}), rig99(4, 25, {});
    combat_rig::FakePlayer other(rig88, 88, combat::Vec3{600, 0, 1098});
    combat_rig::FakePlayer bystander(rig99, 99, combat::Vec3{900, 0, 1098});
    other.guid = {0x22220001u, 0x22220002u, 0x22220003u, 0x22220004u};
    bystander.guid = {0x33330001u, 0x33330002u, 0x33330003u, 0x33330004u};
    s.world->add_player(&other);
    s.world->add_player(&bystander);
    uint64_t corpse_id = 0;
    for (int i = 0; i < 20 && !corpse_id; ++i) {
        const uint64_t id = s.wolf(18, 100.0 + 400.0 * i);   // the owner's level 25 against a level-18 mob (7 levels apart: Loot_Falloff 1)
        if (!id) break;
        combat::Resolved r;
        r.mods.push_back({1, content::kGuidHealth, -1.0});
        s.world->apply_hit_to_npc(id, 88, r, s.now);
        s.kill(id, 77);
        const combat::Corpse* c = s.after->corpse_of(id);
        if (c && c->parts) corpse_id = id;
    }
    CHECK_OK(corpse_id != 0, "a level-18 Wolf killed by a level-25 player drops loot");
    if (!corpse_id) return;
    const world::WorldActor* a = s.w.registry.find(corpse_id);
    const combat::Corpse* c = s.after->corpse_of(corpse_id);
    bool any_equipment = false;
    for (const combat::LootStack& stack : c->loot) any_equipment = any_equipment || items::is_equipment(catalog, stack.item);
    CHECK_OK(any_equipment, "its loot holds a gear item");
    CHECK_OK(a && top_rarity_of(*a) == c->top_rarity && c->top_rarity >= 0 && c->top_rarity < 7, "its loot component's TopRarity is the best stack's");
    CHECK_OK(has_equipment_of(a), "and bHasEquipment is 1: the client draws the equipment's light");
    const std::vector<int64_t> offered = state_guids_of(a);
    const auto guid_word_of = [](const combat_rig::FakePlayer& p) { return static_cast<int64_t>(static_cast<int32_t>(p.character_guid()[0])); };
    CHECK_OK(offered.size() == 2 && std::find(offered.begin(), offered.end(), guid_word_of(other)) != offered.end() &&
                 std::find(offered.begin(), offered.end(), guid_word_of(bystander)) == offered.end(),
             "the corpse is offered to the two fighters and not to the bystander");
    repl::ObjectState open_loot, open_inter;
    CHECK_OK(open_state_of(*a, "ULootComponent", open_loot) && bool_of(open_loot, "CosmeticData.bHasEquipment", false) &&
                 number_of(open_loot, "CosmeticData.TopRarity") == static_cast<uint64_t>(c->top_rarity),
             "a new connection's open carries the same rarity and equipment");
    CHECK_OK(open_state_of(*a, "UInteractableComponent", open_inter) && state_guids_in(open_inter).size() == 2, "and the same two players");
    // the last stack taken: the light goes
    for (size_t i = 0; i < c->loot.size(); ++i) s.after->take(corpse_id, i);
    s.after->emptied(corpse_id, s.now);
    CHECK_OK(top_rarity_of(*s.w.registry.find(corpse_id)) == 7 && state_guids_of(s.w.registry.find(corpse_id)).empty(),
             "the last stack taken: TopRarity 7 and the interaction closed (the light goes)");
    repl::ObjectState empty_loot;
    CHECK_OK(open_state_of(*s.w.registry.find(corpse_id), "ULootComponent", empty_loot) && number_of(empty_loot, "CosmeticData.TopRarity") == 7,
             "a client that opens it now sees the emptied loot");
}

void test_loot_light_owner_case(Fixture& f) {
    std::printf("the owner's case: his level-25 character and a Riverlands mob within the 9-level loot falloff\n");
    Scene s(f, 25);
    CHECK_OK(s.ok(), ("the scene: " + s.rig.problem + " " + s.w.problem).c_str());
    if (!s.ok()) return;
    items::ContentItemCatalog catalog(*f.db);
    s.after->set_equipment_of([&catalog](int64_t item) { return items::is_equipment(catalog, item); });
    // within the falloff (7 levels): loot, and its light fields are written; 10 levels apart: no loot, so no light
    uint64_t near_id = 0, far_id = 0;
    for (int i = 0; i < 20 && (!near_id || !far_id); ++i) {
        const uint64_t near = s.wolf(18, 100.0 + 400.0 * i);
        s.kill(near);
        const combat::Corpse* nc = s.after->corpse_of(near);
        if (nc && nc->parts && !near_id) near_id = near;
        const uint64_t far = s.wolf(15, 300.0 + 400.0 * i);   // 10 levels apart: Loot_Falloff 0
        s.kill(far);
        const combat::Corpse* fc = s.after->corpse_of(far);
        if (fc && !fc->parts && !far_id) far_id = far;
    }
    CHECK_OK(near_id != 0, "a level-18 mob beside the level-25 player drops loot");
    CHECK_OK(far_id != 0, "a level-15 mob (10 levels apart) drops none: the loot falloff");
    if (!near_id) return;
    const world::WorldActor* a = s.w.registry.find(near_id);
    CHECK_OK(a && top_rarity_of(*a) >= 0 && top_rarity_of(*a) < 7, "its corpse's loot component names a rarity (the light's colour)");
    CHECK_OK(far_id != 0 && count_sub(*s.w.registry.find(far_id), "ULootComponent", true) == 0, "the falloff's corpse has no loot component, so no light");
}

namespace {

/// The size in bits of the ULootComponent block a connection that opens the actor now receives (0: the actor has none).
size_t open_loot_bits_of(const world::WorldActor& a) {
    repl::ActorShadow shadow;
    std::vector<repl::ContentBlock> blocks;
    if (!repl::collect_open_blocks(a.actor, shadow, blocks)) return 0;
    for (const repl::ContentBlock& b : blocks) {
        if (b.sub && b.sub->obj.schema && std::string(b.sub->obj.schema->name) == "ULootComponent") return b.payload.bit_pos();
    }
    return 0;
}

}  // namespace

void test_loot_light_block(Fixture& f) {
    std::printf("the light's field in the loot component: sent only when the loot holds equipment, every other field as it was\n");
    Scene s(f, 25);
    CHECK_OK(s.ok(), ("the scene: " + s.rig.problem + " " + s.w.problem).c_str());
    if (!s.ok()) return;
    // two corpses made the way the death hook makes them: the loot component straight on an NPC, with and without equipment
    const uint64_t plain_id = s.wolf(10, 100), gear_id = s.wolf(10, 300);
    const repl::GuidValue me{0x0EEE0001u, 0x0EEE0002u, 0x0EEE0003u, 0x0EEE0004u};
    const auto mint = [&] { return s.guids.next(); };
    std::string why;
    const auto plain = world::add_loot(s.w.registry, plain_id, mint, me, {me}, 1, false, why);
    const auto gear = world::add_loot(s.w.registry, gear_id, mint, me, {me}, 1, true, why);
    CHECK_OK(plain && gear, ("both corpses get their loot components: " + why).c_str());
    const world::WorldActor* pa = s.w.registry.find(plain_id);
    const world::WorldActor* ga = s.w.registry.find(gear_id);
    if (!plain || !gear || !pa || !ga) return;
    const size_t plain_bits = open_loot_bits_of(*pa), gear_bits = open_loot_bits_of(*ga);
    CHECK_OK(plain_bits == 317, "a corpse's loot component with no equipment is the size of the retail block (317 bits): the field is left out, as it is there");
    CHECK_OK(plain_bits > 0 && gear_bits == plain_bits + 9, "with equipment it is 9 bits longer (the field's handle and its bit) and nothing else changes in size");
    repl::ObjectState plain_state, gear_state;
    CHECK_OK(open_state_of(*pa, "ULootComponent", plain_state) && open_state_of(*ga, "ULootComponent", gear_state), "both blocks decode from their own bits");
    CHECK_OK(!has_value(plain_state, "CosmeticData.bHasEquipment") && bool_of(gear_state, "CosmeticData.bHasEquipment", false),
             "the block without equipment sends no bHasEquipment (the client reads an absent bool as clear); the one with it sends 1");
    bool same = true;
    for (const char* path : {"OwnerGuid.A", "OwnerGuid.B", "OwnerGuid.C", "OwnerGuid.D", "CosmeticData.TopRarity"}) {
        const repl::Value* a = plain_state.state.get(*plain_state.schema, path);
        const repl::Value* b = gear_state.state.get(*gear_state.schema, path);
        same = same && a && b && *a == *b;
    }
    CHECK_OK(same, "the owner's guid and TopRarity decode the same in both: the bits before the field are as they were");
    CHECK_OK(has_value(plain_state, "ServerLootContainer") && has_value(gear_state, "ServerLootContainer"),
             "and the loot container's reference after the field decodes in both: the bits after it are as they were");
}

void test_loot_light_takes(Fixture& f) {
    std::printf("the loot pile's light after a take: the best equipment's rarity while it holds equipment, then the best of what is left\n");
    Scene s(f, 25);
    CHECK_OK(s.ok(), ("the scene: " + s.rig.problem + " " + s.w.problem).c_str());
    if (!s.ok()) return;
    items::ContentItemCatalog catalog(*f.db);
    s.after->set_equipment_of([&catalog](int64_t item) { return items::is_equipment(catalog, item); });
    s.after->set_rarity_of([&catalog](int64_t item, int quality) {
        const auto info = catalog.info(item);
        return info ? info->rarity.rarity_of(static_cast<double>(quality)) : items::default_rarity_facts().rarity_of(static_cast<double>(quality));
    });
    const repl::GuidValue owner{0x0ABB0001u, 0x0ABB0002u, 0x0ABB0003u, 0x0ABB0004u};
    std::string error;
    CHECK_OK(items::is_equipment(catalog, kLightHelmet) && !items::is_equipment(catalog, kLightDaffodil) && !items::is_equipment(catalog, kLightGlintT2),
             "the rule on the real rows: the Bard's helmet is worn in a slot, the daffodil and the Glint are not");
    // the owner's case, at both pile kinds: a Common helmet (the equipment light), a Rare material beside it
    for (int kind = 0; kind < 2; ++kind) {
        const bool item_drop = kind == 0;
        const std::string what = item_drop ? "a full bag's pile" : "a dead player's pile";
        const uint64_t pile = s.after->drop_pile(100.0 + 400.0 * kind, 0, 1000, owner, "Cyb", {light_stack(kLightHelmet, 1, 1), light_stack(kLightDaffodil, 3, 16)}, s.now,
                                                 error, item_drop);
        CHECK_OK(pile != 0, (what + " of a Common helmet and a Rare material is made: " + error).c_str());
        if (!pile) return;
        const world::WorldActor* a = s.w.registry.find(pile);
        CHECK_OK(a && top_rarity_of(*a) == 0 && has_equipment_of(a),
                 (what + ": the Common helmet alone colours it (the ribbon at the Common colour) while the material is Rare").c_str());
        repl::ActorShadow first, second;
        std::vector<repl::ContentBlock> blocks;
        CHECK_OK(a && repl::collect_open_blocks(a->actor, first, blocks) && repl::collect_open_blocks(a->actor, second, blocks),
                 (what + ": two connections hold it").c_str());
        repl::ObjectState open_before;
        CHECK_OK(open_state_of(*a, "ULootComponent", open_before) && number_of(open_before, "CosmeticData.TopRarity") == 0 &&
                     bool_of(open_before, "CosmeticData.bHasEquipment", false),
                 (what + ": a connection that opens it sees TopRarity Common and the equipment light").c_str());
        CHECK_OK(s.after->take(pile, 0) != nullptr, (what + ": the helmet is taken").c_str());
        const world::WorldActor* after_take = s.w.registry.find(pile);
        CHECK_OK(after_take && top_rarity_of(*after_take) == 2 && !has_equipment_of(after_take),
                 (what + ": the Rare material left makes TopRarity Rare without the equipment light (the regular spark)").c_str());
        const combat::Corpse* left = s.after->corpse_of(pile);
        CHECK_OK(left && left->remaining() == 1 && left->top_rarity == 2, (what + ": its record says the Rare material is what is left").c_str());
        repl::ObjectState update_first, update_second;
        CHECK_OK(after_take && loot_update_of(*after_take, first, update_first) && number_of(update_first, "CosmeticData.TopRarity") == 2 &&
                     has_value(update_first, "CosmeticData.bHasEquipment") && !bool_of(update_first, "CosmeticData.bHasEquipment", true),
                 (what + ": the first connection's update says TopRarity Rare and bHasEquipment 0").c_str());
        CHECK_OK(after_take && loot_update_of(*after_take, second, update_second) && number_of(update_second, "CosmeticData.TopRarity") == 2 &&
                     has_value(update_second, "CosmeticData.bHasEquipment") && !bool_of(update_second, "CosmeticData.bHasEquipment", true),
                 (what + ": the second connection's too: every connection that holds the pile is told").c_str());
        repl::ObjectState open_after;
        CHECK_OK(after_take && open_state_of(*after_take, "ULootComponent", open_after) && number_of(open_after, "CosmeticData.TopRarity") == 2 &&
                     has_value(open_after, "CosmeticData.bHasEquipment") && !bool_of(open_after, "CosmeticData.bHasEquipment", true),
                 (what + ": a connection that opens it after the take sees the Rare regular spark").c_str());
        CHECK_OK(s.after->take(pile, 1) != nullptr && s.after->corpse_of(pile)->remaining() == 0, (what + ": the last stack taken").c_str());
        s.after->emptied(pile, s.now);
        CHECK_OK(top_rarity_of(*s.w.registry.find(pile)) == 7 && state_guids_of(s.w.registry.find(pile)).empty(),
                 (what + ": emptied it says TopRarity 7 and is offered to nobody, as before").c_str());
    }
    // two rarities of equipment with a better material: the equipment only, then the better material once the equipment is gone
    {
        const uint64_t pile = s.after->drop_pile(900, 0, 1000, owner, "Cyb",
                                                 {light_stack(kLightChest, 1, 1), light_stack(kLightChest, 1, 16), light_stack(kLightDaffodil, 2, 51)}, s.now, error, true);
        CHECK_OK(pile != 0, ("the two-rarity pile is made: " + error).c_str());
        if (!pile) return;
        const world::WorldActor* a = s.w.registry.find(pile);
        CHECK_OK(a && top_rarity_of(*a) == 2 && has_equipment_of(a), "a Common and a Rare chest with an Epic material: TopRarity Rare (the equipment only) and the equipment light");
        repl::ActorShadow held;
        std::vector<repl::ContentBlock> blocks;
        CHECK_OK(a && repl::collect_open_blocks(a->actor, held, blocks), "a connection holds it");
        CHECK_OK(s.after->take(pile, 1) != nullptr, "the Rare chest is taken");
        const world::WorldActor* after_rare = s.w.registry.find(pile);
        CHECK_OK(after_rare && top_rarity_of(*after_rare) == 0 && has_equipment_of(after_rare), "the Common chest left: TopRarity Common and still the equipment light");
        repl::ObjectState update_rare;
        CHECK_OK(after_rare && loot_update_of(*after_rare, held, update_rare) && number_of(update_rare, "CosmeticData.TopRarity") == 0 &&
                     !has_value(update_rare, "CosmeticData.bHasEquipment"),
                 "its update says TopRarity Common and does not repeat the equipment light, which is still on");
        CHECK_OK(s.after->take(pile, 0) != nullptr, "the Common chest is taken too");
        const world::WorldActor* after_common = s.w.registry.find(pile);
        CHECK_OK(after_common && top_rarity_of(*after_common) == 4 && !has_equipment_of(after_common), "the Epic material left: TopRarity Epic without the equipment light");
        repl::ObjectState update_common;
        CHECK_OK(after_common && loot_update_of(*after_common, held, update_common) && number_of(update_common, "CosmeticData.TopRarity") == 4 &&
                     has_value(update_common, "CosmeticData.bHasEquipment") && !bool_of(update_common, "CosmeticData.bHasEquipment", true),
                 "its update says TopRarity Epic and bHasEquipment 0");
        CHECK_OK(s.after->take(pile, 2) != nullptr, "the Epic material is taken");
        s.after->emptied(pile, s.now);
        CHECK_OK(top_rarity_of(*s.w.registry.find(pile)) == 7, "the emptied pile says TopRarity 7");
    }
    // Glint only: the plain set, the best tier's colour, no equipment light ever
    {
        const uint64_t pile = s.after->drop_pile(1300, 0, 1000, owner, "Cyb", {light_stack(kLightGlintT2, 1, 21), light_stack(kLightGlint, 1, 1)}, s.now, error, true);
        CHECK_OK(pile != 0, ("the Glint pile is made: " + error).c_str());
        if (!pile) return;
        const world::WorldActor* a = s.w.registry.find(pile);
        repl::ObjectState open_before;
        CHECK_OK(a && top_rarity_of(*a) == 1 && !has_equipment_of(a) && open_state_of(*a, "ULootComponent", open_before) &&
                     number_of(open_before, "CosmeticData.TopRarity") == 1 && !has_value(open_before, "CosmeticData.bHasEquipment"),
                 "Glint only: TopRarity Uncommon and no bHasEquipment field (the regular spark)");
        repl::ActorShadow held;
        std::vector<repl::ContentBlock> blocks;
        CHECK_OK(a && repl::collect_open_blocks(a->actor, held, blocks), "a connection holds the Glint pile");
        CHECK_OK(s.after->take(pile, 0) != nullptr, "the Uncommon Glint is taken");
        const world::WorldActor* after = s.w.registry.find(pile);
        repl::ObjectState update;
        CHECK_OK(after && top_rarity_of(*after) == 0 && !has_equipment_of(after) && loot_update_of(*after, held, update) &&
                     number_of(update, "CosmeticData.TopRarity") == 0 && !has_value(update, "CosmeticData.bHasEquipment"),
                 "the Common Glint left: TopRarity Common, still no equipment light, sent in the update");
        CHECK_OK(s.after->take(pile, 1) != nullptr, "the Common Glint is taken");
        s.after->emptied(pile, s.now);
        CHECK_OK(top_rarity_of(*s.w.registry.find(pile)) == 7, "the emptied Glint pile says TopRarity 7");
    }
    // a pile of materials only: the best material's rarity, worked out again after the best is taken
    {
        const uint64_t pile = s.after->drop_pile(1700, 0, 1000, owner, "Cyb", {light_stack(kLightDaffodil, 2, 1), light_stack(kLightDaffodil, 1, 16)}, s.now, error, true);
        CHECK_OK(pile != 0, ("the materials pile is made: " + error).c_str());
        if (!pile) return;
        const world::WorldActor* a = s.w.registry.find(pile);
        CHECK_OK(a && top_rarity_of(*a) == 2 && !has_equipment_of(a), "materials only: TopRarity Rare (the best material) and no equipment light");
        repl::ActorShadow held;
        std::vector<repl::ContentBlock> blocks;
        CHECK_OK(a && repl::collect_open_blocks(a->actor, held, blocks), "a connection holds the materials pile");
        CHECK_OK(s.after->take(pile, 1) != nullptr, "the Rare material is taken");
        const world::WorldActor* after = s.w.registry.find(pile);
        repl::ObjectState update;
        CHECK_OK(after && top_rarity_of(*after) == 0 && loot_update_of(*after, held, update) && number_of(update, "CosmeticData.TopRarity") == 0,
                 "the Common material left: TopRarity Common, sent in the update");
        CHECK_OK(s.after->take(pile, 0) != nullptr, "the Common material is taken");
        s.after->emptied(pile, s.now);
        CHECK_OK(top_rarity_of(*s.w.registry.find(pile)) == 7, "the emptied materials pile says TopRarity 7");
    }
    // two piles side by side: a take from one leaves the other's light and sends it nothing
    {
        const uint64_t one = s.after->drop_pile(2100, 0, 1000, owner, "Cyb", {light_stack(kLightHelmet, 1, 1), light_stack(kLightDaffodil, 1, 16)}, s.now, error, true);
        const uint64_t two = s.after->drop_pile(2200, 0, 1000, owner, "Cyb", {light_stack(kLightHelmet, 1, 1)}, s.now, error, true);
        CHECK_OK(one != 0 && two != 0, ("the two piles are made: " + error).c_str());
        if (!one || !two) return;
        const world::WorldActor* b = s.w.registry.find(two);
        repl::ActorShadow other;
        std::vector<repl::ContentBlock> blocks;
        CHECK_OK(b && repl::collect_open_blocks(b->actor, other, blocks), "a connection holds the other pile");
        CHECK_OK(s.after->take(one, 0) != nullptr, "the helmet of the first pile is taken");
        const world::WorldActor* first = s.w.registry.find(one);
        CHECK_OK(first && top_rarity_of(*first) == 2 && !has_equipment_of(first), "the first pile turns to the Rare material's regular spark");
        CHECK_OK(b && top_rarity_of(*b) == 0 && has_equipment_of(b), "the pile beside it keeps its Common equipment light");
        std::vector<repl::ContentBlock> untouched;
        CHECK_OK(b && repl::collect_update_blocks(b->actor, other, untouched) && untouched.empty(), "the take sends the other pile's connections nothing");
        CHECK_OK(s.after->take(one, 1) != nullptr, "the first pile's material is taken");
        s.after->emptied(one, s.now);
        CHECK_OK(top_rarity_of(*s.w.registry.find(one)) == 7 && top_rarity_of(*s.w.registry.find(two)) == 0,
                 "the first pile is emptied while the other still shows its light");
    }
}

namespace {

/// What the light of the stacks that are left must say, worked out here from the catalog's own rows and not by the aftermath: while any stack that is left is equipment, the best
/// rarity among the equipment stacks and the flag set; else the best rarity of every stack that is left and the flag clear.
struct LightWanted {
    int  top = 0;
    bool equipment = false;
    bool operator==(const LightWanted& o) const { return top == o.top && equipment == o.equipment; }
};

int light_rarity_of(const items::ItemCatalog& catalog, const combat::LootStack& s) {
    const auto info = catalog.info(s.item);
    return info ? info->rarity.rarity_of(static_cast<double>(s.quality)) : items::default_rarity_facts().rarity_of(static_cast<double>(s.quality));
}

LightWanted light_wanted(const items::ItemCatalog& catalog, const std::vector<combat::LootStack>& stacks) {
    LightWanted w;
    for (const combat::LootStack& s : stacks) w.equipment = w.equipment || (!s.taken && items::is_equipment(catalog, s.item));
    for (const combat::LootStack& s : stacks) {
        if (s.taken || (w.equipment && !items::is_equipment(catalog, s.item))) continue;
        w.top = std::max(w.top, light_rarity_of(catalog, s));
    }
    return w;
}

}  // namespace

void test_loot_light_put_back(Fixture& f) {
    std::printf("the loot pile's light when a taken stack is put back (the bag had no room for it), and when a take changes the equipment light and not the rarity\n");
    Scene s(f, 25);
    CHECK_OK(s.ok(), ("the scene: " + s.rig.problem + " " + s.w.problem).c_str());
    if (!s.ok()) return;
    items::ContentItemCatalog catalog(*f.db);
    s.after->set_equipment_of([&catalog](int64_t item) { return items::is_equipment(catalog, item); });
    s.after->set_rarity_of([&catalog](int64_t item, int quality) {
        const auto info = catalog.info(item);
        return info ? info->rarity.rarity_of(static_cast<double>(quality)) : items::default_rarity_facts().rarity_of(static_cast<double>(quality));
    });
    const repl::GuidValue owner{0x0ABC0001u, 0x0ABC0002u, 0x0ABC0003u, 0x0ABC0004u};
    std::string error;
    // the owner's case at both pile kinds: the helmet is taken, the bag has no room, and the stack goes back: the ribbon is back too
    for (int kind = 0; kind < 2; ++kind) {
        const bool item_drop = kind == 0;
        const std::string what = item_drop ? "a full bag's pile" : "a dead player's pile";
        const uint64_t pile = s.after->drop_pile(100.0 + 400.0 * kind, 0, 1000, owner, "Cyb", {light_stack(kLightHelmet, 1, 1), light_stack(kLightDaffodil, 3, 16)}, s.now, error,
                                                 item_drop);
        CHECK_OK(pile != 0, (what + " of a Common helmet and a Rare material is made: " + error).c_str());
        if (!pile) return;
        const world::WorldActor* a = s.w.registry.find(pile);
        repl::ActorShadow held;
        std::vector<repl::ContentBlock> blocks;
        CHECK_OK(a && repl::collect_open_blocks(a->actor, held, blocks), (what + ": a connection holds it").c_str());
        CHECK_OK(s.after->take(pile, 0) != nullptr && top_rarity_of(*a) == 2 && !has_equipment_of(a), (what + ": the helmet is taken: the Rare regular spark").c_str());
        repl::ObjectState sent_take;
        CHECK_OK(loot_update_of(*a, held, sent_take) && number_of(sent_take, "CosmeticData.TopRarity") == 2, (what + ": the connection is told the spark").c_str());
        s.after->put_back(pile, 0);
        const combat::Corpse* c = s.after->corpse_of(pile);
        CHECK_OK(c && c->remaining() == 2 && c->top_rarity == 0 && c->has_equipment, (what + ": the helmet put back: the record says the Common equipment light again").c_str());
        CHECK_OK(top_rarity_of(*a) == 0 && has_equipment_of(a), (what + ": the pile shows the Common ribbon again, as the helmet is still in it").c_str());
        repl::ObjectState sent_back;
        CHECK_OK(loot_update_of(*a, held, sent_back) && number_of(sent_back, "CosmeticData.TopRarity") == 0 && has_value(sent_back, "CosmeticData.bHasEquipment") &&
                     bool_of(sent_back, "CosmeticData.bHasEquipment", false),
                 (what + ": and the connection is told the ribbon is back (TopRarity Common, bHasEquipment 1)").c_str());
        // the whole pile taken in one request, then the helmet does not fit: only the helmet goes back
        CHECK_OK(s.after->take(pile, 0) != nullptr && s.after->take(pile, 1) != nullptr && s.after->corpse_of(pile)->remaining() == 0, (what + ": both stacks taken").c_str());
        s.after->put_back(pile, 0);
        CHECK_OK(top_rarity_of(*a) == 0 && has_equipment_of(a) && s.after->corpse_of(pile)->remaining() == 1,
                 (what + ": the helmet back, the material gone: the pile shows the helmet's Common ribbon").c_str());
        repl::ObjectState sent_one;
        CHECK_OK(!loot_update_of(*a, held, sent_one), (what + ": the connection's copy is already right: nothing more is sent").c_str());
        // the material does not fit either
        s.after->put_back(pile, 1);
        CHECK_OK(top_rarity_of(*a) == 0 && has_equipment_of(a) && s.after->corpse_of(pile)->remaining() == 2, (what + ": the material back too: the helmet still decides the light").c_str());
        // the other order: the material is the one that does not fit
        CHECK_OK(s.after->take(pile, 1) != nullptr && s.after->take(pile, 0) != nullptr, (what + ": both taken again").c_str());
        s.after->put_back(pile, 1);
        CHECK_OK(top_rarity_of(*a) == 2 && !has_equipment_of(a) && s.after->corpse_of(pile)->remaining() == 1,
                 (what + ": only the Rare material goes back: its regular spark, no equipment light").c_str());
        CHECK_OK(s.after->take(pile, 1) != nullptr, (what + ": and it is taken").c_str());
        s.after->emptied(pile, s.now);
        CHECK_OK(top_rarity_of(*s.w.registry.find(pile)) == 7 && state_guids_of(s.w.registry.find(pile)).empty(), (what + ": emptied, as before").c_str());
    }
    // the same rarity before and after: a Common helmet beside a Common material.  The helmet's taking changes the light's shape and not its colour, so the flag alone is sent
    for (int kind = 0; kind < 2; ++kind) {
        const bool item_drop = kind == 0;
        const std::string what = item_drop ? "a full bag's pile" : "a dead player's pile";
        const uint64_t pile = s.after->drop_pile(1100.0 + 400.0 * kind, 0, 1000, owner, "Cyb", {light_stack(kLightHelmet, 1, 1), light_stack(kLightDaffodil, 3, 1)}, s.now, error,
                                                 item_drop);
        CHECK_OK(pile != 0, (what + " of a Common helmet and a Common material is made: " + error).c_str());
        if (!pile) return;
        const world::WorldActor* a = s.w.registry.find(pile);
        repl::ActorShadow held;
        std::vector<repl::ContentBlock> blocks;
        CHECK_OK(a && top_rarity_of(*a) == 0 && has_equipment_of(a) && repl::collect_open_blocks(a->actor, held, blocks),
                 (what + ": the Common ribbon, and a connection holds it").c_str());
        CHECK_OK(s.after->take(pile, 0) != nullptr, (what + ": the helmet is taken").c_str());
        const combat::Corpse* c = s.after->corpse_of(pile);
        CHECK_OK(top_rarity_of(*a) == 0 && !has_equipment_of(a) && c && c->top_rarity == 0 && !c->has_equipment,
                 (what + ": the colour is still Common and the ribbon is gone (the regular Common spark)").c_str());
        repl::ObjectState sent_take;
        CHECK_OK(loot_update_of(*a, held, sent_take) && has_value(sent_take, "CosmeticData.bHasEquipment") && !bool_of(sent_take, "CosmeticData.bHasEquipment", true) &&
                     !has_value(sent_take, "CosmeticData.TopRarity"),
                 (what + ": the connection is told bHasEquipment 0 and not the colour again").c_str());
        s.after->put_back(pile, 0);
        CHECK_OK(top_rarity_of(*a) == 0 && has_equipment_of(a) && s.after->corpse_of(pile)->has_equipment, (what + ": the helmet put back: the ribbon is back").c_str());
        repl::ObjectState sent_back;
        CHECK_OK(loot_update_of(*a, held, sent_back) && has_value(sent_back, "CosmeticData.bHasEquipment") && bool_of(sent_back, "CosmeticData.bHasEquipment", false),
                 (what + ": and the connection is told").c_str());
    }
}

void test_loot_light_of_a_kill(Fixture& f) {
    std::printf("a mob's corpse, the main case: the real rolled loot gives the rule's light at the death and after every take, to a connection that holds it\n");
    combat::AftermathConfig cfg;
    cfg.loot_chance_mult = 100.0;   // the gear's percent rolls always pass: a kill's loot holds equipment and several other stacks
    Scene s(f, 25, cfg);
    CHECK_OK(s.ok(), ("the scene: " + s.rig.problem + " " + s.w.problem).c_str());
    if (!s.ok()) return;
    items::ContentItemCatalog catalog(*f.db);
    s.after->set_equipment_of([&catalog](int64_t item) { return items::is_equipment(catalog, item); });
    s.after->set_rarity_of([&catalog](int64_t item, int quality) {
        const auto info = catalog.info(item);
        return info ? info->rarity.rarity_of(static_cast<double>(quality)) : items::default_rarity_facts().rarity_of(static_cast<double>(quality));
    });
    size_t corpses = 0, mixed = 0, steps = 0, flag_only = 0, wrong_death = 0, wrong_take = 0, wrong_update = 0;
    int spawned = 0;
    // this rig's reward book rolls no rarities (every stack comes out at quality 1, Common): the first regime keeps that (the gear and the others are all Common, so the equipment light
    // goes and the colour stays); in the second a drop that is not equipment takes the quality of a Rare, which makes the loot hold Common gear beside better stacks, as a world that rolls
    // rarities makes it
    for (int regime = 0; regime < 2; ++regime) {
        if (regime == 1) s.after->set_quality_of([&catalog](int64_t item) { return items::is_equipment(catalog, item) ? 1 : 16; });
        size_t checked = 0;
        for (int i = 0; i < 40 && checked < 8; ++i) {
            const uint64_t id = s.wolf(18, 100.0 + 250.0 * spawned++);   // the owner's level 25 against a level-18 mob (7 levels apart: Loot_Falloff 1)
            if (!id) break;
            s.kill(id);
            const combat::Corpse* c = s.after->corpse_of(id);
            if (!c || !c->parts || c->loot.size() < 2) continue;
            ++corpses;
            ++checked;
            const world::WorldActor* a = s.w.registry.find(id);
            LightWanted want = light_wanted(catalog, c->loot);
            int best_of_all = 0;
            for (const combat::LootStack& stack : c->loot) best_of_all = std::max(best_of_all, light_rarity_of(catalog, stack));
            if (want.equipment && best_of_all > want.top) ++mixed;   // a corpse where a better stack sits beside the gear: the owner's case
            if (!a || top_rarity_of(*a) != want.top || has_equipment_of(a) != want.equipment || c->top_rarity != want.top || c->has_equipment != want.equipment) {
                ++wrong_death;
                std::printf("  corpse %zu at its death: wanted TopRarity %d equipment %d, the component says %lld, %d, its record %d, %d\n", corpses, want.top,
                            want.equipment ? 1 : 0, a ? static_cast<long long>(top_rarity_of(*a)) : -1ll, has_equipment_of(a) ? 1 : 0, c->top_rarity, c->has_equipment ? 1 : 0);
            }
            if (!a) continue;
            repl::ActorShadow held;
            std::vector<repl::ContentBlock> blocks;
            if (!repl::collect_open_blocks(a->actor, held, blocks)) ++wrong_update;
            std::vector<size_t> order(c->loot.size());
            for (size_t k = 0; k < order.size(); ++k) order[k] = checked % 2 == 0 ? order.size() - 1 - k : k;   // the stacks taken in one order, then in the other
            for (size_t k = 0; k + 1 < order.size(); ++k) {   // every take but the last: the last one empties the loot (TopRarity 7)
                if (!s.after->take(id, order[k])) {
                    ++wrong_take;
                    continue;
                }
                ++steps;
                const LightWanted now = light_wanted(catalog, c->loot);
                if (top_rarity_of(*a) != now.top || has_equipment_of(a) != now.equipment || c->top_rarity != now.top || c->has_equipment != now.equipment) {
                    ++wrong_take;
                    std::printf("  corpse %zu after take %zu: wanted TopRarity %d equipment %d, the component says %lld, %d, its record %d, %d\n", corpses, k + 1,
                                now.top, now.equipment ? 1 : 0, static_cast<long long>(top_rarity_of(*a)), has_equipment_of(a) ? 1 : 0, c->top_rarity, c->has_equipment ? 1 : 0);
                }
                repl::ObjectState sent;
                const bool got = loot_update_of(*a, held, sent);
                const bool changed = !(now == want);
                if (now.top == want.top && now.equipment != want.equipment) ++flag_only;
                bool right = got == changed;
                if (got && changed) {
                    right = (now.top == want.top ? !has_value(sent, "CosmeticData.TopRarity") : number_of(sent, "CosmeticData.TopRarity") == static_cast<uint64_t>(now.top)) &&
                            (now.equipment == want.equipment ? !has_value(sent, "CosmeticData.bHasEquipment")
                                                              : has_value(sent, "CosmeticData.bHasEquipment") && bool_of(sent, "CosmeticData.bHasEquipment", false) == now.equipment);
                }
                if (!right) {
                    ++wrong_update;
                    std::printf("  corpse %zu after take %zu: the connection's update is %s while the light %s\n", corpses, k + 1, got ? "sent" : "not sent", changed ? "changed" : "did not");
                }
                want = now;
            }
            if (s.after->take(id, order.back())) {
                s.after->emptied(id, s.now);
                if (top_rarity_of(*s.w.registry.find(id)) != 7 || !state_guids_of(s.w.registry.find(id)).empty()) ++wrong_take;
            }
        }
    }
    CHECK_OK(corpses >= 12, ("kills of level-18 mobs leave corpses with loot: " + std::to_string(corpses) + " of them were checked").c_str());
    CHECK_OK(mixed >= 1, ("among them corpses with a better stack beside the gear (the owner's case): " + std::to_string(mixed)).c_str());
    CHECK_OK(flag_only >= 1, ("and takes of the last equipment that change the equipment light and not the colour: " + std::to_string(flag_only)).c_str());
    CHECK_OK(wrong_death == 0, ("every corpse's light at the death is the equipment rule's: " + std::to_string(wrong_death) + " differ").c_str());
    CHECK_OK(steps >= 20 && wrong_take == 0, ("after each of the " + std::to_string(steps) + " takes the light is the rule's again: " + std::to_string(wrong_take) + " differ").c_str());
    CHECK_OK(wrong_update == 0, ("a connection that holds the corpse is sent what changed, and nothing when nothing did: " + std::to_string(wrong_update) + " differ").c_str());
}
