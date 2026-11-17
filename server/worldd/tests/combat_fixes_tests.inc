// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_fixes_tests.inc: the six things the game test of the ground circles found (included by combat_registered.inc inside
//  test_worldd_combat's anonymous namespace, after the other combat test files)
//
//    the Chilling Lament    its zone stands under the Bard and its circle is not sent to the client (combat_ground_circles_tests.inc)
//    the Magma Field        the cast ends when the server's cast ends, also when the client's cast bar never asked for its branch
//    the Arcane Circle      the buff the circle puts on the Mage ends with the circle
//    the Prismatic Beam     the tick the client asks for lands the beam's damage on the enemy, not only its aggro
//    the Consecrating Wave  the wave's ally hit heals its caster
//    the Chains             with Essence Infusion on and too little Divine Power the cast pays its normal Mana cost
// ============================================================================

#undef CHECK
#undef CHECKF
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); std::fflush(stdout); ++g_pass; } \
    else { std::printf("  [FAIL] %s\n", msg); std::fflush(stdout); ++g_fail; } \
} while (0)
#define CHECKF(cond, ...) do { \
    char fixes_line[1024]; \
    std::snprintf(fixes_line, sizeof fixes_line, __VA_ARGS__); \
    if (cond) { std::printf("  [ok ] %s\n", fixes_line); std::fflush(stdout); ++g_pass; } \
    else { std::printf("  [FAIL] %s\n", fixes_line); std::fflush(stdout); ++g_fail; } \
} while (0)

namespace fixes {

/// The targeting record of the ability's hit, by its key.
int64_t targeting_of(const abilities::AbilityDef& def, int64_t hit) {
    for (const content::AbilityHitRow& h : def.hits) {
        if (h.hit_guid != hit || h.source != "hit") continue;
        for (const content::TargetingRow& t : def.targetings) {
            if (t.key == static_cast<int>(h.key)) return t.guid;
        }
    }
    return content::kTargetingDefaultSelf;
}

/// How many effects of `guid` the caster holds (the client's CurrentEffects items).
size_t caster_effects(const Rig& rig, int64_t guid) {
    size_t n = 0;
    for (const auto& [key, e] : rig.pawn.effects) n += e.effect_guid == guid ? 1 : 0;
    return n;
}

/// The moments the server ended a client cast (ClientEndAbility), from the rig's log.
std::vector<double> end_times(const Rig& rig, int32_t client_id) {
    std::vector<double> out;
    for (const Seen& s : rig.log) {
        if (s.end && *s.end == client_id) out.push_back(s.at);
    }
    return out;
}

}  // namespace fixes

void test_combat_fixes_the_magma_field_cast_ends(Fixture& f) {
    std::printf("the Magma Field: the server ends the client's cast when its own cast ends, so the animation does not play on after the field\n");
    Forge m(f, 8, {});   // a Mage with no talent: the base field opens
    CHECKF(m.ok(), "the Mage and his world: %s %s", m.rig.problem.c_str(), m.w.problem.c_str());
    if (!m.ok()) return;
    m.rig.know({"Mage_MagmaField"});
    const double t0 = 100.0;
    const int32_t id = m.rig.next_client_id;
    TriggerInfo info;
    info.place = AimPlace{300, 0, 1000};   // the ground under the Wolf, as the client casts a field
    const Refusal refusal = m.rig.pipe->trigger(m.rig.ability("Mage_MagmaField"), m.rig.next_client_id++, t0, info);
    CHECKF(refusal == Refusal::None, "the Magma Field is cast (%s)", refusal_name(refusal));
    if (refusal != Refusal::None) return;
    m.rig.run_to(t0 + 2.0);
    CHECKF(fixes::end_times(m.rig, id).empty(), "the cast is still running at 2 s: the server has not ended it yet (%zu end(s))", fixes::end_times(m.rig, id).size());
    m.rig.run_to(t0 + 6.0);
    const std::vector<double> ends = fixes::end_times(m.rig, id);
    CHECKF(ends.size() == 1 && ends.front() >= t0 + 3.0 && ends.front() <= t0 + 5.0,
           "the server ends the client's Magma Field cast at its own end, before 5 s (%zu end(s), the first at %.2f s)", ends.size(),
           ends.empty() ? -1.0 : ends.front() - t0);
    const bootstrap::ActiveAbilityItem* item = m.rig.pawn.ability_item();
    CHECKF(item == nullptr || item->ability_guid != m.rig.ability("Mage_MagmaField") || item->montage_guid == 0,
           "the ability item's montage is cleared (the cast is over)");
    CHECK(m.rig.pawn.broken.empty(), "(the pawn took every change)");
}
AOC_REGISTER_TEST(test_combat_fixes_the_magma_field_cast_ends);

