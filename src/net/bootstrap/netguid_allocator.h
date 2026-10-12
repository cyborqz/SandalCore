// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/bootstrap/netguid_allocator.h
//
//  Per-server NetGUID allocator.  Splits the FIntrepidNetworkGUID
//  space into two classes:
//
//    1. STATIC (session-invariant) — class CDOs, archetypes, the
//       Verra_World_Master level asset.  These are content-addressable
//       (same class hash → same NetGUID across all sessions).  Hard-coded
//       values extracted from captured replay.
//
//    2. DYNAMIC (per-actor-per-session) — PC actor, Pawn actor,
//       PlayerState actor, other spawned actors.  Allocated from a
//       monotonic ObjectId counter with a stable ServerId and a random
//       Randomizer.
//
//  For M2 (single-client bootstrap) we can use the captured dynamic
//  NetGUIDs exactly once — they're unique within the capture, and our
//  patched-recipe pipeline reproduces them byte-for-byte.  For M3+
//  (multi-client), the allocator returns fresh dynamic GUIDs per
//  character login.
//
//  LAYER:   net / bootstrap
//  SESSION: M2
// ============================================================================
#pragma once

#include "net/bootstrap/character_profile.h"

#include <cstdint>
#include <random>

namespace aoc { namespace net { namespace bootstrap {

/// A 128-bit FIntrepidNetworkGUID.
struct IntrepidNetGUID {
    uint64_t obj;       ///< ObjectId
    uint32_t srv;       ///< ServerId
    uint32_t rnd;       ///< Randomizer
};

class NetGUIDAllocator {
public:
    /// Build with the server identity used for this session's
    /// FIntrepidNetworkGUID.ServerId field.  Captured Hatemost session
    /// used Srv=60; pick the same for M2 byte-identity, or a different
    /// value for multi-server setups later.
    explicit NetGUIDAllocator(uint32_t server_id = 60)
        : server_id_(server_id),
          rng_(std::random_device{}()),
          next_dynamic_obj_(10'000'000ULL) {}

    // ── Static NetGUIDs (compile-time constants) ─────────────────────
    //
    // Values extracted from captured_pc_spawn_reassembled.bin and
    // related fixtures.  These correspond to content-addressable
    // NetGUIDs that the client has hard-coded in its PackageMap
    // (class CDOs) or derives from the map name (PersistentLevel).
    //
    // If a future patch changes a class or the level, these NetGUIDs
    // change too — verify against a fresh replay capture.

    static constexpr uint64_t kAoCPlayerControllerBP_CDO = 3503756484819958835ULL;
    static constexpr uint64_t kAoCPlayerControllerBP_Outer = 4074085207143396457ULL;  // /Game/ThirdPersonCPP/Blueprints/AoCPlayerControllerBP
    static constexpr uint64_t kPersistentLevel           = 16442478405498561049ULL;
    static constexpr uint64_t kVerraWorldMaster          = 16604466839667550161ULL;
    static constexpr uint64_t kVerraWorldMasterAsset     = 12923414834320654503ULL;  // /Game/Levels/Verra_World_Master/Verra_World_Master
    static constexpr uint64_t kGlobalGMCommands          = 485698175673737329ULL;
    static constexpr uint64_t kGameSystemsPlugin         = 158953490572197689ULL;    // /Script/GameSystemsPlugin
// built by C-Y-B with CoA

    // TODO(Phase B+): once identified from captured pkt#78 + downstream
    // packets, add:
    //   kPlayerPawn_CDO, kPlayerPawn_Outer
    //   kBP_AOCHUD_CDO, kBP_AOCHUD_Outer
    //   kAoCPlayerStateBP_CDO, kAoCPlayerStateBP_Outer
    //   kAoCGameStateBase_CDO

    // ── Checksums (class hashes) ──────────────────────────────────────
    static constexpr uint32_t kPlayerControllerChecksum = 0x6b62891c;
    static constexpr uint32_t kGlobalGMCommandsChecksum = 0xcaaaee3e;
    static constexpr uint32_t kBP_AOCHUD_Checksum       = 0xac6f1251;

    // ── Dynamic allocation ────────────────────────────────────────────

    /// Fill in a character's per-session actor NetGUIDs.  Call once at
    /// login.  Modifies profile in place.  The ObjectId values are drawn
    /// from a monotonic counter so each character in a session gets
    /// distinct NetGUIDs.  ServerId is constant; Randomizer is random
    /// per actor (per captured convention).
    void allocate_for_character(CharacterProfile& profile) {
        profile.pc_netguid_obj           = alloc_dynamic_obj();
        profile.pawn_netguid_obj         = alloc_dynamic_obj();
        profile.player_state_netguid_obj = alloc_dynamic_obj();
    }

    /// Get the full 128-bit PC actor NetGUID for a character profile.
    IntrepidNetGUID pc_actor_netguid(const CharacterProfile& p) const {
        return {p.pc_netguid_obj, server_id_, alloc_rnd_for(p.pc_netguid_obj)};
    }

    IntrepidNetGUID pawn_actor_netguid(const CharacterProfile& p) const {
        return {p.pawn_netguid_obj, server_id_, alloc_rnd_for(p.pawn_netguid_obj)};
    }

    IntrepidNetGUID player_state_netguid(const CharacterProfile& p) const {
        return {p.player_state_netguid_obj, server_id_, alloc_rnd_for(p.player_state_netguid_obj)};
    }

    /// M2 byte-identity mode: populate a profile with the captured
    /// Hatemost NetGUIDs so emitted bytes match the fixture exactly.
    /// Lets us round-trip existing captures without regenerating them.
    void use_captured_hatemost_netguids(CharacterProfile& profile) const {
        profile.pc_netguid_obj            = 10341530ULL;    // from pc_spawn fixture
        profile.pawn_netguid_obj          = 0;              // TODO(Phase D): extract from pkt#78
        profile.player_state_netguid_obj  = 0;              // TODO(Phase D)
    }

    uint32_t server_id() const { return server_id_; }

private:
    uint32_t server_id_;
    std::mt19937 rng_;
    uint64_t next_dynamic_obj_;

    uint64_t alloc_dynamic_obj() {
        return next_dynamic_obj_++;
    }

    /// Deterministic Randomizer per ObjectId so repeated lookups return
    /// the same value (client caches NetGUID → Obj mapping; changing
    /// Randomizer mid-session would desync).
    uint32_t alloc_rnd_for(uint64_t obj_id) const {
        // Cheap hash — good enough for non-adversarial single-server
        // use.  If we ever federate across server instances, swap in
        // a proper PRNG seeded per-character at allocation time.
        uint64_t h = obj_id * 0x9E3779B97F4A7C15ULL;
        h ^= (h >> 33);
        h *= 0xFF51AFD7ED558CCDULL;
        h ^= (h >> 33);
        return static_cast<uint32_t>(h);
    }
};

}}} // namespace aoc::net::bootstrap
