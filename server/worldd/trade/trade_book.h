// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  trade/trade_book.h
//
//  World-level state machine for player-to-player trades.
//  Owned by PlayerWorld (mirroring world/duels.h), ticked from PlayerWorld::tick_world:
//    - tracks connected players by character GUID, pawn id, and non-owning TradePeer*
//    - enforces 1 pending request or 1 active trade per player
//    - enforces 3D range (10 m / 1,000,000 cm^2), 60 s request TTL, and status
//      checks (phased in, alive, out of combat, uncorrupted)
//    - tracks authoritative per-side item snapshots, copper, monotonic offer versions,
//      lock/unlock/confirm state transitions, 2 s server cooldown & confirm guard,
//      dual-unlock on unlock or partner edit, and asynchronous atomic commit settlement
// ============================================================================
#pragma once

#include "items/inventory.h"
#include "items/wallet.h"
#include "protocol/repl/trade_rpc.h"
#include "session/position_saver.h"
#include "svc/services.h"
#include "trade/trade_jobs.h"
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
inline constexpr double  kTradeServerCooldownSec = 2.0;
inline constexpr double  kTradeConfirmGuardSec = 2.0;
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

enum class SideState : uint8_t {
    Opened = 0,
    Locked = 1,
    Confirmed = 2,
};

struct OfferSide {
    std::vector<protocol::repl::ItemInstance> items;
    std::vector<OfferedItemSnapshot>          snapshots;
    int64_t                                   copper = 0;
    int32_t                                   last_version = 0;
    SideState                                 state = SideState::Opened;
    double                                    last_state_change = -1.0e9;
};

enum class PlanError : uint8_t {
    None = 0,
    Drifted,
    MaxCurrency,
    InventorySpace,
};

struct TradeSidePlan {
    items::Inventory          after_inventory;
    items::Wallet             after_wallet;
    std::vector<items::Event> removed_events;
    std::vector<items::Event> added_events;
    int64_t                   copper_before = 0;
    int64_t                   copper_after = 0;
    int64_t                   net_copper_delta = 0;
    svc::TradeSideSnapshot    persist;
};

struct TradeBeforeSnapshot {
    std::string                     character_id;
    bool                            inventory_changed = false;
    std::vector<svc::InventoryItem> items;
    std::vector<svc::WalletEntry>   wallet;
    std::vector<svc::ProcessingJob> jobs;
    std::vector<int64_t>            known_recipes;
    std::vector<svc::WarehouseItem> warehouse;
};

struct TradeCommitWire {
    std::vector<items::Event> removed_events;
    std::vector<items::Event> added_events;
    bool                      copper_changed = false;
    int64_t                   copper_after = 0;
    int64_t                   net_copper_delta = 0;
};

/// Non-owning interface implemented by PlayerSession for synchronous dry-run planning and commit settlement.
class TradePeer {
public:
    virtual ~TradePeer() = default;
    virtual std::string         character_id() const = 0;
    virtual bool                can_commit_trade() const = 0;
    virtual bool                collect_trade_offer(const OfferSide& mine, std::vector<svc::InventoryItem>& out_offered) const = 0;
    virtual PlanError           plan_trade_side(const OfferSide& mine, const std::vector<svc::InventoryItem>& incoming_items,
                                                int64_t incoming_copper, TradeSidePlan& out_plan) const = 0;
    virtual TradeBeforeSnapshot capture_before_trade_snapshot() const = 0;
    virtual void                apply_committed_trade(const TradeSidePlan& plan) = 0;
    virtual void                on_trade_commit_aborted() = 0;
    virtual void                on_trade_commit_undecided() = 0;
};

struct ActiveTrade {
    TradeGuid           guid{};
    PlayerGuid          a{};
    PlayerGuid          b{};
    OfferSide           offer_a;
    OfferSide           offer_b;
    double              started_at = 0.0;
    double              both_locked_at = 0.0;
    bool                committing = false;
    uint64_t            commit_ticket = 0;
    std::string         trade_id;
    uint64_t            pawn_a = 0;
    uint64_t            pawn_b = 0;
    bool                disconnected_a = false;
    bool                disconnected_b = false;
    TradeSidePlan       plan_a;
    TradeSidePlan       plan_b;
    TradeBeforeSnapshot before_a;
    TradeBeforeSnapshot before_b;

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
        TradeLocked,
        TradeUnlocked,
        TradeConfirmed,
        CommitSucceeded,
        TradeClosed,
        Callout,
    };
    Kind                       kind = Kind::TradeClosed;
    PlayerGuid                 other_guid{};
    TradeGuid                  trade_guid{};
    uint8_t                    response = 0;
    uint8_t                    status = 0;
    protocol::repl::TradeOffer offer;
    std::string                message;
    TradeCommitWire            commit_wire;
};

