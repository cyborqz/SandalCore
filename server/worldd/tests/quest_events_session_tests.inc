// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/quest_events_session_tests.inc: what the character does that a quest counts (quests milestone, the first tasks)
//  (registered with AOC_REGISTER_TEST: test_worldd_area_session.cpp runs them through aoc_tests::run; included from area_session_registered.inc after destiny_session_tests.inc, whose
//  harness and data it uses)
//
//    the engine      a used ability counts for the objectives that list it, in a fight for those that say so; a kill counts by instance, by set and by tag query (a query a creature's
//                    tags do not satisfy counts nothing); an item put on counts for Equip and items sold count for Sell, for the listed items only; each finished task pays its
//                    reward once and raises its track's count
//    the session     a Sprint, a Dodge, an item put on count through the real paths (the client's trigger and item calls): a refused ability counts nothing, a Dodge outside a fight
//                    counts nothing; a finished task raises the count the window is told, and the client is told again without asking
//    the drops       a kill hands out the quest item a creature drops for an active objective of any kind, a Collect objective's too, when no Kill objective is active (engine and a real kill)
// ============================================================================
#include <set>

namespace quest_events_test {

constexpr int64_t kReward = 6064639900000000001ll;   // a reward table the finished tasks name

inline repl::CodecOptions client_options() {
    repl::CodecOptions o;
    o.inline_objects = true;
    return o;
}

inline content::QuestAction toast() {
    content::QuestAction a;
    a.action = "Toast";
    a.text = "Destiny Objective Complete";
    return a;
}

/// A single-objective task quest of the kind `type`, as the Destiny tasks are: it names a reward table and shows the toast when its objective is done.
inline content::QuestRow task_quest(int64_t guid, const std::string& name, int64_t objective, const char* type, int64_t count) {
    content::QuestRow q = destiny_test::quest(guid, name);
    q.rewards = {kReward};
    content::QuestObjectiveRow o;
    o.guid = objective;
    o.name = name + "_Complete";
    o.type = type;
    o.count = count;
    o.awake = true;
    o.on_success.push_back(toast());
    q.objectives.push_back(std::move(o));
    return q;
}

inline content::QuestRow& objective_quest(content::QuestData& d, int64_t quest) { return d.quests.at(quest); }

/// The Destiny data with First Steps' tasks 1 to 5 made of the new kinds: Sprint (an ability), Dodge (an ability, 3, in a fight only), Equip, Kill by a tag query (2 kills) and Sell.
inline QuestTestData data() {
    namespace dt = destiny_test;
    QuestTestData out = dt::data();
    content::QuestData& d = out.quests;
    const auto put = [&](int i, content::QuestRow q) {
        q.rewards = {kReward};
        d.quests[dt::task(0, i)] = std::move(q);
    };
    {
        content::QuestRow q = task_quest(dt::task(0, 1), "Task_0_1", dt::task_objective(0, 1), "UseAbility", 1);
        q.objectives[0].abilities = {content::kGuidSprint};
        put(1, std::move(q));
    }
    {
        content::QuestRow q = task_quest(dt::task(0, 2), "Task_0_2", dt::task_objective(0, 2), "UseAbility", 3);
        q.objectives[0].abilities = {content::kGuidDodge};
        q.objectives[0].in_combat = true;
        put(2, std::move(q));
    }
    {
        content::QuestRow q = task_quest(dt::task(0, 3), "Task_0_3", dt::task_objective(0, 3), "Collect", 1);
        q.objectives[0].collect_type = "Equip";
        q.objectives[0].items = {fi::kSword2H};
        put(3, std::move(q));
    }
    {
        content::QuestRow q = task_quest(dt::task(0, 4), "Task_0_4", dt::task_objective(0, 4), "Kill", 2);
        q.objectives[0].tag_query = true;
        auto query = std::make_shared<content::TagQuery>();
        query->present = true;
        query->tok = {0, 1, 1, 1, 0};
        query->tags = {"character"};
        q.objectives[0].kill_query = query;
        put(4, std::move(q));
    }
    {
        content::QuestRow q = task_quest(dt::task(0, 5), "Task_0_5", dt::task_objective(0, 5), "Collect", 1);
        q.objectives[0].collect_type = "Sell";
        q.objectives[0].items = {fi::kPotion};
        put(5, std::move(q));
    }
    for (const auto& [guid, q] : d.quests) d.by_name[q.name] = guid;
    return out;
}

inline std::shared_ptr<content::TagQuery> query_of(std::vector<std::string> tags) {
    auto q = std::make_shared<content::TagQuery>();
    q->present = true;
    q->tok = {0, 1, 1, static_cast<int64_t>(tags.size())};
    for (size_t i = 0; i < tags.size(); ++i) q->tok.push_back(static_cast<int64_t>(i));
    q->tags = std::move(tags);
    return q;
}

}  // namespace quest_events_test

