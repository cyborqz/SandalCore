// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_three_fixes_review_tests.inc: the checks that came with the review of Scatter Shot's ground marks, a held Scatter Shot's end and Air Strike's
//  per-target cooldown (included from combat_registered.inc, after combat_three_fixes_tests.inc, whose rigs and helpers it uses).
//
//    a refused press   a press the server refuses (an ability the character does not know) ends nothing: a held Scatter Shot goes on from its release
//    the marks         a held Scatter Shot's tap sends the thirty ground marks too; they die with a caster who dies before the arrows land; a caster who stands
//                      away from the origin gets them under his own feet; the same marks and shots after an Escape work as before
//    left alone        a held cast the key has released, or whose charge is full, has gone out: the client's end changes nothing of it
//    two Air Strikes   two Rangers' Air Strikes over one Wolf: the Wolf's own cooldown (every caster's) lets one area hurt it; a caster who dies between two areas
//                      lands no more of them
// ============================================================================

namespace three_fixes_review {

using namespace aoc::worldd;
using three_fixes::HeldShot;
using three_fixes::Mark;
using three_fixes::TwoRangers;
using three_fixes::ability_instance;
using three_fixes::first_area_at;
using three_fixes::hit_named;
using three_fixes::hits_on;
using three_fixes::marks_of;

// ---- a refused press -------------------------------------------------------------------------------------------------------------------------------

// A press the server refuses (the character does not know the ability) changes nothing of the cast the key still holds: the key goes up half a second
// after the trigger and the scatter is sent, paid and cooled down as for any held cast.
void test_review_refused_press_leaves_the_held_cast(Fixture& f) {
    std::printf("a refused press leaves a held Scatter Shot alone: it goes on from the key's release\n");
    constexpr double t0 = 1.0;
    HeldShot h(f, t0);
    CHECKF(h.ok() && h.accepted, "the held Scatter Shot is cast (%s)", h.g.rig.problem.c_str());
    if (!h.ok() || !h.accepted) return;
    h.g.rig.run_to(t0 + 0.3);
    const double mana = h.g.rig.mana();
    const Refusal refused = h.g.rig.trigger("Ranger_AirStrike", t0 + 0.3, false);
    CHECKF(refused == Refusal::NotKnown, "an ability the Ranger does not know is refused (%s)", refusal_name(refused));
    CHECK(h.g.rig.pipe->cancel(h.client_id, t0 + 0.5, 0.2), "the key goes up half a second after the trigger");
    h.g.rig.run_to(t0 + 1.5);
    CHECKF(h.g.lost({h.wolf}) > 0.0, "the scatter is sent after the refused press: the Wolf takes damage (%.1f)", h.g.lost({h.wolf}));
    CHECKF(h.g.rig.mana() < mana - 0.5, "...and its cost is paid (%.1f of %.1f)", h.g.rig.mana(), mana);
    CHECKF(h.g.rig.pipe->charges_left(h.g.rig.ability("Ranger_ScatterShot")) != -1, "...and its cooldown runs (%d)", h.g.rig.pipe->charges_left(h.g.rig.ability("Ranger_ScatterShot")));
}

// ---- the ground marks ------------------------------------------------------------------------------------------------------------------------------

// A held Scatter Shot's tap (the key up at the recorded 125 ms) is the normal scatter: its thirty ground marks come with the homing arrow's hit, on the Wolf.
void test_review_held_tap_sends_the_marks(Fixture& f) {
    std::printf("a held Scatter Shot released at once sends the thirty ground marks with its scatter\n");
    const abilities::HitDef* ground = hit_named(f, "Ranger_Scattershot_GroundFX_ImpactHit");
    const abilities::HitDef* damage = hit_named(f, "Ranger_ScatterShot");
    CHECK(ground != nullptr && damage != nullptr, "the ground impact hit and the scatter's hit are in the data");
    if (!ground || !damage) return;
    constexpr double t0 = 1.0;
    HeldShot h(f, t0);
    CHECKF(h.ok() && h.accepted, "the held Scatter Shot is cast (%s)", h.g.rig.problem.c_str());
    if (!h.ok() || !h.accepted) return;
    h.g.rig.run_to(t0 + 0.125);
    CHECK(h.g.rig.pipe->cancel(h.client_id, t0 + 0.125, 0.1), "the key goes up 125 ms after the trigger");
    h.g.rig.run_to(t0 + 1.5);
    const std::vector<Mark> found = marks_of(h.g.rig, ground->row.guid);
    CHECKF(found.size() == 30, "thirty ground impact confirmations are sent (%zu)", found.size());
    size_t on_wolf = 0, off_moment = 0;
    for (const Mark& m : found) {
        on_wolf += (m.targets == 1 && m.target == h.wolf) ? 1 : 0;
        off_moment += std::fabs(m.at - found.front().at) <= 1e-9 ? 0 : 1;
    }
    CHECKF(on_wolf == 30, "all of them name the Wolf (%zu)", on_wolf);
    CHECKF(off_moment == 0, "all of them come in one moment (%zu are elsewhere)", off_moment);
    double damage_at = -1;
    for (const Seen& s : h.g.rig.log) {
        for (const bootstrap::HitConfirm& c : s.confirmations) {
            if (c.record_guid != damage->row.guid) continue;
            for (const bootstrap::HitTargetOut& t : c.targets) {
                if (t.component.ObjectId == h.wolf && damage_at < 0) damage_at = s.at;
            }
        }
    }
    CHECKF(!found.empty() && damage_at >= 0 && std::fabs(damage_at - found.front().at) < 1e-9, "...with the homing arrow's damage hit (%.3f against %.3f)",
           found.empty() ? -1.0 : found.front().at, damage_at);
}

// The Wolf is 22 m away: the jump comes about 0.22 s after the trigger and the homing arrow lands 0.41 s after the jump.  A caster who dies between the two
// lands nothing: the marks die with the cast, as the arrow does.  The same scene without the death sends all thirty.
void test_review_marks_die_with_the_caster(Fixture& f) {
    std::printf("a caster who dies after his scatter's jump and before its arrows land sends no ground marks and hurts nobody\n");
    const abilities::HitDef* ground = hit_named(f, "Ranger_Scattershot_GroundFX_ImpactHit");
    CHECK(ground != nullptr, "the ground impact hit is in the data");
    if (!ground) return;
    constexpr double t0 = 1.0;
    for (const bool dies : {false, true}) {
        Forge g(f, 1, {});
        CHECKF(g.ok(), "the Ranger and the world: %s%s", g.rig.problem.c_str(), g.w.problem.c_str());
        if (!g.ok()) return;
        g.rig.asks = false;
        const uint64_t wolf = g.wolf(2200, 0);
        CHECK(wolf != 0, "a Wolf 22 m ahead of the Ranger");
        if (wolf == 0) return;
        hitchecks::set_evasion(f, g.world, wolf, -5.0, t0 - 1.0);
        g.rig.know({"Ranger_ScatterShot"});
        CHECK(g.rig.trigger("Ranger_ScatterShot", t0, false) == Refusal::None, "the Scatter Shot is cast");
        g.rig.run_to(t0 + 0.3);
        CHECKF(marks_of(g.rig, ground->row.guid).empty(), "the marks wait for the arrow (%zu so far)", marks_of(g.rig, ground->row.guid).size());
        if (dies) g.rig.pipe->set_dead(true);
        g.rig.run_to(t0 + 2.0);
        const size_t sent = marks_of(g.rig, ground->row.guid).size();
        if (dies) {
            CHECKF(sent == 0, "the dead caster's scatter sends no ground marks (%zu)", sent);
            CHECKF(g.lost({wolf}) == 0.0, "...and its arrow hurts nobody (%.1f)", g.lost({wolf}));
        } else {
            CHECKF(sent == 30, "the living caster's scatter sends its thirty (%zu)", sent);
            CHECKF(g.lost({wolf}) > 0.0, "...and its arrow hurts the Wolf (%.1f)", g.lost({wolf}));
        }
    }
}

// A caster who stands 6 m from the origin with nobody ahead: the thirty marks are at the ground under him (his pawn is the target), not at the world's origin.
void test_review_marks_under_a_caster_who_stands_elsewhere(Fixture& f) {
    std::printf("a Scatter Shot at nobody sends its ground marks under the caster who stands 6 m from the origin\n");
    const abilities::HitDef* ground = hit_named(f, "Ranger_Scattershot_GroundFX_ImpactHit");
    CHECK(ground != nullptr, "the ground impact hit is in the data");
    if (!ground) return;
    TwoRangers s(f);
    CHECKF(s.ok(), "the two Rangers and their world: %s%s%s", s.a.problem.c_str(), s.b.problem.c_str(), s.w.problem.c_str());
    if (!s.ok()) return;
    s.a.asks = false;
    s.b.asks = false;
    s.a.know({"Ranger_ScatterShot"});
    s.b.know({"Ranger_ScatterShot"});
    constexpr double t0 = 1.0;
    CHECK(s.b.trigger("Ranger_ScatterShot", t0, false) == Refusal::None, "the second Ranger's Scatter Shot is cast with nobody ahead");
    s.a.run_to(t0 + 1.5);
    s.b.run_to(t0 + 1.5);
    const std::vector<Mark> marks_b = marks_of(s.b, ground->row.guid);
    CHECKF(marks_b.size() == 30, "his thirty ground impact confirmations are sent (%zu)", marks_b.size());
    CHECKF(marks_of(s.a, ground->row.guid).empty(), "the first Ranger, who cast nothing, sends none");
    size_t on_him = 0, off_point = 0;
    for (const Mark& m : marks_b) {
        on_him += m.target == s.pb.pawn_guid().ObjectId ? 1 : 0;
        off_point += (std::fabs(m.x - 600.0) <= 1.0 && std::fabs(m.y) <= 1.0) ? 0 : 1;
    }
    CHECKF(on_him == 30, "all of them name his own pawn (%zu)", on_him);
    CHECKF(off_point == 0, "all of them are at the ground under him, 600 cm from the origin (%zu are off it)", off_point);
}

// ---- Escape, and what it leaves alone --------------------------------------------------------------------------------------------------------------

// After an Escape the character casts the Scatter Shot again at once: the first cast left no record, no cooldown and no half-state behind, so the second goes
// out as a normal held cast: released after 0.2 s it sends its scatter and its thirty marks.
void test_review_scatter_shot_again_after_an_escape(Fixture& f) {
    std::printf("a Scatter Shot cast again after an Escape goes out as any other\n");
    const abilities::HitDef* ground = hit_named(f, "Ranger_Scattershot_GroundFX_ImpactHit");
    CHECK(ground != nullptr, "the ground impact hit is in the data");
    if (!ground) return;
    constexpr double t0 = 1.0;
    HeldShot h(f, t0);
    CHECKF(h.ok() && h.accepted, "the held Scatter Shot is cast (%s)", h.g.rig.problem.c_str());
    if (!h.ok() || !h.accepted) return;
    h.g.rig.run_to(t0 + 0.3);
    const int32_t instance = ability_instance(h.g.rig, h.g.rig.ability("Ranger_ScatterShot"));
    CHECKF(instance >= 0, "the held cast's ability item is on the pawn (%d)", instance);
    if (instance < 0) return;
    CHECK(h.g.rig.pipe->end_request(instance, t0 + 0.3), "the client ends the held cast");
    h.g.rig.run_to(t0 + 0.6);
    const int32_t second = h.g.rig.next_client_id;
    CHECK(h.g.rig.trigger("Ranger_ScatterShot", t0 + 0.6, false) == Refusal::None, "the Scatter Shot is cast again");
    h.g.rig.run_to(t0 + 0.8);
    CHECK(h.g.rig.pipe->cancel(second, t0 + 0.8, 0.2), "the key goes up 0.2 s after the second trigger");
    h.g.rig.run_to(t0 + 2.2);
    CHECKF(h.g.lost({h.wolf}) > 0.0, "the second cast hurts the Wolf (%.1f)", h.g.lost({h.wolf}));
    CHECKF(marks_of(h.g.rig, ground->row.guid).size() == 30, "...and sends its thirty marks, no more (%zu)", marks_of(h.g.rig, ground->row.guid).size());
}

// A held cast the key has let go of has gone out, and so has one whose charge is full: the client's end (Escape) changes nothing of either.
void test_review_released_and_charged_casts_are_left_alone(Fixture& f) {
    std::printf("an Escape after the key's release, or after the full charge, leaves the shot to go out\n");
    const abilities::HitDef* charged = hit_named(f, "Ranger_Scattershot_Charged");
    const int64_t branch = ability_rig::record_guid(real_db_path(), "AnimBranchRecordBase", "Ranger_Passive_ScatterShot_Concentrated");
    CHECK(charged != nullptr && branch != 0, "the charged hit and the full charge's branch are in the data");
    if (!charged || branch == 0) return;
    constexpr double t0 = 1.0;
    {
        HeldShot h(f, t0);
        CHECKF(h.ok() && h.accepted, "the released Scatter Shot is cast (%s)", h.g.rig.problem.c_str());
        if (!h.ok() || !h.accepted) return;
        h.g.rig.run_to(t0 + 0.4);
        CHECK(h.g.rig.pipe->cancel(h.client_id, t0 + 0.4, 0.2), "the key goes up 0.4 s after the trigger");
        h.g.rig.run_to(t0 + 0.45);
        const int32_t instance = ability_instance(h.g.rig, h.g.rig.ability("Ranger_ScatterShot"));
        CHECKF(instance >= 0, "the cast's ability item is on the pawn (%d)", instance);
        if (instance < 0) return;
        CHECK(h.g.rig.pipe->end_request(instance, t0 + 0.45), "the client ends it 50 ms after the release");
        h.g.rig.run_to(t0 + 1.5);
        CHECKF(h.g.lost({h.wolf}) > 0.0, "the released cast's scatter is still sent: the Wolf takes damage (%.1f)", h.g.lost({h.wolf}));
    }
    {
        HeldShot h(f, t0);
        CHECKF(h.ok() && h.accepted, "the charging Scatter Shot is cast (%s)", h.g.rig.problem.c_str());
        if (!h.ok() || !h.accepted) return;
        h.g.rig.run_to(t0 + 1.1);
        CHECK(h.g.rig.pipe->branch_request(h.client_id, branch, t0 + 1.11), "the client asks for the full charge's branch");
        const int32_t instance = ability_instance(h.g.rig, h.g.rig.ability("Ranger_ScatterShot"));
        CHECKF(instance >= 0, "the cast's ability item is on the pawn (%d)", instance);
        if (instance < 0) return;
        h.g.rig.run_to(t0 + 1.13);
        CHECK(h.g.rig.pipe->end_request(instance, t0 + 1.13), "the client ends it before the charged shot's jump");
        h.g.rig.run_to(t0 + 2.2);
        CHECKF(hits_on(h.g.rig, charged->row.guid, h.wolf) >= 1, "the charged shot still lands on the Wolf (%zu)", hits_on(h.g.rig, charged->row.guid, h.wolf));
    }
}

// ---- Air Strike ------------------------------------------------------------------------------------------------------------------------------------

// Two Rangers leap at the same moment over one Wolf that stands inside the areas of both: the Wolf's cooldown counts every caster's effect (the data's count
// names no source), so the first area to land hurts it and the others skip it for 0.5 s, whichever Ranger's they are.
void test_review_two_air_strikes_over_one_wolf(Fixture& f) {
    std::printf("two Rangers' Air Strikes over one Wolf: its cooldown lets one area hurt it\n");
    const abilities::HitDef* aoe = hit_named(f, "Ranger_AirStrike_AoE");
    const std::string wolf_path = wolf_archetype(f);
    CHECK(aoe != nullptr && !wolf_path.empty(), "the area hit and the Wolf are in the data");
    if (!aoe || wolf_path.empty()) return;
    TwoRangers s(f);
    CHECKF(s.ok(), "the two Rangers and their world: %s%s%s", s.a.problem.c_str(), s.b.problem.c_str(), s.w.problem.c_str());
    if (!s.ok()) return;
    s.a.asks = false;
    s.b.asks = false;
    const uint64_t wolf = s.w.spawn(wolf_path, 300, 200, 25, 100);
    CHECK(wolf != 0, "a Wolf 360 cm from each Ranger, inside the areas of both");
    if (wolf == 0) return;
    constexpr double t0 = 1.0;
    hitchecks::set_evasion(f, s.world, wolf, -5.0, t0 - 1.0);
    s.a.know({"Ranger_AirStrike"});
    s.b.know({"Ranger_AirStrike"});
    CHECK(s.a.trigger("Ranger_AirStrike", t0, false) == Refusal::None, "the first Ranger's Air Strike is cast");
    CHECK(s.b.trigger("Ranger_AirStrike", t0, false) == Refusal::None, "the second Ranger's Air Strike is cast");
    s.a.run_to(t0 + 2.5);
    s.b.run_to(t0 + 2.5);
    const size_t by_a = hits_on(s.a, aoe->row.guid, wolf), by_b = hits_on(s.b, aoe->row.guid, wolf);
    CHECKF(by_a + by_b == 1, "the Wolf is hurt by one area of the six (%zu of the first Ranger's, %zu of the second's)", by_a, by_b);
    CHECKF(s.world.max_health_of(wolf) - s.world.health_of(wolf) > 0.0, "...and it has lost Health (%.1f)", s.world.max_health_of(wolf) - s.world.health_of(wolf));
}

// A caster who dies between two areas lands no more of them: the Wolf keeps the one hit of the area that landed before the death.
void test_review_air_strike_caster_dies_between_areas(Fixture& f) {
    std::printf("a caster who dies between two Air Strike areas lands no more of them\n");
    const abilities::HitDef* aoe = hit_named(f, "Ranger_AirStrike_AoE");
    const abilities::HitDef* arrow = hit_named(f, "Ranger_AirStrike");
    CHECK(aoe != nullptr && arrow != nullptr, "the area hit and the arrow's hit are in the data");
    if (!aoe || !arrow) return;
    Forge g(f, 1, {});
    CHECKF(g.ok(), "the Ranger and the world: %s%s", g.rig.problem.c_str(), g.w.problem.c_str());
    if (!g.ok()) return;
    g.rig.asks = false;
    const uint64_t wolf = g.wolf(0, 300);
    CHECK(wolf != 0, "a Wolf beside the Ranger, in all three landing areas");
    if (wolf == 0) return;
    constexpr double t0 = 1.0;
    hitchecks::set_evasion(f, g.world, wolf, -5.0, t0 - 1.0);
    g.rig.know({"Ranger_AirStrike"});
    double ground_z = 1000.0;
    CHECK(g.world.ground_height(0, 0, 1098, &ground_z), "the world knows the ground under the caster");
    CHECK(g.rig.trigger("Ranger_AirStrike", t0, false) == Refusal::None, "the Air Strike is cast");
    g.rig.run_to(first_area_at(t0, ground_z) + 0.02);
    CHECKF(hits_on(g.rig, aoe->row.guid, wolf) == 1, "the first area hurts the Wolf (%zu)", hits_on(g.rig, aoe->row.guid, wolf));
    const size_t arrows = marks_of(g.rig, arrow->row.guid).size();
    CHECKF(arrows >= 1, "the first arrow has landed (%zu)", arrows);
    g.rig.pipe->set_dead(true);
    g.rig.run_to(t0 + 2.5);
    CHECKF(marks_of(g.rig, arrow->row.guid).size() == arrows, "no other arrow lands after the death (%zu)", marks_of(g.rig, arrow->row.guid).size());
    CHECKF(hits_on(g.rig, aoe->row.guid, wolf) == 1, "the Wolf keeps its one hit (%zu)", hits_on(g.rig, aoe->row.guid, wolf));
}

AOC_REGISTER_TEST(test_review_refused_press_leaves_the_held_cast);
AOC_REGISTER_TEST(test_review_held_tap_sends_the_marks);
AOC_REGISTER_TEST(test_review_marks_die_with_the_caster);
AOC_REGISTER_TEST(test_review_marks_under_a_caster_who_stands_elsewhere);
AOC_REGISTER_TEST(test_review_scatter_shot_again_after_an_escape);
AOC_REGISTER_TEST(test_review_released_and_charged_casts_are_left_alone);
AOC_REGISTER_TEST(test_review_two_air_strikes_over_one_wolf);
AOC_REGISTER_TEST(test_review_air_strike_caster_dies_between_areas);

}  // namespace three_fixes_review
