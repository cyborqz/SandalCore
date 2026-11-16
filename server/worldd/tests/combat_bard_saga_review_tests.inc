// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_bard_saga_review_tests.inc: the Bard's Saga's other hits (included from combat_registered.inc inside test_worldd_combat's anonymous
//  namespace, after combat_bard_saga_tests.inc, whose Band and helpers it uses)
//
//    the enemy hits   the hits of the Apocalypse and of the Horrors of the Void land on the creatures the client's request names, as every hit of a
//                     cast does; the Saga's ally selection is for its ally hits alone
//    the themes       the Saga's theme-consuming hit takes the notes off the Bard who cast it, and off nobody else of his party
// ============================================================================

namespace sagareview {

using namespace aoc::worldd;

/// The targeting record of the Saga named `name` (0 when the ability lists none).
int64_t targeting_named(Rig& r, const char* name) {
    const abilities::AbilityDef* def = r.catalog->ability_named("Bard_Saga");
    if (!def) return 0;
    for (const content::TargetingRow& t : def->targetings) {
        if (t.name == name) return t.guid;
    }
    return 0;
}

/// The client's request for the Saga's hit `hit` at `t`, naming `targets` (the creatures its own area found; none: the caster's own component alone) and the record `targeting`.
AbilityPipeline::HitVerdict ask(Rig& r, const char* hit, const char* targeting, double t, const std::vector<uint64_t>& targets = {}) {
    const abilities::HitDef* h = r.catalog->hit_named(hit);
    if (!h) return AbilityPipeline::HitVerdict::NotItsHit;
    r.run_to(t);
    const int64_t tg = std::string(targeting) == "Default_Self" ? content::kTargetingDefaultSelf : targeting_named(r, targeting);
    return r.pipe->hit_request(r.ability("Bard_Saga"), tg, h->row.guid, 1, targets.empty() ? 1 : 0, t, targets);
}

/// One enemy of the Bard's Saga and the Saga's casts of one variant against it: the notes, the cast, the request of `hit` naming the creature, and the creature's Health before and after.
struct Enemy {
    size_t told = 0;         // the confirmations of the hit that name the creature
    double damage = 0;       // the Health the confirmed hit took off it (0: no mod was told)
    bool   ran = false;
};

Enemy cast_at_a_wolf(Fixture& f, const char* what, const std::vector<const char*>& stories, bool volume_two, const char* hit, const char* targeting) {
    Enemy out;
    saga::Band b(f, {}, true, volume_two ? std::vector<std::string>{"Bard_Passive_Saga_Vol2"} : std::vector<std::string>());
    CHECKF(b.ok(), "%s: the world (%s)", what, b.problem.c_str());
    if (!b.ok()) return out;
    const std::string wolf_path = creature_path(f, "/Beast/Canine/Wolf.Default__Wolf_C");
    const uint64_t wolf = wolf_path.empty() ? 0 : b.w.spawn(wolf_path, 400.0, 0.0, 10);
    CHECKF(wolf != 0, "%s: a Wolf stands 4 m from the Bard", what);
    if (wolf == 0) return out;
    Rig& r = b.caster;
    double t = 5.0;
    bool notes = true;
    for (const char* story : stories) {
        notes = notes && saga::story_note(r, story, t);
        t += 3.0;
    }
    CHECKF(notes, "%s: the notes land", what);
    r.asks = false;
    r.know({"Bard_Saga"});
    const double t0 = 20.0;
    CHECKF(r.trigger("Bard_Saga", t0) == Refusal::None, "%s: the Saga is cast", what);
    const AbilityPipeline::HitVerdict verdict = ask(r, hit, targeting, t0 + 0.6, {wolf});
    CHECKF(verdict == AbilityPipeline::HitVerdict::Scheduled, "%s: the request for %s is taken", what, hit);
    b.run_to(t0 + 2.0);
    const abilities::HitDef* h = r.catalog->hit_named(hit);
    for (const bootstrap::HitConfirm* c : saga::confirms_of(r, h ? h->row.guid : 0)) {
        for (const bootstrap::HitTargetOut& tg : c->targets) {
            if (tg.component.ObjectId != wolf) continue;
            ++out.told;
            for (const bootstrap::HitStatModOut& m : tg.mods) out.damage += m.value < 0 ? -m.value : 0.0;
        }
    }
    out.ran = true;
    return out;
}

void test_saga_review_the_enemy_hits_land(Fixture& f) {
    std::printf("the Saga's hits at enemies (the Apocalypse's, the Horrors of the Void's) land on the creatures the client's request names\n");
    {
        const Enemy e = cast_at_a_wolf(f, "three Tragedy (The Apocalypse)", {"Bard_Story_Conflict", "Bard_Story_Conflict", "Bard_Story_Conflict"}, false, "Bard_Saga_Apoc_InitialHit", "Bard_Saga_FlameAndFang");
        CHECKF(e.told >= 1 && e.damage > 0.0, "the Apocalypse's first hit hurts the Wolf and the client is told (%zu confirmation(s) name it, %.1f damage)", e.told, e.damage);
    }
    {
        const Enemy e = cast_at_a_wolf(f, "two Tragedy and one Mystery with the Volume II passive (The Horrors of the Void)", {"Bard_Story_Conflict", "Bard_Story_Conflict", "Bard_Story_Wonder"}, true,
                                       "Bard_Saga_HorrorsUnknown_Hit", "Bard_Saga_HorrorsUnknown");
        CHECKF(e.told >= 1 && e.damage > 0.0, "the Horrors' hit hurts the Wolf and the client is told (%zu confirmation(s) name it, %.1f damage)", e.told, e.damage);
    }
}
AOC_REGISTER_TEST(test_saga_review_the_enemy_hits_land);

void test_saga_review_the_themes_are_the_casters_own(Fixture& f) {
    std::printf("the Return to Verra takes the notes off the Bard who cast it and off nobody of his party: a second Bard's own notes stay\n");
    saga::Band b(f, {3.0}, /*party=*/true, {}, {"Bard_Passive_Saga_Vol2"});
    CHECKF(b.ok(), "the world (%s)", b.problem.c_str());
    if (!b.ok()) return;
    Rig& first = b.caster;
    Rig& second = *b.allies[0];
    for (int i = 0; i < 3; ++i) CHECKF(saga::story_note(first, "Bard_Story_Joy", 5.0 + 3.0 * static_cast<double>(i)), "(the first Bard's Comedy note %d)", i + 1);
    for (int i = 0; i < 3; ++i) CHECKF(saga::story_note(second, "Bard_Story_Conflict", 6.0 + 3.0 * static_cast<double>(i)), "(the second Bard's Tragedy note %d)", i + 1);
    const int64_t comedy = first.effect("Bard_Note_Comedy"), tragedy = second.effect("Bard_Note_Tragedy");
    CHECKF(first.pipe->stacks_of(comedy) == 3 && second.pipe->stacks_of(tragedy) == 3, "each holds his three notes (%d, %d)", first.pipe->stacks_of(comedy), second.pipe->stacks_of(tragedy));
    first.asks = false;
    first.know({"Bard_Saga"});
    const double t0 = 20.0;
    CHECKF(first.trigger("Bard_Saga", t0) == Refusal::None, "the first Bard casts his Saga");
    // the client's first answer: the theme-consuming hit (named Default_Self) and the first burst (named Joyousness)
    ask(first, "Bard_Saga_ConsumeThemes", "Default_Self", t0 + 0.604);
    ask(first, "Bard_Saga_RTV_AoE_1", "Bard_Saga_Joyousness", t0 + 0.604);
    b.run_to(t0 + 2.0);
    CHECKF(first.pipe->stacks_of(comedy) == 0, "his own three Comedy notes are spent (%d)", first.pipe->stacks_of(comedy));
    CHECKF(second.pipe->stacks_of(tragedy) == 3, "the second Bard's three Tragedy notes are not (%d)", second.pipe->stacks_of(tragedy));
    const int64_t consume = first.catalog->hit_named("Bard_Saga_ConsumeThemes")->row.guid;
    size_t consumed_on_him = 0;
    for (const int64_t g : b.ally_ports[0]->ally_lands) consumed_on_him += g == consume ? 1 : 0;
    CHECKF(consumed_on_him == 0, "and his client is told of no theme-consuming hit (%zu)", consumed_on_him);
    CHECKF(b.ally_ports[0]->ally_removals == 0, "nothing was taken off his character (%zu)", b.ally_ports[0]->ally_removals);
}
AOC_REGISTER_TEST(test_saga_review_the_themes_are_the_casters_own);


/// The Return to Verra cast by a Bard with a player of his party (`party`) or outside it at 10 m, who is hurt: the confirmations of the first burst's landed hit and of the later bursts' that name each.
struct Bursts {
    size_t first_on_caster = 0, first_on_ally = 0, later_on_caster = 0, later_on_ally = 0;
    size_t launchers_first = 0, launchers_later = 0;
    double ally_health_before = 0, ally_health_after = 0;
    bool ran = false;
};

Bursts cast_the_return(Fixture& f, bool party, const char* targeting = "Bard_Saga_Joyousness") {
    Bursts out;
    saga::Band b(f, {10.0}, party);
    CHECKF(b.ok(), "the world (%s)", b.problem.c_str());
    if (!b.ok()) return out;
    Rig& r = b.caster;
    Rig& ally = *b.allies[0];
    for (int i = 0; i < 3; ++i) CHECKF(saga::story_note(r, "Bard_Story_Joy", 5.0 + 3.0 * static_cast<double>(i)), "(Comedy note %d)", i + 1);
    r.set_current(content::kGuidHealth, r.pawn.stat(content::kGuidMaxHealth) - 500.0);
    ally.set_current(content::kGuidHealth, ally.pawn.stat(content::kGuidMaxHealth) - 600.0);
    out.ally_health_before = ally.pawn.stat(content::kGuidHealth);
    r.asks = false;
    r.know({"Bard_Saga"});
    const double t0 = 20.0;
    CHECKF(r.trigger("Bard_Saga", t0) == Refusal::None, "the Saga is cast");
    ask(r, "Bard_Saga_ConsumeThemes", "Default_Self", t0 + 0.604);
    ask(r, "Bard_Saga_RTV_AoE_1", targeting, t0 + 0.604);
    ask(r, "Bard_Saga_RTV_AoE", targeting, t0 + 1.377);
    ask(r, "Bard_Saga_RTV_AoE", targeting, t0 + 2.207);
    b.run_to(t0 + 8.0);
    const int64_t first = r.catalog->hit_named("Bard_Saga_RTV_Hit_1")->row.guid, later = r.catalog->hit_named("Bard_Saga_RTV_Hit")->row.guid;
    const uint64_t me = b.caster_port.actor_id(), him = b.ally_ports[0]->actor_id();
    out.first_on_caster = saga::landed_on(r, first, me, me);
    out.first_on_ally = saga::landed_on(r, first, him, me);
    out.later_on_caster = saga::landed_on(r, later, me, me);
    out.later_on_ally = saga::landed_on(r, later, him, me);
    out.launchers_first = saga::confirms_of(r, r.catalog->hit_named("Bard_Saga_RTV_AoE_1")->row.guid).size();
    out.launchers_later = saga::confirms_of(r, r.catalog->hit_named("Bard_Saga_RTV_AoE")->row.guid).size();
    out.ally_health_after = ally.pawn.stat(content::kGuidHealth);
    out.ran = true;
    return out;
}

void test_saga_review_the_three_bursts(Fixture& f) {
    std::printf("the Return to Verra's three bursts: each of the client's three requests is answered with its launcher's and its landed hits\n");
    const Bursts p = cast_the_return(f, /*party=*/true);
    if (!p.ran) return;
    CHECKF(p.launchers_first == 1, "the first burst's launcher is confirmed once (%zu)", p.launchers_first);
    CHECKF(p.launchers_later == 2, "and the second and third bursts' launchers are confirmed, one each (%zu)", p.launchers_later);
    CHECKF(p.first_on_caster == 1 && p.first_on_ally == 1, "the first burst lands on the Bard and on the party member (%zu, %zu)", p.first_on_caster, p.first_on_ally);
    CHECKF(p.later_on_caster == 2 && p.later_on_ally == 2, "the second and third bursts land on both, once each (%zu, %zu)", p.later_on_caster, p.later_on_ally);
    CHECKF(p.ally_health_after > p.ally_health_before, "and the party member's Health rose (%.1f -> %.1f)", p.ally_health_before, p.ally_health_after);
}
AOC_REGISTER_TEST(test_saga_review_the_three_bursts);

void test_saga_review_a_player_outside_the_party(Fixture& f) {
    std::printf("the Return to Verra heals the Bard and his party, not a hurt player who is not of it\n");
    const Bursts p = cast_the_return(f, /*party=*/false);
    if (!p.ran) return;
    CHECKF(p.first_on_caster >= 1, "the Bard is healed (%zu)", p.first_on_caster);
    CHECKF(p.first_on_ally == 0 && p.later_on_ally == 0, "the player outside the party is not (%zu, %zu)", p.first_on_ally, p.later_on_ally);
}
AOC_REGISTER_TEST(test_saga_review_a_player_outside_the_party);

void test_saga_review_the_return_whatever_record_is_named(Fixture& f) {
    std::printf("the Return to Verra's three bursts do not depend on the record the client names: the cast's own variant picks the allies when a request names the caster alone\n");
    const Bursts p = cast_the_return(f, /*party=*/true, "Default_Self");
    if (!p.ran) return;
    CHECKF(p.launchers_first == 1 && p.launchers_later == 2, "the three launchers are confirmed (%zu, %zu)", p.launchers_first, p.launchers_later);
    CHECKF(p.first_on_caster == 1 && p.first_on_ally == 1 && p.later_on_caster == 2 && p.later_on_ally == 2, "and the three bursts land on the Bard and on the party member (%zu, %zu, %zu, %zu)", p.first_on_caster,
           p.first_on_ally, p.later_on_caster, p.later_on_ally);
}
AOC_REGISTER_TEST(test_saga_review_the_return_whatever_record_is_named);



void test_saga_review_the_eight_are_eight(Fixture& f) {
    std::printf("more than eight allies in range: the burst reaches eight, and an unhurt caster the injured ones fill the places of is not added as a ninth\n");
    std::vector<double> xs;
    for (int m = 4; m <= 36; m += 4) xs.push_back(m);   // nine party members at 4 to 36 m, every one hurt
    saga::Band b(f, xs);
    CHECKF(b.ok(), "the world (%s)", b.problem.c_str());
    if (!b.ok()) return;
    Rig& r = b.caster;
    for (int i = 0; i < 3; ++i) CHECKF(saga::story_note(r, "Bard_Story_Joy", 5.0 + 3.0 * static_cast<double>(i)), "(Comedy note %d)", i + 1);
    for (auto& ally : b.allies) ally->set_current(content::kGuidHealth, ally->pawn.stat(content::kGuidMaxHealth) * 0.5);
    r.asks = false;
    r.know({"Bard_Saga"});
    CHECKF(r.trigger("Bard_Saga", 20.0) == Refusal::None, "the Saga is cast");
    ask(r, "Bard_Saga_RTV_AoE_1", "Bard_Saga_Joyousness", 20.604);
    b.run_to(30.0);
    const auto launchers = saga::confirms_of(r, r.catalog->hit_named("Bard_Saga_RTV_AoE_1")->row.guid);
    CHECKF(launchers.size() == 1, "the first burst's launcher is confirmed (%zu)", launchers.size());
    if (launchers.size() != 1) return;
    CHECKF(launchers[0]->targets.size() == 8, "it names eight targets (%zu)", launchers[0]->targets.size());
    const uint64_t me = b.caster_port.actor_id();
    CHECKF(saga::landed_on(r, r.catalog->hit_named("Bard_Saga_RTV_Hit_1")->row.guid, me, me) == 0, "and the unhurt caster, outside the eight, takes none of the first burst");
}
AOC_REGISTER_TEST(test_saga_review_the_eight_are_eight);

void test_saga_review_the_themes_go_unconfirmed(Fixture& f) {
    std::printf("the Return to Verra's theme-consuming hit takes the notes and is no confirmation to the client, as every hit on the caster alone with nothing to show\n");
    saga::Band b(f, {});
    CHECKF(b.ok(), "the world (%s)", b.problem.c_str());
    if (!b.ok()) return;
    Rig& r = b.caster;
    for (int i = 0; i < 3; ++i) CHECKF(saga::story_note(r, "Bard_Story_Joy", 5.0 + 3.0 * static_cast<double>(i)), "(Comedy note %d)", i + 1);
    r.asks = false;
    r.know({"Bard_Saga"});
    CHECKF(r.trigger("Bard_Saga", 20.0) == Refusal::None, "the Saga is cast");
    ask(r, "Bard_Saga_ConsumeThemes", "Default_Self", 20.604);
    ask(r, "Bard_Saga_RTV_AoE_1", "Bard_Saga_Joyousness", 20.604);
    b.run_to(22.0);
    CHECKF(r.pipe->stacks_of(r.effect("Bard_Note_Comedy")) == 0, "the three Comedy notes are spent (%d)", r.pipe->stacks_of(r.effect("Bard_Note_Comedy")));
    const size_t told = saga::confirms_of(r, r.catalog->hit_named("Bard_Saga_ConsumeThemes")->row.guid).size();
    CHECKF(told == 0, "and the client is told of no theme-consuming hit (%zu)", told);
    CHECKF(saga::confirms_of(r, r.catalog->hit_named("Bard_Saga_RTV_Hit_1")->row.guid).size() == 1, "while the first burst's landed hit is (%zu)",
           saga::confirms_of(r, r.catalog->hit_named("Bard_Saga_RTV_Hit_1")->row.guid).size());
}
AOC_REGISTER_TEST(test_saga_review_the_themes_go_unconfirmed);

void test_saga_review_a_player_stands_once_in_a_zone(Fixture& f) {
    std::printf("a player who stands in a lingering zone is one stander, as the world's registry also holds his pawn: the zone's hits land on him once\n");
    saga::Band b(f, {2.0, 8.0});
    CHECKF(b.ok(), "the world (%s)", b.problem.c_str());
    if (!b.ok()) return;
    // the registry holds every player's pawn as the other players are shown it (ActorKind::Player, the actor id the player's port has); the first ally's id is even
    const uint64_t first = b.ally_ports[0]->actor_id();
    world::WorldActor pawn;
    pawn.id = first;
    pawn.kind = world::ActorKind::Player;
    pawn.actor.guid.ObjectId = first;
    pawn.x = 200.0;
    pawn.y = 0.0;
    pawn.z = saga::kGroundZ;
    CHECKF(first % 2 == 0 && b.w.registry.add(std::move(pawn)), "the first ally's pawn is in the registry (actor %llu)", static_cast<unsigned long long>(first));
    combat::ZoneArea area;
    area.radius = 500.0;
    const std::vector<combat::ZoneTarget> standing = b.world.actors_in_area(b.caster_port.fighter(), combat::Vec3{0, 0, saga::kGroundZ}, area, 16);
    size_t once = 0, caster = 0, far_one = 0;
    for (const combat::ZoneTarget& t : standing) {
        once += t.actor == first ? 1 : 0;
        caster += t.actor == b.caster_port.actor_id() ? 1 : 0;
        far_one += t.actor == b.ally_ports[1]->actor_id() ? 1 : 0;
    }
    CHECKF(once == 1, "the ally 2 m away stands in a zone of 5 m once (%zu)", once);
    CHECKF(caster == 0 && far_one == 0, "and neither the caster (the zone adds him himself) nor the ally 8 m away does (%zu, %zu)", caster, far_one);
    // (and the players are no one's enemies: the areas that gather enemies do not find the ally whose pawn the registry holds)
    CHECKF(b.world.enemies_around(b.caster_port.fighter(), combat::Vec3{0, 0, saga::kGroundZ}, 1000.0, 16).empty(), "no area of enemies finds him (%d)",
           static_cast<int>(b.world.inspect(first, b.caster_port.fighter()).relation));
}
AOC_REGISTER_TEST(test_saga_review_a_player_stands_once_in_a_zone);

void test_saga_review_two_bards_at_once(Fixture& f) {
    std::printf("two Bards of one party cast the Return at the same time: each one's bursts heal the other, and neither's notes are touched by the other's cast\n");
    saga::Band b(f, {3.0});
    CHECKF(b.ok(), "the world (%s)", b.problem.c_str());
    if (!b.ok()) return;
    Rig& first = b.caster;
    Rig& second = *b.allies[0];
    b.caster_port.party_with = b.ally_ports[0]->actor_id();   // (the second Bard sees the first in his party too)
    for (int i = 0; i < 3; ++i) CHECKF(saga::story_note(first, "Bard_Story_Joy", 5.0 + 3.0 * static_cast<double>(i)), "(the first Bard's Comedy note %d)", i + 1);
    for (int i = 0; i < 3; ++i) CHECKF(saga::story_note(second, "Bard_Story_Joy", 5.5 + 3.0 * static_cast<double>(i)), "(the second Bard's Comedy note %d)", i + 1);
    first.set_current(content::kGuidHealth, first.pawn.stat(content::kGuidMaxHealth) - 500.0);
    second.set_current(content::kGuidHealth, second.pawn.stat(content::kGuidMaxHealth) - 500.0);
    first.asks = false;
    second.asks = false;
    first.know({"Bard_Saga"});
    second.know({"Bard_Saga"});
    CHECKF(first.trigger("Bard_Saga", 20.0) == Refusal::None, "the first Bard casts");
    CHECKF(second.trigger("Bard_Saga", 20.1) == Refusal::None, "the second Bard casts a moment after");
    for (const double at : {0.604, 1.377, 2.207}) {
        b.run_to(20.0 + at);
        if (at < 1.0) ask(first, "Bard_Saga_ConsumeThemes", "Default_Self", 20.0 + at);
        ask(first, at < 1.0 ? "Bard_Saga_RTV_AoE_1" : "Bard_Saga_RTV_AoE", "Bard_Saga_Joyousness", 20.0 + at);
        b.run_to(20.1 + at);
        if (at < 1.0) ask(second, "Bard_Saga_ConsumeThemes", "Default_Self", 20.1 + at);
        ask(second, at < 1.0 ? "Bard_Saga_RTV_AoE_1" : "Bard_Saga_RTV_AoE", "Bard_Saga_Joyousness", 20.1 + at);
    }
    b.run_to(30.0);
    const uint64_t one = b.caster_port.actor_id(), two = b.ally_ports[0]->actor_id();
    const int64_t first_hit = first.catalog->hit_named("Bard_Saga_RTV_Hit_1")->row.guid, later_hit = first.catalog->hit_named("Bard_Saga_RTV_Hit")->row.guid;
    // each Bard's confirmations are told to his own client: the first lists the hits that landed on both
    CHECKF(saga::landed_on(first, first_hit, two, one) == 1 && saga::landed_on(first, later_hit, two, one) == 2, "the first Bard's bursts landed on the second (%zu, %zu)",
           saga::landed_on(first, first_hit, two, one), saga::landed_on(first, later_hit, two, one));
    CHECKF(saga::landed_on(second, first_hit, one, two) == 1 && saga::landed_on(second, later_hit, one, two) == 2, "the second Bard's bursts landed on the first (%zu, %zu)",
           saga::landed_on(second, first_hit, one, two), saga::landed_on(second, later_hit, one, two));
    // (a hit on a pawn itself has no target entry: the first Bard's client is told of his own three bursts and of the second Bard's three)
    CHECKF(saga::landed_on(first, first_hit, one, one) == 2 && saga::landed_on(first, later_hit, one, one) == 4, "and the first Bard's client is told of the hits on himself, his own and the second Bard's (%zu, %zu)",
           saga::landed_on(first, first_hit, one, one), saga::landed_on(first, later_hit, one, one));
    CHECKF(first.pipe->stacks_of(first.effect("Bard_Note_Comedy")) == 0 && second.pipe->stacks_of(second.effect("Bard_Note_Comedy")) == 0, "each Bard's own three notes are spent (%d, %d)",
           first.pipe->stacks_of(first.effect("Bard_Note_Comedy")), second.pipe->stacks_of(second.effect("Bard_Note_Comedy")));
    CHECKF(b.ally_ports[0]->ally_removals == 0 && b.ally_ports[0]->ally_lands.size() >= 3, "nothing of the first Bard's cast took anything off the second, who took the first's landed hits (%zu removals, %zu landed)", b.ally_ports[0]->ally_removals,
           b.ally_ports[0]->ally_lands.size());
    CHECK(first.pawn.broken.empty() && second.pawn.broken.empty(), "(both pawns took every change)");
}
AOC_REGISTER_TEST(test_saga_review_two_bards_at_once);

}  // namespace sagareview
