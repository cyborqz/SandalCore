// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/fast_array.h
//
//  FFastArraySerializer's delta format: the payload of a custom-delta field
//  (UAoCStatsComponent StatRepInt32Owner / StatRepFloatArrayEveryoneProxy /
//  CurrentEffects, UPlayerInfo HeldItems, AAoCGameStateBase's fast maps ...) in
//  the field loop after a RepLayout block.  Derived by decoding the fast arrays
//  of the whole capture to their last bit; the UE 5.6 code it follows is
//  FRepLayout::SendCustomDeltaProperty / ReceiveCustomDeltaProperty
//  (RepLayout.cpp) and FastArrayDeltaSerialize (Net/Core FastArraySerializer.h),
//  classic branch.
//
//  Payload (bits LSB first, integers raw, no packing):
//
//    bSupportsFastArrayDelta   1 bit   the driver's net.SupportFastArrayDelta;
//                                      captured 1 on every array
//    [static array index       SerializeIntPacked, only for an ArrayDim > 1
//                              property: not supported here]
//    ArrayReplicationKey       int32   the array's current key
//    BaseReplicationKey        int32   the key the connection last had (0: the
//                                      default state, on a first send)
//    NumDeletes                int32
//    NumChanged                int32
//    NumDeletes x  ElementID   uint32
//    NumChanged x  ElementID   uint32, then the item
//
//  An item is what FNetSerializeCB::NetSerializeStruct writes for a struct
//  without a native NetDeltaSerialize: a NetSerialize struct through its codec
//  (FStatInt32InstRep), any other struct through
//  FRepLayout::SerializePropertiesForStruct -- every command of the item, in
//  the RepLayout's order, no handles; a dynamic array member is a uint16 count
//  and then every element the same way.  The changed items come in the array's
//  order, the deleted IDs in the order of the previous send (UE's: the captured
//  ones ascend).
//
//  Evidence (tools/ghidra/fast_array_check.py walks every field loop of the
//  capture: 7,849 custom-delta payloads, and all 7,849 read to their last bit).
//  In the form above 7,786 read:
//    - 1,201 need no AoC struct codec: all 842 StatsComponent arrays (the pawn's
//      and the NPC channels': StatRepInt32Owner / OwnerProxy / EveryoneProxy,
//      StatRepFloatArrayEveryoneProxy, CurrentEffects), HeldItems, four GameState
//      maps, the 341 foliage IndexedStates and 13 ability / interactable payloads
//      with no FQuat or FGuidToBoolWrapper in them;
//    - 6,585 need one (protocol/repl/codecs.h): 6,547 of the 6,550
//      UAoCAbilityComponent.ReplicatedActiveAbilities (its items hold a Transform,
//      whose FQuat is a NetSerialize struct) and 38 of the 48 payloads of
//      UInteractableComponent's two GUID-to-bool maps (FGuidToBoolWrapper).
//  The other 63 read too, in a different form; that is not a fault of the one above:
//    - 63 payloads of AAoCGameStateBase.ReplicatedGlobalQuestCache are in the OTHER
//      form.  FFastArraySerializer's constructor asks for struct-delta items and
//      GlobalQuestDataArray keeps that; an item is ElementID, a bAnythingToSend
//      bit and, when set, SerializeIntPacked(handle) + value for each changed
//      command and a closing SerializeIntPacked(0) (DeltaSerializeFastArrayProperty),
//      a new item with every command.  All 63 read to the last bit that way once
//      GeneralQuestMarker, a TOptional<FVector_NetQuantize> (an IsSet bit, then the
//      quantized vector: FOptionalProperty::NetSerializeItem), is read.  That form,
//      and the Optional in the schema (it is PropType::Unknown), are not implemented
//      here: fast_array_layout refuses the array rather than write the wrong form.
//      Which form an array is in is the struct's compiled-in choice, not on the
//      wire.
//
//  Server side (what UE keeps in FFastArraySerializer and, per connection, in
//  FNetFastTArrayBaseState):
//    FastArrayState   the items (ReplicationID, ReplicationKey, values), the
//                     IDCounter and the ArrayReplicationKey, changed through
//                     add / change / remove like MarkItemDirty / MarkArrayDirty
//    FastArrayShadow  the array key and the ID -> key map of the last send
//    write_fast_array_delta(state, shadow) = FastArrayDeltaSerialize's writer:
//                     nothing when the keys agree, else the changed and deleted
//                     items against the shadow
//
//  What the shadow is.  UE's connection state (SendingRepState->
//  RecentCustomDeltaState, DataReplication.cpp ReplicateCustomDeltaProperties)
//  is the state of the last *send*, not of the last acknowledgement: it becomes
//  the new state as soon as a payload is written, and a NAK of that packet puts
//  the state from before the send back (FObjectReplicator::ReceivedNak,
//  Retirement.DynamicState) so the update is rebuilt on the next tick.  It starts
//  as the state of the default object (InitWithObject -> InitRecentProperties
//  serializes the archetype's array against a null old state: key 0, no items --
//  the base key 0 of every first send in the capture).  This shadow is the same: "last sent",
//  starting at key 0 / empty, copyable so a channel that has to survive a NAK can
//  keep the shadow from before each unacknowledged send and put it back.  The
//  capture shows the NAK behaviour: an NPC's CurrentEffects update (ch 21) went
//  out three times, byte for byte, in pkt#556, #882 and #1722, its base key never
//  moving on.  worldd's actor channel is reliable, so it keeps no copies.
//
//  A RepState refuses these properties (rep_state.h) and the RepLayout block
//  skips them; an object's replicator holds a FastArrays next to its RepState,
//  and a FastArrayShadows next to its RepShadow per connection, and calls
//  write_fast_array_fields right after write_rep_block (a content block with no
//  RepLayout block, like the captured StatsComponent's, starts with the fields).
//  Why not a Value kind in RepState: an item's ReplicationID / Key are server
//  state that Value equality and the RepShadow (which keeps copies of what was
//  sent) have no place for, the shadow needs only the ID -> key map, and the
//  payload is a field, not a handle of the RepLayout block.
// ============================================================================
#pragma once

