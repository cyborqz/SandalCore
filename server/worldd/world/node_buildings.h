// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/node_buildings.h  (Nodes 3: the service buildings and the node's skill points)
//
//  A node's service buildings, kept with the node's elections (world/elections.h NodeElection::buildings, so elections.json holds them).  Pure logic over the data's
//  table (world/node_buildings_table.h, tools/worldd/gen_node_buildings.py); the session (session/player_node_buildings.cpp) talks to the client, and the world
//  (PlayerWorld::tick_buildings) brings the plots to the time and shows them (CityNodes::set_live_plots: the summary, the scenery and the town's people follow).
//
//    a plot      the data's row (world/node_wire.h PlotRow, given here as a PlotInfo) says what stands on it at the start: the civic core and the seed's workshops
//                Operational, the NodeXP construction projects UnderConstruction below a Village, the mayor's service plots Empty.  A plot something happened to
//                is in NodeBuildings::plots, which overrides the row (an explicit Empty after a demolition, too).
//    buying      the mayor's ServerPurchaseServiceBuilding: an Empty plot, one of its buildings (service_plot_building) that the Building Manager shows
//                (bDisplayInBuildingManager) and that costs something; the node pays NodeConstructionCost (copper from the treasury, items from the stores; bFree:
//                nothing) and the plot is UnderConstruction for ConstructionPhaseDuration (the players' phase; a building that cannot time out waits forever).
//    giving      players give the items of PlayerConstructionCost (the construction crates); an entry that names more items takes any of them.  When all is given the
//                contribution stages run (BuildingContributionPhasesId: each stage's InterpolationTime, 60 s for the crates' phases) and the building is
//                Operational; CompletionNodeExperience goes to the node (the NodeXP projects).  bSkipPlayerPhase: the players' cost counts as given.  A players'
//                phase that runs out first (CanConstructionTimeOut) leaves the plot Empty; the node's cost is not given back [modelled].
//    points      an Operational building's NodeModifications (and its finished expansions') grant node skill points (the five support buildings: 1 each of their
//                kind); the expansions bought spend them.  The summary shows what is left of each node kind.
//    expansions  the building's ExpansionSkillTree: the mayor's ServerPurchaseExpansion names a rank.  The node must be at the tree node's LevelRequirement, the
//                tree must hold PointRequirement ranks bought already, its SkillsRequired (all, or any one) bought and its MutuallyExclusiveSkills not; the node
//                pays the rank's SkillCost (skill points, commodities from the stores, copper; bFree: nothing).  Then the expansion's phases: the citizens'
//                vote (VoteDuration: 2 days for the support buildings' expansions, none for the workshops'; it passes unless more vote no), the players'
//                phase (PostInitiationPlayerConstructionCostId, PlayerConstructionPhaseDuration; bSkipPlayerPhase skips it and the vote), the construction
//                (ConstructionDuration) and Finished.  A players' phase that runs out ends the project (its costs are
//                not given back, its points are) [modelled].  ServerCancelExpansionPurchase ends a project before its construction.
//    removal     ServerRemoveExpansion: a vote of RemovalVoteDuration (2 days) that the node's citizens vote on (ServerVoteOnExpansion); it passes unless more
//                vote no than yes [modelled: the data's VoteResultCurve is empty]; an expansion that another bought one needs cannot go.
//                ServerCancelExpansionRemoval ends the vote.
//    demolition  ServerCommenceDemolition: an Operational (or Disarray) building the mayor bought or the seed placed (not the civic core), whose skill points no
//                expansion spends: Demolition for DemolitionPhaseDuration, then Empty.  ServerCancelDemolition before that.  DemolitionVoteDuration is 0 in
//                the data, so there is no vote.
//    upkeep      every week [modelled: the data has no period; the summary always showed a week] each Operational building's MaintenanceCost and its
//                expansions' AddedMaintenance are taken from the treasury and the stores, in the plots' order; the world's own buildings (the data's and the
//                seed's: the civic core, Joeva's Factories and Farm) pay no MaintenanceCost of their own [modelled: no node starts with the T2 commodities a
//                Factory's upkeep asks for, so they would fall apart a week after the start], their expansions' they do; a building the node cannot pay for falls into
//                Disarray (no services, no points, no effects).  At the next upkeep a building in Disarray whose upkeep and DisarrayRepairCost the node can pay is
//                Repairing for RepairPhaseDuration, then Operational [modelled: the client's Repair button sends nothing in this build].
//    income      every hour [modelled: the data gives no period] the node effects of the running modifications (running_mods) give the node's stores their
//                CommoditiesToGiveNode: a Factory 1 each of three commodities (168 a week against an upkeep of 60).  The world does this (it has the effects).
// ============================================================================
#pragma once

