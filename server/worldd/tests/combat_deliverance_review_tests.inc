// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_deliverance_review_tests.inc: the checks that came with the review of the Deliverance, the Resplendent Beam's bounces and the bars
//  (included from combat_registered.inc, after combat_deliverance_beam_bars_tests.inc, whose Ring and helpers it uses).
//
//    the bounce   the Beam's recursive targeting sorts the injured first and then the nearest (its second sorting entry has no filter), and the
//                 recorded cast healed three targets at full Health: a whole ally is a bounce target after the hurt ones.  The chain starts from the
//                 target the first hit landed on, never from one that was out of reach or dead.
//    the charge   a Deliverance's full charge is the montage reaching its CastLoopComplete section: the time it takes follows the caster's play rate.
// ============================================================================

namespace deliverance_review {

using namespace aoc::worldd;
using deliverance::Conf;
using deliverance::Ring;
using deliverance::ask_hit;
using deliverance::branch;
using deliverance::cast;
using deliverance::confirms_of;
using deliverance::heal_of;
using deliverance::hurt;
using deliverance::items_of;
using deliverance::release;
using deliverance::target_of;

constexpr int64_t  kMagicalSpeedStat = 109343399106257ll;
constexpr uint64_t kCaster = deliverance::kCaster;

/// The rate the caster's montages play at (his MagicalSpeed, as the runtime's client_rate reads it).
double play_rate_of(Rig& r) {
    const stats::StatValue* v = r.cs->find(kMagicalSpeedStat);
    return (v && v->value > 0.2 && v->value < 3.0) ? v->value : 1.0;
}

/// One Beam cast at the caster himself, the client's bar ending at the recorded 1.241 s (progress 0.414: three hits) and its one hit request.
void beam_at_self(Ring& g, double t0) {
    Rig& r = g.caster;
    const int32_t id = cast(r, "Cleric_ResplendentBeam", t0, true);
    if (id <= 0) return;
    g.run_to(t0 + 1.20);
    branch(r, id, deliverance::kBeamBranch, t0 + 1.241);
    g.run_to(t0 + 1.40);
    ask_hit(r, "Cleric_ResplendentBeam", "Cleric_ResplendentBeam_Hit", t0 + 1.44, true);
    g.run_to(t0 + 1.7);
}

void test_review_beam_reaches_whole_allies(Fixture& f) {
    std::printf("the Beam's bounces go to the injured first and then to the nearest ally, hurt or not (the recorded cast healed three targets at full Health)\n");
    // the recorded cast: the caster and two allies, none of them hurt: three hits
    {
        Ring g(f, {{2.0, true}, {4.0, true}});
        CHECK(g.ok(), "a world with two allies at full Health");
        if (!g.ok()) return;
        beam_at_self(g, 1.0);
        const std::vector<Conf> confs = confirms_of(g.caster, deliverance::kBeamHit);
        CHECKF(confs.size() == 3, "a 41 %% charge heals the caster and the two whole allies (got %zu hits)", confs.size());
        if (confs.size() == 3) {
            CHECKF(target_of(confs[1]) == g.ally_ports[0]->actor_id() && target_of(confs[2]) == g.ally_ports[1]->actor_id(), "the nearest whole ally first, then the next");
            CHECKF(heal_of(confs[1]) > 0.0 && heal_of(confs[2]) > 0.0, "each bounce still carries its heal (%.1f, %.1f)", heal_of(confs[1]), heal_of(confs[2]));
        }
    }
    // the order: a hurt ally 8 m away is before a whole one 2 m away (the filtered sorting entry first), the whole one comes after him
    {
        Ring g(f, {{2.0, true}, {8.0, true}});
        CHECK(g.ok(), "a world with a whole ally 2 m away and a hurt one 8 m away");
        if (!g.ok()) return;
        hurt(*g.allies[1], 0.5);
        beam_at_self(g, 1.0);
        const std::vector<Conf> confs = confirms_of(g.caster, deliverance::kBeamHit);
        CHECKF(confs.size() == 3, "three hits (got %zu)", confs.size());
        if (confs.size() == 3) {
            CHECK(target_of(confs[1]) == g.ally_ports[1]->actor_id(), "the hurt ally is the first bounce though he stands farther");
            CHECK(target_of(confs[2]) == g.ally_ports[0]->actor_id(), "the whole ally comes after the hurt one");
        }
    }
    // a whole ally past 1,250 cm of the one just hit is still out of reach
    {
        Ring g(f, {{3.0, true}, {15.6, true}});
        CHECK(g.ok(), "a world with a whole ally 3 m away and another 12.6 m from him");
        if (!g.ok()) return;
        Rig& r = g.caster;
        const int32_t id = cast(r, "Cleric_ResplendentBeam", 1.0, true);
        if (id <= 0) return;
        g.run_to(4.0);
        branch(r, id, deliverance::kBeamBranch, 4.0);
        g.run_to(4.1);
        ask_hit(r, "Cleric_ResplendentBeam", "Cleric_ResplendentBeam_Hit", 4.12, true);
        g.run_to(4.5);
        const std::vector<Conf> confs = confirms_of(r, deliverance::kBeamHit);
        CHECKF(confs.size() == 2, "the whole ally 12.6 m from the one just hit is not reached (got %zu hits)", confs.size());
    }
}

void test_review_beam_starts_from_the_target_it_hit(Fixture& f) {
    std::printf("the Beam's chain starts from the ally the first hit landed on: an ally who is out of reach or dead by the hit starts none\n");
    // the named ally walks out of the caster's reach before the hit lands (his neighbour with him): nothing is healed, nobody 80 m away is bounced to
    {
        Ring g(f, {{3.0, true}, {3.5, true}});
        CHECK(g.ok(), "a world with two hurt allies near the caster");
        if (!g.ok()) return;
        Rig& r = g.caster;
        hurt(*g.allies[0], 0.5);
        hurt(*g.allies[1], 0.5);
        const uint64_t named = g.ally_ports[0]->actor_id();
        const int32_t id = cast(r, "Cleric_ResplendentBeam", 1.0, false, {named});
        CHECKF(id > 0, "the beam names the ally");
        if (id <= 0) return;
        g.run_to(4.0);
        branch(r, id, deliverance::kBeamBranch, 4.0);
        g.run_to(4.1);
        g.ally_ports[0]->rig_fighter().move_to({8000.0, 0, deliverance::kGroundZ});   // (80 m: past the 45 m a player's ability reaches)
        g.ally_ports[1]->rig_fighter().move_to({8100.0, 0, deliverance::kGroundZ});
        const double before0 = g.allies[0]->health(), before1 = g.allies[1]->health();
        ask_hit(r, "Cleric_ResplendentBeam", "Cleric_ResplendentBeam_Hit", 4.12, false, {named});
        g.run_to(4.6);
        const std::vector<Conf> confs = confirms_of(r, deliverance::kBeamHit);
        CHECKF(confs.empty(), "a hit that reached nobody bounces nowhere (got %zu hits)", confs.size());
        CHECKF(std::fabs(g.allies[1]->health() - before1) <= 30.0 && std::fabs(g.allies[0]->health() - before0) <= 30.0,
               "the allies 80 m away are not healed (%.1f -> %.1f and %.1f -> %.1f)", before0, g.allies[0]->health(), before1, g.allies[1]->health());
    }
    // the named ally has died: his neighbour is not healed through him
    {
        Ring g(f, {{3.0, true}, {3.5, true}});
        CHECK(g.ok(), "a world with two hurt allies, one of whom will die");
        if (!g.ok()) return;
        Rig& r = g.caster;
        hurt(*g.allies[0], 0.5);
        hurt(*g.allies[1], 0.5);
        const uint64_t named = g.ally_ports[0]->actor_id();
        const int32_t id = cast(r, "Cleric_ResplendentBeam", 1.0, false, {named});
        CHECKF(id > 0, "the beam names the ally");
        if (id <= 0) return;
        g.run_to(4.0);
        branch(r, id, deliverance::kBeamBranch, 4.0);
        g.run_to(4.1);
        g.allies[0]->set_current(content::kGuidHealth, 0.0);   // (he is dead when the hit lands)
        const double before1 = g.allies[1]->health();
        ask_hit(r, "Cleric_ResplendentBeam", "Cleric_ResplendentBeam_Hit", 4.12, false, {named});
        g.run_to(4.6);
        const std::vector<Conf> confs = confirms_of(r, deliverance::kBeamHit);
        CHECKF(confs.empty(), "a dead ally is no target and starts no chain (got %zu hits)", confs.size());
        CHECKF(std::fabs(g.allies[1]->health() - before1) <= 30.0, "his neighbour is not healed (%.1f -> %.1f)", before1, g.allies[1]->health());
    }
}

void test_review_deliverance_full_charge_follows_the_play_rate(Fixture& f) {
    std::printf("the Deliverance's full charge is the montage reaching its end section: a faster or a slower caster's request comes when his montage gets there and still heals the whole amount\n");
    Ring g(f);
    CHECK(g.ok(), "a world for the Deliverance");
    if (!g.ok()) return;
    Rig& r = g.caster;
    // 0: the character's own rate, 1: a faster one (1.0), 2: a slower one than the recorded casts' (0.62: the bar runs about 1.3 times as long as the recorded one)
    constexpr double kRates[3] = {0.0, 1.0, 0.62};
    double full[3] = {0, 0, 0}, rates[3] = {0, 0, 0}, jumped[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        const double t0 = 1.0 + 12.0 * i;
        if (kRates[i] > 0) set_rate(r, kRates[i], true);
        hurt(r, 0.6);
        const int32_t id = cast(r, "Cleric_Deliverance", t0, true);
        CHECKF(id > 0, "the cast %d is accepted", i + 1);
        if (id <= 0) return;
        rates[i] = play_rate_of(r);
        // the client's bar reaches the section 2.9629629 montage seconds in: that many over the rate in wall seconds (the recorded requests came 12 to 27 ms before it)
        const double at = 2.9629629 / rates[i] - 0.02;
        g.run_to(t0 + at - 0.05);
        double jump_at = 0;
        float jump_to = 0;
        CHECKF(!deliverance::jump_of(r, deliverance::kDeliverance, jump_at, jump_to, t0), "cast %d: the server has not moved the montage before the client's own full charge (%.2f s)", i + 1, at);
        CHECK(branch(r, id, deliverance::kDeliveranceBranch, t0 + at), "the client's branch request at its full charge belongs to the cast");
        g.run_to(t0 + at + 0.1);
        if (deliverance::jump_of(r, deliverance::kDeliverance, jump_at, jump_to, t0)) jumped[i] = jump_at - t0 - at;
        ask_hit(r, "Cleric_Deliverance", "Cleric_Deliverance_Hit", t0 + at + 0.17, true);
        g.run_to(t0 + at + 0.5);
        const std::vector<Conf> confs = confirms_of(r, deliverance::kDeliveranceHit, t0);
        CHECKF(confs.size() == 1, "cast %d heals once", i + 1);
        if (confs.size() != 1) return;
        full[i] = heal_of(confs[0]);
    }
    CHECKF(rates[1] > rates[0] + 0.05 && rates[2] < rates[0] - 0.05, "the three casters play at %.3f, %.3f and %.3f", rates[0], rates[1], rates[2]);
    CHECKF(full[0] > 5.0 && std::fabs(full[1] / full[0] - 1.0) <= 0.015, "the faster caster's full charge heals the same whole amount (%.1f against %.1f)", full[1], full[0]);
    CHECKF(std::fabs(full[2] / full[0] - 1.0) <= 0.015, "the slower caster's full charge heals the same whole amount (%.1f against %.1f)", full[2], full[0]);
    CHECKF(jumped[2] > 0 && jumped[2] < 0.15, "the slower caster's montage jumps after his own request, not before it (%.3f s after it)", jumped[2]);
}

void test_review_two_beams_at_once(Fixture& f) {
    std::printf("two Clerics whose beams are aimed at each other at once write only through the ally path and both heal\n");
    Ring g(f, {{5.0, true}});
    CHECK(g.ok(), "a world with two Clerics");
    if (!g.ok()) return;
    Rig& a = g.caster;
    Rig& b = *g.allies[0];
    combat_rig::FakePlayer& pb = *g.ally_ports[0];
    hurt(a, 0.4);
    hurt(b, 0.4);
    const uint64_t id_a = g.caster_port.actor_id(), id_b = pb.actor_id();
    const double t0 = 1.0;
    const int32_t ca = cast(a, "Cleric_ResplendentBeam", t0, false, {id_b});
    const int32_t cb = cast(b, "Cleric_ResplendentBeam", t0, false, {id_a});
    CHECKF(ca > 0 && cb > 0, "both Clerics cast the beam at each other at once");
    if (ca <= 0 || cb <= 0) return;
    a.run_to(t0 + 3.0);
    b.run_to(t0 + 3.0);
    CHECK(branch(a, ca, deliverance::kBeamBranch, t0 + 3.0) && branch(b, cb, deliverance::kBeamBranch, t0 + 3.0), "both ask for the branch at the 3 s hold");
    a.run_to(t0 + 3.1);
    b.run_to(t0 + 3.1);
    const double a_health = a.health(), b_health = b.health(), b_clock = b.clock;
    ask_hit(a, "Cleric_ResplendentBeam", "Cleric_ResplendentBeam_Hit", t0 + 3.12, false, {id_b});
    CHECKF(b.clock == b_clock, "the other Cleric's runtime was not advanced by the first one's step (its clock stays %.2f)", b_clock);
    ask_hit(b, "Cleric_ResplendentBeam", "Cleric_ResplendentBeam_Hit", t0 + 3.12, false, {id_a});
    a.run_to(t0 + 3.5);
    b.run_to(t0 + 3.5);
    CHECKF(a.health() > a_health && b.health() > b_health, "both heal (%.1f -> %.1f and %.1f -> %.1f)", a_health, a.health(), b_health, b.health());
    // each log holds its own beam's two hits (the other Cleric, then himself again by the bounce) and the other beam's first hit on him: three
    CHECKF(confirms_of(a, deliverance::kBeamHit).size() == 3 && confirms_of(b, deliverance::kBeamHit).size() == 3,
           "each beam hit its target and bounced back to its caster (%zu and %zu hits on the two clients)", confirms_of(a, deliverance::kBeamHit).size(),
           confirms_of(b, deliverance::kBeamHit).size());
}

void test_review_deliverance_on_another_player_by_missing_health(Fixture& f) {
    std::printf("the Deliverance at another player: the Redemptive passive reads the TARGET's Health, an early release heals by its montage time, a whole target is confirmed too\n");
    // with the passive: a target at 10 % takes 1.9 times what a whole one takes; without it the same
    for (const bool passive : {true, false}) {
        double heals[2] = {0, 0};
        for (int i = 0; i < 2; ++i) {   // 0: the party member near death, 1: a whole one
            Ring g(f, {{6.0, true}}, passive ? std::vector<std::string>{"Cleric_Passive_Deliverance_Redemptive"} : std::vector<std::string>{});
            CHECKF(g.ok(), "a world with a party member 6 m away (%s the passive)", passive ? "with" : "without");
            if (!g.ok()) return;
            Rig& r = g.caster;
            hurt(*g.allies[0], i == 0 ? 0.10 : 1.0);
            const uint64_t ally_id = g.ally_ports[0]->actor_id();
            const double t0 = 1.0;
            const int32_t id = cast(r, "Cleric_Deliverance", t0, false, {ally_id});
            CHECKF(id > 0, "the cast names the party member");
            if (id <= 0) return;
            g.run_to(t0 + 3.53);
            branch(r, id, deliverance::kDeliveranceBranch, t0 + 3.534);
            g.run_to(t0 + 3.62);
            const double before = g.allies[0]->health();
            ask_hit(r, "Cleric_Deliverance", "Cleric_Deliverance_Hit", t0 + 3.684, false, {ally_id});
            g.run_caster_to(t0 + 3.9);
            const std::vector<Conf> confs = confirms_of(r, deliverance::kDeliveranceHit);
            CHECKF(confs.size() == 1, "the heal on the %s party member is confirmed (got %zu)", i == 0 ? "dying" : "whole", confs.size());
            if (confs.size() != 1) return;
            heals[i] = heal_of(confs[0]);
            if (i == 1) CHECKF(std::fabs(g.allies[0]->health() - before) <= 30.0 && heals[i] > 5.0, "a whole target keeps his Health and the heal shows its value (%.1f -> %.1f, %.1f)", before, g.allies[0]->health(), heals[i]);
        }
        const double ratio = heals[1] > 0 ? heals[0] / heals[1] : 0.0;
        if (passive) CHECKF(ratio > 1.85 && ratio < 1.95, "with the passive the heal on a party member at 10 %% Health is 1.9 times the heal on a whole one (%.3f)", ratio);
        else CHECKF(std::fabs(ratio - 1.0) <= 0.02, "without it both heal the same (%.3f)", ratio);
    }
    // an early release: the heal on another player follows the montage time of the release, as the heal on the caster does
    {
        Ring g(f, {{6.0, true}});
        CHECK(g.ok(), "a world with a party member 6 m away");
        if (!g.ok()) return;
        Rig& r = g.caster;
        const uint64_t ally_id = g.ally_ports[0]->actor_id();
        double heals[2] = {0, 0};
        for (int i = 0; i < 2; ++i) {   // 0: released early, 1: the full charge
            const double t0 = 1.0 + 8.0 * i;
            hurt(*g.allies[0], 0.4);
            const int32_t id = cast(r, "Cleric_Deliverance", t0, false, {ally_id});
            CHECKF(id > 0, "the cast %d names the party member", i + 1);
            if (id <= 0) return;
            if (i == 0) {
                g.run_to(t0 + 1.2);
                release(r, id, t0 + 1.212, 1.011);
                g.run_to(t0 + 1.30);
                ask_hit(r, "Cleric_Deliverance", "Cleric_Deliverance_Hit", t0 + 1.362, false, {ally_id});
                g.run_caster_to(t0 + 1.7);
            } else {
                g.run_to(t0 + 3.53);
                branch(r, id, deliverance::kDeliveranceBranch, t0 + 3.534);
                g.run_to(t0 + 3.62);
                ask_hit(r, "Cleric_Deliverance", "Cleric_Deliverance_Hit", t0 + 3.684, false, {ally_id});
                g.run_caster_to(t0 + 3.9);
            }
            const std::vector<Conf> confs = confirms_of(r, deliverance::kDeliveranceHit, t0);
            CHECKF(confs.size() == 1, "cast %d heals the party member once (got %zu)", i + 1, confs.size());
            if (confs.size() != 1) return;
            heals[i] = heal_of(confs[0]);
        }
        CHECKF(heals[1] > 5.0 && std::fabs(heals[0] / heals[1] - 1.011 / 2.9629629) <= 0.01, "the early heal on him is %.3f of the full one (its montage time 1.011 s over 2.963 s)", heals[0] / heals[1]);
    }
}

void test_review_disengage_late_request(Fixture& f) {
    std::printf("a Disengage's branch request that comes after its montage was cleared clears nothing twice\n");
    Ring g(f, {}, {}, 11, 1);   // (a Ranger)
    CHECK(g.ok(), "a world for the Disengage");
    if (!g.ok()) return;
    Rig& r = g.caster;
    const double t0 = 1.0;
    const int32_t id = cast(r, "Ranger_Disengage", t0, false);
    CHECKF(id > 0, "the Disengage is accepted");
    if (id <= 0) return;
    g.run_to(t0 + 1.5);
    const auto clears = [&]() {
        size_t n = 0;
        for (const deliverance::Item& i : items_of(r, deliverance::kDisengage)) {
            if (i.at > t0 && i.kind == 1 && i.path.empty()) ++n;
        }
        return n;
    };
    CHECKF(clears() == 1, "with no request its montage was cleared once at the recorded moment (%zu clears)", clears());
    CHECK(branch(r, id, deliverance::kDisengageBranch, t0 + 1.5), "the late CancelIfFalling request is taken quietly");
    g.run_to(t0 + 2.2);
    CHECKF(clears() == 1, "the late request does not clear the montage again (%zu clears)", clears());
}

void test_review_beam_released_early(Fixture& f) {
    std::printf("a Resplendent Beam whose key goes up before the bar is full: its hits follow the montage time of the release over the 3 s hold (0.3 of the hold: three hits, 0.8: five)\n");
    // the release's CancelledTime is in montage seconds: the wall time held times the caster's play rate
    struct Run {
        double held;
        size_t hits;
    };
    for (const Run want : {Run{0.9, 3}, Run{2.4, 5}}) {
        Ring g(f, {{2.0, true}, {4.0, true}, {6.0, true}, {8.0, true}, {10.0, true}});
        CHECK(g.ok(), "a world with five hurt allies");
        if (!g.ok()) return;
        Rig& r = g.caster;
        for (auto& a : g.allies) hurt(*a, 0.5);
        const double t0 = 1.0;
        const int32_t id = cast(r, "Cleric_ResplendentBeam", t0, true);
        CHECKF(id > 0, "the beam is accepted");
        if (id <= 0) return;
        g.run_to(t0 + want.held - 0.02);
        release(r, id, t0 + want.held, want.held * play_rate_of(r));
        g.run_to(t0 + want.held + 0.15);
        ask_hit(r, "Cleric_ResplendentBeam", "Cleric_ResplendentBeam_Hit", t0 + want.held + 0.2, true);
        g.run_to(t0 + want.held + 0.6);
        const std::vector<Conf> confs = confirms_of(r, deliverance::kBeamHit);
        CHECKF(confs.size() == want.hits, "released after %.1f s of the 3 s hold the beam hits %zu targets (got %zu)", want.held, want.hits, confs.size());
    }
}

AOC_REGISTER_TEST(test_review_beam_reaches_whole_allies);
AOC_REGISTER_TEST(test_review_beam_starts_from_the_target_it_hit);
AOC_REGISTER_TEST(test_review_deliverance_full_charge_follows_the_play_rate);
AOC_REGISTER_TEST(test_review_two_beams_at_once);
AOC_REGISTER_TEST(test_review_beam_released_early);
AOC_REGISTER_TEST(test_review_deliverance_on_another_player_by_missing_health);
AOC_REGISTER_TEST(test_review_disengage_late_request);

}  // namespace deliverance_review
