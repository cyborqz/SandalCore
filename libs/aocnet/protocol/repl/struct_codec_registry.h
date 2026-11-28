// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/struct_codec_registry.h
//
//  Thread-safe, pointer-stable registry for self-registering NetSerialize
//  struct codecs.  New structs live in their own files under
//  protocol/repl/codecs/<feature>_codec.{h,cpp} and register here via
//  register_<feature>_codecs() (auto-discovered by libs/aocnet/CMakeLists.txt),
//  avoiding edits to value.h or codecs.cpp.
// ============================================================================
#pragma once

#include "protocol/repl/codecs.h"

#include <optional>
#include <string_view>

namespace aoc { namespace protocol { namespace repl {

/// Callbacks for one self-registered NetSerialize struct codec.
///
/// `accepts`, `read`, and `write` are required (non-null).  `zero` is optional:
/// leave it nullptr when the struct's wire zero form is not verified, in which
/// case zero_struct() returns std::nullopt.
struct StructCodecOps {
    bool (*accepts)(const Value&) = nullptr;
    std::optional<Value> (*zero)() = nullptr;
    std::optional<Value> (*read)(wire::PacketReader&, const CodecOptions&) = nullptr;
    bool (*write)(const Value&, emit::BunchWriter&, const CodecOptions&) = nullptr;
};

/// Registers a NetSerialize struct codec under `struct_name` (e.g. "FMyStruct").
///
/// Thread-safe and monotonic (entries are never erased, so pointers returned
/// by find_registered_struct_codec remain valid for the process lifetime).
/// Throws std::invalid_argument if `struct_name` is empty or any required
/// callback (`accepts`, `read`, `write`) is null.
/// Throws std::logic_error if `struct_name` collides with a built-in struct
/// or an already-registered custom struct.
void register_struct_codec(std::string_view struct_name, StructCodecOps ops);

/// Looks up a registered struct codec by name, ensuring all auto-discovered
/// feature codecs under protocol/repl/codecs/ have been registered first.
/// Returns nullptr when `struct_name` is not registered.
const StructCodecOps* find_registered_struct_codec(std::string_view struct_name);

/// Internal helper defined in codecs.cpp so register_struct_codec can reject
/// collisions with built-in struct names without duplicating the built-in table.
bool is_builtin_struct(std::string_view struct_name);

}}}  // namespace aoc::protocol::repl
