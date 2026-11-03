// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/ability_npc_pets_target_tests.inc: whom a Summoner's pet may play an ability at, asked before the owner pays; what a pet's ability costs its owner; Attack at an enemy and at a friend
//  (registered with AOC_REGISTER_TEST: test_worldd_ability_npc.cpp runs it through aoc_tests::run; it is included from ability_npc_registered.inc)
// ============================================================================

/// The path of the first Blueprint of the table whose path holds `needle`; empty when none does.
static std::string blueprint_path_holding(const npc::MotionTable& motion, const char* needle) {
    for (const auto& [p, row] : motion.rows()) {
        if (p.find(needle) != std::string::npos) return p;
    }
    return {};
}

void test_pet_target_sides() {
    PetTestData shared;
    if (!shared.ok) return;
    std::string error;
    std::printf("whom a pet's ability may be played at, asked before the owner pays\n");
    Rig r(6, 25);
    const auto rows = r.db->combat_rows(error);
    std::optional<combat::CombatData> data;
    if (rows) data = combat::CombatData::build(*rows, *r.ctx->bundle, error);
    combat_rig::World w;
    CHECK(r.ok() && data && w.problem.empty(), "(the world of the pet's targets)");
    if (!r.ok() || !data || !w.problem.empty()) return;
    combat::NpcStatBook book(*r.ctx->bundle, *r.ctx->gear);
    combat::CombatWorld cw(w.registry, w.npcs.get(), &navfix::shared_nav(), *data, *r.catalog, book, shared.motion);
    combat_rig::FakePlayer player(r, 77, {0, 0, 1098});
    cw.add_player(&player);
    const std::string friend_path = blueprint_path_holding(shared.motion, "Quest_NPC_Master.Default__Quest_NPC_Master_C");
    CHECK(!friend_path.empty(), "(a questgiver, who is no enemy of a player, is in the Blueprint table)");
    if (friend_path.empty()) return;
    const pets::Definition& guardian = pets::kDefinitions[0];
    const uint64_t pet = w.spawn(guardian.archetype, 200, 0, 25, 1.0);
    const uint64_t enemy = w.spawn(shared.wolf_path, 900, 0, 25, 1.0);
    const uint64_t friendly = w.spawn(friend_path, 0, 900, 25, 0.0);
    const uint64_t far_enemy = w.spawn(shared.wolf_path, 6500, 0, 25, 1.0);   // (farther from the owner than a pet's fight is held to: 5,000 cm)
    const uint64_t dead_enemy = w.spawn(shared.wolf_path, 0, -900, 25, 1.0);
    CHECK(pet && enemy && friendly && far_enemy && dead_enemy && cw.pet_summoned(pet, 77), "(the pet, an enemy, a friend, a far enemy and one that will be dead)");
    if (!pet || !enemy || !friendly || !far_enemy || !dead_enemy) return;
    std::string why;
    double now = 1.0;
    CHECK(cw.gm_kill(dead_enemy, now, why), "(a dead enemy)");
    const uint64_t missing = 987654321;   // no actor of the registry
    const uint64_t owner = 77;

    // each ability by what its record says: the mask of the sides it reaches, and whether it asks for a target.  The rule is the pipeline's (a creature the owner may harm needs the Enemy bit,
    // any other target needs Self, Ally, Party or Raid) and the target is an actor of the registry that is alive.
    struct Row {
        int64_t     ability;
        const char* name;
        bool        at_enemy, at_friend, at_owner, at_pet, at_none, at_dead, at_far, at_missing;
    };
    const Row table[] = {
        // Enemy only: Taunt, Eviscerate, Lunge, Incapacitate, Essence Eruption (with no one named they are played where the pet stands, as the pipeline casts a player's at the air)
        {pets::kGuardianTaunt, "Taunt",                 true,  false, false, false, true,  false, false, false},
        {6064633577430779319ll, "Eviscerate",           true,  false, false, false, true,  false, false, false},
        {pets::kHunterLunge, "Lunge",                   true,  false, false, false, true,  false, false, false},
        {pets::kHunterIncapacitate, "Incapacitate",     true,  false, false, false, true,  false, false, false},
        {6064633577433793980ll, "Essence Eruption",     true,  false, false, false, true,  false, false, false},
        // Self and Ally: Recreate and Cleanse reach the owner or the pet, never an enemy
        {6064633577432614330ll, "Recreate",             false, true,  true,  true,  true,  false, false, false},
        {6064634103529603072ll, "Cleanse",              false, true,  true,  true,  true,  false, false, false},
        // Self, Enemy and Ally: Stomp is played around the pet whatever it is aimed at
        {6064633577427633586ll, "Stomp",                true,  true,  true,  true,  true,  false, false, false},
        // Self only: Brace
        {6064634403600334848ll, "Brace",                false, true,  true,  true,  true,  false, false, false},
    };
    for (const Row& row : table) {
        const struct { const char* at; uint64_t target; bool expected; } cases[] = {
            {"an enemy", enemy, row.at_enemy},      {"a friend", friendly, row.at_friend}, {"its owner", owner, row.at_owner},   {"the pet", pet, row.at_pet},
            {"no one", 0, row.at_none},             {"a dead enemy", dead_enemy, row.at_dead}, {"a far enemy", far_enemy, row.at_far}, {"no actor", missing, row.at_missing},
        };
        for (const auto& c : cases) {
            std::string refused;
            const bool ok = cw.pet_can_use(pet, owner, row.ability, c.target, &refused);
            CHECKF(ok == c.expected, "%s at %s is %s (it %s)%s%s", row.name, c.at, c.expected ? "played" : "refused", ok ? "was played" : "was refused", ok ? "" : ": ", ok ? "" : refused.c_str());
        }
    }
    // asking changes nothing: no combatant is made for anyone it named, and the pet starts nothing
    CHECK(cw.health_of(friendly) == -1.0 && cw.health_of(far_enemy) == -1.0 && cw.health_of(enemy) == -1.0, "asking made no combatant of a friend, a far enemy or an enemy");
    CHECK(!cw.pet_casting(pet) && !cw.pet_fighting(pet), "... and the pet started nothing");
    CHECK(!cw.pet_can_use(pet, owner, 987654, enemy) && !cw.pet_can_use(pet, 5555, pets::kGuardianTaunt, enemy), "an ability that is not there, or an owner that is not there, is refused");
    CHECK(!cw.pet_can_use(dead_enemy, owner, pets::kGuardianTaunt, enemy), "a dead pet plays nothing");

    // the enemy that is asked about is played at: the cast starts and its hit lands
    cw.fighter_of(enemy);
    CHECK(cw.pet_can_use(pet, owner, pets::kGuardianTaunt, enemy) && cw.pet_use(pet, owner, pets::kGuardianTaunt, enemy, pets::kGuardianTauntReach) && cw.pet_casting(pet), "Taunt at the enemy is played");
    for (int i = 0; i < 80 && cw.pet_casting(pet); ++i) {
        now += 0.1;
        w.npcs->note_player(77, 0, 0, now);
        w.npcs->tick(now);
        cw.tick(now);
    }
    CHECK(cw.effect_stacks(enemy, 6064630758760448234ll) == 1, "... and demoralizes the enemy");

    // Attack: an enemy is gone for, a friend and what is not there are not
    const int64_t attack = guardian.attack;
    CHECK(cw.pet_attack(pet, owner, attack, enemy), "Attack at an enemy: the pet goes");
    cw.pet_stop(pet);
    CHECK(!cw.pet_attack(pet, owner, attack, friendly), "Attack at a friend: the pet does not go");
    CHECK(!cw.pet_attack(pet, owner, attack, missing), "Attack at no actor: the pet does not go");
    CHECK(!cw.pet_attack(pet, owner, attack, dead_enemy), "Attack at a dead enemy: the pet does not go");
    CHECK(!cw.pet_fighting(pet), "... and fights nothing");
    CHECK(cw.health_of(friendly) == -1.0, "Attack at a friend made no combatant of it");
}
AOC_REGISTER_TEST(test_pet_target_sides);

