// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_alert_effects_tests.inc
//
//  Registered tests of test_worldd_combat: the alert moves whose hits put an effect on the caster itself or on its allies (the goblins' Rally, the Tidebreakers' Yawp, the goblin shamans'
//  auras and Rally, the Minotaur Battle Priests' prayers and the Berserkers' rage).  The hit lands at its montage's hit notify, on the caster (Self) and on the living creatures on its
//  friendly side (Ally: the relation of the alignments, whatever the Blueprint) inside its targeting record, and puts the effect on them through the world's effect code.  A player, a
//  creature the relation calls hostile, a dead mob and a mob out of the radius are never reached; the effect changes the holder's stats and ends with its time, its death or its reset.
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================

namespace alert_effects {

// The program's CHECK prints only a failure; these checks print the pass too, so the green run's log holds the line of the same check that failed red.
#define ALERT_EFFECT(cond, ...) do { \
    if (cond) { std::printf("  [ok ] "); ++g_pass; } \
    else { std::printf("  [FAIL] "); ++g_fail; } \
    std::printf(__VA_ARGS__); std::printf("\n"); \
} while (0)

/// A one-alert tree for a caster path: the alert first (no condition, `cooldown` seconds) and an attack after it, so the creature has a fight to animate.
struct LoadedTree {
    combat::NpcAbilityData tables;
    bool                   ok = false;
    std::string            error;
};

inline LoadedTree one_alert_tree(const std::string& path, int64_t alert, int cooldown, int64_t attack) {
    namespace fs = std::filesystem;
    LoadedTree out;
    std::string dir = NPC_MOTION_PATH;
    dir.erase(dir.find_last_of("/\\") + 1);
    const fs::path tmp = fs::temp_directory_path() / ("aoc_alert_effects_" + unique_temp_suffix());   // (a folder of its own: two gates can run this program at once)
    std::error_code ec;
    fs::create_directories(tmp, ec);
    const std::string table = (tmp / "abilities.tsv").string();
    {
        std::ofstream o(table, std::ios::binary);
        o << "archetype\tability\ttask\tcooldown\tstop_movement\torder\tmin_distance\tmax_distance\tmax_angle\tgates\tunknown\tfight\tconds\twhy\tstarts\trotate\tarrival\n";
        o << path << "\t" << alert << "\tTest\t" << cooldown << "\t1\t10\t-1\t-1\t-1\tTest\t0\t1\t\t\t\t\t-1\n";
        o << path << "\t" << attack << "\t\t0\t1\t24\t-1\t360\t90\t\t0\t1\t\t\t\t360:0\t-1\n";
    }
    out.ok = out.tables.load(table, dir + "npc_montages.tsv", dir + "npc_targeting.tsv", out.error);
    fs::remove_all(tmp, ec);
    return out;
}

/// A world of one tested caster, its allies and a player who stands still at (px, py) of the plain.
struct Field {
    Fixture&                       f;
    const combat::NpcAbilityData&  tables;
    Rig                            rig;
    combat_rig::World              w;
    combat::NpcStatBook            book;
    combat::CombatWorld            world;
    combat_rig::FakePlayer         player;

    Field(Fixture& fixture, const combat::NpcAbilityData& loaded, double px = 0, double py = 0)
        : f(fixture), tables(loaded), rig(4, 20, {}), book(*fixture.ctx->bundle, *fixture.ctx->gear),
          world(w.registry, w.npcs.get(), &navfix::shared_nav(), *fixture.data, *fixture.catalog, book, fixture.motion, config()), player(rig, 77, {px, py, 1098}) {
        world.set_ability_data(&tables);
        world.add_player(&player);
    }
    static combat::CombatWorldConfig config() {
        combat::CombatWorldConfig c;
        c.seed = 5;
        return c;
    }
    bool ok() const { return rig.ok() && w.problem.empty(); }
    uint64_t spawn(const std::string& path, double x, double y, double aggro = 1, const spawns::OpenTemplate* made = nullptr) {
        const auto bp = f.motion.find(path);
        if (!bp) return 0;
        return w.spawn(path, x, y, 7, aggro, bp->half_height, made, &*bp);
    }
    void tick(double now) {
        if (const stats::StatValue* max = rig.cs->find(content::kGuidMaxHealth)) rig.cs->set_current(content::kGuidHealth, max->value);   // (the fight is about the mobs, not the player's life)
        const combat::Vec3 at = player.rig_fighter().position();
        w.npcs->note_player(77, at.x, at.y, now);
        w.npcs->tick(now);
        world.tick(now);
    }
    int stacks(uint64_t actor, int64_t effect) const { return world.effect_stacks(actor, effect); }
    /// Whether the actor's CurrentEffects array (what its clients see) holds the effect record.
    bool shows(uint64_t actor, int64_t effect) const {
        const world::WorldActor* a = w.registry.find(actor);
        if (!a) return false;
        for (const combat::EffectItemWrite& it : combat::read_effect_items(*a)) {
            if (it.item.effect_guid == effect) return true;
        }
        return false;
    }
    double stat(uint64_t actor, int64_t stat_guid) {
        combat::Fighter* fighter = world.fighter_of(actor);
        const stats::StatValue* v = fighter ? fighter->stats().find(stat_guid) : nullptr;
        return v ? v->value : 0.0;
    }
    double health(uint64_t actor) {
        combat::Fighter* fighter = world.fighter_of(actor);
        const stats::StatValue* h = fighter ? fighter->stats().find(content::kGuidHealth) : nullptr;
        return h ? h->value : -1.0;
    }
};

/// The effect an ability's first apply row names.
inline const abilities::EffectDef* effect_of(Fixture& f, const abilities::AbilityDef& ability) {
    for (const content::AbilityHitRow& r : ability.hits) {
        const abilities::HitDef* hit = f.catalog->hit(r.hit_guid);
        if (!hit) continue;
        for (const content::HitEffectRow& e : hit->effects) {
            if (e.kind != "self") return f.catalog->effect(e.effect_guid);
        }
    }
    return nullptr;
}

}  // namespace alert_effects

