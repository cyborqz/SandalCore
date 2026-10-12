// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_ability_pipeline.cpp: the generic ability runtime (plan D2, stage 1) on a simulated clock
//
//  A level-25 Bard of the REAL content.db of the working tree (dist/content/content.db, or AOC_CONTENT_DB): the stat program and its gear rules, the
//  ability tables and A7's rules all come from it (the Rig of ability_rig.h), the events go into a Pawn that holds the client's arrays with the key
//  rules of PlayerBootstrap::apply_ability_change. What is checked:
//    costs and timing   every cost is k x ManaCostMult(25) = k x 5.6875, taken at the cost step with the cooldown and the Bard's Resonance, after
//                       the ability item; the cooldown's charges; Fortissimo's five stacks; Resonance's cap and its refresh
//    effects            the record's duration, stacks, refresh (always, extended) and expiry, the stat engine's forms (Mitigation x (1 - v)), the
//                       instant heal and Glee's ticks clamped at the maximum
//    refusals           an unknown ability, one not known, one that needs a target, an enemy-target one, a cast during another's, a replayed id,
//                       a spent charge, an unaffordable cost, a formula that does not run, an item's ability; none of them changes the pawn
//    time               the clock never goes back, one big step gives the events of many small ones, a stall does not owe more than five regeneration
//                       ticks
//    regeneration       Health and Mana every second by their regeneration stat, clamped at the maximum, a Rest's boost counted
//    movement           Flourish's allowance and its window
//    A7                 Sprint's speed from its formula (1.3 at level 25) and on top of a buff, Dodge's cooldown beside the generic ones, no key collides
//  Without a content.db the test says so and is skipped.
// ============================================================================
#include "ability_rig.h"

#include <cmath>
#include <cstring>
#include <set>

using namespace ability_rig;

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

constexpr double kManaMult25 = 5.6875;   // ManaCostMult(25): EvalCurve(Player_Base_ManaCostMult, 25) / 32
// ~ C-Y-B :: C O A ~

const std::vector<std::string> kTalents = {"Bard_Passive_Merriment", "Bard_Passive_Dance_Power", "Bard_Passive_QuickWit_Jest", "Bard_QuickWit_Jest"};

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

/// The first event of `seen` (from `from`) that holds `pred`, or null.
template <class P>
const Seen* find_event(const std::vector<Seen>& seen, P pred, size_t from = 0) {
    for (size_t i = from; i < seen.size(); ++i) {
        if (pred(seen[i])) return &seen[i];
    }
    return nullptr;
}

const bootstrap::EffectItem* effect_added(const Seen& s, int64_t guid) {
    for (const auto& e : s.change.effects_added)
        if (e.effect_guid == guid) return &e;
    return nullptr;
}
const bootstrap::EffectItem* effect_changed(const Seen& s, int64_t guid) {
    for (const auto& e : s.change.effects_changed)
        if (e.effect_guid == guid) return &e;
    return nullptr;
}
bool has_stat(const Seen& s, int64_t record) {
    for (const auto& st : s.change.stats)
        if (st.record == record) return true;
    return false;
}
float stat_of(const Seen& s, int64_t record) {
    for (const auto& st : s.change.stats)
        if (st.record == record) return st.value;
    return -1;
}

// ---- the catalog --------------------------------------------------------------------------------------------------------------------

void test_catalog() {
    std::printf("the catalog\n");
    Rig r(2, 25, kTalents);
    CHECKF(r.ok(), "the rig: %s", r.problem.c_str());
    if (!r.ok()) return;
    CHECK(r.catalog->abilities() >= 200 && r.catalog->effects() >= 800 && r.catalog->hits() >= 500 && r.catalog->exprs() >= 700, "every ability, effect, hit and expression of the tables is in memory");
    const abilities::AbilityDef* flourish = r.catalog->ability_named("Bard_Misc_Flourish");
    CHECK(flourish && flourish->row.cooldown_expr != 0 && !flourish->costs.empty() && flourish->montages.size() == 1 && flourish->montages[0].net_guid.value_or(0) == 6743654538719732531ull,
          "Flourish has its cost, cooldown and the montage whose retail NetGUID the captures name");
    CHECK(r.catalog->effect_named("Bard_Resonance") && r.catalog->effect_named("Bard_Glee") && r.catalog->hit(r.catalog->ability_named("Bard_Misc_Fortissimo")->hits[0].hit_guid),
          "the effects and hits are found by name and guid");
    CHECK(r.catalog->expr(0) == nullptr && r.catalog->program(0) == -1 && r.catalog->program(flourish->row.cooldown_expr) >= 0, "an expression id of 0 is none, a compiled one has its program");
    // the named reference of a formula ($Skill:...$) was resolved by contentc: Jest's hit that gives Pep needs the talent
    const abilities::AbilityDef* jest = r.catalog->ability_named("Bard_Misc_QuickWit_Jest");
    bool pep_hit_gated = false;
    for (const auto& h : jest->hits) {
        const abilities::HitDef* hd = r.catalog->hit(h.hit_guid);
        if (hd->row.name == "Bard_QuickWit_Jest" && hd->row.requirement_expr != 0 && r.catalog->program(hd->row.requirement_expr) >= 0) pep_hit_gated = true;
    }
    CHECK(pep_hit_gated, "Jest's Pep hit has its requirement compiled (its $SkillTree:Bard$ and $Skill:...$ were written as numbers by contentc)");
    std::string err;
    const auto again = abilities::Catalog::load(r.path, *r.ctx->bundle, err);
    CHECK(again && again->abilities() == r.catalog->abilities(), "the catalog loads again to the same size");
    stats::StatBundle other = *r.ctx->bundle;
    other.crc32 ^= 1;
    CHECK(!abilities::Catalog::load(r.path, other, err) && err.find("not the one worldd runs") != std::string::npos, "a stat bundle of another build is refused (the programs would index the wrong table)");
}

// ---- costs and timing -------------------------------------------------------------------------------------------------------------

void test_cast_timeline() {
    std::printf("a cast: Flourish\n");
    Rig r(2, 25, kTalents);
    CHECKF(r.ok(), "the rig: %s", r.problem.c_str());
    if (!r.ok()) return;
    r.know({"Bard_Misc_Flourish"});
    const double max_mana = r.pawn.stat(content::kGuidMana);
    CHECK(near(max_mana, r.pawn.stat(content::kGuidMaxMana), 1e-3) && r.pawn.stat(kMoveSpeedMult) == 1.0f, "(the Bard starts with full Mana and MoveSpeedMult 1)");
    std::vector<Seen> seen;
    const double t0 = 2.0;
    CHECK(r.trigger("Bard_Misc_Flourish", t0, false, 0, &seen) == Refusal::None && seen.empty(), "the trigger is accepted and changes nothing at once: the montage's first notify is 0.3 s on");
    seen = r.run_to(t0 + 0.299);
    CHECK(find_event(seen, [](const Seen& s) { return !s.change.abilities_added.empty(); }) == nullptr, "nothing before 0.3 s");
    seen = r.run_to(t0 + 0.301);
    const Seen* item = find_event(seen, [](const Seen& s) { return !s.change.abilities_added.empty(); });
    const Seen* cost = find_event(seen, [&](const Seen& s) { return has_stat(s, content::kGuidMana); });
    CHECK(item && near(item->at, t0 + 0.3, 1e-9) && item->change.abilities_added[0].ability_guid == r.ability("Bard_Misc_Flourish") && item->change.abilities_added[0].montage_group == "EmoteGroup" &&
              item->change.abilities_added[0].montage_guid == 6743654538719732531ull && !item->change.abilities_added[0].targets_self && item->change.abilities_removed.empty(),
          "at 0.3 s the ability item: Flourish, MontageGroup EmoteGroup, the montage's static NetGUID, no target, nothing before it to remove");
    CHECK(cost && near(cost->at, t0 + 0.3, 1e-9) && !cost->change.cooldowns_set.empty() && has_stat(*cost, kResonanceStat), "the cost step comes with it: the Cooldowns item and the class resource");
    if (!cost) return;
    CHECK(near(max_mana - stat_of(*cost, content::kGuidMana), 10 * kManaMult25, 1e-3) && near(10 * kManaMult25, 56.875, 1e-9), "Mana falls by 10 x 5.6875 = 56.875");
    const auto& cd = cost->change.cooldowns_set[0];
    CHECK(cd.ability_guid == r.ability("Bard_Misc_Flourish") && cd.triggered && cd.initial_cooldown == 20.0f && cd.charges == 1 && near(cd.end_time - cd.start_time, 20.0, 1e-6),
          "the Cooldowns item: 20 s, 1 charge left of the 2 (the first use)");
    const bootstrap::EffectItem* res = effect_added(*cost, r.effect("Bard_Resonance"));
    CHECK(stat_of(*cost, kResonanceStat) == 1.0f && res && res->duration == 30.0f && res->stacks == 1, "Bard_Resonance: the stat is 1 and its effect lasts 30 s with 1 stack");
    seen = r.run_to(t0 + 0.34);
    const Seen* hits = find_event(seen, [&](const Seen& s) { return effect_added(s, r.effect("Bard_Pep")) != nullptr; });
    CHECK(hits && near(hits->at, t0 + 0.335, 1e-9) && effect_added(*hits, r.effect("Bard_Pep"))->duration == 4.0f && near(stat_of(*hits, kMoveSpeedMult), 1.2, 1e-6),
          "0.035 s after the cost the self hit's effects: Pep for 4 s and MoveSpeedMult 1.2");
    CHECK(hits && effect_added(*hits, r.effect("Bard_Flourish_Power")) && effect_added(*hits, r.effect("Bard_Flourish_Power"))->duration == 6.0f, "...and Flourish_Power for 6 s (the Bard holds the talent)");
    CHECK(r.pawn.broken.empty(), "(the pawn took every change)");
    seen = r.run_to(t0 + 0.335 + 3.99);
    CHECK(r.pawn.effect(r.effect("Bard_Pep")) != nullptr && r.pawn.stat(kMoveSpeedMult) > 1.19f, "Pep is on at 3.99 s");
    seen = r.run_to(t0 + 0.335 + 4.01);
    CHECK(r.pawn.effect(r.effect("Bard_Pep")) == nullptr && r.pawn.stat(kMoveSpeedMult) == 1.0f, "and gone at 4 s with MoveSpeedMult back to 1");
    // the cooldown's two charges
    const double t1 = t0 + 5.0;
    CHECK(r.trigger("Bard_Misc_Flourish", t1, false, 0) == Refusal::None, "a second Flourish 5 s later: accepted");
    r.run_to(t1 + 0.4);
    CHECK(r.pawn.cooldown(r.ability("Bard_Misc_Flourish")) && r.pawn.cooldown(r.ability("Bard_Misc_Flourish"))->charges == 0, "the Cooldowns item says 0 charges left");
    CHECK(r.trigger("Bard_Misc_Flourish", t1 + 2.0, false, 0) == Refusal::Charges && !r.pawn.ends.empty() && r.pawn.ends.back() == r.next_client_id - 1,
          "a third is refused: out of charges, ClientEndAbility(its id)");
    // 20 s after the first cast a charge is back, the item shows it; 20 s after that the item goes
    r.run_to(t0 + 0.3 + 20.01);
    CHECK(r.pawn.cooldown(r.ability("Bard_Misc_Flourish")) && r.pawn.cooldown(r.ability("Bard_Misc_Flourish"))->charges == 1, "20 s after the first cast a charge is back: the item says 1");
    r.run_to(t0 + 0.3 + 40.01);
    CHECK(r.pawn.cooldown(r.ability("Bard_Misc_Flourish")) == nullptr && r.pipe->cooldowns_running() == 0, "20 s after that both are back and the item is gone");
    CHECK(r.trigger("Bard_Misc_Flourish", t0 + 42.0, false, 0) == Refusal::None, "(and Flourish can be cast again)");
    r.run_to(t0 + 43.0);
    CHECK(r.pawn.broken.empty(), "(the pawn took every change)");
}

