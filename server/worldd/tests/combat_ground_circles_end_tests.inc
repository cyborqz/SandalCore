// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_ground_circles_end_tests.inc: how a zone's last moment goes, and the rows these checks run on (included by combat_registered.inc after
//  combat_ground_circles_review_tests.inc, inside test_worldd_combat's anonymous namespace)
//
//    the last tick      a tick that falls on the moment a zone's life is over is that moment's End: the Chains' hits are the Initial hit and eight more in their 8 s, and
//                       a zone that skips its first tick lands nothing when that first tick is its end (the Hoarfrost's blast is the cast's own hit, once)
//    the hits shown     a zone hit that has an effect to play and no number is still confirmed (the Absorption Field's End hit)
//    the rows           the spawn and volume values the other checks give a zone by hand are the extraction's own rows
//  These checks run on a game-data file that carries the zones' spawn (contentc 44) and say so when it does not.
// ============================================================================

namespace ground {

/// Whether the game data holds the zone's spawn and volume (a contentc 44 file): the checks of this file read the real rows.
bool has_real_spawn(Fixture& f, int64_t zone) {
    const content::ZoneRow* row = zone != 0 ? f.catalog->zone(zone) : nullptr;
    return row != nullptr && row->has_spawn && row->volume.has_volume();
}

}  // namespace ground

void test_ground_circles_the_chains_last_tick_is_their_end(Fixture& f) {
    std::printf("the Chains of Restraint: a Wolf in them takes the Initial hit and eight more in the zone's 8 s, the last at its end, not two at once\n");
    Forge g(f, 3, {});
    CHECKF(g.ok(), "the Cleric and his world: %s %s", g.rig.problem.c_str(), g.w.problem.c_str());
    if (!g.ok()) return;
    const int64_t zone = ground::zone_of(f, "Cleric_ChainsOfRestraint");
    const abilities::HitDef* damage = f.catalog->hit_named("Cleric_ChainsOfRestraint");
    CHECK(zone != 0 && damage != nullptr, "the data has the Chains' zone and its damage hit");
    if (zone == 0 || !damage) return;
    if (!ground::has_real_spawn(f, zone)) {
        std::printf("  Not tested: the game data does not carry the zones' spawn (a contentc 43 file)\n");
        return;
    }
    const content::ZoneRow* row = f.catalog->zone(zone);
    CHECKF(row != nullptr && row->life_time == 8.0 && row->tick_rate == 1.0, "the zone's record: 8 s of life, a tick every 1 s (%.1f s, %.1f s)", row ? row->life_time : -1.0, row ? row->tick_rate : -1.0);
    const uint64_t wolf = g.wolf(500, 0);
    CHECK(wolf != 0, "a Wolf 5 m from the Cleric");
    if (!wolf) return;
    g.rig.know({"Cleric_ChainsOfRestraint"});
    const double t0 = 100.0;
    const ground::Cast cast = ground::cast_zone(g.rig, "Cleric_ChainsOfRestraint", t0, combat::Vec3{500, 0, 1000});
    CHECKF(cast.cast, "the Chains are cast at the Wolf's place");
    if (!cast.cast) return;
    ground::run(g.rig, g.world, t0, t0 + 12.0);
    const std::vector<ground::ZoneHit> hits = ground::zone_hits(g.rig, damage->row.guid);
    const size_t on_wolf = ground::zone_hits_on(hits, wolf);
    CHECKF(on_wolf == 9, "the Wolf took 9 hits of the zone, the Initial hit and eight more (%zu)", on_wolf);
    bool apart = hits.size() >= 2;
    for (size_t i = 1; i < hits.size(); ++i) apart = apart && hits[i].at - hits[i - 1].at > 0.5;
    CHECK(apart, "no two of them at the same moment");
    CHECKF(!hits.empty() && std::fabs(hits.back().at - hits.front().at - 8.0) < 0.2, "the last came 8 s after the first, when the zone's life was over (%.2f s)", hits.empty() ? -1.0 : hits.back().at - hits.front().at);
    CHECK(g.rig.pawn.broken.empty(), "(the pawn took every change)");
}
AOC_REGISTER_TEST(test_ground_circles_the_chains_last_tick_is_their_end);

