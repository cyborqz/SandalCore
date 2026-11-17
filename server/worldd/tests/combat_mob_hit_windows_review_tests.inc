// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_mob_hit_windows_review_tests.inc
//
//  Registered tests of test_worldd_combat, after the review of the hit-window moves: the Launch's impact montage is entered when the launch's root motion is obstructed (the game's
//  movement event of that name), not at the end of the montage's travel, and not at all when nothing obstructed the launch; a hit-window state that allows several hits lands them
//  at its own interval; the column's interval reads, and a table with a field it cannot read is refused.
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================

namespace mob_windows {

/// The branch event the creature table names for the montage a creature's ability enters when its root motion is obstructed.
const char* const kBranchEvent = "obstructed";

}  // namespace mob_windows

// R2 (the game's condition): the Launch's conditional branch is bound to the movement event RootMotionObstructed.  The creature is held where it is by the freeze box ahead of it (a body
// in it), the client plays the impact montage then, and its two hit notifies land the impact's hits at their own times after that: not at the end of the montage's travel, which a launch
// that was held by the player has long passed.
void test_the_launch_lands_its_impact_when_its_root_motion_is_obstructed(Fixture& f) {
    std::printf("the Mycellette's Launch lands its impact hits at the notifies of the impact montage, counted from the moment the player obstructs the launch\n");
    combat::NpcAbilityData tables;
    if (!mob_windows::load_with_moves(f, {{mob_windows::kMycelletteLaunch, false, "", true, 60, false, "", false, "", std::string(mob_windows::kBranchEvent) + ":" + mob_windows::kLaunchImpact}}, {},
                                      "the creature tables with the Launch's obstruction branch load", tables))
        return;
    const std::string path = creature_path(f, mob_windows::kMycellettePath);
    MOVE_CHECK(!path.empty() && f.motion.find(path), "the Blueprint table has the Mycellette");
    if (path.empty() || !f.motion.find(path)) return;
    BanditOpen made;
    MOVE_CHECK(mob_windows::make_open(f, tables, path, made), "the world makes an open for the Mycellette");
    if (!made.open) return;
    std::vector<LungeSample> samples;
    std::vector<combat::IncomingHit> hits;
    if (!run_lunge_fight(f, tables, path, 20.0, 0, 500, nullptr, samples, &hits, false, -1, nullptr, 40000.0, made.open)) return;
    double cast_at = -1, obstructed_at = -1;
    for (const mob_windows::CastAt& c : mob_windows::casts_of(samples)) {
        if (c.ability == mob_windows::kMycelletteLaunch) {
            cast_at = c.t;
            break;
        }
    }
    for (const LungeSample& s : samples) {
        if (cast_at > 0 && s.t >= cast_at && s.obstructed == 1) {
            obstructed_at = s.t;
            break;
        }
    }
    MOVE_CHECKF(cast_at >= 12.0, "the Mycellette plays the Launch's montage from the 12 s its tree waits (%.2f s)", cast_at);
    if (cast_at <= 0) return;
    MOVE_CHECKF(obstructed_at > cast_at && obstructed_at < cast_at + 1.2, "the player standing ahead of the launch obstructs its root motion %.3f s after the cast, before the montage's travel is over", obstructed_at - cast_at);
    const combat::IncomingHit* first = mob_windows::first_hit(hits, mob_windows::kMycelletteImpactFirst);
    const combat::IncomingHit* second = mob_windows::first_hit(hits, mob_windows::kMycelletteImpactSecond);
    MOVE_CHECKF(first && mob_windows::hits_of(hits, mob_windows::kMycelletteImpactFirst) == 1, "the first impact hit lands once (%zu)", mob_windows::hits_of(hits, mob_windows::kMycelletteImpactFirst));
    MOVE_CHECKF(first && obstructed_at > 0 && first->at - obstructed_at > 0 && first->at - obstructed_at <= 0.14, "it lands at the impact montage's 0.038 s notify after the obstruction (%.3f s after it)",
                first && obstructed_at > 0 ? first->at - obstructed_at : -1.0);
    MOVE_CHECKF(first && first->at - cast_at < 1.2, "... which is before the 1.27 s the end of the montage's travel would give (%.3f s after the cast)", first ? first->at - cast_at : -1.0);
    MOVE_CHECKF(second && mob_windows::hits_of(hits, mob_windows::kMycelletteImpactSecond) == 1, "the second impact hit lands once (%zu)", mob_windows::hits_of(hits, mob_windows::kMycelletteImpactSecond));
    MOVE_CHECKF(second && obstructed_at > 0 && second->at - obstructed_at >= 0.28 && second->at - obstructed_at <= 0.41, "... at the 0.284 s notify after the obstruction (%.3f s after it)",
                second && obstructed_at > 0 ? second->at - obstructed_at : -1.0);
    MOVE_CHECKF(mob_windows::hits_of(hits, mob_windows::kMycelletteLaunchOnly) == 0, "the launch montage's own key-2 row lands nothing (%zu)", mob_windows::hits_of(hits, mob_windows::kMycelletteLaunchOnly));
}

