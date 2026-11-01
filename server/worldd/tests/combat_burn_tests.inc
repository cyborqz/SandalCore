// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_burn_tests.inc: the checks of the burn on creatures (included by combat_registered.inc inside test_worldd_combat's anonymous namespace)
// ============================================================================

// ---- the burn lands on creatures ------------------------------------------------------------------------------------------------------------------------
// A burning hit raises two status stats the creature holds (the fire damage left to burn, and for how many seconds) and puts the one-second Status_BurningStat on it.  That effect's Apply hits
// (the damage of one share, the aura, the pool's and seconds' fall) run at once; when it ends its Expire hit puts it on again while there is fire left.  The creature shows what it holds as its
// CurrentEffects; each damage tick is confirmed to the player who burns it.
void test_creature_burn(Fixture& f) {
    std::printf("a creature takes a burn: its pool and seconds, one tick a second for 8 s, the aura, the end; a second burning hit; a kill, a leash reset and the player leaving\n");
    Rig rig(8, 25, {});
    CHECKF(rig.ok(), "the rig: %s", rig.problem.c_str());
    if (!rig.ok()) return;
    combat_rig::World w;
    if (!w.problem.empty()) return;
    const abilities::HitDef* application = f.catalog->hit_named("Mage_EE_Fire_Damage");
    const abilities::HitDef* tick_hit = f.catalog->hit_named("Status_BurningStatTick_Dmg");
    const abilities::EffectDef* burning_stat = f.catalog->effect_named("Status_BurningStat");
    const abilities::EffectDef* aura = f.catalog->effect_named("Status_BurningAura");
    const combat::CombatStatIds& ids = f.data->ids();
    CHECK(application && tick_hit && burning_stat && aura && ids.status_burning != 0 && ids.status_burning_dur != 0,
          "the burn's application hit, its tick, its two effects and its two status stats are in the data");
    if (!application || !tick_hit || !burning_stat || !aura || ids.status_burning == 0 || ids.status_burning_dur == 0) return;
    const std::string wolf_path = wolf_archetype(f);
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion);
    combat_rig::FakePlayer player(rig, 77, {0, 0, 1098});
    world.add_player(&player);
    const bool dump = std::getenv("AOC_EFFECT_DUMP") != nullptr;   // prints the creature's effect items every second, to read beside a recording of the same burn

    const auto value_of = [&](uint64_t wolf, int64_t stat) -> double {
        combat::Fighter* fgt = world.fighter_of(wolf);
        const stats::StatValue* v = fgt ? fgt->stats().find(stat) : nullptr;
        return v ? static_cast<double>(v->value) : -1.0;
    };
    const auto value_of_player = [&](int64_t stat) {
        const stats::StatValue* v = rig.cs->find(stat);
        return v ? static_cast<double>(v->value) : -1.0;
    };
    const auto items_of = [&](uint64_t wolf) {
        std::vector<std::pair<std::string, combat::EffectItemWrite>> out;
        if (const world::WorldActor* a = w.registry.find(wolf)) {
            for (const combat::EffectItemWrite& it : combat::read_effect_items(*a)) {
                const abilities::EffectDef* d = f.catalog->effect(it.item.effect_guid);
                out.emplace_back(d ? d->row.name : std::string("?"), it);
            }
        }
        return out;
    };
    const auto item_named = [&](uint64_t wolf, const char* name) -> std::optional<combat::EffectItemWrite> {
        for (const auto& [n, it] : items_of(wolf))
            if (n == name) return it;
        return std::nullopt;
    };
    const auto print_items = [&](uint64_t wolf, double since, const char* when) {
        if (!dump) return;
        std::printf("    [effect items %s, t0 + %.3f s]\n", when, since);
        for (const auto& [n, it] : items_of(wolf)) {
            std::printf("      id %lld  %-22s start t0 + %.3f  duration %.3f  tick %.2f  stacks %d  from %08x%08x%08x%08x\n", static_cast<long long>(it.item.effect_id), n.c_str(),
                        it.item.start_time - bootstrap::effect_clock(100.0), static_cast<double>(it.item.duration), static_cast<double>(it.item.tick_time), it.item.stacks, it.applier[0],
                        it.applier[1], it.applier[2], it.applier[3]);
        }
    };

    // a burning hit as the pipeline lands it on a creature: the hit's mods, then the effects it puts on (worked out after the mods)
    struct Burn {
        bool   ok = false;
        double pool = 0, dur = 0, pool_added = 0, expected_tick = 0;
        double formula_pool = 0;   // what the hit's own formula gave (the pool the mechanics run on)
    };
    const auto burn = [&](uint64_t wolf, double t) {
        Burn b;
        combat::Fighter* target = world.fighter_of(wolf);
        if (!target) return b;
        combat::HitMeta meta;
        meta.source_type = content::kSourceTypeReaction;
        combat::Resolved r;
        std::string why;
        if (!world.resolver().resolve(player.fighter(), *target, *application, meta, r, why)) return b;
        for (combat::ResolvedMod& m : r.mods) {
            if (m.stat_guid != ids.status_burning) continue;
            b.formula_pool = m.value;
            b.pool_added = m.value;
        }
        world.apply_hit_to_npc(wolf, player.actor_id(), r, t);
        b.pool = value_of(wolf, ids.status_burning);
        b.dur = value_of(wolf, ids.status_burning_dur);
        // what one tick of this burn is when it does not crit: the tick's own hit worked out for the burn as it stands now
        combat::HitMeta tick_meta;
        tick_meta.source_type = content::kSourceTypeEffect;
        tick_meta.source_guid = burning_stat->row.guid;
        for (int tries = 0; tries < 60; ++tries) {
            combat::Resolved probe;
            if (world.resolver().resolve(player.fighter(), *target, *tick_hit, tick_meta, probe, why) && probe.flags == 0 && !probe.mods.empty()) {
                b.expected_tick = probe.mods[0].value;
                break;
            }
        }
        combat::Resolved gate;
        if (!world.resolver().resolve_effects(player.fighter(), *target, *application, meta, gate, why)) return b;
        world.apply_effects(wolf, player.actor_id(), gate.effects, t);
        b.ok = true;
        return b;
    };
    struct Tick {
        double  at = 0, value = 0, pool = 0, dur = 0;
        uint8_t flags = 0;
        bool    predicted = false;
        int64_t effect = 0;
    };
    const auto drain = [&](uint64_t wolf, std::vector<Tick>& ticks) {
        for (const combat::EffectHitNote& n : player.effect_notes) {
            if (n.holder != wolf || n.hit_guid != tick_hit->row.guid) continue;
            ticks.push_back({n.at, n.mods.empty() ? 0.0 : n.mods[0].value, value_of(wolf, ids.status_burning), value_of(wolf, ids.status_burning_dur), n.flags, n.predicted, n.effect_guid});
        }
        player.effect_notes.clear();   // (the session confirms them to the client at its next tick)
    };
    const auto run = [&](uint64_t wolf, double from, double to, std::vector<Tick>& ticks, double t0) {
        size_t before = ticks.size();
        for (double t = from; t <= to + 1e-9; t += 0.05) {
            world.tick(t);
            drain(wolf, ticks);
            if (ticks.size() != before) {
                print_items(wolf, t - t0, "after a tick");
                before = ticks.size();
            }
        }
    };

    // ---- a fresh burn: 8 ticks
    const uint64_t wolf = w.spawn(wolf_path, 600, 0, 45, 100);   // (a high level, so that it lives through the burn)
    CHECK(wolf != 0, "a Wolf 6 m away");
    if (!wolf) return;
    const double t0 = 100.0;
    const Burn b = burn(wolf, t0);
    CHECK(b.ok, "the Fire Empowerment's hit lands on the Wolf and its effect goes on");
    CHECKF(b.formula_pool > 0.0, "the burn's formula gives the Mage a pool above 0 (%.4f for a level-25 Mage with %.2f magical power)", b.formula_pool, value_of_player(f.data->profile().magical.damage));
    CHECKF(near(b.dur, 8.0, 1e-4) && b.pool > 0.0 && near(b.pool, b.pool_added, b.pool * 1e-5), "the hit set the burn: %.2f fire damage over %.1f s (the hit's formula gave %.2f)", b.pool, b.dur, b.pool_added);
    std::vector<Tick> ticks;
    drain(wolf, ticks);
    CHECKF(ticks.size() == 1 && !ticks[0].predicted && ticks[0].effect == burning_stat->row.guid && ticks[0].value < 0,
           "the first tick comes with the application: one damage tick from Status_BurningStat, not predicted by the client (%zu)", ticks.size());
    const auto first_stat = item_named(wolf, "Status_BurningStat");
    const auto first_aura = item_named(wolf, "Status_BurningAura");
    CHECK(first_stat && first_aura, "the creature's effect list has the burn's one-second effect and its aura");
    if (!first_stat || !first_aura) return;
    CHECKF(near(first_stat->item.duration, 1.0, 1e-4) && near(first_aura->item.duration, 8.0, 1e-4) && first_aura->item.stacks == 1 && first_stat->item.stacks == 1 &&
               first_stat->item.start_time == first_aura->item.start_time && first_aura->applier == player.guid,
           "...the effect lasts 1.0 s and the aura 8.0 s (%.3f, %.3f), one stack each, from one start, from the player", static_cast<double>(first_stat->item.duration),
           static_cast<double>(first_aura->item.duration));
    print_items(wolf, 0.0, "at the application");
    run(wolf, t0 + 0.05, t0 + 0.55, ticks, t0);
    const auto stat_a = item_named(wolf, "Status_BurningStat");
    run(wolf, t0 + 0.6, t0 + 1.55, ticks, t0);
    const auto stat_b = item_named(wolf, "Status_BurningStat");
    const auto aura_b = item_named(wolf, "Status_BurningAura");
    CHECK(stat_a && stat_b && aura_b && stat_b->item.effect_id > stat_a->item.effect_id && aura_b->item.effect_id == first_aura->item.effect_id &&
              aura_b->item.start_time == first_aura->item.start_time,
          "a second on, the one-second effect is a new item (put on again as it ends) and the aura is the same item, not refreshed while it burns");
    run(wolf, t0 + 1.6, t0 + 9.0, ticks, t0);
    CHECKF(ticks.size() == 8, "a fresh burn ticks 8 times (%zu)", ticks.size());
    double worst_gap = 0;
    for (size_t i = 1; i < ticks.size(); ++i) worst_gap = std::max(worst_gap, std::fabs(ticks[i].at - ticks[i - 1].at - 1.0));
    CHECKF(worst_gap <= 0.05, "...one every second (the worst gap is off by %.4f s)", worst_gap);
    int plain = 0;
    bool same = true, falls = true;
    for (size_t i = 0; i < ticks.size(); ++i) {
        const bool crit = (ticks[i].flags & stats::kHitCrit) != 0, miss = (ticks[i].flags & stats::kHitMiss) != 0;
        if (!crit && !miss) {
            ++plain;
            same = same && near(ticks[i].value, b.expected_tick, std::fabs(b.expected_tick) * 0.01);
        } else if (crit) {
            same = same && std::fabs(ticks[i].value) > std::fabs(b.expected_tick);
        }
        const double left = static_cast<double>(8 - static_cast<int>(i) - 1);   // after tick i: its share fell off the pool and one second off the seconds
        falls = falls && near(ticks[i].dur, left, 1e-4) && near(ticks[i].pool, b.pool * left / 8.0, b.pool * 1e-4);
    }
    CHECKF(plain >= 4 && same && b.expected_tick < 0, "...each tick is the pool over its seconds through the damage equation, the same every second (%.3f expected; %d of 8 without a critical hit or miss)", b.expected_tick, plain);
    CHECK(falls, "...and after each tick the pool and the seconds have fallen by one share: pool x (8 - k) / 8 over 8 - k seconds");
    CHECK(near(value_of(wolf, ids.status_burning), 0.0, 1e-6) && near(value_of(wolf, ids.status_burning_dur), 0.0, 1e-6), "the burn is used up: nothing left to burn and no seconds left");
    CHECK(world.effect_count(wolf) == 0 && items_of(wolf).empty(), "...the creature's effect list is empty again (the aura ended with the last second)");
    double sum = 0;
    for (const Tick& tk : ticks) sum += tk.value;
    CHECKF(near(world.max_health_of(wolf) - world.health_of(wolf), -sum, 0.5), "the Wolf lost what the ticks took: %.2f of %.2f Health", world.max_health_of(wolf) - world.health_of(wolf), -sum);
    // the ticks the client was told: the confirmation the session makes from a note (Instigator the player, Source the Wolf, the number the tick took)
    {
        combat::EffectHitNote note;
        note.holder = wolf;
        note.hit_guid = tick_hit->row.guid;
        note.effect_guid = burning_stat->row.guid;
        note.mods.push_back({tick_hit->mods[0].stat_mod_guid, content::kGuidHealth, ticks[0].value});
        note.effective.push_back(ticks[0].value);
        note.at = t0;
        combat::WorldHitPort port(world, w.registry, player.fighter());
        rig.pipe->set_combat(&port);
        rig.pipe->confirm_effect_hit(note, t0);
        const std::vector<Seen> seen = rig.take();
        const bootstrap::HitConfirm* conf = nullptr;
        for (const Seen& s : seen)
            for (const bootstrap::HitConfirm& c : s.confirmations)
                if (c.record_guid == tick_hit->row.guid) conf = &c;
        CHECK(conf && conf->source_type == content::kSourceTypeEffect && conf->source_guid == burning_stat->row.guid && !conf->source_is_actor && conf->source && conf->source->ObjectId == wolf &&
                  !conf->instigator && conf->targets.size() == 1 && conf->targets[0].component.ObjectId == wolf && conf->targets[0].mods.size() == 1 && conf->targets[0].mods[0].value < 0 &&
                  !conf->predicted_autonomous && !conf->predicted_simulated,
              "a tick is confirmed as an effect's hit: the player is its instigator, the Wolf its source and its target, the number is the tick's, and neither client predicted it");
        rig.pipe->set_combat(nullptr);
    }

    // ---- a second burning hit while it burns: its pool and seconds are added, and the extra application ticks at once
    const uint64_t wolf4 = w.spawn(wolf_path, 600, 120, 45, 100);
    std::vector<Tick> ticks4;
    burn(wolf4, t0);
    drain(wolf4, ticks4);
    run(wolf4, t0 + 0.05, t0 + 3.5, ticks4, t0);
    CHECKF(ticks4.size() == 4 && near(value_of(wolf4, ids.status_burning_dur), 4.0, 1e-4), "(after 3.5 s the burn has ticked 4 times and has 4 s left: %zu, %.1f)", ticks4.size(), value_of(wolf4, ids.status_burning_dur));
    const double pool_before = value_of(wolf4, ids.status_burning);
    const Burn again = burn(wolf4, t0 + 3.5);
    CHECKF(near(again.dur, 8.0, 1e-4) && near(again.pool, pool_before + again.pool_added, again.pool * 1e-4),
           "a second burning hit adds its pool and max(8 - left, 4) seconds: %.2f over %.1f s (it was %.2f over 4 s, the hit added %.2f)", again.pool, again.dur, pool_before, again.pool_added);
    drain(wolf4, ticks4);
    CHECKF(ticks4.size() == 5 && near(ticks4.back().at, t0 + 3.5, 1e-6), "...and the extra application ticks at once (%zu ticks so far)", ticks4.size());
    run(wolf4, t0 + 3.55, t0 + 14.0, ticks4, t0);
    CHECKF(ticks4.size() == 12 && near(ticks4.back().at, t0 + 10.5, 0.05), "...so it ticked 12 times in all, the last at 10.5 s (%zu, %.3f)", ticks4.size(), ticks4.empty() ? 0.0 : ticks4.back().at - t0);
    CHECK(world.effect_count(wolf4) == 0 && near(value_of(wolf4, ids.status_burning), 0.0, 1e-6), "...and then nothing is left on it");

    // ---- the longest a burn lasts
    const uint64_t wolf6 = w.spawn(wolf_path, 600, 240, 45, 100);
    for (int i = 0; i < 10; ++i) burn(wolf6, t0);
    const double capped = value_of(wolf6, ids.status_burning_dur);
    CHECKF(capped > 8.0 && capped <= 24.0 + 1e-6, "ten burning hits at once cannot make it last more than 24 s (%.1f)", capped);

    // ---- a kill: nothing after it
    const uint64_t wolf2 = w.spawn(wolf_path, 600, 360, 45, 100);
    std::vector<Tick> ticks2;
    burn(wolf2, t0);
    drain(wolf2, ticks2);
    run(wolf2, t0 + 0.05, t0 + 2.5, ticks2, t0);
    CHECKF(ticks2.size() == 3 && world.effect_count(wolf2) >= 2, "(a second Wolf burns: %zu ticks in 2.5 s)", ticks2.size());
    combat::Resolved kill;
    kill.mods.push_back({1, content::kGuidHealth, -1.0e9});
    world.apply_hit_to_npc(wolf2, 77, kill, t0 + 2.5);
    CHECK(world.dead(wolf2) && world.effect_count(wolf2) == 0 && items_of(wolf2).empty(), "a kill ends the burn: the Wolf is dead, holds no effect, and its effect list is empty");
    run(wolf2, t0 + 2.55, t0 + 12.0, ticks2, t0);
    CHECKF(ticks2.size() == 3 && world.health_of(wolf2) == 0.0 && world.effect_count(wolf2) == 0, "...and nothing ticks after it (%zu ticks, Health %.1f)", ticks2.size(), world.health_of(wolf2));

    // ---- a leash reset does not end it: the burn follows the creature home and keeps ticking
    const uint64_t wolf3 = w.spawn(wolf_path, 600, 480, 45, 100);
    std::vector<Tick> ticks3;
    burn(wolf3, t0);
    drain(wolf3, ticks3);
    run(wolf3, t0 + 0.05, t0 + 2.5, ticks3, t0);
    world.gm_reset(wolf3, t0 + 2.5);   // (the reset a broken leash makes)
    CHECKF(ticks3.size() == 3 && world.effect_count(wolf3) >= 2 && item_named(wolf3, "Status_BurningAura"), "a reset leaves its effects: the Wolf still holds them and shows the aura (%zu effects)", world.effect_count(wolf3));
    run(wolf3, t0 + 2.55, t0 + 9.0, ticks3, t0);
    CHECKF(ticks3.size() == 8 && near(value_of(wolf3, ids.status_burning), 0.0, 1e-6), "...and the burn goes on ticking after the reset: 8 ticks in all (%zu), the pool used up", ticks3.size());

    // ---- the player who burns it leaves: what he put on it ends
    const uint64_t wolf5 = w.spawn(wolf_path, 600, 600, 45, 100);
    std::vector<Tick> ticks5;
    burn(wolf5, t0);
    drain(wolf5, ticks5);
    run(wolf5, t0 + 0.05, t0 + 2.5, ticks5, t0);
    world.remove_player(&player);
    CHECK(world.effect_count(wolf5) == 0 && items_of(wolf5).empty() && near(value_of(wolf5, ids.status_burning), 0.0, 1e-6), "a burn ends when the player who lit it leaves: nothing is held, the list is empty, nothing is left to burn");
    world.add_player(&player);
    run(wolf5, t0 + 2.55, t0 + 9.0, ticks5, t0);
    CHECKF(ticks5.size() == 3, "...and it ticks no more (%zu ticks)", ticks5.size());
    CHECK(rig.pawn.broken.empty(), "(the pawn took every change)");
}
AOC_REGISTER_TEST(test_creature_burn);

