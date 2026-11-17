// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_stealth_tests.inc
//
//  Registered tests of test_worldd_combat: the game's stealth rule in a creature's sight and aggro.  A stealth effect's rule reads the character's
//  StealthLevel and the creature's StealthPerceptionLevel (both the character's level from the CombatStats), the distance and the creature's angle:
//  a camouflaged character is noticed at least as "detected" (its own effect's rule), and only a "revealed" one is a target; the others are Hidden.
//  A creature already fighting a player loses him as a target when he stealths out of its reveal range, and keeps him inside it.
//  (every test registers itself, see tests/README.md)
// ============================================================================

// This program's CHECK counts a passing check without printing it; these tests print it too, so the run's log names the check that passed (a gate line).
#undef CHECK
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); ++g_pass; } \
    else { std::printf("  [FAIL] %s\n", msg); ++g_fail; } \
} while (0)

namespace {

/// A Wolf at the origin (facing +X), a player of the rig's level 20 at `player_pos`, and the world between them.  `stealth` is what the player carries;
/// an empty active state is a player that does not stealth at all.
struct StealthScene {
    ability_rig::Rig            rig;
    combat_rig::World           w;
    combat::NpcStatBook         book;
    combat::CombatWorld         world;
    combat_rig::FakePlayer      player;
    npc::SensesTable            senses;         // the data's own (npc_senses.tsv), when the scene asks for them
    bool                        senses_ok = true;
    uint64_t                    wolf = 0;
    double                      now = 0;

    /// `real_senses`: the Wolf has the senses its Blueprint gives it (hearing, sight, their vertical limits, its aggro radius and the level's curve), as in the game; else it notices by
    /// the plane distance within `aggro_cm`.
    StealthScene(Fixture& f, int wolf_level, double aggro_cm, combat::Vec3 player_pos, combat::StealthState stealth, bool real_senses = false)
        : rig(1, 20, {}), book(*f.ctx->bundle, *f.ctx->gear),
          world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion), player(rig, 77, player_pos) {
        if (!rig.ok() || !w.problem.empty()) return;
        player.stealth_state = stealth;
        world.add_player(&player);
        if (real_senses) {
            std::string dir = NPC_MOTION_PATH;
            dir.erase(dir.find_last_of("/\\") + 1);
            std::string error;
            senses_ok = senses.load(dir + "npc_senses.tsv", dir + "npc_aggro_curves.tsv", error);
            if (senses_ok) world.set_senses_data(&senses);
        }
        wolf = w.spawn(wolf_archetype(f), 0, 0, wolf_level, aggro_cm);
        if (wolf != 0) w.registry.modify(wolf, ::aoc::worldd::world::Change::State, [](::aoc::worldd::world::WorldActor& a) { a.yaw_degrees = 0; });   // facing +X: the player at +x is in front
    }
    bool ok() const { return wolf != 0 && rig.ok() && w.problem.empty() && senses_ok; }
    /// The Wolf's own height: a player standing over or under it.
    double wolf_z() const {
        const world::WorldActor* a = w.registry.find(wolf);
        return a ? a->z : 0.0;
    }
    /// Runs the world at the rig's think rate; the player's feet are where the test put him.
    void run(double seconds) {
        const double step = 1.0 / 15.0;
        const double end = now + seconds;
        while (now < end) {
            now += step;
            w.npcs->note_player(77, player.rig_fighter().position().x, player.rig_fighter().position().y, now);
            w.npcs->tick(now);
            world.tick(now);
        }
    }
    bool fighting() const {
        const auto st = world.ai_state(wolf);
        return st && (*st == npc::ai::State::Chase || *st == npc::ai::State::Attack);
    }
    bool in_combat() const {
        const world::WorldActor* a = w.registry.find(wolf);
        return a && combat_rig::combat_flag(*a, "bInCombat");
    }
    uint64_t target() const {
        const world::WorldActor* a = w.registry.find(wolf);
        return a ? combat_rig::active_target(*a) : 0;
    }
};

constexpr combat::Vec3 at(double x, double y) { return {x, y, 1098.0}; }

combat::StealthState camouflaged(double stealth_level) {
    combat::StealthState s;
    s.active = true;
    s.camouflage = true;
    s.level = stealth_level;
    s.perception = 20.0;
    return s;
}

