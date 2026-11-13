// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/buy_orders.inc: the members of PlayerSession for the node's buy orders (session/player_buy_orders.cpp)
//  (included at the end of the class by session/members_registered.inc)
// ============================================================================
private:
    /// ServerGetPossibleAndActiveBuyOrders(NodeGuid): ClientUpdateBuyOrderData for the node.
    void on_buy_orders_request(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    /// ServerAddBuyOrder(RequestedBuyOrder): the mayor places an order.
    void on_buy_order_add(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    /// ServerFulfillActiveBuyOrder(NodeGuid, RequestedBuyOrderGuid, FulfillmentCombo): a delivery at the Buy Order Manager.
    void on_buy_order_fulfil(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    /// The conversation reached the Buy Order Manager's BuyOrderFulfillment node: the window's node (the NPC's), then the node's orders.
    void open_buy_order_window(Connection& conn, const world::WorldActor& npc, Clock::time_point now);
    /// ClientUpdateBuyOrderData: the node's running orders, what this character gave them, its slots, and for its mayor what it may order.
    void send_buy_order_data(Connection& conn, int64_t node, Clock::time_point now);
    /// The orders a node may run at once (citizenship_node's MinBuyOrderSlots of its level; 0 for a level that takes no citizens).
    int  buy_order_slots(int64_t node) const;
    /// The node's biome (EBiomeType; 0 for a node of no known biome).
    int  node_biome(int64_t node) const;
    /// A delivery's reward: `rolls` rolls of the order's reward table into the bags (at most kMaxBuyOrderRewardRolls).
    void roll_buy_order_rewards(Connection& conn, int64_t table, int64_t rolls, Clock::time_point now);
    /// A wall-clock time as the GameState's world seconds (the clock of the node projects' times on the wire).
    int64_t to_world_seconds(int64_t wall_at_ms, Clock::time_point now);
    RequestBudget      buy_order_budget_{6, std::chrono::milliseconds(1000)};
    int64_t            buy_order_node_ = 0;   // the node whose Buy Order Manager's window this session opened last
    std::optional<int> buy_order_slots_for_test_;
    bool               buy_order_window_for_test_ = false;   // a test opened the window with no conversation (open_buy_orders_for_test)
    uint32_t buy_order_windows_ = 0, buy_order_data_sent_ = 0, buy_orders_added_ = 0, buy_orders_refused_ = 0, buy_order_deliveries_ = 0, buy_order_deliveries_refused_ = 0;
    int64_t  buy_order_units_ = 0;
public:
    uint32_t buy_order_windows() const { return buy_order_windows_; }
    uint32_t buy_order_data_sent() const { return buy_order_data_sent_; }
    uint32_t buy_orders_added() const { return buy_orders_added_; }
    uint32_t buy_orders_refused() const { return buy_orders_refused_; }
    uint32_t buy_order_deliveries() const { return buy_order_deliveries_; }
    uint32_t buy_order_deliveries_refused() const { return buy_order_deliveries_refused_; }
    int64_t  buy_order_units() const { return buy_order_units_; }
    /// A test: the node's buy order slots, whatever the data says.
    void set_buy_order_slots_for_test(int slots) { buy_order_slots_for_test_ = slots; }
    /// A test: as if the Buy Order Manager's panel of `node` were open (no conversation and no NPC are checked for a delivery).
    void open_buy_orders_for_test(int64_t node) {
        buy_order_node_ = node;
        buy_order_window_for_test_ = true;
    }
private:
