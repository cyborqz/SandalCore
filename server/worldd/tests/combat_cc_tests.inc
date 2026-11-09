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

/// What the crowd control state of a creature holds now, read from its stats component (not from an update).  A field the world never wrote reads as the game's default (no animation: 255, an end of -1).
struct CcNow {
    int64_t flags = 0;
    double  snare = 0;
    bool    ocam = false, rooted_by_other = false;
    int64_t anim_request = 255, anim_record = 0;
    double  anim_start = 0, anim_duration = 0, anim_end = -1;
};

static CcNow cc_now(const world::WorldActor& a) {
    CcNow out;
    for (const aoc::protocol::repl::Subobject& sub : a.actor.subobjects) {
        if (!sub.obj.schema || std::string(sub.obj.schema->name) != "UAoCStatsComponent") continue;
        const aoc::protocol::schema::ClassSchema& sc = *sub.obj.schema;
        const auto get = [&](const char* path) { return sub.obj.state.get(sc, path); };
        if (const auto* v = get("CrowdControlState.StatusFlags"); v && v->get_if<uint64_t>()) out.flags = static_cast<int64_t>(*v->get_if<uint64_t>());
        if (const auto* v = get("CrowdControlState.SnareValue"); v && v->get_if<float>()) out.snare = *v->get_if<float>();
        if (const auto* v = get("CrowdControlState.bOverrideClientAuthMovement"); v && v->get_if<bool>()) out.ocam = *v->get_if<bool>();
        if (const auto* v = get("CrowdControlState.bRootedByOther"); v && v->get_if<bool>()) out.rooted_by_other = *v->get_if<bool>();
        if (const auto* v = get("CrowdControlState.CurrentAnimationRequest"); v && v->get_if<uint64_t>()) out.anim_request = static_cast<int64_t>(*v->get_if<uint64_t>());
        if (const auto* v = get("CrowdControlState.CrowdControlRecordToAnimate"); v && v->get_if<int64_t>()) out.anim_record = *v->get_if<int64_t>();
        if (const auto* v = get("CrowdControlState.SyncStartAnimTime"); v && v->get_if<double>()) out.anim_start = *v->get_if<double>();
        if (const auto* v = get("CrowdControlState.Duration"); v && v->get_if<float>()) out.anim_duration = *v->get_if<float>();
        if (const auto* v = get("CrowdControlState.EndAnimTime"); v && v->get_if<double>()) out.anim_end = *v->get_if<double>();
    }
    return out;
}

/// One thing that happens once in a fight, when the clock first reaches `at` (and `when`, if there is one, says yes: it is asked before the worlds move, the samples so far are the run's).
struct CcEvent {
    double at = 0;
    std::function<void(LungeStepContext&)> act;
    bool done = false;
    std::function<bool(LungeStepContext&)> when = nullptr;
};

/// What a fight left: the tick-by-tick samples, how many seconds the Chilled effect had left on the creature after each tick, and the moments the events recorded.
struct CcRun {
    std::vector<LungeSample> s;
    std::vector<double>      chill_left;
    std::vector<combat::IncomingHit> hits;   // what reached the player
    const abilities::EffectDef* watch = nullptr;    // effects whose seconds left are kept after every tick (beside the Chilled effect's)
    const abilities::EffectDef* watch2 = nullptr;
    std::vector<double>      watch_left, watch2_left;
    double on_at = -1, on_seconds = 0;          // when the effect the test is about went on, and how long its record says it lasts
    double second_at = -1, second_seconds = 0;  // ... and a second one put on over it
    bootstrap::EffectItem on_item, second_item;  // the creature's list item for the first effect of each of them, as it stood when it went on
    CcNow  end_state;                            // the crowd control state at the end of the fight, and the speed scale the creature's walker holds
    double end_scale = -1;
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

/// A fight of the Wolf against the player standing at (player_x, player_y) with `events` happening in it.  `provoke_at` is when the player starts hitting it.  `made` (if any) is the open the
/// creature is spawned from (a humanoid made from a generic one, whose ability component lists its tree's).
static bool cc_fight(Fixture& f, const combat::NpcAbilityData& tables, const std::string& path, double seconds, double player_x, double player_y, double provoke_at, const LungeSteer& steer,
                     std::vector<CcEvent> events, CcRun& out, const spawns::OpenTemplate* made = nullptr) {
    const abilities::EffectDef* chilled = f.catalog->effect_named("Status_Chilled");
    const LungeStep step = [&](LungeStepContext& c) {
        if (c.after) {
            out.chill_left.push_back(chilled ? c.world.effect_remaining(c.creature, chilled->row.guid, c.now) : 0.0);
            if (out.watch) out.watch_left.push_back(c.world.effect_remaining(c.creature, out.watch->row.guid, c.now));
            if (out.watch2) out.watch2_left.push_back(c.world.effect_remaining(c.creature, out.watch2->row.guid, c.now));
            return;
        }
        for (CcEvent& e : events) {
            if (e.done || c.now < e.at || (e.when && !e.when(c))) continue;
            e.done = true;
            e.act(c);
        }
    };
    out.ran = run_lunge_fight(f, tables, path, seconds, player_x, player_y, steer, out.s, &out.hits, false, provoke_at, nullptr, 40000.0, made, 0, 0, step);
    return out.ran;
}

/// The index of the first sample at or after `t`.
static size_t cc_at(const std::vector<LungeSample>& s, double t) {
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i].t >= t - 1e-9) return i;
    }
    return s.size();
}

/// What a `[cc]` line of the log says: the snare and the speed the world uses for the creature under it, and the status flags (-1: the line names none).
struct CcLine {
    double snare = -1, speed = -1;
    long long flags = -1;
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
        long long flags = -1;
        const int got = std::sscanf(line.c_str() + at, "[cc] %63s (%llu) snare %lf speed %lf flags %lld", leaf, &id, &l.snare, &l.speed, &flags);
        if (got >= 4) {
            l.flags = got == 5 ? flags : -1;
            out.push_back(l);
        }
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

