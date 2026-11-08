// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/items_session_stacks_tests.inc: joining and splitting stacks in the item bag, and the client's request for a storage payload again
//  (registered with AOC_REGISTER_TEST: test_worldd_items_session.cpp runs it through aoc_tests::run; it is included from items_session_registered.inc)
// ============================================================================

/// The Bard's 13 worn items, for a character whose bag the stack tests build.
static std::vector<svc::InventoryItem> stack_worn_items() {
    std::vector<svc::InventoryItem> items;
    for (const svc::InventoryItem& it : bard_items()) {
        if (it.container == Worn) items.push_back(it);
    }
    return items;
}

/// A potion stack in the item bag at a slot, quality and quantity.
static svc::InventoryItem stack_potion(int slot, uint32_t n, int32_t quantity, int32_t quality = 1, float durability = 11.0f) {
    svc::InventoryItem it = make_item(Bag, slot, fi::kPotion, n, quantity);
    it.quality = quality;
    it.durability = durability;
    return it;
}

/// The client's ServerRetrieveStoragePayload for one container.
static BunchWriter stack_retrieve_payload(const emit::FIntrepidNetworkGUID& target) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerRetrieveStoragePayload");
    repl::RpcParams p;
    if (!rpc || !repl::set_rpc_param(*rpc, p, "TargetStorage", repl::Value(target))) return BunchWriter();
    return actor_rpc(pc, "ServerRetrieveStoragePayload", p);
}

/// Every slot of a storage payload is the inventory's: the item as the wire carries it, or the empty instance.
static bool stack_payload_is_inventory(const repl::StoragePayload& payload, const items::Inventory& inv, svc::ItemContainer container) {
    const int slots = items::slot_count(container);
    if (payload.maximum_items != slots || payload.storage.size() != static_cast<size_t>(slots)) return false;
    for (int slot = 0; slot < slots; ++slot) {
        const svc::InventoryItem* it = inv.at(container, slot);
        const repl::ItemInstance want = it ? items::to_instance(*it) : repl::ItemInstance{};
        const repl::StorageSlot& got = payload.storage[static_cast<size_t>(slot)];
        if (!(got.item == want)) return false;
        if (it && got.slot != slot) return false;
    }
    return true;
}

/// A character whose action bar names the potion stack `linked` on bank 0 slot 4.
static svc::Character character_with_linked_potion(std::vector<svc::InventoryItem> items, const std::string& linked) {
    svc::Character c = record_with(std::move(items));
    svc::ActionbarSlot slot;
    slot.bank = 0;
    slot.slot = 4;
    slot.use_type = repl::ActionbarUseType::Item;
    slot.item_record_id = fi::kPotion;
    slot.item_guid = linked;
    slot.item_quality = 1;
    slot.item_rarity = 0;
    c.action_bar.push_back(slot);
    return c;
}

void test_stack_session_action_bar_follows_a_join() {
    std::printf("a join that empties a stack on the action bar moves the bar slot to the stack it joined\n");
    std::vector<svc::InventoryItem> items = stack_worn_items();
    items.push_back(stack_potion(1, 201, 6));
    items.push_back(stack_potion(4, 204, 14));
    Harness h(character_with_linked_potion(items, guid_n(201)));
    h.request(client_load_rpc(), h.pc_ch);   // the client has its bar before the join
    const repl::ActionbarUseData* linked = h.s->action_bar().at(0, 4);
    CHECK(linked && linked->use_type == repl::ActionbarUseType::Item && items::guid_hex(linked->item_guid) == guid_n(201), "(the bar slot names the stack in bag slot 1)");
    const size_t payloads = h.mirror.bar_payloads.size();
    h.request(bag_move(h, repl::StorageEventType::Move, 1, 6, guid_n(201), 4, guid_n(204), 6), h.pc_ch);
    CHECK(h.s->inventory().at(Bag, 4) && h.s->inventory().at(Bag, 4)->quantity == 20 && !h.s->inventory().at(Bag, 1), "(the join emptied the linked stack into the 14)");
    const repl::ActionbarUseData* moved = h.s->action_bar().at(0, 4);
    CHECK(moved && items::guid_hex(moved->item_guid) == guid_n(204) && moved->item_record_id == fi::kPotion && moved->item_quality == 1,
          "the bar slot now names the stack it was joined into (its guid), so the key still uses the item");
    CHECK(h.mirror.bar_payloads.size() == payloads + 1 && h.mirror.bar_payloads.back().slots.size() == 1 && h.mirror.bar_payloads.back().slots[0].bank_index == 0 &&
              h.mirror.bar_payloads.back().slots[0].slot_index == 4 && items::guid_hex(h.mirror.bar_payloads.back().slots[0].use.item_guid) == guid_n(204),
          "the client is told of that one bar slot with ClientReceiveActionbarSlotPayload");
    CHECK(client_bar_is(h, h.s->action_bar()), "and the client's bar is the session's");
}

