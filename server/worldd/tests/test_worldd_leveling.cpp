// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_leveling.cpp: the level curve, the XP grants, the item uses and content.db's plan D1 rows (worldd plan D1, stage 1)
//
//  Pure logic on the fixture's content.db (content_db_fixture.h: the real Adventuring curve, the GM scrolls' real rows, the Universal
//  tree's auto grants) and, with AOC_CONTENT_DB, on the real file:
//
//    the curve          thresholds, the level of a total, the cap (25) and the most XP the tracker holds, the span of a level
//    the scrolls' XP    the formula contentc resolved: (curve(target) - curve(level) + debt) / max(0.001, 1 + mult) + 10
//    a grant of XP      the debt paid first, the levels crossed, the ranks they grant in order, the cap and an overflow past it
//    the uses           a level scroll, a class scroll (one's own class refused), a gear scroll, a consumables scroll, a scroll that is
//                       not consumed, an action of another type, an unmodelled table, a table whose predicate the level fails, an item
//                       that is not used
//    the readers        item_use, action_response, reward_table, leveling_type_named, auto_grants on the fixture; the plan B3 review
//                       fixes: a stat_program row whose crc32 is the real one + 2^32 is refused, and a set bonus, quality curve or stat
//                       block holding text or a fraction where an integer or number belongs is refused, as the Go readers refuse it
// ============================================================================
#include "content/content_db.h"
#include "content_db_fixture.h"
#include "session/item_use.h"
#include "session/leveling.h"
#include "stats_fixture.h"

#include <sqlite3.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace content = aoc::worldd::content;
namespace leveling = aoc::worldd::leveling;
namespace use = aoc::worldd::use;
namespace cf = content_fixture;
namespace gm = content_fixture::gm;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

static bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

static std::optional<content::ContentDb> open_fixture(const fs::path& dir, const char* name, const cf::DbOptions& o = {}) {
    const fs::path p = dir / name;
    if (!cf::make_db(p, o)) return std::nullopt;
    std::string error;
    auto db = content::ContentDb::open(p.string(), error);
    if (!db) std::printf("    the fixture does not open: %s\n", error.c_str());
    return db;
}
// -- Cyb x C-O-A --

static leveling::LevelCurve curve_of(const content::ContentDb& db) {
    std::string error;
    const auto type = db.leveling_type_named("Adventuring", error);
    if (!type) {
        std::printf("  [FAIL] no Adventuring leveling type: %s\n", error.c_str());
        std::exit(2);
    }
    const auto curve = leveling::LevelCurve::from(*type, error);
    if (!curve) {
        std::printf("  [FAIL] the curve does not build: %s\n", error.c_str());
        std::exit(2);
    }
    return *curve;
}

// ---- the curve ---------------------------------------------------------------------------------------------------------------

static void test_curve(const content::ContentDb& db) {
    std::printf("the Adventuring level curve\n");
    const leveling::LevelCurve c = curve_of(db);
    CHECK(c.max_level() == 26 && c.cap() == 25 && c.guid() == content::kGuidAdventuring, "26 levels on the curve, the cap 25 (LevelCapOverride)");
    CHECK(c.xp_for(1) == 0 && c.xp_for(2) == 300 && c.xp_for(10) == 418400 && c.xp_for(25) == 14295400 && c.xp_for(26) == 16563400 && c.xp_for(99) == 16563400 && c.xp_for(0) == 0,
          "the thresholds: 0, 300 ... 418,400 at 10, 14,295,400 at 25, 16,563,400 at 26 (a level past the curve: the last)");
    CHECK(c.level_of(0) == 1 && c.level_of(299) == 1 && c.level_of(300) == 2 && c.level_of(418399) == 9 && c.level_of(418400) == 10 && c.level_of(14295400) == 25 &&
              c.level_of(16563400) == 25 && c.level_of(1LL << 60) == 25,
          "the level of a total: the highest threshold reached, held at the cap");
    CHECK(c.max_xp() == 16563399, "the most XP the tracker holds: one below level 26's threshold");
    CHECK(c.span_of(1) == 300 && c.span_of(9) == 418400 - 246600 && c.span_of(26) == 16563400 - 14295400 && c.span_of(0) == 300, "GetPlayerXPForCurrentLevel: the span to the next level");

    // a curve that is not one
    content::LevelingType bad;
    bad.name = "Bad";
    bad.curve = {{2, 0}, {3, 10}};
    std::string error;
    CHECK(!leveling::LevelCurve::from(bad, error) && contains(error, "level 1"), "a curve that does not start at level 1 is refused");
    bad.curve = {{1, 0}, {3, 10}};
    CHECK(!leveling::LevelCurve::from(bad, error) && contains(error, "skips"), "a curve that skips a level is refused");
    bad.curve = {{1, 0}, {2, 10}, {3, 5}};
    CHECK(!leveling::LevelCurve::from(bad, error), "a curve that goes down is refused");
    bad.curve = {{1, 0}, {2, 10}};
    bad.level_cap = 5;
    CHECK(!leveling::LevelCurve::from(bad, error) && contains(error, "cap"), "a cap past the curve is refused");
    bad.level_cap = 0;
    const auto two = leveling::LevelCurve::from(bad, error);
    CHECK(two && two->cap() == 2 && two->max_xp() == 10, "no cap: the curve's last level, and the tracker holds its last total");
}

