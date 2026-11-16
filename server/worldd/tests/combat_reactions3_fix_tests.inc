// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_reactions3_fix_tests.inc: reactions part 2, the Thorns and Epic fixes
//
//    Thorns          the Ranger's Thorns adds its child Bleeding roll to the attacker even when the damage formula has no program, it answers a Goblin's
//                    real hit on the world's own path, a miss or a full shield raises no reaction at all, two creatures at once each get their own, and
//                    taking Thorns off stops it
//    Epic            the Resonance the data spends for a finisher's Epic is spent once for every ally the AoE reaches: alone at 1, 2 and 3 stacks,
//                    three allies and two stacks, an ally behind the ground, one with four crowd-control effects, and one who already holds Pep
//  (registered with AOC_REGISTER_TEST: test_worldd_combat.cpp runs it through aoc_tests::run; it is included from combat_registered.inc)
// ============================================================================

namespace reactions2_fix {

using reactions2::confirms_of;
using reactions2::Duo;
using reactions2::finish_axe;
using reactions2::grant;
using reactions2::Melodies;
using reactions2::near;
using reactions2::stacks_of;
using reactions2::wake;

/// The Thorns records a test reads: the effect the Ranger's ability puts on its holder, the reaction's damage hit, its Bleeding child and the effect.
struct Thorns {
    const abilities::EffectDef* effect = nullptr;
    const abilities::HitDef* damage = nullptr;
    const abilities::HitDef* bleed = nullptr;
    const abilities::EffectDef* bleeding = nullptr;
    void look_up(Fixture& f) {
        const abilities::Catalog& c = *f.catalog;
        effect = c.effect_named("Ranger_Thorns");
        damage = c.hit_named("Ranger_Thorns_Damage");
        bleed = c.hit_named("Ranger_Thorns_BleedChance");
        bleeding = c.effect_named("Status_Bleeding");
    }
    bool ok() const { return effect && damage && bleed && bleeding; }
};

/// Whether the Thorns damage formula has a program in this content.db (the game's data compiles it once the reader models the division of two whole numbers).
bool thorns_damage_compiles(Fixture& f, const Thorns& t) {
    if (!t.damage || t.damage->mods.empty() || !f.catalog) return false;
    const content::ExprRow* row = f.catalog->expr(t.damage->mods.front().value_expr);
    return row && row->program >= 0;
}

/// What a fight's log says the Goblin lost, by cause: the log's lines "player 77 hit <the Goblin> ...: Health A -> B" are the Thorns' damage hits (the character is the
/// only player in the world), the lines "takes X from Status_Bleeding_Tick ...: Health A -> B" the Bleeding's ticks.
struct GoblinLosses {
    double reflected = 0, bleeding = 0;
    size_t reflections = 0, ticks = 0;
};

GoblinLosses goblin_losses(const std::string& log) {
    GoblinLosses out;
    size_t from = 0;
    while (from < log.size()) {
        size_t end = log.find('\n', from);
        if (end == std::string::npos) end = log.size();
        const std::string line = log.substr(from, end - from);
        from = end + 1;
        const size_t at = line.find(": Health ");
        double a = 0, b = 0;
        if (at == std::string::npos || std::sscanf(line.c_str() + at + 9, "%lf -> %lf", &a, &b) != 2) continue;
        if (line.find("player 77 hit Goblin3_Gravehide") != std::string::npos) {
            out.reflected += a - b;
            if (a > b) ++out.reflections;
        } else if (line.find("from Status_Bleeding_Tick") != std::string::npos && line.find("Goblin3_Gravehide") != std::string::npos) {
            out.bleeding += a - b;
            ++out.ticks;
        }
    }
    return out;
}

/// A hit of the Goblin's swing on the character: the recorded hit and the Health it took.
combat::IncomingHit goblin_hit(uint64_t goblin, double health) {
    combat::IncomingHit in;
    in.instigator = goblin;
    in.hit_guid = 109024903452282ll;   // Goblin_Swing_Hit
    in.source_type = content::kSourceTypeAbility;
    in.source_guid = 0;
    in.at = 1.0;
    in.mods.push_back({1, content::kGuidHealth, health});
    return in;
}

// The Thorns case on the world's path: a Goblin of the Riverlands swings at the Ranger holder, the being-hit event fires the reaction, and the
// reaction's damage hit is asked of the Goblin.  Its formula has no program in this content.db yet (the reader fix compiles it), so the check of the
// number is made when it does compile; what always holds is that the child Bleeding roll reaches the Goblin the parent hit, not nobody.
void test_reactions3_thorns_answers_a_goblins_real_hit(Fixture& f) {
    std::printf("a Goblin's real swing at the Thorns holder fires the reaction, and its Bleeding child reaches the Goblin\n");
    Thorns t;
    t.look_up(f);
    CHECKF(t.ok(), "the Thorns effect, its damage hit, its Bleeding child and the Bleeding effect are in the data");
    if (!t.ok()) return;
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string path = creature_path(f, "Goblin3_Gravehide");
    const auto bp = path.empty() ? std::nullopt : f.motion.find(path);
    CHECKF(!path.empty() && bp && tables.abilities_of(path), "the Goblin's Blueprint is in the motion table and the creature tables list its abilities");
    if (path.empty() || !bp || !tables.abilities_of(path)) return;
    Rig rig(1, 25, {}, true, 23);   // (Ranger, level 25; the seed makes the first Bleeding roll a success)
    if (!rig.ok()) return;
    combat_rig::World w;
    if (!w.problem.empty()) return;
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorldConfig cfg;
    cfg.seed = 5;
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion, cfg);
    world.set_ability_data(&tables);
    const uint64_t goblin = w.spawn(path, 0, 400, 3, 900, bp->half_height, nullptr, &*bp);
    CHECKF(goblin != 0, "the Goblin (level 3) stands 4 m ahead");
    if (goblin == 0) return;
    combat_rig::FakePlayer player(rig, 77, {0, 0, 1098});
    player.pipeline_hits = true;   // the session's path: the creature's hit raises the holder's being-hit reactions
    world.add_player(&player);
    combat::WorldHitPort port(world, w.registry, player.fighter());
    rig.pipe->set_combat(&port);
    CHECK(rig.pipe->grant_effect(t.effect->row.guid, 1, 0.1), "(the Ranger holds Thorns)");
    rig.take();
    int bleeding_seen = 0;
    std::ostringstream captured;
    const auto previous_logger = spdlog::default_logger();
    auto logger = std::make_shared<spdlog::logger>("thorns_fight", std::make_shared<spdlog::sinks::ostream_sink_mt>(captured));
    spdlog::set_default_logger(logger);
    for (double now = 0.2; now < 25.0; now += 1.0 / 15.0) {
        w.npcs->note_player(77, 0, 0, now);
        w.npcs->tick(now);
        world.tick(now);
        bleeding_seen = std::max(bleeding_seen, world.effect_stacks(goblin, t.bleeding->row.guid));
    }
    spdlog::default_logger()->flush();
    spdlog::set_default_logger(previous_logger);
    const std::string fight_log = captured.str();
    CHECKF(!player.hits.empty(), "the Goblin's swings reached the character (%zu hits)", player.hits.size());
    size_t took_health = 0;
    for (const combat::IncomingHit& h : player.hits) {
        double health = 0;
        for (const combat::ResolvedMod& m : h.mods) {
            if (m.stat_guid == content::kGuidHealth && m.value < 0) health += m.value;
        }
        if (health < 0 && (h.flags & stats::kHitMiss) == 0) ++took_health;
    }
    CHECKF(took_health > 0, "the Goblin's hits took Health from the holder (%zu of %zu)", took_health, player.hits.size());
    CHECKF(rig.pipe->reactions_fired() == took_health, "the reaction fired once for every hit that took Health (%zu over %zu)", rig.pipe->reactions_fired(), took_health);
    // The child roll: the family the damage hit fires at the attacker it was aimed at.  It reaches the Goblin even while the damage formula has no program.
    CHECKF(bleeding_seen > 0, "the Bleeding child reached the Goblin the parent damage hit (%d stacks at its first roll)", bleeding_seen);
    const auto hits = confirms_of(rig, t.damage->row.guid);
    if (thorns_damage_compiles(f, t)) {
        CHECKF(hits.size() == took_health, "the damage hit ran on the Goblin for every hit (%zu over %zu)", hits.size(), took_health);
        // the tooltip's one third of the holder's level, and the game's integer division of two whole numbers: 25 / 3 = 8
        CHECKF(!hits.empty() && !hits[0].targets.empty() && !hits[0].targets[0].mods.empty() && near(hits[0].targets[0].mods[0].value, -8.0, 0.01),
               "the reflected number is the holder's level 25 / 3 = 8 (%.3f)", hits.empty() || hits[0].targets.empty() || hits[0].targets[0].mods.empty() ? 0.0 : hits[0].targets[0].mods[0].value);
        // what the Goblin lost, by the log's own lines: the world holds a creature in its table only after its first tick, so the Health to compare with is the one the
        // Goblin started with (the most it can have), not a read before the fight (-1: not known yet)
        const GoblinLosses lost = goblin_losses(fight_log);
        const double start = world.max_health_of(goblin);
        CHECKF(start > 0, "the Goblin's Health is known (%.1f)", start);
        CHECKF(lost.reflections == took_health && near(lost.reflected, 8.0 * static_cast<double>(took_health), 1.5),
               "the Goblin lost 8 to each of the %zu hits that took Health (%.1f over %zu reflections)", took_health, lost.reflected, lost.reflections);
        CHECKF(near(start - world.health_of(goblin), lost.reflected + lost.bleeding, 3.0),
               "the Goblin's whole loss is the reflected damage and the Bleeding's ticks (%.1f of %.1f + %.1f)", start - world.health_of(goblin), lost.reflected, lost.bleeding);
    } else {
        CHECKF(hits.empty(), "the damage hit has no program in this content.db yet, so it ran on nobody (%zu)", hits.size());
        // what the Goblin lost is the Bleeding's ticks, not the reflected 8 for each of the hits that took Health
        const GoblinLosses lost = goblin_losses(fight_log);
        const double start = world.max_health_of(goblin);
        CHECKF(start > 0 && lost.reflections == 0 && near(start - world.health_of(goblin), lost.bleeding, 3.0),
               "the Goblin lost only the Bleeding's ticks, not the reflected number (%.1f of %.1f, %zu reflections)", start - world.health_of(goblin), lost.bleeding, lost.reflections);
    }
    // the log line must not claim a hit was applied when it did not run
    const bool ran = thorns_damage_compiles(f, t);
    const bool counted = ran ? fight_log.find("the reaction Ranger_Thorns fired (1 of its 1 hit(s) applied") != std::string::npos
                             : fight_log.find("the reaction Ranger_Thorns fired (0 of its 1 hit(s) applied") != std::string::npos;
    CHECKF(counted, "the reaction's log line counts the hits that ran (the damage hit %s)", ran ? "ran" : "did not run");
}


