// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/riding.cpp
//
//  See riding.h.
// ============================================================================
#include "session/riding.h"

#include "bootstrap/constants.h"
#include "content/content_db.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace aoc::worldd {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

Ride::Ride(Setup setup) : s_(std::move(setup)) { cooldown_until_ = s_.dash_ready_at; }

void Ride::fill_speeds(Setup& s, const spawns::MountBook& book, const content::MountRow& row) {
    s.fly_speed = 0;
    s.swim_speed = 0;
    if (const spawns::MountBook::Speeds* sp = book.speeds_of(row.blueprint)) {
        s.fly_speed = sp->fly;
        s.swim_speed = sp->swim;
    }
    if (row.kind == "flying") s.fly_speed = std::max(s.fly_speed, kFlyBaseCm * row.flying_pct / 100.0);
    else s.fly_speed = 0;
    if (row.water_pct > 0) s.swim_speed = std::max(s.swim_speed, kSwimBaseCm * row.water_pct / 100.0);
}

double Ride::base_speed(uint8_t mode) const {
    if (mode == 4 && s_.swim_speed > 0) return s_.swim_speed;
    if (mode == 5 && s_.fly_speed > 0) return s_.fly_speed;
    return s_.run_speed;
}

double Ride::speed_limit(uint8_t mode, double t) const {
    const double mult = std::max(mult_, t < grace_until_ ? grace_mult_ : 0.0);
    return base_speed(mode) * mult * kSpeedMargin;
}

MoveLimits Ride::limits(const MoveLimits& base, double t) const {
    MoveLimits l = base;
    l.max_speed = speed_limit(1, t);
    // stage 4: swimming (mode 4) and flying (mode 5) are judged in 3D by the checker (move_checker.h); mode 5 only for a flyer
    l.air = base.air;
    l.air.active = true;
    l.air.can_fly = can_fly();
    l.air.fly_speed = can_fly() ? s_.fly_speed : 0.0;
    l.air.dive_speed = std::max(kMaxFlyFallSpeed, l.air.fly_speed);
    l.air.swim_speed = s_.swim_speed > 0 ? s_.swim_speed : base.air.swim_speed;
    return l;
}
// C | Y | B x C O A

void Ride::recompute(double t) {
    if (!s_.registry) return;
    s_.registry->modify(s_.actor, world::Change::State, [&](world::WorldActor& a) {
        double mult = 1.0;
        if (road_effect_ != 0 && s_.data) {
            if (const content::MountEffectRow* r = s_.data->road(road_tier_)) mult += r->value;
        }
        if (dash_effect_ != 0 && s_.data) {
            if (const content::MountEffectRow* d = s_.data->dash()) mult += d->value;
        }
        mult *= s_.gm_multiplier;   // the GM travel tools: a factor on top of the road and the dash
        if (mult < mult_) {
            grace_mult_ = mult_;
            grace_until_ = t + kSpeedGraceSeconds;
        }
        mult_ = mult;
        spawns::set_stat_value(a, spawns::kStatMoveSpeedMult, static_cast<float>(mult));
    });
}

bool Ride::begin(const spawns::Possession& p, double t, std::string& error) {
    if (!s_.registry || !s_.registry->find(s_.actor)) {
        error = "the mount is gone";
        return false;
    }
    if (dead_) {
        error = "the mount is dead";
        return false;
    }
    bool ok = false;
    s_.registry->modify(s_.actor, world::Change::State, [&](world::WorldActor& a) { ok = spawns::possess(a, p, error); });
    if (!ok) return false;
    riding_ = true;
    recompute(t);
    // review A I3: a dash still cooling down from the rider's earlier mount (a desummon and a summon do not reset its 30 s: the tooltip) shows on this one's bar too
    if (cooldown_until_ > t && cooldown_array_id_ == 0 && s_.dash_ability != 0 && s_.data && s_.data->dash()) {
        const double start = cooldown_until_ - kDashCooldownSeconds;
        s_.registry->modify(s_.actor, world::Change::State, [&](world::WorldActor& a) {
            bootstrap::CooldownItem cd;
            cd.ability_guid = s_.dash_ability;
            cd.triggered = true;
            cd.initial_cooldown = static_cast<float>(kDashCooldownSeconds);
            cd.start_time = bootstrap::effect_clock(clock(start));
            cd.end_time = cd.start_time + kDashCooldownSeconds;
            cd.charges = 0;
            if (const auto id = spawns::add_cooldown(a, cd)) cooldown_array_id_ = *id;
        });
    }
    return true;
}