// R1: who each alert's hit reaches and what it puts on, from the ability's own data: the mask (Self 1, Ally 4), the targeting record's sphere, the hit's cap and the effect's record.
void test_alert_effects_in_the_ability_data(Fixture& f) {
    std::printf("the alert moves that put effects on the caster or its allies: the data's mask, shape, cap, hit notify and effect record\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    struct Row {
        const char* ability;
        int         mask;
        double      radius;
        const char* effect;
        bool        infinite;
        bool        capped;
    };
    const Row rows[] = {
        {"Goblin_Rally", 4, 1000, "Goblin_Rally_Effect", false, true},
        {"GoblinShaman_Rally", 4, 1000, "Goblin_Rally_Effect", false, true},
        {"Goblin_Warlord_Rally", 4, 1000, "Goblin_Rally_Effect", false, true},
        {"Tidebreaker_Yawp", 5, 2000, "Tidebreaker_Yawp_Effect", true, false},
        {"GoblinShaman_FreneticAura", 4, 1400, "GoblinShaman_FreneticAura_Effect", false, false},
        {"GoblinShaman_FireShieldAura", 5, 1400, "GoblinShaman_FireShieldAura_Effect", false, false},
        {"Minotaur_BattlePriest_BattlePrayer_Daze", 1, 0, "Minotaur_BattlePriest_Radiant", false, false},
        {"Minotaur_BattlePriestMummy_BattlePrayer_Daze", 1, 0, "Minotaur_BattlePriest_Radiant_Mummy", false, false},
        {"Minotaur_Berserker_BloodFury", 1, 0, "Minotaur_Berserker_BloodFury_Effect", false, false},
    };
    for (const Row& row : rows) {
        const abilities::AbilityDef* d = f.catalog->ability_named(row.ability);
        ALERT_EFFECT(d != nullptr, "the ability data has %s", row.ability);
        if (!d) continue;
        const content::AbilityHitRow* hit_row = nullptr;
        for (const content::AbilityHitRow& r : d->hits) {
            if (r.source == "hit") hit_row = &r;
        }
        const abilities::HitDef* hit = hit_row ? f.catalog->hit(hit_row->hit_guid) : nullptr;
        ALERT_EFFECT(hit != nullptr && hit->row.applicable_targets == row.mask, "%s hits mask %d (Self 1, Ally 4): %d", row.ability, row.mask, hit ? hit->row.applicable_targets : -1);
        if (!hit) continue;
        ALERT_EFFECT(combat::is_alert_move(*d, *f.catalog, f.data->ids().health), "%s passes the alert rule (it hurts nobody, and every hit puts an effect on its own side)", row.ability);
        ALERT_EFFECT(hit->mods.empty() && hit->remove_effects.empty(), "%s's hit changes no stat directly and takes nothing off anyone", row.ability);
        const combat::BiteArea* area = tables.area_of(d->row.guid, 0);
        if (row.radius > 0) {
            const bool sphere = area && area->volumes.size() == 1 && area->volumes[0].shape == combat::HitVolume::Shape::Sphere && near(area->volumes[0].radius, row.radius, 1.0);
            ALERT_EFFECT(sphere, "%s reaches its allies in a sphere of %.0f cm (a radius of %s)", row.ability, row.radius, area && !area->volumes.empty() ? std::to_string(area->volumes[0].radius).c_str() : "no volume");
        }
        const bool cap_named = hit->row.max_targets_expr != 0;
        ALERT_EFFECT(cap_named == row.capped, "%s names %s cap (%s)", row.ability, row.capped ? "a" : "no", cap_named ? "a max_targets expression" : "none");
        ALERT_EFFECT(!hit->effects.empty(), "%s's hit applies an effect", row.ability);
        const abilities::EffectDef* effect = alert_effects::effect_of(f, *d);
        ALERT_EFFECT(effect && effect->row.name == row.effect, "%s puts on %s (%s)", row.ability, row.effect, effect ? effect->row.name.c_str() : "no effect");
        if (effect) ALERT_EFFECT(effect->row.infinite_duration == row.infinite, "%s lasts %s", row.effect, row.infinite ? "until its holder dies or resets" : "a number of seconds");
        // the effect lands at the montage's hit notify, not at the cast's start
        const combat::MontageData* montage = nullptr;
        for (const content::MontageRow& m : d->montages) {
            if (!m.soft_path.empty() && (montage = tables.montage(m.soft_path))) break;
        }
        bool notify = false;
        if (montage) {
            for (const combat::MontageData::Hit& h : montage->hits) notify = notify || h.key == 0;
        }
        ALERT_EFFECT(notify, "%s's montage has a hit notify for its key 0 (the effect lands at the animation's hit, not at the cast's start)", row.ability);
    }
    // the two stat-changing records the tests use: Frenetic Aura's power and speed, the Resonance prayer's mitigation
    const abilities::EffectDef* frenetic = f.catalog->effect_named("GoblinShaman_FreneticAura_Effect");
    bool speed = false, power = false;
    if (frenetic) {
        for (const content::ModRow& m : frenetic->mods) {
            const content::ExprRow* value = f.catalog->expr(m.value_expr);
            if (value && m.stat_guid == f.data->ids().move_speed_mult && value->text == "0.2") speed = true;
            if (value && m.stat_name == "PhysicalPowerMult" && value->text == "1.1") power = true;
        }
    }
    ALERT_EFFECT(frenetic && speed && power, "Frenetic Aura raises PhysicalPowerMult to 1.1 (a tenth more) and MoveSpeedMult by 0.2");
    const abilities::EffectDef* resonance = f.catalog->effect_named("Minotaur_BattlePriest_Resonance");
    ALERT_EFFECT(resonance && !resonance->mods.empty(), "the Resonance prayer raises a mitigation stat on its holder");
    // its hit opens a lingering zone (the game's healing ground): the world has no ground zones of a creature, so the prayer is held, not an alert
    if (const abilities::AbilityDef* resonance_prayer = f.catalog->ability_named("Minotaur_BattlePriest_BattlePrayer_Resonance")) {
        ALERT_EFFECT(!combat::is_alert_move(*resonance_prayer, *f.catalog, f.data->ids().health), "the Resonance prayer, whose hit opens a zone, is not an alert");
    }
}

AOC_REGISTER_TEST(test_alert_effects_in_the_ability_data);

