// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/callout_codec.h
//
//  The NetSerialize codec of FLocalizedMessage (mounts milestone, stage 1): the `LocalizedMessage` member of a PlayerController callout
//  (AAoCPlayerController::CalloutQueueReplication, a fast array whose items are FCalloutQueueInfo: Title, Message, TextMessage, LocalizedMessage,
//  FormattedOnScreenMessage, FromSourcePlayer, PointSize, SetCalloutType, ChatCategory, NodeLevel, bIsMessageSentByGM, ChatEmblems, ItemAssociated,
//  ToastRecordID, ActionResponseID, ContextGuid, ContextLocation, CreationTime, NodeId, PlotId, BuildingId; the array is written the "classic" way, every
//  command of an item in order with no handles: tools/ghidra/fast_array_check.py).  codecs.h's read_struct / write_struct reach it by the struct name
//  "FLocalizedMessage"; the functions here are the same codec for a caller that holds a reader.
//
//  FLocalizedMessage (mappings: StringTableId Str, Key Str, FormatArgs Array, LookupArgs Array).  The layout is read from the retail captures, not from
//  the exe (its NetSerialize is not available): per message
//
//    32 + 8*n   StringTableId as an FString: SaveNum = n (the characters with their NUL; 0 for an empty string)
//    32 + 8*n   Key, the same
//    32         FormatArgs.Num (an int32); each argument would follow (no capture has one: see below)
//
//  and nothing for LookupArgs.  [observed] 876 and 870 (the refused mount summon, "Cannot summon mount here." is the callout's plain `Message`; the struct
//  is the empty message, 96 bits of zero), and 533 callouts of the retail captures decode to the last bit with it, among them the table "Economy" with the
//  keys "UnequipFailCorruption" (336 bits) and "GatheringInterruption_Movement" (408), and the table "NCS" with "Event_Toast_Failed" (280),
//  "Event_Toast_Completed", "Quest_Toast_Started", "Quest_Toast_Abandoned" and others (tools/capture/gen_mount.py picks the ones capture_mount.h holds).
//  A message with format arguments is in no capture, and an argument's layout is unknown: the reader refuses a non-zero count and the writer has no
//  field for one, so nothing is guessed.
//
//  The strings are ASCII in every capture; the codec reads and writes an FString as codecs.h does (ANSI when pure ASCII, UTF-16 otherwise).
// ============================================================================
#pragma once

#include "protocol/emit/bunch_writer.h"
#include "protocol/repl/value.h"
#include "protocol/wire/packet_reader.h"

#include <optional>

namespace aoc { namespace protocol { namespace repl {

/// True when write_localized_message writes `m` and read_localized_message reads it back equal: both strings fit an FString (codecs.h fstring_fits).
bool localized_message_fits(const LocalizedMessage& m);

/// nullopt for a read past the end, a string the exe's reader would refuse, and a message with format arguments (see above).
std::optional<LocalizedMessage> read_localized_message(wire::PacketReader& r);

/// False (writing nothing) when `m` does not fit.
bool write_localized_message(emit::BunchWriter& out, const LocalizedMessage& m);

}}}  // namespace aoc::protocol::repl
