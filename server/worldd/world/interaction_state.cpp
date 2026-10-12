// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/interaction_state.cpp
//
//  See interaction_state.h.
// ============================================================================
#include "world/interaction_state.h"

#include <algorithm>
#include <string>

namespace aoc::worldd::world {

namespace repl = ::aoc::protocol::repl;

namespace {

repl::Subobject* interactable(repl::Actor& actor) {
    for (repl::Subobject& s : actor.subobjects) {
        if (s.obj.schema && std::string(s.obj.schema->name) == "UInteractableComponent") return &s;
    }
    return nullptr;
}

const repl::Subobject* interactable(const repl::Actor& actor) {
    for (const repl::Subobject& s : actor.subobjects) {
        if (s.obj.schema && std::string(s.obj.schema->name) == "UInteractableComponent") return &s;
    }
    return nullptr;
}

}  // namespace

std::vector<int64_t> interaction_inputs(const repl::Actor& actor) {
    std::vector<int64_t> keys;
    const repl::Subobject* inter = interactable(actor);
    if (!inter) return keys;
    const repl::Value* data = inter->obj.state.get(*inter->obj.schema, "InteractionInputData");
    const auto* arr = data ? data->get_if<repl::ArrayValue>() : nullptr;
    if (!arr) return keys;
    for (size_t i = 0; i < arr->elements.size(); ++i) {
        const auto g = arr->elements[i].values.find(6);   // InteractionInputData.PlayerInteractDefinition.Guid
        const auto* guid = g == arr->elements[i].values.end() ? nullptr : g->second.get_if<int64_t>();
        if (guid && *guid != 0) keys.push_back(static_cast<int64_t>(i));
    }
    return keys;
}

bool is_interactable(const repl::Actor& actor) { return !interaction_inputs(actor).empty(); }
// ~ C-O-A - C_Y_B ~

bool allow_inputs(repl::Actor& actor, int inputs) {
    repl::Subobject* inter = interactable(actor);
    if (!inter) return false;
    repl::ArrayValue values;
    for (int k = 0; k < inputs; ++k) {
        repl::ElementState e;
        e.values[1] = repl::Value::boolean(true);
        values.elements.push_back(std::move(e));
    }
    return inter->obj.state.set(*inter->obj.schema, "CanInputActivate", repl::Value(std::move(values)));
}

bool set_player(repl::Actor& actor, const repl::GuidValue& player, bool on) {
    repl::Subobject* inter = interactable(actor);
    if (!inter) return false;
    const std::vector<int64_t> keys = interaction_inputs(actor);
    if (keys.empty()) return false;
    for (const char* name : {"InteractVisibleStateMap", "InteractGameplayStateMap"}) {
        repl::FastArrayState* map = inter->obj.arrays.get(*inter->obj.schema, name);
        if (!map) continue;
        for (const int64_t key : keys) {
            const repl::FastArrayItem* item = nullptr;
            for (const repl::FastArrayItem& it : map->items()) {
                const auto k = it.values.values.find(1);
                const auto* kv = k == it.values.values.end() ? nullptr : k->second.get_if<int64_t>();
                if (kv && *kv == key) item = &it;
            }
            repl::GuidToBoolWrapper wrapper;
            if (item) {
                const auto v = item->values.values.find(2);
                if (v != item->values.values.end()) {
                    if (const auto* w = v->second.get_if<repl::GuidToBoolWrapper>()) wrapper = *w;
                }
            }
            wrapper.pairs.erase(std::remove_if(wrapper.pairs.begin(), wrapper.pairs.end(), [&](const auto& p) { return p.first == player; }), wrapper.pairs.end());
            if (on) wrapper.pairs.emplace_back(player, true);
            repl::ElementState values;
            values.values[1] = repl::Value::i64(key);
            values.values[2] = repl::Value(std::move(wrapper));
            if (item) map->change(item->id, std::move(values));
            else map->add(std::move(values));
        }
    }
    return true;
}

}  // namespace aoc::worldd::world