void test_ground_circles_the_hoarfrost_blasts_once(Fixture& f) {
    std::printf("the Hoarfrost: the Mage's blast hits the Wolf once, with the cast; the zone lives its second and goes, and lands no second blast at its end\n");
    Forge g(f, 8, {});
    CHECKF(g.ok(), "the Mage and his world: %s %s", g.rig.problem.c_str(), g.w.problem.c_str());
    if (!g.ok()) return;
    const int64_t zone = ground::zone_of(f, "Mage_Hoarfrost");
    const abilities::AbilityDef* def = f.catalog->ability_named("Mage_Hoarfrost");
    const abilities::HitDef* blast = f.catalog->hit_named("Mage_Hoarfrost");
    CHECK(zone != 0 && def != nullptr && blast != nullptr, "the data has the Hoarfrost, its zone and its hit");
    if (zone == 0 || !def || !blast) return;
    if (!ground::has_real_spawn(f, zone)) {
        std::printf("  Not tested: the game data does not carry the zones' spawn (a contentc 43 file)\n");
        return;
    }
    const content::ZoneRow* row = f.catalog->zone(zone);
    bool end_is_the_blast = false;
    if (row) {
        const auto end_hits = row->hits.find("EndHitsIds");
        end_is_the_blast = end_hits != row->hits.end() && !end_hits->second.empty() && end_hits->second.front() == blast->row.guid;
    }
    CHECKF(row != nullptr && row->life_time == 1.0 && row->tick_rate == 1.0 && row->spawn.no_first_tick && end_is_the_blast,
           "the zone's record: 1 s of life, a tick every 1 s, no hit on its first tick, and its End hit is the blast's own hit");
    const uint64_t wolf = g.wolf(300, 0);
    CHECK(wolf != 0, "a Wolf 3 m from the Mage");
    if (!wolf) return;
    g.rig.know({"Mage_Hoarfrost"});
    const double t0 = 100.0;
    TriggerInfo info;   // cast at a place on the ground; the client's own request for the hit follows
    info.target_count = 0;
    info.place = AimPlace{300, 0, 1000};
    const Refusal refusal = g.rig.pipe->trigger(def->row.guid, g.rig.next_client_id++, t0, info);
    CHECKF(refusal == Refusal::None, "the Hoarfrost is cast (%s)", refusal_name(refusal));
    if (refusal != Refusal::None) return;
    int64_t targeting = content::kTargetingDefaultSelf;
    for (const content::AbilityHitRow& h : def->hits) {
        if (h.hit_guid != blast->row.guid || h.source != "hit") continue;
        for (const content::TargetingRow& tg : def->targetings) {
            if (tg.key == h.key) targeting = tg.guid;
        }
    }
    const auto verdict = g.rig.pipe->hit_request(def->row.guid, targeting, blast->row.guid, 1, 0, t0 + 0.35, {wolf});
    CHECKF(verdict == AbilityPipeline::HitVerdict::Scheduled, "the client's request for the blast is taken (%s)", AbilityPipeline::verdict_name(verdict));
    ground::run(g.rig, g.world, t0, t0 + 0.9);
    const double lost_by_the_cast = g.world.max_health_of(wolf) - g.world.health_of(wolf);
    ground::run(g.rig, g.world, t0 + 0.9, t0 + 4.0);
    size_t from_the_cast = 0, from_the_zone = 0;
    for (const Seen& s : g.rig.log) {
        for (const bootstrap::HitConfirm& c : s.confirmations) {
            if (c.record_guid != blast->row.guid || c.targets.empty()) continue;
            (c.source_type == content::kSourceTypeZone ? from_the_zone : from_the_cast) += 1;
        }
    }
    CHECKF(from_the_cast == 1 && from_the_zone == 0, "the blast was confirmed once, as the cast's hit, and not again as the zone's (%zu from the cast, %zu from the zone)", from_the_cast, from_the_zone);
    CHECKF(lost_by_the_cast > 0 && std::fabs((g.world.max_health_of(wolf) - g.world.health_of(wolf)) - lost_by_the_cast) < 1e-6,
           "the Wolf lost its Health once: nothing more came off it after the blast (%.1f, then %.1f)", lost_by_the_cast, g.world.max_health_of(wolf) - g.world.health_of(wolf));
    CHECKF(ground::zone_ops_of(g.rig, "Mage_Hoarfrost_Linger", bootstrap::ZoneOp::Kind::Open) == 1 && ground::zone_ops_of(g.rig, "Mage_Hoarfrost_Linger", bootstrap::ZoneOp::Kind::Close) == 1,
           "the zone itself opened and closed: its second of life is still drawn");
    CHECK(g.rig.pawn.broken.empty(), "(the pawn took every change)");
}
AOC_REGISTER_TEST(test_ground_circles_the_hoarfrost_blasts_once);

