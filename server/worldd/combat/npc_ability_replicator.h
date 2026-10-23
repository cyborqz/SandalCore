// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  combat/npc_ability_replicator.h
//
//  Writes the attack a creature is playing into the ReplicatedActiveAbilities array of its AbilityComponent (world/actor_registry.h), the array a client
//  plays a creature's animation from.  One element per creature at a time:
//
//    start    a new element with the ability, the target, the montage and the animation counter; the element of the cast before is deleted in the same step
//    end      the same element again with the montage cleared and the animation counter one higher: the montage is over
//    clear    the end, written when the creature dies or its fight is reset
//
//  The client starts the montage the moment it gets the element and stops it at the end write, so the element is written when the attack begins and its
//  hits are confirmed to the player afterwards, never before.  The montage's export (its path) rides with the update that first names it
//  (WorldActor::ability_exports).  Header only; combat_world.cpp is the caller.
// ============================================================================
#pragma once

#include "protocol/emit/intrepid_netguid.h"
#include "protocol/repl/replicator.h"
#include "world/actor_registry.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aoc::worldd::combat {

namespace repl = ::aoc::protocol::repl;
namespace emit = ::aoc::protocol::emit;

/// What the replicator keeps of one creature's element between its writes.
struct NpcCastState {
    int32_t element = 0;        // the live element's ReplicationID (0: none)
    int32_t counter = 0;        // the AnimationCounter the next write carries (it moves by one on every write)
    int64_t next_instance = 0;  // the ServerInstanceId the next cast carries (0 for the first)
    bool    playing = false;    // the element holds a montage (the end has not been written)
};

/// One cast.
struct NpcCast {
    int64_t                 ability = 0;
    uint64_t                target = 0;            // the actor the attack is aimed at (0: none)
    double                  tx = 0, ty = 0, tz = 0;   // ... and where it stands (cm)
    uint64_t                montage = 0;           // the montage object's id
    float                   play_rate = 1.0f;
    float                   position = 0.0f;
};

namespace npc_ability_detail {

inline repl::Subobject* ability_component(world::WorldActor& a) {
    for (repl::Subobject& s : a.actor.subobjects) {
        if (s.obj.schema && std::string(s.obj.schema->name) == "UAoCAbilityComponent") return &s;
    }
    return nullptr;
}

inline repl::FastArrayState* active_abilities(world::WorldActor& a) {
    repl::Subobject* s = ability_component(a);
    return s ? s->obj.arrays.get(*s->obj.schema, "ReplicatedActiveAbilities") : nullptr;
}

/// The element's 17 members, in the order of the item: the ability, the cast's number, its target and where the target stood, the montage group and the
/// montage (cleared when `montage` is 0), the play rate and start position, the animation counter and the creature itself.
inline repl::ElementState item_state(const NpcCast& c, int64_t instance, uint64_t montage, int32_t counter, uint64_t instigator) {
    repl::ElementState es;
    auto& v = es.values;
    v[1] = repl::Value::i64(c.ability);
    v[2] = repl::Value::i64(instance);
    repl::ArrayValue targets;
    repl::ArrayValue transforms;
    if (c.target != 0) {
        repl::ElementState t;
        t.values[1] = repl::Value(emit::FIntrepidNetworkGUID::from_u64(c.target));
        targets.elements.push_back(std::move(t));
    }
    {
        repl::ElementState tt;   // where the target stood when the attack began; the creature faces it
        tt.values[1] = repl::Value(repl::Quat{0.0, 0.0, 0.0});
        tt.values[2] = repl::Value(repl::Vec3d{c.tx, c.ty, c.tz});
        tt.values[3] = repl::Value(repl::Vec3d{1.0, 1.0, 1.0});
        transforms.elements.push_back(std::move(tt));
    }
    v[3] = repl::Value(std::move(targets));
    v[4] = repl::Value(std::move(transforms));
    v[5] = repl::Value::boolean(false);         // bPvpForceFlag
    v[6] = repl::Value::i64(0);                 // ReplicatedProgressInfo.NumDisruptiveHits
    v[7] = repl::Value::f32(0);                 // .TotalDisruptionSeconds
    v[8] = repl::Value::f32(0);                 // .CastDuration
    v[9] = repl::Value::boolean(true);          // bBranchPredictedOnAutonomous
    v[10] = repl::Value(repl::NameValue::plain("DefaultGroup"));
    v[11] = repl::Value(emit::FIntrepidNetworkGUID::from_u64(montage));
    v[12] = repl::Value::f32(c.play_rate);
    v[13] = repl::Value::f32(c.position);
    v[14] = repl::Value::boolean(false);        // bMayJumpToPosition
    v[15] = repl::Value::i64(counter);
    v[16] = repl::Value::u64(0);                // MeshYaw
    v[17] = repl::Value(emit::FIntrepidNetworkGUID::from_u64(instigator));
    return es;
}

}  // namespace npc_ability_detail

