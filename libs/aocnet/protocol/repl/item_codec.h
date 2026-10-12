// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/item_codec.h
//
//  The NetSerialize codecs of AoC's item structs (plan B2): FItemInstance, the
//  StorageEventContainer and FItemStorageEvent that carry it.  codecs.h's
//  read_struct / write_struct reach them by struct name ("FItemInstance",
//  "FItemStorageEvent"), which is how an RPC parameter or a RepLayout command
//  uses them; the functions here are the same codecs for a caller that holds a
//  reader.
//
//  Every layout is the client exe's own (AOCClient-Win64-Shipping.exe, read-only),
//  in the order the code runs.  Bit stream, LSB first; Serialize(p, n) is n * 8
//  raw bits, a UBOOL a bool as 32 bits (a value above 1 is an archive error),
//  "packed" SerializeIntPacked (7 bits a byte, bit 0 "more follows").
//
//  FItemInstance::NetSerialize 0x146540110 (helpers named below):
//
//    1        bHasItem          0: nothing else follows, the empty item
//    64       ItemRecordId
//    packed   Quantity, Quality, EnchantmentLevel, TemperingValue (four int32)
//    gems     0x1465187d0: int32 count (the exe refuses a negative count and, on a net archive, one above 0x80000), each
//             element (0x146544130): SocketIndex 8, ItemRecordId 64, SocketRecordId 64, Quality 32, a UBOOL
//    32       Durability (float)
//    32       bIsStolen as a UBOOL
//    128 + 64 GUID (four int32), CreationTime (FDateTime ticks)
//    128      CrateGuid
//    1        bIsStolen again, as a bit
//    128 + 64 GUID and CreationTime again: the exe's code serializes both into the same members twice
//    64       NodeGuidPurchasedFrom
//    128      CrateGuid again
//    64       LockExpiration
//    stats    0x1465181c0: a 6-bit count (the exe refuses more than 31), each StatId 64 and StatModValue 32
//    cooldowns 0x146518390: int32 count (net limit 0xaaaaa), each RecordGuid 64, DateTimeStart 64 (a double), SavedCharges
//             32 (element 0x14651c8c0)
//    custom   0x146518570: int32 count (net limit 0xaaaaa), each a byte and an FString (0x141340770)
//    64       SynchronizedTimestamp (a double)
//
//  A plain item is 1,096 bits and the empty item 1.  Proven bit for bit on the 2,342 instances of the full-login capture
//  (1,512 empty, 806 plain, 24 mounts with two persisted stats), the captured Bard's 1,217 and the retail client's own
//  ItemData (tests/capture_items.h, test_repl_items): every field that occurs there, and the persisted stats, cooldowns
//  and lock of a mount.  Not in any capture and read from the exe only: gems, custom data, a stolen or crate-carrying
//  item, a non-zero enchantment or tempering, a packed int of more than one byte.
//
//  StorageEventContainer 0x1464614d0: the object reference (128 bits), a TArray of {Slot int32, Count int32, FGuid}
//  (int32 count), the StorageEventStatus byte.
//
//  FItemStorageEvent::NetSerialize 0x145c58f20: MoveSourceContainer, ItemData, a UBOOL (1: a further optional struct
//  follows, 0x145c599d0, not decoded), EventType byte, InstigatingClient FGuid, three TOptionals (a UBOOL, then FGuid /
//  int64 / FGuid), TargetContainers (int32 count and the containers).  Round trips the 125 events of the login capture,
//  the 13 of the Bard and 31 of the client's ServerRequestStorageEvent.
//
//  Readers return nullopt for a read past the end, an instance whose duplicated members differ, a packed int that is not
//  its shortest form or over 32 bits, a UBOOL above 1, a set optional-struct flag, and a count above the bounds below (the
//  exe's own limits are far higher: a hostile count must not buy memory).  A count is also checked against the bits left,
//  so nothing is reserved for elements the input cannot hold.  A reader never reads past `effective_bits`.
// ============================================================================
#pragma once
// == C O A | Cyb ==

#include "protocol/emit/bunch_writer.h"
#include "protocol/repl/value.h"
#include "protocol/wire/packet_reader.h"

#include <cstddef>
#include <optional>

namespace aoc { namespace protocol { namespace repl {

/// Bounds on what one item or event may hold.  The exe refuses a stat count above 31 (kMaxItemStats); the others are ours.
constexpr size_t kMaxItemGems = 64;
constexpr size_t kMaxItemStats = 31;
constexpr size_t kMaxItemCooldowns = 64;
constexpr size_t kMaxItemCustomData = 64;
constexpr size_t kMaxEventContainers = 64;       // TargetContainers
constexpr size_t kMaxEventSlots = 4096;          // slots of one container

/// True when write_item_instance writes `item` and read_item_instance reads it back equal: an empty item (`has_item`
/// false) has every other member at its default, the arrays are within their bounds, custom data strings fit an FString.
bool item_instance_fits(const ItemInstance& item);

/// nullopt for what the notes above refuse.
std::optional<ItemInstance> read_item_instance(wire::PacketReader& r);

/// False (writing nothing) when `item` does not fit.
bool write_item_instance(emit::BunchWriter& out, const ItemInstance& item);

/// True when write_item_storage_event writes `e`: the instance fits, no container names ObjectId 1 (the export-flags form
/// of an object reference), the container and slot counts are within bounds.
bool item_storage_event_fits(const ItemStorageEvent& e);

std::optional<ItemStorageEvent> read_item_storage_event(wire::PacketReader& r);

/// False (writing nothing) when `e` does not fit.
bool write_item_storage_event(emit::BunchWriter& out, const ItemStorageEvent& e);

}}}  // namespace aoc::protocol::repl
