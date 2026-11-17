// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_node_buildings.cpp  (Nodes 3: the service buildings and the node's skill points)
//
//  The client's side of world/node_buildings.h (the rules) and the world's clock over it.
//
//    the Building Manager   the mayor's node window (UNodeBuildingManagerMenu::HandleBuildingActionConfirmed, read in Ghidra): Construct sends
//                           ServerPurchaseServiceBuilding(node, plot, building, false, false); Upgrade ServerPurchaseExpansion(node, plot, rank, false, false); Cancel Upgrade
//                           ServerCancelExpansionPurchase; Downgrade ServerRemoveExpansion; Cancel Downgrade ServerCancelExpansionRemoval; Demolish
//                           ServerCommenceDemolition(plot); Cancel Demolition ServerCancelDemolition(plot).  Repair, Clear Rubble and Cancel Construction send nothing in
//                           this build.  The window reads the plots from ClientReceiveNodeSummary (FAoCNodePlotOverview: the times, gifts, expansions, votes) and the
//                           node's skill points (NodeSkillPoints): after a change the actor is sent the node's summary again.
//    the Construction Foreman  the Warehouse's Node_Manager_Foreman (dialogue zLegacy_DD_ServiceNPC_ConstructionForeman): its greeting takes the line that runs
//                           Construction_Constribution_AR (EActionResponse::OpenBuildingContributionWindow; dialogue/talk.h kContributionWindowNodes).  This build places
//                           no Construction Manager at a site (ServiceBuilding_L3_Generic_Construction is empty) and no crate drop-off, so the Foreman takes the crates:
//                           the crate on the character's back goes to the node's site that needs it [modelled], and the window opens
//                           (ClientShowBuildingContributionWindow(NPC, node, plot, building, end, the players' cost record): the HUD's menu).  The window's
//                           ServerGetBuildingConstructionProgress is answered with ClientReceiveBuildingConstructionProgress; its ServerFulfillServiceBuildingConstruction
//                           (sent by the window's Blueprint: no native sender) gives what the character carries, the bags too.  Each gift is told to the giver
//                           (Client_SendConstructionContributionItemFeed) and to the quests (construction_deposit: CSQ_0156's "Contribute").
//    votes                  ServerVoteOnExpansion by the node's citizens, once per account (an expansion's removal vote; the purchase votes are 0 s in the data).
//                           ServerVoteOnDemolition: the data's demolitions have no vote, so it is refused.
//    the world              PlayerWorld::tick_buildings (every 5 s with the elections): node_buildings::advance, the hourly income of the running modifications' node
//                           effects (CommoditiesToGiveNode), the weekly upkeep, the node XP of a project built (CompletionNodeExperience, as it is: no rate), the
//                           node's feed (ENodeSystemNotificationType 1 to 5), then show_node_plots: CityNodes::set_live_plots (the summary, the scenery and the
//                           town's people follow) and the boons' modifications (building_mods).
//    game masters           ServerGMRequestServiceBuildingPlotData (ClientGMReceiveServiceBuildingPlotData), ServerGMRequestNodeSkillPointData
//                           (ClientGMReceiveNodeSkillPointData), ServerGMAdvanceServiceBuildingPhaseEndTime, ServerContributeToServiceBuildingConstructionCheat,
//                           ServerForceBuildingMaintainance (its ProratePercent scales the upkeep) and ServerSetPlotState; a game master's purchase honours bFree and
//                           bSkipPlayerPhase (a mayor's does not).
// ============================================================================
#include "session/player_session.h"

#include "session/player_session_util.h"
#include "session/rpc_table.h"

#include "world/elections.h"
#include "world/node_buildings.h"
#include "world/node_rewards.h"
#include "world/node_wire.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;
namespace schema = ::aoc::protocol::schema;
namespace nb = world::node_buildings;

namespace {

/// ENodeSystemNotificationType for the node's feed.
constexpr uint8_t kNewsConstructionStarted = 1, kNewsStateAdvanced = 2, kNewsExpansion = 3, kNewsExpansionAdvanced = 4, kNewsExpansionFinished = 5;

const repl::Value* nparam(const repl::ClientRpc& rpc, std::string_view path) {
    return rpc.call.decoded && rpc.call.rpc ? repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, path) : nullptr;
}

int64_t nint(const repl::ClientRpc& rpc, std::string_view path) {
    const repl::Value* v = nparam(rpc, path);
    if (!v) return 0;
    if (const int64_t* i = v->get_if<int64_t>()) return *i;
    if (const uint64_t* u = v->get_if<uint64_t>()) return static_cast<int64_t>(*u);
    if (const bool* b = v->get_if<bool>()) return *b ? 1 : 0;
    return 0;
}

bool nbool(const repl::ClientRpc& rpc, std::string_view path) { return nint(rpc, path) != 0; }

std::string nstr(const repl::ClientRpc& rpc, std::string_view path) {
    const repl::Value* v = nparam(rpc, path);
    const std::string* s = v ? v->get_if<std::string>() : nullptr;
    return s ? *s : std::string();
}

const world::PlotInfo* plot_in(const std::vector<world::PlotInfo>& plots, int64_t plot) {
    for (const world::PlotInfo& p : plots) {
        if (p.plot == plot) return &p;
    }
    return nullptr;
}

std::string building_label(int64_t b) {
    const nb::Building* x = nb::building(b);
    return x ? x->name : std::to_string(b);
}

std::string expansion_label(int64_t rank) {
    const nb::TreeNode* t = nb::rank_node(rank);
    return t ? t->name : std::to_string(rank);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

uint8_t news_state_of(nb::Event::Kind k) {
    switch (k) {
        case nb::Event::Built: case nb::Event::Repaired: return world::kPlotOperational;
        case nb::Event::Disarray: return world::kPlotDisarray;
        case nb::Event::Repairing: return world::kPlotRepairing;
        default: return world::kPlotEmpty;
    }
}

}  // namespace

// ---- the world ------------------------------------------------------------------------------------------------------------------------------------------------------

std::vector<world::PlotInfo> PlayerWorld::node_plots(int64_t node) const {
    if (const auto it = plots_for_test_.find(node); it != plots_for_test_.end()) return it->second;
    if (!node_wire_ || !city_nodes_ || node == 0) return {};
    return node_wire_->plot_infos(node, city_nodes_->level_of(node));
}

const std::vector<int64_t>& PlayerWorld::building_mods(int64_t node) const {
    static const std::vector<int64_t> kNone;
    if (const auto t = mods_for_test_.find(node); t != mods_for_test_.end()) return t->second;
    const auto it = building_mods_.find(node);
    return it == building_mods_.end() ? kNone : it->second;
}

