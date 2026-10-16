// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/fast_array.cpp
//
//  See fast_array.h.  FastArrayDeltaSerialize's write side is
//  TFastArraySerializeHelper::ConditionalCreateNewDeltaState /
//  BuildChangedAndDeletedBuffers / WriteDeltaHeader (FastArraySerializer.h,
//  UE 5.6) and the classic loop `Writer << ID; NetSerializeStruct`; the items
//  are FRepLayout::SerializePropertiesForStruct.
// ============================================================================
#include "protocol/repl/fast_array.h"

#include "protocol/repl/node_layout.h"
#include "protocol/repl/rep_block.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace aoc { namespace protocol { namespace repl {

using emit::BunchWriter;
using schema::PropType;
using schema::RepCommand;
using wire::PacketReader;

namespace {

constexpr int kMaxItemDepth = 8;   // nested arrays inside an item; the captured items nest 1 deep

/// The element commands of the array command `cmd`.
std::optional<std::span<const RepCommand>> elements_of(const RepCommand& cmd, std::span<const RepCommand> table) {
    if (cmd.elem_end < cmd.elem_begin || cmd.elem_end > table.size()) return std::nullopt;
    return table.subspan(cmd.elem_begin, cmd.elem_end - cmd.elem_begin);
}

}  // namespace

// The three functions below are declared in fast_array.h: an RPC parameter's array elements (rpc.cpp) go out in the same
// form as an item's members.

bool element_fits(std::span<const RepCommand> cmds, std::span<const RepCommand> table, const ElementState& es, int depth) {
    if (depth > kMaxItemDepth || es.values.size() != cmds.size()) return false;
    for (size_t i = 0; i < cmds.size(); ++i) {
        const auto it = es.values.find(static_cast<uint16_t>(i + 1));
        if (it == es.values.end()) return false;
        const RepCommand& c = cmds[i];
        if (c.type == PropType::DynamicArray) {
            const ArrayValue* a = it->second.get_if<ArrayValue>();
            const auto sub = elements_of(c, table);
            if (!a || !sub || a->elements.size() > 0xFFFF) return false;
            for (const ElementState& e : a->elements) {
                if (!element_fits(*sub, table, e, depth + 1)) return false;
            }
        } else if (!value_matches(c, table, it->second)) {
            return false;
        }
    }
    return true;
}
// built by C | O | A with C | Y | B

/// SerializePropertiesForStruct, writing: every command in order, a dynamic
/// array as its uint16 count and its elements the same way.
bool write_element(std::span<const RepCommand> cmds, std::span<const RepCommand> table, const ElementState& es,
                   BunchWriter& out, const CodecOptions& opts, int depth) {
    if (depth > kMaxItemDepth) return false;
    for (size_t i = 0; i < cmds.size(); ++i) {
        const auto it = es.values.find(static_cast<uint16_t>(i + 1));
        if (it == es.values.end()) return false;
        const RepCommand& c = cmds[i];
        if (c.type == PropType::DynamicArray) {
            const ArrayValue* a = it->second.get_if<ArrayValue>();
            const auto sub = elements_of(c, table);
            if (!a || !sub || a->elements.size() > 0xFFFF) return false;
            out.write_bits(a->elements.size(), 16);
            for (const ElementState& e : a->elements) {
                if (!write_element(*sub, table, e, out, opts, depth + 1)) return false;
            }
        } else if (!write_value(c, table, it->second, out, opts)) {
            return false;
        }
    }
    return true;
}

std::optional<ElementState> read_element(std::span<const RepCommand> cmds, std::span<const RepCommand> table,
                                         PacketReader& r, const CodecOptions& opts, int depth) {
    if (depth > kMaxItemDepth) return std::nullopt;
    ElementState es;
    for (size_t i = 0; i < cmds.size(); ++i) {
        const RepCommand& c = cmds[i];
        if (c.type == PropType::DynamicArray) {
            const auto sub = elements_of(c, table);
            const size_t count = static_cast<size_t>(r.read_bits(16));
            if (!sub || overran(r)) return std::nullopt;
            ArrayValue a;
            for (size_t k = 0; k < count; ++k) {
                auto e = read_element(*sub, table, r, opts, depth + 1);
                if (!e) return std::nullopt;
                a.elements.push_back(std::move(*e));
            }
            es.values.emplace(static_cast<uint16_t>(i + 1), Value(std::move(a)));
        } else {
            auto v = read_value(c, table, r, opts);
            if (!v) return std::nullopt;
            es.values.emplace(static_cast<uint16_t>(i + 1), std::move(*v));
        }
    }
    return es;
}

