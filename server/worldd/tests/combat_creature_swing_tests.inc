// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_creature_swing_tests.inc
//
//  Registered tests of test_worldd_combat: how a creature's swing lands: bites, lunges, cones, walking players, facing and turning.
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================

void test_bite_lands_wherever_the_player_stands(Fixture& f) {
    std::printf("a Wolf's bite, whose targeting record measures nothing (validity off): at its notify it lands on its target wherever he stands, in front of it, behind it or far away\n");
    constexpr double kPi = 3.14159265358979323846;
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string wolf_path = creature_path(f, "/Beast/Canine/Wolf.Default__Wolf_C");
    const abilities::AbilityDef* bite = f.catalog->ability_named("Wolf_Bite");
    CHECK(!wolf_path.empty() && f.motion.find(wolf_path) && tables.abilities_of(wolf_path) && bite, "the Blueprint table has the Wolf, the creature tables list its abilities and the ability data has the Bite");
    if (wolf_path.empty() || !f.motion.find(wolf_path) || !tables.abilities_of(wolf_path) || !bite) return;
    const combat::BiteArea* record = tables.area_of(bite->row.guid);
    const combat::RootMotionFreezeRule* freeze = tables.freeze_of(bite->row.guid);
    CHECK(record && record->gate() == combat::BiteArea::Gate::Any && record->discovery == combat::BiteArea::Discovery::PrefersActive && !record->check_validity && record->volume_count == 0 && freeze &&
              freeze->kind == combat::RootMotionFreezeRule::Kind::Pause && freeze->distance > 0,
          "the Bite's targeting record finds the creature's own target, measures nothing and sweeps no volume (an Any record), and its lunge pauses on contact");
    if (!record || !freeze) return;

    // the player stands in front for 30 s; then again, but steps behind the Wolf as each bite begins; then again, but is put 25 m away as each bite begins
    const auto fight = [&](int where, int& bites, int& hits) {
        double flip_at = -1;
        std::vector<LungeSample> samples;
        std::vector<combat::IncomingHit> got;
        const LungeSteer steer = [&](double now, const LungeSample* prev, double& px, double& py, const world::WorldActor& wolf) {
            if (where != 0 && prev && prev->cast_began && prev->ability == bite->row.guid) flip_at = now + 0.1;
            if (flip_at < 0 || now < flip_at) return false;
            flip_at = -1;
            const double d = where == 1 ? -150.0 : 2500.0;   // behind it, or far ahead of it
            px = wolf.x + d * std::cos(wolf.yaw_degrees * kPi / 180.0);
            py = wolf.y + d * std::sin(wolf.yaw_degrees * kPi / 180.0);
            return true;
        };
        if (!run_lunge_fight(f, tables, wolf_path, 30.0, 0, 800, steer, samples, &got, false, -1, nullptr, 40000.0)) return false;   // (a fight that drifts far must not be given up)
        bites = 0;
        hits = 0;
        for (const LungeSample& s : samples) {
            if (s.cast_began && s.ability == bite->row.guid) ++bites;
        }
        for (const combat::IncomingHit& h : got) {
            if (h.source_guid == bite->row.guid) ++hits;
        }
        return true;
    };
    int front_bites = 0, front_hits = 0, behind_bites = 0, behind_hits = 0, far_bites = 0, far_hits = 0;
    const bool a = fight(0, front_bites, front_hits);
    const bool b = fight(1, behind_bites, behind_hits);
    const bool c = fight(2, far_bites, far_hits);
    CHECK(a && b && c, "the three fights run (the Wolf is made from its open and knows the Bite)");
    if (!a || !b || !c) return;
    std::printf("  30 s against a player who stands in front: %d bites, %d reached him; who steps behind the Wolf as each bite begins: %d bites, %d reached him; who is put 25 m away as each bite begins: %d bites, %d "
                "reached him\n", front_bites, front_hits, behind_bites, behind_hits, far_bites, far_hits);
    CHECKF(front_bites >= 5 && front_hits >= front_bites - 1, "every bite of the Wolf reaches a player who stands in front of it (%d of %d; the last one may be still in the air when the fight ends)", front_hits, front_bites);
    CHECKF(behind_bites >= 4 && behind_hits >= behind_bites - 1, "... and one who has stepped behind it when the bite lands (%d of %d bites reached him)", behind_hits, behind_bites);
    CHECKF(far_bites >= 2 && far_hits >= far_bites - 1, "... and one who is 25 m away when it lands (%d of %d bites reached him)", far_hits, far_bites);
}

