// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  spawns/mount_state.cpp
//
//  See mount_state.h.
// ============================================================================
#include "spawns/mount_state.h"

#include "bootstrap/constants.h"
#include "spawns/spawn_util.h"
#include "world/interaction_state.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace aoc::worldd::spawns {

using repl::Value;

namespace {

Value word_value(uint32_t w) { return Value::i64(static_cast<int32_t>(w)); }

bool fail(std::string& error, const char* what) {
    error = what;
    return false;
}

repl::ElementState guid_element(const repl::GuidValue& g) {
    repl::ElementState e;
    e.values[1] = word_value(g.a);
    e.values[2] = word_value(g.b);
    e.values[3] = word_value(g.c);
    e.values[4] = word_value(g.d);
    return e;
}

/// The array value of a property as the object holds it now (empty when it holds none).
repl::ArrayValue array_of(const repl::ObjectState& o, const char* path) {
    if (const repl::Value* v = o.state.get(*o.schema, path)) {
        if (const auto* a = v->get_if<repl::ArrayValue>()) return *a;
    }
    return {};
}
// C O A and C.Y.B

/// Replaces the stat items of one owner array by `rows`: the items it holds are changed in order, the rest of `rows` added; items beyond `rows` stay.
bool fill_array(repl::ObjectState& o, const char* field, const std::vector<repl::ElementState>& rows) {
    repl::FastArrayState* arr = o.arrays.get(*o.schema, field);
    if (!arr) return false;
    std::vector<int32_t> ids;
    for (const repl::FastArrayItem& it : arr->items()) ids.push_back(it.id);
    for (size_t i = 0; i < rows.size(); ++i) {
        if (i < ids.size()) {
            if (!arr->change(ids[i], rows[i])) return false;
        } else if (!arr->add(rows[i])) {
            return false;
        }
    }
    return true;
}

}  // namespace

repl::Subobject* sub_of(world::WorldActor& a, const char* cls) {
    for (repl::Subobject& s : a.actor.subobjects) {
        if (s.obj.schema && std::string(s.obj.schema->name) == cls) return &s;
    }
    return nullptr;
}

const repl::Subobject* sub_of(const world::WorldActor& a, const char* cls) {
    for (const repl::Subobject& s : a.actor.subobjects) {
        if (s.obj.schema && std::string(s.obj.schema->name) == cls) return &s;
    }
    return nullptr;
}

void write_motion(world::WorldActor& a, double x, double y, double z, double yaw, double vx, double vy, bool moving, double stamp) {
    repl::ObjectState& obj = a.actor.obj;
    if (!obj.schema) return;
    repl::RepMovement m;
    if (const repl::Value* v = obj.state.get(7)) {
        if (const auto* old = v->get_if<repl::RepMovement>()) m = *old;
    }
    m.simulated_physic_sleep = false;
    m.rep_physics = false;
    m.location = repl::quantize_location(obj.opts, x, y, z);
    m.linear_velocity = repl::quantize_velocity(obj.opts, vx, vy, 0.0);
    m.angular_velocity = repl::QuantVec{repl::QuantVec::Mode::Scaled, 0, 0, 0};
    m.acceleration.reset();
    m.rotation = repl::Rot{0, obj.opts.rep_movement_short_rotation ? util::angle16(yaw) : util::angle8(yaw), 0};
    if (obj.schema->command("ReplicatedMovement")) obj.state.set(*obj.schema, "ReplicatedMovement", repl::Value(m));
    if (obj.schema->command("ReplicatedServerLastTransformUpdateTimeStamp"))
        obj.state.set(*obj.schema, "ReplicatedServerLastTransformUpdateTimeStamp", repl::Value::f32(static_cast<float>(stamp)));
    if (obj.schema->command("CurrentMovementInput"))
        obj.state.set(*obj.schema, "CurrentMovementInput", repl::Value(repl::PlayerMovementInput{static_cast<uint8_t>(moving ? repl::PlayerMovementInput::kForward : 0)}));
    a.actor.open.spawn.location = repl::quantize_vector(10, x, y, z);
    a.actor.open.spawn.rotation = repl::Rot{0, util::angle16(yaw), 0};
    a.actor.open.spawn.velocity.reset();
}

