// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_mob_shared_reads_tests.inc
//
//  Registered tests of test_worldd_combat: the two places where the creatures' hits and the rest of the world read the same thing.
//   - A lingering zone's reach is the catalog's one zone volume (the shape, size and boxes of the targeting record the zone names), read once for the class zones and for the
//     creatures: a sphere a creature's hit opens is placed, a zone whose record is a box keeps its box and is not placed as a sphere.
//   - The NetGUID the world gives an actor: a player's is his session's pawn, a pet's (it stands in the players' list too) and a creature's are the registry's.
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================

namespace mob_shared {

// The program's CHECK prints only a failure; these checks print the pass too, so the green run's log holds the line of the same check that failed red.
#define MOB_SHARED(cond, ...) do { \
    if (cond) { std::printf("  [ok ] "); ++g_pass; } \
    else { std::printf("  [FAIL] "); ++g_fail; } \
    std::printf(__VA_ARGS__); std::printf("\n"); \
} while (0)

constexpr int64_t kOozingZone = 6064629976727486795ll;        // GoblinShaman_OozingBomb_LingeringEffect
constexpr int64_t kTotemChargeZone = 6064628973376634880ll;   // CyclopsShaman_TotemChargeHit_LingeringEffect (its record is a box)
constexpr int64_t kGraveDustZone = 6064629857491025923ll;     // GraveyardGolem_GraveDustCloud_LingeringEffect (its record is a box)

// R2 (one read for both): the zones the creatures open come through the volume the catalog reads for every zone.
void test_the_creature_zones_read_the_one_zone_volume(Fixture& f) {
    std::printf("the creatures' lingering zones read as the catalog's one zone volume: a sphere is placed by a creature's hit, a box keeps its box and is not\n");
    std::string error;
    // a fresh read of the file: the shared catalog's zones may hold a volume an earlier test gave them
    auto content = content::AbilityContent::open(f.path, error);
    MOB_SHARED(content.has_value(), "the ability tables open (%s)", error.c_str());
    if (!content) return;
    struct Want {
        int64_t     zone;
        const char* name;
        bool        sphere;
        double      radius;
    };
    const Want wants[] = {{mob_children::kFireBombZone, "the Fire Bomb's fire", true, 300.0},
                          {kOozingZone, "the Oozing Bomb's ooze", true, 200.0},
                          {kTotemChargeZone, "the Cyclops Shaman's totem charge", false, 0.0},
                          {kGraveDustZone, "the Graveyard Golem's dust cloud", false, 0.0}};
    for (const Want& w : wants) {
        auto row = content->zone(w.zone, error);
        std::optional<content::TargetingDefRow> def;
        if (row && row->targeting_guid != 0) def = content->targeting_def(row->targeting_guid, error);
        MOB_SHARED(row && def, "%s: the zone and its own targeting record are in the data", w.name);
        if (!row || !def) continue;
        row->volume = content::zone_volume_of(*def);
        MOB_SHARED(row->placeable() == w.sphere, "%s %s a sphere a creature's hit places (%s, %.0f cm)", w.name, w.sphere ? "is" : "is not", row->volume.shape.c_str(), row->volume.radius);
        if (w.sphere) MOB_SHARED(std::fabs(row->volume.radius - w.radius) < 1e-6, "%s reaches %.0f cm (%.0f)", w.name, w.radius, row->volume.radius);
        else MOB_SHARED(!row->volume.boxes.empty(), "%s keeps its box for the class side's reading (%zu box(es))", w.name, row->volume.boxes.size());
        // the catalog the fights use holds the same volume for a zone no test changed
        const content::ZoneRow* held = f.catalog->zone(w.zone);
        if (!w.sphere) MOB_SHARED(held && !held->placeable() && held->volume.shape == row->volume.shape && held->volume.boxes.size() == row->volume.boxes.size(), "%s: the catalog holds the same volume", w.name);
    }
}

AOC_REGISTER_TEST(test_the_creature_zones_read_the_one_zone_volume);

// R2 (the guid of an actor): a player, a pet and a creature, each by the guid it is known by.
void test_the_world_names_a_player_a_pet_and_a_creature_by_their_own_guids(Fixture& f) {
    std::printf("the world names a player by his pawn's guid, and a pet (it stands in the players' list too) and a creature by their actors' guids\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string path = creature_path(f, mob_children::kFlamespeaker);
    MOB_SHARED(!path.empty(), "the Blueprint table has the Flamespeaker");
    if (path.empty()) return;
    BanditOpen made;
    MOB_SHARED(make_bandit_open(f, tables, path, made), "the world makes the open for the Flamespeaker (verdict: %s)", spawns::verdict_name(made.why));
    if (!made.open) return;
    mob_children::Field field(f, tables, 0, 100);
    MOB_SHARED(field.ok(), "the fight's world is made");
    if (!field.ok()) return;
    field.caster = field.spawn(path, 0, 1700, 2000, made.open);
    const uint64_t pet = field.w.spawn(pets::kDefinitions[0].archetype, 150, 100, 25, 0);
    MOB_SHARED(field.caster != 0 && pet != 0 && field.world.pet_summoned(pet, field.player.actor_id()), "the Flamespeaker and a Spirit Guardian are in the world");
    if (field.caster == 0 || pet == 0) return;
    const world::WorldActor* creature = field.w.registry.find(field.caster);
    const world::WorldActor* guardian = field.w.registry.find(pet);
    MOB_SHARED(creature && guardian, "both stand in the registry");
    if (!creature || !guardian) return;
    const uint64_t player = field.player.actor_id();
    MOB_SHARED(field.world.actor_guid(player) == combat_rig::rig_pawn_guid(player), "the player is named by his session's pawn");
    MOB_SHARED(field.world.actor_guid(field.caster) == creature->actor.guid, "the creature is named by its actor's guid");
    MOB_SHARED(field.world.actor_guid(pet) == guardian->actor.guid, "the pet is named by its actor's guid, not as a player");
    MOB_SHARED(field.world.actor_guid(pet) != combat_rig::rig_pawn_guid(pet), "... which is not the pawn guid a player of that id would have");
    MOB_SHARED(field.world.actor_guid(987654321) == aoc::protocol::emit::FIntrepidNetworkGUID{}, "an id nothing names has no guid");
}

AOC_REGISTER_TEST(test_the_world_names_a_player_a_pet_and_a_creature_by_their_own_guids);

}  // namespace mob_shared
