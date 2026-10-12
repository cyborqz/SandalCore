// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/mounts.cpp (see mounts.h)
// ============================================================================
#include "session/mounts.h"

#include "spawns/spawn_util.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace aoc::worldd {

namespace util = spawns::util;
namespace repl = ::aoc::protocol::repl;
using bootstrap::AbilityChange;

namespace {

constexpr double kPi = 3.14159265358979323846;

double radians(double degrees) { return degrees * kPi / 180.0; }

using spawns::sub_of;
using spawns::write_motion;

}  // namespace

void MountController::Effects::merge(Effects&& o) {
    const auto take = [](auto& to, auto& from) {
        to.insert(to.end(), std::make_move_iterator(from.begin()), std::make_move_iterator(from.end()));
    };
    take(ability.abilities_removed, o.ability.abilities_removed);
    take(ability.abilities_added, o.ability.abilities_added);
    take(ability.abilities_changed, o.ability.abilities_changed);
    if (o.bar) {
        bar = true;
        bar_ability = o.bar_ability;
        bar_slot = o.bar_slot;
    }
    take(callouts, o.callouts);
    take(callouts_removed, o.callouts_removed);
    take(ends, o.ends);
    if (o.persist) persist = o.persist;
    if (o.pet_changed) {
        pet_changed = true;
        pet = o.pet;
    }
}
// [Cyb + C | O | A]

MountController::MountController(world::ActorRegistry& registry, const spawns::MountBook& book, spawns::Minter mint, Owner owner)
    : registry_(registry), book_(book), mint_(std::move(mint)), owner_(std::move(owner)) {}

void MountController::add(double due, Kind kind, uint64_t key) {
    Step s;
    s.due = due;
    s.kind = kind;
    s.cast = (kind == Kind::DesummonFlags || kind == Kind::DesummonRun || kind == Kind::DesummonFlagsOff || kind == Kind::DesummonInputOff ||
              kind == Kind::DesummonClose || kind == Kind::DeathClose || kind == Kind::CalloutOff)
                 ? 0
                 : cast_;
    s.key = key;
    steps_.push_back(s);
}

void MountController::drop_steps(uint64_t cast, bool keep_desummon) {
    (void)keep_desummon;
    steps_.erase(std::remove_if(steps_.begin(), steps_.end(), [&](const Step& s) { return s.cast == cast && cast != 0; }), steps_.end());
}

void MountController::forget_mount(Effects* out) {
    if (pet_set_) {   // the pawn lets its mountable pet go [R: 898 packet 601, with the mount's close]
        Effects& e = out ? *out : pending_effects_;
        e.pet_changed = true;
        e.pet = protocol::emit::FIntrepidNetworkGUID{};
        pet_set_ = false;
    }
    actor_ = 0;
    item_record_ = 0;
    phase_ = Phase::Idle;
}

double MountController::run_speed(const content::MountRow& row) const {
    if (speed_of_) {
        const double v = speed_of_(row.blueprint);
        if (v > 0 && std::isfinite(v)) return v;
    }
    return kDefaultSpeedCm * (row.ground_pct > 0 ? row.ground_pct / 100.0 : 1.0);
}

