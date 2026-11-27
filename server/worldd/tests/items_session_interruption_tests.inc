// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/items_session_interruption_tests.inc: a meal or a drink interrupted by a move, a hit or another ability
//
//  The eating ability's own branch ("Consumable_Food_CancelOnMoveOrDamage") fires when the owner moves or takes a Health loss that is no reaction's and no
//  effect's, and its AnimCancel lets any ability cancel the eating animation.  A seated eater stands up (the montage jumps to its end section, no ClientEndAbility).
//  Before the use's item is taken (the item and the cost step come together, 65 ms after the press) there is nothing to stand up from: the use is cancelled and the
//  item stays in the bag.  Registered with AOC_REGISTER_TEST; included from items_session_registered.inc after the consumables' checks, whose helpers it uses.
//  Ported from war-drayv's pull request #21: the lines they wrote end in "#21 (war-drayv)".
// ============================================================================

namespace gc {
inline constexpr int64_t kMilkCow = 6064630417662476437ll, kMilkCowAbility = 6064632587302273320ll;   // Consumable_Drink_Milk_Cow
}  // namespace gc

/// An ability the server does not run: a press it refuses.
constexpr int64_t kNotRun = 4242424242ll;

/// A level-25 character with `quantity` of `item` in bag slot 4.
static svc::Character stack_of(int64_t item, uint32_t guid, int32_t quantity) {
    svc::Character rec = cyb_record();
    rec.level = 25;
    std::vector<svc::InventoryItem> items;
    for (const auto& it : rec.items)
        if (it.container == Worn) items.push_back(it);
    items.push_back(make_item(Bag, 4, item, guid, quantity));
    rec.items = items;
    return rec;
}

/// How many of the stack in bag slot 4 are left (0: the slot is empty).
static int stack_in_slot(const Harness& h) {
    const svc::InventoryItem* it = h.s->inventory().at(Bag, 4);
    return it ? static_cast<int>(it->quantity) : 0;
}

