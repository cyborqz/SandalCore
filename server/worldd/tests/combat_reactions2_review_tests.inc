// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_reactions2_review_tests.inc: reactions part 2, after the review
//
//    Epic            the Resonance the data spends for a finisher's Epic is spent once however many allies the melody reaches
//    the party       Cheerful and Pensive favour the Bard himself and the members of his party over a stranger who is hurt more
//    being hit       a Tank's reflect that lands on another player waits for that player's own tick instead of running inside the attacker's cast, and a hit
//                    that lands on the rider's mount is seen by the rider's own effects through the pipeline
//  (registered with AOC_REGISTER_TEST: test_worldd_combat.cpp runs it through aoc_tests::run; it is included from combat_registered.inc)
// ============================================================================

namespace reactions2_review {

using reactions2::confirms_of;
using reactions2::Duo;
using reactions2::grant;
using reactions2::Melodies;
using reactions2::near;
using reactions2::stacks_of;
using reactions2::wake;

/// One finishing hit of the 1H axe combo at several targets: the cast, the client's request naming all of them, and what the rig saw.
bool finish_axe_at(Rig& rig, int32_t& id, double t, const std::vector<uint64_t>& targets) {
    const abilities::AbilityDef* combo = rig.catalog ? rig.catalog->ability_named("WeaponCombo_Melee_1H_Axe") : nullptr;
    const abilities::HitDef* finisher = rig.catalog ? rig.catalog->hit_named("WeaponCombo_Axe_1H_Hit4+") : nullptr;
    if (!combo || !finisher || targets.empty()) return false;
    int64_t targeting = content::kTargetingDefaultSelf;
    for (const content::AbilityHitRow& h : combo->hits) {
        if (h.hit_guid != finisher->row.guid || h.source != "hit") continue;
        for (const content::TargetingRow& tg : combo->targetings) {
            if (tg.key == h.key) targeting = tg.guid;
        }
    }
    rig.clock = t;
    TriggerInfo info;
    info.animation_index = 0;
    info.target_count = static_cast<int>(targets.size());
    info.target_actors = targets;
    const bool accepted = rig.pipe->trigger(combo->row.guid, id++, t, info) == Refusal::None;
    rig.run_to(t + 0.4);
    rig.pipe->hit_request(combo->row.guid, targeting, finisher->row.guid, 1, 0, t + 0.8, targets);
    rig.run_to(t + 1.8);
    return accepted;
}

// Epic reaches up to Clamp(Resonance,1,3) allies, and the Cost1 hit the data hangs on that hit takes one Resonance for every ally it reached: two allies
// under crowd control spend two.
void test_reactions2_review_epic_spends_for_every_ally_it_reaches(Fixture& f) {
    std::printf("Epic at a finisher with two allies under crowd control: both are cleansed and the one Resonance of the data is spent for each of them\n");
    Melodies m;
    m.look_up(f);
    if (!m.ok()) return;
    Duo d(f);
    CHECKF(d.ok(), "the world: %s%s", d.a.problem.c_str(), d.w.problem.c_str());
    if (!d.ok()) return;
    Rig c(2, 25, {}, true, 103);
    combat_rig::FakePlayer pc(c, 79, {600, 0, 1098});
    combat::WorldHitPort portc(d.world, d.w.registry, pc.fighter());
    if (!c.ok()) return;
    d.world.add_player(&pc);
    c.pipe->set_combat(&portc);
    const uint64_t wolf = d.wolf(1200, 0);
    if (wolf == 0) return;
    d.a.know({"WeaponCombo_Melee_1H_Axe"});
    const double t0 = 100.0;
    CHECK(grant(d.a, m.epic_self, 1, t0 - 0.5) && grant(d.a, m.resonance, 3, t0 - 0.5), "(Epic with three Resonance)");
    CHECK(grant(d.a, m.pep, 1, t0 - 0.5), "(the Bard holds Pep, so the targeting record leaves him out)");
    CHECK(grant(d.b, m.rooted, 1, t0 - 0.5) && grant(c, m.rooted, 1, t0 - 0.5), "(two allies are rooted)");
    int32_t id = 500;
    reactions2::finish_axe(d.a, id, t0, wolf);
    CHECKF(stacks_of(d.b, m.rooted) == 0 && stacks_of(c, m.rooted) == 0, "both allies are cleansed (%d, %d)", stacks_of(d.b, m.rooted), stacks_of(c, m.rooted));
    CHECKF(d.b.pawn.effect(m.pep) != nullptr && c.pawn.effect(m.pep) != nullptr, "both hold Pep");
    CHECKF(stacks_of(d.a, m.resonance) == 1, "the data's Cost1 spent one Resonance for each ally it reached (3 -> %d)", stacks_of(d.a, m.resonance));
    d.world.remove_player(&pc);
    CHECK(d.a.pawn.broken.empty() && d.b.pawn.broken.empty() && c.pawn.broken.empty(), "(the pawns took every change)");
}

// "Favors yourself and party members": the targeting record's score gives +1000 to an injured member of the caster's party (or the caster himself) and none to
// a stranger, so a stranger who is hurt more does not come before a member.
void test_reactions2_review_cheerful_favours_the_party(Fixture& f) {
    std::printf("Cheerful heals an injured party member before a stranger who is hurt more, and the Bard himself before a stranger too\n");
    Melodies m;
    m.look_up(f);
    if (!m.ok()) return;
    Duo d(f);
    CHECKF(d.ok(), "the world: %s%s", d.a.problem.c_str(), d.w.problem.c_str());
    if (!d.ok()) return;
    Rig c(2, 25, {}, true, 103);
    combat_rig::FakePlayer pc(c, 79, {600, 0, 1098});
    combat::WorldHitPort portc(d.world, d.w.registry, pc.fighter());
    if (!c.ok()) return;
    d.world.add_player(&pc);
    c.pipe->set_combat(&portc);
    d.pb.party_with = d.pa.actor_id();   // the second player is of the Bard's party; the third is a stranger
    const uint64_t wolf = d.wolf(1200, 0);
    if (wolf == 0) return;
    d.a.know({"WeaponCombo_Melee_1H_Axe"});
    int32_t id = 500;
    // a party member at 60 % and a stranger at 20 %
    double t0 = 100.0;
    CHECK(grant(d.a, m.cheerful_self, 1, t0 - 0.5) && grant(d.a, m.resonance, 1, t0 - 0.5), "(Cheerful with one Resonance)");
    d.b.set_current(content::kGuidHealth, d.b.max_health() * 0.6);
    c.set_current(content::kGuidHealth, c.max_health() * 0.2);
    const double member = d.b.health(), stranger = c.health();
    reactions2::finish_axe(d.a, id, t0, wolf);
    CHECKF(d.b.health() > member + 1.0, "the party member was healed (%.1f over %.1f)", d.b.health(), member);
    CHECKF(near(c.health(), stranger, 1e-3), "the stranger who is hurt more was not (%.1f of %.1f)", c.health(), stranger);
    // the Bard himself at 50 % and the stranger at 20 %: he heals himself
    t0 = 130.0;
    d.b.set_current(content::kGuidHealth, d.b.max_health());
    d.a.set_current(content::kGuidHealth, d.a.max_health() * 0.5);
    c.set_current(content::kGuidHealth, c.max_health() * 0.2);
    const double own = d.a.health();
    CHECK(grant(d.a, m.cheerful_self, 1, t0 - 0.5) && grant(d.a, m.resonance, 1, t0 - 0.5), "(Cheerful with one Resonance again)");
    reactions2::finish_axe(d.a, id, t0, wolf);
    CHECKF(d.a.health() > own + 1.0, "the Bard healed himself (%.1f over %.1f)", d.a.health(), own);
    CHECKF(near(c.health(), stranger, 1e-3), "the stranger was left alone again (%.1f)", c.health());
    d.world.remove_player(&pc);
    CHECK(d.a.pawn.broken.empty() && d.b.pawn.broken.empty() && c.pawn.broken.empty(), "(the pawns took every change)");
}

// The targeting record's two sorting entries are tried in order: every injured ally comes before a whole one, whatever the two scores' scales (a stranger who
// is hurt a little scores below a whole ally standing at the Bard's feet, and still comes first).
void test_reactions2_review_an_injured_stranger_comes_before_a_whole_bard(Fixture& f) {
    std::printf("Cheerful heals an injured stranger rather than the whole Bard and the whole party member\n");
    Melodies m;
    m.look_up(f);
    if (!m.ok()) return;
    Duo d(f);
    CHECKF(d.ok(), "the world: %s%s", d.a.problem.c_str(), d.w.problem.c_str());
    if (!d.ok()) return;
    Rig c(2, 25, {}, true, 103);
    combat_rig::FakePlayer pc(c, 79, {600, 0, 1098});
    combat::WorldHitPort portc(d.world, d.w.registry, pc.fighter());
    if (!c.ok()) return;
    d.world.add_player(&pc);
    c.pipe->set_combat(&portc);
    d.pb.party_with = d.pa.actor_id();
    const uint64_t wolf = d.wolf(1200, 0);
    if (wolf == 0) return;
    d.a.know({"WeaponCombo_Melee_1H_Axe"});
    const double t0 = 100.0;
    CHECK(grant(d.a, m.cheerful_self, 1, t0 - 0.5) && grant(d.a, m.resonance, 1, t0 - 0.5), "(Cheerful with one Resonance)");
    c.set_current(content::kGuidHealth, c.max_health() * 0.8);   // a little hurt, a stranger
    const double before = c.health();
    int32_t id = 500;
    const size_t from = d.a.log.size();
    reactions2::finish_axe(d.a, id, t0, wolf);
    const auto heals = confirms_of(d.a, m.cheerful_hit, from);
    CHECKF(heals.size() == 1 && heals[0].targets.size() == 1 && heals[0].targets[0].component.ObjectId == pc.actor_id(), "the one heal went to the injured stranger (%zu)", heals.size());
    CHECKF(c.health() > before + 1.0, "his Health rose (%.1f over %.1f)", c.health(), before);
    d.world.remove_player(&pc);
    CHECK(d.a.pawn.broken.empty() && d.b.pawn.broken.empty() && c.pawn.broken.empty(), "(the pawns took every change)");
}

// A hit that lands on another player runs that player's effects' reactions between his own steps (the contract of the Bard's Saga): under the PvP rules a
// harmful hit between players waits in the target's session inbox, so a Tank hit by a player whose cast is still under way reflects at that player when the
// Tank's own step applies the hit, and the cast goes on and reaches its other target.
void test_reactions2_review_a_reflect_waits_for_the_tanks_own_tick(Fixture& f) {
    std::printf("a player's cast hits a Tank and a Wolf: the Tank's reflect lands on him on the Tank's own tick and the cast reaches the Wolf\n");
    const abilities::EffectDef* reflect = f.catalog->effect_named("Tank_Reflect");
    const abilities::HitDef* reflect_hit = f.catalog->hit_named("Tank_Deflect_Reflect_Hit");
    CHECK(reflect && reflect_hit, "the Reflect effect and its hit are in the data");
    if (!reflect || !reflect_hit) return;
    Duo d(f, 5, 7, {}, {});
    CHECKF(d.ok(), "the two players and the world: %s%s", d.a.problem.c_str(), d.w.problem.c_str());
    if (!d.ok()) return;
    d.pa.pvp_rules = true;           // the PvP rules judge the cast's hit on the Tank
    d.pb.pvp_rules = true;
    d.pa.pvp_force = true;           // the caster holds the PvP key: his harmful hit on a non-combatant is allowed and flags him
    d.pb.pipeline_hits = true;       // his session applies what reaches his inbox through the pipeline (PlayerSession::take_hit)
    d.pb.queue_player_hits = true;   // ... in his own step, not inside the attacker's cast (PlayerSession::CombatSide)
    const uint64_t wolf = d.wolf(300, 200);
    if (wolf == 0) return;
    d.a.know({"WeaponCombo_Melee_1H_Axe"});
    const double t0 = 100.0;
    CHECK(grant(d.b, reflect->row.guid, 1, t0 - 0.5), "(the Tank holds the Reflect)");
    wake(d.world, wolf, d.pa.actor_id(), t0 - 1.0);
    const double attacker_before = d.a.health(), tank_before = d.b.health(), wolf_before = d.world.health_of(wolf);
    int32_t id = 500;
    CHECK(finish_axe_at(d.a, id, t0, {d.pb.actor_id(), wolf}), "the finishing hit was accepted");
    const double tank_after_hit = d.b.health();   // (the harmful hit waits in his inbox until his own step)
    const size_t reflects_before = confirms_of(d.b, reflect_hit->row.guid, 0).size();
    CHECKF(reflects_before == 0, "the Tank's runtime has not reflected yet: the hit waits in his inbox for his own step (%zu)", reflects_before);
    CHECKF(near(tank_after_hit, tank_before, 1e-3), "the hit has not taken its Health yet: it waits for his own step (%.1f of %.1f)", tank_after_hit, tank_before);
    // the Tank's own step applies the hit and brings the reflect
    d.b.run_to(t0 + 1.9);
    d.pb.take_incoming(t0 + 1.9);
    d.a.take();
    CHECKF(d.b.health() < tank_before - 1.0, "the attacker's hit took Health from the Tank on his own step (%.1f of %.1f)", d.b.health(), tank_before);
    CHECKF(d.world.health_of(wolf) < wolf_before - 1.0, "the cast went on to the Wolf (%.1f of %.1f)", d.world.health_of(wolf), wolf_before);
    CHECKF(d.a.health() < attacker_before - 0.5, "the Tank's reflect took its part from the attacker (%.1f of %.1f)", d.a.health(), attacker_before);
    CHECKF(!confirms_of(d.b, reflect_hit->row.guid, 0).empty(), "the reflect was confirmed by the Tank's runtime");
    CHECKF(d.pa.pvp_flag == combat::pvp::Flag::Combatant, "the caster who harmed the Tank is a combatant now: his first harmful action flags him");
    CHECKF(d.pb.pvp_flag == combat::pvp::Flag::Combatant, "the Tank's harmful reflect flags the Tank in turn (harming a combatant)");
    CHECK(d.a.pawn.broken.empty() && d.b.pawn.broken.empty(), "(both pawns took every change)");
}

// A hit that lands on the rider's mount: the numbers went to the mount, and the rider's own effects that listen to being hit still react.  (The session's
// call is one line that reaches this one; the mount rig has no ability tables, so the pipeline's half is what can be shown here.)
void test_reactions2_review_a_hit_on_the_mount_is_seen_by_the_riders_effects(Fixture& f) {
    std::printf("a hit that landed on the mount: the Tank's own Reflect still answers the attacker and his own Health stays\n");
    const abilities::EffectDef* reflect = f.catalog->effect_named("Tank_Reflect");
    const abilities::HitDef* swipe = f.catalog->hit_named("Deer_HeadSwipe_Hit");
    const abilities::HitDef* reflect_hit = f.catalog->hit_named("Tank_Deflect_Reflect_Hit");
    CHECK(reflect && swipe && reflect_hit, "the Reflect effect and the hits are in the data");
    if (!reflect || !swipe || !reflect_hit) return;
    Forge g(f, 7, {});
    CHECKF(g.ok(), "the Tank and the world: %s%s", g.rig.problem.c_str(), g.w.problem.c_str());
    if (!g.ok()) return;
    const uint64_t wolf = g.wolf(600, 0);
    if (wolf == 0) return;
    const double t0 = 100.0;
    CHECK(grant(g.rig, reflect->row.guid, 1, t0 - 0.5), "(the Tank holds the Reflect)");
    wake(g.world, wolf, g.player.actor_id(), t0 - 1.0);
    const double own = g.rig.health(), wolf_before = g.world.health_of(wolf);
    combat::IncomingHit in;
    in.instigator = wolf;
    in.hit_guid = swipe->row.guid;
    in.source_type = content::kSourceTypeAbility;
    in.flags = 0;
    in.mods.push_back({1, content::kGuidHealth, -30.0});
    in.at = t0;
    const size_t from = g.rig.log.size();
    g.rig.pipe->raise_hit_taken(in, t0);
    g.rig.take();
    CHECKF(near(g.rig.health(), own, 1e-3), "the rider's own Health stays: the numbers went to the mount (%.1f of %.1f)", g.rig.health(), own);
    CHECKF(g.world.health_of(wolf) < wolf_before - 1.0, "the Wolf took the reflect (%.1f of %.1f)", g.world.health_of(wolf), wolf_before);
    const auto reflects = confirms_of(g.rig, reflect_hit->row.guid, from);
    CHECKF(reflects.size() == 1 && reflects[0].source_type == content::kSourceTypeReaction, "the reflect was confirmed as the reaction's (%zu)", reflects.size());
    CHECK(g.rig.pawn.broken.empty(), "(the pawn took every change)");
}

}  // namespace reactions2_review

using reactions2_review::test_reactions2_review_a_hit_on_the_mount_is_seen_by_the_riders_effects;
using reactions2_review::test_reactions2_review_a_reflect_waits_for_the_tanks_own_tick;
using reactions2_review::test_reactions2_review_an_injured_stranger_comes_before_a_whole_bard;
using reactions2_review::test_reactions2_review_cheerful_favours_the_party;
using reactions2_review::test_reactions2_review_epic_spends_for_every_ally_it_reaches;

AOC_REGISTER_TEST(test_reactions2_review_epic_spends_for_every_ally_it_reaches);
AOC_REGISTER_TEST(test_reactions2_review_cheerful_favours_the_party);
AOC_REGISTER_TEST(test_reactions2_review_an_injured_stranger_comes_before_a_whole_bard);
AOC_REGISTER_TEST(test_reactions2_review_a_hit_on_the_mount_is_seen_by_the_riders_effects);
AOC_REGISTER_TEST(test_reactions2_review_a_reflect_waits_for_the_tanks_own_tick);
