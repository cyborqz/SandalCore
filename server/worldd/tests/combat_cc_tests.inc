// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_cc_tests.inc: crowd control on creatures, the slows (included by combat_registered.inc inside test_worldd_combat's anonymous namespace)
//
//  A creature under a snare moves at (1 - the strongest snare) of its speed, at once, in every way it walks (a chase, a wander, the walk home), and its crowd control state says so in the update
//  that carries the effect.  The checks run real fights of a Wolf against the player on the combat rig (run_lunge_fight): what the Wolf covers per second and the speed its updates carry
//  are measured in stretches of the fight with the slow on and without it, and the crowd control state is read from the updates a client is sent.  Its attack's lunge is not slowed.
// ============================================================================

/// How fast the creature went in a stretch of a fight: the ground it covered per second by where the server had it at each tick, and the mean of the speeds the updates of the stretch carry.
struct CcSpeed {
    double walked = 0, wire = 0;
    size_t ticks = 0, wire_ticks = 0;
};

static CcSpeed cc_speed(const std::vector<LungeSample>& s, double from, double to) {
    CcSpeed out;
    double path = 0, time = 0, wire = 0;
    for (size_t i = 1; i < s.size(); ++i) {
        if (s[i].t <= from || s[i].t > to) continue;
        path += std::hypot(s[i].wx - s[i - 1].wx, s[i].wy - s[i - 1].wy);
        time += s[i].t - s[i - 1].t;
        ++out.ticks;
        if (s[i].wire_speed >= 0) {
            wire += s[i].wire_speed;
            ++out.wire_ticks;
        }
    }
    out.walked = time > 0 ? path / time : 0.0;
    out.wire = out.wire_ticks ? wire / static_cast<double>(out.wire_ticks) : 0.0;
    return out;
}

/// How far the creature went in one tick (cm).
static double cc_step(const std::vector<LungeSample>& s, size_t i) { return i > 0 && i < s.size() ? std::hypot(s[i].wx - s[i - 1].wx, s[i].wy - s[i - 1].wy) : 0.0; }

/// Puts the effects a hit has on the creature as the ability runtime does: their numbers worked out for the player's character, then through the world's own door.  Returns the seconds the first of
/// them lasts (0: none came out).
static double cc_land(LungeStepContext& c, const abilities::HitDef& hit) {
    combat::Fighter* target = c.world.fighter_of(c.creature);
    if (!target) return 0.0;
    combat::HitMeta meta;
    meta.source_type = content::kSourceTypeAbility;
    combat::Resolved gate;
    std::string why;
    if (!c.world.resolver().resolve_effects(c.player.fighter(), *target, hit, meta, gate, why) || gate.effects.empty()) return 0.0;
    const double seconds = gate.effects.front().duration;
    c.world.apply_effects(c.creature, c.player.actor_id(), gate.effects, c.now);
    return seconds;
}

/// One thing that happens once in a fight, when the clock first reaches `at`.
struct CcEvent {
    double at = 0;
    std::function<void(LungeStepContext&)> act;
    bool done = false;
};

/// What a fight left: the tick-by-tick samples, how many seconds the Chilled effect had left on the creature after each tick, and the moments the events recorded.
struct CcRun {
    std::vector<LungeSample> s;
    std::vector<double>      chill_left;
    double lit = -1;            // when the chill went on
    double chill_seconds = 0;   // how long its record says it lasts
    double snared_at = -1;      // when a second snare went on (a weaker one), and how long it lasts
    double snare_seconds = 0;
    double reset_at = -1;       // when the creature was sent home
    double left_at = -1;        // when the player who chilled it left the world
    double died_at = -1;        // when it was killed
    bool   walking = false;     // the world answered that the creature walks home
    bool   ran = false;
};

/// A fight of the Wolf against the player standing at (player_x, player_y) with `events` happening in it.  `provoke_at` is when the player starts hitting it.
static bool cc_fight(Fixture& f, const combat::NpcAbilityData& tables, const std::string& path, double seconds, double player_x, double player_y, double provoke_at, const LungeSteer& steer,
                     std::vector<CcEvent> events, CcRun& out) {
    const abilities::EffectDef* chilled = f.catalog->effect_named("Status_Chilled");
    const LungeStep step = [&](LungeStepContext& c) {
        if (c.after) {
            out.chill_left.push_back(chilled ? c.world.effect_remaining(c.creature, chilled->row.guid, c.now) : 0.0);
            return;
        }
        for (CcEvent& e : events) {
            if (e.done || c.now < e.at) continue;
            e.done = true;
            e.act(c);
        }
    };
    out.ran = run_lunge_fight(f, tables, path, seconds, player_x, player_y, steer, out.s, nullptr, false, provoke_at, nullptr, 40000.0, nullptr, 0, 0, step);
    return out.ran;
}