// R2 and R4 (R10: the friendly side is the relation): a goblin's Rally reaches the living creatures on its friendly side in its 10 m sphere, up to the hit's 10, whatever their
// Blueprint family or alignment set: the goblins, a wolf and a goblin of another set are all on its side when the relation says so; the caster, the players, a goblin out of the
// radius and a dead goblin are not reached, and a hostile creature is not either.
void test_goblin_rally_buffs_the_allies_its_filter_picks(Fixture& f) {
    std::printf("a goblin's Rally puts its effect on the ten nearest living creatures on its friendly side in its 10 m sphere, at its montage's hit notify; the players, the far and the dead are not reached\n");
    const abilities::AbilityDef* rally = f.catalog->ability_named("Goblin_Rally");
    const abilities::AbilityDef* claw = f.catalog->ability_named("Tidebreaker_Claw");
    const abilities::EffectDef* effect = f.catalog->effect_named("Goblin_Rally_Effect");
    const std::string caster_path = creature_path(f, "/Humanoid/Goblin/Goblin.Default__Goblin_C");
    const std::string ally_path = creature_path(f, "/Humanoid/Goblin/Goblin_Bonebinder.Default__Goblin_Bonebinder_C");
    const std::string out_path = creature_path(f, "/Humanoid/Goblin/Goblin_Bonebinder.Default__Goblin_Bonebinder_C");
    const std::string wolf_path = creature_path(f, "/Beast/Canine/Wolf.Default__Wolf_C");
    const std::string other_faction = creature_path(f, "/Humanoid/Goblin/Goblin_LanguidGoblin.Default__Goblin_LanguidGoblin_C");
    const std::string hostile_path = creature_path(f, "/Monster/Undead/Skeleton/Skeleton.Default__Skeleton_C");
    ALERT_EFFECT(!caster_path.empty() && !ally_path.empty() && !wolf_path.empty() && !other_faction.empty() && !hostile_path.empty() && rally && claw && effect,
                 "the Blueprint table has the goblins, the wolf and the Skeleton, and the ability data has the Rally, its Claw and its effect");
    if (caster_path.empty() || ally_path.empty() || wolf_path.empty() || other_faction.empty() || hostile_path.empty() || !rally || !claw || !effect) return;
    const alert_effects::LoadedTree tree = alert_effects::one_alert_tree(caster_path, rally->row.guid, 3.5, claw->row.guid);
    ALERT_EFFECT(tree.ok, "the goblin's one-alert tree loads (%s)", tree.error.c_str());
    if (!tree.ok) return;
    BanditOpen made;
    ALERT_EFFECT(make_bandit_open(f, tree.tables, caster_path, made), "the world makes an open for the goblin whose tree has the Rally (verdict: %s)", spawns::verdict_name(made.why));
    if (!made.open) return;
    alert_effects::Field field(f, tree.tables);
    ALERT_EFFECT(field.ok(), "the fight's world is made (the captured data and the rig)");
    if (!field.ok()) return;
    Rig rig2(4, 20, {});
    ALERT_EFFECT(rig2.ok(), "a second player's rig is made");
    if (!rig2.ok()) return;
    combat_rig::FakePlayer player2(rig2, 78, {-300, 0, 1098});
    field.world.add_player(&player2);   // a second player in the fight is never an ally of the mobs
    constexpr double cx = 0, cy = 500;
    const uint64_t caster = field.spawn(caster_path, cx, cy, 900, made.open);
    std::vector<uint64_t> allies;
    for (int i = 0; i < 12; ++i) {   // a ring at 5 m: all inside the 10 m sphere, more than the hit's cap of ten
        const double rad = 2.0 * 3.14159265358979323846 * static_cast<double>(i) / 12.0;
        allies.push_back(field.spawn(ally_path, cx + 500.0 * std::cos(rad), cy + 500.0 * std::sin(rad), 1));
    }
    const uint64_t far = field.spawn(out_path, cx, cy + 1600.0, 1);            // out of the 10 m sphere
    const uint64_t wolf = field.spawn(wolf_path, cx + 300.0, cy, 1);            // another family, another alignment set
    const uint64_t other = field.spawn(other_faction, cx - 300.0, cy, 1);       // the same family, another alignment set
    const uint64_t dead = field.spawn(ally_path, cx + 200.0, cy, 1);            // dead before the cast
    ALERT_EFFECT(caster != 0 && far != 0 && wolf != 0 && other != 0 && dead != 0 && std::find(allies.begin(), allies.end(), 0) == allies.end(),
                 "the caster, the ring, the goblin out of the radius, the wolf, the other-set goblin and the dead goblin are put in the world");
    if (caster == 0 || far == 0 || wolf == 0 || other == 0 || dead == 0) return;
    // who the relation calls friendly: the caster's alignments against the wolf's, the other goblin's and a Skeleton's
    const auto relation_to = [&](const std::string& path) {
        const auto caster_bp = f.motion.find(caster_path), other_bp = f.motion.find(path);
        if (!caster_bp || !other_bp) return combat::Relation::Neutral;
        std::vector<int64_t> a(caster_bp->alignments.begin(), caster_bp->alignments.end());
        std::vector<int64_t> b(other_bp->alignments.begin(), other_bp->alignments.end());
        return f.data->relation(a, b);
    };
    const combat::Relation wolf_relation = relation_to(wolf_path);
    const combat::Relation other_relation = relation_to(other_faction);
    const combat::Relation skeleton_relation = relation_to(hostile_path);
    const int friendly_others = (wolf_relation != combat::Relation::Hostile ? 1 : 0) + (other_relation != combat::Relation::Hostile ? 1 : 0);
    uint64_t hostile = 0;
    if (skeleton_relation == combat::Relation::Hostile) {
        hostile = field.spawn(hostile_path, cx, cy + 350.0, 1);   // hostile to the caster, inside the sphere
        if (world::WorldActor* actor = field.w.registry.find(hostile)) actor->npc.aggro_off = true;   // (it stands there: it does not start a fight that would disturb the ring)
        ALERT_EFFECT(hostile != 0, "a hostile Skeleton is put in the sphere");
    }
    std::string why;
    field.world.gm_kill(dead, 0.0, why);
    std::vector<int> ever(allies.size(), 0);
    bool wolf_ever = false, other_ever = false;
    size_t casts = 0;
    GremCastView last;
    const double dt = 1.0 / 15.0;
    for (double now = dt; now <= 8.0 + 1e-9; now += dt) {
        field.tick(now);
        const GremCastView view = grem_cast_view(field.w.registry, caster);
        if (view.montage != 0 && (view.element != last.element || last.montage == 0)) ++casts;   // (the same cast twice in a row: the 3.5 s cooldown lets the second begin after the first montage)
        last = view;
        for (size_t i = 0; i < allies.size(); ++i) {
            if (field.stacks(allies[i], effect->row.guid) > 0) ever[i] = 1;
        }
        wolf_ever = wolf_ever || field.stacks(wolf, effect->row.guid) > 0;
        other_ever = other_ever || field.stacks(other, effect->row.guid) > 0;
    }
    size_t held_at_end = 0;
    for (const uint64_t ally : allies) held_at_end += field.stacks(ally, effect->row.guid) > 0 ? 1 : 0;
    const int ring_buffed = static_cast<int>(std::count(ever.begin(), ever.end(), 1));
    const int total_buffed = ring_buffed + (wolf_ever ? 1 : 0) + (other_ever ? 1 : 0);
    ALERT_EFFECT(casts >= 2, "the Rally is cast twice in the 8 s fight (the same cast twice in a row): %zu", casts);
    ALERT_EFFECT(total_buffed == 10, "the Rally reaches ten creatures on its side in the sphere (the hit's cap): %d reached", total_buffed);
    ALERT_EFFECT(ring_buffed == 10 - friendly_others, "the cap leaves %d of the twelve goblins of the ring reached (the %d friendly others beside it are nearer)", ring_buffed, friendly_others);
    ALERT_EFFECT(field.stacks(far, effect->row.guid) == 0, "the goblin 16 m away is out of the 10 m sphere and is not reached");
    ALERT_EFFECT(wolf_relation == combat::Relation::Hostile ? field.stacks(wolf, effect->row.guid) == 0 : field.stacks(wolf, effect->row.guid) > 0,
                 "the wolf is reached exactly when the relation calls it friendly (the relation is %s)", wolf_relation == combat::Relation::Hostile ? "hostile" : "friendly or neutral");
    ALERT_EFFECT(other_relation == combat::Relation::Hostile ? field.stacks(other, effect->row.guid) == 0 : field.stacks(other, effect->row.guid) > 0,
                 "the goblin of another alignment set is reached exactly when the relation calls it friendly (the relation is %s)", other_relation == combat::Relation::Hostile ? "hostile" : "friendly or neutral");
    if (skeleton_relation == combat::Relation::Hostile) {
        ALERT_EFFECT(hostile != 0 && field.stacks(hostile, effect->row.guid) == 0, "the hostile Skeleton in the sphere is not reached (the relation is hostile)");
    }
    ALERT_EFFECT(field.stacks(dead, effect->row.guid) == 0, "the dead goblin is not reached");
    ALERT_EFFECT(field.stacks(caster, effect->row.guid) == 0, "the caster is not reached by its own Rally (the hit's mask names its allies, not itself)");
    ALERT_EFFECT(field.rig.pipe->stacks_of(effect->row.guid) == 0, "the player beside the fight is never buffed");
    ALERT_EFFECT(rig2.pipe->stacks_of(effect->row.guid) == 0, "a second player in the fight is never buffed either");
    const size_t held_total = held_at_end + (field.stacks(wolf, effect->row.guid) > 0 ? 1 : 0) + (field.stacks(other, effect->row.guid) > 0 ? 1 : 0);
    ALERT_EFFECT(held_total == static_cast<size_t>(total_buffed), "the reached creatures hold the effect now (its three seconds from the last pulse): %zu of %d", held_total, total_buffed);
    size_t shown = 0;
    for (const uint64_t ally : allies) shown += field.shows(ally, effect->row.guid) ? 1 : 0;
    ALERT_EFFECT(shown == static_cast<size_t>(ring_buffed), "the reached goblins of the ring show it in their CurrentEffects array (what a client beside them is told): %zu", shown);
}