void test_combat_fixes_the_arcane_circle_buff_ends(Fixture& f) {
    std::printf("the Arcane Circle: the buff its circle puts on the Mage ends with the circle, not never\n");
    Forge m(f, 8, {});   // a Mage
    CHECKF(m.ok(), "the Mage and his world: %s %s", m.rig.problem.c_str(), m.w.problem.c_str());
    if (!m.ok()) return;
    const abilities::EffectDef* buff = f.catalog->effect_named("Mage_ArcaneCircle");
    CHECK(buff != nullptr, "the data has the Arcane Circle's buff");
    if (!buff) return;
    m.rig.know({"Mage_ArcaneCircle"});
    const double t0 = 100.0;
    const Refusal refusal = m.rig.trigger("Mage_ArcaneCircle", t0, false);
    CHECKF(refusal == Refusal::None, "the Arcane Circle is cast (%s)", refusal_name(refusal));
    if (refusal != Refusal::None) return;
    m.rig.run_to(t0 + 0.5);
    CHECKF(fixes::caster_effects(m.rig, buff->row.guid) >= 1, "the circle put its buff on the Mage (%zu instance(s))", fixes::caster_effects(m.rig, buff->row.guid));
    m.rig.run_to(t0 + 11.0);   // the zone's life is 10 s
    CHECKF(fixes::caster_effects(m.rig, buff->row.guid) == 0, "the buff is gone once the circle has ended (%zu instance(s) at 11 s)", fixes::caster_effects(m.rig, buff->row.guid));
    // a second circle applied to a Mage who still holds nothing from the first: its buff ends with it too
    const Refusal again = m.rig.trigger("Mage_ArcaneCircle", t0 + 70.0, false);   // after its 60 s cooldown
    CHECKF(again == Refusal::None, "a second Arcane Circle is cast (%s)", refusal_name(again));
    if (again != Refusal::None) return;
    m.rig.run_to(t0 + 81.0);
    CHECKF(fixes::caster_effects(m.rig, buff->row.guid) == 0, "the second circle's buff is gone too (%zu instance(s))", fixes::caster_effects(m.rig, buff->row.guid));
    CHECK(m.rig.pawn.broken.empty(), "(the pawn took every change)");
}
AOC_REGISTER_TEST(test_combat_fixes_the_arcane_circle_buff_ends);

