// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/ability_session_pvp_rules_tests.inc: PvP's pure rules (combat/pvp.h): who may harm and support whom as the client's GetCombatPermissions has them,
//  what flags the actor, the alignment timer's key as the client finds it, and the pawn's Alignments for each flag; corruption's gain, burn-off and cost
//  (combat/pvp_corruption.h) as GameStatProfile's formulas and the CorruptionTier curves have them.
//  (registered with AOC_REGISTER_TEST: test_worldd_ability_session.cpp runs it through aoc_tests::run; it is included from ability_session_registered.inc)
// ============================================================================
#include "combat/pvp.h"
#include "combat/pvp_corruption.h"

#include <bit>

namespace pvp_rules_tests {
namespace pvp = aoc::worldd::combat::pvp;
pvp::Side side(uint64_t actor, pvp::Flag f) {
    pvp::Side s;
    s.actor = actor;
    s.flag = f;
    return s;
}
}  // namespace pvp_rules_tests

static void pvp_rules_timer_keys() {
    using namespace pvp_rules_tests;
    std::printf("the removal timer's key, as the client finds it (UAlignmentComponent FindTimer 0x145e72d10)\n");
    CHECKF(pvp::timer_key(pvp::kCombatant) == 1905362248u, "Combatant with no instance: 1905362248 (%u)", pvp::timer_key(pvp::kCombatant));
    CHECKF(pvp::timer_key(pvp::kCorrupted) == 3685380188u, "Corrupted: 3685380188 (%u)", pvp::timer_key(pvp::kCorrupted));
    CHECKF(pvp::timer_key(pvp::kNonCombatant) == 2460361991u, "NonCombatant: 2460361991 (%u)", pvp::timer_key(pvp::kNonCombatant));
    CHECKF(pvp::timer_key(pvp::kDuel) == 3847377649u, "Duel: 3847377649 (%u)", pvp::timer_key(pvp::kDuel));
    CHECK(pvp::timer_key(pvp::kDuel, {1, 2, 3, 4}) != pvp::timer_key(pvp::kDuel), "a duel's instance changes the key");
}
AOC_REGISTER_TEST(pvp_rules_timer_keys);

static void pvp_rules_permissions() {
    using namespace pvp_rules_tests;
    std::printf("who may harm and support whom (ABaseCharacter::GetCombatPermissions 0x146b3f0e0)\n");
    const pvp::Pair none, party{1};
    const pvp::Side a = side(1, pvp::Flag::NonCombatant);
    const pvp::Side nc = side(2, pvp::Flag::NonCombatant), cb = side(3, pvp::Flag::Combatant), cr = side(4, pvp::Flag::Corrupted);
    CHECK(pvp::permissions(a, nc, none, false) == pvp::kHelp, "a non-combatant without the force flag: support only (harming him would flag)");
    CHECK(pvp::permissions(a, nc, none, true) == (pvp::kHarm | pvp::kHelp), "... with the force flag: harm and support (Neutral_Player)");
    CHECK(pvp::permissions(a, cb, none, false) == (pvp::kHarm | pvp::kHelp), "a combatant without the force flag: harm and support (both given back)");
    CHECK(pvp::permissions(a, cr, none, false) == pvp::kHarm, "a corrupted player without the force flag: harm only (supporting him would flag, and is not given back)");
    CHECK(pvp::permissions(a, cr, none, true) == (pvp::kHarm | pvp::kHelp), "... with the force flag: both");
    pvp::Side careful = a;
    careful.require_force_to_harm_combatants = true;
    CHECK(pvp::permissions(careful, cb, none, false) == pvp::kHelp, "bRequireForceFlagToHarmCombatants: a combatant needs the force flag to be harmed");
    CHECK(pvp::permissions(careful, cr, none, false) == pvp::kHarm, "... a corrupted one does not");
    careful.require_force_to_support_unaffiliated = true;
    CHECK(pvp::permissions(careful, cb, none, false) == 0, "bRequireForceFlagToSupportUnaffiliatedCombatants: nor supported");
    CHECK(pvp::permissions(careful, cb, none, true) == (pvp::kHarm | pvp::kHelp), "... with the force flag, both");
    CHECK(pvp::permissions(a, cb, party, false) == pvp::kHelp && pvp::permissions(a, cb, party, true) == pvp::kHelp, "a party member: support only, force flag or not");
    pvp::Side g1 = a, g2 = cb;
    g1.guild = g2.guild = {7, 7, 7, 7};
    CHECK(pvp::permissions(g1, g2, none, true) == pvp::kHelp, "a guild mate: support only (Friendly_Guild)");
    pvp::Side d1 = a, d2 = nc;
    d1.duel = d2.duel = 99;
    CHECK(pvp::permissions(d1, d2, none, false) == pvp::kHarm, "the same duel: harm only, without the force flag (Duel -> Duel: Hostile)");
    pvp::Side d3 = nc;
    d3.duel = 98;
    CHECK(pvp::permissions(d1, d3, none, false) == pvp::kHelp, "another duel is no duel of his");
    CHECK(pvp::permissions(a, a, none, true) == pvp::kHelp, "himself: support only");
    pvp::Side safe = nc;
    safe.suppressed = true;
    CHECK(pvp::permissions(a, safe, none, true) == pvp::kHelp, "in a PvP suppression area: support only");
    pvp::Side prot = nc;
    prot.protection = true;
    CHECK(pvp::permissions(a, prot, none, true) == pvp::kHelp, "PvP opt-in protection: no harm, even with the force flag");
    prot.duel = d1.duel;
    CHECK(pvp::permissions(d1, prot, none, false) == pvp::kHarm, "... but in a duel with him");
}
AOC_REGISTER_TEST(pvp_rules_permissions);

