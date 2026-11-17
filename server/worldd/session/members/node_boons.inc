// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/node_boons.inc: the members of PlayerSession for the node buildings' boons (session/player_node_boons.cpp)
//  (included at the end of the class by session/members_registered.inc)
// ============================================================================
private:
    std::map<int64_t, int> node_boons_;            // effect record -> the stacks the session put on the character for the nodes' running modifications
    std::set<int64_t>      node_boons_refused_;    // ... the effects the runtime does not have (asked once)
    Clock::time_point      next_node_boons_{};     // the next look (every kNodeBoonInterval, and at once when asked: node_boons_dirty_)
    bool                   node_boons_dirty_ = true;
    size_t                 node_boons_log_ = 0;
    /// The boons the nodes' running modifications put on the character where it stands now (effect record -> stacks).
    std::map<int64_t, int> wanted_node_boons() const;
    /// Puts on and takes off the boons (each tick; it looks every kNodeBoonInterval or when something changed).
    void run_node_boons(Connection& conn, Clock::time_point now);
public:
    const std::map<int64_t, int>& node_boons() const { return node_boons_; }
    /// (tests) the boons are looked at on the next tick.
    void refresh_node_boons_for_test() { node_boons_dirty_ = true; }
