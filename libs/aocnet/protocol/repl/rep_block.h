// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/rep_block.h
//
//  RepLayout blocks from a RepState (FRepLayout::SendProperties) and back
//  (ReceiveProperties, 0x1444e4a40), in the form the original server sent:
//
//    bDoChecksum (1 bit, 0)
//    { SerializeIntPacked(handle), value }   ascending handles
//    SerializeIntPacked(0)
//
//  A dynamic array's value is uint16 count, then { SerializeIntPacked(
//  element * HandlesPerElement + local handle), value } and a closing 0, the
//  element handles ascending (an element command's local handle is its
//  1-based position in the array's element commands).
//
//  write_rep_block sends what the connection's RepShadow says the client does
//  not have: every set handle on the first send, later the handles whose value
//  changed.  An array is re-sent when its count or an element value changed;
//  like UE's array changelist it lists only the element values that differ
//  (the client keeps the rest, RepLayout ReceiveProperties resizes in place).
//  The capture shows no array update, so that part is not proven against it;
//  the first send is: elements the server never set are absent (the pawn's
//  ResourceContainers, three elements, sends element 0 alone).
//
//  What an absent element value means.  The client resizes an array in place
//  (ReceiveProperties, RepLayout.cpp:3548): the elements it had keep their values
//  and a new one is default-constructed, zeroed.  So a value absent from a new
//  element is the default, which is why the captured first send leaves out what
//  the server never set.  UE's server has no "unset" either: a property reset to
//  its default is a change like any other.  Here too: an element value that was
//  sent and is absent now is sent as the type's zero value (a null object, 0, "",
//  None, an empty array; zero_value below) unless the client already has that,
//  else the client would keep the stale one.  (The capture shows no such reset.)
//  A type whose zero is not known (FText, the structs codecs.h zero_struct does
//  not list) cannot be reset that way: the write fails (*failed) and the caller
//  sets a value instead of removing it.  An empty array on a first send goes out
//  as count 0, which the client already has (UE leaves a default-empty array
//  out); that costs a few bits and is harmless.
//
//  Reading is bounded: a block may build at most kMaxBlockElements array
//  elements in all (nested arrays included), however large their counts say.
//
//  Custom-delta properties (fast arrays, struct NetDeltaSerialize) are not in
//  the block; they travel in the field loop after it, whose header this file
//  also writes and reads: SerializeInt(field, NumFields + 1) +
//  SerializeIntPacked(payload bits), then the payload.
// ============================================================================
#pragma once

