// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/node_layout.h  (Nodes 1, stage 3a)
//
//  The town's scenery on the wire: ANodeLayoutReplicator.LayoutAssetSetGuids, a FNodeLayoutAssetSetGuids.  It is a fast array (FFastArraySerializer) whose own
//  NetDeltaSerialize (exe 0x145b7e440) writes a header between the leading bSupportsFastArrayDelta bit and the stock counters.  The payload, decoded from 92 retail
//  payloads to their last bit (research/spike_replicator/decode.md):
//
//    bit    bSupportsFastArrayDelta   1
//    u8     NodeLevel                 ENodeLevel  0 Wilderness .. 6 Metropolis
//    u8     NodeCulture               ECharacterRace (0 Kaelar)
//    u8     NodeSeason                ESeason  0 Winter 1 Spring 2 Summer 3 Fall
//    tags   NodeGameplayTags          FGameplayTagContainer::NetSerialize: bit IsEmpty; else 6 bits of count and per tag a bit bHardcoded (0), the name as an FString
//                                     (int32 length with the NUL, the bytes) and the FName number (int32, 0).  Retail's is always War.Siege.States.SiegeInactive
//    u8     PlotChangeVersion
//    i32    ArrayReplicationKey   i32 BaseReplicationKey   i32 NumDeletes   i32 NumChanged       (the stock body, fast_array.h)
//    u32 x  deleted ReplicationIDs
//    x      { u32 ReplicationID, item }
//    item   i64 Value.OwnerGuid, i32 Value.OwnerSecondaryId, i32 Value.OwnerArrayIndex, i64 Value.AssetSetRecordGuid, then TransformOffset: Rotation (FQuat::NetSerialize:
//           X, Y, Z as doubles), Translation and Scale3D (three doubles each): 768 bits, no quantisation
//
//  The header rides on the generic fast-array machinery as the layout's `prefix_size` (fast_array.h): the FastArrayState holds it as bits (set_prefix), the writer puts it
//  after the leading bit, the reader keeps it in FastArrayDelta::prefix.  So a replicator actor's LayoutAssetSetGuids is an ordinary fast array of its object (the actor
//  view sends it in an open, in an update, and again after a lost packet), and the typed side below only makes and reads the header and the items.
//
//  What an item is (the builder is worldd's world/node_layout.h): a layout item (OwnerGuid the node, OwnerArrayIndex the prop's index in the layout), or a plot item
//  (OwnerGuid the service building plot, OwnerSecondaryId the EBuildingState, OwnerArrayIndex the position in the building's asset sets of that state).
//
//  sync_node_layout brings a replicator's array to a header and an item list.  A change sends the new FULL list: every item the array had is removed and every item of the
//  new list added with a new ReplicationID (retail only ever shows full snapshots, and the client's OnRep re-filters all of them), so a client that holds the old list gets
//  one delta that deletes it and adds the new.  The first sync of an empty array is IDs 1..N and ArrayReplicationKey N, which is what a fresh server sends.
// ============================================================================
#pragma once

#include "protocol/emit/bunch_writer.h"
#include "protocol/repl/fast_array.h"
#include "protocol/repl/value.h"
#include "protocol/schema/generated/schema_types.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aoc { namespace protocol { namespace repl {

inline constexpr const char* kNodeLayoutClass = "ANodeLayoutReplicator";
inline constexpr const char* kNodeLayoutProperty = "LayoutAssetSetGuids";
inline constexpr int kNodeLayoutTagCountBits = 6;      // FGameplayTagContainer::NetSerialize: NumBitsForContainerSize

/// The header FNodeLayoutAssetSetGuids::NetDeltaSerialize writes.
struct NodeLayoutHeader {
    uint8_t                  level = 0;           // ENodeLevel
    uint8_t                  culture = 0;         // ECharacterRace
    uint8_t                  season = 0;          // ESeason
    std::vector<std::string> tags;                // NodeGameplayTags
    uint8_t                  plot_version = 0;    // PlotChangeVersion
    bool operator==(const NodeLayoutHeader&) const = default;
};