// ---- the scrolls' XP and a grant --------------------------------------------------------------------------------------------------

static void test_xp(const content::ContentDb& db) {
    std::printf("the level scrolls' XP and what a grant does\n");
    const leveling::LevelCurve c = curve_of(db);
    std::string error;
    const auto grants = db.auto_grants(content::kGuidAdventuring, error);
    if (!grants) {
        std::printf("  [FAIL] no auto grants: %s\n", error.c_str());
        return;
    }
    std::vector<content::AutoGrant> universal;
    for (const content::AutoGrant& g : *grants) {
        if (g.tree_guid == content::kGuidUniversalTree) universal.push_back(g);
    }
    CHECK(universal.size() == 8 && grants->size() == 9, "the fixture: the eight Universal grants of the real tree (and a Rogue one that is not the Universal tree's)");

    leveling::Progress p;   // level 1, 0 XP
    CHECK(leveling::scroll_xp(c, 10, p, 0, 10) == 418410 && leveling::scroll_xp(c, 15, p, 0, 10) == 1853010 && leveling::scroll_xp(c, 20, p, 0, 10) == 6146410 &&
              leveling::scroll_xp(c, 25, p, 0, 10) == 14295410,
          "at level 1 with no debt and no multiplier: the four scrolls give the threshold + 10");
    leveling::Progress at9{9, 300000, 0};
    CHECK(leveling::scroll_xp(c, 10, at9, 0, 10) == 418400 - 246600 + 10, "at level 9 (300,000 XP): curve(10) - curve(9) + 10, whatever the XP into the level");
    leveling::Progress in_debt{9, 300000, 1000};
    CHECK(leveling::scroll_xp(c, 10, in_debt, 0, 10) == 418400 - 246600 + 1000 + 10, "a debt of 1,000: the formula's debt term is the debt itself");
    CHECK(leveling::scroll_xp(c, 10, p, 0.5, 10) == static_cast<int64_t>(418400 / 1.5 + 10), "AdventuringXPAddMult 0.5: the grant is divided by 1.5");
    CHECK(leveling::scroll_xp(c, 10, p, -5, 10) == static_cast<int64_t>(418400 / 0.001 + 10), "a multiplier below -1: Max(0.001, ...)");
    CHECK(leveling::scroll_xp(c, 10, leveling::Progress{10, 418400, 0}, 0, 10) == 0 && leveling::scroll_xp(c, 10, leveling::Progress{12, 800000, 0}, 0, 10) == 0,
          "at or past the target: nothing");
    CHECK(!leveling::scroll_xp(c, 27, p, 0, 10) && !leveling::scroll_xp(c, 0, p, 0, 10), "a target off the curve: nullopt");
// C_Y_B : CoA

    // a grant: 1 -> 10 with Sprint Training I (5) and Endurance Training I (10)
    leveling::XpResult r = leveling::add_xp(c, universal, p, 418410);
    CHECK(r.after.level == 10 && r.after.xp == 418410 && r.after.debt == 0 && r.leveled() && r.changed(), "418,410 XP at level 1: level 10, the total 418,410");
    CHECK(r.granted.size() == 2 && r.granted[0]->level_requirement == 5 && r.granted[0]->rank_guid == gm::kSprint1Rank && r.granted[1]->level_requirement == 10 &&
              r.granted[1]->rank_guid == gm::kStamBoost1Rank,
          "the levels crossed grant Sprint Training I (5) and Endurance Training I (10), in order");
    CHECK(r.granted[1]->mods.size() == 1 && r.granted[1]->mods[0].stat_guid == content::kGuidMaxStamina && r.granted[1]->mods[0].value == 10,
          "Endurance Training I's mod: MaxStamina +10");
    // on to 15: Guard Training I only
    leveling::XpResult r2 = leveling::add_xp(c, universal, r.after, *leveling::scroll_xp(c, 15, r.after, 0, 10));
    CHECK(r2.after.level == 15 && r2.granted.size() == 1 && r2.granted[0]->rank_guid == gm::kGuard1Rank, "10 -> 15 grants Guard Training I alone");
    // a debt is paid first
    leveling::XpResult r3 = leveling::add_xp(c, universal, leveling::Progress{1, 0, 100}, 150);
    CHECK(r3.after.debt == 0 && r3.after.xp == 50 && r3.after.level == 1, "a grant of 150 with a debt of 100 pays the debt and adds 50");
    leveling::XpResult r4 = leveling::add_xp(c, universal, leveling::Progress{1, 0, 100}, 60);
    CHECK(r4.after.debt == 40 && r4.after.xp == 0 && r4.changed() && !r4.leveled(), "a grant smaller than the debt only shrinks the debt");
    // the cap and an overflow
    leveling::XpResult r5 = leveling::add_xp(c, universal, leveling::Progress{20, 6146410, 0}, 1LL << 62);
    CHECK(r5.after.level == 25 && r5.after.xp == c.max_xp() && r5.granted.empty(),
          "an XP overflow: the level stops at the cap 25, the total just below level 26, and no Universal rank is granted from 21 to 25 (Endurance Training II is at 30)");
    leveling::XpResult r6 = leveling::add_xp(c, universal, r5.after, 1LL << 62);
    CHECK(r6.after == r5.after && !r6.changed() && r6.granted.empty(), "another overflow at the cap changes nothing");
    CHECK(leveling::add_xp(c, universal, p, 0).after == p && leveling::add_xp(c, universal, p, -5).after == p, "zero or negative XP changes nothing");
    // a record whose level is above its XP keeps the level
    leveling::XpResult r7 = leveling::add_xp(c, universal, leveling::Progress{12, 0, 0}, 10);
    CHECK(r7.after.level == 12 && r7.after.xp == 10 && !r7.leveled(), "a level above the XP's never goes down");

    // granted_by and progress_of
    const auto at10 = leveling::granted_by(universal, 10);
    CHECK(at10.size() == 2 && at10[0]->level_requirement == 5 && at10[1]->level_requirement == 10 && leveling::granted_by(universal, 4).empty() &&
              leveling::granted_by(universal, 50).size() == 8,
          "granted_by: the nodes a level reaches, by level");
    CHECK(leveling::progress_of(c, 10, std::nullopt, 0) == (leveling::Progress{10, 418400, 0}) && leveling::progress_of(c, 10, 500000, 7) == (leveling::Progress{10, 500000, 7}) &&
              leveling::progress_of(c, 10, 100, 0) == (leveling::Progress{10, 418400, 0}) && leveling::progress_of(c, 30, 0, 0).level == 25 &&
              leveling::progress_of(c, 0, 0, -3) == (leveling::Progress{1, 0, 0}) && leveling::progress_of(c, 25, 1LL << 60, 0).xp == c.max_xp(),
          "progress_of: a record's level with its XP row at or past the threshold, else the threshold; the level clamped to 1..cap; the XP to the most held");
}

