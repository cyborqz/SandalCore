// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  npc/ai/npc_ai.cpp (see npc_ai.h)
// ============================================================================
#include "npc/ai/npc_ai.h"

#include <algorithm>
#include <cmath>

namespace aoc::worldd::npc::ai {

using combat::Vec3;

const char* state_name(State s) {
    switch (s) {
        case State::Idle: return "idle";
        case State::Chase: return "chase";
        case State::Attack: return "attack";
        case State::Reset: return "reset";
        case State::Dead: return "dead";
    }
    return "?";
}

bool NpcAi::within_leash(const Vec3& self, const Vec3& home) const { return combat::distance_xy_cm(self, home) <= params_.leash_radius_cm; }

namespace {

/// The angle between two headings, 0 to 180 degrees.
double heading_difference_degrees(double a, double b) {
    const double d = std::fmod(std::fabs(a - b), 360.0);
    return d > 180.0 ? 360.0 - d : d;
}

}  // namespace

bool NpcAi::notices(const Sense& s, const Candidate& c, double& distance) const {
    if (!params_.senses.enabled) {
        distance = combat::distance_xy_cm(s.self, c.pos);
        return distance <= params_.aggro_radius_cm + c.radius_cm && within_leash(s.self, s.home);
    }
    // within the aggro radius (centre to centre), and then heard (all round) or seen (inside the cone in front of it), each within its own radius and vertical limit
    const double dx = c.pos.x - s.self.x, dy = c.pos.y - s.self.y, dz = c.pos.z - s.self.z;
    distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!(distance < params_.aggro_radius_cm)) return false;
    const Senses& e = params_.senses;
    const double vertical = std::fabs(dz);
    if (e.hearing_radius_cm > 0 && distance < e.hearing_radius_cm && (e.hearing_vertical_cm <= 0 || vertical < e.hearing_vertical_cm)) return true;
    if (e.sight_radius_cm > 0 && distance < e.sight_radius_cm && (e.sight_vertical_cm <= 0 || vertical < e.sight_vertical_cm)) {
        const double bearing = std::atan2(dy, dx) * 180.0 / 3.14159265358979323846;
        return heading_difference_degrees(bearing, s.self_yaw_degrees) < e.sight_cone_deg;
    }
    return false;
}

void NpcAi::notice(uint64_t id, double threat, double now) {
    if (state_ == State::Dead || state_ == State::Reset) return;
    threat_.add(id, threat, now);
    if (state_ == State::Idle) pending_fight_ = true;
}

void NpcAi::kill() {
    state_ = State::Dead;
    threat_.clear();
    target_ = 0;
}

void NpcAi::revive() {
    state_ = State::Idle;
    threat_.clear();
    target_ = 0;
    pending_fight_ = false;
}

void NpcAi::reset_now() {
    if (state_ == State::Dead) return;
    state_ = State::Reset;
    threat_.clear();
    target_ = 0;
    pending_fight_ = false;
}

Orders NpcAi::think(const Sense& s) {
    Orders o;
    if (state_ == State::Dead) return o;
// -- Cyb & C-O-A --

    const auto find = [&](uint64_t id) -> const Candidate* {
        for (const Candidate& c : s.known) {
            if (c.id == id) return &c;
        }
        return nullptr;
    };

    // the table loses whoever died or left
    {
        std::vector<uint64_t> gone;
        for (const ThreatTable::Entry& e : threat_.entries()) {
            const Candidate* c = find(e.id);
            if (!c || !c->alive) gone.push_back(e.id);
        }
        for (const uint64_t id : gone) threat_.remove(id);
    }

    const double home_dist = combat::distance_xy_cm(s.self, s.home);

    if (state_ == State::Reset) {
        o.move = Orders::Move::Home;
        o.dest = s.home;
        o.speed_cm_s = params_.run_home_speed_cm_s;
        if (home_dist <= params_.reset_arrive_cm) {
            state_ = State::Idle;
            o.move = Orders::Move::Release;
            o.reset_done = true;
        }
        return o;
    }

    if (state_ == State::Idle) {
        if (params_.proximity_aggro && s.now >= next_sense_) {
            next_sense_ = s.now + params_.sense_interval;
            for (const Candidate& c : s.known) {
                if (!c.alive || !c.hostile) continue;
                double d = 0;
                if (!notices(s, c, d)) continue;
                if (s.line_of_sight && !s.line_of_sight(s.self, c.pos)) continue;
                threat_.add(c.id, params_.proximity_threat + (params_.aggro_radius_cm - d) * 0.001, s.now);
                pending_fight_ = true;
            }
        }
        if (threat_.empty()) {
            pending_fight_ = false;
            return o;
        }
        state_ = State::Chase;
        o.began_fight = true;
        target_ = 0;
        pending_fight_ = false;
        anchor_ = s.self;   // where the fight began
    }

    // chase and attack: the leash first
    const auto leash = [&](const char* reason) {
        threat_.clear();
        target_ = 0;
        state_ = State::Reset;
        o.began_reset = true;
        o.reset_reason = reason;
        o.move = Orders::Move::Home;
        o.dest = s.home;
        o.speed_cm_s = params_.run_home_speed_cm_s;
        return o;
    };
    const double anchor_dist = combat::distance_xy_cm(s.self, params_.anchor_at_fight_start ? anchor_ : s.home);
    if (anchor_dist > params_.leash_radius_cm) return leash(params_.anchor_at_fight_start ? "it is farther from where its fight began than its leash distance" : "it is farther from home than its leash distance");
    if (threat_.empty()) return leash("nobody is left to fight");
    target_ = threat_.pick(target_);
    const Candidate* t = find(target_);
    if (!t) {   // (cannot happen: the table only holds what is known)
        threat_.remove(target_);
        return o;
    }
    // a creature with senses also gives up on a target that has got farther from it than the leash radius
    if (params_.senses.enabled && combat::distance_xy_cm(s.self, t->pos) > params_.leash_radius_cm) return leash("its target is farther from it than its leash distance");
    o.target = target_;
    o.in_combat = true;
    const double surface = combat::distance_xy_cm(s.self, t->pos) - s.self_radius_cm - t->radius_cm;
    const double range = s.attack_range_cm * (state_ == State::Attack ? params_.attack_hysteresis : 1.0);
    if (surface <= range) {
        state_ = State::Attack;
        o.attack = true;
        o.move = Orders::Move::Stand;
        return o;
    }
    state_ = State::Chase;
    const bool moved = combat::distance_xy_cm(last_order_dest_, t->pos) >= params_.repath_distance_cm;
    if (s.now - last_order_at_ >= params_.repath_interval || moved) {
        last_order_at_ = s.now;
        last_order_dest_ = t->pos;
        o.move = Orders::Move::ChaseTo;
        o.dest = t->pos;
        o.stop_distance_cm = std::max(0.0, std::min(range * params_.chase_stop_fraction, s.attack_range_cm) + s.self_radius_cm + t->radius_cm);
        o.speed_cm_s = params_.chase_speed_cm_s;
    }
    return o;
}

}  // namespace aoc::worldd::npc::ai
