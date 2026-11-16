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
#include "content/craft_filter.h"
#include "crafting/artisan.h"
#include "crafting/rewards.h"
#include "session/ability_catalog.h"
#include "session/item_use.h"
#include "session/player_world.h"
#include "session/professions.h"

#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace aoc::worldd;
namespace fs = std::filesystem;
namespace pr = aoc::worldd::professions;
namespace repl = aoc::protocol::repl;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)
#define SAYOK(msg) std::printf("  [ok ] %s\n", msg)
// The farming tests print the passing line too.
#define CHECK_SHOWN(cond, msg) do { \
    if (cond) { g_pass++; std::printf("  [ok ] %s\n", msg); } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

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

// ---- crafting quality: the artisan ratings of the crafting and processing professions ------------------------------------------------------------------------
//
//  The curves and the rating stats read from the real file, the formulas of crafting/artisan.h and crafting/rewards.h against numbers the game's own crafts gave (a Lumber Milling job of 300 s
//  that lasted 235.25 s, the profession experience of two jobs collected with a rating of 250, a Heroic-band helm of mean rarity 3.70 that came out at 52), and the
//  rolls of a job of 250 units drawn many times: the extra units and the steps up come out at the chance the curves give.

static const content::CraftRecipe* recipe_named(const content::CraftRecipeData& d, const char* name) {
    for (const auto& [guid, r] : d.recipes) {
        if (r.name == name) return &r;
    }
    return nullptr;
}

