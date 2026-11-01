// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_fire_notify_tests.inc: a creature ability counts as one that shoots where its cast flies a projectile
//  (registered with AOC_REGISTER_TEST: test_worldd_combat.cpp runs it through aoc_tests::run; it is included from combat_registered.inc)
// ============================================================================

// A creature that shoots closes in on its target to the arrival distance of its tree and casts from its shot reach (CombatWorld::launches_projectiles decides which abilities are shots). The cast flies
// a projectile where a fire notify of the montage names a projectile row of the ability by its key; an ability whose notify names a key that no projectile row has flies nothing, and a creature that
// stood off for it would repeat a cast that shows nothing. Both now follow the same rule, for every ability of the creature tables.
void test_creature_shots_follow_the_fire_notify(Fixture& f) {
    std::printf("a creature ability counts as a shot where a fire notify of its montage names a projectile row of the ability\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    combat_rig::World w;
    if (!w.problem.empty()) return;
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion);
    world.set_ability_data(&tables);
    size_t checked = 0, shots = 0, other_notify = 0, differ = 0;
    std::set<int64_t> seen;
    for (const auto& [archetype, tasks] : tables.by_kind()) {
        for (const combat::CreatureAbility& task : tasks) {
            if (!seen.insert(task.ability).second) continue;
            const abilities::AbilityDef* def = f.catalog->ability(task.ability);
            if (!def) continue;
            // the rule the cast flies by (CombatWorld::plan_cast): a fire notify of a montage of the ability, and a projectile row of the ability with the notify's key whose projectile is in the data
            bool notifies = false, flies = false;
            for (const content::MontageRow& m : def->montages) {
                const combat::MontageData* montage = m.soft_path.empty() ? nullptr : tables.montage(m.soft_path);
                if (!montage) continue;
                for (const combat::MontageData::Fire& fire : montage->fires) {
                    notifies = true;
                    for (const content::AbilityHitRow& r : def->hits) flies = flies || (r.source == "projectile" && r.key == fire.key && f.catalog->projectile(r.owner_guid) != nullptr);
                }
            }
            ++checked;
            shots += flies ? 1 : 0;
            other_notify += (notifies && !flies) ? 1 : 0;
            differ += world.launches_projectiles(*def) != flies ? 1 : 0;
        }
    }
    std::printf("  %zu abilities of the creature tables: %zu fly a projectile, %zu have a fire notify whose key no projectile row has\n", checked, shots, other_notify);
    CHECK(checked > 0 && shots > 0, "the creature tables list abilities, and some of them fly a projectile");
    CHECKF(differ == 0, "the answer of the world agrees with the cast's rule for every one of them (%zu differ)", differ);
}
AOC_REGISTER_TEST(test_creature_shots_follow_the_fire_notify);