static void pvp_rules_flagging() {
    using namespace pvp_rules_tests;
    std::printf("what flags the actor (InterServerHandleFlaggingOnHitInstigated)\n");
    const pvp::Pair none, party{1};
    const pvp::Side a = side(1, pvp::Flag::NonCombatant);
    const pvp::Side nc = side(2, pvp::Flag::NonCombatant), cb = side(3, pvp::Flag::Combatant), cr = side(4, pvp::Flag::Corrupted);
    CHECK(pvp::flags(a, nc, none, pvp::kAttack), "harming a non-combatant flags");
    CHECK(pvp::flags(a, cb, none, pvp::kAttack), "harming a combatant flags");
    CHECK(!pvp::flags(a, cr, none, pvp::kAttack), "harming a corrupted player does not");
    CHECK(!pvp::flags(a, nc, none, pvp::kSupport), "supporting a non-combatant does not");
    CHECK(pvp::flags(a, cb, none, pvp::kSupport), "supporting a combatant flags");
    CHECK(pvp::flags(a, cr, none, pvp::kSupport), "supporting a corrupted player flags");
    CHECK(pvp::flags(a, cb, party, pvp::kSupport), "supporting a combatant party member flags too (PriorityAgnosticFlaggingActions)");
    CHECK(!pvp::flags(a, nc, party, pvp::kSupport), "supporting a non-combatant party member does not");
    pvp::Side d1 = a, d2 = cb;
    d1.duel = d2.duel = 5;
    CHECK(!pvp::flags(d1, d2, none, pvp::kAttack), "harming a duel partner does not flag (Duel FlaggingActions 0)");
    const pvp::Side bad = side(9, pvp::Flag::Corrupted);
    CHECK(!pvp::flags(bad, nc, none, pvp::kAttack), "a corrupted actor is never made a combatant");
}
AOC_REGISTER_TEST(pvp_rules_flagging);

static void pvp_rules_container() {
    using namespace pvp_rules_tests;
    std::printf("the pawn's Alignments for each flag\n");
    pvp::Alignments al;
    al.self = {11, 22, 33, 44};
    const auto plain = pvp::container(al);
    CHECK(plain.instances.size() == 4 && plain.timers.empty(), "a non-combatant: four alignments, no timer (the retail capture's pkt#107)");
    CHECK(plain.instances[0].key == static_cast<uint64_t>(pvp::kPlayer) && plain.instances[1].key == static_cast<uint64_t>(pvp::kNonCombatant) &&
              plain.instances[2].key == static_cast<uint64_t>(pvp::kInstigator) && plain.instances[3].key == static_cast<uint64_t>(pvp::kPvpSuppressedPerceiver),
          "Player, NonCombatant, Instigator, PVPSuppressed_Perceiver, in that order");
    CHECK(plain.instances[2].guids.size() == 1 && plain.instances[2].guids[0].a == 11 && plain.instances[2].guids[0].d == 44, "Instigator holds his own guid");
    al.flag = pvp::Flag::Combatant;
    al.combatant_since = 13468284.5;
    const auto purple = pvp::container(al);
    CHECK(purple.instances[1].key == static_cast<uint64_t>(pvp::kCombatant), "a combatant: Combatant in NonCombatant's place");
    CHECK(purple.timers.size() == 1 && purple.timers[0].key == 1905362248u && purple.timers[0].alignment_id_a == static_cast<uint64_t>(pvp::kCombatant) &&
              purple.timers[0].alignment_id_b == static_cast<uint64_t>(pvp::kAlignmentTypeId),
          "... with its removal timer (key 1905362248, AlignmentId {Combatant, its TypeId})");
    CHECK(std::bit_cast<double>(purple.timers[0].set_time_stamp) == 13468284.5, "... set when the flag was (the synchronized clock, a double)");
    al.flag = pvp::Flag::Corrupted;
    const auto red = pvp::container(al);
    CHECK(red.instances[1].key == static_cast<uint64_t>(pvp::kCorrupted) && red.timers.empty(), "corrupted: Corrupted, no timer (RemovalTimer 0)");
    al.duel = {5, 6, 7, 8};
    const auto duel = pvp::container(al);
    CHECK(duel.instances.size() == 5 && duel.instances[4].key == static_cast<uint64_t>(pvp::kDuel) && duel.instances[4].guids[0].a == 5, "a duel adds Duel {the duel's guid}");
}
AOC_REGISTER_TEST(pvp_rules_container);

