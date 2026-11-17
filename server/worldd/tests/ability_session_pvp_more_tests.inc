// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/ability_session_pvp_more_tests.inc: what the first PvP tests leave out, on the same rigs (ability_session_pvp_tests.inc, ability_session_players_tests.inc,
//  ability_session_duels_tests.inc, which this file is included after)
//
//    another player's bleed     its ticks are that player's hits: they keep the force flag the bleed was put on with (not the last cast's), flag him, and take Health
//    a cleanse                  a hit that takes effects off another player does it through his own session
//    the pawn's Alignments      the flag's and the duel's rows change, every other instance and timer stays
//    a duel's request           one request a second at most: a request and a forfeit loop makes the banner of the world's actors come and go for everyone around
//    a relog in a duel          the old session's late leave neither erases the new session's entry nor ends the duel
//  (registered with AOC_REGISTER_TEST: test_worldd_ability_session.cpp runs it through aoc_tests::run; it is included from ability_session_registered.inc)
// ============================================================================

// ---- the pawn's Alignments: the flag and the duel change, every other instance stays -----------------------------------------------------------------

static void pvp_rules_merge_keeps_what_the_pawn_holds() {
    using namespace pvp_rules_tests;
    std::printf("the pawn's Alignments: the flag's and the duel's rows change, an instance a later feature added stays\n");
    constexpr uint64_t kForeign = 6064629999999999999ull;   // (an alignment this code knows nothing of: a party's or a node's)
    const auto has = [](const aoc::protocol::repl::AlignmentContainer& c, int64_t alignment) {
        for (const auto& i : c.instances) {
            if (i.key == static_cast<uint64_t>(alignment)) return true;
        }
        return false;
    };
    const auto timers_of = [](const aoc::protocol::repl::AlignmentContainer& c, int64_t alignment) {
        size_t n = 0;
        for (const auto& t : c.timers) n += t.alignment_id_a == static_cast<uint64_t>(alignment) ? 1 : 0;
        return n;
    };
    pvp::Alignments al;
    al.self = {11, 22, 33, 44};
    aoc::protocol::repl::AlignmentContainer held = pvp::container(al);
    held.instances.push_back({kForeign, {aoc::protocol::repl::GuidValue{9, 9, 9, 9}}});
    aoc::protocol::repl::AlignmentContainer::Timer foreign_timer;
    foreign_timer.key = 4242;
    foreign_timer.alignment_id_a = kForeign;
    held.timers.push_back(foreign_timer);

    al.flag = pvp::Flag::Combatant;
    al.combatant_since = 100.0;
    const auto purple = pvp::merge_container(held, pvp::container(al));
    CHECK(has(purple, pvp::kCombatant) && !has(purple, pvp::kNonCombatant), "a combatant: Combatant takes NonCombatant's place");
    CHECK(has(purple, static_cast<int64_t>(kForeign)) && purple.instances.size() == held.instances.size(), "... and the instance a later feature added is still there (nothing is dropped)");
    CHECK(timers_of(purple, pvp::kCombatant) == 1 && timers_of(purple, static_cast<int64_t>(kForeign)) == 1, "... with the Combatant's timer, and the other feature's timer stays");
    CHECK(purple.instances.size() >= 2 && purple.instances[0].key == static_cast<uint64_t>(pvp::kPlayer) && purple.instances[1].key == static_cast<uint64_t>(pvp::kCombatant),
          "the flag's instance sits where the old one did, after Player");

    al.duel = {5, 6, 7, 8};
    al.duel_since = 120.0;
    const auto dueling = pvp::merge_container(purple, pvp::container(al));
    CHECK(has(dueling, pvp::kDuel) && has(dueling, static_cast<int64_t>(kForeign)) && timers_of(dueling, pvp::kDuel) == 1 && timers_of(dueling, pvp::kCombatant) == 1,
          "a duel adds its instance and its timer; the flag's timer and the other feature's rows stay");

    pvp::Alignments after;
    after.self = al.self;
    const auto over = pvp::merge_container(dueling, pvp::container(after));
    CHECK(!has(over, pvp::kDuel) && !has(over, pvp::kCombatant) && has(over, pvp::kNonCombatant) && timers_of(over, pvp::kDuel) == 0 && timers_of(over, pvp::kCombatant) == 0,
          "the duel's end and the flag's end take their rows and timers away: white again, no duel");
    CHECK(has(over, static_cast<int64_t>(kForeign)) && timers_of(over, static_cast<int64_t>(kForeign)) == 1, "... and the other feature's instance and timer are still there");
}
AOC_REGISTER_TEST(pvp_rules_merge_keeps_what_the_pawn_holds);

