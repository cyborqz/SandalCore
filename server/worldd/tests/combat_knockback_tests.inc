// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_knockback_tests.inc: knock-backs, launches, pulls and the crowd control that comes with them, on creatures (included by combat_registered.inc inside test_worldd_combat's
//  anonymous namespace)
//
//  A creature a hit throws (the Bard's Get Off The Stage), a push that an effect plays on it (the Mage's Cone of Cold with its talent, the Tank's Ground Pound) and a pull (the Tank's Grapple) are worked
//  out on the combat rig with a Wolf and a player.  A hit lands as the ability runtime lands it (its numbers resolved for the two, its record named as its origin), and what a client is shown of the Wolf is
//  read from its replicated state tick by tick: where it is, the velocity and the movement mode it is sent, and the crowd control state that comes with the effect.  The throw is the data's: one velocity
//  change of 1000 cm/s at 35 degrees up along the Bard's facing, the same at every distance and angle, added to what the creature already has; the tuned throw (which depended on the distance and the angle)
//  stays in the code behind its setting and is checked on its own.  The push is the formula's own, worked out here the slow way for the friction the Wolf has.
// ============================================================================

constexpr double kKbPi = 3.14159265358979323846;

/// What slows a creature's body on the ground and what a force is divided by: its Blueprint's own where the table has them (the Wolf's ground friction is 24), else the engine's defaults.
struct KbBody {
    double friction = 8.0, braking = 2048.0, mass = 100.0;
};

static KbBody kb_body(Fixture& f, const std::string& path) {
    KbBody b;
    if (const auto bp = f.motion.find(path)) {
        if (bp->ground_friction >= 0) b.friction = bp->ground_friction;
        if (bp->braking_deceleration >= 0) b.braking = bp->braking_deceleration;
        if (bp->mass > 0) b.mass = bp->mass;
    }
    return b;
}

/// What the replicated state of the creature says at the end of a tick.
struct KbTick {
    double  t = 0, x = 0, y = 0, z = 0, yaw = 0, vx = 0, vy = 0, vz = 0;
    int64_t mode = -1;    // ReplicatedMovementMode (-1: it has none)
    int64_t flags = 0;    // CrowdControlState.StatusFlags as the state holds them
};

static KbTick kb_tick(const world::WorldActor& a, double t) {
    KbTick k;
    k.t = t;
    k.x = a.x;
    k.y = a.y;
    k.z = a.z;
    k.yaw = a.yaw_degrees;
    const aoc::protocol::repl::ObjectState& obj = a.actor.obj;
    if (obj.schema) {
        if (const auto* v = obj.state.get(*obj.schema, "ReplicatedMovement")) {
            if (const auto* m = v->get_if<aoc::protocol::repl::RepMovement>()) {
                const aoc::protocol::repl::Vec3d vel = aoc::protocol::repl::dequantize_velocity(obj.opts, m->linear_velocity);
                k.vx = vel.x;
                k.vy = vel.y;
                k.vz = vel.z;
            }
        }
        if (const auto* v = obj.state.get(*obj.schema, "ReplicatedMovementMode")) {
            if (const uint64_t* p = v->get_if<uint64_t>()) k.mode = static_cast<int64_t>(*p);
        }
    }
    k.flags = cc_now(a).flags;
    return k;
}

/// The push fields of the creature's crowd control state, read from its stats component (not from an update).
struct KbPushNow {
    int64_t flags = 0, record = 0;
    int64_t instigator[4] = {0, 0, 0, 0}, source[4] = {0, 0, 0, 0};
    double  start = 0;
    bool    has_origin = false;
    double  ox = 0, oy = 0, oz = 0;
};

static KbPushNow kb_push_now(const world::WorldActor& a) {
    KbPushNow out;
    for (const aoc::protocol::repl::Subobject& sub : a.actor.subobjects) {
        if (!sub.obj.schema || std::string(sub.obj.schema->name) != "UAoCStatsComponent") continue;
        const aoc::protocol::schema::ClassSchema& sc = *sub.obj.schema;
        const auto get = [&](const std::string& path) { return sub.obj.state.get(sc, path.c_str()); };
        if (const auto* v = get("CrowdControlState.StatusFlags"); v && v->get_if<uint64_t>()) out.flags = static_cast<int64_t>(*v->get_if<uint64_t>());
        if (const auto* v = get("CrowdControlState.CrowdControlRecordToDisplace"); v && v->get_if<int64_t>()) out.record = *v->get_if<int64_t>();
        static const char* const kWord[4] = {"A", "B", "C", "D"};
        for (int i = 0; i < 4; ++i) {
            if (const auto* v = get(std::string("CrowdControlState.PushInstigatorGuid.") + kWord[i]); v && v->get_if<int64_t>()) out.instigator[i] = *v->get_if<int64_t>();
            if (const auto* v = get(std::string("CrowdControlState.PushSourceGuid.") + kWord[i]); v && v->get_if<int64_t>()) out.source[i] = *v->get_if<int64_t>();
        }
        if (const auto* v = get("CrowdControlState.SyncStartPushTime"); v && v->get_if<double>()) out.start = *v->get_if<double>();
        if (const auto* v = get("CrowdControlState.InitialOriginTransform.Transform.Translation"); v && v->get_if<aoc::protocol::repl::Vec3d>()) {
            const aoc::protocol::repl::Vec3d* p = v->get_if<aoc::protocol::repl::Vec3d>();
            out.has_origin = p->x != 0.0 || p->y != 0.0 || p->z != 0.0;
            out.ox = p->x;
            out.oy = p->y;
            out.oz = p->z;
        }
    }
    return out;
}

/// How far a body that moves at `v` cm/s on the ground goes before friction `k` (1/s) and braking `b` (cm/s^2) stop it.
static double kb_coast(double v, double k, double b) {
    const double sign = v < 0 ? -1.0 : 1.0;
    double speed = std::fabs(v), x = 0;
    constexpr double h = 0.0005;
    while (speed > 1e-9) {
        const double next = std::max(0.0, speed - (k * speed + b) * h);
        x += 0.5 * (speed + next) * h;
        speed = next;
    }
    return sign * x;
}

/// The Blueprint table with some cells of one row set: (column, text) pairs, 12 the tags column and 21 the ground friction.  False when the row or a column is not there.
static bool kb_table_cells(const std::string& path, const std::vector<std::pair<size_t, std::string>>& set, npc::MotionTable& out) {
    std::ifstream in(NPC_MOTION_PATH, std::ios::binary);
    if (!in) return false;
    std::ostringstream all;
    all << in.rdbuf();
    std::istringstream lines(all.str());
    std::string line, result;
    bool first = true, changed = false;
    while (std::getline(lines, line)) {
        std::string tail;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
            tail = "\r";
        }
        if (!first && line.compare(0, path.size() + 1, path + "\t") == 0) {
            std::vector<std::string> cells;
            size_t from = 0;
            for (;;) {
                const size_t tab = line.find('\t', from);
                cells.push_back(line.substr(from, tab == std::string::npos ? std::string::npos : tab - from));
                if (tab == std::string::npos) break;
                from = tab + 1;
            }
            bool all_there = true;
            for (const auto& [column, text] : set) {
                if (column < cells.size()) cells[column] = text;
                else all_there = false;
            }
            if (all_there) {
                line.clear();
                for (size_t i = 0; i < cells.size(); ++i) line += (i ? "\t" : "") + cells[i];
                changed = true;
            }
        }
        first = false;
        result += line + tail + "\n";
    }
    std::string err;
    return changed && out.parse(result, err);
}

/// The Blueprint table with the Boss tag on this Blueprint's row.
static bool kb_boss_table(const std::string& path, npc::MotionTable& out) { return kb_table_cells(path, {{12, "Boss"}}, out); }

/// The modelled Get Off The Stage worked out the slow way (the model of combat/knockback.h written out again): the lift and the speed at that distance (m) and angle off the facing (degrees), a flight
/// under gravity 980 with no friction in the air, then the slide a body's friction `k` and braking `b` give.  `closing` is the speed (cm/s) at which the body runs at the Bard when the hit lands: the throw
/// is added to it.  `rest` is how far from where it stood the body comes to rest, `touchdown` where it comes down (both along the throw, cm), `apex` the height of its top (cm).
struct KbThrow {
    double rest = 0, touchdown = 0, apex = 0, air_seconds = 0;
};

static KbThrow kb_throw_model(double metres, double phi_degrees, double k, double b, double closing = 0.0) {
    const double lift = metres <= 0.5 ? 60.0 : metres <= 2.0 ? 60.0 - 25.0 * (metres - 0.5) / 1.5 : metres <= 5.0 ? 35.0 - 15.0 * (metres - 2.0) / 3.0 : 20.0;
    const double reach = metres <= 2.0 ? 1.0 : metres <= 5.0 ? 1.0 - 0.5 * (metres - 2.0) / 3.0 : 0.5;
    const double phi = std::min(std::max(phi_degrees, 0.0), 45.0);
    const double speed = 1000.0 * reach * (1.0 - 0.5 * phi / 45.0);
    const double vh = speed * std::cos(lift * kKbPi / 180.0) - closing, vz = speed * std::sin(lift * kKbPi / 180.0);
    KbThrow t;
    t.air_seconds = 2.0 * vz / 980.0;
    t.apex = vz * vz / (2.0 * 980.0);
    t.touchdown = vh * t.air_seconds;
    t.rest = t.touchdown + kb_coast(vh, k, b);
    return t;
}

/// The Cone of Cold's push worked out the slow way: a force of (1,500,000 - d^2) over the mass along the Mage's facing for `seconds`, d the distance from the Mage to the body's centre (it grows as the body is pushed;
/// `dz` is the height between them), friction `k` and braking `b`, then the coast.  `gap` is the distance along the facing when the push begins.
static double kb_cone_reach(double gap, double dz, double mass, double k, double b, double seconds) {
    double v = 0, x = 0;
    constexpr double h = 0.0005;
    for (double t = 0; t < seconds; t += h) {
        const double d2 = (gap + x) * (gap + x) + dz * dz;
        const double a = std::max(0.0, 1500000.0 - d2) / mass;
        const double next = std::max(0.0, v + (a - k * v - b) * h);
        x += 0.5 * (v + next) * h;
        v = next;
    }
    return x + kb_coast(v, k, b);
}