// A hit that took no Health raises no being-hit event the data's trigger can see: a miss carries no Health, and a full shield absorbs what did come.
void test_reactions3_thorns_does_not_fire_on_a_hit_that_took_no_health(Fixture& f) {
    std::printf("Thorns does not fire on a miss or on a hit a shield absorbed entirely\n");
    Thorns t;
    t.look_up(f);
    CHECKF(t.ok(), "the Thorns records are in the data");
    if (!t.ok()) return;
    Forge g(f, 1, {});
    g.player.pipeline_hits = true;   // the session's path: the hit raises the holder's being-hit reactions
    CHECKF(g.ok(), "the Ranger and the world: %s%s", g.rig.problem.c_str(), g.w.problem.c_str());
    if (!g.ok()) return;
    const uint64_t goblin = g.wolf(600, 0);
    if (goblin == 0) return;
    wake(g.world, goblin, g.player.actor_id(), 0.5);
    CHECK(g.rig.pipe->grant_effect(t.effect->row.guid, 1, 0.5), "(the Ranger holds Thorns)");
    g.rig.take();
    // a hit that misses
    combat::IncomingHit missed = goblin_hit(goblin, -5.58);
    missed.flags = stats::kHitMiss;
    const size_t fired = g.rig.pipe->reactions_fired();
    const double health = g.rig.health();
    g.player.receive_hit(missed, 1.0);
    CHECKF(g.rig.pipe->reactions_fired() == fired, "a miss raises no reaction (%zu over %zu)", g.rig.pipe->reactions_fired() - fired, fired);
    CHECKF(near(g.rig.health(), health, 1e-3), "the miss took no Health (%.1f of %.1f)", g.rig.health(), health);
    CHECKF(!g.world.effect_stacks(goblin, t.bleeding->row.guid), "a miss rolls no Bleeding either (%d stacks)", g.world.effect_stacks(goblin, t.bleeding->row.guid));
    // a hit a shield absorbs: the shield is an effect's TempHealth, every part of the hit goes to it
    const abilities::EffectDef* shell = f.catalog->effect_named("Mage_Shell");
    CHECK(shell != nullptr, "(a shield effect is in the data)");
    if (!shell) return;
    CHECK(g.rig.pipe->grant_effect(shell->row.guid, 1, 1.5), "(the Ranger holds a shield)");
    g.rig.take();
    double shield = 0;
    for (const auto& s : g.rig.pipe->shields()) shield = std::max(shield, s.second);
    CHECKF(shield > 1.0, "the shield holds more than the hit (%.1f)", shield);
    if (!(shield > 1.0)) return;
    const double before = g.rig.health();
    const size_t fired2 = g.rig.pipe->reactions_fired();
    g.player.receive_hit(goblin_hit(goblin, -1.0), 2.0);
    CHECKF(g.rig.pipe->reactions_fired() == fired2, "a hit the shield absorbed entirely raises no reaction (%zu over %zu)", g.rig.pipe->reactions_fired() - fired2, fired2);
    CHECKF(near(g.rig.health(), before, 1e-3), "the absorbed hit took no Health (%.1f of %.1f)", g.rig.health(), before);
    CHECKF(g.rig.pawn.broken.empty(), "(the pawn took every change)");
}


