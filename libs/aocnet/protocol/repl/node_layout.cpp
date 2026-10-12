// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/node_layout.cpp (see node_layout.h)
// ============================================================================
#include "protocol/repl/node_layout.h"

#include "protocol/repl/codecs.h"

#include <algorithm>

namespace aoc { namespace protocol { namespace repl {

using emit::BunchWriter;
using wire::PacketReader;

namespace {

constexpr size_t kMaxTagChars = 255;
constexpr size_t kItemCommands = 7;

bool ascii_tag(const std::string& s) {
    if (s.empty() || s.size() > kMaxTagChars) return false;
    return std::all_of(s.begin(), s.end(), [](char c) { return c > 0 && static_cast<unsigned char>(c) < 0x80; });
}

/// The header from `r`'s position; the reader advances.  nullopt for a read past the end or a tag form that is not the plain FString one.
std::optional<NodeLayoutHeader> read_header(PacketReader& r) {
    NodeLayoutHeader h;
    h.level = r.read_uint8();
    h.culture = r.read_uint8();
    h.season = r.read_uint8();
    if (overran(r)) return std::nullopt;
    if (!r.read_bit()) {   // not empty
        const uint64_t n = r.read_bits(kNodeLayoutTagCountBits);
        for (uint64_t i = 0; i < n; ++i) {
            if (r.read_bit()) return std::nullopt;   // a hardcoded FName: not seen
            const int32_t len = static_cast<int32_t>(r.read_uint32());
            if (overran(r) || len < 2 || len > static_cast<int32_t>(kMaxTagChars) + 1) return std::nullopt;
            std::string s;
            s.reserve(static_cast<size_t>(len));
            for (int32_t k = 0; k < len; ++k) s.push_back(static_cast<char>(r.read_uint8()));
            if (overran(r) || s.back() != '\0') return std::nullopt;
            s.pop_back();
            if (r.read_uint32() != 0) return std::nullopt;   // an FName number: not seen
            h.tags.push_back(std::move(s));
        }
    }
    h.plot_version = r.read_uint8();
    if (overran(r)) return std::nullopt;
    return h;
}

}  // namespace

// ---- the header ---------------------------------------------------------------------------------------------------------------------------------

bool write_node_layout_header(const NodeLayoutHeader& header, BunchWriter& out) {
    if (header.tags.size() >= (1u << kNodeLayoutTagCountBits)) return false;
    for (const std::string& t : header.tags) {
        if (!ascii_tag(t)) return false;
    }
    out.write_uint8(header.level);
    out.write_uint8(header.culture);
    out.write_uint8(header.season);
    out.write_bit(header.tags.empty() ? 1 : 0);   // FGameplayTagContainer::NetSerialize: IsEmpty
    if (!header.tags.empty()) {
        out.write_bits(header.tags.size(), kNodeLayoutTagCountBits);
        for (const std::string& t : header.tags) {
            out.write_bit(0);               // not a hardcoded FName
            out.write_fstring_ansi(t);      // int32 length with the NUL, the bytes, the NUL
            out.write_int32(0);             // the FName number
        }
    }
    out.write_uint8(header.plot_version);
    return true;
}

std::optional<size_t> node_layout_header_bits(const uint8_t* data, size_t end, size_t pos) {
    if (pos >= end) return std::nullopt;
    PacketReader r(data, (end + 7) / 8, end);
    r.set_pos(pos);
    if (!read_header(r)) return std::nullopt;
    return r.pos() - pos;
}
// [C O A ~ Cyb]

std::optional<NodeLayoutHeader> read_node_layout_header(const uint8_t* data, size_t end, size_t& pos) {
    if (pos >= end) return std::nullopt;
    PacketReader r(data, (end + 7) / 8, end);
    r.set_pos(pos);
    auto h = read_header(r);
    if (!h) return std::nullopt;
    pos = r.pos();
    return h;
}

// ---- the items ----------------------------------------------------------------------------------------------------------------------------------

ElementState node_layout_item_values(const NodeLayoutItem& item) {
    ElementState es;
    es.values.emplace(1, Value::i64(item.owner_guid));
    es.values.emplace(2, Value::i64(item.owner_secondary));
    es.values.emplace(3, Value::i64(item.owner_index));
    es.values.emplace(4, Value::i64(item.asset_set));
    es.values.emplace(5, Value(item.rotation));
    es.values.emplace(6, Value(item.translation));
    es.values.emplace(7, Value(item.scale));
    return es;
}

std::optional<NodeLayoutItem> node_layout_item_of(const ElementState& values) {
    if (values.values.size() != kItemCommands) return std::nullopt;
    const auto get = [&](uint16_t h) -> const Value* {
        const auto it = values.values.find(h);
        return it == values.values.end() ? nullptr : &it->second;
    };
    const Value *v1 = get(1), *v2 = get(2), *v3 = get(3), *v4 = get(4), *v5 = get(5), *v6 = get(6), *v7 = get(7);
    if (!v1 || !v2 || !v3 || !v4 || !v5 || !v6 || !v7) return std::nullopt;
    const int64_t *a = v1->get_if<int64_t>(), *b = v2->get_if<int64_t>(), *c = v3->get_if<int64_t>(), *d = v4->get_if<int64_t>();
    const Quat* q = v5->get_if<Quat>();
    const Vec3d *t = v6->get_if<Vec3d>(), *s = v7->get_if<Vec3d>();
    if (!a || !b || !c || !d || !q || !t || !s) return std::nullopt;
    NodeLayoutItem it;
    it.owner_guid = *a;
    it.owner_secondary = static_cast<int32_t>(*b);
    it.owner_index = static_cast<int32_t>(*c);
    it.asset_set = *d;
    it.rotation = *q;
    it.translation = *t;
    it.scale = *s;
    return it;
}

bool node_layout_schema_ok(const schema::ClassSchema& replicator) {
    if (std::string_view(replicator.name) != kNodeLayoutClass) return false;
    const auto layout = fast_array_layout(replicator, kNodeLayoutProperty);
    if (!layout || !layout->prefix_size || layout->element.size() != kItemCommands) return false;
    using schema::PropType;
    const PropType want[kItemCommands] = {PropType::Int64, PropType::Int32, PropType::Int32, PropType::Int64,
                                         PropType::NetSerializeStruct, PropType::NetSerializeStruct, PropType::NetSerializeStruct};
    for (size_t i = 0; i < kItemCommands; ++i) {
        if (layout->element[i].type != want[i]) return false;
    }
    return std::string_view(layout->element[4].struct_name ? layout->element[4].struct_name : "") == "FQuat" &&
           std::string_view(layout->element[5].struct_name ? layout->element[5].struct_name : "") == "FVector" &&
           std::string_view(layout->element[6].struct_name ? layout->element[6].struct_name : "") == "FVector";
}

// ---- whole payloads -----------------------------------------------------------------------------------------------------------------------------

std::optional<NodeLayoutPayload> read_node_layout_payload(const schema::ClassSchema& replicator, const uint8_t* data, size_t bits) {
    const auto layout = fast_array_layout(replicator, kNodeLayoutProperty);
    if (!layout || !layout->prefix_size) return std::nullopt;
    size_t pos = 0;
    const auto delta = read_fast_array_delta(*layout, data, bits, pos);
    if (!delta || pos != bits || !delta->supports_delta) return std::nullopt;
    size_t hp = 0;
    const auto header = read_node_layout_header(delta->prefix.data(), delta->prefix_bits, hp);
    if (!header || hp != delta->prefix_bits) return std::nullopt;
    NodeLayoutPayload p;
    p.header = *header;
    p.array_key = delta->array_key;
    p.base_key = delta->base_key;
    p.deleted = delta->deleted;
    for (const auto& c : delta->changed) {
        auto item = node_layout_item_of(c.values);
        if (!item) return std::nullopt;
        p.items.push_back({c.id, std::move(*item)});
    }
    return p;
}

size_t write_node_layout_snapshot(const schema::ClassSchema& replicator, const NodeLayoutPayload& payload, BunchWriter& out) {
    if (payload.base_key != 0 || !payload.deleted.empty()) return 0;
    const auto layout = fast_array_layout(replicator, kNodeLayoutProperty);
    if (!layout || !layout->prefix_size) return 0;
    BunchWriter header(64);
    if (!write_node_layout_header(payload.header, header)) return 0;
    FastArrayState state(*layout);
    state.set_prefix(header.bytes(), header.bit_pos());
    for (const auto& e : payload.items) {
        if (!state.insert(e.id, node_layout_item_values(e.item))) return 0;
    }
    state.set_array_key(payload.array_key);
    FastArrayShadow shadow;   // empty: key 0, no items
    bool failed = false;
    const size_t bits = write_fast_array_delta(state, shadow, out, {}, &failed);
    return failed ? 0 : bits;
}

// ---- the server's state -------------------------------------------------------------------------------------------------------------------------
// -- C-Y-B x C.O.A --

std::optional<NodeLayoutPayload> read_node_layout_state(const schema::ClassSchema& replicator, const FastArrays& arrays) {
    const schema::NetField* f = replicator.field(kNodeLayoutProperty);
    if (!f) return std::nullopt;
    const FastArrayState* state = arrays.find(f->index);
    if (!state || state->prefix_bits() == 0) return std::nullopt;
    size_t hp = 0;
    const auto header = read_node_layout_header(state->prefix().data(), state->prefix_bits(), hp);
    if (!header) return std::nullopt;
    NodeLayoutPayload p;
    p.header = *header;
    p.array_key = state->array_key();
    for (const FastArrayItem& it : state->items()) {
        auto item = node_layout_item_of(it.values);
        if (!item) return std::nullopt;
        p.items.push_back({it.id, std::move(*item)});
    }
    return p;
}

bool sync_node_layout(const schema::ClassSchema& replicator, FastArrays& arrays, const NodeLayoutHeader& header, const std::vector<NodeLayoutItem>& items,
                      NodeLayoutSync* result) {
    if (result) *result = {};
    if (!node_layout_schema_ok(replicator)) return false;
    BunchWriter hw(64);
    if (!write_node_layout_header(header, hw)) return false;
    std::vector<ElementState> values;
    values.reserve(items.size());
    const auto layout = fast_array_layout(replicator, kNodeLayoutProperty);
    for (const NodeLayoutItem& it : items) {
        ElementState es = node_layout_item_values(it);
        if (!item_matches(*layout, es)) return false;
        values.push_back(std::move(es));
    }
    FastArrayState* state = arrays.get(replicator, kNodeLayoutProperty);
    if (!state) return false;

    const bool same_header = state->prefix_bits() == hw.bit_pos() && [&] {
        FastArrayState probe(*layout);
        probe.set_prefix(hw.bytes(), hw.bit_pos());
        return probe.prefix() == state->prefix();
    }();
    bool same_items = state->items().size() == values.size();
    for (size_t i = 0; same_items && i < values.size(); ++i) same_items = state->items()[i].values == values[i];
    if (same_header && same_items && state->prefix_bits() != 0) return true;   // nothing to say

    NodeLayoutSync r;
    r.header_changed = !same_header;
    std::vector<int32_t> old;
    for (const FastArrayItem& it : state->items()) old.push_back(it.id);
    for (const int32_t id : old) {
        state->remove(id);
        ++r.removed;
    }
    for (ElementState& v : values) {
        if (!state->add(std::move(v))) return false;   // (cannot happen: every item was checked)
        ++r.added;
    }
    state->set_prefix(hw.bytes(), hw.bit_pos());
    if (r.added == 0 && r.removed == 0) state->mark_array_dirty();   // a header change alone must reach the clients too (an item change has moved the key already)
    r.changed = true;
    if (result) *result = r;
    return true;
}

}}}  // namespace aoc::protocol::repl
