// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_tree_clock_tests.inc
//
//  Registered tests of test_worldd_combat: the clock of a creature's state tree.  Some moves wait for "more than 12 seconds" on the tree's own clock, which runs from the time the creature
//  was put into the world, not from the start of its fight (the tree's cooldowns read the same clock), so a creature that has stood in the world for a while may use them the moment its fight begins.
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================

// The Wolf's tree is cut to its Bite alone under a wait on the tree's clock; the montages and the targeting are the real tables.  The rig's world clock starts at 0 with the fight, so the tree's
// clock reads the same seconds as the fight's here: the creature's tree has run since the world began.
void test_creature_waits_for_its_tree_clock(Fixture& f) {
    std::printf("a creature whose only animated attack waits on its tree's clock swings its single swing until the clock allows the attack, and plays it after; one whose attack is open only early plays it early and swings after\n");
    namespace fs = std::filesystem;
    combat::NpcAbilityData real;
    if (!load_creature_tables(real)) return;
    const std::string wolf_path = creature_path(f, "/Beast/Canine/Wolf.Default__Wolf_C");
    const abilities::AbilityDef* bite = f.catalog->ability_named("Wolf_Bite");
    CHECK(!wolf_path.empty() && f.motion.find(wolf_path) && real.abilities_of(wolf_path) && bite, "the Blueprint table has the Wolf, the creature tables list its abilities and the ability data has the Bite");
    if (wolf_path.empty() || !f.motion.find(wolf_path) || !real.abilities_of(wolf_path) || !bite) return;
    std::string dir = NPC_MOTION_PATH;
    dir.erase(dir.find_last_of("/\\") + 1);
    std::error_code ec;
    const fs::path tmp = fs::temp_directory_path() / ("aoc_creature_tree_clock_" + unique_temp_suffix());   // (a folder of its own: two gates can run this program at once)
    fs::create_directories(tmp, ec);
    const std::string abilities_header =
        "archetype\tability\ttask\tcooldown\tstop_movement\torder\tmin_distance\tmax_distance\tmax_angle\tgates\tunknown\tfight\tconds\twhy\tstarts\trotate\tarrival";
    struct Run {
        bool   ran = false, loaded = false;
        double first_cast = -1, last_cast = -1, first_hit = -1;   // seconds into the fight: the first bite played, the last, the first hit that reached the player
        size_t casts = 0, hits_before_5s = 0, hits = 0;
    };
    // a 16 s fight against a player who stands 8 m from the Wolf, whose tree is the Bite alone under `conds`
    const auto play = [&](const char* conds) {
        Run r;
        const std::string table = (tmp / "abilities.tsv").string();
        {
            std::ofstream out(table, std::ios::binary);
            out << abilities_header << "\n" << wolf_path << "\t" << bite->row.guid << "\t\t0\t1\t37\t-1\t270\t45\tGlobal Cooldown\t0\t1\t" << conds << "\t\tGlobal Cooldown=0.5\t360:0\t-1\n";
        }
        combat::NpcAbilityData tables;
        std::string err;
        r.loaded = tables.load(table, dir + "npc_montages.tsv", dir + "npc_targeting.tsv", err);
        CHECKF(r.loaded, "the Wolf's tree with its Bite under '%s' loads (%s)", conds, err.c_str());
        if (!r.loaded) return r;
        std::vector<LungeSample> samples;
        std::vector<combat::IncomingHit> got;
        r.ran = run_lunge_fight(f, tables, wolf_path, 16.0, 0, 800, nullptr, samples, &got, false, -1, nullptr, 40000.0);
        if (!r.ran) return r;
        for (const LungeSample& s : samples) {
            if (!s.cast_began) continue;
            ++r.casts;
            if (r.first_cast < 0) r.first_cast = s.t;
            r.last_cast = s.t;
        }
        for (const combat::IncomingHit& h : got) {
            ++r.hits;
            if (r.first_hit < 0) r.first_hit = h.at;
            if (h.at < 5.0) ++r.hits_before_5s;
        }
        return r;
    };
    const Run late = play("c0:tt(ge,6)");
    const Run early = play("c0:tt(lt,6)");
    fs::remove_all(tmp, ec);
    CHECK(late.loaded && early.loaded && late.ran && early.ran, "the two fights run (the Wolf is made from its open and knows the Bite)");
    if (!late.loaded || !early.loaded || !late.ran || !early.ran) return;
    std::printf("  Bite under tt(ge,6): first bite played at %.2f s, %zu hits in the first 5 s, %zu bites in 16 s;  under tt(lt,6): first bite at %.2f s, last at %.2f s, %zu bites in 16 s\n", late.first_cast, late.hits_before_5s,
                late.casts, early.first_cast, early.last_cast, early.casts);
    // the Bite waits for the tree's sixth second: until then the Wolf swings its single swing (its hits reach the player), and the bite is played once the clock has opened
    CHECKF(late.hits_before_5s >= 2 && late.first_hit >= 0 && late.first_hit < 5.0, "the Wolf whose Bite waits for its tree's sixth second swings in the meantime (%zu hits in the first 5 s)", late.hits_before_5s);
    CHECKF(late.first_cast >= 6.0 && late.first_cast < 12.0 && late.casts >= 1, "... and plays the Bite once the clock allows it, never before (first bite at %.2f s, %zu bites in 16 s)", late.first_cast, late.casts);
    // a Bite that is open only while the tree is younger than six seconds is played at the start and never after: the creature swings once the clock has run out
    CHECKF(early.first_cast > 0 && early.first_cast < 4.0 && early.casts >= 1, "the Wolf whose Bite is open only in its tree's first six seconds plays it from the start (first bite at %.2f s)", early.first_cast);
    CHECKF(early.last_cast < 6.2 && early.hits >= 4, "... plays no bite once the clock has passed six seconds, and swings instead (last bite at %.2f s, %zu hits in 16 s)", early.last_cast, early.hits);
}

