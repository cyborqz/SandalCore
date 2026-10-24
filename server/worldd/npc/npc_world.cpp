// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "npc/npc_world.h"

#include "protocol/emit/intrepid_netguid.h"
#include "protocol/repl/codecs.h"
#include "protocol/repl/move_packed.h"
#include "protocol/repl/value.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace aoc::worldd::npc {

namespace repl = ::aoc::protocol::repl;
namespace emit = ::aoc::protocol::emit;

namespace {

uint64_t splitmix(uint64_t& s) {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

uint16_t angle16(double degrees) {
    double d = std::fmod(degrees, 360.0);
    if (d < 0) d += 360.0;
    return static_cast<uint16_t>(std::lround(d / 360.0 * 65536.0) & 0xFFFF);
}

uint16_t angle8(double degrees) {
    double d = std::fmod(degrees, 360.0);
    if (d < 0) d += 360.0;
    return static_cast<uint16_t>(std::lround(d / 360.0 * 256.0) & 0xFF);
}

double dist2d(const nav::Vec3& a, const nav::Vec3& b) { return std::hypot(a.x - b.x, a.y - b.y); }

bool finite_setup(const NpcMotionSetup& s) {
    for (double v : {s.home_yaw, s.foot_offset, s.yaw_rate, s.wander_min, s.wander_max, s.wander_delay, s.wander_delay_deviation, s.walk_speed, s.patrol_speed, s.run_speed, s.leg_median, s.leg_p90,
                     s.arrival_distance, s.awake_radius}) {
        if (!std::isfinite(v)) return false;
    }
    for (const nav::Vec3& p : s.route) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return false;
    }
    for (double p : s.pauses) {
        if (!std::isfinite(p)) return false;
    }
    return s.foot_offset >= 0 && s.foot_offset < 5000 && s.wander_min >= 0 && s.wander_max >= s.wander_min && s.walk_speed >= 0 && s.patrol_speed >= 0 && s.run_speed >= 0;
}
// [Cyb :: C.O.A]

/// Writes what the retail server writes for a walking NPC into the actor's replicated state: ReplicatedMovement (location 0.01 cm, velocity in whole cm/s,
/// yaw as one byte), its timestamp and CurrentMovementInput.  The open's own location and rotation follow: a re-open (a dormant actor coming back) starts
/// where the NPC is.
/// The client applies none of it while the actor's bReplicateMovement is off, and the opens the NPCs are made from do not all have it on, so every place the world writes
/// turns it on: the creature is where the server has it from its first step, its first turn and its first montage on.
/// `again` (1, 2, 3 ...) is the number of the time this same place is sent once more, as the retail server does after a montage (its floats move below what the wire shows, so it sends the place at every
/// update): the replicator sends a value only when it differs from the one it sent, so the copy differs from the last in the angular velocity, which the wire carries for a physics body only and so never
/// for a creature: the bits that go out are the same ones.
void write_replicated(world::WorldActor& a, double x, double y, double z, double yaw, const nav::Vec3& vel, bool moving, double stamp, int again = 0) {
    repl::ObjectState& obj = a.actor.obj;
    if (!obj.schema) return;
    repl::RepMovement m;
    if (const repl::Value* v = obj.state.get(7)) {
        if (const auto* old = v->get_if<repl::RepMovement>()) m = *old;
    }
    if (obj.schema->command("bReplicateMovement")) obj.state.set(*obj.schema, "bReplicateMovement", repl::Value::boolean(true));
    m.simulated_physic_sleep = false;
    m.rep_physics = false;
    m.location = repl::quantize_location(obj.opts, x, y, z);
    m.linear_velocity = repl::quantize_velocity(obj.opts, vel.x, vel.y, vel.z);
    m.angular_velocity = repl::QuantVec{repl::QuantVec::Mode::Scaled, again, 0, 0};
    m.acceleration.reset();
    m.rotation = repl::Rot{0, obj.opts.rep_movement_short_rotation ? angle16(yaw) : angle8(yaw), 0};
    if (obj.schema->command("ReplicatedMovement")) obj.state.set(*obj.schema, "ReplicatedMovement", repl::Value(m));
    if (obj.schema->command("ReplicatedServerLastTransformUpdateTimeStamp"))
        obj.state.set(*obj.schema, "ReplicatedServerLastTransformUpdateTimeStamp", repl::Value::f32(static_cast<float>(stamp)));
    if (obj.schema->command("CurrentMovementInput"))
        obj.state.set(*obj.schema, "CurrentMovementInput", repl::Value(repl::PlayerMovementInput{static_cast<uint8_t>(moving ? repl::PlayerMovementInput::kForward : 0)}));
    a.actor.open.spawn.location = repl::quantize_vector(10, x, y, z);
    a.actor.open.spawn.rotation = repl::Rot{0, angle16(yaw), 0};
    a.actor.open.spawn.velocity.reset();
}

/// Writes what the retail server writes while a montage with root motion plays (ACharacter::RepRootMotion): the montage, whether it still carries the creature, how far it has played, and
/// the creature's own place and facing at that position, which the client sets its copy back to as it plays the same motion.  The montage is cleared when it no longer carries the creature.
/// `obstructed` is bRootMotionObstructed: the creature is held where it is because a body is in the ability's freeze box (the client zeroes its copy's root-motion velocity while it is set; the
/// montage's clock goes on).
void write_root_motion(world::WorldActor& a, bool active, uint64_t montage, double position, double x, double y, double z, double yaw, const nav::Vec3& vel, bool obstructed = false) {
    repl::ObjectState& obj = a.actor.obj;
    if (!obj.schema) return;
    const auto set = [&](const char* name, repl::Value v) {
        if (obj.schema->command(name)) obj.state.set(*obj.schema, name, std::move(v));
    };
    set("bRootMotionObstructed", repl::Value::boolean(obstructed));
    // the client puts its copy where these say (and where ReplicatedMovement says) only while the actor replicates its movement (every place the world writes turns that on)
    set("bReplicateMovement", repl::Value::boolean(true));
    set("RepRootMotion.Animation", repl::Value(emit::FIntrepidNetworkGUID::from_u64(active ? montage : 0)));
    set("RepRootMotion.bIsActive", repl::Value::boolean(active));
    set("RepRootMotion.Position", repl::Value::f32(static_cast<float>(position)));
    set("RepRootMotion.Location", repl::Value(repl::quantize_vector(100, x, y, z)));
    set("RepRootMotion.Rotation", repl::Value(repl::Rot{0, angle16(yaw), 0}));
    set("RepRootMotion.LinearVelocity", repl::Value(repl::quantize_vector(10, vel.x, vel.y, vel.z)));   // (the creature's velocity: the retail server's carries it in every update of a lunge)
}

/// Sets the creature's ReplicatedMovementMode and returns the mode it had (none when it carries none).  The retail server puts a creature in the walking mode while a montage carries it and back
/// when the montage lets go; the client moves its copy by the montage's root motion in that mode.
std::optional<uint64_t> swap_movement_mode(world::WorldActor& a, uint64_t mode) {
    std::optional<uint64_t> old;
    repl::ObjectState& obj = a.actor.obj;
    if (!obj.schema || !obj.schema->command("ReplicatedMovementMode")) return old;
    if (const repl::Value* v = obj.state.get(*obj.schema, "ReplicatedMovementMode")) {
        if (const uint64_t* p = v->get_if<uint64_t>()) old = *p;
    }
    obj.state.set(*obj.schema, "ReplicatedMovementMode", repl::Value::u64(mode));
    return old;
}

/// Where a montage's root is `at` seconds into it (a play rate of 1): linear between the steps, the last one after the end.
void root_at(const std::vector<NpcWorld::RootMotionStep>& steps, double at, double* x, double* y) {
    *x = *y = 0;
    if (steps.empty()) return;
    if (at <= steps.front().time) {
        *x = steps.front().x;
        *y = steps.front().y;
        return;
    }
    for (size_t i = 1; i < steps.size(); ++i) {
        if (at > steps[i].time) continue;
        const double span = steps[i].time - steps[i - 1].time;
        const double f = span > 0 ? (at - steps[i - 1].time) / span : 1.0;
        *x = steps[i - 1].x + (steps[i].x - steps[i - 1].x) * f;
        *y = steps[i - 1].y + (steps[i].y - steps[i - 1].y) * f;
        return;
    }
    *x = steps.back().x;
    *y = steps.back().y;
}

}  // namespace

