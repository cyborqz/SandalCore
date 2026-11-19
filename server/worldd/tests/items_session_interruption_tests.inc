// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/items_session_interruption_tests.inc: a food or drink use ends when its player moves, is hit or starts another ability
//  (the data's cancel rule: the use's ability carries "Consumable_Food_CancelOnMoveOrDamage" and cancels into every ability; the other kinds are not
//  cancellable).  Registered with AOC_REGISTER_TEST; included from items_session_registered.inc.
// ============================================================================

/// A walk move from the pawn's current place, as the client sends it: `cm` along x, at the client's stamp `ts`.
static BunchWriter walk_move(const GoodPosition& from, float ts, double cm) {
    repl::NetworkMove m;
    m.time_stamp = ts;
    m.acceleration = repl::quantize_vector(10, cm > 0 ? 100 : 0, 0, 0);
    m.location = repl::quantize_vector(100, from.x + cm, from.y, from.z);
    m.control_rotation.yaw = static_cast<uint16_t>(from.yaw_degrees * 65536.0 / 360.0);
    m.movement_mode = repl::kMoveWalking;
    m.input_bits = cm > 0 ? 1 : 0;
    BunchWriter out;
    const schema::ClassSchema& cls = class_named("PlayerPawn_C");
    const schema::NetField* f = cls.field("ServerMovePacked");
    repl::MovePacked p;
    p.new_move = m;
    BunchWriter params, field;
    if (!f || !repl::write_move_packed(params, p) || repl::write_field(cls, f->index, params, field) == 0) return out;
    repl::BlockHeader header;
    repl::write_content_block(out, header, field, /*server_form=*/false);
    return out;
}