// The wand's Burning finisher through the ability runtime: the reaction's hit puts the burn on the Wolf, it is not confirmed (it has nothing to show), and the burn's first tick is.
void test_creature_burn_from_the_wand(Fixture& f) {
    std::printf("the wand's Burning finisher burns a creature: the reaction's hit is not confirmed, the burn's first tick is\n");
    Rig rig(8, 25, {});
    CHECKF(rig.ok(), "the rig: %s", rig.problem.c_str());
    if (!rig.ok()) return;
    const talents::Tree* wand = rig.talent_catalog->tree_named("Weapon_Ranged_Wand");
    const talents::Node* burning = nullptr;
    if (wand) {
        for (const talents::Node& n : wand->nodes)
            if (n.skill_name == "Weapon_Finisher_Burning" && !n.ranks.empty()) burning = &n;
    }
    CHECK(wand && burning, "the wand's tree and its Burning pick");
    if (!wand || !burning) return;
    rig.book->load(wand->guid, {burning->ranks.front().guid});
    combat_rig::World w;
    if (!w.problem.empty()) return;
    const std::string wolf_path = wolf_archetype(f);
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion);
    const uint64_t wolf = w.spawn(wolf_path, 600, 0, 45, 100);
    combat_rig::FakePlayer player(rig, 77, {0, 0, 1098});
    world.add_player(&player);
    combat::WorldHitPort port(world, w.registry, player.fighter());
    rig.pipe->set_combat(&port);
    rig.know({"WeaponCombo_Ranged_Wand"});
    const abilities::AbilityDef* wand_combo = f.catalog->ability_named("WeaponCombo_Ranged_Wand");
    const abilities::HitDef* beam_end = f.catalog->hit_named("Weapon_Wand_Beam_ComboEnd_Hit");
    const abilities::HitDef* burning_hit = f.catalog->hit_named("Weapon_Finisher_Burning");
    const abilities::HitDef* tick_hit = f.catalog->hit_named("Status_BurningStatTick_Dmg");
    const combat::CombatStatIds& ids = f.data->ids();
    CHECK(wolf != 0 && wand_combo && beam_end && burning_hit && tick_hit, "a Wolf, the wand's combo, its finishing hit and the Burning reaction's hit");
    if (!wolf || !wand_combo || !beam_end || !burning_hit || !tick_hit) return;
    int64_t targeting = content::kTargetingDefaultSelf;
    for (const content::AbilityHitRow& h : wand_combo->hits) {
        if (h.hit_guid != beam_end->row.guid || h.source != "hit") continue;
        for (const content::TargetingRow& tg : wand_combo->targetings)
            if (tg.key == h.key) targeting = tg.guid;
    }
    rig.cs->set_passives(rig.book->passives(wand->weapon_line));
    rig.cs->set_ranks(rig.book->held_ranks(wand->weapon_line));
    rig.pipe->set_reaction_carriers(rig.book->reaction_carriers(wand->weapon_line));
    rig.pipe->refresh(rig.clock, "the wand in hand", true);
    rig.take();

    double t = 100.0;
    TriggerInfo info;
    info.animation_index = 4;
    info.target_count = 1;
    info.target_actors = {wolf};
    CHECK(rig.pipe->trigger(wand_combo->row.guid, 500, t, info) == Refusal::None, "the wand's combo is cast at the Wolf");
    rig.run_to(t + 0.4);
    CHECK(rig.pipe->hit_request(wand_combo->row.guid, targeting, beam_end->row.guid, 1, 0, t + 0.8, {wolf}) == AbilityPipeline::HitVerdict::Scheduled, "the beam's end is asked for");
    int reaction_confirmed = 0, beam_confirmed = 0;
    for (const Seen& s : rig.run_to(t + 1.6)) {
        for (const bootstrap::HitConfirm& c : s.confirmations) {
            reaction_confirmed += c.record_guid == burning_hit->row.guid && c.source_type == content::kSourceTypeReaction ? 1 : 0;
            beam_confirmed += c.record_guid == beam_end->row.guid ? 1 : 0;
        }
    }
    CHECKF(rig.pipe->reactions_fired() >= 1, "the Burning reaction fired (%zu)", rig.pipe->reactions_fired());
    CHECKF(beam_confirmed == 1 && reaction_confirmed == 0, "the beam's end is confirmed (%d) and the reaction's hit, which only raises the burn, is not (%d)", beam_confirmed, reaction_confirmed);
    const double pool = world.fighter_of(wolf)->stats().find(ids.status_burning)->value;
    const double dur = world.fighter_of(wolf)->stats().find(ids.status_burning_dur)->value;
    CHECKF(pool > 0 && near(dur, 7.0, 1e-4), "the Wolf burns: %.2f fire damage to burn over %.1f s (a fresh 8 s burn, 1 s spent on the first tick)", pool, dur);
    const world::WorldActor* actor = w.registry.find(wolf);
    bool has_aura = false, has_stat = false;
    if (actor) {
        for (const combat::EffectItemWrite& it : combat::read_effect_items(*actor)) {
            const abilities::EffectDef* d = f.catalog->effect(it.item.effect_guid);
            has_aura = has_aura || (d && d->row.name == "Status_BurningAura");
            has_stat = has_stat || (d && d->row.name == "Status_BurningStat");
        }
    }
    CHECK(has_aura && has_stat, "...and shows the burn: the aura and the one-second effect are in its effect list");
    CHECKF(player.effect_notes.size() == 1 && player.effect_notes[0].hit_guid == tick_hit->row.guid, "the first tick is waiting to be confirmed to the player (%zu)", player.effect_notes.size());
    player.deliver_effect_notes();
    bool tick_confirmed = false;
    for (const bootstrap::HitConfirm& c : rig.pawn.confirmations)
        tick_confirmed = tick_confirmed || (c.record_guid == tick_hit->row.guid && c.source_type == content::kSourceTypeEffect && !c.targets.empty() && c.targets[0].component.ObjectId == wolf);
    CHECK(tick_confirmed, "...and the session confirms it: an effect's hit on the Wolf");
    CHECK(rig.pawn.broken.empty(), "(the pawn took every change)");
}
AOC_REGISTER_TEST(test_creature_burn_from_the_wand);

