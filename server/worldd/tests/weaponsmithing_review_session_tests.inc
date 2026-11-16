// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  weaponsmithing_review_session_tests.inc: the tool craft through a session, the cases the first tests left open (the Weaponsmithing milestone, after its review)
//
//  What the client is told of a made tool (the Add names the Equipment storage and the tool slot, the last Add carries the craft's id), a tool made with the bag full (the free
//  tool slot needs no room, a worn one sends the tool to a bag that has none and the craft is over with nothing taken) and a connection lost in the middle of a craft (what the
//  character saved holds the whole materials, and the same craft is made after the reconnect).  Included by test_worldd_items_session.cpp after
//  weaponsmithing_session_tests.inc; its tests are called from main().
// ============================================================================

/// The quantity of `record` the session holds in all its storages.
static int64_t ws_total(const PlayerSession& s, int64_t record) {
    int64_t n = 0;
    for (const svc::InventoryItem& it : s.inventory().items()) n += it.item_record_id == record ? it.quantity : 0;
    return n;
}

/// The bag of an Apprentice who makes the Golgora pickaxe once with `boards` boards, `ingots` brass ingots and `molds` molds, and `fillers` Brooksteel Axes in the slots after them.
static std::vector<svc::InventoryItem> ws_golgora_bag(int boards, int ingots, int molds, int fillers) {
    std::vector<svc::InventoryItem> items = {make_item(Bag, 0, wss::kBoardOak, 800, boards), make_item(Bag, 1, wss::kIngotBrass, 801, ingots), make_item(Bag, 2, wss::kMoldBasalt, 802, molds)};
    for (int i = 0; i < fillers; ++i) items.push_back(make_item(Bag, 3 + i, wss::kAxeItem, 900 + static_cast<uint32_t>(i)));
    return items;
}

/// Opens the Apprentice station of a harness and starts the Golgora pickaxe with the stacks of the first craft.
static WsStation ws_start_golgora(Harness& h) {
    const WsStation station = ws_add_station(h, wss::kSmithT2, "CraftingStation_Weaponsmithing_T2", wss::kT1Tax, 300, 0);
    if (station.id == 0) return station;
    ws_phase_in(h);
    h.request(ws_interact(station.component), h.pawn_ch);
    h.request(ws_start_craft(station, wss::kGolgoraRecipe, {wss_resource(wss::kBoardOak, 10), wss_resource(wss::kIngotBrass, 10), wss_resource(wss::kMoldBasalt, 1)}), h.pc_ch);
    return station;
}

/// What the client is told: the Removes of the materials, then the Add of the pickaxe in the Equipment's Pickaxe slot, carrying the id of the craft.
static void test_ws_session_tool_events() {
    std::printf("a made tool: the client's Add names the Equipment storage and the tool slot\n");
    if (!ws_real_db()) {
        std::printf("  [skip] no real content.db: the Weaponsmithing session tests are skipped\n");
        return;
    }
    Harness h(ws_character(1, {wss::kGolgoraRecipe}, ws_golgora_bag(10, 10, 1, 0)), true, true, ws_real_db());
    CHECK(h.s != nullptr, "(joined as an Apprentice)");
    if (!h.s) return;
    const size_t chains = h.got.size();
    const WsStation station = ws_start_golgora(h);
    if (station.id == 0) return;
    CHECK(h.s->quick_active(), "the craft begins");
    ws_run(h, 2400);
    h.settle();
    CHECK(h.s->quick_made() == 1, "the pickaxe is made");
    const bs::SessionGuids& guids = h.s->bootstrap().setup().guids;
    int removes = 0, adds = 0, in_slot = 0, backpack_adds = 0;
    bool craft_id = false;
    for (const repl::ItemStorageEvent& ev : ws_storage_events_since(h, chains)) {
        if (ev.targets.size() != 1 || ev.targets[0].slots.size() != 1) continue;
        if (ev.type == static_cast<uint8_t>(repl::StorageEventType::Remove)) ++removes;
        if (ev.type != static_cast<uint8_t>(repl::StorageEventType::Add)) continue;
        ++adds;
        backpack_adds += ev.targets[0].object == guids.backpack ? 1 : 0;
        if (ev.targets[0].object == guids.equipment && ev.targets[0].slots[0].slot == items::equip::Pickaxe && ev.item.record_id == wss::kGolgoraItem && ev.item.quality == 1 &&
            std::fabs(ev.item.durability - 300.0) < 0.01) {
            ++in_slot;
            craft_id = ev.extra_guid_b.has_value() && *ev.extra_guid_b == repl::GuidValue{7, 8, 9, 10};
        }
    }
    CHECK(removes == 3, "three Removes: the boards, the ingots and the mold");
    CHECK(adds == 1 && in_slot == 1 && backpack_adds == 0, "one Add, in the Equipment's Pickaxe slot 26 at quality 1 and durability 300, and none in the bag");
    CHECK(craft_id, "the Add carries the id of the craft (it ends the client's craft)");
    CHECK(h.mirror.unreadable == 0, "(the client read every chain)");
}