static void test_artisan(const content::ContentDb& db, const PlayerWorld& world) {
    namespace cr = aoc::worldd::crafting;
    std::printf("artisan ratings (crafting quality)\n");
    std::string error;
    const auto data = db.craft_recipe_data(error);
    if (!data) {
        std::printf("  [FAIL] the recipes do not read: %s\n", error.c_str());
        g_fail++;
        return;
    }
    const content::ArtisanData& a = data->artisan;
    CHECK(a.ok(), "the artisan curves and the rating stats are read");
    CHECK(a.crafting_professions == 8 && a.processing_professions == 9, "8 crafting and 9 processing professions have rating stats");
    CHECK(a.general_crafting.quality != 0 && a.general_crafting.xp != 0 && a.general_processing.time != 0 && a.general_processing.quantity != 0 && a.general_processing.rarity != 0 &&
              a.general_processing.xp != 0,
          "the general ratings are found");
    size_t whole = 0;
    for (const auto& [guid, p] : a.by_profession) whole += (p.processing ? (p.time && p.quantity && p.rarity && p.xp) : (p.quality && p.xp)) ? 1 : 0;
    CHECK(whole == a.by_profession.size() && whole == 17, "every crafting profession has its quality and experience ratings and every processing one its time, quantity, rarity and experience ratings");

    // the curves
    CHECK(cr::eval_points(a.crafting_quality, 0) == 0 && std::fabs(cr::eval_points(a.crafting_quality, 316) - 3.0) < 1e-9 && std::fabs(cr::eval_points(a.crafting_quality, 1984) - 10.0) < 1e-9 &&
              std::fabs(cr::eval_points(a.crafting_quality, 8409) - 25.0) < 1e-9 && std::fabs(cr::eval_points(a.crafting_quality, 99999) - 25.0) < 1e-9,
          "the crafting quality curve: 0 at 0, +3 at 316, +10 at 1,984, +25 at 8,409 and flat past it");
    CHECK(std::fabs(cr::eval_points(a.processing_time, 0) - 1.0) < 1e-9 && std::fabs(cr::eval_points(a.processing_time, 99999) - 0.2) < 1e-9, "the processing time curve runs from 1.0 to 0.2");
    CHECK(std::fabs(300.0 * cr::eval_points(a.processing_time, 450) - 235.25) < 1e-6, "[R] a time rating of 450 makes the 300 s of the recorded Lumber Milling job 235.25 s");
    CHECK(std::fabs(cr::eval_points(a.quantity_chance[0], 316) - 0.1) < 1e-9 && std::fabs(cr::eval_points(a.quantity_chance[1], 316) - 0.05) < 1e-9 && cr::eval_points(a.quantity_chance[5], 316) == 0,
          "the quantity chance of a Common unit is 10 % at 316, an Uncommon unit's 5 %, a Legendary unit's 0");
    CHECK(std::fabs(cr::eval_points(a.rarity_step[0], 316) - 0.05) < 1e-9 && std::fabs(cr::eval_points(a.rarity_step[1], 316) - 0.025) < 1e-9, "the step-up chance of a Common unit is 5 % at 316, an Uncommon unit's 2.5 %");

    // the ratings of a crafter: the general rating and the profession's own are added
    constexpr int64_t kLumberMilling = 108752756954756ll, kCarpentry = 108752746927737ll, kGathering = 108752769996425ll;
    const auto lm = a.by_profession.find(kLumberMilling);
    const auto cp = a.by_profession.find(kCarpentry);
    CHECK(lm != a.by_profession.end() && lm->second.processing && cp != a.by_profession.end() && !cp->second.processing, "Lumber Milling is a processing profession and Carpentry a crafting one");
    CHECK(a.by_profession.find(kGathering) == a.by_profession.end(), "a gathering profession has no crafting rating");
    if (lm == a.by_profession.end() || cp == a.by_profession.end()) return;
    std::map<int64_t, double> stats = {{a.general_processing.quantity, 100}, {lm->second.quantity, 237}, {a.general_processing.xp, 150}, {lm->second.xp, 100}, {a.general_crafting.quality, 400}, {cp->second.quality, 104}};
    const auto reader = [&](int64_t stat) { return stats.count(stat) != 0 ? stats[stat] : 0.0; };
    const cr::Ratings lumber = cr::ratings_of(a, kLumberMilling, reader), carpentry = cr::ratings_of(a, kCarpentry, reader);
    CHECK(lumber.quantity == 337 && lumber.xp == 250 && lumber.quality == 0 && carpentry.quality == 504 && carpentry.xp == 0 && lumber.any() && !cr::ratings_of(a, kLumberMilling, {}).any(),
          "a recipe's ratings are the general rating plus its profession's (the other profession's and a crafter with no stats add nothing)");

    // the experience of the recorded claims: ONE rating of 250 gives both the Lumber Milling job (10 Uncommon, 75 an unit) and the Tanning job (2 units at rarity 0, 60 an unit)
    cr::Ratings r250;
    r250.xp = 250;
    const double m250 = cr::xp_multiplier(a, r250, true);
    CHECK(cr::profession_xp(content::XpRule::Processing, 0, 1, 10, m250) == 1340, "[R] the Lumber Milling job x10 of the recording paid 1,340 profession experience (134 an unit)");
    CHECK(cr::profession_xp(content::XpRule::Processing, 1, 0, 2, m250) == 214, "[R] the Tanning job x2 of the recording paid 214 (107 an unit)");
    cr::Ratings r32;
    r32.xp = 32;
    CHECK(cr::profession_xp(content::XpRule::Processing, 0, 2, 21, cr::xp_multiplier(a, r32, true)) == 2310, "a rating of 32 on a Rare unit (100 an unit) pays 110 an unit: 2,310 for 21 (the arithmetic of a third recorded claim, whose paid rarity the recordings do not show)");
    CHECK(cr::profession_xp(content::XpRule::Processing, 1, 2, 3) == 360 && cr::profession_xp(content::XpRule::Processing, 2, 6, 1) == 280, "a crafter with no rating is paid the base tables");

    // the quality of a crafted piece: exact all the way (the base's share of its rarity band and the rating's bonus are never cut off), the nearest whole number only at the very last step
    const items::ItemRarityFacts& common_curve = items::default_rarity_facts();
    const std::vector<cr::PaidUnits> helm = {{1, 51, 5, 4}, {1, 31, 40, 3}, {2, 51, 1, 4}, {3, 51, 2, 4}};   // the Ironwake helm of the recording: plating 5 Epic + 40 Heroic, fragments, mold
    const double helm_mean = cr::craft_mean_rarity(helm);
    const double helm_base = cr::craft_base_quality(helm_mean, common_curve);
    CHECK(std::fabs(helm_mean - 3.7037037) < 1e-5 && std::fabs(helm_base - (31.0 + 19.0 * 0.7037037)) < 1e-4, "a mean rarity of 3.70 is Heroic 31 and 13.37 of the band: 44.37, nothing cut off");
    cr::Ratings q;
    q.quality = 1503;
    CHECK(cr::quality_from(helm_base + cr::quality_bonus(a, q)) == 52, "a quality rating of 1,503 adds 7.996 to 44.37: 52.37, the nearest whole number 52");
    q.quality = 1504;
    CHECK(cr::quality_from(helm_base + cr::quality_bonus(a, q)) == 52, "[R] a quality rating of 1,504 makes the recorded Ironwake helm 52 (44.37 and 8 is 52.37)");
    const std::vector<cr::PaidUnits> timber = {{9, 6, 9, 1}};   // the carriage: nine Uncommon timber
    q.quality = 211;
    CHECK(cr::craft_base_quality(cr::craft_mean_rarity(timber), common_curve) == 6.0 && cr::quality_from(6.0 + cr::quality_bonus(a, q)) == 8, "[R] nine Uncommon timber start at 6 and a rating of 211 makes the recorded carriage 8");
    const std::vector<cr::PaidUnits> citizen = {{1, 1, 4, 0}, {2, 1, 2, 0}, {3, 1, 2, 0}, {4, 51, 1, 4}}, market = {{1, 1, 6, 0}, {2, 1, 3, 0}};
    CHECK(cr::craft_base_quality(cr::craft_mean_rarity(citizen), common_curve) == 6.0 && cr::craft_base_quality(cr::craft_mean_rarity(market), common_curve) == 1.0,
          "[R] the recorded citizen crate (three Common stacks and an Epic certificate) starts at 6 and the market crate (all Common) at 1");
    CHECK(cr::craft_base_quality(cr::craft_mean_rarity({}), common_curve) == 1.0, "a craft that paid no material starts at 1");
    CHECK(cr::craft_base_quality(0.5, common_curve) == 3.0 && std::fabs(cr::craft_base_quality(0.6, common_curve) - 3.4) < 1e-9 && std::fabs(cr::craft_base_quality(1.0 / 3.0 + 1.0, common_curve) - 9.0) < 1e-9 &&
              cr::craft_base_quality(2.5, common_curve) == 23.0,
          "a share of the band is exact, whole number of qualities or not (half way up Common 1 to 5 is 3, 0.6 of the way 3.4), and every product has it: a piece of gear and a stackable one");
    CHECK(cr::quality_from(9.98) == 10 && cr::quality_from(9.4) == 9 && cr::quality_from(9.5) == 10 && cr::quality_from(4.979) == 5 && cr::quality_from(8.5347) == 9 && cr::quality_from(0.2) == 0,
          "the whole quality is the nearest whole number to the exact one, a half up (9.98 is 10, 9.4 is 9)");
    CHECK(cr::quality_from(std::nan("")) == 0 && cr::quality_from(1.0e300) == 1000000000 && cr::quality_from(-1.0e300) == -1000000000,
          "a NaN rating is quality 0 and an enormous one is cut off, neither is an undefined conversion");
    // the slot is what a quality is averaged over, not the record: one slot paid with two records is one slot
    const std::vector<cr::PaidUnits> two_records = {{1, 1, 37, 0}, {1, 6, 37, 1}, {2, 1, 25, 0}}, two_slots = {{1, 1, 37, 0}, {2, 6, 37, 1}, {3, 1, 25, 0}};
    CHECK(std::fabs(cr::craft_mean_rarity(two_records) - 0.25) < 1e-9 && std::fabs(cr::craft_mean_rarity(two_slots) - 1.0 / 3.0) < 1e-9,
          "a slot paid with Common and Uncommon units is one slot (a mean of 0.25 with a Common slot beside it), three slots would be a mean of 0.33");

    // crafts of slots of (units, rarity 0 to 5) and a quality rating, with the whole quality the exact calculation gives (the base and the bonus kept exact, the nearest whole number at the end): the
    // inputs are the community calculator's own screens and the first test's crafts, whose answers the calculator cuts off (4.98 is 4 there, 9.98 is 9)
    struct CalcCase {
        std::vector<std::vector<std::pair<int64_t, int>>> slots;
        double                                            rating;
        int                                               quality;
    };
    const std::vector<CalcCase> calc = {
        {{{{74, 0}}, {{25, 0}}}, 0, 1},
        {{{{74, 0}}, {{25, 0}}}, 316, 4},
        {{{{74, 0}}, {{25, 0}}}, 1504, 9},
        {{{{74, 0}}, {{25, 0}}}, 8409, 26},
        {{{{12, 2}}, {{12, 2}}, {{5, 3}}, {{1, 5}}}, 0, 31},
        {{{{1, 0}, {1, 1}}}, 0, 3},
        {{{{37, 0}, {37, 1}}, {{25, 0}}}, 0, 2},
        {{{{2, 1}, {1, 2}}}, 0, 9},
        {{{{1, 2}, {1, 3}}}, 0, 23},
        {{{{5, 4}, {40, 3}}, {{1, 4}}, {{2, 4}}}, 1503, 52},
        {{{{5, 4}, {40, 3}}, {{1, 4}}, {{2, 4}}}, 1504, 52},
        {{{{9, 1}}}, 211, 8},
        {{{{20, 0}, {5, 0}}, {{18, 0}, {2, 4}}}, 700, 7},
        {{{{13, 0}, {7, 1}}}, 105, 3},
        {{{{135, 4}}, {{20, 4}}, {{6, 0}, {3, 1}, {1, 0}}}, 12000, 52},
        {{{{18, 4}, {56, 0}}}, 1264, 12},
        {{{{25, 4}}, {{6, 4}, {34, 0}}, {{3, 4}, {4, 3}, {3, 2}}}, 8409, 48},
        {{{{76, 1}, {124, 1}}, {{40, 4}}, {{43, 3}, {20, 2}, {11, 4}}}, 105, 26},
        {{{{9, 0}}}, 504, 5},
        {{{{9, 0}}}, 0, 1},
        {{{{8, 1}, {1, 0}}}, 0, 5},
        {{{{8, 1}, {1, 0}}}, 504, 9},
    };
    size_t calc_ok = 0;
    for (size_t i = 0; i < calc.size(); ++i) {
        std::vector<cr::PaidUnits> paid;
        for (size_t s = 0; s < calc[i].slots.size(); ++s) {
            for (const auto& [units, rarity] : calc[i].slots[s]) paid.push_back({static_cast<int64_t>(s), 1, units, rarity});
        }
        cr::Ratings rating;
        rating.quality = calc[i].rating;
        const int got = cr::quality_from(cr::craft_base_quality(cr::craft_mean_rarity(paid), common_curve) + cr::quality_bonus(a, rating));
        if (got == calc[i].quality) ++calc_ok;
        else std::printf("  craft case %zu: %d, expected %d\n", i, got, calc[i].quality);
    }
    CHECK(calc_ok == calc.size(), "22 crafts (gear templates, the crate template, mixed slots, whole-number shares, ratings from 0 to 12,000, the first test's four crafts) start and end at the whole quality of the exact calculation");

    // the calculator's own screen: Weaponsmithing "Lvl 23 2H", one group of 74 Uncommon, quality rating 504 shows "TOTAL QUALITY 2.3333, UNCOMMON +33.3%" (it cuts 9.98 off to 9); the server carries 9.98 to the
    // nearest whole number, 10 (Uncommon is 6 to 15: 4 qualities up its 9 steps, 2.4444, +44.4 %); the carriage of nine Uncommon timber is the same piece
    const std::vector<cr::PaidUnits> seventy_four = {{0, 6, 74, 1}}, nine_timber = {{0, 6, 9, 1}};
    cr::Ratings r504;
    r504.quality = 504;
    const auto crafted = [&](const std::vector<cr::PaidUnits>& paid, const cr::Ratings& rating) {
        return cr::quality_from(cr::craft_base_quality(cr::craft_mean_rarity(paid), common_curve) + cr::quality_bonus(a, rating));
    };
    CHECK(crafted(seventy_four, r504) == 10 && crafted(nine_timber, r504) == 10 && std::fabs(cr::tier_scale(common_curve, 10) - (2.0 + 4.0 / 9.0)) < 1e-9 && std::fabs(cr::quality_bonus(a, r504) - 3.9791667) < 1e-6,
          "74 Uncommon (or the carriage's 9 Uncommon timber) at a rating of 504 is 6 and 3.98: 9.98, the whole quality 10 (the calculator's 2.3333 is its cut-off 9)");
    CHECK(cr::tier_scale(common_curve, 1) == 1.0 && cr::tier_scale(common_curve, 6) == 2.0 && std::fabs(cr::tier_scale(common_curve, 5) - 1.9999) < 1e-9 && std::fabs(cr::tier_scale(common_curve, 15) - 2.9999) < 1e-9 &&
              std::fabs(cr::tier_scale(common_curve, 71) - 6.0) < 1e-9 && std::fabs(cr::tier_scale(common_curve, 100) - 6.99) < 1e-9 && std::fabs(cr::tier_scale(common_curve, 23) - (3.0 + 7.0 / 14.0)) < 1e-9,
          "the game's quality in its bands and the calculator's tier scale: a band's first is its whole number, its last stops short of the next (Legendary at .99), Rare 23 is 3.5");
    // the rating a craft needs to move up a rarity from the bottom of a tier (the exact base plus the bonus must come to the next rarity's first quality less a half): all Common materials (1) reach
    // Uncommon (6) at 604, all Uncommon (6) reach Rare (16) at 1,864, Rare (16) reach Heroic (31) at 3,576, Heroic (31) reach Epic (51) and Epic (51) reach Legendary (71) at 5,608
    const auto reached = [&](double base, double rating) {
        cr::Ratings x;
        x.quality = rating;
        return cr::quality_from(base + cr::quality_bonus(a, x));
    };
    CHECK(reached(1, 603) == 5 && reached(1, 604) == 6 && reached(1, 700) == 6 && reached(1, 800) == 6 && reached(6, 1863) == 15 && reached(6, 1864) == 16 && reached(6, 2400) == 17 && reached(16, 3575) == 30 &&
              reached(16, 3576) == 31 && reached(31, 5607) == 50 && reached(31, 5608) == 51 && reached(51, 5607) == 70 && reached(51, 5608) == 71,
          "from the bottom of a tier a craft moves up a rarity at a rating of 604 (Common to Uncommon), 1,864 (Uncommon to Rare), 3,576 (Rare to Heroic) and 5,608 (Heroic to Epic, Epic to Legendary); 800 and 2,400 are past the first two");

    // the quality of a job's products: the mean of the units' band-first qualities (per resource, then over the resources, in single precision like the client) rounded UP, as the game does and the
    // community crafting calculator's newest version shows for processing; the band that whole number is in is the products' rarity
    const auto resource = [](int64_t record, std::initializer_list<svc::JobRarity> rarities) {
        svc::JobResource r;
        r.record_id = record;
        r.rarities = rarities;
        return r;
    };
    const cr::PaidMean uniform = cr::paid_mean({resource(1, {{1, 250, false}})});
    const cr::PaidMean otter = cr::paid_mean({resource(1, {{0, 1, false}, {1, 1, false}}), resource(2, {{1, 1, false}}), resource(3, {{1, 1, false}})});   // the recorded mixed job: 3.5, 6 and 6 over three resources
    const cr::PaidMean near_top = cr::paid_mean({resource(1, {{0, 2, false}, {1, 23, false}})});                                                          // 5.6 rounds up to 6
    const cr::PaidMean wide = cr::paid_mean({resource(1, {{0, 1, false}, {5, 1, false}})});                                                              // Common and Legendary: 36, Heroic
    const cr::PaidMean none = cr::paid_mean({});
    CHECK(uniform.points == 6.0f && uniform.quality == 6 && uniform.rarity == 1 && std::fabs(otter.points - 31.0f / 6.0f) < 1e-5f && otter.quality == 6 && otter.rarity == 1 && std::fabs(near_top.points - 5.6f) < 1e-5f &&
              near_top.quality == 6 && near_top.rarity == 1 && wide.points == 36.0f && wide.quality == 36 && wide.rarity == 3 && none.points == 1.0f && none.quality == 1 && none.rarity == 0,
          "[R] the mean of what a job was paid is rounded up to a whole quality: uniform Uncommon is 6; the recorded mixed job (5.17) is 6, Uncommon, and so is 5.6; Common and Legendary (36) is Heroic; no item is 1");

    // one Lumber Milling job of 60 Uncommon oak with the T3 set at Legendary (time 30, quantity 337, rarity 349, experience 74): the chances the community calculator's newest version shows (2.71 % per item to
    // come out Rare, 0.03 % on to Heroic; its curves are the data's), and what it does not model: the chance of one more unit (5.27 %), the time (x0.986, 1,775 s of 1,800) and the experience (x1.234, 93 of 75)
    cr::Ratings lm_set;
    lm_set.time = 30;
    lm_set.quantity = 337;
    lm_set.rarity = 349;
    lm_set.xp = 74;
    CHECK(std::fabs(cr::rarity_step_chance(a, lm_set, 1) - 0.0271484) < 1e-5 && std::fabs(cr::rarity_step_chance(a, lm_set, 1) * cr::rarity_step_chance(a, lm_set, 2) - 0.0003065) < 1e-6 &&
              std::fabs(cr::quantity_chance(a, lm_set, 1) - 0.052734) < 1e-5 && std::fabs(1800.0 * cr::time_multiplier(a, lm_set) - 1775.0) < 0.5 && std::fabs(cr::xp_multiplier(a, lm_set, true) - 1.2342) < 1e-4,
          "the Lumber Milling T3 set: 2.71 % of the Uncommon oak comes out Rare and 0.03 % Heroic, 5.27 % of the units come with one more, 1,800 s is 1,775 s, and the experience is x1.234");

    // a job of 250 Lumber Milling units, drawn many times
    const content::CraftRecipe* oak = recipe_named(*data, "LumberMilling_T1_Timber_Oak");
    CHECK(oak && oak->has_duration_formula && oak->has_quantity_formula && oak->has_quality_formula && !oak->inputs.empty(), "the Oak Timber recipe has a time, a quantity and a quality formula");
    if (!oak || oak->inputs.empty()) return;
    const auto facts = [&](int64_t item) { return world.item_rarity_facts(item); };
    svc::ProcessingJob job;
    job.recipe_guid = oak->guid;
    job.quantity = 250;
    job.resources = {{oak->inputs[0].item, {{1, 250, false}}}};   // 250 Uncommon logs
    job.job_guid = "00000000000000000000000000000001";
    const auto plain = cr::roll_job(*data, job, 20, cr::Ratings{}, facts);
    CHECK(plain && plain->products.size() == 2 && plain->products[0].quantity == 250 && plain->products[1].quantity == 250 && plain->quality == 6 && plain->products[0].quality == 6 &&
              plain->extra_units == 0 && plain->stepped_units == 0 && plain->rarity == 1,
          "a crafter with no rating makes the quantity of the recipe, all at the paid quality (6), and its experience is worked out at the paid rarity (Uncommon)");
    // the recorded mixed job (otter hide 1 Common + 1 Uncommon, wood and chips Uncommon) comes out Uncommon at 6 (the mean 5.17 rounded up) and pays what a Common job pays (its experience is worked out at the
    // mean RARITY of what was paid, 0.83, rounded down)
    svc::ProcessingJob mixed = job, common_only = job;
    mixed.resources = {resource(oak->inputs[0].item, {{0, 1, false}, {1, 1, false}}), resource(2, {{1, 1, false}}), resource(3, {{1, 1, false}})};
    common_only.resources = {resource(oak->inputs[0].item, {{0, 250, false}})};
    const auto mixed_made = cr::roll_job(*data, mixed, 20, cr::Ratings{}, facts);
    const auto common_made = cr::roll_job(*data, common_only, 20, cr::Ratings{}, facts);
    CHECK(mixed_made && common_made && !mixed_made->products.empty() && mixed_made->quality == 6 && mixed_made->products[0].quality == 6 && mixed_made->rarity == 0 && mixed_made->profession_xp == common_made->profession_xp &&
              mixed_made->adventuring_xp == common_made->adventuring_xp && mixed_made->profession_xp > 0,
          "[R] a job paid with the recorded mixed rarities makes Uncommon products at quality 6 and pays the experience of a Common job (the experience rarity is the mean rarity rounded down)");
    cr::Ratings both;
    both.quantity = 316;
    both.rarity = 316;
    int64_t extra = 0, stepped = 0, jobs_ok = 0, runs = 0;
    bool same = true, conserved = true, shapes = true;
    for (int i = 1; i <= 400; ++i) {
        char guid[40];
        std::snprintf(guid, sizeof(guid), "%032d", i);
        job.job_guid = guid;
        const auto made = cr::roll_job(*data, job, 20, both, facts);
        const auto again = cr::roll_job(*data, job, 20, both, facts);
        if (!made || !again) continue;
        ++runs;
        same = same && made->products.size() == again->products.size() && made->extra_units == again->extra_units && made->stepped_units == again->stepped_units;
        int64_t total = 0;
        for (const cr::Product& p : made->products) {
            total += p.quantity;
            shapes = shapes && p.quantity > 0 && (p.quality == 6 || p.quality == 16 || p.quality == 31 || p.quality == 51);
        }
        conserved = conserved && total == 500 + made->extra_units;
        extra += made->extra_units;
        stepped += made->stepped_units;
        jobs_ok += made->profession_xp > 0 ? 1 : 0;
    }
    const double mean_extra = static_cast<double>(extra) / static_cast<double>(std::max<int64_t>(1, runs)), mean_stepped = static_cast<double>(stepped) / static_cast<double>(std::max<int64_t>(1, runs));
    std::printf("  400 jobs of 250 units at quantity and rarity ratings of 316: %.2f extra units and %.2f steps up a job (expected 25.0 and about 13.3)\n", mean_extra, mean_stepped);
    CHECK(runs == 400 && same && conserved && shapes, "a job rolls the same every time it is looked at, loses no unit to a step up, and its lines come in the first quality of a rarity");
    CHECK(mean_extra > 24.0 && mean_extra < 26.0, "the extra units follow the quantity chance of an Uncommon unit (5 %): 25 a job of two products of 250");
    CHECK(mean_stepped > 12.2 && mean_stepped < 14.4, "the steps up follow the step-up chance of an Uncommon unit (2.5 %), and again for the Rare units");
    CHECK(jobs_ok == runs, "a job with ratings still pays its experience");

    // a job of ONE unit with huge ratings: a unit that steps up twice (Uncommon, Rare, Heroic) leaves no empty line behind (the Rare line at 0 units made a job that could never be collected: the window showed
    // a phantom unit and the grant of an empty product was refused at every retry)
    cr::Ratings big;
    big.quantity = 99999;
    big.rarity = 99999;
    svc::ProcessingJob one = job;
    one.quantity = 1;
    one.resources = {{oak->inputs[0].item, {{1, 1, false}}}};   // one Uncommon log
    bool none_empty = true, units_kept = true, shown_all = true;
    int deep = 0, one_runs = 0;
    for (int i = 1; i <= 3000; ++i) {
        char guid[40];
        std::snprintf(guid, sizeof(guid), "%032d", 100000 + i);
        one.job_guid = guid;
        const auto made = cr::roll_job(*data, one, 20, big, facts);
        if (!made) continue;
        ++one_runs;
        int64_t total = 0;
        bool heroic = false;
        for (const cr::Product& p : made->products) {
            none_empty = none_empty && p.quantity >= 1;
            total += p.quantity;
            heroic = heroic || p.quality >= 31;
        }
        units_kept = units_kept && total == 2 + made->extra_units;   // two products of one unit each, and the extra units: no unit is lost to a step up
        shown_all = shown_all && cr::product_instances(one, aoc::protocol::repl::GuidValue{1, 2, 3, 4}, *made).size() == made->products.size();
        deep += heroic ? 1 : 0;
    }
    CHECK(one_runs == 3000 && none_empty && units_kept && shown_all && deep > 20,
          "a job of one unit with huge ratings leaves no empty line (a unit that steps up twice, a few in a hundred of these jobs, used to leave one), loses no unit and shows every line");

    // a job keeps the ratings it was queued with (saved with it: migration 0019): make_job stores the three sums a job's formulas use, a rating below 0 or that is no number is kept as 0, and the job
    // rolls from its saved ratings exactly as from the ratings it started with
    cr::Ratings queued;
    queued.xp = 285.25;
    queued.quantity = 337;
    queued.rarity = -4;
    queued.time = 30;
    cr::Ratings odd;
    odd.xp = std::nan("");
    odd.quantity = 1e12;
    odd.rarity = INFINITY;
    const aoc::protocol::repl::GuidValue txn{1, 2, 3, 4};
    const svc::ProcessingJob kept = cr::make_job({}, "0123456789abcdef", oak->guid, 250, 300000, txn, job.resources, 1790000000000ll, queued);
    const svc::ProcessingJob oddly = cr::make_job({}, "0123456789abcdef", oak->guid, 250, 300000, txn, job.resources, 1790000000000ll, odd);
    const cr::Ratings back = cr::job_ratings(kept), back_odd = cr::job_ratings(oddly);
    CHECK(back.xp == 285.25 && back.quantity == 337 && back.rarity == 0 && back.time == 0 && back.quality == 0 && back_odd.xp == 0 && back_odd.quantity == 1e9 && back_odd.rarity == 0,
          "a job keeps the experience, quantity and rarity ratings it was queued with (below 0 or no number as 0, the table's bound as it is) and no other rating");
    bool from_snapshot = true;
    for (int i = 1; i <= 20; ++i) {
        char guid[40];
        std::snprintf(guid, sizeof(guid), "%032d", i);
        svc::ProcessingJob saved = cr::make_job({}, "0123456789abcdef", oak->guid, 250, 300000, txn, job.resources, 1790000000000ll, both);
        saved.job_guid = guid;
        job.job_guid = guid;
        const auto via = cr::roll_job(*data, saved, 20, cr::job_ratings(saved), facts);
        const auto direct = cr::roll_job(*data, job, 20, both, facts);
        from_snapshot = from_snapshot && via && direct && via->extra_units == direct->extra_units && via->stepped_units == direct->stepped_units && via->products.size() == direct->products.size() &&
                        via->profession_xp == direct->profession_xp;
    }
    CHECK(from_snapshot, "a job rolls from its saved ratings exactly as from the ratings it was started with");
}

