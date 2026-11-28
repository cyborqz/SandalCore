// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/destiny_session_tests.inc: the Destiny window's tracks (quests milestone, the Destiny window)
//  (registered with AOC_REGISTER_TEST: test_worldd_area_session.cpp runs them through aoc_tests::run; included from area_session_registered.inc)
//
//  The data is built here, in the shape the game's own data has: a master quest with one SubQuest objective for each of six tracks (the first and the last open at level 1, the
//  other four at level 5), each track's own quest with eight task objectives (each starts a task quest and raises the quest variable Destiny_Reward when that quest is done) and four
//  hidden milestone objectives (open at 2, 4, 6 and 8 tasks), and the track quest the client names (a quest with one objective that starts the own quest).
//
//    the engine      a SubQuest objective starts its quest once, is done when that quest is, fails with it, counts a quest that was finished before, and leaves the milestones' quests
//                    alone; an awake SubQuest objective whose rule does not hold starts inactive; the parent of a started quest is found again from the log and the data
//    the login       a new character at level 1 gets the First Steps and Combat tracks (own quest and eight tasks each) and nothing else: no master quest, no track quest, no
//                    milestone quest; each task's cache item names its track's quest instance; a second pass changes nothing; a relog brings the same instances back
//    level 5         the other four tracks start; the level task of every track completes and raises Destiny_Reward to 1; a task that is done at 2 opens the first milestone but starts no quest
//    the answer      the six tracks in the window's order are answered in that order with the guid of each own quest's instance (the guid its cache item carries), 8 tasks, none done;
//                    an unknown track, a track named twice, no track and a thousand tracks give bounded answers; the requests are budgeted
//    the wire        one answer element is bit for bit the one the game's server sent (a recorded answer, First Steps)
// ============================================================================
#include "protocol/repl/destiny_rpc.h"
#include "quest/destiny.h"

#include <deque>
#include <functional>

/// A check whose message says the numbers (the area session program has no CHECKF of its own).
#define DESTINY_CHECKF(cond, ...) do { char destiny_msg[512]; std::snprintf(destiny_msg, sizeof destiny_msg, __VA_ARGS__); CHECK(cond, destiny_msg); } while (0)

namespace destiny_test {

constexpr int64_t kMaster = 6064639100000000001ll;
constexpr int64_t kBase = 6064639200000000000ll, kStep = 10000;
inline int64_t base(int t) { return kBase + t * kStep; }
inline int64_t root(int t) { return base(t) + 1; }                       // the track quest the client names
inline int64_t own(int t) { return base(t) + 10; }                       // the track's own quest
inline int64_t task(int t, int i) { return base(t) + 100 + i; }          // the task quest i
inline int64_t slot(int t, int i) { return base(t) + 200 + i; }          // the objective of the own quest that starts task i
inline int64_t task_objective(int t, int i) { return base(t) + 400 + i; }
inline int64_t milestone_slot(int t, int j) { return base(t) + 300 + j; }
inline int64_t milestone_quest(int t, int j) { return base(t) + 500 + j; }
inline int64_t master_slot(int t) { return kMaster + 10 + t; }

// the data's order: First Steps, Initiate Adventuring, Gathering, Processing, Crafting, Combat
constexpr int kGate[6] = {1, 5, 5, 5, 5, 0};            // the level that opens the track (0: no rule)
constexpr int kWindowOrder[6] = {0, 5, 1, 4, 2, 3};      // the window's own order: First Steps, Combat, Initiate, Crafting, Gathering, Processing

inline content::DialogueRequirement rule(const char* kind, const char* variable, int64_t value, const char* expression = "") {
    content::DialogueRequirement r;
    content::DialogueCriterion c;
    c.kind = kind;
    c.op = "GreaterThanOrEqual";
    c.value = value;
    c.variable = variable;
    c.expression = expression;
    r.tokens = {1, 1, 0};
    r.criteria.push_back(std::move(c));
    return r;
}

inline content::QuestRow quest(int64_t guid, const std::string& name) {
    content::QuestRow q;
    q.guid = guid;
    q.name = name;
    q.flavor = "Destiny";
    q.locality = "Local";
    q.level = 1;
    return q;
}

inline content::QuestObjectiveRow sub_quest(int64_t guid, const std::string& name, int64_t child, bool hidden = false) {
    content::QuestObjectiveRow o;
    o.guid = guid;
    o.name = name;
    o.type = "SubQuest";
    o.count = 1;
    o.awake = true;
    o.hidden = hidden;
    o.sub_quest = child;
    o.sub_kind = "AutoGrant";
    o.sub_wait = true;
    return o;
}

inline content::QuestObjectiveRow custom(int64_t guid, const std::string& name, content::DialogueRequirement r) {
    content::QuestObjectiveRow o;
    o.guid = guid;
    o.name = name;
    o.type = "Custom";
    o.count = 1;
    o.awake = true;
    o.rule = std::move(r);
    return o;
}

/// The six tracks.  Task 0 of a track is the level task (Custom: level 5), the others wait for the player variable DestinyTask_<track>_<task>.
inline QuestTestData data() {
    QuestTestData out;
    content::QuestData& d = out.quests;
    content::QuestRow master = quest(kMaster, "Destiny_Root_Quest");
    for (int t = 0; t < 6; ++t) {
        content::QuestObjectiveRow m = sub_quest(master_slot(t), "Destiny_Master_" + std::to_string(t), own(t));
        if (kGate[t] > 0) m.activation = rule("Expression", "", kGate[t], "GetCharacterLevel(GetPlayer())");
        master.objectives.push_back(std::move(m));
    }
    d.quests[master.guid] = master;
    d.by_name[master.name] = master.guid;
    for (int t = 0; t < 6; ++t) {
        content::QuestRow r = quest(root(t), "Destiny_Root_" + std::to_string(t));
        r.objectives.push_back(sub_quest(base(t) + 2, "Destiny_Root_Obj_" + std::to_string(t), own(t)));
        d.quests[r.guid] = r;
        content::QuestRow o = quest(own(t), "Destiny_Own_" + std::to_string(t));
        std::vector<int64_t> milestones;
        for (int i = 0; i < 8; ++i) {
            content::QuestObjectiveRow task_slot = sub_quest(slot(t, i), "Task_" + std::to_string(t) + "_" + std::to_string(i), task(t, i));
            content::QuestAction up;
            up.action = "IncrementQuestVariable";
            up.variable = "Destiny_Reward";
            up.value = 1;
            task_slot.on_success.push_back(up);
            o.objectives.push_back(std::move(task_slot));
        }
        for (int j = 0; j < 4; ++j) {
            content::QuestObjectiveRow m = sub_quest(milestone_slot(t, j), "Milestone_" + std::to_string(t) + "_" + std::to_string(j), milestone_quest(t, j), /*hidden=*/true);
            m.activation = rule("LocalQuestVariable", "Destiny_Reward", 2 * (j + 1));
            o.objectives.push_back(std::move(m));
            milestones.push_back(milestone_slot(t, j));
        }
        d.quests[o.guid] = o;
        for (int i = 0; i < 8; ++i) {
            content::QuestRow k = quest(task(t, i), "Task_" + std::to_string(t) + "_" + std::to_string(i));
            k.objectives.push_back(i == 0 ? custom(task_objective(t, i), "Level", rule("Expression", "", 5, "GetCharacterLevel(GetPlayer())"))
                                          : custom(task_objective(t, i), "Done", rule("PlayerVariable", ("DestinyTask_" + std::to_string(t) + "_" + std::to_string(i)).c_str(), 1)));
            d.quests[k.guid] = k;
        }
        for (int j = 0; j < 4; ++j) {
            content::QuestRow k = quest(milestone_quest(t, j), "Milestone_" + std::to_string(t) + "_" + std::to_string(j));
            k.objectives.push_back(custom(base(t) + 600 + j, "Claim", rule("PlayerVariable", "NeverSet", 1)));
            d.quests[k.guid] = k;
        }
        content::DestinyTrackRow track;
        track.track_quest = root(t);
        track.quest = own(t);
        track.skill_tree = base(t) + 7;
        track.milestones = milestones;
        d.destiny_tracks.push_back(std::move(track));
    }
    for (const auto& [guid, q] : d.quests) d.by_name[q.name] = guid;
    out.book.emplace();
    return out;
}

/// What the conditions ask of a character at `level` with this log.
struct Facts : dl::Facts {
    const qs::QuestLog& log;
    int                 level;
    Facts(const qs::QuestLog& l, int lvl) : log(l), level(lvl) {}
    int64_t player_variable(const std::string& n) const override { return log.variable(n); }
    std::optional<int64_t> quest_count(int64_t q, const std::string& query) const override { return log.quest_count(q, query); }
    int64_t quest_variable(int64_t q, const std::string& n) const override { return log.quest_variable(q, n); }
    std::optional<dl::ExprValue> call(const std::string& name, const std::vector<dl::ExprValue>&) const override {
        if (name == "GetPlayer") return dl::ExprValue::of(1);
        if (name == "GetCharacterLevel" || name == "GetPlayerLevel") return dl::ExprValue::of(level);
        return std::nullopt;
    }
};

/// The quest engine without a session: the part of the world that starts what a SubQuest objective asks for is played here.
struct World {
    qs::QuestLog&            log;
    const qs::QuestBook&     book;
    int                      level = 1;
    double                   now = 100;
    dl::Book                 dbook;
    std::vector<qs::Effect>  seen;   // every effect, in order

