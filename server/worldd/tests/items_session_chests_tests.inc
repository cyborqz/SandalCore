// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/items_session_chests_tests.inc: chests (random coffers and choose-one selectors)
//  Included from items_session_registered.inc; the program runs the registered tests through aoc_tests::run.
// ============================================================================

static std::vector<svc::InventoryItem> chest_worn_items(const svc::Character& rec) {
    std::vector<svc::InventoryItem> items;
    for (const auto& it : rec.items) {
        if (it.container == Worn) items.push_back(it);
    }
    return items;
}

static BunchWriter select_reward(int32_t index) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerSelectReward");
    repl::RpcParams p;
    if (!rpc || !repl::set_rpc_param(*rpc, p, "Index", repl::Value::i64(index))) return BunchWriter();
    return actor_rpc(pc, "ServerSelectReward", p);
}

static BunchWriter cancel_reward() {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    return actor_rpc(pc, "ServerCancelSelectReward", {});
}

/// A move of one chest to another bag slot, as the client sends it (the item's own record, instance and quantity).
static BunchWriter chest_move(Harness& h, int from, int to, const std::string& guid, int64_t record) {
    const bs::SessionGuids& g = h.s->bootstrap().setup().guids;
    repl::ItemStorageEvent ev;
    ev.type = static_cast<uint8_t>(repl::StorageEventType::Move);
    const auto gv = items::guid_of(guid);
    ev.item.has_item = true;
    ev.item.record_id = static_cast<uint64_t>(record);
    ev.item.quantity = 1;
    ev.item.quality = 1;
    if (gv) ev.item.guid = *gv;
    ev.item.creation_time = items::ticks_of_ms(1'771'299'474'000ll);
    ev.move_source.object = g.backpack;
    ev.move_source.slots.push_back({from, 1, gv ? *gv : repl::GuidValue{}});
    repl::StorageEventContainer target;
    target.object = g.backpack;
    target.slots.push_back({to, 1, gv ? *gv : repl::GuidValue{}});
    ev.targets.push_back(target);
    const auto& cg = h.s->bootstrap().setup().character_guid;
    ev.instigator = repl::GuidValue{cg[0], cg[1], cg[2], cg[3]};
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerRequestStorageEvent");
    const auto params = rpc ? repl::storage_event_params(*rpc, ev) : std::nullopt;
    return params ? actor_rpc(pc, "ServerRequestStorageEvent", *params) : BunchWriter();
}

