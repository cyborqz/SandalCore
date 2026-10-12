// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/skill_rpc.cpp
//
//  See skill_rpc.h.
// ============================================================================
#include "protocol/repl/skill_rpc.h"

#include "protocol/repl/rpc_fields.h"

#include <algorithm>
#include <climits>

namespace aoc { namespace protocol { namespace repl {

namespace {

using schema::RepCommand;
using schema::RpcSchema;

bool is_rpc(const RpcCall& call, std::string_view name) { return call.rpc && call.decoded && name == call.rpc->name; }

/// The element members of an array parameter `path` of `rpc` by local handle, looked up by `names` (in order); empty when the
/// RPC has no such array or lacks a member.
std::vector<uint16_t> element_handles(const RpcSchema& rpc, std::string_view path, std::initializer_list<const char*> names) {
    const RepCommand* cmd = rpc_command(rpc, path);
    std::vector<uint16_t> out;
    if (!cmd) return out;
    const auto elems = rpcf::elements_of(rpc, *cmd);
    for (const char* n : names) {
        const uint16_t h = rpcf::local(elems, n);
        if (!h) return {};
        out.push_back(h);
    }
    return out;
}

Value guid_part(uint32_t v) { return Value::i64(static_cast<int32_t>(v)); }

// ---- FDesignDataObjectId as three members of an array element ------------------------------------------------------------------

void put_id(ElementState& e, uint16_t guid, uint16_t type, uint16_t name, const DesignDataId& id) {
    e.values[guid] = Value::i64(id.guid);
    e.values[type] = Value::i64(id.type_id);
    e.values[name] = Value(id.name);
}

std::optional<DesignDataId> take_id(const ElementState& e, uint16_t guid, uint16_t type, uint16_t name) {
    const auto g = rpcf::int_or(rpcf::element_value(e, guid), 0);
    const auto t = rpcf::int_or(rpcf::element_value(e, type), 0);
    const NameValue* n = rpcf::as<NameValue>(rpcf::element_value(e, name));
    if (!g || !t) return std::nullopt;
    DesignDataId d;
    d.guid = *g;
    d.type_id = *t;
    if (n) d.name = *n;
    return d;
}
// -- C | O | A . C.Y.B --

// ---- FActionbarSlotUseData: the same members in an array element (prefix "ActionbarSlotData.UseData.") and in the parameter of
//      ServerSetActionbarSlotUseData (prefix "UseData.") -------------------------------------------------------------------------

constexpr const char* kUseMembers[] = {
    "UseType", "Instigator", "AbilityId", "SkillRankId", "PetCommandId", "Item.ItemRecordId", "Item.GUID.A", "Item.GUID.B",
    "Item.GUID.C", "Item.GUID.D", "Item.Rarity", "Item.Quality", "ComboButton",
};
constexpr size_t kUseCount = std::size(kUseMembers);

/// The handles of the use data's members (by `prefix` in `elems`).
std::optional<std::vector<uint16_t>> use_handles(std::span<const RepCommand> elems, const std::string& prefix) {
    std::vector<uint16_t> h;
    for (const char* m : kUseMembers) {
        const uint16_t x = rpcf::local(elems, prefix + m);
        if (!x) return std::nullopt;
        h.push_back(x);
    }
    return h;
}

void put_use(ElementState& e, const std::vector<uint16_t>& h, const ActionbarUseData& u) {
    e.values[h[0]] = Value::u64(u.use_type);
    e.values[h[1]] = Value::u64(u.instigator);
    e.values[h[2]] = Value::i64(u.ability_id);
    e.values[h[3]] = Value::i64(u.skill_rank_id);
    e.values[h[4]] = Value::i64(u.pet_command_id);
    e.values[h[5]] = Value::i64(u.item_record_id);
    e.values[h[6]] = guid_part(u.item_guid.a);
    e.values[h[7]] = guid_part(u.item_guid.b);
    e.values[h[8]] = guid_part(u.item_guid.c);
    e.values[h[9]] = guid_part(u.item_guid.d);
    e.values[h[10]] = Value::u64(u.item_rarity);
    e.values[h[11]] = Value::i64(u.item_quality);
    e.values[h[12]] = Value::u64(u.combo_button);
}

/// `get(handle)` gives a member's value or nullptr (a member at its default is absent).
template <class Get>
std::optional<ActionbarUseData> take_use(const std::vector<uint16_t>& h, Get get) {
    const auto type = rpcf::uint_or(get(h[0]), 0), instig = rpcf::uint_or(get(h[1]), 0), rarity = rpcf::uint_or(get(h[10]), 0),
               combo = rpcf::uint_or(get(h[12]), 0);
    const auto ability = rpcf::int_or(get(h[2]), 0), rank = rpcf::int_or(get(h[3]), 0), pet = rpcf::int_or(get(h[4]), 0),
               record = rpcf::int_or(get(h[5]), 0), a = rpcf::int_or(get(h[6]), 0), b = rpcf::int_or(get(h[7]), 0),
               c = rpcf::int_or(get(h[8]), 0), d = rpcf::int_or(get(h[9]), 0), quality = rpcf::int_or(get(h[11]), 0);
    if (!type || !instig || !rarity || !combo || !ability || !rank || !pet || !record || !a || !b || !c || !d || !quality)
        return std::nullopt;
    if (*type > 15 || *instig > 7 || *rarity > 15 || *combo > 15) return std::nullopt;
    ActionbarUseData u;
    u.use_type = static_cast<uint8_t>(*type);
    u.instigator = static_cast<uint8_t>(*instig);
    u.ability_id = *ability;
    u.skill_rank_id = *rank;
    u.pet_command_id = *pet;
    u.item_record_id = *record;
    u.item_guid = GuidValue{static_cast<uint32_t>(*a), static_cast<uint32_t>(*b), static_cast<uint32_t>(*c), static_cast<uint32_t>(*d)};
    u.item_rarity = static_cast<uint8_t>(*rarity);
    u.item_quality = static_cast<int32_t>(*quality);
    u.combo_button = static_cast<uint8_t>(*combo);
    return u;
}

bool fits_use(const ActionbarUseData& u) {
    return u.use_type <= 15 && u.instigator <= 7 && u.item_rarity <= 15 && u.combo_button <= 15;
}

}  // namespace

// ---- ServerPurchaseSkillBuild -------------------------------------------------------------------------------------------------

bool SkillBuild::refund_only() const { return purchases() == 0 && refunds() > 0; }

size_t SkillBuild::purchases() const {
    return static_cast<size_t>(std::count_if(changes.begin(), changes.end(), [](const SkillRankChange& c) { return c.purchase; }));
}

size_t SkillBuild::refunds() const { return changes.size() - purchases(); }

namespace {

constexpr const char* kBuildArray = "BuildChanges.AllocationChanges";
constexpr std::initializer_list<const char*> kBuildMembers = {
    "AllocationChanges.SkillRank.Guid", "AllocationChanges.SkillRank.TypeId", "AllocationChanges.SkillRank.Name",
    "AllocationChanges.SkillTree.Guid", "AllocationChanges.SkillTree.TypeId", "AllocationChanges.SkillTree.Name",
    "AllocationChanges.LevelingType.Guid", "AllocationChanges.LevelingType.TypeId", "AllocationChanges.LevelingType.Name",
    "AllocationChanges.bPurchase",
};

bool is_build_rpc(const RpcSchema& rpc) {
    return rpc.name && std::string_view(rpc.name) == "ServerPurchaseSkillBuild" && rpc_command(rpc, kBuildArray);
}

}  // namespace

std::optional<SkillBuild> parse_purchase_skill_build(const RpcCall& call) {
    if (!is_rpc(call, "ServerPurchaseSkillBuild") || !is_build_rpc(*call.rpc)) return std::nullopt;
    const auto h = element_handles(*call.rpc, kBuildArray, kBuildMembers);
    if (h.empty()) return std::nullopt;
    SkillBuild build;
    const Value* v = rpcf::value_of(*call.rpc, call.params, kBuildArray);
    if (!v) return build;   // an empty list is the default: not on the wire
    const ArrayValue* a = v->get_if<ArrayValue>();
    if (!a || a->elements.size() > kMaxSkillBuildChanges) return std::nullopt;
    for (const ElementState& e : a->elements) {
        const auto rank = take_id(e, h[0], h[1], h[2]);
        const auto tree = take_id(e, h[3], h[4], h[5]);
        const auto type = take_id(e, h[6], h[7], h[8]);
        const auto purchase = rpcf::bool_or(rpcf::element_value(e, h[9]), false);
        if (!rank || !tree || !type || !purchase) return std::nullopt;
        build.changes.push_back({*rank, *tree, *type, *purchase});
    }
    return build;
}

std::optional<RpcParams> purchase_skill_build_params(const RpcSchema& rpc, const SkillBuild& build) {
    if (!is_build_rpc(rpc) || build.changes.size() > kMaxSkillBuildChanges) return std::nullopt;
    const auto h = element_handles(rpc, kBuildArray, kBuildMembers);
    if (h.empty()) return std::nullopt;
    ArrayValue a;
    for (const SkillRankChange& c : build.changes) {
        ElementState e;
        put_id(e, h[0], h[1], h[2], c.rank);
        put_id(e, h[3], h[4], h[5], c.tree);
        put_id(e, h[6], h[7], h[8], c.leveling_type);
        e.values[h[9]] = Value::boolean(c.purchase);
        a.elements.push_back(std::move(e));
    }
    RpcParams params;
    if (!set_rpc_param(rpc, params, kBuildArray, Value(std::move(a)))) return std::nullopt;
    return params;
}
// (CoA) (C Y B)

// ---- ClientUpdateSkillOnActionbar ----------------------------------------------------------------------------------------------

std::optional<SkillOnActionbar> parse_update_skill_on_actionbar(const RpcCall& call) {
    if (!is_rpc(call, "ClientUpdateSkillOnActionbar")) return std::nullopt;
    const RpcSchema& rpc = *call.rpc;
    const auto skill = rpcf::int_or(rpcf::value_of(rpc, call.params, "SkillId"), 0);
    const auto tree = rpcf::int_or(rpcf::value_of(rpc, call.params, "SkillTreeId"), 0);
    const auto rank = rpcf::int_or(rpcf::value_of(rpc, call.params, "RankId"), 0);
    const auto refunding = rpcf::bool_or(rpcf::value_of(rpc, call.params, "bIsRefunding"), false);
    const auto hint = rpcf::bool_or(rpcf::value_of(rpc, call.params, "bUseHint"), false);
    if (!skill || !tree || !rank || !refunding || !hint || *rank < INT32_MIN || *rank > INT32_MAX) return std::nullopt;
    return SkillOnActionbar{*skill, *tree, static_cast<int32_t>(*rank), *refunding, *hint};
}

std::optional<RpcParams> update_skill_on_actionbar_params(const RpcSchema& rpc, const SkillOnActionbar& u) {
    if (!rpc.name || std::string_view(rpc.name) != "ClientUpdateSkillOnActionbar") return std::nullopt;
    RpcParams params;
    // A parameter at its default is not on the wire (a 0 send bit) and not in what a reader gives: it is left out here too.
    bool ok = u.skill_id == 0 || set_rpc_param(rpc, params, "SkillId", Value::i64(u.skill_id));
    ok = ok && (u.skill_tree_id == 0 || set_rpc_param(rpc, params, "SkillTreeId", Value::i64(u.skill_tree_id)));
    ok = ok && (u.rank_id == 0 || set_rpc_param(rpc, params, "RankId", Value::i64(u.rank_id)));
    ok = ok && set_rpc_param(rpc, params, "bIsRefunding", Value::boolean(u.is_refunding));
    ok = ok && set_rpc_param(rpc, params, "bUseHint", Value::boolean(u.use_hint));
    if (!ok) return std::nullopt;
    return params;
}

std::optional<Emission> client_update_skill_on_actionbar(const Actor& pc, const SkillOnActionbar& update, const ExportAcks& acks) {
    const RpcSchema* rpc = pc.obj.schema ? pc.obj.schema->rpc("ClientUpdateSkillOnActionbar") : nullptr;
    if (!rpc) return std::nullopt;
    const auto params = update_skill_on_actionbar_params(*rpc, update);
    if (!params) return std::nullopt;
    return call_rpc(pc, nullptr, "ClientUpdateSkillOnActionbar", *params, {}, acks);
}

// ---- the action bar ----------------------------------------------------------------------------------------------------------------

namespace {

bool is_payload_rpc(const RpcSchema& rpc) {
    return rpc.name && std::string_view(rpc.name) == "ClientReceiveActionbarSlotPayload" && rpc_command(rpc, "ActionbarSlotData");
}

bool is_set_slot_rpc(const RpcSchema& rpc) {
    return rpc.name && std::string_view(rpc.name) == "ServerSetActionbarSlotUseData" && rpc_command(rpc, "UseData.UseType");
}

}  // namespace

std::optional<ActionbarPayload> parse_actionbar_payload(const RpcCall& call) {
    if (!is_rpc(call, "ClientReceiveActionbarSlotPayload") || !is_payload_rpc(*call.rpc)) return std::nullopt;
    const RpcSchema& rpc = *call.rpc;
    const RepCommand* cmd = rpc_command(rpc, "ActionbarSlotData");
    const auto elems = rpcf::elements_of(rpc, *cmd);
    const uint16_t h_bank = rpcf::local(elems, "ActionbarSlotData.BankIndex"), h_slot = rpcf::local(elems, "ActionbarSlotData.SlotIndex");
    const auto use = use_handles(elems, "ActionbarSlotData.UseData.");
    if (!h_bank || !h_slot || !use) return std::nullopt;
    ActionbarPayload p;
    const auto granted = rpcf::bool_or(rpcf::value_of(rpc, call.params, "bAutoGranted"), false);
    if (!granted) return std::nullopt;
    p.auto_granted = *granted;
    const Value* v = rpcf::value_of(rpc, call.params, "ActionbarSlotData");
    if (!v) return p;   // an empty array is the default: not on the wire
    const ArrayValue* a = v->get_if<ArrayValue>();
    if (!a || a->elements.size() > kMaxActionbarSlots) return std::nullopt;
    for (const ElementState& e : a->elements) {
        const auto bank = rpcf::int_or(rpcf::element_value(e, h_bank), 0);
        const auto slot = rpcf::int_or(rpcf::element_value(e, h_slot), 0);
        const auto u = take_use(*use, [&](uint16_t h) { return rpcf::element_value(e, h); });
        if (!bank || !slot || !u) return std::nullopt;
        p.slots.push_back({static_cast<int32_t>(*bank), static_cast<int32_t>(*slot), *u});
    }
    return p;
}
// (C O A + Cyb)

std::optional<RpcParams> actionbar_payload_params(const RpcSchema& rpc, const ActionbarPayload& payload) {
    if (!is_payload_rpc(rpc) || payload.slots.size() > kMaxActionbarSlots) return std::nullopt;
    const RepCommand* cmd = rpc_command(rpc, "ActionbarSlotData");
    const auto elems = rpcf::elements_of(rpc, *cmd);
    const uint16_t h_bank = rpcf::local(elems, "ActionbarSlotData.BankIndex"), h_slot = rpcf::local(elems, "ActionbarSlotData.SlotIndex");
    const auto use = use_handles(elems, "ActionbarSlotData.UseData.");
    if (!h_bank || !h_slot || !use) return std::nullopt;
    ArrayValue a;
    for (const ActionbarSlot& s : payload.slots) {
        if (!fits_use(s.use)) return std::nullopt;
        ElementState e;
        e.values[h_bank] = Value::i64(s.bank_index);
        e.values[h_slot] = Value::i64(s.slot_index);
        put_use(e, *use, s.use);
        a.elements.push_back(std::move(e));
    }
    RpcParams params;
    // An empty array is the default: not on the wire, not in what a reader gives.
    if ((!payload.slots.empty() && !set_rpc_param(rpc, params, "ActionbarSlotData", Value(std::move(a)))) ||
        !set_rpc_param(rpc, params, "bAutoGranted", Value::boolean(payload.auto_granted)))
        return std::nullopt;
    return params;
}

std::optional<Emission> client_receive_actionbar_slot_payload(const Actor& pc, const ActionbarPayload& payload, const ExportAcks& acks) {
    const RpcSchema* rpc = pc.obj.schema ? pc.obj.schema->rpc("ClientReceiveActionbarSlotPayload") : nullptr;
    if (!rpc) return std::nullopt;
    const auto params = actionbar_payload_params(*rpc, payload);
    if (!params) return std::nullopt;
    return call_rpc(pc, nullptr, "ClientReceiveActionbarSlotPayload", *params, {}, acks);
}

std::optional<SetActionbarSlot> parse_set_actionbar_slot(const RpcCall& call) {
    if (!is_rpc(call, "ServerSetActionbarSlotUseData") || !is_set_slot_rpc(*call.rpc)) return std::nullopt;
    const RpcSchema& rpc = *call.rpc;
    const auto bank = rpcf::int_or(rpcf::value_of(rpc, call.params, "BankIndex"), 0);
    const auto slot = rpcf::int_or(rpcf::value_of(rpc, call.params, "SlotIndex"), 0);
    // the use data is a struct parameter: always sent, every member
    std::vector<uint16_t> h;
    for (const char* m : kUseMembers) {
        const RepCommand* c = rpc_command(rpc, std::string("UseData.") + m);
        if (!c) return std::nullopt;
        h.push_back(c->handle);
    }
    const auto u = take_use(h, [&](uint16_t handle) {
        const auto it = call.params.values.find(handle);
        return it == call.params.values.end() ? nullptr : &it->second;
    });
    if (!bank || !slot || !u || *bank < INT32_MIN || *bank > INT32_MAX || *slot < INT32_MIN || *slot > INT32_MAX) return std::nullopt;
    return SetActionbarSlot{static_cast<int32_t>(*bank), static_cast<int32_t>(*slot), *u};
}

std::optional<RpcParams> set_actionbar_slot_params(const RpcSchema& rpc, const SetActionbarSlot& slot) {
    if (!is_set_slot_rpc(rpc) || !fits_use(slot.use)) return std::nullopt;
    RpcParams params;
    bool ok = slot.bank_index == 0 || set_rpc_param(rpc, params, "BankIndex", Value::i64(slot.bank_index));
    ok = ok && (slot.slot_index == 0 || set_rpc_param(rpc, params, "SlotIndex", Value::i64(slot.slot_index)));
    const ActionbarUseData& u = slot.use;
    const Value values[kUseCount] = {Value::u64(u.use_type), Value::u64(u.instigator), Value::i64(u.ability_id), Value::i64(u.skill_rank_id),
                                     Value::i64(u.pet_command_id), Value::i64(u.item_record_id), guid_part(u.item_guid.a),
                                     guid_part(u.item_guid.b), guid_part(u.item_guid.c), guid_part(u.item_guid.d),
                                     Value::u64(u.item_rarity), Value::i64(u.item_quality), Value::u64(u.combo_button)};
    for (size_t i = 0; ok && i < kUseCount; ++i) ok = set_rpc_param(rpc, params, std::string("UseData.") + kUseMembers[i], values[i]);
    if (!ok) return std::nullopt;
    return params;
}

// ---- ServerSetUseRangedWeapon ------------------------------------------------------------------------------------------------

std::optional<bool> parse_set_use_ranged_weapon(const RpcCall& call) {
    if (!is_rpc(call, "ServerSetUseRangedWeapon")) return std::nullopt;
    return rpcf::bool_or(rpcf::value_of(*call.rpc, call.params, "bUseRanged"), false);
}

std::optional<RpcParams> set_use_ranged_weapon_params(const RpcSchema& rpc, bool use_ranged) {
    if (!rpc.name || std::string_view(rpc.name) != "ServerSetUseRangedWeapon") return std::nullopt;
    RpcParams params;
    if (!set_rpc_param(rpc, params, "bUseRanged", Value::boolean(use_ranged))) return std::nullopt;
    return params;
}

}}}  // namespace aoc::protocol::repl