    World(qs::QuestLog& l, const qs::QuestBook& b, int lvl = 1) : log(l), book(b), level(lvl) {}

    void run(const std::function<void(qs::Engine&)>& fn) {
        Facts facts(log, level);
        dl::Evaluator ev(dbook, facts);
        std::vector<qs::Effect> fx;
        {
            qs::Engine eng(log, book, ev, facts, fx, now);
            fn(eng);
        }
        std::deque<qs::Effect> todo(fx.begin(), fx.end());
        while (!todo.empty()) {
            const qs::Effect e = todo.front();
            todo.pop_front();
            seen.push_back(e);
            if (e.kind != qs::Effect::Kind::StartQuest) continue;
            const content::QuestRow* def = book.quest(e.quest);
            Facts f2(log, level);
            dl::Evaluator ev2(dbook, f2);
            if (!def || log.can_start(def, ev2, false) != qs::Create::Ok) continue;
            std::vector<qs::Effect> more;
            qs::Engine eng(log, book, ev2, f2, more, now);
            eng.start(*def, 0);
            todo.insert(todo.end(), more.begin(), more.end());
        }
    }
    void settle() { run([](qs::Engine& e) { e.settle(); }); }
    /// The login: the quests the character's level opens.
    void seed() {
        Facts facts(log, level);
        dl::Evaluator ev(dbook, facts);
        for (const content::QuestRow* def : qs::destiny_seeds(book, log, ev)) {
            run([&](qs::Engine& e) { e.start(*def, 0); });
        }
    }
    size_t count(qs::Effect::Kind k, int64_t quest = 0) const {
        return static_cast<size_t>(std::count_if(seen.begin(), seen.end(), [&](const qs::Effect& e) { return e.kind == k && (quest == 0 || e.quest == quest); }));
    }
};

inline size_t told_about(const World& w, const char* what) {
    return static_cast<size_t>(std::count_if(w.seen.begin(), w.seen.end(), [&](const qs::Effect& e) { return e.kind == qs::Effect::Kind::Told && e.text.find(what) != std::string::npos; }));
}

inline size_t in_log(const qs::QuestLog& log, int t, bool own_quest, bool tasks) {
    size_t n = own_quest && log.find(own(t)) ? 1 : 0;
    for (int i = 0; tasks && i < 8; ++i) n += log.find(task(t, i)) ? 1 : 0;
    return n;
}

inline bool nothing_else(const qs::QuestLog& log, const std::set<int>& tracks) {
    // only the own quests and the tasks of `tracks`: no master, no track quest, no milestone quest, no other track
    size_t expected = 0;
    for (const int t : tracks) expected += in_log(log, t, true, true);
    bool others = false;
    for (const qs::QuestInstance& q : log.instances()) {
        for (int t = 0; t < 6; ++t) {
            if (tracks.count(t)) continue;
            others = others || q.record == own(t) || q.record == root(t);
            for (int i = 0; i < 8; ++i) others = others || q.record == task(t, i);
        }
        for (int t = 0; t < 6; ++t) {
            for (int j = 0; j < 4; ++j) others = others || q.record == milestone_quest(t, j);
            others = others || q.record == root(t);
        }
        others = others || q.record == kMaster;
    }
    return !others && log.instances().size() == expected;
}

}  // namespace destiny_test

// ---- the engine ----------------------------------------------------------------------------------------------------------------------------

