// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_professions.cpp: the professions and the GM artisan items (Gathering stage 2, part "Professions and GM items")
//
//  Pure logic on a small book of professions made here, and on the real content.db of the working tree (dist/content/content.db, or AOC_CONTENT_DB):
//
//    the rules        level from XP, the tier's cap and the XP held at it, Promote (only at the cap), Demote, the character record's rows (missing, corrected)
//    the reader       the 22 professions in the order of the retail join, the five certification levels, the 98 certification actions, the Profession rewards'
//                     expressions and the profession each names
//    the 67 scrolls   every GM level scroll (22 x Level 9, 22 x Level 15, 22 + `_All` x Level 25) planned from a fresh character with the real expressions, the
//                     real curves and the real stat engine: the final level, tier and XP against an independent model of the data's own action order (the XP is
//                     held at the tier's cap until a Promote), the T3 tools of the five gathering professions, `_All` modelled as its description says
//    the refusals     a scroll that would change nothing, a profession reward with no expression runner, a certification action with no professions
//    the runner       GetPlayerProfessionLevel answers the real level (the scroll's XP is the XP from the start of the current level)
//    the tools        the five T3 tools of the Level 25 scrolls are worn in their slots (gather_tool agrees); the GM vendors list all 67 scrolls and the Starter and T1 tools
// ============================================================================
#include "content/content_db.h"
#include "session/ability_catalog.h"
#include "session/item_use.h"
#include "session/player_world.h"
#include "session/professions.h"

#include <sqlite3.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace aoc::worldd;
namespace fs = std::filesystem;
namespace pr = aoc::worldd::professions;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)
#define SAYOK(msg) std::printf("  [ok ] %s\n", msg)

static bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

// ---- a small book: three professions on the real curve, the real five certification levels --------------------------------------------------------------

static constexpr int64_t kMining = 108752769996425ll, kLumber = 108752769996426ll, kAlchemy = 108752757020295ll;
static constexpr int64_t kLtMining = 109460719621753ll, kLtLumber = 109273709237880ll, kLtAlchemy = 109460747540106ll;
static constexpr int64_t kCertGuid[5] = {108906339653240ll, 108906340767353ll, 108906342012538ll, 108906343061115ll, 108906343782012ll};

static content::LevelingType curve400(int64_t guid, const char* name) {
    content::LevelingType t;
    t.guid = guid;
    t.name = name;
    t.experience_type = "Gathering";
    for (int l = 1; l <= 50; ++l) t.curve.emplace_back(l, l == 1 ? 0 : 400ll * l * l);
    return t;
}

static content::ProfessionData small_data() {
    content::ProfessionData d;
    d.professions.push_back({kAlchemy, "Alchemy", "Processing", kLtAlchemy, 1, curve400(kLtAlchemy, "LevelingTypeDef_Alchemist")});
    d.professions.push_back({kMining, "Mining", "Gathering", kLtMining, 2, curve400(kLtMining, "LevelingTypeDef_Miner")});
    d.professions.push_back({kLumber, "Lumberjacking", "Gathering", kLtLumber, 3, curve400(kLtLumber, "LevelingTypeDef_Lumberjack")});
    const char* names[5] = {"Novice", "Apprentice", "Journeyman", "Master", "GrandMaster"};
    const int allow[5] = {0, 5, 4, 3, 2};
    for (int t = 0; t < 5; ++t) d.certifications.push_back({kCertGuid[t], names[t], t, t == 0 ? 1 : 10 * t, 10 * (t + 1), t > 0, allow[t]});
    d.actions[7001] = {7001, "Promote Mining", kMining, true};
    d.actions[7002] = {7002, "Demote Mining", kMining, false};
    return d;
}
// (CoA + C:Y:B)

