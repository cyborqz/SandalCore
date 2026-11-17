// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_mage_review_tests.inc: more tests of the four Mage fixes (included by combat_registered.inc inside
//  test_worldd_combat's anonymous namespace, after combat_mage_fixes_tests.inc, whose helpers and macros it uses)
//
//    the Blizzard        its whole channel (14 ticks), and the freeze the client asks as the channel ends, at its own end and when the key goes up
//    the Magma Field     its field comes with the end of its cast bar, not with its start, and not at all when the bar is cut short
//    the Prismatic Beam  the client's flood of one self hit is a request the cast has used up: it costs no request budget (the session's test is in test_worldd_ability_session.cpp)
//    a dead caster       nothing of his channel lands after his death
// ============================================================================

// ---- the Blizzard's whole channel and its end --------------------------------------------------------------------------------------------
// The recorded Blizzard's montage was cleared 8.717 s after the trigger and its client asks the damage hit about every 0.634 s: fourteen ticks. The cap of a hit's requests and the
// channel's window hold all fourteen, and the window is stretched for a client that plays the montage slower than the recording. Chilled enemies "caught in the end of the blizzard are Frozen": that hit (a ServerVerify one) is asked as the channel ends.
void test_mage_review_the_blizzard_takes_its_fourteen_ticks_and_its_end_hit(Fixture& f) {
    std::printf("the Blizzard: the 14 ticks of its whole channel (and two more of a slower client) are taken, a 17th is not, and the freeze the client asks as the channel ends lands on a chilled Wolf\n");
    Forge m(f, 8, {});
    CHECKF(m.ok(), "the Mage and his world: %s %s", m.rig.problem.c_str(), m.w.problem.c_str());
    if (!m.ok()) return;
    const abilities::AbilityDef* def = f.catalog->ability_named("Mage_Blizzard");
    const abilities::HitDef* damage = f.catalog->hit_named("Mage_Blizzard_Damage_Hit");
    const abilities::HitDef* freeze = f.catalog->hit_named("Mage_Status_Chilled->Frozen");
    const abilities::EffectDef* chilled = f.catalog->effect_named("Status_Chilled");
    const abilities::EffectDef* frozen = f.catalog->effect_named("Status_Frozen");
    CHECK(def && damage && freeze && chilled && frozen, "the data has the Blizzard's damage and freeze hits and the Chilled and Frozen effects");
    if (!def || !damage || !freeze || !chilled || !frozen) return;
    const uint64_t wolf = m.wolf(300, 0);
    CHECK(wolf != 0, "a Wolf 3 m from the Mage");
    if (!wolf) return;
    m.rig.know({"Mage_Blizzard"});
    m.rig.pipe->set_data_timings(true);   // (the channel's recorded end is what this test is about)
    m.rig.asks = false;
    const double t0 = 100.0;
    CHECK(m.rig.trigger("Mage_Blizzard", t0, false) == Refusal::None, "the Blizzard is cast");
    m.rig.run_to(t0 + 0.2);
    int taken = 0;
    for (int i = 0; i < 14; ++i) {
        const double at = t0 + 0.7 + 0.634 * static_cast<double>(i);
        if (mage_fixes::ask_tick(m.rig, *def, *damage, at, wolf, false) == AbilityPipeline::HitVerdict::Scheduled) ++taken;
        m.rig.run_to(at + 0.1);
        m.world.gm_heal(wolf);   // (the Wolf stays alive for the end of the channel)
    }
    CHECKF(taken == 14, "the channel's fourteen ticks are all taken (%d of 14, the last %.2f s after the trigger)", taken, 0.7 + 0.634 * 13.0);
    // the end of the channel: the montage's clear, and the freeze the client asks there
    effects::Applied put;
    put.def = chilled;
    put.duration = 600.0;
    m.world.apply_effects(wolf, 77, {put}, t0 + 8.95);
    CHECKF(m.world.effect_stacks(wolf, chilled->row.guid) == 1 && m.world.effect_stacks(wolf, frozen->row.guid) == 0, "the Wolf is chilled and not frozen yet (%d, %d)",
           m.world.effect_stacks(wolf, chilled->row.guid), m.world.effect_stacks(wolf, frozen->row.guid));
    CHECKF(mage_fixes::ask_tick(m.rig, *def, *freeze, t0 + 9.0, wolf, false) == AbilityPipeline::HitVerdict::Scheduled, "the freeze the client asks as the channel ends (9.0 s after the trigger) is taken");
    m.rig.run_to(t0 + 9.2);
    CHECKF(m.world.effect_stacks(wolf, frozen->row.guid) >= 1, "the chilled Wolf is frozen (%d stack)", m.world.effect_stacks(wolf, frozen->row.guid));
    // a client that plays the channel up to 1.5 times slower than the recording asks two more ticks past the recorded end: they are taken, and what stops the next one is the hit's count (16)
    // while the cast's window is open, and the window itself once it is over
    CHECKF(mage_fixes::ask_tick(m.rig, *def, *damage, t0 + 0.7 + 0.634 * 14.0, wolf, false) == AbilityPipeline::HitVerdict::Scheduled &&
               mage_fixes::ask_tick(m.rig, *def, *damage, t0 + 0.7 + 0.634 * 15.0, wolf, false) == AbilityPipeline::HitVerdict::Scheduled,
           "the 15th and 16th ticks of a slower client (%.2f and %.2f s after the trigger) are taken", 0.7 + 0.634 * 14.0, 0.7 + 0.634 * 15.0);
    CHECKF(mage_fixes::ask_tick(m.rig, *def, *damage, t0 + 0.7 + 0.634 * 16.0, wolf, false) == AbilityPipeline::HitVerdict::TooMany, "a 17th tick is more than a hit takes in a cast");
    CHECKF(mage_fixes::ask_tick(m.rig, *def, *damage, t0 + 14.5, wolf, false) == AbilityPipeline::HitVerdict::NoCast, "and 14.5 s after the trigger, past the window, the cast is not there");
    CHECK(m.rig.pawn.broken.empty(), "(the pawn took every change)");
}
AOC_REGISTER_TEST(test_mage_review_the_blizzard_takes_its_fourteen_ticks_and_its_end_hit);

