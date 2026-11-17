// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/node_buildings.inc: the members of PlayerSession for the node's service buildings (session/player_node_buildings.cpp)
//  (included at the end of the class by session/members_registered.inc)
// ============================================================================
private:
    /// ServerPurchaseServiceBuilding(NodeId, PlotId, BuildingId, bFree, bSkipPlayerPhase): the mayor's Construct in the Building Manager.
    void on_building_purchase(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    /// ServerGetBuildingConstructionProgress(Node, Plot): ClientReceiveBuildingConstructionProgress.
    void on_building_progress(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    /// ServerFulfillServiceBuildingConstruction(NodeGuid, PlotGuid, BuildingGuid): the contribution window gives what the character carries.
    void on_building_fulfil(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    /// The expansions: ServerPurchaseExpansion, ServerCancelExpansionPurchase, ServerRemoveExpansion, ServerCancelExpansionRemoval, ServerVoteOnExpansion.
    void on_expansion_purchase(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    void on_expansion_cancel(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    void on_expansion_remove(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    void on_expansion_cancel_removal(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    void on_expansion_vote(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    /// The demolitions: ServerCommenceDemolition(Plot), ServerCancelDemolition(Plot), ServerVoteOnDemolition(Plot, bIsVoteYes).
    void on_demolition(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    void on_demolition_cancel(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    void on_demolition_vote(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    /// A game master's: ServerGMRequestServiceBuildingPlotData, ServerGMRequestNodeSkillPointData, ServerGMAdvanceServiceBuildingPhaseEndTime,
    /// ServerContributeToServiceBuildingConstructionCheat, ServerForceBuildingMaintainance, ServerSetPlotState.
    void on_gm_building_rpc(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);

    /// The node of a plot-only call (ServerCommenceDemolition names the plot alone): the node whose plots at its level hold it, the character's own first (0: none).
    int64_t node_of_plot(int64_t plot) const;
    /// May this character act for `node`'s buildings: its mayor, or a game master with the console on (`gm`).  Refused with the mayor's notice.
    bool may_build(Connection& conn, int64_t node, const char* rpc, bool& gm, Clock::time_point now);
    /// After a change: the elections saved, the plots shown, the node's summary and stores sent to this character.
    void buildings_changed(Connection& conn, int64_t node);
    /// ClientReceiveBuildingConstructionProgress for the plot.
    void send_building_progress(Connection& conn, int64_t node, int64_t plot);
    /// The character gives the plot what it needs of what it carries: the crate on its back (and with `bag`, its bags).  The units it gave.
    int64_t contribute_carried(Connection& conn, int64_t node, int64_t plot, bool bag, Clock::time_point now);
    /// The plot of `node` the Foreman's window shows: the construction (or expansion) needing players' gifts whose time runs out first, a crate the character
    /// carries can go to first (0: none).
    int64_t contribution_plot_of(int64_t node) const;
    /// A system line in the character's chat.
    void building_notice(Connection& conn, const std::string& text);

    RequestBudget building_budget_{8, std::chrono::milliseconds(1000)};
    int64_t  contribution_node_ = 0, contribution_plot_ = 0;   // the site the Foreman's window shows (ServerFulfillServiceBuildingConstruction must name it)
    uint32_t buildings_bought_ = 0, building_actions_ = 0, building_refusals_ = 0, contribution_windows_ = 0, building_progress_sent_ = 0, building_gm_ = 0;
    int64_t  contribution_units_ = 0;
public:
    /// The Construction Foreman's lines (dialogue::Result::open_contribution): the units the crate on the character's back gives the node's site, then the window
    /// (ClientShowBuildingContributionWindow) with the site.
    void open_contribution_window(Connection& conn, const world::WorldActor& npc, Clock::time_point now);
    uint32_t buildings_bought() const { return buildings_bought_; }
    uint32_t building_actions() const { return building_actions_; }
    uint32_t building_refusals() const { return building_refusals_; }
    uint32_t contribution_windows() const { return contribution_windows_; }
    uint32_t building_progress_sent() const { return building_progress_sent_; }
    int64_t  contribution_units() const { return contribution_units_; }