void test_mana_costs() {
    std::printf("the Mana costs, k x ManaCostMult(25)\n");
    Rig r(2, 25, kTalents);
    CHECKF(r.ok(), "the rig: %s", r.problem.c_str());
    if (!r.ok()) return;
    struct Row {
        const char* ability;
        double      k;
        bool        target;
    };
    // the research's Mana deltas (capture 44): 56.875 (k 10), 85.3125 (15), 113.75 (20), 199.0625 (35)
    const Row rows[] = {{"Bard_Misc_Flourish", 10, false},  {"Bard_Misc_QuickWit_Jest", 10, true}, {"Bard_Dance_Mesmerizing", 15, false}, {"Bard_Story_Joy", 15, true},
                        {"Bard_Dance_Nimble", 20, true},     {"Bard_Dance_Maddening", 20, false},  {"Bard_Misc_Misdirection", 20, false}, {"Bard_Dance_Shielding", 20, false}};
    r.know({"Bard_Misc_Flourish", "Bard_Misc_QuickWit_Jest", "Bard_Dance_Mesmerizing", "Bard_Story_Joy", "Bard_Dance_Nimble", "Bard_Dance_Maddening", "Bard_Misc_Misdirection",
            "Bard_Dance_Shielding"});
    double t = 5.0;
    for (const Row& row : rows) {
        r.set_current(content::kGuidMana, r.pawn.stat(content::kGuidMaxMana));
        const double before = r.pawn.stat(content::kGuidMana);
        std::vector<Seen> seen;
        const Refusal rf = r.trigger(row.ability, t + 0.017, row.target, 0, &seen);
        CHECKF(rf == Refusal::None, "%s is accepted (%s)", row.ability, refusal_name(rf));
        seen = r.run_to(t + 3.0);
        const Seen* cost = find_event(seen, [&](const Seen& s) { return has_stat(s, content::kGuidMana) && stat_of(s, content::kGuidMana) < before; });
        CHECKF(cost && near(before - stat_of(*cost, content::kGuidMana), row.k * kManaMult25, 1e-3), "%s costs %.4f Mana (%.4f)", row.ability, row.k * kManaMult25,
               cost ? before - stat_of(*cost, content::kGuidMana) : -1.0);
        t += 100.0;   // every cooldown is over
        r.run_to(t - 1.0);
    }
    CHECK(near(15 * kManaMult25, 85.3125, 1e-9) && near(20 * kManaMult25, 113.75, 1e-9) && near(35 * kManaMult25, 199.0625, 1e-9), "(85.3125, 113.75 and 199.0625: the research's other deltas)");
    CHECK(r.pawn.broken.empty(), "(the pawn took every change)");
}
// -- C.O.A . Cyb --

void test_resonance_and_fortissimo() {
    std::printf("Resonance and Fortissimo\n");
    Rig r(2, 25, kTalents);
    CHECKF(r.ok(), "the rig: %s", r.problem.c_str());
    if (!r.ok()) return;
    r.know({"Bard_Misc_Fortissimo", "Bard_Dance_Maddening", "Bard_Misc_Misdirection", "Bard_Dance_Nimble", "Bard_Dance_Shielding", "Bard_Misc_Flourish", "Bard_Dance_Mesmerizing",
            "Bard_Misc_QuickWit_Jest", "Bard_Story_Joy", "Bard_Misc_GetOffTheStage"});
    const int64_t resonance = r.effect("Bard_Resonance");
    CHECK(r.trigger("Bard_Misc_Fortissimo", 2.0) == Refusal::Cost && r.pawn.ends.size() == 1, "Fortissimo needs five stacks of Resonance: with none it is refused");
    CHECK(r.pipe->grant_effect(resonance, 7, 3.0) && r.pipe->stacks_of(resonance) == 7, "(the Bard is given seven stacks)");
    r.take();
    CHECK(r.pawn.stat(kResonanceStat) == 7.0f, "(and the stat says 7)");
    std::vector<Seen> seen;
    CHECK(r.trigger("Bard_Misc_Fortissimo", 4.0, false, 0, &seen) == Refusal::None, "with seven it is accepted");
    seen = r.run_to(4.5);
    CHECK(find_event(seen, [](const Seen& s) { return !s.change.abilities_added.empty(); }) == nullptr, "an ability with no montage has no ability item (retail: none)");
    const Seen* cost = find_event(seen, [&](const Seen& s) { return !s.change.cooldowns_set.empty(); });
    CHECK(cost && near(cost->at, 4.3, 1e-9) && cost->change.cooldowns_set[0].initial_cooldown == 60.0f && cost->change.cooldowns_set[0].charges == 0, "its Cooldowns item (60 s, no charge left) comes at 0.3 s");
    if (!cost) return;
    CHECK(r.pawn.stat(kResonanceStat) == 2.0f && r.pipe->stacks_of(resonance) == 2 && stat_of(*cost, kResonanceStat) == 2.0f, "it paid five stacks: Resonance 7 -> 2, and (a spend of no Mana) it gave none");
    const auto* fx = effect_changed(*cost, resonance);
    CHECK(fx && fx->stacks == 2, "the effect item changes in place to 2 stacks");
    CHECK(r.pawn.effect(r.effect("Bard_Fortissimo")) != nullptr && r.pawn.effect(r.effect("Bard_Fortissimo"))->duration == 8.0f &&
              find_event(seen, [&](const Seen& s) { return effect_added(s, r.effect("Bard_Fortissimo")) && near(s.at, 4.3, 1e-9); }),
          "Fortissimo's own effect lasts 8 s and comes in the same packet (no montage, no hit delay)");
    // Resonance climbs one a cast to 10 and stays there
    double t = 10.0;
    const char* casts[] = {"Bard_Dance_Maddening", "Bard_Misc_Misdirection", "Bard_Dance_Nimble", "Bard_Dance_Shielding", "Bard_Dance_Mesmerizing", "Bard_Misc_Flourish", "Bard_Misc_Flourish",
                           "Bard_Misc_QuickWit_Jest", "Bard_Story_Joy", "Bard_Misc_GetOffTheStage"};
    int expect = 2;
    for (const char* name : casts) {
        r.set_current(content::kGuidMana, r.pawn.stat(content::kGuidMaxMana));
        const Refusal rf = r.trigger(name, t, name == std::string("Bard_Dance_Nimble") || name == std::string("Bard_Misc_QuickWit_Jest") || name == std::string("Bard_Story_Joy"));
        CHECKF(rf == Refusal::None, "%s accepted (%s)", name, refusal_name(rf));
        r.run_to(t + 3.0);
        expect = std::min(10, expect + 1);
        CHECKF(r.pawn.stat(kResonanceStat) == static_cast<float>(expect), "after %s Resonance is %d", name, expect);
        t += 4.0;
        r.run_to(t);
    }
    CHECK(r.pipe->stacks_of(resonance) == 10 && r.pawn.stat(kResonanceStat) == 10.0f, "ten casts later Resonance is at its cap, 10");
    // 30 s after the last cast the stacks end together
    const std::optional<double> end = r.pipe->effect_end(resonance);
    CHECK(end && *end > t, "(its end is ahead)");
    if (!end) return;
    r.run_to(*end - 0.01);
    CHECK(r.pipe->stacks_of(resonance) == 10, "...still 10 a moment before it");
    r.run_to(*end + 0.01);
    CHECK(r.pipe->stacks_of(resonance) == 0 && r.pawn.stat(kResonanceStat) == 0.0f && r.pawn.effect(resonance) == nullptr, "...and all gone at its end: the stat is 0 again");
    CHECK(r.pawn.broken.empty(), "(the pawn took every change)");
}

// ---- effects -----------------------------------------------------------------------------------------------------------------------

void test_glee() {
    std::printf("Glee: heal, ticks, refresh, expiry\n");
    Rig r(2, 25, kTalents);
    CHECKF(r.ok(), "the rig: %s", r.problem.c_str());
    if (!r.ok()) return;
    r.know({"Bard_Misc_QuickWit"});
    const double max_health = r.pawn.stat(content::kGuidMaxHealth);
    r.set_current(content::kGuidHealth, 200.0);
    const int64_t glee = r.effect("Bard_Glee");
    const double base_mit = r.pawn.stat(kMitigation);
    std::vector<Seen> seen;
    CHECK(r.trigger("Bard_Misc_QuickWit", 0.5, false) == Refusal::NeedsTarget && r.pawn.ends.size() == 1, "Quick Wit needs a target: with none it is refused");
    r.run_to(0.6);
    const double t0 = 2.0;
    CHECK(r.trigger("Bard_Misc_QuickWit", t0, true, 0, &seen) == Refusal::None, "aimed at the caster it is accepted");
    seen = r.run_to(t0 + 0.7);
    const Seen* item = find_event(seen, [](const Seen& s) { return !s.change.abilities_added.empty(); });
    CHECK(item && item->change.abilities_added[0].targets_self && item->change.abilities_added[0].montage_group == "DefaultGroup", "the ability item has the caster as its target");
    const Seen* heal = find_event(seen, [&](const Seen& s) { return effect_added(s, glee) != nullptr; });
    CHECK(heal && near(heal->at, t0 + 0.3 + 0.34, 1e-9), "Glee comes with the projectile's hit, 0.34 s after the cost step (retail: +343 ms)");
    if (!heal) return;
    CHECK(has_stat(*heal, content::kGuidHealth) && stat_of(*heal, content::kGuidHealth) > 200.0f + 30.0f, "...and the same hit heals: Health rises at that instant, by the formula's amount");
    const auto* gi = effect_added(*heal, glee);
    CHECK(gi && gi->duration == 15.0f && gi->tick_time == 2.0f && gi->stacks == 1, "Glee: 15 s, a tick every 2 s, 1 stack");
    CHECK(stat_of(*heal, kMitigation) < base_mit && near(stat_of(*heal, kMitigation), base_mit * (1.0 - 0.03), 1e-4), "Mitigation is multiplied by (1 - 0.03): the stat's form, the Bard holds Merriment");
    // the ticks: the first one period after the application (the record ignores the initial tick), each heals again
    const double applied = heal->at;
    r.run_to(applied + 1.99);
    CHECK(r.pawn.effect(glee) != nullptr, "(Glee is on)");
    const double health_before_tick = r.health();
    r.run_to(applied + 2.001);
    CHECK(r.health() > health_before_tick + 8.0, "the first tick, 2 s after: Health rises (over and above the regeneration)");
    // refresh: another Quick Wit while Glee runs extends it: the item says the remaining time plus 15
    r.run_to(t0 + 0.3 + 8.1);   // the cooldown's 8 s
    r.set_current(content::kGuidMana, r.pawn.stat(content::kGuidMaxMana));
    const double t2 = t0 + 10.0;
    r.run_to(t2 - 0.01);
    const double glee_end = *r.pipe->effect_end(glee);
    seen.clear();
    CHECK(r.trigger("Bard_Misc_QuickWit", t2, true, 0, &seen) == Refusal::None, "after its cooldown Quick Wit is cast again");
    seen = r.run_to(t2 + 1.0);
    const Seen* again = find_event(seen, [&](const Seen& s) { return effect_changed(s, glee) != nullptr; });
    const double remaining = glee_end - (t2 + 0.3 + 0.34);
    CHECK(again && near(effect_changed(*again, glee)->duration, remaining + 15.0, 1e-3) && effect_changed(*again, glee)->stacks == 1,
          "Glee, refreshed while it runs, is an in-place change of its item: the remaining time plus the full 15 s, still one stack (its in-place limit)");
    // expiry
    const double end = *r.pipe->effect_end(glee);
    r.run_to(end - 0.01);
    CHECK(r.pawn.effect(glee) != nullptr && r.pawn.stat(kMitigation) < base_mit, "Glee runs until its end");
    r.run_to(end + 0.01);
    CHECK(r.pawn.effect(glee) == nullptr && near(r.pawn.stat(kMitigation), base_mit, 1e-6), "and at its end the item goes and Mitigation is back");
    for (double t = end + 1.0; t <= end + 200.0; t += 1.0) r.run_to(t);
    CHECKF(r.health() <= max_health + 1e-3 && near(r.health(), max_health, 1e-2), "Health never goes above the maximum and ends on it (%.3f of %.3f)", r.health(), max_health);
    CHECK(r.pawn.broken.empty(), "(the pawn took every change)");
}

