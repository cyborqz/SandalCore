// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/rpc_fields.h
//
//  Small helpers the typed RPC layers of plans D1 and D2 (skill_rpc, hit_rpc, leveling) share: finding a parameter's
//  value in an RpcCall's RpcParams by its dotted path, the element commands of an array parameter and their local handles,
//  and typed access to a Value.  Header only.
//
//  A parameter left at its default is not on the wire (a 0 send bit) and not in RpcParams; `value_of` then returns nullptr
//  and the readers below fall back to the default.  An array element holds its members by local handle (the 1-based
//  position of the member's command in the array's element commands).
// ============================================================================
#pragma once

#include "protocol/repl/rpc.h"
#include "protocol/repl/value.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace aoc { namespace protocol { namespace repl { namespace rpcf {

/// The element commands of an array command of `rpc`, empty when its range is bad.
inline std::span<const schema::RepCommand> elements_of(const schema::RpcSchema& rpc, const schema::RepCommand& array) {
    if (array.type != schema::PropType::DynamicArray || array.elem_end < array.elem_begin ||
        array.elem_end > rpc.element_commands.size())
        return {};
    return rpc.element_commands.subspan(array.elem_begin, array.elem_end - array.elem_begin);
}

/// The local handle of the element command `path` in `elems`, 0 when there is none.
inline uint16_t local(std::span<const schema::RepCommand> elems, std::string_view path) {
    for (size_t i = 0; i < elems.size(); ++i) {
        if (path == elems[i].path) return static_cast<uint16_t>(i + 1);
    }
    return 0;
}

/// The value of command `path` of the RPC in `params`, nullptr when it is not there.
inline const Value* value_of(const schema::RpcSchema& rpc, const RpcParams& params, std::string_view path) {
    const schema::RepCommand* c = rpc_command(rpc, path);
    if (!c) return nullptr;
    const auto it = params.values.find(c->handle);
    return it == params.values.end() ? nullptr : &it->second;
}

/// The value of an array element's member `handle`, nullptr when it is not there.
inline const Value* element_value(const ElementState& e, uint16_t handle) {
    if (!handle) return nullptr;
    const auto it = e.values.find(handle);
    return it == e.values.end() ? nullptr : &it->second;
}

template <class T>
const T* as(const Value* v) { return v ? v->get_if<T>() : nullptr; }
// [C_Y_B x CoA]

/// A signed integer member: the Value, or `fallback` when it is not there; nullopt when it is of another kind.
inline std::optional<int64_t> int_or(const Value* v, int64_t fallback) {
    if (!v) return fallback;
    if (const int64_t* i = v->get_if<int64_t>()) return *i;
    return std::nullopt;
}

/// An unsigned integer (or enum) member, the same way.
inline std::optional<uint64_t> uint_or(const Value* v, uint64_t fallback) {
    if (!v) return fallback;
    if (const uint64_t* u = v->get_if<uint64_t>()) return *u;
    return std::nullopt;
}

/// A bool member: the Value, `fallback` when it is not there (a bool is always on the wire, so only an absent member).
inline std::optional<bool> bool_or(const Value* v, bool fallback) {
    if (!v) return fallback;
    if (const bool* b = v->get_if<bool>()) return *b;
    return std::nullopt;
}

}}}}  // namespace aoc::protocol::repl::rpcf
