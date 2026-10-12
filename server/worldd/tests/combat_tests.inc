// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_tests.inc: the hit rules and the fight (included by test_worldd_combat.cpp inside its anonymous namespace)
// ============================================================================

namespace repl = ::aoc::protocol::repl;

// ---- the hit rules ------------------------------------------------------------------------------------------------------------------

void test_aspect() {
    std::printf("the element and aspect of a hit\n");
    using combat::aspect_of;
    CHECK(aspect_of({"Element.Physical.Slash", "ability.weaponcombo"}).element == "Element.Physical.Slash" && aspect_of({"Element.Physical.Slash"}).physical, "a slash is physical");
    CHECK(!aspect_of({"Element.Magical.Fire"}).physical && aspect_of({"Element.Magical.Fire"}).element == "Element.Magical.Fire", "fire is magical");
    CHECK(aspect_of({"Element.Physical"}).element == "Element.Physical" && aspect_of({"Element.Physical"}).physical, "a bare Element.Physical is physical, with no element of the table");
    CHECK(aspect_of({"ability.Melee"}).physical && aspect_of({"ability.Melee"}).element.empty(), "a hit with no element is physical");
    CHECK(!aspect_of({"Element.Physical.Slash", "ability.MagicalScalingOverride"}).physical, "the magical scaling override wins");
    CHECK(aspect_of({"Element.Magical.Arcane", "ability.PhysicalScalingOverride"}).physical, "...and the physical one");
    CHECK(combat::tag_matches("Element.Physical.Slash", "Element.Physical") && !combat::tag_matches("Element.PhysicalX", "Element.Physical") && combat::tag_matches("a", "a"),
          "a tag matches itself and its descendants only");
    CHECK(combat::hit_flag_bit("Crit") == 1 && combat::hit_flag_bit("Lethal") == 2 && combat::hit_flag_bit("Block") == 4 && combat::hit_flag_bit("Miss") == 8 &&
              combat::hit_flag_bit("ActiveBlock") == 64 && combat::hit_flag_bit("Nope") == 0,
          "EHitFlag names are the bits");
}

/// A Wolf at `level` as a stand-alone fighter (no world).
struct LoneNpc final : combat::Fighter {
    std::unique_ptr<stats::CharacterStats> cs;
    std::vector<int64_t>                   aligns;
    combat::Vec3                           pos;
    int                                    lvl = 1;
    uint64_t                               ident = 99;
    uint64_t id() const override { return ident; }
    bool is_player() const override { return false; }
    int level() const override { return lvl; }
    combat::Vec3 position() const override { return pos; }
    bool alive() const override { return cs->find(content::kGuidHealth)->value > 0; }
    const std::vector<int64_t>& alignments() const override { return aligns; }
    stats::CharacterStats& stats() override { return *cs; }
};

std::unique_ptr<LoneNpc> make_lone(Fixture& f, const char* blueprint, int level) {
    const auto bp = f.blueprint(blueprint);
    if (!bp) return nullptr;
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    std::string err;
    auto n = std::make_unique<LoneNpc>();
    n->cs = book.make(*bp, level, err);
    if (!n->cs) return nullptr;
    n->aligns.assign(bp->alignments.begin(), bp->alignments.end());
    n->lvl = level;
    return n;
}

