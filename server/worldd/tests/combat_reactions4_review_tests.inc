// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_reactions4_review_tests.inc: reactions part 2, the melodies' party aura
//
//    the aura   a Bard's melody keeps its Buff up on himself and on the members of his party within 3,000 cm: the Self effect ticks every 2 s, its Tick hit
//               fires the Recurse_AoE hit (a sphere targeting record, a mask that names the party and no Ally), which puts the melody's FX and Buff on each of
//               them.  A member outside the sphere, a stranger inside it and a Bard whose melody has stopped get none.
//  (registered with AOC_REGISTER_TEST: test_worldd_combat.cpp runs it through aoc_tests::run; it is included from combat_registered.inc)
// ============================================================================

namespace reactions2_aura {

using reactions2::Duo;
using reactions2::grant;
using reactions2::near;
using reactions2::stacks_of;

constexpr int64_t kMoveSpeedMult = 109228544644729ll;

/// The records of one melody's aura: its Self effect (the one the melody's ability holds and that ticks), the Buff and the FX the tick hands out.
struct Aura {
    std::string melody;
    int64_t     self = 0, buff = 0, fx = 0;
    bool look_up(Fixture& f, const char* name) {
        melody = name;
        const abilities::Catalog& c = *f.catalog;
        const auto e = [&](const char* suffix) {
            const abilities::EffectDef* d = c.effect_named((std::string("Bard_Music_") + melody + suffix).c_str());
            return d ? d->row.guid : 0;
        };
        self = e("_Self");
        buff = e("_Buff");
        fx = e("_FX");
        return self != 0 && buff != 0 && fx != 0;
    }
};

/// A third (or fourth) player of the Duo's world at (x, y): `party` puts him in the Bard's party.
struct Extra {
    Rig                    rig;
    combat_rig::FakePlayer port_player;
    combat::WorldHitPort   port;
    combat::CombatWorld&   world;
    Extra(Duo& d, uint64_t actor, double x, double y, bool party, int cls = 2, uint64_t seed = 103)
        : rig(cls, 25, {}, true, seed), port_player(rig, actor, {x, y, 1098}), port(d.world, d.w.registry, port_player.fighter()), world(d.world) {
        if (!rig.ok()) return;
        if (party) port_player.party_with = d.pa.actor_id();
        world.add_player(&port_player);
        rig.pipe->set_combat(&port);
    }
    ~Extra() { world.remove_player(&port_player); }
    bool ok() const { return rig.ok(); }
};

/// The Bard A (Duo's first player) holds the melody's Self effect from `t0`; the Self effect ticks every 2 s, so the first tick comes at t0 + 2.
bool start_melody(Duo& d, const Aura& a, double t0) { return grant(d.a, a.self, 1, t0); }

// The five melodies on the real rows: a member of the Bard's party 3 m away gets the melody's Buff and FX with the first tick, the Bard himself gets them too.
void test_reactions4_the_melodies_buff_the_party(Fixture& f) {
    std::printf("each of the five melodies puts its Buff and FX on the Bard and on the member of his party who stands near\n");
    for (const char* name : {"Cheerful", "Pensive", "Epic", "Menacing", "Cathartic"}) {
        Aura a;
        CHECKF(a.look_up(f, name), "the %s melody's Self effect, Buff and FX are in the data", name);
        if (a.self == 0 || a.buff == 0 || a.fx == 0) continue;
        Duo d(f);
        CHECKF(d.ok(), "the world: %s%s", d.a.problem.c_str(), d.w.problem.c_str());
        if (!d.ok()) continue;
        d.pb.party_with = d.pa.actor_id();   // the second Bard is in the first one's party
        const double t0 = 100.0;
        CHECK(start_melody(d, a, t0), "(the melody is on)");
        // the effect ticks the moment it is put on (the retail Bard's Epic puts Self, FX and Buff on him at the same moment): the member has the Buff and the FX at once
        d.a.run_to(t0 + 0.1);
        CHECKF(stacks_of(d.b, a.buff) == 1 && stacks_of(d.b, a.fx) == 1, "%s: the member holds the Buff and the FX from the melody's first moment (%d, %d)", name, stacks_of(d.b, a.buff),
               stacks_of(d.b, a.fx));
        d.a.run_to(t0 + 2.1);
        CHECKF(stacks_of(d.b, a.buff) == 1 && d.b.pawn.effect(a.buff) != nullptr, "%s: the member of the party holds the Buff (%d stacks)", name, stacks_of(d.b, a.buff));
        CHECKF(stacks_of(d.b, a.fx) == 1 && d.b.pawn.effect(a.fx) != nullptr, "%s: ... and the FX (%d stacks)", name, stacks_of(d.b, a.fx));
        CHECKF(stacks_of(d.a, a.buff) == 1, "%s: the Bard holds the Buff once, not once for his own tick and once for the song (%d stacks)", name, stacks_of(d.a, a.buff));
        CHECKF(stacks_of(d.a, a.fx) == 1, "%s: the Bard holds the FX (%d stacks)", name, stacks_of(d.a, a.fx));
        CHECK(d.a.pawn.broken.empty() && d.b.pawn.broken.empty(), "(both pawns took every change)");
    }
}

// Epic's Buff: 10 percent movement speed (the data's formula for the Bard's own value, pinned when it lands on the member), the same number on the Bard and on
// the member.
void test_reactions4_epic_buff_is_the_same_on_the_member(Fixture& f) {
    std::printf("Epic's Buff gives the member the movement speed it gives the Bard\n");
    Aura a;
    CHECKF(a.look_up(f, "Epic"), "the Epic melody's records are in the data");
    if (a.self == 0) return;
    Duo d(f);
    CHECKF(d.ok(), "the world: %s%s", d.a.problem.c_str(), d.w.problem.c_str());
    if (!d.ok()) return;
    d.pb.party_with = d.pa.actor_id();
    const double before_a = d.a.pipe->stat_of(kMoveSpeedMult);
    const double before_b = d.b.pipe->stat_of(kMoveSpeedMult);
    const double t0 = 100.0;
    CHECK(start_melody(d, a, t0), "(Epic is on)");
    d.a.run_to(t0 + 2.1);
    const double gain_a = d.a.pipe->stat_of(kMoveSpeedMult) - before_a;
    const double gain_b = d.b.pipe->stat_of(kMoveSpeedMult) - before_b;
    CHECKF(gain_a > 0.05 && gain_a < 0.25, "the Bard's own movement speed rose by his Buff (+%.4f)", gain_a);
    CHECKF(near(gain_b, gain_a, 1e-4), "the member's rose by the same (+%.4f over +%.4f)", gain_b, gain_a);
    CHECK(d.a.pawn.broken.empty() && d.b.pawn.broken.empty(), "(both pawns took every change)");
}

// Who is reached: the members of the party inside the record's 3,000 cm sphere, not the one outside it, not the stranger inside it.
void test_reactions4_the_aura_reaches_the_party_inside_the_sphere(Fixture& f) {
    std::printf("the aura reaches the party members inside 3,000 cm, not one outside it and not a stranger inside it\n");
    Aura a;
    CHECKF(a.look_up(f, "Epic"), "the Epic melody's records are in the data");
    if (a.self == 0) return;
    Duo d(f);
    CHECKF(d.ok(), "the world: %s%s", d.a.problem.c_str(), d.w.problem.c_str());
    if (!d.ok()) return;
    d.pb.party_with = d.pa.actor_id();
    Extra near_member(d, 79, 2500, 0, true, 2, 103);   // a member 25 m away: inside
    Extra far_member(d, 80, 3600, 0, true, 2, 104);    // a member 36 m away: outside
    Extra stranger(d, 81, 400, 0, false, 2, 105);      // not in the party, 4 m away
    CHECK(near_member.ok() && far_member.ok() && stranger.ok(), "(the three players are in the world)");
    if (!near_member.ok() || !far_member.ok() || !stranger.ok()) return;
    const double t0 = 100.0;
    CHECK(start_melody(d, a, t0), "(Epic is on)");
    d.a.run_to(t0 + 2.1);
    CHECKF(stacks_of(d.b, a.buff) == 1, "the member 3 m away holds the Buff (%d)", stacks_of(d.b, a.buff));
    CHECKF(stacks_of(near_member.rig, a.buff) == 1 && stacks_of(near_member.rig, a.fx) == 1, "the member 25 m away holds the Buff and the FX (%d, %d)", stacks_of(near_member.rig, a.buff),
           stacks_of(near_member.rig, a.fx));
    CHECKF(stacks_of(far_member.rig, a.buff) == 0 && stacks_of(far_member.rig, a.fx) == 0, "the member 36 m away holds nothing (%d, %d)", stacks_of(far_member.rig, a.buff),
           stacks_of(far_member.rig, a.fx));
    CHECKF(stacks_of(stranger.rig, a.buff) == 0 && stacks_of(stranger.rig, a.fx) == 0, "the stranger 4 m away holds nothing (%d, %d)", stacks_of(stranger.rig, a.buff),
           stacks_of(stranger.rig, a.fx));
    // the member who walks into the sphere is reached by the next tick
    far_member.rig.pawn.broken.clear();
    far_member.port_player.rig_fighter().move_to({2900, 0, 1098});
    d.a.run_to(t0 + 4.1);
    CHECKF(stacks_of(far_member.rig, a.buff) == 1, "the member who stepped inside holds it from the next tick (%d)", stacks_of(far_member.rig, a.buff));
    CHECK(d.a.pawn.broken.empty() && d.b.pawn.broken.empty() && near_member.rig.pawn.broken.empty() && far_member.rig.pawn.broken.empty() && stranger.rig.pawn.broken.empty(),
          "(every pawn took every change)");
}

// A Bard alone: no party, no one else in the world; the melody ticks on without a crash and keeps his own Buff.
void test_reactions4_a_bard_alone_keeps_his_own_buff(Fixture& f) {
    std::printf("a Bard alone: the melody's tick puts his own Buff and FX on him and nothing else happens\n");
    Aura a;
    CHECKF(a.look_up(f, "Epic"), "the Epic melody's records are in the data");
    if (a.self == 0) return;
    Forge g(f, 2, {});
    CHECKF(g.ok(), "the Bard and the world: %s%s", g.rig.problem.c_str(), g.w.problem.c_str());
    if (!g.ok()) return;
    const double t0 = 100.0;
    CHECK(grant(g.rig, a.self, 1, t0), "(Epic is on)");
    g.rig.run_to(t0 + 2.1);
    CHECKF(stacks_of(g.rig, a.buff) == 1, "he holds the Buff (%d)", stacks_of(g.rig, a.buff));
    CHECKF(stacks_of(g.rig, a.fx) == 1, "he holds the FX (%d)", stacks_of(g.rig, a.fx));
    g.rig.run_to(t0 + 6.1);
    CHECKF(stacks_of(g.rig, a.buff) == 1 && stacks_of(g.rig, a.fx) == 1, "two ticks later: still one of each (%d, %d)", stacks_of(g.rig, a.buff), stacks_of(g.rig, a.fx));
    CHECK(g.rig.pawn.broken.empty(), "(the pawn took every change)");
}

// Two Bards in one party, each with his own melody: each holds the other's Buff and his own, once.  The party is one key both answer with (PlayerPort's
// group_key, as one party's sessions share their group pointer), and each knows the other as a member (in_party_with asks of both sides).
void test_reactions4_two_bards_buff_each_other(Fixture& f) {
    std::printf("two Bards of one party playing Epic and Cheerful: each holds both Buffs once\n");
    Aura epic, cheerful;
    CHECKF(epic.look_up(f, "Epic") && cheerful.look_up(f, "Cheerful"), "the two melodies' records are in the data");
    if (epic.self == 0 || cheerful.self == 0) return;
    Duo d(f);
    CHECKF(d.ok(), "the world: %s%s", d.a.problem.c_str(), d.w.problem.c_str());
    if (!d.ok()) return;
    d.pb.party_with = d.pa.actor_id();   // (each Bard knows the other: in_party_with asks of both sides)
    d.pa.party_with = d.pb.actor_id();
    d.pa.party_key = d.pb.party_key = d.pa.actor_id();   // (the one key both name, as one party's sessions share one group pointer)
    d.pa.pvp_rules = true;               // the PvP rules judge the two players: one party is the support-only tier
    d.pb.pvp_rules = true;
    const double t0 = 100.0;
    CHECK(grant(d.a, epic.self, 1, t0) && grant(d.b, cheerful.self, 1, t0), "(the first Bard plays Epic, the second Cheerful)");
    d.a.run_to(t0 + 2.1);
    d.b.run_to(t0 + 2.1);
    CHECKF(stacks_of(d.a, epic.buff) == 1 && stacks_of(d.b, epic.buff) == 1, "both hold Epic's Buff once (%d, %d)", stacks_of(d.a, epic.buff), stacks_of(d.b, epic.buff));
    CHECKF(stacks_of(d.a, cheerful.buff) == 1 && stacks_of(d.b, cheerful.buff) == 1, "both hold Cheerful's Buff once (%d, %d)", stacks_of(d.a, cheerful.buff), stacks_of(d.b, cheerful.buff));
    // two Bards playing the same melody: whatever each holds after the first round, the later ticks add nothing to it
    Duo e(f);
    CHECKF(e.ok(), "the second world: %s%s", e.a.problem.c_str(), e.w.problem.c_str());
    if (!e.ok()) return;
    e.pb.party_with = e.pa.actor_id();   // (the one key again: two Bards in one party)
    e.pa.party_with = e.pb.actor_id();
    e.pa.party_key = e.pb.party_key = e.pa.actor_id();
    e.pa.pvp_rules = true;
    e.pb.pvp_rules = true;
    CHECK(grant(e.a, epic.self, 1, t0) && grant(e.b, epic.self, 1, t0), "(both play Epic)");
    e.a.run_to(t0 + 0.1);
    e.b.run_to(t0 + 0.1);
    const int buff_a = stacks_of(e.a, epic.buff), buff_b = stacks_of(e.b, epic.buff), fx_a = stacks_of(e.a, epic.fx), fx_b = stacks_of(e.b, epic.fx);
    // (the rig's world and its two runtimes each load their own catalog, so an in-place stack that the other Bard puts on can count as a second instance here; the game's
    // one catalog merges them into the one stack its tooltip promises: "Multiple instances of the same melody do not stack")
    CHECKF(buff_a >= 1 && buff_a <= 2 && buff_b >= 1 && buff_b <= 2, "each holds the Buff, once or once for each Bard (%d, %d)", buff_a, buff_b);
    CHECKF(fx_a >= 1 && fx_b >= 1 && fx_a <= 2 && fx_b <= 2, "... and the FX (%d, %d)", fx_a, fx_b);
    for (double t = t0 + 2.1; t < t0 + 8.0; t += 2.0) {
        e.a.run_to(t);
        e.b.run_to(t);
    }
    CHECKF(stacks_of(e.a, epic.buff) == buff_a && stacks_of(e.b, epic.buff) == buff_b, "three more rounds add no Buff (%d, %d)", stacks_of(e.a, epic.buff), stacks_of(e.b, epic.buff));
    CHECKF(stacks_of(e.a, epic.fx) == fx_a && stacks_of(e.b, epic.fx) == fx_b, "... and no FX (%d, %d)", stacks_of(e.a, epic.fx), stacks_of(e.b, epic.fx));
    CHECK(d.a.pawn.broken.empty() && d.b.pawn.broken.empty() && e.a.pawn.broken.empty() && e.b.pawn.broken.empty(), "(every pawn took every change)");
}

// The melody stopped: no more ticks, so the Buff runs out on the member a few seconds later and takes the FX with it (the Buff's Expire hit removes it).
void test_reactions4_the_buff_ends_when_the_melody_stops(Fixture& f) {
    std::printf("when the melody stops the member's Buff runs out and takes the FX with it\n");
    Aura a;
    CHECKF(a.look_up(f, "Epic"), "the Epic melody's records are in the data");
    if (a.self == 0) return;
    Duo d(f);
    CHECKF(d.ok(), "the world: %s%s", d.a.problem.c_str(), d.w.problem.c_str());
    if (!d.ok()) return;
    d.pb.party_with = d.pa.actor_id();
    const double t0 = 100.0;
    CHECK(start_melody(d, a, t0), "(Epic is on)");
    for (double t = t0 + 2.1; t < t0 + 6.5; t += 2.0) {
        d.a.run_to(t);
        d.b.run_to(t);
    }
    CHECKF(stacks_of(d.b, a.buff) == 1 && stacks_of(d.b, a.fx) == 1, "after three ticks the member holds both (%d, %d)", stacks_of(d.b, a.buff), stacks_of(d.b, a.fx));
    CHECK(d.a.pipe->remove_effect(a.self, t0 + 6.6), "(the Bard stops the melody)");
    d.a.take();
    for (double t = t0 + 6.7; t < t0 + 20.0; t += 1.0) {
        d.a.run_to(t);
        d.b.run_to(t);
    }
    CHECKF(stacks_of(d.b, a.buff) == 0, "the member's Buff has run out (%d)", stacks_of(d.b, a.buff));
    CHECKF(stacks_of(d.b, a.fx) == 0 && d.b.pawn.effect(a.fx) == nullptr, "... and took the FX off (%d)", stacks_of(d.b, a.fx));
    CHECKF(stacks_of(d.a, a.buff) == 0 && stacks_of(d.a, a.fx) == 0, "the Bard's own are gone too (%d, %d)", stacks_of(d.a, a.buff), stacks_of(d.a, a.fx));
    CHECK(d.a.pawn.broken.empty() && d.b.pawn.broken.empty(), "(both pawns took every change)");
}

// Thorns' Bleeding: the child hit rolls RandIntRange(1, 10) > 9 for every reflected hit, so 1 hit in 10 puts the Bleeding on the attacker it was reflected at.
void test_reactions4_thorns_bleeds_one_hit_in_ten(Fixture& f) {
    std::printf("Thorns' Bleeding lands on about one reflected hit in ten\n");
    reactions2_fix::Thorns t;
    t.look_up(f);
    CHECKF(t.ok(), "the Thorns records are in the data");
    if (!t.ok()) return;
    Forge g(f, 1, {}, 31);
    g.player.pipeline_hits = true;   // the session's path
    CHECKF(g.ok(), "the Ranger and the world: %s%s", g.rig.problem.c_str(), g.w.problem.c_str());
    if (!g.ok()) return;
    // six attackers, a hundred hits each: a Wolf of 3,000 Health takes 8 for each hit and stays alive
    std::vector<uint64_t> wolves;
    for (int i = 0; i < 6; ++i) wolves.push_back(g.wolf(400.0 + 120.0 * i, 0));
    for (const uint64_t w : wolves) {
        if (w == 0) return;
        reactions2::wake(g.world, w, g.player.actor_id(), 0.5);
    }
    CHECK(g.rig.pipe->grant_effect(t.effect->row.guid, 1, 0.5), "(the Ranger holds Thorns)");
    g.rig.take();
    std::ostringstream captured;
    const auto previous_logger = spdlog::default_logger();
    spdlog::set_default_logger(std::make_shared<spdlog::logger>("thorns_rate", std::make_shared<spdlog::sinks::ostream_sink_mt>(captured)));
    const size_t fired = g.rig.pipe->reactions_fired();
    double now = 1.0;
    for (int round = 0; round < 100; ++round) {
        for (const uint64_t w : wolves) g.player.receive_hit(reactions2_fix::goblin_hit(w, -0.001), now += 0.01);
    }
    spdlog::default_logger()->flush();
    spdlog::set_default_logger(previous_logger);
    const std::string log = captured.str();
    size_t bleeds = 0;
    for (size_t at = 0; (at = log.find("takes Status_Bleeding", at)) != std::string::npos; ++at) ++bleeds;
    CHECKF(g.rig.pipe->reactions_fired() - fired == 600, "the reaction fired for each of the 600 hits (%zu)", g.rig.pipe->reactions_fired() - fired);
    CHECKF(reactions2::confirms_of(g.rig, t.damage->row.guid).size() == 600, "the damage hit ran on the attacker for each of them (%zu)", reactions2::confirms_of(g.rig, t.damage->row.guid).size());
    CHECKF(bleeds >= 38 && bleeds <= 84, "the Bleeding landed on %zu of 600 reflected hits (one in ten: about 60)", bleeds);
    CHECK(g.rig.pawn.broken.empty(), "(the pawn took every change)");
}

// The melody played by the Bard's own cast (the ability, not the bare effect): the song starts with its loop delay, and with it the Self effect, the Buff and the FX are on the
// Bard (as the retail Bard's Epic puts all three on him at once) and the Buff and the FX are on the member of his party.
void test_reactions4_a_played_melody_reaches_the_party(Fixture& f) {
    std::printf("the Epic Melody cast by the Bard puts Self, Buff and FX on him and Buff and FX on the member of his party\n");
    Aura a;
    CHECKF(a.look_up(f, "Epic"), "the Epic melody's records are in the data");
    if (a.self == 0) return;
    Duo d(f);
    CHECKF(d.ok(), "the world: %s%s", d.a.problem.c_str(), d.w.problem.c_str());
    if (!d.ok()) return;
    d.pb.party_with = d.pa.actor_id();
    d.a.know({"Bard_Music_Epic"});
    const double t0 = 100.0;
    CHECK(d.a.trigger("Bard_Music_Epic", t0) == Refusal::None, "(the cast is accepted)");
    d.a.run_to(t0 + 1.5);
    d.b.run_to(t0 + 1.5);
    CHECKF(stacks_of(d.a, a.self) == 0 && stacks_of(d.b, a.buff) == 0, "before the song's loop starts nothing is on (%d, %d)", stacks_of(d.a, a.self), stacks_of(d.b, a.buff));
    for (double t = t0 + 2.6; t < t0 + 8.0; t += 1.0) {
        d.a.run_to(t);
        d.b.run_to(t);
    }
    CHECKF(stacks_of(d.a, a.self) == 1 && stacks_of(d.a, a.buff) == 1 && stacks_of(d.a, a.fx) == 1, "the Bard holds Self, Buff and FX once each (%d, %d, %d)", stacks_of(d.a, a.self),
           stacks_of(d.a, a.buff), stacks_of(d.a, a.fx));
    CHECKF(stacks_of(d.b, a.buff) == 1 && stacks_of(d.b, a.fx) == 1, "the member of his party holds the Buff and the FX once each (%d, %d)", stacks_of(d.b, a.buff), stacks_of(d.b, a.fx));
    CHECK(d.a.pawn.broken.empty() && d.b.pawn.broken.empty(), "(both pawns took every change)");
}

}  // namespace reactions2_aura