combat::StealthState stealthy(double stealth_level) {
    combat::StealthState s;
    s.active = true;
    s.level = stealth_level;
    s.perception = 20.0;
    return s;
}

/// A creature in front of a camouflaged player at 5 m notices him (revealed); behind him at 5 m it does not (detected only), and neither at 20 m nor 40 m.
void test_camouflage_position(Fixture& f) {
    std::printf("stealth sight: a camouflaged player, in front and behind at 5 m, and at 20 m and 40 m\n");
    const combat::StealthState camo = camouflaged(15.0);   // a level-20 Ranger: StealthLevel 20 less the effect's 5
    {
        StealthScene s(f, 20, 800, at(500, 0), camo);
        if (!s.ok()) { CHECK(false, "the scene builds"); return; }
        s.run(0.6);
        CHECK(s.fighting() && s.in_combat() && s.target() == 77, "in front at 5 m (level 20 against stealth 15): it notices and fights");
    }
    {
        StealthScene s(f, 20, 800, at(-500, 0), camo);
        if (!s.ok()) { CHECK(false, "the scene builds"); return; }
        s.run(0.6);
        CHECK(!s.fighting() && !s.in_combat() && s.target() == 0, "behind at 5 m: it does not notice him (Camouflage is detected, not revealed)");
    }
    {
        StealthScene s(f, 20, 3000, at(2000, 0), camo);
        if (!s.ok()) { CHECK(false, "the scene builds"); return; }
        s.run(0.6);
        CHECK(!s.fighting() && s.target() == 0, "in front at 20 m: detected, not revealed, so not a target");
    }
    {
        StealthScene s(f, 20, 5000, at(4000, 0), camo);
        if (!s.ok()) { CHECK(false, "the scene builds"); return; }
        s.run(0.6);
        CHECK(!s.fighting() && s.target() == 0, "in front at 40 m: neither detected nor revealed by distance");
    }
    {
        // two creatures: the one in front at 5 m sees him, the one with the player 5 m behind it does not
        StealthScene s(f, 20, 800, at(500, 0), camo);
        if (!s.ok()) { CHECK(false, "the scene builds"); return; }
        const uint64_t behind = s.w.spawn(wolf_archetype(f), 1000, 0, 20, 800);
        if (behind == 0) { CHECK(false, "the second Wolf is made"); return; }
        s.w.registry.modify(behind, world::Change::State, [](world::WorldActor& a) { a.yaw_degrees = 0; });
        s.run(0.6);
        const auto st = s.world.ai_state(behind);
        CHECK(s.target() == 77 && (!st || *st == npc::ai::State::Idle), "two creatures: the one in front is in the fight, the one behind him is not");
    }
}

/// Without Camouflage the rule is the distance alone: revealed inside 7.5 m in front, hidden at 20 m and 40 m and behind at 5 m.
void test_stealth_distance(Fixture& f) {
    std::printf("stealth sight: a stealthed player (no Camouflage) by distance\n");
    const combat::StealthState stealth = stealthy(20.0);
    {
        StealthScene s(f, 20, 800, at(500, 0), stealth);
        if (!s.ok()) { CHECK(false, "the scene builds"); return; }
        s.run(0.6);
        CHECK(s.fighting() && s.in_combat() && s.target() == 77, "in front at 5 m: revealed and fought (detection 15 m, reveal 7.5 m at equal levels)");
    }
    {
        StealthScene s(f, 20, 3000, at(2000, 0), stealth);
        if (!s.ok()) { CHECK(false, "the scene builds"); return; }
        s.run(0.6);
        CHECK(!s.fighting() && s.target() == 0, "in front at 20 m: hidden (the detection range is 15 m)");
    }
    {
        StealthScene s(f, 30, 5000, at(2000, 0), stealth);
        if (!s.ok()) { CHECK(false, "the scene builds"); return; }
        s.run(0.6);
        CHECK(!s.fighting() && s.target() == 0, "a level-30 creature at 20 m: detected (its 10 points of perception) but not revealed");
    }
    {
        StealthScene s(f, 20, 5000, at(4000, 0), stealth);
        if (!s.ok()) { CHECK(false, "the scene builds"); return; }
        s.run(0.6);
        CHECK(!s.fighting() && s.target() == 0, "at 40 m: hidden");
    }
    {
        StealthScene s(f, 20, 800, at(-500, 0), stealth);
        if (!s.ok()) { CHECK(false, "the scene builds"); return; }
        s.run(0.6);
        CHECK(!s.fighting() && s.target() == 0, "behind at 5 m: hidden (the hearing range is 0 cm at equal levels)");
    }
}

