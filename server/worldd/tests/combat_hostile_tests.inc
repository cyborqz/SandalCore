// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_hostile_tests.inc: the hostile cases of the combat world (a hit on a stranger, NaN and huge values, a miss, inspect, the kill switch)
//  (registered with AOC_REGISTER_TEST: test_worldd_combat.cpp runs it through aoc_tests::run; it is included from combat_registered.inc)
// ============================================================================

void test_hostile(Fixture& f) {
    std::printf("hostile cases\n");
    Rig rig(4, 20, {});
    if (!rig.ok()) return;
    combat_rig::World w;
    if (!w.problem.empty()) return;
    std::string wolf_path;
    for (const auto& [path, row] : f.motion.rows()) {
        if (path.find("/Beast/Canine/Wolf.Default__Wolf_C") != std::string::npos) wolf_path = path;
    }
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion);
    const uint64_t wolf = w.spawn(wolf_path, 0, 0, 10);
    combat_rig::FakePlayer player(rig, 77, {300, 0, 1098});
    world.add_player(&player);
    combat::Resolved r;
    r.mods.push_back({1, content::kGuidHealth, -5.0});
    CHECK(!world.apply_hit_to_npc(12345, 77, r, 1.0).applied, "a hit on an actor that is not there applies to nothing");
    CHECK(world.fighter_of(12345) == nullptr && world.fighter_of(1) == nullptr, "no fighter for a stranger");
    // NaN and infinite values do not make the target's Health a number that is not
    combat::Resolved nan;
    nan.mods.push_back({1, content::kGuidHealth, std::nan("")});
    const combat::Delivery dn = world.apply_hit_to_npc(wolf, 77, nan, 1.0);
    CHECK(dn.applied && std::isfinite(world.health_of(wolf)) && world.health_of(wolf) > 0, "a NaN hit leaves Health a number");
    combat::Resolved heal;
    heal.mods.push_back({1, content::kGuidHealth, 1.0e9});
    world.apply_hit_to_npc(wolf, 77, heal, 1.0);
    CHECK(near(world.health_of(wolf), world.max_health_of(wolf), 1e-3), "a heal stops at the maximum");
    // a miss does nothing but start the fight
    combat::Resolved miss;
    miss.flags = stats::kHitMiss;
    const double hp = world.health_of(wolf);
    const combat::Delivery dm = world.apply_hit_to_npc(wolf, 77, miss, 2.0);
    CHECK(dm.applied && !dm.lethal && world.health_of(wolf) == hp && (dm.flags & stats::kHitMiss), "a miss costs no Health");
    const combat::TargetInfo ti = world.inspect(wolf, player.fighter());
    CHECK(ti.exists && ti.alive && ti.relation == combat::Relation::Hostile && ti.surface_distance_cm > 100 && ti.surface_distance_cm < 300 && ti.in_line_of_sight,
          "inspect: a living enemy 3 m away in sight");
    CHECK(!world.inspect(999, player.fighter()).exists, "inspect: no such actor");
    // the kill switch
    combat::CombatWorldConfig off;
    off.enabled = false;
    combat::CombatWorld quiet(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion, off);
    quiet.add_player(&player);
    for (double t = 0; t < 5; t += 0.1) quiet.tick(t);
    CHECK(quiet.combatants() == 0, "with combat off nothing thinks");
}

AOC_REGISTER_TEST(test_hostile);
