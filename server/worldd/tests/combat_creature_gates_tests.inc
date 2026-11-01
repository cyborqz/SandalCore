// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_creature_gates_tests.inc
//
//  Registered tests of test_worldd_combat: a creature's fight with a standing player through the real tables: the movement the client sees, the gates of a cast, the hit areas and the strict reading of the tables.
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================

void test_grem_fight(Fixture& f) {
    std::printf("a Grem against a player who stands still: it lunges to the player, bites in reach, never goes through or drifts away, turns, and comes back when the player steps away\n");
    constexpr double kPi = 3.14159265358979323846;
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    std::string rabid_path;
    for (const auto& [path, row] : f.motion.rows()) {
        if (path.find("/Beast/Rabid/Rabid.Default__Rabid_C") != std::string::npos) rabid_path = path;
    }
    const auto bp = f.motion.find(rabid_path);
    CHECK(!rabid_path.empty() && bp && tables.abilities_of(rabid_path) != nullptr, "the Blueprint table has the Rabid and the creature tables list its abilities");
    if (rabid_path.empty() || !bp || !tables.abilities_of(rabid_path)) return;
    const abilities::AbilityDef* fury = f.catalog->ability_named("Rabid_Fury");
    const abilities::AbilityDef* snap = f.catalog->ability_named("Rabid_Snap");
    CHECK(fury && snap, "the ability data has the Grem's Fury and Snap");
    if (!fury || !snap) return;

    Rig rig(4, 20, {});
    if (!rig.ok()) return;
    combat_rig::World w;
    if (!w.problem.empty()) return;
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorldConfig cfg;
    cfg.seed = 5;
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion, cfg);
    world.set_ability_data(&tables);
    const uint64_t grem = w.spawn(rabid_path, 0, 0, 7, 900, bp->half_height);
    CHECK(grem != 0, "a Grem is made from the Rabid's capture");
    if (grem == 0) return;
    const double r_grem = bp->radius, r_player = 34.0, touch = r_grem + r_player;
    {
        // the animated creature attacks of the tables, by where their hits land (the line worldd logs when it loads them)
        const combat::CombatWorld::GateCensus census = world.gate_census();
        std::printf("  %s\n", census.text().c_str());
        const auto list = [](const char* what, const std::vector<std::string>& names) {
            std::string s;
            for (const std::string& n : names) s += (s.empty() ? "" : ", ") + n;
            std::printf("    %s: %s\n", what, s.c_str());
        };
        list("on the target wherever it stands", census.any_names);
        list("inside a cone", census.validity_names);
        list("by volumes", census.volume_names);
        list("keep the single swing", census.kept_names);
        list("no hit of the montage's to decide by", census.unplayed_names);
        CHECKF(census.any >= 1 && census.validity >= 1 && census.volumes >= 1,
               "the tables hold attacks of every kind of rule: %zu on the target wherever it stands, %zu inside a cone, %zu by volumes (%zu keep the swing)", census.any, census.validity, census.volumes, census.kept_swing);
    }
    world::WorldActor* actor = w.registry.find(grem);
    CHECK(actor && combat::npc_knows_ability(*actor, fury->row.guid) && combat::npc_knows_ability(*actor, snap->row.guid), "its open lists the Fury and the Snap, so the world plays their animations");
    double reach_cm = 0;   // what the tree lets it start an attack from, surface to surface
    for (const combat::CreatureAbility& t : *tables.abilities_of(rabid_path)) {
        if (!t.unknown && t.max_distance > reach_cm) reach_cm = t.max_distance;
    }
    CHECK(reach_cm > 0, "the tree gives its basic attacks a reach");

    double px = 0, py = 800;
    combat_rig::FakePlayer player(rig, 77, {px, py, 1098});
    world.add_player(&player);

    // the player stands for 20 s, steps 6 m away (and the Grem has to come back), and after 8 s more is put at the Grem's back (it has to turn)
    const double dt = 1.0 / 15.0, t_away = 20.0, t_behind = 28.0, t_end = 36.0;
    struct Pose {
        double t, x, y, yaw, px, py;
        int    mode;   // its ReplicatedMovementMode (-1: none)
    };
    const auto movement_mode = [&](const world::WorldActor& g) {
        const aoc::protocol::repl::ObjectState& obj = g.actor.obj;
        const aoc::protocol::repl::Value* v = obj.schema ? obj.state.get(*obj.schema, "ReplicatedMovementMode") : nullptr;
        return v && v->get_if<uint64_t>() ? static_cast<int>(*v->get_if<uint64_t>()) : -1;
    };
    const int mode_before = movement_mode(*w.registry.find(grem));
    std::vector<Pose> trace;
    std::vector<GremCast> casts;
    std::vector<double> moves;   // when the player was put somewhere else
    GremCastView last_view;
    // what the connection that has the Grem open is sent while it fights: the actor's own block of each update, read back (the properties that changed)
    struct Wire {
        double   t = 0;
        int      active = -1;              // RepRootMotion.bIsActive (-1: not in the block)
        double   position = -1;            // RepRootMotion.Position
        bool     has_animation = false;
        uint64_t animation = 0;            // RepRootMotion.Animation's ObjectId
        bool     has_rrm_loc = false, has_mv_loc = false;
        int64_t  rrm_loc[3] = {0, 0, 0}, mv_loc[3] = {0, 0, 0};
        bool     has_mv_rot = false, has_rrm_rot = false, has_rrm_vel = false;
        double   mv_yaw = 0, rrm_yaw = 0;   // the yaw as a client reads the wire integer: degrees, 8 or 16 bits as the block was written
        int64_t  mv_vel[2] = {0, 0};        // ReplicatedMovement's linear velocity, cm/s
        int64_t  rrm_vel[2] = {0, 0};       // RepRootMotion.LinearVelocity, tenths of cm/s
        int      rep_movement = -1;         // bReplicateMovement (-1: not in the block)
    };
    std::vector<Wire> wire;
    aoc::protocol::repl::ActorShadow shadow;
    // bReplicateMovement as the open leaves it on the client (-1: the open does not carry it, the class default holds)
    int open_rep_movement = -1;
    if (const auto* v = actor->actor.obj.state.get(*actor->actor.obj.schema, "bReplicateMovement")) {
        if (v->get_if<bool>()) open_rep_movement = *v->get_if<bool>() ? 1 : 0;
    }
    {
        aoc::protocol::repl::ExportAcks acks;
        bool failed = false;
        const auto open = aoc::protocol::repl::open_actor(actor->actor, shadow, acks, &failed);
        CHECK(open.has_value() && !failed, "the Grem's open builds (the shadow of the connection that watches the fight)");
    }
    double min_centre = 1e9, farthest_in_fight = 0, first_cast_at = -1;
    bool away = false, behind = false;
    double now = 0;
    while (now < t_end) {
        now += dt;
        if (!away && now >= t_away) {   // the player steps 6 m away, mid-fight
            away = true;
            py += 600;
            player.rig_fighter().move_to({px, py, 1098});
            moves.push_back(now);
        }
        if (!behind && now >= t_behind) {   // ... and is then at the Grem's back, 3 m from it
            behind = true;
            const world::WorldActor* g = w.registry.find(grem);
            px = g->x - 300.0 * std::cos(g->yaw_degrees * kPi / 180.0);
            py = g->y - 300.0 * std::sin(g->yaw_degrees * kPi / 180.0);
            player.rig_fighter().move_to({px, py, 1098});
            moves.push_back(now);
        }
        w.npcs->note_player(77, px, py, now);
        w.npcs->tick(now);
        const world::WorldActor* before_actor = w.registry.find(grem);
        const double bx = before_actor->x, by = before_actor->y, byaw = before_actor->yaw_degrees;
        world.tick(now);
        const world::WorldActor* g = w.registry.find(grem);
        trace.push_back({now, g->x, g->y, g->yaw_degrees, px, py, movement_mode(*g)});
        {
            std::vector<aoc::protocol::repl::ContentBlock> blocks;
            if (aoc::protocol::repl::collect_update_blocks(actor->actor, shadow, blocks)) {
                for (const aoc::protocol::repl::ContentBlock& b : blocks) {
                    if (b.sub || !b.has_rep_layout) continue;
                    const auto st = aoc::protocol::repl::load_object_state(*actor->actor.obj.schema, b.payload.data(), b.payload.bit_pos(), true);
                    if (!st) continue;
                    const aoc::protocol::schema::ClassSchema& sc = *actor->actor.obj.schema;
                    Wire ws;
                    ws.t = now;
                    if (const auto* v = st->state.get(sc, "RepRootMotion.bIsActive")) {
                        if (v->get_if<bool>()) ws.active = *v->get_if<bool>() ? 1 : 0;
                    }
                    if (const auto* v = st->state.get(sc, "RepRootMotion.Position")) {
                        if (v->get_if<float>()) ws.position = *v->get_if<float>();
                    }
                    if (const auto* v = st->state.get(sc, "RepRootMotion.Animation")) {
                        if (const auto* o = v->get_if<aoc::protocol::emit::FIntrepidNetworkGUID>()) {
                            ws.has_animation = true;
                            ws.animation = o->ObjectId;
                        }
                    }
                    if (const auto* v = st->state.get(sc, "RepRootMotion.Location")) {
                        if (const auto* q = v->get_if<aoc::protocol::repl::QuantVec>()) {
                            ws.has_rrm_loc = true;
                            ws.rrm_loc[0] = q->x, ws.rrm_loc[1] = q->y, ws.rrm_loc[2] = q->z;
                        }
                    }
                    if (const auto* v = st->state.get(sc, "ReplicatedMovement")) {
                        if (const auto* m = v->get_if<aoc::protocol::repl::RepMovement>()) {
                            ws.has_mv_loc = true;
                            ws.mv_loc[0] = m->location.x, ws.mv_loc[1] = m->location.y, ws.mv_loc[2] = m->location.z;
                            // the rotation as the client decodes it: FRotator::DecompressAxisFromByte (raw * 360 / 256) or ...FromShort (raw * 360 / 65536)
                            ws.has_mv_rot = true;
                            ws.mv_yaw = m->rotation.yaw * 360.0 / (st->opts.rep_movement_short_rotation ? 65536.0 : 256.0);
                            ws.mv_vel[0] = m->linear_velocity.x, ws.mv_vel[1] = m->linear_velocity.y;
                        }
                    }
                    if (const auto* v = st->state.get(sc, "RepRootMotion.Rotation")) {
                        if (const auto* r = v->get_if<aoc::protocol::repl::Rot>()) {
                            ws.has_rrm_rot = true;
                            ws.rrm_yaw = r->yaw * 360.0 / 65536.0;
                        }
                    }
                    if (const auto* v = st->state.get(sc, "RepRootMotion.LinearVelocity")) {
                        if (const auto* q = v->get_if<aoc::protocol::repl::QuantVec>()) {
                            ws.has_rrm_vel = true;
                            ws.rrm_vel[0] = q->x, ws.rrm_vel[1] = q->y;
                        }
                    }
                    if (const auto* v = st->state.get(sc, "bReplicateMovement")) {
                        if (v->get_if<bool>()) ws.rep_movement = *v->get_if<bool>() ? 1 : 0;
                    }
                    wire.push_back(ws);
                }
            }
        }
        const double centre = std::hypot(g->x - px, g->y - py);
        min_centre = std::min(min_centre, centre);
        if (first_cast_at >= 0 && now < t_away) farthest_in_fight = std::max(farthest_in_fight, centre);
        const GremCastView view = grem_cast_view(w.registry, grem);
        if (view.montage != 0 && (view.element != last_view.element || last_view.montage == 0)) {
            GremCast c;
            c.start = now;
            c.ability = view.ability;
            c.montage_id = view.montage;
            const abilities::AbilityDef* def = f.catalog->ability(view.ability);
            c.name = def ? def->row.name : "?";
            c.surface_before = std::hypot(bx - px, by - py) - r_grem - r_player;
            c.angle_before = std::fabs(wrap180(std::atan2(py - by, px - bx) * 180.0 / kPi - byaw));
            c.facing_error = std::fabs(wrap180(std::atan2(py - g->y, px - g->x) * 180.0 / kPi - g->yaw_degrees));
            c.centre_start = centre;
            if (def) {
                for (const content::MontageRow& m : def->montages) {
                    const combat::MontageData* md = m.soft_path.empty() ? nullptr : tables.montage(m.soft_path);
                    if (!md || md->guid != view.montage) continue;
                    c.duration = md->seconds(1.0);
                    for (const combat::MontageData::Hit& h : md->hits) {
                        for (const content::AbilityHitRow& r : def->hits) {
                            if (r.source != "hit" || r.key != h.key) continue;
                            ++c.bites;
                            c.notifies.push_back(h.time / md->rate_scale);
                            break;
                        }
                    }
                }
            }
            if (first_cast_at < 0) first_cast_at = now;
            casts.push_back(std::move(c));
        }
        last_view = view;
    }
    const stats::StatValue* hp = rig.cs->find(content::kGuidHealth);
    CHECK(hp && hp->value > 0, "the player is still alive at the end (the bites went on)");

    // what reached the player during each cast; how close the Grem was at the end of it and how far it went back from the closest it came
    for (GremCast& c : casts) {
        c.centre_end = c.centre_start;
        double closest = c.centre_start;
        for (const Pose& p : trace) {
            if (p.t < c.start) continue;
            const double d = std::hypot(p.x - p.px, p.y - p.py);
            if (p.t <= c.start + c.duration) {
                closest = std::min(closest, d);
                c.centre_end = d;
            }
            if (std::fabs(p.t - c.start) < 0.5 * dt) c.mode_at_start = p.mode;
            if (p.t >= c.start + c.duration - 0.1 && c.mode_after < 0) c.mode_after = p.mode;
        }
        c.closest = closest;
        c.stepped_back = c.centre_end - closest;
        for (const combat::IncomingHit& h : player.hits) {
            if (h.source_guid == c.ability && h.at >= c.start - 0.05 && h.at <= c.start + c.duration + 0.25) ++c.delivered;
        }
        c.settled = c.start + c.duration + 0.25 <= t_end;   // the player stood where it was for the whole cast, and the cast was over before the test was
        for (const double m : moves) c.settled = c.settled && !(m > c.start - 0.01 && m < c.start + c.duration + 0.25);
    }
    std::printf("  %zu casts in %.0f s (%llu attacks, %llu hits and %llu misses reached the player); the first cast at %.1f s\n", casts.size(), t_end,
                static_cast<unsigned long long>(world.stats().npc_attacks), static_cast<unsigned long long>(world.stats().npc_hits), static_cast<unsigned long long>(world.stats().npc_misses), first_cast_at);
    {
        size_t with_rrm = 0, starts = 0, ends = 0;
        for (const Wire& ws : wire) {
            if (ws.position >= 0) ++with_rrm;
            if (ws.active == 1) ++starts;
            if (ws.active == 0) ++ends;
        }
        std::printf("  the Grem's own block was sent %zu times; %zu carried RepRootMotion's position, %zu said a montage carries the Grem, %zu said it let go\n", wire.size(), with_rrm, starts, ends);
    }
    for (size_t i = 0; i < casts.size() && i < 60; ++i) {
        const GremCast& c = casts[i];
        std::printf("    t %5.2f  %-11s  before: surface %6.1f cm, %5.1f deg off   facing error %5.1f deg   centre %6.1f -> %6.1f cm (back %4.1f)   bites %d of %d%s\n", c.start, c.name.c_str(),
                    c.surface_before, c.angle_before, c.facing_error, c.centre_start, c.centre_end, c.stepped_back, c.delivered, c.bites, c.settled ? "" : "   (the player moved during it)");
    }

    // ---- the checks ----
    size_t before_away = 0, after_away = 0, after_behind = 0;
    for (const GremCast& c : casts) {
        if (c.start + c.duration <= t_away) ++before_away;
        if (c.start >= t_away) ++after_away;
        if (c.start >= t_behind) ++after_behind;
    }
    CHECKF(before_away >= 6, "the Grem attacks a player who stands in front of it again and again (%zu casts in the first %.0f s)", before_away, t_away);
    // it never comes closer than the two bodies touch
    CHECKF(min_centre >= touch - 1.0, "its centre never comes closer to the player's than the two radii (%.1f cm), the closest was %.1f cm", touch, min_centre);
    // it does not drift: once it fights, it stays within the reach of the tree's attacks
    const double bound = touch + reach_cm * 1.15 + 1.0;
    CHECKF(first_cast_at >= 0 && farthest_in_fight <= bound, "while the player stands it stays within %.0f cm (the radii and the tree's reach with the AI's slack) of the player, the farthest was %.1f cm", bound, farthest_in_fight);
    // it does not step back from the body it has reached when the animation winds back
    size_t backed = 0;
    double most_back = 0;
    for (const GremCast& c : casts) {
        if (!c.settled || c.closest > touch + 1.0) continue;   // (a lunge that found room goes the animation's whole way, back included)
        if (c.stepped_back > 1.5) ++backed;
        most_back = std::max(most_back, c.stepped_back);
    }
    CHECKF(backed == 0, "once it is at the player's body it does not step back when the animation winds back (%zu casts did, the most %.1f cm)", backed, most_back);
    // while a montage carries the Grem it walks (the movement mode the client moves it by the montage's root motion in), and afterwards it is as it was
    size_t wrong_mode = 0;
    for (const GremCast& c : casts) {
        if (c.start + c.duration > t_end) continue;   // (the last cast was cut off by the end of the test)
        if (c.mode_at_start != aoc::protocol::repl::kMoveWalking || c.mode_after != mode_before) ++wrong_mode;
    }
    CHECKF(wrong_mode == 0, "its movement mode is Walking while a montage carries it and what it was (%d) when the montage is over (%zu of %zu casts differ)", mode_before, wrong_mode, casts.size());
    // the yaw of the Grem as the client reads it from the wire is the bearing to its target while it stands and bites (retail's relation, 10 recorded casts: the bearing less the yaw is 1.8 deg
    // median at the cast's first update, 8-bit and 16-bit alike, a mirrored or flipped reading 83 and 178 deg off)
    {
        size_t samples = 0, off8 = 0, off16 = 0;
        double worst8 = 0, worst16 = 0;
        for (const GremCast& c : casts) {
            // the yaw the client holds when the cast begins: the last it was sent (the rotation is sent when it changes)
            double yaw8 = 0, yaw16 = 0;
            bool have8 = false, have16 = false;
            for (const Wire& ws : wire) {
                if (ws.t > c.start + 1e-6) break;
                if (ws.has_mv_rot) yaw8 = ws.mv_yaw, have8 = true;
                if (ws.has_rrm_rot) yaw16 = ws.rrm_yaw, have16 = true;
            }
            const Pose* at = nullptr;
            for (const Pose& p : trace) {
                if (std::fabs(p.t - c.start) < 1e-6) at = &p;
            }
            if (!at || !have8) continue;
            const double bearing = at->yaw;   // the facing the server has when the cast begins (the Grem turns to its target at its rate, so this is within a couple of degrees of the bearing: the check above)
            const double d8 = std::fabs(wrap180(bearing - yaw8)), d16 = have16 ? std::fabs(wrap180(bearing - yaw16)) : 0.0;
            ++samples;
            worst8 = std::max(worst8, d8);
            worst16 = std::max(worst16, d16);
            if (d8 > 0.75 * 360.0 / 256.0) ++off8;
            if (have16 && d16 > 0.75 * 360.0 / 65536.0 + 0.01) ++off16;
        }
        std::printf("  the wire's yaw against the server's, as the client holds it at the start of each cast (%zu): ReplicatedMovement (8 bits) at most %.2f deg off, RepRootMotion.Rotation (16 bits) at most %.3f deg off\n",
                    samples, worst8, worst16);
        CHECKF(samples >= 8 && off8 == 0 && off16 == 0, "the Grem's yaw on the wire, read as the client reads it, is the yaw the server bites with: %zu casts sampled, %zu off in ReplicatedMovement, %zu in RepRootMotion.Rotation",
               samples, off8, off16);
    }
    // ... and a Grem that walks up to the player faces the way it walks: the same encoding, the same reading (a walking creature never needed the lunge's rotation)
    {
        size_t walking = 0, wrong = 0;
        double worst = 0;
        for (const Wire& ws : wire) {
            if (ws.t > first_cast_at || ws.t < 0.55 || !ws.has_mv_rot) continue;
            if (std::hypot(static_cast<double>(ws.mv_vel[0]), static_cast<double>(ws.mv_vel[1])) < 200.0) continue;
            const double heading = std::atan2(static_cast<double>(ws.mv_vel[1]), static_cast<double>(ws.mv_vel[0])) * 180.0 / kPi;
            const double d = std::fabs(wrap180(heading - ws.mv_yaw));
            ++walking;
            worst = std::max(worst, d);
            if (d > 10.0) ++wrong;
        }
        CHECKF(walking >= 3 && wrong == 0, "a Grem that walks up to the player faces the way it walks, on the wire (%zu updates sampled, the worst %.1f deg off)", walking, worst);
    }
    // RepRootMotion.LinearVelocity is the Grem's velocity (what the recorded updates carry in every one of a lunge: ReplicatedMovement's velocity in tenths of cm/s)
    {
        size_t lunging = 0, missing = 0;
        for (const Wire& ws : wire) {
            if (ws.position < 0) continue;
            ++lunging;
            if (!ws.has_rrm_vel && std::hypot(static_cast<double>(ws.mv_vel[0]), static_cast<double>(ws.mv_vel[1])) > 1.0) ++missing;
        }
        CHECKF(missing == 0, "every update of a lunge that moves the Grem carries RepRootMotion.LinearVelocity (%zu of %zu do not)", missing, lunging);
    }
    // Fury: the lunge stops for good once the player is in the box ahead of it (the ability record: StopOnContact, FreezeDistance, FreezeRadiusScale), at a centre distance of 235 to 274 cm
    // in the recorded casts; one that begins inside the box does not move at all
    if (const combat::RootMotionFreezeRule* rule = tables.freeze_of(fury->row.guid)) {
        const double edge = rule->distance + rule->radius_scale * r_grem + r_player;
        size_t outside = 0, inside = 0, stopped_wrong = 0, moved_inside = 0;
        double lowest = 1e9, highest = 0;
        for (const GremCast& c : casts) {
            if (c.ability != fury->row.guid || !c.settled) continue;
            if (c.centre_start > edge) {
                ++outside;
                lowest = std::min(lowest, c.centre_end);
                highest = std::max(highest, c.centre_end);
                if (c.centre_end < 235.0 || c.centre_end > 274.0) ++stopped_wrong;
            } else {
                ++inside;
                if (c.centre_start - c.closest > 2.0) ++moved_inside;
            }
        }
        std::printf("  the Fury: its freeze box reaches %.0f cm ahead of the Grem's centre (%.0f + %.0f x %.0f + %.0f); %zu began outside it and stopped at %.1f to %.1f cm, %zu began inside it\n", edge,
                    rule->distance, rule->radius_scale, r_grem, r_player, outside, outside ? lowest : 0.0, highest, inside);
        CHECKF(rule->kind == combat::RootMotionFreezeRule::Kind::Stop && outside >= 1, "a Fury begins outside its freeze box, as the Grem walks up to the player (%zu did)", outside);
        CHECKF(stopped_wrong == 0, "a Fury that begins outside its freeze box stops at a centre distance of 235 to 274 cm from a player who stands (%zu of %zu did not, %.1f to %.1f cm)", stopped_wrong, outside,
               outside ? lowest : 0.0, highest);
        CHECKF(moved_inside == 0, "a Fury that begins inside its freeze box does not move (%zu of %zu did)", moved_inside, inside);
    }
    // what the client is sent while a montage carries the Grem: the montage and that it carries the Grem in the cast's first update, the montage's position and the Grem's place in every one, and its
    // end in the last; the place in RepRootMotion is the place in ReplicatedMovement
    size_t no_start = 0, no_end = 0, thin = 0, places_differ = 0, complete_casts = 0;
    for (const GremCast& c : casts) {
        if (c.start + c.duration > t_end) continue;
        ++complete_casts;
        bool started = false, ended = false;
        size_t positions = 0;
        for (const Wire& ws : wire) {
            if (ws.t < c.start - 1e-6 || ws.t > c.start + c.duration + 1e-6) continue;
            if (ws.active == 1 && ws.has_animation && ws.animation == c.montage_id && ws.t <= c.start + 1e-6) started = true;
            if (ws.active == 0 && ws.has_animation && ws.animation == 0) ended = true;
            if (ws.position >= 0) ++positions;
            if (ws.has_rrm_loc && ws.has_mv_loc && (ws.rrm_loc[0] != ws.mv_loc[0] || ws.rrm_loc[1] != ws.mv_loc[1] || ws.rrm_loc[2] != ws.mv_loc[2])) ++places_differ;
        }
        if (!started) ++no_start;
        if (!ended) ++no_end;
        if (positions < 6) ++thin;
    }
    CHECKF(no_start == 0 && no_end == 0 && thin == 0 && places_differ == 0,
           "the updates of a lunge carry RepRootMotion: its first names the montage and says it carries the Grem (%zu of %zu casts lack it), every one the montage's position (%zu casts have fewer than 6), the last "
           "its end (%zu lack it), and the place is ReplicatedMovement's (%zu updates differ)",
           no_start, complete_casts, thin, no_end, places_differ);
    // when a montage lets go of the Grem the client's copy, which the montage's root motion moved, is put back where the server has it: the retail server sends ReplicatedMovement again after the end of a
    // Snap or a Fury (the same place, no velocity), the first within 0.03 to 0.2 s of the end and then at every update while the creature stands; so does this server, for a few updates
    {
        size_t ends = 0, unsent = 0, wrong_place = 0, moving = 0, thin_tail = 0;
        double slowest = 0;
        for (const GremCast& c : casts) {
            if (!c.settled || c.start + c.duration > t_end) continue;
            const Wire* end = nullptr;
            for (const Wire& ws : wire) {
                if (ws.t < c.start - 1e-6 || ws.t > c.start + c.duration + 1e-6) continue;
                if (ws.active == 0 && ws.has_animation && ws.animation == 0) {
                    end = &ws;
                    break;
                }
            }
            if (!end) continue;
            ++ends;
            const Wire* again = nullptr;
            size_t tail = 0;
            for (const Wire& ws : wire) {
                if (ws.t <= end->t + 1e-6 || ws.t > end->t + 0.35 || !ws.has_mv_loc) continue;
                if (!again) again = &ws;
                ++tail;
            }
            if (tail < 3) ++thin_tail;
            if (!again || again->t - end->t > 0.25 + 1e-6) {
                ++unsent;
                continue;
            }
            slowest = std::max(slowest, again->t - end->t);
            const Pose* at = nullptr;
            for (const Pose& p : trace) {
                if (std::fabs(p.t - again->t) < 1e-6) at = &p;
            }
            if (!at || std::fabs(static_cast<double>(again->mv_loc[0]) - at->x * 100.0) > 100.0 || std::fabs(static_cast<double>(again->mv_loc[1]) - at->y * 100.0) > 100.0) ++wrong_place;
            if (again->mv_vel[0] != 0 || again->mv_vel[1] != 0) ++moving;
        }
        std::printf("  after a montage lets go of the Grem (%zu ends), ReplicatedMovement goes out again within %.3f s at the longest\n", ends, slowest);
        CHECKF(ends >= 6 && unsent == 0, "after every cast's end a ReplicatedMovement goes out within 0.25 s (%zu of %zu ends have none)", unsent, ends);
        CHECKF(wrong_place == 0 && moving == 0, "... with the Grem's place and no velocity (%zu with another place, %zu with a velocity)", wrong_place, moving);
        CHECKF(thin_tail == 0, "... and it goes on at the following updates for a few more (%zu of %zu ends are followed by fewer than 3 in 0.35 s)", thin_tail, ends);
    }
    // the client queues a lunge's RepRootMotion, and applies a ReplicatedMovement, only while the actor replicates its movement: from the first cast on, every update
    // that says where the Grem is must reach a client that holds bReplicateMovement on (the open's value, then each update's, applied before the update's notifies run)
    {
        int held = open_rep_movement;
        size_t lunge = 0, lunge_off = 0, place = 0, place_off = 0;
        for (const Wire& ws : wire) {
            if (ws.rep_movement >= 0) held = ws.rep_movement;
            if (first_cast_at < 0 || ws.t < first_cast_at - 1e-6) continue;
            if (ws.active == 1) {
                ++lunge;
                if (held != 1) ++lunge_off;
            }
            if (ws.has_mv_loc) {
                ++place;
                if (held != 1) ++place_off;
            }
        }
        std::printf("  bReplicateMovement: %d in the open (-1: not in it), %d at the end of the fight\n", open_rep_movement, held);
        CHECKF(lunge > 0 && lunge_off == 0, "every RepRootMotion update of a lunge reaches a client that replicates the Grem's movement (%zu of %zu do not)", lunge_off, lunge);
        CHECKF(place_off == 0, "... and so does every ReplicatedMovement from the first cast on (%zu of %zu do not)", place_off, place);
    }
    // every cast turns to the player before it begins
    size_t unfaced = 0;
    double worst_facing = 0;
    for (const GremCast& c : casts) {
        if (c.facing_error > 3.0) ++unfaced;
        worst_facing = std::max(worst_facing, c.facing_error);
    }
    CHECKF(unfaced == 0, "every cast faces the player when it begins (%zu of %zu do not, the worst %.1f deg off)", unfaced, casts.size(), worst_facing);
    // the tree's conditions hold when a cast begins: the distance and the angle of the task that uses the ability
    size_t outside = 0;
    double first_outside_at = -1, first_surface = 0, first_angle = 0;
    for (const GremCast& c : casts) {
        bool ok = false;
        for (const combat::CreatureAbility& t : *tables.abilities_of(rabid_path)) {
            if (t.unknown || t.ability != c.ability) continue;
            const bool d_ok = (t.max_distance < 0 || c.surface_before <= t.max_distance + 0.5) && (t.min_distance < 0 || c.surface_before >= t.min_distance - 0.5);
            const bool a_ok = t.max_angle < 0 || c.angle_before <= t.max_angle + 0.5;
            ok = ok || (d_ok && a_ok);
        }
        if (!ok && outside++ == 0) {
            first_outside_at = c.start;
            first_surface = c.surface_before;
            first_angle = c.angle_before;
        }
    }
    CHECKF(outside == 0, "no cast begins while the tree's conditions fail (%zu of %zu did; the first at %.2f s, %.1f cm from the player's surface and %.1f deg off its facing)", outside, casts.size(), first_outside_at,
           first_surface, first_angle);
    // every bite of a cast that began and ended while the player stood where he was reaches the player
    size_t short_casts = 0;
    int missing_bites = 0;
    double first_short_at = -1;
    for (const GremCast& c : casts) {
        if (!c.settled) continue;
        if (c.delivered < c.bites) {
            if (short_casts++ == 0) first_short_at = c.start;
            missing_bites += c.bites - c.delivered;
        }
    }
    CHECKF(short_casts == 0, "every bite of every cast lands while the player stands in its area (%zu casts lost %d bites; the first at %.2f s)", short_casts, missing_bites, first_short_at);
    // the way back: the player steps 6 m away, the Grem walks up to it again and bites
    double t_back = -1;
    for (const Pose& p : trace) {
        if (p.t < t_away || p.t >= t_behind) continue;
        if (std::hypot(p.x - p.px, p.y - p.py) <= touch + reach_cm) {
            t_back = p.t;
            break;
        }
    }
    CHECKF(t_back >= 0 && t_back - t_away <= 7.0, "when the player steps 6 m away the Grem is back within its reach in time (%.1f s after the step)", t_back < 0 ? -1.0 : t_back - t_away);
    int bites_after = 0;
    size_t casts_between = 0;
    for (const GremCast& c : casts) {
        if (c.start >= t_away && c.start < t_behind) {
            ++casts_between;
            bites_after += c.delivered;
        }
    }
    CHECKF(casts_between >= 1 && bites_after >= 1, "and it lands bites again (%zu casts and %d bites between the step and the next)", casts_between, bites_after);
    // at its back, it turns and bites
    int bites_behind = 0;
    for (const GremCast& c : casts) {
        if (c.start >= t_behind) bites_behind += c.delivered;
    }
    CHECKF(after_behind >= 1 && bites_behind >= 1, "when the player is at its back it turns and bites (%zu casts and %d bites after it)", after_behind, bites_behind);
}