// ---- farming: a recipe input named by a gather-item filter, and the Novice loop the filter unblocks ----------------------------------------------------------
//
//  The game's own data: a recipe input whose item is 0 and whose filter is a GatherItemFilterRecordBase (a curated list: ALL_Raw_Plants_Any's 22 plants, Raw_Wood_Any's 9
//  woods, Processed_Paper_Any's 17 papers).  The client shows the list as the input's materials and sends the pick in a tag selection, so a job takes a pick only when it is
//  one of the filter's items, not when it merely carries the input's tag (45 plants carry Item.Resource.Raw.Plant, 22 are in the filter).  The filter lists come from
//  contentc version 47's craft_filter_item; the data here may be an older file, so the test names the three lists it uses and the reader has a test of its own.

namespace farm {
constexpr int64_t kFarming = 108752756954757ll;
constexpr int64_t kMulch = 6064630961167400960ll, kRotrootPulp = 6064634007467655178ll, kWoodchips = 6064632988998041610ll;
constexpr int64_t kWater = 6064630417665753417ll, kDaffodil = 6064629184431849472ll, kBarley = 6064629132834439185ll;
constexpr int64_t kSpringdrawnWater = 6064634088144896370ll, kAppleSeed = 6064629252117233675ll, kPitchResin = 6064633979457306628ll;
constexpr int64_t kOak = 109162169652924ll, kEasternHemlock = 6064629252096851973ll, kWoodTagOnly = 6064629252172021765ll;
constexpr int64_t kFadedTreasureMap = 6064632204577210671ll, kOraclesEye = 6064632204597920050ll, kBlueInk = 6064632203704795151ll;
constexpr int64_t kPlantFilter = 6064633963143168007ll, kWoodFilter = 6064633967776563206ll, kPaperFilter = 6064634767191769088ll;
constexpr int64_t kSeed[5] = {6064630417666212191ll, 6064630683142782977ll, 6064630971851145222ll, 6064630417666277730ll, 6064630417666212192ll};   // Blackberry, Cotton, Flax, Celery, Lettuce
const char* const kCrop[5] = {"Farming_T1_Fruit_Blackberry", "Farming_T1_Textile_Cotton", "Farming_T1_Textile_Flax", "Farming_T1_Vegetable_Celery", "Farming_T1_Vegetable_Lettuce"};
}  // namespace farm