// Two creatures hit at once: each hit is its own event, and the child Bleeding roll follows the attacker its own parent hit, never the other one.
void test_reactions3_thorns_answers_two_creatures_at_once(Fixture& f) {
    std::printf("two creatures hit the Thorns holder at once: each reaction aims at its own attacker\n");
    Thorns t;
    t.look_up(f);
    CHECKF(t.ok(), "the Thorns records are in the data");
    if (!t.ok()) return;
    Forge g(f, 1, {}, 23);   // (the seed makes the first Bleeding roll a success)
    g.player.pipeline_hits = true;   // the session's path: the hit raises the holder's being-hit reactions
    CHECKF(g.ok(), "the Ranger and the world: %s%s", g.rig.problem.c_str(), g.w.problem.c_str());
    if (!g.ok()) return;
    const uint64_t first = g.wolf(300, 0);
    const uint64_t second = g.wolf(0, 300);
    if (first == 0 || second == 0) return;
    wake(g.world, first, g.player.actor_id(), 0.5);
    wake(g.world, second, g.player.actor_id(), 0.5);
    CHECK(g.rig.pipe->grant_effect(t.effect->row.guid, 1, 0.5), "(the Ranger holds Thorns)");
    g.rig.take();
    const size_t fired = g.rig.pipe->reactions_fired();
    g.player.receive_hit(goblin_hit(first, -5.58), 1.0);
    g.player.receive_hit(goblin_hit(second, -5.58), 1.0);
    CHECKF(g.rig.pipe->reactions_fired() == fired + 2, "both hits raised the reaction (%zu over %zu)", g.rig.pipe->reactions_fired() - fired, fired);
    CHECKF(g.world.effect_stacks(first, t.bleeding->row.guid) > 0, "the first attacker took the Bleeding roll of its own hit (%d stacks)", g.world.effect_stacks(first, t.bleeding->row.guid));
    CHECKF(g.world.effect_stacks(second, t.bleeding->row.guid) == 0, "the second attacker did not take the first one's roll (%d stacks)", g.world.effect_stacks(second, t.bleeding->row.guid));
    CHECK(g.rig.pawn.broken.empty(), "(the pawn took every change)");
}


