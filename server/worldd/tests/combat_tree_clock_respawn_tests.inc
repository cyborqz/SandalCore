// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_tree_clock_respawn_tests.inc
//
//  Registered tests of test_worldd_combat: a creature's tree clock begins when the creature is put into the world.  A creature that is made again after its death is a new actor with a new tree,
//  so a move that waits for its tree to have run N seconds waits N seconds from the respawn, not from the start of the world.
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================

// The Wolf's tree is cut to its Bite alone under a wait on the tree's clock (six seconds).  A first Wolf stands in the world from the start; at the twentieth second it is taken out and a second one is
// put in its place, as a respawn does.  The first plays its Bite from the sixth second on; the second must not play one before it has stood in the world for six seconds.
void test_respawned_creature_waits_for_its_tree_clock_again(Fixture& f) {
    std::printf("a creature put into the world at the twentieth second waits for its tree's sixth second from then on: its first Bite comes after the twenty-sixth second of the world, not at once\n");
    namespace fs = std::filesystem;
    combat::NpcAbilityData real;
    if (!load_creature_tables(real)) return;
    const std::string wolf_path = creature_path(f, "/Beast/Canine/Wolf.Default__Wolf_C");
    const abilities::AbilityDef* bite = f.catalog->ability_named("Wolf_Bite");
    const auto bp = f.motion.find(wolf_path);
    CHECK(!wolf_path.empty() && bp && real.abilities_of(wolf_path) && bite, "the Blueprint table has the Wolf, the creature tables list its abilities and the ability data has the Bite");
    if (wolf_path.empty() || !bp || !real.abilities_of(wolf_path) || !bite) return;
    std::string dir = NPC_MOTION_PATH;
    dir.erase(dir.find_last_of("/\\") + 1);
    std::error_code ec;
    const fs::path tmp = fs::temp_directory_path() / ("aoc_creature_respawn_clock_" + unique_temp_suffix());   // (a folder of its own: two gates can run this program at once)
    fs::create_directories(tmp, ec);
    const std::string abilities_header =
        "archetype\tability\ttask\tcooldown\tstop_movement\torder\tmin_distance\tmax_distance\tmax_angle\tgates\tunknown\tfight\tconds\twhy\tstarts\trotate\tarrival";
    const std::string table = (tmp / "abilities.tsv").string();
    {
        std::ofstream out(table, std::ios::binary);
        out << abilities_header << "\n" << wolf_path << "\t" << bite->row.guid << "\t\t0\t1\t37\t-1\t270\t45\tGlobal Cooldown\t0\t1\tc0:tt(ge,6)\t\tGlobal Cooldown=0.5\t360:0\t-1\n";
    }
    combat::NpcAbilityData tables;
    std::string err;
    const bool loaded = tables.load(table, dir + "npc_montages.tsv", dir + "npc_targeting.tsv", err);
    fs::remove_all(tmp, ec);
    CHECKF(loaded, "the Wolf's tree with its Bite under 'c0:tt(ge,6)' loads (%s)", err.c_str());
    if (!loaded) return;

    Rig rig(4, 20, {});
    if (!rig.ok()) return;
    combat_rig::World w;
    if (!w.problem.empty()) return;
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorldConfig cfg;
    cfg.seed = 5;
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion, cfg);
    world.set_ability_data(&tables);
    const uint64_t first = w.spawn(wolf_path, 0, 0, 7, 900, bp->half_height, nullptr, &*bp);
    CHECK(first != 0, "a Wolf is made from the Wolf's capture");
    if (first == 0) return;
    double px = 0, py = 800;
    combat_rig::FakePlayer player(rig, 77, {px, py, 1098});
    world.add_player(&player);
    const double dt = 1.0 / 15.0, t_respawn = 20.0, t_end = 40.0;
    uint64_t second = 0;
    GremCastView last_first, last_second;
    double first_cast_of_first = -1, first_cast_of_second = -1;
    double now = 0;
    while (now < t_end) {
        now += dt;
        if (second == 0 && now >= t_respawn) {
            // the death and the respawn: the creature goes out of the world and a new one, with a new id, is made at the same place
            world.forget(first);
            w.registry.remove(first);
            second = w.spawn(wolf_path, 0, 0, 7, 900, bp->half_height, nullptr, &*bp);
            CHECK(second != 0 && second != first, "the respawned Wolf is a new actor");
            if (second == 0) return;
        }
        if (const stats::StatValue* max_hp = rig.cs->find(content::kGuidMaxHealth)) rig.cs->set_current(content::kGuidHealth, max_hp->value);
        w.npcs->note_player(77, px, py, now);
        w.npcs->tick(now);
        world.tick(now);
        if (second == 0) {
            const GremCastView view = grem_cast_view(w.registry, first);
            if (view.montage != 0 && (view.element != last_first.element || last_first.montage == 0) && view.ability == bite->row.guid && first_cast_of_first < 0) first_cast_of_first = now;
            last_first = view;
        } else {
            const GremCastView view = grem_cast_view(w.registry, second);
            if (view.montage != 0 && (view.element != last_second.element || last_second.montage == 0) && view.ability == bite->row.guid && first_cast_of_second < 0) first_cast_of_second = now;
            last_second = view;
        }
    }
    std::printf("  the first Wolf played its Bite at %.2f s; the Wolf put in at %.1f s played its first Bite at %.2f s\n", first_cast_of_first, t_respawn, first_cast_of_second);
    CHECKF(first_cast_of_first >= 6.0 && first_cast_of_first < t_respawn, "the Wolf that stood in the world from the start plays its Bite once its tree is six seconds old (at %.2f s)", first_cast_of_first);
    CHECKF(first_cast_of_second >= t_respawn + 6.0 && first_cast_of_second < t_end, "the Wolf put in at the twentieth second plays its first Bite only after its own tree has run six seconds, not at once (at %.2f s)", first_cast_of_second);
}

AOC_REGISTER_TEST(test_respawned_creature_waits_for_its_tree_clock_again);