static void test_destiny_sub_quest_engine() {
    std::printf("destiny: a SubQuest objective starts its quest and waits for it\n");
    namespace dt = destiny_test;
    const QuestTestData data = dt::data();
    const qs::QuestBook book(data.quests);
    const content::QuestRow* own0 = book.quest(dt::own(0));
    CHECK(own0 && own0->objectives.size() == 12 && book.destiny_tracks().size() == 6 && book.destiny_track(dt::root(3)) && book.destiny_track(dt::root(3))->quest == dt::own(3),
          "(the data: a track's own quest has twelve objectives and the book knows six tracks)");
    CHECK(book.destiny_milestone(dt::milestone_slot(0, 0)) && !book.destiny_milestone(dt::slot(0, 0)), "(the book knows the milestone objectives)");
    if (!own0) return;
    qs::QuestLog log(7);
    dt::World w(log, book);
    w.run([&](qs::Engine& e) { e.start(*own0, 0); });
    CHECK(log.instances().size() == 9 && dt::in_log(log, 0, true, true) == 9, "the own quest and its eight tasks are in the log, started by the objectives");
    CHECK(w.count(qs::Effect::Kind::StartQuest) >= 8 && w.count(qs::Effect::Kind::StartQuest, dt::milestone_quest(0, 0)) == 0, "the milestones' quests were not asked for");
    const qs::QuestInstance* q = log.find(dt::own(0));
    bool milestones_off = q != nullptr;
    for (int j = 0; q && j < 4; ++j) milestones_off = milestones_off && !q->objectives[8 + j].active && q->objectives[8 + j].hidden;
    bool tasks_on = q != nullptr;
    for (int i = 0; q && i < 8; ++i) tasks_on = tasks_on && q->objectives[static_cast<size_t>(i)].active;
    CHECK(milestones_off && tasks_on, "the eight tasks are active and the four milestones are hidden and inactive (an awake SubQuest objective with a rule that does not hold starts inactive)");
    bool parents = q != nullptr;
    for (int i = 0; q && i < 8; ++i) {
        const qs::QuestInstance* k = log.find(dt::task(0, i));
        parents = parents && k && k->parent == q->guid;
    }
    CHECK(parents && q && q->parent.zero(), "every task's parent is the own quest's instance, and the own quest has none in the log");
    // a second pass starts nothing and changes nothing
    const size_t before = w.count(qs::Effect::Kind::StartQuest);
    w.settle();
    CHECK(log.instances().size() == 9 && w.count(qs::Effect::Kind::StartQuest) == before, "settling again asks for no quest: the tasks are in the log");
    // a quest told once
    CHECK(told_about(w, "Claim button") <= 1, "(the milestones' wait is told at most once for an engine)");

    // a task is done: its objective in the own quest is done at once, and Destiny_Reward rises
    log.set_variable("DestinyTask_0_1", 1);
    w.settle();
    const qs::QuestInstance* done = log.find(dt::task(0, 1));
    CHECK(done && done->status == qs::Status::Completed, "the task whose rule holds is done");
    q = log.find(dt::own(0));
    CHECK(q && q->objectives[1].completed && q->objectives[1].succeeded && !q->objectives[0].completed, "... and so is its objective of the own quest (only that one)");
    CHECK(log.quest_variable(dt::own(0), "Destiny_Reward") == 1, "... which raised Destiny_Reward to 1");
    // the world takes the finished task out of the log: nothing changes, nothing counts twice
    log.finish(dt::task(0, 1));
    w.settle();
    CHECK(log.quest_variable(dt::own(0), "Destiny_Reward") == 1 && log.find(dt::task(0, 1)) == nullptr && log.completions(dt::task(0, 1)) == 1, "the finished task leaves the log and counts once");
    // a second task: two done opens the first milestone's objective, whose quest waits for the Claim button
    log.set_variable("DestinyTask_0_2", 1);
    w.settle();
    q = log.find(dt::own(0));
    CHECK(log.quest_variable(dt::own(0), "Destiny_Reward") == 2 && q && q->objectives[8].active && !q->objectives[9].active, "two tasks done: Destiny_Reward 2 opens the first milestone's objective and no other");
    CHECK(log.find(dt::milestone_quest(0, 0)) == nullptr && w.count(qs::Effect::Kind::StartQuest, dt::milestone_quest(0, 0)) == 0, "... and its quest is not started");
    CHECK(told_about(w, "Claim button") >= 1, "... which is told");

    // a child that fails fails the objective
    qs::QuestInstance* failing = log.find(dt::task(0, 3));
    CHECK(failing != nullptr, "(task 3 is in the log)");
    if (failing) {
        failing->status = qs::Status::Failed;
        log.touch(*failing);
        w.settle();
        q = log.find(dt::own(0));
        CHECK(q && q->objectives[3].failed && !q->objectives[3].completed && log.quest_variable(dt::own(0), "Destiny_Reward") == 2, "a task that fails fails its objective and counts for nothing");
    }

    // a task that was finished before the own quest started counts at once (a log saved between the task's end and the objective's)
    qs::QuestLog late(9);
    dt::World lw(late, book);
    lw.run([&](qs::Engine& e) { e.start(*book.quest(dt::task(1, 0)), 0); });
    late.finish(dt::task(1, 0));
    lw.run([&](qs::Engine& e) { e.start(*book.quest(dt::own(1)), 0); });
    CHECK(late.completions(dt::task(1, 0)) == 1 && late.find(dt::task(1, 0)) == nullptr && late.quest_variable(dt::own(1), "Destiny_Reward") == 1 && late.find(dt::own(1)) &&
              late.find(dt::own(1))->objectives[0].completed,
          "a task finished before counts when the own quest starts, and it is not started again");
}

static void test_destiny_seeds() {
    std::printf("destiny: the tracks the level opens\n");
    namespace dt = destiny_test;
    const QuestTestData data = dt::data();
    const qs::QuestBook book(data.quests);
    const auto seeds = [&](const qs::QuestLog& log, int level) {
        dt::Facts facts(log, level);
        dl::Evaluator ev(*data.book, facts);
        std::vector<int64_t> out;
        for (const content::QuestRow* d : qs::destiny_seeds(book, log, ev)) out.push_back(d->guid);
        return out;
    };
    qs::QuestLog log(11);
    CHECK(seeds(log, 1) == std::vector<int64_t>({dt::own(0), dt::own(5)}), "level 1 opens First Steps and Combat, in the master quest's order");
    CHECK(seeds(log, 4) == seeds(log, 1), "level 4 opens the same two");
    CHECK(seeds(log, 5) == std::vector<int64_t>({dt::own(0), dt::own(1), dt::own(2), dt::own(3), dt::own(4), dt::own(5)}), "level 5 opens all six");
    dt::World w(log, book, 1);
    w.seed();
    CHECK(seeds(log, 1).empty() && log.find(dt::own(0)) && log.find(dt::own(5)), "the two tracks are in the log: nothing more to take at level 1");
    CHECK(seeds(log, 5) == std::vector<int64_t>({dt::own(1), dt::own(2), dt::own(3), dt::own(4)}), "level 5: the four that are not in the log");
    log.finish(dt::own(0));
    CHECK(seeds(log, 5) == std::vector<int64_t>({dt::own(1), dt::own(2), dt::own(3), dt::own(4)}), "a track that was finished is not taken again");
    qs::QuestLog empty_book_log(3);
    const qs::QuestBook none{content::QuestData{}};
    dt::Facts facts(empty_book_log, 10);
    dl::Evaluator ev(*data.book, facts);
    CHECK(qs::destiny_seeds(none, empty_book_log, ev).empty(), "a book without the master quest seeds nothing");
}

