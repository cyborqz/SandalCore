// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/node_taxes.inc: the members of PlayerSession for the node taxes: the vendor window's tax table (session/player_vendors.cpp), the mayor's rates and
//  the node's treasury (session/player_node_taxes.cpp)
//  (included at the end of the class by session/members_registered.inc)
// ============================================================================
public:
    /// Tax-table requests (the answer is 7 KB): a burst of three, then one a second.
    static constexpr uint32_t                  kTaxBurst = 3;
    static constexpr std::chrono::milliseconds kTaxInterval{1000};
    /// The tax-table requests of the vendor windows, and the tables sent in answer.
    size_t   tax_requests() const { return tax_requests_; }
    size_t   tax_tables_sent() const { return tax_tables_sent_; }

private:
    void on_node_tax_request(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    RequestBudget tax_budget_{kTaxBurst, kTaxInterval};
    size_t        tax_requests_ = 0, tax_tables_sent_ = 0, tax_logged_ = 0;
    void on_set_tax_override(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    void on_gm_set_tax_override(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    void on_show_mayor_inventory(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    void on_gm_add_node_gold(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    /// ClientSetNodeTaxTable with `node`'s table now (the mayor's rates in it).
    void send_node_tax_table(Connection& conn, int64_t node);
    uint32_t tax_overrides_taken_ = 0, tax_overrides_refused_ = 0;
public:
    uint32_t tax_overrides_taken() const { return tax_overrides_taken_; }
    uint32_t tax_overrides_refused() const { return tax_overrides_refused_; }