/// A hit as the ability runtime lands it on the creature: its numbers worked out for the player and the creature (rolled again while it misses; its damage left out unless asked for, the Wolf is the
/// subject of the checks and not its Health), delivered with the hit's record as its origin, then what the hit takes off the creature and the effects it puts on.  False when the hit does not run.
static bool kb_land(LungeStepContext& c, const abilities::HitDef& hit, bool damage = false) {
    combat::Fighter* target = c.world.fighter_of(c.creature);
    if (!target) return false;
    combat::HitMeta meta;
    meta.source_type = content::kSourceTypeAbility;
    combat::Resolved r;
    std::string why;
    bool resolved = false;
    for (int tries = 0; tries < 80 && !resolved; ++tries) resolved = c.world.resolver().resolve(c.player.fighter(), *target, hit, meta, r, why) && !r.miss();
    if (!resolved) return false;
    if (!damage) {
        for (combat::ResolvedMod& m : r.mods) m.value = 0.0;
    }
    combat::HitOrigin origin;
    origin.hit = &hit;
    origin.source_type = content::kSourceTypeAbility;
    if (!c.world.apply_hit_to_npc(c.creature, c.player.actor_id(), r, c.now, 0, &origin).applied) return false;
    combat::Resolved gate;
    if (!c.world.resolver().resolve_effects(c.player.fighter(), *target, hit, meta, gate, why)) return false;
    if (!hit.remove_effects.empty() || (hit.max_remove > 0 && hit.removal != abilities::HitDef::Removal::Unknown)) c.world.remove_effects_from_npc(c.creature, hit, c.now);
    c.world.apply_effects(c.creature, c.player.actor_id(), gate.effects, c.now);
    return true;
}

/// What a fight left: the creature's state tick by tick, and the samples of the fight itself.
struct KbRun {
    std::vector<KbTick>      ticks;
    std::vector<LungeSample> s;
    bool                     ran = false;
};

/// A fight of a creature that starts none, against a player at (px, py), with `act` called at every tick before the worlds move.
static bool kb_fight(Fixture& f, const combat::NpcAbilityData& tables, const std::string& path, double seconds, double px, double py, double creature_x, double creature_y,
                     const std::function<void(LungeStepContext&)>& act, KbRun& out) {
    out.ran = run_lunge_fight(f, tables, path, seconds, px, py, nullptr, out.s, nullptr, true, -1, nullptr, 40000.0, nullptr, creature_x, creature_y, [&](LungeStepContext& c) {
        if (!c.after) {
            act(c);
            return;
        }
        if (const world::WorldActor* a = c.w.registry.find(c.creature)) out.ticks.push_back(kb_tick(*a, c.now));
    });
    return out.ran;
}

/// The index of the first tick at or after `t`.
static size_t kb_index(const std::vector<KbTick>& k, double t) {
    for (size_t i = 0; i < k.size(); ++i) {
        if (k[i].t >= t - 1e-9) return i;
    }
    return k.size();
}

// ---- the Bard's Get Off The Stage -----------------------------------------------------------------------------------------------------------------------------------
// The data's throw is one velocity change: 1000 cm/s at 35 degrees up along the Bard's facing, the same at every distance and every angle inside the hit's cone, added to the velocity the creature
// already has.  The tuned throw (up by 60 degrees at 0.5 m or less, 35 at 2 m and 20 at 5 m or more; at 1000 cm/s up to 2 m and 500 at 5 m; less by half of phi/45 for a creature phi degrees off the
// facing, and along the line from the Bard to the creature) is kept in combat/knockback.h behind its setting.  The numbers here are worked out with the body integrator the NPC world steps (on the walking
// map's flat test ground) and each creature's own ground friction and braking from the Blueprint table.

/// The data's throw worked out with the same body integrator the NPC world steps (npc/displacement.h) on the walking map's flat test ground: one velocity change of `speed` cm/s at `lift_degrees` up along
/// (`dir_x`, `dir_y`), added to the velocity the creature already has, then the slide its own ground friction and braking stop.  `rest_x`/`rest_y` are where it came to rest from where the hit took it (cm),
/// `apex` the highest its feet went above the floor at the 15 Hz ticks (cm), `air_seconds` the flight and `air_ticks` the updates its movement mode is falling.
struct KbDataThrow {
    double rest_x = 0, rest_y = 0, rest = 0, apex = 0, air_seconds = 0;
    size_t air_ticks = 0;
};

static KbDataThrow kb_data_throw(const KbBody& body, double speed, double lift_degrees, double dir_x, double dir_y, double own_vx, double own_vy) {
    constexpr double kStep = 1.0 / 15.0;
    const double a = lift_degrees * kKbPi / 180.0;
    npc::Displacement d;
    d.friction = body.friction;
    d.braking = body.braking;
    d.mass = body.mass;
    d.velocity = {own_vx, own_vy, 0.0};
    npc::add_velocity_change(d, nav::Vec3{dir_x * std::cos(a) * speed, dir_y * std::cos(a) * speed, std::sin(a) * speed}, kStep);
    npc::DisplacePose p;
    const npc::DisplaceWorld world{[](double, double, double, double* z) { *z = 0.0; return true; },
                                   [](const nav::Vec3&, const nav::Vec3&, nav::Vec3*) { return true; }};
    KbDataThrow out;
    out.air_seconds = 2.0 * d.velocity.z / npc::kGravityCm;
    for (int tick = 0; tick < 400; ++tick) {
        npc::DisplaceOutcome step;
        npc::displace_step(d, p, tick * kStep, kStep, world, step);
        out.apex = std::max(out.apex, p.feet_z);
        if (d.airborne) ++out.air_ticks;
        if (step.at_rest) break;
    }
    out.rest_x = p.x;
    out.rest_y = p.y;
    out.rest = std::hypot(p.x, p.y);
    return out;
}

/// What one throw did.  `end_x`/`end_y` are where it came to rest from where the hit took it (cm), `reach` the part along the line from the Bard (cm; negative: towards him), `apex` the highest it went
/// above where it stood, and `own_vx`/`own_vy` the velocity it already had when the hit landed.  `bard_x`/`bard_y` are where the Bard stood when he threw it.
struct KbFlight {
    bool   ran = false, hit_ok = false, flew = false, landed = false;
    double reach = 0, apex = 0, end_z_error = 1.0e9, first_speed = 0;
    double end_x = 0, end_y = 0, own_vx = 0, own_vy = 0, first_vx = 0, first_vy = 0, first_vz = 0, bard_x = 0, bard_y = 0;
    size_t air_ticks = 0;
    double start_x = 0, start_y = 0, start_z = 0;
};

/// The Bard stands `gap_cm` from the Wolf along +x (it is thrown towards -x), facing so that the Wolf is `phi_degrees` off his facing.  The Wolf runs the other way, at `closing_cm_s`, when that is above 0 (a
/// creature that charges at him).  `boss`: the Wolf's Blueprint carries the Boss tag.
static KbFlight kb_flight(Fixture& f, const combat::NpcAbilityData& tables, const std::string& wolf_path, double gap_cm, double phi_degrees, double closing_cm_s, bool boss = false) {
    KbFlight out;
    const abilities::HitDef* hit = f.catalog->hit_named("Bard_Misc_GetOffTheStage");
    const auto bp = f.motion.find(wolf_path);
    if (!hit || !bp) return out;
    npc::MotionTable boss_table;   // (the table with the Boss tag on this Blueprint's row)
    if (boss && !kb_boss_table(wolf_path, boss_table)) return out;
    if (boss) std::swap(f.motion, boss_table);
    const double order_at = 0.25, hit_at = closing_cm_s > 0 ? 1.0 : 0.4;
    bool ordered = false, landed_hit = false;
    KbRun run;
    kb_fight(f, tables, wolf_path, 4.0, 3000.0, 0.0, 0.0, 0.0, [&](LungeStepContext& c) {
        const world::WorldActor* wolf = c.w.registry.find(c.creature);
        if (!wolf) return;
        if (closing_cm_s > 0 && !ordered && c.now >= order_at) {
            ordered = true;
            const npc::Mover* m = c.w.npcs->mover(c.creature);
            c.w.npcs->order_move(c.creature, nav::Vec3{2500.0, 0.0, m ? m->ground().z : 1000.0}, closing_cm_s, 0.0);
        }
        if (landed_hit || c.now < hit_at - 1e-9) return;
        landed_hit = true;
        out.start_x = wolf->x;
        out.start_y = wolf->y;
        out.start_z = wolf->z;
        if (const npc::Mover* m = c.w.npcs->mover(c.creature)) {
            out.own_vx = m->velocity().x;
            out.own_vy = m->velocity().y;
        }
        c.player.rig_fighter().move_to({wolf->x + gap_cm, wolf->y, 1098.0});
        c.player.rig_fighter().face(180.0 + phi_degrees);
        out.bard_x = c.player.fighter().position().x;
        out.bard_y = c.player.fighter().position().y;
        out.hit_ok = kb_land(c, *hit);
    }, run);
    if (boss) std::swap(f.motion, boss_table);
    out.ran = run.ran;
    if (!run.ran || !out.hit_ok) return out;
    const size_t h = kb_index(run.ticks, hit_at);
    if (h >= run.ticks.size()) return out;
    out.first_speed = std::sqrt(run.ticks[h].vx * run.ticks[h].vx + run.ticks[h].vy * run.ticks[h].vy + run.ticks[h].vz * run.ticks[h].vz);
    out.first_vx = run.ticks[h].vx;
    out.first_vy = run.ticks[h].vy;
    out.first_vz = run.ticks[h].vz;
    size_t first_air = run.ticks.size(), landed_at = run.ticks.size();
    for (size_t i = h; i < run.ticks.size(); ++i) {
        out.apex = std::max(out.apex, run.ticks[i].z - out.start_z);
        if (run.ticks[i].mode == 3) {
            ++out.air_ticks;
            if (first_air == run.ticks.size()) first_air = i;
        } else if (first_air != run.ticks.size() && landed_at == run.ticks.size()) {
            landed_at = i;
        }
    }
    out.flew = first_air != run.ticks.size();
    out.landed = out.flew && landed_at != run.ticks.size();
    // where it came to rest: the first tick from the landing on at which it is nearly still (its walk is held back until it is)
    size_t rest = out.landed ? landed_at : run.ticks.size() - 1;
    if (out.landed) {
        while (rest + 1 < run.ticks.size() && rest < landed_at + 20 && std::hypot(run.ticks[rest].vx, run.ticks[rest].vy) > 15.0) ++rest;
    }
    out.end_x = run.ticks[rest].x - out.start_x;
    out.end_y = run.ticks[rest].y - out.start_y;
    out.reach = -out.end_x;
    double ground = 0;
    if (navfix::shared_nav().ground_height(run.ticks[rest].x, run.ticks[rest].y, out.start_z - bp->half_height, &ground)) out.end_z_error = run.ticks[rest].z - (ground + bp->half_height);
    return out;
}