static void test_destiny_answers_pure() {
    std::printf("destiny: the answer for the window's request\n");
    namespace dt = destiny_test;
    const QuestTestData data = dt::data();
    const qs::QuestBook book(data.quests);
    qs::QuestLog log(5);
    dt::World w(log, book, 1);
    w.seed();
    std::vector<int64_t> asked;
    for (const int t : dt::kWindowOrder) asked.push_back(dt::root(t));
    auto answers = qs::destiny_answers(book, log, asked);
    bool order = answers.size() == 6;
    for (size_t i = 0; order && i < 6; ++i) order = answers[i].track_quest == dt::root(dt::kWindowOrder[i]);
    CHECK(order, "six tracks in the window's order are answered in that order, each with the quest the client named");
    bool open_ok = order, shut_ok = order;
    for (size_t i = 0; order && i < 6; ++i) {
        const int t = dt::kWindowOrder[i];
        const qs::QuestInstance* q = log.find(dt::own(t));
        if (dt::kGate[t] <= 1) open_ok = open_ok && q && answers[i].active && answers[i].guid == q->guid && answers[i].total == 8 && answers[i].completed == 0;
        else shut_ok = shut_ok && !q && !answers[i].active && answers[i].guid.zero() && answers[i].total == 8 && answers[i].completed == 0;
    }
    CHECK(open_ok, "First Steps and Combat are open, with the guid of their own quest's instance, eight tasks and none done");
    CHECK(shut_ok, "the other four are not open, with a zero guid");
    // the tasks done are counted
    log.set_variable("DestinyTask_0_1", 1);
    log.set_variable("DestinyTask_0_2", 1);
    w.settle();
    answers = qs::destiny_answers(book, log, {dt::root(0)});
    CHECK(answers.size() == 1 && answers[0].completed == 2, "two tasks done: completed 2");
    log.set_quest_variable(dt::own(0), "Destiny_Reward", 99);
    answers = qs::destiny_answers(book, log, {dt::root(0)});
    CHECK(answers.size() == 1 && answers[0].completed == 8, "a count above the eight is held at the eight");
    // what the request may hold
    CHECK(qs::destiny_answers(book, log, {12345, dt::root(5), 0, -1}).size() == 1, "a quest that is no track and a zero are skipped");
    CHECK(qs::destiny_answers(book, log, {dt::root(0), dt::root(0), dt::root(0)}).size() == 1, "a track named three times is answered once");
    CHECK(qs::destiny_answers(book, log, {}).empty(), "no track, no answer");
    std::vector<int64_t> many(1000, dt::root(1));
    for (size_t i = 0; i < many.size(); ++i) many[i] = dt::root(static_cast<int>(i % 6));
    CHECK(qs::destiny_answers(book, log, many).size() <= 6, "a thousand elements give at most the six tracks");
    // a track whose own quest the data lacks answers with no guid
    content::QuestData odd = data.quests;
    odd.destiny_tracks.push_back({/*track_quest=*/777, /*quest=*/0, /*skill_tree=*/0, {}});
    const qs::QuestBook odd_book(odd);
    const auto odd_answers = qs::destiny_answers(odd_book, log, {777});
    CHECK(odd_answers.size() == 1 && odd_answers[0].guid.zero() && !odd_answers[0].active && odd_answers[0].total == 0, "a track with no own quest in the data is answered as closed");
}

// ---- the session ---------------------------------------------------------------------------------------------------------------------------

/// A quest session of a world that gives the Destiny tracks (the real worldd turns that on; the tests of the other quests leave it off).
struct DestinyHarness : QuestHarness {
    DestinyHarness(FakeQuestApi& api_, const QuestTestData& data, svc::SavedQuestLog log, svc::Character r = cyb_record()) : QuestHarness(api_, data, std::move(log), std::move(r)) {
        fx.world->set_destiny_tracks(true);   // (the session looks at the tracks on its first quest tick: after the join, so this is in time)
    }
};

/// What the session's quest arrays held for each quest the client was sent: its instance guid and its parent's (the last item of a quest wins).
static std::map<int64_t, std::pair<std::string, std::string>> destiny_sent(const Harness& h) {
    std::map<int64_t, std::pair<std::string, std::string>> out;
    for (const QuestBlock& b : quest_blocks_since(h, 0)) {
        for (const SeenQuest& q : quests_in(b)) out[q.record] = {q.instance, q.parent};
    }
    return out;
}

static BunchWriter destiny_request(const std::vector<int64_t>& tracks) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerGetDestinyClientData");
    if (!rpc) return BunchWriter();
    const auto params = repl::destiny_request_params(*rpc, tracks);
    return params ? actor_rpc(pc, "ServerGetDestinyClientData", *params) : BunchWriter();
}

/// The answers among the chains got[from..], each as the list of its elements (nullopt: a call that does not read).
static std::vector<std::optional<std::vector<repl::DestinyTrackReply>>> destiny_answers_since(const Harness& h, size_t from) {
    std::vector<std::optional<std::vector<repl::DestinyTrackReply>>> out;
    for (const repl::RpcCall& c : pc_calls_since(h, from)) {
        if (c.rpc && std::string(c.rpc->name) == "ClientReceiveDestinyClientData") out.push_back(repl::parse_destiny_client_data(c));
    }
    return out;
}