static void test_book_and_rules() {
    std::printf("the professions' rules on a small book\n");
    std::string error;
    const content::ProfessionData data = small_data();
    const auto book = pr::Book::from(data, error);
    CHECK(book.has_value(), "the small book builds");
    if (!book) {
        std::printf("    %s\n", error.c_str());
        return;
    }
    CHECK(book->size() == 3 && book->all()[0].guid == kAlchemy && book->all()[2].guid == kLumber, "the professions are kept in guid order of the data");
    CHECK(book->find(kMining) && book->find(kMining)->name == "Mining" && book->named("Mining") == book->find(kMining) && book->named("MINING") == book->find(kMining) && !book->find(1) &&
              !book->named("Fishing"),
          "find and named (the name without regard to case)");
    CHECK(book->tier(0).max_level == 10 && book->tier(1).max_level == 20 && book->tier(4).max_level == 50 && book->tier(9).tier == 4 && book->tier(-1).tier == 0, "the tiers' caps; a tier past the ends is clamped");
    CHECK(book->tier_of_guid(kCertGuid[2]) == 2 && book->tier_of_guid(5) == -1, "the tier of a certification guid");
    CHECK(book->action(7001) && book->action(7001)->promote && book->action(7002) && !book->action(7002)->promote && !book->action(7003), "the certification actions");
    const pr::Profession& mining = *book->find(kMining);
    CHECK(book->max_xp(mining, 0) == 400ll * 11 * 11 - 1 && book->max_xp(mining, 1) == 400ll * 21 * 21 - 1 && book->max_xp(mining, 4) == 400ll * 50 * 50,
          "the XP a tier holds: one below the threshold of the level past its cap; the curve's last total at level 50");
    CHECK(book->level_of(mining, 0, 0) == 1 && book->level_of(mining, 1599, 0) == 1 && book->level_of(mining, 1600, 0) == 2 && book->level_of(mining, 40000, 0) == 10 &&
              book->level_of(mining, 900000, 0) == 10 && book->level_of(mining, 900000, 1) == 20 && book->level_of(mining, 900000, 4) == 47,
          "the level of an XP total: the curve's, held at the tier's cap");

    pr::Professions p(*book);
    CHECK(p.valid() && p.level(kMining) == 1 && p.tier(kMining) == 0 && p.xp(kMining) == 0 && p.level(12345) == 1 && p.tier(12345) == 0, "a new set is Novice, level 1, no XP (an unknown profession too)");
    CHECK(p.can_use(kMining, 0, 1) && !p.can_use(kMining, 1, 1) && !p.can_use(kMining, 0, 2) && !p.can_use(999, 0, 1), "can_use: tier and level at least; an unknown profession can use nothing");

    pr::Change c = p.add_xp(kMining, 1600);
    CHECK(c.profession == kMining && c.before.level == 1 && c.after.level == 2 && c.leveled() && !c.promoted() && p.level(kMining) == 2 && p.xp(kMining) == 1600, "1,600 XP is level 2");
    c = p.add_xp(kMining, 0);
    CHECK(!c.changed() && p.xp(kMining) == 1600, "no XP is no change");
    c = p.add_xp(kMining, -5);
    CHECK(!c.changed() && p.xp(kMining) == 1600, "XP is never taken away");
    c = p.add_xp(999, 100);
    CHECK(!c.changed() && c.profession == 0, "XP to an unknown profession changes nothing");
    c = p.add_xp(kMining, 100000);
    CHECK(c.after.level == 10 && c.after.xp == 48399 && p.xp(kMining) == 48399 && p.level(kMining) == 10, "a Novice's XP is held at the Novice cap (level 10, one XP below level 11)");
    CHECK(!p.add_xp(kMining, 5).changed(), "a Novice at the cap gains nothing");

    bool skipped = false;
    c = p.promote(kLumber, true, &skipped);
    CHECK(skipped && !c.changed() && p.tier(kLumber) == 0, "a Promote below the tier's cap is skipped");
    c = p.promote(kMining, true, &skipped);
    CHECK(!skipped && c.promoted() && c.after.tier == 1 && c.after.level == 10 && p.tier(kMining) == 1 && c.moved(), "a Promote at the cap is one tier up, the level kept");
    c = p.add_xp(kMining, 1000000);
    CHECK(c.after.level == 20 && c.after.xp == 400ll * 21 * 21 - 1, "an Apprentice's XP is held at the Apprentice cap");
    c = p.promote(kMining, false);
    CHECK(c.after.tier == 2 && p.tier(kMining) == 2, "a Promote that does not need the cap");
    for (int i = 0; i < 2; ++i) {
        p.add_xp(kMining, 100000000);
        p.promote(kMining);
    }
    p.add_xp(kMining, 100000000);
    c = p.promote(kMining, true, &skipped);
    CHECK(p.tier(kMining) == 4 && p.level(kMining) == 50 && p.xp(kMining) == 1000000 && !c.changed(), "a GrandMaster stays; level 50 holds the curve's last total");
    c = p.demote(kMining);
    CHECK(c.demoted() && c.after.tier == 3 && c.after.level == 40 && c.after.xp == 400ll * 41 * 41 - 1, "a Demote is one tier down: the level and the XP are held at the lower cap");
    pr::Professions n(*book);
    CHECK(!n.demote(kMining).changed() && !n.demote(1).changed(), "a Novice stays; an unknown profession changes nothing");
    c = n.raise_xp_to(kAlchemy, 5000);
    CHECK(c.after.xp == 5000 && c.after.level == 3 && !n.raise_xp_to(kAlchemy, 100).changed() && n.xp(kAlchemy) == 5000, "raise_xp_to raises, never lowers");

    // the levels the formulas read, the rows the character service stores, the differences
    const auto levels = p.levels_by_name();
    CHECK(levels.size() == 3 && levels.at("Mining") == 40 && levels.at("Alchemy") == 1 && levels.at("Lumberjacking") == 1, "levels_by_name: what GetPlayerProfessionLevel reads");
    const auto rows = p.to_svc();
    CHECK(rows.size() == 3 && rows[0].profession_guid == kAlchemy && rows[1].profession_guid == kMining && rows[1].certification_tier == 3 && rows[1].level == 40 &&
              rows[1].xp == 400ll * 41 * 41 - 1 && rows[2].level == 1,
          "to_svc: every profession, in guid order");
    CHECK(pr::Professions().to_svc().empty() && !pr::Professions().valid() && !pr::Professions().add_xp(kMining, 5).changed() && pr::Professions().level(kMining) == 1,
          "a set with no book holds nothing and changes nothing");
    const std::vector<pr::Change> d = pr::Professions::diff(pr::Professions(*book), p);
    CHECK(d.size() == 1 && d[0].profession == kMining && d[0].before.tier == 0 && d[0].after.tier == 3, "diff: only the professions that differ");

    // the rows of the character record
    std::vector<std::string> notes;
    std::vector<svc::ProfessionState> stored = {{kMining, 1, 12, 60000}, {kLumber, 9, 99, -5}, {kAlchemy, 0, 30, 100}, {777, 2, 5, 5}, {kMining, 2, 3, 3}};
    const pr::Professions loaded = pr::Professions::load(*book, stored, &notes);
    CHECK(loaded.tier(kMining) == 1 && loaded.level(kMining) == 12 && loaded.xp(kMining) == 60000, "a good row is kept");
    CHECK(loaded.tier(kLumber) == 4 && loaded.level(kLumber) == 50 && loaded.xp(kLumber) == 400ll * 50 * 50, "a tier past GrandMaster is GrandMaster; the level is held at the curve and the XP at its threshold");
    CHECK(loaded.tier(kAlchemy) == 0 && loaded.level(kAlchemy) == 10 && loaded.xp(kAlchemy) == 40000,
          "a level past the Novice cap is the cap, with the XP at its threshold");
    CHECK(!notes.empty(), "what was corrected, left out or repeated is said");
    const pr::Professions none = pr::Professions::load(*book, {}, nullptr);
    CHECK(none.valid() && none.level(kMining) == 1 && none.tier(kMining) == 0 && none.xp(kMining) == 0, "no rows at all is Novice, level 1 everywhere");
    const pr::Professions low = pr::Professions::load(*book, std::vector<svc::ProfessionState>{{kMining, 0, 5, 0}}, nullptr);
    CHECK(low.level(kMining) == 5 && low.xp(kMining) == 400ll * 5 * 5, "a level whose XP trails it keeps the level, the XP at its threshold");

    // the profession a reward's expression is for
    CHECK(pr::profession_named_in("const xp = EvalCurve($#7037:6064$, 25.0);\r\nconst cur = EvalCurve($#7037:6064$, (1.0 * GetPlayerProfessionLevel(GetPlayer(), \"Mining\")));\r\nxp - cur + 1") == "Mining" &&
              pr::profession_named_in("1 + 1").empty() && pr::profession_named_in("GetPlayerProfessionLevel(GetPlayer(), \"\")").empty(),
          "the profession named in an expression's GetPlayerProfessionLevel");
    SAYOK("the rules on a small book");
}