// A burning creature that is dragged beyond its leash and walks home takes every tick on the way (the ticks still deal their damage) and starts no fight and gains no threat from them;
// once it is home the burn's next tick starts the fight again, as a hit would; a burn that ended on the way leaves it idle at home.
void test_burning_creature_resets(Fixture& f) {
    std::printf("a burning creature that resets: its ticks add no threat and start no fight on the way home; the first tick after it is home starts the fight again; a burn that ended on the way leaves it idle\n");
    Rig rig(8, 25, {});
    CHECKF(rig.ok(), "the rig: %s", rig.problem.c_str());
    if (!rig.ok()) return;
    combat_rig::World w;
    if (!w.problem.empty()) return;
    const abilities::HitDef* application = f.catalog->hit_named("Mage_EE_Fire_Damage");
    const abilities::HitDef* tick_hit = f.catalog->hit_named("Status_BurningStatTick_Dmg");
    const combat::CombatStatIds& ids = f.data->ids();
    CHECK(application && tick_hit && ids.status_burning != 0 && ids.status_burning_dur != 0, "the burn's application hit, its tick and its two status stats are in the data");
    if (!application || !tick_hit || ids.status_burning == 0 || ids.status_burning_dur == 0) return;
    const std::string wolf_path = wolf_archetype(f);
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion);
    combat_rig::FakePlayer player(rig, 77, {0, 0, 1098});
    world.add_player(&player);
    using AiState = npc::ai::State;
    const auto value_of_player = [&](int64_t stat) {
        const stats::StatValue* v = rig.cs->find(stat);
        return v ? static_cast<double>(v->value) : -1.0;
    };
    const auto value_of = [&](uint64_t wolf, int64_t stat) -> double {
        combat::Fighter* fgt = world.fighter_of(wolf);
        const stats::StatValue* v = fgt ? fgt->stats().find(stat) : nullptr;
        return v ? static_cast<double>(v->value) : -1.0;
    };

    // a burning hit as the pipeline lands it: the hit's mods, then the effects it puts on
    const auto burn = [&](uint64_t wolf, double t) {
        combat::Fighter* target = world.fighter_of(wolf);
        if (!target) return false;
        combat::HitMeta meta;
        meta.source_type = content::kSourceTypeReaction;
        combat::Resolved r;
        std::string why;
        if (!world.resolver().resolve(player.fighter(), *target, *application, meta, r, why)) return false;
        world.apply_hit_to_npc(wolf, player.actor_id(), r, t);
        combat::Resolved gate;
        if (!world.resolver().resolve_effects(player.fighter(), *target, *application, meta, gate, why)) return false;
        world.apply_effects(wolf, player.actor_id(), gate.effects, t);
        return true;
    };
    struct Tick {
        double  at = 0, value = 0;
        AiState state = AiState::Idle;   // what its AI was doing as the tick landed (the state at the start of the step: the AI thinks after the effects of a step)
    };
    double now = 1.0;
    // one 0.05 s step of both worlds; the ticks that landed in it go to `ticks`
    const auto step = [&](uint64_t wolf, std::vector<Tick>& ticks) {
        const std::optional<AiState> before = world.ai_state(wolf);
        now += 0.05;
        w.npcs->note_player(77, player.rig_fighter().position().x, player.rig_fighter().position().y, now);
        w.npcs->tick(now);
        world.tick(now);
        for (const combat::EffectHitNote& n : player.effect_notes) {
            if (n.holder == wolf && n.hit_guid == tick_hit->row.guid) ticks.push_back({n.at, n.mods.empty() ? 0.0 : n.mods[0].value, before.value_or(AiState::Idle)});
        }
        player.effect_notes.clear();
    };
    const auto in_combat_flag = [&](uint64_t wolf) {
        const world::WorldActor* a = w.registry.find(wolf);
        return a && combat_rig::combat_flag(*a, "bInCombat");
    };
    const auto target_of = [&](uint64_t wolf) {
        const world::WorldActor* a = w.registry.find(wolf);
        return a ? combat_rig::active_target(*a) : uint64_t{0};
    };

    // What the walk home looked like: the creature is dragged `drag_cm` from its home (it keeps its home), its AI breaks the leash, and we watch it until it is home again.
    struct Walk {
        bool     moved = false, reset = false, arrived = false;
        double   reset_at = 0, arrived_at = 0;
        int      steps = 0, with_threat = 0, with_flag = 0, with_target = 0, fights_begun = 0;
        std::vector<Tick> ticks;   // every tick of the walk
        double   lost_after = 0;    // Health lost to the ticks that landed while it was in its reset
    };
    const auto drag_and_watch = [&](uint64_t wolf, double home_x, double drag_y, double give_up_after, Walk& out) {
        const npc::NpcWorld::RelocateResult rel = w.npcs->relocate(wolf, home_x, drag_y, 1000.0, false);
        out.moved = rel.status == npc::NpcWorld::Relocated::Ok;
        w.npcs->restore_home(wolf, nullptr);   // (the drag moved the walk's home with it: it goes home to where it was made)
        const uint64_t aggros0 = world.stats().aggros;
        const double started = now;
        while (now < started + give_up_after) {
            step(wolf, out.ticks);
            const std::optional<AiState> st = world.ai_state(wolf);
            if (st && *st == AiState::Reset) {
                if (!out.reset) {
                    out.reset = true;
                    out.reset_at = now;
                }
                ++out.steps;
                const npc::ai::NpcAi* ai = world.ai_of(wolf);
                if (ai && !ai->threat().empty()) ++out.with_threat;
                if (in_combat_flag(wolf)) ++out.with_flag;
                if (target_of(wolf) != 0) ++out.with_target;
            } else if (out.reset) {
                out.arrived = true;
                out.arrived_at = now;
                break;
            }
        }
        out.fights_begun = static_cast<int>(world.stats().aggros - aggros0);
        for (const Tick& tk : out.ticks)
            if (tk.state == AiState::Reset) out.lost_after += -tk.value;
    };
    const auto ticks_in_reset = [](const Walk& walk) {
        int n = 0;
        for (const Tick& tk : walk.ticks) n += tk.state == AiState::Reset ? 1 : 0;
        return n;
    };

    // ---- 1: the burn ends on the way. One burning hit makes 8 s; the Wolf is dragged 50 m off and needs longer than that to walk home (it walks home at about 3 m/s).
    const uint64_t wolf_b = w.spawn(wolf_path, 600, 2400, 45, 100);
    CHECK(wolf_b != 0, "a Wolf 25 m from the player");
    if (!wolf_b) return;
    CHECK(burn(wolf_b, now), "...burned once: 8 s");
    std::vector<Tick> home_ticks_b;
    const double lit_b = now;
    while (now < lit_b + 1.5) step(wolf_b, home_ticks_b);
    const double max_hp_b = world.max_health_of(wolf_b);
    Walk walk_b;
    drag_and_watch(wolf_b, 600, 7400, 30.0, walk_b);
    CHECK(walk_b.moved && walk_b.reset && walk_b.arrived, "dragged 50 m from its home it resets and walks home");
    const int b_ticks = ticks_in_reset(walk_b);
    CHECKF(b_ticks >= 4 && walk_b.arrived_at - lit_b > 8.0, "the burn ended on the way: %d of its ticks landed during the walk, and it arrived %.1f s after the burn began", b_ticks, walk_b.arrived ? walk_b.arrived_at - lit_b : -1.0);
    CHECKF(walk_b.with_threat == 0 && walk_b.with_flag == 0 && walk_b.with_target == 0 && walk_b.fights_begun == 0, "...no threat and no fight on the way (threat in %d of %d steps, flag in %d, target in %d, fights %d)", walk_b.with_threat, walk_b.steps,
           walk_b.with_flag, walk_b.with_target, walk_b.fights_begun);
    CHECKF(near(max_hp_b - world.health_of(wolf_b), walk_b.lost_after, 0.5), "...and the ticks of the walk took %.2f Health (%.2f gone)", walk_b.lost_after, max_hp_b - world.health_of(wolf_b));
    std::printf("    1: burn of 8 s, dragged 50 m: its reset began %.2f s after the burn, it was home %.2f s after it (%.2f s walking); %d of the 8 ticks landed on the way, %.2f Health lost to them; threat table filled in %d, combat flag up in %d, fights begun %d\n",
                walk_b.reset_at - lit_b, walk_b.arrived_at - lit_b, walk_b.arrived_at - walk_b.reset_at, b_ticks, walk_b.lost_after, walk_b.with_threat, walk_b.with_flag, walk_b.fights_begun);
    CHECK(world.effect_count(wolf_b) == 0 && near(value_of(wolf_b, ids.status_burning), 0.0, 1e-6), "the burn is over: nothing is left on it");
    const uint64_t fights_b = world.stats().aggros;
    std::vector<Tick> rest;
    const double rest_from = now;
    while (now < rest_from + 3.0) step(wolf_b, rest);
    const std::optional<AiState> st_b = world.ai_state(wolf_b);
    CHECKF(st_b && *st_b == AiState::Idle && world.stats().aggros == fights_b && !in_combat_flag(wolf_b) && target_of(wolf_b) == 0 && rest.empty(),
           "it stands idle at home for the next 3 s: no tick, no fight, no combat flag (%zu ticks)", rest.size());
    // ---- 2: the burn outlasts the walk (the first Wolf is idle at home by now and the player is unhurt by it). Five burning hits make 24 s of it (five ticks come with them); the Wolf fights at home for 1.5 s, is dragged 32 m off (its leash is 30 m) and walks back in about 10 s.
    const uint64_t wolf_a = w.spawn(wolf_path, 600, 3200, 45, 100);
    CHECK(wolf_a != 0, "a Wolf 32 m from the player (a level 45 Wolf kills the rig's 894 Health in two bites: it must not reach him before it is dragged away)");
    if (!wolf_a) return;
    bool lit = true;
    for (int i = 0; i < 5; ++i) lit = burn(wolf_a, now) && lit;
    CHECK(lit, "the Fire Empowerment's hit lands on the Wolf five times: a burn of 24 s");
    std::vector<Tick> home_ticks;
    const double lit_at = now;
    while (now < lit_at + 1.5) step(wolf_a, home_ticks);
    const std::optional<AiState> fighting = world.ai_state(wolf_a);
    const world::WorldActor* wa_now = w.registry.find(wolf_a);
    CHECKF(fighting && (*fighting == AiState::Chase || *fighting == AiState::Attack) && in_combat_flag(wolf_a) && target_of(wolf_a) == 77,
           "burning, it fights the player who burns it: its AI is in a fight and its combat flag is up (its AI is %s, the flag %d, it names %llu; the player has %.0f Health; it stands at %.0f, %.0f; %zu effects)",
           fighting ? npc::ai::state_name(*fighting) : "not a combatant", in_combat_flag(wolf_a) ? 1 : 0, static_cast<unsigned long long>(target_of(wolf_a)),
           value_of_player(content::kGuidHealth), wa_now ? wa_now->x : -1.0, wa_now ? wa_now->y : -1.0, world.effect_count(wolf_a));
    CHECKF(home_ticks.size() >= 2, "...and its burn ticks every second (%zu ticks in 1.5 s; %.2f to burn over %.1f s with %zu effects)", home_ticks.size(), value_of(wolf_a, ids.status_burning), value_of(wolf_a, ids.status_burning_dur), world.effect_count(wolf_a));
    const double max_hp = world.max_health_of(wolf_a);
    Walk walk_a;
    const uint64_t resets_before = world.stats().resets;
    drag_and_watch(wolf_a, 600, 6400, 25.0, walk_a);
    CHECK(walk_a.moved && walk_a.reset, "dragged 32 m from its home it breaks its leash: its AI is in its reset");
    CHECKF(world.stats().resets == resets_before + 1, "...once (%llu resets)", static_cast<unsigned long long>(world.stats().resets - resets_before));
    CHECKF(walk_a.arrived && walk_a.steps >= 100, "...and walks home: it arrives %.1f s after the reset began", walk_a.arrived ? walk_a.arrived_at - walk_a.reset_at : -1.0);
    const int a_ticks = ticks_in_reset(walk_a);
    CHECKF(a_ticks >= 5, "the burn ticked %d times on the way home", a_ticks);
    CHECKF(walk_a.with_threat == 0 && walk_a.with_flag == 0 && walk_a.with_target == 0 && walk_a.fights_begun == 0,
           "...and those ticks added no threat and began no fight: the threat table was empty in %d of %d steps, the combat flag was up in %d, a target named in %d, fights begun %d", walk_a.with_threat, walk_a.steps,
           walk_a.with_flag, walk_a.with_target, walk_a.fights_begun);
    CHECKF(near(max_hp - world.health_of(wolf_a), walk_a.lost_after, 0.5) && walk_a.lost_after > 0.0,
           "...but they dealt their damage: it healed to full as it turned, then lost %.2f of %.2f Health to the %d ticks of the walk (%.2f gone)", walk_a.lost_after, max_hp, a_ticks, max_hp - world.health_of(wolf_a));
    CHECKF(walk_a.arrived && world.effect_count(wolf_a) >= 2 && value_of(wolf_a, ids.status_burning) > 0.0, "it is home and still burning: its effects and its burn are still on it (%zu effects, %.2f to burn over %.1f s; %zu ticks in all since the drag; %zu before it)",
           world.effect_count(wolf_a), value_of(wolf_a, ids.status_burning), value_of(wolf_a, ids.status_burning_dur), walk_a.ticks.size(), home_ticks.size());
    // home: idle until the burn's next tick, which starts the fight again
    std::vector<Tick> after;
    const uint64_t fights_home = world.stats().aggros;
    double fight_at = -1;
    int idle_steps = 0;
    while (now < walk_a.arrived_at + 2.0 && fight_at < 0) {
        step(wolf_a, after);
        const std::optional<AiState> st = world.ai_state(wolf_a);
        if (st && (*st == AiState::Chase || *st == AiState::Attack)) fight_at = now;
        else if (st && *st == AiState::Idle) ++idle_steps;
    }
    CHECKF(!after.empty() && fight_at > 0 && std::fabs(fight_at - after.front().at) <= 0.12,
           "the first tick after it is home starts the fight again: the tick at %.2f, the fight at %.2f (%d idle steps in between)", after.empty() ? -1.0 : after.front().at - lit_at, fight_at - lit_at, idle_steps);
    CHECKF(world.stats().aggros == fights_home + 1 && in_combat_flag(wolf_a) && target_of(wolf_a) == 77,
           "...against the player who burns it: one fight began (%llu), its combat flag is up and it names him", static_cast<unsigned long long>(world.stats().aggros - fights_home));
    std::printf("    2: burn of 24 s, dragged 32 m: its reset began %.2f s after the burn, it was home %.2f s after it (%.2f s walking); %d ticks landed on the way, %.2f Health lost to them; threat table filled in %d, combat flag up in %d, fights begun %d; the first tick after it was home came %.2f s after it arrived and the fight began in the same step\n",
                walk_a.reset_at - lit_at, walk_a.arrived_at - lit_at, walk_a.arrived_at - walk_a.reset_at, a_ticks, walk_a.lost_after, walk_a.with_threat, walk_a.with_flag, walk_a.fights_begun, after.empty() ? -1.0 : after.front().at - walk_a.arrived_at);
    const npc::ai::NpcAi* ai_a = world.ai_of(wolf_a);
    CHECK(ai_a && !ai_a->threat().empty() && ai_a->target() == 77, "...and the tick's damage is threat on its table, as a hit's would be");

    CHECK(rig.pawn.broken.empty(), "(the pawn took every change)");
}
AOC_REGISTER_TEST(test_burning_creature_resets);
