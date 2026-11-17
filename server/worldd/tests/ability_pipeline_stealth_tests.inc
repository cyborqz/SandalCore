// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/ability_pipeline_stealth_tests.inc: what ends a stealth effect, and what ending it leaves behind
//
//    Rogue_Stealth     an offensive ability ends it through its Reaction_Stealth_AbilityBreak; Guard, which the reaction names as always allowed, does not
//    Camouflage        the damage that ends it starts the 60 s cooldown the ability's text promises, as an action that ends it does (the effect's End hit)
//
//  (registered with AOC_REGISTER_TEST: test_worldd_ability_pipeline.cpp runs it through aoc_tests::run; it is included from ability_pipeline_registered.inc)
// ============================================================================

/// The Rogue's stealth is taken off by an offensive ability (the reaction's trigger is every ability but the ones it lists), and not by the ones it lists.
void test_rogue_stealth_ends_on_an_action() {
    std::printf("Rogue_Stealth: an offensive ability ends it, Guard (an ability it allows) does not\n");
    for (const char* name : {"Rogue_Stab", "Rogue_Kick", "General_Guard"}) {
        Rig r(5, 25);   // a level-25 Rogue
        CHECKF(r.ok(), "the Rogue rig: %s", r.problem.c_str());
        if (!r.ok()) return;
        r.pipe->set_data_timings(true);
        r.know({"Rogue_Stealth", name});
        const int64_t stealth = r.effect("Rogue_Stealth");
        CHECKF(r.trigger("Rogue_Stealth", 1.0) == Refusal::None, "%s: Stealth is pressed", name);
        r.run_to(3.0);
        CHECKF(r.pawn.effect(stealth) != nullptr, "%s: Stealth is on", name);
        const size_t fired = r.pipe->reactions_fired();
        CHECKF(r.trigger(name, 3.0, true) == Refusal::None, "%s: the ability is accepted", name);
        r.run_to(4.0);
        const bool allowed = std::strcmp(name, "General_Guard") == 0;
        CHECKF((r.pawn.effect(stealth) != nullptr) == allowed, "%s: Stealth is %s", name, allowed ? "still on (the reaction allows it)" : "off (an offensive ability fades it)");
        CHECKF((r.pipe->reactions_fired() > fired) == !allowed, "%s: the reaction %s", name, allowed ? "does not fire" : "fires");
    }
}
AOC_REGISTER_TEST(test_rogue_stealth_ends_on_an_action);

/// Damage that ends Camouflage is an end like the press that ends it: the effect's End hit runs and the ability's 60 s cooldown starts (the ability's text: "This will trigger a
/// 60 second cooldown"); a hit that took no Health ends nothing and starts nothing.
void test_camouflage_ended_by_damage_starts_the_cooldown() {
    std::printf("Camouflage: the damage that ends it starts the 60 s cooldown\n");
    Rig r(1, 25);   // a level-25 Ranger
    CHECKF(r.ok(), "the Ranger rig: %s", r.problem.c_str());
    if (!r.ok()) return;
    r.pipe->set_data_timings(true);
    r.know({"Ranger_Camouflage"});
    const int64_t effect = r.effect("Ranger_Camouflage");
    const int64_t ability = r.ability("Ranger_Camouflage");
    CHECK(r.trigger("Ranger_Camouflage", 1.0) == Refusal::None, "Camouflage is pressed");
    r.run_to(10.0);
    CHECK(r.pawn.effect(effect) != nullptr && near(r.pawn.stat(kStealthLevel), 20.0, 1e-3), "9 s later it is on (the stealth level 25 less its 5), and the ability's own 1 s cooldown is over");
    combat::IncomingHit mana;
    mana.mods.push_back({1, content::kGuidMana, -10.0});
    r.pipe->receive_hit(mana, 10.0);
    r.take();
    CHECK(r.pawn.effect(effect) != nullptr, "a hit that takes Mana only ends nothing");
    combat::IncomingHit hit;
    hit.mods.push_back({2, content::kGuidHealth, -5.0});
    r.pipe->receive_hit(hit, 11.0);
    r.run_to(11.5);
    CHECK(r.pawn.effect(effect) == nullptr && near(r.pawn.stat(kStealthLevel), 25.0, 1e-3), "a hit that takes Health ends it, and the stealth level is the character's 25 again");
    CHECK(r.pawn.cooldown(ability) && near(r.pawn.cooldown(ability)->initial_cooldown, 60.0, 1e-3), "...and starts the 60 s cooldown (the effect's End hit: the ability's 1 s is long over)");
}
AOC_REGISTER_TEST(test_camouflage_ended_by_damage_starts_the_cooldown);