/// The items of the gather-item filters this test uses, as the design records hold them: ALL_Raw_Plants_Any (22), Raw_Wood_Any (9), Processed_Paper_Any (17).
static std::vector<int64_t> plants_filter() {
    return {6064633259572002818ll, 6064633221579931712ll, 6064633221496700975ll, 6064633221397217305ll, 6064633288204353544ll, 6064632983206232082ll, 6064633855580307516ll,
            6064629184431849472ll, 6064630417666212190ll, 6064629207110844431ll, 109234000713341ll,     6064631505975640102ll, 6064630520851136512ll, 6064632954949599236ll,
            6064632954941538306ll, 6064632954942390275ll, 6064633855522111540ll, 6064633368529010691ll, 6064629207102521358ll, 6064629252161929216ll, 6064629252167237635ll,
            6064633391237496838ll};
}
static std::vector<int64_t> wood_filter() {
    return {6064632954938851328ll, 109162169652924ll, 6064628912716447747ll, 6064628912704782337ll, 6064629252096851973ll, 6064632954939899905ll, 6064633254017630211ll,
            6064633221016387589ll, 6064633391232581636ll};
}
static std::vector<int64_t> paper_filter() {
    return {6064632989076815886ll, 6064631793389076481ll, 6064633328413179961ll, 6064631793401987077ll, 6064629252096851973ll, 6064634086374965256ll, 6064633328412000311ll,
            6064632989077536783ll, 6064633328413835322ll, 6064631793394647043ll, 6064634086384861194ll, 6064633328414425147ll, 6064634086373523463ll, 6064631793385668608ll,
            6064633328412655672ll, 6064631793400479748ll, 6064631793390780418ll};
}

static svc::InventoryItem farm_item(int slot, int64_t record, int quantity = 1) {
    static uint32_t n = 0;
    svc::InventoryItem it;
    it.container = svc::ItemContainer::Backpack;
    it.slot = slot;
    it.item_record_id = record;
    it.instance_guid = [&] {
        char buf[33];
        std::snprintf(buf, sizeof buf, "%032x", ++n + 0x5000);
        return std::string(buf);
    }();
    it.quantity = quantity;
    it.quality = 1;
    it.durability = 100;
    it.created_at_ms = 1'771'299'474'000ll;
    return it;
}

static std::optional<items::Inventory> bag_of(std::vector<svc::InventoryItem> items) {
    std::string error;
    auto inv = items::Inventory::from_items(items, error);
    if (!inv) std::printf("    the bag does not read: %s\n", error.c_str());
    return inv;
}

static const content::CraftStationDef* station_named(const content::CraftStationData& stations, const char* name) {
    const auto it = stations.by_name.find(name);
    return it == stations.by_name.end() ? nullptr : &stations.defs.at(it->second);
}

static repl::CraftResource resource(int64_t record, int count, int rarity = 0) {
    repl::CraftResource r;
    r.record = record;
    r.rarities.push_back({static_cast<uint8_t>(rarity), count, false});
    return r;
}

static repl::CraftTagSelection pick_of(std::vector<repl::CraftResource> resources, std::vector<std::string> tags) {
    repl::CraftTagSelection s;
    s.resources = std::move(resources);
    s.tags = std::move(tags);
    return s;
}

static repl::StartCraft start_of(const content::CraftRecipe& recipe, int quantity) {
    repl::StartCraft s;
    s.recipe = recipe.guid;
    s.quantity = quantity;
    return s;
}

static crafting::StartFacts filter_facts(const content::CraftRecipeData& data, const content::CraftStationDef& station, const items::Inventory& inventory, const PlayerWorld& world,
                                         int tier = 0, bool quick = false) {
    crafting::StartFacts f;
    f.recipes = &data;
    f.station = &station;
    f.quick = quick;
    f.tier_of = [tier](int64_t) { return tier; };
    f.taught = [](int64_t) { return true; };
    f.inventory = &inventory;
    f.rarity_facts = [&world](int64_t item) { return world.item_rarity_facts(item); };   // (the rarity the client shows: an item that is never Common reads its own)
    f.copper = 1'000'000;
    f.node.level = 3;
    return f;
}

/// The table the reader writes (contentc version 47): a small SQLite file with craft_filter_item only.
static void test_filter_table() {
    std::printf("the gather-item filter table (craft_filter_item)\n");
    const fs::path path = fs::temp_directory_path() / "worldd_professions_filter_table.db";
    std::error_code ignored;
    fs::remove(path, ignored);
    sqlite3* db = nullptr;
    CHECK_SHOWN(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK, "a scratch SQLite file opens");
    if (!db) return;
    const auto exec = [&](const char* sql) {
        char* why = nullptr;
        const bool ok = sqlite3_exec(db, sql, nullptr, nullptr, &why) == SQLITE_OK;
        if (!ok) std::printf("    %s\n", why ? why : "sqlite refused");
        sqlite3_free(why);
        return ok;
    };
    CHECK_SHOWN(exec("CREATE TABLE craft_filter_item (filter_guid INTEGER NOT NULL, item_guid INTEGER NOT NULL, PRIMARY KEY (filter_guid, item_guid)) WITHOUT ROWID"),
          "the table has the reader's key names: filter_guid and item_guid");
    CHECK_SHOWN(exec("INSERT INTO craft_filter_item VALUES (7, 30), (7, 10), (7, 20), (9, 5)"), "rows are written");
    std::map<int64_t, std::vector<int64_t>> items;
    bool present = false;
    std::string error;
    CHECK_SHOWN(content::read_craft_filter_items(db, items, present, error), "the table reads");
    CHECK_SHOWN(present && items.size() == 2 && items[7] == std::vector<int64_t>({10, 20, 30}) && items[9] == std::vector<int64_t>({5}),
          "each filter's items are kept once and ascending, grouped by the filter's guid");
    // a file without the table is an older one: no error, no item, present false
    CHECK_SHOWN(exec("DROP TABLE craft_filter_item"), "the table is dropped");
    items.clear();
    present = true;
    CHECK_SHOWN(content::read_craft_filter_items(db, items, present, error) && !present && items.empty(), "a file without the table is an older contentc's: no item and no error");
    // a column that is not an integer is refused, not read as zero
    CHECK_SHOWN(exec("CREATE TABLE craft_filter_item (filter_guid, item_guid)"), "a table of untyped columns");
    CHECK_SHOWN(exec("INSERT INTO craft_filter_item VALUES ('a filter', 1)"), "a text filter guid is written");
    items.clear();
    present = false;
    error.clear();
    CHECK_SHOWN(!content::read_craft_filter_items(db, items, present, error) && error.find("wrong storage class") != std::string::npos,
          "a column of another storage class is an error, not a filter of guid 0");
    sqlite3_close(db);
    fs::remove(path, ignored);
    SAYOK("the filter table");
}