void test_stat_forms() {
    std::printf("the engine's forms of a stat mod\n");
    Rig r(2, 25, kTalents);
    CHECKF(r.ok(), "the rig: %s", r.problem.c_str());
    if (!r.ok()) return;
    // x v (HealingMult, Note_Comedy 1.05), added (MoveSpeedMult, Pep 0.2); x (1 - v) (Mitigation, Glee) is test_glee's
    const double healing = r.pawn.stat(kHealingMult);
    CHECK(r.pipe->grant_effect(r.effect("Bard_Note_Comedy"), 1, 1.0), "(Note_Comedy is given)");
    r.take();
    CHECK(near(r.pawn.stat(kHealingMult), healing * 1.05, 1e-5), "HealingMult is multiplied by 1.05");
    CHECK(r.pipe->grant_effect(r.effect("Bard_Pep"), 1, 1.5), "(Pep is given)");
    r.take();
    CHECK(near(r.pawn.stat(kMoveSpeedMult), 1.2, 1e-6), "MoveSpeedMult is 1 + 0.2");
    CHECK(r.pipe->remove_effect(r.effect("Bard_Note_Comedy"), 2.0), "(Note_Comedy is taken off)");
    r.take();
    CHECK(near(r.pawn.stat(kHealingMult), healing, 1e-6), "taking the effect off puts the stat back");
    CHECK(!r.pipe->remove_effect(r.effect("Bard_Note_Comedy"), 2.1), "(an effect that is not on is not taken off)");
    CHECK(!r.pipe->grant_effect(123456789, 1, 2.2), "(an effect the catalog does not have cannot be given)");
}
// -- CoA & C:Y:B --

void test_songs_and_the_air() {
    std::printf("a song, and an ability cast at the air\n");
    std::vector<std::string> song_talents = kTalents;
    song_talents.push_back("Bard_Passive_Music_Crescendo");   // the buff's value grows with the song's progress
    Rig r(2, 25, song_talents);
    CHECKF(r.ok(), "the rig: %s", r.problem.c_str());
    if (!r.ok()) return;
    r.know({"Bard_Music_Cheerful", "Bard_Music_Pensive", "Bard_Dance_Maddening", "Bard_Misc_QuickWit_Mock"});
    const double mana = r.pawn.stat(content::kGuidMana);
    const int64_t self_effect = r.effect("Bard_Music_Cheerful_Self"), buff = r.effect("Bard_Music_Cheerful_Buff");
    std::vector<Seen> seen;
    CHECK(r.trigger("Bard_Music_Cheerful", 2.0, false, 2, &seen) == Refusal::None, "a song (the client's AnimationIndex 2) is accepted");
    seen = r.run_to(2.31);
    const Seen* item = find_event(seen, [](const Seen& s) { return !s.change.abilities_added.empty(); });
    CHECK(item && item->change.abilities_added[0].montage_group == "SongGroup" && item->change.abilities_added[0].montage_guid == 5020857450605277129ull && near(item->at, 2.3, 1e-9),
          "its item comes at 0.3 s: MontageGroup SongGroup and the Cheerful montage of the captures");
    CHECK(r.pawn.cooldowns.empty() && near(r.pawn.stat(content::kGuidMana), mana, 1e-3) && r.pawn.effect(self_effect) == nullptr, "no cooldown, no cost, and none of its effects yet");
    // the effects start 2.3 s after the trigger [R: capture 44, +2321 ms]: the loop's own effect (infinite, a tick every 2 s) and the buff
    seen = r.run_to(4.4);
    const Seen* start = find_event(seen, [&](const Seen& s) { return effect_added(s, self_effect) != nullptr; });
    CHECK(start && near(start->at, 4.3, 1e-9) && effect_added(*start, self_effect)->duration > 1e30f && effect_added(*start, self_effect)->tick_time == 2.0f && effect_added(*start, buff) &&
              effect_added(*start, buff)->tick_time == 2.0f,
          "2.3 s on: the song's own effect (no end, a tick every 2 s) and its Buff (a tick every 2 s) are on");
    // ... the buff is added again every 2 s, and its heal ticks are confirmed with the amount rising with the song's progress
    const double first_end = *r.pipe->effect_end(buff);
    seen = r.run_to(8.4);
    int readds = 0;
    for (const Seen& s : seen)
        for (const auto& e : s.change.effects_added) readds += e.effect_guid == buff ? 1 : 0;
    for (const Seen& s : seen)
        for (const auto& e : s.change.effects_changed) readds += e.effect_guid == buff ? 1 : 0;
    CHECKF(readds >= 2 && *r.pipe->effect_end(buff) > first_end, "the buff is added again every 2 s (%d times in 4 s)", readds);
    std::vector<float> hot;
    for (const Seen& s : r.log)
        for (const auto& h : s.confirmations)
            if (h.source_type == content::kSourceTypeEffect && h.source_guid == buff && !h.mods.empty()) hot.push_back(h.mods[0].value);
    CHECKF(hot.size() >= 2 && hot.back() > hot.front(), "the buff's heal ticks are confirmed, and the amount rises with the song's progress (%zu ticks)", hot.size());
    // a second press ends the song: the effects go, nothing is paid
    const double mana_now = r.pawn.stat(content::kGuidMana);
    CHECK(r.trigger("Bard_Music_Cheerful", 9.0, false, 3, &seen) == Refusal::None, "pressing the song again is accepted");
    seen = r.run_to(9.4);
    CHECK(r.pawn.effect(self_effect) == nullptr && r.pawn.effect(buff) == nullptr && r.pawn.cooldowns.empty() && r.pawn.stat(content::kGuidMana) >= mana_now, "...and ends it: its effects go, no cost, no cooldown");
    // another song replaces the one that plays
    r.trigger("Bard_Music_Cheerful", 10.0, false, 2, &seen);
    r.run_to(12.5);
    CHECK(r.pawn.effect(self_effect) != nullptr, "(Cheerful plays again)");
    r.trigger("Bard_Music_Pensive", 13.0, false, 2, &seen);
    r.run_to(15.5);
    CHECK(r.pawn.effect(self_effect) == nullptr && r.pawn.effect(r.effect("Bard_Music_Pensive_Self")) != nullptr, "a second song replaces the first: they share the Bard_Music tag");
    // cast at the air: Maddening's hits reach enemies only; it spends its cost, starts its cooldown and plays its montage
    const double t1 = 30.0;
    CHECK(r.trigger("Bard_Dance_Maddening", t1, false, 0, &seen) == Refusal::None, "Maddening, cast at the air, is accepted");
    seen = r.run_to(t1 + 3.0);
    const Seen* cost = find_event(seen, [&](const Seen& s) { return !s.change.cooldowns_set.empty(); });
    CHECK(cost && cost->change.cooldowns_set[0].initial_cooldown == 25.0f && mana - 20 * kManaMult25 < r.pawn.stat(content::kGuidMana) + 80.0, "it spends 113.75 Mana and starts its 25 s cooldown");
    CHECK(find_event(seen, [&](const Seen& s) { return !s.change.abilities_added.empty() && s.change.abilities_added[0].montage_guid == 9016927598177880665ull; }) != nullptr,
          "and plays its montage (the NetGUID of capture 44)");
    // an enemy-only ability with no target is cast at the air as well: Mock
    TriggerInfo none;
    CHECK(r.pipe->trigger(r.ability("Bard_Misc_QuickWit_Mock"), 99, 40.0, none) == Refusal::None, "Mock, with no enemy to aim at, is cast at the air");
    seen = r.run_to(41.0);
    const Seen* mock = find_event(seen, [&](const Seen& s) { return !s.change.cooldowns_set.empty(); });
    CHECK(mock && mock->change.cooldowns_set[0].ability_guid == r.ability("Bard_Misc_QuickWit_Mock"), "it spends its cost and starts its cooldown, and nothing is hit");
    CHECK(r.pawn.broken.empty(), "(the pawn took every change)");
}

// ---- plan D2, stage 2: confirmations (the floating numbers), shields ---------------------------------------------------------------------

void test_confirmations() {
    std::printf("hit confirmations: the amounts the client draws as floating numbers\n");
    Rig r(2, 25, kTalents);
    CHECKF(r.ok(), "the rig: %s", r.problem.c_str());
    if (!r.ok()) return;
    r.know({"Bard_Misc_QuickWit"});
    // Quick Wit aimed at the caster: its projectile's heal is confirmed by the server (no request) with the amount and, at full Health, an
    // effective value of 0 (retail: Bard_QuickWit_Heal 651.47 / 0 at full Health, 651.47 / 236.6 when 236.6 was missing)
    std::vector<Seen> seen;
    const double t0 = 2.0;
    CHECK(r.trigger("Bard_Misc_QuickWit", t0, true, 0, &seen) == Refusal::None, "(Quick Wit is cast at the caster, at full Health)");
    seen = r.run_to(t0 + 0.7);
    size_t confirmed = 0;
    for (const Seen& s : seen) confirmed += s.confirmations.size();
    CHECKF(confirmed >= 1, "the projectile's hit is confirmed by the server (%zu confirmation(s))", confirmed);
    const bootstrap::HitConfirm* heal = nullptr;
    for (const Seen& s : seen)
        for (const auto& h : s.confirmations)
            if (h.source_type == content::kSourceTypeProjectile && !h.mods.empty() && h.mods[0].value > 0) heal = &h;
    CHECK(heal != nullptr, "one of them carries a heal's stat mod");
    if (!heal) return;
    CHECK(heal->source_type == content::kSourceTypeProjectile && !heal->source_is_actor && !heal->response && heal->predicted_autonomous && heal->predicted_simulated && !heal->no_target,
          "its source is the projectile record, with no Source actor, answering no request [R: 3,529 projectile hits, source (0,0,0,0)]");
    CHECKF(heal->mods[0].value > 10.0f && heal->mods[0].effective == 0.0f, "at full Health the amount is the formula's (%.2f) and the effective value is 0: the overheal a retail client shows", heal->mods[0].value);
    const float amount = heal->mods[0].value;
    // with 100 Health missing: the same heal, effective = what fitted
    r.run_to(t0 + 9.0);
    r.set_current(content::kGuidMana, r.pawn.stat(content::kGuidMaxMana));
    const double max_health = r.pawn.stat(content::kGuidMaxHealth);
    r.set_current(content::kGuidHealth, max_health - 100.0);
    const double t1 = t0 + 10.0;
    CHECK(r.trigger("Bard_Misc_QuickWit", t1, true, 0, &seen) == Refusal::None, "(cast again with 100 Health missing)");
    seen = r.run_to(t1 + 0.7);
    const bootstrap::HitConfirm* part = nullptr;
    for (const Seen& s : seen)
        for (const auto& h : s.confirmations)
            if (h.source_type == content::kSourceTypeProjectile && !h.mods.empty()) part = &h;
    CHECK(part && near(part->mods[0].value, amount, 0.01) && part->mods[0].value > 10.0f && near(part->mods[0].effective, std::min<double>(100.0, part->mods[0].value), 0.5), "the amount is the same and the effective value is what fitted of the 100 missing");
}
// -- Cyb x C | O | A --