MountController::Outcome MountController::summon(const Request& r, double now) {
    // a mount that left the world by itself (a registry removal) is no mount
    if (actor_ != 0 && !registry_.find(actor_)) {
        steps_.erase(std::remove_if(steps_.begin(), steps_.end(), [](const Step& s) { return s.kind != Kind::CalloutOff; }), steps_.end());
        cast_ = 0;
        forget_mount();
    }
    const content::MountRow* row = book_.find(r.item_record);
    if (!row) {
        ++stats_.ignored;
        return Outcome::Ignored;
    }
    if (phase_ == Phase::Desummoning || phase_ == Phase::Dying) {
        ++stats_.ignored;
        return Outcome::Ignored;
    }
    if (phase_ == Phase::Out || phase_ == Phase::Summoning) {
        // one mount per player: the old one goes, nothing new is summoned (retail: a different mount item used while one is out is a desummon of the old only)
        desummon(now);
        return Outcome::Desummoning;
    }
    if (cast_ != 0) {   // a cast or a refusal is playing: a summon on top of it is ignored (a flood of requests)
        ++stats_.ignored;
        return Outcome::Ignored;
    }

    req_ = r;
    clock_offset_ = r.world_time - now;
    cast_ = next_cast_++;
    open_done_ = false;
    item_record_ = r.item_record;

    // the refusals
    bool refuse = r.dead || (r.lock_expiration > r.now_unix);
    if (!refuse && ground_) {
        const double yaw = radians(r.place.yaw_degrees);
        const double tx = r.place.x + kSpawnAheadCm * std::cos(yaw), ty = r.place.y + kSpawnAheadCm * std::sin(yaw);
        const double feet = r.place.z - kPlayerFootOffsetCm;
        const std::optional<double> g = ground_(tx, ty, feet);
        refuse = !g || std::fabs(*g - feet) > kMaxHeightDifferenceCm;
    }
    if (refuse) {
        ++stats_.refused;
        refusal_ = true;
        add(now + kRefusalItemDelay, Kind::RefusalItem);
        add(now + kRefusalEnds, Kind::RefusalEnd);
        return Outcome::Refused;
    }
    ++stats_.summons;
    refusal_ = false;
    phase_ = Phase::Summoning;
    add(now + kItemDelay, Kind::Item);
    add(now + kOpenDelay, Kind::Open);
    add(now + kCastEnds, Kind::CastEnd);
    return Outcome::Started;
}

bool MountController::desummon(double now) {
    if (phase_ == Phase::Desummoning || phase_ == Phase::Dying) return false;
    if (phase_ == Phase::Idle) {
        if (cast_ != 0 && !refusal_) {   // (a summon whose actor is not open yet)
            ++stats_.desummons;
            cancel_cast(now, pending_effects_, false);
            return true;
        }
        return false;
    }
    ++stats_.desummons;
    if (cast_ != 0) cancel_cast(now, pending_effects_, false);
    begin_desummon(now);
    return true;
}

void MountController::begin_desummon(double now) {
    const world::WorldActor* a = mount();
    if (!a) {
        forget_mount();
        return;
    }
    const content::MountRow* row = book_.find(item_record_);
    phase_ = Phase::Desummoning;
    registry_.modify(actor_, world::Change::State, [](world::WorldActor& m) { spawns::set_interaction_open(m, false); });
    pending_effects_.persist = persist_now();
    run_x_ = a->x;
    run_y_ = a->y;
    run_z_ = a->z;
    run_yaw_ = a->yaw_degrees;
    run_speed_ = row ? run_speed(*row) : kDefaultSpeedCm;
    run_at_ = now + kDesummonRun - kRunStepSeconds;
    add(now + kDesummonFlags, Kind::DesummonFlags);
    add(now + kDesummonRun, Kind::DesummonRun);
    add(now + kDesummonFlagsOff, Kind::DesummonFlagsOff);
    add(now + kDesummonInputOff, Kind::DesummonInputOff);
    add(now + kDesummonClose, Kind::DesummonClose);
    run_end_ = now + kDesummonFlagsOff;
}

void MountController::on_damage(double now) {
    if (cast_ == 0) return;
    if (refusal_) {
        cancel_cast(now, pending_effects_, false);   // (the refusal's item ends at once too)
        return;
    }
    // The user's rule (stage 4): a mount stays through a fight.  A hit may cut the cast only BEFORE the mount appears (the first ~104 ms: nothing spawns); once it is open
    // nothing a hit does desummons it, and the cast runs on to its end.
    if (actor_ != 0) return;
    ++stats_.cancelled;
    cancel_cast(now, pending_effects_, false);
}
// -- CoA & C:Y:B --

void MountController::cancel_cast(double now, Effects& out, bool dismiss_mount) {
    (void)now;
    const uint64_t cast = cast_;
    drop_steps(cast, true);
    cast_ = 0;
    if (item_instance_ >= 0 && item_shown_) item_visible_effects(out, true);
    const bool had_mount = actor_ != 0 && phase_ != Phase::Desummoning;
    if (had_mount && dismiss_mount) {
        // the mount is open already: it is desummoned (its cast was cut)
        ++stats_.desummons;
        begin_desummon(now);
    } else if (!had_mount && !refusal_) {
        phase_ = Phase::Idle;
    }
    if (actor_ == 0) item_record_ = 0;
    refusal_ = false;
}

