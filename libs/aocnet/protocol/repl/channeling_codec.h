// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/channeling_codec.h
//
//  The NetSerialize codec of FChannelingData (Gathering stage 2, the fix after the user's test 1): the `ChannelingData` member (handle 26) of the pawn's InteractInfo component
//  (UCharacterSecondaryInfo; the one worldd already uses for MountablePet and MountPtr).  codecs.h's read_struct / write_struct reach it by the struct name "FChannelingData"; the
//  functions here are the same codec for a caller that holds a reader.
//
//  What it is for.  The retail server sets this property on the harvester's pawn at the start of a harvest (in the packet that wakes the node, 87 ms after the interact) and clears it
//  about 2 ms after the result frame.  Without it the client has no channel: it plays no gather animation, shows no bar, and never sends ServerInterruptChannel when the player moves
//  (the user's test 1: "no chopping animation", "even after walking around, the tree was chopped").  It names the node, when the channel started, how long it lasts and the tool in use.
//
//  The layout is read from ten retail harvests (the research captures 1427 1454 1455 1457 1460 1461 1466 1469 1470 1471; tests/capture_gather.h, tools/capture/gen_gather.py), not from
//  the exe (its NetSerialize was not read).  After the object every item is a 32-bit word (a bool is 32 bits wide: one UBOOL):
//
//    start   576 bits   Target (128-bit netguid: the node's actor) | Flags 6 | Active 1 | StartTime (a double) | Timed 1 | Duration (a float) | 1 | ToolRecord (int64) | 1 | ToolGuid (A B C D)
//    clear   288 bits   Target (the same node)                     | Flags 0x106 | Active 0 | StartTime 0.0   | Timed 0                      (nothing follows)
//
//  [observed] in all ten: Flags 6 / 0x106, Active 1 / 0, Timed 1 / 0, the two flags inside the timed part 1, StartTime a whole number of seconds on the synchronized clock of
//  CurrentEffects (13,788,648.0 in 1427, whose effects start at 13,788,648.65), Duration the channel's length (11.03 s for the Daffodil, 14.71 s for an Oak), ToolRecord the worn tool's
//  item record id and ToolGuid its instance guid (the ItemGuid of the same frame's ClientUpdateItemDurability).
//  [inferred] what Flags and Active mean (6 is the channel kind, 0x106 adds a bit for "over"; Active the channel running), what the two flags inside the timed part guard, and that a
//  channel that ends otherwise (a cancel, which no capture shows) clears the way the result does.
//
//  The reader keeps the words it reads as they are (Flags raw) and refuses what no capture shows rather than guess: a bool above 1, an unflagged tool record or guid, an object with
//  ObjectId 1 (the export form).  The writer writes exactly the two forms above (Timed picks which) and refuses a value the wire cannot carry: a clear with a duration or tool set.
// ============================================================================
#pragma once

#include "protocol/emit/bunch_writer.h"
#include "protocol/repl/value.h"
#include "protocol/wire/packet_reader.h"

#include <optional>

namespace aoc { namespace protocol { namespace repl {

/// True when write_channeling_data writes `d` and read_channeling_data reads it back equal: a clear (not Timed) carries no duration and no tool, and the target is no ObjectId 1.
bool channeling_data_fits(const ChannelingData& d);

/// nullopt for a read past the end and for what the layout above refuses.
std::optional<ChannelingData> read_channeling_data(wire::PacketReader& r);

/// False (writing nothing) when `d` does not fit.
bool write_channeling_data(emit::BunchWriter& out, const ChannelingData& d);

}}}  // namespace aoc::protocol::repl
