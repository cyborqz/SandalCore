// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/ability_npc_pets_holds_tests.inc: what a pet puts on a creature ends with the pet
//  (registered with AOC_REGISTER_TEST: test_worldd_ability_npc.cpp runs it through aoc_tests::run; it is included from ability_npc_registered.inc)
//
//  The Hunter's Incapacitate puts a creature to sleep under its owner's name.  The sleep is the pet's: it ends when the pet is dismissed or dies, and what the owner put on the same creature himself
//  stays.
// ============================================================================

void test_pet_holds_end_with_the_pet() {
    PetTestData shared;
    if (!shared.ok) return;
    std::string error;
    std::printf("what a pet puts on a creature ends when the pet is dismissed or dies, and what its owner put there himself stays\n");
    for (const int how : {0, 1}) {   // 0: the pet is dismissed (its session takes it away), 1: it dies
        Rig r(6, 25);
        const auto rows = r.db->combat_rows(error);
        std::optional<combat::CombatData> data;
        if (rows) data = combat::CombatData::build(*rows, *r.ctx->bundle, error);
        combat_rig::World w;
        CHECK(r.ok() && data && w.problem.empty(), "(the world of the pet's hold)");
        if (!r.ok() || !data || !w.problem.empty()) return;
        const pets::PetAbility* use = nullptr;
        for (const pets::PetAbility& candidate : pets::kPetAbilities) {
            const abilities::AbilityDef* a = r.catalog->ability(candidate.ability);
            if (a && a->row.name.find("Incapacitate") != std::string::npos && candidate.ability != pets::kHunterStanceSwap) {
                use = &candidate;
                break;
            }
        }
        const pets::Definition* def = use ? pets::for_effect(use->pet_effect) : nullptr;
        CHECK(use && def, "the Hunter's Incapacitate is among the pets' abilities");
        if (!use || !def) return;
        combat::NpcStatBook book(*r.ctx->bundle, *r.ctx->gear);
        combat::CombatWorld cw(w.registry, w.npcs.get(), &navfix::shared_nav(), *data, *r.catalog, book, shared.motion);
        combat_rig::FakePlayer player(r, 77, {0, 0, 1098});
        cw.add_player(&player);
        const uint64_t pet = w.spawn(def->archetype, 200, 0, 25, 1.0);
        const uint64_t prey = w.spawn(shared.wolf_path, 500, 0, 25, 1.0);
        CHECK(pet && prey, "(the pet and a creature)");
        if (!pet || !prey) return;
        double now = 1.0;
        const auto run = [&](double seconds) {
            for (double t = 0; t < seconds; t += 0.1) {
                now += 0.1;
                r.set_current(content::kGuidHealth, r.pawn.stat(content::kGuidMaxHealth));
                r.take();
                w.npcs->note_player(77, 0, 0, now);
                w.npcs->tick(now);
                cw.tick(now);
            }
        };
        cw.fighter_of(prey);
        CHECK(cw.pet_use(pet, 77, use->ability, prey, def->reach_cm), "the pet takes the Incapacitate");
        for (int i = 0; i < 80 && cw.pet_casting(pet); ++i) run(0.1);
        const int asleep_before = cw.effect_stacks(prey, pets::kHunterIncapMelee);
        CHECKF(asleep_before == 1, "the pet puts the creature to sleep (%d)", asleep_before);
        if (asleep_before != 1) continue;
        // the owner's own root on the same creature (his, not the pet's)
        effects::Applied root;
        root.def = r.catalog->effect_named("Status_Rooted");
        root.duration = 60.0;
        CHECK(root.def != nullptr, "the data has the root");
        if (!root.def) return;
        cw.apply_effects(prey, 77, {root}, now);
        CHECKF(cw.effect_stacks(prey, root.def->row.guid) == 1 && cw.effect_stacks(prey, pets::kHunterIncapMelee) == 1, "the owner's root is on it beside the pet's sleep (root %d, sleep %d)",
               cw.effect_stacks(prey, root.def->row.guid), cw.effect_stacks(prey, pets::kHunterIncapMelee));
        if (how == 0) {
            cw.pet_gone(pet);   // (what the session does when the player dismisses it)
        } else {
            CHECK(cw.gm_kill(pet, now, error), "the pet dies");
        }
        CHECKF(cw.effect_stacks(prey, pets::kHunterIncapMelee) == 0, "%s: the sleep it put on the creature is off in the same call (%d)", how == 0 ? "dismissed" : "dead", cw.effect_stacks(prey, pets::kHunterIncapMelee));
        CHECKF(cw.effect_stacks(prey, root.def->row.guid) == 1, "...and the owner's own root stays (%d)", cw.effect_stacks(prey, root.def->row.guid));
    }
}
AOC_REGISTER_TEST(test_pet_holds_end_with_the_pet);

