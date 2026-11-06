// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_wake_tests.inc: what ends a crowd control on a creature before its time (included by combat_registered.inc inside test_worldd_combat's anonymous namespace)
//
//  A sleep wakes at the first damage the creature takes, but for a lullaby's hit and a poison's tick; the lockout of a slow takes the slow off when its stacks reach the limit; what a player put on
//  the creatures ends when he dies; a Mage's lightning on a Frozen creature shatters it; a facing that goes on hold forgets the turn it was on; a stun stops the leap it lands in.
// ============================================================================

/// A loss of Health (or whatever `stat` is) as a hit of the record `record` (null: a hit of no record) takes it from `wolf`, dealt by the player of the bed at time `t`.
static void wake_loss(dots::Bed& bed, uint64_t wolf, double t, const abilities::HitDef* record, double amount = -5.0, uint8_t flags = 0, int64_t stat = content::kGuidHealth) {
    combat::Resolved r;
    r.flags = flags;
    r.mods.push_back({1, stat, amount});
    combat::HitOrigin origin;
    origin.hit = record;
    origin.source_type = content::kSourceTypeAbility;
    bed.world.apply_hit_to_npc(wolf, bed.player.actor_id(), r, t, 0, record ? &origin : nullptr);
}

/// Puts an effect on the Wolf for `seconds` as a hit's application does, from `by` (the bed's first player when null), through the world's own door.
static void wake_put(dots::Bed& bed, uint64_t wolf, const abilities::EffectDef* def, double seconds, double t, combat_rig::FakePlayer* by = nullptr) {
    effects::Applied a;
    a.def = def;
    a.duration = seconds;
    bed.world.apply_effects(wolf, (by ? *by : bed.player).actor_id(), {a}, t);
}

/// What the Wolf shows of an effect: its crowd control flags, its stacks and items, and the share of its speed its walker keeps.
struct WakeState {
    int64_t flags = -1;
    int     stacks = 0;
    size_t  items = 0;
    double  scale = -1;
};

static WakeState wake_state(dots::Bed& bed, uint64_t wolf, const abilities::EffectDef* def) {
    WakeState s;
    if (const world::WorldActor* a = bed.w.registry.find(wolf)) s.flags = cc_now(*a).flags;
    s.stacks = bed.world.effect_stacks(wolf, def->row.guid);
    s.items = bed.items_named(wolf, def->row.name.c_str());
    if (const npc::Mover* m = bed.w.npcs->mover(wolf)) s.scale = m->speed_scale();
    return s;
}

