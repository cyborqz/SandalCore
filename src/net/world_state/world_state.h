// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/world_state/world_state.h
//
//  Authoritative-server state tracker.  Owns one CharacterState per
//  connected client; runs a 10Hz replication tick that broadcasts
//  dirty property updates to subscribers.
//
//  Design reference: docs/LIVE-SERVER-STATE-ARCHITECTURE.md
//
//  ── Invariants ────────────────────────────────────────────────────
//  * One CharacterState per ClientState::key (IP:port).
//  * State changes (HP, MP, position, name) call
//    `CharacterState::mark_dirty(flag)`.  The tick loop picks them up.
//  * Property updates go out as non-reliable bunches on the correct
//    per-property channel (PC=3, Pawn=14/114, subobjects per the
//    captured actor channel assignment — to be calibrated).
//
//  LAYER:   net / world state
//  OWNER:   Path B
//  SESSION: M2+
// ============================================================================
#pragma once

#include "net/bootstrap/character_profile.h"
#include "net/bootstrap/netguid_allocator.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace aoc { namespace net { class IGameServerHost; }}

namespace aoc { namespace net { namespace world_state {
// C | O | A x C.Y.B

/// Per-character dirty-flag set.  When a property changes, the owner
/// calls `mark_dirty(FLAG)` which OR's into the atomic bitmap.  The
/// tick loop clears flags after broadcasting.
struct DirtyFlags {
    enum Flag : uint32_t {
        HEALTH    = 1u << 0,
        MANA      = 1u << 1,
        STAMINA   = 1u << 2,
        POSITION  = 1u << 3,
        ROTATION  = 1u << 4,
        NAME      = 1u << 5,
        TITLE     = 1u << 6,
        GUILD     = 1u << 7,
        LEVEL     = 1u << 8,
        APPEARANCE= 1u << 9,
        // ... extend as we RE more properties
    };

    std::atomic<uint32_t> bits{0};

    void mark(uint32_t flag)   { bits.fetch_or(flag); }
    void clear_flag(uint32_t f) { bits.fetch_and(~f); }
    bool test(uint32_t flag) const { return bits.load() & flag; }
    uint32_t snapshot()     { return bits.load(); }
    uint32_t take_and_clear() { return bits.exchange(0); }
};

/// Per-channel chSeq tracker.  UE5's reliable-channel protocol requires
/// chSeq increments by exactly 1 per reliable bunch.  When we send a
/// reliable bunch on channel N, we allocate `++map[N]` for its chSeq.
///
/// Captured session seeded ch=3 at chSeq=1978; we track that offset so
/// our first reliable bunch on ch=3 uses chSeq=1979 (or wherever the
/// last captured bunch left off).
///
/// Non-reliable bunches don't allocate chSeq (always written as 0).
class PerChannelChSeq {
public:
    /// Get the next chSeq to use on this channel.  Post-increments.
    uint32_t next(uint32_t channel) {
        std::lock_guard<std::mutex> lk(mu_);
        return ++map_[channel];
    }
    /// Seed a channel (e.g. after replay emission, track where captured
    /// left off).  `starting_chseq` will be incremented before first use.
    void seed(uint32_t channel, uint32_t starting_chseq) {
        std::lock_guard<std::mutex> lk(mu_);
        map_[channel] = starting_chseq;
    }
private:
    mutable std::mutex                    mu_;
    std::unordered_map<uint32_t, uint32_t> map_;
};

/// Everything the server tracks about a connected character at runtime.
struct CharacterState {
    // Identity (populated from CharacterProfile at login)
    bootstrap::CharacterProfile profile;

    // Live mutable state — changes during play
    bootstrap::FVectorScaled    position;
    float                       cur_health  = 100.0f;
    float                       cur_mana    = 50.0f;
    float                       cur_stamina = 100.0f;

    // Replication bookkeeping
    DirtyFlags                  dirty;
    PerChannelChSeq             ch_seq;

    // NetGUIDs (allocated at spawn)
    bootstrap::IntrepidNetGUID  pc_netguid{};
    bootstrap::IntrepidNetGUID  pawn_netguid{};
    bootstrap::IntrepidNetGUID  player_state_netguid{};

    // Connection info
    std::string    client_key;     // "127.0.0.1:58153" etc.
    // sockaddr_in omitted here to avoid dragging winsock into the header;
    // WorldState holds addr separately in a side-map.

    std::chrono::steady_clock::time_point last_broadcast =
        std::chrono::steady_clock::now();
// -- CoA & C Y B --

    /// Convenience: mark HP dirty AND update the value in one step.
    void set_health(float v) {
        cur_health = v;
        dirty.mark(DirtyFlags::HEALTH);
    }
    void set_mana(float v) {
        cur_mana = v;
        dirty.mark(DirtyFlags::MANA);
    }
    void set_stamina(float v) {
        cur_stamina = v;
        dirty.mark(DirtyFlags::STAMINA);
    }
    void set_position(bootstrap::FVectorScaled p) {
        position = p;
        dirty.mark(DirtyFlags::POSITION);
    }
    void set_name(const std::string& n) {
        profile.name = n;
        dirty.mark(DirtyFlags::NAME);
    }
};

/// Top-level authoritative state + tick loop.
class WorldState {
public:
    WorldState(IGameServerHost& host,
                bootstrap::NetGUIDAllocator& alloc)
        : host_(host), alloc_(alloc) {}

    ~WorldState() { stop(); }

    /// Called when a client finishes login.  Populates CharacterState
    /// from the profile, allocates NetGUIDs, starts tracking.
    void on_character_login(const std::string& client_key,
                             const void* addr_ptr,  // sockaddr_in*, opaque in header
                             bootstrap::CharacterProfile profile);

    /// Called when client disconnects — removes from state map.
    void on_character_logout(const std::string& client_key);

    /// Get mutable reference to a character's state (for combat/input
    /// systems that mutate HP/position).  Returns nullptr if not logged in.
    CharacterState* get(const std::string& client_key);

    /// Start the 10Hz replication tick thread.
    void start_tick();

    /// Signal stop.  Joins the tick thread.
    void stop();

private:
    IGameServerHost&                                 host_;
    bootstrap::NetGUIDAllocator&                      alloc_;
    std::mutex                                       mu_;
    std::unordered_map<std::string, CharacterState>  characters_;
    // Per-character sockaddr_in storage, parallel to characters_ map.
    // Keyed by same client_key.  Stored as opaque bytes to keep header
    // free of winsock2.
    std::unordered_map<std::string, std::array<uint8_t, 32>> addrs_;

    std::atomic<bool> running_{false};
    std::thread       tick_thread_;

    void tick_loop();                                        ///< main 10Hz loop
    void broadcast_dirty_state(CharacterState& s,
                                const void* addr_ptr);       ///< per-character broadcast
};

}}} // namespace aoc::net::world_state
