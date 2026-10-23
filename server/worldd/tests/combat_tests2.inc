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

/// The Health the first hit of a level 10 Wolf takes from a player standing 6 m away, with the NPC damage multiplier at `mult` (NaN: the Wolf never hit).
static double wolf_first_hit_health(Fixture& f, double mult) {
    const double none = std::nan("");
    Rig rig(4, 20, {});
    if (!rig.ok()) return none;
    combat_rig::World w;
    if (!w.problem.empty()) return none;
    const std::string wolf_path = wolf_archetype(f);
    if (wolf_path.empty()) return none;
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorldConfig cfg;
    cfg.seed = 5;
    cfg.npc_damage_mult = mult;
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion, cfg);
    if (w.spawn(wolf_path, 0, 0, 10, 900) == 0) return none;
    combat_rig::FakePlayer player(rig, 77, {600, 0, 1098});
    world.add_player(&player);
    double now = 0;
    while (now < 10.0 && player.hits.empty()) {
        now += 1.0 / 15.0;
        w.npcs->note_player(77, player.rig_fighter().position().x, player.rig_fighter().position().y, now);
        w.npcs->tick(now);
        world.tick(now);
    }
    if (player.hits.empty()) return none;
    for (const combat::ResolvedMod& m : player.hits.front().mods) {
        if (m.stat_guid == content::kGuidHealth) return m.value;
    }
    return none;
}

void test_npc_damage_mult(Fixture& f) {
    std::printf("the NPC damage multiplier: 0.2 takes 80 %% off the Health a creature's hit does to a player\n");
    const double full = wolf_first_hit_health(f, 1.0);
    const double fifth = wolf_first_hit_health(f, 0.2);
    CHECK(std::isfinite(full) && full < 0, "a level 10 Wolf's first hit takes Health at the data's own damage");
    CHECK(std::isfinite(fifth) && std::isfinite(full) && near(fifth, full * 0.2, std::fabs(full) * 1e-6 + 1e-6), "with the multiplier at 0.2 the same hit takes a fifth of it");
    const double none = wolf_first_hit_health(f, 0.0);
    CHECK(std::isfinite(none) && none == 0.0, "with the multiplier at 0 the hit takes nothing");
}

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

// ---- a Grem against a player who stands still: its own animation, its lunge, its bites and its way back ---------------------------------------------------------------------

/// The cast element of a creature's AbilityComponent as the world wrote it last: which element it is, its ability and its montage (0 once the montage is over).
struct GremCastView {
    int32_t  element = 0;
    int64_t  ability = 0;
    uint64_t montage = 0;
};

GremCastView grem_cast_view(world::ActorRegistry& registry, uint64_t actor) {
    GremCastView v;
    world::WorldActor* a = registry.find(actor);
    aoc::protocol::repl::FastArrayState* arr = a ? combat::npc_ability_detail::active_abilities(*a) : nullptr;
    if (!arr) return v;
    for (const aoc::protocol::repl::FastArrayItem& it : arr->items()) {
        v.element = it.id;
        const auto g = it.values.values.find(1);
        if (g != it.values.values.end() && g->second.get_if<int64_t>()) v.ability = *g->second.get_if<int64_t>();
        const auto m = it.values.values.find(11);
        if (m != it.values.values.end() && m->second.get_if<aoc::protocol::emit::FIntrepidNetworkGUID>()) v.montage = m->second.get_if<aoc::protocol::emit::FIntrepidNetworkGUID>()->ObjectId;
    }
    return v;
}

/// One attack with its own animation, as the test saw it begin and end.
struct GremCast {
    double      start = 0, duration = 0;
    int64_t     ability = 0;
    uint64_t    montage_id = 0;
    std::string name;
    int         bites = 0;                              // the hit notifies of its montage that fire a hit of the ability
    double      surface_before = 0, angle_before = 0;   // the tree's two conditions as the world looked at them (the creature as it stood before it turned)
    double      facing_error = 0;                       // degrees between the facing the cast locked and the way to the player
    double      centre_start = 0, centre_end = 0;       // centre distance to the player when it began and when it was over
    int         delivered = 0;                          // hits of the ability that reached the player during it
    double      stepped_back = 0;                       // how far the Grem went back from the closest it came to the player during the cast
    double      closest = 0;                            // the closest it came to the player's centre during the cast
    bool        settled = true;                         // the player stood where it was for the whole cast
    int         mode_at_start = -1, mode_after = -1;    // the Grem's ReplicatedMovementMode when the cast began and shortly before it was over (the montage has let go by then)
    std::vector<double> notifies;                       // seconds after the start at which each bite is due
};

static double wrap180(double a) {
    while (a > 180.0) a -= 360.0;
    while (a < -180.0) a += 360.0;
    return a;
}