namespace {

/// The fast arrays whose items are in the struct-delta form (fast_array.h): not supported.
bool is_struct_delta_array(const schema::NetField& f) {
    struct Entry { std::string_view owner, name; };
    static constexpr Entry kStructDelta[] = {
        {"AAoCGameStateBase", "ReplicatedGlobalQuestCache"},   // 63 captured payloads, all in that form
    };
    for (const Entry& e : kStructDelta) {
        if (e.owner == f.owner && e.name == f.name) return true;
    }
    return false;
}

/// IncrementArrayReplicationKey / the IDCounter's increment: +1, wrapping like an int32 and skipping INDEX_NONE.
int32_t next_key(int32_t key) {
    key = static_cast<int32_t>(static_cast<uint32_t>(key) + 1u);
    return key == kIndexNone ? static_cast<int32_t>(static_cast<uint32_t>(key) + 1u) : key;
}

}  // namespace

// ---- layout -----------------------------------------------------------------

std::optional<FastArrayLayout> fast_array_layout(const schema::ClassSchema& schema, uint32_t field) {
    const schema::NetField* f = schema.field(field);
    if (!f || f->kind != schema::FieldKind::Property || !(f->flags & schema::kRepFastArray) ||
        (f->flags & schema::kRepStaticArray) || is_struct_delta_array(*f))
        return std::nullopt;
    const RepCommand* first = schema.command(f->handle);
    if (!first) return std::nullopt;
    // The property's commands are the run with its parent index; exactly one is an array (the Items).
    const RepCommand* items = nullptr;
    for (uint32_t h = f->handle; h <= schema.commands.size(); ++h) {
        const RepCommand& c = schema.commands[h - 1];
        if (c.parent != first->parent) break;
        if (c.type != PropType::DynamicArray) continue;
        if (items) return std::nullopt;
        items = &c;
    }
    if (!items || items->elem_end <= items->elem_begin || items->elem_end > schema.element_commands.size())
        return std::nullopt;
    FastArrayLayout l;
    l.field = field;
    l.element = schema.elements(*items);
    l.table = schema.element_commands;
    // the one struct of the game whose NetDeltaSerialize writes a header of its own (node_layout.h)
    if (std::string_view(f->owner) == "ANodeLayoutReplicator" && std::string_view(f->name) == "LayoutAssetSetGuids") l.prefix_size = &node_layout_header_bits;
    return l;
}

std::optional<FastArrayLayout> fast_array_layout(const schema::ClassSchema& schema, std::string_view property_name) {
    const schema::NetField* f = schema.field(property_name);
    return f ? fast_array_layout(schema, f->index) : std::nullopt;
}

bool item_matches(const FastArrayLayout& layout, const ElementState& values) {
    return element_fits(layout.element, layout.table, values, 0);
}

// ---- state ------------------------------------------------------------------

const FastArrayItem* FastArrayState::find(int32_t id) const {
    for (const FastArrayItem& it : items_) {
        if (it.id == id) return &it;
    }
    return nullptr;
}

std::optional<int32_t> FastArrayState::add(ElementState values) {
    if (!item_matches(layout_, values)) return std::nullopt;
    id_counter_ = next_key(id_counter_);   // ++IDCounter, skipping INDEX_NONE
    FastArrayItem it;
    it.id = id_counter_;
    it.key = kIndexNone + 1;   // ReplicationKey starts at INDEX_NONE; MarkItemDirty increments it
    it.values = std::move(values);
    items_.push_back(std::move(it));
    mark_array_dirty();
    return id_counter_;
}