/// ServerTriggerAbility for an ability of the pawn's ability component, as the client sends it for a plain press.
static BunchWriter qe_trigger(const emit::FIntrepidNetworkGUID& ability, int64_t guid, int32_t id) {
    const schema::ClassSchema& cls = class_named("UAoCAbilityComponent");
    const schema::RpcSchema* rpc = cls.rpc("ServerTriggerAbility");
    BunchWriter out;
    repl::RpcParams p;
    bool ok = rpc && repl::set_rpc_param(*rpc, p, "AbilityGuid", repl::Value::i64(guid));
    if (ok && id != 0) ok = repl::set_rpc_param(*rpc, p, "ClientInstanceId", repl::Value::i64(id));
    ok = ok && repl::set_rpc_param(*rpc, p, "InputVector", repl::Value(repl::Vec2d{1, 0}));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.Slot", repl::Value::i64(-1));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.ItemInstance", repl::Value(repl::ItemInstance{}));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.Location", repl::Value::u64(24));
    BunchWriter field;
    bool failed = false;
    if (!ok || repl::write_rpc_field(cls, *rpc, p, field, quest_events_test::client_options(), &failed) == 0 || failed) {
        std::printf("  [FAIL] the trigger's parameters do not write\n");
        return out;
    }
    repl::BlockHeader header;
    header.is_actor = false;
    header.has_rep_layout = false;
    header.object = ability;
    repl::write_content_block(out, header, field, /*server_form=*/false);
    return out;
}

/// ServerItemUsed for an item of the item bag, as the client sends it for a double click.
static BunchWriter qe_item_used(int slot, const std::string& guid_hex) {
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    const schema::RpcSchema* rpc = pawn.rpc("ServerItemUsed");
    repl::RpcParams p;
    const auto g = items::guid_of(guid_hex);
    const repl::GuidValue gv = g ? *g : repl::GuidValue{};
    repl::set_rpc_param(*rpc, p, "Storage", repl::Value::u64(static_cast<uint64_t>(repl::ItemLocation::Backpack)));
    repl::set_rpc_param(*rpc, p, "Slot", repl::Value::i64(slot));
    repl::set_rpc_param(*rpc, p, "ExpectedItem.A", repl::Value::i64(static_cast<int32_t>(gv.a)));
    repl::set_rpc_param(*rpc, p, "ExpectedItem.B", repl::Value::i64(static_cast<int32_t>(gv.b)));
    repl::set_rpc_param(*rpc, p, "ExpectedItem.C", repl::Value::i64(static_cast<int32_t>(gv.c)));
    repl::set_rpc_param(*rpc, p, "ExpectedItem.D", repl::Value::i64(static_cast<int32_t>(gv.d)));
    repl::set_rpc_param(*rpc, p, "RequestedEquipSlot", repl::Value::i64(static_cast<int32_t>(0xFFFFFFFFu)));
    return actor_rpc(pawn, "ServerItemUsed", p);
}

// ---- the engine ----------------------------------------------------------------------------------------------------------------------------