void test_stack_session_action_bar_kept() {
    std::printf("a split, a whole move, a swap and a drag onto the linked stack keep the action bar\n");
    // a split: the linked stack keeps its guid
    {
        std::vector<svc::InventoryItem> items = stack_worn_items();
        items.push_back(stack_potion(1, 201, 14));
        Harness h(character_with_linked_potion(items, guid_n(201)));
        h.request(client_load_rpc(), h.pc_ch);
        const size_t payloads = h.mirror.bar_payloads.size();
        h.request(bag_move(h, repl::StorageEventType::Move, 1, 5, guid_n(201), 6), h.pc_ch);
        const repl::ActionbarUseData* slot = h.s->action_bar().at(0, 4);
        CHECK(h.s->inventory().at(Bag, 1) && h.s->inventory().at(Bag, 1)->quantity == 9 && slot && items::guid_hex(slot->item_guid) == guid_n(201) &&
                  h.mirror.bar_payloads.size() == payloads,
              "a split of the linked stack: it keeps its guid, so the bar slot is unchanged and no bar payload is sent");
    }
    // a whole move: the guid moves with the stack
    {
        std::vector<svc::InventoryItem> items = stack_worn_items();
        items.push_back(stack_potion(1, 201, 7));
        Harness h(character_with_linked_potion(items, guid_n(201)));
        h.request(client_load_rpc(), h.pc_ch);
        const size_t payloads = h.mirror.bar_payloads.size();
        h.request(bag_move(h, repl::StorageEventType::Move, 1, 7, guid_n(201), 6), h.pc_ch);
        const repl::ActionbarUseData* slot = h.s->action_bar().at(0, 4);
        CHECK(h.s->inventory().at(Bag, 6) && h.s->inventory().at(Bag, 6)->instance_guid == guid_n(201) && slot && items::guid_hex(slot->item_guid) == guid_n(201) &&
                  h.mirror.bar_payloads.size() == payloads,
              "a whole move of the linked stack: every guid is kept, so the bar slot is unchanged and no bar payload is sent");
    }
    // a swap: both guids are kept
    {
        std::vector<svc::InventoryItem> items = stack_worn_items();
        items.push_back(stack_potion(1, 201, 7));
        items.push_back(make_item(Bag, 2, fi::kRingA, 202));
        Harness h(character_with_linked_potion(items, guid_n(201)));
        h.request(client_load_rpc(), h.pc_ch);
        const size_t payloads = h.mirror.bar_payloads.size();
        h.request(bag_move(h, repl::StorageEventType::Swap, 1, 7, guid_n(201), 2, guid_n(202), 1), h.pc_ch);
        const repl::ActionbarUseData* slot = h.s->action_bar().at(0, 4);
        CHECK(h.s->inventory().at(Bag, 2) && h.s->inventory().at(Bag, 2)->instance_guid == guid_n(201) && slot && items::guid_hex(slot->item_guid) == guid_n(201) &&
                  h.mirror.bar_payloads.size() == payloads,
              "a swap of the linked stack: every guid is kept, so the bar slot is unchanged and no bar payload is sent");
    }
    // a drag onto the linked stack: the linked stack is the target and keeps its guid
    {
        std::vector<svc::InventoryItem> items = stack_worn_items();
        items.push_back(stack_potion(1, 201, 6));
        items.push_back(stack_potion(4, 204, 14));
        Harness h(character_with_linked_potion(items, guid_n(204)));
        h.request(client_load_rpc(), h.pc_ch);
        const size_t payloads = h.mirror.bar_payloads.size();
        h.request(bag_move(h, repl::StorageEventType::Move, 1, 6, guid_n(201), 4, guid_n(204), 6), h.pc_ch);
        const repl::ActionbarUseData* slot = h.s->action_bar().at(0, 4);
        CHECK(h.s->inventory().at(Bag, 4) && h.s->inventory().at(Bag, 4)->quantity == 20 && slot && items::guid_hex(slot->item_guid) == guid_n(204) &&
                  h.mirror.bar_payloads.size() == payloads,
              "a drag onto the linked stack: it keeps its guid, so the bar slot is unchanged and no bar payload is sent");
    }
}

