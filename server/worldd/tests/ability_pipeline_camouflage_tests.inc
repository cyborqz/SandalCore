// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/ability_pipeline_camouflage_tests.inc: the Ranger's camouflage as the game's data gives it: the infinite stealth effect, its StealthLevel -5,
//  the character's stealth level and the array every client is sent it in (the same numbers another player reads)
//  (registered with AOC_REGISTER_TEST: test_worldd_ability_pipeline.cpp runs it through aoc_tests::run; it is included from ability_pipeline_registered.inc)
// ============================================================================

namespace {
constexpr int64_t kStealthLevel = 6064633186496741774ll;   // StatTypeDef StealthLevel (CombatStats)
}

void test_camouflage_stealth_data() {
    std::printf("Camouflage: the stealth effect, the stealth level it lowers and who is sent it\n");
    Rig r(1, 25);   // a level-25 Ranger
    CHECKF(r.ok(), "the Ranger rig: %s", r.problem.c_str());
    if (!r.ok()) return;
    const int64_t camo_effect = r.effect("Ranger_Camouflage");
    const int64_t camo = r.ability("Ranger_Camouflage");
    const abilities::EffectDef* camo_def = r.catalog->effect(camo_effect);
    const abilities::AbilityDef* camo_ability = r.catalog->ability(camo);
    CHECK(camo_def && camo_def->row.infinite_duration && camo_def->row.name == "Ranger_Camouflage", "the data: Ranger_Camouflage is an infinite effect");
    bool level_mod = false;
    for (const content::ModRow& m : camo_def->mods) level_mod = level_mod || (m.stat_guid == kStealthLevel && m.value_expr != 0);
    CHECK(level_mod, "the effect's stat mod names StealthLevel (the data's -5)");
    CHECK(camo_ability && std::find(camo_ability->row.tags.begin(), camo_ability->row.tags.end(), "ability.Toggle") != camo_ability->row.tags.end(),
          "the ability is the data's toggle (ability.Toggle)");
    CHECK(r.pawn.stat(kStealthLevel) == 25.0f, "the character's StealthLevel is its level, 25");
    CHECK(r.cs->find(kStealthLevel) && r.cs->find(kStealthLevel)->replication == 7, "...and everyone is sent it (the EveryoneProxy array: another player reads the same number)");
}
AOC_REGISTER_TEST(test_camouflage_stealth_data);

// The toggle as the retail recordings of Camouflage show it: on 0.1 s after the press, on until an action ends it, and the press that ends it starts the
// 61 s cooldown (the ability's 1 s and the End hit's 60 s)
void test_camouflage_toggle() {
    std::printf("Camouflage: on after the press, on until the next action, and the press that ends it starts the 61 s cooldown\n");
    Rig r(1, 25);
    CHECKF(r.ok(), "the Ranger rig: %s", r.problem.c_str());
    if (!r.ok()) return;
    r.pipe->set_data_timings(true);   // (the retail times of the cast: the cost at 0.10 s, the item at 0.11 s, the hit at 0.14 s)
    r.know({"Ranger_Camouflage", "General_Guard"});
    const int64_t effect = r.effect("Ranger_Camouflage");
    const int64_t ability = r.ability("Ranger_Camouflage");
    CHECK(r.trigger("Ranger_Camouflage", 1.0) == Refusal::None, "the cast is accepted");
    r.run_to(1.5);
    CHECK(r.pawn.effect(effect) != nullptr && near(r.pawn.stat(kStealthLevel), 20.0, 1e-3), "0.5 s after the press: the effect is on and the stealth level is 25 less its 5");
    CHECK(r.pawn.cooldown(ability) && near(r.pawn.cooldown(ability)->initial_cooldown, 1.0, 1e-3), "the ability's own cooldown is 1 s");
    r.run_to(5.0);
    CHECK(r.pawn.effect(effect) != nullptr && near(r.pawn.stat(kStealthLevel), 20.0, 1e-3), "4 s after the press: it is still on (nothing ends it but an action)");
    // the press that ends it: the client asks for no hit of it (the retail recording of the second press has none)
    r.asks = false;
    CHECK(r.trigger("Ranger_Camouflage", 5.0, false, 1) == Refusal::None, "the second press is accepted");
    r.run_to(5.5);
    CHECK(r.pawn.effect(effect) == nullptr && near(r.pawn.stat(kStealthLevel), 25.0, 1e-3), "0.5 s after it: the effect is off and the stealth level is its 25 again");
    CHECK(r.pawn.cooldown(ability) && near(r.pawn.cooldown(ability)->initial_cooldown, 61.0, 1e-3), "the cooldown the press left is 61 s (the ability's 1 s and the End hit's 60 s, as the retail recordings of the toggle off)");
    CHECK(r.trigger("Ranger_Camouflage", 20.0, false, 0) != Refusal::None, "20 s later it cannot be pressed again");
    CHECK(r.pawn.effect(effect) == nullptr, "(and the refused press left nothing on)");
    // any other action ends it too (the reaction's trigger is every ability but Sprint)
    Rig q(1, 25);
    if (!q.ok()) return;
    q.pipe->set_data_timings(true);
    q.know({"Ranger_Camouflage", "General_Guard"});
    CHECK(q.trigger("Ranger_Camouflage", 1.0) == Refusal::None, "(a second Ranger: Camouflage is pressed)");
    q.run_to(3.0);
    CHECK(q.pawn.effect(effect) != nullptr, "it is on");
    CHECK(q.trigger("General_Guard", 3.0) == Refusal::None, "another ability is pressed");
    q.run_to(3.6);
    CHECK(q.pawn.effect(effect) == nullptr && near(q.pawn.stat(kStealthLevel), 25.0, 1e-3), "the action ended it");
    CHECK(q.pawn.cooldown(ability) && near(q.pawn.cooldown(ability)->initial_cooldown, 60.0, 1e-3), "and started the 60 s cooldown (the ability's text: ending it triggers it; its own 1 s is long over)");
}
AOC_REGISTER_TEST(test_camouflage_toggle);
