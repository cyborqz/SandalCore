// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_ground_circles_review_tests.inc: the ground circles against the game's own records (included by combat_registered.inc after
//  combat_ground_circles_tests.inc, inside test_worldd_combat's anonymous namespace)
//
//    the volumes          a zone's volume is read from its targeting record as the records hold it: a sphere is no box, a size by expression is read
//    where a zone stands  a zone whose origin is the instigator stands at the caster, whatever the cast named
//    which zones open     every zone of the game's data opens with the cast that owns it, the talent-gated ones by their talent
//    the traps            a trap arms itself (DelayTime) and goes off once (ExpireAfterAppliedHits)
//    the effects          an infinite effect a zone puts on a creature ends when the creature leaves the zone and when the zone ends
//    the sides            a zone's ally list reaches a friend, not a creature that is no one's enemy or friend; a second player who is not grouped with the caster is neither
//    the limit            the zones alive at once are bounded, and the one that goes closes its actor
//    the ticks            a zone lands a tick for every period of its life before its end, whatever moment it opened at (the tick on the end is the End's)
// ============================================================================

namespace ground {

/// The names of the zones whose actors opened in what the rig has seen, in order.
std::vector<std::string> opened_zones(const Rig& rig) {
    std::vector<std::string> out;
    for (const Seen& s : rig.log) {
        for (const bootstrap::ZoneOp& z : s.zones) {
            if (z.kind != bootstrap::ZoneOp::Kind::Open) continue;
            const content::ZoneRow* row = rig.catalog->zone(z.zone);
            out.push_back(row ? row->name : std::string("?"));
        }
    }
    return out;
}

/// How many times the actors of the zone called `name` were faded out or closed in what the rig has seen.
size_t zone_ops_of(const Rig& rig, const char* name, bootstrap::ZoneOp::Kind kind) {
    size_t n = 0;
    for (const Seen& s : rig.log) {
        for (const bootstrap::ZoneOp& z : s.zones) {
            const content::ZoneRow* row = rig.catalog->zone(z.zone);
            if (z.kind == kind && row && row->name == name) ++n;
        }
    }
    return n;
}

/// The guid of the zone a hit of the data names (0 when there is none).
int64_t zone_of_hit(Fixture& f, const char* hit) {
    const abilities::HitDef* h = f.catalog->hit_named(hit);
    return h && h->zones.size() == 1 ? h->zones.front() : 0;
}

/// An archetype of the motion table whose name holds `part` and that stands on `side` of a player (a chicken is no one's friend or enemy, a huntable animal is a friend), or "".
std::string archetype_on_side(Fixture& f, combat::Relation side, const char* part) {
    std::string out;
    for (const auto& [path, row] : f.motion.rows()) {
        if (path.find(part) == std::string::npos) continue;
        const std::vector<int64_t> aligns(row.alignments.begin(), row.alignments.end());
        if (f.data->relation(combat::CombatData::player_alignments(), aligns) == side) out = path;
    }
    return out;
}

}  // namespace ground