// ---- a sleep wakes at the first damage ------------------------------------------------------------------------------------------------------------------------
void test_cc_sleep_wakes(Fixture& f) {
    std::printf("a sleeping creature wakes at the first damage it takes, but for a lullaby's hit and a poison's tick; a stun or a root is not broken by damage\n");
    dots::Bed bed(f);
    CHECKF(bed.ok(), "the bed: %s", bed.problem().c_str());
    if (!bed.ok()) return;
    const abilities::EffectDef* asleep = f.catalog->effect_named("Status_Incapacitated");
    const abilities::EffectDef* slumber = f.catalog->effect_named("Mage_Slumber");
    const abilities::EffectDef* hunter = f.catalog->effect_named("Summoner_Pet_Hunter_Incap");
    const abilities::EffectDef* hunter_ranged = f.catalog->effect_named("Summoner_Pet_Hunter_Incap_Ranged");
    const abilities::EffectDef* stunned = f.catalog->effect_named("Status_Stunned");
    const abilities::EffectDef* rooted = f.catalog->effect_named("Status_Rooted");
    const abilities::EffectDef* poisoned = f.catalog->effect_named("Status_Poisoned");
    const abilities::HitDef* lullaby = f.catalog->hit_named("Bard_Song_Lullaby_Enemy");
    const abilities::HitDef* lullaby_area = f.catalog->hit_named("Bard_Song_Lullaby_AOE_Enemy");
    const abilities::HitDef* poison_tick = f.catalog->hit_named("Status_Poisoned_Tick");
    const abilities::HitDef* frostbolt = f.catalog->hit_named("Mage_Frostbolt_Hit");
    const bool have = asleep && slumber && hunter && hunter_ranged && stunned && rooted && poisoned && lullaby && lullaby_area && poison_tick && frostbolt;
    CHECK(have, "the data has the four sleeps, a stun, a root, a poison, the two lullaby hits, the poison's tick and a Frostbolt's hit");
    if (!have) return;

    // the data: each sleep breaks at the first damage, except for the hit records the fragility names
    for (const abilities::EffectDef* e : {asleep, slumber, hunter, hunter_ranged}) {
        const bool ok = e->breaks_on_damage && e->breaks_on_damage->except_hits.size() == 3 && e->breaks_on_damage->leaves(lullaby->row.guid) && e->breaks_on_damage->leaves(lullaby_area->row.guid) &&
                        e->breaks_on_damage->leaves(poison_tick->row.guid);
        CHECKF(ok, "%s breaks at the first damage, but for Bard_Song_Lullaby_Enemy, Bard_Song_Lullaby_AOE_Enemy and Status_Poisoned_Tick", e->row.name.c_str());
    }
    CHECK(!stunned->breaks_on_damage && !rooted->breaks_on_damage, "a stun and a root have no fragility of that kind");

    const double t0 = 100.0;
    // 1: asleep, then hits that leave the sleep alone
    const uint64_t wolf = bed.wolf(0);
    CHECK(wolf != 0, "a Wolf 30 m away");
    if (!wolf) return;
    wake_put(bed, wolf, asleep, 10.0, t0);
    WakeState s = wake_state(bed, wolf, asleep);
    CHECKF(s.stacks == 1 && s.flags == 7 && s.items == 1, "put to sleep: one stack of Incapacitated, crowd control flags %lld (7), %zu item(s)", static_cast<long long>(s.flags), s.items);
    double t = t0 + 0.1;
    wake_loss(bed, wolf, t, lullaby);
    wake_loss(bed, wolf, t + 0.05, lullaby_area);
    wake_loss(bed, wolf, t + 0.1, poison_tick);
    t += 0.2;
    s = wake_state(bed, wolf, asleep);
    CHECKF(s.stacks == 1 && s.flags == 7 && s.items == 1, "a Lullaby's hit, its area hit and a poison's tick each take Health and the sleep stays on (stacks %d, flags %lld)", s.stacks, static_cast<long long>(s.flags));
    wake_loss(bed, wolf, t, frostbolt, -5.0, stats::kHitMiss);   // a miss takes nothing
    wake_loss(bed, wolf, t + 0.05, frostbolt, +5.0);             // a heal takes no Health
    wake_loss(bed, wolf, t + 0.1, frostbolt, -5.0, 0, content::kGuidMana);   // a loss of Mana is no loss of Health
    t += 0.2;
    s = wake_state(bed, wolf, asleep);
    CHECKF(s.stacks == 1 && s.flags == 7, "...nor does a miss, a heal or a loss of Mana end it (stacks %d, flags %lld)", s.stacks, static_cast<long long>(s.flags));
    // a poison on the sleeper ticks for 6 s of its own: it takes Health and the sleep stays
    wake_put(bed, wolf, poisoned, 8.0, t);
    const double health_before = bed.world.health_of(wolf);
    bed.run(t + 0.05, t + 6.0);
    t += 6.05;
    s = wake_state(bed, wolf, asleep);
    CHECKF(bed.world.health_of(wolf) < health_before && bed.world.effect_stacks(wolf, poisoned->row.guid) >= 1 && s.stacks == 1 && s.flags == 7,
           "a poison's own ticks take Health from the sleeper (%.1f -> %.1f) and it sleeps on (stacks %d, flags %lld)", health_before, bed.world.health_of(wolf), s.stacks, static_cast<long long>(s.flags));
    // any other hit that takes Health wakes it at once: the state clears in the same call
    wake_loss(bed, wolf, t, frostbolt);
    s = wake_state(bed, wolf, asleep);
    CHECKF(s.stacks == 0 && s.flags == 0 && s.items == 0 && s.scale == 1.0,
           "a Frostbolt's damage wakes it at once: stacks %d, crowd control flags %lld (0), %zu item(s), and its walker keeps %.2f of its speed", s.stacks, static_cast<long long>(s.flags), s.items, s.scale);
    CHECKF(bed.world.effect_stacks(wolf, poisoned->row.guid) >= 1, "...and the poison it was bitten by goes on (%d)", bed.world.effect_stacks(wolf, poisoned->row.guid));

    // 2: the other sleeps, a stun and a root
    const uint64_t slumbering = bed.wolf(100), hunted = bed.wolf(200), hunted_far = bed.wolf(300), stunned_wolf = bed.wolf(400), rooted_wolf = bed.wolf(500);
    CHECK(slumbering && hunted && hunted_far && stunned_wolf && rooted_wolf, "five more Wolves");
    if (!slumbering || !hunted || !hunted_far || !stunned_wolf || !rooted_wolf) return;
    wake_put(bed, slumbering, slumber, 10.0, t0);
    wake_put(bed, hunted, hunter, 10.0, t0);
    wake_put(bed, hunted_far, hunter_ranged, 10.0, t0);
    wake_put(bed, stunned_wolf, stunned, 10.0, t0);
    wake_put(bed, rooted_wolf, rooted, 10.0, t0);
    for (const auto& [id, def] : std::vector<std::pair<uint64_t, const abilities::EffectDef*>>{{slumbering, slumber}, {hunted, hunter}, {hunted_far, hunter_ranged}, {stunned_wolf, stunned}, {rooted_wolf, rooted}}) {
        CHECKF(wake_state(bed, id, def).stacks == 1, "%s is on its Wolf", def->row.name.c_str());
        wake_loss(bed, id, t0 + 0.5, nullptr);
    }
    for (const auto& [id, def] : std::vector<std::pair<uint64_t, const abilities::EffectDef*>>{{slumbering, slumber}, {hunted, hunter}, {hunted_far, hunter_ranged}}) {
        const WakeState w = wake_state(bed, id, def);
        CHECKF(w.stacks == 0 && w.flags == 0, "%s ends at the first damage (stacks %d, flags %lld)", def->row.name.c_str(), w.stacks, static_cast<long long>(w.flags));
    }
    const WakeState st = wake_state(bed, stunned_wolf, stunned), ro = wake_state(bed, rooted_wolf, rooted);
    CHECKF(st.stacks == 1 && st.flags == 7 && ro.stacks == 1 && ro.flags == 1, "a stun and a root hold through damage (stun: stacks %d flags %lld; root: stacks %d flags %lld)", st.stacks, static_cast<long long>(st.flags),
           ro.stacks, static_cast<long long>(ro.flags));

    // 3: in a fight.  The Condemn puts the Wolf to sleep on the tick after it began an attack; three seconds into the sleep it takes a hit: it wakes in that tick (the update says flags 0), and it
    // attacks again; the same fight with no hit sleeps through the same four seconds
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string wolf_path = creature_path(f, "/Beast/Canine/Wolf.Default__Wolf_C");
    const abilities::HitDef* condemn = f.catalog->hit_named("Cleric_Condemn_Hit");
    CHECK(!wolf_path.empty() && condemn, "the Blueprint table has the Wolf and the data has the Condemn's hit");
    if (wolf_path.empty() || !condemn) return;
    CcRun woken, slept;
    woken.watch = asleep;
    slept.watch = asleep;
    const auto began = [](CcRun& run) { return [&run](LungeStepContext&) { return !run.s.empty() && run.s.back().cast_began; }; };
    const bool slept_ran = cc_fight(f, tables, wolf_path, 20.0, 0, 200, 0.2, cc_player_steer(200, nullptr, 0), {CcEvent{1.5, cc_put(slept, condemn), false, began(slept)}}, slept);
    const bool woken_ran = cc_fight(
        f, tables, wolf_path, 20.0, 0, 200, 0.2, cc_player_steer(200, nullptr, 0),
        {CcEvent{1.5, cc_put(woken, condemn), false, began(woken)},
         CcEvent{0.0,
                 [&woken](LungeStepContext& c) {
                     combat::Resolved r;
                     r.mods.push_back({1, content::kGuidHealth, -5.0});
                     c.world.apply_hit_to_npc(c.creature, c.player.actor_id(), r, c.now);
                     woken.second_at = c.now;
                 },
                 false, [&woken](LungeStepContext& c) { return woken.on_at > 0 && c.now >= woken.on_at + 3.0; }}},
        woken);
    CHECK(slept_ran && woken_ran, "two fights in which the Condemn puts the Wolf to sleep run, one of them with a hit three seconds into the sleep");
    if (slept_ran && woken_ran && woken.on_at > 0 && woken.second_at > 0) {
        const size_t wake_i = cc_at(woken.s, woken.second_at);
        const size_t natural_i = cc_off(woken.s, woken.on_at, woken.on_seconds);
        const size_t asleep_casts = cc_casts(woken.s, woken.on_at, woken.second_at - 1e-6), asleep_hits = cc_hits(woken.hits, woken.on_at, woken.second_at - 1e-6);
        const size_t awake_casts = cc_casts(woken.s, woken.second_at, woken.second_at + 4.0), slept_casts = cc_casts(slept.s, woken.second_at, woken.second_at + 4.0);
        std::printf("    sleep put on at %.2f s for %.2f s, hit at %.2f s: %zu attacks and %zu hits while asleep; in the 4 s after the hit %zu attacks begun (the sleeper that is not hit: %zu)\n", woken.on_at,
                    woken.on_seconds, woken.second_at, asleep_casts, asleep_hits, awake_casts, slept_casts);
        CHECKF(wake_i < woken.s.size() && natural_i < woken.s.size() && woken.second_at + 4.0 < woken.on_at + woken.on_seconds - 1.0, "the hit comes well before the sleep would have ended (at %.2f s, the sleep's end at %.2f s)",
               woken.second_at, woken.on_at + woken.on_seconds);
        if (wake_i < woken.s.size() && natural_i < woken.s.size()) {
            CHECKF(asleep_casts == 0 && asleep_hits == 0, "asleep it begins no attack and none of its hits lands (%zu attacks, %zu hits)", asleep_casts, asleep_hits);
            CHECKF(woken.s[wake_i].cc_flags == 0 && woken.watch_left[wake_i] == 0.0, "the update of the tick it is hit in says the state is clear (flags %d), and the sleep is off (%.2f s left)", woken.s[wake_i].cc_flags,
                   woken.watch_left[wake_i]);
            CHECKF(awake_casts >= 1 && slept_casts == 0, "...and it attacks again within 4 s (%zu attacks begun), where the sleeper that is not hit begins none (%zu)", awake_casts, slept_casts);
        }
    }

    // 4: a hit of the world's player-death path: what he put on the creatures ends at once (in the same call), and what another player put on goes on
    const uint64_t bound = bed.wolf(600);
    CHECK(bound != 0, "a Wolf for the player's death");
    if (!bound) return;
    wake_put(bed, bound, rooted, 30.0, t0);
    wake_put(bed, bound, f.catalog->effect_named("Status_Snared"), 30.0, t0, &bed.other);
    CHECKF(bed.world.effect_count(bound) == 2 && wake_state(bed, bound, rooted).flags == 17, "two players hold the Wolf: a root and a snare (%zu effects, flags %lld)", bed.world.effect_count(bound),
           static_cast<long long>(wake_state(bed, bound, rooted).flags));
    bed.world.player_died(bed.player.actor_id());
    const WakeState after = wake_state(bed, bound, rooted);
    CHECKF(after.stacks == 0 && bed.world.effect_count(bound) == 1 && after.flags == 16,
           "the first player dies: his root is off in the same call (stacks %d) and the other's snare stays (%zu effects, flags %lld)", after.stacks, bed.world.effect_count(bound), static_cast<long long>(after.flags));
    bed.world.player_died(bed.other.actor_id());
    const WakeState none = wake_state(bed, bound, rooted);
    CHECKF(bed.world.effect_count(bound) == 0 && none.flags == 0 && none.scale == 1.0, "...and the second's death takes the snare: no effects, flags %lld, and the walker keeps %.2f of its speed", static_cast<long long>(none.flags), none.scale);
}
AOC_REGISTER_TEST(test_cc_sleep_wakes);