static void pvp_rules_tiers() {
    using namespace pvp_rules_tests;
    std::printf("the corruption tiers (Corruption->CorruptionTier)\n");
    CHECK(pvp::corruption_tier(0) == 0 && pvp::corruption_tier(0.0005) == 0, "none at 0");
    CHECK(pvp::corruption_tier(0.5) == 1 && pvp::corruption_tier(2.99) == 1, "1 above 0");
    CHECK(pvp::corruption_tier(3) == 2 && pvp::corruption_tier(6) == 3 && pvp::corruption_tier(12) == 4 && pvp::corruption_tier(24) == 5 && pvp::corruption_tier(500) == 5,
          "2 at 3, 3 at 6, 4 at 12, 5 at 24 and above");
}

static bool pvp_near(double a, double b) { return std::fabs(a - b) < 1e-9; }
AOC_REGISTER_TEST(pvp_rules_tiers);

static void pvp_rules_corruption() {
    using namespace pvp_rules_tests;
    std::printf("corruption: a player kill, a death, a creature's kill (GameStatProfile)\n");
    CHECK(pvp_near(pvp::corruption_for_kill(30, 30, 0), 1), "a kill of a player of his level: 1");
    CHECK(pvp_near(pvp::corruption_for_kill(30, 26, 0), 1), "... 4 levels below him: still 1");
    CHECKF(pvp_near(pvp::corruption_for_kill(30, 25, 0), 2), "... 5 below: 1 + 0.2 x 5 = 2 (%.3f)", pvp::corruption_for_kill(30, 25, 0));
    CHECK(pvp_near(pvp::corruption_for_kill(25, 30, 0), 1), "... above him: 1");
    CHECKF(pvp_near(pvp::corruption_for_kill(30, 30, 3), 2.5), "with 3 blight: 1 + 3 / 2 = 2.5 (%.3f)", pvp::corruption_for_kill(30, 30, 3));
    CHECK(pvp_near(pvp::blight_for_kill(30, 30), 1) && pvp_near(pvp::blight_for_kill(30, 20), 2), "blight: 1, or 1 + 0.1 x the levels above him at 5 or more");
    CHECK(pvp_near(pvp::corruption_lost_dying(0), 0), "a death without corruption takes none");
    CHECK(pvp_near(pvp::corruption_lost_dying(10), 5), "a death at 10: half (5)");
    CHECK(pvp_near(pvp::corruption_lost_dying(3.5), 2), "a death at 3.5: at least 2");
    CHECK(pvp_near(pvp::corruption_lost_dying(2.5), 2.5), "a death at 2.5: all of it (less than 1 would remain)");
    CHECK(pvp_near(pvp::corruption_lost_dying(1.5), 1.5), "a death at 1.5: all of it");
    CHECK(pvp_near(pvp::base_kill_xp(1), 50) && pvp_near(pvp::base_kill_xp(10), 550), "Base_Kill_XP: 50 at level 1, 50 x (level + 1) above");
    CHECKF(pvp_near(pvp::corruption_lost_killing(550, 10), 1.0 / 40), "a regular kill of his level burns 1/40 (%.4f)", pvp::corruption_lost_killing(550, 10));
    CHECK(pvp_near(pvp::corruption_lost_killing(550 * 80, 10), 1), "... a huge one at most 1");
    CHECK(pvp_near(pvp::corruption_lost_killing(0, 10), 0), "... none for no experience");
}
AOC_REGISTER_TEST(pvp_rules_corruption);

static void pvp_rules_corruption_tiers() {
    using namespace pvp_rules_tests;
    std::printf("what a tier costs (CorruptionTier curves)\n");
    CHECK(pvp_near(pvp::xp_debt_mult(0), 1) && pvp_near(pvp::xp_debt_mult(1), 2) && pvp_near(pvp::xp_debt_mult(3), 7) && pvp_near(pvp::xp_debt_mult(5), 15),
          "experience debt x1 x2 x4 x7 x10 x15");
    CHECK(pvp_near(pvp::resource_drop_add(0), 0) && pvp_near(pvp::resource_drop_add(2), 0.35) && pvp_near(pvp::resource_drop_add(5), 1), "resources dropped +0 .. +1");
    CHECK(pvp::item_drop_min(0) == 0 && pvp::item_drop_max(0) == 0, "tier 0 drops no worn item");
    CHECK(pvp::item_drop_min(1) == 1 && pvp::item_drop_max(1) == 3 && pvp::item_drop_min(5) == 3 && pvp::item_drop_max(5) == 6, "1..3 at tier 1, 3..6 at tier 5");
    CHECK(pvp::tier_effect(0) == 0 && pvp::tier_effect(1) == 6064630485691858944ll && pvp::tier_effect(5) == 6064630697907585228ll,
          "the debuff: none, Corruption_1 .. Corruption_5");
    CHECK(pvp::xp_debt_mult(9) == pvp::xp_debt_mult(5) && pvp::tier_effect(-1) == 0, "a tier out of range is clamped");
}
AOC_REGISTER_TEST(pvp_rules_corruption_tiers);