/// A creature of a lower level than the stealth: a level-10 Wolf cannot see a level-20 stealthed player at 5 m, a level-15 one cannot see him at 5 m either;
/// a higher one (25) sees a camouflaged player in front at 5 m and not at 20 m.
void test_stealth_levels(Fixture& f) {
    std::printf("stealth sight: a creature of a higher and of a lower level\n");
    {
        StealthScene s(f, 10, 800, at(500, 0), camouflaged(15.0));
        if (!s.ok()) { CHECK(false, "the scene builds"); return; }
        s.run(0.6);
        CHECK(!s.fighting() && s.target() == 0, "a level-10 creature against a level-20 camouflaged player at 5 m: detection 10 m, reveal 2.5 m, so detected only");
    }
    {
        StealthScene s(f, 25, 800, at(500, 0), camouflaged(15.0));
        if (!s.ok()) { CHECK(false, "the scene builds"); return; }
        s.run(0.6);
        CHECK(s.fighting() && s.target() == 77, "a level-25 creature against him at 5 m: revealed (detection and reveal both hold) and fought");
    }
    {
        StealthScene s(f, 25, 3000, at(2000, 0), camouflaged(15.0));
        if (!s.ok()) { CHECK(false, "the scene builds"); return; }
        s.run(0.6);
        CHECK(!s.fighting() && s.target() == 0, "the same creature at 20 m: detected, not revealed");
    }
    {
        StealthScene s(f, 15, 800, at(500, 0), stealthy(20.0));
        if (!s.ok()) { CHECK(false, "the scene builds"); return; }
        s.run(0.6);
        CHECK(!s.fighting() && s.target() == 0, "a level-15 creature against a level-20 stealth at 5 m: detected, not revealed");
    }
    {
        StealthScene s(f, 15, 800, at(200, 0), stealthy(20.0));
        if (!s.ok()) { CHECK(false, "the scene builds"); return; }
        s.run(0.6);
        CHECK(s.fighting() && s.target() == 77, "...and at 2 m it is revealed and fought");
    }
}

/// A creature already fighting loses a camouflaged player who goes behind it out of the reveal range, and keeps one who stays in front close by.
void test_fight_loses_and_keeps_a_stealthed_player(Fixture& f) {
    std::printf("stealth sight: a creature already fighting, when the player stealths behind and in front\n");
    {
        StealthScene s(f, 20, 2000, at(500, 0), {});
        if (!s.ok()) { CHECK(false, "the scene builds"); return; }
        s.run(0.6);
        CHECK(s.fighting() && s.target() == 77, "the player is fought before he stealths");
        s.player.stealth_state = camouflaged(15.0);
        s.player.rig_fighter().move_to(at(-900, 0));
        s.run(0.5);
        CHECK(s.target() == 0 && !s.in_combat() && !s.fighting(), "he stealths behind it: the creature loses him as a target and leaves the fight");
    }
    {
        StealthScene s(f, 20, 2000, at(500, 0), {});
        if (!s.ok()) { CHECK(false, "the scene builds"); return; }
        s.run(0.6);
        CHECK(s.fighting() && s.target() == 77, "the player is fought before he stealths");
        s.player.stealth_state = camouflaged(15.0);
        s.run(0.5);
        CHECK(s.target() == 77 && s.in_combat(), "he stealths at 5 m in front: revealed, so the fight goes on");
    }
}