// ---- a duel's request: one a second --------------------------------------------------------------------------------------------------------------

static void duels_a_request_loop_is_held_to_one_a_second() {
    using namespace duels_tests;
    std::printf("a request and a forfeit again and again makes one banner a second at most\n");
    DuelRig r;
    r.move(kDuelPawnB, 1000, 0);
    CHECK(r.book.request(kDuelA, "Beta", 10.0).empty() && r.banner() != nullptr, "the first request goes: its banner is in the world");
    CHECK(r.book.forfeit(kDuelA, 10.2) && r.banner() == nullptr, "he calls it off: the banner goes");
    CHECK(r.book.request(kDuelA, "Beta", 10.4) == "Wait a moment before asking again.", "a second request within a second is refused");
    CHECK(r.book.duels() == 0 && r.banner() == nullptr, "... and no banner is made for it");
    CHECK(r.book.request(kDuelA, "Beta", 11.1).empty() && r.banner() != nullptr, "a second after the first it goes again");
    const auto sent = DuelRig::last(r.book.take_notices(kDuelA), DuelNotice::Kind::Sent);
    CHECK(sent.has_value(), "(his client is told the request went)");
    // a refusal does not use the second up: only a request that made a banner does
    DuelRig q;
    q.move(kDuelPawnB, 1000, 0);
    CHECK(q.book.request(kDuelA, "Nobody", 20.0) == "You cannot duel that player." && q.book.request(kDuelA, "Beta", 20.1).empty(), "a request that is refused leaves the next one free");
}
AOC_REGISTER_TEST(duels_a_request_loop_is_held_to_one_a_second);

// ---- another player's bleed ----------------------------------------------------------------------------------------------------------------------

static void test_pvp_a_bleed_ticks_as_the_hit_of_the_player_who_left_it() {
    using namespace pvp_tests;
    namespace pvp = combat::pvp;
    std::printf("PvP: another player's bleed ticks as his hit, with the force flag it was put on with (not the last cast's): it takes Health and flags him %s\n", kAftermathNote);
    Duo d;
    const bool started = d.start();
    CHECK(started, "two players in one world with ground, both with the 1H axe combo");
    if (!started) return;
    Run& a = *d.a;
    Run& b = *d.b;
    PlayerWorld& world = *a.h.fx.world;
    const abilities::EffectDef* bleed = world.ability_catalog()->effect_named("Status_Bleeding");
    CHECK(bleed != nullptr && b.h.s->pipeline() != nullptr, "(the data's Status_Bleeding is in the tables)");
    if (!bleed || !b.h.s->pipeline()) return;
    const float full = b.m.stats[content::kGuidMaxHealth];
    CHECK(a.h.s->pvp_flag_state() == pvp::Flag::NonCombatant && b.h.s->pvp_flag_state() == pvp::Flag::NonCombatant, "both start as non-combatants");

    // the bleed is put on him with the PvP key held
    send_on_pc(a, pvp_key(true));
    d.both(0.2);
    effects::Applied put;
    put.def = bleed;
    put.duration = 12.0;
    world.combat()->apply_effects_to_player(d.sb.guids.pawn.ObjectId, d.sa.guids.pawn.ObjectId, {put}, a.now());
    d.both(0.3);
    CHECK(b.h.s->pipeline()->stacks_of(bleed->row.guid) == 1, "the bleed is on him");

    // the key is let go, and a cast without it is made: the last trigger's flag is off now
    send_on_pc(a, pvp_key(false));
    d.both(0.2);
    d.swing(a, d.sa, d.sb, false);

    // its ticks (each second) are still his hits, judged with the key the bleed was put on with
    d.both(4.0);
    CHECKF(b.m.stats[content::kGuidHealth] < full - 1.0f, "the bleed's ticks take Health from a non-combatant although the key is off now (%.0f of %.0f)", b.m.stats[content::kGuidHealth], full);
    CHECK(a.h.s->pvp_flag_state() == pvp::Flag::Combatant && a.h.s->pvp_flaggings() >= 1, "... and the player who left the bleed is a combatant: harming a non-combatant flags him");
    CHECK(a.h.s->in_pvp_combat() && b.h.s->in_pvp_combat(), "both are in PvP combat");
    CHECK(!a.h.s->abilities_failed() && !b.h.s->abilities_failed(), "(nothing failed)");
}
AOC_REGISTER_TEST(test_pvp_a_bleed_ticks_as_the_hit_of_the_player_who_left_it);

// ---- a cleanse on another player -----------------------------------------------------------------------------------------------------------------

