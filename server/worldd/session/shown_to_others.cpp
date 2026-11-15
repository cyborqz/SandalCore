// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/shown_to_others.cpp  (Social, seeing other players)
//
//  See shown_to_others.h.
// ============================================================================
#include "session/shown_to_others.h"

#include "protocol/repl/move_packed.h"
#include "spawns/mount_state.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace aoc::worldd {

namespace repl = ::aoc::protocol::repl;
using repl::Value;

namespace {

/// Takes every item out of a fast array of the object (nothing when it has none).  The last item first: taking the first one out moves every item after it, which made this the larger
/// part of the cost of every other player's copy of a ridden mount (a rider's stat arrays hold a hundred items each).
void empty_fast_array(repl::ObjectState& o, const char* field) {
    if (!o.schema || !o.schema->field(field)) return;
    repl::FastArrayState* arr = o.arrays.get(*o.schema, field);
    if (!arr) return;
    std::vector<int32_t> ids;
    ids.reserve(arr->items().size());
    for (const repl::FastArrayItem& it : arr->items()) ids.push_back(it.id);
    for (auto id = ids.rbegin(); id != ids.rend(); ++id) arr->remove(*id);
}

/// What only the owner of a mount is sent (the replication conditions: owner only): the stats of its rider that its HUD reads, and the dash.
void leave_out_the_owners_parts(repl::Actor& a) {
    for (repl::Subobject& s : a.subobjects) {
        if (!s.obj.schema) continue;
        const std::string_view cls = s.obj.schema->name;
        if (cls == "UAoCStatsComponent") {
            empty_fast_array(s.obj, "StatRepInt32Owner");
            empty_fast_array(s.obj, "StatRepInt32OwnerProxy");
        } else if (cls == "UAoCAbilityComponent") {
            if (const auto* known = s.obj.schema->command("KnownAbilities")) s.obj.state.erase(known->handle);   // (not an empty list: nothing of the property is sent)
            empty_fast_array(s.obj, "Cooldowns");
        }
    }
}

/// A mount its owner rides, as a simulated proxy: no owner, no controller, and the motion the owner's moves gave it.
void show_the_ridden_mount(const world::WorldActor& w, repl::Actor& a) {
    repl::ObjectState& obj = a.obj;
    if (!obj.schema) return;
    const protocol::emit::FIntrepidNetworkGUID none;
    const auto set = [&](const char* path, Value v) {
        if (obj.schema->command(path)) obj.state.set(*obj.schema, path, std::move(v));
    };
    set("RemoteRole", Value::u64(1));
    set("Owner", Value(none));
    set("PlayerState", Value(none));
    set("Controller", Value(none));
    const world::MountShown& m = w.mount_shown;
    spawns::write_motion(a, w.x, w.y, w.z, w.yaw_degrees, m.vx, m.vy, m.vz, m.moving, m.stamp);
    set("ReplicatedMovementMode", Value::u64(m.mode));
}

}  // namespace

uint8_t shown_movement_mode(uint8_t mode) {
    switch (mode) {
        case repl::kMoveWalking:
        case repl::kMoveFalling:
        case repl::kMoveSwimming:
        case repl::kMoveFlying:
        case 16:
        case 17:
        case 18:
            return mode;
        default:
            return repl::kMoveWalking;
    }
}

const repl::Actor& form_for_others(const world::WorldActor& a, repl::Actor& scratch) {
    if (a.kind != world::ActorKind::Mount || !a.mount_shown.rode) return a.actor;
    scratch = a.actor;
    leave_out_the_owners_parts(scratch);
    if (a.mount_shown.possessed) show_the_ridden_mount(a, scratch);
    return scratch;
}

}  // namespace aoc::worldd