static void test_destiny_login_level_1() {
    std::printf("destiny: a new character at level 1 gets First Steps and Combat\n");
    namespace dt = destiny_test;
    const QuestTestData data = dt::data();
    FakeQuestApi api;
    qs::Guid128 own0_guid, own5_guid;
    {
        DestinyHarness h(api, data, log_after_start());
        h.play();
        h.run(milliseconds(3500));
        const qs::QuestLog& log = h.s->quest_log();
        CHECK(log.instances().size() == 18 && dt::nothing_else(log, {0, 5}), "the log holds the own quest and the eight tasks of First Steps and of Combat, and nothing else (no master quest, no track quest, no milestone quest)");
        CHECK(h.s->quests_taken() == 18 && h.s->destiny_tracks_seeded() == 2, "eighteen quests were taken, two of them as tracks");
        CHECK(log.quest_variable(dt::own(0), "Destiny_Reward") == 0 && log.quest_variable(dt::own(5), "Destiny_Reward") == 0, "no task is done: Destiny_Reward is 0");
        const qs::QuestInstance* own0 = log.find(dt::own(0));
        const qs::QuestInstance* own5 = log.find(dt::own(5));
        CHECK(own0 && own5 && own0->objectives.size() == 12, "(both own quests are in the log, with their twelve objectives)");
        if (!own0 || !own5) return;
        own0_guid = own0->guid;
        own5_guid = own5->guid;
        // the client's cache: every task names its track's instance as its parent, the own quests have none
        const auto sent = destiny_sent(h);
        bool tasks = true, own_zero = true;
        for (const int t : {0, 5}) {
            const qs::QuestInstance* o = log.find(dt::own(t));
            const auto s = sent.find(dt::own(t));
            own_zero = own_zero && s != sent.end() && s->second.first == o->guid.hex() && s->second.second == qs::Guid128{}.hex();
            for (int i = 0; i < 8; ++i) {
                const auto k = sent.find(dt::task(t, i));
                tasks = tasks && k != sent.end() && k->second.second == o->guid.hex() && k->second.first == log.find(dt::task(t, i))->guid.hex();
            }
        }
        CHECK(sent.size() == 18 && tasks && own_zero, "the client was sent the eighteen quests: each task's cache item carries its track's guid as ParentInstanceGuid, each own quest's carries none");
        CHECK(sent.count(dt::kMaster) == 0 && sent.count(dt::root(0)) == 0 && sent.count(dt::root(5)) == 0 && sent.count(dt::milestone_quest(0, 0)) == 0, "the master quest, the track quests and the milestone quests never reach the client");
        h.run(milliseconds(4000));
        CHECK(log.instances().size() == 18 && h.s->quests_taken() == 18, "a second look at the tracks changes nothing");
        h.saved();
        CHECK(api.log(kCyb).instances.size() == 18, "the log was saved with the eighteen quests");
    }
    // the next login: the same instances, no duplicates, the parents found again
    DestinyHarness again(api, data, api.log(kCyb));
    again.play();
    again.run(milliseconds(3500));
    const qs::QuestLog& log = again.s->quest_log();
    CHECK(log.instances().size() == 18 && again.s->quests_taken() == 0 && again.s->destiny_tracks_seeded() == 0, "a relog gives the eighteen quests back and takes none");
    CHECK(log.find(dt::own(0)) && log.find(dt::own(0))->guid == own0_guid && log.find(dt::own(5)) && log.find(dt::own(5))->guid == own5_guid, "... the own quests with the guids they had");
    const auto sent = destiny_sent(again);
    bool tasks = !sent.empty();
    for (int i = 0; i < 8; ++i) {
        const auto k = sent.find(dt::task(0, i));
        tasks = tasks && k != sent.end() && k->second.second == own0_guid.hex();
    }
    CHECK(tasks, "... and the parents of the tasks are found again from the data and the log");
}

static void test_destiny_world_without_tracks() {
    std::printf("destiny: a world that does not give the tracks leaves the SubQuest objectives alone\n");
    namespace dt = destiny_test;
    const QuestTestData data = dt::data();
    FakeQuestApi api;
    QuestHarness h(api, data, log_after_start(), cyb_record(), /*gm=*/true);   // (the harness of the other quest tests: the tracks are off)
    h.play();
    h.run(milliseconds(3500));
    CHECK(h.s->quest_log().instances().empty() && h.s->destiny_tracks_seeded() == 0, "a level 1 character is given no track");
    api.queue(kCyb, "give", "Destiny_Own_0");
    h.commands();
    h.run(milliseconds(2500));
    const qs::QuestLog& log = h.s->quest_log();
    CHECK(log.instances().size() == 1 && log.find(dt::own(0)) != nullptr && log.find(dt::task(0, 0)) == nullptr, "a quest with SubQuest objectives that a game master starts starts none of their quests");
    const qs::QuestInstance* q = log.find(dt::own(0));
    bool as_before = q != nullptr && q->parent.zero();
    for (int j = 0; q && j < 4; ++j) as_before = as_before && q->objectives[static_cast<size_t>(8 + j)].active;   // (awake objectives start active, the rule or not, as they always did)
    CHECK(as_before, "... and its objectives stay as they were before the tracks: the awake ones start active");
}

static void test_destiny_level_5() {
    std::printf("destiny: level 5 opens the other four tracks\n");
    namespace dt = destiny_test;
    const QuestTestData data = dt::data();
    FakeQuestApi api;
    qs::Guid128 first_steps;
    {
        DestinyHarness h(api, data, log_after_start());
        h.play();
        h.run(milliseconds(3500));
        first_steps = h.s->quest_log().find(dt::own(0)) ? h.s->quest_log().find(dt::own(0))->guid : qs::Guid128{};
        h.saved();
        CHECK(h.s->quest_log().instances().size() == 18, "(a level 1 character with the two tracks)");
    }
    // the character comes back at level 5: the four tracks start beside the two that are there
    svc::Character at5 = cyb_record();
    at5.level = 5;
    {
        DestinyHarness h(api, data, api.log(kCyb), at5);
        h.play();
        h.run(milliseconds(5000));
        CHECK(h.s->progress().level == 5, "(the character is at level 5)");
        const qs::QuestLog& log = h.s->quest_log();
        CHECK(h.s->destiny_tracks_seeded() == 4 && dt::nothing_else(log, {0, 1, 2, 3, 4, 5}), "the other four tracks started, and nothing but the own quests and their tasks is in the log");
        CHECK(log.find(dt::own(0)) && log.find(dt::own(0))->guid == first_steps, "First Steps keeps its instance");
        for (int t = 0; t < 6; ++t) CHECK(dt::in_log(log, t, true, false) == 1, "every track's own quest is in the log once");
        // the level task of every track: done at level 5; Destiny_Reward is 1; no milestone is open
        bool reward_one = true, no_milestone = true;
        for (int t = 0; t < 6; ++t) {
            reward_one = reward_one && log.quest_variable(dt::own(t), "Destiny_Reward") == 1 && log.completions(dt::task(t, 0)) == 1;
            const qs::QuestInstance* o = log.find(dt::own(t));
            for (int j = 0; o && j < 4; ++j) no_milestone = no_milestone && !o->objectives[8 + j].active && log.find(dt::milestone_quest(t, j)) == nullptr;
            const qs::ObjectiveState* done = o ? &o->objectives[0] : nullptr;
            reward_one = reward_one && done && done->completed && done->succeeded;
        }
        CHECK(reward_one, "the level task of each track is done at level 5: its objective of the own quest succeeded and Destiny_Reward is 1");
        CHECK(no_milestone, "the milestones stay inactive and their quests are not in the log");
        // two more tasks of First Steps: Destiny_Reward 3; the milestone at 2 is open, its quest is not started
        api.queue(kCyb, "set-var", {}, "DestinyTask_0_1", 1);
        h.commands();
        h.run(milliseconds(1500));
        const qs::QuestInstance* o = log.find(dt::own(0));
        CHECK(log.quest_variable(dt::own(0), "Destiny_Reward") == 2 && o && o->objectives[8].active && log.find(dt::milestone_quest(0, 0)) == nullptr,
              "a second task done: Destiny_Reward 2, the first milestone is open and no quest is started for it");
        h.saved();
    }
    // a relog at level 5: no duplicates, and the counts are back (the variables are saved with the log)
    DestinyHarness h(api, data, api.log(kCyb), at5);
    h.play();
    h.run(milliseconds(3500));
    const qs::QuestLog& log = h.s->quest_log();
    size_t own_quests = 0;
    for (const qs::QuestInstance& q : log.instances()) {
        for (int t = 0; t < 6; ++t) own_quests += q.record == dt::own(t) ? 1 : 0;
    }
    CHECK(own_quests == 6 && h.s->quests_taken() == 0 && h.s->destiny_tracks_seeded() == 0, "a relog at level 5: six own quests, none taken again");
    CHECK(log.quest_variable(dt::own(0), "Destiny_Reward") == 2, "... and First Steps still counts two");
}