/// A damage tick and a direct hit end the stealth of a character, as the data's Incoming reaction and the Rogue's 5 % fragility say; a miss or a Mana loss does not.
void test_stealth_breaks_on_damage(Fixture& f) {
    std::printf("stealth damage: a hit and a damage tick end Camouflage, a miss does not; the Rogue's stealth breaks above 5 %% of MaxHealth\n");
    {
        ability_rig::Rig rig(1, 25);   // a level-25 Ranger
        if (!rig.ok()) { CHECK(false, "the Ranger rig builds"); return; }
        const int64_t camo = rig.effect("Ranger_Camouflage");
        CHECK(rig.pipe->grant_effect(camo, 2.0), "Camouflage is put on");
        rig.take();
        CHECK(rig.pawn.effect(camo) != nullptr, "(the effect is on the character)");
        combat::IncomingHit miss;
        miss.flags = stats::kHitMiss;
        miss.mods.push_back({1, content::kGuidHealth, -50.0});
        rig.pipe->receive_hit(miss, 2.001);
        rig.take();
        CHECK(rig.pawn.effect(camo) != nullptr, "a miss does not end it");
        combat::IncomingHit mana;
        mana.mods.push_back({2, content::kGuidMana, -50.0});
        rig.pipe->receive_hit(mana, 2.002);
        rig.take();
        CHECK(rig.pawn.effect(camo) != nullptr, "a Mana loss does not end it (the reaction asks for a negative Health)");
        combat::IncomingHit tick;
        tick.source_type = content::kSourceTypeEffect;   // a damage tick of an effect on him
        tick.hit_guid = 109234434627195;
        tick.mods.push_back({3, content::kGuidHealth, -1.0});
        rig.pipe->receive_hit(tick, 2.003);
        rig.take();
        CHECK(rig.pawn.effect(camo) == nullptr, "a damage tick ends it");
    }
    {
        ability_rig::Rig rig(5, 25);   // a level-25 Rogue
        if (!rig.ok()) { CHECK(false, "the Rogue rig builds"); return; }
        const int64_t stealth = rig.effect("Rogue_Stealth");
        CHECK(rig.pipe->grant_effect(stealth, 2.0), "Rogue_Stealth is put on");
        rig.take();
        const double max = rig.cs->find(content::kGuidMaxHealth)->value;
        combat::IncomingHit small;
        small.mods.push_back({4, content::kGuidHealth, -max * 0.04});
        rig.pipe->receive_hit(small, 2.001);
        rig.take();
        CHECK(rig.pawn.effect(stealth) != nullptr, "4 % of MaxHealth does not break it (the threshold is 5 %)");
        combat::IncomingHit more;
        more.mods.push_back({5, content::kGuidHealth, -max * 0.02});
        rig.pipe->receive_hit(more, 2.002);
        rig.take();
        CHECK(rig.pawn.effect(stealth) == nullptr, "2 % more (6 % together) breaks it");
    }
}

/// What the session reads of a player: the effects he holds and the two stats (session/stealth_state.h), the numbers the creatures' sight compares.
void test_the_state_the_session_reads(Fixture&) {
    std::printf("stealth state: the effects a player holds and his StealthLevel, as the session hands them to the creatures\n");
    {
        ability_rig::Rig rig(1, 20);   // a level-20 Ranger
        if (!rig.ok()) { CHECK(false, "the Ranger rig builds"); return; }
        const combat::StealthState none = stealth_state_of(&*rig.pipe, &*rig.cs);
        CHECK(!none.active && !none.camouflage && none.level == 20.0 && none.perception == 20.0, "before it: not stealthed, StealthLevel and StealthPerceptionLevel are his level, 20");
        CHECK(rig.pipe->grant_effect(rig.effect("Ranger_Camouflage"), 2.0), "Camouflage is put on");
        rig.take();
        const combat::StealthState camo = stealth_state_of(&*rig.pipe, &*rig.cs);
        CHECK(camo.active && camo.camouflage && camo.level == 15.0 && camo.perception == 20.0, "Camouflage: stealthed, camouflaged, StealthLevel 15 (his 20 less the effect's 5), perception unchanged");
        CHECK(rig.pipe->remove_effect(rig.effect("Ranger_Camouflage"), 3.0), "Camouflage is taken off");
        rig.take();
        const combat::StealthState off = stealth_state_of(&*rig.pipe, &*rig.cs);
        CHECK(!off.active && off.level == 20.0, "after it: not stealthed, StealthLevel back to 20");
    }
    {
        ability_rig::Rig rig(5, 20);   // a level-20 Rogue
        if (!rig.ok()) { CHECK(false, "the Rogue rig builds"); return; }
        CHECK(rig.pipe->grant_effect(rig.effect("Rogue_Stealth"), 2.0), "Rogue_Stealth is put on");
        rig.take();
        const combat::StealthState st = stealth_state_of(&*rig.pipe, &*rig.cs);
        CHECK(st.active && !st.camouflage && st.level == 20.0, "Rogue_Stealth: stealthed, not camouflaged, StealthLevel his 20");
    }
    {
        ability_rig::Rig rig(6, 20);   // a level-20 Summoner (Phase Out is its escape)
        if (!rig.ok()) { CHECK(false, "the Summoner rig builds"); return; }
        CHECK(rig.pipe->grant_effect(rig.effect("Summoner_PhaseOut"), 2.0), "Summoner_PhaseOut is put on");
        rig.take();
        const combat::StealthState st = stealth_state_of(&*rig.pipe, &*rig.cs);
        CHECK(st.active && !st.camouflage && st.level == 20.0, "Summoner_PhaseOut: stealthed, not camouflaged");
    }
}

