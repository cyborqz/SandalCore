// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  combat/combat_knockback.inc: what throws, pushes and pulls a creature (included at the end of combat_world.cpp, inside its namespace)
//
//  A hit that carries a velocity change (Get Off The Stage) adds it to the velocity of the creature it lands on, along the frame the record's origin transform makes.  A crowd-control record with the
//  displaced bit pushes for as long as its effect lives: the NPC world plays the push a step at a time (npc/displacement.h) and asks this file, at every step, where the push works from and how strong it
//  is now.  A hit that names the teleport "in front of the instigator" (the Grapple) puts the creature in front of the one who pulled it.  A creature whose Blueprint carries the Boss tag is none of
//  thrown, pushed or pulled: the throw and the pull are left out, and the effect of a push is not put on it.
// ============================================================================

namespace {

/// Whether a list of tags (comma or semicolon separated, as the Blueprint table keeps them) holds the tag; a tag's name is not case sensitive, as an FName is not.
bool tag_list_has(const std::string& list, const char* want) {
    const size_t want_size = std::strlen(want);
    size_t from = 0;
    while (from <= list.size()) {
        size_t to = list.find_first_of(",;", from);
        if (to == std::string::npos) to = list.size();
        size_t a = from, b = to;
        while (a < b && std::isspace(static_cast<unsigned char>(list[a]))) ++a;
        while (b > a && std::isspace(static_cast<unsigned char>(list[b - 1]))) --b;
        if (b - a == want_size) {
            bool same = true;
            for (size_t i = 0; i < want_size && same; ++i) same = std::tolower(static_cast<unsigned char>(list[a + i])) == std::tolower(static_cast<unsigned char>(want[i]));
            if (same) return true;
        }
        from = to + 1;
    }
    return false;
}

OriginActor origin_actor_of_fighter(Fighter& f) {
    OriginActor o;
    o.known = true;
    o.pos = f.position();
    o.yaw_degrees = f.facing_degrees();
    o.radius_cm = f.radius_cm();
    return o;
}

/// The evaluations a push's formula runs in (the one who pushes and the one pushed), made when the formula is first asked and kept for as long as the push plays.
struct PushEval {
    std::unique_ptr<stats::CharacterStats::Evaluation> by, mob;
    bool tried = false;
};

constexpr double kKbPi = 3.14159265358979323846;

}  // namespace

bool CombatWorld::is_boss(const Npc& n) const {
    return tag_list_has(n.bp.tags, "Boss");
}

void CombatWorld::spare_boss(const Npc& n, const std::string& by, const char* verb) {
    if (boss_spared_.insert({n.actor_id, verb}).second) spdlog::info("[combat] {} ({}) carries the Boss tag: {} does not {} it", n.leaf, n.actor_id, by, verb);
}

OriginActors CombatWorld::origin_actors(const Npc& n, PlayerPort* by) {
    OriginActors a;
    if (by) a.owner = a.instigator = a.source = origin_actor_of_fighter(by->fighter());
    if (const world::WorldActor* w = registry_.find(n.actor_id)) {
        a.target.known = true;
        a.target.pos = {w->x, w->y, w->z};
        a.target.yaw_degrees = w->yaw_degrees;
        a.target.radius_cm = n.bp.radius;
    }
    return a;
}