static bool possess_in(world::WorldActor& mount, const Possession& p, std::string& error) {
    repl::ObjectState& actor = mount.actor.obj;
    repl::Subobject* stats = sub_of(mount, "UAoCStatsComponent");
    repl::Subobject* ability = sub_of(mount, "UAoCAbilityComponent");
    repl::Subobject* info = sub_of(mount, "UNPCSecondaryInfo");
    repl::Subobject* inter = sub_of(mount, "UInteractableComponent");
    if (!actor.schema || !stats || !ability || !info || !inter) return fail(error, "the mount lacks a component riding writes to");
    const auto set = [&](repl::ObjectState& o, const char* path, Value v) { return o.state.set(*o.schema, path, std::move(v)); };
    if (!set(actor, "RemoteRole", Value::u64(2)) || !set(actor, "Owner", Value(p.player_controller)) || !set(actor, "PlayerState", Value(p.player_state)) ||
        !set(actor, "Controller", Value(p.player_controller))) {
        return fail(error, "the mount's actor does not take its possession");
    }
    if (!fill_array(stats->obj, "StatRepInt32Owner", p.owner_stats) || !fill_array(stats->obj, "StatRepInt32OwnerProxy", p.owner_proxy_stats)) {
        return fail(error, "the mount's owner stat arrays do not take the rider's stats");
    }
    repl::ArrayValue known;
    {
        repl::ElementState e;
        e.values[1] = Value::i64(p.dash_ability);
        known.elements.push_back(std::move(e));
    }
    if (p.dash_ability != 0 && !set(ability->obj, "KnownAbilities", Value(std::move(known)))) return fail(error, "the mount's AbilityComponent does not take its abilities");
    if (!set(info->obj, "RiderPtr", Value(p.rider_pawn))) return fail(error, "the mount's InteractInfo does not take its rider");
    // the interaction: input 2 was used by the rider's pawn, and its state is 0
    {
        repl::ArrayValue statuses = array_of(inter->obj, "InputEffectStatuses");
        statuses.elements.resize(std::max<size_t>(statuses.elements.size(), 4));
        repl::ElementState& e = statuses.elements[kMountInput];
        e.values[1] = Value::u64(kInputEffectStatus);
        e.values[2] = Value::boolean(true);
        e.values[3] = Value(p.rider_pawn);
        if (!set(inter->obj, "InputEffectStatuses", Value(std::move(statuses)))) return fail(error, "the mount's interaction does not take its status");
        repl::ArrayValue states = array_of(inter->obj, "CurrentInputStates");
        states.elements.resize(std::max<size_t>(states.elements.size(), 4));
        repl::ElementState& s = states.elements[kMountInput];
        s.values[1] = Value::u64(0);
        s.values[2] = Value(repl::ArrayValue{});
        if (!set(inter->obj, "CurrentInputStates", Value(std::move(states)))) return fail(error, "the mount's interaction does not take its state");
    }
    return true;
}

static bool release_in(world::WorldActor& mount, const repl::GuidValue& owner, std::string& error) {
    repl::ObjectState& actor = mount.actor.obj;
    repl::Subobject* stats = sub_of(mount, "UAoCStatsComponent");
    repl::Subobject* info = sub_of(mount, "UNPCSecondaryInfo");
    repl::Subobject* inter = sub_of(mount, "UInteractableComponent");
    if (!actor.schema || !stats || !info || !inter) return fail(error, "the mount lacks a component riding writes to");
    const auto set = [&](repl::ObjectState& o, const char* path, Value v) { return o.state.set(*o.schema, path, std::move(v)); };
    const protocol::emit::FIntrepidNetworkGUID none;
    if (!set(actor, "RemoteRole", Value::u64(1)) || !set(actor, "Owner", Value(none)) || !set(actor, "PlayerState", Value(none)) ||
        !set(actor, "Controller", Value(none))) {
        return fail(error, "the mount's actor does not take its release");
    }
    if (!set(info->obj, "RiderPtr", Value(none))) return fail(error, "the mount does not let its rider go");
    repl::ArrayValue states = array_of(inter->obj, "CurrentInputStates");
    states.elements.resize(std::max<size_t>(states.elements.size(), 4));
    repl::ElementState& s = states.elements[kMountInput];
    s.values[1] = Value::u64(2);
    repl::ArrayValue guids;
    guids.elements.push_back(guid_element(owner));
    s.values[2] = Value(std::move(guids));
    if (!set(inter->obj, "CurrentInputStates", Value(std::move(states)))) return fail(error, "the mount's interaction does not take its state");
    return true;
}

bool possess(world::WorldActor& mount, const Possession& p, std::string& error) {
    world::WorldActor backup = mount;
    if (possess_in(mount, p, error)) return true;
    mount = std::move(backup);   // a refused part leaves nothing half done
    return false;
}

bool release(world::WorldActor& mount, const repl::GuidValue& owner, std::string& error) {
    world::WorldActor backup = mount;
    if (release_in(mount, owner, error)) return true;
    mount = std::move(backup);
    return false;
}
// -- C Y B & C-O-A --

