// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_unreachable_tests.inc: a creature whose target stands where no path reaches (the test world's platform, 5 m up) gives up when the seconds it
//  has tried to find a way reach the check's 5 s, walks home and is healed to full; two targets and a second fight; a way lost for a moment; a target in
//  reach after a walk that found no way (included by combat_registered.inc)
// ============================================================================

// the program's CHECK prints only a failure; this test prints its own passing lines too, so that a Green run shows each check
#define CHECK_OK(cond, msg) do { if (cond) { std::printf("  [ok ] %s\n", msg); ++g_pass; } else { std::printf("  [FAIL] %s\n", msg); ++g_fail; } } while (0)

void test_unreachable_target_leashes(Fixture& f) {
    std::printf("a creature whose target cannot be reached: it gives up, walks home and is healed\n");
    Rig rig(4, 25, {});
    CHECK_OK(rig.ok(), ("the rig: " + rig.problem).c_str());
    if (!rig.ok()) return;
    combat_rig::World w;
    if (!w.problem.empty()) return;
    const std::string wolf_path = wolf_archetype(f);
    CHECK_OK(!wolf_path.empty(), "the Blueprint table has the Wolf");
    if (wolf_path.empty()) return;
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion);
    combat_rig::FakePlayer player(rig, 77, {8000, -2800, 1098});
    world.add_player(&player);
    const uint64_t wolf = w.spawn(wolf_path, 8000, -3000, 10, 0);
    CHECK_OK(wolf != 0, "a level-10 Wolf stands on the ground at (80, -30) m");
    if (wolf == 0) return;

    double now = 0;
    const auto tick = [&](double seconds) {
        const double end = now + seconds;
        while (now < end) {
            now += 1.0 / 15.0;
            w.npcs->note_player(77, player.rig_fighter().position().x, player.rig_fighter().position().y, now);
            w.npcs->tick(now);
            world.tick(now);
        }
    };
    const auto health = [&](int64_t stat) -> double {
        combat::Fighter* fg = world.fighter_of(wolf);
        const stats::StatValue* v = fg ? fg->stats().find(stat) : nullptr;
        return v ? static_cast<double>(v->value) : -1.0;
    };
    const double max_health = health(content::kGuidMaxHealth);
    CHECK_OK(max_health > 0, "the Wolf has a maximum Health");
    tick(0.5);
    // a hit starts the fight and takes some Health
    combat::Resolved r;
    r.mods.push_back({1, content::kGuidHealth, -40.0});
    world.apply_hit_to_npc(wolf, 77, r, now);
    tick(0.5);
    const world::WorldActor* a = w.registry.find(wolf);
    CHECK_OK(a && combat_rig::combat_flag(*a, "bInCombat") && health(content::kGuidHealth) < max_health, "the Wolf is in the fight, hurt");
    const double hurt_health = health(content::kGuidHealth);

    // the player steps onto the platform (x 8,000..10,000, y -6,000..-4,000, 5 m above the ground): no path to him
    player.rig_fighter().move_to({9000, -5000, 1540});
    tick(4.6);
    a = w.registry.find(wolf);
    CHECK_OK(a && combat_rig::combat_flag(*a, "bInCombat") && health(content::kGuidHealth) == hurt_health,
          "4.6 s with no path: it still fights and its Health is still down");
    tick(1.2);
    a = w.registry.find(wolf);
    CHECK_OK(a && !combat_rig::combat_flag(*a, "bInCombat"), "5.8 s with no path: it has given up (bInCombat 0)");
    CHECK_OK(health(content::kGuidHealth) == max_health, "and it is healed to full Health");
    CHECK_OK(a && combat_rig::active_target(*a) == 0, "with no active target");
    // it walks home
    tick(3.0);
    a = w.registry.find(wolf);
    CHECK_OK(a && std::hypot(a->x - 8000.0, a->y + 3000.0) < 300.0, "it is back at its home within 3 s");
    CHECK_OK(health(content::kGuidHealth) == max_health, "still at full Health");

    // the same thing twice: the player comes down and the Wolf starts a new fight
    player.rig_fighter().move_to({8200, -3000, 1098});
    tick(1.0);
    a = w.registry.find(wolf);
    CHECK_OK(a && combat_rig::combat_flag(*a, "bInCombat"), "the player comes down: the Wolf fights again");

    // two of a kind: both players on the platform; the Wolf gives up once and does not start a second fight with the other unreachable one
    combat_rig::FakePlayer second(rig, 78, {9500, -5500, 1540});
    world.add_player(&second);
    tick(0.5);
    player.rig_fighter().move_to({9000, -5000, 1540});
    second.rig_fighter().move_to({9500, -5500, 1540});
    tick(7.0);
    a = w.registry.find(wolf);
    CHECK_OK(a && !combat_rig::combat_flag(*a, "bInCombat") && health(content::kGuidHealth) == max_health, "both targets unreachable: it is home and healed, not fighting");
    CHECK_OK(std::hypot(a->x - 8000.0, a->y + 3000.0) < 300.0, "and it stays at home while neither can be reached");
}
AOC_REGISTER_TEST(test_unreachable_target_leashes);

