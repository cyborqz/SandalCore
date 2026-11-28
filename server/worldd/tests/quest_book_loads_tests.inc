// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/quest_book_loads_tests.inc: the quest book of the real content.db must load, and what it says of kill queries (quests milestone, the Destiny window)
//  (registered with AOC_REGISTER_TEST: test_worldd_area_session.cpp runs them through aoc_tests::run; included from area_session_registered.inc after destiny_session_tests.inc,
//  whose engine harness it uses)
//
//  The other quest tests of this program read the real content.db and SKIP when its quests do not read, so a file with one row worldd cannot read left every one of them
//  skipped and the run green, while worldd started with its quests off.  test_quest_book_loads is the test that refuses that: with AOC_CONTENT_DB naming a file that has quest
//  tables, it loads the quest book the way worldd does (ContentDb::open, then quest_data) and FAILS when the book does not load.  On success it says how many quests and
//  objectives it read.  It skips only when AOC_CONTENT_DB is not set, names no file, or the file has no quest tables.
//
//  test_quest_kill_queries_real is the same data at work.  A tag query that names no tag is no query ("all tags match" of none is true, so it would count every kill): the Kill
//  objectives of the data whose query has no tag keep none, and count by their creatures (instances and sets) alone; the First Steps "Kill 10 Enemies", whose query has a tag
//  (any character), still counts every creature.
// ============================================================================
#include <system_error>

/// A check whose message says the reason or the numbers (the area session program has no CHECKF of its own).
#define QUEST_BOOK_CHECKF(cond, ...) do { char quest_book_msg[1024]; std::snprintf(quest_book_msg, sizeof quest_book_msg, __VA_ARGS__); CHECK(cond, quest_book_msg); } while (0)

namespace quest_book_load_test {

constexpr int64_t kKillEnemies = 6064634478936391775ll;   // First Steps, "Kill 10 Enemies": the query ANY(character)

/// How many of the two quest tables (quest, quest_objective) the file has; -1 (with `why`) when it cannot be read as a database.
inline int quest_tables_of(const std::string& path, std::string& why) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        why = db ? sqlite3_errmsg(db) : "out of memory";
        if (db) sqlite3_close(db);
        return -1;
    }
    int n = -1;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT count(*) FROM sqlite_master WHERE type = 'table' AND name IN ('quest', 'quest_objective')", -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
        n = sqlite3_column_int(st, 0);
    } else {
        why = sqlite3_errmsg(db);
    }
    if (st) sqlite3_finalize(st);
    sqlite3_close(db);
    return n;
}

/// The guids of the Kill objectives (one per row of the file) whose tag query names no tag: the text of their detail says `"tags":[]`.
inline std::vector<int64_t> bare_kill_queries_of(const std::string& path) {
    std::vector<int64_t> out;
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return out;
    }
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT guid FROM quest_objective WHERE obj_type = 'Kill' AND detail LIKE '%\"gq\":{\"tok\":[%],\"tags\":[]}%' ORDER BY quest_guid, idx", -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) out.push_back(sqlite3_column_int64(st, 0));
    }
    if (st) sqlite3_finalize(st);
    sqlite3_close(db);
    return out;
}

/// The quest book of the file AOC_CONTENT_DB names, as worldd loads it.  False (nothing more to check) when the variable is not set, names no file or the file has no quest
/// tables (a skip, said), and when the book does not load (a FAIL, said): that is the one that must never pass.
struct Loaded {
    std::string                       path;
    std::optional<content::ContentDb> db;
    std::optional<content::QuestData> data;
};

inline bool load(Loaded& out) {
    const char* env = std::getenv("AOC_CONTENT_DB");
    if (!env || !*env) { std::printf("  [skip] SKIP: AOC_CONTENT_DB is not set\n"); return false; }
    out.path = env;
    std::error_code ec;
    if (!fs::exists(out.path, ec)) { std::printf("  [skip] SKIP: AOC_CONTENT_DB names no file (%s)\n", out.path.c_str()); return false; }
    std::string why;
    const int tables = quest_tables_of(out.path, why);
    QUEST_BOOK_CHECKF(tables >= 0, "the file AOC_CONTENT_DB names reads as a database: %s", why.c_str());
    if (tables < 0) return false;
    if (tables == 0) { std::printf("  [skip] SKIP: this content.db has no quest tables\n"); return false; }
    out.db = content::ContentDb::open(out.path, why);
    QUEST_BOOK_CHECKF(out.db.has_value(), "the content.db opens: %s", why.c_str());
    if (!out.db) return false;
    std::string error;
    out.data = out.db->quest_data(error);
    QUEST_BOOK_CHECKF(out.data.has_value(), "the quests load (worldd starts with its quests off when they do not): %s", error.c_str());
    return out.data.has_value();
}

}  // namespace quest_book_load_test