/// A creature with the senses its Blueprint gives it notices a stealthed player only when it would notice him anyway (it hears or sees him: its radii and its vertical limits) and the
/// game's stealth rule reveals him: the rule is a filter on its senses, not a replacement of them (a creature on a ledge above its vertical limits does not see him through the rule).
void test_stealth_filters_the_senses(Fixture& f) {
    std::printf("stealth sight: with the creature's real senses, a stealthed player is noticed only when its senses would notice him and the rule reveals him\n");
    const auto camouflaged_scene = [&](combat::Vec3 at_pos, bool stealthed) -> std::unique_ptr<StealthScene> {
        auto s = std::make_unique<StealthScene>(f, 20, 2000, at_pos, combat::StealthState{}, /*real_senses=*/true);   // (the Wolf's own aggro radius, 2,000 cm)
        if (!s->ok()) return nullptr;
        if (stealthed) {
            s->player.real_stealth = true;   // the effect on the rig's pipeline, the numbers from its stats
            if (!s->rig.pipe->grant_effect(s->rig.effect("Ranger_Camouflage"), 2.0)) return nullptr;
            s->rig.take();
        }
        const double z = s->wolf_z();
        s->player.rig_fighter().move_to({at_pos.x, at_pos.y, z + at_pos.z});   // (z: the height over the Wolf)
        return s;
    };
    {
        auto s = camouflaged_scene({500, 0, 0}, false);
        if (!s) { CHECK(false, "the scene builds"); return; }
        s->run(0.6);
        CHECK(s->fighting() && s->target() == 77, "a player who is not stealthed, 5 m in front, level against level: the Wolf hears him and fights (its real senses)");
    }
    {
        auto s = camouflaged_scene({500, 0, 0}, true);
        if (!s) { CHECK(false, "the scene builds"); return; }
        s->run(0.6);
        CHECK(s->fighting() && s->target() == 77, "the same with Camouflage on (the effect and the stat -5 from the rig): revealed in front, so it fights");
    }
    {
        auto s = camouflaged_scene({-500, 0, 0}, true);
        if (!s) { CHECK(false, "the scene builds"); return; }
        s->run(0.6);
        CHECK(!s->fighting() && s->target() == 0, "5 m behind it with Camouflage: it hears him and the rule does not reveal him, so it does not fight");
    }
    {
        auto s = camouflaged_scene({500, 0, 800}, false);
        if (!s) { CHECK(false, "the scene builds"); return; }
        s->run(0.6);
        CHECK(!s->fighting() && s->target() == 0, "a player not stealthed 5 m in front and 8 m above it: over its vertical limits, it does not notice him");
    }
    {
        auto s = camouflaged_scene({500, 0, 800}, true);
        if (!s) { CHECK(false, "the scene builds"); return; }
        s->run(0.6);
        CHECK(!s->fighting() && s->target() == 0, "...and with Camouflage on he is not noticed through the rule either (the rule filters its senses, it does not replace them)");
    }
}

}  // namespace

AOC_REGISTER_TEST(test_camouflage_position);
AOC_REGISTER_TEST(test_stealth_distance);
AOC_REGISTER_TEST(test_stealth_levels);
AOC_REGISTER_TEST(test_fight_loses_and_keeps_a_stealthed_player);
AOC_REGISTER_TEST(test_stealth_breaks_on_damage);
AOC_REGISTER_TEST(test_the_state_the_session_reads);
AOC_REGISTER_TEST(test_stealth_filters_the_senses);
