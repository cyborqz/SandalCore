// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/node_xp.inc: the members of PlayerSession for the nodes' XP and level-ups (session/player_node_xp.cpp)
//  (included at the end of the class by session/members_registered.inc)
// ============================================================================
private:
    uint64_t                   node_levels_version_ = 0;   // the registry's version run_node_levels last looked at
    std::map<int64_t, int64_t> level_up_toasts_;           // node -> the countdown whose toast was looked at (its level_up_at_ms)
    size_t                     level_up_toasts_sent_ = 0;
    size_t                     node_xp_gains_ = 0;
    std::string                node_maps_levels_;          // the levels the node maps were last sent with (their log line says a change of level)
    /// The countdown toast of a node that levels up, to the players in its zone (from run_nodes).
    void run_node_levels(Connection& conn, Clock::time_point now);
public:
    /// `base` XP (before the players' rate) the player earned where it stands gives that node's XP (times the node rate).
    void earn_node_experience(int64_t base, const char* why);
    size_t node_xp_gains() const { return node_xp_gains_; }
    size_t level_up_toasts_sent() const { return level_up_toasts_sent_; }
