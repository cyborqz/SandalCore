// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_dots_tests.inc: the checks of the damage over time and the stat debuffs that land on creatures (included by combat_registered.inc inside
//  test_worldd_combat's anonymous namespace)
// ============================================================================

// ---- a test bed: two players' ports, a world of Wolves, and the way a real hit lands its effects on one -------------------------------------------------------------
// The hit's effects are worked out and put on the creature as the ability pipeline does it: the hit's own stat mods first (a probe can leave the damage out), then the effects it lists,
// each worked out against the creature as it is then (a condition reads what the creature holds).

namespace dots {

/// What the creature's book holds, for the reads of it in a hit's expressions (GetEffectRemainingDuration).
class BookEffects final : public combat::TargetEffects {
public:
    BookEffects(const combat::CombatWorld& w, uint64_t wolf, double now) : w_(w), wolf_(wolf), now_(now) {}
    double remaining(int64_t effect_guid) const override { return w_.effect_remaining(wolf_, effect_guid, now_); }

private:
    const combat::CombatWorld& w_;
    uint64_t                   wolf_;
    double                     now_;
};

struct Landing {
    bool                          ok = false;      // the hit and its effects were worked out
    std::vector<effects::Applied> applied;         // the effects it put on, as worked out
    bool has(const char* name) const {
        for (const effects::Applied& a : applied)
            if (a.def->row.name == name) return true;
        return false;
    }
    const effects::Applied* find(const char* name) const {
        for (const effects::Applied& a : applied)
            if (a.def->row.name == name) return &a;
        return nullptr;
    }
};

struct Tick {
    double   at = 0, value = 0;
    uint8_t  flags = 0;
    bool     predicted = false;
    int64_t  effect = 0;
    uint64_t by = 0;
    bool plain() const { return (flags & (stats::kHitCrit | stats::kHitMiss)) == 0; }
    bool missed() const { return (flags & stats::kHitMiss) != 0; }
};

struct Bed {
    Fixture&               f;
    Rig                    rig;
    combat_rig::World      w;
    combat::NpcStatBook    book;
    combat::CombatWorld    world;
    combat_rig::FakePlayer player, other;
    std::string            wolf_path;

    explicit Bed(Fixture& fixture)
        : f(fixture), rig(8, 25, {}), book(*f.ctx->bundle, *f.ctx->gear), world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion),
          player(rig, 77, {0, 0, 1098}), other(rig, 78, {0, 100, 1098}) {
        if (!rig.ok() || !w.problem.empty()) return;
        world.add_player(&player);
        world.add_player(&other);
        wolf_path = wolf_archetype(f);
    }
    bool ok() const { return rig.ok() && w.problem.empty(); }
    std::string problem() const { return !rig.ok() ? rig.problem : w.problem; }

    /// A Wolf of level 45 (it lives through the probes) 30 m from the players.
    uint64_t wolf(double y) { return w.spawn(wolf_path, 3000, y, 45, 100); }

    /// The hit `name` as the pipeline lands it on `wolf` at `t`, from `by`: its stat mods when `damage`, then its effects.
    Landing land(uint64_t wolf, const char* name, double t, combat_rig::FakePlayer& by, bool damage = false) {
        Landing out;
        const abilities::HitDef* hit = f.catalog->hit_named(name);
        combat::Fighter* target = world.fighter_of(wolf);
        if (!hit || !target) return out;
        combat::HitMeta meta;
        meta.source_type = content::kSourceTypeAbility;
        std::string why;
        if (damage) {
            combat::Resolved r;
            if (!world.resolver().resolve(by.fighter(), *target, *hit, meta, r, why)) return out;
            world.apply_hit_to_npc(wolf, by.actor_id(), r, t);
        }
        const BookEffects view(world, wolf, t);
        meta.target_effects = &view;
        combat::Resolved gate;
        out.ok = world.resolver().resolve_effects(by.fighter(), *target, *hit, meta, gate, why);
        out.applied = gate.effects;
        world.apply_effects(wolf, by.actor_id(), gate.effects, t);
        return out;
    }

    void run(double from, double to) {
        for (double t = from; t <= to + 1e-9; t += 0.05) world.tick(t);
    }

    /// The ticks of the hit `hit_name` on `wolf` the players were told of since the last call, by time.
    std::vector<Tick> ticks(uint64_t wolf, const char* hit_name) {
        std::vector<Tick> out;
        const abilities::HitDef* hit = f.catalog->hit_named(hit_name);
        for (combat_rig::FakePlayer* p : {&player, &other}) {
            for (const combat::EffectHitNote& n : p->effect_notes) {
                if (!hit || n.holder != wolf || n.hit_guid != hit->row.guid) continue;
                out.push_back({n.at, n.mods.empty() ? 0.0 : n.mods[0].value, n.flags, n.predicted, n.effect_guid, p->actor_id()});
            }
            p->effect_notes.clear();
        }
        std::sort(out.begin(), out.end(), [](const Tick& a, const Tick& b) { return a.at < b.at; });
        return out;
    }