// ---- abandoning ----------------------------------------------------------------------------------------------------------------------------

static void test_destiny_abandon_of_a_done_quest() {
    std::printf("destiny: a quest that is done cannot be abandoned: it leaves the log as finished, once\n");
    namespace dt = destiny_test;
    const QuestTestData data = dt::data();
    FakeQuestApi api;
    svc::Character at5 = cyb_record();
    at5.level = 5;
    std::vector<qs::Guid128> done;   // the level tasks: done at once when the tracks are taken
    {
        DestinyHarness h(api, data, log_after_start(), at5);
        h.settle();
        h.client_sends({h.client.actor(h.pawn_ch, actor_rpc(class_named("PlayerPawn_C"), "ServerClientIsReadyToPhaseIn", {}), true)});
        const qs::QuestLog& log = h.s->quest_log();
        for (int i = 0; i < 400 && done.empty(); ++i) {   // (a step at a time: a done quest stays in the log for 1.5 s and the abandon has to come inside that time)
            h.step();
            for (const qs::QuestInstance& q : log.instances()) {
                if (q.status == qs::Status::Completed) done.push_back(q.guid);
            }
        }
        CHECK(done.size() == 6 && h.s->quests_finished() == 6, "the level task of each of the six tracks is done, and is still in the log");
        if (done.size() != 6) return;
        const size_t refused_before = h.s->quests_refused(), abandoned_before = h.s->quests_abandoned();
        // the exploit: the done level tasks AND the six track quests (still active, so they may go) in one call, then a relog
        std::vector<qs::Guid128> all = done;
        for (int t = 0; t < 6; ++t) all.push_back(log.find(dt::own(t)) ? log.find(dt::own(t))->guid : qs::Guid128{});
        h.client_request(abandon_rpc(all), h.pc_ch);
        bool all_there = true;
        for (const qs::Guid128& g : done) {
            const qs::QuestInstance* q = log.find_guid(g);
            all_there = all_there && q && q->status == qs::Status::Completed;
        }
        CHECK(all_there, "the client's abandon of the six done instances changes nothing: they are in the log, done");
        CHECK(h.s->quests_refused() - refused_before == 6 && h.s->quests_abandoned() - abandoned_before == 6, "... the six done ones are refused, and the six track quests, which are active, are abandoned");
        h.run(milliseconds(2500));
        bool finished_once = true;
        for (int t = 0; t < 6; ++t) finished_once = finished_once && log.completions(dt::task(t, 0)) == 1 && log.find(dt::task(t, 0)) == nullptr;
        CHECK(finished_once, "the six leave the log the way a done quest does, each counted as finished once");
        CHECK(h.s->quests_finished() == 6, "(nothing was done twice)");
        h.saved();
    }
    // the next login: the track quests are taken again (they were abandoned), but the level tasks are finished: they count at once and are not taken again, and nothing is paid again
    DestinyHarness again(api, data, api.log(kCyb), at5);
    again.play();
    again.run(milliseconds(5000));
    const qs::QuestLog& log = again.s->quest_log();
    bool not_again = true;
    for (int t = 0; t < 6; ++t) not_again = not_again && log.completions(dt::task(t, 0)) == 1 && log.find(dt::task(t, 0)) == nullptr;
    CHECK(not_again, "a relog: no level task is taken again, each is still finished once");
    bool counted_once = true;
    for (int t = 0; t < 6; ++t) counted_once = counted_once && log.find(dt::own(t)) && log.quest_variable(dt::own(t), "Destiny_Reward") == 1;
    CHECK(again.s->quests_finished() == 0 && again.s->destiny_tracks_seeded() == 6 && counted_once, "... the six track quests are taken again, each counts its level task once, and no quest is done again, so nothing is paid again");
}

static void test_destiny_abandon_of_an_active_quest() {
    std::printf("destiny: a quest that is active can still be abandoned\n");
    namespace dt = destiny_test;
    const QuestTestData data = dt::data();
    FakeQuestApi api;
    DestinyHarness h(api, data, log_after_start());
    h.play();
    h.run(milliseconds(3500));
    const qs::QuestLog& log = h.s->quest_log();
    const qs::QuestInstance* combat = log.find(dt::own(5));
    CHECK(combat != nullptr && combat->status == qs::Status::Active, "(the Combat track is in the log and active)");
    if (!combat) return;
    const qs::Guid128 g = combat->guid;
    const size_t refused_before = h.s->quests_refused(), abandoned_before = h.s->quests_abandoned();
    h.client_request(abandon_rpc({g}), h.pc_ch);
    CHECK(log.find(dt::own(5)) == nullptr && h.s->quests_abandoned() - abandoned_before == 1 && h.s->quests_refused() == refused_before, "the abandon of an active quest takes it out of the log, as before");
    CHECK(log.completions(dt::own(5)) == 0, "... and does not count it as finished");
}