/// What the change reaches, and the rule at its source: a filter's own list decides a pick, not the input's tag.
static void test_filter_rules(const content::ContentDb& db, const PlayerWorld& world) {
    namespace cr = aoc::worldd::crafting;
    std::printf("a filter input's picks, and the recipes the filter rule reaches\n");
    std::string error;
    auto loaded = db.craft_recipe_data(error);
    if (!loaded) {
        std::printf("  [FAIL] the recipes do not read: %s\n", error.c_str());
        ++g_fail;
        return;
    }
    content::CraftRecipeData data = std::move(*loaded);
    data.filter_items[farm::kPlantFilter] = plants_filter();
    data.filter_items[farm::kWoodFilter] = wood_filter();
    data.filter_items[farm::kPaperFilter] = paper_filter();
    for (int64_t filter : {farm::kPlantFilter, farm::kWoodFilter, farm::kPaperFilter}) {   // (the table holds each filter's items ascending: filter_admits looks them up so)
        std::vector<int64_t>& items = data.filter_items[filter];
        std::sort(items.begin(), items.end());
    }
    data.filters_read = true;
    std::string station_error;
    const auto station_data = db.craft_station_data(station_error);
    const content::CraftStationData* stations = station_data ? &*station_data : nullptr;
    CHECK_SHOWN(stations != nullptr, "the station definitions are read");
    if (!stations) return;

    // the reach, from the data: every recipe input that names a filter, and those at a station worldd serves with an Inventory destination (the refusal this change removes)
    size_t filter_rows = 0, filter_recipes = 0;
    std::set<int64_t> professions, filters;
    for (const auto& [guid, r] : data.recipes) {
        bool has = false;
        for (const content::CraftInput& in : r.inputs) {
            if (in.item == 0 && in.filter != 0) {
                ++filter_rows;
                has = true;
                filters.insert(in.filter);
            }
        }
        if (!has) continue;
        ++filter_recipes;
        professions.insert(r.profession);
    }
    CHECK_SHOWN(filter_rows == 496 && filter_recipes == 236 && professions.size() == 14 && filters.size() == 51,
          "the data's filter inputs: 496 rows, 236 recipes, 14 professions, 51 filters");
    std::set<int64_t> reached, crate_only;
    std::set<int64_t> reached_professions;
    for (const auto& [guid, r] : data.recipes) {
        if (!r.inventory_destination) continue;
        bool has = false;
        for (const content::CraftInput& in : r.inputs) has = has || (in.item == 0 && in.filter != 0);
        if (!has) continue;
        bool served = false, crate = false;
        for (const auto& entry : stations->defs) {
            const content::CraftStationDef& station = entry.second;
            if (!data.set_holds(station.recipe_set, guid)) continue;
            if (station.processing || cr::serves_quick_make(station)) served = true;
            crate = crate || cr::packs_crates(station) || cr::is_commodity_table(station);
        }
        if (served && !crate) {
            reached.insert(guid);
            reached_professions.insert(r.profession);
        }
        if (served && crate) crate_only.insert(guid);
    }
    CHECK_SHOWN(reached.size() == 52 && reached_professions.size() == 13, "52 Inventory recipes at a served profession station reach the filter rule, in 13 professions");
    CHECK_SHOWN(crate_only.size() == 133, "133 more at a crate or commodity station take a filter input where the tag check served them");

    const content::CraftStationDef* farming = station_named(*stations, "ProcessingStation_Farming_T2");
    const content::CraftStationDef* metal = station_named(*stations, "ProcessingStation_Metalworking_T2");
    const content::CraftStationDef* scribe = station_named(*stations, "CraftingStation_Scribe_T3");
    const content::CraftStationDef* crates = station_named(*stations, "ProcessingStation_CratePacking_Construction_Gathering");
    const content::CraftRecipe* mulch = recipe_named(data, "Farming_T1_Mulch");
    const content::CraftRecipe* charcoal = recipe_named(data, "Metalworking_T2_Reagent_Charcoal");
    const content::CraftRecipe* treasure = recipe_named(data, "Scribe_T3_TreasureMap");
    const content::CraftRecipe* crate = recipe_named(data, "Recipe_Crate_Construction_Gathering_Lumberjacking");
    CHECK_SHOWN(farming && metal && scribe && crates && mulch && charcoal && treasure && crate, "the stations and the recipes of the cases are in the data");
    if (!farming || !metal || !scribe || !crates || !mulch || !charcoal || !treasure || !crate) return;
    CHECK_SHOWN(farming->max_tier == 1 && farming->display_name == "Farming - Apprentice", "the live village Marketplace station is Farming - Apprentice (tier 1 at most)");

    // the Farming filter: a Novice's Mulch job takes a plant of the filter's 22, not a plant that only carries the tag
    auto mulch_bag = bag_of({farm_item(0, farm::kRotrootPulp), farm_item(1, farm::kWoodchips), farm_item(2, farm::kDaffodil)});
    CHECK_SHOWN(mulch_bag.has_value(), "a bag of a Rotroot Pulp, a Woodchips and a Daffodil");
    if (!mulch_bag) return;
    repl::StartCraft mulch_req = start_of(*mulch, 1);
    mulch_req.general = {resource(farm::kWoodchips, 1)};
    mulch_req.tags = {pick_of({resource(farm::kDaffodil, 1)}, {"Item.Resource.Raw.Plant"})};
    cr::Plan mulch_plan = cr::plan_start(mulch_req, filter_facts(data, *farming, *mulch_bag, world));
    CHECK_SHOWN(mulch_plan.ok, "a Novice starts Farming_T1_Mulch with a Daffodil pick at the Farming - Apprentice station");
    if (mulch_plan.ok) {
        bool plant_paid = false;
        for (const repl::CraftResource& paid : mulch_plan.paid) plant_paid = plant_paid || (paid.record == farm::kDaffodil && !paid.rarities.empty() && paid.rarities.front().count == 1);
        CHECK_SHOWN(mulch_plan.takes.size() == 3 && plant_paid, "the job takes the reagent, the plant and the Woodchips, and the picked plant is among what it was paid with");
    }
    auto barley_bag = bag_of({farm_item(0, farm::kRotrootPulp), farm_item(1, farm::kWoodchips), farm_item(2, farm::kBarley)});
    repl::StartCraft barley_req = mulch_req;
    barley_req.tags = {pick_of({resource(farm::kBarley, 1)}, {"Item.Resource.Raw.Plant"})};
    cr::Plan barley_plan = barley_bag ? cr::plan_start(barley_req, filter_facts(data, *farming, *barley_bag, world)) : cr::Plan{};
    CHECK_SHOWN(!barley_plan.ok && barley_plan.refusal.find("not one of the items of the gather-item filter") != std::string::npos,
          "Barley carries the plant tag and is not one of the filter's 22: refused for the filter, though the bags hold it");
    auto short_bag = bag_of({farm_item(0, farm::kRotrootPulp, 2), farm_item(1, farm::kWoodchips, 2), farm_item(2, farm::kDaffodil)});
    repl::StartCraft short_req = start_of(*mulch, 2);
    short_req.general = {resource(farm::kWoodchips, 2)};
    short_req.tags = {pick_of({resource(farm::kDaffodil, 1)}, {"Item.Resource.Raw.Plant"})};
    cr::Plan short_plan = short_bag ? cr::plan_start(short_req, filter_facts(data, *farming, *short_bag, world)) : cr::Plan{};
    CHECK_SHOWN(!short_plan.ok && short_plan.refusal.find("names 1 items for the tags") != std::string::npos, "a job of two units with one plant picked is refused: the picks add up to the quantity");

    // two players at one station: the plan takes nothing, so one player's bag pays only his own job
    CHECK_SHOWN(mulch_bag->count_materials(farm::kDaffodil, items::default_rarity_facts(), 0, items::kRarityCount - 1) == 1, "planning takes nothing from the bags");
    auto empty_bag = bag_of({farm_item(0, farm::kRotrootPulp), farm_item(1, farm::kWoodchips)});
    cr::Plan poor = empty_bag ? cr::plan_start(mulch_req, filter_facts(data, *farming, *empty_bag, world)) : cr::Plan{};
    CHECK_SHOWN(!poor.ok && poor.refusal.find("the bags hold 0 of item") != std::string::npos, "a second player without the plant is refused: the first player's pick is not his");
    auto own_bag = bag_of({farm_item(0, farm::kRotrootPulp), farm_item(1, farm::kWoodchips), farm_item(2, farm::kDaffodil)});
    cr::Plan second = own_bag ? cr::plan_start(mulch_req, filter_facts(data, *farming, *own_bag, world)) : cr::Plan{};
    CHECK_SHOWN(second.ok, "a second player with his own plant starts the same job at the same station");

    // the same start twice: the first job's takes empty the bag, so the second cannot pay; the job's saved resources (a reconnect) hold the pick
    auto twice_bag = bag_of({farm_item(0, farm::kRotrootPulp), farm_item(1, farm::kWoodchips), farm_item(2, farm::kDaffodil)});
    cr::Plan first = twice_bag ? cr::plan_start(mulch_req, filter_facts(data, *farming, *twice_bag, world)) : cr::Plan{};
    bool applied = first.ok && twice_bag.has_value();
    for (const cr::Take& t : first.takes) {
        if (twice_bag) applied = twice_bag->take_materials(t.record, t.facts, t.rarity_lo, t.rarity_hi, t.count, t.lowest_first).ok && applied;
    }
    cr::Plan again = twice_bag ? cr::plan_start(mulch_req, filter_facts(data, *farming, *twice_bag, world)) : cr::Plan{};
    CHECK_SHOWN(applied && first.ok && !again.ok, "the same job started twice: the second finds the materials the first took gone");
    const std::vector<svc::JobResource> saved = cr::job_resources_of(first.paid);
    bool plant_saved = false;
    for (const svc::JobResource& r : saved) plant_saved = plant_saved || r.record_id == farm::kDaffodil;
    CHECK_SHOWN(plant_saved, "the job's saved resources name the picked plant, so a reconnect mid-job shows what was paid");
    const svc::ProcessingJob job = cr::make_job({}, "0123456789abcdef", mulch->guid, 1, 60000, {1, 2, 3, 4}, saved, 1000);
    const auto state = cr::to_state(job, 5000, 10.0);
    CHECK_SHOWN(state && state->general.size() == 2 && state->general.back().record == farm::kDaffodil, "the reconnected job's window lists the plant");

    // another profession's filter (Metalworking's Raw_Wood_Any): the wood pick is a wood of its 9, not a plant of the other filter
    auto metal_bag = bag_of({farm_item(0, farm::kOak, 2), farm_item(1, farm::kPitchResin, 2)});
    repl::StartCraft metal_req = start_of(*charcoal, 1);
    metal_req.general = {resource(farm::kPitchResin, 2)};
    metal_req.tags = {pick_of({resource(farm::kOak, 2)}, {"Item.Resource.Raw.Wood"})};
    cr::Plan metal_plan = metal_bag ? cr::plan_start(metal_req, filter_facts(data, *metal, *metal_bag, world, 1)) : cr::Plan{};
    CHECK_SHOWN(metal_plan.ok, "Metalworking's charcoal takes an Oak the wood filter admits (a second filter, at a job)");
    auto plant_in_wood = bag_of({farm_item(0, farm::kDaffodil, 2), farm_item(1, farm::kPitchResin, 2)});
    repl::StartCraft wrong_kind = metal_req;
    wrong_kind.tags = {pick_of({resource(farm::kDaffodil, 2)}, {"Item.Resource.Raw.Wood"})};
    cr::Plan wrong_plan = plant_in_wood ? cr::plan_start(wrong_kind, filter_facts(data, *metal, *plant_in_wood, world, 1)) : cr::Plan{};
    CHECK_SHOWN(!wrong_plan.ok && wrong_plan.refusal.find("not one of the items of the gather-item filter") != std::string::npos,
          "a plant is not one of the wood filter's 9, though another filter admits it");
    CHECK_SHOWN(data.filter_admits(farm::kPlantFilter, farm::kDaffodil) && !data.filter_admits(farm::kPlantFilter, farm::kOak) && data.filter_admits(farm::kWoodFilter, farm::kOak) &&
              !data.filter_admits(farm::kWoodFilter, farm::kDaffodil),
          "the filter's own list answers, not the tag: the plant filter takes Daffodil and not Oak, the wood filter the other way round");

    // the paper filter (a quick make): the filter admits a raw hemlock the paper tag does not carry, and does not admit one paper the tag does
    auto map_bag = bag_of({farm_item(0, farm::kFadedTreasureMap), farm_item(1, farm::kOraclesEye), farm_item(2, farm::kBlueInk, 10), farm_item(3, farm::kEasternHemlock)});
    repl::StartCraft map_req = start_of(*treasure, 1);
    map_req.general = {resource(farm::kFadedTreasureMap, 1), resource(farm::kOraclesEye, 1), resource(farm::kBlueInk, 10)};
    map_req.tags = {pick_of({resource(farm::kEasternHemlock, 1)}, {"Item.Resource.Processed.Paper"})};
    cr::Plan map_plan = map_bag ? cr::plan_start(map_req, filter_facts(data, *scribe, *map_bag, world, 2, true)) : cr::Plan{};
    CHECK_SHOWN(map_plan.ok, "a quick make takes a raw EasternHemlock the paper filter admits though its tag is the wood one: the filter is the list");
    auto paper_bag = bag_of({farm_item(0, farm::kFadedTreasureMap), farm_item(1, farm::kOraclesEye), farm_item(2, farm::kBlueInk, 10), farm_item(3, 6064634086376734729ll)});
    repl::StartCraft paper_req = map_req;
    paper_req.tags = {pick_of({resource(6064634086376734729ll, 1)}, {"Item.Resource.Processed.Paper"})};
    cr::Plan paper_plan = paper_bag ? cr::plan_start(paper_req, filter_facts(data, *scribe, *paper_bag, world, 2, true)) : cr::Plan{};
    CHECK_SHOWN(!paper_plan.ok && paper_plan.refusal.find("not one of the items of the gather-item filter") != std::string::npos,
          "a paper the tag admits and the filter does not is refused");

    // a crate station's recipe that the tags already served: its picks are the filter's 9 woods now, not the tag's 29
    auto crate_bag = bag_of({farm_item(0, farm::kOak, 30)});
    repl::StartCraft crate_req = start_of(*crate, 1);
    crate_req.tags = {pick_of({resource(farm::kOak, 30)}, {"Item.Resource.Raw.Wood"})};
    cr::Plan crate_plan = crate_bag ? cr::plan_start(crate_req, filter_facts(data, *crates, *crate_bag, world, 0, true)) : cr::Plan{};
    CHECK_SHOWN(crate_plan.ok, "a construction crate takes 30 Oak of the wood filter");
    auto outsider_bag = bag_of({farm_item(0, farm::kWoodTagOnly, 30)});
    repl::StartCraft outsider_req = crate_req;
    outsider_req.tags = {pick_of({resource(farm::kWoodTagOnly, 30)}, {"Item.Resource.Raw.Wood"})};
    cr::Plan outsider_plan = outsider_bag ? cr::plan_start(outsider_req, filter_facts(data, *crates, *outsider_bag, world, 0, true)) : cr::Plan{};
    CHECK_SHOWN(!outsider_plan.ok && outsider_plan.refusal.find("not one of the items of the gather-item filter") != std::string::npos,
          "a wood the tag admits and the filter does not is refused at the crate station too (its picks narrow to the 9)");
    SAYOK("a filter input's picks and the recipes it reaches");
}