void test_grem_fight(Fixture& f) {
    std::printf("a Grem against a player who stands still: it lunges to the player, bites in reach, never goes through or drifts away, turns, and comes back when the player steps away\n");
    constexpr double kPi = 3.14159265358979323846;
    std::string dir = NPC_MOTION_PATH;
    dir.erase(dir.find_last_of("/\\") + 1);
    combat::NpcAbilityData tables;
    std::string err;
    if (!tables.load(dir + "npc_abilities.tsv", dir + "npc_montages.tsv", dir + "npc_targeting.tsv", err)) {
        std::printf("  [skip] the creature tables (tools/npcabilities, the npcabilities extractor step): %s\n", err.c_str());
        return;
    }
    std::string rabid_path;
    for (const auto& [path, row] : f.motion.rows()) {
        if (path.find("/Beast/Rabid/Rabid.Default__Rabid_C") != std::string::npos) rabid_path = path;
    }
    const auto bp = f.motion.find(rabid_path);
    CHECK(!rabid_path.empty() && bp && tables.abilities_of(rabid_path) != nullptr, "the Blueprint table has the Rabid and the creature tables list its abilities");
    if (rabid_path.empty() || !bp || !tables.abilities_of(rabid_path)) return;
    const abilities::AbilityDef* fury = f.catalog->ability_named("Rabid_Fury");
    const abilities::AbilityDef* snap = f.catalog->ability_named("Rabid_Snap");
    CHECK(fury && snap, "the ability data has the Grem's Fury and Snap");
    if (!fury || !snap) return;

    Rig rig(4, 20, {});
    if (!rig.ok()) return;
    combat_rig::World w;
    if (!w.problem.empty()) return;
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorldConfig cfg;
    cfg.seed = 5;
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion, cfg);
    world.set_ability_data(&tables);
    const uint64_t grem = w.spawn(rabid_path, 0, 0, 7, 900, bp->half_height);
    CHECK(grem != 0, "a Grem is made from the Rabid's capture");
    if (grem == 0) return;
    const double r_grem = bp->radius, r_player = 34.0, touch = r_grem + r_player;
    {
        // the animated creature attacks of the tables, by where their hits land (the line worldd logs when it loads them)
        const combat::CombatWorld::GateCensus census = world.gate_census();
        std::printf("  %s\n", census.text().c_str());
        const auto list = [](const char* what, const std::vector<std::string>& names) {
            std::string s;
            for (const std::string& n : names) s += (s.empty() ? "" : ", ") + n;
            std::printf("    %s: %s\n", what, s.c_str());
        };
        list("on the target wherever it stands", census.any_names);
        list("inside a cone", census.validity_names);
        list("by volumes", census.volume_names);
        list("keep the single swing", census.kept_names);
        list("no hit of the montage's to decide by", census.unplayed_names);
        CHECKF(census.any >= 1 && census.validity >= 1 && census.volumes >= 1,
               "the tables hold attacks of every kind of rule: %zu on the target wherever it stands, %zu inside a cone, %zu by volumes (%zu keep the swing)", census.any, census.validity, census.volumes, census.kept_swing);
    }
    world::WorldActor* actor = w.registry.find(grem);
    CHECK(actor && combat::npc_knows_ability(*actor, fury->row.guid) && combat::npc_knows_ability(*actor, snap->row.guid), "its open lists the Fury and the Snap, so the world plays their animations");
    double reach_cm = 0;   // what the tree lets it start an attack from, surface to surface
    for (const combat::CreatureAbility& t : *tables.abilities_of(rabid_path)) {
        if (!t.unknown && t.max_distance > reach_cm) reach_cm = t.max_distance;
    }
    CHECK(reach_cm > 0, "the tree gives its basic attacks a reach");

    double px = 0, py = 800;
    combat_rig::FakePlayer player(rig, 77, {px, py, 1098});
    world.add_player(&player);

    // the player stands for 20 s, steps 6 m away (and the Grem has to come back), and after 8 s more is put at the Grem's back (it has to turn)
    const double dt = 1.0 / 15.0, t_away = 20.0, t_behind = 28.0, t_end = 36.0;
    struct Pose {
        double t, x, y, yaw, px, py;
        int    mode;   // its ReplicatedMovementMode (-1: none)
    };
    const auto movement_mode = [&](const world::WorldActor& g) {
        const aoc::protocol::repl::ObjectState& obj = g.actor.obj;
        const aoc::protocol::repl::Value* v = obj.schema ? obj.state.get(*obj.schema, "ReplicatedMovementMode") : nullptr;
        return v && v->get_if<uint64_t>() ? static_cast<int>(*v->get_if<uint64_t>()) : -1;
    };
    const int mode_before = movement_mode(*w.registry.find(grem));
    std::vector<Pose> trace;
    std::vector<GremCast> casts;
    std::vector<double> moves;   // when the player was put somewhere else
    GremCastView last_view;
    // what the connection that has the Grem open is sent while it fights: the actor's own block of each update, read back (the properties that changed)
    struct Wire {
        double   t = 0;
        int      active = -1;              // RepRootMotion.bIsActive (-1: not in the block)
        double   position = -1;            // RepRootMotion.Position
        bool     has_animation = false;
        uint64_t animation = 0;            // RepRootMotion.Animation's ObjectId
        bool     has_rrm_loc = false, has_mv_loc = false;
        int64_t  rrm_loc[3] = {0, 0, 0}, mv_loc[3] = {0, 0, 0};
        bool     has_mv_rot = false, has_rrm_rot = false, has_rrm_vel = false;
        double   mv_yaw = 0, rrm_yaw = 0;   // the yaw as a client reads the wire integer: degrees, 8 or 16 bits as the block was written
        int64_t  mv_vel[2] = {0, 0};        // ReplicatedMovement's linear velocity, cm/s
        int64_t  rrm_vel[2] = {0, 0};       // RepRootMotion.LinearVelocity, tenths of cm/s
        int      rep_movement = -1;         // bReplicateMovement (-1: not in the block)
    };
    std::vector<Wire> wire;
    aoc::protocol::repl::ActorShadow shadow;
    // bReplicateMovement as the open leaves it on the client (-1: the open does not carry it, the class default holds)
    int open_rep_movement = -1;
    if (const auto* v = actor->actor.obj.state.get(*actor->actor.obj.schema, "bReplicateMovement")) {
        if (v->get_if<bool>()) open_rep_movement = *v->get_if<bool>() ? 1 : 0;
    }
    {
        aoc::protocol::repl::ExportAcks acks;
        bool failed = false;
        const auto open = aoc::protocol::repl::open_actor(actor->actor, shadow, acks, &failed);
        CHECK(open.has_value() && !failed, "the Grem's open builds (the shadow of the connection that watches the fight)");
    }
    double min_centre = 1e9, farthest_in_fight = 0, first_cast_at = -1;
    bool away = false, behind = false;
    double now = 0;
    while (now < t_end) {
        now += dt;
        if (!away && now >= t_away) {   // the player steps 6 m away, mid-fight
            away = true;
            py += 600;
            player.rig_fighter().move_to({px, py, 1098});
            moves.push_back(now);
        }
        if (!behind && now >= t_behind) {   // ... and is then at the Grem's back, 3 m from it
            behind = true;
            const world::WorldActor* g = w.registry.find(grem);
            px = g->x - 300.0 * std::cos(g->yaw_degrees * kPi / 180.0);
            py = g->y - 300.0 * std::sin(g->yaw_degrees * kPi / 180.0);
            player.rig_fighter().move_to({px, py, 1098});
            moves.push_back(now);
        }
        w.npcs->note_player(77, px, py, now);
        w.npcs->tick(now);
        const world::WorldActor* before_actor = w.registry.find(grem);
        const double bx = before_actor->x, by = before_actor->y, byaw = before_actor->yaw_degrees;
        world.tick(now);
        const world::WorldActor* g = w.registry.find(grem);
        trace.push_back({now, g->x, g->y, g->yaw_degrees, px, py, movement_mode(*g)});
        {
            std::vector<aoc::protocol::repl::ContentBlock> blocks;
            if (aoc::protocol::repl::collect_update_blocks(actor->actor, shadow, blocks)) {
                for (const aoc::protocol::repl::ContentBlock& b : blocks) {
                    if (b.sub || !b.has_rep_layout) continue;
                    const auto st = aoc::protocol::repl::load_object_state(*actor->actor.obj.schema, b.payload.data(), b.payload.bit_pos(), true);
                    if (!st) continue;
                    const aoc::protocol::schema::ClassSchema& sc = *actor->actor.obj.schema;
                    Wire ws;
                    ws.t = now;
                    if (const auto* v = st->state.get(sc, "RepRootMotion.bIsActive")) {
                        if (v->get_if<bool>()) ws.active = *v->get_if<bool>() ? 1 : 0;
                    }
                    if (const auto* v = st->state.get(sc, "RepRootMotion.Position")) {
                        if (v->get_if<float>()) ws.position = *v->get_if<float>();
                    }
                    if (const auto* v = st->state.get(sc, "RepRootMotion.Animation")) {
                        if (const auto* o = v->get_if<aoc::protocol::emit::FIntrepidNetworkGUID>()) {
                            ws.has_animation = true;
                            ws.animation = o->ObjectId;
                        }
                    }
                    if (const auto* v = st->state.get(sc, "RepRootMotion.Location")) {
                        if (const auto* q = v->get_if<aoc::protocol::repl::QuantVec>()) {
                            ws.has_rrm_loc = true;
                            ws.rrm_loc[0] = q->x, ws.rrm_loc[1] = q->y, ws.rrm_loc[2] = q->z;
                        }
                    }
                    if (const auto* v = st->state.get(sc, "ReplicatedMovement")) {
                        if (const auto* m = v->get_if<aoc::protocol::repl::RepMovement>()) {
                            ws.has_mv_loc = true;
                            ws.mv_loc[0] = m->location.x, ws.mv_loc[1] = m->location.y, ws.mv_loc[2] = m->location.z;
                            // the rotation as the client decodes it: FRotator::DecompressAxisFromByte (raw * 360 / 256) or ...FromShort (raw * 360 / 65536)
                            ws.has_mv_rot = true;
                            ws.mv_yaw = m->rotation.yaw * 360.0 / (st->opts.rep_movement_short_rotation ? 65536.0 : 256.0);
                            ws.mv_vel[0] = m->linear_velocity.x, ws.mv_vel[1] = m->linear_velocity.y;
                        }
                    }
                    if (const auto* v = st->state.get(sc, "RepRootMotion.Rotation")) {
                        if (const auto* r = v->get_if<aoc::protocol::repl::Rot>()) {
                            ws.has_rrm_rot = true;
                            ws.rrm_yaw = r->yaw * 360.0 / 65536.0;
                        }
                    }
                    if (const auto* v = st->state.get(sc, "RepRootMotion.LinearVelocity")) {
                        if (const auto* q = v->get_if<aoc::protocol::repl::QuantVec>()) {
                            ws.has_rrm_vel = true;
                            ws.rrm_vel[0] = q->x, ws.rrm_vel[1] = q->y;
                        }
                    }
                    if (const auto* v = st->state.get(sc, "bReplicateMovement")) {
                        if (v->get_if<bool>()) ws.rep_movement = *v->get_if<bool>() ? 1 : 0;
                    }
                    wire.push_back(ws);
                }
            }
        }
        const double centre = std::hypot(g->x - px, g->y - py);
        min_centre = std::min(min_centre, centre);
        if (first_cast_at >= 0 && now < t_away) farthest_in_fight = std::max(farthest_in_fight, centre);
        const GremCastView view = grem_cast_view(w.registry, grem);
        if (view.montage != 0 && (view.element != last_view.element || last_view.montage == 0)) {
            GremCast c;
            c.start = now;
            c.ability = view.ability;
            c.montage_id = view.montage;
            const abilities::AbilityDef* def = f.catalog->ability(view.ability);
            c.name = def ? def->row.name : "?";
            c.surface_before = std::hypot(bx - px, by - py) - r_grem - r_player;
            c.angle_before = std::fabs(wrap180(std::atan2(py - by, px - bx) * 180.0 / kPi - byaw));
            c.facing_error = std::fabs(wrap180(std::atan2(py - g->y, px - g->x) * 180.0 / kPi - g->yaw_degrees));
            c.centre_start = centre;
            if (def) {
                for (const content::MontageRow& m : def->montages) {
                    const combat::MontageData* md = m.soft_path.empty() ? nullptr : tables.montage(m.soft_path);
                    if (!md || md->guid != view.montage) continue;
                    c.duration = md->seconds(1.0);
                    for (const combat::MontageData::Hit& h : md->hits) {
                        for (const content::AbilityHitRow& r : def->hits) {
                            if (r.source != "hit" || r.key != h.key) continue;
                            ++c.bites;
                            c.notifies.push_back(h.time / md->rate_scale);
                            break;
                        }
                    }
                }
            }
            if (first_cast_at < 0) first_cast_at = now;
            casts.push_back(std::move(c));
        }
        last_view = view;
    }
    const stats::StatValue* hp = rig.cs->find(content::kGuidHealth);
    CHECK(hp && hp->value > 0, "the player is still alive at the end (the bites went on)");

    // what reached the player during each cast; how close the Grem was at the end of it and how far it went back from the closest it came
    for (GremCast& c : casts) {
        c.centre_end = c.centre_start;
        double closest = c.centre_start;
        for (const Pose& p : trace) {
            if (p.t < c.start) continue;
            const double d = std::hypot(p.x - p.px, p.y - p.py);
            if (p.t <= c.start + c.duration) {
                closest = std::min(closest, d);
                c.centre_end = d;
            }
            if (std::fabs(p.t - c.start) < 0.5 * dt) c.mode_at_start = p.mode;
            if (p.t >= c.start + c.duration - 0.1 && c.mode_after < 0) c.mode_after = p.mode;
        }
        c.closest = closest;
        c.stepped_back = c.centre_end - closest;
        for (const combat::IncomingHit& h : player.hits) {
            if (h.source_guid == c.ability && h.at >= c.start - 0.05 && h.at <= c.start + c.duration + 0.25) ++c.delivered;
        }
        c.settled = c.start + c.duration + 0.25 <= t_end;   // the player stood where it was for the whole cast, and the cast was over before the test was
        for (const double m : moves) c.settled = c.settled && !(m > c.start - 0.01 && m < c.start + c.duration + 0.25);
    }
    std::printf("  %zu casts in %.0f s (%llu attacks, %llu hits and %llu misses reached the player); the first cast at %.1f s\n", casts.size(), t_end,
                static_cast<unsigned long long>(world.stats().npc_attacks), static_cast<unsigned long long>(world.stats().npc_hits), static_cast<unsigned long long>(world.stats().npc_misses), first_cast_at);
    {
        size_t with_rrm = 0, starts = 0, ends = 0;
        for (const Wire& ws : wire) {
            if (ws.position >= 0) ++with_rrm;
            if (ws.active == 1) ++starts;
            if (ws.active == 0) ++ends;
        }
        std::printf("  the Grem's own block was sent %zu times; %zu carried RepRootMotion's position, %zu said a montage carries the Grem, %zu said it let go\n", wire.size(), with_rrm, starts, ends);
    }
    for (size_t i = 0; i < casts.size() && i < 60; ++i) {
        const GremCast& c = casts[i];
        std::printf("    t %5.2f  %-11s  before: surface %6.1f cm, %5.1f deg off   facing error %5.1f deg   centre %6.1f -> %6.1f cm (back %4.1f)   bites %d of %d%s\n", c.start, c.name.c_str(),
                    c.surface_before, c.angle_before, c.facing_error, c.centre_start, c.centre_end, c.stepped_back, c.delivered, c.bites, c.settled ? "" : "   (the player moved during it)");
    }

    // ---- the checks ----
    size_t before_away = 0, after_away = 0, after_behind = 0;
    for (const GremCast& c : casts) {
        if (c.start + c.duration <= t_away) ++before_away;
        if (c.start >= t_away) ++after_away;
        if (c.start >= t_behind) ++after_behind;
    }
    CHECKF(before_away >= 6, "the Grem attacks a player who stands in front of it again and again (%zu casts in the first %.0f s)", before_away, t_away);
    // it never comes closer than the two bodies touch
    CHECKF(min_centre >= touch - 1.0, "its centre never comes closer to the player's than the two radii (%.1f cm), the closest was %.1f cm", touch, min_centre);
    // it does not drift: once it fights, it stays within the reach of the tree's attacks
    const double bound = touch + reach_cm * 1.15 + 1.0;
    CHECKF(first_cast_at >= 0 && farthest_in_fight <= bound, "while the player stands it stays within %.0f cm (the radii and the tree's reach with the AI's slack) of the player, the farthest was %.1f cm", bound, farthest_in_fight);
    // it does not step back from the body it has reached when the animation winds back
    size_t backed = 0;
    double most_back = 0;
    for (const GremCast& c : casts) {
        if (!c.settled || c.closest > touch + 1.0) continue;   // (a lunge that found room goes the animation's whole way, back included)
        if (c.stepped_back > 1.5) ++backed;
        most_back = std::max(most_back, c.stepped_back);
    }
    CHECKF(backed == 0, "once it is at the player's body it does not step back when the animation winds back (%zu casts did, the most %.1f cm)", backed, most_back);
    // while a montage carries the Grem it walks (the movement mode the client moves it by the montage's root motion in), and afterwards it is as it was
    size_t wrong_mode = 0;
    for (const GremCast& c : casts) {
        if (c.start + c.duration > t_end) continue;   // (the last cast was cut off by the end of the test)
        if (c.mode_at_start != aoc::protocol::repl::kMoveWalking || c.mode_after != mode_before) ++wrong_mode;
    }
    CHECKF(wrong_mode == 0, "its movement mode is Walking while a montage carries it and what it was (%d) when the montage is over (%zu of %zu casts differ)", mode_before, wrong_mode, casts.size());
    // the yaw of the Grem as the client reads it from the wire is the bearing to its target while it stands and bites (retail's relation, 10 recorded casts: the bearing less the yaw is 1.8 deg
    // median at the cast's first update, 8-bit and 16-bit alike, a mirrored or flipped reading 83 and 178 deg off)
    {
        size_t samples = 0, off8 = 0, off16 = 0;
        double worst8 = 0, worst16 = 0;
        for (const GremCast& c : casts) {
            // the yaw the client holds when the cast begins: the last it was sent (the rotation is sent when it changes)
            double yaw8 = 0, yaw16 = 0;
            bool have8 = false, have16 = false;
            for (const Wire& ws : wire) {
                if (ws.t > c.start + 1e-6) break;
                if (ws.has_mv_rot) yaw8 = ws.mv_yaw, have8 = true;
                if (ws.has_rrm_rot) yaw16 = ws.rrm_yaw, have16 = true;
            }
            const Pose* at = nullptr;
            for (const Pose& p : trace) {
                if (std::fabs(p.t - c.start) < 1e-6) at = &p;
            }
            if (!at || !have8) continue;
            const double bearing = at->yaw;   // the facing the server has when the cast begins (the Grem turns to its target at its rate, so this is within a couple of degrees of the bearing: the check above)
            const double d8 = std::fabs(wrap180(bearing - yaw8)), d16 = have16 ? std::fabs(wrap180(bearing - yaw16)) : 0.0;
            ++samples;
            worst8 = std::max(worst8, d8);
            worst16 = std::max(worst16, d16);
            if (d8 > 0.75 * 360.0 / 256.0) ++off8;
            if (have16 && d16 > 0.75 * 360.0 / 65536.0 + 0.01) ++off16;
        }
        std::printf("  the wire's yaw against the server's, as the client holds it at the start of each cast (%zu): ReplicatedMovement (8 bits) at most %.2f deg off, RepRootMotion.Rotation (16 bits) at most %.3f deg off\n",
                    samples, worst8, worst16);
        CHECKF(samples >= 8 && off8 == 0 && off16 == 0, "the Grem's yaw on the wire, read as the client reads it, is the yaw the server bites with: %zu casts sampled, %zu off in ReplicatedMovement, %zu in RepRootMotion.Rotation",
               samples, off8, off16);
    }
    // ... and a Grem that walks up to the player faces the way it walks: the same encoding, the same reading (a walking creature never needed the lunge's rotation)
    {
        size_t walking = 0, wrong = 0;
        double worst = 0;
        for (const Wire& ws : wire) {
            if (ws.t > first_cast_at || ws.t < 0.55 || !ws.has_mv_rot) continue;
            if (std::hypot(static_cast<double>(ws.mv_vel[0]), static_cast<double>(ws.mv_vel[1])) < 200.0) continue;
            const double heading = std::atan2(static_cast<double>(ws.mv_vel[1]), static_cast<double>(ws.mv_vel[0])) * 180.0 / kPi;
            const double d = std::fabs(wrap180(heading - ws.mv_yaw));
            ++walking;
            worst = std::max(worst, d);
            if (d > 10.0) ++wrong;
        }
        CHECKF(walking >= 3 && wrong == 0, "a Grem that walks up to the player faces the way it walks, on the wire (%zu updates sampled, the worst %.1f deg off)", walking, worst);
    }
    // RepRootMotion.LinearVelocity is the Grem's velocity (what the recorded updates carry in every one of a lunge: ReplicatedMovement's velocity in tenths of cm/s)
    {
        size_t lunging = 0, missing = 0;
        for (const Wire& ws : wire) {
            if (ws.position < 0) continue;
            ++lunging;
            if (!ws.has_rrm_vel && std::hypot(static_cast<double>(ws.mv_vel[0]), static_cast<double>(ws.mv_vel[1])) > 1.0) ++missing;
        }
        CHECKF(missing == 0, "every update of a lunge that moves the Grem carries RepRootMotion.LinearVelocity (%zu of %zu do not)", missing, lunging);
    }
    // Fury: the lunge stops for good once the player is in the box ahead of it (the ability record: StopOnContact, FreezeDistance, FreezeRadiusScale), at a centre distance of 235 to 274 cm
    // in the recorded casts; one that begins inside the box does not move at all
    if (const combat::RootMotionFreezeRule* rule = tables.freeze_of(fury->row.guid)) {
        const double edge = rule->distance + rule->radius_scale * r_grem + r_player;
        size_t outside = 0, inside = 0, stopped_wrong = 0, moved_inside = 0;
        double lowest = 1e9, highest = 0;
        for (const GremCast& c : casts) {
            if (c.ability != fury->row.guid || !c.settled) continue;
            if (c.centre_start > edge) {
                ++outside;
                lowest = std::min(lowest, c.centre_end);
                highest = std::max(highest, c.centre_end);
                if (c.centre_end < 235.0 || c.centre_end > 274.0) ++stopped_wrong;
            } else {
                ++inside;
                if (c.centre_start - c.closest > 2.0) ++moved_inside;
            }
        }
        std::printf("  the Fury: its freeze box reaches %.0f cm ahead of the Grem's centre (%.0f + %.0f x %.0f + %.0f); %zu began outside it and stopped at %.1f to %.1f cm, %zu began inside it\n", edge,
                    rule->distance, rule->radius_scale, r_grem, r_player, outside, outside ? lowest : 0.0, highest, inside);
        CHECKF(rule->kind == combat::RootMotionFreezeRule::Kind::Stop && outside >= 1, "a Fury begins outside its freeze box, as the Grem walks up to the player (%zu did)", outside);
        CHECKF(stopped_wrong == 0, "a Fury that begins outside its freeze box stops at a centre distance of 235 to 274 cm from a player who stands (%zu of %zu did not, %.1f to %.1f cm)", stopped_wrong, outside,
               outside ? lowest : 0.0, highest);
        CHECKF(moved_inside == 0, "a Fury that begins inside its freeze box does not move (%zu of %zu did)", moved_inside, inside);
    }
    // what the client is sent while a montage carries the Grem: the montage and that it carries the Grem in the cast's first update, the montage's position and the Grem's place in every one, and its
    // end in the last; the place in RepRootMotion is the place in ReplicatedMovement
    size_t no_start = 0, no_end = 0, thin = 0, places_differ = 0, complete_casts = 0;
    for (const GremCast& c : casts) {
        if (c.start + c.duration > t_end) continue;
        ++complete_casts;
        bool started = false, ended = false;
        size_t positions = 0;
        for (const Wire& ws : wire) {
            if (ws.t < c.start - 1e-6 || ws.t > c.start + c.duration + 1e-6) continue;
            if (ws.active == 1 && ws.has_animation && ws.animation == c.montage_id && ws.t <= c.start + 1e-6) started = true;
            if (ws.active == 0 && ws.has_animation && ws.animation == 0) ended = true;
            if (ws.position >= 0) ++positions;
            if (ws.has_rrm_loc && ws.has_mv_loc && (ws.rrm_loc[0] != ws.mv_loc[0] || ws.rrm_loc[1] != ws.mv_loc[1] || ws.rrm_loc[2] != ws.mv_loc[2])) ++places_differ;
        }
        if (!started) ++no_start;
        if (!ended) ++no_end;
        if (positions < 6) ++thin;
    }
    CHECKF(no_start == 0 && no_end == 0 && thin == 0 && places_differ == 0,
           "the updates of a lunge carry RepRootMotion: its first names the montage and says it carries the Grem (%zu of %zu casts lack it), every one the montage's position (%zu casts have fewer than 6), the last "
           "its end (%zu lack it), and the place is ReplicatedMovement's (%zu updates differ)",
           no_start, complete_casts, thin, no_end, places_differ);
    // when a montage lets go of the Grem the client's copy, which the montage's root motion moved, is put back where the server has it: the retail server sends ReplicatedMovement again after the end of a
    // Snap or a Fury (the same place, no velocity), the first within 0.03 to 0.2 s of the end and then at every update while the creature stands; so does this server, for a few updates
    {
        size_t ends = 0, unsent = 0, wrong_place = 0, moving = 0, thin_tail = 0;
        double slowest = 0;
        for (const GremCast& c : casts) {
            if (!c.settled || c.start + c.duration > t_end) continue;
            const Wire* end = nullptr;
            for (const Wire& ws : wire) {
                if (ws.t < c.start - 1e-6 || ws.t > c.start + c.duration + 1e-6) continue;
                if (ws.active == 0 && ws.has_animation && ws.animation == 0) {
                    end = &ws;
                    break;
                }
            }
            if (!end) continue;
            ++ends;
            const Wire* again = nullptr;
            size_t tail = 0;
            for (const Wire& ws : wire) {
                if (ws.t <= end->t + 1e-6 || ws.t > end->t + 0.35 || !ws.has_mv_loc) continue;
                if (!again) again = &ws;
                ++tail;
            }
            if (tail < 3) ++thin_tail;
            if (!again || again->t - end->t > 0.25 + 1e-6) {
                ++unsent;
                continue;
            }
            slowest = std::max(slowest, again->t - end->t);
            const Pose* at = nullptr;
            for (const Pose& p : trace) {
                if (std::fabs(p.t - again->t) < 1e-6) at = &p;
            }
            if (!at || std::fabs(static_cast<double>(again->mv_loc[0]) - at->x * 100.0) > 100.0 || std::fabs(static_cast<double>(again->mv_loc[1]) - at->y * 100.0) > 100.0) ++wrong_place;
            if (again->mv_vel[0] != 0 || again->mv_vel[1] != 0) ++moving;
        }
        std::printf("  after a montage lets go of the Grem (%zu ends), ReplicatedMovement goes out again within %.3f s at the longest\n", ends, slowest);
        CHECKF(ends >= 6 && unsent == 0, "after every cast's end a ReplicatedMovement goes out within 0.25 s (%zu of %zu ends have none)", unsent, ends);
        CHECKF(wrong_place == 0 && moving == 0, "... with the Grem's place and no velocity (%zu with another place, %zu with a velocity)", wrong_place, moving);
        CHECKF(thin_tail == 0, "... and it goes on at the following updates for a few more (%zu of %zu ends are followed by fewer than 3 in 0.35 s)", thin_tail, ends);
    }
    // the client queues a lunge's RepRootMotion, and applies a ReplicatedMovement, only while the actor replicates its movement: from the first cast on, every update
    // that says where the Grem is must reach a client that holds bReplicateMovement on (the open's value, then each update's, applied before the update's notifies run)
    {
        int held = open_rep_movement;
        size_t lunge = 0, lunge_off = 0, place = 0, place_off = 0;
        for (const Wire& ws : wire) {
            if (ws.rep_movement >= 0) held = ws.rep_movement;
            if (first_cast_at < 0 || ws.t < first_cast_at - 1e-6) continue;
            if (ws.active == 1) {
                ++lunge;
                if (held != 1) ++lunge_off;
            }
            if (ws.has_mv_loc) {
                ++place;
                if (held != 1) ++place_off;
            }
        }
        std::printf("  bReplicateMovement: %d in the open (-1: not in it), %d at the end of the fight\n", open_rep_movement, held);
        CHECKF(lunge > 0 && lunge_off == 0, "every RepRootMotion update of a lunge reaches a client that replicates the Grem's movement (%zu of %zu do not)", lunge_off, lunge);
        CHECKF(place_off == 0, "... and so does every ReplicatedMovement from the first cast on (%zu of %zu do not)", place_off, place);
    }
    // every cast turns to the player before it begins
    size_t unfaced = 0;
    double worst_facing = 0;
    for (const GremCast& c : casts) {
        if (c.facing_error > 3.0) ++unfaced;
        worst_facing = std::max(worst_facing, c.facing_error);
    }
    CHECKF(unfaced == 0, "every cast faces the player when it begins (%zu of %zu do not, the worst %.1f deg off)", unfaced, casts.size(), worst_facing);
    // the tree's conditions hold when a cast begins: the distance and the angle of the task that uses the ability
    size_t outside = 0;
    double first_outside_at = -1, first_surface = 0, first_angle = 0;
    for (const GremCast& c : casts) {
        bool ok = false;
        for (const combat::CreatureAbility& t : *tables.abilities_of(rabid_path)) {
            if (t.unknown || t.ability != c.ability) continue;
            const bool d_ok = (t.max_distance < 0 || c.surface_before <= t.max_distance + 0.5) && (t.min_distance < 0 || c.surface_before >= t.min_distance - 0.5);
            const bool a_ok = t.max_angle < 0 || c.angle_before <= t.max_angle + 0.5;
            ok = ok || (d_ok && a_ok);
        }
        if (!ok && outside++ == 0) {
            first_outside_at = c.start;
            first_surface = c.surface_before;
            first_angle = c.angle_before;
        }
    }
    CHECKF(outside == 0, "no cast begins while the tree's conditions fail (%zu of %zu did; the first at %.2f s, %.1f cm from the player's surface and %.1f deg off its facing)", outside, casts.size(), first_outside_at,
           first_surface, first_angle);
    // every bite of a cast that began and ended while the player stood where he was reaches the player
    size_t short_casts = 0;
    int missing_bites = 0;
    double first_short_at = -1;
    for (const GremCast& c : casts) {
        if (!c.settled) continue;
        if (c.delivered < c.bites) {
            if (short_casts++ == 0) first_short_at = c.start;
            missing_bites += c.bites - c.delivered;
        }
    }
    CHECKF(short_casts == 0, "every bite of every cast lands while the player stands in its area (%zu casts lost %d bites; the first at %.2f s)", short_casts, missing_bites, first_short_at);
    // the way back: the player steps 6 m away, the Grem walks up to it again and bites
    double t_back = -1;
    for (const Pose& p : trace) {
        if (p.t < t_away || p.t >= t_behind) continue;
        if (std::hypot(p.x - p.px, p.y - p.py) <= touch + reach_cm) {
            t_back = p.t;
            break;
        }
    }
    CHECKF(t_back >= 0 && t_back - t_away <= 7.0, "when the player steps 6 m away the Grem is back within its reach in time (%.1f s after the step)", t_back < 0 ? -1.0 : t_back - t_away);
    int bites_after = 0;
    size_t casts_between = 0;
    for (const GremCast& c : casts) {
        if (c.start >= t_away && c.start < t_behind) {
            ++casts_between;
            bites_after += c.delivered;
        }
    }
    CHECKF(casts_between >= 1 && bites_after >= 1, "and it lands bites again (%zu casts and %d bites between the step and the next)", casts_between, bites_after);
    // at its back, it turns and bites
    int bites_behind = 0;
    for (const GremCast& c : casts) {
        if (c.start >= t_behind) bites_behind += c.delivered;
    }
    CHECKF(after_behind >= 1 && bites_behind >= 1, "when the player is at its back it turns and bites (%zu casts and %d bites after it)", after_behind, bites_behind);
}