#include "world/node_buildings_table.h"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace aoc::worldd::world {

struct NodeElection;

/// EBuildingState and EAoCExpansionState / EAoCExpansionStateType (the usmap).
enum : uint8_t { kPlotEmpty = 0, kPlotUnderConstruction = 1, kPlotOperational = 2, kPlotDisarray = 3, kPlotDamaged = 4, kPlotRepairing = 5, kPlotDestroyed = 6,
                 kPlotDemolition = 7 };
enum : uint8_t { kExpNone = 0, kExpPreInitiation = 1, kExpVote = 2, kExpVoteDelay = 3, kExpPlayerContribution = 4, kExpPostPlayerContribution = 5,
                 kExpConstruction = 6, kExpRemovalPreInitiation = 7, kExpRemovalVote = 8, kExpRemovalVoteDelay = 9, kExpFinished = 10 };
enum : uint8_t { kExpTypeNone = 0, kExpTypePurchase = 1, kExpTypeRemoval = 2 };

/// What happened to a plot (it overrides the data's row).
struct PlotState {
    int64_t building = 0;                       // 0: the plot's empty building (an Empty plot)
    uint8_t state = kPlotEmpty;                 // EBuildingState
    int64_t phase_started_ms = 0, phase_ends_ms = 0;   // the state's phase (0: none, an Operational building)
    std::map<int64_t, int32_t> given;           // the players' cost given so far: an entry's (first) item -> quantity
    int64_t complete_ms = 0;                    // all was given: Operational at this time (the contribution stages); 0 while something is missing
    int64_t built_ms = 0;                       // ConstructionDate: when it became Operational
    std::vector<int64_t> ranks;                 // the expansions bought (their skill ranks), in order
    // the expansion project
    uint8_t exp_state = kExpNone, exp_type = kExpTypeNone;
    int64_t exp_rank = 0;
    int64_t exp_started_ms = 0, exp_ends_ms = 0;
    std::map<int64_t, int32_t> exp_given;
    std::map<int64_t, bool> votes;              // account -> yes (the expansion's vote or removal vote)
    bool operator==(const PlotState&) const = default;
};

struct NodeBuildings {
    std::map<int64_t, PlotState> plots;         // plot -> what happened to it
    int64_t next_upkeep_ms = 0;                 // the next upkeep (0: not scheduled yet)
    int64_t next_income_ms = 0;                 // the next hour's income of the buildings' node effects (0: not scheduled yet)
    bool operator==(const NodeBuildings&) const = default;
};

/// A plot of the node at its level as the data has it (the world fills it from NodeWire::data_plot_rows_of).
struct PlotInfo {
    int64_t plot = 0;
    int64_t building = 0;          // the data's building (an Empty plot: its empty building)
    uint8_t state = kPlotEmpty;
    int64_t empty_building = 0;    // ServiceBuildingPlotRecordBase.EmptyBuildingId
    bool    pre_construct = false; // the civic core, the NodeXP projects (and the seed's workshops)
    bool    seeded = false;        // a workshop the seed placed (it may be demolished, as the mayor's)
    std::vector<int64_t> candidates;   // what may stand on it (service_plot_building)
};

