// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/subobject.h
//
//  What surrounds an object's replicated data on an actor channel: the
//  package-map exports that name it and the content block that carries it.
//  Every form here was read off the original server's opens (fixtures/
//  replay_data.bin) with tools/decode_actor_open.py, which reads all 743
//  captured opens and their exports to their last bit.
//
//  Exports (UPackageMapClient::ExportNetGUID, UIntrepidNetServerPackageMap;
//  emit/package_map_exporter.h writes one entry):
//    an export bunch is bHasRepLayoutExport(1) = 0, int32 count, entries.  An
//    entry is the object's 128-bit GUID, FExportFlags (bit 0 path, 1 no-load,
//    2 checksum) and, with a path, its outer's entry, its name (FString) and,
//    flagged, a uint32 checksum; the outermost outer is the null GUID.
//      archetype / class   flags 5 (path + checksum): "Default__PlayerPawn_C",
//                          outer the package "/Game/.../PlayerPawn" flags 5,
//                          checksum 0 (pkt#78, pkt#22)
//      level               flags 7 on all three links: PersistentLevel,
//                          Verra_World_Master, /Game/Levels/.../Verra_World_Master
//      component           flags 7, its name and checksum; the outer is the
//                          actor: its GUID and flags 6 (no-load + checksum, no
//                          path: a dynamic object has none) -- pkt#78, #107
//    A GUID that already went into the same export bunch is written again as
//    its GUID and its flags without the path bit: the second appearance of
//    "/Script/GameSystemsPlugin" (32 times in the capture, e.g. ch 16 pkt#66,
//    a 7289-bit bunch of ten entries).  Bunches are filled entry by entry up to
//    UE's MaxSingleBunchSizeBits (7621 bits with AoC's MaxPacket, the 33-bit
//    header counting): the largest captured export bunch is 7593 bits, and in
//    each of the 9 chains with two the first bunch plus the next entry (7737 to
//    8937 bits) is over the limit.
//    UE writes an export again until the client acknowledged it, per GUID
//    (ShouldSendFullPath, NetGUIDHasBeenAckd): pkt#22 and #78 both carry
//    PersistentLevel, pkt#78 and #107 the same four components, the PlayerState
//    open at pkt#434 no longer does.  ExportAcks is that knowledge.  A GUID that
//    already went into the same bunch is not exported again at all when it is a
//    chain's own first GUID (ShouldSendFullPath: CurrentExportBunch->ExportNetGUIDs
//    holds it; no entry, not even a path-less one, PackageMapClient.cpp:346-352,
//    2532-2536), and an outer of a chain gets its GUID and flags alone.
//
//  Content blocks (UActorChannel::WriteContentBlockPayload / ReadContentBlockHeader):
//    bHasRepLayout(1), bIsActor(1) and, for a subobject, its 128-bit GUID; the
//    server then says how the client finds the object:
//      stably named    bStablyNamed(1) = 1 (a component of the actor's class)
//      dynamic         bStablyNamed = 0, bIsDestroyMessage(1) = 0, the class
//                      GUID (exported), bActorIsOuter(1) = 1 (else the outer's
//                      GUID): the PlayerController's GlobalGMCommands at pkt#22,
//                      the pawn's ResourceStorageComponent at pkt#107
//    then SerializeIntPacked(payload bits) and the payload.  The client's blocks
//    (C>S) carry only the two bits and the subobject's GUID: "only the server
//    can tell clients to create objects".  A subobject the server never gave the
//    client a GUID for (a component worldd does not replicate) is named IN PLACE,
//    as UPackageMapClient writes an unmapped object: the default GUID (ObjectId 1,
//    the rest 0), FExportFlags and, with the path flag, the outer's GUID, the name
//    (FString) and, flagged, the checksum -- Gathering stage 3's trees: the
//    client's 550-bit blocks, ten a second, which dropped the whole bunch.
//    An object with nothing to say at an open still gets a block, bHasRepLayout 0
//    and 0 payload bits (139 bits: pkt#78's OwnerInfo, Backpack, Equipment, ...).
// ============================================================================
#pragma once

#include "protocol/emit/bunch_writer.h"
#include "protocol/emit/intrepid_netguid.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace aoc { namespace protocol { namespace repl {

// ---- exports -----------------------------------------------------------------

/// UE's MaxSingleBunchSizeBits on AoC's connection (worldd's Connection::kMaxSingleBunchBits: MaxPacket
/// 1028 * 8 - 256 - 1 - 308 - 38): what an export bunch, header included, may take.
constexpr size_t kMaxExportBunchBits = 7621;
// -- C Y B . C.O.A --

/// One object of an export chain.
struct ExportLink {
    emit::FIntrepidNetworkGUID guid;
    std::string                name;            // its path element (FString); empty: no path (a dynamic actor)
    bool                       no_load = false;
    bool                       has_checksum = false;   // flagged; the checksum itself follows only a path
    uint32_t                   checksum = 0;

    bool has_path() const { return !name.empty(); }
};

/// The object first, then its outer, outermost last (a package: its outer is the null GUID, written for
/// you).  An asset ("Default__PlayerPawn_C" in its package), a level, a class, or a component whose last link
/// is the actor without a name.
using ExportChain = std::vector<ExportLink>;

/// A component of `actor`: flags 7 with its name and checksum, and the actor (no path, flags 6) as outer.
ExportChain component_export(const emit::FIntrepidNetworkGUID& actor, const emit::FIntrepidNetworkGUID& component,
                             std::string name, uint32_t checksum);

