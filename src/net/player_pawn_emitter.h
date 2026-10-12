// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/player_pawn_emitter.h
//
//  PM39 (2026-04-30) — Path C Phase 2: Native player Pawn ActorOpen.
//
//  Emits a player-controlled Pawn actor on its own channel.  Distinct from
//  PawnEmitter (which splices a captured Guard_Soldier_C NPC).  This one
//  mints a fresh Pawn NetGUID for the connected player and opens a Pawn
//  channel that PC can possess (Phase 3 will link PC.Pawn → this Pawn).
//
//  RE foundation:
//    - Archetype: "Default__PlayerPawn_C" (FString 22)
//    - Outer:     "/Game/ThirdPersonCPP/Blueprints/PlayerPawn" (FString 43)
//    - Schema:    libs/aocnet/protocol/schema/pawn_schema.cpp
//                  - 10 root properties (location, controller, ...)
//                  - 6 subobject components (Alignment, InteractInfo,
//                    CharacterInformationComponent, CombatInfo,
//                    AbilityComponent, StatsComponent)
//    - Source: docs/RE-AOC-CLASSES.md + test_pawn_spawn_diff.cpp
//
//  Step 1 of Phase 2 scope: ship the ActorOpen ONLY (no captured-tail
//  splice — same pattern as PcEmitter PM35).  Pawn will exist on the
//  client with default property values until Phase 3 links PC → Pawn.
// ============================================================================
#pragma once

#include <cstdint>
#include <string>

struct sockaddr_in;

namespace aoc { namespace net {

class IGameServerHost;

class PlayerPawnEmitter {
public:
    PlayerPawnEmitter(IGameServerHost& host, const std::string& client_key);

    /// Emit the player Pawn ActorOpen bunch.  Returns true if sent.
    bool emit_open(const sockaddr_in& client_addr);

    /// A pawn property update setting bHidden, as reliable bunch `ch_seq` on
    /// `channel`.  The captured pawn update (pkt#107) hides the pawn; the
    /// original server revealed it once the client finished loading (#2523).
    bool emit_hidden(const sockaddr_in& client_addr, uint32_t channel, uint32_t ch_seq,
                     bool hidden);

private:
    IGameServerHost& host_;
    std::string client_key_;
};

}} // namespace aoc::net