NpcWorld::NpcWorld(world::ActorRegistry& registry, const nav::AreaNav* nav, NpcWorldConfig config) : registry_(registry), nav_(nav), cfg_(config) {
    // an actor the registry removes (a dead NPC) leaves the NPC world with it
    remove_listener_ = registry_.on_remove([this](const world::WorldActor& a) { remove(a.id); });
}

NpcWorld::~NpcWorld() { registry_.off_remove(remove_listener_); }

bool NpcWorld::remove(uint64_t actor_id) {
    const auto it = npcs_.find(actor_id);
    if (it == npcs_.end()) return false;
    // (review A M1) an NPC removed while it waits for its chunk no longer waits: the count follows (its id stays in pending_by_chunk_ and is skipped when the chunk comes)
    if (it->second.pending_ground && pending_count_ > 0) --pending_count_;
    npcs_.erase(it);
    active_.erase(std::remove(active_.begin(), active_.end(), actor_id), active_.end());
    rooted_.erase(std::remove(rooted_.begin(), rooted_.end(), actor_id), rooted_.end());
    settling_.erase(std::remove(settling_.begin(), settling_.end(), actor_id), settling_.end());
    return true;
}

double NpcWorld::next_unit(Npc& n) {
    return static_cast<double>(splitmix(n.rng) >> 11) / 9007199254740992.0;   // [0, 1)
}

bool NpcWorld::add(uint64_t actor_id, const NpcMotionSetup& setup, bool* ground_found, bool* pending) {
    if (ground_found) *ground_found = false;
    if (pending) *pending = false;
    if (!nav_ || npcs_.count(actor_id) || !finite_setup(setup)) return false;
    world::WorldActor* a = registry_.find(actor_id);
    if (!a || (a->kind != world::ActorKind::Npc && a->kind != world::ActorKind::Station)) return false;   // (crafting, step 0: a station is grounded like a town's people and never moves)
    Npc n;
    n.id = actor_id;
    n.setup = setup;
    n.rng = actor_id * 0x9E3779B97F4A7C15ull ^ 0xA5A5A5A5A5A5A5A5ull;
    n.mover.init(nav_, setup.foot_offset, setup.yaw_rate);
    if (!nav_->ready_at(a->x, a->y)) {
        // The ground's window has not loaded this NPC's chunk: the navmesh is not there to be asked.  It stands on the terrain (which is always resident: the heightfield is
        // the exact surface for most NPCs) and sleeps; `chunks_resident` grounds it when the chunk comes, with a floor of the navmesh where the chunk has one.
        // A city node's people (`ground_by_data`) keep the height the data gave them: the terrain under a bowl is 50 m below the town's floor, so it would bury them.
        double terrain = 0;
        const bool have_terrain = !a->npc.ground_by_data && nav_->terrain_height(a->x, a->y, &terrain);
        const double hint = have_terrain ? terrain : a->z - setup.foot_offset;
        n.mover.place(a->x, a->y, hint, setup.home_yaw);
        n.mode = Mode::Stand;
        n.home = {a->x, a->y, hint};
        n.pending_ground = true;
        n.data_z = a->z;
        if (have_terrain) {
            registry_.move(actor_id, a->x, a->y, n.mover.origin_z(), n.mover.yaw_degrees());
            write_replicated(*a, a->x, a->y, n.mover.origin_z(), n.mover.yaw_degrees(), {}, false, now_);
        }
        pending_by_chunk_[nav_->chunk_at(a->x, a->y)].push_back(actor_id);
        ++pending_count_;
        n.spawn_home = n.home;
        n.spawn_ai_home = {a->npc.home_x, a->npc.home_y, a->npc.home_z};
        npcs_.emplace(actor_id, std::move(n));
        if (pending) *pending = true;
        return true;
    }
    const bool found = ground_npc(n, *a);
    if (ground_found) *ground_found = found;
    n.spawn_home = n.home;   // where it was made: a game master's move changes `home` and leaves this
    n.spawn_ai_home = {a->npc.home_x, a->npc.home_y, a->npc.home_z};
    npcs_.emplace(actor_id, std::move(n));
    return true;
}

bool NpcWorld::ground_npc(Npc& n, world::WorldActor& actor) {
    world::WorldActor* a = &actor;
    const NpcMotionSetup& setup = n.setup;
    // the ground under it: the surface nearest the height the data gave it (an origin: a half height above the feet)
    double gz = 0;
    double hint = (n.pending_ground_done ? n.data_z : a->z) - setup.foot_offset;
    bool found = false;
    double terrain = 0;
    if (a->npc.ground_by_terrain && nav_->terrain_height(a->x, a->y, &terrain)) {
        hint = terrain;   // a PopulationArea's random point has no height of its own: the ground under it is the height
        found = nav_->ground_height(a->x, a->y, hint, &gz, 1500.0, 1500.0);
    } else {
        // a city node's person has a height that is right to 1-168 cm: a tight window, so that it never takes a roof, a ledge or the pit's terrain for its floor
        found = a->npc.ground_by_data ? ground_by_data_at(a->x, a->y, hint, &gz)
                                      : nav_->ground_height(a->x, a->y, hint, &gz, 1500.0, 1500.0);
        // (a city node's people keep their data height when no surface is within reach: their town's floor is not in the baked ground, whose terrain lies 50 m below it)
        if (!found && !a->npc.ground_by_data && nav_->terrain_height(a->x, a->y, &terrain)) {
            // the data's height is out of reach of any surface here (a placement drawn at the wrong height): the terrain's is the ground
            hint = terrain;
            found = nav_->ground_height(a->x, a->y, hint, &gz, 1500.0, 1500.0);
        }
        // a creature whose data says SnapToGround (`snaps_to_ground`) with no floor within the tight reach: the ground nearest the height the data gave it, within the open world's reach, is its floor (the
        // data draws a town's wolves 4 to 13 m over its street and ten of its animals under it).  A flyer is only lifted out of the ground, never pulled down out of the air.
        if (!found && a->npc.snaps_to_ground) {
            double wide = 0;
            if (nav_->ground_height(a->x, a->y, hint, &wide, 1500.0, 1500.0) && (!a->npc.flies || wide >= hint)) {
                gz = wide;
                found = true;
            }
        }
    }
    n.grounded = found;
    n.data_feet = hint;
    n.ground_dz = found ? gz - hint : 0.0;
    if (!found) {
        // no ground there (outside the extracted area, or over a hole): it keeps its height and stands
        n.mover.place(a->x, a->y, hint, setup.home_yaw);
        n.mode = Mode::Stand;
        n.home = {a->x, a->y, hint};
        return false;
    }
    n.mover.place(a->x, a->y, gz, setup.home_yaw);
    n.home = {a->x, a->y, gz};
    switch (setup.ambient) {
        case Ambient::Stationary:
            n.mode = Mode::Stand;
            break;
        case Ambient::Wander:
            n.mode = Mode::Idle;
            n.timer = cfg_.min_idle + next_unit(n) * (cfg_.max_idle - cfg_.min_idle) * 0.5;
            break;
        case Ambient::Patrol:
            if (setup.route.size() >= 2) {
                n.mode = Mode::PatrolPause;
                n.timer = next_unit(n) * 3.0;
                n.wp = 0;
                if (setup.start_closest) {
                    double best = 1e30;
                    for (size_t i = 0; i < setup.route.size(); ++i) {
                        const double d = dist2d(setup.route[i], n.home);
                        if (d < best) { best = d; n.wp = i; }
                    }
                }
                n.wp_dir = 1;
                n.first_leg = true;   // it walks to its start (the nearest waypoint, or the first) and goes on from there
            } else {
                n.mode = Mode::Stand;
            }
            break;
    }
    // stands on the ground now
    const double yaw = n.mover.yaw_degrees();
    registry_.move(n.id, a->x, a->y, n.mover.origin_z(), yaw);
    write_replicated(*a, a->x, a->y, n.mover.origin_z(), yaw, {}, false, now_);
    return true;
}

