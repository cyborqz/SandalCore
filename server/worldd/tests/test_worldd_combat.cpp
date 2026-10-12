// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_combat.cpp: the combat core (plan E1, stage 3, part 1) on the REAL content.db of the working tree (dist/content/content.db, or
//  AOC_CONTENT_DB) and dist/content/npc_motion.tsv
//
//    the data         the combat tables (the damage profile, the numbers, the alignments) load and the profile's stats are the stat program's
//    NPC stats        the stat engine's run for a kind of NPC at its level gives the MaxHealth the retail captures show (the golden table of the research:
//                     263 pairs of 89 archetypes, the ones here are a spread of the area's types)
//    the hit rules    the rolls, the element and the aspect of a hit, the damage of a player's hit on an NPC and an NPC's on a player
// Without a content.db the test says so and is skipped.
// ============================================================================
#include "ability_rig.h"
#include "combat_rig.h"
#include "combat/combat_data.h"
#include "combat/hit_resolver.h"
#include "combat/npc_stats.h"
#include "npc/motion_table.h"
#include "spawns/captured_spawns.h"
#include "spawns/generic_npc.h"

#include <bit>
#include <cmath>
#include <cstring>
#include <set>

#ifndef NPC_MOTION_PATH
#define NPC_MOTION_PATH "dist/content/npc_motion.tsv"
#endif

using namespace ability_rig;
using namespace aoc::worldd;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)
#define CHECKF(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { std::printf("  [FAIL] "); std::printf(__VA_ARGS__); std::printf("\n"); g_fail++; } \
} while (0)

namespace {

struct Fixture {
    std::string                          path;
    std::optional<content::ContentDb>    db;
    const bootstrap::StatContext*        ctx = nullptr;
    npc::MotionTable                     motion;
    std::optional<combat::CombatData>    data;
    std::optional<abilities::Catalog>    catalog;
    std::string                          problem;

    Fixture() {
        path = real_db_path();
        std::string error;
        db = content::ContentDb::open(path, error);
        if (!db) {
            problem = "content.db: " + error;
            return;
        }
        ctx = &world_stats::of(*db);
        if (!motion.load(NPC_MOTION_PATH, error)) {
            problem = std::string("npc_motion.tsv: ") + error;
            return;
        }
        const auto rows = db->combat_rows(error);
        if (!rows) {
            problem = "the combat tables: " + error;
            return;
        }
        data = combat::CombatData::build(*rows, *ctx->bundle, error);
        if (!data) {
            problem = "the combat data: " + error;
            return;
        }
        catalog = abilities::Catalog::load(path, *ctx->bundle, error);
        if (!catalog) problem = "the ability tables: " + error;
    }
    bool ok() const { return problem.empty(); }
// (C-O-A + Cyb)