// The key goes up: the client's montage goes to the end of the blizzard and asks the freeze there. The record took no more ticks (a tick asked past the slack finds no cast), but kept the end hit.
void test_mage_review_the_blizzard_released_still_takes_its_end_hit(Fixture& f) {
    std::printf("the Blizzard: when the key goes up the freeze the client asks as the channel ends is still taken, and the ticks are not\n");
    Forge m(f, 8, {});
    CHECKF(m.ok(), "the Mage and his world: %s %s", m.rig.problem.c_str(), m.w.problem.c_str());
    if (!m.ok()) return;
    const abilities::AbilityDef* def = f.catalog->ability_named("Mage_Blizzard");
    const abilities::HitDef* damage = f.catalog->hit_named("Mage_Blizzard_Damage_Hit");
    const abilities::HitDef* freeze = f.catalog->hit_named("Mage_Status_Chilled->Frozen");
    const abilities::EffectDef* chilled = f.catalog->effect_named("Status_Chilled");
    const abilities::EffectDef* frozen = f.catalog->effect_named("Status_Frozen");
    if (!def || !damage || !freeze || !chilled || !frozen) {
        CHECK(false, "the data has the Blizzard's hits and the Chilled and Frozen effects");
        return;
    }
    const uint64_t wolf = m.wolf(300, 0);
    CHECK(wolf != 0, "a Wolf 3 m from the Mage");
    if (!wolf) return;
    m.rig.know({"Mage_Blizzard"});
    m.rig.pipe->set_data_timings(true);
    m.rig.asks = false;
    const double t0 = 100.0;
    const int32_t id = m.rig.next_client_id;
    CHECK(m.rig.trigger("Mage_Blizzard", t0, false) == Refusal::None, "the Blizzard is cast");
    m.rig.run_to(t0 + 0.2);
    effects::Applied put;
    put.def = chilled;
    put.duration = 600.0;
    m.world.apply_effects(wolf, 77, {put}, t0 + 0.5);
    CHECK(mage_fixes::ask_tick(m.rig, *def, *damage, t0 + 0.7, wolf, false) == AbilityPipeline::HitVerdict::Scheduled, "its first tick is taken");
    m.rig.run_to(t0 + 1.9);
    m.rig.pipe->cancel(id, t0 + 2.0);   // the key goes up
    m.rig.run_to(t0 + 2.1);
    CHECKF(mage_fixes::ask_tick(m.rig, *def, *freeze, t0 + 2.3, wolf, false) == AbilityPipeline::HitVerdict::Scheduled, "the freeze asked 0.3 s after the key went up is taken");
    m.rig.run_to(t0 + 2.6);
    CHECKF(m.world.effect_stacks(wolf, frozen->row.guid) >= 1, "the chilled Wolf is frozen (%d stack)", m.world.effect_stacks(wolf, frozen->row.guid));
    CHECKF(mage_fixes::ask_tick(m.rig, *def, *damage, t0 + 2.9, wolf, false) == AbilityPipeline::HitVerdict::NoCast, "a tick asked 0.9 s after the key went up finds no cast");
    CHECK(m.rig.pawn.broken.empty(), "(the pawn took every change)");
}
AOC_REGISTER_TEST(test_mage_review_the_blizzard_released_still_takes_its_end_hit);

