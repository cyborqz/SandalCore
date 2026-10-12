// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_ability_content.cpp: the reader of the ability tables (plan D2, content/ability_content.h)
//    - what it refuses: no file, no SQLite file, a content.db of an older contentc, one without the ability tables;
//    - on the content.db of the working tree (dist/content/content.db, or AOC_CONTENT_DB; built by `go run ./cmd/contentc`):
//      the Bard's abilities and what they reach, the rows as the Go readers give them, and, with the stat engine and the bundle the file
//      stores, the retail numbers of the research: the Mana costs (56.875 ... 199.0625), the cooldowns and charges, the Sprint's speed (1.3 at
//      level 25 with the ranks the game grants by itself), Glee's 0.03 and Note_Comedy's 1.05.
//  Without a content.db the second half says so and is skipped.
// ============================================================================
#include "content/ability_content.h"
#include "stats/stat_engine.h"
#include "stats/stat_program.h"

#include <sqlite3.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace aoc::worldd;
using namespace aoc::worldd::content;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)
#define CHECKF(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { std::printf("  [FAIL] "); std::printf(__VA_ARGS__); std::printf("\n"); g_fail++; } \
} while (0)

#ifndef ABILITY_CONTENT_DB_PATH
#define ABILITY_CONTENT_DB_PATH "dist/content/content.db"
#endif

namespace {

bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

std::string temp_path(const char* name) {
    return (std::filesystem::temp_directory_path() / (std::string("aoc_ability_content_") + name)).string();
}

/// A SQLite file with the given statements run in it.
std::string make_db(const char* name, const char* sql) {
    const std::string path = temp_path(name);
    std::filesystem::remove(path);
    sqlite3* db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) std::exit(2);
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::printf("  [FAIL] test setup: %s\n", err ? err : "?");
        std::exit(2);
    }
    sqlite3_close(db);
    return path;
}
// ~ C | O | A :: C | Y | B ~

void test_refusals() {
    std::printf("what the reader refuses\n");
    std::string err;
    CHECK(!AbilityContent::open(temp_path("missing.db"), err) && contains(err, "not found"), "a missing file");
    const std::string text = temp_path("text.db");
    {
        std::ofstream f(text);
        f << "this is not SQLite";
    }
    err.clear();
    CHECK(!AbilityContent::open(text, err) && !err.empty(), "a file that is no database");
    const std::string old = make_db("old.db", "CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL); "
                                              "INSERT INTO meta VALUES ('schema_version', '1'), ('contentc.version', '5');");
    err.clear();
    CHECK(!AbilityContent::open(old, err) && contains(err, "version 5") && contains(err, "rebuild"), "a content.db of contentc 5 says to rebuild it");
    const std::string other_schema = make_db("schema.db", "CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL); "
                                                          "INSERT INTO meta VALUES ('schema_version', '9'), ('contentc.version', '6');");
    err.clear();
    CHECK(!AbilityContent::open(other_schema, err) && contains(err, "schema version"), "another schema version");
    const std::string no_tables = make_db("notables.db", "CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL); "
                                                         "INSERT INTO meta VALUES ('schema_version', '1'), ('contentc.version', '6');");
    err.clear();
    CHECK(!AbilityContent::open(no_tables, err) && contains(err, "lacks a table"), "version 6 without the ability tables");
    const std::string no_meta = make_db("nometa.db", "CREATE TABLE other (x INTEGER);");
    err.clear();
    CHECK(!AbilityContent::open(no_meta, err) && contains(err, "not a content.db"), "a database with no meta table");
}

constexpr uint64_t kBardEnum = 2;
constexpr int64_t kBardClassGuid = 108640158570105;   // the class record: CombatInfo.PrimaryClassId.Guid

stats::StatInputs bard(int level, std::vector<stats::SkillRank> ranks) {
    stats::StatInputs in;
    in.class_enum = static_cast<int>(kBardEnum);
    in.level = level;
    in.ranks = std::move(ranks);
    return in;
}