void PlayerWorld::show_node_plots(int64_t node) const {
    if (!city_nodes_ || node == 0) return;
    const world::NodeElection* e = elections_ ? elections_->node(node) : nullptr;
    static const world::NodeBuildings kNoBuildings;
    const world::NodeBuildings& b = e ? e->buildings : kNoBuildings;
    std::vector<world::CityNodes::PlotLive> live;
    for (const auto& [plot, s] : b.plots) {
        world::CityNodes::PlotLive l;
        l.plot = plot;
        l.building = s.building;
        l.state = s.state;
        for (const int64_t rank : s.ranks) {   // the expansions built: their asset sets (the last wins) and building spawns
            const nb::TreeNode* t = nb::rank_node(rank);
            const nb::Expansion* ex = t ? nb::expansion(t->expansion) : nullptr;
            if (!ex) continue;
            if (ex->asset_sets != 0) {
                l.asset_sets = ex->asset_sets;
                l.replace_assets = ex->replace_building;
            }
            if (ex->pop_spawn != 0) l.spawns.emplace_back(ex->pop_spawn, ex->replace_spawn);
        }
        live.push_back(std::move(l));
    }
    city_nodes_->set_live_plots(node, std::move(live));
    building_mods_[node] = nb::running_mods(b, node_plots(node));
}

void PlayerWorld::tick_buildings(int64_t now_ms) const {
    if (!elections_ || !city_nodes_) return;
    for (const world::CityNodeInfo& info : city_nodes_->nodes()) {
        const int64_t node = info.guid;
        const bool data = node_wire_ && node_wire_->is_real_node(node) && node_wire_->layout_of(node) != 0;
        if (!data && !plots_for_test_.count(node)) continue;
        const std::vector<world::PlotInfo> plots = node_plots(node);
        std::vector<nb::Event> events;
        int64_t income = 0;
        int hours = 0;
        elections_->change_node(node, [&](world::NodeElection& e) {
            const world::NodeBuildings before = e.buildings;
            events = nb::advance(e, plots, now_ms);
            hours = nb::income_due(e.buildings, now_ms);
            if (hours > 0 && node_rewards_) {   // the hours' income of the running modifications' node effects
                for (const int64_t mod : nb::running_mods(e.buildings, plots)) {
                    for (const world::NodeCommodityGift& g : node_rewards_->commodities_of(mod)) {
                        const int64_t item = world::commodity_item(g.commodity);
                        if (item == 0 || g.quantity <= 0) continue;
                        const int64_t add = int64_t{g.quantity} * hours;
                        int32_t& have = e.stored[item];
                        have = static_cast<int32_t>(std::min<int64_t>(int64_t{have} + add, INT32_MAX));
                        income += add;
                    }
                }
            }
            return !(e.buildings == before) || income > 0;
        });
        if (income > 0) spdlog::info("[nodes] {}: its buildings' income: {} commodity unit(s) to the stores ({} hour(s))", info.display_name, income, hours);
        const world::NodeElection* e = elections_->node(node);
        const std::string mayor = e ? e->mayor.name : std::string();
        for (const nb::Event& ev : events) {
            ++building_events_;
            spdlog::info("[nodes] {}: plot {}: {} {}{}", info.display_name, ev.plot, ev.expansion != 0 ? "the expansion " + std::to_string(ev.expansion) + " of" : std::string("the"),
                         ev.kind == nb::Event::Upkeep ? "upkeep" : building_label(ev.building) + " " + nb::event_name(ev.kind),
                         ev.kind == nb::Event::Upkeep ? ": " + std::to_string(ev.copper) + " copper from the treasury" : std::string());
            if (ev.kind == nb::Event::Built && ev.node_xp >= 1) {
                add_node_experience(node, static_cast<int64_t>(std::llround(ev.node_xp)), "a construction project built", /*rated=*/false);
                spdlog::info("[nodes] {}: {} gives the node {} XP", info.display_name, building_label(ev.building), static_cast<int64_t>(std::llround(ev.node_xp)));
            }
            uint8_t type = 0;
            switch (ev.kind) {
                case nb::Event::Built: case nb::Event::TimedOut: case nb::Event::Demolished: case nb::Event::Disarray: case nb::Event::Repairing: case nb::Event::Repaired:
                    type = kNewsStateAdvanced;
                    break;
                case nb::Event::ExpansionBuilt: type = kNewsExpansionFinished; break;
                case nb::Event::ExpansionStarted: case nb::Event::ExpansionFailed: case nb::Event::ExpansionRemoved: case nb::Event::ExpansionKept: type = kNewsExpansionAdvanced; break;
                case nb::Event::Upkeep: break;
            }
            if (type != 0) elections_->add_news(node, world::NodeNews{type, now_ms, mayor, ev.plot, ev.building, 0, news_state_of(ev.kind)});
        }
        show_node_plots(node);
    }
}

// ---- the session ----------------------------------------------------------------------------------------------------------------------------------------------------

int64_t PlayerSession::node_of_plot(int64_t plot) const {
    if (!world_ || plot == 0) return 0;
    const GoodPosition at = checker_.good();
    std::vector<int64_t> nodes;
    if (const int64_t here = citizenship_node_at(at.x, at.y).first) nodes.push_back(here);
    if (citizenship_.node_guid != 0) nodes.push_back(citizenship_.node_guid);
    if (const world::Elections* el = world_->elections()) {
        if (const int64_t m = el->mayor_node(account_id_)) nodes.insert(nodes.begin(), m);
    }
    for (const int64_t n : nodes) {
        if (plot_in(world_->node_plots(n), plot)) return n;
    }
    return 0;
}

bool PlayerSession::may_build(Connection& conn, int64_t node, const char* rpc, bool& gm, Clock::time_point now) {
    gm = false;
    world::Elections* el = world_ ? world_->elections() : nullptr;
    if (!el || node == 0) {
        spdlog::info("[nodes] {}: {} is refused: {}", name(), rpc, el ? "it names no node" : "this world keeps no node buildings");
        return false;
    }
    if (world_->gm_commands()) gm = gm_rpc_admitted(rpc, "node " + std::to_string(node), now);   // (a game master who is the mayor too may use the GM flags)
    if (el->mayor_node(account_id_) == node || gm) return true;
    ++building_refusals_;
    spdlog::info("[nodes] {}: {} for node {} is refused: the account is not its mayor", name(), rpc, node);
    send_must_be_mayor(conn);
    return false;
}

void PlayerSession::building_notice(Connection& conn, const std::string& text) {
    receive_chat_line(conn, text, 4, "");
}

void PlayerSession::buildings_changed(Connection& conn, int64_t node) {
    if (!world_) return;
    world_->save_elections();
    world_->show_node_plots(node);
    note_node_request(node_summary_wait_, node_summary_queued_, node, "a building changed");
    send_node_inventory(conn, node);
    node_boons_dirty_ = true;
}