void test_combat_fixes_the_prismatic_beam_hurts(Fixture& f) {
    std::printf("the Prismatic Beam: the Recurse tick the client asks for lands the beam's damage on the Wolf\n");
    Forge m(f, 8, {});   // a Mage
    CHECKF(m.ok(), "the Mage and his world: %s %s", m.rig.problem.c_str(), m.w.problem.c_str());
    if (!m.ok()) return;
    const uint64_t wolf = m.wolf(300, 0);
    CHECK(wolf != 0, "a Wolf 3 m from the Mage");
    if (!wolf) return;
    const abilities::AbilityDef* def = f.catalog->ability_named("Mage_PrismaticBeam");
    const abilities::HitDef* recurse = f.catalog->hit_named("Mage_PrismaticBeam_Recurse");
    const abilities::HitDef* damage = f.catalog->hit_named("Mage_PrismaticBeam_Dmg");
    CHECK(def && recurse && damage, "the data has the Beam's Recurse tick and its damage hit");
    if (!def || !recurse || !damage) return;
    m.rig.know({"Mage_PrismaticBeam"});
    const double t0 = 100.0;
    const Refusal refusal = m.rig.trigger("Mage_PrismaticBeam", t0, false);
    CHECKF(refusal == Refusal::None, "the Beam is cast (%s)", refusal_name(refusal));
    if (refusal != Refusal::None) return;
    m.rig.run_to(t0 + 0.2);   // its cost step is 83 ms
    const auto verdict = m.rig.pipe->hit_request(def->row.guid, fixes::targeting_of(*def, recurse->row.guid), recurse->row.guid, 1, 0, t0 + 0.3, {wolf});
    CHECKF(verdict == AbilityPipeline::HitVerdict::Scheduled, "the client's Recurse tick is taken (%d)", static_cast<int>(verdict));
    m.rig.run_to(t0 + 0.6);
    // the Wolf spawned at full Health; after the tick its Health is read for the first time, so the max is read with it
    const double hp_after = m.world.health_of(wolf);
    const double max_hp = m.world.max_health_of(wolf);
    CHECKF(hp_after > 0.0 && hp_after < max_hp - 0.01, "the Recurse tick hurt the Wolf (%.1f of %.1f Health after the hit)", hp_after, max_hp);
    const std::vector<const bootstrap::HitConfirm*> hits = forged_confirmations(m.rig, damage->row.guid);
    CHECKF(!hits.empty(), "the Beam's damage hit is confirmed on it, with its number (%zu confirmation(s))", hits.size());
    CHECK(m.rig.pawn.broken.empty(), "(the pawn took every change)");
}
AOC_REGISTER_TEST(test_combat_fixes_the_prismatic_beam_hurts);

void test_combat_fixes_the_consecrating_wave_heals(Fixture& f) {
    std::printf("the Consecrating Wave: its ally hit heals the caster, the Restorative talent held\n");
    Forge g(f, 3, {"Cleric_Passive_ConsecratingWave_Restorative"});   // a Cleric with the Restorative talent
    CHECKF(g.ok(), "the Cleric and his world: %s %s", g.rig.problem.c_str(), g.w.problem.c_str());
    if (!g.ok()) return;
    const abilities::AbilityDef* def = f.catalog->ability_named("Cleric_ConsecratingWave");
    const abilities::HitDef* ally = f.catalog->hit_named("Cleric_ConsecratingWave_AllyHit");
    const abilities::EffectDef* restorative = f.catalog->effect_named("Cleric_Passive_ConsecratingWave_Restorative");
    CHECK(def && ally && restorative, "the data has the wave's ally hit and the Restorative talent");
    if (!def || !ally || !restorative) return;
    g.rig.know({"Cleric_ConsecratingWave"});
    g.rig.run_to(1.0);
    g.rig.set_current(content::kGuidHealth, g.rig.max_health() - 500.0);   // a hurt Cleric, so the heal has room
    const double health_before = g.rig.health();
    const double t0 = 100.0;
    const Refusal refusal = g.rig.trigger("Cleric_ConsecratingWave", t0, false);
    CHECKF(refusal == Refusal::None, "the wave is cast (%s)", refusal_name(refusal));
    if (refusal != Refusal::None) return;
    g.rig.run_to(t0 + 0.2);
    const auto verdict = g.rig.pipe->hit_request(def->row.guid, fixes::targeting_of(*def, ally->row.guid), ally->row.guid, 1, 1, t0 + 0.3);
    CHECKF(verdict == AbilityPipeline::HitVerdict::Scheduled, "the client's ally hit on the caster is taken (%d)", static_cast<int>(verdict));
    g.rig.run_to(t0 + 0.6);
    const double health_after = g.rig.health();
    CHECKF(health_after > health_before + 0.01, "the wave healed the Cleric (%.1f -> %.1f of %.1f Health)", health_before, health_after, g.rig.max_health());
    CHECK(g.rig.pawn.broken.empty(), "(the pawn took every change)");
}
AOC_REGISTER_TEST(test_combat_fixes_the_consecrating_wave_heals);