// ---- the real file -------------------------------------------------------------------------------------------------------------------------------------
// C-O-A + C-Y-B

static std::vector<std::pair<int64_t, std::string>> items_like(const std::string& path, const char* pattern) {
    std::vector<std::pair<int64_t, std::string>> out;
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) return out;
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT guid, name FROM item WHERE name LIKE ?1 ORDER BY name", -1, &s, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, pattern, -1, SQLITE_TRANSIENT);
        while (sqlite3_step(s) == SQLITE_ROW) out.emplace_back(sqlite3_column_int64(s, 0), reinterpret_cast<const char*>(sqlite3_column_text(s, 1)));
    }
    sqlite3_finalize(s);
    sqlite3_close(db);
    return out;
}

static void test_reader(const content::ContentDb& db) {
    std::printf("the real content.db's professions\n");
    std::string error;
    const auto data = db.profession_data(error);
    CHECK(data.has_value(), "profession_data reads");
    if (!data) {
        std::printf("    %s\n", error.c_str());
        return;
    }
    CHECK(data->professions.size() == 22 && data->certifications.size() == 5, "22 professions and 5 certification levels");
    // the retail join's order (1198 pkt 1510's LevelingCertificationsFastMap): the keys are the professions' leveling types, in the order of the CharacterProfessions guids
    const int64_t retail_order[22] = {108651600696952ll, 109460729190018ll, 109460729190017ll, 109460729190016ll, 109460729190015ll, 109460729190014ll, 109460729190013ll,
                                      109460729190012ll, 109460747540099ll, 109460747540100ll, 109460747540101ll, 109460747540102ll, 109460747540103ll, 109460747540104ll,
                                      109460747540105ll, 109460747540106ll, 109460747540107ll, 109460719621753ll, 109273709237880ll, 109460724995707ll, 109460713264760ll,
                                      109460722964090ll};
    bool order = data->professions.size() == 22;
    for (size_t i = 0; order && i < 22; ++i) order = data->professions[i].leveling_type_guid == retail_order[i];
    CHECK(order, "the professions' leveling types are in the order of the retail join's certification map");
    bool curves = true;
    for (const content::ProfessionRow& p : data->professions) {
        curves = curves && p.leveling.curve.size() == 50 && p.leveling.curve[0].second == 0 && p.leveling.curve[9].second == 40000 && p.leveling.curve[49].second == 1000000 &&
                 p.leveling.curve[24].second == 250000;
    }
    CHECK(curves, "every profession's curve has 50 levels: 0 at level 1, 40,000 at 10, 250,000 at 25, 1,000,000 at 50");
    const int max[5] = {10, 20, 30, 40, 50}, allow[5] = {0, 5, 4, 3, 2};
    bool tiers = true;
    for (int t = 0; t < 5 && data->certifications.size() == 5; ++t) {
        tiers = tiers && data->certifications[static_cast<size_t>(t)].tier == t && data->certifications[static_cast<size_t>(t)].max_level == max[t] &&
                data->certifications[static_cast<size_t>(t)].max_allowable == allow[t] && data->certifications[static_cast<size_t>(t)].guid == kCertGuid[t];
    }
    CHECK(tiers, "the certification levels: Novice 10, Apprentice 20, Journeyman 30, Master 40, GrandMaster 50; at most 5, 4, 3, 2 professions above Novice");
    int promotes = 0, demotes = 0;
    for (const auto& [guid, a] : data->actions) (a.promote ? promotes : demotes)++;
    CHECK(data->actions.size() == 98 && promotes == 70 && demotes == 28, "98 certification actions: 70 Promote, 28 Demote");

    const auto book = pr::Book::from(*data, error);
    CHECK(book.has_value(), "the real book builds");
    if (!book) {
        std::printf("    %s\n", error.c_str());
        return;
    }
    // the Mining Level 25 scroll's first reward line: a compiled expression that names Mining
    const auto mining25 = items_like(db.path(), "GM_Scroll_MakeMeLevel25Artisanship_Mining");
    CHECK(mining25.size() == 1, "the Mining Level 25 scroll is in the file");
    if (mining25.size() == 1) {
        const auto use = db.item_use(mining25[0].first, error);
        CHECK(use && use->actions.size() == 6, "it has 6 actions: XP, Promote, XP, Promote, XP, the T3 tool");
        if (use && use->actions.size() == 6) {
            const auto xp_action = db.action_response(use->actions[0], error);
            const auto table = xp_action && !xp_action->reward_tables.empty() ? db.reward_table(xp_action->reward_tables[0], error) : std::nullopt;
            CHECK(table && table->experience.size() == 1 && table->experience[0].experience_type == "Profession" && table->experience[0].expr > 0 &&
                      pr::profession_named_in(table->experience[0].expression) == "Mining",
                  "its first action's reward is a Profession XP line with a compiled expression for Mining");
            const auto cert_action = db.action_response(use->actions[1], error);
            CHECK(cert_action && cert_action->response_type == "SetCertificationLevel" && book->action(cert_action->guid) && book->action(cert_action->guid)->promote &&
                      book->action(cert_action->guid)->profession == kMining,
                  "its second action is a Promote of Mining");
        }
    }
    // all 88 Profession XP lines name a profession of the book, and have a compiled expression
    sqlite3* raw = nullptr;
    int lines = 0, named = 0, compiled = 0;
    if (sqlite3_open_v2(db.path().c_str(), &raw, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(raw, "SELECT expression, expr FROM reward_xp WHERE experience_type = 'Profession'", -1, &s, nullptr) == SQLITE_OK) {
            while (sqlite3_step(s) == SQLITE_ROW) {
                ++lines;
                const std::string name = pr::profession_named_in(reinterpret_cast<const char*>(sqlite3_column_text(s, 0)));
                named += book->named(name) != nullptr;
                if (!book->named(name)) std::printf("    no profession for '%s'\n", name.c_str());
                compiled += sqlite3_column_int64(s, 1) > 0;
            }
        }
        sqlite3_finalize(s);
        sqlite3_close(raw);
    }
    CHECK(lines == 88 && named == 88 && compiled == 88, "all 88 Profession XP lines name one of the 22 professions and have a compiled expression");
    SAYOK("the real content.db's professions");
}
// C O A / Cyb