#include "protocol/emit/bunch_writer.h"
#include "protocol/repl/codecs.h"
#include "protocol/repl/rep_block.h"
#include "protocol/repl/rep_state.h"
#include "protocol/schema/generated/schema_types.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace aoc { namespace protocol { namespace repl {

constexpr int32_t kIndexNone = -1;   // UE's INDEX_NONE

/// FFastArraySerializer::MaxNumberOfAllowedChangesPerUpdate / ...DeletionsPerUpdate
/// (net.MaxNumberOfAllowedTArrayChangesPerUpdate / ...DeletionsPerUpdate, UE's
/// defaults; AoC's values are unknown).  A payload with more is an error to the
/// reader: the writer refuses it and the reader here does too.
constexpr int32_t kMaxFastArrayChanged = 2048;
constexpr int32_t kMaxFastArrayDeleted = 2048;

/// How the items of one fast-array field are made: the property's Items array
/// command and its element commands.  Refers into the generated schema tables
/// (static storage), so it is cheap to copy.
struct FastArrayLayout {
    uint32_t                          field = 0;      // the NetField index
    std::span<const schema::RepCommand> element;      // the item's own commands: local handle = index + 1
    std::span<const schema::RepCommand> table;        // the owner's element command table (nested arrays index into it)
    /// A struct whose own NetDeltaSerialize writes its fields between the leading bSupportsFastArrayDelta bit and the counters
    /// (FNodeLayoutAssetSetGuids: the level, culture, season, tags and PlotChangeVersion; node_layout.h): the size in bits of that
    /// header when it starts at bit `pos` of `data` (`end`: the bit it must not run past), nullopt when it does not read.  null: none.
    using PrefixSizeFn = std::optional<size_t> (*)(const uint8_t* data, size_t end, size_t pos);
    PrefixSizeFn                      prefix_size = nullptr;
};

/// The layout of a fast-array field; nullopt for a field that is not a
/// FFastArraySerializer property (or is a static array of them, or has no single
/// replicated array of items, or is one of the struct-delta arrays this file
/// does not write: ReplicatedGlobalQuestCache).
std::optional<FastArrayLayout> fast_array_layout(const schema::ClassSchema& schema, uint32_t field);
std::optional<FastArrayLayout> fast_array_layout(const schema::ClassSchema& schema, std::string_view property_name);

/// True when `values` gives every command of `layout` a value that fits it (a
/// nested array recursively): what an item must hold to be written.
bool item_matches(const FastArrayLayout& layout, const ElementState& values);

// ---- struct members without handles ------------------------------------------------------------------------------
// FRepLayout::SerializePropertiesForStruct's form, which is also how an RPC parameter's array elements go out
// (rpc.h): every command of `cmds` in order, no handles; a dynamic array member is its uint16 count and then each
// element the same way.  `es` holds the values by local handle (index + 1 in `cmds`); `table` is the owner's element
// command table (nested arrays index into it).  Arrays may nest 8 deep.

/// True when `es` gives every command of `cmds` a value that fits it (a nested array recursively).
bool element_fits(std::span<const schema::RepCommand> cmds, std::span<const schema::RepCommand> table,
                  const ElementState& es, int depth = 0);
/// Writes the members; false (partial bits may remain: the caller truncates) for a value that does not fit.
bool write_element(std::span<const schema::RepCommand> cmds, std::span<const schema::RepCommand> table,
                   const ElementState& es, emit::BunchWriter& out, const CodecOptions& opts, int depth = 0);