void test_stack_session_removed_toast_count() {
    std::printf("the Removed toast of part of a stack says the count that left\n");
    std::vector<svc::InventoryItem> items = stack_worn_items();
    items.push_back(stack_potion(1, 201, 7));
    Harness h(record_with(items));
    MoveSpec destroy;
    destroy.type = static_cast<uint8_t>(repl::StorageEventType::Remove);
    destroy.from_object = destroy.to_object = h.s->bootstrap().setup().guids.backpack;
    destroy.from_slot = 1;
    destroy.from_count = 2;
    destroy.to_slot = 1;
    destroy.to_count = 2;
    destroy.source_guid = destroy.event_guid = destroy.target_guid = guid_n(201);
    const size_t events = h.mirror.events.size();
    h.request(storage_move(destroy), h.pc_ch);
    CHECK(h.mirror.events.size() == events + 1 && h.s->inventory().at(Bag, 1) && h.s->inventory().at(Bag, 1)->quantity == 5, "(two of the seven were destroyed)");
    const repl::ItemStorageEvent& removed = h.mirror.events.back();
    CHECK(removed.callout && removed.callout->message.find("x2") != std::string::npos && removed.callout->message.find("x7") == std::string::npos,
          "the Removed toast says the count that left (x2), not the whole stack (x7)");
}

void test_stack_payload_after_a_top_up() {
    std::printf("the item bag's payload after a join holds the whole stack\n");
    std::vector<svc::InventoryItem> items = stack_worn_items();
    items.push_back(stack_potion(1, 201, 7));
    items.push_back(stack_potion(4, 204, 14));
    Harness h(record_with(items));
    const bs::SessionGuids& g = h.s->bootstrap().setup().guids;
    // drag the 7 onto the 14: the client names 6 (the room)
    h.request(bag_move(h, repl::StorageEventType::Move, 1, 6, guid_n(201), 4, guid_n(204), 6), h.pc_ch);
    CHECK(h.s->inventory().at(Bag, 4) && h.s->inventory().at(Bag, 4)->quantity == 20 && h.s->inventory().at(Bag, 1) && h.s->inventory().at(Bag, 1)->quantity == 1,
          "(the join made the target 20 and left 1 in the source)");
    // the client asks for the item bag again (ServerRetrieveStoragePayload)
    const size_t payloads = h.mirror.payload_data.size();
    h.request(stack_retrieve_payload(g.backpack), h.pc_ch);
    CHECK(h.mirror.payload_data.size() == payloads + 1 && h.mirror.payload_data.back().target == g.backpack, "the item bag's payload came again");
    CHECK(h.mirror.payload_data.size() == payloads + 1 && stack_payload_is_inventory(h.mirror.payload_data.back(), h.s->inventory(), Bag),
          "every slot of the payload is the inventory's: the joined stack is 20, not the 6 that were added");
}