/// The ability item of Summon_Mount: shown (added, in place of the previous one) or ended (changed in place, no montage).
bool MountController::item_visible_effects(Effects& out, bool end) {
    if (!end) {
        const int64_t instance = instance_ ? instance_() : -1;
        if (instance < 0) {
            item_instance_ = -1;
            item_shown_ = false;
            return false;
        }
        if (item_instance_ >= 0) out.ability.abilities_removed.push_back(item_instance_);
        bootstrap::ActiveAbilityItem item;
        item.ability_guid = book_.summon_ability();
        item.server_instance_id = instance;
        item.montage_path = kMontagePath;
        item.montage_guid = kMontageGuid;
        item.animation_counter = ++animation_counter_;
        out.ability.abilities_added.push_back(std::move(item));
        item_instance_ = instance;
        item_shown_ = true;
        return true;
    }
    if (item_instance_ < 0 || !item_shown_) return false;
    bootstrap::ActiveAbilityItem item;
    item.ability_guid = book_.summon_ability();
    item.server_instance_id = item_instance_;
    item.animation_counter = ++animation_counter_;   // (no montage: the cast is over)
    out.ability.abilities_changed.push_back(std::move(item));
    item_shown_ = false;
    return true;
}

void MountController::run_open(double now, Effects& out) {
    const content::MountRow* row = book_.find(req_.item_record);
    const double yaw = radians(req_.place.yaw_degrees);
    spawns::MountParams p;
    p.player_id = owner_.player_id;
    p.owner_guid = owner_.guid;
    p.owner_name = owner_.name;
    p.owner_pawn = owner_.pawn;
    p.owner_level = req_.owner_level;
    p.server_id = owner_.server_id;
    p.world_time = now + clock_offset_;
    p.item_guid = req_.item_guid;
    p.x = req_.place.x + kSpawnAheadCm * std::cos(yaw);
    p.y = req_.place.y + kSpawnAheadCm * std::sin(yaw);
    p.z = req_.place.z - 41.0;   // [R: 911: the mount's origin 41 cm below the player's; 12 captured summons: under 1.2 m]
    p.yaw_degrees = req_.place.yaw_degrees;
    p.seed = (static_cast<uint64_t>(owner_.player_id) << 32) ^ (cast_ * 0x9E3779B97F4A7C15ull) ^ 0xA5A5A5A5ull;
    p.health = req_.health;
    p.owner_alignments = req_.alignments;
    if (ground_) {
        if (const auto g = ground_(p.x, p.y, req_.place.z - kPlayerFootOffsetCm)) p.z = *g + kMountFootOffsetCm;
    }
    std::string error;
    auto made = row ? book_.make(*row, p, mint_, error) : std::nullopt;
    if (!made) {
        spdlog::warn("[mounts] {}: the mount of item {} cannot be made: {}", owner_.name, req_.item_record, row ? error : std::string("no such mount"));
        out.ends.push_back(req_.client_instance);
        if (item_instance_ >= 0 && item_shown_) item_visible_effects(out, true);
        drop_steps(cast_, true);
        cast_ = 0;
        forget_mount();
        return;
    }
    const uint64_t id = made->actor.id;
    const protocol::emit::FIntrepidNetworkGUID pet = made->actor.actor.guid;
    if (!registry_.add(std::move(made->actor))) {
        spdlog::warn("[mounts] {}: the registry does not take the mount of item {}", owner_.name, req_.item_record);
        out.ends.push_back(req_.client_instance);
        if (item_instance_ >= 0 && item_shown_) item_visible_effects(out, true);
        drop_steps(cast_, true);
        cast_ = 0;
        forget_mount();
        return;
    }
    actor_ = id;
    open_done_ = true;
    ++stats_.opened;
    // the pawn names the mount it can mount: the client's action bar resolves the bank-18 Mount slot (the dash) through it [R: 871 packet 210, 898 packet 158]
    out.pet_changed = true;
    out.pet = pet;
    pet_set_ = true;
    spdlog::info("[mounts] {}: {} is summoned (actor {}, {:.0f} cm in front of the player; its rider sits on {})", owner_.name, row->item_name, id, kSpawnAheadCm, book_.rider_anim_of(*row));
}
// Cyb captures, C | O | A code

