// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_rally_group_tests.inc
//
//  Registered tests of test_worldd_combat: the state groups of a creature's tree whose later state carries the move's timer (the goblin shamans' Rally and the aura after it,
//  the Withered Shambler's fallback Maw).  The untimed state's task starts the timed state's timer and readies it, so the pair plays in the tree's order and the timer holds both
//  back afterwards; the loaded table is read against the file for exactly the rows the pass changes.  The Flamespeaker's real rows are fought through: the Rally at 40 s of the fight,
//  the Fire Shield Aura after it, the effects on the caster and on the allies the records name, the reaction that strikes a player back, and the log lines that name each of them;
//  with two Flamespeakers and a second player, a reset and a respawn; and a camp of twelve goblins that all rally, whose landing lines grow with the casts, not their square.
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================

namespace rally_group {

/// A cast of the fight as the client saw it begin: when, and which ability.
struct CastAt {
    double  t = 0;
    int64_t ability = 0;
};

/// Runs the body with the world's log going to a string: the lines an alert's cast and its effects write are the ones the test reads back.
struct LogCapture {
    std::ostringstream             stream;
    std::shared_ptr<spdlog::logger> previous;
    LogCapture() {
        previous = spdlog::default_logger();
        auto logger = std::make_shared<spdlog::logger>("rally_group_test", std::make_shared<spdlog::sinks::ostream_sink_mt>(stream));
        logger->set_level(spdlog::level::info);
        spdlog::set_default_logger(logger);
    }
    ~LogCapture() {
        spdlog::default_logger()->flush();
        spdlog::set_default_logger(previous);
    }
    std::string text() {
        spdlog::default_logger()->flush();
        return stream.str();
    }
};

/// Whether a task waits on a cooldown: by its own name, a gate, or a condition that asks for it (`cd(x)`).
bool waits_on(const combat::CreatureAbility& t, const std::string& name) {
    if (!t.task.empty() && t.task == name) return true;
    for (const std::string& gate : t.gates) {
        if (gate == name) return true;
    }
    for (const combat::CondGroup& group : t.groups) {
        for (const combat::CondToken& token : group) {
            if (token.atom.kind == combat::CondAtom::Kind::Cooldown && token.atom.name == name) return true;
        }
    }
    return false;
}

/// Whether the world plays an ability: it hurts enemies, or it is an alert or a state (the three kinds choose_animated lets a creature play).
bool plays(const Fixture& f, const abilities::AbilityDef& d) {
    return combat::hurts_enemies(d, *f.catalog, f.data->ids().health) || combat::is_alert_move(d, *f.catalog, f.data->ids().health) ||
           combat::is_state_move(d, *f.catalog, f.data->ids().health);
}

/// How many lines of the captured log say that the effect record landed on the creature: a landing line names the record's guid, then "lands on", then the creatures it landed on
/// (each as its id and its Blueprint in brackets).
size_t landings_on(const std::string& text, int64_t effect_guid, uint64_t creature, const std::string& leaf) {
    const std::string marker = "(" + std::to_string(effect_guid) + ") lands on ";
    const std::string who = " " + std::to_string(creature) + " (" + leaf + ")";
    size_t n = 0;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const size_t at = line.find(marker);
        if (at != std::string::npos && line.find(who, at + marker.size() - 1) != std::string::npos) ++n;
    }
    return n;
}

/// ... and from one caster: only the landing lines of that caster's alert count.
size_t landings_from(const std::string& text, uint64_t caster, int64_t effect_guid, uint64_t creature, const std::string& leaf) {
    const std::string from = " of caster " + std::to_string(caster) + " (";
    const std::string marker = "(" + std::to_string(effect_guid) + ") lands on ";
    const std::string who = " " + std::to_string(creature) + " (" + leaf + ")";
    size_t n = 0;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const size_t at = line.find(marker);
        if (line.find(from) != std::string::npos && at != std::string::npos && line.find(who, at + marker.size() - 1) != std::string::npos) ++n;
    }
    return n;
}

/// How many lines of the captured log hold every one of `parts`.
size_t lines_with(const std::string& text, const std::vector<std::string>& parts) {
    size_t n = 0;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        bool all = true;
        for (const std::string& p : parts) all = all && line.find(p) != std::string::npos;
        if (all) ++n;
    }
    return n;
}

}  // namespace rally_group

