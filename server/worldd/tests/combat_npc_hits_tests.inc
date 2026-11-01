// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_npc_hits_tests.inc
//
//  Registered tests of test_worldd_combat: an NPC's hit on the player: the damage, the damage multiplier, the fallback attack and the combat flag.
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================

void test_npc_hits_player(Fixture& f) {
    std::printf("an NPC's hit on the player: Health through the shields, the confirmation\n");
    Rig rig(4, 20, {});
    if (!rig.ok()) return;
    combat_rig::World w;
    if (!w.problem.empty()) return;
    const std::string wolf_path = wolf_archetype(f);
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion);
    const uint64_t wolf = w.spawn(wolf_path, 300, 0, 10);
    combat_rig::FakePlayer player(rig, 77, {0, 0, 1098});
    world.add_player(&player);
    combat::WorldHitPort port(world, w.registry, player.fighter());
    rig.pipe->set_combat(&port);
    const double max = rig.cs->find(content::kGuidMaxHealth)->value;
    combat::IncomingHit in;
    in.instigator = wolf;
    in.hit_guid = 109234434627193;   // Wolf_Bite_Hit
    in.source_type = content::kSourceTypeAbility;
    in.source_guid = 109184795825810;   // Wolf_Bite
    in.mods.push_back({109234434627194, content::kGuidHealth, -50.0});
    rig.pipe->receive_hit(in, 2.0);
    std::vector<Seen> seen = rig.take();
    CHECK(rig.cs->find(content::kGuidHealth)->value == static_cast<float>(max - 50.0), "Health fell by 50");
    const Seen* conf = nullptr;
    for (const Seen& s : seen) {
        if (!s.confirmations.empty()) conf = &s;
    }
    CHECK(conf && conf->confirmations.size() == 1 && conf->confirmations[0].instigator && conf->confirmations[0].instigator->ObjectId == wolf && conf->confirmations[0].targets.empty() &&
              conf->confirmations[0].mods.size() == 1 && conf->confirmations[0].mods[0].value == -50.0f && conf->confirmations[0].mods[0].effective == -50.0f,
          "the confirmation names the Wolf as the instigator and the pawn as the target, with the value and the effective loss");
    // a miss costs nothing and carries the flag
    const float before_miss = rig.cs->find(content::kGuidHealth)->value;
    in.flags = stats::kHitMiss;
    rig.pipe->receive_hit(in, 2.0001);
    seen = rig.take();
    CHECK(rig.cs->find(content::kGuidHealth)->value >= before_miss, "a miss costs no Health");
    // a killing blow carries Lethal and stops at 0
    in.flags = 0;
    in.mods[0].value = -1.0e9;
    rig.pipe->receive_hit(in, 2.0002);
    seen = rig.take();
    bool lethal = false;
    for (const Seen& s : seen) {
        for (const bootstrap::HitConfirm& c : s.confirmations) lethal = lethal || (c.flags & stats::kHitLethal);
    }
    CHECK(rig.cs->find(content::kGuidHealth)->value == 0.0f && lethal, "a killing blow takes Health to 0 and is flagged Lethal");
    CHECK(!player.rig_fighter().alive(), "the player is dead to the AI");
    // a NaN does nothing
    combat::IncomingHit bad = in;
    bad.mods[0].value = std::nan("");
    rig.pipe->receive_hit(bad, 2.0003);
    rig.take();
    CHECK(rig.pawn.broken.empty(), "(the pawn took every change)");
}

AOC_REGISTER_TEST(test_npc_hits_player);

void test_npc_damage_mult(Fixture& f) {
    std::printf("the NPC damage multiplier: 0.2 takes 80 %% off the Health a creature's hit does to a player\n");
    const double full = wolf_first_hit_health(f, 1.0);
    const double fifth = wolf_first_hit_health(f, 0.2);
    CHECK(std::isfinite(full) && full < 0, "a level 10 Wolf's first hit takes Health at the data's own damage");
    CHECK(std::isfinite(fifth) && std::isfinite(full) && near(fifth, full * 0.2, std::fabs(full) * 1e-6 + 1e-6), "with the multiplier at 0.2 the same hit takes a fifth of it");
    const double none = wolf_first_hit_health(f, 0.0);
    CHECK(std::isfinite(none) && none == 0.0, "with the multiplier at 0 the hit takes nothing");
}

AOC_REGISTER_TEST(test_npc_damage_mult);

void test_fallback_attack(Fixture& f) {
    std::printf("the attack of a creature with no kit of its own: a factor times the shared damage equation, not a flat number\n");
    const combat::CombatWorldConfig cfg;
    const abilities::AbilityDef* fallback = f.catalog->ability_named(cfg.fallback_ability);
    CHECK(fallback != nullptr, "the fallback ability is in the data");
    const auto health_exprs = [&](const abilities::AbilityDef& d) {
        std::vector<std::string> out;
        for (const content::AbilityHitRow& h : d.hits) {
            const abilities::HitDef* hit = f.catalog->hit(h.hit_guid);
            if (!hit) continue;
            for (const content::ModRow& m : hit->mods) {
                if (m.stat_guid != content::kGuidHealth) continue;
                const content::ExprRow* e = f.catalog->expr(m.value_expr);
                out.push_back(e ? e->text : std::string());
            }
        }
        return out;
    };
    if (fallback) {
        const std::vector<std::string> now = health_exprs(*fallback);
        bool formula = !now.empty();
        for (const std::string& t : now) formula = formula && t.find("EvalFormula") != std::string::npos;
        CHECK(formula, "its Health damage is a factor times the shared damage equation, so the creature's level and stats, the player's mitigation and the level difference all count");
    }
    // the data's own placeholder attack is a flat number: the reason it is not the fallback
    const abilities::AbilityDef* flat = f.catalog->ability_named("NPC_Bandit_Melee_Slash_ABL_1");
    if (flat) {
        const std::vector<std::string> placeholder = health_exprs(*flat);
        CHECK(placeholder.size() == 1 && placeholder[0] == "-50", "the data's NPC_Bandit_Melee_Slash_ABL_1 takes a flat 50 Health whatever the creature and the player are");
    }
}

AOC_REGISTER_TEST(test_fallback_attack);

void test_combat_flag(Fixture& f) {
    std::printf("the player's in-combat flag ends the out-of-combat regeneration\n");
    (void)f;
    Rig rig(4, 20, {});
    if (!rig.ok()) return;
    const int64_t regen = f.data->ids().health_regeneration;
    const stats::StatValue* v = rig.cs->find(regen);
    CHECK(v != nullptr, "the character has a HealthRegeneration stat");
    if (!v) return;
    const float out_of_combat = v->value;
    rig.cs->set_in_combat(true);
    rig.pipe->refresh(1.0, "combat starts", true);
    rig.take();
    const float in_combat = rig.cs->find(regen)->value;
    CHECKF(out_of_combat > 0 && std::fabs(out_of_combat / in_combat - 5.0) < 0.01, "out of combat it is five times faster (%.3f against %.3f)", out_of_combat, in_combat);
    CHECK(rig.pawn.stat(regen) == in_combat || rig.pawn.stat(regen) == -12345.0f, "the change goes to the pawn's arrays when the stat is replicated");
    rig.cs->set_in_combat(false);
    rig.pipe->refresh(7.0, "combat ends", true);
    rig.take();
    CHECK(rig.cs->find(regen)->value == out_of_combat, "and returns when combat ends");
}

AOC_REGISTER_TEST(test_combat_flag);