// ---- the lockout of a slow takes the slow off ----------------------------------------------------------------------------------------------------------------
// A chill ticks a build-up item onto its holder (+2 a second); at the build-up's limit of 24 the data's lockout hit takes the build-up and the chill off and puts the lockout on.  A hit's removal
// list is applied wherever the hit lands: this one is an effect's tick hit.
void test_cc_lockout_ends_slow(Fixture& f) {
    std::printf("the lockout of a slow: at 24 stacks of the build-up the chill is taken off the creature (a hit's removal list applied on the tick path)\n");
    dots::Bed bed(f);
    CHECKF(bed.ok(), "the bed: %s", bed.problem().c_str());
    if (!bed.ok()) return;
    const abilities::EffectDef* chilled = f.catalog->effect_named("Status_Chilled");
    const abilities::EffectDef* buildup = f.catalog->effect_named("DisableEvasion_Movement_T1_Buildup");
    const abilities::EffectDef* lockout = f.catalog->effect_named("DisableEvasion_Movement_T1_Lockout");
    const abilities::HitDef* lock_hit = f.catalog->hit_named("DisableEvasion_Movement_T1_Lockout");
    CHECK(chilled && buildup && lockout && lock_hit && f.catalog->hit_named("Mage_Frostbolt_Hit"), "the data has the chill, its build-up and lockout items, the lockout's hit and a Frostbolt's hit");
    if (!chilled || !buildup || !lockout || !lock_hit) return;
    CHECKF(lock_hit->remove_effects.size() == 3 && lock_hit->max_remove == 1000, "the lockout's hit names three effects to take off, up to 1000 (%zu, %d)", lock_hit->remove_effects.size(), lock_hit->max_remove);
    const uint64_t wolf = bed.wolf(0);
    CHECK(wolf != 0, "a Wolf");
    if (!wolf) return;
    const double t0 = 100.0;
    bed.land(wolf, "Mage_Frostbolt_Hit", t0, bed.player);
    CHECKF(wake_state(bed, wolf, chilled).stacks == 1 && wake_state(bed, wolf, chilled).flags == 16, "a Frostbolt chills it (flags %lld)", static_cast<long long>(wake_state(bed, wolf, chilled).flags));
    double locked_at = -1, buildup_peak = 0;
    WakeState at_lock, chill_at_lock, build_at_lock;
    for (int k = 1; k <= 600; ++k) {
        const double t = t0 + 0.05 * k;
        // the chill is kept up with a Frostbolt every 3 s until the lockout is on (the chill's time grows with each, up to its longest)
        if (locked_at < 0 && k % 60 == 0) bed.land(wolf, "Mage_Frostbolt_Hit", t, bed.player);
        bed.world.tick(t);
        buildup_peak = std::max(buildup_peak, static_cast<double>(bed.world.effect_stacks(wolf, buildup->row.guid)));
        if (locked_at < 0 && bed.world.effect_stacks(wolf, lockout->row.guid) > 0) {
            locked_at = t;
            at_lock = wake_state(bed, wolf, lockout);
            chill_at_lock = wake_state(bed, wolf, chilled);
            build_at_lock = wake_state(bed, wolf, buildup);
        }
    }
    std::printf("    the build-up peaked at %.0f stacks; the lockout went on at %.2f s (%.2f s into the chill)\n", buildup_peak, locked_at, locked_at - t0);
    // (the build-up's 24th stack is put on and taken off in the same step, so the most the end of a step shows is 22)
    CHECKF(locked_at > 0 && buildup_peak >= 20.0, "kept chilled, the build-up climbs two stacks a second to its limit of 24 (22 seen at the end of a step, %.0f) and the lockout goes on (at %.2f s)", buildup_peak, locked_at);
    if (locked_at > 0) {
        CHECKF(std::fabs((locked_at - t0) - 11.0) < 0.2 && buildup_peak == 22.0, "...the lockout goes on 11 s into the chill (%.2f s) with the build-up at 22 stacks (%.0f)", locked_at - t0, buildup_peak);
        CHECKF(chill_at_lock.stacks == 0 && build_at_lock.stacks == 0, "...the lockout's hit took the chill and the build-up off: %d stack(s) of the chill and %d of the build-up are left", chill_at_lock.stacks, build_at_lock.stacks);
        CHECKF(at_lock.flags == 0 && at_lock.scale == 1.0 && at_lock.stacks == 1,
               "...and the creature is free: crowd control flags %lld (0), its walker keeps %.2f of its speed (1), the lockout on it (%d)", static_cast<long long>(at_lock.flags), at_lock.scale, at_lock.stacks);
    }
}
AOC_REGISTER_TEST(test_cc_lockout_ends_slow);