void PlayerSession::on_building_purchase(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!within_budget(building_budget_, "ServerPurchaseServiceBuilding", now)) return;
    const int64_t node = nint(rpc, "NodeId"), plot = nint(rpc, "PlotId"), building = nint(rpc, "BuildingId");
    bool gm = false;
    if (!may_build(conn, node, "ServerPurchaseServiceBuilding", gm, now)) return;
    const bool free = gm && nbool(rpc, "bFree"), skip = gm && nbool(rpc, "bSkipPlayerPhase");
    const std::vector<world::PlotInfo> plots = world_->node_plots(node);
    const world::PlotInfo* p = plot_in(plots, plot);
    nb::Result r = nb::Result::NoPlot;
    int64_t left = 0;
    if (p) {
        world_->elections()->change_node(node, [&](world::NodeElection& e) {
            r = nb::purchase(e, *p, building, wall_ms(), free, skip);
            left = e.treasury;
            return r == nb::Result::Ok;
        });
    }
    if (r != nb::Result::Ok) {
        ++building_refusals_;
        spdlog::info("[nodes] {}: the {} of plot {} of node {} is not bought: {}", name(), building_label(building), plot, node, nb::result_name(r));
        building_notice(conn, std::string("The building cannot be bought: ") + nb::result_name(r) + ".");
        note_node_request(node_summary_wait_, node_summary_queued_, node, "a refused purchase");
        return;
    }
    ++buildings_bought_;
    world_->elections()->add_news(node, world::NodeNews{kNewsConstructionStarted, wall_ms(), name(), plot, building, 0, world::kPlotUnderConstruction});
    const nb::Building* bd = nb::building(building);
    spdlog::info("[nodes] {}: {} buys the {} for plot {} of node {}{}{}: under construction for {} h; the treasury has {} copper left", name(), gm ? "a game master" : "the mayor",
                 building_label(building), plot, node, free ? " (free)" : "", skip ? " (the players' phase skipped)" : "", bd ? bd->construction_ms / 3600000 : 0, left);
    buildings_changed(conn, node);
}

void PlayerSession::send_building_progress(Connection& conn, int64_t node, int64_t plot) {
    const world::Elections* el = world_ ? world_->elections() : nullptr;
    if (!el) return;
    const std::vector<world::PlotInfo> plots = world_->node_plots(node);
    const world::PlotInfo* p = plot_in(plots, plot);
    if (!p) return;
    static const world::NodeBuildings kNoBuildings;
    const world::NodeElection* e = el->node(node);
    const world::NodeBuildings& b = e ? e->buildings : kNoBuildings;
    const nb::PlotNow now = nb::now_of(b, *p);
    const std::vector<nb::NeedRow> rows = nb::progress(b, *p);
    int64_t state_end = 0, project_end = 0, expansion = 0, slot = 0;
    uint8_t exp_state = world::kExpNone;
    if (now.live) {
        if (now.state == world::kPlotUnderConstruction) {
            state_end = (now.live->complete_ms != 0 ? now.live->complete_ms : now.live->phase_ends_ms) / 1000;
            project_end = now.live->phase_ends_ms / 1000;
        } else if (now.state == world::kPlotDemolition || now.state == world::kPlotRepairing) {
            state_end = now.live->phase_ends_ms / 1000;
        }
        exp_state = now.live->exp_state;
        if (const nb::TreeNode* t = nb::rank_node(now.live->exp_rank)) {
            expansion = t->expansion;
            slot = t->node;
        }
        if (exp_state != world::kExpNone) project_end = now.live->exp_ends_ms / 1000;
    }
    const int64_t building = now.building;
    const uint8_t state = now.state;
    ++building_progress_sent_;
    send_citizenship_call(conn, "ClientReceiveBuildingConstructionProgress", [=](const schema::RpcSchema& r, repl::RpcParams& par) {
        const schema::RepCommand* list = repl::rpc_command(r, "Progress");
        if (!list) return false;
        const auto elems = repl::rpcf::elements_of(r, *list);
        const uint16_t h_res = repl::rpcf::local(elems, "Progress.Resource"), h_q = repl::rpcf::local(elems, "Progress.Quantity");
        if (!h_res || !h_q) return false;
        repl::ArrayValue progress;
        for (const nb::NeedRow& row : rows) {
            repl::ElementState x;
            x.values[h_res] = repl::Value::i64(row.item);
            x.values[h_q] = repl::Value::u64(static_cast<uint64_t>(std::max(0, row.given)));
            progress.elements.push_back(std::move(x));
        }
        (void)building;
        return repl::set_rpc_param(r, par, "Node", repl::Value::i64(node)) && repl::set_rpc_param(r, par, "Plot", repl::Value::i64(plot)) &&
               repl::set_rpc_param(r, par, "BuildingState", repl::Value::u64(state)) && repl::set_rpc_param(r, par, "BuildStateTimeEnd", repl::Value::i64(state_end)) &&
               repl::set_rpc_param(r, par, "ProjectTimeEnd", repl::Value::i64(project_end)) && repl::set_rpc_param(r, par, "ExpansionSlot", repl::Value::i64(slot)) &&
               repl::set_rpc_param(r, par, "Expansion", repl::Value::i64(expansion)) && repl::set_rpc_param(r, par, "ExpansionState", repl::Value::u64(exp_state)) &&
               repl::set_rpc_param(r, par, "Progress", repl::Value(std::move(progress)));
    });
}

void PlayerSession::on_building_progress(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!within_budget(building_budget_, "ServerGetBuildingConstructionProgress", now)) return;
    const int64_t node = nint(rpc, "Node"), plot = nint(rpc, "Plot");
    if (node == 0 || plot == 0) return;
    send_building_progress(conn, node, plot);
}

int64_t PlayerSession::contribution_plot_of(int64_t node) const {
    const world::Elections* el = world_ ? world_->elections() : nullptr;
    if (!el) return 0;
    static const world::NodeBuildings kNoBuildings;
    const world::NodeElection* e = el->node(node);
    const world::NodeBuildings& b = e ? e->buildings : kNoBuildings;
    const svc::InventoryItem* crate = inventory_.at(svc::ItemContainer::Equipment, items::equip::CargoHold);
    int64_t best = 0, best_end = INT64_MAX;
    bool best_takes_crate = false;
    for (const world::PlotInfo& p : world_->node_plots(node)) {
        const nb::CostList need = nb::needs(b, p);
        if (need.empty()) continue;
        bool takes = false;
        if (crate) {
            for (const auto& [item, q] : need) takes = takes || item == crate->item_record_id;
        }
        const nb::PlotNow now = nb::now_of(b, p);
        int64_t end = INT64_MAX - 1;
        if (now.live) {
            const int64_t e2 = now.live->exp_state == world::kExpPlayerContribution ? now.live->exp_ends_ms : now.live->phase_ends_ms;
            if (e2 != 0) end = e2;
        }
        if (best == 0 || (takes && !best_takes_crate) || (takes == best_takes_crate && end < best_end)) {
            best = p.plot;
            best_end = end;
            best_takes_crate = takes;
        }
    }
    return best;
}