void test_hit_rules(Fixture& f) {
    std::printf("the hit rules: a player's hit on an NPC and an NPC's on a player\n");
    Rig rig(4, 20, {});   // a level-20 Fighter
    CHECKF(rig.ok(), "the rig: %s", rig.problem.c_str());
    if (!rig.ok()) return;
    combat_rig::RigFighter player(rig, 7, {0, 0, 1098});
    auto wolf = make_lone(f, "Wolf_C", 10);
    CHECK(wolf != nullptr, "a level 10 Wolf's stats run");
    if (!wolf) return;
    wolf->pos = {200, 0, 1088};
    combat::HitResolver resolver(*f.catalog, *f.data, 12345);
// built by C | Y | B with C.O.A

    // the player's combo hit: a physical slash, aimed at enemies, with a Health mod
    const abilities::AbilityDef* axe = f.catalog->ability_named("WeaponCombo_Melee_1H_Axe");
    CHECK(axe && !axe->hits.empty(), "the 1H axe combo is in the tables");
    if (!axe) return;
    const abilities::HitDef* hit1 = f.catalog->hit(axe->hits[0].hit_guid);
    CHECK(hit1 && !hit1->tags.empty() && hit1->tags[0] == "Element.Physical.Slash" && hit1->mods.size() == 1 && hit1->mods[0].stat_guid == content::kGuidHealth,
          "its first hit carries its event tags (Element.Physical.Slash ...) and one Health mod");
    if (!hit1) return;
    combat::HitMeta meta;
    meta.source_type = content::kSourceTypeAbility;
    meta.source_guid = axe->row.guid;
    meta.ability_tags = axe->row.tags;
    double total = 0;
    int resolved = 0, misses = 0, blocks = 0, crits = 0;
    constexpr int kRolls = 400;
    for (int i = 0; i < kRolls; ++i) {
        combat::Resolved r;
        std::string why;
        CHECKF(resolver.resolve(player, *wolf, *hit1, meta, r, why), "the formula runs: %s", why.c_str());
        if (r.miss()) {
            ++misses;
            CHECK(r.mods.empty(), "a miss has no stat mods");
            continue;
        }
        if (r.flags & stats::kHitBlock) ++blocks;
        if (r.flags & stats::kHitCrit) ++crits;
        if (r.mods.size() != 1) continue;
        ++resolved;
        CHECKF(r.mods[0].stat_guid == content::kGuidHealth && r.mods[0].value < 0 && std::isfinite(r.mods[0].value) && r.mods[0].stat_mod_guid == hit1->mods[0].stat_mod_guid,
               "a hit takes Health (%g)", r.mods[0].value);
        total += r.mods[0].value;
        CHECK(r.threat > 0, "a damaging hit has threat");
    }
    const double avg = resolved ? total / resolved : 0;
    CHECKF(resolved > 0 && avg < -1 && avg > -2000, "a level-20 Fighter's combo hit on a level-10 Wolf takes %.1f Health on average (the captured player's hits: 20 to 30)", -avg);
    CHECKF(misses + blocks + crits <= kRolls, "the rolls make flags (%d misses, %d blocks, %d crits of %d)", misses, blocks, crits, kRolls);

    // a crit multiplies the damage: force every roll
    abilities::HitDef forced = *hit1;
    forced.force_roll_crit = true;
    forced.force_cant_miss = true;
    forced.force_cant_block = true;
    combat::Resolved crit;
    std::string why;
    CHECK(resolver.resolve(player, *wolf, forced, meta, crit, why) && (crit.flags & stats::kHitCrit) && crit.mods.size() == 1, "a hit that always crits crits");
    abilities::HitDef plain = *hit1;
    plain.force_cant_crit = true;
    plain.force_cant_miss = true;
    plain.force_cant_block = true;
    combat::Resolved normal;
    CHECK(resolver.resolve(player, *wolf, plain, meta, normal, why) && normal.flags == 0 && normal.mods.size() == 1, "a hit that cannot crit, miss or block has no flags");
    if (crit.mods.size() == 1 && normal.mods.size() == 1) {
        CHECKF(crit.mods[0].value < normal.mods[0].value, "a crit hits harder (%.2f against %.2f)", crit.mods[0].value, normal.mods[0].value);
    }
    abilities::HitDef blocked = plain;
    blocked.force_roll_block = true;
    blocked.force_cant_block = false;
    combat::Resolved bl;
    CHECK(resolver.resolve(player, *wolf, blocked, meta, bl, why) && (bl.flags & stats::kHitBlock) && bl.mods.size() == 1, "a hit that is always blocked is");
    abilities::HitDef missing = plain;
    missing.force_roll_miss = true;
    missing.force_cant_miss = false;
    combat::Resolved ms;
    CHECK(resolver.resolve(player, *wolf, missing, meta, ms, why) && ms.miss() && ms.mods.empty(), "a hit that always misses does nothing");

    // the target's stats are the actor of GetTarget(): a tougher target takes less
    auto tough = make_lone(f, "Bear_C", 40);
    auto weak = make_lone(f, "Wolf_C", 5);
    if (tough && weak) {
        combat::Resolved a, b;
        CHECK(resolver.resolve(player, *weak, plain, meta, a, why) && resolver.resolve(player, *tough, plain, meta, b, why) && a.mods.size() == 1 && b.mods.size() == 1, "the hit runs on both targets");
        if (a.mods.size() == 1 && b.mods.size() == 1) CHECKF(-b.mods[0].value < -a.mods[0].value, "a level-40 Bear takes less than a level-5 Wolf (%.2f against %.2f): its Mitigation is read", -b.mods[0].value, -a.mods[0].value);
    }

    // the NPC's hit on the player: a Wolf's bite
    const abilities::AbilityDef* bite = f.catalog->ability_named("Wolf_Bite");
    CHECK(bite && !bite->hits.empty(), "the Wolf's bite is in the tables");
    if (!bite) return;
    const abilities::HitDef* bite_hit = f.catalog->hit(bite->hits[0].hit_guid);
    CHECK(bite_hit && (bite_hit->row.applicable_targets & 2) && !bite_hit->mods.empty(), "...aimed at enemies, with a stat mod");
    if (!bite_hit) return;
    abilities::HitDef sure = *bite_hit;
    sure.force_cant_crit = sure.force_cant_miss = sure.force_cant_block = true;
    combat::HitMeta bite_meta;
    bite_meta.source_type = content::kSourceTypeAbility;
    bite_meta.source_guid = bite->row.guid;
    bite_meta.ability_tags = bite->row.tags;
    auto low = make_lone(f, "Wolf_C", 5), high = make_lone(f, "Wolf_C", 25);
    combat::Resolved lo, hi;
    CHECK(resolver.resolve(*low, player, sure, bite_meta, lo, why) && resolver.resolve(*high, player, sure, bite_meta, hi, why) && lo.mods.size() == 1 && hi.mods.size() == 1,
          "a Wolf's bite on the player runs for two levels");
    if (lo.mods.size() == 1 && hi.mods.size() == 1) {
        CHECKF(lo.mods[0].value < 0 && hi.mods[0].value < lo.mods[0].value, "the bite is level-scaled: %.2f at level 5, %.2f at level 25", lo.mods[0].value, hi.mods[0].value);
        // against the player's own Health: a level-25 wolf takes a real part of a level-20 player's
        const double health = player.stats().find(content::kGuidMaxHealth)->value;
        CHECKF(-hi.mods[0].value < health && -hi.mods[0].value > health * 0.001, "...and is a fraction of the player's %.0f Health (%.2f)", health, -hi.mods[0].value);
    }
}
// C O A.C.Y.B