/// A tool with the bag full: a free tool slot needs no room, a worn one sends the tool to the bag and the craft is over with nothing taken.
static void test_ws_session_tool_room() {
    std::printf("a tool made with the bag full\n");
    if (!ws_real_db()) {
        std::printf("  [skip] no real content.db: the Weaponsmithing session tests are skipped\n");
        return;
    }
    // 20 boards, 20 ingots and 2 molds: the craft takes half of each, so the three stacks stay and the 67 axes behind them fill the 70 slots of the bag
    const int fillers = items::kBackpackSlots - 3;
    {
        Harness h(ws_character(1, {wss::kGolgoraRecipe}, ws_golgora_bag(20, 20, 2, fillers)), true, true, ws_real_db());
        CHECK(h.s != nullptr && h.s->inventory().count(svc::ItemContainer::Backpack) == static_cast<size_t>(items::kBackpackSlots), "(an Apprentice with a full bag and a free Pickaxe slot)");
        if (!h.s) return;
        const WsStation station = ws_start_golgora(h);
        if (station.id == 0) return;
        CHECK(h.s->quick_active(), "the craft begins");
        ws_run(h, 2400);
        h.settle();
        const svc::InventoryItem* worn = h.s->inventory().at(svc::ItemContainer::Equipment, items::equip::Pickaxe);
        CHECK(h.s->quick_made() == 1 && worn != nullptr && worn->item_record_id == wss::kGolgoraItem, "the tool goes to its free slot: it needs no room in the bag");
        CHECK(ws_total(*h.s, wss::kBoardOak) == 10 && ws_total(*h.s, wss::kIngotBrass) == 10 && ws_total(*h.s, wss::kMoldBasalt) == 1, "the materials left it: 10 boards, 10 ingots and a mold remain");
        CHECK(h.s->wallet().copper() == 100000 - 1500, "1,500 copper left the purse");
    }
    {
        std::vector<svc::InventoryItem> items = ws_golgora_bag(20, 20, 2, fillers);
        items.push_back(make_item(Worn, items::equip::Pickaxe, wss::kGolgoraItem, 760));
        Harness h(ws_character(1, {wss::kGolgoraRecipe}, std::move(items)), true, true, ws_real_db());
        CHECK(h.s != nullptr, "(the same bag with the Pickaxe slot worn)");
        if (!h.s) return;
        const WsStation station = ws_start_golgora(h);
        if (station.id == 0) return;
        CHECK(h.s->quick_active(), "the craft begins: nothing is checked against the bag until it is made");
        ws_run(h, 2400);
        h.settle();
        CHECK(h.s->quick_made() == 0 && !h.s->quick_active() && h.s->quick_interrupted() >= 1, "the worn slot sends the tool to the bag, which has no room: the craft is over, nothing is made");
        CHECK(ws_total(*h.s, wss::kBoardOak) == 20 && ws_total(*h.s, wss::kIngotBrass) == 20 && ws_total(*h.s, wss::kMoldBasalt) == 2, "nothing was taken: 20 boards, 20 ingots and 2 molds");
        CHECK(h.s->wallet().copper() == 100000, "and nothing was paid");
        const svc::InventoryItem* worn = h.s->inventory().at(svc::ItemContainer::Equipment, items::equip::Pickaxe);
        CHECK(worn != nullptr && worn->instance_guid == make_item(Worn, items::equip::Pickaxe, wss::kGolgoraItem, 760).instance_guid, "the worn pickaxe is the one it was");
        CHECK(g_log.str().find("the bags have no room") != std::string::npos, "the log says the bags had no room");
    }
}