// ---- a caster who dies mid-channel -------------------------------------------------------------------------------------------------------
// The test of the channel's cut ends shows the verdict; this one shows that nothing lands: no Health lost after the death, no damage number.
void test_mage_review_nothing_of_a_dead_casters_channel_lands(Fixture& f) {
    std::printf("a caster who dies mid-channel: the Blizzard's later ticks hurt nobody and draw no number\n");
    Forge m(f, 8, {});
    CHECKF(m.ok(), "the Mage and his world: %s %s", m.rig.problem.c_str(), m.w.problem.c_str());
    if (!m.ok()) return;
    const abilities::AbilityDef* def = f.catalog->ability_named("Mage_Blizzard");
    const abilities::HitDef* damage = f.catalog->hit_named("Mage_Blizzard_Damage_Hit");
    if (!def || !damage) {
        CHECK(false, "the data has the Blizzard's damage hit");
        return;
    }
    const uint64_t wolf = m.wolf(300, 0);
    CHECK(wolf != 0, "a Wolf 3 m from the Mage");
    if (!wolf) return;
    m.rig.know({"Mage_Blizzard"});
    m.rig.pipe->set_data_timings(true);
    m.rig.asks = false;
    const double t0 = 100.0;
    CHECK(m.rig.trigger("Mage_Blizzard", t0, false) == Refusal::None, "the Blizzard is cast");
    m.rig.run_to(t0 + 0.2);
    CHECK(mage_fixes::ask_tick(m.rig, *def, *damage, t0 + 0.7, wolf, false) == AbilityPipeline::HitVerdict::Scheduled, "its first tick is taken");
    m.rig.run_to(t0 + 0.9);
    const double health = m.world.health_of(wolf);
    const int numbers = mage_fixes::confirmations_of(m.rig, damage->row.guid);
    CHECKF(health < m.world.max_health_of(wolf) - 0.01 && numbers == 1, "the first tick hurt the Wolf (%.1f of %.1f Health, %d number)", health, m.world.max_health_of(wolf), numbers);
    m.rig.pipe->set_dead(true);
    m.rig.run_to(t0 + 1.2);
    (void)mage_fixes::ask_tick(m.rig, *def, *damage, t0 + 1.5, wolf, false);
    m.rig.run_to(t0 + 2.0);
    CHECKF(std::fabs(m.world.health_of(wolf) - health) < 1e-6 && mage_fixes::confirmations_of(m.rig, damage->row.guid) == numbers,
           "the Wolf lost nothing after the death and no new number was drawn (%.1f Health, %d number)", m.world.health_of(wolf), mage_fixes::confirmations_of(m.rig, damage->row.guid));
}
AOC_REGISTER_TEST(test_mage_review_nothing_of_a_dead_casters_channel_lands);