void test_stack_session_drag_join_then_split() {
    std::printf("a stack joins another, then a stack splits, then the piece joins again\n");
    std::vector<svc::InventoryItem> items = stack_worn_items();
    items.push_back(stack_potion(1, 201, 7));
    items.push_back(stack_potion(4, 204, 14));
    items.push_back(stack_potion(5, 205, 14));
    Harness h(record_with(items));
    const bs::SessionGuids& g = h.s->bootstrap().setup().guids;
    const std::string source = guid_n(201), target = guid_n(204), splitting = guid_n(205);
    // drag the 7 onto the 14: the client names 6 (the room), the target's guid and 6 in the target's slot
    const size_t events = h.mirror.events.size();
    h.request(bag_move(h, repl::StorageEventType::Move, 1, 6, source, 4, target, 6), h.pc_ch);
    const svc::InventoryItem* joined = h.s->inventory().at(Bag, 4);
    const svc::InventoryItem* kept = h.s->inventory().at(Bag, 1);
    CHECK(joined && joined->quantity == 20 && joined->instance_guid == target && joined->quality == 1 && kept && kept->quantity == 1 && kept->instance_guid == source,
          "the 7 dragged onto the 14: the target is 20 with its own guid and quality, the source keeps 1");
    CHECK(h.mirror.events.size() == events + 2, "the answer is two events");
    if (h.mirror.events.size() == events + 2) {
        const repl::ItemStorageEvent& rem = h.mirror.events[events];
        const repl::ItemStorageEvent& add = h.mirror.events[events + 1];
        CHECK(rem.type == static_cast<uint8_t>(repl::StorageEventType::Remove) && rem.targets[0].object == g.backpack && rem.targets[0].slots[0].slot == 1 &&
                  rem.targets[0].slots[0].count == 6 && items::guid_hex(rem.item.guid) == source && rem.item.quantity == 7,
              "a Remove of 6 from the source slot, naming the stack as it was (7)");
        CHECK(add.type == static_cast<uint8_t>(repl::StorageEventType::Add) && add.targets[0].object == g.backpack && add.targets[0].slots[0].slot == 4 &&
                  add.targets[0].slots[0].count == 6 && items::guid_hex(add.item.guid) == target && add.item.quantity == 6 && add.item.quality == 1,
              "an Add of 6 into the target's slot, with the target's guid and quality");
    }
    CHECK(mirror_matches(h), "the client's bag is the server's after the join");
    // right-click split the other 14: 5 to the free slot 6, 9 stay
    const size_t split_events = h.mirror.events.size();
    h.request(bag_move(h, repl::StorageEventType::Move, 5, 5, splitting, 6, std::string(), 5), h.pc_ch);
    const svc::InventoryItem* left = h.s->inventory().at(Bag, 5);
    const svc::InventoryItem* piece = h.s->inventory().at(Bag, 6);
    CHECK(left && left->quantity == 9 && left->instance_guid == splitting, "the split: the source keeps 9 and its own guid");
    CHECK(piece && piece->quantity == 5 && piece->quality == 1 && piece->instance_guid != splitting && items::guid_of(piece->instance_guid).has_value(),
          "the piece is a new stack of 5 with a new guid and the source's quality, in the free slot");
    CHECK(h.mirror.events.size() == split_events + 2, "the split's answer is two events");
    if (h.mirror.events.size() == split_events + 2 && piece) {
        const repl::ItemStorageEvent& rem = h.mirror.events[split_events];
        const repl::ItemStorageEvent& add = h.mirror.events[split_events + 1];
        CHECK(rem.type == static_cast<uint8_t>(repl::StorageEventType::Remove) && rem.targets[0].slots[0].slot == 5 && rem.targets[0].slots[0].count == 5 &&
                  items::guid_hex(rem.item.guid) == splitting && rem.item.quantity == 14,
              "a Remove of 5 from the source slot, naming the stack as it was (14)");
        CHECK(add.type == static_cast<uint8_t>(repl::StorageEventType::Add) && add.targets[0].slots[0].slot == 6 && add.targets[0].slots[0].count == 5 &&
                  items::guid_hex(add.item.guid) == piece->instance_guid && add.item.quantity == 5,
              "an Add of the new 5 in the slot the client named");
    }
    CHECK(mirror_matches(h), "the client's bag is the server's after the split");
    // drag the new 5 back onto the 9: 14 in the 9's slot
    const std::string fresh = piece ? piece->instance_guid : std::string();
    h.request(bag_move(h, repl::StorageEventType::Move, 6, 5, fresh, 5, splitting, 5), h.pc_ch);
    CHECK(h.s->inventory().at(Bag, 5) && h.s->inventory().at(Bag, 5)->quantity == 14 && h.s->inventory().at(Bag, 5)->instance_guid == splitting &&
              !h.s->inventory().at(Bag, 6) && mirror_matches(h),
          "the new 5 joins the 9 again: 14 in the 9's slot with its guid, and the client agrees");
}

