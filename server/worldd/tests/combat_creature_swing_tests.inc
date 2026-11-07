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

// PR #10 review: a creature whose every attack with its own animation waits on the fight clock or on its health keeps the single swing every creature had until one of them opens; one with an attack
// that waits on nothing plays it from the start, as before.  The Wolf's tree is cut to its Bite alone, under the condition the case names; the montages and the targeting are the real tables.
void test_creature_swings_until_its_timed_attack_opens(Fixture& f) {
    std::printf("a creature whose only animated attack waits on the fight clock (or on its health) swings its single swing until the attack may start, and plays the attack once the clock opens; one whose attack waits on nothing plays it at once\n");
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
    const fs::path tmp = fs::temp_directory_path() / ("aoc_creature_timed_swing_" + unique_temp_suffix());   // (a folder of its own: two gates can run this program at once)
    fs::create_directories(tmp, ec);
    const std::string abilities_header =
        "archetype\tability\ttask\tcooldown\tstop_movement\torder\tmin_distance\tmax_distance\tmax_angle\tgates\tunknown\tfight\tconds\twhy\tstarts\trotate\tarrival";

    struct Run {
        bool   ran = false;
        double first_cast = -1, first_hit = -1;   // seconds into the fight: the first bite played, the first hit that reached the player
        size_t casts = 0, hits_before_5s = 0, hits = 0;
    };
    // a 16 s fight against a player who stands 8 m from the Wolf, whose tree is the Bite alone under `conds` (the tree's condition text; empty: none)
    const auto play = [&](const char* conds) {
        Run r;
        const std::string table = (tmp / "abilities.tsv").string();
        {
            std::ofstream out(table, std::ios::binary);
            out << abilities_header << "\n" << wolf_path << "\t" << bite->row.guid << "\t\t0\t1\t37\t-1\t270\t45\tGlobal Cooldown\t0\t1\t" << conds << "\t\tGlobal Cooldown=0.5\t360:0\t-1\n";
        }
        combat::NpcAbilityData tables;
        std::string err;
        const bool loaded = tables.load(table, dir + "npc_montages.tsv", dir + "npc_targeting.tsv", err);
        CHECKF(loaded, "the Wolf's tree with its Bite under '%s' loads (%s)", conds, err.c_str());
        if (!loaded) return r;
        std::vector<LungeSample> samples;
        std::vector<combat::IncomingHit> got;
        r.ran = run_lunge_fight(f, tables, wolf_path, 16.0, 0, 800, nullptr, samples, &got, false, -1, nullptr, 40000.0);
        if (!r.ran) return r;
        for (const LungeSample& s : samples) {
            if (!s.cast_began) continue;
            ++r.casts;
            if (r.first_cast < 0) r.first_cast = s.t;
        }
        for (const combat::IncomingHit& h : got) {
            ++r.hits;
            if (r.first_hit < 0) r.first_hit = h.at;
            if (h.at < 5.0) ++r.hits_before_5s;
        }
        return r;
    };
    const Run timed = play("c0:fc(ge,6)");
    const Run health = play("c0:hp(0.5)");
    const Run plain = play("");
    fs::remove_all(tmp, ec);
    CHECK(timed.ran && health.ran && plain.ran, "the three fights run (the Wolf is made from its open and knows the Bite)");
    if (!timed.ran || !health.ran || !plain.ran) return;
    std::printf("  Bite under fc(ge,6): first bite played at %.2f s, first hit at %.2f s, %zu hits in the first 5 s, %zu bites in 16 s;  under hp(0.5): %zu bites, %zu hits;  under nothing: first bite at %.2f s, first hit "
                "at %.2f s, %zu hits in the first 5 s\n", timed.first_cast, timed.first_hit, timed.hits_before_5s, timed.casts, health.casts, health.hits, plain.first_cast, plain.first_hit, plain.hits_before_5s);
    // the Bite waits for the fight's sixth second: until then the Wolf swings its single swing (its hits reach the player), and the bite is played once the clock has opened
    CHECKF(timed.hits_before_5s >= 2 && timed.first_hit >= 0 && timed.first_hit < 5.0, "the Wolf whose Bite waits for the fight's sixth second swings in the meantime (%zu hits in the first 5 s, the first at %.2f s)", timed.hits_before_5s, timed.first_hit);
    CHECKF(timed.first_cast >= 6.0 && timed.first_cast < 12.0 && timed.casts >= 1, "... and plays the Bite once the clock allows it, never before (first bite at %.2f s, %zu bites in 16 s)", timed.first_cast, timed.casts);
    // a Bite that waits for the Wolf's health to fall under half never opens against a player who does not hurt it: the Wolf swings the whole fight and plays nothing
    CHECKF(health.casts == 0 && health.hits >= 4, "the Wolf whose Bite waits for its health to fall under half swings all the fight (%zu bites, %zu hits in 16 s)", health.casts, health.hits);
    // a Bite that waits for nothing is played at once, with no swing before it
    CHECKF(plain.first_cast > 0 && plain.first_cast < 4.0 && plain.first_hit >= plain.first_cast && plain.hits_before_5s >= 1,
           "the Wolf whose Bite waits for nothing plays it from the start, with no swing before it (first bite at %.2f s, first hit at %.2f s)", plain.first_cast, plain.first_hit);
}