void test_chests() {
    const char* path = professions_db_path();
    if (!path) {
        std::printf("Chests: skipped (no real content.db)\n");
        return;
    }
    std::printf("Chests: a selector's window and choice, a cancel, and a random coffer\n");
    constexpr int64_t kSelector = 6064634654234247330ll, kSelectorTable = 6064634654246109347ll;
    constexpr int64_t kLongbow = 6064634654088298644ll, kShortbow = 6064634654088364183ll,
                      kWand = 6064634654088364185ll, kTome = 6064634654088298642ll;
    constexpr int64_t kCoffer = 6064634008145100806ll;   // Lootbox_Commission_RVR_Cull_lvl1
    constexpr int64_t kStacked = 6064633950439145478ll;  // CSQ_RVR_0111_EotGH_WaysOfThePast_RewardBox
    constexpr int64_t kRations = 6064632025123913728ll;

    // the selector: the window, the choice, the grant and the chest's removal
    {
        svc::Character rec = cyb_record();
        rec.level = 25;
        std::vector<svc::InventoryItem> items = chest_worn_items(rec);
        items.push_back(make_item(Bag, 4, kSelector, 900, 1));
        rec.items = items;
        Harness h(rec, true, true, path);
        const size_t events_before = h.mirror.events.size();
        h.client_sends({h.client.actor(h.pawn_ch, item_used(3, 4, guid_n(900)), true),
                        h.client.actor(h.pawn_ch, item_activate(3, 4, guid_n(900)), true)});
        for (int i = 0; i < 10; ++i) h.step();
        CHECK(h.mirror.reward_windows.size() == 1, "the selector opens ClientOpenRewardSelection");
        if (h.mirror.reward_windows.size() == 1) {
            const repl::RewardInstance& win = h.mirror.reward_windows.back();
            CHECK(win.items.size() == 4, "the window lists its four options");
            CHECK(win.items.size() == 4 && win.items[0].record_id == kLongbow && win.items[1].record_id == kShortbow &&
                      win.items[2].record_id == kWand && win.items[3].record_id == kTome,
                  "... in the table's order");
            CHECK(win.source_reward_tables.size() == 1 && win.source_reward_tables[0].first == kSelectorTable,
                  "... and names the selection table as their source");
        }
        CHECK(h.mirror.reward_window_items.size() == 1 && items::guid_hex(h.mirror.reward_window_items[0]) == guid_n(900),
              "the window names the chest's own instance");
        h.client_sends({h.client.actor(h.pc_ch, select_reward(2), true)});
        for (int i = 0; i < 10; ++i) h.step();
        CHECK(h.mirror.events.size() == events_before + 2, "the choice sends one Add and one Remove");
        if (h.mirror.events.size() >= events_before + 2) {
            const auto& add = h.mirror.events[events_before];
            const auto& remove = h.mirror.events[events_before + 1];
            CHECK(add.type == static_cast<uint8_t>(repl::StorageEventType::Add) && add.item.record_id == kWand,
                  "the chosen Wand is added");
            CHECK(remove.type == static_cast<uint8_t>(repl::StorageEventType::Remove) && remove.item.record_id == kSelector,
                  "the chest is removed");
            CHECK(add.callout.has_value() && remove.callout.has_value(), "the item and the chest carry their usual popups");
        }
        CHECK(h.s->inventory().at(Bag, 4) == nullptr, "the chest left the bag");
        bool wand = false;
        for (const svc::InventoryItem& it : h.s->inventory().items()) wand = wand || it.item_record_id == kWand;
        CHECK(wand, "the chosen item is in the bags");
    }

    // the first option: Index 0 is the default and is not on the wire; it must still be selectable
    {
        svc::Character rec = cyb_record();
        rec.level = 25;
        std::vector<svc::InventoryItem> items = chest_worn_items(rec);
        items.push_back(make_item(Bag, 4, kSelector, 903, 1));
        rec.items = items;
        Harness h(rec, true, true, path);
        h.client_sends({h.client.actor(h.pawn_ch, item_used(3, 4, guid_n(903)), true)});
        for (int i = 0; i < 10; ++i) h.step();
        CHECK(h.mirror.reward_windows.size() == 1, "(the window opens for the first-option test)");
        const size_t before = h.mirror.events.size();
        h.client_sends({h.client.actor(h.pc_ch, select_reward(0), true)});
        for (int i = 0; i < 10; ++i) h.step();
        CHECK(h.mirror.events.size() == before + 2, "choosing index 0 sends the Add and the Remove");
        if (h.mirror.events.size() >= before + 2) {
            CHECK(h.mirror.events[before].type == static_cast<uint8_t>(repl::StorageEventType::Add) &&
                  h.mirror.events[before].item.record_id == kLongbow,
                  "the first option (Longbow) is granted");
            CHECK(h.mirror.events[before + 1].type == static_cast<uint8_t>(repl::StorageEventType::Remove) &&
                  h.mirror.events[before + 1].item.record_id == kSelector,
                  "the chest is removed");
        }
        CHECK(h.s->inventory().at(Bag, 4) == nullptr, "the chest left the bag after index 0");
    }

    // stacked options: the table's quantity, not one
    {
        svc::Character rec = cyb_record();
        rec.level = 25;
        std::vector<svc::InventoryItem> items = chest_worn_items(rec);
        items.push_back(make_item(Bag, 4, kStacked, 906, 1));
        rec.items = items;
        Harness h(rec, true, true, path);
        h.client_sends({h.client.actor(h.pawn_ch, item_used(3, 4, guid_n(906)), true)});
        for (int i = 0; i < 10; ++i) h.step();
        CHECK(h.mirror.reward_windows.size() == 1, "(the stacked selector opens)");
        if (h.mirror.reward_windows.size() == 1) {
            const repl::RewardInstance& win = h.mirror.reward_windows.back();
            CHECK(win.items.size() == 3 && win.items[0].record_id == kRations && win.items[0].quantity == 10,
                  "the window shows ten Rations, the table's quantity");
            CHECK(win.items.size() == 3 && win.items[1].quantity == 5 && win.items[2].quantity == 5,
                  "... and five of each salve");
        }
        const size_t before = h.mirror.events.size();
        h.client_sends({h.client.actor(h.pc_ch, select_reward(0), true)});
        for (int i = 0; i < 10; ++i) h.step();
        CHECK(h.mirror.events.size() == before + 2, "(the stacked pick sends the Add and the Remove)");
        if (h.mirror.events.size() >= before + 2) {
            CHECK(h.mirror.events[before].type == static_cast<uint8_t>(repl::StorageEventType::Add) &&
                  h.mirror.events[before].item.record_id == kRations && h.mirror.events[before].item.quantity == 10,
                  "ten Rations are granted, not one");
        }
    }

    // a cancel leaves the chest and sends nothing
    {
        svc::Character rec = cyb_record();
        rec.level = 25;
        std::vector<svc::InventoryItem> items = chest_worn_items(rec);
        items.push_back(make_item(Bag, 4, kSelector, 901, 1));
        rec.items = items;
        Harness h(rec, true, true, path);
        const size_t events_before = h.mirror.events.size();
        h.client_sends({h.client.actor(h.pawn_ch, item_used(3, 4, guid_n(901)), true)});
        for (int i = 0; i < 10; ++i) h.step();
        CHECK(h.mirror.reward_windows.size() == 1, "(the window opens)");
        h.client_sends({h.client.actor(h.pc_ch, cancel_reward(), true)});
        for (int i = 0; i < 5; ++i) h.step();
        CHECK(h.mirror.events.size() == events_before && h.s->inventory().at(Bag, 4) != nullptr,
              "a cancel leaves the chest and sends nothing");
    }

    // a random coffer: no window, the rolled items in the bags and the coffer consumed
    {
        svc::Character rec = cyb_record();
        rec.level = 25;
        std::vector<svc::InventoryItem> items = chest_worn_items(rec);
        items.push_back(make_item(Bag, 4, kCoffer, 902, 1));
        rec.items = items;
        Harness h(rec, true, true, path);
        const size_t events_before = h.mirror.events.size();
        h.client_sends({h.client.actor(h.pawn_ch, item_used(3, 4, guid_n(902)), true),
                        h.client.actor(h.pawn_ch, item_activate(3, 4, guid_n(902)), true)});
        for (int i = 0; i < 20; ++i) h.step();
        CHECK(h.mirror.reward_windows.empty(), "a random coffer does not open the selection window");
        CHECK(h.s->inventory().at(Bag, 4) == nullptr, "the coffer is consumed");
        size_t adds = 0, removes = 0;
        for (size_t i = events_before; i < h.mirror.events.size(); ++i) {
            if (h.mirror.events[i].type == static_cast<uint8_t>(repl::StorageEventType::Add)) ++adds;
            if (h.mirror.events[i].type == static_cast<uint8_t>(repl::StorageEventType::Remove)) ++removes;
        }
        CHECK(adds >= 1, "the rolled items arrive as Adds");
        CHECK(removes == 1, "the coffer is removed");
    }
}
AOC_REGISTER_TEST(test_chests);