int64_t PlayerSession::contribute_carried(Connection& conn, int64_t node, int64_t plot, bool bag, Clock::time_point now) {
    world::Elections* el = world_ ? world_->elections() : nullptr;
    if (!el || node == 0 || plot == 0 || dead_) return 0;
    const std::vector<world::PlotInfo> plots = world_->node_plots(node);
    const world::PlotInfo* p = plot_in(plots, plot);
    if (!p) return 0;
    // what the character offers: the crate on its back, and with `bag` what its bags hold of what the plot needs
    nb::CostList offer;
    const svc::InventoryItem* crate = inventory_.at(svc::ItemContainer::Equipment, items::equip::CargoHold);
    const std::string crate_guid = crate ? crate->instance_guid : std::string();
    const int64_t crate_item = crate ? crate->item_record_id : 0;
    if (crate) offer.emplace_back(crate_item, 1);
    if (bag) {
        static const world::NodeBuildings kNoBuildings;
        const world::NodeElection* e = el->node(node);
        for (const auto& [item, q] : nb::needs(e ? e->buildings : kNoBuildings, *p)) {
            const int64_t have = inventory_.count_of(item) - (item == crate_item ? 1 : 0);
            if (have > 0) offer.emplace_back(item, static_cast<int32_t>(std::min<int64_t>(have, q)));
        }
    }
    if (offer.empty()) return 0;
    nb::CostList taken;
    int64_t building = 0;
    el->change_node(node, [&](world::NodeElection& e) {
        taken = nb::contribute(e, *p, offer, wall_ms());
        building = nb::now_of(e.buildings, *p).building;
        return !taken.empty();
    });
    if (taken.empty()) return 0;
    // the items leave the character: the crate on its back first, the rest from the bags
    std::vector<std::pair<int64_t, int64_t>> from_bags;
    bool crate_taken = false;
    int64_t units = 0;
    for (const auto& [item, q] : taken) {
        units += q;
        int64_t rest = q;
        if (crate && item == crate_item && !crate_taken) {
            items::Outcome o = inventory_.remove_cargo(crate_guid);
            if (o.ok) {
                apply_outcome(conn, o, "a crate was given to a construction");
                crate_taken = true;
                --rest;
            }
        }
        if (rest > 0) from_bags.emplace_back(item, rest);
    }
    if (!from_bags.empty()) take_quest_items(conn, from_bags);
    contribution_units_ += units;
    for (const auto& [item, q] : taken) {
        construction_deposit(node, building, item, q);   // the quests (CSQ_0156's "Contribute")
        send_citizenship_call(conn, "Client_SendConstructionContributionItemFeed", [node, plot, building, item = item](const schema::RpcSchema& r, repl::RpcParams& par) {
            return repl::set_rpc_param(r, par, "NodeId", repl::Value::i64(node)) && repl::set_rpc_param(r, par, "PlotId", repl::Value::i64(plot)) &&
                   repl::set_rpc_param(r, par, "BuildingRecordId", repl::Value::i64(building)) && repl::set_rpc_param(r, par, "ContributedItemRecordGuid", repl::Value::i64(item));
        });
    }
    const world::NodeElection* e = el->node(node);
    const bool done = e && nb::needs(e->buildings, *p).empty();
    spdlog::info("[nodes] {}: gives {} unit(s) to the {} of plot {} of node {}{}", name(), units, building_label(building), plot, node,
                 done ? ": it has all it needs and is being finished" : "");
    building_notice(conn, std::to_string(units) + (units == 1 ? " crate was" : " crates were") + " given to the construction of the " + building_label(building) +
                              (done ? ". It has all it needs and is being finished." : "."));
    buildings_changed(conn, node);
    send_building_progress(conn, node, plot);
    (void)now;
    return units;
}

void PlayerSession::open_contribution_window(Connection& conn, const world::WorldActor& npc, Clock::time_point now) {
    if (!world_) return;
    const int64_t node = citizenship_node_at(npc.x, npc.y).first;
    if (node == 0) {
        spdlog::info("[nodes] {}: {} opens the building contributions but stands in no node", name(), npc.npc.name);
        return;
    }
    int64_t plot = contribution_plot_of(node);
    if (plot == 0) {
        spdlog::info("[nodes] {}: {} has no construction that needs players' gifts in node {}", name(), npc.npc.name, node);
        building_notice(conn, "Nothing in this node is waiting for construction crates.");
        return;
    }
    contribute_carried(conn, node, plot, /*bag=*/false, now);   // the crate on the back [modelled: this build has no drop-off at the site]
    if (const int64_t next = contribution_plot_of(node)) plot = next;   // (that site may have all it needs now)
    const world::Elections* el = world_->elections();
    static const world::NodeBuildings kNoBuildings;
    const world::NodeElection* e = el ? el->node(node) : nullptr;
    const world::NodeBuildings& b = e ? e->buildings : kNoBuildings;
    const std::vector<world::PlotInfo> plots = world_->node_plots(node);
    const world::PlotInfo* p = plot_in(plots, plot);
    if (!p) return;
    const nb::PlotNow pn = nb::now_of(b, *p);
    int64_t cost = 0, end = 0;
    if (pn.live && pn.live->exp_state == world::kExpPlayerContribution) {
        const nb::TreeNode* t = nb::rank_node(pn.live->exp_rank);
        const nb::Expansion* ex = t ? nb::expansion(t->expansion) : nullptr;
        cost = ex ? ex->player_cost : 0;
        end = pn.live->exp_ends_ms / 1000;
    } else {
        const nb::Building* bd = nb::building(pn.building);
        cost = bd ? bd->player_cost : 0;
        end = pn.live ? pn.live->phase_ends_ms / 1000 : 0;
    }
    contribution_node_ = node;
    contribution_plot_ = plot;
    ++contribution_windows_;
    const FIntrepidNetworkGUID guid = npc.actor.guid;
    const int64_t building = pn.building;
    spdlog::info("[nodes] {}: {} opens the contribution window for the {} of plot {} of node {}", name(), npc.npc.name, building_label(building), plot, node);
    send_citizenship_call(conn, "ClientShowBuildingContributionWindow", [=](const schema::RpcSchema& r, repl::RpcParams& par) {
        const schema::RepCommand* list = repl::rpc_command(r, "PlayerConstructionResourceCostIds");
        if (!list) return false;
        repl::ArrayValue ids;
        if (cost != 0) {
            repl::ElementState x;
            x.values[1] = repl::Value::i64(cost);
            ids.elements.push_back(std::move(x));
        }
        return repl::set_rpc_param(r, par, "NPC", repl::Value(guid)) && repl::set_rpc_param(r, par, "NodeId", repl::Value::i64(node)) &&
               repl::set_rpc_param(r, par, "PlotId", repl::Value::i64(plot)) && repl::set_rpc_param(r, par, "BuildingId", repl::Value::i64(building)) &&
               repl::set_rpc_param(r, par, "EndTime", repl::Value::i64(end)) && repl::set_rpc_param(r, par, "PlayerConstructionResourceCostIds", repl::Value(std::move(ids)));
    });
    send_building_progress(conn, node, plot);
}