// ---- the volumes ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Every volume of a targeting record carries a BoxExtent (24 cm by default) whatever its shape, and a size the game computes sits in an expression column beside the raw one.  The
// Bear Trap's and the Hunting Trap's rows are read from the game data as they stand: their spheres are 150 cm, and no box.  The other records' volumes are given here as the JSON of the
// design records (the extraction writes the same).
void test_ground_circles_the_volumes_the_records_give(Fixture& f) {
    std::printf("a zone's volume: a sphere is a sphere of its radius and no box, a box is a box, a size by expression is read\n");
    // the real rows
    const abilities::AbilityDef* trap = f.catalog->ability_named("Ranger_BearTrap");
    CHECK(trap != nullptr && trap->zones.size() == 2, "the Bear Trap lists its two zones");
    if (trap) {
        for (const int64_t zone : trap->zones) {
            const content::ZoneRow* row = f.catalog->zone(zone);
            CHECKF(row != nullptr && row->volume.has_volume() && row->volume.shape == "EHitboxShape::SHAPE_Sphere" && row->volume.radius == 150.0 && row->volume.boxes.empty(),
                   "the Bear Trap's %s is a sphere of 150 cm and no box (%zu box(es), %.0f cm)", row ? row->name.c_str() : "?", row ? row->volume.boxes.size() : 0, row ? row->volume.radius : 0.0);
        }
    }
    const int64_t hunting = ground::zone_of_hit(f, "Hunting_TrapPlacement_Hit");
    const content::ZoneRow* hunting_row = hunting != 0 ? f.catalog->zone(hunting) : nullptr;
    CHECKF(hunting_row != nullptr && hunting_row->volume.radius == 150.0 && hunting_row->volume.boxes.empty(), "the Hunting Trap's zone is a sphere of 150 cm and no box (%zu box(es))",
           hunting_row ? hunting_row->volume.boxes.size() : 0);

    // records as the extraction writes them: an Aegis-like sphere whose BoxExtent is no size of its (a record's default 225 x 200 x 100)
    content::TargetingDefRow aegis;
    aegis.shape = "EHitboxShape::SHAPE_Sphere";
    aegis.radius = 450;
    aegis.angle = 90;
    aegis.volumes =
        R"json([{"Angle":90,"BoxExtent":{"X":225,"Y":200,"Z":100},"BoxExtentExpr":"Vector(225.0, 200.0, 100.0)","Location":{"X":0,"Y":0,"Z":0},"Rotation":{"Pitch":0,"Roll":0,"Yaw":0},"SphereRadius":450,"SphereRadiusExpr":"450.0","bEnableLOSCheck":true,"id":"EHitboxShape::SHAPE_Sphere"}])json";
    const content::ZoneVolume sphere = content::zone_volume_of(aegis);
    CHECKF(sphere.has_volume() && sphere.radius == 450.0 && sphere.boxes.empty() && sphere.los, "a sphere with a BoxExtent of its record's is a sphere of 450 cm and no box (%zu box(es))", sphere.boxes.size());

    // the Saga's box: 24 raw, 1000 x 2500 x 1000 by its expression, centred 5 m behind the caster
    content::TargetingDefRow saga;
    saga.shape = "EHitboxShape::SHAPE_Box";
    saga.radius = 24;
    saga.angle = 60;
    saga.volumes =
        R"json([{"Angle":60,"BoxExtent":{"X":24,"Y":24,"Z":24},"BoxExtentExpr":"Vector(1000.0, 2500.0, 1000.0)","Location":{"X":-500,"Y":0,"Z":0},"Rotation":{"Pitch":0,"Roll":0,"Yaw":0},"SphereRadius":24,"SphereRadiusExpr":"2500","bEnableLOSCheck":true,"id":"EHitboxShape::SHAPE_Box"}])json";
    const content::ZoneVolume box = content::zone_volume_of(saga);
    CHECKF(box.has_volume() && box.boxes.size() == 1 && box.boxes[0].ex == 1000.0 && box.boxes[0].ey == 2500.0 && box.boxes[0].ez == 1000.0 && box.boxes[0].cx == -500.0,
           "the Saga's box is 1000 x 2500 x 1000 by its expression and stands 500 cm behind (%zu box(es), %.0f x %.0f x %.0f)", box.boxes.size(), box.boxes.empty() ? 0.0 : box.boxes[0].ex,
           box.boxes.empty() ? 0.0 : box.boxes[0].ey, box.boxes.empty() ? 0.0 : box.boxes[0].ez);

    // a Pantheon sphere: 24 raw, 100 by its expression
    content::TargetingDefRow pantheon;
    pantheon.shape = "EHitboxShape::SHAPE_Sphere";
    pantheon.radius = 24;
    pantheon.volumes =
        R"json([{"BoxExtent":{"X":24,"Y":24,"Z":24},"BoxExtentExpr":"","Location":{"X":0,"Y":0,"Z":0},"SphereRadius":24,"SphereRadiusExpr":"100","id":"EHitboxShape::SHAPE_Sphere"}])json";
    CHECKF(content::zone_volume_of(pantheon).radius == 100.0, "a Pantheon sphere is 100 cm by its expression (%.0f)", content::zone_volume_of(pantheon).radius);

    // a dance's sphere is 500 cm, or 750 by a talent: the expression computes, so the raw 500 stands
    content::TargetingDefRow dance;
    dance.shape = "EHitboxShape::SHAPE_Sphere";
    dance.radius = 500;
    dance.volumes =
        R"json([{"BoxExtent":{"X":24,"Y":24,"Z":24},"Location":{"X":0,"Y":0,"Z":0},"SphereRadius":500,"SphereRadiusExpr":"const power = GetEffectCount(GetInstigator(), $#1:2$) > 0;\r\nSelectFloat(power, 750.0, 500.0)","id":"EHitboxShape::SHAPE_Sphere"}])json";
    CHECKF(content::zone_volume_of(dance).radius == 500.0 && content::zone_volume_of(dance).boxes.empty(), "a size the game computes keeps its raw value (%.0f)", content::zone_volume_of(dance).radius);

    // the Wall: three boxes, each with its own place and turn
    content::TargetingDefRow wall;
    wall.shape = "EHitboxShape::SHAPE_Box";
    wall.radius = 1;
    wall.volumes =
        R"json([{"BoxExtent":{"X":400,"Y":250,"Z":300},"BoxExtentExpr":"Vector(400.0, 250.0, 300.0)","Location":{"X":-175,"Y":350,"Z":350},"Rotation":{"Pitch":0,"Roll":-90,"Yaw":25},"SphereRadius":1,"id":"EHitboxShape::SHAPE_Box"},)json"
        R"json({"BoxExtent":{"X":400,"Y":500,"Z":400},"BoxExtentExpr":"Vector(400.0, 500.0, 400.0)","Location":{"X":-125,"Y":0,"Z":350},"Rotation":{"Pitch":0,"Roll":-90,"Yaw":0},"SphereRadius":1,"id":"EHitboxShape::SHAPE_Box"},)json"
        R"json({"BoxExtent":{"X":400,"Y":250,"Z":300},"BoxExtentExpr":"Vector(400.0, 250.0, 300.0)","Location":{"X":-175,"Y":-350,"Z":350},"Rotation":{"Pitch":0,"Roll":-90,"Yaw":-25},"SphereRadius":1,"id":"EHitboxShape::SHAPE_Box"}])json";
    const content::ZoneVolume wall_volume = content::zone_volume_of(wall);
    CHECKF(wall_volume.boxes.size() == 3 && wall_volume.boxes[0].yaw == 25.0 && wall_volume.boxes[2].yaw == -25.0 && wall_volume.boxes[1].ey == 500.0,
           "the Wall is three boxes with their own turns (%zu boxes)", wall_volume.boxes.size());
}
AOC_REGISTER_TEST(test_ground_circles_the_volumes_the_records_give);

// ---- where a zone stands -------------------------------------------------------------------------------------------------------------------------------------------------------------
// The data gives each zone an origin: the zones of a cast aimed at a place or at an enemy stand there (HitLocation), the Saga's, the Arcane Circle's and a trap's stand at the caster (Instigator,
// Owner), whatever the cast named.
void test_ground_circles_an_instigator_zone_stands_at_the_caster(Fixture& f) {
    std::printf("a zone whose origin is the instigator stands at the caster even when the cast names a place and a target; a HitLocation zone stands at the place\n");
    const std::string wolf_path = wolf_archetype(f);
    CHECK(!wolf_path.empty(), "the Wolf is in the motion table");
    if (wolf_path.empty()) return;
    Forge g(f, 3, {});
    CHECKF(g.ok(), "the Cleric and his world: %s %s", g.rig.problem.c_str(), g.w.problem.c_str());
    if (!g.ok()) return;
    const int64_t zone = ground::zone_of(f, "Cleric_ChainsOfRestraint");
    CHECK(zone != 0, "the data has the Chains' zone");
    if (zone == 0) return;
    g.rig.know({"Cleric_ChainsOfRestraint"});
    const auto open_of = [&](const char* origin, double t0, double offset = 0.0) {
        const auto given_zone = ground::give_zone(*g.rig.catalog, zone, origin, "EAbilitySystemAttachType::None", 500.0);
        const_cast<content::ZoneRow*>(g.rig.catalog->zone(zone))->spawn.rel_x = offset;   // (RelativeLocation: the guard puts the row back)
        const size_t seen = g.rig.log.size();
        g.rig.pipe->clear_cooldowns(t0);
        g.rig.set_current(content::kGuidMana, g.rig.pawn.stat(content::kGuidMaxMana));
        const ground::Cast cast = ground::cast_zone(g.rig, "Cleric_ChainsOfRestraint", t0, combat::Vec3{500, 0, 1000});
        ground::run(g.rig, g.world, t0, t0 + 8.0);
        std::optional<bootstrap::ZoneOp> open;
        for (size_t i = seen; i < g.rig.log.size(); ++i)
            for (const bootstrap::ZoneOp& z : g.rig.log[i].zones)
                if (z.kind == bootstrap::ZoneOp::Kind::Open && z.zone == zone) open = z;
        return std::make_pair(cast.cast, open);
    };
    const auto at_caster = open_of("EAbilitySystemOrigin::Instigator", 100.0);
    CHECK(at_caster.first && at_caster.second.has_value(), "the Chains are cast at a place with an Instigator origin and open their zone");
    if (at_caster.second) {
        CHECKF(at_caster.second->placed && !at_caster.second->attached && std::fabs(at_caster.second->x) < 1.0 && std::fabs(at_caster.second->y) < 1.0,
               "...at the caster (%.0f, %.0f), not at the place the cast named (500, 0)", at_caster.second->x, at_caster.second->y);
    }
    const auto owner = open_of("EAbilitySystemOrigin::Owner", 120.0);
    CHECK(owner.first && owner.second.has_value() && std::fabs(owner.second->x) < 1.0, "an Owner origin stands at the caster too");
    // the Lament's zone is the Bard's own cone: it runs on the server only, whatever origin its record names (the game shows no circle for it)
    {
        BardMobWorld bw(f, wolf_path, 2);
        CHECKF(bw.problem.empty(), "the Bard and his world: %s", bw.problem.c_str());
        const int64_t lament_zone = bw.problem.empty() ? ground::zone_of(f, "Bard_Song_Lament") : 0;
        const uint64_t lament_wolf = lament_zone != 0 ? bw.w.spawn(wolf_path, 600, 0, 10, 0) : 0;
        if (lament_zone != 0 && lament_wolf != 0) {
            bw.rig.know({"Bard_Song_Lament"});
            double t0 = 100.0;
            for (const char* origin : {"EAbilitySystemOrigin::Instigator", "EAbilitySystemOrigin::HitLocation"}) {
                const auto given_lament = ground::give_zone(*bw.rig.catalog, lament_zone, origin, "EAbilitySystemAttachType::None", 300.0, true);
                const size_t seen = bw.rig.log.size();
                bw.rig.pipe->clear_cooldowns(t0);
                const ground::Cast cast = ground::cast_zone(bw.rig, "Bard_Song_Lament", t0, std::nullopt, {lament_wolf});
                ground::run(bw.rig, *bw.world, t0, t0 + 4.0);
                std::optional<bootstrap::ZoneOp> open;
                for (size_t i = seen; i < bw.rig.log.size(); ++i)
                    for (const bootstrap::ZoneOp& z : bw.rig.log[i].zones)
                        if (z.kind == bootstrap::ZoneOp::Kind::Open && z.zone == lament_zone) open = z;
                // the Lament's zone is the Bard's own cone: it runs on the server only (the game draws no circle for it), whatever its record's origin says
                CHECKF(cast.cast && !open, "the Lament at the Wolf with %s: no zone actor is sent, the circle is the client's own effect (opened %s)", origin, open ? "1" : "0");
                t0 += 20.0;
            }
        }
    }
    const auto ahead = open_of("EAbilitySystemOrigin::Instigator", 140.0, 100.0);
    CHECK(ahead.first && ahead.second.has_value(), "a trap's record places it 1 m ahead of its origin: it is cast");
    if (ahead.second) CHECKF(std::fabs(ahead.second->x - 100.0) < 1.0 && std::fabs(ahead.second->y) < 1.0, "...and its actor stands 1 m ahead of the caster (%.0f, %.0f)", ahead.second->x, ahead.second->y);
    const auto at_hit = open_of("EAbilitySystemOrigin::HitLocation", 160.0);
    CHECK(at_hit.first && at_hit.second.has_value(), "the Chains are cast with a HitLocation origin and open their zone");
    if (at_hit.second) {
        CHECKF(at_hit.second->placed && std::fabs(at_hit.second->x - 500.0) < 1.0, "...at the place the cast was aimed at (%.0f, %.0f)", at_hit.second->x, at_hit.second->y);
    }
}
AOC_REGISTER_TEST(test_ground_circles_an_instigator_zone_stands_at_the_caster);

// ---- which zones open ------------------------------------------------------------------------------------------------------------------------------------------------------------------
// The data names a cast's zones in two places: the ability lists the ones that no hit of it names (the dances), and each hit that spawns a zone names it (the Chains', the Vine Field's, the Wall's
// ...).  A hit that asks for a talent opens its zone only for the caster who bought it: the Magma Field's base, Pyromania and Wildfire fields.
struct ZoneCase {
    int                      klass;
    const char*              ability;
    std::vector<std::string> zones;
    bool                     server_only = false;   // the zone runs but no actor is sent (the Chilling Lament's circle is the client's own effect)
};

void test_ground_circles_every_zone_opens_with_its_cast(Fixture& f) {
    (void)f;
    std::printf("every ability that leaves a lasting area opens the zone its data names, with its cast\n");
    const std::vector<ZoneCase> cases = {
        {2, "Bard_Dance_Maddening", {"Bard_Dance_Maddening"}},
        {2, "Bard_Dance_Mesmerizing", {"Bard_Dance_Mesmerizing"}},
        {2, "Bard_Dance_Nimble", {"Bard_Dance_Nimble"}},
        {2, "Bard_Dance_Shielding", {"Bard_Dance_Shielding"}},
        {2, "Bard_Song_Lament", {"Bard_Song_Lament_Linger"}, true},
        {3, "Cleric_ChainsOfRestraint", {"Cleric_ChainsOfRestraint"}},
        {3, "Cleric_DivineFlare", {"Cleric_DivineFlare_Zone"}},
        {8, "Mage_ArcaneCircle", {"Mage_ArcaneCircle_Linger"}},
        {8, "Mage_ArcaneEye", {"Mage_ArcaneEye"}},
        {8, "Mage_ConeOfCold", {"Mage_ConeOfCold_Lingering"}},
        {8, "Mage_Hoarfrost", {"Mage_Hoarfrost_Linger"}},
        {1, "Ranger_BearTrap", {"Ranger_BearTrap", "Ranger_BearTrap_Base"}},
        {1, "Ranger_VineField", {"Ranger_VineField_Zone"}},
        {5, "Rogue_Caltrops", {"Rogue_Caltrops"}},
        {5, "Rogue_SmokeBomb", {"Rogue_SmokeBomb"}},
        {6, "Summoner_Void", {"Summoner_Void"}},
        {7, "Tank_AbsorptionField", {"Tank_AbsorptionField"}},
        {7, "Tank_Aegis", {"Tank_Aegis"}},
        {7, "Tank_IntimidatingAura", {"Tank_IntimidatingAura_Linger"}},
        {7, "Tank_IronAura", {"Tank_IronAura"}},
        {7, "Tank_Wall", {"Tank_Wall"}},
    };
    for (const ZoneCase& c : cases) {
        Rig rig(c.klass, 25, {});
        CHECKF(rig.ok(), "the rig for %s: %s", c.ability, rig.problem.c_str());
        if (!rig.ok()) continue;
        rig.know({c.ability});
        const Refusal refusal = rig.trigger(c.ability, 1.0, false);
        rig.run_to(7.0);
        std::vector<std::string> opened = ground::opened_zones(rig);
        std::vector<std::string> expected = c.zones;
        std::sort(opened.begin(), opened.end());
        std::sort(expected.begin(), expected.end());
        std::string got;
        for (const std::string& z : opened) got += (got.empty() ? "" : ", ") + z;
        if (c.server_only) {
            CHECKF(refusal == Refusal::None && opened.empty(), "%s opens no actor, its zone runs on the server only (cast: %s; opened: %s)", c.ability, refusal_name(refusal),
                   got.empty() ? "none" : got.c_str());
            continue;
        }
        CHECKF(refusal == Refusal::None && opened == expected, "%s opens %s (cast: %s; opened: %s)", c.ability, c.zones.size() == 1 ? c.zones[0].c_str() : "its two zones", refusal_name(refusal),
               got.empty() ? "none" : got.c_str());
    }
}
AOC_REGISTER_TEST(test_ground_circles_every_zone_opens_with_its_cast);

void test_ground_circles_the_magma_field_opens_the_field_its_talent_picks(Fixture& f) {
    (void)f;
    std::printf("the Magma Field opens the field its talents pick: the base field, the Pyromania field or the Wildfire field, and no other\n");
    struct Variant {
        std::vector<std::string> talents;
        std::vector<std::string> zones;
        const char*              what;
    };
    const std::vector<Variant> variants = {
        {{}, {"Mage_MagmaField_LingeringField"}, "no talent"},
        {{"Mage_Passive_MagmaField_Pyromania"}, {"Mage_MagmaField_LingeringField_Pyromania"}, "Pyromania"},
        {{"Mage_Passive_MagmaField_Wildfire"}, {"Mage_MagmaField_LingeringField_Wildfire"}, "Wildfire"},
    };
    for (const Variant& v : variants) {
        Rig rig(8, 25, v.talents);
        CHECKF(rig.ok(), "the Mage with %s: %s", v.what, rig.problem.c_str());
        if (!rig.ok()) continue;
        rig.know({"Mage_MagmaField"});
        const Refusal refusal = rig.trigger("Mage_MagmaField", 1.0, false);
        rig.run_to(5.0);   // (the field opens with the bar's end: no request comes, so at its deadline, 4.3 s)
        const std::vector<std::string> opened = ground::opened_zones(rig);
        CHECKF(refusal == Refusal::None && opened == v.zones, "a Mage with %s opens %s only (cast: %s; %zu zone(s) opened, the first %s)", v.what, v.zones[0].c_str(), refusal_name(refusal),
               opened.size(), opened.empty() ? "none" : opened[0].c_str());
    }
}
AOC_REGISTER_TEST(test_ground_circles_the_magma_field_opens_the_field_its_talent_picks);

// ---- the traps -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// The Bear Trap's two zones: the base zone arms itself 2 s after it opens (DelayTime) and lands its 0.1 s tick on the first creature in it, and a zone whose record says ExpireAfterAppliedHits 1
// ends with that hit; the Trapslinger's zone lands its overlap hit at the open and ends with it.  Their rows (the delay and the limit) are the real ones.
void test_ground_circles_a_trap_arms_itself_and_goes_off_once(Fixture& f) {
    std::printf("the Bear Trap: its zones land one hit on the Wolf that stands in them and end, the base zone after its 2 s delay\n");
    const std::string wolf_path = wolf_archetype(f);
    CHECK(!wolf_path.empty(), "the Wolf is in the motion table");
    if (wolf_path.empty()) return;
    Forge g(f, 1, {});
    CHECKF(g.ok(), "the Ranger and his world: %s %s", g.rig.problem.c_str(), g.w.problem.c_str());
    if (!g.ok()) return;
    const abilities::AbilityDef* def = f.catalog->ability_named("Ranger_BearTrap");
    const abilities::HitDef* base_hit = f.catalog->hit_named("Ranger_BearTrap_TripHit");
    const abilities::HitDef* slinger_hit = f.catalog->hit_named("Ranger_BearTrap_Trapslinger_TripHit");
    CHECK(def && def->zones.size() == 2 && base_hit && slinger_hit, "the data has the Bear Trap's two zones and their hits");
    if (!def || def->zones.size() != 2 || !base_hit || !slinger_hit) return;
    int64_t base_zone = 0, slinger_zone = 0;
    for (const int64_t z : def->zones) {
        const content::ZoneRow* row = f.catalog->zone(z);
        if (row && row->name == "Ranger_BearTrap_Base") base_zone = z;
        if (row && row->name == "Ranger_BearTrap") slinger_zone = z;
    }
    const content::ZoneRow* base_row = f.catalog->zone(base_zone);
    CHECKF(base_row != nullptr && base_row->delay_time == 2.0 && base_row->expire_after_hits_expr != 0, "the base zone's record holds its 2 s delay and its applied-hit limit (%.1f s)", base_row ? base_row->delay_time : -1.0);
    if (!base_row || slinger_zone == 0) return;
    const auto given_base = ground::give_zone(*g.rig.catalog, base_zone, "EAbilitySystemOrigin::HitLocation", "EAbilitySystemAttachType::None", 150.0, true);
    const auto given_slinger = ground::give_zone(*g.rig.catalog, slinger_zone, "EAbilitySystemOrigin::HitLocation", "EAbilitySystemAttachType::None", 150.0, true);
    const uint64_t wolf = g.wolf(600, 0);
    CHECK(wolf != 0, "a Wolf 6 m from the Ranger");
    if (!wolf) return;
    g.rig.know({"Ranger_BearTrap"});
    const double t0 = 100.0;
    const ground::Cast cast = ground::cast_zone(g.rig, "Ranger_BearTrap", t0, combat::Vec3{600, 0, 1000});
    CHECKF(cast.cast, "the Bear Trap is cast at the Wolf's place");
    if (!cast.cast) return;
    ground::run(g.rig, g.world, t0, t0 + 6.0);
    const std::vector<ground::ZoneHit> base_hits = ground::zone_hits(g.rig, base_hit->row.guid);
    const std::vector<ground::ZoneHit> slinger_hits = ground::zone_hits(g.rig, slinger_hit->row.guid);
    CHECKF(ground::zone_hits_on(slinger_hits, wolf) == 1, "the Trapslinger's zone landed its hit once (%zu)", ground::zone_hits_on(slinger_hits, wolf));
    CHECKF(ground::zone_hits_on(base_hits, wolf) == 1, "the base zone landed its tick on the Wolf once, not every 0.1 s for its life (%zu)", ground::zone_hits_on(base_hits, wolf));
    double opened_at = -1;
    for (const Seen& s : g.rig.log)
        for (const bootstrap::ZoneOp& z : s.zones)
            if (z.kind == bootstrap::ZoneOp::Kind::Open && z.zone == base_zone) opened_at = s.at;
    CHECKF(opened_at >= 0 && !base_hits.empty() && base_hits.front().at >= opened_at + 2.0 - 1e-6, "...after the zone's 2 s delay (opened at %.2f s, hit at %.2f s)", opened_at, base_hits.empty() ? -1.0 : base_hits.front().at);
    CHECKF(ground::zone_ops_of(g.rig, "Ranger_BearTrap_Base", bootstrap::ZoneOp::Kind::Close) == 1 && ground::zone_ops_of(g.rig, "Ranger_BearTrap", bootstrap::ZoneOp::Kind::Close) == 1,
           "both zones are over by 6 s, not at the end of their 30 s");
}
AOC_REGISTER_TEST(test_ground_circles_a_trap_arms_itself_and_goes_off_once);

// ---- the effects a zone leaves on creatures ----------------------------------------------------------------------------------------------------------------------------------
// The Maddening Dance's overlap hit puts an infinite mark on the enemies in its circle (Bard_Dance_Maddening_Aura); nothing else would take it off.
void test_ground_circles_a_zone_takes_its_effects_off_when_a_creature_leaves_or_it_ends(Fixture& f) {
    std::printf("the infinite mark a dance's zone puts on an enemy goes when the enemy leaves the circle and when the dance ends\n");
    const std::string wolf_path = wolf_archetype(f);
    CHECK(!wolf_path.empty(), "the Wolf is in the motion table");
    if (wolf_path.empty()) return;
    BardMobWorld bw(f, wolf_path, 2);
    CHECKF(bw.problem.empty(), "the Bard and his world: %s", bw.problem.c_str());
    if (!bw.problem.empty()) return;
    const int64_t zone = ground::zone_of(f, "Bard_Dance_Maddening");
    const abilities::EffectDef* aura = f.catalog->effect_named("Bard_Dance_Maddening_Aura");
    CHECK(zone != 0 && aura && aura->row.infinite_duration, "the data has the Maddening Dance's zone and its infinite mark");
    if (zone == 0 || !aura) return;
    const auto given_zone = ground::give_zone(*bw.rig.catalog, zone, "EAbilitySystemOrigin::Instigator", "EAbilitySystemAttachType::AttachedInstigator", 500.0);
    const uint64_t near_wolf = bw.w.spawn(wolf_path, 300, 0, 10, 0);
    const uint64_t far_wolf = bw.w.spawn(wolf_path, 350, 0, 10, 0);
    CHECK(near_wolf != 0 && far_wolf != 0, "two Wolves 3 m from the Bard");
    if (!near_wolf || !far_wolf) return;
    bw.rig.know({"Bard_Dance_Maddening"});
    const double t0 = 100.0;
    CHECK(ground::cast_zone(bw.rig, "Bard_Dance_Maddening", t0).cast, "the Maddening Dance is cast");
    ground::run(bw.rig, *bw.world, t0, t0 + 2.0);
    CHECKF(bw.world->effect_stacks(near_wolf, aura->row.guid) == 1 && bw.world->effect_stacks(far_wolf, aura->row.guid) == 1, "both Wolves in the circle carry the mark (%d, %d)",
           bw.world->effect_stacks(near_wolf, aura->row.guid), bw.world->effect_stacks(far_wolf, aura->row.guid));
    bw.w.npcs->warp(far_wolf, 1500, 0, 1000.0);   // it leaves the circle (5 m) while the dance goes on
    ground::run(bw.rig, *bw.world, t0 + 2.0, t0 + 3.6);
    CHECKF(bw.world->effect_stacks(far_wolf, aura->row.guid) == 0 && bw.world->effect_stacks(near_wolf, aura->row.guid) == 1,
           "the Wolf that left carries no mark at the next tick, the one that stayed still does (%d, %d)", bw.world->effect_stacks(far_wolf, aura->row.guid),
           bw.world->effect_stacks(near_wolf, aura->row.guid));
    ground::run(bw.rig, *bw.world, t0 + 3.6, t0 + 33.0);
    size_t closed = 0;
    for (const Seen& s : bw.rig.log)
        for (const bootstrap::ZoneOp& z : s.zones) closed += z.kind == bootstrap::ZoneOp::Kind::Close && z.zone == zone ? 1 : 0;
    CHECKF(closed == 1, "the dance is over (its actor closed: %zu)", closed);
    CHECKF(bw.world->effect_stacks(near_wolf, aura->row.guid) == 0, "...and the mark is gone (%d)", bw.world->effect_stacks(near_wolf, aura->row.guid));
}
AOC_REGISTER_TEST(test_ground_circles_a_zone_takes_its_effects_off_when_a_creature_leaves_or_it_ends);

// ---- the sides -----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// A creature that is no one's enemy or friend (a chicken) is on no side of a zone's hits: the Divine Flare's heal for the caster's side does not reach it, and it reaches a friend (a huntable
// animal).  The world's answer of who stands in a zone names each one's side of the caster: a Wolf Hostile, a huntable animal Friendly, and a second player who is not grouped with him Neutral
// (two players' alignments name no relation: a party's group alignment would make them friends, and the hits on other players are not served yet).
void test_ground_circles_the_sides_of_a_zone(Fixture& f) {
    std::printf("who a zone's hits reach: the enemy and the friend, not a creature that is neither; a second player who is not grouped with the caster is neither\n");
    const std::string wolf_path = wolf_archetype(f);
    const std::string chicken_path = ground::archetype_on_side(f, combat::Relation::Neutral, "/Chicken_Base.Default__Chicken_Base_C");
    const std::string friend_path = ground::archetype_on_side(f, combat::Relation::Friendly, "/Hunting_Bork.Default__Hunting_Bork_C");
    CHECK(!wolf_path.empty() && !chicken_path.empty() && !friend_path.empty(), "the Wolf, a chicken (no one's enemy or friend) and a huntable animal (a friend) are in the motion table");
    if (wolf_path.empty() || chicken_path.empty() || friend_path.empty()) return;
    Forge g(f, 3, {});
    CHECKF(g.ok(), "the Cleric and his world: %s %s", g.rig.problem.c_str(), g.w.problem.c_str());
    if (!g.ok()) return;
    Rig second(3, 25, {});
    CHECKF(second.ok(), "the second Cleric: %s", second.problem.c_str());
    if (!second.ok()) return;
    combat_rig::FakePlayer other(second, 78, {200, 0, 1098});
    g.world.add_player(&other);
    const uint64_t wolf = g.wolf(400, 0);
    const uint64_t chicken = g.w.spawn(chicken_path, 400, 100, 25, 0);
    const uint64_t friend_animal = g.w.spawn(friend_path, 400, -100, 25, 0);
    CHECK(wolf != 0 && chicken != 0 && friend_animal != 0, "a Wolf, a chicken and a huntable animal 4 m from the Cleric");
    if (!wolf || !chicken || !friend_animal) return;
    // the world's answer
    combat::ZoneArea area;
    area.radius = 500;
    const std::vector<combat::ZoneTarget> in = g.world.actors_in_area(g.player.fighter(), combat::Vec3{0, 0, 1098}, area, 10);
    const auto side_of = [&](uint64_t actor, int fallback) {
        for (const combat::ZoneTarget& t : in) {
            if (t.actor == actor) return static_cast<int>(t.relation);
        }
        return fallback;
    };
    const int wolf_side = side_of(wolf, -1), chicken_side = side_of(chicken, -1), friend_side = side_of(friend_animal, -1), player_side = side_of(other.actor_id(), -1);
    CHECKF(in.size() == 4 && wolf_side == static_cast<int>(combat::Relation::Hostile) && chicken_side == static_cast<int>(combat::Relation::Neutral) &&
               friend_side == static_cast<int>(combat::Relation::Friendly) && player_side == static_cast<int>(combat::Relation::Neutral),
           "the world names the Wolf Hostile (%d), the chicken Neutral (%d), the huntable animal Friendly (%d) and the second player, not grouped with the caster, Neutral (%d) (%zu found)", wolf_side, chicken_side,
           friend_side, player_side, in.size());
    // the Divine Flare's zone: a heal and a shield for the caster's side
    const int64_t zone = ground::zone_of_hit(f, "Cleric_DivineFlare_SpawnLinger");
    const abilities::HitDef* heal = f.catalog->hit_named("Cleric_DivineFlare_EndHit");
    CHECK(zone != 0 && heal, "the data has the Divine Flare's zone and its heal");
    if (zone == 0 || !heal) return;
    const auto given_zone = ground::give_zone(*g.rig.catalog, zone, "EAbilitySystemOrigin::HitLocation", "EAbilitySystemAttachType::None", 500.0);
    g.rig.know({"Cleric_DivineFlare"});
    const double t0 = 100.0;
    const ground::Cast cast = ground::cast_zone(g.rig, "Cleric_DivineFlare", t0, combat::Vec3{400, 0, 1000});
    CHECKF(cast.cast, "the Divine Flare is cast at the place");
    if (!cast.cast) return;
    ground::run(g.rig, g.world, t0, t0 + 6.0);
    const std::vector<ground::ZoneHit> heals = ground::zone_hits(g.rig, heal->row.guid);
    CHECKF(ground::zone_hits_on(heals, friend_animal) >= 1, "the zone healed the huntable animal, a friend (%zu)", ground::zone_hits_on(heals, friend_animal));
    CHECKF(ground::zone_hits_on(heals, chicken) == 0, "...and nothing reached the chicken (%zu)", ground::zone_hits_on(heals, chicken));
    CHECKF(ground::zone_hits_on(heals, wolf) == 0, "...or the Wolf, an enemy (%zu)", ground::zone_hits_on(heals, wolf));
    CHECK(g.rig.pawn.broken.empty(), "(the pawn took every change)");
}
AOC_REGISTER_TEST(test_ground_circles_the_sides_of_a_zone);

// ---- the limit -----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// At most 16 zones are alive at once.  The one that goes for the 17th closes its actor: before this the actor stayed in the world, open, with no zone behind it.
void test_ground_circles_the_zone_that_goes_for_the_limit_closes_its_actor(Fixture& f) {
    (void)f;
    std::printf("more zones alive than the limit: the oldest goes with its actor closed\n");
    Rig rig(1, 25, {});
    CHECKF(rig.ok(), "the Ranger: %s", rig.problem.c_str());
    if (!rig.ok()) return;
    rig.know({"Ranger_BearTrap"});
    // the Bear Trap leaves two zones of 30 s each; nine casts three seconds apart are 18 zones in 24 s
    size_t cast_ok = 0;
    for (int i = 0; i < 9; ++i) {
        const double t = 100.0 + 3.0 * i;
        rig.pipe->clear_cooldowns(t);
        rig.set_current(content::kGuidMana, rig.pawn.stat(content::kGuidMaxMana));
        rig.set_current(content::kGuidHealth, rig.pawn.stat(content::kGuidMaxHealth));
        if (ground::cast_zone(rig, "Ranger_BearTrap", t, combat::Vec3{600, 0, 1000}).cast) ++cast_ok;
        rig.run_to(t + 2.9);
    }
    CHECKF(cast_ok == 9, "nine Bear Traps are cast (%zu)", cast_ok);
    const size_t opened = ground::opened_zones(rig).size();
    const size_t closed = forged_zone_ops(rig, bootstrap::ZoneOp::Kind::Close);
    CHECKF(opened == 18 && closed >= 2, "18 zones opened and the two oldest went with their actors closed, none of them at the end of its life yet (%zu opened, %zu closed)", opened, closed);
}
AOC_REGISTER_TEST(test_ground_circles_the_zone_that_goes_for_the_limit_closes_its_actor);

// ---- the ticks ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// A zone of 10 s at 0.2 s lands 49 ticks and its End: the moments are counted from the open, not summed in rounded steps (a sum of 0.2 lands 48 or 50 ticks at some moments).  The Chains' damage hit
// is in the Initial, Lingering and End lists: one at the open, 49 ticks and one at the end (the tick that falls on the end is the End's: the Chains' hits are nine in 8 s).
void test_ground_circles_a_zone_lands_a_tick_for_every_period_of_its_life(Fixture& f) {
    std::printf("a zone of 10 s at 0.2 s lands 49 ticks and its End, whatever moment it opened at\n");
    const std::string wolf_path = wolf_archetype(f);
    CHECK(!wolf_path.empty(), "the Wolf is in the motion table");
    if (wolf_path.empty()) return;
    const int64_t zone = ground::zone_of(f, "Cleric_ChainsOfRestraint");
    const abilities::HitDef* damage = f.catalog->hit_named("Cleric_ChainsOfRestraint");
    CHECK(zone != 0 && damage, "the data has the Chains' zone and its damage hit");
    if (zone == 0 || !damage) return;
    const double starts[] = {100.0, 100.07, 100.13, 100.31, 100.53, 100.77, 101.19};
    size_t wrong = 0, runs = 0;
    std::string seen;
    for (const double t0 : starts) {
        Forge g(f, 3, {});
        if (!g.ok()) continue;
        auto* row = const_cast<content::ZoneRow*>(g.rig.catalog->zone(zone));
        if (!row) continue;
        const auto given_zone = ground::give_zone(*g.rig.catalog, zone, "EAbilitySystemOrigin::HitLocation", "EAbilitySystemAttachType::None", 500.0);
        row->tick_rate = 0.2;
        row->life_time = 10.0;
        const uint64_t wolf = g.wolf(600, 0);
        if (!wolf) continue;
        g.rig.know({"Cleric_ChainsOfRestraint"});
        if (!ground::cast_zone(g.rig, "Cleric_ChainsOfRestraint", t0, combat::Vec3{600, 0, 1000}).cast) continue;
        ground::run(g.rig, g.world, t0, t0 + 14.0);
        const size_t hits = ground::zone_hits_on(ground::zone_hits(g.rig, damage->row.guid), wolf);
        ++runs;
        if (hits != 51) {
            ++wrong;
            seen += (seen.empty() ? "" : ", ") + std::to_string(hits);
        }
    }
    CHECKF(runs == 7 && wrong == 0, "51 hits of the zone at every one of 7 starts (the Initial hit, 49 ticks, the End hit): %zu of %zu starts were wrong%s%s", wrong, runs, seen.empty() ? "" : " with ", seen.c_str());
}
AOC_REGISTER_TEST(test_ground_circles_a_zone_lands_a_tick_for_every_period_of_its_life);