bool NpcWorld::ground_by_data_at(double x, double y, double hint, double* gz) const {
    if (nav_->ground_height(x, y, hint, gz, cfg_.data_ground_below_cm, cfg_.data_ground_above_cm)) return true;
    // within the agent's radius of a wall the map has no polygon at the exact place (a villager posted at a stall): the floor of the nearest polygon within a small radius, in the same window
    const double lo = hint - cfg_.data_ground_below_cm, hi = hint + cfg_.data_ground_above_cm;
    nav::Vec3 stand;
    if (!nav_->nearest_point({x, y, 0.5 * (lo + hi)}, cfg_.data_ground_near_cm, 0.5 * (hi - lo), &stand) || std::hypot(stand.x - x, stand.y - y) > cfg_.data_ground_near_cm || stand.z < lo || stand.z > hi)
        return false;   // (the nearest polygon is found by bounding boxes: it may be farther than the radius)
    *gz = stand.z;
    return true;
}

NpcWorld::StandReport NpcWorld::stand_report(const std::vector<uint64_t>& ids) const {
    StandReport r;
    for (const uint64_t id : ids) {
        const auto it = npcs_.find(id);
        if (it == npcs_.end()) continue;
        const Npc& n = it->second;
        ++r.people;
        if (n.pending_ground) {
            ++r.waiting;
            continue;
        }
        if (n.grounded) {
            if (std::fabs(n.ground_dz) <= kStandNearCm) {
                ++r.on_floor;
            } else {
                ++r.moved;
                r.max_moved_cm = std::max(r.max_moved_cm, std::fabs(n.ground_dz));
            }
            continue;
        }
        ++r.no_floor;
        if (n.setup.ambient != Ambient::Stationary) ++r.stuck;
        // no floor in reach: is there any walkable ground near it at all?  If not it stands in a wall, a stall, under a floor or in the air
        const world::WorldActor* a = registry_.find(id);
        nav::Vec3 near_ground;
        if (a) {
            const bool ground_near = nav_->nearest_point({a->x, a->y, n.data_feet + 0.5 * (kBlockedAboveCm - kBlockedBelowCm)}, kBlockedCm, 0.5 * (kBlockedAboveCm + kBlockedBelowCm), &near_ground) &&
                                     std::hypot(near_ground.x - a->x, near_ground.y - a->y) <= kBlockedCm && near_ground.z >= n.data_feet - kBlockedBelowCm && near_ground.z <= n.data_feet + kBlockedAboveCm;
            if (!ground_near) ++r.blocked;
        }
    }
    return r;
}

void NpcWorld::chunks_resident(const std::vector<nav::ChunkKey>& chunks) {
    for (const nav::ChunkKey& key : chunks) {
        const auto it = pending_by_chunk_.find(key);
        if (it == pending_by_chunk_.end()) continue;
        for (const uint64_t id : it->second) {
            const auto n = npcs_.find(id);
            if (n == npcs_.end() || !n->second.pending_ground) continue;   // killed and removed meanwhile
            world::WorldActor* a = registry_.find(id);
            if (!a) continue;
            n->second.pending_ground = false;
            n->second.pending_ground_done = true;   // (ground_npc reads the height the data gave, not the terrain's it stood on meanwhile)
            if (pending_count_ > 0) --pending_count_;
            const double z_before = a->z;
            ground_npc(n->second, *a);
            if (!n->second.relocated) n->second.spawn_home = n->second.home;   // (its home as grounded now is where it was made)
            // (the investigation's D1) a view that holds this NPC as a Snapshot or Dormant actor sends it again only when a reliable change happened: grounding that moves it by more
            // than a hand's breadth (a floor under it, a data height that was not the terrain's) is one, or the client keeps showing it at the terrain's height until it wakes
            if (std::fabs(a->z - z_before) > kRegroundReliableCm) registry_.touch(*a, world::Change::State);
        }
        pending_by_chunk_.erase(it);
    }
}
// CoA captures, C_Y_B code

std::vector<std::pair<double, double>> NpcWorld::player_points(double now) const {
    std::vector<std::pair<double, double>> pts;
    for (const auto& [id, p] : players_)
        if (p.at >= now - cfg_.player_ttl) pts.emplace_back(p.x, p.y);
    return pts;
}

void NpcWorld::note_player(uint64_t player, double x, double y, double now) {
    if (!std::isfinite(x) || !std::isfinite(y)) return;
    players_[player] = {x, y, now};
}

void NpcWorld::forget_player(uint64_t player) { players_.erase(player); }

bool NpcWorld::awake_for_someone(const world::WorldActor& a) const {
    const double r = (a.awake_radius_cm > 0 ? a.awake_radius_cm : cfg_.awake_cm) + cfg_.awake_margin_cm;
    for (const auto& [id, p] : players_) {
        if (p.at < now_ - cfg_.player_ttl) continue;
        if (std::hypot(p.x - a.x, p.y - a.y) <= r) return true;
    }
    return false;
}