    /// The effect items the Wolf shows, by effect name.
    std::vector<std::pair<std::string, combat::EffectItemWrite>> items(uint64_t wolf) const {
        std::vector<std::pair<std::string, combat::EffectItemWrite>> out;
        if (const world::WorldActor* a = w.registry.find(wolf)) {
            for (const combat::EffectItemWrite& it : combat::read_effect_items(*a)) {
                const abilities::EffectDef* d = f.catalog->effect(it.item.effect_guid);
                out.emplace_back(d ? d->row.name : std::string("?"), it);
            }
        }
        return out;
    }
    size_t items_named(uint64_t wolf, const char* name) const {
        size_t n = 0;
        for (const auto& [nm, it] : items(wolf)) n += nm == name ? 1 : 0;
        return n;
    }
    int64_t effect_guid(const char* name) const {
        const abilities::EffectDef* d = f.catalog->effect_named(name);
        return d ? d->row.guid : 0;
    }
    /// A stat of the Wolf as its stats run now (its effects included).
    double stat_of(uint64_t wolf, int64_t stat) {
        combat::Fighter* g = world.fighter_of(wolf);
        std::string err;
        auto ev = g ? g->stats().evaluation(g->worn(), err, true) : nullptr;
        return ev ? ev->stat(stat) : -1.0e18;
    }
    /// The Health the first plain (no critical hit, no miss) roll of `hit` between the two takes or gives; 0 when none came.
    double plain_damage(combat::Fighter& from, combat::Fighter& to, const char* hit_name) {
        const abilities::HitDef* hit = f.catalog->hit_named(hit_name);
        if (!hit) return 0;
        combat::HitMeta meta;
        meta.source_type = content::kSourceTypeAbility;
        for (int tries = 0; tries < 200; ++tries) {
            combat::Resolved r;
            std::string why;
            if (!world.resolver().resolve(from, to, *hit, meta, r, why) || r.flags != 0) continue;
            for (const combat::ResolvedMod& m : r.mods)
                if (m.stat_guid == content::kGuidHealth) return m.value;
        }
        return 0;
    }
};

}  // namespace dots