static void test_events_engine() {
    std::printf("quest events: the engine counts abilities, kills by tag, items put on and items sold\n");
    namespace qet = quest_events_test;
    namespace dt = destiny_test;
    const QuestTestData data = qet::data();
    const qs::QuestBook book(data.quests);
    qs::QuestLog log(21);
    dt::World w(log, book, 1);
    const auto start = [&](int64_t quest) {
        const content::QuestRow* def = book.quest(quest);
        w.run([&](qs::Engine& e) { e.start(*def, 0); });
    };
    const auto objective_of = [&](int64_t quest) -> const qs::ObjectiveState* {
        const qs::QuestInstance* q = log.find(quest);
        return q && !q->objectives.empty() ? &q->objectives[0] : nullptr;
    };
    for (const int i : {1, 2, 3, 4, 5}) start(dt::task(0, i));
    CHECK(log.instances().size() == 5, "(five task quests of the new kinds are in the log)");

    // UseAbility: the listed ability counts one use; another ability counts nothing
    w.run([](qs::Engine& e) { e.ability(content::kGuidDodge + 1, true); });
    CHECK(objective_of(dt::task(0, 1))->count == 0, "an ability the objective does not list counts nothing");
    const size_t rewards_before = w.count(qs::Effect::Kind::GrantRewards), toasts_before = w.count(qs::Effect::Kind::Toast);
    w.run([](qs::Engine& e) { e.ability(content::kGuidSprint, false); });
    CHECK(log.find(dt::task(0, 1)) && log.find(dt::task(0, 1))->status == qs::Status::Completed, "a Sprint completes the quest that counts one (it counts in a fight or out of one)");
    CHECK(w.count(qs::Effect::Kind::GrantRewards, dt::task(0, 1)) == 1 && w.count(qs::Effect::Kind::GrantRewards) == rewards_before + 1 && w.count(qs::Effect::Kind::Toast) == toasts_before + 1,
          "... it pays its reward once and shows the toast once");
    w.run([](qs::Engine& e) { e.ability(content::kGuidSprint, true); });
    CHECK(w.count(qs::Effect::Kind::GrantRewards, dt::task(0, 1)) == 1 && w.count(qs::Effect::Kind::Toast) == toasts_before + 1, "a second Sprint pays nothing more: the objective is done");
    CHECK(log.quest_variable(dt::own(0), "Destiny_Reward") == 0, "(this log has no own quest: nothing to raise)");

    // a Dodge counts three times and only in a fight
    for (int i = 0; i < 4; ++i) w.run([](qs::Engine& e) { e.ability(content::kGuidDodge, false); });
    CHECK(objective_of(dt::task(0, 2))->count == 0, "four Dodges outside a fight count nothing");
    for (int i = 0; i < 2; ++i) w.run([](qs::Engine& e) { e.ability(content::kGuidDodge, true); });
    CHECK(objective_of(dt::task(0, 2))->count == 2 && log.find(dt::task(0, 2))->status == qs::Status::Active, "two Dodges in a fight count two");
    w.run([](qs::Engine& e) { e.ability(content::kGuidDodge, true); });
    CHECK(log.find(dt::task(0, 2))->status == qs::Status::Completed && w.count(qs::Effect::Kind::GrantRewards, dt::task(0, 2)) == 1, "the third finishes it, and pays once");

    // Equip: the listed item counts; another item, or the same item sold, counts nothing
    w.run([](qs::Engine& e) { e.collected("Equip", fi::kHelmet, 1); });
    w.run([](qs::Engine& e) { e.collected("Sell", fi::kSword2H, 1); });
    CHECK(objective_of(dt::task(0, 3))->count == 0, "another item put on, and the listed item sold, count nothing for an Equip objective");
    w.run([](qs::Engine& e) { e.collected("Equip", fi::kSword2H, 1); });
    CHECK(log.find(dt::task(0, 3))->status == qs::Status::Completed && w.count(qs::Effect::Kind::GrantRewards, dt::task(0, 3)) == 1, "the listed item put on completes it");
    // Sell: the listed item sold counts; an Equip of it does not
    w.run([](qs::Engine& e) { e.collected("Equip", fi::kPotion, 1); });
    CHECK(objective_of(dt::task(0, 5))->count == 0, "the listed item put on counts nothing for a Sell objective");
    w.run([](qs::Engine& e) { e.collected("Sell", fi::kPotion, 3); });
    CHECK(log.find(dt::task(0, 5))->status == qs::Status::Completed && w.count(qs::Effect::Kind::GrantRewards, dt::task(0, 5)) == 1, "the listed item sold completes it (a count above the objective's is held at it)");

    // Kill by tag query: "character" is every creature's; a creature whose tags are another kind's does not count for a query of that kind
    const std::set<std::string> wolf = {"character", "character.beast", "character.beast.wolf"};
    w.run([&](qs::Engine& e) { e.kill(777, 0, wolf); });
    CHECK(objective_of(dt::task(0, 4))->count == 1, "a kill of any creature counts for the query ANY(character)");
    w.run([&](qs::Engine& e) { e.kill(778, 0, wolf); });
    CHECK(log.find(dt::task(0, 4))->status == qs::Status::Completed && w.count(qs::Effect::Kind::GrantRewards, dt::task(0, 4)) == 1, "the second kill finishes it, and pays once");
    {
        qs::QuestLog log2(22);
        content::QuestData d2 = data.quests;
        d2.quests.at(dt::task(0, 4)).objectives[0].kill_query = qet::query_of({"character.undead"});
        const qs::QuestBook book2(d2);
        dt::World w2(log2, book2, 1);
        w2.run([&](qs::Engine& e) { e.start(*book2.quest(dt::task(0, 4)), 0); });
        w2.run([&](qs::Engine& e) { e.kill(777, 0, wolf); });
        const qs::QuestInstance* q = log2.find(dt::task(0, 4));
        CHECK(q && q->objectives[0].count == 0, "a wolf does not count for the query ANY(character.undead)");
        w2.run([&](qs::Engine& e) { e.kill(779, 0, {"character", "character.undead", "character.undead.zombie"}); });
        CHECK(q && log2.find(dt::task(0, 4))->objectives[0].count == 1, "a zombie does");
        // two tags that must both be there
        d2.quests.at(dt::task(0, 4)).objectives[0].kill_query = [] {
            auto query = std::make_shared<content::TagQuery>();
            query->present = true;
            query->tok = {0, 1, 2, 2, 0, 1};   // ALL of the two tags
            query->tags = {"character.humanoid", "affiliation.bandit"};
            return query;
        }();
        qs::QuestLog log3(23);
        const qs::QuestBook book3(d2);
        dt::World w3(log3, book3, 1);
        w3.run([&](qs::Engine& e) { e.start(*book3.quest(dt::task(0, 4)), 0); });
        w3.run([&](qs::Engine& e) { e.kill(1, 0, {"character", "character.humanoid"}); });
        w3.run([&](qs::Engine& e) { e.kill(2, 0, {"character", "character.humanoid", "affiliation", "affiliation.bandit"}); });
        CHECK(log3.find(dt::task(0, 4)) && log3.find(dt::task(0, 4))->objectives[0].count == 1, "a query that needs two tags counts the creature that has both, not the one that has one");
        // a record that says a tag query but gives none: told, counts nothing
        d2.quests.at(dt::task(0, 4)).objectives[0].kill_query = nullptr;
        qs::QuestLog log4(24);
        const qs::QuestBook book4(d2);
        dt::World w4(log4, book4, 1);
        w4.run([&](qs::Engine& e) { e.start(*book4.quest(dt::task(0, 4)), 0); });
        w4.run([&](qs::Engine& e) { e.kill(1, 0, wolf); });
        CHECK(log4.find(dt::task(0, 4)) && log4.find(dt::task(0, 4))->objectives[0].count == 0 && told_about(w4, "tag query") >= 1, "a tag query the data did not give counts nothing, and it is told");
    }
}