// Thorns taken off (the ability's Ranger_Thorns_Remove): the effect that carries the reaction is gone, so the next hit raises none.
void test_reactions3_thorns_stops_when_it_is_removed(Fixture& f) {
    std::printf("Thorns taken off its holder stops answering hits\n");
    Thorns t;
    t.look_up(f);
    CHECKF(t.ok(), "the Thorns records are in the data");
    if (!t.ok()) return;
    Forge g(f, 1, {});
    g.player.pipeline_hits = true;   // the session's path: the hit raises the holder's being-hit reactions
    CHECKF(g.ok(), "the Ranger and the world: %s%s", g.rig.problem.c_str(), g.w.problem.c_str());
    if (!g.ok()) return;
    const uint64_t goblin = g.wolf(600, 0);
    if (goblin == 0) return;
    wake(g.world, goblin, g.player.actor_id(), 0.5);
    CHECK(g.rig.pipe->grant_effect(t.effect->row.guid, 1, 0.5), "(the Ranger holds Thorns)");
    g.rig.take();
    const size_t fired = g.rig.pipe->reactions_fired();
    g.player.receive_hit(goblin_hit(goblin, -5.58), 1.0);
    CHECKF(g.rig.pipe->reactions_fired() == fired + 1, "the hit with Thorns on fired it (%zu over %zu)", g.rig.pipe->reactions_fired() - fired, fired);
    CHECK(g.rig.pipe->remove_effect(t.effect->row.guid, 1.5), "(Thorns is taken off)");
    g.rig.take();
    const size_t fired2 = g.rig.pipe->reactions_fired();
    g.player.receive_hit(goblin_hit(goblin, -5.58), 2.0);
    CHECKF(g.rig.pipe->reactions_fired() == fired2, "the next hit raised nothing (%zu over %zu)", g.rig.pipe->reactions_fired() - fired2, fired2);
    CHECK(g.rig.pawn.broken.empty(), "(the pawn took every change)");
}