void CombatWorld::move_by_hit(Npc& n, uint64_t attacker, const Resolved& r, const HitOrigin& origin, double now) {
    (void)now;
    if (!npcs_world_ || !origin.hit || r.miss() || n.dead || is_pet(n.actor_id)) return;
    const abilities::HitDef& hit = *origin.hit;
    if (!hit.impulse && !hit.teleport) return;
    PlayerPort* by = player(attacker);
    if (!by) return;
    const OriginActors actors = origin_actors(n, by);
    if (!actors.instigator.known || !actors.target.known) return;

    const bool boss = is_boss(n);   // a boss is neither thrown nor pulled

    // ---- the velocity change
    if (hit.impulse && hit.impulse->applies && r.impulse.valid) {
        const content::HitImpulseRow& im = *hit.impulse;
        if (boss) {
            spare_boss(n, hit.row.name, "throw");
        } else {
            const OriginFrame frame = origin_frame(im.origin_location, im.origin_orientation, Vec3{im.origin_rel_x, im.origin_rel_y, im.origin_rel_z}, actors);
            if (!frame.ok) {
                spdlog::debug("[combat] {} puts its origin at {} / {}, which is not served: it throws nobody", hit.row.name, im.origin_location, im.origin_orientation);
            } else {
                double speed = r.impulse.magnitude, lift = r.impulse.angle_degrees;
                std::string where;   // (how far the creature stood from the Bard and how far off his facing, for the log)
                nav::Vec3 dir;
                const Vec3& me = actors.instigator.pos;
                const Vec3& it = actors.target.pos;
                if (kGetOffTheStageModelled && hit.row.name == "Bard_Misc_GetOffTheStage") {
                    // the modelled throw: along the line from the Bard to the creature, shaped by how near it is and how far off his facing
                    const double facing = actors.instigator.yaw_degrees * kKbPi / 180.0;
                    const double dx = it.x - me.x, dy = it.y - me.y, d = std::hypot(dx, dy);
                    const double ux = d > 1e-6 ? dx / d : std::cos(facing), uy = d > 1e-6 ? dy / d : std::sin(facing);
                    const double phi = std::acos(std::clamp(ux * std::cos(facing) + uy * std::sin(facing), -1.0, 1.0)) * 180.0 / kKbPi;
                    const ThrowShape shape = modelled_throw(d, phi, speed, lift);
                    where = fmt::format(" from {:.1f} m, {:.0f} degrees off the way he faces", d / 100.0, phi);
                    speed = shape.speed;
                    lift = shape.lift_degrees;
                    const double a = lift * kKbPi / 180.0;
                    dir = {ux * std::cos(a), uy * std::cos(a), std::sin(a)};
                } else {
                    // the record's direction in its frame, lifted up by its angle
                    const double len = std::hypot(im.dir_x, im.dir_y);
                    const double lx = len > 1e-9 ? im.dir_x / len : 1.0, ly = len > 1e-9 ? im.dir_y / len : 0.0;
                    const double a = lift * kKbPi / 180.0;
                    dir = npc::frame_rotate(frame.yaw_degrees, frame.pitch_degrees, nav::Vec3{lx * std::cos(a), ly * std::cos(a), std::sin(a)});
                }
                if (!im.velocity_change) speed /= npc::kDefaultMass;   // an impulse over the mass, not a velocity change [no hit in the data is one]
                const nav::Vec3 change{dir.x * speed, dir.y * speed, dir.z * speed};
                const bool airborne = npcs_world_->launch(n.actor_id, change);
                if (npcs_world_->displaced(n.actor_id)) note_moved(n);
                if (airborne) drop_attack(n);   // a creature thrown into the air loses the swing it had begun [modelled]
                if (im.rotation_behavior.find("FaceOrigin") != std::string::npos) npcs_world_->face_toward(n.actor_id, frame.pos.x, frame.pos.y);
                spdlog::info("[combat] {} ({}) is thrown by {}{}: {:.0f} cm/s at {:.0f} degrees{}", n.leaf, n.actor_id, hit.row.name, where, speed, lift, airborne ? ", into the air" : "");
            }
        }
    }

    // ---- the pull: the creature is put in front of the one who pulled it, the gap and both radii from him, facing him
    if (hit.teleport && boss) {
        spare_boss(n, hit.row.name, "pull");
    } else if (hit.teleport) {
        const content::TeleportRow& tp = *hit.teleport;
        const bool in_front_of_instigator = tp.script.find("TeleportToInstigatorScript") != std::string::npos && enum_tail(tp.target) == "Instigator" && !tp.swap_with_target;
        if (!in_front_of_instigator) {
            spdlog::debug("[combat] the teleport {} of {} is not served: it moves nobody", tp.name, hit.row.name);
        } else {
            const OriginActor& me = actors.instigator;
            const double yaw = me.yaw_degrees * kKbPi / 180.0;
            const double gap = actors.target.radius_cm + me.radius_cm + kPullPaddingCm;
            const double x = me.pos.x + std::cos(yaw) * gap, y = me.pos.y + std::sin(yaw) * gap;
            double ground = 0;
            if (ground_height(x, y, me.pos.z, &ground) && npcs_world_->warp(n.actor_id, x, y, ground)) {
                npcs_world_->face_toward(n.actor_id, me.pos.x, me.pos.y);
                spdlog::info("[combat] {} ({}) is pulled in front of player {} by {}", n.leaf, n.actor_id, attacker, hit.row.name);
            }
        }
    }
}

