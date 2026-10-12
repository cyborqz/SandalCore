// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/leveling.cpp
//
//  See leveling.h.
// ============================================================================
#include "protocol/repl/leveling.h"

#include "protocol/repl/rpc_fields.h"
#include "protocol/schema/generated/index.h"

#include <algorithm>
#include <cstdlib>

namespace aoc { namespace protocol { namespace repl {

namespace {

using schema::RepCommand;

constexpr const char* kExperience = "ExperienceFastMap";
constexpr const char* kPoints = "SkillPointFastMap";
constexpr const char* kDebt = "ExperienceDebtFastMap";
constexpr const char* kCertifications = "LevelingCertificationsFastMap";
constexpr const char* kPurchases = "SkillTreePurchasesFastMap";

/// The local handles of an item's members in a map's layout.
struct Handles {
    uint16_t guid = 0, type_id = 0, name = 0, value = 0;
};

std::optional<Handles> handles_of(const FastArrayLayout& layout, const char* value_path) {
    Handles h;
    h.guid = rpcf::local(layout.element, "Items.Key.Guid");
    h.type_id = rpcf::local(layout.element, "Items.Key.TypeId");
    h.name = rpcf::local(layout.element, "Items.Key.Name");
    h.value = rpcf::local(layout.element, value_path);
    if (!h.guid || !h.type_id || !h.name || !h.value) return std::nullopt;
    return h;
}

ElementState key_item(const Handles& h, const DesignDataId& key) {
    ElementState es;
    es.values[h.guid] = Value::i64(key.guid);
    es.values[h.type_id] = Value::i64(key.type_id);
    es.values[h.name] = Value(key.name);
    return es;
}

std::optional<DesignDataId> read_key(const ElementState& es, const Handles& h) {
    const auto guid = rpcf::int_or(rpcf::element_value(es, h.guid), 0);
    const auto type = rpcf::int_or(rpcf::element_value(es, h.type_id), 0);
    const NameValue* name = rpcf::as<NameValue>(rpcf::element_value(es, h.name));
    if (!guid || !type || !name) return std::nullopt;
    DesignDataId d;
    d.guid = *guid;
    d.type_id = *type;
    d.name = *name;
    return d;
}

const FastArrayItem* find_item(const FastArrayState* st, const Handles& h, const DesignDataId& key) {
    if (!st) return nullptr;
    for (const FastArrayItem& it : st->items()) {
        const auto k = read_key(it.values, h);
        if (k && *k == key) return &it;
    }
    return nullptr;
}

/// Adds or changes the item of `key` so it holds `value` (a member `h.value`).
bool put(FastArrayState* st, const Handles& h, const DesignDataId& key, Value value) {
    if (!st) return false;
    ElementState es = key_item(h, key);
    es.values[h.value] = std::move(value);
    if (const FastArrayItem* it = find_item(st, h, key)) {
        if (it->values == es) return true;   // nothing to mark dirty
        return st->change(it->id, std::move(es));
    }
    return st->add(std::move(es)).has_value();
}
// -- CoA x C_Y_B --

bool erase(FastArrayState* st, const Handles& h, const DesignDataId& key) {
    const FastArrayItem* it = find_item(st, h, key);
    return it && st->remove(it->id);
}

std::optional<ArrayValue> rank_array(const FastArrayLayout& layout, const std::vector<DesignDataId>& ranks) {
    if (ranks.size() > kMaxTreeRanks) return std::nullopt;
    const RepCommand* ids = nullptr;
    for (const RepCommand& c : layout.element) {
        if (std::string_view(c.path) == "Items.Value.IDs") ids = &c;
    }
    if (!ids || ids->type != schema::PropType::DynamicArray || ids->elem_end > layout.table.size() || ids->elem_end < ids->elem_begin)
        return std::nullopt;
    const auto elems = layout.table.subspan(ids->elem_begin, ids->elem_end - ids->elem_begin);
    const uint16_t g = rpcf::local(elems, "IDs.Guid"), t = rpcf::local(elems, "IDs.TypeId"), n = rpcf::local(elems, "IDs.Name");
    if (!g || !t || !n) return std::nullopt;
    ArrayValue a;
    for (const DesignDataId& r : ranks) {
        ElementState e;
        e.values[g] = Value::i64(r.guid);
        e.values[t] = Value::i64(r.type_id);
        e.values[n] = Value(r.name);
        a.elements.push_back(std::move(e));
    }
    return a;
}

std::optional<std::vector<DesignDataId>> read_ranks(const FastArrayLayout& layout, const ElementState& item, const Handles& h) {
    const ArrayValue* a = rpcf::as<ArrayValue>(rpcf::element_value(item, h.value));
    if (!a) return std::nullopt;
    const RepCommand* ids = nullptr;
    for (const RepCommand& c : layout.element) {
        if (std::string_view(c.path) == "Items.Value.IDs") ids = &c;
    }
    if (!ids || ids->elem_end > layout.table.size() || ids->elem_end < ids->elem_begin) return std::nullopt;
    const auto elems = layout.table.subspan(ids->elem_begin, ids->elem_end - ids->elem_begin);
    Handles eh;
    eh.guid = rpcf::local(elems, "IDs.Guid");
    eh.type_id = rpcf::local(elems, "IDs.TypeId");
    eh.name = rpcf::local(elems, "IDs.Name");
    eh.value = 1;
    if (!eh.guid || !eh.type_id || !eh.name) return std::nullopt;
    std::vector<DesignDataId> out;
    for (const ElementState& e : a->elements) {
        const auto r = read_key(e, eh);
        if (!r) return std::nullopt;
        out.push_back(*r);
    }
    return out;
}

}  // namespace

LevelingState::LevelingState() : schema_(schema::gen::find_class_schema("ULevelingComponent")) {
    if (!schema_) std::abort();   // the generated schema is part of the library
}

// ---- the server's side -------------------------------------------------------------------------------------------------------

#define AOC_LEVELING_MAP(NAME, VALUE_PATH)                                                         \
    const auto layout = fast_array_layout(*schema_, NAME);                                         \
    const auto h = layout ? handles_of(*layout, VALUE_PATH) : std::nullopt;                        \
    if (!h) return false;                                                                          \
    FastArrayState* st = arrays_.get(*schema_, NAME);

bool LevelingState::set_experience(const DesignDataId& key, int64_t xp) {
    AOC_LEVELING_MAP(kExperience, "Items.Value")
    return put(st, *h, key, Value::i64(xp));
}

bool LevelingState::set_experience_debt(const DesignDataId& key, int64_t debt) {
    AOC_LEVELING_MAP(kDebt, "Items.Value")
    return put(st, *h, key, Value::i64(debt));
}

bool LevelingState::set_skill_points(const DesignDataId& key, uint16_t points) {
    AOC_LEVELING_MAP(kPoints, "Items.Value")
    return put(st, *h, key, Value::u64(points));
}

bool LevelingState::set_certification(const DesignDataId& key, int64_t certification) {
    AOC_LEVELING_MAP(kCertifications, "Items.Value")
    return put(st, *h, key, Value::i64(certification));
}

bool LevelingState::set_purchased_ranks(const DesignDataId& tree, const std::vector<DesignDataId>& ranks) {
    AOC_LEVELING_MAP(kPurchases, "Items.Value.IDs")
    auto a = rank_array(*layout, ranks);
    if (!a) return false;
    return put(st, *h, tree, Value(std::move(*a)));
}

bool LevelingState::resend_purchased_ranks(const DesignDataId& tree) {
    AOC_LEVELING_MAP(kPurchases, "Items.Value.IDs")
    const FastArrayItem* it = find_item(st, *h, tree);
    if (!it) return false;
    ElementState again = it->values;
    return st->change(it->id, std::move(again));
}

bool LevelingState::erase_experience(const DesignDataId& key) {
    AOC_LEVELING_MAP(kExperience, "Items.Value")
    return erase(st, *h, key);
}

bool LevelingState::erase_experience_debt(const DesignDataId& key) {
    AOC_LEVELING_MAP(kDebt, "Items.Value")
    return erase(st, *h, key);
}

bool LevelingState::erase_skill_points(const DesignDataId& key) {
    AOC_LEVELING_MAP(kPoints, "Items.Value")
    return erase(st, *h, key);
}

bool LevelingState::erase_certification(const DesignDataId& key) {
    AOC_LEVELING_MAP(kCertifications, "Items.Value")
    return erase(st, *h, key);
}

bool LevelingState::erase_purchased_ranks(const DesignDataId& tree) {
    AOC_LEVELING_MAP(kPurchases, "Items.Value.IDs")
    return erase(st, *h, tree);
}

#undef AOC_LEVELING_MAP

// ---- reading -------------------------------------------------------------------------------------------------------------------

namespace {

std::vector<LevelingInt64> int64_items(const schema::ClassSchema& schema, const FastArrays& arrays, const char* name) {
    std::vector<LevelingInt64> out;
    const auto layout = fast_array_layout(schema, name);
    const schema::NetField* f = schema.field(name);
    const auto h = layout ? handles_of(*layout, "Items.Value") : std::nullopt;
    const FastArrayState* st = f ? arrays.find(f->index) : nullptr;
    if (!h || !st) return out;
    for (const FastArrayItem& it : st->items()) {
        const auto k = read_key(it.values, *h);
        const auto v = rpcf::int_or(rpcf::element_value(it.values, h->value), 0);
        if (k && v) out.push_back({*k, *v});
    }
    return out;
}

}  // namespace

std::vector<LevelingInt64> LevelingState::experience() const { return int64_items(*schema_, arrays_, kExperience); }
std::vector<LevelingInt64> LevelingState::experience_debt() const { return int64_items(*schema_, arrays_, kDebt); }
std::vector<LevelingInt64> LevelingState::certifications() const { return int64_items(*schema_, arrays_, kCertifications); }

std::vector<LevelingPoints> LevelingState::skill_points() const {
    std::vector<LevelingPoints> out;
    const auto layout = fast_array_layout(*schema_, kPoints);
    const schema::NetField* f = schema_->field(kPoints);
    const auto h = layout ? handles_of(*layout, "Items.Value") : std::nullopt;
    const FastArrayState* st = f ? arrays_.find(f->index) : nullptr;
    if (!h || !st) return out;
    for (const FastArrayItem& it : st->items()) {
        const auto k = read_key(it.values, *h);
        const auto v = rpcf::uint_or(rpcf::element_value(it.values, h->value), 0);
        if (k && v) out.push_back({*k, static_cast<uint16_t>(*v)});
    }
    return out;
}
// == C-O-A | Cyb ==

std::vector<SkillTreePurchase> LevelingState::purchases() const {
    std::vector<SkillTreePurchase> out;
    const auto layout = fast_array_layout(*schema_, kPurchases);
    const schema::NetField* f = schema_->field(kPurchases);
    const auto h = layout ? handles_of(*layout, "Items.Value.IDs") : std::nullopt;
    const FastArrayState* st = f ? arrays_.find(f->index) : nullptr;
    if (!h || !st) return out;
    for (const FastArrayItem& it : st->items()) {
        const auto k = read_key(it.values, *h);
        auto ranks = read_ranks(*layout, it.values, *h);
        if (k && ranks) out.push_back({*k, std::move(*ranks)});
    }
    return out;
}

namespace {

template <class V>
std::optional<V> lookup(const std::vector<V>& items, const DesignDataId& key) {
    for (const V& i : items) {
        if (i.key == key) return i;
    }
    return std::nullopt;
}

}  // namespace

std::optional<int64_t> LevelingState::experience(const DesignDataId& key) const {
    const auto i = lookup(experience(), key);
    return i ? std::optional<int64_t>(i->value) : std::nullopt;
}

std::optional<int64_t> LevelingState::experience_debt(const DesignDataId& key) const {
    const auto i = lookup(experience_debt(), key);
    return i ? std::optional<int64_t>(i->value) : std::nullopt;
}

std::optional<uint16_t> LevelingState::skill_points(const DesignDataId& key) const {
    const auto i = lookup(skill_points(), key);
    return i ? std::optional<uint16_t>(i->value) : std::nullopt;
}

std::optional<int64_t> LevelingState::certification(const DesignDataId& key) const {
    const auto i = lookup(certifications(), key);
    return i ? std::optional<int64_t>(i->value) : std::nullopt;
}

std::optional<std::vector<DesignDataId>> LevelingState::purchased_ranks(const DesignDataId& tree) const {
    for (SkillTreePurchase& p : purchases()) {
        if (p.tree == tree) return std::move(p.ranks);
    }
    return std::nullopt;
}

// ---- the wire ------------------------------------------------------------------------------------------------------------------

bool LevelingState::apply(const FastArrayField& field) {
    const schema::NetField* f = schema_->field(field.field);
    if (!f || f->kind != schema::FieldKind::Property) return false;
    const std::string_view name = f->name;
    if (name != kExperience && name != kPoints && name != kDebt && name != kCertifications && name != kPurchases) return false;
    FastArrayState* st = arrays_.get(*schema_, field.field);
    return st && apply_fast_array_delta(*st, field.delta);
}

size_t LevelingState::write(emit::BunchWriter& out, FastArrayShadows& shadows, const CodecOptions& opts, bool* failed) const {
    return write_fast_array_fields(*schema_, arrays_, shadows, out, opts, failed);
}

}}}  // namespace aoc::protocol::repl