// R1, R3 and R4: the Flamespeaker's real rows.  It rallies at 40 s of its fight, the Fire Shield Aura follows at once as the tree's next state, the Rally's effect lands on the
// allies in its 10 m sphere (not on the caster), the aura's effect and its reaction reach the caster and the allies in its 14 m sphere, a player who hits the shielded caster is
// struck back, the pair comes again only after the 40 s timer, the melee and the Fire Bomb fill the wait, and the log names the alert's cast and each effect's landing.
void test_the_flamespeaker_rallies_then_shields(Fixture& f) {
    std::printf("a Flamespeaker rallies at 40 s of the fight, its Fire Shield Aura follows it, the effects reach the caster and the allies its records name, the shield strikes a player "
                "back, and the log says so\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string caster_path = creature_path(f, "/GoblinFlameShaman/Goblin_FlameShaman_Gravehide.Default__Goblin_FlameShaman_Gravehide_C");
    const std::string ally_path = creature_path(f, "/Humanoid/Goblin/Goblin_Bonebinder.Default__Goblin_Bonebinder_C");
    const abilities::AbilityDef* rally = f.catalog->ability_named("GoblinShaman_Rally");
    const abilities::AbilityDef* aura = f.catalog->ability_named("GoblinShaman_FireShieldAura");
    const abilities::AbilityDef* bludgeon = f.catalog->ability_named("GoblinShaman_Bludgeon");
    const abilities::AbilityDef* bomb = f.catalog->ability_named("GoblinShaman_FireBomb");
    const abilities::EffectDef* rally_effect = f.catalog->effect_named("Goblin_Rally_Effect");
    const abilities::EffectDef* aura_effect = f.catalog->effect_named("GoblinShaman_FireShieldAura_Effect");
    constexpr int64_t kReaction = 6064631997936631813;   // GoblinShaman_FireShieldHit_Reaction
    ALERT_EFFECT(!caster_path.empty() && !ally_path.empty() && rally && aura && bludgeon && bomb && rally_effect && aura_effect && f.catalog->reaction(kReaction),
                 "the Blueprint table has the Gravehide Flamespeaker and a goblin ally, and the ability data has its Rally, its Fire Shield Aura, its Bludgeon, its Fire Bomb, their "
                 "effects and the shield's reaction");
    if (caster_path.empty() || ally_path.empty() || !rally || !aura || !bludgeon || !bomb || !rally_effect || !aura_effect || !f.catalog->reaction(kReaction)) return;
    BanditOpen made;
    ALERT_EFFECT(make_bandit_open(f, tables, caster_path, made), "the world makes an open for the Flamespeaker (verdict: %s)", spawns::verdict_name(made.why));
    if (!made.open) return;
    rally_group::LogCapture log;
    alert_effects::Field field(f, tables, 0, 100);   // the player stands 1.5 m from where the caster is put: the Bludgeon and the Melee Fire Bomb reach him
    ALERT_EFFECT(field.ok(), "the fight's world is made (the captured data and the rig)");
    if (!field.ok()) return;
    const uint64_t caster = field.spawn(caster_path, 0, 250, 900, made.open);
    const uint64_t near_ally = field.spawn(ally_path, 500, 250, 1);       // 5 m from the caster: in the Rally's 10 m sphere and the aura's 14 m
    const uint64_t far_ally = field.spawn(ally_path, 1300, 250, 1);       // 13 m: out of the Rally's sphere, in the aura's
    const uint64_t out_ally = field.spawn(ally_path, 0, 1850, 1);         // 16 m: out of both
    ALERT_EFFECT(caster != 0 && near_ally != 0 && far_ally != 0 && out_ally != 0, "the caster and the near, far and out allies are put in the world");
    if (caster == 0 || near_ally == 0 || far_ally == 0 || out_ally == 0) return;
    for (const uint64_t ally : {near_ally, far_ally, out_ally}) {
        if (world::WorldActor* a = field.w.registry.find(ally)) a->npc.aggro_off = true;   // (they stand and take the buffs: they do not start a fight that disturbs the counts)
    }
    // the run has already written its first cast lines: the alert's own lines must still be written
    field.world.spend_the_cast_log_budget();
    std::vector<rally_group::CastAt> casts;
    GremCastView last;
    bool near_rallied = false, far_rallied = false, caster_rallied = false;
    bool caster_shielded = false, near_shielded = false, far_shielded = false, out_shielded = false, shown = false;
    bool tried = false, reacted = false;
    const abilities::HitDef* hit = nullptr;
    for (const content::AbilityHitRow& r : bludgeon->hits) {
        const abilities::HitDef* h = f.catalog->hit(r.hit_guid);
        if (h && (h->row.applicable_targets & 2) != 0 && !h->mods.empty()) hit = h;
    }
    const double dt = 1.0 / 15.0;
    double now = 0;
    for (now = dt; now <= 92.0 + 1e-9; now += dt) {
        field.tick(now);
        const GremCastView view = grem_cast_view(field.w.registry, caster);
        if (view.montage != 0 && (view.element != last.element || last.montage == 0)) casts.push_back({now, view.ability});
        last = view;
        near_rallied = near_rallied || field.stacks(near_ally, rally_effect->row.guid) > 0;
        far_rallied = far_rallied || field.stacks(far_ally, rally_effect->row.guid) > 0;
        caster_rallied = caster_rallied || field.stacks(caster, rally_effect->row.guid) > 0;
        caster_shielded = caster_shielded || field.stacks(caster, aura_effect->row.guid) > 0;
        near_shielded = near_shielded || field.stacks(near_ally, aura_effect->row.guid) > 0;
        far_shielded = far_shielded || field.stacks(far_ally, aura_effect->row.guid) > 0;
        out_shielded = out_shielded || field.stacks(out_ally, aura_effect->row.guid) > 0;
        shown = shown || field.shows(caster, aura_effect->row.guid);
        // the player hits the shielded caster once: the effect's reaction answers the hit
        if (!tried && caster_shielded && hit) {
            tried = true;
            if (combat::Fighter* body = field.world.fighter_of(caster)) {
                combat::HitMeta meta;
                meta.source_type = content::kSourceTypeAbility;
                meta.source_guid = bludgeon->row.guid;
                combat::Resolved r;
                std::string why;
                if (field.world.resolver().resolve(field.player.fighter(), *body, *hit, meta, r, why)) {
                    combat::HitOrigin origin;
                    origin.landed_here = true;
                    origin.hit = hit;
                    origin.source_type = meta.source_type;
                    origin.source_guid = meta.source_guid;
                    const size_t before = field.player.hits.size();
                    field.world.apply_hit_to_npc(caster, field.player.actor_id(), r, now, 0, &origin);
                    for (size_t i = before; i < field.player.hits.size(); ++i) {
                        if (field.player.hits[i].source_type == content::kSourceTypeReaction && field.player.hits[i].source_guid == kReaction) reacted = true;
                    }
                }
            }
        }
    }
    std::vector<double> rallies, auras, bludgeons, bombs;
    for (const rally_group::CastAt& c : casts) {
        if (c.ability == rally->row.guid) rallies.push_back(c.t);
        if (c.ability == aura->row.guid) auras.push_back(c.t);
        if (c.ability == bludgeon->row.guid) bludgeons.push_back(c.t);
        if (c.ability == bomb->row.guid) bombs.push_back(c.t);
    }
    std::printf("  rallies at:");
    for (const double t : rallies) std::printf(" %.2f", t);
    std::printf(" s; auras at:");
    for (const double t : auras) std::printf(" %.2f", t);
    std::printf(" s; %zu bludgeons and %zu fire bombs\n", bludgeons.size(), bombs.size());
    ALERT_EFFECT(!rallies.empty() && rallies.front() >= 40.0 && rallies.front() < 42.0, "the first Rally comes once the fight is 40 s old (%.2f s)", rallies.empty() ? -1.0 : rallies.front());
    ALERT_EFFECT(!rallies.empty() && !auras.empty() && auras.front() > rallies.front() && auras.front() - rallies.front() >= 5.0 && auras.front() - rallies.front() <= 6.6,
                 "the Fire Shield Aura follows the Rally as the tree's next state: it begins %.2f s after the Rally (its 5.3 s montage, then the chain)",
                 rallies.empty() || auras.empty() ? -1.0 : auras.front() - rallies.front());
    size_t early = 0;
    for (size_t i = 1; i < rallies.size(); ++i) early += rallies[i] - rallies[i - 1] < 40.0 ? 1 : 0;
    ALERT_EFFECT(rallies.size() >= 2 && early == 0, "the pair comes again only after the 40 s its timer starts (%zu rallies, %zu pairs closer than 40 s; the second at %.2f s)", rallies.size(), early,
                 rallies.size() < 2 ? -1.0 : rallies[1]);
    size_t melee_before = 0, melee_between = 0;
    const double first_rally = rallies.empty() ? -1.0 : rallies.front();
    const double first_aura = auras.empty() ? -1.0 : auras.front();
    const double second_rally = rallies.size() < 2 ? 1e9 : rallies[1];
    for (const double t : bludgeons) {
        if (first_rally < 0 || t < first_rally) ++melee_before;
        else if (t > first_aura && t < second_rally) ++melee_between;
    }
    ALERT_EFFECT(melee_before >= 10, "it fights with its Bludgeon before the first Rally (%zu bludgeons)", melee_before);
    ALERT_EFFECT(melee_between >= 10, "... and between the pair and the next Rally, not only rallies (%zu bludgeons)", melee_between);
    // the Fire Bomb is one of its tree's moves, but its projectile's own hit is empty: no stat, no effect, no zone.  Its damage, burn and lingering fire are on the child hit the data
    // chains to that hit, which the world does not follow yet.  The world plays neither an attack nor a show of it, so it waits like any move whose hit nothing shows; the Bludgeon is
    // the melee that fills the wait between the rallies.
    bool bomb_reads = false;
    for (const content::AbilityHitRow& r : bomb->hits) {
        const abilities::HitDef* h = f.catalog->hit(r.hit_guid);
        bomb_reads = bomb_reads || (h && (!h->mods.empty() || !h->effects.empty() || !h->zones.empty()));
    }
    ALERT_EFFECT(!combat::hurts_enemies(*bomb, *f.catalog, f.data->ids().health) && !combat::is_alert_move(*bomb, *f.catalog, f.data->ids().health) &&
                     !combat::is_state_move(*bomb, *f.catalog, f.data->ids().health) && !bomb_reads,
                 "the Fire Bomb's projectile hit carries nothing the world can land (0 stat changes, 0 effects, 0 zones): the world plays neither an attack nor a show of it, and it is "
                 "not cast (%zu casts)", bombs.size());
    ALERT_EFFECT(caster_shielded, "the Fire Shield Aura's effect is on the Flamespeaker itself (the hit's Self bit)");
    ALERT_EFFECT(shown, "the caster shows the aura in its CurrentEffects array, what its clients are told");
    ALERT_EFFECT(near_shielded && far_shielded, "the aura reaches the allies in its 14 m sphere (5 m and 13 m away)");
    ALERT_EFFECT(!out_shielded, "the ally 16 m away is out of the aura's sphere and is not reached");
    ALERT_EFFECT(near_rallied, "the Rally's effect is on the ally in its 10 m sphere");
    ALERT_EFFECT(!far_rallied, "the ally 13 m away is out of the Rally's 10 m sphere and is not reached by it (it gets the aura, not the Rally)");
    ALERT_EFFECT(!caster_rallied, "the Flamespeaker is not reached by its own Rally (the hit's mask names its allies, not itself)");
    ALERT_EFFECT(reacted, "a player who hits the shielded Flamespeaker is struck back by the Fire Shield's reaction");
    // R4: the lines a fight's log gives: the alert's cast and each effect's landing, past the run's first cast lines
    const std::string text = log.text();
    const std::string caster_id = std::to_string(caster);
    const size_t cast_lines = rally_group::lines_with(text, {"plays the alert", "GoblinShaman_Rally", caster_id, "Goblin_FlameShaman_Gravehide"});
    ALERT_EFFECT(cast_lines >= 2, "the log names every Rally cast of the Flamespeaker, past the run's first cast lines (%zu lines)", cast_lines);
    const size_t aura_lines = rally_group::lines_with(text, {"alert GoblinShaman_FireShieldAura of caster " + caster_id + " (Goblin_FlameShaman_Gravehide)", "GoblinShaman_FireShieldAura_Effect"});
    const size_t caster_shield_lines = rally_group::landings_on(text, aura_effect->row.guid, caster, "Goblin_FlameShaman_Gravehide");
    ALERT_EFFECT(aura_lines >= 1 && caster_shield_lines >= 1, "a line names the aura's effect record on the Flamespeaker itself, with its caster and its Blueprint (%zu lines)", caster_shield_lines);
    const size_t near_shield_lines = rally_group::landings_on(text, aura_effect->row.guid, near_ally, "Goblin_Bonebinder");
    ALERT_EFFECT(near_shield_lines >= 1, "a line names the aura's effect record on the ally, with the caster's Blueprint and the ally's (%zu lines)", near_shield_lines);
    const size_t near_rally_lines = rally_group::landings_on(text, rally_effect->row.guid, near_ally, "Goblin_Bonebinder");
    ALERT_EFFECT(near_rally_lines >= 1, "a line names the Rally's effect record on the ally (%zu lines)", near_rally_lines);
    const size_t caster_rally_lines = rally_group::landings_on(text, rally_effect->row.guid, caster, "Goblin_FlameShaman_Gravehide");
    ALERT_EFFECT(caster_rally_lines == 0, "no Rally landing line names the Flamespeaker itself (%zu lines)", caster_rally_lines);
}

AOC_REGISTER_TEST(test_the_flamespeaker_rallies_then_shields);

// R2: every one of the table's state groups (a 0 s row of a task and a timed row of the same task) under the same rule: the untimed row starts the timed row's timer when the
// rows do not already link; a pair the rows already link is left; a 0 s row outside the fight, a row the tree switches off, and an ability the world does not play are not
// changed in what the player sees.  The reader's own row for the Flamespeaker (its starts and next written out) loads the same way.
void test_the_state_groups_of_the_ability_table(Fixture& f) {
    std::printf("the state groups of the creature table: the untimed row starts the timed row's timer unless the rows already link, and 13 Blueprints cast differently\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    size_t groups = 0, changed = 0, pre_linked = 0, outside = 0, switched_off = 0, unplayable = 0;
    const combat::CreatureAbility* flame_a = nullptr;
    const combat::CreatureAbility* flame_b = nullptr;
    const combat::CreatureAbility* shaman_a = nullptr;
    const combat::CreatureAbility* shaman_b = nullptr;
    const combat::CreatureAbility* maw_a = nullptr;
    const combat::CreatureAbility* basic_a = nullptr;
    const combat::CreatureAbility* basic_b = nullptr;
    const std::string flame_path = creature_path(f, "/GoblinFlameShaman/Goblin_FlameShaman_Gravehide.Default__Goblin_FlameShaman_Gravehide_C");
    const std::string shaman_path = creature_path(f, "/GoblinShaman/Goblin_Shaman.Default__Goblin_Shaman_C");
    const std::string maw_path = creature_path(f, "/Shambler/Withered_Shambler.Default__Withered_Shambler_C");
    const std::string basic_path = creature_path(f, "/Najash_Tracker_RotationTest.Default__Najash_Tracker_RotationTest_C");
    const abilities::AbilityDef* flame_rally = f.catalog->ability_named("GoblinShaman_Rally");
    const abilities::AbilityDef* flame_aura = f.catalog->ability_named("GoblinShaman_FireShieldAura");
    const abilities::AbilityDef* shaman_aura = f.catalog->ability_named("GoblinShaman_FreneticAura");
    const abilities::AbilityDef* maw = f.catalog->ability_named("Withered_Shambler_WitheredMaw");
    const abilities::AbilityDef* sweep = f.catalog->ability_named("Najash_Hunter_SpearSweep");
    const abilities::AbilityDef* thrust = f.catalog->ability_named("Najash_Hunter_SpearThrust");
    ALERT_EFFECT(flame_rally && flame_aura && shaman_aura && maw && sweep && thrust, "the ability data has the abilities of the Rally, Maw and Basic groups");
    if (!flame_rally || !flame_aura || !shaman_aura || !maw || !sweep || !thrust) return;
    for (const auto& [archetype, rows] : tables.by_kind()) {
        std::map<std::string, std::vector<const combat::CreatureAbility*>> by_task;
        for (const combat::CreatureAbility& t : rows) {
            if (!t.task.empty()) by_task[t.task].push_back(&t);
        }
        for (auto& [task, list] : by_task) {
            std::vector<const combat::CreatureAbility*> zero, timed;
            for (const combat::CreatureAbility* t : list) {
                if (t->cooldown == 0) zero.push_back(t);
                else if (t->cooldown > 0) timed.push_back(t);
            }
            if (zero.empty() || timed.empty()) continue;
            ++groups;
            const combat::CreatureAbility* a = nullptr;
            const combat::CreatureAbility* b = nullptr;
            for (const combat::CreatureAbility* t : zero) {
                if (t->fight) a = t;
            }
            for (const combat::CreatureAbility* t : timed) {
                if (t->fight) b = t;
            }
            if (!a || !b) {
                ++outside;   // the 0 s rows are the opening or idle states of the same task name: the fight never plays them (the bears' rear-up group, the Reaper's soultheft)
                continue;
            }
            if (archetype == flame_path && a->ability == flame_rally->row.guid) {
                flame_a = a;
                flame_b = b;
            }
            if (archetype == shaman_path && a->ability == flame_rally->row.guid) {
                shaman_a = a;
                shaman_b = b;
            }
            if (archetype == maw_path && a->ability == maw->row.guid) maw_a = a;
            if (archetype == basic_path && a->ability == sweep->row.guid) {
                basic_a = a;
                basic_b = b;
            }
            if (a->off || a->unknown) {
                ++switched_off;   // (a parameter of the tree switches the 0 s row off, or a condition the table cannot say keeps it: it is never played)
                continue;
            }
            // the rows already link through a cooldown of their own that the timed row waits on (the shamans' RallyLastUsed): the pair and its timer already work
            bool pre = false;
            for (const combat::CooldownStart& started : a->starts) pre = pre || (started.name != b->task && rally_group::waits_on(*b, started.name));
            if (pre) {
                ++pre_linked;
                continue;
            }
            ++changed;
            if (const abilities::AbilityDef* d = f.catalog->ability(a->ability); !d || !rally_group::plays(f, *d)) ++unplayable;
        }
    }
    ALERT_EFFECT(groups == 62, "the table has the 62 Blueprints with a 0 s row and a timed row of the same task (%zu)", groups);
    ALERT_EFFECT(outside == 27, "in 27 of them the 0 s rows are outside the fight (the bears' rear-up chain and the Reaper's soultheft): the world never plays them and they are left as they are (%zu)", outside);
    ALERT_EFFECT(switched_off == 9, "in 9 of them the tree switches the 0 s row off (the bandit musicians that do not use the song): left as they are (%zu)", switched_off);
    ALERT_EFFECT(pre_linked == 10, "in 10 of them the rows already link through their own cooldown (the shamans' RallyLastUsed): the pair and the 40 s timer already work (%zu)", pre_linked);
    ALERT_EFFECT(changed == 16, "in 16 of them the untimed row now starts the timed row's timer (%zu)", changed);
    ALERT_EFFECT(unplayable == 3, "three of those 16 are a heal the world does not play (the bandit musician's Lustful Serenade): their timer changes nothing the player sees (%zu)", unplayable);
    ALERT_EFFECT(changed - unplayable == 13, "13 Blueprints cast differently: the 9 goblin shamans' Rally groups, the 3 Withered Shamblers' Maw and the one development Blueprint's spears (%zu)", changed - unplayable);
    ALERT_EFFECT(flame_a && flame_b && flame_a->starts.size() == 1 && flame_a->starts[0].name == "Rally" && near(flame_a->starts[0].low, 40.0, 1e-6) &&
                     flame_b->cooldown == 40.0f && std::find(flame_a->next.begin(), flame_a->next.end(), flame_aura->row.guid) != flame_a->next.end(),
                 "the Flamespeaker's untimed Rally starts the aura's 40 s timer once and readies the aura (the aura also starts its own 40 s cooldown).");
    ALERT_EFFECT(shaman_a && shaman_b && std::find(shaman_a->next.begin(), shaman_a->next.end(), shaman_aura->row.guid) != shaman_a->next.end(),
                 "a shaman whose Rally already starts RallyLastUsed leaves it to that cooldown: its pair was already linked and is not changed");
    ALERT_EFFECT(maw_a && maw_a->starts.size() == 1 && maw_a->starts[0].name == "Maw" && near(maw_a->starts[0].low, 4.0, 1e-6) && maw_a->next.empty(),
                 "the Withered Shambler's fallback Maw starts the timed row's 4 s timer and does not chain the row whose condition the table cannot say");
    ALERT_EFFECT(basic_a && basic_b && basic_a->starts.size() == 1 && basic_a->starts[0].name == "Basic" && near(basic_a->starts[0].low, 1.0, 1e-6) &&
                     std::find(basic_a->next.begin(), basic_a->next.end(), thrust->row.guid) != basic_a->next.end(),
                 "the development Blueprint's Spear Sweep starts the timed Spear Thrust's 1 s timer and readies it");
    // the row the reader writes for the Flamespeaker (its starts and next written out) loads the same way: the loader does not add a second one
    std::string dir = NPC_MOTION_PATH;
    dir.erase(dir.find_last_of("/\\") + 1);
    std::string error;
    sequences::RowEdit edit;
    edit.ability = flame_rally->row.guid;
    edit.fields = {{14, "Rally=40"}};
    edit.next = std::to_string(flame_aura->row.guid);
    const std::string patched = sequences::table_with_next(dir, flame_path, {edit}, error, sequences::NextColumn::Keep);
    ALERT_EFFECT(!patched.empty(), "the Flamespeaker's rows as the reader writes them (the Rally starts Rally=40 and readies the aura) are written (%s)", error.c_str());
    if (!patched.empty()) {
        combat::NpcAbilityData loaded;
        const bool ok = loaded.load(patched, dir + "npc_montages.tsv", dir + "npc_targeting.tsv", error);
        ALERT_EFFECT(ok, "the reader's rows load (%s)", error.c_str());
        const std::vector<combat::CreatureAbility>* rows = ok ? loaded.abilities_of(flame_path) : nullptr;
        const combat::CreatureAbility* a = nullptr;
        if (rows) {
            for (const combat::CreatureAbility& t : *rows) {
                if (t.ability == flame_rally->row.guid && t.fight) a = &t;
            }
        }
        ALERT_EFFECT(a && a->starts.size() == 1 && a->starts[0].name == "Rally" && near(a->starts[0].low, 40.0, 1e-6) &&
                         std::find(a->next.begin(), a->next.end(), flame_aura->row.guid) != a->next.end(),
                     "the reader's row keeps exactly one 40 s start and the aura in its next (no second start is added)");
        std::error_code ec;
        std::filesystem::remove(patched, ec);
    }
}

AOC_REGISTER_TEST(test_the_state_groups_of_the_ability_table);

// R2, the loaded table: the pass changes the starts of exactly the rows the 62 groups' census names and the chains of those that can chain, and no other row of the table.  The file's
// own rows are read back: a row outside the list loads with the starts and the chain the file gives it; a row of the list carries the timed twin's timer once and (when the twin can
// play) the twin's ability in its chain, whether the file already says so (a reader that writes the pass itself) or not.
namespace rally_group {

/// A row of the creature table as the file has it, before the loader's passes: what the state-group pass may change.
struct FileRow {
    std::vector<combat::CooldownStart> starts;
    std::vector<int64_t>               next;
    int                                seen = 0;
};

/// The fields of a line between two separators (empty fields kept).
std::vector<std::string> split_on(const std::string& text, char separator) {
    std::vector<std::string> out;
    size_t from = 0;
    while (true) {
        const size_t at = text.find(separator, from);
        if (at == std::string::npos) {
            out.push_back(text.substr(from));
            break;
        }
        out.push_back(text.substr(from, at - from));
        from = at + 1;
    }
    return out;
}

std::string file_key(const std::string& archetype, int64_t ability, int order) { return archetype + "|" + std::to_string(ability) + "|" + std::to_string(order); }

/// The file's rows by Blueprint path, ability and order; false when it does not read.
bool read_file_rows(const std::string& path, std::map<std::string, FileRow>& out, size_t& rows) {
    std::ifstream in(path, std::ios::binary);
    std::string line;
    if (!in || !std::getline(in, line)) return false;
    rows = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const std::vector<std::string> c = split_on(line, '\t');
        if (c.size() < 17) return false;
        FileRow r;
        for (const std::string& entry : split_on(c[14], ';')) {
            const size_t eq = entry.find('=');
            if (entry.empty() || eq == std::string::npos) continue;
            combat::CooldownStart s;
            s.name = entry.substr(0, eq);
            const std::string span = entry.substr(eq + 1);
            const size_t tilde = span.find('~');
            s.low = std::strtod(span.c_str(), nullptr);
            s.high = tilde == std::string::npos ? s.low : std::strtod(span.c_str() + tilde + 1, nullptr);
            r.starts.push_back(s);
        }
        if (c.size() > 17) {
            for (const std::string& n : split_on(c[17], ';')) {
                if (!n.empty()) r.next.push_back(std::strtoll(n.c_str(), nullptr, 10));
            }
        }
        FileRow& slot = out[file_key(c[0], std::strtoll(c[1].c_str(), nullptr, 10), std::atoi(c[5].c_str()))];
        ++slot.seen;
        slot.starts = r.starts;
        slot.next = r.next;
        ++rows;
    }
    return true;
}

bool same_starts(const std::vector<combat::CooldownStart>& a, const std::vector<combat::CooldownStart>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].name != b[i].name || !near(a[i].low, b[i].low, 1e-6) || !near(a[i].high, b[i].high, 1e-6)) return false;
    }
    return true;
}