// ---- two Mages on one Wolf ---------------------------------------------------------------------------------------------------------------
// The earlier two-Mage test gives each Mage a world of his own. Here both stand in one world and channel their Blizzards at the same Wolf: each cast keeps its own window and its
// own ticks, each Mage's damage numbers are his own, and each earns threat on the Wolf's table.
void test_mage_review_two_mages_channel_on_one_wolf(Fixture& f) {
    std::printf("two Mages side by side on one Wolf: each Blizzard keeps its own nine ticks and each Mage earns his threat on the Wolf\n");
    Forge a(f, 8, {});
    CHECKF(a.ok(), "the first Mage and his world: %s %s", a.rig.problem.c_str(), a.w.problem.c_str());
    if (!a.ok()) return;
    Rig brig(8, 25, {}, true, 11);
    combat_rig::FakePlayer bplayer(brig, 78, {0, 200, 1098});
    combat::WorldHitPort bport(a.world, a.w.registry, bplayer.fighter());
    CHECKF(brig.ok(), "the second Mage: %s", brig.problem.c_str());
    if (!brig.ok()) return;
    a.world.add_player(&bplayer);
    brig.pipe->set_combat(&bport);
    const abilities::AbilityDef* def = f.catalog->ability_named("Mage_Blizzard");
    const abilities::HitDef* damage = f.catalog->hit_named("Mage_Blizzard_Damage_Hit");
    const uint64_t wolf = a.wolf(300, 100);
    CHECK(def && damage && wolf != 0, "the data's Blizzard and one Wolf near both Mages");
    if (!def || !damage || !wolf) return;
    a.rig.know({"Mage_Blizzard"});
    brig.know({"Mage_Blizzard"});
    a.rig.pipe->set_data_timings(true);
    brig.pipe->set_data_timings(true);
    a.rig.asks = brig.asks = false;
    const double t0 = 300.0;
    CHECK(a.rig.trigger("Mage_Blizzard", t0, false) == Refusal::None && brig.trigger("Mage_Blizzard", t0 + 0.05, false) == Refusal::None, "both Mages cast their Blizzards");
    a.rig.run_to(t0 + 0.3);
    brig.run_to(t0 + 0.35);
    int taken_a = 0, taken_b = 0;
    for (int i = 0; i < 9; ++i) {
        const double at = t0 + 0.7 + 0.634 * static_cast<double>(i);
        if (mage_fixes::ask_tick(a.rig, *def, *damage, at, wolf, false) == AbilityPipeline::HitVerdict::Scheduled) ++taken_a;
        a.rig.run_to(at + 0.1);
        if (mage_fixes::ask_tick(brig, *def, *damage, at + 0.05, wolf, false) == AbilityPipeline::HitVerdict::Scheduled) ++taken_b;
        brig.run_to(at + 0.15);
        a.world.gm_heal(wolf);   // (the Wolf outlives eighteen ticks)
    }
    CHECKF(taken_a == 9 && taken_b == 9, "both channels take their nine ticks (%d and %d)", taken_a, taken_b);
    CHECKF(mage_fixes::confirmations_of(a.rig, damage->row.guid) == 9 && mage_fixes::confirmations_of(brig, damage->row.guid) == 9, "each Mage's client draws his own nine numbers (%d and %d)",
           mage_fixes::confirmations_of(a.rig, damage->row.guid), mage_fixes::confirmations_of(brig, damage->row.guid));
    const npc::ai::NpcAi* ai = a.world.ai_of(wolf);
    CHECKF(ai != nullptr && ai->threat().of(77) > 0.0 && ai->threat().of(78) > 0.0, "both Mages are on the Wolf's threat table (%.1f and %.1f)", ai ? ai->threat().of(77) : 0.0, ai ? ai->threat().of(78) : 0.0);
    CHECK(a.rig.pawn.broken.empty() && brig.pawn.broken.empty(), "(both pawns took every change)");
}
AOC_REGISTER_TEST(test_mage_review_two_mages_channel_on_one_wolf);

