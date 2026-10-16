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
//  FItemStorageEvent::NetSerialize 0x145c58f20: MoveSourceContainer, ItemData, a UBOOL (1: a TOptional<FCalloutQueueInfo>
//  follows, 0x145c599d0: below), EventType byte, InstigatingClient FGuid, three TOptionals (a UBOOL, then FGuid /
//  int64 / FGuid), TargetContainers (int32 count and the containers).  Round trips the 125 events of the login capture,
//  the 13 of the Bard and 31 of the client's ServerRequestStorageEvent.
//
//  FCalloutQueueInfo 0x145c599d0 (Gathering stage 3, "the pick-up popup"; the same struct as an item of AAoCPlayerController's CalloutQueueReplication, written
//  by a NetSerialize of its own that picks some of its members; the memory offsets are the exe's, the names the replicated properties of the same offsets, inferred),
//  in the order the code runs it, read from the exe and proven bit for bit on the 24 events of the retail harvests and hunts that fill the Resource containers
//  (tests/capture_resource.h, `full_hex`: 22 captures, each event 7.0 to 7.3 kbit of which the callout is 5.2 to 5.4):
//
//    FText    TextMessage (+0x20)        flags 32, history 8 (0xFF = none), for none a UBOOL: has a culture-invariant string (0, so no more): 72 bits, in all 24
//    FString  Message (+0x10)            "Acquired <a id=...> [Coconut Wood] </>x5" (items/acquired.h has the text)
//    FLocalizedMessage (+0x30)           0x1456f4680, callout_codec.h: the table and the key as FStrings and the argument count: 96 bits of zero in all 24
//    8        SetCalloutType (+0x94)     6 in all 24
//    32       PointSize (+0x90)          36
//    8        ChatCategory (+0x95)       141
//    FString  FormattedOnScreenMessage (+0x70)   the message again in all 24;  FString FromSourcePlayer (+0x80) and FString ChatEmblems (+0x98): empty
//    8        NodeLevel (+0x96)          0;  UBOOL bIsMessageSentByGM (+0x97): 0
//    FItemInstance ItemAssociated (+0xa8)        the event's own item, equal to its ItemData in all 24 (the same guid and creation time)
//    64 + 64  ToastRecordID (+0x198), ActionResponseID (+0x1a0)   0
//    FVector  ContextLocation (+0x1b8)   0x14141ed80: three doubles at AoC's net version, 0
//    64       CreationTime (+0x1d0)      FDateTime ticks: the moment the server made it (the only member that differs between two events of the same item)
//
//  The client's event apply (0x146a8c320) hands it, when set (the TOptional's flag is at +0x240 of the struct, 576 bytes), to 0x146a90160 (the item feed: the callout, and a chat
//  line when +0x95 is 6 -- it is 141 in the retail events).  The event has a second optional the apply uses, a TOptional<FText> that it sends to the pawn's text delegate
//  (+0x1c50): no harvest event sets one (the three optionals after the instigator are unset in all 24; which of the serialized members it is, is not known).  An FText of another history and a localized message with arguments are refused (the codecs refuse them: codecs.h, callout_codec.h); neither is in a capture.
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