// A Thorns the Ranger put on another party member is carried by that member's own effects, and its data casts it from the Owner: the hit reads the
// HOLDER's level, not the Ranger's (the tooltip says the Ranger's; the data's answer is kept).  The number is checked when the formula has a program.
void test_reactions3_thorns_cast_on_another_member_uses_the_holders_level(Fixture& f) {
    std::printf("Thorns on a level-10 member: the data's Owner instigator names the holder's level, not the Ranger's\n");
    Thorns t;
    t.look_up(f);
    CHECKF(t.ok(), "the Thorns records are in the data");
    if (!t.ok()) return;
    Rig holder(1, 10, {}, true, 23);
    CHECKF(holder.ok(), "the level-10 holder and his runtime: %s", holder.problem.c_str());
    if (!holder.ok()) return;
    combat_rig::World w;
    if (!w.problem.empty()) return;
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion);
    const uint64_t goblin = w.spawn(wolf_archetype(f), 600, 0, 3);
    if (goblin == 0) return;
    combat_rig::FakePlayer player(holder, 77, {0, 0, 1098});
    player.pipeline_hits = true;   // the session's path
    world.add_player(&player);
    combat::WorldHitPort port(world, w.registry, player.fighter());
    holder.pipe->set_combat(&port);
    wake(world, goblin, player.actor_id(), 0.5);
    const abilities::ReactionDef* reaction = !t.effect->reactions.empty() && holder.catalog ? holder.catalog->reaction(t.effect->reactions.front()) : nullptr;
    CHECKF(reaction && reaction->row.instigator == "Owner", "the data casts the reaction from the Owner (Instigator = %s)", reaction ? reaction->row.instigator.c_str() : "?");
    CHECK(holder.pipe->grant_effect(t.effect->row.guid, 1, 0.5), "(the level-10 member holds the Ranger's Thorns)");
    holder.take();
    const size_t fired = holder.pipe->reactions_fired();
    player.receive_hit(goblin_hit(goblin, -5.58), 1.0);
    CHECKF(holder.pipe->reactions_fired() == fired + 1, "the holder's own effects carried the reaction (%zu over %zu)", holder.pipe->reactions_fired() - fired, fired);
    const auto hits = confirms_of(holder, t.damage->row.guid);
    if (thorns_damage_compiles(f, t)) {
        CHECKF(hits.size() == 1 && !hits[0].targets.empty() && !hits[0].targets[0].mods.empty() && near(hits[0].targets[0].mods[0].value, -3.0, 0.01),
               "the reflected number is the holder's level 10 / 3 = 3 (%.3f)", hits.empty() || hits[0].targets.empty() || hits[0].targets[0].mods.empty() ? 0.0 : hits[0].targets[0].mods[0].value);
    } else {
        CHECKF(hits.empty(), "the damage hit has no program in this content.db yet (%zu)", hits.size());
    }
    CHECKF(world.effect_stacks(goblin, t.bleeding->row.guid) > 0, "the Bleeding child reached the Goblin (%d stacks)", world.effect_stacks(goblin, t.bleeding->row.guid));
    CHECK(holder.pawn.broken.empty(), "(the pawn took every change)");
}


// ---- Epic's Resonance, the cases ------------------------------------------------------------------------------------------------------------

/// One Epic finisher with the Bard alone: `stacks` Resonance, no Pep; the AoE reaches the Bard alone, so one Cost1 spends one.
void epic_alone(Fixture& f, int stacks, int left_wanted) {
    Melodies m;
    m.look_up(f);
    if (!m.ok()) return;
    Forge g(f, 2, {});
    CHECKF(g.ok(), "the Bard and the world: %s%s", g.rig.problem.c_str(), g.w.problem.c_str());
    if (!g.ok()) return;
    const uint64_t wolf = g.wolf(1200, 0);
    if (wolf == 0) return;
    g.rig.know({"WeaponCombo_Melee_1H_Axe"});
    CHECKF(grant(g.rig, m.epic_self, 1, 99.5) && grant(g.rig, m.resonance, stacks, 99.5), "(Epic with %d Resonance)", stacks);
    const size_t fired = g.rig.pipe->reactions_fired();
    int32_t id = 500;
    finish_axe(g.rig, id, 100.0, wolf);
    CHECKF(g.rig.pipe->reactions_fired() > fired, "the Epic reaction fired (%zu over %zu)", g.rig.pipe->reactions_fired() - fired, fired);
    CHECKF(stacks_of(g.rig, m.resonance) == left_wanted, "alone with %d Resonance: the one target it reached spent one (%d -> %d)", stacks, stacks, stacks_of(g.rig, m.resonance));
    CHECKF(g.rig.pawn.effect(m.pep) != nullptr, "the Bard holds Pep from the projectile");
    CHECK(g.rig.pawn.broken.empty(), "(the pawn took every change)");
}

void test_reactions3_epic_alone_spends_once(Fixture& f) {
    std::printf("Epic alone at 1, 2 and 3 Resonance: the AoE reaches the Bard alone and one Cost1 spends one stack each time\n");
    epic_alone(f, 1, 0);
    epic_alone(f, 2, 1);
    epic_alone(f, 3, 2);
}


