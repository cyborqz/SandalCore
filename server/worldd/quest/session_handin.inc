// ============================================================================
//  quest/session_handin.inc
//
//  Quests part 3, slice 2 (the item hand-in to a person): the player's answer to the hand-in panel.  Included at the end of session/player_session.cpp.
//
//  A TurnIn node of a conversation (dialogue/talk.h) queues the hand-in panel (ESubDialogueType ItemTurnIn) with the node's quest and the TurnIn objectives of it that are active.  The client
//  lists those objectives (UMayoralCommissionsDeliveryMenu::SetTurnInFromSubDialogue), the player picks items for them from the bags, and the dialogue widget sends ONE call
//  (UDialogueWidget::HandleItemTurnInConfirmed): ServerRequestItemDelivery(QuestRecordId, DeliveryItems).  A delivery is, for each objective, the quest instance's guid (ContextGuid), the
//  objective's record and the items picked: the item record, copies of the picked item instances and, for each picked stack, its instance guid, the quantity taken from it and where it lies.
//
//  The game's server body of the call is not in the client's exe (the slot holds an empty stub), so what the server does is modelled on what the client sends and expects; each point below is a modelling choice:
//    * the call is taken only while a hand-in panel is open and names the panel's quest; each objective of it must be one the panel lists and active, and each item one the objective's
//      filter names (a filter by tags or rarities is not served);
//    * every stack the call names must be in the bag it says (the bag or the quest bag), hold that item and hold the quantity; a stack cannot be named twice beyond what it holds;
//      one stack that does not check refuses the whole delivery (nothing is taken, the panel stays open);
//    * an objective takes no more than it still needs (its CompletionCount less its count): the rest of what was picked stays with the character;
//    * the items leave the bags, then the objective counts them (and raises the quest variable it names), then the panel ends and the talk goes on to the node the TurnIn names (none: it ends).
//  The client sends nothing else for a hand-in and expects nothing but the changes: the bags' events, the quest component and the conversation.
// ============================================================================

namespace {

/// One stack the client picked for a hand-in.
struct HandInPick {
    int64_t            item = 0;
    svc::ItemContainer container = svc::ItemContainer::Backpack;
    std::string        guid;   // the instance guid as the bag holds it (hex)
    int32_t            quantity = 0;
};

/// The picks for one objective.
struct HandInRow {
    int64_t                 objective = 0;
    size_t                  index = 0;   // of the objective in the quest
    std::vector<HandInPick> picks;
    int64_t                 offered = 0, take = 0;
};

/// The local handle of the element command named `path` of the RPC (0: none).
uint16_t handin_handle(const schema::RpcSchema& rpc, std::string_view path) {
    for (const schema::RepCommand& c : rpc.element_commands) {
        if (path == c.path) return c.handle;
    }
    return 0;
}

const repl::Value* handin_value(const repl::ElementState& e, uint16_t handle) {
    if (handle == 0) return nullptr;
    const auto it = e.values.find(handle);
    return it == e.values.end() ? nullptr : &it->second;
}

std::optional<svc::ItemContainer> handin_container(int64_t location) {
    if (location == static_cast<int64_t>(repl::ItemLocation::Backpack)) return svc::ItemContainer::Backpack;
    if (location == static_cast<int64_t>(repl::ItemLocation::Quest)) return svc::ItemContainer::Quest;
    return std::nullopt;
}

}  // namespace

