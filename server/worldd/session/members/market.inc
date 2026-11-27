// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/market.inc: the Marketplace listing saves in flight (market/market_requests.inc)
//  Included at the end of PlayerSession (session/player_session.h); it sets its own public: and private:.
// ============================================================================
public:
    /// Marketplace: the listing saves the worker still writes (a test runs ticks until it is 0).
    size_t market_pending() const { return market_pending_.size(); }
    /// Marketplace: true while a listing's save is in flight; a bag save is held until it answers.
    bool market_save_in_flight() const { return market_save_in_flight_; }

private:
    /// One listing whose save the worker has and the session has not applied yet: the listing, the stack and the copper it took (market::PendingListing), and what the
    /// client is sent only when the store answered Ok.
    struct PendingMarket : market::PendingListing {
        std::string                       stack_guid;    // the held stack's instance guid, as the request named it
        std::vector<bootstrap::ItemEvent> events;        // the storage events of the units leaving the bag
    };

    void push_storefront_pages(Connection& conn, int64_t node);
    void market_resync(Connection& conn, Clock::time_point now);
    bool market_hold_bag_save(const char* why);
    void run_market(Connection& conn, Clock::time_point now);
    void market_landed(Connection& conn, Clock::time_point now, PendingMarket job, const market::ListingSaveAnswer& answer);
    void market_refund(const PendingMarket& job);
    void market_end(const char* why);
    void market_send_listed(Connection& conn, const PendingMarket& job, Clock::time_point now);

    std::deque<PendingMarket>   market_pending_;          // the saves in flight, oldest first
    std::set<std::string>       market_stacks_in_flight_; // the held stacks a save still writes
    bool                        market_save_in_flight_ = false;
    bool                        market_bag_held_ = false; // a bag save was held while a listing's save was in flight
    size_t                      market_logged_ = 0;
    size_t                      market_refused_logged_ = 0;
    size_t                      market_answered_logged_ = 0;   // the storefront pages and the data sent
    size_t                      market_ignored_logged_ = 0;    // the calls of the window that were ignored