// 3 Resonance and one ally near: two targets (the Bard and the ally), so two Cost1 hits spend two.
void test_reactions3_epic_three_resonance_one_ally(Fixture& f) {
    std::printf("Epic with 3 Resonance and one ally near: two targets spend two, one is left\n");
    Melodies m;
    m.look_up(f);
    if (!m.ok()) return;
    Duo d(f);
    CHECKF(d.ok(), "the world: %s%s", d.a.problem.c_str(), d.w.problem.c_str());
    if (!d.ok()) return;
    const uint64_t wolf = d.wolf(1200, 0);
    if (wolf == 0) return;
    d.a.know({"WeaponCombo_Melee_1H_Axe"});
    const double t0 = 100.0;
    CHECK(grant(d.a, m.epic_self, 1, t0 - 0.5) && grant(d.a, m.resonance, 3, t0 - 0.5), "(Epic with three Resonance)");
    CHECK(grant(d.b, m.rooted, 1, t0 - 0.5), "(the ally is rooted)");
    int32_t id = 500;
    finish_axe(d.a, id, t0, wolf);
    CHECKF(stacks_of(d.b, m.rooted) == 0, "the ally is cleansed (%d left)", stacks_of(d.b, m.rooted));
    CHECKF(d.b.pawn.effect(m.pep) != nullptr, "the ally holds Pep");
    CHECKF(d.a.pawn.effect(m.pep) != nullptr, "the Bard holds Pep");
    CHECKF(stacks_of(d.a, m.resonance) == 1, "two targets spent two Resonance (3 -> %d)", stacks_of(d.a, m.resonance));
    CHECK(d.a.pawn.broken.empty() && d.b.pawn.broken.empty(), "(both pawns took every change)");
}


// 3 Resonance and two allies near: three targets (the Bard and both allies), so two Cost1 hits spend three.
void test_reactions3_epic_three_resonance_two_allies(Fixture& f) {
    std::printf("Epic with 3 Resonance and two allies near: three targets spend three\n");
    Melodies m;
    m.look_up(f);
    if (!m.ok()) return;
    Duo d(f);
    CHECKF(d.ok(), "the world: %s%s", d.a.problem.c_str(), d.w.problem.c_str());
    if (!d.ok()) return;
    Rig c(2, 25, {}, true, 103);
    combat_rig::FakePlayer pc(c, 79, {600, 0, 1098});
    combat::WorldHitPort portc(d.world, d.w.registry, pc.fighter());
    if (!c.ok()) return;
    d.world.add_player(&pc);
    c.pipe->set_combat(&portc);
    const uint64_t wolf = d.wolf(1200, 0);
    if (wolf == 0) return;
    d.a.know({"WeaponCombo_Melee_1H_Axe"});
    const double t0 = 100.0;
    CHECK(grant(d.a, m.epic_self, 1, t0 - 0.5) && grant(d.a, m.resonance, 3, t0 - 0.5), "(Epic with three Resonance)");
    CHECK(grant(d.b, m.rooted, 1, t0 - 0.5) && grant(c, m.rooted, 1, t0 - 0.5), "(both allies are rooted)");
    int32_t id = 500;
    const size_t from = d.a.log.size();
    finish_axe(d.a, id, t0, wolf);
    const auto hits = confirms_of(d.a, m.epic_hit, from);
    CHECKF(hits.size() == 3, "one Epic projectile lands on each of the three targets (%zu confirmations)", hits.size());
    CHECKF(stacks_of(d.b, m.rooted) == 0 && stacks_of(c, m.rooted) == 0, "both allies are cleansed (%d, %d)", stacks_of(d.b, m.rooted), stacks_of(c, m.rooted));
    CHECKF(d.b.pawn.effect(m.pep) != nullptr && c.pawn.effect(m.pep) != nullptr, "both hold Pep");
    CHECKF(stacks_of(d.a, m.resonance) == 0, "three targets spent three Resonance (3 -> %d)", stacks_of(d.a, m.resonance));
    d.world.remove_player(&pc);
    CHECK(d.a.pawn.broken.empty() && d.b.pawn.broken.empty() && c.pawn.broken.empty(), "(the pawns took every change)");
}


// 2 Resonance and three allies near: Clamp(2,1,3) is two targets, so two Cost1 hits spend two and a third ally is left out.
void test_reactions3_epic_two_resonance_three_allies(Fixture& f) {
    std::printf("Epic with 2 Resonance and three allies near: two targets spend two and one ally is left alone\n");
    Melodies m;
    m.look_up(f);
    if (!m.ok()) return;
    Duo d(f);
    CHECKF(d.ok(), "the world: %s%s", d.a.problem.c_str(), d.w.problem.c_str());
    if (!d.ok()) return;
    Rig c(2, 25, {}, true, 103), e(2, 25, {}, true, 104);
    combat_rig::FakePlayer pc(c, 79, {600, 0, 1098}), pe(e, 80, {900, 0, 1098});
    combat::WorldHitPort portc(d.world, d.w.registry, pc.fighter()), porte(d.world, d.w.registry, pe.fighter());
    if (!c.ok() || !e.ok()) return;
    d.world.add_player(&pc);
    d.world.add_player(&pe);
    c.pipe->set_combat(&portc);
    e.pipe->set_combat(&porte);
    const uint64_t wolf = d.wolf(1200, 0);
    if (wolf == 0) return;
    d.a.know({"WeaponCombo_Melee_1H_Axe"});
    const double t0 = 100.0;
    CHECK(grant(d.a, m.epic_self, 1, t0 - 0.5) && grant(d.a, m.resonance, 2, t0 - 0.5), "(Epic with two Resonance)");
    CHECK(grant(d.b, m.rooted, 1, t0 - 0.5) && grant(c, m.rooted, 1, t0 - 0.5) && grant(e, m.rooted, 1, t0 - 0.5), "(three allies are rooted)");
    int32_t id = 500;
    finish_axe(d.a, id, t0, wolf);
    const int cleaned = (stacks_of(d.b, m.rooted) == 0 ? 1 : 0) + (stacks_of(c, m.rooted) == 0 ? 1 : 0) + (stacks_of(e, m.rooted) == 0 ? 1 : 0);
    CHECKF(cleaned == 1, "the Bard and the nearest of the three allies are the two targets; one of the allies is cleansed (%d)", cleaned);
    // the Bard stands at the centre and is the nearest: he is one of the two, and the nearest ally the other
    CHECKF(d.b.pawn.effect(m.pep) != nullptr && c.pawn.effect(m.pep) == nullptr, "the two nearest were the Bard and the first ally");
    CHECKF(stacks_of(d.a, m.resonance) == 0, "two targets spent two Resonance (2 -> %d)", stacks_of(d.a, m.resonance));
    d.world.remove_player(&pc);
    d.world.remove_player(&pe);
    CHECK(d.a.pawn.broken.empty() && d.b.pawn.broken.empty() && c.pawn.broken.empty() && e.pawn.broken.empty(), "(the pawns took every change)");
}