// ---- the session ---------------------------------------------------------------------------------------------------------------------------

static void test_events_session() {
    std::printf("quest events: a Sprint, a Dodge and an item put on count through the real paths, and the window is told\n");
    namespace qet = quest_events_test;
    namespace dt = destiny_test;
    const QuestTestData data = qet::data();
    FakeQuestApi api;
    qs::Guid128 first_steps;
    {
        DestinyHarness h(api, data, log_after_start());
        h.play();
        h.run(milliseconds(3500));
        const qs::QuestLog& log = h.s->quest_log();
        CHECK(h.s->abilities() != nullptr && h.s->bootstrap().setup().guids.ability.ObjectId != 0, "(the session runs abilities)");
        CHECK(log.find(dt::task(0, 1)) && log.find(dt::task(0, 2)) && log.find(dt::task(0, 3)) && log.quest_variable(dt::own(0), "Destiny_Reward") == 0, "(First Steps is in the log: no task done)");
        first_steps = log.find(dt::own(0)) ? log.find(dt::own(0))->guid : qs::Guid128{};
        // the window asks: nothing done
        const std::vector<int64_t> asked = {dt::root(0), dt::root(5)};
        size_t from = h.got.size();
        h.client_request(destiny_request(asked), h.pc_ch);
        auto answers = destiny_answers_since(h, from);
        CHECK(answers.size() == 1 && answers[0] && answers[0]->size() == 2 && (*answers[0])[0].completed == 0, "(the window's answer says none of the eight)");

        // a Sprint: the server starts it, the quest counts it, Destiny_Reward rises, and the client is told the new count without asking
        const emit::FIntrepidNetworkGUID ability = h.s->bootstrap().setup().guids.ability;
        const size_t finished_before = h.s->quests_finished();
        from = h.got.size();
        h.client_request(qe_trigger(ability, content::kGuidSprint, 1), h.pawn_ch);
        h.run(milliseconds(300));
        CHECK(log.quest_variable(dt::own(0), "Destiny_Reward") == 1 && h.s->quests_finished() == finished_before + 1, "a Sprint the server started completes the Sprint task: Destiny_Reward is 1");
        const auto pushed = destiny_answers_since(h, from);
        CHECK(!pushed.empty() && pushed.back() && !pushed.back()->empty() && (*pushed.back())[0].track_quest == dt::root(0) && (*pushed.back())[0].completed == 1,
              "the client is told the new count without asking (an answer that says 1 of 8)");

        // a Dodge costs 50 Stamina of 100: each Dodge comes after a pause (which gives the Stamina back), then the fight is on again
        const auto dodge_count = [&]() -> int64_t {
            const qs::QuestInstance* q = log.find(dt::task(0, 2));
            return q && !q->objectives.empty() ? q->objectives[0].count : -1;
        };
        const auto settle = [&](bool fight) {
            h.s->set_fight_for_tests(false);
            h.run(milliseconds(30000));
            h.s->set_fight_for_tests(fight);
            h.run(milliseconds(500));
        };
        const auto dodge = [&](int id) -> bool {   // true when the abilities started it (it paid its Stamina)
            const auto* rt = h.s->abilities();
            const double before = rt ? rt->stamina() : 0;
            h.client_request(qe_trigger(ability, content::kGuidDodge, id), h.pawn_ch);
            h.run(milliseconds(50));
            return rt && rt->stamina() < before - 40;
        };
        // a Dodge outside a fight counts nothing, though the server starts it
        settle(false);
        const bool out_first = dodge(10);
        settle(false);
        const bool out_second = dodge(11);
        CHECK(out_first && out_second && dodge_count() == 0, "two Dodges the server started outside a fight count nothing");
        // in a fight one counts; one asked for at once is on its cooldown, refused, and counts nothing
        settle(true);
        const bool in_first = dodge(20);
        const bool in_again = dodge(21);
        CHECK(in_first && !in_again && dodge_count() == 1, "a Dodge in a fight counts one; one asked for at once (still on its cooldown) is refused and counts nothing");
        for (int i = 0; i < 2; ++i) {
            settle(true);
            dodge(22 + i);
        }
        h.run(milliseconds(300));
        CHECK(log.quest_variable(dt::own(0), "Destiny_Reward") == 2, "three Dodges in a fight complete the Dodge task: Destiny_Reward is 2");
        const qs::QuestInstance* own = log.find(dt::own(0));
        CHECK(own && own->objectives[8].active && log.find(dt::milestone_quest(0, 0)) == nullptr, "the second finished task makes the first milestone's objective active; its quest stays unstarted (the Claim comes later)");
        // a Dodge asked for after the task is done changes nothing
        const int64_t reward_now = log.quest_variable(dt::own(0), "Destiny_Reward");
        settle(true);
        dodge(30);
        CHECK(log.quest_variable(dt::own(0), "Destiny_Reward") == reward_now, "(more Dodges change nothing: the Dodge task is done)");

        // an item put on: the sword in the bag is worn
        const svc::InventoryItem* sword = h.s->inventory().at(svc::ItemContainer::Backpack, 0);
        CHECK(sword != nullptr && sword->item_record_id == fi::kSword2H, "(the sword is in the first slot of the item bag)");
        if (sword) {
            const std::string guid = sword->instance_guid;
            h.client_request(qe_item_used(0, guid), h.pawn_ch);
            h.run(milliseconds(300));
            const qs::QuestInstance* equip = log.find(dt::task(0, 3));
            CHECK(log.quest_variable(dt::own(0), "Destiny_Reward") == 3 && (!equip || equip->status == qs::Status::Completed), "putting on the listed sword completes the Equip task: Destiny_Reward is 3");
        }
        h.saved();
    }
    // a relog keeps the counts and takes no finished task again
    DestinyHarness again(api, data, api.log(kCyb));
    again.play();
    again.run(milliseconds(5000));
    const qs::QuestLog& log = again.s->quest_log();
    CHECK(log.find(dt::own(0)) && log.find(dt::own(0))->guid == first_steps && log.quest_variable(dt::own(0), "Destiny_Reward") == 3, "a relog: First Steps keeps its instance and its count of 3");
    CHECK(!log.find(dt::task(0, 1)) && !log.find(dt::task(0, 2)) && !log.find(dt::task(0, 3)) && again.s->quests_finished() == 0, "... the three finished tasks are not taken again, and nothing is paid again");
}