void PlayerSession::on_building_fulfil(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!within_budget(building_budget_, "ServerFulfillServiceBuildingConstruction", now)) return;
    const int64_t node = nint(rpc, "NodeGuid"), plot = nint(rpc, "PlotGuid");
    if (node == 0 || plot == 0 || node != contribution_node_) {
        ++building_refusals_;
        spdlog::info("[nodes] {}: ServerFulfillServiceBuildingConstruction for plot {} of node {} is refused: no contribution window of that node is open", name(), plot, node);
        return;
    }
    if (contribute_carried(conn, node, plot, /*bag=*/true, now) == 0) {
        building_notice(conn, "You carry nothing this construction needs.");
        send_building_progress(conn, node, plot);
    }
}

namespace {

/// The plot a call names, checked against the node at its level (null: none).
const world::PlotInfo* named_plot(const std::vector<world::PlotInfo>& plots, int64_t plot) { return plot_in(plots, plot); }

}  // namespace

void PlayerSession::on_expansion_purchase(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!within_budget(building_budget_, "ServerPurchaseExpansion", now)) return;
    const int64_t node = nint(rpc, "NodeGuid"), plot = nint(rpc, "PlotGuid"), rank = nint(rpc, "ExpansionSkillRankGuid");
    bool gm = false;
    if (!may_build(conn, node, "ServerPurchaseExpansion", gm, now)) return;
    const bool free = gm && nbool(rpc, "bFree"), skip = gm && nbool(rpc, "bSkipPlayerPhase");
    const std::vector<world::PlotInfo> plots = world_->node_plots(node);
    const world::PlotInfo* p = named_plot(plots, plot);
    const int level = world_->city_nodes() ? world_->city_nodes()->level_of(node) : 0;
    nb::Result r = nb::Result::NoPlot;
    if (p) {
        world_->elections()->change_node(node, [&](world::NodeElection& e) {
            r = nb::purchase_expansion(e, plots, *p, rank, level, wall_ms(), free, skip);
            return r == nb::Result::Ok;
        });
    }
    if (r != nb::Result::Ok) {
        ++building_refusals_;
        spdlog::info("[nodes] {}: the expansion {} of plot {} of node {} is not bought: {}", name(), expansion_label(rank), plot, node, nb::result_name(r));
        building_notice(conn, std::string("The expansion cannot be bought: ") + nb::result_name(r) + ".");
        note_node_request(node_summary_wait_, node_summary_queued_, node, "a refused expansion");
        return;
    }
    ++building_actions_;
    world_->elections()->add_news(node, world::NodeNews{kNewsExpansion, wall_ms(), name(), plot, p->building, 0, world::kPlotOperational});
    spdlog::info("[nodes] {}: {} buys the expansion {} for plot {} of node {}{}{}", name(), gm ? "a game master" : "the mayor", expansion_label(rank), plot, node, free ? " (free)" : "",
                 skip ? " (the players' phase skipped)" : "");
    buildings_changed(conn, node);
}

void PlayerSession::on_expansion_cancel(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!within_budget(building_budget_, "ServerCancelExpansionPurchase", now)) return;
    const int64_t node = nint(rpc, "NodeGuid"), plot = nint(rpc, "PlotGuid");
    bool gm = false;
    if (!may_build(conn, node, "ServerCancelExpansionPurchase", gm, now)) return;
    const std::vector<world::PlotInfo> plots = world_->node_plots(node);
    const world::PlotInfo* p = named_plot(plots, plot);
    nb::Result r = nb::Result::NoPlot;
    if (p) world_->elections()->change_node(node, [&](world::NodeElection& e) { return (r = nb::cancel_expansion_purchase(e, *p)) == nb::Result::Ok; });
    spdlog::info("[nodes] {}: cancels the expansion project of plot {} of node {}: {}", name(), plot, node, nb::result_name(r));
    if (r == nb::Result::Ok) {
        ++building_actions_;
        buildings_changed(conn, node);
    } else {
        ++building_refusals_;
        building_notice(conn, std::string("Nothing was cancelled: ") + nb::result_name(r) + ".");
    }
}

void PlayerSession::on_expansion_remove(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!within_budget(building_budget_, "ServerRemoveExpansion", now)) return;
    const int64_t node = nint(rpc, "NodeGuid"), plot = nint(rpc, "PlotGuid"), rank = nint(rpc, "ExpansionSkillRankGuid");
    bool gm = false;
    if (!may_build(conn, node, "ServerRemoveExpansion", gm, now)) return;
    const std::vector<world::PlotInfo> plots = world_->node_plots(node);
    const world::PlotInfo* p = named_plot(plots, plot);
    nb::Result r = nb::Result::NoPlot;
    if (p) world_->elections()->change_node(node, [&](world::NodeElection& e) { return (r = nb::remove_expansion(e, *p, rank, wall_ms())) == nb::Result::Ok; });
    spdlog::info("[nodes] {}: the removal of the expansion {} of plot {} of node {}: {}", name(), expansion_label(rank), plot, node, nb::result_name(r));
    if (r == nb::Result::Ok) {
        ++building_actions_;
        world_->elections()->add_news(node, world::NodeNews{kNewsExpansionAdvanced, wall_ms(), name(), plot, p->building, 0, world::kPlotOperational});
        buildings_changed(conn, node);
    } else {
        ++building_refusals_;
        building_notice(conn, std::string("The expansion cannot be removed: ") + nb::result_name(r) + ".");
    }
}