AOC_REGISTER_TEST(test_creature_swings_until_its_timed_attack_opens);

// The data's stop_movement flag: an attack whose task does not stand for its montage leaves the chase (and so the movement component's turning) running, so the creature keeps moving and turning
// at a target that moves during the montage; one whose task stands holds the creature and its facing until the montage ends, unless the task's Rotate To turns it.  A root still holds its feet.
// The tree here is a temp table over the real montages and targeting, as test_creature_swings_until_its_timed_attack_opens does; the Growl's montage plays in place (test_creature_movement_reaches_client).
void test_attack_stop_movement_keeps_the_chase(Fixture& f) {
    std::printf("an attack whose task does not stop movement keeps the creature chasing and turning at a player who moves during the montage; one that stops it holds the facing until the montage ends; a root still stops it\n");
    namespace fs = std::filesystem;
    combat::NpcAbilityData real;
    if (!load_creature_tables(real)) return;
    const std::string alpha_path = creature_path(f, "/Beast/Canine/AlphaWolf.Default__AlphaWolf_C");
    const auto bp = f.motion.find(alpha_path);
    const abilities::AbilityDef* growl = f.catalog->ability_named("AlphaWolf_Growl");
    const abilities::EffectDef* rooted = f.catalog->effect_named("Status_Rooted");
    CHECK(!alpha_path.empty() && bp && real.abilities_of(alpha_path) && growl && rooted,
          "the Blueprint table has the Alpha Wolf, the creature tables list its abilities, and the ability data has the Growl and the Rooted effect");
    if (alpha_path.empty() || !bp || !real.abilities_of(alpha_path) || !growl || !rooted) return;

    std::string dir = NPC_MOTION_PATH;
    dir.erase(dir.find_last_of("/\\") + 1);
    std::error_code ec;
    const fs::path tmp = fs::temp_directory_path() / ("aoc_creature_stop_movement_" + unique_temp_suffix());   // (a folder of its own: two gates can run this program at once)
    fs::create_directories(tmp, ec);
    const std::string abilities_header =
        "archetype\tability\ttask\tcooldown\tstop_movement\torder\tmin_distance\tmax_distance\tmax_angle\tgates\tunknown\tfight\tconds\twhy\tstarts\trotate\tarrival";
    const double dt = 1.0 / 15.0;

    struct Cast {
        size_t first = 0, last = 0;
        double turned = 0, moved = 0, player_moved = 0, bearing_error = 0;
        double needed = 0;         // how far off the bearing to the player (where the cast ends) the creature faced when the cast began: the turn the cast asks of it
        bool   in_place = false;   // its montage stays in place, so the chase during it is the creature's own walk
    };
    struct Run {
        bool                     ran = false;
        std::vector<LungeSample> samples;
        std::vector<Cast>        casts;
    };
    // a fight against a player who starts 8 m away and, from the first cast on, walks along +x at 400 cm/s: every cast after it is at a target that moves
    const auto play = [&](const char* stop, bool root, double seconds) {
        Run r;
        const std::string table = (tmp / "abilities.tsv").string();
        {
            std::ofstream out(table, std::ios::binary);
            out << abilities_header << "\n" << alpha_path << "\t" << growl->row.guid << "\tGrowl\t0.5\t" << stop << "\t0\t-1\t100\t-1\t\t0\t1\t\t\t\t\t-1\n";
        }
        combat::NpcAbilityData tables;
        std::string err;
        if (!tables.load(table, dir + "npc_montages.tsv", dir + "npc_targeting.tsv", err)) {
            CHECKF(false, "the Alpha Wolf's tree with the Growl at stop_movement=%s loads (%s)", stop, err.c_str());
            return r;
        }
        bool walking = false;
        const LungeSteer steer = [&](double, const LungeSample* prev, double& px, double&, const world::WorldActor&) {
            if (prev && prev->cast_began) walking = true;
            if (!walking) return false;
            px += 400.0 * dt;
            return true;
        };
        bool put = false;
        const LungeStep step = [&](LungeStepContext& c) {
            if (!root || put || !c.after || !c.sample || !c.sample->cast_began) return;
            put = true;
            effects::Applied a;
            a.def = rooted;
            a.duration = 30.0;
            c.world.apply_effects(c.creature, c.player.actor_id(), {a}, c.now);
        };
        r.ran = run_lunge_fight(f, tables, alpha_path, seconds, 0, 800, steer, r.samples, nullptr, false, -1, nullptr, 40000.0, nullptr, 0, 0, step);
        if (!r.ran) return r;
        for (size_t i = 0; i < r.samples.size(); ++i) {
            if (!r.samples[i].cast_began) continue;
            size_t last = i;
            while (last + 1 < r.samples.size() && r.samples[last + 1].montage != 0 && !r.samples[last + 1].cast_began) ++last;
            Cast c;
            c.first = i;
            c.last = last;
            for (size_t k = i; k <= last; ++k) c.turned = std::max(c.turned, std::fabs(wrap180(r.samples[k].yaw - r.samples[i].yaw)));
            c.moved = std::hypot(r.samples[last].wx - r.samples[i].wx, r.samples[last].wy - r.samples[i].wy);
            c.player_moved = std::hypot(r.samples[last].px - r.samples[i].px, r.samples[last].py - r.samples[i].py);
            const double bearing = std::atan2(r.samples[last].py - r.samples[last].wy, r.samples[last].px - r.samples[last].wx) * 180.0 / 3.14159265358979323846;
            c.bearing_error = std::fabs(wrap180(r.samples[last].yaw - bearing));
            c.needed = std::fabs(wrap180(r.samples[i].yaw - bearing));
            const combat::MontageData* played = montage_of_cast(f, tables, r.samples[i].ability, r.samples[i].montage);
            c.in_place = played && played->root_motion.empty();
            r.casts.push_back(c);
        }
        return r;
    };
    const Run free_run = play("0", false, 12.0);
    const Run held_run = play("1", false, 12.0);
    const Run rooted_run = play("0", true, 12.0);
    fs::remove_all(tmp, ec);
    CHECK(free_run.ran && held_run.ran && rooted_run.ran, "the three fights run (the Alpha Wolf is made from its open and knows the Growl)");
    if (!free_run.ran || !held_run.ran || !rooted_run.ran) return;
    size_t free_cases = 0, free_moved = 0, free_turned = 0, free_asked = 0, free_faced = 0, free_air = 0;
    double least_moved = 1e9, least_turned = 1e9, worst_facing = 0;
    for (const Cast& c : free_run.casts) {
        if (!c.in_place) ++free_air;
        if (c.player_moved <= 50.0) continue;   // (the player stood for this one: it says nothing about a chase)
        ++free_cases;
        least_moved = std::min(least_moved, c.moved);
        least_turned = std::min(least_turned, c.turned);
        worst_facing = std::max(worst_facing, c.bearing_error);
        if (c.moved < 20.0) ++free_moved;
        if (c.needed >= 10.0) {   // (a creature that trails the player straight behind him has no turn to make: only a cast that began facing 10 deg or more off him asks for one)
            ++free_asked;
            if (c.turned < 5.0) ++free_turned;
        }
        if (c.bearing_error > 45.0) ++free_faced;
    }
    size_t held_cases = 0, held_moved = 0, held_turned = 0, held_air = 0;
    double most_moved = 0, most_turned = 0;
    for (const Cast& c : held_run.casts) {
        if (!c.in_place) ++held_air;
        if (c.player_moved <= 50.0) continue;
        ++held_cases;
        most_moved = std::max(most_moved, c.moved);
        most_turned = std::max(most_turned, c.turned);
        if (c.moved > 5.0) ++held_moved;
        if (c.turned > 1.0) ++held_turned;
    }
    size_t rooted_cases = 0, rooted_moved = 0, rooted_air = 0;
    double most_rooted = 0;
    for (const Cast& c : rooted_run.casts) {
        if (!c.in_place) ++rooted_air;
        if (c.player_moved <= 50.0) continue;
        ++rooted_cases;
        most_rooted = std::max(most_rooted, c.moved);
        if (c.moved > 5.0) ++rooted_moved;
    }
    std::printf("  stop_movement=0: %zu casts at a moving target, the least %.1f cm moved and %.1f deg turned, the worst facing %.1f deg off;  stop_movement=1: %zu casts, the most %.1f cm and %.1f deg;  "
                "rooted: %zu casts, the most %.1f cm\n", free_cases, free_cases ? least_moved : 0.0, free_cases ? least_turned : 0.0, worst_facing, held_cases, most_moved, most_turned, rooted_cases, most_rooted);
    CHECKF(free_air == 0 && held_air == 0 && rooted_air == 0, "every cast's montage stays in place, so a chase during it is the creature's own walk (%zu, %zu and %zu casts did not)", free_air, held_air, rooted_air);
    CHECKF(free_cases >= 1, "a stop_movement=0 attack begins while the player moves (%zu such casts)", free_cases);
    CHECKF(free_moved == 0, "during every stop_movement=0 attack at a moving target the creature chases it (%zu of %zu casts moved less than 20 cm, the least %.1f cm)", free_moved, free_cases, free_cases ? least_moved : 0.0);
    CHECKF(free_asked >= 1 && free_turned == 0, "... and turns towards it where it began facing off him (%zu of %zu casts that began 10 deg or more off the player turned less than 5 deg)", free_turned, free_asked);
    CHECKF(free_faced == 0, "... and ends facing it within 45 deg (%zu of %zu casts did not, the worst %.1f deg)", free_faced, free_cases, worst_facing);
    CHECKF(held_cases >= 1, "a stop_movement=1 attack begins while the player moves (%zu such casts)", held_cases);
    CHECKF(held_moved == 0, "during every stop_movement=1 attack at a moving target the creature stands (%zu of %zu casts moved more than 5 cm, the most %.1f cm)", held_moved, held_cases, most_moved);
    CHECKF(held_turned == 0, "... and keeps its facing (%zu of %zu casts turned more than 1 deg, the most %.1f deg)", held_turned, held_cases, most_turned);
    CHECKF(rooted_cases >= 1, "a stop_movement=0 attack begins with the creature rooted (%zu such casts)", rooted_cases);
    CHECKF(rooted_moved == 0, "a root still stops it during a stop_movement=0 attack (%zu of %zu casts moved more than 5 cm, the most %.1f cm)", rooted_moved, rooted_cases, most_rooted);
}

AOC_REGISTER_TEST(test_attack_stop_movement_keeps_the_chase);

// A stop_movement=0 attack that has a Rotate To as well (970 of the 1,584 such tasks) leaves the chase and the Rotate To track running together.  Both turn the creature, so while the track runs it alone
// turns the facing: the chase's walk and the track must not add their turns up, or the creature turns faster than its own rate.  The player steps to the creature's back as its first cast begins (a run
// round it, a dodge), so that both want the same large turn at once.  The task is the Growl of the test above, with a Rotate To of 360 deg/s for the whole montage.
void test_attack_with_rotate_to_turns_at_the_creatures_rate(Fixture& f) {
    std::printf("an attack that does not stop the creature and has a Rotate To: the creature chases a player who stepped behind it and turns round to him, never faster than its own rate\n");
    namespace fs = std::filesystem;
    combat::NpcAbilityData real;
    if (!load_creature_tables(real)) return;
    const std::string alpha_path = creature_path(f, "/Beast/Canine/AlphaWolf.Default__AlphaWolf_C");
    const auto bp = f.motion.find(alpha_path);
    const abilities::AbilityDef* growl = f.catalog->ability_named("AlphaWolf_Growl");
    CHECK(!alpha_path.empty() && bp && real.abilities_of(alpha_path) && growl, "the Blueprint table has the Alpha Wolf, the creature tables list its abilities, and the ability data has the Growl");
    if (alpha_path.empty() || !bp || !real.abilities_of(alpha_path) || !growl) return;

    std::string dir = NPC_MOTION_PATH;
    dir.erase(dir.find_last_of("/\\") + 1);
    std::error_code ec;
    const fs::path tmp = fs::temp_directory_path() / ("aoc_creature_rotate_chase_" + unique_temp_suffix());   // (a folder of its own: two gates can run this program at once)
    fs::create_directories(tmp, ec);
    const std::string table = (tmp / "abilities.tsv").string();
    {
        std::ofstream out(table, std::ios::binary);
        out << "archetype\tability\ttask\tcooldown\tstop_movement\torder\tmin_distance\tmax_distance\tmax_angle\tgates\tunknown\tfight\tconds\twhy\tstarts\trotate\tarrival\n"
            << alpha_path << "\t" << growl->row.guid << "\tGrowl\t0.5\t0\t0\t-1\t100\t-1\t\t0\t1\t\t\t\t360:0\t-1\n";
    }
    combat::NpcAbilityData tables;
    std::string err;
    const bool loaded = tables.load(table, dir + "npc_montages.tsv", dir + "npc_targeting.tsv", err);
    fs::remove_all(tmp, ec);
    CHECKF(loaded, "the Alpha Wolf's tree with the Growl at stop_movement=0 and a Rotate To of 360:0 loads (%s)", err.c_str());
    if (!loaded) return;

    constexpr double kRate = 360.0;   // the creature's own turning rate, set on its walker so that the check does not depend on what the rig starts it with
    bool jumped = false;
    const LungeSteer steer = [&](double, const LungeSample* prev, double& px, double& py, const world::WorldActor& wolf) {
        if (!prev || !prev->cast_began || jumped) return false;
        jumped = true;   // (the player steps to the wolf's back, 8 m from it, in the tick after its first cast began)
        const double back = (wolf.yaw_degrees + 180.0) * 3.14159265358979323846 / 180.0;
        px = wolf.x + 800.0 * std::cos(back);
        py = wolf.y + 800.0 * std::sin(back);
        return true;
    };
    const LungeStep step = [&](LungeStepContext& c) {
        if (!c.after) c.w.npcs->set_yaw_rate(c.creature, kRate);
    };
    std::vector<LungeSample> samples;
    const bool ran = run_lunge_fight(f, tables, alpha_path, 8.0, 0, 800, steer, samples, nullptr, false, -1, nullptr, 40000.0, nullptr, 0, 0, step);
    CHECK(ran, "a fight of 8 s against the player runs (the Alpha Wolf is made from its open and knows the Growl)");
    if (!ran) return;
    size_t first = samples.size();
    for (size_t i = 0; i < samples.size(); ++i) {
        if (samples[i].cast_began) {
            first = i;
            break;
        }
    }
    CHECK(jumped && first + 1 < samples.size(), "the Alpha Wolf plays its Growl, and the player steps behind it as it begins");
    if (!jumped || first + 1 >= samples.size()) return;
    size_t last = first;   // the end of that first cast
    while (last + 1 < samples.size() && samples[last + 1].montage != 0 && !samples[last + 1].cast_began) ++last;
    double worst_step = 0, turned = 0;
    for (size_t k = first + 1; k < samples.size(); ++k) worst_step = std::max(worst_step, std::fabs(wrap180(samples[k].yaw - samples[k - 1].yaw)));
    for (size_t k = first; k <= last; ++k) turned = std::max(turned, std::fabs(wrap180(samples[k].yaw - samples[first].yaw)));
    const double moved = std::hypot(samples[last].wx - samples[first].wx, samples[last].wy - samples[first].wy);
    const double bearing = std::atan2(samples[last].py - samples[last].wy, samples[last].px - samples[last].wx) * 180.0 / 3.14159265358979323846;
    const double error = std::fabs(wrap180(samples[last].yaw - bearing));
    std::printf("  the player stepped behind it at %.2f s: in that cast of %.2f s it moved %.0f cm and turned %.1f deg, ending %.1f deg off him; from then on the biggest turn in a tick was %.1f deg (its rate allows %.1f)\n",
                samples[first].t, static_cast<double>(last - first + 1) / 15.0, moved, turned, error, worst_step, kRate / 15.0);
    CHECKF(moved >= 20.0, "during its stop_movement=0 attack the creature chases the player who stepped behind it (%.0f cm moved in the cast)", moved);
    CHECKF(turned >= 120.0 && error <= 45.0, "... and turns round to him (%.1f deg turned, %.1f deg off him at the end of the cast)", turned, error);
    CHECKF(worst_step <= kRate / 15.0 + 1.0, "... and its facing never turns faster than its own rate, with the chase and the Rotate To both at work (%.1f deg in a tick, %.1f allowed)", worst_step, kRate / 15.0 + 1.0);
}