// ---- bleeding ------------------------------------------------------------------------------------------------------------------------------------------------------
// A bleed ticks every second for 6 s, the first tick a second after it lands (retail's Hemorrhage and Conflagration, whose last tick would fall on the very moment they end, never tick then).
// One stack in place per caster: the same caster's second bleed refreshes it, another caster's makes a bleed of its own.
void test_creature_bleed(Fixture& f) {
    std::printf("a creature bleeds: a tick a second for 6 s, one stack per caster, two casters' bleeds tick apart\n");
    dots::Bed bed(f);
    CHECKF(bed.ok(), "the bed: %s", bed.problem().c_str());
    if (!bed.ok()) return;
    const abilities::EffectDef* bleeding = f.catalog->effect_named("Status_Bleeding");
    const abilities::HitDef* tick_hit = f.catalog->hit_named("Status_Bleeding_Tick");
    CHECK(bleeding && tick_hit && f.catalog->hit_named("Gryphon_Rake_Hit"), "the bleed, its tick and a hit that applies it are in the data");
    if (!bleeding || !tick_hit) return;
    const int64_t bleed = bleeding->row.guid;
    const double t0 = 100.0;

    // one bleed
    const uint64_t wolf = bed.wolf(0);
    CHECK(wolf != 0, "a Wolf 30 m away");
    if (!wolf) return;
    const dots::Landing l = bed.land(wolf, "Gryphon_Rake_Hit", t0, bed.player);
    CHECK(l.ok && l.has("Status_Bleeding"), "the Rake's hit puts a bleed on the Wolf");
    CHECKF(bed.world.effect_count(wolf) >= 1 && bed.world.effect_stacks(wolf, bleed) == 1 && bed.items_named(wolf, "Status_Bleeding") == 1, "...one stack of it, shown as one item (%d stacks)",
           bed.world.effect_stacks(wolf, bleed));
    const auto item = [&](uint64_t id, const char* name) -> std::optional<combat::EffectItemWrite> {
        for (const auto& [n, it] : bed.items(id))
            if (n == name) return it;
        return std::nullopt;
    };
    const auto first = item(wolf, "Status_Bleeding");
    CHECKF(first && near(first->item.duration, 6.0, 1e-4) && near(first->item.tick_time, 1.0, 1e-4) && first->item.stacks == 1, "...for 6 s, ticking every second (%.2f s, %.2f)",
           first ? static_cast<double>(first->item.duration) : -1.0, first ? static_cast<double>(first->item.tick_time) : -1.0);
    bed.run(t0 + 0.05, t0 + 7.0);
    const std::vector<dots::Tick> ticks = bed.ticks(wolf, "Status_Bleeding_Tick");
    CHECKF(ticks.size() == 5, "it ticks 5 times in its 6 s, at 1 s to 5 s: the tick that would fall on its last moment does not happen (%zu ticks)", ticks.size());
    double worst = 0;
    for (size_t i = 0; i < ticks.size(); ++i) worst = std::max(worst, std::fabs(ticks[i].at - (t0 + 1.0 + static_cast<double>(i))));
    CHECKF(worst <= 0.05, "...each on its second (the worst is off by %.4f s)", worst);
    bool predicted = !ticks.empty(), takes = !ticks.empty();
    for (const dots::Tick& t : ticks) {
        predicted = predicted && t.predicted && t.effect == bleed;
        takes = takes && (t.missed() || t.value < 0);
    }
    CHECK(predicted && takes, "...every tick is the bleed's, takes Health (unless it missed) and is one the clients predicted");
    bool same = true;
    double plain_value = 0;
    for (const dots::Tick& t : ticks) {
        if (!t.plain()) continue;
        if (plain_value == 0) plain_value = t.value;
        same = same && near(t.value, plain_value, std::fabs(plain_value) * 0.01);
    }
    CHECKF(same && plain_value < 0, "...the ticks without a critical hit are all the same size (%.2f)", plain_value);
    CHECK(bed.world.effect_count(wolf) == 0 && bed.items(wolf).empty(), "when the 6 s are over the bleed is gone from the Wolf and its list");

    // the same caster again: still one stack, refreshed
    const uint64_t wolf2 = bed.wolf(300);
    bed.land(wolf2, "Gryphon_Rake_Hit", t0, bed.player);
    bed.run(t0 + 0.05, t0 + 2.5);
    bed.land(wolf2, "Gryphon_Rake_Hit", t0 + 2.5, bed.player);
    CHECKF(bed.world.effect_stacks(wolf2, bleed) == 1 && bed.items_named(wolf2, "Status_Bleeding") == 1 && near(bed.world.effect_remaining(wolf2, bleed, t0 + 2.5), 6.0, 1e-4),
           "the same caster's second bleed adds no stack and runs the 6 s again from then (%d stacks, %.2f s left)", bed.world.effect_stacks(wolf2, bleed),
           bed.world.effect_remaining(wolf2, bleed, t0 + 2.5));
    bed.run(t0 + 2.55, t0 + 9.5);
    const std::vector<dots::Tick> again = bed.ticks(wolf2, "Status_Bleeding_Tick");
    CHECKF(again.size() == 8 && bed.world.effect_count(wolf2) == 0, "...so it ticked 8 times, up to 8 s, and then ended at 8.5 s (%zu ticks)", again.size());

    // another caster: a bleed of its own
    const uint64_t wolf3 = bed.wolf(600);
    bed.land(wolf3, "Gryphon_Rake_Hit", t0, bed.player);
    bed.run(t0 + 0.05, t0 + 2.5);
    bed.land(wolf3, "Gryphon_Rake_Hit", t0 + 2.5, bed.other);
    CHECKF(bed.items_named(wolf3, "Status_Bleeding") == 2 && bed.world.effect_stacks(wolf3, bleed) == 2, "a second caster's bleed is a second instance, the first one left as it was (%zu items)", bed.items_named(wolf3, "Status_Bleeding"));
    bed.run(t0 + 2.55, t0 + 9.0);
    const std::vector<dots::Tick> both = bed.ticks(wolf3, "Status_Bleeding_Tick");
    size_t by_first = 0, by_second = 0;
    for (const dots::Tick& t : both) {
        by_first += t.by == bed.player.actor_id() ? 1 : 0;
        by_second += t.by == bed.other.actor_id() ? 1 : 0;
    }
    CHECKF(by_first == 5 && by_second == 5, "...each caster is told of his own bleed's ticks: 5 and 5 (%zu, %zu)", by_first, by_second);
    int overlapping = 0;
    for (const dots::Tick& t : both) overlapping += (t.at > t0 + 3.4 && t.at < t0 + 5.6) ? 1 : 0;
    CHECKF(overlapping == 5, "...two ticks a second where they overlap (%d ticks between 3.5 s and 5.5 s)", overlapping);

    // a caster who leaves takes his bleed with him; the other caster's goes on
    const uint64_t wolf4 = bed.wolf(900);
    bed.land(wolf4, "Gryphon_Rake_Hit", t0, bed.player);
    bed.land(wolf4, "Gryphon_Rake_Hit", t0, bed.other);
    CHECK(bed.items_named(wolf4, "Status_Bleeding") == 2, "(two casters bleed a fourth Wolf)");
    bed.world.remove_player(&bed.other);
    CHECKF(bed.items_named(wolf4, "Status_Bleeding") == 1 && bed.world.effect_stacks(wolf4, bleed) == 1, "a caster who leaves takes his bleed with him and the other caster's goes on (%zu left)",
           bed.items_named(wolf4, "Status_Bleeding"));
    bed.world.add_player(&bed.other);
}
AOC_REGISTER_TEST(test_creature_bleed);

