// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/rep_state.h
//
//  RepState: the replicated values of one object (an actor or a component),
//  keyed by RepLayout handle.  A value is "set" once the server has given it
//  one; only set values are sent (the first block carries every set handle,
//  later blocks the ones a connection's shadow says changed: rep_block.h).
//
//  set() is checked against the class schema (schema/generated): the path (or
//  handle) must name a command, the value's kind must be the one the command's
//  PropType carries and fit its width, and a NetSerialize struct needs a codec
//  (codecs.h) and a value of its type.  Properties of custom-delta structs
//  (fast arrays) are not part of the RepLayout block and are refused here.
//
//  What set() accepts is what the writer writes: a string without a NUL and of at
//  most 65,535 characters, a quantized component of at most 63 bits (also inside
//  an FRepMovement), an FUniqueNetIdRepl of a non-V2 type with at most 255 id
//  bytes (codecs.h struct_accepts).  A value the writer refuses would stall the
//  object's updates for good, so it is refused here, where the caller that built
//  it can see.  The one write-time failure left is FRepMovement's rotation, whose
//  width depends on the object (CodecOptions::rep_movement_short_rotation).
// ============================================================================
#pragma once

#include "protocol/repl/value.h"
#include "protocol/schema/generated/schema_types.h"

#include <cstdint>
#include <map>
#include <span>
#include <string_view>

namespace aoc { namespace protocol { namespace repl {
// ~ C.O.A & C:Y:B ~

/// True when `v` is a value command `cmd` can carry and write_value writes: the kind of its PropType,
/// within its width, strings and structs as struct_accepts says, recursively for the elements of a
/// dynamic array (`element_commands` is the owner's element command table).
bool value_matches(const schema::RepCommand& cmd, std::span<const schema::RepCommand> element_commands, const Value& v);

class RepState {
public:
    /// Sets the command named by `path` ("bHidden", "ReplicatedMovement",
    /// "AttachmentReplication.AttachParent"); false, leaving the state
    /// unchanged, when the path names no command or `v` does not fit it.
    bool set(const schema::ClassSchema& schema, std::string_view path, Value v);
    bool set(const schema::ClassSchema& schema, uint32_t handle, Value v);

    /// Forgets a value (plan E1: a template's value an instance leaves unset, say an NPC's Instigator): nothing of the handle is sent to a
    /// connection that never had it.  False when the handle was not set.  (A connection that already holds it keeps what it has: UE has no
    /// "unset" either.)
    bool erase(uint32_t handle) { return values_.erase(static_cast<uint16_t>(handle)) != 0; }

    const Value* get(uint32_t handle) const;
    const Value* get(const schema::ClassSchema& schema, std::string_view path) const;

    /// Every set value by ascending handle.
    const std::map<uint16_t, Value>& values() const { return values_; }
    bool empty() const { return values_.empty(); }

    bool operator==(const RepState& o) const { return values_ == o.values_; }

private:
    std::map<uint16_t, Value> values_;
};

}}}  // namespace aoc::protocol::repl
