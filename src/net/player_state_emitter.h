// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/player_state_emitter.h  —  Phase D Step 3 (2026-05-05)
//
//  Emits an ActorOpen for AAoCPlayerState on a dedicated channel after the
//  Pawn ActorOpen lands.  Carries replicated player identity properties:
//
//    - PlayerNamePrivate  (FString)  — the visible nameplate ("MaxPayne")
//    - CharacterArchetype (UInt32)   — class ID (Ranger=5, Rogue=4, etc.)
//    - CharacterGuid      (FGuid)    — 16-byte unique character ID
//    - Score / PlayerId   (Float, Int32) — UE5 standard
//
//  The PlayerState appears as a separate actor on its own channel.  PC's
//  PlayerState property gets linked to it via a follow-up update (similar
//  to PcEmitter::emit_pawn_link).
//
//  ── Why this might unlock visible mesh ──────────────────────────────
//  AOC's mesh assembler likely reads CharacterArchetype + Race + Gender
//  from the PlayerState (not from the appearance subobject).  When those
//  properties replicate to the client, OnRep_CharacterArchetype on the
//  PlayerState may trigger mesh assembly via DataTable lookup.
//
//  ── Probe gating ────────────────────────────────────────────────────
//  Default DISABLED (probe_player_state_emit.txt absent or "0").  Set to
//  "1" to enable.  This protects the working baseline (possession + name-
//  plate + stable connection) while the implementation is being calibrated.
//
//  Call sequence:
//    1. PlayerPawnEmitter::emit_open       — opens Pawn ch=19
//    2. PcEmitter::emit_pawn_link          — fires ClientRestart
//    3. AppearanceEmitter::emit_seed       — sends appearance
//    4. PlayerStateEmitter::emit_open      — opens PlayerState ch=21
//    5. PlayerStateEmitter::emit_properties — sends Name/Archetype/etc.
//    6. PcEmitter::emit_player_state_link  — links PC.PlayerState (TODO)
//
//  LAYER:   net
//  OWNER:   Phase D Step 3
//  SESSION: 2026-05-05
// ============================================================================
#pragma once

#include <cstdint>
#include <string>

struct sockaddr_in;
// == C | O | A | C:Y:B ==

namespace aoc { namespace net {

class IGameServerHost;

class PlayerStateEmitter {
public:
    PlayerStateEmitter(IGameServerHost& host, const std::string& client_key);

    /// Emit the PlayerState ActorOpen + initial properties.
    ///
    /// Returns true if (a) probe is disabled (no-op success), or
    /// (b) the bunch was sent successfully.  Returns false only on
    /// hard failure (NetGUID block missing, build error).
    bool emit_open(const sockaddr_in& client_addr);

    /// Phase D Step 3 Iter3 — emit a property-update bunch on the PS
    /// channel carrying PlayerNamePrivate=<character_name>.  Must be
    /// called AFTER emit_open lands on the client (the channel must
    /// exist).  No-op success when the probe is disabled or when the
    /// host has no character_name available.
    bool emit_player_name(const sockaddr_in& client_addr);

private:
    IGameServerHost& host_;
    std::string client_key_;
};

}}  // namespace aoc::net