AOC_REGISTER_TEST(test_the_launch_lands_its_impact_when_its_root_motion_is_obstructed);

// R2, the other half: a launch nothing obstructs plays out its travel and lands no impact (the branch's event never happens).  The player is taken out of the launch's way as it begins.
void test_a_launch_nothing_obstructs_lands_no_impact(Fixture& f) {
    std::printf("the Mycellette's Launch lands no impact when the player is out of its way and nothing obstructs its root motion\n");
    combat::NpcAbilityData tables;
    if (!mob_windows::load_with_moves(f, {{mob_windows::kMycelletteLaunch, false, "", true, 60, false, "", false, "", std::string(mob_windows::kBranchEvent) + ":" + mob_windows::kLaunchImpact}}, {},
                                      "the creature tables with the Launch's obstruction branch load", tables))
        return;
    const std::string path = creature_path(f, mob_windows::kMycellettePath);
    MOVE_CHECK(!path.empty() && f.motion.find(path), "the Blueprint table has the Mycellette");
    if (path.empty() || !f.motion.find(path)) return;
    BanditOpen made;
    MOVE_CHECK(mob_windows::make_open(f, tables, path, made), "the world makes an open for the Mycellette");
    if (!made.open) return;
    bool moved = false;
    const LungeSteer steer = [&](double, const LungeSample* prev, double& px, double& py, const world::WorldActor&) {
        if (moved || !prev || !prev->cast_began || prev->ability != mob_windows::kMycelletteLaunch) return false;
        px = 0;
        py = 4000;   // (40 m away, far out of the launch's 15 m of travel and of the impact's box)
        moved = true;
        return true;
    };
    std::vector<LungeSample> samples;
    std::vector<combat::IncomingHit> hits;
    if (!run_lunge_fight(f, tables, path, 20.0, 0, 500, steer, samples, &hits, false, -1, nullptr, 40000.0, made.open)) return;
    double cast_at = -1;
    for (const mob_windows::CastAt& c : mob_windows::casts_of(samples)) {
        if (c.ability == mob_windows::kMycelletteLaunch) {
            cast_at = c.t;
            break;
        }
    }
    MOVE_CHECKF(cast_at >= 12.0 && moved, "the Mycellette plays the Launch (%.2f s) and the player is taken out of its way", cast_at);
    if (cast_at <= 0) return;
    bool obstructed = false;
    for (const LungeSample& s : samples) obstructed = obstructed || (s.t >= cast_at && s.t <= cast_at + 3.0 && s.obstructed == 1);
    MOVE_CHECK(!obstructed, "nothing obstructs the launch's root motion");
    const size_t impacts = mob_windows::hits_of(hits, mob_windows::kMycelletteImpactFirst) + mob_windows::hits_of(hits, mob_windows::kMycelletteImpactSecond);
    MOVE_CHECKF(impacts == 0, "and the impact montage's hits land nowhere (%zu)", impacts);
}