    /// The Blueprint with the class name `leaf` ("Wolf_C").
    std::optional<npc::MotionStats> blueprint(const std::string& leaf) const {
        const std::string suffix = "." + std::string("Default__") + leaf;
        for (const auto& [path, row] : motion.rows()) {
            if (path.size() > suffix.size() && path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0) return row;
        }
        return std::nullopt;
    }
};

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// ---- the data -----------------------------------------------------------------------------------------------------------------------

void test_data(Fixture& f) {
    std::printf("the combat data\n");
    const combat::CombatData& d = *f.data;
    CHECK(d.profile().elements.size() == 11, "the damage profile has the eleven elements (four physical, seven magical)");
    CHECK(d.profile().physical.damage != 0 && d.profile().magical.damage != 0 && d.profile().physical.damage != d.profile().magical.damage, "the two aspects have their own Power stats");
    CHECK(d.profile().element("Element.Magical.Fire") && d.profile().element("Element.Physical.Slash") && !d.profile().element("Element.Healing"), "elements are found by tag");
    CHECK(near(d.in_combat_timer(), 6.0, 1e-9) && near(d.global_damage_multiplier(), 1.0, 1e-9) && near(d.activation_leeway_cm(), 1000.0, 1e-9),
          "DefaultCombat.ini's numbers: in combat for 6 s, damage multiplier 1, cast leeway 1,000 cm");
    const combat::CombatStatIds& id = d.ids();
    CHECK(id.health == content::kGuidHealth && id.max_health == content::kGuidMaxHealth && id.physical_crit_chance != 0 && id.physical_evasion != 0 && id.threat_generation != 0,
          "the stats the hit rules read are found by name in the stat program");
    // who fights whom: a Wolf and a Rabid are enemies of the players, a questgiver and a guard-less villager are not
    const auto wolf = f.blueprint("Wolf_C");
    CHECK(wolf.has_value(), "the Blueprint table has the Wolf");
    if (wolf) {
        std::vector<int64_t> al;
        for (const int64_t a : wolf->alignments) al.push_back(a);
        CHECK(d.relation(al, combat::CombatData::player_alignments()) == combat::Relation::Hostile, "a Wolf is hostile to a player");
    }
    const auto quest = f.blueprint("Quest_NPC_Master_C");
    if (quest) {
        std::vector<int64_t> al(quest->alignments.begin(), quest->alignments.end());
        CHECK(d.relation(al, combat::CombatData::player_alignments()) != combat::Relation::Hostile, "a quest master is not hostile to a player");
    }
}

// ---- NPC stats ----------------------------------------------------------------------------------------------------------------------

struct Golden {
    const char* blueprint;
    int         level;
    double      max_health;
};

// research/combat picks.json: tables.npc_level_maxhealth (the MaxHealth the retail server sent an NPC of the Blueprint at the level)
constexpr Golden kGolden[] = {
    {"Wolf_C", 5, 320},          {"Wolf_C", 6, 372},          {"Wolf_C", 7, 444},          {"Wolf_C", 11, 860},         {"Wolf_C", 12, 1011},
    {"Rabid_C", 8, 675},         {"Rabid_C", 9, 793},         {"Rabid_C", 11, 1089},       {"Rabid_C", 12, 1275},
    {"Raven_C", 5, 463},         {"Raven_C", 9, 885},         {"Ram_C", 5, 845},           {"Ram_C", 9, 1683},
    {"Deer_C", 6, 764},          {"Deer_C", 10, 1549},        {"Skeleton_C", 4, 309},      {"Skeleton_C", 5, 485},
    {"Bandit_Raider_Dunir_C", 6, 719}, {"Bandit_Raider_Dunir_C", 10, 1436}, {"Bandit_Healer_Dunir_C", 6, 666}, {"Bandit_Arsonist_Dunir_C", 9, 750},
    {"Minotaur_Berserker_C", 5, 1122}, {"Minotaur_Berserker_C", 12, 3312}, {"AlphaWolf_C", 6, 1105}, {"Bear_C", 11, 5997},
    {"RockElemental_C", 9, 8434}, {"Bandit_Raider3_Dunir_C", 13, 18327}, {"Guard_Soldier_C", 50, 13539725}, {"Guard_Captain_C", 50, 22629358},
};

void test_npc_stats(Fixture& f) {
    std::printf("NPC stats\n");
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    size_t within1 = 0, within5 = 0, total = 0;
    for (const Golden& g : kGolden) {
        const auto bp = f.blueprint(g.blueprint);
        CHECKF(bp.has_value(), "the Blueprint table has %s", g.blueprint);
        if (!bp) continue;
        std::string err;
        auto cs = book.make(*bp, g.level, err);
        CHECKF(cs != nullptr, "%s at level %d: %s", g.blueprint, g.level, err.c_str());
        if (!cs) continue;
        const stats::StatValue* hp = cs->find(content::kGuidMaxHealth);
        const stats::StatValue* cur = cs->find(content::kGuidHealth);
        CHECKF(hp && cur && hp->value > 0 && cur->value == hp->value, "%s at level %d has full Health", g.blueprint, g.level);
        if (!hp) continue;
        ++total;
        const double rel = std::fabs(hp->value - g.max_health) / g.max_health;
        // Within 1 % for every golden, elites and guards included: the E1 review found the cause of the earlier 5-19 % (rank 0), 25-40 % (elites) and
        // several-fold (guards) differences in the engine, not the data: an initializer entry that reads GetStatInitRunningTotal() sees the stat's value
        // so far with the Base folded in (statprog/engine.go), so the elite scalar's `total * f` scales Constitution's part of MaxHealth too.
        if (cs->npc().elite_rank > 0 || g.level > 40) ++within5;
        CHECKF(rel <= 0.01, "%s at level %d: MaxHealth %.1f is within 1 %% of retail's %.0f (%.3f)", g.blueprint, g.level, hp->value, g.max_health, hp->value / g.max_health);
        if (rel <= 0.01) ++within1;
    }
    std::printf("  %zu of %zu within 1 %% of the retail MaxHealth (%zu elites and guards among them)\n", within1, total, within5);
    CHECK(within1 == total && total == 29 && within5 >= 7, "all 29 goldens, elites and guards among them, within 1 %");
}
// [CoA - C_Y_B]

// What the captured opens say: an NPC's level (UNPCInfo.Level) and the EveryoneProxy stat items it was opened with.
void test_captured_npcs(Fixture& f) {
    std::printf("the captured NPCs' stats\n");
    std::string err;
    const auto captured = spawns::CapturedData::load(err);
    CHECKF(captured.has_value(), "the captured data loads: %s", err.c_str());
    if (!captured) return;
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    size_t n = 0, hp_ok = 0, mp_ok = 0, ms_ok = 0;
    for (const spawns::OpenTemplate& o : captured->opens()) {
        if (o.category != "npc") continue;
        const spawns::ParsedBlock* info = nullptr;
        const spawns::ParsedBlock* st = nullptr;
        const spawns::ParsedBlock* ci = nullptr;
        for (const spawns::ParsedBlock& b : o.blocks) {
            if (b.cls == "UNPCCombatInfo") ci = &b;
            if (b.cls == "UNPCInfo") info = &b;
            if (b.cls == "UAoCStatsComponent") st = &b;
        }
        if (!info || !st || !st->schema) continue;
        const aoc::protocol::repl::Value* lv = info->state.state.get(*info->schema, "Level");
        const int64_t* level = lv ? lv->get_if<int64_t>() : nullptr;
        const auto* arr = const_cast<aoc::protocol::repl::ObjectState&>(st->state).arrays.get(*st->schema, "StatRepInt32EveryoneProxy");
        if (!level || !arr) continue;
        std::map<int64_t, float> cap;
        for (const aoc::protocol::repl::FastArrayItem& it : arr->items()) {
            const auto v = it.values.values.find(1);
            const auto* s = v == it.values.values.end() ? nullptr : v->second.get_if<aoc::protocol::repl::StatInt32Inst>();
            if (s) cap[s->record] = std::bit_cast<float>(s->value);
        }
        const auto bp = f.motion.find(o.archetype_path);
        if (!bp || !cap.count(content::kGuidMaxHealth)) continue;
        auto cs = book.make(*bp, static_cast<int>(*level), err);
        if (!cs) continue;
        ++n;
        int64_t captured_rank = -1;
        if (ci && ci->schema) {
            const aoc::protocol::repl::Value* ev = ci->state.state.get(*ci->schema, "EliteRank");
            if (ev && ev->get_if<int64_t>()) captured_rank = *ev->get_if<int64_t>();
        }
        const double hp = cs->find(content::kGuidMaxHealth)->value;
        const stats::StatValue* mp = cs->find(f.data->ids().max_mana);
        const stats::StatValue* ms = cs->find(f.data->ids().move_speed_mult);
        const double want_hp = cap[content::kGuidMaxHealth];
        const double want_mp = cap.count(f.data->ids().max_mana) ? cap[f.data->ids().max_mana] : -1;
        const double want_ms = cap.count(f.data->ids().move_speed_mult) ? cap[f.data->ids().move_speed_mult] : -1;
        const bool hp_within = std::fabs(hp - want_hp) <= 0.01 * want_hp;
        const bool mp_within = mp && want_mp >= 0 && std::fabs(mp->value - want_mp) <= 0.01 * std::max(1.0, want_mp);
        // the elite scalar's MoveSpeedMult is `GetStatInitRunningTotal() * (1.05 + 0.1 * rank)` over the Base's 1: 1.05, 1.15, 1.25, 1.35 by rank in every
        // retail open (161 / 65 / 28 / 29 NPCs); the engine gave every NPC 1.0 before the review's fix. The Guard overtuned list has no such entry: 1.0.
        const bool guard = bp->list2 == combat::kGuidGuardOvertunedList;
        const double rule_ms = guard ? 1.0 : 1.05 + 0.1 * static_cast<double>(cs->npc().elite_rank);
        const bool ms_within = ms && want_ms >= 0 && std::fabs(ms->value - want_ms) <= 1e-4 && std::fabs(want_ms - rule_ms) <= 1e-4;
        if (hp_within) ++hp_ok;
        if (mp_within) ++mp_ok;
        if (ms_within) ++ms_ok;
        std::printf("    %-40s L%-3lld MaxHealth %9.1f / %9.1f (%.3f)  MaxMana %9.1f / %9.1f  MoveSpeedMult %.2f / %.2f  dist %d,%d,%d,%d,%d,%d,%d,%d elite %s rank %lld\n",
                    spawns::blueprint_leaf(o.archetype_path).c_str(), static_cast<long long>(*level), hp, want_hp, hp / want_hp, mp ? mp->value : -1.0, want_mp, ms ? ms->value : -1.0, want_ms, bp->dist[0],
                    bp->dist[1], bp->dist[2], bp->dist[3], bp->dist[4], bp->dist[5], bp->dist[6], bp->dist[7],
                    bp->elite.empty() ? "-" : (std::to_string(bp->elite[0].first) + ":" + std::to_string(bp->elite[0].second)).c_str(), static_cast<long long>(captured_rank));
        CHECKF(hp_within && (want_mp < 0 || mp_within) && (want_ms < 0 || ms_within), "%s at level %lld: MaxHealth, MaxMana and MoveSpeedMult are the captured open's",
               spawns::blueprint_leaf(o.archetype_path).c_str(), static_cast<long long>(*level));
    }
    std::printf("  %zu captured NPCs: MaxHealth within 1 %% for %zu, MaxMana for %zu, MoveSpeedMult 1.05 + 0.1 x rank for %zu\n", n, hp_ok, mp_ok, ms_ok);
    CHECK(n >= 20 && hp_ok == n, "every captured NPC's MaxHealth is the engine's, within 1 %");
}

#include "combat_tests.inc"
#include "combat_tests2.inc"

}  // namespace

int main() {
    if (!real_db_present()) {
        std::printf("test_worldd_combat: no content.db at %s (set AOC_CONTENT_DB): skipped\n", real_db_path().c_str());
        return 0;
    }
    Fixture f;
    if (!f.ok()) {
        std::printf("  [FAIL] fixture: %s\n", f.problem.c_str());
        return 1;
    }
    test_data(f);
    test_npc_stats(f);
    test_captured_npcs(f);
    test_aspect();
    test_hit_rules(f);
    test_fight(f);
    test_hostile(f);
    test_player_cast(f);
    test_npc_hits_player(f);
    test_combat_flag(f);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