// ---- hemorrhage, and the conditions of effects ---------------------------------------------------------------------------------------------------------------
// A hemorrhage ticks every half second for 5 s, the first tick as it lands; each application is a stack of its own and none refreshes another.  It lands only on a bleeding creature
// (the data writes that as a stack count of 0: Ranger_Snipe_V2, or a row requirement: Rogue_Lacerate).
void test_creature_hemorrhage(Fixture& f) {
    std::printf("a creature takes a hemorrhage: a tick every 0.5 s for 5 s, separate stacks that never refresh, and only while it bleeds\n");
    dots::Bed bed(f);
    CHECKF(bed.ok(), "the bed: %s", bed.problem().c_str());
    if (!bed.ok()) return;
    const abilities::EffectDef* hemorrhage = f.catalog->effect_named("Status_Hemorrhage");
    CHECK(hemorrhage && f.catalog->hit_named("Snipe_Hemorrhage") && f.catalog->hit_named("Ranger_Snipe_V2") && f.catalog->hit_named("Rogue_Lacerate") && f.catalog->hit_named("Gryphon_Rake_Hit"),
          "the hemorrhage, a hit that applies it unconditionally and the two that need a bleeding creature are in the data");
    if (!hemorrhage) return;
    const int64_t hem = hemorrhage->row.guid;
    const double t0 = 100.0;

    // the stacks
    const uint64_t wolf = bed.wolf(0);
    CHECK(wolf != 0, "a Wolf 30 m away");
    if (!wolf) return;
    const dots::Landing first = bed.land(wolf, "Snipe_Hemorrhage", t0, bed.player);   // (the hit's own requirement is the pipeline's: the effect is what is under test)
    CHECK(first.ok && first.has("Status_Hemorrhage"), "a hemorrhage lands");
    bed.run(t0 + 0.05, t0 + 1.0);
    bed.land(wolf, "Snipe_Hemorrhage", t0 + 1.0, bed.player);
    CHECKF(bed.world.effect_count(wolf) == 2 && bed.items_named(wolf, "Status_Hemorrhage") == 2, "a second one a second later is a second instance: each application is a stack of its own (%zu items)", bed.items_named(wolf, "Status_Hemorrhage"));
    const auto items = bed.items(wolf);
    bool durations = items.size() == 2;
    for (const auto& [n, it] : items) durations = durations && near(it.item.duration, 5.0, 1e-4) && near(it.item.tick_time, 0.5, 1e-4) && it.item.stacks == 1;
    CHECK(durations, "...each lasts 5 s and ticks every half second, the first one not lengthened by the second");
    bed.run(t0 + 1.05, t0 + 7.0);
    const std::vector<dots::Tick> ticks = bed.ticks(wolf, "Status_Hemorrhage_Tick");
    CHECKF(ticks.size() == 20, "10 ticks of each, 20 in all: the first at the moment it lands, the last half a second before its end (%zu)", ticks.size());
    size_t at_first = 0, at_second = 0;
    for (const dots::Tick& t : ticks) {
        at_first += (t.at >= t0 - 0.01 && t.at <= t0 + 4.51) ? 1 : 0;
        at_second += (t.at >= t0 + 0.99 && t.at <= t0 + 5.51) ? 1 : 0;
    }
    CHECKF(at_first >= 10 && at_second >= 10 && ticks.front().at < t0 + 0.06 && ticks.back().at > t0 + 5.4 && ticks.back().at < t0 + 5.6,
           "...ticking from 0 s to 4.5 s and from 1 s to 5.5 s (first %.3f s, last %.3f s)", ticks.empty() ? 0.0 : ticks.front().at - t0, ticks.empty() ? 0.0 : ticks.back().at - t0);
    bool predicted = true;
    for (const dots::Tick& t : ticks) predicted = predicted && t.predicted && t.effect == hem && (t.missed() || t.value < 0);
    CHECK(predicted, "...each takes Health (unless it missed) and is a tick both clients predicted");
    CHECK(bed.world.effect_count(wolf) == 0, "both are gone after their 5 s");

    // the condition: a creature that does not bleed takes none
    const uint64_t clean = bed.wolf(300);
    CHECK(!bed.land(clean, "Ranger_Snipe_V2", t0, bed.player).has("Status_Hemorrhage"), "a Snipe on a creature that does not bleed puts no hemorrhage on it (the stack count works out to 0)");
    CHECK(!bed.land(clean, "Rogue_Lacerate", t0, bed.player).has("Status_Hemorrhage"), "...nor does a Lacerate (the row requires a bleeding target)");
    CHECK(bed.land(clean, "Gryphon_Rake_Hit", t0 + 0.1, bed.player).has("Status_Bleeding") && bed.world.effect_stacks(clean, bed.effect_guid("Status_Bleeding")) == 1, "a bleed lands on it");
    CHECK(bed.land(clean, "Ranger_Snipe_V2", t0 + 0.2, bed.player).has("Status_Hemorrhage"), "now a Snipe puts a hemorrhage on it");
    const uint64_t bleeding = bed.wolf(600);
    bed.land(bleeding, "Gryphon_Rake_Hit", t0, bed.player);
    CHECK(bed.land(bleeding, "Rogue_Lacerate", t0 + 0.1, bed.player).has("Status_Hemorrhage"), "...and so does a Lacerate on a bleeding creature (the tag Debuff.Bleeding is on it)");
}
AOC_REGISTER_TEST(test_creature_hemorrhage);