/// A filter that lists no item (30 of the 51 the recipes name: the crates' own, the gatherers' fish, plants and ore, the processed goods of each profession) carries no tag query either, so the
/// input's own tags name its materials, as the tag check did before the filter table existed.  A file without the table (contentc before 47) keeps the crate stations on their tags, and refuses
/// a filter input at a profession's station (its list is not known).
static void test_filter_without_items(const content::ContentDb& db, const PlayerWorld& world) {
    namespace cr = aoc::worldd::crafting;
    constexpr int64_t kPlantsAnyFilter = 6064633967759720452ll, kFishAnyFilter = 6064633967742943235ll;
    constexpr int64_t kBluegill = 6064629326176452611ll, kCopperOre = 109162169652920ll;
    std::printf("a filter that lists no item: the input's tags decide\n");
    std::string error;
    auto loaded = db.craft_recipe_data(error);
    if (!loaded) {
        std::printf("  [FAIL] the recipes do not read: %s\n", error.c_str());
        ++g_fail;
        return;
    }
    content::CraftRecipeData data = std::move(*loaded);
    data.filter_items.clear();   // (what a version-45 file holds: the 21 filters that list items; here the plant filter of Mulch)
    data.filter_items[farm::kPlantFilter] = plants_filter();
    std::sort(data.filter_items[farm::kPlantFilter].begin(), data.filter_items[farm::kPlantFilter].end());
    data.filters_read = true;
    std::string station_error;
    const auto station_data = db.craft_station_data(station_error);
    const content::CraftStationData* stations = station_data ? &*station_data : nullptr;
    CHECK_SHOWN(stations != nullptr && data.tags_read, "the station definitions and the tag table are read");
    if (!stations) return;
    const content::CraftStationDef* crates = station_named(*stations, "ProcessingStation_CratePacking_Construction_Gathering");
    const content::CraftStationDef* cooking = station_named(*stations, "ProcessingStation_Cooking_T2");
    const content::CraftStationDef* farming = station_named(*stations, "ProcessingStation_Farming_T2");
    const content::CraftRecipe* herb_crate = recipe_named(data, "Recipe_Crate_Construction_Gathering_Herbalism");
    const content::CraftRecipe* bait = recipe_named(data, "Cooking_T1_Bait_Chum_Freshwater");
    const content::CraftRecipe* mulch = recipe_named(data, "Farming_T1_Mulch");
    CHECK_SHOWN(crates && cooking && farming && herb_crate && bait && mulch, "the crate station, the Cooking and Farming stations and the three recipes of the cases are in the data");
    if (!crates || !cooking || !farming || !herb_crate || !bait || !mulch) return;
    bool herb_unlisted = false, bait_unlisted = false;
    for (const content::CraftInput& in : herb_crate->inputs) herb_unlisted = herb_unlisted || (in.item == 0 && in.filter == kPlantsAnyFilter);
    for (const content::CraftInput& in : bait->inputs) bait_unlisted = bait_unlisted || (in.item == 0 && in.filter == kFishAnyFilter);
    CHECK_SHOWN(herb_unlisted && bait_unlisted && data.filter_items.count(kPlantsAnyFilter) == 0 && data.filter_items.count(kFishAnyFilter) == 0,
                "the Herbalism crate names the filter Raw_Plants_Any and the fish bait Raw_Fish_Any, and the file lists no item for either");

    // a construction crate: the tag decides, as before (Barley carries the plant tag; an Oak does not)
    const auto crate_start = [&](int64_t record, const content::CraftRecipeData& d) {
        auto bag = bag_of({farm_item(0, record, 30)});
        repl::StartCraft req = start_of(*herb_crate, 1);
        req.tags = {pick_of({resource(record, 30)}, {"Item.Resource.Raw.Plant"})};
        return bag ? cr::plan_start(req, filter_facts(d, *crates, *bag, world, 0, true)) : cr::Plan{};
    };
    const cr::Plan crate_daffodil = crate_start(farm::kDaffodil, data);
    CHECK_SHOWN(crate_daffodil.ok, "the Herbalism crate takes 30 Daffodil by the plant tag (its filter lists no item)");
    if (!crate_daffodil.ok) std::printf("    %s\n", crate_daffodil.refusal.c_str());
    const cr::Plan crate_barley = crate_start(farm::kBarley, data);
    CHECK_SHOWN(crate_barley.ok, "the Herbalism crate takes 30 Barley too: the plant tag admits it where Mulch's filter does not");
    const cr::Plan crate_oak = crate_start(farm::kOak, data);
    CHECK_SHOWN(!crate_oak.ok && crate_oak.refusal.find("does not carry the tag item.resource.raw.plant") != std::string::npos, "the Herbalism crate refuses an Oak: it does not carry the plant tag");

    // the fish bait at a profession's station: 50 fish by the fish tag
    const auto bait_start = [&](int64_t record, const content::CraftRecipeData& d) {
        auto bag = bag_of({farm_item(0, record, 50)});
        repl::StartCraft req = start_of(*bait, 1);
        req.tags = {pick_of({resource(record, 50)}, {"Item.Resource.Raw.Fish"})};
        return bag ? cr::plan_start(req, filter_facts(d, *cooking, *bag, world, 0)) : cr::Plan{};
    };
    const cr::Plan bait_fish = bait_start(kBluegill, data);
    CHECK_SHOWN(bait_fish.ok, "a Novice starts the freshwater bait chum at Cooking - Apprentice with 50 Bluegill: the fish tag names the pick");
    if (!bait_fish.ok) std::printf("    %s\n", bait_fish.refusal.c_str());
    const cr::Plan bait_plant = bait_start(farm::kDaffodil, data);
    CHECK_SHOWN(!bait_plant.ok && bait_plant.refusal.find("does not carry the tag item.resource.raw.fish") != std::string::npos, "the bait chum refuses 50 Daffodil: no fish tag");
    auto copper = bag_of({farm_item(0, kCopperOre, 30)});
    repl::StartCraft ore_req = start_of(*herb_crate, 1);
    ore_req.tags = {pick_of({resource(kCopperOre, 30)}, {"Item.Resource.Raw.Plant"})};
    const cr::Plan ore_plan = copper ? cr::plan_start(ore_req, filter_facts(data, *crates, *copper, world, 0, true)) : cr::Plan{};
    CHECK_SHOWN(!ore_plan.ok && ore_plan.refusal.find("does not carry the tag") != std::string::npos, "a copper ore is no plant for the Herbalism crate");

    // a list still decides where there is one: Barley is no pick for Mulch (the plant filter lists 22), however many tags it carries
    auto barley_bag = bag_of({farm_item(0, farm::kRotrootPulp), farm_item(1, farm::kWoodchips), farm_item(2, farm::kBarley)});
    repl::StartCraft barley_req = start_of(*mulch, 1);
    barley_req.general = {resource(farm::kWoodchips, 1)};
    barley_req.tags = {pick_of({resource(farm::kBarley, 1)}, {"Item.Resource.Raw.Plant"})};
    const cr::Plan barley_plan = barley_bag ? cr::plan_start(barley_req, filter_facts(data, *farming, *barley_bag, world)) : cr::Plan{};
    CHECK_SHOWN(!barley_plan.ok && barley_plan.refusal.find("not one of the items of the gather-item filter") != std::string::npos, "Mulch still refuses Barley: its filter lists items, and Barley is not one");

    // a file without the table: the crate keeps its tags, a profession's station refuses the filter input (its list is not known) and says why
    content::CraftRecipeData old_file = data;
    old_file.filter_items.clear();
    old_file.filters_read = false;
    const cr::Plan old_crate = crate_start(farm::kDaffodil, old_file);
    CHECK_SHOWN(old_crate.ok, "a file of contentc before 47: the Herbalism crate is still made by its tags");
    auto mulch_bag = bag_of({farm_item(0, farm::kRotrootPulp), farm_item(1, farm::kWoodchips), farm_item(2, farm::kDaffodil)});
    repl::StartCraft mulch_req = start_of(*mulch, 1);
    mulch_req.general = {resource(farm::kWoodchips, 1)};
    mulch_req.tags = {pick_of({resource(farm::kDaffodil, 1)}, {"Item.Resource.Raw.Plant"})};
    const cr::Plan old_mulch = mulch_bag ? cr::plan_start(mulch_req, filter_facts(old_file, *farming, *mulch_bag, world)) : cr::Plan{};
    CHECK_SHOWN(!old_mulch.ok && old_mulch.refusal.find("lists no items for filters") != std::string::npos, "a file of contentc before 47: Mulch is refused with the reason (rebuild the data), not started by the wider tag");
    const cr::Plan old_bait = [&] {
        auto bag = bag_of({farm_item(0, kBluegill, 50)});
        repl::StartCraft req = start_of(*bait, 1);
        req.tags = {pick_of({resource(kBluegill, 50)}, {"Item.Resource.Raw.Fish"})};
        return bag ? cr::plan_start(req, filter_facts(old_file, *cooking, *bag, world)) : cr::Plan{};
    }();
    CHECK_SHOWN(!old_bait.ok && old_bait.refusal.find("lists no items for filters") != std::string::npos, "a file of contentc before 47: the bait chum is refused with the same reason");
    SAYOK("a filter that lists no item");
}