// ---- the uses ---------------------------------------------------------------------------------------------------------------------

static void test_uses(const content::ContentDb& db) {
    std::printf("using the GM items\n");
    const leveling::LevelCurve c = curve_of(db);
    use::Context bard;
    bard.class_enum = 2;
    bard.progress = {1, 0, 0};
    bard.curve = &c;

    use::Plan p = use::plan_use(db, gm::kLevel10, bard);
    CHECK(p.ok && p.consume && p.adventuring_xp == 418410 && p.items.empty() && !p.set_class, "the level-10 scroll at level 1: consumed, +418,410 XP, nothing else");
    CHECK(p.notes.size() == 1 && contains(p.notes[0], "Weapon"), "its Weapon XP reward is a note (stage 1 levels Adventuring only)");
    CHECK(contains(p.summary, "418,410 XP"), "the summary says the XP");
    use::Context at12 = bard;
    at12.progress = {12, 800000, 0};
    p = use::plan_use(db, gm::kLevel10, at12);
    CHECK(!p.ok && contains(p.refusal, "below level 10") && contains(p.refusal, "level 12"), "the level-10 scroll at level 12: refused by the table's predicate, nothing consumed");
    p = use::plan_use(db, gm::kLevel25, at12);
    CHECK(p.ok && p.adventuring_xp == 14295400 - 788400 + 10, "the level-25 scroll (no predicate) at level 12: curve(25) - curve(12) + 10");
    use::Context at25 = bard;
    at25.progress = {25, 14295400, 0};
    p = use::plan_use(db, gm::kLevel25, at25);
    CHECK(!p.ok && contains(p.refusal, "grant nothing"), "the level-25 scroll at level 25: nothing to grant, refused");
    use::Context boosted = bard;
    boosted.xp_add_mult = 1.0;
    p = use::plan_use(db, gm::kLevel10, boosted);
    CHECK(p.ok && p.adventuring_xp == 418400 / 2 + 10, "AdventuringXPAddMult 1.0 halves the grant");

    p = use::plan_use(db, gm::kClassBard, bard);
    CHECK(!p.ok && contains(p.refusal, "a Bard already"), "the Bard scroll for a Bard: refused");
    p = use::plan_use(db, gm::kClassTank, bard);
    CHECK(p.ok && p.set_class && *p.set_class == 7 && p.consume && p.adventuring_xp == 0 && p.items.empty() && contains(p.summary, "Tank"), "the Tank scroll for a Bard: the class becomes Tank (7), the scroll is consumed");
    CHECK(use::class_enum_of(db, 108640158570105ll) == 2 && use::class_enum_of(db, 108640201692798ll) == 7 && use::class_enum_of(db, 0) == 0 && use::class_enum_of(db, 42) == 0,
          "class_enum_of: the Bard's and the Tank's class records, 0 for none");

    p = use::plan_use(db, gm::kGear01, bard);
    CHECK(p.ok && p.consume && p.items.size() == 2 && p.items[0].record == cf::item::kChest && p.items[1].record == cf::item::kLegs && p.items[0].quantity == 1 &&
              p.items[0].quality == 1 && p.items[0].durability == 11,
          "the gear scroll: the chest and the legs, one each, quality 1, durability 11 (the record's MaxDurability)");
    p = use::plan_use(db, gm::kConsumables, bard);
    CHECK(p.ok && p.items.size() == 1 && p.items[0].record == cf::item::kPotion && p.items[0].quantity == 20, "the consumables scroll: 20 potions in one stack (MaxStackSize 20)");
    p = use::plan_use(db, gm::kKeep, bard);
    CHECK(p.ok && !p.consume && p.items.size() == 1, "a scroll whose archetype does not consume: not consumed");
    p = use::plan_use(db, gm::kOdd, bard);
    CHECK(!p.ok && contains(p.refusal, "Callout") && contains(p.refusal, "stage 1 does not run"), "an action of type Callout: refused, saying the type");
    p = use::plan_use(db, gm::kRandom, bard);
    CHECK(!p.ok && contains(p.refusal, "not of the shape"), "a table that is not modelled: refused");
    p = use::plan_use(db, cf::item::kPotion, bard);
    CHECK(!p.ok && contains(p.refusal, "neither worn in a slot nor used"), "an item with no activation archetype: refused");
    p = use::plan_use(db, 42, bard);
    CHECK(!p.ok, "an item that is none: refused");
    use::Context no_curve = bard;
    no_curve.curve = nullptr;
    p = use::plan_use(db, gm::kLevel10, no_curve);
    CHECK(!p.ok && contains(p.refusal, "no Adventuring level curve"), "a level scroll with no curve to compute against: refused");
}