void test_stack_session_join_rules() {
    std::printf("the join's rules on the wire: the colour, the room, a split's empty slot and the state\n");
    const auto character_of = [](int target_quality, int source_quality, int32_t target_count, int32_t source_count, uint32_t target_n, uint32_t source_n,
                                 float target_durability = 11.0f, float source_durability = 11.0f) {
        std::vector<svc::InventoryItem> items = stack_worn_items();
        items.push_back(stack_potion(4, target_n, target_count, target_quality, target_durability));
        items.push_back(stack_potion(1, source_n, source_count, source_quality, source_durability));
        return record_with(items);
    };
    // qualities 20 and 25 are both Rare: 6 of 7 joins 14 and the target keeps quality 20
    {
        Harness h(character_of(20, 25, 14, 7, 204, 201));
        const size_t events = h.mirror.events.size();
        h.request(bag_move(h, repl::StorageEventType::Move, 1, 6, guid_n(201), 4, guid_n(204), 6), h.pc_ch);
        const svc::InventoryItem* target = h.s->inventory().at(Bag, 4);
        CHECK(target && target->quantity == 20 && target->quality == 20 && target->instance_guid == guid_n(204), "quality 25 joins quality 20: the stack is 20 at quality 20 and the target's guid");
        CHECK(h.mirror.events.size() == events + 2 && h.mirror.events[events + 1].item.quality == 20, "...and the Add says quality 20");
    }
    // a Move of 7 over the room of 6 is refused whole
    {
        Harness h(character_of(20, 25, 14, 7, 204, 201));
        const items::Inventory before = h.s->inventory();
        CHECK(refused_cleanly(h, before, "a Move of 7 over the room", bag_move(h, repl::StorageEventType::Move, 1, 7, guid_n(201), 4, guid_n(204), 7), h.pc_ch),
              "a Move of 7 over the room of 6 is refused, both stacks unchanged");
    }
    // qualities 20 and 10 are different colours: the Move is refused, the client's Swap exchanges the stacks
    {
        Harness h(character_of(20, 10, 14, 7, 204, 201));
        const items::Inventory before = h.s->inventory();
        CHECK(refused_cleanly(h, before, "a Move between colours", bag_move(h, repl::StorageEventType::Move, 1, 6, guid_n(201), 4, guid_n(204), 14), h.pc_ch),
              "a Move of 6 (the room) between two colours is refused");
        const size_t events = h.mirror.events.size();
        h.request(bag_move(h, repl::StorageEventType::Swap, 1, 7, guid_n(201), 4, guid_n(204), 14), h.pc_ch);
        CHECK(h.mirror.events.size() == events + 4, "the client's Swap of the two colours is four events");
        CHECK(h.s->inventory().at(Bag, 4) && h.s->inventory().at(Bag, 4)->instance_guid == guid_n(201) && h.s->inventory().at(Bag, 4)->quality == 10 &&
                  h.s->inventory().at(Bag, 1) && h.s->inventory().at(Bag, 1)->instance_guid == guid_n(204) && h.s->inventory().at(Bag, 1)->quality == 20,
              "the two whole stacks changed places");
    }
    // a split names a free slot: a Move of 5 onto the same item and colour in an occupied slot is refused
    {
        Harness h(character_of(20, 20, 14, 7, 204, 201));
        const items::Inventory before = h.s->inventory();
        CHECK(refused_cleanly(h, before, "a split onto an occupied slot", bag_move(h, repl::StorageEventType::Move, 1, 5, guid_n(201), 4), h.pc_ch),
              "a split whose target slot is occupied (the same item and colour) is refused, nothing changes");
    }
    // a stack of one has no split below it: a count of 0 is refused, and its one item is a plain move
    {
        Harness h(character_of(20, 20, 14, 1, 204, 201));
        const items::Inventory before = h.s->inventory();
        CHECK(refused_cleanly(h, before, "a split of a stack of one", bag_move(h, repl::StorageEventType::Move, 1, 0, guid_n(201), 6), h.pc_ch),
              "a split of a stack of one (a count of 0) is refused");
        h.request(bag_move(h, repl::StorageEventType::Move, 1, 1, guid_n(201), 6), h.pc_ch);
        CHECK(h.s->inventory().at(Bag, 6) && h.s->inventory().at(Bag, 6)->quantity == 1 && h.s->inventory().at(Bag, 6)->instance_guid == guid_n(201) && !h.s->inventory().at(Bag, 1),
              "its one item still moves whole as a plain move, with the same guid");
    }
    // the same record and colour with another durability is another state: no join
    {
        Harness h(character_of(20, 20, 14, 7, 204, 201, 10.0f, 11.0f));
        const items::Inventory before = h.s->inventory();
        CHECK(refused_cleanly(h, before, "a join with another durability", bag_move(h, repl::StorageEventType::Move, 1, 6, guid_n(201), 4, guid_n(204), 6), h.pc_ch),
              "the same record and colour with another durability does not join");
    }
}

