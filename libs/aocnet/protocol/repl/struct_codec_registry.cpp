// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/struct_codec_registry.cpp
//
//  See struct_codec_registry.h.
// ============================================================================
#include "protocol/repl/struct_codec_registry.h"

#include "protocol/repl/registered_feature_codecs.inc"

#include <functional>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>

namespace aoc { namespace protocol { namespace repl {

namespace {

struct RegistryState {
    std::shared_mutex                                  mutex;
    std::map<std::string, StructCodecOps, std::less<>> codecs;
};

RegistryState& registry() {
    static RegistryState state;
    return state;
}

void ensure_registered_codecs() {
    static std::once_flag once;
    std::call_once(once, []() { register_all_feature_codecs(); });
}

}  // namespace

void register_struct_codec(std::string_view struct_name, StructCodecOps ops) {
    if (struct_name.empty()) {
        throw std::invalid_argument("register_struct_codec: struct name must not be empty");
    }
    if (!ops.accepts || !ops.read || !ops.write) {
        throw std::invalid_argument(
            "register_struct_codec: accepts, read, and write callbacks are required for " +
            std::string(struct_name));
    }
    if (is_builtin_struct(struct_name)) {
        throw std::logic_error(
            "register_struct_codec: struct '" + std::string(struct_name) +
            "' collides with a built-in struct codec");
    }

    RegistryState& state = registry();
    std::unique_lock<std::shared_mutex> lock(state.mutex);
    if (state.codecs.find(struct_name) != state.codecs.end()) {
        throw std::logic_error(
            "register_struct_codec: struct '" + std::string(struct_name) +
            "' is already registered");
    }
    state.codecs.emplace(std::string(struct_name), ops);
}

const StructCodecOps* find_registered_struct_codec(std::string_view struct_name) {
    ensure_registered_codecs();
    RegistryState& state = registry();
    std::shared_lock<std::shared_mutex> lock(state.mutex);
    const auto it = state.codecs.find(struct_name);
    return it == state.codecs.end() ? nullptr : &it->second;
}

}}}  // namespace aoc::protocol::repl