// ---- a creature whose open has the client ignore its movement: every place, turn and montage the world writes reaches a client that applies it ---------------------------------

/// What one update of a creature's actor block says about its movement, as a client reads it.
struct MoveWire {
    double t = 0;
    int    rep_movement = -1;     // bReplicateMovement (-1: not in the block)
    bool   place = false;         // ReplicatedMovement (where it is, its facing)
    bool   lunge = false;         // RepRootMotion says a montage carries it
    double yaw = 0;               // the facing ReplicatedMovement says, in degrees
};

/// The blocks of the creature's own channel in the update the world has pending, read back as the client's connection would read them.
void read_move_wire(world::WorldActor& actor, aoc::protocol::repl::ActorShadow& shadow, double now, std::vector<MoveWire>& out) {
    std::vector<aoc::protocol::repl::ContentBlock> blocks;
    if (!aoc::protocol::repl::collect_update_blocks(actor.actor, shadow, blocks)) return;
    for (const aoc::protocol::repl::ContentBlock& b : blocks) {
        if (b.sub || !b.has_rep_layout) continue;
        const auto st = aoc::protocol::repl::load_object_state(*actor.actor.obj.schema, b.payload.data(), b.payload.bit_pos(), true);
        if (!st) continue;
        const aoc::protocol::schema::ClassSchema& sc = *actor.actor.obj.schema;
        MoveWire w;
        w.t = now;
        if (const auto* v = st->state.get(sc, "bReplicateMovement")) {
            if (v->get_if<bool>()) w.rep_movement = *v->get_if<bool>() ? 1 : 0;
        }
        if (const auto* v = st->state.get(sc, "RepRootMotion.bIsActive")) {
            if (v->get_if<bool>()) w.lunge = *v->get_if<bool>();
        }
        if (const auto* v = st->state.get(sc, "ReplicatedMovement")) {
            if (const auto* m = v->get_if<aoc::protocol::repl::RepMovement>()) {
                w.place = true;
                w.yaw = m->rotation.yaw * 360.0 / (st->opts.rep_movement_short_rotation ? 65536.0 : 256.0);
            }
        }
        out.push_back(w);
    }
}

void test_creature_movement_reaches_client(Fixture& f) {
    std::printf("a creature whose open has the client ignore its movement: it walks, plays an in-place attack and turns, and every one of those updates reaches a client that applies them\n");
    constexpr double kPi = 3.14159265358979323846;
    std::string dir = NPC_MOTION_PATH;
    dir.erase(dir.find_last_of("/\\") + 1);
    combat::NpcAbilityData tables;
    std::string err;
    if (!tables.load(dir + "npc_abilities.tsv", dir + "npc_montages.tsv", dir + "npc_targeting.tsv", err)) {
        std::printf("  [skip] the creature tables (tools/npcabilities, the npcabilities extractor step): %s\n", err.c_str());
        return;
    }
    // an Alpha Wolf that is hurt: its Growl plays in place, and it bites with a short lunge
    std::string alpha_path;
    for (const auto& [path, row] : f.motion.rows()) {
        if (path.find("/Beast/Canine/AlphaWolf.Default__AlphaWolf_C") != std::string::npos) alpha_path = path;
    }
    const auto bp = f.motion.find(alpha_path);
    CHECK(!alpha_path.empty() && bp && tables.abilities_of(alpha_path) != nullptr, "the Blueprint table has the Alpha Wolf and the creature tables list its abilities");
    if (alpha_path.empty() || !bp || !tables.abilities_of(alpha_path)) return;
    const abilities::AbilityDef* growl = f.catalog->ability_named("AlphaWolf_Growl");
    CHECK(growl != nullptr, "the ability data has the Growl");
    if (!growl) return;

    Rig rig(4, 20, {});
    if (!rig.ok()) return;
    combat_rig::World w;
    if (!w.problem.empty()) return;
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorldConfig cfg;
    cfg.seed = 5;
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion, cfg);
    world.set_ability_data(&tables);
    const uint64_t wolf = w.spawn(alpha_path, 0, 0, 7, 900, bp->half_height);
    CHECK(wolf != 0, "an Alpha Wolf is made from its open");
    if (wolf == 0) return;
    world::WorldActor* actor = w.registry.find(wolf);
    CHECK(combat::npc_knows_ability(*actor, growl->row.guid), "its open lists the Growl, so the world plays its animation");
    const aoc::protocol::schema::ClassSchema& schema = *actor->actor.obj.schema;
    CHECK(schema.command("bReplicateMovement") != nullptr, "its actor has the bReplicateMovement property");
    // an open the client was sent with the property off: the client ignores every place until the world says otherwise
    actor->actor.obj.state.set(schema, "bReplicateMovement", aoc::protocol::repl::Value::boolean(false));
    combat::Fighter* fighter = world.fighter_of(wolf);
    CHECK(fighter != nullptr, "the world makes it a fighter");
    if (!fighter) return;

    double px = 0, py = 800;
    combat_rig::FakePlayer player(rig, 77, {px, py, 1098});
    world.add_player(&player);
    aoc::protocol::repl::ActorShadow shadow;
    {
        aoc::protocol::repl::ExportAcks acks;
        bool failed = false;
        const auto open = aoc::protocol::repl::open_actor(actor->actor, shadow, acks, &failed);
        CHECK(open.has_value() && !failed, "the Alpha Wolf's open builds (the shadow of the connection that watches the fight)");
    }
    const double dt = 1.0 / 15.0, t_behind = 12.0, t_end = 22.0;
    std::vector<MoveWire> wire;
    struct Cast {
        double  start = 0;
        bool    in_place = false;
        int     held = -1;   // what the client held when it began
    };
    std::vector<Cast> casts;
    GremCastView last_view;
    int held = 0;   // bReplicateMovement as the client holds it: the open's value, then each update's
    bool behind = false;
    double behind_at = 0, yaw_before_turn = 0;
    const stats::StatValue* wolf_max = fighter->stats().find(content::kGuidMaxHealth);
    double now = 0;
    while (now < t_end) {
        now += dt;
        if (!behind && now >= t_behind) {   // the player is put at its back: it has to turn
            behind = true;
            behind_at = now;
            const world::WorldActor* g = w.registry.find(wolf);
            yaw_before_turn = g->yaw_degrees;
            px = g->x - 300.0 * std::cos(g->yaw_degrees * kPi / 180.0);
            py = g->y - 300.0 * std::sin(g->yaw_degrees * kPi / 180.0);
            player.rig_fighter().move_to({px, py, 1098});
        }
        if (const stats::StatValue* max_hp = rig.cs->find(content::kGuidMaxHealth)) rig.cs->set_current(content::kGuidHealth, max_hp->value);   // (the fight is about the creature's updates, not the player's life; asked afresh: a hit rebuilds the stats)
        if (wolf_max) fighter->stats().set_current(content::kGuidHealth, static_cast<float>(wolf_max->value * 0.6));   // (hurt: its Growl is open to it)
        w.npcs->note_player(77, px, py, now);
        w.npcs->tick(now);
        world.tick(now);
        const size_t first_new = wire.size();
        read_move_wire(*actor, shadow, now, wire);
        for (size_t i = first_new; i < wire.size(); ++i) {
            if (wire[i].rep_movement >= 0) held = wire[i].rep_movement;
            wire[i].rep_movement = held;   // (from here on: what the client holds once it has applied the update)
        }
        const GremCastView view = grem_cast_view(w.registry, wolf);
        if (view.montage != 0 && (view.element != last_view.element || last_view.montage == 0)) {
            Cast c;
            c.start = now;
            c.held = held;
            const abilities::AbilityDef* def = f.catalog->ability(view.ability);
            if (def) {
                for (const content::MontageRow& m : def->montages) {
                    const combat::MontageData* md = m.soft_path.empty() ? nullptr : tables.montage(m.soft_path);
                    if (md && md->guid == view.montage) c.in_place = md->root_motion.empty();
                }
            }
            casts.push_back(c);
        }
        last_view = view;
    }
    size_t places = 0, places_ignored = 0, lunges = 0, lunges_ignored = 0, turns = 0, turns_ignored = 0;
    for (const MoveWire& m : wire) {
        if (m.place) {
            ++places;
            if (m.rep_movement != 1) ++places_ignored;
            if (behind && m.t >= behind_at && std::fabs(m.yaw - yaw_before_turn) > 45.0 && std::fabs(m.yaw - yaw_before_turn) < 315.0) {
                ++turns;
                if (m.rep_movement != 1) ++turns_ignored;
            }
        }
        if (m.lunge) {
            ++lunges;
            if (m.rep_movement != 1) ++lunges_ignored;
        }
    }
    size_t in_place = 0, in_place_ignored = 0;
    for (const Cast& c : casts) {
        if (!c.in_place) continue;
        ++in_place;
        if (c.held != 1) ++in_place_ignored;
    }
    std::printf("  %zu casts in %.0f s, %zu of them in place; %zu place updates, %zu with a montage carrying the creature, %zu that turn it\n", casts.size(), t_end, in_place, places, lunges, turns);
    CHECKF(places >= 20, "the creature's places reach the client while it walks, fights and turns (%zu updates)", places);
    CHECKF(places_ignored == 0, "every place update reaches a client that applies it: the bit is on from the first one (%zu of %zu would be ignored)", places_ignored, places);
    CHECKF(in_place >= 1 && in_place_ignored == 0, "an attack that plays in place begins with the bit on (%zu such casts, %zu begin with it off)", in_place, in_place_ignored);
    CHECKF(turns >= 1 && turns_ignored == 0, "its turn to the player at its back reaches a client that applies it (%zu such updates, %zu ignored)", turns, turns_ignored);
    CHECKF(lunges_ignored == 0, "and so does every update of a lunge (%zu of %zu ignored)", lunges_ignored, lunges);
}