// ---- poison --------------------------------------------------------------------------------------------------------------------------------------------------
// A poison ticks every 2 s for 20 s; its stacks grow in place up to ten and make each tick bigger; another caster's poison is a poison of its own.
void test_creature_poison(Fixture& f) {
    std::printf("a creature is poisoned: a tick every 2 s for 20 s, stacks in place up to ten that grow the tick, another caster's poison apart\n");
    dots::Bed bed(f);
    CHECKF(bed.ok(), "the bed: %s", bed.problem().c_str());
    if (!bed.ok()) return;
    const abilities::EffectDef* poison = f.catalog->effect_named("Status_Poisoned");
    CHECK(poison && f.catalog->hit_named("Status_Poisoned_Tick") && f.catalog->hit_named("Scorpion_TailSting_Hit"), "the poison, its tick and a hit that applies it are in the data");
    if (!poison) return;
    const int64_t pg = poison->row.guid;
    const double t0 = 100.0;

    // one stack
    const uint64_t wolf1 = bed.wolf(0);
    CHECK(wolf1 != 0, "a Wolf 30 m away");
    if (!wolf1) return;
    CHECK(bed.land(wolf1, "Scorpion_TailSting_Hit", t0, bed.player).has("Status_Poisoned"), "a sting poisons the Wolf");
    const auto one = bed.items(wolf1);
    CHECKF(one.size() == 1 && near(one[0].second.item.duration, 20.0, 1e-4) && near(one[0].second.item.tick_time, 2.0, 1e-4) && one[0].second.item.stacks == 1, "...for 20 s, a tick every 2 s, one stack");
    bed.run(t0 + 0.05, t0 + 21.0);
    const std::vector<dots::Tick> t1 = bed.ticks(wolf1, "Status_Poisoned_Tick");
    CHECKF(t1.size() == 9, "9 ticks at 2 s, 4 s ... 18 s: the one that would fall on its last moment does not happen (%zu)", t1.size());
    double v1 = 0;
    for (const dots::Tick& t : t1)
        if (t.plain() && v1 == 0) v1 = t.value;
    bool predicted = !t1.empty();
    for (const dots::Tick& t : t1) predicted = predicted && t.predicted && t.effect == pg && (t.missed() || t.value < 0);
    CHECKF(predicted && v1 < 0, "...each takes Health (unless it missed) and is predicted by both clients (a tick of one stack: %.4f)", v1);

    // stacks grow in place and grow the tick
    const uint64_t wolf3 = bed.wolf(300);
    for (int i = 0; i < 3; ++i) bed.land(wolf3, "Scorpion_TailSting_Hit", t0 + 0.1 * i, bed.player);
    CHECKF(bed.world.effect_stacks(wolf3, pg) == 3 && bed.items_named(wolf3, "Status_Poisoned") == 1, "three stings are one poison of three stacks (%d)", bed.world.effect_stacks(wolf3, pg));
    bed.run(t0 + 0.25, t0 + 12.1);
    const std::vector<dots::Tick> t3 = bed.ticks(wolf3, "Status_Poisoned_Tick");
    double v3 = 0;
    for (const dots::Tick& t : t3)
        if (t.plain() && v3 == 0) v3 = t.value;
    CHECKF(v1 < 0 && v3 < 0 && near(v3 / v1, 3.0, 0.05), "...and each of its ticks is three times a tick of one stack: %.4f against %.4f", v3, v1);
    // the ten stacks
    const uint64_t wolf12 = bed.wolf(600);
    for (int i = 0; i < 12; ++i) bed.land(wolf12, "Scorpion_TailSting_Hit", t0 + 0.05 * i, bed.player);
    CHECKF(bed.world.effect_stacks(wolf12, pg) == 10, "twelve stings stack to ten and no more (%d)", bed.world.effect_stacks(wolf12, pg));
    bed.run(t0 + 0.7, t0 + 12.1);
    const std::vector<dots::Tick> t10 = bed.ticks(wolf12, "Status_Poisoned_Tick");
    double v10 = 0;
    for (const dots::Tick& t : t10)
        if (t.plain() && v10 == 0) v10 = t.value;
    CHECKF(v10 < 0 && near(v10 / v1, 10.0, 0.15), "...and a tick of ten stacks is ten times a tick of one: %.4f against %.4f", v10, v1);

    // another caster
    const uint64_t wolf2 = bed.wolf(900);
    bed.land(wolf2, "Scorpion_TailSting_Hit", t0, bed.player);
    bed.land(wolf2, "Scorpion_TailSting_Hit", t0 + 0.1, bed.other);
    CHECKF(bed.items_named(wolf2, "Status_Poisoned") == 2, "a second caster's poison is a poison of its own (%zu items)", bed.items_named(wolf2, "Status_Poisoned"));

    // a slow is still left off a mob (the next slice): the Chilling poison's poison lands and its Chilled does not
    const uint64_t wolf5 = bed.wolf(1200);
    bed.land(wolf5, "Rogue_Poison_Chilling", t0, bed.player);
    CHECKF(bed.items_named(wolf5, "Status_Poisoned") == 1 && bed.items_named(wolf5, "Status_Chilled") == 0 && bed.world.effect_stacks(wolf5, bed.effect_guid("Status_Chilled")) == 0,
           "the Chilling poison puts its poison on the Wolf and leaves its Chilled off (%zu poisons, %zu chills)", bed.items_named(wolf5, "Status_Poisoned"), bed.items_named(wolf5, "Status_Chilled"));
}
AOC_REGISTER_TEST(test_creature_poison);