bool Ride::end(double t, double x, double y, double z, double yaw_degrees, uint8_t movement_mode, std::string& error) {
    if (!s_.registry || !s_.registry->find(s_.actor)) {
        riding_ = false;
        error = "the mount is gone";
        return false;
    }
    set_road(-1, t);
    bool ok = false;
    const double stamp = clock(t);
    s_.registry->move(s_.actor, x, y, z, yaw_degrees);
    s_.registry->modify(s_.actor, world::Change::State, [&](world::WorldActor& a) {
        ok = spawns::release(a, s_.owner, error);
        spawns::write_motion(a, x, y, z, yaw_degrees, 0, 0, false, stamp);
        // fix 2: a flyer (5) or swimmer (4) stays where it was, in its mode (the client draws it hovering or floating instead of letting it fall); anything else is walking again
        repl::ObjectState& obj = a.actor.obj;
        if (obj.schema && obj.schema->command("ReplicatedMovementMode")) {
            const bool held = movement_mode == repl::kMoveFlying || movement_mode == repl::kMoveSwimming;
            const repl::Value* old = obj.state.get(*obj.schema, "ReplicatedMovementMode");
            const auto* was = old ? old->get_if<uint64_t>() : nullptr;
            if (held) obj.state.set(*obj.schema, "ReplicatedMovementMode", repl::Value::u64(movement_mode));
            else if (was && (*was == repl::kMoveFlying || *was == repl::kMoveSwimming)) obj.state.set(*obj.schema, "ReplicatedMovementMode", repl::Value::u64(repl::kMoveWalking));
        }
    });
    riding_ = false;
    return ok;
}

void Ride::moved(double x, double y, double z, double yaw_degrees, double t, uint16_t pitch16, uint8_t mode) {
    if (!s_.registry) return;
    mode_ = mode;
    s_.registry->move(s_.actor, x, y, z, yaw_degrees);
    if (view_pitch_ != static_cast<int32_t>(pitch16)) {
        // RemoteViewPitch16 follows the rider's view, a change at a time [R: 871: 456 of 457 values are the move's own pitch]
        s_.registry->modify(s_.actor, world::Change::State, [&](world::WorldActor& a) { spawns::set_view_pitch(a, pitch16); });
        view_pitch_ = static_cast<int32_t>(pitch16);
    }
    // review A M2: retail's road is an overlap volume on the ground; a flyer in the air (mode 5) is over it, not on it, and gets no bonus
    const int tier = (s_.roads && mode != repl::kMoveFlying) ? s_.roads->tier_at(x, y) : -1;
    if (tier != road_tier_) set_road(tier, t);
}

void Ride::set_road(int tier, double t) {
    if (tier == road_tier_ && (tier < 0 || road_effect_ != 0)) return;
    const content::MountEffectRow* row = (tier >= 0 && s_.data) ? s_.data->road(tier) : nullptr;
    if (!s_.registry) return;
    s_.registry->modify(s_.actor, world::Change::State, [&](world::WorldActor& a) {
        if (road_effect_ != 0) {
            spawns::remove_effect(a, road_array_id_);
            road_effect_ = 0;
            road_array_id_ = 0;
        }
        if (row) {
            bootstrap::EffectItem item;
            item.effect_id = next_effect_id_++;
            item.effect_guid = row->effect;
            item.start_time = bootstrap::effect_clock(clock(t));
            item.duration = std::numeric_limits<float>::max();   // no end: the item goes when the mount leaves the road
            item.tick_time = 0;
            item.stacks = 1;
            if (const auto id = spawns::add_effect(a, s_.owner, item)) {
                road_effect_ = item.effect_id;
                road_array_id_ = *id;
            }
        }
    });
    road_tier_ = tier;
    recompute(t);
}