static void test_players_a_cleanse_takes_the_bleed_off_another_player() {
    using namespace players_tests;
    std::printf("abilities between players: the Cleric's Purify hit takes another player's bleed off him, through his own session %s\n", kAftermathNote);
    Players p;
    const bool started = p.start(3);
    CHECK(started, "(three players in one world with ground and fights)");
    if (!started) return;
    PlayerWorld& world = p.world();
    combat::CombatWorld& cw = *world.combat();
    const bs::PlayerSetup sa = p.setup(0), sb = p.setup(1), sc = p.setup(2);
    Run& b = p[1];
    const abilities::EffectDef* bleed = world.ability_catalog()->effect_named("Status_Bleeding");
    const abilities::HitDef* purify = world.ability_catalog()->hit_named("Cleric_Purify");
    CHECK(bleed != nullptr && purify != nullptr && b.h.s->pipeline() != nullptr, "(Status_Bleeding and the Purify hit are in the tables)");
    if (!bleed || !purify || !b.h.s->pipeline()) return;
    // a bleed the stranger left on the patient
    effects::Applied put;
    put.def = bleed;
    put.duration = 30.0;
    cw.apply_effects_to_player(sb.guids.pawn.ObjectId, sc.guids.pawn.ObjectId, {put}, p[0].now());
    p.all(0.5);
    CHECK(b.h.s->pipeline()->stacks_of(bleed->row.guid) == 1, "the patient bleeds (the stranger's)");
    // the Cleric's Purify hit on him: its filter takes his debuffs off (a hit's removal list, or a cleanse's filter, runs on the target's session)
    cw.remove_effects_from_player(sb.guids.pawn.ObjectId, sa.guids.pawn.ObjectId, *purify, p[0].now());
    p.all(0.5);
    CHECK(b.h.s->pipeline()->stacks_of(bleed->row.guid) == 0, "the bleed is gone");
    CHECK(!b.m.has_effect(bleed->row.guid), "... and his client holds no bleed either");
    // a hit on a player who is gone, or dead, takes nothing and breaks nothing
    cw.remove_effects_from_player(0xdead0000ull, sa.guids.pawn.ObjectId, *purify, p[0].now());
    p.all(0.2);
    CHECK(!p[0].h.s->abilities_failed() && !b.h.s->abilities_failed() && !p[2].h.s->abilities_failed(), "(nothing failed)");
}
AOC_REGISTER_TEST(test_players_a_cleanse_takes_the_bleed_off_another_player);

// ---- a relog in a duel ---------------------------------------------------------------------------------------------------------------------------

static void test_pvp_a_relog_keeps_the_duel() {
    using namespace pvp_tests;
    std::printf("PvP: the player's new session joins before his old one has left (a relog): the old session's late leave does not erase the new entry nor end the duel %s\n", kAftermathNote);
    Duo d;
    const bool started = d.start();
    CHECK(started, "two players in one world with ground, both with the 1H axe combo");
    if (!started) return;
    Run& a = *d.a;
    PlayerWorld& world = *a.h.fx.world;
    const world::DuelBook* book = a.h.s->duel_book_for_test();
    CHECK(book != nullptr, "(the world's duel book)");
    if (!book) return;
    send_on_pawn(a, duel_request(d.rb.name));
    d.both(0.5);
    send_on_pawn(*d.b, duel_call("ServerAcceptDuel", d.sa.character_guid));
    d.both(5.5);
    CHECK(book->fighting_key(d.sa.character_guid) != 0, "the two fight a duel");
    const uint64_t old_pawn = d.sb.guids.pawn.ObjectId;
    // the same character joins again; his old session is still there
    auto fresh = std::make_unique<Run>(0, true, &d.rb, std::function<void(PlayerWorld&)>{}, &world);
    fresh->play(a.now() - fresh->now(), false);
    const uint64_t new_pawn = fresh->h.s->bootstrap().setup().guids.pawn.ObjectId;
    CHECK(new_pawn != 0 && new_pawn != old_pawn, "(the new session has a pawn of its own)");
    CHECK(book->connected(d.sb.character_guid) && book->partner_pawn(d.sa.character_guid) == new_pawn, "the book names the new session's pawn as his partner's");
    // the old session's leave arrives late
    d.b.reset();
    CHECK(book->connected(d.sb.character_guid), "the old session's leave did not erase the new session's entry");
    CHECK(book->duels() == 1 && book->fighting_key(d.sa.character_guid) != 0, "... and the duel goes on");
    fresh.reset();
    CHECK(!book->connected(d.sb.character_guid) && book->duels() == 0, "(the new session's own leave ends it)");
}
AOC_REGISTER_TEST(test_pvp_a_relog_keeps_the_duel);