void test_ground_circles_a_hit_with_an_effect_to_play_is_still_confirmed(Fixture& f) {
    std::printf("a zone's hit with an effect to play and no number is confirmed to the client: the Absorption Field's End hit, on the Wolf that stands in it when it ends\n");
    Forge g(f, 7, {});   // a Tank
    CHECKF(g.ok(), "the Tank and his world: %s %s", g.rig.problem.c_str(), g.w.problem.c_str());
    if (!g.ok()) return;
    const int64_t zone = ground::zone_of(f, "Tank_AbsorptionField");
    const abilities::HitDef* end_hit = f.catalog->hit_named("Tank_AbsorptionField_AOE_End");
    CHECK(zone != 0 && end_hit != nullptr, "the data has the Absorption Field's zone and its End hit");
    if (zone == 0 || !end_hit) return;
    CHECKF(end_hit->row.has_fx && end_hit->mods.empty() && end_hit->row.applicable_targets == 2, "the End hit has an effect to play, no stat mod, and reaches enemies (%d, %zu mods, mask %d)", end_hit->row.has_fx,
           end_hit->mods.size(), end_hit->row.applicable_targets);
    if (!ground::has_real_spawn(f, zone)) {
        std::printf("  Not tested: the game data does not carry the zones' spawn (a contentc 43 file)\n");
        return;
    }
    const uint64_t wolf = g.wolf(500, 0);
    CHECK(wolf != 0, "a Wolf 5 m from the Tank");
    if (!wolf) return;
    g.rig.know({"Tank_AbsorptionField"});
    const double t0 = 100.0;
    const ground::Cast cast = ground::cast_zone(g.rig, "Tank_AbsorptionField", t0, combat::Vec3{500, 0, 1000});
    CHECKF(cast.cast, "the Absorption Field is cast at the Wolf's place");
    if (!cast.cast) return;
    ground::run(g.rig, g.world, t0, t0 + 12.0);
    const std::vector<ground::ZoneHit> hits = ground::zone_hits(g.rig, end_hit->row.guid);
    CHECKF(ground::zone_hits_on(hits, wolf) == 1, "the End hit is confirmed once, naming the Wolf (%zu)", ground::zone_hits_on(hits, wolf));
    CHECKF(!hits.empty() && hits.front().at - t0 > 8.5, "...when the zone's 9 s are over (%.1f s after the cast)", hits.empty() ? -1.0 : hits.front().at - t0);
    CHECK(g.rig.pawn.broken.empty(), "(the pawn took every change)");
}
AOC_REGISTER_TEST(test_ground_circles_a_hit_with_an_effect_to_play_is_still_confirmed);