// ---- stat debuffs --------------------------------------------------------------------------------------------------------------------------------------------
// Shaken and Volatile make a creature take more physical and magical damage, Demoralized makes it hit softer and Blinded lowers its accuracy: the mods act through the creature's own
// stats, which read the effects it holds; they end with the effect.  Shaken and Volatile last 6 s, a new application adds at least 3 s and none leaves more than 15 s.
void test_creature_stat_debuffs(Fixture& f) {
    std::printf("the stat debuffs: Shaken and Volatile raise the damage a creature takes, Demoralized lowers what it deals, Blinded its accuracy; the extension of Shaken\n");
    dots::Bed bed(f);
    CHECKF(bed.ok(), "the bed: %s", bed.problem().c_str());
    if (!bed.ok()) return;
    const abilities::EffectDef* shaken = f.catalog->effect_named("Status_Shaken");
    const abilities::EffectDef* volatile_ = f.catalog->effect_named("Status_Volatile");
    const abilities::EffectDef* demoralized = f.catalog->effect_named("Status_Demoralized");
    const abilities::EffectDef* blinded = f.catalog->effect_named("Status_Blinded");
    CHECK(shaken && volatile_ && demoralized && blinded && !shaken->mods.empty() && !volatile_->mods.empty() && !demoralized->mods.empty() && !blinded->mods.empty(), "the four debuffs and their stat mods are in the data");
    if (!shaken || !volatile_ || !demoralized || !blinded) return;
    const double t0 = 100.0;
    const uint64_t wolf = bed.wolf(0);
    CHECK(wolf != 0, "a Wolf 30 m away");
    if (!wolf) return;
    combat::Fighter* wolf_fighter = bed.world.fighter_of(wolf);
    combat::Fighter& mage = bed.player.fighter();

    // the damage the Wolf takes from a physical and a magical hit, and the damage it deals, before the debuffs
    const double physical_before = bed.plain_damage(mage, *wolf_fighter, "Ranger_Snipe_V2");
    const double magical_before = bed.plain_damage(mage, *wolf_fighter, "Mage_BallLightning");
    const double dealt_before = bed.plain_damage(*wolf_fighter, mage, "Ranger_Snipe_V2");
    const int64_t accuracy = blinded->mods[0].stat_guid, damage_mult = demoralized->mods[0].stat_guid;
    const double accuracy_before = bed.stat_of(wolf, accuracy), mult_before = bed.stat_of(wolf, damage_mult);
    CHECKF(physical_before < 0 && magical_before < 0 && dealt_before < 0, "(a Snipe takes %.1f from the Wolf, a Ball Lightning %.1f, and the Wolf deals %.1f with the Snipe's formula)", physical_before, magical_before, dealt_before);

    // Shaken: more physical damage, none more magical
    const dots::Landing s = bed.land(wolf, "Beetle_Subsonics_Hit", t0, bed.player);
    CHECK(s.has("Status_Shaken") && s.has("Status_Blinded"), "a hit puts Shaken and Blinded on the Wolf");
    const double physical_shaken = bed.plain_damage(mage, *wolf_fighter, "Ranger_Snipe_V2");
    const double magical_shaken = bed.plain_damage(mage, *wolf_fighter, "Mage_BallLightning");
    CHECKF(physical_shaken < physical_before * 1.01 && physical_shaken < 0, "Shaken: the Wolf takes more physical damage (%.1f, it was %.1f)", physical_shaken, physical_before);
    CHECKF(near(magical_shaken, magical_before, std::fabs(magical_before) * 1e-6), "...and no more magical damage (%.1f, it was %.1f)", magical_shaken, magical_before);
    CHECKF(bed.stat_of(wolf, accuracy) < accuracy_before - 1e-9, "Blinded: its accuracy is lower (%.3f, it was %.3f)", bed.stat_of(wolf, accuracy), accuracy_before);
    const dots::Landing v = bed.land(wolf, "Mage_BallLightning", t0 + 0.1, bed.player);
    CHECK(v.has("Status_Volatile"), "a Ball Lightning puts Volatile on it");
    const double magical_volatile = bed.plain_damage(mage, *wolf_fighter, "Mage_BallLightning");
    CHECKF(magical_volatile < magical_before * 1.01 && magical_volatile < 0, "Volatile: the Wolf takes more magical damage (%.1f, it was %.1f)", magical_volatile, magical_before);
    const dots::Landing d = bed.land(wolf, "Fighter_BattleCry_Enemy", t0 + 0.2, bed.player);
    CHECK(d.has("Status_Demoralized"), "a Battle Cry puts Demoralized on it");
    const double dealt_demoralized = bed.plain_damage(*wolf_fighter, mage, "Ranger_Snipe_V2");
    CHECKF(dealt_demoralized > dealt_before - 1e-9 && dealt_demoralized < 0 && near(bed.stat_of(wolf, damage_mult), mult_before, 1e-12) == false,
           "Demoralized: the Wolf deals less damage (%.1f, it was %.1f; its damage multiplier %.3f, it was %.3f)", dealt_demoralized, dealt_before, bed.stat_of(wolf, damage_mult), mult_before);

    std::printf("    (Shaken: a physical hit x%.3f, Volatile: a magical hit x%.3f, Demoralized: the Wolf's hits x%.3f, its damage multiplier %.3f -> %.3f, Blinded: accuracy %.3f -> %.3f)\n",
                physical_before != 0 ? physical_shaken / physical_before : 0.0, magical_before != 0 ? magical_volatile / magical_before : 0.0, dealt_before != 0 ? dealt_demoralized / dealt_before : 0.0,
                mult_before, bed.stat_of(wolf, damage_mult), accuracy_before, bed.stat_of(wolf, accuracy));

    // they end with the effect
    bed.run(t0 + 0.25, t0 + 40.0);
    CHECK(bed.world.effect_count(wolf) == 0, "after their time nothing is left on the Wolf");
    const double physical_after = bed.plain_damage(mage, *wolf_fighter, "Ranger_Snipe_V2");
    const double dealt_after = bed.plain_damage(*wolf_fighter, mage, "Ranger_Snipe_V2");
    CHECKF(near(physical_after, physical_before, std::fabs(physical_before) * 1e-6) && near(dealt_after, dealt_before, std::fabs(dealt_before) * 1e-6) && near(bed.stat_of(wolf, accuracy), accuracy_before, 1e-9),
           "...and the damage it takes, the damage it deals and its accuracy are what they were (%.1f, %.1f)", physical_after, dealt_after);

    // the extension of Shaken: 6 s at first; another application adds max(6 - left, 3) s and no more than 15 s are ever left
    const int64_t sg = shaken->row.guid;
    const uint64_t wolf2 = bed.wolf(300);
    bed.land(wolf2, "Beetle_Subsonics_Hit", t0, bed.player);   // (this hit gives it 24 s: its own duration)
    const uint64_t wolf3 = bed.wolf(600);
    const dots::Landing a = bed.land(wolf3, "Weapon_Proc_Concussion_2", t0, bed.player);
    const effects::Applied* sa = a.find("Status_Shaken");
    CHECK(sa != nullptr, "a Concussion hit puts Shaken on the Wolf");
    if (!sa) return;
    CHECKF(sa->duration > 5.99 && sa->extension >= 3.0 && near(sa->extended_max, 15.0, 1e-9), "...for at least the record's 6 s (%.2f s), with an extension of at least 3 s (%.2f) and no more than 15 s ever (%.1f)", sa->duration,
           sa->extension, sa->extended_max);
    const double first = bed.world.effect_remaining(wolf3, sg, t0);
    bed.land(wolf3, "Weapon_Proc_Concussion_2", t0 + 1.0, bed.player);
    const double second = bed.world.effect_remaining(wolf3, sg, t0 + 1.0);
    CHECKF(near(second, (first - 1.0) + 3.0, 0.6) || second > first - 1.0, "a second application a second later adds to what is left (%.2f s left of %.2f s)", second, first);
    for (int i = 0; i < 12; ++i) bed.land(wolf3, "Weapon_Proc_Concussion_2", t0 + 1.0 + 0.01 * i, bed.player);
    const double most = bed.world.effect_remaining(wolf3, sg, t0 + 1.2);
    CHECKF(most <= 15.0 + 1e-6 && most >= 14.0, "...and however often it is applied, never more than 15 s are left (%.2f)", most);
    CHECKF(bed.world.effect_stacks(wolf3, sg) == 1, "...and Shaken stays one stack (%d)", bed.world.effect_stacks(wolf3, sg));

    // the Bard's finisher melodies: Menacing puts Shocked on the Wolf and Cathartic puts Wounded on it
    const uint64_t wolf4 = bed.wolf(900);
    const dots::Landing menacing = bed.land(wolf4, "Bard_ResonantWeapon_End_Menacing", t0, bed.player);
    const dots::Landing cathartic = bed.land(wolf4, "Bard_ResonantWeapon_End_Cathartic", t0, bed.player);
    CHECK(menacing.has("Status_Shocked") && cathartic.has("Status_Wounded") && bed.items_named(wolf4, "Status_Shocked") == 1 && bed.items_named(wolf4, "Status_Wounded") == 1,
          "the Bard's Menacing melody puts Shocked on the Wolf and the Cathartic one Wounded");
}
AOC_REGISTER_TEST(test_creature_stat_debuffs);