// ---- the lockout of a root, a freeze, a stun, a trip and a sleep takes them off ---------------------------------------------------------------------------------
// Each of them ticks a build-up item onto its holder (the root and the freeze every half second, up to 16 stacks; the stun and the trip every half second, up to 10; the sleep every second, up to 15) and at
// the limit the data's lockout hit takes the effect off however long it was put on for.  The first tick comes as the effect lands, so the root and the freeze come off at 7.5 s, the stun and the trip at
// 4.5 s and the sleep at 14 s.
void test_cc_lockout_ends_holds(Fixture& f) {
    std::printf("the lockout of a hold: a root or a freeze put on for 30 s is taken off at 7.5 s, a stun or a trip at 4.5 s, a sleep at 14 s; the creature is free and the lockout is on it\n");
    dots::Bed bed(f);
    CHECKF(bed.ok(), "the bed: %s", bed.problem().c_str());
    if (!bed.ok()) return;
    struct Hold {
        const char* effect;
        const char* lockout;   // the effect the lockout's hit puts on
        double      off_at;    // seconds after the hold went on
    };
    const Hold holds[] = {{"Status_Rooted", "DisableEvasion_Movement_T2_Lockout", 7.5}, {"Status_Frozen", "DisableEvasion_Movement_T2_Lockout", 7.5}, {"Status_Stunned", "DisableEvasion_Hard_T2_Lockout", 4.5},
                          {"Status_Tripped", "DisableEvasion_Hard_T2_Lockout", 4.5},   {"Status_Incapacitated", "DisableEvasion_Hard_T1_Lockout", 14.0}};
    for (const Hold& h : holds) {
        const abilities::EffectDef* hold = f.catalog->effect_named(h.effect);
        const abilities::EffectDef* lockout = f.catalog->effect_named(h.lockout);
        CHECKF(hold && lockout, "the data has %s and %s", h.effect, h.lockout);
        if (!hold || !lockout) continue;
        const uint64_t wolf = bed.wolf(0);
        CHECKF(wolf != 0, "a Wolf for the %s", h.effect);
        if (!wolf) continue;
        const double t0 = 100.0;
        wake_put(bed, wolf, hold, 30.0, t0);
        CHECKF(wake_state(bed, wolf, hold).stacks == 1 && wake_state(bed, wolf, hold).flags != 0, "%s: it is held for 30 s (crowd control flags %lld)", h.effect, static_cast<long long>(wake_state(bed, wolf, hold).flags));
        double off_at = -1;
        WakeState before, at_off;
        for (int k = 1; k <= 400; ++k) {
            const double t = t0 + 0.05 * k;
            bed.world.tick(t);
            if (std::fabs((t - t0) - (h.off_at - 0.3)) < 0.025) before = wake_state(bed, wolf, hold);
            if (off_at < 0 && bed.world.effect_stacks(wolf, hold->row.guid) == 0) {
                off_at = t;
                at_off = wake_state(bed, wolf, lockout);
            }
        }
        std::printf("    %s came off at %.2f s\n", h.effect, off_at - t0);
        CHECKF(before.stacks == 1 && before.flags != 0, "%s: still held 0.3 s before its lockout (stacks %d, flags %lld)", h.effect, before.stacks, static_cast<long long>(before.flags));
        CHECKF(off_at > t0 + h.off_at - 0.1 && off_at < t0 + h.off_at + 0.1, "%s: the lockout takes it off at %.1f s, not at the 30 s of its own time (%.2f s)", h.effect, h.off_at, off_at - t0);
        CHECKF(at_off.flags == 0 && at_off.scale == 1.0 && at_off.stacks >= 1, "...and the creature is free: flags %lld (0), its walker keeps %.2f of its speed (1), the lockout on it (%d)", static_cast<long long>(at_off.flags),
               at_off.scale, at_off.stacks);
    }
}
AOC_REGISTER_TEST(test_cc_lockout_ends_holds);

