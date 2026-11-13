// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/node_inventory.inc: the members of PlayerSession for the node's stores (session/player_node_inventory.cpp)
//  (included at the end of the class by session/members_registered.inc)
// ============================================================================
private:
    void on_node_inventory_request(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    /// ServerGMAddNodeItem and ServerGMAddAllNodeItems: GM only.
    void on_gm_node_items(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    /// ClientSendNodeInventoryData: the node's stores and its money.
    void send_node_inventory(Connection& conn, int64_t node);
    /// The node whose stores the player sees: the account's citizenship, else the node the player stands in (0: none).
    int64_t stores_node() const;
    RequestBudget node_inventory_budget_{4, std::chrono::milliseconds(1000)};
    uint32_t      node_inventories_sent_ = 0;
public:
    uint32_t node_inventories_sent() const { return node_inventories_sent_; }
    /// A test: the inventory tab's request (as the client's ServerRequestNodeInventoryData).
    void send_node_inventory_for_test(Connection& conn, int64_t node) { send_node_inventory(conn, node); }
private:
