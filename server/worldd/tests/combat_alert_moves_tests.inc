// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_alert_moves_tests.inc
//
//  Registered tests of test_worldd_combat: the alert moves of the creatures (a growl, a yawp, a rally, a rear-up): fight moves of an ability that hurts nobody.  The creature plays the montage with no
//  target in the cast, stands still when its tree says so and starts the cooldowns when the animation is over; nothing lands.  A move whose hits change someone's stats, or whose hit the tables do not
//  hold (a zone, a spray), is not an alert.
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================

#define MOVE_CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); std::fflush(stdout); ++g_pass; } \
    else { std::printf("  [FAIL] %s\n", msg); std::fflush(stdout); ++g_fail; } \
} while (0)
#define MOVE_CHECKF(cond, ...) do { \
    char move_line[1024]; \
    std::snprintf(move_line, sizeof move_line, __VA_ARGS__); \
    if (cond) { std::printf("  [ok ] %s\n", move_line); std::fflush(stdout); ++g_pass; } \
    else { std::printf("  [FAIL] %s\n", move_line); std::fflush(stdout); ++g_fail; } \
} while (0)

namespace alert_moves {

/// A fight of 'seconds' of one creature of `path`, whose tree is the rows of `table_rows` (a table text without its header), against a player who stands 8 m away from the start.
struct AlertFight {
    bool                          ran = false;
    std::vector<LungeSample>      samples;
    std::vector<combat::IncomingHit> hits;
};

/// The casts of a fight: when each began, which ability, and whether its item named a target.
struct CastAt {
    double  t = 0;
    int64_t ability = 0;
    bool    has_target = false;
    double  seconds = 0;   // how long the item held its montage
};

inline std::vector<CastAt> casts_of(const std::vector<LungeSample>& samples, double dt) {
    std::vector<CastAt> out;
    for (const LungeSample& s : samples) {
        if (s.cast_began) {
            CastAt c;
            c.t = s.t;
            c.ability = s.ability;
            c.has_target = s.item_target.ObjectId != 0;
            out.push_back(c);
        }
        if (!out.empty() && s.montage != 0 && s.ability == out.back().ability) out.back().seconds += dt;
    }
    return out;
}

}  // namespace alert_moves