void test_chest_refusals() {
    const char* path = professions_db_path();
    if (!path) {
        std::printf("Chest refusals: skipped (no real content.db)\n");
        return;
    }
    std::printf("Chests: a pick with no window, an index out of range, a moved chest and a full bag\n");
    constexpr int64_t kSelector = 6064634654234247330ll;
    constexpr int64_t kLongbow = 6064634654088298644ll;

    // no window open
    {
        svc::Character rec = cyb_record();
        rec.level = 25;
        rec.items = chest_worn_items(rec);
        Harness h(rec, true, true, path);
        const size_t before = h.mirror.events.size();
        h.client_sends({h.client.actor(h.pc_ch, select_reward(0), true)});
        for (int i = 0; i < 5; ++i) h.step();
        CHECK(h.mirror.events.size() == before && !h.s->failed(), "a pick with no window open changes nothing");
    }

    // an index out of range
    {
        svc::Character rec = cyb_record();
        rec.level = 25;
        std::vector<svc::InventoryItem> items = chest_worn_items(rec);
        items.push_back(make_item(Bag, 4, kSelector, 907, 1));
        rec.items = items;
        Harness h(rec, true, true, path);
        h.client_sends({h.client.actor(h.pawn_ch, item_used(3, 4, guid_n(907)), true)});
        for (int i = 0; i < 10; ++i) h.step();
        CHECK(h.mirror.reward_windows.size() == 1, "(the window opens for the out-of-range test)");
        const size_t before = h.mirror.events.size();
        h.client_sends({h.client.actor(h.pc_ch, select_reward(99), true)});
        for (int i = 0; i < 5; ++i) h.step();
        CHECK(h.mirror.events.size() == before && h.s->inventory().at(Bag, 4) != nullptr,
              "an index out of range grants nothing and leaves the chest");
    }

    // the chest moved before the pick
    {
        svc::Character rec = cyb_record();
        rec.level = 25;
        std::vector<svc::InventoryItem> items = chest_worn_items(rec);
        items.push_back(make_item(Bag, 4, kSelector, 908, 1));
        rec.items = items;
        Harness h(rec, true, true, path);
        h.client_sends({h.client.actor(h.pawn_ch, item_used(3, 4, guid_n(908)), true)});
        for (int i = 0; i < 10; ++i) h.step();
        CHECK(h.mirror.reward_windows.size() == 1, "(the window opens for the moved-chest test)");
        h.request(chest_move(h, 4, 5, guid_n(908), kSelector), h.pc_ch);
        for (int i = 0; i < 5; ++i) h.step();
        CHECK(h.s->inventory().at(Bag, 5) != nullptr, "(the chest is in slot 5 now)");
        const size_t before = h.mirror.events.size();
        h.client_sends({h.client.actor(h.pc_ch, select_reward(0), true)});
        for (int i = 0; i < 5; ++i) h.step();
        bool granted = false;
        for (const svc::InventoryItem& it : h.s->inventory().items()) granted = granted || it.item_record_id == kLongbow;
        CHECK(h.mirror.events.size() == before && !granted && h.s->inventory().at(Bag, 5) != nullptr,
              "a chest moved after its window opened grants nothing and stays");
    }

    // a full bag
    {
        svc::Character rec = cyb_record();
        rec.level = 25;
        std::vector<svc::InventoryItem> items = chest_worn_items(rec);
        for (int s = 0; s < 70; ++s) {
            if (s == 4) continue;
            items.push_back(make_item(Bag, s, fi::kPotion, 1000 + s, 1));
        }
        items.push_back(make_item(Bag, 4, kSelector, 909, 1));
        rec.items = items;
        Harness h(rec, true, true, path);
        h.client_sends({h.client.actor(h.pawn_ch, item_used(3, 4, guid_n(909)), true)});
        for (int i = 0; i < 10; ++i) h.step();
        CHECK(h.mirror.reward_windows.size() == 1, "(the window opens with a full bag)");
        const size_t before = h.mirror.events.size();
        h.client_sends({h.client.actor(h.pc_ch, select_reward(0), true)});
        for (int i = 0; i < 5; ++i) h.step();
        CHECK(h.mirror.events.size() == before && h.s->inventory().at(Bag, 4) != nullptr,
              "a full bag grants nothing, sends nothing and leaves the chest");
        CHECK(h.mirror.reward_windows.size() == 1, "... and the window stays open");
    }
}
AOC_REGISTER_TEST(test_chest_refusals);