// ---- the Magma Field's field comes with the end of its bar -------------------------------------------------------------------------------
// The talent says "Upon cast completion, an additional Magma Field spawns", and the recorded field's first damage came with the cost step (1,378 ms after the trigger, the bar's request at
// 1,277 ms). The field opened with the start of the cast, so it burned the Wolf for the whole bar before the spell was cast.
void test_mage_review_the_magma_fields_field_comes_with_the_end_of_its_bar(Fixture& f) {
    std::printf("the Magma Field: its field opens with the end of its cast bar (the client's request, or the deadline), not with its start, and never when the bar is cut short\n");
    constexpr int64_t kBarEnd = 6064629060044980230ll;   // the branch the client names when its bar ends (Mage_MagmaField_Complete)
    const auto cast = [&](const std::vector<std::string>& skills, const char* what, double request_at, bool dodge) {
        Forge m(f, 8, skills);
        CHECKF(m.ok(), "the Mage with %s and his world: %s %s", what, m.rig.problem.c_str(), m.w.problem.c_str());
        if (!m.ok()) return;
        const uint64_t wolf = m.wolf(300, 0);
        CHECKF(wolf != 0, "a Wolf 3 m from the Mage (%s)", what);
        if (!wolf) return;
        m.rig.know({"Mage_MagmaField"});
        const int64_t ability = m.rig.ability("Mage_MagmaField");
        const double t0 = 100.0;
        TriggerInfo info;
        info.place = AimPlace{300, 0, 1000};   // the ground under the Wolf
        const int32_t id = m.rig.next_client_id++;
        CHECKF(m.rig.pipe->trigger(ability, id, t0, info) == Refusal::None, "the Magma Field is cast at the ground under the Wolf (%s)", what);
        m.rig.run_to(t0 + 0.6);
        CHECKF(mage_fixes::zone_ops(m.rig, bootstrap::ZoneOp::Kind::Open) == 0 && m.world.max_health_of(wolf) - m.world.health_of(wolf) < 0.01,
               "0.6 s into the bar: no field yet and the Wolf is unhurt (%s: %zu opens, %.1f Health lost)", what, mage_fixes::zone_ops(m.rig, bootstrap::ZoneOp::Kind::Open),
               m.world.max_health_of(wolf) - m.world.health_of(wolf));
        if (dodge) {
            CHECKF(m.rig.pipe->end_by_dodge(t0 + 0.7) >= 1, "a Dodge ends the bar (%s)", what);
            m.rig.run_to(t0 + 5.0);
            CHECKF(mage_fixes::zone_ops(m.rig, bootstrap::ZoneOp::Kind::Open) == 0 && m.world.max_health_of(wolf) - m.world.health_of(wolf) < 0.01,
                   "the cut bar made no field and the Wolf is unhurt (%s: %zu opens, %.1f Health lost)", what, mage_fixes::zone_ops(m.rig, bootstrap::ZoneOp::Kind::Open),
                   m.world.max_health_of(wolf) - m.world.health_of(wolf));
            return;
        }
        if (request_at > 0) {
            m.rig.run_to(t0 + request_at - 0.1);
            CHECKF(mage_fixes::zone_ops(m.rig, bootstrap::ZoneOp::Kind::Open) == 0, "just before the client's request for the bar's end there is still no field (%s)", what);
            m.rig.pipe->branch_request(id, kBarEnd, t0 + request_at);
            m.rig.run_to(t0 + request_at + 0.6);
        } else {
            m.rig.run_to(t0 + 3.0);   // (no request comes: the deadline, 3.2 s, and the cost step after it)
            CHECKF(mage_fixes::zone_ops(m.rig, bootstrap::ZoneOp::Kind::Open) == 0, "3.0 s into a bar whose request never comes there is still no field (%s)", what);
            m.rig.run_to(t0 + 3.6);
        }
        CHECKF(mage_fixes::zone_ops(m.rig, bootstrap::ZoneOp::Kind::Open) == 1, "the field opens with the end of the bar (%s: %zu opens)", what, mage_fixes::zone_ops(m.rig, bootstrap::ZoneOp::Kind::Open));
        CHECKF(m.world.max_health_of(wolf) - m.world.health_of(wolf) > 0.01, "and its first hit hurts the Wolf (%s: %.1f Health lost)", what, m.world.max_health_of(wolf) - m.world.health_of(wolf));
        CHECK(m.rig.pawn.broken.empty(), "(the pawn took every change)");
    };
    cast({}, "no talent, the client's request at 2.1 s", 2.1, false);
    cast({"Mage_Passive_MagmaField_Pyromania"}, "Pyromania, the client's request at 1.2 s", 1.2, false);
    cast({}, "no talent, no request", 0.0, false);
    cast({}, "no talent, cut by a Dodge", 0.0, true);
}
AOC_REGISTER_TEST(test_mage_review_the_magma_fields_field_comes_with_the_end_of_its_bar);

