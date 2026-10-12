// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_tests2.inc: a player's cast and hits on an NPC through the ability runtime, and an NPC's hit on the player
//  (included by test_worldd_combat.cpp inside its anonymous namespace, after combat_tests.inc)
// ============================================================================

std::string wolf_archetype(Fixture& f) {
    std::string wolf_path;
    for (const auto& [path, row] : f.motion.rows()) {
        if (path.find("/Beast/Canine/Wolf.Default__Wolf_C") != std::string::npos) wolf_path = path;
    }
    return wolf_path;
}

void test_player_cast(Fixture& f) {
    std::printf("a player's cast and its hits on an NPC\n");
    Rig rig(4, 20, {});
    CHECKF(rig.ok(), "the rig: %s", rig.problem.c_str());
    if (!rig.ok()) return;
    combat_rig::World w;
    if (!w.problem.empty()) return;
    const std::string wolf_path = wolf_archetype(f);
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion);
    const uint64_t wolf = w.spawn(wolf_path, 600, 0, 8, 100);   // (a small aggro radius: it does not come for the player in this test)
    combat_rig::FakePlayer player(rig, 77, {0, 0, 1098});
    world.add_player(&player);
    combat::WorldHitPort port(world, w.registry, player.fighter());
    rig.pipe->set_combat(&port);
    rig.know({"WeaponCombo_Melee_1H_Axe", "Bard_Misc_Flourish"});
    const abilities::AbilityDef* axe = f.catalog->ability_named("WeaponCombo_Melee_1H_Axe");
    CHECK(axe != nullptr && wolf != 0, "the 1H axe combo and a Wolf 6 m away");
    if (!axe || wolf == 0) return;
    const int64_t ability = axe->row.guid;
    const abilities::HitDef* hit1 = f.catalog->hit(axe->hits[0].hit_guid);
    int64_t targeting = content::kTargetingDefaultSelf;
    for (const content::TargetingRow& t : axe->targetings) {
        if (t.key == axe->hits[0].key) targeting = t.guid;
    }
// [C.O.A x C_Y_B]

    // a cast that names the Wolf: accepted; the ability item's Targets is the Wolf's collision cylinder
    TriggerInfo info;
    info.target_count = 1;
    info.target_actors = {wolf};
    double t = 1.0;
    CHECK(rig.pipe->trigger(ability, 10, t, info) == Refusal::None, "a cast at the Wolf is accepted");
    std::vector<Seen> seen = rig.run_to(t + 0.4);
    const Seen* item = nullptr;
    for (const Seen& s : seen) {
        if (!s.change.abilities_added.empty()) item = &s;
    }
    CHECK(item != nullptr, "its ability item comes");
    if (item) {
        const bootstrap::ActiveAbilityItem& it = item->change.abilities_added[0];
        CHECK(!it.targets_self && it.target_objects.size() == 1 && it.target_objects[0].ObjectId == wolf + 4, "the item's Targets is the Wolf's collision cylinder (the actor's + 4, inside its block: E1 stage 3 fix 3), not the caster's");
    }

    // the client's hit verification: the request names the Wolf's collision cylinder
    t += 0.5;
    const auto verdict = rig.pipe->hit_request(ability, targeting, hit1->row.guid, 1, 0, t, {wolf});
    CHECK(verdict == AbilityPipeline::HitVerdict::Scheduled, "the hit request is scheduled");
    seen = rig.run_to(t + 0.5);
    const bootstrap::HitConfirm* conf = nullptr;
    for (const Seen& s : seen) {
        for (const bootstrap::HitConfirm& c : s.confirmations) {
            if (c.record_guid == hit1->row.guid && !c.targets.empty()) conf = &c;
        }
    }
    CHECK(conf != nullptr, "the hit is confirmed with its targets");
    if (conf) {
        // E1 stage 3 fix 2: the HitComp is the Wolf ACTOR [R: research/hitfx 1, 7,450 retail targets]; its collision cylinder (the actor's id + 4, retail's slot
        // inside the block: E1 stage 3 fix 3) rides as the export
        CHECK(conf->targets.size() == 1 && conf->targets[0].component.ObjectId == wolf && conf->targets[0].cylinder.ObjectId == wolf + 4 && conf->targets[0].has_impact &&
                  conf->mods.empty() && conf->response,
              "one FHitInst with the Wolf actor as its target (its cylinder to export, the impact at it); the pawn is none");
        const bootstrap::HitTargetOut& tg = conf->targets[0];
        if ((tg.flags & stats::kHitMiss) == 0) {
            CHECK(tg.mods.size() == 1 && tg.mods[0].stat_mod_guid == hit1->mods[0].stat_mod_guid && tg.mods[0].value < 0 && tg.mods[0].effective < 0 &&
                      std::fabs(tg.mods[0].effective) <= std::fabs(tg.mods[0].value) + 1e-3,
                  "the target's stat mod: the formula's value and the part that took effect (both a loss)");
            CHECKF(world.health_of(wolf) < world.max_health_of(wolf), "the Wolf lost Health (%.1f of %.1f)", world.health_of(wolf), world.max_health_of(wolf));
        } else {
            CHECK(tg.mods.empty(), "a miss has no stat mods");
        }
    }
    // the caster is untouched by the hit: its own Health is full
    const stats::StatValue* hp = rig.cs->find(content::kGuidHealth);
    const stats::StatValue* hp_max = rig.cs->find(content::kGuidMaxHealth);
    CHECK(hp && hp_max && hp->value == hp_max->value, "the caster's Health is untouched");
    CHECK(rig.pawn.broken.empty(), "(the pawn took every change)");

    // refusals: out of range, dead, an actor that is not there, a friendly ability on an enemy
    const uint64_t far_wolf = w.spawn(wolf_path, 9000, 0, 8, 100);
    TriggerInfo far;
    far.target_count = 1;
    far.target_actors = {far_wolf};
    CHECK(rig.pipe->trigger(ability, 11, t + 3.0, far) == Refusal::OutOfRange, "a Wolf 90 m away is out of range");
    TriggerInfo nobody;
    nobody.target_count = 1;
    CHECK(rig.pipe->trigger(ability, 12, t + 3.1, nobody) == Refusal::None, "a cast naming a target the session could not resolve (no actor) is a cast at the air, as stage 2's");
    // kill the Wolf, then name it
    combat::Resolved kill;
    kill.mods.push_back({1, content::kGuidHealth, -1.0e9});
    world.apply_hit_to_npc(wolf, 77, kill, t + 3.2);
    TriggerInfo dead;
    dead.target_count = 1;
    dead.target_actors = {wolf};
    CHECK(rig.pipe->trigger(ability, 13, t + 3.3, dead) == Refusal::BadTarget, "a dead Wolf is no target");
    TriggerInfo strange;
    strange.target_count = 1;
    strange.target_actors = {123456789};
    CHECK(rig.pipe->trigger(ability, 14, t + 3.4, strange) == Refusal::BadTarget, "an actor that is not in the world is refused");
    // Flourish (a friendly ability: Self only) at an enemy
    const abilities::AbilityDef* flourish = f.catalog->ability_named("Bard_Misc_Flourish");
    if (flourish) {
        TriggerInfo at_enemy;
        at_enemy.target_count = 1;
        at_enemy.target_actors = {far_wolf};
        const Refusal r = rig.pipe->trigger(flourish->row.guid, 15, t + 3.5, at_enemy);
        CHECK(r == Refusal::BadTarget || r == Refusal::OutOfRange, "an ability that only helps allies is not cast at an enemy");
    }
}
// built with C_Y_B and CoA

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