Ride::Dash Ride::trigger_dash(int64_t ability, double t) {
    if (!riding_ || dead_ || s_.dash_ability == 0 || ability != s_.dash_ability || !s_.data || !s_.data->dash()) return Dash::NotTheDash;
    if (t < cooldown_until_ || dash_apply_at_ >= 0) return Dash::OnCooldown;
    dash_apply_at_ = t + kDashReplyDelay;
    cooldown_until_ = dash_apply_at_ + kDashCooldownSeconds;
    return Dash::Started;
}

Ride::Events Ride::advance(double t) {
    Events ev;
    if (!s_.registry || !s_.registry->find(s_.actor)) return ev;
    if (dash_apply_at_ >= 0 && t >= dash_apply_at_) {
        const content::MountEffectRow* d = s_.data ? s_.data->dash() : nullptr;
        const double at = dash_apply_at_;
        dash_apply_at_ = -1.0;
        if (d) {
            s_.registry->modify(s_.actor, world::Change::State, [&](world::WorldActor& a) {
                bootstrap::CooldownItem cd;
                cd.ability_guid = s_.dash_ability;
                cd.triggered = true;
                cd.initial_cooldown = static_cast<float>(kDashCooldownSeconds);
                cd.start_time = bootstrap::effect_clock(clock(at));
                cd.end_time = cd.start_time + kDashCooldownSeconds;
                cd.charges = 0;
                if (const auto id = spawns::add_cooldown(a, cd)) cooldown_array_id_ = *id;
                bootstrap::EffectItem item;
                item.effect_id = next_effect_id_++;
                item.effect_guid = d->effect;
                item.start_time = bootstrap::effect_clock(clock(at));
                item.duration = static_cast<float>(d->duration);
                item.tick_time = 0;
                item.stacks = 1;
                if (const auto id = spawns::add_effect(a, s_.owner, item)) {
                    dash_effect_ = item.effect_id;
                    dash_array_id_ = *id;
                    dash_until_ = at + d->duration;
                }
            });
            if (dash_effect_ != 0) {
                recompute(at);
                ev.dash_applied = true;
            }
        }
    }
    if (dash_effect_ != 0 && t >= dash_until_) {
        s_.registry->modify(s_.actor, world::Change::State, [&](world::WorldActor& a) { spawns::remove_effect(a, dash_array_id_); });
        dash_effect_ = 0;
        dash_array_id_ = 0;
        recompute(dash_until_);
        ev.dash_ended = true;
    }
    if (cooldown_array_id_ != 0 && t >= cooldown_until_) {
        s_.registry->modify(s_.actor, world::Change::State, [&](world::WorldActor& a) { spawns::remove_cooldown(a, cooldown_array_id_); });
        cooldown_array_id_ = 0;
    }
    return ev;
}
// -- C Y B & C | O | A --

double Ride::health() const {
    const world::WorldActor* a = mount();
    if (!a) return 0;
    return spawns::stat_value(*a, content::kGuidHealth).value_or(0.0);
}

double Ride::max_health() const {
    const world::WorldActor* a = mount();
    if (!a) return 0;
    return spawns::stat_value(*a, content::kGuidMaxHealth).value_or(0.0);
}

Ride::Hurt Ride::hurt(double amount) {
    Hurt h;
    if (dead_ || !(amount > 0) || !std::isfinite(amount) || !s_.registry) return h;
    const double before = health();
    if (before <= 0) return h;
    h.taken = std::min(before, amount);
    const double after = before - h.taken;
    s_.registry->modify(s_.actor, world::Change::State, [&](world::WorldActor& a) { spawns::set_stat_value(a, content::kGuidHealth, static_cast<float>(after)); });
    if (after <= 0) {
        dead_ = true;
        h.died = true;
    }
    return h;
}

Ride::Place Ride::dismount_place(double x, double y, double z, double yaw_degrees) {
    const double yaw = yaw_degrees * kPi / 180.0;
    Place p;
    p.x = x + kDismountForwardCm * std::cos(yaw);
    p.y = y + kDismountForwardCm * std::sin(yaw);
    p.z = z + kDismountRiseCm;
    p.yaw_degrees = yaw_degrees;
    return p;
}

Ride::Place Ride::rest_place(double x, double y, double z, double yaw_degrees) {
    Place p = dismount_place(x, y, z, yaw_degrees);
    p.z = z + 41.0;
    return p;
}

}  // namespace aoc::worldd