void test_kb_get_off_the_stage(Fixture& f) {
    std::printf("Get Off The Stage gives a standing Wolf one velocity change of 1000 cm/s at 35 degrees up along the Bard's facing, the same at every distance and angle; each case is measured on its own against the NPC world's own body integrator and the creature's own friction\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string wolf_path = creature_path(f, "/Beast/Canine/Wolf.Default__Wolf_C");
    const abilities::HitDef* hit = f.catalog->hit_named("Bard_Misc_GetOffTheStage");
    CHECK(!wolf_path.empty() && f.motion.find(wolf_path) && tables.abilities_of(wolf_path) && hit, "the Blueprint table has the Wolf, the creature tables list its abilities, and the data has the Bard's throw");
    if (wolf_path.empty() || !f.motion.find(wolf_path) || !tables.abilities_of(wolf_path) || !hit) return;
    const KbBody body = kb_body(f, wolf_path);
    CHECKF(near(body.friction, 24.0, 1e-9), "the Wolf's ground friction is %.0f; must be 24, error allowed 0 (its Blueprint sets it)", body.friction);
    CHECKF(near(body.braking, 2048.0, 1e-9) && near(body.mass, 100.0, 1e-9), "...its braking is %.0f and its mass %.0f; must be 2048 and 100, error allowed 0 (the engine's defaults, its Blueprint sets neither)", body.braking, body.mass);

    // a standing Wolf at 0.5, 1, 2, 3, 4 and 5 m facing the Bard: the throw takes it along the Bard's facing, the same at every distance
    const double facing0 = 180.0 * kKbPi / 180.0;
    for (const double gap : {50.0, 100.0, 200.0, 300.0, 400.0, 500.0}) {
        const KbFlight fl = kb_flight(f, tables, wolf_path, gap, 0.0, 0.0);
        const KbDataThrow m = kb_data_throw(body, 1000.0, 35.0, std::cos(facing0), std::sin(facing0), fl.own_vx, fl.own_vy);
        const double rest_error = 0.015 * m.rest + 2.0;
        const double stood = std::hypot(fl.start_x - fl.bard_x, fl.start_y - fl.bard_y);
        const double from_bard = std::hypot(fl.start_x + fl.end_x - fl.bard_x, fl.start_y + fl.end_y - fl.bard_y);
        std::printf("    standing at %.0f cm: %s, rests (%.1f, %.1f) cm from where it was hit (the integrator (%.1f, %.1f)), %.1f cm from the Bard (%.1f), highest %.2f cm, %zu updates in the air\n", gap,
                    fl.hit_ok ? "thrown" : "no hit", fl.end_x, fl.end_y, m.rest_x, m.rest_y, from_bard, stood + m.rest, fl.apex, fl.air_ticks);
        CHECKF(fl.ran && fl.hit_ok, "the Bard's hit lands on a Wolf %.0f cm away facing him", gap);
        CHECKF(fl.flew && fl.landed, "...the Wolf goes into the air (its movement mode is falling) and lands");
        CHECKF(near(fl.end_x, m.rest_x, rest_error) && near(fl.end_y, m.rest_y, 2.0),
               "...it rests at (%.1f, %.1f) cm from where it was hit; must be (%.1f, %.1f) cm (the data's throw through the body integrator), error allowed %.1f cm (1.5 %% + 2 cm) and 2.0 cm", fl.end_x, fl.end_y, m.rest_x,
               m.rest_y, rest_error);
        CHECKF(near(from_bard, stood + m.rest, rest_error), "...it comes to rest %.1f cm from the Bard, along the Bard's facing; must be %.1f cm (the distance it stood plus the throw), error allowed %.1f cm (1.5 %% + 2 cm)",
               from_bard, stood + m.rest, rest_error);
        CHECKF(near(fl.end_z_error, 0.0, 0.5), "...it rests on the ground: %.2f cm off it; must be 0.00 cm, error allowed 0.5 cm", fl.end_z_error);
        if (gap == 200.0) {
            const double apex_error = 0.015 * m.apex + 0.6;
            const double launch_vz = fl.first_vz + npc::kGravityCm / 15.0, launch_vh = std::hypot(fl.first_vx - fl.own_vx, fl.first_vy - fl.own_vy);
            const double launch_speed = std::hypot(launch_vh, launch_vz), launch_lift = std::atan2(launch_vz, launch_vh) * 180.0 / kKbPi;
            CHECKF(near(launch_speed, 1000.0, 0.015 * 1000.0 + 2.0) && near(launch_lift, 35.0, 0.5),
                   "...the velocity change it takes is %.1f cm/s at %.1f degrees up; must be 1000.0 cm/s at 35.0 degrees (the data's throw), error allowed %.1f cm/s (1.5 %% + 2 cm/s) and 0.5 degree", launch_speed, launch_lift,
                   0.015 * 1000.0 + 2.0);
            CHECKF(near(fl.apex, m.apex, apex_error), "...at 2 m it goes up %.2f cm; must be %.2f cm (the body integrator), error allowed %.2f cm (1.5 %% + 0.6 cm: its top is sampled once in 67 ms)", fl.apex, m.apex, apex_error);
            CHECKF(near(static_cast<double>(fl.air_ticks), m.air_seconds * 15.0, 1.5), "...it is in the air for %zu updates; must be %.1f (%.2f s of flight in updates of 67 ms), error allowed 1.5 updates", fl.air_ticks,
                   m.air_seconds * 15.0, m.air_seconds);
        }
    }

    // a standing Wolf 40 degrees off the Bard's facing at the same distances: it flies along the Bard's facing, not away from the Bard
    const double facing40 = (180.0 + 40.0) * kKbPi / 180.0;
    for (const double gap : {50.0, 100.0, 200.0, 300.0, 400.0, 500.0}) {
        const KbFlight fl = kb_flight(f, tables, wolf_path, gap, 40.0, 0.0);
        const KbDataThrow m = kb_data_throw(body, 1000.0, 35.0, std::cos(facing40), std::sin(facing40), fl.own_vx, fl.own_vy);
        const double error_x = 0.015 * std::fabs(m.rest_x) + 2.0, error_y = 0.015 * std::fabs(m.rest_y) + 2.0;
        std::printf("    standing %.0f cm off his facing, 40 degrees: %s, rests at (%.1f, %.1f) cm from where it was hit (the integrator along the Bard's facing (%.1f, %.1f); straight away from the Bard would be (%.1f, 0.0))\n",
                    gap, fl.hit_ok ? "thrown" : "no hit", fl.end_x, fl.end_y, m.rest_x, m.rest_y, -m.rest);
        CHECKF(fl.ran && fl.hit_ok && fl.flew && fl.landed, "the Bard's hit throws a Wolf that stands %.0f cm away, 40 degrees off his facing, and it lands", gap);
        CHECKF(near(fl.end_x, m.rest_x, error_x) && near(fl.end_y, m.rest_y, error_y),
               "...it rests at (%.1f, %.1f) cm from where it was hit; must be (%.1f, %.1f) cm along the Bard's facing, error allowed %.1f and %.1f cm (1.5 %% + 2 cm each): it flies along the Bard's facing, not away from the Bard",
               fl.end_x, fl.end_y, m.rest_x, m.rest_y, error_x, error_y);
        CHECKF(near(fl.end_z_error, 0.0, 0.5), "...it rests on the ground: %.2f cm off it; must be 0.00 cm, error allowed 0.5 cm", fl.end_z_error);
    }

    // a Wolf that charges at the Bard keeps its own velocity, and the throw is added to it: at 2 m, a closing speed of 300, 600, 900 and 1200 cm/s
    for (const double closing : {300.0, 600.0, 900.0, 1200.0}) {
        const KbFlight fl = kb_flight(f, tables, wolf_path, 200.0, 0.0, closing);
        const KbDataThrow m = kb_data_throw(body, 1000.0, 35.0, -1.0, 0.0, fl.own_vx, fl.own_vy);
        const double reach_error = 0.015 * m.rest + 2.0;
        std::printf("    charging at %.0f cm/s: its own speed when hit is %.1f cm/s, it ends (%.1f, %.1f) cm from where it was hit (the integrator (%.1f, %.1f))\n", closing, fl.own_vx, fl.end_x, fl.end_y, m.rest_x,
                    m.rest_y);
        CHECKF(fl.ran && fl.hit_ok && fl.flew && fl.landed, "a Wolf that charges at the Bard at %.0f cm/s is thrown, goes into the air and lands", closing);
        CHECKF(near(fl.own_vx, closing, 0.015 * closing + 5.0), "...its own speed when the hit lands is %.1f cm/s; must be %.0f cm/s, error allowed %.1f cm/s (1.5 %% + 5 cm/s)", fl.own_vx, closing,
               0.015 * closing + 5.0);
        CHECKF(near(fl.end_x, m.rest_x, reach_error) && near(fl.end_y, m.rest_y, 2.0),
               "...it ends at (%.1f, %.1f) cm from where it was hit; must be (%.1f, %.1f) cm (the throw added to its own speed), error allowed %.1f cm (1.5 %% + 2 cm) and 2.0 cm", fl.end_x, fl.end_y, m.rest_x, m.rest_y,
               reach_error);
    }

    // the Minotaur Berserker, on its own row of the table and with its own friction, thrown from the owner's 2.9 m and 3.9 m
    const std::string mino_path = creature_path(f, "/Minotaur/Berserker/Minotaur_Berserker.Default__Minotaur_Berserker_C");
    CHECK(!mino_path.empty() && f.motion.find(mino_path), "the Blueprint table has the Minotaur Berserker");
    if (!mino_path.empty() && f.motion.find(mino_path)) {
        const KbBody mino = kb_body(f, mino_path);
        for (const double gap : {290.0, 390.0}) {
            const KbFlight fl = kb_flight(f, tables, mino_path, gap, 0.0, 0.0);
            const KbDataThrow m = kb_data_throw(mino, 1000.0, 35.0, -1.0, 0.0, fl.own_vx, fl.own_vy);
            const double rest_error = 0.015 * m.rest + 2.0;
            const double from_bard = std::hypot(fl.start_x + fl.end_x - fl.bard_x, fl.start_y + fl.end_y - fl.bard_y);
            std::printf("    the Minotaur Berserker at %.0f cm (its friction %.0f): %s, rests %.1f cm from where it was hit (%.1f), %.1f cm from the Bard (%.1f), %zu updates in the air (%.1f)\n", gap, mino.friction,
                        fl.hit_ok ? "thrown" : "no hit", fl.reach, m.rest, from_bard, gap + m.rest, fl.air_ticks, m.air_seconds * 15.0);
            CHECKF(fl.ran && fl.hit_ok && fl.flew && fl.landed, "Get Off The Stage throws the Minotaur Berserker from %.0f cm", gap);
            CHECKF(near(fl.reach, m.rest, rest_error), "...it rests %.1f cm from where it was hit; must be %.1f cm (the body integrator with its own friction %.0f), error allowed %.1f cm (1.5 %% + 2 cm)", fl.reach, m.rest,
                   mino.friction, rest_error);
            CHECKF(near(from_bard, gap + m.rest, rest_error), "...it comes to rest %.1f cm from the Bard; must be %.1f cm, error allowed %.1f cm (1.5 %% + 2 cm)", from_bard, gap + m.rest, rest_error);
            CHECKF(near(static_cast<double>(fl.air_ticks), m.air_seconds * 15.0, 1.5), "...it is in the air for %zu updates; must be %.1f (%.2f s of flight in updates of 67 ms), error allowed 1.5 updates", fl.air_ticks,
                   m.air_seconds * 15.0, m.air_seconds);
        }
    }
}
AOC_REGISTER_TEST(test_kb_get_off_the_stage);

// The tuned throw is kept behind kGetOffTheStageModelled (false: the data's throw is what plays).  Its pure function turns the distance and the angle off the facing into a speed and a lift: at 2 m it
// is the record's own 1000 cm/s at 35 degrees, at 5 m half the speed at 20 degrees, and at 2 m it loses half its speed again at 45 degrees off the facing.
void test_kb_modelled_throw(Fixture&) {
    std::printf("the saved tuned throw still turns the distance and the angle off the facing into a speed and a lift (2 m: 1000 cm/s at 35 degrees; 5 m: 500 cm/s at 20 degrees; 2 m at 45 degrees off: 500 cm/s at 35 degrees)\n");
    const combat::ThrowShape two = combat::modelled_throw(200.0, 0.0, 1000.0, 35.0);
    CHECKF(near(two.speed, 1000.0, 1e-9) && near(two.lift_degrees, 35.0, 1e-9), "at 2 m facing the Bard the tuned throw is %.0f cm/s at %.1f degrees; must be 1000 cm/s at 35 degrees, error allowed 0", two.speed,
           two.lift_degrees);
    const combat::ThrowShape five = combat::modelled_throw(500.0, 0.0, 1000.0, 35.0);
    CHECKF(near(five.speed, 500.0, 1e-9) && near(five.lift_degrees, 20.0, 1e-9), "at 5 m facing the Bard the tuned throw is %.0f cm/s at %.1f degrees; must be 500 cm/s at 20 degrees, error allowed 0", five.speed,
           five.lift_degrees);
    const combat::ThrowShape side = combat::modelled_throw(200.0, 45.0, 1000.0, 35.0);
    CHECKF(near(side.speed, 500.0, 1e-9) && near(side.lift_degrees, 35.0, 1e-9), "at 2 m and 45 degrees off the Bard's facing the tuned throw is %.0f cm/s at %.1f degrees; must be 500 cm/s at 35 degrees, error allowed 0",
           side.speed, side.lift_degrees);
}
AOC_REGISTER_TEST(test_kb_modelled_throw);

// ---- the Mage's Cone of Cold push --------------------------------------------------------------------------------------------------------------------------------------
// With its talent the Cone of Cold pushes what it hits for the half second its effect lasts: a force of 1,500,000 less the square of the distance between the Mage and the creature, over a mass of 100,
// along the way the Mage faces, no more once the creature is within 50 cm of where the Mage stood.  The state the clients get names the push: the status flags (36), the record, who pushed, when and from
// where, and it clears with the effect.

/// The talent's ranks, as the data's expression asks for them: GetSkillPurchased(instigator, tree, skill) in the passive hit's requirement.
static bool kb_talent_ranks(Fixture& f, std::vector<stats::SkillRank>& out) {
    const abilities::HitDef* passive = f.catalog->hit_named("Mage_ConeOfCold_Hit_Passive");
    const content::ExprRow* req = passive && passive->row.requirement_expr != 0 ? f.catalog->expr(passive->row.requirement_expr) : nullptr;
    if (!req) return false;
    std::vector<stats::RecordRef> refs;
    for (size_t p = req->text.find("$#"); p != std::string::npos; p = req->text.find("$#", p + 1)) {
        char* end = nullptr;
        const unsigned long long type = std::strtoull(req->text.c_str() + p + 2, &end, 10);
        if (!end || *end != ':') continue;
        const long long guid = std::strtoll(end + 1, &end, 10);
        if (!end || *end != '$') continue;
        refs.push_back({static_cast<uint64_t>(type), static_cast<int64_t>(guid)});
    }
    if (refs.size() < 2) return false;
    out.push_back({refs[0], refs[1]});
    return true;
}

/// What one Cone of Cold on a Wolf `gap_cm` from the Mage did.  `how`: 0 the push runs its half second; 1 the Mage leaves the world a fifth of a second in; 2 the Wolf is killed a fifth of a second in.
struct KbPush {
    bool      ran = false, hit_ok = false;
    double    reach = 0, distance = 0, event_x = 0, settled_x = 0, peak_after = 0, peak_forward = 0, hit_now = 0;   // (`hit_now`: the time of the world at which the hit landed)
    size_t    on_i = 0, off_i = 0, event_i = 0, pushed_updates = 0;                                              // (`pushed_updates`: how many updates of the fight said the displaced bit (32) was on)
    KbPushNow mid, late;
};

static constexpr double kKbConeAt = 0.4;

static KbPush kb_push(Fixture& f, const combat::NpcAbilityData& tables, const std::string& wolf_path, double gap_cm, int how, KbRun& run, bool boss = false) {
    KbPush out;
    const abilities::HitDef* hit = f.catalog->hit_named("Mage_ConeOfCold_Hit");
    std::vector<stats::SkillRank> ranks;
    if (!hit || !kb_talent_ranks(f, ranks)) return out;
    npc::MotionTable boss_table;   // (`boss`: the table with the Boss tag on this Blueprint's row)
    if (boss && !kb_boss_table(wolf_path, boss_table)) return out;
    if (boss) std::swap(f.motion, boss_table);
    bool landed_hit = false, left = false, killed = false, mid_read = false, late_read = false;
    double x0 = 0;
    run = KbRun{};
    kb_fight(f, tables, wolf_path, 3.0, 0.0, 0.0, gap_cm, 0.0, [&](LungeStepContext& c) {
        const world::WorldActor* wolf = c.w.registry.find(c.creature);
        if (!wolf) return;
        if (!landed_hit && c.now >= kKbConeAt - 1e-9) {
            landed_hit = true;
            x0 = wolf->x;
            out.hit_now = c.now;
            out.distance = std::sqrt(wolf->x * wolf->x + wolf->y * wolf->y + (wolf->z - 1098.0) * (wolf->z - 1098.0));
            c.player.rig_fighter().face(0.0);
            c.player.fighter().stats().set_ranks(ranks);
            out.hit_ok = kb_land(c, *hit);
            return;
        }
        if (!landed_hit) return;
        if (!mid_read && c.now >= kKbConeAt + 0.2 - 1e-9) {
            mid_read = true;
            out.mid = kb_push_now(*wolf);
        }
        if (!late_read && c.now >= kKbConeAt + 1.2 - 1e-9) {
            late_read = true;
            out.late = kb_push_now(*wolf);
        }
        if (how == 1 && !left && c.now >= kKbConeAt + 0.2 - 1e-9) {
            left = true;
            c.world.remove_player(&c.player);
        }
        if (how == 2 && !killed && c.now >= kKbConeAt + 0.2 - 1e-9) {
            killed = true;
            combat::Resolved lethal;
            lethal.mods.push_back({1, content::kGuidHealth, -1.0e9});
            c.world.apply_hit_to_npc(c.creature, c.player.actor_id(), lethal, c.now);
        }
    }, run);
    if (boss) std::swap(f.motion, boss_table);
    out.ran = run.ran;
    if (!run.ran || run.ticks.empty()) return out;
    for (const LungeSample& smp : run.s) out.pushed_updates += smp.cc_flags >= 0 && (smp.cc_flags & 32) != 0 ? 1 : 0;   // (-1: the update does not say)
    // the ticks the effect was on, by the state the updates carried
    out.on_i = out.off_i = run.s.size();
    for (size_t i = 0; i < run.s.size(); ++i) {
        if (out.on_i == run.s.size() && run.s[i].cc_flags == 36 && run.s[i].cc_items >= 1) out.on_i = i;
        if (out.on_i != run.s.size() && i > out.on_i && run.s[i].cc_flags == 0) {
            out.off_i = i;
            break;
        }
    }
    // where it came to rest: the first tick after it started to move at which it is nearly still (a Wolf that is not moved rests where it stood)
    const size_t h = kb_index(run.ticks, kKbConeAt);
    size_t moved = run.ticks.size(), rest = h < run.ticks.size() ? h : 0;
    for (size_t i = h; i < run.ticks.size(); ++i) {
        if (std::hypot(run.ticks[i].vx, run.ticks[i].vy) > 15.0) {
            moved = i;
            break;
        }
    }
    if (moved < run.ticks.size()) {
        rest = run.ticks.size() - 1;
        for (size_t i = moved + 1; i < run.ticks.size(); ++i) {
            if (std::hypot(run.ticks[i].vx, run.ticks[i].vy) <= 15.0) {
                rest = i;
                break;
            }
        }
    }
    out.reach = run.ticks[rest].x - x0;
    for (size_t i = h; i < run.ticks.size(); ++i) out.peak_forward = std::max(out.peak_forward, run.ticks[i].x - x0);   // (the farthest it ever was along the Mage's facing: a Wolf that is not pushed walks back to the Mage)
    // what became of it after the event (the Mage leaves, the Wolf dies): where it stood 0.4 s later, and the farthest it was after that
    out.event_i = kb_index(run.ticks, kKbConeAt + 0.2);
    const size_t settled = kb_index(run.ticks, kKbConeAt + 0.6);
    if (settled < run.ticks.size()) {
        out.settled_x = run.ticks[settled].x - x0;
        out.peak_after = out.settled_x;
        for (size_t i = settled; i < run.ticks.size(); ++i) out.peak_after = std::max(out.peak_after, run.ticks[i].x - x0);
    }
    return out;
}

void test_kb_cone_of_cold_push(Fixture& f) {
    std::printf("the Cone of Cold's push (with its talent) throws a Wolf along the way the Mage faces by the force of that distance, falls with the square of it and reaches nothing from 12.2 m; the state names the push and clears with its effect\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string wolf_path = creature_path(f, "/Beast/Canine/Wolf.Default__Wolf_C");
    const abilities::EffectDef* push = f.catalog->effect_named("Mage_ConeOfCold_Push");
    CHECK(!wolf_path.empty() && f.motion.find(wolf_path) && tables.abilities_of(wolf_path) && push && push->row.crowd_control_guid != 0,
          "the data has the Wolf, the tables its abilities, and the Cone of Cold's push effect with its crowd control record");
    if (wolf_path.empty() || !f.motion.find(wolf_path) || !tables.abilities_of(wolf_path) || !push || push->row.crowd_control_guid == 0) return;
    const double seconds = 0.5;   // the effect's length

    // ---- the distances: the force worked out step by step for the friction the Wolf has, and the numbers the data's recordings fit
    const KbBody body = kb_body(f, wolf_path);
    CHECKF(near(body.friction, 24.0, 1e-9), "the Wolf's ground friction is %.0f; must be 24, error allowed 0 (its Blueprint sets it: a Wolf's push dies three times as fast as the engine's default)", body.friction);
    for (const double gap : {200.0, 500.0, 1000.0}) {
        KbRun run;
        const KbPush p = kb_push(f, tables, wolf_path, gap, 0, run);
        const double dz = std::sqrt(std::max(0.0, p.distance * p.distance - gap * gap));
        const double model = kb_cone_reach(gap, dz, body.mass, body.friction, body.braking, seconds);
        const double fitted = gap == 200.0 ? 240.0 : gap == 500.0 ? 190.0 : 50.0;   // the numbers for a friction of 24 that the data's recordings fit: 2.4 m at 2 m, 1.9 m at 5 m, 0.5 m at 10 m
        const double model_error = 0.015 * model + 2.0, fitted_error = 0.02 * fitted + 5.0;
        std::printf("    %.0f cm from the Mage: %s, pushed %.1f cm (the force worked out step by step %.1f, the fitted %.0f), the updates say flags 36 from update %zu to update %zu\n", gap, p.hit_ok ? "hit" : "no hit", p.reach, model, fitted,
                    p.on_i, p.off_i);
        CHECKF(p.ran && p.hit_ok, "the Cone of Cold lands on a Wolf %.0f cm from the Mage", gap);
        CHECKF(near(p.reach, model, model_error), "...it is pushed %.1f cm along the Mage's facing; must be %.1f cm (the force of 1,500,000 less the square of the distance, over a mass of %.0f, for half a second, friction %.0f), error allowed %.1f cm (1.5 %% + 2 cm)",
               p.reach, model, body.mass, body.friction, model_error);
        CHECKF(near(p.reach, fitted, fitted_error), "...and the number the data's recordings fit for this distance: %.1f cm; must be %.0f cm, error allowed %.1f cm (2 %% + 5 cm: the fit is given to 0.1 m)", p.reach, fitted, fitted_error);
    }
    {
        KbRun run;
        const KbPush p = kb_push(f, tables, wolf_path, 1300.0, 0, run);
        CHECKF(p.ran && p.hit_ok, "the Cone of Cold lands on a Wolf 13 m from the Mage");
        CHECKF(near(p.peak_forward, 0.0, 0.5), "...the force is under the braking: the Wolf goes %.2f cm along the Mage's facing at most; must be 0.00 cm, error allowed 0.5 cm (a Wolf that is not pushed walks back to the Mage)", p.peak_forward);
    }

    // ---- the state: flags 36 and the record in the update that carries the effect, who pushed and from where, and everything back at the end of the effect
    {
        KbRun run;
        const KbPush p = kb_push(f, tables, wolf_path, 200.0, 0, run);
        const int64_t record = push->row.crowd_control_guid;
        CHECKF(p.on_i < run.s.size() && run.s[p.on_i].cc_flags == 36 && run.s[p.on_i].cc_items >= 1, "the update that carries the effect says the crowd control state: flags %d (36 is a push that stops its abilities), with %zu item(s)",
               p.on_i < run.s.size() ? run.s[p.on_i].cc_flags : -1, p.on_i < run.s.size() ? run.s[p.on_i].cc_items : 0);
        CHECKF(p.mid.flags == 36 && p.mid.record == record, "...it names the push record %lld (the state holds %lld, flags %lld)", static_cast<long long>(record), static_cast<long long>(p.mid.record), static_cast<long long>(p.mid.flags));
        const uint32_t who[4] = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
        bool by_player = true, source_player = true;
        for (int i = 0; i < 4; ++i) {
            by_player = by_player && static_cast<uint32_t>(p.mid.instigator[i]) == who[i];
            source_player = source_player && static_cast<uint32_t>(p.mid.source[i]) == who[i];
        }
        CHECK(by_player && source_player, "...the player as who pushed it and where it came from");
        CHECKF(near(p.mid.start, bootstrap::effect_clock(p.hit_now), 0.001), "...when the push began: %.3f; must be %.3f (the clock of the effects at the hit), error allowed 0.001 s", p.mid.start, bootstrap::effect_clock(p.hit_now));
        CHECKF(p.mid.has_origin && near(p.mid.ox, 0.0, 1.0) && near(p.mid.oy, 0.0, 1.0), "...and the place the push works from: (%.1f, %.1f); must be (0, 0), where the Mage stood, error allowed 1.0 cm", p.mid.ox, p.mid.oy);
        CHECKF(p.off_i < run.s.size() && p.on_i < run.s.size() && run.s[p.off_i].cc_flags == 0 && near(run.s[p.off_i].t - run.s[p.on_i].t, seconds, 0.07),
               "the update that takes the effect's item off says the flags are 0 again, %.3f s after the one that put it on; must be 0.500 s (the effect's length), error allowed 0.07 s (one update of 67 ms)",
               p.off_i < run.s.size() && p.on_i < run.s.size() ? run.s[p.off_i].t - run.s[p.on_i].t : -1.0);
        CHECKF(p.late.flags == 0 && p.late.record == 0 && p.late.start == 0.0 && !p.late.has_origin, "...and the record, who pushed, when and from where are back at nothing (flags %lld, record %lld)", static_cast<long long>(p.late.flags),
               static_cast<long long>(p.late.record));
    }

    // ---- the push ends with the Mage who made it and with the Wolf: no force is left, and no velocity
    for (const int how : {1, 2}) {
        KbRun run;
        const KbPush p = kb_push(f, tables, wolf_path, 200.0, how, run);
        const size_t at = kb_index(run.ticks, kKbConeAt + 0.2);
        const bool cleared = at < run.s.size() && run.s[at].cc_flags == 0 && run.s[at].held_effects == 0;
        const size_t n = run.ticks.size();
        std::printf("    %s: pushed %.0f cm before it, %.0f cm by 0.4 s later, no farther than %.0f cm after that\n", how == 1 ? "the Mage leaves" : "the Wolf dies", p.reach, p.settled_x, p.peak_after);
        CHECKF(p.ran && p.hit_ok && p.on_i < run.s.size(), "%s a fifth of a second into the push: the push began", how == 1 ? "the Mage leaves the world" : "the Wolf is killed");
        CHECKF(cleared, "...the update of that tick says the state is cleared (flags %d) and the Wolf holds no effect (%zu)", at < run.s.size() ? run.s[at].cc_flags : -1, at < run.s.size() ? run.s[at].held_effects : 0);
        CHECKF(near(p.peak_after - p.settled_x, 0.0, 3.0), "...nothing pushes it any more: it goes %.2f cm farther than where it stood 0.6 s after the hit; must be 0.00 cm, error allowed 3.0 cm", p.peak_after - p.settled_x);
        if (how == 2) {
            const double end_speed = n > 0 ? std::hypot(run.ticks[n - 1].vx, run.ticks[n - 1].vy) : -1.0;
            const double end_drift = n > 0 ? std::fabs(run.ticks[n - 1].x - run.ticks[kb_index(run.ticks, kKbConeAt + 1.0)].x) : -1.0;
            CHECKF(near(end_speed, 0.0, 1.0), "...and a dead Wolf has a velocity of %.2f cm/s at the end; must be 0.00 cm/s, error allowed 1.0 cm/s", end_speed);
            CHECKF(near(end_drift, 0.0, 0.5), "...and it moves %.2f cm between 1.0 s after the hit and the end; must be 0.00 cm, error allowed 0.5 cm", end_drift);
        }
    }
}
AOC_REGISTER_TEST(test_kb_cone_of_cold_push);

// ---- the Cone of Cold's freeze -------------------------------------------------------------------------------------------------------------------------------------------
// The Frozen the Cone of Cold puts on a Chilled creature comes with the hit when the Mage has no talent.  With the talent the hit pushes instead, and the freeze is a second hit it fires a second later
// (the passive hit's delay), which freezes the creature if it is still Chilled.

struct KbFreeze {
    bool   ran = false;
    double request_at = -1, push_at = -1, freeze_at = -1;
    int    refusal = -1;
};

static KbFreeze kb_freeze(Fixture& f, const std::string& wolf_path, bool talent) {
    KbFreeze out;
    Rig rig(8, 25, {});
    if (!rig.ok()) return out;
    const abilities::AbilityDef* ability = f.catalog->ability_named("Mage_ConeOfCold");
    const abilities::HitDef* hit = f.catalog->hit_named("Mage_ConeOfCold_Hit");
    const abilities::EffectDef* chilled = f.catalog->effect_named("Status_Chilled");
    const abilities::EffectDef* frozen = f.catalog->effect_named("Status_Frozen");
    const abilities::EffectDef* push = f.catalog->effect_named("Mage_ConeOfCold_Push");
    std::vector<stats::SkillRank> ranks;
    if (!ability || !hit || !chilled || !frozen || !push || (talent && !kb_talent_ranks(f, ranks))) return out;
    combat_rig::World w;
    if (!w.problem.empty()) return out;
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion);
    const uint64_t wolf = w.spawn(wolf_path, 300, 0, 8, 100);
    if (wolf == 0) return out;
    combat_rig::FakePlayer player(rig, 77, {0, 0, 1098});
    world.add_player(&player);
    combat::WorldHitPort port(world, w.registry, player.fighter());
    rig.pipe->set_combat(&port);
    rig.know({"Mage_ConeOfCold"});
    if (talent) rig.use_extra_ranks(ranks);
    int64_t targeting = content::kTargetingDefaultSelf;
    for (const content::AbilityHitRow& h : ability->hits) {
        if (h.hit_guid != hit->row.guid || h.source != "hit") continue;
        for (const content::TargetingRow& tg : ability->targetings)
            if (tg.key == h.key) targeting = tg.guid;
    }
    const double t = 5.0;
    effects::Applied chill;
    chill.def = chilled;
    chill.duration = 30.0;
    world.apply_effects(wolf, 77, {chill}, t);
    TriggerInfo info;   // (a cone has no target of its own: the client names the creatures it hits when it asks for the hit)
    const Refusal refusal = rig.pipe->trigger(ability->row.guid, 10, t, info);
    out.refusal = static_cast<int>(refusal);
    if (refusal != Refusal::None) return out;
    bool asked = false;
    for (double now = t; now < t + 4.0; now += 0.02) {
        rig.run_to(now);
        if (!asked && now >= t + 0.5) {
            asked = true;
            out.request_at = now;
            rig.pipe->hit_request(ability->row.guid, targeting, hit->row.guid, 1, 0, now, {wolf});
        }
        w.npcs->note_player(77, 0.0, 0.0, now);
        w.npcs->tick(now);
        world.tick(now);
        if (out.push_at < 0 && world.effect_remaining(wolf, push->row.guid, now) > 0.0) out.push_at = now;
        if (out.freeze_at < 0 && world.effect_remaining(wolf, frozen->row.guid, now) > 0.0) out.freeze_at = now;
    }
    out.ran = true;
    return out;
}

void test_kb_cone_of_cold_freeze(Fixture& f) {
    std::printf("the Cone of Cold's freeze: with the talent the hit pushes and the freeze comes a second later (not before), without it the freeze comes with the hit\n");
    const std::string wolf_path = creature_path(f, "/Beast/Canine/Wolf.Default__Wolf_C");
    CHECK(!wolf_path.empty(), "the Blueprint table has the Wolf");
    if (wolf_path.empty()) return;
    const KbFreeze with = kb_freeze(f, wolf_path, true);
    std::printf("    with the talent: asked at %.2f, pushed at %.2f, frozen at %.2f (refusal %d)\n", with.request_at, with.push_at, with.freeze_at, with.refusal);
    CHECKF(with.ran && with.push_at >= 0.0 && near(with.push_at - with.request_at, 0.10, 0.10), "with the talent the hit lands and puts the push on the Wolf %.2f s after it was asked for; must be 0.10 s, error allowed 0.10 s", with.push_at - with.request_at);
    CHECKF(with.freeze_at >= 0.0 && with.push_at >= 0.0 && near(with.freeze_at - with.push_at, 1.00, 0.04), "...and the Wolf is frozen %.2f s after the push began; must be 1.00 s (the passive hit's delay), error allowed 0.04 s (the loop steps 20 ms)",
           with.freeze_at - with.push_at);
    const KbFreeze without = kb_freeze(f, wolf_path, false);
    std::printf("    without the talent: asked at %.2f, pushed at %.2f, frozen at %.2f (refusal %d)\n", without.request_at, without.push_at, without.freeze_at, without.refusal);
    CHECKF(without.ran && without.freeze_at >= 0.0 && near(without.freeze_at - without.request_at, 0.10, 0.10), "without the talent the Wolf is frozen with the hit, %.2f s after it was asked for; must be 0.10 s, error allowed 0.10 s", without.freeze_at - without.request_at);
    CHECK(without.push_at < 0.0, "...and it is not pushed (no push effect at any time: the clock stays at -1)");
}
AOC_REGISTER_TEST(test_kb_cone_of_cold_freeze);

// ---- the Tank's Ground Pound -----------------------------------------------------------------------------------------------------------------------------------------------
// Ground Pound hits a tripped creature for damage and puts a tenth of a second of crowd control on it.  The push inside it starts where it already is (the data's origin is 100 cm under the creature and the
// push counts as arrived within 100 cm), so the creature is not lifted: it stays where it stands.

void test_kb_ground_pound(Fixture& f) {
    std::printf("Ground Pound hurts a tripped Wolf and puts a tenth of a second of push on it (flags 36 on top of the trip's 7), and does not lift it\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string wolf_path = creature_path(f, "/Beast/Canine/Wolf.Default__Wolf_C");
    const abilities::HitDef* launch = f.catalog->hit_named("Tank_GroundPound_Launch Hit");
    const abilities::EffectDef* tripped = f.catalog->effect_named("Status_Tripped");
    CHECK(!wolf_path.empty() && f.motion.find(wolf_path) && tables.abilities_of(wolf_path) && launch && tripped, "the data has the Wolf, the tables its abilities, Ground Pound's second hit and the Tripped effect");
    if (wolf_path.empty() || !f.motion.find(wolf_path) || !tables.abilities_of(wolf_path) || !launch || !tripped) return;
    bool tripped_on = false, struck = false;
    double health_before = 0, health_after = 0, start_x = 0, start_y = 0, start_z = 0;
    KbRun run;
    kb_fight(f, tables, wolf_path, 2.5, 0.0, 0.0, 300.0, 0.0, [&](LungeStepContext& c) {
        const world::WorldActor* wolf = c.w.registry.find(c.creature);
        if (!wolf) return;
        if (!tripped_on && c.now >= 0.3 - 1e-9) {
            tripped_on = true;
            combat::Fighter* target = c.world.fighter_of(c.creature);   // (it becomes a combatant)
            (void)target;
            effects::Applied trip;
            trip.def = tripped;
            trip.duration = 3.0;
            c.world.apply_effects(c.creature, c.player.actor_id(), {trip}, c.now);
        }
        if (!struck && c.now >= 0.7 - 1e-9) {
            struck = true;
            start_x = wolf->x;
            start_y = wolf->y;
            start_z = wolf->z;
            health_before = c.world.health_of(c.creature);
            kb_land(c, *launch, true);
            health_after = c.world.health_of(c.creature);
        }
    }, run);
    CHECK(run.ran && struck, "the fight runs and the second hit lands on the tripped Wolf");
    if (!run.ran) return;
    CHECKF(health_after < health_before, "the hit takes Health from it (%.1f -> %.1f; it must take more than 0.0: the damage formula is not under test here)", health_before, health_after);
    // the state: the trip's flags (7) with the push's (36) for a tenth of a second, then the trip's again
    size_t on = run.s.size(), off = run.s.size();
    for (size_t i = 0; i < run.s.size(); ++i) {
        if (on == run.s.size() && run.s[i].t >= 0.7 - 1e-9 && run.s[i].cc_flags == 39) on = i;
        if (on != run.s.size() && i > on && run.s[i].cc_flags == 7) {
            off = i;
            break;
        }
    }
    CHECKF(on < run.s.size() && off < run.s.size(), "an update says flags 39 (the trip's 7 and the push's 36), and a later one says 7 again (updates %zu and %zu)", on, off);
    const double cc_seconds = on < run.s.size() && off < run.s.size() ? run.s[off].t - run.s[on].t : -1.0;
    CHECKF(near(cc_seconds, 0.1, 0.07), "...the push's crowd control is on it for %.3f s; must be 0.100 s (the effect's length), error allowed 0.07 s (one update of 67 ms)", cc_seconds);
    double lift = 0, drift = 0;
    size_t falling = 0;
    for (const KbTick& k : run.ticks) {
        if (k.t < 0.7 - 1e-9) continue;
        lift = std::max(lift, std::fabs(k.z - start_z));
        drift = std::max(drift, std::hypot(k.x - start_x, k.y - start_y));
        if (k.mode == 3) ++falling;
    }
    CHECKF(near(lift, 0.0, 0.5), "...it is not lifted: %.2f cm up or down at most; must be 0.00 cm, error allowed 0.5 cm", lift);
    CHECKF(near(drift, 0.0, 0.5), "...it does not move along the ground: %.2f cm at most; must be 0.00 cm, error allowed 0.5 cm", drift);
    CHECKF(falling == 0, "...and its movement mode is never falling (%zu updates; must be 0, error allowed 0)", falling);
}
AOC_REGISTER_TEST(test_kb_ground_pound);

// ---- the Tank's Grapple pull -----------------------------------------------------------------------------------------------------------------------------------------------
// The hit's teleport puts the creature in front of the Tank, 300 cm and both their radii from his centre, facing him.

void test_kb_grapple_pull(Fixture& f) {
    std::printf("the Tank's Grapple pulls a Wolf in front of him, 300 cm and both their radii from him, facing him\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string wolf_path = creature_path(f, "/Beast/Canine/Wolf.Default__Wolf_C");
    const abilities::HitDef* pull = f.catalog->hit_named("Tank_Grapple_Teleport_Hit");
    const auto bp = f.motion.find(wolf_path);
    CHECK(!wolf_path.empty() && bp && tables.abilities_of(wolf_path) && pull, "the data has the Wolf, the tables its abilities and the Grapple's teleport hit");
    if (wolf_path.empty() || !bp || !tables.abilities_of(wolf_path) || !pull) return;
    // the Tank at (500, 500) faces 90 degrees (towards +y); the Wolf stands 8 m off
    bool pulled = false, landed_hit = false;
    double put_x = 0, put_y = 0;   // where the pull put it, before the world moved it on (a Wolf in reach bites at once, and its bite carries it a step)
    KbRun run;
    kb_fight(f, tables, wolf_path, 1.5, 500.0, 500.0, 500.0 + 800.0, 500.0, [&](LungeStepContext& c) {
        if (landed_hit || c.now < 0.4 - 1e-9) return;
        landed_hit = true;
        c.player.rig_fighter().face(90.0);
        pulled = kb_land(c, *pull);
        if (const world::WorldActor* wolf = c.w.registry.find(c.creature)) {
            put_x = wolf->x;
            put_y = wolf->y;
        }
    }, run);
    CHECK(run.ran && pulled, "the hit lands");
    if (!run.ran || !pulled) return;
    const size_t h = kb_index(run.ticks, 0.4);
    CHECK(h < run.ticks.size(), "the fight ran past the hit");
    if (h >= run.ticks.size()) return;
    const double want_gap = 300.0 + bp->radius + 34.0;
    const KbTick& k = run.ticks[h];
    const double gap = std::hypot(put_x - 500.0, put_y - 500.0);
    std::printf("    the Wolf is put at (%.2f, %.2f), %.2f cm from the Tank (300 + its radius %.0f + his 34 = %.0f), facing %.1f degrees at the end of the tick\n", put_x, put_y, gap, bp->radius, want_gap, k.yaw);
    CHECKF(near(put_x, 500.0, 1.0), "it is put at x = %.2f; must be 500.00 (straight ahead of the Tank, who faces +y), error allowed 1.0 cm", put_x);
    CHECKF(near(put_y, 500.0 + want_gap, 1.0), "...at y = %.2f, %.2f cm in front of him; must be %.2f cm (300 + the Wolf's radius %.0f + his 34), error allowed 1.0 cm", put_y, put_y - 500.0, want_gap, bp->radius);
    CHECKF(near(wrap180(k.yaw - 270.0), 0.0, 1.0), "...facing %.1f degrees; must be 270.0 (toward him), error allowed 1.0 degree", k.yaw);
}
AOC_REGISTER_TEST(test_kb_grapple_pull);

// ---- the Ranger's Air Strike ---------------------------------------------------------------------------------------------------------------------------------------------
// Air Strike roots a creature that is Snared, and takes off up to three of its snares: a snare's tag is the one the hit's filter names.

void test_kb_air_strike(Fixture& f) {
    std::printf("Air Strike on a Snared Wolf roots it and takes the snare off: the flags go from 16 to 1, in one update\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string wolf_path = creature_path(f, "/Beast/Canine/Wolf.Default__Wolf_C");
    const abilities::HitDef* strike = f.catalog->hit_named("Ranger_AirStrike_AoE");
    const abilities::EffectDef* snared = f.catalog->effect_named("Generic_Snared_Effect");
    const abilities::EffectDef* rooted = f.catalog->effect_named("Status_Rooted");
    CHECK(!wolf_path.empty() && f.motion.find(wolf_path) && tables.abilities_of(wolf_path) && strike && snared && rooted, "the data has the Wolf, the tables its abilities, Air Strike's area hit and the Snared and Rooted effects");
    if (wolf_path.empty() || !f.motion.find(wolf_path) || !tables.abilities_of(wolf_path) || !strike || !snared || !rooted) return;
    bool snare_on = false, struck = false;
    double snare_left_before = 0, snare_left_after = -1, root_left_after = 0;
    KbRun run;
    kb_fight(f, tables, wolf_path, 2.0, 0.0, 0.0, 900.0, 0.0, [&](LungeStepContext& c) {
        if (!snare_on && c.now >= 0.3 - 1e-9) {
            snare_on = true;
            c.world.fighter_of(c.creature);   // (it becomes a combatant)
            effects::Applied a;
            a.def = snared;
            a.duration = 8.0;
            c.world.apply_effects(c.creature, c.player.actor_id(), {a}, c.now);
        }
        if (!struck && c.now >= 0.7 - 1e-9) {
            struck = true;
            snare_left_before = c.world.effect_remaining(c.creature, snared->row.guid, c.now);
            kb_land(c, *strike);
            snare_left_after = c.world.effect_remaining(c.creature, snared->row.guid, c.now);
            root_left_after = c.world.effect_remaining(c.creature, rooted->row.guid, c.now);
        }
    }, run);
    CHECK(run.ran && struck, "the fight runs and Air Strike's area hit lands");
    if (!run.ran) return;
    CHECKF(near(snare_left_before, 7.6, 0.07), "the Wolf is Snared first: %.2f s left; must be 7.60 s (8.0 s put on 0.4 s before), error allowed 0.07 s (one update of 67 ms)", snare_left_before);
    CHECKF(snare_left_after == 0.0, "after the hit the Snared is gone: %.2f s left; must be 0.00 s, error allowed 0", snare_left_after);
    CHECKF(near(root_left_after, 4.1, 0.1), "...and the Wolf is Rooted for %.2f s; must be 4.10 s (4 s times the caster's disable duration of 1.016 to 1.037), error allowed 0.1 s", root_left_after);
    size_t on = run.s.size(), hit_tick = run.s.size();
    for (size_t i = 0; i < run.s.size(); ++i) {
        if (on == run.s.size() && run.s[i].cc_flags == 16) on = i;
        if (hit_tick == run.s.size() && run.s[i].t >= 0.7 - 1e-9) hit_tick = i;
    }
    CHECKF(on < run.s.size() && hit_tick < run.s.size() && on < hit_tick, "the snare's flags (16) reached the clients before the strike (tick %zu, the strike in %zu)", on, hit_tick);
    CHECKF(hit_tick < run.s.size() && run.s[hit_tick].cc_flags == 1 && run.s[hit_tick].has_cc_snare && run.s[hit_tick].cc_snare == 0.0 && run.s[hit_tick].cc_items >= 1,
           "...the update of the strike says the flags are 1 (a root and nothing else) and the snare 0, with the item of the new effect (flags %d, snare %.2f, %zu items)",
           hit_tick < run.s.size() ? run.s[hit_tick].cc_flags : -1, hit_tick < run.s.size() ? run.s[hit_tick].cc_snare : -1.0, hit_tick < run.s.size() ? run.s[hit_tick].cc_items : 0);
}
AOC_REGISTER_TEST(test_kb_air_strike);

// ---- a boss is not moved ---------------------------------------------------------------------------------------------------------------------------------------------------------
// A creature whose Blueprint carries the Boss tag is not thrown, not pushed and not pulled.  The tag is the actor's Tags (AActor::Tags) of the Blueprint's class default object, nearest class first: the
// Blueprint table's tags column has them.  Every part is measured on its own: each Blueprint, each ability (the throw, the push, the pull), each number.

/// Whether the table's row of a Blueprint has the tag in its tags column (comma separated).
static bool kb_has_tag(const npc::MotionTable& table, const std::string& path, const std::string& want) {
    const auto bp = table.find(path);
    if (!bp) return false;
    size_t from = 0;
    while (from <= bp->tags.size()) {
        size_t to = bp->tags.find(',', from);
        if (to == std::string::npos) to = bp->tags.size();
        if (bp->tags.compare(from, to - from, want) == 0 && to - from == want.size()) return true;
        from = to + 1;
    }
    return false;
}

/// Where the Tank's Grapple left a creature right after the hit: the Tank stands at (500, 500) facing +y, the creature `gap_cm` from him along +x.  `boss`: its row of the table carries the Boss tag.
struct KbPull {
    bool   ran = false, hit_ok = false;
    double start_x = 0, start_y = 0, put_x = 0, put_y = 0;
};

static KbPull kb_pull(Fixture& f, const combat::NpcAbilityData& tables, const std::string& path, double gap_cm, bool boss) {
    KbPull out;
    const abilities::HitDef* pull = f.catalog->hit_named("Tank_Grapple_Teleport_Hit");
    if (!pull) return out;
    npc::MotionTable boss_table;
    if (boss && !kb_boss_table(path, boss_table)) return out;
    if (boss) std::swap(f.motion, boss_table);
    bool landed = false;
    KbRun run;
    kb_fight(f, tables, path, 1.0, 500.0, 500.0, 500.0 + gap_cm, 500.0, [&](LungeStepContext& c) {
        const world::WorldActor* creature = c.w.registry.find(c.creature);
        if (!creature || landed || c.now < 0.4 - 1e-9) return;
        landed = true;
        out.start_x = creature->x;
        out.start_y = creature->y;
        c.player.rig_fighter().face(90.0);
        out.hit_ok = kb_land(c, *pull);
        if (const world::WorldActor* after = c.w.registry.find(c.creature)) {
            out.put_x = after->x;
            out.put_y = after->y;
        }
    }, run);
    if (boss) std::swap(f.motion, boss_table);
    out.ran = run.ran;
    return out;
}

void test_kb_boss_tag_in_the_data(Fixture& f) {
    std::printf("the Blueprint table's tags column holds the actor Tags: Boss is on the world bosses, the named bosses and the giants, and not on a Wolf, a Bear or a rare spawn that is only called a boss\n");
    struct Want {
        const char* leaf;
        bool        boss;
        const char* what;
    };
    static const Want kWant[] = {
        {"/GiantCyclops/CyclopsShaman.Default__CyclopsShaman_C", true, "Tumok the Wretched's (the Cyclops Shaman world boss)"},
        {"/NamedBoss/Dunir_Fighter_Forgelord_Zammer.Default__Dunir_Fighter_Forgelord_Zammer_C", true, "Forgelord Zammer's (a named boss)"},
        {"/Scorpion/SklaRisha_MotherOfPain.Default__Sklarisha_MotherOfPain_C", true, "the scorpion world boss's"},
        {"/MagmaGolem/MagmaGolem_Bellowsmasher.Default__MagmaGolem_Bellowsmasher_C", true, "the Bellowsmasher's"},
        {"/FlameElemental_TheUntendedFlame.Default__FlameElemental_TheUntendedFlame_C", true, "the Untended Flame's"},
        {"/Giants/Giant_Base.Default__Giant_Base_C", true, "the giants'"},
        {"/Warbeast_Child_Quest.Default__Warbeast_Child_Quest_C", true, "the quest warbeast's"},
        {"/Canine/Wolf.Default__Wolf_C", false, "the Wolf's"},
        {"/Bear/Bear.Default__Bear_C", false, "the Bear's"},
        {"/GoblinWarlord/Goblin_RareSpawn_BossNakNak.Default__Goblin_RareSpawn_BossNakNak_C", false, "the rare spawn NakNak's (called a boss, tagged by nobody)"},
        {"/LariaLeMonte/Laria_LeMonte.Default__Laria_LeMonte_C", false, "Laria Le Monte's (a quest boss of rank 4, tagged by nobody)"},
    };
    for (const Want& w : kWant) {
        const std::string path = creature_path(f, w.leaf);
        const auto bp = path.empty() ? std::optional<npc::MotionStats>{} : f.motion.find(path);
        CHECKF(bp.has_value(), "the table has %s Blueprint", w.what);
        if (!bp) continue;
        CHECKF(kb_has_tag(f.motion, path, "Boss") == w.boss, "...its tags %s the Boss tag (\"%s\"); must %s", w.boss ? "hold" : "do not hold", bp->tags.c_str(), w.boss ? "hold it" : "not hold it");
    }
    size_t bosses = 0, tagged = 0;
    for (const auto& [path, row] : f.motion.rows()) {
        if (!row.tags.empty()) ++tagged;
        if (kb_has_tag(f.motion, path, "Boss")) ++bosses;
    }
    CHECKF(bosses == 36, "36 Blueprints of the table carry the Boss tag; %zu do (must be 36, error allowed 0)", bosses);
    CHECKF(tagged == 197, "197 Blueprints of the table have some tag in the column; %zu do (must be 197, error allowed 0)", tagged);
}
AOC_REGISTER_TEST(test_kb_boss_tag_in_the_data);

void test_kb_boss_tag_gate(Fixture& f) {
    std::printf("a Wolf whose row carries the Boss tag is not thrown, not pushed and not pulled; a Wolf without it is, each measured on its own\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string wolf_path = creature_path(f, "/Beast/Canine/Wolf.Default__Wolf_C");
    CHECK(!wolf_path.empty() && f.motion.find(wolf_path) && tables.abilities_of(wolf_path), "the Blueprint table has the Wolf and the creature tables list its abilities");
    if (wolf_path.empty() || !f.motion.find(wolf_path) || !tables.abilities_of(wolf_path)) return;
    const KbBody body = kb_body(f, wolf_path);

    // Get Off The Stage, the Wolf 2 m from the Bard, facing him
    const KbFlight thrown = kb_flight(f, tables, wolf_path, 200.0, 0.0, 0.0);
    const KbFlight held = kb_flight(f, tables, wolf_path, 200.0, 0.0, 0.0, true);
    const KbThrow model = kb_throw_model(2.0, 0.0, body.friction, body.braking);
    std::printf("    Get Off The Stage at 2 m: a Wolf rests %.1f cm away (the model %.1f); a boss %.2f cm, %.2f cm up, %zu updates in the air\n", thrown.reach, model.rest, held.reach, held.apex, held.air_ticks);
    CHECKF(thrown.ran && thrown.hit_ok && near(thrown.reach, model.rest, 0.015 * model.rest + 2.0), "a Wolf without the tag is thrown %.1f cm; must be %.1f cm (the model), error allowed %.1f cm (1.5 %% + 2 cm)", thrown.reach, model.rest, 0.015 * model.rest + 2.0);
    CHECK(held.ran && held.hit_ok, "Get Off The Stage lands on a Wolf with the Boss tag");
    CHECKF(near(held.reach, 0.0, 0.5), "...it moves %.2f cm; must be 0.00 cm, error allowed 0.5 cm", held.reach);
    CHECKF(near(held.apex, 0.0, 0.5), "...it goes up %.2f cm; must be 0.00 cm, error allowed 0.5 cm", held.apex);
    CHECKF(held.air_ticks == 0, "...it is in the air for %zu updates; must be 0, error allowed 0", held.air_ticks);

    // the Cone of Cold with its talent, the Wolf 2 m from the Mage
    KbRun run_normal, run_boss;
    const KbPush pushed = kb_push(f, tables, wolf_path, 200.0, 0, run_normal, false);
    const KbPush spared = kb_push(f, tables, wolf_path, 200.0, 0, run_boss, true);
    const double dz = std::sqrt(std::max(0.0, pushed.distance * pushed.distance - 200.0 * 200.0));
    const double cone = kb_cone_reach(200.0, dz, body.mass, body.friction, body.braking, 0.5);
    std::printf("    the Cone of Cold at 2 m: a Wolf is pushed %.1f cm (the model %.1f), %zu updates say the displaced bit; a boss %.2f cm (farthest %.2f cm), %zu updates say it\n", pushed.reach, cone, pushed.pushed_updates, spared.reach,
                spared.peak_forward, spared.pushed_updates);
    CHECKF(pushed.ran && pushed.hit_ok && near(pushed.reach, cone, 0.015 * cone + 2.0), "a Wolf without the tag is pushed %.1f cm; must be %.1f cm (the model), error allowed %.1f cm (1.5 %% + 2 cm)", pushed.reach, cone, 0.015 * cone + 2.0);
    const double pushed_for = pushed.on_i < run_normal.s.size() && pushed.off_i < run_normal.s.size() ? run_normal.s[pushed.off_i].t - run_normal.s[pushed.on_i].t : -1.0;
    CHECKF(pushed.pushed_updates == 1 && near(pushed_for, 0.5, 0.07), "...its state says the displaced bit in %zu update (the one that carries the effect; must be 1, error allowed 0) and clears %.3f s later; must be 0.500 s (the effect's length), error allowed 0.07 s (one update of 67 ms)", pushed.pushed_updates, pushed_for);
    CHECK(spared.ran && spared.hit_ok, "the Cone of Cold lands on a Wolf with the Boss tag");
    CHECKF(near(spared.reach, 0.0, 0.5), "...it moves %.2f cm; must be 0.00 cm, error allowed 0.5 cm", spared.reach);
    CHECKF(near(spared.peak_forward, 0.0, 0.5), "...it goes %.2f cm along the Mage's facing at most; must be 0.00 cm, error allowed 0.5 cm", spared.peak_forward);
    CHECKF(spared.pushed_updates == 0, "...its state says the displaced bit in %zu updates; must be 0, error allowed 0 (none of the CC of the push is put on it)", spared.pushed_updates);

    // the Grapple, the Wolf 8 m from the Tank
    const KbPull pulled = kb_pull(f, tables, wolf_path, 800.0, false);
    const KbPull kept = kb_pull(f, tables, wolf_path, 800.0, true);
    const double want_y = 500.0 + 300.0 + f.motion.find(wolf_path)->radius + 34.0;
    std::printf("    the Grapple at 8 m: a Wolf is put at (%.2f, %.2f); a boss at (%.2f, %.2f), where it stood: (%.2f, %.2f)\n", pulled.put_x, pulled.put_y, kept.put_x, kept.put_y, kept.start_x, kept.start_y);
    CHECK(pulled.ran && pulled.hit_ok, "the Grapple lands on a Wolf without the tag");
    CHECKF(near(pulled.put_x, 500.0, 1.0), "...it is put at x = %.2f; must be 500.00, error allowed 1.0 cm", pulled.put_x);
    CHECKF(near(pulled.put_y, want_y, 1.0), "...at y = %.2f; must be %.2f (300 + its radius + the Tank's 34 in front of him), error allowed 1.0 cm", pulled.put_y, want_y);
    CHECK(kept.ran && kept.hit_ok, "the Grapple lands on a Wolf with the Boss tag");
    CHECKF(near(kept.put_x, kept.start_x, 0.5), "...it moves %.2f cm along x; must be 0.00 cm, error allowed 0.5 cm", kept.put_x - kept.start_x);
    CHECKF(near(kept.put_y, kept.start_y, 0.5), "...and %.2f cm along y; must be 0.00 cm, error allowed 0.5 cm", kept.put_y - kept.start_y);
}
AOC_REGISTER_TEST(test_kb_boss_tag_gate);

void test_kb_real_bosses_are_not_moved(Fixture& f) {
    std::printf("the real bosses of the Riverlands (6 Blueprints, 43 mobs), on their real rows of the table (nothing changed in them), are not thrown, pushed or pulled: each boss and each ability on its own\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    struct Boss {
        const char* leaf;
        const char* what;
    };
    static const Boss kBosses[] = {
        {"/GiantCyclops/CyclopsShaman.Default__CyclopsShaman_C", "Tumok the Wretched"},
        {"/NamedBoss/Dunir_Fighter_Forgelord_Zammer.Default__Dunir_Fighter_Forgelord_Zammer_C", "Forgelord Zammer"},
        {"/Giants/Giant_Base.Default__Giant_Base_C", "a giant"},
        {"/MagmaGolem/MagmaGolem_Bellowsmasher.Default__MagmaGolem_Bellowsmasher_C", "the Bellowsmasher"},
        {"/FlameElemental_TheUntendedFlame.Default__FlameElemental_TheUntendedFlame_C", "the Untended Flame"},
        {"/Warbeast_Child_Quest.Default__Warbeast_Child_Quest_C", "the quest warbeast"},
    };
    for (const Boss& b : kBosses) {
        const std::string path = creature_path(f, b.leaf);
        const auto bp = path.empty() ? std::optional<npc::MotionStats>{} : f.motion.find(path);
        CHECKF(bp.has_value() && kb_has_tag(f.motion, path, "Boss"), "%s: the real table has the Blueprint and its tags hold the Boss tag (\"%s\")", b.what, bp ? bp->tags.c_str() : "no row");
        if (!bp || !kb_has_tag(f.motion, path, "Boss")) continue;

        const KbFlight fl = kb_flight(f, tables, path, 200.0, 0.0, 0.0);
        std::printf("    %s, Get Off The Stage at 2 m: %s, moved %.2f cm, %.2f cm up, %zu updates in the air\n", b.what, fl.hit_ok ? "hit" : "no hit", fl.reach, fl.apex, fl.air_ticks);
        CHECKF(fl.ran && fl.hit_ok, "%s: Get Off The Stage lands on it", b.what);
        CHECKF(near(fl.reach, 0.0, 0.5), "...it moves %.2f cm; must be 0.00 cm, error allowed 0.5 cm", fl.reach);
        CHECKF(near(fl.apex, 0.0, 0.5), "...it goes up %.2f cm; must be 0.00 cm, error allowed 0.5 cm", fl.apex);
        CHECKF(fl.air_ticks == 0, "...it is in the air for %zu updates; must be 0, error allowed 0", fl.air_ticks);

        KbRun run;
        const KbPush p = kb_push(f, tables, path, 200.0, 0, run);
        std::printf("    %s, the Cone of Cold at 2 m: %s, moved %.2f cm, farthest %.2f cm along the Mage's facing, %zu updates say the displaced bit\n", b.what, p.hit_ok ? "hit" : "no hit", p.reach, p.peak_forward, p.pushed_updates);
        CHECKF(p.ran && p.hit_ok, "%s: the Cone of Cold lands on it", b.what);
        CHECKF(near(p.reach, 0.0, 0.5), "...it moves %.2f cm; must be 0.00 cm, error allowed 0.5 cm", p.reach);
        CHECKF(near(p.peak_forward, 0.0, 0.5), "...it goes %.2f cm along the Mage's facing at most; must be 0.00 cm, error allowed 0.5 cm", p.peak_forward);
        CHECKF(p.pushed_updates == 0, "...its state says the displaced bit in %zu updates; must be 0, error allowed 0 (none of the CC of the push is put on it)", p.pushed_updates);

        const KbPull pl = kb_pull(f, tables, path, 800.0, false);
        std::printf("    %s, the Grapple at 8 m: %s, put at (%.2f, %.2f), where it stood: (%.2f, %.2f)\n", b.what, pl.hit_ok ? "hit" : "no hit", pl.put_x, pl.put_y, pl.start_x, pl.start_y);
        CHECKF(pl.ran && pl.hit_ok, "%s: the Grapple lands on it", b.what);
        CHECKF(near(pl.put_x, pl.start_x, 0.5), "...it moves %.2f cm along x; must be 0.00 cm, error allowed 0.5 cm", pl.put_x - pl.start_x);
        CHECKF(near(pl.put_y, pl.start_y, 0.5), "...and %.2f cm along y; must be 0.00 cm, error allowed 0.5 cm", pl.put_y - pl.start_y);
    }
}
AOC_REGISTER_TEST(test_kb_real_bosses_are_not_moved);

// ---- the body constants of the Blueprint table ---------------------------------------------------------------------------------------------------------------------------
// The table's last three columns say what moves a creature's body when something other than its legs does: the ground friction, the braking deceleration and the mass; empty where no Blueprint
// sets them, and a table of 21 columns or fewer has none.

void test_kb_motion_table_body_columns(Fixture&) {
    std::printf("the Blueprint table reads the ground friction, braking and mass of a body (empty: the engine's defaults), and still reads the shorter tables\n");
    const std::string head = "archetype\tparents\tdefault_scale\tstarting_alignments\thalf_height\tradius\tmax_walk_speed\tmax_run_speed\tmax_acceleration\trotation_rate_yaw\tmove_mode\tfly_speed\ttags";
    const std::string visuals = "\tmesh\tanim\tplaceholder\thidden";
    const std::string combat = "\tlists\tdist\telite\trewards";
    const std::string body = "\tground_friction\tbraking_deceleration\tmass";
    const auto row = [](const char* name, const char* tail) {
        return std::string(name) + "\t\t1\t\t96\t96\t900\t1200\t8192\t720\tEMovementMode::MOVE_NavWalking\t0\t" + tail;
    };
    npc::MotionTable table;
    std::string err;
    const std::string text = head + visuals + combat + body + "\n" + row("/Game/A.Default__A_C", "\t\t\t0\t0\t1;2;3\t10,10,10,10,10,10,10,10\t\t\t24\t\t150") + "\n" + row("/Game/B.Default__B_C", "\t\t\t0\t0\t1;2;3\t10,10,10,10,10,10,10,10\t\t\t\t\t") + "\n";
    CHECKF(table.parse(text, err), "a table with the three columns reads: %s", err.c_str());
    const auto a = table.find("/Game/A.Default__A_C"), b = table.find("/Game/B.Default__B_C");
    CHECK(a && b, "both rows are there");
    if (a && b) {
        CHECKF(near(a->ground_friction, 24.0, 1e-9) && a->braking_deceleration < 0 && near(a->mass, 150.0, 1e-9), "a row that sets a friction and a mass has them and no braking (%.0f, %.0f, %.0f)", a->ground_friction, a->braking_deceleration,
               a->mass);
        CHECKF(b->ground_friction < 0 && b->braking_deceleration < 0 && b->mass < 0, "a row that sets none has none (%.0f, %.0f, %.0f)", b->ground_friction, b->braking_deceleration, b->mass);
    }
    npc::MotionTable shorter;
    const std::string old_text = head + visuals + combat + "\n" + row("/Game/C.Default__C_C", "\t\t\t0\t0\t1;2;3\t10,10,10,10,10,10,10,10\t\t") + "\n";
    CHECKF(shorter.parse(old_text, err), "a table of 21 columns still reads: %s", err.c_str());
    if (const auto c = shorter.find("/Game/C.Default__C_C")) CHECK(c->ground_friction < 0 && c->mass < 0, "...and has no body constants");
    npc::MotionTable bad;
    CHECK(!bad.parse(head + visuals + combat + body + "\n" + row("/Game/D.Default__D_C", "\t\t\t0\t0\t1;2;3\t10,10,10,10,10,10,10,10\t\t\tfast\t\t") + "\n", err), "a body constant that is no number is refused");
}
AOC_REGISTER_TEST(test_kb_motion_table_body_columns);
