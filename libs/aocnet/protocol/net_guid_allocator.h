// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/net_guid_allocator.h
//
//  Server-side dynamic NetGUID allocator for multiplayer.
//
//  UE5 NetGUIDs are the client's stable reference to an actor/subobject.
//  For our live-server transition:
//
//    * STATIC NetGUIDs (class BPs, level, plugins) are SHARED across all
//      players.  They match what the client already has loaded from disk.
//      Hardcoded in the actor builders (e.g. Archetype=120, Level=10).
//
//    * DYNAMIC NetGUIDs (a player's PlayerController, Pawn, PlayerState,
//      AbilityComponent, etc.) are ALLOCATED PER-PLAYER by this class.
//      Each connecting player gets a unique block so their actors don't
//      collide with other players' actors.
//
//  Allocation scheme:
//    * kDynamicBase = 0x01000000 (16.7M) — well clear of captured NetGUIDs
//      which are in the low thousands range.
//    * Per-player block of kBlockSize = 256 GUIDs.  A player gets their
//      PC=base, Pawn=base+1, PlayerState=base+2, plus room for components.
//    * Up to (MaxPlayers) = (UINT32_MAX - kDynamicBase) / kBlockSize
//      ≈ 16 million players.  More than enough for any realistic server.
//
//  Thread-safety: the allocator is lock-free via std::atomic.  Multiple
//  GameServer threads can allocate concurrently without blocking.
//
//  Lifecycle:
//    * allocate_player_block() — called on player connect → returns the
//      base NetGUID for that player's block.
//    * release_player_block(base) — called on player disconnect.  Marks
//      the block as free for reuse.  In the MVP we just leak; reuse is
//      a TODO for long-running servers.
// ============================================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <spdlog/spdlog.h>

namespace aoc { namespace protocol {
// -- C.O.A & Cyb --

/// Per-player NetGUID block.  Returned to the caller after allocation
/// so they can pass specific GUIDs into the actor builders.
///
/// PM52 (2026-04-30) — STRIDE BY 2 (even ObjectIds only).
///   AOC's UE5 client treats bit 0 of the NetGUID's ObjectId as a flag:
///     bit 0 = 0  → bare dynamic reference (just 128 bits, no path data)
///     bit 0 = 1  → inline-export reference (followed by outer GUID + path)
///   When the parser sees an "inline-export" GUID with no inline data
///   following, it raises an archive error → SerializeNewActor fails.
///   PC at base+0 worked because base 0x01000000 is even.  Pawn at
///   base+1 (= 0x01000001, odd) failed with "archive error parsing
///   NetGUID" + IsDynamic:0.  Striding by 2 keeps every slot even.
///   Block size 256 / stride 2 = 128 dynamic actors per player — still
///   plenty.
struct PlayerNetGuidBlock {
    uint64_t base                 = 0;   // First GUID in the block (even)
    uint64_t player_controller    = 0;   // base + 0
    uint64_t pawn                 = 0;   // base + 2
    uint64_t player_state         = 0;   // base + 4
    uint64_t ability_component    = 0;   // base + 6
    uint64_t stats_component      = 0;   // base + 8
    uint64_t alignment_component  = 0;   // base + 10
    uint64_t combat_component     = 0;   // base + 12
    uint64_t base_character_info  = 0;   // base + 14
    uint64_t interact_info        = 0;   // base + 16
    // + room for future growth: 128 - 9 used = 119 free slots × stride 2

    bool is_valid() const { return base != 0; }
};

class NetGuidAllocator {
public:
    // Dynamic block base — chosen to be clearly above any captured NetGUID.
    // Captured RandomChar's actor NetGUID is 1; class BP is 120; level is 10.
    // All captured NetGUIDs fit in the low thousands.  16.7M gives us an
    // unmistakable gap.
    static constexpr uint64_t kDynamicBase = 0x01000000ULL;  // 16,777,216

    // Number of GUIDs reserved per player.  256 is comfortable:
    //   - 10 actor/component slots defined now
    //   - Room for 246 more future slots (pets, summons, etc.)
    //   - Aligns to 0x100 boundary for easy debugging in hex
    static constexpr uint64_t kBlockSize = 256;

    /// Allocate a fresh block for a newly-connected player.
    /// Thread-safe.  Returns a valid block or an all-zero block if
    /// exhausted (only happens at >16M concurrent players).
    PlayerNetGuidBlock allocate_player_block(const std::string& player_key) {
        const uint64_t slot = next_slot_.fetch_add(1, std::memory_order_relaxed);
        const uint64_t base = kDynamicBase + slot * kBlockSize;

        if (base + kBlockSize < base) {
            // Overflow — should never realistically happen
            spdlog::error("[NetGuidAllocator] Overflow: {} slots exhausted",
                          slot);
            return {};
        }

        // PM52: stride by 2 to keep ObjectId bit 0 = 0 (bare-dynamic flag).
        PlayerNetGuidBlock block;
        block.base                 = base;
        block.player_controller    = base + 0;
        block.pawn                 = base + 2;
        block.player_state         = base + 4;
        block.ability_component    = base + 6;
        block.stats_component      = base + 8;
        block.alignment_component  = base + 10;
        block.combat_component     = base + 12;
        block.base_character_info  = base + 14;
        block.interact_info        = base + 16;

        {
            std::lock_guard<std::mutex> lk(mu_);
            player_to_block_[player_key] = block;
        }

        spdlog::info("[NetGuidAllocator] Allocated block for \"{}\": "
                     "base=0x{:x} (PC={}, Pawn={}, PS={})",
                     player_key, base,
                     block.player_controller, block.pawn, block.player_state);
        return block;
    }

    /// Return the block for a player if one was allocated.
    PlayerNetGuidBlock get_block(const std::string& player_key) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = player_to_block_.find(player_key);
        if (it == player_to_block_.end()) return {};
        return it->second;
    }

    /// Called on player disconnect.  MVP: just records the release.
    /// Reuse of released blocks is a future optimization.
    void release_player_block(const std::string& player_key) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = player_to_block_.find(player_key);
        if (it == player_to_block_.end()) return;
        spdlog::info("[NetGuidAllocator] Released block for \"{}\" "
                     "(base=0x{:x})", player_key, it->second.base);
        player_to_block_.erase(it);
    }
// Cyb with C O A

    /// Diagnostics: how many blocks have been allocated so far.
    size_t live_block_count() const {
        std::lock_guard<std::mutex> lk(mu_);
        return player_to_block_.size();
    }
    uint64_t total_slots_issued() const {
        return next_slot_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<uint64_t> next_slot_{0};
    mutable std::mutex mu_;
    std::unordered_map<std::string, PlayerNetGuidBlock> player_to_block_;
};

}} // namespace aoc::protocol