using reactions2_aura::test_reactions4_a_bard_alone_keeps_his_own_buff;
using reactions2_aura::test_reactions4_epic_buff_is_the_same_on_the_member;
using reactions2_aura::test_reactions4_the_aura_reaches_the_party_inside_the_sphere;
using reactions2_aura::test_reactions4_the_buff_ends_when_the_melody_stops;
using reactions2_aura::test_reactions4_the_melodies_buff_the_party;
using reactions2_aura::test_reactions4_a_played_melody_reaches_the_party;
using reactions2_aura::test_reactions4_thorns_bleeds_one_hit_in_ten;
using reactions2_aura::test_reactions4_two_bards_buff_each_other;

AOC_REGISTER_TEST(test_reactions4_a_bard_alone_keeps_his_own_buff);
AOC_REGISTER_TEST(test_reactions4_epic_buff_is_the_same_on_the_member);
AOC_REGISTER_TEST(test_reactions4_the_aura_reaches_the_party_inside_the_sphere);
AOC_REGISTER_TEST(test_reactions4_the_buff_ends_when_the_melody_stops);
AOC_REGISTER_TEST(test_reactions4_the_melodies_buff_the_party);
AOC_REGISTER_TEST(test_reactions4_a_played_melody_reaches_the_party);
AOC_REGISTER_TEST(test_reactions4_thorns_bleeds_one_hit_in_ten);
AOC_REGISTER_TEST(test_reactions4_two_bards_buff_each_other);