// ---- a creature whose tree gates an attack on a tree parameter and on its own health ----------------------------------------------------------------------------------------

void test_creature_parameter_and_health_gates(Fixture& f) {
    std::printf("the Alpha Wolf's Growl is gated by a tree parameter (the plain Wolf's is switched off) and by its own health (under 75 %%): the same fight, a full-health Alpha, a hurt Alpha and a hurt Wolf\n");
    std::string dir = NPC_MOTION_PATH;
    dir.erase(dir.find_last_of("/\\") + 1);
    combat::NpcAbilityData tables;
    std::string err;
    if (!tables.load(dir + "npc_abilities.tsv", dir + "npc_montages.tsv", dir + "npc_targeting.tsv", err)) {
        std::printf("  [skip] the creature tables (tools/npcabilities, the npcabilities extractor step): %s\n", err.c_str());
        return;
    }
    const auto path_of = [&](const char* leaf) {
        std::string found;
        for (const auto& [path, row] : f.motion.rows()) {
            if (path.find(leaf) != std::string::npos) found = path;
        }
        return found;
    };
    const std::string alpha_path = path_of("/Beast/Canine/AlphaWolf.Default__AlphaWolf_C"), wolf_path = path_of("/Beast/Canine/Wolf.Default__Wolf_C");
    CHECK(!alpha_path.empty() && !wolf_path.empty() && tables.abilities_of(alpha_path) && tables.abilities_of(wolf_path), "the Blueprint table has the Alpha Wolf and the Wolf and the creature tables list their abilities");
    if (alpha_path.empty() || wolf_path.empty() || !tables.abilities_of(alpha_path) || !tables.abilities_of(wolf_path)) return;
    const abilities::AbilityDef* growl = f.catalog->ability_named("AlphaWolf_Growl");
    const abilities::AbilityDef* bite = f.catalog->ability_named("Wolf_Bite");
    CHECK(growl && bite, "the ability data has the Growl and the Bite");
    if (!growl || !bite) return;
    // what the tables say about the Growl of each: switched off for the Wolf, usable (while it is hurt) for the Alpha
    const auto growl_task = [&](const std::string& path) -> const combat::CreatureAbility* {
        for (const combat::CreatureAbility& t : *tables.abilities_of(path)) {
            if (t.ability == growl->row.guid) return &t;
        }
        return nullptr;
    };
    const combat::CreatureAbility* alpha_task = growl_task(alpha_path);
    const combat::CreatureAbility* wolf_task = growl_task(wolf_path);
    CHECK(alpha_task && wolf_task, "both Blueprints' trees have the Growl");
    if (!alpha_task || !wolf_task) return;
    CHECKF(!alpha_task->off && !alpha_task->unknown && alpha_task->fight && !alpha_task->groups.empty(), "the Alpha Wolf's Growl is usable in the fight and carries a condition on its health (off %d, unknown %d, groups %zu)",
           alpha_task->off ? 1 : 0, alpha_task->unknown ? 1 : 0, alpha_task->groups.size());
    CHECK(wolf_task->off, "the plain Wolf's Growl is switched off by its tree's parameters");
    {
        // the load log's line for each: what a fight can use and what is left out
        std::string alpha_line, wolf_line;
        for (const std::string& line : tables.coverage_lines()) {
            if (line.rfind("AlphaWolf: ", 0) == 0) alpha_line = line;
            if (line.rfind("Wolf: ", 0) == 0) wolf_line = line;
        }
        std::printf("  load log: %s\n            %s\n", alpha_line.c_str(), wolf_line.c_str());
        const auto alpha_cover = tables.coverage().at(alpha_path), wolf_cover = tables.coverage().at(wolf_path);
        CHECKF(alpha_cover.usable > wolf_cover.usable && wolf_cover.off > alpha_cover.off && !alpha_line.empty() && !wolf_line.empty(),
               "the log counts the Alpha Wolf's usable tasks (%zu) above the Wolf's (%zu), and the Wolf's switched-off ones (%zu) above the Alpha's (%zu)", alpha_cover.usable, wolf_cover.usable, wolf_cover.off, alpha_cover.off);
    }

    const auto fight = [&](const std::string& path, double hurt_to, double seconds, bool needs_growl, int& growls, int& bites) {
        Rig rig(4, 20, {});
        if (!rig.ok()) return false;
        combat_rig::World w;
        if (!w.problem.empty()) return false;
        combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
        combat::CombatWorldConfig cfg;
        cfg.seed = 5;
        combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion, cfg);
        world.set_ability_data(&tables);
        const auto bp = f.motion.find(path);
        if (!bp) return false;
        const uint64_t wolf = w.spawn(path, 0, 0, 7, 900, bp->half_height);
        if (wolf == 0) return false;
        world::WorldActor* actor = w.registry.find(wolf);
        if ((needs_growl && !combat::npc_knows_ability(*actor, growl->row.guid)) || !combat::npc_knows_ability(*actor, bite->row.guid)) return false;
        combat_rig::FakePlayer player(rig, 77, {0, 800, 1098});
        world.add_player(&player);
        combat::Fighter* fighter = world.fighter_of(wolf);
        if (!fighter) return false;
        const double dt = 1.0 / 15.0;
        GremCastView last;
        double now = 0;
        bool hurt = false;
        while (now < seconds) {
            now += dt;
            if (!hurt && hurt_to < 1.0 && now >= 2.0) {   // the fight is on: the creature is hurt to a fraction of its health
                const stats::StatValue* hp = fighter->stats().find(content::kGuidMaxHealth);
                if (hp) fighter->stats().set_current(content::kGuidHealth, static_cast<float>(hp->value * hurt_to));
                hurt = true;
            }
            if (const stats::StatValue* max_hp = rig.cs->find(content::kGuidMaxHealth)) rig.cs->set_current(content::kGuidHealth, max_hp->value);
            w.npcs->note_player(77, 0, 800, now);
            w.npcs->tick(now);
            world.tick(now);
            const GremCastView view = grem_cast_view(w.registry, wolf);
            if (view.montage != 0 && (view.element != last.element || last.montage == 0)) {
                if (view.ability == growl->row.guid) ++growls;
                if (view.ability == bite->row.guid) ++bites;
            }
            last = view;
        }
        return true;
    };
    int full_growls = 0, full_bites = 0, hurt_growls = 0, hurt_bites = 0, wolf_growls = 0, wolf_bites = 0;
    const bool a = fight(alpha_path, 1.0, 16.0, true, full_growls, full_bites);
    const bool b = fight(alpha_path, 0.6, 16.0, true, hurt_growls, hurt_bites);
    const bool c = fight(wolf_path, 0.6, 16.0, false, wolf_growls, wolf_bites);
    CHECK(a && b && c, "the three fights run (the creatures are made from their opens and know the Bite, the Alpha the Growl too)");
    if (!(a && b && c)) return;
    std::printf("  Alpha Wolf at full health: %d bites, %d growls; hurt to 60 %%: %d bites, %d growls; plain Wolf hurt to 60 %%: %d bites, %d growls\n", full_bites, full_growls, hurt_bites, hurt_growls, wolf_bites, wolf_growls);
    CHECKF(full_bites >= 3 && full_growls == 0, "the Alpha Wolf at full health bites and never growls (%d bites, %d growls)", full_bites, full_growls);
    CHECKF(hurt_growls >= 1 && hurt_bites >= 1, "hurt under 75 %% it growls (%d growls, %d bites)", hurt_growls, hurt_bites);
    CHECKF(wolf_bites >= 3 && wolf_growls == 0, "the plain Wolf, hurt as much, still never growls: its tree switches the Growl off (%d bites, %d growls)", wolf_bites, wolf_growls);
}

// ---- a creature whose tree rolls for an attack ---------------------------------------------------------------------------------------------------------------------------------

void test_creature_roll_gate(Fixture& f) {
    std::printf("the Bear's right swing is gated by a roll of one half, asked afresh at every try: over a long fight it swings right and left, neither always\n");
    std::string dir = NPC_MOTION_PATH;
    dir.erase(dir.find_last_of("/\\") + 1);
    combat::NpcAbilityData tables;
    std::string err;
    if (!tables.load(dir + "npc_abilities.tsv", dir + "npc_montages.tsv", dir + "npc_targeting.tsv", err)) {
        std::printf("  [skip] the creature tables (tools/npcabilities, the npcabilities extractor step): %s\n", err.c_str());
        return;
    }
    std::string bear_path;
    for (const auto& [path, row] : f.motion.rows()) {
        if (path.find("/Beast/Bear/Bear.Default__Bear_C") != std::string::npos) bear_path = path;
    }
    const auto bp = f.motion.find(bear_path);
    CHECK(!bear_path.empty() && bp && tables.abilities_of(bear_path) != nullptr, "the Blueprint table has the Bear and the creature tables list its abilities");
    if (bear_path.empty() || !bp || !tables.abilities_of(bear_path)) return;
    const abilities::AbilityDef* right = f.catalog->ability_named("Bear_RightSwing");
    const abilities::AbilityDef* left = f.catalog->ability_named("Bear_LeftSwing");
    CHECK(right && left, "the ability data has the Bear's swings");
    if (!right || !left) return;
    const combat::CreatureAbility* right_task = nullptr;
    for (const combat::CreatureAbility& t : *tables.abilities_of(bear_path)) {
        if (t.ability == right->row.guid) right_task = &t;
    }
    bool has_roll = false;
    if (right_task) {
        for (const combat::CondGroup& g : right_task->groups) {
            for (const combat::CondToken& t : g) has_roll = has_roll || t.atom.kind == combat::CondAtom::Kind::Random;
        }
    }
    CHECK(right_task && !right_task->unknown && !right_task->off && has_roll, "the tables carry the right swing with a roll among its conditions");
    if (!right_task) return;

    Rig rig(4, 20, {});
    if (!rig.ok()) return;
    combat_rig::World w;
    if (!w.problem.empty()) return;
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorldConfig cfg;
    cfg.seed = 5;
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion, cfg);
    world.set_ability_data(&tables);
    const uint64_t bear = w.spawn(bear_path, 0, 0, 7, 900, bp->half_height);
    CHECK(bear != 0, "a Bear is made from its open");
    if (bear == 0) return;
    CHECK(combat::npc_knows_ability(*w.registry.find(bear), right->row.guid) && combat::npc_knows_ability(*w.registry.find(bear), left->row.guid), "its open lists both swings, so the world plays their animations");
    combat_rig::FakePlayer player(rig, 77, {0, 800, 1098});
    world.add_player(&player);
    const double dt = 1.0 / 15.0;
    GremCastView last;
    int rights = 0, lefts = 0, others = 0;
    double now = 0;
    while (now < 60.0) {
        now += dt;
        if (const stats::StatValue* max_hp = rig.cs->find(content::kGuidMaxHealth)) rig.cs->set_current(content::kGuidHealth, max_hp->value);
        w.npcs->note_player(77, 0, 800, now);
        w.npcs->tick(now);
        world.tick(now);
        const GremCastView view = grem_cast_view(w.registry, bear);
        if (view.montage != 0 && (view.element != last.element || last.montage == 0)) {
            if (view.ability == right->row.guid) ++rights;
            else if (view.ability == left->row.guid) ++lefts;
            else ++others;
        }
        last = view;
    }
    std::printf("  60 s against a standing player: %d right swings, %d left swings, %d other attacks\n", rights, lefts, others);
    CHECKF(rights >= 3 && lefts >= 3, "the swings come in both hands: the roll lets the right one through about half the time (%d right, %d left)", rights, lefts);
    CHECKF(rights + lefts >= 10, "and the Bear keeps attacking while it rolls (%d swings in 60 s)", rights + lefts);
}