void NpcWorld::refresh_active(double t) {
    std::vector<uint64_t> next;
    std::vector<uint64_t> near;
    for (const auto& [pid, p] : players_) {
        if (p.at < t - cfg_.player_ttl) continue;
        near.clear();
        registry_.query(p.x, p.y, cfg_.awake_cm + cfg_.awake_margin_cm + 6000.0, near);   // (a large creature's awake radius may be more than the default)
        for (const uint64_t id : near) {
            const auto it = npcs_.find(id);
            // (an NPC that stands, and has no turning to do, is not stepped)
            if (it == npcs_.end() || (it->second.mode == Mode::Stand && !it->second.turning && !it->second.track) || it->second.pending_ground) continue;
            const world::WorldActor* a = registry_.find(id);
            // (an NPC whose chunk of the ground has gone out of the window sleeps where it stands: nothing is there to walk on)
            if (a && awake_for_someone(*a) && nav_->ready_at(a->x, a->y)) next.push_back(id);
        }
    }
    std::sort(next.begin(), next.end());
    next.erase(std::unique(next.begin(), next.end()), next.end());
    // the ones that left every ring stop where they are, and say so
    for (const uint64_t id : active_) {
        if (std::binary_search(next.begin(), next.end(), id)) continue;
        Npc& n = npcs_.find(id)->second;
        n.active = false;
        if (n.reported_moving) commit(n, t, true);
    }
    for (const uint64_t id : next) npcs_.find(id)->second.active = true;
    active_ = std::move(next);
}

void NpcWorld::tick(double now) {
    if (!std::isfinite(now) || now < now_) return;
    now_ = now;
    if (next_step_ < 0) {
        next_step_ = now;
        // the NPCs' timestamps began at 0 (the loader's clock): the clock they will be moved on starts here, so that no NPC's first move jumps by the age
        // of the server and an open made before it carries a time the client can follow.  (The registry's version is not bumped: nothing is to be sent.)
        for (auto& [id, n] : npcs_) {
            world::WorldActor* a = registry_.find(id);
            if (a && a->actor.obj.schema && a->actor.obj.schema->command("ReplicatedServerLastTransformUpdateTimeStamp"))
                a->actor.obj.state.set(*a->actor.obj.schema, "ReplicatedServerLastTransformUpdateTimeStamp", repl::Value::f32(static_cast<float>(now)));
        }
    }
    int steps = 0;
    while (next_step_ <= now && steps < cfg_.max_catch_up_steps) {
        step(next_step_);
        next_step_ += cfg_.step_seconds;
        ++steps;
    }
    if (next_step_ <= now) next_step_ = now + cfg_.step_seconds;   // too far behind: no catching up
}

void NpcWorld::step(double t) {
    const auto step_start = std::chrono::steady_clock::now();
    pathfinds_this_step_ = 0;
    iterations_this_step_ = 0;
    ++stats_.steps;
    refresh_active(t);
    stats_.max_refresh_ms = std::max(stats_.max_refresh_ms, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - step_start).count());
    // the step's path-finding budget goes round: the NPC the walk starts from is a different one each step, so that no NPC (a patrol that cannot find its way,
    // say) can keep the rest from ever being asked
    const size_t count = active_.size();
    const size_t first = count ? static_cast<size_t>(stats_.steps % count) : 0;
    for (size_t i = 0; i < count; ++i) {
        Npc& n = npcs_.find(active_[(first + i) % count])->second;
        if (!n.rm) advance_npc(n, t);   // (a creature a montage carries is moved by the montage)
    }
    const std::vector<uint64_t> carried = rooted_;   // (finishing a montage takes the NPC out of the list)
    for (const uint64_t id : carried) {
        const auto it = npcs_.find(id);
        if (it != npcs_.end() && it->second.rm) advance_root_motion(it->second, t);
    }
    const std::vector<uint64_t> settling = settling_;   // (a settle ends by taking the NPC out of the list)
    for (const uint64_t id : settling) {
        const auto it = npcs_.find(id);
        if (it != npcs_.end()) settle(it->second, t);
    }
    stats_.max_step_ms = std::max(stats_.max_step_ms, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - step_start).count());
    stats_.max_active = std::max(stats_.max_active, count);
    stats_.max_pathfinds_in_step = std::max(stats_.max_pathfinds_in_step, pathfinds_this_step_);
}
// -- CoA . C | Y | B --

void NpcWorld::go_idle(Npc& n) {
    n.mover.stop();
    n.ordered_return = false;
    switch (n.setup.ambient) {
        case Ambient::Wander: {
            n.mode = Mode::Idle;
            const double base = n.setup.wander_delay * cfg_.idle_per_delay, spread = n.setup.wander_delay_deviation * cfg_.idle_spread;
            n.timer = std::clamp(base + (next_unit(n) * 2.0 - 1.0) * spread, cfg_.min_idle, cfg_.max_idle);
            break;
        }
        case Ambient::Patrol:
            if (n.setup.route.size() >= 2) { n.mode = Mode::PatrolPause; n.timer = 0.2; break; }
            [[fallthrough]];
        default:
            n.mode = Mode::Stand;
            break;
    }
}

void NpcWorld::start_wander(Npc& n) {
    const NpcMotionSetup& s = n.setup;
    const double lo = std::max(0.0, s.wander_min), hi = std::max(lo + 1.0, s.wander_max);
    const double median = std::clamp(s.leg_median, 50.0, hi), p90 = std::max(s.leg_p90, median * 1.2);
    const double sigma = std::log(p90 / median) / 1.2815515655446004;   // log-normal: the median and the 90th percentile
    ++stats_.wander_asks;
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (pathfinds_this_step_ >= cfg_.max_pathfinds_per_step || iterations_this_step_ >= cfg_.max_search_iterations_per_step) { n.timer = cfg_.step_seconds; ++stats_.wander_deferred; return; }   // decide again next step
        const double z = (next_unit(n) + next_unit(n) + next_unit(n) + next_unit(n) - 2.0) * 1.7320508075688772;   // ~ N(0, 1)
        const double leg = std::clamp(median * std::exp(sigma * z), std::min(lo, 100.0) + 50.0, hi);
        const nav::Vec3 here = n.mover.ground();
        double a = next_unit(n) * 6.283185307179586;
        nav::Vec3 want{here.x + std::cos(a) * leg, here.y + std::sin(a) * leg, here.z};
        if (std::hypot(want.x - n.home.x, want.y - n.home.y) > hi) {
            // a leg that would leave the radius turns back towards home
            a = std::atan2(n.home.y - here.y, n.home.x - here.x) + (next_unit(n) - 0.5) * 1.5707963267948966;
            const double back = std::min(leg, std::max(dist2d(here, n.home), 150.0));
            want = {here.x + std::cos(a) * back, here.y + std::sin(a) * back, here.z};
        }
        nav::Vec3 np;
        if (!nav_->nearest_point(want, 150, 300, &np) || std::hypot(np.x - n.home.x, np.y - n.home.y) > hi + 100.0) { ++stats_.wander_no_point; continue; }
        ++pathfinds_this_step_;
        const double straight = dist2d(n.mover.ground(), np);
        if (straight < 50.0) continue;
        const bool walks = n.mover.move_to(np, s.walk_speed, 0, straight * cfg_.max_wander_path_factor + 500.0);
        iterations_this_step_ += n.mover.last_search_iterations();
        if (walks) {
            n.mode = Mode::Wander;
            n.fails = 0;
            ++stats_.moves_started;
            return;
        }
        ++stats_.no_path;
        ++stats_.wander_no_path;
        ++stats_.wander_fail[static_cast<size_t>(n.mover.last_fail())];
    }
    ++n.fails;
    n.mode = Mode::Idle;
    n.timer = std::min(30.0, 2.0 + 2.0 * n.fails);   // nowhere to go from here: try again later
}