AOC_REGISTER_TEST(test_grem_fight);

void test_creature_movement_reaches_client(Fixture& f) {
    std::printf("a creature whose open has the client ignore its movement: it walks, plays an in-place attack and turns, and every one of those updates reaches a client that applies them\n");
    constexpr double kPi = 3.14159265358979323846;
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    // an Alpha Wolf that is hurt: its Growl plays in place, and it bites with a short lunge
    std::string alpha_path;
    for (const auto& [path, row] : f.motion.rows()) {
        if (path.find("/Beast/Canine/AlphaWolf.Default__AlphaWolf_C") != std::string::npos) alpha_path = path;
    }
    const auto bp = f.motion.find(alpha_path);
    CHECK(!alpha_path.empty() && bp && tables.abilities_of(alpha_path) != nullptr, "the Blueprint table has the Alpha Wolf and the creature tables list its abilities");
    if (alpha_path.empty() || !bp || !tables.abilities_of(alpha_path)) return;
    const abilities::AbilityDef* growl = f.catalog->ability_named("AlphaWolf_Growl");
    CHECK(growl != nullptr, "the ability data has the Growl");
    if (!growl) return;

    Rig rig(4, 20, {});
    if (!rig.ok()) return;
    combat_rig::World w;
    if (!w.problem.empty()) return;
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorldConfig cfg;
    cfg.seed = 5;
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion, cfg);
    world.set_ability_data(&tables);
    const uint64_t wolf = w.spawn(alpha_path, 0, 0, 7, 900, bp->half_height);
    CHECK(wolf != 0, "an Alpha Wolf is made from its open");
    if (wolf == 0) return;
    world::WorldActor* actor = w.registry.find(wolf);
    CHECK(combat::npc_knows_ability(*actor, growl->row.guid), "its open lists the Growl, so the world plays its animation");
    const aoc::protocol::schema::ClassSchema& schema = *actor->actor.obj.schema;
    CHECK(schema.command("bReplicateMovement") != nullptr, "its actor has the bReplicateMovement property");
    // an open the client was sent with the property off: the client ignores every place until the world says otherwise
    actor->actor.obj.state.set(schema, "bReplicateMovement", aoc::protocol::repl::Value::boolean(false));
    combat::Fighter* fighter = world.fighter_of(wolf);
    CHECK(fighter != nullptr, "the world makes it a fighter");
    if (!fighter) return;

    double px = 0, py = 800;
    combat_rig::FakePlayer player(rig, 77, {px, py, 1098});
    world.add_player(&player);
    aoc::protocol::repl::ActorShadow shadow;
    {
        aoc::protocol::repl::ExportAcks acks;
        bool failed = false;
        const auto open = aoc::protocol::repl::open_actor(actor->actor, shadow, acks, &failed);
        CHECK(open.has_value() && !failed, "the Alpha Wolf's open builds (the shadow of the connection that watches the fight)");
    }
    const double dt = 1.0 / 15.0, t_behind = 12.0, t_end = 22.0;
    std::vector<MoveWire> wire;
    struct Cast {
        double  start = 0;
        bool    in_place = false;
        int     held = -1;   // what the client held when it began
    };
    std::vector<Cast> casts;
    GremCastView last_view;
    int held = 0;   // bReplicateMovement as the client holds it: the open's value, then each update's
    bool behind = false;
    double behind_at = 0, yaw_before_turn = 0;
    const stats::StatValue* wolf_max = fighter->stats().find(content::kGuidMaxHealth);
    double now = 0;
    while (now < t_end) {
        now += dt;
        if (!behind && now >= t_behind) {   // the player is put at its back: it has to turn
            behind = true;
            behind_at = now;
            const world::WorldActor* g = w.registry.find(wolf);
            yaw_before_turn = g->yaw_degrees;
            px = g->x - 300.0 * std::cos(g->yaw_degrees * kPi / 180.0);
            py = g->y - 300.0 * std::sin(g->yaw_degrees * kPi / 180.0);
            player.rig_fighter().move_to({px, py, 1098});
        }
        if (const stats::StatValue* max_hp = rig.cs->find(content::kGuidMaxHealth)) rig.cs->set_current(content::kGuidHealth, max_hp->value);   // (the fight is about the creature's updates, not the player's life; asked afresh: a hit rebuilds the stats)
        if (wolf_max) fighter->stats().set_current(content::kGuidHealth, static_cast<float>(wolf_max->value * 0.6));   // (hurt: its Growl is open to it)
        w.npcs->note_player(77, px, py, now);
        w.npcs->tick(now);
        world.tick(now);
        const size_t first_new = wire.size();
        read_move_wire(*actor, shadow, now, wire);
        for (size_t i = first_new; i < wire.size(); ++i) {
            if (wire[i].rep_movement >= 0) held = wire[i].rep_movement;
            wire[i].rep_movement = held;   // (from here on: what the client holds once it has applied the update)
        }
        const GremCastView view = grem_cast_view(w.registry, wolf);
        if (view.montage != 0 && (view.element != last_view.element || last_view.montage == 0)) {
            Cast c;
            c.start = now;
            c.held = held;
            const abilities::AbilityDef* def = f.catalog->ability(view.ability);
            if (def) {
                for (const content::MontageRow& m : def->montages) {
                    const combat::MontageData* md = m.soft_path.empty() ? nullptr : tables.montage(m.soft_path);
                    if (md && md->guid == view.montage) c.in_place = md->root_motion.empty();
                }
            }
            casts.push_back(c);
        }
        last_view = view;
    }
    size_t places = 0, places_ignored = 0, lunges = 0, lunges_ignored = 0, turns = 0, turns_ignored = 0;
    for (const MoveWire& m : wire) {
        if (m.place) {
            ++places;
            if (m.rep_movement != 1) ++places_ignored;
            if (behind && m.t >= behind_at && std::fabs(m.yaw - yaw_before_turn) > 45.0 && std::fabs(m.yaw - yaw_before_turn) < 315.0) {
                ++turns;
                if (m.rep_movement != 1) ++turns_ignored;
            }
        }
        if (m.lunge) {
            ++lunges;
            if (m.rep_movement != 1) ++lunges_ignored;
        }
    }
    size_t in_place = 0, in_place_ignored = 0;
    for (const Cast& c : casts) {
        if (!c.in_place) continue;
        ++in_place;
        if (c.held != 1) ++in_place_ignored;
    }
    std::printf("  %zu casts in %.0f s, %zu of them in place; %zu place updates, %zu with a montage carrying the creature, %zu that turn it\n", casts.size(), t_end, in_place, places, lunges, turns);
    CHECKF(places >= 20, "the creature's places reach the client while it walks, fights and turns (%zu updates)", places);
    CHECKF(places_ignored == 0, "every place update reaches a client that applies it: the bit is on from the first one (%zu of %zu would be ignored)", places_ignored, places);
    CHECKF(in_place >= 1 && in_place_ignored == 0, "an attack that plays in place begins with the bit on (%zu such casts, %zu begin with it off)", in_place, in_place_ignored);
    CHECKF(turns >= 1 && turns_ignored == 0, "its turn to the player at its back reaches a client that applies it (%zu such updates, %zu ignored)", turns, turns_ignored);
    CHECKF(lunges_ignored == 0, "and so does every update of a lunge (%zu of %zu ignored)", lunges_ignored, lunges);
}