/// The index of the first sample at or after `t`.
static size_t cc_at(const std::vector<LungeSample>& s, double t) {
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i].t >= t - 1e-9) return i;
    }
    return s.size();
}

/// What a `[cc]` line of the log says: the snare and the speed the world uses for the creature under it.
struct CcLine {
    double snare = -1, speed = -1;
};

/// Runs `body` with the log going to a string instead of the console, and returns the `[cc]` lines it wrote, in order.
static std::vector<CcLine> cc_logged(const std::function<void()>& body) {
    std::ostringstream log;
    const auto previous = spdlog::default_logger();
    auto logger = std::make_shared<spdlog::logger>("slow_test", std::make_shared<spdlog::sinks::ostream_sink_mt>(log));
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(logger);
    body();
    logger->flush();
    spdlog::set_default_logger(previous);
    std::vector<CcLine> out;
    std::istringstream in(log.str());
    for (std::string line; std::getline(in, line);) {
        const size_t at = line.find("[cc] ");
        if (at == std::string::npos) continue;
        CcLine l;
        char leaf[64] = {};
        unsigned long long id = 0;
        if (std::sscanf(line.c_str() + at, "[cc] %63s (%llu) snare %lf speed %lf", leaf, &id, &l.snare, &l.speed) == 4) out.push_back(l);
    }
    return out;
}

/// The speed a Wolf that wanders walks at, taken at each step it walks in 60 s (cm/s, the middle one), with its NPC world told to keep `scale` of the speed of its walks: -1 when the Wolf could not be
/// made or walked less than 20 steps.
static double cc_wander_speed(const std::string& wolf_path, double scale, size_t* steps) {
    combat_rig::World w;
    if (!w.problem.empty()) return -1.0;
    const uint64_t id = w.spawn(wolf_path, 0, 0, 7, 900);
    if (id == 0 || !w.npcs->remove(id)) return -1.0;
    npc::NpcMotionSetup setup;
    setup.ambient = npc::Ambient::Wander;
    setup.walk_speed = 150.0;
    if (!w.npcs->add(id, setup) || !w.npcs->set_speed_scale(id, scale)) return -1.0;
    std::vector<double> speeds;
    for (double now = 1.0 / 15.0; now < 60.0; now += 1.0 / 15.0) {
        w.npcs->note_player(77, 0, 500, now);
        w.npcs->tick(now);
        const npc::Mover* m = w.npcs->mover(id);
        if (m && m->moving()) speeds.push_back(std::hypot(m->velocity().x, m->velocity().y));
    }
    if (steps) *steps = speeds.size();
    if (speeds.size() < 20) return -1.0;
    std::sort(speeds.begin(), speeds.end());
    return speeds[speeds.size() / 2];
}