void NpcWorld::start_patrol_leg(Npc& n) {
    const NpcMotionSetup& s = n.setup;
    const size_t count = s.route.size();
    ++stats_.patrol_asks;
    for (size_t tries = 0; tries < count; ++tries) {
        if (pathfinds_this_step_ >= cfg_.max_pathfinds_per_step || iterations_this_step_ >= cfg_.max_search_iterations_per_step) { n.timer = cfg_.step_seconds; n.mode = Mode::PatrolPause; ++stats_.patrol_deferred; return; }
        // the next waypoint: round a loop, or there and back (the first leg of all goes to the start itself)
        if (n.first_leg) {
            // n.wp is the start
        } else if (s.loop) {
            n.wp = (n.wp + 1) % count;
        } else {
            if (n.wp_dir > 0 && n.wp + 1 >= count) n.wp_dir = -1;
            else if (n.wp_dir < 0 && n.wp == 0) n.wp_dir = 1;
            n.wp = static_cast<size_t>(static_cast<long long>(n.wp) + n.wp_dir);
        }
        ++pathfinds_this_step_;
        const bool walks = n.mover.move_to(s.route[n.wp], s.patrol_speed, 0, 0);
        iterations_this_step_ += n.mover.last_search_iterations();
        if (walks) {
            n.first_leg = false;
            n.mode = Mode::Patrol;
            n.fails = 0;
            ++stats_.moves_started;
            return;
        }
        ++stats_.no_path;   // a waypoint nobody can walk to: the next one
        ++stats_.patrol_no_path;
        ++stats_.patrol_fail[static_cast<size_t>(n.mover.last_fail())];
        n.first_leg = false;
    }
    ++n.fails;
    n.mode = Mode::PatrolPause;
    n.timer = std::min(60.0, 5.0 * n.fails);
}

void NpcWorld::decide(Npc& n, double) {
    switch (n.mode) {
        case Mode::Idle: {
            // too far from home (pushed, or a stage-3 chase ended): back, at a run
            const double far = std::max(2.0 * n.setup.wander_max, 1500.0);
            if (dist2d(n.mover.ground(), n.home) > far) {
                if (n.mover.move_to(n.home, n.setup.run_speed, 0, 0)) {
                    n.mode = Mode::Return;
                    n.ordered_return = false;
                    ++stats_.returns_home;
                    return;
                }
            }
            start_wander(n);
            break;
        }
        case Mode::PatrolPause:
            start_patrol_leg(n);
            break;
        default:
            break;
    }
}
// ~ C-O-A :: Cyb ~

void NpcWorld::commit(Npc& n, double t, bool force) {
    world::WorldActor* a = registry_.find(n.id);
    if (!a) return;
    const nav::Vec3 g = n.mover.ground();
    const double z = n.mover.origin_z(), yaw = n.mover.yaw_degrees();
    const bool moving = n.mover.moving() && n.active;
    const nav::Vec3 vel = moving ? n.mover.velocity() : nav::Vec3{};
    const bool moved = std::fabs(a->x - g.x) > 1e-3 || std::fabs(a->y - g.y) > 1e-3 || std::fabs(a->z - z) > 1e-3 || std::fabs(a->yaw_degrees - yaw) > 1e-3;
    if (!force && !moved && moving == n.reported_moving) return;
    registry_.move(n.id, g.x, g.y, z, yaw);
    write_replicated(*a, g.x, g.y, z, yaw, vel, moving, t);
    n.reported_moving = moving;
    ++stats_.writes;
}

void NpcWorld::steer(Npc& n, double t) {
    const double dt = cfg_.step_seconds;
    bool turned = false;
    if (n.track) {
        nav::Vec3 at;
        if (n.track_until > 0 && t >= n.track_until) {
            n.track = nullptr;
        } else if (n.track(at)) {
            const nav::Vec3 g = n.mover.ground();
            if (std::hypot(at.x - g.x, at.y - g.y) > 1.0) {
                n.mover.turn_toward(std::atan2(at.y - g.y, at.x - g.x) * 180.0 / 3.14159265358979323846, dt, n.track_rate);
                turned = true;
            }
        }
    } else if (n.turning && !n.mover.moving()) {
        if (n.mover.turn_toward(n.turn_yaw, dt, n.turn_rate)) n.turning = false;
        turned = true;
    }
    if (turned) commit(n, t, false);
}

void NpcWorld::pursue_step(Npc& n, double) {
    if (!n.pursue) return;
    nav::Vec3 at;
    if (n.mode != Mode::Ordered || !n.pursue(at)) {   // another order took over, or the target is gone: the AI decides what next
        n.pursue = nullptr;
        return;
    }
    const nav::Vec3 g = n.mover.ground();
    if (std::hypot(at.x - g.x, at.y - g.y) <= n.pursue_stop) return;   // where it wants to be: the AI judges its range
    // on its way, and the target has not gone far from where the way was found to lead: the way is kept and its end follows the target
    if (n.mover.moving() && std::hypot(at.x - n.pursue_plan_x, at.y - n.pursue_plan_y) < 200.0 && n.mover.set_end(at.x, at.y)) {
        n.pursue_x = at.x;
        n.pursue_y = at.y;
        return;
    }
    if (pathfinds_this_step_ >= cfg_.max_pathfinds_per_step || iterations_this_step_ >= cfg_.max_search_iterations_per_step) return;   // the next step
    ++pathfinds_this_step_;
    const bool found = n.mover.move_to(at, n.pursue_speed, n.pursue_stop, 0);
    iterations_this_step_ += n.mover.last_search_iterations();
    n.pursue_x = n.pursue_plan_x = at.x;
    n.pursue_y = n.pursue_plan_y = at.y;
    if (!found) n.pursue = nullptr;   // no way to the target: the AI's next order tries again (a search that fails is not repeated at every step)
}

void NpcWorld::advance_npc(Npc& n, double t) {
    const double dt = cfg_.step_seconds;
    pursue_step(n, t);
    if (n.mode == Mode::Stand) {
        steer(n, t);
        return;
    }
    if (n.mode == Mode::Idle || n.mode == Mode::PatrolPause) {
        n.timer -= dt;
        if (n.timer <= 0) decide(n, t);
        if (n.mode == Mode::Idle || n.mode == Mode::PatrolPause) {
            if (n.reported_moving) commit(n, t, false);   // (an NPC that was walking when it came to rest says it stands)
            return;
        }
        // a walk began: its first step is this step's
    }
    const Mover::Status st = n.mover.advance(dt);
    steer(n, t);
    commit(n, t, false);
    if (st == Mover::Status::Arrived) {
        if (n.mode == Mode::Ordered) {   // the AI's order is done: it stands until it is given another or released
            n.mover.stop();
            return;
        }
        if (n.mode == Mode::Patrol) {
            const size_t i = n.wp;
            const double pause = i < n.setup.pauses.size() ? std::max(0.0, n.setup.pauses[i]) : 0.0;
            n.mover.stop();
            n.mode = Mode::PatrolPause;
            n.timer = pause;
        } else {
            go_idle(n);
        }
    } else if (st == Mover::Status::Blocked || st == Mover::Status::Idle) {
        if (n.mode != Mode::Ordered) go_idle(n);
    }
}