void test_combat_fixes_the_chains_use_the_normal_cost(Fixture& f) {
    std::printf("the Chains of Restraint: while Essence Infusion is on, too little Divine Power pays the normal Mana cost instead of refusing the cast\n");
    Forge g(f, 3, {"Cleric_EssenceInfusion"});   // a Cleric who has the skill, as one who can cast it does: its expire hit then leaves the buff alone
    CHECKF(g.ok(), "the Cleric and his world: %s %s", g.rig.problem.c_str(), g.w.problem.c_str());
    if (!g.ok()) return;
    const abilities::EffectDef* infusion = f.catalog->effect_named("Cleric_EssenceInfusion");
    CHECK(infusion != nullptr, "the data has the Essence Infusion effect that switches the Divine Power cost on");
    if (!infusion) return;
    g.rig.know({"Cleric_EssenceInfusion", "Cleric_ChainsOfRestraint"});
    const abilities::HitDef* apply = f.catalog->hit_named("Cleric_EssenceInfusion");
    CHECK(apply != nullptr, "the data has the Essence Infusion hit that applies the buff");
    if (!apply) return;
    const double t0 = 100.0;
    g.rig.asks = false;   // the client asks only the apply hit of Essence Infusion, below (not its cancel and expire)
    const Refusal first = g.rig.trigger("Cleric_EssenceInfusion", t0, false);
    CHECKF(first == Refusal::None, "Essence Infusion is cast (%s)", refusal_name(first));
    if (first != Refusal::None) return;
    g.rig.run_to(t0 + 0.2);
    // the client asks for the ability's own hit, which puts the buff on the caster (the cancel and expire hits are not asked)
    const auto asked = g.rig.pipe->hit_request(g.rig.ability("Cleric_EssenceInfusion"), content::kTargetingDefaultSelf, apply->row.guid, 1, 1, t0 + 0.25);
    CHECKF(asked == AbilityPipeline::HitVerdict::Scheduled, "the client's Essence Infusion hit is taken (%d)", static_cast<int>(asked));
    g.rig.run_to(t0 + 1.2);   // the cost step of this cast comes about 0.6 s after its trigger, and its hit after that
    CHECKF(fixes::caster_effects(g.rig, infusion->row.guid) >= 1, "the Cleric holds Essence Infusion (%zu instance(s))", fixes::caster_effects(g.rig, infusion->row.guid));
    g.rig.set_current(6064629623747772676ll, 0.0);   // no Divine Power at all
    const double mana_before = g.rig.mana();
    const Refusal second = g.rig.trigger("Cleric_ChainsOfRestraint", t0 + 1.5, false);
    std::printf("  (the Chains' trigger: %s)\n", refusal_name(second));
    CHECKF(second == Refusal::None, "the Chains are cast, not refused for the Divine Power the Cleric does not hold");
    if (second != Refusal::None) return;
    g.rig.run_to(t0 + 3.9);   // through its cast bar and cost step (about 2.2 s after the trigger)
    const double mana_after = g.rig.mana();
    std::printf("  (the Chains' normal cost: Mana %.1f -> %.1f, Divine Power %.2f)\n", mana_before, mana_after, g.rig.pawn.stat(6064629623747772676ll));
    CHECKF(g.rig.pawn.stat(6064629623747772676ll) <= 0.01, "no Divine Power was taken, because there was none (%.2f)", g.rig.pawn.stat(6064629623747772676ll));
    CHECK(g.rig.pawn.broken.empty(), "(the pawn took every change)");
}
AOC_REGISTER_TEST(test_combat_fixes_the_chains_use_the_normal_cost);
