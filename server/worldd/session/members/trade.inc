// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/trade.inc: PlayerSession members for player trading (session/player_trade.cpp)
//  Included at the end of PlayerSession (session/members_registered.inc); sets its own public: and private:.
// ============================================================================
public:
    const trade::TradeBook* trade_book_for_test() const { return trade_book_.get(); }
    bool                    trade_in_progress() const { return trade_in_progress_; }
    bool                    trade_commit_pending() const;
    bool                    trade_undecided() const { return trade_undecided_; }
    size_t                  trade_calls() const { return trade_calls_; }
    bool                    spend_copper_for_test(int64_t copper) { return wallet_.pay(copper); }
    void                    set_combat_flag_for_test(bool in_combat) { combat_flag_ = in_combat; }
    void                    reserve_item_for_test(int32_t cast_id, const std::string& instance_guid);
    void                    unreserve_item_for_test(int32_t cast_id) { item_reserved_.erase(cast_id); }

private:
    class TradePeerAdapter;

    void connect_trade(PlayerWorld& world);
    void leave_trade(Clock::time_point now);
    void run_trade(Connection& conn, Clock::time_point now);
    void trade_interrupted(Clock::time_point now, const char* why);
    void sync_trade_status(Clock::time_point now);
    bool trade_defers_hand_over(const char* why, bool periodic);

    void on_trade_request(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_trade_request_response(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_trade_update_items(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_trade_transaction(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);

    bool trade_item_reserved(const std::string& instance_guid) const;
    bool validate_trade_offer(const protocol::repl::TradeOffer& incoming,
                              std::vector<protocol::repl::ItemInstance>& out_items,
                              std::vector<trade::OfferedItemSnapshot>& out_snapshots,
                              int64_t& out_copper,
                              std::string& refusal) const;
    bool verify_active_trade_offer(const trade::OfferSide& side) const;
    bool can_commit_trade() const;
    bool collect_trade_offer(const trade::OfferSide& mine, std::vector<svc::InventoryItem>& out_offered) const;
    trade::PlanError plan_trade_side(const trade::OfferSide& mine,
                                     const std::vector<svc::InventoryItem>& incoming_items,
                                     int64_t incoming_copper,
                                     trade::TradeSidePlan& out_plan) const;
    trade::TradeBeforeSnapshot capture_before_trade_snapshot() const;
    void apply_committed_trade(const trade::TradeSidePlan& plan);
    void on_trade_commit_aborted();
    void on_trade_commit_undecided();

    static constexpr size_t                    kTradeBurst = 16;
    static constexpr std::chrono::milliseconds kTradeInterval{100};

    std::shared_ptr<trade::TradeBook> trade_book_;
    std::unique_ptr<trade::TradePeer> trade_peer_;
    RequestBudget                     trade_budget_{kTradeBurst, kTradeInterval};
    bool                              trade_in_progress_ = false;
    bool                              trade_bag_held_ = false;
    bool                              trade_commit_left_in_flight_ = false;
    bool                              trade_undecided_ = false;
    size_t                            trade_calls_ = 0;
    size_t                            trade_logged_ = 0;
    size_t                            trade_refused_logged_ = 0;
private:
