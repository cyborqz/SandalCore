// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_fixes_review_tests.inc: what the six fixes of combat_fixes_tests.inc reach beyond their own ability (included by combat_registered.inc inside
//  test_worldd_combat's anonymous namespace, after combat_fixes_tests.inc, whose checks and helpers it uses)
//
//    the Chains          the cost step takes the Divine Power or the normal Mana cost, and the chains go out
//    the Prismatic Beam  the hits its tick lands are the server's own, predicted on neither side
//    the Lament          its zone stands under the Bard whatever aim place the call carries
//    the cast end        which casts get the server's end and which keep their client's
//    the Arcane Circle   its buff ends with the circle also when the Mage left it and came back
// ============================================================================

namespace fixes {

constexpr int64_t kDivinePower = 6064629623747772676ll;   // Cleric_DivinePower
constexpr int64_t kChainsBranch = 109341746222712ll;      // the branch the client's cast bar of the Chains asks for

/// One cast of the Chains of Restraint by a level 25 Cleric whose Divine Power is `divine_power`, with Essence Infusion on or not: the client's cast bar asks for its branch 2.07 s after the
/// trigger, as the recordings show.  What the Cleric held before and after its cost step.
struct ChainsRun {
    bool        cast = false;
    bool        infused = false;
    bool        zone = false;
    double      mana_before = 0, mana_after = 0, power_before = 0, power_after = 0;
    std::string why;
};

ChainsRun chains_run(Fixture& f, bool infusion, double divine_power) {
    ChainsRun out;
    Forge g(f, 3, {"Cleric_EssenceInfusion"});   // a Cleric who has the skill, as one who can cast it does
    if (!g.ok()) {
        out.why = g.rig.problem + g.w.problem;
        return out;
    }
    const abilities::EffectDef* buff = f.catalog->effect_named("Cleric_EssenceInfusion");
    const abilities::HitDef* apply = f.catalog->hit_named("Cleric_EssenceInfusion");
    if (!buff || !apply) {
        out.why = "the data has no Essence Infusion";
        return out;
    }
    g.rig.know({"Cleric_EssenceInfusion", "Cleric_ChainsOfRestraint"});
    g.rig.asks = false;   // (the client asks only the hit that puts the buff on, below)
    constexpr double t0 = 100.0;
    if (infusion) {
        if (g.rig.trigger("Cleric_EssenceInfusion", t0, false) != Refusal::None) {
            out.why = "Essence Infusion is refused";
            return out;
        }
        g.rig.run_to(t0 + 0.2);
        g.rig.pipe->hit_request(g.rig.ability("Cleric_EssenceInfusion"), content::kTargetingDefaultSelf, apply->row.guid, 1, 1, t0 + 0.25);
    }
    g.rig.run_to(t0 + 1.2);
    out.infused = caster_effects(g.rig, buff->row.guid) >= 1;
    g.rig.set_current(kDivinePower, divine_power);
    const double ta = t0 + 1.5;
    const int32_t id = g.rig.next_client_id;
    out.cast = g.rig.trigger("Cleric_ChainsOfRestraint", ta, false) == Refusal::None;
    if (!out.cast) {
        out.why = "the Chains are refused";
        return out;
    }
    g.rig.run_to(ta + 2.07);
    g.rig.pipe->branch_request(id, kChainsBranch, ta + 2.07);
    g.rig.run_to(ta + 2.10);   // the request is in and the cost step (0.17 s after it) has not come yet
    out.mana_before = g.rig.mana();
    out.power_before = g.rig.pawn.stat(kDivinePower);
    g.rig.run_to(ta + 2.60);   // the cost step is behind
    out.mana_after = g.rig.mana();
    out.power_after = g.rig.pawn.stat(kDivinePower);
    g.rig.run_to(ta + 4.5);
    for (const std::string& z : ground::opened_zones(g.rig)) out.zone = out.zone || z == "Cleric_ChainsOfRestraint";
    return out;
}

}  // namespace fixes

void test_combat_fixes_the_chains_pay_what_the_data_enables(Fixture& f) {
    std::printf("the Chains of Restraint: the cost step takes the Divine Power when the Cleric has it and the normal Mana cost when he has not, and the chains go out\n");
    const fixes::ChainsRun plain = fixes::chains_run(f, false, 0.0);   // no Essence Infusion: the normal cost
    const fixes::ChainsRun short_of_power = fixes::chains_run(f, true, 0.0);
    const fixes::ChainsRun with_power = fixes::chains_run(f, true, 100.0);
    CHECKF(plain.cast && short_of_power.cast && with_power.cast, "the three casts are made (%s %s %s)", plain.why.c_str(), short_of_power.why.c_str(), with_power.why.c_str());
    if (!plain.cast || !short_of_power.cast || !with_power.cast) return;
    CHECK(short_of_power.infused && with_power.infused && !plain.infused, "Essence Infusion is on for the two Clerics who cast it, and not for the one who did not");
    const double plain_cost = plain.mana_before - plain.mana_after;
    const double short_cost = short_of_power.mana_before - short_of_power.mana_after;
    std::printf("  (Mana %.1f -> %.1f without the infusion, %.1f -> %.1f with it and no Divine Power, %.1f -> %.1f with 100 Divine Power taking %.2f -> %.2f)\n", plain.mana_before,
                plain.mana_after, short_of_power.mana_before, short_of_power.mana_after, with_power.mana_before, with_power.mana_after, with_power.power_before, with_power.power_after);
    CHECKF(plain_cost > 100.0, "the normal cost is paid in Mana without Essence Infusion (%.1f)", plain_cost);
    CHECKF(std::fabs(short_cost - plain_cost) < 17.0, "with Essence Infusion on and no Divine Power the Cleric pays the normal Mana cost too, not none and not more (%.1f against %.1f)", short_cost, plain_cost);
    CHECKF(short_of_power.power_after >= -1e-6 && short_of_power.power_after <= short_of_power.power_before + 0.5, "...and no Divine Power is taken (%.2f -> %.2f)", short_of_power.power_before,
           short_of_power.power_after);
    CHECKF(with_power.power_before - with_power.power_after >= 58.0, "with the Divine Power the cost step takes its 60 (%.2f -> %.2f)", with_power.power_before, with_power.power_after);
    CHECKF(with_power.mana_after >= with_power.mana_before - 0.01, "...and no Mana (%.1f -> %.1f)", with_power.mana_before, with_power.mana_after);
    CHECK(plain.zone && short_of_power.zone && with_power.zone, "the chains go out in all three casts: the zone's actor opens");
}
AOC_REGISTER_TEST(test_combat_fixes_the_chains_pay_what_the_data_enables);

void test_combat_fixes_the_beam_children_are_unpredicted(Fixture& f) {
    std::printf("the Prismatic Beam: the hits its Recurse tick lands are confirmed as the server's own, predicted on neither side, as every recorded child confirmation of the beam is\n");
    Forge m(f, 8, {});
    CHECKF(m.ok(), "the Mage and his world: %s %s", m.rig.problem.c_str(), m.w.problem.c_str());
    if (!m.ok()) return;
    const uint64_t wolf = m.wolf(300, 0);
    const abilities::AbilityDef* def = f.catalog->ability_named("Mage_PrismaticBeam");
    const abilities::HitDef* recurse = f.catalog->hit_named("Mage_PrismaticBeam_Recurse");
    const abilities::HitDef* damage = f.catalog->hit_named("Mage_PrismaticBeam_Dmg");
    CHECK(wolf != 0 && def && recurse && damage, "a Wolf 3 m away and the Beam's data");
    if (!wolf || !def || !recurse || !damage) return;
    m.rig.know({"Mage_PrismaticBeam"});
    m.rig.asks = false;   // (no hit is asked but the Recurse tick below: a hit the client asks by itself is predicted on its side, and the tick's children are not)
    const double t0 = 100.0;
    CHECK(m.rig.trigger("Mage_PrismaticBeam", t0, false) == Refusal::None, "the Beam is cast");
    m.rig.run_to(t0 + 0.2);
    m.rig.pipe->hit_request(def->row.guid, fixes::targeting_of(*def, recurse->row.guid), recurse->row.guid, 1, 0, t0 + 0.3, {wolf});
    m.rig.run_to(t0 + 0.6);
    std::set<int64_t> children;
    for (const content::RecursiveHitRow& r : recurse->recursive) children.insert(r.child_hit);
    size_t confirmed = 0, predicted = 0, damage_confirmed = 0;
    for (const Seen& s : m.rig.log) {
        for (const bootstrap::HitConfirm& c : s.confirmations) {
            if (!children.count(c.record_guid)) continue;
            ++confirmed;
            damage_confirmed += c.record_guid == damage->row.guid ? 1 : 0;
            predicted += (c.predicted_autonomous || c.predicted_simulated) ? 1 : 0;
        }
    }
    CHECKF(damage_confirmed >= 1, "the damage hit is confirmed (%zu)", damage_confirmed);
    CHECKF(confirmed >= 1 && predicted == 0, "none of the %zu child confirmations is predicted on the autonomous or the simulated side (%zu are)", confirmed, predicted);
    CHECK(m.rig.pawn.broken.empty(), "(the pawn took every change)");
}
AOC_REGISTER_TEST(test_combat_fixes_the_beam_children_are_unpredicted);

void test_combat_fixes_the_beam_children_keep_their_own_requirements(Fixture& f) {
    std::printf("the Prismatic Beam: with the Fire empowerment on, the tick lands the damage and the Fire status and none of the other empowerments' hits, as the recorded Fire beam's ticks do\n");
    Forge m(f, 8, {});
    CHECKF(m.ok(), "the Mage and his world: %s %s", m.rig.problem.c_str(), m.w.problem.c_str());
    if (!m.ok()) return;
    const uint64_t wolf = m.wolf(300, 0);
    const abilities::AbilityDef* def = f.catalog->ability_named("Mage_PrismaticBeam");
    const abilities::EffectDef* fire = f.catalog->effect_named("Mage_EE_Fire");
    const abilities::HitDef* recurse = f.catalog->hit_named("Mage_PrismaticBeam_Recurse");
    const abilities::HitDef* damage = f.catalog->hit_named("Mage_PrismaticBeam_Dmg");
    const abilities::HitDef* fire_status = f.catalog->hit_named("Mage_PrismaticBeam_Fire_Status");
    const abilities::HitDef* ice_status = f.catalog->hit_named("Mage_PrismaticBeam_Ice_Status");
    const abilities::HitDef* lightning_status = f.catalog->hit_named("Mage_PrismaticBeam_Lightning_Status");
    const abilities::HitDef* mana = f.catalog->hit_named("Mage_PrismaticBeam_Arcane_Mana");
    CHECK(wolf != 0 && def && fire && recurse && damage && fire_status && ice_status && lightning_status && mana, "a Wolf 3 m away, the Fire empowerment and the Beam's hits are in the data");
    if (!wolf || !def || !fire || !recurse || !damage || !fire_status || !ice_status || !lightning_status || !mana) return;
    m.rig.know({"Mage_PrismaticBeam"});
    m.rig.asks = false;
    CHECK(m.rig.pipe->grant_effect(fire->row.guid, 1, 99.0), "the Mage holds the Fire empowerment");
    const double t0 = 100.0;
    CHECK(m.rig.trigger("Mage_PrismaticBeam", t0, false) == Refusal::None, "the Beam is cast");
    m.rig.run_to(t0 + 0.2);
    m.rig.pipe->hit_request(def->row.guid, fixes::targeting_of(*def, recurse->row.guid), recurse->row.guid, 1, 0, t0 + 0.3, {wolf});
    m.rig.run_to(t0 + 0.6);
    const auto n = [&](const abilities::HitDef* h) { return forged_confirmations(m.rig, h->row.guid).size(); };
    CHECKF(n(damage) >= 1 && n(fire_status) >= 1, "the damage (%zu) and the Fire status (%zu) are confirmed on the Wolf", n(damage), n(fire_status));
    CHECKF(n(ice_status) == 0 && n(lightning_status) == 0 && n(mana) == 0, "the Ice status (%zu), the Lightning status (%zu) and the Arcane mana hit (%zu) are not: their requirements do not hold", n(ice_status),
           n(lightning_status), n(mana));
    CHECK(m.rig.pawn.broken.empty(), "(the pawn took every change)");
}
AOC_REGISTER_TEST(test_combat_fixes_the_beam_children_keep_their_own_requirements);

void test_combat_fixes_the_lament_ignores_the_aim_place(Fixture& f) {
    std::printf("the Chilling Lament: a cast with an aim place and no target still has its zone under the Bard, not at the place\n");
    const std::string wolf_path = wolf_archetype(f);
    BardMobWorld bw(f, wolf_path, 2);
    CHECKF(bw.problem.empty(), "the Bard and his world: %s", bw.problem.c_str());
    if (!bw.problem.empty()) return;
    const int64_t zone = ground::zone_of(f, "Bard_Song_Lament");
    const abilities::EffectDef* chilled = f.catalog->effect_named("Status_Chilled");
    CHECK(zone != 0 && chilled, "the data has the Lament's zone and Chilled");
    if (zone == 0 || !chilled) return;
    const auto given_zone = ground::give_zone(*bw.rig.catalog, zone, "EAbilitySystemOrigin::HitLocation", "EAbilitySystemAttachType::None", 300.0, true);
    const uint64_t near_wolf = bw.w.spawn(wolf_path, 200, 0, 10, 0);
    const uint64_t far_wolf = bw.w.spawn(wolf_path, 800, 0, 10, 0);
    CHECK(near_wolf != 0 && far_wolf != 0, "a Wolf 2 m from the Bard and one 8 m from him");
    if (!near_wolf || !far_wolf) return;
    bw.rig.know({"Bard_Song_Lament"});
    const double t0 = 100.0;
    const ground::Cast cast = ground::cast_zone(bw.rig, "Bard_Song_Lament", t0, combat::Vec3{800, 0, 1098}, {});   // aimed at the ground 8 m ahead, at nobody
    CHECK(cast.cast, "the Lament is cast with an aim place");
    if (!cast.cast) return;
    ground::run(bw.rig, *bw.world, t0, t0 + 0.8);
    CHECKF(bw.world->effect_stacks(near_wolf, chilled->row.guid) >= 1, "the Wolf 2 m from the Bard took the zone's Chilled hit (%d stack)", bw.world->effect_stacks(near_wolf, chilled->row.guid));
    CHECKF(bw.world->effect_stacks(far_wolf, chilled->row.guid) == 0, "the Wolf at the aim place took none (%d stack)", bw.world->effect_stacks(far_wolf, chilled->row.guid));
    CHECK(ground::opened_zones(bw.rig).empty(), "and no zone actor is sent");
    CHECK(bw.rig.pawn.broken.empty(), "(the pawn took every change)");
}
AOC_REGISTER_TEST(test_combat_fixes_the_lament_ignores_the_aim_place);

