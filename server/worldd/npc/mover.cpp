// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "npc/mover.h"

#include <algorithm>
#include <cmath>

namespace aoc::worldd::npc {

namespace {

constexpr double kPi = 3.14159265358979323846;
// The A* of an NPC's walk gives up after this many node expansions (nav::AreaNav::find_path), plus a share per metre of the straight line to its end (review A I5): the Riverlands'
// navmesh has about 135 polygons to a tile (a 40 m tile: about 3.4 m a polygon, about 300 along a kilometre), and Detour's A* expands a few times the path's length, more round a lake.
// A wander leg is a few hundred metres and a leg that cannot be found must not explore a navmesh of 4,096 tiles (G1 stage 1: that was a step of 10 ms); a patrol leg of a kilometre or
// two must not be given up at the same number.  The world also caps the expansions of one step (NpcWorldConfig::max_search_iterations_per_step).
constexpr int kSearchIterations = 2500;
constexpr double kSearchPerMetre = 5.0;
constexpr double kSearchMaxMetres = 3000.0;

double wrap180(double a) {
    a = std::fmod(a + 180.0, 360.0);
    if (a < 0) a += 360.0;
    return a - 180.0;
}

double dist2d(const Vec3& a, const Vec3& b) { return std::hypot(a.x - b.x, a.y - b.y); }

bool finite3(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

}  // namespace

int Mover::search_iterations(double straight_cm) {
    const double metres = std::isfinite(straight_cm) ? std::clamp(straight_cm, 0.0, kSearchMaxMetres * 100.0) / 100.0 : 0.0;
    return kSearchIterations + static_cast<int>(metres * kSearchPerMetre);
}

void Mover::init(const nav::AreaNav* nav, double foot_offset_cm, double yaw_rate_degrees_per_second) {
    nav_ = nav;
    foot_offset_ = std::isfinite(foot_offset_cm) ? foot_offset_cm : 88.0;
    yaw_rate_ = yaw_rate_degrees_per_second > 0 ? yaw_rate_degrees_per_second : 360.0;
}
// Cyb and C.O.A

void Mover::place(double x, double y, double ground_z, double yaw_degrees) {
    ground_ = {x, y, ground_z};
    yaw_ = std::isfinite(yaw_degrees) ? wrap180(yaw_degrees) : 0.0;
    velocity_ = {};
    path_.clear();
    next_ = 0;
    status_ = Status::Idle;
}

void Mover::finish(Status s) {
    status_ = s;
    velocity_ = {};
    speed_ = 0;
    path_.clear();
    next_ = 0;
}

void Mover::stop() { finish(Status::Idle); }

double Mover::remaining() const {
    if (status_ != Status::Moving) return 0;
    double total = 0;
    Vec3 p = ground_;
    for (size_t i = next_; i < path_.size(); ++i) {
        total += dist2d(p, path_[i]);
        p = path_[i];
    }
    return total;
}

bool Mover::follow(const std::vector<Vec3>& path, double speed_cm_s) {
    if (!(speed_cm_s > 0) || !std::isfinite(speed_cm_s) || path.empty()) {
        finish(Status::Blocked);
        return false;
    }
    for (const Vec3& p : path) {
        if (!finite3(p)) {
            finish(Status::Blocked);
            return false;
        }
    }
    path_ = path;
    next_ = 0;
    speed_ = speed_cm_s;
    stop_distance_ = 0;
    status_ = Status::Moving;
    velocity_ = {};
    // a path that starts where it stands: the first point is the start
    if (dist2d(path_[0], ground_) < 1.0) next_ = 1;
    if (next_ >= path_.size()) finish(Status::Arrived);
    return true;
}

bool Mover::move_to(const Vec3& dest, double speed_cm_s, double stop_distance, double max_length, bool allow_partial) {
    if (!nav_ || !finite3(dest) || !(speed_cm_s > 0) || !std::isfinite(speed_cm_s)) {
        finish(Status::Blocked);
        return false;
    }
    const nav::PathResult p = nav_->find_path(ground_, dest, 200, 400, search_iterations(dist2d(ground_, dest)));
    last_iterations_ = p.iterations;
    last_fail_ = p.fail;
    if (p.found && p.complete && max_length > 0 && p.length > max_length) last_fail_ = nav::PathResult::Fail::TooLong;
    if (!p.found || (!p.complete && !allow_partial) || (max_length > 0 && p.length > max_length) || p.points.empty()) {
        finish(Status::Blocked);
        return false;
    }
    std::vector<Vec3> pts = p.points;
    pts[0] = ground_;   // the walk starts from where it stands, not from the navmesh point nearest to it
    if (!follow(pts, speed_cm_s)) return false;
    stop_distance_ = std::max(0.0, stop_distance);
    return true;
}
// [C-O-A :: C-Y-B]

Mover::Status Mover::advance(double dt) {
    if (status_ != Status::Moving) {
        velocity_ = {};
        return status_;
    }
    if (!(dt > 0) || !std::isfinite(dt)) return status_;
    const Vec3 before = ground_;
    double budget = speed_ * dt;
    // end short of the destination when asked
    if (stop_distance_ > 0) {
        const double left = remaining();
        if (left <= stop_distance_) {
            finish(Status::Arrived);
            return status_;
        }
        budget = std::min(budget, left - stop_distance_);
    }
    double heading = yaw_;
    bool have_heading = false;
    while (budget > 0 && next_ < path_.size()) {
        const Vec3& target = path_[next_];
        const double d = dist2d(ground_, target);
        if (d > 1e-6) {
            heading = std::atan2(target.y - ground_.y, target.x - ground_.x) * 180.0 / kPi;
            have_heading = true;
        }
        if (d <= budget) {
            ground_ = target;
            budget -= d;
            ++next_;
        } else {
            const double t = budget / d;
            ground_.x += (target.x - ground_.x) * t;
            ground_.y += (target.y - ground_.y) * t;
            ground_.z += (target.z - ground_.z) * t;
            budget = 0;
        }
    }
    // the ground under the new place: the navmesh's floor or the landscape's own height, nearest the height it stood at a step ago (the path's own
    // heights are those of polygon corners and may be far from the ground between them on a slope)
    if (nav_) {
        double gz = 0;
        const double window = 80.0 + 2.0 * std::hypot(ground_.x - before.x, ground_.y - before.y);
        if (nav_->ground_height(ground_.x, ground_.y, before.z, &gz, window, window)) ground_.z = gz;
        else ++ground_misses_;
    }
    // turn towards the way it goes, at the Blueprint's rotation rate
    if (have_heading) {
        const double diff = wrap180(heading - yaw_), max_turn = yaw_rate_ * dt;
        yaw_ = wrap180(yaw_ + std::clamp(diff, -max_turn, max_turn));
    }
    const Vec3 v{(ground_.x - before.x) / dt, (ground_.y - before.y) / dt, (ground_.z - before.z) / dt};
    if (next_ >= path_.size()) {
        finish(Status::Arrived);   // it has stopped: the step that got it there is not a velocity it keeps
        return status_;
    }
    velocity_ = v;
    return status_;
}

}  // namespace aoc::worldd::npc
