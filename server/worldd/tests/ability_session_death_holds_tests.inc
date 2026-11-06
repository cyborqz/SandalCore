// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/ability_session_death_holds_tests.inc: a player's death in a real session takes what he put on the creatures off them
//  (registered with AOC_REGISTER_TEST: test_worldd_ability_session.cpp runs it through aoc_tests::run; it is included from ability_session_registered.inc)
// ============================================================================

void test_player_death_ends_holds_on_creatures() {
    std::printf("a player's death takes a root he put on a creature off it in the step he dies in, not at the next tick of the root\n");
    const svc::Character mage = class25(8, "Mage", 17750);
    WolfWorld ww;
    Run r(0, true, &mage, [&](PlayerWorld& w) { ww.prepare(w); });
    CHECK(ww.ground && ww.wolf != 0 && r.h.fx.world->combat() != nullptr, "a world with ground and a Wolf next to the player");
    if (!ww.ground || ww.wolf == 0 || !r.h.fx.world->combat()) return;
    r.play(0.5, false);
    combat::CombatWorld& cw = *r.h.fx.world->combat();
    const abilities::Catalog* cat = r.h.fx.world->ability_catalog();
    const abilities::EffectDef* rooted = cat ? cat->effect_named("Status_Rooted") : nullptr;
    CHECK(rooted != nullptr, "the data has the root");
    if (!rooted) return;
    const uint64_t pawn = r.h.s->bootstrap().setup().guids.pawn.ObjectId;
    CHECK(cw.find_player(pawn) != nullptr, "the player is on the fights' side of the world");
    effects::Applied a;
    a.def = rooted;
    a.duration = 60.0;
    cw.apply_effects(ww.wolf, pawn, {a}, r.now());
    r.play(0.2, false);
    CHECKF(cw.effect_stacks(ww.wolf, rooted->row.guid) == 1, "his root is on the Wolf (%d stack)", cw.effect_stacks(ww.wolf, rooted->row.guid));
    r.h.s->take_damage(r.h.conn, 1.0e9, r.h.t);
    r.play(0.1, false);
    CHECKF(cw.effect_stacks(ww.wolf, rooted->row.guid) == 0, "he dies, and the root is off the Wolf at once (%d stack)", cw.effect_stacks(ww.wolf, rooted->row.guid));
}
AOC_REGISTER_TEST(test_player_death_ends_holds_on_creatures);