/// One item of the array (FNodeLayoutAssetSetItem): an asset set at a place, owned by a layout prop or a plot's building.
struct NodeLayoutItem {
    int64_t owner_guid = 0;        // the node (a layout prop) or the ServiceBuildingPlot (a building)
    int32_t owner_secondary = 0;   // 0 for a prop; the EBuildingState for a plot
    int32_t owner_index = 0;       // the prop's index in the layout / the position in the building's asset sets of that state
    int64_t asset_set = 0;         // NodeAssetSet
    Quat    rotation;              // the quaternion's X, Y, Z as the wire holds them (W = sqrt(1 - |xyz|^2) >= 0)
    Vec3d   translation;           // cm, node-local
    Vec3d   scale{1, 1, 1};
    bool operator==(const NodeLayoutItem&) const = default;
};
// [Cyb + C O A]

/// A whole payload, typed.
struct NodeLayoutPayload {
    NodeLayoutHeader header;
    int32_t          array_key = 0;
    int32_t          base_key = 0;
    std::vector<int32_t> deleted;
    struct Entry {
        int32_t        id = 0;     // ReplicationID
        NodeLayoutItem item;
        bool operator==(const Entry&) const = default;
    };
    std::vector<Entry> items;
    bool operator==(const NodeLayoutPayload&) const = default;
};

// ---- the header ---------------------------------------------------------------------------------------------------------------------------------

/// Writes the header (the bits after the leading bit, before the counters).  False, with `out` as it was, for more tags than the 6-bit count holds, a tag that is empty,
/// longer than 255 characters or not ASCII.
bool write_node_layout_header(const NodeLayoutHeader& header, emit::BunchWriter& out);

/// FastArrayLayout::prefix_size for this array: the size of the header at bit `pos` (nullopt: it does not read, or runs past `end`).
std::optional<size_t> node_layout_header_bits(const uint8_t* data, size_t end, size_t pos);

/// Reads the header at bit `pos` of `data` (`end`: the bit it must not run past).  Advances `pos`.  nullopt (pos unchanged) when it does not read.
std::optional<NodeLayoutHeader> read_node_layout_header(const uint8_t* data, size_t end, size_t& pos);

// ---- the items -----------------------------------------------------------------------------------------------------------------------------------

/// The item as the array's element values (the schema's seven element commands, local handles 1 to 7).
ElementState node_layout_item_values(const NodeLayoutItem& item);
/// The item of element values (nullopt when a member is missing or of the wrong kind).
std::optional<NodeLayoutItem> node_layout_item_of(const ElementState& values);

/// True when the schema is the game's ANodeLayoutReplicator: the property is a fast array of the seven-member item, with the header.
bool node_layout_schema_ok(const schema::ClassSchema& replicator);

// ---- whole payloads --------------------------------------------------------------------------------------------------------------------------------

/// Reads a field payload (the bits of LayoutAssetSetGuids after the field header) to its last bit.  nullopt when it does not read.
std::optional<NodeLayoutPayload> read_node_layout_payload(const schema::ClassSchema& replicator, const uint8_t* data, size_t bits);

/// Writes a payload as a full snapshot (BaseReplicationKey 0, no deletes: what a connection with an empty shadow is sent), keeping the payload's ids and keys.  Returns the
/// bits written; 0 (`out` as it was) when the payload has a base key or deletes, does not fit the schema, or the header does not write.
size_t write_node_layout_snapshot(const schema::ClassSchema& replicator, const NodeLayoutPayload& payload, emit::BunchWriter& out);

// ---- the server's state ------------------------------------------------------------------------------------------------------------------------------

struct NodeLayoutSync {
    bool   changed = false;         // the array was changed (a new key)
    size_t removed = 0, added = 0;  // items
    bool   header_changed = false;
};

/// Brings the LayoutAssetSetGuids of `arrays` (a replicator's fast arrays) to `header` and `items`: nothing when it already holds exactly that; else every item it had is
/// removed, the new list is added (new ReplicationIDs, in order), the header is set and the array's key moves.  False, with the array as it was, when the schema is not the
/// replicator's, an item does not fit or the header does not write.
bool sync_node_layout(const schema::ClassSchema& replicator, FastArrays& arrays, const NodeLayoutHeader& header, const std::vector<NodeLayoutItem>& items,
                      NodeLayoutSync* result = nullptr);

/// What the array holds now (nullopt when it has none, or its header does not read).  base_key and deleted are empty: it is the state, not a delta.
std::optional<NodeLayoutPayload> read_node_layout_state(const schema::ClassSchema& replicator, const FastArrays& arrays);

}}}  // namespace aoc::protocol::repl