// ---- the readers and the plan B3 fixes ------------------------------------------------------------------------------------------

static bool run_sql(const fs::path& path, const char* sql) {
    sqlite3* raw = nullptr;
    bool ok = sqlite3_open_v2(path.string().c_str(), &raw, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK;
    ok = ok && cf::exec(raw, sql);
    if (raw) sqlite3_close(raw);
    return ok;
}

static void test_readers(const fs::path& dir, const content::ContentDb& db) {
    std::printf("content.db's plan D1 rows and the plan B3 review fixes\n");
    std::string error;
    const auto u = db.item_use(gm::kLevel10, error);
    CHECK(u && u->activation == "ActivateActionResponses" && u->consume_on_use && u->actions.size() == 1 && u->actions[0] == 6064634615529013375ll && error.empty(),
          "item_use: the level-10 scroll's archetype and action");
    CHECK(!db.item_use(cf::item::kPotion, error) && error.empty(), "an item that is not used: nullopt, no error");
    const auto a = db.action_response(6064632988317974535ll, error);
    CHECK(a && a->response_type == "SetPrimaryArchetype" && a->class_guid == 108640201692798ll && a->class_name == "Tank" && a->reward_tables.empty(), "action_response: SetPrimaryArchetypeTank");
    const auto t = db.reward_table(6064634615529865344ll, error);
    CHECK(t && t->modelled && t->level_below == 10 && t->items.empty() && t->experience.size() == 2 && t->experience[0].experience_type == "Adventuring" &&
              t->experience[0].target_level == 10 && t->experience[0].margin == 10 && t->experience[0].curve_guid == gm::kAdventuringCurve &&
              t->experience[0].mult_stat_guid == gm::kXPAddMult && t->experience[1].target_level == 0,
          "reward_table: the level-10 table, its predicate as 10, the two XP rewards");
    const auto r = db.reward_table(7100000000000000031ll, error);
    CHECK(r && !r->modelled && r->level_below == -1, "a table that is not modelled, with a predicate contentc does not read");
    CHECK(!db.reward_table(42, error) && error.empty() && !db.action_response(42, error) && error.empty(), "a table or action that is none: nullopt, no error");
    const auto lt = db.leveling_type_named("Adventuring", error);
    CHECK(lt && lt->guid == content::kGuidAdventuring && lt->level_cap == 25 && lt->uses_debt && lt->xp_mult_stat_guid == gm::kXPAddMult && lt->curve.size() == 26 &&
              lt->curve[9] == std::make_pair(10, int64_t{418400}),
          "leveling_type_named: Adventuring with its 26 keys");
    CHECK(!db.leveling_type_named("Mining", error) && contains(error, "Mining"), "a leveling type that is none: an error naming it");
    const auto g = db.auto_grants(content::kGuidAdventuring, error);
    CHECK(g && g->size() == 9 && (*g)[0].level_requirement == 5 && (*g)[0].skill_guid == gm::kSprint1Skill && (*g)[1].mods.size() == 1 && (*g)[1].mods[0].value == 10 &&
              (*g)[0].mods.empty(),
          "auto_grants: by level, with the ranks' constant mods (Endurance Training's; Sprint Training's finite effect has none)");
    const auto item = db.item(cf::item::kPotion, error);
    CHECK(item && item->max_stack_size == 20 && item->default_quality == 1 && item->max_durability == 0, "item: MaxStackSize, DefaultQuality and MaxDurability come through");

    // plan B3 fix (a): a crc32 of the real value + 2^32 is refused, not folded into the real one by a cast
    {
        const fs::path p = dir / "crc.db";
        bool ok = cf::make_db(p);
        ok = ok && run_sql(p, "UPDATE stat_program SET crc32 = crc32 + 4294967296");
        auto bad = ok ? content::ContentDb::open(p.string(), error) : std::nullopt;
        error.clear();
        const auto row = bad ? bad->stat_program(error) : std::nullopt;
        CHECK(ok && bad && !row && contains(error, "crc32"), "a stat_program row with crc32 = the real one + 2^32 is refused (it used to pass through a narrowing cast)");
        ok = run_sql(p, "UPDATE stat_program SET crc32 = crc32 - 4294967296, stats = 'many'");
        error.clear();
        const auto row2 = ok && bad ? bad->stat_program(error) : std::nullopt;
        CHECK(ok && !row2 && contains(error, "storage class"), "a stat count that is text is refused");
    }
    // plan B3 fix (b): the item tables refuse text and fractions where the Go readers do
    {
        const fs::path p = dir / "classes.db";
        bool ok = cf::make_db(p);
        ok = ok && run_sql(p, "INSERT INTO set_bonus (set_guid, tier_ord, pieces, bonus_ord, stat_guid, common) VALUES "
                              "(7005, 0, 'two', 0, 109183598614138, 10), (7006, 0, 2.5, 0, 109183598614138, 10), (7007, 0, 2, 0, 109183598614138, 'ten'), "
                              "(7008, 0, 0, 0, 109183598614138, 10), (7009, 0, 84, 0, 109183598614138, 10), (7010, 0, 2, 0, 109183598614138, 10)");
        ok = ok && run_sql(p, "UPDATE stat_block_stat SET common_min = 'x' WHERE block_guid = 7000000000000000003");
        ok = ok && run_sql(p, "UPDATE quality_curve SET y = 'five' WHERE guid = 109166871729784 AND ord = 0");
        auto bad = ok ? content::ContentDb::open(p.string(), error) : std::nullopt;
        CHECK(ok && bad.has_value(), "the fixture with the bad rows opens (the checks are per query)");
        if (bad) {
            for (const auto& [guid, what] : std::vector<std::pair<int64_t, const char*>>{{7005, "text pieces"}, {7006, "fractional pieces"}, {7007, "a text value"},
                                                                                        {7008, "0 pieces"}, {7009, "84 pieces"}}) {
                error.clear();
                const auto s = bad->set_bonus(guid, error);
                CHECK(!s && !error.empty(), (std::string("set_bonus with ") + what + " is refused with an error").c_str());
            }
            error.clear();
            const auto good = bad->set_bonus(7010, error);
            CHECK(good && good->tiers.size() == 1 && good->tiers[0].pieces == 2 && error.empty(), "a good set bonus beside them reads");
            error.clear();
            CHECK(!bad->item_gear(cf::item::kMace1H, error) && contains(error, "not a number"), "a stat block value that is text is refused");
            error.clear();
            CHECK(!bad->item_gear(cf::item::kHeavyChest, error) && contains(error, "not a number"), "a quality curve band that is text is refused");
        }
    }
}
// C:Y:B - C.O.A

// ---- the real content.db -----------------------------------------------------------------------------------------------------------

static void test_real() {
    const char* path = std::getenv("AOC_CONTENT_DB");
    if (!path || !*path) {
        std::printf("[skip] AOC_CONTENT_DB not set: the real content.db is not checked\n");
        return;
    }
    std::printf("the real content.db (%s)\n", path);
    std::string error;
    auto db = content::ContentDb::open(path, error);
    if (!db) {
        std::printf("  [FAIL] %s\n", error.c_str());
        g_fail++;
        return;
    }
    const leveling::LevelCurve c = curve_of(*db);
    CHECK(c.max_level() == 26 && c.cap() == 25 && c.xp_for(25) == 14295400 && c.max_xp() == 16563399, "Adventuring: 26 levels, cap 25, 14,295,400 XP at 25");
    const auto grants = db->auto_grants(content::kGuidAdventuring, error);
    size_t universal = 0;
    bool stam = false;
    if (grants) {
        for (const content::AutoGrant& g : *grants) {
            if (g.tree_guid != content::kGuidUniversalTree) continue;
            ++universal;
            if (g.node_name == "General_Stamina_Passive_StamBoost_1") stam = g.mods.size() == 1 && g.mods[0].stat_guid == content::kGuidMaxStamina && g.mods[0].value == 10;
        }
    }
    CHECK(grants && universal == 8 && stam, "the Universal tree's eight auto grants, Endurance Training I with MaxStamina +10");
    use::Context bard;
    bard.class_enum = 2;
    bard.progress = {1, 0, 0};
    bard.curve = &c;
    const int64_t scrolls[] = {gm::kLevel10, gm::kLevel15, gm::kLevel20, gm::kLevel25};
    const int64_t want[] = {418410, 1853010, 6146410, 14295410};
    bool all = true;
    for (int i = 0; i < 4; ++i) {
        const use::Plan p = use::plan_use(*db, scrolls[i], bard);
        all = all && p.ok && p.consume && p.adventuring_xp == want[i];
    }
    CHECK(all, "the four level scrolls at level 1: 418,410, 1,853,010, 6,146,410 and 14,295,410 XP, consumed");
    const use::Plan gear = use::plan_use(*db, gm::kGear01, bard);
    CHECK(gear.ok && gear.items.size() == 19 && gear.consume, "the starter gear scroll: 19 items");
    const use::Plan tank = use::plan_use(*db, gm::kClassTank, bard);
    CHECK(tank.ok && tank.set_class == 7, "the Tank scroll: class 7");
    const use::Plan cons = use::plan_use(*db, gm::kConsumables, bard);
    size_t stacks = 0;
    for (const use::Granted& g : cons.items) stacks += static_cast<size_t>(g.quantity);
    CHECK(cons.ok && !cons.items.empty() && stacks >= 20, "the consumables scroll grants stacks");
}

int main() {
    const fs::path dir = cf::make_temp_dir("aoc_worldd_leveling_test_");
    {
        auto db = open_fixture(dir, "fixture.db");
        if (!db) {
            std::printf("  [FAIL] the fixture does not open\n");
            return 1;
        }
        test_curve(*db);
        test_xp(*db);
        test_uses(*db);
        test_readers(dir, *db);
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
    test_real();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