/// The Novice loop from a fresh character: Mulch, then the five crops, 50 experience a tier-0 unit up to the Novice cap, and the tier-1 recipes after a certification.
static void test_farming_loop(const content::ContentDb& db, const PlayerWorld& world) {
    namespace cr = aoc::worldd::crafting;
    std::printf("the Farming Novice loop from a fresh character\n");
    std::string error;
    auto loaded = db.craft_recipe_data(error);
    if (!loaded) {
        std::printf("  [FAIL] the recipes do not read: %s\n", error.c_str());
        ++g_fail;
        return;
    }
    content::CraftRecipeData data = std::move(*loaded);
    data.filter_items[farm::kPlantFilter] = plants_filter();
    std::sort(data.filter_items[farm::kPlantFilter].begin(), data.filter_items[farm::kPlantFilter].end());
    data.filters_read = true;
    std::string station_error;
    const auto station_data = db.craft_station_data(station_error);
    const content::CraftStationData* stations = station_data ? &*station_data : nullptr;
    const pr::Book* book = world.professions();
    const content::CraftStationDef* farming = stations ? station_named(*stations, "ProcessingStation_Farming_T2") : nullptr;
    const content::CraftRecipe* mulch = recipe_named(data, "Farming_T1_Mulch");
    CHECK_SHOWN(farming && mulch && book, "the Farming - Apprentice station, the Mulch recipe and the 22 professions are read");
    if (!farming || !mulch || !book) return;
    pr::Professions fresh(*book);
    CHECK_SHOWN(fresh.valid() && fresh.tier(farm::kFarming) == 0 && fresh.level(farm::kFarming) == 1 && fresh.xp(farm::kFarming) == 0,
          "a fresh character holds Farming at Novice, level 1, no XP");
    const content::CraftReward* reward = data.reward_of(*mulch);
    bool makes_mulch = false;
    if (reward) {
        for (const content::CraftRewardContainer& box : reward->containers)
            for (const std::vector<content::CraftRewardItem>& units : box.rewards)
                for (const content::CraftRewardItem& item : units) makes_mulch = makes_mulch || (item.item == farm::kMulch && item.quantity == 1);
    }
    CHECK_SHOWN(makes_mulch, "Farming_T1_Mulch yields one Mulch a unit: the only producer of the five crops' Mulch");
    auto bag = bag_of({farm_item(0, farm::kRotrootPulp), farm_item(1, farm::kWoodchips), farm_item(2, farm::kDaffodil)});
    repl::StartCraft mulch_req = start_of(*mulch, 1);
    mulch_req.general = {resource(farm::kWoodchips, 1)};
    mulch_req.tags = {pick_of({resource(farm::kDaffodil, 1)}, {"Item.Resource.Raw.Plant"})};
    cr::Plan mulch_plan = bag ? cr::plan_start(mulch_req, filter_facts(data, *farming, *bag, world, 0)) : cr::Plan{};
    CHECK_SHOWN(mulch_plan.ok, "the first recipe a fresh character can start is the Mulch job, with the plant picked from the filter");
    bool crops_start = true;
    for (int i = 0; i < 5; ++i) {
        const content::CraftRecipe* crop = recipe_named(data, farm::kCrop[i]);
        auto crop_bag = bag_of({farm_item(0, farm::kWater), farm_item(1, farm::kSeed[i]), farm_item(2, farm::kMulch)});
        repl::StartCraft req = crop ? start_of(*crop, 1) : repl::StartCraft{};
        req.general = {resource(farm::kMulch, 1)};
        cr::Plan plan = crop && crop_bag ? cr::plan_start(req, filter_facts(data, *farming, *crop_bag, world, 0)) : cr::Plan{};
        if (!plan.ok) {
            std::printf("    %s: %s\n", farm::kCrop[i], plan.refusal.c_str());
            crops_start = false;
        }
    }
    CHECK_SHOWN(crops_start, "the five crops (Blackberry, Cotton, Flax, Celery, Lettuce) start with a Water, a seed and a Mulch at the same station");
    {   // the real recipes pay what the loop says: a unit of a tier-0 Farming recipe is 50 Farming experience at Common, and the Mulch job makes the Mulch
        const auto facts = [&](int64_t item) { return world.item_rarity_facts(item); };
        const auto roll = [&](const content::CraftRecipe& recipe, int quantity, std::vector<svc::JobResource> paid) {
            svc::ProcessingJob job;
            job.recipe_guid = recipe.guid;
            job.quantity = quantity;
            job.resources = std::move(paid);
            job.job_guid = "00000000000000000000000000000007";
            return cr::roll_job(data, job, 1, cr::Ratings{}, facts);
        };
        const auto one_mulch = roll(*mulch, 1, {{farm::kWoodchips, {{0, 1, false}}}, {farm::kDaffodil, {{0, 1, false}}}});
        CHECK_SHOWN(one_mulch && one_mulch->profession == farm::kFarming && one_mulch->profession_xp == 50 && one_mulch->products.size() == 1 && one_mulch->products[0].item == farm::kMulch &&
                        one_mulch->products[0].quantity == 1,
                    "a Mulch job of one unit, paid with a Common Woodchips and a Common Daffodil, makes 1 Mulch and pays 50 Farming experience");
        const auto many_mulch = roll(*mulch, 32, {{farm::kWoodchips, {{0, 32, false}}}, {farm::kDaffodil, {{0, 32, false}}}});
        CHECK_SHOWN(many_mulch && many_mulch->profession_xp == 1600 && many_mulch->products.size() == 1 && many_mulch->products[0].quantity == 32, "32 units of Mulch make 32 Mulch and pay 1,600 Farming experience (level 2)");
        bool crops_pay = true;
        for (int i = 0; i < 5; ++i) {
            const content::CraftRecipe* crop = recipe_named(data, farm::kCrop[i]);
            const auto made = crop ? roll(*crop, 1, {{farm::kMulch, {{0, 1, false}}}}) : std::nullopt;
            const bool pays = made && made->profession == farm::kFarming && made->profession_xp == 50 && !made->products.empty() && made->products[0].quantity >= 1;
            if (!pays) std::printf("    %s does not pay 50 Farming experience for a unit\n", farm::kCrop[i]);
            crops_pay = crops_pay && pays;
        }
        CHECK_SHOWN(crops_pay, "each of the five crops pays 50 Farming experience for a unit paid with a Common Mulch, and yields its produce");
    }
    CHECK_SHOWN(cr::profession_xp(content::XpRule::Processing, 0, 0, 1) == 50, "a tier-0 unit pays 50 profession experience at Common");
    CHECK_SHOWN(cr::profession_xp(content::XpRule::Processing, 0, 0, 32) == 1600, "32 units pay 1,600 experience: level 2");
    fresh.add_xp(farm::kFarming, 1600);
    CHECK_SHOWN(fresh.level(farm::kFarming) == 2 && fresh.xp(farm::kFarming) == 1600, "32 units of a tier-0 job make the Novice level 2");
    fresh.add_xp(farm::kFarming, 38400);
    CHECK_SHOWN(fresh.level(farm::kFarming) == 10 && fresh.xp(farm::kFarming) == 40000, "800 units reach level 10 (40,000 experience)");
    fresh.add_xp(farm::kFarming, 10000);
    CHECK_SHOWN(fresh.level(farm::kFarming) == 10 && fresh.xp(farm::kFarming) == 48399, "a Novice's experience is held at 48,399 until a certification");
    const pr::Change promoted = fresh.promote(farm::kFarming);
    CHECK_SHOWN(promoted.promoted() && fresh.tier(farm::kFarming) == 1 && fresh.level(farm::kFarming) == 10, "at the Novice cap a certification makes Farming Apprentice");
    const content::CraftRecipe* apple = recipe_named(data, "Farming_T2_Fruit_Apple");
    auto apple_bag = bag_of({farm_item(0, farm::kSpringdrawnWater), farm_item(1, farm::kAppleSeed), farm_item(2, farm::kMulch)});
    repl::StartCraft apple_req = apple ? start_of(*apple, 1) : repl::StartCraft{};
    apple_req.general = {resource(farm::kMulch, 1)};
    cr::Plan novice_apple = apple && apple_bag ? cr::plan_start(apple_req, filter_facts(data, *farming, *apple_bag, world, 0)) : cr::Plan{};
    cr::Plan apprentice_apple = apple && apple_bag ? cr::plan_start(apple_req, filter_facts(data, *farming, *apple_bag, world, 1)) : cr::Plan{};
    CHECK_SHOWN(!novice_apple.ok && novice_apple.refusal.find("needs certification tier 1") != std::string::npos, "an Apple stays out of a Novice's reach (tier 1)");
    CHECK_SHOWN(apprentice_apple.ok, "the certification opens the tier-1 Apple at the same Farming - Apprentice station");
    SAYOK("the Farming Novice loop");
}

