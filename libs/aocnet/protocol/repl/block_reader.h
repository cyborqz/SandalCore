// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/block_reader.h  (plan E1)
//
//  The other direction of a content block: the payload of one block (a RepLayout block when the block has one, then the field loop of its fast
//  arrays) read into the ObjectState the replicator writes from.  An actor a server replicates from data it did not compute (an NPC the retail
//  server opened, kept as the bits it sent) is loaded through this: the state, its fast arrays and the codec settings that made the bits read.
//
//  The settings the bits do not announce are tried in the order schema_check.py's ATTEMPT_OPTS has them (FRepMovement's rotation in bytes, then
//  in shorts; a customization whose preset the client does not know, then one it does) and the first that reads the payload to its last bit
//  is kept in `ObjectState::opts`, so the state writes back to the same bits.  The client's stat table is not on the wire either:
//  `stat_int32_extended` marks the Records whose items carry Base and EquipmentValue (the captured pawn's four, and no NPC's).
//
//  An RPC field in the loop (the shared multicast RPC of an update) is not an object's state: the block is refused.
// ============================================================================
#pragma once

#include "protocol/repl/replicator.h"
#include "protocol/schema/generated/schema_types.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace aoc { namespace protocol { namespace repl {

/// What `bits` bits at `data` say of an object of class `schema`: its RepLayout values (when `has_rep_layout`) and its fast arrays.  nullopt,
/// with `why` (when given) saying what stopped the read, for a payload that does not read to its last bit under any of the settings, a field
/// that is an RPC, or a property field that is no fast array.
std::optional<ObjectState> load_object_state(const schema::ClassSchema& schema, const uint8_t* data, size_t bits, bool has_rep_layout,
                                             std::string* why = nullptr);

/// The same from a payload held as hex (LSB first, as the generators write them).
std::optional<ObjectState> load_object_state_hex(const schema::ClassSchema& schema, const char* hex, size_t bits, bool has_rep_layout,
                                                 std::string* why = nullptr);

/// The settings load_object_state tries first: no FRepMovement short rotation, the preset unknown, the pawn's extended stats.
CodecOptions default_block_options();

}}}  // namespace aoc::protocol::repl