AOC_REGISTER_TEST(test_creature_movement_reaches_client);

// ---- a creature whose tree gates an attack on a tree parameter and on its own health ----------------------------------------------------------------------------------------

void test_creature_parameter_and_health_gates(Fixture& f) {
    std::printf("the Alpha Wolf's Growl is gated by a tree parameter (the plain Wolf's is switched off) and by its own health (under 75 %%): the same fight, a full-health Alpha, a hurt Alpha and a hurt Wolf\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const auto path_of = [&](const char* leaf) {
        std::string found;
        for (const auto& [path, row] : f.motion.rows()) {
            if (path.find(leaf) != std::string::npos) found = path;
        }
        return found;
    };
    const std::string alpha_path = path_of("/Beast/Canine/AlphaWolf.Default__AlphaWolf_C"), wolf_path = path_of("/Beast/Canine/Wolf.Default__Wolf_C");
    CHECK(!alpha_path.empty() && !wolf_path.empty() && tables.abilities_of(alpha_path) && tables.abilities_of(wolf_path), "the Blueprint table has the Alpha Wolf and the Wolf and the creature tables list their abilities");
    if (alpha_path.empty() || wolf_path.empty() || !tables.abilities_of(alpha_path) || !tables.abilities_of(wolf_path)) return;
    const abilities::AbilityDef* growl = f.catalog->ability_named("AlphaWolf_Growl");
    const abilities::AbilityDef* bite = f.catalog->ability_named("Wolf_Bite");
    CHECK(growl && bite, "the ability data has the Growl and the Bite");
    if (!growl || !bite) return;
    // what the tables say about the Growl of each: switched off for the Wolf, usable (while it is hurt) for the Alpha
    const auto growl_task = [&](const std::string& path) -> const combat::CreatureAbility* {
        for (const combat::CreatureAbility& t : *tables.abilities_of(path)) {
            if (t.ability == growl->row.guid) return &t;
        }
        return nullptr;
    };
    const combat::CreatureAbility* alpha_task = growl_task(alpha_path);
    const combat::CreatureAbility* wolf_task = growl_task(wolf_path);
    CHECK(alpha_task && wolf_task, "both Blueprints' trees have the Growl");
    if (!alpha_task || !wolf_task) return;
    CHECKF(!alpha_task->off && !alpha_task->unknown && alpha_task->fight && !alpha_task->groups.empty(), "the Alpha Wolf's Growl is usable in the fight and carries a condition on its health (off %d, unknown %d, groups %zu)",
           alpha_task->off ? 1 : 0, alpha_task->unknown ? 1 : 0, alpha_task->groups.size());
    CHECK(wolf_task->off, "the plain Wolf's Growl is switched off by its tree's parameters");
    {
        // the load log's line for each: what a fight can use and what is left out
        std::string alpha_line, wolf_line;
        for (const std::string& line : tables.coverage_lines()) {
            if (line.rfind("AlphaWolf: ", 0) == 0) alpha_line = line;
            if (line.rfind("Wolf: ", 0) == 0) wolf_line = line;
        }
        std::printf("  load log: %s\n            %s\n", alpha_line.c_str(), wolf_line.c_str());
        const auto alpha_cover = tables.coverage().at(alpha_path), wolf_cover = tables.coverage().at(wolf_path);
        CHECKF(alpha_cover.usable > wolf_cover.usable && wolf_cover.off > alpha_cover.off && !alpha_line.empty() && !wolf_line.empty(),
               "the log counts the Alpha Wolf's usable tasks (%zu) above the Wolf's (%zu), and the Wolf's switched-off ones (%zu) above the Alpha's (%zu)", alpha_cover.usable, wolf_cover.usable, wolf_cover.off, alpha_cover.off);
    }

    const auto fight = [&](const std::string& path, double hurt_to, double seconds, bool needs_growl, int& growls, int& bites) {
        Rig rig(4, 20, {});
        if (!rig.ok()) return false;
        combat_rig::World w;
        if (!w.problem.empty()) return false;
        combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
        combat::CombatWorldConfig cfg;
        cfg.seed = 5;
        combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion, cfg);
        world.set_ability_data(&tables);
        const auto bp = f.motion.find(path);
        if (!bp) return false;
        const uint64_t wolf = w.spawn(path, 0, 0, 7, 900, bp->half_height);
        if (wolf == 0) return false;
        world::WorldActor* actor = w.registry.find(wolf);
        if ((needs_growl && !combat::npc_knows_ability(*actor, growl->row.guid)) || !combat::npc_knows_ability(*actor, bite->row.guid)) return false;
        combat_rig::FakePlayer player(rig, 77, {0, 800, 1098});
        world.add_player(&player);
        combat::Fighter* fighter = world.fighter_of(wolf);
        if (!fighter) return false;
        const double dt = 1.0 / 15.0;
        GremCastView last;
        double now = 0;
        bool hurt = false;
        while (now < seconds) {
            now += dt;
            if (!hurt && hurt_to < 1.0 && now >= 2.0) {   // the fight is on: the creature is hurt to a fraction of its health
                const stats::StatValue* hp = fighter->stats().find(content::kGuidMaxHealth);
                if (hp) fighter->stats().set_current(content::kGuidHealth, static_cast<float>(hp->value * hurt_to));
                hurt = true;
            }
            if (const stats::StatValue* max_hp = rig.cs->find(content::kGuidMaxHealth)) rig.cs->set_current(content::kGuidHealth, max_hp->value);
            w.npcs->note_player(77, 0, 800, now);
            w.npcs->tick(now);
            world.tick(now);
            const GremCastView view = grem_cast_view(w.registry, wolf);
            if (view.montage != 0 && (view.element != last.element || last.montage == 0)) {
                if (view.ability == growl->row.guid) ++growls;
                if (view.ability == bite->row.guid) ++bites;
            }
            last = view;
        }
        return true;
    };
    int full_growls = 0, full_bites = 0, hurt_growls = 0, hurt_bites = 0, wolf_growls = 0, wolf_bites = 0;
    const bool a = fight(alpha_path, 1.0, 16.0, true, full_growls, full_bites);
    const bool b = fight(alpha_path, 0.6, 16.0, true, hurt_growls, hurt_bites);
    const bool c = fight(wolf_path, 0.6, 16.0, false, wolf_growls, wolf_bites);
    CHECK(a && b && c, "the three fights run (the creatures are made from their opens and know the Bite, the Alpha the Growl too)");
    if (!(a && b && c)) return;
    std::printf("  Alpha Wolf at full health: %d bites, %d growls; hurt to 60 %%: %d bites, %d growls; plain Wolf hurt to 60 %%: %d bites, %d growls\n", full_bites, full_growls, hurt_bites, hurt_growls, wolf_bites, wolf_growls);
    CHECKF(full_bites >= 3 && full_growls == 0, "the Alpha Wolf at full health bites and never growls (%d bites, %d growls)", full_bites, full_growls);
    CHECKF(hurt_growls >= 1 && hurt_bites >= 1, "hurt under 75 %% it growls (%d growls, %d bites)", hurt_growls, hurt_bites);
    CHECKF(wolf_bites >= 3 && wolf_growls == 0, "the plain Wolf, hurt as much, still never growls: its tree switches the Growl off (%d bites, %d growls)", wolf_bites, wolf_growls);
}