bool FastArrayState::change(int32_t id, ElementState values) {
    if (!item_matches(layout_, values)) return false;
    for (FastArrayItem& it : items_) {
        if (it.id != id) continue;
        it.values = std::move(values);
        it.key = static_cast<int32_t>(static_cast<uint32_t>(it.key) + 1u);   // ReplicationKey++
        mark_array_dirty();
        return true;
    }
    return false;
}
// ~ C-Y-B :: C O A ~

bool FastArrayState::set_values(int32_t id, ElementState values) {
    if (!item_matches(layout_, values)) return false;
    for (FastArrayItem& it : items_) {
        if (it.id != id) continue;
        it.values = std::move(values);
        return true;
    }
    return false;
}

bool FastArrayState::remove(int32_t id) {
    const auto it = std::find_if(items_.begin(), items_.end(), [id](const FastArrayItem& i) { return i.id == id; });
    if (it == items_.end()) return false;
    items_.erase(it);
    mark_array_dirty();
    return true;
}

void FastArrayState::mark_array_dirty() { array_key_ = next_key(array_key_); }

bool FastArrayState::set_prefix(const std::vector<uint8_t>& bytes, size_t bits) {
    std::vector<uint8_t> v(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(std::min(bytes.size(), (bits + 7) / 8)));
    v.resize((bits + 7) / 8, 0);
    if (bits % 8) v.back() &= static_cast<uint8_t>((1u << (bits % 8)) - 1);   // the bits past the header are not part of it
    if (v == prefix_ && bits == prefix_bits_) return false;
    prefix_ = std::move(v);
    prefix_bits_ = bits;
    return true;
}

bool FastArrayState::insert(int32_t id, ElementState values) {
    if (id == kIndexNone || find(id) || !item_matches(layout_, values)) return false;
    FastArrayItem it;
    it.id = id;
    it.key = 0;
    it.values = std::move(values);
    items_.push_back(std::move(it));
    id_counter_ = std::max(id_counter_, id);
    return true;
}

// ---- the writer -------------------------------------------------------------

size_t write_fast_array_delta(const FastArrayState& state, FastArrayShadow& shadow, BunchWriter& out,
                              const CodecOptions& opts, bool* failed) {
    if (failed) *failed = false;
    auto fail = [&]() -> size_t {
        if (failed) *failed = true;
        return 0;
    };

    // ConditionalCreateNewDeltaState: the same key as the connection's: nothing to send.
    if (state.array_key() == shadow.array_key && !shadow.resend) return 0;

    // BuildChangedAndDeletedBuffers
    std::unordered_map<int32_t, int32_t> old;
    old.reserve(shadow.items.size());
    for (const auto& [id, key] : shadow.items) old.emplace(id, key);

    std::vector<std::pair<int32_t, int32_t>> now;
    now.reserve(state.items().size());
    std::unordered_set<int32_t> present;
    present.reserve(state.items().size());
    std::vector<const FastArrayItem*> changed;
    int64_t delete_count = static_cast<int64_t>(shadow.items.size()) - static_cast<int64_t>(state.items().size());
    for (const FastArrayItem& it : state.items()) {
        now.emplace_back(it.id, it.key);
        present.insert(it.id);
        const auto o = old.find(it.id);
        if (o == old.end()) {
            changed.push_back(&it);   // new
            ++delete_count;
        } else if (o->second != it.key) {
            changed.push_back(&it);   // changed; an unchanged item "might have moved but we dont care"
        }
    }
    std::vector<int32_t> deleted;
    if (delete_count > 0) {
        for (const auto& entry : shadow.items) {
            if (present.count(entry.first)) continue;
            deleted.push_back(entry.first);
            if (--delete_count <= 0) break;
        }
    }
    if (changed.size() > static_cast<size_t>(kMaxFastArrayChanged) || deleted.size() > static_cast<size_t>(kMaxFastArrayDeleted))
        return fail();
// C | O | A / Cyb

    // WriteDeltaHeader, then `Writer << ID` and the item for each changed one.
    const size_t start = out.bit_pos();
    out.write_bit(1);   // bSupportsFastArrayDelta
    if (state.layout().prefix_size) {
        // the struct's own header (its NetDeltaSerialize writes it before the stock body); a layout with one and no header set cannot be written
        if (state.prefix_bits() == 0) {
            out.truncate(start);
            return fail();
        }
        out.write_bit_range(state.prefix().data(), 0, state.prefix_bits());
    }
    out.write_int32(state.array_key());
    out.write_int32(shadow.array_key);
    out.write_int32(static_cast<int32_t>(deleted.size()));
    out.write_int32(static_cast<int32_t>(changed.size()));
    for (const int32_t id : deleted) out.write_int32(id);
    const FastArrayLayout& layout = state.layout();
    for (const FastArrayItem* it : changed) {
        out.write_int32(it->id);
        if (!write_element(layout.element, layout.table, it->values, out, opts, 0)) {
            out.truncate(start);
            return fail();
        }
    }
    shadow.array_key = state.array_key();
    shadow.items = std::move(now);
    shadow.resend = false;
    return out.bit_pos() - start;
}