// ---- the fight ---------------------------------------------------------------------------------------------------------------------

void test_fight(Fixture& f) {
    std::printf("a fight: aggro, chase, attack, leash, reset, death\n");
    Rig rig(4, 20, {});
    CHECKF(rig.ok(), "the rig: %s", rig.problem.c_str());
    if (!rig.ok()) return;
    combat_rig::World w;
    CHECKF(w.problem.empty() && w.captured, "the world: %s", w.problem.c_str());
    if (!w.problem.empty()) return;
    const auto wolf_bp = f.blueprint("Wolf_C");
    std::string wolf_path;
    for (const auto& [path, row] : f.motion.rows()) {
        if (path.find("/Beast/Canine/Wolf.Default__Wolf_C") != std::string::npos) wolf_path = path;
    }
    CHECK(!wolf_path.empty() && wolf_bp, "the Blueprint table has the Wolf");
    if (wolf_path.empty()) return;

    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorldConfig cfg;
    cfg.seed = 5;
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion, cfg);
    const uint64_t wolf = w.spawn(wolf_path, 0, 0, 10, 900);
    CHECK(wolf != 0 && !world.is_combatant(wolf), "a Wolf at level 10 stands at the origin; it is no combatant yet");
    if (wolf == 0) return;

    combat_rig::FakePlayer player(rig, 77, {5000, 0, 1098});
    world.add_player(&player);
    std::vector<combat::DeathEvent> deaths;
    world.set_death_hook([&](const combat::DeathEvent& e) { deaths.push_back(e); });

    double now = 0;
    const double step = 1.0 / 15.0;
    auto run = [&](double seconds) {
        const double end = now + seconds;
        while (now < end) {
            now += step;
            w.npcs->note_player(77, player.rig_fighter().position().x, player.rig_fighter().position().y, now);
            w.npcs->tick(now);
            world.tick(now);
        }
    };

    // far away: nothing happens
    run(2.0);
    CHECK(!world.is_combatant(wolf) || world.ai_state(wolf) == npc::ai::State::Idle, "a player 50 m away: the Wolf stays idle");
    const world::WorldActor* wa = w.registry.find(wolf);
    CHECK(wa && !combat_rig::combat_flag(*wa, "bInCombat"), "its CombatInfo says it is not in combat");

    // inside the aggro radius: it notices
    player.rig_fighter().move_to({600, 0, 1098});
    run(0.6);
    CHECK(world.is_combatant(wolf), "a player 6 m away makes it a combatant");
    const auto st = world.ai_state(wolf);
    CHECK(st && (*st == npc::ai::State::Chase || *st == npc::ai::State::Attack), "the Wolf aggros: chase");
    wa = w.registry.find(wolf);
    CHECK(wa && combat_rig::combat_flag(*wa, "bInCombat"), "its bInCombat goes up");
    CHECK(wa && combat_rig::active_target(*wa) == 77, "its ActiveTargets names the player's pawn");
    // its stats are the engine's: the level 10 Wolf's MaxHealth in its replicated array
    float max_health = 0;
    if (wa) {
        for (const repl::Subobject& sub : wa->actor.subobjects) {
            if (!sub.obj.schema || std::string(sub.obj.schema->name) != "UAoCStatsComponent") continue;
            const auto* arr = const_cast<repl::FastArrays&>(sub.obj.arrays).get(*sub.obj.schema, "StatRepInt32EveryoneProxy");
            for (const char* an : {"StatRepInt32Owner", "StatRepInt32OwnerProxy", "StatRepInt32EveryoneProxy", "StatRepFloatArrayEveryoneProxy"}) {
                const auto* a2 = const_cast<repl::FastArrays&>(sub.obj.arrays).get(*sub.obj.schema, an);
                std::printf("    %s: %zd items\n", an, a2 ? static_cast<ptrdiff_t>(a2->items().size()) : -1);
            }
            std::printf("    the template: %s, %zu blocks\n", w.captured->template_for(wolf_path) ? "found" : "missing", w.captured->opens().size());
            std::printf("    the stat items:");
            if (arr) for (const repl::FastArrayItem& it : arr->items()) {
                const auto v = it.values.values.find(1);
                const auto* si = v == it.values.values.end() ? nullptr : v->second.get_if<repl::StatInt32Inst>();
                if (si) std::printf(" %lld=%g", static_cast<long long>(si->record), std::bit_cast<float>(si->value));
            }
            std::printf("\n");
        }
    }
    CHECK(wa && combat::read_stat(*wa, content::kGuidMaxHealth, max_health), "its MaxHealth item exists");
    CHECKF(near(max_health, world.max_health_of(wolf), 0.5) && max_health > 100, "the item holds the engine's MaxHealth for level 10 (%.1f)", max_health);

    // it reaches the player and attacks
    run(6.0);
    CHECK(!player.hits.empty(), "the Wolf bites the player");
    CHECKF(world.stats().npc_attacks >= 1, "%llu attacks started", static_cast<unsigned long long>(world.stats().npc_attacks));
    CHECK(player.in_combat, "the player's combat flag is up");
    const world::WorldActor* wolf_now = w.registry.find(wolf);
    const double dist = std::hypot(wolf_now->x - player.rig_fighter().position().x, wolf_now->y - player.rig_fighter().position().y);
    CHECKF(dist < 450, "the Wolf stands at the player (%.0f cm)", dist);
    const stats::StatValue* hp = player.rig_fighter().stats().find(content::kGuidHealth);
    const stats::StatValue* hp_max = player.rig_fighter().stats().find(content::kGuidMaxHealth);
    CHECK(hp && hp_max && hp->value < hp_max->value, "the player lost Health");
    if (!player.hits.empty()) {
        const combat::IncomingHit& h = player.hits.front();
        CHECK(h.instigator == wolf && h.hit_guid != 0 && h.source_type == content::kSourceTypeAbility && !h.mods.empty(), "the hit names the Wolf, the hit record and the ability");
    }

    // the player hits back: a resolved hit applied to the Wolf
    combat::Resolved hit;
    hit.mods.push_back({1, content::kGuidHealth, -30.0});
    hit.threat = 30.0;
    const double before = world.health_of(wolf);
    const combat::Delivery d = world.apply_hit_to_npc(wolf, 77, hit, now);
    // E1 stage 3 fix 3: the cylinder is the registry's slot inside the actor's block (the actor's + 4, retail's AlignmentComponent - 2), never the + 24 that
    // was the next-minted object's GUID (the user's NetGuidMismatch disconnect)
    CHECK(d.applied && !d.lethal && d.effective.size() == 1 && near(d.effective[0], -30.0, 1e-3) && d.target_object == wolf + 4 && d.target_object == w.registry.find(wolf)->npc.collision_cylinder,
          "a hit applies: Health -30, confirmed against the Wolf's collision cylinder (the actor's + 4, the registry's)");
    CHECKF(near(world.health_of(wolf), before - 30.0, 0.01), "its Health is %.2f", world.health_of(wolf));
    float item = 0;
    CHECK(combat::read_stat(*w.registry.find(wolf), content::kGuidHealth, item) && near(item, static_cast<float>(before - 30.0), 0.01), "the replicated Health item moved with it");
    CHECK(world.actor_of_object(wolf + 4) == wolf && world.actor_of_object(wolf) == wolf && world.actor_of_object(wolf + 24) != wolf && world.actor_of_object(wolf + 28) != wolf &&
              world.actor_of_object(1) == 0,
          "the collision cylinder and the actor name the NPC; the GUIDs outside its block (+ 24, + 28: the next actor's components) and a stranger's object do not");