AOC_REGISTER_TEST(test_goblin_rally_buffs_the_allies_its_filter_picks);

// R3: Frenetic Aura raises a goblin ally's MoveSpeedMult while it lasts, and the effect and the number end with its 20 seconds.
void test_frenetic_aura_changes_a_stat_and_ends_with_its_time(Fixture& f) {
    std::printf("a goblin shaman's Frenetic Aura raises the MoveSpeedMult of the goblins in its 14 m sphere while it lasts, and both end with its 20 s\n");
    const abilities::AbilityDef* aura = f.catalog->ability_named("GoblinShaman_FreneticAura");
    const abilities::AbilityDef* claw = f.catalog->ability_named("Tidebreaker_Claw");
    const abilities::EffectDef* effect = f.catalog->effect_named("GoblinShaman_FreneticAura_Effect");
    const std::string caster_path = creature_path(f, "/Humanoid/Goblin/GoblinCaster/GoblinShaman/Goblin_Shaman.Default__Goblin_Shaman_C");
    const std::string ally_path = creature_path(f, "/Humanoid/Goblin/Goblin_Bonebinder.Default__Goblin_Bonebinder_C");
    ALERT_EFFECT(!caster_path.empty() && !ally_path.empty() && aura && claw && effect && f.data->ids().move_speed_mult != 0, "the Blueprint table has the goblin shaman and a goblin, and the ability data has the Frenetic Aura, its Claw and its effect");
    if (caster_path.empty() || ally_path.empty() || !aura || !claw || !effect) return;
    const alert_effects::LoadedTree tree = alert_effects::one_alert_tree(caster_path, aura->row.guid, 300, claw->row.guid);
    ALERT_EFFECT(tree.ok, "the shaman's one-alert tree loads (%s)", tree.error.c_str());
    if (!tree.ok) return;
    BanditOpen made;
    ALERT_EFFECT(make_bandit_open(f, tree.tables, caster_path, made), "the world makes an open for the goblin shaman (verdict: %s)", spawns::verdict_name(made.why));
    if (!made.open) return;
    alert_effects::Field field(f, tree.tables);
    ALERT_EFFECT(field.ok(), "the fight's world is made");
    if (!field.ok()) return;
    const uint64_t caster = field.spawn(caster_path, 0, 500, 900, made.open);
    const uint64_t ally = field.spawn(ally_path, 250, 500, 1);
    const uint64_t far = field.spawn(ally_path, 0, 500 + 1600, 1);
    ALERT_EFFECT(caster != 0 && ally != 0 && far != 0, "the shaman, an ally in the 14 m sphere and one out of it are put in the world");
    if (caster == 0 || ally == 0 || far == 0) return;
    const double base = field.stat(ally, f.data->ids().move_speed_mult);
    const double base_far = field.stat(far, f.data->ids().move_speed_mult);
    const double dt = 1.0 / 15.0;
    double at_5 = -1, at_22 = -1;
    bool buffed = false, shown = false;
    for (double now = dt; now <= 24.0 + 1e-9; now += dt) {
        field.tick(now);
        if (now >= 5.0 && at_5 < 0) at_5 = field.stat(ally, f.data->ids().move_speed_mult);
        if (now >= 22.0 && at_22 < 0) at_22 = field.stat(ally, f.data->ids().move_speed_mult);
        buffed = buffed || field.stacks(ally, effect->row.guid) > 0;
        shown = shown || field.shows(ally, effect->row.guid);
    }
    ALERT_EFFECT(buffed, "the ally in the sphere is reached by the aura in the 24 s fight");
    ALERT_EFFECT(shown, "the ally shows the aura in its CurrentEffects array (what its client is told)");
    ALERT_EFFECT(near(at_5, base + 0.2, 1e-6), "the aura raises the ally's MoveSpeedMult by 0.2 while it lasts (%.4f from %.4f)", at_5, base);
    ALERT_EFFECT(field.stacks(far, effect->row.guid) == 0 && near(field.stat(far, f.data->ids().move_speed_mult), base_far, 1e-9), "the ally 16 m away is out of the sphere and keeps its number");
    ALERT_EFFECT(field.stacks(caster, effect->row.guid) == 0, "the shaman is not buffed by its own aura (the hit's mask names its allies)");
    ALERT_EFFECT(at_22 >= 0 && near(at_22, base, 1e-6), "after the 20 s the effect has ended and the ally's MoveSpeedMult is back to its base (%.4f from %.4f)", at_22, base);
    ALERT_EFFECT(!field.shows(ally, effect->row.guid), "... and its CurrentEffects array no longer lists the aura");
}

AOC_REGISTER_TEST(test_frenetic_aura_changes_a_stat_and_ends_with_its_time);