/// Whether the creature was opened with the record of `ability` among the ones its AbilityComponent loads (ReplicatedRecordsToLoad): the client plays only the abilities it has the
/// record of, so a creature opened from a template that lists none of its own (a donor's) must not be told to play one.
inline bool npc_knows_ability(const world::WorldActor& a, int64_t ability) {
    for (const repl::Subobject& s : a.actor.subobjects) {
        if (!s.obj.schema || std::string(s.obj.schema->name) != "UAoCAbilityComponent") continue;
        const repl::Value* v = s.obj.state.get(*s.obj.schema, "ReplicatedRecordsToLoad");
        const repl::ArrayValue* list = v ? v->get_if<repl::ArrayValue>() : nullptr;
        if (!list) return false;
        for (const repl::ElementState& e : list->elements) {
            const auto it = e.values.find(1);
            const int64_t* guid = it == e.values.end() ? nullptr : it->second.get_if<int64_t>();
            if (guid && *guid == ability) return true;
        }
        return false;
    }
    return false;
}

/// Starts a cast: the element of the cast before goes, a new one with the montage comes.  `montage_export` is the montage's export chain (its path),
/// kept on the actor until the update that names it has gone out.  False when the actor is not held or has no AbilityComponent.
inline bool start_npc_cast(world::ActorRegistry& registry, uint64_t actor_id, NpcCastState& state, const NpcCast& cast, const repl::ExportChain& montage_export) {
    bool ok = false;
    registry.modify(actor_id, world::Change::State, [&](world::WorldActor& a) {
        repl::FastArrayState* arr = npc_ability_detail::active_abilities(a);
        if (!arr) return;
        if (state.element != 0) arr->remove(state.element);
        state.element = 0;
        const int64_t instance = state.next_instance;
        const std::optional<int32_t> id = arr->add(npc_ability_detail::item_state(cast, instance, cast.montage, state.counter, actor_id));
        if (!id) return;
        state.element = *id;
        ++state.counter;
        ++state.next_instance;
        state.playing = true;
        a.ability_exports.assign(1, montage_export);
        ok = true;
    });
    return ok;
}

/// Ends the live cast: the same element with the montage cleared.  False when there is none playing.
inline bool end_npc_cast(world::ActorRegistry& registry, uint64_t actor_id, NpcCastState& state) {
    if (state.element == 0 || !state.playing) return false;
    bool ok = false;
    registry.modify(actor_id, world::Change::State, [&](world::WorldActor& a) {
        repl::FastArrayState* arr = npc_ability_detail::active_abilities(a);
        if (!arr) return;
        const repl::FastArrayItem* live = arr->find(state.element);
        if (!live) return;
        repl::ElementState es = live->values;
        es.values[11] = repl::Value(emit::FIntrepidNetworkGUID{});
        es.values[15] = repl::Value::i64(state.counter);
        if (!arr->change(state.element, std::move(es))) return;
        ++state.counter;
        a.ability_exports.clear();
        ok = true;
    });
    state.playing = false;
    return ok;
}

}  // namespace aoc::worldd::combat
