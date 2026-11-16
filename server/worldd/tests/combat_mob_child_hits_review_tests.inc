// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_mob_child_hits_review_tests.inc
//
//  Registered tests of test_worldd_combat, the second part of the mob child hits' tests: what a creature's hit puts on a player stays what it was unless a child, a zone or an
//  effect's tick of the creature does it (a bear's bite, no bleeding); a child's own requirement is asked (the Skin Walker's curse comes with a quarter of its bites); an overlap hit
//  of a zone lands on the player who entered or left it; a self hit's zone does not open at the player's feet; a zone whose record is a box is not placed as a sphere; and the
//  one debuff the data allows once (its in-place limit) ticks once when two casters put it on.
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================

namespace mob_children {

/// A zone's targeting record as the data step will write it (a shape and a radius) and, for a rule no real zone that plays shows, its overlap roles; undone when the test ends so the
/// next test finds the zone as the tables hold it.
struct ZoneTweak {
    content::ZoneRow*                           row = nullptr;
    content::ZoneVolume                         volume;
    std::map<std::string, std::vector<int64_t>> hits;

    ZoneTweak(const abilities::Catalog& catalog, int64_t zone, double r, const char* s) : row(const_cast<content::ZoneRow*>(catalog.zone(zone))) {
        if (!row) return;
        volume = row->volume;
        hits = row->hits;
        row->volume.shape = s;
        row->volume.radius = r;
    }
    ~ZoneTweak() {
        if (!row) return;
        row->volume = volume;
        row->hits = hits;
    }
    ZoneTweak(const ZoneTweak&) = delete;
    ZoneTweak& operator=(const ZoneTweak&) = delete;
};

constexpr const char* kSphere = "EHitboxShape::SHAPE_Sphere";
constexpr const char* kBox = "EHitboxShape::SHAPE_Box";

/// How many IncomingHits of one record, and of one source type, a player took.
size_t count_hits(const PipelinePlayer& p, int64_t hit, int64_t source_type) {
    size_t n = 0;
    for (const combat::IncomingHit& h : p.hits) n += (h.hit_guid == hit && h.source_type == source_type) ? 1 : 0;
    return n;
}

/// The smallest gap between two ticks of one hit record that landed as an effect's tick (infinity when there are fewer than two).
double smallest_tick_gap(const PipelinePlayer& p, int64_t hit) {
    std::vector<double> at;
    for (const combat::IncomingHit& h : p.hits) {
        if (h.hit_guid == hit && h.source_type == content::kSourceTypeEffect) at.push_back(h.at);
    }
    std::sort(at.begin(), at.end());
    double gap = std::numeric_limits<double>::infinity();
    for (size_t i = 1; i < at.size(); ++i) gap = std::min(gap, at[i] - at[i - 1]);
    return gap;
}

constexpr int64_t kBearBiteHit = 109031297013375ll;
constexpr int64_t kBearBleed = 109031309334146ll;
constexpr int64_t kCursedBiteHit = 6064631035758911447ll;
constexpr int64_t kCurseApplicationHit = 6064631046485704719ll;
constexpr int64_t kBogBash = 6064633333309964292ll;
constexpr int64_t kMudPoolZone = 6064633333237547009ll;
constexpr int64_t kMudPoolSnare = 6064633254059573303ll;
constexpr int64_t kOozingBomb = 6064629976325095562ll;
constexpr int64_t kOozingZone = 6064629976727486795ll;
constexpr int64_t kOozingFirstHit = 6064632104850424044ll;
constexpr int64_t kRainOfBone = 6064631946110764175ll;

}  // namespace mob_children