// R3: the same effect ends with the ally's death and with the ally's reset (and the number comes back), while a player's own effect keeps its existing behaviour.
void test_alert_effects_end_with_the_allies_death_and_reset(Fixture& f) {
    std::printf("a buff an ally got from a creature ends with the ally's death and with its reset, and the ally's stats come back; a player's effect is not touched by the reset\n");
    const abilities::AbilityDef* aura = f.catalog->ability_named("GoblinShaman_FreneticAura");
    const abilities::AbilityDef* claw = f.catalog->ability_named("Tidebreaker_Claw");
    const abilities::EffectDef* effect = f.catalog->effect_named("GoblinShaman_FreneticAura_Effect");
    const std::string caster_path = creature_path(f, "/Humanoid/Goblin/GoblinCaster/GoblinShaman/Goblin_Shaman.Default__Goblin_Shaman_C");
    const std::string ally_path = creature_path(f, "/Humanoid/Goblin/Goblin_Bonebinder.Default__Goblin_Bonebinder_C");
    ALERT_EFFECT(!caster_path.empty() && !ally_path.empty() && aura && claw && effect && f.data->ids().move_speed_mult != 0, "the Blueprint table has the goblin shaman and a goblin, and the ability data has the Frenetic Aura, its Claw and its effect");
    if (caster_path.empty() || ally_path.empty() || !aura || !claw || !effect) return;
    const alert_effects::LoadedTree tree = alert_effects::one_alert_tree(caster_path, aura->row.guid, 300, claw->row.guid);
    ALERT_EFFECT(tree.ok, "the shaman's one-alert tree loads (%s)", tree.error.c_str());
    if (!tree.ok) return;
    BanditOpen made;
    ALERT_EFFECT(make_bandit_open(f, tree.tables, caster_path, made), "the world makes an open for the goblin shaman (verdict: %s)", spawns::verdict_name(made.why));
    if (!made.open) return;
    alert_effects::Field field(f, tree.tables);
    ALERT_EFFECT(field.ok(), "the fight's world is made");
    if (!field.ok()) return;
    const uint64_t caster = field.spawn(caster_path, 0, 500, 900, made.open);
    const uint64_t dies = field.spawn(ally_path, 250, 500, 1);
    const uint64_t resets = field.spawn(ally_path, -250, 500, 1);
    ALERT_EFFECT(caster != 0 && dies != 0 && resets != 0, "the shaman and two goblin allies are put in the world");
    if (caster == 0 || dies == 0 || resets == 0) return;
    const double base = field.stat(dies, f.data->ids().move_speed_mult);
    bool both_buffed = false;
    const double dt = 1.0 / 15.0;
    double now = 0;
    for (; now <= 4.0 + 1e-9; now += dt) {
        field.tick(now);
        both_buffed = field.stacks(dies, effect->row.guid) > 0 && field.stacks(resets, effect->row.guid) > 0;
    }
    ALERT_EFFECT(both_buffed, "both allies in the sphere held the aura before the death and the reset");
    std::string why;
    field.world.gm_kill(dies, now, why);
    field.world.gm_reset(resets, now);
    ALERT_EFFECT(field.stacks(dies, effect->row.guid) == 0, "the dead ally's effect is off (it died)");
    ALERT_EFFECT(field.stacks(resets, effect->row.guid) == 0, "the reset ally's creature-given effect is off (its fight ended)");
    ALERT_EFFECT(near(field.stat(resets, f.data->ids().move_speed_mult), base, 1e-6), "the reset ally's MoveSpeedMult is back to its base (%.4f from %.4f)", field.stat(resets, f.data->ids().move_speed_mult), base);
}

AOC_REGISTER_TEST(test_alert_effects_end_with_the_allies_death_and_reset);

// R5 (R10: the Ally bit follows the relation): the Berserker's rage is on the caster itself (Self), and the Yawp (Self | Ally) is on the caster and on the creatures on its
// friendly side around it; a creature the relation calls hostile is not reached by the Ally bit.
void test_self_effects_land_on_the_caster(Fixture& f) {
    std::printf("the Berserker's rage and the Tidebreakers' Yawp land on the caster itself (the Self bit); the Yawp also follows its Ally bit by the relation, and a hostile creature in its sphere is not reached\n");
    const abilities::AbilityDef* rage = f.catalog->ability_named("Minotaur_Berserker_BloodFury");
    const abilities::AbilityDef* yawp = f.catalog->ability_named("Tidebreaker_Yawp");
    const abilities::AbilityDef* claw = f.catalog->ability_named("Tidebreaker_Claw");
    const abilities::EffectDef* rage_effect = f.catalog->effect_named("Minotaur_Berserker_BloodFury_Effect");
    const abilities::EffectDef* yawp_effect = f.catalog->effect_named("Tidebreaker_Yawp_Effect");
    const std::string berserker_path = creature_path(f, "/Humanoid/Minotaur/Berserker/Minotaur_Berserker.Default__Minotaur_Berserker_C");
    const std::string tide_path = creature_path(f, "/Beast/Tidebreaker/Tidebreaker.Default__Tidebreaker_C");
    const std::string tide_other_path = creature_path(f, "/Beast/Tidebreaker/Tidebreaker1.Default__Tidebreaker1_C");
    const std::string skeleton_path = creature_path(f, "/Monster/Undead/Skeleton/Skeleton.Default__Skeleton_C");
    ALERT_EFFECT(!berserker_path.empty() && !tide_path.empty() && !tide_other_path.empty() && !skeleton_path.empty() && rage && yawp && claw && rage_effect && yawp_effect,
          "the Blueprint table has the Minotaur Berserker, two Tidebreakers and a Skeleton, and the ability data has the rage, the Yawp, its Claw and their effects");
    if (berserker_path.empty() || tide_path.empty() || tide_other_path.empty() || skeleton_path.empty() || !rage || !yawp || !claw || !rage_effect || !yawp_effect) return;
    {
        const alert_effects::LoadedTree tree = alert_effects::one_alert_tree(berserker_path, rage->row.guid, 300, claw->row.guid);
        ALERT_EFFECT(tree.ok, "the Berserker's one-alert tree loads (%s)", tree.error.c_str());
        if (!tree.ok) return;
        BanditOpen made;
        ALERT_EFFECT(make_bandit_open(f, tree.tables, berserker_path, made), "the world makes an open for the Berserker (verdict: %s)", spawns::verdict_name(made.why));
        if (!made.open) return;
        alert_effects::Field field(f, tree.tables);
        ALERT_EFFECT(field.ok(), "the Berserker's fight's world is made");
        if (!field.ok()) return;
        const uint64_t caster = field.spawn(berserker_path, 0, 500, 900, made.open);
        const uint64_t goblin = field.spawn(skeleton_path, 250, 500, 1);
        ALERT_EFFECT(caster != 0 && goblin != 0, "the Berserker and a Skeleton beside it are put in the world");
        if (caster == 0 || goblin == 0) return;
        bool self = false;
        const double dt = 1.0 / 15.0;
        for (double now = dt; now <= 18.0 + 1e-9; now += dt) {
            field.tick(now);
            self = self || field.stacks(caster, rage_effect->row.guid) > 0;
        }
        ALERT_EFFECT(self, "the Berserker's rage is on the caster itself while it lasts");
        ALERT_EFFECT(field.stacks(goblin, rage_effect->row.guid) == 0, "a creature beside the Berserker is not reached by its rage (the hit's mask names only itself)");
        ALERT_EFFECT(field.stacks(caster, rage_effect->row.guid) == 0, "after its 15.7 s the rage has ended");
    }
    {
        const alert_effects::LoadedTree tree = alert_effects::one_alert_tree(tide_path, yawp->row.guid, 300, claw->row.guid);
        ALERT_EFFECT(tree.ok, "the Tidebreaker's one-alert tree loads (%s)", tree.error.c_str());
        if (!tree.ok) return;
        BanditOpen made;
        ALERT_EFFECT(make_bandit_open(f, tree.tables, tide_path, made), "the world makes an open for the Tidebreaker (verdict: %s)", spawns::verdict_name(made.why));
        if (!made.open) return;
        alert_effects::Field field(f, tree.tables);
        ALERT_EFFECT(field.ok(), "the Tidebreaker's fight's world is made");
        if (!field.ok()) return;
        const uint64_t caster = field.spawn(tide_path, 0, 500, 900, made.open);
        const uint64_t other = field.spawn(tide_other_path, -250, 500, 1);
        const uint64_t goblin = field.spawn(skeleton_path, 250, 500, 1);
        if (world::WorldActor* actor = field.w.registry.find(goblin)) actor->npc.aggro_off = true;   // (it stands there: it does not start a fight that would disturb the Yawp)
        ALERT_EFFECT(caster != 0 && other != 0 && goblin != 0, "the Yawp's caster, another Tidebreaker in its 20 m sphere and a Skeleton in it are put in the world");
        if (caster == 0 || other == 0 || goblin == 0) return;
        const auto tide_bp = f.motion.find(tide_path), other_bp = f.motion.find(skeleton_path);
        std::vector<int64_t> tide_aligns, other_aligns;
        if (tide_bp) tide_aligns.assign(tide_bp->alignments.begin(), tide_bp->alignments.end());
        if (other_bp) other_aligns.assign(other_bp->alignments.begin(), other_bp->alignments.end());
        const combat::Relation relation = f.data->relation(tide_aligns, other_aligns);
        bool self = false, ally = false;
        const double dt = 1.0 / 15.0;
        for (double now = dt; now <= 2.5 + 1e-9; now += dt) {
            field.tick(now);
            self = self || field.stacks(caster, yawp_effect->row.guid) > 0;
            ally = ally || field.stacks(other, yawp_effect->row.guid) > 0;
        }
        ALERT_EFFECT(self, "the Yawp's effect is on the caster itself (the Self bit of its mask)");
        ALERT_EFFECT(ally, "the Yawp's effect is on the other Tidebreaker in its sphere (the Ally bit)");
        ALERT_EFFECT(relation == combat::Relation::Hostile ? field.stacks(goblin, yawp_effect->row.guid) == 0 : field.stacks(goblin, yawp_effect->row.guid) > 0,
                     "the Skeleton in the same sphere is reached exactly when the relation calls it friendly (the relation is %s)", relation == combat::Relation::Hostile ? "hostile" : "friendly or neutral");
    }
}