void PlayerSession::on_expansion_cancel_removal(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!within_budget(building_budget_, "ServerCancelExpansionRemoval", now)) return;
    const int64_t node = nint(rpc, "NodeGuid"), plot = nint(rpc, "PlotGuid");
    bool gm = false;
    if (!may_build(conn, node, "ServerCancelExpansionRemoval", gm, now)) return;
    const std::vector<world::PlotInfo> plots = world_->node_plots(node);
    const world::PlotInfo* p = named_plot(plots, plot);
    nb::Result r = nb::Result::NoPlot;
    if (p) world_->elections()->change_node(node, [&](world::NodeElection& e) { return (r = nb::cancel_expansion_removal(e, *p)) == nb::Result::Ok; });
    spdlog::info("[nodes] {}: cancels the removal vote of plot {} of node {}: {}", name(), plot, node, nb::result_name(r));
    if (r == nb::Result::Ok) {
        ++building_actions_;
        buildings_changed(conn, node);
    } else {
        ++building_refusals_;
    }
}

void PlayerSession::on_expansion_vote(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!within_budget(building_budget_, "ServerVoteOnExpansion", now)) return;
    const int64_t plot = nint(rpc, "Plot");
    const bool yes = nbool(rpc, "bIsYesVote");
    const int64_t node = citizenship_.node_guid;
    world::Elections* el = world_ ? world_->elections() : nullptr;
    const std::vector<world::PlotInfo> plots = world_ ? world_->node_plots(node) : std::vector<world::PlotInfo>{};
    const world::PlotInfo* p = named_plot(plots, plot);
    nb::Result r = nb::Result::NoPlot;
    if (el && p) el->change_node(node, [&](world::NodeElection& e) { return (r = nb::vote(e, *p, account_id_, yes)) == nb::Result::Ok; });
    spdlog::info("[nodes] {}: votes {} on plot {} of node {} (the citizen node): {}", name(), yes ? "yes" : "no", plot, node, p ? nb::result_name(r) : "the plot is not the citizen node's");
    if (r == nb::Result::Ok) {
        ++building_actions_;
        buildings_changed(conn, node);
    } else {
        ++building_refusals_;
        building_notice(conn, std::string("The vote is not counted: ") + (p ? nb::result_name(r) : "only the node's citizens vote on its buildings") + ".");
    }
}

void PlayerSession::on_demolition(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!within_budget(building_budget_, "ServerCommenceDemolition", now)) return;
    const int64_t plot = nint(rpc, "Plot");
    const int64_t node = node_of_plot(plot);
    bool gm = false;
    if (!may_build(conn, node, "ServerCommenceDemolition", gm, now)) return;
    const std::vector<world::PlotInfo> plots = world_->node_plots(node);
    const world::PlotInfo* p = named_plot(plots, plot);
    nb::Result r = nb::Result::NoPlot;
    int64_t building = 0;
    if (p) {
        world_->elections()->change_node(node, [&](world::NodeElection& e) {
            building = nb::now_of(e.buildings, *p).building;
            return (r = nb::demolish(e, plots, *p, wall_ms())) == nb::Result::Ok;
        });
    }
    spdlog::info("[nodes] {}: the demolition of the {} of plot {} of node {}: {}", name(), building_label(building), plot, node, nb::result_name(r));
    if (r == nb::Result::Ok) {
        ++building_actions_;
        world_->elections()->add_news(node, world::NodeNews{kNewsStateAdvanced, wall_ms(), name(), plot, building, 0, world::kPlotDemolition});
        buildings_changed(conn, node);
    } else {
        ++building_refusals_;
        building_notice(conn, std::string("The building cannot be demolished: ") + nb::result_name(r) + ".");
    }
}

void PlayerSession::on_demolition_cancel(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!within_budget(building_budget_, "ServerCancelDemolition", now)) return;
    const int64_t plot = nint(rpc, "Plot");
    const int64_t node = node_of_plot(plot);
    bool gm = false;
    if (!may_build(conn, node, "ServerCancelDemolition", gm, now)) return;
    const std::vector<world::PlotInfo> plots = world_->node_plots(node);
    const world::PlotInfo* p = named_plot(plots, plot);
    nb::Result r = nb::Result::NoPlot;
    if (p) world_->elections()->change_node(node, [&](world::NodeElection& e) { return (r = nb::cancel_demolition(e, *p)) == nb::Result::Ok; });
    spdlog::info("[nodes] {}: cancels the demolition of plot {} of node {}: {}", name(), plot, node, nb::result_name(r));
    if (r == nb::Result::Ok) {
        ++building_actions_;
        buildings_changed(conn, node);
    } else {
        ++building_refusals_;
    }
}

void PlayerSession::on_demolition_vote(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!within_budget(building_budget_, "ServerVoteOnDemolition", now)) return;
    ++building_refusals_;
    spdlog::info("[nodes] {}: ServerVoteOnDemolition on plot {}: refused, the data's demolitions have no vote (DemolitionVoteDuration 0)", name(), nint(rpc, "Plot"));
    building_notice(conn, "Demolitions are not voted on.");
}