bool NpcWorld::order_move(uint64_t id, const nav::Vec3& dest, double speed_cm_s, double stop_distance) {
    const auto it = npcs_.find(id);
    if (it == npcs_.end()) return false;
    Npc& n = it->second;
    if (n.rm) finish_root_motion(n, now_);   // (an order takes the feet back from the montage)
    n.turning = false;
    n.pursue = nullptr;
    if (!n.mover.move_to(dest, speed_cm_s, stop_distance, 0)) return false;
    n.mode = Mode::Ordered;
    ++stats_.moves_started;
    return true;
}

bool NpcWorld::release(uint64_t id) {
    const auto it = npcs_.find(id);
    if (it == npcs_.end() || it->second.mode != Mode::Ordered) return false;
    it->second.pursue = nullptr;
    go_idle(it->second);
    return true;
}

bool NpcWorld::halt(uint64_t id) {
    const auto it = npcs_.find(id);
    if (it == npcs_.end()) return false;
    Npc& n = it->second;
    if (n.rm) finish_root_motion(n, now_);
    n.turning = false;
    n.pursue = nullptr;
    n.track = nullptr;
    n.mover.stop();
    n.mode = Mode::Stand;
    n.ordered_return = false;
    commit(n, now_, true);
    return true;
}

bool NpcWorld::face_toward(uint64_t id, double x, double y) {
    const auto it = npcs_.find(id);
    if (it == npcs_.end()) return false;
    const world::WorldActor* a = registry_.find(id);
    if (!a) return false;
    const double dx = x - a->x, dy = y - a->y;
    if (std::fabs(dx) < 1e-6 && std::fabs(dy) < 1e-6) return true;
    Npc& n = it->second;
    if (n.rm) return false;   // a montage carries it in the facing it began with
    n.mover.face(std::atan2(dy, dx) * 180.0 / 3.14159265358979323846);
    commit(n, now_, true);
    return true;
}

bool NpcWorld::turn_toward(uint64_t id, double x, double y, double rate) {
    const auto it = npcs_.find(id);
    if (it == npcs_.end() || !std::isfinite(x) || !std::isfinite(y)) return false;
    const world::WorldActor* a = registry_.find(id);
    if (!a) return false;
    const double dx = x - a->x, dy = y - a->y;
    if (std::fabs(dx) < 1e-6 && std::fabs(dy) < 1e-6) return true;
    Npc& n = it->second;
    if (n.rm) return false;   // a montage carries it in the facing it has
    n.turning = true;
    n.turn_yaw = std::atan2(dy, dx) * 180.0 / 3.14159265358979323846;
    n.turn_rate = std::isfinite(rate) && rate > 0 ? rate : 0.0;
    return true;
}

bool NpcWorld::track_target(uint64_t id, Where where, double rate, double until) {
    const auto it = npcs_.find(id);
    if (it == npcs_.end() || !where) return false;
    Npc& n = it->second;
    n.track = std::move(where);
    n.track_rate = std::isfinite(rate) && rate > 0 ? rate : 0.0;
    n.track_until = std::isfinite(until) ? until : 0.0;
    return true;
}

void NpcWorld::stop_tracking(uint64_t id) {
    const auto it = npcs_.find(id);
    if (it != npcs_.end()) it->second.track = nullptr;
}

bool NpcWorld::order_pursue(uint64_t id, Where where, double speed_cm_s, double stop_distance) {
    const auto it = npcs_.find(id);
    if (it == npcs_.end() || !where) return false;
    Npc& n = it->second;
    nav::Vec3 at;
    if (!where(at)) return false;
    if (!order_move(id, at, speed_cm_s, stop_distance)) return false;
    n.pursue = std::move(where);
    n.pursue_speed = speed_cm_s;
    n.pursue_stop = std::max(0.0, stop_distance);
    n.pursue_x = n.pursue_plan_x = at.x;
    n.pursue_y = n.pursue_plan_y = at.y;
    return true;
}

bool NpcWorld::order_return_home(uint64_t id) {
    const auto it = npcs_.find(id);
    if (it == npcs_.end()) return false;
    Npc& n = it->second;
    if (n.rm) finish_root_motion(n, now_);
    n.turning = false;
    n.pursue = nullptr;
    n.track = nullptr;
    if (!n.mover.move_to(n.home, n.setup.run_speed, 0, 0)) return false;
    n.mode = Mode::Return;
    n.ordered_return = true;
    ++stats_.returns_home;
    return true;
}

bool NpcWorld::in_freeze_box(double cx, double cy, double yaw_degrees, double distance, double wide, double bx, double by, double body) {
    if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(bx) || !std::isfinite(by)) return false;
    constexpr double kPi = 3.14159265358979323846;
    const double fx = std::cos(yaw_degrees * kPi / 180.0), fy = std::sin(yaw_degrees * kPi / 180.0);
    const double ex = bx - cx, ey = by - cy;
    const double ahead = ex * fx + ey * fy, aside = -ex * fy + ey * fx;
    return ahead >= -body && ahead <= distance + wide + body && std::fabs(aside) <= wide + body;
}

bool NpcWorld::begin_root_motion(uint64_t id, RootMotionPlay play) {
    const auto it = npcs_.find(id);
    if (it == npcs_.end() || it->second.pending_ground || play.steps.empty() || !(play.length > 0) || !std::isfinite(play.length) || !std::isfinite(play.rate) || !std::isfinite(play.blend_out)) return false;
    for (const RootMotionStep& s : play.steps) {
        if (!std::isfinite(s.time) || !std::isfinite(s.x) || !std::isfinite(s.y)) return false;
    }
    Npc& n = it->second;
    if (!registry_.find(id)) return false;
    if (n.rm) finish_root_motion(n, now_);   // (one montage at a time: the one before is over)
    n.turning = false;
    n.pursue = nullptr;
    RootMotion r;
    r.play = std::move(play);
    r.start = now_;
    r.yaw = n.mover.yaw_degrees();
    const nav::Vec3 g = n.mover.ground();
    r.ox = g.x;
    r.oy = g.y;
    n.rm = std::move(r);
    rooted_.push_back(id);
    // the montage is on its first frame: the actor says so in the update that carries the attack naming it
    const double z = n.mover.origin_z(), yaw = n.mover.yaw_degrees();
    const uint64_t montage = n.rm->play.montage;
    std::optional<uint64_t> saved;
    registry_.modify(id, world::Change::Movement, [&](world::WorldActor& a) {
        saved = swap_movement_mode(a, repl::kMoveWalking);
        write_root_motion(a, true, montage, 0.0, g.x, g.y, z, yaw, {});
    });
    n.rm->saved_mode = saved;
    return true;
}

bool NpcWorld::end_root_motion(uint64_t id) {
    const auto it = npcs_.find(id);
    if (it == npcs_.end() || !it->second.rm) return false;
    finish_root_motion(it->second, now_);
    return true;
}

bool NpcWorld::root_motion_active(uint64_t id) const {
    const auto it = npcs_.find(id);
    return it != npcs_.end() && it->second.rm.has_value();
}