#include "protocol/emit/bunch_writer.h"
#include "protocol/repl/codecs.h"
#include "protocol/repl/rep_state.h"
#include "protocol/schema/generated/schema_types.h"
// [C O A :: C.Y.B]

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace aoc { namespace protocol { namespace repl {

/// The last value sent per handle for one object on one connection.
struct RepShadow {
    std::map<uint16_t, Value> sent;

    bool empty() const { return sent.empty(); }
    void clear() { sent.clear(); }
};

/// Which of an object's replicated properties one connection gets: UE evaluates every property's replication condition
/// (COND_OwnerOnly, SkipOwner, SimulatedOnly, AutonomousOnly, InitialOnly ...) per connection, from the connection's
/// relation to the object, and leaves out the properties whose condition is false (FRepLayout::RebuildConditionalProperties,
/// RepLayout.cpp:7225-7250; DataReplication.cpp:1655-1672 for a custom-delta property).  The mask is that outcome: the
/// caller computes it from the connection's role and the writers skip the inactive parents.  A parent is a top-level
/// property: RepCommand::parent of its commands (a fast array's field through NetField::handle).  Everything is active
/// until the caller says otherwise.
///
/// An inactive parent is left out and its shadow stays as it was, so when it becomes active whatever differs from the
/// shadow is sent: the whole value when it was never sent, else what changed since.  The table that says which property
/// has which condition is not built here (it comes from the exe's GetLifetimeReplicatedProps: plan A5); the caller
/// decides per property.
class ParentMask {
public:
    bool active(uint32_t parent) const { return inactive_.count(parent) == 0; }
    void set_active(uint32_t parent, bool on) {
        if (on) inactive_.erase(parent);
        else inactive_.insert(parent);
    }
    /// The property `path` names ("ReplicatedMovement", "AttachmentReplication.AttachParent"); false for a path that
    /// names no command.
    bool set_active(const schema::ClassSchema& schema, std::string_view path, bool on) {
        const schema::RepCommand* cmd = schema.command(path);
        if (!cmd) return false;
        set_active(cmd->parent, on);
        return true;
    }
    bool all_active() const { return inactive_.empty(); }
    void reset() { inactive_.clear(); }

private:
    std::set<uint32_t> inactive_;
};

/// The shadow entries write_rep_block overwrote, to put back when a later write of the same update fails (the
/// replicator's all-or-nothing update, without a copy of the whole shadow).
struct RepShadowUndo {
    std::vector<std::pair<uint16_t, std::optional<Value>>> entries;   // handle, what it held (nullopt: no entry)

    /// Restores `shadow` to its state before the writes recorded here.
    void apply(RepShadow& shadow) const;
};

/// A block may build at most this many array elements in all, nested ones included (an element costs about 250 bytes:
/// a std::map per element), however large the counts of a hostile block say.  read_rep_block and read_value refuse more.
constexpr size_t kMaxBlockElements = 16384;

/// Appends the RepLayout block of every handle of `now` whose value differs
/// from `shadow` (all set handles when the shadow is empty), updates the
/// shadow and returns the bits written.  Writes nothing and returns 0 when
/// nothing changed.  Also 0, with `out` and `shadow` unchanged and *failed
/// true, when a value cannot be carried (a rotation component beyond the
/// FRepMovement quantization, a custom-delta or unknown handle, a removed array
/// element value without a known zero).
///
/// `active` (ParentMask): the handles of an inactive parent are left out (not sent, not recorded in the shadow).
/// `undo`: receives the shadow entries the write overwrote, when it wrote.
size_t write_rep_block(const schema::ClassSchema& schema, const RepState& now, RepShadow& shadow,
                       emit::BunchWriter& out, const CodecOptions& opts = {}, bool* failed = nullptr,
                       const ParentMask* active = nullptr, RepShadowUndo* undo = nullptr);

/// Reads the RepLayout block starting at bit `pos` of `data` (`bits` bits in
/// all) into a RepState and advances `pos` past its closing handle 0.  nullopt
/// (`pos` unchanged) for a set bDoChecksum, a handle that names no command, is
/// not ascending or is custom-delta, a struct without a codec, an array element
/// handle beyond the array's count or not ascending, more than kMaxBlockElements
/// array elements, or a read past the end.
std::optional<RepState> read_rep_block(const schema::ClassSchema& schema, const uint8_t* data, size_t bits,
                                       size_t& pos, const CodecOptions& opts = {});

/// One value of `cmd` (an array with its element handles).  `element_commands`
/// is the owner's element command table (ClassSchema::element_commands,
/// RpcSchema::element_commands).  An array is bounded by kMaxBlockElements.
std::optional<Value> read_value(const schema::RepCommand& cmd, std::span<const schema::RepCommand> element_commands,
                                wire::PacketReader& r, const CodecOptions& opts = {});

/// Writes one value of `cmd`; `sent` is what the client already has of it (an
/// array then lists only its differing element values).  False for a value
/// the command cannot carry (partial bits may remain: the caller truncates).
bool write_value(const schema::RepCommand& cmd, std::span<const schema::RepCommand> element_commands, const Value& v,
                 emit::BunchWriter& out, const CodecOptions& opts = {}, const Value* sent = nullptr);

/// What the client holds of `cmd` when nothing was ever sent for it: zeroed memory.  nullopt for an FText, a struct
/// whose zero is not known (codecs.h zero_struct) and a flattened struct.
std::optional<Value> zero_value(const schema::RepCommand& cmd);

// ---- the field loop ---------------------------------------------------------

struct FieldHeader {
    uint32_t field = 0;
    size_t   payload_bits = 0;
};

/// SerializeInt(field, NumFields + 1) + SerializeIntPacked(payload_bits);
/// false (nothing written) when `field` is not a field of the class.
bool write_field_header(const schema::ClassSchema& schema, uint32_t field, size_t payload_bits, emit::BunchWriter& out);

/// A header and its payload bits; returns the bits written (0 for a bad field).
size_t write_field(const schema::ClassSchema& schema, uint32_t field, const emit::BunchWriter& payload,
                   emit::BunchWriter& out);

/// Reads a field header at `pos`, leaving `pos` at the payload's first bit.
/// nullopt (`pos` unchanged) for a field beyond the class or a payload that
/// runs past `bits`.
std::optional<FieldHeader> read_field_header(const schema::ClassSchema& schema, const uint8_t* data, size_t bits,
                                             size_t& pos);

}}}  // namespace aoc::protocol::repl