// ---- the host functions of an effect in progress -------------------------------------------------------------------------------------------------------------
// While the numbers of an effect are worked out its expressions ask what the effect is: EffectHasTag(GetEffect(), tag), GetEffect() == <effect>, and how long another effect lasts
// (CalcEffectDuration).  The context answers them only for an effect in progress; otherwise they are left to whoever runs the cast.
void test_effect_host_functions(Fixture&) {
    std::printf("EffectHasTag, GetEffect() and CalcEffectDuration are answered for the effect whose numbers are worked out\n");
    const std::vector<std::string> tags = {"Element.Physical.Bleed", "Debuff.Bleeding"};
    const std::map<int64_t, double> durations = {{111, 6.0}, {222, 0.0}};
    combat::HitContext ctx;
    double out = -1;
    CHECK(!ctx.host_fn(stats::kHostEffectHasTag, "Debuff.Bleeding", nullptr, 0, out) && !ctx.host_fn(stats::kHostEffectIs, "5", nullptr, 0, out) &&
              !ctx.host_fn(stats::kHostEffectDuration, "111", nullptr, 0, out),
          "with no effect in progress none of the three is answered: the cast's own host does");
    ctx.effect_guid = 5;
    ctx.effect_tags = &tags;
    ctx.effect_durations = &durations;
    const auto ask = [&](int fn, const char* name) {
        out = -1;
        return ctx.host_fn(fn, name, nullptr, 0, out) ? out : -999.0;
    };
    CHECK(ask(stats::kHostEffectHasTag, "Debuff.Bleeding") == 1.0 && ask(stats::kHostEffectHasTag, "Element.Physical") == 1.0 && ask(stats::kHostEffectHasTag, "Debuff") == 1.0,
          "EffectHasTag: the effect has its own tags and the ones above them (Element.Physical.Bleed is Element.Physical)");
    CHECK(ask(stats::kHostEffectHasTag, "Element.Magical") == 0.0 && ask(stats::kHostEffectHasTag, "Debuff.Snared") == 0.0, "...and not the tags it does not have");
    CHECK(ask(stats::kHostEffectIs, "5") == 1.0 && ask(stats::kHostEffectIs, "6") == 0.0, "GetEffect() == <effect> is true for the effect in progress and for no other");
    CHECK(ask(stats::kHostEffectDuration, "111") == 6.0 && ask(stats::kHostEffectDuration, "222") == 0.0 && ask(stats::kHostEffectDuration, "333") == 0.0, "CalcEffectDuration reads the length worked out for the effect named (0 for one that was not)");
}
AOC_REGISTER_TEST(test_effect_host_functions);