void NpcWorld::advance_root_motion(Npc& n, double t) {
    RootMotion& r = *n.rm;
    const RootMotionPlay& p = r.play;
    world::WorldActor* a = registry_.find(n.id);
    if (!a) {
        n.rm.reset();
        rooted_.erase(std::remove(rooted_.begin(), rooted_.end(), n.id), rooted_.end());
        return;
    }
    const double dt = cfg_.step_seconds;
    const double rate = p.rate > 0 ? p.rate : 1.0;
    const double carry_end = std::max(0.0, p.length - std::max(0.0, p.blend_out));   // the montage carries the creature until it starts to blend out
    const double reached = std::max(0.0, t - r.start) * rate;
    const bool last = reached >= carry_end;
    const double at = last ? carry_end : reached;
    constexpr double kPi = 3.14159265358979323846;
    const nav::Vec3 g = n.mover.ground();
    // A creature that tracks its target turns while the montage carries it (its rate, no jumps), and the rest of the montage's travel goes the way it faces then: the travel so far stays
    // where it is, and the travel still to come is turned about the creature's place.
    if (n.track && n.track_until > 0 && t >= n.track_until) n.track = nullptr;
    if (n.track) {
        nav::Vec3 at_target;
        if (n.track(at_target) && std::hypot(at_target.x - g.x, at_target.y - g.y) > 1.0) {
            n.mover.turn_toward(std::atan2(at_target.y - g.y, at_target.x - g.x) * 180.0 / kPi, dt, n.track_rate);
        }
    }
    if (std::fabs(n.mover.yaw_degrees() - r.yaw) > 1e-9) {
        r.yaw = n.mover.yaw_degrees();
        r.ox = g.x - (std::cos(r.yaw * kPi / 180.0) * r.last_tx - std::sin(r.yaw * kPi / 180.0) * r.last_ty);
        r.oy = g.y - (std::sin(r.yaw * kPi / 180.0) * r.last_tx + std::cos(r.yaw * kPi / 180.0) * r.last_ty);
    }
    // where the montage's root is now (ahead of the creature and to its right, from where it stood), and so where the creature would be with nothing in its way: the root's travel less the part
    // a pause made it skip (the client's copy takes the montage's travel tick by tick, so what it did not take while it was held it does not take later)
    double rx = 0, ry = 0;
    root_at(p.steps, at, &rx, &ry);
    r.position = at;
    const double fx = std::cos(r.yaw * kPi / 180.0), fy = std::sin(r.yaw * kPi / 180.0);
    const double tx = rx - r.skip_x, ty = ry - r.skip_y;
    double nx = r.ox + fx * tx - fy * ty, ny = r.oy + fy * tx + fx * ty;
    std::vector<RootMotionBlock> blocks;
    if (p.blockers) p.blockers(blocks);
    // the freeze box (the ability record's RootMotionFreeze): a body in the box ahead of the creature ends the lunge for good (StopOnContact; a lunge that begins with one in it does not begin) or
    // holds the creature where it is for as long as the body stays in it (PauseOnContact)
    const double wide = p.freeze_scale * p.own_radius;
    const auto in_box = [&](double cx, double cy) {
        if (!p.freeze_stop && !p.freeze_pause) return false;
        for (const RootMotionBlock& b : blocks) {
            if (p.ignore_non_target && !b.target) continue;
            if (in_freeze_box(cx, cy, r.yaw, p.freeze_distance, wide, b.x, b.y, b.body)) return true;
        }
        return false;
    };
    bool obstructed = false;   // bRootMotionObstructed: the creature is held by the box in this update
    if (p.freeze_stop && !r.frozen && in_box(g.x, g.y)) r.frozen = true;
    if (r.frozen) {
        nx = g.x;
        ny = g.y;
        obstructed = true;
    } else if (p.freeze_pause) {
        // How far along this step the creature is in the box: 0 where it is already, a fraction where the step brings it to the box's edge (the first point of the way, by sixteenths and then
        // by halving, that is inside), and none (2) where the step ends outside it.
        double entry = 2.0;
        if (in_box(g.x, g.y)) {
            entry = 0.0;
        } else {
            constexpr int kSub = 16;
            for (int i = 1; i <= kSub && entry > 1.0; ++i) {
                const double hi_f = static_cast<double>(i) / kSub;
                if (!in_box(g.x + (nx - g.x) * hi_f, g.y + (ny - g.y) * hi_f)) continue;
                double lo = static_cast<double>(i - 1) / kSub, hi = hi_f;
                for (int k = 0; k < 14; ++k) {
                    const double mid = 0.5 * (lo + hi);
                    (in_box(g.x + (nx - g.x) * mid, g.y + (ny - g.y) * mid) ? hi : lo) = mid;
                }
                entry = hi;
            }
        }
        if (entry <= 1.0) {
            // held: the creature goes no farther than the box's edge (its velocity is zero from the next step on), and the montage's travel from there to where the root is now is the part it skips
            nx = g.x + (nx - g.x) * entry;
            ny = g.y + (ny - g.y) * entry;
            obstructed = true;
            const double ex = nx - r.ox, ey = ny - r.oy;
            r.skip_x = rx - (ex * fx + ey * fy);
            r.skip_y = ry - (-ex * fy + ey * fx);
        }
    }
    r.last_tx = rx - r.skip_x;
    r.last_ty = ry - r.skip_y;
    // it goes from where it is towards that place, and not into the bodies it is told to keep out of: a body in the way stops the step where they touch (the recorded creatures stop at the
    // player's capsule and stay there, they do not slide round it and do not step back when the animation does); one it already overlaps is not pushed out of, it only does not get deeper
    for (const RootMotionBlock& b : blocks) {
        if (!std::isfinite(b.x) || !std::isfinite(b.y) || !(b.radius > 0)) continue;
        // the way from where it is to where the montage wants it, up to the first point at which the creature's centre comes to `keep` cm of the body's (a body it overlaps keeps the
        // distance it has: it is on that circle already)
        const double sx = g.x - b.x, sy = g.y - b.y, dx = nx - g.x, dy = ny - g.y;
        const double a2 = dx * dx + dy * dy, was2 = sx * sx + sy * sy;
        if (a2 < 1e-12) continue;
        const double keep = std::min(b.radius, std::sqrt(was2));
        const double bq = sx * dx + sy * dy, cq = was2 - keep * keep, disc = bq * bq - a2 * cq;
        if (bq >= 0 || disc < 0) continue;   // the way leads away from the body, or passes it
        const double f = std::max(0.0, (-bq - std::sqrt(disc)) / a2);   // (a creature on the circle already meets it at 0, whatever the rounding says)
        if (f >= 1) continue;   // the way stops short of the body
        nx = g.x + dx * f;
        ny = g.y + dy * f;
    }
    // ... nor off the ground it can walk on
    nav::Vec3 to{nx, ny, g.z};
    if (nav_->on_navmesh(g)) {
        nav::Vec3 edge;
        if (!nav_->raycast(g, to, &edge)) {
            to.x = edge.x;
            to.y = edge.y;
        }
    }
    double gz = g.z;
    const double window = 80.0 + 2.0 * std::hypot(to.x - g.x, to.y - g.y);
    if (nav_->ground_height(to.x, to.y, g.z, &gz, window, window)) {
        to.z = gz;
    } else {
        to = g;   // no ground there: it stays
    }
    if (p.freeze_stop && !r.frozen && in_box(to.x, to.y)) r.frozen = true;
    n.mover.shift(to.x, to.y, to.z);
    const double z = n.mover.origin_z(), yaw = n.mover.yaw_degrees();
    registry_.move(n.id, to.x, to.y, z, yaw);
    const nav::Vec3 vel = last ? nav::Vec3{} : nav::Vec3{(to.x - g.x) / dt, (to.y - g.y) / dt, (to.z - g.z) / dt};
    write_replicated(*a, to.x, to.y, z, yaw, vel, false, t);
    write_root_motion(*a, !last, p.montage, at, to.x, to.y, z, yaw, vel, obstructed && !last);
    n.reported_moving = false;
    ++stats_.writes;
    if (last) {
        if (r.saved_mode) swap_movement_mode(*a, *r.saved_mode);
        registry_.touch(*a, world::Change::State);   // the end, with the place the creature stopped at, must not be lost: it goes on a reliable chain
        n.rm.reset();
        rooted_.erase(std::remove(rooted_.begin(), rooted_.end(), n.id), rooted_.end());
        begin_settle(n, t);
    }
}