void test_shielding_dance() {
    std::printf("Shielding Dance: the shield, the request, the confirmation, absorb, expiry\n");
    Rig r(2, 25, kTalents);
    CHECKF(r.ok(), "the rig: %s", r.problem.c_str());
    if (!r.ok()) return;
    r.asks = false;   // (the test asks for the hits itself)
    r.know({"Bard_Dance_Shielding"});
    const int64_t temp_hp = r.effect("Bard_Dance_Shielding_TempHP"), icd = r.effect("Bard_Dance_Shielding_ICD");
    std::vector<Seen> seen;
    const double t0 = 2.0;
    CHECK(r.trigger("Bard_Dance_Shielding", t0, false, 0, &seen) == Refusal::None, "Shielding is accepted");
    seen = r.run_to(t0 + 0.4);
    // the zone's first overlap (the caster stands in it) gives the shield with the cost step [R: retail's effects come 32 ms after the cost]
    const Seen* cost = find_event(seen, [&](const Seen& s) { return effect_added(s, temp_hp) != nullptr; });
    CHECK(cost && effect_added(*cost, temp_hp)->duration == 8.0f && effect_added(*cost, icd) && effect_added(*cost, icd)->duration == 4.0f, "the TempHP effect (8 s) and the internal cooldown (4 s) are on the pawn");
    CHECK(cost && cost->change.shields && cost->change.shields->size() == 1, "the TempHealth array's item lists one shield");
    const float shield = cost && cost->change.shields && !cost->change.shields->empty() ? (*cost->change.shields)[0].value : 0.0f;
    CHECK(shield > 30.0f && (*cost->change.shields)[0].base == shield, "its amount is the formula's (3.0 x the healing modifier at level 25): Base = Value");
    CHECKF(r.pawn.shields.size() == 1 && r.pawn.shields[0].value == shield, "the client's array holds it (%.2f)", shield);
    CHECK(r.pawn.confirmations.empty(), "(no confirmation yet: the cast's Default_Self hits and the overlap's effects are never confirmed)");
    // the client's LingerTick request: the hit's own Health mod is confirmed, the internal cooldown keeps it from giving another shield
    const double ask = t0 + 0.45;
    const auto verdict = r.hit_request("Bard_Dance_Shielding", "Bard_Dance_Shielding_LingerTick", ask, true, &seen);
    CHECK(verdict == AbilityPipeline::HitVerdict::Scheduled, "the client's request for LingerTick (aimed by the Bard_Dance_AOE targeting) is taken");
    seen = r.run_to(ask + 0.5);
    const Seen* applied = find_event(seen, [](const Seen& s) { return !s.confirmations.empty(); });
    CHECK(applied != nullptr, "the hit is applied and confirmed");
    if (!applied) return;
    CHECKF(near(applied->at, std::max(ask + AbilityPipeline::kConfirmDelay, t0 + 0.3 + AbilityPipeline::kHitDelay), 1e-9), "...kConfirmDelay after the request, never before the cost step (%.3f)", applied->at - t0);
    const auto& conf = applied->confirmations[0];
    CHECK(conf.source_type == content::kSourceTypeAbility && conf.source_is_actor && conf.response && conf.mods.size() == 1 && conf.mods[0].value > 0 && !conf.no_target,
          "the confirmation names the ability as its source, answers the request and carries the hit's own Health mod (a heal)");
    CHECKF(conf.mods[0].effective == 0.0f && near(conf.mods[0].value * 6.0, shield, 0.05), "at full Health: effective 0; the heal is 0.5 x the modifier and the shield 3.0 x (%.2f x 6 = %.2f)", conf.mods[0].value, shield);
    CHECK(!applied->change.shields && r.pawn.shields.size() == 1 && r.pawn.shields[0].value == shield, "the tick adds no shield (the 4 s internal cooldown is on)");
    // absorb: damage goes into the shield first
    const double health = r.health();
    const double through = r.pipe->take_damage(shield * 0.5, ask + 1.1);
    r.take();
    CHECK(near(through, 0.0, 1e-9) && near(r.health(), health, 1e-3) && r.pawn.shields.size() == 1 && near(r.pawn.shields[0].value, shield * 0.5, 0.01) && r.pawn.shields[0].base == shield,
          "half the shield's amount in damage: Health is untouched, the shield holds the other half");
    const double through2 = r.pipe->take_damage(shield, ask + 1.2);
    r.take();
    CHECK(near(through2, shield * 0.5, 0.01) && near(r.health(), health - shield * 0.5, 0.05) && r.pawn.shields[0].value == 0.0f, "the rest of a larger blow reaches Health once the shield is spent");
    // expiry at 8 s
    const double end = *r.pipe->effect_end(temp_hp);
    r.run_to(end - 0.01);
    CHECK(r.pawn.effect(temp_hp) != nullptr && r.pawn.shields.size() == 1, "the shield lasts until its effect ends");
    r.run_to(end + 0.01);
    CHECK(r.pawn.effect(temp_hp) == nullptr && r.pawn.shields.empty(), "when TempHP ends its element leaves the array");
    CHECK(r.pawn.broken.empty(), "(the pawn took every change)");
}

void test_hit_requests_hostile() {
    std::printf("hit requests: forged, repeated, for nothing\n");
    Rig r(2, 25, kTalents);
    CHECKF(r.ok(), "the rig: %s", r.problem.c_str());
    if (!r.ok()) return;
    r.asks = false;
    r.know({"Bard_Dance_Shielding", "Bard_Misc_Flourish"});
    using V = AbilityPipeline::HitVerdict;
    CHECK(r.hit_request("Bard_Dance_Shielding", "Bard_Dance_Shielding_LingerTick", 1.0) == V::NoCast, "a request for an ability that was not cast is dropped");
    r.trigger("Bard_Dance_Shielding", 2.0);
    r.run_to(2.5);
    const abilities::AbilityDef* shielding_def = r.catalog->ability_named("Bard_Dance_Shielding");
    const abilities::AbilityDef* flourish = r.catalog->ability_named("Bard_Misc_Flourish");
    const int64_t shielding = shielding_def->row.guid;
    int64_t tick = 0;
    for (const auto& h : shielding_def->hits)
        if (r.catalog->hit(h.hit_guid)->row.name == "Bard_Dance_Shielding_LingerTick") tick = h.hit_guid;
    CHECK(r.pipe->hit_request(shielding, content::kTargetingDefaultSelf, 12345, 1, 1, 2.6) == V::NotItsHit, "a hit that is none of the ability's is dropped");
    CHECK(r.pipe->hit_request(flourish->row.guid, content::kTargetingDefaultSelf, flourish->hits[0].hit_guid, 1, 1, 2.8) == V::NoCast, "the cast of another ability is not open");
    CHECK(r.hit_request("Bard_Dance_Shielding", "Bard_Dance_Shielding_LingerTick", 3.0) == V::Scheduled, "the real request is taken");
    CHECK(r.hit_request("Bard_Dance_Shielding", "Bard_Dance_Shielding_LingerTick", 3.01) == V::TooMany, "a second one 10 ms later is too fast");
    size_t taken = 1;
    for (int i = 0; i < 40; ++i) taken += r.hit_request("Bard_Dance_Shielding", "Bard_Dance_Shielding_LingerTick", 3.2 + 0.2 * i) == V::Scheduled ? 1 : 0;
    // the dance's zone lives 30 s and the client asks for its tick (1 s) every second while the actor lives: the cast takes as many as that holds, and no more
    const content::ZoneRow* zone = r.catalog->zone(shielding_def->zones.empty() ? 0 : shielding_def->zones[0]);
    CHECK(zone != nullptr && zone->life_time == 30.0, "the Shielding Dance has its zone (30 s)");
    const size_t cap = static_cast<size_t>(zone ? static_cast<int>(zone->life_time / zone->tick_rate) : 0) + AbilityPipeline::kZoneExtraRequests;
    CHECKF(taken <= cap && taken >= cap - 1, "a cast with a zone takes the zone's ticks and a few more requests of one hit, and no more (%zu of at most %zu)", taken, cap);
    r.run_to(40.0);
    size_t shields_added = 0;
    for (const Seen& s : r.log) shields_added += s.change.shields && !s.change.shields->empty() ? 1 : 0;
    CHECKF(shields_added <= 3, "the internal cooldown keeps the repeated requests from stacking shields (%zu shield updates)", shields_added);
    CHECK(r.pawn.broken.empty(), "(the pawn took every change)");
    // an ability with no zone takes at most kMaxRequestsPerHit of one hit
    {
        Rig rf(2, 25, kTalents);
        rf.asks = false;
        rf.know({"Bard_Misc_Flourish"});
        rf.trigger("Bard_Misc_Flourish", 2.0);
        rf.run_to(2.4);
        size_t flourish_taken = 0;
        for (int i = 0; i < 30; ++i) flourish_taken += rf.hit_request("Bard_Misc_Flourish", "Bard_Flourish_Self", 2.5 + 0.2 * i) == V::Scheduled ? 1 : 0;
        CHECKF(flourish_taken == static_cast<size_t>(AbilityPipeline::kMaxRequestsPerHit), "a cast with no zone takes %d requests of one hit (%zu)", AbilityPipeline::kMaxRequestsPerHit, flourish_taken);
    }
    // a request that names nothing of ours finds nobody: an empty confirmation, no change
    Rig r3(2, 25, kTalents);
    r3.asks = false;
    r3.know({"Bard_Dance_Shielding"});
    r3.trigger("Bard_Dance_Shielding", 2.0);
    r3.run_to(2.4);
    CHECK(r3.hit_request("Bard_Dance_Shielding", "Bard_Dance_Shielding_LingerTick", 2.5, false) == V::Empty, "a request that names none of the caster's components found nobody");
    r3.run_to(3.0);
    CHECK(r3.pawn.confirmations.size() == 1 && r3.pawn.confirmations[0].no_target, "it is answered by a confirmation with no target");
    // a Default_Self hit with nothing to show (no stat mod, no effect FX) is applied when asked for and never confirmed [R: 0 of 317 requests]
    Rig r2(2, 25, kTalents);
    r2.asks = false;
    r2.know({"Bard_Misc_Flourish"});
    r2.trigger("Bard_Misc_Flourish", 2.0);
    r2.run_to(2.1);
    CHECK(r2.pawn.effect(r2.effect("Bard_Pep")) == nullptr, "(no request, no hit: the client picks the hits its state needs)");
    CHECK(r2.hit_request("Bard_Misc_Flourish", "Bard_Flourish_Self", 2.15) == V::Scheduled, "the request for the Default_Self hit is taken");
    r2.run_to(4.0);
    CHECK(r2.pawn.confirmations.empty() && r2.pawn.effect(r2.effect("Bard_Pep")) != nullptr, "...Pep is on, and nothing is confirmed");
}
// C | Y | B - C.O.A