// The Grem growls before it bites.  A Grem that has stood in the world for 13 s (its tree's clock is past 12) meets a player: the first thing it does is the Snarl, 1.2 s long, with no target in the cast
// (a Snarl is no attack: nothing lands), and its first bite comes after the growl is over.  The Snarl is not repeated before its cooldown (9 s) has passed.
void test_grem_growls_before_it_bites(Fixture& f) {
    std::printf("a Grem that meets a player after 13 s in the world growls first (1.2 s, no target in the cast, nothing lands), bites after the growl, and growls again only after its cooldown\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string path = creature_path(f, "/Beast/Rabid/Rabid.Default__Rabid_C");
    const abilities::AbilityDef* snarl = f.catalog->ability_named("Rabid_Snarl");
    CHECK(!path.empty() && f.motion.find(path) && tables.abilities_of(path) && snarl, "the Blueprint table has the Rabid, the creature tables list its abilities and the ability data has the Snarl");
    if (path.empty() || !f.motion.find(path) || !tables.abilities_of(path) || !snarl) return;
    BanditOpen made;
    CHECKF(make_bandit_open(f, tables, path, made), "the world makes an open for the Grem (verdict: %s)", spawns::verdict_name(made.why));
    if (!made.open) return;
    constexpr double kDt = 1.0 / 15.0, kArrives = 13.0;
    std::vector<LungeSample> samples;
    std::vector<combat::IncomingHit> got;
    const LungeSteer steer = [&](double now, const LungeSample*, double& px, double& py, const world::WorldActor&) {
        if (now >= kArrives && py > 900.0) {   // (the player has stood far away, out of its notice, and comes up to 8 m)
            py = 800.0;
            return true;
        }
        return false;
    };
    if (!run_lunge_fight(f, tables, path, 40.0, 0, 5000, steer, samples, &got, false, -1, nullptr, 40000.0, made.open)) return;
    const std::vector<alert_moves::CastAt> casts = alert_moves::casts_of(samples, kDt);
    std::string line;
    for (const alert_moves::CastAt& c : casts) {
        const abilities::AbilityDef* d = f.catalog->ability(c.ability);
        line += (line.empty() ? "" : ", ") + std::string(d ? d->row.name : "?") + " " + std::to_string(c.t).substr(0, 5) + (c.has_target ? "" : " (no target)");
    }
    std::printf("  casts: %s\n", line.c_str());
    CHECK(!casts.empty() && casts.front().t >= kArrives, "the Grem casts nothing before the player comes up (13 s)");
    if (casts.empty()) return;
    CHECKF(casts.front().ability == snarl->row.guid, "the first thing it does in the fight is the Snarl (the first cast is %s at %.2f s)", f.catalog->ability(casts.front().ability) ? f.catalog->ability(casts.front().ability)->row.name.c_str() : "?", casts.front().t);
    CHECK(casts.front().ability == snarl->row.guid && !casts.front().has_target, "the Snarl's cast names no target (Targets is empty, as in the recorded casts of growls)");
    CHECKF(casts.front().seconds > 1.0 && casts.front().seconds < 1.5, "the growl lasts about 1.2 s (%.2f s)", casts.front().seconds);
    if (casts.size() >= 2) {
        CHECKF(casts[1].t - casts[0].t >= 1.2 && casts[1].t - casts[0].t < 4.0, "the first bite comes after the growl is over, not during it (%.2f s after the growl began)", casts[1].t - casts[0].t);
        CHECK(casts[1].ability != snarl->row.guid && casts[1].has_target, "... and it is an attack that names the player");
    }
    // nothing the growl does lands: no hit reaches the player during it
    size_t during = 0;
    for (const combat::IncomingHit& h : got) {
        if (h.at >= casts.front().t && h.at < casts.front().t + casts.front().seconds) ++during;
    }
    CHECKF(during == 0, "no hit reaches the player while the Grem growls (%zu hits)", during);
    for (size_t i = 1; i < casts.size(); ++i) {
        if (casts[i].ability == snarl->row.guid) {
            CHECKF(casts[i].t - casts.front().t >= 9.0, "the Snarl comes again only after its cooldown (9 s): the second at %.2f s, the first at %.2f s", casts[i].t, casts.front().t);
            break;
        }
    }
}

AOC_REGISTER_TEST(test_grem_growls_before_it_bites);

// An alert whose hit only puts an effect on the creature's own side (the Tidebreaker's Yawp) is played; a move whose hit the tables do not hold (the Arsonist's ring of fire: a zone) is not.  The
// Tidebreaker's tree is cut to the move under test and its Claw.
void test_alerts_with_an_effect_for_allies_play_and_zones_do_not(Fixture& f) {
    std::printf("an alert whose hit only puts an effect on the creature's own side is played with no target; a move with an empty hit (a zone) is not an alert and is never played\n");
    namespace fs = std::filesystem;
    combat::NpcAbilityData real;
    if (!load_creature_tables(real)) return;
    const std::string path = creature_path(f, "/Beast/Tidebreaker/Tidebreaker.Default__Tidebreaker_C");
    const abilities::AbilityDef* yawp = f.catalog->ability_named("Tidebreaker_Yawp");
    const abilities::AbilityDef* ring = f.catalog->ability_named("Bandit_Arsonist_FlameRing");
    const abilities::AbilityDef* claw = f.catalog->ability_named("Tidebreaker_Claw");
    CHECK(!path.empty() && f.motion.find(path) && real.abilities_of(path) && yawp && ring && claw, "the Blueprint table has the Tidebreaker and the ability data has its Yawp and Claw and the Arsonist's Flame Ring");
    if (path.empty() || !f.motion.find(path) || !real.abilities_of(path) || !yawp || !ring || !claw) return;
    std::string dir = NPC_MOTION_PATH;
    dir.erase(dir.find_last_of("/\\") + 1);
    std::error_code ec;
    const fs::path tmp = fs::temp_directory_path() / ("aoc_creature_alerts_" + unique_temp_suffix());   // (a folder of its own: two gates can run this program at once)
    fs::create_directories(tmp, ec);
    const std::string header =
        "archetype\tability\ttask\tcooldown\tstop_movement\torder\tmin_distance\tmax_distance\tmax_angle\tgates\tunknown\tfight\tconds\twhy\tstarts\trotate\tarrival";
    // the tree: the move under test first (the Tidebreaker's own Yawp row: its task, cooldown, gate and conditions), then the Claw within 3.6 m
    const auto fight = [&](const abilities::AbilityDef* under_test, alert_moves::AlertFight& out, bool real_yawp) {
        const std::string table = (tmp / "abilities.tsv").string();
        {
            std::ofstream o(table, std::ios::binary);
            o << header << "\n";
            if (real_yawp) o << path << "\t" << under_test->row.guid << "\tYawp\t300\t0\t15\t-1\t-1\t-1\tYawp\t0\t1\tc0:hp(0.5) a0:fc(ge,5)\t\t\t\t175\n";
            else o << path << "\t" << under_test->row.guid << "\t\t0\t0\t15\t-1\t-1\t-1\t\t0\t1\t\t\t\t\t-1\n";
            o << path << "\t" << claw->row.guid << "\t\t0\t1\t24\t-1\t360\t90\t\t0\t1\t\t\t\t360:0\t-1\n";
        }
        combat::NpcAbilityData tables;
        std::string err;
        const bool loaded = tables.load(table, dir + "npc_montages.tsv", dir + "npc_targeting.tsv", err);
        CHECKF(loaded, "the Tidebreaker's tree with %s and its Claw loads (%s)", under_test->row.name.c_str(), err.c_str());
        if (!loaded) return;
        BanditOpen made;
        CHECKF(make_bandit_open(f, tables, path, made), "the world makes an open for the Tidebreaker with %s (verdict: %s)", under_test->row.name.c_str(), spawns::verdict_name(made.why));
        if (!made.open) return;
        // the real Yawp waits for the creature's health under a half (or five seconds): the test keeps it under a half so the move is tried
        const LungeStep low_health = [](LungeStepContext& c) {
            if (!c.after) return;
            combat::Fighter* body = c.world.fighter_of(c.creature);
            if (!body) return;
            const stats::StatValue* max = body->stats().find(content::kGuidMaxHealth);
            if (max) body->stats().set_current(content::kGuidHealth, static_cast<float>(max->value * 0.4));
        };
        const LungeStep step = real_yawp ? LungeStep(low_health) : LungeStep();
        out.ran = run_lunge_fight(f, tables, path, 25.0, 0, 800, nullptr, out.samples, &out.hits, false, -1, nullptr, 40000.0, made.open, 0, 0, step);
    };
    alert_moves::AlertFight with_yawp, with_ring;
    fight(yawp, with_yawp, true);
    fight(ring, with_ring, false);
    fs::remove_all(tmp, ec);
    CHECK(with_yawp.ran && with_ring.ran, "the two fights run");
    if (!with_yawp.ran || !with_ring.ran) return;
    const std::vector<alert_moves::CastAt> a = alert_moves::casts_of(with_yawp.samples, 1.0 / 15.0), b = alert_moves::casts_of(with_ring.samples, 1.0 / 15.0);
    size_t yawps = 0, rings = 0, claws_a = 0, claws_b = 0;
    bool yawp_without_target = true;
    for (const alert_moves::CastAt& c : a) {
        if (c.ability == yawp->row.guid) {
            ++yawps;
            yawp_without_target = yawp_without_target && !c.has_target;
        }
        claws_a += c.ability == claw->row.guid ? 1 : 0;
    }
    for (const alert_moves::CastAt& c : b) {
        rings += c.ability == ring->row.guid ? 1 : 0;
        claws_b += c.ability == claw->row.guid ? 1 : 0;
    }
    std::printf("  with the Yawp: %zu Yawps (first at %.2f s), %zu Claws;  with the Flame Ring: %zu rings, %zu Claws\n", yawps, a.empty() ? -1.0 : a.front().t, claws_a, rings, claws_b);
    CHECKF(yawps == 1 && yawp_without_target && claws_a >= 3, "the Tidebreaker plays its Yawp once (its cooldown is 300 s), with no target in the cast, and goes on clawing (%zu Yawps, %zu Claws)", yawps, claws_a);
    CHECKF(!a.empty() && yawps >= 1, "the Yawp is played in the 25 s fight");
    CHECKF(rings == 0 && claws_b >= 3, "the ring of fire, whose hit the tables do not hold, is never played and the Tidebreaker claws (%zu rings, %zu Claws)", rings, claws_b);
}

AOC_REGISTER_TEST(test_alerts_with_an_effect_for_allies_play_and_zones_do_not);

// R3: a move whose ability itself lists effects (the Dunir Tank's Fortify and Scorch) is not an alert: it is held (not played) instead of shown with nothing done.  The tank's
// other moves still play.  (The moves whose point is a spawn, a shift, a potion or a state the tables cannot show are held in the reader's Waiting list; see out/notes.md.)
void test_ability_level_effects_are_held(Fixture& f) {
    std::printf("a fight move whose ability itself applies an effect is held rather than shown; an effect-only alert is still an alert\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string path = creature_path(f, "/Monster/Undead/Dunir/DunirTank/Dunir_Tank.Default__Dunir_Tank_C");
    const abilities::AbilityDef* fortify = f.catalog->ability_named("Dunir_Tank_Fortify");
    const abilities::AbilityDef* scorch = f.catalog->ability_named("Dunir_Tank_Scorch");
    const abilities::AbilityDef* yawp = f.catalog->ability_named("Tidebreaker_Yawp");
    MOVE_CHECK(!path.empty() && f.motion.find(path) && tables.abilities_of(path) && fortify && scorch && yawp,
          "the Blueprint table has the Dunir Tank and the ability data has its Fortify, Scorch and the Yawp");
    if (path.empty() || !f.motion.find(path) || !tables.abilities_of(path) || !fortify || !scorch || !yawp) return;
    MOVE_CHECK(!combat::is_alert_move(*fortify, *f.catalog, f.data->ids().health), "the Fortify, whose ability lists an effect and has no hit, is not an alert");
    MOVE_CHECK(!combat::is_alert_move(*scorch, *f.catalog, f.data->ids().health), "the Scorch, whose ability lists an effect and has no hit, is not an alert");
    MOVE_CHECK(combat::is_alert_move(*yawp, *f.catalog, f.data->ids().health), "the Yawp, whose hits only put effects on its own side, is still an alert");
    BanditOpen made;
    MOVE_CHECKF(make_bandit_open(f, tables, path, made), "the world makes an open for the Dunir Tank (verdict: %s)", spawns::verdict_name(made.why));
    if (!made.open) return;
    std::vector<LungeSample> samples;
    if (!run_lunge_fight(f, tables, path, 30.0, 0, 800, nullptr, samples, nullptr, false, -1, nullptr, 40000.0, made.open)) return;
    const std::vector<alert_moves::CastAt> casts = alert_moves::casts_of(samples, 1.0 / 15.0);
    size_t fortifies = 0, scorches = 0, others = 0;
    for (const alert_moves::CastAt& c : casts) {
        fortifies += c.ability == fortify->row.guid ? 1 : 0;
        scorches += c.ability == scorch->row.guid ? 1 : 0;
        if (c.ability != fortify->row.guid && c.ability != scorch->row.guid) ++others;
    }
    std::printf("  %zu Fortifies, %zu Scorches, %zu other casts in 30 s\n", fortifies, scorches, others);
    MOVE_CHECKF(fortifies == 0 && scorches == 0, "the tank never plays the held ability-effect moves (%zu Fortifies, %zu Scorches)", fortifies, scorches);
    MOVE_CHECKF(others > 0, "it still plays its real attacks (%zu other casts)", others);
}

AOC_REGISTER_TEST(test_ability_level_effects_are_held);
