// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/emit/name_update_bunch.h
//
//  Phase III M1: native "Name property update" bunch emitter.
//
//  PURPOSE
//  -------
//  The captured replay sets the PlayerController's visible character name to
//  "RandomChar" via a partial-bunch chain starting at pkt#104.  That chain
//  can't be length-mutated in place (the client validates a cross-fragment
//  invariant — see docs/phase-ii-postmortem.md §cause).
//
//  This emitter produces a **single-fragment, non-partial** bunch that
//  contains only the Name property update.  When sent AFTER pkt#22 opens
//  the PlayerController channel, the client overwrites its cached Name
//  value without needing to reassemble the partial chain — sidestepping
//  the failing invariant entirely.
//
//  WIRE FORMAT (derived from 2026-04-24 RE; see docs/wire-format.md §18)
//  --------------------------------------------------------------------
//  Bunch payload for a Name update on the actor channel:
//
//    [16-byte prefix]  00 00 00 01  00 00 00 01  00 00 00 01  00 00 00 6A
//                      ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^  ^^^^^^^^^^^^
//                      meaning unknown (constant across captures)     0x6A
//                      hypothesis: remote-subobject refcount +        = cmd
//                                  cmd_index tag                        byte
//
//    [int32 save_num]   length(name)+1  (positive = ASCII path)
//    [save_num bytes]   ASCII-encoded name + NUL terminator
//
//  BIT-EXACT CHECKPOINT
//  --------------------
//  build_name_update_bunch_payload("RandomChar", buf) produces 31 bytes
//  that are byte-for-byte identical to captured pkt#104 bits 1496..1744
//  (248 bits).  Validated by test_name_update_bunch.
//
//  INTEGRATION (caller responsibility)
//  -----------------------------------
//  The caller wraps the payload in a full bunch header (channel = PC's
//  actor channel = 3 in captured fixture, is_reliable=1, is_partial=0,
//  b_open=0).  Header emission is ActorBuilder's job or copy it from the
//  existing synthesis path in game_server::replay_loop.
//
//  LAYER:   Protocol / emit
//  OWNER:   Phase III M1
//  SESSION: 2026-04-24
// ============================================================================
#pragma once

#include "protocol/emit/bunch_writer.h"

#include <cstdint>
#include <string>
// [C | O | A - C_Y_B]

namespace aoc { namespace protocol { namespace emit {

/// Build the Name property-update bunch payload (the "content block" —
/// no bunch header; caller handles that).
///
/// Writes `[16-byte prefix][int32 save_num][save_num bytes]` into `out`
/// LSB-first per byte (same convention as BunchWriter / UE5 wire format).
///
/// Returns the total number of bits written: `128 + 32 + 8*(name.size()+1)`.
///
/// The `name` is encoded as ASCII + NUL terminator via the UE5 "positive
/// length" FString path.  For unicode names we'd use UCS-2 + negative
/// length — deferred until we confirm the basic ASCII path works live.
size_t build_name_update_bunch_payload(const std::string& name,
                                        BunchWriter& out);

/// V2 — proper property-delta bunch payload (for LIVE sends).
///
/// Unlike V1 (which copies a mid-bunch 16-byte region that looks valid
/// only when following earlier property content), V2 starts with the
/// canonical bunch-payload header that UActorChannel::ProcessBunch
/// expects when applying a delta on an already-open actor channel:
///
///   [1 bit ]  bHasRepLayoutExport = 0
///   [32 bits] NumGUIDsInBunch     = 0
///   [32 bits] cmd_index           = <caller's choice>
///   [32 bits] FString save_num    = name.size() + 1
///   [N+1 B ]  FString bytes + NUL
///
/// The caller chooses `cmd_index`:
///   28  (0x1C) — our flat catalog hierarchy index for
///                AAoCPlayerController.Name
///   106 (0x6A) — value observed in captured pkt#104 at the Name slot
///                (exact semantics unverified; may be FName index or
///                AoC-specific per-class numbering)
///
/// Total bits written: `97 + 8*(name.size()+1)`.
size_t build_name_update_bunch_payload_v2(const std::string& name,
                                            uint32_t cmd_index,
                                            BunchWriter& out);

}}} // namespace aoc::protocol::emit
