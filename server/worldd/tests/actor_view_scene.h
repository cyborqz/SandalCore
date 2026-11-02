// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/actor_view_scene.h: mobs for the checks of the shared updates (tests/actor_view_shadow_redo_tests.inc, tests/actor_view_shared_updates_tests.inc)
//
//  Included by the test programs after their own helpers (make_npc, guid, link, schema_named): a mob with the components the loader builds, each with a random half of its
//  replicated values set from a small domain, and the generator of those values.  Integers only, so that a scene is the same on every machine.
// ============================================================================
#pragma once

#include "combat/npc_ability_replicator.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace view_scene {

struct Rng {
    uint64_t s;
    uint32_t next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<uint32_t>(s >> 33);
    }
    int64_t below(int64_t n) { return static_cast<int64_t>(next()) % n; }
    bool chance(int percent) { return below(100) < percent; }
};

/// A value for a scalar command from a small domain (pick 0 is the zero value), none for a kind the test does not build.
inline std::optional<repl::Value> scalar_value(const schema::RepCommand& c, int64_t pick) {
    using T = schema::PropType;
    switch (c.type) {
        case T::Bool: return repl::Value::boolean((pick & 1) != 0);
        case T::Int8:
        case T::Int16:
        case T::Int32:
        case T::Int64: {
            const int64_t room = c.width_bits == 0 || c.width_bits >= 4 ? 4 : (int64_t{1} << (c.width_bits - 1));
            return repl::Value::i64(pick % room);
        }
        case T::UInt8:
        case T::UInt16:
        case T::UInt32:
        case T::UInt64:
        case T::Enum: {
            const int64_t room = c.width_bits == 0 || c.width_bits >= 3 ? 4 : (int64_t{1} << c.width_bits);
            return repl::Value::u64(static_cast<uint64_t>(pick % room));
        }
        case T::Float: return repl::Value::f32(0.5f * static_cast<float>(pick));
        case T::Double: return repl::Value::f64(0.5 * static_cast<double>(pick));
        case T::Name: return pick == 0 ? repl::Value(repl::NameValue::hardcoded_name(0)) : repl::Value(repl::NameValue::plain("n" + std::to_string(pick)));
        case T::Str: return repl::Value::str(std::string(static_cast<size_t>(pick), 'a'));
        case T::Object: {
            FIntrepidNetworkGUID g;
            if (pick != 0) g = guid(0x7000 + 2 * static_cast<uint64_t>(pick));
            return repl::Value(g);
        }
        default: return std::nullopt;
    }
}

/// An array of up to three elements whose element commands are scalars (about half of them set).
inline std::optional<repl::Value> array_value(const schema::ClassSchema& sc, const schema::RepCommand& c, Rng& rng) {
    if (c.elem_end <= c.elem_begin || c.elem_end > sc.element_commands.size()) return std::nullopt;
    const auto elems = sc.element_commands.subspan(c.elem_begin, c.elem_end - c.elem_begin);
    repl::ArrayValue a;
    const int n = static_cast<int>(rng.below(4));
    for (int i = 0; i < n; ++i) {
        repl::ElementState es;
        for (size_t k = 0; k < elems.size(); ++k) {
            if (!rng.chance(55)) continue;
            if (auto v = scalar_value(elems[k], rng.below(4))) es.values[static_cast<uint16_t>(k + 1)] = std::move(*v);
        }
        a.elements.push_back(std::move(es));
    }
    return repl::Value(std::move(a));
}

/// The value a command can be set to in this test: a scalar, or an array of scalars.
inline std::optional<repl::Value> any_value(const schema::ClassSchema& sc, const schema::RepCommand& c, Rng& rng) {
    if (c.flags & schema::kRepCustomDelta) return std::nullopt;
    if (c.type == schema::PropType::DynamicArray) return array_value(sc, c, rng);
    return scalar_value(c, rng.below(4));
}

/// Object 0 is the actor, object i the (i-1)th subobject.
inline repl::ObjectState& object_of(repl::Actor& a, size_t i) { return i == 0 ? a.obj : a.subobjects[i - 1].obj; }

/// A mob with the components of the ones the loader builds, each with about half of its replicated values set.
inline world::WorldActor make_real_npc(uint64_t id, double x, double y, Rng& rng) {
    world::WorldActor w = make_npc(id, x, y, "Wolf");   // the actor, its info and its combat info
    const auto add = [&](const char* schema_name, const char* name, uint32_t checksum, uint64_t offset) {
        repl::Subobject s;
        s.guid = guid(id + offset);
        s.name = name;
        s.checksum = checksum;
        s.obj.schema = &schema_named(schema_name);
        w.actor.subobjects.push_back(std::move(s));
    };
    add("UAlignmentComponent", "AlignmentComponent", 1069384348u, 10);
    add("UAoCAbilityComponent", "AbilityComponent", 2657332828u, 12);
    add("UInteractableComponent", "InteractableComponent", 2170799233u, 14);
    for (size_t i = 0; i <= w.actor.subobjects.size(); ++i) {
        repl::ObjectState& o = object_of(w.actor, i);
        for (const schema::RepCommand& c : o.schema->commands) {
            if (c.type == schema::PropType::NetSerializeStruct || !rng.chance(50)) continue;
            if (const auto v = any_value(*o.schema, c, rng)) o.state.set(*o.schema, c.handle, *v);
        }
    }
    return w;
}

/// Makes the mob one a player can interact with: its InteractableComponent names an interaction definition (world::is_interactable), so every view that has a player draws it with the
/// player's own guid in its interaction maps.
inline void make_interactable(world::WorldActor& w, int64_t definition) {
    for (repl::Subobject& s : w.actor.subobjects) {
        if (std::string(s.obj.schema->name) != "UInteractableComponent") continue;
        repl::ArrayValue inputs;
        repl::ElementState es;
        es.values[6] = repl::Value::i64(definition);   // InteractionInputData.PlayerInteractDefinition.Guid
        inputs.elements.push_back(std::move(es));
        s.obj.state.set(*s.obj.schema, "InteractionInputData", repl::Value(std::move(inputs)));
    }
}

}  // namespace view_scene