// ---- the beam's client floods one self hit -------------------------------------------------------------------------------------------
// The recorded Prismatic Beam: 566 requests of its Arcane Mana in about three seconds (two every 11 ms), ten of its Recurse tick. The session's budget (40 at once, 40 a second) can only hold the
// ticks if the flood is not charged to it: a request the cast has used up (its count reached, or within the gap of the last one taken) is `spent`.
void test_mage_review_a_request_the_cast_has_used_up_is_spent(Fixture& f) {
    std::printf("the Prismatic Beam: a request for a hit the cast has used up is spent (the session charges it no budget), a request for another hit or another cast is not\n");
    Forge m(f, 8, {});
    CHECKF(m.ok(), "the Mage and his world: %s %s", m.rig.problem.c_str(), m.w.problem.c_str());
    if (!m.ok()) return;
    const uint64_t wolf = m.wolf(300, 0);
    const abilities::AbilityDef* def = f.catalog->ability_named("Mage_PrismaticBeam");
    const abilities::HitDef* mana = f.catalog->hit_named("Mage_PrismaticBeam_Arcane_Mana");
    const abilities::HitDef* recurse = f.catalog->hit_named("Mage_PrismaticBeam_Recurse");
    CHECK(wolf != 0 && def && mana && recurse, "a Wolf 3 m away and the Beam's Arcane Mana and Recurse hits");
    if (!wolf || !def || !mana || !recurse) return;
    m.rig.know({"Mage_PrismaticBeam"});
    m.rig.asks = false;
    const double t0 = 100.0;
    const auto spent = [&](const abilities::HitDef& hit, double at) { return m.rig.pipe->hit_request_spent(def->row.guid, hit.row.guid, at); };
    CHECK(!spent(*mana, t0), "before any cast no request is spent");
    CHECK(m.rig.trigger("Mage_PrismaticBeam", t0, false) == Refusal::None, "the Beam is cast");
    m.rig.run_to(t0 + 0.2);
    CHECK(!spent(*mana, t0 + 0.5) && !spent(*recurse, t0 + 0.5), "its first requests are not spent");
    CHECK(mage_fixes::ask_tick(m.rig, *def, *mana, t0 + 0.5, wolf, true) != AbilityPipeline::HitVerdict::TooMany, "the first request of the Arcane Mana is read");
    CHECK(spent(*mana, t0 + 0.516), "the next one, a frame later, is spent (it comes within the gap)");
    CHECK(!spent(*recurse, t0 + 0.516), "the Recurse tick is not spent by it");
    CHECK(!spent(*mana, t0 + 0.7), "once the gap has passed the Arcane Mana is asked again for real");
    // the flood: a request every 16 ms for 3 s; the ones that are not spent are the ones the pipeline reads, never more than the cap
    int fresh = 0;
    for (int i = 0; i < 190; ++i) {
        const double at = t0 + 1.0 + 0.016 * static_cast<double>(i);
        if (spent(*mana, at)) continue;
        ++fresh;
        (void)mage_fixes::ask_tick(m.rig, *def, *mana, at, wolf, true);
    }
    CHECKF(fresh <= AbilityPipeline::kMaxRequestsPerHit, "of 190 requests in 3 s the budget would be charged for %d (at most %d)", fresh, AbilityPipeline::kMaxRequestsPerHit);
    CHECK(spent(*mana, t0 + 4.5), "past its cap every further request of the hit is spent");
    CHECK(!spent(*recurse, t0 + 4.5), "and the Recurse tick of the same cast is still a request that costs budget and is read");
    // what is no cast of the ability, or no hit of it, is the pipeline's to answer and costs budget
    CHECK(!m.rig.pipe->hit_request_spent(def->row.guid, 12345, t0 + 4.5) && !m.rig.pipe->hit_request_spent(12345, mana->row.guid, t0 + 4.5), "a hit that is none of the ability's and an ability that was not cast are not spent");
    CHECK(!spent(*mana, t0 + 40.0), "after the cast's window nothing is spent");
}
AOC_REGISTER_TEST(test_mage_review_a_request_the_cast_has_used_up_is_spent);