AOC_REGISTER_TEST(test_attack_with_rotate_to_turns_at_the_creatures_rate);

// The Minotaur Berserker's axe swing is an attack that does not stop the creature (stop_movement=0) and has no Rotate To, and its montage carries the creature for most of its 2.5 s.  This swing (the
// one such attack the recordings show turning, kAttacksThatKeepTurning) keeps the creature turning towards its target at its own rate for the whole montage, also while the montage carries it: a player who runs round it is faced at the end of every swing, and the
// facing never turns faster than the creature's rate.
void test_carried_attack_keeps_turning_to_a_circling_player(Fixture& f) {
    std::printf("a Minotaur Berserker whose player runs round it at 90 deg/s turns to him through every axe swing its montage carries, at its own rate, and ends each swing facing him\n");
    constexpr double kPi = 3.14159265358979323846;
    const double dt = 1.0 / 15.0;
    combat::NpcAbilityData real;
    if (!load_creature_tables(real)) return;
    const std::string mino = creature_path(f, "/Minotaur/Berserker/Minotaur_Berserker.Default__Minotaur_Berserker_C");
    const auto bp = f.motion.find(mino);
    BanditOpen made;
    const bool have = !mino.empty() && bp && real.abilities_of(mino) && make_bandit_open(f, real, mino, made) && made.open;
    CHECK(have, "the Blueprint table has the Minotaur Berserker, the creature tables list its abilities, and the world makes an open for it");
    if (!have) return;
    bool started = false;
    double angle = 0, radius = 0, cx = 0, cy = 0;
    const LungeSteer steer = [&](double, const LungeSample* prev, double& px, double& py, const world::WorldActor& c) {
        if (prev && prev->cast_began && !started) {   // (from its first swing on, the player runs round the place it swung from, at the distance he had)
            started = true;
            cx = c.x;
            cy = c.y;
            angle = std::atan2(py - cy, px - cx);
            radius = std::hypot(px - cx, py - cy);
        }
        if (!started) return false;
        angle += 90.0 * kPi / 180.0 * dt;
        px = cx + radius * std::cos(angle);
        py = cy + radius * std::sin(angle);
        return true;
    };
    std::vector<LungeSample> samples;
    const bool ran = run_lunge_fight(f, real, mino, 40.0, 0, 450, steer, samples, nullptr, false, 0.2, nullptr, 40000.0, made.open);
    CHECK(ran, "a fight of 40 s against a player who runs round the Minotaur Berserker runs");
    if (!ran) return;
    const double allowed = bp->rotation_rate_yaw * combat::kHumanoidTurnGain / 15.0 + 1.0;   // (its rate in a tick, with the turn gain of the creatures that run at their stat speed)
    double worst_step = 0;
    for (size_t i = 1; i < samples.size(); ++i) worst_step = std::max(worst_step, std::fabs(wrap180(samples[i].yaw - samples[i - 1].yaw)));
    size_t swings = 0, unturned = 0, off = 0;
    double least_turn = 1e9, worst_end = 0;
    std::vector<double> ends;
    for (size_t i = 0; i < samples.size(); ++i) {
        if (!samples[i].cast_began) continue;
        size_t last = i;
        while (last + 1 < samples.size() && samples[last + 1].montage != 0 && !samples[last + 1].cast_began) ++last;
        size_t carried = 0;
        double turn = 0;
        for (size_t k = i + 1; k <= last; ++k) {
            if (samples[k].active != 1) continue;
            ++carried;
            turn += std::fabs(wrap180(samples[k].yaw - samples[k - 1].yaw));
        }
        if (carried * 2 < last - i + 1) continue;   // (a swing the montage does not carry for most of its time says nothing here)
        const LungeSample& e = samples[last];
        if (std::hypot(e.px - e.wx, e.py - e.wy) < 150.0) continue;   // (a player inside its reach at the end has no clear way to face)
        const double bearing = std::atan2(e.py - e.wy, e.px - e.wx) * 180.0 / kPi;
        const double end_off = std::fabs(wrap180(e.yaw - bearing));
        ++swings;
        least_turn = std::min(least_turn, turn);
        worst_end = std::max(worst_end, end_off);
        ends.push_back(end_off);
        if (turn < 20.0) ++unturned;
        if (end_off > 15.0) ++off;
    }
    std::sort(ends.begin(), ends.end());
    const double median = ends.empty() ? 0.0 : ends[ends.size() / 2];
    std::printf("  %zu carried swings at the runner: the least turn while carried %.1f deg; at their end %.1f deg off him at the median, %.1f at the worst; the biggest turn in a tick %.1f deg (%.1f allowed)\n",
                swings, swings ? least_turn : 0.0, median, worst_end, worst_step, allowed);
    CHECKF(swings >= 3, "the Minotaur Berserker swings its axe again and again at a player who runs round it (%zu carried swings)", swings);
    CHECKF(unturned == 0, "it turns towards him while every swing carries it (%zu of %zu swings turned less than 20 deg while carried, the least %.1f deg)", unturned, swings, swings ? least_turn : 0.0);
    CHECKF(off == 0 && median <= 5.0, "... and ends every swing facing him (%zu of %zu ended more than 15 deg off; %.1f deg at the median, 5 allowed)", off, swings, median);
    CHECKF(worst_step <= allowed, "... and its facing never turns faster than its own rate (%.1f deg in a tick, %.1f allowed)", worst_step, allowed);
    // the facing a client is sent while the montage carries it is the server's, on every carried tick (ReplicatedMovement's 8 bits, RepRootMotion.Rotation's 16)
    size_t carried_ticks = 0, wire_off = 0;
    double worst_wire = 0;
    for (const LungeSample& s : samples) {
        if (s.active != 1) continue;
        ++carried_ticks;
        if (s.has_mv_yaw) {
            const double d8 = std::fabs(wrap180(s.mv_yaw - s.yaw));
            worst_wire = std::max(worst_wire, d8);
            if (d8 > 0.75 * 360.0 / 256.0) ++wire_off;
        }
        if (s.has_rrm_yaw) {
            const double d16 = std::fabs(wrap180(s.rrm_yaw - s.yaw));
            worst_wire = std::max(worst_wire, d16);
            if (d16 > 0.05) ++wire_off;
        }
    }
    CHECKF(carried_ticks >= 5 && wire_off == 0, "the facing a client is sent while the montage carries it is the server's (%zu carried ticks, %zu off, the worst %.2f deg)", carried_ticks, wire_off,
           worst_wire);
}