/// Reads the members; nullopt for a read past the end, a struct without a codec or an unmodelled kind.
std::optional<ElementState> read_element(std::span<const schema::RepCommand> cmds,
                                         std::span<const schema::RepCommand> table, wire::PacketReader& r,
                                         const CodecOptions& opts, int depth = 0);

struct FastArrayItem {
    int32_t      id = kIndexNone;     // ReplicationID
    int32_t      key = kIndexNone;    // ReplicationKey
    ElementState values;              // by the item's local handles
};
// CoA - C:Y:B

/// The server's side of one fast array: FFastArraySerializer.
class FastArrayState {
public:
    explicit FastArrayState(const FastArrayLayout& layout) : layout_(layout) {}

    const FastArrayLayout&            layout() const { return layout_; }
    const std::vector<FastArrayItem>& items() const { return items_; }
    int32_t                           array_key() const { return array_key_; }
    const FastArrayItem*              find(int32_t id) const;

    /// A new item at the end: MarkItemDirty gives it the next ReplicationID
    /// (++IDCounter), bumps its ReplicationKey (from INDEX_NONE to 0) and the
    /// array's key.  nullopt, nothing changed, when `values` do not fit the layout.
    std::optional<int32_t> add(ElementState values);

    /// Replaces an item's values: MarkItemDirty on an existing item.  False when
    /// there is no such ID or the values do not fit.
    bool change(int32_t id, ElementState values);

    /// Removes an item, keeping the others in order, and marks the array dirty
    /// (Items.RemoveAt + MarkArrayDirty).  False when there is no such ID.
    bool remove(int32_t id);

    /// MarkArrayDirty: ArrayReplicationKey++ (skipping INDEX_NONE).  The
    /// original server was seen to call it more than once per change.
    void mark_array_dirty();

    /// Loading an array (a saved one, or one rebuilt from a payload): an item
    /// with a given ID, added without touching the keys; IDCounter follows it.
    /// False for an ID that is INDEX_NONE or taken, or values that do not fit.
    /// Nothing is replicated by an insert alone: it leaves ArrayReplicationKey
    /// where it was, and a connection whose shadow has that key sends nothing.
    /// After inserting into an array clients already have, call mark_array_dirty
    /// (or set_array_key) so the next update carries the items (the item's own
    /// key starts at 0, so a connection that never had the ID gets it as new).
    bool insert(int32_t id, ElementState values);
    /// Sets ArrayReplicationKey (loading).
    void set_array_key(int32_t key) { array_key_ = key; }

    /// The header of a layout with a `prefix_size` (FastArrayLayout): `bits` bits of `bytes`, LSB first.  True when it differs from what was set before; the
    /// caller then marks the array dirty (a header is sent only with the counters, which a connection that has the array's key never gets).
    bool set_prefix(const std::vector<uint8_t>& bytes, size_t bits);
    const std::vector<uint8_t>& prefix() const { return prefix_; }
    size_t                      prefix_bits() const { return prefix_bits_; }

private:
    FastArrayLayout            layout_;
    std::vector<FastArrayItem> items_;
    int32_t                    id_counter_ = 0;
    int32_t                    array_key_ = 0;
    std::vector<uint8_t>       prefix_;
    size_t                     prefix_bits_ = 0;
};

/// What the connection has of one array: FNetFastTArrayBaseState.
struct FastArrayShadow {
    /// ArrayReplicationKey of the last send.  0 is the default state (an empty
    /// array with key 0) that every first send in the capture is a delta against
    /// (BaseReplicationKey 0), and it is why an array nobody touched sends nothing.
    int32_t array_key = 0;
    /// ReplicationID -> ReplicationKey of the items in the last send, in the order
    /// of that send's array (UE builds the map by adding the items in order, and
    /// iterates it in insertion order to list the deletions).
    std::vector<std::pair<int32_t, int32_t>> items;
    /// A payload that was lost (SendLedger, send_ledger.h): the array is sent even though its key equals the shadow's, the items the ledger
    /// gave kUnknownKey go out with their current values (or as deletions when the array no longer has them).  Cleared by the next write.
    bool resend = false;

    void clear() { array_key = 0; items.clear(); resend = false; }
};

/// The item key a NAK puts in a shadow for an item the client may or may not have as the shadow says: it equals no real ReplicationKey, so the
/// next write sends the item's current state, or its deletion when the array has lost it.
constexpr int32_t kUnknownKey = INT32_MIN;

