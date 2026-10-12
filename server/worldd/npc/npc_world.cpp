// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "npc/npc_world.h"

#include "protocol/repl/codecs.h"
#include "protocol/repl/value.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace aoc::worldd::npc {

namespace repl = ::aoc::protocol::repl;

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
void write_replicated(world::WorldActor& a, double x, double y, double z, double yaw, const nav::Vec3& vel, bool moving, double stamp) {
    repl::ObjectState& obj = a.actor.obj;
    if (!obj.schema) return;
    repl::RepMovement m;
    if (const repl::Value* v = obj.state.get(7)) {
        if (const auto* old = v->get_if<repl::RepMovement>()) m = *old;
    }
    m.simulated_physic_sleep = false;
    m.rep_physics = false;
    m.location = repl::quantize_location(obj.opts, x, y, z);
    m.linear_velocity = repl::quantize_velocity(obj.opts, vel.x, vel.y, vel.z);
    m.angular_velocity = repl::QuantVec{repl::QuantVec::Mode::Scaled, 0, 0, 0};
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
    if (!a || a->kind != world::ActorKind::Npc) return false;
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
        npcs_.emplace(actor_id, std::move(n));
        if (pending) *pending = true;
        return true;
    }
    const bool found = ground_npc(n, *a);
    if (ground_found) *ground_found = found;
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
        found = nav_->ground_height(a->x, a->y, hint, &gz, 1500.0, 1500.0);
        // (a city node's people keep their data height when no surface is within reach: their town's floor is not in the baked ground, whose terrain lies 50 m below it)
        if (!found && !a->npc.ground_by_data && nav_->terrain_height(a->x, a->y, &terrain)) {
            // the data's height is out of reach of any surface here (a placement drawn at the wrong height): the terrain's is the ground
            hint = terrain;
            found = nav_->ground_height(a->x, a->y, hint, &gz, 1500.0, 1500.0);
        }
    }
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
            if (it == npcs_.end() || it->second.mode == Mode::Stand || it->second.pending_ground) continue;
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
    for (size_t i = 0; i < count; ++i) advance_npc(npcs_.find(active_[(first + i) % count])->second, t);
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

void NpcWorld::advance_npc(Npc& n, double t) {
    const double dt = cfg_.step_seconds;
    if (n.mode == Mode::Stand) return;
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
    if (!n.mover.move_to(dest, speed_cm_s, stop_distance, 0)) return false;
    n.mode = Mode::Ordered;
    ++stats_.moves_started;
    return true;
}

bool NpcWorld::release(uint64_t id) {
    const auto it = npcs_.find(id);
    if (it == npcs_.end() || it->second.mode != Mode::Ordered) return false;
    go_idle(it->second);
    return true;
}

bool NpcWorld::halt(uint64_t id) {
    const auto it = npcs_.find(id);
    if (it == npcs_.end()) return false;
    Npc& n = it->second;
    n.mover.stop();
    n.mode = Mode::Stand;
    n.ordered_return = false;
    commit(n, now_, true);
    return true;
}

bool NpcWorld::order_return_home(uint64_t id) {
    const auto it = npcs_.find(id);
    if (it == npcs_.end()) return false;
    Npc& n = it->second;
    if (!n.mover.move_to(n.home, n.setup.run_speed, 0, 0)) return false;
    n.mode = Mode::Return;
    n.ordered_return = true;
    ++stats_.returns_home;
    return true;
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