AOC_REGISTER_TEST(test_self_effects_land_on_the_caster);

// R11: the Fire Shield's reaction (deals damage to any attacker) runs for a creature's own effect: a player who hits the goblin carrying the aura is hit back by the
// reaction's record.  Before the fix the reaction needed a player caster and was skipped for a creature's effect.
void test_the_fire_shields_reaction_hits_a_players_attack(Fixture& f) {
    std::printf("a goblin shaman carrying its Fire Shield hits a player back when the player hits it (the effect's reaction runs for a creature's effect)\n");
    const abilities::AbilityDef* aura = f.catalog->ability_named("GoblinShaman_FireShieldAura");
    const abilities::AbilityDef* claw = f.catalog->ability_named("Tidebreaker_Claw");
    const abilities::EffectDef* effect = f.catalog->effect_named("GoblinShaman_FireShieldAura_Effect");
    const int64_t reaction = 6064631997936631813;   // GoblinShaman_FireShieldHit_Reaction
    const std::string caster_path = creature_path(f, "/Humanoid/Goblin/GoblinCaster/GoblinShaman/Goblin_Shaman.Default__Goblin_Shaman_C");
    ALERT_EFFECT(!caster_path.empty() && aura && claw && effect && f.catalog->reaction(reaction), "the Blueprint table has the goblin shaman and the ability data has the Fire Shield Aura, its Claw, its effect and its reaction");
    if (caster_path.empty() || !aura || !claw || !effect || !f.catalog->reaction(reaction)) return;
    const alert_effects::LoadedTree tree = alert_effects::one_alert_tree(caster_path, aura->row.guid, 300, claw->row.guid);
    ALERT_EFFECT(tree.ok, "the shaman's one-alert tree loads (%s)", tree.error.c_str());
    if (!tree.ok) return;
    BanditOpen made;
    ALERT_EFFECT(make_bandit_open(f, tree.tables, caster_path, made), "the world makes an open for the shaman (verdict: %s)", spawns::verdict_name(made.why));
    if (!made.open) return;
    alert_effects::Field field(f, tree.tables);
    ALERT_EFFECT(field.ok(), "the fight's world is made");
    if (!field.ok()) return;
    const uint64_t caster = field.spawn(caster_path, 0, 500, 900, made.open);
    ALERT_EFFECT(caster != 0, "the shaman is put in the world");
    if (caster == 0) return;
    const double dt = 1.0 / 15.0;
    bool shielded = false;
    for (double now = dt; now <= 6.0 + 1e-9; now += dt) {
        field.tick(now);
        shielded = shielded || field.stacks(caster, effect->row.guid) > 0;
        if (shielded) break;
    }
    ALERT_EFFECT(shielded, "the Fire Shield Aura is on the shaman before the player's hit");
    if (!shielded) return;
    const abilities::HitDef* hit = nullptr;
    for (const content::AbilityHitRow& r : claw->hits) {
        const abilities::HitDef* h = f.catalog->hit(r.hit_guid);
        if (h && (h->row.applicable_targets & 2) != 0 && !h->mods.empty()) hit = h;
    }
    ALERT_EFFECT(hit != nullptr, "a real damaging hit is used for the player's attack");
    if (!hit) return;
    combat::HitMeta meta;
    meta.source_type = content::kSourceTypeAbility;
    meta.source_guid = claw->row.guid;
    combat::Fighter* shaman = field.world.fighter_of(caster);
    combat::Resolved r;
    std::string why;
    ALERT_EFFECT(shaman && field.world.resolver().resolve(field.player.fighter(), *shaman, *hit, meta, r, why), "the player's hit resolves against the shaman");
    if (!shaman) return;
    const size_t before = field.player.hits.size();
    combat::HitOrigin origin;
    origin.landed_here = true;
    origin.hit = hit;
    origin.source_type = meta.source_type;
    origin.source_guid = meta.source_guid;
    field.world.apply_hit_to_npc(caster, field.player.actor_id(), r, 7.0, 0, &origin);
    bool reacted = false;
    for (size_t i = before; i < field.player.hits.size(); ++i) {
        if (field.player.hits[i].source_type == content::kSourceTypeReaction && field.player.hits[i].source_guid == reaction) reacted = true;
    }
    ALERT_EFFECT(reacted, "the Fire Shield's reaction hit reaches the player");
    field.player.deliver_effect_notes();
}

AOC_REGISTER_TEST(test_the_fire_shields_reaction_hits_a_players_attack);