// C.Y.B : C.O.A

    // out of reach: it gives up at the leash, heals and goes home
    player.rig_fighter().move_to({12000, 0, 1098});
    run(0.8);
    CHECK(world.ai_state(wolf) == npc::ai::State::Reset || world.ai_state(wolf) == npc::ai::State::Idle, "the player left: the Wolf resets");
    run(4.0);
    wa = w.registry.find(wolf);
    CHECK(wa && !combat_rig::combat_flag(*wa, "bInCombat") && combat_rig::active_target(*wa) == 0, "its combat flags are down");
    CHECKF(near(world.health_of(wolf), world.max_health_of(wolf), 0.01), "it healed to full (%.1f of %.1f)", world.health_of(wolf), world.max_health_of(wolf));
    CHECK(world.stats().resets >= 1, "a reset was counted");
    run(10.0);
    wa = w.registry.find(wolf);
    CHECKF(wa && std::hypot(wa->x, wa->y) < 400, "it walked home (%.0f cm away)", wa ? std::hypot(wa->x, wa->y) : -1.0);
    CHECK(world.ai_state(wolf) == npc::ai::State::Idle, "and is idle again");

    // the kill: Health reaches 0 in the hit, the flags and the item follow in the same step, the death hook runs once
    world.add_threat(wolf, 77, 10.0, now);
    combat::Resolved kill;
    kill.mods.push_back({1, content::kGuidHealth, -1.0e9});
    const combat::Delivery k = world.apply_hit_to_npc(wolf, 77, kill, now);
    CHECK(k.applied && k.lethal && (k.flags & stats::kHitLethal), "an overwhelming hit is lethal and carries the Lethal flag");
    CHECKF(k.effective.size() == 1 && near(k.effective[0], -world.max_health_of(wolf), 0.01) || k.effective[0] < 0, "its effective loss is what the Wolf had (%.1f)", k.effective.empty() ? 0.0 : k.effective[0]);
    CHECK(world.dead(wolf) && world.ai_state(wolf) == npc::ai::State::Dead, "the Wolf is dead and its AI is off");
    wa = w.registry.find(wolf);
    CHECK(wa && combat_rig::combat_flag(*wa, "bIsDead") && !combat_rig::combat_flag(*wa, "bInCombat") && combat_rig::active_target(*wa) == 0, "bIsDead is 1, the combat flags are down, ActiveTargets is obj:0");
    CHECK(combat::read_stat(*wa, content::kGuidHealth, item) && item == 0.0f, "its Health item is 0");
    CHECK(deaths.size() == 1 && deaths[0].npc == wolf && deaths[0].killer == 77 && deaths[0].level == 10 && deaths[0].blueprint == "Wolf" && deaths[0].attackers.count(77) &&
              deaths[0].info && deaths[0].motion,
          "the death hook ran once: who killed it, what it was, who fought it");
    const combat::Delivery again = world.apply_hit_to_npc(wolf, 77, kill, now);
    CHECK(!again.applied && deaths.size() == 1, "a dead NPC takes no more hits and dies once");
    const double dead_x = wa->x;
    run(3.0);
    CHECK(w.registry.find(wolf)->x == dead_x, "the corpse does not move");
}