// ---- fix batch 1: the reach of a bite whose record states none, the pause of a lunge, a lunge the AI's walk orders do not cut, a creature with nothing to play ---------------------------

/// What one update of a creature's actor block says about a lunge, as a client reads it.
struct LungeWire {
    double  t = 0;
    int     active = -1;       // RepRootMotion.bIsActive (-1: not in the block)
    int     obstructed = -1;   // bRootMotionObstructed
    double  position = -1;     // RepRootMotion.Position
    bool    has_mv_yaw = false, has_rrm_yaw = false;
    double  mv_yaw = 0, rrm_yaw = 0;   // the facing ReplicatedMovement (8 or 16 bits) and RepRootMotion.Rotation (16 bits) say, in degrees, as a client reads them
};

void read_lunge_wire(world::WorldActor& actor, aoc::protocol::repl::ActorShadow& shadow, double now, std::vector<LungeWire>& out) {
    std::vector<aoc::protocol::repl::ContentBlock> blocks;
    if (!aoc::protocol::repl::collect_update_blocks(actor.actor, shadow, blocks)) return;
    for (const aoc::protocol::repl::ContentBlock& b : blocks) {
        if (b.sub || !b.has_rep_layout) continue;
        const auto st = aoc::protocol::repl::load_object_state(*actor.actor.obj.schema, b.payload.data(), b.payload.bit_pos(), true);
        if (!st) continue;
        const aoc::protocol::schema::ClassSchema& sc = *actor.actor.obj.schema;
        LungeWire w;
        w.t = now;
        if (const auto* v = st->state.get(sc, "RepRootMotion.bIsActive")) {
            if (v->get_if<bool>()) w.active = *v->get_if<bool>() ? 1 : 0;
        }
        if (const auto* v = st->state.get(sc, "bRootMotionObstructed")) {
            if (v->get_if<bool>()) w.obstructed = *v->get_if<bool>() ? 1 : 0;
        }
        if (const auto* v = st->state.get(sc, "RepRootMotion.Position")) {
            if (v->get_if<float>()) w.position = *v->get_if<float>();
        }
        if (const auto* v = st->state.get(sc, "ReplicatedMovement")) {
            if (const auto* m = v->get_if<aoc::protocol::repl::RepMovement>()) {
                w.has_mv_yaw = true;
                w.mv_yaw = m->rotation.yaw * 360.0 / (st->opts.rep_movement_short_rotation ? 65536.0 : 256.0);
            }
        }
        if (const auto* v = st->state.get(sc, "RepRootMotion.Rotation")) {
            if (const auto* r = v->get_if<aoc::protocol::repl::Rot>()) {
                w.has_rrm_yaw = true;
                w.rrm_yaw = r->yaw * 360.0 / 65536.0;
            }
        }
        out.push_back(w);
    }
}

/// One tick of a fight between a creature and a player: where the creature was, where the player was, and what a client that has the creature open holds by then (whether a montage carries it,
/// whether it is held, how far the montage has played).
struct LungeSample {
    double   t = 0, wx = 0, wy = 0, px = 0, py = 0, position = -1;
    int      active = 0, obstructed = 0;
    int64_t  ability = 0;      // the ability of the cast element the creature's AbilityComponent holds
    uint64_t montage = 0;      // ... and its montage (0: none is playing)
    bool     cast_began = false;
    double   yaw = 0;          // the facing the server has
    double   mv_yaw = 0, rrm_yaw = 0;   // the facing a client holds by this tick: the last ReplicatedMovement and RepRootMotion.Rotation it was sent
    bool     has_mv_yaw = false, has_rrm_yaw = false;
};

static std::string creature_path(Fixture& f, const char* leaf) {
    std::string found;
    for (const auto& [path, row] : f.motion.rows()) {
        if (path.find(leaf) != std::string::npos) found = path;
    }
    return found;
}

/// A fight of one creature against one player, tick by tick, as the client that watches it would hold it.  `steer` is asked at every tick before the world moves (with the sample of the tick before)
/// and may put the player somewhere else; it returns true when it did.  `timid` makes the creature one that does not start a fight; `provoke_at` (if >= 0) is when the player starts hitting it;
/// `leash_cm` (if > 0) is how far from home the creature chases.
using LungeSteer = std::function<bool(double now, const LungeSample* prev, double& px, double& py, const world::WorldActor& creature)>;
static bool run_lunge_fight(Fixture& f, const combat::NpcAbilityData& tables, const std::string& path, double seconds, double player_x, double player_y, const LungeSteer& steer,
                            std::vector<LungeSample>& out, std::vector<combat::IncomingHit>* hits = nullptr, bool timid = false, double provoke_at = -1, combat::CombatWorld::Stats* stats = nullptr,
                            double leash_cm = 0) {
    Rig rig(4, 20, {});
    if (!rig.ok()) return false;
    combat_rig::World w;
    if (!w.problem.empty()) return false;
    const auto bp = f.motion.find(path);
    if (!bp) return false;
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorldConfig cfg;
    cfg.seed = 5;
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion, cfg);
    world.set_ability_data(&tables);
    const uint64_t creature = w.spawn(path, 0, 0, 7, 900, bp->half_height);
    if (creature == 0) return false;
    world::WorldActor* actor = w.registry.find(creature);
    if (timid) actor->npc.aggro_off = true;
    if (leash_cm > 0) actor->npc.leash_radius_cm = leash_cm;   // (a chase that goes far must not be given up)
    aoc::protocol::repl::ActorShadow shadow;
    {
        aoc::protocol::repl::ExportAcks acks;
        bool failed = false;
        const auto open = aoc::protocol::repl::open_actor(actor->actor, shadow, acks, &failed);
        if (!open.has_value() || failed) return false;
    }
    double px = player_x, py = player_y;
    combat_rig::FakePlayer player(rig, 77, {px, py, 1098});
    world.add_player(&player);
    const double dt = 1.0 / 15.0;
    GremCastView last;
    LungeSample prev;
    bool have_prev = false;
    int active = 0, obstructed = 0;
    double mv_yaw = 0, rrm_yaw = 0;
    bool has_mv_yaw = false, has_rrm_yaw = false;
    double now = 0, next_hit = provoke_at;
    while (now < seconds) {
        now += dt;
        if (steer && steer(now, have_prev ? &prev : nullptr, px, py, *w.registry.find(creature))) player.rig_fighter().move_to({px, py, 1098});
        if (next_hit >= 0 && now >= next_hit) {   // the player hits it, and goes on hitting it every second: the threat a hit gives
            next_hit = now + 1.0;
            world.add_threat(creature, 77, 100.0, now);
        }
        if (const stats::StatValue* max_hp = rig.cs->find(content::kGuidMaxHealth)) rig.cs->set_current(content::kGuidHealth, max_hp->value);   // (the fight is about the creature, not the player's life; asked afresh: a hit rebuilds the stats)
        w.npcs->note_player(77, px, py, now);
        w.npcs->tick(now);
        world.tick(now);
        std::vector<LungeWire> wire;
        read_lunge_wire(*actor, shadow, now, wire);
        double position = -1;
        for (const LungeWire& u : wire) {
            if (u.active >= 0) active = u.active;
            if (u.obstructed >= 0) obstructed = u.obstructed;
            if (u.position >= 0) position = u.position;
            if (u.has_mv_yaw) mv_yaw = u.mv_yaw, has_mv_yaw = true;
            if (u.has_rrm_yaw) rrm_yaw = u.rrm_yaw, has_rrm_yaw = true;
        }
        const GremCastView view = grem_cast_view(w.registry, creature);
        const world::WorldActor* g = w.registry.find(creature);
        LungeSample s;
        s.t = now;
        s.wx = g->x;
        s.wy = g->y;
        s.px = px;
        s.py = py;
        s.position = position;
        s.active = active;
        s.obstructed = obstructed;
        s.ability = view.ability;
        s.montage = view.montage;
        s.cast_began = view.montage != 0 && (view.element != last.element || last.montage == 0);
        s.yaw = g->yaw_degrees;
        s.mv_yaw = mv_yaw;
        s.rrm_yaw = rrm_yaw;
        s.has_mv_yaw = has_mv_yaw;
        s.has_rrm_yaw = has_rrm_yaw;
        last = view;
        out.push_back(s);
        prev = s;
        have_prev = true;
    }
    if (hits) *hits = player.hits;
    if (stats) *stats = world.stats();
    return true;
}

/// The lunges in a fight's samples: runs of ticks a montage carries the creature in, `last` being the last of them (the tick after it is the one that wrote the end of the carry).
struct LungeRun {
    size_t first = 0, last = 0;
};
static std::vector<LungeRun> lunge_runs(const std::vector<LungeSample>& s, int64_t ability = 0) {
    std::vector<LungeRun> runs;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i].active != 1 || (i > 0 && s[i - 1].active == 1)) continue;
        size_t j = i;
        while (j + 1 < s.size() && s[j + 1].active == 1) ++j;
        if ((ability == 0 || s[i].ability == ability) && j + 1 < s.size()) runs.push_back({i, j});   // (a lunge the fight ended in is left out)
    }
    return runs;
}

/// The montage data of the montage a cast element named, among the montages of the ability.
static const combat::MontageData* montage_of_cast(Fixture& f, const combat::NpcAbilityData& tables, int64_t ability, uint64_t montage) {
    const abilities::AbilityDef* def = f.catalog->ability(ability);
    if (!def) return nullptr;
    for (const content::MontageRow& m : def->montages) {
        const combat::MontageData* md = m.soft_path.empty() ? nullptr : tables.montage(m.soft_path);
        if (md && md->guid == montage) return md;
    }
    return nullptr;
}

static bool load_creature_tables(combat::NpcAbilityData& tables) {
    std::string dir = NPC_MOTION_PATH;
    dir.erase(dir.find_last_of("/\\") + 1);
    std::string err;
    if (tables.load(dir + "npc_abilities.tsv", dir + "npc_montages.tsv", dir + "npc_targeting.tsv", err)) return true;
    std::printf("  [skip] the creature tables (tools/npcabilities, the npcabilities extractor step): %s\n", err.c_str());
    return false;
}

void test_bite_lands_wherever_the_player_stands(Fixture& f) {
    std::printf("a Wolf's bite, whose targeting record measures nothing (validity off): at its notify it lands on its target wherever he stands, in front of it, behind it or far away\n");
    constexpr double kPi = 3.14159265358979323846;
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string wolf_path = creature_path(f, "/Beast/Canine/Wolf.Default__Wolf_C");
    const abilities::AbilityDef* bite = f.catalog->ability_named("Wolf_Bite");
    CHECK(!wolf_path.empty() && f.motion.find(wolf_path) && tables.abilities_of(wolf_path) && bite, "the Blueprint table has the Wolf, the creature tables list its abilities and the ability data has the Bite");
    if (wolf_path.empty() || !f.motion.find(wolf_path) || !tables.abilities_of(wolf_path) || !bite) return;
    const combat::BiteArea* record = tables.area_of(bite->row.guid);
    const combat::RootMotionFreezeRule* freeze = tables.freeze_of(bite->row.guid);
    CHECK(record && record->gate() == combat::BiteArea::Gate::Any && record->discovery == combat::BiteArea::Discovery::PrefersActive && !record->check_validity && record->volume_count == 0 && freeze &&
              freeze->kind == combat::RootMotionFreezeRule::Kind::Pause && freeze->distance > 0,
          "the Bite's targeting record finds the creature's own target, measures nothing and sweeps no volume (an Any record), and its lunge pauses on contact");
    if (!record || !freeze) return;

    // the player stands in front for 30 s; then again, but steps behind the Wolf as each bite begins; then again, but is put 25 m away as each bite begins
    const auto fight = [&](int where, int& bites, int& hits) {
        double flip_at = -1;
        std::vector<LungeSample> samples;
        std::vector<combat::IncomingHit> got;
        const LungeSteer steer = [&](double now, const LungeSample* prev, double& px, double& py, const world::WorldActor& wolf) {
            if (where != 0 && prev && prev->cast_began && prev->ability == bite->row.guid) flip_at = now + 0.1;
            if (flip_at < 0 || now < flip_at) return false;
            flip_at = -1;
            const double d = where == 1 ? -150.0 : 2500.0;   // behind it, or far ahead of it
            px = wolf.x + d * std::cos(wolf.yaw_degrees * kPi / 180.0);
            py = wolf.y + d * std::sin(wolf.yaw_degrees * kPi / 180.0);
            return true;
        };
        if (!run_lunge_fight(f, tables, wolf_path, 30.0, 0, 800, steer, samples, &got, false, -1, nullptr, 40000.0)) return false;   // (a fight that drifts far must not be given up)
        bites = 0;
        hits = 0;
        for (const LungeSample& s : samples) {
            if (s.cast_began && s.ability == bite->row.guid) ++bites;
        }
        for (const combat::IncomingHit& h : got) {
            if (h.source_guid == bite->row.guid) ++hits;
        }
        return true;
    };
    int front_bites = 0, front_hits = 0, behind_bites = 0, behind_hits = 0, far_bites = 0, far_hits = 0;
    const bool a = fight(0, front_bites, front_hits);
    const bool b = fight(1, behind_bites, behind_hits);
    const bool c = fight(2, far_bites, far_hits);
    CHECK(a && b && c, "the three fights run (the Wolf is made from its open and knows the Bite)");
    if (!a || !b || !c) return;
    std::printf("  30 s against a player who stands in front: %d bites, %d reached him; who steps behind the Wolf as each bite begins: %d bites, %d reached him; who is put 25 m away as each bite begins: %d bites, %d "
                "reached him\n", front_bites, front_hits, behind_bites, behind_hits, far_bites, far_hits);
    CHECKF(front_bites >= 5 && front_hits >= front_bites - 1, "every bite of the Wolf reaches a player who stands in front of it (%d of %d; the last one may be still in the air when the fight ends)", front_hits, front_bites);
    CHECKF(behind_bites >= 4 && behind_hits >= behind_bites - 1, "... and one who has stepped behind it when the bite lands (%d of %d bites reached him)", behind_hits, behind_bites);
    CHECKF(far_bites >= 2 && far_hits >= far_bites - 1, "... and one who is 25 m away when it lands (%d of %d bites reached him)", far_hits, far_bites);
}