// ---- the reader -------------------------------------------------------------

bool FastArrayDelta::operator==(const FastArrayDelta& o) const {
    if (supports_delta != o.supports_delta || prefix_bits != o.prefix_bits || prefix != o.prefix || array_key != o.array_key || base_key != o.base_key ||
        deleted != o.deleted || changed.size() != o.changed.size())
        return false;
    for (size_t i = 0; i < changed.size(); ++i) {
        if (changed[i].id != o.changed[i].id || !(changed[i].values == o.changed[i].values)) return false;
    }
    return true;
}

std::optional<FastArrayDelta> read_fast_array_delta(const FastArrayLayout& layout, const uint8_t* data, size_t end,
                                                    size_t& pos, const CodecOptions& opts) {
    PacketReader r(data, (end + 7) / 8, end);
    r.set_pos(pos);
    FastArrayDelta d;
    d.supports_delta = r.read_bit() != 0;
    if (layout.prefix_size) {
        if (overran(r)) return std::nullopt;
        const auto bits = layout.prefix_size(data, end, r.pos());
        if (!bits || r.pos() + *bits > end) return std::nullopt;
        d.prefix.assign((*bits + 7) / 8, 0);
        for (size_t i = 0; i < *bits; ++i) {
            if (r.read_bit()) d.prefix[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
        }
        d.prefix_bits = *bits;
    }
    d.array_key = static_cast<int32_t>(r.read_uint32());
    d.base_key = static_cast<int32_t>(r.read_uint32());
    const int32_t num_deletes = static_cast<int32_t>(r.read_uint32());
    const int32_t num_changed = static_cast<int32_t>(r.read_uint32());
    if (overran(r) || num_deletes < 0 || num_changed < 0 || num_deletes > kMaxFastArrayDeleted ||
        num_changed > kMaxFastArrayChanged)
        return std::nullopt;
    if (static_cast<uint64_t>(num_deletes) * 32 + static_cast<uint64_t>(num_changed) * 32 > r.remaining_bits())
        return std::nullopt;
    for (int32_t i = 0; i < num_deletes; ++i) d.deleted.push_back(static_cast<int32_t>(r.read_uint32()));
    for (int32_t i = 0; i < num_changed; ++i) {
        FastArrayDelta::Changed c;
        c.id = static_cast<int32_t>(r.read_uint32());
        if (overran(r)) return std::nullopt;
        auto values = read_element(layout.element, layout.table, r, opts, 0);
        if (!values) return std::nullopt;
        c.values = std::move(*values);
        d.changed.push_back(std::move(c));
    }
    if (overran(r)) return std::nullopt;
    pos = r.pos();
    return d;
}

bool apply_fast_array_delta(FastArrayState& state, const FastArrayDelta& delta) {
    for (const auto& c : delta.changed) {
        if (!item_matches(state.layout(), c.values)) return false;
    }
    for (const int32_t id : delta.deleted) state.remove(id);   // an unknown ID is skipped, as the client does
    for (const auto& c : delta.changed) {
        if (state.find(c.id)) state.change(c.id, c.values);
        else state.insert(c.id, c.values);
    }
    state.set_array_key(delta.array_key);
    if (state.layout().prefix_size) state.set_prefix(delta.prefix, delta.prefix_bits);
    return true;
}

// ---- an object's fast arrays -------------------------------------------------

FastArrayState* FastArrays::get(const schema::ClassSchema& schema, uint32_t field) {
    const auto it = arrays_.find(field);
    if (it != arrays_.end()) return &it->second;
    const auto layout = fast_array_layout(schema, field);
    if (!layout) return nullptr;
    return &arrays_.emplace(field, FastArrayState(*layout)).first->second;
}

FastArrayState* FastArrays::get(const schema::ClassSchema& schema, std::string_view property_name) {
    const schema::NetField* f = schema.field(property_name);
    return f ? get(schema, f->index) : nullptr;
}

const FastArrayState* FastArrays::find(uint32_t field) const {
    const auto it = arrays_.find(field);
    return it == arrays_.end() ? nullptr : &it->second;
}

void FastArrayShadowsUndo::apply(FastArrayShadows& shadows) const {
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        if (it->second) shadows.by_field[it->first] = *it->second;
        else shadows.by_field.erase(it->first);
    }
}
// C | O | A + Cyb