void test_combat_fixes_the_cast_end_reaches_only_ground_casts_the_client_never_asked(Fixture& f) {
    std::printf("the cast end: the Void gets the server's end, and the Chains whose branch the client asked and a Fireball aimed at a Wolf get none\n");
    // the Void, a ground cast: the client's call names the place and no branch request comes
    {
        Rig rig(6, 25, {});
        CHECKF(rig.ok(), "the Summoner: %s", rig.problem.c_str());
        if (rig.ok()) {
            rig.know({"Summoner_Void"});
            TriggerInfo info;
            info.place = AimPlace{500, 0, 1000};
            const int32_t id = rig.next_client_id;
            const Refusal refusal = rig.pipe->trigger(rig.ability("Summoner_Void"), rig.next_client_id++, 100.0, info);
            CHECKF(refusal == Refusal::None, "the Void is cast at a place (%s)", refusal_name(refusal));
            rig.run_to(112.0);
            const std::vector<double> ends = fixes::end_times(rig, id);
            CHECKF(ends.size() == 1 && ends.front() > 105.0 && ends.front() < 111.0, "the server ends it once, at its own end (%zu end(s), the first at %.2f s)", ends.size(),
                   ends.empty() ? -1.0 : ends.front() - 100.0);
        }
    }
    // the Chains, a ground cast whose cast bar did ask: the client ends its own cast
    {
        Rig rig(3, 25, {});
        CHECKF(rig.ok(), "the Cleric: %s", rig.problem.c_str());
        if (rig.ok()) {
            rig.know({"Cleric_ChainsOfRestraint"});
            rig.asks = false;
            TriggerInfo info;
            info.place = AimPlace{500, 0, 1000};
            const int32_t id = rig.next_client_id;
            const Refusal refusal = rig.pipe->trigger(rig.ability("Cleric_ChainsOfRestraint"), rig.next_client_id++, 100.0, info);
            CHECKF(refusal == Refusal::None, "the Chains are cast at a place (%s)", refusal_name(refusal));
            rig.run_to(102.07);
            rig.pipe->branch_request(id, fixes::kChainsBranch, 102.07);
            rig.run_to(112.0);
            CHECKF(fixes::end_times(rig, id).empty(), "the client asked for the branch: the server sends no end of its own (%zu)", fixes::end_times(rig, id).size());
        }
    }
    // a Fireball aimed at a Wolf and never asked for its branch: a cast aimed at an actor keeps its client's own end
    {
        Forge m(f, 8, {});
        CHECKF(m.ok(), "the Mage and his world: %s %s", m.rig.problem.c_str(), m.w.problem.c_str());
        const uint64_t wolf = m.ok() ? m.wolf(600, 0) : 0;
        if (m.ok() && wolf) {
            m.rig.know({"Mage_Fireball"});
            TriggerInfo info;
            info.target_count = 1;
            info.target_actors = {wolf};
            const int32_t id = m.rig.next_client_id;
            const Refusal refusal = m.rig.pipe->trigger(m.rig.ability("Mage_Fireball"), m.rig.next_client_id++, 100.0, info);
            CHECKF(refusal == Refusal::None, "the Fireball is cast at a Wolf (%s)", refusal_name(refusal));
            m.rig.run_to(112.0);
            CHECKF(fixes::end_times(m.rig, id).empty(), "the server sends no end for it (%zu)", fixes::end_times(m.rig, id).size());
        }
    }
}
AOC_REGISTER_TEST(test_combat_fixes_the_cast_end_reaches_only_ground_casts_the_client_never_asked);

void test_combat_fixes_the_arcane_circle_buff_ends_after_the_caster_left_and_came_back(Fixture& f) {
    std::printf("the Arcane Circle: the Mage who runs out of the circle and back, or stays outside, loses the buff when the circle ends\n");
    Forge m(f, 8, {});
    CHECKF(m.ok(), "the Mage and his world: %s %s", m.rig.problem.c_str(), m.w.problem.c_str());
    if (!m.ok()) return;
    const abilities::EffectDef* buff = f.catalog->effect_named("Mage_ArcaneCircle");
    CHECK(buff != nullptr, "the data has the Arcane Circle's buff");
    if (!buff) return;
    m.rig.know({"Mage_ArcaneCircle"});
    const auto held = [&] { return fixes::caster_effects(m.rig, buff->row.guid); };
    const double t0 = 100.0;
    CHECK(m.rig.trigger("Mage_ArcaneCircle", t0, false) == Refusal::None, "the circle is cast");
    m.rig.run_to(t0 + 0.5);
    CHECKF(held() >= 1, "the buff is on the Mage (%zu)", held());
    // he sprints out of the circle (3.5 m) one second in, and is back in it at 3 s
    m.rig.run_to(t0 + 1.0);
    m.player.rig_fighter().move_to({1200, 0, 1098});
    m.rig.run_to(t0 + 3.0);
    m.player.rig_fighter().move_to({0, 0, 1098});
    m.rig.run_to(t0 + 5.0);
    m.rig.run_to(t0 + 11.0);
    CHECKF(held() == 0, "the circle has ended (10 s): the buff is gone (%zu instance(s) at 11 s)", held());
    // a second circle he leaves and does not come back to
    CHECK(m.rig.trigger("Mage_ArcaneCircle", t0 + 70.0, false) == Refusal::None, "a second circle is cast after the cooldown");
    m.rig.run_to(t0 + 70.5);
    m.player.rig_fighter().move_to({1200, 0, 1098});
    m.rig.run_to(t0 + 82.0);
    CHECKF(held() == 0, "the second circle ended with him outside it: the buff is gone (%zu instance(s))", held());
    CHECK(m.rig.pawn.broken.empty(), "(the pawn took every change)");
}
AOC_REGISTER_TEST(test_combat_fixes_the_arcane_circle_buff_ends_after_the_caster_left_and_came_back);