// ---- a kill hands out the quest items of an objective of any kind --------------------------------------------------------------------------------

/// A quest with one Collect objective and no Kill objective, whose item reward names the creature instance `instance`: the creature drops the table `table` while the objective is active.
static content::QuestRow drop_quest(int64_t guid, int64_t table, int64_t instance) {
    content::QuestRow q = destiny_test::quest(guid, "Drops_" + std::to_string(guid));
    content::QuestObjectiveRow o;
    o.guid = guid + 1;
    o.name = "Collect_Drops";
    o.type = "Collect";
    o.count = 3;
    o.awake = true;
    content::QuestObjectiveRow::ItemReward r;
    r.table = table;
    r.instances = {instance};
    o.item_rewards.push_back(std::move(r));
    q.objectives.push_back(std::move(o));
    return q;
}

static void test_events_kill_drops_engine() {
    std::printf("quest events: a kill hands out the quest item a Collect objective's creature drops (no Kill objective is active)\n");
    namespace dt = destiny_test;
    constexpr int64_t kQuest = 7001, kTable = 91, kDropper = 4001;
    content::QuestData d;
    d.quests[kQuest] = drop_quest(kQuest, kTable, kDropper);
    for (const auto& [guid, q] : d.quests) d.by_name[q.name] = guid;
    const qs::QuestBook book(d);
    qs::QuestLog log(31);
    dt::World w(log, book, 1);
    w.run([&](qs::Engine& e) { e.start(*book.quest(kQuest), 0); });
    const qs::QuestInstance* q = log.find(kQuest);
    CHECK(q && !q->objectives.empty() && q->objectives[0].active && w.count(qs::Effect::Kind::GrantTable) == 0, "(the quest is taken: its Collect objective is active, it has no Kill objective, and nothing was granted)");
    w.run([](qs::Engine& e) { e.kill(4002, 0); });
    CHECK(w.count(qs::Effect::Kind::GrantTable) == 0, "a kill of a creature the objective does not list drops nothing");
    w.run([](qs::Engine& e) { e.kill(kDropper, 0); });
    std::vector<int64_t> tables;
    for (const qs::Effect& e : w.seen) {
        if (e.kind == qs::Effect::Kind::GrantTable) tables.insert(tables.end(), e.tables.begin(), e.tables.end());
    }
    CHECK(w.count(qs::Effect::Kind::GrantTable) == 1 && w.count(qs::Effect::Kind::GrantTable, kQuest) == 1 && tables == std::vector<int64_t>({kTable}), "a kill of the listed creature gives the table of the quest item, once");
    w.run([](qs::Engine& e) { e.kill(kDropper, 0, {"character", "character.beast"}); });
    CHECK(w.count(qs::Effect::Kind::GrantTable) == 2, "a kill that comes with the creature's tags gives it as well (the tags change nothing for a drop)");
}