void CombatWorld::track_push(Npc& n, const effects::Book::Crowd& was, const effects::Book::Crowd& cc) {
    if (!npcs_world_) return;
    if (!cc.push) {   // no push plays any more: the creature slides on to a stop from the velocity it has
        if (was.push) {
            npcs_world_->end_push(n.actor_id);
            n.push_origin = {};
        }
        return;
    }
    if (was.push && was.push_key == cc.push_key) return;   // the same application plays on
    n.push_origin = {};
    const content::CrowdControlRow& rec = *cc.push;
    npc::PushPlay play;
    if (rec.push_parameterization.find("Force") != std::string::npos) play.kind = npc::PushPlay::Kind::Force;
    else if (rec.push_parameterization.find("Acceleration") != std::string::npos) play.kind = npc::PushPlay::Kind::Acceleration;
    else if (rec.push_parameterization.find("Velocity") != std::string::npos) play.kind = npc::PushPlay::Kind::Velocity;
    else return;   // (a push by time: the data has it for tests only)
    PlayerPort* by = player(cc.push_instigator);
    if (!by) return;   // (nobody to push from: its effect ends with him)
    const Vec3 rel{rec.push_origin_rel_x, rec.push_origin_rel_y, rec.push_origin_rel_z};
    const OriginFrame frame = origin_frame(rec.push_origin_location, rec.push_origin_orientation, rel, origin_actors(n, by));
    if (!frame.ok) {
        spdlog::debug("[combat] the push {} puts its origin at {} / {}, which is not served: it pushes nobody", rec.name, rec.push_origin_location, rec.push_origin_orientation);
        return;
    }
    n.push_origin = {true, frame};
    play.dir = {rec.push_dir_x, rec.push_dir_y, rec.push_dir_z};
    play.arrival_cm = rec.push_arrival_distance;
    play.end_at_arrival = rec.end_push_at_arrival;
    // the number the record gives: a plain number as it is, a formula run for the two (GetDistanceBetween reads the distance between their bodies' centres)
    const auto number = [this](const content::CrowdControlRow& r, PlayerPort& pusher, Npc& mob, const nav::Vec3& body, PushEval& eval) -> double {
        if (r.push_magnitude_expr == 0) return 0.0;
        const content::ExprRow* row = catalog_.expr(r.push_magnitude_expr);
        if (!row) return 0.0;
        char* end = nullptr;
        const double plain = std::strtod(row->text.c_str(), &end);
        if (end != row->text.c_str() && row->text.find_first_not_of(" \t\r\n", static_cast<size_t>(end - row->text.c_str())) == std::string::npos) return plain;
        if (!eval.tried) {
            eval.tried = true;
            std::string err;
            eval.by = pusher.fighter().stats().evaluation(pusher.fighter().worn(), err, true);
            eval.mob = mob.stats().evaluation(mob.worn(), err, true);
        }
        if (!eval.by || !eval.mob) return 0.0;
        const Vec3 me = pusher.fighter().position();
        const double dx = me.x - body.x, dy = me.y - body.y, dz = me.z - body.z;
        double value = 0;
        std::string why;
        if (!resolver_.eval_between(*eval.by, eval.mob.get(), r.push_magnitude_expr, std::sqrt(dx * dx + dy * dy + dz * dz), value, why)) {
            spdlog::debug("[combat] the push {}: its number does not run: {}", r.name, why);
            return 0.0;
        }
        return value;
    };
    const auto eval = std::make_shared<PushEval>();
    play.frame = [this, id = n.actor_id, pusher = cc.push_instigator, rec = cc.push, cached = frame, start = cc.push_start, end = cc.push_end, rel, number, eval](
                     const nav::Vec3& body, double at, double dt, npc::PushFrame& f) {
        const auto it = npcs_.find(id);
        PlayerPort* by = player(pusher);
        if (it == npcs_.end() || it->second->dead || !by || !by->fighter().alive() || at - dt >= end) {   // the creature, the one who pushed or the effect is gone
            f.alive = false;
            return true;
        }
        Npc& mob = *it->second;
        OriginFrame fr = cached;
        if (!rec->push_cache_origin) {   // the origin is worked out again at every step from where the actors are now
            fr = origin_frame(rec->push_origin_location, rec->push_origin_orientation, rel, origin_actors(mob, by));
            if (!fr.ok) {
                f.alive = false;
                return true;
            }
        }
        f.origin = {fr.pos.x, fr.pos.y, fr.pos.z};
        f.yaw_degrees = fr.yaw_degrees;
        f.pitch_degrees = fr.pitch_degrees;
        f.fraction = dt > 0 ? std::clamp((std::min(at, end) - std::max(at - dt, start)) / dt, 0.0, 1.0) : 1.0;   // the share of this step that the effect lived for
        f.magnitude = number(*rec, *by, mob, body, *eval);
        return true;
    };
    if (!npcs_world_->begin_push(n.actor_id, std::move(play))) return;
    note_moved(n);
    spdlog::info("[push] {} ({}) is pushed by {}", n.leaf, n.actor_id, rec.name);
}

