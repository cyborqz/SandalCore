// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/storage_rpc.cpp
//
//  See storage_rpc.h.
// ============================================================================
#include "protocol/repl/storage_rpc.h"

#include <cstring>

namespace aoc { namespace protocol { namespace repl {

namespace {

using schema::PropType;
using schema::RepCommand;
using schema::RpcSchema;

constexpr std::string_view kPayloadRpc = "ClientReceiveStoragePayload";

constexpr std::string_view kLootPayloadRpc = "ClientReceiveLootStoragePayload";

/// `target` names the first parameter (the storage component, or the loot actor).
bool is_payload_rpc(const RpcSchema& rpc, std::string_view name = kPayloadRpc, const char* target = "TargetStorage") {
    return rpc.name && name == rpc.name && rpc_command(rpc, target) && rpc_command(rpc, "NewStorageData.Storage") &&
           rpc_command(rpc, "NewStorageData.OverrideMatrix");
}

/// The element commands of an array command of `rpc`, empty when its range is bad.
std::span<const RepCommand> elements_of(const RpcSchema& rpc, const RepCommand& array) {
    if (array.type != PropType::DynamicArray || array.elem_end < array.elem_begin ||
        array.elem_end > rpc.element_commands.size())
        return {};
    return rpc.element_commands.subspan(array.elem_begin, array.elem_end - array.elem_begin);
}
// C Y B - C.O.A

/// The local handle (1-based position) of the element command `path` in `elems`, 0 when it has none.
uint16_t local(std::span<const RepCommand> elems, std::string_view path) {
    for (size_t i = 0; i < elems.size(); ++i) {
        if (path == elems[i].path) return static_cast<uint16_t>(i + 1);
    }
    return 0;
}

const Value* value_of(const RpcSchema& rpc, const RpcParams& params, std::string_view path) {
    const RepCommand* c = rpc_command(rpc, path);
    if (!c) return nullptr;
    const auto it = params.values.find(c->handle);
    return it == params.values.end() ? nullptr : &it->second;
}

const Value* element_value(const ElementState& e, uint16_t handle) {
    if (!handle) return nullptr;
    const auto it = e.values.find(handle);
    return it == e.values.end() ? nullptr : &it->second;
}

Value int32_value(int32_t v) { return Value::i64(v); }
Value guid_part(uint32_t v) { return Value::i64(static_cast<int32_t>(v)); }

/// The four int32 members of an FGuid (`prefix`.A .. D), by local handle in `elems`.
bool put_guid(ElementState& e, std::span<const RepCommand> elems, std::string_view prefix, const GuidValue& g) {
    const uint32_t parts[4] = {g.a, g.b, g.c, g.d};
    const char* names[4] = {".A", ".B", ".C", ".D"};
    for (int i = 0; i < 4; ++i) {
        const uint16_t h = local(elems, std::string(prefix) + names[i]);
        if (!h) return false;
        e.values[h] = guid_part(parts[i]);
    }
    return true;
}

std::optional<GuidValue> take_guid(const ElementState& e, std::span<const RepCommand> elems, std::string_view prefix) {
    uint32_t parts[4] = {};
    const char* names[4] = {".A", ".B", ".C", ".D"};
    for (int i = 0; i < 4; ++i) {
        const Value* v = element_value(e, local(elems, std::string(prefix) + names[i]));
        const int64_t* n = v ? v->get_if<int64_t>() : nullptr;
        if (!n) return std::nullopt;
        parts[i] = static_cast<uint32_t>(static_cast<int32_t>(*n));
    }
    return GuidValue{parts[0], parts[1], parts[2], parts[3]};
}

template <class T>
const T* get_as(const Value* v) { return v ? v->get_if<T>() : nullptr; }

}  // namespace

bool StoragePayload::operator==(const StoragePayload& o) const {
    return target == o.target && container_name == o.container_name && same_bits(maximum_weight, o.maximum_weight) &&
           maximum_items == o.maximum_items && loot_fountain_on_death == o.loot_fountain_on_death &&
           same_bits(item_drop_on_destruction_percent, o.item_drop_on_destruction_percent) &&
           same_bits(maximum_items_to_drop_percent, o.maximum_items_to_drop_percent) &&
           minimum_impulse == o.minimum_impulse && maximum_impulse == o.maximum_impulse &&
           randomize_angular == o.randomize_angular && storage == o.storage &&
           client_retrieve_only == o.client_retrieve_only && dimension == o.dimension &&
           override_matrix == o.override_matrix;
}

size_t filled_slots(const StoragePayload& p) {
    size_t n = 0;
    for (const StorageSlot& s : p.storage) n += s.item.has_item ? 1 : 0;
    return n;
}

namespace {

std::optional<RpcParams> payload_params_impl(const RpcSchema& rpc, const StoragePayload& p, std::string_view name, const char* target) {
    if (!is_payload_rpc(rpc, name, target)) return std::nullopt;
    RpcParams params;
    bool ok = set_rpc_param(rpc, params, target, Value(p.target));
    ok = ok && set_rpc_param(rpc, params, "NewStorageData.ContainerName", Value::str(p.container_name));
    ok = ok && set_rpc_param(rpc, params, "NewStorageData.MaximumWeight", Value::f32(p.maximum_weight));
    ok = ok && set_rpc_param(rpc, params, "NewStorageData.MaximumItems", int32_value(p.maximum_items));
    ok = ok && set_rpc_param(rpc, params, "NewStorageData.bLootFountainOnDeath", Value::boolean(p.loot_fountain_on_death));
    ok = ok && set_rpc_param(rpc, params, "NewStorageData.ItemDropOnDestructionPercent",
                             Value::f32(p.item_drop_on_destruction_percent));
    ok = ok && set_rpc_param(rpc, params, "NewStorageData.MaximumItemsToDropPercent",
                             Value::f32(p.maximum_items_to_drop_percent));
    ok = ok && set_rpc_param(rpc, params, "NewStorageData.MinimumImpulse", Value(p.minimum_impulse));
    ok = ok && set_rpc_param(rpc, params, "NewStorageData.MaximumImpulse", Value(p.maximum_impulse));
    ok = ok && set_rpc_param(rpc, params, "NewStorageData.bRandomizeAngular", Value::boolean(p.randomize_angular));
    ok = ok && set_rpc_param(rpc, params, "NewStorageData.bClientRetrieveOnly", Value::boolean(p.client_retrieve_only));
    ok = ok && set_rpc_param(rpc, params, "NewStorageData.Dimension", Value(p.dimension));
    if (!ok) return std::nullopt;

    // Storage: per slot every member of the element, the loot permission array nested.
    const RepCommand* storage = rpc_command(rpc, "NewStorageData.Storage");
    const auto selems = elements_of(rpc, *storage);
    const uint16_t h_item = local(selems, "Storage.ItemData"), h_slot = local(selems, "Storage.Slot"),
                   h_check = local(selems, "Storage.bCheckItemLootPermission"),
                   h_loot = local(selems, "Storage.ItemLootPermission");
    if (!h_item || !h_slot || !h_check || !h_loot || p.storage.size() > 0xFFFF) return std::nullopt;
    const RepCommand& loot_cmd = selems[h_loot - 1];
    const auto lelems = elements_of(rpc, loot_cmd);
    ArrayValue slots;
    for (const StorageSlot& s : p.storage) {
        ElementState e;
        e.values[h_item] = Value(s.item);
        e.values[h_slot] = int32_value(s.slot);
        if (!put_guid(e, selems, "Storage.TransactionIDLock", s.transaction_lock)) return std::nullopt;
        e.values[h_check] = Value::boolean(s.check_loot_permission);
        ArrayValue loot;
        if (s.loot_permission.size() > 0xFFFF) return std::nullopt;
        for (const GuidValue& g : s.loot_permission) {
            ElementState le;
            if (!put_guid(le, lelems, "ItemLootPermission", g)) return std::nullopt;
            loot.elements.push_back(std::move(le));
        }
        e.values[h_loot] = Value(std::move(loot));
        slots.elements.push_back(std::move(e));
    }
    if (!set_rpc_param(rpc, params, "NewStorageData.Storage", Value(std::move(slots)))) return std::nullopt;

    // OverrideMatrix: an array of TetrisColumn { Column: an array of int32 }.
    const RepCommand* matrix = rpc_command(rpc, "NewStorageData.OverrideMatrix");
    const auto melems = elements_of(rpc, *matrix);
    const uint16_t h_column = local(melems, "OverrideMatrix.Column");
    if (!h_column || p.override_matrix.size() > 0xFFFF) return std::nullopt;
    const auto celems = elements_of(rpc, melems[h_column - 1]);
    if (celems.size() != 1) return std::nullopt;
    ArrayValue columns;
    for (const auto& column : p.override_matrix) {
        if (column.size() > 0xFFFF) return std::nullopt;
        ArrayValue cells;
        for (int32_t c : column) {
            ElementState ce;
            ce.values[1] = int32_value(c);
            cells.elements.push_back(std::move(ce));
        }
        ElementState col;
        col.values[h_column] = Value(std::move(cells));
        columns.elements.push_back(std::move(col));
    }
    if (!set_rpc_param(rpc, params, "NewStorageData.OverrideMatrix", Value(std::move(columns)))) return std::nullopt;
    return params;
}
// [C | O | A x C:Y:B]

std::optional<StoragePayload> parse_payload_impl(const RpcSchema& rpc, const RpcParams& params, std::string_view name, const char* target);

}  // namespace

std::optional<RpcParams> storage_payload_params(const RpcSchema& rpc, const StoragePayload& p) {
    return payload_params_impl(rpc, p, kPayloadRpc, "TargetStorage");
}

std::optional<StoragePayload> parse_storage_payload(const RpcSchema& rpc, const RpcParams& params) {
    return parse_payload_impl(rpc, params, kPayloadRpc, "TargetStorage");
}

namespace {

std::optional<StoragePayload> parse_payload_impl(const RpcSchema& rpc, const RpcParams& params, std::string_view rpc_name, const char* target) {
    if (!is_payload_rpc(rpc, rpc_name, target)) return std::nullopt;
    StoragePayload p;
    if (const Value* t = value_of(rpc, params, target)) {
        const auto* g = get_as<emit::FIntrepidNetworkGUID>(t);
        if (!g) return std::nullopt;
        p.target = *g;
    }
    // NewStorageData is a struct parameter: sent whole or left out (then there is no payload to speak of).
    const auto* name = get_as<std::string>(value_of(rpc, params, "NewStorageData.ContainerName"));
    const auto* weight = get_as<float>(value_of(rpc, params, "NewStorageData.MaximumWeight"));
    const auto* items = get_as<int64_t>(value_of(rpc, params, "NewStorageData.MaximumItems"));
    const auto* fountain = get_as<bool>(value_of(rpc, params, "NewStorageData.bLootFountainOnDeath"));
    const auto* drop = get_as<float>(value_of(rpc, params, "NewStorageData.ItemDropOnDestructionPercent"));
    const auto* drop_max = get_as<float>(value_of(rpc, params, "NewStorageData.MaximumItemsToDropPercent"));
    const auto* imp_min = get_as<Vec3d>(value_of(rpc, params, "NewStorageData.MinimumImpulse"));
    const auto* imp_max = get_as<Vec3d>(value_of(rpc, params, "NewStorageData.MaximumImpulse"));
    const auto* angular = get_as<bool>(value_of(rpc, params, "NewStorageData.bRandomizeAngular"));
    const auto* retrieve = get_as<bool>(value_of(rpc, params, "NewStorageData.bClientRetrieveOnly"));
    const auto* dim = get_as<Vec2d>(value_of(rpc, params, "NewStorageData.Dimension"));
    const auto* storage = get_as<ArrayValue>(value_of(rpc, params, "NewStorageData.Storage"));
    const auto* matrix = get_as<ArrayValue>(value_of(rpc, params, "NewStorageData.OverrideMatrix"));
    if (!name || !weight || !items || !fountain || !drop || !drop_max || !imp_min || !imp_max || !angular || !retrieve ||
        !dim || !storage || !matrix || *items < INT32_MIN || *items > INT32_MAX)
        return std::nullopt;
    p.container_name = *name;
    p.maximum_weight = *weight;
    p.maximum_items = static_cast<int32_t>(*items);
    p.loot_fountain_on_death = *fountain;
    p.item_drop_on_destruction_percent = *drop;
    p.maximum_items_to_drop_percent = *drop_max;
    p.minimum_impulse = *imp_min;
    p.maximum_impulse = *imp_max;
    p.randomize_angular = *angular;
    p.client_retrieve_only = *retrieve;
    p.dimension = *dim;

    const RepCommand* storage_cmd = rpc_command(rpc, "NewStorageData.Storage");
    const auto selems = elements_of(rpc, *storage_cmd);
    const uint16_t h_item = local(selems, "Storage.ItemData"), h_slot = local(selems, "Storage.Slot"),
                   h_check = local(selems, "Storage.bCheckItemLootPermission"),
                   h_loot = local(selems, "Storage.ItemLootPermission");
    if (!h_item || !h_slot || !h_check || !h_loot) return std::nullopt;
    const auto lelems = elements_of(rpc, selems[h_loot - 1]);
    for (const ElementState& e : storage->elements) {
        StorageSlot s;
        const auto* item = get_as<ItemInstance>(element_value(e, h_item));
        const auto* slot = get_as<int64_t>(element_value(e, h_slot));
        const auto* check = get_as<bool>(element_value(e, h_check));
        const auto* loot = get_as<ArrayValue>(element_value(e, h_loot));
        const auto lock = take_guid(e, selems, "Storage.TransactionIDLock");
        if (!item || !slot || !check || !loot || !lock || *slot < INT32_MIN || *slot > INT32_MAX) return std::nullopt;
        s.item = *item;
        s.slot = static_cast<int32_t>(*slot);
        s.transaction_lock = *lock;
        s.check_loot_permission = *check;
        for (const ElementState& le : loot->elements) {
            const auto g = take_guid(le, lelems, "ItemLootPermission");
            if (!g) return std::nullopt;
            s.loot_permission.push_back(*g);
        }
        p.storage.push_back(std::move(s));
    }
// -- C:Y:B & C O A --

    const RepCommand* matrix_cmd = rpc_command(rpc, "NewStorageData.OverrideMatrix");
    const auto melems = elements_of(rpc, *matrix_cmd);
    const uint16_t h_column = local(melems, "OverrideMatrix.Column");
    if (!h_column) return std::nullopt;
    for (const ElementState& col : matrix->elements) {
        const auto* cells = get_as<ArrayValue>(element_value(col, h_column));
        if (!cells) return std::nullopt;
        std::vector<int32_t> column;
        for (const ElementState& ce : cells->elements) {
            const auto* c = get_as<int64_t>(element_value(ce, 1));
            if (!c || *c < INT32_MIN || *c > INT32_MAX) return std::nullopt;
            column.push_back(static_cast<int32_t>(*c));
        }
        p.override_matrix.push_back(std::move(column));
    }
    return p;
}

}  // namespace

std::optional<StoragePayload> parse_storage_payload(const RpcCall& call) {
    if (!call.rpc || !call.decoded) return std::nullopt;
    return parse_storage_payload(*call.rpc, call.params);
}

namespace {

/// The command of the one FItemStorageEvent parameter of `rpc`, nullptr for an RPC of another shape.
const RepCommand* event_command(const RpcSchema& rpc) {
    if (rpc.params.size() != 1 || rpc.commands.size() != 1) return nullptr;
    const RepCommand& c = rpc.commands[0];
    if (c.type != PropType::NetSerializeStruct || !c.struct_name || std::strcmp(c.struct_name, "FItemStorageEvent") != 0)
        return nullptr;
    return &c;
}

}  // namespace

std::optional<RpcParams> storage_event_params(const RpcSchema& rpc, const ItemStorageEvent& event) {
    const RepCommand* c = event_command(rpc);
    if (!c) return std::nullopt;
    RpcParams params;
    if (!set_rpc_param(rpc, params, c->path, Value(event))) return std::nullopt;
    return params;
}

std::optional<ItemStorageEvent> parse_storage_event(const RpcSchema& rpc, const RpcParams& params) {
    const RepCommand* c = event_command(rpc);
    if (!c) return std::nullopt;
    const auto it = params.values.find(c->handle);
    if (it == params.values.end()) return std::nullopt;
    const auto* e = it->second.get_if<ItemStorageEvent>();
    if (!e) return std::nullopt;
    return *e;
}

std::optional<ItemStorageEvent> parse_storage_event(const RpcCall& call) {
    if (!call.rpc || !call.decoded) return std::nullopt;
    return parse_storage_event(*call.rpc, call.params);
}

// ---- plan E1, stage 3, part 2: the loot window ----------------------------------------------------------------------------------

namespace {

constexpr const char* kAllocationPaths[] = {
    "LootAllocationMethods.DefaultLootAllocationMethod",   "LootAllocationMethods.CommonLootAllocationMethod",    "LootAllocationMethods.UncommonLootAllocationMethod",
    "LootAllocationMethods.RareLootAllocationMethod",      "LootAllocationMethods.HeroicLootAllocationMethod",    "LootAllocationMethods.EpicLootAllocationMethod",
    "LootAllocationMethods.LegendaryLootAllocationMethod", "LootAllocationMethods.ArtifactLootAllocationMethod"};

}  // namespace

std::optional<RpcParams> loot_payload_params(const RpcSchema& rpc, const LootPayload& p) {
    StoragePayload data = p.data;
    data.target = p.actor;
    auto params = payload_params_impl(rpc, data, kLootPayloadRpc, "LootActor");
    if (!params) return std::nullopt;
    for (size_t i = 0; i < 8; ++i) {
        const uint8_t v = i == 0 ? p.allocation.default_method : p.allocation.by_rarity[i - 1];
        if (!set_rpc_param(rpc, *params, kAllocationPaths[i], Value::u64(v))) return std::nullopt;
    }
    return params;
}

std::optional<LootPayload> parse_loot_payload(const RpcCall& call) {
    if (!call.rpc || !call.decoded) return std::nullopt;
    auto data = parse_payload_impl(*call.rpc, call.params, kLootPayloadRpc, "LootActor");
    if (!data) return std::nullopt;
    LootPayload p;
    p.actor = data->target;
    p.data = std::move(*data);
    for (size_t i = 0; i < 8; ++i) {
        const auto* v = get_as<uint64_t>(value_of(*call.rpc, call.params, kAllocationPaths[i]));
        if (!v || *v > 255) return std::nullopt;
        (i == 0 ? p.allocation.default_method : p.allocation.by_rarity[i - 1]) = static_cast<uint8_t>(*v);
    }
    return p;
}

std::optional<std::vector<ItemStorageEvent>> parse_loot_events(const RpcCall& call) {
    if (!call.rpc || !call.decoded || !call.rpc->name || std::string_view("ServerRequestLootStorageEvents") != call.rpc->name) return std::nullopt;
    const RepCommand* c = rpc_command(*call.rpc, "ItemEvents");
    if (!c || c->type != PropType::DynamicArray) return std::nullopt;
    const auto it = call.params.values.find(c->handle);
    std::vector<ItemStorageEvent> out;
    if (it == call.params.values.end()) return out;   // no events
    const auto* array = it->second.get_if<ArrayValue>();
    if (!array || array->elements.size() > 256) return std::nullopt;
    const auto elems = elements_of(*call.rpc, *c);
    if (elems.size() != 1) return std::nullopt;
    for (const ElementState& e : array->elements) {
        const auto* ev = get_as<ItemStorageEvent>(element_value(e, 1));
        if (!ev) return std::nullopt;
        out.push_back(*ev);
    }
    return out;
}

std::optional<RpcParams> remove_listener_params(const RpcSchema& rpc, const emit::FIntrepidNetworkGUID& actor) {
    if (!rpc.name || std::string_view("ClientRemoveListenerFromStorage") != rpc.name || !rpc_command(rpc, "LootActor")) return std::nullopt;
    RpcParams params;
    if (!set_rpc_param(rpc, params, "LootActor", Value(actor))) return std::nullopt;
    return params;
}
// C-Y-B.C.O.A

// ---- ClientUpdateItemDurability ----------------------------------------------------------------------------------------------

namespace {

constexpr std::string_view kDurabilityRpc = "ClientUpdateItemDurability";
constexpr std::string_view kDurabilityContainer = "ItemDurabilityUpdate.TargetContainer";
constexpr std::string_view kDurabilityArray = "ItemDurabilityUpdate.ItemDurabilityData";
constexpr size_t kMaxDurabilityItems = 256;

}  // namespace

std::optional<RpcParams> item_durability_params(const RpcSchema& rpc, const ItemDurabilityUpdate& u) {
    if (!rpc.name || kDurabilityRpc != rpc.name || u.items.empty() || u.items.size() > kMaxDurabilityItems) return std::nullopt;
    const RepCommand* array = rpc_command(rpc, kDurabilityArray);
    if (!rpc_command(rpc, kDurabilityContainer) || !array) return std::nullopt;
    const auto elems = elements_of(rpc, *array);
    const uint16_t ha = local(elems, "ItemDurabilityData.ItemGuid.A"), hb = local(elems, "ItemDurabilityData.ItemGuid.B"), hc = local(elems, "ItemDurabilityData.ItemGuid.C"),
                   hd = local(elems, "ItemDurabilityData.ItemGuid.D"), hs = local(elems, "ItemDurabilityData.Slot"), hu = local(elems, "ItemDurabilityData.Durability");
    if (!ha || !hb || !hc || !hd || !hs || !hu) return std::nullopt;
    ArrayValue a;
    for (const ItemDurability& d : u.items) {
        ElementState e;
        e.values[ha] = guid_part(d.item.a);
        e.values[hb] = guid_part(d.item.b);
        e.values[hc] = guid_part(d.item.c);
        e.values[hd] = guid_part(d.item.d);
        e.values[hs] = int32_value(d.slot);
        e.values[hu] = Value::f32(d.durability);
        a.elements.push_back(std::move(e));
    }
    RpcParams params;
    if (!set_rpc_param(rpc, params, kDurabilityContainer, Value(u.container)) || !set_rpc_param(rpc, params, kDurabilityArray, Value(std::move(a)))) return std::nullopt;
    return params;
}

std::optional<ItemDurabilityUpdate> parse_item_durability(const RpcSchema& rpc, const RpcParams& params) {
    if (!rpc.name || kDurabilityRpc != rpc.name) return std::nullopt;
    const RepCommand* array = rpc_command(rpc, kDurabilityArray);
    if (!array || !rpc_command(rpc, kDurabilityContainer)) return std::nullopt;
    const auto elems = elements_of(rpc, *array);
    const uint16_t ha = local(elems, "ItemDurabilityData.ItemGuid.A"), hb = local(elems, "ItemDurabilityData.ItemGuid.B"), hc = local(elems, "ItemDurabilityData.ItemGuid.C"),
                   hd = local(elems, "ItemDurabilityData.ItemGuid.D"), hs = local(elems, "ItemDurabilityData.Slot"), hu = local(elems, "ItemDurabilityData.Durability");
    if (!ha || !hb || !hc || !hd || !hs || !hu) return std::nullopt;
    ItemDurabilityUpdate out;
    if (const auto* c = get_as<emit::FIntrepidNetworkGUID>(value_of(rpc, params, kDurabilityContainer))) out.container = *c;
    const Value* v = value_of(rpc, params, kDurabilityArray);
    if (!v) return out;   // an empty array is the default: not on the wire
    const auto* a = v->get_if<ArrayValue>();
    if (!a || a->elements.size() > kMaxDurabilityItems) return std::nullopt;
    const auto word = [&](const ElementState& e, uint16_t h) -> std::optional<uint32_t> {
        const Value* x = element_value(e, h);
        if (!x) return 0u;   // a member at its default is not on the wire
        const auto* i = x->get_if<int64_t>();
        if (!i) return std::nullopt;
        return static_cast<uint32_t>(*i);
    };
    for (const ElementState& e : a->elements) {
        ItemDurability d;
        const auto wa = word(e, ha), wb = word(e, hb), wc = word(e, hc), wd = word(e, hd);
        if (!wa || !wb || !wc || !wd) return std::nullopt;
        d.item = GuidValue{*wa, *wb, *wc, *wd};
        if (const Value* s = element_value(e, hs)) {
            const auto* i = s->get_if<int64_t>();
            if (!i) return std::nullopt;
            d.slot = static_cast<int32_t>(*i);
        }
        if (const Value* u = element_value(e, hu)) {
            const auto* f = u->get_if<float>();
            if (!f) return std::nullopt;
            d.durability = *f;
        }
        out.items.push_back(d);
    }
    return out;
}

std::optional<ItemDurabilityUpdate> parse_item_durability(const RpcCall& call) {
    if (!call.rpc || !call.decoded) return std::nullopt;
    return parse_item_durability(*call.rpc, call.params);
}

}}}  // namespace aoc::protocol::repl