// R11: the Blood Fury's reaction (drain health from a bleeding target) runs for a creature's own effect: the Berserker that hits a bleeding player heals itself.
void test_the_blood_fury_drains_a_bleeding_target(Fixture& f) {
    std::printf("a Minotaur Berserker carrying its Blood Fury heals from its own hit on a bleeding player (the effect's reaction runs for a creature's effect)\n");
    const abilities::AbilityDef* rage = f.catalog->ability_named("Minotaur_Berserker_BloodFury");
    const abilities::AbilityDef* claw = f.catalog->ability_named("Tidebreaker_Claw");
    const abilities::EffectDef* effect = f.catalog->effect_named("Minotaur_Berserker_BloodFury_Effect");
    const int64_t bleed = 6064629761775576467;   // Status_Bleeding
    const std::string caster_path = creature_path(f, "/Humanoid/Minotaur/Berserker/Minotaur_Berserker.Default__Minotaur_Berserker_C");
    ALERT_EFFECT(!caster_path.empty() && rage && claw && effect && f.catalog->effect(bleed), "the Blueprint table has the Berserker and the ability data has its Blood Fury, its Claw, its effect and the bleeding record");
    if (caster_path.empty() || !rage || !claw || !effect || !f.catalog->effect(bleed)) return;
    const alert_effects::LoadedTree tree = alert_effects::one_alert_tree(caster_path, rage->row.guid, 300, claw->row.guid);
    ALERT_EFFECT(tree.ok, "the Berserker's one-alert tree loads (%s)", tree.error.c_str());
    if (!tree.ok) return;
    BanditOpen made;
    ALERT_EFFECT(make_bandit_open(f, tree.tables, caster_path, made), "the world makes an open for the Berserker (verdict: %s)", spawns::verdict_name(made.why));
    if (!made.open) return;
    alert_effects::Field field(f, tree.tables);
    ALERT_EFFECT(field.ok(), "the fight's world is made");
    if (!field.ok()) return;
    const uint64_t caster = field.spawn(caster_path, 0, 500, 900, made.open);
    ALERT_EFFECT(caster != 0, "the Berserker is put in the world");
    if (caster == 0) return;
    field.rig.pipe->grant_effect(bleed, 1, 0.0);   // the player it hits bleeds
    combat::Fighter* body = field.world.fighter_of(caster);
    if (!body) {
        ALERT_EFFECT(false, "the Berserker's fighter is made");
        return;
    }
    const stats::StatValue* max = body->stats().find(content::kGuidMaxHealth);
    const double half = max ? max->value * 0.5 : 0.0;
    const double dt = 1.0 / 15.0;
    double healed = half;
    bool raged = false;
    for (double now = dt; now <= 14.0 + 1e-9; now += dt) {
        body->stats().set_current(content::kGuidHealth, static_cast<float>(half));
        field.tick(now);
        raged = raged || field.stacks(caster, effect->row.guid) > 0;
        healed = std::max(healed, field.health(caster));
    }
    ALERT_EFFECT(raged, "the Berserker carries its Blood Fury in the fight");
    ALERT_EFFECT(healed > half + 1.0, "its hit on the bleeding player heals it above the half it stood at (%.1f from %.1f)", healed, half);
    field.player.deliver_effect_notes();
}

AOC_REGISTER_TEST(test_the_blood_fury_drains_a_bleeding_target);

// R12: MoveSpeedMult changes the AI's walk speed too while the buff lasts, and back when it ends: the Frenetic Aura raises the ally's chase speed by the stat, not just the
// stat alone.  (Before the fix the speed was set once in make_combatant and never followed the stat.)
void test_a_speed_buff_moves_the_ai_walk(Fixture& f) {
    std::printf("a goblin shaman's Frenetic Aura raises the ally's chase speed with its MoveSpeedMult while it lasts, and the speed comes back when it ends\n");
    const abilities::AbilityDef* aura = f.catalog->ability_named("GoblinShaman_FreneticAura");
    const abilities::AbilityDef* claw = f.catalog->ability_named("Tidebreaker_Claw");
    const abilities::EffectDef* effect = f.catalog->effect_named("GoblinShaman_FreneticAura_Effect");
    const std::string caster_path = creature_path(f, "/Humanoid/Goblin/GoblinCaster/GoblinShaman/Goblin_Shaman.Default__Goblin_Shaman_C");
    const std::string ally_path = creature_path(f, "/Humanoid/Goblin/Goblin_Bonebinder.Default__Goblin_Bonebinder_C");
    ALERT_EFFECT(!caster_path.empty() && !ally_path.empty() && aura && claw && effect && f.data->ids().move_speed_mult != 0, "the Blueprint table has the shaman and the ally and the ability data has the Frenetic Aura, its Claw and its effect");
    if (caster_path.empty() || ally_path.empty() || !aura || !claw || !effect) return;
    const alert_effects::LoadedTree tree = alert_effects::one_alert_tree(caster_path, aura->row.guid, 300, claw->row.guid);
    ALERT_EFFECT(tree.ok, "the shaman's one-alert tree loads (%s)", tree.error.c_str());
    if (!tree.ok) return;
    BanditOpen made;
    ALERT_EFFECT(make_bandit_open(f, tree.tables, caster_path, made), "the world makes an open for the shaman (verdict: %s)", spawns::verdict_name(made.why));
    if (!made.open) return;
    alert_effects::Field field(f, tree.tables);
    ALERT_EFFECT(field.ok(), "the fight's world is made");
    if (!field.ok()) return;
    const uint64_t caster = field.spawn(caster_path, 0, 500, 900, made.open);
    const uint64_t ally = field.spawn(ally_path, 250, 500, 1);
    ALERT_EFFECT(caster != 0 && ally != 0, "the shaman and an ally in its 14 m sphere are put in the world");
    if (caster == 0 || ally == 0) return;
    const auto bp = f.motion.find(ally_path);
    if (!bp) return;
    const double run = std::max({bp->max_run_speed, bp->max_walk_speed, 300.0});
    const auto expected_speed = [&](double mult) { return mult > 1.0 ? run * mult : run; };
    const double base_stat = field.stat(ally, f.data->ids().move_speed_mult);
    const double base_speed = field.world.npc_chase_speed(ally);
    ALERT_EFFECT(near(base_speed, expected_speed(base_stat), 0.5), "the ally's chase speed is the run speed times its MoveSpeedMult at the start (%.1f, stat %.3f)", base_speed, base_stat);
    const double dt = 1.0 / 15.0;
    double buffed_speed = -1, buffed_stat = -1, after_speed = -1;
    for (double now = dt; now <= 24.0 + 1e-9; now += dt) {
        field.tick(now);
        if (buffed_speed < 0 && field.stacks(ally, effect->row.guid) > 0 && now >= 2.0) {
            buffed_speed = field.world.npc_chase_speed(ally);
            buffed_stat = field.stat(ally, f.data->ids().move_speed_mult);
        }
        if (now >= 22.0) after_speed = field.world.npc_chase_speed(ally);
    }
    ALERT_EFFECT(buffed_speed > 0 && buffed_stat > 0, "the ally was reached by the aura and its stat moved (stat %.3f, speed %.1f)", buffed_stat, buffed_speed);
    ALERT_EFFECT(near(buffed_stat, base_stat + 0.2, 1e-6) && near(buffed_speed, expected_speed(buffed_stat), 0.5), "the chase speed follows the buffed stat (%.1f, stat %.3f)", buffed_speed, buffed_stat);
    ALERT_EFFECT(after_speed > 0 && near(after_speed, expected_speed(base_stat), 0.5), "the speed comes back when the buff ends (%.1f from %.1f)", after_speed, base_stat);
}