AOC_REGISTER_TEST(test_creature_waits_for_its_tree_clock);

// The Skeleton's axe swipe waits for the tree's clock to pass twelve seconds.  A Skeleton that has stood in the world long enough plays it from the start of its fight; the rig's fight begins at
// the world's first second, so the swipe comes at twelve.
void test_skeleton_plays_its_axe_swipe_once_its_tree_is_twelve_seconds_old(Fixture& f) {
    std::printf("a Skeleton against a player who stands still plays its axe swipe once its tree has run twelve seconds, and swings its axe before\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string path = creature_path(f, "/Undead/Skeleton/Skeleton.Default__Skeleton_C");
    const abilities::AbilityDef* swipe = f.catalog->ability_named("SkeletonWarrior_AxeSwipe");
    CHECK(!path.empty() && f.motion.find(path) && tables.abilities_of(path) && swipe, "the Blueprint table has the Skeleton, the creature tables list its abilities and the ability data has the axe swipe");
    if (path.empty() || !f.motion.find(path) || !tables.abilities_of(path) || !swipe) return;
    BanditOpen made;   // (the open the world makes for a creature of a generic Blueprint: its tree's abilities are the records it loads)
    CHECKF(make_bandit_open(f, tables, path, made), "the world makes an open for the Skeleton (verdict: %s)", spawns::verdict_name(made.why));
    if (!made.open) return;
    std::vector<LungeSample> samples;
    std::vector<combat::IncomingHit> got;
    if (!run_lunge_fight(f, tables, path, 45.0, 0, 800, nullptr, samples, &got, false, -1, nullptr, 40000.0, made.open)) return;
    std::vector<double> swipes;   // when each cast of the swipe began
    for (const LungeSample& s : samples) {
        if (s.cast_began && s.ability == swipe->row.guid) swipes.push_back(s.t);
    }
    size_t other_casts = 0;
    double first_other = -1;
    for (const LungeSample& s : samples) {
        if (!s.cast_began || s.ability == swipe->row.guid) continue;
        ++other_casts;
        if (first_other < 0) first_other = s.t;
    }
    std::printf("  axe swipe cast at:");
    for (const double t : swipes) std::printf(" %.2f", t);
    std::printf(" s; %zu other casts, the first at %.2f s; %zu hits reached the player\n", other_casts, first_other, got.size());
    CHECKF(other_casts >= 3 && first_other >= 0 && first_other < 8.0, "the Skeleton fights from the start with the moves it had before (%zu other casts, the first at %.2f s)", other_casts, first_other);
    CHECKF(!swipes.empty(), "the axe swipe is played in a 45 second fight (%zu casts)", swipes.size());
    CHECKF(swipes.empty() || swipes.front() >= 12.0, "... and never before its tree has run twelve seconds (the first at %.2f s)", swipes.empty() ? -1.0 : swipes.front());
}

AOC_REGISTER_TEST(test_skeleton_plays_its_axe_swipe_once_its_tree_is_twelve_seconds_old);

// The condition itself: the table's text for the tree's clock reads, holds by the seconds the tree has run, counts as a clock (a creature whose moves all wait on clocks swings its single swing meanwhile),
// holds for a creature that only stands ready, and a text that is no clock condition does not read.
void test_tree_clock_condition_reads_and_decides(Fixture& f) {
    std::printf("the tree's clock condition reads from the table, holds by the seconds the tree has run, counts as a clock, and a text that is no clock condition does not load\n");
    namespace fs = std::filesystem;
    const std::string wolf_path = creature_path(f, "/Beast/Canine/Wolf.Default__Wolf_C");
    const abilities::AbilityDef* bite = f.catalog->ability_named("Wolf_Bite");
    CHECK(!wolf_path.empty() && bite, "the Blueprint table has the Wolf and the ability data has the Bite");
    if (wolf_path.empty() || !bite) return;
    std::string dir = NPC_MOTION_PATH;
    dir.erase(dir.find_last_of("/\\") + 1);
    std::error_code ec;
    const fs::path tmp = fs::temp_directory_path() / ("aoc_creature_tree_clock_atom_" + unique_temp_suffix());   // (a folder of its own: two gates can run this program at once)
    fs::create_directories(tmp, ec);
    const std::string abilities_header =
        "archetype\tability\ttask\tcooldown\tstop_movement\torder\tmin_distance\tmax_distance\tmax_angle\tgates\tunknown\tfight\tconds\twhy\tstarts\trotate\tarrival";
    const auto load = [&](const char* conds, combat::NpcAbilityData& tables, std::string& err) {
        const std::string table = (tmp / "abilities.tsv").string();
        {
            std::ofstream out(table, std::ios::binary);
            out << abilities_header << "\n" << wolf_path << "\t" << bite->row.guid << "\t\t0\t1\t37\t-1\t270\t45\tGlobal Cooldown\t0\t1\t" << conds << "\t\tGlobal Cooldown=0.5\t360:0\t-1\n";
        }
        return tables.load(table, dir + "npc_montages.tsv", dir + "npc_targeting.tsv", err);
    };
    // three clock conditions in one list: the tree has run at least 3 s, under 9 s, and not exactly 5 s
    combat::NpcAbilityData tables;
    std::string err;
    const bool loaded = load("c0:tt(ge,3) a0:tt(lt,9) a0:tt(ne,5)", tables, err);
    CHECKF(loaded, "a list of three tree clock conditions loads (%s)", err.c_str());
    if (loaded) {
        const std::vector<combat::CreatureAbility>* tasks = tables.abilities_of(wolf_path);
        CHECK(tasks && tasks->size() == 1 && (*tasks)[0].groups.size() == 1 && (*tasks)[0].groups[0].size() == 3, "the task holds one group of three conditions");
        if (tasks && tasks->size() == 1 && (*tasks)[0].groups.size() == 1 && (*tasks)[0].groups[0].size() == 3) {
            using K = combat::CondAtom::Kind;
            const combat::CondGroup& g = (*tasks)[0].groups[0];
            CHECK(g[0].atom.kind == K::TreeTime && g[0].atom.op == combat::CondAtom::Ge && g[0].atom.value == 3.0, "the first reads as the tree's clock, at least 3");
            CHECK(g[1].atom.kind == K::TreeTime && g[1].atom.op == combat::CondAtom::Lt && g[1].atom.value == 9.0, "the second reads as the tree's clock, under 9");
            CHECK(g[2].atom.kind == K::TreeTime && g[2].atom.op == combat::CondAtom::Ne && g[2].atom.value == 5.0, "the third reads as the tree's clock, not 5");
            combat::CondContext c;
            c.has_target = true;
            c.fight_seconds = 100.0;   // (the fight's clock says nothing: only the tree's clock decides)
            const auto holds = [&](double tree_seconds) {
                c.tree_seconds = tree_seconds;
                return combat::eval_group(g, c);
            };
            CHECK(!holds(2.0) && holds(3.0) && holds(4.0) && !holds(5.0) && holds(8.9) && !holds(9.0) && !holds(40.0), "it holds from 3 s up to 9 s of the tree's clock, except at 5 s (and not by the fight's clock)");
            c.preview = true;
            c.tree_seconds = 0.0;
            CHECK(combat::eval_group(g, c), "a creature that only stands ready (no target, nothing yet to count) holds every clock condition");
            c.preview = false;
            c.timers_only = true;
            c.tree_seconds = 2.0;
            CHECK(!combat::eval_group(g, c), "asked about the clocks alone, a young tree keeps the task shut");
            c.tree_seconds = 4.0;
            CHECK(combat::eval_group(g, c), "... and an older one opens it");
            CHECK(combat::is_timer_atom(K::TreeTime) && combat::is_timer_atom(K::FightTime) && !combat::is_timer_atom(K::Cooldown), "the tree's clock is a clock, as the fight's is, and a cooldown is not");
        }
    }
    // text that is no clock condition (an operator that does not exist, a missing number) does not load: the strict reading of the table
    {
        combat::NpcAbilityData bad_op, bad_count;
        std::string e1, e2;
        CHECK(!load("c0:tt(zz,3)", bad_op, e1) && e1.find("the column conds holds") != std::string::npos, "an operator that is none of eq, ne, lt, le, gt, ge does not load");
        CHECK(!load("c0:tt(ge)", bad_count, e2) && e2.find("the column conds holds") != std::string::npos, "a clock condition without its number does not load");
    }
    fs::remove_all(tmp, ec);
}

AOC_REGISTER_TEST(test_tree_clock_condition_reads_and_decides);