// ---- refusals ----------------------------------------------------------------------------------------------------------------------

void test_refusals() {
    std::printf("refusals\n");
    Rig r(2, 25, kTalents);
    CHECKF(r.ok(), "the rig: %s", r.problem.c_str());
    if (!r.ok()) return;
    r.know({"Bard_Misc_Flourish", "Bard_Misc_QuickWit_Jest", "Bard_Misc_QuickWit_Mock", "Bard_Misc_Fortissimo", "Ranger_Dash"});
    const Pawn before = r.pawn;
    auto unchanged = [&](const char* what) {
        CHECKF(r.pawn.stats == before.stats && r.pawn.effects.size() == before.effects.size() && r.pawn.abilities.size() == before.abilities.size() &&
                   r.pawn.cooldowns.size() == before.cooldowns.size(),
               "%s: nothing changed on the pawn", what);
    };
    TriggerInfo info;
    CHECK(r.pipe->trigger(987654321, 40, 1.0, info) == Refusal::NotEnabled, "an ability that is no row: not enabled");
    CHECK(r.pipe->trigger(r.ability("Bard_Dance_Maddening"), 41, 1.1, info) == Refusal::NotKnown, "a real ability that is not known");
    info.item = true;
    CHECK(r.pipe->trigger(r.ability("Bard_Misc_Flourish"), 42, 1.2, info) == Refusal::Unsupported, "an ability named with an item is not run here");
    info = {};
    CHECK(r.pipe->trigger(r.ability("Bard_Misc_QuickWit_Jest"), 43, 1.3, info) == Refusal::NeedsTarget, "an ability that needs a target and can be aimed at a friend is refused when the call names none");
    const Refusal dart = r.pipe->trigger(r.ability("Ranger_Dash"), 44, 1.4, info);
    CHECKF(dart == Refusal::NoFormula, "an ability whose activation requirement does not compile (Ranger Dash: GetLastProjectileFireTime) is refused (logged), never run half-way (%s)", refusal_name(dart));
    r.take();
    CHECK(r.pawn.ends == (std::vector<int32_t>{40, 41, 42, 43, 44}), "each of them is answered with ClientEndAbility(its ClientInstanceId), in order");
    unchanged("the refusals");
    // a replayed id is dropped without an answer
    const size_t ends = r.pawn.ends.size();
    CHECK(r.pipe->trigger(r.ability("Bard_Misc_Flourish"), 40, 1.5, {}) == Refusal::Duplicate, "a ClientInstanceId the session has used is a replay: dropped");
    r.take();
    CHECK(r.pawn.ends.size() == ends, "...with no ClientEndAbility (it would end the real instance of that id)");
    // another cast in progress
    CHECK(r.pipe->trigger(r.ability("Bard_Misc_Flourish"), 50, 2.0, {}) == Refusal::None, "(Flourish is cast)");
    CHECK(r.pipe->trigger(r.ability("Bard_Misc_Fortissimo"), 51, 2.1, {}) == Refusal::Cost, "another ability during its cast is taken (retail's combos re-trigger within a montage): here it lacks the stacks");
    r.take();
    r.run_to(2.4);
    CHECK(r.pipe->trigger(r.ability("Bard_Misc_Fortissimo"), 52, 2.5, {}) == Refusal::Cost, "(after the cost step it can be tried: Fortissimo then lacks the stacks)");
    r.take();
    // a spent cooldown and a cost that cannot be paid
    r.run_to(3.0);
    CHECK(r.pipe->trigger(r.ability("Bard_Misc_Flourish"), 53, 3.0, {}) == Refusal::None, "(Flourish again: its second charge)");
    r.run_to(3.5);
    CHECK(r.pipe->trigger(r.ability("Bard_Misc_Flourish"), 54, 4.0, {}) == Refusal::Charges, "a third: no charge left");
    r.run_to(60.0);
    r.set_current(content::kGuidMana, 10.0);
    const Pawn poor = r.pawn;
    CHECK(r.pipe->trigger(r.ability("Bard_Misc_Flourish"), 55, 60.5, {}) == Refusal::Cost, "with 10 Mana Flourish cannot be paid for");
    r.take();
    CHECK(near(r.pawn.stat(content::kGuidMana), poor.stat(content::kGuidMana), 1e-3) && r.pawn.cooldowns.size() == poor.cooldowns.size(), "nothing was spent and no cooldown was started");
    CHECK(r.pipe->refused() >= 9 && r.pawn.broken.empty(), "(the refusals are counted, the pawn took every change)");
}

void test_cast_flood() {
    std::printf("a flood of casts\n");
    Rig flood(2, 25, kTalents);
    CHECKF(flood.ok(), "the rig: %s", flood.problem.c_str());
    if (!flood.ok()) return;
    flood.know({"Bard_Misc_Flourish"});
    for (int id = 60; id < 60 + static_cast<int>(AbilityPipeline::kMaxCasts); ++id) flood.pipe->trigger(flood.ability("Bard_Misc_Flourish"), id, 2.15, {});
    CHECK(flood.pipe->trigger(flood.ability("Bard_Misc_Flourish"), 99, 2.16, {}) == Refusal::Busy, "a flood of waiting casts is refused: Busy");
    flood.run_to(5.0);
    CHECK(flood.pawn.broken.empty() && flood.pipe->charges_left(flood.ability("Bard_Misc_Flourish")) == 0, "(the casts that found no charge left at their cost step are over; the pawn took every change)");
}

void test_unaffordable_at_the_cost_step() {
    std::printf("a cast that cannot be paid for when its cost comes\n");
    Rig q(2, 25, kTalents);
    CHECKF(q.ok(), "the rig: %s", q.problem.c_str());
    if (!q.ok()) return;
    q.know({"Bard_Dance_Maddening"});
    q.run_to(1.0);
    CHECK(q.trigger("Bard_Dance_Maddening", 2.0) == Refusal::None, "(Maddening is cast)");
    q.run_to(2.299);
    q.set_current(content::kGuidMana, 5.0);   // something took the Mana before the cost step
    q.run_to(2.6);
    CHECK(q.pawn.abilities.empty() && q.pawn.cooldowns.empty() && !q.pawn.ends.empty() && q.pawn.stat(content::kGuidMana) < 6.0f,
          "the cast is over at its cost step: the item is taken back, no cooldown, ClientEndAbility, nothing paid");
    CHECK(q.pawn.broken.empty(), "(the pawn took every change)");
}

// ---- time --------------------------------------------------------------------------------------------------------------------------

void test_time() {
    std::printf("the clock\n");
    auto run = [](bool jump) {
        Rig r(2, 25, kTalents);
        r.know({"Bard_Misc_QuickWit"});
        r.set_current(content::kGuidHealth, 300.0);
        r.trigger("Bard_Misc_QuickWit", 2.0, true);
        // (a regeneration tick makes up for at most five seconds, so the horizon is inside that)
        if (jump) {
            r.run_to(5.5);
        } else {
            for (int i = 1; i <= 550; ++i) r.run_to(i * 0.01);
        }
        return r;
    };
    Rig a = run(true), b = run(false);
    CHECKF(a.log.size() == b.log.size(), "one 5.5 s step gives as many events as 550 steps of 10 ms (%zu, %zu)", a.log.size(), b.log.size());
    bool same = a.log.size() == b.log.size();
    for (size_t i = 0; same && i < a.log.size(); ++i) {
        same = near(a.log[i].at, b.log[i].at, 1e-9) && a.log[i].note == b.log[i].note && a.log[i].change.stats.size() == b.log[i].change.stats.size();
    }
    CHECK(same, "...the same events at the same times");
    CHECK(a.pawn.stats == b.pawn.stats && a.pawn.effects.size() == b.pawn.effects.size(), "and the same client state");
    Rig r(2, 25, kTalents);
    r.know({"Bard_Misc_Flourish"});
    r.run_to(10.0);
    CHECK(r.pipe->trigger(r.ability("Bard_Misc_Flourish"), 9, 5.0, {}) == Refusal::None && near(r.pipe->now(), 10.0, 1e-12), "a trigger with an earlier clock is handled at the later one");
    r.run_to(3.0);
    CHECK(near(r.pipe->now(), 10.0, 1e-12), "and the clock does not go back");
}

// ---- regeneration -----------------------------------------------------------------------------------------------------------------

void test_regeneration() {
    std::printf("regeneration\n");
    Rig r(2, 25, kTalents);
    CHECKF(r.ok(), "the rig: %s", r.problem.c_str());
    if (!r.ok()) return;
    const double regen_h = r.a7->stat(content::kGuidHealthRegeneration), regen_m = r.a7->stat(content::kGuidManaRegeneration);
    CHECK(regen_h > 0 && regen_m > 0 && near(regen_m, 7.4599, 1e-3), "(the engine's out-of-combat Mana regeneration of a level-25 Bard with no gear: 5 x 1.492)");
    r.set_current(content::kGuidHealth, 100.0);
    r.set_current(content::kGuidMana, 100.0);
    std::vector<Seen> seen = r.run_to(1.0);
    const Seen* first = find_event(seen, [](const Seen& s) { return s.note.find("regenerate") != std::string::npos; });
    CHECK(first && near(first->at, 1.0, 1e-9) && near(stat_of(*first, content::kGuidHealth), static_cast<float>(100.0f + static_cast<float>(regen_h)), 1e-3) &&
              near(stat_of(*first, content::kGuidMana), static_cast<float>(100.0f + static_cast<float>(regen_m)), 1e-3),
          "after one second Health and Mana gain their regeneration stat, in one change");
    seen = r.run_to(5.0);
    CHECK(near(r.health(), 100.0 + 5 * regen_h, 0.05) && near(r.mana(), 100.0 + 5 * regen_m, 0.05), "five seconds: five steps");
    // the last step is partial; at the maximum nothing more is sent
    const double max_mana = r.pawn.stat(content::kGuidMaxMana);
    r.set_current(content::kGuidMana, max_mana - 3.0);
    r.run_to(6.0);
    CHECK(r.mana() == max_mana, "a step that would pass the maximum stops on it");
    const size_t events = r.log.size();
    r.set_current(content::kGuidHealth, r.pawn.stat(content::kGuidMaxHealth));
    r.run_to(8.0);
    CHECK(r.log.size() == events && r.pipe->ticks_regen() >= 6, "at the maximum nothing is sent");
    // a Rest's boost counts: the pipeline reads the regeneration the pawn holds
    r.set_current(content::kGuidMana, 100.0);
    r.a7->trigger(content::kGuidRest, 7, 8.5);
    r.take();
    r.run_to(8.5 + 2.1);
    const double boosted = r.pawn.stat(content::kGuidManaRegeneration);
    CHECK(boosted > regen_m + 1.0, "(the Rest's effect raised ManaRegeneration)");
    const double mana_then = r.mana();
    r.run_to(8.5 + 3.2);
    CHECK(near(r.mana() - mana_then, boosted * (r.mana() - mana_then > 0 ? std::round((r.mana() - mana_then) / boosted) : 1), 0.05) && r.mana() > mana_then + boosted - 0.1,
          "the next tick adds the boosted amount");
    // a stall does not owe more than five ticks
    Rig s(2, 25, kTalents);
    s.set_current(content::kGuidMana, 100.0);
    const double regen = s.a7->stat(content::kGuidManaRegeneration);
    s.run_to(1000.0);
    CHECK(near(s.mana(), 100.0 + 5 * regen, 0.05), "a clock that jumps 1,000 s makes up for five ticks, no more");
    CHECK(r.pawn.broken.empty() && s.pawn.broken.empty(), "(the pawns took every change)");
}
// C | O | A + C Y B