/// The real file's table, when the data has one (contentc version 47): the filters' items, on the values the design records hold.
static void test_filter_real_data(const content::ContentDb& db) {
    std::printf("the real content.db's gather-item filters\n");
    std::string error;
    const auto data = db.craft_recipe_data(error);
    if (!data) {
        std::printf("  [FAIL] the recipes do not read: %s\n", error.c_str());
        ++g_fail;
        return;
    }
    if (!data->filters_read) {
        std::printf("  [skip] %s has no craft_filter_item (contentc before 47): the filters' items are not checked here\n", db.path().c_str());
        return;
    }
    const auto plants = data->filter_items.find(farm::kPlantFilter);
    const auto woods = data->filter_items.find(farm::kWoodFilter);
    size_t rows = 0;
    for (const auto& [filter, items] : data->filter_items) rows += items.size();
    CHECK_SHOWN(plants != data->filter_items.end() && plants->second.size() == 22 && std::binary_search(plants->second.begin(), plants->second.end(), farm::kDaffodil) &&
              !std::binary_search(plants->second.begin(), plants->second.end(), farm::kBarley),
          "ALL_Raw_Plants_Any holds its 22 plants (Daffodil, not Barley)");
    CHECK_SHOWN(woods != data->filter_items.end() && woods->second.size() == 9, "Raw_Wood_Any holds its 9 woods");
    // the filters the recipe inputs name: 21 list items (236 rows); the other 30 list none (the trade and construction crates' own filters, the gatherers' fish, plants and ore, the processed goods
    // of each profession) and name their materials by the input's tags, so the rule has to fall back on the tags for them
    std::set<int64_t> named;
    for (const auto& [guid, r] : data->recipes) {
        for (const content::CraftInput& in : r.inputs) {
            if (in.item == 0 && in.filter != 0) named.insert(in.filter);
        }
    }
    size_t listed = 0;
    for (const int64_t filter : named) listed += data->filter_items.count(filter);
    CHECK_SHOWN(named.size() == 51 && data->filter_items.size() == 21 && listed == 21 && rows == 236, "of the 51 filters the recipe inputs name, 21 list items (236 rows) and 30 list none");
    // every filter input can be picked from: its filter lists items, or at least one item carries all of the input's tags
    size_t unpickable = 0;
    for (const auto& [guid, r] : data->recipes) {
        for (const content::CraftInput& in : r.inputs) {
            if (in.item != 0 || in.filter == 0 || data->filter_items.count(in.filter) != 0) continue;
            const std::vector<std::string> want = crafting::tag_key(crafting::split_tags(in.tags));
            std::vector<int64_t> common;
            bool first = true;
            for (const std::string& tag : want) {
                const auto it = data->tag_items.find(tag);
                const std::vector<int64_t> none;
                const std::vector<int64_t>& have = it == data->tag_items.end() ? none : it->second;
                if (first) {
                    common = have;
                    first = false;
                } else {
                    std::vector<int64_t> both;
                    std::set_intersection(common.begin(), common.end(), have.begin(), have.end(), std::back_inserter(both));
                    common = std::move(both);
                }
            }
            if (want.empty() || common.empty()) ++unpickable;
        }
    }
    CHECK_SHOWN(unpickable == 0, "every filter input that lists no item has an item that carries all of its tags (the rule can serve it)");
    SAYOK("the real file's filters");
}

#include "cooking_tests.inc"
#include "husbandry_tests.inc"

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
    test_artisan(*db, world);
    test_filter_table();
    test_filter_rules(*db, world);
    test_filter_without_items(*db, world);
    test_farming_loop(*db, world);
    test_filter_real_data(*db);
    test_cooking_recipes(*db, world);
    test_cooking_conditions(*db, world);
    test_cooking_novice_station(*db, world);
    test_cooking_shop_and_saved_jobs(*db, world);
    test_husbandry_recipes(*db, world);
    test_husbandry_sources(*db, world);
    test_husbandry_outputs(*db, world);
    test_husbandry_hunts(*db, world);
    test_husbandry_livestock_chain(*db, world);
    test_husbandry_conditions(*db, world);
    test_husbandry_novice(*db, world);
    test_husbandry_picks(*db, world);
    test_husbandry_every_product(*db, world);
    test_husbandry_saved_jobs(*db, world);
    test_scrolls(path, world);
    test_tools_and_vendor(path, world);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