// ---- Shatter: a Mage's lightning on a Frozen creature ---------------------------------------------------------------------------------------------------------
// The Mage's Shatter passive carries a reaction cast by the instigator of its carrier (the Mage himself); when his lightning takes Health from a creature and the creature is Frozen, the reaction's
// hit lands on the creature.  The reaction's requirement asks about the creature, not about the Mage.
struct ShatterCast {
    bool   struck = false;     // the Lightning Strike's own hit landed on the Wolf without a miss
    int    shattered = 0;      // the confirmations of the reaction's hit
    double lost = 0;           // the Health the Wolf lost
    size_t reactions = 0;
};

static ShatterCast shatter_cast(Fixture& f, uint64_t seed, bool talent, bool frozen) {
    ShatterCast out;
    Forge g(f, 8, talent ? std::vector<std::string>{"Mage_Passive_Shatter"} : std::vector<std::string>{}, seed);
    if (!g.ok()) return out;
    const uint64_t wolf = g.wolf(600, 0);
    const abilities::AbilityDef* def = f.catalog->ability_named("Mage_LightningStrike");
    const abilities::HitDef* hit = f.catalog->hit_named("Mage_LightningStrike_Hit");
    const abilities::HitDef* shatter = f.catalog->hit_named("Mage_Status_Frozen->Shattered");
    const abilities::EffectDef* freeze = f.catalog->effect_named("Status_Frozen");
    if (!wolf || !def || !hit || !shatter || !freeze) return out;
    g.rig.know({"Mage_LightningStrike"});
    constexpr double t0 = 1.0;
    constexpr int32_t id = 41;
    constexpr int64_t kCastComplete = 6064628894971461633ll;   // the branch of the cast bar (Mage_Lightning_CastComplete)
    if (frozen) {
        effects::Applied a;
        a.def = freeze;
        a.duration = 30.0;
        g.world.apply_effects(wolf, 77, {a}, t0);
    }
    int64_t targeting = content::kTargetingDefaultSelf;
    for (const content::AbilityHitRow& h : def->hits) {
        if (h.hit_guid != hit->row.guid || h.source != "hit") continue;
        for (const content::TargetingRow& tg : def->targetings) {
            if (tg.key == h.key) targeting = tg.guid;
        }
    }
    TriggerInfo info;
    info.target_count = 1;
    info.target_actors = {wolf};
    if (g.rig.pipe->trigger(def->row.guid, id, t0, info) != Refusal::None) return out;
    g.rig.run_to(t0 + 0.171);
    g.rig.pipe->branch_request(id, kCastComplete, t0 + 0.171);   // (a tap: the quick cast the recordings show, asked for 171 ms after the trigger)
    const double ask = t0 + 1.2;
    g.rig.run_to(ask);
    g.rig.pipe->hit_request(def->row.guid, targeting, hit->row.guid, 1, 0, ask, {wolf});
    g.rig.run_to(ask + 1.0);
    for (const Seen& s : g.rig.log) {
        for (const bootstrap::HitConfirm& c : s.confirmations) {
            if (c.record_guid == hit->row.guid && !c.targets.empty() && (c.targets[0].flags & stats::kHitMiss) == 0) out.struck = true;
            if (c.record_guid == shatter->row.guid && c.source_type == content::kSourceTypeReaction && !c.targets.empty()) ++out.shattered;
        }
    }
    out.lost = g.lost({wolf});
    out.reactions = g.rig.pipe->reactions_fired();
    return out;
}

