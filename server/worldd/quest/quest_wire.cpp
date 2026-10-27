// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#include "quest/quest_wire.h"

#include "protocol/repl/rep_block.h"
#include "protocol/schema/generated/index.h"

#include <string_view>

namespace aoc::worldd::quest {

namespace repl = ::aoc::protocol::repl;
namespace schema = ::aoc::protocol::schema;

const schema::ClassSchema* component_schema() { return schema::gen::find_component_schema("QuestConsumerComponent"); }

namespace {

using repl::ElementState;
using repl::Value;

/// The commands of one element layout, found by their dotted path.
class Layout {
public:
    Layout(std::span<const schema::RepCommand> cmds, std::span<const schema::RepCommand> table) : cmds_(cmds), table_(table) {}

    /// Every command with its zero value; false when the layout has a command no zero is known for.
    bool zeroed(ElementState& es) const {
        for (const schema::RepCommand& c : cmds_) {
            auto z = repl::zero_value(c);
            if (!z) return false;
            es.values[c.handle] = std::move(*z);
        }
        return true;
    }

    bool set(ElementState& es, std::string_view path, Value v) const {
        for (const schema::RepCommand& c : cmds_) {
            if (path == c.path) {
                es.values[c.handle] = std::move(v);
                return true;
            }
        }
        return false;
    }

    bool set_guid(ElementState& es, std::string prefix, const Guid128& g) const {
        bool ok = true;
        const uint32_t words[4] = {g.a, g.b, g.c, g.d};
        const char* const letters[4] = {".A", ".B", ".C", ".D"};
        for (int i = 0; i < 4; ++i) ok = set(es, prefix + letters[i], Value::i64(static_cast<int32_t>(words[i]))) && ok;
        return ok;
    }

    /// The element commands of the array command at `path`.
    std::optional<Layout> sub(std::string_view path) const {
        for (const schema::RepCommand& c : cmds_) {
            if (path == c.path && c.type == schema::PropType::DynamicArray && c.elem_end > c.elem_begin && c.elem_end <= table_.size()) {
                return Layout(table_.subspan(c.elem_begin, c.elem_end - c.elem_begin), table_);
            }
        }
        return std::nullopt;
    }

    uint16_t handle(std::string_view path) const {
        for (const schema::RepCommand& c : cmds_) {
            if (path == c.path) return c.handle;
        }
        return 0;
    }

private:
    std::span<const schema::RepCommand> cmds_, table_;
};

/// A zero vector the way the game's server writes it: header 0 and three 32-bit words.
Value zero_vector() { return Value(repl::QuantVec{repl::QuantVec::Mode::RawFloat, 0, 0, 0}); }

}  // namespace

std::optional<ElementState> cache_item(const repl::FastArrayLayout& layout, const QuestInstance& q) {
    const Layout top(layout.element, layout.table);
    ElementState es;
    if (!top.zeroed(es)) return std::nullopt;
    bool ok = true;
    ok = top.set(es, "Quests.InstigatorItemInstance.ItemId.TypeId", Value::i64(kItemRecordTypeId)) && ok;
    ok = top.set(es, "Quests.ActivationStartTime", Value::f64(q.start_time)) && ok;
    ok = top.set(es, "Quests.Locality", Value::u64(1)) && ok;   // ENarrativeLocality::Local
    ok = top.set(es, "Quests.QuestObjectiveMarker.Center", zero_vector()) && ok;
    ok = top.set(es, "Quests.QuestObjectiveMarker.bCompletesGoToObjective", Value::boolean(true)) && ok;
    ok = top.set_guid(es, "Quests.InstanceGuid", q.guid) && ok;
    ok = top.set(es, "Quests.RecordId", Value::i64(q.record)) && ok;
    const auto objectives = top.sub("Quests.Objectives");
    if (!objectives) return std::nullopt;
    repl::ArrayValue list;
    for (const ObjectiveState& o : q.objectives) {
        ElementState oe;
        if (!objectives->zeroed(oe)) return std::nullopt;
        ok = objectives->set_guid(oe, "Objectives.InstanceGuid", o.guid) && ok;
        ok = objectives->set(oe, "Objectives.RecordId", Value::i64(o.record)) && ok;
        ok = objectives->set(oe, "Objectives.ActivationStartTime", Value::f64(o.start_time)) && ok;
        ok = objectives->set(oe, "Objectives.ObjectiveLocation", zero_vector()) && ok;
        list.elements.push_back(std::move(oe));
    }
    es.values[top.handle("Quests.Objectives")] = Value(std::move(list));
    if (!ok) return std::nullopt;
    return es;
}

std::optional<ElementState> variables_item(const repl::FastArrayLayout& layout, const QuestInstance& q) {
    const Layout top(layout.element, layout.table);
    ElementState es;
    if (!top.zeroed(es)) return std::nullopt;
    repl::HashedVariable vars;
    vars.variables = instance_variables(q);
    bool ok = top.set(es, "Items.Value", Value(std::move(vars)));
    ok = top.set_guid(es, "Items.Key", q.guid) && ok;
    if (!ok) return std::nullopt;
    return es;
}

bool sync(repl::FastArrays& arrays, QuestLog& log, std::string* error) {
    const auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    const schema::ClassSchema* cls = component_schema();
    if (!cls) return fail("no schema for the quest component");
    repl::FastArrayState* cache = arrays.get(*cls, "ReplicatedQuestCache");
    repl::FastArrayState* vars = arrays.get(*cls, "RepQuestVariables");
    if (!cache || !vars) return fail("the quest component has no cache or variable array");
    for (const QuestLog::Removed& r : log.take_removed()) {
        if (r.cache_id != repl::kIndexNone) cache->remove(r.cache_id);
        if (r.vars_id != repl::kIndexNone) vars->remove(r.vars_id);
    }
    for (QuestInstance& q : log.instances_for_wire()) {
        if (q.cache_id != repl::kIndexNone && q.vars_id != repl::kIndexNone && !q.wire_dirty) continue;
        const auto ci = cache_item(cache->layout(), q);
        const auto vi = variables_item(vars->layout(), q);
        if (!ci || !vi) return fail("a quest does not fit the layout of the quest component's arrays");
        if (q.cache_id == repl::kIndexNone) {
            const auto id = cache->add(*ci);
            if (!id) return fail("the quest cache refused an item");
            q.cache_id = *id;
        } else if (!cache->change(q.cache_id, *ci)) {
            return fail("the quest cache refused a changed item");
        }
        if (q.vars_id == repl::kIndexNone) {
            const auto id = vars->add(*vi);
            if (!id) return fail("the quest variables refused an item");
            q.vars_id = *id;
        } else if (!vars->change(q.vars_id, *vi)) {
            return fail("the quest variables refused a changed item");
        }
        q.wire_dirty = false;
    }
    return true;
}

}  // namespace aoc::worldd::quest
