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

std::optional<ElementState> marker_item(const repl::FastArrayLayout& layout, int64_t objective_record, const Guid128& objective_instance, const std::vector<MarkerLocation>& locations) {
    const Layout top(layout.element, layout.table);
    ElementState es;
    if (!top.zeroed(es)) return std::nullopt;
    // ContextGuid stays zero: it names the quest instance of a quest shared by many characters (an event); a character's own quest has none
    bool ok = top.set(es, "Items.ObjectiveId", Value::i64(objective_record));
    ok = top.set_guid(es, "Items.RequesterContextGuid", objective_instance) && ok;
    const auto places = top.sub("Items.Locations");
    if (!places) return std::nullopt;
    repl::ArrayValue list;
    for (const MarkerLocation& l : locations) {
        ElementState le;
        if (!places->zeroed(le)) return std::nullopt;
        ok = places->set(le, "Locations.DisplayType", Value::u64(static_cast<uint64_t>(l.display))) && ok;
        ok = places->set(le, "Locations.Center", Value(repl::quantize_vector(1, l.x, l.y, l.z))) && ok;   // whole centimetres
        ok = places->set(le, "Locations.Radius", Value::f32(static_cast<float>(l.radius))) && ok;
        ok = places->set(le, "Locations.bCompletesGoToObjective", Value::boolean(l.completes)) && ok;
        ok = places->set(le, "Locations.QuestLocationId", Value::i64(l.id)) && ok;
        list.elements.push_back(std::move(le));   // (MapMarkerOverride stays unset)
    }
    es.values[top.handle("Items.Locations")] = Value(std::move(list));
    if (!ok) return std::nullopt;
    return es;
}

uint8_t giver_flavor(const std::string& data_flavor) {
    // EQuestFlavor: Quest, Commission, TreasureMap, Event, Collection, Module, Destiny, None, HiddenEvent
    static const char* const kNames[] = {"Quest", "Commission", "TreasureMap", "Event", "Collection", "Module", "Destiny", "None", "HiddenEvent"};
    for (uint8_t i = 0; i < 9; ++i) {
        if (data_flavor == kNames[i]) return i;
    }
    return 0;
}

std::optional<ElementState> giver_item(const repl::FastArrayLayout& layout, int64_t instance, uint8_t flavor, int64_t registration) {
    const Layout top(layout.element, layout.table);
    ElementState es;
    if (!top.zeroed(es)) return std::nullopt;
    // the giver is the NPC itself (its id is also the map's key); every id carries its record type, the zero registration too; the names stay None
    bool ok = top.set(es, "Items.Value.QuestGiverId.Guid", Value::i64(instance));
    ok = top.set(es, "Items.Value.QuestGiverId.TypeId", Value::i64(kPopulationInstanceTypeId)) && ok;
    ok = top.set(es, "Items.Value.QuestFlavor", Value::u64(flavor)) && ok;
    ok = top.set(es, "Items.Value.MapRegistrationId.Guid", Value::i64(registration)) && ok;
    ok = top.set(es, "Items.Value.MapRegistrationId.TypeId", Value::i64(kMapRegistrationTypeId)) && ok;
    ok = top.set(es, "Items.Key.Guid", Value::i64(instance)) && ok;
    ok = top.set(es, "Items.Key.TypeId", Value::i64(kPopulationInstanceTypeId)) && ok;
    if (!ok) return std::nullopt;
    return es;
}

bool sync(repl::FastArrays& arrays, QuestLog& log, std::string* error, MarkerContext* markers) {
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
    // the map pins: an active objective with places to mark and no item yet gets one, in the update that makes it active (the client matches the item to the objective by their ids)
    // (a pin that cannot be made is counted and left out: the quests do not depend on it)
    if (markers && markers->book && markers->points) {
        repl::FastArrayState* marks = arrays.get(*cls, "ObjectiveMarkerCache");
        for (QuestInstance& q : log.instances_for_wire()) {
            if (q.status != Status::Active) continue;
            const content::QuestRow* def = markers->book->quest(q.record);
            if (!def) continue;
            for (size_t i = 0; i < q.objectives.size() && i < def->objectives.size(); ++i) {
                ObjectiveState& o = q.objectives[i];
                if (!o.active || o.completed || o.failed || o.marker_id != repl::kIndexNone) continue;
                const std::vector<MarkerLocation> places = compute_locations(def->objectives[i], *markers->points);
                if (places.empty()) continue;
                const auto item = marks ? marker_item(marks->layout(), o.record, o.guid, places) : std::nullopt;
                const auto id = item ? marks->add(*item) : std::nullopt;
                if (!id) {
                    ++markers->refused;
                    continue;
                }
                o.marker_id = *id;
                markers->made.push_back({o.record, q.record, places.size(), places.front().x, places.front().y, places.front().radius, places.front().display == MarkerLocation::Display::Area});
            }
        }
    }
    return true;
}

}  // namespace aoc::worldd::quest
