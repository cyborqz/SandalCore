// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_goblin_rally_tests.inc
//
//  Registered tests of test_worldd_combat: the goblins' Rally group.  The Rally is an alert (a cast that hurts nobody: no target in the cast, nothing lands) that the goblins' tree opens once the fight
//  is a number of seconds old that the tree draws once between 20 and 30; the group's two Headbutts have no hit in their montages and are held back, as are the moves of the birds that fly.
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================

namespace goblin_rally {

/// A cast of a fight: when it began, which ability, whether its item named a target.
struct CastAt {
    double  t = 0;
    int64_t ability = 0;
    bool    has_target = false;
};

inline std::vector<CastAt> casts_of(const std::vector<LungeSample>& samples) {
    std::vector<CastAt> out;
    for (const LungeSample& s : samples) {
        if (s.cast_began) out.push_back({s.t, s.ability, s.item_target.ObjectId != 0});
    }
    return out;
}

}  // namespace goblin_rally

// The goblins' Rally group waits for a number of seconds the tree draws once (20 to 30 s into the fight), the Rally is an alert with an effect for the goblin's allies, and the group's two Headbutts have no
// hit in their montages (they would land as hits nothing shows), so the tables hold them back.  The tables are the real ones.
void test_goblin_rally_group_in_the_real_tables(Fixture& f) {
    std::printf("the goblins' Rally waits for the random number of seconds their tree draws, the two Headbutts are held back, and the Raven's retreat and the birds' raids are held back until flying mobs exist\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string goblin = creature_path(f, "/Humanoid/Goblin/Goblin_Bonebinder.Default__Goblin_Bonebinder_C");
    const std::string raven = creature_path(f, "/Beast/Bird/Raven/Raven.Default__Raven_C");
    const std::string hawk = creature_path(f, "/Beast/Bird/Hawk/Hawk.Default__Hawk_C");
    CHECK(!goblin.empty() && tables.abilities_of(goblin) && !raven.empty() && tables.abilities_of(raven), "the Blueprint table has the Bonebinder goblin and the Raven and the creature tables list their abilities");
    if (goblin.empty() || !tables.abilities_of(goblin) || raven.empty() || !tables.abilities_of(raven)) return;
    using K = combat::CondAtom::Kind;
    const auto task_of = [&](const std::string& path, const char* name) -> const combat::CreatureAbility* {
        const abilities::AbilityDef* d = f.catalog->ability_named(name);
        if (!d || !tables.abilities_of(path)) return nullptr;
        for (const combat::CreatureAbility& t : *tables.abilities_of(path)) {
            if (t.ability == d->row.guid && t.fight) return &t;
        }
        return nullptr;
    };
    const combat::CreatureAbility* rally = task_of(goblin, "Goblin_Rally");
    CHECK(rally != nullptr, "the goblin's tree has a Rally");
    if (rally) {
        const combat::CondAtom* roll = nullptr;
        for (const combat::CondGroup& g : rally->groups) {
            for (const combat::CondToken& t : g) {
                if (t.atom.kind == K::FightRoll) roll = &t.atom;
            }
        }
        CHECKF(!rally->unknown && roll && roll->op == combat::CondAtom::Ge && roll->chance == 1.0 && roll->low == 20.0 && roll->high == 30.0,
               "the Rally is a move the tables can say: it waits for the fight to be a number of seconds old that the tree draws between 20 and 30 (unknown %d, draw %s)", rally->unknown ? 1 : 0, roll ? "found" : "missing");
    }
    for (const char* held : {"Goblin_Headbutt", "Goblin_StationaryHeadbutt"}) {
        const combat::CreatureAbility* t = task_of(goblin, held);
        CHECKF(t && t->unknown && t->why.find("hit notify") != std::string::npos, "%s is held back: its montage has no hit (unknown %d, the table says: %s)", held, t && t->unknown ? 1 : 0, t ? t->why.c_str() : "no task");
    }
    const combat::CreatureAbility* retreat = task_of(raven, "Raven_Retreat");
    CHECKF(retreat && retreat->unknown && retreat->why.find("flying") != std::string::npos, "the Raven's retreat is held back until flying creatures exist (unknown %d, the table says: %s)", retreat && retreat->unknown ? 1 : 0, retreat ? retreat->why.c_str() : "no task");
    if (!hawk.empty() && tables.abilities_of(hawk)) {
        const combat::CreatureAbility* raid = task_of(hawk, "Hawk_Raid");
        CHECKF(raid && raid->unknown && raid->why.find("flying") != std::string::npos, "the Hawk's raid is held back until flying creatures exist (unknown %d, the table says: %s)", raid && raid->unknown ? 1 : 0, raid ? raid->why.c_str() : "no task");
    }
}

AOC_REGISTER_TEST(test_goblin_rally_group_in_the_real_tables);

// A goblin against a player who stands still: it swings from the start, rallies once its fight is 20 to 30 s old (an alert: no target in the cast, nothing lands), and does not play the Headbutts.
void test_goblin_rallies_after_twenty_seconds(Fixture& f) {
    std::printf("a Goblin against a player who stands still rallies once its fight is 20 to 30 s old, with no target in the cast, and not again before its cooldown (40 s); it never headbutts\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string path = creature_path(f, "/Humanoid/Goblin/Goblin_Bonebinder.Default__Goblin_Bonebinder_C");
    const abilities::AbilityDef* rally = f.catalog->ability_named("Goblin_Rally");
    const abilities::AbilityDef* head = f.catalog->ability_named("Goblin_Headbutt");
    const abilities::AbilityDef* still = f.catalog->ability_named("Goblin_StationaryHeadbutt");
    CHECK(!path.empty() && f.motion.find(path) && tables.abilities_of(path) && rally && head && still, "the Blueprint table has the Bonebinder goblin and the ability data has its Rally and Headbutts");
    if (path.empty() || !f.motion.find(path) || !tables.abilities_of(path) || !rally || !head || !still) return;
    BanditOpen made;
    CHECKF(make_bandit_open(f, tables, path, made), "the world makes an open for the goblin (verdict: %s)", spawns::verdict_name(made.why));
    if (!made.open) return;
    std::vector<LungeSample> samples;
    std::vector<combat::IncomingHit> got;
    if (!run_lunge_fight(f, tables, path, 80.0, 0, 800, nullptr, samples, &got, false, -1, nullptr, 40000.0, made.open)) return;
    const std::vector<goblin_rally::CastAt> casts = goblin_rally::casts_of(samples);
    std::vector<double> rallies;
    size_t headbutts = 0, others = 0;
    bool rally_without_target = true;
    double first_other = -1;
    for (const goblin_rally::CastAt& c : casts) {
        if (c.ability == rally->row.guid) {
            rallies.push_back(c.t);
            rally_without_target = rally_without_target && !c.has_target;
        } else if (c.ability == head->row.guid || c.ability == still->row.guid) {
            ++headbutts;
        } else {
            ++others;
            if (first_other < 0) first_other = c.t;
        }
    }
    std::printf("  rally at:");
    for (const double t : rallies) std::printf(" %.2f", t);
    std::printf(" s; %zu other casts (the first at %.2f s); %zu headbutts\n", others, first_other, headbutts);
    CHECKF(others >= 6 && first_other >= 0 && first_other < 8.0, "the goblin fights from the start with the moves it had before (%zu other casts, the first at %.2f s)", others, first_other);
    CHECKF(!rallies.empty() && rally_without_target, "it rallies in an 80 s fight, with no target in the cast (%zu rallies)", rallies.size());
    CHECKF(!rallies.empty() && rallies.front() >= 20.0 && rallies.front() < 36.0, "... once its fight is 20 to 30 s old (the first at %.2f s of a fight that began within the first second)", rallies.empty() ? -1.0 : rallies.front());
    CHECKF(rallies.size() < 2 || rallies[1] - rallies[0] >= 40.0, "... and again only after the 40 s its Rally starts on the group's timer (%.2f s apart)", rallies.size() < 2 ? -1.0 : rallies[1] - rallies[0]);
    CHECKF(headbutts == 0, "it never headbutts: the Headbutts are held back (%zu)", headbutts);
}

AOC_REGISTER_TEST(test_goblin_rallies_after_twenty_seconds);