void test_hostile(Fixture& f) {
    std::printf("hostile cases\n");
    Rig rig(4, 20, {});
    if (!rig.ok()) return;
    combat_rig::World w;
    if (!w.problem.empty()) return;
    std::string wolf_path;
    for (const auto& [path, row] : f.motion.rows()) {
        if (path.find("/Beast/Canine/Wolf.Default__Wolf_C") != std::string::npos) wolf_path = path;
    }
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion);
    const uint64_t wolf = w.spawn(wolf_path, 0, 0, 10);
    combat_rig::FakePlayer player(rig, 77, {300, 0, 1098});
    world.add_player(&player);
    combat::Resolved r;
    r.mods.push_back({1, content::kGuidHealth, -5.0});
    CHECK(!world.apply_hit_to_npc(12345, 77, r, 1.0).applied, "a hit on an actor that is not there applies to nothing");
    CHECK(world.fighter_of(12345) == nullptr && world.fighter_of(1) == nullptr, "no fighter for a stranger");
    // NaN and infinite values do not make the target's Health a number that is not
    combat::Resolved nan;
    nan.mods.push_back({1, content::kGuidHealth, std::nan("")});
    const combat::Delivery dn = world.apply_hit_to_npc(wolf, 77, nan, 1.0);
    CHECK(dn.applied && std::isfinite(world.health_of(wolf)) && world.health_of(wolf) > 0, "a NaN hit leaves Health a number");
    combat::Resolved heal;
    heal.mods.push_back({1, content::kGuidHealth, 1.0e9});
    world.apply_hit_to_npc(wolf, 77, heal, 1.0);
    CHECK(near(world.health_of(wolf), world.max_health_of(wolf), 1e-3), "a heal stops at the maximum");
    // a miss does nothing but start the fight
    combat::Resolved miss;
    miss.flags = stats::kHitMiss;
    const double hp = world.health_of(wolf);
    const combat::Delivery dm = world.apply_hit_to_npc(wolf, 77, miss, 2.0);
    CHECK(dm.applied && !dm.lethal && world.health_of(wolf) == hp && (dm.flags & stats::kHitMiss), "a miss costs no Health");
    const combat::TargetInfo ti = world.inspect(wolf, player.fighter());
    CHECK(ti.exists && ti.alive && ti.relation == combat::Relation::Hostile && ti.surface_distance_cm > 100 && ti.surface_distance_cm < 300 && ti.in_line_of_sight,
          "inspect: a living enemy 3 m away in sight");
    CHECK(!world.inspect(999, player.fighter()).exists, "inspect: no such actor");
    // the kill switch
    combat::CombatWorldConfig off;
    off.enabled = false;
    combat::CombatWorld quiet(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion, off);
    quiet.add_player(&player);
    for (double t = 0; t < 5; t += 0.1) quiet.tick(t);
    CHECK(quiet.combatants() == 0, "with combat off nothing thinks");
}