void test_real(const std::string& path) {
    std::printf("the ability tables of %s\n", path.c_str());
    std::string err;
    auto opened = AbilityContent::open(path, err);
    CHECKF(opened.has_value(), "open: %s", err.c_str());
    if (!opened) return;
    const AbilityContent& db = *opened;
    CHECK(db.contentc_version() >= kMinAbilityContentcVersion, "contentc version 6 or later");

    // the Bard's abilities: the research's 27 and the songs of the talent slots (plan D1's tables list every option), each a row with its hits,
    // effects and expressions
    std::vector<int64_t> ranks;
    CHECK(db.class_ability_guids(kBardClassGuid, ranks, err), "the Bard's abilities");
    std::set<int64_t> bard_abilities(ranks.begin(), ranks.end());
    CHECKF(bard_abilities.size() >= 27, "the Bard has %zu abilities, the research counted 27 without the songs", bard_abilities.size());
    std::set<int64_t> seen_hits, seen_effects;
    for (int64_t g : bard_abilities) {
        auto a = db.ability(g, err);
        CHECKF(a.has_value(), "ability %lld: %s", static_cast<long long>(g), err.c_str());
        if (!a) continue;
        std::vector<AbilityHitRow> hits;
        std::vector<std::pair<int64_t, int64_t>> effects;
        std::vector<CostRow> costs;
        CHECK(db.ability_hits(g, hits, err) && db.ability_effects(g, effects, err) && db.costs(g, costs, err), "an ability's hits, effects and costs");
        for (const AbilityHitRow& h : hits) {
            auto row = db.hit(h.hit_guid, err);
            CHECKF(row.has_value(), "%s: hit %lld: %s", a->name.c_str(), static_cast<long long>(h.hit_guid), err.c_str());
            seen_hits.insert(h.hit_guid);
        }
        for (const auto& [key, e] : effects) {
            auto row = db.effect(e, err);
            CHECKF(row.has_value(), "%s: effect %lld: %s", a->name.c_str(), static_cast<long long>(e), err.c_str());
            seen_effects.insert(e);
        }
        if (a->cooldown_expr != 0) {
            auto ex = db.expr(a->cooldown_expr, err);
            CHECKF(ex.has_value() && ex->program >= 0, "%s: the cooldown expression compiles (%s)", a->name.c_str(), ex ? ex->error.c_str() : err.c_str());
        }
    }
    CHECK(seen_hits.size() > 20 && seen_effects.size() > 10, "the Bard's abilities reach hits and effects");

    // Jest: its cost is Mana, its montage the one the capture names, and the unnamed index has none
    auto jest = db.ability_by_name("Bard_Misc_QuickWit_Jest", err);
    CHECKF(jest.has_value(), "Jest: %s", err.c_str());
    if (jest) {
        std::vector<MontageRow> ms;
        CHECK(db.montages(jest->guid, ms, err) && !ms.empty() && ms[0].net_guid && *ms[0].net_guid == 16448494777319543161ull, "Jest's montage NetGUID, a uint64 above 2^63");
        CHECK(ms.size() >= 1 && ms[0].anim_index == 0 && contains(ms[0].soft_path, "QuickWit_Help"), "Jest's montage path");
        CHECK(jest->applicable_targets == 5 && contains(jest->target_requirement, "Always"), "Jest: a target is required, Self or Ally");
    }
    CHECK(!db.ability_by_name("No_Such_Ability", err) && contains(err, "No_Such_Ability"), "an ability that is none");
    CHECK(!db.ability(1, err) && !db.hit(1, err) && !db.effect(1, err) && !db.projectile(1, err) && !db.zone(1, err) && !db.expr(0, err), "rows that are none");

    // Fortissimo consumes five stacks of Bard_Resonance
    auto fort = db.ability_by_name("Bard_Misc_Fortissimo", err);
    std::vector<EffectCostRow> ecs;
    CHECK(fort && db.effect_costs(fort->guid, ecs, err) && ecs.size() == 1 && ecs[0].effect_guid == 6064632468947286527ll, "Fortissimo's effect cost");
    if (!ecs.empty()) {
        auto n = db.expr(ecs[0].num_expr, err);
        CHECK(n && n->text == "5", "...is 5 stacks");
        auto res = db.effect(ecs[0].effect_guid, err);
        CHECK(res && res->name == "Bard_Resonance", "...of Bard_Resonance");
    }

    // Nimble Dance: a zone with the record's life time and tick, a hit fired by it
    auto nimble = db.ability_by_name("Bard_Dance_Nimble", err);
    std::vector<AbilityHitRow> nh;
    CHECK(nimble && db.ability_hits(nimble->guid, nh, err) && !nh.empty(), "Nimble's hits");
    bool zone_ok = false;
    for (const AbilityHitRow& h : nh) {
        if (h.source != "zone") continue;
        auto z = db.zone(h.owner_guid, err);
        zone_ok = z && z->life_time == 30 && z->tick_rate == 1.5 && !z->hits.empty();
        break;
    }
    CHECK(zone_ok, "Nimble's lingering zone: 30 s, a tick every 1.5 s");
    auto pep = db.effect_by_name("Bard_Pep", err);
    CHECK(pep && !pep->infinite_duration, "Bard_Pep");

    // projectiles of QuickWit
    auto quick = db.ability_by_name("Bard_Misc_QuickWit", err);
    std::vector<AbilityHitRow> qh;
    CHECK(quick && db.ability_hits(quick->guid, qh, err), "QuickWit's hits");
    bool projectile_ok = false;
    for (const AbilityHitRow& h : qh) {
        if (h.source != "projectile") continue;
        auto p = db.projectile(h.owner_guid, err);
        projectile_ok = p && p->speed > 0 && !p->hits.empty() && p->life_time > 0;
        break;
    }
    CHECK(projectile_ok, "QuickWit's projectiles have a speed and hits");
// built by CoA with C Y B

    // every expression is there once, in id order, and a program it names is an expression program of the stored bundle
    auto data = db.stat_program_data(err);
    CHECKF(data.has_value(), "stat program: %s", err.c_str());
    if (!data) return;
    stats::StatBundle b;
    CHECKF(stats::parse_bundle(*data, b, err), "the stored bundle: %s", err.c_str());
    if (!err.empty()) return;
    int64_t n = 0, compiled = 0;
    bool consistent = true;
    for (int64_t id = 1;; ++id) {
        std::string e;
        auto row = db.expr(id, e);
        if (!row) break;
        ++n;
        if (row->program >= 0) {
            ++compiled;
            consistent = consistent && b.is_expr(row->program);
        } else {
            consistent = consistent && !row->error.empty();
        }
    }
    CHECKF(n > 700 && compiled > 450 && consistent && compiled >= static_cast<int64_t>(b.exprs.size()), "%lld expressions, %lld with a program, each in the bundle's exprs", static_cast<long long>(n),
           static_cast<long long>(compiled));

    // ---- the retail numbers, with the stat engine ----
    const stats::StatState none;
    std::vector<stats::SkillRank> at25;
    CHECK(db.auto_ranks_at(25, at25, err) && !at25.empty(), "the ranks the game grants a level-25 character");
    std::vector<stats::SkillRank> at1, at35;
    CHECK(db.auto_ranks_at(1, at1, err) && db.auto_ranks_at(35, at35, err) && at1.size() < at25.size() && at25.size() < at35.size(), "more ranks at 35 than at 25 than at 1");
    stats::StatInputs in = bard(25, at25);
    stats::StatResult bare;
    CHECKF(stats::run_stats(b, in, none, bare, err), "a level-25 Bard: %s", err.c_str());
    stats::ExprEvaluator ev(b, in, none, bare);
    stats::ExprHost host;
    auto eval = [&](int64_t expr_id, double& v) {
        std::string e;
        auto row = db.expr(expr_id, e);
        if (!row || row->program < 0) return false;
        return ev.eval(row->program, host, v, e);
    };
    const struct { const char* name; double mana; } costs[] = {
        {"Bard_Misc_QuickWit_Jest", 56.875}, {"Bard_Misc_Flourish", 56.875}, {"Bard_Dance_Mesmerizing", 85.3125}, {"Bard_Story_Joy", 85.3125},
        {"Bard_Dance_Nimble", 113.75}, {"Bard_Misc_Misdirection", 113.75}, {"Bard_Saga", 199.0625}};
    for (const auto& c : costs) {
        auto a = db.ability_by_name(c.name, err);
        std::vector<CostRow> cr;
        double v = 0;
        bool ok = a && db.costs(a->guid, cr, err);
        double mana = -1;
        for (const CostRow& r : cr)
            if (r.stat_name == "Mana" && eval(r.amount_expr, v)) mana = v;
        CHECKF(ok && mana == c.mana, "%s: Mana %g, retail %g", c.name, mana, c.mana);
    }
    auto flourish = db.ability_by_name("Bard_Misc_Flourish", err);
    double cd = 0, charges = 0;
    CHECK(flourish && eval(flourish->cooldown_expr, cd) && cd == 20 && eval(flourish->charges_expr, charges) && charges == 2, "Flourish: a 20 s cooldown, 2 charges");

    // the Sprint: MoveSpeedMult is 1 + 0.2 + 0.05 per Universal sprint skill held
    auto sprint = db.effect_by_name("General_Sprint", err);
    std::vector<ModRow> mods;
    CHECK(sprint && db.mods("effect", sprint->guid, mods, err) && mods.size() == 1, "General_Sprint has one stat mod");
    if (sprint && mods.size() == 1) {
        auto speed_of = [&](const std::vector<stats::SkillRank>& rk) {
            stats::StatInputs si = bard(25, rk);
            stats::StatResult r0, r1;
            std::string e;
            if (!stats::run_stats(b, si, none, r0, e)) return -1.0;
            stats::ExprEvaluator ev0(b, si, none, r0);
            std::string x;
            auto row = db.expr(mods[0].value_expr, x);
            double mod = 0;
            if (!row || row->program < 0 || !ev0.eval(row->program, host, mod, x)) return -2.0;
            stats::ActiveEffect fx;
            fx.effect = {stats::kRefTypeEffect, sprint->guid};
            fx.stacks = 1;
            fx.mods = {{mods[0].stat_guid, mod}};
            si.effects = {fx};
            if (!stats::run_stats(b, si, none, r1, e)) return -3.0;
            auto it = b.by_guid.find(mods[0].stat_guid);
            return it == b.by_guid.end() ? -4.0 : r1.values[it->second];
        };
        const double f13 = static_cast<double>(static_cast<float>(1.3)), f12 = static_cast<double>(static_cast<float>(1.2)), f135 = static_cast<double>(static_cast<float>(1.35));
        CHECKF(speed_of(at25) == f13, "the level-25 Bard sprints at MoveSpeedMult 1.3 (%.9g)", speed_of(at25));
        CHECK(speed_of(at1) == f12 || speed_of(at1) == static_cast<double>(static_cast<float>(1.25)), "a level-1 Bard: 1.2 (or 1.25 at level 5)");
        CHECKF(speed_of(at35) == f135, "with all three Universal sprint skills 1.35 (%.9g)", speed_of(at35));
        CHECK(speed_of({}) == f12, "no skill: 1.2");
    }
// ~ C Y B :: CoA ~

    // Glee on Mitigation (x (1 - v)) with the talent Merriment, Note_Comedy on HealingMult (x v)
    auto glee = db.effect_by_name("Bard_Glee", err);
    std::vector<ModRow> gm;
    CHECK(glee && db.mods("effect", glee->guid, gm, err) && !gm.empty(), "Bard_Glee's mods");
    auto comedy = db.effect_by_name("Bard_Note_Comedy", err);
    std::vector<ModRow> cm;
    CHECK(comedy && db.mods("effect", comedy->guid, cm, err) && cm.size() == 1 && cm[0].stat_name == "HealingMult", "Bard_Note_Comedy raises HealingMult");
    double v = 0;
    CHECK(cm.size() == 1 && eval(cm[0].value_expr, v) && v == 1.05, "...by 1.05");
    CHECK(!gm.empty() && gm[0].stat_name == "Mitigation" && eval(gm[0].value_expr, v) && std::fabs(v - 0.02) < 1e-6, "Glee is 0.02 without Merriment (0.03 with it)");
}

}  // namespace

int main() {
    test_refusals();
    std::string path = ABILITY_CONTENT_DB_PATH;
    if (const char* env = std::getenv("AOC_CONTENT_DB")) path = env;
    std::error_code ec;
    if (std::filesystem::is_regular_file(std::filesystem::path(path), ec)) {
        test_real(path);
    } else {
        std::printf("no content.db at %s (cd services && go run ./cmd/contentc): the real-data half is skipped\n", path.c_str());
    }
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
