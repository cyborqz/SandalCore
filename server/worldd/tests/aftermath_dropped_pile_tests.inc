// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/aftermath_dropped_pile_tests.inc: the blocks of a full bag's pile as the retail full-bag open has them (included by test_worldd_aftermath.cpp)
//
//  Beside the pile's class (test_dropped_pile_look): the interaction of its first input is the data's FoliageLootPileInteraction (no channel), not the
//  dead player's PlayerLootPileInteraction (a 3 s channel); its loot is not public and is offered to the player whose bag it fell from alone; the actor
//  says its roles and sends no bHasLoot, as the retail open does.
// ============================================================================

namespace {

constexpr int64_t kFoliageLootPileInteraction = 6064632235501551616ll;   // PlayerInteractionDef FoliageLootPileInteraction: the interaction of the retail full-bag pile's input 0
constexpr int64_t kPlayerLootPileInteraction = 6064628934801555456ll;    // PlayerInteractionDef PlayerLootPileInteraction: the dead player's pile's (a 3 s channel)

const repl::Subobject* sub_of(const world::WorldActor* wa, const char* cls) {
    if (!wa) return nullptr;
    for (const repl::Subobject& s : wa->actor.subobjects) {
        if (s.obj.schema && std::string(s.obj.schema->name) == cls) return &s;
    }
    return nullptr;
}

/// InteractionInputData[0].PlayerInteractDefinition.Guid of the pile's interactable (0: none).
int64_t input_def_of(const world::WorldActor* wa) {
    const repl::Subobject* i = sub_of(wa, "UInteractableComponent");
    const repl::Value* v = i ? i->obj.state.get(*i->obj.schema, "InteractionInputData") : nullptr;
    const auto* arr = v ? v->get_if<repl::ArrayValue>() : nullptr;
    if (!arr || arr->elements.empty()) return 0;
    const auto it = arr->elements[0].values.find(6);   // InteractionInputData.PlayerInteractDefinition.Guid
    const int64_t* g = it != arr->elements[0].values.end() ? it->second.get_if<int64_t>() : nullptr;
    return g ? *g : 0;
}

/// The players the first input's state names (StateGuids), as the first word of each guid.
std::vector<int64_t> state_guids_of(const world::WorldActor* wa) {
    std::vector<int64_t> out;
    const repl::Subobject* i = sub_of(wa, "UInteractableComponent");
    const repl::Value* v = i ? i->obj.state.get(*i->obj.schema, "CurrentInputStates") : nullptr;
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

bool bool_of(const repl::ObjectState& o, const char* path, bool absent) {
    const repl::Value* v = o.schema ? o.state.get(*o.schema, path) : nullptr;
    const bool* b = v ? v->get_if<bool>() : nullptr;
    return b ? *b : absent;
}

bool has_value(const repl::ObjectState& o, const char* path) { return o.schema && o.state.get(*o.schema, path) != nullptr; }

uint64_t number_of(const repl::ObjectState& o, const char* path) {
    const repl::Value* v = o.schema ? o.state.get(*o.schema, path) : nullptr;
    const uint64_t* n = v ? v->get_if<uint64_t>() : nullptr;
    return n ? *n : 0;
}

}  // namespace

void test_dropped_pile_blocks(Fixture& f) {
    std::printf("a full bag's pile as the retail full-bag open has it: the foliage pile's interaction, offered to its owner alone\n");
    Scene s(f, 20);
    CHECK_OK(s.ok(), ("the scene: " + s.rig.problem + " " + s.w.problem).c_str());
    if (!s.ok()) return;
    repl::GuidValue harvester{0x0AAA0001u, 0x0AAA0002u, 0x0AAA0003u, 0x0AAA0004u};
    repl::GuidValue other{0x0BBB0001u, 0x0BBB0002u, 0x0BBB0003u, 0x0BBB0004u};
    combat::LootStack stack;
    stack.item = 6064629184431849472ll;   // Resource_Raw_Plant_Daffodil
    stack.quantity = 3;
    stack.quality = 1;
    std::string error;
    const uint64_t mine = s.after->drop_pile(100, 0, 1000, harvester, "Harvester", {stack}, s.now, error, /*item_drop=*/true);
    const uint64_t theirs = s.after->drop_pile(900, 0, 1000, other, "Other", {stack}, s.now, error, /*item_drop=*/true);
    const uint64_t death = s.after->drop_pile(300, 0, 1000, harvester, "Harvester", {stack}, s.now, error);
    CHECK_OK(mine != 0 && theirs != 0 && death != 0, ("the three piles are made: " + error).c_str());
    if (!mine || !theirs || !death) return;
    const world::WorldActor* m = s.w.registry.find(mine);
    const world::WorldActor* t = s.w.registry.find(theirs);
    const world::WorldActor* d = s.w.registry.find(death);
    CHECK_OK(input_def_of(m) == kFoliageLootPileInteraction, "the drop's interaction is the data's FoliageLootPileInteraction (no channel: the retail full-bag pile's)");
    CHECK_OK(input_def_of(t) == kFoliageLootPileInteraction, "the other player's drop has it too");
    CHECK_OK(input_def_of(d) == kPlayerLootPileInteraction, "a dead player's pile keeps PlayerLootPileInteraction (its 3 s channel)");
    const repl::Subobject* loot = sub_of(m, "ULootComponent");
    const repl::Subobject* death_loot = sub_of(d, "ULootComponent");
    CHECK_OK(loot && !bool_of(loot->obj, "bPublicLoot", false), "the drop's loot is not public: the retail full-bag pile has no bPublicLoot");
    CHECK_OK(death_loot && bool_of(death_loot->obj, "bPublicLoot", false), "a dead player's pile's loot is public");
    // the interaction is offered to the player whose bag it fell from, alone
    const std::vector<int64_t> mine_for = state_guids_of(m), theirs_for = state_guids_of(t);
    CHECK_OK(mine_for.size() == 1 && mine_for[0] == static_cast<int32_t>(harvester.a), "the first player's drop is offered to him alone");
    CHECK_OK(theirs_for.size() == 1 && theirs_for[0] == static_cast<int32_t>(other.a), "the second player's drop is offered to him alone");
    // the actor's own values: the roles the retail open sets, and no bHasLoot (a death pile sets it)
    CHECK_OK(m && number_of(m->actor.obj, "Role") == 4 && number_of(m->actor.obj, "RemoteRole") == 1, "Role 4 and RemoteRole 1, as the retail full-bag open sends them");
    CHECK_OK(m && !has_value(m->actor.obj, "bHasLoot"), "the drop sends no bHasLoot (the retail open sends none)");
    CHECK_OK(d && bool_of(d->actor.obj, "bHasLoot", false), "a death pile still sends bHasLoot 1");
    // emptied: the drop's interaction closes like any pile's, and the other player's drop is untouched
    CHECK_OK(s.after->take(mine, 0) != nullptr, "the first player takes his stack");
    s.after->emptied(mine, s.now);
    CHECK_OK(state_guids_of(s.w.registry.find(mine)).empty() && state_guids_of(s.w.registry.find(theirs)).size() == 1,
             "an emptied drop is offered to nobody; the other player's drop still to its owner");
}