/// FastArrayDeltaSerialize's writer.  Appends the payload of the field and
/// returns its bits; 0, writing nothing, when the array key equals the shadow's
/// (nothing changed).  Otherwise it always writes a header, even with no item
/// changed or deleted (UE sends it so the client can look for implicit
/// deletes), then the items that are new or whose key differs from the shadow,
/// in array order, and the IDs the shadow has that the array no longer does.
/// Updates the shadow.  Also 0, with `out` and `shadow` unchanged and *failed
/// true, when an item cannot be carried (a value the codecs refuse, an item that
/// does not fit the layout) or more than kMaxFastArrayChanged / Deleted change.
size_t write_fast_array_delta(const FastArrayState& state, FastArrayShadow& shadow, emit::BunchWriter& out,
                              const CodecOptions& opts = {}, bool* failed = nullptr);

/// A payload as read.
struct FastArrayDelta {
    bool     supports_delta = true;   // the leading bit
    std::vector<uint8_t> prefix;      // FastArrayLayout::prefix_size: the struct's own header bits (LSB first), empty for a layout with none
    size_t   prefix_bits = 0;
    int32_t  array_key = 0;
    int32_t  base_key = 0;
    std::vector<int32_t> deleted;
    struct Changed { int32_t id = 0; ElementState values; };
    std::vector<Changed> changed;
    bool operator==(const FastArrayDelta&) const;
};

/// The reader: one field payload from bit `pos` of `data` (`end` is the bit
/// the payload must not run past, the field header's bit count added to `pos`).
/// Advances `pos` past the payload.  nullopt (`pos` unchanged) for a read past
/// the end, more than kMaxFastArrayChanged / Deleted, or an item it cannot read
/// (a struct without a codec, an unmodelled property kind).  It does not check
/// that the payload ends at `end`: read_fast_array_fields does.
std::optional<FastArrayDelta> read_fast_array_delta(const FastArrayLayout& layout, const uint8_t* data, size_t end,
                                                    size_t& pos, const CodecOptions& opts = {});

/// The client's application of a delta to its copy: the deleted IDs are
/// removed, the changed items replaced or appended.  The array key becomes the
/// delta's.  (Used to rebuild a state from a payload; the item keys of a
/// rebuilt state are 0.)  False, state unchanged, when an item does not fit.
bool apply_fast_array_delta(FastArrayState& state, const FastArrayDelta& delta);

// ---- an object's fast arrays --------------------------------------------------

/// The fast arrays of one replicated object by field index.
class FastArrays {
public:
    /// The array of a fast-array field, created empty on first use; nullptr for
    /// a field that is not one.
    FastArrayState* get(const schema::ClassSchema& schema, uint32_t field);
    FastArrayState* get(const schema::ClassSchema& schema, std::string_view property_name);
    const FastArrayState* find(uint32_t field) const;

    const std::map<uint32_t, FastArrayState>& all() const { return arrays_; }

private:
    std::map<uint32_t, FastArrayState> arrays_;
};

/// One connection's shadows of an object's fast arrays.
struct FastArrayShadows {
    std::map<uint32_t, FastArrayShadow> by_field;
    void clear() { by_field.clear(); }
};

/// The shadows write_fast_array_fields replaced, to put back when a later write of the same update fails.
struct FastArrayShadowsUndo {
    std::vector<std::pair<uint32_t, std::optional<FastArrayShadow>>> entries;   // field, what it held (nullopt: none)

    /// Restores `shadows` to its state before the writes recorded here.
    void apply(FastArrayShadows& shadows) const;
};

/// Writes the field of every array that changed, ascending by field: the field
/// header (write_field) and the payload.  Returns the bits appended (0 when
/// nothing changed, without copying a shadow: an array whose key the shadow has
/// is skipped first).  All or nothing: on a failure (*failed true) `out` and the
/// shadows are as they were.
///
/// `active` (rep_block.h ParentMask): an array whose property's parent is inactive
/// is left out, its shadow untouched, so it is sent in full or as a delta once the
/// parent is active.  `undo`: receives the shadows the write replaced, when it wrote.
size_t write_fast_array_fields(const schema::ClassSchema& schema, const FastArrays& arrays, FastArrayShadows& shadows,
                               emit::BunchWriter& out, const CodecOptions& opts = {}, bool* failed = nullptr,
                               const ParentMask* active = nullptr, FastArrayShadowsUndo* undo = nullptr);

struct FastArrayField {
    uint32_t        field = 0;
    FastArrayDelta  delta;
};

/// Reads the field loop from `pos` up to bit `end`, every field of which must
/// be a fast array and its payload must end exactly where its header says.
/// Advances `pos` to `end`.  nullopt (`pos` unchanged) for anything else,
/// an RPC field included.
std::optional<std::vector<FastArrayField>> read_fast_array_fields(const schema::ClassSchema& schema, const uint8_t* data,
                                                                  size_t end, size_t& pos, const CodecOptions& opts = {});

}}}  // namespace aoc::protocol::repl