// ---- a Wolf under a slow ------------------------------------------------------------------------------------------------------------------------------------------------------
// The Mage's Frostbolt chills what it hits: a snare of 0.5 for 6 seconds (scaled by the caster's disable duration).  A creature under it covers half the ground it covers without it, whatever it
// is doing on foot, from the moment the effect goes on to the moment it comes off, and its attacks' own movement is not slowed.
void test_cc_on_creatures(Fixture& f) {
    std::printf("a Wolf under a slow: half the speed in a chase and on the walk home, at once, full speed again when the chill ends, the applier leaves or it dies; the state says so in the update of the effect; its lunge goes on unslowed\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string wolf_path = creature_path(f, "/Beast/Canine/Wolf.Default__Wolf_C");
    const abilities::HitDef* frostbolt = f.catalog->hit_named("Mage_Frostbolt_Hit");
    const abilities::EffectDef* chilled = f.catalog->effect_named("Status_Chilled");
    CHECK(!wolf_path.empty() && f.motion.find(wolf_path) && tables.abilities_of(wolf_path) && frostbolt && chilled,
          "the Blueprint table has the Wolf, the creature tables list its abilities, and the data has Frostbolt's hit and the Chilled effect");
    if (wolf_path.empty() || !f.motion.find(wolf_path) || !tables.abilities_of(wolf_path) || !frostbolt || !chilled) return;
    std::string err;
    auto ability_db = content::AbilityContent::open(f.path, err);
    const auto record = ability_db ? ability_db->crowd_control(chilled->row.crowd_control_guid, err) : std::nullopt;
    CHECKF(record && record->applied_status == 16 && near(record->snare_strength, 0.5, 1e-9), "the Chilled effect's crowd control record is a snare of 0.5 and nothing else (status flags 16): %s", err.c_str());
    if (!record) return;
    const double kept = 1.0 - record->snare_strength;   // the share of its speed a chilled creature keeps
    const double dt = 1.0 / 15.0;

    // the chill goes on (its moment and its length are kept); the creature is put 45 m from its home and sent walking back
    const auto chill_on = [frostbolt](CcRun& run) {
        return [&run, frostbolt](LungeStepContext& c) {
            run.chill_seconds = cc_land(c, *frostbolt);
            run.lit = c.now;
        };
    };
    const auto sent_home = [](CcRun& run) {
        return [&run](LungeStepContext& c) {
            c.world.fighter_of(c.creature);   // (it becomes a combatant)
            const npc::NpcWorld::RelocateResult moved = c.w.npcs->relocate(c.creature, 0, 4500, 1000.0, false);
            c.w.npcs->restore_home(c.creature, nullptr);   // (the move took its home along: it goes home to where it was made)
            run.reset_at = c.now;
            run.walking = moved.status == npc::NpcWorld::Relocated::Ok && c.world.gm_reset(c.creature, c.now) == combat::CombatWorld::GmReset::Walking;
        };
    };

    // ---- 1: a chase.  The Wolf runs after the player, who is put 140 m away a moment after the fight begins; 3 s in, the chill goes on.
    CcRun chase;
    bool put_away = false;
    const LungeSteer far_off = [&put_away](double now, const LungeSample*, double& x, double& y, const world::WorldActor&) {
        if (put_away || now < 0.6) return false;
        put_away = true;
        x = 0;
        y = 14000;
        return true;
    };
    const std::vector<CcLine> chase_log = cc_logged([&] { cc_fight(f, tables, wolf_path, 12.5, 0, 3000, 0.2, far_off, {CcEvent{3.0, chill_on(chase)}}, chase); });
    CHECK(chase.ran, "a fight of the Wolf against a player who is put 140 m away runs");
    if (!chase.ran) return;
    const double chase_end = chase.lit + chase.chill_seconds;
    const size_t on_i = cc_at(chase.s, chase.lit);
    size_t off_i = on_i;   // the first tick after the chill went on in which it is off again
    while (off_i < chase.chill_left.size() && (off_i == on_i || chase.chill_left[off_i] > 0)) ++off_i;
    const CcSpeed run_free = cc_speed(chase.s, 1.0, chase.lit - 0.1);
    const CcSpeed run_chilled = cc_speed(chase.s, chase.lit + 0.4, chase_end - 0.4);
    const CcSpeed run_after = cc_speed(chase.s, chase_end + 0.4, chase_end + 2.4);
    std::printf("    1 chase: free %.1f cm/s (the updates say %.1f), chilled at %.2f s for %.2f s: %.1f cm/s (%.1f), after it %.1f cm/s (%.1f)\n", run_free.walked, run_free.wire, chase.lit, chase.chill_seconds, run_chilled.walked,
                run_chilled.wire, run_after.walked, run_after.wire);
    CHECKF(chase.chill_seconds > 4.0 && chase.chill_seconds < 9.0 && on_i < chase.s.size() && on_i < chase.chill_left.size() && chase.chill_left[on_i] > 0.0,
           "the chill is on the Wolf after Frostbolt's hit: it lasts %.2f s by the record, and %.2f s of it are left after the tick it went on in", chase.chill_seconds, on_i < chase.chill_left.size() ? chase.chill_left[on_i] : -1.0);
    CHECKF(run_free.walked > 300.0 && run_free.ticks >= 20 && near(run_free.wire, run_free.walked, 0.03 * run_free.walked), "the Wolf runs after the player at %.1f cm/s and its updates carry the same (%.1f)", run_free.walked,
           run_free.wire);
    CHECKF(near(run_chilled.walked, kept * run_free.walked, 0.015 * run_free.walked) && run_chilled.ticks >= 20, "chilled it covers %.2f of the ground it covered free (%.1f of %.1f cm/s): %.2f is what the snare leaves", run_chilled.walked / run_free.walked,
           run_chilled.walked, run_free.walked, kept);
    CHECKF(near(run_chilled.wire, kept * run_free.wire, 0.015 * run_free.wire + 2.0) && run_chilled.wire_ticks >= 20, "...and the speed its updates carry is %.2f of the free one (%.1f of %.1f cm/s)", run_chilled.wire / run_free.wire,
           run_chilled.wire, run_free.wire);
    CHECKF(near(run_after.walked, run_free.walked, 0.015 * run_free.walked) && near(run_after.wire, run_free.wire, 0.015 * run_free.wire + 2.0) && run_after.ticks >= 20,
           "when the chill has run out it runs at its full speed again (%.1f cm/s, the updates say %.1f; %.1f free)", run_after.walked, run_after.wire, run_free.walked);
    // at once: the step of the tick the chill went on in is already the slowed one, and the step of the tick after it ran out is a free one
    const double free_step = run_free.walked * dt;
    CHECKF(near(cc_step(chase.s, on_i), kept * free_step, 0.05 * free_step) && near(cc_step(chase.s, on_i + 1), kept * free_step, 0.05 * free_step),
           "the slow is at once: the first two steps under it are %.1f and %.1f cm, a slowed step is %.1f", cc_step(chase.s, on_i), cc_step(chase.s, on_i + 1), kept * free_step);
    CHECKF(off_i < chase.s.size() && near(cc_step(chase.s, off_i + 1), free_step, 0.05 * free_step), "...and so is the end: the step after the chill came off is %.1f cm, a free step is %.1f", cc_step(chase.s, off_i + 1), free_step);
    // the crowd control state in the updates: set in the update of the tick that adds the effect's item, cleared in the one that takes it off, and nothing in between
    CHECKF(on_i < chase.s.size() && chase.s[on_i].cc_flags == 16 && chase.s[on_i].has_cc_snare && near(chase.s[on_i].cc_snare, record->snare_strength, 1e-6) && chase.s[on_i].cc_items >= 1 && chase.s[on_i].cc_fields == 2,
           "the update that adds the effect's item says the state: flags %d (16 is a snare), snare %.2f, and nothing else of it (%zu fields), in the same update as %zu item(s)", on_i < chase.s.size() ? chase.s[on_i].cc_flags : -1,
           on_i < chase.s.size() ? chase.s[on_i].cc_snare : -1.0, on_i < chase.s.size() ? chase.s[on_i].cc_fields : 0, on_i < chase.s.size() ? chase.s[on_i].cc_items : 0);
    CHECKF(off_i < chase.s.size() && chase.s[off_i].cc_flags == 0 && chase.s[off_i].has_cc_snare && chase.s[off_i].cc_snare == 0.0 && chase.s[off_i].cc_fields == 2,
           "the update of the tick the chill comes off says it cleared: flags %d, snare %.2f (%zu fields)", off_i < chase.s.size() ? chase.s[off_i].cc_flags : -1, off_i < chase.s.size() ? chase.s[off_i].cc_snare : -1.0,
           off_i < chase.s.size() ? chase.s[off_i].cc_fields : 0);
    size_t said_before = 0, said_between_wrong = 0, said_after = 0;
    for (size_t i = 0; i < chase.s.size(); ++i) {
        if (chase.s[i].cc_flags < 0) continue;
        if (i < on_i) ++said_before;
        else if (i > on_i && i < off_i && (chase.s[i].cc_flags != 16 || !near(chase.s[i].cc_snare, record->snare_strength, 1e-6))) ++said_between_wrong;
        else if (i > off_i) ++said_after;
    }
    CHECKF(said_before == 0 && said_between_wrong == 0 && said_after == 0, "no other update says anything of the state (%zu before the chill, %zu that say something else during it, %zu after it)", said_before, said_between_wrong, said_after);
    // the log: one line when it goes on, one when it comes off, with the speed the world uses
    CHECKF(chase_log.size() == 2 && near(chase_log[0].snare, record->snare_strength, 0.005) && near(chase_log[1].snare, 0.0, 0.005) && near(chase_log[0].speed, kept * chase_log[1].speed, 1.0) &&
               near(chase_log[1].speed, run_free.walked, 0.02 * run_free.walked),
           "the log says it once when the chill goes on and once when it comes off, with the speed the world uses: %zu lines, snare %.2f speed %.0f, then snare %.2f speed %.0f (free %.0f)", chase_log.size(),
           chase_log.size() > 0 ? chase_log[0].snare : -1.0, chase_log.size() > 0 ? chase_log[0].speed : -1.0, chase_log.size() > 1 ? chase_log[1].snare : -1.0, chase_log.size() > 1 ? chase_log[1].speed : -1.0, run_free.walked);

    // ---- 2: the walk home.  The Wolf is put 45 m from its home and sent back, free in one fight and chilled in the other.
    CcRun home_free, home_chilled;
    CHECK(cc_fight(f, tables, wolf_path, 11.0, 0, 14000, -1, nullptr, {CcEvent{1.0, sent_home(home_free)}}, home_free) &&
              cc_fight(f, tables, wolf_path, 11.0, 0, 14000, -1, nullptr, {CcEvent{1.0, chill_on(home_chilled)}, CcEvent{1.0, sent_home(home_chilled)}}, home_chilled),
          "two walks home run, one of a free Wolf and one of a chilled one");
    if (!home_free.ran || !home_chilled.ran) return;
    CHECK(home_free.walking && home_chilled.walking, "the Wolf is sent home on foot in both");
    const double home_end = home_chilled.lit + home_chilled.chill_seconds;
    const CcSpeed walk_free = cc_speed(home_free.s, home_free.reset_at + 0.5, home_free.reset_at + 5.0);
    const CcSpeed walk_chilled = cc_speed(home_chilled.s, home_chilled.reset_at + 0.5, std::min(home_chilled.reset_at + 5.0, home_end - 0.4));
    const CcSpeed walk_free_after = cc_speed(home_free.s, home_end + 0.4, home_end + 3.4);
    const CcSpeed walk_chilled_after = cc_speed(home_chilled.s, home_end + 0.4, home_end + 3.4);
    std::printf("    2 walk home: free %.1f cm/s (%.1f), chilled %.1f (%.1f), after the chill %.1f (%.1f), free then %.1f\n", walk_free.walked, walk_free.wire, walk_chilled.walked, walk_chilled.wire, walk_chilled_after.walked,
                walk_chilled_after.wire, walk_free_after.walked);
    CHECKF(walk_free.walked > 100.0 && walk_free.ticks >= 20 && near(walk_free.wire, walk_free.walked, 0.03 * walk_free.walked), "a free Wolf walks home at %.1f cm/s and its updates carry %.1f", walk_free.walked, walk_free.wire);
    CHECKF(near(walk_chilled.walked, kept * walk_free.walked, 0.015 * walk_free.walked) && walk_chilled.ticks >= 20, "chilled it walks home at %.2f of that (%.1f of %.1f cm/s)", walk_chilled.walked / walk_free.walked, walk_chilled.walked,
           walk_free.walked);
    CHECKF(near(walk_chilled.wire, kept * walk_free.wire, 0.015 * walk_free.wire + 2.0) && walk_chilled.wire_ticks >= 20, "...and its updates carry %.2f of the free speed (%.1f of %.1f cm/s)", walk_chilled.wire / walk_free.wire, walk_chilled.wire,
           walk_free.wire);
    CHECKF(near(walk_chilled_after.walked, walk_free_after.walked, 0.015 * walk_free_after.walked) && walk_chilled_after.ticks >= 20, "when the chill has run out it walks at the full speed (%.1f cm/s, free %.1f)", walk_chilled_after.walked,
           walk_free_after.walked);

    // ---- 3: the player who chilled it leaves the world: what he put on the creatures ends, in the update of that tick, and the Wolf is free
    CcRun left;
    bool left_ran = cc_fight(f, tables, wolf_path, 8.0, 0, 14000, -1, nullptr,
                             {CcEvent{1.0, chill_on(left)}, CcEvent{1.0, sent_home(left)}, CcEvent{3.0, [&left](LungeStepContext& c) {
                                                                                                  left.left_at = c.now;
                                                                                                  c.world.remove_player(&c.player);
                                                                                              }}},
                             left);
    CHECK(left_ran, "a walk home in which the player leaves runs");
    if (left_ran) {
        const size_t i = cc_at(left.s, left.left_at);
        const CcSpeed after_leaving = cc_speed(left.s, left.left_at + 0.4, left.left_at + 2.9);
        const CcSpeed free_then = cc_speed(home_free.s, left.left_at + 0.4, left.left_at + 2.9);
        CHECKF(i < left.s.size() && left.chill_left[i] == 0.0 && left.s[i].cc_flags == 0 && left.s[i].has_cc_snare && left.s[i].cc_snare == 0.0 && left.s[i].held_effects == 0,
               "the update of the tick the player leaves says the state cleared (flags %d, snare %.2f) and the chill is gone", i < left.s.size() ? left.s[i].cc_flags : -1, i < left.s.size() ? left.s[i].cc_snare : -1.0);
        CHECKF(near(after_leaving.walked, free_then.walked, 0.015 * free_then.walked) && after_leaving.ticks >= 20, "...and the Wolf walks at its full speed at once (%.1f cm/s, a free Wolf %.1f)", after_leaving.walked, free_then.walked);
    }

    // ---- 4: it dies chilled: the state is cleared in the update of its death
    CcRun died;
    bool died_ran = cc_fight(f, tables, wolf_path, 6.0, 0, 14000, -1, nullptr,
                             {CcEvent{1.0, chill_on(died)}, CcEvent{1.0, sent_home(died)}, CcEvent{3.0, [&died](LungeStepContext& c) {
                                                                                                  died.died_at = c.now;
                                                                                                  combat::Resolved lethal;
                                                                                                  lethal.mods.push_back({1, content::kGuidHealth, -1.0e9});
                                                                                                  c.world.apply_hit_to_npc(c.creature, c.player.actor_id(), lethal, c.now);
                                                                                              }}},
                             died);
    CHECK(died_ran, "a walk home in which the Wolf is killed runs");
    if (died_ran) {
        const size_t i = cc_at(died.s, died.died_at);
        CHECKF(i < died.s.size() && died.chill_left[i] == 0.0 && died.s[i].cc_flags == 0 && died.s[i].has_cc_snare && died.s[i].cc_snare == 0.0 && died.s[i].held_effects == 0,
               "the update of the tick it dies says the state cleared (flags %d, snare %.2f) and it holds nothing", i < died.s.size() ? died.s[i].cc_flags : -1, i < died.s.size() ? died.s[i].cc_snare : -1.0);
    }

    // ---- 5: its attack goes on unslowed.  A player stands 8 m from the Wolf; free in one fight and chilled from its first moment in the other.  The chilled Wolf comes up at half speed, and its lunge
    // (the attack animation carries it) travels what the free one's does, in steps as long.
    CcRun lunge_free, lunge_chilled;
    CHECK(cc_fight(f, tables, wolf_path, 10.0, 0, 800, 0.2, nullptr, {}, lunge_free) && cc_fight(f, tables, wolf_path, 10.0, 0, 800, 0.2, nullptr, {CcEvent{0.1, chill_on(lunge_chilled)}}, lunge_chilled),
          "a fight against a player 8 m away runs, free and chilled");
    if (lunge_free.ran && lunge_chilled.ran) {
        struct Lunge {
            bool   found = false, chilled = false;
            double at = 0, travelled = 0, longest_step = 0;
        };
        const auto first_lunge = [&](const CcRun& r) {
            Lunge out;
            for (const LungeRun& lr : lunge_runs(r.s)) {
                const double travelled = std::hypot(r.s[lr.last + 1].wx - r.s[lr.first].wx, r.s[lr.last + 1].wy - r.s[lr.first].wy);
                if (travelled < 40.0) continue;
                out.found = true;
                out.at = r.s[lr.first].t;
                out.travelled = travelled;
                out.chilled = lr.first < r.chill_left.size() && r.chill_left[lr.first] > 0.0;
                for (size_t i = lr.first; i <= lr.last + 1; ++i) out.longest_step = std::max(out.longest_step, cc_step(r.s, i));
                break;
            }
            return out;
        };
        const Lunge a = first_lunge(lunge_free), b = first_lunge(lunge_chilled);
        std::printf("    5 lunge: free %s at %.2f s travelled %.0f cm (longest step %.1f); chilled %s at %.2f s (chill on: %d) travelled %.0f cm (longest step %.1f)\n", a.found ? "yes" : "none", a.at, a.travelled, a.longest_step,
                    b.found ? "yes" : "none", b.at, b.chilled ? 1 : 0, b.travelled, b.longest_step);
        CHECK(a.found && b.found, "both Wolves lunge at the player");
        CHECKF(b.found && b.chilled, "the chilled Wolf lunges while the chill is on it (the chill is on it at %.2f s)", b.at);
        CHECKF(a.found && b.found && near(b.travelled, a.travelled, 0.1 * a.travelled + 10.0) && near(b.longest_step, a.longest_step, 0.1 * a.longest_step + 2.0),
               "...and its lunge is the free one's: %.0f against %.0f cm, steps of %.1f against %.1f cm", b.travelled, a.travelled, b.longest_step, a.longest_step);
    }

    // ---- 6: two snares at once.  The Wolf walks home under the chill (0.5) and a Snared (0.25) put on in the same moment: it keeps what the stronger one leaves, not the product of the two
    // (0.375); when the chill is off the weaker one holds it at 0.75, and when that is off it is free.
    const abilities::HitDef* disengage = f.catalog->hit_named("Ranger_Disengage_Hit");
    const abilities::EffectDef* snared = f.catalog->effect_named("Status_Snared");
    CHECK(disengage && snared, "the data has the Ranger's Disengage hit and the Snared effect");
    if (disengage && snared) {
        const auto snared_record = ability_db ? ability_db->crowd_control(snared->row.crowd_control_guid, err) : std::nullopt;
        CHECKF(snared_record && snared_record->applied_status == 16 && near(snared_record->snare_strength, 0.25, 1e-9), "Snared is a snare of 0.25 and nothing else: %s", err.c_str());
        if (snared_record) {
            CcRun two;
            const abilities::HitDef* snare_hit = disengage;
            const bool ran = cc_fight(f, tables, wolf_path, 11.5, 0, 14000, -1, nullptr,
                                      {CcEvent{1.0, chill_on(two)}, CcEvent{1.0, [&two, snare_hit](LungeStepContext& c) {
                                                                                 two.snare_seconds = cc_land(c, *snare_hit);
                                                                                 two.snared_at = c.now;
                                                                             }},
                                       CcEvent{1.0, sent_home(two)}},
                                      two);
            CHECK(ran, "a walk home under a chill and a Snared runs");
            if (ran) {
                const double chill_off = two.lit + two.chill_seconds, snare_off = two.snared_at + two.snare_seconds;
                const CcSpeed both = cc_speed(two.s, two.reset_at + 0.5, chill_off - 0.4);
                const CcSpeed weaker = cc_speed(two.s, chill_off + 0.4, snare_off - 0.4);
                const CcSpeed none = cc_speed(two.s, snare_off + 0.4, snare_off + 1.9);
                const double snared_keeps = 1.0 - snared_record->snare_strength;
                std::printf("    6 two snares: both %.1f cm/s, the Snared alone %.1f, none %.1f (free %.1f); the chill off at %.2f s, the Snared at %.2f s\n", both.walked, weaker.walked, none.walked, walk_free.walked, chill_off, snare_off);
                CHECKF(snare_off > chill_off + 1.0, "the Snared outlasts the chill (%.2f s against %.2f s)", snare_off, chill_off);
                CHECKF(both.ticks >= 20 && near(both.walked, kept * walk_free.walked, 0.015 * walk_free.walked) && near(both.wire, kept * walk_free.wire, 0.015 * walk_free.wire + 2.0),
                       "under both it keeps what the stronger leaves: %.2f of its speed (%.1f of %.1f cm/s), not the product %.3f", both.walked / walk_free.walked, both.walked, walk_free.walked, kept * snared_keeps);
                CHECKF(weaker.ticks >= 12 && near(weaker.walked, snared_keeps * walk_free.walked, 0.015 * walk_free.walked) && near(weaker.wire, snared_keeps * walk_free.wire, 0.015 * walk_free.wire + 2.0),
                       "when the chill is off the Snared holds it at %.2f (%.1f of %.1f cm/s)", weaker.walked / walk_free.walked, weaker.walked, walk_free.walked);
                CHECKF(none.ticks >= 10 && near(none.walked, walk_free.walked, 0.015 * walk_free.walked), "...and when that is off it is free again (%.1f cm/s)", none.walked);
                // what the state says: 16 and the stronger snare while both are on, 16 and the weaker one once the chill is off, 0 and nothing at the end
                size_t chill_end_i = cc_at(two.s, two.lit);
                while (chill_end_i < two.chill_left.size() && two.chill_left[chill_end_i] > 0) ++chill_end_i;
                // (the flags stay 16 across this change, so the update may leave them out: only the snare changes)
                CHECKF(chill_end_i < two.s.size() && two.s[chill_end_i].cc_flags == -1 && two.s[chill_end_i].cc_fields == 1 && two.s[chill_end_i].has_cc_snare &&
                           near(two.s[chill_end_i].cc_snare, snared_record->snare_strength, 1e-6),
                       "the update of the tick the chill comes off says only that the snare is the weaker one now: snare %.2f (%zu field; the flags stay 16, so it leaves them out: %d)",
                       chill_end_i < two.s.size() ? two.s[chill_end_i].cc_snare : -1.0, chill_end_i < two.s.size() ? two.s[chill_end_i].cc_fields : 0, chill_end_i < two.s.size() ? two.s[chill_end_i].cc_flags : -2);
                size_t cleared_at = two.s.size();
                for (size_t i = chill_end_i + 1; i < two.s.size(); ++i) {
                    if (two.s[i].cc_flags == 0) {
                        cleared_at = i;
                        break;
                    }
                }
                CHECKF(cleared_at < two.s.size() && two.s[cleared_at].has_cc_snare && two.s[cleared_at].cc_snare == 0.0 && std::fabs(two.s[cleared_at].t - snare_off) < 0.2,
                       "...and the update of the tick the Snared comes off says it cleared (flags 0, snare 0 at %.2f s; it runs out at %.2f s)", cleared_at < two.s.size() ? two.s[cleared_at].t : -1.0, snare_off);
            }
        }
    }

    // ---- 7: a Wolf that wanders (the NPC world's own walk, with no order from the fight) walks its legs at half the speed too
    size_t free_steps = 0, slow_steps = 0;
    const double wander_free = cc_wander_speed(wolf_path, 1.0, &free_steps), wander_slow = cc_wander_speed(wolf_path, kept, &slow_steps);
    std::printf("    7 wander: free %.1f cm/s over %zu steps, slowed %.1f over %zu steps\n", wander_free, free_steps, wander_slow, slow_steps);
    CHECKF(wander_free > 50.0 && wander_slow > 0.0 && near(wander_slow, kept * wander_free, 0.015 * wander_free), "a wandering Wolf walks its legs at %.2f of its speed when its NPC world keeps %.2f of it (%.1f of %.1f cm/s)",
           wander_free > 0 ? wander_slow / wander_free : -1.0, kept, wander_slow, wander_free);

    // ---- 8: the Tank's Grapple: three snares of about a second each, one after the other (0.75, 0.5, 0.25), the next one put on in the very step the one before comes off.  The Wolf is never free between
    // them, the updates and the log name the three strengths and then the end, and it walks at what each leaves.
    const abilities::HitDef* grapple = f.catalog->hit_named("Tank_Grapple_HitSnare1");
    CHECK(grapple != nullptr, "the data has the Grapple's first snare");
    if (grapple) {
        CcRun chain;
        const std::vector<CcLine> chain_log = cc_logged([&] {
            cc_fight(f, tables, wolf_path, 8.0, 0, 14000, -1, nullptr,
                     {CcEvent{1.0, [&chain, grapple](LungeStepContext& c) {
                                  chain.snare_seconds = cc_land(c, *grapple);
                                  chain.snared_at = c.now;
                              }},
                      CcEvent{1.0, sent_home(chain)}},
                     chain);
        });
        CHECK(chain.ran, "a walk home under the Grapple's snares runs");
        if (chain.ran) {
            std::vector<size_t> says;   // the updates that name a snare
            for (size_t i = 0; i < chain.s.size(); ++i) {
                if (chain.s[i].has_cc_snare) says.push_back(i);
            }
            const double want[4] = {0.75, 0.5, 0.25, 0.0};
            bool in_order = says.size() == 4;
            for (size_t k = 0; in_order && k < 4; ++k) in_order = near(chain.s[says[k]].cc_snare, want[k], 1e-6);
            CHECKF(in_order, "the updates name the three snares and then the end, in order: %zu updates (%.2f, %.2f, %.2f and %.2f wanted)", says.size(), want[0], want[1], want[2], want[3]);
            CHECKF(chain_log.size() == 4 && near(chain_log[0].snare, 0.75, 0.005) && near(chain_log[1].snare, 0.5, 0.005) && near(chain_log[2].snare, 0.25, 0.005) && near(chain_log[3].snare, 0.0, 0.005),
                   "...and so does the log, with no free Wolf between the snares: %zu lines", chain_log.size());
            if (in_order) {
                for (size_t k = 0; k < 3; ++k) {
                    const CcSpeed under = cc_speed(chain.s, chain.s[says[k]].t + 0.15, chain.s[says[k + 1]].t - 0.15);
                    CHECKF(under.ticks >= 6 && near(under.walked, (1.0 - want[k]) * walk_free.walked, 0.015 * walk_free.walked) && near(under.wire, (1.0 - want[k]) * walk_free.wire, 0.015 * walk_free.wire + 2.0),
                           "under the snare of %.2f it walks at %.1f cm/s (%.2f of the free %.1f; the updates say %.1f)", want[k], under.walked, 1.0 - want[k], walk_free.walked, under.wire);
                }
            }
        }
    }
}
AOC_REGISTER_TEST(test_cc_on_creatures);
