// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/node_rpc.cpp
//
//  See node_rpc.h.
// ============================================================================
#include "protocol/repl/node_rpc.h"

#include "protocol/repl/rpc_fields.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <string_view>

namespace aoc { namespace protocol { namespace repl {

using schema::PropType;
using schema::RepCommand;

namespace {

bool is_rpc(const schema::RpcSchema& rpc, std::string_view name) { return rpc.name && name == rpc.name; }
bool decoded_as(const RpcCall& call, std::string_view name) { return call.rpc && call.decoded && is_rpc(*call.rpc, name); }

// ---- the GameState's arrays: the handles of each item (the index + 1 of the member in the item's commands) ------------------------------------------

constexpr const char* kEstates = "EstateIDToOwnerGuid";
constexpr const char* kNodeData = "NodeIDToClientNodeData";
constexpr const char* kPercent = "NodePercentLevelCompleteFastMap";
constexpr const char* kCitizens = "NodeNumberOfCitizensFastMap";

Value word(uint32_t w) { return Value::i64(static_cast<int32_t>(w)); }   // an Int32 member

ElementState estate_item(int64_t key, const std::array<uint32_t, 4>& owner) {
    ElementState e;
    e.values[1] = Value::i64(key);
    for (size_t i = 0; i < 4; ++i) e.values[static_cast<uint16_t>(2 + i)] = word(owner[i]);
    return e;
}

ElementState node_item(const NodeDataEntry& n) {
    ElementState e;
    e.values[1] = Value::i64(n.guid);
    e.values[2] = Value::u64(n.level);
    e.values[3] = Value::u64(n.culture);
    for (size_t i = 0; i < 4; ++i) e.values[static_cast<uint16_t>(4 + i)] = word(n.mayor_guid[i]);
    e.values[8] = Value::str(n.mayor_name);
    e.values[9] = Value::i64(n.parent);
    e.values[10] = Value::i64(n.bounty);
    GameplayTagContainer tags;
    for (const std::string& t : n.tags) tags.tags.push_back(NameValue::plain(t));
    e.values[11] = Value(std::move(tags));
    e.values[12] = Value::f32(n.deploy_radius);
    return e;
}
// C:Y:B / CoA

ElementState small_item(uint64_t key, uint8_t value) {
    ElementState e;
    e.values[1] = Value::u64(value);
    e.values[2] = Value::u64(key);
    return e;
}

int64_t i64_of(const ElementState& e, uint16_t h) {
    const auto it = e.values.find(h);
    const int64_t* v = it == e.values.end() ? nullptr : it->second.get_if<int64_t>();
    return v ? *v : 0;
}
uint64_t u64_of(const ElementState& e, uint16_t h) {
    const auto it = e.values.find(h);
    const uint64_t* v = it == e.values.end() ? nullptr : it->second.get_if<uint64_t>();
    return v ? *v : 0;
}

std::array<uint32_t, 4> words_of(const ElementState& e, uint16_t first) {
    std::array<uint32_t, 4> w{};
    for (size_t i = 0; i < 4; ++i) w[i] = static_cast<uint32_t>(i64_of(e, static_cast<uint16_t>(first + i)));
    return w;
}

NodeDataEntry node_entry(const ElementState& e) {
    NodeDataEntry n;
    n.guid = i64_of(e, 1);
    n.level = static_cast<uint8_t>(u64_of(e, 2));
    n.culture = static_cast<uint8_t>(u64_of(e, 3));
    n.mayor_guid = words_of(e, 4);
    if (const auto it = e.values.find(8); it != e.values.end()) {
        if (const std::string* s = it->second.get_if<std::string>()) n.mayor_name = *s;
    }
    n.parent = i64_of(e, 9);
    n.bounty = i64_of(e, 10);
    if (const auto it = e.values.find(11); it != e.values.end()) {
        if (const auto* c = it->second.get_if<GameplayTagContainer>()) {
            for (const NameValue& t : c->tags) n.tags.push_back(t.hardcoded ? "#" + std::to_string(t.index) : t.text);
        }
    }
    if (const auto it = e.values.find(12); it != e.values.end()) {
        if (const float* f = it->second.get_if<float>()) n.deploy_radius = *f;
    }
    return n;
}

/// One array brought to a list of (key, item): the items it holds by key are changed when the values differ, new keys are added in order, gone keys removed.
template <class Key>
bool sync_array(FastArrayState& a, const std::vector<std::pair<Key, ElementState>>& want, uint16_t key_handle, size_t& changed) {
    std::map<Key, int32_t> have;   // key -> ReplicationID
    for (const FastArrayItem& it : a.items()) {
        const auto k = it.values.values.find(key_handle);
        if (k == it.values.values.end()) continue;
        const int64_t* i = k->second.template get_if<int64_t>();
        const uint64_t* u = k->second.template get_if<uint64_t>();
        have[static_cast<Key>(i ? static_cast<uint64_t>(*i) : u ? *u : 0)] = it.id;
    }
    std::vector<Key> wanted;
    for (const auto& [key, values] : want) {
        wanted.push_back(key);
        const auto h = have.find(key);
        if (h == have.end()) {
            if (!a.add(values)) return false;
            ++changed;
        } else if (const FastArrayItem* cur = a.find(h->second); cur && !(cur->values == values)) {
            if (!a.change(h->second, values)) return false;
            ++changed;
        }
    }
    for (const auto& [key, id] : have) {
        if (std::find(wanted.begin(), wanted.end(), key) != wanted.end()) continue;
        if (!a.remove(id)) return false;
        ++changed;
    }
    return true;
}

}  // namespace
// C_Y_B - CoA

// ---- the GameState's maps ---------------------------------------------------------------------------------------------------------------------------

bool node_maps_layout_ok(const schema::ClassSchema& gs) {
    const auto estates = fast_array_layout(gs, kEstates);
    const auto nodes = fast_array_layout(gs, kNodeData);
    const auto percent = fast_array_layout(gs, kPercent);
    const auto citizens = fast_array_layout(gs, kCitizens);
    return estates && estates->element.size() == 5 && nodes && nodes->element.size() == 12 && percent && percent->element.size() == 2 && citizens &&
           citizens->element.size() == 2;
}

bool sync_node_maps(const schema::ClassSchema& gs, FastArrays& arrays, const NodeStateMaps& maps, size_t* changed_out) {
    if (!node_maps_layout_ok(gs)) return false;
    FastArrayState* estates = arrays.get(gs, kEstates);
    FastArrayState* nodes = arrays.get(gs, kNodeData);
    FastArrayState* percent = arrays.get(gs, kPercent);
    FastArrayState* citizens = arrays.get(gs, kCitizens);
    if (!estates || !nodes || !percent || !citizens) return false;

    // Everything is checked first: a value the layout refuses must not leave one array changed and the others not.
    std::vector<std::pair<int64_t, ElementState>> w_estates, w_nodes;
    std::vector<std::pair<uint64_t, ElementState>> w_percent, w_citizens;
    for (const auto& [key, owner] : maps.estates) w_estates.emplace_back(key, estate_item(key, owner));
    for (const NodeDataEntry& n : maps.nodes) w_nodes.emplace_back(n.guid, node_item(n));
    for (const auto& [rank, value] : maps.percent) w_percent.emplace_back(rank, small_item(rank, value));
    for (const auto& [rank, value] : maps.citizens) w_citizens.emplace_back(rank, small_item(rank, value));
    for (const auto& [k, e] : w_estates) if (!item_matches(estates->layout(), e)) return false;
    for (const auto& [k, e] : w_nodes) if (!item_matches(nodes->layout(), e)) return false;
    for (const auto& [k, e] : w_percent) if (!item_matches(percent->layout(), e) || k > 0xFF) return false;
    for (const auto& [k, e] : w_citizens) if (!item_matches(citizens->layout(), e)) return false;

    size_t changed = 0;
    // (the keys are int64 for the two maps keyed by a guid and unsigned for the ranks; sync_array reads either)
    if (!sync_array<int64_t>(*estates, w_estates, 1, changed) || !sync_array<int64_t>(*nodes, w_nodes, 1, changed) ||
        !sync_array<uint64_t>(*percent, w_percent, 2, changed) || !sync_array<uint64_t>(*citizens, w_citizens, 2, changed)) {
        return false;
    }
    if (changed_out) *changed_out = changed;
    return true;
}

NodeStateMaps read_node_maps(const schema::ClassSchema& gs, const FastArrays& arrays) {
    NodeStateMaps m;
    const auto find = [&](const char* name) -> const FastArrayState* {
        const schema::NetField* f = gs.field(name);
        return f ? arrays.find(f->index) : nullptr;
    };
    if (const FastArrayState* a = find(kEstates)) {
        for (const FastArrayItem& it : a->items()) m.estates.emplace_back(i64_of(it.values, 1), words_of(it.values, 2));
    }
    if (const FastArrayState* a = find(kNodeData)) {
        for (const FastArrayItem& it : a->items()) m.nodes.push_back(node_entry(it.values));
    }
    if (const FastArrayState* a = find(kPercent)) {
        for (const FastArrayItem& it : a->items()) m.percent.emplace_back(static_cast<uint32_t>(u64_of(it.values, 2)), static_cast<uint8_t>(u64_of(it.values, 1)));
    }
    if (const FastArrayState* a = find(kCitizens)) {
        for (const FastArrayItem& it : a->items()) m.citizens.emplace_back(static_cast<uint32_t>(u64_of(it.values, 2)), static_cast<uint8_t>(u64_of(it.values, 1)));
    }
    return m;
}

NodeStateMaps node_maps_of_fields(const std::vector<FastArrayField>& fields) {
    // (the field numbers of AAoCGameStateBase: 25 estates, 26 nodes, 28 percent, 29 citizens)
    NodeStateMaps m;
    for (const FastArrayField& f : fields) {
        for (const FastArrayDelta::Changed& c : f.delta.changed) {
            switch (f.field) {
                case 25: m.estates.emplace_back(i64_of(c.values, 1), words_of(c.values, 2)); break;
                case 26: m.nodes.push_back(node_entry(c.values)); break;
                case 28: m.percent.emplace_back(static_cast<uint32_t>(u64_of(c.values, 2)), static_cast<uint8_t>(u64_of(c.values, 1))); break;
                case 29: m.citizens.emplace_back(static_cast<uint32_t>(u64_of(c.values, 2)), static_cast<uint8_t>(u64_of(c.values, 1))); break;
                default: break;
            }
        }
    }
    return m;
}

// ---- the RPC parameter sets ------------------------------------------------------------------------------------------------------------------------

namespace {

/// The value zeroed memory holds for a command (the one `rpc.h` sends as a 0 bit when it stands alone): every member of a flattened struct needs a value.
Value zero_of(const RepCommand& c) {
    switch (c.type) {
        case PropType::Bool: return Value::boolean(false);
        case PropType::Int8: case PropType::Int16: case PropType::Int32: case PropType::Int64: return Value::i64(0);
        case PropType::UInt8: case PropType::UInt16: case PropType::UInt32: case PropType::UInt64: case PropType::Enum: return Value::u64(0);
        case PropType::Float: return Value::f32(0.0f);
        case PropType::Double: return Value::f64(0.0);
        case PropType::Str: return Value::str("");
        case PropType::Name: return Value(NameValue::hardcoded_name(0));
        case PropType::DynamicArray: return Value(ArrayValue{});
        default: return Value{};   // a kind these structs do not hold
    }
}

/// Every command of `rpc` at its zero.
RpcParams zeroed(const schema::RpcSchema& rpc) {
    RpcParams p;
    for (const RepCommand& c : rpc.commands) p.values[c.handle] = zero_of(c);
    return p;
}
// [Cyb ~ C O A]

bool put(const schema::RpcSchema& rpc, RpcParams& p, std::string_view path, Value v) {
    const RepCommand* c = rpc_command(rpc, path);
    if (!c) return false;
    p.values[c->handle] = std::move(v);
    return true;
}

}  // namespace

std::array<uint32_t, 4> org_guid_of_node(int64_t node_guid) {
    const uint64_t g = static_cast<uint64_t>(node_guid);
    return {0u, 0u, static_cast<uint32_t>(g >> 32), static_cast<uint32_t>(g & 0xFFFFFFFFu)};
}

SiegeSummary idle_siege_summary(int64_t node_guid, const std::string& display_name) {
    SiegeSummary s;
    s.target_org_name = display_name;
    s.target_org_guid = org_guid_of_node(node_guid);
    return s;
}

std::optional<RpcParams> node_summary_params(const schema::RpcSchema& rpc, const NodeSummary& s) {
    if (!is_rpc(rpc, "ClientReceiveNodeSummary")) return std::nullopt;
    RpcParams p = zeroed(rpc);
    const RepCommand* plots = rpc_command(rpc, "Summary.PlotOverview");
    if (!plots) return std::nullopt;
    const auto elems = rpcf::elements_of(rpc, *plots);
    const uint16_t h_plot = rpcf::local(elems, "PlotOverview.Plot"), h_building = rpcf::local(elems, "PlotOverview.Building"),
                   h_state = rpcf::local(elems, "PlotOverview.State"), h_end = rpcf::local(elems, "PlotOverview.BuildingTimeEnd");
    if (!h_plot || !h_building || !h_state || !h_end) return std::nullopt;
    ArrayValue overview;
    for (const PlotEntry& e : s.plots) {
        ElementState es;
        for (size_t i = 0; i < elems.size(); ++i) es.values[static_cast<uint16_t>(i + 1)] = zero_of(elems[i]);
        es.values[h_plot] = Value::i64(e.plot);
        es.values[h_building] = Value::i64(e.building);
        es.values[h_state] = Value::u64(e.state);
        es.values[h_end] = Value::i64(e.building_time_end);
        overview.elements.push_back(std::move(es));
    }
    if (!put(rpc, p, "Summary.Node", Value::i64(s.node)) || !put(rpc, p, "Summary.Layout", Value::i64(s.layout)) ||
        !put(rpc, p, "Summary.NodeLevel", Value::i64(s.level)) || !put(rpc, p, "Summary.Culture", Value::u64(s.culture)) ||
        !put(rpc, p, "Summary.PlotOverview", Value(std::move(overview))) ||
        !put(rpc, p, "Summary.TimeUntilNextMaintenance", Value::i64(s.time_until_next_maintenance)) ||
        !put(rpc, p, "Summary.MayorMessage", Value::str(s.mayor_message)) ||
        !put(rpc, p, "Summary.NextMayorMessageEditTime", Value::i64(s.next_mayor_message_edit_time)) ||
        !put(rpc, p, "Summary.CurrentNodeEssence", Value::u64(s.current_node_essence)) || !put(rpc, p, "Summary.MaxNodeEssence", Value::u64(s.max_node_essence)) ||
        !put(rpc, p, "Summary.ActiveRelicSlots", Value::u64(s.active_relic_slots))) {
        return std::nullopt;
    }
    if (!rpc_params_match(rpc, p)) return std::nullopt;
    return p;
}

std::optional<RpcParams> siege_summary_params(const schema::RpcSchema& rpc, const SiegeSummary& s) {
    if (!is_rpc(rpc, "ClientReceiveSiegeSummary")) return std::nullopt;
    RpcParams p = zeroed(rpc);
    constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();
    bool ok = put(rpc, p, "SiegeData.CurrentPhaseIndex", Value::u64(255)) && put(rpc, p, "SiegeData.DateStarted", Value::u64(kMax)) &&
              put(rpc, p, "SiegeData.DateEnded", Value::u64(kMax)) && put(rpc, p, "SiegeData.DatePhaseStarted", Value::u64(kMax)) &&
              put(rpc, p, "SiegeData.DateEventStarts", Value::u64(kMax)) && put(rpc, p, "SiegeData.TargetOrgName", Value::str(s.target_org_name)) &&
              put(rpc, p, "SiegeData.SiegeParticipantsData.AttackingTeamLeader.CharacterLevel", Value::i64(1)) &&
              put(rpc, p, "SiegeData.SiegeParticipantsData.DefendingTeamLeader.CharacterLevel", Value::i64(1));
    static const char* const kPart[4] = {"A", "B", "C", "D"};
    for (size_t i = 0; ok && i < 4; ++i) ok = put(rpc, p, std::string("SiegeData.TargetOrgGuid.") + kPart[i], word(s.target_org_guid[i]));
    if (!ok || !rpc_params_match(rpc, p)) return std::nullopt;
    return p;
}

std::optional<RpcParams> empty_siege_cooldowns_params(const schema::RpcSchema& rpc) {
    if (!is_rpc(rpc, "ClientReceiveAllCurrentNodeSiegeCooldowns")) return std::nullopt;
    RpcParams p = zeroed(rpc);
    if (!rpc_params_match(rpc, p)) return std::nullopt;
    return p;
}

std::optional<int64_t> parse_node_summary_request(const RpcCall& call) {
    if (!decoded_as(call, "ServerRequestNodeSummary")) return std::nullopt;
    return rpcf::int_or(rpcf::value_of(*call.rpc, call.params, "Node"), 0);
}

std::optional<int64_t> parse_siege_summary_request(const RpcCall& call) {
    if (!decoded_as(call, "ServerRequestCurrentSiegeSummary")) return std::nullopt;
    return rpcf::int_or(rpcf::value_of(*call.rpc, call.params, "NodeId"), 0);
}
// (CoA) (C:Y:B)

std::optional<NodeSummary> parse_node_summary(const RpcCall& call) {
    if (!decoded_as(call, "ClientReceiveNodeSummary")) return std::nullopt;
    const schema::RpcSchema& rpc = *call.rpc;
    NodeSummary s;
    const auto get_i = [&](const char* path) { return rpcf::int_or(rpcf::value_of(rpc, call.params, path), 0).value_or(0); };
    const auto get_u = [&](const char* path) { return rpcf::uint_or(rpcf::value_of(rpc, call.params, path), 0).value_or(0); };
    s.node = get_i("Summary.Node");
    s.layout = get_i("Summary.Layout");
    s.level = static_cast<int32_t>(get_i("Summary.NodeLevel"));
    s.culture = static_cast<uint8_t>(get_u("Summary.Culture"));
    s.time_until_next_maintenance = get_i("Summary.TimeUntilNextMaintenance");
    if (const std::string* m = rpcf::as<std::string>(rpcf::value_of(rpc, call.params, "Summary.MayorMessage"))) s.mayor_message = *m;
    s.next_mayor_message_edit_time = get_i("Summary.NextMayorMessageEditTime");
    s.current_node_essence = static_cast<uint8_t>(get_u("Summary.CurrentNodeEssence"));
    s.max_node_essence = static_cast<uint8_t>(get_u("Summary.MaxNodeEssence"));
    s.active_relic_slots = static_cast<uint8_t>(get_u("Summary.ActiveRelicSlots"));
    const RepCommand* plots = rpc_command(rpc, "Summary.PlotOverview");
    if (plots) {
        const auto elems = rpcf::elements_of(rpc, *plots);
        const uint16_t h_plot = rpcf::local(elems, "PlotOverview.Plot"), h_building = rpcf::local(elems, "PlotOverview.Building"),
                       h_state = rpcf::local(elems, "PlotOverview.State"), h_end = rpcf::local(elems, "PlotOverview.BuildingTimeEnd");
        if (const ArrayValue* a = rpcf::as<ArrayValue>(rpcf::value_of(rpc, call.params, "Summary.PlotOverview"))) {
            for (const ElementState& e : a->elements) {
                PlotEntry pe;
                pe.plot = i64_of(e, h_plot);
                pe.building = i64_of(e, h_building);
                pe.state = static_cast<uint8_t>(u64_of(e, h_state));
                pe.building_time_end = i64_of(e, h_end);
                s.plots.push_back(pe);
            }
        }
    }
    return s;
}

std::optional<SiegeSummary> parse_siege_summary(const RpcCall& call) {
    if (!decoded_as(call, "ClientReceiveSiegeSummary")) return std::nullopt;
    const schema::RpcSchema& rpc = *call.rpc;
    SiegeSummary s;
    if (const std::string* n = rpcf::as<std::string>(rpcf::value_of(rpc, call.params, "SiegeData.TargetOrgName"))) s.target_org_name = *n;
    static const char* const kPart[4] = {"A", "B", "C", "D"};
    for (size_t i = 0; i < 4; ++i) {
        const auto v = rpcf::int_or(rpcf::value_of(rpc, call.params, std::string("SiegeData.TargetOrgGuid.") + kPart[i]), 0);
        s.target_org_guid[i] = static_cast<uint32_t>(v.value_or(0));
    }
    return s;
}

// ---- the GM console -------------------------------------------------------------------------------------------------------------------------------

std::optional<GmSetNodeLevel> parse_gm_set_node_level(const RpcCall& call) {
    if (!decoded_as(call, "ServerSetNodeLevel")) return std::nullopt;
    GmSetNodeLevel g;
    if (const std::string* s = rpcf::as<std::string>(rpcf::value_of(*call.rpc, call.params, "InNode"))) g.node = *s;
    g.level = static_cast<int32_t>(rpcf::int_or(rpcf::value_of(*call.rpc, call.params, "InLevel"), 0).value_or(0));
    g.culture = static_cast<int32_t>(rpcf::int_or(rpcf::value_of(*call.rpc, call.params, "InCulture"), 0).value_or(0));
    g.disable_vassalship = rpcf::bool_or(rpcf::value_of(*call.rpc, call.params, "bDisableVassalshipSystem"), false).value_or(false);
    return g;
}

std::optional<GmAddNodeExperience> parse_gm_add_node_experience(const RpcCall& call) {
    if (!decoded_as(call, "ServerAddNodeExperience")) return std::nullopt;
    GmAddNodeExperience g;
    if (const std::string* s = rpcf::as<std::string>(rpcf::value_of(*call.rpc, call.params, "InNode"))) g.node = *s;
    g.experience = static_cast<int32_t>(rpcf::int_or(rpcf::value_of(*call.rpc, call.params, "Experience"), 0).value_or(0));
    g.culture = static_cast<int32_t>(rpcf::int_or(rpcf::value_of(*call.rpc, call.params, "InCulture"), 0).value_or(0));
    g.raw_xp = rpcf::bool_or(rpcf::value_of(*call.rpc, call.params, "bUseRawXP"), false).value_or(false);
    return g;
}

std::optional<std::string> parse_gm_get_node_data(const RpcCall& call) {
    if (!decoded_as(call, "ServerGMGetNodeData")) return std::nullopt;
    const std::string* s = rpcf::as<std::string>(rpcf::value_of(*call.rpc, call.params, "NodeName"));
    return s ? *s : std::string();
}

}}}  // namespace aoc::protocol::repl