AOC_REGISTER_TEST(test_carried_attack_keeps_turning_to_a_circling_player);

// Only the Minotaur's axe strike turns the creature through a montage that carries it (the recordings show its facing following the player through the whole swing).  Every other attack
// that does not stop the creature and has no Rotate To keeps the facing it began with while the montage carries it.  The Ashen Haunt's Rebuke cannot show this: the world keeps its single
// swing (its hit lands by a volume the world cannot place).  The test gives the Wolf's Bite, a carried lunge, a tree that says it does not stop the creature and has no Rotate To; at a
// player who runs round it from the first tick, its facing stays where the lunge began.
void test_carried_attack_outside_the_list_keeps_its_facing(Fixture& f) {
    std::printf("a carried attack that does not stop the creature and has no Rotate To (the Wolf's Bite in a tree that says so) keeps the facing it began the lunge with while the player runs round it\n");
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kDt = 1.0 / 15.0;
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
    const fs::path tmp = fs::temp_directory_path() / ("aoc_creature_carried_hold_" + unique_temp_suffix());   // (a folder of its own: two gates can run this program at once)
    fs::create_directories(tmp, ec);
    const std::string table = (tmp / "abilities.tsv").string();
    {
        std::ofstream out(table, std::ios::binary);
        out << "archetype\tability\ttask\tcooldown\tstop_movement\torder\tmin_distance\tmax_distance\tmax_angle\tgates\tunknown\tfight\tconds\twhy\tstarts\trotate\tarrival\n"
            << wolf_path << "\t" << bite->row.guid << "\tBite\t0.5\t0\t0\t-1\t5000\t-1\t\t0\t1\t\t\t\t\t-1\n";
    }
    combat::NpcAbilityData tables;
    std::string err;
    const bool loaded = tables.load(table, dir + "npc_montages.tsv", dir + "npc_targeting.tsv", err);
    fs::remove_all(tmp, ec);
    CHECKF(loaded, "the Wolf's tree with the Bite at stop_movement=0 and no Rotate To loads (%s)", err.c_str());
    if (!loaded) return;

    // from the first tick of the lunge the player runs round the place it began from, at the distance he had
    bool started = false;
    double angle = 0, radius = 0, cx = 0, cy = 0;
    const LungeSteer steer = [&](double, const LungeSample* prev, double& px, double& py, const world::WorldActor& c) {
        if (!started) {
            if (!prev || !prev->cast_began || prev->ability != bite->row.guid) return false;
            started = true;
            cx = c.x;
            cy = c.y;
            angle = std::atan2(py - cy, px - cx);
            radius = std::max(200.0, std::hypot(px - cx, py - cy));
        }
        angle += 90.0 * kPi / 180.0 * kDt;
        px = cx + radius * std::cos(angle);
        py = cy + radius * std::sin(angle);
        return true;
    };
    std::vector<LungeSample> samples;
    const bool ran = run_lunge_fight(f, tables, wolf_path, 30.0, 0, 800, steer, samples, nullptr, false, -1, nullptr, 40000.0);
    CHECK(ran, "a fight of 30 s against a player who runs round the Wolf from the first tick of its carried Bite runs");
    if (!ran) return;
    size_t casts = 0, carried = 0;
    double turned = 0, circled = 0;
    for (size_t i = 0; i < samples.size(); ++i) {
        if (!samples[i].cast_began || samples[i].ability != bite->row.guid) continue;
        ++casts;
        size_t last = i;   // the end of that cast
        while (last + 1 < samples.size() && samples[last + 1].montage != 0 && !samples[last + 1].cast_began) ++last;
        size_t carried_last = i;
        for (size_t k = i + 1; k <= last; ++k) {
            if (samples[k].active == 1) {
                ++carried;
                carried_last = k;
            }
        }
        if (carried_last > i) {
            turned = std::fabs(wrap180(samples[carried_last].yaw - samples[i].yaw));
            const double a0 = std::atan2(samples[i].py - samples[i].wy, samples[i].px - samples[i].wx);
            const double a1 = std::atan2(samples[carried_last].py - samples[carried_last].wy, samples[carried_last].px - samples[carried_last].wx);
            circled = std::fabs(wrap180((a1 - a0) * 180.0 / kPi));
        }
        break;   // (the first one is enough)
    }
    std::printf("  %zu Bite cast(s); the first carried it for %zu ticks, the player went %.0f deg round it in them and its facing turned %.1f deg\n", casts, carried, circled, turned);
    CHECKF(casts >= 1 && carried >= 3, "it plays its Bite and the montage carries it (%zu casts, %zu carried ticks)", casts, carried);
    CHECKF(circled >= 25.0 && turned <= 10.0, "... and it keeps the facing it began the lunge with while the player runs round it (%.0f deg round, facing turned %.1f deg; 10 allowed)", circled, turned);
}