static void test_destiny_window_answer() {
    std::printf("destiny: the window's answer\n");
    namespace dt = destiny_test;
    const QuestTestData data = dt::data();
    FakeQuestApi api;
    DestinyHarness h(api, data, log_after_start());
    h.play();
    h.run(milliseconds(3500));
    const qs::QuestLog& log = h.s->quest_log();
    std::vector<int64_t> asked;
    for (const int t : dt::kWindowOrder) asked.push_back(dt::root(t));
    size_t from = h.got.size();
    const BunchWriter request = destiny_request(asked);
    DESTINY_CHECKF(request.bit_pos() > 0, "the request of the six tracks is written (%zu bits)", request.bit_pos());
    h.client_request(request, h.pc_ch);
    auto answers = destiny_answers_since(h, from);
    DESTINY_CHECKF(answers.size() == 1 && answers[0] && answers[0]->size() == 6 && h.s->destiny_answers_sent() == 1,
           "the request of the six tracks gets one answer of six elements (%zu answer(s), the first reads %d with %zu element(s); the session answered %zu, GM-less requests refused or dropped: %zu)", answers.size(),
           answers.empty() || !answers[0] ? 0 : 1, answers.empty() || !answers[0] ? size_t(0) : answers[0]->size(), h.s->destiny_answers_sent(), h.s->requests_dropped());
    if (answers.size() != 1 || !answers[0] || answers[0]->size() != 6) return;
    const auto sent = destiny_sent(h);
    bool order = true, first = true, locked = true;
    for (size_t i = 0; i < 6; ++i) {
        const repl::DestinyTrackReply& r = (*answers[0])[i];
        const int t = dt::kWindowOrder[i];
        order = order && r.track_quest == dt::root(t);
        const qs::Guid128 g{r.guid[0], r.guid[1], r.guid[2], r.guid[3]};
        if (dt::kGate[t] <= 1) {
            const qs::QuestInstance* o = log.find(dt::own(t));
            const auto s = sent.find(dt::own(t));
            first = first && o && r.active && g == o->guid && r.total == 8 && r.completed == 0 && s != sent.end() && s->second.first == g.hex();
        } else {
            locked = locked && !r.active && g.zero() && r.total == 8 && r.completed == 0;
        }
    }
    CHECK(order, "the elements come in the window's order: First Steps, Combat, Initiate, Crafting, Gathering, Processing");
    CHECK(first, "First Steps and Combat are open with the guid their own quest's cache item carries, eight tasks and none done");
    CHECK(locked, "the other four are not open and carry no guid");
    // the answer follows the log
    api.queue(kCyb, "set-var", {}, "DestinyTask_0_1", 1);
    h.commands();
    h.run(milliseconds(1500));
    from = h.got.size();
    h.run(milliseconds(600));
    h.client_request(destiny_request({dt::root(0)}), h.pc_ch);
    answers = destiny_answers_since(h, from);
    CHECK(answers.size() == 1 && answers[0] && answers[0]->size() == 1 && (*answers[0])[0].completed == 1 && (*answers[0])[0].active, "a task done: the answer says one of eight");
    // an unknown track, a track twice, none, a thousand: bounded answers
    const auto one_answer = [&](const std::vector<int64_t>& req) -> std::optional<std::vector<repl::DestinyTrackReply>> {
        h.run(milliseconds(700));   // (the budget gives a token every half second)
        const size_t at = h.got.size();
        h.client_request(destiny_request(req), h.pc_ch);
        const auto a = destiny_answers_since(h, at);
        if (a.size() != 1) return std::nullopt;
        return a[0] ? a[0] : std::optional<std::vector<repl::DestinyTrackReply>>(std::vector<repl::DestinyTrackReply>{});
    };
    auto a = one_answer({4242, dt::root(5)});
    CHECK(a && a->size() == 1 && (*a)[0].track_quest == dt::root(5), "an unknown track is skipped, the known one answered");
    a = one_answer({dt::root(0), dt::root(0)});
    CHECK(a && a->size() == 1, "a track named twice is answered once");
    a = one_answer({});
    CHECK(a && a->empty(), "no track: an empty answer");
    // the most elements one packet holds (a request of a thousand does not fit: the client would send it in pieces; the pure answer above takes a thousand)
    const size_t fit = (conn::kMaxPacketBytes * 8 - 800) / 830;
    std::vector<int64_t> many;
    for (size_t i = 0; i < fit; ++i) many.push_back(dt::root(static_cast<int>(i % 6)));
    a = one_answer(many);
    CHECK(!a || a->size() <= 6, "a request that fills a packet with tracks: nothing above the six tracks");
    CHECK(h.s->quests_on() && !h.s->quest_log().instances().empty(), "(the session is alive and its log is as it was)");
    // the requests are budgeted
    h.run(milliseconds(5000));
    const size_t answered = h.s->destiny_answers_sent();
    from = h.got.size();
    std::vector<fake::Bunch> burst;
    for (int i = 0; i < 12; ++i) burst.push_back(h.client.actor(h.pc_ch, destiny_request({dt::root(0)}), true));
    h.client_sends(std::move(burst));
    h.settle();
    const size_t answers_to_burst = destiny_answers_since(h, from).size();
    CHECK(answers_to_burst >= 1 && answers_to_burst <= 6 && h.s->destiny_answers_sent() - answered == answers_to_burst, "twelve requests at once are answered within the budget (not all of them)");
}

// ---- the game's own data ---------------------------------------------------------------------------------------------------------------------

constexpr int64_t kRealMaster = 6064634444438765568ll;           // Destiny_Root_Quest
constexpr int64_t kFirstStepsRoot = 6064634619243986946ll;       // Destiny_Novice_FirstStep_02_Root
constexpr int64_t kFirstStepsOwn = 6064634478640431178ll;        // Destiny_Novice_FirstStep_02
constexpr int64_t kCombatRoot = 6064634693269913600ll;           // Destiny_Novice_Combat_Root
constexpr int64_t kCombatOwn = 6064634676602077184ll;            // Destiny_Novice_Combat

/// The six tracks of the real data (a content.db built with the reader that keeps destiny_track; an older file has none and the test says so).
static void test_destiny_real_data() {
    std::printf("destiny: the game's own tracks\n");
    std::string why;
    const auto data = quest_test_data(why);
    if (!data) { std::printf("  [skip] SKIP: %s\n", why.c_str()); return; }
    if (data->quests.destiny_tracks.empty()) { std::printf("  [skip] SKIP: this content.db has no destiny_track table (rebuild it with this checkout's contentc)\n"); return; }
    const qs::QuestBook book(data->quests);
    CHECK(book.destiny_tracks().size() == 6, "the data lists six tracks");
    const content::DestinyTrackRow* fs = book.destiny_track(kFirstStepsRoot);
    CHECK(fs && fs->quest == kFirstStepsOwn && fs->milestones.size() == 4, "First Steps: its track quest starts Destiny_Novice_FirstStep_02, four milestones");
    const content::DestinyTrackRow* combat = book.destiny_track(kCombatRoot);
    CHECK(combat && combat->quest == kCombatOwn && combat->milestones.size() == 3, "Combat: Destiny_Novice_Combat, three milestones");
    for (const content::DestinyTrackRow& t : book.destiny_tracks()) {
        const content::QuestRow* root = book.quest(t.track_quest);
        const content::QuestRow* own = book.quest(t.quest);
        CHECK(root && own && root->objectives.size() == 1 && root->objectives[0].sub_quest == t.quest && root->objectives[0].sub_kind == "AutoGrant" && root->objectives[0].sub_wait,
              "every track quest has one objective that starts the track's own quest and waits for it");
        size_t tasks = 0;
        bool all_sub = own != nullptr;
        for (size_t i = 0; own && i < own->objectives.size(); ++i) {
            const content::QuestObjectiveRow& o = own->objectives[i];
            all_sub = all_sub && o.type == "SubQuest" && o.sub_quest != 0 && o.sub_kind == "AutoGrant";
            tasks += o.activation.empty() ? 1 : 0;
        }
        CHECK(all_sub && tasks == 8, "every own quest is made of SubQuest objectives that start a quest, eight of them without a rule (the tasks)");
        for (const int64_t m : t.milestones) CHECK(book.destiny_milestone(m), "every milestone objective of the data is known to the book");
    }
    // the master quest's gates: the level opens the tracks (First Steps at 1, four at 5, Combat none)
    const content::QuestRow* master = book.quest(kRealMaster);
    CHECK(master && master->objectives.size() == 6, "the master quest has six objectives, one for each track");
    qs::QuestLog log(5);
    const auto seeds = [&](int level) {
        namespace dt = destiny_test;
        dt::Facts facts(log, level);
        dl::Evaluator ev(*data->book, facts);
        std::vector<int64_t> out;
        for (const content::QuestRow* d : qs::destiny_seeds(book, log, ev)) out.push_back(d->guid);
        return out;
    };
    CHECK(seeds(1) == std::vector<int64_t>({kFirstStepsOwn, kCombatOwn}), "level 1 opens First Steps and Combat");
    CHECK(seeds(4) == std::vector<int64_t>({kFirstStepsOwn, kCombatOwn}), "level 4 opens the same two");
    CHECK(seeds(5).size() == 6, "level 5 opens all six");
    // the chain on the real data: the own quest starts its eight tasks and no milestone
    namespace dt = destiny_test;
    dt::World w(log, book, 1);
    w.seed();
    const qs::QuestInstance* own = log.find(kFirstStepsOwn);
    CHECK(own && own->objectives.size() == 12 && log.find(kCombatOwn), "the two tracks of level 1 are in the log");
    size_t tasks_in_log = 0;
    for (const qs::QuestInstance& q : log.instances()) {
        const content::QuestRow* d = book.quest(q.record);
        tasks_in_log += d && d->flavor == "Destiny" && q.record != kFirstStepsOwn && q.record != kCombatOwn ? 1 : 0;
    }
    CHECK(tasks_in_log == 16 && log.instances().size() == 18, "each own quest started its eight tasks: eighteen quests, no master, no track quest, no milestone quest");
    bool parents = own != nullptr;
    for (const qs::QuestInstance& q : log.instances()) {
        if (q.record == kFirstStepsOwn || q.record == kCombatOwn) parents = parents && q.parent.zero();
        else parents = parents && !q.parent.zero() && (log.find_guid(q.parent) != nullptr);
    }
    CHECK(parents, "the tasks name the instance of their track's own quest, the own quests name none");
}