AOC_REGISTER_TEST(test_bite_lands_wherever_the_player_stands);

void test_fury_lands_only_inside_its_cone(Fixture& f) {
    std::printf("a Grem's Fury (validity on: 300 cm, 60 degrees, line of sight) lands each of its four bites at its notify only while the player is inside its cone, and its Snap (validity off) wherever he stands\n");
    constexpr double kPi = 3.14159265358979323846;
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string rabid_path = creature_path(f, "/Beast/Rabid/Rabid.Default__Rabid_C");
    const abilities::AbilityDef* fury = f.catalog->ability_named("Rabid_Fury");
    const abilities::AbilityDef* snap = f.catalog->ability_named("Rabid_Snap");
    CHECK(!rabid_path.empty() && f.motion.find(rabid_path) && tables.abilities_of(rabid_path) && fury && snap, "the Blueprint table has the Rabid, the creature tables list its abilities and the ability data has the Fury and the Snap");
    if (rabid_path.empty() || !f.motion.find(rabid_path) || !tables.abilities_of(rabid_path) || !fury || !snap) return;
    const combat::BiteArea* fury_area = tables.area_of(fury->row.guid, 1);   // (the Fury's hits are keys 1 and 2; its one record is key 0)
    const combat::BiteArea* snap_area = tables.area_of(snap->row.guid, 0);
    CHECK(fury_area && fury_area->gate() == combat::BiteArea::Gate::Validity && fury_area->discovery == combat::BiteArea::Discovery::OnlyLocked && fury_area->cone_check && fury_area->los_check &&
              fury_area->distance == 300 && fury_area->angle == 60 && fury_area->volume_count == 0,
          "the Fury's record finds the locked target and measures: 300 cm, 60 degrees, the cone and the line of sight");
    CHECK(snap_area && snap_area->gate() == combat::BiteArea::Gate::Any && snap_area->distance == 500 && snap_area->angle == 120 && !snap_area->check_validity,
          "the Snap's record states 500 cm and 120 degrees and has its validity off (an Any record)");
    if (!fury_area || !snap_area) return;

    struct Place {
        const char* name;
        double      distance, bearing;
        bool        fury_lands;
    };
    const Place places[] = {{"220 cm ahead", 220, 0, true}, {"220 cm, 15 deg off", 220, 15, true}, {"220 cm, 50 deg off", 220, 50, false}, {"220 cm behind", 220, 180, false}, {"520 cm ahead", 520, 0, false}};
    for (const Place& place : places) {
        // as each Fury or Snap begins, the player is put there relative to the Grem, and kept there while its montage plays
        std::vector<LungeSample> samples;
        std::vector<combat::IncomingHit> got;
        const LungeSteer steer = [&](double, const LungeSample* prev, double& px, double& py, const world::WorldActor& grem) {
            if (!prev || prev->montage == 0 || (prev->ability != fury->row.guid && prev->ability != snap->row.guid)) return false;
            const double a = (grem.yaw_degrees + place.bearing) * kPi / 180.0;
            px = grem.x + place.distance * std::cos(a);
            py = grem.y + place.distance * std::sin(a);
            return true;
        };
        CHECKF(run_lunge_fight(f, tables, rabid_path, 40.0, 0, 800, steer, samples, &got, false, -1, nullptr, 40000.0), "the fight against a player put %s runs", place.name);   // (a fight that drifts far must not be given up)
        size_t fury_casts = 0, fury_due = 0, fury_landed = 0, snap_casts = 0, snap_due = 0, snap_landed = 0;
        for (const LungeSample& s : samples) {
            if (!s.cast_began || (s.ability != fury->row.guid && s.ability != snap->row.guid)) continue;
            const bool is_fury = s.ability == fury->row.guid;
            (is_fury ? fury_casts : snap_casts) += 1;
            const combat::MontageData* md = montage_of_cast(f, tables, s.ability, s.montage);
            for (const double at : cast_bites(f, tables, s.ability, s.montage)) {
                if (s.t + at <= 40.0 - 0.2) (is_fury ? fury_due : snap_due) += 1;
            }
            size_t in_window = 0;
            for (const combat::IncomingHit& h : got) {
                if (h.source_guid == s.ability && h.at >= s.t - 0.05 && h.at <= s.t + (md ? md->seconds(1.0) : 1.0) + 0.25) {
                    (is_fury ? fury_landed : snap_landed) += 1;
                    ++in_window;
                }
            }
            if (in_window == 0 && (!is_fury || place.fury_lands) && !cast_bites(f, tables, s.ability, s.montage).empty() && s.t + cast_bites(f, tables, s.ability, s.montage).front() <= 40.0 - 0.2) {
                std::printf("    (%s at %.2f s: %zu bites due and none landed; hits of that ability near it:", is_fury ? "Fury" : "Snap", s.t, cast_bites(f, tables, s.ability, s.montage).size());
                for (const combat::IncomingHit& h : got) {
                    if (h.source_guid == s.ability && std::fabs(h.at - s.t) < 2.0) std::printf(" %.2f", h.at);
                }
                std::printf(")\n");
            }
        }
        std::printf("  the player put %-18s: Fury %zu casts, %zu bites due, %zu landed;  Snap %zu casts, %zu bites due, %zu landed\n", place.name, fury_casts, fury_due, fury_landed, snap_casts, snap_due, snap_landed);
        CHECKF(fury_casts >= 2 && fury_due >= 6, "the Grem casts the Fury at a player put %s (%zu casts, %zu bites due)", place.name, fury_casts, fury_due);
        if (place.fury_lands) {
            CHECKF(fury_landed >= fury_due, "every bite of its Fury lands on a player put %s, inside the cone (%zu of %zu)", place.name, fury_landed, fury_due);
        } else {
            CHECKF(fury_landed == 0, "no bite of its Fury lands on a player put %s, outside the cone (%zu of %zu landed)", place.name, fury_landed, fury_due);
        }
        // the Snap's record states 500 cm and 120 degrees but does not measure: it lands wherever he is put
        CHECKF(snap_casts == 0 || snap_landed >= snap_due, "every bite of its Snap lands on a player put %s (%zu of %zu)", place.name, snap_landed, snap_due);
    }
}