void test_shatter_reaction(Fixture& f) {
    std::printf("Shatter: the Mage's lightning on a Frozen creature fires the reaction of his passive (cast by its instigator), and on none else\n");
    const abilities::HitDef* shatter = f.catalog->hit_named("Mage_Status_Frozen->Shattered");
    const content::ReactionRow* reaction = nullptr;
    const abilities::EffectDef* carrier = f.catalog->effect_named("Mage_Passive_Shatter");
    if (carrier && !carrier->reactions.empty()) {
        if (const abilities::ReactionDef* r = f.catalog->reaction(carrier->reactions.front())) reaction = &r->row;
    }
    CHECK(shatter && reaction && reaction->name == "Mage_Shatter" && reaction->instigator == "Instigator" && (reaction->targets_mask & 8) != 0, "the data: Mage_Shatter is carried by the passive, cast by the Instigator, and lands on the hit's targets");
    if (!shatter || !reaction) return;
    // (a seed whose Strike does not miss; the same seed for the four casts of the comparison)
    bool done = false;
    for (uint64_t seed = 1; seed <= 12 && !done; ++seed) {
        const ShatterCast on = shatter_cast(f, seed, true, true);
        if (!on.struck) continue;
        const ShatterCast not_frozen = shatter_cast(f, seed, true, false), no_talent = shatter_cast(f, seed, false, true), neither = shatter_cast(f, seed, false, false);
        if (!not_frozen.struck || !no_talent.struck || !neither.struck) continue;
        done = true;
        std::printf("    seed %llu: shattered %d (lost %.1f); not frozen %d (lost %.1f); no talent %d (lost %.1f); neither %d (lost %.1f)\n", static_cast<unsigned long long>(seed), on.shattered, on.lost, not_frozen.shattered,
                    not_frozen.lost, no_talent.shattered, no_talent.lost, neither.shattered, neither.lost);
        CHECKF(on.shattered >= 1, "a Lightning Strike on a Frozen Wolf, with the passive: the reaction's hit lands on it (%d confirmation(s), %zu reaction(s) fired)", on.shattered, on.reactions);
        CHECKF(on.lost > not_frozen.lost + 1.0, "...and it costs the Wolf more Health than the same Strike on one that is not Frozen (%.1f against %.1f)", on.lost, not_frozen.lost);
        CHECKF(not_frozen.shattered == 0 && no_talent.shattered == 0 && neither.shattered == 0, "...but not on a Wolf that is not Frozen (%d), nor without the passive (%d), nor with neither (%d)", not_frozen.shattered,
               no_talent.shattered, neither.shattered);
    }
    CHECK(done, "a seed was found in which the Strike lands in all four casts");
}
AOC_REGISTER_TEST(test_shatter_reaction);