void test_stack_retrieve_storage_payload() {
    std::printf("the client's ServerRetrieveStoragePayload is answered with that container's payload\n");
    Harness h;
    const bs::SessionGuids& g = h.s->bootstrap().setup().guids;
    const size_t payloads = h.mirror.payloads;
    const size_t targets = h.mirror.payload_targets.size();
    h.request(stack_retrieve_payload(g.backpack), h.pc_ch);
    CHECK(h.mirror.payloads == payloads + 1 && h.mirror.payload_targets.size() == targets + 1 && h.mirror.payload_targets.back() == g.backpack,
          "a request for the item bag sends its storage payload again");
    // an unknown container: nothing is sent, the request is logged
    const std::string log_before = g_log.str();
    const size_t payloads_after = h.mirror.payloads;
    h.request(stack_retrieve_payload(emit::FIntrepidNetworkGUID{777777, 60, 5}), h.pc_ch);
    const std::string said = g_log.str().substr(log_before.size());
    CHECK(h.mirror.payloads == payloads_after, "a request for a container worldd does not know sends nothing");
    CHECK(said.find("ServerRetrieveStoragePayload names a container") != std::string::npos || said.find("names a container") != std::string::npos,
          "...and says so in the log");
}

void test_stack_session_move_to_free_slot() {
    std::printf("a whole move to a free slot names no item or the moved stack's own instance\n");
    // the target names the moved stack's own guid: the whole stack moves
    {
        std::vector<svc::InventoryItem> items = stack_worn_items();
        items.push_back(stack_potion(4, 204, 7));
        Harness h(record_with(items));
        MoveSpec m = potion_move(h);
        m.from_slot = 4;
        m.from_count = 7;
        m.source_guid = guid_n(204);
        m.event_guid = guid_n(204);
        m.to_slot = 5;
        m.to_count = 7;
        m.target_guid = guid_n(204);
        const size_t events = h.mirror.events.size();
        h.request(storage_move(m), h.pc_ch);
        const svc::InventoryItem* moved = h.s->inventory().at(Bag, 5);
        CHECK(moved && moved->instance_guid == guid_n(204) && moved->quantity == 7 && !h.s->inventory().at(Bag, 4),
              "a Move to the empty slot 5 that names the stack's own guid is accepted: the stack is in slot 5 with its guid and 7, slot 4 empty");
        CHECK(h.mirror.events.size() == events + 2 && mirror_matches(h), "... and the client is told, its bag is the server's");
    }
    // the target names another item's guid: the move is refused whole
    {
        std::vector<svc::InventoryItem> items = stack_worn_items();
        items.push_back(stack_potion(1, 201, 3));
        items.push_back(stack_potion(4, 204, 7));
        Harness h(record_with(items));
        const items::Inventory before = h.s->inventory();
        MoveSpec m = potion_move(h);
        m.from_slot = 4;
        m.from_count = 7;
        m.source_guid = guid_n(204);
        m.event_guid = guid_n(204);
        m.to_slot = 5;
        m.to_count = 7;
        m.target_guid = guid_n(201);
        CHECK(refused_cleanly(h, before, "a Move to a free slot naming another item", storage_move(m), h.pc_ch),
              "a Move to the empty slot 5 that names another item's guid is refused, nothing changes");
    }
}
AOC_REGISTER_TEST(test_stack_session_drag_join_then_split);
AOC_REGISTER_TEST(test_stack_session_join_rules);
AOC_REGISTER_TEST(test_stack_payload_after_a_top_up);
AOC_REGISTER_TEST(test_stack_session_action_bar_follows_a_join);
AOC_REGISTER_TEST(test_stack_session_action_bar_kept);
AOC_REGISTER_TEST(test_stack_session_removed_toast_count);
AOC_REGISTER_TEST(test_stack_session_move_to_free_slot);
AOC_REGISTER_TEST(test_stack_retrieve_storage_payload);
