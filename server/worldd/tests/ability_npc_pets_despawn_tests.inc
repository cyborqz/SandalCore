// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/ability_npc_pets_despawn_tests.inc: a game master's hands on a Summoner's pet: taking it out of the world and killing it
//  (registered with AOC_REGISTER_TEST: test_worldd_ability_npc.cpp runs it through aoc_tests::run; it is included from ability_npc_registered.inc)
// ============================================================================

/// Whether the world's players list holds a port for this actor.
static bool port_of_actor(const combat::CombatWorld& cw, uint64_t actor) {
    for (const combat::PlayerPort* p : cw.players()) {
        if (p->actor_id() == actor) return true;
    }
    return false;
}

void test_pet_despawn_and_kill() {
    PetTestData shared;
    if (!shared.ok) return;
    std::string error;
    std::printf("a game master takes a pet out of the world, or kills it\n");
    Rig r(6, 25);
    const auto rows = r.db->combat_rows(error);
    std::optional<combat::CombatData> data;
    if (rows) data = combat::CombatData::build(*rows, *r.ctx->bundle, error);
    combat_rig::World w;
    CHECK(r.ok() && data && w.problem.empty(), "(the world of the game master's hands)");
    if (!r.ok() || !data || !w.problem.empty()) return;
    combat::NpcStatBook book(*r.ctx->bundle, *r.ctx->gear);
    combat::CombatWorld cw(w.registry, w.npcs.get(), &navfix::shared_nav(), *data, *r.catalog, book, shared.motion);
    combat_rig::FakePlayer player(r, 77, {0, 0, 1098});
    cw.add_player(&player);
    uint64_t next_guid = 1;
    combat::Aftermath after(w.registry, cw, nullptr, [&] {
        aoc::protocol::emit::FIntrepidNetworkGUID g;
        g.ObjectId = 9000000 + next_guid++;
        return g;
    });
    after.install();   // (a death makes a corpse and its loot, as in the world)
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
    const pets::Definition& guardian = pets::kDefinitions[0];
    const uint64_t other = w.spawn(shared.wolf_path, 1500, 0, 25, 0.0);   // (an ordinary creature, no enemy of anyone here: it aggros nothing)
    cw.fighter_of(other);

    // taken out of the world through the aftermath, as the game master's despawn does: the pet's port is gone from the players' list before its combatant is
    const uint64_t pet = w.spawn(guardian.archetype, 200, 0, 25, 1.0);
    CHECK(pet && cw.pet_summoned(pet, 77) && cw.is_pet(pet) && port_of_actor(cw, pet) && cw.pet_of(77) == pet, "(a pet on its owner's side, with its port in the players' list)");
    run(0.5);
    double seconds = 0;
    CHECK(after.despawn(pet, now, true, &seconds), "the game master's despawn takes the pet out of the world");
    CHECK(!cw.is_pet(pet) && !port_of_actor(cw, pet) && cw.pet_of(77) == 0, "its port is gone from the players' list, with its owner and its fight");
    CHECK(cw.health_of(pet) == -1.0, "... and its combatant is forgotten");
    run(2.0);   // (a tick that reads every port: with a port left over, it would read a combatant that is gone)
    CHECK(port_of_actor(cw, 77) && cw.health_of(other) > 0.0, "the world ticks on: the owner and the other creature are as they were");

    // an ordinary creature taken out of the world changes nothing about a pet that stands beside it
    const uint64_t pet2 = w.spawn(guardian.archetype, 200, 100, 25, 1.0);
    CHECK(pet2 && cw.pet_summoned(pet2, 77) && port_of_actor(cw, pet2), "(a second pet)");
    CHECK(after.despawn(other, now, true, &seconds), "an ordinary creature is taken out of the world");
    CHECK(cw.is_pet(pet2) && port_of_actor(cw, pet2) && cw.pet_of(77) == pet2, "... and the pet is still on its owner's side");
    run(0.5);

    // killed by a game master: dead for its session to see, with no corpse and no loot, and the session's dismissal takes its port
    CHECK(cw.gm_kill(pet2, now, error), "the game master's kill takes the pet's Health");
    CHECK(cw.dead(pet2) && after.corpse_of(pet2) == nullptr, "the pet is dead and leaves no corpse");
    run(2.0);
    CHECK(cw.is_pet(pet2) && cw.dead(pet2), "(until its session sees it dead, it stands in the world's tables)");
    cw.pet_gone(pet2);   // (what the session does when it sees the pet dead)
    CHECK(!cw.is_pet(pet2) && !port_of_actor(cw, pet2) && cw.pet_of(77) == 0, "the session's dismissal takes its port");
    run(1.0);

    // a creature killed the same way leaves its corpse as before
    const uint64_t prey = w.spawn(shared.wolf_path, 1500, 300, 25, 0.0);
    cw.fighter_of(prey);
    CHECK(cw.gm_kill(prey, now, error) && after.corpse_of(prey) != nullptr, "an ordinary creature a game master kills still leaves its corpse");
    run(0.5);

    // a pet that is killed while it fights is forgotten without a trace as well
    const uint64_t pet3 = w.spawn(guardian.archetype, 200, -100, 25, 1.0);
    const uint64_t enemy = w.spawn(shared.wolf_path, 600, -100, 25, 1.0);
    CHECK(pet3 && enemy && cw.pet_summoned(pet3, 77) && cw.pet_attack(pet3, 77, guardian.attack, enemy) && cw.pet_fighting(pet3), "(a pet that fights an enemy)");
    CHECK(after.despawn(pet3, now, true, &seconds), "the game master takes the fighting pet out of the world");
    CHECK(!cw.pet_fighting(pet3) && !cw.is_pet(pet3) && !port_of_actor(cw, pet3), "its fight is over and its port is gone");
    run(3.0);
    CHECK(cw.health_of(enemy) > 0.0, "the enemy it fought lives on, and the world ticks on");
}
AOC_REGISTER_TEST(test_pet_despawn_and_kill);