    // ---- 6: two snares at once.  The Wolf walks home under the chill (0.5) and a snare of 0.25 put on in the same moment: it keeps what the stronger one leaves, not the product of the two
    // (0.375); when the chill is off the weaker one holds it at 0.75, and when that is off it is free.  The weaker one is the Generic Snared (8 s), which ticks no build-up: the Disengage's
    // Snared and a chill together put 6 stacks a second on the build-up of the lockout (a chill 2 and the Snared 1, each tick), whose lockout at 24 stacks takes both snares off in about 4 s.
    const abilities::EffectDef* snared = f.catalog->effect_named("Generic_Snared_Effect");
    CHECK(snared != nullptr, "the data has the Generic Snared effect");
    if (snared) {
        const auto snared_record = ability_db ? ability_db->crowd_control(snared->row.crowd_control_guid, err) : std::nullopt;
        CHECKF(snared_record && snared_record->applied_status == 16 && near(snared_record->snare_strength, 0.25, 1e-9), "Snared is a snare of 0.25 and nothing else: %s", err.c_str());
        if (snared_record) {
            CcRun two;
            const bool ran = cc_fight(f, tables, wolf_path, 11.5, 0, 14000, -1, nullptr,
                                      {CcEvent{1.0, chill_on(two)}, CcEvent{1.0, [&two, snared](LungeStepContext& c) {
                                                                                 effects::Applied weaker;
                                                                                 weaker.def = snared;
                                                                                 weaker.duration = 8.0;
                                                                                 c.world.apply_effects(c.creature, c.player.actor_id(), {weaker}, c.now);
                                                                                 two.snare_seconds = weaker.duration;
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

// ============================================================================================================================================================================
//  The crowd control that binds a Wolf: a root, a freeze, a stun, a trip and a sleep.
//
//  A rooted or frozen creature takes no step of any walk (a chase, a wander, the walk home) and may still attack what is in its reach; a root may turn and a freeze may not; its own attack's lunge
//  plays on.  A stunned, tripped or sleeping creature is a root that does not turn either, starts no attack, and loses the attack it had begun: the cast stops and its hits that had not landed are
//  dropped.  The update that carries the effect's item says the state: the flags, the override of the client's movement, and for a stun or a trip the animation to play (the strongest one).
// ============================================================================================================================================================================

/// The index of the tick an effect that went on at `on_at` for `seconds` runs out in (the first tick at or after its end).
static size_t cc_off(const std::vector<LungeSample>& s, double on_at, double seconds) { return cc_at(s, on_at + seconds); }

/// The ground the creature covered by walking over the ticks (first, last] (cm).  The steps of a tick a montage carried it in (a lunge, and the tick that ended the carry) are not walking.
static double cc_walked(const std::vector<LungeSample>& s, size_t first, size_t last) {
    std::vector<char> carried(s.size(), 0);
    for (const LungeRun& r : lunge_runs(s)) {
        for (size_t i = r.first; i <= r.last + 1 && i < s.size(); ++i) carried[i] = 1;
    }
    double total = 0;
    for (size_t i = first + 1; i <= last && i < s.size(); ++i) {
        if (!carried[i]) total += cc_step(s, i);
    }
    return total;
}

/// The attacks the creature began (a cast with a montage) in the seconds (from, to], and the hits that reached the player in them.
static size_t cc_casts(const std::vector<LungeSample>& s, double from, double to) {
    size_t n = 0;
    for (const LungeSample& x : s) {
        if (x.cast_began && x.t > from + 1e-9 && x.t <= to + 1e-9) ++n;
    }
    return n;
}

static size_t cc_hits(const std::vector<combat::IncomingHit>& h, double from, double to) {
    size_t n = 0;
    for (const combat::IncomingHit& x : h) {
        if (x.at > from + 1e-9 && x.at <= to + 1e-9) ++n;
    }
    return n;
}

/// The player stands `gap` cm in front of the creature (the way it faces) from the first look, and `gap` cm behind it `after` seconds after the moment `*on_at` points to (when it is set), so that the
/// creature has to turn to go on.  A null `on_at` never moves him.
static LungeSteer cc_player_steer(double gap, const double* on_at, double after) {
    const auto stage = std::make_shared<int>(0);
    return [=](double now, const LungeSample*, double& x, double& y, const world::WorldActor& c) {
        constexpr double kPi = 3.14159265358979323846;
        const double yaw = c.yaw_degrees * kPi / 180.0;
        if (*stage == 0) {
            *stage = 1;
            x = c.x + std::cos(yaw) * gap;
            y = c.y + std::sin(yaw) * gap;
            return true;
        }
        if (*stage == 1 && on_at && *on_at >= 0 && now >= *on_at + after) {
            *stage = 2;
            x = c.x - std::cos(yaw) * gap;
            y = c.y - std::sin(yaw) * gap;
            return true;
        }
        return false;
    };
}

/// An event's act: puts the effects of `hit` on the creature, and notes in the run the moment, the seconds the first of them lasts and its item as the creature's list holds it (the second time over, in
/// the run's second slots).
static std::function<void(LungeStepContext&)> cc_put(CcRun& run, const abilities::HitDef* hit, bool second = false) {
    return [&run, hit, second](LungeStepContext& c) {
        const double seconds = cc_land(c, *hit);
        (second ? run.second_seconds : run.on_seconds) = seconds;
        (second ? run.second_at : run.on_at) = c.now;
        const world::WorldActor* a = c.w.registry.find(c.creature);
        if (!a || hit->effects.empty()) return;
        for (const combat::EffectItemWrite& it : combat::read_effect_items(*a)) {
            if (it.item.effect_guid == hit->effects.front().effect_guid) (second ? run.second_item : run.on_item) = it.item;
        }
    };
}

/// An event that notes, a moment before the fight ends, the crowd control state the creature holds and the speed scale of its walker.
static CcEvent cc_probe_at(CcRun& run, double at) {
    return CcEvent{at, [&run](LungeStepContext& c) {
                       if (const world::WorldActor* a = c.w.registry.find(c.creature)) run.end_state = cc_now(*a);
                       if (const npc::Mover* m = c.w.npcs->mover(c.creature)) run.end_scale = m->speed_scale();
                   }};
}

/// An event's act: the creature is put 45 m from its home and sent walking back (a game master's reset).
static std::function<void(LungeStepContext&)> cc_sent_home(CcRun& run) {
    return [&run](LungeStepContext& c) {
        c.world.fighter_of(c.creature);   // (it becomes a combatant)
        const npc::NpcWorld::RelocateResult moved = c.w.npcs->relocate(c.creature, 0, 4500, 1000.0, false);
        c.w.npcs->restore_home(c.creature, nullptr);
        run.reset_at = c.now;
        run.walking = moved.status == npc::NpcWorld::Relocated::Ok && c.world.gm_reset(c.creature, c.now) == combat::CombatWorld::GmReset::Walking;
    };
}

/// The first lunge of a fight that carried the creature 40 cm or more: where it began and ended (sample indexes) and how far it went.
struct CcLunge {
    bool   found = false;
    size_t first = 0, last = 0;
    double travelled = 0;
};

static CcLunge cc_first_lunge(const std::vector<LungeSample>& s) {
    CcLunge out;
    for (const LungeRun& r : lunge_runs(s)) {
        const double travelled = std::hypot(s[r.last + 1].wx - s[r.first].wx, s[r.last + 1].wy - s[r.first].wy);
        if (travelled < 40.0) continue;
        out = {true, r.first, r.last, travelled};
        break;
    }
    return out;
}

/// How far a Wolf that wanders (the NPC world's own walk, no fight; a player stands near so it is awake) went in each stretch of a long run with a root of `root_seconds` put on it at `root_at`:
/// before the root, during it and after it (cm), and the speed scale its walker holds at the end.  -1 in every field when it could not be made.
struct CcWanderRun {
    double before = -1, during = -1, after = -1, scale = -1;
};

static CcWanderRun cc_wander_rooted(Fixture& f, const std::string& wolf_path, double root_at, double root_seconds, double total, double window_seconds = -1.0) {
    CcWanderRun out;
    Rig rig(4, 20, {});
    if (!rig.ok()) return out;
    combat_rig::World w;
    if (!w.problem.empty()) return out;
    const abilities::EffectDef* rooted = f.catalog->effect_named("Status_Rooted");
    const uint64_t id = w.spawn(wolf_path, 0, 0, 7, 0);
    if (!rooted || id == 0 || !w.npcs->remove(id)) return out;
    if (world::WorldActor* a = w.registry.find(id)) a->npc.aggro_off = true;   // (it never starts a fight)
    npc::NpcMotionSetup setup;
    setup.ambient = npc::Ambient::Wander;
    setup.walk_speed = 150.0;
    if (!w.npcs->add(id, setup)) return out;
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorldConfig cfg;
    cfg.seed = 5;
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion, cfg);
    // (the player who roots it stands far off: the root's own ticks give him threat, and a Wolf that has threat from a player 5 m away fights him instead of wandering; the NPC world is told of a player
    // near it all the same, so that the Wolf is awake)
    combat_rig::FakePlayer player(rig, 77, {0, 90000, 1098});
    world.add_player(&player);
    const world::WorldActor* first = w.registry.find(id);
    if (!first) return out;
    double lx = first->x, ly = first->y;
    double walked[3] = {0, 0, 0};
    bool put = false;
    const double dt = 1.0 / 15.0;
    for (double now = dt; now < total; now += dt) {
        if (!put && now >= root_at) {   // the root goes on through the world's own door, as the hits' effects do
            put = true;
            effects::Applied app;
            app.def = rooted;
            app.duration = root_seconds;
            world.apply_effects(id, 77, {app}, now);
        }
        w.npcs->note_player(77, 0, 500, now);
        w.npcs->tick(now);
        world.tick(now);
        const world::WorldActor* a = w.registry.find(id);
        if (!a) return out;
        const double step = std::hypot(a->x - lx, a->y - ly);
        lx = a->x;
        ly = a->y;
        walked[now < root_at ? 0 : (now < root_at + (window_seconds > 0 ? window_seconds : root_seconds) ? 1 : 2)] += step;
    }
    out.before = walked[0];
    out.during = walked[1];
    out.after = walked[2];
    if (const npc::Mover* m = w.npcs->mover(id)) out.scale = m->speed_scale();
    return out;
}

void test_cc_binds_creatures(Fixture& f) {
    std::printf("a Wolf under a root, a freeze, a stun, a trip or a sleep: no walk of any kind, a root turns and a freeze does not, a stun starts no attack and drops the hits not landed, its lunge plays on, the state and the animation come in the update of the effect, nothing is stuck after\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string wolf_path = creature_path(f, "/Beast/Canine/Wolf.Default__Wolf_C");
    const abilities::HitDef* frostbolt = f.catalog->hit_named("Mage_Frostbolt_Hit");
    const abilities::HitDef* cone = f.catalog->hit_named("Mage_ConeOfCold_Hit");
    const abilities::HitDef* bindings = f.catalog->hit_named("Summoner_SpectralBindings");
    const abilities::HitDef* condemn = f.catalog->hit_named("Cleric_Condemn_Hit");
    const abilities::HitDef* kick = f.catalog->hit_named("Rogue_Kick");
    const abilities::EffectDef* rooted = f.catalog->effect_named("Status_Rooted");
    const abilities::EffectDef* frozen = f.catalog->effect_named("Status_Frozen");
    const abilities::EffectDef* asleep = f.catalog->effect_named("Status_Incapacitated");
    const abilities::EffectDef* tripped = f.catalog->effect_named("Status_Tripped");
    const bool have = !wolf_path.empty() && f.motion.find(wolf_path) && tables.abilities_of(wolf_path) && frostbolt && cone && bindings && condemn && kick && rooted && frozen && asleep && tripped;
    CHECK(have, "the Blueprint table has the Wolf, the creature tables list its abilities, and the data has the hits and effects of a chill, a freeze, a root, a sleep and a trip");
    if (!have) return;
    std::string err;
    auto ability_db = content::AbilityContent::open(f.path, err);
    const auto root_rec = ability_db ? ability_db->crowd_control(rooted->row.crowd_control_guid, err) : std::nullopt;
    const auto freeze_rec = ability_db ? ability_db->crowd_control(frozen->row.crowd_control_guid, err) : std::nullopt;
    const auto sleep_rec = ability_db ? ability_db->crowd_control(asleep->row.crowd_control_guid, err) : std::nullopt;
    const auto trip_rec = ability_db ? ability_db->crowd_control(tripped->row.crowd_control_guid, err) : std::nullopt;
    CHECKF(root_rec && root_rec->applied_status == 1 && root_rec->override_client_move, "Rooted is a root (status flags 1) that overrides the client's own movement: %s", err.c_str());
    CHECKF(freeze_rec && freeze_rec->applied_status == 11 && freeze_rec->override_client_move, "Frozen is a root that does not turn and has the emplace bit (status flags 11) and overrides the client's own movement: %s", err.c_str());
    CHECKF(sleep_rec && sleep_rec->applied_status == 7 && sleep_rec->name == "General_Stun" && sleep_rec->animation_request.find("Stunned") != std::string::npos && sleep_rec->priority == 200,
           "Incapacitated (the sleep) is a stun: status flags 7, the General_Stun record, the Stunned animation at priority 200: %s", err.c_str());
    CHECKF(trip_rec && trip_rec->applied_status == 7 && trip_rec->name == "General_Trip" && trip_rec->animation_request.find("Tripped") != std::string::npos && trip_rec->priority == 300,
           "Tripped is a stun with the Tripped animation at priority 300 (above the stun's): %s", err.c_str());
    if (!root_rec || !freeze_rec || !sleep_rec || !trip_rec) return;
    const double dt = 1.0 / 15.0;

    // ---- 1: a Wolf rooted on its way to a player 8 m away stands where it is, starts no attack and walks again when the root has run out; the update and the log say so
    CcRun far_run;
    far_run.watch = rooted;
    const std::vector<CcLine> far_log = cc_logged([&] {
        cc_fight(f, tables, wolf_path, 11.0, 0, 800, 0.2, cc_player_steer(800, nullptr, 0),
                 {CcEvent{0.2, cc_put(far_run, bindings), false, [&far_run](LungeStepContext&) { return !far_run.s.empty() && far_run.s.back().wire_speed > 100.0; }}, cc_probe_at(far_run, 10.5)}, far_run);
    });
    CHECK(far_run.ran, "a fight of the Wolf against a player 8 m in front of it, with a root put on it the tick after the Wolf is seen running at him, runs");
    if (!far_run.ran) return;
    const size_t far_on = cc_at(far_run.s, far_run.on_at);
    const size_t far_off = cc_off(far_run.s, far_run.on_at, far_run.on_seconds);
    const bool far_ok = far_run.on_at > 0 && far_on >= 1 && far_off < far_run.s.size() && far_off > far_on;
    CHECKF(far_run.on_seconds > 3.0 && far_run.on_seconds < 6.0 && far_ok && far_run.watch_left[far_on] > 0.0 && far_run.watch_left[far_off] == 0.0,
           "the root is on the Wolf after the hit (%.2f s by the record) and off again when its time is over (tick %zu of %zu)", far_run.on_seconds, far_off, far_run.s.size());
    if (far_ok) {
        const double walked = cc_walked(far_run.s, far_on - 1, far_off);
        std::printf("    1 root 8 m away: on at %.2f s for %.2f s, walked %.1f cm while it was on, %zu attacks begun, then %.0f cm in the 2 s after it\n", far_run.on_at, far_run.on_seconds, walked,
                    cc_casts(far_run.s, far_run.on_at, far_run.s[far_off].t), cc_walked(far_run.s, far_off + 1, far_off + 31));
        CHECKF(walked <= 5.0, "rooted on its way to the player it stays where it is: %.1f cm walked in the %.2f s of the root (5 cm allowed)", walked, far_run.s[far_off].t - far_run.on_at);
        CHECKF(cc_casts(far_run.s, far_run.on_at, far_run.s[far_off].t - 1e-6) == 0, "...and it starts no attack from there (the player is out of its reach)");
        CHECKF(far_run.s[far_on].wire_speed >= 0 && far_run.s[far_on].wire_speed < 1.0, "...the update of the tick the root went on in says it stands (a speed of %.1f cm/s)", far_run.s[far_on].wire_speed);
        CHECKF(far_run.s[far_on].cc_flags == 1 && far_run.s[far_on].cc_ocam == 1 && far_run.s[far_on].cc_fields == 2 && far_run.s[far_on].cc_items >= 1 && far_run.s[far_on].cc_anim_request < 0,
               "the update that adds the root's item says the state: flags %d (1 is a root), the override of the client's own movement %d, and nothing else of it (%zu fields), with %zu item(s)", far_run.s[far_on].cc_flags,
               far_run.s[far_on].cc_ocam, far_run.s[far_on].cc_fields, far_run.s[far_on].cc_items);
        CHECKF(far_run.s[far_off].cc_flags == 0 && far_run.s[far_off].cc_ocam == 0 && far_run.s[far_off].cc_fields == 2,
               "the update of the tick the root ends says it cleared: flags %d, override %d (%zu fields)", far_run.s[far_off].cc_flags, far_run.s[far_off].cc_ocam, far_run.s[far_off].cc_fields);
        CHECKF(cc_walked(far_run.s, far_off + 1, far_off + 31) >= 100.0, "when the root has run out it walks on towards the player (to the reach of its leap): %.0f cm in the 2 s after it", cc_walked(far_run.s, far_off + 1, far_off + 31));
        CHECKF(far_run.end_state.flags == 0 && !far_run.end_state.ocam && !far_run.end_state.rooted_by_other && far_run.end_state.anim_request == 255 && far_run.end_scale == 1.0,
               "afterwards the creature's state is empty: flags %lld, override %d, animation %lld, and its walker keeps %.2f of its speed", static_cast<long long>(far_run.end_state.flags), far_run.end_state.ocam ? 1 : 0,
               static_cast<long long>(far_run.end_state.anim_request), far_run.end_scale);
        CHECKF(far_log.size() == 2 && far_log[0].flags == 1 && far_log[0].speed == 0.0 && far_log[1].flags == 0 && far_log[1].speed > 100.0,
               "the log says it once when the root goes on (flags 1, speed 0) and once when it comes off (flags 0, a speed again): %zu lines, flags %lld speed %.0f, then flags %lld speed %.0f", far_log.size(),
               far_log.size() > 0 ? far_log[0].flags : -9LL, far_log.size() > 0 ? far_log[0].speed : -1.0, far_log.size() > 1 ? far_log[1].flags : -9LL, far_log.size() > 1 ? far_log[1].speed : -1.0);
    }

    // ---- 1b: a root that goes on in the first tick, before the Wolf has taken a step: it never leaves the place it stood at (8 m from the player)
    CcRun still_run;
    still_run.watch = rooted;
    const bool still_ran = cc_fight(f, tables, wolf_path, 6.0, 0, 800, 0.2, cc_player_steer(800, nullptr, 0), {CcEvent{0.0, cc_put(still_run, bindings)}}, still_run);
    CHECK(still_ran, "a fight in which a root goes on in the first tick, with a player 8 m away, runs");
    if (still_ran) {
        const size_t off_i = cc_off(still_run.s, still_run.on_at, still_run.on_seconds);
        if (still_run.on_at >= 0 && off_i > 20 && off_i < still_run.s.size()) {
            double drift = 0;
            for (size_t j = 0; j <= off_i; ++j) drift = std::max(drift, std::hypot(still_run.s[j].wx, still_run.s[j].wy));
            CHECKF(drift <= 5.0 && cc_casts(still_run.s, 0, still_run.s[off_i].t) == 0, "rooted 8 m from the player before it moved, the Wolf stays within 5 cm of where it stood for the whole root (%.1f cm) and begins no attack", drift);
        } else {
            CHECKF(false, "the root outlasts the first moments of the fight (it went on at %.2f s for %.2f s)", still_run.on_at, still_run.on_seconds);
        }
    }

    // ---- 2: a root with the player in reach.  The Wolf bites at the rate of a free one, and turns on the spot to follow a player who steps behind it.
    CcRun near_ctl, near_run;
    near_run.watch = rooted;
    const bool near_ran = cc_fight(f, tables, wolf_path, 9.0, 0, 200, 0.2, cc_player_steer(200, nullptr, 0), {}, near_ctl) &&
                          cc_fight(f, tables, wolf_path, 9.0, 0, 200, 0.2, cc_player_steer(200, &near_run.on_at, 2.0), {CcEvent{0.4, cc_put(near_run, bindings)}}, near_run);
    CHECK(near_ran, "two fights against a player 2 m in front of the Wolf run, one free and one with a root put on it at 0.4 s and the player stepping behind it two seconds into the root");
    if (near_ran) {
        const size_t on_i = cc_at(near_run.s, near_run.on_at);
        const size_t off_i = cc_off(near_run.s, near_run.on_at, near_run.on_seconds);
        const bool ok = near_run.on_at > 0 && on_i >= 1 && off_i < near_run.s.size() && off_i > on_i + 50;
        CHECK(ok && near_run.watch_left[on_i] > 0.0 && near_run.watch_left[off_i] == 0.0, "the root is on the Wolf after the hit, outlasts the two seconds before the player steps behind it, and is off when its time is over");
        if (ok) {
            const double flip_t = near_run.on_at + 2.0, off_t = near_run.s[off_i].t;
            const size_t flip_i = cc_at(near_run.s, flip_t);
            const size_t casts_ctl = cc_casts(near_ctl.s, near_run.on_at, flip_t), casts_root = cc_casts(near_run.s, near_run.on_at, flip_t);
            const size_t hits_ctl = cc_hits(near_ctl.hits, near_run.on_at, flip_t), hits_root = cc_hits(near_run.hits, near_run.on_at, flip_t);
            double turned = 0;
            for (size_t j = flip_i; j <= off_i && j < near_run.s.size(); ++j) turned = std::max(turned, std::fabs(wrap180(near_run.s[j].yaw - near_run.s[flip_i].yaw)));
            const size_t casts_behind = cc_casts(near_run.s, flip_t, off_t);
            std::printf("    2 root in reach: attacks in the 2 s before the player steps behind it: free %zu, rooted %zu (hits %zu and %zu); after it the rooted Wolf turned %.0f deg and began %zu attack(s) before the root ran out\n",
                        casts_ctl, casts_root, hits_ctl, hits_root, turned, casts_behind);
            CHECKF(casts_root >= 1 && casts_root + 1 >= casts_ctl && casts_ctl + 1 >= casts_root && hits_root + 1 >= hits_ctl && hits_ctl + 1 >= hits_root,
                   "rooted with the player in its reach it attacks at the rate of a free Wolf: %zu attacks and %zu hits in 2 s against %zu and %zu free", casts_root, hits_root, casts_ctl, hits_ctl);
            CHECKF(cc_walked(near_run.s, on_i - 1, off_i) <= 5.0, "...and it walks nowhere: %.1f cm walked in the root (5 cm allowed)", cc_walked(near_run.s, on_i - 1, off_i));
            CHECKF(turned > 120.0 && casts_behind >= 1, "a root may turn: the player steps behind the Wolf, it turns %.0f deg on the spot and attacks him again (%zu attack(s) begun)", turned, casts_behind);
            CHECKF(near_run.s[on_i].cc_flags == 1 && near_run.s[on_i].cc_ocam == 1 && near_run.s[on_i].cc_fields == 2, "the root's update says flags %d and the override %d (%zu fields)", near_run.s[on_i].cc_flags,
                   near_run.s[on_i].cc_ocam, near_run.s[on_i].cc_fields);
        }
    }

    // ---- 3: a freeze (the Cone of Cold on a chilled Wolf).  Frozen is a root that does not turn: the Wolf bites what is in front of it and does nothing about a player who steps behind it.
    CcRun frz_run;
    frz_run.watch = frozen;
    const bool frz_ran = cc_fight(f, tables, wolf_path, 10.0, 0, 200, 0.2, cc_player_steer(200, &frz_run.on_at, 2.0),
                                  {CcEvent{0.3, [frostbolt](LungeStepContext& c) { cc_land(c, *frostbolt); }}, CcEvent{0.8, cc_put(frz_run, cone)}, cc_probe_at(frz_run, 9.5)},
                                  frz_run);
    CHECK(frz_ran, "a fight in which a chilled Wolf takes a Cone of Cold with the player 2 m in front of it runs");
    if (frz_ran) {
        const size_t on_i = cc_at(frz_run.s, frz_run.on_at);
        const size_t off_i = cc_off(frz_run.s, frz_run.on_at, frz_run.on_seconds);
        const bool ok = frz_run.on_at > 0 && frz_run.on_seconds > 3.0 && frz_run.on_seconds < 6.0 && on_i >= 1 && off_i < frz_run.s.size() && off_i > on_i + 50;
        CHECKF(ok && frz_run.watch_left[on_i] > 0.0 && frz_run.watch_left[off_i] == 0.0, "the Cone of Cold freezes the chilled Wolf (Frozen lasts %.2f s by the record) and the freeze is off when its time is over", frz_run.on_seconds);
        if (ok) {
            const double flip_t = frz_run.on_at + 2.0, off_t = frz_run.s[off_i].t;
            const size_t flip_i = cc_at(frz_run.s, flip_t);
            double yaw_held = 0;
            for (size_t j = on_i; j <= off_i; ++j) yaw_held = std::max(yaw_held, std::fabs(wrap180(frz_run.s[j].yaw - frz_run.s[on_i].yaw)));
            const size_t casts_before = cc_casts(frz_run.s, frz_run.on_at, flip_t), casts_ctl = cc_casts(near_ctl.s, frz_run.on_at, flip_t);
            const size_t casts_behind = cc_casts(frz_run.s, flip_t + dt, off_t - 1e-6);
            const double behind = std::fabs(wrap180(std::atan2(frz_run.s[off_i].py - frz_run.s[off_i].wy, frz_run.s[off_i].px - frz_run.s[off_i].wx) * 180.0 / 3.14159265358979323846 - frz_run.s[off_i].yaw));
            double turned_after = 0;
            for (size_t j = off_i; j < frz_run.s.size() && j <= off_i + 45; ++j) turned_after = std::max(turned_after, std::fabs(wrap180(frz_run.s[j].yaw - frz_run.s[on_i].yaw)));
            std::printf("    3 freeze: on at %.2f s for %.2f s; attacks begun before the player steps behind it %zu (a free Wolf %zu), after it %zu; the facing moved at most %.2f deg while it was frozen (the player %.0f deg off it); %.0f deg when it was over\n",
                        frz_run.on_at, frz_run.on_seconds, casts_before, casts_ctl, casts_behind, yaw_held, behind, turned_after);
            CHECKF(frz_run.s[on_i].cc_flags == 27 && frz_run.s[on_i].cc_rooted_by_other == 1 && frz_run.s[on_i].cc_ocam == 1 && frz_run.s[on_i].cc_fields == 3 && frz_run.s[on_i].cc_items >= 1,
                   "the update that adds Frozen to the chill says flags %d (27: the chill's 16 and the freeze's 11), rooted by another %d, the override %d, and no more (%zu fields)", frz_run.s[on_i].cc_flags,
                   frz_run.s[on_i].cc_rooted_by_other, frz_run.s[on_i].cc_ocam, frz_run.s[on_i].cc_fields);
            CHECKF(frz_run.s[off_i].cc_flags == 16 && frz_run.s[off_i].cc_rooted_by_other == 0 && frz_run.s[off_i].cc_ocam == 0 && frz_run.s[off_i].cc_fields == 3,
                   "...and the update of the tick Frozen ends says flags %d (the chill's 16 is left), rooted by another %d, override %d (%zu fields)", frz_run.s[off_i].cc_flags, frz_run.s[off_i].cc_rooted_by_other,
                   frz_run.s[off_i].cc_ocam, frz_run.s[off_i].cc_fields);
            CHECKF(cc_walked(frz_run.s, on_i - 1, off_i) <= 5.0, "frozen it walks nowhere: %.1f cm walked in the freeze (5 cm allowed)", cc_walked(frz_run.s, on_i - 1, off_i));
            CHECKF(casts_before >= 1 && casts_before <= casts_ctl + 1, "...but it attacks what is in reach: %zu attacks begun in the 2 s before the player steps behind it (a free Wolf %zu)", casts_before, casts_ctl);
            CHECKF(behind > 150.0 && yaw_held < 1.0 && casts_behind == 0,
                   "Frozen does not turn: the player steps behind it (%.0f deg off its facing), its facing moves %.2f deg in the whole freeze, and it begins %zu attacks at him", behind, yaw_held, casts_behind);
            CHECKF(turned_after > 120.0, "...and when the freeze is over it turns to him (%.0f deg)", turned_after);
            CHECKF(frz_run.end_state.flags == 0 && !frz_run.end_state.ocam && !frz_run.end_state.rooted_by_other && frz_run.end_state.snare == 0.0 && frz_run.end_scale == 1.0,
                   "when the chill has run out too the state is empty: flags %lld, snare %.2f, override %d, and its walker keeps %.2f of its speed", static_cast<long long>(frz_run.end_state.flags), frz_run.end_state.snare,
                   frz_run.end_state.ocam ? 1 : 0, frz_run.end_scale);
            (void)flip_i;
        }
    }

    // ---- 4: a root does not cut the Wolf's own lunge.  The player is 8 m away; the root goes on the tick after the lunge begins, and the lunge travels what the free Wolf's does.
    CcRun lunge_ctl, lunge_run;
    lunge_run.watch = rooted;
    const bool lunge_ran = cc_fight(f, tables, wolf_path, 8.0, 0, 800, 0.2, cc_player_steer(800, nullptr, 0), {}, lunge_ctl) &&
                           cc_fight(f, tables, wolf_path, 8.0, 0, 800, 0.2, cc_player_steer(800, nullptr, 0),
                                    {CcEvent{0.0, cc_put(lunge_run, bindings), false, [&lunge_run](LungeStepContext&) { return !lunge_run.s.empty() && lunge_run.s.back().active == 1; }}}, lunge_run);
    CHECK(lunge_ran, "two fights against a player 8 m away run, one free and one in which a root goes on the tick after the Wolf's lunge begins");
    if (lunge_ran) {
        const CcLunge a = cc_first_lunge(lunge_ctl.s), b = cc_first_lunge(lunge_run.s);
        const size_t on_i = cc_at(lunge_run.s, lunge_run.on_at);
        const size_t off_i = cc_off(lunge_run.s, lunge_run.on_at, lunge_run.on_seconds);
        std::printf("    4 lunge under a root: free lunge %.0f cm over %zu ticks; rooted at %.2f s (the lunge ran from %.2f to %.2f s): %.0f cm over %zu ticks\n", a.travelled, a.found ? a.last - a.first + 1 : 0, lunge_run.on_at,
                    b.found ? lunge_run.s[b.first].t : -1.0, b.found ? lunge_run.s[b.last].t : -1.0, b.travelled, b.found ? b.last - b.first + 1 : 0);
        CHECK(a.found && b.found, "both Wolves lunge at the player");
        if (a.found && b.found && on_i >= 1 && off_i < lunge_run.s.size()) {
            CHECKF(lunge_run.on_at > lunge_run.s[b.first].t && lunge_run.on_at <= lunge_run.s[b.last].t + 1e-9 && lunge_run.watch_left[on_i] > 0.0,
                   "the root went on while the lunge was carrying the Wolf (the lunge ran from %.2f to %.2f s, the root went on at %.2f s)", lunge_run.s[b.first].t, lunge_run.s[b.last].t, lunge_run.on_at);
            CHECKF(near(b.travelled, a.travelled, 0.1 * a.travelled + 10.0) && b.last - b.first + 2 >= a.last - a.first + 1, "...and the lunge goes on to its end: %.0f cm against %.0f cm free, over %zu ticks against %zu",
                   b.travelled, a.travelled, b.last - b.first + 1, a.last - a.first + 1);
            CHECKF(lunge_run.s[on_i].cc_flags == 1 && lunge_run.s[on_i].cc_ocam == 1, "...and the root's update says flags %d, override %d", lunge_run.s[on_i].cc_flags, lunge_run.s[on_i].cc_ocam);
            CHECKF(cc_walked(lunge_run.s, on_i - 1, off_i) <= 5.0, "...and the Wolf walks nowhere in the rest of the root: %.1f cm (5 cm allowed; its lunges are not counted)", cc_walked(lunge_run.s, on_i - 1, off_i));
        }
    }

    // ---- 5: a sleep (Incapacitated, a stun).  The Condemn lands on the tick after the Wolf began an attack: the attack's hit does not land, the cast stops, nothing is begun for the length of the sleep, the Wolf
    // does not turn to a player who steps behind it, and when the sleep is over it attacks again.
    CcRun stun_ctl, stun_run;
    stun_run.watch = asleep;
    const double stun_len = 20.0;
    cc_fight(f, tables, wolf_path, stun_len, 0, 200, 0.2, cc_player_steer(200, nullptr, 0), {}, stun_ctl);
    double wind_start = -1, wind_hit = -1;   // the free Wolf's first attack after 1.5 s, and the moment its first hit of that attack lands
    for (const LungeSample& x : stun_ctl.s) {
        if (x.cast_began && x.t >= 1.5) {
            wind_start = x.t;
            break;
        }
    }
    for (const combat::IncomingHit& h : stun_ctl.hits) {
        if (wind_start >= 0 && h.at > wind_start) {
            wind_hit = h.at;
            break;
        }
    }
    CHECKF(wind_start >= 0 && wind_hit > wind_start + 2 * dt, "the free Wolf begins an attack at %.2f s and the first hit of it lands at %.2f s (the wind-up the stun has to land in)", wind_start, wind_hit);
    const bool stun_ran = cc_fight(f, tables, wolf_path, stun_len, 0, 200, 0.2, cc_player_steer(200, &stun_run.on_at, 3.0),
                                   {CcEvent{1.5, cc_put(stun_run, condemn), false, [&stun_run](LungeStepContext&) { return !stun_run.s.empty() && stun_run.s.back().cast_began; }}, cc_probe_at(stun_run, stun_len - 0.5)},
                                   stun_run);
    CHECK(stun_ran, "a fight in which the Condemn lands on the tick after the Wolf began an attack runs");
    if (stun_ran && wind_start >= 0) {
        const size_t on_i = cc_at(stun_run.s, stun_run.on_at);
        const size_t off_i = cc_off(stun_run.s, stun_run.on_at, stun_run.on_seconds);
        const bool ok = stun_run.on_at > 0 && on_i >= 1 && off_i < stun_run.s.size() && std::fabs(stun_run.on_at - (wind_start + dt)) < 1e-9 && stun_run.on_seconds > 8.0 && stun_run.on_seconds < 12.0 && off_i > on_i + 120;
        CHECKF(ok && stun_run.watch_left[on_i] > 0.0 && stun_run.watch_left[off_i] == 0.0, "the Condemn puts the Wolf to sleep on the tick after its attack began (%.2f s; the free Wolf began at %.2f s) for %.2f s by the record, and the sleep is off when its time is over",
               stun_run.on_at, wind_start, stun_run.on_seconds);
        if (ok) {
            const double off_t = stun_run.s[off_i].t, flip_t = stun_run.on_at + 3.0;
            const LungeSample& so = stun_run.s[on_i];
            const LungeSample& se = stun_run.s[off_i];
            double yaw_held = 0;
            for (size_t j = on_i; j <= off_i; ++j) yaw_held = std::max(yaw_held, std::fabs(wrap180(stun_run.s[j].yaw - stun_run.s[on_i].yaw)));
            const double behind = std::fabs(wrap180(std::atan2(se.py - se.wy, se.px - se.wx) * 180.0 / 3.14159265358979323846 - se.yaw));
            double turned_after = 0;
            for (size_t j = off_i; j < stun_run.s.size() && j <= off_i + 60; ++j) turned_after = std::max(turned_after, std::fabs(wrap180(stun_run.s[j].yaw - stun_run.s[on_i].yaw)));
            const size_t casts_in = cc_casts(stun_run.s, stun_run.on_at, off_t - 1e-6), hits_in = cc_hits(stun_run.hits, stun_run.on_at, off_t - 1e-6), casts_after = cc_casts(stun_run.s, off_t - 1e-6, off_t + 4.0);
            std::printf("    5 sleep: the free Wolf's attack began at %.2f s and its hit landed at %.2f s; asleep from %.2f s for %.2f s: %zu attacks begun, %zu hits landed, the facing moved %.2f deg (the player %.0f deg off it, flipped at %.2f s); after it %zu attacks in 4 s and %.0f deg turned\n",
                        wind_start, wind_hit, stun_run.on_at, stun_run.on_seconds, casts_in, hits_in, yaw_held, behind, flip_t, casts_after, turned_after);
            CHECKF(casts_in == 0 && hits_in == 0, "asleep it begins no attack and none of its hits lands: %zu attacks begun and %zu hits landed in the %.2f s (the hit the free Wolf landed at %.2f s is dropped)", casts_in, hits_in,
                   off_t - stun_run.on_at, wind_hit);
            CHECKF(so.montage == 0 && so.active != 1, "the attack it had begun is stopped: no montage plays in the tick of the sleep (montage %llu, lunge %d)", static_cast<unsigned long long>(so.montage), so.active);
            CHECKF(cc_walked(stun_run.s, on_i - 1, off_i) <= 5.0, "it walks nowhere: %.1f cm walked while it sleeps (5 cm allowed)", cc_walked(stun_run.s, on_i - 1, off_i));
            CHECKF(behind > 150.0 && yaw_held < 1.0, "it does not turn: the player steps behind it (%.0f deg off its facing) and its facing moves %.2f deg while it sleeps", behind, yaw_held);
            CHECKF(turned_after > 120.0 && casts_after >= 1, "when the sleep is over it turns to the player (%.0f deg) and attacks again (%zu attacks begun in 4 s)", turned_after, casts_after);
            const double want_end = stun_run.on_item.start_time + static_cast<double>(stun_run.on_item.duration) - 1.4;
            CHECKF(so.cc_flags == 7 && so.cc_anim_request == 0 && so.has_cc_anim_record && so.cc_anim_record == sleep_rec->guid && so.has_cc_anim_start && so.cc_anim_start == stun_run.on_item.start_time &&
                       so.has_cc_anim_duration && so.cc_anim_duration == static_cast<double>(stun_run.on_item.duration) && so.has_cc_anim_end && near(so.cc_anim_end, want_end, 1e-6) && so.cc_fields == 6 && so.cc_items >= 1,
                   "the update that adds the sleep's item says the state and the animation: flags %d (7), animation %d (0 is Stunned), its record %lld, start %.3f (the item's %.3f), %.3f s, end %.3f (%.3f), %zu fields, %zu item(s)",
                   so.cc_flags, so.cc_anim_request, static_cast<long long>(so.cc_anim_record), so.cc_anim_start, stun_run.on_item.start_time, so.cc_anim_duration, so.cc_anim_end, want_end, so.cc_fields, so.cc_items);
            CHECKF(se.cc_flags == 0 && se.cc_anim_request == 255 && se.has_cc_anim_record && se.cc_anim_record == 0 && se.has_cc_anim_start && se.cc_anim_start == 0.0 && se.has_cc_anim_duration && se.cc_anim_duration == 0.0 &&
                       se.has_cc_anim_end && se.cc_anim_end == -1.0 && se.cc_fields == 6,
                   "the update of the tick the sleep ends clears it all: flags %d, animation %d, record %lld, start %.1f, %.1f s, end %.1f (%zu fields)", se.cc_flags, se.cc_anim_request, static_cast<long long>(se.cc_anim_record),
                   se.cc_anim_start, se.cc_anim_duration, se.cc_anim_end, se.cc_fields);
            size_t said_between = 0;
            for (size_t j = on_i + 1; j < off_i; ++j) said_between += stun_run.s[j].cc_fields;
            CHECKF(said_between == 0, "no other update says anything of the state while it sleeps (%zu fields)", said_between);
            CHECKF(stun_run.end_state.flags == 0 && stun_run.end_state.anim_request == 255 && stun_run.end_state.anim_record == 0 && stun_run.end_scale == 1.0,
                   "afterwards the creature's state is empty: flags %lld, animation %lld, record %lld, and its walker keeps %.2f of its speed", static_cast<long long>(stun_run.end_state.flags),
                   static_cast<long long>(stun_run.end_state.anim_request), static_cast<long long>(stun_run.end_state.anim_record), stun_run.end_scale);
        }
    }

    // ---- 6: a trip over a sleep.  Both are a stun (flags 7); the animation is the strongest one's, the trip's (priority 300 against 200): the trip's update names the Tripped animation, the one that ends it goes
    // back to the sleep's, and the end of the sleep clears it.
    CcRun pr_run;
    pr_run.watch = asleep;
    pr_run.watch2 = tripped;
    const bool pr_ran = cc_fight(f, tables, wolf_path, 13.0, 0, 200, 0.2, cc_player_steer(200, nullptr, 0),
                                 {CcEvent{0.5, cc_put(pr_run, condemn)}, CcEvent{2.5, cc_put(pr_run, kick, true)}, cc_probe_at(pr_run, 12.5)}, pr_run);
    CHECK(pr_ran, "a fight in which a Wolf is put to sleep and tripped two seconds later runs");
    if (pr_ran) {
        const size_t a_on = cc_at(pr_run.s, pr_run.on_at), t_on = cc_at(pr_run.s, pr_run.second_at);
        const size_t a_off = cc_off(pr_run.s, pr_run.on_at, pr_run.on_seconds), t_off = cc_off(pr_run.s, pr_run.second_at, pr_run.second_seconds);
        const bool ok = pr_run.on_at > 0 && pr_run.second_at > 0 && a_on >= 1 && a_on < t_on && t_on < t_off && t_off < a_off && a_off < pr_run.s.size() && pr_run.on_seconds > 8.0 && pr_run.second_seconds > 2.0 && pr_run.second_seconds < 5.0;
        CHECKF(ok && pr_run.watch_left[a_on] > 0.0 && pr_run.watch2_left[t_on] > 0.0 && pr_run.watch2_left[t_off] == 0.0 && pr_run.watch_left[t_off] > 0.0 && pr_run.watch_left[a_off] == 0.0,
               "the sleep (%.2f s) outlasts the trip (%.2f s) that goes on over it two seconds in, and both are on the Wolf until their time is over", pr_run.on_seconds, pr_run.second_seconds);
        if (ok) {
            const LungeSample& sa = pr_run.s[a_on];
            const LungeSample& st = pr_run.s[t_on];
            const LungeSample& sb = pr_run.s[t_off];
            const LungeSample& se = pr_run.s[a_off];
            const double trip_end = pr_run.second_item.start_time + static_cast<double>(pr_run.second_item.duration) - 1.4;
            CHECKF(sa.cc_flags == 7 && sa.cc_anim_request == 0 && sa.cc_anim_record == sleep_rec->guid && sa.cc_anim_start == pr_run.on_item.start_time && sa.cc_fields == 6,
                   "the sleep's update names the Stunned animation: flags %d, animation %d, record %lld, %zu fields", sa.cc_flags, sa.cc_anim_request, static_cast<long long>(sa.cc_anim_record), sa.cc_fields);
            CHECKF(st.cc_flags == -1 && st.cc_anim_request == 1 && st.has_cc_anim_record && st.cc_anim_record == trip_rec->guid && st.has_cc_anim_start && st.cc_anim_start == pr_run.second_item.start_time &&
                       st.has_cc_anim_duration && st.cc_anim_duration == static_cast<double>(pr_run.second_item.duration) && st.has_cc_anim_end && near(st.cc_anim_end, trip_end, 1e-6) && st.cc_fields == 5 && st.cc_items >= 1,
                   "the update that adds the trip's item names the Tripped animation over the sleep's (the flags stay 7 and are not sent): animation %d (1 is Tripped), record %lld, start %.3f (the item's %.3f), %zu fields", st.cc_anim_request,
                   static_cast<long long>(st.cc_anim_record), st.cc_anim_start, pr_run.second_item.start_time, st.cc_fields);
            CHECKF(sb.cc_flags == -1 && sb.cc_anim_request == 0 && sb.cc_anim_record == sleep_rec->guid && sb.cc_anim_start == pr_run.on_item.start_time && sb.cc_fields == 5,
                   "...and the update of the tick the trip ends goes back to the sleep's animation: animation %d, record %lld, start %.3f (the sleep's %.3f), %zu fields", sb.cc_anim_request, static_cast<long long>(sb.cc_anim_record),
                   sb.cc_anim_start, pr_run.on_item.start_time, sb.cc_fields);
            CHECKF(se.cc_flags == 0 && se.cc_anim_request == 255 && se.cc_anim_record == 0 && se.cc_anim_start == 0.0 && se.cc_anim_duration == 0.0 && se.cc_anim_end == -1.0 && se.cc_fields == 6,
                   "...and the update of the tick the sleep ends clears the state (flags %d, animation %d, %zu fields)", se.cc_flags, se.cc_anim_request, se.cc_fields);
            const size_t casts_in = cc_casts(pr_run.s, pr_run.on_at, pr_run.s[a_off].t - 1e-6), hits_in = cc_hits(pr_run.hits, pr_run.on_at, pr_run.s[a_off].t - 1e-6);
            CHECKF(casts_in == 0 && hits_in == 0, "it begins no attack and lands no hit for the whole sleep, the trip over it included (%zu attacks begun, %zu hits landed in %.2f s)", casts_in, hits_in, pr_run.s[a_off].t - pr_run.on_at);
        }
    }

    // ---- 7: the player who rooted the Wolf leaves.  What he put on the creatures ends in the update of that tick, and the Wolf walks home at once.
    CcRun leave_run;
    leave_run.watch = rooted;
    const bool leave_ran = cc_fight(f, tables, wolf_path, 8.0, 0, 14000, -1, nullptr,
                                    {CcEvent{1.0, cc_put(leave_run, bindings)}, CcEvent{1.0, cc_sent_home(leave_run)},
                                     CcEvent{3.0, [&leave_run](LungeStepContext& c) {
                                                      leave_run.left_at = c.now;
                                                      c.world.remove_player(&c.player);
                                                  }},
                                     cc_probe_at(leave_run, 7.5)},
                                    leave_run);
    CHECK(leave_ran && leave_run.walking, "a fight in which a rooted Wolf is sent home and the player who rooted it leaves runs, and the Wolf is sent home on foot");
    if (leave_ran) {
        const size_t on_i = cc_at(leave_run.s, leave_run.on_at), left_i = cc_at(leave_run.s, leave_run.left_at);
        const bool ok = on_i >= 1 && left_i > on_i + 20 && left_i < leave_run.s.size();
        CHECK(ok, "the player leaves while the root is on");
        if (ok) {
            const double before = cc_walked(leave_run.s, on_i, left_i - 1), after = cc_walked(leave_run.s, left_i + 1, left_i + 38);
            std::printf("    7 root, a walk home, the player leaves: %.1f cm walked while rooted, %.0f cm in the 2.5 s after he left\n", before, after);
            CHECKF(before <= 5.0, "rooted and sent home the Wolf does not walk home: %.1f cm in the %.2f s before the player leaves (5 cm allowed)", before, leave_run.left_at - leave_run.on_at);
            CHECKF(leave_run.watch_left[left_i] == 0.0 && leave_run.s[left_i].cc_flags == 0 && leave_run.s[left_i].cc_ocam == 0 && leave_run.s[left_i].held_effects == 0,
                   "the update of the tick the player leaves says the state cleared (flags %d, override %d) and the root is gone", leave_run.s[left_i].cc_flags, leave_run.s[left_i].cc_ocam);
            CHECKF(after >= 500.0, "...and the Wolf walks home at once: %.0f cm in 2.5 s", after);
            CHECKF(leave_run.end_state.flags == 0 && !leave_run.end_state.ocam && leave_run.end_scale == 1.0, "afterwards the creature's state is empty (flags %lld) and its walker keeps %.2f of its speed",
                   static_cast<long long>(leave_run.end_state.flags), leave_run.end_scale);
        }
    }

    // ---- 8: the Wolf dies asleep.  The state is cleared in the update of its death, and nothing is left stuck: the NPC world walks it again when it is given a walk.
    CcRun dead_run;
    dead_run.watch = asleep;
    bool walk_given = false;
    const bool dead_ran = cc_fight(f, tables, wolf_path, 6.0, 0, 200, 0.2, cc_player_steer(200, nullptr, 0),
                                   {CcEvent{1.0, cc_put(dead_run, condemn)},
                                    CcEvent{3.0, [&dead_run](LungeStepContext& c) {
                                                     dead_run.died_at = c.now;
                                                     combat::Resolved lethal;
                                                     lethal.mods.push_back({1, content::kGuidHealth, -1.0e9});
                                                     c.world.apply_hit_to_npc(c.creature, c.player.actor_id(), lethal, c.now);
                                                 }},
                                    CcEvent{3.6, [&dead_run, &walk_given](LungeStepContext& c) {
                                                     const npc::Mover* m = c.w.npcs->mover(c.creature);
                                                     if (!m) return;
                                                     const nav::Vec3 g = m->ground();
                                                     dead_run.second_at = c.now;
                                                     walk_given = c.w.npcs->order_move(c.creature, nav::Vec3{g.x, g.y + 600.0, g.z}, 400.0, 0.0);
                                                 }},
                                    cc_probe_at(dead_run, 5.5)},
                                   dead_run);
    CHECK(dead_ran, "a fight in which a sleeping Wolf is killed runs");
    if (dead_ran) {
        const size_t on_i = cc_at(dead_run.s, dead_run.on_at), died_i = cc_at(dead_run.s, dead_run.died_at), walk_i = cc_at(dead_run.s, dead_run.second_at);
        const bool ok = on_i >= 1 && died_i > on_i && walk_i > died_i && walk_i + 25 < dead_run.s.size() && dead_run.on_seconds > 5.0;
        CHECK(ok, "the Wolf is killed while the sleep is on");
        if (ok) {
            const double walked = cc_walked(dead_run.s, walk_i, walk_i + 24);
            CHECKF(dead_run.watch_left[died_i] == 0.0 && dead_run.s[died_i].cc_flags == 0 && dead_run.s[died_i].cc_anim_request == 255 && dead_run.s[died_i].cc_fields == 6 && dead_run.s[died_i].held_effects == 0,
                   "the update of the tick it dies says the state cleared (flags %d, animation %d, %zu fields) and it holds nothing", dead_run.s[died_i].cc_flags, dead_run.s[died_i].cc_anim_request, dead_run.s[died_i].cc_fields);
            CHECKF(walk_given && walked >= 300.0, "nothing of the sleep is left on its walker: given a walk after its death it covers %.0f cm in 1.6 s", walked);
            CHECKF(dead_run.end_state.flags == 0 && dead_run.end_state.anim_request == 255 && dead_run.end_scale == 1.0, "...and its state is empty (flags %lld, animation %lld) with its walker keeping %.2f of its speed",
                   static_cast<long long>(dead_run.end_state.flags), static_cast<long long>(dead_run.end_state.anim_request), dead_run.end_scale);
        }
    }

    // ---- 9: the walk home.  A Wolf that is rooted and sent home (a game master's reset) stays where it is and walks home when the root is off, at the speed of a free Wolf's walk home.
    CcRun home_ctl, home_run;
    home_run.watch = rooted;
    const bool home_ran = cc_fight(f, tables, wolf_path, 11.0, 0, 14000, -1, nullptr, {CcEvent{1.0, cc_sent_home(home_ctl)}}, home_ctl) &&
                          cc_fight(f, tables, wolf_path, 11.0, 0, 14000, -1, nullptr, {CcEvent{1.0, cc_put(home_run, bindings)}, CcEvent{1.0, cc_sent_home(home_run)}, cc_probe_at(home_run, 10.5)}, home_run);
    CHECK(home_ran && home_ctl.walking && home_run.walking, "two walks home run, one of a free Wolf and one of a rooted one, and the Wolf is sent home on foot in both");
    if (home_ran) {
        const size_t on_i = cc_at(home_run.s, home_run.on_at);
        const size_t off_i = cc_off(home_run.s, home_run.on_at, home_run.on_seconds);
        const bool ok = home_run.on_at > 0 && on_i >= 1 && off_i + 60 < home_run.s.size() && home_run.on_seconds > 3.0;
        CHECK(ok && home_run.watch_left[on_i] > 0.0 && home_run.watch_left[off_i] == 0.0, "the root is on the Wolf for a few seconds of the walk home and off when its time is over");
        if (ok) {
            const double off_t = home_run.s[off_i].t;
            const CcSpeed free_walk = cc_speed(home_ctl.s, home_ctl.reset_at + 0.5, home_ctl.reset_at + 5.0);
            const CcSpeed after = cc_speed(home_run.s, off_t + 0.4, off_t + 2.9);
            std::printf("    9 root on the walk home: %.1f cm walked while it was on, then %.1f cm/s (the updates say %.1f); a free Wolf walks home at %.1f cm/s\n", cc_walked(home_run.s, on_i, off_i), after.walked, after.wire, free_walk.walked);
            CHECKF(cc_walked(home_run.s, on_i, off_i) <= 5.0, "rooted on its way home it stays where it is: %.1f cm walked in the %.2f s of the root (5 cm allowed)", cc_walked(home_run.s, on_i, off_i), off_t - home_run.on_at);
            CHECKF(cc_casts(home_run.s, home_run.on_at, home_run.s.back().t) == 0, "...and it begins no fight on the way");
            CHECKF(free_walk.ticks >= 20 && after.ticks >= 20 && near(after.walked, free_walk.walked, 0.015 * free_walk.walked) && near(after.wire, free_walk.wire, 0.015 * free_walk.wire + 2.0),
                   "when the root has run out it walks home at the speed of a free Wolf (%.1f cm/s, the updates say %.1f; %.1f free)", after.walked, after.wire, free_walk.walked);
            CHECKF(home_run.end_state.flags == 0 && home_run.end_scale == 1.0, "afterwards the creature's state is empty (flags %lld) and its walker keeps %.2f of its speed", static_cast<long long>(home_run.end_state.flags), home_run.end_scale);
        }
    }

    // ---- 10: the Ranger's Air Strike roots a Wolf that a slow is on (the Disengage first): the root's update says the flags of both, and the Wolf stands.
    const abilities::HitDef* disengage = f.catalog->hit_named("Ranger_Disengage_Hit");
    const abilities::HitDef* air_strike = f.catalog->hit_named("Ranger_AirStrike_AoE");
    CHECK(disengage && air_strike, "the data has the Ranger's Disengage hit and Air Strike's hit");
    if (disengage && air_strike) {
        CcRun air_run;
        air_run.watch = rooted;
        const bool air_ran = cc_fight(f, tables, wolf_path, 9.0, 0, 800, 0.2, cc_player_steer(800, nullptr, 0),
                                      {CcEvent{0.2, [disengage](LungeStepContext& c) { cc_land(c, *disengage); }},
                                       CcEvent{0.5, [&air_run, air_strike](LungeStepContext& c) {
                                                        cc_land(c, *air_strike);
                                                        air_run.on_at = c.now;
                                                    }}},
                                      air_run);
        CHECK(air_ran, "a fight in which a Wolf is snared and then hit by Air Strike runs");
        if (air_ran) {
            const size_t on_i = cc_at(air_run.s, air_run.on_at);
            size_t off_i = on_i;
            while (off_i < air_run.watch_left.size() && air_run.watch_left[off_i] > 0) ++off_i;
            const bool ok = air_run.on_at > 0 && on_i >= 1 && on_i < air_run.s.size() && air_run.watch_left[on_i] > 0.0 && off_i > on_i + 30 && off_i < air_run.s.size();
            CHECKF(ok, "Air Strike roots the snared Wolf (%zu ticks of root)", off_i - on_i);
            if (ok) {
                CHECKF(air_run.s[on_i].cc_flags == 17 && air_run.s[on_i].cc_ocam == 1 && air_run.s[on_i].cc_fields == 2 && air_run.s[on_i].cc_items >= 1,
                       "the update that adds the root to the snare says flags %d (17: the snare's 16 and the root's 1) and the override %d, and no more (%zu fields)", air_run.s[on_i].cc_flags, air_run.s[on_i].cc_ocam,
                       air_run.s[on_i].cc_fields);
                CHECKF(cc_walked(air_run.s, on_i - 1, off_i) <= 5.0, "...and the Wolf stands where it is while the root is on: %.1f cm walked in %.2f s (5 cm allowed)", cc_walked(air_run.s, on_i - 1, off_i),
                       air_run.s[off_i].t - air_run.on_at);
                CHECKF(air_run.s[off_i].cc_flags == 16 && air_run.s[off_i].cc_ocam == 0, "...and when the root ends the snare is left: flags %d, override %d", air_run.s[off_i].cc_flags, air_run.s[off_i].cc_ocam);
            }
        }
    }

    // ---- 11: a Wolf that wanders (the NPC world's own walk, with no fight) stands for the length of a root, and wanders again.
    // (the root lasts 7 s: a root that is kept on longer is taken off by its lockout, whose build-up gains a stack every half second and whose limit is 16)
    const CcWanderRun wander = cc_wander_rooted(f, wolf_path, 25.0, 7.0, 150.0);
    const CcWanderRun free_wander = cc_wander_rooted(f, wolf_path, 25.0, 0.05, 150.0, 7.0);   // (the same Wolf with no root to speak of: what it covers in the same 7 s)
    std::printf("    11 wander: %.0f cm walked before the root, %.1f cm in the 7 s of it (a free Wolf %.0f cm), %.0f cm in the 118 s after it\n", wander.before, wander.during, free_wander.during, wander.after);
    CHECK(wander.before >= 0 && free_wander.before >= 0, "a Wolf that wanders, with a root put on it 25 s in, can be made");
    if (wander.before >= 0 && free_wander.before >= 0) {
        CHECKF(free_wander.during > 100.0, "the Wolf is walking at that moment: a free one covers %.0f cm in the 7 s", free_wander.during);
        CHECKF(wander.during <= 5.0, "a rooted Wolf does not wander: %.1f cm walked in the 7 s of the root (5 cm allowed)", wander.during);
        CHECKF(wander.before > 100.0 && wander.after > 100.0, "...and it wanders before the root and after it (%.0f and %.0f cm)", wander.before, wander.after);
        CHECKF(wander.scale == 1.0, "...and its walker keeps %.2f of its speed at the end", wander.scale);
    }
}
AOC_REGISTER_TEST(test_cc_binds_creatures);

// A freeze that lands while a montage carries a tracked Minotaur swing, at a player who runs round it: a freeze clears every turn a creature is on (as it does for a Rotate To), so the
// facing stays where it was held for the rest of the carry; the swing itself plays on (a freeze does not end the attack, a stun does).
void test_carried_swing_drops_its_track_under_a_freeze(Fixture& f) {
    std::printf("a Minotaur Berserker whose axe swing carries it at a player who runs round it: a freeze that lands in the carry drops the track, and the facing does not turn while it is held\n");
    constexpr double kPi = 3.14159265358979323846;
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string mino = creature_path(f, "/Minotaur/Berserker/Minotaur_Berserker.Default__Minotaur_Berserker_C");
    const auto bp = f.motion.find(mino);
    const abilities::HitDef* frostbolt = f.catalog->hit_named("Mage_Frostbolt_Hit");
    const abilities::HitDef* cone = f.catalog->hit_named("Mage_ConeOfCold_Hit");
    const abilities::EffectDef* frozen = f.catalog->effect_named("Status_Frozen");
    BanditOpen made;
    const bool have = !mino.empty() && bp && tables.abilities_of(mino) && frostbolt && cone && frozen && make_bandit_open(f, tables, mino, made) && made.open;
    CHECK(have, "the Blueprint table has the Minotaur Berserker, the world makes an open for it, and the data has the Frostbolt's chill and the Cone of Cold's freeze");
    if (!have) return;
    const double dt = 1.0 / 15.0;
    // from its first swing on, the player runs round the place it swung from, at the distance he had (as in the Minotaur test)
    bool started = false;
    double angle = 0, radius = 0, cx = 0, cy = 0;
    const LungeSteer steer = [&](double, const LungeSample* prev, double& px, double& py, const world::WorldActor& c) {
        if (!started) {
            if (!prev || !prev->cast_began) return false;
            started = true;
            cx = c.x;
            cy = c.y;
            angle = std::atan2(py - cy, px - cx);
            radius = std::max(150.0, std::hypot(px - cx, py - cy));
        }
        angle += 90.0 * kPi / 180.0 * dt;
        px = cx + radius * std::cos(angle);
        py = cy + radius * std::sin(angle);
        return true;
    };
    CcRun run;
    run.watch = frozen;
    double cone_seconds = 0;
    const auto put_frostbolt = [frostbolt](LungeStepContext& c) { cc_land(c, *frostbolt); };
    const auto put_cone = [&run, &cone_seconds, cone](LungeStepContext& c) {
        cone_seconds = cc_land(c, *cone);   // (the Cone of Cold's freeze wants the chill the Frostbolt leaves)
        run.on_at = c.now;
        run.on_seconds = cone_seconds;
    };
    const bool ran = cc_fight(f, tables, mino, 40.0, 0, 450, 0.2, steer,
                              {CcEvent{0.2, put_frostbolt}, CcEvent{0.0, put_cone, false, [&run](LungeStepContext&) { return !run.s.empty() && run.s.back().active == 1; }}, cc_probe_at(run, 39.5)}, run,
                              made.open);
    CHECK(ran, "a fight of 40 s against a player who runs round the Minotaur, with a freeze put on it while a montage carries its swing, runs");
    if (!ran) return;
    const size_t on_i = cc_at(run.s, run.on_at);
    size_t off_i = on_i;
    while (off_i < run.watch_left.size() && run.watch_left[off_i] > 0) ++off_i;
    size_t carried = 0, active_ticks = 0;
    for (const LungeSample& s : run.s) active_ticks += s.active == 1 ? 1 : 0;
    for (size_t k = on_i; k < off_i && k < run.s.size(); ++k) carried += run.s[k].active == 1 ? 1 : 0;
    const bool ok = run.on_at > 0 && on_i >= 1 && on_i < run.s.size() && off_i > on_i && off_i < run.s.size() && carried >= 3;
    CHECKF(ok, "the freeze lands in a swing its montage carries and holds it for some ticks (%zu carried ticks in it, %zu in the fight; the freeze lasted %.2f s)", carried, active_ticks, cone_seconds);
    if (!ok) return;
    size_t cast_i = on_i;
    while (cast_i > 0 && !run.s[cast_i].cast_began) --cast_i;
    size_t last = cast_i;   // the end of that cast
    while (last + 1 < run.s.size() && run.s[last + 1].montage != 0 && !run.s[last + 1].cast_began) ++last;
    std::string cast_name;
    if (const abilities::AbilityDef* def = f.catalog->ability(run.s[cast_i].ability)) cast_name = def->row.name;
    const double turned = std::fabs(wrap180(run.s[off_i - 1].yaw - run.s[on_i].yaw));
    const double a0 = std::atan2(run.s[on_i].py - run.s[on_i].wy, run.s[on_i].px - run.s[on_i].wx);
    const double a1 = std::atan2(run.s[off_i - 1].py - run.s[off_i - 1].wy, run.s[off_i - 1].px - run.s[off_i - 1].wx);
    const double circled = std::fabs(wrap180((a1 - a0) * 180.0 / kPi));
    // if the carry goes on after the freeze, the facing stays then too: the track the swing began with is gone
    size_t after_carried = 0;
    double after_turn = 0;
    for (size_t k = off_i; k <= last && k < run.s.size(); ++k) {
        if (run.s[k].active != 1) continue;
        ++after_carried;
        after_turn = std::max(after_turn, std::fabs(wrap180(run.s[k].yaw - run.s[off_i - 1].yaw)));
    }
    std::printf("  %s, carried in the freeze for %zu of its %zu ticks (%.2f s): the player went %.0f deg round it, its facing turned %.1f deg; after the freeze %zu carried ticks, facing turned %.1f deg\n", cast_name.c_str(),
                carried, off_i - on_i, run.s[off_i - 1].t - run.s[on_i].t, circled, turned, after_carried, after_turn);
    CHECKF(turned <= 5.0 && circled >= 30.0, "... and the facing does not turn while it is held, though the player runs round it (%.0f deg round, facing turned %.1f deg; 5 allowed)", circled, turned);
    CHECKF(after_carried == 0 || after_turn <= 5.0, "... and the carry after the freeze does not turn it either: the track is dropped (%zu carried ticks after it, facing turned %.1f deg)", after_carried, after_turn);
}

AOC_REGISTER_TEST(test_carried_swing_drops_its_track_under_a_freeze);

// ============================================================================================================================================================================
//  The Bard's Marionette and Dark Lullaby on creatures.
//
//  Marionette is a ten-second full stun (root, no turn, no abilities) whose crowd-control record asks for an animation of its own (the dance): the creature takes the flags and the client
//  plays the dance itself, so the server sends no animation.  Damage taken while it lasts breaks it once it adds up to a tenth of the Bard's maximum Health, and its end puts Humiliated on
//  the creature for eight seconds.  The Dark Lullaby is a cast of four Enemy hits at its montage's notifies; the last one also runs its second hit (the stun) on the targets it lands on,
//  only those that carry the Debuff.Incapacitated tag (the tooltip's final hit).  Its six-second area stun ends early when the game's own T2 lockout reaches ten stacks.
// ============================================================================================================================================================================

/// A character of the class a test passes (a Bard by default) at level 25 on the combat rig, the pipeline connected to a world, and the world's registry for the creatures a test spawns:
/// the shape of combat_player_cast_tests.inc's world, with the ability rig's `asks` off (the Lullaby's own hits are the server's; a test asks for the other hits it means).
struct BardMobWorld {
    Rig                                     rig{2, 25, {}};
    combat_rig::World                       w;
    std::optional<combat::NpcStatBook>      book;
    std::unique_ptr<combat_rig::FakePlayer> player;
    std::unique_ptr<combat::CombatWorld>    world;
    std::unique_ptr<combat::WorldHitPort>   port;
    std::string                             problem;

    BardMobWorld(Fixture& f, const std::string& path, int class_enum = 2) : rig(class_enum, 25, {}) {
        if (!rig.ok()) problem = "the rig: " + rig.problem;
        else if (!w.problem.empty()) problem = w.problem;
        else if (path.empty()) problem = "the Blueprint is not in the table";
        if (!problem.empty()) return;
        book.emplace(*f.ctx->bundle, *f.ctx->gear);
        player = std::make_unique<combat_rig::FakePlayer>(rig, 77, combat::Vec3{0, 0, 1098});
        world = std::make_unique<combat::CombatWorld>(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, *book, f.motion);
        world->add_player(player.get());
        port = std::make_unique<combat::WorldHitPort>(*world, w.registry, player->fighter());
        rig.pipe->set_combat(port.get());
        rig.know({"Bard_Misc_Marionette", "Bard_Song_Lullaby", "Bard_Song_Lullaby_AOE"});
        rig.asks = false;
    }
};

/// Runs the ability runtime and the combat world to `to` in the world's ticks, keeping the Bard alive (the creature fights back), and collects the runtime's events.
static void bard_run(BardMobWorld& bw, double from, double to, std::vector<Seen>* seen = nullptr) {
    for (double now = from; now <= to + 1e-9; now += 1.0 / 15.0) {
        bw.rig.set_current(content::kGuidHealth, bw.rig.pawn.stat(content::kGuidMaxHealth));
        for (Seen& s : bw.rig.take()) {   // (the events of an earlier call, such as a hit the cast landed before this run)
            if (seen) seen->push_back(std::move(s));
        }
        for (Seen& s : bw.rig.run_to(now)) {
            if (seen) seen->push_back(std::move(s));
        }
        bw.world->tick(now);
    }
}

/// The moment the runtime ran a direct hit, from its note ("the hit <hit> of <ability> ...").
static double bard_hit_at(const std::vector<Seen>& seen, const char* hit, const char* ability) {
    const std::string want = std::string("the hit ") + hit + " of " + ability;
    for (const Seen& s : seen) {
        if (s.note.rfind(want, 0) == 0) return s.at;
    }
    return -1.0;
}

/// The confirmation of a hit record in the runtime's events (nullptr: it was not confirmed).
static const bootstrap::HitConfirm* bard_confirmed(const std::vector<Seen>& seen, int64_t record) {
    for (const Seen& s : seen) {
        for (const bootstrap::HitConfirm& c : s.confirmations) {
            if (c.record_guid == record) return &c;
        }
    }
    return nullptr;
}

/// The crowd control state of the creature, read from its replicated stats component.
static CcNow bard_cc(BardMobWorld& bw, uint64_t creature) {
    const world::WorldActor* a = bw.w.registry.find(creature);
    return a ? cc_now(*a) : CcNow{};
}

/// A cast of `ability` at `targets`, the client's request for `first_hit` alone (the client asks for the first hit and not the second), and the world run to `until`.
struct BardCast {
    bool              cast = false;
    bool              requested = false;
    int32_t           client_id = 0;
    int64_t           server_instance = 0;   // the ability item's ServerInstanceId (the client's ServerEndAbility names it)
    std::vector<Seen> seen;
};

static BardCast bard_cast(BardMobWorld& bw, const char* ability, const std::vector<uint64_t>& targets, const char* first_hit, double t, double until) {
    BardCast out;
    const int64_t guid = bw.rig.ability(ability);
    const abilities::AbilityDef* def = bw.rig.catalog->ability(guid);
    if (!def) return out;
    TriggerInfo info;
    info.target_count = static_cast<int>(targets.size());
    info.target_actors = targets;
    out.client_id = bw.rig.next_client_id;
    out.cast = bw.rig.pipe->trigger(guid, bw.rig.next_client_id++, t, info) == Refusal::None;
    if (!out.cast) return out;
    int64_t targeting = content::kTargetingDefaultSelf, hit_guid = 0;
    for (const content::AbilityHitRow& h : def->hits) {
        const abilities::HitDef* hd = bw.rig.catalog->hit(h.hit_guid);
        if (!hd || hd->row.name != first_hit) continue;
        hit_guid = h.hit_guid;
        if (h.source == "hit") {
            for (const content::TargetingRow& tg : def->targetings) {
                if (tg.key == h.key) targeting = tg.guid;
            }
        }
    }
    if (hit_guid == 0) return out;
    out.requested = bw.rig.pipe->hit_request(guid, targeting, hit_guid, 1, 0, t + 0.02, targets) == AbilityPipeline::HitVerdict::Scheduled;
    bard_run(bw, t, until, &out.seen);
    if (const bootstrap::ActiveAbilityItem* item = bw.rig.pawn.ability_item()) out.server_instance = item->server_instance_id;
    return out;
}

void test_bard_marionette_stun(Fixture& f) {
    std::printf("the Bard's Marionette: a ten-second full stun with no animation of the server's, Humiliated at its end, over another stun, and on a boss\n");
    const std::string wolf_path = wolf_archetype(f);
    const abilities::EffectDef* marionette = f.catalog->effect_named("Bard_Marionette");
    const abilities::EffectDef* humiliated = f.catalog->effect_named("Status_Humiliated");
    const abilities::EffectDef* stunned = f.catalog->effect_named("Status_Stunned");
    CHECK(marionette && humiliated && stunned && !wolf_path.empty(), "the data has the Marionette, Humiliated and Stunned effects and the Wolf");
    if (!marionette || !humiliated || !stunned || wolf_path.empty()) return;
    CHECKF(marionette->cc && marionette->cc->applied_status == 7 && marionette->cc->animation_request.find("Custom") != std::string::npos,
           "the Marionette's crowd control is a full stun (status 7) that asks for an animation of its own (%s)", marionette->cc ? marionette->cc->name.c_str() : "none");
    if (!marionette->cc) return;

    // ---- 1: a Wolf takes the ten-second stun, no animation, and Humiliated at its end
    {
        BardMobWorld bw(f, wolf_path);
        CHECKF(bw.problem.empty(), "the Bard, the world and the Wolf: %s", bw.problem.c_str());
        if (!bw.problem.empty()) return;
        const uint64_t wolf = bw.w.spawn(wolf_path, 600, 0, 8, 0);
        CHECK(wolf != 0, "a Wolf 6 m from the Bard");
        if (!wolf) return;
        const double t = 100.0;
        const BardCast cast = bard_cast(bw, "Bard_Misc_Marionette", {wolf}, "Bard_Marionette", t, t + 0.8);
        CHECK(cast.cast && cast.requested, "the cast and the client's request for its hit are accepted");
        const double hit_at = bard_hit_at(cast.seen, "Bard_Marionette", "Bard_Misc_Marionette");
        CHECKF(hit_at > 0.0, "the hit lands (%.2f)", hit_at);
        if (hit_at <= 0.0) return;
        const double left = bw.world->effect_remaining(wolf, marionette->row.guid, hit_at);
        const CcNow cc = bard_cc(bw, wolf);
        CHECKF(bw.world->effect_stacks(wolf, marionette->row.guid) == 1 && near(left, 10.0, 1e-6), "the Wolf takes the Marionette for its own 10 s (%.3f s left at the hit)", left);
        CHECKF(cc.flags == 7 && cc.anim_request == content::kCcAnimNone && cc.anim_record == 0,
               "the crowd control state is a full stun (flags %lld: root + no turn + no abilities) and no animation is sent (request %lld, record %lld)", static_cast<long long>(cc.flags),
               static_cast<long long>(cc.anim_request), static_cast<long long>(cc.anim_record));
        bard_run(bw, t + 0.8, hit_at + 9.9);
        CHECKF(bw.world->effect_stacks(wolf, marionette->row.guid) == 1 && bard_cc(bw, wolf).flags == 7, "it still holds the Wolf at 9.9 s");
        bard_run(bw, hit_at + 9.9, hit_at + 10.0 + 1.0 / 15.0);
        const CcNow after = bard_cc(bw, wolf);
        const double hum_left = bw.world->effect_remaining(wolf, humiliated->row.guid, hit_at + 10.0 + 1.0 / 15.0);
        CHECKF(bw.world->effect_stacks(wolf, marionette->row.guid) == 0 && after.flags == 0 && after.anim_request == content::kCcAnimNone,
               "after 10 s the stun is over (flags %lld, animation %lld)", static_cast<long long>(after.flags), static_cast<long long>(after.anim_request));
        CHECKF(bw.world->effect_stacks(wolf, humiliated->row.guid) == 1 && near(hum_left, 8.0, 0.15), "Humiliated is on the Wolf right after it ends (%.2f s left, the data's 8)", hum_left);
        std::printf("    the Marionette lands %.3f s after the trigger: %.3f s left, flags %lld, animation %lld; Humiliated %.3f s left after it ends\n", hit_at - t, left, static_cast<long long>(cc.flags),
                    static_cast<long long>(cc.anim_request), hum_left);
    }

    // ---- 3: a Wolf already stunned by Status_Stunned: the flags are the two sets together and the stronger animation stays
    {
        BardMobWorld bw(f, wolf_path);
        CHECKF(bw.problem.empty(), "the Bard, the world and the second Wolf: %s", bw.problem.c_str());
        if (!bw.problem.empty()) return;
        const uint64_t wolf = bw.w.spawn(wolf_path, 600, 0, 8, 0);
        CHECK(wolf != 0, "a Wolf 6 m from the Bard");
        if (!wolf) return;
        const double t = 100.0;
        effects::Applied already;
        already.def = stunned;
        already.duration = 4.0;
        bw.world->apply_effects(wolf, 77, {already}, t);
        const CcNow before = bard_cc(bw, wolf);
        CHECKF(before.flags == 7 && before.anim_request == content::kCcAnimStunned && before.anim_record == stunned->cc->guid,
               "the Wolf is stunned by Status_Stunned first (flags %lld, animation %lld, record %lld)", static_cast<long long>(before.flags), static_cast<long long>(before.anim_request),
               static_cast<long long>(before.anim_record));
        const BardCast over = bard_cast(bw, "Bard_Misc_Marionette", {wolf}, "Bard_Marionette", t + 0.5, t + 1.3);
        const double at = bard_hit_at(over.seen, "Bard_Marionette", "Bard_Misc_Marionette");
        const CcNow both = bard_cc(bw, wolf);
        CHECKF(over.cast && over.requested && at > 0.0 && bw.world->effect_stacks(wolf, marionette->row.guid) == 1 && bw.world->effect_stacks(wolf, stunned->row.guid) == 1,
               "the Marionette goes on over the stun (both effects on the Wolf)");
        CHECKF(both.flags == 7 && both.anim_request == content::kCcAnimStunned && both.anim_record == stunned->cc->guid,
               "the flags stay the two sets together (7) and the stronger animation (General_Stun, 200 over the dance's 0) stays (flags %lld, animation %lld, record %lld)",
               static_cast<long long>(both.flags), static_cast<long long>(both.anim_request), static_cast<long long>(both.anim_record));
        std::printf("    over Status_Stunned: flags %lld, animation %lld, record %lld (the stun's own)\n", static_cast<long long>(both.flags), static_cast<long long>(both.anim_request),
                    static_cast<long long>(both.anim_record));
    }

    // ---- 7: a boss takes it (no boss rule for a stun in this job)
    const std::string boss_path = creature_path(f, "/GiantCyclops/CyclopsShaman.Default__CyclopsShaman_C");
    CHECK(!boss_path.empty(), "the Blueprint table has a boss (the Cyclops Shaman)");
    if (!boss_path.empty()) {
        BardMobWorld bw(f, boss_path);
        CHECKF(bw.problem.empty(), "the Bard and the boss's world: %s", bw.problem.c_str());
        if (!bw.problem.empty()) return;
        const uint64_t boss = bw.w.spawn(boss_path, 900, 0, 45, 0);
        CHECK(boss != 0, "a boss 9 m from the Bard");
        if (!boss) return;
        const double t = 100.0;
        const BardCast bc = bard_cast(bw, "Bard_Misc_Marionette", {boss}, "Bard_Marionette", t, t + 0.8);
        const double bat = bard_hit_at(bc.seen, "Bard_Marionette", "Bard_Misc_Marionette");
        const CcNow bcc = bard_cc(bw, boss);
        CHECKF(bc.cast && bc.requested && bat > 0.0 && bw.world->effect_stacks(boss, marionette->row.guid) == 1 && bcc.flags == 7,
               "the boss takes the Marionette like any creature (flags %lld)", static_cast<long long>(bcc.flags));
    }
}
AOC_REGISTER_TEST(test_bard_marionette_stun);

void test_bard_marionette_break(Fixture& f) {
    std::printf("the Marionette's break: a tenth of the Bard's maximum Health in damage taken ends it at that hit, one point less does not, and Humiliated comes when it ends either way\n");
    const std::string wolf_path = wolf_archetype(f);
    const abilities::EffectDef* marionette = f.catalog->effect_named("Bard_Marionette");
    const abilities::EffectDef* humiliated = f.catalog->effect_named("Status_Humiliated");
    CHECK(marionette && humiliated && !wolf_path.empty(), "the data has the Marionette and Humiliated effects and the Wolf");
    if (!marionette || !humiliated || wolf_path.empty()) return;

    // ---- 2a: damage of a tenth of the Bard's maximum Health less one does not break it, and it lasts its ten seconds
    {
        BardMobWorld bw(f, wolf_path);
        CHECKF(bw.problem.empty(), "the Bard, the world and the Wolf: %s", bw.problem.c_str());
        if (!bw.problem.empty()) return;
        const uint64_t wolf = bw.w.spawn(wolf_path, 600, 0, 8, 0);
        CHECK(wolf != 0, "a Wolf 6 m from the Bard");
        if (!wolf) return;
        const double t = 100.0;
        const BardCast cast = bard_cast(bw, "Bard_Misc_Marionette", {wolf}, "Bard_Marionette", t, t + 0.8);
        const double hit_at = bard_hit_at(cast.seen, "Bard_Marionette", "Bard_Misc_Marionette");
        CHECKF(cast.cast && cast.requested && hit_at > 0.0 && bw.world->effect_stacks(wolf, marionette->row.guid) == 1, "the Wolf takes the Marionette");
        if (!(hit_at > 0.0)) return;
        const double h = bw.rig.max_health();
        CHECKF(h > 100.0, "the Bard's maximum Health is %.1f", h);
        const double below = 0.1 * h - 1.0;
        combat::Resolved small;
        small.mods.push_back({1, content::kGuidHealth, -below});
        bw.world->apply_hit_to_npc(wolf, 77, small, hit_at + 1.0);
        const double small_left = bw.world->effect_remaining(wolf, marionette->row.guid, hit_at + 1.0);
        CHECKF(bw.world->effect_stacks(wolf, marionette->row.guid) == 1 && small_left > 8.0,
               "damage of 0.1 x H - 1 (%.1f) does not break it: it is still on with %.1f s left", below, small_left);
        bard_run(bw, t + 0.8, hit_at + 10.0 + 1.0 / 15.0);
        const double hum_a = bw.world->effect_remaining(wolf, humiliated->row.guid, hit_at + 10.0 + 1.0 / 15.0);
        CHECKF(bw.world->effect_stacks(wolf, marionette->row.guid) == 0, "the Wolf hit for less than the tenth keeps it for the 10 s and then it ends");
        CHECKF(bw.world->effect_stacks(wolf, humiliated->row.guid) == 1 && near(hum_a, 8.0, 0.15), "Humiliated comes when it ends (%.2f s left)", hum_a);
        std::printf("    H %.1f: 0.1 x H - 1 = %.1f does not break it (%.1f s left at the hit); Humiliated %.3f s left when it ends\n", h, below, small_left, hum_a);
    }

    // ---- 2b: damage of a tenth of the Bard's maximum Health plus one breaks it at that hit
    {
        BardMobWorld bw(f, wolf_path);
        CHECKF(bw.problem.empty(), "the Bard, the world and the second Wolf: %s", bw.problem.c_str());
        if (!bw.problem.empty()) return;
        const uint64_t wolf = bw.w.spawn(wolf_path, 600, 0, 8, 0);
        CHECK(wolf != 0, "a Wolf 6 m from the Bard");
        if (!wolf) return;
        const double t = 100.0;
        const BardCast cast = bard_cast(bw, "Bard_Misc_Marionette", {wolf}, "Bard_Marionette", t, t + 0.8);
        const double hit_at = bard_hit_at(cast.seen, "Bard_Marionette", "Bard_Misc_Marionette");
        CHECKF(cast.cast && cast.requested && hit_at > 0.0 && bw.world->effect_stacks(wolf, marionette->row.guid) == 1, "the Wolf takes the Marionette");
        if (!(hit_at > 0.0)) return;
        const double h = bw.rig.max_health();
        const double above = 0.1 * h + 1.0;
        combat::Resolved big;
        big.mods.push_back({1, content::kGuidHealth, -above});
        bw.world->apply_hit_to_npc(wolf, 77, big, hit_at + 1.0);
        const double hum_left = bw.world->effect_remaining(wolf, humiliated->row.guid, hit_at + 1.0);
        CHECKF(bw.world->effect_stacks(wolf, marionette->row.guid) == 0, "damage of 0.1 x H + 1 (%.1f) breaks it at that hit", above);
        CHECKF(bw.world->effect_stacks(wolf, humiliated->row.guid) == 1 && near(hum_left, 8.0, 1e-6), "Humiliated is on the broken Wolf right after it ends (%.3f s left, the data's 8)", hum_left);
        std::printf("    H %.1f: 0.1 x H + 1 = %.1f breaks it at that hit; Humiliated %.3f s left\n", h, above, hum_left);
    }
}
AOC_REGISTER_TEST(test_bard_marionette_break);


// The client's own montage of the Dark Lullaby (ABLT_Bard_Lullaby_Enemy) fires its key-1 hit, the Enemy damage hit, four times: at 0.01, 0.7300865, 1.5004842 and
// 2.317239 s of montage time at a play rate of 1 (the montage's hit notifies; both shapes play this montage). The last of them is the cast's final hit. The ability's
// tooltip: the final hit applies the second hit (the Stun) if the target is Incapacitated.
constexpr double kLullabyHits[] = {0.01, 0.7300865, 1.5004842, 2.317239};

/// How fast the Lullaby's montage plays here: the ability's SpeedMultiplierSource is CastSpeed, so the montage and its hit notifies run at the caster's MagicalSpeed
/// (the stat the runtime's client_rate reads; the capture's Bard played at 0.9833).
static double lullaby_scale(BardMobWorld& bw) {
    const stats::StatValue* speed = bw.rig.cs ? bw.rig.cs->find(109343399106257ll) : nullptr;
    const double rate = speed ? speed->value : 1.0;
    return (rate > 0.2 && rate < 3.0) ? 1.0 / rate : 1.0;
}

/// Every moment the runtime ran the direct hit `hit` of `ability`, in order (its events' notes).
static std::vector<double> bard_hit_times(const std::vector<Seen>& seen, const char* hit, const char* ability) {
    const std::string want = std::string("the hit ") + hit + " of " + ability;
    std::vector<double> out;
    for (const Seen& s : seen) {
        if (s.note.rfind(want, 0) == 0) out.push_back(s.at);
    }
    return out;
}

/// The cast's four Enemy hits land at the montage's notifies, scaled by the play rate.
static void lullaby_check_times(const std::vector<Seen>& seen, const char* ability, const char* damage, double t, double scale) {
    const std::vector<double> at = bard_hit_times(seen, damage, ability);
    CHECKF(at.size() == 4, "the cast's damage hit runs four times (%zu)", at.size());
    for (size_t i = 0; i < at.size() && i < 4; ++i) {
        CHECKF(near(at[i] - t, kLullabyHits[i] * scale, 1e-4), "hit %zu lands %.4f s after the trigger (the montage's %.4f s at the play rate)", i + 1, at[i] - t,
               kLullabyHits[i] * scale);
    }
}

void test_bard_lullaby_cast(Fixture& f) {
    std::printf("the Dark Lullaby's cast: four Enemy hits at the montage's notifies, no stun before the last, and the last hit's stun on an Incapacitated target (4 s single, 6 s area)\n");
    const std::string wolf_path = wolf_archetype(f);
    const abilities::EffectDef* stunned = f.catalog->effect_named("Status_Stunned");
    const abilities::EffectDef* incap = f.catalog->effect_named("Status_Incapacitated");
    const abilities::EffectDef* slumber = f.catalog->effect_named("Mage_Slumber");
    const abilities::EffectDef* hunter_incap = f.catalog->effect_named("Summoner_Pet_Hunter_Incap");
    const abilities::EffectDef* buildup = f.catalog->effect_named("DisableEvasion_Hard_T2_Buildup");
    const abilities::HitDef* single_stun = f.catalog->hit_named("Bard_Song_Lullaby_Stun");
    const abilities::HitDef* aoe_stun = f.catalog->hit_named("Bard_Song_Lullaby_AOE_Stun");
    const abilities::HitDef* aoe_enemy = f.catalog->hit_named("Bard_Song_Lullaby_AOE_Enemy");
    CHECK(stunned && incap && slumber && hunter_incap && buildup && single_stun && aoe_stun && aoe_enemy && !wolf_path.empty(),
          "the data has the stun, the two sleeps, the Hunter's Incapacitate, the T2 build-up, the two stun hits and the area damage hit");
    if (!stunned || !incap || !slumber || !hunter_incap || !buildup || !single_stun || !aoe_stun || !aoe_enemy || wolf_path.empty()) return;
    CHECKF(single_stun->row.requirement_expr == 796 && aoe_stun->row.requirement_expr == 796, "both stun hits ask the target for the Incapacitated tag (requirement 796)");

    // ---- 1: the single Lullaby on an Incapacitated Wolf: four hits at the montage's notifies, no stun before the last, the 4 s stun on the last
    {
        BardMobWorld bw(f, wolf_path);
        CHECKF(bw.problem.empty(), "the Bard and the world: %s", bw.problem.c_str());
        if (bw.problem.empty()) {
            const uint64_t wolf = bw.w.spawn(wolf_path, 600, 0, 8, 0);
            effects::Applied asleep;
            asleep.def = incap;
            asleep.duration = 10.0;
            bw.world->apply_effects(wolf, 77, {asleep}, 100.0);
            const double t = 100.0;
            const double scale = lullaby_scale(bw);
            BardCast cast = bard_cast(bw, "Bard_Song_Lullaby", {wolf}, "Bard_Song_Lullaby_Enemy", t, t + kLullabyHits[2] * scale + 0.05);
            CHECKF(cast.cast, "the cast is accepted");
            if (cast.cast) {
                CHECKF(bw.world->effect_stacks(wolf, stunned->row.guid) == 0, "no stun on the first three hits (the last one has not come)");
                bard_run(bw, t + kLullabyHits[2] * scale + 0.05, t + kLullabyHits[3] * scale + 0.05, &cast.seen);
                lullaby_check_times(cast.seen, "Bard_Song_Lullaby", "Bard_Song_Lullaby_Enemy", t, scale);
                const std::vector<double> at = bard_hit_times(cast.seen, "Bard_Song_Lullaby_Enemy", "Bard_Song_Lullaby");
                if (at.size() == 4) {
                    const double left = bw.world->effect_remaining(wolf, stunned->row.guid, at[3]);
                    CHECKF(bw.world->effect_stacks(wolf, stunned->row.guid) == 1 && near(left, 4.0, 1e-6), "the last hit stuns the Incapacitated Wolf for 4 s (%.3f s left)", left);
                    const bootstrap::HitConfirm* stun_confirm = bard_confirmed(cast.seen, single_stun->row.guid);
                    CHECKF(stun_confirm && stun_confirm->source_type == content::kSourceTypeAbility && !stun_confirm->response && stun_confirm->targets.size() == 1,
                           "the server ran the stun itself (a confirmation with no client request) on the one target");
                    std::printf("    the single Lullaby at a play rate of %.4f: hits at %.4f, %.4f, %.4f and %.4f s; the last stuns for %.3f s\n", 1.0 / scale, at[0] - t, at[1] - t,
                                at[2] - t, at[3] - t, left);
                }
            }
        }
    }

    // ---- 2: a Wolf with no crowd control: four hits, no stun on any of them (the last hit's Stun asks the target and it is not Incapacitated)
    {
        BardMobWorld bw(f, wolf_path);
        CHECKF(bw.problem.empty(), "the Bard and the world: %s", bw.problem.c_str());
        if (bw.problem.empty()) {
            const uint64_t wolf = bw.w.spawn(wolf_path, 600, 0, 8, 0);
            const double t = 100.0;
            const double scale = lullaby_scale(bw);
            const BardCast cast = bard_cast(bw, "Bard_Song_Lullaby", {wolf}, "Bard_Song_Lullaby_Enemy", t, t + kLullabyHits[3] * scale + 0.05);
            CHECKF(cast.cast, "the cast is accepted");
            lullaby_check_times(cast.seen, "Bard_Song_Lullaby", "Bard_Song_Lullaby_Enemy", t, scale);
            CHECKF(bw.world->effect_stacks(wolf, stunned->row.guid) == 0, "a Wolf with no crowd control takes no stun, before the last hit or on it");
            std::printf("    the single Lullaby on a Wolf with no crowd control: four hits, no stun\n");
        }
    }

    // ---- 2b: a Wolf asleep from a sleep that carries no Incapacitated tag: the last hit gives no Lullaby stun either
    {
        BardMobWorld bw(f, wolf_path);
        CHECKF(bw.problem.empty(), "the Bard and the world: %s", bw.problem.c_str());
        if (bw.problem.empty()) {
            const uint64_t wolf = bw.w.spawn(wolf_path, 600, 0, 8, 0);
            effects::Applied sleep;
            sleep.def = slumber;
            sleep.duration = 10.0;
            bw.world->apply_effects(wolf, 77, {sleep}, 100.0);
            CHECKF(bw.world->effect_stacks(wolf, slumber->row.guid) == 1 && bard_cc(bw, wolf).flags == 7, "the Wolf is asleep (Mage_Slumber, a stun) and carries no Incapacitated tag");
            const double t = 100.0;
            const double scale = lullaby_scale(bw);
            const BardCast cast = bard_cast(bw, "Bard_Song_Lullaby", {wolf}, "Bard_Song_Lullaby_Enemy", t, t + kLullabyHits[3] * scale + 0.05);
            CHECKF(cast.cast, "the cast is accepted");
            lullaby_check_times(cast.seen, "Bard_Song_Lullaby", "Bard_Song_Lullaby_Enemy", t, scale);
            CHECKF(bw.world->effect_stacks(wolf, stunned->row.guid) == 0, "the asleep Wolf takes no Lullaby stun (only the Incapacitated tag counts)");
        }
    }

    // ---- 2c: the tag from another source (the Hunter's pet's Incapacitate): the last hit's stun lands
    {
        BardMobWorld bw(f, wolf_path);
        CHECKF(bw.problem.empty(), "the Bard and the world: %s", bw.problem.c_str());
        if (bw.problem.empty()) {
            const uint64_t wolf = bw.w.spawn(wolf_path, 600, 0, 8, 0);
            effects::Applied hunter;
            hunter.def = hunter_incap;
            hunter.duration = 10.0;
            bw.world->apply_effects(wolf, 77, {hunter}, 100.0);
            const double t = 100.0;
            const double scale = lullaby_scale(bw);
            const BardCast cast = bard_cast(bw, "Bard_Song_Lullaby", {wolf}, "Bard_Song_Lullaby_Enemy", t, t + kLullabyHits[3] * scale + 0.05);
            CHECKF(cast.cast, "the cast is accepted");
            lullaby_check_times(cast.seen, "Bard_Song_Lullaby", "Bard_Song_Lullaby_Enemy", t, scale);
            const std::vector<double> at = bard_hit_times(cast.seen, "Bard_Song_Lullaby_Enemy", "Bard_Song_Lullaby");
            if (at.size() == 4) {
                CHECKF(bw.world->effect_stacks(wolf, stunned->row.guid) == 1 && near(bw.world->effect_remaining(wolf, stunned->row.guid, at[3]), 4.0, 1e-6),
                       "a Wolf Incapacitated by another source (the Hunter's pet) takes the 4 s stun on the last hit");
            }
        }
    }

    // ---- 3: the area Lullaby on two Incapacitated Wolves: each takes the four hits and its own 6 s stun on the last
    {
        BardMobWorld bw(f, wolf_path);
        CHECKF(bw.problem.empty(), "the Bard and the world: %s", bw.problem.c_str());
        if (bw.problem.empty()) {
            const uint64_t a = bw.w.spawn(wolf_path, 600, 0, 8, 0);
            const uint64_t b = bw.w.spawn(wolf_path, 600, 120, 8, 0);
            effects::Applied asleep;
            asleep.def = incap;
            asleep.duration = 10.0;
            bw.world->apply_effects(a, 77, {asleep}, 100.0);
            bw.world->apply_effects(b, 77, {asleep}, 100.0);
            const double t = 100.0;
            const double scale = lullaby_scale(bw);
            const BardCast cast = bard_cast(bw, "Bard_Song_Lullaby_AOE", {a, b}, "Bard_Song_Lullaby_AOE_Enemy", t, t + kLullabyHits[3] * scale + 0.05);
            CHECKF(cast.cast, "the area cast is accepted");
            lullaby_check_times(cast.seen, "Bard_Song_Lullaby_AOE", "Bard_Song_Lullaby_AOE_Enemy", t, scale);
            const std::vector<double> at = bard_hit_times(cast.seen, "Bard_Song_Lullaby_AOE_Enemy", "Bard_Song_Lullaby_AOE");
            if (at.size() == 4) {
                const double la = bw.world->effect_remaining(a, stunned->row.guid, at[3]);
                const double lb = bw.world->effect_remaining(b, stunned->row.guid, at[3]);
                CHECKF(bw.world->effect_stacks(a, stunned->row.guid) == 1 && near(la, 6.0, 1e-6), "the first Wolf takes the 6 s area stun on its own last hit (%.3f s left)", la);
                CHECKF(bw.world->effect_stacks(b, stunned->row.guid) == 1 && near(lb, 6.0, 1e-6), "the second Wolf takes it too (%.3f s left)", lb);
                const bootstrap::HitConfirm* stun_confirm = bard_confirmed(cast.seen, aoe_stun->row.guid);
                CHECKF(stun_confirm && !stun_confirm->response && stun_confirm->targets.size() == 2, "the area stun was confirmed on both Wolves (%zu)",
                       stun_confirm ? stun_confirm->targets.size() : 0);
                std::printf("    the area Lullaby on two Incapacitated Wolves: %.3f s of the stun on each after the last hit\n", la);
            }
        }
    }

    // ---- 4: the area Lullaby on an Incapacitated and a plain Wolf: the last hit stuns only the Incapacitated one, and both take the damage
    {
        BardMobWorld bw(f, wolf_path);
        CHECKF(bw.problem.empty(), "the Bard and the world: %s", bw.problem.c_str());
        if (bw.problem.empty()) {
            const uint64_t inc = bw.w.spawn(wolf_path, 600, 0, 8, 0);
            const uint64_t plain = bw.w.spawn(wolf_path, 600, 120, 8, 0);
            effects::Applied asleep;
            asleep.def = incap;
            asleep.duration = 10.0;
            bw.world->apply_effects(inc, 77, {asleep}, 100.0);
            const double t = 100.0;
            const double scale = lullaby_scale(bw);
            const BardCast cast = bard_cast(bw, "Bard_Song_Lullaby_AOE", {inc, plain}, "Bard_Song_Lullaby_AOE_Enemy", t, t + kLullabyHits[3] * scale + 0.05);
            CHECKF(cast.cast, "the area cast is accepted");
            lullaby_check_times(cast.seen, "Bard_Song_Lullaby_AOE", "Bard_Song_Lullaby_AOE_Enemy", t, scale);
            const std::vector<double> at = bard_hit_times(cast.seen, "Bard_Song_Lullaby_AOE_Enemy", "Bard_Song_Lullaby_AOE");
            if (at.size() == 4) {
                const double left = bw.world->effect_remaining(inc, stunned->row.guid, at[3]);
                CHECKF(bw.world->effect_stacks(inc, stunned->row.guid) == 1 && near(left, 6.0, 1e-6), "the Incapacitated Wolf takes the 6 s area stun (%.3f s left)", left);
                CHECKF(bw.world->effect_stacks(plain, stunned->row.guid) == 0, "the Wolf with no crowd control takes no stun, though the last hit reaches it");
                const bootstrap::HitConfirm* dmg = bard_confirmed(cast.seen, aoe_enemy->row.guid);
                CHECKF(dmg && dmg->targets.size() == 2, "the damage hit reached both Wolves (%zu)", dmg ? dmg->targets.size() : 0);
                std::printf("    the area Lullaby on an Incapacitated and a plain Wolf: %.3f s of the stun on the first, none on the second; the damage reached both\n", left);
            }
        }
    }

    // ---- 5: a cast interrupted after its third hit: three hits, no last hit, no stun, and the cost and the cooldown stay spent
    {
        BardMobWorld bw(f, wolf_path);
        CHECKF(bw.problem.empty(), "the Bard and the world: %s", bw.problem.c_str());
        if (bw.problem.empty()) {
            const uint64_t wolf = bw.w.spawn(wolf_path, 600, 0, 8, 0);
            effects::Applied asleep;
            asleep.def = incap;
            asleep.duration = 10.0;
            bw.world->apply_effects(wolf, 77, {asleep}, 100.0);
            const int64_t lullaby = bw.rig.ability("Bard_Song_Lullaby");
            const double mana_before = bw.rig.mana();
            const double t = 100.0;
            const double scale = lullaby_scale(bw);
            const double stop = t + kLullabyHits[2] * scale + 2.0 / 15.0;
            BardCast cast = bard_cast(bw, "Bard_Song_Lullaby", {wolf}, "Bard_Song_Lullaby_Enemy", t, stop);
            CHECKF(cast.cast, "the cast is accepted");
            bw.rig.run_to(stop);   // (the helper's last world tick stops just before the regeneration at 102 s: read the Mana at the cancel itself)
            bw.rig.take();
            const double mana_paid = bw.rig.mana();
            CHECKF(mana_paid < mana_before - 1.0, "its cost step has paid Mana before the interrupt (%.2f of %.2f)", mana_paid, mana_before);
            CHECKF(bw.rig.pipe->charges_left(lullaby) == 0, "its charge is spent before the interrupt (%d left)", bw.rig.pipe->charges_left(lullaby));
            const std::vector<double> before = bard_hit_times(cast.seen, "Bard_Song_Lullaby_Enemy", "Bard_Song_Lullaby");
            CHECKF(before.size() == 3, "the first three hits ran (%zu)", before.size());
            CHECKF(bw.rig.pipe->end_request(cast.server_instance, stop), "the cast is interrupted after its third hit");
            const std::vector<Seen> cancel = bw.rig.take();
            const double mana_after = bw.rig.mana();
            bool cooldown_removed = false, mana_given_back = false;
            for (const Seen& s : cancel) {
                cooldown_removed = cooldown_removed || !s.change.cooldowns_removed.empty();
                for (const bootstrap::StatChange& st : s.change.stats) mana_given_back = mana_given_back || (st.record == content::kGuidMana && st.value > mana_paid + 1.0f);
            }
            const bool mana_stays = near(mana_after, mana_paid, 0.01) && mana_after < mana_before - 1.0;
            if (mana_stays) std::printf("  [ok ] the Mana stays spent after the interrupt (%.2f before, %.2f after the cost step, %.2f after the end)\n", mana_before, mana_paid, mana_after);
            CHECKF(mana_stays, "the Mana stays spent after the interrupt (%.2f before, %.2f after the cost step, %.2f after the end)", mana_before, mana_paid, mana_after);
            CHECKF(!cooldown_removed && !mana_given_back, "the end gives back neither a cooldown nor the Mana");
            CHECKF(bw.rig.pipe->charges_left(lullaby) == 0, "the group's charge is still spent (%d left)", bw.rig.pipe->charges_left(lullaby));
            const bootstrap::CooldownItem* cd = bw.rig.pawn.cooldown(lullaby);
            CHECKF(cd && near(cd->initial_cooldown, 25.0, 1e-3) && cd->charges == 0, "the Lullaby's 25 s cooldown is still on the pawn (%.1f s, %d charges)", cd ? cd->initial_cooldown : -1.0,
                   cd ? cd->charges : -1);
            bard_run(bw, stop, t + kLullabyHits[3] * scale + 0.2, &cast.seen);
            const std::vector<double> after = bard_hit_times(cast.seen, "Bard_Song_Lullaby_Enemy", "Bard_Song_Lullaby");
            CHECKF(after.size() == 3, "no fourth hit after the interrupt (%zu)", after.size());
            CHECKF(bw.world->effect_stacks(wolf, stunned->row.guid) == 0, "an interrupted cast gives no stun");
            TriggerInfo again_info;
            again_info.target_count = 1;
            again_info.target_actors = {wolf};
            const Refusal again = bw.rig.pipe->trigger(lullaby, bw.rig.next_client_id++, t + 10.0, again_info);
            bw.rig.take();
            CHECKF(again == Refusal::Charges, "a second Lullaby during the 25 s cooldown is refused (%d)", static_cast<int>(again));
        }
    }

    // ---- 6: the 6 s area stun on the last hit ends when the T2 build-up reaches ten stacks (0.5 s a tick, one stack a tick, at 4.5 s)
    {
        BardMobWorld bw(f, wolf_path);
        CHECKF(bw.problem.empty(), "the Bard and the world: %s", bw.problem.c_str());
        if (bw.problem.empty()) {
            const uint64_t wolf = bw.w.spawn(wolf_path, 600, 0, 8, 0);
            effects::Applied asleep;
            asleep.def = incap;
            asleep.duration = 10.0;
            bw.world->apply_effects(wolf, 77, {asleep}, 100.0);
            const double t = 100.0;
            const double scale = lullaby_scale(bw);
            const BardCast cast = bard_cast(bw, "Bard_Song_Lullaby_AOE", {wolf}, "Bard_Song_Lullaby_AOE_Enemy", t, t + kLullabyHits[3] * scale + 0.05);
            CHECKF(cast.cast, "the area cast is accepted");
            const std::vector<double> at = bard_hit_times(cast.seen, "Bard_Song_Lullaby_AOE_Enemy", "Bard_Song_Lullaby_AOE");
            CHECKF(at.size() == 4 && bw.world->effect_stacks(wolf, stunned->row.guid) == 1, "the area stun lands on the Incapacitated Wolf on the last hit");
            if (at.size() == 4) {
                const double hit4 = at[3];
                bard_run(bw, hit4 + 0.05, hit4 + 4.4);
                const int stacks_before = bw.world->effect_stacks(wolf, buildup->row.guid);
                const double stun_left = bw.world->effect_remaining(wolf, stunned->row.guid, hit4 + 4.4);
                CHECKF(stacks_before == 9 && stun_left > 1.5, "at 4.4 s the T2 build-up has 9 stacks (one a tick, 0.5 s a tick) and the stun is still on (%.1f s left)", stun_left);
                double off_at = -1.0;
                for (double now = hit4 + 4.4; now <= hit4 + 6.5; now += 1.0 / 15.0) {
                    bard_run(bw, now, now);
                    if (bw.world->effect_stacks(wolf, stunned->row.guid) == 0) {
                        off_at = now;
                        break;
                    }
                }
                CHECKF(off_at > 0.0 && near(off_at - hit4, 4.5, 1.0 / 15.0 + 1e-6), "the stun ends at %.3f s (the tenth half-second tick), not at its nominal 6 s", off_at - hit4);
                std::printf("    at 4.4 s after the last hit: %d build-up stacks and %.1f s of the stun left; it ends at %.3f s\n", stacks_before, stun_left, off_at - hit4);
            }
        }
    }

    // ---- 7: an Incapacitated boss takes the last hit's 4 s stun too
    {
        const std::string boss_path = creature_path(f, "/GiantCyclops/CyclopsShaman.Default__CyclopsShaman_C");
        CHECK(!boss_path.empty(), "the Blueprint table has a boss (the Cyclops Shaman)");
        if (!boss_path.empty()) {
            BardMobWorld bw(f, boss_path);
            CHECKF(bw.problem.empty(), "the Bard and the boss's world: %s", bw.problem.c_str());
            if (bw.problem.empty()) {
                const uint64_t boss = bw.w.spawn(boss_path, 900, 0, 45, 0);
                effects::Applied asleep;
                asleep.def = incap;
                asleep.duration = 10.0;
                bw.world->apply_effects(boss, 77, {asleep}, 100.0);
                const double t = 100.0;
                const double scale = lullaby_scale(bw);
                const BardCast cast = bard_cast(bw, "Bard_Song_Lullaby", {boss}, "Bard_Song_Lullaby_Enemy", t, t + kLullabyHits[3] * scale + 0.05);
                const std::vector<double> at = bard_hit_times(cast.seen, "Bard_Song_Lullaby_Enemy", "Bard_Song_Lullaby");
                CHECKF(cast.cast && at.size() == 4 && bw.world->effect_stacks(boss, stunned->row.guid) == 1, "the Incapacitated boss takes the last hit's 4 s stun");
            }
        }
    }
}
AOC_REGISTER_TEST(test_bard_lullaby_cast);

void test_bard_lullaby_cut_before_the_cost(Fixture& f) {
    std::printf("a Dark Lullaby the client ends before its cost step: nothing is paid, nothing more fires, no cooldown starts\n");
    const std::string wolf_path = wolf_archetype(f);
    const abilities::EffectDef* stunned = f.catalog->effect_named("Status_Stunned");
    const abilities::EffectDef* incap = f.catalog->effect_named("Status_Incapacitated");
    CHECK(stunned && incap && !wolf_path.empty(), "the data has the stun, the Incapacitated tag and the Wolf");
    if (!stunned || !incap || wolf_path.empty()) return;
    BardMobWorld bw(f, wolf_path);
    CHECKF(bw.problem.empty(), "the Bard and the world: %s", bw.problem.c_str());
    if (!bw.problem.empty()) return;
    const uint64_t wolf = bw.w.spawn(wolf_path, 600, 0, 8, 0);
    effects::Applied asleep;
    asleep.def = incap;
    asleep.duration = 20.0;
    bw.world->apply_effects(wolf, 77, {asleep}, 100.0);
    const int64_t lullaby = bw.rig.ability("Bard_Song_Lullaby");
    const double mana_before = bw.rig.mana();
    const double t = 100.0;
    const double scale = lullaby_scale(bw);
    const double stop = t + kLullabyHits[0] * scale + 2.0 / 15.0;   // (past the first hit, before the 0.300 s cost step)
    BardCast cast = bard_cast(bw, "Bard_Song_Lullaby", {wolf}, "Bard_Song_Lullaby_Enemy", t, stop);
    CHECKF(cast.cast, "the cast is accepted");
    CHECKF(near(bw.rig.mana(), mana_before, 0.01), "no Mana has been paid before the cost step (%.2f of %.2f)", bw.rig.mana(), mana_before);
    CHECKF(bw.rig.pipe->charges_left(lullaby) == -1 && bw.rig.pawn.cooldown(lullaby) == nullptr, "no cooldown has started before the cost step");
    // (its item is sent at the same 0.300 s as the cost, so an end before it names no server instance: the cast's own is -1)
    bw.rig.pipe->end_request(-1, stop + 1.0 / 15.0);
    bw.rig.take();
    CHECKF(near(bw.rig.mana(), mana_before, 0.01), "the end before the cost step takes no Mana (%.2f of %.2f)", bw.rig.mana(), mana_before);
    CHECKF(bw.rig.pipe->charges_left(lullaby) == -1 && bw.rig.pawn.cooldown(lullaby) == nullptr, "and starts no cooldown");
    bard_run(bw, stop + 1.0 / 15.0, t + kLullabyHits[3] * scale + 0.2, &cast.seen);
    const std::vector<double> at = bard_hit_times(cast.seen, "Bard_Song_Lullaby_Enemy", "Bard_Song_Lullaby");
    const bool only_first = at.size() == 1;
    if (only_first) std::printf("  [ok ] only the first hit ran before the end; no later hit fires (%zu)\n", at.size());
    CHECKF(only_first, "only the first hit ran before the end; no later hit fires (%zu)", at.size());
    CHECKF(bw.world->effect_stacks(wolf, stunned->row.guid) == 0, "the last hit's stun does not run");
    CHECKF(bw.rig.pawn.broken.empty(), "(the pawn took every change)");
}
AOC_REGISTER_TEST(test_bard_lullaby_cut_before_the_cost);

void test_bard_target_hit_requirement(Fixture& f) {
    std::printf("a non-Lullaby hit whose requirement reads the target is still asked of the caster, as before (the Snipe's Hemorrhage)\n");
    const std::string wolf_path = wolf_archetype(f);
    const abilities::EffectDef* bleeding = f.catalog->effect_named("Status_Bleeding");
    const abilities::EffectDef* hemorrhage = f.catalog->effect_named("Status_Hemorrhage");
    const abilities::HitDef* hemo_hit = f.catalog->hit_named("Snipe_Hemorrhage");
    CHECK(bleeding && hemorrhage && hemo_hit && !wolf_path.empty(), "the data has the bleed, the Hemorrhage and the Snipe's Hemorrhage hit");
    if (!bleeding || !hemorrhage || !hemo_hit || wolf_path.empty()) return;
    const content::ExprRow* req = f.catalog->expr(hemo_hit->row.requirement_expr);
    CHECKF(req && req->uses_target, "the Hemorrhage's requirement reads its target (a bleeding one)");
    BardMobWorld bw(f, wolf_path, 1);   // (a Ranger: the Snipe is his)
    CHECKF(bw.problem.empty(), "the Ranger and the world: %s", bw.problem.c_str());
    if (!bw.problem.empty()) return;
    bw.rig.know({"Ranger_Snipe"});
    const uint64_t wolf = bw.w.spawn(wolf_path, 600, 0, 8, 0);
    effects::Applied bleed;
    bleed.def = bleeding;
    bleed.duration = 20.0;
    bw.world->apply_effects(wolf, 77, {bleed}, 100.0);
    CHECKF(bw.world->effect_stacks(wolf, bleeding->row.guid) == 1, "the Wolf is bleeding (the Hemorrhage's requirement holds of it)");
    const BardCast cast = bard_cast(bw, "Ranger_Snipe", {wolf}, "Snipe_Hemorrhage", 100.0, 100.5);
    CHECKF(cast.cast, "the Snipe's cast is accepted");
    CHECKF(bw.world->effect_stacks(wolf, hemorrhage->row.guid) == 0, "the Hemorrhage does not land: its requirement is asked of the caster, as before");
}
AOC_REGISTER_TEST(test_bard_target_hit_requirement);