// ---- the 67 scrolls --------------------------------------------------------------------------------------------------------------------------------------

namespace model {
// An independent model of the data's own action order: the XP of a line is (curve(target) - curve(level)) + 1 added to the XP, held at the tier's cap; a Promote
// takes effect at the cap. curve(L) = 400 L^2 (level 1: 0).
struct M {
    int     tier = 0;
    int     level = 1;
    int64_t xp = 0;
};
inline int64_t curve(int l) { return l <= 1 ? 0 : 400ll * l * l; }
inline int cap(int tier) { return 10 * (tier + 1); }
inline int64_t hold(int tier) { return tier >= 4 ? curve(50) : curve(cap(tier) + 1) - 1; }
inline int level_of(int64_t xp, int tier) {
    int l = 1;
    while (l < cap(tier) && xp >= curve(l + 1)) ++l;
    return l;
}
inline void xp_line(M& m, int target) {
    const int64_t delta = curve(target) - curve(m.level) + 1;
    if (delta > 0) m.xp = std::min(hold(m.tier), m.xp + delta);
    m.level = level_of(m.xp, m.tier);
}
inline void promote(M& m) {
    if (m.tier < 4 && m.level == cap(m.tier)) ++m.tier;
}
}  // namespace model

static std::string normalized(std::string s) {
    std::string out;
    for (char c : s) {
        if (c != ' ' && c != '_') out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (out == "scribing") out = "scribe";
    return out;
}

static void test_scrolls(const std::string& path, PlayerWorld& world) {
    std::printf("the 67 GM level scrolls on a fresh character\n");
    const content::ContentDb& db = *world.content();
    const pr::Book* book = world.professions();
    CHECK(book != nullptr && book->size() == 22, "the world has the 22 professions");
    if (!book || !world.ability_catalog() || !world.stat_bundle()) {
        std::printf("  [FAIL] the world has no professions, ability catalog or stat bundle: cannot plan the scrolls\n");
        ++g_fail;
        return;
    }
    // the expression runner over a level-1 Bard's stats
    const bootstrap::StatContext sc = world.stat_context();
    stats::CharacterStats cs(*sc.bundle, *sc.gear, 2, 1, sc.rounding);
    std::string err;
    CHECK(cs.start({}, err), "a level 1 Bard's stats start");
    pr::XpRunner runner = pr::make_xp_runner(*world.ability_catalog(), cs, {}, err);
    CHECK(static_cast<bool>(runner), "the expression runner builds");
    if (!runner) return;

    use::Context ctx;
    ctx.class_enum = 2;
    ctx.curve = world.level_curve();
    ctx.professions = book;
    ctx.held = pr::Professions(*book);
    ctx.profession_xp = runner;

    const std::set<std::string> gathering = {"fishing", "herbalism", "hunting", "lumberjacking", "mining"};
    int checked = 0, wrong = 0, tools = 0, sets = 0;
    for (const int level : {9, 15, 25}) {
        const std::string pattern = "GM_Scroll_MakeMeLevel" + std::to_string(level) + "Artisanship_%";
        for (const auto& [guid, name] : items_like(path, pattern.c_str())) {
            const std::string suffix = normalized(name.substr(name.find("Artisanship_") + 12));
            if (suffix == "all") continue;
            const pr::Profession* prof = nullptr;
            for (const pr::Profession& p : book->all()) {
                if (normalized(p.name) == suffix) prof = &p;
            }
            ++checked;
            if (!prof) {
                std::printf("    no profession for %s\n", name.c_str());
                ++wrong;
                continue;
            }
            const use::Plan plan = use::plan_use(db, guid, ctx);
            model::M m;
            if (level == 9) {
                model::xp_line(m, 9);
            } else if (level == 15) {
                model::xp_line(m, 15);
                model::promote(m);
                model::xp_line(m, 15);
            } else {
                model::xp_line(m, 25);
                model::promote(m);
                model::xp_line(m, 25);
                model::promote(m);
                model::xp_line(m, 25);
            }
            const bool tool = level == 25 && gathering.count(suffix) > 0;
            const pr::State got = plan.professions_after.valid() ? plan.professions_after.state(prof->guid) : pr::State{};
            const bool ok = plan.ok && plan.consume && got.level == m.level && got.tier == m.tier && got.xp == m.xp && plan.items.size() == (tool ? 1u : 0u) &&
                            plan.profession_changes.size() == 1 && plan.profession_changes[0].profession == prof->guid && plan.adventuring_xp == 0 && !plan.set_class;
            if (!ok) {
                ++wrong;
                std::printf("    %s: ok=%d level %d (want %d) tier %d (want %d) xp %lld (want %lld) items %zu (want %d) changes %zu%s%s\n", name.c_str(), plan.ok, got.level, m.level, got.tier,
                            m.tier, static_cast<long long>(got.xp), static_cast<long long>(m.xp), plan.items.size(), tool ? 1 : 0, plan.profession_changes.size(),
                            plan.ok ? "" : " refusal: ", plan.refusal.c_str());
            }
            // the other 21 professions are untouched
            bool untouched = true;
            for (const pr::Profession& other : book->all()) {
                if (other.guid != prof->guid) untouched = untouched && plan.professions_after.state(other.guid) == pr::State{};
            }
            CHECK(untouched, "a scroll changes only its own profession");
            tools += tool && plan.items.size() == 1;
            sets += plan.ok;
        }
    }
    CHECK(checked == 66 && wrong == 0, "the 22 + 22 + 22 single-profession scrolls: the final level, tier and XP are the model's (Level 9: level 9 Novice; Level 15: level 15 Apprentice; Level 25: level 25 Journeyman)");
    CHECK(checked == 66 && sets == 66 && tools == 5, "all 66 are used; the Level 25 scrolls of Fishing, Herbalism, Hunting, Lumberjacking and Mining give a T3 tool each");
    {
        // the T3 tool of Mining is the Journeyman pickaxe
        const auto mining = items_like(path, "GM_Scroll_MakeMeLevel25Artisanship_Mining");
        const use::Plan plan = use::plan_use(db, mining[0].first, ctx);
        CHECK(plan.ok && plan.items.size() == 1 && plan.items[0].record == 6064629376467927053ll && plan.items[0].quantity == 1, "the Mining Level 25 scroll gives Gear_Artisan_Mining_Tool_Pickaxe_T3_0");
        CHECK(contains(plan.summary, "Mining") && contains(plan.summary, "25") && contains(plan.summary, "Journeyman"), "its summary says the profession, the level and the tier");
    }
// C O A : Cyb

    // `_All`: modelled as its description says (level 25 in all 22 disciplines), whatever the data's single Promote per profession leaves
    {
        const auto all = items_like(path, "GM_Scroll_MakeMeLevel25Artisanship_All");
        CHECK(all.size() == 1, "the Level 25 _All scroll is in the file");
        if (all.size() == 1) {
            const use::Plan plan = use::plan_use(db, all[0].first, ctx);
            bool every = plan.ok && plan.professions_after.valid() && plan.profession_changes.size() == 22;
            for (const pr::Profession& p : book->all()) {
                const pr::State s = plan.professions_after.state(p.guid);
                every = every && s.level == 25 && s.tier == 2 && s.xp == 250000;
            }
            if (!every) {
                std::printf("    ok=%d refusal='%s' changes=%zu\n", plan.ok, plan.refusal.c_str(), plan.profession_changes.size());
                for (const pr::Profession& p : book->all()) {
                    const pr::State s = plan.professions_after.state(p.guid);
                    std::printf("    %s: tier %d level %d xp %lld\n", p.name.c_str(), s.tier, s.level, static_cast<long long>(s.xp));
                }
                for (const std::string& n : plan.notes) std::printf("    note: %s\n", n.c_str());
            }
            CHECK(every, "_All: all 22 professions are level 25, Journeyman, 250,000 XP (the data's single Promote leaves them Apprentice at level 10)");
            bool modelled = false, ignores = false;
            for (const std::string& n : plan.notes) {
                modelled = modelled || contains(n, "modelled");
                ignores = ignores || contains(n, "MaximumAllowable");
            }
            CHECK(modelled && ignores && plan.items.empty(), "_All says it is modelled and that a GM item ignores the certification's limit; it gives no item");
            std::printf("    _All: %s\n", plan.summary.c_str());
        }
    }

    // the cap interleaving: the XP of a Level 25 scroll is held at each tier's cap until the next Promote
    {
        const auto mining = items_like(path, "GM_Scroll_MakeMeLevel25Artisanship_Mining");
        const use::Plan plan = use::plan_use(db, mining[0].first, ctx);
        const pr::State s = plan.professions_after.state(kMining);
        CHECK(s.level == 25 && s.tier == 2 && s.xp == 266400, "Level 25 from Novice: 250,001 held at 48,399; +210,001 held at 176,399; +90,001 is 266,400 (level 25)");
    }

    // a profession further on: the Promote is skipped below the cap, a scroll that changes nothing is refused
    {
        const auto l9 = items_like(path, "GM_Scroll_MakeMeLevel9Artisanship_Mining");
        use::Context later = ctx;
        later.held = pr::Professions::load(*book, std::vector<svc::ProfessionState>{{kMining, 1, 11, 400ll * 11 * 11}}, nullptr);
        const use::Plan again = use::plan_use(db, l9[0].first, later);
        CHECK(!again.ok && contains(again.refusal, "Mining") && contains(again.refusal, "nothing"), "a Level 9 scroll for a level 11 Mining refuses: nothing would change (the scroll stays)");
        CHECK(!again.consume || !again.ok, "a refusal consumes nothing");
        const auto l25 = items_like(path, "GM_Scroll_MakeMeLevel25Artisanship_Alchemy");
        use::Context done = ctx;
        done.held = pr::Professions::load(*book, std::vector<svc::ProfessionState>{{kAlchemy, 2, 25, 250001}}, nullptr);
        const use::Plan nothing = use::plan_use(db, l25[0].first, done);
        CHECK(!nothing.ok && contains(nothing.refusal, "Alchemy"), "a Level 25 scroll for a level 25+ Alchemist (no tool in it) refuses");
        const auto l25m = items_like(path, "GM_Scroll_MakeMeLevel25Artisanship_Mining");
        use::Context mined = ctx;
        mined.held = pr::Professions::load(*book, std::vector<svc::ProfessionState>{{kMining, 3, 33, 400ll * 33 * 33}}, nullptr);
        const use::Plan tool_only = use::plan_use(db, l25m[0].first, mined);
        CHECK(tool_only.ok && tool_only.items.size() == 1, "a Level 25 Mining scroll for a Master miner still gives the tool");
        // a Level 15 scroll from Apprentice level 12: XP raises the level, the Promote at level 15 is skipped (below the cap)
        const auto l15 = items_like(path, "GM_Scroll_MakeMeLevel15Artisanship_Mining");
        use::Context mid = ctx;
        mid.held = pr::Professions::load(*book, std::vector<svc::ProfessionState>{{kMining, 1, 12, 400ll * 12 * 12}}, nullptr);
        CHECK(mid.held.level(kMining) == 12 && mid.held.tier(kMining) == 1, "(set up: Apprentice, level 12)");
        const use::Plan p15 = use::plan_use(db, l15[0].first, mid);
        const pr::State s15 = p15.professions_after.state(kMining);
        bool skipped = false;
        for (const std::string& n : p15.notes) skipped = skipped || contains(n, "skipped");
        CHECK(p15.ok && s15.level == 15 && s15.tier == 1 && skipped, "a Level 15 scroll for an Apprentice at level 12: level 15, still Apprentice, the Promote below the cap is skipped and said so");
    }

    // refusals the context decides
    {
        const auto mining = items_like(path, "GM_Scroll_MakeMeLevel25Artisanship_Mining");
        use::Context bare = ctx;
        bare.professions = nullptr;
        bare.held = pr::Professions();
        const use::Plan no_book = use::plan_use(db, mining[0].first, bare);
        CHECK(!no_book.ok && contains(no_book.refusal, "profession"), "without professions worldd refuses a profession item and says why");
        use::Context no_run = ctx;
        no_run.profession_xp = nullptr;
        const use::Plan no_runner = use::plan_use(db, mining[0].first, no_run);
        CHECK(!no_runner.ok && contains(no_runner.refusal, "expression"), "without an expression runner a profession XP reward is refused and says why");
    }
// [C.O.A ~ Cyb]

    // the runner: GetPlayerProfessionLevel answers the real level
    {
        const auto mining = items_like(path, "GM_Scroll_MakeMeLevel25Artisanship_Mining");
        std::string e;
        const auto use = db.item_use(mining[0].first, e);
        const auto action = db.action_response(use->actions[0], e);
        const auto table = db.reward_table(action->reward_tables[0], e);
        const int64_t expr = table->experience[0].expr;
        std::string why;
        const auto at1 = runner(expr, {{"Mining", 1}}, why);
        const auto at10 = runner(expr, {{"Mining", 10}}, why);
        const auto at25 = runner(expr, {{"Mining", 25}}, why);
        CHECK(at1 && std::fabs(*at1 - 250001.0) < 0.5 && at10 && std::fabs(*at10 - 210001.0) < 0.5 && at25 && std::fabs(*at25 - 1.0) < 0.5,
              "the Mining Level 25 expression: xp - cur + 1 is 250,001 at level 1, 210,001 at level 10 and 1 at level 25");
        CHECK(!runner(0, {}, why) && !why.empty(), "an expression id of none cannot run");
    }
    SAYOK("the 67 scrolls");
}

// ---- the tools and the GM vendor ----------------------------------------------------------------------------------------------------------------------------

static void test_tools_and_vendor(const std::string& path, PlayerWorld& world) {
    std::printf("the T3 tools wear in their slots; the GM vendor sells the scrolls and the T1 tools\n");
    const content::ContentDb& db = *world.content();
    const items::ItemCatalog* catalog = world.item_catalog();
    CHECK(catalog != nullptr, "the world has an item catalog");
    if (!catalog || !world.professions()) return;
    const pr::Book& book = *world.professions();
    stats::CharacterStats cs(*world.stat_context().bundle, *world.stat_context().gear, 2, 1, world.stat_context().rounding);
    std::string err;
    cs.start({}, err);
    pr::XpRunner runner = pr::make_xp_runner(*world.ability_catalog(), cs, {}, err);
    use::Context ctx;
    ctx.class_enum = 2;
    ctx.curve = world.level_curve();
    ctx.professions = &book;
    ctx.held = pr::Professions(book);
    ctx.profession_xp = runner;
    // the five tools of the Level 25 scrolls: the record, the profession's slot (EEquipSlot) and the tier the tool is for
    struct Tool {
        const char* scroll;
        uint8_t     slot;
        int         tier;
        int64_t     profession;
    };
    const Tool tools[] = {{"GM_Scroll_MakeMeLevel25Artisanship_fishing", 35, 2, 108752770061964ll},
                          {"GM_Scroll_MakeMeLevel25Artisanship_Herbalism", 33, 2, 108752770061963ll},
                          {"GM_Scroll_MakeMeLevel25Artisanship_Hunting", 38, 2, 108752770061965ll},
                          {"GM_Scroll_MakeMeLevel25Artisanship_Lumberjacking", 29, 2, 108752769996426ll},
                          {"GM_Scroll_MakeMeLevel25Artisanship_Mining", 26, 2, 108752769996425ll}};
    int worn = 0, for_the_tier = 0;
    sqlite3* raw = nullptr;
    sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr);
    for (const Tool& tool : tools) {
        const auto scroll = items_like(path, tool.scroll);
        if (scroll.size() != 1) continue;
        const use::Plan plan = use::plan_use(db, scroll[0].first, ctx);
        if (!plan.ok || plan.items.size() != 1) continue;
        const auto info = catalog->info(plan.items[0].record);
        worn += info && info->equip_slots.size() == 1 && info->equip_slots[0] == tool.slot;
        // gather_tool says the same slot, this profession and this certification tier
        sqlite3_stmt* s = nullptr;
        if (raw && sqlite3_prepare_v2(raw, "SELECT tool_slot, profession_guid, cert_tier FROM gather_tool WHERE item_guid = ?1", -1, &s, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(s, 1, plan.items[0].record);
            if (sqlite3_step(s) == SQLITE_ROW) {
                for_the_tier += sqlite3_column_int(s, 0) == tool.slot && sqlite3_column_int64(s, 1) == tool.profession && sqlite3_column_int(s, 2) == tool.tier;
            }
        }
        sqlite3_finalize(s);
    }
    if (raw) sqlite3_close(raw);
    CHECK(worn == 5, "each of the five T3 tools is worn in its one slot: Pole 35, Sickle 33, Bow 38, Axe 29, Pickaxe 26");
    CHECK(for_the_tier == 5, "and gather_tool names that slot, its profession and the Journeyman tier");
// C:Y:B and C.O.A

    // the GM vendor lists every scroll (the 66 single-profession ones and `_All`) and the Starter and T1 tools of the five gathering professions
    std::string error;
    const auto vendors = db.vendor_defs_with_prefix("PTR_ONLY_Vendor_", error);
    CHECK(vendors.has_value(), "the GM vendors read");
    if (!vendors) return;
    std::set<int64_t> listed;
    for (const content::VendorDefRow& v : *vendors)
        for (const content::VendorInventoryRow& inv : v.inventories)
            for (const content::VendorListing& l : inv.listings) listed.insert(l.item);
    size_t scrolls = 0, scrolls_listed = 0;
    for (const char* pattern : {"GM_Scroll_MakeMeLevel9Artisanship_%", "GM_Scroll_MakeMeLevel15Artisanship_%", "GM_Scroll_MakeMeLevel25Artisanship_%"}) {
        for (const auto& [guid, name] : items_like(path, pattern)) {
            ++scrolls;
            scrolls_listed += listed.count(guid);
        }
    }
    CHECK(scrolls == 67 && scrolls_listed == 67, "all 67 GM level scrolls are on a GM vendor's list");
    const char* t1[] = {"Gear_Artisan_Mining_Tool_Pickaxe_T1_0", "Gear_Artisan_Lumberjacking_Tool_Axe_T1_0", "Gear_Artisan_Herbalism_Tool_Sickle_T1_0", "Gear_Artisan_Fishing_Tool_Pole_T1_0",
                        "Gear_Artisan_Hunting_Tool_Bow_T1_0", "Gear_Artisan_Mining_Tool_Pickaxe_Starter", "Gear_Artisan_Lumberjacking_Tool_Axe_Starter",
                        "Gear_Artisan_Herbalism_Tool_Sickle_Starter", "Gear_Artisan_Fishing_Tool_Pole_Starter", "Gear_Artisan_Hunting_Tool_Bow_Starter"};
    size_t tool_items = 0, tools_listed = 0;
    for (const char* name : t1) {
        for (const auto& [guid, n] : items_like(path, name)) {
            ++tool_items;
            tools_listed += listed.count(guid);
        }
    }
    CHECK(tool_items == 10 && tools_listed == 10, "the Starter and T1 tools of Mining, Lumberjacking, Herbalism, Fishing and Hunting are on a GM vendor's list");
    SAYOK("the tools and the GM vendor");
}

int main() {
    const char* env = std::getenv("AOC_CONTENT_DB");
    const std::string path = env && *env ? env : PROFESSIONS_CONTENT_DB_PATH;
    test_book_and_rules();
    if (!fs::exists(path)) {
        std::printf("[skip] %s is not there: the real content.db is not checked\n", path.c_str());
        std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
        return g_fail == 0 ? 0 : 1;
    }
    std::string error;
    auto db = content::ContentDb::open(path, error);
    if (!db) {
        std::printf("  [FAIL] content.db %s does not open: %s\n", path.c_str(), error.c_str());
        return 2;
    }
    std::printf("content.db %s (contentc version %s)\n", path.c_str(), db->meta().contentc_version.c_str());
    test_reader(*db);
    aoc::protocol::ServerGuidAllocator guids{60, 0x6ee87ee4u};
    PlayerWorld world(&*db, "", bootstrap::GameStateMode::Tier2, 60, guids);
    if (!world.ready()) {
        std::printf("  [FAIL] the world is not ready on %s: %s\n", path.c_str(), world.content_problem().c_str());
        return 2;
    }
    test_scrolls(path, world);
    test_tools_and_vendor(path, world);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