// An ally behind the wall: the targeting volume asks line of sight, so the ally is not one of the targets and spends no Resonance.
void test_reactions3_epic_leaves_out_an_ally_behind_the_wall(Fixture& f) {
    std::printf("Epic leaves out an ally the wall stands between: the ally is not cleansed and spends no Resonance\n");
    Melodies m;
    m.look_up(f);
    if (!m.ok()) return;
    Duo d(f);
    CHECKF(d.ok(), "the world: %s%s", d.a.problem.c_str(), d.w.problem.c_str());
    if (!d.ok()) return;
    // the wall of the nav fixture stands at x -3,100..-2,900; the Bard west of it, the ally east of it
    d.pa.rig_fighter().move_to({-3200, 0, 1098});
    d.pb.rig_fighter().move_to({-2800, 0, 1098});
    const uint64_t wolf = d.wolf(-3200, 120);
    if (wolf == 0) return;
    d.a.know({"WeaponCombo_Melee_1H_Axe"});
    const double t0 = 100.0;
    CHECK(grant(d.a, m.epic_self, 1, t0 - 0.5) && grant(d.a, m.resonance, 3, t0 - 0.5), "(Epic with three Resonance)");
    CHECK(grant(d.b, m.rooted, 1, t0 - 0.5), "(the ally behind the wall is rooted)");
    int32_t id = 500;
    finish_axe(d.a, id, t0, wolf);
    CHECKF(stacks_of(d.b, m.rooted) == 1, "the ally behind the wall is not reached (%d stacks left)", stacks_of(d.b, m.rooted));
    CHECKF(d.b.pawn.effect(m.pep) == nullptr, "the ally behind the wall does not hold Pep");
    CHECKF(stacks_of(d.a, m.resonance) == 2, "only the Bard was reached, so one Resonance was spent (3 -> %d)", stacks_of(d.a, m.resonance));
    CHECK(d.a.pawn.broken.empty() && d.b.pawn.broken.empty(), "(both pawns took every change)");
}


// An ally who already holds Pep and has no crowd control is skipped by the targeting filter; the ally who is rooted is reached, and the Bard too.
void test_reactions3_epic_skips_an_ally_who_already_holds_pep(Fixture& f) {
    std::printf("Epic skips an ally with Pep and no crowd control; the Bard and the rooted ally spend two\n");
    Melodies m;
    m.look_up(f);
    if (!m.ok()) return;
    Duo d(f);
    CHECKF(d.ok(), "the world: %s%s", d.a.problem.c_str(), d.w.problem.c_str());
    if (!d.ok()) return;
    Rig c(2, 25, {}, true, 103);
    combat_rig::FakePlayer pc(c, 79, {600, 0, 1098});
    combat::WorldHitPort portc(d.world, d.w.registry, pc.fighter());
    if (!c.ok()) return;
    d.world.add_player(&pc);
    c.pipe->set_combat(&portc);
    const uint64_t wolf = d.wolf(1200, 0);
    if (wolf == 0) return;
    d.a.know({"WeaponCombo_Melee_1H_Axe"});
    const double t0 = 100.0;
    CHECK(grant(d.a, m.epic_self, 1, t0 - 0.5) && grant(d.a, m.resonance, 3, t0 - 0.5), "(Epic with three Resonance)");
    CHECK(grant(d.b, m.pep, 1, t0 - 0.5), "(one ally already holds Pep and has no crowd control)");
    CHECK(grant(c, m.rooted, 1, t0 - 0.5), "(the other ally is rooted)");
    int32_t id = 500;
    finish_axe(d.a, id, t0, wolf);
    CHECKF(stacks_of(d.b, m.pep) == 1, "the ally who already held Pep was skipped, not given it again (%d stacks)", stacks_of(d.b, m.pep));
    CHECKF(stacks_of(c, m.rooted) == 0, "the rooted ally was cleansed (%d left)", stacks_of(c, m.rooted));
    CHECKF(stacks_of(d.a, m.resonance) == 1, "two targets spent two Resonance (3 -> %d)", stacks_of(d.a, m.resonance));
    d.world.remove_player(&pc);
    CHECK(d.a.pawn.broken.empty() && d.b.pawn.broken.empty() && c.pawn.broken.empty(), "(the pawns took every change)");
}