AOC_REGISTER_TEST(test_a_speed_buff_moves_the_ai_walk);

// R13: the CurrentEffects item a creature's ability puts on a target carries the caster's CharacterGuid as its Instigator and Source, as the player path does (before the fix
// the item's applier was the four zero words).
void test_a_creature_effect_item_names_its_caster(Fixture& f) {
    std::printf("the CurrentEffects item a creature applies names the caster's CharacterGuid as its Instigator and Source\n");
    const abilities::AbilityDef* rally = f.catalog->ability_named("Goblin_Rally");
    const abilities::AbilityDef* claw = f.catalog->ability_named("Tidebreaker_Claw");
    const abilities::EffectDef* effect = f.catalog->effect_named("Goblin_Rally_Effect");
    const std::string caster_path = creature_path(f, "/Humanoid/Goblin/Goblin.Default__Goblin_C");
    const std::string ally_path = creature_path(f, "/Humanoid/Goblin/Goblin_Bonebinder.Default__Goblin_Bonebinder_C");
    ALERT_EFFECT(!caster_path.empty() && !ally_path.empty() && rally && claw && effect, "the Blueprint table has the goblins and the ability data has the Rally, its Claw and its effect");
    if (caster_path.empty() || ally_path.empty() || !rally || !claw || !effect) return;
    const alert_effects::LoadedTree tree = alert_effects::one_alert_tree(caster_path, rally->row.guid, 3.5, claw->row.guid);
    ALERT_EFFECT(tree.ok, "the goblin's one-alert tree loads (%s)", tree.error.c_str());
    if (!tree.ok) return;
    BanditOpen made;
    ALERT_EFFECT(make_bandit_open(f, tree.tables, caster_path, made), "the world makes an open for the goblin (verdict: %s)", spawns::verdict_name(made.why));
    if (!made.open) return;
    alert_effects::Field field(f, tree.tables);
    ALERT_EFFECT(field.ok(), "the fight's world is made");
    if (!field.ok()) return;
    const uint64_t caster = field.spawn(caster_path, 0, 500, 900, made.open);
    const uint64_t ally = field.spawn(ally_path, 250, 500, 1);
    ALERT_EFFECT(caster != 0 && ally != 0, "the caster and an ally are put in the world");
    if (caster == 0 || ally == 0) return;
    const world::WorldActor* caster_actor = field.w.registry.find(caster);
    ALERT_EFFECT(caster_actor != nullptr, "the caster's actor is there");
    if (!caster_actor) return;
    const std::array<uint32_t, 4> words = combat_rig::character_guid_words(*caster_actor);
    const double dt = 1.0 / 15.0;
    bool held = false;
    for (double now = dt; now <= 8.0 + 1e-9; now += dt) {
        field.tick(now);
        held = held || field.stacks(ally, effect->row.guid) > 0;
        if (held) break;
    }
    ALERT_EFFECT(held, "the Rally's effect is on the ally");
    if (!held) return;
    const world::WorldActor* ally_actor = field.w.registry.find(ally);
    ALERT_EFFECT(ally_actor != nullptr, "the ally's actor is there");
    if (!ally_actor) return;
    bool found = false, named = false;
    for (const combat::EffectItemWrite& item : combat::read_effect_items(*ally_actor)) {
        if (item.item.effect_guid != effect->row.guid) continue;
        found = true;
        named = item.applier == words && words != std::array<uint32_t, 4>{0, 0, 0, 0};
    }
    ALERT_EFFECT(found, "the ally's CurrentEffects array holds the Rally's item");
    ALERT_EFFECT(named, "the item names the caster's CharacterGuid as its Instigator and Source (%08x%08x%08x%08x)", words[0], words[1], words[2], words[3]);
}

AOC_REGISTER_TEST(test_a_creature_effect_item_names_its_caster);

// R14: a reset takes off the shield an infinite TempHealth effect added with the effect: the Scorpion's Bury's shield goes with the effect at the reset (before the fix the
// book effect went and the shield stayed).
void test_a_reset_takes_the_shield_of_its_effect_off(Fixture& f) {
    std::printf("the Scorpion's Bury puts an infinite TempHealth shield on it; a reset takes the effect and its shield off\n");
    const abilities::AbilityDef* bury = f.catalog->ability_named("Scorpion_Bury");
    const abilities::AbilityDef* claw = f.catalog->ability_named("Tidebreaker_Claw");
    const abilities::EffectDef* effect = f.catalog->effect_named("Scorpion_Buried");
    const std::string caster_path = creature_path(f, "/Beast/Scorpion/Scorpion_Small_Sandy.Default__Scorpion_Small_Sandy_C");
    ALERT_EFFECT(!caster_path.empty() && bury && claw && effect && effect->row.infinite_duration, "the Blueprint table has the Scorpion and the ability data has its Bury, its Claw and the infinite Scorpion Buried effect");
    if (caster_path.empty() || !bury || !claw || !effect) return;
    const alert_effects::LoadedTree tree = alert_effects::one_alert_tree(caster_path, bury->row.guid, 300, claw->row.guid);
    ALERT_EFFECT(tree.ok, "the Scorpion's one-alert tree loads (%s)", tree.error.c_str());
    if (!tree.ok) return;
    BanditOpen made;
    ALERT_EFFECT(make_bandit_open(f, tree.tables, caster_path, made), "the world makes an open for the Scorpion (verdict: %s)", spawns::verdict_name(made.why));
    if (!made.open) return;
    alert_effects::Field field(f, tree.tables);
    ALERT_EFFECT(field.ok(), "the fight's world is made");
    if (!field.ok()) return;
    const uint64_t caster = field.spawn(caster_path, 0, 500, 900, made.open);
    ALERT_EFFECT(caster != 0, "the Scorpion is put in the world");
    if (caster == 0) return;
    const double dt = 1.0 / 15.0;
    double shielded_at = -1;
    for (double now = dt; now <= 8.0 + 1e-9; now += dt) {
        field.tick(now);
        if (shielded_at < 0 && field.world.shield_of(caster, now) > 0 && field.stacks(caster, effect->row.guid) > 0) shielded_at = now;
        if (shielded_at >= 0) break;
    }
    ALERT_EFFECT(shielded_at >= 0, "the Bury's effect is on the Scorpion with its shield (at %.2f s, shield %.1f)", shielded_at, shielded_at >= 0 ? field.world.shield_of(caster, shielded_at) : 0.0);
    if (shielded_at < 0) return;
    field.world.gm_reset(caster, shielded_at + 1.0);
    ALERT_EFFECT(field.stacks(caster, effect->row.guid) == 0 && field.world.shield_of(caster, shielded_at + 1.0) == 0.0,
                  "the reset took the effect and its shield off (%d stacks, shield %.1f)", field.stacks(caster, effect->row.guid), field.world.shield_of(caster, shielded_at + 1.0));
}

AOC_REGISTER_TEST(test_a_reset_takes_the_shield_of_its_effect_off);
