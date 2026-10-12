// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/player_pawn_splicer.h  —  PM106 (2026-05-04) — Phase D Step 2.2
//
//  Ships the captured pkt#78 (FULL inner stream — 3 bunches, 5160 bits) over
//  the wire to give the client the 8-subobject PlayerPawn ActorOpen complete
//  with NetGUID exports for the archetype path, level path, and per-component
//  class paths.
//
//  ── Why this exists alongside PlayerPawnEmitter ─────────────────────────
//
//  PlayerPawnEmitter (PM45) builds a SYNTHETIC PlayerPawn ActorOpen using
//  ActorBuilder + a schema with 8 named components.  But its content blocks
//  for those components carry NO actual data — empty property maps result in
//  empty content blocks (Function J rollback).  Result: client constructs
//  Default__PlayerPawn_C with default-CDO components, but the BP-level
//  CharacterAppearanceComponent has no replicated appearance data, so the
//  MergeableSkeletalMeshComponent's mesh slot stays empty.  Player ends up
//  with a "Player" nameplate but no visible character body.
//
//  PlayerPawnSplicer's role is complementary: ship the captured pkt#78
//  verbatim so the client receives the FULL InternalLoadObject chain that
//  the captured AOC server sent during the recording session.  This:
//
//    1. Registers /Game/ThirdPersonCPP/Blueprints/PlayerPawn in PackageMap
//    2. Registers Default__PlayerPawn_C class CDO
//    3. Registers the PersistentLevel path
//    4. Creates a Pawn channel (ch=114) bound to NetGUID 54
//    5. Creates 8 subobject channels bound to component classes:
//         - BaseCharacterInfo
//         - CombatInfo
//         - OwnerInfo
//         - BackpackComponent
//         - EquipmentComponent
//         - QuestStorageComponent
//         - RewardStorageComponent
//         - Character Appearance (CharacterAppearanceComponent)
//
//  The captured Pawn (NetGUID 54) is a SECOND pawn alongside our minted one
//  (NetGUID 16777218 from PM97).  Possession still targets 16777218 via
//  ClientRestart RPC — but the player MIGHT see the captured pawn standing
//  next to themselves with a real mesh, validating that mesh assembly works.
//
//  If that's confirmed, the next step is to either:
//    (a) Possess the captured pawn instead of the minted one (drop our
//        synthetic and use NetGUID 54 throughout)
//    (b) Substitute the captured Pawn NetGUID in pkt#78 with our minted one
//        before shipping (bit-level surgery)
//
//  ── Wire pacing ──────────────────────────────────────────────────────────
//
//  The captured pkt#78 was sent ~150 packets into the captured session,
//  AFTER the PC channel was open and the level was loaded.  In our session
//  we ship it AFTER PlayerPawnEmitter::emit_open and BEFORE PcEmitter::
//  emit_pawn_link, so the splice doesn't conflict with the synthetic Pawn's
//  channel registration (which uses ch=19, not ch=114).
//
//  Failure of this splicer is non-fatal — possession still works via the
//  synthetic path.  The splicer is purely additive PackageMap context.
//
//  ── File-driven enable ──────────────────────────────────────────────────
//
//  Splicer is gated on probe_pkt78_splice.txt.  Set to "1" to enable, "0"
//  to disable.  Default disabled until tested.
//
//  LAYER:  net / player-pawn
//  OWNER:  Phase D Step 2.2
// ============================================================================
#pragma once

#include <string>

struct sockaddr_in;

namespace aoc { namespace net {
// ~ C-O-A & Cyb ~

class IGameServerHost;

class PlayerPawnSplicer {
public:
    PlayerPawnSplicer(IGameServerHost& host, const std::string& client_key);

    /// Ships the full captured pkt#78 stream (5160 bits / 645 bytes,
    /// 3 bunches: ch=85 GUIDExport + ch=0 ctrl-msg + ch=114 Pawn ActorOpen).
    /// Reads probe_pkt78_splice.txt; if absent or "0", returns true without
    /// emitting (no-op — caller treats as success).  If "1", emits the
    /// captured stream verbatim through send_bunch_packet.
    bool emit_captured_stream(const sockaddr_in& client_addr);

private:
    IGameServerHost& host_;
    std::string client_key_;
};

}}  // namespace aoc::net