AOC_REGISTER_TEST(test_fury_lands_only_inside_its_cone);

void test_lunge_pauses_at_the_player(Fixture& f) {
    std::printf("a Wolf's lunge pauses on contact: held ahead of the player while he is in its box, with bRootMotionObstructed going out; when the box is clear the flag clears and the lunge goes on by what the montage still travels\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string wolf_path = creature_path(f, "/Beast/Canine/Wolf.Default__Wolf_C");
    const auto bp = f.motion.find(wolf_path);
    const abilities::AbilityDef* bite = f.catalog->ability_named("Wolf_Bite");
    CHECK(!wolf_path.empty() && bp && tables.abilities_of(wolf_path) && bite, "the Blueprint table has the Wolf, the creature tables list its abilities and the ability data has the Bite");
    if (wolf_path.empty() || !bp || !tables.abilities_of(wolf_path) || !bite) return;
    const combat::RootMotionFreezeRule* freeze = tables.freeze_of(bite->row.guid);
    CHECK(freeze && freeze->kind == combat::RootMotionFreezeRule::Kind::Pause && freeze->distance > 0, "the Bite's ability record pauses its lunge on contact");
    if (!freeze) return;
    const double r_wolf = bp->radius, r_player = 34.0;

    // the player stands in front: the lunge ends up held ahead of him
    std::vector<LungeSample> stand;
    CHECK(run_lunge_fight(f, tables, wolf_path, 30.0, 0, 800, nullptr, stand), "a fight against a player who stands still runs");
    const std::vector<LungeRun> runs = lunge_runs(stand, bite->row.guid);
    size_t held_runs = 0, creeping = 0, uncleared = 0, silent = 0;
    double closest = 1e9, farthest = 0, slowest_creep = 0;
    for (const LungeRun& r : runs) {
        bool held = false;
        for (size_t i = r.first; i <= r.last; ++i) {
            if (stand[i].obstructed != 1) continue;
            held = true;
            const double gap = std::hypot(stand[i].wx - stand[i].px, stand[i].wy - stand[i].py) - r_wolf - r_player;
            closest = std::min(closest, gap);
            farthest = std::max(farthest, gap);
            if (i > r.first && stand[i - 1].obstructed == 1) {   // held: it does not move from one tick to the next
                const double d = std::hypot(stand[i].wx - stand[i - 1].wx, stand[i].wy - stand[i - 1].wy);
                slowest_creep = std::max(slowest_creep, d);
                if (d > 0.5) ++creeping;
            }
        }
        if (held) ++held_runs;
        if (stand[r.last + 1].obstructed != 0) ++uncleared;   // (the end of the carry clears it)
        if (!held) ++silent;
    }
    std::printf("  %zu lunges of the Bite against a player who stands still: %zu held (flag 1) ahead of him, %.1f to %.1f cm from his surface, the pause distance %.0f cm\n", runs.size(), held_runs, closest,
                farthest, freeze->distance);
    CHECKF(runs.size() >= 5 && silent == 0, "every lunge of the Bite is held by the player in its box, with the flag going out (%zu of %zu lunges never carried it)", silent, runs.size());
    CHECKF(closest >= freeze->distance - 2.0 && farthest <= freeze->distance + 0.5, "the Wolf is held at the pause distance, at the box's edge, and does not come nearer: %.1f to %.1f cm of surface for a pause distance of %.0f cm", closest, farthest,
           freeze->distance);
    CHECKF(creeping == 0, "while the flag is on the Wolf does not move (%zu ticks moved, the most %.2f cm)", creeping, slowest_creep);
    CHECKF(uncleared == 0, "the flag is off again when the carry ends (%zu lunges end with it on)", uncleared);

    // the player steps out of the box as the Wolf is held: the flag clears at once and the Wolf goes on by the part of the montage that is left, not by what it skipped
    double leave_at = -1, side = 700.0;
    std::vector<LungeSample> leave;
    const LungeSteer steer = [&](double now, const LungeSample* prev, double& px, double& py, const world::WorldActor&) {
        (void)py;
        if (leave_at < 0 && prev && prev->active == 1 && prev->obstructed == 1 && prev->ability == bite->row.guid) leave_at = now;   // (the tick after the Wolf was held)
        if (leave_at < 0 || now < leave_at) return false;
        leave_at = -1;
        px += side;
        side = -side;
        return true;
    };
    CHECK(run_lunge_fight(f, tables, wolf_path, 40.0, 0, 800, steer, leave), "a fight against a player who steps out of the box once the Wolf is held runs");
    size_t cleared = 0, slow_clear = 0, moved_on = 0, wrong_travel = 0;
    double worst_travel = 0;
    for (const LungeRun& r : lunge_runs(leave, bite->row.guid)) {
        size_t held = r.last + 1, gone = r.last + 1, clear = r.last + 1;
        for (size_t i = r.first; i <= r.last; ++i) {
            if (held > r.last && leave[i].obstructed == 1) held = i;
            if (held <= r.last && gone > r.last && leave[i].px != leave[held].px) gone = i;
            if (held <= r.last && clear > r.last && i > held && leave[i].obstructed == 0) clear = i;
        }
        if (held > r.last || clear > r.last) continue;   // never held, or held to the end of the carry
        ++cleared;
        if (gone > r.last || clear > gone + 2) ++slow_clear;
        const combat::MontageData* md = montage_of_cast(f, tables, leave[r.first].ability, leave[r.first].montage);
        if (!md || leave[clear - 1].position < 0) continue;
        const combat::MontageData::Step from = md->root_at(leave[clear - 1].position), to = md->root_at(md->length - 0.25);
        const double expected = std::hypot(to.x - from.x, to.y - from.y);
        const double observed = std::hypot(leave[r.last + 1].wx - leave[clear - 1].wx, leave[r.last + 1].wy - leave[clear - 1].wy);
        worst_travel = std::max(worst_travel, std::fabs(observed - expected));
        if (observed > 5.0) ++moved_on;
        if (std::fabs(observed - expected) > 12.0) ++wrong_travel;
    }
    std::printf("  %zu lunges had the player step out while the Wolf was held: the flag cleared within two ticks in %zu, the Wolf went on in %zu, its travel off the montage's by %.1f cm at the most\n", cleared,
                cleared - slow_clear, moved_on, worst_travel);
    CHECKF(cleared >= 3 && slow_clear == 0, "once the player is out of the box the flag clears within two ticks (%zu of %zu cleared late)", slow_clear, cleared);
    CHECKF(moved_on >= 1 && wrong_travel == 0, "the lunge then goes on by what the montage still travels (%zu went on; %zu travelled other than the montage's remainder, the worst %.1f cm)", moved_on, wrong_travel, worst_travel);
}

AOC_REGISTER_TEST(test_lunge_pauses_at_the_player);

void test_swing_is_not_cut_by_walk_orders(Fixture& f) {
    std::printf("a Bear's swings carry it to the end of their travel even when the player side-steps and the AI wants to walk: the walk order waits for the carry to end\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string bear_path = creature_path(f, "/Beast/Bear/Bear.Default__Bear_C");
    CHECK(!bear_path.empty() && f.motion.find(bear_path) && tables.abilities_of(bear_path), "the Blueprint table has the Bear and the creature tables list its abilities");
    if (bear_path.empty() || !f.motion.find(bear_path) || !tables.abilities_of(bear_path)) return;
    // each swing begun, the player side-steps 3 m a moment later: out of the Bear's reach, so its AI orders a walk after him while the montage is still carrying it
    double step_at = -1, side = 300.0;
    size_t steps = 0;
    std::vector<LungeSample> samples;
    const LungeSteer steer = [&](double now, const LungeSample* prev, double& px, double& py, const world::WorldActor&) {
        (void)py;
        if (prev && prev->cast_began) step_at = now + 0.1;
        if (step_at < 0 || now < step_at) return false;
        step_at = -1;
        px += side;
        side = -side;
        ++steps;
        return true;
    };
    CHECK(run_lunge_fight(f, tables, bear_path, 45.0, 0, 800, steer, samples), "a fight against a player who side-steps at every swing runs");
    size_t lunges = 0, cut = 0;
    double worst = 0;
    for (const LungeRun& r : lunge_runs(samples)) {
        const combat::MontageData* md = montage_of_cast(f, tables, samples[r.first].ability, samples[r.first].montage);
        if (!md) continue;
        ++lunges;
        const double expected = (md->length - 0.25) / md->rate_scale;   // seconds the montage carries the creature
        const double played = samples[r.last + 1].t - samples[r.first].t;
        worst = std::max(worst, expected - played);
        if (played < expected - 0.2) ++cut;
    }
    std::printf("  %zu side-steps, %zu lunges in 45 s: %zu carried less than the montage's travel (the worst short by %.2f s)\n", steps, lunges, cut, worst);
    CHECKF(lunges >= 4 && cut == 0, "every lunge is carried to the end of the montage's travel, whatever the AI orders (%zu of %zu cut short, the worst by %.2f s)", cut, lunges, worst);
}

AOC_REGISTER_TEST(test_swing_is_not_cut_by_walk_orders);

void test_timid_raven_swings_back(Fixture& f) {
    std::printf("a Timid Raven (a Blueprint no capture opened, so the generic open that lists none of its tree's abilities): it starts no fight, and once it is hit it swings back with the single swing every creature has\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string raven_path = creature_path(f, "/Raven/Raven_Timid.Default__Raven_Timid_C");
    const abilities::AbilityDef* peck = f.catalog->ability_named("Raven_Peck");
    CHECK(!raven_path.empty() && f.motion.find(raven_path) && tables.abilities_of(raven_path) && peck, "the Blueprint table has the Timid Raven, the creature tables list its abilities and the ability data has the Peck");
    if (raven_path.empty() || !f.motion.find(raven_path) || !tables.abilities_of(raven_path) || !peck) return;
    bool lists_peck = false;
    for (const combat::CreatureAbility& t : *tables.abilities_of(raven_path)) lists_peck = lists_peck || t.ability == peck->row.guid;
    CHECK(lists_peck, "the Timid Raven's tree uses the Peck");
    // the same Raven, Timid, with the player 4 m away: nothing happens for 8 s, and then the player hits it
    std::vector<LungeSample> samples;
    std::vector<combat::IncomingHit> hits;
    combat::CombatWorld::Stats stats;
    CHECK(run_lunge_fight(f, tables, raven_path, 20.0, 0, 400, nullptr, samples, &hits, true, 8.0, &stats), "a fight against a Timid Raven that is hit at 8 s runs");
    size_t before = 0, after = 0;
    for (const combat::IncomingHit& h : hits) {
        if (h.at < 8.0) {
            ++before;
        } else {
            ++after;
        }
    }
    std::printf("  hits on the player: %zu before the Raven was hit, %zu after (%llu attacks in all)\n", before, after, static_cast<unsigned long long>(stats.npc_attacks));
    CHECKF(before == 0, "a Timid Raven starts no fight (%zu hits before it was hit)", before);
    CHECKF(after >= 2 && stats.npc_attacks >= 2, "once it is hit it swings back at the player (%zu hits, %llu attacks)", after, static_cast<unsigned long long>(stats.npc_attacks));
}

AOC_REGISTER_TEST(test_timid_raven_swings_back);

void test_chase_of_a_player_who_walks_away(Fixture& f) {
    std::printf("a creature chases a player who runs away at 720 cm/s, straight and zig-zagging: it never stops while it chases, and it catches up and attacks\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    struct Kind {
        const char* name;
        const char* leaf;
    };
    const Kind kinds[] = {{"Wolf", "/Beast/Canine/Wolf.Default__Wolf_C"}, {"Bear", "/Beast/Bear/Bear.Default__Bear_C"}, {"Grem", "/Beast/Rabid/Rabid.Default__Rabid_C"}};
    for (const Kind& k : kinds) {
        const std::string path = creature_path(f, k.leaf);
        CHECKF(!path.empty() && f.motion.find(path) && tables.abilities_of(path), "the Blueprint table has the %s and the creature tables list its abilities", k.name);
        if (path.empty() || !f.motion.find(path) || !tables.abilities_of(path)) continue;
        ChaseReport straight, zig, walk;
        const bool a = run_chase(f, tables, path, 720.0, 0.0, 12.0, straight);
        const bool b = run_chase(f, tables, path, 720.0, 60.0, 12.0, zig);
        const bool c = run_chase(f, tables, path, 360.0, 0.0, 12.0, walk);
        CHECKF(a && b && c, "the three chases of the %s run", k.name);
        if (!a || !b || !c) continue;
        std::printf("  %-4s (runs %.0f cm/s, turns %.0f deg/s) after a player at 720 cm/s, 12 s:\n", k.name, straight.run_speed, straight.turn_rate);
        std::printf("       straight:   %zu attacks (the first at %.1f s), %zu hits, closest %.0f cm from his surface, stopped %zu of %zu chasing ticks, its facing turned %.1f deg at the most in a tick\n", straight.attacks,
                    straight.first_attack_at, straight.hits, straight.closest_gap, straight.halts, straight.ticks, straight.worst_turn);
        std::printf("       zig-zag:    %zu attacks (the first at %.1f s), %zu hits, closest %.0f cm from his surface, stopped %zu of %zu chasing ticks, its facing turned %.1f deg at the most in a tick\n", zig.attacks,
                    zig.first_attack_at, zig.hits, zig.closest_gap, zig.halts, zig.ticks, zig.worst_turn);
        std::printf("       half speed: %zu attacks (the first at %.1f s), %zu hits, closest %.0f cm from his surface, stopped %zu of %zu chasing ticks\n", walk.attacks, walk.first_attack_at, walk.hits, walk.closest_gap, walk.halts,
                    walk.ticks);
        const auto bites_line = [](const char* label, const ChaseReport& r) {
            std::printf("       %-10s bites due %zu on the target wherever he stands: %zu landed (%zu of them rolled a miss, %zu a block, %zu a critical hit); inside a cone: %zu due, %zu landed\n", label, r.any_due, r.any_landed,
                        r.any_missed, r.any_blocked, r.any_crit, r.cone_due, r.cone_landed);
        };
        bites_line("straight", straight);
        bites_line("zig-zag", zig);
        bites_line("half speed", walk);
        CHECKF(straight.halts == 0 && zig.halts == 0 && walk.halts == 0, "the %s does not stop while it chases (%zu, %zu and %zu chasing ticks at a stand)", k.name, straight.halts, zig.halts, walk.halts);
        // a bite whose record measures nothing lands at its notify on the runner wherever he is (the hit roll may still miss it: a miss is a hit that reached him with the miss flag)
        CHECKF(straight.any_landed >= straight.any_due && zig.any_landed >= zig.any_due && walk.any_landed >= walk.any_due,
               "every bite of the %s's attacks that measures nothing reaches the runner at its notify, at 720 and at 360 cm/s (%zu of %zu, %zu of %zu and %zu of %zu)", k.name, straight.any_landed, straight.any_due,
               zig.any_landed, zig.any_due, walk.any_landed, walk.any_due);
        CHECKF(k.name[0] != 'W' || (straight.attacks >= 1 && straight.any_due >= 1 && zig.any_due >= 1),
               "the Wolf's attacks at the runner have bites that fall due (%zu bites in the straight chase, %zu in the zig-zag)", straight.any_due, zig.any_due);
        const double step = k.name[0] == 'W' ? 720.0 / 15.0 : 360.0 / 15.0;
        CHECKF(straight.worst_turn <= step + 1.0 && zig.worst_turn <= step + 1.0, "its facing turns at its rotation rate and never jumps (%.1f and %.1f deg in a tick, %.1f allowed)", straight.worst_turn, zig.worst_turn, step + 1.0);
        if (straight.run_speed > 720.0) {   // faster than the player: it catches up and attacks (the Bear, at 800 against 720, gains 80 cm a second and stands for each swing: it catches up once, then the player is away again)
            const size_t wanted = straight.run_speed > 1000.0 ? 2 : 1;
            CHECKF(straight.attacks >= wanted && zig.attacks >= wanted && straight.first_attack_at >= 0 && straight.first_attack_at < 4.0,
                   "the %s (faster than the player) catches up within 4 s and attacks (%zu attacks in the straight chase, the first at %.1f s; %zu in the zig-zag)", k.name, straight.attacks, straight.first_attack_at, zig.attacks);
        }
    }
}

AOC_REGISTER_TEST(test_chase_of_a_player_who_walks_away);

void test_swing_faces_its_target(Fixture& f) {
    std::printf("a Bear whose player runs round it at 720 cm/s turns to him at its rate through every swing: the facing a client is sent is the server's, and the lunge goes towards him\n");
    constexpr double kPi = 3.14159265358979323846;
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string bear_path = creature_path(f, "/Beast/Bear/Bear.Default__Bear_C");
    const auto bp = f.motion.find(bear_path);
    CHECK(!bear_path.empty() && bp && tables.abilities_of(bear_path), "the Blueprint table has the Bear and the creature tables list its abilities");
    if (bear_path.empty() || !bp || !tables.abilities_of(bear_path)) return;
    const double dt = 1.0 / 15.0, radius = 450.0, angular = 720.0 / radius;   // the player runs round the Bear's start on a circle of 4.5 m
    double angle = kPi / 2.0;
    const LungeSteer steer = [&](double, const LungeSample*, double& x, double& y, const world::WorldActor&) {
        angle += angular * dt;
        x = radius * std::cos(angle);
        y = radius * std::sin(angle);
        return true;
    };
    std::vector<LungeSample> samples;
    std::vector<combat::IncomingHit> got;
    CHECK(run_lunge_fight(f, tables, bear_path, 40.0, 0, radius, steer, samples, &got, false, -1, nullptr, 40000.0), "a fight against a player who runs round the Bear runs");
    // the facing never jumps
    double worst_turn = 0;
    for (size_t i = 1; i < samples.size(); ++i) worst_turn = std::max(worst_turn, std::fabs(wrap180(samples[i].yaw - samples[i - 1].yaw)));
    // every swing: the facing a client holds when it begins is the server's, and the lunge goes towards the player
    size_t swings = 0, sent_off = 0, lunges = 0, wrong_way = 0;
    double worst_sent = 0, worst_lunge = 0;
    for (size_t i = 0; i < samples.size(); ++i) {
        if (!samples[i].cast_began) continue;
        ++swings;
        if (samples[i].has_mv_yaw) {
            const double d8 = std::fabs(wrap180(samples[i].mv_yaw - samples[i].yaw));
            const double d16 = samples[i].has_rrm_yaw ? std::fabs(wrap180(samples[i].rrm_yaw - samples[i].yaw)) : 0.0;
            worst_sent = std::max(worst_sent, std::max(d8, d16));
            if (d8 > 0.75 * 360.0 / 256.0 || d16 > 0.05) ++sent_off;
        }
    }
    for (const LungeRun& r : lunge_runs(samples)) {
        const LungeSample& from = samples[r.first];
        const LungeSample& to = samples[r.last + 1];
        const double travelled = std::hypot(to.wx - from.wx, to.wy - from.wy);
        if (travelled < 60.0) continue;   // (held at the player: it did not go anywhere)
        const combat::MontageData* md = montage_of_cast(f, tables, from.ability, from.montage);
        if (!md || md->hits.empty()) continue;
        const size_t at_hit = std::min(samples.size() - 1, r.first + static_cast<size_t>(md->hits.front().time / md->rate_scale / dt + 0.5));
        const double bearing = std::atan2(samples[at_hit].py - from.wy, samples[at_hit].px - from.wx) * 180.0 / kPi;
        const double lunge = std::atan2(to.wy - from.wy, to.wx - from.wx) * 180.0 / kPi;
        const double off = std::fabs(wrap180(lunge - bearing));
        ++lunges;
        worst_lunge = std::max(worst_lunge, off);
        if (off > 45.0) ++wrong_way;
    }
    std::printf("  %zu swings in 40 s, %zu hits on the runner: the facing a client holds at their start is the server's within %.2f deg (%zu off); %zu lunges of 60 cm or more, the worst %.0f deg off the way to the player when the "
                "hit lands (%zu over 45); its facing turned %.1f deg at the most in a tick (its rate allows %.1f)\n", swings, got.size(), worst_sent, sent_off, lunges, worst_lunge, wrong_way, worst_turn, bp->rotation_rate_yaw / 15.0);
    CHECKF(swings >= 8, "the Bear swings again and again at a player who runs round it (%zu swings)", swings);
    CHECKF(sent_off == 0, "the facing a client is sent when a swing begins is the server's (%zu of %zu swings differ)", sent_off, swings);
    CHECKF(lunges >= 3 && wrong_way == 0, "every lunge goes towards the player, within 45 deg of the way to him at the hit (%zu of %zu do not, the worst %.0f deg)", wrong_way, lunges, worst_lunge);
    CHECKF(worst_turn <= bp->rotation_rate_yaw / 15.0 + 1.0, "its facing turns at its rotation rate and never jumps (%.1f deg in a tick, %.1f allowed)", worst_turn, bp->rotation_rate_yaw / 15.0 + 1.0);
}

AOC_REGISTER_TEST(test_swing_faces_its_target);

void test_turn_to_a_player_behind(Fixture& f) {
    std::printf("a Wolf with a player right behind it turns to him at its rotation rate, one step at a time (never a jump), and its bite is begun within the tree's angle and goes on turning\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string wolf_path = creature_path(f, "/Beast/Canine/Wolf.Default__Wolf_C");
    const auto bp = f.motion.find(wolf_path);
    CHECK(!wolf_path.empty() && bp && tables.abilities_of(wolf_path), "the Blueprint table has the Wolf and the creature tables list its abilities");
    if (wolf_path.empty() || !bp || !tables.abilities_of(wolf_path)) return;
    std::vector<LungeSample> samples;
    CHECK(run_lunge_fight(f, tables, wolf_path, 6.0, -300, 0, nullptr, samples), "a fight against a player right behind the Wolf runs");
    const double step = bp->rotation_rate_yaw / 15.0;
    double worst_turn = 0;
    size_t turned = 0, first_cast = samples.size();
    for (size_t i = 0; i < samples.size(); ++i) {
        const double d = std::fabs(wrap180(samples[i].yaw - (i == 0 ? 0.0 : samples[i - 1].yaw)));   // (it was made facing 0 deg)
        worst_turn = std::max(worst_turn, d);
        if (d > 0.5 && first_cast == samples.size()) ++turned;
        if (samples[i].cast_began && first_cast == samples.size()) first_cast = i;
    }
    CHECK(first_cast < samples.size(), "the Wolf bites the player who was behind it");
    if (first_cast >= samples.size()) return;
    const LungeSample& at = samples[first_cast];
    const double error_at_start = std::fabs(wrap180(std::atan2(at.py - at.wy, at.px - at.wx) * 180.0 / 3.14159265358979323846 - at.yaw));
    const size_t later = std::min(samples.size() - 1, first_cast + 8);   // half a second on
    const double error_later = std::fabs(wrap180(std::atan2(samples[later].py - samples[later].wy, samples[later].px - samples[later].wx) * 180.0 / 3.14159265358979323846 - samples[later].yaw));
    std::printf("  its facing turned %.1f deg at the most in a tick (its rate allows %.1f), over %zu ticks before the bite began; it began %.1f deg off the player and was %.1f deg off half a second on\n", worst_turn, step,
                turned, error_at_start, error_later);
    CHECKF(worst_turn <= step + 1.0, "its facing turns at its rotation rate and never jumps (%.1f deg in a tick, %.1f allowed)", worst_turn, step + 1.0);
    CHECKF(turned >= 3, "it turns step by step to the player behind it (%zu ticks of turning before the bite)", turned);
    CHECKF(error_at_start <= 46.0 && error_later <= 6.0, "the bite begins within the tree's 45 deg and goes on turning to the player (%.1f deg off at its start, %.1f half a second on)", error_at_start, error_later);
}

AOC_REGISTER_TEST(test_turn_to_a_player_behind);