void CombatWorld::note_moved(const Npc& n) {
    if (const world::WorldActor* w = registry_.find(n.actor_id)) moved_from_.emplace(n.actor_id, Vec3{w->x, w->y, w->z});   // (the first place only: a creature thrown and then pushed is measured from where it stood before the throw)
}

void CombatWorld::log_rests() {
    if (moved_from_.empty() || !npcs_world_) return;
    for (auto it = moved_from_.begin(); it != moved_from_.end();) {
        const auto found = npcs_.find(it->first);
        const world::WorldActor* w = registry_.find(it->first);
        if (found == npcs_.end() || found->second->dead || !w) {   // (gone or dead: nothing to name)
            it = moved_from_.erase(it);
            continue;
        }
        if (npcs_world_->displaced(it->first)) {   // (still flying or sliding)
            ++it;
            continue;
        }
        const Npc& n = *found->second;
        spdlog::log(rests_logged_++ < kLoggedRests ? spdlog::level::info : spdlog::level::debug, "[combat] {} ({}) came to rest {:.1f} m from where it was moved", n.leaf, n.actor_id,
                    distance_xy_cm(it->second, Vec3{w->x, w->y, w->z}) / 100.0);
        it = moved_from_.erase(it);
    }
}

void CombatWorld::push_wire(Npc& n, const effects::Book::Crowd& cc, CcWire& wire) {
    if (!cc.push) return;
    wire.push_record = cc.push->guid;
    if (const PlayerPort* p = player(cc.push_instigator)) {
        wire.push_instigator = p->character_guid();
        wire.push_source = wire.push_instigator;   // (the one who started it is where it came from)
    }
    wire.push_start = bootstrap::effect_clock(cc.push_start);
    if (n.push_origin.set) {
        wire.has_origin = true;
        wire.origin_x = n.push_origin.frame.pos.x;
        wire.origin_y = n.push_origin.frame.pos.y;
        wire.origin_z = n.push_origin.frame.pos.z;
        wire.origin_yaw_degrees = n.push_origin.frame.yaw_degrees;
    }
}