AOC_REGISTER_TEST(test_a_launch_nothing_obstructs_lands_no_impact);

// R1 ("once per target unless the data says otherwise"): a hit-window state that allows several hits (bEnableMultipleHits) lands the hit again on the same player every HitIntervalInSeconds
// while he stays in reach.  The Pollen Swipe stands where it is; its window 0.5 to 1.5 s with an interval of 0.4 s hits the player who stays three times, not once.
void test_a_window_that_allows_several_hits_lands_them_at_its_interval(Fixture& f) {
    std::printf("a hit window with an interval hits the player who stays in reach again and again, at that interval\n");
    combat::NpcAbilityData tables;
    if (!mob_windows::load_with_moves(f, {{mob_windows::kPollenSwipe, true, "c0:T", true, 60, true, "", true, ""}}, {{mob_windows::kPollenMontage, "0.5:1.5:0:0.4"}},
                                      "the creature tables with the Pollen Swipe's window and its interval load", tables))
        return;
    const std::string path = creature_path(f, mob_windows::kArcanePath);
    MOVE_CHECK(!path.empty() && f.motion.find(path), "the Blueprint table has the Arcane Bloomerias");
    if (path.empty() || !f.motion.find(path)) return;
    BanditOpen made;
    MOVE_CHECK(mob_windows::make_open(f, tables, path, made), "the world makes an open for the Arcane Bloomerias");
    if (!made.open) return;
    std::vector<LungeSample> samples;
    std::vector<combat::IncomingHit> hits;
    if (!run_lunge_fight(f, tables, path, 15.0, 0, 200, nullptr, samples, &hits, false, -1, nullptr, 40000.0, made.open)) return;
    double cast_at = -1;
    for (const mob_windows::CastAt& c : mob_windows::casts_of(samples)) {
        if (c.ability == mob_windows::kPollenSwipe) {
            cast_at = c.t;
            break;
        }
    }
    MOVE_CHECK(cast_at > 0, "the Arcane Bloomerias plays its Pollen Swipe");
    if (cast_at <= 0) return;
    std::vector<double> at;
    for (const combat::IncomingHit& h : hits) {
        if (h.hit_guid == mob_windows::kPollenSwipeHit && h.at >= cast_at && h.at <= cast_at + 1.69) at.push_back(h.at - cast_at);   // (the montage is 1.7 s long: the Swipe's next cast is not this one's)
    }
    std::printf("  the Pollen Swipe at %.2f s: hits at", cast_at);
    for (const double t : at) std::printf(" %.3f", t);
    std::printf(" s after the cast\n");
    MOVE_CHECKF(at.size() == 3, "the player who stays is hit three times by the window of 0.5 to 1.5 s with an interval of 0.4 s (%zu)", at.size());
    bool spaced = at.size() >= 2;
    for (size_t i = 1; i < at.size(); ++i) spaced = spaced && at[i] - at[i - 1] >= 0.399;
    MOVE_CHECK(spaced, "... each at least the interval after the one before");
    MOVE_CHECK(!at.empty() && at.front() >= 0.49 && at.back() <= 1.51, "... all inside the window");
}

AOC_REGISTER_TEST(test_a_window_that_allows_several_hits_lands_them_at_its_interval);