void MountController::run(const Step& s, double now, Effects& out) {
    switch (s.kind) {
        case Kind::Item:
            item_visible_effects(out, false);
            if (const content::MountRow* row = book_.find(req_.item_record)) {
                const spawns::DashBar dash = book_.dash_bar(*row);
                out.bar = true;
                out.bar_ability = dash.ability;
                out.bar_slot = dash.slot;
            }
            break;
        case Kind::Open:
            run_open(now, out);
            if (open_done_) add(s.due + kSummoningEnds, Kind::SummoningOff);
            break;
        case Kind::SummoningOff:
            if (actor_ != 0) {
                registry_.modify(actor_, world::Change::State, [](world::WorldActor& a) {
                    if (repl::Subobject* m = sub_of(a, "UMountComponent")) m->obj.state.set(*m->obj.schema, "bIsSummoning", repl::Value::boolean(false));
                });
                if (phase_ == Phase::Summoning) {
                    phase_ = Phase::Out;
                    registry_.modify(actor_, world::Change::State, [](world::WorldActor& a) { spawns::set_interaction_open(a, true); });   // now it may be mounted
                }
            }
            break;
        case Kind::CastEnd:
            item_visible_effects(out, true);
            cast_ = 0;
            break;
        case Kind::RefusalItem: {
            item_visible_effects(out, false);
            Callout c;
            c.key = (static_cast<uint64_t>(owner_.player_id) << 32) | next_callout_++;
            c.text = kRefusalText;
            out.callouts.push_back(c);
            add(s.due + kCalloutLife, Kind::CalloutOff, c.key);
            break;
        }
        case Kind::RefusalEnd:
            item_visible_effects(out, true);
            cast_ = 0;
            refusal_ = false;
            item_record_ = 0;
            break;
        case Kind::CalloutOff:
            out.callouts_removed.push_back(s.key);
            break;
        case Kind::DesummonFlags:
            registry_.modify(actor_, world::Change::State, [](world::WorldActor& a) {
                if (repl::Subobject* m = sub_of(a, "UMountComponent")) {
                    m->obj.state.set(*m->obj.schema, "bIsDesummoning", repl::Value::boolean(true));
                    m->obj.state.set(*m->obj.schema, "DissolveTimer", repl::Value::f32(static_cast<float>(kDesummonDissolveTimer)));
                }
                if (repl::Subobject* c = sub_of(a, "UNPCCombatInfo")) {
                    c->obj.state.set(*c->obj.schema, "bInvulnerable", repl::Value::boolean(true));
                    c->obj.state.set(*c->obj.schema, "bNotTargetable", repl::Value::boolean(true));
                }
            });
            break;
        case Kind::DesummonRun: {
            if (actor_ == 0 || !registry_.find(actor_) || s.due >= run_end_) break;
            const double dt = std::clamp(now - run_at_, 0.0, 0.25);
            const double yaw = radians(run_yaw_);
            const double dx = std::cos(yaw), dy = std::sin(yaw);
            run_x_ += dx * run_speed_ * dt;
            run_y_ += dy * run_speed_ * dt;
            if (ground_) {
                if (const auto g = ground_(run_x_, run_y_, run_z_ - kMountFootOffsetCm)) run_z_ = *g + kMountFootOffsetCm;
            }
            run_at_ = now;
            const double stamp = now + clock_offset_;
            registry_.move(actor_, run_x_, run_y_, run_z_, run_yaw_);
            registry_.modify(actor_, world::Change::Movement, [&](world::WorldActor& a) {
                write_motion(a, run_x_, run_y_, run_z_, run_yaw_, dx * run_speed_, dy * run_speed_, true, stamp);
            });
            if (s.due + kRunStepSeconds < run_end_) add(s.due + kRunStepSeconds, Kind::DesummonRun);
            break;
        }
        case Kind::DesummonFlagsOff:
            registry_.modify(actor_, world::Change::State, [](world::WorldActor& a) {
                if (repl::Subobject* m = sub_of(a, "UMountComponent")) m->obj.state.set(*m->obj.schema, "bIsDesummoning", repl::Value::boolean(false));
            });
            break;
        case Kind::DesummonInputOff: {
            if (actor_ == 0 || !registry_.find(actor_)) break;
            const double stamp = now + clock_offset_;
            registry_.modify(actor_, world::Change::Movement, [&](world::WorldActor& a) {
                write_motion(a, run_x_, run_y_, run_z_, run_yaw_, 0, 0, false, stamp);
            });
            break;
        }
        case Kind::DeathClose:
            if (actor_ != 0) {
                registry_.remove(actor_);
                ++stats_.closed;
                spdlog::info("[mounts] {}: the dead mount is gone (actor {})", owner_.name, actor_);
            }
            forget_mount(&out);
            break;
        case Kind::DesummonClose:
            if (actor_ != 0) {
                registry_.remove(actor_);   // every connection that holds it closes its channel, Destroyed
                ++stats_.closed;
                spdlog::info("[mounts] {}: the mount is gone (actor {})", owner_.name, actor_);
            }
            forget_mount(&out);
            break;
    }
}