// ---- the rows the other checks give by hand -------------------------------------------------------------------------------------------------------------------------------------
// Most checks of the ground circles give a zone its spawn and volume with `give_zone` (they were written before the data step).  The values they give are the extraction's own rows: a
// check that ran on values the data does not hold would pass for nothing.
void test_ground_circles_the_rows_the_checks_give_are_the_data_rows(Fixture& f) {
    std::printf("the spawn and volume the other checks give a zone by hand are the rows the extraction holds\n");
    struct Given {
        const char* what;
        int64_t     zone;
        const char* origin;
        const char* attach;
        double      radius;
        bool        no_first_tick;
    };
    const auto base_field = [&]() -> int64_t {
        const abilities::HitDef* spawn = f.catalog->hit_named("Mage_MagmaField_SpawnField");
        if (spawn) {
            for (const int64_t guid : spawn->zones) {
                const content::ZoneRow* row = f.catalog->zone(guid);
                if (row && row->name == "Mage_MagmaField_LingeringField") return guid;
            }
        }
        return 0;
    };
    int64_t bear_base = 0, bear_slinger = 0;
    if (const abilities::AbilityDef* trap = f.catalog->ability_named("Ranger_BearTrap")) {
        for (const int64_t guid : trap->zones) {
            const content::ZoneRow* row = f.catalog->zone(guid);
            if (row && row->name == "Ranger_BearTrap_Base") bear_base = guid;
            if (row && row->name == "Ranger_BearTrap") bear_slinger = guid;
        }
    }
    const std::vector<Given> given = {
        {"the Chains", ground::zone_of(f, "Cleric_ChainsOfRestraint"), "EAbilitySystemOrigin::HitLocation", "EAbilitySystemAttachType::None", 500.0, false},
        {"the Vine Field", ground::zone_of(f, "Ranger_VineField"), "EAbilitySystemOrigin::HitLocation", "EAbilitySystemAttachType::None", 650.0, false},
        {"the Lament", ground::zone_of(f, "Bard_Song_Lament"), "EAbilitySystemOrigin::HitLocation", "EAbilitySystemAttachType::None", 300.0, true},
        {"the Maddening Dance", ground::zone_of(f, "Bard_Dance_Maddening"), "EAbilitySystemOrigin::Instigator", "EAbilitySystemAttachType::AttachedInstigator", 500.0, false},
        {"the Bear Trap's base zone", bear_base, "EAbilitySystemOrigin::HitLocation", "EAbilitySystemAttachType::None", 150.0, true},
        {"the Trapslinger's Bear Trap zone", bear_slinger, "EAbilitySystemOrigin::HitLocation", "EAbilitySystemAttachType::None", 150.0, true},
        {"the Magma Field's base zone", base_field(), "EAbilitySystemOrigin::HitLocation", "EAbilitySystemAttachType::None", 500.0, false},
    };
    for (const Given& g : given) {
        CHECKF(g.zone != 0, "the data has %s", g.what);
    }
    for (const Given& g : given) {
        if (g.zone == 0) continue;
        if (!ground::has_real_spawn(f, g.zone)) {
            std::printf("  Not tested: the game data does not carry the zones' spawn (a contentc 43 file)\n");
            return;
        }
        const content::ZoneRow* row = f.catalog->zone(g.zone);
        CHECKF(row->spawn.origin == g.origin && row->spawn.attach == g.attach && row->volume.radius == g.radius && row->volume.boxes.empty() && row->spawn.no_first_tick == g.no_first_tick,
               "%s: the row holds what the checks give (%s, %s, a sphere of %.0f cm, first tick skipped %d)", g.what, row->spawn.origin.c_str(), row->spawn.attach.c_str(), row->volume.radius, row->spawn.no_first_tick);
    }
}
AOC_REGISTER_TEST(test_ground_circles_the_rows_the_checks_give_are_the_data_rows);
