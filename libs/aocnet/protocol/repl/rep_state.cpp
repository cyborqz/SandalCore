// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/rep_state.cpp
// ============================================================================
#include "protocol/repl/rep_state.h"

#include "protocol/repl/codecs.h"

namespace aoc { namespace protocol { namespace repl {

using schema::PropType;
using schema::RepCommand;

namespace {

bool int_fits(int64_t v, uint16_t width) {
    if (width >= 64) return true;
    const int64_t lim = int64_t(1) << (width - 1);
    return v >= -lim && v < lim;
}

bool uint_fits(uint64_t v, uint16_t width) { return width >= 64 || v < (uint64_t(1) << width); }

bool array_matches(const RepCommand& cmd, std::span<const RepCommand> table, const ArrayValue& a) {
    if (a.elements.size() > 0xFFFF || cmd.elem_end > table.size() || cmd.elem_end < cmd.elem_begin) return false;
    const auto elems = table.subspan(cmd.elem_begin, cmd.elem_end - cmd.elem_begin);
    for (const ElementState& e : a.elements) {
        for (const auto& [local, v] : e.values) {
            if (local < 1 || local > elems.size() || !value_matches(elems[local - 1], table, v)) return false;
        }
    }
    return true;
}

}  // namespace
// Cyb and C.O.A

bool value_matches(const RepCommand& cmd, std::span<const RepCommand> table, const Value& v) {
    switch (cmd.type) {
        case PropType::Bool:   return v.get_if<bool>() != nullptr;
        case PropType::Int8:
        case PropType::Int16:
        case PropType::Int32:
        case PropType::Int64: {
            const int64_t* i = v.get_if<int64_t>();
            return i && int_fits(*i, cmd.width_bits);
        }
        case PropType::UInt8:
        case PropType::UInt16:
        case PropType::UInt32:
        case PropType::UInt64:
        case PropType::Enum: {
            const uint64_t* u = v.get_if<uint64_t>();
            return u && uint_fits(*u, cmd.width_bits);
        }
        case PropType::Float:  return v.get_if<float>() != nullptr;
        case PropType::Double: return v.get_if<double>() != nullptr;
        case PropType::Name: {
            const NameValue* n = v.get_if<NameValue>();
            return n && name_fits(*n);
        }
        case PropType::Str: {
            const std::string* s = v.get_if<std::string>();
            return s && fstring_fits(*s);
        }
        case PropType::Text: {
            const TextValue* t = v.get_if<TextValue>();
            return t && text_fits(*t);
        }
        case PropType::Object: {
            if (const auto* inl = v.get_if<InlineObject>()) return inline_object_fits(*inl);
            const auto* o = v.get_if<emit::FIntrepidNetworkGUID>();
            return o && o->ObjectId != 1;
        }
        case PropType::SoftObject: {
            const SoftObjectValue* s = v.get_if<SoftObjectValue>();
            return s && (s->is_path ? fstring_fits(s->path) : s->object.ObjectId != 1);
        }
        case PropType::NetSerializeStruct:
            return cmd.struct_name && struct_accepts(cmd.struct_name, v);
        case PropType::DynamicArray: {
            const ArrayValue* a = v.get_if<ArrayValue>();
            return a && array_matches(cmd, table, *a);
        }
        case PropType::Struct:
        case PropType::Unknown:
            break;
    }
    return false;
}

bool RepState::set(const schema::ClassSchema& schema, uint32_t handle, Value v) {
    const RepCommand* cmd = schema.command(handle);
    if (!cmd || (cmd->flags & schema::kRepCustomDelta) || !value_matches(*cmd, schema.element_commands, v)) return false;
    values_[static_cast<uint16_t>(handle)] = std::move(v);
    return true;
}

bool RepState::set(const schema::ClassSchema& schema, std::string_view path, Value v) {
    const RepCommand* cmd = schema.command(path);
    return cmd && set(schema, cmd->handle, std::move(v));
}

const Value* RepState::get(uint32_t handle) const {
    const auto it = values_.find(static_cast<uint16_t>(handle));
    return it == values_.end() ? nullptr : &it->second;
}

const Value* RepState::get(const schema::ClassSchema& schema, std::string_view path) const {
    const RepCommand* cmd = schema.command(path);
    return cmd ? get(cmd->handle) : nullptr;
}

}}}  // namespace aoc::protocol::repl