void NpcWorld::finish_root_motion(Npc& n, double t) {
    if (!n.rm) return;
    const double position = n.rm->position;
    const std::optional<uint64_t> saved_mode = n.rm->saved_mode;
    n.rm.reset();
    rooted_.erase(std::remove(rooted_.begin(), rooted_.end(), n.id), rooted_.end());
    world::WorldActor* a = registry_.find(n.id);
    if (!a) return;
    if (saved_mode) swap_movement_mode(*a, *saved_mode);
    const nav::Vec3 g = n.mover.ground();
    const double z = n.mover.origin_z(), yaw = n.mover.yaw_degrees();
    registry_.move(n.id, g.x, g.y, z, yaw);
    write_replicated(*a, g.x, g.y, z, yaw, {}, false, t);
    write_root_motion(*a, false, 0, position, g.x, g.y, z, yaw, {});
    registry_.touch(*a, world::Change::State);   // (reliable, as the montage's own end)
    n.reported_moving = false;
    ++stats_.writes;
    begin_settle(n, t);
}

void NpcWorld::begin_settle(Npc& n, double t) {
    if (!(cfg_.settle_seconds > 0)) return;
    n.settle_began = t;
    n.settle_until = t + cfg_.settle_seconds;
    n.settle_sent = 0;
    if (std::find(settling_.begin(), settling_.end(), n.id) == settling_.end()) settling_.push_back(n.id);
}

void NpcWorld::settle(Npc& n, double t) {
    if (t <= n.settle_began + 1e-9) return;   // (the step the montage let go in has the end written already: the first re-send is the next step's)
    world::WorldActor* a = registry_.find(n.id);
    // over: the actor is gone, a montage has taken the creature again, it walks (its walking says where it is), or the time is up
    if (!a || n.rm || n.reported_moving || t > n.settle_until + 1e-9) {
        settling_.erase(std::remove(settling_.begin(), settling_.end(), n.id), settling_.end());
        return;
    }
    const nav::Vec3 g = n.mover.ground();
    const double z = n.mover.origin_z(), yaw = n.mover.yaw_degrees();
    registry_.move(n.id, g.x, g.y, z, yaw);
    write_replicated(*a, g.x, g.y, z, yaw, {}, false, t, ++n.settle_sent);
    ++stats_.writes;
}

const Mover* NpcWorld::mover(uint64_t id) const {
    const auto it = npcs_.find(id);
    return it == npcs_.end() ? nullptr : &it->second.mover;
}
// C.O.A.C.Y.B

bool NpcWorld::stands(uint64_t id) const {
    const auto it = npcs_.find(id);
    return it == npcs_.end() || it->second.mode == Mode::Stand;
}

bool NpcWorld::is_ordered(uint64_t id) const {
    const auto it = npcs_.find(id);
    return it != npcs_.end() && it->second.mode == Mode::Ordered;
}

nav::Vec3 NpcWorld::home_of(uint64_t id) const {
    const auto it = npcs_.find(id);
    return it == npcs_.end() ? nav::Vec3{} : it->second.home;
}

bool NpcWorld::set_home(uint64_t id, const nav::Vec3& home) {
    const auto it = npcs_.find(id);
    if (it == npcs_.end() || !std::isfinite(home.x) || !std::isfinite(home.y) || !std::isfinite(home.z)) return false;
    it->second.home = home;
    return true;
}

bool NpcWorld::spawn_point(uint64_t id, nav::Vec3* ground_home, nav::Vec3* ai_home) const {
    const auto it = npcs_.find(id);
    if (it == npcs_.end()) return false;
    if (ground_home) *ground_home = it->second.spawn_home;
    if (ai_home) *ai_home = it->second.spawn_ai_home;
    return true;
}

bool NpcWorld::restore_home(uint64_t id, nav::Vec3* ground_home) {
    const auto it = npcs_.find(id);
    if (it == npcs_.end()) return false;
    it->second.home = it->second.spawn_home;
    if (ground_home) *ground_home = it->second.home;
    return true;
}

NpcWorld::RelocateResult NpcWorld::relocate(uint64_t id, double x, double y, double z, bool z_is_origin) {
    RelocateResult r;
    const auto it = npcs_.find(id);
    world::WorldActor* a = registry_.find(id);
    if (it == npcs_.end() || !a) {
        r.status = Relocated::UnknownNpc;
        return r;
    }
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
        r.status = Relocated::NotANumber;
        return r;
    }
    Npc& n = it->second;
    if (n.pending_ground) {
        r.status = Relocated::WaitingForGround;
        return r;
    }
    if (!nav_->ready_at(x, y)) {
        r.status = Relocated::NotLoaded;
        return r;
    }
    const double foot = n.mover.foot_offset();
    double gz = z_is_origin ? z - foot : z;
    if (!z_is_origin) {
        double floor = 0;
        if (nav_->ground_height(x, y, z, &floor, 400.0, 400.0)) gz = floor;   // the walking map's floor near the server's ground, when it has one
    }
    // the feet are taken from what they were doing: a montage that carried it ends, a turn and a look end, a walk is dropped (a chase it was ordered goes on from here)
    if (n.rm) finish_root_motion(n, now_);
    n.turning = false;
    n.track = nullptr;
    n.mover.stop();
    n.mover.place(x, y, gz, n.mover.yaw_degrees());
    n.home = {x, y, gz};
    n.relocated = true;
    if (n.setup.ambient == Ambient::Patrol) n.setup.ambient = Ambient::Wander;   // a route is walked where the data put it: moved, it wanders about its new place
    n.grounded = true;
    n.data_feet = gz;
    n.ground_dz = 0;
    if (n.mode != Mode::Ordered) go_idle(n);
    commit(n, now_, true);
    registry_.touch(*a, world::Change::State);
    r.ground = n.mover.ground();
    r.origin_z = n.mover.origin_z();
    return r;
}

NpcWorld::Stats NpcWorld::stats() const {
    Stats s = stats_;
    s.npcs = npcs_.size();
    s.active_now = active_.size();
    for (const uint64_t id : active_) {
        const auto it = npcs_.find(id);
        if (it != npcs_.end() && it->second.mover.moving()) ++s.moving_now;
    }
    for (const auto& [id, n] : npcs_) {
        switch (n.mode) {
            case Mode::Stand: ++s.standing; break;
            case Mode::Idle: ++s.idling; break;
            case Mode::Wander: ++s.wandering; break;
            case Mode::Patrol: ++s.patrolling; break;
            case Mode::PatrolPause: ++s.patrol_pausing; break;
            case Mode::Return: ++s.returning; break;
            case Mode::Ordered: ++s.ordered; break;
        }
    }
    return s;
}

}  // namespace aoc::worldd::npc
