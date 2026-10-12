// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  combat/npc_replicator.cpp (see npc_replicator.h)
// ============================================================================
#include "combat/npc_replicator.h"

#include <algorithm>
#include <bit>

namespace aoc::worldd::combat {

namespace repl = ::aoc::protocol::repl;

namespace {

repl::Subobject* find_sub(world::WorldActor& a, const char* cls) {
    for (repl::Subobject& s : a.actor.subobjects) {
        if (s.obj.schema && std::string(s.obj.schema->name) == cls) return &s;
    }
    return nullptr;
}

const repl::Subobject* find_sub(const world::WorldActor& a, const char* cls) {
    for (const repl::Subobject& s : a.actor.subobjects) {
        if (s.obj.schema && std::string(s.obj.schema->name) == cls) return &s;
    }
    return nullptr;
}

const repl::StatInt32Inst* stat_of(const repl::FastArrayItem& it) {
    const auto f = it.values.values.find(1);
    return f == it.values.values.end() ? nullptr : f->second.get_if<repl::StatInt32Inst>();
}

// Sets the item of `record` to `value`, adding it when the actor has none (`add`); true when something changed.
bool set_item(world::WorldActor& a, int64_t record, float value, bool add = false) {
    repl::Subobject* st = find_sub(a, "UAoCStatsComponent");
    if (!st || !st->obj.schema) return false;
    repl::FastArrayState* arr = st->obj.arrays.get(*st->obj.schema, "StatRepInt32EveryoneProxy");
    if (!arr) return false;
    const uint32_t bits = std::bit_cast<uint32_t>(value);
    for (const repl::FastArrayItem& it : arr->items()) {
        const repl::StatInt32Inst* s = stat_of(it);
        if (!s || s->record != record) continue;
        if (s->value == bits) return false;
        repl::StatInt32Inst next = *s;
        next.value = bits;
        repl::ElementState values = it.values;
        values.values[1] = repl::Value(next);
        return arr->change(it.id, std::move(values));
    }
    if (!add) return false;
    repl::StatInt32Inst fresh;
    fresh.record = record;
    fresh.value = bits;
    repl::ElementState values;
    values.values[1] = repl::Value(fresh);
    return arr->add(std::move(values)).has_value();
}
// ~ C.O.A & C_Y_B ~

}  // namespace

size_t write_stats(world::ActorRegistry& registry, uint64_t actor_id, const stats::CharacterStats& stats) {
    size_t changed = 0;
    registry.modify(actor_id, world::Change::State, [&](world::WorldActor& a) {
        repl::Subobject* st = find_sub(a, "UAoCStatsComponent");
        if (!st || !st->obj.schema) return;
        repl::FastArrayState* arr = st->obj.arrays.get(*st->obj.schema, "StatRepInt32EveryoneProxy");
        if (!arr) return;
        // every stat of the EveryoneProxy audience (the engine's replication mask 7), and anything the actor was opened with
        std::vector<std::pair<int64_t, float>> todo;
        for (const stats::StatValue& v : stats.values()) {
            if (v.replication == 7 && !v.float_array()) todo.emplace_back(v.guid, v.value);
        }
        for (const repl::FastArrayItem& it : arr->items()) {
            const repl::StatInt32Inst* s = stat_of(it);
            if (!s) continue;
            const stats::StatValue* v = stats.find(s->record);
            if (v && std::none_of(todo.begin(), todo.end(), [&](const auto& t) { return t.first == s->record; })) todo.emplace_back(s->record, v->value);
        }
        for (const auto& [record, value] : todo) {
            if (set_item(a, record, value, true)) ++changed;
        }
    });
    return changed;
}

bool write_stat(world::ActorRegistry& registry, uint64_t actor_id, int64_t record, float value) {
    bool changed = false;
    registry.modify(actor_id, world::Change::State, [&](world::WorldActor& a) { changed = set_item(a, record, value); });
    return changed;
}

bool write_combat(world::ActorRegistry& registry, uint64_t actor_id, bool in_combat, bool dead, uint64_t target_object) {
    bool ok = false;
    registry.modify(actor_id, world::Change::State, [&](world::WorldActor& a) {
        repl::Subobject* ci = find_sub(a, "UNPCCombatInfo");
        if (!ci || !ci->obj.schema) return;
        repl::ObjectState& o = ci->obj;
        ok = true;
        const auto set = [&](const char* path, repl::Value v) {
            if (o.schema->command(path)) o.state.set(*o.schema, path, std::move(v));
        };
        set("bInCombat", repl::Value::boolean(in_combat));
        set("bIsDead", repl::Value::boolean(dead));
        if (o.schema->command("ActiveTargets.ActiveTargets")) {
            repl::ArrayValue targets;
            repl::ElementState e;
            e.values[1] = repl::Value(::aoc::protocol::emit::FIntrepidNetworkGUID::from_u64(target_object));
            if (target_object != 0 || !in_combat) targets.elements.push_back(std::move(e));
            set("ActiveTargets.ActiveTargets", repl::Value(std::move(targets)));
        }
    });
    return ok;
}

bool write_elite_rank(world::ActorRegistry& registry, uint64_t actor_id, int rank) {
    bool ok = false;
    registry.modify(actor_id, world::Change::State, [&](world::WorldActor& a) {
        repl::Subobject* ci = find_sub(a, "UNPCCombatInfo");
        if (!ci || !ci->obj.schema || !ci->obj.schema->command("EliteRank")) return;
        ok = ci->obj.state.set(*ci->obj.schema, "EliteRank", repl::Value::i64(rank));
    });
    return ok;
}

bool read_stat(const world::WorldActor& actor, int64_t record, float& value) {
    const repl::Subobject* st = find_sub(actor, "UAoCStatsComponent");
    if (!st || !st->obj.schema) return false;
    const repl::FastArrayState* arr = const_cast<repl::FastArrays&>(st->obj.arrays).get(*st->obj.schema, "StatRepInt32EveryoneProxy");
    if (!arr) return false;
    for (const repl::FastArrayItem& it : arr->items()) {
        const repl::StatInt32Inst* s = stat_of(it);
        if (s && s->record == record) {
            value = std::bit_cast<float>(s->value);
            return true;
        }
    }
    return false;
}

}  // namespace aoc::worldd::combat
