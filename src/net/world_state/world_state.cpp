// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/world_state/world_state.cpp — scaffold
//
//  Minimum viable implementation: track state + tick every 100 ms.
//  broadcast_dirty_state() is a stub for now — filled in once we have
//  working native property-update emitters per field.
// ============================================================================
#include "net/world_state/world_state.h"
#include "net/native_connect_sequencer.h"   // IGameServerHost

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#endif

#include <cstring>
#include <spdlog/spdlog.h>

namespace aoc { namespace net { namespace world_state {

void WorldState::on_character_login(const std::string& client_key,
                                      const void* addr_ptr,
                                      bootstrap::CharacterProfile profile) {
    std::lock_guard<std::mutex> lk(mu_);
    CharacterState& s = characters_[client_key];
    s.client_key = client_key;
    s.profile    = std::move(profile);
    s.position   = s.profile.spawn_location;

    // Allocate NetGUIDs for this character's actors
    alloc_.allocate_for_character(s.profile);
    s.pc_netguid           = alloc_.pc_actor_netguid(s.profile);
    s.pawn_netguid         = alloc_.pawn_actor_netguid(s.profile);
    s.player_state_netguid = alloc_.player_state_netguid(s.profile);

    // Seed ch=3 chSeq where the captured replay left off.
    // From 150-packet hybrid bootstrap, captured ch=3 chSeqs reach ~2020.
    s.ch_seq.seed(3, 2050);

    // Copy sockaddr_in bytes
    if (addr_ptr) {
        auto& buf = addrs_[client_key];
        std::memcpy(buf.data(), addr_ptr, sizeof(sockaddr_in));
    }

    spdlog::info("[WorldState] Login '{}' (key={}): PC={} Pawn={} PS={}",
                 s.profile.name, client_key,
                 s.pc_netguid.obj, s.pawn_netguid.obj, s.player_state_netguid.obj);
}

void WorldState::on_character_logout(const std::string& client_key) {
    std::lock_guard<std::mutex> lk(mu_);
    characters_.erase(client_key);
    addrs_.erase(client_key);
    spdlog::info("[WorldState] Logout {}", client_key);
}

CharacterState* WorldState::get(const std::string& client_key) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = characters_.find(client_key);
    return it == characters_.end() ? nullptr : &it->second;
}

void WorldState::start_tick() {
    if (running_.exchange(true)) return;
    tick_thread_ = std::thread([this]{ tick_loop(); });
    spdlog::info("[WorldState] Tick thread started (10 Hz)");
}

void WorldState::stop() {
    running_.store(false);
    if (tick_thread_.joinable()) tick_thread_.join();
}
// built with C | O | A and C.Y.B

void WorldState::tick_loop() {
    constexpr auto kTickInterval = std::chrono::milliseconds(100);
    while (running_.load()) {
        std::this_thread::sleep_for(kTickInterval);

        // Collect dirty characters under lock, then release before broadcast.
        std::vector<std::pair<std::string, std::array<uint8_t, 32>>> to_broadcast;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (auto& [key, state] : characters_) {
                if (state.dirty.bits.load() == 0) continue;
                auto it_addr = addrs_.find(key);
                if (it_addr == addrs_.end()) continue;
                to_broadcast.emplace_back(key, it_addr->second);
            }
        }

        for (auto& [key, addr_buf] : to_broadcast) {
            CharacterState* s = get(key);
            if (!s) continue;
            broadcast_dirty_state(*s, addr_buf.data());
        }
    }
    spdlog::info("[WorldState] Tick thread exited");
}

void WorldState::broadcast_dirty_state(CharacterState& s, const void* addr_ptr) {
    // STUB — will invoke PropertyUpdateEmitter methods per dirty flag.
    // For M2 scaffold we just log what would fire.
    uint32_t flags = s.dirty.take_and_clear();
    if (flags & DirtyFlags::HEALTH) {
        spdlog::debug("[WorldState] {} HP→{}", s.profile.name, s.cur_health);
        // TODO: emit_stats_update(s, *addr, HEALTH)
    }
    if (flags & DirtyFlags::MANA) {
        spdlog::debug("[WorldState] {} MP→{}", s.profile.name, s.cur_mana);
    }
    if (flags & DirtyFlags::STAMINA) {
        spdlog::debug("[WorldState] {} Stamina→{}", s.profile.name, s.cur_stamina);
    }
    if (flags & DirtyFlags::POSITION) {
        spdlog::debug("[WorldState] {} pos→({},{},{})", s.profile.name,
                      s.position.x, s.position.y, s.position.z);
    }
    if (flags & DirtyFlags::NAME) {
        spdlog::debug("[WorldState] {} Name→{}", s.client_key, s.profile.name);
    }
    s.last_broadcast = std::chrono::steady_clock::now();
}

}}} // namespace aoc::net::world_state