/// A connection lost in the middle of a craft: the materials and the purse are whole in what was saved, and the same craft is made after the reconnect.
static void test_ws_session_reconnect() {
    std::printf("a connection lost in the middle of a craft, and the reconnect\n");
    if (!ws_real_db()) {
        std::printf("  [skip] no real content.db: the Weaponsmithing session tests are skipped\n");
        return;
    }
    const std::vector<svc::InventoryItem> before = {make_item(Bag, 0, wss::kCopper, 400, 5), make_item(Bag, 1, wss::kTimber, 401), make_item(Bag, 2, wss::kAxeReagent, 402)};
    Harness h(ws_character(0, {}, before), true, true, ws_real_db());
    CHECK(h.s != nullptr, "(joined)");
    if (!h.s) return;
    const WsStation station = ws_add_station(h, wss::kSmithT1, "CraftingStation_Weaponsmithing_T1", wss::kT1Tax, 300, 0);
    if (station.id == 0) return;
    ws_phase_in(h);
    h.request(ws_interact(station.component), h.pawn_ch);
    h.request(ws_start_craft(station, wss::kAxeRecipe, {wss_resource(wss::kCopper, 5), wss_resource(wss::kTimber, 1)}), h.pc_ch);
    CHECK(h.s->quick_active(), "the craft begins");
    ws_run(h, 1000);
    CHECK(h.s->quick_active() && h.s->quick_made() == 0, "half way through (2 s) nothing is made");
    h.s->finish("the connection was lost");
    CHECK(h.s->quick_made() == 0 && !ws_held(*h.s, wss::kAxeItem) && h.s->wallet().copper() == 100000, "the end of the session makes nothing and takes nothing");
    bool whole = true;
    for (const RecordingSink::Saved& saved : h.sink.inventories) {
        int64_t fragments = 0, timber = 0, reagent = 0, axes = 0;
        for (const svc::InventoryItem& it : saved.items) {
            fragments += it.item_record_id == wss::kCopper ? it.quantity : 0;
            timber += it.item_record_id == wss::kTimber ? it.quantity : 0;
            reagent += it.item_record_id == wss::kAxeReagent ? it.quantity : 0;
            axes += it.item_record_id == wss::kAxeItem ? it.quantity : 0;
        }
        whole = whole && fragments == 5 && timber == 1 && reagent == 1 && axes == 0;
    }
    CHECK(whole, "every save the session handed over holds the whole materials and no axe");
    // the character as it was saved (or as it joined, when nothing was handed over): a new session, the same craft
    const std::vector<svc::InventoryItem> rows = h.sink.inventories.empty() ? before : h.sink.inventories.back().items;
    Harness again(ws_character(0, {}, rows), true, true, ws_real_db());
    CHECK(again.s != nullptr && !again.s->quick_active() && again.s->quick_made() == 0, "a new session has no craft under way");
    if (!again.s) return;
    const WsStation second = ws_add_station(again, wss::kSmithT1, "CraftingStation_Weaponsmithing_T1", wss::kT1Tax, 300, 0);
    if (second.id == 0) return;
    ws_phase_in(again);
    again.request(ws_interact(second.component), again.pawn_ch);
    again.request(ws_start_craft(second, wss::kAxeRecipe, {wss_resource(wss::kCopper, 5), wss_resource(wss::kTimber, 1)}), again.pc_ch);
    CHECK(again.s->quick_active(), "the same craft begins again");
    ws_run(again, 2400);
    again.settle();
    CHECK(again.s->quick_made() == 1 && ws_held(*again.s, wss::kAxeItem).has_value() && again.s->wallet().copper() == 100000 - 75, "and is made: the axe, 75 copper paid");
}
