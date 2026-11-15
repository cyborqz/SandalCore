// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_mob_clock_tests.inc
//
//  Registered tests of test_worldd_combat: the clocks a creature's state tree reads before it lets the creature use a move.  A move that waits for "ten seconds into the fight" is played ten
//  seconds into the fight and not before, whatever name the tree gives its timer; the real tables of the game's creatures decide which moves those are.
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================

namespace mob_clock {

/// The task of `path`'s Blueprint for the ability `name` whose reach is `max_distance` cm (-1: the first task of that ability).
inline const combat::CreatureAbility* task_with_reach(Fixture& f, const combat::NpcAbilityData& tables, const std::string& path, const char* name, double max_distance) {
    const abilities::AbilityDef* def = f.catalog->ability_named(name);
    const std::vector<combat::CreatureAbility>* list = tables.abilities_of(path);
    if (!def || !list) return nullptr;
    for (const combat::CreatureAbility& t : *list) {
        if (t.ability != def->row.guid) continue;
        if (max_distance < 0 || std::abs(t.max_distance - max_distance) < 0.5) return &t;
    }
    return nullptr;
}

/// The first condition of `kind` in a task's conditions (nullptr: none).
inline const combat::CondAtom* atom_of(const combat::CreatureAbility& t, combat::CondAtom::Kind kind) {
    for (const combat::CondGroup& g : t.groups) {
        for (const combat::CondToken& token : g) {
            if (token.atom.kind == kind) return &token.atom;
        }
    }
    return nullptr;
}

/// When (seconds into the fight) each cast of `ability` began.
inline std::vector<double> cast_times(const std::vector<LungeSample>& samples, int64_t ability) {
    std::vector<double> out;
    for (const LungeSample& s : samples) {
        if (s.cast_began && s.ability == ability) out.push_back(s.t);
    }
    return out;
}

}  // namespace mob_clock

// The Tidebreaker's rapid bite and the Raven's raid wait for the fight's clock under names the tree gives its timer that no other tree uses ("CombatTimer", "Temporary Combat Timer ...").
// The timer task sits in the state the fight begins in, so its elapsed time is the seconds since the fight began, which is what the creature's fight clock already says.
void test_fight_clock_under_other_names_in_the_real_tables(Fixture& f) {
    std::printf("the Tidebreaker's rapid bite is read from the tree (it waits for the fight's clock, so the tables hold it as a move the creature may play), and the Raven's raid is held back until flying creatures exist\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string tide = creature_path(f, "/Beast/Tidebreaker/Tidebreaker.Default__Tidebreaker_C");
    const std::string raven = creature_path(f, "/Beast/Bird/Raven/Raven.Default__Raven_C");
    CHECK(!tide.empty() && !raven.empty() && tables.abilities_of(tide) && tables.abilities_of(raven), "the Blueprint table has the Tidebreaker and the Raven, and the creature tables list their abilities");
    if (tide.empty() || raven.empty() || !tables.abilities_of(tide) || !tables.abilities_of(raven)) return;
    using K = combat::CondAtom::Kind;
    const combat::CreatureAbility* rapid = mob_clock::task_with_reach(f, tables, tide, "Tidebreaker_RapidBite", 300);
    CHECK(rapid != nullptr, "the Tidebreaker's tree has a rapid bite within 3 m (besides the one it pounces with)");
    if (rapid) {
        const combat::CondAtom* clock = mob_clock::atom_of(*rapid, K::FightTime);
        CHECKF(!rapid->unknown && clock && clock->op == combat::CondAtom::Ge && std::abs(clock->value - 10.0) < 1e-6,
               "the rapid bite is a move the tables can say: it waits until the fight is 10 seconds old (unknown %d, clock %s %.1f)", rapid->unknown ? 1 : 0, clock ? "found" : "missing", clock ? clock->value : -1.0);
    }
    // The Raven's raid waits for the fight to be more than 12 seconds old, which the tables now say, but its montage carries the bird 15 m backwards through the air: a creature that cannot fly would do
    // that along the ground.  The raid stays out of the creature's moves (the tables hold it unknown, with the reason) until flying creatures exist.
    const combat::CreatureAbility* raid = mob_clock::task_with_reach(f, tables, raven, "Raven_Raid", -1);
    CHECK(raid != nullptr, "the Raven's tree has a raid");
    if (raid) {
        CHECKF(raid->unknown && raid->why.find("flying") != std::string::npos, "the raid is held back until flying creatures exist (unknown %d, the table says: %s)", raid->unknown ? 1 : 0, raid->why.c_str());
    }
}

AOC_REGISTER_TEST(test_fight_clock_under_other_names_in_the_real_tables);

// A Tidebreaker against a player who stands still: it bites and claws from the start and plays its rapid bite once the fight is ten seconds old, never before.
void test_tidebreaker_plays_its_rapid_bite_after_ten_seconds(Fixture& f) {
    std::printf("a Tidebreaker against a player who stands still plays its rapid bite from the tenth second of the fight on, and not before\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string tide = creature_path(f, "/Beast/Tidebreaker/Tidebreaker.Default__Tidebreaker_C");
    const abilities::AbilityDef* rapid = f.catalog->ability_named("Tidebreaker_RapidBite");
    CHECK(!tide.empty() && f.motion.find(tide) && tables.abilities_of(tide) && rapid, "the Blueprint table has the Tidebreaker, the creature tables list its abilities and the ability data has the rapid bite");
    if (tide.empty() || !f.motion.find(tide) || !tables.abilities_of(tide) || !rapid) return;
    BanditOpen made;   // (the open the world makes for a creature of a generic Blueprint: its tree's abilities are the records it loads)
    CHECKF(make_bandit_open(f, tables, tide, made), "the world makes an open for the Tidebreaker (verdict: %s)", spawns::verdict_name(made.why));
    if (!made.open) return;
    std::vector<LungeSample> samples;
    std::vector<combat::IncomingHit> got;
    if (!run_lunge_fight(f, tables, tide, 40.0, 0, 800, nullptr, samples, &got, false, -1, nullptr, 40000.0, made.open)) return;
    const std::vector<double> rapid_casts = mob_clock::cast_times(samples, rapid->row.guid);
    size_t other_casts = 0;
    double first_other = -1;
    for (const LungeSample& s : samples) {
        if (!s.cast_began || s.ability == rapid->row.guid) continue;
        ++other_casts;
        if (first_other < 0) first_other = s.t;
    }
    std::printf("  rapid bite cast at:");
    for (const double t : rapid_casts) std::printf(" %.2f", t);
    std::printf(" s; %zu other casts, the first at %.2f s; %zu hits reached the player\n", other_casts, first_other, got.size());
    CHECKF(other_casts >= 3 && first_other >= 0 && first_other < 8.0, "the Tidebreaker fights from the start with the moves it had before (%zu other casts, the first at %.2f s)", other_casts, first_other);
    CHECKF(!rapid_casts.empty(), "the rapid bite is played in a 40 second fight (%zu casts)", rapid_casts.size());
    CHECKF(rapid_casts.empty() || rapid_casts.front() >= 10.0, "... and never before the fight is ten seconds old (the first at %.2f s)", rapid_casts.empty() ? -1.0 : rapid_casts.front());
}

AOC_REGISTER_TEST(test_tidebreaker_plays_its_rapid_bite_after_ten_seconds);