void PlayerSession::on_gm_building_rpc(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const std::string rpc_name = rpc.call.rpc ? rpc.call.rpc->name : "?";
    if (!gm_rpc_admitted(rpc_name.c_str(), std::to_string(rpc.call.param_bits) + " parameter bits", now) || !rpc.call.decoded || !world_ || !world_->elections()) return;
    world::Elections* el = world_->elections();
    ++building_gm_;
    const int64_t wall = wall_ms();
    // the node and plot a call names: by guid, or by record name
    const auto node_named = [&](const char* field) {
        std::string why;
        const int64_t n = gm_node_named(nstr(rpc, field), why);
        if (n == 0) building_notice(conn, rpc_name + ": " + why);
        return n;
    };
    const auto plot_named = [&](int64_t node, const std::string& name) -> int64_t {
        const world::NodeWire* wire = world_->node_wire();
        if (!wire || !world_->city_nodes()) return 0;
        for (const world::PlotRow& row : wire->data_plot_rows_of(node, world_->city_nodes()->level_of(node))) {
            if (lower(row.name) == lower(name) || std::to_string(row.plot) == name) return row.plot;
        }
        return 0;
    };
    if (rpc_name == "ServerGMRequestNodeSkillPointData") {
        const int64_t node = node_named("NodeRecordName");
        if (node == 0) return;
        const world::NodeElection* e = el->node(node);
        static const world::NodeBuildings kNoBuildings;
        const std::map<int64_t, int32_t> points = nb::skill_points(e ? e->buildings : kNoBuildings, world_->node_plots(node));
        send_citizenship_call(conn, "ClientGMReceiveNodeSkillPointData", [points](const schema::RpcSchema& r, repl::RpcParams& par) {
            const schema::RepCommand* list = repl::rpc_command(r, "SkillPointData");
            if (!list) return false;
            const auto elems = repl::rpcf::elements_of(r, *list);
            const uint16_t h_type = repl::rpcf::local(elems, "SkillPointData.SkillPointTypeRecordId"), h_q = repl::rpcf::local(elems, "SkillPointData.SkillPointQuantity");
            if (!h_type || !h_q) return false;
            repl::ArrayValue a;
            for (const auto& [type, q] : points) {
                repl::ElementState x;
                x.values[h_type] = repl::Value::i64(type);
                x.values[h_q] = repl::Value::u64(static_cast<uint64_t>(std::clamp(q, 0, 255)));
                a.elements.push_back(std::move(x));
            }
            return repl::set_rpc_param(r, par, "SkillPointData", repl::Value(std::move(a)));
        });
        std::string text;
        for (const auto& [type, q] : points) text += (text.empty() ? "" : ", ") + nb::point_type_name(type) + " " + std::to_string(q);
        building_notice(conn, "Skill points: " + (text.empty() ? std::string("none") : text));
        return;
    }
    if (rpc_name == "ServerGMRequestServiceBuildingPlotData") {
        const int64_t node = node_named("NodeRecordName");
        if (node == 0) return;
        const int64_t plot = plot_named(node, nstr(rpc, "PlotRecordName"));
        const std::vector<world::PlotInfo> plots = world_->node_plots(node);
        const world::PlotInfo* p = named_plot(plots, plot);
        if (!p) {
            building_notice(conn, rpc_name + ": node " + std::to_string(node) + " has no plot named " + nstr(rpc, "PlotRecordName") + " at its level");
            return;
        }
        const world::NodeElection* e = el->node(node);
        static const world::NodeBuildings kNoBuildings;
        const nb::PlotNow pn = nb::now_of(e ? e->buildings : kNoBuildings, *p);
        const world::PlotState s = pn.live ? *pn.live : world::PlotState{};
        const int64_t building = pn.building;
        const uint8_t state = pn.state;
        send_citizenship_call(conn, "ClientGMReceiveServiceBuildingPlotData", [=](const schema::RpcSchema& r, repl::RpcParams& par) {
            const schema::RepCommand* list = repl::rpc_command(r, "ServiceBuildingPlotData.ExpansionData");
            if (!list) return false;
            const auto elems = repl::rpcf::elements_of(r, *list);
            const uint16_t h_rec = repl::rpcf::local(elems, "ExpansionData.ExpansionRecordId"), h_on = repl::rpcf::local(elems, "ExpansionData.bEnabled"),
                           h_date = repl::rpcf::local(elems, "ExpansionData.ConstructionDate"), h_a = repl::rpcf::local(elems, "ExpansionData.UniqueExpansionGuid.A"),
                           h_b = repl::rpcf::local(elems, "ExpansionData.UniqueExpansionGuid.B"), h_c = repl::rpcf::local(elems, "ExpansionData.UniqueExpansionGuid.C"),
                           h_d = repl::rpcf::local(elems, "ExpansionData.UniqueExpansionGuid.D");
            if (!h_rec || !h_on || !h_date || !h_a || !h_b || !h_c || !h_d) return false;
            repl::ArrayValue a;
            for (const int64_t rank : s.ranks) {
                const nb::TreeNode* t = nb::rank_node(rank);
                repl::ElementState x;
                x.values[h_rec] = repl::Value::i64(t ? t->expansion : 0);
                x.values[h_on] = repl::Value::boolean(true);
                x.values[h_date] = repl::Value::i64(s.built_ms / 1000);
                const uint64_t g = static_cast<uint64_t>(rank);
                x.values[h_a] = repl::Value::i64(0);
                x.values[h_b] = repl::Value::i64(0);
                x.values[h_c] = repl::Value::i64(static_cast<int32_t>(g >> 32));
                x.values[h_d] = repl::Value::i64(static_cast<int32_t>(g & 0xFFFFFFFFu));
                a.elements.push_back(std::move(x));
            }
            uint64_t yes = 0;
            for (const auto& [acc, v] : s.votes) yes += v ? 1 : 0;
            const nb::TreeNode* t = nb::rank_node(s.exp_rank);
            const uint64_t ug = static_cast<uint64_t>(plot) ^ static_cast<uint64_t>(node);
            return repl::set_rpc_param(r, par, "ServiceBuildingPlotData.BuildingRecordId", repl::Value::i64(building)) &&
                   repl::set_rpc_param(r, par, "ServiceBuildingPlotData.UniqueBuildingGuid.A", repl::Value::i64(0)) &&
                   repl::set_rpc_param(r, par, "ServiceBuildingPlotData.UniqueBuildingGuid.B", repl::Value::i64(0)) &&
                   repl::set_rpc_param(r, par, "ServiceBuildingPlotData.UniqueBuildingGuid.C", repl::Value::i64(static_cast<int32_t>(ug >> 32))) &&
                   repl::set_rpc_param(r, par, "ServiceBuildingPlotData.UniqueBuildingGuid.D", repl::Value::i64(static_cast<int32_t>(ug & 0xFFFFFFFFu))) &&
                   repl::set_rpc_param(r, par, "ServiceBuildingPlotData.BuildingState", repl::Value::u64(state)) &&
                   repl::set_rpc_param(r, par, "ServiceBuildingPlotData.ProjectYesVotes", repl::Value::u64(yes)) &&
                   repl::set_rpc_param(r, par, "ServiceBuildingPlotData.ProjectTotalVotes", repl::Value::u64(s.votes.size())) &&
                   repl::set_rpc_param(r, par, "ServiceBuildingPlotData.BuildingPhaseStartRealTime", repl::Value::i64(s.phase_started_ms / 1000)) &&
                   repl::set_rpc_param(r, par, "ServiceBuildingPlotData.BuildingPhaseEndRealTime", repl::Value::i64((s.complete_ms != 0 ? s.complete_ms : s.phase_ends_ms) / 1000)) &&
                   repl::set_rpc_param(r, par, "ServiceBuildingPlotData.ExpansionPhaseStartRealTime", repl::Value::i64(s.exp_started_ms / 1000)) &&
                   repl::set_rpc_param(r, par, "ServiceBuildingPlotData.ExpansionPhaseEndRealTime", repl::Value::i64(s.exp_ends_ms / 1000)) &&
                   repl::set_rpc_param(r, par, "ServiceBuildingPlotData.ExpansionInProgressRecordId", repl::Value::i64(t ? t->expansion : 0)) &&
                   repl::set_rpc_param(r, par, "ServiceBuildingPlotData.ExpansionState", repl::Value::u64(s.exp_state)) &&
                   repl::set_rpc_param(r, par, "ServiceBuildingPlotData.ExpansionStateType", repl::Value::u64(s.exp_type)) &&
                   repl::set_rpc_param(r, par, "ServiceBuildingPlotData.ExpansionData", repl::Value(std::move(a))) &&
                   repl::set_rpc_param(r, par, "ServiceBuildingPlotData.ConstructionDate", repl::Value::i64(s.built_ms / 1000));
        });
        building_notice(conn, building_label(building) + " on " + nstr(rpc, "PlotRecordName") + ": state " + std::to_string(state) + ", " + std::to_string(s.ranks.size()) +
                                  " expansion(s), project state " + std::to_string(s.exp_state));
        return;
    }
    if (rpc_name == "ServerForceBuildingMaintainance") {
        const int64_t node = node_named("NodeName");
        if (node == 0) return;
        const double scale = std::clamp<int64_t>(nint(rpc, "ProratePercent"), 0, 1000) / 100.0;
        const std::vector<world::PlotInfo> plots = world_->node_plots(node);
        std::vector<nb::Event> events;
        el->change_node(node, [&](world::NodeElection& e) {
            events = nb::advance(e, plots, wall, /*force_upkeep=*/true, scale);
            return true;
        });
        size_t disarray = 0;
        int64_t copper = 0;
        for (const nb::Event& ev : events) {
            disarray += ev.kind == nb::Event::Disarray ? 1 : 0;
            if (ev.kind == nb::Event::Upkeep) copper += ev.copper;
        }
        spdlog::info("[gm] {}: ServerForceBuildingMaintainance: node {} pays its upkeep at {:.0f}%: {} copper, {} building(s) in disarray", name(), node, scale * 100.0, copper, disarray);
        building_notice(conn, "Upkeep: " + std::to_string(copper) + " copper taken, " + std::to_string(disarray) + " building(s) in disarray.");
        buildings_changed(conn, node);
        return;
    }
    // the calls that name the node and plot by guid
    const int64_t node = rpc_name == "ServerGMAdvanceServiceBuildingPhaseEndTime" || rpc_name == "ServerContributeToServiceBuildingConstructionCheat" ? nint(rpc, "NodeRecordGuid")
                                                                                                                                                   : nint(rpc, "NodeGuid");
    const int64_t plot = rpc_name == "ServerGMAdvanceServiceBuildingPhaseEndTime" || rpc_name == "ServerContributeToServiceBuildingConstructionCheat" ? nint(rpc, "PlotRecordGuid")
                                                                                                                                                   : nint(rpc, "PlotGuid");
    const std::vector<world::PlotInfo> plots = world_->node_plots(node);
    const world::PlotInfo* p = named_plot(plots, plot);
    if (!p) {
        building_notice(conn, rpc_name + ": node " + std::to_string(node) + " has no plot " + std::to_string(plot) + " at its level");
        return;
    }
    nb::Result r = nb::Result::NoProject;
    std::string what;
    if (rpc_name == "ServerGMAdvanceServiceBuildingPhaseEndTime") {
        const int64_t seconds = nint(rpc, "SecondsUntilPhaseEnd");
        el->change_node(node, [&](world::NodeElection& e) { return (r = nb::set_phase_end(e, *p, wall, seconds)) == nb::Result::Ok; });
        what = "the phase of plot " + std::to_string(plot) + " ends in " + std::to_string(seconds) + " s";
    } else if (rpc_name == "ServerContributeToServiceBuildingConstructionCheat") {
        const int64_t item = nint(rpc, "ItemRecordGuid");
        const int32_t q = static_cast<int32_t>(std::clamp<int64_t>(nint(rpc, "Quantity"), 0, 100000));
        nb::CostList taken;
        el->change_node(node, [&](world::NodeElection& e) {
            nb::CostList offer;
            if (item != 0) offer.emplace_back(item, q);
            else offer = nb::needs(e.buildings, *p);   // no item: all it needs
            taken = nb::contribute(e, *p, offer, wall);
            return !taken.empty();
        });
        int64_t units = 0;
        for (const auto& [i, n] : taken) units += n;
        r = units > 0 ? nb::Result::Ok : nb::Result::NothingNeeded;
        what = std::to_string(units) + " unit(s) given to plot " + std::to_string(plot);
    } else if (rpc_name == "ServerSetPlotState") {
        const int64_t building = nint(rpc, "BuildingGuid");
        const uint8_t state = static_cast<uint8_t>(std::clamp<int64_t>(nint(rpc, "State"), 0, 255));
        el->change_node(node, [&](world::NodeElection& e) { return (r = nb::set_state(e, *p, building, state, wall)) == nb::Result::Ok; });
        what = "plot " + std::to_string(plot) + " holds the " + building_label(building) + " in state " + std::to_string(state);
    }
    spdlog::info("[gm] {}: {}: {}: {}", name(), rpc_name, what, nb::result_name(r));
    building_notice(conn, rpc_name + ": " + (r == nb::Result::Ok ? what : std::string(nb::result_name(r))));
    if (r == nb::Result::Ok) buildings_changed(conn, node);
}