std::optional<double> stat_value(const world::WorldActor& mount, int64_t record) {
    const repl::Subobject* stats = sub_of(mount, "UAoCStatsComponent");
    if (!stats) return std::nullopt;
    const repl::FastArrayState* arr = stats->obj.arrays.find(stats->obj.schema->field("StatRepInt32EveryoneProxy")->index);
    if (!arr) return std::nullopt;
    for (const repl::FastArrayItem& it : arr->items()) {
        const auto f = it.values.values.find(1);
        const auto* t = f == it.values.values.end() ? nullptr : f->second.get_if<repl::StatInt32Inst>();
        if (t && t->record == record) return static_cast<double>(std::bit_cast<float>(t->value));
    }
    return std::nullopt;
}

bool set_stat_value(world::WorldActor& mount, int64_t record, float value) {
    repl::Subobject* stats = sub_of(mount, "UAoCStatsComponent");
    if (!stats || !std::isfinite(value)) return false;
    repl::FastArrayState* arr = stats->obj.arrays.get(*stats->obj.schema, "StatRepInt32EveryoneProxy");
    if (!arr) return false;
    for (const repl::FastArrayItem& it : arr->items()) {
        const auto f = it.values.values.find(1);
        const auto* t = f == it.values.values.end() ? nullptr : f->second.get_if<repl::StatInt32Inst>();
        if (!t || t->record != record) continue;
        repl::StatInt32Inst fresh = *t;
        fresh.value = std::bit_cast<uint32_t>(value);
        repl::ElementState values = it.values;
        values.values[1] = Value(fresh);
        return arr->change(it.id, std::move(values));
    }
    return false;
}

std::optional<int32_t> add_effect(world::WorldActor& mount, const repl::GuidValue& instigator, const bootstrap::EffectItem& item) {
    repl::Subobject* stats = sub_of(mount, "UAoCStatsComponent");
    if (!stats) return std::nullopt;
    repl::FastArrayState* arr = stats->obj.arrays.get(*stats->obj.schema, "CurrentEffects");
    if (!arr) return std::nullopt;
    return arr->add(bootstrap::effect_item_state({instigator.a, instigator.b, instigator.c, instigator.d}, item));
}

bool remove_effect(world::WorldActor& mount, int32_t array_id) {
    repl::Subobject* stats = sub_of(mount, "UAoCStatsComponent");
    repl::FastArrayState* arr = stats ? stats->obj.arrays.get(*stats->obj.schema, "CurrentEffects") : nullptr;
    return arr && arr->remove(array_id);
}

std::optional<int32_t> add_cooldown(world::WorldActor& mount, const bootstrap::CooldownItem& item) {
    repl::Subobject* ab = sub_of(mount, "UAoCAbilityComponent");
    if (!ab) return std::nullopt;
    repl::FastArrayState* arr = ab->obj.arrays.get(*ab->obj.schema, "Cooldowns");
    if (!arr) return std::nullopt;
    return arr->add(bootstrap::cooldown_item_state(item));
}

bool remove_cooldown(world::WorldActor& mount, int32_t array_id) {
    repl::Subobject* ab = sub_of(mount, "UAoCAbilityComponent");
    repl::FastArrayState* arr = ab ? ab->obj.arrays.get(*ab->obj.schema, "Cooldowns") : nullptr;
    return arr && arr->remove(array_id);
}

bool set_view_pitch(world::WorldActor& mount, uint16_t pitch16) {
    repl::ObjectState& obj = mount.actor.obj;
    return obj.schema && obj.schema->command("RemoteViewPitch16") && obj.state.set(*obj.schema, "RemoteViewPitch16", Value::u64(pitch16));
}

bool set_interaction_open(world::WorldActor& mount, bool open) {
    repl::Subobject* inter = sub_of(mount, "UInteractableComponent");
    if (!inter) return false;
    repl::ArrayValue values;
    for (int k = 0; k < 4; ++k) {
        repl::ElementState e;
        e.values[1] = Value::boolean(open);
        values.elements.push_back(std::move(e));
    }
    return inter->obj.state.set(*inter->obj.schema, "CanInputActivate", Value(std::move(values)));
}

bool set_dead(world::WorldActor& mount, bool dead) {
    repl::Subobject* combat = sub_of(mount, "UNPCCombatInfo");
    return combat && combat->obj.state.set(*combat->obj.schema, "bIsDead", Value::boolean(dead));
}

}  // namespace aoc::worldd::spawns