void test_lunge_pauses_at_the_player(Fixture& f) {
    std::printf("a Wolf's lunge pauses on contact: held ahead of the player while he is in its box, with bRootMotionObstructed going out; when the box is clear the flag clears and the lunge goes on by what the montage still travels\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string wolf_path = creature_path(f, "/Beast/Canine/Wolf.Default__Wolf_C");
    const auto bp = f.motion.find(wolf_path);
    const abilities::AbilityDef* bite = f.catalog->ability_named("Wolf_Bite");
    CHECK(!wolf_path.empty() && bp && tables.abilities_of(wolf_path) && bite, "the Blueprint table has the Wolf, the creature tables list its abilities and the ability data has the Bite");
    if (wolf_path.empty() || !bp || !tables.abilities_of(wolf_path) || !bite) return;
    const combat::RootMotionFreezeRule* freeze = tables.freeze_of(bite->row.guid);
    CHECK(freeze && freeze->kind == combat::RootMotionFreezeRule::Kind::Pause && freeze->distance > 0, "the Bite's ability record pauses its lunge on contact");
    if (!freeze) return;
    const double r_wolf = bp->radius, r_player = 34.0;

    // the player stands in front: the lunge ends up held ahead of him
    std::vector<LungeSample> stand;
    CHECK(run_lunge_fight(f, tables, wolf_path, 30.0, 0, 800, nullptr, stand), "a fight against a player who stands still runs");
    const std::vector<LungeRun> runs = lunge_runs(stand, bite->row.guid);
    size_t held_runs = 0, creeping = 0, uncleared = 0, silent = 0;
    double closest = 1e9, farthest = 0, slowest_creep = 0;
    for (const LungeRun& r : runs) {
        bool held = false;
        for (size_t i = r.first; i <= r.last; ++i) {
            if (stand[i].obstructed != 1) continue;
            held = true;
            const double gap = std::hypot(stand[i].wx - stand[i].px, stand[i].wy - stand[i].py) - r_wolf - r_player;
            closest = std::min(closest, gap);
            farthest = std::max(farthest, gap);
            if (i > r.first && stand[i - 1].obstructed == 1) {   // held: it does not move from one tick to the next
                const double d = std::hypot(stand[i].wx - stand[i - 1].wx, stand[i].wy - stand[i - 1].wy);
                slowest_creep = std::max(slowest_creep, d);
                if (d > 0.5) ++creeping;
            }
        }
        if (held) ++held_runs;
        if (stand[r.last + 1].obstructed != 0) ++uncleared;   // (the end of the carry clears it)
        if (!held) ++silent;
    }
    std::printf("  %zu lunges of the Bite against a player who stands still: %zu held (flag 1) ahead of him, %.1f to %.1f cm from his surface, the pause distance %.0f cm\n", runs.size(), held_runs, closest,
                farthest, freeze->distance);
    CHECKF(runs.size() >= 5 && silent == 0, "every lunge of the Bite is held by the player in its box, with the flag going out (%zu of %zu lunges never carried it)", silent, runs.size());
    CHECKF(closest >= freeze->distance - 2.0 && farthest <= freeze->distance + 0.5, "the Wolf is held at the pause distance, at the box's edge, and does not come nearer: %.1f to %.1f cm of surface for a pause distance of %.0f cm", closest, farthest,
           freeze->distance);
    CHECKF(creeping == 0, "while the flag is on the Wolf does not move (%zu ticks moved, the most %.2f cm)", creeping, slowest_creep);
    CHECKF(uncleared == 0, "the flag is off again when the carry ends (%zu lunges end with it on)", uncleared);

    // the player steps out of the box as the Wolf is held: the flag clears at once and the Wolf goes on by the part of the montage that is left, not by what it skipped
    double leave_at = -1, side = 700.0;
    std::vector<LungeSample> leave;
    const LungeSteer steer = [&](double now, const LungeSample* prev, double& px, double& py, const world::WorldActor&) {
        (void)py;
        if (leave_at < 0 && prev && prev->active == 1 && prev->obstructed == 1 && prev->ability == bite->row.guid) leave_at = now;   // (the tick after the Wolf was held)
        if (leave_at < 0 || now < leave_at) return false;
        leave_at = -1;
        px += side;
        side = -side;
        return true;
    };
    CHECK(run_lunge_fight(f, tables, wolf_path, 40.0, 0, 800, steer, leave), "a fight against a player who steps out of the box once the Wolf is held runs");
    size_t cleared = 0, slow_clear = 0, moved_on = 0, wrong_travel = 0;
    double worst_travel = 0;
    for (const LungeRun& r : lunge_runs(leave, bite->row.guid)) {
        size_t held = r.last + 1, gone = r.last + 1, clear = r.last + 1;
        for (size_t i = r.first; i <= r.last; ++i) {
            if (held > r.last && leave[i].obstructed == 1) held = i;
            if (held <= r.last && gone > r.last && leave[i].px != leave[held].px) gone = i;
            if (held <= r.last && clear > r.last && i > held && leave[i].obstructed == 0) clear = i;
        }
        if (held > r.last || clear > r.last) continue;   // never held, or held to the end of the carry
        ++cleared;
        if (gone > r.last || clear > gone + 2) ++slow_clear;
        const combat::MontageData* md = montage_of_cast(f, tables, leave[r.first].ability, leave[r.first].montage);
        if (!md || leave[clear - 1].position < 0) continue;
        const combat::MontageData::Step from = md->root_at(leave[clear - 1].position), to = md->root_at(md->length - 0.25);
        const double expected = std::hypot(to.x - from.x, to.y - from.y);
        const double observed = std::hypot(leave[r.last + 1].wx - leave[clear - 1].wx, leave[r.last + 1].wy - leave[clear - 1].wy);
        worst_travel = std::max(worst_travel, std::fabs(observed - expected));
        if (observed > 5.0) ++moved_on;
        if (std::fabs(observed - expected) > 12.0) ++wrong_travel;
    }
    std::printf("  %zu lunges had the player step out while the Wolf was held: the flag cleared within two ticks in %zu, the Wolf went on in %zu, its travel off the montage's by %.1f cm at the most\n", cleared,
                cleared - slow_clear, moved_on, worst_travel);
    CHECKF(cleared >= 3 && slow_clear == 0, "once the player is out of the box the flag clears within two ticks (%zu of %zu cleared late)", slow_clear, cleared);
    CHECKF(moved_on >= 1 && wrong_travel == 0, "the lunge then goes on by what the montage still travels (%zu went on; %zu travelled other than the montage's remainder, the worst %.1f cm)", moved_on, wrong_travel, worst_travel);
}

void test_swing_is_not_cut_by_walk_orders(Fixture& f) {
    std::printf("a Bear's swings carry it to the end of their travel even when the player side-steps and the AI wants to walk: the walk order waits for the carry to end\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string bear_path = creature_path(f, "/Beast/Bear/Bear.Default__Bear_C");
    CHECK(!bear_path.empty() && f.motion.find(bear_path) && tables.abilities_of(bear_path), "the Blueprint table has the Bear and the creature tables list its abilities");
    if (bear_path.empty() || !f.motion.find(bear_path) || !tables.abilities_of(bear_path)) return;
    // each swing begun, the player side-steps 3 m a moment later: out of the Bear's reach, so its AI orders a walk after him while the montage is still carrying it
    double step_at = -1, side = 300.0;
    size_t steps = 0;
    std::vector<LungeSample> samples;
    const LungeSteer steer = [&](double now, const LungeSample* prev, double& px, double& py, const world::WorldActor&) {
        (void)py;
        if (prev && prev->cast_began) step_at = now + 0.1;
        if (step_at < 0 || now < step_at) return false;
        step_at = -1;
        px += side;
        side = -side;
        ++steps;
        return true;
    };
    CHECK(run_lunge_fight(f, tables, bear_path, 45.0, 0, 800, steer, samples), "a fight against a player who side-steps at every swing runs");
    size_t lunges = 0, cut = 0;
    double worst = 0;
    for (const LungeRun& r : lunge_runs(samples)) {
        const combat::MontageData* md = montage_of_cast(f, tables, samples[r.first].ability, samples[r.first].montage);
        if (!md) continue;
        ++lunges;
        const double expected = (md->length - 0.25) / md->rate_scale;   // seconds the montage carries the creature
        const double played = samples[r.last + 1].t - samples[r.first].t;
        worst = std::max(worst, expected - played);
        if (played < expected - 0.2) ++cut;
    }
    std::printf("  %zu side-steps, %zu lunges in 45 s: %zu carried less than the montage's travel (the worst short by %.2f s)\n", steps, lunges, cut, worst);
    CHECKF(lunges >= 4 && cut == 0, "every lunge is carried to the end of the montage's travel, whatever the AI orders (%zu of %zu cut short, the worst by %.2f s)", cut, lunges, worst);
}

void test_timid_raven_swings_back(Fixture& f) {
    std::printf("a Timid Raven (a Blueprint no capture opened, so the generic open that lists none of its tree's abilities): it starts no fight, and once it is hit it swings back with the single swing every creature has\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string raven_path = creature_path(f, "/Raven/Raven_Timid.Default__Raven_Timid_C");
    const abilities::AbilityDef* peck = f.catalog->ability_named("Raven_Peck");
    CHECK(!raven_path.empty() && f.motion.find(raven_path) && tables.abilities_of(raven_path) && peck, "the Blueprint table has the Timid Raven, the creature tables list its abilities and the ability data has the Peck");
    if (raven_path.empty() || !f.motion.find(raven_path) || !tables.abilities_of(raven_path) || !peck) return;
    bool lists_peck = false;
    for (const combat::CreatureAbility& t : *tables.abilities_of(raven_path)) lists_peck = lists_peck || t.ability == peck->row.guid;
    CHECK(lists_peck, "the Timid Raven's tree uses the Peck");
    // the same Raven, Timid, with the player 4 m away: nothing happens for 8 s, and then the player hits it
    std::vector<LungeSample> samples;
    std::vector<combat::IncomingHit> hits;
    combat::CombatWorld::Stats stats;
    CHECK(run_lunge_fight(f, tables, raven_path, 20.0, 0, 400, nullptr, samples, &hits, true, 8.0, &stats), "a fight against a Timid Raven that is hit at 8 s runs");
    size_t before = 0, after = 0;
    for (const combat::IncomingHit& h : hits) {
        if (h.at < 8.0) {
            ++before;
        } else {
            ++after;
        }
    }
    std::printf("  hits on the player: %zu before the Raven was hit, %zu after (%llu attacks in all)\n", before, after, static_cast<unsigned long long>(stats.npc_attacks));
    CHECKF(before == 0, "a Timid Raven starts no fight (%zu hits before it was hit)", before);
    CHECKF(after >= 2 && stats.npc_attacks >= 2, "once it is hit it swings back at the player (%zu hits, %llu attacks)", after, static_cast<unsigned long long>(stats.npc_attacks));
}

// ---- fix batch 2: the chase of a player who walks away, turning at the creature's rate, and a swing that faces its target -----------------------------------------------------

/// What a fight between a creature and a player who runs away shows: whether the creature stops while it chases, when it is first within reach, what it attacks and what hits.
struct ChaseReport {
    size_t halts = 0, attacks = 0, hits = 0, ticks = 0;
    double first_attack_at = -1, closest_gap = 1e9, worst_turn = 0, run_speed = 0, turn_rate = 0;
    // the bites of the attacks that began (those whose notify fell due before the fight ended) and what became of them: by the kind of rule of their targeting record
    size_t any_due = 0, any_landed = 0, any_missed = 0, any_blocked = 0, any_crit = 0;   // records that measure nothing (the target wherever he stands)
    size_t cone_due = 0, cone_landed = 0;                                                  // records that measure (validity on)
};

/// The hit notifies of a cast that fire a hit which hurts an enemy: seconds after its start.
static std::vector<double> cast_bites(Fixture& f, const combat::NpcAbilityData& tables, int64_t ability, uint64_t montage) {
    std::vector<double> out;
    const abilities::AbilityDef* def = f.catalog->ability(ability);
    const combat::MontageData* md = montage_of_cast(f, tables, ability, montage);
    if (!def || !md) return out;
    for (const combat::MontageData::Hit& h : md->hits) {
        for (const content::AbilityHitRow& r : def->hits) {
            if (r.source != "hit" || r.key != h.key) continue;
            const abilities::HitDef* hit = f.catalog->hit(r.hit_guid);
            if (hit && (hit->row.applicable_targets & 2) && !hit->mods.empty()) out.push_back(std::max(0.1, h.time / md->rate_scale));
            break;
        }
    }
    return out;
}

/// The player runs along +y at `run_cm_s` (zig-zagging `zig_degrees` either side of it, the side changing every second, when that is not 0) from 5 m in front of the creature for `seconds`.
static bool run_chase(Fixture& f, const combat::NpcAbilityData& tables, const std::string& path, double run_cm_s, double zig_degrees, double seconds, ChaseReport& out) {
    constexpr double kPi = 3.14159265358979323846;
    const auto bp = f.motion.find(path);
    if (!bp) return false;
    const double dt = 1.0 / 15.0;
    const LungeSteer steer = [&](double now, const LungeSample*, double& x, double& y, const world::WorldActor&) {
        const double heading = 90.0 + (zig_degrees > 0 ? ((static_cast<int>(now) % 2) ? zig_degrees : -zig_degrees) : 0.0);
        x += run_cm_s * dt * std::cos(heading * kPi / 180.0);
        y += run_cm_s * dt * std::sin(heading * kPi / 180.0);
        return true;
    };
    std::vector<LungeSample> samples;
    std::vector<combat::IncomingHit> hits;
    if (!run_lunge_fight(f, tables, path, seconds, 0, 500, steer, samples, &hits, false, -1, nullptr, 40000.0)) return false;
    out = ChaseReport{};
    out.run_speed = bp->max_run_speed;
    out.turn_rate = bp->rotation_rate_yaw;
    out.hits = hits.size();
    double last_cast_t = -10;
    for (size_t i = 1; i < samples.size(); ++i) {
        const LungeSample& s = samples[i];
        const double speed = std::hypot(s.wx - samples[i - 1].wx, s.wy - samples[i - 1].wy) / dt;
        const double gap = std::hypot(s.wx - s.px, s.wy - s.py) - bp->radius - 34.0;
        out.closest_gap = std::min(out.closest_gap, gap);
        out.worst_turn = std::max(out.worst_turn, std::fabs(wrap180(s.yaw - samples[i - 1].yaw)));
        if (s.cast_began) {
            ++out.attacks;
            if (out.first_attack_at < 0) out.first_attack_at = s.t;
        }
        if (s.montage != 0) last_cast_t = s.t;
        // it is chasing: no montage plays (nor played in the last half second), the player is beyond any reach it has, and the fight has begun
        if (s.montage == 0 && s.t - last_cast_t > 0.5 && gap > 450.0 && s.t > 1.0) {
            ++out.ticks;
            if (speed < 0.05 * out.run_speed) ++out.halts;
        }
        // the bites of the attack that began now: each is due at its notify, and counts if that fell due before the fight ended
        if (s.cast_began) {
            const combat::BiteArea* area = tables.area_of(s.ability);
            const bool measures = area && area->gate() == combat::BiteArea::Gate::Validity;
            for (const double at : cast_bites(f, tables, s.ability, s.montage)) {
                if (s.t + at > seconds - 0.2) continue;
                size_t& due = measures ? out.cone_due : out.any_due;
                ++due;
            }
            const combat::MontageData* md = montage_of_cast(f, tables, s.ability, s.montage);
            const double span = md ? md->seconds(1.0) : 0.0;
            for (const combat::IncomingHit& h : hits) {
                if (h.source_guid != s.ability || h.at < s.t - 0.05 || h.at > s.t + span + 0.25) continue;
                if (measures) {
                    ++out.cone_landed;
                    continue;
                }
                ++out.any_landed;
                if (h.flags & stats::kHitMiss) ++out.any_missed;
                if (h.flags & stats::kHitBlock) ++out.any_blocked;
                if (h.flags & stats::kHitCrit) ++out.any_crit;
            }
        }
    }
    return true;
}

void test_chase_of_a_player_who_walks_away(Fixture& f) {
    std::printf("a creature chases a player who runs away at 720 cm/s, straight and zig-zagging: it never stops while it chases, and it catches up and attacks\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    struct Kind {
        const char* name;
        const char* leaf;
    };
    const Kind kinds[] = {{"Wolf", "/Beast/Canine/Wolf.Default__Wolf_C"}, {"Bear", "/Beast/Bear/Bear.Default__Bear_C"}, {"Grem", "/Beast/Rabid/Rabid.Default__Rabid_C"}};
    for (const Kind& k : kinds) {
        const std::string path = creature_path(f, k.leaf);
        CHECKF(!path.empty() && f.motion.find(path) && tables.abilities_of(path), "the Blueprint table has the %s and the creature tables list its abilities", k.name);
        if (path.empty() || !f.motion.find(path) || !tables.abilities_of(path)) continue;
        ChaseReport straight, zig, walk;
        const bool a = run_chase(f, tables, path, 720.0, 0.0, 12.0, straight);
        const bool b = run_chase(f, tables, path, 720.0, 60.0, 12.0, zig);
        const bool c = run_chase(f, tables, path, 360.0, 0.0, 12.0, walk);
        CHECKF(a && b && c, "the three chases of the %s run", k.name);
        if (!a || !b || !c) continue;
        std::printf("  %-4s (runs %.0f cm/s, turns %.0f deg/s) after a player at 720 cm/s, 12 s:\n", k.name, straight.run_speed, straight.turn_rate);
        std::printf("       straight:   %zu attacks (the first at %.1f s), %zu hits, closest %.0f cm from his surface, stopped %zu of %zu chasing ticks, its facing turned %.1f deg at the most in a tick\n", straight.attacks,
                    straight.first_attack_at, straight.hits, straight.closest_gap, straight.halts, straight.ticks, straight.worst_turn);
        std::printf("       zig-zag:    %zu attacks (the first at %.1f s), %zu hits, closest %.0f cm from his surface, stopped %zu of %zu chasing ticks, its facing turned %.1f deg at the most in a tick\n", zig.attacks,
                    zig.first_attack_at, zig.hits, zig.closest_gap, zig.halts, zig.ticks, zig.worst_turn);
        std::printf("       half speed: %zu attacks (the first at %.1f s), %zu hits, closest %.0f cm from his surface, stopped %zu of %zu chasing ticks\n", walk.attacks, walk.first_attack_at, walk.hits, walk.closest_gap, walk.halts,
                    walk.ticks);
        const auto bites_line = [](const char* label, const ChaseReport& r) {
            std::printf("       %-10s bites due %zu on the target wherever he stands: %zu landed (%zu of them rolled a miss, %zu a block, %zu a critical hit); inside a cone: %zu due, %zu landed\n", label, r.any_due, r.any_landed,
                        r.any_missed, r.any_blocked, r.any_crit, r.cone_due, r.cone_landed);
        };
        bites_line("straight", straight);
        bites_line("zig-zag", zig);
        bites_line("half speed", walk);
        CHECKF(straight.halts == 0 && zig.halts == 0 && walk.halts == 0, "the %s does not stop while it chases (%zu, %zu and %zu chasing ticks at a stand)", k.name, straight.halts, zig.halts, walk.halts);
        // a bite whose record measures nothing lands at its notify on the runner wherever he is (the hit roll may still miss it: a miss is a hit that reached him with the miss flag)
        CHECKF(straight.any_landed >= straight.any_due && zig.any_landed >= zig.any_due && walk.any_landed >= walk.any_due,
               "every bite of the %s's attacks that measures nothing reaches the runner at its notify, at 720 and at 360 cm/s (%zu of %zu, %zu of %zu and %zu of %zu)", k.name, straight.any_landed, straight.any_due,
               zig.any_landed, zig.any_due, walk.any_landed, walk.any_due);
        CHECKF(k.name[0] != 'W' || (straight.attacks >= 1 && straight.any_due >= 1 && zig.any_due >= 1),
               "the Wolf's attacks at the runner have bites that fall due (%zu bites in the straight chase, %zu in the zig-zag)", straight.any_due, zig.any_due);
        const double step = k.name[0] == 'W' ? 720.0 / 15.0 : 360.0 / 15.0;
        CHECKF(straight.worst_turn <= step + 1.0 && zig.worst_turn <= step + 1.0, "its facing turns at its rotation rate and never jumps (%.1f and %.1f deg in a tick, %.1f allowed)", straight.worst_turn, zig.worst_turn, step + 1.0);
        if (straight.run_speed > 720.0) {   // faster than the player: it catches up and attacks (the Bear, at 800 against 720, gains 80 cm a second and stands for each swing: it catches up once, then the player is away again)
            const size_t wanted = straight.run_speed > 1000.0 ? 2 : 1;
            CHECKF(straight.attacks >= wanted && zig.attacks >= wanted && straight.first_attack_at >= 0 && straight.first_attack_at < 4.0,
                   "the %s (faster than the player) catches up within 4 s and attacks (%zu attacks in the straight chase, the first at %.1f s; %zu in the zig-zag)", k.name, straight.attacks, straight.first_attack_at, zig.attacks);
        }
    }
}

void test_swing_faces_its_target(Fixture& f) {
    std::printf("a Bear whose player runs round it at 720 cm/s turns to him at its rate through every swing: the facing a client is sent is the server's, and the lunge goes towards him\n");
    constexpr double kPi = 3.14159265358979323846;
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string bear_path = creature_path(f, "/Beast/Bear/Bear.Default__Bear_C");
    const auto bp = f.motion.find(bear_path);
    CHECK(!bear_path.empty() && bp && tables.abilities_of(bear_path), "the Blueprint table has the Bear and the creature tables list its abilities");
    if (bear_path.empty() || !bp || !tables.abilities_of(bear_path)) return;
    const double dt = 1.0 / 15.0, radius = 450.0, angular = 720.0 / radius;   // the player runs round the Bear's start on a circle of 4.5 m
    double angle = kPi / 2.0;
    const LungeSteer steer = [&](double, const LungeSample*, double& x, double& y, const world::WorldActor&) {
        angle += angular * dt;
        x = radius * std::cos(angle);
        y = radius * std::sin(angle);
        return true;
    };
    std::vector<LungeSample> samples;
    std::vector<combat::IncomingHit> got;
    CHECK(run_lunge_fight(f, tables, bear_path, 40.0, 0, radius, steer, samples, &got, false, -1, nullptr, 40000.0), "a fight against a player who runs round the Bear runs");
    // the facing never jumps
    double worst_turn = 0;
    for (size_t i = 1; i < samples.size(); ++i) worst_turn = std::max(worst_turn, std::fabs(wrap180(samples[i].yaw - samples[i - 1].yaw)));
    // every swing: the facing a client holds when it begins is the server's, and the lunge goes towards the player
    size_t swings = 0, sent_off = 0, lunges = 0, wrong_way = 0;
    double worst_sent = 0, worst_lunge = 0;
    for (size_t i = 0; i < samples.size(); ++i) {
        if (!samples[i].cast_began) continue;
        ++swings;
        if (samples[i].has_mv_yaw) {
            const double d8 = std::fabs(wrap180(samples[i].mv_yaw - samples[i].yaw));
            const double d16 = samples[i].has_rrm_yaw ? std::fabs(wrap180(samples[i].rrm_yaw - samples[i].yaw)) : 0.0;
            worst_sent = std::max(worst_sent, std::max(d8, d16));
            if (d8 > 0.75 * 360.0 / 256.0 || d16 > 0.05) ++sent_off;
        }
    }
    for (const LungeRun& r : lunge_runs(samples)) {
        const LungeSample& from = samples[r.first];
        const LungeSample& to = samples[r.last + 1];
        const double travelled = std::hypot(to.wx - from.wx, to.wy - from.wy);
        if (travelled < 60.0) continue;   // (held at the player: it did not go anywhere)
        const combat::MontageData* md = montage_of_cast(f, tables, from.ability, from.montage);
        if (!md || md->hits.empty()) continue;
        const size_t at_hit = std::min(samples.size() - 1, r.first + static_cast<size_t>(md->hits.front().time / md->rate_scale / dt + 0.5));
        const double bearing = std::atan2(samples[at_hit].py - from.wy, samples[at_hit].px - from.wx) * 180.0 / kPi;
        const double lunge = std::atan2(to.wy - from.wy, to.wx - from.wx) * 180.0 / kPi;
        const double off = std::fabs(wrap180(lunge - bearing));
        ++lunges;
        worst_lunge = std::max(worst_lunge, off);
        if (off > 45.0) ++wrong_way;
    }
    std::printf("  %zu swings in 40 s, %zu hits on the runner: the facing a client holds at their start is the server's within %.2f deg (%zu off); %zu lunges of 60 cm or more, the worst %.0f deg off the way to the player when the "
                "hit lands (%zu over 45); its facing turned %.1f deg at the most in a tick (its rate allows %.1f)\n", swings, got.size(), worst_sent, sent_off, lunges, worst_lunge, wrong_way, worst_turn, bp->rotation_rate_yaw / 15.0);
    CHECKF(swings >= 8, "the Bear swings again and again at a player who runs round it (%zu swings)", swings);
    CHECKF(sent_off == 0, "the facing a client is sent when a swing begins is the server's (%zu of %zu swings differ)", sent_off, swings);
    CHECKF(lunges >= 3 && wrong_way == 0, "every lunge goes towards the player, within 45 deg of the way to him at the hit (%zu of %zu do not, the worst %.0f deg)", wrong_way, lunges, worst_lunge);
    CHECKF(worst_turn <= bp->rotation_rate_yaw / 15.0 + 1.0, "its facing turns at its rotation rate and never jumps (%.1f deg in a tick, %.1f allowed)", worst_turn, bp->rotation_rate_yaw / 15.0 + 1.0);
}

void test_turn_to_a_player_behind(Fixture& f) {
    std::printf("a Wolf with a player right behind it turns to him at its rotation rate, one step at a time (never a jump), and its bite is begun within the tree's angle and goes on turning\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string wolf_path = creature_path(f, "/Beast/Canine/Wolf.Default__Wolf_C");
    const auto bp = f.motion.find(wolf_path);
    CHECK(!wolf_path.empty() && bp && tables.abilities_of(wolf_path), "the Blueprint table has the Wolf and the creature tables list its abilities");
    if (wolf_path.empty() || !bp || !tables.abilities_of(wolf_path)) return;
    std::vector<LungeSample> samples;
    CHECK(run_lunge_fight(f, tables, wolf_path, 6.0, -300, 0, nullptr, samples), "a fight against a player right behind the Wolf runs");
    const double step = bp->rotation_rate_yaw / 15.0;
    double worst_turn = 0;
    size_t turned = 0, first_cast = samples.size();
    for (size_t i = 0; i < samples.size(); ++i) {
        const double d = std::fabs(wrap180(samples[i].yaw - (i == 0 ? 0.0 : samples[i - 1].yaw)));   // (it was made facing 0 deg)
        worst_turn = std::max(worst_turn, d);
        if (d > 0.5 && first_cast == samples.size()) ++turned;
        if (samples[i].cast_began && first_cast == samples.size()) first_cast = i;
    }
    CHECK(first_cast < samples.size(), "the Wolf bites the player who was behind it");
    if (first_cast >= samples.size()) return;
    const LungeSample& at = samples[first_cast];
    const double error_at_start = std::fabs(wrap180(std::atan2(at.py - at.wy, at.px - at.wx) * 180.0 / 3.14159265358979323846 - at.yaw));
    const size_t later = std::min(samples.size() - 1, first_cast + 8);   // half a second on
    const double error_later = std::fabs(wrap180(std::atan2(samples[later].py - samples[later].wy, samples[later].px - samples[later].wx) * 180.0 / 3.14159265358979323846 - samples[later].yaw));
    std::printf("  its facing turned %.1f deg at the most in a tick (its rate allows %.1f), over %zu ticks before the bite began; it began %.1f deg off the player and was %.1f deg off half a second on\n", worst_turn, step,
                turned, error_at_start, error_later);
    CHECKF(worst_turn <= step + 1.0, "its facing turns at its rotation rate and never jumps (%.1f deg in a tick, %.1f allowed)", worst_turn, step + 1.0);
    CHECKF(turned >= 3, "it turns step by step to the player behind it (%zu ticks of turning before the bite)", turned);
    CHECKF(error_at_start <= 46.0 && error_later <= 6.0, "the bite begins within the tree's 45 deg and goes on turning to the player (%.1f deg off at its start, %.1f half a second on)", error_at_start, error_later);
}

// ---- fix batch 3: where a hit lands, by its targeting record -----------------------------------------------------------------------------------------------------------------------

void test_hit_area_rules() {
    std::printf("where a hit lands, by its targeting record: a record that measures nothing hits its target wherever he stands, one that measures holds its cone, volumes hit everyone they overlap\n");
    using combat::BiteArea;
    using combat::HitSubject;
    using combat::HitVolume;
    constexpr double kPi = 3.14159265358979323846;
    // a body `dist` cm from the creature's centre, `bearing` degrees off the way it faces (it faces `yaw`), `radius` wide
    const auto body = [&](double dist, double bearing, double yaw = 0.0, double radius = 34.0) {
        HitSubject s;
        s.cx = 1000.0;
        s.cy = -500.0;
        s.yaw = yaw;
        const double a = (yaw + bearing) * kPi / 180.0;
        s.tx = s.cx + dist * std::cos(a);
        s.ty = s.cy + dist * std::sin(a);
        s.tradius = radius;
        return s;
    };
    const std::function<bool()> clear = [] { return true; };
    const std::function<bool()> blocked = [] { return false; };
    int asked = 0;
    const std::function<bool()> counted = [&asked] {
        ++asked;
        return true;
    };

    // validity off: the Snap's record states 500 cm and 120 degrees, and neither is tested
    BiteArea any;
    any.discovery = BiteArea::Discovery::PrefersActive;
    any.distance = 500;
    any.angle = 120;
    any.cone_check = true;
    any.los_check = true;
    CHECK(any.gate() == BiteArea::Gate::Any && any.usable(), "a record that finds a target and measures nothing is an Any record");
    CHECK(combat::hit_lands(any, body(30, 0), true, clear) && combat::hit_lands(any, body(5000, 180), true, clear) && combat::hit_lands(any, body(900, 90), true, blocked),
          "an Any record hits its target at 30 cm, 50 m behind it and 9 m to its side with no line of sight");
    CHECK(!combat::hit_lands(any, body(30, 0), false, clear), "... and nobody else (it has no volume)");
    asked = 0;
    combat::hit_lands(any, body(300, 20), true, counted);
    CHECK(asked == 0, "... without asking for a line of sight");

    // validity on: the Fury's record (300 cm, 60 degrees, cone and line of sight)
    BiteArea fury = any;
    fury.discovery = BiteArea::Discovery::OnlyLocked;
    fury.check_validity = true;
    fury.distance = 300;
    fury.angle = 60;
    CHECK(fury.gate() == BiteArea::Gate::Validity, "a record whose validity is on is a Validity record");
    CHECK(combat::hit_lands(fury, body(333, 0), true, clear) && !combat::hit_lands(fury, body(335, 0), true, clear), "its reach is 300 cm to the body's surface: a body 34 cm wide lands at 333 cm and not at 335");
    CHECK(combat::hit_lands(fury, body(200, 39), true, clear) && !combat::hit_lands(fury, body(200, 41), true, clear),
          "its cone is 30 degrees either side (half the 60), plus the body's own width: lands 39 degrees off at 200 cm, not 41");
    CHECK(combat::hit_lands(fury, body(200, -39), true, clear) && !combat::hit_lands(fury, body(200, -41), true, clear) && !combat::hit_lands(fury, body(200, 180), true, clear), "... to either side, and not behind");
    CHECK(combat::hit_lands(fury, body(200, 0, 90), true, clear) && !combat::hit_lands(fury, body(200, 90, 90), true, clear), "the cone goes the way the creature faces (facing 90 degrees: ahead lands, 90 degrees off does not)");
    CHECK(!combat::hit_lands(fury, body(200, 0), true, blocked) && combat::hit_lands(fury, body(200, 0), true, clear), "it needs a line of sight");
    asked = 0;
    combat::hit_lands(fury, body(400, 0), true, counted);
    combat::hit_lands(fury, body(200, 80), true, counted);
    CHECK(asked == 0, "... which it asks for only once the body is inside the cone");
    CHECK(!combat::hit_lands(fury, body(200, 0), false, clear), "it hits its target and nobody else");
    BiteArea round = fury;
    round.angle = 360;
    BiteArea none = fury;
    none.angle = 0;
    CHECK(combat::hit_lands(round, body(200, 180), true, clear) && combat::hit_lands(none, body(200, 180), true, clear) && !combat::hit_lands(round, body(340, 180), true, clear),
          "an angle of 360 or 0 is all round, still inside the reach");
    BiteArea shifted = fury;
    shifted.angle_center = 90;
    BiteArea ahead = fury;
    ahead.distance_center = 100;
    CHECK(combat::hit_lands(shifted, body(200, 90), true, clear) && !combat::hit_lands(shifted, body(200, 0), true, clear), "the angle centre turns the cone (90: it points to the creature's right)");
    CHECK(combat::hit_lands(ahead, body(433, 0), true, clear) && !combat::hit_lands(ahead, body(436, 0), true, clear), "the distance centre moves the start of the cone ahead of the creature (100 cm: the reach ends 100 cm farther)");
    BiteArea near = fury;
    near.min_distance = 100;
    CHECK(!combat::hit_lands(near, body(120, 0), true, clear) && combat::hit_lands(near, body(180, 0), true, clear), "a body nearer than the minimum distance to its surface (100 cm) is not hit");
    BiteArea no_reach = fury;
    no_reach.distance = 0;
    CHECK(!combat::hit_lands(no_reach, body(0, 0), true, clear), "a cone with no reach holds nothing");
    BiteArea sight_only = fury;
    sight_only.cone_check = false;
    CHECK(combat::hit_lands(sight_only, body(4000, 180), true, clear) && !combat::hit_lands(sight_only, body(100, 0), true, blocked), "validity on with the cone switch off asks only for the line of sight");

    // volumes: the Grem's pursuit (a sphere 64 cm wide, 75 ahead) and the Ram's headbutt (a box 100 x 80 half-sizes, 130 ahead)
    BiteArea sphere;
    HitVolume ball;
    ball.shape = HitVolume::Shape::Sphere;
    ball.ox = 75;
    ball.radius = 64;
    sphere.volumes.push_back(ball);
    sphere.volume_count = 1;
    CHECK(sphere.gate() == BiteArea::Gate::Volumes && sphere.usable() && !sphere.has_target_part(), "a record with a volume and no target part is a Volumes record");
    CHECK(combat::hit_lands(sphere, body(173, 0), false, clear) && !combat::hit_lands(sphere, body(175, 0), false, clear) && !combat::hit_lands(sphere, body(300, 180), false, clear),
          "a sphere 75 cm ahead with a radius of 64 hits a body 34 cm wide out to 173 cm ahead, and nothing behind it, whoever the body is");
    CHECK(combat::hit_lands(sphere, body(100, 0, 90), false, clear) && !combat::hit_lands(sphere, body(100, 90, 90), false, clear), "the sphere goes the way the creature faces (facing 90 degrees: ahead lands, 90 degrees off does not)");
    CHECK(!combat::hit_lands(sphere, body(100, 0), false, blocked), "a volume asks for a line of sight too (its switch is on)");
    BiteArea crate;
    HitVolume box;
    box.shape = HitVolume::Shape::Box;
    box.ox = 130;
    box.hx = 100;
    box.hy = 80;
    box.los = false;
    crate.volumes.push_back(box);
    crate.volume_count = 1;
    CHECK(combat::hit_lands(crate, body(264, 0), false, clear) && !combat::hit_lands(crate, body(266, 0), false, clear) && combat::hit_lands(crate, body(60, 0), false, clear) &&
              combat::hit_lands(crate, body(-3, 0), false, clear) && !combat::hit_lands(crate, body(-5, 0), false, clear),
          "a box from 30 to 230 cm ahead hits a body 34 cm wide from 3 cm behind the creature's centre to 264 cm ahead of it");
    CHECK(combat::hit_lands(crate, body(150, 35), false, clear) && !combat::hit_lands(crate, body(200, 90), false, clear), "... and 80 cm to each side of its centre line, plus the body's width");
    CHECK(combat::hit_lands(crate, body(150, 0), false, blocked), "a volume whose line-of-sight switch is off hits through a wall");
    BiteArea both = fury;
    both.volumes.push_back(ball);
    both.volume_count = 1;
    CHECK(both.gate() == BiteArea::Gate::Volumes, "a record with a target part and a volume counts as a Volumes record");
    CHECK(combat::hit_lands(both, body(400, 180), true, clear) == false && combat::hit_lands(both, body(100, 0), false, clear) && combat::hit_lands(both, body(200, 0), true, clear),
          "... and hits what either part reaches: its target inside the cone, and anybody in the volume");
    BiteArea odd = sphere;
    odd.readable = false;
    CHECK(!odd.usable(), "a record with a volume the world cannot place is not usable");
}

void test_fury_lands_only_inside_its_cone(Fixture& f) {
    std::printf("a Grem's Fury (validity on: 300 cm, 60 degrees, line of sight) lands each of its four bites at its notify only while the player is inside its cone, and its Snap (validity off) wherever he stands\n");
    constexpr double kPi = 3.14159265358979323846;
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string rabid_path = creature_path(f, "/Beast/Rabid/Rabid.Default__Rabid_C");
    const abilities::AbilityDef* fury = f.catalog->ability_named("Rabid_Fury");
    const abilities::AbilityDef* snap = f.catalog->ability_named("Rabid_Snap");
    CHECK(!rabid_path.empty() && f.motion.find(rabid_path) && tables.abilities_of(rabid_path) && fury && snap, "the Blueprint table has the Rabid, the creature tables list its abilities and the ability data has the Fury and the Snap");
    if (rabid_path.empty() || !f.motion.find(rabid_path) || !tables.abilities_of(rabid_path) || !fury || !snap) return;
    const combat::BiteArea* fury_area = tables.area_of(fury->row.guid, 1);   // (the Fury's hits are keys 1 and 2; its one record is key 0)
    const combat::BiteArea* snap_area = tables.area_of(snap->row.guid, 0);
    CHECK(fury_area && fury_area->gate() == combat::BiteArea::Gate::Validity && fury_area->discovery == combat::BiteArea::Discovery::OnlyLocked && fury_area->cone_check && fury_area->los_check &&
              fury_area->distance == 300 && fury_area->angle == 60 && fury_area->volume_count == 0,
          "the Fury's record finds the locked target and measures: 300 cm, 60 degrees, the cone and the line of sight");
    CHECK(snap_area && snap_area->gate() == combat::BiteArea::Gate::Any && snap_area->distance == 500 && snap_area->angle == 120 && !snap_area->check_validity,
          "the Snap's record states 500 cm and 120 degrees and has its validity off (an Any record)");
    if (!fury_area || !snap_area) return;

    struct Place {
        const char* name;
        double      distance, bearing;
        bool        fury_lands;
    };
    const Place places[] = {{"220 cm ahead", 220, 0, true}, {"220 cm, 15 deg off", 220, 15, true}, {"220 cm, 50 deg off", 220, 50, false}, {"220 cm behind", 220, 180, false}, {"520 cm ahead", 520, 0, false}};
    for (const Place& place : places) {
        // as each Fury or Snap begins, the player is put there relative to the Grem, and kept there while its montage plays
        std::vector<LungeSample> samples;
        std::vector<combat::IncomingHit> got;
        const LungeSteer steer = [&](double, const LungeSample* prev, double& px, double& py, const world::WorldActor& grem) {
            if (!prev || prev->montage == 0 || (prev->ability != fury->row.guid && prev->ability != snap->row.guid)) return false;
            const double a = (grem.yaw_degrees + place.bearing) * kPi / 180.0;
            px = grem.x + place.distance * std::cos(a);
            py = grem.y + place.distance * std::sin(a);
            return true;
        };
        CHECKF(run_lunge_fight(f, tables, rabid_path, 40.0, 0, 800, steer, samples, &got, false, -1, nullptr, 40000.0), "the fight against a player put %s runs", place.name);   // (a fight that drifts far must not be given up)
        size_t fury_casts = 0, fury_due = 0, fury_landed = 0, snap_casts = 0, snap_due = 0, snap_landed = 0;
        for (const LungeSample& s : samples) {
            if (!s.cast_began || (s.ability != fury->row.guid && s.ability != snap->row.guid)) continue;
            const bool is_fury = s.ability == fury->row.guid;
            (is_fury ? fury_casts : snap_casts) += 1;
            const combat::MontageData* md = montage_of_cast(f, tables, s.ability, s.montage);
            for (const double at : cast_bites(f, tables, s.ability, s.montage)) {
                if (s.t + at <= 40.0 - 0.2) (is_fury ? fury_due : snap_due) += 1;
            }
            size_t in_window = 0;
            for (const combat::IncomingHit& h : got) {
                if (h.source_guid == s.ability && h.at >= s.t - 0.05 && h.at <= s.t + (md ? md->seconds(1.0) : 1.0) + 0.25) {
                    (is_fury ? fury_landed : snap_landed) += 1;
                    ++in_window;
                }
            }
            if (in_window == 0 && (!is_fury || place.fury_lands) && !cast_bites(f, tables, s.ability, s.montage).empty() && s.t + cast_bites(f, tables, s.ability, s.montage).front() <= 40.0 - 0.2) {
                std::printf("    (%s at %.2f s: %zu bites due and none landed; hits of that ability near it:", is_fury ? "Fury" : "Snap", s.t, cast_bites(f, tables, s.ability, s.montage).size());
                for (const combat::IncomingHit& h : got) {
                    if (h.source_guid == s.ability && std::fabs(h.at - s.t) < 2.0) std::printf(" %.2f", h.at);
                }
                std::printf(")\n");
            }
        }
        std::printf("  the player put %-18s: Fury %zu casts, %zu bites due, %zu landed;  Snap %zu casts, %zu bites due, %zu landed\n", place.name, fury_casts, fury_due, fury_landed, snap_casts, snap_due, snap_landed);
        CHECKF(fury_casts >= 2 && fury_due >= 6, "the Grem casts the Fury at a player put %s (%zu casts, %zu bites due)", place.name, fury_casts, fury_due);
        if (place.fury_lands) {
            CHECKF(fury_landed >= fury_due, "every bite of its Fury lands on a player put %s, inside the cone (%zu of %zu)", place.name, fury_landed, fury_due);
        } else {
            CHECKF(fury_landed == 0, "no bite of its Fury lands on a player put %s, outside the cone (%zu of %zu landed)", place.name, fury_landed, fury_due);
        }
        // the Snap's record states 500 cm and 120 degrees but does not measure: it lands wherever he is put
        CHECKF(snap_casts == 0 || snap_landed >= snap_due, "every bite of its Snap lands on a player put %s (%zu of %zu)", place.name, snap_landed, snap_due);
    }
}