/// A row the pass changes: the 0 s row of a task, the task's timer its timed twin carries, and the twin's ability when the twin can play (0: it is held, so nothing chains to it).
struct PassRow {
    const char* leaf;
    int64_t     ability;
    int         order;
    const char* task;
    double      cooldown;
    int64_t     twin;
};

constexpr int64_t kRallyAbility = 6064629982192795650, kFlameAura = 6064631997786095616, kShamanTwin = 6064629982195220483, kSweep = 6064632228125671424, kThrust = 6064632227747004416;
constexpr int64_t kMaw = 6064633610525802509, kSerenade = 6064632138952278043;
const PassRow kPassRows[] = {
    {"Goblin_FlameShaman", kRallyAbility, 15, "Rally", 40, kFlameAura},
    {"Goblin_FlameShaman1_Dreadtide", kRallyAbility, 15, "Rally", 40, kFlameAura},
    {"Goblin_FlameShaman2_Dreadtide", kRallyAbility, 15, "Rally", 40, kFlameAura},
    {"Goblin_FlameShaman_Bonebinder", kRallyAbility, 15, "Rally", 40, kFlameAura},
    {"Goblin_FlameShaman_Dreadtide", kRallyAbility, 15, "Rally", 40, kFlameAura},
    {"Goblin_FlameShaman_Gravehide", kRallyAbility, 15, "Rally", 40, kFlameAura},
    {"Goblin_RareSpawn_Boog", kRallyAbility, 15, "Rally", 40, kFlameAura},
    {"Goblin_HighFlameShaman_LawlessTropics", kRallyAbility, 21, "Rally", 40, kShamanTwin},
    {"Goblin_Shaman_RareSpawn_TheHighJubjub", kRallyAbility, 21, "Rally", 40, kShamanTwin},
    {"Najash_Tracker_RotationTest", kSweep, 24, "Basic", 1, kThrust},
    {"Withered_Shambler", kMaw, 30, "Maw", 4, 0},
    {"Withered_Shambler1", kMaw, 30, "Maw", 4, 0},
    {"Withered_Shambler2", kMaw, 30, "Maw", 4, 0},
    {"Bandit_Musician2", kSerenade, 42, "Serenade", 7, 0},
    {"Bandit_Musician2_Dunir", kSerenade, 42, "Serenade", 7, 0},
    {"Bandit_Musician1_Pirate", kSerenade, 42, "Serenade", 7, 0},
};

}  // namespace rally_group