// The Guardian's Taunt puts Demoralized on what it shouts at and, when Demoralized is on already, Humiliated.  A creature the owner demoralized and humiliated himself holds ONE of each (the records keep one
// instance): the pet's Taunt refreshes the Humiliated, and it stays the owner's: the pet's dismissal takes off what the pet put on, not what the owner did.
void test_pet_refresh_keeps_the_owners_hold() {
    PetTestData shared;
    if (!shared.ok) return;
    std::string error;
    std::printf("a pet that refreshes an effect its owner put on a creature does not make it the pet's: the effect stays when the pet is dismissed\n");
    Rig r(6, 25);
    const auto rows = r.db->combat_rows(error);
    std::optional<combat::CombatData> data;
    if (rows) data = combat::CombatData::build(*rows, *r.ctx->bundle, error);
    combat_rig::World w;
    CHECK(r.ok() && data && w.problem.empty(), "(the world of the pet's Taunt)");
    if (!r.ok() || !data || !w.problem.empty()) return;
    const pets::PetAbility* use = nullptr;
    for (const pets::PetAbility& candidate : pets::kPetAbilities) {
        const abilities::AbilityDef* a = r.catalog->ability(candidate.ability);
        if (a && a->row.name.find("Taunt") != std::string::npos) {
            use = &candidate;
            break;
        }
    }
    const pets::Definition* def = use ? pets::for_effect(use->pet_effect) : nullptr;
    CHECK(use && def, "the Guardian's Taunt is among the pets' abilities");
    if (!use || !def) return;
    combat::NpcStatBook book(*r.ctx->bundle, *r.ctx->gear);
    combat::CombatWorld cw(w.registry, w.npcs.get(), &navfix::shared_nav(), *data, *r.catalog, book, shared.motion);
    combat_rig::FakePlayer player(r, 77, {0, 0, 1098});
    cw.add_player(&player);
    const uint64_t pet = w.spawn(def->archetype, 200, 0, 25, 1.0);
    const uint64_t prey = w.spawn(shared.wolf_path, 500, 0, 25, 1.0);
    CHECK(pet && prey, "(the pet and a creature)");
    if (!pet || !prey) return;
    double now = 1.0;
    cw.fighter_of(prey);
    effects::Applied demoralized, humiliated;
    demoralized.def = r.catalog->effect_named("Status_Demoralized");
    humiliated.def = r.catalog->effect_named("Status_Humiliated");
    CHECK(demoralized.def && humiliated.def, "the data has Demoralized and Humiliated");
    if (!demoralized.def || !humiliated.def) return;
    demoralized.duration = humiliated.duration = 60.0;
    const int64_t hum = humiliated.def->row.guid;
    cw.apply_effects(prey, 77, {demoralized, humiliated}, now);   // (the owner's own: 60 s each)
    CHECKF(cw.effect_stacks(prey, demoralized.def->row.guid) == 1 && cw.effect_stacks(prey, hum) == 1 && cw.effect_remaining(prey, hum, now) > 59.0, "the owner demoralizes and humiliates the creature for 60 s (%d, %d, %.1f s)",
           cw.effect_stacks(prey, demoralized.def->row.guid), cw.effect_stacks(prey, hum), cw.effect_remaining(prey, hum, now));
    CHECK(cw.pet_use(pet, 77, use->ability, prey, def->reach_cm), "the pet takes the Taunt");
    for (int i = 0; i < 80 && cw.pet_casting(pet); ++i) {
        now += 0.1;
        w.npcs->note_player(77, 0, 0, now);
        w.npcs->tick(now);
        cw.tick(now);
    }
    const double left = cw.effect_remaining(prey, hum, now);
    CHECKF(cw.effect_stacks(prey, hum) == 1 && left < 30.0, "the Taunt refreshes the same Humiliated: still one, with the pet's shorter time (%d, %.1f s)", cw.effect_stacks(prey, hum), left);
    if (cw.effect_stacks(prey, hum) != 1 || !(left < 30.0)) return;
    cw.pet_gone(pet);   // (what the session does when the player dismisses it)
    CHECKF(cw.effect_stacks(prey, hum) == 1 && cw.effect_stacks(prey, demoralized.def->row.guid) == 1, "the pet is dismissed: what the owner put on stays (Humiliated %d, Demoralized %d)", cw.effect_stacks(prey, hum),
           cw.effect_stacks(prey, demoralized.def->row.guid));
}
AOC_REGISTER_TEST(test_pet_refresh_keeps_the_owners_hold);