// ---- the Lows: a record that is no stun, a facing on hold, a stun in a leap ------------------------------------------------------------------------------------
void test_cc_wake_lows(Fixture& f) {
    std::printf("a record with the abilities bit and no stun's other bits is refused; a facing that goes on hold forgets its turn; a stun stops the leap the Wolf is in\n");
    dots::Bed bed(f);
    CHECKF(bed.ok(), "the bed: %s", bed.problem().c_str());
    if (!bed.ok()) return;
    // 1: the abilities bit alone, or with a snare, is no stun (a disarm with a slow): the creature does not take it; a snare, a root and a full stun are taken
    std::vector<std::unique_ptr<abilities::EffectDef>> made;   // (the effects the Wolves hold point at these: they outlive the checks)
    const auto take = [&](int64_t status, int64_t guid) {
        made.push_back(std::make_unique<abilities::EffectDef>());
        abilities::EffectDef& def = *made.back();
        def.row.guid = guid;
        def.row.name = "test_cc_status_" + std::to_string(status);
        def.row.crowd_control_guid = guid;
        content::CrowdControlRow cc;
        cc.guid = guid;
        cc.name = def.row.name;
        cc.applied_status = status;
        cc.snare_strength = (status & 16) ? 0.5 : 0.0;
        cc.animation_request = "None";
        def.cc = cc;
        const uint64_t wolf = bed.wolf(static_cast<double>(status) * 10.0);
        effects::Applied a;
        a.def = &def;
        a.duration = 5.0;
        bed.world.apply_effects(wolf, bed.player.actor_id(), {a}, 100.0);
        return bed.world.effect_count(wolf);
    };
    CHECKF(take(20, 9100001) == 0 && take(5, 9100002) == 0, "a record of the abilities bit and a snare (20), or of the abilities bit and a root (5), is no stun and is not put on the Wolf");
    CHECKF(take(16, 9100003) == 1 && take(1, 9100004) == 1 && take(7, 9100005) == 1, "a snare (16), a root (1) and a full stun (7) are");

    // 2: a facing on hold forgets the turn it was on
    combat_rig::World w;
    CHECK(w.problem.empty(), "a world for the turn");
    if (w.problem.empty()) {
        const uint64_t wolf = w.spawn(wolf_archetype(f), 0, 0, 7, 0);
        CHECK(wolf != 0, "a Wolf that stands");
        if (wolf) {
            double now = 0;
            const auto run = [&](int ticks) {
                for (int i = 0; i < ticks; ++i) {
                    now += 1.0 / 15.0;
                    w.npcs->note_player(77, 0, 500, now);   // (a player stands near: the Wolf is awake)
                    w.npcs->tick(now);
                }
            };
            run(3);
            const double start_yaw = w.registry.find(wolf)->yaw_degrees;
            CHECK(w.npcs->turn_toward(wolf, 0.0, -1000.0, 90.0), "it is told to turn towards a place off to its side, at 90 degrees a second");
            run(3);
            const double turned = wrap180(w.registry.find(wolf)->yaw_degrees - start_yaw);
            CHECKF(std::fabs(turned) > 5.0 && std::fabs(turned) < 60.0, "it begins to turn (%.1f deg in 0.2 s)", turned);
            CHECK(w.npcs->set_locks(wolf, false, true), "its facing goes on hold (a freeze, a stun)");
            const double held_yaw = w.registry.find(wolf)->yaw_degrees;
            run(30);
            const double still_yaw = w.registry.find(wolf)->yaw_degrees;
            CHECK(w.npcs->set_locks(wolf, false, false), "the hold is lifted");
            run(45);
            const double later_yaw = w.registry.find(wolf)->yaw_degrees;
            CHECKF(std::fabs(wrap180(still_yaw - held_yaw)) < 1.0, "it did not turn while the hold was on (%.1f deg in 2 s)", wrap180(still_yaw - held_yaw));
            CHECKF(std::fabs(wrap180(later_yaw - held_yaw)) < 1.0, "...and does not swing round to the turn it was on when the hold is lifted (%.1f deg in 3 s)", wrap180(later_yaw - held_yaw));
        }
    }

    // 3: a stun in a leap: the leap the Wolf is in stops with the stun (a root and a freeze leave the leap alone)
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string wolf_path = creature_path(f, "/Beast/Canine/Wolf.Default__Wolf_C");
    const abilities::HitDef* condemn = f.catalog->hit_named("Cleric_Condemn_Hit");
    const abilities::EffectDef* asleep = f.catalog->effect_named("Status_Incapacitated");
    CHECK(!wolf_path.empty() && condemn && asleep, "the Wolf, the Condemn's hit and the sleep are in the data");
    if (wolf_path.empty() || !condemn || !asleep) return;
    CcRun ctl, run;
    run.watch = asleep;
    const bool ran = cc_fight(f, tables, wolf_path, 8.0, 0, 800, 0.2, cc_player_steer(800, nullptr, 0), {}, ctl) &&
                     cc_fight(f, tables, wolf_path, 8.0, 0, 800, 0.2, cc_player_steer(800, nullptr, 0),
                              {CcEvent{0.0, cc_put(run, condemn), false, [&run](LungeStepContext&) { return !run.s.empty() && run.s.back().active == 1; }}}, run);
    CHECK(ran, "two fights against a player 8 m away run, one free and one in which the sleep goes on the tick after the Wolf's leap begins");
    if (ran) {
        const CcLunge a = cc_first_lunge(ctl.s), b = cc_first_lunge(run.s);
        const size_t on_i = cc_at(run.s, run.on_at);
        std::printf("    stun in a leap: free leap %.0f cm over %zu ticks; stunned at %.2f s (the leap ran from %.2f s): %.0f cm over %zu ticks\n", a.travelled, a.found ? a.last - a.first + 1 : 0, run.on_at,
                    b.found ? run.s[b.first].t : -1.0, b.travelled, b.found ? b.last - b.first + 1 : 0);
        CHECK(a.found, "the free Wolf leaps at the player");
        if (a.found && on_i >= 1 && on_i < run.s.size()) {
            const double after_stun = std::hypot(run.s.back().wx - run.s[on_i].wx, run.s.back().wy - run.s[on_i].wy);
            CHECKF(run.on_at > 0 && run.s[on_i].active != 1 && run.s[on_i].montage == 0, "the leap stops in the tick of the stun (carried %d, montage %llu)", run.s[on_i].active, static_cast<unsigned long long>(run.s[on_i].montage));
            CHECKF(b.travelled < 0.6 * a.travelled && after_stun < 5.0, "...it covers %.0f cm of the leap's %.0f cm and then stands (%.1f cm moved in the rest of the fight)", b.travelled, a.travelled, after_stun);
        }
    }
}
AOC_REGISTER_TEST(test_cc_wake_lows);