/// The client calls of the node's buildings (session/rpc_table.h).
void PlayerSession::register_node_buildings_rpcs(RpcTable& t) {
    t.add(RpcScope::PlayerController, "ServerPurchaseServiceBuilding", [](PlayerSession& s, const RpcContext& c) { s.on_building_purchase(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerGetBuildingConstructionProgress", [](PlayerSession& s, const RpcContext& c) { s.on_building_progress(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerFulfillServiceBuildingConstruction", [](PlayerSession& s, const RpcContext& c) { s.on_building_fulfil(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerPurchaseExpansion", [](PlayerSession& s, const RpcContext& c) { s.on_expansion_purchase(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerCancelExpansionPurchase", [](PlayerSession& s, const RpcContext& c) { s.on_expansion_cancel(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerRemoveExpansion", [](PlayerSession& s, const RpcContext& c) { s.on_expansion_remove(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerCancelExpansionRemoval", [](PlayerSession& s, const RpcContext& c) { s.on_expansion_cancel_removal(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerVoteOnExpansion", [](PlayerSession& s, const RpcContext& c) { s.on_expansion_vote(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerCommenceDemolition", [](PlayerSession& s, const RpcContext& c) { s.on_demolition(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerCancelDemolition", [](PlayerSession& s, const RpcContext& c) { s.on_demolition_cancel(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerVoteOnDemolition", [](PlayerSession& s, const RpcContext& c) { s.on_demolition_vote(c.conn, c.rpc, c.now); });
    for (const char* gm : {"ServerGMRequestServiceBuildingPlotData", "ServerGMRequestNodeSkillPointData", "ServerGMAdvanceServiceBuildingPhaseEndTime",
                           "ServerContributeToServiceBuildingConstructionCheat", "ServerForceBuildingMaintainance", "ServerSetPlotState"}) {
        t.add(RpcScope::PlayerController, gm, [](PlayerSession& s, const RpcContext& c) { s.on_gm_building_rpc(c.conn, c.rpc, c.now); });
    }
}

}}  // namespace aoc::worldd
