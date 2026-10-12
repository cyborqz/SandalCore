// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/item_rpc.cpp
//
//  See item_rpc.h.
// ============================================================================
#include "protocol/repl/item_rpc.h"

#include <string_view>

namespace aoc { namespace protocol { namespace repl {

namespace {

const Value* param(const RpcCall& call, std::string_view path) {
    const schema::RepCommand* c = rpc_command(*call.rpc, path);
    if (!c) return nullptr;
    const auto it = call.params.values.find(c->handle);
    return it == call.params.values.end() ? nullptr : &it->second;
}

/// An integer parameter of either signedness; `ok` goes false for a value of another kind.  `fallback` when it was not sent.
int64_t integer(const RpcCall& call, std::string_view path, int64_t fallback, bool& ok) {
    const Value* v = param(call, path);
    if (!v) return fallback;
    if (const int64_t* i = v->get_if<int64_t>()) return *i;
    if (const uint64_t* u = v->get_if<uint64_t>()) return static_cast<int64_t>(*u);
    ok = false;
    return fallback;
}

std::optional<ItemUse> parse(const RpcCall& call, std::string_view name) {
    if (!call.rpc || !call.decoded || name != call.rpc->name) return std::nullopt;
    bool ok = true;
    ItemUse u;
    const int64_t storage = integer(call, "Storage", 0, ok);
    const int64_t slot = integer(call, "Slot", 0, ok);
    const int64_t a = integer(call, "ExpectedItem.A", 0, ok);
    const int64_t b = integer(call, "ExpectedItem.B", 0, ok);
    const int64_t c = integer(call, "ExpectedItem.C", 0, ok);
    const int64_t d = integer(call, "ExpectedItem.D", 0, ok);
    const int64_t equip = integer(call, "RequestedEquipSlot", 0, ok);
    if (!ok || storage < 0 || storage > 255 || slot < INT32_MIN || slot > INT32_MAX) return std::nullopt;
    u.storage = static_cast<uint8_t>(storage);
    u.slot = static_cast<int32_t>(slot);
    u.expected_item = GuidValue{static_cast<uint32_t>(a), static_cast<uint32_t>(b), static_cast<uint32_t>(c), static_cast<uint32_t>(d)};
    u.requested_equip_slot = static_cast<uint32_t>(equip);
    return u;
}

}  // namespace

std::optional<ItemUse> parse_item_used(const RpcCall& call) { return parse(call, "ServerItemUsed"); }

std::optional<ItemUse> parse_activate_item(const RpcCall& call) { return parse(call, "ServerActivateItem"); }

}}}  // namespace aoc::protocol::repl