size_t write_fast_array_fields(const schema::ClassSchema& schema, const FastArrays& arrays, FastArrayShadows& shadows,
                               BunchWriter& out, const CodecOptions& opts, bool* failed, const ParentMask* active,
                               FastArrayShadowsUndo* undo) {
    if (failed) *failed = false;
    auto fail = [&]() -> size_t {
        if (failed) *failed = true;
        return 0;
    };

    struct Pending {
        uint32_t        field;
        BunchWriter     payload;
        FastArrayShadow shadow;
    };
    std::vector<Pending> pending;
    for (const auto& [field, state] : arrays.all()) {
        if (active) {   // the property's parent is this connection's to have or not
            const schema::NetField* f = schema.field(field);
            const schema::RepCommand* first = f ? schema.command(f->handle) : nullptr;
            if (first && !active->active(first->parent)) continue;
        }
        const auto old = shadows.by_field.find(field);
        // Nothing changed (the key is the shadow's): no copy of the shadow's items.
        if (state.array_key() == (old == shadows.by_field.end() ? 0 : old->second.array_key) && (old == shadows.by_field.end() || !old->second.resend)) continue;
        Pending p{field, BunchWriter(), old == shadows.by_field.end() ? FastArrayShadow{} : old->second};
        bool bad = false;
        const size_t bits = write_fast_array_delta(state, p.shadow, p.payload, opts, &bad);
        if (bad) return fail();
        if (bits) pending.push_back(std::move(p));
    }
    if (pending.empty()) return 0;

    const size_t start = out.bit_pos();
    for (const Pending& p : pending) {
        if (!write_field(schema, p.field, p.payload, out)) {
            out.truncate(start);
            return fail();
        }
    }
    for (Pending& p : pending) {
        if (undo) {
            const auto old = shadows.by_field.find(p.field);
            undo->entries.emplace_back(p.field, old == shadows.by_field.end() ? std::nullopt : std::optional<FastArrayShadow>(old->second));
        }
        shadows.by_field[p.field] = std::move(p.shadow);
    }
    return out.bit_pos() - start;
}

std::optional<std::vector<FastArrayField>> read_fast_array_fields(const schema::ClassSchema& schema, const uint8_t* data,
                                                                  size_t end, size_t& pos, const CodecOptions& opts) {
    size_t p = pos;
    std::vector<FastArrayField> fields;
    while (p < end) {
        const auto header = read_field_header(schema, data, end, p);   // p: the payload's first bit
        if (!header) return std::nullopt;
        const auto layout = fast_array_layout(schema, header->field);
        if (!layout) return std::nullopt;
        const size_t payload_end = p + header->payload_bits;
        auto delta = read_fast_array_delta(*layout, data, payload_end, p, opts);
        if (!delta || p != payload_end) return std::nullopt;
        fields.push_back({header->field, std::move(*delta)});
    }
    pos = p;
    return fields;
}

}}}  // namespace aoc::protocol::repl
