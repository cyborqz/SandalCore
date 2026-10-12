// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/character_profile.h
//
//  The player's chosen character data — shared INPUT to every actor
//  builder in the protocol module.  Populated from the XClient gRPC
//  flow (character creation / selection) and handed to builders when
//  the server needs to synthesize per-player bunches.
//
//  Scope today (Phase 3.6): just the fields we can currently act on.
//  As new fields get decoded in later phases, we extend this struct.
//  Keeping it plain data (no logic) makes it trivial to move between
//  modules and to serialize for future multi-server / save / load work.
// ============================================================================
#pragma once

#include <cstdint>
#include <string>

namespace aoc { namespace protocol {

/// Snapshot of "who this player is" at spawn time.  Fed into actor
/// builders to personalize their output.  None of these fields have
/// default values baked into the captured replay — that's exactly why
/// we need this struct.
struct CharacterProfile {
    // ─── Identity ──────────────────────────────────────────────────────
    std::string name;           // Display name (e.g. "HATEMOSTTT")

    // ─── Class / race / gender (extracted from CreateCharacterInfo) ────
    // These are AoC-specific integer IDs.  XClientService already parses
    // them from the character-creation protobuf; they reach us via the
    // same path as `name`.
    uint64_t archetype_id = 0;  // Class
    uint64_t race_id      = 0;
    uint64_t gender_id    = 0;

    // ─── Spawn transform (optional) ────────────────────────────────────
    // If these are nonzero the builder will serialize them into the
    // ActorOpen's SerializeNewActor Location/Rotation fields.  Otherwise
    // the actor spawns at origin (captured-replay default).
    struct Vec3 { float x = 0.0f, y = 0.0f, z = 0.0f; };
    struct Rot3 { float pitch = 0.0f, yaw = 0.0f, roll = 0.0f; };

    Vec3 spawn_location;
    Rot3 spawn_rotation;

    // ─── Session metadata (set by GameServer, not the launcher) ───────
    uint32_t actor_netguid_hint = 0;  // Suggest a specific NetGUID for the
                                      // PlayerController; 0 = let server
                                      // pick.  Needed for multiplayer where
                                      // each player's PC gets a unique ID.
};

}} // namespace aoc::protocol
