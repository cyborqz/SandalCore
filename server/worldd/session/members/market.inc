// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/market.inc: the Marketplace steps in flight: a listing's save, a purchase and a removal (market/market_requests.inc, market/market_sales.inc)
//  Included at the end of PlayerSession (session/player_session.h); it sets its own public: and private:.
// ============================================================================
public:
    /// Marketplace: the steps (a listing's save, a purchase, a removal) the worker still writes (a test runs ticks until it is 0).
    size_t market_pending() const { return market_pending_.size(); }
    /// Marketplace: true while a step's save is in flight; a bag save is held until it answers.
    bool market_save_in_flight() const { return market_save_in_flight_; }
    /// Marketplace: the purchases and removals of a cart that wait for the step before them (market/market_sales.inc).
    size_t market_queued() const { return market_queue_.size(); }

private:
    /// One step whose save the worker has and the session has not applied yet: the listing, and for a listing's save the stack and the copper it took (market::PendingListing),
    /// and what the client is sent only when the store answered that the step is stored.
    struct PendingMarket : market::PendingListing {
        std::string                       stack_guid;    // a listing's save: the held stack's instance guid, as the request named it
        std::vector<bootstrap::ItemEvent> events;        // the storage events of the units leaving the bag, or coming into it
        items::Outcome                    added;         // a purchase or a removal: what the units' placement added to the bags (taken out again when the step is refused)
        int64_t                           paid = 0;      // a purchase: the price and the tax the purse paid
        int32_t                           asked = 0;     // a purchase: the units the request named (the answer carries them)
    };

    /// A purchase or a removal that arrived while a step of this session was in flight or waiting.  It holds nothing and has changed nothing: it keeps its place in line and runs all
    /// its checks when the step before it has landed.
    struct QueuedMarket {
        market::StepKind                 kind = market::StepKind::Buy;
        protocol::repl::GuidValue        window;     // the market's guid, as the client named it
        protocol::repl::GuidValue        listing;
        int32_t                          quantity = 0;   // a purchase: the units asked for
    };

    void push_storefront_pages(Connection& conn, int64_t node);
    void market_resync(Connection& conn, Clock::time_point now);
    bool market_hold_bag_save(const char* why);
    void run_market(Connection& conn, Clock::time_point now);
    void market_landed(Connection& conn, Clock::time_point now, PendingMarket job, const market::ListingSaveAnswer& answer);
    void market_refund(const PendingMarket& job);
    void market_end(const char* why);
    void market_send_listed(Connection& conn, const PendingMarket& job, Clock::time_point now);

    // purchases and removals (market/market_sales.inc)
    void on_market_purchase(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_market_removal(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    /// The two steps' own work, with all their checks: when they arrive with nothing of the session in their way, or when the step before them has landed.
    void market_begin_purchase(Connection& conn, const protocol::repl::GuidValue& market_guid, const protocol::repl::GuidValue& listing_guid, int32_t asked_quantity, Clock::time_point now);
    void market_begin_removal(Connection& conn, const protocol::repl::GuidValue& market_guid, const protocol::repl::GuidValue& listing_guid, Clock::time_point now);
    /// A step that arrives behind one of the session's own waits its turn (true), is answered 3 (true: a double submit or a line that is full), or starts at once (false).
    bool market_wait_turn(Connection& conn, QueuedMarket step);
    /// Starts the waiting steps, one at a time, as each one's predecessor has landed.
    void market_run_queue(Connection& conn, Clock::time_point now);
    /// The response a step is refused with before it starts, when this session or the listing has a step in flight (0: none).
    uint8_t market_step_blocked(const protocol::repl::GuidValue& listing) const;
    void market_answer_purchase(Connection& conn, const protocol::repl::GuidValue& listing, int32_t quantity, uint8_t response);
    void market_answer_removal(Connection& conn, const protocol::repl::GuidValue& listing, uint8_t response);
    void market_purchase_landed(Connection& conn, PendingMarket job, const market::ListingSaveAnswer& answer);
    void market_removal_landed(Connection& conn, PendingMarket job, const market::ListingSaveAnswer& answer);
    /// Takes back what a purchase or a removal put in the bags (and the purse) in this session's memory: the step was refused.
    void market_undo(const PendingMarket& job);
    /// The response a refused step answers, from the store's answer (a refusal for a reason of the listing, or one of the request itself).
    uint8_t market_refusal_code(const market::ListingSaveAnswer& answer) const;

    std::deque<PendingMarket>   market_pending_;          // the steps in flight, oldest first (one at a time)
    std::deque<QueuedMarket>    market_queue_;            // the steps of a cart that wait for the one in flight, in the order of the lines (market::kMaxQueuedSteps at most)
    /// ServerRequestMarketplaceItemPurchase and ServerRequestMarketplaceItemRemoval: a cart is sent in one frame, so the burst holds a whole cart and the answers 3 of its lines past the
    /// line (the item budget of eight would drop the lines past the eighth without any answer); then ten a second.
    static constexpr uint32_t   kMarketBurst = 32;
    RequestBudget               market_budget_{kMarketBurst, std::chrono::milliseconds(100)};
    std::set<std::string>       market_stacks_in_flight_; // the held stacks a save still writes
    bool                        market_save_in_flight_ = false;
    bool                        market_bag_held_ = false; // a bag save was held while a step's save was in flight
    size_t                      market_logged_ = 0;
    size_t                      market_refused_logged_ = 0;
    size_t                      market_answered_logged_ = 0;   // the storefront pages and the data sent
    size_t                      market_ignored_logged_ = 0;    // the calls of the window that were ignored
    bool                        mail_note_pending_ = false;    // a sale or an expiry changed this character's mail: the list is read again