// A way lost for a moment, and a target in reach after a walk that found no way: the creature keeps its fight (nothing is leashed, nothing healed)
void test_unreachable_moments(Fixture& f) {
    std::printf("a creature whose way to its target is lost for a moment keeps the fight\n");
    Rig rig(4, 25, {});
    CHECK_OK(rig.ok(), ("the rig: " + rig.problem).c_str());
    if (!rig.ok()) return;
    combat_rig::World w;
    if (!w.problem.empty()) return;
    const std::string wolf_path = wolf_archetype(f);
    if (wolf_path.empty()) return;
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion);
    combat_rig::FakePlayer player(rig, 77, {8000, -2800, 1098});
    world.add_player(&player);
    const uint64_t wolf = w.spawn(wolf_path, 8000, -3000, 10, 0);
    CHECK_OK(wolf != 0, "a level-10 Wolf stands on the ground at (80, -30) m");
    if (wolf == 0) return;
    double now = 0;
    const auto tick = [&](double seconds) {
        const double end = now + seconds;
        while (now < end) {
            now += 1.0 / 15.0;
            if (static_cast<int>(now * 15.0) % 15 == 0) rig.set_current(content::kGuidHealth, rig.max_health());   // (the player lives on: the fight is the point)
            w.npcs->note_player(77, player.rig_fighter().position().x, player.rig_fighter().position().y, now);
            w.npcs->tick(now);
            world.tick(now);
        }
    };
    tick(0.5);
    combat::Resolved r;
    r.mods.push_back({1, content::kGuidHealth, -40.0});
    world.apply_hit_to_npc(wolf, 77, r, now);
    tick(0.5);
    const world::WorldActor* a = w.registry.find(wolf);
    CHECK_OK(a && combat_rig::combat_flag(*a, "bInCombat"), "the Wolf is in the fight");
    const uint64_t resets_before = world.stats().resets;

    // the player is on the platform for 2 s (no path) and comes down: the way is back, the count of the 2 s falls, the fight goes on
    player.rig_fighter().move_to({9000, -5000, 1540});
    tick(2.0);
    player.rig_fighter().move_to({8200, -3000, 1098});
    tick(12.0);
    a = w.registry.find(wolf);
    CHECK_OK(a && combat_rig::combat_flag(*a, "bInCombat") && world.stats().resets == resets_before, "2 s with no way and then 12 s with one: the Wolf has not given up");

    // one look finds no way and the player is then within reach of the Wolf where it stands: no walk is ordered any more, and the fight is not given up
    player.rig_fighter().move_to({9000, -5000, 1540});
    tick(0.8);
    player.rig_fighter().move_to({8000, -2800, 1098});
    tick(12.0);
    a = w.registry.find(wolf);
    CHECK_OK(a && combat_rig::combat_flag(*a, "bInCombat") && world.stats().resets == resets_before,
             "a way lost for 0.8 s, the player then in reach for 12 s: the Wolf has still not given up");
    CHECK_OK(a && std::hypot(a->x - 8000.0, a->y + 3000.0) < 600.0, "(and it is still where the fight is, not walking home)");
}
AOC_REGISTER_TEST(test_unreachable_moments);