AOC_REGISTER_TEST(test_creature_parameter_and_health_gates);

// ---- a creature whose tree rolls for an attack ---------------------------------------------------------------------------------------------------------------------------------

void test_creature_roll_gate(Fixture& f) {
    std::printf("the Bear's right swing is gated by a roll of one half, asked afresh at every try: over a long fight it swings right and left, neither always\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    std::string bear_path;
    for (const auto& [path, row] : f.motion.rows()) {
        if (path.find("/Beast/Bear/Bear.Default__Bear_C") != std::string::npos) bear_path = path;
    }
    const auto bp = f.motion.find(bear_path);
    CHECK(!bear_path.empty() && bp && tables.abilities_of(bear_path) != nullptr, "the Blueprint table has the Bear and the creature tables list its abilities");
    if (bear_path.empty() || !bp || !tables.abilities_of(bear_path)) return;
    const abilities::AbilityDef* right = f.catalog->ability_named("Bear_RightSwing");
    const abilities::AbilityDef* left = f.catalog->ability_named("Bear_LeftSwing");
    CHECK(right && left, "the ability data has the Bear's swings");
    if (!right || !left) return;
    const combat::CreatureAbility* right_task = nullptr;
    for (const combat::CreatureAbility& t : *tables.abilities_of(bear_path)) {
        if (t.ability == right->row.guid) right_task = &t;
    }
    bool has_roll = false;
    if (right_task) {
        for (const combat::CondGroup& g : right_task->groups) {
            for (const combat::CondToken& t : g) has_roll = has_roll || t.atom.kind == combat::CondAtom::Kind::Random;
        }
    }
    CHECK(right_task && !right_task->unknown && !right_task->off && has_roll, "the tables carry the right swing with a roll among its conditions");
    if (!right_task) return;

    Rig rig(4, 20, {});
    if (!rig.ok()) return;
    combat_rig::World w;
    if (!w.problem.empty()) return;
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorldConfig cfg;
    cfg.seed = 5;
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion, cfg);
    world.set_ability_data(&tables);
    const uint64_t bear = w.spawn(bear_path, 0, 0, 7, 900, bp->half_height);
    CHECK(bear != 0, "a Bear is made from its open");
    if (bear == 0) return;
    CHECK(combat::npc_knows_ability(*w.registry.find(bear), right->row.guid) && combat::npc_knows_ability(*w.registry.find(bear), left->row.guid), "its open lists both swings, so the world plays their animations");
    combat_rig::FakePlayer player(rig, 77, {0, 800, 1098});
    world.add_player(&player);
    const double dt = 1.0 / 15.0;
    GremCastView last;
    int rights = 0, lefts = 0, others = 0;
    double now = 0;
    while (now < 60.0) {
        now += dt;
        if (const stats::StatValue* max_hp = rig.cs->find(content::kGuidMaxHealth)) rig.cs->set_current(content::kGuidHealth, max_hp->value);
        w.npcs->note_player(77, 0, 800, now);
        w.npcs->tick(now);
        world.tick(now);
        const GremCastView view = grem_cast_view(w.registry, bear);
        if (view.montage != 0 && (view.element != last.element || last.montage == 0)) {
            if (view.ability == right->row.guid) ++rights;
            else if (view.ability == left->row.guid) ++lefts;
            else ++others;
        }
        last = view;
    }
    std::printf("  60 s against a standing player: %d right swings, %d left swings, %d other attacks\n", rights, lefts, others);
    CHECKF(rights >= 3 && lefts >= 3, "the swings come in both hands: the roll lets the right one through about half the time (%d right, %d left)", rights, lefts);
    CHECKF(rights + lefts >= 10, "and the Bear keeps attacking while it rolls (%d swings in 60 s)", rights + lefts);
}