// An ally under four crowd-control effects: the projectile takes up to three off, and the target still spends one of the three Resonance.
void test_reactions3_epic_four_crowd_controls_lose_three(Fixture& f) {
    std::printf("Epic on an ally with four crowd-control effects: three come off and the target spends one\n");
    Melodies m;
    m.look_up(f);
    if (!m.ok()) return;
    Duo d(f);
    CHECKF(d.ok(), "the world: %s%s", d.a.problem.c_str(), d.w.problem.c_str());
    if (!d.ok()) return;
    const uint64_t wolf = d.wolf(1200, 0);
    if (wolf == 0) return;
    d.a.know({"WeaponCombo_Melee_1H_Axe"});
    const double t0 = 100.0;
    CHECK(grant(d.a, m.epic_self, 1, t0 - 0.5) && grant(d.a, m.resonance, 3, t0 - 0.5), "(Epic with three Resonance)");
    int cc = 0;
    for (const int64_t e : {m.rooted, m.snared, m.stunned, m.chilled}) cc += grant(d.b, e, 1, t0 - 0.5) ? 1 : 0;
    CHECKF(cc == 4, "(the ally holds four crowd-control effects: %d)", cc);
    int32_t id = 500;
    finish_axe(d.a, id, t0, wolf);
    const int left = stacks_of(d.b, m.rooted) + stacks_of(d.b, m.snared) + stacks_of(d.b, m.stunned) + stacks_of(d.b, m.chilled);
    CHECKF(left == 1, "three of the four come off, one stays (%d left)", left);
    CHECKF(d.b.pawn.effect(m.pep) != nullptr, "the ally holds Pep");
    CHECKF(stacks_of(d.a, m.resonance) == 1, "the two targets (the Bard and the ally) spent two Resonance (3 -> %d)", stacks_of(d.a, m.resonance));
    CHECK(d.a.pawn.broken.empty() && d.b.pawn.broken.empty(), "(both pawns took every change)");
}


}  // namespace reactions2_fix

using reactions2_fix::test_reactions3_epic_alone_spends_once;
using reactions2_fix::test_reactions3_epic_four_crowd_controls_lose_three;
using reactions2_fix::test_reactions3_epic_leaves_out_an_ally_behind_the_wall;
using reactions2_fix::test_reactions3_epic_skips_an_ally_who_already_holds_pep;
using reactions2_fix::test_reactions3_epic_three_resonance_one_ally;
using reactions2_fix::test_reactions3_epic_three_resonance_two_allies;
using reactions2_fix::test_reactions3_epic_two_resonance_three_allies;
using reactions2_fix::test_reactions3_thorns_answers_a_goblins_real_hit;
using reactions2_fix::test_reactions3_thorns_answers_two_creatures_at_once;
using reactions2_fix::test_reactions3_thorns_cast_on_another_member_uses_the_holders_level;
using reactions2_fix::test_reactions3_thorns_does_not_fire_on_a_hit_that_took_no_health;
using reactions2_fix::test_reactions3_thorns_stops_when_it_is_removed;

AOC_REGISTER_TEST(test_reactions3_epic_alone_spends_once);
AOC_REGISTER_TEST(test_reactions3_epic_four_crowd_controls_lose_three);
AOC_REGISTER_TEST(test_reactions3_epic_leaves_out_an_ally_behind_the_wall);
AOC_REGISTER_TEST(test_reactions3_epic_skips_an_ally_who_already_holds_pep);
AOC_REGISTER_TEST(test_reactions3_epic_three_resonance_one_ally);
AOC_REGISTER_TEST(test_reactions3_epic_three_resonance_two_allies);
AOC_REGISTER_TEST(test_reactions3_epic_two_resonance_three_allies);
AOC_REGISTER_TEST(test_reactions3_thorns_answers_a_goblins_real_hit);
AOC_REGISTER_TEST(test_reactions3_thorns_answers_two_creatures_at_once);
AOC_REGISTER_TEST(test_reactions3_thorns_cast_on_another_member_uses_the_holders_level);
AOC_REGISTER_TEST(test_reactions3_thorns_does_not_fire_on_a_hit_that_took_no_health);
AOC_REGISTER_TEST(test_reactions3_thorns_stops_when_it_is_removed);