void test_item_use_is_interrupted() {
    const char* path = professions_db_path();
    if (!path) {
        std::printf("Item interruption: skipped (no real content.db)\n");
        return;
    }
    std::printf("A food or drink use is cancelled when its player moves, is hit or starts another ability\n");
    constexpr int64_t kNotRun = 4242424242ll;

    // a move ends a paid use: Resting goes with it, the meal's own buff stays, and the moves are accepted
    {
        svc::Character rec = cyb_record();
        rec.level = 25;
        std::vector<svc::InventoryItem> items;
        for (const auto& it : rec.items)
            if (it.container == Worn) items.push_back(it);
        items.push_back(make_item(Bag, 4, gc::kFreshSalad, 910, 1));
        rec.items = items;
        Harness h(rec, true, true, path);
        const bs::PlayerSetup& setup = h.s->bootstrap().setup();
        const svc::InventoryItem* item = h.s->inventory().at(Bag, 4);
        if (!item) return;
        h.client_sends({h.client.actor(h.pawn_ch, item_used(3, 4, guid_n(910)), true),
                        h.client.actor(h.pawn_ch, trigger_with_item(setup.guids.ability, gc::kFreshSaladAbility, 85, 4, items::to_instance(*item), 3), true)});
        for (int i = 0; i < 110; ++i) h.step();   // 2.2 s: the effects are on (they come when the eating ends)
        CHECK(effect_of(h, gc::kRestingEffect) != nullptr, "(Resting is on while the character eats)");
        CHECK(effect_of(h, gc::kFreshSaladEffect) == nullptr, "(the meal's buff is not on yet: it waits for the end of the eating)");
        const GoodPosition from = h.s->last_position();
        const size_t ends = h.s->ability_ends_sent();
        h.client_sends({h.client.actor(h.pawn_ch, walk_move(from, 100.0f, 10.0), false),
                        h.client.actor(h.pawn_ch, walk_move(from, 100.1f, 20.0), false)});
        for (int i = 0; i < 5; ++i) h.step();
        CHECK(h.s->moves_refused() == 0, "(the moves were accepted)");
        CHECK(effect_of(h, gc::kRestingEffect) == nullptr, "a move ends the use: Resting goes with it");
        CHECK(effect_of(h, gc::kFreshSaladEffect) == nullptr, "... and the meal's buff is not granted: the eating was interrupted, its item spent for nothing");
        CHECK(h.s->inventory().at(Bag, 4) == nullptr && h.s->ability_ends_sent() == ends + 1, "... the item is spent and the client is told the instance is over");
        CHECK(!h.s->failed() && h.mirror.unreadable == 0, "(nothing failed)");
    }

    // a move in the first 65 ms cancels the use outright: nothing is paid, the item is kept and nothing applies later
    {
        svc::Character rec = cyb_record();
        rec.level = 25;
        std::vector<svc::InventoryItem> items;
        for (const auto& it : rec.items)
            if (it.container == Worn) items.push_back(it);
        items.push_back(make_item(Bag, 4, gc::kFreshSalad, 911, 1));
        rec.items = items;
        Harness h(rec, true, true, path);
        const bs::PlayerSetup& setup = h.s->bootstrap().setup();
        const svc::InventoryItem* item = h.s->inventory().at(Bag, 4);
        if (!item) return;
        h.client_sends({h.client.actor(h.pawn_ch, item_used(3, 4, guid_n(911)), true),
                        h.client.actor(h.pawn_ch, trigger_with_item(setup.guids.ability, gc::kFreshSaladAbility, 86, 4, items::to_instance(*item), 3), true)});
        for (int i = 0; i < 2; ++i) h.step();   // 40 ms: before the cost step
        const GoodPosition from = h.s->last_position();
        const size_t ends = h.s->ability_ends_sent();
        h.client_sends({h.client.actor(h.pawn_ch, walk_move(from, 100.0f, 10.0), false),
                        h.client.actor(h.pawn_ch, walk_move(from, 100.1f, 20.0), false)});
        for (int i = 0; i < 110; ++i) h.step();   // past the time the effects would have come
        CHECK(h.s->moves_refused() == 0, "(the moves were accepted)");
        CHECK(h.s->inventory().at(Bag, 4) != nullptr && h.s->inventory().at(Bag, 4)->quantity == 1, "a use cancelled before its cost step keeps the item");
        CHECK(h.s->ability_ends_sent() == ends + 1, "... the client is told the instance is over");
        CHECK(effect_of(h, gc::kRestingEffect) == nullptr && effect_of(h, gc::kFreshSaladEffect) == nullptr, "... and nothing of the meal applies");
        CHECK(!h.s->failed() && h.mirror.unreadable == 0, "(nothing failed)");
    }

    // any ability press ends a paid use — one the server refuses among them (the client plays its press all the same: a weapon combo is refused here and the character still swings)
    {
        svc::Character rec = cyb_record();
        rec.level = 25;
        std::vector<svc::InventoryItem> items;
        for (const auto& it : rec.items)
            if (it.container == Worn) items.push_back(it);
        items.push_back(make_item(Bag, 4, gc::kFreshSalad, 912, 1));
        rec.items = items;
        Harness h(rec, true, true, path);
        const bs::PlayerSetup& setup = h.s->bootstrap().setup();
        const svc::InventoryItem* item = h.s->inventory().at(Bag, 4);
        if (!item) return;
        h.client_sends({h.client.actor(h.pawn_ch, item_used(3, 4, guid_n(912)), true),
                        h.client.actor(h.pawn_ch, trigger_with_item(setup.guids.ability, gc::kFreshSaladAbility, 87, 4, items::to_instance(*item), 3), true)});
        for (int i = 0; i < 110; ++i) h.step();
        CHECK(effect_of(h, gc::kRestingEffect) != nullptr, "(Resting is on)");
        const size_t ends = h.s->ability_ends_sent();
        h.client_sends({h.client.actor(h.pawn_ch, ability_trigger(setup.guids.ability, kNotRun, 90, 0), true)});   // a press the server refuses
        for (int i = 0; i < 10; ++i) h.step();
        CHECK(effect_of(h, gc::kRestingEffect) == nullptr, "a press the server refuses ends the use all the same: Resting goes");
        CHECK(effect_of(h, gc::kFreshSaladEffect) == nullptr, "... and the meal's buff is not granted: the eating was interrupted, its item spent for nothing");
        CHECK(h.s->ability_ends_sent() == ends + 2, "... and the client is told twice over: the use's instance is ended, and the refused press's own instance is answered");
        CHECK(!h.s->failed() && h.mirror.unreadable == 0, "(nothing failed)");
    }

    // a press in the first 65 ms cancels the use outright: nothing is paid, the item is kept and nothing applies later
    {
        svc::Character rec = cyb_record();
        rec.level = 25;
        std::vector<svc::InventoryItem> items;
        for (const auto& it : rec.items)
            if (it.container == Worn) items.push_back(it);
        items.push_back(make_item(Bag, 4, gc::kFreshSalad, 913, 1));
        rec.items = items;
        Harness h(rec, true, true, path);
        const bs::PlayerSetup& setup = h.s->bootstrap().setup();
        const svc::InventoryItem* item = h.s->inventory().at(Bag, 4);
        if (!item) return;
        h.client_sends({h.client.actor(h.pawn_ch, item_used(3, 4, guid_n(913)), true),
                        h.client.actor(h.pawn_ch, trigger_with_item(setup.guids.ability, gc::kFreshSaladAbility, 88, 4, items::to_instance(*item), 3), true)});
        for (int i = 0; i < 2; ++i) h.step();   // 40 ms: before the cost step
        const size_t ends = h.s->ability_ends_sent();
        h.client_sends({h.client.actor(h.pawn_ch, ability_trigger(setup.guids.ability, kNotRun, 92, 0), true)});   // a press the server refuses
        for (int i = 0; i < 110; ++i) h.step();   // past the time the effects would have come
        CHECK(h.s->inventory().at(Bag, 4) != nullptr && h.s->inventory().at(Bag, 4)->quantity == 1, "a press before the cost step keeps the item");
        CHECK(h.s->ability_ends_sent() == ends + 2, "... the client is told twice over: the use's instance is ended, and the refused press's own instance is answered");
        CHECK(effect_of(h, gc::kRestingEffect) == nullptr && effect_of(h, gc::kFreshSaladEffect) == nullptr, "... and nothing of the meal applies");
        CHECK(!h.s->failed() && h.mirror.unreadable == 0, "(nothing failed)");
    }

    // a Dodge ends only a use that has not launched: a paid one goes on, one before its cost step is cancelled with the item kept
    {
        svc::Character rec = cyb_record();
        rec.level = 25;
        std::vector<svc::InventoryItem> items;
        for (const auto& it : rec.items)
            if (it.container == Worn) items.push_back(it);
        items.push_back(make_item(Bag, 4, gc::kFreshSalad, 913, 1));
        items.push_back(make_item(Bag, 5, gc::kFreshSalad, 914, 1));
        rec.items = items;
        Harness h(rec, true, true, path);
        const bs::PlayerSetup& setup = h.s->bootstrap().setup();
        const svc::InventoryItem* first = h.s->inventory().at(Bag, 4);
        const svc::InventoryItem* second = h.s->inventory().at(Bag, 5);
        if (!first || !second) return;
        h.client_sends({h.client.actor(h.pawn_ch, item_used(3, 4, guid_n(913)), true),
                        h.client.actor(h.pawn_ch, trigger_with_item(setup.guids.ability, gc::kFreshSaladAbility, 88, 4, items::to_instance(*first), 3), true)});
        for (int i = 0; i < 110; ++i) h.step();
        CHECK(effect_of(h, gc::kRestingEffect) != nullptr, "(the first use's Resting is on)");
        h.client_sends({h.client.actor(h.pawn_ch, ability_trigger(setup.guids.ability, content::kGuidDodge, 92, 0), true)});
        for (int i = 0; i < 10; ++i) h.step();
        CHECK(effect_of(h, gc::kRestingEffect) != nullptr, "a Dodge does not end a use that has launched: the meal is spent and its Resting stays");
        for (int i = 0; i < 40; ++i) h.step();   // past the Dodge's cooldown (0.65 s), so the second Dodge below is taken
        h.client_sends({h.client.actor(h.pawn_ch, item_used(3, 5, guid_n(914)), true),
                        h.client.actor(h.pawn_ch, trigger_with_item(setup.guids.ability, gc::kFreshSaladAbility, 89, 5, items::to_instance(*second), 3), true)});
        for (int i = 0; i < 2; ++i) h.step();   // 40 ms: before the second use's cost step
        const size_t ends = h.s->ability_ends_sent();
        h.client_sends({h.client.actor(h.pawn_ch, ability_trigger(setup.guids.ability, content::kGuidDodge, 96, 0), true)});
        for (int i = 0; i < 5; ++i) h.step();
        CHECK(h.s->inventory().at(Bag, 5) != nullptr && h.s->ability_ends_sent() >= ends + 1,
              "a Dodge before the cost step cancels the use and keeps the item");
        CHECK(!h.s->failed() && h.mirror.unreadable == 0, "(nothing failed)");
    }

    // damage ends a paid use (the data's cancel rule names a hit that takes Health)
    {
        svc::Character rec = cyb_record();
        rec.level = 25;
        std::vector<svc::InventoryItem> items;
        for (const auto& it : rec.items)
            if (it.container == Worn) items.push_back(it);
        items.push_back(make_item(Bag, 4, gc::kFreshSalad, 915, 1));
        rec.items = items;
        Harness h(rec, true, true, path);
        const bs::PlayerSetup& setup = h.s->bootstrap().setup();
        const svc::InventoryItem* item = h.s->inventory().at(Bag, 4);
        if (!item) return;
        h.client_sends({h.client.actor(h.pawn_ch, item_used(3, 4, guid_n(915)), true),
                        h.client.actor(h.pawn_ch, trigger_with_item(setup.guids.ability, gc::kFreshSaladAbility, 93, 4, items::to_instance(*item), 3), true)});
        for (int i = 0; i < 110; ++i) h.step();
        CHECK(effect_of(h, gc::kRestingEffect) != nullptr, "(Resting is on)");
        h.s->take_damage(h.conn, 100.0, h.t);
        for (int i = 0; i < 5; ++i) h.step();
        CHECK(effect_of(h, gc::kRestingEffect) == nullptr, "taking damage ends the use: Resting goes");
        CHECK(effect_of(h, gc::kFreshSaladEffect) == nullptr, "... and the meal's buff is not granted: the eating was interrupted, its item spent for nothing");
        CHECK(!h.s->failed() && h.mirror.unreadable == 0, "(nothing failed)");
    }

    // the live hit path carries the same rule: a hit with a Health loss ends the use — whoever sent it — while an effect's own tick (a damage-over-time debuff)
    // does not, the rule's own carve-out
    {
        svc::Character rec = cyb_record();
        rec.level = 25;
        std::vector<svc::InventoryItem> items;
        for (const auto& it : rec.items)
            if (it.container == Worn) items.push_back(it);
        items.push_back(make_item(Bag, 4, gc::kFreshSalad, 917, 1));
        items.push_back(make_item(Bag, 5, gc::kFreshSalad, 918, 1));
        rec.items = items;
        Harness h(rec, true, true, path);
        const bs::PlayerSetup& setup = h.s->bootstrap().setup();
        const svc::InventoryItem* first = h.s->inventory().at(Bag, 4);
        const svc::InventoryItem* second = h.s->inventory().at(Bag, 5);
        if (!first || !second) return;
        h.client_sends({h.client.actor(h.pawn_ch, item_used(3, 4, guid_n(917)), true),
                        h.client.actor(h.pawn_ch, trigger_with_item(setup.guids.ability, gc::kFreshSaladAbility, 97, 4, items::to_instance(*first), 3), true)});
        for (int i = 0; i < 110; ++i) h.step();
        CHECK(effect_of(h, gc::kRestingEffect) != nullptr, "(Resting is on)");
        const size_t ends = h.s->ability_ends_sent();
        combat::PlayerIncomingHit blow;   // a creature's or another player's hit landing on the player: the combat world's own path to a session
        blow.source_type = content::kSourceTypeAbility;
        blow.mods.push_back({0, content::kGuidHealth, -100.0});
        blow.at = world_time_at(h.t);
        std::vector<double> effective;
        h.s->receive_player_hit(blow, effective);
        for (int i = 0; i < 10; ++i) h.step();
        CHECK(effect_of(h, gc::kRestingEffect) == nullptr && effect_of(h, gc::kFreshSaladEffect) == nullptr,
              "a hit with a Health loss ends the use through the live path: Resting goes and no buff is granted");
        CHECK(h.s->ability_ends_sent() == ends + 1, "... the client is told the instance is over");
        combat::PlayerIncomingHit tick;   // a damage-over-time tick: an effect's hit, which the rule leaves out
        tick.source_type = content::kSourceTypeEffect;
        tick.mods.push_back({0, content::kGuidHealth, -10.0});
        tick.at = world_time_at(h.t);
        h.client_sends({h.client.actor(h.pawn_ch, item_used(3, 5, guid_n(918)), true),
                        h.client.actor(h.pawn_ch, trigger_with_item(setup.guids.ability, gc::kFreshSaladAbility, 98, 5, items::to_instance(*second), 3), true)});
        for (int i = 0; i < 110; ++i) h.step();
        CHECK(effect_of(h, gc::kRestingEffect) != nullptr, "(the second meal's Resting is on)");
        h.s->receive_player_hit(tick, effective);
        for (int i = 0; i < 10; ++i) h.step();
        CHECK(effect_of(h, gc::kRestingEffect) != nullptr, "a damage-over-time tick does not end the use: the rule leaves an effect's hit out");
        const int64_t server = h.s->pipeline() ? h.s->pipeline()->instance_of(98) : -1;
        if (server >= 0) {
            h.client_sends({h.client.actor(h.pawn_ch, end_ability(setup, server), true)});   // the eating finishes: the buff comes with its own end
            for (int i = 0; i < 15; ++i) h.step();
        }
        CHECK(effect_of(h, gc::kFreshSaladEffect) != nullptr, "... and the meal's buff is granted when its eating finishes");
        CHECK(!h.s->failed() && h.mirror.unreadable == 0, "(nothing failed)");
    }

    // the combat side's own inbox (PlayerSession::take_hit: the path a creature's melee, its projectiles and its zones land through) carries the rule too
    {
        svc::Character rec = cyb_record();
        rec.level = 25;
        std::vector<svc::InventoryItem> items;
        for (const auto& it : rec.items)
            if (it.container == Worn) items.push_back(it);
        items.push_back(make_item(Bag, 4, gc::kFreshSalad, 919, 1));
        rec.items = items;
        Harness h(rec, true, true, path);
        const bs::PlayerSetup& setup = h.s->bootstrap().setup();
        const svc::InventoryItem* item = h.s->inventory().at(Bag, 4);
        if (!item) return;
        h.client_sends({h.client.actor(h.pawn_ch, item_used(3, 4, guid_n(919)), true),
                        h.client.actor(h.pawn_ch, trigger_with_item(setup.guids.ability, gc::kFreshSaladAbility, 99, 4, items::to_instance(*item), 3), true)});
        for (int i = 0; i < 110; ++i) h.step();
        CHECK(effect_of(h, gc::kRestingEffect) != nullptr, "(Resting is on)");
        const size_t ends = h.s->ability_ends_sent();
        combat::IncomingHit blow;   // a creature's melee blow, as the combat world hands it to the session's inbox
        blow.source_type = content::kSourceTypeAbility;
        blow.mods.push_back({0, content::kGuidHealth, -100.0});
        h.s->take_hit(h.conn, blow, h.t);
        for (int i = 0; i < 10; ++i) h.step();
        CHECK(effect_of(h, gc::kRestingEffect) == nullptr && effect_of(h, gc::kFreshSaladEffect) == nullptr,
              "a creature's blow through the combat side's inbox ends the use: Resting goes and no buff is granted");
        CHECK(h.s->ability_ends_sent() == ends + 1, "... the client is told the instance is over");
        CHECK(!h.s->failed() && h.mirror.unreadable == 0, "(nothing failed)");
    }

    // a potion's use is never interrupted (the data has no cancel rule for its kind): the heal lands despite the move
    {
        svc::Character rec = cyb_record();
        rec.level = 25;
        std::vector<svc::InventoryItem> items;
        for (const auto& it : rec.items)
            if (it.container == Worn) items.push_back(it);
        items.push_back(make_item(Bag, 4, gc::kHealthPotion, 916, 1));
        rec.items = items;
        Harness h(rec, true, true, path);
        const sg::CharacterStats* cs = h.s->character_stats();
        const bs::PlayerSetup& setup = h.s->bootstrap().setup();
        if (!cs || !h.s->set_current_value(gc::kHealthStat, 1.0f)) return;
        const float before = cs->find(gc::kHealthStat)->value;
        const svc::InventoryItem* item = h.s->inventory().at(Bag, 4);
        if (!item) return;
        h.client_sends({h.client.actor(h.pawn_ch, item_used(3, 4, guid_n(916)), true),
                        h.client.actor(h.pawn_ch, trigger_with_item(setup.guids.ability, gc::kHealthPotionAbility, 94, 4, items::to_instance(*item), 3), true),
                        h.client.actor(h.pawn_ch, hit_request_for(setup, gc::kHealthPotionAbility, kSelfTargetingGuid, gc::kHealthPotionHit), true)});
        for (int i = 0; i < 2; ++i) h.step();
        const GoodPosition from = h.s->last_position();
        h.client_sends({h.client.actor(h.pawn_ch, walk_move(from, 100.0f, 10.0), false),
                        h.client.actor(h.pawn_ch, walk_move(from, 100.1f, 20.0), false)});
        for (int i = 0; i < 25; ++i) h.step();
        CHECK(h.s->moves_refused() == 0, "(the moves were accepted)");
        CHECK(cs->find(gc::kHealthStat)->value - before == 1052.0f, "a move does not interrupt a potion: the heal lands in full");
        CHECK(h.s->inventory().at(Bag, 4) == nullptr, "(the potion is spent)");
    }

    // combat ends every Resting effect, whatever applied it (here a meal's): the #20 review's rule
    {
        svc::Character rec = cyb_record();
        rec.level = 25;
        std::vector<svc::InventoryItem> items;
        for (const auto& it : rec.items)
            if (it.container == Worn) items.push_back(it);
        items.push_back(make_item(Bag, 4, gc::kFreshSalad, 917, 1));
        rec.items = items;
        Harness h(rec, true, true, path);
        const bs::PlayerSetup& setup = h.s->bootstrap().setup();
        const svc::InventoryItem* item = h.s->inventory().at(Bag, 4);
        if (!item) return;
        h.client_sends({h.client.actor(h.pawn_ch, item_used(3, 4, guid_n(917)), true),
                        h.client.actor(h.pawn_ch, trigger_with_item(setup.guids.ability, gc::kFreshSaladAbility, 95, 4, items::to_instance(*item), 3), true)});
        for (int i = 0; i < 110; ++i) h.step();
        CHECK(effect_of(h, gc::kRestingEffect) != nullptr, "(a meal's Resting is on)");
        if (h.s->pipeline()) const_cast<AbilityPipeline*>(h.s->pipeline())->end_resting_effects(gc::kRestingEffect, world_time_at(h.t), "combat begins");
        for (int i = 0; i < 5; ++i) h.step();
        CHECK(effect_of(h, gc::kRestingEffect) == nullptr, "combat begins: every Resting ends, the meal's among them");
        const int64_t server = h.s->pipeline() ? h.s->pipeline()->instance_of(95) : -1;
        if (server >= 0) {
            h.client_sends({h.client.actor(h.pawn_ch, end_ability(setup, server), true)});   // the eating finishes: the buff is granted as usual
            for (int i = 0; i < 15; ++i) h.step();
        }
        CHECK(effect_of(h, gc::kFreshSaladEffect) != nullptr, "... and the meal's buff is granted when its eating finishes, combat or not");
        CHECK(!h.s->failed() && h.mirror.unreadable == 0, "(nothing failed)");
    }
}
AOC_REGISTER_TEST(test_item_use_is_interrupted);
