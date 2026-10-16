// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  capture_in_place.h  (Gathering stage 3: the trees that start and never finish)
//
//  The client's calls on a subobject it names by path, as worldd's log shows them: "a bunch of 550 bits on channel 3 (PlayerController) does not read: ignored (starts
//  0400000000000000000000000000000014c8240f04000000)" -- three at the login of every run, and ten a second in the run of 2026-10-12 13:34 (210 and more; the runs of 03:17 and 05:31 had the
//  same bunch, and earlier logs had 550, 2200, 4950 and 7150 bits: a multiple of 550, one to thirteen identical blocks in one bunch [inferred: a 0.1 s looping timer
//  of the client firing once per interval that a long frame spans]).  Only the first 24 bytes were ever logged.  They are the client's own and read as: bHasRepLayout 0, bIsActor 0 (a
//  subobject), the default GUID (ObjectId 1, the rest 0: UPackageMapClient writes it for an object the server never gave a GUID, and names the object in place), FExportFlags 5 (path +
//  checksum) and the first 54 bits of the outer's GUID (ObjectId 0x0103C932, a dynamic GUID of worldd's: the PlayerController or another actor of the player).  The reader refused
//  ObjectId 1 in a block's header, so the whole bunch went, and with it any call that shared it.  No retail capture has such a block (333,369 client blocks scanned), so what the client
//  calls here is something worldd does not give it a GUID for and retail's server does, or does not need to [inferred].
//
//  What follows in those 550 bits -- the outer's other bits, the path, the checksum and the call's own field -- was never logged.  block() completes the logged prefix to 550 bits with a
//  PLACEHOLDER: a path of 19 characters and a field of 52 bits.  It is a golden of the layout and of the size, not of what the call was; the next run names it (player_session.cpp:
//  note_unserved_blocks logs the path and the whole first bunch).
// ============================================================================
#pragma once

#include "protocol/emit/bunch_writer.h"
#include "protocol/repl/subobject.h"

#include <cstddef>
#include <cstdint>

namespace capture_in_place {

/// The 24 bytes of the client's bunch as worldd logged them (192 bits, LSB first).
inline constexpr char   kLoggedHex[] = "0400000000000000000000000000000014c8240f04000000";
inline constexpr size_t kLoggedBits = 192;
/// The size of the client's bunch.
inline constexpr size_t kBlockBits = 550;
/// The outer's ObjectId bits the log held (the low 32; the high ones were 0 as far as the log goes).
inline constexpr uint64_t kOuterObjectId = 0x0103C932ull;
/// PLACEHOLDER (19 characters: the 550 bits then come out exactly).
inline constexpr char kPath[] = "UnmappedComponent_0";
// C-O-A.Cyb

/// The block's header: a subobject named in place, flags 5.  The outer's ServerId and Randomizer are placeholders as well.
inline aoc::protocol::repl::BlockHeader header() {
    aoc::protocol::repl::BlockHeader h;
    h.is_actor = false;
    aoc::protocol::repl::InPlaceName name;
    name.flags = 5;
    name.outer.ObjectId = kOuterObjectId;
    name.outer.ServerId = 60;
    name.outer.Randomizer = 0x6ee87ee4u;
    name.path = kPath;
    name.checksum = 0;
    h.in_place = name;
    return h;
}

/// One such block of kBlockBits bits: the header, SerializeIntPacked(52) and a 52-bit payload (a placeholder).
inline aoc::protocol::emit::BunchWriter block() {
    aoc::protocol::emit::BunchWriter payload;
    payload.write_bits(0x5A5A5A5Au, 32);
    payload.write_bits(0x5A5A5u, 20);
    aoc::protocol::emit::BunchWriter out;
    aoc::protocol::repl::write_content_block(out, header(), payload, /*server_form=*/false);
    return out;
}

}  // namespace capture_in_place
