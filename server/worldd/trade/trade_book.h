// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  trade/trade_book.h
//
//  World-level state machine for player-to-player trades.
//  Owned by PlayerWorld (mirroring world/duels.h), ticked from PlayerWorld::tick_world:
//    - tracks connected players by character GUID and pawn id (relog-safe disconnect)
//    - enforces 1 pending request or 1 active trade per player
//    - enforces 3D range (10 m / 1,000,000 cm^2), 60 s request TTL, and status
//      checks (phased in, alive, out of combat, uncorrupted)
//    - tracks authoritative per-side item snapshots, copper, and monotonic offer
//      versions, emitting TradeNotice entries consumed by each PlayerSession
// ============================================================================
#pragma once

#include "protocol/repl/trade_rpc.h"
#include "svc/services.h"
#include "world/actor_registry.h"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace aoc::worldd::trade {

using PlayerGuid = std::array<uint32_t, 4>;
using TradeGuid = std::array<uint32_t, 4>;

inline constexpr double  kMaxTradeRangeCm = 1000.0;
inline constexpr double  kMaxTradeRangeSqCm = kMaxTradeRangeCm * kMaxTradeRangeCm;
inline constexpr double  kTradeRequestTimeoutSec = 60.0;
inline constexpr size_t  kMaxTradeBackpackStacks = 12;
inline constexpr size_t  kMaxTradeTotalStacks = 64;
inline constexpr int64_t kMaxTradeCopper = 10'000'000;

inline bool is_zero_guid(const PlayerGuid& g) { return (g[0] | g[1] | g[2] | g[3]) == 0; }
inline protocol::repl::GuidValue to_guid_value(const PlayerGuid& g) { return {g[0], g[1], g[2], g[3]}; }
inline PlayerGuid from_guid_value(const protocol::repl::GuidValue& g) { return {g.a, g.b, g.c, g.d}; }

struct PlayerStatus {
    bool phased_in = false;
    bool dead = false;
    bool in_combat = false;
    bool corrupted = false;
};

struct OfferedItemSnapshot {
    std::string        instance_guid;
    svc::ItemContainer container = svc::ItemContainer::Backpack;
    int                slot = -1;
    int64_t            record_id = 0;
    int32_t            quantity = 0;
    int32_t            quality = 0;

    bool operator==(const OfferedItemSnapshot&) const = default;
};

struct OfferSide {
    std::vector<protocol::repl::ItemInstance> items;
    std::vector<OfferedItemSnapshot>          snapshots;
    int64_t                                   copper = 0;
    int32_t                                   last_version = 0;
};

struct ActiveTrade {
    TradeGuid  guid{};
    PlayerGuid a{};
    PlayerGuid b{};
    OfferSide  offer_a;
    OfferSide  offer_b;
    double     started_at = 0.0;

    const OfferSide& side_of(const PlayerGuid& who) const { return who == a ? offer_a : offer_b; }
    OfferSide&       side_of(const PlayerGuid& who) { return who == a ? offer_a : offer_b; }
    PlayerGuid       partner_of(const PlayerGuid& who) const { return who == a ? b : a; }
};

struct TradeNotice {
    enum class Kind : uint8_t {
        RequestReceived,
        RequestResponse,
        BeginTrade,
        OfferUpdated,
        TradeClosed,
    };
    Kind                       kind = Kind::TradeClosed;
    PlayerGuid                 other_guid{};
    TradeGuid                  trade_guid{};
    uint8_t                    response = 0;
    uint8_t                    status = 0;
    protocol::repl::TradeOffer offer;
};

class TradeBook {
public:
    explicit TradeBook(const world::ActorRegistry* registry);

    void connect(const PlayerGuid& guid, std::string name, uint64_t pawn);
    void disconnect(const PlayerGuid& guid, double now, uint64_t pawn = 0);

    void update_status(const PlayerGuid& guid, uint64_t pawn, const PlayerStatus& status, double now);
    void update_position(const PlayerGuid& guid, uint64_t pawn, double x, double y, double z);

    /// Initiates a trade request. Returns 0 when pending request was created, or the
    /// ETradeRequestResponse rejection code (and queues RequestResponse for `from`).
    uint8_t request(const PlayerGuid& from, const PlayerGuid& target, double now);

    /// Handles target's response to a pending trade request. Returns false if no matching request exists.
    bool respond(const PlayerGuid& by, const PlayerGuid& requester, uint8_t response, double now);

    /// Applies or rejects a monotonic trade offer update from `by`.
    /// Drops `version <= last_version` and `version >= INT32_MAX`.
    /// When `valid` is true, updates authoritative offer and notifies both participants.
    /// When `valid` is false, preserves prior authoritative offer, records `last_version = version`
    /// (allowing pipelined `version + 1` edits), and echoes the prior offer at `version + 1` to `by`
    /// only so the client UI's strict `<` version check overwrites its local offer cleanly.
    bool apply_offer(const PlayerGuid& by, const TradeGuid& trade_guid, int32_t version, bool valid,
                     std::vector<protocol::repl::ItemInstance> items, std::vector<OfferedItemSnapshot> snapshots,
                     int64_t copper);

    /// Cancels `by`'s active trade if `trade_guid` matches the trade's guid, or (when
    /// `allow_partner_fallback` is true) matches the trade partner's character guid.
    bool cancel(const PlayerGuid& by, const TradeGuid& trade_guid, bool allow_partner_fallback, const char* why);

    /// Immediately cancels any active trade involving `player`.
    bool interrupt(const PlayerGuid& player, const char* why);

    /// Expires pending requests past 60 s and cancels active trades violating range or status.
    void tick(double now);

    bool               connected(const PlayerGuid& guid) const;
    bool               in_trade(const PlayerGuid& guid) const;
    bool               has_pending(const PlayerGuid& guid) const;
    const ActiveTrade* active_trade(const PlayerGuid& guid) const;
    std::vector<TradeNotice> take_notices(const PlayerGuid& guid);

    size_t pending_count() const { return pending_.size(); }
    size_t active_count() const { return active_.size(); }

private:
    struct Player {
        std::string              name;
        uint64_t                 pawn = 0;
        PlayerStatus             status;
        bool                     has_pos = false;
        double                   x = 0.0, y = 0.0, z = 0.0;
        std::vector<TradeNotice> notices;
    };

    struct PendingRequest {
        PlayerGuid requester{};
        PlayerGuid target{};
        double     asked_at = 0.0;
    };

    bool      where(const PlayerGuid& guid, double& x, double& y, double& z) const;
    bool      within_range(const PlayerGuid& a, const PlayerGuid& b) const;
    bool      is_busy(const PlayerGuid& guid) const;
    TradeGuid mint_trade_guid();
    void      notify(const PlayerGuid& to, TradeNotice notice);
    void      close_trade_at(size_t index, uint8_t status, const char* why);
    void      cancel_pending_for(const PlayerGuid& who, bool disconnected);

    const world::ActorRegistry* registry_ = nullptr;
    std::map<PlayerGuid, Player> players_;
    std::vector<PendingRequest>  pending_;
    std::vector<ActiveTrade>     active_;
    uint64_t                     guid_seq_ = 1;
};

}  // namespace aoc::worldd::trade