void test_pet_payment_split() {
    std::printf("what a pet's ability costs its owner: asked first, paid after\n");
    Rig r(6, 25);
    CHECK(r.ok(), "(the Summoner rig loads)");
    if (!r.ok()) return;
    double t = 5.0;
    r.pipe->refresh(t, "(the start)");
    r.take();
    r.set_current(pets::kEssence, 100.0);
    r.take();
    const double full = r.pawn.stat(pets::kEssence);
    AbilityPipeline::PetPayment pay;
    CHECK(r.pipe->can_pay_pet_ability(pets::kGuardianTaunt, t, pay), "Taunt can be paid for with 100 Essence");
    r.take();
    CHECK(pay.costs.size() == 1 && pay.costs[0].first == pets::kEssence && pay.costs[0].second < 0.0, "its cost is Essence, and one line of it");
    CHECK(std::abs(pay.cooldown - 10.0) < 0.01, "its cooldown is the record's 10 s");
    CHECK(r.pawn.stat(pets::kEssence) == full && r.pipe->charges_left(pets::kGuardianTaunt) == -1, "asking pays nothing and starts no cooldown");
    // asked a second time: still nothing
    CHECK(r.pipe->can_pay_pet_ability(pets::kGuardianTaunt, t, pay), "asked again, it can still be paid for");
    r.take();
    CHECK(r.pawn.stat(pets::kEssence) == full && r.pipe->charges_left(pets::kGuardianTaunt) == -1, "... and nothing is paid then either");
    // the commit pays once, and starts the cooldown once
    r.pipe->commit_pet_payment(pets::kGuardianTaunt, pay, t);
    r.take();
    CHECK(std::abs(r.pawn.stat(pets::kEssence) - (full + pay.costs[0].second)) < 0.01, "the commit takes the Essence once");
    CHECK(r.pipe->charges_left(pets::kGuardianTaunt) == 0, "... and starts the cooldown, where the owner's bar reads it");
    // too little Essence: asking says no, and nothing is paid
    r.set_current(pets::kEssence, 5.0);
    r.take();
    t += 20.0;
    r.pipe->refresh(t, "(the cooldown is over)");
    r.take();
    const double little = r.pawn.stat(pets::kEssence);
    AbilityPipeline::PetPayment none;
    CHECK(little < 15.0 && !r.pipe->can_pay_pet_ability(pets::kGuardianTaunt, t, none), "with less than 15 Essence Taunt cannot be paid for");
    r.take();
    CHECK(r.pawn.stat(pets::kEssence) == little && r.pipe->charges_left(pets::kGuardianTaunt) == -1, "... and nothing was paid or started");
    // the one-step payment is both: pay_pet_ability pays when it can and says its cooldown
    r.set_current(pets::kEssence, 100.0);
    r.take();
    double seconds = 0;
    CHECK(r.pipe->pay_pet_ability(pets::kGuardianTaunt, t, seconds) && std::abs(seconds - 10.0) < 0.01, "pay_pet_ability pays and says the cooldown");
    r.take();
    CHECK(r.pawn.stat(pets::kEssence) < 100.0 && r.pipe->charges_left(pets::kGuardianTaunt) == 0, "... Essence taken and the cooldown running");
    // the costs of several cost hits are added up by stat, so that the Essence is tested once against the whole
    const auto total = pets::sum_by_stat({{pets::kEssence, -30.0}, {pets::kEssence, -30.0}, {111, -1.0}});
    CHECK(total.size() == 2 && total[0].first == pets::kEssence && total[0].second == -60.0 && total[1].first == 111 && total[1].second == -1.0, "two cost lines of 30 on one stat are a cost of 60");
    CHECK(50.0 + total[0].second < 0.0, "so that 50 Essence does not pay for two costs of 30");
    CHECK(pets::sum_by_stat({}).empty(), "no cost lines are no cost");
}
AOC_REGISTER_TEST(test_pet_payment_split);