static void test_events_kill_drops_session() {
    std::printf("quest events: a real kill gives the quest item of Heart of the Matter, a Collect objective, while no Kill objective is active\n");
    constexpr int64_t kHeartOfTheMatter = 6064634727321108492ll, kCollectStones = 6064634727347650573ll, kStonesTable = 6064634727435403280ll;   // the quest, its Collect objective and the table that gives the Gold-Etched Heartstone
    constexpr int64_t kDropper = 6064634727048151041ll;   // a creature instance the objective's item reward lists
    std::string why;
    const auto data = quest_test_data(why);
    if (!data) { std::printf("  [skip] SKIP: %s\n", why.c_str()); return; }
    const qs::QuestBook qb(data->quests);
    const content::QuestRow* def = qb.quest(kHeartOfTheMatter);
    if (!def) { std::printf("  [skip] SKIP: this content.db has no Heart of the Matter\n"); return; }
    const content::QuestObjectiveRow* collect = nullptr;
    bool no_kill = true;
    for (const content::QuestObjectiveRow& o : def->objectives) {
        no_kill = no_kill && o.type != "Kill";
        if (o.guid == kCollectStones) collect = &o;
    }
    CHECK(collect && collect->type == "Collect" && collect->awake && no_kill && collect->item_rewards.size() == 1 && collect->item_rewards[0].table == kStonesTable &&
              std::find(collect->item_rewards[0].instances.begin(), collect->item_rewards[0].instances.end(), kDropper) != collect->item_rewards[0].instances.end(),
          "the data: the quest has a Collect objective that starts awake and no Kill objective, and its item reward lists the creature");
    if (!collect) return;
    size_t at = 0;
    for (size_t i = 0; i < def->objectives.size(); ++i) {
        if (def->objectives[i].guid == kCollectStones) at = i;
    }
    FakeQuestApi api;
    QuestHarness h(api, *data, log_after_start());
    h.play();
    h.run(milliseconds(1500));
    api.queue(kCyb, "give", "ANV_FTUE_QST_1036_HeartOfTheMatter");
    h.commands();
    const qs::QuestInstance* qi = h.s->quest_log().find(kHeartOfTheMatter);
    CHECK(qi && qi->objectives.size() > at && qi->objectives[at].active && !qi->objectives[at].completed, "a GM gave the quest: the Collect objective is active");
    if (!qi) return;
    // (this test world has no reward tables to roll from, so the quest item itself cannot reach the bag: the session shows the table it was asked to give)
    const size_t before = h.s->quest_grants().size();
    combat::KillReward other;
    other.instance = 123456;
    other.name = "Another creature";
    h.s->kill_for_tests(h.conn, other, h.t);
    h.run(milliseconds(300));
    CHECK(h.s->kills_rewarded() == 1 && h.s->quest_grants().size() == before, "a kill of a creature the objective does not list asks for no quest item");
    combat::KillReward dropper;
    dropper.instance = kDropper;
    dropper.name = "The creature that drops the heartstone";
    h.s->kill_for_tests(h.conn, dropper, h.t);
    h.run(milliseconds(300));
    CHECK(h.s->kills_rewarded() == 2 && h.s->quest_grants().size() == before + 1 && h.s->quest_grants().back() == kStonesTable, "a kill of the listed creature asks for the table of the quest item, once");
}

AOC_REGISTER_TEST(test_events_engine);
AOC_REGISTER_TEST(test_events_session);
AOC_REGISTER_TEST(test_events_kill_drops_engine);
AOC_REGISTER_TEST(test_events_kill_drops_session);