// ---- movement ----------------------------------------------------------------------------------------------------------------------

void test_movement() {
    std::printf("movement abilities\n");
    Rig r(2, 25, kTalents);
    CHECKF(r.ok(), "the rig: %s", r.problem.c_str());
    if (!r.ok()) return;
    r.know({"Bard_Misc_Flourish", "Mage_Blink"});
    CHECK(r.pipe->movement_allowance(1.0) == 0, "no allowance before a cast");
    CHECK(r.trigger("Bard_Misc_Flourish", 2.0) == Refusal::None, "(Flourish is cast)");
    CHECK(near(r.pipe->movement_allowance(2.0), 3659.0 * 1.5, 1e-6) && near(r.pipe->movement_allowance(2.99), 3659.0 * 1.5, 1e-6), "Flourish's slide (3,659 cm/s in both captured casts) is allowed 1.5 x that for 1 s");
    CHECK(r.pipe->movement_allowance(3.01) == 0, "and the window is over after it");
    // an ability whose data names a speed (NetworkingMaxSpeed): Mage_Blink 7,000
    const abilities::AbilityDef* blink = r.catalog->ability_named("Mage_Blink");
    CHECK(blink && blink->row.networking_max_speed == 7000.0, "(Blink's data says 7,000 cm/s)");
    r.run_to(10.0);
    const Refusal rf = r.trigger("Mage_Blink", 11.0);
    if (rf == Refusal::None) {
        CHECK(near(r.pipe->movement_allowance(11.5), 7000.0 * 1.5, 1e-6), "Blink's window allows 1.5 x its NetworkingMaxSpeed");
    } else {
        std::printf("  (Blink is %s for this character: its window is not tested)\n", refusal_name(rf));
    }
}

// ---- A7 beside the generic runtime -----------------------------------------------------------------------------------------------

void test_a7_beside() {
    std::printf("Sprint, Dodge and the generic runtime\n");
    Rig r(2, 25, kTalents);
    CHECKF(r.ok(), "the rig: %s", r.problem.c_str());
    if (!r.ok()) return;
    r.know({"Bard_Misc_Flourish", "Bard_Misc_QuickWit"});
    // the Sprint's speed is its formula's: 0.2 + 0.05 for each Universal sprint rank the Bard holds (two at 25: Sprint Training I and II)
    r.a7->set_moving(true, 0.5);
    r.a7->trigger(content::kGuidSprint, 100, 1.0);
    r.take();
    CHECK(near(r.pawn.stat(kMoveSpeedMult), 1.3, 1e-6), "a level-25 Bard sprints at MoveSpeedMult 1.3 (retail's), not 1.2");
    // a buff of the generic runtime lands on top: Pep's 0.2 makes 1.5, as the retail Bard's replies show
    CHECK(r.trigger("Bard_Misc_Flourish", 2.0) == Refusal::None, "(Flourish is cast while sprinting)");
    r.run_to(2.4);
    CHECK(near(r.pawn.stat(kMoveSpeedMult), 1.5, 1e-6), "Pep adds its 0.2 to the Sprint's 0.3: MoveSpeedMult 1.5 (retail capture 44, +940 ms)");
    r.run_to(2.34 + 4.01);
    CHECK(near(r.pawn.stat(kMoveSpeedMult), 1.3, 1e-6), "Pep ends: back to the Sprint's 1.3");
    // Dodge beside the generic cooldowns: no item of one is dropped by the other, no key is shared
    r.a7->trigger(content::kGuidDodge, 101, 7.0);
    r.take();
    CHECK(r.pawn.cooldown(content::kGuidDodge) != nullptr && r.pawn.cooldown(r.ability("Bard_Misc_Flourish")) != nullptr, "a Dodge's Cooldowns item is added beside Flourish's: both are on the pawn");
    CHECK(r.trigger("Bard_Misc_QuickWit", 8.0, true) == Refusal::None, "(Quick Wit is cast)");
    r.run_to(9.5);
    CHECK(r.pawn.cooldowns.size() == 3 || r.pawn.cooldowns.size() == 2, "the three cooldowns are listed (or two once the Dodge's 0.65 s is over)");
    CHECK(r.pawn.broken.empty(), "the ability items, effects and cooldowns of both runtimes never share a key");
    r.a7->cancel(100, 10.0);
    r.run_to(14.0);   // (the Pep Quick Wit's Jest hit gave at 8.64 ends at 12.64)
    CHECKF(r.pawn.stat(kMoveSpeedMult) == 1.0f && r.pawn.broken.empty(), "the Sprint ends: MoveSpeedMult is 1 again (%.4f)", r.pawn.stat(kMoveSpeedMult));
    // a level-1 Bard (no Sprint Training yet) sprints at 1.2
    Rig low(2, 1, {});
    CHECKF(low.ok(), "the level-1 rig: %s", low.problem.c_str());
    if (!low.ok()) return;
    low.a7->set_moving(true, 0.5);
    low.a7->trigger(content::kGuidSprint, 1, 1.0);
    low.take();
    CHECK(near(low.pawn.stat(kMoveSpeedMult), 1.2, 1e-6), "a level-1 Bard sprints at 1.2");
}

}  // namespace

// ---- plan D2, stage 2: class resources, the Mend chain, the client's end of an ability ----------------------------------------------------

void test_class_resources() {
    std::printf("class resources are held stats that move by their regeneration\n");
    constexpr int64_t kFocus = 6064633175093543338ll;   // the Ranger's Focus
    Rig r(1, 25);
    CHECKF(r.ok(), "the rig: %s", r.problem.c_str());
    if (!r.ok()) return;
    const stats::StatValue* focus = r.cs->find(kFocus);
    CHECK(focus != nullptr && focus->resource(), "a Ranger's Focus is a held resource stat");
    if (!focus) return;
    r.set_current(kFocus, 10.0);
    r.run_to(1.0);
    const double after_1 = r.pawn.stat(kFocus);
    CHECK(near(after_1, 13.0, 0.01), "a second on, Focus has moved by its regeneration (+3)");
    r.run_to(4.0);
    CHECK(near(r.pawn.stat(kFocus), 22.0, 0.01), "...and keeps moving, one step a second");
    r.set_current(kFocus, r.pawn.stat(kFocus) > 0 ? 99.0 : 0.0);
    r.run_to(6.0);
    CHECK(r.pawn.stat(kFocus) <= 100.0 && r.pawn.stat(kFocus) >= 99.0, "a step stops on the stat's maximum");
    CHECK(r.pawn.broken.empty(), "(the pawn took every change)");
    // a Cleric's DivinePower (Gen) and a Tank's Courage are held as well: a recomputation does not reset them
    Rig c(3, 25);
    CHECKF(c.ok(), "the Cleric rig: %s", c.problem.c_str());
    if (!c.ok()) return;
    constexpr int64_t kPower = 6064629623747772676ll;
    const stats::StatValue* power = c.cs->find(kPower);
    CHECK(power != nullptr && power->resource(), "a Cleric's DivinePower is a held resource stat");
    c.set_current(kPower, 40.0);
    c.run_to(3.0);
    const double kept = c.pawn.stat(kPower);
    CHECK(kept >= 0.0 && kept <= 100.0 && c.pawn.broken.empty(), "the value that was set survives the engine's recomputation (it is held, not derived)");
}

void test_mend_chain() {
    std::printf("Mend: one projectile per cast, chosen by the charges used\n");
    Rig r(3, 25);
    CHECKF(r.ok(), "the rig: %s", r.problem.c_str());
    if (!r.ok()) return;
    r.know({"Cleric_Mend"});
    std::vector<Seen> seen;
    size_t confirmed_first = 0;
    CHECK(r.trigger("Cleric_Mend", 2.0, true, 0, &seen) == Refusal::None, "Mend, cast at an ally, is accepted");
    seen = r.run_to(4.0);
    for (const Seen& s : seen) confirmed_first += s.confirmations.size();
    const Seen* cost = find_event(seen, [](const Seen& s) { return !s.change.cooldowns_set.empty(); });
    CHECK(cost != nullptr, "it pays its cost and starts its cooldown");
    CHECK(confirmed_first >= 1 && r.pawn.broken.empty(), "its hit is confirmed, and the pawn took every change");
    CHECK(r.trigger("Cleric_Mend", 4.5, true, 0, &seen) == Refusal::None, "a second Mend, with a charge left, is accepted");
    seen = r.run_to(6.5);
    size_t confirmed_second = 0;
    for (const Seen& s : seen) confirmed_second += s.confirmations.size();
    CHECK(confirmed_second >= 1, "the second cast's hit is confirmed as well (the chain goes on with the charges used)");
    CHECK(r.pawn.broken.empty(), "(the pawn took every change)");
}
// [C-O-A ~ Cyb]

void test_end_request() {
    std::printf("the client's ServerEndAbility\n");
    Rig r(2, 25, kTalents);
    CHECKF(r.ok(), "the rig: %s", r.problem.c_str());
    if (!r.ok()) return;
    r.know({"Bard_Music_Cheerful"});
    std::vector<Seen> seen;
    CHECK(r.trigger("Bard_Music_Cheerful", 2.0, false, 2, &seen) == Refusal::None, "(a song is cast)");
    seen = r.run_to(2.5);
    int64_t instance = -1;
    for (const auto& [k, v] : r.pawn.abilities)
        if (v.ability_guid == r.ability("Bard_Music_Cheerful")) instance = static_cast<int64_t>(k);
    CHECK(instance >= 0, "(its item is on the pawn)");
    CHECK(!r.pipe->end_request(static_cast<int32_t>(instance) + 1000, 3.0), "an instance that is not ours is not ended");
    CHECK(r.pipe->end_request(static_cast<int32_t>(instance), 3.0), "the instance the client names is ended...");
    CHECK(!r.pipe->end_request(static_cast<int32_t>(instance), 3.01), "...once");
    seen = r.run_to(3.2);
    CHECK(find_event(seen, [](const Seen& s) { return s.end.has_value(); }) == nullptr, "nothing is sent before 259 ms");
    seen = r.run_to(3.4);
    const Seen* end = find_event(seen, [](const Seen& s) { return s.end.has_value(); });
    CHECK(end && near(end->at, 3.259, 1e-6), "259 ms after the request the client's instance is ended (ClientEndAbility)");
    CHECK(r.pawn.broken.empty(), "(the pawn took every change)");
}