void PlayerSession::on_item_delivery(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const auto refuse = [&](const std::string& why) {
        ++hand_ins_refused_;
        spdlog::log(level_for(quests_logged_, 100), "[session] {}: a hand-in is refused: {}", name(), why);
    };
    const dialogue::Book* book = world_ ? world_->dialogue() : nullptr;
    const quest::QuestBook* qb = world_ ? world_->quests() : nullptr;
    if (!quests_on_ || !book || !qb) return refuse("quests are off");
    if (dead_) return refuse("the player is dead");
    if (!talk_.open() || !talk_.sub() || talk_.sub()->type != 3) return refuse("no hand-in panel is open");
    if (!rpc.call.decoded || !rpc.call.rpc) return refuse("its parameters do not decode");
    if (!within_budget(dialogue_budget_, "ServerRequestItemDelivery", now)) return;
    if (!room_for(6)) return;
    const dialogue::Talk::Sub sub = *talk_.sub();
    const int64_t quest = quest_int_value(quest_rpc_value(rpc, "QuestRecordId"));
    if (quest != sub.quest) return refuse("it names quest " + std::to_string(quest) + ", the panel is for quest " + std::to_string(sub.quest));
    const world::WorldActor* npc = registry_ ? registry_->find(talk_.state().source) : nullptr;
    if (!npc) return end_dialogue(conn, "the NPC is gone");
    const GoodPosition at = checker_.good();
    if (std::hypot(npc->x - at.x, npc->y - at.y) > kDialogueRangeCm) return end_dialogue(conn, "the player is too far from the NPC");
    const content::QuestRow* def = qb->quest(sub.quest);
    const quest::QuestInstance* instance = quest_log_.find(sub.quest);
    if (!def || !instance || instance->status != quest::Status::Active) return refuse("the quest is not active");

    // ---- read the call ----
    const schema::RpcSchema& schema_rpc = *rpc.call.rpc;
    const repl::Value* deliveries = quest_rpc_value(rpc, "DeliveryItems");
    const repl::ArrayValue* list = deliveries ? deliveries->get_if<repl::ArrayValue>() : nullptr;
    if (!list || list->elements.empty() || list->elements.size() > 16) return refuse(!list ? "it carries no deliveries" : list->elements.empty() ? "it delivers nothing" : "it names too many objectives");
    const uint16_t h_objective = handin_handle(schema_rpc, "DeliveryItems.ObjectiveId"), h_items = handin_handle(schema_rpc, "DeliveryItems.Items");
    const uint16_t h_context[4] = {handin_handle(schema_rpc, "DeliveryItems.ContextGuid.A"), handin_handle(schema_rpc, "DeliveryItems.ContextGuid.B"),
                                   handin_handle(schema_rpc, "DeliveryItems.ContextGuid.C"), handin_handle(schema_rpc, "DeliveryItems.ContextGuid.D")};
    const uint16_t h_record = handin_handle(schema_rpc, "Items.ItemRecordId"), h_stacks = handin_handle(schema_rpc, "Items.ItemQuantityAndLocations");
    const uint16_t h_guid[4] = {handin_handle(schema_rpc, "ItemQuantityAndLocations.FItemInstanceGuid.A"), handin_handle(schema_rpc, "ItemQuantityAndLocations.FItemInstanceGuid.B"),
                                handin_handle(schema_rpc, "ItemQuantityAndLocations.FItemInstanceGuid.C"), handin_handle(schema_rpc, "ItemQuantityAndLocations.FItemInstanceGuid.D")};
    const uint16_t h_quantity = handin_handle(schema_rpc, "ItemQuantityAndLocations.Quantity"), h_location = handin_handle(schema_rpc, "ItemQuantityAndLocations.InventoryLocated");
    if (!h_objective || !h_items || !h_record || !h_stacks || !h_quantity || !h_location || !h_guid[0] || !h_guid[1] || !h_guid[2] || !h_guid[3]) return refuse("the call's layout is not the one this build reads");

    std::vector<HandInRow> rows;
    std::map<std::string, int64_t> claimed;   // per stack: what the call takes from it so far
    for (const repl::ElementState& d : list->elements) {
        HandInRow row;
        row.objective = quest_int_value(handin_value(d, h_objective));
        if (std::find(sub.objectives.begin(), sub.objectives.end(), row.objective) == sub.objectives.end()) return refuse("objective " + std::to_string(row.objective) + " is not one the panel lists");
        bool known = false;
        for (const HandInRow& other : rows) known = known || other.objective == row.objective;
        if (known) return refuse("objective " + std::to_string(row.objective) + " is named twice");
        size_t index = def->objectives.size();
        for (size_t i = 0; i < def->objectives.size(); ++i) {
            if (def->objectives[i].guid == row.objective) index = i;
        }
        if (index >= def->objectives.size() || index >= instance->objectives.size()) return refuse("objective " + std::to_string(row.objective) + " is not in the quest");
        row.index = index;
        const content::QuestObjectiveRow& r = def->objectives[index];
        const quest::ObjectiveState& state = instance->objectives[index];
        if (r.type != "TurnIn" || r.crate || !state.active || state.completed || state.failed) return refuse("objective " + r.name + " takes no items now");
        if (r.items.empty()) {
            if (quest_told_.insert("hand-in filter " + r.name).second) spdlog::info("[session] {}: the hand-in objective {} filters its items by tags or rarities, which this build does not serve (told once)", name(), r.name);
            return refuse("objective " + r.name + " filters by tags: not served");
        }
        const repl::Value* items = handin_value(d, h_items);
        const repl::ArrayValue* item_list = items ? items->get_if<repl::ArrayValue>() : nullptr;
        if (!item_list || item_list->elements.size() > 32) return refuse("objective " + r.name + " carries no items or too many rows");
        for (const repl::ElementState& it : item_list->elements) {
            const int64_t record = quest_int_value(handin_value(it, h_record));
            if (std::find(r.items.begin(), r.items.end(), record) == r.items.end()) return refuse("item " + std::to_string(record) + " is not one objective " + r.name + " takes");
            const repl::Value* stacks = handin_value(it, h_stacks);
            const repl::ArrayValue* stack_list = stacks ? stacks->get_if<repl::ArrayValue>() : nullptr;
            if (!stack_list || stack_list->elements.size() > 64) return refuse("item " + std::to_string(record) + " names no stack or too many");
            for (const repl::ElementState& s : stack_list->elements) {
                HandInPick pick;
                pick.item = record;
                pick.quantity = static_cast<int32_t>(quest_int_value(handin_value(s, h_quantity)));
                const auto container = handin_container(quest_int_value(handin_value(s, h_location)));
                if (!container) return refuse("a stack lies where a hand-in does not look (location " + std::to_string(quest_int_value(handin_value(s, h_location))) + ")");
                pick.container = *container;
                pick.guid = items::guid_hex(repl::GuidValue{static_cast<uint32_t>(quest_int_value(handin_value(s, h_guid[0]))), static_cast<uint32_t>(quest_int_value(handin_value(s, h_guid[1]))),
                                                            static_cast<uint32_t>(quest_int_value(handin_value(s, h_guid[2]))), static_cast<uint32_t>(quest_int_value(handin_value(s, h_guid[3])))});
                if (pick.quantity <= 0) return refuse("a stack is named with no quantity");
                const svc::InventoryItem* held = inventory_.find_guid(pick.guid);   // (points into the live inventory: items() hands out a copy that is gone after its loop)
                if (held && held->container != pick.container) held = nullptr;
                if (!held) return refuse("a stack the character does not carry (guid " + pick.guid + ")");
                if (held->item_record_id != record) return refuse("a stack holds item " + std::to_string(held->item_record_id) + ", the call says " + std::to_string(record));
                int64_t& used = claimed[pick.guid];
                used += pick.quantity;
                if (used > held->quantity) return refuse("a stack is asked for " + std::to_string(used) + ", it holds " + std::to_string(held->quantity));
                row.offered += pick.quantity;
                row.picks.push_back(std::move(pick));
            }
        }
        if (row.offered > 0) rows.push_back(std::move(row));
    }
    if (rows.empty()) return refuse("it delivers no item");

    // ---- what each objective takes, and the items that leave ----
    std::vector<std::pair<int64_t, int64_t>> leaving;   // item record, quantity
    int64_t total_take = 0;
    for (HandInRow& row : rows) {
        const content::QuestObjectiveRow& r = def->objectives[row.index];
        const int64_t room = std::max<int64_t>(0, r.count - instance->objectives[row.index].count);
        row.take = std::min(row.offered, room);
        int64_t left = row.take;
        for (const HandInPick& p : row.picks) {
            if (left <= 0) break;
            const int64_t part = std::min<int64_t>(p.quantity, left);
            left -= part;
            bool added = false;
            for (auto& [record, quantity] : leaving) {
                if (record == p.item) {
                    quantity += part;
                    added = true;
                }
            }
            if (!added) leaving.emplace_back(p.item, part);
        }
        total_take += row.take;
    }
    if (total_take <= 0) return refuse("the objectives take no more items");

    // ---- carry it out: the items leave, the objectives count them, the panel ends ----
    if (h_context[0] && h_context[1] && h_context[2] && h_context[3]) {   // (the quest instance's guid the client holds: the quest record decides, a difference is only told)
        const repl::ElementState& first = list->elements.front();
        quest::Guid128 context;
        context.a = static_cast<uint32_t>(quest_int_value(handin_value(first, h_context[0])));
        context.b = static_cast<uint32_t>(quest_int_value(handin_value(first, h_context[1])));
        context.c = static_cast<uint32_t>(quest_int_value(handin_value(first, h_context[2])));
        context.d = static_cast<uint32_t>(quest_int_value(handin_value(first, h_context[3])));
        if (!context.zero() && !(context == instance->guid) && quest_told_.insert("hand-in context").second) {
            spdlog::info("[session] {}: a hand-in names the quest instance {} and the log holds {}: the quest record decides (told once)", name(), context.hex(), instance->guid.hex());
        }
    }
    ++hand_ins_;
    hand_in_items_ += static_cast<size_t>(total_take);
    spdlog::log(level_for(quests_logged_, 100), "[session] {}: hands {} item(s) in to {} for quest {} ({} objective(s))", name(), total_take, npc->npc.name, def->name, rows.size());
    take_quest_items(conn, leaving);
    int64_t counted = 0;
    quest_engine(conn, now, "items were handed in", [&](quest::Engine& eng) {
        for (const HandInRow& row : rows) counted += eng.turn_in(sub.quest, row.objective, row.take);
    });
    if (counted != total_take) spdlog::warn("[session] {}: the hand-in for quest {} took {} item(s) out of the bags and the objectives counted {}", name(), def->name, total_take, counted);
    const auto facts = dialogue_facts(npc);
    dialogue::Evaluator ev(*book, *facts, [this](const std::string& what) {
        if (dialogue_told_.insert(what).second) spdlog::info("[session] {}: a condition of a conversation could not be answered and does not hold: {} (told once)", name(), what);
    });
    const dialogue::Result r = talk_.finish_sub(*book, ev, /*taken=*/true);
    run_talk_actions(conn, r, *npc, now);   // what entering the node after the hand-in does
    if (r.outcome == dialogue::Outcome::Changed) {
        ++dialogue_steps_;
        if (!talk_.open()) ++dialogue_closes_;
        send_dialogue(conn, "items were handed in", /*exhaust=*/true);
    }
}