AOC_REGISTER_TEST(test_carried_attack_outside_the_list_keeps_its_facing);

// A Rotate To track whose target is gone (dead, out of the world) turns nothing, so it must not keep the creature's walk from turning it either: a creature that walks with such a track faces where it
// walks, it does not walk on sideways with a frozen facing.
void test_walk_turns_a_creature_whose_track_lost_its_target(Fixture& f) {
    std::printf("a Wolf that walks north with a track whose target is gone turns to face where it walks\n");
    combat_rig::World w;
    CHECK(w.problem.empty(), "a world for the walk");
    if (!w.problem.empty()) return;
    const uint64_t wolf = w.spawn(wolf_archetype(f), 0, 0, 7, 0);
    CHECK(wolf != 0, "a Wolf that stands");
    if (!wolf) return;
    double now = 0;
    const auto run = [&](int ticks) {
        for (int i = 0; i < ticks; ++i) {
            now += 1.0 / 15.0;
            w.npcs->note_player(77, 0, 500, now);   // (a player stands near: the Wolf is awake)
            w.npcs->tick(now);
        }
    };
    run(3);
    w.npcs->face_toward(wolf, 1000.0, 0.0);   // (it faces east)
    run(1);
    const world::WorldActor* a = w.registry.find(wolf);
    const double start_x = a->x, start_y = a->y;
    CHECK(w.npcs->order_move(wolf, nav::Vec3{start_x, start_y + 2000.0, a->z}, 400.0, 0.0), "it is told to walk 20 m north");
    CHECK(w.npcs->track_target(wolf, [](nav::Vec3&) { return false; }, 360.0, 0.0), "it gets a track whose target is gone");
    run(15);
    a = w.registry.find(wolf);
    const double walked = a->y - start_y;
    const double off = std::fabs(wrap180(a->yaw_degrees - 90.0));
    std::printf("  in 1 s it walked %.0f cm north and faces %.1f deg off north\n", walked, off);
    CHECKF(walked > 100.0, "it walks north (%.0f cm in 1 s)", walked);
    CHECKF(off <= 10.0, "... facing where it walks, not frozen in its old facing (%.1f deg off north)", off);
}

AOC_REGISTER_TEST(test_walk_turns_a_creature_whose_track_lost_its_target);