void test_the_state_group_pass_changes_exactly_the_rows_it_names(Fixture&) {
    std::printf("the state groups' pass: 16 rows of the creature table carry the timer of the timed state after them, 10 of them chain to it, and every other row loads as its file says\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    std::string dir = NPC_MOTION_PATH;
    dir.erase(dir.find_last_of("/\\") + 1);
    std::map<std::string, rally_group::FileRow> file;
    size_t file_rows = 0;
    ALERT_EFFECT(rally_group::read_file_rows(dir + "npc_abilities.tsv", file, file_rows), "the creature table's own rows are read back (%zu rows)", file_rows);
    if (file.empty()) return;
    size_t loaded_rows = 0, missing = 0, ambiguous = 0, others = 0, others_changed = 0, listed = 0, listed_right = 0, chained = 0;
    std::set<std::string> listed_seen;
    for (const auto& [archetype, rows] : tables.by_kind()) {
        const std::string leaf = spawns::blueprint_leaf(archetype);
        for (const combat::CreatureAbility& t : rows) {
            ++loaded_rows;
            const auto it = file.find(rally_group::file_key(archetype, t.ability, t.order));
            if (it == file.end()) {
                ++missing;
                continue;
            }
            const rally_group::FileRow& raw = it->second;
            if (raw.seen != 1) ++ambiguous;
            const rally_group::PassRow* wanted = nullptr;
            for (const rally_group::PassRow& p : rally_group::kPassRows) {
                if (leaf == p.leaf && t.ability == p.ability && t.order == p.order) wanted = &p;
            }
            if (!wanted) {
                ++others;
                if (!rally_group::same_starts(t.starts, raw.starts) || t.next != raw.next) {
                    ++others_changed;
                    if (others_changed <= 5) std::printf("    (a row the pass must leave changed: %s ability %lld order %d)\n", leaf.c_str(), static_cast<long long>(t.ability), t.order);
                }
                continue;
            }
            ++listed;
            listed_seen.insert(leaf);
            size_t own = 0;
            bool own_right = false;
            for (const combat::CooldownStart& s : t.starts) {
                if (s.name != wanted->task) continue;
                ++own;
                own_right = near(s.low, wanted->cooldown, 1e-6) && near(s.high, wanted->cooldown, 1e-6);
            }
            bool file_has = false;
            for (const combat::CooldownStart& s : raw.starts) file_has = file_has || s.name == wanted->task;
            const bool starts_right = own == 1 && own_right && t.starts.size() == raw.starts.size() + (file_has ? 0 : 1);
            size_t twins = 0;
            for (const int64_t n : t.next) twins += n == wanted->twin ? 1 : 0;
            bool file_chains = false;
            for (const int64_t n : raw.next) file_chains = file_chains || n == wanted->twin;
            const bool next_right = wanted->twin != 0 ? (twins == 1 && t.next.size() == raw.next.size() + (file_chains ? 0 : 1)) : t.next == raw.next;
            chained += wanted->twin != 0 && twins == 1 ? 1 : 0;
            if (starts_right && next_right) ++listed_right;
            else std::printf("    (a row the pass must change: %s ability %lld order %d: %zu start(s) of %s, %zu chain(s) to the twin)\n", leaf.c_str(), static_cast<long long>(t.ability), t.order, own,
                             wanted->task, twins);
        }
    }
    ALERT_EFFECT(loaded_rows == file_rows && missing == 0 && ambiguous == 0, "every row of the file loads once (%zu of %zu rows, %zu not found, %zu doubled)", loaded_rows, file_rows, missing, ambiguous);
    ALERT_EFFECT(listed == 16 && listed_seen.size() == 16, "the 16 rows of the 13 Blueprints that cast differently and the 3 bandit musicians' heals are all in the table (%zu rows of %zu Blueprints)", listed, listed_seen.size());
    ALERT_EFFECT(listed_right == listed, "each of them carries the timed twin's timer exactly once, with the twin's cooldown, and chains to the twin only when it can play (%zu of %zu right)", listed_right, listed);
    ALERT_EFFECT(chained == 10, "ten of them chain to their twin: the nine goblin shamans' Rally rows and the development Blueprint's Spear Sweep (%zu)", chained);
    ALERT_EFFECT(others > 6000 && others_changed == 0, "no other row's starts or chain differ from the file's (%zu other rows, %zu differ)", others, others_changed);
}

AOC_REGISTER_TEST(test_the_state_group_pass_changes_exactly_the_rows_it_names);

// R2 on rows of a table written here: the pass is the rule of the loader, not of the 62 groups.  A 0 s row and a later timed row of one task start the timer and chain (the next row
// is another move); the same ability under both rows never chains to itself; a timed row before the 0 s row gives it the timer and no chain; a pair the file already links is left.
void test_the_state_group_rule_on_a_table_written_here(Fixture&) {
    std::printf("the state groups' rule on rows written here: the timer from the twin, the chain to another move only, no chain to the same move or to an earlier row, and a linked pair left alone\n");
    namespace fs = std::filesystem;
    const std::string pair = "/Test/GroupPair.Default__GroupPair_C", same = "/Test/GroupSame.Default__GroupSame_C", before = "/Test/GroupBefore.Default__GroupBefore_C",
                      linked = "/Test/GroupLinked.Default__GroupLinked_C";
    std::error_code ec;
    const fs::path tmp = fs::temp_directory_path() / ("aoc_state_group_rule_" + unique_temp_suffix());
    fs::create_directories(tmp, ec);
    const std::vector<std::string> rows = {
        self_effects::row(pair, 101, "Pair", 0, 0, 10, -1, -1, -1, "", 0, 1, "", "", "", "", -1),
        self_effects::row(pair, 102, "Pair", 5, 0, 20, -1, -1, -1, "", 0, 1, "", "", "", "", -1),
        self_effects::row(same, 201, "Same", 0, 0, 10, -1, -1, -1, "", 0, 1, "", "", "", "", -1),
        self_effects::row(same, 201, "Same", 5, 0, 20, -1, -1, -1, "", 0, 1, "", "", "", "", -1),
        self_effects::row(before, 301, "Before", 0, 0, 30, -1, -1, -1, "", 0, 1, "", "", "", "", -1),
        self_effects::row(before, 302, "Before", 4, 0, 20, -1, -1, -1, "", 0, 1, "", "", "", "", -1),
        self_effects::row(linked, 401, "Linked", 0, 0, 10, -1, -1, -1, "", 0, 1, "", "", "LinkedLastUsed=10", "", -1),
        self_effects::row(linked, 402, "Linked", 6, 0, 20, -1, -1, -1, "", 0, 1, "c0:cd(LinkedLastUsed)", "", "", "", -1),
    };
    combat::NpcAbilityData tables;
    const bool ok = self_effects::load_self_table((tmp / "abilities.tsv").string(), rows, "the table written here reads", tables);
    fs::remove_all(tmp, ec);
    if (!ok) return;
    const auto row_of = [&](const std::string& path, int64_t ability) -> const combat::CreatureAbility* {
        const std::vector<combat::CreatureAbility>* list = tables.abilities_of(path);
        if (!list) return nullptr;
        for (const combat::CreatureAbility& t : *list) {
            if (t.ability == ability) return &t;
        }
        return nullptr;
    };
    const combat::CreatureAbility* p = row_of(pair, 101);
    ALERT_EFFECT(p && p->starts.size() == 1 && p->starts[0].name == "Pair" && near(p->starts[0].low, 5.0, 1e-6) && p->next == std::vector<int64_t>{102},
                 "the 0 s row of a pair starts the timed row's 5 s timer and chains to the other move");
    const combat::CreatureAbility* s = row_of(same, 201);
    ALERT_EFFECT(s && s->starts.size() == 1 && s->starts[0].name == "Same" && s->next.empty(), "the same move under both rows gets the timer and no chain to itself (%zu chain(s))", s ? s->next.size() : size_t(0));
    const combat::CreatureAbility* b = row_of(before, 301);
    ALERT_EFFECT(b && b->starts.size() == 1 && b->starts[0].name == "Before" && near(b->starts[0].low, 4.0, 1e-6) && b->next.empty(), "a timed row before the 0 s row gives the timer and is not chained to");
    const combat::CreatureAbility* l = row_of(linked, 401);
    ALERT_EFFECT(l && l->starts.size() == 1 && l->starts[0].name == "LinkedLastUsed" && l->next.empty(), "a pair the rows already link through the timed row's own condition is left as it is");
}

AOC_REGISTER_TEST(test_the_state_group_rule_on_a_table_written_here);

// R1 and R3 with company: two Flamespeakers side by side and a second player in the fight.  Each Flamespeaker keeps its own pair and its own 40 s timer, each is on the other's side
// (the other's Rally and aura reach it), and a second player who hits a shielded one is struck back as the first is.  Then a reset and a respawn: the Flamespeaker that resets and the
// one that is made again begin the fight clock again, so neither rallies before 40 s of its new fight and both go on to rally and shield.
namespace rally_group {

/// One Flamespeaker's casts: when each began, by ability.
struct Pairs {
    self_effects::CastLog log;
    int64_t               rally = 0, aura = 0;

    /// When each Rally cast began.
    std::vector<double> rallies() const {
        std::vector<double> out;
        for (const self_effects::Cast& c : log.casts) {
            if (c.ability == rally) out.push_back(c.t);
        }
        return out;
    }
    /// How many auras begin at a time that no Rally of this Flamespeaker precedes by 5.0 to 6.6 s.
    size_t auras_without_rally() const {
        size_t n = 0;
        for (const self_effects::Cast& c : log.casts) {
            if (c.ability != aura) continue;
            bool led = false;
            for (const self_effects::Cast& r : log.casts) led = led || (r.ability == rally && c.t - r.t >= 5.0 && c.t - r.t <= 6.6);
            n += led ? 0 : 1;
        }
        return n;
    }
};

}  // namespace rally_group

void test_two_flamespeakers_and_a_second_player_keep_their_own_pairs(Fixture& f) {
    std::printf("two Flamespeakers side by side, two players, a reset and a respawn: each keeps its own pair and timer, each is on the other's side, and the clock begins again after a reset or a respawn\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string caster_path = creature_path(f, "/GoblinFlameShaman/Goblin_FlameShaman_Gravehide.Default__Goblin_FlameShaman_Gravehide_C");
    const abilities::AbilityDef* rally = f.catalog->ability_named("GoblinShaman_Rally");
    const abilities::AbilityDef* aura = f.catalog->ability_named("GoblinShaman_FireShieldAura");
    const abilities::AbilityDef* bludgeon = f.catalog->ability_named("GoblinShaman_Bludgeon");
    const abilities::EffectDef* rally_effect = f.catalog->effect_named("Goblin_Rally_Effect");
    const abilities::EffectDef* aura_effect = f.catalog->effect_named("GoblinShaman_FireShieldAura_Effect");
    constexpr int64_t kReaction = 6064631997936631813;   // GoblinShaman_FireShieldHit_Reaction
    ALERT_EFFECT(!caster_path.empty() && rally && aura && bludgeon && rally_effect && aura_effect && f.catalog->reaction(kReaction),
                 "the Blueprint table has the Gravehide Flamespeaker and the ability data has its Rally, Fire Shield Aura, Bludgeon, their effects and the shield's reaction");
    if (caster_path.empty() || !rally || !aura || !bludgeon || !rally_effect || !aura_effect || !f.catalog->reaction(kReaction)) return;
    BanditOpen made;
    ALERT_EFFECT(make_bandit_open(f, tables, caster_path, made), "the world makes an open for the Flamespeaker (verdict: %s)", spawns::verdict_name(made.why));
    if (!made.open) return;
    const auto body = f.motion.find(caster_path);
    rally_group::LogCapture log;
    alert_effects::Field field(f, tables, 0, 100);
    ALERT_EFFECT(field.ok() && body, "the fight's world is made (the captured data and the rig)");
    if (!field.ok() || !body) return;
    Rig rig2(4, 20, {});
    ALERT_EFFECT(rig2.ok(), "a second player's rig is made");
    if (!rig2.ok()) return;
    combat_rig::FakePlayer player2(rig2, 78, {-200, 100, 1098});
    field.world.add_player(&player2);
    const uint64_t a = field.spawn(caster_path, -300, 250, 900, made.open);   // 6 m from the other: inside each one's Rally sphere of 10 m and aura sphere of 14 m
    const uint64_t b = field.spawn(caster_path, 300, 250, 900, made.open);
    ALERT_EFFECT(a != 0 && b != 0, "the two Flamespeakers are put in the world");
    if (a == 0 || b == 0) return;
    rally_group::Pairs pa, pb, pc;
    pa.rally = pb.rally = pc.rally = rally->row.guid;
    pa.aura = pb.aura = pc.aura = aura->row.guid;
    uint64_t c = 0;   // the one made again where the first stood
    bool reset = false, killed = false, tried2 = false, reacted2 = false;
    const abilities::HitDef* hit = nullptr;
    for (const content::AbilityHitRow& r : bludgeon->hits) {
        const abilities::HitDef* h = f.catalog->hit(r.hit_guid);
        if (h && (h->row.applicable_targets & 2) != 0 && !h->mods.empty()) hit = h;
    }
    constexpr double kReset = 60.0, kKill = 62.0, kRespawn = 64.0, kEnd = 140.0;
    const double dt = 1.0 / 15.0;
    for (double now = dt; now <= kEnd + 1e-9; now += dt) {
        field.tick(now);
        if (const stats::StatValue* max = rig2.cs->find(content::kGuidMaxHealth)) rig2.cs->set_current(content::kGuidHealth, max->value);
        const combat::Vec3 at2 = player2.rig_fighter().position();
        field.w.npcs->note_player(78, at2.x, at2.y, now);
        if (!reset && now >= kReset) reset = field.world.gm_reset(b, now) != combat::CombatWorld::GmReset::Dead;
        std::string why;
        if (!killed && now >= kKill) killed = field.world.gm_kill(a, now, why);
        if (c == 0 && now >= kRespawn) c = field.w.spawn(caster_path, -300, 250, 7, 900, body->half_height, made.open, &*body);
        pa.log.look(field.w.registry, a, now);
        pb.log.look(field.w.registry, b, now);
        if (c != 0) pc.log.look(field.w.registry, c, now);
        // the second player hits the shielded Flamespeaker once: the reaction answers him
        if (!tried2 && now < kReset && field.stacks(b, aura_effect->row.guid) > 0 && hit) {
            tried2 = true;
            if (combat::Fighter* target = field.world.fighter_of(b)) {
                combat::HitMeta meta;
                meta.source_type = content::kSourceTypeAbility;
                meta.source_guid = bludgeon->row.guid;
                combat::Resolved r;
                std::string why2;
                if (field.world.resolver().resolve(player2.fighter(), *target, *hit, meta, r, why2)) {
                    combat::HitOrigin origin;
                    origin.landed_here = true;
                    origin.hit = hit;
                    origin.source_type = meta.source_type;
                    origin.source_guid = meta.source_guid;
                    const size_t before = player2.hits.size();
                    field.world.apply_hit_to_npc(b, player2.actor_id(), r, now, 0, &origin);
                    for (size_t i = before; i < player2.hits.size(); ++i) {
                        if (player2.hits[i].source_type == content::kSourceTypeReaction && player2.hits[i].source_guid == kReaction) reacted2 = true;
                    }
                }
            }
        }
    }
    const auto show = [&](const char* who, const rally_group::Pairs& p) {
        std::printf("  %s rallies at:", who);
        for (const double t : p.rallies()) std::printf(" %.2f", t);
        std::printf(" s; auras at:");
        for (const self_effects::Cast& cast : p.log.casts) {
            if (cast.ability == aura->row.guid) std::printf(" %.2f", cast.t);
        }
        std::printf(" s\n");
    };
    show("the first", pa);
    show("the second", pb);
    show("the one made again", pc);
    const std::vector<double> ra = pa.rallies(), rb = pb.rallies(), rc = pc.rallies();
    ALERT_EFFECT(reset && killed && c != 0, "the second Flamespeaker is reset at %.0f s, the first is killed at %.0f s and one is made again at %.0f s", kReset, kKill, kRespawn);
    ALERT_EFFECT(!ra.empty() && ra.front() >= 40.0 && ra.front() < 44.0 && !rb.empty() && rb.front() >= 40.0 && rb.front() < 44.0,
                 "each rallies once its own fight is 40 s old (%.2f s and %.2f s)", ra.empty() ? -1.0 : ra.front(), rb.empty() ? -1.0 : rb.front());
    const double first_aura_a = pa.log.first(aura->row.guid), first_aura_b = pb.log.first(aura->row.guid);
    ALERT_EFFECT(!ra.empty() && first_aura_a - ra.front() >= 5.0 && first_aura_a - ra.front() <= 6.6 && !rb.empty() && first_aura_b - rb.front() >= 5.0 && first_aura_b - rb.front() <= 6.6,
                 "each one's aura follows its own Rally (%.2f s and %.2f s after it)", ra.empty() ? -1.0 : first_aura_a - ra.front(), rb.empty() ? -1.0 : first_aura_b - rb.front());
    ALERT_EFFECT(pa.auras_without_rally() == 0 && pb.auras_without_rally() == 0 && pc.auras_without_rally() == 0, "no aura begins without its own Rally 5.0 to 6.6 s before it");
    size_t early = 0;
    for (const std::vector<double>* list : {&ra, &rb, &rc}) {
        for (size_t i = 1; i < list->size(); ++i) early += (*list)[i] - (*list)[i - 1] < 40.0 ? 1 : 0;
    }
    ALERT_EFFECT(early == 0, "no Flamespeaker rallies twice within 40 s (%zu pairs closer than that)", early);
    // each is on the other's side: the other's Rally (which names allies only) and the other's aura land on it, as the log names them
    const std::string text = log.text();
    const std::string leaf = "Goblin_FlameShaman_Gravehide";
    const size_t a_by_b = rally_group::landings_from(text, b, rally_effect->row.guid, a, leaf), b_by_a = rally_group::landings_from(text, a, rally_effect->row.guid, b, leaf);
    ALERT_EFFECT(a_by_b >= 1 && b_by_a >= 1, "each Flamespeaker is reached by the other's Rally (%zu lines for the first, %zu for the second)", a_by_b, b_by_a);
    const size_t a_shield = rally_group::landings_from(text, b, aura_effect->row.guid, a, leaf), b_shield = rally_group::landings_from(text, a, aura_effect->row.guid, b, leaf);
    ALERT_EFFECT(a_shield >= 1 && b_shield >= 1, "each is reached by the other's Fire Shield Aura (%zu lines for the first, %zu for the second)", a_shield, b_shield);
    ALERT_EFFECT(rally_group::landings_from(text, a, rally_effect->row.guid, a, leaf) == 0 && rally_group::landings_from(text, b, rally_effect->row.guid, b, leaf) == 0,
                 "neither is reached by its own Rally");
    ALERT_EFFECT(reacted2, "a second player who hits the shielded Flamespeaker is struck back by the Fire Shield's reaction as the first is");
    // the reset: its fight clock begins again, so its next Rally is 40 s into the new fight at the soonest (and comes: it does not wait on the timer of the old fight)
    const double after_reset = pb.log.first_after(rally->row.guid, kReset);
    ALERT_EFFECT(after_reset >= kReset + 40.0 && after_reset <= kEnd - 6.0, "the Flamespeaker that reset rallies again at %.2f s, 40 s or more after the reset and not never", after_reset);
    ALERT_EFFECT(after_reset > 0 && pb.log.first_after(aura->row.guid, after_reset) - after_reset >= 5.0 && pb.log.first_after(aura->row.guid, after_reset) - after_reset <= 6.6,
                 "... and its aura follows that Rally (%.2f s later)", after_reset > 0 ? pb.log.first_after(aura->row.guid, after_reset) - after_reset : -1.0);
    // the respawn: a new creature, a new clock; nothing of the dead one's timers is left
    ALERT_EFFECT(c != 0 && !rc.empty() && rc.front() >= kRespawn + 40.0 && rc.front() <= kEnd - 6.0, "the Flamespeaker made again rallies %.2f s into the run, 40 s or more after it was made and not never",
                 rc.empty() ? -1.0 : rc.front());
    ALERT_EFFECT(!rc.empty() && pc.log.first_after(aura->row.guid, rc.front()) - rc.front() >= 5.0 && pc.log.first_after(aura->row.guid, rc.front()) - rc.front() <= 6.6,
                 "... and its aura follows that Rally (%.2f s later)", rc.empty() ? -1.0 : pc.log.first_after(aura->row.guid, rc.front()) - rc.front());
}

AOC_REGISTER_TEST(test_two_flamespeakers_and_a_second_player_keep_their_own_pairs);

// R4: the log's volume.  A camp of twelve goblins that all rally writes a cast line for each cast and about one landing line for each cast, not a line for every creature the Rally
// reaches in every cast (ten of them each time, by the hit's cap): the camp's lines grow with the number of casts, not with its square.
void test_a_camp_of_rallying_goblins_does_not_flood_the_log(Fixture& f) {
    std::printf("a camp of twelve goblins that all rally: the log gets a cast line and about one landing line for each cast, not a line for each creature reached\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string path = creature_path(f, "/Humanoid/Goblin/Goblin_Bonebinder.Default__Goblin_Bonebinder_C");
    const abilities::AbilityDef* rally = f.catalog->ability_named("Goblin_Rally");
    const abilities::EffectDef* effect = f.catalog->effect_named("Goblin_Rally_Effect");
    ALERT_EFFECT(!path.empty() && rally && effect, "the Blueprint table has the Bonebinder goblin and the ability data has the Rally and its effect");
    if (path.empty() || !rally || !effect) return;
    BanditOpen made;
    ALERT_EFFECT(make_bandit_open(f, tables, path, made), "the world makes an open for the goblin (verdict: %s)", spawns::verdict_name(made.why));
    if (!made.open) return;
    rally_group::LogCapture log;
    alert_effects::Field field(f, tables, 0, 0);
    ALERT_EFFECT(field.ok(), "the fight's world is made (the captured data and the rig)");
    if (!field.ok()) return;
    field.world.spend_the_cast_log_budget();
    std::vector<uint64_t> camp;
    for (int i = 0; i < 12; ++i) {   // a ring at 4 m round a point 4 m from the player: every goblin is inside every other's 10 m sphere
        const double rad = 2.0 * 3.14159265358979323846 * static_cast<double>(i) / 12.0;
        camp.push_back(field.spawn(path, 400.0 * std::cos(rad), 400.0 + 400.0 * std::sin(rad), 900, made.open));
    }
    ALERT_EFFECT(std::find(camp.begin(), camp.end(), 0) == camp.end(), "the twelve goblins are put in the world");
    if (std::find(camp.begin(), camp.end(), 0) != camp.end()) return;
    constexpr double kSeconds = 100.0;
    const double dt = 1.0 / 15.0;
    for (double now = dt; now <= kSeconds + 1e-9; now += dt) field.tick(now);
    const std::string text = log.text();
    const size_t casts = rally_group::lines_with(text, {"plays the alert Goblin_Rally"});
    const size_t landings = rally_group::lines_with(text, {"Goblin_Rally_Effect", "lands on"});
    std::printf("  %zu cast lines and %zu landing lines in %.0f s: %.0f lines a minute\n", casts, landings, kSeconds, static_cast<double>(casts + landings) * 60.0 / kSeconds);
    {   // one of each, as the log reads
        std::istringstream in(text);
        std::string line;
        bool cast_shown = false, landing_shown = false;
        while (std::getline(in, line) && !(cast_shown && landing_shown)) {
            const bool is_cast = line.find("plays the alert Goblin_Rally") != std::string::npos;
            const bool is_landing = line.find("Goblin_Rally_Effect") != std::string::npos && line.find("lands on") != std::string::npos;
            if ((is_cast && !cast_shown) || (is_landing && !landing_shown)) std::printf("  e.g. %s\n", line.c_str());
            cast_shown = cast_shown || is_cast;
            landing_shown = landing_shown || is_landing;
        }
    }
    ALERT_EFFECT(casts >= 12, "the camp rallies at least once for each goblin in %.0f s (%zu cast lines)", kSeconds, casts);
    ALERT_EFFECT(landings >= 1 && landings <= 3 * casts, "the landings take at most three lines for each cast, not one for every creature reached (%zu lines for %zu casts)", landings, casts);
    ALERT_EFFECT(static_cast<double>(casts + landings) * 60.0 / kSeconds <= 100.0, "the camp's rallies write 100 lines a minute at the most (%.0f)", static_cast<double>(casts + landings) * 60.0 / kSeconds);
}

AOC_REGISTER_TEST(test_a_camp_of_rallying_goblins_does_not_flood_the_log);