// The window column's fields: "start:end:key" or with the interval after the key; a field the loader cannot read refuses the table (the world never plays half a table).
void test_the_hit_window_column_reads_an_interval_and_refuses_what_it_cannot_read(Fixture& f) {
    std::printf("the hit window column reads an interval after the key and refuses a window it cannot read\n");
    std::string dir = NPC_MOTION_PATH;
    dir.erase(dir.find_last_of("/\\") + 1);
    struct Case {
        const char* windows;
        bool        loads;
        const char* what;
        double      interval;   // what the window reads as (-1: not asked)
    };
    const Case cases[] = {
        {"0.1:1.0:0", true, "a window with no interval", 0.0},
        {"0.1:1.0:0:0.5", true, "a window with an interval of 0.5 s", 0.5},
        {"0.1:1.0:0:0", true, "a window with an interval of 0 (one hit)", 0.0},
        {"0.1:1.0:0:0.5:9", false, "a window with a fifth field", -1.0},
        {"0.1:1.0:0:-0.5", false, "a window with a negative interval", -1.0},
        {"1.0:0.1:0", false, "a window that ends before it begins", -1.0},
    };
    for (const Case& c : cases) {
        std::string abilities, montages, error;
        const bool written = mob_windows::abilities_with(dir, {}, abilities, error) && mob_windows::montages_with(dir, {{mob_windows::kGoblinMontage, c.windows}}, montages, error);
        MOVE_CHECK(written, "the tables with the window are written");
        if (!written) continue;
        combat::NpcAbilityData data;
        const bool loaded = data.load(abilities, montages, dir + "npc_targeting.tsv", error);
        MOVE_CHECKF(loaded == c.loads, "%s: %s (%s)", c.what, loaded ? "the tables load" : "the tables are refused", loaded ? "" : error.c_str());
        if (loaded && c.loads && c.interval >= 0) {
            const combat::MontageData* m = data.montage(mob_windows::kGoblinMontage);
            MOVE_CHECKF(m && m->windows.size() == 1 && std::fabs(m->windows[0].interval - c.interval) < 1e-9, "... and its interval reads as %.1f s (%.3f)", c.interval,
                        m && !m->windows.empty() ? m->windows[0].interval : -1.0);
        }
    }
}

AOC_REGISTER_TEST(test_the_hit_window_column_reads_an_interval_and_refuses_what_it_cannot_read);

// R5, said out loud: the sequence link that lets the Headbutts follow the Rally lets every move of a tree follow the move that starts the cooldown it asks as a condition of its own.  The
// goblin Digger's tree has its Rally and its Rock Throw under the same condition and the same 40 s timer, so its Rock Throw now follows its Rally at once instead of never being asked.
void test_a_digger_throws_its_rock_right_after_its_rally(Fixture& f) {
    std::printf("a Goblin Digger throws its rock right after its Rally, as its tree chains the two\n");
    constexpr int64_t kRally = 6064629687827373911ll, kRockThrow = 6064629659834515632ll;
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string path = creature_path(f, "/GoblinDigger/Goblin_Digger.Default__Goblin_Digger_C");
    MOVE_CHECK(!path.empty() && f.motion.find(path) && tables.abilities_of(path), "the Blueprint table has the Goblin Digger and the ability data has its tree");
    if (path.empty() || !f.motion.find(path) || !tables.abilities_of(path)) return;
    BanditOpen made;
    MOVE_CHECK(mob_windows::make_open(f, tables, path, made), "the world makes an open for the Digger");
    if (!made.open) return;
    std::vector<LungeSample> samples;
    if (!run_lunge_fight(f, tables, path, 60.0, 0, 800, nullptr, samples, nullptr, false, -1, nullptr, 40000.0, made.open)) return;
    double rally_at = -1, rock_after = -1;
    for (const mob_windows::CastAt& c : mob_windows::casts_of(samples)) {
        if (c.ability == kRally && rally_at < 0) rally_at = c.t;
        if (c.ability == kRockThrow && rally_at >= 0 && c.t > rally_at && rock_after < 0) rock_after = c.t - rally_at;
    }
    std::printf("  the first Rally at %.2f s, the first Rock Throw after it %.2f s later\n", rally_at, rock_after);
    MOVE_CHECK(rally_at >= 20.0 && rally_at < 36.0, "the Digger rallies once its fight is 20 to 30 s old");
    MOVE_CHECKF(rock_after > 0 && rock_after < 8.0, "... and throws its rock in the moment its Rally is over (%.2f s after it began)", rock_after);
}

AOC_REGISTER_TEST(test_a_digger_throws_its_rock_right_after_its_rally);