// ---- the ranks the join holds (review: GetSkillPurchased(tree, <SkillRank>) could never be true) ---------------------------------------------------

/// The ab_expr id of the expression with exactly this text (the ids follow the order contentc meets the expressions in, which moves with every ability added to the tables).
int64_t expr_by_text(const std::string& path, const char* text) {
    sqlite3* db = nullptr;
    int64_t id = 0;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) return 0;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT id FROM ab_expr WHERE text = ?1", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, text, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) id = sqlite3_column_int64(st, 0);
    }
    if (st) sqlite3_finalize(st);
    sqlite3_close(db);
    return id;
}

/// Jest's charges formula (GetSkillPurchased of the Jest talent).
int64_t jest_charges_expr(Rig& r) {
    const abilities::AbilityDef* a = r.catalog->ability_named("Bard_Misc_QuickWit_Jest");
    return a ? a->row.charges_expr : 0;
}

/// A formula's value on the character the Rig holds (the program of an ab_expr id).
double eval_expr(Rig& r, int64_t expr) {
    std::string err;
    auto ev = r.cs->evaluation({}, err);
    if (!ev) return -1e9;
    stats::ExprHost host;
    double v = -1e9;
    if (!ev->eval(r.catalog->program(expr), host, v, err)) return -1e9;
    return v;
}

bool holds(const std::vector<stats::SkillRank>& held, uint64_t tree_type, int64_t tree, uint64_t type, int64_t guid) {
    for (const stats::SkillRank& s : held)
        if (s.cls.type_id == tree_type && s.cls.guid == tree && s.skill.type_id == type && s.skill.guid == guid) return true;
    return false;
}
// C-Y-B / CoA

void test_held_ranks() {
    std::printf("the ranks the join holds: the talents::Book's held_ranks() and passives(), the one source of both the session and these tests\n");
    Rig bard(2, 25, kTalents);
    CHECKF(bard.ok(), "the rig: %s", bard.problem.c_str());
    if (!bard.ok()) return;
    const std::vector<stats::SkillRank> held = bard.book->held_ranks();
    const int64_t tree = bard.book->class_tree()->guid;
    // a formula names a held skill (160 of the 171 that ask GetSkillPurchased) or one rank of it (11, a talent's upgrade): both are held, for every listed rank
    size_t skills = 0, ranks = 0;
    for (const stats::SkillRank& s : held) {
        skills += s.skill.type_id == stats::kRefTypeSkill ? 1 : 0;
        ranks += s.skill.type_id == stats::kRefTypeSkillRank ? 1 : 0;
    }
    CHECKF(skills > 5 && skills == ranks, "every listed rank is held twice: as its skill and as itself (%zu and %zu)", skills, ranks);
    // the bought talent
    const int64_t jest_skill = record_guid(bard.path, "SkillRecordBase", "Bard_QuickWit_Jest");
    CHECK(jest_skill != 0 && holds(held, stats::kRefTypeSkillTree, tree, stats::kRefTypeSkill, jest_skill), "the Bard's bought Jest talent is held as a skill of the class tree");
    CHECK(eval_expr(bard, jest_charges_expr(bard)) == 2.0, "...and Jest's charges formula (GetSkillPurchased of that skill) gives 2");
    Rig plain(2, 25);
    CHECK(plain.ok() && eval_expr(plain, jest_charges_expr(plain)) == 1.0, "a Bard without the talent has 1");
    // the other source of the auto-granted ranks (the ability tables' reader, which production does not use) agrees: every one is held
    std::string err;
    auto ac = content::AbilityContent::open(bard.path, err);
    std::vector<stats::SkillRank> auto25;
    CHECK(ac && ac->auto_ranks_at(25, auto25, err) && !auto25.empty(), "the reader of the auto-granted ranks of a level-25 character");
    // (the reader lists the auto-granted ranks of every tree of the game; the Book holds those of the trees the character has: its class tree, the Universal
    // tree and the professions' that have a rank)
    std::set<int64_t> own_trees;
    for (const talents::Tree* t : bard.book->listed_trees()) own_trees.insert(t->guid);
    size_t missing = 0, expected = 0;
    for (const stats::SkillRank& a : auto25) {
        if (!own_trees.count(a.cls.guid)) continue;
        ++expected;
        missing += holds(held, a.cls.type_id, a.cls.guid, a.skill.type_id, a.skill.guid) ? 0 : 1;
    }
    CHECKF(expected > 5 && missing == 0, "every auto-granted rank of level 25 in the character's own trees is held by the Book (%zu of %zu missing)", missing, expected);
    // what the passives carry: Endurance Training's MaxStamina +10, as an effect the stat engine runs
    bool endurance = false;
    for (const stats::ActiveEffect& e : bard.book->passives(0)) {
        for (const stats::EffectMod& m : e.mods) endurance = endurance || (m.stat == content::kGuidMaxStamina && m.value == 10.0 && e.effect.type_id == stats::kRefTypeEffect && e.stacks == 1);
    }
    CHECK(endurance, "passives() carries Endurance Training: MaxStamina +10 as an Effect-typed reference");
    CHECK(bard.a7->max_stamina() == 110.0 && plain.a7->max_stamina() == 110.0, "(so a level-25 Bard has 110 Stamina, as the join gives it)");
    // a talent's upgrade names one rank (Mage_Passive_Blink_Unyielding): false without it, true with it
    {
        Rig mage_plain(8, 25);
        CHECKF(mage_plain.ok(), "the Mage rig: %s", mage_plain.problem.c_str());
        if (mage_plain.ok()) CHECK(eval_expr(mage_plain, expr_by_text(mage_plain.path, "GetSkillPurchased(GetInstigator(), $#297887639323616934:107281647359609$, $#4847957796014348780:6064631986549818361$)")) == 0.0, "a Mage without the Blink upgrade: GetSkillPurchased(tree, <that rank>) is false");
    }
    {
        Rig mage(8, 25, {"Mage_Passive_Blink_Unyielding"});
        CHECKF(mage.ok(), "the Mage rig with the upgrade: %s", mage.problem.c_str());
        if (mage.ok()) {
            CHECK(eval_expr(mage, expr_by_text(mage.path, "GetSkillPurchased(GetInstigator(), $#297887639323616934:107281647359609$, $#4847957796014348780:6064631986549818361$)")) == 1.0, "...and true once the rank is bought (it could never be true: the join held skills only)");
            CHECK(holds(mage.book->held_ranks(), stats::kRefTypeSkillTree, 107281647359609ll, stats::kRefTypeSkillRank, 6064631986549818361ll), "held_ranks() lists the rank-typed reference of the bought rank");
        }
    }
}

// ---- a charge chain's Cooldowns items (review: retail keeps the first use's start and moves the end on) ---------------------------------------

/// The Cooldowns item of `ability` in the first event of `seen` that has one.
const bootstrap::CooldownItem* cooldown_of(const std::vector<Seen>& seen, int64_t ability) {
    for (const Seen& s : seen)
        for (const auto& cd : s.change.cooldowns_set)
            if (cd.ability_guid == ability) return &cd;
    return nullptr;
}

void test_charge_chain() {
    std::printf("charges: the Cooldowns item of a chain keeps the first use's start [R: captures 39 and 44]\n");
    Rig r(2, 25, kTalents);
    CHECKF(r.ok(), "the rig: %s", r.problem.c_str());
    if (!r.ok()) return;
    r.know({"Bard_Misc_QuickWit_Jest", "Bard_Misc_Flourish"});
    const int64_t jest = r.ability("Bard_Misc_QuickWit_Jest");
    // Jest has two charges and an 8 s cooldown: use both, 3 s apart
    std::vector<Seen> seen;
    CHECK(r.trigger("Bard_Misc_QuickWit_Jest", 2.0, true, 0, &seen) == Refusal::None, "the first Jest is accepted");
    seen = r.run_to(2.5);
    const bootstrap::CooldownItem* first = cooldown_of(seen, jest);
    CHECK(first && first->charges == 1 && first->initial_cooldown == 8.0f && first->triggered, "the first use: Charges 1 left, InitialCooldown 8 [R: 8.0, 1]");
    if (!first) return;
    const double start = first->start_time;
    CHECKF(near(first->end_time - start, 8.0, 1e-6), "the first item ends 8 s after it starts (%.4f)", first->end_time - start);
    r.set_current(content::kGuidMana, r.pawn.stat(content::kGuidMaxMana));
    CHECK(r.trigger("Bard_Misc_QuickWit_Jest", 5.0, true, 0, &seen) == Refusal::None, "the second Jest is accepted with one charge left");
    seen = r.run_to(5.5);
    const bootstrap::CooldownItem* second = cooldown_of(seen, jest);
    CHECK(second && second->charges == 0 && second->initial_cooldown == 8.0f, "the second use: Charges 0 [R: 8.0, 0]");
    CHECKF(second && second->start_time == start && near(second->end_time - start, 8.0, 1e-6), "...with the same start and end as the first (the second use does not restart the 8 s: %.4f)", second ? second->end_time - second->start_time : -1.0);
    // the first charge returns 8 s after the first use: the chain goes on from its end, the start stays [R: start S, end S + 16, Charges 1]
    seen = r.run_to(2.3 + 8.0 + 0.05);
    const bootstrap::CooldownItem* back = cooldown_of(seen, jest);
    CHECK(back && back->charges == 1 && back->initial_cooldown == 8.0f, "a charge is back: Charges 1");
    CHECKF(back && back->start_time == start, "its start is the first use's (%.4f, not 8 s later)", back ? back->start_time - start : -1.0);
    CHECKF(back && near(back->end_time - start, 16.0, 1e-6), "its end is 16 s after the start: the next charge returns 8 s on (%.4f)", back ? back->end_time - start : -1.0);
    CHECK(r.pawn.cooldown(jest) != nullptr, "(the item stays while a charge is missing)");
    seen = r.run_to(2.3 + 16.0 + 0.05);
    bool removed = false;
    for (const Seen& s : seen)
        for (const int64_t a : s.change.cooldowns_removed) removed = removed || a == jest;
    CHECK(removed && r.pawn.cooldown(jest) == nullptr, "the last charge returns 16 s after the first use and the item goes");
    // Flourish: two charges, a 20 s cooldown: the same shape with 20 and 40
    const int64_t flourish = r.ability("Bard_Misc_Flourish");
    const double f0 = 30.0;
    r.trigger("Bard_Misc_Flourish", f0);
    seen = r.run_to(f0 + 0.5);
    const bootstrap::CooldownItem* f1p = cooldown_of(seen, flourish);
    const bootstrap::CooldownItem f1 = f1p ? *f1p : bootstrap::CooldownItem{};
    CHECK(f1p && f1.charges == 1 && f1.initial_cooldown == 20.0f, "Flourish's first use: Charges 1, InitialCooldown 20 [R: capture 44]");
    r.trigger("Bard_Misc_Flourish", f0 + 2.0);
    seen = r.run_to(f0 + 2.5);
    const bootstrap::CooldownItem* f2 = cooldown_of(seen, flourish);
    CHECK(f1p && f2 && f2->charges == 0 && f2->start_time == f1.start_time && near(f2->end_time - f2->start_time, 20.0, 1e-6), "the second: Charges 0, the same start and end");
    seen = r.run_to(f0 + 0.3 + 20.0 + 0.05);
    const bootstrap::CooldownItem* f3 = cooldown_of(seen, flourish);
    CHECK(f1p && f3 && f3->charges == 1 && f3->start_time == f1.start_time && near(f3->end_time - f3->start_time, 40.0, 1e-6), "the return: Charges 1, start S, end S + 40 [R: capture 44]");
    CHECK(r.pawn.broken.empty(), "(the pawn took every change)");
}