MountController::Effects MountController::advance(double now) {
    Effects out = std::move(pending_effects_);
    pending_effects_ = {};
    // a mount that left the world by itself: its steps end
    if (actor_ != 0 && !registry_.find(actor_)) {
        steps_.erase(std::remove_if(steps_.begin(), steps_.end(), [](const Step& s) { return s.kind != Kind::CalloutOff && s.kind != Kind::RefusalItem && s.kind != Kind::RefusalEnd; }), steps_.end());
        if (!refusal_) cast_ = 0;
        forget_mount(&out);
    }
    // the steps that are due, in time order; a step may add later ones (and, with a long gap, earlier ones too: they run in the same call)
    for (int guard = 0; guard < 4096; ++guard) {
        auto it = std::min_element(steps_.begin(), steps_.end(), [](const Step& a, const Step& b) { return a.due < b.due; });
        if (it == steps_.end() || it->due > now) break;
        const Step s = *it;
        steps_.erase(it);
        run(s, now, out);
    }
    return out;
}
// [CoA :: C-Y-B]

std::optional<MountController::Persist> MountController::persist_now() const {
    const world::WorldActor* a = mount();
    if (!a) return std::nullopt;
    const std::optional<double> health = spawns::stat_value(*a, content::kGuidHealth);
    if (!health) return std::nullopt;
    Persist p;
    p.item_guid = req_.item_guid;
    p.health = static_cast<float>(std::max(0.0, *health));
    return p;
}

bool MountController::die(double now, int64_t now_unix) {
    const world::WorldActor* a = mount();
    if (!a || phase_ == Phase::Dying || phase_ == Phase::Idle) return false;
    // what is in progress ends: the cast, the desummon's run; the mount is dead, then gone
    steps_.erase(std::remove_if(steps_.begin(), steps_.end(), [](const Step& s) { return s.kind != Kind::CalloutOff; }), steps_.end());
    if (cast_ != 0) cancel_cast(now, pending_effects_, false);
    cast_ = 0;
    ++stats_.deaths;
    phase_ = Phase::Dying;
    registry_.modify(actor_, world::Change::State, [](world::WorldActor& m) {
        spawns::set_interaction_open(m, false);
        spawns::set_dead(m, true);
    });
    Persist p;
    p.item_guid = req_.item_guid;
    p.health = 0;
    p.lock_expiration = now_unix + kLockSeconds;
    p.died = true;
    pending_effects_.persist = p;
    add(now + kDeathClose, Kind::DeathClose);
    return true;
}

std::optional<MountController::Persist> MountController::shutdown() {
    std::optional<Persist> p = (phase_ == Phase::Out || phase_ == Phase::Summoning || phase_ == Phase::Desummoning) ? persist_now() : std::nullopt;
    steps_.clear();
    cast_ = 0;
    if (actor_ != 0) registry_.remove(actor_);
    forget_mount();
    pending_effects_ = {};
    return p;
}


}  // namespace aoc::worldd