AOC_REGISTER_TEST(test_creature_roll_gate);

// ---- fix batch 3: where a hit lands, by its targeting record -----------------------------------------------------------------------------------------------------------------------

void test_hit_area_rules() {
    std::printf("where a hit lands, by its targeting record: a record that measures nothing hits its target wherever he stands, one that measures holds its cone, volumes hit everyone they overlap\n");
    using combat::BiteArea;
    using combat::HitSubject;
    using combat::HitVolume;
    constexpr double kPi = 3.14159265358979323846;
    // a body `dist` cm from the creature's centre, `bearing` degrees off the way it faces (it faces `yaw`), `radius` wide
    const auto body = [&](double dist, double bearing, double yaw = 0.0, double radius = 34.0) {
        HitSubject s;
        s.cx = 1000.0;
        s.cy = -500.0;
        s.yaw = yaw;
        const double a = (yaw + bearing) * kPi / 180.0;
        s.tx = s.cx + dist * std::cos(a);
        s.ty = s.cy + dist * std::sin(a);
        s.tradius = radius;
        return s;
    };
    const std::function<bool()> clear = [] { return true; };
    const std::function<bool()> blocked = [] { return false; };
    int asked = 0;
    const std::function<bool()> counted = [&asked] {
        ++asked;
        return true;
    };

    // validity off: the Snap's record states 500 cm and 120 degrees, and neither is tested
    BiteArea any;
    any.discovery = BiteArea::Discovery::PrefersActive;
    any.distance = 500;
    any.angle = 120;
    any.cone_check = true;
    any.los_check = true;
    CHECK(any.gate() == BiteArea::Gate::Any && any.usable(), "a record that finds a target and measures nothing is an Any record");
    CHECK(combat::hit_lands(any, body(30, 0), true, clear) && combat::hit_lands(any, body(5000, 180), true, clear) && combat::hit_lands(any, body(900, 90), true, blocked),
          "an Any record hits its target at 30 cm, 50 m behind it and 9 m to its side with no line of sight");
    CHECK(!combat::hit_lands(any, body(30, 0), false, clear), "... and nobody else (it has no volume)");
    asked = 0;
    combat::hit_lands(any, body(300, 20), true, counted);
    CHECK(asked == 0, "... without asking for a line of sight");

    // validity on: the Fury's record (300 cm, 60 degrees, cone and line of sight)
    BiteArea fury = any;
    fury.discovery = BiteArea::Discovery::OnlyLocked;
    fury.check_validity = true;
    fury.distance = 300;
    fury.angle = 60;
    CHECK(fury.gate() == BiteArea::Gate::Validity, "a record whose validity is on is a Validity record");
    CHECK(combat::hit_lands(fury, body(333, 0), true, clear) && !combat::hit_lands(fury, body(335, 0), true, clear), "its reach is 300 cm to the body's surface: a body 34 cm wide lands at 333 cm and not at 335");
    CHECK(combat::hit_lands(fury, body(200, 39), true, clear) && !combat::hit_lands(fury, body(200, 41), true, clear),
          "its cone is 30 degrees either side (half the 60), plus the body's own width: lands 39 degrees off at 200 cm, not 41");
    CHECK(combat::hit_lands(fury, body(200, -39), true, clear) && !combat::hit_lands(fury, body(200, -41), true, clear) && !combat::hit_lands(fury, body(200, 180), true, clear), "... to either side, and not behind");
    CHECK(combat::hit_lands(fury, body(200, 0, 90), true, clear) && !combat::hit_lands(fury, body(200, 90, 90), true, clear), "the cone goes the way the creature faces (facing 90 degrees: ahead lands, 90 degrees off does not)");
    CHECK(!combat::hit_lands(fury, body(200, 0), true, blocked) && combat::hit_lands(fury, body(200, 0), true, clear), "it needs a line of sight");
    asked = 0;
    combat::hit_lands(fury, body(400, 0), true, counted);
    combat::hit_lands(fury, body(200, 80), true, counted);
    CHECK(asked == 0, "... which it asks for only once the body is inside the cone");
    CHECK(!combat::hit_lands(fury, body(200, 0), false, clear), "it hits its target and nobody else");
    BiteArea round = fury;
    round.angle = 360;
    BiteArea none = fury;
    none.angle = 0;
    CHECK(combat::hit_lands(round, body(200, 180), true, clear) && combat::hit_lands(none, body(200, 180), true, clear) && !combat::hit_lands(round, body(340, 180), true, clear),
          "an angle of 360 or 0 is all round, still inside the reach");
    BiteArea shifted = fury;
    shifted.angle_center = 90;
    BiteArea ahead = fury;
    ahead.distance_center = 100;
    CHECK(combat::hit_lands(shifted, body(200, 90), true, clear) && !combat::hit_lands(shifted, body(200, 0), true, clear), "the angle centre turns the cone (90: it points to the creature's right)");
    CHECK(combat::hit_lands(ahead, body(433, 0), true, clear) && !combat::hit_lands(ahead, body(436, 0), true, clear), "the distance centre moves the start of the cone ahead of the creature (100 cm: the reach ends 100 cm farther)");
    BiteArea near = fury;
    near.min_distance = 100;
    CHECK(!combat::hit_lands(near, body(120, 0), true, clear) && combat::hit_lands(near, body(180, 0), true, clear), "a body nearer than the minimum distance to its surface (100 cm) is not hit");
    BiteArea no_reach = fury;
    no_reach.distance = 0;
    CHECK(!combat::hit_lands(no_reach, body(0, 0), true, clear), "a cone with no reach holds nothing");
    BiteArea sight_only = fury;
    sight_only.cone_check = false;
    CHECK(combat::hit_lands(sight_only, body(4000, 180), true, clear) && !combat::hit_lands(sight_only, body(100, 0), true, blocked), "validity on with the cone switch off asks only for the line of sight");

    // volumes: the Grem's pursuit (a sphere 64 cm wide, 75 ahead) and the Ram's headbutt (a box 100 x 80 half-sizes, 130 ahead)
    BiteArea sphere;
    HitVolume ball;
    ball.shape = HitVolume::Shape::Sphere;
    ball.ox = 75;
    ball.radius = 64;
    sphere.volumes.push_back(ball);
    sphere.volume_count = 1;
    CHECK(sphere.gate() == BiteArea::Gate::Volumes && sphere.usable() && !sphere.has_target_part(), "a record with a volume and no target part is a Volumes record");
    CHECK(combat::hit_lands(sphere, body(173, 0), false, clear) && !combat::hit_lands(sphere, body(175, 0), false, clear) && !combat::hit_lands(sphere, body(300, 180), false, clear),
          "a sphere 75 cm ahead with a radius of 64 hits a body 34 cm wide out to 173 cm ahead, and nothing behind it, whoever the body is");
    CHECK(combat::hit_lands(sphere, body(100, 0, 90), false, clear) && !combat::hit_lands(sphere, body(100, 90, 90), false, clear), "the sphere goes the way the creature faces (facing 90 degrees: ahead lands, 90 degrees off does not)");
    CHECK(!combat::hit_lands(sphere, body(100, 0), false, blocked), "a volume asks for a line of sight too (its switch is on)");
    BiteArea crate;
    HitVolume box;
    box.shape = HitVolume::Shape::Box;
    box.ox = 130;
    box.hx = 100;
    box.hy = 80;
    box.los = false;
    crate.volumes.push_back(box);
    crate.volume_count = 1;
    CHECK(combat::hit_lands(crate, body(264, 0), false, clear) && !combat::hit_lands(crate, body(266, 0), false, clear) && combat::hit_lands(crate, body(60, 0), false, clear) &&
              combat::hit_lands(crate, body(-3, 0), false, clear) && !combat::hit_lands(crate, body(-5, 0), false, clear),
          "a box from 30 to 230 cm ahead hits a body 34 cm wide from 3 cm behind the creature's centre to 264 cm ahead of it");
    CHECK(combat::hit_lands(crate, body(150, 35), false, clear) && !combat::hit_lands(crate, body(200, 90), false, clear), "... and 80 cm to each side of its centre line, plus the body's width");
    CHECK(combat::hit_lands(crate, body(150, 0), false, blocked), "a volume whose line-of-sight switch is off hits through a wall");
    BiteArea both = fury;
    both.volumes.push_back(ball);
    both.volume_count = 1;
    CHECK(both.gate() == BiteArea::Gate::Volumes, "a record with a target part and a volume counts as a Volumes record");
    CHECK(combat::hit_lands(both, body(400, 180), true, clear) == false && combat::hit_lands(both, body(100, 0), false, clear) && combat::hit_lands(both, body(200, 0), true, clear),
          "... and hits what either part reaches: its target inside the cone, and anybody in the volume");
    BiteArea odd = sphere;
    odd.readable = false;
    CHECK(!odd.usable(), "a record with a volume the world cannot place is not usable");
}