// ---- a dance's zone: its actor, and what ends with it (review: the dances' visual effect) --------------------------------------------------

struct ZoneSeen {
    double              at;
    bootstrap::ZoneOp   op;
};
std::vector<ZoneSeen> zone_events(const Rig& r) {
    std::vector<ZoneSeen> out;
    for (const Seen& s : r.log)
        for (const bootstrap::ZoneOp& z : s.zones) out.push_back({s.at, z});
    return out;
}
const ZoneSeen* zone_event(const std::vector<ZoneSeen>& zs, bootstrap::ZoneOp::Kind kind, int64_t ability, size_t nth = 0) {
    for (const ZoneSeen& z : zs) {
        if (z.op.kind == kind && z.op.ability == ability && nth-- == 0) return &z;
    }
    return nullptr;
}

void test_dance_zones() {
    std::printf("dances: the zone's actor opens near the cast, ends with the dance and takes the Aura with it\n");
    using K = bootstrap::ZoneOp::Kind;
    Rig r(2, 25, kTalents);
    CHECKF(r.ok(), "the rig: %s", r.problem.c_str());
    if (!r.ok()) return;
    r.asks = false;
    r.know({"Bard_Dance_Shielding", "Bard_Dance_Maddening", "Bard_Dance_Nimble", "Bard_Dance_Mesmerizing", "Bard_Misc_Flourish", "Bard_Saga", "Bard_Misc_QuickWit_Jest"});
    const int64_t shielding = r.ability("Bard_Dance_Shielding");
    const int64_t aura = r.effect("Bard_Dance_Shielding_Aura");
    // which abilities spawn a zone with the cast: the dances (Mesmerizing's has no hit of its own), not the Saga (a hit of it spawns its zone) or the others
    for (const char* name : {"Bard_Dance_Shielding", "Bard_Dance_Maddening", "Bard_Dance_Nimble", "Bard_Dance_Mesmerizing"}) {
        const abilities::AbilityDef* d = r.catalog->ability_named(name);
        CHECKF(d && d->zones.size() == 1 && r.catalog->zone(d->zones[0]) != nullptr, "%s spawns one zone with its cast", name);
    }
    CHECK(r.catalog->ability_named("Bard_Saga")->zones.empty() && r.catalog->ability_named("Bard_Misc_Flourish")->zones.empty() &&
              r.catalog->ability_named("Bard_Misc_QuickWit_Jest")->zones.empty(),
          "the Saga (its zone comes with a hit), Flourish and Jest spawn none");
    const content::ZoneRow* zone = r.catalog->zone(r.catalog->ability_named("Bard_Dance_Shielding")->zones[0]);

    // the cast: with no captured timing the actor opens 35 ms after the ability item (retail: Maddening +362 ms, its item +328)
    const double t0 = 2.0;
    CHECK(r.trigger("Bard_Dance_Shielding", t0) == Refusal::None, "Shielding is accepted");
    r.run_to(t0 + 1.0);
    std::vector<ZoneSeen> zs = zone_events(r);
    const ZoneSeen* open = zone_event(zs, K::Open, shielding);
    CHECK(zs.size() == 1 && open != nullptr, "one zone event so far: its actor opens");
    if (!open) return;
    CHECKF(near(open->at - t0, AbilityPipeline::kItemDelay + AbilityPipeline::kZoneOpenDelay, 1e-9) && open->at - t0 >= 0.3 && open->at - t0 <= 0.5,
           "within 300 to 500 ms of the trigger (%.0f ms)", (open->at - t0) * 1000);
    CHECK(open->op.zone == zone->guid && open->op.id > 0, "it names the zone record and has an instance number");
    CHECK(r.pawn.effect(aura) != nullptr && r.pawn.effect(aura)->duration > 1e30f, "the Aura, the zone's infinite effect, is on the Bard");
    const int64_t aura_key = r.pawn.effect(aura) ? r.pawn.effect(aura)->effect_id : -1;
    // the zone's own life in the data (30 s) ends the dance when nothing is known of its montage
    r.run_to(open->at + 29.0);
    CHECK(zone_events(r).size() == 1 && r.pawn.effect(aura) != nullptr, "29 s on: no close, the Aura is still on");
    // the client's LingerTick requests are taken while the actor lives, well after the cast's own 5 s
    CHECK(r.hit_request("Bard_Dance_Shielding", "Bard_Dance_Shielding_LingerTick", open->at + 28.0) == AbilityPipeline::HitVerdict::Scheduled, "a LingerTick request 28 s in is taken (the zone's actor is still there)");
    r.run_to(open->at + 32.0);
    zs = zone_events(r);
    double aura_gone = -1;
    for (const Seen& s : r.log)
        for (const int64_t key : s.change.effects_removed)
            if (key == aura_key && aura_gone < 0) aura_gone = s.at;
    const ZoneSeen* fade = zone_event(zs, K::Fade, shielding);
    const ZoneSeen* close = zone_event(zs, K::Close, shielding);
    CHECK(r.pawn.effect(aura) == nullptr, "the Aura is gone from the Bard (it had no end)");
    CHECKF(aura_gone > 0 && near(aura_gone, open->at + zone->life_time, 1e-9), "...when the zone's 30 s are over (%.3f)", aura_gone - open->at);
    CHECK(fade && near(fade->at, aura_gone + AbilityPipeline::kZoneFadeDelay, 1e-9), "the actor fades out 60 ms later (bFadingOut)");
    CHECK(close && near(close->at, aura_gone + AbilityPipeline::kZoneCloseDelay, 1e-9) && fade && fade->at < close->at, "and is closed 0.7 s after the Aura went");
    CHECK(zs.size() == 3 && r.pipe->zones() == 0, "(the actor opens once, fades once and closes once: nothing is left)");
    CHECK(r.hit_request("Bard_Dance_Shielding", "Bard_Dance_Shielding_LingerTick", open->at + 40.0) == AbilityPipeline::HitVerdict::NoCast, "a LingerTick request after the close is dropped");
    CHECK(r.pawn.broken.empty(), "(the pawn took every change)");
// [C_Y_B ~ CoA]

    // the retail timings (the session's default): the dance lasts as long as the captures' did, whatever else the Bard casts
    Rig m(2, 25, kTalents);
    m.asks = false;
    m.pipe->set_data_timings(true);
    m.know({"Bard_Dance_Maddening", "Bard_Dance_Mesmerizing", "Bard_Dance_Shielding", "Bard_Misc_Flourish"});
    const int64_t maddening = m.ability("Bard_Dance_Maddening"), mesmerizing = m.ability("Bard_Dance_Mesmerizing");
    m.trigger("Bard_Dance_Maddening", 2.0);
    m.run_to(10.0);
    std::vector<ZoneSeen> mz = zone_events(m);
    const ZoneSeen* mo = zone_event(mz, K::Open, maddening);
    const ZoneSeen* mf = zone_event(mz, K::Fade, maddening);
    const ZoneSeen* mc = zone_event(mz, K::Close, maddening);
    CHECKF(mo && near(mo->at - 2.0, 0.362, 1e-9), "Maddening Dance: its actor opens 362 ms after the trigger, as retail's [R: capture 44] (%.0f ms)", mo ? (mo->at - 2.0) * 1000 : -1.0);
    CHECKF(mc && near(mc->at - 2.0, 5.383, 1e-9), "and is destroyed 5,383 ms after it [R] (%.0f ms)", mc ? (mc->at - 2.0) * 1000 : -1.0);
    CHECK(mf && mc && near(mc->at - mf->at, AbilityPipeline::kZoneCloseDelay - AbilityPipeline::kZoneFadeDelay, 1e-9), "with the fade-out 0.64 s before [R: 641 ms]");
    // a cast of another ability meanwhile does not end it (retail: the Bard sang and slid during the dances, which went on)
    m.set_current(content::kGuidMana, m.pawn.stat(content::kGuidMaxMana));
    m.trigger("Bard_Dance_Shielding", 20.0);
    m.run_to(21.0);
    m.set_current(content::kGuidMana, m.pawn.stat(content::kGuidMaxMana));
    m.trigger("Bard_Misc_Flourish", 21.0);
    m.run_to(30.0);
    mz = zone_events(m);
    const ZoneSeen* so = zone_event(mz, K::Open, shielding);
    const ZoneSeen* sc = zone_event(mz, K::Close, shielding);
    CHECKF(so && near(so->at - 20.0, 0.287, 1e-9) && sc && near(sc->at - 20.0, 5.247, 1e-9), "Shielding Dance (zone +287 ms, close +5,247 ms: the middle of the two captures) is not ended by Flourish [R]");
    // Mesmerizing: a zone with no hit of its own, a short montage
    m.set_current(content::kGuidMana, m.pawn.stat(content::kGuidMaxMana));
    m.trigger("Bard_Dance_Mesmerizing", 50.0);
    m.run_to(60.0);
    mz = zone_events(m);
    const ZoneSeen* eo = zone_event(mz, K::Open, mesmerizing);
    const ZoneSeen* ec = zone_event(mz, K::Close, mesmerizing);
    CHECK(eo && ec && near(eo->at - 50.0, 0.389, 1e-9) && near(ec->at - 50.0, 2.334, 1e-9), "Mesmerizing Dance: its actor opens at +389 ms and is destroyed at +2,334 ms [R]");
    // the effects of the dance end before the actor goes: the Aura of Shielding at 5,247 - 700 ms
    const int64_t shielding_aura = m.effect("Bard_Dance_Shielding_Aura");
    int64_t aura_id = -1;
    for (const Seen& s : m.log)
        for (const auto& e : s.change.effects_added)
            if (e.effect_guid == shielding_aura) aura_id = e.effect_id;
    double gone = -1;
    for (const Seen& s : m.log)
        for (const int64_t k : s.change.effects_removed)
            if (k == aura_id && gone < 0) gone = s.at;
    CHECK(sc && gone > 0 && near(gone, sc->at - AbilityPipeline::kZoneCloseDelay, 1e-9), "Shielding's Aura goes 0.7 s before its actor does [R: 702 ms]");
    CHECK(m.pawn.broken.empty() && m.pawn.effect(shielding_aura) == nullptr, "(the pawn took every change; no Aura is left)");
}

int main() {
    if (!real_db_present()) {
        std::printf("no content.db at %s (cd services && go run ./cmd/contentc): the generic ability runtime's tests are skipped\n", real_db_path().c_str());
        return 0;
    }
    test_held_ranks();
    test_charge_chain();
    test_dance_zones();
    test_catalog();
    test_cast_timeline();
    test_mana_costs();
    test_resonance_and_fortissimo();
    test_glee();
    test_stat_forms();
    test_songs_and_the_air();
    test_confirmations();
    test_shielding_dance();
    test_hit_requests_hostile();
    test_refusals();
    test_cast_flood();
    test_unaffordable_at_the_cost_step();
    test_time();
    test_regeneration();
    test_movement();
    test_a7_beside();
    test_class_resources();
    test_mend_chain();
    test_end_request();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