/// The client's own move that stands still: where the last walk left the pawn, with no input.
static BunchWriter stop_bunch(const Walker& w) {
    repl::NetworkMove m;
    m.time_stamp = w.ts;
    m.acceleration = repl::quantize_vector(10, 0, 0, 0);
    m.location = repl::quantize_vector(100, w.x, bs::kDefaultSpawn.y, bs::kDefaultSpawn.z);
    m.control_rotation.yaw = static_cast<uint16_t>(std::llround(bs::kDefaultSpawn.yaw_degrees / 360.0 * 65536.0) & 0xffff);
    m.movement_mode = repl::kMoveWalking;
    m.input_bits = 0;
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

/// The walk ends: one move with no input (the character is no longer moving, so a later press is not a move's).
static void stop(Harness& h, Walker& w) {
    w.ts += 0.1f;
    h.client_sends({h.client.actor(h.pawn_ch, stop_bunch(w), /*reliable=*/false)});
}

/// Nothing of a meal is on the character: no Resting, no buff, no Nourishment.
static bool no_meal_effects(const Harness& h) {
    return effect_of(h, gc::kRestingEffect) == nullptr && effect_of(h, gc::kAnglersSmokedDelightEffect) == nullptr && effect_of(h, gc::kFreshSaladNourishment) == nullptr;
}

/// The three ways a blow reaches the character: the session's own damage (a GM's kill), another player's hit as the combat world hands it over, and the combat side's inbox
/// (a creature's blow).  Every one has the same default: a loss of 100 Health from an ability.
enum class Blow { Damage, PlayerHit, CreatureHit };

static const char* blow_name(Blow b) {
    switch (b) {
        case Blow::Damage: return "the session's own damage";
        case Blow::PlayerHit: return "another player's hit";
        case Blow::CreatureHit: return "a creature's blow";
    }
    return "?";
}

static void hurt(Harness& h, Blow how, int64_t source = content::kSourceTypeAbility, int64_t stat = content::kGuidHealth, double value = -100.0, uint8_t flags = 0) {
    switch (how) {
        case Blow::Damage:
            h.s->take_damage(h.conn, 100.0, h.t);
            break;
        case Blow::PlayerHit: {
            combat::PlayerIncomingHit blow;   // #21 (war-drayv)
            blow.source_type = source;   // #21 (war-drayv)
            blow.flags = flags;
            blow.mods.push_back({0, stat, value});   // #21 (war-drayv)
            blow.at = world_time_at(h.t);   // #21 (war-drayv)
            std::vector<double> effective;
            h.s->receive_player_hit(blow, effective);   // #21 (war-drayv)
            break;
        }
        case Blow::CreatureHit: {
            combat::IncomingHit blow;   // #21 (war-drayv)
            blow.source_type = source;   // #21 (war-drayv)
            blow.flags = flags;
            blow.mods.push_back({0, stat, value});   // #21 (war-drayv)
            h.s->take_hit(h.conn, blow, h.t);   // #21 (war-drayv)
            break;
        }
    }
}

/// A meal eaten to the end of its bar with the buff on, and then 5.1 s more: the character is seated.  Returns the use's ServerInstanceId.
static int64_t seat_with_buff(Harness& h, uint32_t guid, int32_t id) {
    if (!use_item(h, 4, guid, gc::kAnglersSmokedDelightAbility, id)) return -1;
    run_steps(h, 347);   // 6.94 s
    ask_hit(h, gc::kAnglersSmokedDelightAbility, gc::kAnglersSmokedDelightHit);
    run_steps(h, 256);   // 5.1 s: seated
    return h.s->pipeline() ? h.s->pipeline()->instance_of(id) : -1;
}

/// The test the port started from: a move before the cost step keeps the item.
void test_interrupt_move_before_the_cost_step_keeps_the_item() {
    const char* path = professions_db_path();
    if (!path) {
        std::printf("Interruption: a move before the cost step: skipped (no real content.db)\n");
        return;
    }
    std::printf("Interruption: a move before the item is taken (the first 65 ms of a meal) cancels it and keeps the item\n");
    Harness h(stack_of(gc::kAnglersSmokedDelight, 1100, 2), true, true, path);
    if (!use_item(h, 4, 1100, gc::kAnglersSmokedDelightAbility, 1101)) return;
    run_steps(h, 2);   // 40 ms
    CHECK(h.s->item_reservations() == 1 && stack_in_slot(h) == 2, "(40 ms after the press the item is reserved and still in the bag)");
    const size_t ends = h.s->ability_ends_sent();
    Walker w;
    walk(h, w);
    run_steps(h, 2);
    CHECK(h.s->moves_refused() == 0, "(the walk was taken)");
    CHECK(stack_in_slot(h) == 2 && h.s->uses_applied() == 0, "the move cancels the meal: the item stays in the bag and the stack does not change");
    CHECK(h.s->item_reservations() == 0, "... and nothing stays reserved");
    CHECK(h.s->pipeline() && h.s->pipeline()->instance_of(1101) == -1, "... and the use is gone: it never got its item");
    CHECK(h.s->ability_ends_sent() == ends + 1, "... the client is told its use is over, once");
    run_steps(h, 350);   // up to 7 s after the press: the time the apply hit would come at
    const size_t added = effects_added(h);
    ask_hit(h, gc::kAnglersSmokedDelightAbility, gc::kAnglersSmokedDelightHit);
    run_steps(h, 15);
    CHECK(no_meal_effects(h) && effects_added(h) == added, "no Resting, buff or Nourishment comes, not even from the hit request that arrives later");
    CHECK(stack_in_slot(h) == 2 && h.s->uses_applied() == 0 && !h.s->failed() && h.mirror.unreadable == 0, "(the bag is as it was, and nothing failed)");
    // No cooldown was started: the same meal is eaten at once, and it is spent once
    stop(h, w);
    run_steps(h, 2);
    if (!use_item(h, 4, 1100, gc::kAnglersSmokedDelightAbility, 1102)) return;
    run_steps(h, 150);   // 3 s
    CHECK(stack_in_slot(h) == 1 && h.s->uses_applied() == 1 && h.s->item_reservations() == 0, "the meal pressed again is taken: one of the stack is spent, once");
    CHECK(effect_of(h, gc::kRestingEffect) != nullptr, "... and the character sits and rests: the cancelled use started no cooldown and left nothing behind");
}
AOC_REGISTER_TEST(test_interrupt_move_before_the_cost_step_keeps_the_item);

/// The edge between the two: the item and the cost step come 65 ms after the press.  A move before them cancels (the item is kept), a move after them is a stand-up (the item is
/// spent); never both, never neither.
void test_interrupt_move_and_the_cost_step() {
    const char* path = professions_db_path();
    if (!path) {
        std::printf("Interruption: a move and the cost step: skipped (no real content.db)\n");
        return;
    }
    std::printf("Interruption: a move before the cost step keeps the item, a move after it is a stand-up: the item is kept or taken, never both and never lost\n");
    for (int steps = 1; steps <= 5; ++steps) {   // 20 ms .. 100 ms after the press
        const bool before = steps <= 3;
        Harness h(stack_of(gc::kAnglersSmokedDelight, static_cast<uint32_t>(1110 + steps), 2), true, true, path);
        const int32_t id = 1120 + steps;
        if (!use_item(h, 4, static_cast<uint32_t>(1110 + steps), gc::kAnglersSmokedDelightAbility, id)) return;
        run_steps(h, steps);
        const int64_t server = h.s->pipeline() ? h.s->pipeline()->instance_of(id) : -1;
        const size_t ends = h.s->ability_ends_sent();
        Walker w;
        walk(h, w);
        run_steps(h, 5);
        char label[200];
        std::snprintf(label, sizeof label, "a move %d ms after the press: %s", steps * 20, before ? "the item is kept, none is spent" : "the item is taken, one is spent, once");
        CHECK(stack_in_slot(h) == (before ? 2 : 1) && h.s->uses_applied() == (before ? 0u : 1u) && h.s->item_reservations() == 0, label);
        if (before) {
            CHECK(server == -1 && h.s->ability_ends_sent() == ends + 1, "... the use never got its item, and the client is told it is over");
        } else {
            const ActiveItem jumped = active_item(h, server);
            CHECK(server >= 0 && jumped.jump && jumped.position == kEatingEnd && h.s->ability_ends_sent() == ends, "... and the character stands up: the montage jumps to its end, no ClientEndAbility");
        }
        run_steps(h, 150);   // 3 s more
        CHECK(no_meal_effects(h) && stack_in_slot(h) == (before ? 2 : 1), "... and nothing of the meal comes later (no Resting: the walk came before its notify)");
    }
}
AOC_REGISTER_TEST(test_interrupt_move_and_the_cost_step);

/// A drink is a meal for the rule, a potion is not.
void test_interrupt_drink_and_potion() {
    const char* path = professions_db_path();
    if (!path) {
        std::printf("Interruption: a drink and a potion: skipped (no real content.db)\n");
        return;
    }
    std::printf("Interruption: a drink is cancelled by a move in its first moment, a potion is never cancelled\n");
    {
        Harness h(stack_of(gc::kMilkCow, 1140, 1), true, true, path);
        if (!use_item(h, 4, 1140, gc::kMilkCowAbility, 1141)) return;
        run_steps(h, 2);
        CHECK(h.s->item_reservations() == 1 && stack_in_slot(h) == 1, "(the drink is reserved and still in the bag)");
        Walker w;
        walk(h, w);
        run_steps(h, 150);
        CHECK(stack_in_slot(h) == 1 && h.s->uses_applied() == 0 && h.s->item_reservations() == 0, "a move before the item is taken cancels a drink too: the item is kept");
        CHECK(effect_of(h, gc::kRestingEffect) == nullptr && h.s->pipeline() && h.s->pipeline()->instance_of(1141) == -1, "... and the character does not sit down");
    }
    {
        const float expected = potion_heal(path, 1);
        svc::Character rec = stack_of(gc::kHealthPotion, 1150, 1);
        Harness h(rec, true, true, path);
        const sg::CharacterStats* cs = h.s->character_stats();
        if (!cs || !h.s->set_current_value(gc::kHealthStat, 1.0f)) return;
        const float before = cs->find(gc::kHealthStat)->value;
        if (!use_item(h, 4, 1150, gc::kHealthPotionAbility, 1151)) return;
        ask_hit(h, gc::kHealthPotionAbility, gc::kHealthPotionHit);
        run_steps(h, 2);   // 40 ms: before the cost step
        Walker w;
        walk(h, w);
        run_steps(h, 30);
        CHECK(h.s->moves_refused() == 0 && expected > 0.0f, "(the walk was taken, and a potion heals)");
        CHECK(cs->find(gc::kHealthStat)->value - before == expected && h.s->uses_applied() == 1, "a move does not cancel a potion: its heal lands in full");
        CHECK(stack_in_slot(h) == 0, "... and the potion is spent");
    }
}
AOC_REGISTER_TEST(test_interrupt_drink_and_potion);

/// Damage in the first moment: through each of the three ways a blow reaches the character, the meal is cancelled and the item is kept.
void test_interrupt_damage_before_the_cost_step() {
    const char* path = professions_db_path();
    if (!path) {
        std::printf("Interruption: damage before the cost step: skipped (no real content.db)\n");
        return;
    }
    std::printf("Interruption: damage before the item is taken cancels the meal and keeps the item\n");
    uint32_t n = 0;
    for (const Blow how : {Blow::Damage, Blow::PlayerHit, Blow::CreatureHit}) {
        ++n;
        Harness h(stack_of(gc::kAnglersSmokedDelight, 1160 + n, 2), true, true, path);
        const int32_t id = static_cast<int32_t>(1170 + n);
        if (!use_item(h, 4, 1160 + n, gc::kAnglersSmokedDelightAbility, id)) return;
        run_steps(h, 2);   // 40 ms
        const size_t ends = h.s->ability_ends_sent();
        hurt(h, how);
        run_steps(h, 3);
        char label[200];
        std::snprintf(label, sizeof label, "%s in the first moment cancels the meal: the item is kept and nothing is reserved", blow_name(how));
        CHECK(stack_in_slot(h) == 2 && h.s->uses_applied() == 0 && h.s->item_reservations() == 0, label);
        CHECK(h.s->pipeline() && h.s->pipeline()->instance_of(id) == -1 && h.s->ability_ends_sent() == ends + 1, "... the use is gone and the client is told so, once");
        run_steps(h, 350);
        const size_t added = effects_added(h);
        ask_hit(h, gc::kAnglersSmokedDelightAbility, gc::kAnglersSmokedDelightHit);
        run_steps(h, 15);
        CHECK(no_meal_effects(h) && effects_added(h) == added && stack_in_slot(h) == 2, "... and nothing of the meal comes later");
        CHECK(!h.s->failed() && h.mirror.unreadable == 0, "(nothing failed)");
    }
}
AOC_REGISTER_TEST(test_interrupt_damage_before_the_cost_step);

/// Damage while seated, with no combat starting: the character stands up as a walk makes them stand up.
void test_interrupt_damage_while_seated() {
    const char* path = professions_db_path();
    if (!path) {
        std::printf("Interruption: damage while seated: skipped (no real content.db)\n");
        return;
    }
    std::printf("Interruption: a seated eater who takes a Health loss stands up (the montage jumps to its end, Resting goes 0.067 s later, no ClientEndAbility)\n");
    uint32_t n = 0;
    for (const Blow how : {Blow::Damage, Blow::PlayerHit, Blow::CreatureHit}) {
        ++n;
        Harness h(bag_of(gc::kAnglersSmokedDelight, 1180 + n), true, true, path);
        const int32_t id = static_cast<int32_t>(1190 + n);
        const int64_t server = seat_with_buff(h, 1180 + n, id);
        CHECK(server >= 0 && effect_of(h, gc::kRestingEffect) != nullptr && effect_of(h, gc::kAnglersSmokedDelightEffect) != nullptr, "(seated, with the buff on)");
        const ActiveItem seated = active_item(h, server);
        const size_t ends = h.s->ability_ends_sent();
        hurt(h, how);
        if (how == Blow::PlayerHit) {
            CHECK(!active_item(h, server).jump, "(another player's hit only queues the stand-up: this runtime's steps do not run inside it)");
        }
        h.step();
        const ActiveItem jumped = active_item(h, server);
        char label[200];
        std::snprintf(label, sizeof label, "%s stands the seated eater up: the eating montage jumps to its end section", blow_name(how));
        CHECK(jumped.jump && jumped.position == kEatingEnd && jumped.counter == seated.counter + 1 && !jumped.predicted, label);
        run_steps(h, 5);   // past the 0.067 s Resting goes after
        CHECK(effect_of(h, gc::kRestingEffect) == nullptr && h.s->pipeline() && h.s->pipeline()->instance_of(id) == -1, "... Resting is gone and the use is over");
        CHECK(h.s->ability_ends_sent() == ends, "... and no ClientEndAbility was sent");
        CHECK(effect_count_of(h, gc::kAnglersSmokedDelightEffect) == 1 && effect_of(h, gc::kFreshSaladNourishment) == nullptr, "the buff the hit gave stays, one of it");
        run_steps(h, kMontageStillThere - 6);
        CHECK(active_item(h, server).montage != 0, "the montage is still on the item until the jump has played out");
        run_steps(h, 2);
        CHECK(active_item(h, server).present && active_item(h, server).montage == 0, "... and then the item loses it and stays in the list, as after a walk");
        CHECK(h.s->inventory().at(Bag, 4) == nullptr && h.s->uses_applied() == 1 && h.s->item_reservations() == 0, "the meal stays spent, once");
    }
}
AOC_REGISTER_TEST(test_interrupt_damage_while_seated);

/// What the rule leaves out: a hit that is an effect's tick, a reaction's, a heal, a miss or a loss of something else is no damage to it.
void test_interrupt_what_is_not_damage() {
    const char* path = professions_db_path();
    if (!path) {
        std::printf("Interruption: what is not damage: skipped (no real content.db)\n");
        return;
    }
    std::printf("Interruption: an effect's tick, a reaction's hit, a heal, a miss and a loss of Mana leave a seated eater seated\n");
    uint32_t n = 0;
    for (const Blow how : {Blow::PlayerHit, Blow::CreatureHit}) {
        ++n;
        Harness h(bag_of(gc::kAnglersSmokedDelight, 1200 + n), true, true, path);
        const int32_t id = static_cast<int32_t>(1210 + n);
        if (!use_item(h, 4, 1200 + n, gc::kAnglersSmokedDelightAbility, id)) return;
        run_steps(h, 150);   // 3 s: seated, the bar is not full
        const int64_t server = h.s->pipeline() ? h.s->pipeline()->instance_of(id) : -1;
        CHECK(server >= 0 && effect_of(h, gc::kRestingEffect) != nullptr, "(seated, Resting is on)");
        const ActiveItem seated = active_item(h, server);
        const size_t ends = h.s->ability_ends_sent();
        struct Variant {
            const char* what;
            int64_t     source;
            int64_t     stat;
            double      value;
            uint8_t     flags;
        };
        const Variant variants[] = {
            {"an effect's tick", content::kSourceTypeEffect, content::kGuidHealth, -10.0, 0},
            {"a reaction's hit", content::kSourceTypeReaction, content::kGuidHealth, -10.0, 0},
            {"a heal", content::kSourceTypeAbility, content::kGuidHealth, 50.0, 0},
            {"a miss", content::kSourceTypeAbility, content::kGuidHealth, -100.0, sg::kHitMiss},
            {"a loss of Mana", content::kSourceTypeAbility, content::kGuidMana, -50.0, 0},
        };
        for (const Variant& v : variants) {
            hurt(h, how, v.source, v.stat, v.value, v.flags);
            run_steps(h, 3);
            const ActiveItem now = active_item(h, server);
            char label[200];
            std::snprintf(label, sizeof label, "%s (%s) does not stand the eater up: still seated, the montage not told to jump", v.what, blow_name(how));
            CHECK(effect_of(h, gc::kRestingEffect) != nullptr && h.s->pipeline() && h.s->pipeline()->instance_of(id) == server && !now.jump && now.counter == seated.counter && h.s->ability_ends_sent() == ends,
                  label);
        }
        hurt(h, how);   // and a real loss does
        run_steps(h, 5);   // (Resting goes 0.067 s after the jump)
        CHECK(active_item(h, server).jump && effect_of(h, gc::kRestingEffect) == nullptr, "(a loss of Health from an ability does stand them up)");
    }
}
AOC_REGISTER_TEST(test_interrupt_what_is_not_damage);

/// A press the server refuses leaves the eater as they are; another ability the server takes stands them up, and in the first moment it cancels the meal.
void test_interrupt_presses() {
    const char* path = professions_db_path();
    if (!path) {
        std::printf("Interruption: presses: skipped (no real content.db)\n");
        return;
    }
    std::printf("Interruption: a press the server refuses changes nothing; an ability it takes stands the eater up, or cancels the meal in its first moment\n");
    // seated: a refused press leaves the eater seated, with no change to the montage
    {
        Harness h(bag_of(gc::kAnglersSmokedDelight, 1220), true, true, path);
        const bs::PlayerSetup& setup = h.s->bootstrap().setup();
        if (!use_item(h, 4, 1220, gc::kAnglersSmokedDelightAbility, 1221)) return;
        run_steps(h, 150);   // 3 s
        const int64_t server = h.s->pipeline() ? h.s->pipeline()->instance_of(1221) : -1;
        CHECK(server >= 0 && effect_of(h, gc::kRestingEffect) != nullptr, "(seated, Resting is on)");
        const ActiveItem seated = active_item(h, server);
        const size_t ends = h.s->ability_ends_sent();
        h.client_sends({h.client.actor(h.pawn_ch, ability_trigger(setup.guids.ability, kNotRun, 1222, 0), true)});   // a press the server refuses   // #21 (war-drayv)
        run_steps(h, 10);
        const ActiveItem after = active_item(h, server);
        CHECK(h.s->ability_ends_sent() == ends + 1, "(the refused press is answered with its own ClientEndAbility, and nothing else is sent)");
        CHECK(effect_of(h, gc::kRestingEffect) != nullptr && h.s->pipeline() && h.s->pipeline()->instance_of(1221) == server, "a press the server refuses leaves the eater seated: Resting stays and the use goes on");
        CHECK(!after.jump && after.counter == seated.counter && after.montage == seated.montage && after.position == seated.position, "... and the montage is not changed");
        run_steps(h, 200);   // up to 7 s after the press
        ask_hit(h, gc::kAnglersSmokedDelightAbility, gc::kAnglersSmokedDelightHit);
        run_steps(h, 6);
        CHECK(effect_of(h, gc::kAnglersSmokedDelightEffect) != nullptr, "... and the apply hit still gives the buff: the meal was eaten to its end");
    }
    // the first moment: a refused press does not cancel the meal either
    {
        Harness h(stack_of(gc::kAnglersSmokedDelight, 1223, 2), true, true, path);
        const bs::PlayerSetup& setup = h.s->bootstrap().setup();
        if (!use_item(h, 4, 1223, gc::kAnglersSmokedDelightAbility, 1224)) return;
        run_steps(h, 2);
        h.client_sends({h.client.actor(h.pawn_ch, ability_trigger(setup.guids.ability, kNotRun, 1225, 0), true)});   // #21 (war-drayv)
        run_steps(h, 150);
        CHECK(stack_in_slot(h) == 1 && h.s->uses_applied() == 1 && h.s->item_reservations() == 0, "a press the server refuses in the first moment does not cancel the meal: its item is taken at its time");
        CHECK(effect_of(h, gc::kRestingEffect) != nullptr, "... and the character sits and rests");
    }
    // another ability the server takes: a Dodge, and an ordinary ability of the Bard's
    for (int which = 0; which < 2; ++which) {
        const bool dodge = which == 0;
        // in the first moment: the meal is cancelled, the item is kept, and the ability goes on
        {
            Harness h(stack_of(gc::kAnglersSmokedDelight, static_cast<uint32_t>(1230 + which), 2), true, true, path);
            const bs::PlayerSetup& setup = h.s->bootstrap().setup();
            if (!use_item(h, 4, static_cast<uint32_t>(1230 + which), gc::kAnglersSmokedDelightAbility, 1240 + which)) return;
            run_steps(h, 2);
            if (!dodge) {
                CHECK(h.s->pipeline() && h.s->pipeline()->known(kFlourishAbility), "(the pipeline knows Flourish)");
            }
            const size_t ends = h.s->ability_ends_sent();
            h.client_sends({h.client.actor(h.pawn_ch, ability_trigger(setup.guids.ability, dodge ? content::kGuidDodge : kFlourishAbility, 1250 + which, 0), true)});
            run_steps(h, 3);
            CHECK(stack_in_slot(h) == 2 && h.s->uses_applied() == 0 && h.s->item_reservations() == 0,
                  dodge ? "a Dodge in the first moment cancels the meal: the item is kept" : "another ability in the first moment cancels the meal: the item is kept");
            CHECK(h.s->pipeline() && h.s->pipeline()->instance_of(1240 + which) == -1 && h.s->ability_ends_sent() == ends + 1, "... the use is gone and its client is told so, once");
            run_steps(h, 350);
            CHECK(no_meal_effects(h) && stack_in_slot(h) == 2, "... and nothing of the meal comes later");
        }
        // after the cost step (3 s into the bar): the seated eater stands up, as before (the consumables' own checks cover it in full)
        {
            Harness h(bag_of(gc::kAnglersSmokedDelight, static_cast<uint32_t>(1260 + which)), true, true, path);
            const bs::PlayerSetup& setup = h.s->bootstrap().setup();
            if (!use_item(h, 4, static_cast<uint32_t>(1260 + which), gc::kAnglersSmokedDelightAbility, 1270 + which)) return;
            run_steps(h, 150);
            const int64_t server = h.s->pipeline() ? h.s->pipeline()->instance_of(1270 + which) : -1;
            const size_t ends = h.s->ability_ends_sent();
            h.client_sends({h.client.actor(h.pawn_ch, ability_trigger(setup.guids.ability, dodge ? content::kGuidDodge : kFlourishAbility, 1280 + which, 0), true)});
            h.step();
            CHECK(server >= 0 && active_item(h, server).jump && active_item(h, server).position == kEatingEnd && h.s->ability_ends_sent() == ends,
                  dodge ? "a Dodge while seated: the montage jumps, no ClientEndAbility (as before)" : "another ability while seated: the montage jumps, no ClientEndAbility (as before)");
        }
    }
}
AOC_REGISTER_TEST(test_interrupt_presses);

/// Two presses of one stack of two within the first moment: a move cancels both, and the bag is as it was; a press again afterwards spends one, once.
void test_interrupt_double_press() {
    const char* path = professions_db_path();
    if (!path) {
        std::printf("Interruption: a double press: skipped (no real content.db)\n");
        return;
    }
    std::printf("Interruption: two presses of one stack, then a move: both are cancelled, the stack is as it was, and a press afterwards spends one\n");
    {
        Harness h(stack_of(gc::kAnglersSmokedDelight, 1300, 2), true, true, path);
        if (!use_item(h, 4, 1300, gc::kAnglersSmokedDelightAbility, 1301)) return;
        h.step();
        if (!use_item(h, 4, 1300, gc::kAnglersSmokedDelightAbility, 1302)) return;
        CHECK(h.s->item_reservations() <= 2, "(at most one reservation for each press)");
        const size_t ends = h.s->ability_ends_sent();
        Walker w;
        walk(h, w);
        run_steps(h, 5);
        CHECK(stack_in_slot(h) == 2 && h.s->uses_applied() == 0 && h.s->item_reservations() == 0, "the move cancels every press that had not taken its item: the stack is as it was");
        CHECK(h.s->pipeline() && h.s->pipeline()->instance_of(1301) == -1 && h.s->pipeline()->instance_of(1302) == -1 && h.s->ability_ends_sent() >= ends + 1, "... and no use is left");
        run_steps(h, 10);
        stop(h, w);
        run_steps(h, 2);
        if (!use_item(h, 4, 1300, gc::kAnglersSmokedDelightAbility, 1303)) return;
        run_steps(h, 150);
        CHECK(stack_in_slot(h) == 1 && h.s->uses_applied() == 1 && h.s->item_reservations() == 0, "a press afterwards spends one of the two, once");
        CHECK(effect_of(h, gc::kRestingEffect) != nullptr && !h.s->failed() && h.mirror.unreadable == 0, "... and the character eats it");
    }
    // a stack of one: a second press while the first has not paid is refused, and a move then leaves the one item in the bag
    {
        Harness h(stack_of(gc::kAnglersSmokedDelight, 1304, 1), true, true, path);
        if (!use_item(h, 4, 1304, gc::kAnglersSmokedDelightAbility, 1305)) return;
        h.step();
        if (!use_item(h, 4, 1304, gc::kAnglersSmokedDelightAbility, 1306)) return;   // (the one item is reserved: this press has nothing to take)
        Walker w;
        walk(h, w);
        run_steps(h, 5);
        CHECK(stack_in_slot(h) == 1 && h.s->uses_applied() == 0 && h.s->item_reservations() == 0, "a stack of one pressed twice and then a move: the one item is still in the bag, and nothing is reserved");
        run_steps(h, 10);
        stop(h, w);
        run_steps(h, 2);
        if (!use_item(h, 4, 1304, gc::kAnglersSmokedDelightAbility, 1307)) return;
        run_steps(h, 150);
        CHECK(stack_in_slot(h) == 0 && h.s->uses_applied() == 1, "... and a press afterwards eats it: the item is spent once, not twice, not lost");
    }
}
AOC_REGISTER_TEST(test_interrupt_double_press);

/// A logout or a relog in the middle of a use: the bag the session saves is right, and the next session spends the item once.
void test_interrupt_relog() {
    const char* path = professions_db_path();
    if (!path) {
        std::printf("Interruption: a relog: skipped (no real content.db)\n");
        return;
    }
    std::printf("Interruption: a logout in the first moment, after a cancel and after a stand-up: the saved bag is right and no item is spent twice or lost\n");
    // a logout before the item is taken: the item is in the saved bag, and the next session eats it once
    {
        const svc::Character rec = stack_of(gc::kAnglersSmokedDelight, 1310, 2);
        Harness h(rec, true, true, path);
        if (!use_item(h, 4, 1310, gc::kAnglersSmokedDelightAbility, 1311)) return;
        run_steps(h, 2);
        svc::Character saved = rec;
        saved.items = h.s->inventory().items();
        Harness g(saved, true, true, path);
        CHECK(g.s->inventory().at(Bag, 4) && g.s->inventory().at(Bag, 4)->quantity == 2, "a logout before the item is taken: the saved bag holds the whole stack");
        CHECK(g.s->item_reservations() == 0 && g.s->pipeline() && g.s->pipeline()->instance_of(1311) == -1, "... and the new session has no use running and nothing reserved");
        if (!use_item(g, 4, 1310, gc::kAnglersSmokedDelightAbility, 1312)) return;
        run_steps(g, 150);
        CHECK(stack_in_slot(g) == 1 && g.s->uses_applied() == 1, "... and it eats one of the two, once");
    }
    // a move cancels, then a logout: the saved bag holds the whole stack
    {
        const svc::Character rec = stack_of(gc::kAnglersSmokedDelight, 1313, 2);
        Harness h(rec, true, true, path);
        if (!use_item(h, 4, 1313, gc::kAnglersSmokedDelightAbility, 1314)) return;
        run_steps(h, 2);
        Walker w;
        walk(h, w);
        run_steps(h, 5);
        svc::Character saved = rec;
        saved.items = h.s->inventory().items();
        Harness g(saved, true, true, path);
        CHECK(g.s->inventory().at(Bag, 4) && g.s->inventory().at(Bag, 4)->quantity == 2 && h.s->uses_applied() == 0, "a logout after a cancel: the saved bag holds the whole stack");
        if (!use_item(g, 4, 1313, gc::kAnglersSmokedDelightAbility, 1315)) return;
        run_steps(g, 150);
        CHECK(stack_in_slot(g) == 1 && g.s->uses_applied() == 1, "... and the next session eats one of the two, once");
    }
    // eaten, then a walk (a stand-up), then a logout: one is spent, and nothing of the seat or the buff comes back
    {
        const svc::Character rec = stack_of(gc::kAnglersSmokedDelight, 1316, 2);
        Harness h(rec, true, true, path);
        if (!use_item(h, 4, 1316, gc::kAnglersSmokedDelightAbility, 1317)) return;
        run_steps(h, 150);
        Walker w;
        walk(h, w);
        run_steps(h, 10);
        svc::Character saved = rec;
        saved.items = h.s->inventory().items();
        CHECK(h.s->uses_applied() == 1 && stack_in_slot(h) == 1, "(one of the two was spent before the walk)");
        Harness g(saved, true, true, path);
        CHECK(g.s->inventory().at(Bag, 4) && g.s->inventory().at(Bag, 4)->quantity == 1, "a logout after a stand-up: the saved bag holds the one that is left");
        CHECK(no_meal_effects(g) && g.s->pipeline() && g.s->pipeline()->instance_of(1317) == -1, "... and nothing of the seat or the buff comes back");
        Walker x;
        walk(g, x);
        run_steps(g, 10);
        CHECK(!g.s->failed() && g.mirror.unreadable == 0, "(a walk in the new session is nothing to stand up from)");
    }
}
AOC_REGISTER_TEST(test_interrupt_relog);