AOC_REGISTER_TEST(test_hit_area_rules);

// The creature tables are read strictly: a number that is not finite (nan, inf) and a flag that is neither 0 nor 1 fail the load.  They used to be read as a number (a NaN length would freeze a creature, an
// infinite angle would loop) or as a plain 0 without a word.
void test_creature_tables_are_read_strictly() {
    std::printf("the creature tables: a good row loads; a NaN or an infinite number and a flag that is neither 0 nor 1 fail the load\n");
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path() / ("aoc_creature_tables_strict_" + unique_temp_suffix());
    fs::create_directories(dir, ec);
    const std::string abilities_header =
        "archetype\tability\ttask\tcooldown\tstop_movement\torder\tmin_distance\tmax_distance\tmax_angle\tgates\tunknown\tfight\tconds\twhy\tstarts\trotate\tarrival";
    const std::string montages_header = "montage\tguid\tpackage\tpackage_guid\tlength\trate_scale\tsections\thits\troot_motion\troot_unreadable\tanim_end\tfires";
    const std::string targeting_header =
        "ability\tkey\tdistance\tmin_distance\tangle\tangle_center\tcone_check\tfreeze\tfreeze_distance\tfreeze_scale\tignore_non_target\tdiscovery\tcheck_validity\tlos_check\tdistance_center\tvolumes\tshapes";
    const auto write = [&](const char* name, const std::string& header, const std::string& row) {
        std::ofstream out(dir / name, std::ios::binary);
        out << header << "\n";
        if (!row.empty()) out << row << "\n";
        return (dir / name).string();
    };
    const std::string montages = write("montages.tsv", montages_header, "");
    const std::string targeting = write("targeting.tsv", targeting_header, "");
    // archetype, ability, task, cooldown, stop_movement, order, min_distance, max_distance, max_angle, gates, unknown, fight, conds, why, starts, rotate, arrival
    const auto ability_row = [](const char* cooldown, const char* stop, const char* max_distance, const char* fight) {
        return std::string("Wolf\t7\tBite\t") + cooldown + "\t" + stop + "\t0\t0\t" + max_distance + "\t180\t\t0\t" + fight + "\t\t\t\t\t0";
    };
    const auto loads = [&](const std::string& row) {
        combat::NpcAbilityData tables;
        std::string err;
        return tables.load(write("abilities.tsv", abilities_header, row), montages, targeting, err);
    };
    CHECK(loads(ability_row("2.5", "1", "300", "1")), "a good row loads");
    CHECK(!loads(ability_row("nan", "1", "300", "1")), "a NaN cooldown fails the load");
    CHECK(!loads(ability_row("2.5", "1", "inf", "1")), "an infinite reach fails the load");
    CHECK(!loads(ability_row("2.5", "2", "300", "1")) && !loads(ability_row("2.5", "1", "300", "yes")), "a flag that is neither 0 nor 1 fails the load");
    fs::remove_all(dir, ec);
}

AOC_REGISTER_TEST(test_creature_tables_are_read_strictly);