// ---- the wire ------------------------------------------------------------------------------------------------------------------------------

static void test_destiny_answer_bits() {
    std::printf("destiny: one answer element is the one the game's server sent\n");
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ClientReceiveDestinyClientData");
    CHECK(rpc != nullptr, "(the PlayerController has ClientReceiveDestinyClientData)");
    if (!rpc) return;
    // a recorded answer of the game's server to the window's first request: the First Steps element, 330 bits (Destiny_Novice_FirstStep_02_Root, the guid of its own quest's instance, 8 of 8, open),
    // the bytes as the capture holds them from the element's first bit
    static const char* const kElement = "020093ec0de9295488d6ef50f2506d5d012ef38c101f87d2e8db92630a3f4c2f84110000001000000002";
    std::vector<uint8_t> recorded;
    for (const char* h = kElement; h[0] && h[1]; h += 2) recorded.push_back(static_cast<uint8_t>(std::stoi(std::string(h, 2), nullptr, 16)));
    repl::DestinyTrackReply r;
    r.track_quest = 6064634619243986946ll;
    r.guid[0] = 2286320023u;
    r.guid[1] = 4100539279u;
    r.guid[2] = 2234632557u;
    r.guid[3] = 3256329759u;
    r.completed = 8;
    r.total = 8;
    r.active = true;
    const auto params = repl::destiny_client_data_params(*rpc, std::span<const repl::DestinyTrackReply>(&r, 1));
    CHECK(params.has_value(), "the answer's parameters build");
    if (!params) return;
    BunchWriter w;
    bool failed = false;
    const size_t bits = repl::write_rpc_params(*rpc, *params, w, {}, &failed);
    CHECK(!failed && bits > 330, "the parameters write");
    if (failed || bits <= 330) return;
    const auto bit_of = [](const uint8_t* data, size_t i) { return (data[i >> 3] >> (i & 7)) & 1; };
    size_t differ = 0;
    for (size_t i = 0; i < 330; ++i) differ += bit_of(w.data(), bits - 330 + i) != bit_of(recorded.data(), i) ? 1 : 0;
    CHECK(differ == 0, "the element (the last 330 bits of the parameters) is the recorded element bit for bit");
    std::printf("  [info] one element: %zu parameter bits in all, %zu of them before the element\n", bits, bits - 330);
    // the same element read back by our reader, and the request's reader on a request we write
    size_t pos = 0;
    const auto back = repl::read_rpc_params(*rpc, w.data(), bits, pos);
    repl::RpcCall call;
    call.rpc = rpc;
    if (back) { call.params = *back; call.decoded = true; }
    const auto parsed = repl::parse_destiny_client_data(call);
    // the request the client writes reads back as the tracks it names
    {
        const schema::RpcSchema* req = class_named("AoCPlayerControllerBP_C").rpc("ServerGetDestinyClientData");
        const std::vector<int64_t> named = {6064634619243986946ll, 6064634693269913600ll, 6064634619255848966ll};
        const auto req_params = req ? repl::destiny_request_params(*req, named) : std::nullopt;
        BunchWriter rw;
        bool req_failed = false;
        const size_t req_bits = req_params ? repl::write_rpc_params(*req, *req_params, rw, {}, &req_failed) : 0;
        size_t req_pos = 0;
        const auto req_back = req_bits ? repl::read_rpc_params(*req, rw.data(), req_bits, req_pos) : std::nullopt;
        repl::RpcCall req_call;
        req_call.rpc = req;
        if (req_back) { req_call.params = *req_back; req_call.decoded = true; }
        const std::vector<int64_t> back_named = repl::parse_destiny_request(req_call, 16);
        DESTINY_CHECKF(!req_failed && req_bits > 0 && req_back.has_value() && back_named == named, "the request reads back as the tracks it names (%zu bits, read %d, %zu track(s))", req_bits, req_back ? 1 : 0, back_named.size());
    }
    CHECK(parsed && parsed->size() == 1 && (*parsed)[0].track_quest == r.track_quest && (*parsed)[0].guid[0] == r.guid[0] && (*parsed)[0].guid[3] == r.guid[3] && (*parsed)[0].completed == 8 &&
              (*parsed)[0].total == 8 && (*parsed)[0].active,
          "the reader gets the element back");
}

AOC_REGISTER_TEST(test_destiny_sub_quest_engine);
AOC_REGISTER_TEST(test_destiny_seeds);
AOC_REGISTER_TEST(test_destiny_answers_pure);
AOC_REGISTER_TEST(test_destiny_world_without_tracks);
AOC_REGISTER_TEST(test_destiny_login_level_1);
AOC_REGISTER_TEST(test_destiny_level_5);
AOC_REGISTER_TEST(test_destiny_abandon_of_a_done_quest);
AOC_REGISTER_TEST(test_destiny_abandon_of_an_active_quest);
AOC_REGISTER_TEST(test_destiny_window_answer);
AOC_REGISTER_TEST(test_destiny_real_data);
AOC_REGISTER_TEST(test_destiny_answer_bits);