/// The objects whose export the client has acknowledged (UE's NetGUIDAckStatus): UPackageMapClient::ShouldSendFullPath
/// leaves the path out of an export the client has acknowledged and UE sends every other one again.  A chain whose first
/// GUID is acknowledged is not exported at all; an acknowledged outer is written as its GUID and flags.
///
/// When to mark: only once the packet that carried the export bunch is acknowledged, never when the bunch is queued or
/// sent (UPackageMapClient::ReceivedAck / ReceivedNak, PackageMapClient.cpp:2411-2452: an ack marks the bunch's GUIDs
/// acknowledged, a NAK leaves them unacknowledged so the next chain that names them exports them again; the capture
/// agrees: pkt#78's exports went out again at pkt#107).  Even a reliable channel's bunch is not known to have arrived
/// until the ack.  The caller learns of the ack from worldd's Connection and marks the GUIDs of that export bunch
/// (Emission::exported_by_bunch); a NAK marks nothing.  (UE also counts the ack of a later packet: its acks arrive in
/// order and a lost packet is NAKed first.)  A GUID marked stays marked: a new channel for the same connection is the
/// same client.
class ExportAcks {
public:
    bool acked(const emit::FIntrepidNetworkGUID& guid) const { return acked_.count(key(guid)) != 0; }
    void mark(const emit::FIntrepidNetworkGUID& guid) { acked_.insert(key(guid)); }
    void mark(std::span<const emit::FIntrepidNetworkGUID> guids) { for (const auto& g : guids) mark(g); }
    void forget(const emit::FIntrepidNetworkGUID& guid) { acked_.erase(key(guid)); }
    size_t size() const { return acked_.size(); }

private:
    using Key = std::array<uint64_t, 2>;
    static Key key(const emit::FIntrepidNetworkGUID& g) {
        return {g.ObjectId, (static_cast<uint64_t>(g.ServerId) << 32) | g.Randomizer};
    }
    std::set<Key> acked_;
};

struct PackedExports {
    std::vector<emit::BunchWriter>          bunches;    // each a whole export bunch payload
    std::vector<emit::FIntrepidNetworkGUID> exported;   // every GUID written with its path: ExportAcks::mark them once acknowledged
    /// The same GUIDs per bunch, parallel to `bunches`: the ones a bunch's acknowledgement marks.
    std::vector<std::vector<emit::FIntrepidNetworkGUID>> exported_by_bunch;
    bool                                    ok = true;  // false: one chain alone does not fit `max_bits`
};

/// Packs the chains whose first GUID is not acked, and not already in the same bunch (ShouldSendFullPath), into export
/// bunches of at most `max_bits`, in order, starting a new bunch when the next chain does not fit.  Nothing to export
/// gives no bunch.
PackedExports pack_exports(std::span<const ExportChain> chains, const ExportAcks& acks,
                           size_t max_bits = kMaxExportBunchBits);

// ---- content blocks -----------------------------------------------------------

/// A subobject the client names IN PLACE (client -> server): the server never gave it a GUID, so UPackageMapClient writes
/// the default GUID (ObjectId 1) and the object's export instead -- FExportFlags (1 path, 2 no-load, 4 checksum) and, with
/// the path, the outer's GUID, the name and, flagged, the checksum (value.h's InlineObject, the form a parameter takes).
/// Nothing resolves a name here: the block is skipped by its payload bits, and the name says what it was.
struct InPlaceName {
    uint8_t                    flags = 0;
    emit::FIntrepidNetworkGUID outer;       // with the path flag
    std::string                path;        // with the path flag: the object's name in its outer ("XPTracker")
    uint32_t                   checksum = 0;   // with the checksum flag
    bool operator==(const InPlaceName&) const = default;
};

struct BlockHeader {
    bool                       has_rep_layout = false;
    bool                       is_actor = true;         // the channel's actor; else `object`
    emit::FIntrepidNetworkGUID object;
    // server -> client only, for a subobject:
    bool                       stably_named = true;
    emit::FIntrepidNetworkGUID dynamic_class;           // !stably_named
    std::optional<emit::FIntrepidNetworkGUID> outer;    // !stably_named; nullopt: the actor is the outer
    /// client -> server only, for a subobject the client has no GUID for: its name in place; then `object` stays 0.
    std::optional<InPlaceName> in_place;
    bool operator==(const BlockHeader&) const = default;
};

/// The header alone; the server's form (`server_form`) or the client's.  Returns the bits written; 0, writing
/// nothing, when it would name an object by ObjectId 1 (the client's form writes `in_place` instead, when set).
size_t write_block_header(emit::BunchWriter& out, const BlockHeader& header, bool server_form = true);

/// A whole content block: the header, SerializeIntPacked(payload bits), the payload.  Returns the bits written
/// (0 as above).
size_t write_content_block(emit::BunchWriter& out, const BlockHeader& header, const emit::BunchWriter& payload,
                           bool server_form = true);
// Cyb with C | O | A

struct BlockView {
    BlockHeader header;
    size_t      start = 0;         // first bit of the block
    size_t      payload_pos = 0;   // first payload bit
    size_t      payload_bits = 0;
};

/// Reads the block at `pos` (`end` bits in all) and leaves `pos` after its payload.  nullopt (`pos`
/// unchanged) for a destroy message (not supported), a payload past `end` or a read past the end.
std::optional<BlockView> read_content_block(const uint8_t* data, size_t end, size_t& pos, bool server_form = true);

/// Every block up to `end`; nullopt when one does not read.
std::optional<std::vector<BlockView>> read_content_blocks(const uint8_t* data, size_t end, size_t pos,
                                                          bool server_form = true);

}}}  // namespace aoc::protocol::repl
