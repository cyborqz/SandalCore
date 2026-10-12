// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/rep_block.cpp
//
//  See rep_block.h.  read_value is the C++ port of schema_check.py's
//  read_value / decode_rep_block / decode_fields.
// ============================================================================
#include "protocol/repl/rep_block.h"

#include "protocol/emit/rep_layout_writer.h"

#include <vector>

namespace aoc { namespace protocol { namespace repl {

using emit::BunchWriter;
using schema::PropType;
using schema::RepCommand;
using wire::PacketReader;

void RepShadowUndo::apply(RepShadow& shadow) const {
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        if (it->second) shadow.sent[it->first] = *it->second;
        else shadow.sent.erase(it->first);
    }
}

namespace {

int64_t sign_extend(uint64_t v, uint16_t width) {
    if (width >= 64) return static_cast<int64_t>(v);
    const uint64_t sign = 1ull << (width - 1);
    return static_cast<int64_t>((v ^ sign) - sign);
}

/// The element commands of the array command `cmd`, or nullopt when its range is bad.
std::optional<std::span<const RepCommand>> elements_of(const RepCommand& cmd, std::span<const RepCommand> table) {
    if (cmd.elem_end < cmd.elem_begin || cmd.elem_end > table.size()) return std::nullopt;
    return table.subspan(cmd.elem_begin, cmd.elem_end - cmd.elem_begin);
}

const Value* find_local(const ArrayValue* a, size_t element, uint16_t local) {
    if (!a || element >= a->elements.size()) return nullptr;
    const auto it = a->elements[element].values.find(local);
    return it == a->elements[element].values.end() ? nullptr : &it->second;
}
// [C.O.A x Cyb]

bool needs_send(const RepCommand& cmd, std::span<const RepCommand> table, const Value& now, const Value* sent);

/// An array needs sending when it is new, its count changed, an element value differs or an element value the client has
/// is gone from `now` (it is then sent as its zero: rep_block.h).  A removed value without a known zero counts as dirty:
/// the write then fails, loudly.
bool array_dirty(const RepCommand& cmd, std::span<const RepCommand> table, const ArrayValue& now, const ArrayValue& sent) {
    if (now.elements.size() != sent.elements.size()) return true;
    const auto elems = elements_of(cmd, table);
    if (!elems) return true;
    for (size_t i = 0; i < now.elements.size(); ++i) {
        const ElementState& e = now.elements[i];
        for (const auto& [local, v] : e.values) {
            if (local < 1 || local > elems->size()) return true;
            if (needs_send((*elems)[local - 1], table, v, find_local(&sent, i, local))) return true;
        }
        for (const auto& [local, prev] : sent.elements[i].values) {
            if (e.values.count(local)) continue;
            if (local < 1 || local > elems->size()) return true;
            const RepCommand& ec = (*elems)[local - 1];
            const auto zero = zero_value(ec);
            if (!zero || needs_send(ec, table, *zero, &prev)) return true;
        }
    }
    return false;
}

bool needs_send(const RepCommand& cmd, std::span<const RepCommand> table, const Value& now, const Value* sent) {
    if (!sent) return true;
    const ArrayValue* a = now.get_if<ArrayValue>();
    const ArrayValue* b = sent->get_if<ArrayValue>();
    if (a && b) return array_dirty(cmd, table, *a, *b);
    return !(now == *sent);
}

std::optional<Value> read_value_in(const RepCommand& cmd, std::span<const RepCommand> table, PacketReader& r,
                                   const CodecOptions& opts, int64_t& budget);

/// `budget` is what is left of kMaxBlockElements: a hostile count builds nothing once it is spent (the elements are
/// allocated before any of their values is read, and arrays nest).
std::optional<Value> read_array(const RepCommand& cmd, std::span<const RepCommand> table, PacketReader& r,
                                const CodecOptions& opts, int64_t& budget) {
    const auto elems = elements_of(cmd, table);
    if (!elems) return std::nullopt;
    const size_t count = static_cast<size_t>(r.read_bits(16));
    if (overran(r)) return std::nullopt;
    budget -= static_cast<int64_t>(count);
    if (budget < 0) return std::nullopt;
    const size_t per = elems->size();
    ArrayValue a;
    a.elements.resize(count);
    uint64_t last = 0;
    for (;;) {
        const auto h = r.read_sip();
        if (!h) return std::nullopt;
        if (*h == 0) break;
        if (per == 0 || *h <= last) return std::nullopt;   // an element handle in an array without element commands
        last = *h;
        const uint64_t element = (*h - 1) / per, local = (*h - 1) % per;
        if (element >= count) return std::nullopt;
        auto v = read_value_in((*elems)[local], table, r, opts, budget);
        if (!v) return std::nullopt;
        a.elements[element].values.emplace(static_cast<uint16_t>(local + 1), std::move(*v));
    }
    return Value(std::move(a));
}

/// The array's element values that differ from `sent`, ascending by element handle.  The values of `sent` that `now` no
/// longer has go out as their zero.
bool write_array(const RepCommand& cmd, std::span<const RepCommand> table, const ArrayValue& now, const ArrayValue* sent,
                 BunchWriter& out, const CodecOptions& opts) {
    const auto elems = elements_of(cmd, table);
    if (!elems || now.elements.size() > 0xFFFF) return false;
    const size_t per = elems->size();
    out.write_bits(now.elements.size(), 16);
    static const ElementState kNothing;
    for (size_t i = 0; i < now.elements.size(); ++i) {
        const ElementState& e = now.elements[i];
        const ElementState& old = sent && i < sent->elements.size() ? sent->elements[i] : kNothing;
        // The locals of both, ascending: a local only `old` has was removed.
        auto n = e.values.begin();
        auto o = old.values.begin();
        while (n != e.values.end() || o != old.values.end()) {
            const bool have_now = n != e.values.end() && (o == old.values.end() || n->first <= o->first);
            const uint16_t local = have_now ? n->first : o->first;
            const Value* prev = o != old.values.end() && o->first == local ? &o->second : nullptr;
            const Value* now_value = have_now ? &n->second : nullptr;
            if (have_now) ++n;
            if (prev) ++o;
            if (local < 1 || local > per) return false;
            const RepCommand& ec = (*elems)[local - 1];
            std::optional<Value> zero;
            if (!now_value) {   // removed: the client still has `prev`
                zero = zero_value(ec);
                if (!zero) return false;
                now_value = &*zero;
            }
            if (!needs_send(ec, table, *now_value, prev)) continue;
            out.write_sip(static_cast<uint64_t>(i) * per + local);
            if (!write_value(ec, table, *now_value, out, opts, prev)) return false;
        }
    }
    out.write_sip(0);
    return true;
}

}  // namespace

std::optional<Value> zero_value(const RepCommand& cmd) {
    switch (cmd.type) {
        case PropType::Bool:       return Value::boolean(false);
        case PropType::Int8:
        case PropType::Int16:
        case PropType::Int32:
        case PropType::Int64:      return Value::i64(0);
        case PropType::UInt8:
        case PropType::UInt16:
        case PropType::UInt32:
        case PropType::UInt64:
        case PropType::Enum:       return Value::u64(0);
        case PropType::Float:      return Value::f32(0.0f);
        case PropType::Double:     return Value::f64(0.0);
        case PropType::Name:       return Value(NameValue::hardcoded_name(0));   // NAME_None
        case PropType::Str:        return Value::str(std::string());
        case PropType::Object:     return Value(emit::FIntrepidNetworkGUID{});   // null
        case PropType::DynamicArray: return Value(ArrayValue{});
        case PropType::NetSerializeStruct:
            return cmd.struct_name ? zero_struct(cmd.struct_name) : std::nullopt;
        case PropType::Text:       // FText's and FSoftObjectPtr's zero forms are not verified
        case PropType::SoftObject:
        case PropType::Struct:
        case PropType::Unknown:
            break;
    }
    return std::nullopt;
}

namespace {
// ~ C O A & Cyb ~

std::optional<Value> read_value_in(const RepCommand& cmd, std::span<const RepCommand> table, PacketReader& r,
                                   const CodecOptions& opts, int64_t& budget) {
    std::optional<Value> out;
    switch (cmd.type) {
        case PropType::Bool:
            out = Value::boolean(r.read_bit() != 0);
            break;
        case PropType::Int8:
        case PropType::Int16:
        case PropType::Int32:
        case PropType::Int64:
            if (cmd.width_bits == 0 || cmd.width_bits > 64) return std::nullopt;
            out = Value::i64(sign_extend(r.read_bits(cmd.width_bits), cmd.width_bits));
            break;
        case PropType::UInt8:
        case PropType::UInt16:
        case PropType::UInt32:
        case PropType::UInt64:
        case PropType::Enum:
            if (cmd.width_bits == 0 || cmd.width_bits > 64) return std::nullopt;
            out = Value::u64(r.read_bits(cmd.width_bits));
            break;
        case PropType::Float:
            out = Value::f32(std::bit_cast<float>(r.read_uint32()));
            break;
        case PropType::Double:
            out = Value::f64(std::bit_cast<double>(r.read_uint64()));
            break;
        case PropType::Name:
            if (auto n = read_name(r)) out = Value(std::move(*n));
            break;
        case PropType::Str:
            if (auto s = read_fstring(r)) out = Value::str(std::move(*s));
            break;
        case PropType::Text:
            if (auto t = read_text(r)) out = Value(std::move(*t));
            break;
        case PropType::Object:
            out = read_object_value(r, opts.inline_objects);
            break;
        case PropType::SoftObject:
            if (auto s = read_soft_object(r)) out = Value(std::move(*s));
            break;
        case PropType::NetSerializeStruct:
            if (!cmd.struct_name) return std::nullopt;
            out = read_struct(cmd.struct_name, r, opts);
            break;
        case PropType::DynamicArray:
            out = read_array(cmd, table, r, opts, budget);
            break;
        case PropType::Struct:
        case PropType::Unknown:
            return std::nullopt;
    }
    if (!out || overran(r)) return std::nullopt;
    return out;
}

}  // namespace

std::optional<Value> read_value(const RepCommand& cmd, std::span<const RepCommand> table, PacketReader& r,
                                const CodecOptions& opts) {
    int64_t budget = static_cast<int64_t>(kMaxBlockElements);
    return read_value_in(cmd, table, r, opts, budget);
}

bool write_value(const RepCommand& cmd, std::span<const RepCommand> table, const Value& v, BunchWriter& out,
                 const CodecOptions& opts, const Value* sent) {
    switch (cmd.type) {
        case PropType::Bool: {
            const bool* b = v.get_if<bool>();
            if (!b) return false;
            out.write_bit(*b);
            return true;
        }
        case PropType::Int8:
        case PropType::Int16:
        case PropType::Int32:
        case PropType::Int64: {
            const int64_t* i = v.get_if<int64_t>();
            if (!i || cmd.width_bits == 0 || cmd.width_bits > 64) return false;
            const uint64_t mask = cmd.width_bits >= 64 ? ~0ull : (1ull << cmd.width_bits) - 1;
            out.write_bits(static_cast<uint64_t>(*i) & mask, cmd.width_bits);
            return true;
        }
        case PropType::UInt8:
        case PropType::UInt16:
        case PropType::UInt32:
        case PropType::UInt64:
        case PropType::Enum: {
            const uint64_t* u = v.get_if<uint64_t>();
            if (!u || cmd.width_bits == 0 || cmd.width_bits > 64) return false;
            out.write_bits(*u, cmd.width_bits);
            return true;
        }
        case PropType::Float: {
            const float* f = v.get_if<float>();
            if (!f) return false;
            out.write_uint32(std::bit_cast<uint32_t>(*f));
            return true;
        }
        case PropType::Double: {
            const double* d = v.get_if<double>();
            if (!d) return false;
            out.write_uint64(std::bit_cast<uint64_t>(*d));
            return true;
        }
        case PropType::Name: {
            const NameValue* n = v.get_if<NameValue>();
            return n && write_name(out, *n);
        }
        case PropType::Str: {
            const std::string* s = v.get_if<std::string>();
            return s && write_fstring(out, *s);
        }
        case PropType::Text: {
            const TextValue* t = v.get_if<TextValue>();
            return t && write_text(out, *t);
        }
        case PropType::Object: {
            if (const auto* inl = v.get_if<InlineObject>()) return write_inline_object(out, *inl);
            const auto* o = v.get_if<emit::FIntrepidNetworkGUID>();
            return o && write_object(out, *o);
        }
        case PropType::SoftObject: {
            const SoftObjectValue* s = v.get_if<SoftObjectValue>();
            return s && write_soft_object(out, *s);
        }
        case PropType::NetSerializeStruct:
            return cmd.struct_name && write_struct(cmd.struct_name, v, out, opts);
        case PropType::DynamicArray: {
            const ArrayValue* a = v.get_if<ArrayValue>();
            return a && write_array(cmd, table, *a, sent ? sent->get_if<ArrayValue>() : nullptr, out, opts);
        }
        case PropType::Struct:
        case PropType::Unknown:
            break;
    }
    return false;
}

size_t write_rep_block(const schema::ClassSchema& schema, const RepState& now, RepShadow& shadow, BunchWriter& out,
                       const CodecOptions& opts, bool* failed, const ParentMask* active, RepShadowUndo* undo) {
    if (failed) *failed = false;
    auto fail = [&]() -> size_t {
        if (failed) *failed = true;
        return 0;
    };
// [C_Y_B x C | O | A]

    struct Due { const RepCommand* cmd; const Value* value; const Value* sent; };
    std::vector<Due> due;
    for (const auto& [handle, value] : now.values()) {
        const RepCommand* cmd = schema.command(handle);
        if (!cmd || (cmd->flags & schema::kRepCustomDelta)) return fail();
        if (active && !active->active(cmd->parent)) continue;   // this connection does not get the property (yet)
        const auto it = shadow.sent.find(handle);
        const Value* sent = it == shadow.sent.end() ? nullptr : &it->second;
        if (needs_send(*cmd, schema.element_commands, value, sent)) due.push_back({cmd, &value, sent});
    }
    if (due.empty()) return 0;

    const size_t start = out.bit_pos();
    emit::RepLayoutWriter props(out);
    for (const Due& d : due) {
        if (!write_value(*d.cmd, schema.element_commands, *d.value, props.write_handle(d.cmd->handle), opts, d.sent)) {
            out.truncate(start);
            return fail();
        }
    }
    props.finish();
    if (!props.ok()) {   // handles come out of an ordered map: cannot happen
        out.truncate(start);
        return fail();
    }
    for (const Due& d : due) {
        const uint16_t handle = d.cmd->handle;
        if (undo) undo->entries.emplace_back(handle, d.sent ? std::optional<Value>(*d.sent) : std::nullopt);
        shadow.sent[handle] = *d.value;
    }
    return out.bit_pos() - start;
}

std::optional<RepState> read_rep_block(const schema::ClassSchema& schema, const uint8_t* data, size_t bits,
                                       size_t& pos, const CodecOptions& opts) {
    PacketReader r(data, (bits + 7) / 8, bits);
    r.set_pos(pos);
    if (r.read_bit()) return std::nullopt;   // bDoChecksum: a 32-bit checksum would follow every handle
    RepState state;
    uint64_t last = 0;
    int64_t budget = static_cast<int64_t>(kMaxBlockElements);
    for (;;) {
        const auto h = r.read_sip();
        if (!h) return std::nullopt;
        if (*h == 0) break;
        if (*h > schema.commands.size() || *h <= last) return std::nullopt;
        last = *h;
        const RepCommand& cmd = schema.commands[*h - 1];
        if (cmd.flags & schema::kRepCustomDelta) return std::nullopt;
        auto v = read_value_in(cmd, schema.element_commands, r, opts, budget);
        if (!v || !state.set(schema, static_cast<uint32_t>(*h), std::move(*v))) return std::nullopt;
    }
    if (overran(r)) return std::nullopt;
    pos = r.pos();
    return state;
}

// ---- the field loop ---------------------------------------------------------

bool write_field_header(const schema::ClassSchema& schema, uint32_t field, size_t payload_bits, BunchWriter& out) {
    if (field >= schema.num_fields()) return false;
    out.write_serialize_int(field, schema.field_max());
    out.write_sip(payload_bits);
    return true;
}

size_t write_field(const schema::ClassSchema& schema, uint32_t field, const BunchWriter& payload, BunchWriter& out) {
    const size_t start = out.bit_pos();
    if (!write_field_header(schema, field, payload.bit_pos(), out)) return 0;
    out.write_bit_range(payload.data(), 0, payload.bit_pos());
    return out.bit_pos() - start;
}

std::optional<FieldHeader> read_field_header(const schema::ClassSchema& schema, const uint8_t* data, size_t bits,
                                             size_t& pos) {
    PacketReader r(data, (bits + 7) / 8, bits);
    r.set_pos(pos);
    FieldHeader h;
    h.field = r.read_serialize_int(schema.field_max());
    const auto payload = r.read_sip();
    if (!payload || overran(r) || h.field >= schema.num_fields() || *payload > bits - r.pos()) return std::nullopt;
    h.payload_bits = static_cast<size_t>(*payload);
    pos = r.pos();
    return h;
}

}}}  // namespace aoc::protocol::repl