namespace node_buildings {

using CostList = std::vector<std::pair<int64_t, int32_t>>;   // item -> quantity

const Building*  building(int64_t guid);
const Cost*      cost(int64_t guid);
const Expansion* expansion(int64_t guid);
const Phases*    phases(int64_t guid);
const Mod*       mod(int64_t guid);
/// The tree node whose rank `rank` is (null: none); the tree node `node`.
const TreeNode*  rank_node(int64_t rank);
const TreeNode*  tree_node(int64_t node);
/// The skill point type's record name ("Node_Fabrication"); "" for none.
std::string      point_type_name(int64_t type);
/// The five node kinds of skill points (Node_Vocational, _Fabrication, _Civic, _Cultural, _Scholarly), in the data's order.
const std::vector<int64_t>& node_point_types();

/// A cost's items, with `scale` (the upkeep's proration) rounded up.
CostList cost_items(int64_t cost, double scale = 1.0);
int64_t  cost_copper(int64_t cost, double scale = 1.0);
/// The contribution stages' interpolation, in all (ms).
int64_t  interpolation_ms(int64_t phases_guid);

/// A building the mayor may buy from the Building Manager: it shows there and it costs something.
bool buyable(const Building& b);

/// The plot as it is now: the live state when something happened to it, else the data's row.
struct PlotNow {
    int64_t          building = 0;
    uint8_t          state = kPlotEmpty;
    const PlotState* live = nullptr;
};
PlotNow now_of(const NodeBuildings& b, const PlotInfo& p);

/// The node's money and stores can pay `cost` (copper and items, times `scale`).
bool can_pay(const NodeElection& n, int64_t cost, double scale = 1.0);
/// Takes `cost` from the node (as much as there is: the caller checked); the copper it took.
int64_t pay(NodeElection& n, int64_t cost, double scale = 1.0);

/// The skill points of each kind the node has left now: what its Operational buildings and their finished expansions grant, less what the expansions bought or
/// being bought spend (a kind with nothing granted or spent is left out).
std::map<int64_t, int32_t> skill_points(const NodeBuildings& b, const std::vector<PlotInfo>& plots);
/// What a plot's Operational building and its expansions grant (type -> points).
std::map<int64_t, int32_t> granted_by(const PlotState* live, int64_t building);
/// The node modifications that run in the node now: those of its Operational buildings and their finished expansions.
std::vector<int64_t> running_mods(const NodeBuildings& b, const std::vector<PlotInfo>& plots);

enum class Result : uint8_t {
    Ok, NoPlot, NotEmpty, NotCandidate, NotForSale, NoMoney, NotOperational, Busy, NoTree, NoRank, LevelTooLow, NeedsPoints, NeedsPrerequisite, Excluded,
    AlreadyBought, NoSkillPoints, NotBought, NeededByAnother, NoProject, NotVoting, AlreadyVoted, PointsInUse, CannotDemolish, NotDemolishing,
    NotUnderConstruction, NothingNeeded, BadState
};
const char* result_name(Result r);

/// ServerPurchaseServiceBuilding.  `free` (bFree): the node pays nothing; `skip_players` (bSkipPlayerPhase): the players' cost counts as given.
Result purchase(NodeElection& n, const PlotInfo& p, int64_t building, int64_t now_ms, bool free = false, bool skip_players = false);

/// What the plot still needs from the players (an entry's first item -> quantity): its construction's players' cost, or its expansion's in the players'
/// phase.  Empty: nothing (or nothing to give to).
CostList needs(const NodeBuildings& b, const PlotInfo& p);
/// The players' cost the plot asks for now and what was given towards it, entry by entry (item, given, asked): the contribution window's progress.
struct NeedRow {
    int64_t item = 0;
    int32_t given = 0, asked = 0;
};
std::vector<NeedRow> progress(const NodeBuildings& b, const PlotInfo& p);

/// A player offers `offer` (item -> how many it has): the plot takes what it still needs.  What it took (item as offered -> quantity).  The construction's last
/// item starts the contribution stages; the expansion's starts its construction.
CostList contribute(NodeElection& n, const PlotInfo& p, const CostList& offer, int64_t now_ms);

/// ServerPurchaseExpansion: the expansion of `rank` for the plot's building.  `level` is the node's level.
Result purchase_expansion(NodeElection& n, const std::vector<PlotInfo>& plots, const PlotInfo& p, int64_t rank, int level, int64_t now_ms, bool free = false,
                          bool skip_players = false);
/// ServerCancelExpansionPurchase: a purchase not yet in its construction ends (its costs are not given back, its points are).
Result cancel_expansion_purchase(NodeElection& n, const PlotInfo& p);
/// ServerRemoveExpansion: the vote to remove the expansion of `rank` starts (a removal with no vote removes it now).
Result remove_expansion(NodeElection& n, const PlotInfo& p, int64_t rank, int64_t now_ms);
/// ServerCancelExpansionRemoval.
Result cancel_expansion_removal(NodeElection& n, const PlotInfo& p);
/// ServerVoteOnExpansion by a citizen's account: once per vote.
Result vote(NodeElection& n, const PlotInfo& p, int64_t account, bool yes);

/// ServerCommenceDemolition / ServerCancelDemolition.
Result demolish(NodeElection& n, const std::vector<PlotInfo>& plots, const PlotInfo& p, int64_t now_ms);
Result cancel_demolition(NodeElection& n, const PlotInfo& p);

/// A game master's ServerSetPlotState: the plot holds `building` (0: its empty building) in `state` now, its projects and gifts dropped.
Result set_state(NodeElection& n, const PlotInfo& p, int64_t building, uint8_t state, int64_t now_ms);
/// A game master's ServerGMAdvanceServiceBuildingPhaseEndTime: the plot's running phase (its construction, demolition, repair or expansion project) ends `seconds`
/// from now.
Result set_phase_end(NodeElection& n, const PlotInfo& p, int64_t now_ms, int64_t seconds);

/// What advance() did.
struct Event {
    enum Kind : uint8_t { Built, TimedOut, ExpansionStarted, ExpansionBuilt, ExpansionFailed, ExpansionRemoved, ExpansionKept, Demolished, Disarray, Repairing,
                          Repaired, Upkeep };
    Kind    kind = Built;
    int64_t plot = 0, building = 0, expansion = 0;
    double  node_xp = 0;      // Built: the building's CompletionNodeExperience
    int64_t copper = 0;       // Upkeep: the copper it took from the treasury
};
const char* event_name(Event::Kind k);

/// Brings the node's plots to `now_ms`: constructions done or timed out, expansion phases, removal votes, demolitions, repairs, and the upkeep once a week
/// (`force_upkeep`: now, with its costs times `upkeep_scale`, a game master's ServerForceBuildingMaintainance).  What happened, in order.
std::vector<Event> advance(NodeElection& n, const std::vector<PlotInfo>& plots, int64_t now_ms, bool force_upkeep = false, double upkeep_scale = 1.0);

/// The week between two upkeeps, and the hour between two incomes [modelled].
inline constexpr int64_t kUpkeepPeriodMs = 7ll * 24 * 3600 * 1000;
inline constexpr int64_t kIncomePeriodMs = 3600ll * 1000;

/// The hours of income due by `now_ms` (at most `cap`, after a long stop), and the next one scheduled: 0 the first time (it schedules the first hour).
int income_due(NodeBuildings& b, int64_t now_ms, int cap = 24);

/// elections.json's form of a node's buildings.
nlohmann::json to_json(const NodeBuildings& b);
NodeBuildings  from_json(const nlohmann::json& j);

}  // namespace node_buildings
}  // namespace aoc::worldd::world