// R5 (nothing else changes): a creature's own hit lands as it always did, its damage and no more.  The data's bear bite carries a bleeding of its own (Bear_Bite_Bleed_DoT_Effect on
// the hit itself, not on a child), which the world has never put on a player, and which this milestone does not take on: the bite hurts, and the player holds no bleeding.
void test_a_creatures_own_hit_puts_no_debuff_on_the_player(Fixture& f) {
    std::printf("a bear's bite hurts the player and puts no bleeding on him: a creature's own hit keeps landing its damage only\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string path = creature_path(f, "/Beast/Bear/Bear.Default__Bear_C");
    MOB_CHILD(!path.empty(), "the Blueprint table has the Bear");
    if (path.empty()) return;
    BanditOpen made;
    MOB_CHILD(make_bandit_open(f, tables, path, made), "the world makes the open for the Bear (verdict: %s)", spawns::verdict_name(made.why));
    if (!made.open) return;
    mob_children::Field field(f, tables, 0, 100);
    MOB_CHILD(field.ok(), "the fight's world is made");
    if (!field.ok()) return;
    field.caster = field.spawn(path, 0, 400, 2000, made.open);
    MOB_CHILD(field.caster != 0, "the Bear is put in the world 3 m from the player");
    if (field.caster == 0) return;
    field.world.add_threat(field.caster, field.player.actor_id(), 500.0, 0.0);
    const double dt = 1.0 / 15.0;
    int most_bleeding = 0;
    for (double now = dt; now <= 25.0 + 1e-9; now += dt) {
        field.tick(now);
        most_bleeding = std::max(most_bleeding, field.stacks(mob_children::kBearBleed));
    }
    field.player.rig().take();
    size_t bites = 0, clean_bites = 0, with_effects = 0;
    for (const combat::IncomingHit& h : field.player.hits) {
        if (h.hit_guid != mob_children::kBearBiteHit) continue;
        ++bites;
        clean_bites += (h.flags & stats::kHitMiss) == 0 ? 1 : 0;
        with_effects += h.effects.empty() ? 0 : 1;
    }
    MOB_CHILD(clean_bites >= 2, "the Bear's bite landed on the player (%zu bites, %zu of them hits)", bites, clean_bites);
    MOB_CHILD(with_effects == 0 && most_bleeding == 0, "no bite carried a bleeding to the player, and he never held one (%zu bites with effects, at most %d stack(s))", with_effects, most_bleeding);
}

AOC_REGISTER_TEST(test_a_creatures_own_hit_puts_no_debuff_on_the_player);

// R1 (when and with what): a child hit of the data carries its own requirement, which the player's hits and the effects' hits ask before they land.  The Skin Walkers' bite chains
// a curse that asks `RandDouble() < 0.25` (a quarter of the bites), so of the bites that land the curse comes with about one in four, not with every one.
void test_a_child_with_a_requirement_lands_only_when_it_holds(Fixture& f) {
    std::printf("the Skin Walker's bite chains a curse that asks a quarter chance: a child's own requirement is asked\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string path = creature_path(f, "/Beast/Canine/RVR_SPH_Cult_Hound.Default__RVR_SPH_Cult_Hound_C");
    const abilities::HitDef* child = f.catalog->hit(mob_children::kCurseApplicationHit);
    MOB_CHILD(!path.empty() && child && child->row.requirement_expr != 0, "the Blueprint table has the Cult Hound and the data's curse child has a requirement (expression %lld)",
              child ? static_cast<long long>(child->row.requirement_expr) : 0ll);
    if (path.empty() || !child) return;
    const content::ExprRow* requirement = f.catalog->expr(child->row.requirement_expr);
    MOB_CHILD(requirement && requirement->text.find("RandDouble() < 0.25") != std::string::npos, "the requirement is the data's quarter chance");
    BanditOpen made;
    MOB_CHILD(make_bandit_open(f, tables, path, made), "the world makes the open for the Cult Hound (verdict: %s)", spawns::verdict_name(made.why));
    if (!made.open) return;
    mob_children::Field field(f, tables, 0, 100);
    MOB_CHILD(field.ok(), "the fight's world is made");
    if (!field.ok()) return;
    field.caster = field.spawn(path, 0, 300, 2000, made.open);
    MOB_CHILD(field.caster != 0, "the Cult Hound is put in the world 2 m from the player");
    if (field.caster == 0) return;
    field.world.add_threat(field.caster, field.player.actor_id(), 500.0, 0.0);
    const double dt = 1.0 / 15.0;
    for (double now = dt; now <= 120.0 + 1e-9; now += dt) field.tick(now);
    field.player.rig().take();
    size_t bites = 0, children = 0;
    for (const combat::IncomingHit& h : field.player.hits) {
        bites += h.hit_guid == mob_children::kCursedBiteHit ? 1 : 0;
        children += h.hit_guid == mob_children::kCurseApplicationHit ? 1 : 0;
    }
    MOB_CHILD(bites >= 20, "the Cult Hound bites often enough to read the chance (%zu bites)", bites);
    MOB_CHILD(children * 10 <= bites * 6, "the curse child landed with fewer than 6 of 10 bites, not with each (%zu children of %zu bites; the data's chance is 1 in 4)", children, bites);
}

AOC_REGISTER_TEST(test_a_child_with_a_requirement_lands_only_when_it_holds);

// R1 (zones: overlap hits): the hit a zone lands when a player walks in reaches that player, and the one it lands when he walks out reaches him, not the players who stay.  No zone a
// creature opens and plays has overlap hits that the data lets the world place, so the Fire Bomb's fire is given two (the burn's and the fire burn's tick hits, each told apart by its
// own record) and one second player walks into it and out again.
void test_an_overlap_hit_reaches_the_player_who_entered_or_left(Fixture& f) {
    std::printf("a zone's begin-overlap hit lands on the player who walked in and its end-overlap hit on the player who walked out, not on the players who stayed\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string path = creature_path(f, mob_children::kFlamespeaker);
    MOB_CHILD(!path.empty() && f.catalog->zone(mob_children::kFireBombZone), "the Blueprint table has the Flamespeaker and the data has the fire");
    if (path.empty()) return;
    BanditOpen made;
    MOB_CHILD(make_bandit_open(f, tables, path, made), "the world makes the open for the Flamespeaker (verdict: %s)", spawns::verdict_name(made.why));
    if (!made.open) return;
    mob_children::ZoneTweak tweak(*f.catalog, mob_children::kFireBombZone, 300.0, mob_children::kSphere);
    if (!tweak.row) return;
    tweak.row->hits["OnBeginOverlapHitsIds"] = {mob_children::kGenericBurnTick};
    tweak.row->hits["OnEndOverlapHitsIds"] = {mob_children::kFireBombBurnTick};
    mob_children::Field field(f, tables, 0, 100, true);
    MOB_CHILD(field.ok(), "the fight's world is made with two players");
    if (!field.ok()) return;
    field.place_second(900, 100);   // 9 m from the impact: outside the 3 m fire
    field.caster = field.spawn(path, 0, 1700, 2000, made.open);
    MOB_CHILD(field.caster != 0, "the Flamespeaker is put in the world 16 m from the first player");
    if (field.caster == 0) return;
    field.world.add_threat(field.caster, field.player.actor_id(), 500.0, 0.0);
    const double dt = 1.0 / 15.0;
    bool killed = false, walked_in = false, walked_out = false;
    double impact_at = -1;
    for (double now = dt; now <= 20.0 + 1e-9; now += dt) {
        field.tick(now);
        if (!killed && field.casts1.count(mob_children::kFireBomb) >= 1 && now > field.casts1.first(mob_children::kFireBomb) + 2.7) {
            std::string why;
            killed = field.world.gm_kill(field.caster, now, why);
        }
        if (impact_at < 0) {
            for (const combat::IncomingHit& h : field.player.hits) {
                if (h.hit_guid == mob_children::kFireBombChild) impact_at = h.at;
            }
        }
        if (impact_at >= 0 && !walked_in && now >= impact_at + 1.5) {
            walked_in = true;
            field.place_second(200, 100);   // into the fire
        }
        if (impact_at >= 0 && !walked_out && now >= impact_at + 3.5) {
            walked_out = true;
            field.place_second(900, 100);   // and out of it
        }
        if (impact_at >= 0 && now >= impact_at + 5.0) break;
    }
    field.player.rig().take();
    field.player2.rig().take();
    const int64_t zone = content::kSourceTypeZone;
    const size_t begin1 = mob_children::count_hits(field.player, mob_children::kGenericBurnTick, zone);
    const size_t begin2 = mob_children::count_hits(field.player2, mob_children::kGenericBurnTick, zone);
    const size_t end1 = mob_children::count_hits(field.player, mob_children::kFireBombBurnTick, zone);
    const size_t end2 = mob_children::count_hits(field.player2, mob_children::kFireBombBurnTick, zone);
    MOB_CHILD(killed && walked_in && walked_out, "the caster is killed after its bomb left, and the second player walked in and out of the fire");
    MOB_CHILD(begin1 == 1, "the player who stood in the fire when it opened got the begin-overlap hit once, not again when the other came in (%zu)", begin1);
    MOB_CHILD(begin2 == 1, "the player who walked in got the begin-overlap hit once (%zu)", begin2);
    MOB_CHILD(end2 == 1, "the player who walked out got the end-overlap hit (%zu)", end2);
    MOB_CHILD(end1 == 0, "the player who stayed in the fire got no end-overlap hit (%zu)", end1);
}

AOC_REGISTER_TEST(test_an_overlap_hit_reaches_the_player_who_entered_or_left);

// R1 (zones: where): a hit that reaches no enemy (the Bog Brute's bash spawns its mud pool with a hit on itself) opens its zone where that hit lands, which is the caster's place and
// not the place of a player the move reached: it opens no zone at the player's feet.  (The zone has its record's 300 cm, as the data step will write it.)
void test_a_self_hits_zone_does_not_open_at_the_players_feet(Fixture& f) {
    std::printf("the Bog Brute's bash spawns its mud pool with a hit on itself: the pool does not open at the player's feet and does not snare him\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string path = creature_path(f, "/Humanoid/BogBrute/BogBrute.Default__BogBrute_C");
    const abilities::AbilityDef* bash = f.catalog->ability(mob_children::kBogBash);
    MOB_CHILD(!path.empty() && bash && f.catalog->zone(mob_children::kMudPoolZone), "the Blueprint table has the Bog Brute and the data has its bash and its mud pool");
    if (path.empty() || !bash) return;
    BanditOpen made;
    MOB_CHILD(make_bandit_open(f, tables, path, made), "the world makes the open for the Bog Brute (verdict: %s)", spawns::verdict_name(made.why));
    if (!made.open) return;
    mob_children::ZoneTweak tweak(*f.catalog, mob_children::kMudPoolZone, 300.0, mob_children::kSphere);
    if (!tweak.row) return;
    mob_children::Field field(f, tables, 0, 100);
    MOB_CHILD(field.ok(), "the fight's world is made");
    if (!field.ok()) return;
    field.caster = field.spawn(path, 0, 300, 2000, made.open);
    MOB_CHILD(field.caster != 0, "the Bog Brute is put in the world 2 m from the player");
    if (field.caster == 0) return;
    field.world.add_threat(field.caster, field.player.actor_id(), 500.0, 0.0);
    rally_group::LogCapture log;
    const double dt = 1.0 / 15.0;
    int snare = 0;
    for (double now = dt; now <= 60.0 + 1e-9; now += dt) {
        field.tick(now);
        snare = std::max(snare, field.stacks(mob_children::kMudPoolSnare));
    }
    field.player.rig().take();
    MOB_CHILD(field.casts1.count(mob_children::kBogBash) >= 1, "the Bog Brute plays its bash (%zu casts)", field.casts1.count(mob_children::kBogBash));
    MOB_CHILD(rally_group::lines_with(log.text(), {"BogBrute_BogBash_MudPool"}) == 0, "no mud pool was opened for it (%zu log lines)", rally_group::lines_with(log.text(), {"BogBrute_BogBash_MudPool"}));
    MOB_CHILD(snare == 0, "the player never held the mud pool's snare (at most %d stack(s))", snare);
}

AOC_REGISTER_TEST(test_a_self_hits_zone_does_not_open_at_the_players_feet);

// R1 (zones: what shape): a zone whose record is a box has no sphere radius the world can use (the record keeps a leftover one), so it is not placed as a sphere of it.  The Fire Bomb's
// fire is given a box record of the same 300 cm: the impact still lands, and the fire opens nothing.
void test_a_zone_whose_record_is_a_box_is_not_placed_as_a_sphere(Fixture& f) {
    std::printf("a zone whose targeting record is a box is not placed as a sphere: the impact lands, the fire does not open\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string path = creature_path(f, mob_children::kFlamespeaker);
    MOB_CHILD(!path.empty() && f.catalog->zone(mob_children::kFireBombZone), "the Blueprint table has the Flamespeaker and the data has the fire");
    if (path.empty()) return;
    BanditOpen made;
    MOB_CHILD(make_bandit_open(f, tables, path, made), "the world makes the open for the Flamespeaker (verdict: %s)", spawns::verdict_name(made.why));
    if (!made.open) return;
    mob_children::ZoneTweak tweak(*f.catalog, mob_children::kFireBombZone, 300.0, mob_children::kBox);
    if (!tweak.row) return;
    mob_children::Field field(f, tables, 0, 100);
    MOB_CHILD(field.ok(), "the fight's world is made");
    if (!field.ok()) return;
    field.caster = field.spawn(path, 0, 1700, 2000, made.open);
    if (field.caster == 0) return;
    field.world.add_threat(field.caster, field.player.actor_id(), 500.0, 0.0);
    const double dt = 1.0 / 15.0;
    bool killed = false;
    double impact_at = -1;
    for (double now = dt; now <= 20.0 + 1e-9; now += dt) {
        field.tick(now);
        if (!killed && field.casts1.count(mob_children::kFireBomb) >= 1 && now > field.casts1.first(mob_children::kFireBomb) + 2.7) {
            std::string why;
            killed = field.world.gm_kill(field.caster, now, why);
        }
        if (impact_at < 0) {
            for (const combat::IncomingHit& h : field.player.hits) {
                if (h.hit_guid == mob_children::kFireBombChild) impact_at = h.at;
            }
        }
        if (impact_at >= 0 && now >= impact_at + 4.0) break;
    }
    field.player.rig().take();
    MOB_CHILD(impact_at >= 0, "the bomb's impact landed on the player (at %.2f s)", impact_at);
    MOB_CHILD(mob_children::count_hits(field.player, mob_children::kFireBombZoneHit, content::kSourceTypeZone) == 0, "the fire of a box record opened nothing: no tick reached the player (%zu)",
              mob_children::count_hits(field.player, mob_children::kFireBombZoneHit, content::kSourceTypeZone));
}

AOC_REGISTER_TEST(test_a_zone_whose_record_is_a_box_is_not_placed_as_a_sphere);

// R1 (effects: the in-place limit of 1): two Flamespeakers put the same debuff on one player, the player holds one instance of it (the data's limit), and the world runs that one
// instance's ticks once: one tick every 2 s for the fire's burn and one every 1 s for the Burning, not one stream for each caster.  (Level 7: a level-25 elite's impact kills the
// rig's level-20 player, and the debuffs end with him.)
void test_two_casters_of_the_same_debuff_tick_it_once(Fixture& f) {
    std::printf("two Flamespeakers' fires put the same burn on one player: the one instance ticks once, not once for each caster\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string path = creature_path(f, mob_children::kBoog);
    MOB_CHILD(!path.empty(), "the Blueprint table has Boog (an elite Flamespeaker)");
    if (path.empty()) return;
    BanditOpen made;
    MOB_CHILD(make_bandit_open(f, tables, path, made), "the world makes the open for Boog (verdict: %s)", spawns::verdict_name(made.why));
    if (!made.open) return;
    mob_children::ZoneTweak tweak(*f.catalog, mob_children::kFireBombZone, 300.0, mob_children::kSphere);
    if (!tweak.row) return;
    mob_children::Field field(f, tables, 0, 100);
    MOB_CHILD(field.ok(), "the fight's world is made");
    if (!field.ok()) return;
    field.caster = field.spawn(path, -400, 1700, 2000, made.open);
    field.caster2 = field.spawn(path, 400, 1700, 2000, made.open);
    MOB_CHILD(field.caster != 0 && field.caster2 != 0, "two Flamespeakers are put in the world");
    if (field.caster == 0 || field.caster2 == 0) return;
    field.world.add_threat(field.caster, field.player.actor_id(), 500.0, 0.0);
    field.world.add_threat(field.caster2, field.player.actor_id(), 500.0, 0.0);
    const double dt = 1.0 / 15.0;
    bool killed1 = false, killed2 = false;
    double first_impact = -1;
    for (double now = dt; now <= 40.0 + 1e-9; now += dt) {
        field.tick(now);
        std::string why;
        if (!killed1 && field.casts1.count(mob_children::kFireBomb) >= 1 && now > field.casts1.first(mob_children::kFireBomb) + 2.7) killed1 = field.world.gm_kill(field.caster, now, why);
        if (!killed2 && field.casts2.count(mob_children::kFireBomb) >= 1 && now > field.casts2.first(mob_children::kFireBomb) + 2.7) killed2 = field.world.gm_kill(field.caster2, now, why);
        if (first_impact < 0) {
            for (const combat::IncomingHit& h : field.player.hits) {
                if (h.hit_guid == mob_children::kFireBombChild) first_impact = h.at;
            }
        }
        if (first_impact >= 0 && now >= first_impact + 24.0) break;   // the fires' 6 s and the 12 s the last application's burn lasts
    }
    field.player.rig().take();
    MOB_CHILD(killed1 && killed2, "both casters are killed after their first bomb left");
    size_t fire_ticks = 0, burn_ticks = 0, children = 0, applications = 0;
    for (const combat::IncomingHit& h : field.player.hits) {
        fire_ticks += (h.hit_guid == mob_children::kFireBombBurnTick && h.source_type == content::kSourceTypeEffect) ? 1 : 0;
        burn_ticks += (h.hit_guid == mob_children::kGenericBurnTick && h.source_type == content::kSourceTypeEffect) ? 1 : 0;
        children += h.hit_guid == mob_children::kFireBombChild ? 1 : 0;
        applications += (h.hit_guid == mob_children::kFireBombChild && !h.effects.empty()) ? 1 : 0;
    }
    MOB_CHILD(children == 2 && applications == 2, "both bombs landed their impact and put the Burning on (%zu impacts, %zu with the Burning)", children, applications);
    const double fire_gap = mob_children::smallest_tick_gap(field.player, mob_children::kFireBombBurnTick);
    const double burn_gap = mob_children::smallest_tick_gap(field.player, mob_children::kGenericBurnTick);
    MOB_CHILD(fire_ticks >= 4 && burn_ticks >= 4, "the fire's burn and the Burning both ticked on the player (%zu and %zu ticks)", fire_ticks, burn_ticks);
    MOB_CHILD(fire_gap >= 1.9, "the fire's burn ticks every 2 s on the one instance (the closest two ticks are %.2f s apart)", fire_gap);
    MOB_CHILD(burn_gap >= 0.9, "the Burning ticks every 1 s on the one instance (the closest two ticks are %.2f s apart)", burn_gap);
}

AOC_REGISTER_TEST(test_two_casters_of_the_same_debuff_tick_it_once);

// R1 (who: a pet): a Summoner's pet stands among the players the fire reaches (its port is in the players' list): it takes the fire's ticks as a player does.
void test_a_pet_in_the_fire_takes_its_ticks(Fixture& f) {
    std::printf("a Summoner's pet that stands in the Flamespeaker's fire takes its ticks, as the player does\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string path = creature_path(f, mob_children::kFlamespeaker);
    MOB_CHILD(!path.empty() && f.catalog->zone(mob_children::kFireBombZone), "the Blueprint table has the Flamespeaker and the data has the fire");
    if (path.empty()) return;
    BanditOpen made;
    MOB_CHILD(make_bandit_open(f, tables, path, made), "the world makes the open for the Flamespeaker (verdict: %s)", spawns::verdict_name(made.why));
    if (!made.open) return;
    mob_children::ZoneTweak tweak(*f.catalog, mob_children::kFireBombZone, 300.0, mob_children::kSphere);
    if (!tweak.row) return;
    mob_children::Field field(f, tables, 0, 100);
    MOB_CHILD(field.ok(), "the fight's world is made");
    if (!field.ok()) return;
    field.caster = field.spawn(path, 0, 1700, 2000, made.open);
    const uint64_t pet = field.w.spawn(pets::kDefinitions[0].archetype, 150, 100, 25, 0);
    MOB_CHILD(field.caster != 0 && pet != 0 && field.world.pet_summoned(pet, field.player.actor_id()), "the Flamespeaker and a Spirit Guardian 1.5 m from the player are in the world");
    if (field.caster == 0 || pet == 0) return;
    field.world.add_threat(field.caster, field.player.actor_id(), 500.0, 0.0);
    const double before = field.world.health_of(pet), max = field.world.max_health_of(pet);
    const double dt = 1.0 / 15.0;
    bool killed = false;
    double impact_at = -1;
    for (double now = dt; now <= 20.0 + 1e-9; now += dt) {
        field.tick(now);
        if (!killed && field.casts1.count(mob_children::kFireBomb) >= 1 && now > field.casts1.first(mob_children::kFireBomb) + 2.7) {
            std::string why;
            killed = field.world.gm_kill(field.caster, now, why);
        }
        if (impact_at < 0) {
            for (const combat::IncomingHit& h : field.player.hits) {
                if (h.hit_guid == mob_children::kFireBombChild) impact_at = h.at;
            }
        }
        if (impact_at >= 0 && now >= impact_at + 7.0) break;
    }
    field.player.rig().take();
    const double after = field.world.health_of(pet);
    MOB_CHILD(impact_at >= 0 && mob_children::count_hits(field.player, mob_children::kFireBombZoneHit, content::kSourceTypeZone) >= 4, "the bomb landed and its fire ticked on the player (%zu ticks)",
              mob_children::count_hits(field.player, mob_children::kFireBombZoneHit, content::kSourceTypeZone));
    MOB_CHILD(max > 0 && before > 0 && after < before, "the pet stood in the fire and lost Health to it (%.1f of %.1f before, %.1f after)", before, max, after);
}

AOC_REGISTER_TEST(test_a_pet_in_the_fire_takes_its_ticks);

// R1 (the old launch fallback): the plain goblin shaman's Oozing Bomb has no hit of its own that hurts, so while its zone cannot be placed the world lands the zone's first hit where
// the bomb came down (the old way).  With the zone's record in the tables the child opens the zone and the zone lands that first hit itself: once for each bomb, not once by the old
// way and again by the zone.
void test_the_oozing_bombs_first_hit_lands_once_when_its_zone_is_placed(Fixture& f) {
    std::printf("the Oozing Bomb's zone is placed: its first hit lands once for each bomb, from the zone, and the old direct hit is not added to it\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string path = creature_path(f, "/GoblinShaman/Goblin_Shaman.Default__Goblin_Shaman_C");
    MOB_CHILD(!path.empty() && f.catalog->zone(mob_children::kOozingZone), "the Blueprint table has the plain goblin shaman and the data has the Oozing Bomb's zone");
    if (path.empty()) return;
    BanditOpen made;
    MOB_CHILD(make_bandit_open(f, tables, path, made), "the world makes the open for the shaman (verdict: %s)", spawns::verdict_name(made.why));
    if (!made.open) return;
    mob_children::ZoneTweak tweak(*f.catalog, mob_children::kOozingZone, 200.0, mob_children::kSphere);   // (the record's own 200 cm sphere)
    if (!tweak.row) return;
    mob_children::Field field(f, tables, 0, 100);
    MOB_CHILD(field.ok(), "the fight's world is made");
    if (!field.ok()) return;
    field.caster = field.spawn(path, 0, 350, 2000, made.open);
    MOB_CHILD(field.caster != 0, "the shaman is put in the world 2.5 m from the player");
    if (field.caster == 0) return;
    field.world.add_threat(field.caster, field.player.actor_id(), 500.0, 0.0);
    const double dt = 1.0 / 15.0;
    for (double now = dt; now <= 30.0 + 1e-9; now += dt) field.tick(now);
    field.player.rig().take();
    const size_t bombs = field.casts1.count(mob_children::kOozingBomb);
    const size_t from_zone = mob_children::count_hits(field.player, mob_children::kOozingFirstHit, content::kSourceTypeZone);
    const size_t direct = mob_children::count_hits(field.player, mob_children::kOozingFirstHit, content::kSourceTypeProjectile) +
                          mob_children::count_hits(field.player, mob_children::kOozingFirstHit, content::kSourceTypeAbility);
    MOB_CHILD(bombs >= 3, "the shaman throws its bomb over and over (%zu casts)", bombs);
    MOB_CHILD(from_zone >= 3 && from_zone <= bombs, "the zone landed the first hit once for each bomb that came down (%zu hits, %zu casts)", from_zone, bombs);
    MOB_CHILD(direct == 0, "the old direct hit was not added: nothing landed the first hit as the bomb's or the ability's own (%zu)", direct);
}

AOC_REGISTER_TEST(test_the_oozing_bombs_first_hit_lands_once_when_its_zone_is_placed);

// R2 and R5 (the moves the rows say play): the Grim Harvester's Rain of Bone hurts only through the hit its shard projectile fires, and that projectile is fired by a hit the world
// does not follow (no notify of the montage flies it).  The attack test counts the move, but it would play its whole 8 s montage and land nothing: the world does not offer a move it
// cannot deliver, and the Grim Harvester fights with the rest of its tree as it did.
void test_a_carrier_the_world_cannot_deliver_is_not_played(Fixture& f) {
    std::printf("the Grim Harvester's Rain of Bone cannot be delivered (its shards are fired by a hit): the world does not play it, and the Harvester fights on\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string path = creature_path(f, "/Monster/Undead/Skeleton/Skeleton_Reaper1.Default__Skeleton_Reaper1_C");
    const abilities::AbilityDef* rain = f.catalog->ability(mob_children::kRainOfBone);
    MOB_CHILD(!path.empty() && rain && combat::hurts_enemies(*rain, *f.catalog, f.data->ids().health), "the Blueprint table has the Grim Harvester and the data's Rain of Bone counts as an attack through its child");
    if (path.empty() || !rain) return;
    BanditOpen made;
    MOB_CHILD(make_bandit_open(f, tables, path, made), "the world makes the open for the Grim Harvester (verdict: %s)", spawns::verdict_name(made.why));
    if (!made.open) return;
    mob_children::Field field(f, tables, 0, 100);
    MOB_CHILD(field.ok(), "the fight's world is made");
    if (!field.ok()) return;
    field.caster = field.spawn(path, 0, 300, 2000, made.open);
    MOB_CHILD(field.caster != 0, "the Grim Harvester is put in the world 2 m from the player");
    if (field.caster == 0) return;
    field.world.add_threat(field.caster, field.player.actor_id(), 500.0, 0.0);
    const double dt = 1.0 / 15.0;
    for (double now = dt; now <= 90.0 + 1e-9; now += dt) {
        // the move waits for the Harvester's health under 70 %: it is kept under 40 % so the tree would offer it
        if (combat::Fighter* body = field.world.fighter_of(field.caster)) {
            if (const stats::StatValue* max = body->stats().find(content::kGuidMaxHealth)) body->stats().set_current(content::kGuidHealth, static_cast<float>(max->value * 0.4));
        }
        field.tick(now);
    }
    field.player.rig().take();
    size_t hits = 0;
    for (const combat::IncomingHit& h : field.player.hits) hits += (h.flags & stats::kHitMiss) == 0 ? 1 : 0;
    MOB_CHILD(field.casts1.count(mob_children::kRainOfBone) == 0, "the Grim Harvester never plays Rain of Bone (%zu casts)", field.casts1.count(mob_children::kRainOfBone));
    MOB_CHILD(hits >= 3, "the Grim Harvester fights on with the rest of its tree (%zu hits on the player)", hits);
}

AOC_REGISTER_TEST(test_a_carrier_the_world_cannot_deliver_is_not_played);