class TradeBook {
public:
    explicit TradeBook(const world::ActorRegistry* registry);

    /// Wires the world's async commit worker and position sink (called by PlayerWorld).
    void attach_jobs(TradeJobs* jobs, PositionSink* sink);
    void detach_jobs();

    void connect(const PlayerGuid& guid, std::string name, uint64_t pawn, TradePeer* peer = nullptr);
    void disconnect(const PlayerGuid& guid, double now, uint64_t pawn = 0);

    void update_status(const PlayerGuid& guid, uint64_t pawn, const PlayerStatus& status, double now);
    void update_position(const PlayerGuid& guid, uint64_t pawn, double x, double y, double z);

    /// Initiates a trade request. Returns 0 when pending request was created, or the
    /// ETradeRequestResponse rejection code (and queues RequestResponse for `from`).
    uint8_t request(const PlayerGuid& from, const PlayerGuid& target, double now);

    /// Handles target's response to a pending trade request. Returns false if no matching request exists.
    bool respond(const PlayerGuid& by, const PlayerGuid& requester, uint8_t response, double now);

    /// Applies or rejects a monotonic trade offer update from `by`.
    /// Drops updates while committing, or with `version <= last_version` / `version >= INT32_MAX`.
    /// Rejects and reverts at `version + 1` if sender's side is not Opened.
    /// When `valid` is true and partner is Locked/Confirmed, unlocks partner to Opened and emits ClientUnlockTrade(partner) to both.
    bool apply_offer(const PlayerGuid& by, const TradeGuid& trade_guid, int32_t version, bool valid,
                     std::vector<protocol::repl::ItemInstance> items, std::vector<OfferedItemSnapshot> snapshots,
                     int64_t copper);

    /// Handles LOCK_TRADE (2): enforces 2 s cooldown, non-empty check, and dry-run space/currency checks for both sides.
    bool lock_trade(const PlayerGuid& by, const TradeGuid& trade_guid, double now);

    /// Handles UNLOCK_TRADE (3): bypasses cooldown and dual-unlocks both locked/confirmed sides back to Opened.
    bool unlock_trade(const PlayerGuid& by, const TradeGuid& trade_guid, double now);

    /// Handles FINALIZE_TRADE (4): enforces cooldown + both-locked guard, re-validates both sides, emits ClientTradeConfirmed,
    /// and when both sides confirm, submits ONE atomic CommitPlayerTrade job.
    bool confirm_trade(const PlayerGuid& by, const TradeGuid& trade_guid, double now);

    /// Cancels `by`'s active non-committing trade if `trade_guid` matches the trade's guid, or (when
    /// `allow_partner_fallback` is true) matches the trade partner's character guid.
    bool cancel(const PlayerGuid& by, const TradeGuid& trade_guid, bool allow_partner_fallback, const char* why);

    /// Immediately cancels any active non-committing trade involving `player` (optionally guarded by `pawn`).
    bool interrupt(const PlayerGuid& player, const char* why, uint64_t pawn = 0);

    /// Expires pending requests past 60 s, checks range/status on non-committing trades, and settles finished commits.
    void tick(double now);

    bool               connected(const PlayerGuid& guid) const;
    bool               in_trade(const PlayerGuid& guid) const;
    bool               is_committing(const PlayerGuid& guid, uint64_t pawn = 0) const;
    bool               has_pending(const PlayerGuid& guid) const;
    const ActiveTrade* active_trade(const PlayerGuid& guid) const;
    std::vector<TradeNotice> take_notices(const PlayerGuid& guid);

    size_t pending_count() const { return pending_.size(); }
    size_t active_count() const { return active_.size(); }

private:
    struct Player {
        std::string              name;
        uint64_t                 pawn = 0;
        TradePeer*               peer = nullptr;
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
    bool      participants_valid(const ActiveTrade& t) const;
    TradeGuid mint_trade_guid();
    void      notify(const PlayerGuid& to, TradeNotice notice);
    void      notify_callout(const PlayerGuid& to, const TradeGuid& tg, std::string text);
    void      close_trade_at(size_t index, uint8_t status, const char* why);
    void      cancel_pending_for(const PlayerGuid& who, bool disconnected);
    void      settle_commits();

    const world::ActorRegistry*  registry_ = nullptr;
    TradeJobs*                   jobs_ = nullptr;
    PositionSink*                sink_ = nullptr;
    std::map<PlayerGuid, Player> players_;
    std::vector<PendingRequest>  pending_;
    std::vector<ActiveTrade>     active_;
};

}  // namespace aoc::worldd::trade