static void test_quest_book_loads() {
    std::printf("quest book: the content.db that AOC_CONTENT_DB names loads its quests the way worldd loads them\n");
    quest_book_load_test::Loaded in;
    if (!quest_book_load_test::load(in)) return;
    const qs::QuestBook book(*in.data);
    size_t objectives = 0, kill_queries = 0;
    for (const auto& [guid, q] : in.data->quests) {
        objectives += q.objectives.size();
        for (const content::QuestObjectiveRow& o : q.objectives) kill_queries += o.kill_query ? 1 : 0;
    }
    std::printf("  [info] the quest book read %zu quests, %zu objectives, %zu kill queries by tag, %zu destiny tracks (%s)\n", book.quests(), objectives, kill_queries,
                in.data->destiny_tracks.size(), in.path.c_str());
    CHECK(book.quests() > 0 && objectives > 0, "the quest book holds quests and their objectives");
}

static void test_quest_kill_queries_real() {
    std::printf("quest book: a Kill objective whose tag query names no tag keeps no query and counts by its creatures alone\n");
    namespace qbl = quest_book_load_test;
    namespace dt = destiny_test;
    qbl::Loaded in;
    if (!qbl::load(in)) return;
    const content::QuestData& data = *in.data;

    // no query the book keeps is empty
    size_t kept = 0, empty = 0;
    for (const auto& [guid, q] : data.quests) {
        for (const content::QuestObjectiveRow& o : q.objectives) {
            if (!o.kill_query) continue;
            ++kept;
            empty += o.kill_query->tags.empty() ? 1 : 0;
        }
    }
    QUEST_BOOK_CHECKF(kept > 0 && empty == 0, "the book keeps %zu kill queries by tag, and none of them is empty (%zu are)", kept, empty);

    // the objectives of the file whose query names no tag: none has a query in the book, and none says it has one
    const std::vector<int64_t> bare = qbl::bare_kill_queries_of(in.path);
    const std::set<int64_t> bare_guids(bare.begin(), bare.end());
    size_t rows = 0, with_query = 0, says_query = 0, played = 0, no_creature = 0, outside_counted = 0, inside_missed = 0;
    int64_t first_bad = 0;
    int64_t next_quest = 6064639800000000000ll;
    for (const auto& [guid, q] : data.quests) {
        for (const content::QuestObjectiveRow& o : q.objectives) {
            if (bare_guids.count(o.guid) == 0) continue;
            ++rows;
            with_query += o.kill_query ? 1 : 0;
            says_query += o.tag_query ? 1 : 0;
            if (o.kill_owner || o.use_context || (o.advanced && o.elite > 0)) continue;   // the engine does not count these kills at all (quest-owner, quest-spawned, elite rank)

            // a creature the objective names: one of its instances, or one that stands in one of its sets
            int64_t inside = o.kill_instances.empty() ? 0 : o.kill_instances.front();
            for (auto it = data.kill_sets.begin(); inside == 0 && it != data.kill_sets.end(); ++it) {
                for (const int64_t s : it->second) {
                    if (std::find(o.kill_sets.begin(), o.kill_sets.end(), s) != o.kill_sets.end()) { inside = it->first; break; }
                }
            }
            if (inside == 0) { ++no_creature; continue; }

            // the objective alone in a quest of its own, active, asking for many kills
            content::QuestData one;
            one.kill_sets = data.kill_sets;
            content::QuestRow row = dt::quest(++next_quest, "KillQueryCheck");
            row.flavor = "";
            content::QuestObjectiveRow obj = o;
            obj.awake = true;
            obj.hidden = false;
            obj.activation = content::DialogueRequirement();
            obj.on_start.clear();
            obj.on_success.clear();
            obj.count = 100;
            row.objectives.push_back(std::move(obj));
            one.quests[row.guid] = std::move(row);
            const qs::QuestBook one_book(std::move(one));
            qs::QuestLog log(31);
            dt::World w(log, one_book, 1);
            w.run([&](qs::Engine& e) { e.start(*one_book.quest(next_quest), 0); });
            const std::set<std::string> beast = {"character", "character.beast", "character.beast.wolf"};
            const auto count = [&]() { const qs::QuestInstance* qi = log.find(next_quest); return qi && !qi->objectives.empty() ? qi->objectives[0].count : -1; };
            w.run([&](qs::Engine& e) { e.kill(7, 0, beast); });   // a creature of no set and no instance of the data
            const bool outside_ok = count() == 0;
            w.run([&](qs::Engine& e) { e.kill(inside, 0, beast); });
            const bool inside_ok = count() == 1;
            ++played;
            outside_counted += outside_ok ? 0 : 1;
            inside_missed += inside_ok ? 0 : 1;
            if ((!outside_ok || !inside_ok) && first_bad == 0) first_bad = o.guid;
        }
    }
    std::printf("  [info] %zu Kill objectives (rows) have a query with no tag; %zu of them were played through the engine, %zu name no creature the data lists\n", rows, played, no_creature);
    QUEST_BOOK_CHECKF(rows > 0 && rows == bare.size(), "the book has every one of the %zu rows of the file whose kill query names no tag (found %zu)", bare.size(), rows);
    QUEST_BOOK_CHECKF(with_query == 0 && says_query == 0, "none of them has a kept query (%zu) or says it has one (%zu): they count as they did before the queries", with_query, says_query);
    QUEST_BOOK_CHECKF(played > 0 && outside_counted == 0 && inside_missed == 0,
                      "a kill of a creature outside the objective's instances and sets counts nothing (%zu counted), and one of its own counts once (%zu missed), first objective at fault %lld; %zu played",
                      outside_counted, inside_missed, static_cast<long long>(first_bad), played);

    // the First Steps "Kill 10 Enemies": its query has a tag (any character), so every creature counts
    const content::QuestObjectiveRow* enemies = nullptr;
    for (const auto& [guid, q] : data.quests) {
        for (const content::QuestObjectiveRow& o : q.objectives) {
            if (o.guid == qbl::kKillEnemies && !enemies) enemies = &o;
        }
    }
    CHECK(enemies && enemies->kill_query && enemies->kill_query->tags == std::vector<std::string>({"character"}), "First Steps' Kill 10 Enemies keeps its query: any character");
    if (!enemies || !enemies->kill_query) return;
    content::QuestData one;
    content::QuestRow row = dt::quest(++next_quest, "KillEnemiesCheck");
    row.flavor = "";
    content::QuestObjectiveRow obj = *enemies;
    obj.awake = true;
    obj.hidden = false;
    obj.activation = content::DialogueRequirement();
    obj.on_start.clear();
    obj.on_success.clear();
    obj.count = 100;
    row.objectives.push_back(std::move(obj));
    one.quests[row.guid] = std::move(row);
    const qs::QuestBook one_book(std::move(one));
    qs::QuestLog log(32);
    dt::World w(log, one_book, 1);
    w.run([&](qs::Engine& e) { e.start(*one_book.quest(next_quest), 0); });
    w.run([&](qs::Engine& e) { e.kill(101, 0, {"character", "character.beast", "character.beast.wolf"}); });
    w.run([&](qs::Engine& e) { e.kill(102, 0, {"character", "character.undead", "character.undead.zombie"}); });
    w.run([&](qs::Engine& e) { e.kill(103, 2, {"character", "character.humanoid", "affiliation.bandit"}); });
    const qs::QuestInstance* qi = log.find(next_quest);
    QUEST_BOOK_CHECKF(qi && !qi->objectives.empty() && qi->objectives[0].count == 3, "a beast, an undead and an elite humanoid each count once for Kill 10 Enemies (counted %d)", qi && !qi->objectives.empty() ? qi->objectives[0].count : -1);
}

AOC_REGISTER_TEST(test_quest_book_loads);
AOC_REGISTER_TEST(test_quest_kill_queries_real);
